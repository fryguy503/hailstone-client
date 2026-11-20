// Run Modern chat over Dear ImGui without a renderer, including real input events on its context menus.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "app/chat_context_menu.h"
#include "app/chat_tab_bar.h"
#include "app/chat_tabs.h"
#include "app/lua_ui.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"
#include "ui/ui_ini.h"
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
        host.layout_setting = [&](std::string_view key) { return lua.setting(key); };
        host.set_layout_setting = [&](std::string_view key, std::string_view value) { lua.set_setting(key, value); };
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
        // The frame keeps its ImGui identity across changes to the selected tab.
        ImGuiWindow* main = ImGui::FindWindowByName("###modern.Chat1");
        REQUIRE(main);
        CHECK(main->Active);
        CHECK(main->ID == ImHashStr("###modern.Chat1")); // one window whatever its tab

        namespace tabs = app::chat_tabs;
        const int social = tabs::group(1).selected;
        REQUIRE(social > 1);
        CHECK(tabs::tab(social).title == "Social");
        auto step = [&] {
            api.begin_frame();
            frame(lua);
        };
        ImGuiIO& io = ImGui::GetIO();
        auto click_menu_item = [&](int tab, const char* label) {
            CAPTURE(label);
            const auto rows = app::chat_context_rows(tab);
            const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& item) { return item.text == label; });
            REQUIRE(row != rows.end());
            const auto index = static_cast<int>(row - rows.begin());
            step();
            REQUIRE(ctx->OpenPopupStack.Size > 0);
            const auto* popup = ctx->OpenPopupStack[0].Window;
            REQUIRE(popup);
            const auto item = ImHashStr(label, 0, ImHashData(&index, sizeof(index), popup->ID));
            // Find the real selectable row without duplicating the menu's order or style in the test.
            const auto end = popup->Pos.y + popup->Size.y;
            bool hovered = false;
            for (float y = popup->Pos.y + 2; y < end && !hovered; y += 4) {
                io.AddMousePosEvent(popup->Pos.x + 10, y);
                step();
                hovered = ctx->HoveredId == item;
            }
            REQUIRE(hovered);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            step();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            step();
        };
        io.AddMousePosEvent(main->Pos.x + 18, main->Pos.y + 13);
        step();
        step();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        step();
        const auto strip_id = ImHashStr("chat-tabs", 0, main->ID);
        const auto menu_id = ImHashStr("tab-context", 0, strip_id);
        REQUIRE(ctx->OpenPopupStack.Size > 0);
        CHECK(ctx->OpenPopupStack[0].PopupId == menu_id);
        CHECK(tabs::group(1).selected == social);
        const bool highlight = tabs::tab(1).highlight;
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        click_menu_item(1, "Highlight New Messages");
        CHECK(tabs::tab(1).highlight != highlight);
        CHECK(tabs::group(1).selected == social);

        // The selected tab's full menu is also available from the chat body.
        io.AddMousePosEvent(main->Pos.x + 18, main->Pos.y + 60);
        step();
        step();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        step();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        click_menu_item(social, "Detach Tab");
        const int detached = tabs::group_of(social);
        REQUIRE(detached > 1);
        CHECK(tabs::group(detached).selected == social);
        CHECK_FALSE(lua.setting("hud.pos.Chat" + std::to_string(detached)).empty());
        step();
        step();
        const auto* detached_window = ImGui::FindWindowByName(("###modern.Chat" + std::to_string(detached)).c_str());
        REQUIRE(detached_window);
        CHECK(detached_window->Active);
        CHECK(detached_window->Size.x == doctest::Approx(main->Size.x));

        REQUIRE(tabs::move(social, 1));
        step();
        CHECK_FALSE(detached_window->Active);
        CHECK(tabs::group(1).selected == social);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
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

TEST_CASE("Native chat tabs keep their identity, routing and flash settings across moves and layout reloads") {
    namespace tabs = app::chat_tabs;
    test::ScratchDir scratch("native_chat_tab_model");
    const auto dir = scratch.path();
    const auto layout = dir / "UI.ini";
    fs::create_directories(dir / "game");
    const std::string original_layout = "[ChatManager]\nNumWindows=1\nChatWindow1_LanguageId=0\n"
                                        "ChatWindow1_Name=Social\nChatWindow1_Dock=0\nChannelMap5=1\n";
    std::ofstream(layout) << original_layout;

    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ctx(ImGui::CreateContext(), ImGui::DestroyContext);
    GameDir game(dir / "game");
    net::GameConnection conn(game);
    ui::ChatView view;
    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.game = &game;
    host.conn = &conn;
    host.chat = &view;
    app::UiApi api(host);
    api.load_layout({}, layout, {});
    // The chat manager is a process-wide singleton, and this test moves the main window's tabs away. Put the layout it
    // started from back when the test ends, also when a REQUIRE stops it early, or a later test in this process finds
    // no Chat1. A separate file, since load_layout saves the current layout before it loads.
    const auto restore = dir / "restore.ini";
    std::ofstream(restore) << original_layout;
    struct RestoreLayout {
        app::UiApi& api;
        fs::path file;
        ~RestoreLayout() { api.load_layout({}, file, {}); }
    } restore_layout{api, restore};
    std::unique_ptr<lua_State, decltype(&lua_close)> lua(luaL_newstate(), lua_close);
    lua_newtable(lua.get());
    api.register_functions(lua.get());
    lua_setglobal(lua.get(), "hs");
    auto sync = [&] { REQUIRE(luaL_dostring(lua.get(), "hs.ChatWindows()") == LUA_OK); };

    REQUIRE(tabs::group(1).tabs.size() == 2);
    CHECK(tabs::group(1).selected == 1);
    const auto serial = tabs::tab(2).serial;
    view.add(ui::ChatChannel::OOC, "first");
    sync();
    REQUIRE(tabs::tab(2).unread);
    CHECK(tabs::highlight_visible(2, 0.25));
    CHECK_FALSE(tabs::highlight_visible(2, 0.75));
    CHECK(tabs::highlight_visible(2, 5.25));
    REQUIRE(tabs::set_highlight(2, false));
    CHECK(tabs::tab(2).unread);
    CHECK_FALSE(tabs::highlight_visible(2, 0.25));

    const int target = tabs::detach(1);
    REQUIRE(target > 1);
    CHECK(tabs::group(target).main);
    CHECK_FALSE(tabs::close_group(target));
    CHECK_FALSE(tabs::close(1));
    CHECK(tabs::group(1).selected == 2);
    CHECK_FALSE(tabs::tab(2).unread);
    REQUIRE(tabs::move(2, target));
    CHECK(tabs::group(1).slot == 0);
    REQUIRE(tabs::reorder(1, 1));
    CHECK(tabs::group(target).selected == 1);
    REQUIRE(tabs::rename_group(target, "Conversation"));
    CHECK(tabs::tab(2).title == "Social");
    REQUIRE(tabs::set_color(2, 0x123456));
    view.add(ui::ChatChannel::OOC, "after move");
    sync();
    CHECK(view.lines.front().window == serial);
    CHECK(view.lines.back().window == serial);

    api.save_layout();
    api.load_layout({}, layout, {});
    const auto group = tabs::group(target);
    REQUIRE(group.tabs.size() == 2);
    CHECK(group.tabs[0] == 2);
    CHECK(group.tabs[1] == 1);
    CHECK(group.selected == 1);
    CHECK(group.title == "Conversation");
    CHECK(group.main);
    CHECK(tabs::tab(2).highlight_color == 0x123456);
    CHECK_FALSE(tabs::tab(2).highlight);
}

TEST_CASE("Native chat tabs: a layout saved with _Dock and _Up loads its frames, tabs and the tab that was up") {
    namespace tabs = app::chat_tabs;
    test::ScratchDir scratch("native_chat_tab_legacy");
    const auto dir = scratch.path();
    const auto layout = dir / "UI.ini";
    fs::create_directories(dir / "game");
    // Main (0) with Guild (3) docked and up; Combat (1) a window of its own with Loot (2) docked and up; window 4
    // closed (no language key) and window 5 docked in it, so a window of its own.
    std::ofstream(layout) << "[ChatManager]\nNumWindows=5\nChatWindow0_Up=3\n"
                             "ChatWindow1_LanguageId=0\nChatWindow1_Name=Combat\nChatWindow1_Dock=-1\nChatWindow1_Up=2\n"
                             "ChatWindow2_LanguageId=0\nChatWindow2_Name=Loot\nChatWindow2_Dock=1\n"
                             "ChatWindow3_LanguageId=0\nChatWindow3_Name=Guild\nChatWindow3_Dock=0\n"
                             "ChatWindow5_LanguageId=0\nChatWindow5_Name=Raid\nChatWindow5_Dock=4\n";

    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ctx(ImGui::CreateContext(), ImGui::DestroyContext);
    GameDir game(dir / "game");
    net::GameConnection conn(game);
    ui::ChatView view;
    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.game = &game;
    host.conn = &conn;
    host.chat = &view;
    app::UiApi api(host);
    api.load_layout({}, layout, {});
    auto check = [&] {
        const auto main = tabs::group(1);
        REQUIRE(main.tabs.size() == 2);
        CHECK(main.main);
        CHECK(main.tabs[0] == 1);
        CHECK(main.tabs[1] == 4);
        CHECK(main.selected == 4);
        const auto combat = tabs::group(2);
        REQUIRE(combat.tabs.size() == 2);
        CHECK(combat.title == "Combat");
        CHECK(combat.tabs[0] == 2);
        CHECK(combat.tabs[1] == 3);
        CHECK(combat.selected == 3);
        CHECK(tabs::group(3).slot == 0);
        CHECK(tabs::group(4).slot == 0);
        const auto raid = tabs::group(6);
        REQUIRE(raid.tabs.size() == 1);
        CHECK(raid.tabs[0] == 6);
    };
    check();
    // Saved in the new keys, it loads the same.
    api.save_layout();
    api.load_layout({}, layout, {});
    check();
}

// With Highlight New Messages off, a background tab with unseen lines shows the dot after its label, as the Lua
// strip did; with it on, the tab is drawn in its highlight colour instead and gets no dot.
TEST_CASE("Chat window script: a tab that does not flash shows an unread dot") {
    namespace tabs = app::chat_tabs;
    tabs::Tab quiet;
    quiet.unread = true;
    quiet.highlight = false;
    CHECK(app::unread_dot(quiet, false));
    CHECK_FALSE(app::unread_dot(quiet, true)); // (the tab that is up)
    tabs::Tab flashing = quiet;
    flashing.highlight = true;
    CHECK_FALSE(app::unread_dot(flashing, false));
    tabs::Tab seen = quiet;
    seen.unread = false;
    CHECK_FALSE(app::unread_dot(seen, false));

    fs::path src = ui_dir() / "modern";
    hailstone::test::ScratchDir dir_scratch("chat_ui_unread_dot");
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
        lua_State* L = lua.state();
        auto step = [&] {
            api.begin_frame();
            frame(lua);
        };
        ImGui::GetIO().AddMousePosEvent(-1000, -1000);
        const int social = tabs::new_tab(1, "Social");
        REQUIRE(social > 1);
        REQUIRE(luaL_dostring(L, "hs.ChatSetFilter(1, 5, true)") == LUA_OK); // OOC to Social (the tab up)
        REQUIRE(tabs::select(1, 1));
        REQUIRE(tabs::set_highlight(social, false));
        for (int i = 0; i < 3; ++i) step();
        ImGuiWindow* chat = ImGui::FindWindowByName("###modern.Chat1");
        REQUIRE(chat);
        const int before = chat->DrawList->VtxBuffer.Size;
        view.add(ui::ChatChannel::OOC, "to the background tab");
        step();
        step();
        REQUIRE(tabs::tab(social).unread);
        CHECK(chat->DrawList->VtxBuffer.Size > before); // the dot
        // Flashing on: the label takes the highlight colour, and there is no dot.
        REQUIRE(tabs::set_highlight(social, true));
        step();
        step();
        CHECK(chat->DrawList->VtxBuffer.Size == before);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// CChatWindowManager::CChatWindowManager @0x654c80: another window's FontStyle outside 0..7 loads as 3 (RoF2's own
// range there, though /chatfontsize sets 0..10); the main window's is not checked by the original, whose SetChatFont
// @0x656370 throws outside the font list: Hailstone takes 0..10 and otherwise 3.
TEST_CASE("Chat windows: saved font sizes load over RoF2's ranges") {
    auto fonts = [](const std::string& main_font) {
        test::ScratchDir scratch("chat_font_load");
        const auto dir = scratch.path();
        const auto layout = dir / "UI.ini";
        fs::create_directories(dir / "game");
        std::ofstream(layout) << "[ChatManager]\nNumWindows=4\nChatWindow0_FontStyle=" << main_font << "\n"
                                 "ChatWindow1_LanguageId=0\nChatWindow1_Dock=-1\nChatWindow1_FontStyle=9\n"
                                 "ChatWindow2_LanguageId=0\nChatWindow2_Dock=-1\nChatWindow2_FontStyle=7\n"
                                 "ChatWindow3_LanguageId=0\nChatWindow3_Dock=-1\nChatWindow3_FontStyle=-1\n"
                                 "ChatWindow4_LanguageId=0\nChatWindow4_Dock=-1\nChatWindow4_FontStyle=0\n";
        std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ctx(ImGui::CreateContext(),
                                                                             ImGui::DestroyContext);
        GameDir game(dir / "game");
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout({}, layout, {});
        REQUIRE(app::chat_tabs::rename_tab(1, "Main")); // (writes every window's settings back)
        const auto* ini = app::chat_ui_ini();
        REQUIRE(ini);
        std::string out;
        for (int w = 0; w <= 4; ++w) {
            if (w) out += ',';
            out += ini->get("ChatManager", "ChatWindow" + std::to_string(w) + "_FontStyle").value_or("?");
        }
        return out;
    };
    CHECK(fonts("9") == "9,3,7,3,0");
    CHECK(fonts("10") == "10,3,7,3,0");
    CHECK(fonts("11") == "3,3,7,3,0");
    CHECK(fonts("-2") == "3,3,7,3,0");
}
