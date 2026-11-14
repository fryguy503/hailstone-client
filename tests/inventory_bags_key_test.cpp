// The OPEN_INV_BAGS / CLOSE_INV_BAGS / TOGGLE_INV_BAGS keys (Shift+B toggles by default) and CMD_TOGGLE_BANK_BAGS on
// the Lua bag windows (EqApi::open_all_bags and the rest, app/lua_eq_items.cpp), against the mock server.
#include <doctest/doctest.h>

#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "mock_server/harness.h"
#include "net/item_packets.h"

using namespace hailstone;
using namespace hailstone::test;

namespace {

net::Item item(std::uint32_t id, const char* name, std::int16_t slot, bool bag, std::int16_t type = net::kInvPossessions) {
    net::Item it;
    it.id = id;
    it.name = name;
    it.slot = {type, slot, -1, -1, 0, 0};
    if (bag) {
        it.item_class = 1;
        it.bag_slots = 8;
    }
    return it;
}

} // namespace

TEST_CASE("inventory bags keys: every bag in the general slots opens, closes and toggles") {
    ImGuiContext* ctx = ImGui::CreateContext(); // the eq API reads the keyboard's modifiers
    MockConfig config = MockConfig::standard();
    config.characters[0].inventory.push_back(item(1003, "Backpack", 23, true));
    config.characters[0].inventory.push_back(item(13005, "Iron Ration", 24, false));
    config.characters[0].inventory.push_back(item(1004, "Large Sack", 27, true));
    Rig rig(std::move(config));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());

    app::EqHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::EqApi api(host);
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "eq");
    // The bag windows' titles, Bag1 .. Bag3 ("" for a closed one).
    auto open = [&] {
        std::string out;
        for (int n = 1; n <= 3; ++n) {
            std::string chunk = "return (eq.BagInfo('Bag" + std::to_string(n) + "'))";
            REQUIRE(luaL_dostring(L, chunk.c_str()) == LUA_OK);
            const char* name = lua_tostring(L, -1);
            out += std::string(name ? name : "") + "|";
            lua_settop(L, 0);
        }
        return out;
    };
    REQUIRE(open() == "|||");

    api.toggle_all_bags(); // the first press opens (CInvSlotMgr's constructor sets +0x2010)
    CHECK(open() == "Backpack|Large Sack||");
    api.toggle_all_bags();
    CHECK(open() == "|||");

    // One bag opened by hand: the toggle opens the other beside it, and the next press closes both.
    api.right_click_item(net::ItemSlot::possessions(27));
    CHECK(open() == "Large Sack|||");
    api.toggle_all_bags();
    CHECK(open() == "Large Sack|Backpack||");
    api.toggle_all_bags();
    CHECK(open() == "|||");

    // Open and close act whatever the toggle's state; opening twice opens nothing more.
    api.open_all_bags();
    api.open_all_bags();
    CHECK(open() == "Backpack|Large Sack||");
    api.close_all_bags();
    CHECK(open() == "|||");
    api.close_all_bags();
    api.toggle_all_bags(); // after a close the toggle opens
    CHECK(open() == "Backpack|Large Sack||");
    api.close_all_bags();

    lua_close(L);
    ImGui::DestroyContext(ctx);
}

TEST_CASE("bank bags key: the bank's and the shared bank's bags open while the bank shows, and close") {
    ImGuiContext* ctx = ImGui::CreateContext();
    MockConfig config = MockConfig::standard();
    config.characters[0].inventory.push_back(item(1005, "Bank Sack", 2, true, net::kInvBank));
    config.characters[0].inventory.push_back(item(13005, "Iron Ration", 3, false, net::kInvBank));
    config.characters[0].inventory.push_back(item(1006, "Shared Box", 1, true, net::kInvSharedBank));
    config.characters[0].inventory.push_back(item(1007, "Pouch", 25, true));
    Rig rig(std::move(config));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    REQUIRE(rig.until([&] { return conn.inventory().at({net::kInvSharedBank, 1, -1, -1, 0, 0}) != nullptr; }));

    app::EqHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::EqApi api(host);
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "eq");
    auto open = [&] {
        std::string out;
        for (int n = 1; n <= 3; ++n) {
            std::string chunk = "return (eq.BagInfo('Bag" + std::to_string(n) + "'))";
            REQUIRE(luaL_dostring(L, chunk.c_str()) == LUA_OK);
            const char* name = lua_tostring(L, -1);
            out += std::string(name ? name : "") + "|";
            lua_settop(L, 0);
        }
        return out;
    };
    REQUIRE(open() == "|||");

    // Without the bank window nothing opens (OpenAllBankBags' IsReallyVisible), and the toggle stays on open.
    api.toggle_all_bank_bags();
    CHECK(open() == "|||");
    rig.server.add_spawn(make_spawn("Banker", 60, 1, 1, 5.0f, 5.0f, 3.0f));
    REQUIRE(rig.until([&] { return conn.world().spawns.count(60) == 1; }));
    REQUIRE(conn.activate_bank(60));
    api.toggle_all_bank_bags();
    CHECK(open() == "Bank Sack|Shared Box||");
    // The inventory's bags are not the bank key's: the next press closes the bank's alone.
    api.right_click_item(net::ItemSlot::possessions(25));
    CHECK(open() == "Bank Sack|Shared Box|Pouch|");
    api.toggle_all_bank_bags();
    CHECK(open() == "Pouch|||");
    api.toggle_all_bank_bags();
    CHECK(open() == "Pouch|Bank Sack|Shared Box|");
    api.toggle_all_bank_bags();
    api.right_click_item(net::ItemSlot::possessions(25));
    CHECK(open() == "|||");

    // Closing the bank closes every container, carried bags too, and sets both keys back to open
    // (CBankWnd::OnShow @0x627760: CContainerMgr::CloseAllContainers, 0x697e40). Reopening the same banker
    // before the next UI frame must still close the old bags.
    api.toggle_all_bags();
    api.toggle_all_bank_bags();
    CHECK(open() == "Pouch|Bank Sack|Shared Box|");
    conn.close_bank();
    REQUIRE(conn.activate_bank(60));
    CHECK(open() == "|||");
    api.toggle_all_bank_bags();
    CHECK(open() == "Bank Sack|Shared Box||");
    api.right_click_item(net::ItemSlot::possessions(25));
    CHECK(open() == "Bank Sack|Shared Box|Pouch|");
    conn.close_bank();
    CHECK(open() == "|||");
    api.toggle_all_bags(); // the inventory key opens again: its flag was set
    CHECK(open() == "Pouch|||");

    lua_close(L);
    ImGui::DestroyContext(ctx);
}
