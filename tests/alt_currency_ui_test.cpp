// The inventory's Alt. Currency page over the eq API (app/lua_eq_items.cpp InventoryWindow, app/lua_eq.cpp
// AltCurrencies): a UI pack that draws the page itself selects a currency with eq.ListClick and claims or reclaims
// it through the stock buttons, without the stock list ever being drawn.
#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "app/lua_eq_windows.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "net/merchant_packets.h"
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

TEST_CASE("Alt. currency page: eq.ListClick selects by eq.AltCurrencies' order, the list never drawn") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("alt_currency_ui");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        app::EqHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::EqApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "eq");

        // 31 held, 32 none of, 33 held: without Display Missing the rows are 31, 33
        std::vector<net::AltCurrencyEntry> entries{
            {31, 22000, 1500, 1000, true}, {32, 22001, 1501, 500, true}, {33, 22002, 1502, 20, true}};
        conn.receive_zone_packet_for_test(net::encode_alt_currency_populate(entries));
        conn.receive_zone_packet_for_test(net::encode_alt_currency_update({31, 7}));
        conn.receive_zone_packet_for_test(net::encode_alt_currency_update({33, 3}));
        CHECK(run(L, "return #eq.AltCurrencies()") == "2");

        const std::string inv = "'InventoryWindow'", list = "'AltCurr_PointList'";
        CHECK(run(L, "return (eq.ChildState(" + inv + ", 'AltCurr_CreateItemButton'))") == "false"); // none picked
        CHECK(run(L, "return eq.ListClick(" + inv + ", " + list + ", 1, 'left')") == "true");
        CHECK(run(L, "return (select(3, eq.ListInfo(" + inv + ", " + list + ")))") == "1");
        CHECK(run(L, "return (eq.WindowText(" + inv + ", 'AltCurr_MaxStackLabel'))") == "Max stack: 20"); // 33
        CHECK(run(L, "return (eq.ChildState(" + inv + ", 'AltCurr_CreateItemButton'))") == "true");
        CHECK(run(L, "return (eq.ChildState(" + inv + ", 'AltCurr_ReclaimButton'))") == "false"); // nothing held

        // Display Missing: row 1 is now 32, as eq.AltCurrencies(true) lists it
        CHECK(run(L, "return eq.ClickChild(" + inv + ", 'AltCurr_DisplayMissingButton', 'left')") == "true");
        CHECK(run(L, "return (select(2, eq.ChildState(" + inv + ", 'AltCurr_DisplayMissingButton')))") == "true");
        CHECK(run(L, "return eq.AltCurrencies(true)[2].maxStack") == "500");
        CHECK(run(L, "return eq.ListClick(" + inv + ", " + list + ", 1, 'left')") == "true");
        CHECK(run(L, "return (eq.WindowText(" + inv + ", 'AltCurr_MaxStackLabel'))") == "Max stack: 500");
        CHECK(run(L, "return (eq.ChildState(" + inv + ", 'AltCurr_CreateItemButton'))") == "false"); // none of it
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

// A currency's name is dbstr type 0x12 of its id (CInventoryWnd's Alt. Currency list), in eq.AltCurrencies and in
// the stock list's rows; a currency the game folder has no name for shows its number. Type 0x13, which the real
// estate cost text read before, is the AA categories.
TEST_CASE("Alt. currency page: names come from dbstr type 18, the number when there is none") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("alt_currency_names");
        {
            std::ofstream out(scratch.path() / "dbstr_us.txt");
            out << "31^17^Doubloon^0^\n31^18^Doubloons^0^\n33^19^Quest Only^0^\n";
        }
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        app::EqHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::EqApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "eq");

        std::vector<net::AltCurrencyEntry> entries{{31, 22000, 1500, 1000, true}, {33, 22002, 1502, 20, true}};
        conn.receive_zone_packet_for_test(net::encode_alt_currency_populate(entries));
        conn.receive_zone_packet_for_test(net::encode_alt_currency_update({31, 7}));
        conn.receive_zone_packet_for_test(net::encode_alt_currency_update({33, 3}));
        CHECK(run(L, "return eq.AltCurrencies()[1].name") == "Doubloons");
        CHECK(run(L, "return eq.AltCurrencies()[2].name") == "Currency 33");
        const std::string at = "'InventoryWindow', 'AltCurr_PointList'";
        CHECK(run(L, "return (eq.ListRow(" + at + ", 0))[2]") == "Doubloons");
        CHECK(run(L, "return (eq.ListRow(" + at + ", 1))[2]") == "Currency 33");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}
