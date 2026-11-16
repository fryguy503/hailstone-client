// The shared Lua UI colour widgets: live RGB values, a hue wheel, swatches and hostile-argument boundaries.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "ui/lua/lua_host.h"

namespace {

struct ColorWidgets {
    ImGuiContext* previous_context = ImGui::GetCurrentContext();
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{ImGui::CreateContext(), ImGui::DestroyContext};
    std::unique_ptr<lua_State, decltype(&lua_close)> lua{luaL_newstate(), lua_close};
    bool previous_guard = hailstone::ui::lua::consent_guard();

    ColorWidgets() {
        REQUIRE(context);
        REQUIRE(lua);
        ImGui::SetCurrentContext(context.get());
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1000, 1000);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        luaL_openlibs(lua.get());
        hailstone::ui::lua::register_ui_api(lua.get());
        hailstone::ui::lua::set_consent_guard(false);
    }
    ~ColorWidgets() {
        hailstone::ui::lua::set_consent_guard(previous_guard);
        ImGui::SetCurrentContext(previous_context);
    }
    void run(const char* source) {
        const int result = luaL_dostring(lua.get(), source);
        const char* message = result == LUA_OK ? "" : lua_tostring(lua.get(), -1);
        if (!message) message = "Lua execution failed";
        CHECK_MESSAGE(result == LUA_OK, message);
        lua_settop(lua.get(), 0);
    }
    void frame(const char* source) {
        ImGui::SetCurrentContext(context.get());
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(650, 900), ImGuiCond_Always);
        ImGui::Begin("Colour widgets", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        const int ids = ImGui::GetCurrentWindow()->IDStack.Size;
        const int groups = GImGui->GroupStack.Size;
        run(source);
        CHECK(ImGui::GetCurrentWindow()->IDStack.Size == ids);
        CHECK(GImGui->GroupStack.Size == groups);
        ImGui::End();
        ImGui::Render();
        const auto* data = ImGui::GetDrawData();
        REQUIRE(data);
        for (int i = 0; i < data->CmdListsCount; ++i) {
            for (const auto& vertex : data->CmdLists[i]->VtxBuffer) {
                CHECK(std::isfinite(vertex.pos.x));
                CHECK(std::isfinite(vertex.pos.y));
            }
        }
    }
    double number(const char* name) const {
        lua_getglobal(lua.get(), name);
        CHECK(lua_isnumber(lua.get(), -1));
        const double value = lua_tonumber(lua.get(), -1);
        lua_pop(lua.get(), 1);
        return value;
    }
    bool boolean(const char* name) const {
        lua_getglobal(lua.get(), name);
        CHECK(lua_isboolean(lua.get(), -1));
        const bool value = lua_toboolean(lua.get(), -1) != 0;
        lua_pop(lua.get(), 1);
        return value;
    }
    void mouse(float x, float y) { ImGui::GetIO().AddMousePosEvent(x, y); }
    void button(bool down) { ImGui::GetIO().AddMouseButtonEvent(0, down); }
};

} // namespace

TEST_CASE("Lua colour widgets: calls outside an active frame fail before entering ImGui") {
    ColorWidgets widgets;
    constexpr const char* refuse = R"lua(
local count=0
for _,widget in ipairs({ui.ColorButton,ui.ColorPicker3,ui.ColorEdit3}) do
  local ok,why=pcall(widget,'outside',0.2,0.4,0.6)
  assert(not ok and why:find('active ImGui frame',1,true))
  count=count+1
end
REJECTED=count
)lua";
    widgets.run(refuse); // context exists, NewFrame has not run
    CHECK(widgets.number("REJECTED") == 3);
    ImGui::SetCurrentContext(nullptr);
    widgets.run(refuse); // unavailable context is also a Lua error
    CHECK(widgets.number("REJECTED") == 3);
    ImGui::SetCurrentContext(widgets.context.get());
    widgets.frame("ui.ColorButton('valid',0.2,0.4,0.6)");
    widgets.run(refuse); // Render closed the frame
    CHECK(widgets.number("REJECTED") == 3);
}

TEST_CASE("Lua colour widgets: RGB values clamp and curated constants are inherited") {
    ColorWidgets widgets;
    constexpr const char* code = R"lua(
local flags = ui.ColorEditFlags
CONSTANTS = flags.PickerHueWheel ~= nil and flags.DisplayRGB ~= nil and flags.InputRGB ~= nil
  and flags.NoSidePreview ~= nil and flags.NoAlpha ~= nil and flags.NoSmallPreview ~= nil
  and flags.HDR == nil and flags.InputHSV == nil
local ImGui = setmetatable({}, {__index=ui})
INHERITED = ImGui.ColorPicker3 == ui.ColorPicker3 and ImGui.ColorButton == ui.ColorButton
  and ImGui.ColorEditFlags == flags
ui.SetNextItemWidth(300)
R,G,B,CHANGED = ImGui.ColorPicker3('wheel', -1e300, 0.25, 1e300)
ui.SetNextItemWidth(300)
ER,EG,EB,ECHANGED = ImGui.ColorEdit3('editor', -3, 0.5, 8, flags.NoPicker)
)lua";
    widgets.frame(code);
    widgets.frame(code);
    CHECK(widgets.boolean("CONSTANTS"));
    CHECK(widgets.boolean("INHERITED"));
    CHECK(widgets.number("R") == 0);
    CHECK(widgets.number("G") == doctest::Approx(0.25));
    CHECK(widgets.number("B") == 1);
    CHECK_FALSE(widgets.boolean("CHANGED"));
    CHECK(widgets.number("ER") == 0);
    CHECK(widgets.number("EG") == doctest::Approx(0.5));
    CHECK(widgets.number("EB") == 1);
    CHECK_FALSE(widgets.boolean("ECHANGED"));
}

TEST_CASE("Lua colour widgets: clicking a swatch reports a press and draws its bounded RGB") {
    ColorWidgets widgets;
    widgets.run("PRESSES=0");
    constexpr const char* code = R"lua(
local f=ui.ColorEditFlags
local clicked=ui.ColorButton('swatch',-5,0.5,2,nil,f.NoTooltip|f.NoDragDrop,44,24)
if clicked then PRESSES=PRESSES+1 end
X0,Y0,X1,Y1=ui.GetItemRect()
)lua";
    widgets.frame(code);
    widgets.frame(code);
    CHECK(widgets.number("X1") - widgets.number("X0") == doctest::Approx(44));
    CHECK(widgets.number("Y1") - widgets.number("Y0") == doctest::Approx(24));
    const float x = static_cast<float>((widgets.number("X0") + widgets.number("X1")) / 2);
    const float y = static_cast<float>((widgets.number("Y0") + widgets.number("Y1")) / 2);
    widgets.mouse(x, y);
    widgets.frame(code);
    widgets.button(true);
    widgets.frame(code);
    widgets.button(false);
    widgets.frame(code);
    CHECK(widgets.number("PRESSES") == 1);
    const ImU32 expected = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0.5f, 1, 1));
    const auto* window = ImGui::FindWindowByName("Colour widgets");
    REQUIRE(window);
    CHECK(std::any_of(window->DrawList->VtxBuffer.begin(), window->DrawList->VtxBuffer.end(),
                      [expected](const ImDrawVert& vertex) { return vertex.col == expected; }));
}

TEST_CASE("Lua colour widgets: the default hue wheel updates normalized RGB while held") {
    ColorWidgets widgets;
    widgets.run("R,G,B,EDITS=1,0,0,0");
    constexpr const char* code = R"lua(
X,Y=ui.GetCursorScreenPos()
ui.SetNextItemWidth(300)
local changed
R,G,B,changed=ui.ColorPicker3('wheel',R,G,B)
if changed then EDITS=EDITS+1 end
)lua";
    widgets.frame(code);
    widgets.frame(code);
    const float bar = ImGui::GetFrameHeight();
    const float size = 300 - bar - ImGui::GetStyle().ItemInnerSpacing.x;
    const float radius = size * 0.46f;
    const float center_x = static_cast<float>(widgets.number("X")) + (size + bar) * 0.5f;
    const float center_y = static_cast<float>(widgets.number("Y")) + size * 0.5f;
    widgets.mouse(center_x - radius * 0.5f, center_y + radius * 0.8660254f);
    widgets.frame(code);
    widgets.button(true);
    widgets.frame(code);
    CHECK(widgets.number("EDITS") >= 1);
    CHECK(widgets.number("R") < 0.01);
    CHECK(widgets.number("G") > 0.99);
    CHECK(widgets.number("B") < 0.01);
    widgets.button(false);
    widgets.frame(code);
}

TEST_CASE("Lua colour widgets: RGB integer controls accept keyboard edits") {
    ColorWidgets widgets;
    widgets.run("R,G,B,EDITS=0,0.25,0.5,0");
    constexpr const char* code = R"lua(
X,Y=ui.GetCursorScreenPos()
ui.SetNextItemWidth(360)
local f=ui.ColorEditFlags
local changed
R,G,B,changed=ui.ColorEdit3('rgb',R,G,B,f.NoSmallPreview|f.NoPicker|f.DisplayRGB)
if changed then EDITS=EDITS+1 end
)lua";
    widgets.frame(code);
    widgets.frame(code);
    widgets.mouse(static_cast<float>(widgets.number("X")) + 40,
                  static_cast<float>(widgets.number("Y")) + ImGui::GetFrameHeight() / 2);
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    widgets.frame(code);
    widgets.button(true);
    widgets.frame(code);
    widgets.button(false);
    widgets.frame(code);
    io.AddKeyEvent(ImGuiKey_A, true);
    widgets.frame(code);
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    io.AddInputCharactersUTF8("128");
    widgets.frame(code);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    widgets.frame(code);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    widgets.frame(code);
    CHECK(widgets.number("EDITS") >= 1);
    CHECK(widgets.number("R") == doctest::Approx(128.0 / 255).epsilon(0.001));
    CHECK(widgets.number("G") == doctest::Approx(0.25).epsilon(0.01));
    CHECK(widgets.number("B") == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("Lua colour widgets: invalid numbers and flag types fail before changing widget stacks") {
    ColorWidgets widgets;
    widgets.frame(R"lua(
local rejected=0
local function refuses(fn,...) if not pcall(fn,...) then rejected=rejected+1 end end
for _,bad in ipairs({0/0,math.huge,-math.huge}) do
  refuses(ui.ColorPicker3,'bad',bad,0,0)
  refuses(ui.ColorEdit3,'bad',0,bad,0)
  refuses(ui.ColorButton,'bad',0,0,bad)
  refuses(ui.ColorButton,'bad',0,0,0,bad)
  refuses(ui.ColorButton,'bad',0,0,0,1,0,bad,20)
end
for _,bad in ipairs({true,{},'1',1.5,-1,math.huge,0/0}) do
  refuses(ui.ColorPicker3,'bad',0,0,0,bad)
  refuses(ui.ColorEdit3,'bad',0,0,0,bad)
  refuses(ui.ColorButton,'bad',0,0,0,1,bad)
end
REJECTED=rejected
R,G,B,CHANGED=ui.ColorPicker3('still usable',0.2,0.4,0.6,ui.ColorEditFlags.NoInputs)
)lua");
    CHECK(widgets.number("REJECTED") == 36);
    CHECK(widgets.number("R") == doctest::Approx(0.2));
    CHECK(widgets.number("G") == doctest::Approx(0.4));
    CHECK(widgets.number("B") == doctest::Approx(0.6));
    CHECK_FALSE(widgets.boolean("CHANGED"));
}

TEST_CASE("Lua colour widgets: conflicting and unknown flag bits stay within supported RGB modes") {
    ColorWidgets widgets;
    const ImGuiColorEditFlags previous = GImGui->ColorEditOptions;
    widgets.frame(R"lua(
local f=ui.ColorEditFlags
local choices=f.PickerHueBar|f.PickerHueWheel|f.Uint8|f.Float|f.DisplayRGB|f.DisplayHSV|f.DisplayHex
R,G,B,CHANGED=ui.ColorPicker3('conflicting',0.2,0.4,0.6,choices)
ER,EG,EB,ECHANGED=ui.ColorEdit3('all flags',0.2,0.4,0.6,math.maxinteger)
PRESSED=ui.ColorButton('bounded size',0.2,0.4,0.6,1,math.maxinteger,1e300,-1e300)
X0,Y0,X1,Y1=ui.GetItemRect()
)lua");
    CHECK(widgets.number("R") == doctest::Approx(0.2));
    CHECK(widgets.number("G") == doctest::Approx(0.4));
    CHECK(widgets.number("B") == doctest::Approx(0.6));
    CHECK(widgets.number("ER") == doctest::Approx(0.2));
    CHECK(widgets.number("EG") == doctest::Approx(0.4));
    CHECK(widgets.number("EB") == doctest::Approx(0.6));
    CHECK_FALSE(widgets.boolean("CHANGED"));
    CHECK_FALSE(widgets.boolean("ECHANGED"));
    CHECK_FALSE(widgets.boolean("PRESSED"));
    CHECK(widgets.number("X1") - widgets.number("X0") == doctest::Approx(4096));
    CHECK(widgets.number("Y1") > widgets.number("Y0"));
    CHECK(GImGui->ColorEditOptions == previous);
}
