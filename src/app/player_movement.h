#pragma once

// Glue between the client and game/mechanics/movement.h: gathers the
// character's values from the connection (spawn speeds, profile, buffs,
// inventory) and the zone (water regions, collision), runs the original's
// movement for one frame and reports what the original reports (OP_Jump,
// OP_EnvDamage, OP_SafeFallSuccess, OP_SetRunMode).

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "game/mechanics/collision_movement.h"
#include "game/mechanics/ladder.h"
#include "game/mechanics/movement.h"
#include "game/mechanics/movement_inputs.h"
#include "game/mechanics/regions.h"
#include "game/mechanics/can_i_fit_here.h"
#include "game/mechanics/vehicle.h"
#include "game/mechanics/floor_height.h"
#include "game/mechanics/switches.h"
#include "game/swept_sphere.h"

namespace hailstone::assets { class ZoneRegions; }
namespace hailstone::net { class GameConnection; }

namespace hailstone::game::mechanics { class Character; }
namespace hailstone::game { struct Spawn; }

namespace hailstone::app {

struct MovementKeys {
    bool forward = false, back = false;
    bool turn_left = false, turn_right = false;
    bool strafe_left = false, strafe_right = false;
    bool jump = false;        // went down this frame
    bool mouse_look = false;  // __MouseLook (game/mouse_look.h): turn keys strafe
    bool move_down = false;   // CMD_MOVE_DOWN held: sinks while levitating (ProcessControls' 0xddfd78)
    bool both_buttons = false; // 0xddf673: both mouse buttons in mouse look move forward
    std::int32_t camera_index = 0; // CDisplay::cameraType: 0 first person, 6 the wheel-follow (chase) camera
    // The active camera's fields the camera keys move, and whether the zoom and camera turn keys act on it
    // (game/cameras.h camera_active).
    game::mechanics::ControlCamera camera;
    bool camera_active = false;
};

// The zone's collision as the original's DoCollisionMovement asks it
// (game/mechanics/collision_movement.h): the swept sphere against the zone's
// mesh and the doors, the world box and the BSP; the floor and ceiling probes
// for the landing check and CanIFitHere. Positions are world coordinates.
class MoveWorld {
public:
    virtual ~MoveWorld() = default;
    virtual bool collision_loaded() const = 0;                                       // pinstCollision +0x44
    virtual std::optional<std::pair<glm::vec3, glm::vec3>> world_box() const = 0;    // pinstSceneGraph +0x50
    struct DoorHit {
        std::uint8_t id = 0;
        std::uint8_t type = 0; // EQSwitch +0x26
    };
    struct Sweep {
        game::SweepOutput out;  // the clamp point and the skitter
        bool collided = false;  // CollideActor's result
        bool door_hit = false;  // a door (switch, actor type 3) hit was kept
        // The doors whose hit was kept when it was tested (CollideActor calls the hit callback for each), in order.
        std::vector<DoorHit> door_hits;
        // The zone mesh's hit was on a ladder placement (game/zone_ladders.h, an actor of type 6). It is tested
        // before the doors; the original's order among the actors is not traced.
        bool ladder_hit = false;
    };
    // A floor switch the floor test's sweep hit and kept: CollsnCallbackForFloor's switch, type and distance (the
    // hit's t over 1/|d|: how far below the start).
    struct FloorSwitchHit {
        std::uint8_t id = 0;
        std::uint8_t type = 0;
        float distance = 0.0f;
    };
    // s3dCollideActorSweep: the sphere from `from` to `to` (a zero-length one is an overlap test).
    virtual Sweep sweep(const glm::vec3& from, const glm::vec3& to, float radius) const = 0;
    // s3dCollideSphereWithWorld for GetCeilingHeight: the same sphere in the floor-only mode (its twelfth
    // argument 1: the terrain's walls, n.z == 0, are skipped; game/swept_sphere.h), without the doors (its actor
    // filter 0x4764d0 refuses switch actors).
    virtual Sweep world_sweep(const glm::vec3& from, const glm::vec3& to, float radius) const = 0;
    virtual bool in_world(const glm::vec3& p) const = 0;                             // pinstSceneGraph +0x4c
    virtual std::optional<float> floor_below(const glm::vec3& p, float step_up) const = 0;
    // s3dCollideSphereWithWorld for the floor test: the zone, the floor switches and the vehicles (FloorActorFilter),
    // in the floor-only mode (the terrain's walls are skipped); the stop point's z. CollsnCallbackForFloor's
    // vehicle: when the stop is on a vehicle spawn's model (PlayerClient_IsVehicleRace), its id goes to `vehicle`
    // (the spawn's +0x150, 0x8cfd10); other spawns' models are not floors.
    virtual float floor_sweep(const glm::vec3& from, const glm::vec3& to, float radius,
                              std::uint32_t* vehicle = nullptr, std::vector<FloorSwitchHit>* switches = nullptr) const = 0;
    // 0x509a00's overlap test: a still sphere at `at` against the switches that act on touch (filter 0x475c00:
    // a switch actor whose switch passes 0x510210); the doors it touches, in order.
    virtual std::vector<std::uint8_t> touch_overlap(const glm::vec3&, float) const { return {}; }
    virtual std::optional<float> ceiling_above(const glm::vec3& p) const = 0;
};

// The selected remote CollisionPass actor-contact state (ladder / switch), using the same collision sphere as
// the local movement adapter. This does not replace the server's position or the remote predictor's motion.
std::int32_t remote_collision_type(const game::Spawn& spawn, const glm::vec3& from, const glm::vec3& to,
                                    const MoveWorld& world);

class PlayerMovement {
public:
    void reset(const glm::vec3& feet, float yaw);

    struct Frame {
        glm::vec3 feet{0.0f};
        float yaw = 0.0f;
        glm::vec3 velocity{0.0f}; // world units per physics tick (what OP_ClientUpdate carries)
        bool moving = false;
        std::vector<std::string> messages; // chat lines (eqstr texts)
        bool jumped = false;               // OP_Jump sent
        bool landed = false;
        std::optional<std::int32_t> fall_damage; // IHaveFallen ran: the damage (0: none or the first fall)
        float landing_speed = 0.0f;        // SpeedZ at the landing
        bool land_sound = false;           // LandingAndFallCheck's landing sound (0x72)
        bool wet = false;                  // the body is in a liquid region
        bool flying = false;               // levitating or flying (the physics object's +0xc)
        bool running = true;               // run mode (__RunWalkState), for the walk / run animation
        bool follow_off = false;           // ProcessControls' TurnOffAutoFollow (turn, strafe or back keys)
        std::vector<std::int32_t> attacks; // ProcessControls' DoAttack(13 / 14) calls, for the caller to run
        // The standing jump or the auto-duck crouched or stood up: the caller sends it (unk_SetStandState 111 /
        // 100).
        std::optional<std::uint32_t> stand_state;
        // ProcessControls' forward key with AllowAutoStand while unable to move: CEverQuest::Sit (sitting) or the
        // loot window's close (looting), for the caller to run.
        bool sit_toggle = false;
        bool close_loot = false;
        // CameraAngle after ProcessControls (LookSpring eases it back to level), radians as `pitch` takes it.
        float pitch = 0.0f;
        // The camera keys moved the active camera (ProcessControls' camera setters): its fields after.
        std::optional<game::mechanics::ControlCamera> camera;
        bool camera_pitch_set = false;
        // For the animation choice (game/spawn_animation.h).
        float speed_run = 0.0f;            // SpeedRun +0x7c
        float speed_heading = 0.0f;        // SpeedHeading +0x8c (turning)
        std::uint8_t body_wet = 0;         // BodyWet +0xa2
        std::uint8_t feet_wet = 0;         // the region type at the feet (FeetWet +0xa1's writer is not traced)
        std::uint8_t head_wet = 0;         // HeadWet: the region type at the eyes (5 = under water)
        std::int32_t colliding_type = 0;   // CollidingType +0x24 (6: on a ladder)
        // With set_trace: DoCollisionMovement's decisions this frame (sweeps, the world test, CanIFitHere with
        // its floor and ceiling, the overlap test, the result), one line each (testing).
        std::vector<std::string> trace;
    };
    // One frame: `yaw`/`pitch` are the camera's (radians); mouse look turns
    // the heading directly as the original's HandleMouseLook does. `step_ms` above 0 is the game clock's step
    // (core/step_clock.h): the physics steps by it; 0 measures the whole milliseconds since the last update.
    Frame update(const MovementKeys& keys, float yaw, float pitch, float elapsed_ms, std::uint32_t now_ms,
                 net::GameConnection& conn, const assets::ZoneRegions* regions, float floor_offset,
                 const MoveWorld* world, const game::mechanics::Character* character = nullptr,
                 std::uint32_t step_ms = 0, bool local_passive = false);
    // The actor's bounding radius (the swept sphere's; PlayerClient +0x101c's vf44):
    // the skeleton's radius times the model scale times COFactor (1).
    void set_collision_radius(float r) { radius_ = r; }
    // PlayerClient +0x11b4, the ceiling CanIFitHere last found room under (none before it found one), and
    // FloorHeight (+0x28): what CDisplay_UpdateListenerEnvironment reads.
    std::optional<float> player_ceiling() const { return player_ceiling_; }
    std::optional<float> floor_height() const { return floor_valid_ ? std::optional<float>(floor_) : std::nullopt; }
    float collision_radius() const { return radius_; }
    // PlayerClient +0x150 (PlayerClient_GetStandingOn): the vehicle the body stands on, as the last floor test found
    // it (0: none).
    std::uint32_t standing_on() const { return standing_on_; }
    // do_stand's PlayerZoneClient::CanIFitHere(+0x64, 0) on the local player between frames, at the body's position
    // (the player's, before it mounts): whether it fits, and the room between the ceiling it last found room under
    // (+0x11b4; none yet: the open sky) and the floor cache's floor (+0x1070) afterwards.
    struct RoomCheck {
        bool fits = true;
        double room = 1e30;
    };
    RoomCheck can_i_fit_here_now(const MoveWorld* world, float floor_offset, float special_height, std::uint32_t now);
    // The sphere's centre above the feet in the last update: AvatarHeight + the height offset (vf94).
    float sweep_lift() const { return lift_; }
    // The player's race (PlayerClient vf88), for the height offset (vf94).
    void set_race(std::uint32_t race) { race_ = race; }
    // The jump's crouch window (game::mechanics::jump_crouch_window of the controlled spawn's duck clip).
    void set_crouch_window(std::uint32_t ms) { crouch_window_ = ms; }
    // 0x506c90's first test: the animation object's vf24 (a jump animation is +0x10c0), as the renderer last
    // chose it; nothing without a drawn model (then a jump counts for kJumpWindowMs after it).
    void set_jump_animation(std::optional<bool> playing) { jump_animation_ = playing; }

    const game::mechanics::MovementController& controller() const { return ctl_; }
    game::mechanics::ControlsState& controls_state() { return ctl_.controls_state(); }
    void set_autorun(std::uint8_t bits) { ctl_.set_autorun(bits); }
    // A movement key command pressed or released (MovementController::command).
    bool command(int command, bool down, bool accept, std::uint8_t autorun_bits = 0) {
        return ctl_.command(command, down, accept, autorun_bits);
    }
    // Forward, back or a strafe key held, or forward/back autorun on.
    bool moving_keys() const {
        const auto& k = ctl_.held_keys();
        return k.forward || k.backward || k.strafe_left || k.strafe_right || (ctl_.autorun() & 3) != 0;
    }
    // Test hooks. These move the actor too (as a teleport does), so the next
    // collision sweep starts where the body now is.
    void lift(float units) {
        ctl_.body.feet.z += units;
        moved_by_hand();
    }
    // A spell's push (HitBySpell: SpeedY/X/Z += table(heading) * push_back, push_up).
    void push(const glm::vec3& dv) { ctl_.body.velocity += dv; }
    void place(const glm::vec3& feet) {
        ctl_.body.feet = feet;
        ctl_.body.velocity = glm::vec3(0.0f);
        moved_by_hand();
    }
    // A click on a ladder (Ladder_MountClickedLadder, app/player_ladder.cpp):
    // places the body on it. Returns false when nothing happened; `too_far`
    // is set when the original prints 12008.
    bool mount_ladder(const game::mechanics::LadderSwitch& ladder, const MoveWorld* world, float avatar_height,
                      bool& too_far);

    const game::mechanics::CollisionSpawn& collision_state() const { return collision_; }
    // ProcessSwitches' view of the player (a lift's rider) and its reset of CollisionCounter.
    game::mechanics::SwitchSpawn rider() const {
        return {ctl_.body.feet.x, ctl_.body.feet.y, static_cast<double>(radius_)};
    }
    void reset_collision_counter() { collision_.step_count = 0; }
    // PlayerClient::TouchingSwitch (game/mechanics/touching_switch.h) for door `id`, run by the caller with the
    // connection's switches; returns whether +0x2d4 is cleared afterwards.
    using TouchHandler = std::function<bool(std::uint8_t id)>;
    void set_touch_handler(TouchHandler h) { touch_ = std::move(h); }
    // CollsnCallbackForFloor's inputs: the spawn's Height (+0x13c) and the zone (unk_CurrentZoneID).
    void set_touch_context(float height, std::int32_t zone) {
        touch_height_ = height;
        touch_zone_ = zone;
    }
    // pTouchingSwitch (+0x2d4) as a door id, -1 for none.
    std::int32_t touching_switch() const { return touching_switch_; }
    // Testing: record each frame's collision decisions in Frame::trace.
    void set_trace(bool on) { trace_ = on; }
    // At the helm of a small boat (game/boats.h steer_boat): the movement keys sail the boat, so ProcessControls sees
    // none of them held (they stay held for the helm and for afterwards).
    void set_at_helm(bool on) { at_helm_ = on; }
    // The isometric camera's movement relative to the camera (game::iso_move): the player was turned to the way the
    // keys point, so ProcessControls sees only forward held (`forward`) or nothing, and no back, turn or strafe key
    // (they stay held for afterwards). nullopt: the keys as held.
    void set_key_override(std::optional<bool> forward) { key_override_ = forward; }

private:
    // A test placement is a teleport: the actor and the floor cache start over there.
    void moved_by_hand() {
        actor_valid_ = false;
        floor_valid_ = false;
        collision_.step_count = 0;
        floor_cache_.location = {std::nanf(""), std::nanf(""), std::nanf("")};
    }

public:
    std::int32_t air_supply() const { return air_; }
    std::int32_t max_air_supply() const { return max_air_; } // the last second's MaxAirSupply
    // The last frame's character inputs (run / walk speed, the speed modifier, the weight factor; testing).
    const game::mechanics::CharacterInput& last_character() const { return last_ch_; }
    const game::mechanics::MovementFacts& last_facts() const { return last_facts_; }

private:
    game::mechanics::MovementController ctl_;
    game::mechanics::RegionSpawn feet_regions_; // CheckRegionChange's fields (+0x9c held flags, +0x2e1)
    std::int32_t encumbered_shown_ = 0;
    std::uint32_t last_second_ = 0;
    std::uint32_t last_step_ = 0; // the last Step's time (PhysicsObject +0x30)
    std::int32_t air_ = -1, max_air_ = 0;
    game::mechanics::CharacterInput last_ch_;
    game::mechanics::MovementFacts last_facts_;
    // DoCollisionMovement's spawn fields (+0x1060 step count, the movement
    // ring, the last good position, ...) and the actor's location (feet +
    // AvatarHeight, as the spawn's z).
    game::mechanics::CollisionSpawn collision_;
    glm::vec3 actor_{0.0f};
    bool actor_valid_ = false;
    float radius_ = 0.0f;
    float lift_ = 0.0f;
    // GetPlayerFloorHeight's cache and the floor it last found (FloorHeight for the landing check).
    void floor_test(const MoveWorld* world, float floor_offset);
    float floor_probe_lift(float floor_offset) const; // the sphere's bottom above the spawn's z (floor_test)
    game::mechanics::FloorCache floor_cache_;
    game::mechanics::AutoDuck auto_duck_; // CanIFitHere's duck under a low ceiling (0xddf675.., off: 0xde0d5a 0)
    float floor_ = 0.0f;
    bool floor_valid_ = false;
    std::optional<float> player_ceiling_;
    std::uint32_t standing_on_ = 0; // +0x150
    std::int32_t touching_switch_ = -1; // +0x2d4 (a door id)
    TouchHandler touch_;
    float touch_height_ = 0.0f;
    std::int32_t touch_zone_ = 0;
    // TouchingSwitch on door `id` now (+0x2d4 = id first); +0x2d4 = 0 when it says so.
    void touch_now(std::uint8_t id) {
        touching_switch_ = id;
        if (touch_ && touch_(id)) touching_switch_ = -1;
    }
    // Vehicle_BeforeStep / AfterStep: the vehicle's pose when the body was last carried (its id 0: none yet).
    std::uint32_t carried_by_ = 0;
    game::mechanics::VehiclePose carried_pose_;
    std::uint32_t race_ = 0;
    std::uint32_t crouch_window_ = game::mechanics::kJumpWindowMs;
    std::optional<bool> jump_animation_;
    bool trace_ = false;
    bool at_helm_ = false;
    std::optional<bool> key_override_;
    std::uint32_t speed_stops_ = 0, turn_stops_ = 0, stun_leans_ = 0; // GameConnection's, as last seen
    game::mechanics::DrunkenMotion drunken_motion_;
    std::uint32_t drunkenness_motion_updates_ = 0;
    // What the fly mode was last worked out from (local_fly_mode): the buff slots' spells, the SPA 57 cache values
    // and the local spawn's count of OP_SpawnAppearance 19; not ready until the first frame after a reset.
    struct FlyModeSeen {
        bool ready = false;
        std::vector<std::uint32_t> buffs;
        bool lev2 = false, lev5 = false;
        std::uint32_t sets = 0;
    } fly_;
    game::mechanics::FlyMode local_fly_mode(const net::GameConnection& conn, const game::Spawn* me,
                                            const game::mechanics::MovementFacts& facts, bool no_levitate_zone,
                                            game::mechanics::FlyMode current);
};

} // namespace hailstone::app
