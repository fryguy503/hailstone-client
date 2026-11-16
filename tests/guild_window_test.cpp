// The guild window's roster over the hs API (app/lua_ui_social.cpp GuildWindow): the lists are built once a frame
// and again after the window's own state changes, so a sorted list reading every row stays cheap.
#include <doctest/doctest.h>

#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "core/game_dir.h"
#include "mq/commands.h"
#include "mq/tlo.h"
#include "net/game_connection.h"
#include "net/social_packets.h"
#include "plugin/hooks.h"
#include "scratch_dir.h"

using namespace hailstone;

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

net::AppPacket roster(std::initializer_list<const char*> names) {
    net::GuildMemberList l;
    l.player_name = "Tester";
    l.guild_id = 3;
    for (const char* n : names) {
        net::GuildMember m;
        m.name = n;
        m.level = 50;
        m.rank = 5;
        m.zone_id = 2;
        l.members.push_back(m);
    }
    return net::encode(l);
}

} // namespace

TEST_CASE("guild window: the roster list is built once a frame, and again after a click") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("guild_window");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");

        const std::string list = "'GuildManagementWnd', 'MemberList'";
        conn.receive_zone_packet_for_test(roster({"Alice", "Bob", "Carl"}));
        api.begin_frame();
        CHECK(run(L, "return (hs.ListInfo(" + list + "))") == "3");
        CHECK(run(L, "return hs.ListRow(" + list + ", 1)[1]") == "Bob");
        CHECK(run(L, "return (select(3, hs.ListInfo(" + list + ")))") == "-1");
        // A click selects at once, in the same frame.
        CHECK(run(L, "return hs.ListClick(" + list + ", 2, 'left')") == "true");
        CHECK(run(L, "return (select(3, hs.ListInfo(" + list + ")))") == "2");
        // A new roster shows from the next frame, the selection kept by name.
        conn.receive_zone_packet_for_test(roster({"Aaron", "Alice", "Bob", "Carl"}));
        api.begin_frame();
        CHECK(run(L, "return (hs.ListInfo(" + list + "))") == "4");
        CHECK(run(L, "return hs.ListRow(" + list + ", 0)[1]") == "Aaron");
        CHECK(run(L, "return (select(3, hs.ListInfo(" + list + ")))") == "3");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("guild window: Go to URL and Join Channel send the guild's text as it is; only an http(s) page opens") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("guild_window_links");
        GameDir game(scratch.path());
        plugin::Hooks hooks;
        net::ConnectionOptions opts;
        opts.hooks = &hooks;
        net::GameConnection conn(game, opts);
        // MQ's command filter (${...} with the player's read permission), then what reaches the interpreter.
        mq::Registry reg;
        mq::register_game_tlos(reg);
        mq::Context mctx;
        mctx.registry = &reg;
        mctx.conn = &conn;
        mq::CommandEnv ce;
        ce.hooks = &hooks;
        ce.registry = &reg;
        ce.context = [&]() -> const mq::Context& { return mctx; };
        mq::Commands commands(ce);
        std::vector<std::string> lines;
        const auto capture = hooks.new_owner("capture");
        hooks.on_command_line(capture, [&](std::string& line) {
            lines.push_back(line);
            return plugin::Flow::Drop;
        });

        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        auto click = [&](const char* button) {
            api.begin_frame();
            return run(L, std::string("return hs.ClickChild('GuildManagementWnd', '") + button + "', 'left')");
        };
        auto set = [&](std::uint32_t action, const std::string& text) {
            net::AppPacket p = net::encode_guild_update_text(action, text);
            conn.receive_zone_packet_for_test(p);
        };

        // An officer's text holding ${...}: sent as it is, not filled in with the clicker's state.
        set(0, "https://guild.example.org/?who=${Me.Name}");
        set(1, "chan${Me.Name}");
        CHECK(click("GotoURL") == "true");
        CHECK(click("JoinChannel") == "true");
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "/url https://guild.example.org/?who=${Me.Name}");
        CHECK(lines[1] == "/join chan${Me.Name}");

        // Only web pages: another scheme is not opened; one without a scheme gets http://, as /www.
        lines.clear();
        set(0, "javascript:alert(1)");
        CHECK(click("GotoURL") == "true");
        set(0, "file:///etc/passwd");
        CHECK(click("GotoURL") == "true");
        CHECK(lines.empty());
        set(0, "www.guild.example.org");
        CHECK(click("GotoURL") == "true");
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == "/url http://www.guild.example.org");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}
