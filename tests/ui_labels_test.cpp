// Labels and gauges by EQType (app/lua_ui_values.cpp)
// fed by packets: the combat timer, the aggro meter's percents and colours, the gauge's special values and the buff
// windows' fade (game/buff_overlays.h).
#include <doctest/doctest.h>

#include <fstream>
#include <memory>
#include <vector>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_values.h"
#include "app/lua_ui_windows.h"
#include "game/buff_overlays.h"
#include "game/mechanics/character.h"
#include "game_files.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/aggro_packets.h"
#include "net/game_connection.h"
#include "net/group_packets.h"
#include "net/opcodes.h"
#include "net/spell_packets.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {

struct Host {
    net::GameConnection conn;
    app::UiHost host;
    explicit Host(const GameDir& game) : conn(game, {}) {
        host.in_game = true;
        host.game = &game;
        host.conn = &conn;
    }
};

// OP_RestState: u8 in combat, then out of combat the downtime in seconds.
net::AppPacket rest_state(bool combat, std::uint32_t seconds) {
    std::vector<std::byte> b{std::byte{combat ? std::uint8_t{1} : std::uint8_t{0}}};
    if (!combat)
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::byte>((seconds >> (8 * i)) & 0xff));
    return net::AppPacket(net::to_wire(net::Op::RestState), std::move(b));
}

} // namespace

TEST_CASE("Gauge fill: CGaugeWnd::Draw's special values") {
    CHECK(app::gauge_fill(0).fill == 0);
    CHECK(app::gauge_fill(640).fill == 640);
    CHECK_FALSE(app::gauge_fill(640).tint);
    CHECK(app::gauge_fill(-1).fill < 0); // nothing
    CHECK(app::gauge_fill(-3).fill < 0); // a corpse: empty
    CHECK(app::gauge_fill(-9).fill < 0);
    auto red = app::gauge_fill(-2); // full, dark red
    CHECK(red.fill == 1000);
    CHECK(red.tint == 0xff800000u);
    CHECK(app::gauge_fill(-110).fill == 0); // the purple band: (v + 110) * 10
    CHECK(app::gauge_fill(-60).fill == 500);
    CHECK(app::gauge_fill(-10).fill == 1000);
    CHECK(app::gauge_fill(-60).tint == 0xff800080u);
    CHECK(app::gauge_fill(-2000).fill == 0); // the pink band: v + 2000
    CHECK(app::gauge_fill(-1250).fill == 750);
    CHECK(app::gauge_fill(-1000).tint == 0xffc08080u);
    CHECK(app::gauge_fill(-500).fill < 0); // between the bands: empty
}

TEST_CASE("Buff fade: under 19 s, once a second; a bard's short song keeps its alpha") {
    CHECK(game::buff_fade(19000, 123456, false) == 1.0); // 19 s left: not yet
    CHECK(game::buff_fade(18999, 5000, false) == 1.0);   // the second's start
    CHECK(game::buff_fade(18999, 5500, false) == doctest::Approx(0.5));
    CHECK(game::buff_fade(1, 5999, false) == doctest::Approx(0.01));
    CHECK(game::buff_fade(0, 5500, false) == 1.0);  // run out
    CHECK(game::buff_fade(-1, 5500, false) == 1.0); // permanent
    CHECK(game::buff_fade(5000, 5500, true) == 1.0);
}

TEST_CASE("Combat timer: label 137 and gauge 29 from OP_RestState") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Host t(*game);
    CHECK(app::ui_label(t.host, 137).text.empty());
    CHECK(app::ui_gauge(t.host, 29).value == -1);
    t.conn.receive_zone_packet_for_test(rest_state(false, 75));
    auto text = app::ui_label(t.host, 137).text;
    CHECK((text == "01:15" || text == "01:14")); // "%02d:%02d" of the seconds left
    CHECK(app::ui_gauge(t.host, 29).value == 1000); // min(left, 30) * 1000 / 30
    t.conn.receive_zone_packet_for_test(rest_state(false, 15));
    int v = app::ui_gauge(t.host, 29).value;
    CHECK((v == 500 || v == 466));
    t.conn.receive_zone_packet_for_test(rest_state(true, 0)); // in combat: no downtime
    CHECK(app::ui_label(t.host, 137).text.empty());
    CHECK(app::ui_gauge(t.host, 29).value == -1);
}

TEST_CASE("Discipline timer: gauge 26 counts down between ticks with whole seconds") {
    test::EmptyGameDir dir;
    std::ofstream(dir.path() / "spells_us.txt")
        << mq_test::spell_line({{0, "4498"}, {1, "Test Discipline"}, {16, "1"}, {17, "10"}, {83, "1"}, {200, "1"}})
        << '\n'
        << mq_test::spell_line({{0, "4499"}, {1, "Permanent Discipline"}, {16, "50"}, {83, "1"}}) << '\n'
        << mq_test::spell_line({{0, "4500"}, {1, "Zero Duration Discipline"}, {83, "1"}, {200, "1"}}) << '\n';
    GameDir game(dir.path());
    Host t(game);
    game::mechanics::CharacterSheet sheet;
    sheet.level = 50;
    sheet.class_id = 1;
    game::mechanics::GameTables tables;
    game::mechanics::Character character(sheet, tables,
        [&](std::uint32_t id) { return t.conn.spell_table().get(id); });
    t.host.character = &character;
    auto& state = net::GameConnectionTestAccess::spells(t.conn);
    auto& b = state.buffs.back();
    CHECK(app::ui_discipline_gauge(t.host, 1000).value == 0);
    CHECK(app::ui_discipline_gauge(t.host, 1000).text.empty());
    b.spell = 4498;
    b.tics = 10;
    b.received_ms = 1000;
    b.level = 1; // The native denominator uses the player's level, rather than the caster level.
    net::GameConnectionTestAccess::status(t.conn).level = 50;
    auto full = app::ui_discipline_gauge(t.host, 1000);
    CHECK(full.value == 1000);
    CHECK(full.text == "1:00");
    auto partial = app::ui_discipline_gauge(t.host, 1250);
    CHECK(partial.value < full.value);
    CHECK(partial.value > 990);
    CHECK(partial.text == "1:00");
    CHECK(app::ui_discipline_gauge(t.host, 31000).value == 500);
    CHECK(app::ui_discipline_gauge(t.host, 31000).text == "30s");
    CHECK(app::ui_discipline_gauge(t.host, 31001).value == 499);
    CHECK(app::ui_discipline_gauge(t.host, 31001).text == "30s");
    CHECK(app::ui_discipline_gauge(t.host, 60001).text == "1s");
    CHECK(app::ui_discipline_gauge(t.host, 61000).value == 0);
    CHECK(app::ui_discipline_gauge(t.host, 61000).text == "0s");
    CHECK(app::ui_discipline_gauge(t.host, 100000).value == 0);

    const auto before_tick = app::ui_discipline_gauge(t.host, 11000);
    b.tics = 9;
    b.received_ms = 7000;
    CHECK(app::ui_discipline_gauge(t.host, 11000).value == before_tick.value);
    CHECK(app::ui_discipline_gauge(t.host, 11000).text == before_tick.text);

    state.buff_timers_on_hold = true;
    CHECK(app::ui_discipline_gauge(t.host, 100000).value == 900);
    CHECK(app::ui_discipline_gauge(t.host, 100000).text == "54s");
    b.tics = 5;
    // Both the EQType and the window-specific child gauge expose the same frozen countdown.
    CHECK(app::ui_gauge(t.host, 26).value == 500);
    CHECK(app::ui_gauge(t.host, 26).text == "30s");
    t.host.character = nullptr;
    CHECK(app::ui_discipline_gauge(t.host, 100000).value == 0);
    CHECK(app::ui_discipline_gauge(t.host, 100000).text == "30s");
    t.host.character = &character;
    app::UiApi api(t.host);
    std::unique_ptr<lua_State, decltype(&lua_close)> lua(luaL_newstate(), lua_close);
    REQUIRE(lua);
    lua_newtable(lua.get());
    api.register_functions(lua.get());
    lua_setglobal(lua.get(), "hs");
    REQUIRE(luaL_dostring(lua.get(), "return hs.ChildGauge('CombatAbilityWnd', 'CAW_CombatEffectTimeRemainingGauge')") == LUA_OK);
    CHECK(lua_tonumber(lua.get(), -2) == doctest::Approx(0.5));
    CHECK(std::string(lua_tostring(lua.get(), -1)) == "30s");

    b.spell = 4499;
    CHECK(app::ui_discipline_gauge(t.host, 100000).value == 1000);
    CHECK(app::ui_discipline_gauge(t.host, 100000).text == "Permanent");
    b.spell = 4498;
    b.tics = -1;
    CHECK(app::ui_discipline_gauge(t.host, 100000).text == "Permanent");
    b.tics = 20;
    CHECK(app::ui_discipline_gauge(t.host, 100000).value == 1000); // A server extension can exceed the base duration.
    b.spell = 4500;
    CHECK(app::ui_discipline_gauge(t.host, 100000).value == 0); // No division by an unknown or zero duration.
    CHECK(app::ui_discipline_gauge(t.host, 100000).text == "2:00");
    b.spell = 65500;
    CHECK(app::ui_discipline_gauge(t.host, 100000).value == 0);
    CHECK(app::ui_discipline_gauge(t.host, 100000).text.empty());
    b.spell = 4498;
    b.tics = 0;
    CHECK(app::ui_discipline_gauge(t.host, 100000).value == 0);
    CHECK(app::ui_discipline_gauge(t.host, 100000).text == "0s");

    net::BuffPacket fade;
    fade.slot = game::kBuffSlots - 1;
    fade.fade = 1;
    t.conn.receive_zone_packet_for_test(net::encode(fade));
    CHECK_FALSE(t.conn.discipline_active());
    CHECK(app::ui_discipline_gauge(t.host, 100000).value == 0);
    CHECK(app::ui_discipline_gauge(t.host, 100000).text.empty());
}

TEST_CASE("Aggro meter labels: percents, their colours and the lock") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    Host t(*game);
    net::AggroTargetInfo info{0, 77};
    t.conn.receive_zone_packet_for_test(net::encode(info));
    net::AggroUpdate u;
    u.entries = {{0, 80}, {1, 100}, {2, 0}, {8, 40}};
    t.conn.receive_zone_packet_for_test(net::encode(u));
    auto mine = app::ui_label(t.host, 305);
    CHECK(mine.text == "80");
    REQUIRE(mine.argb);
    CHECK(*mine.argb == (0xff000000u | (app::user_color_rgb(0x61) ? app::user_color_rgb(0x61) : 0xf0f0f0u)));
    CHECK(app::ui_label(t.host, 306).text == "80");
    CHECK(app::ui_label(t.host, 307).text == "100");
    CHECK(app::ui_label(t.host, 315).text == "40"); // extended target slot 1: entry 8
    CHECK(app::ui_label(t.host, 314).text.empty());  // below 1: empty
    CHECK(app::ui_gauge(t.host, 149).value == 800);
    // locked on another spawn: the player window's copies (304, 306, 308) go blank
    info.lock_id = 55;
    t.conn.receive_zone_packet_for_test(net::encode(info));
    CHECK(app::ui_label(t.host, 306).text.empty());
    CHECK(app::ui_label(t.host, 308).text.empty());
    CHECK(app::ui_label(t.host, 305).text == "80");
}

// The group window's labels find each member's spawn by name, ignoring case, by the spawn's name or its display name
// (app/lua_ui_values.cpp spawn_by_name), and skip the player's own slot whatever its case (nth_other_member). The
// lookup compares in place (game::display_name_iequals) instead of lower-casing every name each frame.
TEST_CASE("Group labels: members are found by name or display name, ignoring case") {
    test::ScratchDir dir("ui_labels_group_lookup");
    GameDir game(dir.path());
    Host t(game);
    net::PlayerProfile pp;
    pp.name = "Leader";
    t.conn.set_profile_for_test(pp);
    net::GroupUpdateB group;
    group.leader = "Leader";
    const char* names[] = {"leader", "Bob", "Carol", "Dave", "Eve"};
    for (std::uint32_t i = 0; i < 5; ++i) {
        net::GroupUpdateMember m;
        m.name = names[i];
        m.slot = i;
        group.members.push_back(m);
    }
    t.conn.receive_zone_packet_for_test(net::encode(group));
    auto add = [&](std::uint32_t id, const char* name, std::uint8_t hp) {
        game::Spawn s;
        s.id = id;
        s.name = name;
        s.hp_percent = hp;
        t.conn.world_for_test().spawns[id] = s;
    };
    add(2, "BOB", 40);       // the name, another case
    add(3, "carol00", 50);   // the display name ("carol")
    add(4, "#Dave_01", 60);  // "Dave " is not "Dave"
    add(5, "a_rat01", 70);   // not a member
    CHECK(app::ui_label(t.host, 30).text == "Bob"); // "leader" is the player's slot
    CHECK(app::ui_label(t.host, 35).text == "40");
    CHECK(app::ui_label(t.host, 36).text == "50");
    CHECK(app::ui_label(t.host, 37).text == "0");
    CHECK(app::ui_label(t.host, 38).text == "0");
    CHECK(app::ui_gauge(t.host, 11).value == 400);
    CHECK(app::ui_gauge(t.host, 12).value == 500);
    CHECK(app::ui_gauge(t.host, 13).value == -2); // no spawn: full in dark red
    add(6, "#Dave01", 65);
    CHECK(app::ui_label(t.host, 37).text == "65");
}

TEST_CASE("display_name_iequals is iequals over display_name") {
    const char* names[] = {"a_rat01", "A_RAT", "#Guard_Bob00", "#", "##", "", "123", "#12", "Bob_", "bob ", "Bob",
                           "_x_9_", "x9y", "Lord_Bob00"};
    const char* others[] = {"a rat", "A RAT", "guard bob", "", "bob", "Bob ", "x9y", "x9", " x ", "lord bob", "#bob",
                            "a_rat", "123"};
    for (const char* n : names)
        for (const char* o : others) {
            CAPTURE(n);
            CAPTURE(o);
            CHECK(game::display_name_iequals(n, o) == iequals(game::display_name(n), o));
        }
}
