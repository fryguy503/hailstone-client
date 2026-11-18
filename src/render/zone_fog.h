#pragma once

// The fog bank past the zone's edge (docs/graphics.md, "Zone edge fog"): in the modern lighting, outdoors, a fog
// that lies on the ground past the zone's own terrain, so the world does not stop at the zone line nor show the land
// round it up close; the horizon hills' mountains (render/horizon.h) rise out of it far off. Hailstone's own; the
// original drew nothing past a zone's own geometry and hid that in its fog wall.
//
// It is volumetric: shaders/zone_fog.frag marches each pixel's line of sight from the camera to what it shows (to
// kZoneFogReach for the sky) through a density of
//     kZoneFogDensity x ramp(distance past the zone's terrain) x layer(height over the fog's floor)
// (ramp: 0 at the zone's terrain, 1 kZoneFogRamp past it; layer: 1 up to kZoneFogTop over the floor, 0 by
// kZoneFogSoft past it, both smooth): a bank with a top, out of which hills taller than it rise. It mixes the haze's colour (with the sun's
// glow in it, as shaders/lighting.glsl apply_haze) over the scene by what is left of the light. Inside the zone it is
// clear; across open ground at a zone line it thickens until nothing shows; looking up, the sky stays clear.
//
// The distance past the zone and the fog's floor (the zone's edge's height carried out, HorizonField::edge) come
// from the land's field (render::HorizonField), on its squares: zone_fog_field packs them for the GPU.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::render {

struct HorizonField;

// The fog reaches full density this far past the zone's terrain.
inline constexpr float kZoneFogRamp = 1400.0f;
// The bank's top over its floor, and how far either side of it it thins out.
inline constexpr float kZoneFogTop = 250.0f;
inline constexpr float kZoneFogSoft = 100.0f;
// The shader breaks the bank up round the mean these functions give: its top rises and falls by up to kZoneFogSwell
// over broad swells, its thickness varies in patches (0.45 to 1.55 times), both drifting slowly with the time.
inline constexpr float kZoneFogSwell = 100.0f;
// Its optical depth per unit at full density on its floor: through 1000 units of it, 8 % of the light is left.
inline constexpr float kZoneFogDensity = 0.0025f;
// Its colour: the haze's (SceneLighting::haze_color) this many times as bright, a lighter mist than the far air.
inline constexpr float kZoneFogLift = 1.35f;
// The sky's line of sight is marched this far.
inline constexpr float kZoneFogReach = 16000.0f;
// The field's texels hold the distance past the zone in steps of this many units (0..255: past kZoneFogRamp the
// fog is full, so 4080 is enough).
inline constexpr float kZoneFogDistanceStep = 16.0f;

// The fog's field: per square of the land's grid, RGBA8 (R in the low byte): R the distance past the zone's terrain
// (in kZoneFogDistanceStep units, 0 on the zone's terrain and in a pocket it closes round), G the fog's floor
// (floor_lo + G / 255 x floor_range), B and A 0.
struct ZoneFogField {
    glm::vec2 lo{0.0f};
    float cell = 64.0f;
    int nx = 0, ny = 0;
    float floor_lo = 0.0f, floor_range = 1.0f;
    std::vector<std::uint32_t> texels;
    bool valid() const { return nx > 0 && ny > 0 && texels.size() == static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny); }

    // Bilinear, as the GPU samples it (clamped at the grid's edge); `p` past the grid adds its distance to it.
    float distance_at(glm::vec2 p) const;
    float floor_at(glm::vec2 p) const;
};

// Empty (not valid) when the field is not.
ZoneFogField zone_fog_field(const HorizonField& field);

// Round the camera, which is always in the zone, there is never fog, whatever the field says: none within
// kZoneFogClear of it, full by kZoneFogClearFade. And where the field has the camera past the zone's terrain (ground
// the land's field does not count, such as East Freeport's by its North Ro gate, which is placed objects), the fog is
// pushed out by that much and its floor kept under the camera's feet.
inline constexpr float kZoneFogClear = 300.0f;
inline constexpr float kZoneFogClearFade = 900.0f;

// The density at `p` (per unit) without the camera's rules (the bank as it lies).
float zone_fog_density(const ZoneFogField& field, glm::vec3 p);
// The density at `p` (per unit) as zone_fog.frag has it, seen from `eye` (`eye_past`: the field's distance past the
// zone's terrain at the eye, ZoneFogField::distance_at).
float zone_fog_density(const ZoneFogField& field, glm::vec3 p, glm::vec3 eye, float eye_past);
// The share of the light from `to` that reaches `from` (the eye) through the fog, marched as zone_fog.frag marches
// it (kZoneFogSteps, without its dither).
inline constexpr int kZoneFogSteps = 40;
float zone_fog_transmittance(const ZoneFogField& field, glm::vec3 from, glm::vec3 to);

// What the fog pass needs each frame (VkContext::set_zone_fog): the view as world vectors, the haze's colour and
// the sun's glow in it.
struct ZoneFogView {
    glm::vec3 camera{0.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f}, up{0.0f, 0.0f, 1.0f}, forward{0.0f, 1.0f, 0.0f};
    glm::vec3 color{0.0f};   // linear: the haze's (SceneLighting::haze_color)
    glm::vec3 glow{0.0f};    // linear: the sun's light x its glow in the haze x 0.25 (as apply_haze)
    glm::vec3 to_sun{0.0f, 0.0f, 1.0f};
    float reach = kZoneFogReach; // the sky's march (no further than the far plane)
    float time = 0.0f;           // seconds: the mist drifts with it
    float camera_past = 0.0f;    // the field's distance past the zone's terrain at the camera (distance_at)
};

} // namespace hailstone::render
