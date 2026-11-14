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
// lawn.frag): each patch's blades are built in the vertex shader, no texture, at three levels of detail by distance
// (lawn_lod); past kFloraReach every s-th patch each way is kept, s times as wide, so the carpet keeps its cover. A
// grown tile keeps its patches by the coarsest stride that keeps them, so a far tile walks only those it draws.

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/flora.h"
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

// The lawn's levels of detail: blades a side of a patch (side x side of them), the segments of a blade, and its
// width (units at the root: fewer blades are wider, so a patch keeps its cover).
struct LawnLod {
    std::uint32_t side = 0, segments = 0;
    float blade_width = 0.0f;
    std::uint32_t vertices() const { return side * side * ((segments - 1) * 6 + 3); }
};
inline constexpr std::array<LawnLod, 3> kLawnLods = {{{6, 4, 0.3f}, {3, 3, 0.55f}, {2, 1, 0.8f}}};
inline constexpr float kLawnNear = 45.0f, kLawnMid = 140.0f; // where the first two levels end
// The level of detail of a patch at distance `d` (an index into kLawnLods).
std::size_t lawn_lod(float d);
// How many patches each way one kept patch stands for at distance `d`: 1 within kFloraReach, 2 within twice it, 4
// past that (at least 1 / sqrt(flora_keep(d)), so the far lawn costs less than the near does). Powers of two, so the
// patches a coarser stride keeps are among those a finer one keeps.
std::uint32_t lawn_stride(float d);
// The coarsest stride (1, 2 or 4) that keeps the patch standing at `p`: its square's column and row are multiples of it.
std::uint32_t lawn_patch_stride(glm::vec2 p);

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

    // Hailstone's zone flora (assets::ZoneFloraGrower): drops the tiles out of reach, grows the missing ones within it.
    void grow_tiles(const glm::vec3& camera);
    static constexpr std::chrono::microseconds kGrowBudget{3000};

    VkContext& vk_;
    float reach_ = kFloraReach;
    // A grown tile's plants: the lawn first, the patches that stride 4 keeps, then those stride 2 keeps, then the rest
    // (lawn_end: where each part ends), then the other plants.
    struct Grown {
        std::vector<assets::FloraInstance> plants;
        std::array<std::uint32_t, 3> lawn_end{};
    };
    std::unordered_map<std::uint32_t, Grown> grown_; // by index into ZoneFlora::tiles
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
    std::array<Buffer, VkContext::kFramesInFlight> instances_{};
    std::array<void*, VkContext::kFramesInFlight> mapped_{};
    struct GpuPlant; // flora_renderer.cpp
    std::array<std::vector<GpuPlant>, kLawnLods.size()> lawn_; // per draw: the lawn's patches by level
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline lawn_pipeline_ = VK_NULL_HANDLE;
};

} // namespace hailstone::render
