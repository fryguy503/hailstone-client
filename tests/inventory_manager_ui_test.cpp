// The Advanced Inventory (ui/modern/inventorymanager.lua over app/lua_eq_inventory_manager.cpp) and the Keep marks
// (game/kept_items.h), against the mock server: the window draws in both views without script errors and lists what
// the character wears and carries; a kept item is not destroyed or dropped; the marks are kept per character; the
// bulk sell, destroy and tribute of chosen slots (GameConnection::start_bulk_job); /finditem and /advi.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "app/lua_eq_menu.h"
#include "mock_server/harness.h"
#include "net/merchant_packets.h"
#include "net/tribute_packets.h"
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

std::uint32_t id_at(const net::GameConnection& c, int slot, int sub = -1) {
    const net::Item* it =
        c.inventory().at(net::ItemSlot::possessions(static_cast<std::int16_t>(slot), static_cast<std::int16_t>(sub)));
    return it ? it->id : 0u;
}

net::ItemSlot at(int slot, int sub = -1) {
    return net::ItemSlot::possessions(static_cast<std::int16_t>(slot), static_cast<std::int16_t>(sub));
}

net::Item bag_item(std::uint32_t id, const std::string& name, std::int16_t slot) {
    net::Item b = make_item(id, name, slot);
    b.item_class = 1;
    b.bag_slots = 4;
    return b;
}

// The mock's character zones in holding coin (profile_builder.cpp); the bulk jobs, as Sort, want an empty cursor.
void put_coin_away(net::GameConnection& c) {
    for (int i = 0; i < 4; ++i) c.drop_on_doll(); // one coin a click, platinum first
    for (std::uint32_t n : c.cursor_coins()) REQUIRE(n == 0u);
}

std::string file_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("Advanced Inventory: lists what is worn and carried, draws in both views, and marks items") {
    fs::path src = ui_dir() / "modern";
    REQUIRE(fs::exists(src / "inventorymanager.lua"));
    ScratchDir scratch("inventory_manager_ui_test");
    fs::path dir = scratch.path();
    fs::copy(src, dir / "modern", fs::copy_options::recursive);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        net::ConnectionOptions options = quick_options();
        options.servers_dir = dir / "servers";
        Rig rig(MockConfig::standard(), options);
        REQUIRE(rig.to_zone());
        auto& conn = rig.conn;
        put_coin_away(conn);

        app::EqHost host;
        host.in_game = host.ui_ready = true;
        host.game = &rig.game;
        host.conn = &conn;
        app::EqApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        ui::lua::LuaUi lua(dir / "modern", dir / "modern.txt");
        lua.add_eq_api([&](lua_State* L) {
            api.register_functions(L);
            lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
            lua_setfield(L, -2, "InGame");
        });
        lua_State* L = lua.state();
        auto frames = [&] {
            for (int i = 0; i < 3; ++i) {
                api.begin_frame();
                frame(lua);
                rig.step();
            }
        };
        api.show("InventoryManagerWnd", true);
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(ImGui::FindWindowByName("###modern.InventoryManagerWnd") != nullptr);
        // the pack draws it, so the EQ menu offers its row after Inventory
        CHECK(app::menu_command_offered(app::kInventoryManagerCommand, [&](std::string_view s) { return api.replaces(s); }));

        // the mock's character: a Short Sword in Primary, five Rations in the first general slot
        REQUIRE(luaL_dostring(L, R"lua(
local l = eq.AllItems()
return #l, l[1].where, l[1].item.name, l[1].place, l[2].where, l[2].item.count, l[2].place, l[2].kept, l[2].item.serial
)lua") == LUA_OK);
        CHECK(lua_tointeger(L, 1) == 2);
        CHECK(std::string(lua_tostring(L, 2)) == "worn");
        CHECK(std::string(lua_tostring(L, 3)) == "Short Sword");
        CHECK(std::string(lua_tostring(L, 4)) == "i:13");
        CHECK(std::string(lua_tostring(L, 5)) == "carried");
        CHECK(lua_tointeger(L, 6) == 5);
        CHECK(std::string(lua_tostring(L, 7)) == "i:23");
        CHECK_FALSE(lua_toboolean(L, 8));
        REQUIRE(lua_isstring(L, 9));
        CHECK(std::string(lua_tostring(L, 9)) == conn.inventory().at(at(23))->serial);
        lua_settop(L, 0);

        // Keep through the shared tools: the client has it, and a Sell mark on the same item clears it
        REQUIRE(luaL_dostring(L, R"lua(
local tools = dofile("lib/itemtools.lua")
local rations = eq.Item(23)
tools.SetKeep(rations, true)
local kept = tools.Kept(rations)
tools.SetSell(rations, true)
return kept, eq.ItemKept(rations.id), tools.Junk(rations)
)lua") == LUA_OK);
        CHECK(lua_toboolean(L, 1));
        CHECK_FALSE(lua_toboolean(L, 2)); // Sell took the Keep mark off
        CHECK(lua_toboolean(L, 3));
        lua_settop(L, 0);
        REQUIRE(luaL_dostring(L, "local t = dofile('lib/itemtools.lua') t.SetKeep(eq.Item(23), true)") == LUA_OK);
        CHECK(conn.kept_items().kept(1002));
        const auto before_batch = conn.kept_items_revision();
        REQUIRE(luaL_dostring(L, R"lua(
local invalid = pcall(eq.SetItemsKept, {{id=1001, name="Sword"}, {id=0}}, true)
return invalid, eq.ItemKept(1001)
)lua") == LUA_OK);
        CHECK_FALSE(lua_toboolean(L, 1));
        CHECK_FALSE(lua_toboolean(L, 2));
        CHECK(conn.kept_items_revision() == before_batch);
        lua_settop(L, 0);
        REQUIRE(luaL_dostring(L, R"lua(
local tools = dofile("lib/itemtools.lua")
tools.SetKeepMany({eq.Item(13), eq.Item(23), eq.Item(13)}, true)
return eq.ItemKept(1001), eq.ItemKept(1002), tools.Junk(eq.Item(23))
)lua") == LUA_OK);
        CHECK(lua_toboolean(L, 1));
        CHECK(lua_toboolean(L, 2));
        CHECK_FALSE(lua_toboolean(L, 3));
        CHECK(conn.kept_items_revision() == before_batch + 1);
        lua_settop(L, 0);
        conn.set_item_kept(1001, "Sword", false);
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);

        // the grid, a filter and a sort (Kind, no longer a sort, falls back to Place)
        REQUIRE(luaL_dostring(L, "ui.SetSetting('advinv.view', 'grid') ui.SetSetting('advinv.sort', 'Kind') "
                                 "ui.SetSetting('advinv.mark', 'keep')") == LUA_OK);
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);

        // the stats, and the bulk actions' counts (the Rations are kept, the Short Sword worn: neither is taken)
        REQUIRE(luaL_dostring(L, R"lua(
local st = eq.ItemStats()
local l = eq.AllItems()
local nd, ed = eq.BulkItems("destroy", { "i:23", "i:13", "b:0", "nonsense" }, true)
local ns, es = eq.BulkItems("sell", { "i:23" }, true)
local bad = pcall(eq.BulkItems, "juggle", {})
return st[1][1], st[1][2], #st, type(l[1].item.stats), l[2].item.tribute, nd, ed, ns, es, bad
)lua") == LUA_OK);
        CHECK(std::string(lua_tostring(L, 1)) == "ac");
        CHECK(std::string(lua_tostring(L, 2)) == "AC");
        CHECK(lua_tointeger(L, 3) > 40);
        CHECK(std::string(lua_tostring(L, 4)) == "table");
        CHECK(lua_tointeger(L, 5) == 0);
        CHECK(lua_tointeger(L, 6) == 0);
        CHECK(lua_isnil(L, 7)); // nothing stops a destroy, there is just nothing to take
        CHECK(lua_tointeger(L, 8) == 0);
        CHECK(std::string(lua_tostring(L, 9)) == "Open a merchant first.");
        CHECK_FALSE(lua_toboolean(L, 10));
        lua_settop(L, 0);
        REQUIRE(luaL_dostring(L, "local t = dofile('lib/itemtools.lua') t.SetKeep(eq.Item(23), false) "
                                 "return eq.BulkItems('destroy', { 'i:23', 'i:23' }, true)") == LUA_OK);
        CHECK(lua_tointeger(L, 1) == 1); // once, chosen twice
        lua_settop(L, 0);

        // the list with a stat's column and sort, and a selection: the selection's bar draws; a search from
        // /finditem clears the filters and the selection
        REQUIRE(luaL_dostring(L, "ui.SetSetting('advinv.view', 'list') ui.SetSetting('advinv.mark', 'any') "
                                 "ui.SetSetting('advinv.stat', 'ac') ui.SetSetting('advinv.sort', 'Stat')") == LUA_OK);
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(luaL_dostring(L, "ui.SetSetting('advinv.stat', '') ui.SetSetting('advinv.sort', 'Value') "
                                 "local s = ADVANCED_INVENTORY.sel local it = eq.AllItems()[2].item "
                                 "s.places['i:23'] = {id=it.id, serial=it.serial, quantity=it.count} s.n = 1") == LUA_OK);
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(luaL_dostring(L, "return ADVANCED_INVENTORY.sel.n") == LUA_OK);
        CHECK(lua_tointeger(L, 1) == 1); // still shown, still selected
        lua_settop(L, 0);
        host.item_search = "Short";
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK_FALSE(host.item_search.has_value());
        REQUIRE(luaL_dostring(L, "return ADVANCED_INVENTORY.sel.n, ui.GetSetting('advinv.mark')") == LUA_OK);
        CHECK(lua_tointeger(L, 1) == 0);
        lua_settop(L, 0);

        // the inventory window, with its Advanced Inventory button, draws too
        api.show("InventoryWindow", true);
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(ImGui::FindWindowByName("###modern.InventoryWindow") != nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Advanced Inventory selection and confirmation retain item instance and stack identities") {
    ScratchDir scratch("inventory_manager_identity_test");
    const fs::path dir = scratch.path();
    fs::copy(ui_dir() / "modern", dir / "modern", fs::copy_options::recursive);
    std::unique_ptr<ImGuiContext, void (*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto config = MockConfig::standard();
    for (auto& item : config.characters[0].inventory)
        if (item.slot.same(net::ItemSlot::possessions(23))) item.serial = "0000000000001002";
    auto options = quick_options();
    options.servers_dir = dir / "servers";
    Rig rig(std::move(config), options);
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    put_coin_away(conn);
    REQUIRE(conn.inventory().at(at(23)));
    struct Mutation {
        net::GameConnection* conn;
        net::Item replacement;
    } mutation{&conn, *conn.inventory().at(at(23))};
    app::EqHost host;
    host.in_game = host.ui_ready = true;
    host.game = &rig.game;
    host.conn = &conn;
    app::EqApi api(host);
    api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
    ui::lua::LuaUi lua(dir / "modern", dir / "modern.txt");
    lua.add_eq_api([&](lua_State* state) {
        api.register_functions(state);
        lua_pushcfunction(state, [](lua_State* s) { lua_pushboolean(s, 1); return 1; });
        lua_setfield(state, -2, "InGame");
    });
    lua_State* state = lua.state();
    lua_pushlightuserdata(state, &mutation);
    lua_pushcclosure(state, [](lua_State* s) {
        const auto* change = static_cast<Mutation*>(lua_touserdata(s, lua_upvalueindex(1)));
        change->conn->receive_zone_packet_for_test(net::encode_item_packet(false, {net::kItemPacketCharInventory, change->replacement}));
        return 0;
    }, 1);
    lua_setglobal(state, "test_replace");
    REQUIRE(luaL_dostring(state, R"lua(
ui.SetSetting("advinv.view", "grid")
test_started, test_confirm_drawn = 0, 0
local invisible, button, bulk = ui.InvisibleButton, ui.Button, eq.BulkItems
ui.InvisibleButton = function(id, ...)
  local clicked = invisible(id, ...)
  if test_hit == id then test_hit = nil return true end
  return clicked
end
ui.Button = function(label, ...)
  local clicked = button(label, ...)
  if label == "Destroy 1" then test_confirm_drawn = test_confirm_drawn + 1 end
  if test_confirm == label then
    test_confirm = nil
    if test_mutate_on_confirm then test_mutate_on_confirm = nil test_replace() end
    return true
  end
  return clicked
end
eq.BulkItems = function(mode, places, dry)
  if not dry then test_started = test_started + 1 end
  return bulk(mode, places, dry)
end
test_all_items = 0
local all_items = eq.AllItems
eq.AllItems = function(...)
  test_all_items = test_all_items + 1
  return all_items(...)
end
)lua") == LUA_OK);
    auto one_frame = [&] {
        api.begin_frame();
        frame(lua);
        REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
    };
    auto value = [&](const char* code) {
        REQUIRE(luaL_dostring(state, code) == LUA_OK);
        const auto result = lua_tointeger(state, -1);
        lua_settop(state, 0);
        return result;
    };
    api.show("InventoryManagerWnd", true);
    for (int i = 0; i < 3; ++i) one_frame();
    REQUIRE(luaL_dostring(state, "test_hit = '##gridselecti:23'") == LUA_OK);
    one_frame();
    REQUIRE(value("return ADVANCED_INVENTORY.sel.n") == 1);
    REQUIRE(luaL_dostring(state, R"lua(
local selected = ADVANCED_INVENTORY.sel.places["i:23"]
return selected.id, selected.serial, selected.quantity
)lua") == LUA_OK);
    CHECK(lua_tointeger(state, 1) == 1002);
    REQUIRE(lua_type(state, 2) == LUA_TSTRING);
    CHECK(std::string(lua_tostring(state, 2)) == "0000000000001002");
    CHECK(lua_tointeger(state, 3) == 5);
    lua_settop(state, 0);

    enum class ChangeAt { Selection, Modal, Confirm, Unchanged };
    ChangeAt when = ChangeAt::Modal;
    bool rewrite_selection = false;
    SUBCASE("replacement serial before opening confirmation") {
        when = ChangeAt::Selection;
        mutation.replacement.serial = "0000000000002002";
    }
    SUBCASE("stack count before opening confirmation") {
        when = ChangeAt::Selection;
        mutation.replacement.stack_count = 6;
    }
    SUBCASE("replacement definition before opening confirmation") {
        when = ChangeAt::Selection;
        mutation.replacement.id = 2002;
    }
    SUBCASE("replacement serial while confirmation is open") { mutation.replacement.serial = "0000000000002002"; }
    SUBCASE("stack count while confirmation is open") { mutation.replacement.stack_count = 6; }
    SUBCASE("the modal cannot adopt a rebased selection table") {
        mutation.replacement.serial = "0000000000002002";
        rewrite_selection = true;
    }
    SUBCASE("identity is checked again immediately before starting the job") {
        when = ChangeAt::Confirm;
        mutation.replacement.serial = "0000000000002002";
    }
    SUBCASE("an unchanged selection starts the existing bulk job") { when = ChangeAt::Unchanged; }
    auto replace = [&] {
        conn.receive_zone_packet_for_test(net::encode_item_packet(false, {net::kItemPacketCharInventory, mutation.replacement}));
    };
    if (when == ChangeAt::Selection) {
        replace();
        one_frame();
        CHECK(value("return ADVANCED_INVENTORY.sel.n") == 0);
        REQUIRE(luaL_dostring(state, "test_hit = '##advbulkdestroy'") == LUA_OK);
        one_frame();
    } else {
        REQUIRE(luaL_dostring(state, "test_hit = '##advbulkdestroy'") == LUA_OK);
        one_frame();
        REQUIRE(value("return test_confirm_drawn") > 0);
        if (when == ChangeAt::Unchanged) {
            // The open dialog checks the snapshot each frame, but reads the items again only when they change.
            const auto before = value("return test_all_items");
            for (int i = 0; i < 5; ++i) one_frame();
            CHECK(value("return test_all_items") - before <= 1);
        }
        if (when == ChangeAt::Modal) {
            replace();
            if (rewrite_selection)
                REQUIRE(luaL_dostring(state, "ADVANCED_INVENTORY.sel.places['i:23'].serial = '0000000000002002'") == LUA_OK);
        }
        REQUIRE(luaL_dostring(state, when == ChangeAt::Confirm ?
            "test_confirm = 'Destroy 1' test_mutate_on_confirm = true" : "test_confirm = 'Destroy 1'") == LUA_OK);
        one_frame();
    }
    const bool unchanged = when == ChangeAt::Unchanged;
    CHECK(value("return test_started") == (unchanged ? 1 : 0));
    CHECK(conn.item_job().has_value() == unchanged);
    CHECK(value("return ADVANCED_INVENTORY.sel.n") == 0);
    REQUIRE(conn.inventory().at(at(23)));
    CHECK(conn.inventory().at(at(23))->id == mutation.replacement.id);
    CHECK(conn.inventory().at(at(23))->serial == mutation.replacement.serial);
    CHECK(conn.inventory().at(at(23))->stack_count == mutation.replacement.stack_count);
    CHECK_FALSE(conn.inventory().cursor());
}

TEST_CASE("Kept items: not destroyed or dropped, and kept per character in kept_items.txt") {
    ScratchDir scratch("kept_items_conn_test");
    net::ConnectionOptions options = quick_options();
    options.servers_dir = scratch.path();
    {
        Rig rig(MockConfig::standard(), options);
        REQUIRE(rig.to_zone());
        auto& conn = rig.conn;
        put_coin_away(conn);
        conn.set_item_kept(1002, "Rations", true);
        fs::path file = conn.own_files().kept_items();
        REQUIRE(fs::exists(file));
        CHECK(file_text(file).find("\n1002 Rations\n") != std::string::npos);

        // the whole stack onto the cursor
        conn.click_slot(net::ItemSlot::possessions(net::kSlotGeneral1), true);
        REQUIRE(conn.inventory().cursor());
        CHECK(conn.inventory().cursor()->id == 1002u);
        conn.destroy_held();
        conn.destroy_cursor();
        CHECK(conn.inventory().cursor());       // still held
        CHECK_FALSE(conn.request_drop_cursor()); // no drop confirmation asked for
        CHECK_FALSE(conn.drop_cursor_on_ground(true));
        CHECK(conn.inventory().cursor());

        // unmarked, it goes
        conn.set_item_kept(1002, "", false);
        conn.destroy_cursor();
        CHECK_FALSE(conn.inventory().cursor());
        conn.set_item_kept(1001, "Short Sword", true);
    }
    // the next login of the character reads them back
    Rig again(MockConfig::standard(), options);
    REQUIRE(again.to_zone());
    CHECK(again.conn.kept_items().kept(1001));
    CHECK_FALSE(again.conn.kept_items().kept(1002));
}

TEST_CASE("Advanced Inventory bulk destroy: what is chosen and carried, one item at a time") {
    ScratchDir scratch("inventory_bulk_destroy_test");
    MockConfig config = MockConfig::standard();
    auto& inv = config.characters[0].inventory;
    inv.push_back(make_item(2001, "Rusty Dagger", 24));
    net::Item sack = bag_item(2002, "Sack", 25);
    sack.contents.push_back({0, make_item(2003, "Pebble", 25)});
    inv.push_back(sack);
    net::Item heirloom = make_item(2004, "Heirloom", 26);
    heirloom.unknown34_37[3] = 1u << 16; // No Destroy
    inv.push_back(heirloom);
    inv.push_back(bag_item(2005, "Empty Sack", 27));
    net::ConnectionOptions options = quick_options();
    options.servers_dir = scratch.path();
    Rig rig(std::move(config), options);
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    put_coin_away(conn);
    REQUIRE(id_at(conn, 25, 0) == 2003u);
    conn.set_item_kept(1002, "Rations", true);
    using M = net::GameConnection::ItemJobMode;
    // worn, kept, a bag that holds something and a No Destroy item stay; a bag's content and an empty bag go
    const std::vector<net::ItemSlot> chosen{at(13), at(23), at(24), at(25), at(25, 0), at(26), at(27)};
    auto plan = conn.bulk_items_plan(M::Destroy, chosen);
    REQUIRE(plan.size() == 3);
    CHECK(plan[0].same(at(24)));
    CHECK(plan[1].same(at(25, 0)));
    CHECK(plan[2].same(at(27)));
    CHECK(conn.start_bulk_job(M::Destroy, {at(13), at(23)}) == "Nothing chosen is something you carry that can be destroyed.");

    const int moves = MockEqServer::count(rig.server.zone_log, net::Op::MoveItem);
    CHECK(conn.start_bulk_job(M::Destroy, chosen).empty());
    CHECK(conn.start_bulk_job(M::Destroy, chosen) == "Already busy with your items.");
    REQUIRE(conn.item_job());
    CHECK(conn.item_job()->mode == M::Destroy);
    REQUIRE(rig.until([&] { return !conn.item_job(); }));
    CHECK(id_at(conn, 24) == 0u);
    CHECK(id_at(conn, 25, 0) == 0u);
    CHECK(id_at(conn, 27) == 0u);
    CHECK(id_at(conn, 25) == 2002u);
    CHECK(id_at(conn, 26) == 2004u);
    CHECK(id_at(conn, 23) == 1002u);
    CHECK(id_at(conn, 13) == 1001u);
    CHECK_FALSE(conn.inventory().cursor());
    // each one picked up, then destroyed from the cursor
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::MoveItem) >= moves + 6; }));
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::MoveItem) == moves + 6);

    // not while something is held
    conn.click_slot(at(25), true, false);
    REQUIRE(conn.inventory().cursor());
    CHECK(conn.start_bulk_job(M::Destroy, {at(26)}) == "Put down what you are holding first.");
}

TEST_CASE("Advanced Inventory bulk sell: the chosen carried items the open merchant buys, one sale at a time") {
    MockConfig config = MockConfig::standard();
    auto& inv = config.characters[0].inventory;
    inv.push_back(make_item(2010, "Bone Chips", 24, 3));
    inv.push_back(bag_item(2011, "Sack", 25));
    inv.push_back(make_item(2012, "Spider Silk", 26));
    Rig rig(std::move(config));
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    put_coin_away(conn);
    using M = net::GameConnection::ItemJobMode;
    const std::vector<net::ItemSlot> chosen{at(13), at(24), at(25), at(26)};
    CHECK(conn.start_bulk_job(M::Sell, chosen) == "Open a merchant first.");
    CHECK(conn.bulk_items_plan(M::Sell, chosen).size() == 2); // not worn, not a bag

    auto& world = conn.world_for_test();
    game::Spawn m = *world.player();
    m.id = 5001;
    m.name = "Merchant00";
    m.class_id = 41;
    m.is_npc = true;
    world.spawns[m.id] = m;
    conn.open_merchant(m.id);
    net::MerchantClick open;
    open.npc_id = m.id;
    open.player_id = world.player_id;
    open.rate = 1.0f;
    rig.server.zone.send_reliable(net::encode(open));
    REQUIRE(rig.until([&] { return conn.merchant() && conn.merchant()->open; }));

    REQUIRE(conn.start_bulk_job(M::Sell, chosen).empty());
    // each sale waits for the merchant's answer
    for (int k = 1; k <= 2; ++k) {
        REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::ShopPlayerSell) == k; }));
        rig.run_for(std::chrono::milliseconds(200));
        CHECK(MockEqServer::count(rig.server.zone_log, net::Op::ShopPlayerSell) == k);
        std::optional<net::MerchantSell> sold;
        for (auto it = rig.server.zone_log.rbegin(); it != rig.server.zone_log.rend(); ++it)
            if (net::from_wire(it->opcode) == net::Op::ShopPlayerSell) {
                sold = net::decode_merchant_sell(*it);
                break;
            }
        REQUIRE(sold);
        CHECK(sold->slot == (k == 1 ? 24 : 26));
        CHECK(sold->quantity == (k == 1 ? 3u : 1u)); // the whole stack
        sold->price = 10;
        rig.server.zone.send_reliable(net::encode(*sold));
    }
    REQUIRE(rig.until([&] { return !conn.item_job(); }));
    CHECK(id_at(conn, 24) == 0u);
    CHECK(id_at(conn, 26) == 0u);
    CHECK(id_at(conn, 25) == 2011u);
    CHECK(id_at(conn, 13) == 1001u);
}

TEST_CASE("Advanced Inventory bulk tribute: donated one at a time to the open tribute master; a refused one goes back") {
    MockConfig config = MockConfig::standard();
    auto& inv = config.characters[0].inventory;
    net::Item gem = make_item(2020, "Gem", 24);
    gem.favor = 50;
    inv.push_back(gem);
    inv.push_back(make_item(2021, "Lint", 25)); // worth no favour
    net::Item ruby = make_item(2022, "Ruby", 26);
    ruby.favor = 10;
    inv.push_back(ruby);
    Rig rig(std::move(config));
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    put_coin_away(conn);
    using M = net::GameConnection::ItemJobMode;
    const std::vector<net::ItemSlot> chosen{at(13), at(24), at(25), at(26)};
    CHECK(conn.start_bulk_job(M::Tribute, chosen) == "Open a tribute master first.");
    auto master = *conn.world().player();
    master.id = 77;
    master.is_npc = true;
    conn.world_for_test().spawns[77] = master;
    rig.server.zone.send_reliable(net::encode(net::StartTribute{conn.world().player_id, 77, 1}));
    REQUIRE(rig.until([&] { return conn.tribute().master_id == 77u; }));
    CHECK(conn.bulk_items_plan(M::Tribute, chosen).size() == 2);

    REQUIRE(conn.start_bulk_job(M::Tribute, chosen).empty());
    auto donation = [&](int k) {
        REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::TributeItem) == k; }));
        std::optional<net::TributeItem> t;
        for (auto it = rig.server.zone_log.rbegin(); it != rig.server.zone_log.rend() && !t; ++it)
            if (net::from_wire(it->opcode) == net::Op::TributeItem) t = net::decode_tribute_item(*it);
        REQUIRE(t);
        CHECK(t->slot.same(net::ItemSlot::possessions(net::kSlotCursor))); // from the cursor, as the window's Donate
        CHECK(t->master_id == 77u);
        REQUIRE(conn.inventory().cursor());
        return *t;
    };
    net::TributeItem first = donation(1);
    CHECK(conn.inventory().cursor()->id == 2020u);
    rig.run_for(std::chrono::milliseconds(200)); // the next waits for the answer
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::TributeItem) == 1);
    first.points = 50;
    rig.server.zone.send_reliable(net::encode(first));
    net::TributeItem second = donation(2);
    CHECK(conn.inventory().cursor()->id == 2022u);
    second.points = 0; // refused
    rig.server.zone.send_reliable(net::encode(second));
    REQUIRE(rig.until([&] { return !conn.item_job(); }));
    CHECK_FALSE(conn.inventory().cursor());
    CHECK(id_at(conn, 24) == 0u);
    CHECK(id_at(conn, 25) == 2021u);
    CHECK(id_at(conn, 26) == 2022u); // back in its slot
    CHECK(conn.tribute().last_donation == 0);
}

TEST_CASE("/finditem and /advi go to the Advanced Inventory's handler") {
    Rig rig;
    REQUIRE(rig.to_zone());
    std::vector<std::pair<std::string, std::string>> calls;
    rig.conn.set_client_commands([&](std::string_view h, const std::string& args) {
        calls.emplace_back(std::string(h), args);
        return true;
    });
    rig.conn.send_command("/finditem rusty dagger");
    rig.conn.send_command("/ADVI");
    REQUIRE(calls.size() == 2);
    CHECK(calls[0] == std::pair<std::string, std::string>{"Hailstone_finditem", "rusty dagger"});
    CHECK(calls[1] == std::pair<std::string, std::string>{"Hailstone_finditem", ""});
}

TEST_CASE("/camera goes to the client's camera handler (docs/isometric.md)") {
    Rig rig;
    REQUIRE(rig.to_zone());
    std::vector<std::pair<std::string, std::string>> calls;
    rig.conn.set_client_commands([&](std::string_view h, const std::string& args) {
        calls.emplace_back(std::string(h), args);
        return true;
    });
    rig.conn.send_command("/camera");
    rig.conn.send_command("/camera reveal 25");
    rig.conn.send_command("/CAMERA iso");
    REQUIRE(calls.size() == 3);
    CHECK(calls[0] == std::pair<std::string, std::string>{"Hailstone_camera", ""});
    CHECK(calls[1] == std::pair<std::string, std::string>{"Hailstone_camera", "reveal 25"});
    CHECK(calls[2] == std::pair<std::string, std::string>{"Hailstone_camera", "iso"});
}

TEST_CASE("Advanced Inventory: the bank as last seen is listed, marked stale, and out of reach") {
    fs::path src = ui_dir() / "modern";
    ScratchDir scratch("inventory_manager_bank_seen");
    fs::path dir = scratch.path();
    fs::copy(src, dir / "modern", fs::copy_options::recursive);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        MockConfig config = MockConfig::standard();
        net::Item bag = bag_item(17005, "Large Sack", 0);
        bag.slot = net::ItemSlot{net::kInvBank, 3, -1, -1, 0, 0};
        net::Item ring = make_item(9990, "Fine Steel Ring", 0);
        ring.slot = net::ItemSlot{net::kInvBank, 3, 1, -1, 0, 0};
        bag.contents.push_back({1, ring});
        config.characters[0].inventory.push_back(bag);
        net::ConnectionOptions options = quick_options();
        options.servers_dir = dir / "servers";
        Rig rig(std::move(config), options);
        REQUIRE(rig.to_zone());
        auto& conn = rig.conn;
        put_coin_away(conn);
        // the next zone-in leaves the bank out (EQEmu's Inventory:LazyLoadBank)
        rig.server.config.characters[0].inventory.pop_back();
        const int entries = rig.server.zone_entries;
        rig.server.request_zone_change(1, 20.0f, 10.0f, 3.0f);
        REQUIRE(rig.until([&] { return rig.server.zone_entries == entries + 1 && conn.state() == net::ConnectionState::InZone; }));
        REQUIRE(rig.until([&] { return conn.player_status().max_hp != 0; }));
        REQUIRE_FALSE(conn.bank_live());

        app::EqHost host;
        host.in_game = host.ui_ready = true;
        host.game = &rig.game;
        host.conn = &conn;
        app::EqApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        ui::lua::LuaUi lua(dir / "modern", dir / "modern.txt");
        lua.add_eq_api([&](lua_State* L) {
            api.register_functions(L);
            lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
            lua_setfield(L, -2, "InGame");
        });
        lua_State* L = lua.state();
        auto frames = [&] {
            for (int i = 0; i < 3; ++i) {
                api.begin_frame();
                frame(lua);
                rig.step();
            }
        };

        REQUIRE(luaL_dostring(L, R"lua(
local rows, bank, ring = eq.AllItems(), nil, nil
for _, r in ipairs(rows) do
  if r.place == "b:3" then bank = r end
  if r.place == "b:3:1" then ring = r end
end
local live, age = eq.BankSeen()
local n, _, found = eq.FindItems("bank", "steel ring")
local carried = eq.AllItems()[2]
return bank and bank.stale, ring and ring.stale, ring and ring.bag, live, age, n, found[1] and found[1].stale,
  carried.stale == nil
)lua") == LUA_OK);
        CHECK(lua_toboolean(L, 1));
        CHECK(lua_toboolean(L, 2));
        CHECK(std::string(lua_tostring(L, 3)) == "Large Sack");
        CHECK_FALSE(lua_toboolean(L, 4));
        CHECK(lua_isinteger(L, 5));
        CHECK(lua_tointeger(L, 6) == 1);
        CHECK(lua_toboolean(L, 7));
        CHECK(lua_toboolean(L, 8)); // only banked rows are ever stale
        lua_settop(L, 0);

        // Nothing reaches the copy: a bank click, a withdraw, a destroy.
        const int moves = MockEqServer::count(rig.server.zone_log, net::Op::MoveItem);
        REQUIRE(luaL_dostring(L, R"lua(
eq.BankSlotClick(3, nil, "left", false)
eq.BankSlotClick(3, 1, "left", false)
local w = eq.BulkItems("withdraw", { "b:3:1" }, true)
local d = eq.BulkItems("destroy", { "b:3" }, true)
return w, d
)lua") == LUA_OK);
        CHECK(lua_tointeger(L, 1) == 0);
        CHECK(lua_tointeger(L, 2) == 0);
        lua_settop(L, 0);
        CHECK(conn.inventory().cursor() == nullptr);

        // The window draws the stale rows in both views.
        api.show("InventoryManagerWnd", true);
        REQUIRE(luaL_dostring(L, "ui.SetSetting('advinv.view', 'list') ui.SetSetting('advinv.place', 'bank')") == LUA_OK);
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(luaL_dostring(L, "ui.SetSetting('advinv.view', 'grid')") == LUA_OK);
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(MockEqServer::count(rig.server.zone_log, net::Op::MoveItem) == moves);

        // Opened at a banker, the bank is what the server has: no longer stale (here, empty, as nothing came).
        REQUIRE(conn.activate_bank(rig.server.config.own_spawn_id));
        REQUIRE(luaL_dostring(L, R"lua(
local stale = 0
for _, r in ipairs(eq.AllItems()) do if r.stale then stale = stale + 1 end end
return stale, (eq.BankSeen())
)lua") == LUA_OK);
        CHECK(lua_tointeger(L, 1) == 0);
        CHECK(lua_toboolean(L, 2));
        lua_settop(L, 0);
    }
    ImGui::DestroyContext(ctx);
}
