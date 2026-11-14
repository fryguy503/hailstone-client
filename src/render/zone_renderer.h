#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "render/gpu.h"
#include "render/horizon.h"
#include "render/scene_lighting.h"
#include "render/vk_context.h"

namespace hailstone { class GameDir; }
namespace hailstone::assets {
struct Zone;
struct Mesh;
}
namespace hailstone::game {
struct ViewDistance;
struct SceneLight;
}

namespace hailstone::render {

// How zone geometry is lit: the original's region vertex shader
// (game/scene_light.h) or full-bright (a debugging view).
enum class VertexLighting : std::uint32_t { Classic = 0, None = 1 };

struct ZoneRenderOptions {
    // eqclient.ini [Defaults] MultiPassLighting (render::read_multipass_lighting):
    // the zone's point lights shade zone geometry whose effect has a
    // multi-pass (MPL) triple. Off draws the single-pass path, where the
    // region shaders' point lights are zero.
    bool multipass_lighting = false;

    // A neighbouring zone drawn past a zone line (app/neighbour_zones.h): its geometry moved by `offset` into the
    // current zone's coordinates (after its own regions sorted out its water), then cut to the half-spaces in `keep`
    // (xyz . p + w >= 0 for every plane, p after the move); triangles across a plane are split along it.
    glm::vec3 offset{0.0f};
    std::vector<glm::vec4> keep;
    // The parts of a triangle (after the move) to keep, as convex polygons of barycentric weights over its corners,
    // or nullopt to keep it whole: a neighbour's ground goes where the current zone has its own
    // (game::split_off_ground).
    std::function<std::optional<std::vector<std::vector<glm::vec3>>>(const glm::vec3&, const glm::vec3&,
                                                                     const glm::vec3&)>
        split;
    // Drawn behind the current zone, which is drawn first: a neighbour's fragments lose ties and near-ties with what
    // the current zone already drew (a strict depth test and a depth bias pushing them back).
    bool behind = false;
    // Textures at most this many texels across (0: as loaded): the larger levels of their mip chains are dropped.
    std::uint32_t max_texture_size = 0;
    // The classic regions' detail passes, and a texture pack's normal and roughness maps.
    bool details = true;
    bool material_maps = true;
    // Asked between meshes and materials while preparing: true stops it, and prepare() returns null. Unset: never.
    std::function<bool()> cancelled;
};

// CRender::ReadIniRenderSettings @0x10098350 and CDisplay::InitWorld: true
// when eqclient.ini [Defaults] MultiPassLighting starts with t, T or 1. The
// original also needs a ps_1_1 card with a stencil depth format, which every
// Vulkan device here is taken to have.
bool multipass_lighting_setting(std::string_view value);
// The options from the game directory's eqclient.ini.
ZoneRenderOptions zone_render_options(const GameDir& game);

// The liquid surfaces of a classic zone's terrain, which the modern lighting draws as water or lava: per terrain mesh,
// per triangle (index / 3), 0 none, kLiquidWater or kLiquidLava. A level, passable triangle (of a plain material)
// with a liquid region just under it and not just over it; and one of the same material at the same height as such a
// surface where a dry region cuts into the liquid (a zone line's region over the sea, the strip by a zone's edge).
inline constexpr std::uint8_t kLiquidWater = 1, kLiquidLava = 2;
std::vector<std::vector<std::uint8_t>> classic_liquid_triangles(const assets::Zone& zone);

// The EQG effects' scroll time k for a scene time in ms: a_fTime =
// (T mod 100000) x 0.001 s (CRender::SetEffectSceneConstants @0x10088c70),
// k = fmod(a_fTime, 100). A slide offset is k x e_fSlide texture units.
float effect_scroll_seconds(std::uint64_t scene_ms);

struct ZoneRenderStats {
    std::size_t triangles = 0;
    std::size_t draws = 0;
    std::size_t textures = 0;
    std::size_t missing_textures = 0;
    std::size_t chunks = 0; // the boxes the passes cull by
};

// What the zone's passes drew since the last ZoneRenderer::take_drawn: triangles and draw calls into the scene (draw)
// and into the shadow map's layers (draw_shadow, draw_point_shadow); the culling's measure.
struct ZoneDrawn {
    std::size_t triangles = 0;
    std::size_t calls = 0;
    std::size_t shadow_triangles = 0;
    std::size_t shadow_calls = 0;
};

// Which of the zone's draws a draw() call makes. The modern lighting splits the scene around
// VkContext::begin_translucent: Opaque before it (without the water), Translucent after (the water first, reading
// the copies, then the blended and additive draws). All draws everything in one go, the water without the copies.
enum class ZonePhase : std::uint8_t { All, Opaque, Translucent };

// Draws a zone's static geometry: terrain plus every placed object, merged into
// one vertex and index buffer and grouped into one draw per material. Each draw
// is cut into chunks with a bounding box, and every pass draws only the chunks
// its view can reach (render/culling.h): the camera's frustum, the sun cascades'
// boxes, the cover and water maps' square, the lights' reach.
class ZoneRenderer {
public:
    // The CPU half of the build (geometry, chunks, decoded textures with their mip chains): ZoneRenderer::prepare,
    // on any thread; the constructor taking it uploads it.
    struct Prepared;
    struct PreparedDeleter { void operator()(Prepared* p) const; };
    using PreparedPtr = std::unique_ptr<Prepared, PreparedDeleter>;
    // `max_images`: the device's sampled image limit (VkContext::caps().max_sampled_images).
    static PreparedPtr prepare(const assets::Zone& zone, const ZoneRenderOptions& options, std::uint32_t max_images);
    // The prepared data's size in bytes (vertices, indices, textures as uploaded), for the logs.
    static std::size_t prepared_bytes(const Prepared& prepared);

    ZoneRenderer(VkContext& vk, const assets::Zone& zone, const std::filesystem::path& shader_dir,
                 const ZoneRenderOptions& options = {});
    ZoneRenderer(VkContext& vk, PreparedPtr prepared, const std::filesystem::path& shader_dir);
    ~ZoneRenderer();
    ZoneRenderer(const ZoneRenderer&) = delete;
    ZoneRenderer& operator=(const ZoneRenderer&) = delete;

    // The chunks in `view_proj`'s frustum.
    void draw(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
              const game::ViewDistance& view, double time_seconds, ZonePhase phase = ZonePhase::All);
    // The opaque and alpha-tested geometry into the scene's depth buffer, before draw's opaque phase
    // (SceneOptions::depth_prepass): the colour pass then tests equal and writes no depth.
    void draw_depth(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                    const game::ViewDistance& view, double time_seconds);
    // The opaque and alpha-tested geometry into shadow cascade `cascade`, between VkContext::begin_shadow_cascade and
    // end_shadow_cascade; the cascades come from the last set_lighting. With kCoverLayer, the cover map
    // (render/cover_map.h): the opaque geometry only; with kWaterLayer, the water map: the water surfaces only. Only
    // the chunks in the layer's view, from the same set_lighting (the cascade's matrix, the maps' square).
    void draw_shadow(const FrameInfo& frame, std::uint32_t cascade);
    // The solid geometry within `radius` of `light`, found once for the light's six faces (point_reach: the chunks in
    // its sphere, in drawing order); then into face `face` of its shadow cube (render/point_shadows.h) those on the
    // face's side of it, between VkContext::begin_point_shadow and end_point_shadow; the plants at rest.
    struct PointReach {
        glm::vec3 light{0.0f};
        float radius = 0.0f;
        std::vector<std::uint32_t> chunks; // in chunks_
    };
    void point_reach(const glm::vec3& light, float radius, PointReach& out) const;
    void draw_point_shadow(const FrameInfo& frame, const PointReach& reach, std::uint32_t face);
    // Whether the zone has water surfaces (the water map is worth drawing).
    bool has_water() const;
    // Whether what draw_shadow draws into the cover or water map (kCoverLayer, kWaterLayer) moves from frame to frame
    // in the map's view, from the last set_lighting: the plants' swaying parts, with wind.
    bool map_moves(std::uint32_t layer) const;
    // A number no other zone renderer made in this run has (what the cover and water maps show).
    std::uint64_t id() const { return id_; }
    // The modern lighting's backdrop where no sky is drawn, from the last set_lighting (shaders/underwater.frag): the
    // haze as it is infinitely far away, its glow towards the sun too; with the camera under water
    // (render::set_underwater), the open water, its colour and light shafts. Draw it first in the scene.
    void draw_backdrop(const FrameInfo& frame, const glm::mat4& view_proj);
    // The land round the zone (render/horizon.h), built from its terrain while preparing it (not for a neighbour
    // zone): what was measured, and whether draw and draw_depth draw it (off until set; the modern lighting outdoors).
    const HorizonLand& horizon() const { return horizon_; }
    void set_distant(bool show) { show_distant_ = show; }
    // Where a neighbour zone is drawn, the land round the zone is not: its triangles whose middle lies in one of these
    // squares (render::horizon_cell keys, a neighbour's footprint) are left out. Empty: none.
    void set_distant_hidden(std::vector<std::int64_t> cells);
    // A neighbour zone's (ZoneRenderOptions::behind): the squares its opaque geometry covers (render::horizon_footprint).
    const std::vector<std::int64_t>& footprint() const { return footprint_; }

    const ZoneRenderStats& stats() const { return stats_; }
    // The counts since the last call; they start again from zero.
    ZoneDrawn take_drawn() { return std::exchange(drawn_, ZoneDrawn{}); }
    void set_vertex_lighting(VertexLighting mode) { lighting_ = mode; }
    // The frame's lighting constants (game::compute_scene_light).
    void set_scene_light(const game::SceneLight& light);
    // The modern mode's switch, daylight and dynamic lights (render/scene_lighting.h); classic: params.x 0.
    void set_lighting(const SceneLighting& lighting);
    // Leaves the placed objects' flame meshes out of draw (the modern lighting draws its own: render/fire_renderer.h).
    void set_hide_flames(bool hide) { hide_flames_ = hide; }

private:
    enum class Pass : std::uint8_t { Opaque, Blended, Additive };
    // A surface on a water or lava region's top (classic zones, by the regions under and over each triangle), or
    // an EQG water effect: the modern lighting draws them as liquids.
    enum class Liquid : std::uint8_t { None, Water, Lava };

    struct Draw {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::vector<std::uint32_t> frame_slots; // texture slots; more than one = animated
        std::uint32_t frame_delay_ms = 0;
        std::array<std::uint32_t, 4> extra_slots{}; // the effect's other textures (shader slots 1..4)
        std::uint32_t mode = 0;                    // zone.frag's effect
        std::uint32_t maps = 0; // a texture pack's normal map slot | roughness map slot << 16 (kClassic; 0 = none)
        Pass pass = Pass::Opaque;
        float alpha_ref = 0.0f;
        float opacity = 1.0f;
        bool two_sided = false;
        bool front_ccw = false;
        bool fog = true;
        bool lit = true;
        bool multipass = false; // add the point lights (MPL path)
        Liquid liquid = Liquid::None;
        bool flame = false; // a placed object's flame mesh: set_hide_flames leaves it out
        bool distant = false; // the land round the zone: no shadows; set_distant, set_distant_hidden
        std::uint32_t distant_first = 0; // a distant draw's first index in the land's own index buffers
        glm::vec4 slide{0.0f}, extra{0.0f}, extra2{0.0f}, extra3{0.0f}, extra4{0.0f};
        float param_z = 0.0f;
        std::uint32_t surface = 0; // the modern lighting's surface maps (kNormalMap, kSpecular, kReflective)
        std::uint32_t first_chunk = 0; // its chunks in chunks_
        std::uint32_t chunk_count = 0;
        glm::vec3 box_min{0.0f}, box_max{0.0f}; // round all of them: a draw wholly out of view tests no chunk
        // A replacement model's level of detail (assets::MeshPart::lod_near, lod_far): its chunks show only from
        // lod_near to lod_far units from the camera (lod_shows). lod_far 0: always.
        float lod_near = 0.0f, lod_far = 0.0f;
    };

    // Whether every shadow pass but the water map draws `d` without its texture: solid (opaque, not alpha-tested), not
    // a flame. These go into solid_.
    // (A level of detail is drawn on its own, so that its shadow changes level with it.)
    static bool casts_untextured(const Draw& d) {
        return d.pass == Pass::Opaque && !d.flame && !d.distant && !(d.alpha_ref > 0.0f) && d.lod_far <= 0.0f;
    }
    // Whether a chunk of `d` shows at its distance from lod_eye_: the distance from the camera to the chunk's box's
    // centre within the draw's band. A banded draw's chunks are cells of whole placements, so a tree changes level at
    // once, and each cell shows one level of it.
    bool lod_shows(const Draw& d, const glm::vec3& box_min, const glm::vec3& box_max) const {
        if (d.lod_far <= 0.0f) return true;
        const float dist = glm::distance((box_min + box_max) * 0.5f, lod_eye_);
        return dist >= d.lod_near && dist < d.lod_far;
    }
    // The camera's position the levels of detail are chosen by: the last begin_draw's (the shadow passes, drawn
    // before it in a frame, take the frame before's).
    glm::vec3 lod_eye_{0.0f};
    // Whether the depth pre-pass draws `d`, and the colour pass then tests it equal: the opaque pass's draws but the
    // water, which the modern lighting draws blended after VkContext::begin_translucent.
    static bool in_depth_prepass(const Draw& d) { return d.pass == Pass::Opaque && d.liquid != Liquid::Water; }
    // The modern lighting (set_lighting), not the classic.
    bool modern_lighting() const;

    void upload(Prepared& prepared);
    void create_descriptors();
    void create_pipelines(const std::filesystem::path& shader_dir);
    void create_shadow_pipeline(const std::filesystem::path& shader_dir);
    void create_shadow_pipeline(const std::filesystem::path& shader_dir, bool point);
    void create_underwater_pipeline(const std::filesystem::path& shader_dir);
    void upload_scene(std::uint32_t frame_slot);
    // What draw and draw_depth share: the geometry and the sets bound, the frame's scene uploaded. Returns the time
    // in milliseconds.
    std::uint64_t begin_draw(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                             const game::ViewDistance& view, double time_seconds);
    // Draw `d`'s own state: its culling and its push constants, with zone.frag's `flags`.
    void bind_draw(VkCommandBuffer cmd, const Draw& d, std::uint32_t flags, std::uint64_t time_ms);

    VkContext& vk_;
    std::uint64_t id_ = 0;
    ZoneRenderStats stats_;
    VertexLighting lighting_ = VertexLighting::Classic;
    bool hide_flames_ = false;
    struct SceneUniforms { // std140, zone.vert's Scene block
        glm::mat4 view_proj{1.0f};
        glm::vec4 fog_color{0.0f};
        glm::vec4 fog{0.0f}; // start, end, k (seconds of a_fTime), density
        glm::vec4 camera_pos{0.0f};
        SceneLighting lighting{}; // render/scene_lighting.h; the classic fields from set_scene_light
    };
    SceneUniforms scene_{};
    std::array<Buffer, VkContext::kFramesInFlight> scene_buffers_{};
    std::array<void*, VkContext::kFramesInFlight> scene_mapped_{};
    VkDescriptorSetLayout scene_layout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, VkContext::kFramesInFlight> scene_sets_{};
    std::vector<Draw> draws_;
    // Each draw's triangles in pieces of the zone with their bounding box (build), in draw order and, within a draw,
    // in index order: the culling's unit. The opaque draws' are by 128-unit cell; the blended and additive draws keep
    // their triangles' order, cut into runs that stay within a cell's size. The boxes take in how far the plants
    // sway.
    struct Chunk {
        std::uint32_t draw = 0;
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        glm::vec3 box_min{0.0f}, box_max{0.0f};
        bool sways = false; // some of its vertices sway in the wind
    };
    std::vector<Chunk> chunks_;
    // The casts_untextured draws merged for the shadow passes, one chunk a cell (render/shadow_chunks.h): its chunks
    // follow the draws' in chunks_, its indices the draws' in the index buffer. Only its chunk fields are set.
    Draw solid_;
    // Records draw `d`'s chunks that `keep` (a predicate on a Chunk) passes, a run of neighbouring chunks in one
    // vkCmdDrawIndexed; `first` runs once before the first, so a draw with nothing to show sets up nothing.
    // `counted`: where its triangles and calls go in drawn_. The depth pre-pass counts none: the colour pass draws
    // the same triangles.
    enum class Counted : std::uint8_t { Scene, Shadow, None };
    template <class Keep, class First>
    void draw_chunks(VkCommandBuffer cmd, const Draw& d, Counted counted, Keep&& keep, First&& first);
    ZoneDrawn drawn_;
    // The zone's lights for the modern lighting's per-pixel pass (render/light_grid.h): the lights with the grid's
    // header, the cells, the cells' light numbers (zone.frag's set 1, bindings 1 to 3).
    std::array<Buffer, 3> light_grid_buffers_{};
    Buffer vertex_buffer_;
    Buffer index_buffer_;
    std::vector<Texture> textures_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[3]{};
    // SceneOptions::depth_prepass: the depth-only pipeline, and the opaque colour pipeline that tests equal and writes
    // no depth (the pre-pass wrote it).
    bool depth_prepass_ = false;
    bool depth_drawn_ = false; // draw_depth wrote this frame's depth: the next opaque draw may test equal
    VkPipeline depth_pipeline_ = VK_NULL_HANDLE;
    VkPipeline opaque_equal_pipeline_ = VK_NULL_HANDLE;
    bool behind_ = false; // ZoneRenderOptions::behind
    VkPipeline shadow_pipeline_ = VK_NULL_HANDLE;
    VkPipeline point_shadow_pipeline_ = VK_NULL_HANDLE;
    VkPipeline underwater_pipeline_ = VK_NULL_HANDLE;
    HorizonLand horizon_;
    // The land round the zone's chunks are cells this size.
    static constexpr float kDistantChunk = 256.0f;
    bool show_distant_ = false;
    std::vector<std::int64_t> footprint_; // a neighbour's
    // The land's triangles are drawn from index buffers of their own, one per frame in flight, holding
    // distant_shown_: its triangles as built (distant_indices_, in the distant draws' order) with those
    // set_distant_hidden hides collapsed to a point. distant_dirty_: the frame slots whose buffer is behind it (bits).
    std::vector<std::uint32_t> distant_indices_, distant_shown_;
    std::vector<glm::vec2> distant_centres_; // each triangle's middle
    std::array<Buffer, VkContext::kFramesInFlight> distant_buffers_{};
    std::array<void*, VkContext::kFramesInFlight> distant_mapped_{};
    std::uint32_t distant_dirty_ = 0;
    // Binds the land's index buffer for `frame_slot` (brought up to date first), or the zone's.
    void bind_indices(VkCommandBuffer cmd, std::uint32_t frame_slot, bool distant);
};

} // namespace hailstone::render
