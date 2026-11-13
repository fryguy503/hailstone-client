// Exercise the shipped AA window with real ImGui input. Text and hit rectangles are observed at the Lua bridge,
// so these checks cover the headers, scrolled rows and a script's reserved row area together.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "scratch_dir.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"

namespace {
namespace fs = std::filesystem;
using namespace hailstone;

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    ImVec2 center() const { return ImVec2((x0 + x1) / 2, (y0 + y1) / 2); }
};

struct AaSession {
    GameDir game;
    net::GameConnection conn;
    ui::ChatView chat;
    app::EqHost host;
    app::EqApi api;
    ui::lua::LuaUi lua;

    explicit AaSession(const fs::path& dir)
        : game(dir / "game"), conn(game), api(host), lua(dir / "modern", dir / "ui.txt") {
        host.game = &game;
        host.conn = &conn;
        host.chat = &chat;
        host.in_game = host.ui_ready = true;
        lua.add_eq_api([this](lua_State* L) {
            api.register_functions(L);
            lua_pushcfunction(L, [](lua_State* state) { lua_pushboolean(state, true); return 1; });
            lua_setfield(L, -2, "InGame");
        });
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        lua.set_folders({{"plug", dir / "plug"}});
    }
};

struct AaColumns {
    test::ScratchDir scratch{"aa_columns_ui"};
    ImGuiContext* context = nullptr;
    std::unique_ptr<AaSession> session;

    AaColumns() {
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::create_directories(dir / "plug");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern", dir / "modern",
                 fs::copy_options::recursive);
        std::ofstream(dir / "plug" / "aa_columns.lua") << R"lua(
local aa = dofile("lib/aa.lua")
ui.window("Alternate Advancement", function()
  aa.window({ row = { width = 30, draw = function(_, x0, y0, x1, y1)
    ui.DrawText(x0, y0, 1, 1, 1, 1, "SCRIPT PART")
  end } })
end)
)lua";
        context = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        session = std::make_unique<AaSession>(dir);
        lua_pushcfunction(state(), [](lua_State* L) {
            const ImVec4 clip = ImGui::GetWindowDrawList()->_ClipRectStack.back();
            lua_pushnumber(L, clip.x); lua_pushnumber(L, clip.y);
            lua_pushnumber(L, clip.z); lua_pushnumber(L, clip.w);
            return 4;
        });
        lua_setglobal(state(), "aa_test_clip");
        lua_pushcfunction(state(), [](lua_State* L) {
            const auto lo = ImGui::GetItemRectMin();
            const auto hi = ImGui::GetItemRectMax();
            lua_pushnumber(L, lo.x); lua_pushnumber(L, lo.y);
            lua_pushnumber(L, hi.x); lua_pushnumber(L, hi.y);
            return 4;
        });
        lua_setglobal(state(), "aa_test_item_rect");
        run(R"lua(
aa_text, aa_hits = {}, {}
local DrawText, InvisibleButton, EdgeButton, Button = ui.DrawText, ui.InvisibleButton, ui.EdgeButton, ui.Button
ui.DrawText = function(x, y, r, g, b, a, text, ...)
  local old = aa_text[text]
  -- Keep the first row's text, replacing its shadow with the foreground draw.
  if not old or (math.abs(old[1] - x) <= 1.1 and math.abs(old[2] - y) <= 1.1) then
    local cx0, cy0, cx1, cy1 = aa_test_clip()
    aa_text[text] = {x, y, ui.CalcTextSize(text), cx0, cy0, cx1, cy1}
  end
  return DrawText(x, y, r, g, b, a, text, ...)
end
ui.InvisibleButton = function(id, w, h, ...)
  local x, y = ui.GetCursorScreenPos()
  aa_hits[id] = {x, y, x + w, y + h}
  return InvisibleButton(id, w, h, ...)
end
ui.EdgeButton = function(id, x0, y0, x1, y1)
  aa_hits[id] = {x0, y0, x1, y1}
  return EdgeButton(id, x0, y0, x1, y1)
end
ui.Button = function(id, ...)
  local result = Button(id, ...)
  aa_hits[id] = {aa_test_item_rect()}
  local clip = {aa_test_clip()}
  for i = 1, 4 do aa_hits[id][i + 4] = clip[i] end
  return result
end
aa_rows = {}
for i = 1, 80 do
  aa_rows[i] = {string.format("Ability %03d", i), "0/123", "7", "Combat", "3", "Shadow", "1", "1"}
end
local WindowList, ListInfo, ListRow = eq.WindowList, eq.ListInfo, eq.ListRow
eq.WindowList = function(win, id, ...)
  if win == "AAWindow" and id == "ModernList" then return aa_rows end
  return WindowList(win, id, ...)
end
eq.ListInfo = function(win, id, ...)
  if win == "AAWindow" and id == "ModernList" then return #aa_rows, 8, -1 end
  return ListInfo(win, id, ...)
end
eq.ListRow = function(win, id, row, ...)
  if win == "AAWindow" and id == "ModernList" then return aa_rows[row + 1], 1, 1, 1 end
  return ListRow(win, id, row, ...)
end
ui.SetSetting('plugin.plug.hud.size.AAWindow', '1400,650')
ui.SetSetting('plugin.plug.test.pos.AAWindow', '20,20')
eq.ShowWindow('AAWindow', true)
)lua");
    }

    ~AaColumns() {
        session.reset();
        ImGui::DestroyContext(context);
    }

    lua_State* state() { return session->lua.state(); }

    void run(const std::string& code) {
        const int result = luaL_dostring(state(), code.c_str());
        INFO(code);
        INFO((result == LUA_OK ? "" : lua_tostring(state(), -1)));
        REQUIRE(result == LUA_OK);
        lua_settop(state(), 0);
    }

    void frames(int count = 1) {
        for (int i = 0; i < count; ++i) {
            run("aa_text, aa_hits = {}, {}");
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(3600, 2000);
            io.DeltaTime = 1.0f / 60.0f;
            session->api.begin_frame();
            ImGui::NewFrame();
            session->lua.frame();
            ImGui::Render();
            REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
        }
    }

    double value(const char* table, const char* key, int index) {
        lua_getglobal(state(), table);
        lua_getfield(state(), -1, key);
        INFO(table << "[" << key << "]");
        REQUIRE(lua_istable(state(), -1));
        lua_rawgeti(state(), -1, index);
        const double result = lua_tonumber(state(), -1);
        lua_settop(state(), 0);
        return result;
    }

    Rect hit(const char* id) {
        return {static_cast<float>(value("aa_hits", id, 1)), static_cast<float>(value("aa_hits", id, 2)),
                static_cast<float>(value("aa_hits", id, 3)), static_cast<float>(value("aa_hits", id, 4))};
    }

    std::string setting(const char* key) {
        const std::string code = std::string("return ui.GetSetting('plugin.plug.") + key + "') or ''";
        REQUIRE(luaL_dostring(state(), code.c_str()) == LUA_OK);
        std::string out = lua_tostring(state(), -1);
        lua_settop(state(), 0);
        return out;
    }

    void click(ImVec2 at) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y);
        frames();
        io.AddMouseButtonEvent(0, true);
        frames();
        io.AddMouseButtonEvent(0, false);
        frames(2);
    }

    void drag(ImVec2 from, ImVec2 to) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(from.x, from.y);
        frames();
        io.AddMouseButtonEvent(0, true);
        frames();
        for (int i = 1; i <= 5; ++i) {
            const float amount = static_cast<float>(i) / 5;
            io.AddMousePosEvent(from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount);
            frames();
            checkAligned();
        }
        io.AddMouseButtonEvent(0, false);
        frames();
        checkAligned();
        frames(2);
    }

    void checkAligned() {
        // Left aligned columns give a direct check of the same layout in the header and rows.
        CHECK(value("aa_text", "CATEGORY", 1) == doctest::Approx(value("aa_text", "Combat", 1)).epsilon(0.002));
        CHECK(value("aa_text", "ERA", 1) == doctest::Approx(value("aa_text", "Shadow", 1)).epsilon(0.002));
        CHECK(value("aa_text", "TYPE", 1) == doctest::Approx(value("aa_text", "Active", 1)).epsilon(0.002));
    }

    void checkClipped() {
        for (const auto& [head, cell] : {std::pair{"CATEGORY", "Combat"}, std::pair{"ERA", "Shadow"},
                                       std::pair{"TYPE", "Active"}, std::pair{"RANK", "0/123"}}) {
            INFO(head);
            const auto left = value("aa_text", head, 4), right = value("aa_text", head, 6);
            CHECK(right > left);
            CHECK(value("aa_text", cell, 4) == doctest::Approx(left));
            CHECK(value("aa_text", cell, 6) == doctest::Approx(right));
            CHECK(right <= value("aa_text", "SCRIPT PART", 1));
        }
    }

    void checkFooter() {
        const auto button = hit("Reset columns##aareset");
        CHECK(button.x0 >= value("aa_hits", "Reset columns##aareset", 5));
        CHECK(button.y0 >= value("aa_hits", "Reset columns##aareset", 6));
        CHECK(button.x1 <= value("aa_hits", "Reset columns##aareset", 7));
        CHECK(button.y1 <= value("aa_hits", "Reset columns##aareset", 8));
    }

    void reload() {
        CHECK(session->lua.run_for_test("plug", "dofile('aa_columns.lua') return 'loaded'") == "loaded");
        frames(2);
    }
};
} // namespace

TEST_CASE("AA columns: dialog scale, resizing and a row extension share the header layout") {
    AaColumns ui;
    for (const char* scale : {"0.6", "1.5", "2.0"}) {
        INFO(scale);
        ui.run(std::string("ui.SetSetting('plugin.plug.hud.scale.AAWindow', '") + scale + "')");
        ui.frames(3);
        ui.checkAligned();
        ui.checkClipped();
        ui.checkFooter();
        ui.run("ui.SetSetting('plugin.plug.hud.size.AAWindow', '1250,650')");
        ui.frames();
        ui.checkAligned();
        ui.run("ui.SetSetting('plugin.plug.hud.size.AAWindow', '1400,650')");
        ui.frames();
        ui.checkAligned();
    }
    ui.run("aa_all_rows = aa_rows aa_rows = {aa_rows[1], aa_rows[2]}");
    ui.frames();
    ui.checkAligned();
    ui.frames();
    ui.checkAligned();
    ui.run("aa_rows = aa_all_rows");
    ui.frames();
    ui.checkAligned();
    ui.frames();
    ui.checkAligned();
}

TEST_CASE("AA columns: drag resizes, a header reorders without sorting, and Reset columns restores defaults") {
    AaColumns ui;
    ui.run("ui.SetSetting('plugin.plug.hud.scale.AAWindow', '1.5')");
    ui.frames(3);
    const auto initial = ui.hit("##aaheadcategory");
    const auto edge = ui.hit("##aaresizecategory").center();
    ui.drag(edge, ImVec2(edge.x + 24, edge.y));
    const auto resized = ui.hit("##aaheadcategory");
    CHECK(resized.x1 - resized.x0 > initial.x1 - initial.x0 + 18);
    CHECK(ui.setting("aa.widths").find("category=") != std::string::npos);
    CHECK(ui.setting("aa.sort").empty());
    CHECK(ui.setting("aa.columns").empty());
    CHECK(ui.hit("##aaresizecategory").center().x == doctest::Approx(edge.x + 24));
    ui.checkAligned();
    ui.checkClipped();

    const auto saved = ui.setting("aa.widths");
    ui.reload();
    CHECK(ui.setting("aa.widths") == saved);
    CHECK(ui.hit("##aaheadcategory").x1 - ui.hit("##aaheadcategory").x0 ==
          doctest::Approx(resized.x1 - resized.x0));

    ui.click(ui.hit("##aaheadcost").center());
    CHECK(ui.setting("aa.sort") == "cost:asc");
    const auto from = ui.hit("##aaheadcost").center();
    const auto first = ui.hit("##aaheadname");
    ui.drag(from, ImVec2(first.x0 + 2, from.y));
    CHECK(ui.setting("aa.columns").starts_with("cost,name,"));
    CHECK(ui.setting("aa.sort") == "cost:asc");
    CHECK(ui.hit("##aaheadcost").x0 < ui.hit("##aaheadname").x0);
    ui.checkAligned();
    ui.checkClipped();

    ui.click(ui.hit("Reset columns##aareset").center());
    CHECK(ui.setting("aa.widths").empty());
    CHECK(ui.setting("aa.columns").empty());
    CHECK(ui.setting("aa.sort").empty());
    CHECK(ui.hit("##aaheadname").x0 < ui.hit("##aaheadcategory").x0);
    const auto reset = ui.hit("##aaheadcategory");
    CHECK(reset.x1 - reset.x0 == doctest::Approx(initial.x1 - initial.x0));
    ui.checkAligned();
    ui.checkClipped();
}

TEST_CASE("AA columns: legacy order and invalid widths remain bounded in narrow windows") {
    AaColumns ui;
    ui.run(R"lua(
ui.SetSetting('plugin.plug.aa.columns', 'cost,cost,unknown,name,kind,era,rank,tab,category')
ui.SetSetting('plugin.plug.aa.widths', 'name=nan,category=200000,tab=-99,era=inf,rank=0,cost=1e309,unknown=5')
)lua");
    ui.frames(3);
    CHECK(ui.hit("##aaheadcost").x0 < ui.hit("##aaheadname").x0);
    for (const char* width : {"420", "600"}) {
        INFO(width);
        ui.run(std::string("ui.SetSetting('plugin.plug.hud.size.AAWindow', '") + width + ",650')");
        ui.frames(3);
        ui.checkFooter();
        ui.run(R"lua(
local count = 0
for id, rect in pairs(aa_hits) do
  if id:find('##aahead', 1, true) == 1 then
    count = count + 1
    for i = 1, 4 do assert(rect[i] == rect[i] and math.abs(rect[i]) < math.huge) end
    assert(rect[3] > rect[1] and rect[3] <= aa_text['SCRIPT PART'][1])
  end
end
assert(count >= 4)
)lua");
    }
}
