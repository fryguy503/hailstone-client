#include "game/mechanics/controls.h"

#include <cmath>
#include <utility>

#include "game/mechanics/movement.h"

namespace hailstone::game::mechanics {

namespace {

constexpr float kFrozen = -1000.0f;      // 0x9d0788: a speed modifier below it freezes the spawn
constexpr float kSpeedCapBase = kSpeedCap; // 0x9d1940 (1.092006)
constexpr float kHundredth = 0.01f;      // 0x9c4be4
constexpr float kSneakScale = 0.46f;     // 0x9d193c
constexpr float kZoomMax = 400.0f;       // 0x9c7b40
constexpr float kPitchMax = 128.0f, kPitchMin = -128.0f;
constexpr float kHeadingMoved = 2.0f, kSpeedMoved = 0.2f; // 0x9c58e8, 0x9c5728
constexpr std::int32_t kTurnLimit = 12, kMountTurnLimit = 8, kVehicleTurnLimit = 3, kLookLimit = 12;
constexpr std::uint8_t kStanding = 100, kDucking = 111, kStandFrozen = 102, kSitting = 110; // StandState
constexpr std::uint32_t kJumpCooldown = 1500;

// A float loaded onto the FPU (quiet), and one used from memory.
double st(float v) { return static_cast<double>(through_fpu(v)); }
X87 mem(float v) { return X87(v); }

std::int32_t wrap_inc(std::int32_t v) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(v) + 1u);
}

class Controls {
public:
    Controls(ControlsState& s, ControlsQueries& q, const Fpu& f) : s_(s), q_(q), f_(f) {}
    void run();

private:
    ControlSpawn& cp() { return s_.controlled; }
    bool below_floor() { return cp().speed_multiplier < kFrozen; }
    bool at_or_above_floor() { return cp().speed_multiplier >= kFrozen; }
    float weight() { return q_.calculate_weight(); }
    void strafe(bool can_move, std::uint8_t mode, bool rider_partner);
    bool turn_strafes(std::uint8_t bit);
    void turn(std::int32_t& count, bool right);
    void turn_key(bool can_move, bool key, std::uint8_t bit, std::uint8_t mode, std::int32_t& count, bool right);
    void strafe_key(bool can_move, bool key, std::uint8_t mode);
    void camera_heading(bool key, bool add);
    void camera_pan(bool key, bool subtract);
    void center_pitch();
    void jump(bool can_move);
    float forward(float s0);
    float backward(float s0);
    void look(bool key, std::int32_t& count, bool down);
    bool moved_at_least(float a, float b, float threshold);

    ControlsState& s_;
    ControlsQueries& q_;
    const Fpu& f_;
};

// StrafeScale and its quarter for a mount or rider.
void Controls::strafe(bool can_move, std::uint8_t mode, bool rider_partner) {
    if (can_move && at_or_above_floor()) {
        s_.strafing = mode;
        float w = s_.controlled_is_local ? weight() : 1.0f;
        // weight + weight, stored as the scale (kept unrounded); halved sneaking, with the speed modifier below 0
        // or ducking.
        double v = x87_add(f_, st(w), st(w));
        s_.strafe_scale = static_cast<float>(v);
        if (cp().sneaking || 0.0f > cp().speed_multiplier || cp().stand_state == kDucking)
            s_.strafe_scale = static_cast<float>(x87_mul(f_, v, mem(0.5f)));
        ControlRef partner = rider_partner ? q_.get_rider(ControlWho::Controlled) : q_.get_mount(ControlWho::Controlled);
        if (partner != ControlRef::None)
            s_.strafe_scale = static_cast<float>(x87_mul(f_, st(s_.strafe_scale), mem(0.25f)));
    }
    if (below_floor()) {
        s_.strafing = 0;
        s_.strafe_scale = 0.0f;
    }
}

bool Controls::turn_strafes(std::uint8_t bit) {
    bool special = q_.player_vf58(ControlWho::Controlled);
    if (!special) {
        if (s_.turn_keys_strafe) return true;
        if (s_.mouse_look && (s_.camera_index <= 2 || s_.camera_index == 6)) return true;
    }
    return (s_.autorun & bit) && (s_.autorun & 0x10);
}

void Controls::turn(std::int32_t& count, bool right) {
    count = wrap_inc(count);
    std::int32_t limit = q_.is_mount_race(ControlWho::Controlled) ? kMountTurnLimit
                         : q_.player_vf58(ControlWho::Controlled) ? kVehicleTurnLimit
                                                                  : kTurnLimit;
    if (count > limit) count = limit;
    auto rate = right ? static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(count)) : count;
    cp().speed_heading = static_cast<float>(rate); // the integer stored as a float
}

void Controls::turn_key(bool can_move, bool key, std::uint8_t bit, std::uint8_t mode, std::int32_t& count,
                        bool right) {
    if (!key && !(s_.autorun & bit)) return;
    q_.turn_off_auto_follow();
    if (turn_strafes(bit)) strafe(can_move, mode, true);
    else if (can_move) turn(count, right);
}

void Controls::strafe_key(bool can_move, bool key, std::uint8_t mode) {
    if (!key) return;
    q_.turn_off_auto_follow();
    if (!q_.player_vf58(ControlWho::Controlled)) strafe(can_move, mode, false);
}

void Controls::camera_heading(bool key, bool add) {
    if (!s_.camera_index || !key) return;
    if (!q_.camera_active()) return;
    s_.strafing = 0;
    double heading = q_.camera_heading();
    double step = x87_mul(f_, st(s_.rate_scale), mem(12.0f)); // rate * 12.0f
    double v = add ? x87_add(f_, heading, step) : x87_sub(f_, heading, step);
    q_.set_camera_heading(static_cast<float>(fix_heading(static_cast<float>(v), f_)));
}

void Controls::camera_pan(bool key, bool subtract) {
    if (!s_.camera_index || !key) return;
    double v = subtract ? x87_sub(f_, st(s_.camera.pan), mem(s_.rate_scale))
                        : x87_add(f_, st(s_.camera.pan), mem(s_.rate_scale));
    q_.set_camera_pan(static_cast<float>(v));
}

// CenterPitch: above 10 drops by 10, else above 0 becomes 0; then below -10
// rises by 10, else below 0 becomes 0.
void Controls::center_pitch() {
    float& p = cp().pitch;
    if (0.0f < p) p = 10.0f < p ? static_cast<float>(x87_sub(f_, st(p), 10.0)) : 0.0f;
    if (0.0f > p) p = -10.0f > p ? static_cast<float>(x87_add(f_, 10.0, mem(p))) : 0.0f;
}

void Controls::jump(bool can_move) {
    if (!can_move) return;
    if (q_.physics_mode(ControlWho::Controlled) != 0 && q_.physics_mode(ControlWho::Controlled) != 5) return;
    if (q_.player_vf58(ControlWho::Controlled)) return;
    if (!at_or_above_floor() || !s_.jump_key) return;
    if (cp().swim_state != 0) {
        if (s_.jump_started == 0) {
            if (s_.jump_ended > s_.now) s_.jump_ended = 0;
            s_.jump_started = s_.now;
        }
        return;
    }
    if ((cp().stand_state != kStanding && cp().stand_state != kDucking) || s_.jump_started != 0) return;
    if (s_.jump_ended > s_.now) s_.jump_ended = 0;
    if (!q_.feet_on_ground()) return;
    if (s_.now - s_.jump_ended <= kJumpCooldown) return;
    if (!s_.quiet_net_log && !s_.force_move && q_.endurance() <= 50) {
        std::int32_t max = q_.max_endurance();
        if (q_.endurance() <= max / 10) {
            q_.print(kStrTooTiredToJump, 0xf);
            return;
        }
    }
    s_.jump_started = s_.now;
}

// The forward key: the run, walk or sneak speed times the weight factor
// (half the walk speed ducking), through GetSwimmingSpeed when swimming,
// plus the speed modifier.
float Controls::forward(float s0) {
    ControlSpawn& p = cp();
    if (q_.player_vf58(ControlWho::Controlled)) {
        p.speed_run = through_fpu(p.walk_speed);
        if (below_floor()) p.speed_run = 0.0f;
        return through_fpu(p.speed_run);
    }
    double v = 0.0;
    bool swim = false;
    if (p.sneaking) {
        v = x87_mul(f_, st(p.sneak_speed), mem(kSneakScale));
        s0 = static_cast<float>(v);
        if (s_.controlled_is_local) v = x87_mul(f_, st(weight()), mem(s0));
        swim = p.swim_state >= 5;
    } else if (p.stand_state == kDucking) {
        v = x87_mul(f_, st(p.walk_speed), mem(0.5f));
        s0 = static_cast<float>(v);
        if (s_.controlled_is_local) v = x87_mul(f_, st(weight()), mem(s0));
    } else {
        if (s_.run_walk_state) v = x87_mul(f_, static_cast<double>(s_.run_walk_state), mem(p.run_speed));
        else v = st(p.walk_speed);
        s0 = static_cast<float>(v);
        if (s_.controlled_is_local) {
            v = x87_mul(f_, st(weight()), mem(s0));
            s0 = static_cast<float>(v);
        }
        if (p.swim_state >= 5) {
            bool rider = q_.get_rider(ControlWho::Controlled) != ControlRef::None;
            v = st(s0);
            if (!rider) swim = true;
        }
    }
    if (swim) {
        v = swimming_speed(static_cast<float>(v), q_.swimming_skill(), f_);
        s0 = static_cast<float>(v);
        if (s_.swim_jump.value_or(0.0f < p.pitch)) {
            q_.do_swim_jump();
            v = st(s0);
        }
    }
    if (below_floor()) {
        q_.set_accel(ControlWho::Controlled, 0.0f);
        return 0.0f;
    }
    // ForwardAccel: v + the speed modifier, below 0 the minimum 0.01.
    double r = x87_add(f_, v, mem(p.speed_multiplier));
    s0 = static_cast<float>(r);
    if (0.0 > r) {
        r = kHundredth;
        s0 = kHundredth;
    }
    q_.set_accel(ControlWho::Controlled, static_cast<float>(r));
    return s0;
}

// The backward key: the walk speed (halved sneaking or ducking; in water
// unweighted, and only at swim state 5), minus the speed modifier.
float Controls::backward(float s0) {
    ControlSpawn& p = cp();
    if (q_.player_vf58(ControlWho::Controlled)) {
        p.speed_run = -through_fpu(p.walk_speed); // copied through the FPU, negated
        if (below_floor()) p.speed_run = 0.0f;
        return through_fpu(p.speed_run);
    }
    double v = 0.0;
    bool weigh = true;
    if (p.sneaking || p.stand_state == kDucking) {
        v = -x87_mul(f_, st(p.walk_speed), mem(0.5f));
        s0 = static_cast<float>(v);
    } else if (p.swim_state == 0) {
        v = -st(p.walk_speed);
        s0 = static_cast<float>(v);
    } else {
        weigh = false;
        v = p.swim_state == 5 ? -st(p.walk_speed) : 0.0;
    }
    if (weigh && s_.controlled_is_local) v = x87_mul(f_, st(weight()), mem(s0));
    if (below_floor()) {
        q_.set_accel(ControlWho::Controlled, 0.0f);
        return 0.0f;
    }
    // BackwardAccel: v - the speed modifier, above 0 the maximum -0.01.
    double r = x87_sub(f_, v, mem(p.speed_multiplier));
    s0 = static_cast<float>(r);
    if (0.0 < r) {
        r = -kHundredth;
        s0 = -kHundredth;
    }
    q_.set_accel(ControlWho::Controlled, static_cast<float>(r));
    return s0;
}

void Controls::look(bool key, std::int32_t& count, bool down) {
    if (!key) return;
    count = wrap_inc(count);
    if (count > kLookLimit) count = kLookLimit;
    // count * rate, [negated,] stored as a float.
    auto delta = [&] {
        double d = x87_mul(f_, static_cast<double>(count), mem(s_.rate_scale));
        return static_cast<float>(down ? -d : d);
    };
    if (!q_.player_vf58(ControlWho::Controlled)) q_.add_pitch(ControlWho::Controlled, delta());
    q_.add_pitch(ControlWho::Local, delta());
}

// |a - b| not below the threshold.
bool Controls::moved_at_least(float a, float b, float threshold) {
    double d = std::fabs(x87_sub(f_, st(a), mem(b)));
    return d >= static_cast<double>(threshold);
}

void Controls::run() {
    float s0 = 0.0f;
    bool can_move = true;
    if (s_.pc_stunned_fc > 0 || cp().stand_state == kStandFrozen) can_move = false;
    if (s_.skip_controls) return;
    if (s_.quiet_net_log || s_.force_move) can_move = true;
    // Leaving control of another spawn (a boat) hands control back.
    if (!s_.controlled_is_local && q_.player_vf58(ControlWho::Controlled)) {
        if (q_.controlled_spawn_of() != ControlRef::Controlled) {
            if (s_.connected) q_.send_control_boat(cp().spawn_id);
            cp().speed_run = 0.0f;
            cp().speed_heading = 0.0f;
            std::swap(s_.controlled, s_.local); // `local` keeps the released spawn
            s_.controlled_is_local = true;
        }
    }
    if (cp().stand_state != kStanding && cp().stand_state != kDucking) can_move = false;
    s_.strafing = 0;
    if (can_move) {
        cp().speed_heading = 0.0f;
        s0 = kNoAccel;
        if (s_.controlled_is_local) {
            q_.follow(ControlWho::Controlled);
        } else if (q_.get_mount(ControlWho::Local) == ControlRef::Controlled) {
            cp().follow_e40 = s_.local.follow_e40;
            q_.follow(ControlWho::Controlled);
        }
    }
    if (below_floor()) {
        cp().speed_heading = 0.0f;
        cp().speed_run = 0.0f;
    } else if (kSpeedCapBase < cp().speed_multiplier) {
        // The SPA 290 cap: 1.092 + effect * 0.01 (unrounded) when the modifier is above it.
        std::int32_t effect = q_.speed_cap_effect();
        double cap = effect > 0 ? x87_add(f_, x87_mul(f_, static_cast<double>(effect), mem(kHundredth)), mem(kSpeedCapBase))
                                : st(kSpeedCapBase);
        if (cap < static_cast<double>(cp().speed_multiplier)) cp().speed_multiplier = static_cast<float>(cap);
    }
    if (s_.attack_on) {
        if (can_move && !s_.local_null && s_.primary_ready) {
            s_.primary_ready = false;
            q_.do_attack(0xd);
        }
        if (s_.attack_on && can_move && !s_.local_null && s_.secondary_ready) {
            s_.secondary_ready = false;
            q_.do_attack(0xe);
        }
    }
    const ControlKeys& k = s_.keys;
    camera_heading(k.camera_heading_minus, false);
    turn_key(can_move, k.turn_left, 4, 1, s_.turn_left_count, false);
    strafe_key(can_move, k.strafe_left, 1);
    camera_heading(k.camera_heading_plus, true);
    turn_key(can_move, k.turn_right, 8, 2, s_.turn_right_count, true);
    strafe_key(can_move, k.strafe_right, 2);
    if (s_.mouse_look) {
        std::uint8_t stand = cp().stand_state;
        if (s_.camera_index != 0 || stand == kStanding || stand == kDucking) q_.proc_mouse(can_move);
    } else if (k.center_view && !s_.camera_index && !k.look_up && !k.look_down) {
        center_pitch();
    }
    jump(can_move);
    s_.jump_key = 0;
    if (k.zoom_in && q_.camera_active()) {
        // The distance less rate * 2, not below the controlled spawn's height.
        double v = x87_sub(f_, st(s_.camera.distance), x87_add(f_, st(s_.rate_scale), st(s_.rate_scale)));
        if (v < static_cast<double>(cp().height)) v = st(cp().height);
        q_.set_camera_distance(static_cast<float>(v));
    }
    if (k.zoom_out && q_.camera_active()) {
        double v = x87_add(f_, x87_add(f_, st(s_.rate_scale), st(s_.rate_scale)), mem(s_.camera.distance));
        if (static_cast<double>(kZoomMax) < v) v = kZoomMax;
        q_.set_camera_distance(static_cast<float>(v));
    }
    camera_pan(k.camera_pan_plus, false);
    if (k.key_148c && can_move && !q_.player_vf58(ControlWho::Controlled)) {
        if (at_or_above_floor()) s_.held_scale = 2.0f;
    } else {
        s_.held_scale = 0.0f;
    }
    if (k.forward || (s_.autorun & 1) || s_.auto_forward) {
        if (can_move) {
            s0 = forward(s0);
        } else if (s_.forward_sits && k.forward) {
            if (cp().stand_state == kSitting) q_.sit();
            else if (q_.loot_window_active()) q_.close_loot_window();
        }
    }
    camera_pan(k.camera_pan_minus, true);
    if (k.backward || (s_.autorun & 2)) {
        q_.turn_off_auto_follow();
        if (can_move) s0 = backward(s0);
    }
    if (s_.camera_index && k.camera_pitch_plus) {
        double v = x87_add(f_, x87_mul(f_, st(s_.rate_scale), mem(8.0f)), mem(s_.camera.pitch));
        if (static_cast<double>(kPitchMax) < v) v = kPitchMax;
        q_.set_camera_pitch(static_cast<float>(v));
    }
    look(k.look_up, s_.look_up_count, false);
    if (s_.camera_index && k.camera_pitch_minus) {
        double v = x87_sub(f_, X87(s_.camera.pitch), x87_mul(f_, st(s_.rate_scale), mem(8.0f)));
        if (static_cast<double>(kPitchMin) > v) v = kPitchMin;
        q_.set_camera_pitch(static_cast<float>(v));
    }
    look(k.look_down, s_.look_down_count, true);
    if (s0 == kNoAccel) q_.set_accel(ControlWho::Controlled, 0.0f);
    bool& dirty = s_.controlled_is_local ? s_.local_dirty : s_.other_dirty;
    if (moved_at_least(s_.last_sent_heading, cp().heading, kHeadingMoved)) {
        dirty = true;
        s_.last_sent_heading = through_fpu(cp().heading);
    }
    if (moved_at_least(s_.last_sent_speed, cp().speed_run, kSpeedMoved)) {
        dirty = true;
        s_.last_sent_speed = through_fpu(cp().speed_run);
    }
    if (s_.strafing) dirty = true;
}

} // namespace

double fix_heading(double h, const Fpu& f) {
    if (!(h < 512.0)) {
        for (int n = 0;; ++n) {
            h = f.sub(h, 512.0);
            if (n > 5) return 0.0;
            if (h < 512.0 || std::isnan(h)) break;
        }
    }
    if (0.0 > h) {
        for (int n = 0;; ++n) {
            h = f.add(h, 512.0);
            if (n > 5) return 0.0;
            if (!(h < 0.0)) break;
        }
    }
    return h;
}

void process_controls(ControlsState& state, ControlsQueries& q, const Fpu& fpu) { Controls(state, q, fpu).run(); }

bool is_movement_command(int command) {
    return (command >= kCmdAutorun && command <= kCmdStrafeRight) || (command >= kCmdPitchUp && command <= kCmdZoomOut) ||
           (command >= kCmdForwardCam && command <= kCmdPitchDownCam);
}

bool execute_movement_command(ControlsState& s, int command, bool down, bool accept, std::uint8_t autorun_bits) {
    ControlKeys& k = s.keys;
    // Case 0x11, 0x12 and 0x14 come before the gate.
    switch (command) {
    case kCmdPitchUp:
        k.look_up = down;
        if (!down) {
            s.look_up_count = 0;
        } else {
            s.look_down_count = 0;
            k.look_down = false;
        }
        return true;
    case kCmdPitchDown:
        k.look_down = down;
        if (down) {
            s.look_up_count = 0;
            k.look_up = false;
        } else {
            s.look_down_count = 0;
        }
        return true;
    case kCmdZoomIn: k.zoom_in = down; return true;
    case kCmdZoomOut: k.zoom_out = down; return true;
    case kCmdForwardCam: k.camera_pan_plus = down; return true;
    case kCmdBackCam: k.camera_pan_minus = down; return true;
    case kCmdRightCam: k.camera_heading_plus = down; return true;
    case kCmdLeftCam: k.camera_heading_minus = down; return true;
    case kCmdPitchUpCam: k.camera_pitch_plus = down; return true;
    case kCmdPitchDownCam: k.camera_pitch_minus = down; return true;
    default: break;
    }
    if (!accept) return false;
    switch (command) {
    case kCmdCenterView:
        if (!down) return false;
        s.look_up_count = 0;
        s.look_down_count = 0;
        k.look_up = false;
        k.look_down = false;
        k.camera_pitch_plus = false;
        k.camera_pitch_minus = false;
        return true;
    case kCmdAutorun:
        if (!down) return false;
        if (autorun_bits != 0) {
            s.autorun = autorun_bits;
            s.back_count = 0;
        } else if (s.autorun != 0) {
            s.autorun = 0;
        } else {
            s.autorun = 1;
            s.back_count = 0;
            k.backward = false;
        }
        return true;
    case kCmdJump:
        if (!down) return false;
        s.jump_key = 1;
        return true;
    case kCmdForward:
        k.forward = down;
        if (!down) {
            s.forward_count = 0;
            return true;
        }
        if (s.autorun & 3) s.autorun = 0;
        s.back_count = 0;
        k.backward = false;
        return true;
    case kCmdBack:
        k.backward = down;
        if (!down) {
            s.back_count = 0;
            return true;
        }
        if (s.autorun & 3) s.autorun = 0;
        s.forward_count = 0;
        k.forward = false;
        return true;
    case kCmdRight:
        k.turn_right = down;
        if (down) {
            if (s.autorun & 0xc) s.autorun = 0;
            s.turn_left_count = 0;
            k.turn_left = false;
        } else {
            s.turn_right_count = 0;
        }
        k.strafe_left = false;
        k.strafe_right = false;
        return true;
    case kCmdLeft:
        k.turn_left = down;
        if (!down) {
            s.turn_left_count = 0;
        } else {
            if (s.autorun & 0xc) s.autorun = 0;
            s.turn_right_count = 0;
            k.turn_right = false;
        }
        k.strafe_left = false;
        k.strafe_right = false;
        return true;
    case kCmdStrafeLeft:
        k.strafe_left = down;
        return true;
    case kCmdStrafeRight:
        k.strafe_right = down;
        return true;
    default:
        return false;
    }
}

} // namespace hailstone::game::mechanics
