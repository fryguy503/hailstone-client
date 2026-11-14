#pragma once

// Neighbour zones (docs/graphics.md, "Neighbour zones"): the modern look draws the zone past an outdoor zone line in
// the distance, so the world does not end there. Purely visual: nothing collides with it, no spawns stand in it, and
// zoning works as before. Not the original's: it drew nothing past its zone's own geometry.
//
// This file is the arithmetic: where the zone lines are, the offset from the neighbour's coordinates to the current
// zone's, the plane the neighbour is cut at, and which lines qualify. app/neighbour_zones.h loads and draws.
//
// All positions are world (zone file) coordinates (game/coords.h); the zone points' targets come in server
// coordinates and are swapped on the way in.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::assets { class ZoneRegions; }

namespace hailstone::game {

// The side of `plane` (xyz . p + w) a point is on: >= 0 is kept.
inline float plane_side(const glm::vec4& plane, const glm::vec3& p) { return glm::dot(glm::vec3(plane), p) + plane.w; }

// Sutherland-Hodgman: cuts the convex polygon `poly` to the half-space plane_side >= 0, in place. `position(v)` gives
// a vertex's position, `lerp(a, b, t)` the vertex t of the way from a to b. Fewer than 3 vertices left: nothing kept.
template <class V, class Position, class Lerp>
void clip_polygon(std::vector<V>& poly, const glm::vec4& plane, Position&& position, Lerp&& lerp, std::vector<V>& scratch) {
    scratch.clear();
    const std::size_t n = poly.size();
    for (std::size_t i = 0; i < n; ++i) {
        const V& a = poly[i];
        const V& b = poly[(i + 1) % n];
        const float da = plane_side(plane, position(a)), db = plane_side(plane, position(b));
        if (da >= 0.0f) scratch.push_back(a);
        if ((da >= 0.0f) != (db >= 0.0f)) scratch.push_back(lerp(a, b, da / (da - db)));
    }
    poly.swap(scratch);
    if (poly.size() < 3) poly.clear();
}

// A zone point's target as OP_SendZonepoints gives it (net::ZonePoint), in server coordinates; 999999 keeps that
// coordinate of the player's.
struct ZonePointTarget {
    std::uint32_t number = 0;
    std::uint32_t zone_id = 0;
    float server_x = 0.0f, server_y = 0.0f, server_z = 0.0f;
};

// The value that keeps a coordinate (EQEmu's zone_points, OP_SendZonepoints).
inline constexpr float kKeepCoordinate = 999999.0f;
inline bool keeps(float v) { return v >= kKeepCoordinate - 0.5f; }

// Where a zone line leads, in world coordinates; keep[i] when that coordinate stays the player's.
struct LineTarget {
    glm::vec3 position{0.0f};
    glm::bvec3 keep{false};
};
LineTarget line_target_from_server(float server_x, float server_y, float server_z);

// The convex piece of space one BSP leaf of a zone-line region holds (assets::ZoneRegions::leaf_cells).
struct LineCell {
    glm::vec3 lo{0.0f}, hi{0.0f};
};

// Every zone line of the current zone that leads to one other zone.
struct NeighbourLines {
    std::uint32_t zone_id = 0;
    std::vector<LineCell> cells;      // the lines' pieces of space
    std::vector<LineTarget> targets;  // one per zone point (or absolute tag) leading there
    glm::vec3 lo{0.0f}, hi{0.0f};     // round all the cells
};

// The zone lines by the zone they lead to: each zone-line region's leaves, its target from its tag (an absolute
// target) or from the zone point it names. Lines to `current_zone`, to zone 0 and to unknown zone points are left out.
std::vector<NeighbourLines> collect_neighbour_lines(const assets::ZoneRegions& regions, const glm::vec3& zone_lo,
                                                    const glm::vec3& zone_hi, const std::vector<ZonePointTarget>& points,
                                                    std::uint32_t current_zone);

// The seam: the lines' outward direction (one of +-x, +-y: zones are not rotated; a thin line box faces along its
// thin axis, a squarer one the edge of the zone it is nearest), the inner face of the lines along it (where the
// player crosses), and their middle.
struct Seam {
    int axis = 0;          // 0 x, 1 y
    float sign = 1.0f;     // outward along the axis
    float face = 0.0f;     // the coordinate the player crosses at
    glm::vec3 centre{0.0f};
    glm::vec3 lo{0.0f}, hi{0.0f};
    glm::vec3 outward() const {
        glm::vec3 u(0.0f);
        u[axis] = sign;
        return u;
    }
};
// A line box at least this many times longer across than it is thick faces along its thin axis, whichever zone edge
// is nearer.
inline constexpr float kSeamThinRatio = 2.0f;
Seam seam_of(const NeighbourLines& lines, const glm::vec3& zone_lo, const glm::vec3& zone_hi);

// The neighbour's return line: its zone-line cells (whatever they lead to) near the arrival point that face back,
// opposite the seam. Its inner face and middle, in the neighbour's coordinates.
struct ReturnLine {
    float face = 0.0f;
    glm::vec3 centre{0.0f};
    glm::vec3 lo{0.0f}, hi{0.0f}; // round its cells
    std::size_t cells = 0;
};
std::optional<ReturnLine> find_return_line(const std::vector<LineCell>& neighbour_cells, const Seam& seam,
                                           const glm::vec3& arrival, float reach = 400.0f);
// The return line as a seam of the neighbour's own, facing back towards the current zone: its open sky is tested the
// way the current zone's line is, so a pair qualifies only when both sides do (a tunnel whose far mouth opens on a
// desert is still a tunnel).
Seam return_seam(const ReturnLine& back, const Seam& seam);

// The offset that takes the neighbour's coordinates into the current zone's (current = neighbour + offset).
struct NeighbourOffset {
    glm::vec3 offset{0.0f};
    glm::vec3 spread{0.0f};   // largest minus smallest estimate per axis
    std::size_t estimates = 0;
    bool return_line = false; // the travel and side axes came from the neighbour's own line
    bool ground = false;      // z came from matching the ground at the seam
    std::string detail;       // for the log
};
// From the targets alone: per axis, 0 where any target keeps the coordinate (the zones share it); else the median of
// the targets' estimates: the travel axis the inner face minus the arrival, the side axis the lines' middle minus the
// arrival, z the lines' bottom minus the arrival's (a stand-in until refine_offset matches the ground).
NeighbourOffset offset_from_targets(const NeighbourLines& lines, const Seam& seam);
// Ground heights within this of each other agree (refine_offset).
inline constexpr float kGroundAgree = 16.0f;
// The ground under a point: every floor height found going down through the line's height range, highest first.
using Floors = std::function<std::vector<float>(const glm::vec3&)>;
// Refined with the neighbour's geometry: the travel axis lines the two zones' faces up (when the targets did not keep
// it and the faces agree with the first estimate within `tolerance`), the side axis the two lines' middles (when no
// target kept it), and z from the ground heights at `seam_points` (the current zone's floor just inside the seam
// minus the neighbour's just past it): of the floors each side finds there (a gate's arch, a bridge, a sea floor),
// the difference the most points agree on within kGroundAgree (ties: the one nearest the first estimate), then the
// median of the points' differences nearest it. At least half the points must find ground on both sides, and two
// of those and a third at least agree.
NeighbourOffset refine_offset(NeighbourOffset first, const Seam& seam, const NeighbourLines& lines,
                              const std::optional<ReturnLine>& back, const std::vector<glm::vec3>& seam_points,
                              const Floors& floor_here, const Floors& floor_there, float tolerance = 120.0f);

// Points across the seam's face, `count` of them, at its middle height: where the ground and the open sky are tested.
std::vector<glm::vec3> seam_points(const Seam& seam, int count = 9);

// The plane the neighbour is cut at, in the current zone's coordinates: `margin` past the seam's face, keeping what
// lies further out.
glm::vec4 seam_clip_plane(const Seam& seam, float margin);

// Whether a zone line gets its neighbour drawn.
enum class NeighbourVerdict {
    Draw,
    Off,           // display.neighbour_zones off, or the classic lighting
    IndoorHere,    // the current zone has no sky or an indoor time type
    NoLine,        // no zone-line cells (a door or a teleport only)
    ClosedHere,    // the line is under a roof (a tunnel, a cave mouth)
    ClosedThere,   // the arrival is under a roof (a dungeon, a tunnel)
    NoFiles,       // the neighbour's files are not in the asset folder
    Disagree,      // the zone points to it disagree (several gates, a city's inner lines): no one offset fits
    NoReturn,      // the neighbour has no zone line facing back near the arrival: the seam cannot be checked
};
const char* to_string(NeighbourVerdict v);

struct NeighbourCheck {
    bool enabled = true;          // display.neighbour_zones and the modern lighting
    bool outdoor_here = false;    // OP_NewZone: a sky, an outdoor time type (1, 2, 5)
    std::size_t cells = 0;
    float open_here = -1.0f;      // the share of seam points with open sky above in the current zone; < 0 untested
    float open_there = -1.0f;     // the same just past the seam in the neighbour and inside its line back (the lower)
    bool files = true;
    float spread = 0.0f;          // the targets' largest disagreement on the travel and side axes
    int return_line = -1;         // the neighbour's line back: -1 not looked for yet, 0 none, 1 found
};
// The targets may disagree by this much (the zone points' own scatter round a gate).
inline constexpr float kMaxTargetSpread = 150.0f;
// Open sky: no ceiling within this height over the ground.
inline constexpr float kOpenSkyHeight = 400.0f;
// At least this share of the points tested must see the sky.
inline constexpr float kOpenSkyShare = 0.5f;
NeighbourVerdict neighbour_verdict(const NeighbourCheck& c);

// The share of `points` with no ceiling within kOpenSkyHeight above the ground under them (`floor` the ground's
// height, nullopt none; `ceiling` the first surface above, nullopt open).
float open_sky_share(const std::vector<glm::vec3>& points,
                     const std::function<std::optional<float>(const glm::vec3&)>& floor,
                     const std::function<std::optional<float>(const glm::vec3&)>& ceiling);

// The current zone's ground over the band past a seam, where its own filler (the ground it builds past its zone line
// as far as its edge) lies under the neighbour's: heights on a square grid, NaN where it has no ground.
struct GroundGrid {
    glm::vec2 origin{0.0f};
    float cell = 16.0f;
    int width = 0, height = 0;
    std::vector<float> heights;
    // The ground at (x, y): its cell's height, nullopt outside the grid or where there is none.
    std::optional<float> at(float x, float y) const;
    // The ground at (x, y) between the samples: bilinear over the four nearest cell centres (a missing one counts as
    // the cell's own), nullopt where the cell has none.
    std::optional<float> height_at(float x, float y) const;
    // Whether (x, y)'s cell and the eight round it all have ground: away from the ground's edge by a cell at least.
    bool inside_ground(float x, float y) const;
    bool empty() const { return heights.empty(); }
};
// The band the current zone's filler can cover: from the seam's face out to the zone's edge (plus `margin`), across the
// zone's whole width.
void seam_band(const Seam& seam, const glm::vec3& zone_lo, const glm::vec3& zone_hi, float margin, glm::vec2& lo,
               glm::vec2& hi);
GroundGrid sample_ground(const glm::vec2& lo, const glm::vec2& hi, float cell,
                         const std::function<std::optional<float>(float x, float y)>& floor);
// sample_ground a row at a time, so the frame can spread it over several (app/neighbour_zones.cpp): the same grid.
class GroundSampler {
public:
    GroundSampler(const glm::vec2& lo, const glm::vec2& hi, float cell);
    // Samples rows, the first always, the next while `more()` says so; true once every row is done.
    bool step(const std::function<std::optional<float>(float x, float y)>& floor, const std::function<bool()>& more);
    bool done() const { return row_ >= grid_.height; }
    GroundGrid take() { return std::move(grid_); }

private:
    GroundGrid grid_;
    int row_ = 0;
};

// A convex polygon as barycentric weights over a triangle's corners (a, b, c).
using BaryPolygon = std::vector<glm::vec3>;
// Where a neighbour's triangle (in the current zone's coordinates) doubles the current zone's ground, it goes, so the
// current zone's ground shows there without the two fighting over the same pixels. A triangle facing up (normal
// z >= 0.6) is cut along the grid's cells; a piece every corner of which lies within `tolerance` of the ground under
// it (GroundGrid::height_at), a cell or more inside the ground's edge (GroundGrid::inside_ground), is left out: by
// the edge the neighbour's ground stays, so no gap opens where the current zone's ground ends within a cell (there
// the neighbour is drawn behind the current zone, ZoneRenderOptions::behind). Returns the pieces to keep (empty: none), or nullopt when the whole triangle
// stays (it faces sideways, lies outside the grid or doubles no ground anywhere). Walls, buildings and hills standing
// above the ground stay.
std::optional<std::vector<BaryPolygon>> split_off_ground(const GroundGrid& ground, const glm::vec3& a,
                                                         const glm::vec3& b, const glm::vec3& c,
                                                         float tolerance = 24.0f);

// Where the land round the current zone (render/horizon.h) stands in for a neighbour's ground: a grid over the
// neighbour, per cell the neighbour's own ground (its opened ground, render::source_ground; NaN none), whether its own
// terrain is drawn there (where the land has sunk under it, render::horizon_point: past the band), and whether what
// stands on it may be drawn there: where the land (or, over the current zone's own terrain, that terrain) lies no lower
// than the neighbour's ground less the tolerance, so a wall or a house is never seen floating over a gap; else it is
// left out, and the land is all there is.
struct LandMask {
    glm::vec2 origin{0.0f};
    float cell = 32.0f;
    int width = 0, height = 0;
    std::vector<float> ground;        // the neighbour's ground at the cell's centre, NaN none
    std::vector<std::uint8_t> stands;  // what stands there may be drawn
    std::vector<std::uint8_t> terrain; // its terrain is drawn there (empty: everywhere)
    std::vector<std::uint8_t> own;     // the current zone's own terrain or water is there (empty: nowhere)
    std::vector<float> surface;        // the ground drawn there: the land, or the current zone's own (NaN: unknown)
    bool empty() const { return ground.empty(); }
    // Bilinear between the cells' centres (a missing one counts as the cell's own), nullopt where the cell has none.
    std::optional<float> ground_at(float x, float y) const;
    // Whether what stands at (x, y) may be drawn; true outside the grid.
    bool stands_at(float x, float y) const;
    // Whether its terrain at (x, y) is drawn; true outside the grid.
    bool terrain_at(float x, float y) const;
    // Whether the current zone's own terrain or water is at (x, y); false outside the grid.
    bool own_at(float x, float y) const;
    // The ground drawn at (x, y), bilinear between the cells' centres; nullopt outside the grid or where unknown.
    std::optional<float> surface_at(float x, float y) const;
};
// Where its terrain stops (LandMask::terrain_at), it must stop where it meets the land, or the cut leaves a fin in
// the air (a bank, a ridge narrower than its opened ground): from the cells where it is not drawn, a cell where it is
// whose highest ground (`highest` at the cell's middle; its terrain's own, unopened) stands more than `tolerance` over
// the ground drawn there (LandMask::surface) is left out too, and so on from it, at most `reach` cells on. `stop` is
// asked between cells. Returns how many cells were left out.
std::size_t spread_terrain_cut(LandMask& mask, const std::function<std::optional<float>(glm::vec2)>& highest,
                               float tolerance, int reach, const std::function<bool()>& stop = {});
// What a neighbour's triangle is: its own terrain (its zone's meshes), a water or lava surface, or a placed object.
enum class LandPart : std::uint8_t { Placed, Terrain, Liquid };
// A neighbour's triangle (in the current zone's coordinates) where the land stands in for its ground, cut along the
// mask's cells; each piece goes by its middle, or by its corners:
// - its terrain goes where its terrain is not drawn (LandMask::terrain_at): in the band the land is the ground, and a
//   slope of the neighbour's crossing it there would show as shards and strips; by its edge, its own filler. There a
//   piece with a corner more than `tolerance` over its own ground (a wall, a tower, a wall's top: narrower than its
//   opened ground) is kept as a placed object is, where something may stand;
// - its water goes over the current zone's own terrain or water (LandMask::own_at), which would double it;
// - water and placed objects go where nothing may stand (LandMask::stands_at);
// - a placed object's piece facing up (normal z >= 0.6) every corner of which lies within `tolerance` of the
//   neighbour's ground under it is ground, which the land draws instead, and goes.
// Returns the pieces to keep (empty: none), or nullopt when the whole triangle stays.
std::optional<std::vector<BaryPolygon>> split_off_land(const LandMask& mask, const glm::vec3& a, const glm::vec3& b,
                                                       const glm::vec3& c, LandPart part, float tolerance = 24.0f);

// The distance from `p` to the box, in the horizontal plane.
float distance_xy(const glm::vec3& p, const glm::vec3& lo, const glm::vec3& hi);

} // namespace hailstone::game
