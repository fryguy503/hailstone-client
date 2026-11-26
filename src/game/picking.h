#pragma once

// The graphics DLL's half of mouse picking: the scene graph's
// PickActorAtScreenPoint @0x10069790 (EQGraphicsDX9.dll); no vectors: the
// original's vectors fake the DLL, so this half follows the
// disassembly:
//  - the ray: CCamera::GetScreenRay @0x10007640 (the camera-space direction
//    from the cot table and the viewport @0x10006860, rotated by the camera,
//    normalised, times Yon), from the camera position;
//  - up to four passes (mode 0 with passable triangles blocking, mode 1, then
//    both again with passable triangles ignored) until the nearest actor has
//    the required mask bit;
//  - one pass (CCollision::CollideLineSegment @0x100ba1d0): the world
//    triangles first (world best = the nearest), then every actor
//    (FindIntersectionLineSegment @0x10046780 and slot 6 @0x100083c0): the
//    nearest actor whose t is no farther than the world best wins, the first
//    tested on a tie.
// The eqgame side (masks, exclusions, what a click does) is game/clicks.h.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "game/swept_sphere.h"

namespace hailstone::game {

// CMath +0x1084 table read by 0x100b93d0: cos[i] / sin[i] (1e30 for i = 0)
// at i = trunc(angle) & 511 folded into 0..127 (quadrants 1 and 2 use ~i).
float cmath_cot_folded(float angle);

// The camera as CCamera::GetScreenRay reads it.
struct PickCamera {
    glm::vec3 position{0.0f};              // +0xbc
    glm::vec3 right{1, 0, 0}, up{0, 0, 1}, forward{0, 1, 0}; // the rotation rows (+0x40, +0x4c, +0x58)
    float half_width = 0.0f, half_height = 0.0f; // +0x38 / +0x3c: the viewport's width and height / 2
    float half_fov = 45.0f;    // +0x04: SetFieldOfView's h / 2 (512ths of a circle)
    float fov_ratio = 1.0f;    // +0x08: h / v
    float yon = 1000.0f;       // +0x14: the far plane
};

// A segment start + t dir, t in [0, 1].
struct PickSegment {
    glm::vec3 start{0.0f};
    glm::vec3 dir{0.0f};
};

// CCamera::GetScreenRay((float)x, (float)y), x and y relative to the viewport.
PickSegment screen_ray(const PickCamera& cam, float x, float y);

// DistSegmentTriangle² (0x100d37e0): the squared distance between the
// segment and the triangle and the segment parameter of the closest point.
struct SegmentTriangleDistance {
    float distance_squared = 0.0f;
    float t = 0.0f;
};
SegmentTriangleDistance segment_triangle_distance(const PickSegment& s, const glm::vec3& a, const glm::vec3& b,
                                                  const glm::vec3& c);
inline constexpr float kPickHitDistanceSquared = 1.1920929e-7f; // FLT_EPSILON

// The squared distance from p to the segment.
float point_segment_distance_squared(const PickSegment& s, const glm::vec3& p);

// A triangle as the pick tests see it: world space, `normal` facing out of
// the front face (a hit needs n . dir <= 0), the terrain record's flags.
struct PickTriangle {
    glm::vec3 v0{0.0f}, v1{0.0f}, v2{0.0f};
    glm::vec3 normal{0.0f};
    std::uint8_t flags = 0; // kTrianglePassable ...
    bool visual_two_sided = false;
};
using PickTriangleSink = std::function<void(const PickTriangle&)>;

// One actor of the scene.
struct PickActor {
    std::uint32_t mask = 2;     // GetCollisionRestrictionMask
    bool disabled = false;      // IsDisabled
    bool invisible = false;     // IsInvisible (slot 2 skips it)
    std::uint8_t volume = 0;    // the collision-volume type (+0x84): 2 sphere, 3 never hit, else polygons
    bool sphere_in_mode1 = false; // vf34: a hierarchical actor whose model definition's +0x5c is set
    glm::vec3 position{0.0f};   // +0x34
    float sphere_radius = 0.0f; // +0x64 x scale (+0x60)
    glm::vec3 bound_center{0.0f}; // the bounding sphere (vf28), the polygon test's gate
    float bound_radius = 0.0f;
    bool static_mesh = false;   // tested through a static collision mesh (0x100bebb0: t x 0.99, n . dir >= 0 culled)
    // The model's triangles in world space (polygon actors).
    std::function<void(const PickTriangleSink&)> triangles;
    // Hierarchical actors (vf260): each bone with a child, as (bone, child) world positions.
    bool hierarchical = false;
    std::vector<std::pair<glm::vec3, glm::vec3>> bones;
};

struct PickScene {
    // The world's triangles near the segment (the zone mesh), or empty.
    std::function<void(const PickSegment&, const PickTriangleSink&)> world;
    std::span<const PickActor> actors;   // tested in this order (dynamic actors, then static ones)
    int exclude_a = -1, exclude_b = -1;  // the view actor and its mount (indices), or -1
};

struct PickHit {
    int actor = -1;              // index into scene.actors, -1 for none
    float t = 1.0f;
    glm::vec3 point{0.0f};       // the clamp point: 0.001 short of the hit along the segment
    glm::vec3 normal{0.0f};      // info +0x3c: the last stored normal
    int passes = 0;              // passes run (1..4)
    float world_t = 1.0f;        // the nearest world triangle of the last pass
};

// PickActorAtScreenPoint after the ray: the actor whose mask has `required`
// (0: any) found in the first pass that finds one, skipping actors with
// `mask & skip`.
PickHit pick_actor(const PickSegment& seg, const PickScene& scene, std::uint32_t required, std::uint32_t skip);

// A visible wall or steep face stops the ray. Only solid upward ground accepts placement.
std::optional<glm::vec3> pick_ground_placement(const PickSegment& seg, const PickScene& scene);

} // namespace hailstone::game
