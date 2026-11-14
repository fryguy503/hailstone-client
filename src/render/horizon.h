#pragma once

// The land round an outdoor zone (Hailstone's own, the modern lighting; docs/graphics.md "Horizon hills"): terrain past
// the zone's edges, out to about the modern view's reach, so the land does not end in a blank haze. The original drew
// nothing past a zone's own geometry. This is the CPU half: what the zone's own terrain says about the land round it
// (where it lies, how high and rugged it is), the neighbour zones' ground that shapes it past their zone lines, and
// the mesh, which ZoneRenderer::prepare adds to the zone's geometry under the zone's own ground, rock and water
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

// The zone's terrain is laid on a grid of kHorizonCell squares (terrain anywhere in a square covers it; geometry
// further than kHorizonStray past the level ground is left out). Past the covered squares (and in the pockets they
// close round: a gap between the zone's walls, a hole in it) the land starts at the zone's edge, the height of its
// terrain there (a wall's top, the open ground at a zone line), and turns over kHorizonBlend into rolling hills and
// then, further out, mountains, as rugged as the zone's relief says; where the zone's edge is water, the sea, with
// land rising from it far out. Its vertices in covered squares are tucked kHorizonTuck under the zone's terrain there,
// and those in the empty squares beside them just under the lowest of it (a wall's foot), so the land meets the zone's
// edge with no gap. Its squares are kHorizonCell across over the grid and kHorizonFine past it (more for a big zone:
// at most kHorizonMaxFine of them across), then grow by kHorizonGrowth a step out to kHorizonReach past the grid.
inline constexpr float kHorizonCell = 64.0f;
inline constexpr float kHorizonStray = 4000.0f;
inline constexpr float kHorizonBlend = 1200.0f;
inline constexpr float kHorizonTuck = 40.0f;
inline constexpr float kHorizonReach = 10000.0f;
inline constexpr float kHorizonFine = 2500.0f;
inline constexpr int kHorizonMaxFine = 256;
inline constexpr float kHorizonGrowth = 1.12f;
// The steepest land drawn with the zone's ground material (the normal's z); steeper is rock.
inline constexpr float kHorizonRock = 0.8f;
// How far under the water's surface the sea's floor lies, past the shore.
inline constexpr float kHorizonSeaDepth = 250.0f;

// What the land is built from: the zone's terrain on the grid (horizon_field), and the mesh's lines. The same field
// gives the same lines, so the current zone's land and a neighbour's part of it (horizon_mesh with `only`) meet
// vertex for vertex.
struct HorizonField {
    HorizonLand land;
    glm::vec2 lo{0.0f};
    float cell = kHorizonCell;
    int nx = 0, ny = 0;
    std::vector<float> zmin, zmax, water;
    std::vector<std::uint8_t> dry, wet; // terrain, and a water surface, in the square
    std::vector<std::uint8_t> open;     // an empty square the grid's border reaches without crossing terrain
    std::vector<float> dist;            // to the nearest covered square, in squares (empty ones)
    std::vector<float> edge, sea;       // the zone's edge's height there, and whether it is water (0..1)
    std::vector<float> under;           // a covered square's, or one beside it: how high the land is tucked
    float water_level = 0.0f;           // the median of the zone's water surfaces
    bool has_water = false;
    std::vector<float> xs, ys;          // the mesh's lines

    bool valid() const { return land.valid && nx > 0 && xs.size() > 1 && ys.size() > 1; }
    std::size_t at(int x, int y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x);
    }
    bool covered(std::size_t i) const { return dry[i] || wet[i]; }
    // The square of the grid holding `p`, clamped to it; `extra` the distance from p to the grid when it lies outside.
    std::size_t square(glm::vec2 p, float& extra) const;
    // Whether `p` lies in a covered square of the grid (the zone's own terrain is there; the land is tucked under it).
    bool covers(glm::vec2 p) const;
    // The mesh's square holding `p` (row x columns + column), -1 outside the mesh: the key the land's triangles are
    // hidden by (ZoneRenderer::set_distant_hidden) and a neighbour's part of the land lists (HorizonMesh::squares).
    std::int64_t mesh_square(glm::vec2 p) const;
};

// The field of the zone's terrain; not valid when `land` is not.
HorizonField horizon_field(std::span<const HorizonTriangle> triangles, const HorizonLand& land);

// A neighbour zone's ground in the current zone's coordinates (app/neighbour_zones.h), which shapes the land past its
// zone line: heights on a grid of kHorizonCell squares, NaN where it has none (before the seam, in its void),
// measured from its terrain alone (its placed objects, trees and houses, are not its ground). A wall or a crest narrower
// than three squares is taken off (horizon_source: the ground is "opened", eroded then dilated over kHorizonOpening
// squares), so the land does not rise in a ramp against a wall; wider, it follows the terrain. Past its ground its
// influence fades over kHorizonSourceFade (its `cover`, 1 on its ground).
inline constexpr int kHorizonOpening = 1;
inline constexpr float kHorizonSourceFade = 1200.0f;
struct HorizonSource {
    std::string name; // the neighbour's short name
    glm::vec2 lo{0.0f};
    float cell = kHorizonCell;
    int nx = 0, ny = 0;
    std::vector<float> height; // the opened ground, NaN where none; padded by the fade on every side
    std::vector<float> top;    // its terrain's highest point in each square, unopened, NaN where none (or empty)
    // finish_horizon_source: each square's nearest ground, and how far it lies (0 on it); on its ground, how far the
    // nearest square without ground lies (its edge: before the seam, its void past its own boundary walls).
    std::vector<float> fill, dist, inner;
    bool valid() const {
        const auto n = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
        return nx > 0 && ny > 0 && height.size() == n && fill.size() == n && dist.size() == n && inner.size() == n;
    }
};
// The sources a land is built with, sorted by name (each once): the same set builds the same land.
using HorizonSources = std::vector<std::shared_ptr<const HorizonSource>>;
void sort_sources(HorizonSources& sources);

// A source from the neighbour's ground sampled on `nx` x `ny` squares from `lo` (NaN: none): padded, opened, finished.
// `top` (the same squares, or empty): its terrain's highest point in each, kept as it is (horizon_source_top).
HorizonSource horizon_source(std::string name, glm::vec2 lo, float cell, int nx, int ny, std::vector<float> ground,
                             std::vector<float> top = {});
// Fills in `fill` and `dist` from `height` (a source read back from the cache).
void finish_horizon_source(HorizonSource& source);
struct HorizonSourceSample {
    float height = 0.0f; // the ground, or past it the nearest ground's height
    float cover = 0.0f;  // 1 on its ground, fading to 0 over kHorizonSourceFade past it
    float inner = 0.0f;  // on its ground, how far in from its edge
};
HorizonSourceSample sample_source(const HorizonSource& source, glm::vec2 p);
// The opened ground under `p` (bilinear between the squares' centres), nullopt where the source has none.
std::optional<float> source_ground(const HorizonSource& source, glm::vec2 p);
// Its terrain's highest point in the square holding `p`, unopened; nullopt where it has none or was not kept.
std::optional<float> source_top(const HorizonSource& source, glm::vec2 p);

// Where the land comes off the zone's edge onto a neighbour's ground, it eases from the edge's height to the
// neighbour's over a band past the edge: kHorizonBandSlope times their difference, kHorizonBandMin to kHorizonBandMax
// across, so a few units' disagreement is a short slope and a cliff's top a long one.
inline constexpr float kHorizonBandSlope = 3.0f;
inline constexpr float kHorizonBandMin = 200.0f;
inline constexpr float kHorizonBandMax = 800.0f;

// The land's height at `p` (as the mesh's vertices have it).
float horizon_height(const HorizonField& field, const HorizonSources& sources, glm::vec2 p);

// Past the band, where a neighbour that is drawn has its own ground (its cover whole) and at least kHorizonInnerFrom
// in from its edge (kHorizonInnerTo for all of it), the land of its part (horizon_mesh with `only`) sinks kHorizonTuck
// under that ground, so the neighbour's own terrain is drawn there over it (its roads, its textures, its hills) and the
// land fills only what it lacks; in the band and by its edge (its own boundary walls and filler) its terrain goes and
// the land is the ground (game::split_off_land). `settled`, 0..1: how far it has sunk.
inline constexpr float kHorizonInnerFrom = 128.0f;
inline constexpr float kHorizonInnerTo = 512.0f;
struct HorizonPoint {
    float height = 0.0f; // as `only`'s part has it
    float settled = 0.0f;
};
HorizonPoint horizon_point(const HorizonField& field, const HorizonSources& sources, glm::vec2 p, int only);

struct HorizonMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<float> shade;  // per vertex, 0..1: what is left of the light (woods and valleys are darker)
    std::vector<float> weight; // per vertex, 0..1: a neighbour's part (`only`): how much its own ground shows
    std::vector<float> settled; // per vertex, 0..1: a neighbour's part: how far under its terrain (horizon_point)
    // Triangles (index triples into positions), counter-clockwise seen from above: the zone's ground material, its
    // rock (slopes and peaks), and its water (a flat surface at water_level).
    std::vector<std::uint32_t> ground, rock, water;
    float water_level = 0.0f;
    std::vector<std::int64_t> squares; // a neighbour's part: its squares (HorizonField::mesh_square), sorted
    bool empty() const { return ground.empty() && rock.empty() && water.empty(); }
};

// The land round the zone, shaped by `sources` where their ground is; empty when the field is not valid. With `only`
// (an index into `sources`), only the squares that source owns (the one with the most cover at their corners), with
// its weight (its cover times how far across the band the vertex lies) and no water: the part a neighbour zone
// draws with its own ground (app/neighbour_zones.h), where the current zone's land is hidden.
HorizonMesh horizon_mesh(const HorizonField& field, const HorizonSources& sources, int only = -1);
// The land round the zone from its terrain alone.
HorizonMesh horizon_mesh(std::span<const HorizonTriangle> triangles, const HorizonLand& land);

// A neighbour's source as the cache keeps it (app/neighbour_zones.h; cache/horizon/<server>/<zone>/<neighbour>.bin):
// for which pair of zones, the files it was measured from (`key`: their sizes), the offset it was moved by.
struct HorizonCacheEntry {
    std::string zone, neighbour;
    std::uint64_t key = 0;
    glm::vec3 offset{0.0f};
    HorizonSource source;
};
std::vector<std::byte> write_horizon_cache(const HorizonCacheEntry& entry);
// nullopt when the bytes are not a cache entry of this version, or are damaged.
std::optional<HorizonCacheEntry> read_horizon_cache(std::span<const std::byte> data);

} // namespace hailstone::render
