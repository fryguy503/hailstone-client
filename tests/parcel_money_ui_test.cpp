// The merchant's Parcels page and the inventory's coin buttons (0x698290 before CInventoryWnd::ClickedMoneyButton's
// quantity dialog; app/lua_eq_items.cpp, GameConnection::parcel_coin_button / set_parcel_money) over the eq API,
// against the mock server.
#include <doctest/doctest.h>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "mock_server/harness.h"
#include "net/item_packets.h"
#include "net/merchant_packets.h"
#include "net/packet_io.h"
#include "net/tribute_packets.h"

using namespace hailstone;
using namespace hailstone::test;

namespace {

constexpr std::uint32_t kMerchant = 5001;

// A parcel merchant beside the player, opened with all three pages, and 7 platinum 3 gold in the purse, none held.
void open_parcel_merchant(net::GameConnection& conn) {
    auto& w = conn.world_for_test();
    REQUIRE(w.player());
    game::Spawn m = *w.player();
    m.id = kMerchant;
    m.name = "Parcel_Merchant00";
    m.class_id = 41;
    m.is_npc = true;
    w.spawns[kMerchant] = m;
    conn.open_merchant(kMerchant);
    REQUIRE(conn.merchant());
    net::MerchantClick open;
    open.npc_id = kMerchant;
    open.player_id = w.player_id;
    open.rate = 1.0f;
    open.tab_display = 7;
    conn.receive_zone_packet_for_test(net::encode(open));
    REQUIRE(conn.merchant()->open);
    // The mock's profile has coin on the cursor: into the purse first (a coin button with coin held).
    for (int t = 0; t < 4; ++t)
        if (conn.cursor_coins()[static_cast<std::size_t>(3 - t)]) conn.inventory_money_button(t, -1, nullptr);
    REQUIRE(conn.cursor_coins() == std::array<std::uint32_t, 4>{});
    conn.receive_zone_packet_for_test(net::encode_money_update(net::Coins{7, 3, 0, 0}));
    REQUIRE(conn.coins().platinum == 7);
}

struct Lua {
    lua_State* L = luaL_newstate();
    explicit Lua(app::EqApi& api) {
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "eq");
    }
    ~Lua() { lua_close(L); }
    void run(const char* code) {
        REQUIRE(luaL_dostring(L, code) == LUA_OK);
        lua_settop(L, 0);
    }
    bool flag(const char* code) {
        REQUIRE(luaL_dostring(L, code) == LUA_OK);
        bool v = lua_toboolean(L, -1);
        lua_settop(L, 0);
        return v;
    }
    lua_Integer integer(const char* code) {
        REQUIRE(luaL_dostring(L, code) == LUA_OK);
        lua_Integer v = lua_tointeger(L, -1);
        lua_settop(L, 0);
        return v;
    }
};

} // namespace

TEST_CASE("Parcels page: an inventory coin button picks money to send, lets the bag item go, and moves no coin") {
    ImGuiContext* ctx = ImGui::CreateContext(); // the eq API reads the keyboard's modifiers
    ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
    MockConfig config = MockConfig::standard();
    Rig rig(std::move(config));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    open_parcel_merchant(conn);

    app::EqHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::EqApi api(host);
    Lua lua(api);

    // The rations picked in the bags for sending, on the Parcels page.
    conn.merchant_select(net::ItemSlot::possessions(23));
    REQUIRE(conn.merchant()->selected);
    lua.run("eq.SetTabPage('MerchantWnd', 'MW_MerchantSubwindows', 2)");
    const auto moves_before = MockEqServer::count(rig.server.zone_log, net::Op::MoveCoin);

    // Platinum: the item goes, the button is outlined, the dialog asks how much (all 7 to start).
    CHECK(lua.flag("return eq.ClickChild('InventoryWindow', 'IW_Money0', 'left')"));
    CHECK_FALSE(conn.merchant()->selected);
    CHECK(conn.merchant()->parcel_coin == 0);
    CHECK(lua.flag("return select(2, eq.ChildState('InventoryWindow', 'IW_Money0')) == true"));
    CHECK(lua.flag("return select(2, eq.ChildState('InventoryWindow', 'IW_Money1')) == false"));
    REQUIRE(host.quantity);
    CHECK(host.quantity->max == 7);
    auto done = std::move(host.quantity->done);
    host.quantity.reset();
    done(4);
    // The answer is the money picked for sending (Hailstone): nothing on the cursor, the purse as it was.
    REQUIRE(conn.merchant()->parcel_money);
    CHECK(conn.merchant()->parcel_money->coin == 0);
    CHECK(conn.merchant()->parcel_money->amount == 4);
    CHECK(conn.cursor_coins()[3] == 0);
    CHECK(conn.coins().platinum == 7);
    CHECK(lua.integer("local _, _, _, _, _, m = eq.Parcels() return m.amount") == 4);
    CHECK(lua.integer("local _, _, _, _, _, m = eq.Parcels() return m.coin") == 0);
    const lua_Integer serial = lua.integer("local _, _, _, _, _, m = eq.Parcels() return m.serial");

    // Copper with none in the purse: SelectSlot(0) still runs, but no outline and no dialog.
    lua.run("eq.ClickChild('InventoryWindow', 'IW_Money3', 'left')");
    CHECK(conn.merchant()->parcel_coin == -1);
    CHECK_FALSE(host.quantity);

    // A bag click (SelectSlot) clears the outline.
    lua.run("eq.ClickChild('InventoryWindow', 'IW_Money1', 'left')");
    CHECK(conn.merchant()->parcel_coin == 1);
    REQUIRE(host.quantity);
    done = std::move(host.quantity->done);
    host.quantity.reset();
    done(2);
    CHECK(lua.integer("local _, _, _, _, _, m = eq.Parcels() return m.serial") != serial);
    conn.merchant_select(net::ItemSlot::possessions(23));
    CHECK(conn.merchant()->parcel_coin == -1);

    // Off the Parcels page the button is the original's: the answer comes onto the cursor (OP_MoveCoin 1 -> 0).
    lua.run("eq.SetTabPage('MerchantWnd', 'MW_MerchantSubwindows', 0)");
    lua.run("eq.ClickChild('InventoryWindow', 'IW_Money0', 'left')");
    CHECK(conn.merchant()->selected); // no SelectSlot(0) here
    REQUIRE(host.quantity);
    done = std::move(host.quantity->done);
    host.quantity.reset();
    done(3);
    CHECK(conn.cursor_coins()[3] == 3);
    CHECK(conn.coins().platinum == 4);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::MoveCoin) > moves_before; }));
    ImGui::DestroyContext(ctx);
}

TEST_CASE("merchant quantity questions end with their session, including a reopened merchant") {
    for (bool parcels : {false, true}) {
        for (bool reopen_before_frame : {false, true}) {
            CAPTURE(parcels);
            CAPTURE(reopen_before_frame);
            ImGuiContext* ctx = ImGui::CreateContext();
            ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
            Rig rig(MockConfig::standard());
            auto& conn = rig.conn;
            REQUIRE(rig.to_zone());
            open_parcel_merchant(conn);
            app::EqHost host;
            host.in_game = host.ui_ready = true;
            host.conn = &conn;
            app::EqApi api(host);
            Lua lua(api);

            if (parcels) {
                lua.run("eq.SetTabPage('MerchantWnd', 'MW_MerchantSubwindows', 2)");
                lua.run("eq.ClickChild('InventoryWindow', 'IW_Money0', 'left')");
            } else {
                conn.merchant_select(net::ItemSlot::possessions(23));
                REQUIRE(lua.flag("return eq.ClickChild('MerchantWnd', 'MW_Sell_Button', 'left')"));
            }
            REQUIRE(host.quantity);
            auto stale_answer = host.quantity->done;
            conn.close_merchant();
            if (reopen_before_frame) open_parcel_merchant(conn);
            api.begin_frame();
            CHECK_FALSE(host.quantity);
            CHECK_FALSE(lua.flag("return eq.WindowShown('QuantityWnd')"));
            if (!reopen_before_frame) open_parcel_merchant(conn);
            stale_answer(2);
            CHECK(conn.merchant()->pending == 0);
            CHECK_FALSE(conn.merchant()->parcel_money);
            CHECK(conn.coins().platinum == 7);
            CHECK(conn.cursor_coins() == std::array<std::uint32_t, 4>{});
            CHECK(conn.inventory().at(net::ItemSlot::possessions(23))->stack_count == 5);
            conn.close_merchant();
            ImGui::DestroyContext(ctx);
        }
    }
}

TEST_CASE("tribute coin questions close with the personal or guild master session") {
    for (bool guild : {false, true}) {
        CAPTURE(guild);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
        Rig rig(MockConfig::standard());
        auto& conn = rig.conn;
        REQUIRE(rig.to_zone());
        auto& world = conn.world_for_test();
        game::Spawn master = *world.player();
        master.id = kMerchant;
        master.name = "Tribute_Master00";
        master.is_npc = true;
        master.class_id = guild ? 64 : 63;
        world.spawns[kMerchant] = master;
        conn.receive_zone_packet_for_test(net::encode_money_update(net::Coins{7, 0, 0, 0}));
        auto open = [&] {
            auto packet = net::encode(net::StartTribute{world.player_id, kMerchant, 1});
            if (guild) packet.opcode = net::to_wire(net::Op::OpenGuildTributeMaster);
            conn.receive_zone_packet_for_test(packet);
            REQUIRE((guild ? conn.guild_tribute().master_id : conn.tribute().master_id) == kMerchant);
        };
        open();
        app::EqHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::EqApi api(host);
        Lua lua(api);
        REQUIRE(lua.flag("return eq.ClickChild('TributeMasterWnd', 'TMW_CoinButton', 'left')"));
        REQUIRE(host.quantity);
        auto stale_answer = host.quantity->done;
        if (guild) conn.close_guild_tribute_master();
        else conn.close_tribute_master();
        open(); // even reopening the same master cannot reuse the old quantity question
        api.begin_frame();
        CHECK_FALSE(host.quantity);
        stale_answer(3);
        CHECK(lua.integer("return tonumber((eq.WindowText('TributeMasterWnd', 'TMW_CoinButton')))") == 0);
        REQUIRE(lua.flag("return eq.ClickChild('TributeMasterWnd', 'TMW_CoinButton', 'left')"));
        REQUIRE(host.quantity);
        host.quantity->done(2);
        host.quantity.reset();
        CHECK(lua.integer("return tonumber((eq.WindowText('TributeMasterWnd', 'TMW_CoinButton')))") == 2);
        if (guild) conn.close_guild_tribute_master();
        else conn.close_tribute_master();
        open();
        api.begin_frame();
        CHECK(lua.integer("return tonumber((eq.WindowText('TributeMasterWnd', 'TMW_CoinButton')))") == 0);
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("barter and guild bank questions cannot survive a new service session") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
    Rig rig(MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    app::EqHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::EqApi api(host);
    Lua lua(api);

    auto& buyer = const_cast<game::BarterMerchantState&>(conn.barter_merchant());
    auto open_buyer = [&](std::uint64_t serial) {
        buyer.buyer_id = kMerchant;
        buyer.serial = serial;
        net::BuyLine line;
        line.item_id = 1002;
        line.quantity = 5;
        line.slot = 0;
        buyer.lines = {line};
        buyer.selected_slot = 0;
    };
    open_buyer(1);
    REQUIRE(lua.flag("return eq.ClickChild('BarterMerchantWnd', 'SellButton', 'left')"));
    REQUIRE(host.quantity);
    auto stale_sale = host.quantity->done;
    conn.close_buyer();
    open_buyer(2);
    api.begin_frame();
    CHECK_FALSE(host.quantity);
    stale_sale(2);
    CHECK(buyer.pending == 0);
    conn.close_buyer();

    auto& bank = const_cast<net::GameConnection::GuildBank&>(conn.guild_bank());
    auto open_bank = [&](std::uint64_t serial) {
        bank.banker = kMerchant;
        bank.serial = serial;
        bank.selected = 0;
        net::GuildBankItem item;
        item.item_id = 1002;
        item.quantity = 5;
        bank.deposit = {item};
    };
    open_bank(3);
    REQUIRE(lua.flag("return eq.ClickChild('GuildBankWnd', 'WithdrawButton', 'left')"));
    REQUIRE(host.quantity);
    auto stale_withdraw = host.quantity->done;
    conn.close_guild_bank();
    open_bank(4);
    api.begin_frame();
    CHECK_FALSE(host.quantity);
    stale_withdraw(2);
    CHECK(bank.pending == 0);

    REQUIRE(lua.flag("return eq.ClickChild('GuildBankWnd', 'ChangeTargetButton', 'left')"));
    REQUIRE(host.text_entry);
    conn.close_guild_bank();
    open_bank(5);
    api.begin_frame();
    CHECK_FALSE(host.text_entry);
    CHECK_FALSE(lua.flag("return eq.WindowShown('TextEntryWnd')"));
    ImGui::DestroyContext(ctx);
}
