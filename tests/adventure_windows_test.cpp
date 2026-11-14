// Adventure window models over the real eq API: the Alt+V view without a recruiter, offer and active states,
// profile balances, complete theme statistics, and the native leaderboard's category/risk and tied standings.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "app/lua_eq_menu.h"
#include "core/byte_io.h"
#include "game/keymap.h"
#include "manual_clock.h"
#include "mock_server/profile_builder.h"
#include "mq_test_world.h"
#include "net/adventure_packets.h"
#include "net/packet_io.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace std::chrono_literals;

namespace {

struct ImGuiScope {
    ImGuiContext* ctx = ImGui::CreateContext();
    ~ImGuiScope() { ImGui::DestroyContext(ctx); }
};

ConnectionOptions with_clock(const test::ManualClock& clock) {
    ConnectionOptions options;
    options.session.clock = clock.source();
    return options;
}

struct AdventureUi {
    ImGuiScope imgui;
    test::ManualClock clock;
    mq_test::World world{"hailstone_adventure_windows", with_clock(clock)};
    app::EqHost host;
    app::EqApi api{host};
    lua_State* L = luaL_newstate();

    AdventureUi() {
        clock.hold();
        host.in_game = host.ui_ready = true;
        host.game = world.game.get();
        host.conn = world.conn.get();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "eq");
    }
    ~AdventureUi() { lua_close(L); }

    std::string operator()(const std::string& code) {
        INFO(code);
        REQUIRE_MESSAGE(luaL_dostring(L, code.c_str()) == LUA_OK, (lua_isstring(L, -1) ? lua_tostring(L, -1) : ""));
        // WindowText also returns its STML flag; assertions read the expression's first value.
        const std::string value = lua_isnoneornil(L, 1) ? "nil" : luaL_tolstring(L, 1, nullptr);
        lua_settop(L, 0);
        return value;
    }
    void receive(const AppPacket& packet) {
        world.conn->receive_zone_packet_for_test(packet);
        api.begin_frame();
    }
    void recruiter() {
        world.add(20, "Adventure_Recruiter", true, 50, {1, 0, 0});
        world.conn->world_for_test().spawns.at(20).class_id = 60;
        REQUIRE(world.conn->open_adventure_recruiter(20));
    }
};

AppPacket text_packet(Op op, std::string_view text) {
    ByteWriter w;
    w.fixed_string(text, text.size() + 1);
    return make_packet(op, std::move(w));
}

} // namespace

TEST_CASE("adventure windows: Alt+V and the menu toggle the adventure away from a recruiter") {
    AdventureUi ui;
    const auto command = static_cast<int>(game::KeyCommand::TOGGLE_ADVREQUESTWIN);
    REQUIRE(app::toggle_command_window(command));
    CHECK(std::string(app::toggle_command_window(command)) == "AdventureRequestWnd");
    CHECK(app::menu_command_offered(command, [](std::string_view name) { return name == "AdventureRequestWnd"; }));
    game::KeyMap keys;
    CHECK(game::key_text(keys.binding(command, 0)) == "Alt V");
    std::vector<game::KeyMap::Event> events;
    keys.key_down(game::KeyCombo::from_int(0x1000002f), events);
    REQUIRE(std::any_of(events.begin(), events.end(), [command](const auto& event) { return event.command == command && event.down; }));

    CHECK_FALSE(ui.api.shown("AdventureRequestWnd"));
    ui.api.toggle(app::toggle_command_window(command));
    CHECK(ui.api.shown("AdventureRequestWnd"));
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_NPCText')") == "You are not currently assigned to an adventure.");
    CHECK(ui("return (eq.ChildState('AdventureRequestWnd', 'AdvRqst_RequestButton'))") == "false");
    CHECK(ui("return (eq.ChildState('AdventureRequestWnd', 'AdvRqst_AcceptButton'))") == "false");
    CHECK(ui("return (eq.ChildState('AdventureRequestWnd', 'AdvRqst_DeclineButton'))") == "false");
    CHECK(ui("return eq.ChildShown('AdventureRequestWnd', 'AdvRqst_LeaveButton')") == "false");
    CHECK(ui("return #(eq.ComboInfo('AdventureRequestWnd', 'AdvRqst_TypeCombobox'))") == "5");
    CHECK(ui("return #(eq.ComboInfo('AdventureRequestWnd', 'AdvRqst_RiskCombobox'))") == "2");
    CHECK(ui("return eq.ClickChild('AdventureRequestWnd', 'AdvRqst_ViewStatsButton', 'left')") == "true");
    CHECK(ui.api.shown("AdventureStatsWnd"));
    ui.api.toggle("AdventureRequestWnd");
    CHECK_FALSE(ui.api.shown("AdventureRequestWnd"));
    CHECK(ui.world.conn->adventure().recruiter == 0);
}

TEST_CASE("adventure windows: recruiter offers and active adventures expose separate decline and leave actions") {
    AdventureUi ui;
    ui.recruiter();
    CHECK(ui.api.shown("AdventureRequestWnd"));
    CHECK(ui("return (eq.ChildState('AdventureRequestWnd', 'AdvRqst_RequestButton'))") == "true");
    ui.receive(text_packet(Op::AdventureInfo, "Recruiter introduction."));
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_NPCText')") == "Recruiter introduction.");
    ui.receive(text_packet(Op::AdventureDetails, "Offered <adventure> & briefing."));
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_NPCText')") == "Offered &lt;adventure&gt; &amp; briefing.");
    CHECK(ui("return (eq.ChildState('AdventureRequestWnd', 'AdvRqst_RequestButton'))") == "false");
    CHECK(ui("return (eq.ChildState('AdventureRequestWnd', 'AdvRqst_AcceptButton'))") == "true");
    CHECK(ui("return (eq.ChildState('AdventureRequestWnd', 'AdvRqst_DeclineButton'))") == "true");
    CHECK(ui("return eq.ClickChild('AdventureRequestWnd', 'AdvRqst_DeclineButton', 'left')") == "true");
    CHECK(ui.world.conn->adventure().offer.empty());

    AdventureData active;
    active.assigned = true;
    active.text = "Current briefing.";
    active.risk = 2;
    active.time_to_enter = 120;
    active.time_left = 1800;
    ui.receive(encode(active));
    ByteWriter count;
    count.le<std::uint32_t>(3);
    count.le<std::uint32_t>(10);
    ui.receive(make_packet(Op::AdventureUpdate, std::move(count)));
    ui.clock.advance(31s);
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_NPCText')") == "Current briefing.");
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_EnterTimeLeftLabel')") == "2 minutes");
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_CompleteTimeLeftLabel')") == "");
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_ProgressTextLabel')") == "");
    CHECK(ui("return select(2, eq.ComboInfo('AdventureRequestWnd', 'AdvRqst_RiskCombobox'))") == "1");
    CHECK(ui("return (eq.ChildState('AdventureRequestWnd', 'AdvRqst_AcceptButton'))") == "false");
    CHECK(ui("return (eq.ChildState('AdventureRequestWnd', 'AdvRqst_DeclineButton'))") == "false");
    CHECK(ui("return eq.ChildShown('AdventureRequestWnd', 'AdvRqst_LeaveButton')") == "true");
    CHECK(ui("return (eq.ChildState('AdventureRequestWnd', 'AdvRqst_LeaveButton'))") == "true");
    ui("eq.ClickChild('AdventureRequestWnd', 'AdvRqst_DeclineButton', 'left')");
    CHECK(ui.world.conn->adventure().data.assigned);

    active.time_to_enter = 0;
    active.time_left = 90;
    ui.receive(encode(active));
    ByteWriter active_count;
    active_count.le<std::uint32_t>(3);
    active_count.le<std::uint32_t>(10);
    ui.receive(make_packet(Op::AdventureUpdate, std::move(active_count)));
    ui.clock.advance(1s);
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_EnterTimeLeftLabel')") == "");
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_CompleteTimeLeftLabel')") == "2 minutes");
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_ProgressTextLabel')") == "3 / 10");
    ui.clock.advance(30s);
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_CompleteTimeLeftLabel')") == "1 minute");
    ui.clock.advance(100s);
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_CompleteTimeLeftLabel')") == "0 minutes");
    ui.receive(encode(AdventureData{}));
    CHECK(ui("return eq.WindowText('AdventureRequestWnd', 'AdvRqst_ProgressTextLabel')") == "");
    CHECK(ui("return eq.ChildShown('AdventureRequestWnd', 'AdvRqst_LeaveButton')") == "false");
}

TEST_CASE("adventure windows: profile points and full theme statistics include percentages without overflow") {
    AdventureUi ui;
    test::ProfileSpec profile;
    profile.name = "Tester";
    profile.adventure_points = {11, 22, 33, 44, 55};
    profile.adventure_points_available = 165;
    ui.receive(test::build_player_profile(profile));
    CHECK(ui("return eq.WindowText('AdventureStatsWnd', 'AdvStats_AvailableValue')") == "165");
    auto rows = ui.api.list_rows("AdventureStatsWnd", "AdvStats_ThemeList");
    REQUIRE(rows.size() == 6);
    REQUIRE(rows[0].size() == 7);
    CHECK(rows[0][0] == "Deepest Guk");
    CHECK(rows[0][1] == "---");
    CHECK(rows[0][4] == "0.0%");
    CHECK(rows[0][6] == "11");
    CHECK(rows[5][0] == "Totals");
    CHECK(rows[5][6] == "165");

    AdventureStats stats;
    stats.success = {20, 3, 7, 0, std::numeric_limits<std::uint32_t>::max(), 0};
    stats.failure = {5, 1, 3, 0, std::numeric_limits<std::uint32_t>::max(), 0};
    stats.rank = {12, 2, 4, 0, 9, 0};
    stats.rank_percent = {14, 3, 5, 0, 11, 0};
    ui.receive(encode(stats));
    rows = ui.api.list_rows("AdventureStatsWnd", "AdvStats_ThemeList");
    CHECK(rows[0] == std::vector<std::string>{"Deepest Guk", "2", "3", "1", "75.0%", "3", "11"});
    CHECK(rows[1][4] == "70.0%");
    CHECK(rows[2][4] == "0.0%");
    CHECK(rows[3][4] == "50.0%");
    CHECK(rows[5] == std::vector<std::string>{"Totals", "12", "20", "5", "80.0%", "14", "165"});

    AdventurePoints balances;
    balances.theme.fill(std::numeric_limits<std::uint32_t>::max());
    ui.receive(encode(balances));
    rows = ui.api.list_rows("AdventureStatsWnd", "AdvStats_ThemeList");
    CHECK(rows[5][6] == "21474836475");
}

TEST_CASE("adventure windows: leaderboard category and risk are independent and ties retain competition ranks") {
    AdventureUi ui;
    CHECK(ui("return #(eq.ComboInfo('AdventureLeaderboardWnd', 'AdvLeaderboard_SelectCombobox'))") == "12");
    CHECK(ui("return (eq.ComboInfo('AdventureLeaderboardWnd', 'AdvLeaderboard_SelectCombobox'))[1]") == "Total wins");
    CHECK(ui("return (eq.ComboInfo('AdventureLeaderboardWnd', 'AdvLeaderboard_SelectCombobox'))[2]") == "Total percentage");
    CHECK(ui("return #(eq.ComboInfo('AdventureLeaderboardWnd', 'AdvLeaderboard_RiskCombobox'))") == "3");
    for (int category = 0; category < 12; ++category) {
        CHECK(ui.api.select_combo("AdventureLeaderboardWnd", "AdvLeaderboard_SelectCombobox", category));
        const auto& selected = ui.world.conn->adventure().leaderboard_selection;
        CHECK(selected.theme == static_cast<std::uint32_t>(category / 2));
        CHECK(selected.order == static_cast<std::uint32_t>(category % 2));
    }
    CHECK_FALSE(ui.api.select_combo("AdventureLeaderboardWnd", "AdvLeaderboard_SelectCombobox", 12));
    for (int risk = 0; risk < 3; ++risk) {
        CHECK(ui.api.select_combo("AdventureLeaderboardWnd", "AdvLeaderboard_RiskCombobox", risk));
        CHECK(ui.world.conn->adventure().leaderboard_selection.risk == static_cast<std::uint32_t>(risk));
    }
    CHECK_FALSE(ui.api.select_combo("AdventureLeaderboardWnd", "AdvLeaderboard_RiskCombobox", 3));

    // Resolve the first outstanding request, then the selected category once its native cooldown expires.
    ui.receive(encode(AdventureLeaderboard{}));
    ui.clock.advance(10s);
    ui.world.conn->adventure_frame();
    CHECK(ui("return eq.WindowText('AdventureLeaderboardWnd', 'AdvLeaderboard_StatusLabel')") == "Updating leaderboard...");
    AdventureLeaderboard board;
    board.success = 0;
    board.failure = 0;
    board.rank = 0;
    board.rows = {{"Alpha", 10, 2}, {"Bravo", 10, 2}, {"Charlie", 8, 4}, {"Delta", 0, 0}};
    ui.receive(encode(board));
    auto rows = ui.api.list_rows("AdventureLeaderboardWnd", "AdvLeaderboard_LeaderList");
    REQUIRE(rows.size() == 4);
    CHECK(rows[0][0] == "1");
    CHECK(rows[1][0] == "1");
    CHECK(rows[2][0] == "3");
    CHECK(rows[3][0] == "4");
    CHECK(rows[0][4] == "83.3%");
    CHECK(rows[3][4] == "0.0%");
    CHECK(ui("return eq.WindowText('AdventureLeaderboardWnd', 'AdvLeaderboard_YourRankLabel')") == "---");
    CHECK(ui("return eq.WindowText('AdventureLeaderboardWnd', 'AdvLeaderboard_YourPercentLabel')") == "0.0%");
    CHECK(ui("return eq.WindowText('AdventureLeaderboardWnd', 'AdvLeaderboard_StatusLabel')") == "");

    // A blank wire slot is skipped visually but still counts toward the next untied rank.
    CHECK(ui.api.select_combo("AdventureLeaderboardWnd", "AdvLeaderboard_SelectCombobox", 10));
    ui.clock.advance(10s);
    ui.world.conn->adventure_frame();
    board.rank = 3;
    AppPacket sparse = encode(board);
    std::fill(sparse.data.begin() + 20 + 72, sparse.data.begin() + 20 + 72 + 64, std::byte{0});
    ui.receive(sparse);
    rows = ui.api.list_rows("AdventureLeaderboardWnd", "AdvLeaderboard_LeaderList");
    REQUIRE(rows.size() == 3);
    CHECK(rows[0][0] == "1");
    CHECK(rows[1][0] == "3");
    CHECK(rows[2][0] == "4");
    CHECK(ui("return eq.WindowText('AdventureLeaderboardWnd', 'AdvLeaderboard_YourRankLabel')") == "3");

    // A visible personal row uses the board's computed competition rank even when the header has no rank.
    ui.world.conn->adventure_leaderboard_request(0, 5, 2);
    ui.clock.advance(10s);
    ui.world.conn->adventure_frame();
    board.rank = 0;
    board.rows[1].name = "Tester";
    ui.receive(encode(board));
    CHECK(ui("return eq.WindowText('AdventureLeaderboardWnd', 'AdvLeaderboard_YourRankLabel')") == "1");

    // A script can change the selected board between UI frames; each selector preserves the other live choice.
    ui.world.conn->adventure_leaderboard_request(1, 2, 1);
    CHECK(ui.api.select_combo("AdventureLeaderboardWnd", "AdvLeaderboard_RiskCombobox", 0));
    CHECK(ui.world.conn->adventure().leaderboard_selection.order == 1);
    CHECK(ui.world.conn->adventure().leaderboard_selection.theme == 2);
    CHECK(ui.world.conn->adventure().leaderboard_selection.risk == 0);
    ui.world.conn->adventure_leaderboard_request(0, 4, 2);
    CHECK(ui.api.select_combo("AdventureLeaderboardWnd", "AdvLeaderboard_SelectCombobox", 3));
    CHECK(ui.world.conn->adventure().leaderboard_selection.order == 1);
    CHECK(ui.world.conn->adventure().leaderboard_selection.theme == 1);
    CHECK(ui.world.conn->adventure().leaderboard_selection.risk == 2);
}
