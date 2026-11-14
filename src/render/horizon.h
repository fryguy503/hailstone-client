#pragma once

// The land round an outdoor zone (Hailstone's own, the modern lighting; docs/graphics.md "Horizon hills"): terrain past
// the zone's edges, out to about the modern view's reach, so the land does not end in a blank haze. The original drew
// nothing past a zone's own geometry. This is the CPU half: what the zone's own terrain says about the land round it
// (where it lies, how high and rugged it is), and the mesh, which ZoneRenderer::prepare adds to the zone's geometry
// under the zone's own ground, rock and water materials, so it is shaded, lit and hazed as the zone is.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::render {

// A terrain triangle as the zone renderer prepares it: its corners, and whether it is a water or lava surface.
struct HorizonTriangle {
    glm::vec3 a{0.0f}, b{0.0f}, c{0.0f};
    bool liquid = false;
};

// What the zone's terrain says about the land round it. Not valid when the zone has too little level ground to say.
struct HorizonLand {
    bool valid = false;
    glm::vec2 lo{0.0f}, hi{0.0f}; // the level ground's extent (kHorizonExtentCut off each side)
    float ground = 0.0f;          // the level ground's median height
    float relief = 0.0f;          // the level ground's spread in height (10th to 90th percentile), clamped
    float seed = 0.0f;            // the zone's own hills (horizon_seed)
};

// Triangles at most this steep (the normal's z at least) are level ground; walls and slopes do not count. Either way
// up: terrain winding differs between the classic and EQG formats.
inline constexpr float kHorizonLevel = 0.8f;
// Too little level ground to place the land by (square units): a zone of a few rooms.
inline constexpr float kHorizonMinArea = 250000.0f;
// The share of the level ground's area cut off each side when taking its extent (stray islands, a boat's dock far out).
inline constexpr float kHorizonExtentCut = 0.02f;
// The relief taken into account (a valley zone's cliffs; a flat plain's few bumps).
inline constexpr float kHorizonMinRelief = 40.0f;
inline constexpr float kHorizonMaxRelief = 1200.0f;

// The land round the zone from its terrain (the liquid triangles do not count).
HorizonLand horizon_land(std::span<const HorizonTriangle> triangles, float seed);

// A zone's seed for its land, from its short name: the same hills every visit, different from zone to zone.
float horizon_seed(std::string_view zone);

// The land's mesh. The zone's terrain is laid on a grid of kHorizonCell squares (terrain anywhere in a square covers
// it; geometry further than kHorizonStray past the level ground is left out). Past the covered squares (and in the
// pockets they close round: a gap between the zone's walls, a hole in it) the land starts at the zone's edge, the height of its terrain there (a wall's top, the open ground at a zone line), and turns over kHorizonBlend
// into rolling hills and then, further out, mountains, as rugged as the zone's relief says; where the zone's edge is
// water, the sea, with land rising from it far out. Its vertices in covered squares are tucked kHorizonTuck under the
// zone's terrain there, and those in the empty squares beside them just under the lowest of it (a wall's foot), so
// the land meets the zone's edge with no gap. It reaches kHorizonReach past the grid, its
// squares kHorizonCell across over the grid (more for a big zone) and growing by kHorizonGrowth a step past it.
inline constexpr float kHorizonCell = 64.0f;
inline constexpr float kHorizonStray = 4000.0f;
inline constexpr float kHorizonBlend = 1200.0f;
inline constexpr float kHorizonTuck = 40.0f;
inline constexpr float kHorizonReach = 10000.0f;
inline constexpr float kHorizonGrowth = 1.12f;
// The steepest land drawn with the zone's ground material (the normal's z); steeper is rock.
inline constexpr float kHorizonRock = 0.8f;
// How far under the water's surface the sea's floor lies, past the shore.
inline constexpr float kHorizonSeaDepth = 250.0f;

struct HorizonMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<float> shade; // per vertex, 0..1: what is left of the light (woods and valleys are darker)
    // Triangles (index triples into positions), counter-clockwise seen from above: the zone's ground material, its
    // rock (slopes and peaks), and its water (a flat surface at water_level).
    std::vector<std::uint32_t> ground, rock, water;
    float water_level = 0.0f;
    bool empty() const { return ground.empty() && rock.empty() && water.empty(); }
};

// The land round the zone; empty when `land` is not valid.
HorizonMesh horizon_mesh(std::span<const HorizonTriangle> triangles, const HorizonLand& land);

// The squares kHorizonFootprintCell across that triangles cover (along their edges and at the squares' centres inside
// them), as horizon_cell keys, sorted, each once: a neighbour zone's geometry, where the land round the current zone
// is not drawn (ZoneRenderer::set_distant_hidden).
inline constexpr float kHorizonFootprintCell = 64.0f;
std::int64_t horizon_cell(glm::vec2 p);
std::vector<std::int64_t> horizon_footprint(std::span<const HorizonTriangle> triangles);

} // namespace hailstone::render
