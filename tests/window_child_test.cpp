// MQ's Window members for E3's window reads (mq/tlo_game.cpp Window, app::UiApi::window_child): Child's Text,
// Enabled, Value and List from the stock windows' own state, and DoOpen / DoClose / LeftMouseUp acting as they are
// read. No ImGui context and no Lua here: a client without a UI answers alike, as long as it keeps its UiApi.
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "app/lua_ui.h"
#include "mq/parse.h"
#include "mq/tlo.h"
#include "mq_test_world.h"

using namespace hailstone;
using A = net::GameConnectionTestAccess;

namespace {

struct Rig {
    mq_test::World world{"hailstone_window_child_test"};
    app::UiHost host;
    std::unique_ptr<app::UiApi> api;
    mq::Registry reg;
    mq::Context ctx;
    std::vector<std::tuple<std::string, std::string, std::string>> clicks; // what LeftMouseUp / RightMouseUp sent

    Rig() {
        host.in_game = host.ui_ready = true;
        host.conn = world.conn.get();
        api = std::make_unique<app::UiApi>(host);
        mq::register_game_tlos(reg);
        ctx.registry = &reg;
        ctx.conn = world.conn.get();
        // As the client wires them (app/client_app_internal.h): UiApi's answers; the clicks recorded here, where
        // the client sends them as /notify.
        ctx.window_open = [this](std::string_view w) { return api->window_shown(w); };
        ctx.window_child = [this](std::string_view w, std::string_view c, bool rows) { return api->window_child(w, c, rows); };
        ctx.window_act = [this](std::string_view w, std::string_view c, std::string_view action) {
            if (action == "doopen" || action == "doclose") return api->show_window(w, action == "doopen");
            clicks.emplace_back(std::string(w), std::string(c), std::string(action));
            return true;
        };
    }
    std::string p(std::string_view text) { return mq::parse_macro_data(ctx, text); }
};

net::Item item(std::uint32_t id, const char* name) {
    net::Item it;
    it.id = id;
    it.name = name;
    it.races = 0xffffffffu;
    it.classes = 0xffff;
    return it;
}

} // namespace

TEST_CASE("Window: DoOpen and DoClose change what Open reads, with no UI drawn") {
    Rig r;
    CHECK(r.p("${Window[InventoryWindow].Open}") == "FALSE");
    // E3's GiveMe: /invoke ${Window[InventoryWindow].DoOpen}, then it waits for Open.
    CHECK(r.p("${Window[InventoryWindow].DoOpen}") == "TRUE");
    CHECK(r.p("${Window[InventoryWindow].Open} ${Window[InventoryWindow]}") == "TRUE TRUE");
    CHECK(r.p("${Window[InventoryWindow].DoClose}") == "TRUE");
    CHECK(r.p("${Window[InventoryWindow].Open}") == "FALSE");
    // A window this client does not have: nothing to open.
    CHECK(r.p("${Window[NoSuchWnd].DoOpen}") == "FALSE");
    // A window whose state reads ImGui's frame count (the bug report's capture): no ImGui here, no crash.
    CHECK(r.p("${Window[BugReportWindow].Open}") == "FALSE");
    // MQ's names for a window: the Actions window's Abilities page.
    CHECK(r.p("${Window[ActionsAbilitiesPage].DoOpen}") == "TRUE");
    CHECK(r.p("${Window[ActionsWindow].Open}") == "TRUE");
}

TEST_CASE("Window: the quantity dialog's Open and its box's Text come from its state") {
    Rig r;
    CHECK(r.p("${Window[QuantityWnd].Open}") == "FALSE");
    app::UiHost::UiQuantity q;
    q.min = 1, q.max = 20, q.slider = 4;
    q.serial = ++r.host.quantity_serial;
    r.host.quantity = std::move(q);
    CHECK(r.p("${Window[QuantityWnd].Open}") == "TRUE");
    CHECK(r.p("${Window[QuantityWnd].Child[QTYW_SliderInput].Text}") == "5"); // the slider's 4 above the minimum
}

TEST_CASE("Window: the merchant's label, buttons and list rows, in its own order") {
    Rig r;
    auto& c = *r.world.conn;
    c.set_merchant_show_all(true); // every row, whatever the character may use
    auto& m = A::merchant(c).emplace();
    m.open = true;
    m.name = "a merchant";
    m.items[5] = item(13073, "Bone Chips");
    m.items[9] = item(13074, "Zombie Skin");
    m.items[12] = item(13075, "Bone Chips of the Ages");
    const std::string list = "${Window[MerchantWnd].Child[ItemList].List";
    // By text in column 2 (the name): "=" the whole name, else any part, any case; the first such row.
    CHECK(r.p(list + "[=Bone Chips,2]}") == "1");
    CHECK(r.p(list + "[=bone chips of the ages,2]}") == "3");
    CHECK(r.p(list + "[skin,2]}") == "2");
    CHECK(r.p(list + "[=Bone,2]}") == "NULL");
    // By row: the cell's text. A row or column past the list's, NULL.
    CHECK(r.p(list + "[2,2]}") == "Zombie Skin");
    CHECK(r.p(list + "[4,2]}") == "NULL");
    CHECK(r.p(list + "[1,10]}") == "NULL");
    CHECK(r.p(list + "[0,2]}") == "NULL");
    // A column of 0 or below is the first; one at the edges of int's range (or past it) neither overflows nor wraps.
    const std::string first = r.p(list + "[2,1]}");
    CHECK(r.p(list + "[2,0]}") == first);
    CHECK(r.p(list + "[2,-2147483648]}") == first);
    CHECK(r.p(list + "[2,-9223372036854775808]}") == first);
    CHECK(r.p(list + "[2,2147483647]}") == "NULL");
    CHECK(r.p(list + "[2,4294967297]}") == "NULL");
    CHECK(r.p(list + "[2,9223372036854775807]}") == "NULL");
    // MQ's item name for the list (MW_ItemList) is the same list.
    CHECK(r.p("${Window[MerchantWnd].Child[MW_ItemList].List[=Zombie Skin,2]}") == "2");
    // Nothing considered: no label, Buy disabled. Considered: its name, Buy enabled.
    CHECK(r.p("${Window[MerchantWnd].Child[MW_SelectedItemLabel].Text}").empty());
    CHECK(r.p("${Window[MerchantWnd].Child[MW_Buy_Button].Enabled}") == "FALSE");
    c.merchant_consider(9);
    CHECK(r.p("${Window[MerchantWnd].Child[MW_SelectedItemLabel].Text}") == "Zombie Skin");
    CHECK(r.p("${Window[MerchantWnd].Child[MW_Buy_Button].Enabled}") == "TRUE");
    CHECK(r.p("${Window[MerchantWnd].Child[MW_Sell_Button].Enabled}") == "FALSE");
    // A child's Open: its window shows and the window does not hide it (Sell shares Buy's place).
    CHECK(r.p("${Window[MerchantWnd].Child[MW_Buy_Button].Open}") == "TRUE");
    CHECK(r.p("${Window[MerchantWnd].Child[MW_Sell_Button].Open}") == "FALSE");
    CHECK(r.p("${Window[MerchantWnd].Child[MW_Buy_Button].Name}") == "MW_Buy_Button");
}

TEST_CASE("Window: the trade partner's name, by RoF2's child name or the later client's") {
    Rig r;
    auto& t = A::trade(*r.world.conn).emplace();
    t.open = true;
    t.partner_id = 10;
    t.partner_name = "Soandso";
    CHECK(r.p("${Window[TradeWnd].Child[TRDW_HisName].Text}") == "Soandso");
    // E3 asks for TRDW_OtherName when it runs 64-bit, as it does on Hailstone.
    CHECK(r.p("${Window[TradeWnd].Child[TRDW_OtherName].Text}") == "Soandso");
}

TEST_CASE("Window: a gauge's Value, the click methods, and what a child the window does not set reads") {
    Rig r;
    // The combat ability window's time-left gauge: a number on 0..1000 (E3 divides it by 10 for a percent).
    const std::string value = r.p("${Window[CombatAbilityWnd].Child[CAW_CombatEffectTimeRemainingGauge].Value}");
    CHECK(value != "NULL");
    // Not a gauge: NULL.
    CHECK(r.p("${Window[MerchantWnd].Child[MW_Buy_Button].Value}") == "NULL");
    // LeftMouseUp / RightMouseUp send the click on that child as they are read (/invoke ...).
    CHECK(r.p("${Window[BigBankWnd].Child[BIGB_AutoButton].LeftMouseUp}") == "TRUE");
    CHECK(r.p("${Window[LootWnd].Child[LW_LootAllButton].RightMouseUp}") == "TRUE");
    REQUIRE(r.clicks.size() == 2);
    CHECK(r.clicks[0] == std::tuple<std::string, std::string, std::string>{"BigBankWnd", "BIGB_AutoButton", "leftmouseup"});
    CHECK(r.clicks[1] == std::tuple<std::string, std::string, std::string>{"LootWnd", "LW_LootAllButton", "rightmouseup"});
    // A child the window gives no text: "" (whether the XML has such a child is not known here); a window this
    // client does not have: NULL.
    CHECK(r.p("${Window[MerchantWnd].Child[NoSuchChild].Text}").empty());
    CHECK(r.p("${Window[NoSuchWnd].Child[Anything].Text}") == "NULL");
    // Without the client's hooks (a test context, a script host with no UiApi): NULL, nothing sent.
    r.ctx.window_child = nullptr;
    r.ctx.window_act = nullptr;
    CHECK(r.p("${Window[TradeWnd].Child[TRDW_HisName].Text}") == "NULL");
    CHECK(r.p("${Window[InventoryWindow].DoOpen}") == "NULL");
}
