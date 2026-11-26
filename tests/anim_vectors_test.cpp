// The movement animation choice against the original's vectors
// (recorded from eqgame.exe): SelectMovementAnim.jsonl
// (PlayerAnimation_SelectMovementAnim @0x5888a0, 3000 records at x87
// precision 24 and 53) and OldModelAnimSpeed.jsonl (0x4afc90, 2000).
// Floats must match bit for bit; the callees asked must be the ones the
// original asked.
#include <doctest/doctest.h>

#include <bit>
#include <chrono>
#include <initializer_list>
#include <map>
#include <string>

#include "assets/character.h"
#include "game/anim_layers.h"
#include "game/mechanics/anim_select.h"
#include "game/world_state.h"
#include "game_files.h"
#include "manual_clock.h"
#include "vector_files.h"

using namespace hailstone::game::mechanics;
namespace Race = hailstone::game::Race;
using hailstone::test::Json;
using hailstone::test::f32_bits;
using hailstone::test::f32_of;

namespace {

Fpu fpu_of(const Json& r) {
    return Fpu(r["precision"].text == "x87-24" ? FpuPrecision::Single24 : FpuPrecision::Double53);
}

std::string hex(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

} // namespace

TEST_CASE("SelectMovementAnim vectors") {
    hailstone::test::each_vector("SelectMovementAnim.jsonl", [](const Json& r) -> std::string {
        const Json& in = r["in"];
        const Json& out = r["out"];
        MovementAnimInput m;
        m.mounted = in["mounted"].truthy();
        m.speed_y = f32_of(in["velocity_y"]);
        m.speed_x = f32_of(in["velocity_x"]);
        m.speed_z = f32_of(in["velocity_z"]);
        m.speed_run = f32_of(in["speed_run"]);
        m.colliding_type = in["colliding_type"].i32();
        m.feet_wet = static_cast<std::uint8_t>(in["feet_wet"].i64());
        m.body_wet = static_cast<std::uint8_t>(in["body_wet"].i64());
        m.spawn_type = static_cast<std::uint8_t>(in["spawn_type"].i64());
        m.stand_state = static_cast<std::uint8_t>(in["stand_state"].i64());
        m.animation = in["animation"].i32();
        m.jump_strength = f32_of(in["jump_strength"]);
        m.walk_speed = f32_of(in["walk_speed"]);
        m.run_speed = f32_of(in["run_speed"]);
        m.model_scale = f32_of(in["model_scale"]);
        m.anim_scale = f32_of(in["anim_scale"]);
        m.local_player = in["is_local_player"].truthy();
        m.old_model_scale = in["old_model_scale"].truthy();
        // The callees' answers, and which were asked.
        int mounts = 0;
        bool asked_book = false, asked_mount_race = false, asked_new_style = false;
        for (const Json& c : in["calls"].items) {
            const std::string& fn = c["fn"].text;
            if (fn == "get_mount") {
                ++mounts;
            } else if (fn == "spell_book_is_active") {
                asked_book = true;
                m.spell_book_active = c["ret"].truthy();
            } else if (fn == "is_mount_race") {
                asked_mount_race = true;
                m.mount_race = c["ret"].truthy();
            } else if (fn == "is_new_style_model_race") {
                asked_new_style = true;
                m.new_style_race = c["ret"].truthy();
                if (c["race"].i64() != in["race"].i64()) return "is_new_style_model_race asked another race";
            }
        }
        if (mounts != (m.mounted ? 2 : 1)) return "GetMount asked " + std::to_string(mounts) + " times";
        MovementAnimResult got = select_movement_anim(m, fpu_of(r));
        if (got.asked_spell_book != asked_book) return "spell book asked differs";
        if (got.asked_mount_race != asked_mount_race) return "IsMountRace asked differs";
        if (got.asked_new_style_race != asked_new_style) return "IsNewStyleModelRace asked differs";
        if (got.spell_book_slot_0x94 != out["spell_book_slot_0x94"].truthy()) return "spell book slot 0x94 differs";
        if (out["still_since"].i64() != 0) return "still_since not cleared in the vector";
        if (got.start.played != out["played"].truthy()) return "played differs";
        if (!got.start.played) return "";
        if (got.start.anim != out["anim"].i64())
            return "anim " + std::to_string(got.start.anim) + " want " + std::to_string(out["anim"].i64());
        if (got.start.play_once != out["play_once"].truthy()) return "play_once differs";
        if (got.start.reverse != out["reverse"].truthy()) return "reverse differs";
        if (got.start.replay != out["replay"].truthy()) return "replay differs";
        std::uint32_t speed = std::bit_cast<std::uint32_t>(got.start.speed);
        if (speed != f32_bits(out["speed"])) return "speed " + hex(speed) + " want " + hex(f32_bits(out["speed"]));
        return "";
    });
}

TEST_CASE("OldModelAnimSpeed vectors") {
    hailstone::test::each_vector("OldModelAnimSpeed.jsonl", [](const Json& r) -> std::string {
        const Json& in = r["in"];
        double got = old_model_anim_speed(in["anim"].i32(), f32_of(in["speed"]), in["race"].i32(),
                                          in["gender"].i32(), in["new_model"].truthy(), in["display_129"].truthy(),
                                          fpu_of(r));
        double want = r["out"]["value"].f64();
        if (std::bit_cast<std::uint64_t>(got) != std::bit_cast<std::uint64_t>(want))
            return "value " + std::to_string(got) + " want " + std::to_string(want);
        return "";
    });
}

TEST_CASE("Animation tables") {
    CHECK(new_style_tag(kAnimWalk) == "WALK");
    CHECK(new_to_old_anim(kAnimRun) == static_cast<std::uint8_t>(hailstone::AnimationType::Run));
    CHECK(old_anim_clip(new_to_old_anim(kAnimSwim)) == "P06");
    CHECK(old_anim_clip(new_to_old_anim(kAnimSit)) == "P07");
    CHECK(new_style_blend_ms(kAnimWalk, kAnimStand) == 175.0f);
    CHECK(new_style_blend_ms(kAnimStand, kAnimWalk) == 100.0f);
    // The names table (0xb787a0) and the two maps (0xaaf428, 0xaaf5c8).
    CHECK(old_anim_clip(0x47) == "P08");
    // The animation object's vf24 (0x4afd90 / 0x588cf0): a jump.
    CHECK(is_jump_animation(0x13, false));
    CHECK(is_jump_animation(0x14, false));
    CHECK_FALSE(is_jump_animation(0x7b, false));
    CHECK(is_jump_animation(0x7b, true));
    CHECK(is_jump_animation(0x7c, true));
    CHECK_FALSE(is_jump_animation(0x13, true));
    CHECK_FALSE(is_jump_animation(0x20, false));
    CHECK(old_anim_clip(0x4a) == "P09");
    CHECK(old_anim_clip(0x4e) == "L10");
    CHECK(old_anim_clip(0x4d) == "S29");
    CHECK(old_anim_clip(0x4f).empty());
    CHECK(new_style_tag(0x50) == "SLPR");
    CHECK(new_style_tag(0xb6) == "SATK");
    CHECK(new_style_tag(0x4f).empty());
    CHECK(new_to_old_anim(0x51) == 5);    // BLPR -> C05
    CHECK(new_to_old_anim(0x5c) == 8);    // PATK -> C08
    CHECK(new_to_old_anim(0x98) == 0x46); // NBOW -> S28
    CHECK(old_to_new_anim(5) == 0x50);    // C05 -> SLPR
    CHECK(old_to_new_anim(6) == 0x53);    // C06 -> SLSC
    CHECK(old_to_new_anim(0x1a) == 0x6f); // O01 -> IDLE
    CHECK(old_to_new_anim(0x48) == 0x6f); // O02 -> IDLE
    CHECK(old_to_new_anim(0x49) == 0x77); // O03 -> SIDL
    CHECK(old_to_new_anim(0x23) == 0x4f); // P04: none
    CHECK(old_to_new_anim(0x6e) == 0x6e);
    CHECK(new_style_fallback(0x54) == 0x53); // BLSC -> SLSC
    CHECK(new_style_fallback(0x53) == 0x50); // SLSC -> SLPR
    CHECK(new_style_fallback(0x50) == 0x6e); // SLPR -> STND
    CHECK(new_style_fallback(0x6e) == 0x4f);
    CHECK(new_style_attack_flag(0x50));
    CHECK(new_style_attack_flag(0x70)); // STDA
    CHECK_FALSE(new_style_attack_flag(0x6e));
    CHECK(new_style_layer_kind(0x50) == 1);
    CHECK(new_style_layer_kind(0x68) == 2);
}

TEST_CASE("PlayAttackAnimation: old-style models (0x59f750)") {
    auto old = [](std::uint32_t skill, std::uint32_t type, std::uint8_t slot, std::uint32_t race = 1,
                  std::uint8_t cls = 1, std::int32_t range = 0) {
        AttackAnimInput in;
        in.skill = skill;
        in.item_type = type;
        in.slot = slot;
        in.race = race;
        in.class_id = cls;
        in.range = range;
        return attack_animation(in);
    };
    CHECK(old(1, 0, 13) == 5);     // 1H slashing
    CHECK(old(0, 3, 13) == 5);     // 1H blunt
    CHECK(old(3, 1, 13) == 3);     // 2H slashing
    CHECK(old(2, 4, 13) == 3);     // 2H blunt
    CHECK(old(0x24, 2, 13) == 2);  // 1H piercing
    CHECK(old(0x24, 0x23, 13) == 4); // 2H piercing
    CHECK(old(0x1c, 0x2d, 13) == 8); // hand to hand
    CHECK(old(1, 0, 14) == 6);     // the secondary hand
    CHECK(old(0x1c, 0x2d, 14) == 6);
    CHECK(old(0x1e, 0, 7) == 1);   // kick
    CHECK(old(0x26, 0, 7) == 0xb); // round kick
    CHECK(old(0x1a, 0, 7) == 0x2d); // flying kick
    CHECK(old(0x17, 0, 7) == 0x2f); // eagle strike
    CHECK(old(0x15, 0, 7) == 0x2e); // dragon punch
    CHECK(old(0x34, 0, 7) == 0x2e); // tiger claw
    CHECK(old(0xa, 0, 7) == 7);     // bash
    CHECK(old(0xa, 0, 7, 0x80) == 6); // an Iksar's bash
    CHECK(old(0x15, 0, 7, 0x80) == 7); // an Iksar's dragon punch
    CHECK(old(0x47, 0, 7) == 1);     // intimidation
    CHECK(old(0x47, 0, 14, 1, 7) == 0xb); // a monk's: a round kick, even in the secondary hand
    CHECK(old(0x4a, 0, 13) == 5);    // frenzy
    CHECK(old(8, 2, 13) == 2);       // backstab
    CHECK(old(7, 0x1b, 7, 1, 1, 7) == 9); // an arrow
    CHECK(old(0, 9, 7) == 0x2c);
}

TEST_CASE("PlayAttackAnimation: new-style models (0x59f8d0)") {
    auto nu = [](std::uint32_t skill, std::uint32_t type, std::uint8_t slot, std::uint32_t race = 522,
                 std::uint8_t cls = 1, std::uint8_t wet = 0) {
        AttackAnimInput in;
        in.skill = skill;
        in.item_type = type;
        in.slot = slot;
        in.race = race;
        in.class_id = cls;
        in.body_wet = wet;
        in.new_style = true;
        return attack_animation(in);
    };
    CHECK(nu(1, 0, 13) == 0x50);    // SLPR
    CHECK(nu(0, 3, 13) == 0x51);    // BLPR
    CHECK(nu(1, 0, 14) == 0x53);    // SLSC
    CHECK(nu(0, 3, 14) == 0x53);    // BLSC, then the secondary hand's SLSC
    CHECK(nu(0, 3, 7) == 0x54);     // BLSC
    CHECK(nu(3, 1, 13) == 0x56);    // SL2H
    CHECK(nu(0x24, 2, 13) == 0x52); // STPR
    CHECK(nu(0x24, 0x23, 13) == 0x58); // ST2H
    CHECK(nu(0x1c, 0x2d, 13) == 0x5c); // PATK
    CHECK(nu(0x1c, 0x2d, 14) == 0x53); // SATK, then SLSC
    CHECK(nu(0x1c, 0x2d, 7) == 0xb6);  // SATK
    CHECK(nu(0x1e, 0, 7) == 0x5f);  // KICK
    CHECK(nu(0x26, 0, 7) == 0x60);  // RKCK
    CHECK(nu(0xa, 0, 7) == 0x5b);   // BASH
    CHECK(nu(0xa, 0, 7, 0x80) == 0x53);
    CHECK(nu(0x15, 0, 7, 0x80) == 0x5b);
    CHECK(nu(0x47, 0, 14, 522, 7) == 0x60);
    CHECK(nu(1, 0, 13, 522, 1, 5) == 0x5e); // in water: WATK
}

TEST_CASE("ChooseIdleAnimation") {
    IdleAnimInput in;
    in.display_time = 10000;
    in.still_since = 9000;
    in.animation = 0x6e;
    int roll = 50;
    in.random_percent = [&] { return roll; };
    auto r = choose_idle_animation(in);
    CHECK(r.start.played);
    CHECK(r.start.anim == kAnimStand);
    CHECK_FALSE(r.start.play_once);
    CHECK_FALSE(r.set_still_since);
    // 5 s on: a roll; above 75 the fidget.
    in.display_time = 14001;
    roll = 76;
    r = choose_idle_animation(in);
    CHECK(r.set_still_since);
    CHECK(r.start.anim == kAnimIdle);
    CHECK(r.start.play_once);
    roll = 75;
    CHECK(choose_idle_animation(in).start.anim == kAnimStand);
    in.attacking = true; // no fidget while attacking: nothing new
    roll = 90;
    CHECK_FALSE(choose_idle_animation(in).start.played);
    in.attacking = false;
    in.owner_38c = 5; // a pet: above 99
    CHECK(choose_idle_animation(in).start.anim == kAnimStand);
    in.owner_38c = 0;
    // Sitting: NSIT / SIDL; not in first person.
    in.stand_state = 0x6e;
    in.animation = 0x76;
    CHECK(choose_idle_animation(in).start.anim == kAnimSitIdle);
    in.local_first_person = true;
    CHECK_FALSE(choose_idle_animation(in).start.played);
    in.local_first_person = false;
    in.stand_state = 100;
    // Turning.
    in.display_time = 10000;
    in.turning = true;
    in.speed_heading = 2.0f;
    CHECK(choose_idle_animation(in).start.anim == 0x6c);
    in.speed_heading = -2.0f;
    CHECK(choose_idle_animation(in).start.anim == 0x6b);
    in.stand_state = 0x6f;
    r = choose_idle_animation(in);
    CHECK(r.start.anim == kAnimDuckWalk);
    in.turning = false;
    r = choose_idle_animation(in);
    CHECK(r.start.anim == 0x7a); // CRCH at rate 2
    CHECK(r.start.speed == 2.0f);
    in.stand_state = 100;
    in.body_wet = 5;
    CHECK(choose_idle_animation(in).start.anim == kAnimTread);
    in.body_wet = 0;
    in.player_state = 0x20;
    CHECK(choose_idle_animation(in).start.anim == 0x81); // STUN
}

// ---- the per-frame choice (game/spawn_animation.h)

#include "game/spawn_animation.h"

namespace {

struct Model {
    explicit Model(std::initializer_list<const char*> codes) {
        for (const char* code : codes) clips[code].frame_count = 2;
    }

    std::map<std::string, hailstone::assets::AnimationClip, std::less<>> clips;
    const hailstone::assets::AnimationClip* clip(std::string_view code) const {
        auto it = clips.find(code);
        return it == clips.end() ? nullptr : &it->second;
    }
    bool has(std::string_view code) const { return clip(code) != nullptr; }
};

hailstone::game::SpawnAnimChoice frame(hailstone::game::SpawnAnimState& st, const hailstone::game::SpawnAnimInput& in, const Model& m,
                                  bool done = false, int roll = 50) {
    return hailstone::game::choose_spawn_animation(
        st, in, [&](std::string_view c) { return m.clip(c); }, done, [roll] { return roll; });
}

} // namespace

TEST_CASE("Classic models: walk, run, backward, still") {
    Model m{{"P01", "O01", "L01", "L02", "P02", "P07", "O03", "L03", "L04", "L05"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.race = 1;
    in.walk_speed = 0.46f;
    in.now_ms = 1000;
    // Walking: L01 at 0.5 * |v_h| + 1 (0x4afe60), unchanged by 0x4afc90 for L01.
    in.velocity = {0.3f, 0.4f, 0.0f};
    in.speed_run = 0.4f;
    auto c = frame(st, in, m);
    CHECK(c.clip == "L01");
    CHECK(c.rate == doctest::Approx(1.25f));
    CHECK(c.loop);
    CHECK_FALSE(c.reverse);
    // Running: the walk speed below SpeedRun.
    in.speed_run = 0.7f;
    c = frame(st, in, m);
    CHECK(c.clip == "L02");
    CHECK(c.restart);
    // Backward: reversed.
    in.speed_run = -0.4f;
    c = frame(st, in, m);
    CHECK(c.clip == "L01");
    CHECK(c.reverse);
    // Stopping: P01 (the display clock is not yet 5 s past +0x360, cleared while moving).
    in.velocity = {0, 0, 0};
    in.speed_run = 0;
    in.now_ms = 2000;
    c = frame(st, in, m);
    CHECK(c.clip == "P01");
    // After 5 s standing, a roll above 75 starts the idle O01 once.
    in.now_ms = 7100;
    c = frame(st, in, m, false, 90);
    CHECK(c.clip == "O01");
    CHECK_FALSE(c.loop);
}

TEST_CASE("Classic movement falls back from a one-frame walk or run") {
    Model m{{"P01", "L01", "L02"}};
    m.clips.at("P01").frame_count = 1;
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.npc = true;
    in.race = Race::Gnoll;
    in.walk_speed = 0.46f;
    in.now_ms = 1000;
    CHECK(frame(st, in, m).clip == "P01");

    in.velocity = {0.3f, 0.4f, 0.0f};
    in.speed_run = 0.4f;
    m.clips.at("L01").frame_count = 1;
    auto walk = frame(st, in, m);
    CHECK(walk.clip == "L02");
    CHECK(walk.rate == doctest::Approx(1.25f));
    CHECK(walk.loop);
    CHECK_FALSE(walk.reverse);

    in.speed_run = 0.7f;
    auto run = frame(st, in, m);
    CHECK(run.clip == "L02");
    CHECK_FALSE(run.restart);

    m.clips.at("L01").frame_count = 2;
    m.clips.at("L02").frame_count = 1;
    CHECK(frame(st, in, m).clip == "L01");

    m.clips.at("L01").frame_count = 1;
    CHECK(frame(st, in, m).clip == "P01");
}

TEST_CASE("South Karana's classic gnoll animates its patrol" * doctest::skip(!hailstone::test::game_dir())) {
    if (!hailstone::test::game_file("southkarana_chr.s3d")) return;
    auto library = hailstone::assets::CharacterLibrary::load(*hailstone::test::game_dir(), "southkarana");
    const auto gnoll = library.model("GNN");
    REQUIRE(gnoll);
    const auto* walk = gnoll->clip("L01");
    const auto* run = gnoll->clip("L02");
    REQUIRE(run);
    CHECK(run->frame_count > 1);

    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.npc = true;
    in.race = Race::Gnoll;
    in.walk_speed = 0.46f;
    in.now_ms = 1000;
    in.velocity = {0.3f, 0.4f, 0.0f};
    in.speed_run = 0.4f;
    auto choice = hailstone::game::choose_spawn_animation(
        st, in, [&](std::string_view code) { return gnoll->clip(code); }, false, [] { return 50; });
    CHECK(choice.clip == (walk && walk->frame_count > 1 ? "L01" : "L02"));
    CHECK(choice.loop);
}

TEST_CASE("Classic models: moving in deep water swims P06 (0x25), not the C10 action") {
    // 0x4afe60's wet branch starts 0x25 (P06); a model with C10 (action 10) must still swim with P06.
    Model m{{"P01", "L01", "L09", "P06", "C10"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.race = 1;
    in.walk_speed = 0.46f;
    in.now_ms = 1000;
    in.feet_wet = 5;
    in.body_wet = 5;
    in.velocity = {0.3f, 0.4f, 0.0f};
    in.speed_run = 0.4f;
    auto c = frame(st, in, m);
    CHECK(c.clip == "P06");
    CHECK(c.loop);

    OldStyleAnimInput o;
    o.speed_y = 0.3f;
    o.speed_x = 0.4f;
    o.body_wet = 5;
    o.race = 1;
    const OldStyleAnimResult r = select_old_style_anim(o, Fpu(FpuPrecision::Double53));
    CHECK(r.start.played);
    CHECK(r.start.anim == 0x25);
}

TEST_CASE("Classic models: sitting down and standing up") {
    Model m{{"P01", "O01", "L01", "L02", "P02", "P07", "O03"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.race = 1;
    in.now_ms = 100;
    CHECK(frame(st, in, m).clip == "P01");
    in.stand_state = 110;
    auto c = frame(st, in, m);
    CHECK(c.clip == "P02");
    CHECK_FALSE(c.reverse);
    CHECK_FALSE(c.loop);
    CHECK(frame(st, in, m).clip == "P02"); // until it ends
    in.now_ms = 200;
    c = frame(st, in, m, true);
    CHECK(c.clip == "P07"); // seated: standing becomes P07
    // The seated fidget: an idle becomes O03.
    in.now_ms = 6000;
    c = frame(st, in, m, false, 90);
    CHECK(c.clip == "O03");
    in.stand_state = 100;
    c = frame(st, in, m);
    CHECK(c.clip == "P02");
    CHECK(c.reverse);
}

TEST_CASE("Classic models: ducking while still (ChangePosition @0x5a0bc0)") {
    Model m{{"P01", "O01", "L01", "L06", "L08"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.race = 1;
    in.walk_speed = 0.46f;
    in.now_ms = 100;
    CHECK(frame(st, in, m).clip == "P01");
    // Crouching without moving plays L08 once, and it stays.
    in.stand_state = 111;
    auto c = frame(st, in, m);
    CHECK(c.clip == "L08");
    CHECK(c.restart);
    CHECK_FALSE(c.reverse);
    CHECK_FALSE(c.loop);
    in.now_ms = 20000;
    c = frame(st, in, m, true);
    CHECK(c.clip == "L08");
    CHECK_FALSE(c.restart);
    // Moving ducked: the duck walk; crouching again then does not replay L08.
    in.velocity = {0.3f, 0.4f, 0.0f};
    in.speed_run = 0.4f;
    CHECK(frame(st, in, m).clip == "L06");
    // Standing up: L08 backward.
    in.velocity = {0, 0, 0};
    in.speed_run = 0;
    in.stand_state = 100;
    c = frame(st, in, m);
    CHECK(c.clip == "L08");
    CHECK(c.reverse);
}

TEST_CASE("Classic models: P08 / O02 after an animation 0x4b0860 does not name") {
    using hailstone::game::classic_plain_stand_after;
    // 0x4b0860, read from the exe.
    for (int a : {0x20, 1, 5, 0xb, 0x10, 0x11, 0x22, 0x2a, 0x2f, 0x4b, 0xfe})
        CHECK(classic_plain_stand_after(static_cast<std::uint8_t>(a)));
    for (int a : {0, 0x12, 0x13, 0x14, 0x15, 0x18, 0x19, 0x1a, 0x21, 0x26, 0x27, 0x29, 0x30, 0x46, 0x47, 0x48, 0x4a})
        CHECK_FALSE(classic_plain_stand_after(static_cast<std::uint8_t>(a)));

    Model m{{"P01", "P08", "O01", "O02", "L01", "L02"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.race = 1;
    in.walk_speed = 0.46f;
    in.now_ms = 1000;
    // Nothing played yet (+0x10c0 0xfe): P01.
    CHECK(frame(st, in, m).clip == "P01");
    // Stopping after a walk: P01.
    in.velocity = {0.3f, 0.4f, 0.0f};
    in.speed_run = 0.4f;
    CHECK(frame(st, in, m).clip == "L01");
    in.velocity = {0, 0, 0};
    in.speed_run = 0;
    CHECK(frame(st, in, m).clip == "P01");
    // Stopping after a run: P08, which stays (it is not named either).
    in.velocity = {0.3f, 0.4f, 0.0f};
    in.speed_run = 0.7f;
    CHECK(frame(st, in, m).clip == "L02");
    in.velocity = {0, 0, 0};
    in.speed_run = 0;
    auto c = frame(st, in, m);
    CHECK(c.clip == "P08");
    CHECK(c.anim == 0x47);
    CHECK(c.loop);
    CHECK(frame(st, in, m).clip == "P08");
    // The idle after it is O02.
    in.now_ms = 7100;
    c = frame(st, in, m, false, 90);
    CHECK(c.clip == "O02");
    // A model without P08 / O02 keeps P01 / O01.
    Model plain{{"P01", "O01", "L01", "L02"}};
    hailstone::game::SpawnAnimState st2;
    in.now_ms = 1000;
    in.velocity = {0.3f, 0.4f, 0.0f};
    in.speed_run = 0.7f;
    CHECK(frame(st2, in, plain).clip == "L02");
    in.velocity = {0, 0, 0};
    in.speed_run = 0;
    c = frame(st2, in, plain);
    CHECK(c.clip == "P01");
    CHECK(c.anim == 0x20);
    in.now_ms = 7100;
    CHECK(frame(st2, in, plain, false, 90).clip == "O01");
    // An action left as the animation playing decides too: after a social (S06, 0x30) P08, after a slash P01.
    hailstone::game::SpawnAnimState st3;
    in.now_ms = 1000;
    st3.anim = 0x30;
    CHECK(frame(st3, in, m).clip == "P08");
    st3.anim = 5;
    CHECK(frame(st3, in, m).clip == "P01");
    // The follow-up (+0x10c4) 0x47 plays P08 whatever played, and becomes 0x20 after a named animation.
    in.follow_up = 0x47;
    st3.anim = 5;
    c = frame(st3, in, m);
    CHECK(c.clip == "P08");
    CHECK(c.follow_up == 0x20);
    // An action as the follow-up: the base stays P01; +0x10c4 is then 0x47 after a run.
    in.follow_up = 2;
    st3.anim = 0x12;
    c = frame(st3, in, m);
    CHECK(c.clip == "P01");
    CHECK(c.follow_up == 0x47);
    // No stand request: +0x10c4 is left as it is.
    in.velocity = {0.3f, 0.4f, 0.0f};
    in.speed_run = 0.4f;
    CHECK(frame(st3, in, m).follow_up == 2);
}

TEST_CASE("Climbing (CollidingType 6): L07 / CLMB up, reversed down") {
    Model m{{"P01", "L01", "L07"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.race = 1;
    in.now_ms = 1000;
    in.colliding_type = 6;
    in.velocity = {0, 0, 0.5f};
    auto c = frame(st, in, m);
    CHECK(c.clip == "L07");
    CHECK_FALSE(c.reverse);
    in.velocity = {0, 0, -0.5f};
    c = frame(st, in, m);
    CHECK(c.clip == "L07");
    CHECK(c.reverse);
    // Off the ladder the same fall is not a climb.
    in.colliding_type = 0;
    CHECK(frame(st, in, m).clip != "L07");
    // New-style: CLMB.
    Model n{{"STND_BA_1", "CLMB_BA_1"}};
    hailstone::game::SpawnAnimState sn;
    in.new_style = true;
    in.colliding_type = 6;
    in.velocity = {0, 0, 0.5f};
    c = frame(sn, in, n);
    CHECK(c.clip == "CLMB_BA_1");
    CHECK_FALSE(c.reverse);
}

TEST_CASE("New-style models: the sequences' tracks") {
    Model m{{"STND_BA_1", "WALK_BA_1", "NRUN_BA_1", "SWIM_BA_1", "NSIT_BA_1", "STNG_BA_1"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.new_style = true;
    in.race = 522;
    in.walk_speed = 0.46f;
    in.model_scale = 1.3f;
    in.velocity = {0.3f, 0.4f, 0.0f};
    in.speed_run = 0.7f;
    auto c = frame(st, in, m);
    CHECK(c.clip == "NRUN_BA_1");
    in.speed_run = -0.3f;
    c = frame(st, in, m);
    CHECK(c.clip == "WALK_BA_1"); // BWLK missing: its fallback WALK, reversed
    CHECK(c.reverse);
    in.body_wet = 5;
    c = frame(st, in, m);
    CHECK(c.clip == "SWIM_BA_1");
    in.body_wet = 0;
    in.speed_run = 0.3f;
    CHECK(frame(st, in, m).clip == "WALK_BA_1");
    // Still: ChooseIdleAnimation's STND (the display clock past the cleared +0x360).
    in.velocity = {0, 0, 0};
    in.speed_run = 0;
    in.now_ms = 1000;
    CHECK(frame(st, in, m).clip == "STND_BA_1");
    // Sitting down: STNG once, then NSIT.
    in.stand_state = 110;
    c = frame(st, in, m);
    CHECK(c.clip == "STNG_BA_1");
    CHECK_FALSE(c.loop);
    in.now_ms = 1100;
    CHECK(frame(st, in, m, true).clip == "NSIT_BA_1");
}

TEST_CASE("New-style models: ducking while still plays CRCH") {
    Model m{{"STND_BA_1", "WALK_BA_1", "CRCH_BA_1"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.new_style = true;
    in.race = 522;
    in.now_ms = 1000;
    CHECK(frame(st, in, m).clip == "STND_BA_1");
    in.stand_state = 111;
    auto c = frame(st, in, m);
    CHECK(c.clip == "CRCH_BA_1");
    CHECK_FALSE(c.reverse);
    in.now_ms = 1100;
    CHECK(frame(st, in, m, true).clip == "CRCH_BA_1");
    in.stand_state = 100;
    c = frame(st, in, m);
    CHECK(c.clip == "CRCH_BA_1");
    CHECK(c.reverse);
}

TEST_CASE("New-style tracks: fallbacks, columns, alternates and variants") {
    using hailstone::game::NewStyleTrackInput;
    Model m{{"STND_BA_1", "STND_1H_1", "STDA_BA_1", "IDLE_BA_1", "IDLE_BA_2", "IDLE_BA_3", "SLPR_BA_1",
             "SLPR_1H_1", "SL2H_2H_1", "KICK_BA_1"}};
    auto has = [&](std::string_view c) { return m.has(c); };
    int r = 0;
    auto rand = [&] { return r; };
    auto track = [&](std::uint8_t code, NewStyleTrackInput in = {}) {
        return hailstone::game::new_style_track(code, in, has, rand).clip;
    };
    CHECK(track(0x50) == "SLPR_BA_1");
    CHECK(track(0x54) == "SLPR_BA_1"); // BLSC -> SLSC -> SLPR
    CHECK(track(0x60) == "KICK_BA_1"); // RKCK -> KICK
    CHECK(track(0x5b) == "SLPR_BA_1"); // BASH -> SLPR
    CHECK(track(0x81) == "STND_BA_1"); // STUN -> STND
    CHECK(track(0x4f).empty());
    // A primary weapon: column 1 (1H); 2H for SL2H; column 1 then 0 when the column lacks it.
    NewStyleTrackInput armed{0x40, 0, -1};
    CHECK(track(0x50, armed) == "SLPR_1H_1");
    CHECK(track(0x56, armed) == "SL2H_2H_1");
    CHECK(track(0x5f, armed) == "KICK_BA_1");
    CHECK(track(0x6e, armed) == "STND_1H_1");
    // Dual wield (0x80): column 3, then 1.
    CHECK(track(0x50, {0xc0, 0, -1}) == "SLPR_1H_1");
    // The combat stand: aggressive, or after an attack.
    CHECK(track(0x6e, {0x4, 0, -1}) == "STDA_BA_1");
    CHECK(track(0x6e, {0, 0x50, -1}) == "STDA_BA_1");
    CHECK(track(0x6e, {0, 5, -1}) == "STDA_BA_1"); // C05 playing: SLPR
    CHECK(track(0x6e, {0, 0x6e, -1}) == "STND_BA_1");
    // Variants: rand() % 3.
    r = 0;
    CHECK(track(0x6f) == "IDLE_BA_1");
    r = 4;
    CHECK(track(0x6f) == "IDLE_BA_2");
    r = 5;
    CHECK(track(0x6f) == "IDLE_BA_3");
}

TEST_CASE("New-style riders select mount tracks and retain their resolved style") {
    Model m{{"STND_BA_1", "STND_MT_1", "WALK_MT_1", "SLPR_MT_1", "SLPR_1H_1"}};
    auto has = [&](std::string_view name) { return m.has(name); };
    hailstone::game::NewStyleTrackInput input;
    input.mounted = true;
    input.player_state = 0xc0; // the mount style wins over dual wield
    auto standing = hailstone::game::new_style_track(0x6e, input, has, {});
    CHECK(standing.clip == "STND_MT_1");
    CHECK(standing.column == 5);
    auto action = hailstone::game::new_style_track(0x50, input, has, {});
    CHECK(action.clip == "SLPR_MT_1");
    CHECK(hailstone::game::new_style_clip_column(action.clip) == 5);
    CHECK(hailstone::game::new_style_clip_column("STND_1H_3") == 1);
    CHECK(hailstone::game::new_style_clip_column("P09") == 0);
    hailstone::game::SpawnAnimState state;
    hailstone::game::SpawnAnimInput in;
    in.new_style = true;
    in.mounted = true;
    in.player_state = 0xc0;
    in.walk_speed = 0.7f;
    in.run_speed = 1.25f;
    in.model_scale = 1.0f;
    in.velocity = {0.0f, 0.4f, 0.0f};
    in.speed_run = 0.4f;
    auto choice = frame(state, in, m);
    CHECK(choice.clip == "WALK_MT_1");
    CHECK(choice.column == 5);
    CHECK(state.column == 5);
    auto repeated = frame(state, in, m);
    CHECK(repeated.clip == choice.clip);
    CHECK_FALSE(repeated.restart);
}

TEST_CASE("OP_Animation actions: the clip, else 0x4b1210's replacement") {
    auto clip = [](std::uint8_t action, const Model& m) {
        return hailstone::game::action_clip(action, [&](std::string_view c) { return m.has(c); });
    };
    Model rat{{"P01", "L01", "C01", "C05", "D01"}};
    CHECK(clip(5, rat) == "C05");
    CHECK(clip(8, rat) == "C05"); // EQEmu's NPC hand to hand
    CHECK(clip(6, rat) == "C05"); // the off hand
    CHECK(clip(2, rat) == "C05");
    CHECK(clip(11, rat) == "C01"); // round kick -> kick
    CHECK(clip(1, rat) == "C01");
    CHECK(clip(13, rat) == "D01"); // D02 -> D01
    CHECK(clip(14, rat).empty());  // D03 -> D02, missing too
    Model human{{"P01", "C05", "C08", "P06"}};
    CHECK(clip(8, human) == "C08");
    CHECK(clip(10, human) == "P06"); // swim attack -> swimming
    Model bare{{"P01", "L01"}};
    CHECK(clip(8, bare).empty());  // no C05 either: the movement choice keeps playing
    CHECK(clip(26, bare).empty()); // O01 -> P01 is the stand: not a one-shot
    CHECK(clip(0, rat).empty());
    CHECK(clip(0xb7, rat).empty());
    CHECK(clip(0x50, rat) == "C05"); // a new-style code on a classic model: SLPR -> C05
    // New-style models: the old-style number's sequence (0xaaf5c8).
    Model eqg{{"STND_BA_1", "SLPR_BA_1", "SLSC_BA_1", "KICK_BA_1", "FLCH_BA_1"}};
    auto nclip = [&](std::uint8_t action) {
        return hailstone::game::action_clip(action, [&](std::string_view c) { return eqg.has(c); }, true);
    };
    CHECK(nclip(5) == "SLPR_BA_1");
    CHECK(nclip(6) == "SLSC_BA_1");
    CHECK(nclip(8) == "SLPR_BA_1"); // PATK -> SLPR
    CHECK(nclip(11) == "KICK_BA_1"); // RKCK -> KICK
    CHECK(nclip(13) == "FLCH_BA_1"); // MSHT -> FLCH
    CHECK(nclip(0x23).empty());      // P04: no sequence
}

namespace {

// The new tests' motion and clock: a walk at 0.46 units a tick, and a display clock a second in, then past the
// 5 s fidget wait.
constexpr float kWalkSpeed = 0.46f;
constexpr float kVelocityX = 0.3f, kVelocityY = 0.4f;
constexpr float kSpeedRun = 0.4f;
constexpr std::uint32_t kClockMs = 1000, kIdleClockMs = 7100;
constexpr std::uint32_t kIdleWaitMs = kIdleClockMs - kClockMs;
constexpr float kDoubled = 2.0f, kUnchanged = 1.0f, kHalfRate = 0.5f;

// The display clock's ms since the test's start, from a ManualClock.
std::uint32_t clock_ms(const hailstone::test::ManualClock& clock, hailstone::test::ManualClock::time_point start) {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock.source()() - start).count());
}

} // namespace

TEST_CASE("Classic models: 0x4afc90's flags reach the movement rate") {
    // A Luclin elemental (races 209..212) plays at twice the rate while UseLuclinElementals (+0x129) is on.
    Model m{{"P01", "O01", "L01", "L02"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.race = Race::EarthElemental;
    in.walk_speed = kWalkSpeed;
    in.now_ms = kClockMs;
    in.velocity = {kVelocityX, kVelocityY, 0.0f};
    in.speed_run = kSpeedRun;
    in.luclin_elementals = false;
    const float plain = frame(st, in, m).rate;
    in.luclin_elementals = true;
    CHECK(frame(st, in, m).rate == doctest::Approx(plain * kDoubled));
    // Another race is not doubled.
    in.race = Race::Human;
    CHECK(frame(st, in, m).rate == doctest::Approx(plain));
}

TEST_CASE("Classic models: a styled model's stand takes the pre-processing") {
    Model m{{"P01", "O01", "L01"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.race = Race::Human;
    in.now_ms = kClockMs;
    in.tunic_point = true;
    auto c = frame(st, in, m);
    CHECK(c.clip == "P01");
    CHECK(c.rate == doctest::Approx(kHalfRate));
}

TEST_CASE("Classic models: a missing walk falls back to the stand at its own rate") {
    // A styled model with P01 but without L01 / L02: the walk's 1.25 must not reach the stand (0x4b1a70 sets 1.0,
    // then the pre-processing makes a styled P01 0.5).
    Model m{{"P01", "O01"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.race = Race::Human;
    in.walk_speed = kWalkSpeed;
    in.now_ms = kClockMs;
    in.velocity = {kVelocityX, kVelocityY, 0.0f};
    in.speed_run = kSpeedRun;
    in.tunic_point = true;
    auto c = frame(st, in, m);
    CHECK(c.clip == "P01");
    CHECK(c.rate == doctest::Approx(kHalfRate));
    CHECK(c.loop);
    CHECK_FALSE(c.reverse);
}

TEST_CASE("Classic models: a missing backward walk keeps the stand backward") {
    // 0x4b1ab3 clears the restart flag and sets 1.0, but not the reverse flag: a backward walk's stand fallback
    // plays backward too.
    Model m{{"P01", "O01"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.race = Race::Human;
    in.walk_speed = kWalkSpeed;
    in.now_ms = kClockMs;
    in.velocity = {kVelocityX, kVelocityY, 0.0f};
    in.speed_run = -kSpeedRun; // backward
    auto c = frame(st, in, m);
    CHECK(c.clip == "P01");
    CHECK(c.reverse);
}

TEST_CASE("Classic models: an action's rate goes through 0x4b15b0's pre-processing and 0x4afc90") {
    using hailstone::AnimationType;
    using hailstone::game::classic_action_rate;
    constexpr float kRate = 1.0f; // the rate itself (the packet's 10 / 10)
    constexpr std::uint8_t kMale = 0, kFemale = 1, kNeutral = 2;
    constexpr float kMountRaceRun = 0.4f, kHalf = 0.5f, kRace330L04 = 0.4875f, kSocial = 0.75f;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    // A Luclin elemental's action is doubled while the option is on.
    CHECK(classic_action_rate(anim(AnimationType::ThrowOrOneHandSlash), kRate, Race::EarthElemental, kMale, false,
                              true) == doctest::Approx(kDoubled));
    CHECK(classic_action_rate(anim(AnimationType::ThrowOrOneHandSlash), kRate, Race::EarthElemental, kMale, false,
                              false) == doctest::Approx(kUnchanged));
    CHECK(classic_action_rate(anim(AnimationType::ThrowOrOneHandSlash), kRate, Race::Human, kMale, false, true) ==
          doctest::Approx(kUnchanged));
    // D05 on a styled model (the animation set's +0x34, a TUNIC_POINT dag): twice the rate.
    CHECK(classic_action_rate(anim(AnimationType::Death), kRate, Race::Human, kMale, true, true) ==
          doctest::Approx(kDoubled));
    CHECK(classic_action_rate(anim(AnimationType::Death), kRate, Race::Human, kMale, false, true) ==
          doctest::Approx(kUnchanged));
    // A styled model's gender-1 animations 2 and 4 (race 2's 5 too).
    CHECK(classic_action_rate(anim(AnimationType::Piercing), kRate, Race::Human, kFemale, true, true) ==
          doctest::Approx(kDoubled));
    CHECK(classic_action_rate(anim(AnimationType::TwoHandBluntPiercing), kRate, Race::Human, kFemale, true, true) ==
          doctest::Approx(kDoubled));
    CHECK(classic_action_rate(anim(AnimationType::ThrowOrOneHandSlash), kRate, Race::Barbarian, kFemale, true,
                              true) == doctest::Approx(kDoubled));
    CHECK(classic_action_rate(anim(AnimationType::Piercing), kRate, Race::Human, kMale, true, true) ==
          doctest::Approx(kUnchanged));
    // 0x4b15b0's pre-processing: race 330's L04 at 0.65, then 0x4afc90's 0.75, from the rate 1.0.
    CHECK(classic_action_rate(anim(AnimationType::JumpStand), kRate, Race::Froglok2, kMale, false, false) ==
          doctest::Approx(kRace330L04));
    // A mount race's run at 0.4, its other animations at 0.5.
    CHECK(classic_action_rate(anim(AnimationType::Run), kRate, Race::ClockworkBoar, kMale, false, false) ==
          doctest::Approx(kMountRaceRun));
    CHECK(classic_action_rate(anim(AnimationType::Walk), kRate, Race::ClockworkBoar, kMale, false, false) ==
          doctest::Approx(kHalf));
    // Races 42 and 120's neutral gender (2) non-walk at 0.5; gender 1 is not affected.
    CHECK(classic_action_rate(anim(AnimationType::Stand), kRate, Race::Wolf, kNeutral, false, false) ==
          doctest::Approx(kHalf));
    CHECK(classic_action_rate(anim(AnimationType::Stand), kRate, Race::Wolf, kFemale, false, false) ==
          doctest::Approx(kUnchanged));
    // A styled model's stand / idle at 0.5, a social (an S clip) at 0.75.
    CHECK(classic_action_rate(anim(AnimationType::Stand), kRate, Race::Human, kMale, true, false) ==
          doctest::Approx(kHalf));
    CHECK(classic_action_rate(anim(AnimationType::Wave), kRate, Race::Human, kMale, true, false) ==
          doctest::Approx(kSocial));
}

TEST_CASE("Classic models: a pet's idle needs a roll above 99 (+0x38c)") {
    // 0x4afe60's fidget thresholds: 75, or 99 for a spawn with an owner (+0x38c).
    constexpr int kIdleRoll = 75, kPetIdleRoll = 99;
    constexpr std::uint32_t kPetOwner = 42;
    Model m{{"P01", "O01", "L01"}};
    auto roll = [&](std::uint32_t owner, int percent) {
        hailstone::test::ManualClock clock;
        clock.hold();
        const auto start = clock.source()();
        hailstone::game::SpawnAnimState st;
        hailstone::game::SpawnAnimInput in;
        in.race = Race::Human;
        in.now_ms = clock_ms(clock, start);
        in.owner_id = owner;
        frame(st, in, m); // P01, so +0x10c0 is a stand the idle may start from
        clock.advance(std::chrono::milliseconds(kIdleWaitMs));
        in.now_ms = clock_ms(clock, start);
        return frame(st, in, m, false, percent).clip;
    };
    CHECK(roll(kPetOwner, kPetIdleRoll) == "P01");
    CHECK(roll(kPetOwner, kPetIdleRoll + 1) == "O01");
    CHECK(roll(0, kIdleRoll + 1) == "O01");
    CHECK(roll(0, kIdleRoll) == "P01");
}

TEST_CASE("Classic models: turning on the spot plays P03 (0x4afe60's argument)") {
    Model m{{"P01", "O01", "L01", "P03"}};
    hailstone::game::SpawnAnimState st;
    hailstone::game::SpawnAnimInput in;
    in.race = Race::Human;
    in.now_ms = kClockMs;
    CHECK(frame(st, in, m).clip == "P01");
    in.turning = true;
    in.speed_heading = 1.0f;
    auto c = frame(st, in, m);
    CHECK(c.clip == "P03");
    CHECK(c.reverse);
    in.speed_heading = -1.0f;
    c = frame(st, in, m);
    CHECK(c.clip == "P03");
    CHECK_FALSE(c.reverse);
}

TEST_CASE("Classic models: attacking suppresses the idle (0x4afe60)") {
    constexpr int kIdleRoll = 75;
    Model m{{"P01", "O01", "L01"}};
    auto roll = [&](bool attacking, bool local_player) {
        hailstone::test::ManualClock clock;
        clock.hold();
        const auto start = clock.source()();
        hailstone::game::SpawnAnimState st;
        hailstone::game::SpawnAnimInput in;
        in.race = Race::Human;
        in.now_ms = clock_ms(clock, start);
        in.attacking = attacking;
        in.local_player = local_player;
        frame(st, in, m); // P01, so +0x10c0 is a stand the idle may start from
        clock.advance(std::chrono::milliseconds(kIdleWaitMs));
        in.now_ms = clock_ms(clock, start);
        return frame(st, in, m, false, kIdleRoll + 1).clip;
    };
    CHECK(roll(true, true) == "P01");
    CHECK(roll(false, true) == "O01");
    // 0x4b01dd reads the global switch without a local-player check: another actor's fidget stops too.
    CHECK(roll(true, false) == "P01");
    CHECK(roll(false, false) == "O01");
}

TEST_CASE("New-style models: auto attack suppresses the fidget (0x58a352)") {
    constexpr int kIdleRoll = 75;
    Model m{{"STND_BA_1", "IDLE_BA_1"}};
    auto roll = [&](bool attacking) {
        hailstone::test::ManualClock clock;
        clock.hold();
        const auto start = clock.source()();
        hailstone::game::SpawnAnimState st;
        hailstone::game::SpawnAnimInput in;
        in.new_style = true;
        in.race = Race::Drakkin;
        in.now_ms = clock_ms(clock, start);
        in.attacking = attacking;
        frame(st, in, m); // STND, so +0x10c0 is a stand the idle may start from
        clock.advance(std::chrono::milliseconds(kIdleWaitMs));
        in.now_ms = clock_ms(clock, start);
        return frame(st, in, m, false, kIdleRoll + 1).clip;
    };
    CHECK(roll(true) == "STND_BA_1");
    CHECK(roll(false) == "IDLE_BA_1");
}

TEST_CASE("OP_Animation actions: a new-style STND normalizes to P01 for the sit conversion") {
    using hailstone::AnimationType;
    Model m{{"P01", "P07"}};
    std::uint8_t playing = 0, normalized = 0;
    bool stand = false;
    auto clip = [&](std::uint8_t action) {
        playing = 0;
        normalized = 0;
        stand = false;
        return hailstone::game::action_clip(action, [&](std::string_view c) { return m.has(c); }, false, {}, {},
                                            &playing, &stand, &normalized);
    };
    // A raw P01 and a new-style STND both normalize to P01 (the sit conversion sees the normalized request).
    CHECK(clip(static_cast<std::uint8_t>(AnimationType::Stand)).empty());
    CHECK(normalized == static_cast<std::uint8_t>(AnimationType::Stand));
    CHECK(stand);
    CHECK(clip(kAnimStand).empty());
    CHECK(normalized == static_cast<std::uint8_t>(AnimationType::Stand));
    CHECK(stand);
}

TEST_CASE("Classic models: the follow-up after a stand request is P01 or P08, never the action") {
    using hailstone::AnimationType;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    // 0x4b1a7e: the animation playing before the movement choice decides P01 or P08.
    CHECK(hailstone::game::classic_follow_up_after(anim(AnimationType::Stand)) == anim(AnimationType::Stand));
    CHECK(hailstone::game::classic_follow_up_after(anim(AnimationType::Walk)) == anim(AnimationType::Stand));
    CHECK(hailstone::game::classic_follow_up_after(anim(AnimationType::Run)) == anim(AnimationType::StandAfter));
    CHECK(hailstone::game::classic_follow_up_after(anim(AnimationType::Duck)) == anim(AnimationType::StandAfter));
}

TEST_CASE("OP_Animation actions: a missing clip and replacement is a stand request") {
    using hailstone::AnimationType;
    Model bare{{"P01", "L01"}};
    std::uint8_t playing = 0;
    bool stand = false;
    auto clip = [&](std::uint8_t action, const Model& m) {
        playing = 0;
        stand = false;
        return hailstone::game::action_clip(action, [&](std::string_view c) { return m.has(c); }, false, {}, {},
                                            &playing, &stand);
    };
    // 0x4b15b0's stand path: the movement choice plays P01 / the follow-up, and +0x10c0 becomes 0x20.
    CHECK(clip(static_cast<std::uint8_t>(AnimationType::MainhandSlash), bare).empty());
    CHECK(stand);
    CHECK(playing == static_cast<std::uint8_t>(AnimationType::Stand));
    CHECK(clip(static_cast<std::uint8_t>(AnimationType::Idle), bare).empty()); // O01 -> P01 is the stand
    CHECK(stand);
    // A direct P01 request is the stand path, not a one-shot action.
    CHECK(clip(static_cast<std::uint8_t>(AnimationType::Stand), bare).empty());
    CHECK(stand);
    CHECK(playing == static_cast<std::uint8_t>(AnimationType::Stand));
    // A replacement the model has: no stand request, and the replacement is the animation playing.
    Model rat{{"P01", "C05"}};
    CHECK(clip(static_cast<std::uint8_t>(AnimationType::MainhandSlash), rat) == "C05");
    CHECK_FALSE(stand);
    CHECK(playing == static_cast<std::uint8_t>(AnimationType::ThrowOrOneHandSlash));
    CHECK(clip(static_cast<std::uint8_t>(AnimationType::ThrowOrOneHandSlash), rat) == "C05");
    CHECK_FALSE(stand);
    CHECK(playing == static_cast<std::uint8_t>(AnimationType::ThrowOrOneHandSlash));
}
