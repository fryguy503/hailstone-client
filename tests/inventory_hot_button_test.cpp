// An inventory slot onto a hot bar (game/hot_button_menu.h inventory_hot_button, hot_button_item_slot): the left
// button held on a slot puts an item link or an inventory slot hotbutton on the cursor (CInvSlotWnd::
// HandleLButtonHeld, 0x6947f0), a click on a bar puts it there (0x552e00), and a link finds its item where it
// is now. The Lua side (hs.ClickSlot "leftheld", hs.BagSlotClick) against the mock server.
#include <doctest/doctest.h>

#include <array>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "game/hot_button_menu.h"
#include "game/hotbuttons.h"
#include "game/inventory.h"
#include "mock_server/harness.h"
#include "net/item_packets.h"

using namespace hailstone;
using namespace hailstone::test;

namespace {

net::Item item(std::uint32_t id, const char* name, std::int16_t slot, const char* serial, bool bag = false) {
    net::Item it;
    it.id = id;
    it.name = name;
    it.serial = serial;
    it.slot = net::ItemSlot::possessions(slot);
    if (bag) {
        it.item_class = 1;
        it.bag_slots = 8;
    }
    return it;
}

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

TEST_CASE("inventory hot button: what the held left button attaches") {
    using game::HotButtonType;
    const net::Item potion = item(13005, "Potion", 25, "0000000000001234");

    // An item without Shift or Ctrl: a link to it (no slot, its GUID and id, no label or icon of its own).
    auto link = game::inventory_hot_button(net::ItemSlot::possessions(25), &potion, false);
    REQUIRE(link);
    CHECK(link->type == HotButtonType::ItemLink);
    CHECK(link->slot == -1);
    CHECK(link->guid == "0000000000001234");
    CHECK(link->item_id == 13005);
    CHECK(link->label.empty());
    CHECK(link->icon_type == -1);
    // in a bag too
    auto in_bag = game::inventory_hot_button(net::ItemSlot::possessions(23, 4), &potion, false);
    REQUIRE(in_bag);
    CHECK(in_bag->type == HotButtonType::ItemLink);

    // Shift or Ctrl, or an empty slot: the slot itself, top-level slots only.
    auto slot = game::inventory_hot_button(net::ItemSlot::possessions(25), &potion, true);
    REQUIRE(slot);
    CHECK(slot->type == HotButtonType::InventorySlot);
    CHECK(slot->slot == 25);
    auto empty = game::inventory_hot_button(net::ItemSlot::possessions(13), nullptr, false);
    REQUIRE(empty);
    CHECK(empty->type == HotButtonType::InventorySlot);
    CHECK(empty->slot == 13);
    CHECK_FALSE(game::inventory_hot_button(net::ItemSlot::possessions(23, 4), &potion, true));
    CHECK_FALSE(game::inventory_hot_button(net::ItemSlot::possessions(23, 4), nullptr, false));

    // Not the possessions (a bank slot), not an invalid slot.
    CHECK_FALSE(game::inventory_hot_button(net::ItemSlot{net::kInvBank, 3, -1, -1, 0, 0}, &potion, false));
    CHECK_FALSE(game::inventory_hot_button(net::ItemSlot::possessions(-1), &potion, false));
}

TEST_CASE("inventory hot button: a link finds its item by serial, else by id") {
    game::Inventory inv;
    net::Item bag = item(1003, "Backpack", 23, "0000000000000010", true);
    net::BagItem inside;
    inside.index = 2;
    inside.item = item(13005, "Potion", 23, "0000000000000020");
    bag.contents.push_back(inside);
    std::vector<net::Item> all{bag, item(13005, "Potion", 30, "0000000000000030"),
                               item(13005, "Potion", net::kSlotCursor, "0000000000000040")};
    inv.load(all);

    game::HotButton link;
    link.type = game::HotButtonType::ItemLink;
    link.slot = -1;
    link.item_id = 13005;

    link.guid = "0000000000000030";
    auto at = game::hot_button_item_slot(inv, link);
    REQUIRE(at);
    CHECK(at->same(net::ItemSlot::possessions(30)));
    // the serial in a bag wins over an earlier slot holding the same item id
    link.guid = "0000000000000020";
    at = game::hot_button_item_slot(inv, link);
    REQUIRE(at);
    CHECK(at->same(net::ItemSlot::possessions(23, 2)));
    // a serial no item has (a new zone's serials): the first item with the id, the bag's contents after the bag
    link.guid = "0000000000009999";
    at = game::hot_button_item_slot(inv, link);
    REQUIRE(at);
    CHECK(at->same(net::ItemSlot::possessions(23, 2)));
    // never the cursor: only the cursor's item has this serial, so the id finds another
    link.guid = "0000000000000040";
    at = game::hot_button_item_slot(inv, link);
    REQUIRE(at);
    CHECK_FALSE(at->same(net::ItemSlot::possessions(net::kSlotCursor)));
    // nothing with the id
    link.guid = "0000000000000000";
    link.item_id = 1;
    CHECK_FALSE(game::hot_button_item_slot(inv, link));

    // An inventory slot button: its slot, item or not; not the cursor.
    game::HotButton slot;
    slot.type = game::HotButtonType::InventorySlot;
    slot.slot = 24;
    at = game::hot_button_item_slot(inv, slot);
    REQUIRE(at);
    CHECK(at->same(net::ItemSlot::possessions(24)));
    slot.slot = net::kSlotCursor;
    CHECK_FALSE(game::hot_button_item_slot(inv, slot));
    // other buttons have no item
    game::HotButton social;
    social.type = game::HotButtonType::Social;
    CHECK_FALSE(game::hot_button_item_slot(inv, social));
}

TEST_CASE("inventory hot button: held on a slot, dropped on a bar") {
    ImGuiContext* ctx = ImGui::CreateContext(); // the hs API reads the keyboard's modifiers
    MockConfig config = MockConfig::standard();
    config.characters[0].inventory.push_back(item(1003, "Backpack", 23, "0000000000000010", true));
    config.characters[0].inventory.push_back(item(13005, "Potion", 25, "0000000000000020"));
    Rig rig(std::move(config));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());

    game::HotButtonStore store;
    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
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

    // hs.HasSlot: a possessions slot, and a bag place only inside the bag that slot holds (empty places count).
    CHECK(run(L, "return hs.HasSlot(0)") == "true");
    CHECK(run(L, "return hs.HasSlot(33)") == "true");
    CHECK(run(L, "return hs.HasSlot(34)") == "false");
    CHECK(run(L, "return hs.HasSlot(-1)") == "false");
    CHECK(run(L, "return hs.HasSlot(23, 0)") == "true");
    CHECK(run(L, "return hs.HasSlot(23, 7)") == "true");
    CHECK(run(L, "return hs.HasSlot(23, 8)") == "false"); // the backpack has 8 places
    CHECK(run(L, "return hs.HasSlot(25, 0)") == "false"); // a potion is no bag
    CHECK(run(L, "return hs.HasSlot(24, 0)") == "false"); // nothing there
    CHECK(run(L, "return hs.HasSlot(13, 0)") == "false"); // a worn slot holds no open bag
    host.in_game = false;
    CHECK(run(L, "return hs.HasSlot(23)") == "false");
    host.in_game = true;

    // The mock's profile has coin on the cursor (a type 3 attachment: nothing attaches over it). Into the purse first.
    CHECK(run(L, "return hs.ClickSlot(25, nil, 'leftheld')") == "false");
    for (int t = 0; t < 4; ++t)
        if (conn.cursor_coins()[static_cast<std::size_t>(3 - t)]) conn.inventory_money_button(t, -1, nullptr);
    REQUIRE(conn.cursor_coins() == std::array<std::uint32_t, 4>{});

    // The potion held: a link on the cursor, the potion still in its slot.
    CHECK(run(L, "return hs.ClickSlot(25, nil, 'leftheld')") == "true");
    CHECK(run(L, "return hs.HoldingButton()") == "true");
    CHECK(conn.inventory().at(net::ItemSlot::possessions(25)) != nullptr);
    CHECK(conn.inventory().cursor() == nullptr);
    // only one on the cursor at a time
    CHECK(run(L, "return hs.ClickSlot(23, nil, 'leftheld')") == "false");
    CHECK(run(L, "return hs.ClickHotButton('HotButtonWnd', 'HB_Button4', 'left')") == "true");
    CHECK(pressed == -1); // put there, not pressed
    CHECK(run(L, "return hs.HoldingButton()") == "false");
    CHECK(store.current(0, 3).type == game::HotButtonType::ItemLink);
    CHECK(store.current(0, 3).guid == "0000000000000020");
    CHECK(store.current(0, 3).item_id == 13005);
    CHECK(run(L, "return (hs.HotButton('HotButtonWnd', 'HB_Button4'))") == "item");

    // Shift: the slot itself.
    ImGui::GetIO().KeyShift = true;
    CHECK(run(L, "return hs.ClickSlot(25, nil, 'leftheld')") == "true");
    CHECK(run(L, "return hs.ClickHotButton('HotButtonWnd', 'HB_Button5', 'left')") == "true");
    CHECK(store.current(0, 4).type == game::HotButtonType::InventorySlot);
    CHECK(store.current(0, 4).slot == 25);
    // Shift on a bag's slot: nothing
    CHECK(run(L, "return hs.ClickSlot(23, 0, 'leftheld')") == "false");
    ImGui::GetIO().KeyShift = false;

    // An item on the cursor: no hotbutton (CCursorAttachment::RemoveAttachment), and the click still picks up.
    CHECK(run(L, "return hs.ClickSlot(25, nil, 'left')") == "true");
    REQUIRE(conn.inventory().cursor() != nullptr);
    CHECK(run(L, "return hs.ClickSlot(23, nil, 'leftheld')") == "false");
    CHECK(run(L, "return hs.HoldingButton()") == "false");

    lua_close(L);
    ImGui::DestroyContext(ctx);
}

TEST_CASE("cursor attachment: a single stackable item has a quantity of 1, an item that does not stack has none") {
    ImGuiContext* ctx = ImGui::CreateContext();
    MockConfig config = MockConfig::standard();
    net::Item arrow = item(8005, "Arrow", 25, "0000000000000030");
    arrow.max_stack = 100;
    arrow.stack_count = 1;
    config.characters[0].inventory.push_back(arrow);
    config.characters[0].inventory.push_back(item(5019, "Sword", 26, "0000000000000031"));
    Rig rig(std::move(config));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());

    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::UiApi api(host);
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "hs");

    // (the mock's profile has coin on the cursor: into the purse first)
    for (int t = 0; t < 4; ++t)
        if (conn.cursor_coins()[static_cast<std::size_t>(3 - t)]) conn.inventory_money_button(t, -1, nullptr);
    REQUIRE(conn.cursor_coins() == std::array<std::uint32_t, 4>{});

    CHECK(run(L, "return hs.ClickSlot(25, nil, 'left')") == "true");
    REQUIRE(conn.inventory().cursor() != nullptr);
    CHECK(run(L, "return hs.CursorAttachment().quantity") == "1");
    CHECK(run(L, "return hs.ClickSlot(25, nil, 'left')") == "true"); // put back
    REQUIRE(conn.inventory().cursor() == nullptr);

    CHECK(run(L, "return hs.ClickSlot(26, nil, 'left')") == "true");
    REQUIRE(conn.inventory().cursor() != nullptr);
    CHECK(run(L, "return hs.CursorAttachment().quantity") == "nil");

    lua_close(L);
    ImGui::DestroyContext(ctx);
}

TEST_CASE("cursor attachment: coin on the cursor is reported with its type and count") {
    ImGuiContext* ctx = ImGui::CreateContext();
    Rig rig(MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());

    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::UiApi api(host);
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "hs");

    for (int t = 0; t < 4; ++t)
        if (conn.cursor_coins()[static_cast<std::size_t>(3 - t)]) conn.inventory_money_button(t, -1, nullptr);
    REQUIRE(conn.cursor_coins() == std::array<std::uint32_t, 4>{});
    CHECK(run(L, "return hs.CursorAttachment()") == "nil");

    // 5 of the first coin the purse has (0 platinum .. 3 copper) onto the cursor
    int taken = -1;
    for (int t = 0; t < 4 && taken < 0; ++t) {
        conn.inventory_money_button(t, 5, nullptr);
        if (conn.cursor_coins()[static_cast<std::size_t>(3 - t)] == 5) taken = t;
    }
    REQUIRE(taken >= 0);
    CHECK(run(L, "return hs.CursorAttachment().coin") == std::to_string(taken));
    CHECK(run(L, "return hs.CursorAttachment().quantity") == "5");
    CHECK(run(L, "return hs.CursorAttachment().icon") == "nil");
    CHECK(run(L, "return hs.CursorAttachment().iconRect ~= nil") == "true");

    // back into the purse: nothing on the cursor
    conn.inventory_money_button(taken, -1, nullptr);
    CHECK(run(L, "return hs.CursorAttachment()") == "nil");

    lua_close(L);
    ImGui::DestroyContext(ctx);
}
