#pragma once

// PlayerZoneClient_DoCollisionMovement @0x508070: moving a spawn from where
// its actor stands to the position physics integrated, through the zone's
// collision.
//
// Follows the original's described behaviour step by step and is checked
// against its recorded results (tests/movement_vectors_test.cpp). The
// original keeps its locals in a frame and computes on the x87; this keeps
// each rounding and each NaN rule (fpu.h). What it asks a virtual, the scene graph, the collision interface
// (EQGraphicsDX9.dll) or another function is a CollisionQueries call, in the
// original's order; the sweep itself (s3dCollideActorSweep @0x476560) is a
// query too: game/zone_sweep.h implements it for the client.
//
// In short: the step goes into the 20-entry movement ring; a target outside
// the scene's box grown by 2000 puts the spawn back at its last good position
// and stops it. Otherwise the start (the actor's location) and the target are
// lifted by the spawn's height offset (vf94) and the swept sphere (radius:
// the actor's bounding radius) moves from one to the other: once, or ten
// times for the controlled player, each time continuing from where it stopped
// along the skitter direction for the skitter distance the sweep returns; the
// tenth hit may slide back toward the start. The controlled player's end is
// checked against the BSP (scene +0x4c, IsPointInWorld) and CanIFitHere, and
// reverted when either fails. A move of less than 0.001 keeps the spawn where
// it was (last good position) after its third step; otherwise the actor is
// placed. After a hit, a spawn that can swim jump has its speed turned along
// the move. CollidingType (+0x24) is zeroed on every step that is not still;
// only the sweep's hit callback (actor hits) sets it again.

#include <array>
#include <cstdint>

#include "game/mechanics/fpu.h"

namespace hailstone::game::mechanics {

// One MovementStats entry (SDoCollisionMovementStats, 0x74 bytes): the
// physics block before (+0x64..+0x90), the target, the floor height before
// and after, the physics block after.
struct MovementStat {
    std::array<float, 12> before{};
    std::array<float, 3> target{};
    float floor_before = 0.0f;
    float floor_after = 0.0f;
    std::array<float, 12> after{};
};

// The spawn fields DoCollisionMovement reads and writes (PlayerBase offsets).
struct CollisionSpawn {
    // The physics block +0x64..+0x90 in the original's order: y, x, z,
    // speed y, x, z, SpeedRun, heading, angle, accel angle, SpeedHeading,
    // +0x90. The position is physics[0..2].
    std::array<float, 12> physics{};
    float floor_height = 0.0f;          // +0x28
    std::int32_t colliding_type = 0;    // +0x24 CollidingType
    std::uint8_t body_wet = 0;          // +0xa2
    std::uint8_t possibly_stuck = 0;    // +0x124
    std::uint8_t flag_19d = 0;          // +0x19d
    std::uint32_t stand_time = 0;       // +0x360: the display time when it stood still
    std::uint32_t step_count = 0;       // +0x1060
    std::array<MovementStat, 20> ring{}; // +0x528 MovementStats
    std::uint32_t ring_index = 0;       // +0xe3c
    std::uint32_t ring_count = 0;       // +0xe38
    std::array<float, 3> last_good{};   // +0x284: the position 0x507430 saved after the last step
};

using Vec3f = std::array<float, 3>; // the original's order (y, x, z)

// The collision info 0x509190 builds for the overlap test (pinstCollision +0xc).
struct CollisionTestInfo {
    Vec3f from{}, to{};
    float radius = 0.0f, radius_squared = 0.0f, radius_inverse = 0.0f;
    bool has_object = false;
    std::uint8_t flag = 0;
};

// The sweep's results (s3dCollideActorSweep's three outputs).
struct SweepResult {
    Vec3f position{};    // where the sphere stopped (the clamp point)
    Vec3f skitter_dir{}; // the slide direction after a hit
    float skitter_dist = 0.0f; // the slide distance; 0 without a hit
};

// What DoCollisionMovement asks and tells, in its order.
class CollisionQueries {
public:
    virtual ~CollisionQueries() = default;
    virtual Vec3f actor_location() = 0;                 // ActorClient::GetActorLocation (the actor at +0xea4)
    virtual bool zone_collision_loaded() = 0;           // pinstCollision +0x44
    // pinstSceneGraph +0x50: the world's box; false (-1) when no world is loaded.
    virtual bool scene_box(Vec3f& min, Vec3f& max) = 0;
    virtual void zone_client() = 0;                     // PlayerClient +0x44 (the result is not used)
    virtual bool mounted() = 0;                         // EQPlayer::GetMount != 0
    virtual bool is_vehicle_race() = 0;                 // PlayerClient +0x50
    virtual bool has_property(std::uint32_t property) = 0; // PlayerBase::HasProperty(property, 0, 0)
    virtual double height() = 0;                        // PlayerClient +0x94, unrounded on the FPU
    virtual double collision_radius() = 0;              // the +0x101c actor's +0x44, unrounded
    virtual bool moving_state() = 0;                    // 0x506c90
    virtual bool rider() = 0;                           // PlayerBase_GetRider != 0
    virtual void set_rider_stand_time(std::uint32_t time) = 0; // the rider's +0x34
    virtual std::uint32_t display_time() = 0;           // CDisplay +0x154
    // PlayerClient +0xe8: the sweep request (start x, y, z, move x, y, z, radius); false: no collision.
    virtual bool sweep_request(const std::array<float, 7>& request) = 0;
    virtual bool is_controlled() = 0;                   // PlayerClient +0x6c: this is the controlled player
    // s3dCollideActorSweep @0x476560 (the actor, from, to, the filter: with a rider
    // CollisionFilter_MovementWithRider, else CollisionFilter_Movement; the hit callback
    // CollisionHit_SetCollidingType when `hit_callback`). The results start as `out`.
    virtual void sweep(const Vec3f& from, const Vec3f& to, bool rider_filter, bool hit_callback, SweepResult& out) = 0;
    virtual void no_collide() = 0;                      // 0x509a00: the touch-switch overlap test
    virtual void move_without_collision(const Vec3f& target, Vec3f& out) = 0; // 0x507730
    virtual bool in_scene_region(const Vec3f& p) = 0;   // pinstSceneGraph +0x4c (IsPointInWorld)
    virtual bool can_i_fit_here(const Vec3f& p, std::uint32_t flag) = 0; // PlayerZoneClient::CanIFitHere
    virtual void clear_stuck(std::uint32_t arg) = 0;    // 0x509230
    virtual bool check_stuck() = 0;                     // 0x5064c0
    virtual bool collision_test(const CollisionTestInfo& info) = 0; // pinstCollision +0xc
    virtual void set_actor_location(const Vec3f& p) = 0; // ActorClient::SetActorLocation
    virtual bool is_player_spawn() = 0;                 // IsSpawnInfoPlayerSpawn
    virtual void after_place() = 0;                     // 0x509210
    virtual bool can_swim_jump_now() = 0;               // PlayerClient_CanSwimJumpNow
    virtual bool on_ladder() = 0;                       // 0x59fb80
    virtual void finish_move() = 0;                     // 0x507430 (saves the last good position)
    virtual void after_move(const Vec3f& from, const Vec3f& actor) = 0; // EmptyFunction_00509220
};

struct CollisionMove {
    bool stopped = false;  // the return value: the speed's square is not above 0
    bool early = false;    // the target was outside the scene's box: the ring entry's after part is not written
    bool held = false;     // not stuck, but kept at the last good position (still, or barely moved): SpeedZ zeroed
};

// PlayerZoneClient_DoCollisionMovement @0x508070. `target` is the original's
// in/out argument (lifted, or replaced by the actor's location when still).
CollisionMove do_collision_movement(CollisionSpawn& spawn, Vec3f& target, CollisionQueries& q,
                                    const Fpu& fpu = Fpu{});

// 0x5062a0, the radius eqgame gives a spawn's actor as its collision
// volume (CRender::BuildActorForSpawn passes it to CreateActorInstance, and
// PlayerZoneClient::ChangeHeight sets it with the actor's vf48,
// SetCollisionVolumeRadius): AvatarHeight x 0.5 x 0.95 for a player, x 0.35
// for an NPC (+0x125 == 1; x 0.9 more for race 31), doubled for a mount race.
// Unrounded, as the original leaves it on the FPU.
double collision_volume_radius(float avatar_height, bool npc, std::uint32_t race, bool mount_race,
                               const Fpu& fpu = Fpu{});

// CHierarchicalActor's collision volume vf30 @0x10045fd0 for type 1: the model
// definition's +0x38 x the actor scale (V+0x60, the spawn's +0x31c) x
// COFactor (V+0x8c, 1.0 with the shipped moddat.ini). Unrounded.
double model_sweep_radius(float model_radius, float scale, float co_factor = 1.0f, const Fpu& fpu = Fpu{});

// The player radius used for movement and ceiling sweeps. Native local-player
// probes on 2026-11-12 selected type 2, reading the explicit volume radius:
// about 0.890625 at AvatarHeight 1.875 (gnome), and 1.57343745 at 3.3125
// (half-elf), unchanged while crouching. Both support 0x5062a0's player rule
// within one float ULP; the captures used 53-bit/toward-zero x87 rounding.
// Other sizes, large races and mounts still need measurement (docs/unknowns.md).
struct SweepRadiusSpawn {
    float avatar_height = 0.0f; // +0x138
    std::uint32_t race = 0;
};
float sweep_radius(const SweepRadiusSpawn& spawn);

// PlayerClient vf94 @0x5063d0: the collision sphere's centre above the spawn's z. The local and remote probes
// use the same posture, environment and large-race rules; the caller supplies its currently qualified radius.
float collision_height_offset(std::uint8_t stand_state, std::uint8_t body_wet, std::uint32_t race,
                               float avatar_height, float radius);

// 0x509130: a vector scaled to length 1, or zero when its length is
// within 2^-23 of 0 (the length unrounded, compared with the constants).
void normalize_vector(float v[3], const Fpu& fpu = Fpu{});

// 0x507430's first half: with the zone's collision loaded, a position
// outside the scene's box grown by 2000 stops the spawn and puts it back at
// `last_good`; otherwise the position becomes the last good one. Returns
// false when it was put back.
bool keep_in_scene(CollisionSpawn& spawn, bool collision_loaded, const Vec3f& min, const Vec3f& max,
                   const Fpu& fpu = Fpu{});

} // namespace hailstone::game::mechanics
