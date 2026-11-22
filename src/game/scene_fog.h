#pragma once

// The per-frame fog and scene light, as the original runs them each frame:
// RealRender_World -> CDisplay_UpdateSky (outdoor time types) or directly
// CDisplay_UpdateSkyWeatherAndFog -> CDisplay_ApplyWeatherFog ->
// CDisplay::SetFog -> CRender::SetFog (game/fog.h follows them exactly).
//
// SceneFog keeps what the original keeps between frames: the smoothed light
// level (0xac2de4), the fog colour CEverQuest +0x38e60..68, CDisplay's fog
// fields +0x134..+0x13c and its sky flag +0x2d38.

#include <array>
#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

#include "game/fog.h"
#include "game/view_distance.h"

namespace hailstone::game {

struct SceneFogInput {
    std::int32_t hour = 12;         // the sky clock: 1..24
    std::int32_t minute = 0;
    std::uint8_t camera_region = 0; // the region type at the camera (0xd1f3c0)
    bool infravision_innate = false; // profile +0x3248 != 0xff (the race's innate infravision)
    bool ultravision_innate = false; // +0x3264
    std::int32_t infravision_effect = 0; // TotalEffect(65): SPA 65 infravision
    std::int32_t ultravision_effect = 0; // TotalEffect(66): SPA 66 ultravision
    float infravision_strength = 1.0f;   // SkyWeatherInput::infravision_strength (Hailstone's own; 1 the original)
    float ultravision_strength = 1.0f;
    // The player actor's light level (ScaledAmbient); nullopt without an
    // actor (the original then uses 1).
    std::optional<float> actor_light;
    bool raining = false;
    bool lightning = false;
    int clip_plane = 15;
    // The game clock's steps this frame (core/step_clock.h, docs/mechanics.md "Frame rate"): the original's per-frame
    // steps (the light level's 0.01, the fog's) run once for each; 0 gives this frame's look without moving them.
    // Below 0: once (the original's code as written, once a drawn frame).
    int steps = -1;
};

struct SceneFogFrame {
    ViewDistance view;               // the render fog (CRender::SetFog) and the far plane
    std::uint32_t ambient = 0;       // scene graph +0xc8, 0xAARRGGBB
    std::uint32_t special_ambient = 0; // +0xcc
    bool sky = true;                 // CDisplay +0x2d38: draw the sky
    float light_level = 1.0f;        // the smoothed P
    glm::vec3 vision_lift{0.0f};     // SkyWeatherResult::vision_lift: infravision's or ultravision's raise
};

class SceneFog {
public:
    // A new zone: what OP_NewZone sent (Handle_OP_NewZone's fixes are applied
    // here). Keeps the light level, as the original's global does.
    void set_zone(const ZoneFogInput& zone, bool zone_sky);
    SceneFogFrame frame(const SceneFogInput& in);
    const ZoneFogInput& zone() const { return zone_; } // after the fixes

private:
    SceneFogFrame step(const SceneFogInput& in); // one frame of the original's
    ZoneFogInput zone_;   // after the fixes
    bool zone_sky_ = false;
    std::uint32_t light_level_ = 0xbf800000u; // -1: not set yet
    std::array<std::int32_t, 3> fog_colour_{};
    std::uint32_t display_start_ = 0, display_end_ = 0, display_density_ = 0;
    std::uint8_t sky_flag_ = 1;
};

} // namespace hailstone::game
