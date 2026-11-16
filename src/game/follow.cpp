#include "game/follow.h"

#include <algorithm>
#include <cmath>

#include "game/mechanics/controls.h"

namespace hailstone::game {

bool valid_follow_profile(const FollowProfile& p) {
    auto between = [](float v, float low, float high) {
        return std::isfinite(v) && v >= low && v <= high;
    };
    return !(p.flags & ~15u) && between(p.stop_distance, 1, 50) && between(p.restart_margin, 0, 10) &&
           between(p.start_range, 1, 200) && between(p.turn_snap, 0, 16) &&
           between(p.turn_slow, 1, 32) && between(p.turn_fast, 1, 32) &&
           between(p.turn_fast_angle, 0, 256) && between(p.move_angle, 0, 256) &&
           between(p.move_far_margin, 0, 50) && between(p.pitch_gain, 0, 1) &&
           between(p.pitch_down_scale, 0, 1) && between(p.pitch_limit, 0, 128);
}

bool FollowIdentity::matches(std::uint32_t player, std::uint64_t player_incarnation,
                             std::uint32_t followed, std::uint64_t followed_incarnation) const {
    return owner && target && owner != target && owner_incarnation && target_incarnation &&
           owner == player && owner_incarnation == player_incarnation && target == followed &&
           target_incarnation == followed_incarnation;
}

ProfiledFollowStep profiled_follow_step(const FollowInput& in, const FollowProfile& p,
                                      bool moving, float pitch, float seconds) {
    ProfiledFollowStep out{in.heading, pitch, false};
    if (!valid_follow_profile(p) || !std::isfinite(seconds) || seconds <= 0 ||
        !std::isfinite(in.heading) || !std::isfinite(pitch) || !std::isfinite(in.x) || !std::isfinite(in.y) ||
        !std::isfinite(in.z) || !std::isfinite(in.target_x) || !std::isfinite(in.target_y) ||
        !std::isfinite(in.target_z) || !std::isfinite(in.distance) || in.distance < 0 ||
        in.target_animation == kFollowDeadAnimation) return out;
    auto normalized = [](float h) { h = std::fmod(h, 512.0f); return h < 0 ? h + 512.0f : h; };
    constexpr float pi = 3.14159265358979323846f;
    const float desired = normalized(std::atan2(in.target_x - in.x, in.target_y - in.y) * (256.0f / pi));
    float delta = normalized(desired - in.heading + 256.0f) - 256.0f;
    const float elapsed = std::min(seconds, 0.1f) * 60.0f;
    const float rate = std::abs(delta) > p.turn_fast_angle ? p.turn_fast : p.turn_slow;
    const float change = std::min(std::abs(delta), rate * elapsed);
    out.heading = std::abs(delta) <= p.turn_snap ? desired : normalized(in.heading + std::copysign(change, delta));
    delta = normalized(desired - out.heading + 256.0f) - 256.0f;
    const float threshold = p.stop_distance + (moving ? 0.0f : p.restart_margin);
    out.move = in.distance > threshold &&
               (std::abs(delta) < p.move_angle || in.distance > p.stop_distance + p.move_far_margin);
    if (p.flags & FollowLookPitch) {
        const float horizontal = std::hypot(in.target_x - in.x, in.target_y - in.y);
        float aim = std::atan2(in.target_z - in.z, horizontal) * (256.0f / pi);
        if (aim < 0) aim *= p.pitch_down_scale;
        const float blend = 1.0f - std::pow(1.0f - p.pitch_gain, elapsed);
        out.pitch = std::clamp(pitch + (aim - pitch) * blend, -p.pitch_limit, p.pitch_limit);
    }
    return out;
}

namespace {

using mechanics::Fpu;

// The x87 product of an extended-precision value (an arctangent) by a double constant, rounded once to the precision.
double mul_ext(long double a, double b, const Fpu& fpu) {
    double hi = static_cast<double>(a);
    double lo = static_cast<double>(a - static_cast<long double>(hi)); // exact: a has 64 significant bits
    double p = hi * b;
    double err = std::fma(hi, b, -p) + lo * b; // a * b = p + err (lo * b and the sum are exact here)
    if (fpu.precision() == mechanics::FpuPrecision::Double53) return p + err;
    // 24-bit: round p + err once (Fpu::add of two exact parts).
    return fpu.add(p, err);
}

// __HeadingDiff @0x8d1fc0: the gap between the two headings the shorter way (returned unrounded) and its signed
// form (stored as a float): positive up to 256, else the gap minus 512.
double heading_diff(float a, float b, float& direction, const Fpu& fpu) {
    double hi = a, lo = b;
    if (static_cast<double>(a) <= static_cast<double>(b)) {
        hi = b;
        lo = a;
    }
    double d = fpu.sub(hi, lo);
    if (d > 256.0) {
        double wrapped = fpu.sub(d, 512.0);
        direction = static_cast<float>(wrapped);
        return -wrapped;
    }
    direction = static_cast<float>(d);
    return d;
}

// The gradual turn toward the bearing (the original's instructions from 0x58d579).
float turn_toward(float heading, float bearing, const Fpu& fpu) {
    float direction = 0;
    double d = heading_diff(heading, bearing, direction, fpu);
    if (1.0 > d) return bearing; // below one unit: snap
    double step;
    if (4.0 > d) step = d;
    else if (d > 128.0) step = 9.0;
    else step = 4.0;
    bool subtract;
    if (bearing > heading && 0.0 > direction) subtract = true;
    else if (heading > bearing) subtract = 0.0 < direction;
    else subtract = false;
    double turned = subtract ? fpu.sub(heading, step) : fpu.add(step, heading);
    return static_cast<float>(mechanics::fix_heading(static_cast<double>(static_cast<float>(turned)), fpu));
}

} // namespace

double x87_bearing(float y1, float x1, float y2, float x2, const Fpu& fpu) {
    double a = y1, b = x1, c = y2, d = x2;
    double dy = std::fabs(fpu.sub(a, c));
    double dx = std::fabs(fpu.sub(b, d));
    const double eps = 9.999999974752427e-07;
    if (eps > dy) dy = eps;
    long double theta = std::atan2(static_cast<long double>(dx), static_cast<long double>(dy)); // 64 bits, as the x87's
    double deg = fpu.mul(mul_ext(theta, 180.0, fpu), 0.3183099014828645);
    double v;
    if (!(a < c)) {
        if (!(d < b)) v = fpu.add(90.0, fpu.sub(90.0, deg));
        else if (std::isnan(d) || std::isnan(b)) v = deg;
        else v = fpu.add(deg, 180.0);
    } else if (!(d <= b)) {
        v = deg; // x2 above x1 (unordered too): an equal x takes the 270 branch
    } else {
        v = fpu.add(fpu.sub(90.0, deg), 270.0);
    }
    return fpu.mul(fpu.mul(v, 511.5), static_cast<double>(0.0027777778f));
}

FollowStep follow_step(const FollowInput& in, const Fpu& fpu) {
    FollowStep out;
    out.heading = in.heading;
    out.bearing = in.prev_bearing;
    out.distance = in.prev_distance;
    out.speed = in.speed;
    if (in.target_animation == kFollowDeadAnimation) {
        out.stop_message = kStrFollowDead;
        return out;
    }
    out.distance = in.distance;
    if (in.distance > kFollowTooFar) {
        out.stop_message = kStrFollowTooFar;
        return out;
    }
    auto heights = static_cast<float>(fpu.add(in.target_height, in.height));
    out.bearing = static_cast<float>(x87_bearing(in.y, in.x, in.target_y, in.target_x, fpu));
    if (in.clear_line) {
        if (20.0f > out.distance) out.heading = out.bearing;
        else out.heading = turn_toward(in.heading, out.bearing, fpu);
    }
    double stop = fpu.add(15.0, heights);
    if (stop > static_cast<double>(out.distance)) {
        out.move = false;
        out.speed = 0;
        return out;
    }
    double walk = fpu.add(heights, 30.0);
    out.move = true;
    out.run = true;
    if (walk > static_cast<double>(out.distance) && (in.target_speed < in.speed || in.speed == 0.0f)) out.run = false;
    return out;
}

} // namespace hailstone::game
