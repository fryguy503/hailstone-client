// Hailstone's right-click menu on another player (game::opens_player_menu, app::player_menu_rows,
// app::open_player_menu): which spawns have it, and what each row sends, against the mock server.
#include <doctest/doctest.h>

#include <string>
#include <utility>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "game/interact.h"
#include "mock_server/harness.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

std::uint32_t add_player(GameConnection& conn, std::uint32_t id, const std::string& name) {
    auto& w = conn.world_for_test();
    const game::Spawn* me = w.player();
    REQUIRE(me);
    game::Spawn pc;
    pc.id = id;
    pc.name = name;
    pc.position = me->position + glm::vec3(3.0f, 0.0f, 0.0f);
    pc.heights.avatar_height = 6.0f;
    w.spawns[pc.id] = pc;
    return id;
}

} // namespace

TEST_CASE("player menu: only another plain player has it") {
    game::Spawn pc;
    CHECK(game::opens_player_menu(pc, false));
    CHECK_FALSE(game::opens_player_menu(pc, true)); // the player's own spawn
    game::Spawn s = pc;
    s.is_npc = true;
    CHECK_FALSE(game::opens_player_menu(s, false));
    s = pc;
    s.is_corpse = true;
    CHECK_FALSE(game::opens_player_menu(s, false));
    s = pc;
    s.trader = true; // its shop, as the original
    CHECK_FALSE(game::opens_player_menu(s, false));
    s = pc;
    s.buyer = true; // its barter window
    CHECK_FALSE(game::opens_player_menu(s, false));
}

TEST_CASE("player menu: the rows and what each sends") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        // Its own servers folder: the ContextMenus change below is saved to the character's options.ini.
        EmptyGameDir servers;
        ConnectionOptions options = quick_options();
        options.profile_id = "test-server";
        options.servers_dir = servers.path();
        Rig rig(MockConfig::standard(), options);
        auto& conn = rig.conn;
        REQUIRE(rig.to_zone());
        app::UiHost host;
        app::UiApi api{host};
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        const std::uint32_t bob = add_player(conn, 4100, "Bob");

        auto rows = app::player_menu_rows(host, bob);
        REQUIRE(rows.size() == 6);
        CHECK(rows[0].text == "Bob");
        CHECK_FALSE(rows[0].enabled);
        CHECK(rows[1].text == "Send Tell");
        CHECK(rows[2].text == "Invite to Group");
        CHECK(rows[3].text == "Invite to Raid");
        CHECK(rows[4].text == "Trade");
        CHECK(rows[5].text == "Inspect");
        for (std::size_t i = 1; i < rows.size(); ++i) {
            CHECK(rows[i].enabled);
            CHECK(static_cast<bool>(rows[i].act));
        }
        // Not for the player's own spawn.
        CHECK(app::player_menu_rows(host, conn.world().player_id).empty());

        auto count = [&](Op op) { return MockEqServer::count(rig.server.zone_log, op); };
        rows[2].act();
        REQUIRE(rig.until([&] { return count(Op::GroupInvite) == 1; }));
        CHECK(conn.world().target_id == bob);
        rows[3].act();
        REQUIRE(rig.until([&] { return count(Op::RaidInvite) == 1; }));
        rows[5].act();
        REQUIRE(rig.until([&] { return count(Op::InspectRequest) == 1; }));
        conn.destroy_cursor();
        rows[4].act();
        REQUIRE(rig.until([&] { return count(Op::TradeRequest) == 1; }));

        // A row does nothing once the player has gone.
        conn.world_for_test().spawns.erase(bob);
        rows[5].act();
        rig.run_for(std::chrono::milliseconds(100));
        CHECK(count(Op::InspectRequest) == 1);

        // It opens as a game menu, and not with context menus switched off (UiHost::context_menus).
        add_player(conn, 4101, "Ann");
        CHECK(api.open_player_menu(4101, 100.0f, 120.0f));
        REQUIRE(host.menus.size() == 1);
        CHECK(host.menus[0].x == 100.0f);
        CHECK(host.menus[0].rows[0].text == "Ann");
        {
            // hs.GameMenus(scale) measures the rows at that scale, for a pack that zooms its menus.
            lua_State* L = luaL_newstate();
            lua_newtable(L);
            api.register_functions(L);
            lua_setglobal(L, "hs");
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr; // no imgui.ini in the working folder
            unsigned char* px = nullptr;
            int fw = 0, fh = 0;
            io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh); // the default font, built
            io.DisplaySize = ImVec2(1280, 800);
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            auto size = [&](const char* call) {
                REQUIRE(luaL_dostring(L, call) == LUA_OK);
                double w = lua_tonumber(L, -2), h = lua_tonumber(L, -1);
                lua_settop(L, 0);
                return std::pair{w, h};
            };
            auto one = size("local m = hs.GameMenus()[1]; return m.w, m.h");
            auto two = size("local m = hs.GameMenus(2)[1]; return m.w, m.h");
            CHECK(two.first > one.first * 1.5);
            CHECK(two.second > one.second * 1.5);
            CHECK(size("local m = hs.GameMenus(1)[1]; return m.w, m.h") == one);
            ImGui::EndFrame();
            lua_close(L);
        }
        host.menus.clear();
        conn.change_client_option(game::ClientOption::ContextMenus, 0);
        CHECK_FALSE(api.open_player_menu(4101, 100.0f, 120.0f));
        CHECK(host.menus.empty());
    }
    ImGui::DestroyContext(ctx);
}
