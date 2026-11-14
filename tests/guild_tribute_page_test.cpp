// The guild window's Tribute and Banner pages (app/lua_eq_social.cpp GuildWindow) against the mock EQEmu: the
// tribute list from OP_GuildMemberList, a member's row updated by OP_GuildOptInOut, the opt-out buttons as
// CGuildMgmtWnd::UpdateButtons enables them and what they send, and the banner page without banner messages.
#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "core/byte_io.h"
#include "mock_server/harness.h"
#include "mock_server/profile_builder.h"
#include "mq_test_world.h"
#include "net/packet_io.h"
#include "net/social_packets.h"
#include "net/tribute_packets.h"
#include "net/world_packets.h"
#include "net/zone_packets.h"

using namespace hailstone;
using namespace hailstone::test;

namespace {

std::string run(lua_State* L, const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

net::GuildMember member(const char* name, std::uint16_t zone, std::uint32_t donations) {
    net::GuildMember m;
    m.name = name;
    m.level = 50;
    m.rank = 5;
    m.zone_id = zone;
    m.tribute_enable = 1;
    m.total_tribute = donations;
    return m;
}

} // namespace

TEST_CASE("mock server: the guild window's Tribute page lists the members' tribute and opts a member out") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        EmptyGameDir dir;
        GameDir game(dir.path());
        MockEqServer server(MockConfig::standard());
        net::GameConnection conn(game, quick_options());
        auto until = [&](const std::function<bool()>& done) { return pump([&] { conn.update(); server.poll(); }, done); };
        conn.start(server.ticket());
        REQUIRE(until([&] { return conn.state() == net::ConnectionState::CharacterSelect; }));
        conn.enter_world("Gennro");
        REQUIRE(until([&] { return conn.state() == net::ConnectionState::InZone && conn.player_status().max_hp != 0; }));

        // Guild 3 (SpawnAppearance 22 for us), then the roster: us online, Alice offline (rank 5 both, no
        // permissions sent, so no permission 22 to change others).
        server.zone.send_reliable(net::encode(net::SpawnAppearance{0, 22, 3}));
        net::GuildMemberList l;
        l.player_name = "Gennro";
        l.guild_id = 3;
        l.members = {member("Gennro", 2, 500), member("Alice", 0, 40)};
        l.members[1].last_tribute = 1700000000;
        l.members[1].tribute_flag = 0;
        server.zone.send_reliable(net::encode(l));
        REQUIRE(until([&] { return conn.guild().in_guild() && conn.guild().members.size() == 2; }));

        app::EqHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::EqApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "eq");
        const std::string list = "'GuildManagementWnd', 'TributeList'";
        api.begin_frame();
        CHECK(run(L, "return (eq.ListInfo(" + list + "))") == "2");
        // name, tribute state (eqstr 12946 + on; the client's own fallback text here), trophy state, donations, date
        CHECK(run(L, "local r = eq.ListRow(" + list + ", 0) return r[1]..'|'..r[2]..'|'..r[3]..'|'..r[4]..'|'..r[5]") ==
              "Gennro|on|off|500|");
        CHECK(run(L, "return eq.ListRow(" + list + ", 1)[4]") == "40");
        CHECK(run(L, "return #eq.ListRow(" + list + ", 1)[5]") == "8"); // "%02d/%02d/%02d"

        // Nobody selected: the buttons are off. Alice (another member, no permission 22): off. Ourselves: on.
        CHECK(run(L, "return (eq.ChildState('GuildManagementWnd', 'TributeOptOutButton'))") == "false");
        CHECK(run(L, "return eq.ListClick(" + list + ", 1, 'left')") == "true");
        CHECK(run(L, "return (eq.ChildState('GuildManagementWnd', 'TributeOptOutButton'))") == "false");
        CHECK(run(L, "return eq.ListClick(" + list + ", 0, 'left')") == "true");
        CHECK(run(L, "return (eq.ChildState('GuildManagementWnd', 'TributeOptOutButton'))") == "true");
        // EQEmu has no trophy tribute: never on, so its button stays off.
        CHECK(run(L, "return (eq.ChildState('GuildManagementWnd', 'TrophyTributeOptOutButton'))") == "false");
        // The selection is the window's one member: the member list shows it too.
        CHECK(run(L, "return (select(3, eq.ListInfo('GuildManagementWnd', 'MemberList')))") == "0");

        CHECK(run(L, "return eq.ClickChild('GuildManagementWnd', 'TributeOptOutButton', 'left')") == "true");
        auto sent = [&] {
            return std::find_if(server.zone_log.begin(), server.zone_log.end(),
                                [](const net::AppPacket& p) { return net::is_op(p, net::Op::GuildOptInOut); });
        };
        REQUIRE(until([&] { return sent() != server.zone_log.end(); }));
        const net::AppPacket& req = *sent();
        REQUIRE(req.data.size() == 80);
        ByteReader r(req.data);
        CHECK(r.le<std::uint32_t>() == 3); // the guild
        CHECK(r.le<std::uint32_t>() == 0); // out
        CHECK(r.fixed_string(64) == "Gennro");

        // EQEmu's answer (worldserver.cpp ServerOP_GuildTributeOptInToggle): the member off, the request's command
        // echoed (0 here: the cells turn yellow).
        net::GuildTributeOpt reply{"Gennro", false, false, 500, 1700000000, 0};
        server.zone.send_reliable(net::encode(reply));
        REQUIRE(until([&] { return !conn.guild().members[0].tribute_on; }));
        CHECK(conn.guild_tribute().opted_in == false);
        CHECK_FALSE(conn.guild().members[0].tribute_flag);
        api.begin_frame();
        CHECK(run(L, "return eq.ListRow(" + list + ", 0)[2]") == "off");
        CHECK(run(L, "return (eq.ChildState('GuildManagementWnd', 'TributeOptOutButton'))") == "false");
        // An answer naming nobody in the roster changes nothing.
        server.zone.send_reliable(net::encode(net::GuildTributeOpt{"Nobody", true, false, 1, 1, 1}));
        server.zone.send_reliable(net::encode(net::GuildTributeOpt{"alice", false, false, 41, 1, 1}));
        REQUIRE(until([&] { return conn.guild().members[1].donations == 41; })); // by name, case-insensitive
        CHECK(conn.guild().members.size() == 2);

        // The Banner page without banner messages: an empty kit list, "No active banner" (eqstr 636), the
        // controls that need a kit or a banner off, the filter, Show All and Refresh on (and nothing sent).
        CHECK(run(L, "return (eq.ListInfo('GuildManagementWnd', 'BannerKitList'))") == "0");
        CHECK(run(L, "return (eq.WindowText('GuildManagementWnd', 'TimeRemainingValueLabel'))") != "");
        CHECK(run(L, "return (eq.ChildState('GuildManagementWnd', 'PlantBannerButton'))") == "false");
        CHECK(run(L, "return (eq.ChildState('GuildManagementWnd', 'EditBannerButton'))") == "false");
        CHECK(run(L, "return (eq.ChildState('GuildManagementWnd', 'RefreshBannerListButton'))") == "true");
        std::size_t before = server.zone_log.size();
        CHECK(run(L, "return eq.ClickChild('GuildManagementWnd', 'ShowAllPossibleBannersButton', 'left')") == "true");
        CHECK(run(L, "return (select(2, eq.ChildState('GuildManagementWnd', 'ShowAllPossibleBannersButton')))") == "true");
        CHECK(run(L, "return eq.ClickChild('GuildManagementWnd', 'RefreshBannerListButton', 'left')") == "true");
        pump([&] { conn.update(); server.poll(); }, [] { return false; }, std::chrono::milliseconds(300));
        for (std::size_t i = before; i < server.zone_log.size(); ++i) {
            CHECK(server.zone_log[i].opcode != 0x0d92);
            CHECK(server.zone_log[i].opcode != 0x4924);
        }
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("guild tribute benefits require the received permission at the window and send boundary") {
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(), ImGui::DestroyContext);
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& server = rig.server;
    const auto me = static_cast<std::uint16_t>(server.config.own_spawn_id);
    CHECK_FALSE(conn.can_change_guild_tribute());

    server.zone.send_reliable(net::encode(net::SpawnAppearance{me, 22, 3}));
    server.zone.send_reliable(net::encode(net::SpawnAppearance{me, 23, 5}));
    net::TributeAbility first;
    first.id = 71;
    first.name = "First benefit";
    first.tiers[0] = {1, 100, 10};
    first.tiers[1] = {2, 101, 20};
    first.tiers[2] = {3, 102, 30};
    net::TributeAbility second = first;
    second.id = 72;
    second.name = "Second benefit";
    server.zone.send_reliable(net::encode_guild_tribute_info(first));
    server.zone.send_reliable(net::encode_guild_tribute_info(second));
    net::GuildTributeActive active;
    active.tributes = {71, net::kTributeNone};
    server.zone.send_reliable(net::encode(active));
    // The open reply counts only with the guild tribute master in range.
    auto master = *conn.world().player();
    master.id = 77;
    master.is_npc = true;
    master.class_id = 64;
    conn.world_for_test().spawns[77] = master;
    auto open = net::encode(net::StartTribute{server.config.own_spawn_id, 77, 1});
    open.opcode = net::to_wire(net::Op::OpenGuildTributeMaster);
    server.zone.send_reliable(open);
    REQUIRE(rig.until([&] {
        return conn.guild().rank == 5 && conn.guild_tribute().abilities.size() == 2 && conn.guild_tribute().master_id == 77;
    }));

    app::EqHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::EqApi api(host);
    std::unique_ptr<lua_State, decltype(&lua_close)> lua(luaL_newstate(), lua_close);
    lua_State* L = lua.get();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "eq");
    api.begin_frame();
    auto click = [&](const char* child) {
        CHECK(run(L, std::string("return eq.ClickChild('TributeMasterWnd', '") + child + "', 'left')") == "true");
    };
    auto enabled = [&](const char* child) {
        return run(L, std::string("return (eq.ChildState('TributeMasterWnd', '") + child + "'))");
    };
    auto count = [&](net::Op op) { return MockEqServer::count(server.zone_log, op); };
    auto no_changes = [&] {
        rig.run_for(30ms);
        CHECK(count(net::Op::GuildSaveActiveTributes) == 0);
        CHECK(count(net::Op::GuildModifyBenefits) == 0);
        CHECK(count(net::Op::GuildTributeToggleReq) == 0);
    };
    CHECK(enabled("TMW_LockButton") == "false");
    CHECK(enabled("TMW_StatusButton") == "false");
    CHECK(enabled("TMW_UpgradeButton") == "false");
    CHECK(enabled("TMW_DowngradeButton") == "false");
    CHECK(run(L, "return (eq.ChildState('TributeBenefitWnd', 'GuildActivateButton'))") == "false");
    click("TMW_LockButton");
    click("TMW_UpgradeButton");
    click("TMW_StatusButton");
    CHECK(run(L, "return eq.ClickChild('TributeBenefitWnd', 'GuildActivateButton', 'left')") == "true");
    conn.guild_tribute_change(active);
    conn.guild_tribute_toggle(true);
    no_changes();

    // Permission for another rank, or for changing members' participation, cannot edit the guild's benefits.
    server.zone.send_reliable(net::encode_guild_rank_permission(4, 23, true));
    server.zone.send_reliable(net::encode_guild_rank_permission(5, 22, true));
    REQUIRE(rig.until([&] { return conn.guild().permissions.has(5, 22); }));
    CHECK_FALSE(conn.can_change_guild_tribute());
    conn.guild_tribute_change(active);
    conn.guild_tribute_toggle(true);
    no_changes();

    server.zone.send_reliable(net::encode_guild_rank_permission(5, 23, true));
    REQUIRE(rig.until([&] { return conn.can_change_guild_tribute(); }));
    api.begin_frame();
    CHECK(enabled("TMW_LockButton") == "true");
    CHECK(enabled("TMW_StatusButton") == "true");
    CHECK(run(L, "return (eq.ChildState('TributeBenefitWnd', 'GuildActivateButton'))") == "true");
    CHECK(run(L, "return (eq.ListInfo('TributeMasterWnd', 'TMW_ActiveBenefitList'))") == "2");
    CHECK(run(L, "return eq.ListClick('TributeMasterWnd', 'TMW_ActiveBenefitList', 0, 'left')") == "true");
    click("TMW_LockButton");
    click("TMW_UpgradeButton");
    CHECK(run(L, "return (eq.ListInfo('TributeMasterWnd', 'TMW_AvailableBenefitList'))") == "2");
    CHECK(run(L, "return eq.ListClick('TributeMasterWnd', 'TMW_AvailableBenefitList', 1, 'left')") == "true");
    click("TMW_UpgradeButton");
    click("TMW_LockButton");
    click("TMW_StatusButton");
    REQUIRE(rig.until([&] {
        return count(net::Op::GuildSaveActiveTributes) == 1 && count(net::Op::GuildModifyBenefits) == 1 &&
               count(net::Op::GuildTributeToggleReq) == 1;
    }));
    auto saved = std::find_if(server.zone_log.begin(), server.zone_log.end(),
                              [](const net::AppPacket& p) { return net::is_op(p, net::Op::GuildSaveActiveTributes); });
    REQUIRE(saved != server.zone_log.end());
    REQUIRE(saved->data.size() == 48);
    ByteReader r(saved->data);
    CHECK(r.le<std::uint32_t>() == 0);
    r.skip(16);
    CHECK(r.le<std::uint32_t>() == 77);
    CHECK(r.le<std::uint32_t>() == 71);
    CHECK(r.le<std::uint32_t>() == 72);
    CHECK(r.le<std::uint32_t>() == 1);
    CHECK(r.le<std::uint32_t>() == 0);
    // The editable preview never grants a benefit before the server answers.
    CHECK(conn.guild_tribute().active.tributes[1] == net::kTributeNone);
    CHECK(conn.guild_tribute().active.tiers[0] == 0);

    // Revocation between editing and Save is checked even before the next UI frame.
    click("TMW_LockButton");
    server.zone.send_reliable(net::encode_guild_rank_permission(5, 23, false));
    REQUIRE(rig.until([&] { return !conn.can_change_guild_tribute(); }));
    click("TMW_LockButton");
    click("TMW_StatusButton");
    conn.guild_tribute_change(active);
    conn.guild_tribute_toggle(false);
    CHECK(run(L, "return (eq.WindowText('TributeMasterWnd', 'TMW_LockButton'))") == "Modify Benefits");
    rig.run_for(30ms);
    CHECK(count(net::Op::GuildSaveActiveTributes) == 1);
    CHECK(count(net::Op::GuildModifyBenefits) == 1);
    CHECK(count(net::Op::GuildTributeToggleReq) == 1);

    // A normal frame also retires an unsaved edit when permission disappears.
    server.zone.send_reliable(net::encode_guild_rank_permission(5, 23, true));
    REQUIRE(rig.until([&] { return conn.can_change_guild_tribute(); }));
    click("TMW_LockButton");
    server.zone.send_reliable(net::encode_guild_rank_permission(5, 23, false));
    REQUIRE(rig.until([&] { return !conn.can_change_guild_tribute(); }));
    api.begin_frame();
    CHECK(run(L, "return (eq.WindowText('TributeMasterWnd', 'TMW_LockButton'))") == "Modify Benefits");
    CHECK(enabled("TMW_LockButton") == "false");

    server.zone.send_reliable(net::encode_guild_rank_permission(5, 23, true));
    REQUIRE(rig.until([&] { return conn.can_change_guild_tribute(); }));
    server.zone.send_reliable(net::encode(net::SpawnAppearance{me, 22, 0xffffffff}));
    REQUIRE(rig.until([&] { return !conn.guild().in_guild(); }));
    CHECK_FALSE(conn.can_change_guild_tribute());
    conn.guild_tribute_change(active);
    conn.guild_tribute_toggle(true);
    CHECK(run(L, "return (eq.ChildState('TributeBenefitWnd', 'GuildActivateButton'))") == "false");
    CHECK(run(L, "return (eq.ChildState('TributeBenefitWnd', 'OptOutButton'))") == "false");
    rig.run_for(30ms);
    CHECK(count(net::Op::GuildSaveActiveTributes) == 1);
    CHECK(count(net::Op::GuildTributeToggleReq) == 1);
}

TEST_CASE("guild tribute participation permits self changes but requires permission for other members") {
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(), ImGui::DestroyContext);
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& server = rig.server;
    const auto me = static_cast<std::uint16_t>(server.config.own_spawn_id);
    server.zone.send_reliable(net::encode(net::SpawnAppearance{me, 22, 3}));
    server.zone.send_reliable(net::encode(net::SpawnAppearance{me, 23, 5}));
    net::GuildMemberList roster;
    roster.player_name = "Gennro";
    roster.guild_id = 3;
    roster.members = {member("Gennro", 2, 0), member("Alice", 2, 0)};
    server.zone.send_reliable(net::encode(roster));
    REQUIRE(rig.until([&] { return conn.guild().rank == 5 && conn.guild().members.size() == 2; }));
    app::EqHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::EqApi api(host);
    std::unique_ptr<lua_State, decltype(&lua_close)> lua(luaL_newstate(), lua_close);
    lua_State* L = lua.get();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "eq");
    api.begin_frame();
    CHECK(run(L, "return (eq.ListInfo('GuildManagementWnd', 'TributeList'))") == "2");
    CHECK(run(L, "return eq.ListClick('GuildManagementWnd', 'TributeList', 1, 'left')") == "true");
    conn.guild_tribute_opt_out("Alice");
    CHECK(run(L, "return eq.ClickChild('GuildManagementWnd', 'TributeOptOutButton', 'left')") == "true");
    rig.run_for(30ms);
    CHECK(MockEqServer::count(server.zone_log, net::Op::GuildOptInOut) == 0);

    conn.guild_tribute_opt_out("gennro");
    conn.guild_tribute_opt(true);
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, net::Op::GuildOptInOut) == 2; }));
    server.zone.send_reliable(net::encode_guild_rank_permission(5, 22, true));
    REQUIRE(rig.until([&] { return conn.guild().permissions.has(5, 22); }));
    CHECK(run(L, "return (eq.ChildState('GuildManagementWnd', 'TributeOptOutButton'))") == "true");
    CHECK(run(L, "return eq.ClickChild('GuildManagementWnd', 'TributeOptOutButton', 'left')") == "true");
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, net::Op::GuildOptInOut) == 3; }));
    auto sent = std::find_if(server.zone_log.rbegin(), server.zone_log.rend(),
                             [](const net::AppPacket& p) { return net::is_op(p, net::Op::GuildOptInOut); });
    REQUIRE(sent != server.zone_log.rend());
    ByteReader r(sent->data);
    CHECK(r.le<std::uint32_t>() == 3);
    CHECK(r.le<std::uint32_t>() == 0);
    CHECK(r.fixed_string(64) == "Alice");
    server.zone.send_reliable(net::encode_guild_rank_permission(5, 22, false));
    REQUIRE(rig.until([&] { return !conn.guild().permissions.has(5, 22); }));
    conn.guild_tribute_opt_out("Alice");
    CHECK(run(L, "return eq.ClickChild('GuildManagementWnd', 'TributeOptOutButton', 'left')") == "true");
    rig.run_for(30ms);
    CHECK(MockEqServer::count(server.zone_log, net::Op::GuildOptInOut) == 3);
}

TEST_CASE("guild tribute permissions and selections survive Source's metadata before membership ordering") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& connection = rig.conn;
    const auto me = static_cast<std::uint16_t>(rig.server.config.own_spawn_id);
    auto appearance = [&](std::uint16_t type, std::uint32_t value) {
        connection.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{me, type, value}));
    };
    auto permission = [&](std::uint32_t guild_id, std::uint32_t rank, bool on) {
        // SendGuildRanks: action at 0, recipient/header through 75, guild id at 76, then rank/function/value.
        ByteWriter w;
        w.le<std::uint32_t>(5);
        w.zeros(72);
        w.le(guild_id);
        w.le(rank);
        w.le<std::uint32_t>(23);
        w.le<std::uint8_t>(on ? 1 : 0);
        w.zeros(3);
        const auto bytes = w.take();
        CHECK(net::encode_guild_rank_permission(rank, 23, on, guild_id).data == bytes);
        connection.receive_zone_packet_for_test({net::to_wire(net::Op::GuildUpdate), bytes});
    };
    auto active = [&](std::uint32_t benefit) {
        net::GuildTributeActive settings;
        settings.enabled = true;
        settings.favor = 1234;
        settings.timer_ms = 600000;
        settings.tributes[0] = benefit;
        // Source SendGuildActiveTributes leaves its leading, unused DWORD zero.
        connection.receive_zone_packet_for_test(net::encode(settings));
    };

    // Join/refresh sends tagged ranks, untagged rank names and active benefits before its GuildID appearance.
    permission(3, 5, true);
    connection.receive_zone_packet_for_test(net::encode_guild_rank_name(5, "New guild officer"));
    CHECK(connection.received_guild_permission_context() == 3);
    CHECK_FALSE(connection.guild().permissions.has(5, 23));
    active(71);
    CHECK_FALSE(connection.can_change_guild_tribute());
    appearance(22, 3);
    appearance(23, 5);
    CHECK(connection.guild().permissions.has(5, 23));
    CHECK(connection.guild().rank_names[5] == "New guild officer");
    CHECK(connection.can_change_guild_tribute());
    CHECK(connection.guild_tribute().active.tributes[0] == 71);
    CHECK(connection.guild_tribute().active.favor == 1234);
    CHECK(connection.guild_tribute().active.timer_ms == 600000);

    // Our old guild's valid permission cannot authorize the future guild's already received benefits.
    permission(4, 5, true);
    connection.receive_zone_packet_for_test(net::encode_guild_rank_name(5, "Next guild officer"));
    active(73);
    CHECK(connection.guild().permissions.has(5, 23));
    CHECK_FALSE(connection.can_change_guild_tribute());
    appearance(22, 4);
    CHECK_FALSE(connection.can_change_guild_tribute());
    appearance(23, 5);
    CHECK(connection.can_change_guild_tribute());
    CHECK(connection.guild().rank_names[5] == "Next guild officer");
    CHECK(connection.guild_tribute().active.tributes[0] == 73);

    // An untagged permission-change broadcast still updates the current guild.
    connection.receive_zone_packet_for_test(net::encode_guild_rank_permission(5, 23, false));
    CHECK_FALSE(connection.can_change_guild_tribute());
    // A foreign table cannot grant access now, or follow us into a different guild than the stamped one.
    permission(99, 5, true);
    CHECK_FALSE(connection.guild().permissions.has(5, 23));
    CHECK_FALSE(connection.can_change_guild_tribute());
    appearance(22, 6);
    CHECK_FALSE(connection.guild().permissions.has(5, 23));
    CHECK(connection.received_guild_permission_context() == 6);

    // A new profile can establish membership directly, with the same exact-context promotion rule.
    permission(42, 1, true);
    connection.receive_zone_packet_for_test(net::encode_guild_rank_name(1, "Profile guild leader"));
    active(72);
    CHECK_FALSE(connection.can_change_guild_tribute());
    connection.receive_zone_packet_for_test(build_player_profile()); // fixture's guild 42, rank 1
    CHECK(connection.guild().guild_id == 42);
    CHECK(connection.guild().rank == 1);
    CHECK(connection.guild().permissions.has(1, 23));
    CHECK(connection.guild().rank_names[1] == "Profile guild leader");
    CHECK(connection.guild_tribute().active.tributes[0] == 72);
    CHECK(connection.can_change_guild_tribute());

    permission(99, 1, true);
    connection.disconnect();
    CHECK_FALSE(connection.guild().permissions.has(1, 23));
    CHECK(connection.received_guild_permission_context() != 99);
    CHECK_FALSE(connection.can_change_guild_tribute());
}

TEST_CASE("guild tribute permissions wait for the new guild rank when identity and rank arrive separately") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& connection = rig.conn;
    const auto me = static_cast<std::uint16_t>(rig.server.config.own_spawn_id);
    connection.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{me, 22, 3}));
    connection.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{me, 23, 1}));
    connection.receive_zone_packet_for_test(net::encode_guild_rank_permission(1, 23, true, 3));
    CHECK(connection.can_change_guild_tribute());

    // The new guild permits leaders, while our own new rank has not arrived yet.
    connection.receive_zone_packet_for_test(net::encode_guild_rank_permission(1, 23, true, 4));
    connection.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{me, 22, 4}));
    CHECK(connection.guild().permissions.has(1, 23));
    CHECK(connection.guild().rank == 0);
    CHECK(connection.guild().status == 1); // native rank-message history is separate from permission authority
    REQUIRE(connection.world().player());
    CHECK(connection.world().player()->guild_rank == 0);
    CHECK_FALSE(connection.can_change_guild_tribute());
    connection.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{me, 23, 8}));
    CHECK_FALSE(connection.can_change_guild_tribute());
    connection.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{me, 23, 1}));
    CHECK(connection.can_change_guild_tribute());
}

TEST_CASE("guild rank permissions and names survive a zone change; a staged table and a session end do not") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& connection = rig.conn;
    const auto me = static_cast<std::uint16_t>(rig.server.config.own_spawn_id);
    connection.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{me, 22, 3}));
    connection.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{me, 23, 5}));
    connection.receive_zone_packet_for_test(net::encode_guild_rank_permission(5, 23, true, 3));
    connection.receive_zone_packet_for_test(net::encode_guild_rank_name(5, "Officer"));
    REQUIRE(connection.can_change_guild_tribute());
    REQUIRE(connection.guild().rank_names[5] == "Officer");
    // Another guild's tagged rows that no GuildID appearance confirms.
    connection.receive_zone_packet_for_test(net::encode_guild_rank_permission(5, 23, true, 9));
    CHECK(connection.received_guild_permission_context() == 9);

    connection.receive_world_packet_for_test(net::encode(net::ZoneServerInfo{"127.0.0.1", 9000}));
    CHECK(connection.guild().permissions.has(5, 23));
    CHECK(connection.guild().rank_names[5] == "Officer");
    // The staged table is gone, so an untagged rank name reaches the current guild again.
    CHECK(connection.received_guild_permission_context() == 3);
    connection.receive_zone_packet_for_test(net::encode_guild_rank_name(5, "Renamed"));
    CHECK(connection.guild().rank_names[5] == "Renamed");

    connection.disconnect();
    CHECK_FALSE(connection.guild().permissions.has(5, 23));
    CHECK(connection.guild().rank_names[5].empty());
}

TEST_CASE("the tribute master's Donate button is greyed while a guild platinum donation waits for its reply") {
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(), ImGui::DestroyContext);
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    const auto me = static_cast<std::uint16_t>(rig.server.config.own_spawn_id);
    conn.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{me, 22, 3}));
    auto master = *conn.world().player();
    master.id = 77;
    master.is_npc = true;
    master.class_id = 64;
    conn.world_for_test().spawns[77] = master;
    auto open = net::encode(net::StartTribute{conn.world().player_id, 77, 1});
    open.opcode = net::to_wire(net::Op::OpenGuildTributeMaster);
    conn.receive_zone_packet_for_test(open);
    REQUIRE(conn.guild_tribute().master_id == 77);
    net::GameConnectionTestAccess::coins(conn) = {5, 0, 0, 0};
    conn.click_slot(net::ItemSlot::possessions(23), true, false);
    REQUIRE(conn.inventory().cursor());

    app::EqHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::EqApi api(host);
    std::unique_ptr<lua_State, decltype(&lua_close)> lua(luaL_newstate(), lua_close);
    lua_State* L = lua.get();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "eq");
    api.begin_frame();
    auto enabled = [&] { return run(L, "return (eq.ChildState('TributeMasterWnd', 'TMW_DonateButton'))"); };
    CHECK(enabled() == "true");

    REQUIRE(conn.guild_tribute_donate_platinum(3));
    CHECK(conn.guild_platinum_pending());
    CHECK(enabled() == "false");
    // A click on the greyed button sends nothing.
    CHECK(run(L, "return eq.ClickChild('TributeMasterWnd', 'TMW_DonateButton', 'left')") == "true");
    rig.run_for(std::chrono::milliseconds(30));
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::GuildTributeDonateItem) == 0);
    CHECK(conn.inventory().cursor());

    ByteWriter w; // the reply: quantity, an unused DWORD, favour
    w.le<std::uint32_t>(3);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(30);
    conn.receive_zone_packet_for_test({net::to_wire(net::Op::GuildTributeDonatePlat), w.take()});
    CHECK_FALSE(conn.guild_platinum_pending());
    CHECK(conn.coins().platinum == 2);
    CHECK(enabled() == "true");
}
