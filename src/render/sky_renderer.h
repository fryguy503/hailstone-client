#pragma once

// Draws the classic sky (assets/sky.h) behind everything: the dome, the sun
// and moon, then the cloud layer, centred on the camera, pinned to the far
// plane, with no depth writes and no fog. Draw it first in a frame.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "render/gpu.h"
#include "render/vk_context.h"

#include "assets/sky_weather.h"
#include "game/cloud_patches.h"

namespace hailstone { class GameDir; }
namespace hailstone::assets { struct Sky; }

namespace hailstone::render {

class CloudRenderer;

// The sky state for one frame, from the game clock (see SkyClock in game/).
struct SkyFrame {
    float hour = 12.0f;        // sky time: server hour (1..24) plus minutes / 60
    double real_seconds = 0.0; // drives the cloud patches (sky time = GetTickCount x TimeScale 1.0)
    // The modern mode's haze at the horizon (render/scene_lighting.h set_haze): linear rgb, a > 0 blends the dome and
    // the clouds into it near and below the horizon and brightens the sun and moon; 0 leaves the classic sky.
    glm::vec4 horizon_haze{0.0f};
    // The modern mode's sky: an atmosphere (shaders/atmosphere.frag) instead of the dome for the plain skies
    // (SkyRenderer::has_atmosphere), lit by the sun at `to_sun` (render::Daylight), with the clouds lit by
    // `cloud_light` (linear) instead of the sky colour table.
    bool atmosphere = false;
    // Under a zone floating in the sky (render::horizon_open_below): the painted dome and its cloud layers drawn
    // again mirrored under the horizon, behind themselves, so the sky goes on down to the nadir past the dome's foot.
    bool mirror_below = false;
    glm::vec3 to_sun{0.0f, 0.0f, 1.0f};
    glm::vec3 to_moon{0.0f, 0.0f, -1.0f}; // render::Daylight::to_moon, its phase from where it stands to the sun
    float blood_moon = 0.0f;              // render::Daylight::blood_moon: the moon red
    float aurora = 0.0f;                  // game::SkyClock::aurora: the northern lights by night (atmosphere only)
    glm::vec3 cloud_light{1.0f};
    float storm = 0.0f; // render::WeatherLook: an overcast sky, more cloud
    float flash = 0.0f; // the lightning
    // display.volumetric_clouds, with the atmosphere: clouds marched through the air (render/cloud_renderer.h) in
    // place of the painted cloud layer and the weather.ini patches, lit by the open sky's daylight under the weather
    // (render::Daylight after render::apply_storm).
    bool volumetric_clouds = false;
    glm::vec3 to_light{0.0f, 0.0f, 1.0f}; // render::Daylight::to_light
    glm::vec3 light{0.0f};                // render::Daylight::light
    glm::vec3 sky_light{0.0f};            // render::Daylight::sky
};

// The light multiplier for an outdoor zone at a sky hour: the sky light level
// (assets::sky_light_level, 0 night .. 1 day) lifted to a floor of 0.3, because
// zones are drawn full-bright here rather than lit (docs/unknowns.md).
float outdoor_brightness(float hour);

class SkyRenderer {
public:
    static constexpr std::uint32_t kMaxTextures = 12; // sky.frag's texture array
    static constexpr std::uint32_t kMaxCloudVertices = 16384;

    // With `game`, the zone's procedural-sky cloud patches are drawn over the
    // classic layers (assets::load_zone_clouds, game::CloudLayer).
    SkyRenderer(VkContext& vk, const assets::Sky& sky, const std::filesystem::path& shader_dir,
                const GameDir* game = nullptr, std::string_view zone = {});
    ~SkyRenderer();
    SkyRenderer(const SkyRenderer&) = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;

    int sky_index() const { return index_; }
    // The skies the atmosphere stands in for: the normal (1), desert (2) and cottony (4) ones. The others (the
    // planes', Luclin's, the red and grey skies) keep their painted domes.
    bool has_atmosphere() const { return index_ == 1 || index_ == 2 || index_ == 4; }
    // CSkyBase's "TimeScale" property (sky+0x1c, 1.0 by default): sky time per real time.
    void set_time_scale(float scale) { time_scale_ = scale; }

    // Records the volumetric clouds' march for this frame (SkyFrame::volumetric_clouds over the atmosphere; nothing
    // otherwise): outside rendering, before VkContext::begin_scene, with the frame's projection and view as draw
    // takes them. Without it draw shows no volumetric clouds that frame.
    void prepare(const FrameInfo& frame, const glm::mat4& projection, const glm::mat4& view, const SkyFrame& sky);
    // Whether the volumetric clouds were prepared for the frame being drawn.
    bool volumetric_ready() const { return volumetric_ready_; }

    // `view` is the camera's view matrix; its translation is ignored.
    void draw(const FrameInfo& frame, const glm::mat4& projection, const glm::mat4& view, const SkyFrame& sky);

private:
    enum class Kind : std::uint8_t { Dome, Bodies, Clouds };
    struct Draw {
        Kind kind = Kind::Dome;
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t texture = 0;
        int pipeline = 0; // 0 opaque, 1 alpha blended, 2 additive
        float opacity = 1.0f;
    };

    void create_descriptors();
    void create_pipelines(const std::filesystem::path& shader_dir);

    VkContext& vk_;
    int index_ = 0;
    std::vector<Draw> draws_;
    Buffer vertex_buffer_;
    Buffer index_buffer_;
    std::vector<Texture> textures_;
    std::array<Buffer, VkContext::kFramesInFlight> colors_{};
    std::array<void*, VkContext::kFramesInFlight> colors_mapped_{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, VkContext::kFramesInFlight> sets_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[4]{}; // opaque, alpha blended, additive; the atmosphere (a full-screen triangle)

    struct Clouds {
        game::CloudLayer layer;
        std::uint32_t texture = 0;
    };
    std::vector<Clouds> clouds_;
    std::array<Buffer, VkContext::kFramesInFlight> cloud_vertices_{};
    std::array<void*, VkContext::kFramesInFlight> cloud_mapped_{};
    double last_seconds_ = -1.0;
    float time_scale_ = 1.0f;

    // The volumetric clouds, made the first time they are prepared; the atmosphere reads a clear 1x1 image without
    // them (shaders/atmosphere.frag binding 2).
    std::filesystem::path shader_dir_;
    std::unique_ptr<CloudRenderer> volumetric_;
    bool volumetric_ready_ = false;
    bool volumetric_failed_ = false;
    Texture clear_clouds_;
    void draw_clouds(VkCommandBuffer cmd, std::uint32_t slot, const glm::mat4& sky_vp, const SkyFrame& sky,
                     bool atmosphere, bool hidden);
};

} // namespace hailstone::render
