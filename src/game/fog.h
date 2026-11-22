#pragma once

// The original client's fog and per-frame scene light, exactly, checked
// against the original's vectors
// (tests/fog_vectors_test.cpp: CalcFogFadeMargin, SetFog,
// UpdateSkyWeatherAndFog).
//
//   CDisplay_CalcFogFadeMargin @0x48ac00   the margin SetFog takes off the far plane
//   CDisplay::SetFog @0x48ae80             distance fixes, the fade, the sky flag, CRender::SetFog's arguments
//   CDisplay_UpdateSkyWeatherAndFog @0x4942b0  per frame: ambient, light level, vision floors, fog colour
//   CDisplay_ApplyWeatherFog @0x518400     the fog distances by camera region
//
// The originals keep values on the x87 stack between steps; here they are
// doubles run through mechanics::Fpu at the FPU's precision (24-bit in game)
// and rounded to floats exactly where the original stores them. Floats come
// in and go out as their bits, so NaN payloads are kept.
//
// What draws: CRender::SetFog @0x10096640 (EQGraphicsDX9.dll) gets (on,
// (int)start, (int)end, density, 0xAARRGGBB). It sets linear vertex fog render
// states, but the vs_1_1 effects write oFog themselves (fog_factor below).

#include <array>
#include <cstdint>
#include <optional>

#include "game/mechanics/fpu.h"

namespace hailstone::game {

// The fog clip globals SetFog and the margin read: eqclient.ini ClipPlane
// (0xde0c00) and NewZone minclip / maxclip (0xddfb54 / 0xddfb58, after
// Handle_OP_NewZone's floors), as float bits.
struct FogClip {
    std::int32_t clip_plane = 15;
    std::uint32_t min_clip = 0; // float bits
    std::uint32_t max_clip = 0; // float bits
};

// CDisplay_CalcFogFadeMargin(fog_start): 0.15 (far - start), or 0.15 far when
// |far - start| < 0.001, far = minclip + ClipPlane 0.05 (maxclip - minclip);
// the unrounded x87 result (exact as a double at 24- and 53-bit precision).
double calc_fog_fade_margin(const mechanics::Fpu& fpu, std::uint32_t fog_start_bits, const FogClip& clip);

// The graphics DLL's CSky as SetFog asks it (the client has no CSky: its
// classic sky.s3d layers answer none of these, so the client passes none).
struct SkyFogObject {
    bool area_fog = false;                            // virtual +0x5c: inside an "AFG" region
    std::optional<std::uint32_t> area_fog_colour;     // virtual +0x64 (a pointer; null: keep the colour), 0xAARRGGBB
    double area_fog_density = 0.0;                    // virtual +0x6c, returned on the FPU (unrounded)
    std::uint32_t colour = 0;                         // virtual +0x2c: 0xAARRGGBB
};

struct SetFogInput {
    std::uint32_t on = 0;            // argument 1 (the low byte is tested; passed whole to CRender)
    std::uint32_t start = 0;         // float bits
    std::uint32_t end = 0;           // float bits
    std::uint32_t density = 0;       // float bits
    std::uint8_t red = 0, green = 0, blue = 0;
    bool world = true;               // CDisplay +4
    std::uint32_t yon = 0;           // CDisplay +8: the far clip (float bits)
    bool raining = false;            // CDisplay +0x158
    std::uint8_t sky_flag = 0;       // CDisplay +0x2d38 before the call
    std::uint8_t camera_region = 0;  // 0xd1f3c0: the region type at the camera
    FogClip clip;
    bool sky_option = true;          // eqclient.ini Sky (0xde0bb8)
    bool sky_name = false;           // NewZone SkyString2 non-empty
    bool zone_name = true;           // NewZone zone_short_name2 non-empty
    bool zone_sky = false;           // NewZone sky byte
    bool offline = false;            // 0xddf654
    std::optional<SkyFogObject> sky; // 0xd1f9dc
};

// CRender::SetFog's arguments.
struct RenderFog {
    std::uint32_t on = 0;
    std::int32_t start = 0;
    std::int32_t end = 0;
    std::uint32_t density = 0; // float bits
    std::uint32_t colour = 0;  // 0xAARRGGBB
};

struct SetFogResult {
    // CDisplay +0x130..+0x142 (nullopt: not written).
    std::optional<std::uint8_t> fog_on;
    std::optional<std::uint32_t> fog_start, fog_end, fog_density; // float bits
    std::optional<std::uint8_t> red, green, blue;
    std::uint8_t sky_flag = 0;          // CDisplay +0x2d38 after the call
    std::optional<RenderFog> render;    // nullopt: no world, nothing happened
    double start = 0.0, end = 0.0;      // the distances before __ftol2_sse
};

SetFogResult set_fog(const mechanics::Fpu& fpu, const SetFogInput& in);

// CDisplay_UpdateSkyWeatherAndFog's inputs.
struct SkyWeatherInput {
    bool world = true;               // CDisplay +0x2d04
    bool lightning = false;          // the weather manager's +9: a lightning flash
    std::uint8_t camera_region = 0;  // 0xd1f3c0
    std::uint8_t zone_type = 0;      // __ZoneType (NewZone time_type): 1, 2, 5 outdoor; 4 half lit
    std::int32_t hour = 12;          // CDisplay::GetSkyTime: 1..24
    std::int32_t minute = 0;
    std::int32_t infravision_innate = 0xff; // profile +0x3248 (0xff: none)
    std::int32_t ultravision_innate = 0xff; // profile +0x3264
    std::int32_t infravision_effect = 0;    // TotalEffect(65, 1, 0, 1, 1): asked only without the innate
    std::int32_t ultravision_effect = 0;    // TotalEffect(66, ...)
    std::optional<std::uint32_t> sky_ambient; // the CSky's +0x28 colour, 0xAARRGGBB (the client has none)
    std::optional<double> actor_light;     // the controlled player's actor +0xf0 (GetScaledAmbient), unrounded
    std::uint32_t light_level = 0xbf800000u; // 0xac2de4 (float bits; -1: not set yet)
    std::array<std::int32_t, 3> infravision_floor{65, 50, 50};  // 0xddf808 (/255)
    std::array<std::int32_t, 3> ultravision_floor{80, 80, 120}; // 0xddf814
    // Not the original's: how strong infravision's and ultravision's floor is (display.infravision_strength /
    // ultravision_strength, 0..1), eased from the plain 0.08 floor (0, as without it) to the floor above (1, the
    // original's, computed exactly as it does).
    double infravision_strength = 1.0;
    double ultravision_strength = 1.0;
    std::array<std::uint8_t, 3> zone_fog_colour{};  // NewZone fog_red/green/blue[0]
    std::array<std::int32_t, 3> fog_colour{};       // CEverQuest +0x38e60/64/68 before
    std::uint32_t fog_start = 0, fog_end = 0, fog_density = 0; // CDisplay +0x134/+0x138/+0x13c (float bits)
    // The hour tables 0xaf3260 / 0xaf32c0 / 0xaf3320 (float bits), red, green, blue.
    std::array<std::array<std::uint32_t, 24>, 3> hour_ambient{};
};

struct SkyWeatherResult {
    bool asked_infravision = false;      // TotalEffect(65) was called
    bool asked_ultravision = false;      // TotalEffect(66)
    bool apply_weather_fog = false;      // CDisplay_ApplyWeatherFog(0) was called
    // Scene graph +0xcc (special ambient), then +0xc8 (ambient), 0xAARRGGBB
    // (the lightning path sets only the ambient, white).
    std::optional<std::uint32_t> special_ambient;
    std::optional<std::uint32_t> ambient;
    // The lightning path's SetFog(1, +0x134, +0x138, +0x13c, r, g, b).
    std::optional<std::array<std::uint8_t, 3>> set_fog_colour;
    std::uint32_t light_level = 0;       // 0xac2de4 after (float bits)
    std::array<std::int32_t, 3> fog_colour{}; // CEverQuest +0x38e60/64/68 after
    std::optional<std::uint32_t> ambient_mean; // CDisplay +0xc (float bits)
    // Not the original's: how far the infravision or ultravision floor raised each ambient channel above where the
    // plain 0.08 floor leaves it (0 without either), for the modern lighting, which has its own sky instead of the
    // ambient (render/scene_lighting.h SceneLighting::vision).
    std::array<float, 3> vision_lift{};
};

SkyWeatherResult update_sky_weather_and_fog(const mechanics::Fpu& fpu, const SkyWeatherInput& in);

// The game's own hour tables (assets::sky_ambient's), as float bits.
std::array<std::array<std::uint32_t, 24>, 3> game_hour_ambient();

// CDisplay_ApplyWeatherFog @0x518400: SetFog's arguments for the camera
// region: water (5) 30..800, regions 6, 7 and 8 0..50, 9 90..100, 10 0..400,
// with the computed fog colour; otherwise the zone's fog clips, and for time
// types other than 1, 2 and 5 the zone's own fog colour (outdoor zones keep
// the computed one; while raining they keep CDisplay's stored distances).
// Each colour channel is clamped to 1..255.
struct WeatherFogInput {
    std::uint8_t camera_region = 0;
    std::array<std::int32_t, 3> fog_colour{}; // CEverQuest +0x38e60/64/68
    std::uint8_t zone_type = 0;               // NewZone time_type (0xddfaf8)
    bool raining = false;                     // CDisplay +0x158
    std::uint32_t display_start = 0, display_end = 0; // CDisplay +0x134 / +0x138 (float bits)
    std::array<std::uint8_t, 3> zone_fog_colour{};
    std::uint32_t zone_fog_min = 0, zone_fog_max = 0; // NewZone fog_minclip[0] / fog_maxclip[0] (float bits)
    bool fog_enabled = true;                  // NewZone ztype != 0
    std::uint32_t density = 0;                // NewZone fog_density (float bits)
};

struct SetFogArgs {
    std::uint32_t on = 0;
    std::uint32_t start = 0, end = 0, density = 0; // float bits
    std::uint8_t red = 0, green = 0, blue = 0;
};

SetFogArgs apply_weather_fog(const WeatherFogInput& in);

// The vs_1_1 effects' fog (RegionC1 and the other region, object and actor
// effects): f = clamp((dist - FogStart) FogRangeInv fFogRange, 0, fFogRange)
// with FogRangeInv = 1 / (end - start) and fFogRange = 10; oFog =
// exp(-(f FogDensity)^2) per vertex (1: no fog). CRender::SetFog makes end
// at least start + 1. The shaders compute the same (shaders/*.vert).
inline constexpr float kFogRange = 10.0f;
float fog_factor(float distance, std::int32_t start, std::int32_t end, float density);

} // namespace hailstone::game
