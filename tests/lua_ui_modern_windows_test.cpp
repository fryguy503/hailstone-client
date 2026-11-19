// Modern pack windows run against stub helpers (lib/*.lua written here) and a stub hs: the parcel money
// confirmation (merchant.lua), the Lua Scripts search (scripts.lua) and the quantity dialog's minimum
// (quantity.lua). Each window file is the pack's own, copied into a scratch pack beside the stubs.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "ui/lua/lua_host.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;

namespace {

// A scratch pack holding `window` from ui/modern, the stub files given, and an ImGui context to run frames in.
// run(n) draws n frames; eval(expr) gives a Lua expression's value as text.
struct ModernPack {
    hailstone::test::ScratchDir scratch;
    fs::path dir;
    bool ok = false;

    ModernPack(const char* name, const char* window) : scratch(name), dir(scratch.path() / "pack") {
        const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern" / window;
        if (!fs::exists(src)) return; // an installed test binary run away from the sources
        fs::create_directories(dir / "lib");
        fs::copy_file(src, dir / window);
        ok = true;
    }
    void write(const fs::path& rel, const char* text) { std::ofstream(dir / rel) << text; }

    void run(const std::function<void(const std::function<void(int)>&, const std::function<std::string(const char*)>&,
                                      lua_State*)>& body) {
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
            lua_State* L = lua.state();
            REQUIRE(hailstone::test::register_mock_ui_api(L) == LUA_OK);
            auto frames = [&](int n) {
                for (int i = 0; i < n; ++i) {
                    ImGui::GetIO().DisplaySize = ImVec2(1000, 800);
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
            body(frames, eval, L);
            REQUIRE_FALSE(ImGui::FindWindowByName("UI errors"));
        }
        ImGui::DestroyContext(ctx);
    }
};

// A hud whose Dialog draws the body in a plain window, and a T with the drawing helpers as no-ops; T.Button is
// pressed for one frame when B.press names its id.
const char* kHud = R"lua(
local any = { 1, 1, 1, 1 }
local T = {
  color = setmetatable({}, { __index = function() return any end }),
  font = {},
  Text = function() return 10, 10 end,
  TextSize = function() return 10, 10 end,
  Fit = function(t) return t end,
  Icon = function() end, Hit = function() return false end, TakeRightClick = function() end,
  Thousands = function(n) return tostring(n) end,
  Button = function(id) if B.press == id then B.press = nil; return true end return false end,
}
return { T = T, Dialog = function(_, title, _, draw)
  ui.SetNextWindowPos(0, 0, ui.Cond.Always); ui.SetNextWindowSize(700, 700, ui.Cond.Always)
  ui.Begin(title); draw(0, 0, 700, 700, 1); ui.End()
end }
)lua";

} // namespace

TEST_CASE("Modern UI: Send money by parcel asks first, and sends the amount and name it showed") {
    ModernPack pack("lua_ui_merchant_money_test", "merchant.lua");
    if (!pack.ok) return;
    pack.write("lib/hud.lua", kHud);
    pack.write("lib/widgets.lua", R"lua(
return {
  Tabs = function() return 2 end, -- the Parcels page
  Edit = function(_, id) if id == "MW_Send_To_Edit" then return B.to end return "" end,
  Button = function() return false end, Text = function() return "" end, State = function() return nil end,
  Check = function() end, ButtonRow = function() end,
}
)lua");
    pack.write("lib/trade_common.lua", R"lua(
return { Caption = function() end, Empty = function() end, Amount = function() end, Purse = function() return 0 end,
  Copper = function() return 0 end, Split = function() return { 0, 0, 0, 0 } end }
)lua");
    pack.write("lib/itemtools.lua",
               "return { JunkCarried = function() return 0 end, Note = function() end, SellJunk = function() end }");
    pack.write("lib/items.lua", "return { NameTooltip = function() end }");
    pack.write("a_eq.lua", R"lua(
B = { sent = {}, to = "Bob", coin = "5" }
hs = {
  InGame = function() return true end,
  Money = function() return 100, 0, 0, 0 end,
  Parcels = function() return {}, 0, false, false, nil, nil end,
  ItemJob = function() return nil end,
  SendParcelMoney = function(...) B.sent[#B.sent + 1] = table.concat({ ... }, ",") end,
}
TEST_UI_API_ATTACH(hs)
-- the platinum box holds B.coin; a ui.Button is pressed for one frame when B.button names its label; the popup's
-- question is kept
local input, button, text = ui.InputTextWithHint, ui.Button, ui.Text
ui.InputTextWithHint = function(label, ...)
  if label == "##coin1" then return B.coin, true end
  return input(label, ...)
end
ui.Button = function(label, ...)
  local hit = button(label, ...)
  if B.button == label then B.button = nil; return true end
  return hit
end
ui.Text = function(t, ...) if t:find("^Send ") then B.asked = t end return text(t, ...) end
)lua");
    pack.run([](const auto& frames, const auto& eval, lua_State* L) {
        frames(1);
        REQUIRE(luaL_dostring(L, "B.press = '##sendMoney'") == LUA_OK); // the Money side
        frames(2);
        // Send money opens the question; nothing is sent
        REQUIRE(luaL_dostring(L, "B.press = '##sendmoneybtn'") == LUA_OK);
        frames(1);
        CHECK(eval("#B.sent") == "0");
        CHECK(eval("B.asked") == "Send 5 platinum to Bob?");
        // the name typed after the click does not change what was asked
        REQUIRE(luaL_dostring(L, "B.to = 'Someone'") == LUA_OK);
        frames(1);
        CHECK(eval("#B.sent") == "0");
        REQUIRE(luaL_dostring(L, "B.button = 'Send'") == LUA_OK);
        frames(1);
        CHECK(eval("#B.sent") == "1");
        CHECK(eval("B.sent[1]") == "Bob,,5,0,0,0");
        frames(2);
        CHECK(eval("#B.sent") == "1"); // once
        // Cancel sends nothing
        REQUIRE(luaL_dostring(L, "B.press = '##sendmoneybtn'") == LUA_OK);
        frames(1);
        REQUIRE(luaL_dostring(L, "B.button = 'Cancel'") == LUA_OK);
        frames(3);
        CHECK(eval("#B.sent") == "1");
    });
}

TEST_CASE("Modern UI: the Lua Scripts search finds a name written with capitals") {
    ModernPack pack("lua_ui_scripts_search_test", "scripts.lua");
    if (!pack.ok) return;
    pack.write("lib/hud.lua", kHud);
    pack.write("a_eq.lua", R"lua(
B = { filter = "myscr", rows = {} }
local function script(name)
  return { name = name, status = "", allowed = true, found = true, autorun = false, server = false, caps = "",
    why = "", error = "", kept = false }
end
hs = {
  InGame = function() return true end,
  Scripts = function() return { script("MyScript"), script("other") }, { autorun = true, folders = {} } end,
}
TEST_UI_API_ATTACH(hs)
local input, push = ui.InputTextWithHint, ui.PushID
ui.InputTextWithHint = function(label, ...)
  if label == "##filter" then return B.filter end
  return input(label, ...)
end
ui.PushID = function(id, ...) B.rows[#B.rows + 1] = id; return push(id, ...) end
)lua");
    pack.run([](const auto& frames, const auto& eval, lua_State* L) {
        frames(1);
        REQUIRE(luaL_dostring(L, "B.rows = {}") == LUA_OK);
        frames(1);
        CHECK(eval("table.concat(B.rows, ' ')") == "MyScript");
        REQUIRE(luaL_dostring(L, "B.rows = {}; B.filter = 'MYSCRIPT'") == LUA_OK);
        frames(1);
        CHECK(eval("table.concat(B.rows, ' ')") == "MyScript");
    });
}

TEST_CASE("Modern UI: the quantity dialog reads its minimum again when opened for another caller") {
    ModernPack pack("lua_ui_quantity_base_test", "quantity.lua");
    if (!pack.ok) return;
    pack.write("lib/hud.lua", kHud);
    pack.write("lib/widgets.lua", R"lua(
return {
  Edit = function() end, Button = function() end,
  Text = function(_, id) if id == "QTYW_SliderInput" then return Q.text end return "" end,
  Slider = function(_, _, opts) Q.shownMin = opts.format(0) end,
}
)lua");
    pack.write("a_eq.lua", R"lua(
Q = { shown = true, place = true, text = "5", value = 4, count = 10 }
hs = {
  InGame = function() return true end,
  ReplaceWindow = function()
    local place = Q.place
    Q.place = false
    return Q.shown, 10, 10, nil, nil, nil, place
  end,
  ChildState = function() return nil, nil, Q.value, Q.count end,
  SetSlider = function() end, WindowButton = function() end, ShowWindow = function() end,
}
TEST_UI_API_ATTACH(hs)
)lua");
    pack.run([](const auto& frames, const auto& eval, lua_State* L) {
        frames(2);
        CHECK(eval("Q.shownMin") == "1 of 10"); // the box's 5 less the slider's 4
        // opened again by another slot while still up: its own minimum
        REQUIRE(luaL_dostring(L, "Q.place, Q.text, Q.value, Q.count = true, '20', 0, 30") == LUA_OK);
        frames(2);
        CHECK(eval("Q.shownMin") == "20 of 49");
    });
}

TEST_CASE("Modern UI: the quantity box shows the new request's amount when the dialog opens again while focused") {
    ModernPack pack("lua_ui_quantity_reopen_test", "quantity.lua");
    if (!pack.ok) return;
    pack.write("lib/hud.lua", kHud);
    // The box is a real InputText, as W.Edit's is: focused when the dialog opens, it keeps the text it was given.
    pack.write("lib/widgets.lua", R"lua(
return {
  Edit = function(_, id, opts) ui.InputText("##" .. id, Q.text, 256) end,
  Button = function() end,
  Text = function(_, id) if id == "QTYW_SliderInput" then return Q.text end return "" end,
  Slider = function() end,
}
)lua");
    pack.write("a_eq.lua", R"lua(
Q = { shown = true, place = true, text = "100", value = 99, count = 100 }
hs = {
  InGame = function() return true end,
  ReplaceWindow = function()
    local place = Q.place
    Q.place = false
    return Q.shown, 10, 10, nil, nil, nil, place
  end,
  ChildState = function() return nil, nil, Q.value, Q.count end,
  SetSlider = function() end, WindowButton = function() end, ShowWindow = function() end,
}
TEST_UI_API_ATTACH(hs)
)lua");
    pack.run([](const auto& frames, const auto&, lua_State* L) {
        auto box_text = [] {
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            REQUIRE(g.ActiveId != 0);
            REQUIRE(g.InputTextState.ID == g.ActiveId);
            return std::string(g.InputTextState.TextA.Data);
        };
        frames(3);
        CHECK(box_text() == "100"); // the Buy dialog's box, focused
        // Sell asks while it is still up: the box shows that request's amount, not the 100 it was editing.
        REQUIRE(luaL_dostring(L, "Q.place, Q.text, Q.value, Q.count = true, '5', 4, 5") == LUA_OK);
        frames(3);
        CHECK(box_text() == "5");
    });
}
