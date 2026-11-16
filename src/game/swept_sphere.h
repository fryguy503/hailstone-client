#pragma once

// The movement sweep's collision: what s3dCollideActorSweep @0x476560 does
// with a CCollisionInfoSphere and CCollision::CollideActor
// (EQGraphicsDX9.dll 0x100b9ed0), as the original's behaviour is described:
//  - the sphere (radius: the moving actor's bounding radius) moves from
//    `start` along `delta`; t is a fraction of delta and starts at 1;
//  - a triangle is skipped when it is passable (WLD polygon flag 0x10, EQG
//    triangle flag 1) or when n . delta > 0 (backfaceCollisionsIgnored, set
//    for any sweep of non-zero length), n = normalize((v0 - v1) x (v2 - v1))
//    in the file's vertex order; transparent polygons block movement;
//  - a hit is kept when its t <= the current t (later equal hits replace the
//    normal); once t is 0 the remaining triangles are skipped;
//  - a zero-length sweep is an overlap test: inside the radius, t = 0 and
//    the normal is the triangle's;
//  - the result: the sphere stops 0.001 short of the contact along the path
//    (the clamp point) and, after a hit, slides along the skitter direction
//    normalize(normalize(dir - n (dir . n)) + 0.01 n) for
//    max((1 - t) |delta| |s . dir|, 0.0001).
//
// The geometry (the sphere against one triangle with its face, edge and
// vertex cases, GetClampPoint and ComputeSkitter) is game/sphere_sweep.h:
// Hailstone's own implementation, which agrees with the original's recorded
// results within a small tolerance, not bit for bit. The DLL side (the zone
// mesh's records and their culls, the actor tests) follows the described
// behaviour, without recordings.

#include <cstdint>

#include <glm/glm.hpp>

namespace hailstone::game {

inline constexpr std::uint8_t kTrianglePassable = 0x1;   // the terrain record's bit 0 (WLD 0x10)
inline constexpr std::uint8_t kTriangleTransparent = 0x2; // bit 1: no or transparent material
// Render-effect queries only: the material's actual draw class is blended, additive or unmapped even when its
// legacy collision BlendMode is Masked. Movement collision deliberately does not inspect this flag.
inline constexpr std::uint8_t kTriangleNoVisualSurface = 0x8;
// Not a record flag: the triangle belongs to a placed object, which the original collides as an actor
// (CActorData::FindIntersectionSphere, which never asks the info's floor-only slot), not as the zone's terrain.
inline constexpr std::uint8_t kTriangleObject = 0x20;
// Not a record flag: the triangle's material is BlendMode::Invisible (solid, never drawn): CollisionMesh's ground and
// roof probes, which ask what is seen, leave it out.
inline constexpr std::uint8_t kTriangleInvisible = 0x40;
// Not a record flag: the triangle belongs to a ladder placement (game/zone_ladders.h, an actor of type 6), whose kept
// hit makes the player climb (CollisionHit_SetCollidingType).
inline constexpr std::uint8_t kTriangleLadder = 0x80;
// Not a record flag: Hailstone's solid stand-in for a ladder's rungs (game/zone_ladders.h ladder_slab), which only the
// player's movement collides with, and only with CollisionMesh::set_solid_ladders. Always with kTriangleLadder.
inline constexpr std::uint8_t kTriangleLadderSlab = 0x4;

// The collision info of one sweep (CCollisionInfoBase + CCollisionInfoSphere).
struct SphereSweep {
    glm::vec3 start{0.0f};
    glm::vec3 end{0.0f};
    glm::vec3 delta{0.0f};      // end - start (+0x1c)
    glm::vec3 direction{0.0f};  // delta normalised (+0x28)
    float length_inverse = 0.0f; // 1/|delta|, 0 when the length is not above 0 (+0x34)
    float t = 1.0f;             // collisionTime (+0x38)
    glm::vec3 normal{0.0f};     // surfaceNormal (+0x3c)
    bool collided = false;      // +0x58
    bool backfaces_ignored = false; // +0x68
    // The info's slot 5 (+0x9c, s3dCollideSphereWithWorld's twelfth argument): the floor and ceiling queries
    // (CDisplay::GetFloorHeight / GetCeilingHeight, the floor test) pass 1, and the terrain leaf then skips every
    // triangle whose plane normal has z == 0 exactly (vertical walls). Movement sweeps (s3dCollideActorSweep's
    // info, slot 5 @0x460e50) pass 0.
    bool floor_only = false;
    float radius = 0.0f;        // +0x74
    float radius_squared = 0.0f; // +0x84
    float radius_inverse = 0.0f; // +0x88

    static SphereSweep make(const glm::vec3& from, const glm::vec3& to, float radius);
    // Slot 8: on each axis |start| < 100000 - |delta|, and the radius is not NaN.
    bool valid() const;
};

// One triangle of a collision mesh, as CTerrain::BuildCollision stores it.
struct CollisionTriangle {
    glm::vec3 v0{0.0f}, v1{0.0f}, v2{0.0f}; // in the file's order
    glm::vec3 normal{0.0f}; // normalize((v0 - v1) x (v2 - v1)); zero for a degenerate one
    float radius = 0.0f;    // max(|v1 - v0|, |v2 - v0|)
    std::uint8_t flags = 0; // kTrianglePassable, kTriangleTransparent

    static CollisionTriangle make(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, std::uint8_t flags);
};

// The leaf's per-triangle filter (TTerrainCollisionTester<Sphere> vf0's
// checks), then the info's slot 0 @0x7a28e0 (game/sphere_sweep.h). Returns true on a hit,
// with the sweep's t, normal and collided updated.
bool collide_triangle(SphereSweep& sweep, const CollisionTriangle& tri);

// 0x509130's rule for vectors: zero when the length is within 2^-23 of 0.
glm::vec3 normalize_or_zero(const glm::vec3& v);

// s3dCollideActorSweep's outputs from a finished sweep: the clamp point
// (unk_CollisionInfo_GetClampPoint @0x7a2680) and the skitter
// (CCollisionInfoBase::ComputeSkitter @0x7a2720).
struct SweepOutput {
    glm::vec3 position{0.0f};
    glm::vec3 skitter_dir{0.0f};
    float skitter_dist = 0.0f;
};
SweepOutput sweep_output(const SphereSweep& sweep);

} // namespace hailstone::game
