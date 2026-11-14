#include "game/mechanics/movement.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/wrap_math.h"
#include "game/mechanics/fpu.h"

namespace hailstone::game::mechanics {

namespace {

constexpr float kHundredth = 0.01f; // 0x9c4be4

std::int32_t wrap_add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t wrap_mul(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
std::int32_t wrap_neg(std::int32_t a) { return static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(a)); }
// Signed division, truncating; the original faults on a zero divisor and on INT_MIN / -1 (0 and INT_MIN here).
std::int32_t idiv(std::int32_t a, std::int32_t b) {
    if (b == 0) return 0;
    if (b == -1) return wrap_neg(a);
    return a / b;
}
// The wrapped product a * b divided by d (signed) without the pattern MSVC folds (core/wrap_math.h).
std::int32_t mul_idiv(std::int32_t a, std::int32_t b, std::int32_t d) {
    if (d == 0 || d == -1) return idiv(wrap_mul(a, b), d);
    return core::wrapped_mul_div(a, b, d);
}

} // namespace

// ------------------------------------------------------------ speed effects

namespace {

std::uint32_t bits_of(float v) {
    std::uint32_t b;
    std::memcpy(&b, &v, 4);
    return b;
}


// An x87 operand: its value, and whether it was a signalling NaN when loaded
// (converting it to double already made it quiet).
void stop(SpawnSpeeds& s, std::int32_t rate) {
    s.speed_multiplier = static_cast<float>(rate); // the integer stored as a float
    s.speed_run = s.speed_y = s.speed_x = s.speed_heading = 0.0f;
}

} // namespace

float through_fpu(float v) {
    if (!std::isnan(v)) return v;
    std::uint32_t b = bits_of(v) | 0x00400000u;
    std::memcpy(&v, &b, 4);
    return v;
}

bool apply_movement_rate(std::int32_t rate, bool running, const Spa271Caches& spa271, bool items_count,
                         SpawnSpeeds& spawn, SpawnSpeeds* mount, const Fpu& f) {
    if (mount) {
        if (rate > 0) {
            mount->speed_multiplier = 0.0f;
            return false;
        }
        if (rate <= -1000) stop(*mount, rate);
    }
    if (rate <= -1000) {
        stop(spawn, rate);
        return true;
    }
    if (rate < -65) rate = -65;
    std::int32_t magnitude = rate < 0 ? -rate : rate;
    // |rate| * 0.01f (stored as the percent, kept unrounded) * RunSpeed, stored as a float.
    double p = f.mul(static_cast<double>(magnitude), static_cast<double>(kHundredth));
    auto percent = static_cast<float>(p);
    spawn.speed_multiplier = static_cast<float>(f.mul(p, static_cast<double>(spawn.run_speed)));
    if (rate < 0) spawn.speed_multiplier = -spawn.speed_multiplier;
    if (running) {
        std::uint32_t sum = static_cast<std::uint32_t>(spa271.buff) + static_cast<std::uint32_t>(spa271.aa);
        if (items_count) sum += static_cast<std::uint32_t>(spa271.item);
        auto cap = static_cast<std::int32_t>(sum);
        if (rate >= 0 && rate < cap)
            spawn.speed_multiplier = static_cast<float>(f.mul(static_cast<double>(cap), static_cast<double>(kHundredth)));
    }
    if (mount) {
        if (rate >= 0) mount->speed_multiplier = 0.0f;
        else mount->speed_multiplier = static_cast<float>(-f.mul(static_cast<double>(mount->run_speed), percent));
    }
    return false;
}

MovementRate eqspa_movement_rate(std::int32_t rate, float run_speed, bool running, std::int32_t spa271,
                                 std::optional<float> mount_run_speed, const Fpu& fpu) {
    MovementRate out;
    SpawnSpeeds me, mount;
    me.run_speed = run_speed;
    if (mount_run_speed) mount.run_speed = *mount_run_speed;
    if (mount_run_speed && rate > 0) out.changed_player = false;
    out.rooted = apply_movement_rate(rate, running, {spa271, 0, 0}, true, me, mount_run_speed ? &mount : nullptr, fpu);
    out.speed_multiplier = me.speed_multiplier;
    if (mount_run_speed) out.mount_multiplier = mount.speed_multiplier;
    return out;
}

float set_accel(float current, float target, bool immediate, std::optional<AccelRider> rider, float rate_scale,
                float run_speed, const Fpu& f) {
    if (target == current) return current;
    if (immediate || !rider) return through_fpu(target);
    // scale * 50.0f * RunSpeed, stored as a float.
    auto rate = static_cast<float>(x87_mul(f, x87_mul(f, X87(rate_scale), X87(50.0)), X87(run_speed)));
    bool rising = current < target;
    double divisor = (!rider->has_pc || rider->pc_flag) ? 1500.0 : (rising ? 400.0 : 200.0);
    double step = x87_div(f, X87(rate), X87(divisor)); // rate / divisor
    if (rising) {
        double next = x87_add(f, X87(step), X87(current)); // rounds to float only when stored
        return static_cast<double>(target) < next ? through_fpu(target) : static_cast<float>(next);
    }
    double next = x87_sub(f, X87(current), X87(step));
    return static_cast<double>(target) > next ? through_fpu(target) : static_cast<float>(next);
}

float cap_speed_multiplier(float speed_multiplier, std::int32_t spa290, const Fpu& f) {
    if (!(speed_multiplier >= -1000.0f) || !(kSpeedCap < speed_multiplier)) return speed_multiplier;
    double cap = spa290 < 1 ? static_cast<double>(kSpeedCap)
                            : f.add(f.mul(static_cast<double>(spa290), static_cast<double>(kHundredth)),
                                    static_cast<double>(kSpeedCap));
    return cap < static_cast<double>(speed_multiplier) ? static_cast<float>(cap) : speed_multiplier;
}

double swimming_speed(float base, std::int32_t swimming_skill, const Fpu& f, float speed_scale, float skill_scale,
                      float* scaled_base) {
    // scale * base, stored as the base.
    auto scaled = static_cast<float>(x87_mul(f, static_cast<double>(through_fpu(speed_scale)), X87(base)));
    if (scaled_base) *scaled_base = scaled;
    // skill * 0.01 (a double) * scale + base.
    double skill = f.mul(static_cast<double>(swimming_skill), 0.01);
    return x87_add(f, x87_mul(f, skill, X87(skill_scale)), X87(scaled));
}

float add_physics_pitch(float pitch, float delta, bool riding_controlled_mount, const Fpu& f) {
    // pitch + delta, stored as the pitch; the unrounded sum compared with 128.0f and the minimum.
    double sum = x87_add(f, static_cast<double>(through_fpu(pitch)), X87(delta));
    float minimum = riding_controlled_mount ? -64.0f : -128.0f;
    if (128.0 < sum) return 128.0f;
    if (sum < static_cast<double>(minimum)) return minimum;
    return static_cast<float>(sum);
}

float sneak_speed(std::uint8_t class_id, std::uint8_t level) {
    float speed = 0.5f;
    if (class_id == 9 && level > 50) {
        float extra = static_cast<float>(level - 50);
        if (10.0f < extra) extra = 10.0f;
        speed = extra * 0.1f + speed;
    }
    return speed;
}

// -------------------------------------------------------------- encumbrance

float calculate_weight(const WeightInput& in, const Fpu& f) {
    auto carried = static_cast<float>(in.carried);
    auto capacity = static_cast<float>(in.strength);
    auto hpf = static_cast<float>(in.hp);
    float factor = 1.0f;
    int threshold = in.low_level_exempt_off ? 0 : 3;
    if (static_cast<double>(in.max_hp) > static_cast<double>(hpf) && in.level > threshold) {
        double ratio = f.div(hpf, static_cast<float>(in.max_hp)); // hp / max
        if (static_cast<double>(0.2f) > ratio && 50.0f > hpf) factor = 0.5f;
        else if (ratio < static_cast<double>(0.33f) && hpf < 100.0f) factor = 0.75f;
    }
    if (!(carried > capacity)) return factor;
    if (capacity == 0.0f) return factor;
    // factor - (carried - capacity) / (2 capacity), compared with 0 before the store.
    double r = f.sub(factor, f.div(f.sub(carried, capacity), f.add(capacity, capacity)));
    factor = 0.0 > r ? 0.0f : static_cast<float>(r);
    if (in.bazaar) factor = 1.0f;
    if (in.barter) return 1.0f;
    return factor;
}

std::uint32_t encumbrance_message(std::int32_t& shown, std::int32_t carried, std::int32_t strength) {
    if (!(static_cast<float>(carried) > static_cast<float>(strength))) {
        if (shown >= 0 && shown != 1) return 0;
        bool was = shown == 1;
        shown = 0;
        return was ? kStrNoLongerEncumbered : 0;
    }
    if (shown <= 0) {
        shown = 1;
        return kStrEncumbered;
    }
    return 0;
}

std::int32_t calc_carried_weight(std::span<const CarriedItem> items, std::int32_t spa221, const Fpu& f) {
    float total = 0.0f;
    auto tenth = [&](std::int32_t w) { // w * 0.1f, stored as a float
        return static_cast<float>(f.mul(static_cast<double>(w), static_cast<double>(0.1f)));
    };
    for (const CarriedItem& item : items) {
        total = static_cast<float>(f.add(tenth(item.weight), total));
        if (!item.bag) continue;
        auto remaining = static_cast<float>(f.sub(100.0, static_cast<double>(item.weight_reduction)));
        for (std::int32_t w : item.contents) {
            double share = f.mul(f.mul(remaining, static_cast<double>(kHundredth)), tenth(w));
            total = static_cast<float>(f.add(share, total));
        }
    }
    double cut = f.mul(f.mul(static_cast<double>(spa221), total), static_cast<double>(kHundredth));
    double value = f.sub(total, cut);
    if (32766.0 < value) return 32766;
    if (0.0 > value) return 0;
    return Fpu::ftol(value);
}

// ------------------------------------------------------------ levitation

std::int32_t levitation_mode_value(std::int32_t current, bool spa57_value0, bool spa57_value1,
                                   std::optional<std::int32_t> rider_mode, bool no_levitate_zone, bool may_fly_anyway) {
    std::int32_t mode = 0;
    if (spa57_value0) mode = 2;
    else if (spa57_value1) mode = 5;
    if (current == 1) mode = current;
    if (rider_mode) mode = *rider_mode;
    if (!may_fly_anyway && no_levitate_zone) mode = 0;
    return mode;
}

FlyMode levitation_mode(FlyMode current, bool spa57_value0, bool spa57_value1, std::optional<FlyMode> rider_mode,
                        bool no_levitate_zone, bool may_fly_anyway) {
    std::optional<std::int32_t> rider;
    if (rider_mode) rider = static_cast<std::int32_t>(*rider_mode);
    return static_cast<FlyMode>(levitation_mode_value(static_cast<std::int32_t>(current), spa57_value0, spa57_value1,
                                                      rider, no_levitate_zone, may_fly_anyway));
}

FlyMode local_fly_mode(FlyMode current, const FlyModeEvents& ev, bool spa57_value0, bool spa57_value1,
                       std::optional<FlyMode> rider_mode, bool no_levitate_zone, bool may_fly_anyway) {
    FlyMode mode = current;
    if (ev.levitate_faded && !spa57_value0 && !spa57_value1) mode = FlyMode::Ground;
    if (ev.levitate_landed) {
        if (!may_fly_anyway && no_levitate_zone) mode = FlyMode::Ground;
        else mode = *ev.levitate_landed != 0 ? FlyMode::LevitateRun : FlyMode::Levitate;
    }
    if (ev.server_mode) mode = *ev.server_mode;
    if (ev.recache) mode = levitation_mode(mode, spa57_value0, spa57_value1, rider_mode, no_levitate_zone, may_fly_anyway);
    return mode;
}

BuffLevitationChange buff_levitation_change(std::span<const std::uint32_t> before, std::span<const std::uint32_t> after,
                                            const std::function<std::optional<std::int32_t>(std::uint32_t)>& levitate_base2) {
    BuffLevitationChange out;
    const std::size_t n = std::max(before.size(), after.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t was = i < before.size() ? before[i] : kNoBuffSpell;
        const std::uint32_t now = i < after.size() ? after[i] : kNoBuffSpell;
        if (was == now) continue;
        out.changed = true;
        if (was != kNoBuffSpell && levitate_base2(was)) out.faded = true;
        if (now != kNoBuffSpell)
            if (std::optional<std::int32_t> base2 = levitate_base2(now)) out.landed = base2;
    }
    return out;
}

// ------------------------------------------------------------- air supply

std::int32_t max_air_supply(std::int32_t stamina, std::int32_t spa246, std::uint32_t race) {
    std::int32_t base = wrap_add(stamina, -30);
    std::int32_t percent = spa246 <= 0 ? 100 : spa246;
    std::int32_t air = mul_idiv(base, percent, 100);
    if (race == 128) return 127;
    if (race == 330) return 256;
    if (air < 10) return 10;
    return air > percent ? percent : air;
}

bool can_breathe(std::uint8_t head_wet, bool no_air_zone, std::uint32_t race, std::int32_t spa14) {
    if ((head_wet > 4 || no_air_zone) && race != 74 && race != 24 && race != 35) return spa14 > 0;
    return true;
}

AirSupply process_air_supply(std::int32_t air, bool breathe, std::int32_t max_air, std::int32_t max_hp,
                             const Fpu& f) {
    if (breathe) return {max_air, 0};
    if (air >= 1) return {air - 1, 0};
    // Max_HP * 0.125 (a double), truncated.
    std::int32_t damage = Fpu::ftol(f.mul(static_cast<double>(max_hp), 0.125));
    return {0, damage > 0 ? damage : 0};
}

// ----------------------------------------------------------- fall damage

FallDamage apply_fall_damage(const FallDamageInput& in, const Fpu& f) {
    FallDamage out;
    // CurrWeight * 0.01f + v - 1.5f, never rounded to a float.
    double v = f.sub(f.add(f.mul(static_cast<double>(in.carried), static_cast<double>(kHundredth)), in.fall_speed), 1.5);
    std::int32_t damage = 0;
    if (v >= 9.0) {
        damage = kLethalFall;
    } else if (0.0 < v) {
        damage = Fpu::ftol(f.mul(f.mul(10.0, v), v)); // 10 * v * v
        if (damage > 0) {
            std::int32_t agi = in.heroic_agi_bonus;
            damage = wrap_add(damage, wrap_neg(mul_idiv(agi, damage, wrap_add(agi, 1000))));
            if (in.safe_fall_adjusted > 0) {
                std::int32_t base = in.safe_fall_base;
                damage = wrap_add(damage, wrap_add(wrap_neg(base), wrap_neg(mul_idiv(in.spa228, base, 100))));
                out.safe_fall_used = true;
            }
        }
        if (damage <= 0) damage = 0;
    }
    if (in.mounted && damage < 200) damage = 0;
    if (in.no_fall_damage_zone) damage = 0;
    out.damage = damage;
    return out;
}

std::optional<std::int32_t> fall_sound(bool has_pc, std::uint8_t spawn_type, std::int32_t damage_result) {
    if (!has_pc || spawn_type != 0) return std::nullopt;
    return damage_result >= 1 ? 0x73 : 0x72;
}

SwimJump do_swim_jump(bool jump, bool surface_clear, float swim_strength, float speed_z, std::uint32_t now,
                      std::uint32_t next_swim, const Fpu& f) {
    SwimJump out{next_swim, speed_z, false, false};
    if (!can_swim_jump_now(next_swim, now)) return out;
    out.next_swim = now + kSwimJumpCooldownMs;
    if (!jump) return out;
    out.surface_probed = true;
    if (!surface_clear) return out;
    // SwimStrength * 3.0f + SpeedZ, stored as a float.
    out.speed_z = static_cast<float>(x87_add(f, X87(x87_mul(f, X87(swim_strength), X87(3.0))), X87(speed_z)));
    out.jumped = true;
    return out;
}

glm::vec3 swim_probe_point(const glm::vec3& spawn, float heading, float avatar_height, const Fpu& f) {
    // cos(Heading) (table +0xc) * height stored as ahead; sin(Heading) (table +8) * height kept unrounded;
    // y + ahead, that + x, z - 1, each stored.
    auto ahead = static_cast<float>(x87_mul(f, X87(static_cast<double>(table_cos(heading))), X87(avatar_height)));
    double side = x87_mul(f, X87(static_cast<double>(table_sin(heading))), X87(avatar_height));
    return glm::vec3(static_cast<float>(x87_add(f, X87(spawn.x), X87(ahead))),
                     static_cast<float>(x87_add(f, X87(side), X87(spawn.y))),
                     static_cast<float>(x87_sub(f, X87(spawn.z), X87(1.0f))));
}

RegionDamage region_damage(std::uint8_t body_wet, std::uint32_t zone, std::int32_t fire_resist,
                           std::int32_t cold_resist, std::int32_t disease_resist, std::int32_t zone_lava_damage,
                           std::int32_t zone_min_lava_damage) {
    RegionDamage out;
    std::uint8_t type = kEnvDamageLava;
    if (body_wet < 3 || body_wet == 5) return out;
    std::int32_t damage = 0;
    if (body_wet == 3 || body_wet == 7) {
        // Zone +0x210 above 1000 (signed compare) keeps its low 15 bits.
        std::int32_t r = static_cast<std::int32_t>(zone);
        if (r > 1000) r &= 0x7fff;
        std::int32_t resist = fire_resist;
        std::int32_t divisor = 3;
        if (r == 200) {
            resist = disease_resist;
            type = kEnvDamageFire;
        } else if (r == 0x116) {
            divisor = 5;
        }
        damage = wrap_add(zone_lava_damage, wrap_neg(idiv(resist, divisor))); // sub: wraps
        if (damage < zone_min_lava_damage) damage = zone_min_lava_damage;
    }
    if (body_wet == 4 || body_wet == 8) {
        damage = 50 - idiv(cold_resist, 3);
        type = kEnvDamageFreezing;
        if (damage < 10) damage = 10;
    }
    if (damage < 1) return out;
    out.damage = damage;
    out.type = type;
    return out;
}

// ----------------------------------------------------------------- the jump

namespace {
constexpr std::uint8_t kJumpStand = 100, kJumpDuck = 111; // 'd', 'o'
constexpr std::int32_t kNoJumpRaceA = 0x8e, kNoJumpRaceB = 0x8f;
constexpr float kJumpScale = 1.65f;        // 0x9d24c4
constexpr float kJumpSwimLift = 0.5f;      // 0x9c4998
constexpr std::int32_t kJumpMaxCost = 50;
} // namespace

std::uint32_t jump_crouch_window(std::optional<double> length) {
    // Truncated to 64 bits, the low dword; plus one; above 500 (unsigned) keeps 500.
    if (!length || !(*length > -2147483648.0 && *length < 2147483648.0)) return kJumpWindowMs;
    std::uint32_t w = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::trunc(*length))) + 1u;
    return w > kJumpWindowMs ? kJumpWindowMs : w;
}

std::string_view jump_duck_clip(bool new_style_hierarchy) { return new_style_hierarchy ? "CRCH_BA_1" : "L08"; }

void main_loop_jump(JumpState& g, JumpSpawn& s, JumpQueries& q, std::uint32_t now, const Fpu& f) {
    // @0x53e85c: the race (vf88, asked twice) 0x8f or 0x8e drops the jump.
    if (q.race() == kNoJumpRaceB || q.race() == kNoJumpRaceA) {
        g.jump_started = 0;
        return;
    }
    // @0x53e88f: CanIFitHere's duck stops a jump and crouches (not a mount race); a spawn that stays up clears it.
    if (g.auto_duck && !q.mount_race()) {
        g.jump_started = 0;
        if (q.stand_state() != kJumpDuck) {
            q.change_position(kJumpDuck);
            if (q.stand_state() != kJumpDuck) g.auto_duck = 0;
        }
    }
    // @0x53e8e5: a start after now (the clock went back) is dropped; none: nothing to do.
    if (g.jump_started > now) {
        g.jump_started = 0;
        return;
    }
    if (g.jump_started == 0) return;
    const std::uint32_t window = q.crouch_window();
    auto dirty = [&] { (s.local ? g.local_dirty : g.other_dirty) = true; };
    // +0x10 * 1.65f [* CalculateWeight] + SpeedZ, stored as SpeedZ.
    auto lift = [&] {
        double weight = s.local ? q.calculate_weight() : 1.0;
        double v = x87_mul(f, X87(s.jump_strength), X87(kJumpScale));
        if (s.local) v = x87_mul(f, X87(weight), X87(v));
        s.speed_z = static_cast<float>(x87_add(f, X87(v), X87(s.speed_z)));
    };
    // @0x53ecba: OP_Jump, then min(50, Max_Endurance / 10) endurance (Max_Endurance asked again when below 50).
    auto spend = [&] {
        q.send_jump();
        std::int32_t cost = kJumpMaxCost;
        if (idiv(q.max_endurance(), 10) < cost) cost = idiv(q.max_endurance(), 10);
        q.spend_endurance(cost);
    };
    if (now - g.jump_started < window) {
        if (s.body_wet != 0) {
            // @0x53eb3e: in water, SpeedZ += 0.5 each frame of the window.
            s.speed_z = static_cast<float>(x87_add(f, X87(s.speed_z), X87(kJumpSwimLift)));
            return;
        }
        // Moving forward (0 < SpeedRun; NaN is not) jumps at once.
        if (0.0f < s.speed_run) {
            g.jump_started = 0;
            if (!q.feet_on_ground()) return;
            if (q.stand_state() == kJumpDuck) q.change_position(kJumpStand);
            g.jump_ended = now;
            dirty();
            lift();
            if (!s.sneaking) q.push_along_heading(1.0f);
            spend();
            return;
        }
        // Moving back drops the jump; still (or NaN) crouches.
        if (0.0f > s.speed_run) {
            g.jump_started = 0;
            return;
        }
        if (!q.mount_race()) {
            if (q.stand_state() == kJumpStand) q.change_position(kJumpDuck);
            if (q.stand_state() != kJumpDuck) g.jump_started = 0;
        }
        dirty();
        return;
    }
    // @0x53eb4f: the window is over.
    g.jump_started = 0;
    if (q.mount_race()) {
        q.mount_jump();
        return;
    }
    if (q.stand_state() != kJumpDuck) return;
    q.change_position(kJumpStand);
    if (!q.feet_on_ground()) return;
    g.jump_ended = now;
    dirty();
    lift();
    spend();
}

// ---------------------------------------------------------- the controller

void MovementController::reset(const glm::vec3& feet, float heading) {
    body = PhysicsBody{};
    body.feet = feet;
    body.floor_height = feet.z;
    body.heading = heading;
    crouching = false;
    controls_ = {};
    next_swim_ = 0;
    fell_once_ = false;
}

namespace {

// ProcessControls' questions answered for the local player, and its actions
// applied to the local player (no boats, mounts or cameras yet).
class PlayerControls final : public ControlsQueries {
public:
    PlayerControls(MovementController& ctl, ControlsState& s, const CharacterInput& ch, ControlEvents& ev,
                   std::uint32_t now, bool loot_open, float step_scale, bool camera_active)
        : ctl_(ctl), s_(s), ch_(ch), ev_(ev), now_(now), loot_open_(loot_open), step_scale_(step_scale),
          camera_active_(camera_active) {}
    bool player_vf58(ControlWho) override { return false; }
    std::int32_t physics_mode(ControlWho) override { return static_cast<std::int32_t>(ctl_.body.fly_mode); }
    float calculate_weight() override { return ch_.weight_factor; }
    bool is_mount_race(ControlWho) override { return ctl_.body.mount_race; }
    // Riding: the local player's mount is the controlled spawn, whose rider is the local player.
    ControlRef get_mount(ControlWho who) override {
        return ch_.riding && who == ControlWho::Local ? ControlRef::Controlled : ControlRef::None;
    }
    ControlRef get_rider(ControlWho who) override {
        return ch_.riding && who == ControlWho::Controlled ? ControlRef::Local : ControlRef::None;
    }
    bool feet_on_ground() override { return mechanics::feet_on_ground(ctl_.body); }
    std::int32_t endurance() override { return ch_.endurance; }
    std::int32_t max_endurance() override { return ch_.max_endurance; }
    std::int32_t swimming_skill() override { return ch_.swimming_skill; }
    std::int32_t speed_cap_effect() override { return ch_.spa290; }
    bool loot_window_active() override { return loot_open_; }
    ControlRef controlled_spawn_of() override { return ControlRef::Controlled; }
    // The active camera's virtual +0x2c and +0x24 (game/cameras.h).
    bool camera_active() override { return camera_active_; }
    double camera_heading() override { return s_.camera.heading; }
    void follow(ControlWho) override {}
    void do_attack(std::int32_t slot) override { ev_.attacks.push_back(slot); }
    void turn_off_auto_follow() override { ev_.turn_off_auto_follow = true; }
    // SetAccel(target, 0): at once without a rider; a ridden mount ramps toward it. The rider's PcClient exists
    // (the local player's); its virtual +0x80 is the combat flag (OP_RestState).
    void set_accel(ControlWho, float target) override {
        std::optional<AccelRider> rider;
        if (ch_.riding) rider = AccelRider{true, ch_.in_combat};
        s_.controlled.speed_run = mechanics::set_accel(s_.controlled.speed_run, target, false, rider, step_scale_,
                                                       ch_.run_speed);
    }
    void print(std::uint32_t id, std::int32_t) override { ev_.message = id; }
    void do_swim_jump() override {
        ev_.swim_jumped = ctl_.swim_jump(true, ctl_.swim_surface_clear, now_) || ev_.swim_jumped;
    }
    void proc_mouse(bool) override {}
    // 0x516cd0 on that spawn's Angle (+0x84): at least -64 for both while the local player's mount is the
    // controlled player (@0x516cd0 asks pinstLocalPlayer's GetMount), else -128. Riding, the mount's pitch is the
    // body's; the rider's own is overwritten on its next physics step (PhysicsObject::vf8 @0x59e170: the local
    // rider's Angle = its mount actor's pitch, vf8c), so the rider and the camera look where the mount does.
    void add_pitch(ControlWho who, float delta) override {
        if (ch_.remote_control && who == ControlWho::Local) {
            s_.local.pitch = add_physics_pitch(s_.local.pitch, delta, false);
            return;
        }
        if (ch_.riding && who == ControlWho::Local) {
            s_.local.pitch = add_physics_pitch(s_.local.pitch, delta, true);
            return;
        }
        s_.controlled.pitch = add_physics_pitch(s_.controlled.pitch, delta, ch_.riding);
    }
    void close_loot_window() override { ev_.close_loot = true; }
    void sit() override { ev_.sit_toggle = true; }
    // The active camera's setters (+0x20, +0x10, +0x18, +0x14): the caller applies them (ev_.camera).
    void set_camera_heading(float v) override {
        s_.camera.heading = v;
        ev_.camera = s_.camera;
    }
    void set_camera_pan(float v) override {
        s_.camera.pan = v;
        ev_.camera = s_.camera;
    }
    void set_camera_distance(float v) override {
        s_.camera.distance = v;
        ev_.camera = s_.camera;
    }
    void set_camera_pitch(float v) override {
        s_.camera.pitch = v;
        ev_.camera = s_.camera;
        ev_.camera_pitch_set = true;
    }
    void send_control_boat(std::uint32_t) override {}

private:
    MovementController& ctl_;
    ControlsState& s_;
    const CharacterInput& ch_;
    ControlEvents& ev_;
    std::uint32_t now_;
    bool loot_open_;
    float step_scale_;
    bool camera_active_;
};

// DoMainLoop's jump on the local player: ChangePosition is the crouch the
// caller sends (StandState 111 / 100), the window the caller's
// jump_crouch_window of the controlled spawn's duck clip.
class PlayerJump final : public JumpQueries {
public:
    PlayerJump(MovementController& ctl, ControlSpawn& c, const CharacterInput& ch, ControlEvents& ev)
        : ctl_(ctl), c_(c), ch_(ch), ev_(ev) {}
    std::int32_t race() override { return ch_.race; }
    bool mount_race() override { return ctl_.body.mount_race; }
    std::uint8_t stand_state() override { return c_.stand_state; }
    void change_position(std::uint8_t state) override {
        if (c_.stand_state == state) return;
        c_.stand_state = state;
        ctl_.crouching = state == 111;
        ev_.crouch_changed = !ev_.crouch_changed;
    }
    std::uint32_t crouch_window() override { return ch_.crouch_window; }
    bool feet_on_ground() override { return mechanics::feet_on_ground(ctl_.body); }
    double calculate_weight() override { return ch_.weight_factor; }
    void push_along_heading(float amount) override { mechanics::push_along_heading(ctl_.body, amount); }
    void mount_jump() override { ev_.mount_jump = true; }
    void send_jump() override { ev_.jumped = true; }
    std::int32_t max_endurance() override { return ch_.max_endurance; }
    void spend_endurance(std::int32_t cost) override { ev_.endurance_cost = cost; }

private:
    MovementController& ctl_;
    ControlSpawn& c_;
    const CharacterInput& ch_;
    ControlEvents& ev_;
};

constexpr std::uint8_t kStandStanding = 100, kStandFrozen = 102, kStandLooting = 105, kStandSitting = 110,
                       kStandDucking = 111, kStandLying = 115, kStandDead = 120;

} // namespace

ControlEvents MovementController::controls(const ControlInput& in, const CharacterInput& ch, std::uint32_t now) {
    ControlEvents ev;
    PhysicsBody& b = body;
    b.dead = ch.dead;
    // ProcessControls (controls.h, the original's rules) on the local player.
    ControlsState& s = controls_;
    // The held keys are the key handlers' (command()); the drivers' keys add to them for this frame.
    const ControlKeys held = s.keys;
    s.keys.forward = held.forward || in.forward;
    s.keys.backward = held.backward || in.back;
    s.keys.turn_left = held.turn_left || in.turn_left;
    s.keys.turn_right = held.turn_right || in.turn_right;
    s.keys.strafe_left = held.strafe_left || in.strafe_left;
    s.keys.strafe_right = held.strafe_right || in.strafe_right;
    if (in.jump) s.jump_key = 1;
    s.keys.key_148c = in.move_down;
    s.mouse_look = in.mouse_look;
    s.auto_forward = in.both_buttons;
    s.camera_index = in.camera_index;
    s.camera = in.camera;
    // g_LocalPhysicsTimeFactor: PhysicsObject::vf18 @0x59dbd0 sets it to the local player's (or its mount's) last
    // physics step's factor, elapsed ms * 0.02 (step()).
    s.rate_scale = step_scale_;
    s.keys.center_view = in.look_spring;
    s.forward_sits = in.forward_sits;
    s.run_walk_state = ch.run_walk;
    s.quiet_net_log = ch.gm_flag;
    s.skip_controls = ch.interface_locked;
    s.now = now;
    s.controlled_is_local = !ch.riding && !ch.remote_control;
    ControlSpawn& c = s.controlled;
    c.speed_multiplier = ch.speed_multiplier;
    c.swim_state = b.body_wet;
    c.sneaking = ch.sneaking;
    c.stand_state = ch.dead      ? kStandDead
                    : ch.sitting ? kStandSitting
                    : ch.looting ? kStandLooting
                    : ch.lying   ? kStandLying
                    : ch.frozen  ? kStandFrozen
                    : crouching  ? kStandDucking
                                 : kStandStanding;
    if (ch.riding) {
        // The mount stands (0x4b15b0 keeps a mount race's StandState at 100); a frozen rider (stunned, the
        // PcBase's +0xfc) keeps it still. The rider is the local player of ProcessControls' questions.
        c.stand_state = ch.frozen || ch.dead ? kStandFrozen : kStandStanding;
        c.sneaking = false;
        s.local.stand_state = kStandStanding;
        s.local.speed_run = 0.0f;
        s.local.speed_heading = 0.0f;
    }
    c.walk_speed = ch.walk_speed;
    c.height = ch.avatar_height;
    c.run_speed = ch.run_speed;
    c.sneak_speed = ch.sneak_speed;
    c.speed_run = b.speed_run;
    c.heading = b.heading;
    c.speed_heading = b.speed_heading;
    c.pitch = b.camera_angle;
    PlayerControls q(*this, s, ch, ev, now, in.loot_window_open, step_scale_, in.camera_active);
    process_controls(s, q);
    const bool move_down = s.keys.key_148c;
    s.keys = held;
    s.keys.key_148c = move_down;
    b.speed_run = c.speed_run;
    b.speed_heading = c.speed_heading;
    b.camera_angle = c.pitch;
    b.buoyancy = s.held_scale; // GetEnvCoefficients subtracts it from SpeedZ while levitating
    b.walk_speed = ch.walk_speed;
    // The jump (CEverQuest::DoMainLoop @0x53e84e, main_loop_jump) on the local player.
    PlayerJump jq(*this, c, ch, ev);
    JumpState js{s.jump_started, s.jump_ended, static_cast<std::uint8_t>(in.auto_duck ? 1 : 0), s.local_dirty,
                 s.other_dirty};
    JumpSpawn sp{b.velocity.z, b.speed_run, jump_strength, b.body_wet, c.sneaking,
                 !ch.riding && !ch.remote_control};
    main_loop_jump(js, sp, jq, now);
    s.jump_started = js.jump_started;
    s.jump_ended = js.jump_ended;
    s.local_dirty = js.local_dirty;
    s.other_dirty = js.other_dirty;
    ev.auto_duck_cleared = in.auto_duck && js.auto_duck == 0;
    b.velocity.z = sp.speed_z;
    return ev;
}

glm::vec2 MovementController::strafe_push(const CharacterInput& ch) const {
    // vf14 @0x59dd20: only without a turn or without a run speed; half the
    // run (or walk) speed, times the strafe scale when not moving forward,
    // along the heading turned a quarter left (mode 1) or right.
    if (controls_.strafing == 0) return glm::vec2(0.0f);
    if (body.speed_heading != 0.0f && body.speed_run != 0.0f) return glm::vec2(0.0f);
    float base = ch.run_walk != 0 ? ch.run_speed : ch.walk_speed;
    float amount = base * 0.5f;
    if (body.speed_run == 0.0f) amount = amount * controls_.strafe_scale;
    float h = controls_.strafing == 1 ? body.heading + 128.0f : body.heading - 128.0f;
    return glm::vec2(heading_cos(h) * amount, heading_sin(h) * amount);
}

glm::vec3 MovementController::step(const Environment& env, float elapsed_ms, const CharacterInput& ch,
                                   float hover_floor) {
    step_scale_ = elapsed_ms * 0.02f;
    return mechanics::step(body, env, elapsed_ms, strafe_push(ch), hover_floor);
}

namespace {

// The landing check's questions for the local player: a player spawn, not
// skipped, no recovery (that is for other spawns).
class PlayerLanding final : public LandingQueries {
public:
    bool skip_physics() override { return false; }
    bool spawn_vf58() override { return false; }
    bool player_spawn() override { return true; }
    std::int32_t sound_race() override { return 0; }
    bool rider_zone_client() override { return false; }
    bool no_recovery() override { return true; }
    float floor_below(float, float, float) override { return LandingSpawn::kNoFloor; }
};

} // namespace

LandingEvents MovementController::land(float floor, FallDamageInput fall) {
    LandingEvents ev;
    body.floor_height = floor;
    // The body keeps the feet: z is the feet with an avatar height of 0.
    LandingSpawn s;
    s.floor_height = floor;
    s.y = body.feet.y;
    s.x = body.feet.x;
    s.z = body.feet.z;
    s.speed_y = body.velocity.y;
    s.speed_x = body.velocity.x;
    s.speed_z = body.velocity.z;
    s.heading = body.heading;
    s.fall_start_z = body.fall_start_z;
    s.body_wet = body.body_wet;
    s.being_flung = body.being_flung;
    PlayerLanding q;
    LandingResult r = landing_and_fall_check(s, q, LandingWorld{});
    body.feet.z = s.z;
    body.velocity = glm::vec3(s.speed_x, s.speed_y, s.speed_z);
    body.fall_start_z = s.fall_start_z;
    ev.landed = r.landed;
    ev.land_sound = r.land_sound;
    if (r.fall_speed) {
        ev.fell = true;
        fall.fall_speed = through_fpu(*r.fall_speed); // IHaveFallen passes it on through the FPU
        if (!fell_once_) {
            fell_once_ = true; // ApplyFallDamage's first call after zone-in only sets the flag
        } else {
            ev.damage = apply_fall_damage(fall);
        }
    }
    return ev;
}

bool MovementController::swim_jump(bool jump, bool surface_clear, std::uint32_t now) {
    SwimJump j = do_swim_jump(jump, surface_clear, swim_strength, body.velocity.z, now, next_swim_);
    next_swim_ = j.next_swim;
    body.velocity.z = j.speed_z;
    if (j.jumped) push_along_heading(body, 1.0f);
    return j.jumped;
}

} // namespace hailstone::game::mechanics
