#pragma once

// The land round an outdoor zone (Hailstone's own, the modern lighting; docs/graphics.md "Horizon hills"): terrain past
// the zone's edges, out to about the modern view's reach, so the land does not end in a blank haze. The original drew
// nothing past a zone's own geometry. This is the CPU half: what the zone's own terrain says about the land round it
// (where it lies, how high and rugged it is) and the mesh, which ZoneRenderer::prepare adds to the zone's geometry under the zone's own ground, rock and water
// materials, so it is shaded, lit and hazed as the zone is.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
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

// A zone that floats in the sky, with open sky under its ground (by its short name, any case): Plane of Sky
// (airplane), Plane of Air (poair). No land stands round it and no fog lies past its edge (they would be ground where
// there is none), and in the modern lighting its sky is its own painted dome, clouds below it, not the atmosphere,
// which goes into the haze below the horizon. Its sky is the plain one (sky 1 in PEQ's zone table), so the server
// does not say so; the list is the zones' own design (docs/unknowns.md: others may belong in it).
bool horizon_open_below(std::string_view zone);

// The zone's terrain is laid on a grid of kHorizonCell squares (terrain anywhere in a square covers it; geometry
// further than kHorizonStray past the level ground is left out). Past the covered squares (and in the pockets they
// close round: a gap between the zone's walls, a hole in it) the land starts at the zone's edge, the height of its
// terrain there (a wall's top, the open ground at a zone line), and turns over kHorizonBlend into rolling hills (they
// grow from kHorizonHillsFrom to kHorizonHillsFull past the edge) and then, further out, mountains (from
// kHorizonMountainsFrom to kHorizonMountainsFull), as rugged as the zone's relief says, so it rises gently by the zone
// and the steep ground stands far off; where the zone's edge is water, the sea, with
// land rising from it far out. Its vertices in covered squares are tucked kHorizonTuck under the zone's terrain there,
// and those in the empty squares beside them just under the lowest of it (a wall's foot), so the land meets the zone's
// edge with no gap. Its squares are kHorizonCell across over the grid and kHorizonFine past it (more for a big zone:
// at most kHorizonMaxFine of them across), then grow by kHorizonGrowth a step out to kHorizonReach past the grid.
inline constexpr float kHorizonCell = 64.0f;
inline constexpr float kHorizonStray = 4000.0f;
inline constexpr float kHorizonBlend = 2500.0f;
inline constexpr float kHorizonHillsFrom = 300.0f;
inline constexpr float kHorizonHillsFull = 3000.0f;
inline constexpr float kHorizonMountainsFrom = 2500.0f;
inline constexpr float kHorizonMountainsFull = 8000.0f;
inline constexpr float kHorizonTuck = 40.0f;
inline constexpr float kHorizonReach = 10000.0f;
inline constexpr float kHorizonFine = 2500.0f;
inline constexpr int kHorizonMaxFine = 256;
inline constexpr float kHorizonGrowth = 1.12f;
// The steepest land drawn with the zone's ground material (the normal's z); steeper is rock.
inline constexpr float kHorizonRock = 0.8f;
// How far under the water's surface the sea's floor lies, past the shore.
inline constexpr float kHorizonSeaDepth = 250.0f;

// The land's own hills are a generated terrain: warped ridged noise (land_point), worn by water and slumping
// (horizon_terrain), the known ground (the zone's edge, the sea) held where it is. A
// grid over the land: how far the wear moved the ground (`delta`, units, added to the land's height) and how much water
// ran over it (`wet`, 0..1: the valleys and runnels, where the woods gather).
struct HorizonTerrain {
    glm::vec2 lo{0.0f};
    float cell = 64.0f;
    int nx = 0, ny = 0;
    std::vector<float> delta, wet;
    float worn_lo = 0.0f, worn_hi = 0.0f; // the deepest the wear cut, the highest it built (for the log)
    bool valid() const {
        return nx > 1 && ny > 1 && delta.size() == static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) &&
               wet.size() == delta.size();
    }
    float sample(const std::vector<float>& layer, glm::vec2 p) const;
};

// What the land is built from: the zone's terrain on the grid (horizon_field), and the mesh's lines.
struct HorizonField {
    HorizonLand land;
    glm::vec2 lo{0.0f};
    float cell = kHorizonCell;
    int nx = 0, ny = 0;
    std::vector<float> zmin, zmax, water;
    std::vector<std::uint8_t> dry, wet; // terrain, and a water surface, in the square
    std::vector<std::uint8_t> wet_middle; // a water surface over the square's middle (not only along its side)
    std::vector<std::uint8_t> open;     // an empty square the grid's border reaches without crossing terrain
    std::vector<float> dist;            // to the nearest covered square, in squares (empty ones)
    std::vector<float> edge, sea;       // the zone's edge's height there, and whether it is water (0..1)
    std::vector<float> under;           // a covered square's, or one beside it: how high the land is tucked
    float water_level = 0.0f;           // the median of the zone's water surfaces
    bool has_water = false;
    std::vector<float> xs, ys;          // the mesh's lines
    // The wear of the land's hills once horizon_terrain has run (null before: the hills unworn).
    std::shared_ptr<const HorizonTerrain> terrain;

    bool valid() const { return land.valid && nx > 0 && xs.size() > 1 && ys.size() > 1; }
    std::size_t at(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x);
    }
    bool covered(std::size_t i) const { return dry[i] || wet[i]; }
    // The square of the grid holding `p`, clamped to it; `extra` the distance from p to the grid when it lies outside.
    std::size_t square(glm::vec2 p, float& extra) const;
    // Whether `p` lies in a covered square of the grid (the zone's own terrain is there; the land is tucked under it).
    bool covers(glm::vec2 p) const;
    // The mesh's square holding `p` (row x columns + column), -1 outside the mesh.
    std::int64_t mesh_square(glm::vec2 p) const;
};

// The field of the zone's terrain; not valid when `land` is not.
HorizonField horizon_field(std::span<const HorizonTriangle> triangles, const HorizonLand& land);

// How wide a stretch of the zone's edge must be water to be a shore the sea goes on from (horizon_field), and how
// far in from the edge its water may lie behind a wall or a quay.
inline constexpr float kHorizonShoreWidth = 1024.0f;
inline constexpr float kHorizonShoreWall = 192.0f;
// The land under the sea is darker with the depth, down to this share of its light this far under the surface.
inline constexpr float kHorizonSeaFloorShade = 0.3f;
inline constexpr float kHorizonSeaFloorDark = 150.0f;
// Wears the land: its heights on a grid (at least kHorizonCell, at most kHorizonTerrainMax squares a side) worn by
// kHorizonDroplets droplets a square of water running downhill (Beyer's particle erosion), each at most
// kHorizonDropletSteps squares, and a few passes of slumping where it is steeper than the rock holds, each square only
// as much as it is free: none on the zone or under the sea, all of it
// kHorizonTerrainFree past the zone's edge; at most kHorizonTerrainMaxCut down and kHorizonTerrainMaxFill up.
// Deterministic: the same field gives the same terrain (its layers on the cache's steps, so a terrain read
// back from the cache is the one generated).
inline constexpr int kHorizonTerrainMax = 512;
inline constexpr float kHorizonDroplets = 1.0f;
inline constexpr int kHorizonDropletSteps = 48;
inline constexpr float kHorizonTerrainFree = 1500.0f;
inline constexpr float kHorizonTerrainMaxCut = 384.0f;
inline constexpr float kHorizonTerrainMaxFill = 192.0f;
HorizonTerrain horizon_terrain(const HorizonField& field);
// The terrain's cache (cache/terrain/<server>/<zone>.bin, ZoneRenderOptions::terrain_cache): `key` is what it was worn
// from (horizon_terrain_key: the field's grid and heights, the generator's version), so
// a file worn from other ground, of another version or damaged is refused and the terrain worn again.
std::uint64_t horizon_terrain_key(const HorizonField& field);
std::vector<std::byte> write_horizon_terrain(const HorizonTerrain& terrain, std::uint64_t key);
std::optional<HorizonTerrain> read_horizon_terrain(std::span<const std::byte> data, std::uint64_t key);

// The land's height at `p` (as the mesh's vertices have it).
float horizon_height(const HorizonField& field, glm::vec2 p);

struct HorizonMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<float> shade;  // per vertex, 0..1: what is left of the light (woods and valleys are darker)
    // Per vertex: how much rock (x: slopes, peaks) and sand (y: by the water) cover the ground (zone.frag kLandSplat).
    std::vector<glm::vec2> splat;
    // Triangles (index triples into positions), counter-clockwise seen from above: the zone's ground material (its
    // rock splatted in) and its water (a flat surface at water_level).
    std::vector<std::uint32_t> ground, water;
    float water_level = 0.0f;
    bool empty() const { return ground.empty() && water.empty(); }
};

// The land round the zone; empty when the field is not valid.
HorizonMesh horizon_mesh(const HorizonField& field);

// The land's ground near the zone's edge that grows the zone's grass (assets::FloraExtraGround; Hailstone's own), so
// the lawn does not stop in a line where the zone's ground ends: its ground triangles whose middle lies in an open
// square at most kHorizonLawnReach past the zone's terrain (not those tucked under it, nor in a pocket it closes
// round), mostly neither rock nor sand, and not on the sea. Three corners each.
inline constexpr float kHorizonLawnReach = 300.0f;
std::vector<glm::vec3> horizon_lawn_ground(const HorizonField& field, const HorizonMesh& mesh,
                                           float reach = kHorizonLawnReach);
// The land round the zone from its terrain alone.
HorizonMesh horizon_mesh(std::span<const HorizonTriangle> triangles, const HorizonLand& land);

// The land's trees (Hailstone's own): far trees (render/tree_impostor.h) standing on the land's woods, the darker
// patches its shade paints (thick there, a lone one here and there elsewhere), from kHorizonTreeFrom past the zone's
// edge out to kHorizonTreeReach, fewer past kHorizonTreeThin (as kHorizonTreeThin over the distance; the haze takes
// them there). Not on the sea, the mountains' rock, slopes steeper than kHorizonTreeSlope (the normal's z), nor
// in the zone's pockets.
// Each stands on the land's surface as its mesh draws it, a little sunk so a slope shows no gap under it. At most
// kHorizonTreeMax, the nearest kept.
inline constexpr float kHorizonTreeFrom = 256.0f;
inline constexpr float kHorizonTreeReach = 7000.0f;
inline constexpr float kHorizonTreeThin = 2500.0f;
inline constexpr float kHorizonTreeSlope = 0.85f;
inline constexpr std::size_t kHorizonTreeMax = 40000;
inline constexpr float kHorizonTreeSink = 2.0f;
struct HorizonTree {
    glm::vec3 foot{0.0f};
    float yaw = 0.0f;   // radians round z
    float scale = 1.0f; // 0.8 .. 1.2 of the tree's own size
    float pick = 0.0f;  // 0..1, the same for the same spot: which of the zone's trees stands there
};
// `woods` and `open`: how far apart the trees stand in the woods and elsewhere (units; the zone's own trees' spacing
// sets them). Empty when the field is not valid or `mesh` is not horizon_mesh(field).
std::vector<HorizonTree> horizon_trees(const HorizonField& field, const HorizonMesh& mesh, float woods, float open);
// The land's surface at `p` as `mesh` (horizon_mesh(field)) draws it: its height, and the normal
// of the triangle there. nullopt outside its grid.
std::optional<float> horizon_surface(const HorizonField& field, const HorizonMesh& mesh, glm::vec2 p,
                                     glm::vec3* normal = nullptr);

} // namespace hailstone::render
