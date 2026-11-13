#include "game/mechanics/collision_movement.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// A value the original holds on the FPU is a double here (such values are
// quiet: st() loads a float that way); a float it reads straight from memory
// goes in as X87(float) and keeps a signalling NaN's flag. Each operation's
// operands are passed in the order the original combines them, which decides
// which NaN's payload survives.

namespace hailstone::game::mechanics {

float collision_height_offset(std::uint8_t stand_state, std::uint8_t body_wet, std::uint32_t race,
                               float avatar_height, float radius) {
    if (stand_state == 0x6f || stand_state == 0x78 || body_wet >= 5) return 0.0f;
    float height = 1.9f;
    if (race == 2 || race == 9 || race == 10) height = std::max(avatar_height * 0.5f, height);
    return std::max(height, radius);
}

namespace {

constexpr float kBoundsMargin = 2000.0f; // 0x9c8264
constexpr float kStill = 0.01f;          // 0x9c4be4
constexpr float kStillLow = -0.01f;      // 0x9c53e4
constexpr float kSlideLimit = 0.1f;      // 0x9c4a48
constexpr float kFlat = 0.001f;          // 0x9c4be8
constexpr float kBarely = 1e-6f;         // 0x9cf850 (a squared distance)
constexpr float kMinusOne = -1.0f;       // 0x9c4ec4
constexpr float kNormalizeLow = -0x1p-23f; // 0x9c338c
constexpr float kNormalizeHigh = 0x1p-23f; // 0x9c3388
constexpr std::uint32_t kPropertyNoCollide = 0x64;

// A float loaded and stored again: a signalling NaN comes out quiet.
float tf(float v) {
    std::uint32_t b;
    std::memcpy(&b, &v, 4);
    if ((b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu)) b |= 0x00400000u;
    std::memcpy(&v, &b, 4);
    return v;
}

// A float loaded onto the FPU (quiet).
double st(float v) { return static_cast<double>(tf(v)); }

// Rounds to float.
float to_f(double v) { return static_cast<float>(v); }

// The square of a vector's length as the original sums it: with the three
// components loaded in the order (b, a, c) of [+4], [+0], [+8]:
// (b*b + a*a) + c*c, each step rounded (0x509130 and the speed checks).
double length_squared(const Fpu& f, double a, double b, double c) {
    double aa = x87_mul(f, a, a);  // a * a
    double bb = x87_mul(f, b, b);  // b * b
    double sum = x87_add(f, bb, aa); // b*b + a*a
    double cc = x87_mul(f, c, c);  // c * c
    return x87_add(f, sum, cc);    // sum + c*c
}

// This function's working values.
struct Frame {
    Vec3f actor{};               // the actor's location; its z is lifted, later the placed location
    double lifted_z = 0;         // the actor's z + height, unrounded
    Vec3f start{};               // the lifted start
    Vec3f sweep_end{};           // where the sweep stopped (the target before any sweep)
    Vec3f sweep_target{};        // the sweep's target; later a scratch vector
    Vec3f offset{};              // the slide-back vector, later the move from the start
    Vec3f before_lift{};         // the position before the lift; later the check sweep's skitter direction
    Vec3f skitter_dir{};         // the skitter direction
    Vec3f sweep_start{};         // the sweep's start
    float skitter_dist = 0.0f;   // the skitter distance
    float check_dist = 0.0f;     // the check sweep's skitter distance; later |speed|
    float height = 0.0f;         // the height
    float move_0 = 0.0f, move_1 = 0.0f, move_2 = 0.0f; // the move
    float abs_dz = 0.0f;         // |dz| of the move (BarelyMoved)
};

// 0x50817a..0x5081fa: the target strictly inside the box grown by the margin.
bool inside_scene(const Fpu& f, const Vec3f& min, const Vec3f& max, const Vec3f& target) {
    double margin = static_cast<double>(kBoundsMargin);
    double lo[3], hi[3];
    lo[0] = x87_sub(f, st(min[0]), margin); // min - margin
    lo[1] = x87_sub(f, st(min[1]), margin);
    lo[2] = x87_sub(f, st(min[2]), margin);
    hi[0] = x87_add(f, st(max[0]), margin); // max + margin
    hi[1] = x87_add(f, st(max[1]), margin);
    hi[2] = x87_add(f, margin, st(max[2])); // margin + max.z
    // Below, equal or unordered leaves (needs hi > t); anything but strictly
    // below leaves (needs lo < t).
    for (int k = 0; k < 3; ++k)
        if (!(hi[k] > static_cast<double>(target[k]))) return false;
    for (int k = 0; k < 3; ++k)
        if (!(lo[k] < static_cast<double>(target[k]))) return false;
    return true;
}

// 0x50864f..0x5086d0: the distance from the sweep's start to the hit;
// strictly between 0 and 0.1, offset = start - the hit and true.
bool slide_back(const Fpu& f, Frame& fr) {
    double hx = st(fr.sweep_end[0]), hy = st(fr.sweep_end[1]), hz = st(fr.sweep_end[2]);
    double dx = x87_sub(f, hx, st(fr.sweep_start[0])); // hit - start
    double dy = x87_sub(f, hy, st(fr.sweep_start[1]));
    double dz = x87_sub(f, hz, st(fr.sweep_start[2]));
    double xx = x87_mul(f, dx, dx);
    double yy = x87_mul(f, dy, dy);
    double s = x87_add(f, yy, xx);  // y*y + x*x
    double zz = x87_mul(f, dz, dz);
    double total = x87_add(f, zz, s); // z*z + s
    double length = f.sqrt(total);
    // Leaves when 0.1 <= length or unordered.
    if (!(static_cast<double>(kSlideLimit) > length)) return false;
    // Needs 0 < length.
    if (!(0.0 < length)) return false;
    fr.offset[0] = to_f(x87_sub(f, st(fr.start[0]), hx)); // start - hit
    fr.offset[1] = to_f(x87_sub(f, st(fr.start[1]), hy));
    fr.offset[2] = to_f(x87_sub(f, X87(fr.start[2]), hz)); // start z read from memory
    return true;
}


// 0x5086d9..0x5086fd: skitter_dir += offset, each component rounded.
void add_back(const Fpu& f, Frame& fr) {
    fr.skitter_dir[0] = to_f(x87_add(f, st(fr.offset[0]), X87(fr.skitter_dir[0]))); // offset + skitter_dir
    fr.skitter_dir[1] = to_f(x87_add(f, st(fr.skitter_dir[1]), X87(fr.offset[1]))); // skitter_dir + offset
    fr.skitter_dir[2] = to_f(x87_add(f, st(fr.skitter_dir[2]), X87(fr.offset[2])));
}

// 0x50873b..0x5085bf: the next sweep starts at the hit (sweep_start =
// sweep_end) and aims at the hit + skitter_dir * step (sweep_target).
void advance(const Fpu& f, Frame& fr, float step) {
    double s = st(step);
    double hx = st(fr.sweep_end[0]), hy = st(fr.sweep_end[1]), hz = st(fr.sweep_end[2]);
    fr.sweep_start = {to_f(hx), to_f(hy), to_f(hz)};
    double mx = x87_mul(f, st(fr.skitter_dir[0]), s);
    double my = x87_mul(f, st(fr.skitter_dir[1]), s);
    double mz = x87_mul(f, s, st(fr.skitter_dir[2])); // step * z
    fr.sweep_target[0] = to_f(x87_add(f, hx, mx));
    fr.sweep_target[1] = to_f(x87_add(f, hy, my));
    fr.sweep_target[2] = to_f(x87_add(f, mz, hz));     // step * z + hit z
}

// 0x508837..0x508894: abs_dz = |dz| of the move from the start; true when
// |dz| is at most 0.001 and the squared distance is below 1e-6.
bool barely_moved(const Fpu& f, Frame& fr, const Vec3f& position) {
    double dx = std::fabs(x87_sub(f, st(position[0]), X87(fr.start[0]))); // |position - start|
    double dy = std::fabs(x87_sub(f, st(position[1]), X87(fr.start[1])));
    double dz = std::fabs(x87_sub(f, st(position[2]), X87(fr.start[2])));
    fr.abs_dz = to_f(dz); // rounds to float here; dz itself stays unrounded
    // 0.001 < |dz| or unordered: moved far.
    if (static_cast<double>(kFlat) < dz || std::isnan(dz)) return false;
    double sum = length_squared(f, dx, dy, dz); // (dy*dy + dx*dx) + dz*dz
    return sum < static_cast<double>(kBarely); // strictly below; unordered is not
}

} // namespace

void normalize_vector(float v[3], const Fpu& f) {
    double length = f.sqrt(length_squared(f, st(v[0]), st(v[1]), st(v[2])));
    // On to the zero test only when -2^-23 < length (unordered goes on to divide);
    // zero only when 2^-23 > length.
    if (static_cast<double>(kNormalizeLow) < length && static_cast<double>(kNormalizeHigh) > length) {
        v[0] = v[1] = v[2] = 0.0f;
        return;
    }
    double inverse = x87_div(f, 1.0, length); // 1 / length
    v[0] = to_f(x87_mul(f, st(v[0]), inverse)); // v[0] * inverse
    v[1] = to_f(x87_mul(f, inverse, X87(v[1]))); // inverse * v[1] from memory
    v[2] = to_f(x87_mul(f, inverse, X87(v[2])));
}

double collision_volume_radius(float avatar_height, bool npc, std::uint32_t race, bool mount_race, const Fpu& f) {
    double r = x87_mul(f, st(avatar_height), X87(0.5f)); // +0x138 x 0.5
    if (!npc) return x87_mul(f, r, X87(0.95f));
    float v = to_f(x87_mul(f, r, X87(0.35f)));           // rounds to float here
    if (race == 31) v = to_f(x87_mul(f, st(v), X87(0.9f)));
    double out = st(v);
    if (mount_race) out = x87_add(f, out, out);          // doubled
    return out;
}

double model_sweep_radius(float model_radius, float scale, float co_factor, const Fpu& f) {
    float t = to_f(x87_mul(f, st(scale), X87(model_radius))); // getter +0x60 x model +0x38, rounded to float
    return x87_mul(f, st(co_factor), X87(t));                 // getter +0x8c x that
}

float sweep_radius(const SweepRadiusSpawn& spawn) {
    // 0x5062a0's player value, stored by SetCollisionVolumeRadius. Native type-2
    // local-player probes support this rule for the two measured forms.
    return static_cast<float>(collision_volume_radius(spawn.avatar_height, false, spawn.race, false));
}

bool keep_in_scene(CollisionSpawn& s, bool collision_loaded, const Vec3f& min, const Vec3f& max, const Fpu& f) {
    Vec3f position{s.physics[0], s.physics[1], s.physics[2]};
    if (collision_loaded && !inside_scene(f, min, max, position)) {
        s.physics[3] = s.physics[4] = s.physics[5] = 0.0f;
        for (int k = 0; k < 3; ++k) s.physics[k] = tf(s.last_good[k]);
        return false;
    }
    for (int k = 0; k < 3; ++k) s.last_good[k] = s.physics[k]; // a block copy: the bits as they are
    return true;
}

CollisionMove do_collision_movement(CollisionSpawn& s, Vec3f& target, CollisionQueries& q, const Fpu& f) {
    CollisionMove out;
    Frame fr;
    fr.actor = q.actor_location();
    MovementStat& entry = s.ring[s.ring_index % 20];
    s.ring_index = (s.ring_index + 1) % 20;
    if (s.ring_count < 20) ++s.ring_count;
    for (int k = 0; k < 12; ++k) entry.before[static_cast<std::size_t>(k)] = tf(s.physics[static_cast<std::size_t>(k)]);
    entry.floor_before = tf(s.floor_height);
    for (int k = 0; k < 3; ++k) entry.target[static_cast<std::size_t>(k)] = tf(target[static_cast<std::size_t>(k)]);
    ++s.step_count;
    float* position = s.physics.data();
    if (q.zone_collision_loaded()) {
        Vec3f min{}, max{};
        // No world (-1): the original compares uninitialised locals; taken as inside here.
        if (q.scene_box(min, max) && !inside_scene(f, min, max, target)) {
            position[3] = position[4] = position[5] = 0.0f;
            for (int k = 0; k < 3; ++k) position[k] = tf(s.last_good[static_cast<std::size_t>(k)]);
            out.early = true;
            out.stopped = false; // returns 0
            return out;
        }
    }
    q.zone_client();
    bool collide = false;
    if (!q.mounted() && !q.is_vehicle_race()) collide = !q.has_property(kPropertyNoCollide);

    // Lift (0x50823c..0x5082d2).
    for (int k = 0; k < 3; ++k) fr.before_lift[static_cast<std::size_t>(k)] = tf(position[k]);
    double h = q.height();
    fr.height = to_f(h);
    fr.lifted_z = x87_add(f, st(fr.actor[2]), h);         // actor z + height
    fr.actor[2] = to_f(fr.lifted_z);
    target[2] = to_f(x87_add(f, h, st(target[2])));     // h + tz
    for (int k = 0; k < 3; ++k) position[k] = tf(target[static_cast<std::size_t>(k)]);
    fr.start = {tf(fr.actor[0]), tf(fr.actor[1]), to_f(fr.lifted_z)};
    fr.sweep_end = {tf(target[0]), tf(target[1]), tf(target[2])};
    fr.move_0 = to_f(x87_sub(f, st(target[0]), st(fr.actor[0]))); // tx - actor x
    fr.move_1 = to_f(x87_sub(f, st(target[1]), st(fr.actor[1])));
    fr.move_2 = to_f(x87_sub(f, X87(target[2]), fr.lifted_z));  // tz - lifted z

    float radius = to_f(q.collision_radius());
    // The sweep request: the lifted start, the move and the radius.
    std::array<float, 7> request{tf(fr.actor[0]), tf(fr.actor[1]), tf(fr.actor[2]), tf(fr.move_0), tf(fr.move_1), tf(fr.move_2),
                                 radius};
    bool keep = false;
    bool moving = q.moving_state();
    float d1 = request[3], d2 = request[4], d3 = request[5]; // the move as requested
    if (!moving && std::fabs(fr.move_0) < kStill && std::fabs(fr.move_1) < kStill && std::fabs(fr.move_2) < kStill) {
        if (s.step_count > 2) {
            target = {tf(fr.actor[0]), tf(fr.actor[1]), tf(fr.actor[2])};
            d1 = d2 = d3 = 0.0f;
            if (s.body_wet >= 5 && s.stand_time == 0) s.stand_time = q.display_time();
        }
    } else {
        s.colliding_type = 0;
    }
    if (s.step_count > 2 && d1 < kStill && kStillLow < d1 && d2 < kStill && d2 > kStillLow && kStill > d3 &&
        d3 > kStillLow) {
        if (s.body_wet < 5 && s.stand_time == 0) {
            std::uint32_t time = q.display_time();
            s.stand_time = time;
            if (q.rider()) {
                q.rider();
                q.set_rider_stand_time(time);
            }
        }
        if (!moving && position[6] == 0.0f) {
            keep = true;
            collide = false;
        }
    }
    if (collide) collide = q.sweep_request(request);
    bool rider = q.rider();
    fr.skitter_dist = 0.0f;
    std::int32_t count = q.is_controlled() ? 10 : 1;
    s.possibly_stuck = 0;
    bool hit = false;
    if (collide) {
        fr.sweep_start = {tf(fr.start[0]), tf(fr.start[1]), tf(fr.start[2])};
        bool slid = false;
        fr.sweep_target = {tf(target[0]), tf(target[1]), tf(target[2])};
        while (count != 0 || slid) {
            SweepResult r{fr.sweep_end, fr.skitter_dir, fr.skitter_dist};
            q.sweep(fr.sweep_start, fr.sweep_target, rider, true, r);
            fr.sweep_end = r.position;
            fr.skitter_dir = r.skitter_dir;
            fr.skitter_dist = r.skitter_dist;
            --count;
            float step = tf(fr.skitter_dist);
            if (!(step > 0.0f) || slid) break;
            hit = true;
            if (count == 0) {
                if (q.is_controlled() && slide_back(f, fr)) {
                    normalize_vector(fr.offset.data(), f);
                    add_back(f, fr);
                    normalize_vector(fr.skitter_dir.data(), f);
                    slid = true;
                    if (fr.skitter_dist < kSlideLimit) {
                        step = kSlideLimit;
                        fr.skitter_dist = kSlideLimit;
                    } else {
                        step = tf(fr.skitter_dist);
                    }
                } else {
                    step = tf(fr.skitter_dist);
                }
            }
            advance(f, fr, step);
        }
    } else {
        q.no_collide();
        if (q.is_vehicle_race()) q.move_without_collision(target, fr.sweep_end);
    }
    for (int k = 0; k < 3; ++k) position[k] = tf(fr.sweep_end[static_cast<std::size_t>(k)]);
    Vec3f pos{position[0], position[1], position[2]};
    if (collide && q.is_controlled() && !q.is_vehicle_race() && !keep && s.possibly_stuck == 0) {
        bool fits = q.in_scene_region(pos) && q.can_i_fit_here(pos, hit ? 0 : 1);
        if (!fits) {
            s.possibly_stuck = 1;
            for (int k = 0; k < 3; ++k) position[k] = tf(fr.before_lift[static_cast<std::size_t>(k)]);
            keep = true;
        }
    }
    pos = {position[0], position[1], position[2]};
    bool barely = barely_moved(f, fr, pos);
    if (s.possibly_stuck == 0) {
        if (q.is_vehicle_race()) {
            keep = s.flag_19d != 0;
        } else if (barely && s.step_count > 2) {
            keep = true;
        } else {
            s.flag_19d = 0;
            if (kFlat > fr.abs_dz && kMinusOne > position[5]) position[5] = kMinusOne;
            q.clear_stuck(0);
            if (q.check_stuck() && q.is_controlled()) {
                // 0x509190 -> CCollisionInfoSphere::InitFromActor @0x476ab0 and the base @0x441c90.
                CollisionTestInfo info;
                float r = to_f(q.collision_radius());
                info.from = {tf(position[0]), tf(position[1]), tf(position[2])}; // live: check_stuck may move z
                info.to = info.from;
                info.radius = tf(r);
                info.radius_squared = to_f(x87_mul(f, st(r), st(r)));
                info.radius_inverse = to_f(x87_div(f, 1.0, st(r)));
                info.has_object = true;
                info.flag = 1;
                if (q.collision_test(info)) {
                    s.possibly_stuck = 1;
                    collide = false;
                }
            }
            if (q.is_controlled() && collide && !q.is_vehicle_race()) {
                SweepResult r{fr.sweep_target, fr.before_lift, fr.check_dist};
                q.sweep(fr.start, {position[0], position[1], position[2]}, rider, false, r);
                fr.sweep_target = r.position;
                fr.before_lift = r.skitter_dir;
                fr.check_dist = r.skitter_dist;
                if (0.0f < fr.check_dist) s.possibly_stuck = 1;
            }
        }
    }
    if (s.possibly_stuck == 0 && !keep) {
        fr.actor[0] = tf(position[0]);
        fr.actor[1] = tf(position[1]);
        fr.actor[2] = to_f(x87_sub(f, st(position[2]), X87(fr.height)));
        q.set_actor_location(fr.actor);
        position[2] = to_f(x87_sub(f, st(position[2]), X87(fr.height)));
        if (q.is_player_spawn()) q.after_place();
    } else {
        out.held = s.possibly_stuck == 0;
        position[5] = 0.0f;
        for (int k = 0; k < 3; ++k) position[k] = tf(s.last_good[static_cast<std::size_t>(k)]);
    }
    if (hit && s.possibly_stuck == 0 && q.can_swim_jump_now() && !q.on_ladder()) {
        // 0x508aa3..0x508ae8: check_dist = |speed|; offset = the position (z + height) - start.
        fr.check_dist = to_f(f.sqrt(length_squared(f, st(position[3]), st(position[4]), st(position[5]))));
        double pz = x87_add(f, st(position[2]), X87(fr.height)); // z + height
        fr.offset[0] = to_f(x87_sub(f, st(position[0]), X87(fr.start[0])));
        fr.offset[1] = to_f(x87_sub(f, st(position[1]), X87(fr.start[1])));
        fr.offset[2] = to_f(x87_sub(f, pz, X87(fr.start[2])));
        normalize_vector(fr.offset.data(), f);
        fr.sweep_target = {tf(position[3]), tf(position[4]), tf(position[5])};
        normalize_vector(fr.sweep_target.data(), f);
        // 0x508b0f..0x508b5a: max(speed dir . offset, 0) * |speed| along offset.
        double p0 = x87_mul(f, st(fr.sweep_target[0]), st(fr.offset[0]));
        double p1 = x87_mul(f, st(fr.sweep_target[1]), st(fr.offset[1]));
        double p01 = x87_add(f, p1, p0);                       // p1 + p0
        double p2 = x87_mul(f, st(fr.sweep_target[2]), st(fr.offset[2]));
        double dot = x87_add(f, p2, p01);
        // Keeps the dot unless 0 > dot (unordered keeps it).
        double v = 0.0 > dot ? 0.0 : dot;
        v = x87_mul(f, v, X87(fr.check_dist));                         // x |speed|
        position[3] = to_f(x87_mul(f, st(fr.offset[0]), v));
        position[4] = to_f(x87_mul(f, st(fr.offset[1]), v));
        position[5] = to_f(x87_mul(f, st(fr.offset[2]), v));
    }
    q.finish_move();
    q.after_move(fr.start, fr.actor);
    for (int k = 0; k < 12; ++k) entry.after[static_cast<std::size_t>(k)] = tf(s.physics[static_cast<std::size_t>(k)]);
    entry.floor_after = tf(s.floor_height);
    // 0x508bc2..0x508bed: stopped unless the speed's square is above 0.
    double speed2 = length_squared(f, st(position[3]), st(position[4]), st(position[5]));
    out.stopped = !(speed2 > 0.0);
    return out;
}

} // namespace hailstone::game::mechanics
