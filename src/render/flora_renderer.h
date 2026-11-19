#pragma once

// The radial flora of EQG heightmap zones (assets/flora.h; /showgrass, the Options window's Radial Flora): the plants
// of the tiles near the camera, each a quad standing on its foot, or two crossed ones (*CROSSFLORA), turned as it was
// placed, its picture a quarter of its texture with *QUADTEXTURE, swaying (*SWAY: the top moves *SWAYDISPLACEMENT of
// its height to and fro over *SWAYPERIOD seconds). The look is this client's (docs/graphics.md "Radial flora"): cut
// out at half alpha, lit in the shaders as the zone around them (the sun through its shadow, per pixel; the sky, the
// place's light and the lamps per vertex) and hazed with distance, bent away from whoever walks through them; the
// plants shrink into the ground over the last fifth of the reach, as the original draws flora only round the camera.
//
// Hailstone's lawn (assets/zone_flora.h, kLawnDefinition) is drawn by a pipeline of its own (shaders/lawn.vert,
// lawn.frag): each patch's blades are built in the vertex shader, no texture, and nothing about it changes at a line
// as the camera moves: a patch's blades come in a fixed order, any first few of them spread evenly over it, and with
// distance the last of them shrink away one by one while the rest widen (lawn_blades, lawn_blade_width), so the three
// draws by distance (lawn_lod, kLawnLods: each the first blades of the one before) switch only blades already gone.
// Past kFloraReach every second patch each way fades out over kLawnStrideBand while the ones kept widen to take their
// squares, past twice it every fourth (lawn_patch_fade, lawn_patch_side). A grown tile keeps its patches by the
// coarsest stride that keeps them, so a far tile walks only those it draws.

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "assets/flora.h"
#include "render/flora_growth.h"
#include "render/gpu.h"
#include "render/scene_lighting.h"
#include "render/vk_context.h"

namespace hailstone::assets { struct Zone; }

namespace hailstone::render {

struct SceneLighting;

// How far round the camera the flora is drawn (the original's radius is not traced; docs/unknowns.md).
inline constexpr float kFloraReach = 400.0f;
// Hailstone's Flora Distance (display.flora_distance) reaches up to this far. Past kFloraReach the plants thin out
// with distance (flora_keep), so the far ring costs about what the near circle does.
inline constexpr float kMaxFloraReach = 1200.0f;

// The share of the plants kept at distance `d`: all within kFloraReach, (kFloraReach / d)^2 past it.
float flora_keep(float d);

// The lawn's draws by distance: how many of a patch's blades each draws (the first ones) and the segments of a blade.
struct LawnLod {
    std::uint32_t blades = 0, segments = 0;
    std::uint32_t vertices() const { return blades * ((segments - 1) * 6 + 3); }
};
inline constexpr std::array<LawnLod, 3> kLawnLods = {{{49, 3}, {16, 3}, {9, 1}}};
// All of a patch's blades within kLawnFull; kLawnLods[1]'s count by kLawnNear, where the first draw ends, and [2]'s
// by kLawnMid, where the second ends.
inline constexpr float kLawnFull = 40.0f, kLawnNear = 90.0f, kLawnMid = 250.0f;
// A blade's width at its widest with all of a patch's blades, in units.
inline constexpr float kLawnBladeWidth = 0.24f;
// Hailstone's Grass Density (display.grass_density, a share): how many of kLawnLods' blades each draw has, the
// blades keeping their width, so less is sparser (and cheaper) and more is thicker.
inline constexpr float kMinGrassDensity = 0.25f, kMaxGrassDensity = 3.0f;
// The option in percent (the Options window's slider), clamped to 25 .. 300.
inline int grass_density_percent(int percent) { return percent < 25 ? 25 : percent > 300 ? 300 : percent; }
// kLawnLods[k]'s blades at `density` (clamped to the bounds above): at least one.
std::uint32_t lawn_lod_blades(std::size_t k, float density = 1.0f);
// How many of a patch's blades stand at distance `d`, smoothly fewer: kLawnLods[0]'s count to kLawnFull, [1]'s at
// kLawnNear, [2]'s from kLawnMid (each at `density`, lawn_lod_blades). Fractional: the last one is part grown.
float lawn_blades(float d, float density = 1.0f);
// A blade's width at distance `d`: wider as there are fewer than the nearest draw's (by the square root), so a patch
// keeps its cover; kLawnBladeWidth near at any density.
float lawn_blade_width(float d, float density = 1.0f);
// The draw a patch at distance `d` goes in (an index into kLawnLods): one with at least lawn_blades(d) blades.
std::size_t lawn_lod(float d);
// How far before kFloraReach (and twice it) the patches a coarser stride drops fade out.
inline constexpr float kLawnStrideBand = 120.0f;
// How many patches each way one kept patch stands for at distance `d`: 1 within kFloraReach, 2 within twice it, 4
// past that (at least 1 / sqrt(flora_keep(d)), so the far lawn costs less than the near does). Powers of two, so the
// patches a coarser stride keeps are among those a finer one keeps.
std::uint32_t lawn_stride(float d);
// The coarsest stride (1, 2 or 4) that keeps the patch standing at `p`: its square's column and row are multiples of it.
std::uint32_t lawn_patch_stride(glm::vec2 p);
// A patch's height share at distance `d` by its own stride (lawn_patch_stride): 1, then down to 0 over
// kLawnStrideBand before the distance where lawn_stride drops it.
float lawn_patch_fade(std::uint32_t patch_stride, float d);
// The side of the square a patch covers at distance `d` (kLawnCell to 4 of them), growing over the same bands as the
// patches round it fade, so the cover stays whole.
float lawn_patch_side(std::uint32_t patch_stride, float d);

class FloraRenderer {
public:
    static constexpr std::uint32_t kMaxPlants = 262144; // a frame's, nearest first (10 MB of instances a frame)
    static constexpr std::size_t kMaxPushers = 8;
    static constexpr std::uint32_t kNoTexture = 0xffffffffu; // a definition_slot_ for a plant left out
    static constexpr std::uint32_t kMaxTextures = 64; // at most; fewer where the device allows fewer

    // `zone` must outlive this (its archives give the textures and it holds the plants).
    FloraRenderer(VkContext& vk, const assets::Zone& zone, const std::filesystem::path& shader_dir);
    ~FloraRenderer();
    FloraRenderer(const FloraRenderer&) = delete;
    FloraRenderer& operator=(const FloraRenderer&) = delete;

    // How far the plants are drawn: kFloraReach .. kMaxFloraReach (display.flora_distance).
    void set_reach(float reach);
    float reach() const { return reach_; }
    // Lets go of every grown tile, so they grow again from the zone's flora as it is now (it was set up again:
    // assets::add_zone_flora_ground). On the frame's thread, before the frame's draws are recorded.
    void restart() {
        growth_.restart(); // the tiles growing were grown from the flora as it was
        near_tiles_.clear();
    }
    // How thick the lawn is: kMinGrassDensity .. kMaxGrassDensity (display.grass_density).
    void set_density(float density);
    float density() const { return density_; }

    // Cover the zone's files do not hold (the server's doors and objects, a campfire; assets::FloraCover), asked
    // for each tile as it grows with the tile's square and assets::kFloraCoverMargin round it: a test the tile's
    // plants are grown with, or an empty one when nothing there covers. Kept until changed; the tiles already grown
    // keep their plants until regrow.
    using CoverTest = std::function<bool(const glm::vec3& at)>;
    using CoverSource = std::function<CoverTest(glm::vec2 lo, glm::vec2 hi)>;
    void set_cover(CoverSource source) { cover_ = std::move(source); }
    // Grows again the grown tiles whose square, with assets::kFloraCoverMargin round it, touches [lo, hi] (cover
    // there came or went). Each keeps its plants until its new ones have grown, nearest first.
    void regrow(glm::vec2 lo, glm::vec2 hi);

    // Who the plants bend away from (Hailstone's own): xyz a foot, w how far round it (the player, the spawns walking
    // through the grass); at most kMaxPushers, nearest first. Kept until changed.
    void set_pushers(std::span<const glm::vec4> pushers);

    // The plants within reach() of `camera`, nearest tiles first, opaque, just after VkContext::begin_translucent
    // (its depth copy holds the ground without them, for the target ring) and before the water. `lighting`: the frame's (modern: lit in the shaders with the sun's shadow, the
    // sky, the lamps and the haze; classic: the scene's ambient and sun). Returns how many were drawn.
    std::uint32_t draw(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera, double seconds,
                       const SceneLighting& lighting, bool modern);

private:
    void create_pipelines(const std::filesystem::path& shader_dir);
    VkPipeline create_pipeline(const std::filesystem::path& vert, const std::filesystem::path& frag);

    // Hailstone's zone flora (assets::ZoneFloraGrower): drops the tiles out of reach, takes the tiles grown since the
    // last frame and queues the missing and stale ones within reach on growth_, nearest first.
    void grow_tiles(const glm::vec3& camera);
    // How many tiles are queued or growing at once: few, so the next ones queued are the nearest as the camera moves.
    static constexpr std::size_t kGrowInFlight = 3;

    VkContext& vk_;
    // The clock a frame ago (the plants' motion under the temporal upscaler): `seconds_` is this frame's, for the
    // frame `seconds_frame_` (VkContext::frames_begun).
    double seconds_ = 0.0, seconds_before_ = 0.0;
    std::uint64_t seconds_frame_ = 0;
    float reach_ = kFloraReach;
    float density_ = 1.0f;
    // A grown tile's plants: the lawn first, the patches that stride 4 keeps, then those stride 2 keeps, then the rest
    // (lawn_end: where each part ends), then the other plants.
    struct Grown {
        std::vector<assets::FloraInstance> plants;
        std::array<std::uint32_t, 3> lawn_end{};
        bool stale = false; // drawn until grown again (regrow)
    };
    // A tile's plants, on growth_'s thread: touches only the grower (read-only, shared) and the cover test (its own).
    static Grown grow_tile(const assets::ZoneFloraGrower& grower, std::uint32_t t, const CoverTest& extra);
    std::vector<std::pair<float, std::uint32_t>> near_tiles_; // per draw: tiles in reach by distance
    std::uint32_t max_textures_ = 0; // kMaxTextures, within DeviceCaps::max_sampled_images
    const assets::Zone& zone_;
    std::vector<Texture> textures_;             // slot 0: white
    std::vector<std::uint32_t> definition_slot_; // per flora definition
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, VkContext::kFramesInFlight> sets_{};
    std::unique_ptr<FrameBuffers> lighting_buffers_; // SceneLighting, per frame in flight
    std::unique_ptr<FrameBuffers> frame_buffers_;    // FloraFrame
    std::vector<glm::vec4> pushers_;
    CoverSource cover_;
    std::array<Buffer, VkContext::kFramesInFlight> instances_{};
    std::array<void*, VkContext::kFramesInFlight> mapped_{};
    struct GpuPlant; // flora_renderer.cpp
    std::array<std::vector<GpuPlant>, kLawnLods.size()> lawn_; // per draw: the lawn's patches by level
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline lawn_pipeline_ = VK_NULL_HANDLE;
    // The grown tiles (by index into ZoneFlora::tiles) and those growing off the frame (grow_tile), one thread.
    // Last: stopped and joined first as this goes.
    FloraGrowth<Grown> growth_{kGrowInFlight};
};

} // namespace hailstone::render
