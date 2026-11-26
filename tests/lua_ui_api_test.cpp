// The Lua UI's ui table (src/ui/lua/lua_imgui.cpp): the key names, ui.CaptureKeyboard, ui.GetIO().typing and
// ui.DrawText's font size, which the server windows of the Triune plugin use.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "ui/imgui_layer.h"
#include "ui/lua/lua_host.h"
#include "ui/texture_ids.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;

TEST_CASE("Lua UI: all public window flags match ImGui and scrollbar flags reach window creation") {
    hailstone::test::ScratchDir scratch("lua_ui_scrollbar_flags");
    const auto dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "scrollbars.lua") << R"lua(
        local flags = ui.WindowFlags.HorizontalScrollbar | ui.WindowFlags.AlwaysVerticalScrollbar
            | ui.WindowFlags.AlwaysHorizontalScrollbar | ui.WindowFlags.NoSavedSettings
        ui.window('Scrollbar flags', function()
            ui.SetNextWindowSize(260, 180, ui.Cond.Always)
            ui.Begin('Scrollbar flags', nil, flags)
            ui.BeginChild('Scrollbar child', 160, 80, false, flags)
            ui.Text('Short content')
            ui.EndChild()
            ui.End()
        end)
    )lua";
    std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "settings.txt");
    for (const auto& [name, value] : {
            std::pair{"None", ImGuiWindowFlags_None},
            std::pair{"NoTitleBar", ImGuiWindowFlags_NoTitleBar},
            std::pair{"NoResize", ImGuiWindowFlags_NoResize},
            std::pair{"NoMove", ImGuiWindowFlags_NoMove},
            std::pair{"NoScrollbar", ImGuiWindowFlags_NoScrollbar},
            std::pair{"NoScrollWithMouse", ImGuiWindowFlags_NoScrollWithMouse},
            std::pair{"NoCollapse", ImGuiWindowFlags_NoCollapse},
            std::pair{"AlwaysAutoResize", ImGuiWindowFlags_AlwaysAutoResize},
            std::pair{"NoBackground", ImGuiWindowFlags_NoBackground},
            std::pair{"NoSavedSettings", ImGuiWindowFlags_NoSavedSettings},
            std::pair{"NoMouseInputs", ImGuiWindowFlags_NoMouseInputs},
            std::pair{"MenuBar", ImGuiWindowFlags_MenuBar},
            std::pair{"HorizontalScrollbar", ImGuiWindowFlags_HorizontalScrollbar},
            std::pair{"NoFocusOnAppearing", ImGuiWindowFlags_NoFocusOnAppearing},
            std::pair{"NoBringToFrontOnFocus", ImGuiWindowFlags_NoBringToFrontOnFocus},
            std::pair{"AlwaysVerticalScrollbar", ImGuiWindowFlags_AlwaysVerticalScrollbar},
            std::pair{"AlwaysHorizontalScrollbar", ImGuiWindowFlags_AlwaysHorizontalScrollbar},
            std::pair{"NoNavInputs", ImGuiWindowFlags_NoNavInputs},
            std::pair{"NoNavFocus", ImGuiWindowFlags_NoNavFocus},
            std::pair{"UnsavedDocument", ImGuiWindowFlags_UnsavedDocument},
            std::pair{"NoNav", ImGuiWindowFlags_NoNav},
            std::pair{"NoDecoration", ImGuiWindowFlags_NoDecoration},
            std::pair{"NoInputs", ImGuiWindowFlags_NoInputs}}) {
        CAPTURE(name);
        CHECK(lua.run_for_test("", std::string("return ui.WindowFlags.") + name) == std::to_string(value));
    }
    CHECK(lua.run_for_test("", "local count=0 for _ in pairs(ui.WindowFlags) do count=count+1 end return count") == "23");
    CHECK(lua.run_for_test("", "return ui.WindowFlags.NoNav == (ui.WindowFlags.NoNavInputs | ui.WindowFlags.NoNavFocus)") == "true");
    CHECK(lua.run_for_test("", "return ui.WindowFlags.NoInputs == (ui.WindowFlags.NoMouseInputs | ui.WindowFlags.NoNav)") == "true");
    for (const auto name : {"ChildWindow", "Tooltip", "Popup", "Modal", "ChildMenu", "NavFlattened", "AlwaysUseWindowPadding"}) {
        CAPTURE(name);
        CHECK(lua.run_for_test("", std::string("return ui.WindowFlags.") + name) == "nil");
    }
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::GetIO().DisplaySize = ImVec2(800, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        lua.frame();
        ImGui::Render();
    }
    CHECK(lua.load_status().error_count == 0);
    const auto* window = ImGui::FindWindowByName("Scrollbar flags");
    REQUIRE(window);
    CHECK((window->Flags & ImGuiWindowFlags_HorizontalScrollbar) != 0);
    CHECK((window->Flags & ImGuiWindowFlags_AlwaysVerticalScrollbar) != 0);
    CHECK((window->Flags & ImGuiWindowFlags_AlwaysHorizontalScrollbar) != 0);
    CHECK(window->ScrollbarX);
    CHECK(window->ScrollbarY);
    REQUIRE(window->DC.ChildWindows.Size == 1);
    const auto* child = window->DC.ChildWindows.front();
    CHECK((child->Flags & ImGuiWindowFlags_HorizontalScrollbar) != 0);
    CHECK((child->Flags & ImGuiWindowFlags_AlwaysVerticalScrollbar) != 0);
    CHECK((child->Flags & ImGuiWindowFlags_AlwaysHorizontalScrollbar) != 0);
    CHECK(child->ScrollbarX);
    CHECK(child->ScrollbarY);
}

TEST_CASE("Lua UI: IsItemDisabled and GetStyleAlpha observe widget state without changing it") {
    hailstone::test::ScratchDir scratch("lua_ui_disabled_item");
    const auto dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "items.lua") << R"lua(
        REGISTERED = type(ui.IsItemDisabled) == 'function' and type(ui.GetStyleAlpha) == 'function'
        ui.window('Item state', function()
            ui.Begin('Item state')
            ALPHA_ORIGINAL = ui.GetStyleAlpha()
            ui.PushStyleVar(ui.StyleVar.Alpha, 0.375)
            ALPHA_PUSHED = ui.GetStyleAlpha()
            ui.Button('Enabled')
            ENABLED = ui.IsItemDisabled()
            ui.BeginDisabled(true)
            ui.Button('Disabled')
            ALPHA_DISABLED = ui.GetStyleAlpha()
            ui.EndDisabled()
            ALPHA_REENABLED = ui.GetStyleAlpha()
            DISABLED = ui.IsItemDisabled()
            local id = ui.GetItemID()
            REPEATED = ui.IsItemDisabled() and ui.GetItemID() == id
            ui.BeginDisabled(true)
            ui.BeginDisabled(false)
            ui.Checkbox('Nested disabled', false)
            ui.EndDisabled()
            ui.EndDisabled()
            NESTED = ui.IsItemDisabled()
            ui.Button('Enabled after')
            RESTORED = ui.IsItemDisabled()
            ui.BeginDisabled(true)
            ui.SetNextItemWidth(140)
            SLIDER_VALUE, SLIDER_CHANGED = ui.SliderInt('Visible slider label', 42, 0, 100)
            ui.EndDisabled()
            local sliderId = ui.GetItemID()
            TOTAL = {ui.GetItemRect()}
            CONTROL = {ui.GetItemRect(true)}
            local explicit = {ui.GetItemRect(false)}
            RECT_DEFAULT = explicit[1] == TOTAL[1] and explicit[2] == TOTAL[2]
                and explicit[3] == TOTAL[3] and explicit[4] == TOTAL[4]
            SLIDER_ID = ui.GetItemID() == sliderId
            SLIDER_DISABLED = ui.IsItemDisabled()
            ui.PopStyleVar()
            ALPHA_RESTORED = ui.GetStyleAlpha()
            ui.End()
        end)
    )lua";
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "settings.txt");
        for (int frame = 0; frame < 2; ++frame) {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        }
        CHECK(lua.run_for_test("", "return REGISTERED") == "true");
        CHECK(lua.run_for_test("", "return ENABLED") == "false");
        CHECK(lua.run_for_test("", "return DISABLED") == "true");
        CHECK(lua.run_for_test("", "return REPEATED") == "true");
        CHECK(lua.run_for_test("", "return NESTED") == "true");
        CHECK(lua.run_for_test("", "return RESTORED") == "false");
        CHECK(lua.run_for_test("", "return SLIDER_VALUE") == "42");
        CHECK(lua.run_for_test("", "return SLIDER_CHANGED") == "false");
        CHECK(lua.run_for_test("", "return SLIDER_ID and RECT_DEFAULT and SLIDER_DISABLED") == "true");
        CHECK(lua.run_for_test("", "return CONTROL[1]==TOTAL[1] and CONTROL[2]==TOTAL[2] and CONTROL[4]==TOTAL[4]") == "true");
        CHECK(lua.run_for_test("", "return CONTROL[3]>CONTROL[1] and CONTROL[3]<TOTAL[3]") == "true");
        const auto number = [&](const char* key) {
            lua_getglobal(lua.state(), key);
            REQUIRE(lua_isnumber(lua.state(), -1));
            const double value = lua_tonumber(lua.state(), -1);
            lua_pop(lua.state(), 1);
            return value;
        };
        CHECK(number("ALPHA_ORIGINAL") == doctest::Approx(ImGui::GetStyle().Alpha));
        CHECK(number("ALPHA_PUSHED") == doctest::Approx(0.375));
        CHECK(number("ALPHA_DISABLED") == doctest::Approx(0.375 * ImGui::GetStyle().DisabledAlpha));
        CHECK(number("ALPHA_REENABLED") == doctest::Approx(0.375));
        CHECK(number("ALPHA_RESTORED") == doctest::Approx(number("ALPHA_ORIGINAL")));
        CHECK(lua.load_status().error_count == 0);
    }
    ImGui::DestroyContext(context);
}

TEST_CASE("Lua UI: the key names, keyboard capture, typing and DrawText's size") {
    hailstone::test::ScratchDir scratch("lua_ui_api_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "keys.lua") << "ok = ui.Key.A ~= nil and ui.Key.Z ~= nil and ui.Key['0'] ~= nil and ui.Key['9'] ~= nil\n"
                                       "  and ui.Key.UpArrow ~= nil and ui.Key.Space ~= nil and ui.Key.Escape ~= nil\n"
                                       "ui.window('Keys', function()\n"
                                       "  ui.Begin('Keys')\n"
                                       "  ui.CaptureKeyboard()\n"
                                       "  typing = ui.GetIO().typing\n"
                                       "  local x, y = ui.GetCursorScreenPos()\n"
                                       "  ui.DrawText(x, y, 1, 1, 1, 1, 'big', 30)\n"
                                       "  ui.DrawText(x, y, 1, 1, 1, 1, 'plain')\n"
                                       "  ui.End()\n"
                                       "end)\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        for (int i = 0; i < 2; ++i) {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        }
        lua_getglobal(L, "ok");
        CHECK(lua_toboolean(L, -1));
        lua_getglobal(L, "typing");
        CHECK(lua_isboolean(L, -1));
        lua_pop(L, 2);
        // CaptureKeyboard asked for the next frame: the keyboard is the UI's.
        ImGui::NewFrame();
        CHECK(ImGui::GetIO().WantCaptureKeyboard);
        ImGui::Render();
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// ui.FindWindowRect: a window's place and size by name, before its Begin (lib/snap.lua snaps a dialog's drag from
// it); nothing for a window never drawn, and a "###" name finds the window whatever its title.
TEST_CASE("Lua UI: FindWindowRect finds a drawn window by name") {
    hailstone::test::ScratchDir scratch("lua_ui_api_rect_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "rect.lua") << "ui.window('Rect', function()\n"
                                       "  before = { ui.FindWindowRect('###probe') }\n"
                                       "  missing = ui.FindWindowRect('never drawn') == nil\n"
                                       "  ui.SetNextWindowPos(40, 50, ui.Cond.Always)\n"
                                       "  ui.SetNextWindowSize(120, 90, ui.Cond.Always)\n"
                                       "  ui.Begin('Title ' .. ui.GetFrameCount() .. '###probe')\n"
                                       "  ui.End()\n"
                                       "end)\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        for (int i = 0; i < 2; ++i) {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        }
        lua_getglobal(L, "missing");
        CHECK(lua_toboolean(L, -1));
        lua_getglobal(L, "before");
        REQUIRE(lua_istable(L, -1));
        CHECK(lua_rawlen(L, -1) == 4);
        const double want[] = {40, 50, 120, 90};
        for (int i = 0; i < 4; ++i) {
            lua_rawgeti(L, -1, i + 1);
            CHECK(lua_tonumber(L, -1) == want[i]);
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// LuaUi::setting / set_setting: the client's own menus read and change the UI's layout settings (the hot bar's
// Lock layout), the same values ui.GetSetting / ui.SetSetting use.
TEST_CASE("Lua UI: the client reads and writes a layout setting") {
    hailstone::test::ScratchDir scratch("lua_ui_setting_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        CHECK(lua.setting("hud.lock.HotButtonWnd") == "");
        CHECK(lua.set_setting("hud.lock.HotButtonWnd", "1"));
        CHECK(lua.setting("hud.lock.HotButtonWnd") == "1");
        CHECK(lua.set_setting("hud.lock.HotButtonWnd", "0"));
        CHECK(lua.setting("hud.lock.HotButtonWnd") == "0");
    }
}

// ui.GetIO: one table a frame (the HUD asks for it from every window), with the frame's values; the next frame's is
// another, with that frame's.
TEST_CASE("Lua UI: GetIO gives one read-only table, its values the frame's") {
    hailstone::test::ScratchDir scratch("lua_ui_api_io_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "io.lua") << "frames = {}\n"
                                     "ui.window('IO', function()\n"
                                     "  local a, b = ui.GetIO(), ui.GetIO()\n"
                                     "  frames[#frames + 1] = { same = rawequal(a, b), ctrl = a.ctrl, width = b.width, t = a,\n"
                                     "    write = pcall(function() a.width = 1 end), wrote = a.width ~= 1 }\n"
                                     "end)\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    hailstone::ui::configure_keys(); // as the client: no macOS Ctrl/Cmd swap, so the Ctrl event below reads as Ctrl
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        for (int i = 0; i < 2; ++i) {
            ImGui::GetIO().DisplaySize = ImVec2(i == 0 ? 800.0f : 1024.0f, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, i == 1);
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        }
        REQUIRE(luaL_dostring(L, "return frames[1].same and frames[2].same, frames[1].ctrl, frames[2].ctrl, "
                                 "frames[1].width, frames[2].width, rawequal(frames[1].t, frames[2].t) and "
                                 "not frames[1].write and frames[1].wrote and getmetatable(frames[1].t) == false") ==
                LUA_OK);
        CHECK(lua_toboolean(L, -6));
        CHECK_FALSE(lua_toboolean(L, -5));
        CHECK(lua_toboolean(L, -4));
        CHECK(lua_tonumber(L, -3) == 800);
        CHECK(lua_tonumber(L, -2) == 1024);
        CHECK(lua_toboolean(L, -1));
        lua_settop(L, 0);
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// ui.PushStyleColors / ui.PushStyleVars: many pushes in one call from flat lists (lib/style.lua's look for every
// window), skipping an index out of range and returning how many were pushed; the values are the ones in force after.
namespace {
ImVec4 g_text{};
float g_rounding = -1, g_pad_x = -1, g_pad_y = -1;
int probe_style(lua_State*) {
    g_text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    g_rounding = ImGui::GetStyle().WindowRounding;
    g_pad_x = ImGui::GetStyle().WindowPadding.x;
    g_pad_y = ImGui::GetStyle().WindowPadding.y;
    return 0;
}
} // namespace

TEST_CASE("Lua UI: PushStyleColors and PushStyleVars push a list in one call") {
    hailstone::test::ScratchDir scratch("lua_ui_api_style_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "style.lua")
        << "ui.window('Style', function()\n"
           "  nc = ui.PushStyleColors({ ui.Col.Text, 0.25, 0.5, 0.75, 1, 9999, 1, 1, 1, 1, ui.Col.Button, 0, 1, 0, 1 })\n"
           "  nv = ui.PushStyleVars({ ui.StyleVar.WindowRounding, 3, false, -1, 0, 0, ui.StyleVar.WindowPadding, 4, 6 })\n"
           "  probe()\n"
           "  ui.PopStyleVar(nv)\n"
           "  ui.PopStyleColor(nc)\n"
           "end)\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        lua_register(L, "probe", &probe_style);
        ImGui::GetIO().DisplaySize = ImVec2(800, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        const float rounding = ImGui::GetStyle().WindowRounding;
        ImGui::NewFrame();
        lua.frame();
        ImGui::Render();
        lua_getglobal(L, "nc");
        CHECK(lua_tointeger(L, -1) == 2);
        lua_getglobal(L, "nv");
        CHECK(lua_tointeger(L, -1) == 2);
        lua_pop(L, 2);
        CHECK(g_text.x == doctest::Approx(0.25f));
        CHECK(g_text.y == doctest::Approx(0.5f));
        CHECK(g_text.z == doctest::Approx(0.75f));
        CHECK(g_rounding == doctest::Approx(3.0f));
        CHECK(g_pad_x == doctest::Approx(4.0f));
        CHECK(g_pad_y == doctest::Approx(6.0f));
        CHECK(ImGui::GetStyle().WindowRounding == rounding); // popped again
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// The modern pack's shared pieces (ui/modern/lib) over a stand-in hs table: W.List keeps a copy of a sorted or
// filtered list and sorts it again when a row changes (in view or not), W.ButtonRow leaves the caller's opts alone,
// and the testing hooks read their settings once.
TEST_CASE("Modern UI libs: W.List's sorted copy, ButtonRow's opts, the test hooks read once") {
    hailstone::test::ScratchDir scratch("lua_ui_modern_libs_test");
    const fs::path dir = scratch.path() / "pack";
    const fs::path lib = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern" / "lib";
    REQUIRE(fs::exists(lib / "widgets.lua"));
    fs::create_directories(dir);
    fs::copy(lib, dir / "lib", fs::copy_options::recursive);
    std::ofstream(scratch.path() / "ui.txt") << "test.open.Later=1\ntest.open.LaterToo=1\n";
    std::ofstream(dir / "libs.lua") << R"lua(
LIST = {}
for i = 1, 200 do LIST[i] = { tostring(1000 - i) } end -- the list's order is descending
reads, listCalls, shown, opened = 0, 0, {}, nil
hs = {
  ListInfo = function() return #LIST, 1, -1, { "Value" }, { 0 } end,
  ListRow = function(_, _, r)
    reads = reads + 1
    local t = LIST[r + 1]
    if not t then return nil end
    return { t[1] }, 1, 1, 1
  end,
  WindowList = function() listCalls = listCalls + 1; return {} end,
  ChildShown = function() return true end,
  ChildState = function() return true, nil end,
  WindowText = function() return "" end,
  ClickChild = function() end,
  ListClick = function() end,
  InGame = function() return true end,
  ShowWindow = function(w) shown[#shown + 1] = w end,
  HoldingButton = function() return false end,
}
TEST_UI_API_ATTACH(hs)
local asked = {}
local getSetting = ui.GetSetting
ui.GetSetting = function(k, d) asked[k] = (asked[k] or 0) + 1; return getSetting(k, d) end
function askedFor(k) return asked[k] or 0 end

local W = dofile("lib/widgets.lua")
local P = dofile("lib/progress.lua")
local M = dofile("lib/markets.lua")
FILTER, FILTER_KEY = nil, nil
local held = { tooltip = "kept" }
local ROW = { { "B1", "One", held } }
frames = {}
ui.window("Libs", function()
  ui.SetNextWindowPos(0, 0, ui.Cond.Always)
  ui.SetNextWindowSize(500, 560, ui.Cond.Always)
  ui.Begin("Libs")
  local f = { drawn = {} }
  reads = 0
  W.List("Wnd", "L", 0, 300, { sortable = true, filter = FILTER, filterKey = FILTER_KEY,
    onRow = function(_, texts) f.drawn[#f.drawn + 1] = texts[1]; return true end })
  f.reads = reads
  W.ButtonRow("Wnd", ROW, 40)
  local _, y0, _, y1 = ui.GetItemRect()
  f.tall = y1 - y0
  W.ButtonRow("Wnd", ROW)
  _, y0, _, y1 = ui.GetItemRect()
  f.plain = y1 - y0
  f.heldUntouched = held.width == nil and held.height == nil and held.tooltip == "kept"
  P.TestOpen("Never", function() error("not set: never runs") end)
  P.TestOpen("Later", function(a, b) opened = a .. b end, "x", "y")
  P.TestOpen("LaterToo")
  M.TestOpen("NeverM")
  f.forced = M.Forced("NeverD")
  P.Rows("Wnd", "P", 0, 100, { rows = { { "r1" }, { "r2" } } })
  ui.End()
  frames[#frames + 1] = f
end)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        REQUIRE(hailstone::test::register_mock_ui_api(L) == LUA_OK);
        auto frames = [&](int n, float dt = 1.0f / 60.0f) {
            for (int i = 0; i < n; ++i) {
                ImGui::GetIO().DisplaySize = ImVec2(800, 600);
                ImGui::GetIO().DeltaTime = dt;
                ImGui::NewFrame();
                lua.frame();
                ImGui::Render();
            }
        };
        // Evaluates `expr` (Lua) on the last frame drawn.
        auto last = [&](const char* expr) {
            std::string code = std::string("local f = frames[#frames]; return ") + expr;
            REQUIRE(luaL_dostring(L, code.c_str()) == LUA_OK);
            std::string out = lua_isstring(L, -1) ? lua_tostring(L, -1) : (lua_toboolean(L, -1) ? "true" : "false");
            lua_settop(L, 0);
            return out;
        };
        frames(3);
        REQUIRE_FALSE(ImGui::FindWindowByName("UI errors"));
        // sorted ascending on the first column (a sortable table's first sort): numbers as numbers
        CHECK(last("f.drawn[1] .. ',' .. f.drawn[2]") == "800,801");
        // a long list is not read whole each frame: the rows in view and a few more
        CHECK(std::stoi(last("f.reads")) < 60);
        // a row out of view changes (the count, the first and the last row as they were): sorted in within frames
        REQUIRE(luaL_dostring(L, "LIST[100] = { '5' }") == LUA_OK);
        frames(40);
        CHECK(last("f.drawn[1] .. ',' .. f.drawn[2]") == "5,800");
        // numbers as numbers, words without case
        REQUIRE(luaL_dostring(L, "LIST = { { 'b' }, { '10' }, { 'A' }, { '9' } }") == LUA_OK);
        frames(2);
        CHECK(last("table.concat(f.drawn, ',')") == "9,10,A,b");
        // numbers before texts, in one order: '10' < '5x' < '9' as texts and 9 < 10 as numbers made a cycle, which
        // table.sort may refuse ("invalid order function for sorting")
        REQUIRE(luaL_dostring(L, "LIST = {}; for i = 1, 60 do LIST[i] = { i % 3 == 0 and (i .. 'x') or tostring(100 - i) } end")
                == LUA_OK);
        frames(2);
        REQUIRE_FALSE(ImGui::FindWindowByName("UI errors"));
        CHECK(last("f.drawn[1] .. ',' .. f.drawn[2]") == "41,42");
        REQUIRE(luaL_dostring(L, "LIST = { { '10' }, { '5x' }, { '9' } }") == LUA_OK);
        frames(2);
        CHECK(last("table.concat(f.drawn, ',')") == "9,10,5x");
        REQUIRE(luaL_dostring(L, "LIST = { { 'b' }, { '10' }, { 'A' }, { '9' } }") == LUA_OK);
        frames(2);
        // the filter, kept by its key
        REQUIRE(luaL_dostring(L, "FILTER = function(_, t) return t[1]:find('1') ~= nil or t[1] == 'b' end; "
                                 "FILTER_KEY = '1'") == LUA_OK);
        frames(1);
        CHECK(last("table.concat(f.drawn, ',')") == "10,b");
        // a row changes under a kept filter: filtered and sorted again
        REQUIRE(luaL_dostring(L, "LIST[1] = { '1' }") == LUA_OK);
        frames(1);
        CHECK(last("table.concat(f.drawn, ',')") == "1,10");
        REQUIRE(luaL_dostring(L, "FILTER, FILTER_KEY = nil, nil") == LUA_OK);
        frames(1);
        CHECK(last("table.concat(f.drawn, ',')") == "1,9,10,A");

        // ButtonRow: the row's height, and the caller's opts unchanged so a later row without one is the default
        CHECK(last("f.heldUntouched") == "true");
        CHECK(std::stof(last("f.tall")) == doctest::Approx(40));
        CHECK(std::stof(last("f.plain")) < 30);

        // the test hooks: a missing setting is asked for once; a set one runs once, with its arguments
        CHECK(last("askedFor('test.open.Never')") == "1");
        CHECK(last("askedFor('test.open.NeverM')") == "1");
        CHECK(last("askedFor('test.draw.NeverD')") == "1");
        CHECK(last("askedFor('test.select.Wnd.P')") == "1");
        CHECK(last("f.forced == nil") == "true");
        CHECK(last("listCalls") == "0"); // P.Rows used the rows it was given
        CHECK(last("opened == nil and #shown == 0") == "true"); // 6 s after zoning in
        frames(3, 3.0f);
        CHECK(last("opened") == "xy");
        CHECK(last("table.concat(shown, ',')") == "LaterToo");
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// lib/progress.lua's P.Rows keeps a copy of the list (no hs.WindowList, a few rows read a frame) and sorts it again
// only when the rows shown change; lib/find.lua shares rows within a frame (map and minimap) while checking live
// eligibility again in each new frame.
TEST_CASE("Modern UI libs: P.Rows' kept copy and sort, Find's frame-shared rows") {
    hailstone::test::ScratchDir scratch("lua_ui_modern_rows_test");
    const fs::path dir = scratch.path() / "pack";
    const fs::path lib = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern" / "lib";
    REQUIRE(fs::exists(lib / "progress.lua"));
    fs::create_directories(dir);
    fs::copy(lib, dir / "lib", fs::copy_options::recursive);
    std::ofstream(dir / "rows.lua") << R"lua(
LIST = {}
for i = 1, 200 do LIST[i] = { tostring(1000 - i) } end -- the list's order is descending
reads, listCalls, sortCalls, finds = 0, 0, 0, 0
SEARCH = ""
hs = {
  ListInfo = function() return #LIST, 1, -1, { "Value" }, { 0 } end,
  ListRow = function(_, _, r)
    reads = reads + 1
    local t = LIST[r + 1]
    if not t then return nil end
    return { t[1] }, 1, 1, 1
  end,
  WindowList = function() listCalls = listCalls + 1; return {} end,
  ListClick = function() end,
  InGame = function() return true end,
  ZoneName = function() return "Zone", "zone" end,
  FindCategories = function() return { { "merchant", "Merchant" } } end,
  FindNpcs = function(search)
    finds = finds + 1
    return { { id = 1, name = "A" .. search, category = "merchant" }, { id = 2, name = "B", category = "banker" } }
  end,
}
TEST_UI_API_ATTACH(hs)
local P = dofile("lib/progress.lua")
local FL1, FL2 = dofile("lib/find.lua"), dofile("lib/find.lua")
local function byValue(a, b)
  sortCalls = sortCalls + 1
  local x, y = tonumber(a.texts[1]) or 0, tonumber(b.texts[1]) or 0
  if x ~= y then return x < y end
  return a.row < b.row
end
FILTER, FILTER_KEY = nil, nil
frames = {}
ui.window("Rows", function()
  ui.SetNextWindowPos(0, 0, ui.Cond.Always)
  ui.SetNextWindowSize(400, 300, ui.Cond.Always)
  ui.Begin("Rows")
  local f = { drawn = {} }
  reads, sortCalls = 0, 0
  local total, _, shown = P.Rows("Wnd", "L", 0, 200, { rowH = 20, sort = byValue, filter = FILTER,
    filterKey = FILTER_KEY, draw = function(_, texts) f.drawn[#f.drawn + 1] = texts[1] end })
  f.total, f.shown = total, shown
  f.reads, f.sorts = reads, sortCalls
  FIND_STATE.search = SEARCH
  local before = finds
  local a, b = FL1.rows(), FL2.rows()
  f.finds = finds - before
  f.found, f.same = #a .. ":" .. (a[1] and a[1].name or ""), rawequal(a, b)
  ui.End()
  frames[#frames + 1] = f
end)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        REQUIRE(hailstone::test::register_mock_ui_api(L) == LUA_OK);
        auto frames = [&](int n, float dt = 1.0f / 60.0f) {
            for (int i = 0; i < n; ++i) {
                ImGui::GetIO().DisplaySize = ImVec2(800, 600);
                ImGui::GetIO().DeltaTime = dt;
                ImGui::NewFrame();
                lua.frame();
                ImGui::Render();
            }
        };
        auto last = [&](const char* expr) {
            std::string code = std::string("local f = frames[#frames]; return ") + expr;
            REQUIRE(luaL_dostring(L, code.c_str()) == LUA_OK);
            std::string out = lua_isstring(L, -1) ? lua_tostring(L, -1) : (lua_toboolean(L, -1) ? "true" : "false");
            lua_settop(L, 0);
            return out;
        };
        auto run = [&](const char* code) { REQUIRE(luaL_dostring(L, code) == LUA_OK); };
        frames(3);
        REQUIRE_FALSE(ImGui::FindWindowByName("UI errors"));
        CHECK(last("f.drawn[1] .. ',' .. f.drawn[2]") == "800,801");
        CHECK(last("f.total .. ',' .. f.shown") == "200,200");
        CHECK(last("listCalls") == "0");
        // not read whole each frame, and not sorted again while nothing changed
        CHECK(std::stoi(last("f.reads")) < 60);
        CHECK(last("f.sorts") == "0");
        // a row out of view changes: sorted in within frames
        run("LIST[100] = { '5' }");
        frames(40);
        CHECK(last("f.drawn[1] .. ',' .. f.drawn[2]") == "5,800");
        CHECK(last("f.sorts") == "0");
        // a row in view changes: drawn as the list has it at once, and sorted in
        run("LIST[199] = { '1' }"); // (801)
        frames(2);
        CHECK(last("f.drawn[1] .. ',' .. f.drawn[2]") == "1,5");
        // the filter: run over the copy each frame without a key (sorting again only when its rows change), kept
        // with one
        run("FILTER = function(_, t) return t[1]:find('9') ~= nil end");
        frames(2);
        CHECK(last("f.drawn[1] .. ',' .. f.drawn[2]") == "809,819");
        CHECK(last("f.sorts") == "0");
        run("FILTER_KEY = '9'");
        frames(1);
        CHECK(last("f.drawn[1]") == "809");
        run("FILTER, FILTER_KEY = nil, nil");
        frames(1);
        CHECK(last("f.drawn[1] .. ',' .. f.total .. ',' .. f.shown") == "1,200,200");
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));

        // Find: one read for both copies, again when the search changes or the moment passes (0.2 s, as before map
        // policies; acting on a row checks its token then).
        run("SEARCH = 'x'");
        frames(1);
        CHECK(last("f.finds") == "1");
        CHECK(last("f.found") == "2:Ax");
        CHECK(last("f.same") == "true");
        run("finds = 0");
        frames(6); // (0.1 s)
        CHECK(last("finds") == "0");
        CHECK(last("f.same") == "true");
        frames(1, 0.25f);
        CHECK(last("f.finds") == "1");
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// The STML libraries' kept layouts (ui/modern/lib/stml.lua, ui/gamepad/lib/stml.lua): a line's wrap is worked out
// once and kept until the width or the font change, and is the one the per-frame wrap gave; the chat output's lines
// drawn from their summed heights land where the line-by-line walk put them; the gamepad's block cache keeps the
// last few texts.
TEST_CASE("Lua UI: STML wraps are kept until the width changes, and match the per-frame wrap") {
    hailstone::test::ScratchDir scratch("lua_ui_api_stml_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir / "lib");
    const fs::path ui_src = fs::path(__FILE__).parent_path().parent_path() / "ui";
    if (!fs::exists(ui_src / "modern" / "lib" / "stml.lua")) return; // an installed test binary run away from the sources
    fs::copy_file(ui_src / "modern" / "lib" / "stml.lua", dir / "lib" / "stml.lua");
    fs::copy_file(ui_src / "modern" / "lib" / "textselect.lua", dir / "lib" / "textselect.lua");
    fs::copy_file(ui_src / "gamepad" / "lib" / "stml.lua", dir / "lib" / "gpstml.lua");
    // (both libraries' theme, the colours they read)
    std::ofstream(dir / "lib" / "theme.lua")
        << "local c = { link = { 0, 0, 1, 1 }, text = { 1, 1, 1, 1 } }\n"
           "return { color = c, C = c, text = function(x, y, t) return ui.CalcTextSize(t) end }\n";
    std::ofstream(dir / "wrap.lua") << R"lua(
local stml = dofile("lib/stml.lua")
local G = dofile("lib/gpstml.lua")
out = {}
local frame = 0
-- the wrap as stml.DrawLine worked it out every frame before it was kept: each piece and whether it starts a row
local function reference(segments, width)
  local r, x, first = {}, 0, true
  for _, seg in ipairs(segments) do
    for word, space in seg[1]:gmatch("([^ ]*)( *)") do
      if word ~= "" or space ~= "" then
        local piece = word .. space
        local w = ui.CalcTextSize(piece)
        local row = false
        if not first then
          if x + ui.CalcTextSize(word) > width and x > 0 then x = 0; row = true end
        end
        r[#r + 1] = piece .. (row and "|row" or "")
        x = x + w
        first = false
      end
    end
  end
  return table.concat(r, ",")
end
local function kept(segments)
  local r, p = {}, segments.wrap
  for i = 1, #p, 6 do r[#r + 1] = p[i] .. (p[i + 3] and "|row" or "") end
  return table.concat(r, ",")
end
local line = stml.Parse('<c "#ff0000">one two three  four five six seven eight nine ten</c> <a WndNotify="27,x">a link name</a> end')
local view = { }
refHeights = {}
ui.window("Wrap", function()
  frame = frame + 1
  ui.SetNextWindowSize(700, 400, ui.Cond.Always)
  ui.Begin("Wrap")
  if frame == 1 then
    local real, calls = ui.CalcTextSize, 0
    stml.DrawLine(line, 2000, nil, "k")
    local wide = line.wrap
    out.wideSame = kept(line) == reference(line, 2000)
    ui.CalcTextSize = function(...) calls = calls + 1; return real(...) end
    stml.DrawLine(line, 2000, nil, "k")
    ui.CalcTextSize = real
    out.keptCalls = calls
    out.keptTable = rawequal(line.wrap, wide)
    stml.DrawLine(line, 60, nil, "k")
    out.narrowSame = kept(line) == reference(line, 60)
    out.narrowRows = select(2, kept(line):gsub("|row", ""))
    stml.DrawLine(line, 61, nil) -- (no key: the font measured)
    out.measuredSame = kept(line) == reference(line, 61)
    -- gamepad: rows kept on the line until the width changes; blocks by text
    local seg = G.parse('<c "#ff0000">one two three four five</c> six seven')
    local a = G.rows(seg, 2000)
    out.gpSame = rawequal(G.rows(seg, 2000), a)
    local b = G.rows(seg, 40)
    out.gpNew = not rawequal(a, b) and #b > #a and #a == 1
    local first = G.blockRows("first<br>text", 300)
    out.blockSame = rawequal(G.blockRows("first<br>text", 300), first) and #first == 2
    out.blockWidth = not rawequal(G.blockRows("first<br>text", 299), first)
    local again = G.blockRows("first<br>text", 299)
    for i = 1, 8 do G.blockRows("other " .. i, 300) end
    out.blockEvicted = not rawequal(G.blockRows("first<br>text", 299), again)
    -- the chat output: 300 lines, some wrapping
    local raws = {}
    for i = 1, 300 do raws[i] = (i % 3 == 0) and ('<c "#00ff00">line ' .. i .. ' with words enough to wrap at this width, more and more words</c>') or ('<c "#ffffff">line ' .. i .. '</c> <a WndNotify="27,x">link</a>') end
    stml.UpdateLines(view, raws)
  end
  ui.BeginChild("output", 300, 200, false)
  if frame == 3 then ui.SetScrollY(1500) end
  stml.DrawLines(view, 380, 200, "k380")
  local _, y = ui.GetCursorPos()
  out["end" .. frame] = y
  out["fast" .. frame] = view.cumKey == "k380"
  ui.EndChild()
  -- the same lines drawn by the line-by-line walk DrawLines did before it kept their places
  ui.SameLine() -- (beside it: a child out of the window's sight draws nothing)
  ui.BeginChild("reference", 300, 200, false)
  if frame == 3 then ui.SetScrollY(1500) end
  local top = ui.GetScrollY()
  local x0, y = ui.GetCursorPos()
  local skipped = false
  for i, l in ipairs(view.lines) do
    local h = refHeights[i]
    if h and (y + h < top - 40 or y > top + 200 + 40) then
      y = y + h
      skipped = true
    else
      if skipped then ui.SetCursorPos(x0, y); skipped = false end
      stml.DrawLine(l, 380)
      local _, ny = ui.GetCursorPos()
      refHeights[i] = ny - y
      y = ny
    end
  end
  if skipped then ui.SetCursorPos(x0, y); ui.Dummy(0, 0) end
  out["ref" .. frame] = select(2, ui.GetCursorPos())
  ui.EndChild()
  if frame == 4 then -- (last: the rows drawn would push the outputs above out of the window's sight)
    -- a chat line holding new lines (a server's MOTD): each starts a row, an empty one is a blank row, one at the
    -- end adds nothing
    local motd = stml.Parse('<c "#808080">[06:53:16] </c>Be Free! \n\nDo Not Pay\n')
    stml.DrawLine(motd, 2000, nil, "k")
    out.motd = kept(motd)
    local gp = G.layout(G.parse('<c "#808080">[06:53:16] </c>Be Free! \n\nDo Not Pay\n'), 2000)
    local gr = {}
    for _, r in ipairs(gp) do
      local t = {}
      for _, p in ipairs(r) do t[#t + 1] = p[1] end
      gr[#gr + 1] = table.concat(t)
    end
    out.gpMotd = table.concat(gr, "|")
  end
  ui.End()
end)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        for (int i = 0; i < 4; ++i) {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        }
        auto get = [&](const char* key) {
            lua_getglobal(L, "out");
            lua_getfield(L, -1, key);
            std::string v = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
            lua_settop(L, 0);
            return v;
        };
        CHECK(get("wideSame") == "true");
        CHECK(get("keptCalls") == "0");
        CHECK(get("keptTable") == "true");
        CHECK(get("narrowSame") == "true");
        CHECK(get("narrowRows") != "0");
        CHECK(get("measuredSame") == "true");
        CHECK(get("motd") == "[06:53:16] ,Be ,Free! ,|row,Do |row,Not ,Pay");
        CHECK(get("gpMotd") == "[06:53:16] Be Free! ||Do Not Pay");
        CHECK(get("gpSame") == "true");
        CHECK(get("gpNew") == "true");
        CHECK(get("blockSame") == "true");
        CHECK(get("blockWidth") == "true");
        CHECK(get("blockEvicted") == "true");
        // frame 1 walks every line (no heights yet); then the summed heights, ending where the walk ends
        CHECK(get("fast1") == "false");
        CHECK(get("fast2") == "true");
        for (const char* f : {"1", "2", "3", "4"}) CHECK(get((std::string("end") + f).c_str()) == get((std::string("ref") + f).c_str()));
        CHECK(get("fast4") == "true");
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// The modern pack's STML links (ui/modern/lib/stml.lua): an href's target, also when its quote is never closed (the
// storyline pages some servers ship: <a href="file:///storyline/Epic/BardEpic.txt>Bard</a>), a WndNotify link's
// message and data, an <a> with neither only coloured, and a "<" that opens no tag kept as text. Also the <c> colours
// stml.Lines carries from line to line, no more than 32 of them.
TEST_CASE("Lua UI: STML href links, an unclosed quote ending at the tag's end") {
    hailstone::test::ScratchDir scratch("lua_ui_api_stml_href_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir / "lib");
    const fs::path ui_src = fs::path(__FILE__).parent_path().parent_path() / "ui";
    if (!fs::exists(ui_src / "modern" / "lib" / "stml.lua")) return; // an installed test binary run away from the sources
    fs::copy_file(ui_src / "modern" / "lib" / "stml.lua", dir / "lib" / "stml.lua");
    fs::copy_file(ui_src / "modern" / "lib" / "textselect.lua", dir / "lib" / "textselect.lua");
    std::ofstream(dir / "lib" / "theme.lua")
        << "local c = { link = { 0, 0, 1, 1 }, text = { 1, 1, 1, 1 } }\n"
           "return { color = c, C = c, text = function(x, y, t) return ui.CalcTextSize(t) end }\n";
    std::ofstream(dir / "links.lua") << R"lua(
local stml = dofile("lib/stml.lua")
local function link(line)
  for _, seg in ipairs(stml.Parse(line)) do
    if seg[3] then return type(seg[3]) == "table" and (seg[1] .. "=" .. seg[3][1] .. "," .. seg[3][2]) or (seg[1] .. "=plain") end
  end
  return "none"
end
out = {
  closed = link('<a href="file:///storyline/storyEpicsGuide.txt">Epics</a>'),
  unclosed = link('&nbsp;<a href="file:///storyline/Epic/BardEpic.txt>Bard</a><br>'),
  notify = link('<a WndNotify="27,x">item</a>'),
  plain = link('<a name="top">here</a>'),
}
local function plain(line)
  local t = {}
  for _, seg in ipairs(stml.Parse(line)) do t[#t + 1] = seg[1] end
  return table.concat(t)
end
out.lt = plain("a < b") .. "|" .. plain('<c "#ff0000">x &amp; <3') .. "|" .. plain("one <b>two</b> three")
-- a colour open at a line's end carries on; past 32 unclosed tags only the innermost 32 carry
local W = { 1, 1, 1, 1 }
local function hex(c) return string.format("%02x%02x%02x", math.floor(c[1] * 255 + 0.5), math.floor(c[2] * 255 + 0.5), math.floor(c[3] * 255 + 0.5)) end
local lines = stml.Lines('<c "#ff0000">a\nb</c>c', W)
local deep = {}
for i = 1, 40 do deep[i] = string.format('<c "#%02x0000">', i) end
local capped = stml.Lines(table.concat(deep) .. "x\ny" .. ("</c>"):rep(32) .. "z", W)
out.carry = hex(lines[2][1][2]) .. "," .. tostring(lines[2][2][2] == W) .. "|" .. hex(capped[2][1][2]) .. "," ..
  tostring(capped[2][#capped[2]][2] == W)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        ImGui::GetIO().DisplaySize = ImVec2(800, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        lua.frame(); // (the pack's scripts run)
        ImGui::Render();
        auto get = [&](const char* key) {
            lua_getglobal(L, "out");
            if (!lua_istable(L, -1)) {
                lua_settop(L, 0);
                return std::string("no out");
            }
            lua_getfield(L, -1, key);
            std::string v = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
            lua_settop(L, 0);
            return v;
        };
        CHECK(get("closed") == "Epics=-1,file:///storyline/storyEpicsGuide.txt");
        CHECK(get("unclosed") == "Bard=-1,file:///storyline/Epic/BardEpic.txt");
        CHECK(get("notify") == "item=27,x");
        CHECK(get("plain") == "here=plain");
        CHECK(get("lt") == "a < b|x & <3|one two three"); // a "<" that opens no tag is text
        CHECK(get("carry") == "ff0000,true|280000,true");
    }
    ImGui::DestroyContext(ctx);
}

// The chat output's text selection (ui/modern/lib/textselect.lua, drawn by ui/modern/lib/stml.lua): words, the
// character under the mouse never splitting a UTF-8 sequence, multiple clicks, the selected text across lines and
// past lines that have gone, and, drawn, the place under the mouse on a wrapped line's rows.
TEST_CASE("Lua UI: chat text selection finds the place under the mouse and the selected text") {
    hailstone::test::ScratchDir scratch("lua_ui_api_textselect_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir / "lib");
    const fs::path ui_src = fs::path(__FILE__).parent_path().parent_path() / "ui";
    if (!fs::exists(ui_src / "modern" / "lib" / "textselect.lua")) return; // an installed test binary run away from the sources
    fs::copy_file(ui_src / "modern" / "lib" / "stml.lua", dir / "lib" / "stml.lua");
    fs::copy_file(ui_src / "modern" / "lib" / "textselect.lua", dir / "lib" / "textselect.lua");
    std::ofstream(dir / "lib" / "theme.lua")
        << "local c = { link = { 0, 0, 1, 1 }, text = { 1, 1, 1, 1 } }\n"
           "return { color = c, C = c, text = function(x, y, t) return ui.CalcTextSize(t) end }\n";
    std::ofstream(dir / "select.lua") << R"lua(
local stml = dofile("lib/stml.lua")
local ts = dofile("lib/textselect.lua")
local function pair(a, b) return tostring(a) .. "," .. tostring(b) end
out = {}
out.word = pair(ts.Word("tells you, hello there", 12))
out.wordSpace = pair(ts.Word("a  b", 2))
out.wordEnds = pair(ts.Word("edge", 0)) .. "|" .. pair(ts.Word("edge", 99))
local m = function(t) return #t * 10 end
out.charAt = ts.CharAt("abcd", 14, m) .. "," .. ts.CharAt("abcd", 16, m) .. "," .. ts.CharAt("ab", 100, m) .. "," .. ts.CharAt("ab", -1, m)
out.utf8 = ts.CharAt("a\195\169 b", 25, m) .. "," .. ts.CharAt("a\195\169 b", 14, m)
local st = {}
local clicks = {}
for _, c in ipairs({ { 0, 0 }, { 0.1, 1 }, { 0.2, 1 }, { 0.3, 1 }, { 1.0, 1 }, { 1.1, 50 } }) do
  clicks[#clicks + 1] = ts.Clicks(st, c[1], c[2], 0, 0.35, 4)
end
out.clicks = table.concat(clicks)
local view = {}
stml.UpdateLines(view, { "first line", "second", "third one" })
stml.AppendLines(view, { "fourth" }, 3)
out.dropped = view.dropped
local s, e = ts.Range({ anchor = { line = 4, off = 3 }, caret = { line = 2, off = 2 } })
out.extract = ts.Extract(view, s, e)
out.extractGone = ts.Extract(view, { line = 1, off = 3 }, { line = 2, off = 3 })
out.empty = tostring(ts.Range({ anchor = { line = 2, off = 1 }, caret = { line = 2, off = 1 } }))
out.span = pair(ts.Span(s, e, 3, 6, 3)) .. "|" .. pair(ts.Span(s, e, 4, 0, 6)) .. "|" .. pair(ts.Span(s, e, 2, 0, 2)) .. "|" .. tostring(ts.Span(s, e, 5, 0, 3))
stml.UpdateLines(view, { "again" })
out.renumbered = view.dropped
out.markup = ts.LineText(stml.Parse('<c "#808080">[12:00] </c>Bob tells you, &lt;3 <a WndNotify="27,x">Sword</a>'))

-- drawn: a wrapped first line and a second, the mouse put at known places frame by frame
local dview = {}
local first = "alpha beta gamma delta epsilon zeta eta theta"
stml.UpdateLines(dview, { first, "second line" })
local sel = { rows = 0, color = { 1, 1, 1, 0.3 }, anchor = { line = 1, off = 2 }, caret = { line = 2, off = 3 } }
local frame = 0
ui.window("Select", function()
  frame = frame + 1
  ui.SetNextWindowSize(700, 400, ui.Cond.Always)
  ui.Begin("Select")
  if frame == 1 then
    -- a line holding new lines: each piece's offset in the line's text, and which rows the text's new lines start
    local motd = stml.Parse("Be Free! \n\nDo Not")
    stml.DrawLine(motd, 2000, nil, "k", { active = false })
    local offs = {}
    for i = 1, #motd.wrap, 6 do offs[#offs + 1] = motd.wrap[i + 4] .. tostring(motd.wrap[i + 3]) end
    out.motdOffsets = table.concat(offs, " ")
  end
  ui.BeginChild("output", 300, 200, false)
  local x0, y0 = ui.GetCursorScreenPos()
  local alphaW = ui.CalcTextSize("alpha ")
  local _, rowH = ui.CalcTextSize("Ag")
  local points = {
    { x0 + alphaW + 1, y0 + 1 },      -- just inside "beta"
    { x0 + 1, y0 + rowH * 1.5 + 2 },  -- the start of the second row (whatever the rows' spacing)
    { x0 + 1000, y0 + 1 },            -- right of the first row's end
    { x0 + 5, y0 - 50 },              -- above every line
    { x0 + 1, y0 + 150 },             -- below the last line
  }
  local p = points[frame] or points[1]
  sel.mx, sel.my = p[1], p[2]
  sel.s, sel.e = ts.Range(sel)
  sel.active, sel.lineH = true, rowH
  sel.rows, sel.candRow, sel.hitLine, sel.firstLine, sel.overLink = 0, nil, nil, nil, false
  stml.DrawLines(dview, 120, 200, "k120", nil, sel)
  out["line" .. frame], out["off" .. frame], out["first" .. frame] = sel.hitLine, sel.hitOff, sel.firstLine
  if frame == 2 then out.rowStart = sel.hitOff and sel.hitOff > 0 and first:sub(sel.hitOff, sel.hitOff) == " " end
  out.wrappedSingle = dview.lines[2].wrap ~= nil -- (with a selection a one-coloured line is drawn by its wrap)
  ui.EndChild()
  ui.End()
end)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        for (int i = 0; i < 5; ++i) {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        }
        auto get = [&](const char* key) {
            lua_getglobal(L, "out");
            if (!lua_istable(L, -1)) {
                lua_settop(L, 0);
                return std::string("no out");
            }
            lua_getfield(L, -1, key);
            std::string v = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
            lua_settop(L, 0);
            return v;
        };
        CHECK(get("word") == "11,16");
        CHECK(get("wordSpace") == "2,2");
        CHECK(get("wordEnds") == "0,4|0,4");
        CHECK(get("charAt") == "1,2,2,0");
        CHECK(get("utf8") == "3,1"); // (never 2: the middle of the two-byte character)
        CHECK(get("clicks") == "123111");
        CHECK(get("dropped") == "1");
        CHECK(get("extract") == "cond\nthird one\nfou");
        CHECK(get("extractGone") == "sec");
        CHECK(get("empty") == "nil");
        CHECK(get("span") == "0,3|0,3|nil,nil|nil");
        CHECK(get("renumbered") == "0");
        CHECK(get("markup") == "[12:00] Bob tells you, <3 Sword");
        CHECK(get("motdOffsets") == "0false 3false 10hard 11hard 14false");
        // drawn
        CHECK(get("line1") == "1");
        CHECK(get("off1") == "6");
        CHECK(get("line2") == "1");
        CHECK(get("rowStart") == "true");
        CHECK(get("line3") == "1");
        CHECK(get("off3") == get("off2")); // the first row's end is where the second row starts
        CHECK(get("line4") == "nil");
        CHECK(get("first4") == "1");
        CHECK(get("line5") == "2");
        CHECK(get("wrappedSingle") == "true");
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// A word wider than the row (ui/modern/lib/stml.lua's wrap): broken where the row ends, never inside a UTF-8
// sequence, each piece on a row of its own, its offsets still the line's bytes so the place under the mouse is right.
TEST_CASE("Lua UI: STML breaks a word wider than the row at a character's start") {
    hailstone::test::ScratchDir scratch("lua_ui_api_stml_longword_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir / "lib");
    const fs::path ui_src = fs::path(__FILE__).parent_path().parent_path() / "ui";
    if (!fs::exists(ui_src / "modern" / "lib" / "stml.lua")) return; // an installed test binary run away from the sources
    fs::copy_file(ui_src / "modern" / "lib" / "stml.lua", dir / "lib" / "stml.lua");
    fs::copy_file(ui_src / "modern" / "lib" / "textselect.lua", dir / "lib" / "textselect.lua");
    std::ofstream(dir / "lib" / "theme.lua")
        << "local c = { link = { 0, 0, 1, 1 }, text = { 1, 1, 1, 1 } }\n"
           "return { color = c, C = c, text = function(x, y, t) return ui.CalcTextSize(t) end }\n";
    std::ofstream(dir / "longword.lua") << R"lua(
local stml = dofile("lib/stml.lua")
local ts = dofile("lib/textselect.lua")
out = {}
-- the pieces cover the line's text in order, each within the width (but a lone character), none starting inside a
-- UTF-8 sequence; returns "ok" and the number of rows, or what is wrong
local function check(segments, width)
  local p, text, at, rows = segments.wrap, ts.LineText(segments), 0, 1
  for i = 1, #p, 6 do
    if p[i + 4] ~= at then return "offset " .. tostring(p[i + 4]) .. " at " .. at end
    local b = p[i]:byte(1)
    if b and b >= 0x80 and b < 0xC0 then return "split at " .. at end
    local word = p[i]:match("^(.-) *$")
    if ui.CalcTextSize(word) > width and #word > 1 and not word:find("^[\192-\255][\128-\191]*$") then
      return "too wide at " .. at
    end
    if i > 1 and p[i + 3] then rows = rows + 1 end
    at = at + #p[i]
  end
  if at ~= #text then return "covers " .. at .. " of " .. #text end
  return "ok", rows
end
local frame = 0
ui.window("Long", function()
  frame = frame + 1
  ui.SetNextWindowSize(700, 400, ui.Cond.Always)
  ui.Begin("Long")
  local long = ("abcdefghij"):rep(12)
  if frame == 1 then
    -- one colour (drawn by its wrap, as the chat output draws it), then several colours and a UTF-8 run
    local plain = stml.Parse("x " .. long .. " end")
    stml.DrawLine(plain, 100, nil, "k", { active = false })
    local r, rows = check(plain, 100)
    out.plain, out.plainRows = r, rows
    out.plainFirst = plain.wrap[1] .. "|" .. tostring(plain.wrap[4]) .. "|" .. tostring(plain.wrap[10])
    local utf = stml.Parse('<c "#ff0000">ab </c>' .. ("\195\169"):rep(80) .. " z")
    stml.DrawLine(utf, 50, nil, "k")
    r, rows = check(utf, 50)
    out.utf, out.utfRows = r, rows
    local tiny = stml.Parse('<c "#ff0000">a</c>bcd')
    stml.DrawLine(tiny, 1, nil, "k")
    r, rows = check(tiny, 1)
    out.tiny, out.tinyRows = r, rows
    local short = stml.Parse('<c "#ff0000">one </c>two')
    stml.DrawLine(short, 2000, nil, "k")
    out.short = #short.wrap // 6
  end
  -- drawn: the place under the mouse on the long word's second row is that piece's offset
  local view = out.view or {}
  out.view = view
  if frame == 1 then stml.UpdateLines(view, { long }) end
  ui.BeginChild("output", 100, 200, false)
  local x0, y0 = ui.GetCursorScreenPos()
  local _, rowH = ui.CalcTextSize("Ag")
  local sel = { rows = 0, color = { 1, 1, 1, 0.3 }, active = true, lineH = rowH }
  sel.mx, sel.my = x0 + 1, y0 + rowH * 1.5 + 2
  stml.DrawLines(view, 100, 200, "k100", nil, sel)
  local w = view.lines[1].wrap
  out.hit = tostring(sel.hitOff) .. "|" .. tostring(w and w[11])
  ui.EndChild()
  ui.End()
end)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        for (int i = 0; i < 3; ++i) {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        }
        auto get = [&](const char* key) {
            lua_getglobal(L, "out");
            if (!lua_istable(L, -1)) {
                lua_settop(L, 0);
                return std::string("no out");
            }
            lua_getfield(L, -1, key);
            std::string v = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
            lua_settop(L, 0);
            return v;
        };
        CHECK(get("plain") == "ok");
        CHECK(std::stoi(get("plainRows")) >= 4); // (120 letters in rows of 100 pixels)
        CHECK(get("plainFirst") == "x |false|true"); // the long word starts a row of its own
        CHECK(get("utf") == "ok");
        CHECK(std::stoi(get("utfRows")) >= 3);
        CHECK(get("tiny") == "ok");
        CHECK(get("tinyRows") == "4"); // a character a row, and no endless loop
        CHECK(get("short") == "2");    // words that fit are not broken
        const std::string hit = get("hit");
        const auto bar = hit.find('|');
        REQUIRE(bar != std::string::npos);
        CHECK(hit.substr(0, bar) == hit.substr(bar + 1));
        CHECK(hit.substr(0, bar) != "0");
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// The table calls ImGui does not guard: TableNextRow and the header row with no current table read through a null
// table, and a column or freeze set up after the first row asserts. The bindings refuse them; inside a table they
// work as before.
TEST_CASE("Lua UI: table calls outside a table, or set up after the first row, are refused") {
    hailstone::test::ScratchDir scratch("lua_ui_api_table_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "t.lua") << "ui.window('T', function()\n"
                                    "  ui.Begin('T')\n"
                                    "  ui.TableNextRow()\n"
                                    "  outside = ui.TableHeadersRowClicked()\n"
                                    "  ui.TableSetupColumn('x', 0, 0)\n"
                                    "  ui.TableSetupScrollFreeze(1, 1)\n"
                                    "  if ui.BeginTable('tab', 2, ui.TableFlags.ScrollY, 200, 100) then\n"
                                    "    ui.TableSetupColumn('A')\n"
                                    "    ui.TableSetupColumn('B')\n"
                                    "    ui.TableSetupScrollFreeze(-5, 100000)\n"
                                    "    inside = ui.TableHeadersRowClicked()\n"
                                    "    ui.TableSetupColumn('late')\n"
                                    "    ui.TableSetupScrollFreeze(1, 1)\n"
                                    "    ui.TableNextRow()\n"
                                    "    ui.TableNextColumn() ui.Text('a')\n"
                                    "    ui.EndTable()\n"
                                    "  end\n"
                                    "  ui.TableNextRow()\n"
                                    "  done = true\n"
                                    "  ui.End()\n"
                                    "end)\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        for (int i = 0; i < 2; ++i) {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        }
        lua_getglobal(L, "done");
        CHECK(lua_toboolean(L, -1));
        lua_getglobal(L, "outside");
        CHECK(lua_tointeger(L, -1) == -1);
        lua_getglobal(L, "inside");
        CHECK(lua_tointeger(L, -1) == -1);
        lua_pop(L, 3);
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// ui.Image, ui.ImageButton and ui.DrawImage draw only a texture the client handed out (ui/texture_ids.h): other
// userdata (a full userdata such as an mq.TLO value, a light one never made a texture, one since freed) is none, as
// nil is, and never reaches the draw list, which the Vulkan backend would bind as a descriptor set.
TEST_CASE("Lua UI: images draw only live textures the client handed out") {
    hailstone::test::ScratchDir scratch("lua_ui_api_image_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "i.lua") << "ui.window('I', function()\n"
                                    "  ui.Begin('I')\n"
                                    "  for _, t in ipairs({ FULL, STRANGER, FREED, LIVE }) do\n"
                                    "    ui.Image(t, 8, 8)\n"
                                    "    ui.ImageButton('b' .. tostring(_), t, 8, 8)\n"
                                    "    local x, y = ui.GetCursorScreenPos()\n"
                                    "    ui.DrawImage(t, x, y, x + 8, y + 8)\n"
                                    "  end\n"
                                    "  ui.End()\n"
                                    "end)\n";
    static int live = 0, stranger = 0, freed = 0;
    hailstone::ui::texture_id_added(&live);
    hailstone::ui::texture_id_added(&freed);
    hailstone::ui::texture_id_removed(&freed);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        lua_newuserdatauv(L, 16, 0);
        lua_setglobal(L, "FULL");
        lua_pushlightuserdata(L, &stranger);
        lua_setglobal(L, "STRANGER");
        lua_pushlightuserdata(L, &freed);
        lua_setglobal(L, "FREED");
        lua_pushlightuserdata(L, &live);
        lua_setglobal(L, "LIVE");
        int live_draws = 0, other_draws = 0;
        for (int i = 0; i < 2; ++i) {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
            live_draws = other_draws = 0;
            const ImTextureID font = ImGui::GetIO().Fonts->TexRef.GetTexID();
            const ImDrawData* data = ImGui::GetDrawData();
            for (const ImDrawList* list : data->CmdLists)
                for (const ImDrawCmd& cmd : list->CmdBuffer) {
                    if (cmd.ElemCount == 0) continue;
                    const ImTextureID id = cmd.GetTexID();
                    if (id == reinterpret_cast<ImTextureID>(&live)) ++live_draws;
                    else if (id != font) ++other_draws;
                }
        }
        CHECK(live_draws > 0);
        CHECK(other_draws == 0);
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    hailstone::ui::texture_id_removed(&live);
    ImGui::DestroyContext(ctx);
}

// ui.Image's optional r, g, b, a tint the image (ImageWithBg: Dear ImGui's Image takes no tint); without them it is
// drawn untinted.
TEST_CASE("Lua UI: ui.Image takes a tint") {
    hailstone::test::ScratchDir scratch("lua_ui_api_image_tint_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "i.lua") << "ui.window('I', function()\n"
                                    "  ui.Begin('I')\n"
                                    "  ui.Image(TINTED, 8, 8, 0, 0, 1, 1, 1, 0, 0, 1)\n"
                                    "  ui.Image(PLAIN, 8, 8)\n"
                                    "  ui.End()\n"
                                    "end)\n";
    static int tinted = 0, plain = 0;
    hailstone::ui::texture_id_added(&tinted);
    hailstone::ui::texture_id_added(&plain);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        lua_pushlightuserdata(L, &tinted);
        lua_setglobal(L, "TINTED");
        lua_pushlightuserdata(L, &plain);
        lua_setglobal(L, "PLAIN");
        std::vector<ImU32> tinted_cols, plain_cols;
        for (int i = 0; i < 2; ++i) {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
            tinted_cols.clear();
            plain_cols.clear();
            for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
                for (const ImDrawCmd& cmd : list->CmdBuffer) {
                    auto* out = cmd.GetTexID() == reinterpret_cast<ImTextureID>(&tinted) ? &tinted_cols
                                : cmd.GetTexID() == reinterpret_cast<ImTextureID>(&plain) ? &plain_cols
                                                                                          : nullptr;
                    for (unsigned e = 0; out && e < cmd.ElemCount; ++e)
                        out->push_back(list->VtxBuffer[static_cast<int>(cmd.VtxOffset + list->IdxBuffer[static_cast<int>(cmd.IdxOffset + e)])].col);
                }
        }
        REQUIRE_FALSE(tinted_cols.empty());
        REQUIRE_FALSE(plain_cols.empty());
        for (ImU32 c : tinted_cols) CHECK(c == IM_COL32(255, 0, 0, 255));
        for (ImU32 c : plain_cols) CHECK(c == IM_COL32(255, 255, 255, 255));
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    hailstone::ui::texture_id_removed(&tinted);
    hailstone::ui::texture_id_removed(&plain);
    ImGui::DestroyContext(ctx);
}

namespace {
// Two frames of a pack with one script; the global `done` after them.
bool run_pack_frames(const std::string& name, const std::string& script, int frames = 2,
                     const std::function<void(int)>& before_frame = {}) {
    hailstone::test::ScratchDir scratch(name);
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "s.lua") << script;
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    bool done = false;
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        for (int i = 0; i < frames; ++i) {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            if (before_frame) before_frame(i);
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        }
        lua_getglobal(L, "done");
        done = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
    return done;
}
} // namespace

TEST_CASE("Lua UI: ImGui's internal window flags are not a script's, and unbalanced ends are refused") {
    CHECK(run_pack_frames("lua_ui_api_flags_test", "ui.window('F', function()\n"
                                                   "  ui.Begin('F', nil, 1 << 26 | 1 << 27 | 1 << 28)\n"
                                                   "  if ui.BeginChild('c', 50, 50, false, 1 << 26 | 1 << 24) then ui.Text('x') end\n"
                                                   "  ui.EndChild()\n"
                                                   "  ui.EndMenu() ui.EndPopup() ui.EndCombo() ui.EndTooltip()\n"
                                                   "  ui.Text('still F')\n"
                                                   "  ui.End()\n"
                                                   "  done = true\n"
                                                   "end)\n"));
}

// ui.End and ui.EndChild close only a window the script began, of their kind; a window left open when the draw
// function returns is closed then, and what the script set for a next window it never began is dropped, so the
// next script's window neither nests in it nor takes that place.
TEST_CASE("Lua UI: a script ends only its own windows, and what it leaves open stays its own") {
    bool nested = true, moved = true, seen = false;
    CHECK(run_pack_frames("lua_ui_api_ends_test",
                          "ui.window('A', function()\n"
                          "  ui.End() ui.EndChild() ui.End()\n" // nothing begun: nothing ended (not the Debug window)
                          "  ui.Begin('A')\n"
                          "  if ui.BeginChild('c', 50, 50) then ui.End() end\n" // End is not EndChild
                          "  ui.EndChild()\n"
                          "  ui.Text('still A')\n"                              // and A is left open
                          "  ui.SetNextWindowPos(333, 333, ui.Cond.Always)\n"   // for a window never begun
                          "end)\n"
                          "ui.window('B', function()\n"
                          "  ui.SetNextWindowSize(100, 100)\n"
                          "  ui.Begin('B') ui.Text('b') ui.End()\n"
                          "  done = true\n"
                          "end)\n",
                          3, [&](int i) {
                              if (i == 0) return;
                              ImGuiWindow* b = ImGui::FindWindowByName("B");
                              if (!b) return;
                              seen = true;
                              const ImGuiWindow* in = b->ParentWindowInBeginStack;
                              nested = in && std::string(in->Name) == "A";
                              moved = b->Pos.x == 333.0f && b->Pos.y == 333.0f;
                          }));
    CHECK(seen);
    CHECK_FALSE(nested);
    CHECK_FALSE(moved);
}

// A text longer than the capacity is cut to it, the NUL inside the buffer ImGui is given.
TEST_CASE("Lua UI: InputText cuts a longer text to its capacity") {
    CHECK(run_pack_frames("lua_ui_api_inputcap_test",
                          "ui.window('I', function()\n"
                          "  ui.Begin('I')\n"
                          "  local a = ui.InputText('a', string.rep('x', 50), 10)\n"
                          "  local b = ui.InputTextWithHint('b', 'hint', string.rep('y', 50), 12)\n"
                          "  local c = ui.InputTextMultiline('c', string.rep('z', 50), 7)\n"
                          "  local d = ui.InputText('d', 'short', 10)\n"
                          "  ui.End()\n"
                          "  done = #a == 10 and #b == 12 and #c == 7 and d == 'short'\n"
                          "end)\n"));
}

TEST_CASE("Lua UI: style lists with NaN or huge indices and values push nothing undefined") {
    CHECK(run_pack_frames("lua_ui_api_style_nan_test",
                          "ui.window('S', function()\n"
                          "  local nc = ui.PushStyleColors({ 0/0, 1, 1, 1, 1, 1e300, 1, 1, 1, 1, -math.huge, 1, 1, 1, 1,\n"
                          "                                  ui.Col.Text, 1e300, 0/0, 1, 1 })\n"
                          "  local nv = ui.PushStyleVars({ 0/0, 1, false, math.huge, 1, 1, ui.StyleVar.Alpha, 1e300, false })\n"
                          "  ui.PopStyleVar(nv) ui.PopStyleColor(nc)\n"
                          "  done = nc == 1 and nv == 1\n"
                          "end)\n"));
}

TEST_CASE("Lua UI: a completion list whose __len raises is read raw, not left to unwind the editor") {
    CHECK(run_pack_frames("lua_ui_api_complete_len_test",
                          "local frame = 0\n"
                          "ui.window('E', function()\n"
                          "  frame = frame + 1\n"
                          "  ui.SetNextWindowPos(0, 0, ui.Cond.Always)\n"
                          "  ui.SetNextWindowSize(300, 200, ui.Cond.Always)\n"
                          "  ui.Begin('E')\n"
                          "  local opts = { complete = function(before)\n"
                          "    asked = true\n"
                          "    return 1, setmetatable({ 'abc' }, { __len = function() error('len') end })\n"
                          "  end }\n"
                          "  local text, changed, cursor, active = ui.CodeEditor('code', 'a', 200, 100, opts)\n"
                          "  ui.End()\n"
                          "  if frame >= 4 then done = asked == true end\n"
                          "end)\n",
                          5, [](int i) { // a click in the box makes it active
                              ImGui::GetIO().AddMousePosEvent(60, 70);
                              if (i == 1) ImGui::GetIO().AddMouseButtonEvent(0, true);
                              if (i == 2) ImGui::GetIO().AddMouseButtonEvent(0, false);
                          }));
}

TEST_CASE("Lua UI: code editors made from changing ids are forgotten, not kept for the session") {
    std::size_t kept = 0;
    // Sampled once the map has passed 256 and been swept: frame numbers run on across tests (ui::imgui_frame), so
    // editors an earlier test left behind can still be there at the start and are only dropped by that sweep.
    std::int64_t oldest_at_300 = -1, oldest = -1;
    CHECK(run_pack_frames("lua_ui_api_editors_test",
                          "local frame = 0\n"
                          "ui.window('C', function()\n"
                          "  frame = frame + 1\n"
                          "  ui.Begin('C')\n"
                          "  ui.CodeEditor('kept', 'x', 100, 50)\n"
                          "  ui.CodeEditor('id ' .. frame, 'x', 100, 50)\n"
                          "  ui.End()\n"
                          "  done = true\n"
                          "end)\n",
                          400, [&](int i) {
                              if (i == 300) oldest_at_300 = hailstone::ui::lua::code_editors_oldest();
                              if (i == 399) {
                                  kept = hailstone::ui::lua::code_editors_kept();
                                  oldest = hailstone::ui::lua::code_editors_oldest();
                              }
                          }));
    CHECK(kept > 0);
    CHECK(kept <= 258); // past 256, those not drawn last frame go
    REQUIRE(oldest_at_300 >= 0);
    CHECK(oldest == oldest_at_300); // the one drawn every frame was never forgotten
}

TEST_CASE("Lua UI: the code editor places text as CalcTextSize from the line's start does") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    ImGui::GetIO().DisplaySize = ImVec2(800, 600);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    const float line_h = ImGui::GetFontSize();
    auto expected = [&](const std::string& text, std::size_t to) {
        to = std::min(to, text.size());
        const std::size_t nl = to == 0 ? std::string::npos : text.rfind('\n', to - 1);
        const std::size_t start = nl == std::string::npos ? 0 : nl + 1;
        const auto line = static_cast<float>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(start), '\n'));
        return std::pair<float, float>(ImGui::CalcTextSize(text.data() + start, text.data() + to).x, line * line_h);
    };
    const std::vector<std::string> texts = {
        "local x = 1 -- a comment\nprint(x)\n",
        "--[[ a long\ncomment over\nthree lines ]] local s = 'abc'\n\tif x then\n\t\treturn \"t\\tab\" end",
        "caf\xc3\xa9 = '\xe2\x82\xac\xe2\x82\xac' -- multibyte\nna\xc3\xafve = 1\n\n\nlast",
        "",
    };
    for (const auto& text : texts) {
        CAPTURE(text);
        // Increasing offsets one byte at a time (the spans' walk), then a caret before the last (a restart), past
        // the end, and a few in no order.
        std::vector<std::size_t> at;
        for (std::size_t i = 0; i <= text.size(); ++i) at.push_back(i);
        at.push_back(text.size() / 2);
        at.push_back(text.size() + 5);
        at.push_back(1);
        at.push_back(text.size() / 3);
        at.push_back(0);
        const auto got = hailstone::ui::lua::code_editor_places(text, at);
        REQUIRE(got.size() == at.size());
        for (std::size_t i = 0; i < at.size(); ++i) {
            CAPTURE(at[i]);
            CHECK(got[i] == expected(text, at[i]));
        }
    }
    ImGui::EndFrame();
    ImGui::DestroyContext(ctx);
}

// The modern pack's "Close with Esc" (ui/modern/lib/escape.lua): off until ticked; one press closes one ticked window,
// the one opened or clicked last.
TEST_CASE("Modern UI libs: Close with Esc closes the frontmost ticked window, one a press") {
    hailstone::test::ScratchDir scratch("lua_ui_modern_escape_test");
    const fs::path dir = scratch.path() / "pack";
    const fs::path lib = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern" / "lib";
    REQUIRE(fs::exists(lib / "escape.lua"));
    fs::create_directories(dir);
    fs::copy(lib, dir / "lib", fs::copy_options::recursive);
    std::ofstream(dir / "esc.lua") << R"lua(
PRESS = false
hs = { TakeCloseTopWindow = function() local p = PRESS; PRESS = false; return p end }
TEST_UI_API_ATTACH(hs)
local esc = dofile("lib/escape.lua")
OPEN = { A = true, B = false, C = false }
local place = { A = 0, B = 200, C = 400 }
for _, name in ipairs({ "A", "B", "C" }) do
  ui.window(name, function()
    if not OPEN[name] then return end
    ui.SetNextWindowPos(place[name], 0, ui.Cond.Always)
    ui.SetNextWindowSize(150, 100, ui.Cond.Always)
    ui.Begin(name)
    if esc.Frame(name) then OPEN[name] = false end
    ui.End()
  end)
end
function tick(name) ui.SetSetting("hud.esc." .. name, "1") end
function open() return (OPEN.A and "A" or "") .. (OPEN.B and "B" or "") .. (OPEN.C and "C" or "") end
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        REQUIRE(hailstone::test::register_mock_ui_api(L) == LUA_OK);
        auto frames = [&](int n) {
            for (int i = 0; i < n; ++i) {
                ImGui::GetIO().DisplaySize = ImVec2(800, 600);
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                lua.frame();
                ImGui::Render();
            }
        };
        auto lua_do = [&](const char* code) {
            REQUIRE(luaL_dostring(L, code) == LUA_OK);
            std::string out = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return out;
        };
        auto press = [&] {
            lua_do("PRESS = true");
            frames(3);
        };
        frames(2);
        lua_do("OPEN.B = true");
        frames(2);
        lua_do("OPEN.C = true");
        frames(2);
        REQUIRE(lua_do("return open()") == "ABC");
        // nothing ticked: the press closes nothing, and is not kept for later
        press();
        CHECK(lua_do("return open()") == "ABC");
        lua_do("tick('A') tick('B')");
        frames(2);
        CHECK(lua_do("return open()") == "ABC");
        // C opened last but is not ticked: B, the ticked one opened last, goes; then A
        press();
        CHECK(lua_do("return open()") == "AC");
        press();
        CHECK(lua_do("return open()") == "C");
        press();
        CHECK(lua_do("return open()") == "C");
        // a click brings a window to the front: B opened after A, A clicked after that
        lua_do("OPEN.A, OPEN.B = true, true");
        frames(2);
        ImGui::GetIO().AddMousePosEvent(50, 50);
        frames(1);
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        frames(1);
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        frames(1);
        press();
        CHECK(lua_do("return open()") == "BC");
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// Some windows (the inventory, the books, the bags, the map...) have Close with Esc ticked until the player unticks it.
TEST_CASE("Modern UI libs: Close with Esc is on by default for the put-away windows, until unticked") {
    hailstone::test::ScratchDir scratch("lua_ui_modern_escape_default_test");
    const fs::path dir = scratch.path() / "pack";
    const fs::path lib = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern" / "lib";
    REQUIRE(fs::exists(lib / "escape.lua"));
    fs::create_directories(dir);
    fs::copy(lib, dir / "lib", fs::copy_options::recursive);
    std::ofstream(dir / "esc.lua") << R"lua(
PRESS = false
hs = { TakeCloseTopWindow = function() local p = PRESS; PRESS = false; return p end }
TEST_UI_API_ATTACH(hs)
local esc = dofile("lib/escape.lua")
NAMES = { "Other", "InventoryWindow", "Bag7", "ItemDisplayWindow#3" }
OPEN = { Other = true, InventoryWindow = true, Bag7 = true, ["ItemDisplayWindow#3"] = true }
for i, name in ipairs(NAMES) do
  ui.window(name, function()
    if not OPEN[name] then return end
    ui.SetNextWindowPos((i - 1) * 200, 0, ui.Cond.Always)
    ui.SetNextWindowSize(150, 100, ui.Cond.Always)
    ui.Begin(name)
    if esc.Frame(name) then OPEN[name] = false end
    ui.End()
  end)
end
function open()
  local s = ""
  for _, name in ipairs(NAMES) do if OPEN[name] then s = s .. name .. " " end end
  return s
end
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        REQUIRE(hailstone::test::register_mock_ui_api(L) == LUA_OK);
        auto frames = [&](int n) {
            for (int i = 0; i < n; ++i) {
                ImGui::GetIO().DisplaySize = ImVec2(800, 600);
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                lua.frame();
                ImGui::Render();
            }
        };
        auto lua_do = [&](const char* code) {
            REQUIRE(luaL_dostring(L, code) == LUA_OK);
            std::string out = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return out;
        };
        auto press = [&] {
            lua_do("PRESS = true");
            frames(3);
        };
        frames(2);
        // nothing saved: the inventory, the bag and the item display close, the other window does not
        press();
        press();
        press();
        CHECK(lua_do("return open()") == "Other ");
        press();
        CHECK(lua_do("return open()") == "Other ");
        // unticked ("0" saved), the inventory stays open; the bag still closes
        lua_do("ui.SetSetting('hud.esc.InventoryWindow', '0') OPEN.InventoryWindow, OPEN.Bag7 = true, true");
        frames(2);
        press();
        press();
        CHECK(lua_do("return open()") == "Other InventoryWindow ");
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// ui.Begin's collapseRight puts the collapse button beside the close box for that window alone (the style's
// WindowMenuButtonPosition is put back); ui.SetNextWindowCollapsed / ui.IsWindowCollapsed roll it up and say so.
TEST_CASE("Lua UI: a window rolls up, its collapse button at the right") {
    hailstone::test::ScratchDir scratch("lua_ui_api_collapse_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "roll.lua") << "ui.window('Roll', function()\n"
                                       "  ui.SetNextWindowPos(40, 50, ui.Cond.Always)\n"
                                       "  ui.SetNextWindowSize(200, 150, ui.Cond.Always)\n"
                                       "  if ROLL ~= nil then ui.SetNextWindowCollapsed(ROLL, 3) end\n"
                                       "  ui.Begin('Roll###roll', true, 0, true)\n"
                                       "  rolled = ui.IsWindowCollapsed()\n"
                                       "  ui.End()\n"
                                       "end)\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        auto frames = [&](int n) {
            for (int i = 0; i < n; ++i) {
                ImGui::GetIO().DisplaySize = ImVec2(800, 600);
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                lua.frame();
                ImGui::Render();
            }
        };
        auto rolled = [&] {
            lua_getglobal(L, "rolled");
            const bool v = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return v;
        };
        frames(2);
        CHECK(ImGui::GetStyle().WindowMenuButtonPosition == ImGuiDir_Left);
        CHECK_FALSE(rolled());
        // (a cond of more than one flag, which ImGui asserts on, is taken as Always)
        REQUIRE(luaL_dostring(L, "ROLL = true") == LUA_OK);
        frames(2);
        CHECK(rolled());
        ImGuiWindow* win = ImGui::FindWindowByName("###roll");
        REQUIRE(win);
        CHECK(win->Collapsed);
        // the collapse button left of the close box, at the title bar's right
        const ImGuiStyle& st = ImGui::GetStyle();
        const float fs = win->TitleBarHeight - st.FramePadding.y * 2;
        const ImVec2 at(win->Pos.x + win->Size.x - st.FramePadding.x - fs - st.ItemInnerSpacing.x - fs / 2,
                        win->Pos.y + st.FramePadding.y + fs / 2);
        REQUIRE(luaL_dostring(L, "ROLL = nil") == LUA_OK);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y);
        frames(1);
        io.AddMouseButtonEvent(0, true);
        frames(1);
        io.AddMouseButtonEvent(0, false);
        frames(2);
        CHECK_FALSE(rolled());
        CHECK(ImGui::GetStyle().WindowMenuButtonPosition == ImGuiDir_Left);
        CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// ui/modern/browser.lua with a stand-in for hs's browser calls and lib/hud.lua: a first tab that cannot open is not
// asked for again each frame, and why is shown; "+" failing keeps the tab in front.
TEST_CASE("Modern UI: the browser shows why its first tab did not open and keeps the tab in front") {
    hailstone::test::ScratchDir scratch("lua_ui_browser_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir / "lib");
    const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern" / "browser.lua";
    if (!fs::exists(src)) return; // an installed test binary run away from the sources
    fs::copy_file(src, dir / "browser.lua");
    std::ofstream(dir / "lib" / "hud.lua") << R"lua(
return { Dialog = function(_, title, _, draw)
  ui.SetNextWindowPos(0, 0, ui.Cond.Always); ui.SetNextWindowSize(700, 500, ui.Cond.Always)
  ui.Begin(title); draw(0, 0, 700, 500, 1); ui.End()
end }
)lua";
    std::ofstream(dir / "a_eq.lua") << R"lua(
B = { opens = 0, fail = "no browser process", tabs = {} }
hs = {
  WebAvailable = function() return true end,
  WebTabs = function() return B.tabs end,
  WebOpen = function()
    B.opens = B.opens + 1
    if B.fail then return nil, B.fail end
    B.tabs[#B.tabs + 1] = #B.tabs + 1
    return #B.tabs
  end,
  WebState = function(id) return { title = "t" .. id, url = "about:blank", canBack = false, canForward = false } end,
  WebNav = function() end, WebNavigate = function() return true end, WebClose = function() end,
  WebOpenExternal = function() end, WebView = function(id) B.viewed = id end,
}
TEST_UI_API_ATTACH(hs)
-- what the window shows in red, and a button "pressed" by name for one frame
local textColored, button, small = ui.TextColored, ui.Button, ui.SmallButton
ui.TextColored = function(r, g, b, a, t) B.shown = t; return textColored(r, g, b, a, t) end
local function pressing(real) return function(label, ...)
  local hit = real(label, ...)
  if B.press == label then B.press = nil; return true end
  return hit
end end
ui.Button, ui.SmallButton = pressing(button), pressing(small)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        REQUIRE(hailstone::test::register_mock_ui_api(L) == LUA_OK);
        auto frames = [&](int n) {
            for (int i = 0; i < n; ++i) {
                ImGui::GetIO().DisplaySize = ImVec2(800, 600);
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                lua.frame();
                ImGui::Render();
            }
        };
        auto eval = [&](const char* expr) {
            REQUIRE(luaL_dostring(L, (std::string("return ") + expr).c_str()) == LUA_OK);
            std::string out = luaL_tolstring(L, -1, nullptr);
            lua_settop(L, 0);
            return out;
        };
        frames(5);
        REQUIRE_FALSE(ImGui::FindWindowByName("UI errors"));
        CHECK(eval("B.opens") == "1"); // asked once, not each frame
        CHECK(eval("B.shown") == "no browser process");
        REQUIRE(luaL_dostring(L, "B.fail = nil") == LUA_OK);
        frames(2);
        CHECK(eval("B.opens") == "1"); // not again until asked
        // "Try again" opens it
        REQUIRE(luaL_dostring(L, "B.press = 'Try again##webretry'") == LUA_OK);
        frames(3);
        CHECK(eval("B.opens") == "2");
        CHECK(eval("B.viewed") == "1");
        // "+" failing: the tab in front stays, and why is shown
        REQUIRE(luaL_dostring(L, "B.fail = 'too many tabs'; B.shown = nil; B.press = '+##newtab'") == LUA_OK);
        frames(2);
        CHECK(eval("B.opens") == "3");
        CHECK(eval("B.viewed") == "1");
        CHECK(eval("B.shown") == "too many tabs");
        REQUIRE_FALSE(ImGui::FindWindowByName("UI errors"));
    }
    ImGui::DestroyContext(ctx);
}

// A script's PopStyleColor, PopStyleVar and PopFont pop only what it pushed: what the code drawing around it pushed
// stays (ImGui's recovery after the window can push nothing back).
TEST_CASE("Lua UI: a script pops only the style it pushed") {
    hailstone::test::ScratchDir scratch("lua_ui_api_pops_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "s.lua") << "ui.window('P', function()\n"
                                    "  ui.Begin('P')\n"
                                    "  ui.PushStyleColor(ui.Col.Text, 1, 0, 0, 1)\n"
                                    "  ui.PopStyleColor(5) ui.PopStyleVar(5) ui.PopFont() ui.PopFont()\n"
                                    "  ui.End()\n"
                                    "  done = true\n"
                                    "end)\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        for (int i = 0; i < 2; ++i) {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 1, 0, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
            ImGui::PushFont(ImGui::GetFont(), 0.0f);
            const int colors = GImGui->ColorStack.Size, vars = GImGui->StyleVarStack.Size,
                      fonts = GImGui->FontStack.Size;
            lua.frame();
            CHECK(GImGui->ColorStack.Size == colors);
            CHECK(GImGui->StyleVarStack.Size == vars);
            CHECK(GImGui->FontStack.Size == fonts);
            CHECK(ImGui::GetStyleColorVec4(ImGuiCol_Text).y == 1.0f);
            ImGui::PopFont();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::Render();
        }
        lua_getglobal(lua.state(), "done");
        CHECK(lua_toboolean(lua.state(), -1));
        lua_pop(lua.state(), 1);
    }
    ImGui::DestroyContext(ctx);
}

// EndDragDropSource and EndDragDropTarget without their Begin do nothing: ImGui's would end the tooltip it began, and
// with none, in a release build, the script's own window.
namespace {
int current_window_name(lua_State* L) {
    lua_pushstring(L, GImGui->CurrentWindow ? GImGui->CurrentWindow->Name : "");
    return 1;
}
} // namespace

TEST_CASE("Lua UI: ending a drag and drop never begun leaves the window open") {
    hailstone::test::ScratchDir scratch("lua_ui_api_dragdrop_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "s.lua") << "ui.window('D', function()\n"
                                    "  ui.Begin('D')\n"
                                    "  ui.EndDragDropSource() ui.EndDragDropTarget()\n"
                                    "  done = current_window_name() == 'D'\n"
                                    "  ui.End()\n"
                                    "end)\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_register(lua.state(), "current_window_name", &current_window_name);
        ImGui::GetIO().DisplaySize = ImVec2(800, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        lua.frame();
        ImGui::Render();
        lua_getglobal(lua.state(), "done");
        CHECK(lua_toboolean(lua.state(), -1));
        lua_pop(lua.state(), 1);
    }
    ImGui::DestroyContext(ctx);
}

// InputText's options are read raw (a metamethod there could raise past the buffer's destructor), and the edited
// text is pushed as before.
TEST_CASE("Lua UI: InputText reads its options raw") {
    CHECK(run_pack_frames("lua_ui_api_input_opts_test",
                          "ui.window('I', function()\n"
                          "  ui.Begin('I')\n"
                          "  local proxy = setmetatable({}, { __index = function() touched = true; error('no') end })\n"
                          "  local a = ui.InputText('a', 'abc', 10, 0, proxy)\n"
                          "  local b = ui.InputText('b', 'xyz', 10, 0, { moveToEnd = true })\n"
                          "  local n, changed = ui.SliderInt('s', 3, 0, 10, '%d%%')\n"
                          "  ui.End()\n"
                          "  done = a == 'abc' and b == 'xyz' and n == 3 and not touched\n"
                          "end)\n"));
}

// A window registered outside its script's loading (by code run later) that fails is reported and logged on its
// own, with no script to reload, and runs again once registered anew.
TEST_CASE("Lua UI: a window with no script reports its error and comes back when registered again") {
    hailstone::test::ScratchDir scratch("lua_ui_api_late_window_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "s.lua") << "ui.window('Base', function() end)\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        auto frame = [&] {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        };
        frame();
        CHECK(lua.run_for_test("", "ui.window('Late', function() LATE = (LATE or 0) + 1 error('late boom') end) "
                                   "return 1") == "1");
        frame();
        frame();
        CHECK(lua.run_for_test("", "return LATE") == "1"); // failed once, then off
        ImGuiWindow* errors = ImGui::FindWindowByName("UI errors");
        REQUIRE(errors != nullptr);
        CHECK(errors->Active);
        CHECK(lua.run_for_test("", "ui.window('Late', function() LATE = LATE + 1 end) return 1") == "1");
        frame();
        frame();
        CHECK(lua.run_for_test("", "return LATE") == "3");
        CHECK_FALSE(ImGui::FindWindowByName("UI errors")->Active);
    }
    ImGui::DestroyContext(ctx);
}

// A pattern match runs no Lua instructions, so the count hook never stopped one: the vendored Lua's match calls the
// state's check (lua_host.h kMatchCheckKey), which stops it at the code's time limit.
TEST_CASE("Lua UI: a runaway string match is stopped at the time limit") {
    hailstone::test::ScratchDir scratch("lua_ui_api_match_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua.set_limits_for_test(std::size_t{256} << 20, std::chrono::milliseconds(200), std::chrono::milliseconds(100));
        const auto start = std::chrono::steady_clock::now();
        for (const char* code : {"return string.find(('a'):rep(1e5), ('a-'):rep(20) .. 'b')",
                                 "return (string.gsub(('a'):rep(1e5), ('a-'):rep(20) .. 'b', ''))",
                                 "for x in string.gmatch(('a'):rep(1e5), ('a*'):rep(20) .. 'b') do end"})
            CHECK(lua.run_for_test("", code).find("ran too long") != std::string::npos);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
        CHECK(lua.run_for_test("", "return string.find(('a'):rep(100) .. 'b', 'a-b')") == "1"); // runs as before
    }
    ImGui::DestroyContext(ctx);
}
