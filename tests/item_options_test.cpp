// The Options window's item and purchase settings where they act (game/item_options.h, game/tradeskill.h):
// DestroyHeld, the loot window's questions, /fastdrop and /lootnodrop, the purchase questions and OpenContainer's
// tradeskill window choice.
#include <doctest/doctest.h>

#include "core/ini_file.h"
#include "game/client_options.h"
#include "game/item_options.h"
#include "game/tradeskill.h"

using namespace hailstone;
using game::ClientOption;

TEST_CASE("item options: CInventoryWnd::DestroyHeld") {
    using D = game::DestroyDecision;
    game::DestroyInput in;
    CHECK(game::destroy_held_decision(in).decision == D::Nothing);
    in.item = true;
    auto r = game::destroy_held_decision(in);
    CHECK(r.decision == D::Confirm);
    CHECK(r.eqstr == game::kStrDestroyItem);
    in.fast_destroy = true;
    CHECK(game::destroy_held_decision(in).decision == D::DestroyNow);
    in.real_estate = true; // asked even with Fast Item Destroy
    r = game::destroy_held_decision(in);
    CHECK(r.decision == D::Confirm);
    CHECK(r.eqstr == game::kStrDestroyRealEstate);
    in.satchel_in_trader = true;
    CHECK(game::destroy_held_decision(in).decision == D::SatchelRefused);
    game::DestroyInput coin;
    coin.money = true;
    r = game::destroy_held_decision(coin);
    CHECK(r.decision == D::Confirm);
    CHECK(r.eqstr == game::kStrDestroyMoney);
    coin.fast_destroy = true;
    CHECK(game::destroy_held_decision(coin).decision == D::DestroyNow);

    // DestroyHeldItemOrMoney's coin: the highest coin held (platinum first), wire type and amount.
    std::uint32_t none[4] = {0, 0, 0, 0}, some[4] = {7, 0, 3, 0};
    CHECK_FALSE(game::destroy_cursor_coin(none).has_value());
    auto d = game::destroy_cursor_coin(some);
    REQUIRE(d.has_value());
    CHECK(d->type == 2);
    CHECK(d->amount == 3);
}

TEST_CASE("item options: guarded cursor destroy command accepts only exact IDs") {
    CHECK(game::destroy_cursor_item_id("123") == 123u);
    CHECK(game::destroy_cursor_item_id(" 4294967295 ") == 4294967295u);
    for (const auto& invalid : {"", "0", "-1", "+1", "123x", "123 456", "4294967296", "0x7b"})
        CHECK_FALSE(game::destroy_cursor_item_id(invalid).has_value());
}

TEST_CASE("item options: Loot All and no-drop questions") {
    CHECK(game::loot_all_asks(false, 1));
    CHECK_FALSE(game::loot_all_asks(true, 1)); // the player's own corpse
    CHECK_FALSE(game::loot_all_asks(false, 0));

    int asked = 0;
    auto usable = [&](bool v) {
        return [&asked, v] {
            ++asked;
            return v;
        };
    };
    CHECK(game::loot_no_drop_asks(false, false, 0, usable(true)));
    CHECK(asked == 0); // LootNoDrop 0 asks without looking at the item's classes
    CHECK_FALSE(game::loot_no_drop_asks(false, false, 1, usable(true)));
    CHECK(game::loot_no_drop_asks(false, false, 1, usable(false)));
    CHECK(asked == 2);
    CHECK_FALSE(game::loot_no_drop_asks(false, false, 2, usable(false)));
    CHECK_FALSE(game::loot_no_drop_asks(false, true, 0, usable(false)));  // droppable
    CHECK_FALSE(game::loot_no_drop_asks(true, false, 0, usable(false)));  // own corpse
    CHECK(game::loot_no_drop_asks(false, false, 7, usable(true)));         // out of range: 0
    CHECK(game::loot_no_drop_question("Rusty Sword") == "Rusty Sword is a NO DROP item, are you sure you wish to loot it?");
}

TEST_CASE("item options: CLootWnd::LootAll's plan") {
    // Bags first, then the rest, each in slot order.
    auto p = game::plan_loot_all({{23, false, false}, {24, true, false}, {25, false, false}}, false);
    CHECK(p.queue == std::vector<std::int16_t>{24, 23, 25});
    CHECK_FALSE(p.ask.has_value());
    CHECK_FALSE(p.close);

    // One no-drop item: asked about, left out of the list.
    p = game::plan_loot_all({{23, false, false}, {24, false, true}}, false);
    CHECK(p.queue == std::vector<std::int16_t>{23});
    REQUIRE(p.ask.has_value());
    CHECK(*p.ask == 24);
    CHECK(p.popup_pending);

    // A second one drops the whole list, but the first question survives the abort.
    p = game::plan_loot_all({{23, false, true}, {24, false, false}, {25, false, true}}, false);
    CHECK(p.aborted);
    CHECK(p.queue.empty());
    REQUIRE(p.ask.has_value());
    CHECK(*p.ask == 23);
    CHECK(p.popup_pending);
    CHECK_FALSE(p.close);

    // A question from before the scan is kept; no new question replaces it.
    p = game::plan_loot_all({{23, false, false}, {24, false, true}}, true);
    CHECK(p.aborted);
    CHECK(p.queue.empty());
    CHECK_FALSE(p.ask.has_value());
    CHECK(p.popup_pending);
    CHECK_FALSE(p.close);

    // Nothing to loot: the window closes unless a question shows.
    CHECK(game::plan_loot_all({}, false).close);
    p = game::plan_loot_all({{23, false, true}}, false);
    CHECK(p.queue.empty());
    CHECK_FALSE(p.close);
}

TEST_CASE("item options: Loot All preserves the first no-drop question in bag-first order") {
    // Ordinary loot collected in the bag pass is also discarded when a later no-drop item aborts the scan.
    const auto p = game::plan_loot_all(
        {{23, false, true}, {24, true, false}, {25, true, true}, {26, false, false}, {27, true, false}}, false);
    CHECK(p.aborted);
    CHECK(p.queue.empty());
    REQUIRE(p.ask.has_value());
    CHECK(*p.ask == 25); // the bag, although the ordinary no-drop item has a lower slot
    CHECK(p.popup_pending);
    CHECK_FALSE(p.close);
}

TEST_CASE("item options: /fastdrop and /lootnodrop") {
    CHECK(game::fastdrop_choice("ON", "on", "off", "never") == 1);
    CHECK(game::fastdrop_choice("off", "on", "off", "never") == 0);
    CHECK(game::fastdrop_choice("Never", "on", "off", "never") == -1);
    CHECK_FALSE(game::fastdrop_choice("onn", "on", "off", "never").has_value());
    CHECK_FALSE(game::fastdrop_choice("", "on", "off", "never").has_value());
    CHECK(game::fastdrop_message(1) == 13116);
    CHECK(game::fastdrop_message(0) == 13117);
    CHECK(game::fastdrop_message(-1) == 13118);
    CHECK(game::fastdrop_combo_value(0) == 0);
    CHECK(game::fastdrop_combo_value(1) == 1);
    CHECK(game::fastdrop_combo_value(2) == -1);

    auto choice = [](std::string_view a) { return game::lootnodrop_choice(a, "never", "sometimes", "usable", "always"); };
    CHECK(choice("NEVERMORE") == 2); // four characters compared
    CHECK(choice("2") == 2);
    CHECK(choice("some") == 1);
    CHECK(choice("usab") == 1);
    CHECK(choice("1") == 1);
    CHECK(choice("Always") == 0);
    CHECK(choice("0") == 0);
    CHECK_FALSE(choice("nev").has_value()); // shorter than four: the end does not match
    CHECK_FALSE(choice("maybe").has_value());
    CHECK(game::lootnodrop_set_name(0) == 12992);
    CHECK(game::lootnodrop_set_name(1) == 12993);
    CHECK(game::lootnodrop_set_name(2) == 12994);
    CHECK(game::lootnodrop_shown_name(0) == 12994); // the original's swap when showing the setting
    CHECK(game::lootnodrop_shown_name(2) == 12992);
}

TEST_CASE("item options: purchase questions") {
    CHECK(game::aa_train_question(1) == 4041);
    CHECK(game::aa_train_question(3) == 4042);
    CHECK(game::leadership_buy_question(true, 1) == 8582);
    CHECK(game::leadership_buy_question(true, 2) == 8583);
    CHECK(game::leadership_buy_question(false, 1) == 8587);
    CHECK(game::leadership_buy_question(false, 4) == 8588);
    CHECK(game::adventure_purchase_question(1, 1, 1) == 5862);
    CHECK(game::adventure_purchase_question(1, 1, 50) == 5863);
    CHECK(game::adventure_purchase_question(1, 3, 1) == 5931);
    CHECK(game::adventure_purchase_question(2, 1, 9) == 5934);
    CHECK(game::adventure_purchase_question(4, 1, 1) == 5953);
    CHECK(game::adventure_purchase_question(5, 2, 2) == 5960);
    CHECK(game::adventure_purchase_question(0, 1, 1) == 0);
}

TEST_CASE("item options: the defaults and load quirks") {
    game::ClientOptions o;
    CHECK(o.get(ClientOption::TradeskillCtrlClick) == 1); // ReadClientINIBool(TradeskillWnd, CtrlBypass, 1)
    CHECK(o.get(ClientOption::LootNoDrop) == 0);
    CHECK(o.get(ClientOption::FastDrop) == 0);
    IniFile ini;
    ini.set_int("Options", "LootNoDrop", 5);          // out of range: 0, as CLootWnd clamps it
    ini.set_int("Options", "LeadershipNoConfirm", 1); // saved, never read back by loadOptions
    ini.set_int("Options", "AANoConfirm", 1);
    o.load(ini);
    CHECK(o.get(ClientOption::LootNoDrop) == 0);
    CHECK(o.get(ClientOption::LeadershipNoConfirm) == 0);
    CHECK(o.get(ClientOption::AANoConfirm) == 1);
}

TEST_CASE("item options: CContainerMgr::OpenContainer's tradeskill window") {
    // CtrlBypass 1: a plain right click wants the tradeskill window, ctrl the container; 0 swaps them.
    CHECK(game::container_tradeskill_wanted(true, false));
    CHECK_FALSE(game::container_tradeskill_wanted(true, true));
    CHECK_FALSE(game::container_tradeskill_wanted(false, false));
    CHECK(game::container_tradeskill_wanted(false, true));

    game::OpenContainerInput in;
    in.tradeskill = true;
    in.top_level_or_world = true;
    in.bag_type = 16; // a sewing kit
    int asked = 0;
    auto skill = [&](bool v) {
        return [&asked, v] {
            ++asked;
            return v;
        };
    };
    CHECK(game::opens_in_tradeskill_window(in, skill(true)));
    CHECK_FALSE(game::opens_in_tradeskill_window(in, skill(false)));
    in.bag_type = 0x33; // not a tradeskill type: the skill is still asked
    asked = 0;
    CHECK_FALSE(game::opens_in_tradeskill_window(in, skill(true)));
    CHECK(asked == 1);
    in.bag_type = 16;
    in.tradeskill_window_active = true;
    asked = 0;
    CHECK_FALSE(game::opens_in_tradeskill_window(in, skill(true)));
    CHECK(asked == 0);
    in.tradeskill_window_active = false;
    in.top_level_or_world = false; // inside another bag, or the bank
    CHECK_FALSE(game::opens_in_tradeskill_window(in, skill(true)));
    in.top_level_or_world = true;
    in.tradeskill = false;
    CHECK_FALSE(game::opens_in_tradeskill_window(in, skill(true)));

    CHECK(game::is_tradeskill_container_type(9));
    CHECK_FALSE(game::is_tradeskill_container_type(13));
    CHECK(game::is_tradeskill_container_type(0x36));

    game::ContainerUser human_warrior{1, 1, 0};
    CHECK(game::container_tradeskill(16, 0, 0, 0, human_warrior).skill == 61); // tailoring
    auto wiz_only = game::container_tradeskill(24, 0, 0, 0, human_warrior);   // a wizard's jewelry kit
    CHECK(wiz_only.skill == -1);
    CHECK(wiz_only.message == 0x135);
    auto class_mask = game::container_tradeskill(16, 1u << 11, 0, 0, human_warrior); // wizards only by the item
    CHECK(class_mask.skill == -1);
    CHECK(class_mask.message == 5942);
    CHECK(game::container_tradeskill(30, 1u << 11, 0, 0, human_warrior).message == 0); // type 30: said nothing
    // A server extension's research classes (Triune): a Warrior / Wizard takes the wizard's kit and the any-of-four
    // research one; a Warrior / Cleric does not.
    game::ContainerUser war_wiz{1, 1, 0, (1u << 0) | (1u << 11)};
    CHECK(game::container_tradeskill(24, 0, 0, 0, war_wiz).skill == 58);
    CHECK(game::container_tradeskill(29, 0, 0, 0, war_wiz).skill == 58);
    game::ContainerUser war_clr{1, 1, 0, (1u << 0) | (1u << 1)};
    CHECK(game::container_tradeskill(24, 0, 0, 0, war_clr).message == 0x135);
}
