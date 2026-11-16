#pragma once

// One step of /follow: PlayerClient::FollowPlayerAI @0x58d3c0 (checked against its
// FollowPlayerAI vectors). The original runs it every frame while the follow link (+0xe40) is set; it turns the
// player toward the followed spawn and sets the follow movement flags (0xddf720 move, 0xddf670 run) that the
// movement code reads like held keys.

#include <cstdint>
#include <optional>

#include "game/mechanics/fpu.h"

namespace hailstone::game {

struct FollowInput {
    // The player (+0x64 Y, +0x68 X, +0x6c Z as the server's coordinates), heading (+0x80, 0..512),
    // AvatarHeight (+0x138) and current speed (+0x7c SpeedRun).
    float y = 0, x = 0, z = 0;
    float heading = 0;
    float height = 0;
    float speed = 0;
    // The followed spawn.
    float target_y = 0, target_x = 0, target_z = 0;
    float target_height = 0;
    float target_speed = 0;
    std::uint8_t target_animation = 0; // +0x35c: 0x78 = dead
    // CDisplay::TrueDistance of the two positions (a float).
    float distance = 0;
    // __CastRay(target, player position): a clear line lets the heading turn.
    bool clear_line = true;
    // What +0x220 and +0x370 held before (kept when the step ends early).
    float prev_bearing = 0, prev_distance = 0;
};

struct FollowStep {
    // The follow ends with this eqstr and the target's name (12692 dead, 12693 too far); TurnOffAutoFollow.
    std::optional<std::uint32_t> stop_message;
    float heading = 0;       // +0x80 afterwards
    float bearing = 0;       // +0x220: __get_bearing toward the target (0 when it ended first)
    float distance = 0;      // +0x370: the distance kept
    float speed = 0;         // +0x7c afterwards (0 when stopping inside the stop band)
    bool move = false;       // 0xddf720
    bool run = false;        // 0xddf670 (meaningful with move)
};

inline constexpr std::uint8_t kFollowDeadAnimation = 0x78;
inline constexpr float kFollowTooFar = 200.0f;     // 0x9c58e4
inline constexpr std::uint32_t kStrFollowDead = 12692, kStrFollowTooFar = 12693;

// Optional data supplied by an active server extension. No profile means the
// original follow_step below; enhanced following never invokes navigation.
enum FollowFlags : std::uint32_t {
    FollowAllowUngrouped = 1, FollowCancelOnMove = 2,
    FollowLookPitch = 4, FollowPreserveRun = 8
};
struct FollowProfile {
    std::uint32_t flags = 0;
    float stop_distance = 0, restart_margin = 0, start_range = 0;
    float turn_snap = 0, turn_slow = 0, turn_fast = 0, turn_fast_angle = 0;
    float move_angle = 0, move_far_margin = 0;
    float pitch_gain = 0, pitch_down_scale = 0, pitch_limit = 0;
    bool operator==(const FollowProfile&) const = default;
};
struct MountProfile { bool instant_acceleration = false; };
bool valid_follow_profile(const FollowProfile& profile);
struct ProfiledFollowStep {
    float heading = 0, pitch = 0;
    bool move = false;
};
// Pure host controller: time-scaled turning/pitch and a true stop/restart band.
// Run/walk selection, collision and ordinary movement remain the caller's.
ProfiledFollowStep profiled_follow_step(const FollowInput& in, const FollowProfile& profile,
                                      bool moving, float pitch, float seconds);

// Process-unique actor identities bind a manually initiated link to this owner
// and target. Numeric spawn-id reuse never continues an enhanced follow.
struct FollowIdentity {
    std::uint32_t owner = 0, target = 0;
    std::uint64_t owner_incarnation = 0, target_incarnation = 0;
    bool matches(std::uint32_t player, std::uint64_t player_incarnation,
                 std::uint32_t followed, std::uint64_t followed_incarnation) const;
};

// The step. `fpu` is the x87 precision the arithmetic rounds to (24-bit in game).
FollowStep follow_step(const FollowInput& in, const mechanics::Fpu& fpu = mechanics::Fpu{});

// __get_bearing @0x4ef250: the heading (0..511.5) from (y1, x1) toward (y2, x2), as the x87 computes it at the
// given precision (the x87's arctangent itself always gives 64 bits). Returned unrounded (in the precision's range).
double x87_bearing(float y1, float x1, float y2, float x2, const mechanics::Fpu& fpu);

} // namespace hailstone::game
