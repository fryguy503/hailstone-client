#include "render/zone_renderer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#include "assets/texture.h"
#include "assets/texture_overrides.h"
#include "assets/wld_detail.h"
#include "assets/zone.h"
#include "assets/zone_fires.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "game/neighbour_zones.h"
#include "game/scene_light.h"
#include "game/view_distance.h"
#include "render/cover_map.h"
#include "render/culling.h"
#include "render/light_grid.h"
#include "render/material_state.h"
#include "render/shadow_chunks.h"
#include "render/vk_util.h"
#include "platform/process.h"

namespace hailstone::render {

namespace {
// What a step of the zone's build leaves in memory, so a build that holds much more than its geometry can be read
// (debug level). The time counts from the start of this build (Prepared::started).
void mark(const char* phase, std::chrono::steady_clock::time_point start) {
    if (!log::enabled(log::Level::Debug)) return;
    if (auto rss = platform::resident_memory())
        log::debug("zone build: {}: {:.0f} MB resident (+{:.2f}s)", phase, double(*rss) / (1024 * 1024),
                   std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}
} // namespace

namespace {

// Whether a placed object is a plant the wind moves (the modern look's sway, zone.vert): it has see-through leaves or
// blades (a masked material) and its name, or one of its materials' or textures', says what it is.
bool is_plant(std::string_view actor, const assets::Mesh& mesh) {
    static constexpr std::string_view kWords[] = {
        "tree", "pine", "palm", "bush", "shrub", "plant", "fern", "grass", "leaf", "leav", "flower", "vine", "reed",
        "willow", "oak", "branch", "weed", "cactus", "hedge", "frond", "ivy", "foliage", "sapling", "cattail",
        "bamboo", "birch", "maple", "cedar", "spruce", "kelp", "seaweed", "bramble", "thorn"};
    auto lower = [](std::string_view s) {
        std::string out(s);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    auto named = [&](std::string_view s) {
        std::string l = lower(s);
        for (std::string_view w : kWords)
            if (l.find(w) != std::string::npos) return true;
        return false;
    };
    bool masked = false, match = named(actor);
    for (const auto& m : mesh.materials) {
        masked |= m.blend == assets::BlendMode::Masked;
        if (!match) match = named(m.name) || std::any_of(m.frames.begin(), m.frames.end(), named);
    }
    return masked && match;
}

struct GpuVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    std::uint32_t color;
    glm::vec2 uv2;
    std::uint32_t color2;
    glm::vec3 point_light; // sum of the saturated point-light terms (MPL path)
    glm::vec4 sway;        // a plant's: its foot (xy), how far the vertex bends (z) and flutters (w); zone.vert
};

// Where an instanced replacement model stands (zone.vert's per-instance inputs, lighting.glsl instance_point): its
// placement's transform, model to world, as the rows of a 3 x 4 matrix; the normal matrix's rows, the first's w how
// much the sway's distances scale; and what its vertex colours are multiplied by (the placement's baked light). The
// buffer's first is the identity, for the zone's own geometry.
struct GpuInstance {
    glm::vec4 row[3]{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    glm::vec4 normal_row[3]{{1, 0, 0, 1}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    glm::vec4 tint{1.0f};
};

GpuInstance make_instance(const glm::mat4& m, const glm::vec4& tint) {
    GpuInstance g;
    const glm::mat3 n = glm::transpose(glm::inverse(glm::mat3(m)));
    for (int r = 0; r < 3; ++r) {
        g.row[r] = glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]);
        g.normal_row[r] = glm::vec4(n[0][r], n[1][r], n[2][r], 0.0f);
    }
    // sway_plant measures a plant by its height: scaled with its up axis.
    g.normal_row[0].w = glm::length(glm::vec3(m[2]));
    g.tint = tint;
    return g;
}

// A placed plant's sway (zone.vert, lighting.glsl wind_offset): the higher a vertex stands above the plant's foot, the
// further it bends, as the square of its height (a stiff trunk, supple tips), up to 4 % of the plant's height at full
// wind; the vertices of its leaves (the masked parts) also flutter.
void sway_plant(const assets::Mesh& mesh, const glm::mat4& transform, std::span<GpuVertex> out) {
    if (out.empty()) return;
    float low = out[0].position.z, high = low;
    for (const auto& v : out) {
        low = std::min(low, v.position.z);
        high = std::max(high, v.position.z);
    }
    float height = high - low;
    if (!(height > 0.5f)) return;
    std::vector<bool> leaf(out.size(), false);
    for (const auto& part : mesh.parts) {
        if (part.material >= mesh.materials.size() || mesh.materials[part.material].blend != assets::BlendMode::Masked)
            continue;
        for (std::uint32_t i = 0; i < part.index_count; ++i) {
            std::uint32_t k = mesh.indices[part.first_index + i];
            if (k < leaf.size()) leaf[k] = true;
        }
    }
    glm::vec2 foot(transform[3]);
    float bend = std::clamp(0.04f * height, 0.08f, 3.0f);
    float flutter = std::clamp(0.012f * height, 0.04f, 0.35f);
    for (std::size_t i = 0; i < out.size(); ++i) {
        float h = (out[i].position.z - low) / height;
        out[i].sway = glm::vec4(foot, bend * h * h, leaf[i] ? flutter * std::min(h * 2.0f, 1.0f) : 0.0f);
    }
}

// How far a vertex's sway (sway_plant) can move it, at most, for the culling boxes: lighting.glsl wind_offset leans it
// by wind.xy x wind.z x (0.35 + wind.w x gust + rock) x sway.z, and wind_at keeps wind.z <= 1 and wind.w <= 1.4, gust
// <= 1 and the rocking <= 0.35, so <= 2.1 x sway.z; its leaves flutter by <= 0.8 x 1.5 x sway.w. Rounded up.
float sway_reach(const glm::vec4& sway) { return 2.5f * sway.z + 1.5f * sway.w; }

std::uint32_t lerp_rgba(std::uint32_t a, std::uint32_t b, float t) {
    std::uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float x = static_cast<float>((a >> shift) & 0xffu), y = static_cast<float>((b >> shift) & 0xffu);
        out |= static_cast<std::uint32_t>(std::lround(std::clamp(x + (y - x) * t, 0.0f, 255.0f))) << shift;
    }
    return out;
}

// A neighbour zone's depth bias (ZoneRenderOptions::behind): steps of the depth's precision, and times its slope.
constexpr float kBehindDepthBias = 16.0f;
constexpr float kBehindSlopeBias = 2.0f;

// A vertex part way along an edge, for the neighbour zones' cut (game::clip_polygon).
GpuVertex lerp_vertex(const GpuVertex& a, const GpuVertex& b, float t) {
    GpuVertex v;
    v.position = glm::mix(a.position, b.position, t);
    glm::vec3 n = glm::mix(a.normal, b.normal, t);
    v.normal = glm::dot(n, n) > 0.0f ? glm::normalize(n) : a.normal;
    v.uv = glm::mix(a.uv, b.uv, t);
    v.color = lerp_rgba(a.color, b.color, t);
    v.uv2 = glm::mix(a.uv2, b.uv2, t);
    v.color2 = lerp_rgba(a.color2, b.color2, t);
    v.point_light = glm::mix(a.point_light, b.point_light, t);
    v.sway = glm::mix(a.sway, b.sway, t);
    return v;
}

// Matches the push_constant block in zone.vert / zone.frag.
struct PushConstants {
    glm::uvec4 tex;    // texture slots 0..3
    glm::uvec4 tex2;   // slot 4, mode, flags
    glm::vec4 params;  // alpha_ref, opacity, coverage scale, 0
    glm::vec4 slide;
    glm::vec4 extra;
    glm::vec4 extra2;
    glm::vec4 extra3;
    glm::vec4 extra4; // water: the reflection colour (rgb) and amount
};
constexpr std::uint32_t kUnlit = 1; // texture only (full-bright view, or COLOROP SELECTARG1)
constexpr std::uint32_t kNoFog = 2;
constexpr std::uint32_t kMultiPass = 4; // add the point lights
constexpr std::uint32_t kWaterSurface = 8; // a water surface (the modern lighting's water)
constexpr std::uint32_t kLavaSurface = 16;  // a lava surface
constexpr std::uint32_t kWaterCopies = 32;  // VkContext::begin_translucent's copies are readable
constexpr std::uint32_t kNormalMap = 64;    // modern: slot 1 is a tangent-space normal map
constexpr std::uint32_t kSpecular = 128;    // modern: a sun highlight (extra.x shininess, extra.y gloss)
constexpr std::uint32_t kReflective = 256;  // modern: the sky reflected (extra.z strength)
constexpr std::uint32_t kMolten = 512;      // modern: RegionLava drawn as lava (its texture's name says so)
constexpr std::uint32_t kEnergy = 1024;     // modern: RegionLavaH drawn as a glowing field
constexpr std::uint32_t kEnvMap = 2048;     // modern: slot 2 is the water's own environment (an octahedral map)
constexpr std::uint32_t kLodFading = 4096;  // a level of detail: extra3 its band (coverage.glsl lod_fade_samples)
// The depth pre-pass's flags: none of the shading's (bind_draw adds kLodFading, which decides discards, to both).
constexpr std::uint32_t kDepthOnlyFlags = 0;
constexpr VkColorComponentFlags kAllChannels =
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
static_assert(sizeof(PushConstants) <= 128);

// zone.frag's effects.
enum Mode : std::uint32_t {
    kClassic = 0, kTerrainTile = 1, kWaterFall = 2, kLava = 3, kRegionTerrain = 4, kWater = 5, kWldDetail = 6
};

Mode mode_of(assets::EqgShader s) {
    using assets::EqgShader;
    switch (s) {
    case EqgShader::TerrainTile: return kTerrainTile;
    case EqgShader::WaterFall: return kWaterFall;
    case EqgShader::Lava:
    case EqgShader::Lava2: return kLava; // RegionLava2's formula is not settled
    case EqgShader::Terrain: return kRegionTerrain;
    case EqgShader::Water:
    case EqgShader::LavaH: return kWater; // RegionLavaH's formula is not settled
    case EqgShader::WldDetail: return kWldDetail;
    default: return kClassic;
    }
}

// The effect parameters as a grouping key.
std::string effect_key(const assets::EffectParams& e) {
    std::string k;
    for (const auto& t : e.textures) k += t + "|";
    for (float f : {e.slide.x, e.slide.y, e.slide.z, e.slide.w, e.water_color1.r, e.water_color1.g, e.water_color1.b,
                    e.water_color2.r, e.water_color2.g, e.water_color2.b, e.fresnel_bias, e.fresnel_power,
                    e.reflection_amount, e.uv_scale, e.coverage_scale, e.detail_scale.x, e.detail_scale.y,
                    e.detail_scale.z, e.detail_scale.w})
        k += std::to_string(f) + ",";
    k += e.overlay ? "o" : "";
    k += "|" + e.normal_map + "|" + std::to_string(e.shininess) + "," + std::to_string(e.gloss) + "," +
         std::to_string(e.env_strength) + (e.specular ? "s" : "") + (e.reflective ? "r" : "");
    return k;
}

std::vector<std::uint32_t> read_spirv(const std::filesystem::path& path) { return read_spirv_file(path); }

VkShaderModule create_shader(VkDevice device, const std::filesystem::path& path) {
    auto code = read_spirv(path);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size() * 4;
    info.pCode = code.data();
    VkShaderModule module;
    vk_check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

// Draw grouping key: one draw per distinct material look.
struct MaterialKey {
    std::vector<std::string> frames;
    std::uint32_t delay = 0;
    assets::BlendMode blend = assets::BlendMode::Opaque;
    bool two_sided = false;
    bool front_ccw = false;
    assets::EqgShader eqg = assets::EqgShader::None;
    bool multipass = false;
    std::string effect;
    std::uint8_t liquid = 0; // ZoneRenderer::Liquid
    bool flame = false;      // a placed object's flame (assets::flame_material_hue)
    LodBand lod; // assets::MeshPart's level-of-detail band, its place among the mesh's
    std::uint32_t instanced = 0; // an instanced model's number + 1 (ZoneRenderer::Draw::instanced)
    bool distant = false;        // the land round the zone (render/horizon.h)
    bool operator<(const MaterialKey& o) const {
        return std::tie(blend, eqg, two_sided, front_ccw, frames, delay, multipass, effect, liquid, flame, lod,
                        instanced, distant) < std::tie(o.blend, o.eqg, o.two_sided, o.front_ccw, o.frames, o.delay,
                                                       o.multipass, o.effect, o.liquid, o.flame, o.lod, o.instanced,
                                                       o.distant);
    }
};

// The MPL light passes take the point light's colour x 0.5. The notes say the
// path uses every constant at x 0.5; whether a x 2 follows (the full-screen
// quad EnableMultiPassLighting creates) is not settled, so the base pass keeps
// the single-pass constants and only the light colour is halved.
constexpr float kMplLightScale = 0.5f;

// Whether a zone mesh's material runs the multi-pass point-light passes: its
// render-material descriptor has the three MPL effect ids
// (CRender::AddTerrainTileToBucket @0x10090740). Heightmap tiles and the EQG
// CB* classes have them; lava, water, waterfall, the old-style (WLD
// translucent and detail) regions and every actor do not. Classic opaque
// WLD regions are taken as RegionC1's; whether that descriptor carries an MPL
// triple is not settled (docs/unknowns.md).
bool has_mpl_triple(const assets::Mesh& mesh, const assets::Material& m) {
    using assets::EqgShader;
    if (m.eqg_shader == EqgShader::TerrainTile) return true;
    if (m.eqg_shader != EqgShader::None) return false;
    if (m.blend != assets::BlendMode::Opaque) return false;
    if (mesh.front_ccw) return m.multipass_lighting; // EQG .ter: the loader marks the CB* classes
    return true;                                      // classic WLD opaque region
}

} // namespace

struct ZoneRenderer::Prepared {
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now(); // for the build's log lines
    std::vector<GpuVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Draw> draws;
    std::vector<GpuInstance> instances; // the identity, then the instanced models' placements
    std::vector<Chunk> chunks; // the draws', then solid's
    std::size_t draw_chunks = 0;
    std::size_t triangles = 0; // as drawn, each instance's counted
    Draw solid; // ZoneRenderer::solid_
    struct Image {
        assets::Image image;
        TextureUse use = TextureUse::Data;
    };
    std::vector<Image> images; // by texture slot; slot 0 plain white
    std::size_t missing_textures = 0;
    std::vector<std::byte> lights, grid_cells, grid_indices; // the light grid's three buffers
    bool behind = false;                                      // ZoneRenderOptions::behind
    HorizonLand horizon;                                      // the land round the zone (not for a neighbour)
    std::vector<std::uint32_t> distant_indices;               // ZoneRenderer::distant_indices_
    std::vector<glm::vec2> distant_centres;
    std::vector<std::int64_t> footprint;                      // a neighbour's (ZoneRenderer::footprint_)
};

void ZoneRenderer::PreparedDeleter::operator()(Prepared* p) const { delete p; }

ZoneRenderer::PreparedCounts ZoneRenderer::prepared_counts(const Prepared& p) {
    return {p.vertices.size(), p.indices.size(), p.instances.empty() ? 0 : p.instances.size() - 1, p.triangles,
            p.draw_chunks};
}

std::size_t ZoneRenderer::prepared_bytes(const Prepared& p) {
    std::size_t n = p.vertices.size() * sizeof(GpuVertex) + p.indices.size() * sizeof(std::uint32_t) +
                    p.instances.size() * sizeof(GpuInstance);
    for (const auto& i : p.images)
        for (const auto& level : i.image.mips) n += level.size();
    return n;
}

void ZoneRenderer::upload(Prepared& p) {
    UploadBatch upload(vk_);
    textures_.reserve(p.images.size());
    // What the zone's textures and geometry cost, so a load that holds much more than this can be read (the
    // textures are held twice on the way up: decoded on the CPU, then in the staging buffers).
    std::size_t texture_bytes = 0;
    // A big zone has thousands of textures and a vertex buffer as big as its geometry: their staging is released
    // as the GPU reads it, so the load does not hold the zone twice over.
    constexpr std::size_t kStagingBudget = 32u * 1024 * 1024;
    for (auto& i : p.images) {
        for (const auto& level : i.image.mips) texture_bytes += level.size();
        textures_.push_back(upload.immutable_texture(i.image, i.use));
        i.image = {}; // freed as it goes
        if (upload.staging_bytes() >= kStagingBudget) {
            upload.submit();
            vk_.release_finished_uploads(kStagingBudget);
        }
    }
    mark("textures staged", p.started);
    if (vk_.gpu_shared_cache()) {
        const auto shared = std::count_if(textures_.begin(), textures_.end(), [](const Texture& texture) {
            return static_cast<bool>(texture.shared);
        });
        log::info("GPU immutable zone textures: total={} shared={} private={}", textures_.size(), shared,
                  textures_.size() - static_cast<std::size_t>(shared));
    }
    light_grid_buffers_[0] = upload.buffer(p.lights, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    light_grid_buffers_[1] = upload.buffer(p.grid_cells, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    light_grid_buffers_[2] = upload.buffer(p.grid_indices, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    vertex_buffer_ = upload.buffer_pieces(std::as_bytes(std::span(p.vertices)), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                          kStagingBudget);
    index_buffer_ = upload.buffer_pieces(std::as_bytes(std::span(p.indices)), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                         kStagingBudget);
    if (p.instances.empty()) p.instances.emplace_back();
    instance_buffer_ = upload.buffer(std::as_bytes(std::span(p.instances)), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    upload.submit();
    mark("submitted", p.started);
    if (!distant_indices_.empty())
        for (std::size_t i = 0; i < distant_buffers_.size(); ++i) {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = distant_indices_.size() * sizeof(std::uint32_t);
            bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo out{};
            vk_check(vmaCreateBuffer(vk_.allocator(), &bi, &ai, &distant_buffers_[i].buffer,
                                     &distant_buffers_[i].allocation, &out),
                     "create the land's index buffer");
            distant_buffers_[i].size = bi.size;
            distant_mapped_[i] = out.pMappedData;
            distant_dirty_ |= 1u << i;
        }
    log::info("zone renderer prepared: {} textures ({} MB), {} vertices ({} MB), {} indices ({} MB), {} instances",
              textures_.size(), texture_bytes / (1024 * 1024), p.vertices.size(),
              p.vertices.size() * sizeof(GpuVertex) / (1024 * 1024), p.indices.size(),
              p.indices.size() * sizeof(std::uint32_t) / (1024 * 1024), p.instances.size() - 1);
    draws_ = std::move(p.draws);
    chunks_ = std::move(p.chunks);
    instance_origins_.clear();
    for (const GpuInstance& i : p.instances) instance_origins_.emplace_back(i.row[0].w, i.row[1].w, i.row[2].w);
    solid_ = p.solid;
    stats_.triangles = p.triangles;
    stats_.draws = draws_.size();
    stats_.textures = textures_.size();
    stats_.chunks = p.draw_chunks;
    stats_.missing_textures = p.missing_textures;
}

ZoneRenderer::ZoneRenderer(VkContext& vk, const assets::Zone& zone, const std::filesystem::path& shader_dir,
                           const ZoneRenderOptions& options)
    : ZoneRenderer(vk, prepare(zone, options, vk.caps().max_sampled_images), shader_dir) {}

ZoneRenderer::ZoneRenderer(VkContext& vk, PreparedPtr prepared, const std::filesystem::path& shader_dir) : vk_(vk) {
    static std::atomic<std::uint64_t> made{0};
    id_ = ++made;
    behind_ = prepared->behind;
    horizon_ = prepared->horizon;
    footprint_ = std::move(prepared->footprint);
    distant_indices_ = std::move(prepared->distant_indices);
    distant_centres_ = std::move(prepared->distant_centres);
    distant_shown_ = distant_indices_;
    // A neighbour zone (behind_) keeps its single pass: its ties go by the draw order (GREATER, first drawn wins),
    // which an equal test over the pre-pass's written depth would turn around.
    depth_prepass_ = vk.scene_options().depth_prepass && !behind_;
    upload(*prepared);
    prepared.reset();
    create_descriptors();
    // Shared textures moved into this client's own images when sharing stopped: the set takes the new views.
    shared_listener_ = vk_.on_shared_textures_moved([this] {
        bool changed = false;
        for (auto& t : textures_)
            if (refresh_shared_texture(t)) changed = true;
        if (changed) write_texture_set();
    });
    create_pipelines(shader_dir);
    create_shadow_pipeline(shader_dir);
    create_underwater_pipeline(shader_dir);
}

ZoneRenderer::~ZoneRenderer() {
    vk_.remove_shared_textures_listener(shared_listener_);
    // Retired rather than destroyed after a wait for the device: a neighbour zone is let go in the middle of a frame
    // (app/neighbour_zones.h), and vkDeviceWaitIdle there was a hitch. Its objects go once the frames that may still
    // draw it, and its uploads, are done (VkContext::retire).
    struct Objects {
        std::vector<VkPipeline> pipelines;
        VkPipelineLayout layout;
        VkDescriptorPool pool;
        std::vector<VkDescriptorSetLayout> set_layouts;
        std::vector<Buffer> buffers;
        VkSampler sampler;
        std::vector<Texture> textures;
    } o{{shadow_pipeline_, point_shadow_pipeline_, underwater_pipeline_, depth_pipeline_, opaque_equal_pipeline_},
        layout_,
        pool_,
        {set_layout_, scene_layout_},
        {},
        sampler_,
        std::move(textures_)};
    for (VkPipeline p : pipelines_) o.pipelines.push_back(p);
    for (const auto& b : scene_buffers_) o.buffers.push_back(b);
    for (const auto& b : light_grid_buffers_) o.buffers.push_back(b);
    o.buffers.push_back(vertex_buffer_);
    o.buffers.push_back(instance_buffer_);
    o.buffers.push_back(index_buffer_);
    for (const auto& b : distant_buffers_) o.buffers.push_back(b);
    vk_.retire([d = vk_.device(), allocator = vk_.allocator(), o]() mutable {
        for (VkPipeline p : o.pipelines)
            if (p) vkDestroyPipeline(d, p, nullptr);
        if (o.layout) vkDestroyPipelineLayout(d, o.layout, nullptr);
        if (o.pool) vkDestroyDescriptorPool(d, o.pool, nullptr);
        for (VkDescriptorSetLayout l : o.set_layouts)
            if (l) vkDestroyDescriptorSetLayout(d, l, nullptr);
        for (Buffer& b : o.buffers) b.destroy(allocator);
        if (o.sampler) vkDestroySampler(d, o.sampler, nullptr);
        for (Texture& t : o.textures) t.destroy(d, allocator);
    });
}

std::vector<std::vector<std::uint8_t>> classic_liquid_triangles(const assets::Zone& zone) {
    std::vector<std::vector<std::uint8_t>> out(zone.terrain.size());
    if (zone.regions.empty()) return out;
    // The flags of each region, by number.
    std::vector<std::uint32_t> region_flags(zone.regions.region_count() + 1, 0u);
    for (std::uint32_t r = 1; r < region_flags.size(); ++r) region_flags[r] = zone.regions.info(r).flags;
    auto region_type = [&](const glm::vec3& p) {
        std::uint32_t r = zone.regions.region_at(p);
        return r < region_flags.size() ? static_cast<assets::RegionType>(region_flags[r] & 0xff)
                                       : assets::RegionType::Normal;
    };
    auto liquid_kind = [](assets::RegionType t) -> std::uint8_t {
        using T = assets::RegionType;
        if (t == T::Lava) return kLiquidLava;
        if (t == T::Water || t == T::Slime || t == T::FreezingWater || t == T::Water2 || t == T::Water3)
            return kLiquidWater;
        return 0;
    };
    // The plain materials, on passable triangles only: every classic water and lava surface is passable (WLD 0x10),
    // while ground lying just under a water region's top (a beach's sand at the waterline, a dock, the rock round a
    // lava pool) is solid and stays ground.
    auto classed = [&](const assets::Mesh& mesh, const assets::MeshPart& part) {
        if (part.solid || part.material >= mesh.materials.size()) return false;
        const auto& m = mesh.materials[part.material];
        return m.eqg_shader == assets::EqgShader::None && m.blend != assets::BlendMode::Masked &&
               m.blend != assets::BlendMode::Additive;
    };
    auto corners = [](const assets::Mesh& mesh, std::uint32_t first, glm::vec3 (&p)[3]) {
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = mesh.indices[first + static_cast<std::uint32_t>(k)];
            if (v >= mesh.vertices.size()) return false;
            p[k] = mesh.vertices[v].position;
            // Heightmap vertices carry z as trunc(height * 8) in a short (as prepare draws them).
            if (mesh.heightmap_terrain) p[k].z = std::trunc(p[k].z * 8.0f) / 8.0f;
        }
        return true;
    };
    auto level = [](const glm::vec3 (&p)[3]) {
        const glm::vec3 n = glm::cross(p[1] - p[0], p[2] - p[0]);
        const float len = glm::length(n);
        return len > 1e-6f && std::abs(n.z) >= 0.8f * len;
    };
    // First by the regions: a level triangle with a liquid region just under it and not just over it.
    std::map<std::pair<std::string, long>, std::uint8_t> levels; // (material, height x 4) -> the liquid
    auto height = [](float z) { return std::lround(static_cast<double>(z) * 4.0); };
    for (std::size_t mi = 0; mi < zone.terrain.size(); ++mi) {
        const auto& mesh = zone.terrain[mi];
        out[mi].assign(mesh.indices.size() / 3, 0);
        for (const auto& part : mesh.parts) {
            if (!classed(mesh, part)) continue;
            for (std::uint32_t i = 0; i + 2 < part.index_count; i += 3) {
                glm::vec3 p[3];
                if (!corners(mesh, part.first_index + i, p) || !level(p)) continue;
                const glm::vec3 centre = (p[0] + p[1] + p[2]) / 3.0f;
                const std::uint8_t under = liquid_kind(region_type(centre - glm::vec3(0.0f, 0.0f, 1.5f)));
                if (!under || liquid_kind(region_type(centre + glm::vec3(0.0f, 0.0f, 1.5f))) == under) continue;
                out[mi][(part.first_index + i) / 3] = under;
                levels[{mesh.materials[part.material].name, height(centre.z)}] = under;
            }
        }
    }
    // Then past the regions: a liquid's surface runs on where a dry region cuts into it (a zone line's region over
    // the sea, as North Ro's "DR" line to Oasis along its shore, or the strip by a zone's edge). A level triangle the
    // regions leave out is the liquid's when one of the same material at the same height (within a quarter unit) is
    // and the region over it is not that liquid; else it would be drawn as plain opaque ground in the sea.
    for (std::size_t mi = 0; mi < zone.terrain.size() && !levels.empty(); ++mi) {
        const auto& mesh = zone.terrain[mi];
        for (const auto& part : mesh.parts) {
            if (!classed(mesh, part)) continue;
            for (std::uint32_t i = 0; i + 2 < part.index_count; i += 3) {
                auto& kind = out[mi][(part.first_index + i) / 3];
                glm::vec3 p[3];
                if (kind || !corners(mesh, part.first_index + i, p) || !level(p)) continue;
                const glm::vec3 centre = (p[0] + p[1] + p[2]) / 3.0f;
                auto it = levels.find({mesh.materials[part.material].name, height(centre.z)});
                if (it == levels.end()) continue;
                if (liquid_kind(region_type(centre + glm::vec3(0.0f, 0.0f, 1.5f))) == it->second) continue;
                kind = it->second;
            }
        }
    }
    return out;
}

ZoneRenderer::PreparedPtr ZoneRenderer::prepare(const assets::Zone& zone, const ZoneRenderOptions& options,
                                                std::uint32_t max_images) {
    PreparedPtr prepared(new Prepared);
    Prepared& prep = *prepared;
    mark("prepare start", prep.started);
    prep.behind = options.behind;
    std::vector<GpuVertex>& vertices = prep.vertices;
    std::map<MaterialKey, std::vector<std::uint32_t>> by_material;
    std::map<MaterialKey, assets::Material> looks;

    // The zone's point lights in a coarse grid, for the MPL passes.
    constexpr float kCell = 128.0f;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> light_grid;
    auto cell_key = [](std::int64_t x, std::int64_t y) { return (x << 32) ^ (y & 0xffffffffLL); };
    if (options.multipass_lighting)
        for (std::uint32_t i = 0; i < zone.lights.size(); ++i) {
            const auto& l = zone.lights[i];
            if (!(l.radius > 0.0f)) continue;
            auto x0 = static_cast<std::int64_t>(std::floor((l.position.x - l.radius) / kCell));
            auto x1 = static_cast<std::int64_t>(std::floor((l.position.x + l.radius) / kCell));
            auto y0 = static_cast<std::int64_t>(std::floor((l.position.y - l.radius) / kCell));
            auto y1 = static_cast<std::int64_t>(std::floor((l.position.y + l.radius) / kCell));
            for (auto x = x0; x <= x1; ++x)
                for (auto y = y0; y <= y1; ++y) light_grid[cell_key(x, y)].push_back(i);
        }
    std::size_t lit_vertices = 0;

    // The classic regions' liquid surfaces, by terrain mesh and triangle.
    static_assert(static_cast<std::uint8_t>(Liquid::Water) == kLiquidWater &&
                  static_cast<std::uint8_t>(Liquid::Lava) == kLiquidLava);
    const auto terrain_liquid = classic_liquid_triangles(zone);
    std::size_t liquid_triangles = 0;

    // A classic region's detail passes (assets/wld_detail.h), as the region builder 0x10019780 adds them: each
    // batch's triangles once more, over the base, with vertices of their own: the UVs x the layer's scale, the pass's
    // vertex alpha in color2's alpha and the batch's box in uv2 (min x, min y) and point_light (max x, max y), for
    // zone.vert / zone.frag mode 6 (the WLD colour stays for the light). A single detail's alpha is the vertex
    // colour's; a palette layer's 0xff where the map gives the vertex the layer and 0 where it does not.
    std::map<std::string, std::optional<assets::wld::PaletteMap>> palette_maps;
    std::size_t detail_triangles = 0;
    auto append_details = [&](const assets::Mesh& mesh, std::uint32_t base) {
        for (std::uint32_t mi = 0; mi < mesh.materials.size(); ++mi) {
            const auto& m = mesh.materials[mi];
            if (m.detail_layers.empty() || m.blend == assets::BlendMode::Invisible || m.frames.empty() ||
                !material_state(m).draw)
                continue;
            const assets::wld::PaletteMap* map = nullptr;
            if (!m.detail_palette.empty()) {
                auto [it, fresh] = palette_maps.try_emplace(m.detail_palette);
                if (fresh) {
                    if (auto data = zone.read_file(m.detail_palette)) it->second = assets::wld::decode_palette_map(*data);
                    if (!it->second) log::warn("zone detail palette {}: not found or not an 8-bit BMP", m.detail_palette);
                }
                if (!it->second) continue;
                map = &*it->second;
            }
            for (const auto& batch : assets::wld::detail_batches(mesh, mi, map)) {
                const auto& l = m.detail_layers[batch.layer];
                if (l.texture.empty()) continue;
                std::unordered_map<std::uint32_t, std::uint32_t> copy; // mesh vertex -> its vertex in this batch
                std::vector<std::uint32_t> tris;
                tris.reserve(batch.indices.size());
                for (std::uint32_t v : batch.indices) {
                    auto [it, fresh] = copy.try_emplace(v, static_cast<std::uint32_t>(vertices.size()));
                    if (fresh) {
                        GpuVertex g = vertices[base + v];
                        g.uv = mesh.vertices[v].uv * l.uv_scale;
                        std::uint32_t alpha =
                            map ? (assets::wld::palette_entry(*map, mesh.vertices[v].uv, m.detail_layers.size()) ==
                                           static_cast<int>(batch.layer)
                                       ? 0xffu
                                       : 0u)
                                : g.color >> 24;
                        g.color2 = (g.color2 & 0x00ffffffu) | alpha << 24;
                        g.uv2 = glm::vec2(batch.box.x, batch.box.z);
                        g.point_light = glm::vec3(batch.box.y, batch.box.w, 0.0f); // no MPL triple: no point lights
                        vertices.push_back(g);
                    }
                    tris.push_back(it->second);
                }
                assets::Material look;
                look.name = m.name;
                look.frames = {l.texture};
                look.two_sided = m.two_sided;
                look.eqg_shader = assets::EqgShader::WldDetail;
                MaterialKey key{look.frames, 0, look.blend, look.two_sided, mesh.front_ccw, look.eqg_shader, false, {}};
                looks.try_emplace(key, look);
                auto& out = by_material[key];
                out.insert(out.end(), tris.begin(), tris.end());
                detail_triangles += tris.size() / 3;
            }
        }
    };

    // The zone's terrain for the land round it (render/horizon.h): its own opaque triangles and its liquid surfaces.
    // Not for a neighbour zone, which draws none.
    const bool measure_horizon = options.offset == glm::vec3(0.0f) && options.keep.empty() && !options.behind;
    std::vector<HorizonTriangle> horizon_triangles;
    // Per material: its level ground's and its slopes' area, its texture's scale (UV area over area), which way up
    // its level triangles face, and how many liquid triangles; the level ground's vertex colours.
    struct HorizonUse {
        float level = 0.0f, slope = 0.0f, uv_area = 0.0f, area = 0.0f, up = 0.0f;
        std::size_t liquid = 0;
        std::vector<std::uint32_t> colors;
    };
    std::map<MaterialKey, HorizonUse> horizon_uses;
    std::size_t plants = 0;
    // The replacement models (assets/model_overrides.h), drawn instanced: each mesh once, in its own space, with a
    // transform for each of its placements (GpuInstance). Kept apart from `vertices` and `by_material` until the
    // neighbour zone's cut below, which works on world positions.
    struct InstancedModel {
        std::vector<GpuInstance> instances;
        std::vector<glm::mat4> transforms; // the instances' own, for their boxes
        std::uint32_t first_instance = 0;  // in Prepared::instances
    };
    std::vector<InstancedModel> models;
    std::unordered_map<const assets::Mesh*, std::uint32_t> model_numbers; // mesh -> its number in models + 1
    std::vector<GpuVertex> model_vertices;
    std::map<MaterialKey, std::vector<std::uint32_t>> model_by_material;
    // `instanced`: an instanced model's number + 1, its mesh appended to model_vertices untransformed.
    auto append = [&](const assets::Mesh& mesh, const glm::mat4& transform, const std::vector<std::uint32_t>& colors,
                      bool region, bool plant, const std::vector<std::uint8_t>* liquid = nullptr,
                      std::uint32_t instanced = 0) {
        std::vector<GpuVertex>& out = instanced ? model_vertices : vertices;
        auto& out_by_material = instanced ? model_by_material : by_material;
        auto base = static_cast<std::uint32_t>(out.size());
        glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(transform)));
        // Point lights reach only zone regions (not placed actors) whose
        // material has an MPL triple.
        bool any_mpl = false;
        if (region && options.multipass_lighting)
            for (const auto& m : mesh.materials) any_mpl |= has_mpl_triple(mesh, m);
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const auto& v = mesh.vertices[i];
            GpuVertex g;
            g.position = glm::vec3(transform * glm::vec4(v.position, 1.0f));
            // Heightmap vertices carry z as trunc(height * 8) in a short.
            if (mesh.heightmap_terrain) g.position.z = std::trunc(g.position.z * 8.0f) / 8.0f;
            glm::vec3 n = normal_matrix * v.normal;
            g.normal = glm::dot(n, n) > 0.0f ? glm::normalize(n) : glm::vec3(0, 0, 1);
            g.uv = v.uv;
            g.color = i < colors.size() ? colors[i] : v.color;
            g.uv2 = v.uv2;
            g.color2 = v.color2;
            g.point_light = glm::vec3(0.0f);
            if (any_mpl) {
                auto it = light_grid.find(cell_key(static_cast<std::int64_t>(std::floor(g.position.x / kCell)),
                                                   static_cast<std::int64_t>(std::floor(g.position.y / kCell))));
                if (it != light_grid.end()) {
                    for (std::uint32_t li : it->second) {
                        const auto& l = zone.lights[li];
                        // One additive pass per light with its colour x 0.5
                        // (0x10099fc0); each pass saturates on its own.
                        g.point_light += glm::min(game::point_light_term(l.position, l.color * kMplLightScale, l.radius,
                                                                         g.position, g.normal),
                                                  glm::vec3(1.0f));
                    }
                    if (g.point_light != glm::vec3(0.0f)) ++lit_vertices;
                }
            }
            g.sway = glm::vec4(0.0f);
            out.push_back(g);
        }
        if (plant) sway_plant(mesh, transform, std::span(out).subspan(base));
        plants += plant;
        // The mesh's levels of detail by their near ends, nearest first: the shadow passes pick a level by its place.
        std::vector<float> bands;
        for (const auto& part : mesh.parts)
            if (part.lod_far > 0.0f) bands.push_back(part.lod_near);
        std::sort(bands.begin(), bands.end());
        bands.erase(std::unique(bands.begin(), bands.end()), bands.end());
        for (const auto& part : mesh.parts) {
            if (part.material >= mesh.materials.size()) continue;
            const auto& m = mesh.materials[part.material];
            if (m.blend == assets::BlendMode::Invisible || m.frames.empty()) continue;
            if (!material_state(m).draw) continue; // no render effect: the original draws nothing
            MaterialKey key{m.frames, m.frame_delay_ms, m.blend, m.two_sided, mesh.front_ccw, m.eqg_shader,
                            region && options.multipass_lighting && has_mpl_triple(mesh, m), effect_key(m.effect)};
            if (m.eqg_shader == assets::EqgShader::Water) key.liquid = static_cast<std::uint8_t>(Liquid::Water);
            key.flame = !region && assets::flame_material_hue(m) != assets::FlameHue::None;
            key.instanced = instanced;
            if (part.lod_far > 0.0f) {
                key.lod.near = part.lod_near;
                key.lod.far = part.lod_far;
                key.lod.level = static_cast<std::uint8_t>(std::min<std::ptrdiff_t>(
                    std::lower_bound(bands.begin(), bands.end(), part.lod_near) - bands.begin(), 255));
                key.lod.levels = static_cast<std::uint8_t>(std::min<std::size_t>(bands.size(), 255));
            }
            // Classic regions' plain materials: each triangle by the regions around it (classic_liquid_triangles).
            const bool classify = region && liquid != nullptr;
            for (std::uint32_t i = 0; i + 2 < part.index_count; i += 3) {
                std::uint32_t t[3];
                bool valid = true;
                for (int k = 0; k < 3; ++k) {
                    t[k] = mesh.indices[part.first_index + i + k];
                    valid &= t[k] < mesh.vertices.size();
                }
                if (!valid) continue;
                MaterialKey tri_key = key;
                if (classify) {
                    const std::size_t at = (part.first_index + i) / 3;
                    if (at < liquid->size() && (*liquid)[at]) {
                        tri_key.liquid = (*liquid)[at];
                        ++liquid_triangles;
                    }
                }
                if (measure_horizon && region && (tri_key.liquid != 0 || m.blend == assets::BlendMode::Opaque)) {
                    const GpuVertex &va = vertices[base + t[0]], &vb = vertices[base + t[1]], &vc = vertices[base + t[2]];
                    horizon_triangles.push_back({va.position, vb.position, vc.position, tri_key.liquid != 0});
                    HorizonUse& use = horizon_uses[tri_key];
                    const glm::vec3 n = glm::cross(vb.position - va.position, vc.position - va.position);
                    const float twice = glm::length(n);
                    if (twice > 1e-4f && std::isfinite(twice)) {
                        const float up = std::abs(n.z) / twice;
                        const glm::vec2 du = vb.uv - va.uv, dv = vc.uv - va.uv;
                        use.uv_area += 0.5f * std::abs(du.x * dv.y - du.y * dv.x);
                        use.area += 0.5f * twice;
                        if (tri_key.liquid != 0) {
                            ++use.liquid;
                            use.up += n.z > 0.0f ? 0.5f * twice : -0.5f * twice;
                            if (use.colors.size() < 4096)
                                use.colors.insert(use.colors.end(), {va.color, vb.color, vc.color});
                        } else if (up >= kHorizonLevel) {
                            use.level += 0.5f * twice;
                            use.up += n.z > 0.0f ? 0.5f * twice : -0.5f * twice;
                            if (use.colors.size() < 4096) use.colors.insert(use.colors.end(), {va.color, vb.color, vc.color});
                        } else if (up >= 0.3f) {
                            use.slope += 0.5f * twice;
                        }
                    }
                }
                auto& tris_out = out_by_material[tri_key];
                looks.try_emplace(tri_key, m);
                for (std::uint32_t k : t) tris_out.push_back(base + k);
            }
        }
        if (region && options.details) append_details(mesh, base);
    };

    // Vertex colours are the WLD's, unchanged: RGB is baked light, alpha how
    // much ambient and sun reach the vertex (CRender::BuildRegionVertices
    // @0x1008d380; game/scene_light.h).
    static const std::vector<std::uint32_t> kNoColors;
    // options.cancelled: a load thrown away (a neighbour's, on zoning) stops here and between materials below.
    auto cancelled = [&] { return options.cancelled && options.cancelled(); };
    for (std::size_t mi = 0; mi < zone.terrain.size(); ++mi) {
        if (cancelled()) return nullptr;
        append(zone.terrain[mi], glm::mat4(1.0f), kNoColors, true, false, &terrain_liquid[mi]);
    }
    std::size_t missing_objects = 0, replaced_objects = 0;
    for (std::size_t pi = 0; pi < zone.placements.size(); ++pi) {
        if (pi % 256 == 0 && cancelled()) return nullptr;
        const auto& p = zone.placements[pi];
        auto it = zone.objects.find(p.actor);
        if (it == zone.objects.end()) { ++missing_objects; continue; }
        // A replacement model (assets/model_overrides.h): the placement's baked light is one colour per vertex of the
        // game's mesh, so the replacement takes their average (its shade, not its gradient), times its own vertex
        // colours (a generated tree's shade inside its crown).
        if (auto r = zone.draw_objects.find(p.actor); r != zone.draw_objects.end()) {
            glm::vec4 tint(1.0f);
            if (!p.vertex_colors.empty()) {
                std::uint64_t sum[4] = {};
                for (std::uint32_t c : p.vertex_colors)
                    for (int k = 0; k < 4; ++k) sum[k] += (c >> (8 * k)) & 0xff;
                for (int k = 0; k < 4; ++k)
                    tint[k] = static_cast<float>(sum[k]) / static_cast<float>(p.vertex_colors.size()) / 255.0f;
            }
            auto [number, fresh] = model_numbers.try_emplace(&r->second, 0u);
            if (fresh) {
                models.emplace_back();
                number->second = static_cast<std::uint32_t>(models.size());
                append(r->second, glm::mat4(1.0f), kNoColors, false, is_plant(p.actor, r->second), nullptr,
                       number->second);
            }
            InstancedModel& model = models[number->second - 1];
            model.instances.push_back(make_instance(p.transform, tint));
            model.transforms.push_back(p.transform);
            ++replaced_objects;
            continue;
        }
        append(it->second, p.transform, p.vertex_colors, false, is_plant(p.actor, it->second));
    }
    if (plants) log::info("zone renderer: {} plants sway in the wind", plants);
    if (missing_objects) log::warn("{} placements name objects the zone does not define", missing_objects);
    if (replaced_objects)
        log::info("zone renderer: {} placements drawn from model overrides, {} models instanced", replaced_objects,
                  models.size());
    if (options.multipass_lighting)
        log::info("multi-pass lighting: {} lights reach {} zone vertices", zone.lights.size(), lit_vertices);
    if (liquid_triangles) log::info("zone renderer: {} water and lava surface triangles", liquid_triangles);
    if (detail_triangles) log::info("zone renderer: {} detail texture triangles", detail_triangles);
    mark("mesh loop done", prep.started);

    // The land round the zone (render/horizon.h): a mesh past its edges, drawn with the zone's own plain materials
    // that cover the most level ground, slopes and water (their texture scale, their winding, the level ground's
    // median baked light), so it is shaded, lit and hazed as the zone is. Its draws are `distant`: no shadows, shown
    // by set_distant.
    if (measure_horizon) {
        prep.horizon = horizon_land(horizon_triangles, horizon_seed(zone.name));
        const HorizonMesh land = horizon_mesh(horizon_triangles, prep.horizon);
        const MaterialKey *ground = nullptr, *rock = nullptr, *water = nullptr;
        for (const auto& [key, use] : horizon_uses) {
            if (!(use.uv_area > 0.0f) || !(use.area > 0.0f)) continue;
            const bool plain = key.eqg == assets::EqgShader::None && !key.frames.empty() &&
                               looks.at(key).effect.slide == glm::vec4(0.0f);
            if (plain && key.liquid == 0) {
                if (!ground || use.level > horizon_uses.at(*ground).level) ground = &key;
                if (!rock || use.slope > horizon_uses.at(*rock).slope) rock = &key;
            }
            if (key.liquid == static_cast<std::uint8_t>(Liquid::Water) &&
                (!water || use.liquid > horizon_uses.at(*water).liquid))
                water = &key;
        }
        if (ground && !(horizon_uses.at(*ground).level > 0.0f)) ground = nullptr;
        if (ground && !(horizon_uses.at(*rock).slope > 0.0f)) rock = ground;
        std::size_t land_triangles = 0;
        if (ground && !land.empty()) {
            // The median of each channel, so a few lamp-lit vertices do not tint the land.
            auto median_color = [](const std::vector<std::uint32_t>& colors) {
                if (colors.empty()) return 0xffffffffu;
                std::uint32_t out = 0;
                std::vector<std::uint32_t> channel(colors.size());
                for (std::uint32_t k = 0; k < 4; ++k) {
                    for (std::size_t i = 0; i < colors.size(); ++i) channel[i] = (colors[i] >> (8 * k)) & 0xffu;
                    const auto mid = channel.begin() + static_cast<std::ptrdiff_t>(channel.size() / 2);
                    std::nth_element(channel.begin(), mid, channel.end());
                    out |= *mid << (8 * k);
                }
                return out;
            };
            const std::uint32_t land_color = median_color(horizon_uses.at(*ground).colors);
            auto add_part = [&](const MaterialKey& from, const std::vector<std::uint32_t>& tris, std::uint32_t color) {
                if (tris.empty()) return;
                const HorizonUse& use = horizon_uses.at(from);
                const float density = std::sqrt(use.uv_area / use.area); // texture units a world unit
                const bool flip = use.up < 0.0f; // the zone's own faces up the other way round
                MaterialKey key = from;
                key.distant = true;
                key.multipass = false;
                looks.try_emplace(key, looks.at(from));
                auto& out = by_material[key];
                std::unordered_map<std::uint32_t, std::uint32_t> made;
                for (std::size_t i = 0; i + 2 < tris.size(); i += 3) {
                    const std::uint32_t t[3] = {tris[i], tris[i + (flip ? 2 : 1)], tris[i + (flip ? 1 : 2)]};
                    for (std::uint32_t v : t) {
                        auto [it, fresh] = made.try_emplace(v, static_cast<std::uint32_t>(vertices.size()));
                        if (fresh) {
                            GpuVertex g{};
                            g.position = land.positions[v];
                            g.normal = land.normals[v];
                            g.uv = glm::vec2(g.position) * density;
                            std::uint32_t c = color & 0xff000000u;
                            for (std::uint32_t k = 0; k < 3; ++k)
                                c |= static_cast<std::uint32_t>(
                                         static_cast<float>((color >> (8 * k)) & 0xffu) * land.shade[v] + 0.5f)
                                     << (8 * k);
                            g.color = c;
                            vertices.push_back(g);
                        }
                        out.push_back(it->second);
                    }
                }
                land_triangles += tris.size() / 3;
            };
            add_part(*ground, land.ground, land_color);
            add_part(*rock, land.rock, land_color);
            if (water) add_part(*water, land.water, median_color(horizon_uses.at(*water).colors));
        }
        const HorizonLand& h = prep.horizon;
        if (land_triangles)
            log::info("zone renderer: the land round the zone, {} triangles ({} ground '{}', {} rock '{}', {} water), "
                      "level ground ({:.0f} {:.0f}) to ({:.0f} {:.0f}) at {:.0f}, relief {:.0f}",
                      land_triangles, land.ground.size() / 3, ground->frames.front(), land.rock.size() / 3,
                      rock->frames.front(), water ? land.water.size() / 3 : 0, h.lo.x, h.lo.y, h.hi.x, h.hi.y, h.ground,
                      h.relief);
        else
            log::info("zone renderer: no land round the zone ({} terrain triangles{})", horizon_triangles.size(),
                      !h.valid ? ", too little level ground" : ground ? "" : ", no plain ground material");
    }

    // A neighbouring zone: moved into the current zone's coordinates, then cut to what lies past the zone line.
    if (options.offset != glm::vec3(0.0f) || !options.keep.empty()) {
        for (auto& v : vertices) {
            v.position += options.offset;
            if (v.sway.z != 0.0f || v.sway.w != 0.0f) v.sway += glm::vec4(options.offset.x, options.offset.y, 0.0f, 0.0f);
        }
        std::size_t kept = 0, cut = 0, dropped = 0, doubled = 0, split = 0;
        std::vector<GpuVertex> poly, scratch;
        auto position = [](const GpuVertex& v) { return v.position; };
        // The vertex at barycentric weights w over (a, b, c).
        auto blend = [](const GpuVertex& a, const GpuVertex& b, const GpuVertex& c, const glm::vec3& w) {
            const float ab = w.x + w.y;
            const GpuVertex m = ab > 0.0f ? lerp_vertex(a, b, w.y / ab) : a;
            return lerp_vertex(m, c, w.z);
        };
        for (auto& [key, list] : by_material) {
            std::vector<std::uint32_t> out;
            out.reserve(list.size());
            // `poly` cut to the keep planes and appended as a fan; false when nothing of it is left.
            auto emit = [&]() {
                for (const glm::vec4& plane : options.keep) game::clip_polygon(poly, plane, position, lerp_vertex, scratch);
                if (poly.size() < 3) return false;
                const auto first = static_cast<std::uint32_t>(vertices.size());
                vertices.insert(vertices.end(), poly.begin(), poly.end());
                for (std::uint32_t k = 1; k + 1 < poly.size(); ++k) {
                    out.push_back(first);
                    out.push_back(first + k);
                    out.push_back(first + k + 1);
                }
                return true;
            };
            for (std::size_t i = 0; i + 2 < list.size(); i += 3) {
                const std::uint32_t t[3] = {list[i], list[i + 1], list[i + 2]};
                if (options.split) {
                    auto parts = options.split(vertices[t[0]].position, vertices[t[1]].position, vertices[t[2]].position);
                    if (parts) {
                        if (parts->empty()) {
                            ++doubled;
                            continue;
                        }
                        ++split;
                        // Copies: emit appends to `vertices`, which may move the corners.
                        const GpuVertex a = vertices[t[0]], b = vertices[t[1]], c = vertices[t[2]];
                        for (const auto& piece : *parts) {
                            poly.clear();
                            for (const glm::vec3& w : piece) poly.push_back(blend(a, b, c, w));
                            emit();
                        }
                        continue;
                    }
                }
                bool inside = true, outside = false;
                for (const glm::vec4& plane : options.keep) {
                    int in = 0;
                    for (std::uint32_t k : t) in += game::plane_side(plane, vertices[k].position) >= 0.0f;
                    inside &= in == 3;
                    outside |= in == 0;
                }
                if (inside) {
                    out.insert(out.end(), t, t + 3);
                    ++kept;
                    continue;
                }
                if (outside) {
                    ++dropped;
                    continue;
                }
                poly = {vertices[t[0]], vertices[t[1]], vertices[t[2]]};
                if (emit()) ++cut;
                else ++dropped;
            }
            list = std::move(out);
        }
        std::erase_if(by_material, [](const auto& kv) { return kv.second.empty(); });
        // Where the current zone's land is not drawn (set_distant_hidden): the squares its opaque geometry covers.
        std::vector<HorizonTriangle> covering;
        for (const auto& [key, list] : by_material)
            if (key.blend == assets::BlendMode::Opaque && key.liquid == 0)
                for (std::size_t i = 0; i + 2 < list.size(); i += 3)
                    covering.push_back({vertices[list[i]].position, vertices[list[i + 1]].position,
                                        vertices[list[i + 2]].position, false});
        prep.footprint = horizon_footprint(covering);
        log::info("zone renderer: moved by ({:.1f} {:.1f} {:.1f}), {} triangles kept, {} cut at the planes, {} left "
                  "out, {} doubling the ground left out, {} cut where they double it",
                  options.offset.x, options.offset.y, options.offset.z, kept, cut, dropped, doubled, split);
    }

    // The instanced models join the geometry after the cut: their meshes' vertices and triangles at the end, their
    // placements in the instance buffer after the identity, each model's together and by 128-unit cell (so that the
    // neighbouring ones in view go in one call). A neighbour zone's are moved with it and kept whole or left out by
    // where they stand.
    prep.instances.assign(1, GpuInstance{});
    {
        const auto vbase = static_cast<std::uint32_t>(vertices.size());
        vertices.insert(vertices.end(), model_vertices.begin(), model_vertices.end());
        for (auto& [key, list] : model_by_material) {
            for (std::uint32_t& i : list) i += vbase;
            by_material.emplace(key, std::move(list));
        }
        const bool moved = options.offset != glm::vec3(0.0f) || !options.keep.empty();
        std::size_t left_out = 0;
        for (InstancedModel& model : models) {
            struct Placed { std::int64_t cell; std::size_t at; };
            std::vector<Placed> order;
            for (std::size_t i = 0; i < model.instances.size(); ++i) {
                glm::mat4& m = model.transforms[i];
                if (moved) {
                    m[3] += glm::vec4(options.offset, 0.0f);
                    for (int r = 0; r < 3; ++r) model.instances[i].row[r].w = m[3][r];
                    const glm::vec3 at(m[3]);
                    if (!std::all_of(options.keep.begin(), options.keep.end(),
                                     [&](const glm::vec4& plane) { return game::plane_side(plane, at) >= 0.0f; })) {
                        ++left_out;
                        continue;
                    }
                }
                order.push_back({cell_key(static_cast<std::int64_t>(std::floor(m[3].x / 128.0f)),
                                          static_cast<std::int64_t>(std::floor(m[3].y / 128.0f))),
                                 i});
            }
            std::stable_sort(order.begin(), order.end(),
                             [](const Placed& a, const Placed& b) { return a.cell < b.cell; });
            std::vector<GpuInstance> instances;
            std::vector<glm::mat4> transforms;
            for (const Placed& o : order) {
                instances.push_back(model.instances[o.at]);
                transforms.push_back(model.transforms[o.at]);
            }
            model.instances = std::move(instances);
            model.transforms = std::move(transforms);
            model.first_instance = static_cast<std::uint32_t>(prep.instances.size());
            prep.instances.insert(prep.instances.end(), model.instances.begin(), model.instances.end());
        }
        if (left_out) log::info("zone renderer: {} model placements past the zone line left out", left_out);
    }
    // Texture slot 0 is plain white, used for anything that fails to load.
    const std::size_t replaced_before = assets::texture_overrides().applied();
    {
        assets::Image white;
        white.width = white.height = 1;
        white.mips.push_back(std::vector<std::byte>(4, std::byte{0xff}));
        prep.images.push_back({std::move(white), TextureUse::Data});
    }
    // A neighbour's textures (options.max_texture_size): their chains built here, the large levels dropped.
    auto keep_image = [&](assets::Image image, TextureUse use) {
        prepare_capped_texture(image, use, options.max_texture_size);
        prep.images.push_back({std::move(image), use});
    };
    std::unordered_map<std::string, std::uint32_t> slots; // name + masked, data and reads-alpha flags -> slot
    auto slot_for = [&](const std::string& name, bool masked, TextureUse use = TextureUse::Color,
                        bool reads_alpha = false) -> std::uint32_t {
        if (name.empty()) return 0;
        std::string key = name + (masked ? "#m" : "") + (use == TextureUse::Data ? "#d" : "") + (reads_alpha ? "#a" : "");
        if (auto it = slots.find(key); it != slots.end()) return it->second;
        std::uint32_t slot = 0;
        auto data = zone.read_file(name);
        std::string error;
        std::optional<assets::Image> image;
        image = assets::decode_game_texture(
            name, data, {.palette_index0_transparent = masked, .wld_bmp_rows = true, .reads_alpha = reads_alpha}, &error,
            zone.name);
        if (image && prep.images.size() < max_images) {
            slot = static_cast<std::uint32_t>(prep.images.size());
            keep_image(std::move(*image), use);
        } else {
            ++prep.missing_textures;
            log::warn("zone texture {}: {}", name, data ? error : "not found");
        }
        slots.emplace(std::move(key), slot);
        return slot;
    };
    // A texture pack's normal or roughness map for the texture `name` (assets/texture_overrides.h), 0 = none.
    auto map_slot_for = [&](const std::string& name, assets::TextureMap map) -> std::uint32_t {
        if (name.empty()) return 0;
        std::string key = name + (map == assets::TextureMap::Normal ? "#n" : "#r");
        if (auto it = slots.find(key); it != slots.end()) return it->second;
        std::uint32_t slot = 0;
        auto image = assets::decode_texture_map(name, zone.read_file(name), map, true, zone.name);
        if (image && prep.images.size() < max_images && prep.images.size() <= 0xffff) {
            slot = static_cast<std::uint32_t>(prep.images.size());
            keep_image(std::move(*image), TextureUse::Data);
        }
        slots.emplace(std::move(key), slot);
        return slot;
    };

    // A water material's own environment, a cube map whose name holds "noswap" (the original keeps it instead of the
    // scene's sky cube; a cave's, a volcano's): laid out as an octahedral map (assets::decode_cube_as_octahedral).
    auto env_slot_for = [&](const std::string& name) -> std::uint32_t {
        if (name.find("noswap") == std::string::npos) return 0;
        std::string key = name + "#env";
        if (auto it = slots.find(key); it != slots.end()) return it->second;
        std::uint32_t slot = 0;
        if (auto data = zone.read_file(name))
            if (auto image = assets::decode_cube_as_octahedral(*data); image && prep.images.size() < max_images) {
                slot = static_cast<std::uint32_t>(prep.images.size());
                keep_image(std::move(*image), TextureUse::Color);
            }
        if (!slot) log::debug("zone environment map {}: not a cube map", name);
        slots.emplace(std::move(key), slot);
        return slot;
    };

    std::vector<std::uint32_t> indices;
    for (auto& [key, list] : by_material) {
        if (cancelled()) return nullptr; // (each material may decode its textures)
        const assets::Material& look = looks.at(key);
        Draw d;
        d.first_index = static_cast<std::uint32_t>(indices.size());
        d.index_count = static_cast<std::uint32_t>(list.size());
        d.frame_delay_ms = key.delay;
        d.two_sided = key.two_sided;
        d.front_ccw = key.front_ccw;
        d.lod = key.lod;
        d.instanced = key.instanced;
        MaterialState st = material_state(look);
        for (const auto& name : key.frames) d.frame_slots.push_back(slot_for(name, st.masked_texture, TextureUse::Color, !st.alpha_unused()));
        const auto& e = look.effect;
        for (std::size_t i = 0; i < e.textures.size() && i < d.extra_slots.size(); ++i)
            d.extra_slots[i] = slot_for(e.textures[i], false);
        d.mode = mode_of(look.eqg_shader);
        if (d.mode == kClassic && !key.frames.empty() && options.material_maps)
            d.maps = map_slot_for(key.frames.front(), assets::TextureMap::Normal) |
                     map_slot_for(key.frames.front(), assets::TextureMap::Roughness) << 16;
        d.slide = e.slide;
        if (d.mode == kClassic) {
            // The surface maps of the opaque, Alpha and Chroma classes (the modern lighting's): the normal map in
            // slot 1, filtered as data.
            if (!e.normal_map.empty()) {
                d.extra_slots[0] = slot_for(e.normal_map, false, TextureUse::Data);
                d.surface |= d.extra_slots[0] != 0 ? kNormalMap : 0u;
            }
            d.surface |= (e.specular ? kSpecular : 0u) | (e.reflective && e.env_strength > 0.0f ? kReflective : 0u);
            d.extra = {e.shininess, e.gloss, e.env_strength, 0.0f};
            // A material's own normal map and highlight win over a texture pack's.
            if ((d.surface & (kNormalMap | kSpecular)) != 0u) d.maps = 0;
        }
        // RegionLava draws more than lava (energy, slime, mist): the shader makes a fire-coloured texture lava, and
        // one named so whatever its colour.
        if (d.mode == kLava && !key.frames.empty()) {
            const std::string& name = key.frames.front();
            for (const char* word : {"lava", "magma", "molten"})
                if (name.find(word) != std::string::npos) d.surface |= kMolten;
        }
        if (look.eqg_shader == assets::EqgShader::LavaH) d.surface |= kEnergy;
        switch (d.mode) {
        case kTerrainTile: d.extra = e.detail_scale; break;
        case kRegionTerrain: d.param_z = e.coverage_scale; break;
        case kWater:
            d.extra = {e.fresnel_bias, e.fresnel_power, e.uv_scale, e.reflection_amount};
            d.extra2 = e.water_color1;
            d.extra3 = e.water_color2;
            d.extra4 = glm::vec4(glm::vec3(e.reflection_color), e.reflection_amount);
            if (e.textures.size() > 1)
                if (std::uint32_t env = env_slot_for(e.textures[1])) {
                    d.extra_slots[1] = env;
                    d.surface |= kEnvMap;
                }
            break;
        default: break;
        }
        d.multipass = key.multipass;
        d.liquid = static_cast<Liquid>(key.liquid);
        d.flame = key.flame;
        d.distant = key.distant;
        d.pass = static_cast<Pass>(st.pass);
        d.alpha_ref = st.alpha_ref;
        d.opacity = st.opacity;
        d.fog = st.fog;
        d.lit = st.lit;
        indices.insert(indices.end(), list.begin(), list.end());
        prep.draws.push_back(std::move(d));
    }
    std::stable_sort(prep.draws.begin(), prep.draws.end(), [](const Draw& a, const Draw& b) { return a.pass < b.pass; });
    // The draws' triangles in chunks, the culling's unit (render/culling.h; draw, draw_shadow, draw_point_shadow). An
    // opaque draw's triangles, whose order shows nowhere (the depth test settles what is in front), are sorted by the
    // 128-unit cell of their centre, one chunk a cell. A blended or additive draw keeps its triangles' order (a
    // blended one shows it where its triangles cross), so its chunks are runs of neighbouring triangles: a new one
    // starts when the next triangle would stretch the run's box past a cell's size across. A placement appends its
    // triangles together, so a run is mostly one object or a stretch of the zone's own surface. Each vertex counts
    // with the room its sway can take it (sway_reach).
    constexpr float kChunk = 128.0f;
    std::vector<std::int64_t> chunk_cells; // an opaque draw's chunks' cells, by chunk
    for (std::uint32_t di = 0; di < prep.draws.size(); ++di) {
        Draw& d = prep.draws[di];
        const std::uint32_t count = d.index_count / 3;
        if (d.instanced) {
            // An instanced model's draw: a chunk for each placement, all its triangles, its box the mesh's moved by
            // the placement's transform.
            glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
            bool sways = false;
            for (std::uint32_t i = 0; i < d.index_count; ++i) {
                const GpuVertex& v = vertices[indices[d.first_index + i]];
                const float r = sway_reach(v.sway);
                sways |= r > 0.0f;
                lo = glm::min(lo, v.position - r);
                hi = glm::max(hi, v.position + r);
            }
            const InstancedModel& model = models[d.instanced - 1];
            d.first_chunk = static_cast<std::uint32_t>(prep.chunks.size());
            for (std::size_t i = 0; i < model.instances.size() && d.index_count > 0; ++i) {
                const glm::mat4& m = model.transforms[i];
                Chunk c;
                c.draw = di;
                c.first_index = d.first_index;
                c.index_count = d.index_count;
                c.instance = model.first_instance + static_cast<std::uint32_t>(i);
                c.sways = sways;
                c.box_min = glm::vec3(std::numeric_limits<float>::max());
                c.box_max = glm::vec3(-std::numeric_limits<float>::max());
                for (int k = 0; k < 8; ++k) {
                    const glm::vec3 corner((k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z);
                    const glm::vec3 w(m * glm::vec4(corner, 1.0f));
                    c.box_min = glm::min(c.box_min, w);
                    c.box_max = glm::max(c.box_max, w);
                }
                prep.chunks.push_back(c);
                chunk_cells.push_back(0);
                prep.triangles += count;
                d.box_min = i == 0 ? c.box_min : glm::min(d.box_min, c.box_min);
                d.box_max = i == 0 ? c.box_max : glm::max(d.box_max, c.box_max);
            }
            d.chunk_count = static_cast<std::uint32_t>(prep.chunks.size()) - d.first_chunk;
            continue;
        }
        prep.triangles += count;
        const bool sorted = d.pass == Pass::Opaque;
        std::vector<std::int64_t> keys;
        if (sorted) {
            struct Tri { std::int64_t key; std::uint32_t at; };
            std::vector<Tri> tris(count);
            for (std::uint32_t t = 0; t < count; ++t) {
                const std::uint32_t* idx = &indices[d.first_index + t * 3];
                const glm::vec3 c =
                    (vertices[idx[0]].position + vertices[idx[1]].position + vertices[idx[2]].position) / 3.0f;
                const float cell = d.distant ? kDistantChunk : kChunk;
                tris[t] = {cell_key(static_cast<std::int64_t>(std::floor(c.x / cell)),
                                    static_cast<std::int64_t>(std::floor(c.y / cell))), t};
            }
            std::stable_sort(tris.begin(), tris.end(), [](const Tri& a, const Tri& b) { return a.key < b.key; });
            std::vector<std::uint32_t> old(indices.begin() + d.first_index,
                                           indices.begin() + d.first_index + std::size_t{count} * 3);
            keys.resize(count);
            for (std::uint32_t t = 0; t < count; ++t) {
                for (int k = 0; k < 3; ++k)
                    indices[d.first_index + t * 3 + static_cast<std::uint32_t>(k)] =
                        old[tris[t].at * 3 + static_cast<std::size_t>(k)];
                keys[t] = tris[t].key;
            }
        }
        d.first_chunk = static_cast<std::uint32_t>(prep.chunks.size());
        for (std::uint32_t t = 0; t < count; ++t) {
            const std::uint32_t* idx = &indices[d.first_index + t * 3];
            glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
            bool sways = false;
            for (int k = 0; k < 3; ++k) {
                const GpuVertex& v = vertices[idx[k]];
                float r = sway_reach(v.sway);
                sways |= r > 0.0f;
                lo = glm::min(lo, v.position - r);
                hi = glm::max(hi, v.position + r);
            }
            bool fresh = t == 0;
            if (!fresh && sorted) fresh = keys[t] != keys[t - 1];
            if (!fresh && !sorted) {
                const Chunk& c = prep.chunks.back();
                glm::vec3 span = glm::max(c.box_max, hi) - glm::min(c.box_min, lo);
                fresh = span.x > kChunk || span.y > kChunk;
            }
            if (fresh) {
                Chunk c;
                c.draw = di;
                c.first_index = d.first_index + t * 3;
                c.box_min = lo;
                c.box_max = hi;
                prep.chunks.push_back(c);
                chunk_cells.push_back(sorted ? keys[t] : 0);
            }
            Chunk& c = prep.chunks.back();
            c.index_count += 3;
            c.sways |= sways;
            c.box_min = glm::min(c.box_min, lo);
            c.box_max = glm::max(c.box_max, hi);
        }
        d.chunk_count = static_cast<std::uint32_t>(prep.chunks.size()) - d.first_chunk;
        for (std::uint32_t i = 0; i < d.chunk_count; ++i) {
            const Chunk& c = prep.chunks[d.first_chunk + i];
            d.box_min = i == 0 ? c.box_min : glm::min(d.box_min, c.box_min);
            d.box_max = i == 0 ? c.box_max : glm::max(d.box_max, c.box_max);
        }
    }
    // The solid casters in one chunk a cell, for the shadow passes (render/shadow_chunks.h): the draws every shadow
    // pass but the water map draws with no texture (draw_shadow's cover map criterion), their indices copied to the
    // end of the buffer.
    {
        std::vector<CellChunk> solid;
        for (const Draw& d : prep.draws) {
            if (!casts_untextured(d)) continue;
            for (std::uint32_t i = d.first_chunk; i < d.first_chunk + d.chunk_count; ++i) {
                const Chunk& c = prep.chunks[i];
                solid.push_back({chunk_cells[i], c.first_index, c.index_count, Box{c.box_min, c.box_max}, c.sways});
            }
        }
        prep.draw_chunks = prep.chunks.size();
        Draw& s = prep.solid;
        s.first_chunk = static_cast<std::uint32_t>(prep.chunks.size());
        for (const CellChunk& m : merge_cell_chunks(solid, indices)) {
            Chunk c;
            c.draw = std::numeric_limits<std::uint32_t>::max();
            c.first_index = m.first_index;
            c.index_count = m.index_count;
            c.box_min = m.box.min;
            c.box_max = m.box.max;
            c.sways = m.sways;
            s.box_min = s.chunk_count == 0 ? c.box_min : glm::min(s.box_min, c.box_min);
            s.box_max = s.chunk_count == 0 ? c.box_max : glm::max(s.box_max, c.box_max);
            if (s.chunk_count == 0) s.first_index = c.first_index;
            s.index_count += c.index_count;
            ++s.chunk_count;
            prep.chunks.push_back(c);
        }
    }
    // The land round the zone's triangles once more, in the distant draws' order with each one's middle, for its own
    // index buffers (set_distant_hidden).
    for (Draw& d : prep.draws) {
        if (!d.distant) continue;
        d.distant_first = static_cast<std::uint32_t>(prep.distant_indices.size());
        for (std::uint32_t i = d.first_index; i + 2 < d.first_index + d.index_count; i += 3) {
            prep.distant_indices.insert(prep.distant_indices.end(), {indices[i], indices[i + 1], indices[i + 2]});
            prep.distant_centres.push_back(glm::vec2(vertices[indices[i]].position + vertices[indices[i + 1]].position +
                                                     vertices[indices[i + 2]].position) /
                                           3.0f);
        }
    }
    if (std::size_t replaced = assets::texture_overrides().applied() - replaced_before)
        log::info("zone renderer: {} textures replaced from {}", replaced, assets::texture_overrides().folder().string());

    // The light grid: a header (origin, 1 / cell; width, height, light count) and the lights, then the cells and
    // their light numbers. Each buffer holds at least one element, for a zone without lights.
    {
        LightGrid grid = build_light_grid(zone.lights);
        struct Header { glm::vec4 grid; glm::uvec4 dims; };
        std::vector<std::byte>& lights = prep.lights;
        lights.resize(sizeof(Header));
        Header h{glm::vec4(grid.origin, 1.0f / grid.cell, 0.0f),
                 glm::uvec4(grid.width, grid.height, static_cast<std::uint32_t>(zone.lights.size()), 0u)};
        std::memcpy(lights.data(), &h, sizeof(h));
        for (const auto& l : zone.lights) {
            GpuPointLight g{glm::vec4(l.position, l.radius), glm::vec4(l.color, 0.0f)};
            const auto* b = reinterpret_cast<const std::byte*>(&g);
            lights.insert(lights.end(), b, b + sizeof(g));
        }
        if (grid.cells.empty()) grid.cells.emplace_back(0u, 0u);
        if (grid.indices.empty()) grid.indices.push_back(0u);
        auto cells = std::as_bytes(std::span(grid.cells));
        prep.grid_cells.assign(cells.begin(), cells.end());
        auto numbers = std::as_bytes(std::span(grid.indices));
        prep.grid_indices.assign(numbers.begin(), numbers.end());
        if (grid.width) log::info("zone renderer: {} lights in a {}x{} grid of {:.0f}-unit cells", zone.lights.size(),
                                  grid.width, grid.height, grid.cell);
    }
    prep.indices = std::move(indices);
    mark("prepared", prep.started);
    return prepared;
}

bool multipass_lighting_setting(std::string_view v) {
    return !v.empty() && (v[0] == 't' || v[0] == 'T' || v[0] == '1');
}

ZoneRenderOptions zone_render_options(const GameDir& game) {
    ZoneRenderOptions o;
    IniFile ini;
    if (auto path = game.find("eqclient.ini")) ini.load(*path);
    o.multipass_lighting = multipass_lighting_setting(ini.get_string("Defaults", "MultiPassLighting"));
    return o;
}

void ZoneRenderer::write_texture_set() {
    std::vector<VkDescriptorImageInfo> infos;
    for (const auto& t : textures_) infos.push_back({sampler_, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = 0;
    w.descriptorCount = static_cast<std::uint32_t>(infos.size());
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = infos.data();
    vkUpdateDescriptorSets(vk_.device(), 1, &w, 0, nullptr);
}

void ZoneRenderer::create_descriptors() {
    VkDevice d = vk_.device();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vk_.caps().max_anisotropy > 0.0f) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(8.0f, vk_.caps().max_anisotropy);
    }
    vk_check(vkCreateSampler(d, &si, nullptr, &sampler_), "vkCreateSampler");

    auto count = static_cast<std::uint32_t>(textures_.size());
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = count;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorBindingFlags flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
        vk_.texture_array_binding_flags();
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 1;
    bf.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bf;
    li.flags = vk_.texture_array_layout_flags();
    li.bindingCount = 1;
    li.pBindings = &binding;
    vk_check(vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");

    // Set 1: the scene lighting constants, one uniform buffer per frame in flight; the light grid's three buffers.
    VkDescriptorSetLayoutBinding ub[4]{};
    ub[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (std::uint32_t i = 1; i < 4; ++i)
        ub[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 4;
    sli.pBindings = ub;
    vk_check(vkCreateDescriptorSetLayout(d, &sli, nullptr, &scene_layout_), "vkCreateDescriptorSetLayout (scene)");
    for (std::size_t i = 0; i < scene_buffers_.size(); ++i) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sizeof(SceneUniforms);
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo out{};
        vk_check(vmaCreateBuffer(vk_.allocator(), &bi, &ai, &scene_buffers_[i].buffer, &scene_buffers_[i].allocation,
                                 &out),
                 "create scene uniform buffer");
        scene_buffers_[i].size = bi.size;
        scene_mapped_[i] = out.pMappedData;
    }

    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VkContext::kFramesInFlight},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * VkContext::kFramesInFlight}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    // The texture set's flags; the scene sets (set 1) come from it too, as a plain layout may.
    pi.flags = vk_.texture_array_pool_flags();
    pi.maxSets = 1 + VkContext::kFramesInFlight;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    vk_check(vkCreateDescriptorPool(d, &pi, nullptr, &pool_), "vkCreateDescriptorPool");

    VkDescriptorSetVariableDescriptorCountAllocateInfo vc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    vc.descriptorSetCount = 1;
    vc.pDescriptorCounts = &count;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.pNext = &vc;
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout_;
    vk_check(vkAllocateDescriptorSets(d, &ai, &set_), "vkAllocateDescriptorSets");

    write_texture_set();

    for (std::size_t i = 0; i < scene_sets_.size(); ++i) {
        VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sai.descriptorPool = pool_;
        sai.descriptorSetCount = 1;
        sai.pSetLayouts = &scene_layout_;
        vk_check(vkAllocateDescriptorSets(d, &sai, &scene_sets_[i]), "vkAllocateDescriptorSets (scene)");
        VkDescriptorBufferInfo bi[4] = {{scene_buffers_[i].buffer, 0, sizeof(SceneUniforms)}};
        for (std::uint32_t b = 0; b < 3; ++b) bi[b + 1] = {light_grid_buffers_[b].buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet sw[4]{};
        for (std::uint32_t b = 0; b < 4; ++b) {
            sw[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            sw[b].dstSet = scene_sets_[i];
            sw[b].dstBinding = b;
            sw[b].descriptorCount = 1;
            sw[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            sw[b].pBufferInfo = &bi[b];
        }
        vkUpdateDescriptorSets(d, 4, sw, 0, nullptr);
    }
}

void ZoneRenderer::set_scene_light(const game::SceneLight& light) {
    scene_.lighting.ambient = glm::vec4(light.ambient, 0.0f);
    scene_.lighting.special_ambient = glm::vec4(light.special_ambient, 0.0f);
    scene_.lighting.directional = glm::vec4(light.directional, 0.0f);
    scene_.lighting.direction = glm::vec4(light.direction, 0.0f);
    scene_.lighting.bounce = glm::vec4(light.bounce, 0.0f);
}

void ZoneRenderer::set_lighting(const SceneLighting& lighting) {
    // The classic fields stay as set_scene_light left them.
    SceneLighting l = lighting;
    l.ambient = scene_.lighting.ambient;
    l.special_ambient = scene_.lighting.special_ambient;
    l.directional = scene_.lighting.directional;
    l.direction = scene_.lighting.direction;
    l.bounce = scene_.lighting.bounce;
    scene_.lighting = l;
}

void ZoneRenderer::create_pipelines(const std::filesystem::path& shader_dir) {
    VkDevice d = vk_.device();
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout set_layouts[3] = {set_layout_, scene_layout_, vk_.shadow_set_layout()};
    li.setLayoutCount = 3;
    li.pSetLayouts = set_layouts;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pc;
    vk_check(vkCreatePipelineLayout(d, &li, nullptr, &layout_), "vkCreatePipelineLayout");

    VkShaderModule vs = create_shader(d, shader_dir / "zone.vert.spv");
    VkShaderModule fs = create_shader(d, shader_dir / "zone.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};

    const VkVertexInputBindingDescription vb[] = {{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                                  {1, sizeof(GpuInstance), VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription attrs[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GpuVertex, color)},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv2)},
        {5, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GpuVertex, color2)},
        {6, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, point_light)},
        {7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, sway)},
        {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuInstance, row)},
        {9, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuInstance, row) + sizeof(glm::vec4)},
        {10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuInstance, row) + 2 * sizeof(glm::vec4)},
        {11, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuInstance, normal_row)},
        {12, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuInstance, normal_row) + sizeof(glm::vec4)},
        {13, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuInstance, normal_row) + 2 * sizeof(glm::vec4)},
        {14, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuInstance, tint)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(std::size(attrs));
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT; // dynamic: none for two-sided materials
    rs.frontFace = kEqFrontFace;
    rs.lineWidth = 1.0f;
    if (behind_) {
        // A neighbour zone (ZoneRenderOptions::behind): pushed back (the depth is reversed, so towards 0) by a few
        // steps of the depth's own precision and by its slope, so that where its surface meets the current zone's,
        // the current zone's pixels win at every angle and distance.
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = -kBehindDepthBias;
        rs.depthBiasSlopeFactor = -kBehindSlopeBias;
    }
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = vk_.scene_samples();
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 4;
    ds.pDynamicStates = dyn;
    VkFormat color_format = vk_.scene_format();
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color_format;
    ri.depthAttachmentFormat = vk_.depth_format();

    // Multisampled, the opaque pass turns the alpha test into coverage: alpha-to-coverage, and the fragment
    // shader's constant_id 0 makes its alpha the coverage (shaders/zone.frag, actor.frag, object.frag).
    const VkBool32 coverage = vk_.scene_samples() != VK_SAMPLE_COUNT_1_BIT ? VK_TRUE : VK_FALSE;
    // constant_id 1: the scene's samples, which a level of detail's fade shares out (lighting.glsl lod_fade_samples).
    struct Spec {
        VkBool32 coverage = VK_FALSE;
        std::int32_t samples = 1;
    } spec_value;
    spec_value.samples = static_cast<std::int32_t>(vk_.scene_samples());
    const VkSpecializationMapEntry spec_entries[] = {{0, offsetof(Spec, coverage), sizeof(VkBool32)},
                                                     {1, offsetof(Spec, samples), sizeof(std::int32_t)}};
    VkSpecializationInfo spec{2, spec_entries, sizeof(Spec), &spec_value};
    stages[1].pSpecializationInfo = &spec;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &ri;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDynamicState = &ds;
    info.layout = layout_;

    for (int pass = 0; pass < 3; ++pass) {
        spec_value.coverage = pass == 0 ? coverage : VK_FALSE;
        ms.alphaToCoverageEnable = spec_value.coverage;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = VK_TRUE;
        dss.depthWriteEnable = pass == 0 ? VK_TRUE : VK_FALSE;
        // Behind the current zone, a tie goes to the current zone (drawn first).
        dss.depthCompareOp = behind_ ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_GREATER_OR_EQUAL;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = kAllChannels;
        if (pass == 1) {
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.colorBlendOp = VK_BLEND_OP_ADD;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (pass == 2) {
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.colorBlendOp = VK_BLEND_OP_ADD;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;

        info.pDepthStencilState = &dss;
        info.pColorBlendState = &cb;
        vk_check(vkCreateGraphicsPipelines(d, vk_.pipeline_cache(), 1, &info, nullptr, &pipelines_[pass]),
                 "vkCreateGraphicsPipelines");
    }

    // The depth pre-pass: the same vertex shader, a fragment shader that only tests alpha, no colour writes (the
    // colour attachment stays for alpha-to-coverage), depth written. Its shader is optional: an install without it
    // draws the single pass.
    const std::filesystem::path depth_shader = shader_dir / "zone_depth.frag.spv";
    std::error_code missing;
    if (depth_prepass_ && !std::filesystem::exists(depth_shader, missing)) {
        log::warn("depth pre-pass off: no {}", depth_shader.string());
        depth_prepass_ = false;
    }
    if (depth_prepass_) {
        // Both pipelines take the opaque pass's coverage, so an edge covers the same samples in each.
        spec_value.coverage = coverage;
        ms.alphaToCoverageEnable = coverage;
        VkPipelineColorBlendAttachmentState ba{};
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = VK_TRUE;
        info.pDepthStencilState = &dss;
        info.pColorBlendState = &cb;

        VkShaderModule dfs = create_shader(d, depth_shader);
        stages[1].module = dfs;
        ba.colorWriteMask = 0;
        dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        vk_check(vkCreateGraphicsPipelines(d, vk_.pipeline_cache(), 1, &info, nullptr, &depth_pipeline_),
                 "vkCreateGraphicsPipelines (depth)");
        vkDestroyShaderModule(d, dfs, nullptr);

        // The colour pass over it: the depth is already the nearest, so test equal and write none.
        stages[1].module = fs;
        ba.colorWriteMask = kAllChannels;
        dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_EQUAL;
        vk_check(vkCreateGraphicsPipelines(d, vk_.pipeline_cache(), 1, &info, nullptr, &opaque_equal_pipeline_),
                 "vkCreateGraphicsPipelines (equal)");
    }
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
}

template <class Keep, class First>
void ZoneRenderer::draw_chunks(VkCommandBuffer cmd, const Draw& d, Counted counted, Keep&& keep, First&& first,
                               LodPick lod) {
    // The draw's box first: a draw wholly out of view tests none of its chunks. Neighbouring chunks lie end to end in
    // the index buffer (build), so a run of kept ones is one draw. Skipping chunks keeps the order of the rest, which
    // the blended draws need.
    Chunk whole;
    whole.box_min = d.box_min;
    whole.box_max = d.box_max;
    if (d.chunk_count == 0 || !keep(whole)) return;
    bool started = false;
    // A run: neighbouring chunks of the zone's own geometry (end to end in the index buffer), or of an instanced
    // model's placements (one range of indices, neighbouring instances).
    std::uint32_t run_first = 0, run_count = 0, run_instance = 0, run_instances = 0;
    auto flush = [&] {
        if (run_count == 0) return;
        if (!started) {
            first();
            started = true;
        }
        // The land round the zone from its own index buffers (bind_indices).
        vkCmdDrawIndexed(cmd, run_count, run_instances, d.distant ? run_first - d.first_index + d.distant_first : run_first,
                         0, run_instance);
        if (counted == Counted::Scene) {
            drawn_.triangles += std::size_t{run_count / 3} * run_instances;
            ++drawn_.calls;
        } else if (counted == Counted::Shadow) {
            drawn_.shadow_triangles += std::size_t{run_count / 3} * run_instances;
            ++drawn_.shadow_calls;
        }
        run_count = 0;
    };
    for (std::uint32_t i = d.first_chunk; i < d.first_chunk + d.chunk_count; ++i) {
        const Chunk& c = chunks_[i];
        if (!lod_shows(d, c, lod) || !keep(c)) continue;
        if (run_count > 0 && extends_run(c, run_first, run_count, run_instance, run_instances)) continue;
        flush();
        run_first = c.first_index;
        run_count = c.index_count;
        run_instance = c.instance;
        run_instances = 1;
    }
    flush();
}

bool ZoneRenderer::lod_shows(const Draw& d, const Chunk& c, LodPick pick) const {
    if (!d.lod.banded()) return true;
    const glm::vec3 at = c.instance < instance_origins_.size() ? instance_origins_[c.instance]
                                                               : (c.box_min + c.box_max) * 0.5f;
    return render::lod_shows(d.lod, glm::distance(at, lod_eye_), pick, lod_scale_);
}

void ZoneRenderer::bind_geometry(VkCommandBuffer cmd) const {
    const VkBuffer buffers[] = {vertex_buffer_.buffer, instance_buffer_.buffer};
    const VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
}

bool ZoneRenderer::modern_lighting() const { return scene_.lighting.params.x > 0.5f; }

std::uint64_t ZoneRenderer::begin_draw(const FrameInfo& frame, const glm::mat4& view_proj,
                                       const glm::vec3& camera_pos, const game::ViewDistance& view,
                                       double time_seconds) {
    VkCommandBuffer cmd = frame.cmd;
    bind_geometry(cmd);
    auto time_ms = static_cast<std::uint64_t>(time_seconds * 1000.0);
    scene_.view_proj = view_proj;
    scene_.fog_color = glm::vec4(view.fog_color, 1.0f);
    scene_.camera_pos = glm::vec4(camera_pos, 1.0f);
    lod_eye_ = camera_pos;
    scene_.fog = glm::vec4(view.fog_start, view.fog_end, effect_scroll_seconds(time_ms), view.fog_density);
    upload_scene(frame.frame_slot);
    const VkDescriptorSet sets[] = {set_, scene_sets_[frame.frame_slot], vk_.shadow_set()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, static_cast<std::uint32_t>(std::size(sets)),
                            sets, 0, nullptr);
    return time_ms;
}

void ZoneRenderer::bind_draw(VkCommandBuffer cmd, const Draw& d, std::uint32_t flags, std::uint64_t time_ms) {
    vkCmdSetCullMode(cmd, d.two_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
    vkCmdSetFrontFace(cmd, d.front_ccw ? opposite(kEqFrontFace) : kEqFrontFace);
    const std::size_t frame_index = texture_frame(time_ms, d.frame_delay_ms, d.frame_slots.size());
    PushConstants pc{};
    pc.tex = {d.frame_slots[frame_index], d.extra_slots[0], d.extra_slots[1], d.extra_slots[2]};
    // The heightmap tiles bind their third detail map in slot 4.
    pc.tex2 = {d.extra_slots[3], d.mode, flags, d.maps};
    pc.params = glm::vec4(d.alpha_ref, d.opacity, d.param_z, 0.0f);
    pc.slide = d.slide;
    pc.extra = d.extra;
    pc.extra2 = d.extra2;
    pc.extra3 = d.extra3;
    // A level of detail fades into its neighbours (render/lod_bands.h): its band as the scene sees it. Only the
    // classic effect is banded (a replacement model's), which leaves extra3 to it.
    if (d.lod.banded()) {
        pc.tex2.z |= kLodFading;
        pc.extra3 = glm::vec4(d.lod.near * lod_scale_, d.lod.far * lod_scale_, render::kLodFade, 0.0f);
    }
    pc.extra4 = d.extra4;
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
}

void ZoneRenderer::draw(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                        const game::ViewDistance& view, double time_seconds, ZonePhase phase) {
    if (draws_.empty()) return;
    VkCommandBuffer cmd = frame.cmd;
    const std::uint64_t time_ms = begin_draw(frame, view_proj, camera_pos, view, time_seconds);

    // The modern water draws blended after the opaque world, reading begin_translucent's copies when split. The
    // regions' detail passes belong to the ground under it: blended, but with the opaque world (after its draws).
    const bool modern = modern_lighting();
    // The equal pipeline only where draw_depth wrote this frame's depth (a caller that draws without it, the model
    // viewer, would otherwise test equal against an empty buffer and lose the world).
    const bool prepassed = depth_prepass_ && modern && depth_drawn_;
    if (phase != ZonePhase::Translucent) depth_drawn_ = false;
    auto modern_water = [&](const Draw& d) { return modern && d.liquid == Liquid::Water; };
    auto late = [&](const Draw& d) {
        return (d.pass != Pass::Opaque && d.mode != kWldDetail) || modern_water(d);
    };
    std::vector<const Draw*> order;
    order.reserve(draws_.size());
    if (phase == ZonePhase::Translucent)
        for (const auto& d : draws_)
            if (modern_water(d)) order.push_back(&d);
    for (const auto& d : draws_) {
        if (d.flame && hide_flames_) continue;
        if (phase == ZonePhase::Opaque && late(d)) continue;
        if (phase == ZonePhase::Translucent && (!late(d) || modern_water(d))) continue;
        order.push_back(&d);
    }
    const std::uint32_t copies = phase == ZonePhase::Translucent ? kWaterCopies : 0u;
    // Only the chunks in the camera's view; a draw none of whose chunks shows sets up nothing.
    const Frustum frustum = frustum_from(view_proj);

    VkPipeline bound = VK_NULL_HANDLE;
    bool distant_bound = false;
    for (const Draw* draw : order) {
        const Draw& d = *draw;
        if (d.distant && !show_distant_) continue;
        auto seen = [&](const Chunk& c) { return box_in_frustum(frustum, Box{c.box_min, c.box_max}); };
        draw_chunks(cmd, d, Counted::Scene, seen, [&] {
            if (d.distant != distant_bound) {
                bind_indices(cmd, frame.frame_slot, d.distant);
                distant_bound = d.distant;
            }
            int pass = modern_water(d) ? static_cast<int>(Pass::Blended) : static_cast<int>(d.pass);
            VkPipeline pipeline = prepassed && in_depth_prepass(d) ? opaque_equal_pipeline_ : pipelines_[pass];
            if (pipeline != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                bound = pipeline;
            }
            std::uint32_t flags = (lighting_ == VertexLighting::None || !d.lit ? kUnlit : 0u) |
                                  (d.fog ? 0u : kNoFog) |
                                  (d.multipass && lighting_ != VertexLighting::None ? kMultiPass : 0u) |
                                  (d.liquid == Liquid::Water ? kWaterSurface | copies : 0u) |
                                  (d.liquid == Liquid::Lava ? kLavaSurface : 0u) | d.surface;
            bind_draw(cmd, d, flags, time_ms);
        }, kSceneLod);
    }
}

void ZoneRenderer::draw_depth(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                              const game::ViewDistance& view, double time_seconds) {
    // The modern lighting's: the classic one has no per-pixel shading to save.
    if (!depth_prepass_ || !modern_lighting() || draws_.empty()) return;
    VkCommandBuffer cmd = frame.cmd;
    const std::uint64_t time_ms = begin_draw(frame, view_proj, camera_pos, view, time_seconds);
    const Frustum frustum = frustum_from(view_proj);
    bool bound = false, distant_bound = false;
    for (const auto& d : draws_) {
        if (d.flame && hide_flames_) continue;
        if (!in_depth_prepass(d)) continue;
        // The colour pass tests equal: it must draw the same.
        if (d.distant && !show_distant_) continue;
        auto seen = [&](const Chunk& c) { return box_in_frustum(frustum, Box{c.box_min, c.box_max}); };
        draw_chunks(cmd, d, Counted::None, seen, [&] {
            if (!bound) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_pipeline_);
            bound = true;
            if (d.distant != distant_bound) {
                bind_indices(cmd, frame.frame_slot, d.distant);
                distant_bound = d.distant;
            }
            bind_draw(cmd, d, kDepthOnlyFlags, time_ms);
        }, kSceneLod);
    }
    depth_drawn_ = true;
}

void ZoneRenderer::upload_scene(std::uint32_t frame_slot) {
    std::memcpy(scene_mapped_[frame_slot], &scene_, sizeof(scene_));
    vmaFlushAllocation(vk_.allocator(), scene_buffers_[frame_slot].allocation, 0, sizeof(scene_));
}

void ZoneRenderer::draw_backdrop(const FrameInfo& frame, const glm::mat4& view_proj) {
    if (!underwater_pipeline_) return;
    VkCommandBuffer cmd = frame.cmd;
    scene_.view_proj = view_proj;
    upload_scene(frame.frame_slot);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, underwater_pipeline_);
    VkDescriptorSet sets[3] = {set_, scene_sets_[frame.frame_slot], vk_.shadow_set()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, sets, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void ZoneRenderer::create_underwater_pipeline(const std::filesystem::path& shader_dir) {
    VkDevice d = vk_.device();
    VkShaderModule vs = create_shader(d, shader_dir / "underwater.vert.spv");
    VkShaderModule fs = create_shader(d, shader_dir / "underwater.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = vk_.scene_samples();
    // Behind everything: drawn first, no depth test or write.
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                        VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkFormat color_format = vk_.scene_format();
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color_format;
    ri.depthAttachmentFormat = vk_.depth_format();
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &ri;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &dss;
    info.pColorBlendState = &cb;
    info.pDynamicState = &ds;
    info.layout = layout_;
    vk_check(vkCreateGraphicsPipelines(d, vk_.pipeline_cache(), 1, &info, nullptr, &underwater_pipeline_),
             "vkCreateGraphicsPipelines (underwater)");
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
}

void ZoneRenderer::set_distant_hidden(std::vector<std::int64_t> cells) {
    std::sort(cells.begin(), cells.end());
    distant_shown_ = distant_indices_;
    if (!cells.empty())
        for (std::size_t t = 0; t < distant_centres_.size(); ++t)
            if (std::binary_search(cells.begin(), cells.end(), horizon_cell(distant_centres_[t])))
                distant_shown_[t * 3 + 1] = distant_shown_[t * 3 + 2] = distant_shown_[t * 3];
    distant_dirty_ = (1u << VkContext::kFramesInFlight) - 1u;
}

void ZoneRenderer::bind_indices(VkCommandBuffer cmd, std::uint32_t frame_slot, bool distant) {
    if (!distant || !distant_buffers_[frame_slot].buffer) {
        vkCmdBindIndexBuffer(cmd, index_buffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
        return;
    }
    if (distant_dirty_ & (1u << frame_slot)) {
        const std::size_t bytes = distant_shown_.size() * sizeof(std::uint32_t);
        std::memcpy(distant_mapped_[frame_slot], distant_shown_.data(), bytes);
        vmaFlushAllocation(vk_.allocator(), distant_buffers_[frame_slot].allocation, 0, bytes);
        distant_dirty_ &= ~(1u << frame_slot);
    }
    vkCmdBindIndexBuffer(cmd, distant_buffers_[frame_slot].buffer, 0, VK_INDEX_TYPE_UINT32);
}

bool ZoneRenderer::map_moves(std::uint32_t layer) const {
    // zone_shadow.vert moves only the swaying vertices, and only with wind (lighting.glsl wind_offset).
    if (scene_.lighting.wind.z <= 0.0f) return false;
    const Box area = cover_box(scene_.lighting.cover, scene_.lighting.cover_params);
    auto moves = [&](const Draw& d) {
        if (!boxes_overlap(area, Box{d.box_min, d.box_max})) return false;
        for (std::uint32_t i = d.first_chunk; i < d.first_chunk + d.chunk_count; ++i)
            if (chunks_[i].sways && boxes_overlap(area, Box{chunks_[i].box_min, chunks_[i].box_max})) return true;
        return false;
    };
    if (layer == kCoverLayer) return moves(solid_);
    return std::any_of(draws_.begin(), draws_.end(),
                       [&](const Draw& d) { return d.liquid == Liquid::Water && moves(d); });
}

bool ZoneRenderer::has_water() const {
    return std::any_of(draws_.begin(), draws_.end(), [](const Draw& d) { return d.liquid == Liquid::Water; });
}

void ZoneRenderer::draw_shadow(const FrameInfo& frame, std::uint32_t cascade) {
    if (draws_.empty() || !shadow_pipeline_) return;
    VkCommandBuffer cmd = frame.cmd;
    // The cascades in this frame's block; draw() writes it again later in the frame with the same cascades.
    upload_scene(frame.frame_slot);
    bind_geometry(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_);
    VkDescriptorSet sets[3] = {set_, scene_sets_[frame.frame_slot], vk_.shadow_set()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, sets, 0, nullptr);
    // Only the chunks in the layer's view. A cascade's box (render/shadows.h) already reaches kShadowCasterReach
    // towards the light past its slice, for the casters out there, and the pipeline clips at its ends (no depth
    // clamp), so its clip volume is exactly what can land in the layer. The cover and water maps see a square of
    // the zone round the camera, through their depth range (render/cover_map.h).
    const bool map = cascade >= kShadowCascades;
    const Frustum frustum = map ? Frustum{} : frustum_from(scene_.lighting.shadow_view_proj[cascade]);
    const Box area = map ? cover_box(scene_.lighting.cover, scene_.lighting.cover_params) : Box{};
    auto seen = [&](const Chunk& c) {
        Box b{c.box_min, c.box_max};
        return map ? boxes_overlap(area, b) : box_in_frustum(frustum, b);
    };
    PushConstants pc{};
    pc.tex2.w = cascade;
    static_assert(kShadowLodLevel.size() == kShadowCascades);
    const std::uint8_t min_level = cascade < kShadowCascades ? kShadowLodLevel[cascade] : std::uint8_t{0};
    // The solid casters first, a cell at a time whatever their material: they need no texture.
    if (cascade != kWaterLayer)
        draw_chunks(cmd, solid_, Counted::Shadow, seen, [&] {
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                               &pc);
        });
    for (const auto& d : draws_) {
        if (d.distant) continue; // far past the shadows' reach and the maps' square
        // The water map: the water surfaces alone.
        if (cascade == kWaterLayer) {
            if (d.liquid != Liquid::Water) continue;
        } else {
            if (d.pass != Pass::Opaque || d.flame) continue; // water, glass, glows and flames cast nothing
            if (casts_untextured(d)) continue;               // in solid_
            // The cover map keeps to solid things: leaves and grass are crossed cards, which from above would
            // shelter thin lines of ground.
            if (cascade == kCoverLayer) continue;
        }
        draw_chunks(cmd, d, Counted::Shadow, seen, [&] {
            pc.tex.x = d.frame_slots[0];
            pc.params = glm::vec4(d.alpha_ref, 1.0f, 0.0f, 0.0f);
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                               &pc);
        }, LodPick{min_level, false});
    }
}

void ZoneRenderer::point_reach(const glm::vec3& light, float radius, PointReach& out) const {
    out.light = light;
    out.radius = radius;
    out.chunks.clear();
    auto in_reach = [&](const glm::vec3& lo, const glm::vec3& hi) {
        glm::vec3 nearest = glm::clamp(light, lo, hi);
        return glm::dot(nearest - light, nearest - light) <= radius * radius;
    };
    // The solid casters a cell at a time (no texture), then the alpha-tested ones by material: draw_point_shadow's
    // order.
    auto add = [&](const Draw& d) {
        if (d.chunk_count == 0 || !in_reach(d.box_min, d.box_max)) return;
        for (std::uint32_t i = d.first_chunk; i < d.first_chunk + d.chunk_count; ++i)
            if (in_reach(chunks_[i].box_min, chunks_[i].box_max)) out.chunks.push_back(i);
    };
    add(solid_);
    for (const Draw& d : draws_)
        if (d.pass == Pass::Opaque && !d.flame && !d.distant && !casts_untextured(d)) add(d);
}

void ZoneRenderer::draw_point_shadow(const FrameInfo& frame, const PointReach& reach, std::uint32_t face) {
    if (reach.chunks.empty() || !point_shadow_pipeline_) return;
    VkCommandBuffer cmd = frame.cmd;
    bind_geometry(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, point_shadow_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    PushConstants pc{};
    pc.tex2.w = face;
    pc.slide = glm::vec4(reach.light, reach.radius);
    pc.extra = glm::vec4(kPointShadowFaceScale, kPointShadowNear, 0.0f, 0.0f);
    // The chunks in the light's reach on this face's side of it; a run of neighbouring ones of one draw in one call
    // (as draw_chunks), its texture pushed when the draw changes.
    std::uint32_t pushed = 0, run_draw = 0, run_first = 0, run_count = 0, run_instance = 0, run_instances = 0;
    bool any_pushed = false;
    auto flush = [&] {
        if (run_count == 0) return;
        if (!any_pushed || pushed != run_draw) {
            const bool solid = run_draw == std::numeric_limits<std::uint32_t>::max();
            pc.tex.x = solid ? 0u : draws_[run_draw].frame_slots[0];
            pc.params = glm::vec4(solid ? 0.0f : draws_[run_draw].alpha_ref, 1.0f, 0.0f, 0.0f);
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                               &pc);
            pushed = run_draw;
            any_pushed = true;
        }
        vkCmdDrawIndexed(cmd, run_count, run_instances, run_first, 0, run_instance);
        drawn_.shadow_triangles += std::size_t{run_count / 3} * run_instances;
        ++drawn_.shadow_calls;
        run_count = 0;
    };
    for (std::uint32_t i : reach.chunks) {
        const Chunk& c = chunks_[i];
        if (!box_reaches_face(reach.light, face, c.box_min, c.box_max)) continue;
        if (c.draw < draws_.size() && !lod_shows(draws_[c.draw], c, kPointShadowLod)) continue;
        if (run_count > 0 && c.draw == run_draw && extends_run(c, run_first, run_count, run_instance, run_instances))
            continue;
        flush();
        run_draw = c.draw;
        run_first = c.first_index;
        run_count = c.index_count;
        run_instance = c.instance;
        run_instances = 1;
    }
    flush();
}

void ZoneRenderer::create_shadow_pipeline(const std::filesystem::path& shader_dir) {
    create_shadow_pipeline(shader_dir, false);
    create_shadow_pipeline(shader_dir, true);
}

void ZoneRenderer::create_shadow_pipeline(const std::filesystem::path& shader_dir, bool point) {
    VkDevice d = vk_.device();
    VkShaderModule vs = create_shader(d, shader_dir / (point ? "zone_point_shadow.vert.spv" : "zone_shadow.vert.spv"));
    VkShaderModule fs = create_shader(d, shader_dir / (point ? "zone_point_shadow.frag.spv" : "zone_shadow.frag.spv"));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    const VkVertexInputBindingDescription vb[] = {{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                                  {1, sizeof(GpuInstance), VK_VERTEX_INPUT_RATE_INSTANCE}};
    // The lights' cubes are drawn once: no wind (the sway and its scale, the last two).
    VkVertexInputAttributeDescription attrs[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, position)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv)},
        {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuInstance, row)},
        {9, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuInstance, row) + sizeof(glm::vec4)},
        {10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuInstance, row) + 2 * sizeof(glm::vec4)},
        {7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, sway)},
        {11, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuInstance, normal_row)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = point ? 5 : 7;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // thin walls and cards shadow from either side
    rs.depthBiasEnable = point ? VK_FALSE : VK_TRUE; // VkContext::begin_shadow_cascade sets it; the cubes need none
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = VK_TRUE;
    dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = point ? 2 : 3;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.depthAttachmentFormat = vk_.shadow_format();
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &ri;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &dss;
    info.pColorBlendState = &cb;
    info.pDynamicState = &ds;
    info.layout = layout_;
    vk_check(vkCreateGraphicsPipelines(d, vk_.pipeline_cache(), 1, &info, nullptr,
                                       point ? &point_shadow_pipeline_ : &shadow_pipeline_),
             "vkCreateGraphicsPipelines (zone shadow)");
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
}

float effect_scroll_seconds(std::uint64_t scene_ms) {
    // CRender::SetEffectSceneConstants @0x10088c70: a_fTime = (T mod 100000)
    // x 0.001 s; the effects' preshader takes k = fmod(a_fTime, 100).
    float a_ftime = static_cast<float>(scene_ms % 100000u) * 0.001f;
    return std::fmod(a_ftime, 100.0f);
}

} // namespace hailstone::render
