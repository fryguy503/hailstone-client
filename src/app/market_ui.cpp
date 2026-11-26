#include "app/market_ui.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <imgui.h>

#include "core/env.h"
#include "game/bazaar.h"
#include "game/mail.h"
#include "net/game_connection.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

MarketUi::MarketUi() {
    if (const char* w = hailstone::core::env("MAIL_WINDOW")) mail_open_ = *w && *w != '0';
}

void MarketUi::draw(net::GameConnection& conn, bool dev_windows) {
    if (conn.state() != net::ConnectionState::InZone) return;
    // Each window here is off while its stock Lua window (ui/<pack>) stands in for it.
    using ui::lua::replaces;

    if (const auto& v = conn.inspect_view(); v && !replaces("InspectWnd")) {
        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(("Inspect: " + v->player_name + "###market_inspect").c_str(), &open)) {
            for (std::size_t i = 0; i < v->item_names.size(); ++i)
                if (!v->item_names[i].empty()) ImGui::Text("%2zu  %s", i, v->item_names[i].c_str());
            ImGui::Separator();
            ImGui::TextWrapped("%s", v->text.c_str());
        }
        ImGui::End();
        if (!open) conn.close_inspect();
    }

    if (const auto& b = conn.book(); b && !replaces(b->type == 0 ? "NoteWindow" : "BookWindow")) {
        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Book###market_book", &open)) ImGui::TextWrapped("%s", b->text.c_str());
        ImGui::End();
        if (!open) conn.close_book();
    }

    if (conn.duel_state().pending() && dev_windows) { // no stock window: the developer windows only
        const auto& d = conn.duel_state();
        if (ImGui::Begin("Duel###market_duel")) {
            ImGui::Text("challenged %u, challenger %u, %s", d.challenged, d.challenger, d.active ? "fighting" : "pending");
            if (d.challenger && !d.active && ImGui::Button("Decline")) conn.decline_duel();
        }
        ImGui::End();
    }

    if (conn.trader_state().window_open && !replaces("BazaarWnd")) draw_trader(conn);
    if (conn.bazaar_search_open() && !replaces("BazaarSearchWnd")) draw_search(conn);
    if (conn.barter_merchant().buyer_id && !replaces("BarterMerchantWnd")) {
        bool open = true;
        if (ImGui::Begin("Buyer###market_barter", &open))
            for (const auto& l : conn.barter_merchant().lines)
                ImGui::Text("%u: %s x%u for %s", l.slot, l.item_name.c_str(), l.quantity, game::coin_compact(l.cost).c_str());
        ImGui::End();
        if (!open) conn.close_buyer();
    }
    if (replaces("MailWindow")) return;
    if (!conn.mail().headers.empty() || conn.mail().window_open) mail_open_ = true;
    if (mail_open_) draw_mail(conn);
}

void MarketUi::draw_confirmation_fallback(net::GameConnection& conn) {
    if (conn.state() != net::ConnectionState::InZone) return;
    const auto& c = conn.confirmation();
    if (!c || ui::lua::replaces("ConfirmationDialogBox")) return;
    if (ImGui::Begin("Confirm###market_confirm")) {
        ImGui::TextWrapped("%s", c->text.c_str());
        if (c->ok_only) {
            if (ImGui::Button("OK")) conn.answer_confirmation(false);
        } else {
            if (ImGui::Button("Yes")) conn.answer_confirmation(true);
            ImGui::SameLine();
            if (ImGui::Button("No")) conn.answer_confirmation(false);
        }
    }
    ImGui::End();
}

void MarketUi::draw_trader(net::GameConnection& conn) {
    const auto& t = conn.trader_state();
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(460, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Bazaar trader###market_trader", &open)) {
        for (std::size_t i = 0; i < t.slots.size(); ++i) {
            const auto& s = t.slots[i];
            bool sel = t.selected == static_cast<int>(i);
            std::string label = s.name + " x" + std::to_string(s.count) + "  " + game::coin_compact(s.price) + (s.active ? "  *" : "");
            if (ImGui::Selectable((label + "##ts" + std::to_string(i)).c_str(), sel)) {
                conn.trader_select(static_cast<int>(i));
                auto p = game::split_price(t.entered_price);
                for (int k = 0; k < 4; ++k) price_[k] = static_cast<int>(p[static_cast<std::size_t>(k)]);
            }
        }
        ImGui::InputInt4("p g s c", price_);
        conn.trader_enter_price(game::join_price(static_cast<std::uint32_t>(std::max(0, price_[0])), static_cast<std::uint32_t>(std::max(0, price_[1])),
                                                 static_cast<std::uint32_t>(std::max(0, price_[2])), static_cast<std::uint32_t>(std::max(0, price_[3]))));
        if (ImGui::Button("Set Price")) conn.trader_set_price();
        ImGui::SameLine();
        if (ImGui::Button("Clear")) conn.trader_clear_price();
        if (t.trader) {
            ImGui::SameLine();
            if (ImGui::Button("Add")) conn.trader_add();
            ImGui::SameLine();
            if (ImGui::Button("Remove")) conn.trader_remove();
            ImGui::SameLine();
            if (ImGui::Button("List")) conn.trader_list();
            ImGui::SameLine();
            if (ImGui::Button("End Trader")) conn.trader_end();
        } else {
            ImGui::SameLine();
            if (ImGui::Button("Begin Trader")) conn.trader_start();
        }
        ImGui::Separator();
        for (const auto& l : t.log) ImGui::TextWrapped("%s", l.text.c_str());
    }
    ImGui::End();
    if (!open) conn.close_trader_window();
}

void MarketUi::draw_search(net::GameConnection& conn) {
    const auto& s = conn.bazaar_search_state();
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(520, 460), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Bazaar search###market_search", &open)) {
        ImGui::InputText("Name", search_name_, sizeof search_name_);
        ImGui::SameLine();
        if (ImGui::Button("Find")) {
            net::BazaarQuery q;
            q.name = search_name_;
            conn.bazaar_search(q);
        }
        ImGui::SameLine();
        if (ImGui::Button("Welcome")) conn.bazaar_welcome();
        ImGui::Text("%zu traders, %zu results%s", s.traders.size(), s.results.size(), s.searching ? " (searching)" : "");
        for (std::size_t i = 0; i < s.results.size(); ++i) {
            const auto& r = s.results[i];
            auto t = s.traders.find(r.trader_id);
            ImGui::Text("%s x%u  %s  %s", r.name.c_str(), r.quantity, game::coin_compact(r.cost).c_str(),
                        t == s.traders.end() ? "?" : t->second.name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(("Inspect##bi" + std::to_string(i)).c_str())) conn.bazaar_inspect(i);
            ImGui::SameLine();
            if (ImGui::SmallButton(("Buy##bb" + std::to_string(i)).c_str())) conn.bazaar_buy(i, 1, false);
        }
    }
    ImGui::End();
    if (!open) conn.close_bazaar_search();
}

void MarketUi::draw_mail(net::GameConnection& conn) {
    const auto& m = conn.mail();
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Mail###market_mail", &open)) {
        if (!m.window_open && ImGui::Button("Open mailbox")) conn.mail_open();
        ImGui::Text("mailbox %d of %zu, %zu messages", m.current, m.mailboxes.size(), m.headers.size());
        for (const auto* h : m.inbox()) {
            char when[32] = {};
            std::time_t t = static_cast<std::time_t>(h->timestamp);
            std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&t));
            if (ImGui::Selectable((game::mail_short_name(h->from) + ": " + h->subject + "  " + when + "##m" + std::to_string(h->id)).c_str(),
                                  mail_selected_ == h->id)) {
                mail_selected_ = h->id;
                conn.mail_request_body(h->id);
            }
        }
        if (auto it = m.headers.find(mail_selected_); it != m.headers.end()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", it->second.body ? it->second.body->c_str() : "(requesting...)");
            if (ImGui::Button("Delete")) conn.mail_delete({mail_selected_});
        }
        ImGui::Separator();
        ImGui::InputText("To", mail_to_, sizeof mail_to_);
        ImGui::InputText("Subject", mail_subject_, sizeof mail_subject_);
        ImGui::InputTextMultiline("Body", mail_body_, sizeof mail_body_, ImVec2(-1, 80));
        if (ImGui::Button("Send")) conn.mail_send(mail_to_, mail_subject_, mail_body_);
    }
    ImGui::End();
    if (!open) {
        mail_open_ = false;
        conn.mail_close();
    }
}

} // namespace hailstone::app
