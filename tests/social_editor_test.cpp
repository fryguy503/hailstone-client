// A social's lines as a press runs them (game::split_social_line, social_passage_of_time) and the calls the social
// editors use (app/lua_ui_socialedit.cpp: hs.SocialLine, hs.SocialEditorSocial; hs.SetLuaSocial's lines).
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <format>
#include <string>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/actions_ui.h"
#include "app/lua_ui.h"
#include "core/config.h"
#include "core/game_dir.h"
#include "game/hot_button_press.h"
#include "game/hotbuttons.h"
#include "game/lua_social.h"
#include "net/game_connection.h"
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

struct Runner : game::SocialRunQueries {
    std::vector<std::string> lines = std::vector<std::string>(5);
    std::vector<std::string> calls;
    bool cooling = false;
    const std::string* line(std::int32_t, std::int32_t n) override { return &lines[static_cast<std::size_t>(n)]; }
    void interpret(const std::string& c) override { calls.push_back(c); }
    void alternate_ability(std::int32_t g) override { calls.push_back(std::format("aa {}", g)); }
    bool cooldown_running(std::int32_t) override { return cooling; }
    void start_cooldown(std::int32_t, std::int32_t ms) override {
        calls.push_back(std::format("cooldown {}", ms));
        cooling = true;
    }
};

} // namespace

TEST_CASE("Social lines: /pause and /timer before a comma, the rest for the interpreter") {
    using game::split_social_line;
    auto p = split_social_line("/pause 5, /bow");
    CHECK(p.pause == 5);
    CHECK(p.timers.empty());
    CHECK(p.command == " /bow"); // (the space after the comma stays; InterpretCmd skips it)
    // Only the comma is skipped: a second prefix follows it with no space, or it is the command.
    p = split_social_line("/TIMER 600,/Pause 9999, /cast 1");
    CHECK(p.timers == std::vector<std::int32_t>{600});
    CHECK(p.pause == 600);
    CHECK(p.command == " /cast 1");
    p = split_social_line("/timer 600, /pause 10");
    CHECK_FALSE(p.pause);
    CHECK(p.command == " /pause 10");
    p = split_social_line("/pause 0");
    CHECK(p.pause == 1);
    CHECK(p.command.empty());
    p = split_social_line("/timer -4,/pause 3,/pause 7, hi");
    CHECK(p.timers == std::vector<std::int32_t>{0});
    CHECK(p.pause == 7);
    CHECK(p.command == " hi");
    p = split_social_line("/pause 2,");
    CHECK(p.command == ","); // one character after the comma is kept as the original keeps it
    p = split_social_line("/say /pause 5");
    CHECK_FALSE(p.pause);
    CHECK(p.command == "/say /pause 5");
}

TEST_CASE("Social lines: a press runs a line a step, waits a step after each and /pause N steps") {
    Runner r;
    r.lines = {"/assist", "", "/timer 300,/pause 3, /attack on", "/timer 50, /say done", ""};
    game::SocialQueue q;
    q.current = 0;
    std::uint32_t now = 1000;
    std::vector<std::string> at; // what ran at each step
    for (int step = 0; step < 12 && q.current >= 0; ++step) {
        now += 101;
        std::size_t before = r.calls.size();
        game::social_passage_of_time(q, now, r);
        std::string ran;
        for (std::size_t i = before; i < r.calls.size(); ++i) ran += (ran.empty() ? "" : "|") + r.calls[i];
        at.push_back(ran);
    }
    // line 1, a step's wait, line 3 (the empty line 2 skipped) and its cooldown, its 3 steps' wait, line 4 (the
    // running cooldown not restarted), a step's wait, then the empty line 5 skipped and the queue ends.
    CHECK(at == std::vector<std::string>{"/assist", "", "cooldown 30000| /attack on", "", "", "", " /say done", "", ""});
    CHECK(q.current == -1);
}

TEST_CASE("Social editor: hs.SocialLine, hs.SocialEditorSocial and hs.SetLuaSocial's lines") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        test::ScratchDir scratch("social_editor");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        game::HotButtonStore store;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.hot_buttons = &store;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");

        CHECK(run(L, R"lua(
local l = hs.SocialLine('/timer 600,/pause 10, /assist')
return string.format('%s %d %d %s %s', l.kind, l.pause, l.timers[1], l.name, l.command)
)lua") == "command 10 600 /assist  /assist");
        CHECK(run(L, "local l = hs.SocialLine('#petcmd attack all') return l.kind .. ' ' .. tostring(l.pause)") ==
              "text nil");
        CHECK(run(L, "return hs.SocialLine('/nosuchcommand').kind") == "invalid");
        CHECK(run(L, "return hs.SocialLine(';tell friend hi').kind") == "chatserver");
        CHECK(run(L, "return hs.SocialLine('/pause 5').kind") == "empty");

        // The editor's social: none until it opens, the one it opened on, none once Accept closes it.
        CHECK(run(L, "return hs.SocialEditorSocial()") == "nil");
        store.social(14).name = "Pet";
        store.social(14).lines[0] = "/pet attack";
        REQUIRE(host.open_social_editor);
        host.open_social_editor(14);
        CHECK(run(L, "return hs.SocialEditorSocial()") == "14");
        run(L, "hs.ClickChild('SocialEditWnd', 'SEW_Accept_Button', 'left')");
        CHECK(run(L, "return hs.SocialEditorSocial()") == "nil");
        host.open_social_editor(14);
        CHECK(run(L, "hs.ShowWindow('SocialEditWnd', false) return hs.SocialEditorSocial()") == "nil");

        // A Lua social back to command lines: the lines given (cut to 255), the Lua gone.
        CHECK(run(L, "return tostring(hs.SetLuaSocial(3, {name = 'Assist', press = \"mq.cmd('/assist')\"}))") ==
              "true");
        CHECK(store.social(3).is_lua());
        CHECK(run(L, R"lua(
return tostring(hs.SetLuaSocial(3, {name = 'Assist', color = 2, press = '', face = '',
                                    lines = {'/assist', '/pause 5, /attack on', string.rep('x', 300)}}))
)lua") == "true");
        CHECK_FALSE(store.social(3).is_lua());
        CHECK(store.social(3).color == 2);
        CHECK(store.social(3).lines[0] == "/assist");
        CHECK(store.social(3).lines[1] == "/pause 5, /attack on");
        CHECK(store.social(3).lines[2].size() == 255);
        CHECK(store.social(3).lines[3].empty());
        // Lines given with Lua are not kept: a Lua social has none.
        CHECK(run(L, "return tostring(hs.SetLuaSocial(3, {name = 'Assist', press = 'x()', lines = {'/sit'}}))") ==
              "true");
        CHECK(store.social(3).lines[0].empty());
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

// lib/social_lines.lua (both editors): a line put together from its parts reads back the same, the timeline, and
// command lines to Lua and back.
TEST_CASE("Social editor: the modern pack's social_lines.lua over hs.SocialLine") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        test::ScratchDir scratch("social_lines");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        game::HotButtonStore store;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.hot_buttons = &store;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        const std::filesystem::path lib =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "ui" / "modern" / "lib" / "social_lines.lua";
        REQUIRE(std::filesystem::exists(lib));
        REQUIRE(luaL_dofile(L, lib.string().c_str()) == LUA_OK);
        lua_setglobal(L, "SL");

        CHECK(run(L, "return SL.Compose(600, 10, '/assist')") == "/timer 600,/pause 10, /assist");
        CHECK(run(L, "return SL.Compose(nil, 5, '')") == "/pause 5");
        CHECK(run(L, "return SL.Compose(nil, nil, '/sit')") == "/sit");
        CHECK(run(L, R"lua(
local p = SL.Parts(SL.Compose(600, 10, '/assist'))
return string.format('%d %d [%s] %s', p.timer, p.pause, p.command, p.name)
)lua") == "600 10 [/assist] /assist");
        CHECK(run(L, "local p = SL.Parts('/timer 0,/timer 50, hi') return p.timer .. ' ' .. p.kind") == "50 text");

        // A line a step, a step's wait after it, N after /pause N.
        CHECK(run(L, R"lua(
local t = SL.Timeline({'/assist', '', '/pause 5, /x', '/y', ''})
return string.format('%.1f %s %.1f %.1f %.1f %d', t.at[1], tostring(t.at[2]), t.at[3], t.at[4], t.total, t.runs)
)lua") == "0.0 nil 0.2 0.8 0.8 3");

        // Commands to Lua and back.
        const std::string lua = run(L, "return SL.ToLua({'/assist', '/pause 5, /pet attack', '/attack on', '', ''})");
        CHECK(lua == "mq.cmd(\"/assist\")\nmq.cmd(\"/pet attack\")\n"
                     "mq.delay(500) -- /pause 5 (waits need a server in full mode)\nmq.cmd(\"/attack on\")");
        CHECK(run(L, "return table.concat(SL.FromLua(SL.ToLua({'/assist', '/pause 5, /pet attack', '/attack on'})), '|')") ==
              "/assist|/pause 5, /pet attack|/attack on||");
        CHECK(run(L, "return SL.ToLua({'/timer 600, /shout Ready'})") ==
              "-- /timer 600: a 60 s cooldown (a face can show one)\nmq.cmd(\"/shout Ready\")");
        CHECK(run(L, "return table.concat(SL.FromLua(\"-- note\\nmq.delay(1000)\\nmq.cmd('/sit');\", ''), '|')") ==
              "/pause 10|/sit|||"); // (a wait before any command: a line that only waits)
        CHECK(run(L, "return select(2, SL.FromLua(\"mq.cmd('/sit')\", 'return \\\"x\\\"'))") ==
              "It has a face: command lines cannot show one.");
        CHECK(run(L, "return tostring(SL.FromLua(\"if button.shift then mq.cmd('/sit') end\", ''))") == "nil");
        CHECK(run(L, "return tostring(SL.FromLua(\"mq.cmd('/say it''s')\", ''))") == "nil");
        CHECK(run(L, "return select(2, SL.FromLua(string.rep(\"mq.cmd('/sit')\\n\", 6), ''))") ==
              "It sends 6 commands; a social has 5 lines.");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

// Share text for a plain social: its lines in a "-- [lines]" section (empty ones kept up to the last), read back the
// same; a Lua social's file has none, and lines given with Lua are not written.
TEST_CASE("Social share text: a plain social's lines") {
    game::LuaSocialFile plain{"Assist", 2, {}, {"/assist", "", "/pause 5, /attack on", "", ""}};
    const std::string file = game::format_lua_social(plain);
    CHECK(file == "-- Hailstone Lua social\n-- name: Assist\n-- color: 2\n-- [lines]\n/assist\n\n/pause 5, /attack on\n");
    auto back = game::import_lua_social(game::export_lua_social(plain));
    REQUIRE(back);
    CHECK(back->name == "Assist");
    CHECK(back->color == 2);
    CHECK(back->lua.empty());
    CHECK(back->lines == std::vector<std::string>{"/assist", "", "/pause 5, /attack on"});
    // a line end inside a line, a sixth line and a long line
    game::LuaSocialFile odd{"Odd", 0, {}, {"/say a\nb", std::string(300, 'x'), "3", "4", "5", "/sixth"}};
    back = game::import_lua_social(game::export_lua_social(odd));
    REQUIRE(back);
    REQUIRE(back->lines.size() == 5);
    CHECK(back->lines[0] == "/say a b");
    CHECK(back->lines[1].size() == 255);
    // Lua wins over lines
    game::LuaSocialFile lua{"L", 0, {"mq.cmd('/sit')", ""}, {"/stand"}};
    CHECK(game::format_lua_social(lua).find("[lines]") == std::string::npos);
    back = game::import_lua_social(game::export_lua_social(lua));
    REQUIRE(back);
    CHECK(back->lua.press == "mq.cmd('/sit')");
    CHECK(back->lines.empty());
}

TEST_CASE("Social editor: hs.SlashCommands, the share text of an unsaved social, and pressing one from the editor") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        test::ScratchDir scratch("social_editor_press");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        Config config;
        app::ActionsUi actions(config, scratch.path() / "hailstone.ini");
        game::HotButtonStore& store = actions.hot_buttons();
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.hot_buttons = &store;
        host.actions = &actions;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");

        // The game's commands that start with what was typed, in the order it tries them; none for a non-command.
        CHECK(run(L, "return hs.SlashCommands('/ASSI')[1]") == "/assist");
        CHECK(run(L, "local l = hs.SlashCommands('/g') return tostring(#l > 3) .. ' ' .. l[1]") == "true /gsay");
        CHECK(run(L, "return #hs.SlashCommands('/zzzz')") == "0");
        CHECK(run(L, R"lua(
for _, c in ipairs(hs.SlashCommands('/')) do if c:sub(1, 1) ~= '/' then return c end end
return 'all slash'
)lua") == "all slash");

        // Share text of what the editor shows (unsaved), read back with its lines.
        CHECK(run(L, R"lua(
local text = hs.LuaSocialExport(0, {name = 'Pull', color = 13, lines = {'/assist', '', '/g Incoming', '', ''}})
local t = hs.LuaSocialImport(text)
return string.format('%s %d %s [%s] [%s] [%s] [%s]', t.name, t.color, tostring(t.lua), t.lines[1], t.lines[2],
                     t.lines[3], t.lines[5])
)lua") == "Pull 13 false [/assist] [] [/g Incoming] []");
        CHECK(run(L, "return tostring(hs.LuaSocialImport(hs.LuaSocialExport(0, {name = 'L', press = 'x()'})).lua)") ==
              "true");
        // A saved plain social's share text has its lines too.
        store.social(4).name = "Med";
        store.social(4).lines[0] = "/sit";
        CHECK(run(L, "return hs.LuaSocialImport(hs.LuaSocialExport(4)).lines[1]") == "/sit");

        // Test: queued as a hot button's press queues it; Stop as /stopsocial.
        CHECK(run(L, "return hs.CurrentSocial()") == "nil");
        CHECK(run(L, "return tostring(hs.PressSocial(4))") == "true");
        CHECK(actions.current_social() == 4);
        CHECK(run(L, "return hs.CurrentSocial()") == "4");
        CHECK(run(L, "hs.StopSocial() return hs.CurrentSocial()") == "nil");
        CHECK(run(L, "return hs.PressSocial(120)") == "nil");
        // A Lua social runs its Lua instead of being queued.
        int lua_pressed = -1;
        app::HotButtonHooks hooks;
        hooks.press_lua_social = [&](std::int32_t social, int, int) { lua_pressed = social; };
        actions.set_hooks(hooks);
        store.social(5).name = "L";
        store.social(5).lua.press = "mq.cmd('/sit')";
        run(L, "hs.PressSocial(5)");
        CHECK(lua_pressed == 5);
        CHECK(actions.current_social() == -1);
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

// Both packs read lines with the same lib/social_lines.lua: the gamepad pack's is a copy, kept the same.
TEST_CASE("Social editor: the packs' social_lines.lua are the same file") {
    const std::filesystem::path ui = std::filesystem::path(__FILE__).parent_path().parent_path() / "ui";
    auto read = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    const std::string modern = read(ui / "modern" / "lib" / "social_lines.lua");
    CHECK_FALSE(modern.empty());
    CHECK(modern == read(ui / "gamepad" / "lib" / "social_lines.lua"));
}
