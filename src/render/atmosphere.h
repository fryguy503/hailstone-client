#pragma once

// The modern look's atmosphere on the CPU (this client's own, not the original's; docs/graphics.md "Sky", "Haze",
// "Colour grade"): the sky model shaders/atmosphere.frag draws, sampled to light the world with the sky's own colour;
// the haze eased between places and weathers; the storm's fog band; and the frame's colour grade.

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "game/view_distance.h"

namespace hailstone::render {

struct Daylight;

// The light of the sky in direction `dir` (z up, unit) with the sun at `to_sun`, linear, as shaders/atmosphere.frag's
// scatter() computes it (single Rayleigh and Mie scattering; `mie_scale` the haze in the air, 1 normal). Below the
// horizon the ray is taken just above it.
glm::vec3 sky_radiance(const glm::vec3& dir, const glm::vec3& to_sun, float mie_scale = 1.0f);

// The sky sampled for the lighting: `ambient` the light the open sky gives an upward-facing surface (cosine-weighted
// over the hemisphere, scaled to the modern lighting's units: kSkyAmbientScale), `horizon` its colour a few degrees
// over the horizon averaged round it (the haze's colour), `horizon_sun` that colour towards the sun's side.
inline constexpr float kSkyAmbientScale = 2.9f;
struct SkyLight {
    glm::vec3 ambient{0.0f};
    glm::vec3 horizon{0.0f};
    glm::vec3 horizon_sun{0.0f};
};
// The last result is kept: the frame asks for the same sun several times.
SkyLight sky_light(const glm::vec3& to_sun, float mie_scale = 1.0f);

// Display (sRGB) values to linear, as shaders/lighting.glsl to_linear.
glm::vec3 linear_color(const glm::vec3& display);

// ---- The haze between places ----

// What the haze would be this frame (render::set_haze's inputs) and how far the modern view reaches (0 the original's
// far plane, 1 the extended one).
struct HazeTarget {
    glm::vec3 color{0.0f}; // linear
    float length = 1000.0f;
    float extend = 1.0f;
};

// Eases the haze towards its target, so walking into a lava cave, out of a thick-fog region or through a weather
// change does not pop: the colour over about kHazeEaseSeconds, the length and reach as fast. A zone-in (reset) or the
// first frame takes the target at once.
inline constexpr float kHazeEaseSeconds = 1.5f;
class HazeBlend {
public:
    void reset() { valid_ = false; }
    const HazeTarget& update(const HazeTarget& target, float dt);
    const HazeTarget& current() const { return current_; }

private:
    HazeTarget current_;
    bool valid_ = false;
};

// The haze's colour for an outdoor zone under its sky (linear): the zone's fog colour (`fog`, SetFog's, linear) drawn
// towards the sky's own colour at the horizon (`horizon`), so the land fades into the sky behind it: by `sky_share`
// (kHazeSkyShare under a drawn sky, kHazeSkyShareNoSky without one), less in a thick fog (the fog's end `fog_end`
// under kClearFogEnd; the zone's builders' murk stays theirs). Never brighter than the fog by more than the sky allows
// at night: both are dark then.
inline constexpr float kHazeSkyShare = 0.7f;
inline constexpr float kHazeSkyShareNoSky = 0.35f;
inline constexpr float kClearFogEnd = 1200.0f;
glm::vec3 haze_sky_color(const glm::vec3& fog, const glm::vec3& horizon, bool sky_drawn, float fog_end);

// The weather's fog band (this client's own): the haze under rain or snow. `overcast` (WeatherLook::storm, 0..1)
// pulls it in by up to kStormHazeReach and greys its colour (cooler and darker in rain, whiter in snow: `snow`
// 0..1 the share of the precipitation that is snow); a lightning strike (`flash`) lights it.
inline constexpr float kStormHazeReach = 0.5f;
void weather_haze(HazeTarget& haze, float overcast, float snow);
// The lightning's light in the haze (linear), not eased: `flash` WeatherLook::flash.
glm::vec3 lightning_haze(const glm::vec3& color, float flash, float overcast);

// The frame's haze target in the modern lighting, for the client and the viewer alike: the original's view this frame
// (`original`, SceneFogFrame::view: its fog colour and distances), drawn out by `multiplier` (display.view_distance)
// when `extend` (the sky flag: not under water, in lava or a thick-fog region); outdoors (time types 1, 2, 5) its colour
// drawn towards the sky's (haze_sky_color, `sky_drawn` a sky drawn, the sun at `to_sun`); then the weather's band
// (`overcast` WeatherLook::storm, `snow` WeatherLook::snowing). `in_the_sky` (render::horizon_open_below: a zone
// floating in the sky, its sky all round it): the sky's own colour at the horizon, whatever the zone's fog (Plane of
// Sky's is black in PEQ's zone table, which turned its far islands and the sky under it black).
HazeTarget haze_target(const game::ViewDistance& original, float multiplier, bool extend, bool outdoor, bool sky_drawn,
                       const glm::vec3& to_sun, float overcast, float snow, bool in_the_sky = false);

// What shows past the painted dome's foot under a zone floating in the sky (render::horizon_open_below) in the
// modern lighting, the sun at `to_sun` (Daylight::to_sun): the open sky overhead's colour (sky_radiance straight up),
// so the gap under the dome is sky, not the zone's fog (Plane of Sky's black). Linear.
glm::vec3 open_sky_below(const glm::vec3& to_sun);

// ---- The colour grade ----

// The frame's grade in the post pass (render::SceneOptions::mood; display.color_grade): `tint` multiplies the linear
// scene before the tonemap (its luminance 1), `saturation` scales the chroma after it. A subtle look by the time of day
// (warm low sun, cool night with less colour), the weather (greyer in a storm) and the zone's own fog hue.
struct ColorGrade {
    glm::vec3 tint{1.0f};
    float saturation = 1.0f;
};
// `fog` the zone's fog colour (linear), `outdoor` the zone's time type, `overcast` the storm.
ColorGrade color_grade(const Daylight& daylight, bool outdoor, const glm::vec3& fog, float overcast);

} // namespace hailstone::render
