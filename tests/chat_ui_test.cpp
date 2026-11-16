// The chat window script (ui/modern/chat.lua, with the rest of the Modern UI) run for real over Dear ImGui with no renderer: a new window shows,
// tabs draw, the menu's calls exist, and no script error comes up.
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "app/lua_ui.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

fs::path ui_dir() { return fs::path(__FILE__).parent_path().parent_path() / "ui"; }

void frame(ui::lua::LuaUi& lua) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    lua.frame();
    ImGui::Render();
}

} // namespace

TEST_CASE("Chat window script: new windows and tabs draw without script errors") {
    fs::path src = ui_dir() / "modern";
    REQUIRE(fs::exists(src / "chat.lua"));
    hailstone::test::ScratchDir dir_scratch("chat_ui_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        GameDir game(dir / "game");
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        ui::lua::LuaUi lua(dir / "modern", dir / "modern.txt");
        lua.add_ui_api([&](lua_State* L) {
            api.register_functions(L);
            lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
            lua_setfield(L, -2, "InGame");
            hailstone::test::sync_ui_api_aliases(L);
        });
        view.add(ui::ChatChannel::Say, "hello");
        for (int i = 0; i < 3; ++i) {
            api.begin_frame();
            frame(lua);
        }
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(ImGui::FindWindowByName("###modern.Chat1") != nullptr);

        // A new window from the menu's call, a second tab, a tell tab: all draw.
        lua_State* L = lua.state();
        REQUIRE(luaL_dostring(L, "hs.ChatNewWindow('Fights'); hs.ChatNewTab(1, 'Social'); hs.ChatSetTellWindows(true)") == LUA_OK);
        view.add(ui::ChatChannel::Tell, "Bob tells you, 'hi'", 0, 257, "Bob");
        for (int i = 0; i < 3; ++i) {
            api.begin_frame();
            frame(lua);
        }
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(ImGui::FindWindowByName("###modern.Chat2") != nullptr);
        // the main window, titled by the tab that is up (the new one), is still the same ImGui window
        ImGuiWindow* main = ImGui::FindWindowByName("###modern.Chat1");
        REQUIRE(main);
        CHECK(main->Active);
        CHECK(main->ID == ImHashStr("###modern.Chat1")); // one window whatever its tab
    }
    ImGui::DestroyContext(ctx);
    fs::remove_all(dir);
}

// The chat windows (frames, not dialogs) fold into their tab strip, as the combat ability window folds into its
// title bar: the strip stays (the tabs are still there), the output and the input go, and the window is as tall as
// the strip. The button that does it is at the strip's right end.
TEST_CASE("Chat window script: a chat rolls up into its tab strip") {
    fs::path src = ui_dir() / "modern";
    REQUIRE(fs::exists(src / "chat.lua"));
    hailstone::test::ScratchDir dir_scratch("chat_ui_rollup_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        GameDir game(dir / "game");
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        ui::lua::LuaUi lua(dir / "modern", dir / "modern.txt");
        lua.add_ui_api([&](lua_State* L) {
            api.register_functions(L);
            lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
            lua_setfield(L, -2, "InGame");
            hailstone::test::sync_ui_api_aliases(L);
        });
        view.add(ui::ChatChannel::Say, "hello");
        lua_State* L = lua.state();
        auto step = [&] {
            api.begin_frame();
            frame(lua);
        };
        auto setting = [&](const char* key) {
            const std::string code = std::string("return ui.GetSetting('") + key + "') or ''";
            REQUIRE(luaL_dostring(L, code.c_str()) == LUA_OK);
            const char* v = lua_tostring(L, -1);
            const std::string out = v ? v : "";
            lua_settop(L, 0);
            return out;
        };
        for (int i = 0; i < 3; ++i) step();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        ImGuiWindow* chat = ImGui::FindWindowByName("###modern.Chat1");
        REQUIRE(chat);
        const float open = chat->Size.y;
        CHECK(open > 100); // (the chat is open)
        REQUIRE(luaL_dostring(L, "return ui.GetIO().uiScale or 1") == LUA_OK);
        const float scale = static_cast<float>(lua_tonumber(L, -1));
        lua_settop(L, 0);
        ImGuiIO& io = ImGui::GetIO();
        // the button at the tab strip's right end (the strip is 26 design pixels tall, the button 16)
        auto clickButton = [&] {
            const ImVec2 at(chat->Pos.x + chat->Size.x - 14 * scale, chat->Pos.y + 13 * scale);
            io.AddMousePosEvent(at.x, at.y);
            step();
            io.AddMouseButtonEvent(0, true);
            step();
            io.AddMouseButtonEvent(0, false);
            step();
            io.AddMousePosEvent(-1000, -1000);
            step();
            step();
        };
        clickButton();
        CHECK(setting("hud.rolled.Chat1") == "1");
        CHECK(chat->Size.y == doctest::Approx(26.0f * scale)); // (the strip only)
        // the output is not drawn any more
        ImGuiWindow* output = ImGui::FindWindowByName("output");
        const bool output_hidden = output == nullptr || output->LastFrameActive < ImGui::GetFrameCount();
        CHECK(output_hidden);
        // and it opens again
        clickButton();
        CHECK(setting("hud.rolled.Chat1") == "");
        CHECK(chat->Size.y == doctest::Approx(open));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}
