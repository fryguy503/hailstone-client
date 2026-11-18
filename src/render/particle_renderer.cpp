#include "render/particle_renderer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "assets/texture.h"
#include "assets/texture_overrides.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "core/worker_pool.h"
#include "game/particles.h"
#include "game/sine_table.h"
#include "render/culling.h"
#include "render/scene_lighting.h"
#include "render/vk_util.h"

namespace hailstone::render {

namespace {

// particle.vert / particle.frag's push block.
constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
struct Push {
    glm::mat4 view_proj;
    glm::vec4 screen; // SceneLighting::screen: the depth terms, 1 / the target's size (modern)
    glm::vec4 params; // x: 1 modern
};

struct GpuVertex {
    glm::vec3 position;
    glm::vec2 uv;
    glm::vec4 color;
    std::uint32_t texture; // bit 31: additive (modern)
    glm::vec4 shade;       // rgb the light it is drawn in (linear), w how deep it fades into the scene (modern)
    glm::vec4 haze;        // rgb the haze's colour in front of it (linear), w how much of it shows through
};
constexpr std::uint32_t kAdditiveBit = 0x80000000u;

glm::vec3 to_linear(const glm::vec3& c) {
    auto one = [](float v) {
        v = std::max(v, 0.0f);
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return {one(c.r), one(c.g), one(c.b)};
}

// shaders/lighting.glsl light_pool.
float light_pool(float d2, float r) {
    float window = 1.0f - std::min(d2 / (r * r), 1.0f);
    float core = std::clamp(r * 0.2f, 8.0f, 16.0f);
    return window * core * core / (core * core + d2);
}

// Additive smoke (the Luclin chimneys' CSMOKE.DDS): light the smoke scatters, not light of its own; the modern
// lighting lights it as the alpha-blended smoke, so it does not glow at night.
bool scatters_light(std::string_view texture) {
    // Case-blind search without a lowered copy (asked once per lit definition each frame).
    constexpr std::string_view kSmoke = "smoke";
    const auto it = std::search(texture.begin(), texture.end(), kSmoke.begin(), kSmoke.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
    });
    return it != texture.end();
}

VkShaderModule load_shader(VkDevice device, const std::filesystem::path& path) {
    return create_shader_module(device, path);
}

} // namespace

bool particle_corners(const game::Particle& p, std::uint32_t billboard, const glm::vec3& right, const glm::vec3& up,
                      std::array<glm::vec3, 4>& corners) {
    if (p.hidden) return false;
    const float c = game::cmath_cos(p.rotation), sn = game::cmath_sin(p.rotation);
    const glm::vec3& at = p.position;
    if (billboard == 1) {
        // A streak along the particle's axis (0x10074b48, corners 0x100750d2): its ends half its length either side,
        // widened on the screen's x only by the half width x (dy cos + dx sin) / the streak's screen length.
        const glm::vec3 half = p.axis * (0.5f * p.length);
        const float ar = glm::dot(p.axis, right), au = glm::dot(p.axis, up);
        const float len = std::sqrt(ar * ar + au * au) * std::max(p.ramp_width, 1e-3f);
        if (!(len > 1e-6f)) return false;
        const float hw = 0.5f * p.width * p.size_factor * p.ramp_height;
        const glm::vec3 o = right * (hw * (-au * c + ar * sn) / len);
        corners = {at + half - o, at + half + o, at - half + o, at - half - o};
        return true;
    }
    const float hw = 0.5f * p.width * p.size_factor * p.ramp_width, hh = 0.5f * p.height * p.size_factor * p.ramp_height;
    glm::vec3 ax, ay; // half extents towards the texture's right and top
    if (billboard == game::kWorldBasisBillboard) {
        const float across = glm::length(p.axes[0]), forward = glm::length(p.axes[1]);
        if (!(across > 1e-6f) || !(forward > 1e-6f) || !std::isfinite(across) || !std::isfinite(forward)) return false;
        ax = p.axes[0] * (hw / across);
        ay = p.axes[1] * (hh / forward);
    } else if (billboard == game::kTrailStrandBillboard) {
        const float forward = glm::length(p.axes[1]);
        if (!(forward > 1e-6f) || !std::isfinite(forward)) return false;
        const glm::vec3 tangent = p.axes[1] / forward;
        const glm::vec3 across = glm::cross(tangent, glm::cross(up, right));
        const float width = glm::length(across);
        if (!(width > 1e-6f) || !std::isfinite(width)) return false;
        ax = across * (hw / width);
        ay = tangent * hh;
    } else if (billboard == 2) {
        // Flat on the world's horizontal plane, turned by the rotation (0x10074d6b..0x100750cd).
        ay = glm::vec3(c, sn, 0.0f) * hh;
        ax = glm::vec3(sn, -c, 0.0f) * hw;
    } else {
        // Facing the camera; the rotation turns it clockwise on the screen (0x100751d8).
        ax = (right * c - up * sn) * hw;
        ay = (right * sn + up * c) * hh;
    }
    corners = {at - ax + ay, at + ax + ay, at + ax - ay, at - ax - ay};
    return true;
}

// The modern light on a puff of smoke or dust at `p`, as the objects' shading (shaders/object.frag) without a normal:
// the sky and the ground ambient mixed as for a surface facing a little up, 0.3 of the sun's or moon's light, the
// special ambient and half of each point light's pool round it; at least the light floor.
glm::vec3 particle_light(const SceneLighting& l, const glm::vec3& p) { return ParticleLight(l).at(p); }

ParticleLight::ParticleLight(const SceneLighting& l)
    : base_(glm::mix(glm::vec3(l.ground), glm::vec3(l.sky), 0.65f) + glm::vec3(l.light) * 0.3f +
            to_linear(glm::vec3(l.special_ambient))),
      floor_(l.params.w) {
    const int count = std::min(static_cast<int>(l.params.y + 0.5f), static_cast<int>(kMaxSceneLights));
    lights_.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i) {
        const GpuPointLight& pl = l.lights[static_cast<std::size_t>(i)];
        if (pl.position_radius.w <= 0.0f) continue;
        lights_.push_back({glm::vec3(pl.position_radius), pl.position_radius.w, to_linear(glm::vec3(pl.color))});
    }
}

glm::vec3 ParticleLight::at(const glm::vec3& p) const {
    glm::vec3 light = base_;
    for (const Light& pl : lights_) {
        glm::vec3 d = pl.position - p;
        float d2 = glm::dot(d, d);
        if (d2 >= pl.radius * pl.radius) continue;
        light += pl.color * (2.5f * 0.5f * light_pool(d2, pl.radius));
    }
    return glm::max(light, glm::vec3(floor_));
}

bool particle_in_view(const Frustum& view, const glm::vec3& position, float width, float height, float size_factor) {
    // The quad's corners lie within half its diagonal of its centre, however it turns or stands.
    float half = 0.5f * std::sqrt(width * width + height * height) * std::abs(size_factor);
    return box_in_frustum(view, sphere_box(position, half));
}

// shaders/lighting.glsl apply_haze at `p`, taken per particle: what shows through, the colour in front, and under
// water what the water absorbs on the way.
ParticleHaze haze_at(const SceneLighting& l, const glm::vec3& p) {
    ParticleHaze h;
    if (l.haze.z < 0.5f) return h;
    glm::vec3 d = p - glm::vec3(l.camera);
    float dist = glm::length(d);
    if (l.haze.z > 1.5f) h.absorb = glm::exp(-dist * glm::vec3(0.018f, 0.006f, 0.004f));
    float x = dist * l.haze.x;
    h.through = std::exp(-3.5f * x * x * std::exp(-std::max(d.z, 0.0f) * l.haze.y));
    glm::vec3 dir = d / std::max(dist, 1e-4f);
    float glow = std::pow(std::max(glm::dot(dir, glm::vec3(l.to_light)), 0.0f), 6.0f) * l.haze.w;
    h.color = glm::vec3(l.haze_color) + glm::vec3(l.light) * glow * 0.25f;
    return h;
}

ParticleRenderer::ParticleRenderer(VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir, bool scene)
    : vk_(vk), scene_(scene), game_(game) {
    VkDevice d = vk_.device();
    max_textures_ = std::min(kMaxTextures, vk_.caps().max_sampled_images);
    {
        UploadBatch upload(vk_);
        assets::Image white;
        white.width = white.height = 1;
        white.mips.push_back(std::vector<std::byte>(4, std::byte{0xff}));
        textures_.push_back(upload.texture(white));
        upload.submit();
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    vk_check(vkCreateSampler(d, &si, nullptr, &sampler_), "vkCreateSampler");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = max_textures_;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | vk_.texture_array_binding_flags();
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 1;
    bf.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bf;
    li.flags = vk_.texture_array_layout_flags();
    li.bindingCount = 1;
    li.pBindings = &binding;
    vk_check(vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_), "vkCreateDescriptorSetLayout (particles)");
    constexpr auto kSets = static_cast<std::uint32_t>(VkContext::kFramesInFlight);
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_textures_ * kSets};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = vk_.texture_array_pool_flags();
    pi.maxSets = kSets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    vk_check(vkCreateDescriptorPool(d, &pi, nullptr, &pool_), "vkCreateDescriptorPool (particles)");
    std::array<VkDescriptorSetLayout, VkContext::kFramesInFlight> layouts;
    layouts.fill(set_layout_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kSets;
    ai.pSetLayouts = layouts.data();
    vk_check(vkAllocateDescriptorSets(d, &ai, sets_.data()), "vkAllocateDescriptorSets (particles)");

    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sizeof(GpuVertex) * kMaxVertices;
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo out{};
        vk_check(vmaCreateBuffer(vk_.allocator(), &bi, &aci, &vertices_[i].buffer, &vertices_[i].allocation, &out),
                 "create particle vertex buffer");
        vertices_[i].size = bi.size;
        mapped_[i] = out.pMappedData;
    }
    create_pipelines(shader_dir);
}

ParticleRenderer::~ParticleRenderer() {
    if (loader_) loader_->stop();
    VkDevice d = vk_.device();
    vk_.wait_idle();
    for (VkPipeline p : pipelines_)
        if (p) vkDestroyPipeline(d, p, nullptr);
    if (modern_pipeline_) vkDestroyPipeline(d, modern_pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
    if (sampler_) vkDestroySampler(d, sampler_, nullptr);
    for (auto& t : textures_) t.destroy(d, vk_.allocator());
    for (auto& b : vertices_) b.destroy(vk_.allocator());
}

const std::string& ParticleRenderer::lowered(std::string_view name) const {
    // assign keeps the buffer's capacity, so the lookups made every frame allocate nothing once it has grown.
    lower_scratch_.assign(name);
    for (char& c : lower_scratch_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower_scratch_;
}

glm::vec3 ParticleRenderer::texture_color(const std::string& name) const {
    auto it = colors_.find(lowered(name));
    return it == colors_.end() ? glm::vec3(1.0f) : it->second;
}

void ParticleRenderer::prepare(const game::ParticleSystem& particles) {
    // Read, decoded and averaged on the loader (they were in the frame: a new effect's first cast hitched); put up
    // here, in one submission. A zone's own copy is taken from its archives here, in memory: the reader is the zone's
    // and goes with it.
    std::vector<std::pair<std::string, LoadedParticleTexture>> done;
    if (loader_) done = loader_->take();
    std::vector<std::string> wanted;
    auto request = [&](std::string_view texture) {
        // Every live emitter asks each frame; only a name not yet loaded or loading is copied.
        const std::string& name = lowered(texture);
        if (!name.empty() && !slots_.contains(name) && !loading_.contains(name) &&
            std::find(wanted.begin(), wanted.end(), name) == wanted.end())
            wanted.push_back(name);
    };
    for (const auto& e : particles.emitters()) {
        const auto& d = particles.definitions()[e.definition];
        if (d.texture_frames.empty()) request(d.texture);
        else for (const auto& name : d.texture_frames) request(name);
    }
    if (!wanted.empty() && !loader_) loader_ = std::make_unique<core::LatestJobs<std::string, LoadedParticleTexture>>();
    for (auto& name : wanted) {
        std::optional<std::vector<std::byte>> zone_bytes;
        TextureReader late; // read in the job (reader_any_thread_)
        if (reader_ && reader_any_thread_) late = reader_;
        else if (reader_) zone_bytes = reader_(name);
        loading_.insert(name);
        const GameDir* game = &game_;
        // A job that throws would hand back no result, and the name would stay in loading_ for the session; failing
        // here, it comes back empty and counts as a missing texture.
        loader_->submit(name, [name, bytes = std::move(zone_bytes), late = std::move(late), game]() mutable {
            try {
                if (late) bytes = late(name);
                return load_particle_texture(name, std::move(bytes), *game);
            } catch (...) {
                return LoadedParticleTexture{};
            }
        });
    }
    if (done.empty()) return;
    UploadBatch upload(vk_);
    for (auto& [name, t] : done) {
        loading_.erase(name);
        // A missing texture's particles are not drawn (particle_texture_slot), nor are those of one past the limit.
        const std::uint32_t slot = particle_upload_slot(t.image.has_value(), textures_.size(), max_textures_);
        if (slot != kMissingParticleTexture) {
            colors_[name] = t.color;
            textures_.push_back(upload.texture(*t.image, TextureUse::Color));
        } else if (!t.image) {
            log::debug("particle texture {} not found", name);
        } else {
            log::debug("particle texture {} past the limit of {}", name, max_textures_);
        }
        slots_.emplace(name, slot);
    }
    upload.submit();
    ++version_;
}

void ParticleRenderer::draw(const FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& view,
                            const game::ParticleSystem& particles, float glow, std::span<const std::uint8_t> hidden,
                            const SceneLighting* modern) {
    if (particles.emitters().empty()) return;
    if (!scene_) modern = nullptr; // a preview has no scene-depth copy for the soft-particle shader
    const std::uint32_t slot = frame.frame_slot;
    if (set_version_[slot] != version_) {
        std::vector<VkDescriptorImageInfo> infos;
        for (const auto& t : textures_) infos.push_back({sampler_, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets_[slot];
        w.dstBinding = 0;
        w.descriptorCount = static_cast<std::uint32_t>(infos.size());
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = infos.data();
        vkUpdateDescriptorSets(vk_.device(), 1, &w, 0, nullptr);
        set_version_[slot] = version_;
    }
    // Camera axes from the view matrix (rows of its rotation), and the eye (its translation undone).
    glm::vec3 right(view[0][0], view[1][0], view[2][0]);
    glm::vec3 up(view[0][1], view[1][1], view[2][1]);
    const glm::vec3 eye = -glm::transpose(glm::mat3(view)) * glm::vec3(view[3]);
    auto* out = static_cast<GpuVertex*>(mapped_[slot]);
    std::uint32_t count = 0;
    const glm::vec3 forward = glm::cross(up, right);
    // One particle's quad, its six vertices at `dst`: the corners top-left, top-right, bottom-right, bottom-left
    // (particle_corners), the depth offset (+0xd0) taking it that much nearer along the view (the DLL subtracts it
    // from the view depth before projecting, so it keeps its place on the screen and its pixels grow). False when it
    // shows nothing. Called on the workers: it reads only what is shared and writes only `dst`.
    auto quad = [&](GpuVertex* dst, const game::Particle& p, std::uint32_t tex, glm::uvec2 grid,
                    std::uint32_t atlas_frame, const glm::vec4& shade, const glm::vec4& haze, std::uint32_t billboard,
                    float depth_offset) {
        std::array<glm::vec3, 4> corners;
        if (!particle_corners(p, billboard, right, up, corners)) return false;
        if (depth_offset != 0.0f) {
            const float z = glm::dot(p.position - eye, forward);
            if (z > 0.0f) {
                const float k = (z - depth_offset) / z;
                if (k <= 0.0f) return false;
                for (auto& v : corners) v = eye + (v - eye) * k;
            }
        }
        glm::vec2 cell(static_cast<float>(atlas_frame % grid.x), static_cast<float>(atlas_frame / grid.x));
        glm::vec2 cs(1.0f / static_cast<float>(grid.x), 1.0f / static_cast<float>(grid.y));
        auto v = [&](int i, float u, bool top) {
            float row = (top != rows_bottom_up_) ? 0.0f : 1.0f;
            return GpuVertex{corners[static_cast<std::size_t>(i)], (cell + glm::vec2(u, row)) * cs, p.color, tex, shade,
                             haze};
        };
        dst[0] = v(0, 0.0f, true);
        dst[1] = v(1, 1.0f, true);
        dst[2] = v(2, 1.0f, false);
        dst[3] = v(0, 0.0f, true);
        dst[4] = v(2, 1.0f, false);
        dst[5] = v(3, 0.0f, false);
        return true;
    };
    // In place of a quad that shows nothing after its place in the buffer was given out (the places are given out
    // before the quads are built, so the workers can build them at once): six vertices at one point draw no pixel.
    auto empty_quad = [](GpuVertex* dst) {
        const GpuVertex none{glm::vec3(0.0f), glm::vec2(0.0f), glm::vec4(0.0f), 0u, glm::vec4(0.0f), glm::vec4(0.0f)};
        for (int i = 0; i < 6; ++i) dst[i] = none;
    };
    // The classic lighting draws what the DLL draws: nothing nearer than 2 units or beyond 500 along the view
    // (0x100721d2: the camera's far distance, at most 500). A preview instead uses its own close camera and clip planes.
    auto classic_in_range = [&](const game::Particle& p) {
        const float z = glm::dot(p.position - eye, forward);
        return scene_ ? z >= 2.0f && z <= 500.0f : z > 0.0f;
    };
    auto extent = [](const game::Particle& p) { return std::max(p.width, p.length); };
    std::vector<Batch>& batches = batches_;
    batches.clear();
    // Only what can show: a particle outside the camera's view is not lit, sorted or built (it was, every one of
    // them, every frame).
    const Frustum frustum = frustum_from(view_proj);
    // The definitions' texture slots and flipbook grids, looked up once a frame rather than once a particle. Filled
    // on this thread (look) before the workers read them (looks[definition]).
    std::vector<DefinitionLook>& looks = looks_;
    looks.resize(particles.definitions().size());
    for (auto& l : looks) l.known = false;
    auto look = [&](std::size_t definition) -> const DefinitionLook& {
        DefinitionLook& l = looks[definition];
        if (!l.known) {
            const auto& d = particles.definitions()[definition];
            l.known = true;
            l.separate_frames = !d.texture_frames.empty();
            const auto frame_count = std::max<std::size_t>(d.texture_frames.size(), 1);
            l.frames.clear();
            l.frames.reserve(frame_count);
            for (std::size_t i = 0; i < frame_count; ++i) {
                const auto texture_frame_info = particle_texture_frame(d, static_cast<std::uint32_t>(i));
                l.grid = texture_frame_info.grid;
                // Lowered once into the scratch buffer for both lookups, no string made.
                const std::string& name = lowered(texture_frame_info.texture);
                const auto texture_slot = particle_texture_slot(name, slots_, loading_);
                l.frames.push_back({texture_slot.value_or(0u), !texture_slot});
            }
        }
        return l;
    };
    const auto& emitters = particles.emitters();
    const auto& definitions = particles.definitions();
    if (modern) {
        // Every particle in one stream, back to front by distance (the nearest kept when there are too many).
        const glm::vec3 camera(modern->camera);
        std::vector<OrderItem>& order = order_;
        order.clear();
        for (std::size_t ei = 0; ei < emitters.size(); ++ei) {
            if (ei < hidden.size() && hidden[ei]) continue;
            const auto& ps = emitters[ei].particles;
            for (std::size_t pi = 0; pi < ps.size(); ++pi) {
                const auto& p = ps[pi];
                if (p.hidden || !particle_in_view(frustum, p.position, extent(p), p.height, p.size_factor)) continue;
                glm::vec3 d = p.position - camera;
                order.push_back({glm::dot(d, d), static_cast<std::uint32_t>(ei), static_cast<std::uint32_t>(pi)});
            }
        }
        keep_nearest_back_to_front(order, kMaxVertices / 6);
        // Light sources stay unlit; the haze takes away their light. Smoke and dust are lit and hazed. Each
        // definition's look and lit flag first, here; then every item's quad at its own place, on the workers.
        std::vector<std::int8_t>& lit_by_definition = lit_by_definition_;
        lit_by_definition.assign(definitions.size(), -1);
        for (const OrderItem& item : order) {
            const auto definition = emitters[item.emitter].definition;
            look(definition);
            std::int8_t& lit = lit_by_definition[definition];
            if (lit < 0) lit = !definitions[definition].additive || scatters_light(definitions[definition].texture) ? 1 : 0;
        }
        const ParticleLight scene_light(*modern);
        core::parallel_for(order.size(), kQuadGrain, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                const OrderItem& item = order[i];
                GpuVertex* dst = out + i * 6;
                const auto& e = emitters[item.emitter];
                const auto& d = definitions[e.definition];
                const auto& p = e.particles[item.particle];
                const DefinitionLook& dl = looks[e.definition];
                const FrameLook& fl = dl.frames[p.frame % dl.frames.size()];
                if (fl.skip) {
                    empty_quad(dst);
                    continue;
                }
                ParticleHaze h = haze_at(*modern, p.position);
                glm::vec3 light = (lit_by_definition[e.definition] ? scene_light.at(p.position) : glm::vec3(glow)) * h.absorb;
                // It fades into the scene over a quarter of its size.
                float soft = std::max(p.width, p.height) * p.size_factor * 0.25f;
                std::uint32_t tex = fl.tex | (d.additive ? kAdditiveBit : 0u);
                // A flat one lying on what it meets (a rune circle on the ground) would fade away whole: it fades
                // over a quarter unit.
                if (d.billboard == 2 || d.billboard == game::kWorldBasisBillboard) soft = 0.25f;
                if (!quad(dst, p, tex, dl.grid, dl.separate_frames ? 0u : p.frame,
                          glm::vec4(light, std::max(soft, 0.25f)), glm::vec4(h.color, h.through), d.billboard,
                          d.depth_offset))
                    empty_quad(dst);
            }
        });
        count = static_cast<std::uint32_t>(order.size() * 6);
        if (count > 0) batches.push_back({0, count, -1});
    } else {
        // Each emitter's particles that show, counted on the workers; their places in the buffer given out here in
        // the emitters' order, up to the buffer's size; then their quads built on the workers.
        auto shows = [&](const game::Particle& p, const DefinitionLook& dl) {
            return !p.hidden && classic_in_range(p) &&
                   particle_in_view(frustum, p.position, extent(p), p.height, p.size_factor) &&
                   !dl.frames[p.frame % dl.frames.size()].skip;
        };
        for (std::size_t ei = 0; ei < emitters.size(); ++ei)
            if (!(ei < hidden.size() && hidden[ei]) && !emitters[ei].particles.empty()) look(emitters[ei].definition);
        std::vector<std::uint32_t>& quads = emitter_quads_;
        std::vector<std::uint32_t>& firsts = emitter_firsts_;
        quads.assign(emitters.size(), 0);
        firsts.assign(emitters.size(), 0);
        core::parallel_for(emitters.size(), kEmitterGrain, [&](std::size_t begin, std::size_t end) {
            for (std::size_t ei = begin; ei < end; ++ei) {
                const auto& e = emitters[ei];
                if ((ei < hidden.size() && hidden[ei]) || e.particles.empty()) continue;
                const DefinitionLook& dl = looks[e.definition];
                std::uint32_t n = 0;
                for (const auto& p : e.particles)
                    if (shows(p, dl)) ++n;
                quads[ei] = n;
            }
        });
        for (std::size_t ei = 0; ei < emitters.size(); ++ei) {
            const std::uint32_t room = (kMaxVertices - count) / 6;
            quads[ei] = std::min(quads[ei], room);
            if (quads[ei] == 0) continue;
            firsts[ei] = count;
            // The DLL's particle render state (FUN_1009eec0(5)) turns depth writes off for every emitter, +0x64 or
            // not (its +0x64 branch turns them back on after its own draw, so the emitters after it in that list
            // write depth: not copied).
            batches.push_back({count, quads[ei] * 6, definitions[emitters[ei].definition].additive ? 1 : 0});
            count += quads[ei] * 6;
        }
        core::parallel_for(emitters.size(), kEmitterGrain, [&](std::size_t begin, std::size_t end) {
            for (std::size_t ei = begin; ei < end; ++ei) {
                const std::uint32_t n = quads[ei];
                if (n == 0) continue;
                const auto& e = emitters[ei];
                const auto& d = definitions[e.definition];
                const DefinitionLook& dl = looks[e.definition];
                const glm::vec4 shade(glm::vec3(d.additive ? glow : 1.0f), 0.0f);
                GpuVertex* dst = out + firsts[ei];
                std::uint32_t written = 0;
                for (const auto& p : e.particles) {
                    if (written == n) break;
                    if (!shows(p, dl)) continue;
                    const FrameLook& fl = dl.frames[p.frame % dl.frames.size()];
                    const auto texture = fl.tex | (!scene_ && d.additive ? kAdditiveBit : 0u);
                    if (!quad(dst + written * 6, p, texture, dl.grid, dl.separate_frames ? 0u : p.frame, shade,
                              glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), d.billboard, d.depth_offset))
                        empty_quad(dst + written * 6);
                    ++written;
                }
            }
        });
    }
    if (batches.empty()) return;
    vmaFlushAllocation(vk_.allocator(), vertices_[slot].allocation, 0, VK_WHOLE_SIZE);
    VkCommandBuffer cmd = frame.cmd;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_[slot].buffer, &offset);
    // Set 1: the depth copy (modern only; the classic shader path does not read it).
    VkDescriptorSet sets[2] = {sets_[slot], vk_.shadow_set()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
    Push pc{view_proj, modern ? modern->screen : glm::vec4(0.0f),
            glm::vec4(modern ? 1.0f : 0.0f, scene_ ? 0.0f : 1.0f, 0.0f, 0.0f)};
    vkCmdPushConstants(cmd, layout_, kPushStages, 0, sizeof(pc), &pc);
    int bound = -2;
    for (const auto& b : batches) {
        if (b.pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, b.pipeline < 0 ? modern_pipeline_ : pipelines_[b.pipeline]);
            bound = b.pipeline;
        }
        vkCmdDraw(cmd, b.count, 1, b.first, 0);
    }
}

void ParticleRenderer::create_pipelines(const std::filesystem::path& shader_dir) {
    VkDevice d = vk_.device();
    VkPushConstantRange pc{kPushStages, 0, sizeof(Push)};
    VkDescriptorSetLayout set_layouts[2] = {set_layout_, vk_.shadow_set_layout()};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = set_layouts;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pc;
    vk_check(vkCreatePipelineLayout(d, &li, nullptr, &layout_), "vkCreatePipelineLayout (particles)");
    VkShaderModule vs = load_shader(d, shader_dir / "particle.vert.spv");
    VkShaderModule fs = load_shader(d, shader_dir / "particle.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr},
    };
    VkVertexInputBindingDescription binding{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, color)},
        {3, 0, VK_FORMAT_R32_UINT, offsetof(GpuVertex, texture)},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, shade)},
        {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, haze)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 6;
    vi.pVertexAttributeDescriptions = attrs;
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
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = vk_.scene_rendering(scene_);
    // Variants 0..1 classic (1 additive); 2 the modern stream: premultiplied (an additive particle gives alpha 0).
    // None writes depth.
    for (int variant = 0; variant < 3; ++variant) {
        const bool modern = variant == 2;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = VK_TRUE;
        dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 0xf;
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = modern ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = (!modern && (variant & 1)) ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        // Scene alpha is retained; an offscreen preview needs particle coverage in its transparent background
        // so that ImGui composites the glow outside the item's mesh too.
        ba.srcAlphaBlendFactor = scene_ ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = scene_ ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
        const SceneBlends blends = vk_.scene_blends(ba, SceneOutput::Reactive, scene_);
        const VkPipelineColorBlendStateCreateInfo cb = blends.info();
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
                                           modern ? &modern_pipeline_ : &pipelines_[variant]),
                 "vkCreateGraphicsPipelines (particles)");
    }
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
}

} // namespace hailstone::render
