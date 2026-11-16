// The real estate windows over the hs API (app/lua_ui_realestate.cpp): what the plot, manage, items, neighborhood and
// plot search windows show from the server's records and what their buttons send, against the mock server.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "core/byte_io.h"
#include "mock_server/harness.h"
#include "net/real_estate_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

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

ReDefinition def(std::uint32_t id, ReType type, std::int32_t area_link = -1) {
    ReDefinition d;
    d.id = id;
    d.type = type;
    d.f01 = area_link;
    d.price_group = 100;
    d.upkeep_group = 200;
    d.capacity = 40;
    d.vault_capacity = 10;
    return d;
}

ReRecord rec(std::uint32_t id, std::uint32_t parent, std::uint32_t definition, std::vector<std::uint32_t> children = {}) {
    ReRecord r;
    r.id = id;
    r.parent = parent;
    r.definition = definition;
    r.children = std::move(children);
    r.address = std::to_string(id) + " Main Street";
    return r;
}

const AppPacket* last_sent(const std::vector<AppPacket>& log, std::uint16_t op) {
    for (auto it = log.rbegin(); it != log.rend(); ++it)
        if (it->opcode == op) return &*it;
    return nullptr;
}

struct Ui {
    Rig rig;
    app::UiHost host;
    app::UiApi api{host};
    lua_State* L = nullptr;
    Ui() {
        host.in_game = host.ui_ready = true;
        host.conn = &rig.conn;
        L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
    }
    ~Ui() { lua_close(L); }
    std::string operator()(const std::string& code) { return run(L, code); }
};

} // namespace

TEST_CASE("real estate windows: the plot window's purchase, the manage window's escrow and access, the items window") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        Ui ui;
        auto& rig = ui.rig;
        auto& server = rig.server;
        REQUIRE(rig.to_zone());

        // A neighborhood with plot 2 (area 7 around the player) and its house 3; prices 50 platinum or 20 of
        // currency 5; upkeep 1 platinum a day.
        ReCatalog catalog;
        catalog.definitions = {def(10, ReType::Neighborhood), def(20, ReType::Plot, 7), def(30, ReType::House)};
        catalog.costs = {{100, {{11, {{0, 0, 50000}}}, {12, {{5, 0, 20}}}}}, {200, {{3, {{0, 0, 1000}}}}}};
        server.zone.send_reliable(encode(catalog));
        server.zone.send_reliable(encode_re_record(kOpReRecord, rec(2, 1, 20, {3})));
        server.zone.send_reliable(encode_re_record(kOpReRecord, rec(3, 2, 30)));
        server.zone.send_reliable(encode_re_record(kOpReSetCurrent, rec(1, game::kReNone, 10, {2})));
        ReArea area;
        area.id = 7;
        area.m = {86, 0, 0, 0, 0, 86, 0, 0, 0, 0, 350, 0, -285, -148, 0, 1};
        server.zone.send_reliable(encode(std::vector<ReArea>{area}));
        REQUIRE(rig.until([&] { return rig.conn.real_estate().occupancy().yard == 2; }));

        // ---- the plot window on plot 2
        rig.conn.re_show_plot(2);
        const std::string pw = "'RealEstatePurchaseWnd'";
        CHECK(rig.conn.real_estate_ui().purchase_open);
        CHECK(ui("return (hs.WindowText(" + pw + ", 'AddressLabel'))") == "2 Main Street");
        CHECK(ui("return (hs.WindowText(" + pw + ", 'TypeLabel'))") == "Player");
        CHECK(ui("return (hs.WindowText(" + pw + ", 'CapacityLabel'))") == "40");
        CHECK(ui("return #(hs.ComboInfo(" + pw + ", 'PriceCombo'))") == "2");
        CHECK(ui("return (hs.ComboInfo(" + pw + ", 'PriceCombo'))[1]") == "50 platinum");
        CHECK(ui("return (hs.ChildState(" + pw + ", 'PurchaseButton'))") == "true");
        CHECK(ui("return hs.ChildShown(" + pw + ", 'SellButton')") == "false");
        // The second price, then Purchase: 0x45be with keys 12 and 3.
        CHECK(ui("return hs.ComboSelect(" + pw + ", 'PriceCombo', 1)") == "true");
        CHECK(ui("return hs.ClickChild(" + pw + ", 'PurchaseButton', 'left')") == "true");
        REQUIRE(rig.until([&] { return last_sent(server.zone_log, kOpRePurchase) != nullptr; }));
        auto buy = decode_re_transaction(*last_sent(server.zone_log, kOpRePurchase));
        REQUIRE(buy);
        CHECK(buy->re == 2);
        CHECK(buy->ac == 12);
        CHECK(buy->b0 == 3);
        CHECK_FALSE(rig.conn.real_estate_ui().purchase_open);

        // The server sells it to us: owned, the window shows Sell / Transfer, Manage is enabled.
        ReRecord owned = rec(2, 1, 20, {3});
        owned.owner = "Gennro";
        owned.upkeep_set = 3;
        owned.escrow = {{0, 2500.0}};
        server.zone.send_reliable(encode_re_record(kOpReRecord, owned));
        ByteWriter ok;
        ok.le<std::int32_t>(0);
        server.zone.send_reliable(AppPacket(kOpReResult, ok.take()));
        REQUIRE(rig.until([&] { return rig.conn.real_estate().already_owned(2) && !rig.conn.real_estate().pending(); }));
        rig.conn.re_show_plot(2);
        CHECK(ui("return hs.ChildShown(" + pw + ", 'SellButton')") == "true");
        CHECK(ui("return hs.ChildShown(" + pw + ", 'PurchaseButton')") == "false");
        CHECK(ui("return (hs.ChildState(" + pw + ", 'ManageButton'))") == "true");
        // Sell asks first (eqstr 7634); yes sends 0x792c for the plot we stand on.
        CHECK(ui("return hs.ClickChild(" + pw + ", 'SellButton', 'left')") == "true");
        REQUIRE(ui.host.confirm);
        auto yes = ui.host.confirm->done;
        ui.host.confirm.reset();
        yes(true);
        REQUIRE(rig.until([&] { return last_sent(server.zone_log, kOpReSell) != nullptr; }));
        CHECK(decode_re_transaction(*last_sent(server.zone_log, kOpReSell))->re == 2);

        // ---- the manage window: the escrow's coin as denominations, the time left, Add Day, a withdrawal.
        const std::string mw = "'RealEstateManageWnd'";
        ui("hs.ShowWindow(" + mw + ", true)");
        CHECK(rig.conn.real_estate_ui().manage_open);
        CHECK(ui("return (hs.WindowText(" + mw + ", 'TimeLeftLabel'))") == "2d"); // 2500 copper at 1000 a day
        CHECK(ui("return (hs.ListInfo(" + mw + ", 'BalanceList'))") == "4");
        CHECK(ui("return hs.ListRow(" + mw + ", 'BalanceList', 0)[2]") == "2"); // 2 platinum
        CHECK(ui("return hs.ClickChild(" + mw + ", 'AddDayButton', 'left')") == "true");
        REQUIRE(rig.until([&] { return last_sent(server.zone_log, kOpReEscrow) != nullptr; }));
        CHECK(decode_re_escrow(*last_sent(server.zone_log, kOpReEscrow))->action == 3);
        // The upkeep plan: escrow action 2 with the entry's key at +8.
        CHECK(ui("return hs.ComboSelect(" + mw + ", 'UpkeepCombo', 0)") == "true");
        REQUIRE(rig.until([&] { return decode_re_escrow(*last_sent(server.zone_log, kOpReEscrow))->action == 2; }));
        CHECK(decode_re_escrow(*last_sent(server.zone_log, kOpReEscrow))->f8 == 3);
        // An access bit toggled sends the whole table on 0x3146 (Everyone's Enter goes off).
        CHECK(ui("return (select(2, hs.ChildState(" + mw + ", 'Perm_0_1')))") == "true");
        CHECK(ui("return hs.ClickChild(" + mw + ", 'Perm_0_1', 'left')") == "true");
        REQUIRE(rig.until([&] { return last_sent(server.zone_log, kOpReAccessSave) != nullptr; }));
        {
            ByteReader r(last_sent(server.zone_log, kOpReAccessSave)->data);
            CHECK(r.le<std::uint32_t>() == 2);
            r.le<std::uint32_t>();
            CHECK(r.le<std::uint32_t>() == 1); // Everyone: 3 without bit 1
        }

        // ---- the items window: the yard's rows; Shed sends 0x3424 with the key, ids and destination 3.
        ReItemList list;
        list.re = 2;
        list.name = "Plot";
        ReItemRow row;
        row.re = 2;
        row.item_id = 9;
        row.item.id = 1001;
        row.item.name = "Chair";
        row.item.serial = "0000000000000042";
        list.rows = {row};
        server.zone.send_reliable(encode(kOpReItemSummary, list));
        REQUIRE(rig.until([&] { return rig.conn.real_estate().summary(2) != nullptr; }));
        const std::string iw = "'RealEstateItemsWnd'";
        ui("hs.ShowWindow(" + iw + ", true)");
        CHECK(ui("return (hs.ListInfo(" + iw + ", 'ItemList'))") == "1");
        CHECK(ui("return hs.ListRow(" + iw + ", 'ItemList', 0)[2]") == "Chair");
        CHECK(ui("return (hs.ChildState(" + iw + ", 'ShedButton'))") == "false"); // nothing selected
        CHECK(ui("return hs.ListClick(" + iw + ", 'ItemList', 0, 'left')") == "true");
        CHECK(ui("return (hs.ChildState(" + iw + ", 'ShedButton'))") == "true");
        CHECK(ui("return (hs.ChildState(" + iw + ", 'PlaceButton'))") == "false");
        CHECK(ui("return hs.ClickChild(" + iw + ", 'ShedButton', 'left')") == "true");
        REQUIRE(rig.until([&] { return last_sent(server.zone_log, kOpReToStorage) != nullptr; }));
        {
            const AppPacket* p = last_sent(server.zone_log, kOpReToStorage);
            REQUIRE(p->data.size() == 48);
            CHECK(std::string(reinterpret_cast<const char*>(p->data.data())) == "0000000000000042");
            ByteReader r(p->data);
            r.skip(0x20);
            CHECK(r.le<std::uint32_t>() == 2);
            CHECK(r.le<std::uint32_t>() == 9);
            r.skip(4);
            CHECK(r.le<std::uint32_t>() == 3);
        }
        // A right click asks for the item's menu (0x21ec with the key and ids).
        CHECK(ui("return hs.ListClick(" + iw + ", 'ItemList', 0, 'right')") == "true");
        REQUIRE(rig.until([&] { return last_sent(server.zone_log, kOpReContextMenu) != nullptr; }));
        auto q = decode_re_menu(*last_sent(server.zone_log, kOpReContextMenu));
        REQUIRE(q);
        CHECK(q->key == "0000000000000042");
        CHECK(q->item == 9);
        CHECK(q->f2f8 == 1001);
        // The server's menu: "enter" (type 1) and a placement (type 2, greyed). Entering sends 0x218c {re, item}.
        ReMenu menu = *q;
        menu.name = "Small House";
        menu.restricted = false;
        menu.entries = {{0, 1, 0, 5244, -1}, {1, 2, 0, 719, -1}};
        server.zone.send_reliable(encode(menu));
        REQUIRE(rig.until([&] { return rig.conn.real_estate_ui().menu.has_value(); }));
        auto rows = app::real_estate_menu_rows(ui.host, *rig.conn.real_estate_ui().menu);
        REQUIRE(rows.size() == 3); // the name, then the two entries
        CHECK(rows[0].text == "Small House");
        CHECK(rows[2].enabled == false);
        REQUIRE(rows[1].act);
        rows[1].act();
        REQUIRE(rig.until([&] { return last_sent(server.zone_log, kOpReTravel) != nullptr; }));
        {
            ByteReader r(last_sent(server.zone_log, kOpReTravel)->data);
            CHECK(r.le<std::uint32_t>() == 2);
            CHECK(r.le<std::uint32_t>() == 9);
        }
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("real estate windows: the neighborhood list and the plot search") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        Ui ui;
        auto& rig = ui.rig;
        auto& server = rig.server;
        REQUIRE(rig.to_zone());
        ReCatalog catalog;
        catalog.definitions = {def(10, ReType::Neighborhood)};
        server.zone.send_reliable(encode(catalog));
        server.zone.send_reliable(encode_re_record(kOpReSetCurrent, rec(1, game::kReNone, 10)));
        ReSearchReply hoods;
        hoods.kind = ReSearchReply::Neighborhoods;
        hoods.target = 0;
        hoods.show = true;
        hoods.header_id = 1;
        hoods.neighborhoods = {{1, "Hilltop", -1, 10, 2, 4, 1}};
        server.zone.send_reliable(encode(hoods));
        REQUIRE(rig.until([&] { return rig.conn.real_estate_ui().neighborhood_open; }));
        const std::string nw = "'RealEstateNeighborhoodWnd'";
        CHECK(ui("return hs.ListRow(" + nw + ", 'NeighborhoodList', 0)[1]") == "Hilltop");
        CHECK(ui("return hs.ListRow(" + nw + ", 'NeighborhoodList', 0)[3]") == "2/10");
        // A row: 0x2691 subtype 2 with its id; Go: 0x218c {1, -1}.
        CHECK(ui("return hs.ListClick(" + nw + ", 'NeighborhoodList', 0, 'left')") == "true");
        REQUIRE(rig.until([&] { return last_sent(server.zone_log, kOpReSearch) != nullptr; }));
        {
            ByteReader r(last_sent(server.zone_log, kOpReSearch)->data);
            CHECK(r.le<std::uint32_t>() == 2);
            CHECK(r.le<std::uint32_t>() == 1);
        }
        CHECK(ui("return hs.ClickChild(" + nw + ", 'GoButton', 'left')") == "true");
        REQUIRE(rig.until([&] { return last_sent(server.zone_log, kOpReTravel) != nullptr; }));
        {
            ByteReader r(last_sent(server.zone_log, kOpReTravel)->data);
            CHECK(r.le<std::uint32_t>() == 1);
            CHECK(r.le<std::uint32_t>() == 0xffffffffu);
        }

        // The plot search: the name at +0x5c; the reply's rows; Go with mode 1 sends 0x796c.
        server.zone.send_reliable(AppPacket(kOpRePlotSearchOpen, {std::byte{1}}));
        REQUIRE(rig.until([&] { return rig.conn.real_estate_ui().plot_search_open; }));
        const std::string sw = "'RealEstatePlotSearchWnd'";
        ui("hs.SetWindowText(" + sw + ", 'NameEdit', 'Bob')");
        std::size_t before = server.zone_log.size();
        CHECK(ui("return hs.ClickChild(" + sw + ", 'SearchButton', 'left')") == "true");
        REQUIRE(rig.until([&] {
            const AppPacket* p = last_sent(server.zone_log, kOpReSearch);
            return server.zone_log.size() > before && p && p->data.size() == 0xe0 && p->data[0x5c] == std::byte{'B'};
        }));
        ReSearchReply plots;
        plots.kind = ReSearchReply::PlotList;
        plots.target = 4;
        plots.title = "Plots of Bob";
        plots.plots = {{600, ReType::Plot, 2, "Bob", "2 Main Street"}};
        server.zone.send_reliable(encode(plots));
        REQUIRE(rig.until([&] { return rig.conn.real_estate_ui().plots.size() == 1; }));
        CHECK(ui("return hs.WindowTitle(" + sw + ")") == "Plots of Bob");
        CHECK(ui("return (hs.ChildState(" + sw + ", 'GoButton'))") == "false");
        CHECK(ui("return hs.ListClick(" + sw + ", 'PlotList', 0, 'left')") == "true");
        CHECK(ui("return hs.ClickChild(" + sw + ", 'GoButton', 'left')") == "true");
        REQUIRE(rig.until([&] { return last_sent(server.zone_log, kOpRePlotSelect) != nullptr; }));
        ByteReader r(last_sent(server.zone_log, kOpRePlotSelect)->data);
        CHECK(r.le<std::uint32_t>() == 2);
    }
    ImGui::DestroyContext(ctx);
}
