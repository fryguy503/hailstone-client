// The inventory and bank windows' Auto-Inventory button (Hailstone_AutoInventory, app/lua_ui_items.cpp; the modern
// pack's inventory.lua and bank.lua draw it) over the hs API, against the mock server.
#include <doctest/doctest.h>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "mock_server/harness.h"
#include "net/item_packets.h"
#include "net/packet_io.h"

using namespace hailstone;
using namespace hailstone::test;

TEST_CASE("Auto-Inventory button: enabled with an item on the cursor; a click puts it in the inventory") {
    ImGuiContext* ctx = ImGui::CreateContext(); // the hs API reads the keyboard's modifiers
    net::Item held;
    held.id = 13005;
    held.name = "Iron Ration";
    held.slot = net::ItemSlot::possessions(net::kSlotCursor);
    MockConfig config = MockConfig::standard();
    config.characters[0].inventory.push_back(held);
    Rig rig(std::move(config));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    REQUIRE(conn.inventory().cursor());

    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::UiApi api(host);
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "hs");
    auto enabled = [&] {
        REQUIRE(luaL_dostring(L, "return (hs.ChildState('InventoryWindow', 'Hailstone_AutoInventory'))") == LUA_OK);
        bool on = lua_toboolean(L, -1);
        lua_settop(L, 0);
        return on;
    };
    CHECK(enabled());
    REQUIRE(luaL_dostring(L, "return hs.ClickChild('InventoryWindow', 'Hailstone_AutoInventory', 'left')") == LUA_OK);
    CHECK(lua_toboolean(L, -1));
    lua_settop(L, 0);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::MoveItem) >= 1; }));
    CHECK_FALSE(conn.inventory().cursor());
    const net::AppPacket* move = nullptr;
    for (const auto& p : rig.server.zone_log)
        if (net::is_op(p, net::Op::MoveItem)) move = &p;
    REQUIRE(move);
    auto mv = net::decode_move_item(*move);
    REQUIRE(mv);
    CHECK(mv->from.slot == net::kSlotCursor);
    CHECK(mv->to.slot >= net::kSlotGeneral1);
    // Nothing on the cursor: the button is off.
    CHECK_FALSE(enabled());
    lua_close(L);
    ImGui::DestroyContext(ctx);
}
