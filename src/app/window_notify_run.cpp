// /notify and MQ's window methods (MQ's WindowNotify and SendWndClick2): what a script, a plugin or the player
// presses, run from the client's state with or without a UI. ClientApp::notify_command and window_act call these.

#include <format>
#include <string>
#include <string_view>

#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "core/log.h"
#include "game/window_notify.h"
#include "net/game_connection.h"
#include "plugin/policy.h"

namespace hailstone::app {

bool run_notify_command(UiHost& host, UiApi* api, std::string_view args) {
    if (!host.conn) return false;
    const game::WindowNotify n = game::parse_window_notify(args);
    if (!n.error.empty()) {
        host.conn->ui_message(n.error);
        return false;
    }
    // What E3 presses runs from the client's state, UI or none (game/window_notify.h's maps): each with the call its
    // window's handler makes, and only while that window would show (the stock window's shown()'s checks). The
    // windows read and write the same state (the merchant's and the alternate currency's selection, the task and
    // respawn choice, the quantity dialog's slider, the confirmation's question), so the two agree.
    using A = game::NotifyAction;
    using L = game::NotifyList;
    auto& c = *host.conn;
    if (n.notification == "leftmouseup")
        if (const A action = game::notify_action(n.window, n.child); action != A::None) {
            switch (action) {
            case A::TradeAccept: if (c.trade() && c.trade()->open) c.trade_accept(); break;
            case A::TradeCancel: if (c.trade() && c.trade()->open) c.trade_cancel(); break;
            case A::LootAll: if (c.loot_session() && c.loot_session()->open) c.loot_all(); break;
            case A::LootDone: if (c.loot_session() && c.loot_session()->open) c.end_loot(); break;
            // The box's question, whichever it is: the ground drop, the server's, a window's own, the guild invite.
            // A script's or plugin's Yes on a destroy's or a guild disband's question is refused, as through the hs
            // calls: only the player answers it (the player's own typed /notify has no caller name).
            case A::ConfirmYes:
                if (refuse_code_destroy_yes(host, !plugin::caller_name().empty())) return false;
                answer_confirmation_question(host, true);
                break;
            case A::ConfirmNo: answer_confirmation_question(host, false); break;
            case A::LargeDialogYes: if (c.large_dialog()) c.answer_large_dialog(1); break;
            case A::LargeDialogNo: if (c.large_dialog()) c.answer_large_dialog(2); break;
            case A::TaskAccept: c.accept_selected_task(); break;
            case A::TaskDecline: if (c.tasks().offers) c.decline_tasks(); break;
            case A::BankAuto: if (c.bank()) c.auto_bank(); break;
            case A::BankDone: if (c.bank()) c.close_bank(); break;
            case A::TributeToggle: c.tribute_toggle(!c.tribute().settings.active); break;
            case A::RespawnSelect: c.choose_selected_respawn(); break;
            case A::RaidLock:
            case A::RaidUnlock: {
                // RAID_LockButton is enabled for the leader of an unlocked raid, RAID_UnlockButton of a locked one.
                const auto& r = c.raid();
                const bool lock = action == A::RaidLock;
                if (r.active && r.leader == c.character_name() && r.locked != lock) c.raid_lock(lock);
                break;
            }
            case A::MerchantBuy: merchant_buy_button(host); break;
            case A::MerchantSell: merchant_sell_button(host); break;
            case A::MerchantDone: if (c.merchant() && c.merchant()->open) c.close_merchant(); break;
            case A::QuantityAccept: quantity_accept(host); break;
            case A::AltCurrencyReclaim: alt_currency_reclaim_button(host); break;
            case A::AltCurrencyCreate: alt_currency_create_button(host); break;
            case A::Money0: case A::Money1: case A::Money2: case A::Money3:
                inventory_money_button(host, static_cast<int>(action) - static_cast<int>(A::Money0));
                break;
            case A::None: break;
            }
            return true;
        }
    if (n.notification == "listselect")
        if (const L list = game::notify_list(n.window, n.child); list != L::None) {
            const int row = n.value - 1;
            switch (list) {
            case L::MerchantItems: merchant_select_row(host, row); break;
            case L::AltCurrencies: alt_currency_select_row(host, row); break;
            case L::TaskOffers: c.select_task_offer(row); break;
            case L::RespawnOptions:
                if (const auto& w = c.respawn_window(); w && row >= 0 && row < static_cast<int>(w->options.size()))
                    c.set_respawn_choice(w->options[static_cast<std::size_t>(row)].id);
                break;
            case L::None: break;
            }
            return true;
        }
    if (n.notification == "newvalue" && game::notify_quantity_slider(n.window, n.child)) {
        quantity_set_slider(host, n.value);
        return true;
    }
    // Anything else (a page's tab, a window E3 does not use): the stock window, as the player's click would.
    if (!api) {
        log::info("/notify: {} {} {}: needs the UI", n.window, n.child, n.notification);
        return true;
    }
    bool done = false;
    // MQ's listselect sets the list's selection. Few stock lists take a selection of their own (list_select); the
    // rest select a row on its click, so that is the fallback. The row is the window's own (StockWindow::list), not
    // a UI pack's sorted or filtered view of it.
    if (n.notification == "listselect")
        done = api->select_list(n.window, n.child, n.value - 1, 0) || api->click_list(n.window, n.child, n.value - 1, 0);
    else if (n.notification == "tabselect") api->set_tab_page(n.window, n.child, n.value - 1), done = true;
    else if (n.notification != "newvalue")
        done = api->click_child(n.window, n.child, n.notification == "rightmouseup" ? "rightup" : "left");
    if (!done) log::info("/notify: {} {} {}: nothing took it", n.window, n.child, n.notification);
    return true;
}

bool run_window_act(UiHost& host, UiApi* api, std::string_view window, std::string_view child, std::string_view action) {
    // A read that acts, held to what /notify needs: a script without 'actions' cannot click or close (a merchant's
    // or trade's DoClose ends it) by reading a member instead of sending the command.
    if ((plugin::caller_caps() & plugin::kCapActions) == 0) {
        if (host.conn)
            host.conn->ui_message(std::format("{}: the window method {} needs the server's 'actions' permission",
                                              plugin::caller_name(), action));
        return false;
    }
    // MQ's SendWndClick2 on a child: the click /notify would send (its game-state actions first, the UI after).
    if (action == "leftmouseup" || action == "rightmouseup") {
        if (child.empty()) return false;
        return run_notify_command(host, api, std::string(window) + " " + std::string(child) + " " + std::string(action));
    }
    // CXWnd::Show(true / false): the stock window's own open state (UiApi's windows are state alone, built with or
    // without anything drawn), which Window[...].Open then reads.
    if (action == "doopen" || action == "doclose") return api && api->show_window(window, action == "doopen");
    return false;
}

} // namespace hailstone::app
