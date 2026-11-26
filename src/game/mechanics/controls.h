#pragma once

// CEverQuest::ProcessControls @0x52bbd0, the original's per-frame movement
// controls: keys become the turn rate, the strafe flag and scale, the
// forward/backward acceleration (SetAccel), the jump timer, the look pitch
// and the camera moves; the speed is capped with SPA 290 and the position is
// marked dirty for the next update packet.
//
// Follows the original exactly,
// checked against its vectors, which list every callee in call
// order (tests/movement_vectors_test.cpp). The original's globals are
// ControlsState fields (addresses in the comments); what it asks a callee is
// a ControlsQueries call, and what it tells one (SetAccel, the pitch, the
// camera, chat) is a ControlsQueries call too, in the original's order.

#include <cstdint>
#include <optional>

#include "game/mechanics/fpu.h"

namespace hailstone::game::mechanics {

// The fields of a spawn ProcessControls reads and writes.
struct ControlSpawn {
    float speed_multiplier = 0.0f; // +0x18: the speed modifier (below -1000: frozen)
    std::uint8_t swim_state = 0;   // +0xa2 (5 and up: swimming)
    bool sneaking = false;         // +0x264
    std::uint8_t stand_state = 0;  // +0x35c StandState: 100 standing, 111 ducking, 110 sitting, 102 frozen
    float walk_speed = 0.0f;       // +0x330
    float run_speed = 0.0f;        // +0x350
    float sneak_speed = 0.0f;      // +0x11c8 (PlayerZoneClient::InitSneakMod)
    float height = 0.0f;           // +0x138 AvatarHeight (the closest zoom)
    std::uint32_t spawn_id = 0;    // +0x148
    std::int32_t follow_e40 = 0;   // +0xe40 (auto follow, copied to a controlled mount)
    // The physics block (+0x64): +0x18, +0x1c, +0x28, +0x2c.
    float speed_run = 0.0f;
    float heading = 0.0f;
    float speed_heading = 0.0f;
    float pitch = 0.0f;
};

struct ControlKeys {
    bool forward = false, backward = false;        // __pulForward / __pulBackward
    bool turn_left = false, turn_right = false;    // 0xde0d9c / 0xde0d98
    bool strafe_left = false, strafe_right = false; // 0xde0da0 / 0xde0da4
    bool look_up = false, look_down = false;       // 0xde0dcc / 0xde0dd0
    bool zoom_in = false, zoom_out = false;        // 0xde0dd8 / 0xde0ddc
    bool camera_pan_plus = false, camera_pan_minus = false;         // 0xde12a8 / 0xde12ac
    bool camera_heading_plus = false, camera_heading_minus = false; // 0xde12b0 / 0xde12b4
    bool camera_pitch_plus = false, camera_pitch_minus = false;     // 0xde12b8 / 0xde12bc
    bool key_148c = false;      // 0xde148c: held, sets held_scale to 2
    bool center_view = false;   // 0xde0bec
};

// The active camera (EverQuest::Cameras[cameraType]).
struct ControlCamera {
    float pan = 0.0f;      // +0x2c
    float pitch = 0.0f;    // +0x30
    float distance = 0.0f; // +0x34
    float heading = 0.0f;  // +0x38 (getter +0x24, setter +0x20)
};

enum class ControlWho : std::uint8_t { Controlled, Local };
// What a lookup (GetMount, GetRider, the local player's +0x150) found.
enum class ControlRef : std::uint8_t { None, Controlled, Local, Other };

struct ControlsState {
    ControlKeys keys;
    std::int32_t jump_key = 0;       // 0xde0d8c: cleared every frame
    std::uint8_t autorun = 0;        // __pulAutoRun: 1 forward, 2 backward, 4 turn left, 8 turn right, 0x10 strafe
    bool auto_forward = false;       // 0xddf673
    bool mouse_look = false;         // __MouseLook
    bool turn_keys_strafe = false;   // 0xddfd01
    bool forward_sits = false;       // 0xde0d5b
    std::uint8_t run_walk_state = 1; // __RunWalkState: 0 walk, else the RunSpeed multiplier
    bool skip_controls = false;      // 0xe15dac
    bool quiet_net_log = false;      // 0xddf654: lets a frozen player move
    bool force_move = false;         // 0xddf790
    bool attack_on = false;          // __Attack
    bool has_target = false;         // pinstTarget
    bool connected = true;           // __gWorld: OP_ControlBoat is sent only then
    float rate_scale = 1.0f;         // g_LocalPhysicsTimeFactor
    std::uint32_t now = 0;           // CDisplay +0x154
    std::int8_t pc_stunned_fc = 0;   // the local PcBase's +0xfc: positive freezes
    bool controlled_is_local = true; // pinstControlledPlayer == pinstLocalPlayer
    bool local_null = false;         // no pinstLocalPlayer (the queued attacks wait)
    ControlSpawn controlled;         // pinstControlledPlayer
    ControlSpawn local;              // pinstLocalPlayer (when another spawn is controlled); after a hand-back
                                     // from a boat, the released boat
    std::int32_t camera_index = 0;   // CDisplay::cameraType
    ControlCamera camera;
    std::uint8_t strafing = 0;       // 0xddf704: 0, 1 left, 2 right
    float strafe_scale = 0.0f;       // 0xddfd70
    float held_scale = 0.0f;         // 0xddfd78
    std::int32_t turn_left_count = 0, turn_right_count = 0; // 0xddfd24 / 0xddfd28
    std::int32_t forward_count = 0, back_count = 0;         // 0xddfd1c / 0xddfd20: cleared by the key handlers only
    std::int32_t look_up_count = 0, look_down_count = 0;    // 0xddfd14 / 0xddfd18
    std::uint32_t jump_started = 0;  // 0xddf714 (CDisplay +0x154 time, 0 none)
    std::uint32_t jump_ended = 0;    // 0xddf718
    float last_sent_heading = 0.0f;  // 0xe635d0
    float last_sent_speed = 0.0f;    // 0xe635cc
    bool local_dirty = false;        // 0xddfd59
    bool other_dirty = false;        // 0xddfd58
    // The swing-ready flags DoPassageOfTime sets (game/mechanics/melee.h swing_ready; the vectors call them
    // ranged_queued / throw_queued): DoAttack(13) / DoAttack(14) while auto attack is on.
    bool primary_ready = false;      // 0xde0864
    bool secondary_ready = false;    // 0xde0865
    std::optional<bool> swim_jump;   // follow can supply a jump intent independent of the camera

    ControlSpawn& spawn(ControlWho w) { return w == ControlWho::Local && !controlled_is_local ? local : controlled; }
};

// What ProcessControls asks and tells, in its order.
class ControlsQueries {
public:
    virtual ~ControlsQueries() = default;
    // Questions.
    virtual bool player_vf58(ControlWho who) = 0;     // controlling something else (a boat)
    virtual std::int32_t physics_mode(ControlWho who) = 0; // vf78's +0xc: 0 or 5 allow the jump
    virtual float calculate_weight() = 0;             // EQ_Character::CalculateWeight
    virtual bool is_mount_race(ControlWho who) = 0;
    virtual ControlRef get_mount(ControlWho who) = 0; // EQPlayer::GetMount
    virtual ControlRef get_rider(ControlWho who) = 0; // PlayerBase_GetRider
    virtual bool feet_on_ground() = 0;                // PlayerBase::MyFeetAreOnGround (controlled)
    virtual std::int32_t endurance() = 0;             // the local PcBase's virtual +0x70
    virtual std::int32_t max_endurance() = 0;         // the CharacterZoneClient's virtual +0x28 (1)
    virtual std::int32_t swimming_skill() = 0;        // GetAdjustedSkill(50), asked by GetSwimmingSpeed
    virtual std::int32_t speed_cap_effect() = 0;      // TotalEffect(290, 1, 0, 1, 1)
    virtual bool loot_window_active() = 0;
    virtual ControlRef controlled_spawn_of() = 0;     // unk_PlayerClient_Get150 on the local player
    virtual bool camera_active() = 0;                 // camera virtual +0x2c
    virtual double camera_heading() = 0;              // camera virtual +0x24 (unrounded)
    // Actions.
    virtual void follow(ControlWho who) = 0;          // AutoFollow
    virtual void do_attack(std::int32_t slot) = 0;    // DoAttack(slot 0xd primary / 0xe secondary, 0, target)
    virtual void turn_off_auto_follow() = 0;
    virtual void set_accel(ControlWho who, float target) = 0; // PlayerClient::SetAccel(target, 0)
    virtual void print(std::uint32_t string_id, std::int32_t color) = 0; // dsp_chat
    virtual void do_swim_jump() = 0;                  // PlayerClient::DoSwimJump(1) on the controlled player
    virtual void proc_mouse(bool can_move) = 0;       // CEverQuest::procMouse
    virtual void add_pitch(ControlWho who, float delta) = 0; // 0x516cd0 on that spawn's physics
    virtual void close_loot_window() = 0;             // CLootWnd virtual +0xd8 (0, 1, 1)
    virtual void sit() = 0;
    virtual void set_camera_heading(float value) = 0; // camera virtuals +0x20, +0x10, +0x18, +0x14
    virtual void set_camera_pan(float value) = 0;
    virtual void set_camera_distance(float value) = 0;
    virtual void set_camera_pitch(float value) = 0;
    virtual void send_control_boat(std::uint32_t boat_id) = 0; // OP_ControlBoat {id, 0}
};

inline constexpr float kNoAccel = -20001.1f;      // 0x9d1944: no forward or backward key this frame
inline constexpr std::uint32_t kStrTooTiredToJump = 0x304a;

// __FixHeading @0x8d1f50: a heading into [0, 512) by up to six turns either
// way (seven or more give 0); unrounded.
double fix_heading(double heading, const Fpu& fpu = Fpu{});

// CEverQuest::ProcessControls @0x52bbd0.
void process_controls(ControlsState& state, ControlsQueries& q, const Fpu& fpu = Fpu{});

// __ExecuteCmd @0x4d7230's movement commands (key command numbers; the
// handlers are inline in its switch), run on each key
// press (`down`) and release:
//   AUTORUN (0, press): `autorun_bits` when given, else toggles forward
//     autorun (1; turning it on also releases BACK); clears 0xddfd20.
//   JUMP (1, press): 0xde0d8c = 1.
//   FORWARD (2) / BACK (3): the held flag; a release clears the key's own
//     count (0xddfd1c / 0xddfd20), a press ends forward/back autorun (bits 3),
//     releases the other key and clears its count.
//   RIGHT (4) / LEFT (5): the held flag, and both strafe keys are released; a
//     release clears the key's own turn ramp (0xddfd28 / 0xddfd24), a press
//     ends turn autorun (bits 0xc), releases the other turn key and clears its
//     ramp.
//   STRAFE_LEFT (6) / STRAFE_RIGHT (7): the held flag.
//   PITCHUP (17) / PITCHDOWN (18), whatever `accept` says: the held flag; a
//     press releases the other key and clears its ramp (0xddfd18 /
//     0xddfd14), a release clears the key's own.
//   CENTERVIEW (19, press): both look keys and their ramps and the camera
//     pitch keys (0xde12b8 / 0xde12bc) are released. Its CameraAngle = 0 (in
//     the first person camera, on the controlled and the local player) is the
//     caller's, which owns the pitch.
//   ZOOMIN (20), ZOOMOUT (21), FORWARD_CAM .. PITCHDOWN_CAM (328..333),
//     whatever `accept` says: the held flag (case 20 writes 0xde0d88 +
//     command * 4).
// SpeedRun is never written. `accept` is the original's gate: false while the
// player is frozen (StandState 102) unless the GM flags are set. Returns
// whether the original returns 1 (handled).
enum MovementCommand : int {
    kCmdAutorun = 0, kCmdJump = 1, kCmdForward = 2, kCmdBack = 3, kCmdRight = 4, kCmdLeft = 5,
    kCmdStrafeLeft = 6, kCmdStrafeRight = 7,
    kCmdPitchUp = 17, kCmdPitchDown = 18, kCmdCenterView = 19, kCmdZoomIn = 20, kCmdZoomOut = 21,
    kCmdForwardCam = 328, kCmdBackCam = 329, kCmdRightCam = 330, kCmdLeftCam = 331, kCmdPitchUpCam = 332,
    kCmdPitchDownCam = 333,
};
// The key commands execute_movement_command handles.
bool is_movement_command(int command);
bool execute_movement_command(ControlsState& state, int command, bool down, bool accept,
                              std::uint8_t autorun_bits = 0);

} // namespace hailstone::game::mechanics
