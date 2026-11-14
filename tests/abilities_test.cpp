// Key bindings, hot buttons, skills and combat abilities: the packet bytes UseSkill sends, the reuse timers and
// haste (ModifyAttackSpeed at the in-game 24-bit precision), UseSkill's checks and messages, the key map's ini
// round trip and the hot button format.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#include "core/config.h"
#include "core/ini_file.h"
#include "game/hotbuttons.h"
#include "game/keymap.h"
#include "game/mechanics/abilities.h"
#include "game/mechanics/character.h"
#include "game/mechanics/tables.h"
#include "net/ability_packets.h"
#include "net/opcodes.h"

using namespace hailstone;
using namespace hailstone::game;
using namespace hailstone::game::mechanics;

namespace {

std::vector<std::uint8_t> bytes(const net::AppPacket& p) {
    std::vector<std::uint8_t> out(p.data.size());
    if (!p.data.empty()) std::memcpy(out.data(), p.data.data(), p.data.size());
    return out;
}

// A warrior with the given skills; TotalEffect from a table (spa, sub) → value.
struct Fixture {
    GameTables tables;
    CharacterSheet sheet;
    std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t> effects;
    Character ch{sheet, tables, [](std::int32_t) -> const SpellData* { return nullptr; }};
    Fixture() {
        sheet.class_id = 1;
        sheet.race = 1;
        sheet.level = 10;
        ch.hooks.total_effect = [this](std::int32_t spa, bool, std::int32_t sub, bool, bool) -> std::optional<std::int32_t> {
            auto it = effects.find({spa, sub});
            if (it == effects.end()) it = effects.find({spa, -1});
            return it == effects.end() ? 0 : it->second;
        };
        ch.hooks.adjusted_skill = [this](std::uint32_t s) -> std::optional<std::int32_t> {
            return s < kSkills ? sheet.skills[s] : 0;
        };
        ch.hooks.skill_with_item_mods = [this](std::uint32_t s, bool) -> std::optional<std::int32_t> {
            return s < kSkills ? sheet.skills[s] : 0;
        };
    }
    SkillContext context(std::uint32_t now) {
        SkillContext c;
        c.ch = &ch;
        c.me.id = 100;
        c.me.type = 0;
        c.me.heading = 0; // facing +y
        c.me.melee_radius = 5.3f;
        c.me.scale = 1.2f;
        c.me.avatar_height = 3.75f;
        c.me.level = 10;
        c.now = now;
        c.tick_count = now;
        c.random = [] { return 0; };
        c.random_range = [](std::int32_t lo, std::int32_t) { return lo; };
        return c;
    }
    static SkillActor rat(float y) {
        SkillActor a;
        a.id = 200;
        a.type = 1;
        a.y = y;
        a.melee_radius = 1.3f;
        a.scale = 0.4f;
        a.avatar_height = 1.25f;
        a.level = 1;
        a.name = "a large rat";
        return a;
    }
};

} // namespace

TEST_CASE("ability packets are byte-exact") {
    CHECK(net::encode_combat_ability(0x1234, 100, 30).opcode == 0x3eba);
    CHECK(bytes(net::encode_combat_ability(0x1234, 100, 30)) ==
          std::vector<std::uint8_t>{0x34, 0x12, 0, 0, 100, 0, 0, 0, 30, 0, 0, 0});
    CHECK(net::encode_bind_wound(0x55).opcode == 0x0386);
    CHECK(bytes(net::encode_bind_wound(0x55)) == std::vector<std::uint8_t>{0x55, 0, 0, 0, 0, 0, 0, 0});
    CHECK(net::encode_disarm(1, 2, 37).opcode == 0x5ec8);
    CHECK(bytes(net::encode_disarm(1, 2, 37)) ==
          std::vector<std::uint8_t>{1, 0, 0, 0, 2, 0, 0, 0, 37, 0, 0, 0, 0xff, 0, 0, 0});
    // PickPocket_Struct (18 bytes): to, from, myskill (u16) + type + unknown, coin, lastsix.
    CHECK(net::encode_pick_pocket(7, 9, 50).opcode == 0x39e8);
    CHECK(bytes(net::encode_pick_pocket(7, 9, 50)) ==
          std::vector<std::uint8_t>{7, 0, 0, 0, 9, 0, 0, 0, 50, 0, 0, 0, 0xff, 0, 0, 0, 0, 0});
    CHECK(net::encode_begging(7, 9, 5).opcode == 0x6703);
    CHECK(bytes(net::encode_begging(7, 9, 5)).size() == 18);
    CHECK(bytes(net::encode_feign_death(0x102)) == std::vector<std::uint8_t>{2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    CHECK(net::encode_feign_death(1).opcode == 0x52fa);
    CHECK(bytes(net::encode_taunt(0x0a0b)) == std::vector<std::uint8_t>{0x0b, 0x0a, 0, 0});
    CHECK(net::encode_taunt(1).opcode == 0x2703);
    CHECK(bytes(net::encode_animation(0x123, 5, 5)) == std::vector<std::uint8_t>{0x23, 0x01, 5, 5});
    CHECK(net::encode_hide_cancel().opcode == 0x67fe);
    CHECK(bytes(net::encode_hide_cancel()) == std::vector<std::uint8_t>{0, 0, 0, 0});

    using K = SkillSend::Kind;
    auto empty = [](K k, std::uint16_t op) {
        auto p = net::encode_skill_send({k});
        REQUIRE(p);
        CHECK(p->opcode == op);
        CHECK(p->data.empty());
    };
    empty(K::Hide, 0x67fe);
    empty(K::Sneak, 0x5d55);
    empty(K::Forage, 0x5306);
    empty(K::Mend, 0x0ecf);
    empty(K::SenseHeading, 0x260a);
    empty(K::TrackUnknown, 0x4577);
    empty(K::Fishing, 0x1e2a);
    empty(K::InstillDoubt, 0x640e);
    empty(K::SenseTraps, 0x02af);
    empty(K::DisarmTraps, 0x78bf);
    auto stop = net::encode_skill_send({K::StopSneak, 0x77});
    REQUIRE(stop);
    CHECK(stop->opcode == 0x0971);
    CHECK(bytes(*stop) == std::vector<std::uint8_t>{0x77, 0, 15, 0, 0, 0, 0, 0});

    auto upd = net::decode_skill_update(net::encode_skill_update({30, 42}));
    REQUIRE(upd);
    CHECK(upd->skill == 30);
    CHECK(upd->value == 42);
}

TEST_CASE("reuse timers: GetSkillReuseTime and ModifyAttackSpeed at 24-bit precision") {
    Fixture f;
    CHECK(skill_table_entry(skill::kKick).reuse_ms == 8000);
    CHECK(skill_table_entry(skill::kKick).combat_type == 2);
    CHECK(skill_table_entry(skill::kTaunt).reuse_ms == 6000);
    CHECK(skill_table_entry(skill::kHide).reuse_ms == 10000);
    CHECK(skill_reuse_time(f.ch, skill::kKick) == 8000);
    f.effects[{0xe3, skill::kKick}] = 2; // SPA 227: 2 seconds off kick only
    CHECK(skill_reuse_time(f.ch, skill::kKick) == 6000);
    CHECK(skill_reuse_time(f.ch, skill::kBash) == 8000);

    CHECK(modify_attack_speed(f.ch, 8000) == 8000); // no haste
    f.effects[{11, -1}] = 150;
    // 150 * 0.01f rounds to 1.5 at 24 bits: 8000 / 1.5 = 5333.33 -> 5333.
    CHECK(modify_attack_speed(f.ch, 8000) == 5333);
    f.effects[{11, -1}] = 130;
    // 130 * 0.01f = 1.29999995 (24 bits); 8000 / 1.29999995 = 6153.85 -> 6153.
    CHECK(modify_attack_speed(f.ch, 8000) == 6153);
    CHECK(modify_attack_speed(f.ch, 0) == 0);
}

TEST_CASE("UseSkill: kick goes through DoAttack with a hasted combat timer") {
    Fixture f;
    f.sheet.skills[skill::kKick] = 10;
    f.effects[{11, -1}] = 150;
    SkillState st;
    SkillContext c = f.context(1000000);
    c.target = Fixture::rat(10.0f); // 10 units ahead (+y)

    SkillResult r = use_skill(c, st, skill::kKick);
    REQUIRE(r.sends.size() == 1);
    CHECK(r.sends[0].kind == SkillSend::Kind::CombatAbility);
    CHECK(r.sends[0].target == 200);
    CHECK(r.sends[0].skill == skill::kKick);
    CHECK_FALSE(st.timers.is_available(skill::kKick));
    CHECK(st.timers.combat_ticks[0] == 8000);
    // The shown duration is hasted: 8000 / 1.5.
    CHECK(st.timers.timer_duration(f.ch, skill::kKick) == 5333);

    // Too early: DoAttack refuses silently (the combat timer is hasted).
    c.now += 5000;
    CHECK(use_skill(c, st, skill::kKick).sends.empty());
    st.timers.update(f.ch, c.now, false, false);
    CHECK_FALSE(st.timers.is_available(skill::kKick));
    c.now += 400; // 5400 >= 5333
    st.timers.update(f.ch, c.now, false, false);
    CHECK(st.timers.is_available(skill::kKick));
    CHECK(use_skill(c, st, skill::kKick).sends.size() == 1);
}

TEST_CASE("UseSkill: a skill the server's profile gives is used without the game's skill caps") {
    // No SkillCaps.txt (or another server's caps): GetAdjustedSkill alone says 0, which refused kick, bash and
    // taunt without a word.
    Fixture f;
    f.ch.hooks.adjusted_skill = nullptr;
    f.sheet.skills[skill::kKick] = 10;
    SkillState st;
    SkillContext c = f.context(1000000);
    c.target = Fixture::rat(10.0f);
    SkillResult r = use_skill(c, st, skill::kKick);
    REQUIRE(r.sends.size() == 1);
    CHECK(r.sends[0].kind == SkillSend::Kind::CombatAbility);
    // an untrained skill still does nothing
    CHECK(use_skill(c, st, skill::kTaunt).sends.empty());
}

TEST_CASE("CanIHit: range and facing with the original's messages") {
    Fixture f;
    f.sheet.skills[skill::kKick] = 10;
    SkillState st;
    SkillContext c = f.context(1000);
    // __get_melee_range: (5.3 * 1.2 + 1.3 * 0.4) * 0.75 = 5.16 -> 14, + 2 + |3.75 - 1.25| = 18.5.
    SkillActor near = Fixture::rat(0);
    CHECK(melee_range(&c.me, &near) == doctest::Approx(18.5f));
    c.target = Fixture::rat(25.0f);
    SkillResult far = use_skill(c, st, skill::kKick);
    CHECK(far.sends.empty());
    REQUIRE(far.messages.size() == 1);
    CHECK(far.messages[0].eqstr == 124); // Your target is too far away, get closer!

    SkillState st2;
    c.target = Fixture::rat(-10.0f); // behind
    SkillResult behind = use_skill(c, st2, skill::kKick);
    REQUIRE(behind.messages.size() == 1);
    CHECK(behind.messages[0].eqstr == 108); // You cannot see your target.

    SkillState st3;
    c.target.reset();
    SkillResult none = use_skill(c, st3, skill::kKick);
    REQUIRE(none.messages.size() == 1);
    CHECK(none.messages[0].eqstr == 12697);

    // __get_bearing: +y is heading 0 (straight ahead the original takes the 270 branch: 511.5), +x is 128.
    CHECK(bearing(0, 0, 10, 0) == doctest::Approx(511.5));
    CHECK(bearing(0, 0, 10, 0.5) == doctest::Approx(4.067).epsilon(0.001));
    CHECK(bearing(0, 0, 0, 10) == doctest::Approx(127.875).epsilon(0.001));
}

TEST_CASE("UseSkill: bash needs a shield, taunt needs an NPC in melee range") {
    Fixture f;
    f.sheet.skills[skill::kBash] = 5;
    f.sheet.skills[skill::kTaunt] = 5;
    SkillState st;
    SkillContext c = f.context(1000);
    c.target = Fixture::rat(5.0f);
    SkillResult bash = use_skill(c, st, skill::kBash);
    REQUIRE(bash.messages.size() == 1);
    CHECK(bash.messages[0].eqstr == 12394);
    CHECK(bash.sends.empty());

    SkillResult taunt = use_skill(c, st, skill::kTaunt);
    REQUIRE(taunt.sends.size() == 1);
    CHECK(taunt.sends[0].kind == SkillSend::Kind::Taunt);
    CHECK(taunt.messages[0].eqstr == 12397);
    CHECK(taunt.messages[0].args == std::vector<std::string>{"a large rat"});
    CHECK_FALSE(st.timers.is_available(skill::kTaunt));
    // The timer is the raw reuse (no haste for non-combat skills): usable again after 6000 ms.
    st.timers.update(f.ch, 6999, false, false);
    CHECK_FALSE(st.timers.is_available(skill::kTaunt));
    st.timers.update(f.ch, 7001, false, false);
    CHECK(st.timers.is_available(skill::kTaunt));

    SkillState st2;
    c.target = Fixture::rat(40.0f);
    SkillResult far = use_skill(c, st2, skill::kTaunt);
    REQUIRE(far.messages.size() == 1);
    CHECK(far.messages[0].eqstr == 386);
    CHECK(st2.timers.is_available(skill::kTaunt)); // a failed check gives the skill back at once
}

TEST_CASE("UseSkill: the hide-moved check on a corrupt position: __ftol2's INT_MIN, a 32-bit z difference") {
    Fixture f;
    SkillContext c = f.context(1000);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr std::int32_t kMin = std::numeric_limits<std::int32_t>::min();
    // Hidden where the position was NaN (save_position stored INT_MIN three times): still NaN, so not moved.
    SkillState st;
    st.hidden = true;
    st.hide_x = st.hide_y = st.hide_z = kMin;
    c.me.x = c.me.y = c.me.z = nan;
    CHECK_FALSE(check_hide_moved(c, st, 1));
    CHECK(st.hidden);
    // The same x and y, z back at 100: INT_MIN - 100 wraps to 0x7fffff9c, which is more than 5.
    c.me.z = 100.0f;
    CHECK(check_hide_moved(c, st, 1));
    // Hidden at z 0, z now past the int range (INT_MIN): 0 - INT_MIN has the magnitude INT_MIN, not above 5.
    SkillState st2;
    st2.hidden = true;
    c.me.x = c.me.y = 0.0f;
    c.me.z = 3.0e9f;
    CHECK_FALSE(check_hide_moved(c, st2, 1));
    // In range it is the plain truncation and abs: 5.9 truncates to 5 (not moved), -6.5 to -6 (moved).
    c.me.z = 5.9f;
    CHECK_FALSE(check_hide_moved(c, st2, 1));
    c.me.z = -6.5f;
    CHECK(check_hide_moved(c, st2, 1));
}

TEST_CASE("UseSkill: sneak and hide toggles") {
    Fixture f;
    f.sheet.class_id = 9;
    f.sheet.skills[skill::kSneak] = 10;
    f.sheet.skills[skill::kHide] = 10;
    SkillState st;
    SkillContext c = f.context(1000);
    SkillResult on = use_skill(c, st, skill::kSneak);
    REQUIRE(on.sends.size() == 1);
    CHECK(on.sends[0].kind == SkillSend::Kind::Sneak);
    CHECK(on.messages[0].eqstr == 12422); // You begin to sneak...
    CHECK_FALSE(st.sneaking);             // the server's SpawnAppearance 15 sets it
    st.sneaking = true;
    SkillResult off = use_skill(c, st, skill::kSneak); // no timer check while sneaking
    REQUIRE(off.sends.size() == 1);
    CHECK(off.sends[0].kind == SkillSend::Kind::StopSneak);
    CHECK(off.messages[0].eqstr == 12338);

    SkillResult hide = use_skill(c, st, skill::kHide);
    REQUIRE(hide.sends.size() == 1);
    CHECK(hide.sends[0].kind == SkillSend::Kind::Hide);
    CHECK(st.hidden);
    CHECK(hide.messages[0].eqstr == 12425);
    // Moving ends it.
    c.me.y = 3.0f;
    auto moved = check_hide_moved(c, st, 1);
    REQUIRE(moved);
    CHECK(moved->eqstr == 12682);
    CHECK_FALSE(st.hidden);

    SkillState st2;
    c.speed_y = 1.0f; // moving: not still enough to hide
    SkillResult fail = use_skill(c, st2, skill::kHide);
    REQUIRE(fail.messages.size() == 1);
    CHECK(fail.messages[0].eqstr == 12424);
}

TEST_CASE("UseSkill: sense heading and the no-skill gate") {
    Fixture f;
    SkillState st;
    SkillContext c = f.context(1000);
    CHECK(use_skill(c, st, skill::kSenseHeading).messages.empty()); // skill 0: nothing
    f.sheet.skills[skill::kSenseHeading] = 50;
    c.me.heading = 128; // West? no: 97..160 is "West" in the original's table
    SkillResult r = use_skill(c, st, skill::kSenseHeading);
    REQUIRE(r.messages.size() == 1);
    CHECK(r.messages[0].eqstr == 12435);
    CHECK(r.messages[0].args == std::vector<std::string>{"@12433"});
    REQUIRE(r.sends.size() == 1);
    CHECK(r.sends[0].kind == SkillSend::Kind::SenseHeading);
    CHECK(r.compass_reset); // 0x65b650 on the compass window
    // the message reads the true heading (me +0x80), in bands of 64 from 0x45c8b6: 225..288 is South (12431), as
    // 256 is due south
    for (auto [heading, id] : {std::pair{225.0f, 12431}, {288.9f, 12431}, {289.0f, 12430}, {224.9f, 12432},
                               {32.9f, 12427}, {33.0f, 12434}, {480.0f, 12427}, {479.0f, 12428}}) {
        c.me.heading = heading;
        SkillState fresh; // (the reuse timer)
        SkillResult h = use_skill(c, fresh, skill::kSenseHeading);
        REQUIRE(h.messages.size() == 1);
        CHECK(h.messages[0].args == std::vector<std::string>{"@" + std::to_string(id)});
    }
    c.compass_window = false;
    SkillState fresh;
    CHECK_FALSE(use_skill(c, fresh, skill::kSenseHeading).compass_reset);
}

TEST_CASE("key map: the original's defaults, names and ini round trip") {
    KeyMap km;
    km.reset_defaults(true);
    CHECK(km.binding(static_cast<int>(KeyCommand::FORWARD), 0).code == 0x11);  // W
    CHECK(km.binding(static_cast<int>(KeyCommand::FORWARD), 1).code == 0xc8);  // up arrow
    CHECK(km.binding(static_cast<int>(KeyCommand::AUTOPRIM), 0).code == 0x10); // Q
    CHECK(km.binding(static_cast<int>(KeyCommand::HOT1_1), 0).code == 0x02);   // 1
    CHECK(km.binding(static_cast<int>(KeyCommand::HOT1_12), 0).code == 0x0d);  // =
    // Hailstone: Tab is TARGETNPC (nearest NPC), F8 its second key; TOGGLETARGET has no key.
    CHECK(km.binding(static_cast<int>(KeyCommand::TARGETNPC), 0).code == 0x0f);
    CHECK(km.binding(static_cast<int>(KeyCommand::TARGETNPC), 1).code == 0x42);
    CHECK_FALSE(km.binding(static_cast<int>(KeyCommand::TOGGLETARGET), 0).bound());
    // Hailstone has no voice macros or audio triggers: the original's Ctrl+V (CMD_TOGGLEVOICEWIN) is unbound and
    // neither window's command is in the keys list.
    for (KeyCommand c : {KeyCommand::CMD_TOGGLEVOICEWIN, KeyCommand::CMD_TOGGLE_AUDIO_TRIGGER_WINDOW}) {
        int i = static_cast<int>(c);
        CHECK(key_command_dropped(i));
        CHECK_FALSE(km.binding(i, 0).bound());
        CHECK_FALSE(km.binding(i, 1).bound());
        CHECK(std::find(listed_commands().begin(), listed_commands().end(), i) == listed_commands().end());
    }
    // Daybreak's services are left out too: the claim, polls, alerts and social share keys lose their default keys
    // (Shift+C, Shift+V, Shift+H, Ctrl+Shift+S), and so do real estate's leave key and the customer service key (P);
    // the welcome screen's key has none.
    for (KeyCommand c : {KeyCommand::CMD_TOGGLE_CLAIM_WIN, KeyCommand::CMD_VOTE_RESULTS, KeyCommand::CMD_HISTORIC_ALERTS,
                         KeyCommand::CMD_SOCIAL_SHARE, KeyCommand::CMD_MARKETPLACE_GOLD,
                         KeyCommand::CMD_REAL_ESTATE_LEAVE,
                         KeyCommand::CMD_TOGGLE_TICKET_WND, KeyCommand::CMD_WELCOMESCREEN}) {
        int i = static_cast<int>(c);
        CHECK(key_command_dropped(i));
        CHECK_FALSE(km.binding(i, 0).bound());
        CHECK(std::find(listed_commands().begin(), listed_commands().end(), i) == listed_commands().end());
    }
    // The Marketplace window's key stays (no default key in the original): the window speaks the original's opcodes.
    CHECK_FALSE(key_command_dropped(static_cast<int>(KeyCommand::CMD_MARKETPLACE)));
    CHECK_FALSE(key_command_dropped(static_cast<int>(KeyCommand::TOGGLE_FELLOWSHIPWIN)));
    CHECK(std::find(listed_commands().begin(), listed_commands().end(), static_cast<int>(KeyCommand::TOGGLE_FELLOWSHIPWIN)) != listed_commands().end());
    CHECK_FALSE(km.binding(static_cast<int>(KeyCommand::CMD_MARKETPLACE), 0).bound());
    CHECK_FALSE(key_command_dropped(static_cast<int>(KeyCommand::CMD_MERCENARIES)));
    CHECK_FALSE(key_command_dropped(static_cast<int>(KeyCommand::CMD_ACHIEVEMENT_WINDOW)));
    CHECK(std::find(listed_commands().begin(), listed_commands().end(), static_cast<int>(KeyCommand::CMD_ACHIEVEMENT_WINDOW)) != listed_commands().end());
    // Select Reward has its own stock window; veteran /claim remains separate.
    const int reward = static_cast<int>(KeyCommand::CMD_TOGGLE_REWARD_SELECTION_WIN);
    CHECK_FALSE(key_command_dropped(reward));
    CHECK(std::find(listed_commands().begin(), listed_commands().end(), reward) != listed_commands().end());
    CHECK(command_category(reward) == 0x40u);
    {
        const KeyCombo key{0x58, true, true, false};
        REQUIRE(km.bind(reward, 0, key));
        std::vector<KeyMap::Event> events;
        km.key_down(key, events);
        REQUIRE(events.size() == 1);
        CHECK(events.front().command == reward);
        CHECK(events.front().down);
        events.clear();
        km.key_up(key.code, events);
        REQUIRE(events.size() == 1);
        CHECK(events.front().command == reward);
        CHECK_FALSE(events.front().down);
    }
    // Real estate's item placement keys are listed (placement mode is built).
    for (int i = static_cast<int>(KeyCommand::CMD_REAL_ESTATE_ITEM_PLACEMENT_TOGGLE_MODE);
         i <= static_cast<int>(KeyCommand::CMD_REAL_ESTATE_ITEM_PLACEMENT_ROTATE_ITEM_WHEEL_ROTATION_90_DEGREES); ++i) {
        CHECK_FALSE(key_command_dropped(i));
        CHECK(std::find(listed_commands().begin(), listed_commands().end(), i) != listed_commands().end());
    }
    CHECK_FALSE(key_command_dropped(static_cast<int>(KeyCommand::CMD_TOGGLE_AURAWND)));
    CHECK_FALSE(key_command_dropped(static_cast<int>(KeyCommand::CMD_TOGGLE_VOICE_BAR)));
    CHECK(std::find(listed_commands().begin(), listed_commands().end(), static_cast<int>(KeyCommand::FORWARD)) !=
          listed_commands().end());
    {
        std::vector<KeyMap::Event> ev;
        km.key_down(KeyCombo{0x0f}, ev);
        // CYCLEREPLY keeps Tab too, as in the original.
        CHECK(std::any_of(ev.begin(), ev.end(),
                          [](const KeyMap::Event& e) { return e.command == static_cast<int>(KeyCommand::TARGETNPC); }));
        CHECK(std::none_of(ev.begin(), ev.end(), [](const KeyMap::Event& e) {
            return e.command == static_cast<int>(KeyCommand::TOGGLETARGET);
        }));
        km.key_up(0x0f, ev);
    }
    KeyCombo sit = km.binding(static_cast<int>(KeyCommand::SIT_STAND), 0);
    CHECK(sit.ctrl);
    CHECK(sit.code == 0x1f);
    CHECK(key_text(sit) == "Ctrl S");
    CHECK(key_text(KeyCombo{0x4c, false, false, false}) == "NPAD 5");
    CHECK(key_text(KeyCombo{0x42, true, true, false}) == "ShCtl F8");
    CHECK(key_text(KeyCombo{}) == "-------");
    KeyMap arrows;
    arrows.reset_defaults(false);
    CHECK(arrows.binding(static_cast<int>(KeyCommand::FORWARD), 0).code == 0x48); // NPAD 8

    CHECK(parse_keymap_text("SHIFT+CONTROL+F1") == 0x6000003b);
    CHECK(parse_keymap_text("alt+q") == 0x10000010);
    CHECK(parse_keymap_text("*NONE*") == -2);
    CHECK(parse_keymap_text("NOKEY") == -1);
    CHECK(parse_keymap_text("BOGUS") == -1);
    CHECK(keymapping_key(static_cast<int>(KeyCommand::HOT1_1), 0) == "KEYMAPPING_HOT1_1_1");
    CHECK(is_reserved_key(KeyCombo{0x1c}));

    // Key presses: Ctrl+S sits, S alone moves back; release ends the command.
    std::vector<KeyMap::Event> ev;
    km.key_down(KeyCombo{0x1f, false, true, false}, ev);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].command == static_cast<int>(KeyCommand::SIT_STAND));
    ev.clear();
    km.key_up(0x1f, ev);
    REQUIRE(ev.size() == 1);
    CHECK_FALSE(ev[0].down);

    // Round trip: change two bindings, save, load into a fresh map.
    CHECK(km.bind(static_cast<int>(KeyCommand::HOT1_1), 0, KeyCombo{0x3b, false, false, true}));
    CHECK(km.bind(static_cast<int>(KeyCommand::JUMP), 0, KeyCombo{}));
    CHECK_FALSE(km.bind(static_cast<int>(KeyCommand::JUMP), 1, KeyCombo{0x2a})); // reserved
    Config cfg;
    cfg.set("KEYMAP_CONSIDER", "SHIFT+c"); // the legacy text form is read and replaced
    km.save(cfg);
    CHECK(cfg.get_int("KEYMAPPING_HOT1_1_1", 0) == 0x1000003b);
    CHECK(cfg.get_int("KEYMAPPING_JUMP_1", -5) == 0);
    CHECK_FALSE(cfg.get("KEYMAPPING_FORWARD_1")); // defaults are not written
    CHECK_FALSE(cfg.get("KEYMAP_CONSIDER"));
    KeyMap back;
    back.load(cfg);
    for (int c = 0; c < kKeyCommands; ++c)
        for (int b = 0; b < 2; ++b) CHECK(back.binding(c, b) == km.binding(c, b));
    Config legacy;
    legacy.set("KEYMAP_CONSIDER", "SHIFT+c");
    KeyMap fromLegacy;
    fromLegacy.load(legacy);
    CHECK(fromLegacy.binding(static_cast<int>(KeyCommand::CONSIDER), 0) == KeyCombo{0x2e, true, false, false});
}

TEST_CASE("key map: the combos bound to more than one listed command") {
    KeyMap km;
    km.reset_defaults(true);
    auto shared = [&](const KeyCombo& k) {
        auto v = km.shared_combos();
        return std::binary_search(v.begin(), v.end(), k.to_int());
    };
    const KeyCombo f9{0x43, true, true, true}; // ShCtlAlt F9: no default command has it
    CHECK_FALSE(shared(f9));
    CHECK(km.bind(static_cast<int>(KeyCommand::JUMP), 1, f9));
    CHECK_FALSE(shared(f9));
    CHECK(km.bind(static_cast<int>(KeyCommand::JUMP), 0, f9)); // one command on both banks is no clash
    CHECK_FALSE(shared(f9));
    CHECK(km.bind(static_cast<int>(KeyCommand::SIT_STAND), 1, f9));
    CHECK(shared(f9));
    CHECK(km.bind(static_cast<int>(KeyCommand::SIT_STAND), 1, KeyCombo{}));
    CHECK_FALSE(shared(f9));
    // A dropped command's key is not counted: the list cannot show or change it.
    CHECK(km.bind(static_cast<int>(KeyCommand::CMD_TOGGLEVOICEWIN), 0, f9));
    CHECK_FALSE(shared(f9));
    // Unbound keys are never a clash.
    const auto all = km.shared_combos();
    CHECK(std::find(all.begin(), all.end(), 0) == all.end());
}

TEST_CASE("hot buttons: the character ini format") {
    HotButton b = parse_hot_button("B0,@-1,0000000000000000,0,Melee Attack");
    CHECK(b.type == HotButtonType::WeaponSlot);
    CHECK(b.slot == 0);
    CHECK(b.icon_type == -1);
    CHECK(b.icon_slot == -1);
    CHECK(b.label == "Melee Attack");
    CHECK(format_hot_button(b) == "B0,@-1,0000000000000000,0,Melee Attack");
    HotButton item = parse_hot_button("M-1,@-1,0000000037008149,999990,");
    CHECK(item.type == HotButtonType::ItemLink);
    CHECK(item.slot == -1);
    CHECK(item.guid == "0000000037008149");
    CHECK(item.item_id == 999990);
    CHECK(item.label.empty());

    HotButtonStore store;
    store.at(0, 0, 0) = b;
    HotButton kick;
    kick.type = HotButtonType::CombatSkill;
    kick.slot = 30;
    store.at(1, 2, 3) = kick;
    store.social(13).name = "Pet ATK";
    store.social(13).lines[0] = "/pet attack";
    store.combat_skills[0] = 30;
    IniFile ini;
    store.save(ini);
    CHECK(ini.get_string("HotButtons", "Page1Button1") == "B0,@-1,0000000000000000,0,Melee Attack");
    CHECK(ini.get_string("HotButtons2", "Page3Button4") == "C30,@-1,0000000000000000,0,");
    CHECK(ini.get_string("Socials", "Page2Button2Name") == "Pet ATK");
    CHECK(ini.get_int("CombatSkills", "CombatSkill1", 0) == 30);
    IniFile reread;
    reread.parse(ini.to_string());
    HotButtonStore back;
    back.load(reread);
    CHECK(back.at(1, 2, 3).type == HotButtonType::CombatSkill);
    CHECK(back.at(1, 2, 3).slot == 30);
    CHECK(back.at(0, 0, 0).label == "Melee Attack");
    CHECK(back.at(0, 0, 1).empty());
    CHECK(back.social(13).lines[0] == "/pet attack");
    CHECK(back.combat_skills[0] == 30);
    CHECK(back.abilities[0] == -1);
}

TEST_CASE("Disarm while hidden drops the first buff of each invisibility SPA, then unhides") {
    Fixture f;
    f.sheet.skills[skill::kDisarm] = 20;
    SkillState st;
    st.hidden = true;
    SkillContext c = f.context(1000);
    c.target = Fixture::rat(10.0f);
    // slot 2: SPA 12 and 28 (one buff), slot 4: SPA 28, slot 5: SPA 312
    c.buff_slots_with_spa = [](std::int32_t spa) -> std::vector<int> {
        if (spa == 12) return {2};
        if (spa == 28) return {2, 4};
        if (spa == 312) return {5};
        return {};
    };
    SkillResult r = use_skill(c, st, skill::kDisarm);
    CHECK(r.removed_buffs == std::vector<int>{2, 4, 5});
    REQUIRE(r.sends.size() == 2);
    CHECK(r.sends[0].kind == SkillSend::Kind::Unhide);
    CHECK(r.sends[1].kind == SkillSend::Kind::Disarm);
    CHECK_FALSE(st.hidden);

    SkillState visible;
    SkillResult v = use_skill(c, visible, skill::kDisarm);
    CHECK(v.removed_buffs.empty()); // only while hidden
}

TEST_CASE("DoAttack: Sanctuary goes before the attack; the attack lock refuses; Slam needs the innate") {
    Fixture f;
    f.sheet.skills[skill::kKick] = 10;
    f.effects[{312, -1}] = 1;
    SkillState st;
    SkillContext c = f.context(1000000);
    c.target = Fixture::rat(10.0f);
    c.buff_slots_with_spa = [](std::int32_t spa) -> std::vector<int> { return spa == 312 ? std::vector<int>{1, 7} : std::vector<int>{}; };
    SkillResult r = use_skill(c, st, skill::kKick);
    CHECK(r.removed_buffs == std::vector<int>{1, 7});
    CHECK(r.sends.size() == 1);

    SkillState locked;
    c.attack_locked = true;
    CHECK(use_skill(c, locked, skill::kKick).sends.empty());

    CHECK(slam_innate(9) == 0);  // Troll
    CHECK(slam_innate(93) == 0);
    CHECK(slam_innate(1) == 0xff); // Human
}

TEST_CASE("InitInnates: the race's infravision and ultravision, the class's slots") {
    auto vision = [](std::uint32_t race) {
        auto s = init_innates(race, 1);
        return (s[kInnateInfravision] == 0 ? 1 : 0) | (s[kInnateUltravision] == 0 ? 2 : 0);
    };
    CHECK(vision(1) == 0);   // Human
    CHECK(vision(2) == 0);   // Barbarian
    CHECK(vision(3) == 0);   // Erudite
    CHECK(vision(4) == 1);   // Wood Elf
    CHECK(vision(5) == 1);   // High Elf
    CHECK(vision(6) == 2);   // Dark Elf
    CHECK(vision(7) == 1);   // Half Elf (the default)
    CHECK(vision(8) == 1);   // Dwarf
    CHECK(vision(9) == 1);   // Troll
    CHECK(vision(10) == 1);  // Ogre
    CHECK(vision(11) == 1);  // Halfling
    CHECK(vision(12) == 1);  // Gnome
    CHECK(vision(128) == 1); // Iksar
    CHECK(vision(130) == 1); // Vah Shir
    CHECK(vision(330) == 2); // Froglok
    CHECK(vision(367) == 2);
    CHECK(vision(400) == 1);
    auto troll = init_innates(9, 1);
    CHECK(troll[10] == 0);
    CHECK(troll[14] == 0);
    CHECK(troll[15] == 0);
    CHECK(troll[0] == 0xff);
    CHECK(init_innates(1, 7)[12] == 0); // Monk
    CHECK(init_innates(1, 7)[1] == 0);
    CHECK(init_innates(1, 9)[16] == 0); // Rogue
    CHECK(init_innates(1, 6)[4] == 0);  // Druid
    CHECK(init_innates(1, 2)[1] == 0xff);
}

TEST_CASE("CanIHit: AllowedToAttack decides when given") {
    Fixture f;
    f.sheet.skills[skill::kKick] = 10;
    SkillState st;
    SkillContext c = f.context(1000000);
    c.target = Fixture::rat(10.0f);
    c.allowed_to_attack = false;
    CHECK(use_skill(c, st, skill::kKick).sends.empty());
}

TEST_CASE("UseSkill port through the context: feign death's packet bytes, stand state and attack off") {
    Fixture f;
    f.sheet.skills[skill::kFeignDeath] = 30;
    SkillState st;
    SkillContext c = f.context(1000);
    SkillResult r = use_skill(c, st, skill::kFeignDeath);
    CHECK(r.stand_state == std::optional<std::uint8_t>(115));
    CHECK(r.stop_attack);
    REQUIRE(r.sends.size() == 1);
    CHECK(r.sends[0].kind == SkillSend::Kind::FeignDeath);
    auto p = net::encode_skill_send(r.sends[0]);
    REQUIRE(p);
    CHECK(p->opcode == 0x52fa);
    // {own id, 0xddf708 (0), a byte flag 0 sent as a dword}: the bytes encode_feign_death builds.
    CHECK(bytes(*p) == bytes(net::encode_feign_death(100)));
    CHECK_FALSE(st.timers.is_available(skill::kFeignDeath));
}
