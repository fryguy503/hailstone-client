#pragma once

// The distant hills round an outdoor zone (Hailstone's own, the modern lighting; docs/graphics.md "Horizon hills"):
// rings of hills and mountains past the zone's edges, drawn behind everything where nothing else is
// (shaders/horizon.frag, ZoneRenderer::draw_horizon), so the land does not end in a blank haze. The original drew
// nothing past a zone's own geometry. This is the CPU half: what the zone's own terrain says about the land round it
// (where it lies, how high and rugged it is, what colour its ground and slopes are), and how strongly to draw it.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <glm/glm.hpp>

namespace hailstone::assets { struct Image; }

namespace hailstone::render {

// A terrain triangle as the zone renderer prepares it: its corners and its texture (an index into the colours given
// to horizon_land; any index past them has no colour).
struct HorizonTriangle {
    glm::vec3 a{0.0f}, b{0.0f}, c{0.0f};
    std::uint32_t texture = 0;
};

// The land round a zone, for shaders/horizon.frag. Not valid when the zone has too little level ground to say.
struct HorizonLand {
    bool valid = false;
    glm::vec2 centre{0.0f}; // the middle of the zone's level ground
    float radius = 0.0f;    // the first ring's distance from the centre: past the level ground's corners
    float ground = 0.0f;    // the level ground's median height: the hills' foot
    float relief = 0.0f;    // the level ground's spread in height (10th to 90th percentile), clamped
    glm::vec3 ground_color{0.0f}; // linear: the level ground's mean texture colour
    glm::vec3 slope_color{0.0f};  // linear: the slopes' (rock, cliffs)
    float seed = 0.0f;            // the zone's own hills (horizon_seed)
};

// Triangles at most this steep (the normal's z at least) are level ground; up to kHorizonSlope, slopes. Either way
// up: terrain winding differs between the classic and EQG formats.
inline constexpr float kHorizonLevel = 0.8f;
inline constexpr float kHorizonSlope = 0.3f;
// Too little level ground to place the hills by (square units): a zone of a few rooms.
inline constexpr float kHorizonMinArea = 250000.0f;
// The share of the level ground's area cut off each side when taking its extent (stray islands, a boat's dock far out).
inline constexpr float kHorizonExtentCut = 0.02f;
// How far past the level ground's corners the first ring stands.
inline constexpr float kHorizonMargin = 400.0f;
// The relief taken into account (a valley zone's cliffs; a flat plain's few bumps).
inline constexpr float kHorizonMinRelief = 40.0f;
inline constexpr float kHorizonMaxRelief = 1200.0f;
// The colours when no texture says (a muted green, a grey).
inline constexpr glm::vec3 kHorizonGround{0.10f, 0.13f, 0.06f};
inline constexpr glm::vec3 kHorizonRock{0.16f, 0.15f, 0.13f};

// The land round the zone from its terrain. `colors[i]` is texture i's mean linear colour, or nullopt when not known.
HorizonLand horizon_land(std::span<const HorizonTriangle> triangles, std::span<const std::optional<glm::vec3>> colors,
                         float seed);

// A texture's mean colour, linear (its texels are sRGB), weighted by alpha; nullopt for a format it cannot read
// (BC4, BC5, BC7) or a texture that is all clear. Reads the smallest level with at least 16 texels, a sample of them.
std::optional<glm::vec3> mean_texture_color(const assets::Image& image);

// A zone's seed for its hills, from its short name: the same hills every visit, different from zone to zone.
float horizon_seed(std::string_view zone);

// How strongly the hills are drawn, 0..1: none where the zone's fog ends by kHorizonFogNone (the classic distance,
// before the modern view's extension: nektulos' murk at 400, Freeport's walls at 450), all from kHorizonFogFull
// (East Commonlands' 800 nearly so); `extend` the modern view's reach (0 in lava or a thick-fog region, as the haze
// eases); fading under a storm (`overcast`, WeatherLook::storm), which would hide them.
inline constexpr float kHorizonFogNone = 350.0f;
inline constexpr float kHorizonFogFull = 900.0f;
float horizon_strength(float fog_end, float extend, float overcast);

} // namespace hailstone::render
