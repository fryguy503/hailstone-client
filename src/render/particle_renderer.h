#pragma once

// Draws game::ParticleSystem's particles as quads (particle_corners). Textures
// come from the game directory's SpellEffects\, EnvEmitterEffects\ and
// ActorEffects\ (searched in that order). EDD +0x68 picks DESTBLEND ONE
// (additive) over INVSRCALPHA; no particle writes depth (the DLL's render state).
//
// The modern lighting (docs/graphics.md, Particles) draws them its own way: one stream sorted back to front, no depth
// writes, soft where they meet the scene (VkContext::begin_translucent's depth copy, so drawn after it), in the haze,
// and the alpha-blended ones (smoke, dust) lit by the scene's sky, sun or moon and point lights.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "core/latest_jobs.h"
#include "render/gpu.h"
#include "render/particle_textures.h"
#include "render/vk_context.h"

namespace hailstone { class GameDir; }
namespace hailstone::game { class ParticleSystem; struct Particle; }

namespace hailstone::render {

struct SceneLighting;

// The modern light on a puff of smoke or dust (or a plant, render/flora_renderer.h) at `p`, as the objects' shading
// without a normal; and shaders/lighting.glsl apply_haze there: what shows through, the colour in front, and under
// water what the water absorbs on the way.
glm::vec3 particle_light(const SceneLighting& l, const glm::vec3& p);
// particle_light with the frame's part worked out once (the ambient and the lights' linear colours): the same light,
// for drawing many particles or plants under one SceneLighting.
class ParticleLight {
public:
    explicit ParticleLight(const SceneLighting& l);
    glm::vec3 at(const glm::vec3& p) const;

private:
    struct Light {
        glm::vec3 position{0.0f};
        float radius = 0.0f;
        glm::vec3 color{0.0f}; // linear
    };
    glm::vec3 base_{0.0f};
    float floor_ = 0.0f;
    std::vector<Light> lights_;
};
struct ParticleHaze {
    glm::vec3 color{0.0f};
    float through = 1.0f;
    glm::vec3 absorb{1.0f};
};
ParticleHaze haze_at(const SceneLighting& l, const glm::vec3& p);

struct SceneLighting;

// The modern lighting's scale for additive (glowing) particles: spells and fires light up past white and bloom.
constexpr float kParticleGlow = 2.5f;

// A particle's quad corners (top-left, top-right, bottom-right, bottom-left: the texture's top edge first) for an
// EDD billboard mode (+0x70), as EQGraphicsDX9's quad builder (in FUN_10072160) places them: 1 a streak along the
// particle's axis, widened on the screen's x; 2 flat on the world's horizontal plane; anything else facing the
// camera (`right`, `up`). The rotation turns modes 0 and 2. Hailstone's kWorldBasisBillboard instead takes the
// width and height directions from the particle's axes[0] and axes[1], without camera-facing rotation.
// kTrailStrandBillboard keeps the segment along axes[1], with its width facing the camera.
// False when it is not drawn (below the axis, an invalid generated basis, or a strand viewed end-on).
bool particle_corners(const game::Particle& p, std::uint32_t billboard, const glm::vec3& right, const glm::vec3& up,
                      std::array<glm::vec3, 4>& corners);

struct Frustum;
// Whether a particle's quad can show in the camera's view volume (render/culling.h): the sphere it may turn in, its
// half-diagonal, against the frustum. Conservative, as the zone's chunk tests: nothing that shows is dropped.
bool particle_in_view(const Frustum& view, const glm::vec3& position, float width, float height, float size_factor);

// The modern stream's order: back to front by `dist2` (the farthest first), keeping the `cap` nearest when there are
// more. Past the cap only the kept ones are sorted (std::nth_element picks them first).
template <class Item> void keep_nearest_back_to_front(std::vector<Item>& items, std::size_t cap) {
    auto farther = [](const Item& a, const Item& b) { return a.dist2 > b.dist2; };
    if (items.size() > cap) {
        std::nth_element(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(cap), items.end(),
                         [](const Item& a, const Item& b) { return a.dist2 < b.dist2; });
        items.resize(cap);
    }
    std::sort(items.begin(), items.end(), farther);
}

class ParticleRenderer {
public:
    static constexpr std::uint32_t kMaxTextures = 512; // at most; fewer where the device allows fewer
    static constexpr std::uint32_t kMaxVertices = 6 * 20000;

    // scene=false targets a single-colour preview image, without the scene's motion/reactive attachments or depth copy.
    ParticleRenderer(VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir, bool scene = true);
    ~ParticleRenderer();
    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // A source tried before the game directory's effect folders: the zone's
    // archives for the WLD particle clouds' textures (GENG00.DDS, CSMOKE.DDS
    // live in <zone>_obj.s3d).
    // With `any_thread` (a reader safe on any thread, such as assets::CharacterLibrary::read_file) it runs in the
    // loader job, so a read waiting on its source (the library's lock, held by the background dresser) never holds
    // up a frame; otherwise it runs in prepare, on the calling thread.
    using TextureReader = std::function<std::optional<std::vector<std::byte>>(const std::string&)>;
    void set_texture_reader(TextureReader reader, bool any_thread = false) {
        reader_ = std::move(reader);
        reader_any_thread_ = any_thread;
    }
    // The texture's last row at the quad's top: the DLL's quad builder swaps top and bottom when its fourth argument
    // is set (0x10075614), which FUN_100759a0 passes only for the WLD clouds' list (a flame texture's tail then
    // points up). The EDD emitters (spells, actors, environment) keep the first row at the top.
    void set_rows_bottom_up(bool on) { rows_bottom_up_ = on; }
    // Asks for the textures the live emitters need, and puts up those the loader thread has finished (a frame or
    // more later: until then their particles are not drawn); call outside a frame.
    void prepare(const game::ParticleSystem& particles);
    // `glow` scales the additive particles' light (the modern lighting's HDR: above 1 they bloom). The emitters
    // whose entry in `hidden` (by emitter index, when given) is non-zero are left out. With `modern` (the frame's
    // modern SceneLighting, its haze and screen set) they are drawn the modern way, after begin_translucent.
    void draw(const FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& view,
              const game::ParticleSystem& particles, float glow = 1.0f, std::span<const std::uint8_t> hidden = {},
              const SceneLighting* modern = nullptr);
    // A loaded texture's colour, its brightest channel 1 (what its additive particles add, on average): the hue of
    // the light they give off (game::particle_lights). White for one not loaded.
    glm::vec3 texture_color(const std::string& name) const;

private:
    // `name` lower-cased into lower_scratch_, valid until the next call: the per-frame lookups make no string.
    const std::string& lowered(std::string_view name) const;
    void create_pipelines(const std::filesystem::path& shader_dir);

    VkContext& vk_;
    bool scene_ = true;
    std::uint32_t max_textures_ = 0; // kMaxTextures, within DeviceCaps::max_sampled_images
    const GameDir& game_;
    TextureReader reader_;
    bool reader_any_thread_ = false;
    bool rows_bottom_up_ = false;
    std::vector<Texture> textures_;               // slot 0: white (no particle draws it now)
    std::unordered_map<std::string, std::uint32_t> slots_; // lower-case name -> slot (kMissingParticleTexture: none)
    std::unordered_map<std::string, glm::vec3> colors_;    // lower-case name -> texture_color
    mutable std::string lower_scratch_;                    // lowered's buffer (main thread only)
    // prepare's pass over the emitters: each definition's textures are asked for once a pass, not once per emitter
    // (emitters share definitions by the hundred). def_pass_[d] is the pass that last asked for definition d.
    std::vector<std::uint32_t> def_pass_;
    std::uint32_t prepare_pass_ = 0;
    std::vector<std::string> wanted_; // prepare's names to load this pass (kept for its capacity)
    std::uint32_t version_ = 1;
    std::array<std::uint32_t, VkContext::kFramesInFlight> set_version_{};
    std::array<Buffer, VkContext::kFramesInFlight> vertices_{};
    std::array<void*, VkContext::kFramesInFlight> mapped_{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, VkContext::kFramesInFlight> sets_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[2]{}; // classic: 1 additive
    VkPipeline modern_pipeline_ = VK_NULL_HANDLE; // premultiplied: both kinds in one sorted stream, no depth write

    // draw()'s lists, kept so their memory is reused frame to frame: the modern stream's particles in order, each
    // definition's texture slot and flipbook grid (`skip`: its texture is not up yet, missing, unnamed or past the
    // limit, so its particles are not drawn rather than drawn white; particle_texture_slot), and whether its particles
    // are lit (-1 not known yet).
    struct OrderItem { float dist2; std::uint32_t emitter, particle; };
    struct FrameLook { std::uint32_t tex = 0; bool skip = false; };
    struct DefinitionLook {
        std::vector<FrameLook> frames;
        glm::uvec2 grid{1};
        bool known = false, separate_frames = false;
    };
    struct Batch { std::uint32_t first, count; int pipeline; };
    std::vector<OrderItem> order_;
    // Per emitter, the particles it shows and the first one's place: the classic path's quads and first vertex, the
    // modern path's items in order_ (draw gives the places out before the workers fill them).
    std::vector<std::uint32_t> emitter_quads_, emitter_firsts_;
    static constexpr std::size_t kQuadGrain = 256;  // modern particles a worker builds at a time
    static constexpr std::size_t kEmitterGrain = 8; // emitters a worker takes at a time
    std::vector<DefinitionLook> looks_;
    std::vector<std::int8_t> lit_by_definition_;
    std::vector<Batch> batches_;

    // Textures asked for and not put up yet (lower case), and the thread reading and decoding them
    // (load_particle_texture). Last, so it stops first: its jobs use game_.
    std::unordered_set<std::string> loading_;
    std::unique_ptr<core::LatestJobs<std::string, LoadedParticleTexture>> loader_;
};

} // namespace hailstone::render
