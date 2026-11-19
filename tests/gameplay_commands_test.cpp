// The gameplay slash commands of net/game_connection_game_commands.cpp and net/game_connection_player.cpp:
// /targetgroupbuff (/tgb), /spellscribe, /offlinemode, /aggrometer, /aggrolock, /claim and /bugreport, against the
// mock EQEmu, and /spellscribe's packet.

#include <doctest/doctest.h>

#include <cstring>

#include "app/actions_ui.h"
#include "app/lua_ui.h"
#include "core/config.h"
#include "core/ini_file.h"
#include "game/client_options.h"
#include "game/mechanics/abilities.h"
#include "game/mechanics/character.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/player_packets.h"
#include "net/zone_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace mech = hailstone::game::mechanics;

namespace {

std::vector<std::uint32_t> tgb_values(const std::vector<AppPacket>& log) {
    std::vector<std::uint32_t> out;
    for (const auto& p : log)
        if (from_wire(p.opcode) == Op::TGB && p.data.size() == 4) {
            std::uint32_t v = 0;
            std::memcpy(&v, p.data.data(), 4);
            out.push_back(v);
        }
    return out;
}

int raw_count(const std::vector<AppPacket>& log, std::uint16_t opcode) {
    int n = 0;
    for (const auto& p : log) n += p.opcode == opcode;
    return n;
}

// UseSkill's Forage packet (0x5306): no target, no facing, so a command test can check the skill it used.
constexpr std::uint16_t kOpForage = 0x5306;

} // namespace

TEST_CASE("/spellscribe's packet: our name over the target's, 4 at +0x80") {
    AppPacket p = encode_spell_scribe("Gennro", "a_large_rat00");
    CHECK(p.opcode == 0x51e5);
    REQUIRE(p.data.size() == kSpellScribeSize);
    auto at = [&](std::size_t k) { return static_cast<char>(p.data[k]); };
    CHECK(std::string(reinterpret_cast<const char*>(p.data.data())) == "Gennro");
    CHECK(std::string(reinterpret_cast<const char*>(p.data.data()) + 0x40) == "Gennro");
    // strcpy over the longer target name: its tail stays past our NUL.
    CHECK(at(0x46) == '\0');
    CHECK(at(0x47) == '_');
    CHECK(at(0x48) == 'r');
    CHECK(at(0x4c) == '0');
    CHECK(at(0x80) == 4);
    for (std::size_t k = 0x81; k < kSpellScribeSize; ++k) CHECK(at(k) == 0);
}

TEST_CASE("client options: TargetGroupBuff is unset until written, then its atoi") {
    game::ClientOptions opts;
    CHECK(opts.get(game::ClientOption::TargetGroupBuff) == game::kTargetGroupBuffUnset);
    IniFile ini;
    ini.set("Defaults", "TargetGroupBuff", "*NULL*");
    opts.load(ini);
    CHECK(opts.get(game::ClientOption::TargetGroupBuff) == game::kTargetGroupBuffUnset);
    ini.set("Defaults", "TargetGroupBuff", "1");
    opts.load(ini);
    CHECK(opts.get(game::ClientOption::TargetGroupBuff) == 1);
    ini.set("Defaults", "TargetGroupBuff", "x"); // atoi
    opts.load(ini);
    CHECK(opts.get(game::ClientOption::TargetGroupBuff) == 0);
    opts.set(game::ClientOption::TargetGroupBuff, 1);
    IniFile out;
    opts.store(game::ClientOption::TargetGroupBuff, out);
    CHECK(out.get("Defaults", "TargetGroupBuff") == "1");
}

TEST_CASE("mock server: /tgb, /spellscribe, /offlinemode and the window commands") {
    MockConfig config = MockConfig::standard();
    config.log_server_spell_scribe = 1;
    Rig rig(config);
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    rig.chat();
    // The zone-in sends the kept setting, if any (the character's options.ini lives on between test runs).
    REQUIRE(rig.until([&] { return conn.player_status().max_hp != 0; }));
    const std::size_t base = tgb_values(server.zone_log).size();
    auto sent = [&] {
        auto v = tgb_values(server.zone_log);
        return std::vector<std::uint32_t>(v.begin() + static_cast<std::ptrdiff_t>(base), v.end());
    };

    // /tgb on: 1 then the query 2; /targetgroupbuff off: 0, 2; anything else (and "of") only asks.
    conn.send_command("/tgb on");
    REQUIRE(rig.until([&] { return sent().size() == 2; }));
    CHECK(sent() == std::vector<std::uint32_t>{1, 2});
    CHECK(conn.client_options().get(game::ClientOption::TargetGroupBuff) == 1);
    conn.send_command("/targetgroupbuff OFF");
    conn.send_command("/tgb of");
    conn.send_command("/tgb");
    REQUIRE(rig.until([&] { return sent().size() == 6; }));
    CHECK(sent() == std::vector<std::uint32_t>{1, 2, 0, 2, 2, 2});
    CHECK(conn.client_options().get(game::ClientOption::TargetGroupBuff) == 0);

    // /spellscribe with OP_LogServer +0x18 on: 0x51e5 with our name twice (no target).
    conn.send_command("/spellscribe a 1 2");
    REQUIRE(rig.until([&] { return raw_count(server.zone_log, kOpSpellScribe) == 1; }));
    for (const auto& p : server.zone_log)
        if (p.opcode == kOpSpellScribe) {
            REQUIRE(p.data.size() == kSpellScribeSize);
            CHECK(std::string(reinterpret_cast<const char*>(p.data.data()) + 0x40) == "Gennro");
        }

    // /offlinemode: saves (OP_Save), then not a trader nor a buyer: 771, and no 0x53d3.
    const int saves = MockEqServer::count(server.zone_log, Op::Save);
    conn.send_command("/offlinemode");
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, Op::Save) == saves + 1; }));
    CHECK(has_line(rig.chat(), "[771]"));
    CHECK_FALSE(conn.confirmation());
    CHECK(raw_count(server.zone_log, kOpOfflineMode) == 0);

    // /aggrometer (the mock's profile has Rain of Fear) shows and hides the meter; /claim and /bugreport only show.
    CHECK_FALSE(conn.aggro().window_open);
    conn.send_command("/aggrometer");
    CHECK(conn.aggro().window_open);
    conn.send_command("/aggrometer");
    CHECK_FALSE(conn.aggro().window_open);
    conn.send_command("/bugreport");
    conn.send_command("/bugreport");
    CHECK(conn.bug_report_open());
    conn.send_command("/claim");
    conn.send_command("/claim");
    CHECK(conn.claims().open);
}

TEST_CASE("mock server: the kept TargetGroupBuff at zone-in; /spellscribe off when the world does not allow it") {
    {
        Rig rig;
        REQUIRE(rig.to_zone());
        rig.conn.send_command("/tgb off");
        REQUIRE(rig.until([&] { return rig.conn.client_options().get(game::ClientOption::TargetGroupBuff) == 0; }));
    }
    Rig rig;
    REQUIRE(rig.to_zone());
    // DoMainLoop's start: the setting kept by the earlier session ("0").
    REQUIRE(rig.until([&] { return tgb_values(rig.server.zone_log).size() == 1; }));
    CHECK(tgb_values(rig.server.zone_log).front() == 0);
    rig.conn.send_command("/spellscribe");
    rig.conn.send_command("/tgb");
    REQUIRE(rig.until([&] { return tgb_values(rig.server.zone_log).size() == 2; }));
    CHECK(raw_count(rig.server.zone_log, kOpSpellScribe) == 0);
}

TEST_CASE("mock server: /doability takes a skill's name") {
    Rig rig;
    REQUIRE(rig.to_zone());

    Config config;
    app::ActionsUi actions(config, rig.dir.path() / "hailstone.ini");
    // The client's path: the name found as the Ability Book finds it, then its hot button run by the Actions window.
    net::GameConnectionTestAccess::with_character(rig.conn, [&](mech::Character& ch) {
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &rig.conn;
        host.character = &ch;
        host.actions = &actions;
        host.run_button = [&](const game::HotButton& b) { actions.run_button(rig.conn, &ch, app::AbilityView{}, b); };
        actions.doability_command(rig.conn, &ch, app::AbilityView{}, "forage", // a name, in any case
                                  [&](std::string_view name) { return app::use_ability_named(host, name); });
    });
    REQUIRE(rig.until([&] { return raw_count(rig.server.zone_log, kOpForage) == 1; }));

    // A number is still the Actions window's button: Forage on button 1 sends the same.
    app::ActionsUi by_button(config, rig.dir.path() / "hailstone.ini");
    by_button.set_ability_button(false, 0, mech::skill::kForage);
    net::GameConnectionTestAccess::with_character(rig.conn, [&](mech::Character& ch) {
        by_button.doability_command(rig.conn, &ch, app::AbilityView{}, "1");
    });
    REQUIRE(rig.until([&] { return raw_count(rig.server.zone_log, kOpForage) == 2; }));

    // A name the player has no such skill for says so, sends nothing and is not the button usage.
    net::GameConnectionTestAccess::with_character(rig.conn, [&](mech::Character& ch) {
        by_button.doability_command(rig.conn, &ch, app::AbilityView{}, "no such skill");
    });
    const auto messages = by_button.take_messages();
    REQUIRE(messages.size() == 1);
    CHECK(messages.front().first == "No ability by that name.");
    rig.run_for(std::chrono::milliseconds(20));
    CHECK(raw_count(rig.server.zone_log, kOpForage) == 2);
}

TEST_CASE("mock server: a GM-only command follows the player spawn's GM flag, as SpawnAppearance 20 sets it") {
    Rig rig(MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    REQUIRE(rig.until([&] { return conn.world().player() != nullptr; }));
    rig.chat();
    // InterpretCmd: a command with a Restriction asks me->IsGm(); for anyone else it is not a command (eqstr 12380).
    conn.send_command("/goto");
    CHECK(has_line(rig.chat(), "[12380]"));
    // #gm on mid-session: EQEmu sends SpawnAppearance 20 = 1 for the player; the profile's flag stays as it was.
    const auto own = static_cast<std::uint16_t>(conn.world().player_id);
    conn.receive_zone_packet_for_test(encode(SpawnAppearance{own, 20, 1}));
    REQUIRE(conn.world().player()->gm);
    conn.send_command("/goto");
    CHECK_FALSE(has_line(rig.chat(), "[12380]"));
    conn.receive_zone_packet_for_test(encode(SpawnAppearance{own, 20, 0}));
    conn.send_command("/goto");
    CHECK(has_line(rig.chat(), "[12380]"));
}
