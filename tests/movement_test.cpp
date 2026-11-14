// The original's movement rules (game/mechanics/movement.h, physics.h) and
// the movement packets (net/movement_packets.h). Expected values are worked
// out by hand from the original's instructions (the addresses in the
// headers).

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "game/mechanics/movement.h"
#include "game/mechanics/physics.h"
#include "net/movement_packets.h"

using namespace hailstone;
using namespace hailstone::game::mechanics;

TEST_CASE("Drunken motion sways sideways per moving step and sobering resets its parameters") {
    PhysicsBody body;
    DrunkenMotion motion;
    body.velocity.x = 1;
    update_drunken_motion(body, motion, 40);
    CHECK(motion.phase == 0);
    CHECK(motion.stride == 6);
    CHECK(motion.amplitude == doctest::Approx(0.45f));
    motion.phase = 128; // The sine peak; heading zero's sideways direction is +Y.
    apply_drunken_motion(body, motion);
    CHECK(body.feet.x == 0);
    CHECK(body.feet.y == doctest::Approx(0.45f));
    CHECK(motion.phase == 134);
    CHECK(body.velocity.x == 1); // Sway changes the proposed position, before its collision sweep.
    body.velocity = glm::vec3(0);
    const auto still = body.feet;
    apply_drunken_motion(body, motion);
    CHECK(body.feet == still);
    CHECK(motion.phase == 134);
    body.velocity.y = 1;
    motion.phase = 512;
    apply_drunken_motion(body, motion);
    CHECK(motion.phase == 518); // 512 itself is retained for one step, then values above it reset to zero.
    apply_drunken_motion(body, motion);
    CHECK(motion.phase == 6);
    update_drunken_motion(body, motion, 39);
    CHECK(motion.phase == -1);
    CHECK(motion.stride == 1);
    CHECK(motion.amplitude == 0.35f);

    body.velocity = {0, -2, 0};
    update_drunken_motion(body, motion, 100);
    CHECK(body.velocity.x == 0.001f);
    CHECK(body.velocity.y == -2);
    CHECK(motion.stride == 13);
    CHECK(motion.amplitude == doctest::Approx(0.60f));
    body.velocity = glm::vec3(0);
    update_drunken_motion(body, motion, 100, false); // A rider's update does not seed its mount's velocity.
    CHECK(body.velocity == glm::vec3(0));
}

namespace {

// n * 0.01f: the percent the original multiplies by.
float pct(int n) { return static_cast<float>(n * static_cast<double>(0.01f)); }

std::uint32_t bits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

} // namespace

TEST_CASE("eqspa_movement_rate: snare floor, percent of RunSpeed, root, SPA 271 while running") {
    // SoW-like +40: 0.4f * 0.7f.
    MovementRate r = eqspa_movement_rate(40, 0.7f, true, 0);
    CHECK(bits(r.speed_multiplier) == bits(pct(40) * 0.7f));
    CHECK_FALSE(r.rooted);
    // Snares stop at -65.
    r = eqspa_movement_rate(-90, 0.7f, true, 0);
    CHECK(bits(r.speed_multiplier) == bits(-(pct(65) * 0.7f)));
    r = eqspa_movement_rate(-65, 0.7f, false, 0);
    CHECK(bits(r.speed_multiplier) == bits(-(pct(65) * 0.7f)));
    // Roots: the rate itself.
    r = eqspa_movement_rate(-1000, 0.7f, true, 0);
    CHECK(r.rooted);
    CHECK(r.speed_multiplier == -1000.0f);
    r = eqspa_movement_rate(-999, 0.7f, true, 0);
    CHECK_FALSE(r.rooted);
    // Running with SPA 271 above the rate: 0.01 * SPA 271, not scaled by RunSpeed.
    r = eqspa_movement_rate(10, 0.7f, true, 30);
    CHECK(bits(r.speed_multiplier) == bits(pct(30)));
    r = eqspa_movement_rate(10, 0.7f, false, 30); // walking: no SPA 271
    CHECK(bits(r.speed_multiplier) == bits(pct(10) * 0.7f));
    r = eqspa_movement_rate(-10, 0.7f, true, 30); // a snare is not lifted by SPA 271
    CHECK(r.speed_multiplier < 0.0f);
    r = eqspa_movement_rate(30, 0.7f, true, 30); // not below
    CHECK(bits(r.speed_multiplier) == bits(pct(30) * 0.7f));
    // Mounted: a positive rate zeroes the mount's and leaves the player's.
    r = eqspa_movement_rate(20, 0.7f, true, 0, 1.2f);
    CHECK_FALSE(r.changed_player);
    REQUIRE(r.mount_multiplier);
    CHECK(*r.mount_multiplier == 0.0f);
    r = eqspa_movement_rate(-20, 0.7f, true, 0, 1.2f);
    REQUIRE(r.mount_multiplier);
    CHECK(bits(*r.mount_multiplier) == bits(-(1.2f * pct(20))));
    CHECK(movement_rate_effect(-10000, 40) == -10000);
    CHECK(movement_rate_effect(0, 40) == 40);
}

TEST_CASE("Speed cap: 1.092006, raised by SPA 290") {
    CHECK(cap_speed_multiplier(0.5f, 0) == 0.5f);
    CHECK(cap_speed_multiplier(2.0f, 0) == kSpeedCap);
    CHECK(bits(cap_speed_multiplier(2.0f, 20)) == bits(20.0f * 0.01f + kSpeedCap));
    CHECK(cap_speed_multiplier(1.1f, 50) == 1.1f);
    CHECK(cap_speed_multiplier(-1001.0f, 0) == -1001.0f);
}

TEST_CASE("CalculateWeight: low HP slowdown, overweight, bazaar") {
    WeightInput w;
    w.hp = 1000;
    w.max_hp = 1000;
    w.level = 50;
    w.strength = 100;
    w.carried = 50;
    CHECK(calculate_weight(w) == 1.0f);
    w.hp = 40; // 4% and below 50 HP
    CHECK(calculate_weight(w) == 0.5f);
    w.hp = 60; // below 20% but not below 50 HP; below 33% but not below 100 HP? 60 < 100: 0.75
    CHECK(calculate_weight(w) == 0.75f);
    w.hp = 300; // 30%, 300 HP: not below 100
    CHECK(calculate_weight(w) == 1.0f);
    w.max_hp = 200;
    w.hp = 60; // 30% and below 100
    CHECK(calculate_weight(w) == 0.75f);
    w.level = 3; // level 3 and below are exempt
    CHECK(calculate_weight(w) == 1.0f);
    w.low_level_exempt_off = true;
    CHECK(calculate_weight(w) == 0.75f);
    // Overweight: 1 - (150 - 100) / 200 = 0.75; with the slowdown 0.75 - 0.25.
    w = {};
    w.hp = w.max_hp = 100;
    w.level = 10;
    w.strength = 100;
    w.carried = 150;
    CHECK(calculate_weight(w) == 0.75f);
    w.carried = 400; // floored at 0
    CHECK(calculate_weight(w) == 0.0f);
    w.bazaar = true;
    CHECK(calculate_weight(w) == 1.0f);
    w.bazaar = false;
    w.barter = true;
    CHECK(calculate_weight(w) == 1.0f);
    w = {};
    w.carried = 5; // no capacity: no overweight term
    CHECK(calculate_weight(w) == 1.0f);

    std::int32_t shown = 0;
    CHECK(encumbrance_message(shown, 50, 100) == 0u);
    CHECK(encumbrance_message(shown, 150, 100) == kStrEncumbered);
    CHECK(encumbrance_message(shown, 160, 100) == 0u);
    CHECK(encumbrance_message(shown, 90, 100) == kStrNoLongerEncumbered);
    CHECK(shown == 0);
}

TEST_CASE("CalcCarriedWeight: tenths, bag reduction, SPA 221, clamp") {
    std::array<std::int32_t, 2> contents{100, 55}; // 10.0 and 5.5
    std::vector<CarriedItem> items;
    items.push_back({25, false, 0, {}});       // 2.5
    items.push_back({20, true, 50, contents}); // bag 2.0 + (10 + 5.5) * 50%
    CHECK(calc_carried_weight(items, 0) == 12); // 2.5 + 2 + 5 + 2.75 = 12.25
    CHECK(calc_carried_weight(items, 50) == 6);  // 12.25 - 6.125
    std::vector<CarriedItem> heavy{{400000, false, 0, {}}};
    CHECK(calc_carried_weight(heavy, 0) == 32766);
    CHECK(calc_carried_weight(items, 200) == 0);
}

TEST_CASE("Swimming, sneak and SetAccel") {
    CHECK(bits(static_cast<float>(swimming_speed(0.7f, 100))) ==
          bits(static_cast<float>(100 * 0.01) * 1.5f + 2.15f * 0.7f));
    CHECK(bits(static_cast<float>(swimming_speed(0.7f, 0))) == bits(2.15f * 0.7f));
    CHECK(sneak_speed(9, 50) == 0.5f);
    CHECK(bits(sneak_speed(9, 55)) == bits(5.0f * 0.1f + 0.5f));
    CHECK(bits(sneak_speed(9, 65)) == bits(10.0f * 0.1f + 0.5f));
    CHECK(sneak_speed(1, 65) == 0.5f);
    // No rider: at once.
    CHECK(set_accel(0.0f, 1.5f, false, std::nullopt, 1.0f, 0.7f) == 1.5f);
    // A mount with a player rider ramps by scale * 50 * run / 400 rising.
    float up = set_accel(0.0f, 1.5f, false, AccelRider{true, false}, 1.0f, 1.2f);
    CHECK(bits(up) == bits(1.0f * 50.0f * 1.2f / 400.0f));
    float down = set_accel(1.5f, 0.0f, false, AccelRider{true, false}, 1.0f, 1.2f);
    CHECK(bits(down) == bits(1.5f - 1.0f * 50.0f * 1.2f / 200.0f));
    CHECK(set_accel(1.4f, 1.5f, false, AccelRider{true, false}, 1.0f, 1.2f) == 1.5f); // stops at the target
    float slow = set_accel(0.0f, 1.5f, false, AccelRider{false, false}, 1.0f, 1.2f);
    CHECK(bits(slow) == bits(1.0f * 50.0f * 1.2f / 1500.0f));
    // In combat (the PcClient's virtual +0x80, the OP_RestState byte) the ramp is the slow one both ways.
    CHECK(bits(set_accel(0.0f, 1.5f, false, AccelRider{true, true}, 1.0f, 1.2f)) == bits(1.0f * 50.0f * 1.2f / 1500.0f));
    CHECK(bits(set_accel(1.5f, 0.0f, false, AccelRider{true, true}, 1.0f, 1.2f)) ==
          bits(1.5f - 1.0f * 50.0f * 1.2f / 1500.0f));
}

TEST_CASE("Local fly mode: server modes, buffs, no-levitation zones, rebuilds") {
    constexpr std::array<FlyMode, 6> all{FlyMode::Ground, FlyMode::Flying, FlyMode::Levitate,
                                         FlyMode::Water, FlyMode::Floating, FlyMode::LevitateRun};
    auto set = [](FlyMode m) { FlyModeEvents e; e.server_mode = m; return e; };
    FlyModeEvents recache;
    recache.recache = true;
    const FlyModeEvents none;

    // OP_SpawnAppearance 19 sets every mode outright, buffs or not, in a no-levitation zone too: the rules wait for
    // the next rebuild.
    for (FlyMode m : all) {
        CHECK(local_fly_mode(FlyMode::Ground, set(m), false, false, std::nullopt, false) == m);
        CHECK(local_fly_mode(FlyMode::Levitate, set(m), true, false, std::nullopt, true) == m);
    }
    // Without a rebuild nothing changes it: frames without events keep the server's mode against the buffs.
    CHECK(local_fly_mode(FlyMode::Water, none, true, false, std::nullopt, false) == FlyMode::Water);
    CHECK(local_fly_mode(FlyMode::Ground, none, true, false, std::nullopt, false) == FlyMode::Ground);

    // A rebuild (a buff changing) puts modes 0 and 2 to 5 back to what the buffs give.
    for (FlyMode m : {FlyMode::Ground, FlyMode::Levitate, FlyMode::Water, FlyMode::Floating, FlyMode::LevitateRun}) {
        CHECK(local_fly_mode(m, recache, false, false, std::nullopt, false) == FlyMode::Ground);
        CHECK(local_fly_mode(m, recache, true, false, std::nullopt, false) == FlyMode::Levitate);
        CHECK(local_fly_mode(m, recache, false, true, std::nullopt, false) == FlyMode::LevitateRun);
    }
    // #set flymode 0 while levitating: on the ground until the buffs next change, then levitating again.
    FlyMode mode = local_fly_mode(FlyMode::Levitate, set(FlyMode::Ground), true, false, std::nullopt, false);
    CHECK(mode == FlyMode::Ground);
    mode = local_fly_mode(mode, none, true, false, std::nullopt, false);
    CHECK(mode == FlyMode::Ground);
    mode = local_fly_mode(mode, recache, true, false, std::nullopt, false);
    CHECK(mode == FlyMode::Levitate);
    // #set flymode 3 without levitation, then an unrelated buff lands: back on the ground.
    mode = local_fly_mode(FlyMode::Ground, set(FlyMode::Water), false, false, std::nullopt, false);
    CHECK(mode == FlyMode::Water);
    mode = local_fly_mode(mode, recache, false, false, std::nullopt, false);
    CHECK(mode == FlyMode::Ground);

    // Mode 1 survives a rebuild, but not in a no-levitation zone (unless the spawn may fly anyway).
    CHECK(local_fly_mode(FlyMode::Flying, recache, false, false, std::nullopt, false) == FlyMode::Flying);
    CHECK(local_fly_mode(FlyMode::Flying, recache, true, false, std::nullopt, false) == FlyMode::Flying);
    CHECK(local_fly_mode(FlyMode::Flying, recache, false, false, std::nullopt, true) == FlyMode::Ground);
    CHECK(local_fly_mode(FlyMode::Flying, recache, false, false, std::nullopt, true, true) == FlyMode::Flying);
    // A levitate buff in a no-levitation zone gives nothing at a rebuild.
    CHECK(local_fly_mode(FlyMode::Levitate, recache, true, false, std::nullopt, true) == FlyMode::Ground);
    // A rider follows its mount's mode at a rebuild.
    CHECK(local_fly_mode(FlyMode::Ground, recache, true, false, FlyMode::Flying, false) == FlyMode::Flying);

    // A server mode and a rebuild in the same frame: the rebuild comes last.
    FlyModeEvents both = set(FlyMode::Water);
    both.recache = true;
    CHECK(local_fly_mode(FlyMode::Ground, both, false, false, std::nullopt, false) == FlyMode::Ground);
    both.server_mode = FlyMode::Flying;
    CHECK(local_fly_mode(FlyMode::Ground, both, false, false, std::nullopt, false) == FlyMode::Flying);
    CHECK(local_fly_mode(FlyMode::Ground, both, false, false, std::nullopt, true) == FlyMode::Ground);

    // A levitate buff landing (FUN_00432b20) replaces even mode 1: 2, or 5 for a non-zero Base2; 0 in a
    // no-levitation zone. The rebuild after it keeps that.
    FlyModeEvents landed;
    landed.levitate_landed = 0;
    landed.recache = true;
    CHECK(local_fly_mode(FlyMode::Flying, landed, true, false, std::nullopt, false) == FlyMode::Levitate);
    landed.levitate_landed = 1;
    CHECK(local_fly_mode(FlyMode::Flying, landed, false, true, std::nullopt, false) == FlyMode::LevitateRun);
    landed.recache = false;
    CHECK(local_fly_mode(FlyMode::Flying, landed, false, true, std::nullopt, true) == FlyMode::Ground);
    CHECK(local_fly_mode(FlyMode::Flying, landed, false, true, std::nullopt, true, true) == FlyMode::LevitateRun);

    // A levitate buff fading with no levitation left (RemoveAffect) grounds mode 1 too; with another levitation
    // left, mode 1 stays.
    FlyModeEvents faded;
    faded.levitate_faded = true;
    faded.recache = true;
    CHECK(local_fly_mode(FlyMode::Flying, faded, false, false, std::nullopt, false) == FlyMode::Ground);
    CHECK(local_fly_mode(FlyMode::Water, faded, false, false, std::nullopt, false) == FlyMode::Ground);
    CHECK(local_fly_mode(FlyMode::Flying, faded, true, false, std::nullopt, false) == FlyMode::Flying);
}

TEST_CASE("Buff levitation change between frames") {
    // Spell 10 levitates (Base2 0), 11 levitates while running (Base2 1), 20 does not.
    auto base2 = [](std::uint32_t id) -> std::optional<std::int32_t> {
        if (id == 10) return 0;
        if (id == 11) return 1;
        return std::nullopt;
    };
    constexpr std::uint32_t E = kNoBuffSpell;
    const std::vector<std::uint32_t> empty{E, E, E};

    BuffLevitationChange c = buff_levitation_change(empty, empty, base2);
    CHECK_FALSE(c.changed);
    CHECK_FALSE(c.faded);
    CHECK_FALSE(c.landed);

    // An unrelated buff: a change (a rebuild), no levitation event.
    c = buff_levitation_change(empty, std::vector<std::uint32_t>{E, 20, E}, base2);
    CHECK(c.changed);
    CHECK_FALSE(c.faded);
    CHECK_FALSE(c.landed);

    // Landing: the last slot's Base2.
    c = buff_levitation_change(empty, std::vector<std::uint32_t>{10, E, E}, base2);
    CHECK(c.changed);
    CHECK(c.landed == 0);
    c = buff_levitation_change(empty, std::vector<std::uint32_t>{10, E, 11}, base2);
    CHECK(c.landed == 1);

    // Fading, and a levitate buff replaced by another spell; a slot keeping its spell is no event.
    c = buff_levitation_change(std::vector<std::uint32_t>{10, 20, E}, std::vector<std::uint32_t>{E, 20, E}, base2);
    CHECK(c.changed);
    CHECK(c.faded);
    CHECK_FALSE(c.landed);
    c = buff_levitation_change(std::vector<std::uint32_t>{11, E, E}, std::vector<std::uint32_t>{20, E, E}, base2);
    CHECK(c.faded);
    c = buff_levitation_change(std::vector<std::uint32_t>{10, E, E}, std::vector<std::uint32_t>{10, 20, E}, base2);
    CHECK(c.changed);
    CHECK_FALSE(c.faded);
    CHECK_FALSE(c.landed);
    // The slot count differing (no slots yet before the profile): the new ones count as changed.
    c = buff_levitation_change(std::vector<std::uint32_t>{}, std::vector<std::uint32_t>{E, 10}, base2);
    CHECK(c.changed);
    CHECK(c.landed == 0);
}

TEST_CASE("Levitation mode, air supply, breathing") {
    CHECK(levitation_mode(FlyMode::Ground, true, false, std::nullopt, false) == FlyMode::Levitate);
    CHECK(levitation_mode(FlyMode::Ground, false, true, std::nullopt, false) == FlyMode::LevitateRun);
    CHECK(levitation_mode(FlyMode::Levitate, false, false, std::nullopt, false) == FlyMode::Ground);
    CHECK(levitation_mode(FlyMode::Flying, false, false, std::nullopt, false) == FlyMode::Flying);
    CHECK(levitation_mode(FlyMode::Ground, true, false, FlyMode::Flying, false) == FlyMode::Flying);
    CHECK(levitation_mode(FlyMode::Ground, true, false, std::nullopt, true) == FlyMode::Ground);
    CHECK(levitation_mode(FlyMode::Ground, true, false, std::nullopt, true, true) == FlyMode::Levitate);

    CHECK(max_air_supply(100, 0, 1) == 70);   // (100 - 30) * 100 / 100
    CHECK(max_air_supply(200, 0, 1) == 100);  // at most the percent
    CHECK(max_air_supply(200, 300, 1) == 300); // 510, capped at the percent
    CHECK(max_air_supply(35, 0, 1) == 10);    // at least 10
    CHECK(max_air_supply(100, 0, 128) == 127); // Iksar
    CHECK(max_air_supply(100, 0, 330) == 256); // Froglok
    // 45 * 100000000 wraps to 205032704 before the / 100, in every compiler's build (core/wrap_math.h).
    CHECK(max_air_supply(75, 100000000, 1) == 2050327);

    CHECK(can_breathe(0, false, 1, 0));
    CHECK_FALSE(can_breathe(5, false, 1, 0));
    CHECK(can_breathe(5, false, 1, 1));
    CHECK(can_breathe(5, false, 74, 0));
    CHECK_FALSE(can_breathe(0, true, 1, 0));
}

TEST_CASE("ApplyFallDamage: 10 f², heroic agility, Safe Fall, lethal at 9, mounts") {
    FallDamageInput in;
    in.fall_speed = 4.5f; // f = 3.0
    CHECK(apply_fall_damage(in).damage == 90);
    in.carried = 100; // + 1.0: f = 4
    CHECK(apply_fall_damage(in).damage == 160);
    in.heroic_agi_bonus = 1000; // - 160 * 1000 / 2000
    CHECK(apply_fall_damage(in).damage == 80);
    in.heroic_agi_bonus = 0;
    in.safe_fall_adjusted = 50;
    in.safe_fall_base = 40;
    in.spa228 = 50; // - 40 - 40 * 50 / 100
    FallDamage d = apply_fall_damage(in);
    CHECK(d.damage == 100);
    CHECK(d.safe_fall_used);
    in.safe_fall_base = 200; // never below 0
    CHECK(apply_fall_damage(in).damage == 0);
    in.safe_fall_base = 40;
    in.spa228 = 60000000; // 40 * 60000000 wraps to -1894967296: - 40 + 18949672
    CHECK(apply_fall_damage(in).damage == 18949792);
    in.spa228 = 50;
    in = {};
    in.fall_speed = 10.5f; // f = 9: lethal
    CHECK(apply_fall_damage(in).damage == kLethalFall);
    in.fall_speed = 1.5f; // f = 0: nothing
    CHECK(apply_fall_damage(in).damage == 0);
    in.fall_speed = 7.0f; // f = 5.5: 302.5 → 302
    CHECK(apply_fall_damage(in).damage == 302);
    in.mounted = true;
    CHECK(apply_fall_damage(in).damage == 302);
    in.fall_speed = 4.5f;
    CHECK(apply_fall_damage(in).damage == 0); // mounted, below 200
    in.mounted = false;
    in.no_fall_damage_zone = true;
    CHECK(apply_fall_damage(in).damage == 0);
}

TEST_CASE("Region damage: lava and freezing water") {
    // LocalPlayer_ApplyEnvironmentRegionDamage @0x459860: (body, zone, FR, CR, DR, lava damage, minimum).
    RegionDamage r = region_damage(7, 1, 30, 0, 0, 100, 20);
    CHECK(r.damage == 90);
    CHECK(r.type == kEnvDamageLava);
    CHECK(region_damage(3, 1, 30, 0, 0, 100, 20).damage == 90);
    CHECK(region_damage(7, 1, 300, 0, 0, 100, 20).damage == 20);
    CHECK(region_damage(7, 0x116, 30, 0, 0, 100, 20).damage == 94); // FR / 5
    // Zone 200: the disease resist (+0x4c8) and type 0xf4.
    r = region_damage(7, 200, 30, 0, 60, 100, 20);
    CHECK(r.damage == 80);
    CHECK(r.type == kEnvDamageFire);
    // A zone id above 1000 keeps its low 15 bits (0x100c8 & 0x7fff = 200); 1000 itself is not masked.
    CHECK(region_damage(7, 0x100c8, 30, 0, 60, 100, 20).damage == 80);
    CHECK(region_damage(7, 0x80c8 | 0x10000, 30, 0, 60, 100, 20).type == kEnvDamageFire);
    CHECK(region_damage(7, 1000, 30, 0, 60, 100, 20).damage == 90);
    // Truncating division of a negative resist: -4 / 3 = -1.
    CHECK(region_damage(7, 1, -4, 0, 0, 100, 20).damage == 101);
    // The server's lava damage is any 32-bit value; the sub wraps as the original's does (it overflowed, UB).
    CHECK(region_damage(7, 1, 60, 0, 0, INT32_MIN, 20).damage == 0x7fffffec);
    CHECK(region_damage(7, 1, -3, 0, 0, INT32_MAX, 20).damage == 20);
    r = region_damage(8, 1, 0, 30, 0, 0, 0);
    CHECK(r.damage == 40);
    CHECK(r.type == kEnvDamageFreezing);
    CHECK(region_damage(4, 1, 0, 30, 0, 0, 0).damage == 40);
    CHECK(region_damage(8, 1, 0, 300, 0, 0, 0).damage == 10);
    // Lava whose damage is below 1 after the minimum: nothing.
    CHECK(region_damage(7, 1, 300, 0, 0, 0, 0).damage == 0);
    CHECK(region_damage(5, 1, 0, 0, 0, 100, 100).damage == 0);
    CHECK(region_damage(6, 1, 0, 0, 0, 100, 100).damage == 0);
    CHECK(region_damage(0, 1, 0, 0, 0, 100, 100).damage == 0);
}

TEST_CASE("ProcessAirSupply: refill, count down, drown at 1 or less") {
    // PcClient::ProcessAirSupply @0x584ac0.
    AirSupply a = process_air_supply(3, true, 70, 1000);
    CHECK(a.air == 70);
    CHECK(a.drowning_damage == 0);
    a = process_air_supply(70, false, 70, 1000);
    CHECK(a.air == 69);
    a = process_air_supply(1, false, 70, 1000);
    CHECK(a.air == 0); // cmp 1 / jl: 1 still counts down
    CHECK(a.drowning_damage == 0);
    a = process_air_supply(0, false, 70, 1000);
    CHECK(a.air == 0);
    CHECK(a.drowning_damage == 125); // Max_HP x 0.125
    a = process_air_supply(-5, false, 70, 1003);
    CHECK(a.air == 0);               // a negative supply drowns too and becomes 0
    CHECK(a.drowning_damage == 125); // truncated
    CHECK(process_air_supply(0, false, 70, 7).drowning_damage == 0);
    CHECK(process_air_supply(0, false, 70, -100).drowning_damage == 0);
}

TEST_CASE("IsSwimSurfaceClear: the probe point and the rule") {
    // Heading 0: the table's cos 1, sin 0: one height ahead along +x (the original's y), z - 1.
    glm::vec3 p = swim_probe_point({10.0f, 20.0f, 5.0f}, 0.0f, 6.25f);
    CHECK(p.x == 16.25f);
    CHECK(p.y == 20.0f);
    CHECK(p.z == 4.0f);
    // Heading 128 (a quarter turn): along +y.
    p = swim_probe_point({10.0f, 20.0f, 5.0f}, 128.0f, 6.25f);
    CHECK(p.x == static_cast<float>(10.0f + static_cast<float>(table_cos(128.0f) * 6.25f)));
    CHECK(p.y == 26.25f);
    // The table index is the truncated heading: 64.9 reads entry 64.
    glm::vec3 a = swim_probe_point({0.0f, 0.0f, 0.0f}, 64.9f, 3.0f);
    glm::vec3 b = swim_probe_point({0.0f, 0.0f, 0.0f}, 64.0f, 3.0f);
    CHECK(bits(a.x) == bits(b.x));
    CHECK(bits(a.y) == bits(b.y));
    CHECK(bits(a.x) == bits(static_cast<float>(table_cos(64.0f) * 3.0f)));
    CHECK(swim_surface_clear(false, 6));
    CHECK(swim_surface_clear(true, 0));
    CHECK_FALSE(swim_surface_clear(true, 6));
    CHECK_FALSE(swim_surface_clear(true, 1));
}

TEST_CASE("Physics: heading wrap, feet on ground, coefficients") {
    CHECK(integrate_heading(500.0f, 12.0f, 1.0f) == 0.0f);
    CHECK(integrate_heading(10.0f, -12.0f, 1.0f) == 510.0f);
    CHECK(integrate_heading(100.0f, 0.0f, 1.0f) == 100.0f);
    CHECK(integrate_heading(4000.0f, 0.0f, 1.0f) == 0.0f); // more than 7 turns: 0

    PhysicsBody b;
    b.floor_height = 10.0f;
    b.feet.z = 10.5f;
    CHECK(feet_on_ground(b));
    b.feet.z = 10.6f;
    CHECK_FALSE(feet_on_ground(b));
    b.velocity.z = -0.2f;
    CHECK(feet_on_ground(b));

    Environment env;
    b = {};
    EnvCoefficients c = env_coefficients(b, env, 1.0f, 0.0f);
    CHECK(c == EnvCoefficients{0.8f, 0.0f, 0.625f, 0.99f});
    CHECK(b.velocity.z == -0.4f);
    b = {};
    b.feet.z = 20.0f; // in the air
    c = env_coefficients(b, env, 1.0f, 0.0f);
    CHECK(c == EnvCoefficients{0.0f, 0.0f, 0.99f, 0.99f});
    b = {};
    b.body_wet = 5; // water: no gravity
    c = env_coefficients(b, env, 1.0f, 0.0f);
    CHECK(c == EnvCoefficients{0.025f, 0.2f, 0.9f, 0.15f});
    CHECK(b.velocity.z == 0.0f);
    env.zone_id = 222;
    CHECK(env_coefficients(b, env, 1.0f, 0.0f) == EnvCoefficients{0.02f, 0.1f, 0.85f, 0.1f});
    env.zone_id = 0;
    b.body_wet = 7;
    CHECK(env_coefficients(b, env, 1.0f, 0.0f) == EnvCoefficients{0.02f, 0.8f, 0.94f, 0.15f});
    b.body_wet = 9;
    CHECK(env_coefficients(b, env, 1.0f, 0.0f) == EnvCoefficients{0.025f, 0.8f, 0.9f, 0.15f});
    b.sinks_in_water = true;
    env_coefficients(b, env, 1.0f, 0.0f);
    CHECK(b.velocity.z == -0.4f);
}

TEST_CASE("Physics: the run speed's steady state is SpeedRun * 0.8 / 0.375 per tick") {
    PhysicsBody b;
    b.speed_run = 0.7f;
    Environment env;
    for (int i = 0; i < 400; ++i) {
        game::mechanics::step(b, env, 16.0f, {}, 0.0f);
        b.feet.z = 0.0f; // stays on the floor
        b.velocity.z = 0.0f;
    }
    CHECK(b.velocity.x == doctest::Approx(0.7 * 0.8 / 0.375).epsilon(1e-4));
    CHECK(b.velocity.y == doctest::Approx(0.0).epsilon(1e-4));
    // Heading 128 faces +y.
    b = {};
    b.heading = 128.0f;
    b.speed_run = 1.0f;
    game::mechanics::step(b, env, 50.0f, {}, 0.0f);
    CHECK(b.velocity.y == doctest::Approx(0.8));
    CHECK(std::fabs(b.velocity.x) < 1e-6f);
}

TEST_CASE("Controller: run speed, weight, swimming, backwards") {
    MovementController m;
    m.reset({0, 0, 0}, 0.0f);
    CharacterInput ch;
    ch.run_speed = 0.7f;
    ch.walk_speed = 0.46f;
    ControlInput in;
    in.forward = true;
    m.controls(in, ch, 1000);
    CHECK(m.body.speed_run == 0.7f);
    ch.run_walk = 0;
    m.controls(in, ch, 1010);
    CHECK(m.body.speed_run == 0.46f);
    ch.run_walk = 1;
    ch.weight_factor = 0.5f;
    ch.speed_multiplier = 0.28f;
    m.controls(in, ch, 1020);
    CHECK(bits(m.body.speed_run) == bits(0.5f * 0.7f + 0.28f));
    ch.weight_factor = 1.0f;
    ch.speed_multiplier = -0.455f; // snared -65%
    m.controls(in, ch, 1030);
    CHECK(bits(m.body.speed_run) == bits(0.7f + -0.455f));
    ch.speed_multiplier = -2000.0f; // rooted
    m.controls(in, ch, 1040);
    CHECK(m.body.speed_run == 0.0f);
    ch.speed_multiplier = 0.0f;
    m.body.body_wet = 5;
    ch.swimming_skill = 100;
    m.controls(in, ch, 1050);
    CHECK(bits(m.body.speed_run) == bits(static_cast<float>(swimming_speed(0.7f, 100))));
    in = {};
    in.back = true;
    m.controls(in, ch, 1060);
    CHECK(m.body.speed_run == -0.46f); // water type 5: -walk, not weighted
    m.body.body_wet = 6;
    m.controls(in, ch, 1070);
    CHECK(m.body.speed_run == 0.0f); // other liquids: none
    m.body.body_wet = 0;
    ch.weight_factor = 0.5f;
    m.controls(in, ch, 1080);
    CHECK(bits(m.body.speed_run) == bits(0.5f * -0.46f));
    in = {};
    m.controls(in, ch, 1090);
    CHECK(m.body.speed_run == 0.0f);
}

TEST_CASE("Controller: turning ramps to 12, strafing") {
    MovementController m;
    m.reset({0, 0, 0}, 0.0f);
    CharacterInput ch;
    ControlInput in;
    in.turn_left = true;
    for (int i = 1; i <= 15; ++i) {
        m.controls(in, ch, static_cast<std::uint32_t>(i));
        CHECK(m.body.speed_heading == static_cast<float>(std::min(i, 12)));
    }
    in = {};
    m.controls(in, ch, 20);
    CHECK(m.body.speed_heading == 0.0f);
    in.turn_right = true;
    m.controls(in, ch, 21);
    CHECK(m.body.speed_heading == -1.0f);
    // Strafing still: run speed / 2 * 2 * weight to the left (heading + 128).
    in = {};
    in.strafe_left = true;
    m.controls(in, ch, 22);
    glm::vec2 s = m.strafe_push(ch);
    CHECK(s.x == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(s.y == doctest::Approx(0.7f * 0.5f * 2.0f));
    // Mouse look turns the turn keys into strafes.
    in = {};
    in.turn_right = true;
    in.mouse_look = true;
    m.controls(in, ch, 23);
    CHECK(m.strafe_mode() == 2);
    CHECK(m.body.speed_heading == 0.0f);
    // Moving forward while strafing: half the run speed, unscaled.
    in.forward = true;
    m.controls(in, ch, 24);
    s = m.strafe_push(ch);
    CHECK(s.y == doctest::Approx(-0.35));
}

TEST_CASE("Controller: the look keys ramp the pitch by the physics time factor") {
    MovementController m;
    m.reset({0, 0, 0}, 0.0f);
    CharacterInput ch;
    ControlInput in;
    // g_LocalPhysicsTimeFactor: the last step's elapsed ms * 0.02 (a 60 Hz step: 1/3).
    m.step(Environment{}, 50.0f, ch, 0.0f);
    m.body.camera_angle = 0.0f;
    m.command(kCmdPitchUp, true, true);
    float expect = 0.0f;
    for (int i = 1; i <= 14; ++i) {
        m.controls(in, ch, static_cast<std::uint32_t>(i));
        // The controlled and the local player are one spawn: 0x516cd0 adds the step twice.
        expect += 2.0f * static_cast<float>(std::min(i, 12));
        CHECK(m.body.camera_angle == doctest::Approx(std::min(expect, 128.0f)));
    }
    CHECK(m.body.camera_angle == 128.0f); // the clamp
    m.command(kCmdPitchUp, false, true);
    m.controls(in, ch, 20);
    CHECK(m.body.camera_angle == 128.0f);
    m.command(kCmdPitchDown, true, true);
    m.controls(in, ch, 21);
    CHECK(m.body.camera_angle == doctest::Approx(126.0f));
    m.command(kCmdPitchDown, false, true);
    // A shorter step looks slower.
    m.step(Environment{}, 25.0f, ch, 0.0f);
    m.body.camera_angle = 0.0f;
    m.command(kCmdPitchDown, true, true);
    m.controls(in, ch, 30);
    CHECK(m.body.camera_angle == doctest::Approx(-1.0f));
}

TEST_CASE("Controller: the camera keys move an active camera; zoom not below AvatarHeight nor past 400") {
    MovementController m;
    m.reset({0, 0, 0}, 0.0f);
    CharacterInput ch;
    ch.avatar_height = 6.0f;
    ControlInput in;
    m.step(Environment{}, 50.0f, ch, 0.0f); // the factor 1
    in.camera_index = 3;                     // a user camera
    in.camera_active = true;
    in.camera.distance = 10.0f;
    m.command(kCmdZoomIn, true, false);
    ControlEvents ev = m.controls(in, ch, 1);
    REQUIRE(ev.camera);
    CHECK(ev.camera->distance == 8.0f); // rate * 2
    CHECK_FALSE(ev.camera_pitch_set);
    in.camera.distance = 7.0f;
    ev = m.controls(in, ch, 2);
    CHECK(ev.camera->distance == 6.0f);
    m.command(kCmdZoomIn, false, false);
    m.command(kCmdZoomOut, true, false);
    in.camera.distance = 399.0f;
    ev = m.controls(in, ch, 3);
    CHECK(ev.camera->distance == 400.0f);
    // A camera whose virtual +0x2c is false (the overhead, tether and wheel-follow cameras) does not zoom.
    in.camera_active = false;
    ev = m.controls(in, ch, 4);
    CHECK_FALSE(ev.camera);
    m.command(kCmdZoomOut, false, false);
    // The camera turn keys: +0x38 by 12 x rate, FixHeading'd; active cameras only.
    in.camera_active = true;
    in.camera.heading = 5.0f;
    m.command(kCmdLeftCam, true, false);
    ev = m.controls(in, ch, 5);
    CHECK(ev.camera->heading == 505.0f);
    m.command(kCmdLeftCam, false, false);
    // The pan and pitch keys work in any camera but the first person one.
    in.camera_active = false;
    in.camera.pan = 2.0f;
    in.camera.pitch = 10.0f;
    m.command(kCmdForwardCam, true, false);
    m.command(kCmdPitchUpCam, true, false);
    ev = m.controls(in, ch, 6);
    REQUIRE(ev.camera);
    CHECK(ev.camera->pan == 3.0f);
    CHECK(ev.camera->pitch == 18.0f);
    CHECK(ev.camera_pitch_set);
    in.camera_index = 0;
    ev = m.controls(in, ch, 7);
    CHECK_FALSE(ev.camera);
}

TEST_CASE("Controller: held turn keys strafe during mouse look (the right button held)") {
    MovementController m;
    m.reset({0, 0, 0}, 0.0f);
    CharacterInput ch;
    ControlInput in;
    m.command(kCmdLeft, true, true);
    m.controls(in, ch, 1);
    CHECK(m.strafe_mode() == 0);
    CHECK(m.body.speed_heading == 1.0f);
    in.mouse_look = true;
    for (int camera : {0, 6}) {
        in.camera_index = camera;
        m.controls(in, ch, 2);
        CHECK(m.strafe_mode() == 1);
        CHECK(m.body.speed_heading == 0.0f);
    }
    m.command(kCmdLeft, false, true);
    m.command(kCmdRight, true, true);
    m.controls(in, ch, 3);
    CHECK(m.strafe_mode() == 2);
    in.mouse_look = false;
    m.controls(in, ch, 4);
    CHECK(m.strafe_mode() == 0);
    CHECK(m.body.speed_heading == -1.0f);
}

TEST_CASE("Controller: running jump, cooldown, endurance") {
    MovementController m;
    m.reset({0, 0, 0}, 0.0f);
    CharacterInput ch;
    ch.endurance = 100;
    ch.max_endurance = 1000;
    ControlInput in;
    in.forward = true;
    in.jump = true;
    ControlEvents ev = m.controls(in, ch, 10000);
    CHECK(ev.jumped);
    CHECK(ev.endurance_cost == 50);
    CHECK(bits(m.body.velocity.z) == bits(kJumpStrength * 1.65f * 1.0f + 0.0f));
    CHECK(m.body.velocity.x == doctest::Approx(1.0)); // PushAlongHeading(1)
    // Within 1.5 s: no jump.
    m.body.velocity = glm::vec3(0.0f);
    ev = m.controls(in, ch, 11400);
    CHECK_FALSE(ev.jumped);
    ev = m.controls(in, ch, 11501);
    CHECK(ev.jumped);
    // Too tired: endurance 50 or less and at most a tenth of the maximum.
    m.body.velocity = glm::vec3(0.0f);
    ch.endurance = 40;
    ch.max_endurance = 500;
    ev = m.controls(in, ch, 20000);
    CHECK_FALSE(ev.jumped);
    CHECK(ev.message == kStrTooFatiguedToJump);
    ch.max_endurance = 300; // 40 > 30
    ev = m.controls(in, ch, 20001);
    CHECK(ev.jumped);
    CHECK(ev.endurance_cost == 30);
}

TEST_CASE("Controller: a standing jump crouches, then jumps after 500 ms") {
    MovementController m;
    m.reset({0, 0, 0}, 0.0f);
    CharacterInput ch;
    ch.endurance = ch.max_endurance = 1000;
    ControlInput in;
    in.jump = true;
    ControlEvents ev = m.controls(in, ch, 10000);
    CHECK_FALSE(ev.jumped);
    CHECK(m.crouching);
    in.jump = false;
    ev = m.controls(in, ch, 10300);
    CHECK_FALSE(ev.jumped);
    ev = m.controls(in, ch, 10500);
    CHECK(ev.jumped);
    CHECK_FALSE(m.crouching);
    CHECK(m.body.velocity.x == 0.0f); // no push for a standing jump
}

namespace {

// DoMainLoop's jump queries, logging each call in order.
struct JumpLog final : JumpQueries {
    std::int32_t race_value = 1;
    bool mount = false;
    std::uint8_t stand = 100;
    bool refuse_change = false;
    std::uint32_t window = 500;
    bool on_ground = true;
    double weight = 1.0;
    std::int32_t max_end = 1000;
    std::vector<std::string> calls;
    std::int32_t race() override { calls.push_back("race"); return race_value; }
    bool mount_race() override { calls.push_back("mount_race"); return mount; }
    std::uint8_t stand_state() override { calls.push_back("stand"); return stand; }
    void change_position(std::uint8_t state) override {
        calls.push_back("change " + std::to_string(state));
        if (!refuse_change) stand = state;
    }
    std::uint32_t crouch_window() override { calls.push_back("window"); return window; }
    bool feet_on_ground() override { calls.push_back("feet"); return on_ground; }
    double calculate_weight() override { calls.push_back("weight"); return weight; }
    void push_along_heading(float a) override { calls.push_back("push " + std::to_string(a)); }
    void mount_jump() override { calls.push_back("mount_jump"); }
    void send_jump() override { calls.push_back("send_jump"); }
    std::int32_t max_endurance() override { calls.push_back("max_end"); return max_end; }
    void spend_endurance(std::int32_t c) override { calls.push_back("spend " + std::to_string(c)); }
};

} // namespace

TEST_CASE("DoMainLoop's jump: running, standing, in water, races, mounts, auto-duck") {
    using V = std::vector<std::string>;
    // A running jump within the window: at once, weighted, pushed, OP_Jump, min(50, max / 10).
    {
        JumpLog q;
        q.weight = 0.75;
        q.max_end = 300;
        JumpState g{1000, 0, 0, false, false};
        JumpSpawn s{0.25f, 1.6f, kJumpStrength, 0, false, true};
        main_loop_jump(g, s, q, 1200);
        CHECK(q.calls == V{"race", "race", "window", "feet", "stand", "weight", "push 1.000000", "send_jump",
                           "max_end", "max_end", "spend 30"});
        CHECK(g.jump_started == 0);
        CHECK(g.jump_ended == 1200);
        CHECK(g.local_dirty);
        CHECK(bits(s.speed_z) == bits(static_cast<float>(kJumpStrength * 1.65f) * 0.75f + 0.25f));
    }
    // Sneaking: no push; another controlled spawn: no weight, the other dirty flag; 50 at most.
    {
        JumpLog q;
        JumpState g{1000, 0, 0, false, false};
        JumpSpawn s{0.0f, 1.0f, 2.0f, 0, true, false};
        main_loop_jump(g, s, q, 1000);
        CHECK(q.calls == V{"race", "race", "window", "feet", "stand", "send_jump", "max_end", "spend 50"});
        CHECK(g.other_dirty);
        CHECK_FALSE(g.local_dirty);
        CHECK(bits(s.speed_z) == bits(2.0f * 1.65f));
    }
    // In the air: the running jump is dropped.
    {
        JumpLog q;
        q.on_ground = false;
        JumpState g{1000, 0, 0, false, false};
        JumpSpawn s{0.0f, 1.0f, kJumpStrength, 0, false, true};
        main_loop_jump(g, s, q, 1100);
        CHECK(q.calls == V{"race", "race", "window", "feet"});
        CHECK(g.jump_started == 0);
        CHECK(g.jump_ended == 0);
    }
    // Standing still: crouch ('o'), the jump stays pending; then at the window's end stand and jump unpushed.
    {
        JumpLog q;
        JumpState g{1000, 0, 0, false, false};
        JumpSpawn s{0.0f, 0.0f, kJumpStrength, 0, false, true};
        main_loop_jump(g, s, q, 1100);
        CHECK(q.calls == V{"race", "race", "window", "mount_race", "stand", "change 111", "stand"});
        CHECK(g.jump_started == 1000);
        CHECK(g.local_dirty);
        q.calls.clear();
        main_loop_jump(g, s, q, 1500); // 500 is not below the window
        CHECK(q.calls == V{"race", "race", "window", "mount_race", "stand", "change 100", "feet", "weight",
                           "send_jump", "max_end", "spend 50"});
        CHECK(g.jump_started == 0);
        CHECK(g.jump_ended == 1500);
        CHECK(bits(s.speed_z) == bits(static_cast<float>(kJumpStrength * 1.65f)));
    }
    // A spawn that cannot crouch (sitting): the jump is dropped.
    {
        JumpLog q;
        q.stand = 110;
        JumpState g{1000, 0, 0, false, false};
        JumpSpawn s{0.0f, 0.0f, kJumpStrength, 0, false, true};
        main_loop_jump(g, s, q, 1100);
        CHECK(q.calls == V{"race", "race", "window", "mount_race", "stand", "stand"});
        CHECK(g.jump_started == 0);
        CHECK(g.local_dirty);
    }
    // A NaN SpeedRun crouches like a still one; moving back drops the jump.
    {
        JumpLog q;
        JumpState g{1000, 0, 0, false, false};
        JumpSpawn s{0.0f, std::nanf(""), kJumpStrength, 0, false, true};
        main_loop_jump(g, s, q, 1100);
        CHECK(q.stand == 111);
        JumpLog q2;
        JumpState g2{1000, 0, 0, false, false};
        JumpSpawn s2{0.0f, -0.5f, kJumpStrength, 0, false, true};
        main_loop_jump(g2, s2, q2, 1100);
        CHECK(q2.calls == V{"race", "race", "window"});
        CHECK(g2.jump_started == 0);
        CHECK_FALSE(g2.local_dirty);
    }
    // In water: SpeedZ + 0.5 each frame of the window, the jump stays pending.
    {
        JumpLog q;
        JumpState g{1000, 0, 0, false, false};
        JumpSpawn s{1.0f, 1.0f, kJumpStrength, 6, false, true};
        main_loop_jump(g, s, q, 1100);
        CHECK(s.speed_z == 1.5f);
        CHECK(g.jump_started == 1000);
    }
    // A shorter crouch window (the crouch animation's length + 1).
    {
        JumpLog q;
        q.window = 300;
        JumpState g{1000, 0, 0, false, false};
        JumpSpawn s{0.0f, 0.0f, kJumpStrength, 0, false, true};
        q.stand = 111;
        main_loop_jump(g, s, q, 1300);
        CHECK(g.jump_ended == 1300);
    }
    // Races 0x8e / 0x8f never jump (and the auto-duck is not looked at).
    {
        JumpLog q;
        q.race_value = 0x8f;
        JumpState g{1000, 0, 1, false, false};
        JumpSpawn s{0.0f, 1.0f, kJumpStrength, 0, false, true};
        main_loop_jump(g, s, q, 1100);
        CHECK(q.calls == V{"race"});
        CHECK(g.jump_started == 0);
        CHECK(g.auto_duck == 1);
        JumpLog q2;
        q2.race_value = 0x8e;
        JumpState g2{1000, 0, 0, false, false};
        main_loop_jump(g2, s, q2, 1100);
        CHECK(q2.calls == V{"race", "race"});
    }
    // A mount race: no crouch; at the window's end its own jump.
    {
        JumpLog q;
        q.mount = true;
        JumpState g{1000, 0, 0, false, false};
        JumpSpawn s{0.0f, 0.0f, kJumpStrength, 0, false, true};
        main_loop_jump(g, s, q, 1600);
        CHECK(q.calls == V{"race", "race", "window", "mount_race", "mount_jump"});
        CHECK(g.jump_started == 0);
    }
    // The auto-duck stops a jump and crouches; a spawn that stays up clears it.
    {
        JumpLog q;
        JumpState g{1000, 0, 1, false, false};
        JumpSpawn s{0.0f, 1.0f, kJumpStrength, 0, false, true};
        main_loop_jump(g, s, q, 1100);
        CHECK(q.calls == V{"race", "race", "mount_race", "stand", "change 111", "stand"});
        CHECK(g.jump_started == 0);
        CHECK(g.auto_duck == 1);
        JumpLog q2;
        q2.refuse_change = true;
        JumpState g2{0, 0, 1, false, false};
        main_loop_jump(g2, s, q2, 1100);
        CHECK(g2.auto_duck == 0);
    }
    // A start after now (the clock went back) is dropped.
    {
        JumpLog q;
        JumpState g{2000, 0, 0, false, false};
        JumpSpawn s{0.0f, 1.0f, kJumpStrength, 0, false, true};
        main_loop_jump(g, s, q, 1100);
        CHECK(g.jump_started == 0);
        CHECK(q.calls == V{"race", "race"});
    }
}

TEST_CASE("Controller: landing, the first fall after zone-in, fall damage") {
    MovementController m;
    m.reset({0, 0, 100}, 0.0f);
    m.body.velocity.z = -7.0f;
    m.body.feet.z = -0.5f;
    LandingEvents ev = m.land(0.0f, {});
    CHECK(ev.landed);
    CHECK(ev.fell);
    CHECK(ev.damage.damage == 0); // the first fall only sets the flag
    CHECK(m.body.feet.z == 0.0f);
    CHECK(m.body.velocity.z == 0.0f);
    m.body.velocity.z = -7.0f; // |-7 + 2.5| = 4.5: f = 3
    m.body.feet.z = -0.5f;
    ev = m.land(0.0f, {});
    CHECK(ev.damage.damage == 90);
    m.body.velocity.z = -2.0f; // above -2.5: no fall
    m.body.feet.z = -0.5f;
    ev = m.land(0.0f, {});
    CHECK(ev.landed);
    CHECK_FALSE(ev.fell);
    m.body.velocity.z = -7.0f;
    m.body.feet.z = 0.5f; // above the floor: no landing
    CHECK_FALSE(m.land(0.0f, {}).landed);
}

TEST_CASE("Controller: swim jump cooldown") {
    MovementController m;
    m.reset({0, 0, 0}, 0.0f);
    CHECK(m.swim_jump(true, true, 1000)); // NextSwim starts at 0
    CHECK(m.body.velocity.z == kSwimStrength * 3.0f);
    CHECK_FALSE(m.swim_jump(true, true, 2000));
    CHECK_FALSE(m.swim_jump(true, true, 2500)); // not before now + 1500
    CHECK_FALSE(m.swim_jump(true, false, 2501)); // the timer restarts even without a clear surface
    CHECK_FALSE(m.swim_jump(true, true, 4000));
    CHECK(m.swim_jump(true, true, 4002));
}

TEST_CASE("Movement packets: OP_EnvDamage (39 bytes), OP_Jump, OP_SetRunMode") {
    net::AppPacket p = net::encode(net::EnvDamage{0x12345, 302, 0xfc});
    CHECK(p.opcode == 0x51fd);
    REQUIRE(p.data.size() == 39);
    auto u8 = [&](std::size_t i) { return std::to_integer<int>(p.data[i]); };
    CHECK(u8(0) == 0x45);
    CHECK(u8(1) == 0x23);
    CHECK(u8(2) == 0);
    CHECK(u8(6) == 302 % 256);
    CHECK(u8(7) == 1);
    CHECK(u8(13) == 0x3f); // 1.0f
    CHECK(u8(26) == 0xfc);
    CHECK(u8(33) == 0xff);
    CHECK(u8(36) == 0xff);
    CHECK(u8(37) == 0xff);
    CHECK(u8(38) == 0);
    auto d = net::decode_env_damage(p);
    REQUIRE(d);
    CHECK(d->spawn_id == 0x2345);
    CHECK(d->damage == 302u);
    CHECK(d->type == 0xfc);
    CHECK(net::encode_jump().opcode == 0x31f4);
    CHECK(net::encode_jump().data.empty());
    CHECK(net::encode_safe_fall_success().opcode == 0x2219);
    net::AppPacket r = net::encode_set_run_mode(true);
    CHECK(r.opcode == 0x009f);
    CHECK(r.data == std::vector<std::byte>{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}});
}

TEST_CASE("Controller: LookSpring, AllowAutoStand's forward key and AllowAutoDuck's crouch") {
    MovementController m;
    m.reset({0, 0, 0}, 0.0f);
    CharacterInput ch;
    ControlInput in;
    // LookSpring (0xde0bec): out of mouse look in first person the pitch eases back by 10 a frame.
    m.body.camera_angle = 25.0f;
    in.look_spring = true;
    m.controls(in, ch, 1000);
    CHECK(m.body.camera_angle == 15.0f);
    in.camera_index = 6; // not in the wheel-follow camera
    m.controls(in, ch, 1010);
    CHECK(m.body.camera_angle == 15.0f);
    in.camera_index = 0;
    in.mouse_look = true; // nor during mouse look
    m.controls(in, ch, 1020);
    CHECK(m.body.camera_angle == 15.0f);
    in.mouse_look = false;
    in.look_spring = false;
    m.controls(in, ch, 1030);
    CHECK(m.body.camera_angle == 15.0f);

    // AllowAutoStand (0xde0d5b): the forward key while sitting runs CEverQuest::Sit, while looting closes the loot
    // window; without the option nothing.
    in.forward = true;
    ch.sitting = true;
    ch.frozen = true;
    ControlEvents ev = m.controls(in, ch, 1040);
    CHECK_FALSE(ev.sit_toggle);
    in.forward_sits = true;
    ev = m.controls(in, ch, 1050);
    CHECK(ev.sit_toggle);
    CHECK(m.body.speed_run == 0.0f);
    ch.sitting = false;
    ch.frozen = false;
    ch.looting = true;
    in.loot_window_open = true;
    ev = m.controls(in, ch, 1060);
    CHECK_FALSE(ev.sit_toggle);
    CHECK(ev.close_loot);
    ch.looting = false;
    in.forward = false;

    // CanIFitHere's auto-duck (byte 0xddf675) crouches the player (DoMainLoop's unk_SetStandState 0x6f).
    in.auto_duck = true;
    ev = m.controls(in, ch, 1070);
    CHECK(ev.crouch_changed);
    CHECK(m.crouching);
    ev = m.controls(in, ch, 1080);
    CHECK_FALSE(ev.crouch_changed);
}

TEST_CASE("Controller: feigning death (StandState 115) holds the player still; the forward key does not stand it") {
    MovementController m;
    m.reset({0, 0, 0}, 0.0f);
    CharacterInput ch;
    ch.endurance = ch.max_endurance = 1000;
    ch.lying = true;
    ControlInput in;
    in.forward = true;
    in.forward_sits = true; // AllowAutoStand stands a sitting player only
    in.turn_left = true;
    in.strafe_right = true;
    in.jump = true;
    ControlEvents ev = m.controls(in, ch, 10000);
    CHECK(m.stand_state() == 115);
    CHECK(m.body.speed_run == 0.0f);
    CHECK(m.body.speed_heading == 0.0f);
    CHECK(m.strafe_mode() == 0);
    CHECK_FALSE(ev.jumped);
    CHECK_FALSE(ev.sit_toggle);
    CHECK_FALSE(m.crouching);
    in = {};
    in.back = true;
    m.controls(in, ch, 10010);
    CHECK(m.body.speed_run == 0.0f);
    // Stood up (/sit or /stand), the same keys move it.
    ch.lying = false;
    in = {};
    in.forward = true;
    m.controls(in, ch, 10020);
    CHECK(m.stand_state() == 100);
    CHECK(m.body.speed_run == 0.7f);
}

TEST_CASE("the jump's crouch window: the duck track's length + 1, at most 500 (0x406740)") {
    CHECK(jump_crouch_window(std::nullopt) == 500);
    CHECK(jump_duck_clip(false) == "L08");
    CHECK(jump_duck_clip(true) == "CRCH_BA_1");
    CHECK(jump_crouch_window(0.0) == 1);
    CHECK(jump_crouch_window(300.0) == 301);
    CHECK(jump_crouch_window(300.9) == 301); // truncated (chop) before the 1 is added
    CHECK(jump_crouch_window(499.0) == 500);
    CHECK(jump_crouch_window(500.0) == 500); // 501 is above 500
    CHECK(jump_crouch_window(1600.0) == 500);
    // The controller's jump asks the character's window: a standing jump crouches, then jumps when it ends.
    MovementController m;
    m.reset({0, 0, 0}, 0.0f);
    CharacterInput ch;
    ch.crouch_window = 201;
    ch.endurance = ch.max_endurance = 1000;
    ControlInput in;
    m.command(kCmdJump, true, true);
    ControlEvents ev = m.controls(in, ch, 2000);
    CHECK_FALSE(ev.jumped);
    CHECK(m.crouching);
    ev = m.controls(in, ch, 2200);
    CHECK_FALSE(ev.jumped);
    ev = m.controls(in, ch, 2300);
    CHECK(ev.jumped);
}
