#pragma once

// Static zone collision: every triangle of the zone (terrain and placed
// objects) in a uniform XY grid, with the original's triangle records
// (game/swept_sphere.h: the plane normal in the file's vertex order and the
// passable / transparent flags), for the movement sweep, floor and ray
// queries.
//
// Movement (sweep_sphere) follows CCollision::CollideActor: passable
// polygons (WLD 0x10, EQG flag 1) never block, transparent ones do, back
// faces are skipped. The floor and line-of-sight probes are the client's own
// (the original's GetFloorHeight and CollideLineSegment are not ported):
// they use the triangles that are neither passable nor transparent
// (translucent and additive surfaces: water, glass, effects), as before.
//
// Each solid ladder also has a slab (game/zone_ladders.h ladder_slab, flagged
// kTriangleLadderSlab). Only the sweeps, floor_below and ceiling_above (the
// player's movement and its floor and ceiling tests) use it, and only with
// set_solid_ladders(true); they then leave out the ladder's own polygons.
// Everything else, and triangles(), keeps the ladder's polygons.

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "game/swept_sphere.h"

namespace hailstone::assets { struct Zone; }

namespace hailstone::game {

class CollisionMesh {
public:
    static CollisionMesh build(const assets::Zone& zone);
    // Effects only: the renderer's refined terrain/replacement objects when selected by the loaded zone. Keeps
    // upward opaque/masked drawn surfaces and excludes moving parts, which AnimatedObjects poses separately.
    static CollisionMesh build_visual(const assets::Zone& zone);
    // From triangles (tests and tools).
    static CollisionMesh from_triangles(std::vector<CollisionTriangle> triangles);

    // Highest floor at (x, y) no higher than z + step_up, or nullopt if there
    // is no floor below.
    std::optional<float> floor_below(const glm::vec3& p, float step_up) const;
    // Lowest surface at (x, y) above z, or nullopt.
    std::optional<float> ceiling_above(const glm::vec3& p) const;
    // Every surface at (x, y) above z, lowest first, with its record normal: a normal facing up (z > 0) is a top
    // seen from below. For the buried zone lines' test (game/zone_lines.h).
    struct SurfaceHit {
        float z = 0.0f;
        glm::vec3 normal{0.0f};
    };
    std::vector<SurfaceHit> surfaces_above(const glm::vec3& p) const;

    // What is seen rather than what is stood on (the neighbour zones' seam tests, game/neighbour_zones.h): the
    // highest surface at (x, y) at or below z that faces up (normal z > 0) and is drawn (not BlendMode::Invisible),
    // and the lowest drawn surface above z, facing either way. A tunnel roof's underside is not ground, and an
    // invisible lid over a zone line is not a roof.
    std::optional<float> ground_below(const glm::vec3& p) const;
    std::optional<float> roof_above(const glm::vec3& p) const;

    // Visual effects: highest upward-facing drawn opaque/masked surface no higher than p.z, including passable
    // decorative props. Excludes transparent surfaces, invisible collision and movement-only ladder slabs.
    // This deliberately does not change the movement/ground probes' collision policy.
    std::optional<float> visual_surface_below(const glm::vec3& p) const;
    bool visual_surfaces(const glm::vec2& low,const glm::vec2& high,
                         const std::function<bool(const CollisionTriangle&)>& visit,std::uint32_t& work) const;

    // True when no solid triangle blocks the segment from a to b.
    bool line_of_sight(const glm::vec3& a, const glm::vec3& b) const;

    // Every triangle whose grid cells the segment a -> b crosses, once each
    // (with the huge ones), for the mouse pick's world pass (game/picking.h).
    void for_each_on_segment(const glm::vec3& a, const glm::vec3& b,
                             const std::function<void(const CollisionTriangle&)>& fn) const;

    // The first surface the segment a -> b meets as the precipitation's CollideLineSegment finds it (its
    // CPrecipitationCollisionInfo: transparent surfaces ignored, passable ones hit, back faces skipped): the fraction
    // along the segment and the surface's normal, or nullopt. `back_faces`: back faces are hit too (not the
    // original's), so a ceiling with nothing over it stops the rain: the East Commonlands tunnel's mountain has no top,
    // and the original's probe falls through the tunnel's roof to its floor.
    struct SegmentHit {
        float t = 1.0f;
        glm::vec3 normal{0.0f};
    };
    std::optional<SegmentHit> first_hit(const glm::vec3& a, const glm::vec3& b, bool back_faces = false) const;
    // CTerrainBase::CollideSphere: tests the sweep against every triangle the
    // swept sphere's box touches.
    void sweep_sphere(SphereSweep& sweep) const;
    // The same sweep; the index of the triangle that gave the final hit, or
    // -1 without one.
    std::int32_t sweep_sphere_hit(SphereSweep& sweep) const;
    // Hailstone: whether the finished sweep met a passable ladder triangle (a LADDER* placement whose polygons are
    // all passable, as Cabilis's LADDERCB101) short of where it stopped. Such a triangle never stops the sphere.
    bool touches_passable_ladder(const SphereSweep& sweep) const;

    // Hailstone ([movement] solid_ladders): the sweeps, floor_below and ceiling_above meet each solid ladder as its
    // slab instead of its rails and rungs (game/zone_ladders.h). Off (the default), as the original: the rungs.
    void set_solid_ladders(bool on) { solid_ladders_ = on; }
    bool solid_ladders() const { return solid_ladders_; }

    // The light level of a zone triangle, as CActor::GetScaledAmbient reads
    // it (0x1001fa60): (t[a] + t[b] + t[c]) x 0.33333299 of its vertex alpha
    // bytes, t = byte / 256 (CRender +0x85a0), on the FPU at 24 bits. NaN for
    // a placed object's triangle (the DLL's hit has no region triangle there).
    float triangle_light(std::int32_t triangle) const {
        return triangle >= 0 && static_cast<std::size_t>(triangle) < light_.size() ? light_[static_cast<std::size_t>(triangle)]
                                                                                   : std::numeric_limits<float>::quiet_NaN();
    }

    // The zone's box (pinstSceneGraph +0x50, the CTerrain's min / max) over
    // the terrain's vertices; nullopt without terrain.
    const std::optional<std::pair<glm::vec3, glm::vec3>>& bounds() const { return bounds_; }

    std::size_t triangle_count() const { return triangles_.size(); }
    // Every triangle, terrain then placed objects (the navigation mesh's input, nav/nav_mesh.h), ladder slabs among
    // them (kTriangleLadderSlab: leave them out for anything but the movement).
    std::span<const CollisionTriangle> triangles() const { return triangles_; }

private:
    static CollisionMesh build_scene(const assets::Zone& zone, bool visual);
    static constexpr float kCell = 64.0f;
    static constexpr std::size_t kGroundDivisions = 16;
    static constexpr float kGroundCell = kCell / static_cast<float>(kGroundDivisions);
    struct GroundSubgrid {
        std::array<std::size_t, kGroundDivisions * kGroundDivisions + 1> offsets{};
        std::vector<std::uint32_t> candidates;
    };
    static std::int64_t cell_key(std::int32_t x, std::int32_t y) {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(y);
    }
    // The grid column or row holding coordinate c. A coordinate past +-6.4e10 (a hostile teleport, a script's
    // LineOfSight) or NaN is clamped instead of overflowing the int32 cast; no triangle is listed there.
    static std::int32_t cell_of(float c) {
        const float f = std::floor(c / kCell);
        if (!(f > -1.0e9f)) return -1000000000; // NaN too
        if (f > 1.0e9f) return 1000000000;
        return static_cast<std::int32_t>(f);
    }
    static std::size_t ground_subcell(float c, std::int32_t coarse) {
        const double local = (static_cast<double>(c) - static_cast<double>(coarse) * kCell) / kGroundCell;
        if (!(local > 0.0)) return 0; // NaN too; no unchecked float-to-integer conversion
        if (local >= static_cast<double>(kGroundDivisions)) return kGroundDivisions - 1;
        return static_cast<std::size_t>(local);
    }
    void index();
    // fn(triangle) for every triangle listed in the grid cell under (p.x, p.y), then the huge ones.
    template <class Fn>
    void for_each_at(const glm::vec3& p, Fn&& fn) const;
    // fn(index) for every triangle the swept sphere's box touches, once each, then the huge ones.
    template <class Fn>
    void for_each_near_sweep(const SphereSweep& s, Fn&& fn) const;
    // Triangles for the floor and sight probes (never a ladder's slab).
    static bool probe_solid(const CollisionTriangle& t) {
        return !(t.flags & (kTrianglePassable | kTriangleTransparent | 0x10 | kTriangleLadderSlab));
    }
    static bool visible_ground(const CollisionTriangle& t) {
        return probe_solid(t) && !(t.flags & kTriangleInvisible) && t.normal.z > 0.0f;
    }
    static bool visual_surface(const CollisionTriangle& t) {
        return t.normal.z > 0.0f &&
            !(t.flags & (kTriangleTransparent | kTriangleInvisible | kTriangleLadderSlab | kTriangleNoVisualSurface | 0x10));
    }
    // Whether the movement's queries take the triangle: the slab with solid_ladders_, else the ladder's own polygons
    // (a polygon of a ladder that is not passable means the ladder has a slab).
    bool movement_kept(const CollisionTriangle& t) const {
        if (t.flags & kTriangleLadderSlab) return solid_ladders_;
        return !(solid_ladders_ && (t.flags & kTriangleLadder) && !(t.flags & kTrianglePassable));
    }
    // probe_solid for the movement's floor and ceiling tests.
    bool movement_solid(const CollisionTriangle& t) const {
        return movement_kept(t) && !(t.flags & (kTrianglePassable | kTriangleTransparent | 0x10));
    }

    std::vector<CollisionTriangle> triangles_;
    std::vector<float> light_; // per triangle (triangle_light); empty from from_triangles
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> cells_;
    // Immutable finer candidate lists only for dense cells, used by ground_below. Movement and other probes keep
    // their original coarse lists; only visible, upward-facing probe surfaces enter these 4-unit subdivisions.
    std::unordered_map<std::int64_t, GroundSubgrid> ground_cells_;
    // The effect query has a separate policy: passable drawn props are surfaces too. Its immutable fine index
    // keeps dense EQG scenery from making each terrain sample inspect hundreds of irrelevant coarse candidates.
    std::unordered_map<std::int64_t, GroundSubgrid> visual_cells_;
    std::vector<std::uint32_t> big_; // triangles too large for the grid
    std::optional<std::pair<glm::vec3, glm::vec3>> bounds_;
    bool solid_ladders_ = false;
};

} // namespace hailstone::game
