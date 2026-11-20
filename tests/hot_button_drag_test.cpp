// Hotbuttons on the cursor (app/lua_ui.cpp hs.PickUp, app/lua_ui_hud.cpp): dragged from the Actions window or
// off a bar, put on a bar by a click on a hotbutton (what was there goes on the cursor).
#include <doctest/doctest.h>

#include <filesystem>
#include <memory>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "game/hotbuttons.h"
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

} // namespace

TEST_CASE("Hotbuttons: an Actions button dragged to a bar, moved off it and swapped") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        game::HotButtonStore store;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.hot_buttons = &store;
        host.set_hot_button = [&](int bar, int button, const game::HotButton& b) { store.current(bar, button) = b; };
        int pressed = -1;
        host.press_hot_button = [&](int, int button, bool) { pressed = button; };
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");

        CHECK(run(L, "return hs.HoldingButton()") == "false");
        CHECK(run(L, "return hs.PickUp('ActionsWindow', 'AMP_WhoButton')") == "true");
        CHECK(run(L, "return hs.HoldingButton()") == "true");
        // only one button on the cursor at a time
        CHECK(run(L, "return hs.PickUp('ActionsWindow', 'AMP_CampButton')") == "false");
        // an empty social makes no button
        run(L, "hs.ClearCursorButton()");
        CHECK(run(L, "return hs.PickUp('ActionsWindow', 'ASP_SocialButton1')") == "false");

        CHECK(run(L, "return hs.PickUp('ActionsWindow', 'AMP_WhoButton')") == "true");
        CHECK(run(L, "return hs.ClickHotButton('HotButtonWnd', 'HB_Button3', 'left')") == "true");
        CHECK(pressed == -1); // put there, not pressed
        CHECK(store.current(0, 2).type == game::HotButtonType::MenuButton);
        CHECK(store.current(0, 2).slot == static_cast<int>(game::MenuButton::Who));
        CHECK(run(L, "return hs.HoldingButton()") == "false");

        // a click on it now presses it
        run(L, "hs.ClickHotButton('HotButtonWnd', 'HB_Button3', 'left')");
        CHECK(pressed == 2);

        // the melee button on button 1, then Who dragged onto it: Melee comes onto the cursor
        CHECK(run(L, "return hs.PickUp('ActionsWindow', 'ACP_MeleeAttackButton')") == "true");
        run(L, "hs.ClickHotButton('HotButtonWnd', 'HB_Button1', 'left')");
        CHECK(store.current(0, 0).type == game::HotButtonType::WeaponSlot);
        CHECK(run(L, "return hs.PickUp('HotButtonWnd', 'HB_Button3')") == "true");
        CHECK(store.current(0, 2).empty()); // moved off the bar
        run(L, "hs.ClickHotButton('HotButtonWnd', 'HB_Button1', 'left')");
        CHECK(store.current(0, 0).type == game::HotButtonType::MenuButton);
        CHECK(host.cursor_button);
        CHECK(host.cursor_button->type == game::HotButtonType::WeaponSlot);
        // an empty button gives nothing to drag
        run(L, "hs.ClearCursorButton()");
        CHECK(run(L, "return hs.PickUp('HotButtonWnd', 'HB_Button5')") == "false");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Hotbuttons: a layout-locked bar rejects pickup and placement but can still be used") {
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ctx(ImGui::CreateContext(),
                                                                      &ImGui::DestroyContext);
    game::HotButtonStore store;
    store.current(0, 0).type = game::HotButtonType::WeaponSlot;
    store.current(1, 0).type = game::HotButtonType::MenuButton;
    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.hot_buttons = &store;
    host.set_hot_button = [&](int bar, int button, const game::HotButton& b) { store.current(bar, button) = b; };
    bool locked = true;
    host.layout_setting = [&](std::string_view key) -> std::string {
        if (key == "hud.lockLayout.HotButtonWnd2") return locked ? "1" : "0";
        if (key == "hud.lock.HotButtonWnd2" || key == "hud.locked") return "1";
        return {};
    };
    int presses = 0;
    host.press_hot_button = [&](int bar, int button, bool mouse) {
        CHECK(bar == 1);
        CHECK(button == 0);
        CHECK(mouse);
        ++presses;
    };
    app::UiApi api(host);
    std::unique_ptr<lua_State, decltype(&lua_close)> lua(luaL_newstate(), &lua_close);
    REQUIRE(lua);
    luaL_openlibs(lua.get());
    lua_newtable(lua.get());
    api.register_functions(lua.get());
    lua_setglobal(lua.get(), "hs");
    lua_State* L = lua.get();

    CHECK(run(L, "return hs.UI.cursor.PickUp('HotButtonWnd2', 'HB_Button1')") == "false");
    CHECK(store.current(1, 0).type == game::HotButtonType::MenuButton);
    CHECK_FALSE(host.cursor_button);
    CHECK(api.click_hot_button("HotButtonWnd2", "HB_Button1", "left"));
    CHECK(presses == 1);
    CHECK(api.click_child("HotButtonWnd2", "HB_PageDownButton"));
    CHECK(store.page(1) == 1);
    CHECK(api.click_child("HotButtonWnd2", "HB_PageUpButton"));
    CHECK(store.page(1) == 0);

    REQUIRE(run(L, "return hs.UI.cursor.PickUp('HotButtonWnd', 'HB_Button1')") == "true");
    CHECK(run(L, "return hs.UI.hotbars.Click('HotButtonWnd2', 'HB_Button1', 'left')") == "true");
    CHECK(run(L, "return hs.UI.hotbars.Click('HotButtonWnd2', 'HB_Button2', 'left')") == "true");
    CHECK(store.current(1, 0).type == game::HotButtonType::MenuButton);
    CHECK(store.current(1, 1).empty());
    REQUIRE(host.cursor_button);
    CHECK(host.cursor_button->type == game::HotButtonType::WeaponSlot);
    CHECK(presses == 1);
    CHECK(api.click_hot_button("HotButtonWnd", "HB_Button1", "left"));
    CHECK(store.current(0, 0).type == game::HotButtonType::WeaponSlot);
    CHECK_FALSE(host.cursor_button);

    locked = false; // The frame locks remain on.
    CHECK(run(L, "return hs.UI.cursor.PickUp('HotButtonWnd2', 'HB_Button1')") == "true");
    CHECK(api.click_hot_button("HotButtonWnd2", "HB_Button2", "left"));
    CHECK(store.current(1, 0).empty());
    CHECK(store.current(1, 1).type == game::HotButtonType::MenuButton);
    CHECK_FALSE(host.cursor_button);
}

TEST_CASE("Window places: a moved window is where it was after the layout is saved and read again") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
    ImGui::GetIO().IniFilename = nullptr;
    hailstone::test::ScratchDir dir_scratch("layout_test");
    std::filesystem::path dir = dir_scratch.path();
    std::filesystem::create_directories(dir);
    auto place = [&](const char* code) {
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        api.begin_frame();
        std::string out = run(L, code);
        api.begin_frame(); // stores what moved
        api.save_layout();
        lua_close(L);
        return out;
    };
    // placed (where ReplaceWindow put it: not kept), then moved by the player (kept)
    place("local _, x, y, w, h = hs.ReplaceWindow('Chat1'); hs.WindowPlaced('Chat1', x, y, w, h, true); "
          "hs.WindowPlaced('Chat1', 321, 234, 400, 300, true); return ''");
    CHECK(place("local shown, x, y, w, h = hs.ReplaceWindow('Chat1'); return x .. ',' .. y .. ',' .. w .. ',' .. h") ==
          "321,234,400,300");
    std::filesystem::remove_all(dir);
    ImGui::DestroyContext(ctx);
}
