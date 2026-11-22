#include "render/scene_lighting.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "assets/sky.h"
#include "game/light_list.h"
#include "game/scene_light.h"
#include "render/atmosphere.h"
#include "render/vk_util.h"

namespace hailstone::render {

std::optional<LightingMode> parse_lighting_mode(std::string_view name) {
    if (name == "classic") return LightingMode::Classic;
    if (name == "smooth") return LightingMode::SmoothClassic;
    if (name == "modern" || name == "enhanced") return LightingMode::Modern;
    return std::nullopt;
}

std::string_view lighting_mode_name(LightingMode mode) {
    switch (mode) {
    case LightingMode::Classic: return "classic";
    case LightingMode::SmoothClassic: return "smooth";
    case LightingMode::Modern: break;
    }
    return "modern";
}

Tonemap automatic_tonemap(LightingMode mode) {
    switch (mode) {
    case LightingMode::Classic: return Tonemap::None;
    case LightingMode::SmoothClassic: return Tonemap::Soft;
    case LightingMode::Modern: break;
    }
    return Tonemap::AgX;
}

namespace {

float smoothstep(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

namespace {

// The way to the sun at a sky hour: the angle a = 512 - h of the sweep's heading h (512ths; 0 at noon, a quarter turn
// at 6:00 and 18:00) round the tilted sky's pole (sky_pole): at 6:00 due east (-Y), at noon kSkyNoonElevation over the
// southern horizon (-X), at 18:00 due west (+Y), and as far under the northern horizon at midnight.
glm::vec3 sun_direction(float hour) {
    float a = (512.0f - assets::sky_sweep_heading(hour)) * (2.0f * std::numbers::pi_v<float> / 512.0f);
    const float tilt = glm::radians(90.0f - kSkyNoonElevation);
    const glm::vec3 noon(-std::sin(tilt), 0.0f, std::cos(tilt));
    const glm::vec3 east(0.0f, -1.0f, 0.0f);
    return std::cos(a) * noon + std::sin(a) * east;
}

} // namespace

glm::vec3 moon_direction(float hour, float phase) {
    // Where the sun stood phase x 24 hours before, a little off the sun's path (so a new moon passes beside the sun).
    float h = hour - 24.0f * phase;
    h = std::fmod(h - 1.0f, 24.0f);
    if (h < 0.0f) h += 24.0f;
    return glm::normalize(sun_direction(h + 1.0f) + 0.09f * sky_pole());
}

glm::vec3 sky_pole() {
    const float e = glm::radians(90.0f - kSkyNoonElevation);
    return {std::cos(e), 0.0f, std::sin(e)};
}

float moon_lit(float phase) {
    return 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * phase));
}

float haze_ambient_scale(float sun_z, bool outdoor, float ambient) {
    ambient = std::clamp(ambient, 0.0f, 1.0f);
    return outdoor ? glm::mix(ambient, 1.0f, smoothstep(-0.1f, 0.25f, sun_z)) : ambient;
}

Daylight daylight(float hour, bool outdoor, float moon_phase, float blood_moon, float ambient, float baked_lamps) {
    ambient = std::clamp(ambient, 0.0f, 1.0f);
    baked_lamps = std::clamp(baked_lamps, 0.0f, kMaxBakedLamps);
    Daylight out;
    if (!outdoor) {
        out.sky = glm::vec3(0.06f * ambient);
        out.ground = glm::vec3(0.04f * ambient);
        out.baked = kIndoorBaked * ambient;
        out.baked_lamps = kIndoorBakedLamps * baked_lamps;
        out.lamp_light = baked_lamps;
        return out;
    }
    glm::vec3 to_sun = sun_direction(hour);
    out.to_sun = to_sun;
    out.to_moon = moon_direction(hour, moon_phase);
    out.moon_lit = moon_lit(moon_phase);
    out.blood_moon = blood_moon;
    float s = to_sun.z;
    float elevation = std::asin(std::clamp(s, -1.0f, 1.0f)) * (180.0f / std::numbers::pi_v<float>);

    // Air mass (Kasten and Young) and a per-channel extinction: white-yellow overhead, orange to red at the horizon.
    float mass = elevation > -3.8f ? 1.0f / (std::max(s, 0.0f) + 0.50572f * std::pow(elevation + 6.07995f, -1.6364f))
                                   : 40.0f;
    glm::vec3 transmit = glm::exp(-glm::vec3(0.03f, 0.065f, 0.15f) * mass);
    glm::vec3 sun = 3.2f * transmit * smoothstep(-0.03f, 0.06f, s);

    // The open sky's light (render/atmosphere.h): the colour of the modelled sky itself, so it turns with the
    // sun (deep blue by day, paler and warmer round sunrise and sunset), a quarter less saturated (the clouds and the
    // light scattered more than once, which the model leaves out). The night's floor under it.
    glm::vec3 atmosphere = sky_light(to_sun).ambient;
    float grey = glm::dot(atmosphere, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    atmosphere = glm::mix(atmosphere, glm::vec3(grey), 0.25f);
    float night = 1.0f - smoothstep(-0.1f, 0.15f, s);
    glm::vec3 sky_night(0.03f, 0.045f, 0.09f);

    // By night the moon: a cool light as it is up and as much of it as is lit, rising steeply to the full moon (an
    // eighth of it at the new moon, the earthshine, so the night is never black), dimmed as the sun comes up; red
    // under the blood moon.
    float moon_up = smoothstep(0.0f, 0.25f, out.to_moon.z) * smoothstep(0.0f, -0.15f, s);
    float moon_strength = moon_up * (0.125f + 0.875f * out.moon_lit * out.moon_lit);
    glm::vec3 moon = glm::mix(kFullMoon, kBloodMoon, blood_moon) * moon_strength;
    // Without the moon, the starlight more (the eye's adaptation: a moonless night stays readable, if barely); with
    // it, the sky it lights. `ambient` scales the starlight: at 0 a moonless night is dark.
    glm::vec3 moon_sky = glm::mix(glm::vec3(0.03f, 0.04f, 0.07f), glm::vec3(0.06f, 0.012f, 0.01f), blood_moon);
    const glm::vec3 starlight = sky_night * (1.6f - 0.6f * moon_strength) * ambient;
    out.sky = atmosphere + (starlight + moon_sky * moon_strength) * night;

    // The baked colour (its ambient and the sun the builders put in it; its lamps' part) fades with the day, from the
    // sun 15 degrees up to 6 below the horizon, to what is left of it by night: a little more under the moon. The
    // darkness controls scale what is left by night.
    float baked_day = smoothstep(-0.1f, 0.25f, s);
    out.baked = glm::mix((kNightBaked + kMoonBaked * moon_strength) * ambient, 1.0f, baked_day);
    out.baked_lamps = glm::mix(kNightBakedLamps * baked_lamps, 1.0f, baked_day);
    out.lamp_light = baked_lamps;
    if (s >= 0.0f) {
        out.to_light = to_sun;
        out.light = sun;
    } else {
        out.to_light = out.to_moon;
        out.light = moon;
    }
    // The ground returns some of the sky and the sun.
    out.ground = out.sky * glm::vec3(0.30f, 0.28f, 0.24f) + sun * (0.06f * std::max(s, 0.0f));
    return out;
}

glm::vec3 cloud_light(const Daylight& d) {
    glm::vec3 sun = d.to_sun.z > 0.0f ? d.light : glm::vec3(0.0f);
    return d.sky * 1.5f + sun * 0.3f + glm::vec3(0.004f, 0.005f, 0.008f);
}

glm::vec3 classic_actor_light(const SceneLighting& l, const glm::vec3& n) {
    glm::vec3 baked(0.0f);
    float share = 1.0f;
    if (l.probe.w > 0.5f) {
        baked = glm::vec3(l.probe);
        share = l.probe.w - 1.0f;
    }
    const glm::vec3 dn(l.direction);
    glm::vec3 rgb = baked + glm::vec3(l.special_ambient) +
                    share * (glm::vec3(l.ambient) + glm::vec3(l.bounce) * std::max(glm::dot(n, dn), 0.0f) +
                             glm::vec3(l.directional) * std::max(glm::dot(n, -dn), 0.0f));
    return glm::clamp(rgb, glm::vec3(kClassicActorFloor), glm::vec3(1.0f));
}

SceneLighting classic_full_bright() {
    SceneLighting out;
    out.ambient = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    return out;
}

SceneLighting make_scene_lighting(const game::SceneLight& scene, std::span<const game::LightSource> lights,
                                  std::size_t dynamic, LightingMode mode, const Daylight& daylight, std::size_t carried,
                                  float carried_stacking) {
    SceneLighting out;
    out.ambient = glm::vec4(scene.ambient, 0.0f);
    out.special_ambient = glm::vec4(scene.special_ambient, 0.0f);
    out.directional = glm::vec4(scene.directional, 0.0f);
    out.direction = glm::vec4(scene.direction, 0.0f);
    out.bounce = glm::vec4(scene.bounce, 0.0f);
    out.vision = glm::vec4(scene.vision_lift, daylight.lamp_light);
    bool modern = mode == LightingMode::Modern;
    std::size_t n = modern ? std::min(lights.size(), kMaxSceneLights) : 0;
    for (std::size_t i = 0; i < n; ++i) {
        out.lights[i].position_radius = glm::vec4(lights[i].position, lights[i].radius);
        // The zone's lamps (after the dynamic lights) as strong as display.baked_lamp_light makes them.
        out.lights[i].color = glm::vec4(lights[i].color * (i < dynamic ? 1.0f : daylight.lamp_light), 0.0f);
    }
    out.params = glm::vec4(modern ? 1.0f : 0.0f, static_cast<float>(n), static_cast<float>(std::min(dynamic, n)), 0.0f);
    out.carried = glm::vec4(static_cast<float>(std::min({carried, dynamic, n})), std::clamp(carried_stacking, 0.0f, 1.0f),
                            0.0f, 0.0f);
    out.to_light = glm::vec4(daylight.to_light, 0.0f);
    out.light = glm::vec4(daylight.light, 0.0f);
    out.sky = glm::vec4(daylight.sky, daylight.baked);
    out.ground = glm::vec4(daylight.ground, daylight.baked_lamps);
    return out;
}

void tag_shadowed_lights(SceneLighting& lighting) {
    const auto shadowed = static_cast<std::size_t>(std::clamp(lighting.point_shadow_params.x, 0.0f,
                                                              static_cast<float>(kMaxPointShadows)));
    const auto count = static_cast<std::size_t>(std::clamp(lighting.params.y, 0.0f, static_cast<float>(kMaxSceneLights)));
    for (std::size_t i = 0; i < count; ++i) {
        GpuPointLight& l = lighting.lights[i];
        l.color.w = 0.0f;
        for (std::size_t e = 0; e < shadowed; ++e)
            if (same_shadow_light(lighting.point_shadow_light[e], l.position_radius)) {
                l.color.w = static_cast<float>(e + 1);
                break;
            }
    }
}

void set_shadows(SceneLighting& lighting, const ShadowCascades& cascades, std::uint32_t map_size,
                 const glm::vec3& camera, const glm::vec3& forward) {
    lighting.shadow_view_proj = cascades.view_proj;
    lighting.shadow_splits = cascades.splits;
    lighting.shadow_texel = cascades.texel_world;
    lighting.shadow_params = glm::vec4(1.0f, 1.0f / static_cast<float>(map_size), 0.0f, 0.0f);
    lighting.shadow_origin = glm::vec4(camera, 1.0f);
    lighting.shadow_forward = glm::vec4(forward, 0.0f);
}

void set_haze(SceneLighting& lighting, const glm::vec3& camera, float length, const glm::vec3& fog_color) {
    auto linear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    set_haze_linear(lighting, camera, length, glm::vec3(linear(fog_color.r), linear(fog_color.g), linear(fog_color.b)));
}

void set_haze_linear(SceneLighting& lighting, const glm::vec3& camera, float length, const glm::vec3& color) {
    lighting.haze = glm::vec4(1.0f / std::max(length, 1.0f), 1.0f / 600.0f, 1.0f, haze_glow(color));
    lighting.haze_color = glm::vec4(color, 0.0f);
    lighting.camera = glm::vec4(camera, 1.0f);
}

float haze_glow(const glm::vec3& haze_linear) {
    float luminance = glm::dot(haze_linear, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    return kHazeGlow * std::clamp(luminance / kHazeGlowFull, 0.0f, 1.0f);
}

glm::vec3 underwater_color(const Daylight& d) {
    glm::vec3 linear = (d.sky * 0.6f + d.light * 0.12f + glm::vec3(0.004f)) * glm::vec3(0.10f, 0.42f, 0.50f);
    auto display = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    };
    return {display(linear.r), display(linear.g), display(linear.b)};
}

void set_underwater(SceneLighting& lighting, const glm::vec3& camera, const glm::vec3& color) {
    set_haze(lighting, camera, kUnderwaterReach, color);
    lighting.haze.y = 0.0f; // as thick at every height
    lighting.haze.z = 2.0f; // under water: the absorption
    lighting.haze.w = 0.0f; // no glow round the sun
}

float extended_far_plane(float original, float multiplier) {
    return std::max(original, std::min(original * std::max(multiplier, 1.0f), kMaxViewDistance));
}

ModernView modern_view(const game::ViewDistance& view, float multiplier, float extend) {
    extend = std::clamp(extend, 0.0f, 1.0f);
    if (extend >= 1.0f) return modern_view(view, multiplier, true);
    ModernView near = modern_view(view, multiplier, false);
    if (extend <= 0.0f) return near;
    ModernView far = modern_view(view, multiplier, true);
    ModernView out = far;
    out.view.far_plane = glm::mix(near.view.far_plane, far.view.far_plane, extend);
    out.view.fog_start = glm::mix(near.view.fog_start, far.view.fog_start, extend);
    out.view.fog_end = glm::mix(near.view.fog_end, far.view.fog_end, extend);
    out.haze_length = glm::mix(near.haze_length, far.haze_length, extend);
    return out;
}

float zone_haze_length(const game::ViewDistance& view) {
    if (!view.fog_enabled || !(view.fog_density > 0.0f)) return 0.0f;
    // The original's exp(-(10 density (d - start) / (end - start))^2) (shaders/zone.vert fog_factor) is half clear
    // at start + sqrt(ln 2) (end - start) / (10 density); the haze's exp(-kHazeDepth (d / length)^2) at
    // length sqrt(ln 2 / kHazeDepth).
    constexpr float kLn2 = 0.69314718f;
    const float half = std::max(view.fog_start, 0.0f) +
                       std::sqrt(kLn2) * std::max(view.fog_end - view.fog_start, 1.0f) / (10.0f * view.fog_density);
    return std::max(half * std::sqrt(kHazeDepth / kLn2), 50.0f);
}

ModernView modern_view(const game::ViewDistance& view, float multiplier, bool extend) {
    ModernView out;
    out.view = view;
    // The haze is as thick as the zone's fog (zone_haze_length), never longer than the view; without fog, the view.
    const float zone = zone_haze_length(view);
    if (!extend) {
        const float reach = std::max(std::min(view.fog_end, view.far_plane), 50.0f);
        out.haze_length = zone > 0.0f ? zone : reach;
        return out;
    }
    float far = extended_far_plane(view.far_plane, multiplier);
    out.view.far_plane = far;
    out.view.fog_start = std::max(view.fog_start, 0.5f * far);
    out.view.fog_end = far;
    out.haze_length = zone > 0.0f ? std::min(zone, far) : far;
    return out;
}

FrameBuffers::FrameBuffers(VkContext& vk, VkDeviceSize size, VkBufferUsageFlags usage) : vk_(vk), size_(size) {
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        vk_check(vmaCreateBuffer(vk_.allocator(), &bi, &ai, &buffers_[i].buffer, &buffers_[i].allocation, &info),
                 "create per-frame buffer");
        buffers_[i].size = size;
        mapped_[i] = info.pMappedData;
        std::memset(mapped_[i], 0, static_cast<std::size_t>(size));
    }
}

FrameBuffers::~FrameBuffers() {
    for (auto& b : buffers_) b.destroy(vk_.allocator());
}

void FrameBuffers::write(std::uint32_t frame_slot, const void* data, VkDeviceSize size) {
    VkDeviceSize n = std::min(size, size_);
    std::memcpy(mapped_[frame_slot], data, static_cast<std::size_t>(n));
    vmaFlushAllocation(vk_.allocator(), buffers_[frame_slot].allocation, 0, n);
}

void set_screen(SceneLighting& lighting, float near_plane, float far_plane, std::uint32_t width, std::uint32_t height,
                double seconds) {
    float range = std::max(far_plane - near_plane, 1e-3f);
    lighting.screen = glm::vec4(near_plane / range, far_plane * near_plane / range,
                                1.0f / static_cast<float>(std::max(width, 1u)), 1.0f / static_cast<float>(std::max(height, 1u)));
    lighting.time = glm::vec4(static_cast<float>(std::fmod(seconds, 1024.0)), 0.0f, 0.0f, 0.0f);
}

} // namespace hailstone::render
