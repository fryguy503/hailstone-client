#pragma once

// The original client's per-frame physics for the local player: the parts of
// PhysicsObject::Step @0x8d11a0 that move a player (heading, gravity, the
// environment's acceleration and friction, the velocity and the position),
// PhysicsObject::GetEnvCoefficients @0x8d09d0, PhysicsObject::ApplyFriction
// @0x8d2160, PhysicsObject vf3 @0x8d2040 (the push from the run speed),
// PlayerPhysics::IntegrateHeading @0x8d1e80 and PlayerBase::MyFeetAreOnGround
// @0x8cfca0. Collision (DoCollisionMovement) is the caller's.
//
// Sources: the original's MyFeetAreOnGround, vf0, vf4, PushAlongHeading,
// SetAccel, the strafe vf14, Step, GetEnvCoefficients, LandingAndFallCheck (at
// the end of this header, following the original exactly), vf3, ApplyFriction
// and SlowInMode5; each step names its address. All of them are checked against
// the original's vectors (tests/movement_vectors_test.cpp).
//
// Units are the original's: positions in world units, velocities in world
// units per physics tick, where one tick is 50 ms (Step: dt = elapsed_ms *
// 0.02), headings in 512ths of a turn. Positions here are world (zone file)
// coordinates (game/coords.h): the original's Y/X axes are swapped, and its
// heading 0 faces +Y; here heading 0 faces +X, counterclockwise, so the
// original's "cos(heading) to Y, sin(heading) to X" is "cos to x, sin to y".
//
// Float math: the original runs on the x87 FPU, which Direct3D 9 sets to 24-bit
// precision, so every operation rounds to a float; this code computes in float
// in the same order. The functions that follow the original exactly take the
// precision (fpu.h).
//
// Friction and the run-speed push are applied once per call (per frame), not
// scaled by the elapsed time, exactly as in the original: like the original,
// the ramp-up and the in-air damping depend on the frame rate, the steady
// speeds do not.

#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

#include "game/mechanics/fpu.h"

namespace hailstone::game::mechanics {

// Step: dt = elapsed_ms * 0.02f (one tick = 50 ms), at most 3 ticks (the
// physics manager's vf3 @0x588240 clamps it, 0x9d7070).
inline constexpr float kTicksPerMs = 0.02f;
inline constexpr float kMaxStepTicks = 3.0f;
// The zone's gravity per tick², from OP_NewZone (EQEmu default 0.4); the
// original starts every zone with 0.4 (0x9c73e0, stored to 0xddfaf4).
inline constexpr float kDefaultGravity = 0.4f;
// PlayerZoneClient::ResetVariables: the default ground acceleration and
// friction (0xddf680 = 0.8, 0xddf67c = 0.625, set by CDisplay).
inline constexpr float kGroundAcceleration = 0.8f;
inline constexpr float kGroundFriction = 0.625f;
// CheckRegionChange @0x508e10: a slippery (ice) region.
inline constexpr float kIceAcceleration = 0.02f;
inline constexpr float kIceFriction = 0.99f;
// The vertical friction on the ground and in the air (0x3f7d70a4).
inline constexpr float kAirFriction = 0.99f;

// PlayerPhysics fly modes (PlayerPhysics_SetFlyMode @0x8d0910; EQEmu FlyMode).
enum class FlyMode : std::uint8_t {
    Ground = 0,      // gravity on
    Flying = 1,
    Levitate = 2,
    Water = 3,
    Floating = 4,
    LevitateRun = 5, // levitation that keeps the run speed
};

// The four coefficients GetEnvCoefficients returns: the run-speed push is
// scaled by accel (xy, z), the velocity by friction (xy, z) every Step.
struct EnvCoefficients {
    float accel_xy = 0.0f;
    float accel_z = 0.0f;
    float friction_xy = 0.0f;
    float friction_z = 0.0f;
    bool operator==(const EnvCoefficients&) const = default;
};

// The player's physics state (PlayerBase fields; offsets for reference).
struct PhysicsBody {
    glm::vec3 feet{0.0f};          // position of the feet (the original's Z minus AvatarHeight +0x138)
    glm::vec3 velocity{0.0f};      // +0x70/+0x74 (horizontal) and +0x78 SpeedZ, units per tick
    float speed_run = 0.0f;        // +0x7c SpeedRun: the forward speed ProcessControls asks for
    float heading = 0.0f;          // +0x80, 0..512
    float speed_heading = 0.0f;    // +0x8c, heading units per tick
    float camera_angle = 0.0f;     // +0x90 look pitch in 512ths (positive up); swimming follows it
    float floor_height = 0.0f;     // +0x28 FloorHeight under the player (world z of the floor)
    float acceleration_friction = kGroundAcceleration; // AccelerationFriction (ice: 0.02)
    float area_friction = kGroundFriction;              // AreaFriction (ice: 0.99)
    std::uint8_t body_wet = 0;     // +0xa2 region type at the body (0 dry, 5..10 liquids)
    FlyMode fly_mode = FlyMode::Ground; // the physics object's +0xc
    bool sinks_in_water = false;   // +0x2c bSinksInWater
    bool mount_race = false;       // PlayerBase_IsMountRace
    bool being_flung = false;      // bBeingFlung (knockback)
    bool dead = false;             // StandState 120: Step's vf13 stops the speeds
    float fall_start_z = -1e27f;   // +0x20c: the feet's z where a fall began (LandingAndFallCheck)
    float buoyancy = 0.0f;         // 0xddfd78: 2.0 while CMD_MOVE_DOWN is held (ProcessControls)
    float walk_speed = 0.0f;       // +0x330: vf64, which 0x8d0800 compares with SpeedRun
    std::int32_t colliding_type = 0; // +0x24 CollidingType: DoCollisionMovement zeroes it on every step
                                     // that is not still; a door hit sets 3 (the switch's actor type)
};

struct DrunkenMotion {
    std::int32_t phase = -1;
    std::int32_t stride = 1;
    float amplitude = 0.35f;
};
// UpdateDrunkenness' physics parameters, called only when a positive value was decremented. A mount has its own
// physics object: update the player's parameters without seeding the mount's velocity.
void update_drunken_motion(PhysicsBody& body, DrunkenMotion& motion, std::int32_t value, bool seed_velocity = true,
                           const Fpu& fpu = Fpu{});
// PlayerPhysics vf44 @0x59de70 after integration, before landing/collision: a moving player's lateral sway is per
// step (not multiplied by dt); its phase advances only while either horizontal velocity is nonzero.
void apply_drunken_motion(PhysicsBody& body, DrunkenMotion& motion, const Fpu& fpu = Fpu{});

// PlayerBase::MyFeetAreOnGround @0x8cfca0: (Z - AvatarHeight) + SpeedZ is not
// above FloorHeight + 0.5 + 0.001; Z - AvatarHeight is body.feet.z here.
bool feet_on_ground(const PhysicsBody& body);

// PlayerPhysics::IntegrateHeading @0x8d1e80: heading += speed * dt, wrapped
// into [0, 512) by at most 7 subtractions or additions (else 0).
float integrate_heading(float heading, float speed_heading, float dt);

// What the environment does to a moving body this frame.
struct Environment {
    float gravity = kDefaultGravity;
    std::uint16_t zone_id = 0;   // zone 222 has its own water (physics manager vf6 @0x5882d0)
    bool on_ground_flag = false; // PlayerBase +0x124 (counts as on the ground; vehicles)
};

// get_env_coefficients() for the local player: subtracts gravity * dt from
// SpeedZ (not in water unless the body sinks) and returns the coefficients
// for ground, ice, air, the six liquids or levitation, whose hover (modes 2
// and 5) pulls SpeedZ toward a height over the floor. The player's answers
// to the original's queries are in physics.cpp (PlayerEnv; three are
// unknowns, docs/unknowns.md). `hover_floor` is the floor height under the
// body for the hover (the original probes 70 units down and falls back to
// FloorHeight).
EnvCoefficients env_coefficients(PhysicsBody& body, const Environment& env, float dt, float hover_floor);

// One Step for the local player (`elapsed_ms`: the whole milliseconds since
// the last Step, as the original's timer counts): dead stops the speeds (vf13), the strafe
// push (`strafe`, from strafe_push()), the heading, gravity and
// coefficients, the run-speed push along the heading (and along the look
// pitch, for swimming; vf3), friction (ApplyFriction), then the position
// moves by velocity * dt. `hover_floor` as for env_coefficients. Returns
// the displacement applied to body.feet (the caller then resolves walls and
// floors and runs the landing check).
struct SwimSteering {
    float pitch = 0.0f; // movement pitch in 512ths of a turn
    bool vertical_only = false;
};
glm::vec3 step(PhysicsBody& body, const Environment& env, float elapsed_ms, const glm::vec2& strafe,
               float hover_floor, std::optional<SwimSteering> swim = std::nullopt);

// PlayerZoneClient::PushAlongHeading @0x506510: velocity += amount along the heading.
void push_along_heading(PhysicsBody& body, float amount);

// sin/cos of a heading in 512ths. The original reads a sine table
// (unk_pinstSinCosTable slots +8 and +0xc); this computes them directly.
float heading_sin(float heading);
float heading_cos(float heading);

// ------------------------------------------------- the original's Step, env, landing
//
// PhysicsObject::Step, GetEnvCoefficients and LandingAndFallCheck, with
// vectors recorded from the original for all three.
// The functions below follow them on the original's own fields, with
// every question the original asks a virtual or another function turned
// into a query, so tests/movement_vectors_test.cpp can feed them the
// recorded answers. env_coefficients() and the controller's landing run on
// them. Fpu: the x87 precision (fpu.h); in game 24-bit.

// The spawn fields GetEnvCoefficients reads (PlayerBase offsets).
struct EnvSpawn {
    std::int32_t colliding_type = 0;  // +0x24 CollidingType (6: 0.8, 0.8, 0.625, 0.625)
    float floor_height = 0.0f;        // +0x28 FloorHeight
    float acceleration_friction = kGroundAcceleration; // +0x20
    float area_friction = kGroundFriction;             // +0x1c
    bool sinks_in_water = false;      // +0x2c bSinksInWater
    std::uint8_t body_wet = 0;        // +0xa2 BodyWet
    std::uint8_t type = 0;            // +0x125 Type (0 player, 1 NPC)
    bool being_flung = false;         // +0x163 bBeingFlung
    std::int32_t spline_id = 0;       // +0x180 SplineID
    float avatar_height = 0.0f;       // +0x138 AvatarHeight
};

// The physics object's position and speeds (PlayerBase +0x64..+0x7c; the
// original's Y and X fields are named y and x).
struct EnvObject {
    float y = 0.0f, x = 0.0f, z = 0.0f;
    float speed_y = 0.0f, speed_x = 0.0f, speed_z = 0.0f;
    float speed_run = 0.0f;
};

// What GetEnvCoefficients asks while it runs, in the order it asks.
class EnvQueries {
public:
    virtual ~EnvQueries() = default;
    virtual bool spawn_vf90() = 0;            // PlayerClient +0x90 (a race check): gravity although not walking
    virtual bool possibly_stuck() = 0;        // 0x8cfc90: PlayerBase +0x124
    virtual bool collision_zone_loaded() = 0; // pinstCollision +0x44 (asked up to three times)
    virtual float gravity() = 0;              // the physics manager's +0x10
    virtual bool mount_race() = 0;            // PlayerBase_IsMountRace
    virtual bool corpse() = 0;                // PlayerClient::IsCorpse
    virtual bool climbing() = 0;              // this->vf9(spawn), asked for spawn type 1
    virtual bool swim_zone() = 0;             // the physics manager's +0x18 (zone 222's water)
    virtual bool feet_on_ground() = 0;        // PlayerBase::MyFeetAreOnGround
    virtual bool spawn_vf58() = 0;            // PlayerClient +0x58 (race and gender info, not +0x54)
    virtual bool controlled() = 0;            // PlayerClient +0x6c: this is pinstControlledPlayer
    virtual bool spawn_vf50() = 0;            // PlayerClient +0x50: race and gender info set
    virtual float floor_at(float y, float x, float z) = 0; // the physics manager's +0x14: -1e27 without a floor
    virtual bool rising() = 0;                // 0x8d0800
    virtual float buoyancy() = 0;             // this->vf15(spawn, &out)
};

// PhysicsObject::GetEnvCoefficients @0x8d09d0: changes the object's z speed
// (gravity, the levitation and water pulls) or, in levitation mode 4, its
// z, and returns the four coefficients; nullopt when the original leaves
// them unwritten (a BodyWet that is not a liquid). `walking` is the physics
// object's +4 (fly mode 0, PlayerPhysics_SetFlyMode), `mode` its +0xc.
std::optional<EnvCoefficients> get_env_coefficients(bool walking, std::int32_t mode, const EnvSpawn& spawn,
                                                     EnvObject& object, float dt, EnvQueries& q,
                                                     const Fpu& fpu = Fpu{});

// The spawn fields PlayerZoneClient_LandingAndFallCheck reads and writes.
struct LandingSpawn {
    static constexpr float kNoFloor = -1e27f; // 0x9c5760

    float floor_height = 0.0f;  // +0x28
    float avatar_height = 0.0f; // +0x138: z is the feet plus this
    float y = 0.0f, x = 0.0f, z = 0.0f; // +0x64..+0x6c
    float speed_y = 0.0f, speed_x = 0.0f, speed_z = 0.0f; // +0x70..+0x78
    float heading = 0.0f;       // +0x80
    float fall_start_z = kNoFloor; // +0x20c: the z where the fall began, -1e27 when not falling
    std::uint8_t body_wet = 0;  // +0xa2
    std::uint8_t type = 0;      // +0x125
    bool being_flung = false;   // +0x163
    std::int32_t fling_active_timer = 0; // +0x164
    std::int32_t fling_timer_start = 0;  // +0x168
};

// What the landing check asks.
class LandingQueries {
public:
    virtual ~LandingQueries() = default;
    virtual bool skip_physics() = 0;        // PlayerClient +0xb0
    virtual bool spawn_vf58() = 0;          // PlayerClient +0x58
    virtual bool player_spawn() = 0;        // IsSpawnInfoPlayerSpawn
    virtual std::int32_t sound_race() = 0;  // PlayerClient +0x88 (0x7f: no landing sound)
    virtual bool rider_zone_client() = 0;   // 0x58cfb0: a non-player spawn's rider
    virtual bool no_recovery() = 0;         // PlayerClient +0x7c
    // CDisplay::GetFloorHeight from (y, x, z) down; -1e27 without a floor.
    virtual float floor_below(float y, float x, float z) = 0;
};

// The world values the landing check reads.
struct LandingWorld {
    bool connected = true;        // the zone connection (the vehicle-fall packet)
    std::uint8_t zone_type = 0;   // __ZoneType 0xddfaf8: 0 has no under-world recovery
    float world_bottom = -1e27f;  // 0xddfb50: feet below it trigger the recovery
    float safe_floor = 0.0f;      // 0xddfb4c: a recovered floor must be below it minus 10
};

struct LandingResult {
    bool landed = false;
    std::optional<float> fall_speed; // IHaveFallen(|SpeedZ + 2.5|) was called with it
    bool fall_on_rider = false;      // ... on the rider's zone client (a non-player spawn)
    bool land_sound = false;         // sound 0x72
    bool vehicle_fall_sent = false;  // 0x65c7: a flung player spawn landed
    bool fling_stopped = false;      // PlayerClient +0x30(0)
    // 0x5024b0: placed on a recovered floor (y, x, z, heading).
    std::optional<glm::vec4> placed;
};

// PlayerZoneClient_LandingAndFallCheck @0x506e20, after each Step.
LandingResult landing_and_fall_check(LandingSpawn& spawn, LandingQueries& q, const LandingWorld& world,
                                     const Fpu& fpu = Fpu{});

// The math table (CMath): 512 floats,
// cos(i * pi/256) at +4 and sin at +0x804, the quadrant entries exact; its
// vtable +8 (sin) and +0xc (cos) return table[(int)trunc(angle) & 511], with
// no interpolation. heading_sin / heading_cos above use it.
float table_sin(float angle);
float table_cos(float angle);

// What PhysicsObject::vf3 asks: the math table and the object's own virtuals.
class PushQueries {
public:
    virtual ~PushQueries() = default;
    virtual double cos(float angle) { return table_cos(angle); } // math table +0xc (on the FPU)
    virtual double sin(float angle) { return table_sin(angle); } // math table +8
    virtual double vf0(float value) = 0;                          // PhysicsObject vf0 (unrounded)
    virtual void add_push(float push[3]) = 0;                     // PhysicsObject vf4 (+0x10), with a spawn
};

// PhysicsObject::vf3 @0x8d2040: with a SpeedRun that
// is not 0, push[0] += cos(heading) * SpeedRun and push[1] += sin(heading) *
// SpeedRun (the products unrounded), and with a camera angle that is not 0,
// push[2] += vf0(sin(angle) * SpeedRun); with a spawn vf4 adds to the push;
// then push[0..1] *= a and push[2] *= b where not 0 (a NaN is scaled).
// The arrays are in the physics block's order (the original's y, x, z: the
// world's x, y, z).
void run_speed_push(float speed_run, float heading, float camera_angle, float push[3], bool spawn, float a, float b,
                    PushQueries& q, const Fpu& fpu = Fpu{});

// PhysicsObject::ApplyFriction @0x8d2160: speed[0..1] *= a and speed[2] *= b
// where not 0; magnitudes below 2^-23 become 0; then the push is added.
void apply_friction(float speed[3], const float push[3], float a, float b, const Fpu& fpu = Fpu{});

// PhysicsObject_SlowInMode5 @0x8d0800: levitation mode 5 and the walk speed
// (the spawn's vf64, unrounded) at least SpeedRun.
bool slow_in_mode5(std::int32_t levitation_mode, double walk_speed, float speed_run);

// PlayerClient_IsSmallBoatRace (PlayerClient vf +0x58): a vehicle race (the
// race/gender info's flags +0x2c bit 0) that is not a large ship (bit 1).
inline bool is_small_boat_race(bool has_race_info, std::uint32_t race_flags) {
    return has_race_info && (race_flags & 1) && !(race_flags & 2);
}

// PhysicsObject::vf15 @0x59de30, the buoyancy: for the controlled player the
// float at 0xddfd78 (ProcessControls' held_scale: 2.0 while CMD_MOVE_DOWN is
// held) when it is above 0; otherwise the value is left as it was.
inline float physics_buoyancy(bool controlled, float move_down, float current) {
    return controlled && 0.0f < move_down ? move_down : current;
}

// Step @0x8d11a0's time: (now - last) milliseconds, unsigned, times 0.02.
float elapsed_steps(std::uint32_t now, std::uint32_t last, const Fpu& fpu = Fpu{});

// Step's integration of one axis: position + speed * dt (the product unrounded).
float integrate_axis(float position, float speed, float dt, const Fpu& fpu = Fpu{});

} // namespace hailstone::game::mechanics
