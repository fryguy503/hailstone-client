// /notify's line (game/window_notify.h): the window, child and notification, MQ's child names, and the buttons a
// client with no UI runs from the connection.
#include <doctest/doctest.h>

#include "game/window_notify.h"

using namespace hailstone;
using game::NotifyAction;
using game::notify_action;
using game::parse_window_notify;

TEST_CASE("notify: window, child, notification and value") {
    auto n = parse_window_notify("TradeWnd TRDW_Trade_Button leftmouseup");
    CHECK(n.error.empty());
    CHECK(n.window == "TradeWnd");
    CHECK(n.child == "TRDW_Trade_Button");
    CHECK(n.notification == "leftmouseup");
    n = parse_window_notify("MerchantWnd ItemList listselect 4");
    CHECK(n.error.empty());
    CHECK(n.notification == "listselect");
    CHECK(n.value == 4);
    CHECK(parse_window_notify("InventoryWindow IW_Subwindows TabSelect 5").value == 5);
    CHECK(parse_window_notify("LootWnd DoneButton RIGHTMOUSEUP").notification == "rightmouseup");
    // Errors.
    CHECK(parse_window_notify("LootWnd DoneButton").error.rfind("Syntax: /notify", 0) == 0);
    CHECK(parse_window_notify("MerchantWnd ItemList listselect").error == "Invalid listselect value");
    CHECK(parse_window_notify("MerchantWnd ItemList listselect 0").error == "Invalid listselect value");
    CHECK(parse_window_notify("MerchantWnd ItemList listselect x").error == "Invalid listselect value");
    CHECK(parse_window_notify("QuantityWnd QTYW_Slider wheel 5").error == "Notification 'wheel' not supported");
}

TEST_CASE("notify: MQ's item names and case become the ScreenID; pages their window") {
    CHECK(parse_window_notify("ConfirmationDialogBox CD_Yes_Button leftmouseup").child == "Yes_Button");
    CHECK(parse_window_notify("TaskSelectWnd TSEL_AcceptButton leftmouseup").child == "AcceptButton");
    CHECK(parse_window_notify("BigBankWnd BIGB_AutoButton leftmouseup").child == "AutoButton");
    CHECK(parse_window_notify("RaidWindow Raid_UnlockButton leftmouseup").child == "RAID_UnlockButton");
    CHECK(parse_window_notify("QuantityWnd QTYW_slider leftmouseup").child == "QTYW_Slider");
    CHECK(parse_window_notify("LootWnd LootAllButton leftmouseup").child == "LootAllButton"); // a ScreenID stays
    auto t = parse_window_notify("TBW_PersonalPage TBWP_ActivateButton leftmouseup");
    CHECK(t.window == "TributeBenefitWnd");
    CHECK(t.child == "ActivateButton");
    CHECK(parse_window_notify("ActionsAbilitiesPage AAP_FirstAbilityButton leftmouseup").window == "ActionsWindow");
}

TEST_CASE("notify: the buttons a client with no UI runs") {
    auto act = [](std::string_view line) {
        auto n = parse_window_notify(line);
        return notify_action(n.window, n.child);
    };
    CHECK(act("TradeWnd TRDW_Trade_Button leftmouseup") == NotifyAction::TradeAccept);
    CHECK(act("TradeWnd TRDW_Cancel_Button leftmouseup") == NotifyAction::TradeCancel);
    CHECK(act("GiveWnd GVW_Give_Button leftmouseup") == NotifyAction::TradeAccept);
    CHECK(act("LootWnd LootAllButton leftmouseup") == NotifyAction::LootAll);
    CHECK(act("LootWnd DoneButton leftmouseup") == NotifyAction::LootDone);
    CHECK(act("ConfirmationDialogBox Yes_Button leftmouseup") == NotifyAction::ConfirmYes);
    CHECK(act("ConfirmationDialogBox CD_Yes_Button leftmouseup") == NotifyAction::ConfirmYes);
    CHECK(act("ConfirmationDialogBox No_Button leftmouseup") == NotifyAction::ConfirmNo);
    CHECK(act("LargeDialogWindow LDW_YesButton leftmouseup") == NotifyAction::LargeDialogYes);
    CHECK(act("LargeDialogWindow LDW_NoButton leftmouseup") == NotifyAction::LargeDialogNo);
    CHECK(act("TaskSelectWnd TSEL_DeclineButton leftmouseup") == NotifyAction::TaskDecline);
    CHECK(act("BigBankWnd BIGB_AutoButton leftmouseup") == NotifyAction::BankAuto);
    CHECK(act("BigBankWnd BIGB_DoneButton leftmouseup") == NotifyAction::BankDone);
    CHECK(act("TBW_PersonalPage TBWP_ActivateButton leftmouseup") == NotifyAction::TributeToggle);
    CHECK(act("TaskSelectWnd TSEL_AcceptButton leftmouseup") == NotifyAction::TaskAccept);
    CHECK(act("RespawnWnd SelectButton leftmouseup") == NotifyAction::RespawnSelect);
    CHECK(act("RaidWindow Raid_UnlockButton leftmouseup") == NotifyAction::RaidUnlock);
    CHECK(act("RaidWindow raid_lockbutton leftmouseup") == NotifyAction::RaidLock);
    CHECK(act("MerchantWnd MW_Buy_Button leftmouseup") == NotifyAction::MerchantBuy);
    CHECK(act("MerchantWnd MW_Sell_Button leftmouseup") == NotifyAction::MerchantSell);
    CHECK(act("MerchantWnd MW_Done_Button leftmouseup") == NotifyAction::MerchantDone);
    CHECK(act("QuantityWnd QTYW_Accept_Button leftmouseup") == NotifyAction::QuantityAccept);
    CHECK(act("InventoryWindow IW_Money0 leftmouseup") == NotifyAction::Money0);
    CHECK(act("InventoryWindow IW_Money3 leftmouseup") == NotifyAction::Money3);
    CHECK(act("InventoryWindow IW_AltCurr_ReclaimButton leftmouseup") == NotifyAction::AltCurrencyReclaim);
    CHECK(act("InventoryWindow IW_AltCurr_CreateItemButton leftmouseup") == NotifyAction::AltCurrencyCreate);
    CHECK(act("RespawnWnd RW_SelectButton leftmouseup") == NotifyAction::RespawnSelect);
    // Not one of E3's: the stock window's (a UI).
    CHECK(act("InventoryWindow IW_Tinting leftmouseup") == NotifyAction::None);
    CHECK(act("LootWnd TRDW_Trade_Button leftmouseup") == NotifyAction::None); // another window's button
}

TEST_CASE("notify: the lists a listselect picks from, and the quantity slider's newvalue") {
    auto list = [](std::string_view line) {
        auto n = parse_window_notify(line);
        return game::notify_list(n.window, n.child);
    };
    using game::NotifyList;
    CHECK(list("MerchantWnd ItemList listselect 2") == NotifyList::MerchantItems);
    CHECK(list("MerchantWnd MW_ItemList listselect 2") == NotifyList::MerchantItems);
    CHECK(list("InventoryWindow AltCurr_PointList listselect 1") == NotifyList::AltCurrencies);
    CHECK(list("TaskSelectWnd TSEL_TaskList listselect 1") == NotifyList::TaskOffers);
    CHECK(list("RespawnWnd RW_OptionsList listselect 1") == NotifyList::RespawnOptions);
    CHECK(list("BazaarSearchWnd ItemList listselect 1") == NotifyList::None);
    auto n = parse_window_notify("QuantityWnd QTYW_slider newvalue 20");
    CHECK(n.error.empty());
    CHECK(n.value == 20);
    CHECK(game::notify_quantity_slider(n.window, n.child));
    CHECK(parse_window_notify("QuantityWnd QTYW_Slider newvalue 0").error.empty()); // a slider may go to 0
    CHECK(parse_window_notify("QuantityWnd QTYW_Slider newvalue").error == "Invalid newvalue value");
}
