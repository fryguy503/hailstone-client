#include "game/mechanics/physics.h"

#include <cfloat>
#include <array>
#include <cmath>
#include <cstring>

#include <glm/gtc/constants.hpp>

namespace hailstone::game::mechanics {

namespace {

// Liquid coefficients by body environment (GetEnvCoefficients @0x8d0b6e..).
EnvCoefficients liquid(std::uint8_t wet) {
    switch (wet) {
    case 5: return {0.025f, 0.2f, 0.9f, 0.15f};
    case 6:
    case 7:
    case 8: return {0.02f, 0.8f, 0.94f, 0.15f};
    case 9:
    case 10: return {0.025f, 0.8f, 0.9f, 0.15f};
    default: return {};
    }
}

bool is_levitating(FlyMode m) { return m == FlyMode::Levitate || m == FlyMode::LevitateRun; }

// PhysicsObject vf0 @0x8d0990: a positive value becomes 0 while levitating dry.
float vf0(const PhysicsBody& body, float value) {
    if (is_levitating(body.fly_mode) && body.body_wet == 0 && 0.0f < value) return 0.0f;
    return value;
}

} // namespace

float heading_sin(float heading) { return table_sin(heading); }
float heading_cos(float heading) { return table_cos(heading); }

void update_drunken_motion(PhysicsBody& body, DrunkenMotion& motion, std::int32_t value, bool seed_velocity,
                           const Fpu& fpu) {
    if (value >= 40) {
        if (motion.phase == -1) motion.phase = 0;
        motion.amplitude = static_cast<float>(fpu.add(fpu.mul(static_cast<double>(value >> 2), 0.01f), 0.35f));
        motion.stride = (value >> 3) + 1;
    } else {
        motion = {};
    }
    if (seed_velocity && value >= 100) {
        if (body.velocity.x == 0.0f) body.velocity.x = 0.001f;
        if (body.velocity.y == 0.0f) body.velocity.y = 0.001f;
    }
}

void apply_drunken_motion(PhysicsBody& body, DrunkenMotion& motion, const Fpu& fpu) {
    if (motion.phase < 0 || (body.velocity.x == 0.0f && body.velocity.y == 0.0f)) return;
    if (motion.phase > 512) motion.phase = 0;
    const float sway = static_cast<float>(fpu.mul(table_sin(static_cast<float>(motion.phase)), motion.amplitude));
    const float sideways = static_cast<float>(fpu.add(body.heading, 128.0f));
    body.feet.x = static_cast<float>(fpu.add(body.feet.x, fpu.mul(table_cos(sideways), sway)));
    body.feet.y = static_cast<float>(fpu.add(body.feet.y, fpu.mul(table_sin(sideways), sway)));
    motion.phase += motion.stride;
}

bool feet_on_ground(const PhysicsBody& body) {
    float feet = body.feet.z + body.velocity.z;
    float bound = body.floor_height + 0.5f + 0.001f;
    return !(feet > bound);
}

float integrate_heading(float heading, float speed_heading, float dt) {
    float h = speed_heading * dt + heading;
    if (h >= 512.0f) {
        int i = 0;
        do {
            h -= 512.0f;
            if (i > 5) {
                h = 0.0f;
                break;
            }
            ++i;
        } while (h >= 512.0f);
    }
    for (int i = 0; h < 0.0f; ++i) {
        h += 512.0f;
        if (i > 5) {
            h = 0.0f;
            break;
        }
    }
    return h;
}

namespace {

// The game's answers for the local player (GetEnvCoefficients' queries):
// a player spawn in a loaded zone, not stuck, controlled, never a corpse or
// a mount, with no ladders. PlayerClient +0x58 is IsSmallBoatRace (false for
// players); the buoyancy is ProcessControls' 0xddfd78; 0x8d0800 is mode 5
// with vf64 (the walk speed) at least SpeedRun (the analysis's answers to
// docs/unknowns.md; +0x90 is taken as false). The levitation floor probe
// answers `hover_floor`.
class PlayerEnv final : public EnvQueries {
public:
    PlayerEnv(const PhysicsBody& body, const Environment& env, float hover_floor)
        : body_(body), env_(env), hover_floor_(hover_floor) {}
    bool spawn_vf90() override { return false; }
    bool possibly_stuck() override { return false; }
    bool collision_zone_loaded() override { return true; }
    float gravity() override { return env_.gravity; }
    bool mount_race() override { return body_.mount_race; }
    bool corpse() override { return false; }
    bool climbing() override { return false; }
    bool swim_zone() override { return env_.zone_id == 222; }
    bool feet_on_ground() override { return mechanics::feet_on_ground(body_) || env_.on_ground_flag; }
    bool spawn_vf58() override { return false; }
    bool controlled() override { return true; }
    bool spawn_vf50() override { return true; }
    float floor_at(float, float, float) override { return hover_floor_; }
    bool rising() override { return body_.fly_mode == FlyMode::LevitateRun && body_.walk_speed >= body_.speed_run; }
    float buoyancy() override { return body_.buoyancy; }

private:
    const PhysicsBody& body_;
    const Environment& env_;
    float hover_floor_;
};

} // namespace

EnvCoefficients env_coefficients(PhysicsBody& body, const Environment& env, float dt, float hover_floor) {
    // The body keeps the feet, so the spawn's z is the feet and its avatar
    // height 0: z - AvatarHeight (the depth over the floor) is then exact.
    // Only levitation mode 4 differs, which snaps z itself to the floor.
    EnvSpawn s;
    s.colliding_type = body.colliding_type; // DoCollisionMovement's (game/mechanics/collision_movement.h)
    s.floor_height = body.floor_height;
    s.acceleration_friction = body.acceleration_friction;
    s.area_friction = body.area_friction;
    s.sinks_in_water = body.sinks_in_water;
    s.body_wet = body.body_wet;
    s.being_flung = body.being_flung;
    EnvObject o{body.feet.y, body.feet.x, body.feet.z, body.velocity.y, body.velocity.x, body.velocity.z,
                body.speed_run};
    PlayerEnv q(body, env, hover_floor);
    std::optional<EnvCoefficients> c =
        get_env_coefficients(body.fly_mode == FlyMode::Ground, static_cast<std::int32_t>(body.fly_mode), s, o, dt, q);
    body.feet.z = o.z;
    body.velocity.z = o.speed_z;
    return c.value_or(EnvCoefficients{});
}

void push_along_heading(PhysicsBody& body, float amount) {
    body.velocity.x = heading_cos(body.heading) * amount + body.velocity.x;
    body.velocity.y = heading_sin(body.heading) * amount + body.velocity.y;
}

glm::vec3 step(PhysicsBody& body, const Environment& env, float elapsed_ms, const glm::vec2& strafe,
               float hover_floor, std::optional<SwimSteering> swim) {
    float dt = elapsed_ms * kTicksPerMs;
    if (!(dt <= kMaxStepTicks)) dt = kMaxStepTicks;
    if (body.dead) {
        body.speed_run = 0.0f;
        body.speed_heading = 0.0f;
    }
    // The push this frame: the strafe (vf14), then the run speed (vf3).
    glm::vec3 push(strafe.x, strafe.y, 0.0f);
    body.heading = integrate_heading(body.heading, body.speed_heading, dt);
    EnvCoefficients c = env_coefficients(body, env, dt, hover_floor);
    // vf3 @0x8d2040 with vf4 @0x8d14e0 (no rising while levitating dry),
    // then ApplyFriction @0x8d2160.
    struct Push : PushQueries {
        const PhysicsBody& body;
        glm::vec2 strafe;
        bool vertical_only;
        Push(const PhysicsBody& b, glm::vec2 s, bool vertical) : body(b), strafe(s), vertical_only(vertical) {}
        double vf0(float value) override { return mechanics::vf0(body, value); }
        void add_push(float p[3]) override {
            if (vertical_only) {
                // Swimming at a steep pitch still adds a full forward push. Inside the horizontal
                // follow band, keep the vertical thrust and let existing momentum slow down normally.
                p[0] = strafe.x;
                p[1] = strafe.y;
            }
            if (is_levitating(body.fly_mode) && body.body_wet == 0 && 0.0f < p[2]) p[2] = 0.0f;
        }
    } q(body, strafe, body.body_wet >= 5 && swim && swim->vertical_only);
    float p[3] = {push.x, push.y, push.z};
    const float pitch = body.body_wet >= 5 && swim ? swim->pitch : body.camera_angle;
    run_speed_push(body.speed_run, body.heading, pitch, p, true, c.accel_xy, c.accel_z, q);
    glm::vec3& v = body.velocity;
    float speed[3] = {v.x, v.y, v.z};
    apply_friction(speed, p, c.friction_xy, c.friction_z);
    v = glm::vec3(speed[0], speed[1], speed[2]);
    // Step @0x8d143b: the position moves by velocity * dt.
    glm::vec3 from = body.feet;
    body.feet.x = integrate_axis(body.feet.x, v.x, dt);
    body.feet.y = integrate_axis(body.feet.y, v.y, dt);
    body.feet.z = integrate_axis(body.feet.z, v.z, dt);
    return body.feet - from;
}

// ------------------------------------------------- the original's Step, env, landing

namespace {

// The constants GetEnvCoefficients and LandingAndFallCheck load (floats).
constexpr float kAirCoefficient = 0.8f;   // 0xa1eb28
constexpr float kAirDrag = 0.625f;        // 0xa1eb30
constexpr float kGroundDrag = 0.99f;      // 0xa1eb24
constexpr float kHoverDrop = 10.0f;       // 0x9c5b28: levitation mode 4 looks this far down
constexpr float kSinkDrop = 70.0f;        // 0x9c8358: the water modes look this far down
constexpr float kNoFloor = LandingSpawn::kNoFloor;
constexpr float kLandSpeed = -2.5f;       // 0x9c8328
constexpr float kQueryTop = 32000.0f;     // 0x9cf840

// A float copied through the FPU: a signalling NaN comes out quiet.
float quiet(float v) {
    if (!std::isnan(v)) return v;
    std::uint32_t b;
    std::memcpy(&b, &v, 4);
    b |= 0x00400000u;
    std::memcpy(&v, &b, 4);
    return v;
}

// An operand loaded onto the FPU stack (quiet); a float passed as X87 is a
// memory operand, which keeps a signalling NaN for the x87's NaN rules.
X87 st(float v) { return X87(static_cast<double>(quiet(v))); }

// The swim modes' coefficients (BodyWet 5, 6-8, 9-10); nullopt otherwise.
std::optional<EnvCoefficients> water(std::uint8_t wet) {
    if (wet == 5 || (wet >= 6 && wet <= 10)) return liquid(wet);
    return std::nullopt;
}

// The floor probe of the levitation paths (ProbeFloor): z - drop, raised to
// the spawn's floor when below it, asked at (y, x, z) rounded.
double probe_floor(const EnvSpawn& s, const EnvObject& o, float drop, EnvQueries& q, const Fpu& f) {
    double z = x87_sub(f, st(o.z), drop);
    if (z < static_cast<double>(s.floor_height)) z = st(s.floor_height).v;
    return quiet(q.floor_at(quiet(o.y), quiet(o.x), static_cast<float>(z)));
}

} // namespace

std::optional<EnvCoefficients> get_env_coefficients(bool walking, std::int32_t mode, const EnvSpawn& s,
                                                     EnvObject& o, float dt, EnvQueries& q, const Fpu& f) {
    if (s.colliding_type == 6) return EnvCoefficients{kAirCoefficient, kAirCoefficient, kAirDrag, kAirDrag};
    bool gravity = false;
    if ((walking || q.spawn_vf90()) && !q.possibly_stuck()) {
        if (q.collision_zone_loaded()) gravity = true;
        else if (!q.collision_zone_loaded() && s.being_flung) gravity = true;
    }
    if (gravity) {
        // SpeedZ -= gravity * dt; in a liquid only for a mount race, a corpse
        // or a body that sinks (else 0 * dt).
        double g = st(q.gravity()).v;
        if (s.body_wet < 5) {
            // SpeedZ - gravity * dt.
            o.speed_z = static_cast<float>(x87_sub(f, o.speed_z, x87_mul(f, g, dt)));
        } else {
            bool falls = q.mount_race() || q.corpse() || s.sinks_in_water;
            double a = falls ? static_cast<double>(static_cast<float>(g)) : 0.0;
            o.speed_z = static_cast<float>(x87_sub(f, o.speed_z, x87_mul(f, a, dt)));
        }
        bool climbing = s.type == 1 && q.climbing();
        if (s.being_flung || s.spline_id > 0) return EnvCoefficients{0.0f, 0.0f, 1.0f, 1.0f};
        if (s.body_wet != 0 && !climbing) {
            if (s.body_wet == 5 && q.swim_zone()) return EnvCoefficients{0.02f, 0.1f, 0.85f, 0.1f};
            return water(s.body_wet);
        }
        bool grounded = q.feet_on_ground() || q.possibly_stuck();
        std::int32_t state = s.colliding_type;
        if (!grounded && (state == 4 || state == 2 || state == 0))
            return EnvCoefficients{0.0f, 0.0f, kGroundDrag, kGroundDrag};
        return EnvCoefficients{quiet(s.acceleration_friction), 0.0f, quiet(s.area_friction), kGroundDrag};
    }
    // Levitating (or stuck, or no zone): mode 4 stands on the floor 10 below.
    if (mode == 4) {
        double h = probe_floor(s, o, kHoverDrop, q, f);
        if (!(h == static_cast<double>(kNoFloor))) o.z = static_cast<float>(h);
    }
    EnvCoefficients c{kAirCoefficient, kAirCoefficient, kAirDrag, kAirDrag};
    if (s.type == 1 && !q.mount_race()) return c;
    if (mode != 2 && mode != 5) return c;
    if (!q.spawn_vf58() && s.body_wet != 0) {
        if (auto w = water(s.body_wet)) c = *w;
    }
    if (!q.collision_zone_loaded()) return c;
    if (!q.controlled() && !q.spawn_vf50()) return c;
    // The depth of the feet over the floor up to 70 below picks the pull.
    double h = probe_floor(s, o, kSinkDrop, q, f);
    if (h == static_cast<double>(kNoFloor)) h = quiet(s.floor_height);
    float depth = static_cast<float>(x87_sub(f, x87_sub(f, st(o.z), s.avatar_height), h));
    bool rising = q.rising();
    bool moving = o.speed_run != 0.0f;
    if (depth > 11.0f || rising) {
        // SpeedZ -= ((0.05 when moving [+ 0.4 rising]) + 0.1 / 0.2 / 0.3 by depth) * dt.
        double base = moving ? static_cast<double>(0.05f) : 0.0;
        if (rising) base = x87_add(f, base, 0.4f);
        float pull = 60.0f < depth ? 0.3f : (30.0f < depth ? 0.2f : 0.1f);
        o.speed_z = static_cast<float>(x87_sub(f, o.speed_z, x87_mul(f, x87_add(f, base, pull), dt)));
    } else if ((5.0f > depth && mode == 5) || depth < 1.5f) {
        // SpeedZ = ((0.02 when moving) + 0.05) + SpeedZ.
        double base = moving ? static_cast<double>(0.02f) : 0.0;
        o.speed_z = static_cast<float>(x87_add(f, x87_add(f, base, 0.05f), o.speed_z));
    } else if (q.spawn_vf58() && 0.4f < depth) {
        o.speed_z = static_cast<float>(x87_sub(f, o.speed_z, x87_mul(f, st(dt), 0.1f)));
    }
    // this->vf15's buoyancy, negated, plus SpeedZ.
    o.speed_z = static_cast<float>(x87_add(f, -st(q.buoyancy()).v, o.speed_z));
    return c;
}

LandingResult landing_and_fall_check(LandingSpawn& s, LandingQueries& q, const LandingWorld& w, const Fpu& f) {
    LandingResult r;
    if (q.skip_physics()) return r;
    float floor = quiet(s.floor_height);
    float height = quiet(s.avatar_height);
    if (q.spawn_vf58()) return r;
    double feet = x87_sub(f, st(s.z), st(height)); // both loaded
    if (static_cast<double>(floor) > feet) {
        r.landed = true;
        if (q.player_spawn()) {
            if (s.being_flung) {
                r.vehicle_fall_sent = w.connected;
                r.fling_stopped = true;
                s.fling_timer_start = 0;
                s.fling_active_timer = 0;
            }
        } else if (s.being_flung) {
            s.speed_y = 0.0f;
            s.speed_x = 0.0f;
            r.fling_stopped = true;
        }
        // The landing sound: out of water, unless race 0x7f, at 2 * SpeedZ <= -2.5.
        if (s.body_wet == 0 && q.sound_race() != 0x7f && x87_add(f, st(s.speed_z), st(s.speed_z)) <= kLandSpeed)
            r.land_sound = true;
        if (kLandSpeed >= s.speed_z && s.body_wet == 0) {
            float speed = static_cast<float>(std::fabs(x87_sub(f, s.speed_z, st(kLandSpeed))));
            if (s.type == 0) {
                r.fall_speed = speed;
            } else if (q.rider_zone_client()) {
                q.rider_zone_client();
                r.fall_speed = speed;
                r.fall_on_rider = true;
            }
        }
        s.speed_z = 0.0f;
        s.z = static_cast<float>(x87_add(f, st(floor), st(height)));
        s.fall_start_z = kNoFloor;
    } else if (static_cast<double>(floor) < feet && 0.0f > s.speed_z && s.fall_start_z == kNoFloor) {
        s.fall_start_z = quiet(s.z);
    }
    // Under the world: a non-player spawn in a zone with a recovery is put
    // back on the first floor below the safe height (searched from 32000).
    if (x87_sub(f, s.z, st(height)) >= static_cast<double>(w.world_bottom)) return r; // z - height
    if (q.no_recovery() || s.type == 0 || w.zone_type == 0) return r;
    double limit = x87_sub(f, st(w.safe_floor), kHoverDrop);
    auto recovery = [&](float from, float& found) {
        double h = quiet(q.floor_below(quiet(s.y), quiet(s.x), quiet(from)));
        if (h == static_cast<double>(kNoFloor)) return 0;
        found = static_cast<float>(h);
        return limit > h ? 1 : 2;
    };
    float found = 0.0f;
    int result = recovery(kQueryTop, found);
    if (result == 0) return r;
    if (result == 2) {
        float start = found;
        if (recovery(start, found) != 1) return r;
    }
    r.placed = glm::vec4(quiet(s.y), quiet(s.x), found, quiet(s.heading));
    return r;
}

float elapsed_steps(std::uint32_t now, std::uint32_t last, const Fpu& f) {
    auto elapsed = static_cast<std::int32_t>(now - last);
    double v = elapsed; // read as signed
    if (elapsed < 0) v = f.add(v, 4294967296.0);
    return static_cast<float>(f.mul(v, static_cast<double>(kTicksPerMs)));
}

float integrate_axis(float position, float speed, float dt, const Fpu& f) {
    return static_cast<float>(x87_add(f, position, x87_mul(f, st(speed), st(dt))));
}

// ------------------------------------------------- the original's friction

namespace {

struct MathTable {
    std::array<float, 512> cos{}, sin{};
    MathTable() {
        for (int i = 0; i < 512; ++i) {
            double a = i * (3.14159265358979323846 / 256.0);
            cos[static_cast<std::size_t>(i)] = static_cast<float>(std::cos(a));
            sin[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(a));
        }
        // The quadrant entries are exact.
        for (std::size_t q = 0; q < 4; ++q) {
            cos[q * 128] = q == 0 ? 1.0f : q == 2 ? -1.0f : 0.0f;
            sin[q * 128] = q == 1 ? 1.0f : q == 3 ? -1.0f : 0.0f;
        }
    }
};
const MathTable& math_table() {
    static const MathTable t;
    return t;
}
// (int)trunc(angle) & 511, with the x87's integer indefinite (0x80000000)
// for NaN and out-of-range values.
std::size_t table_index(float angle) {
    double t = std::trunc(static_cast<double>(angle));
    std::int32_t i = (std::isnan(t) || t >= 2147483648.0 || t < -2147483648.0) ? INT32_MIN
                                                                                : static_cast<std::int32_t>(t);
    return static_cast<std::size_t>(i & 511);
}

constexpr float kTinySpeed = 1.1920929e-07f; // 0x9c3388: 2^-23

// ScaleNonZero: v[0] and v[1] by a, v[2] by b, each unless it is 0 (an equality test: a
// NaN is scaled). value_first is vf3's operand order (v * a, v * b);
// ApplyFriction's is a * v. v[1] is a * v in both.
void scale_non_zero(float v[3], float a, float b, bool value_first, const Fpu& f) {
    if (!(v[0] == 0.0f))
        v[0] = static_cast<float>(value_first ? x87_mul(f, st(v[0]), st(a)) : x87_mul(f, st(a), X87(v[0])));
    if (!(v[1] == 0.0f)) v[1] = static_cast<float>(x87_mul(f, st(a), X87(v[1])));
    if (!(v[2] == 0.0f))
        v[2] = static_cast<float>(value_first ? x87_mul(f, st(v[2]), X87(b)) : x87_mul(f, st(b), X87(v[2])));
}

} // namespace

float table_sin(float angle) { return math_table().sin[table_index(angle)]; }
float table_cos(float angle) { return math_table().cos[table_index(angle)]; }

void run_speed_push(float speed_run, float heading, float camera_angle, float push[3], bool spawn, float a, float b,
                    PushQueries& q, const Fpu& f) {
    if (!(speed_run == 0.0f)) {
        float speed = quiet(speed_run); // copied through the FPU into a temporary: quiet
        push[0] = static_cast<float>(x87_add(f, x87_mul(f, X87(q.cos(quiet(heading))), X87(speed)), X87(push[0])));
        push[1] = static_cast<float>(x87_add(f, x87_mul(f, X87(q.sin(quiet(heading))), X87(speed)), X87(push[1])));
        if (!(camera_angle == 0.0f)) {
            float climb = static_cast<float>(x87_mul(f, X87(q.sin(quiet(camera_angle))), X87(speed)));
            push[2] = static_cast<float>(x87_add(f, X87(q.vf0(climb)), X87(push[2])));
        }
    }
    if (spawn) q.add_push(push);
    scale_non_zero(push, a, b, true, f);
}

void apply_friction(float speed[3], const float push[3], float a, float b, const Fpu& f) {
    scale_non_zero(speed, a, b, false, f);
    for (int k = 0; k < 3; ++k)
        if (std::fabs(speed[k]) < kTinySpeed) speed[k] = 0.0f;
    for (int k = 0; k < 3; ++k) speed[k] = static_cast<float>(x87_add(f, st(push[k]), X87(speed[k])));
}

bool slow_in_mode5(std::int32_t levitation_mode, double walk_speed, float speed_run) {
    return levitation_mode == 5 && walk_speed >= static_cast<double>(speed_run);
}

} // namespace hailstone::game::mechanics
