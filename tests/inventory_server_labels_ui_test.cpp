// The modern inventory's Mitigation and Avoidance rows (label types 6668 and 6667, ui/modern/inventory.lua): the stock
// client has no such types (hs.Label gives "Unknown\tLabel<n>"), only a server's client fills them in (the NMS
// plugin), so on a stock server the rows are left out instead of drawing that text over AC and ATK.
#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "app/lua_ui.h"
#include "mock_server/harness.h"
#include "ui/lua/lua_host.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::test;
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

// Every text the window drew last frame, joined by '|'.
std::string drawn(lua_State* L) {
    REQUIRE(luaL_dostring(L, "return table.concat(test_seen, '|')") == LUA_OK);
    std::string s = lua_tostring(L, -1);
    lua_settop(L, 0);
    return s;
}

// How many times `word` was drawn as a whole text.
int count(const std::string& seen, const std::string& word) {
    std::string all = "|" + seen + "|", key = "|" + word + "|";
    int n = 0;
    for (auto at = all.find(key); at != std::string::npos; at = all.find(key, at + 1)) ++n;
    return n;
}

} // namespace

TEST_CASE("Inventory (modern): Mitigation and Avoidance show only when the server's client gives them") {
    fs::path src = ui_dir() / "modern";
    REQUIRE(fs::exists(src / "inventory.lua"));
    ScratchDir scratch("inventory_server_labels_ui_test");
    fs::path dir = scratch.path();
    fs::copy(src, dir / "modern", fs::copy_options::recursive);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Rig rig;
        REQUIRE(rig.to_zone());
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &rig.game;
        host.conn = &rig.conn;
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
        REQUIRE(luaL_dostring(L, R"(
            test_seen = {}
            local dt = ui.DrawText
            ui.DrawText = function(...)
              for _, v in ipairs({ ... }) do if type(v) == "string" then test_seen[#test_seen + 1] = v end end
              return dt(...)
            end)") == LUA_OK);
        api.show("InventoryWindow", true);
        auto frames = [&] {
            for (int i = 0; i < 3; ++i) {
                REQUIRE(luaL_dostring(L, "test_seen = {}") == LUA_OK);
                api.begin_frame();
                frame(lua);
                rig.step();
            }
        };

        // The stock server: the client has no text for the two types, and neither row is drawn on either page.
        REQUIRE(luaL_dostring(L, "return hs.Label(6668)") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1)).starts_with("Unknown"));
        lua_settop(L, 0);
        // (The Stats page has an item-stat "Avoidance" row of its own, type 221.)
        std::map<std::string, int> stock_mit, stock_avoid;
        for (const char* page : {"character", "stats"}) {
            CAPTURE(std::string(page));
            REQUIRE(luaL_dostring(L, (std::string("ui.SetSetting('test.inv.page', '") + page + "')").c_str()) == LUA_OK);
            frames();
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            std::string s = drawn(L);
            CHECK(s.find("Unknown") == std::string::npos);
            stock_mit[page] = count(s, "Mitigation");
            stock_avoid[page] = count(s, "Avoidance");
            CHECK(stock_mit[page] == 0);
            if (std::string(page) == "character") CHECK(stock_avoid[page] == 0);
            CHECK(s.find(std::string(page) == "stats" ? "Attack" : "ATK") != std::string::npos); // the page was drawn
        }

        // A server's client that fills them in (as the NMS plugin's labels do): both rows with their values.
        REQUIRE(luaL_dostring(L, R"(
            local label = hs.Label
            hs.Label = function(t)
              if t == 6668 then return "321" elseif t == 6667 then return "123" end
              return label(t)
            end)") == LUA_OK);
        hailstone::test::sync_ui_api_aliases(L);
        for (const char* page : {"character", "stats"}) {
            CAPTURE(std::string(page));
            REQUIRE(luaL_dostring(L, (std::string("ui.SetSetting('test.inv.page', '") + page + "')").c_str()) == LUA_OK);
            frames();
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            std::string s = drawn(L);
            // (A text may be drawn twice: its shadow, then itself.)
            CHECK(count(s, "Mitigation") > stock_mit[page]);
            CHECK(count(s, "Avoidance") > stock_avoid[page]);
            CHECK(count(s, "321") > 0);
            CHECK(count(s, "123") > 0);
        }
    }
    ImGui::DestroyContext(ctx);
}
