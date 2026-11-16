// The player's own HP (net::GameConnection::own_hp, set by app/stats_ui.cpp share_own_hp_items): EQEmu's OP_HPUpdate
// for the player leaves out the items' HP (SendHPUpdate: GetHP() - itembonuses.HP), which Cur_HP / Max_HP add back
// from the character's item sums; a server extension's vitals are already the whole value. The windows read it here;
// Me.CurrentHPs (mq_tlo_test), hailstone.player (lua_plugin_test) and HailstonePlayer (native_extension_test) read the same.
// Against the mock server.
#include <doctest/doctest.h>

#include <functional>

#include "app/lua_ui.h"
#include "app/lua_ui_values.h"
#include "app/stats_ui.h"
#include "game/mechanics/character.h"
#include "game/mechanics/tables.h"
#include "mock_server/harness.h"
#include "net/zone_packets.h"

using namespace hailstone;
using namespace hailstone::test;
namespace mech = hailstone::game::mechanics;

namespace {

constexpr const char* kBaseData = "60^1^400^0^300^3^6^0.9^0^1.25\n";
constexpr const char* kSkillCaps = "1^15^60^210^0\n";

// A level 60 warrior wearing one item of `item_hp` HP (worn slot 2).
mech::CharacterSheet warrior_with(std::int32_t item_hp) {
    mech::CharacterSheet s;
    s.race = 1;
    s.class_id = 1;
    s.level = s.highest_level = 60;
    s.base_str = s.base_sta = s.base_cha = s.base_dex = s.base_int = s.base_agi = s.base_wis = 75;
    s.stored_hp = 100000;
    s.buffs.resize(mech::kPlayerBuffSlots);
    net::Item helm;
    helm.races = helm.classes = 0xffff;
    helm.hp = item_hp;
    s.possessions[2] = helm;
    return s;
}

} // namespace

TEST_CASE("Own HP: the windows add the items' HP that EQEmu's HP update leaves out, unless an extension gives it") {
    EmptyGameDir dir;
    GameDir game(dir.path());
    MockEqServer server(MockConfig::standard());
    net::GameConnection conn(game, quick_options());
    auto until = [&](const std::function<bool()>& done) {
        return pump([&] { conn.update(); server.poll(); }, done);
    };
    conn.start(server.ticket());
    REQUIRE(until([&] { return conn.state() == net::ConnectionState::CharacterSelect; }));
    conn.enter_world("Gennro");
    REQUIRE(until([&] { return conn.state() == net::ConnectionState::InZone && conn.player_status().max_hp != 0; }));

    mech::GameTables tables;
    tables.class_levels.parse(kBaseData);
    tables.skill_caps.parse(kSkillCaps);
    mech::SpellLookup no_spells = [](std::int32_t) -> const game::SpellData* { return nullptr; };
    mech::CharacterSheet sheet = warrior_with(262); // Character keeps a reference to its sheet
    mech::Character ch(sheet, tables, no_spells);
    ch.process_all_stats();
    REQUIRE(ch.stats().hp_bonus == 262);

    // The server's points without the item: 100 of 200.
    net::HpUpdate u;
    u.spawn_id = static_cast<std::int16_t>(conn.world().player_id);
    u.cur_hp = 100;
    u.max_hp = 200;
    conn.receive_zone_packet_for_test(net::encode(u));

    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    // Before the player's sheet: the server's points as they came.
    CHECK(app::ui_label(host, 17).text == "100");
    CHECK(app::ui_label(host, 18).text == "200");

    host.character = &ch;
    app::share_own_hp_items(conn, &ch); // as StatsUi::draw does each in-zone frame
    CHECK(app::ui_label(host, 17).text == "362"); // Cur_HP
    CHECK(app::ui_label(host, 18).text == "462"); // Max_HP
    CHECK(app::ui_label(host, 19).text == "78");  // 362 / 462
    CHECK(app::ui_label(host, 70).text == "362/462");
    CHECK(app::ui_gauge(host, 1).value == 780);   // the HP gauge: the percent times 10

    // A server extension's vitals (the NMS and Project Triune plugins' set_vitals) are the whole value: nothing added.
    mech::CharacterSheet given = warrior_with(262);
    given.vitals.cur_hp = 100;
    given.vitals.max_hp = 200;
    mech::Character ext(given, tables, no_spells);
    ext.process_all_stats();
    host.character = &ext;
    app::share_own_hp_items(conn, &ext);
    CHECK(app::ui_label(host, 17).text == "100");
    CHECK(app::ui_label(host, 18).text == "200");

    // Leaving the zone (no character): the server's points as they came.
    app::share_own_hp_items(conn, nullptr);
    CHECK(conn.own_hp().cur == 100);
    CHECK(conn.own_hp().max == 200);
}
