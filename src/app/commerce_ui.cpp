#include "app/commerce_ui.h"

#include <cstdlib>
#include <glm/geometric.hpp>
#include <imgui.h>

#include "core/env.h"
#include "core/distribution.h"
#include "core/log.h"
#include "game/merchant.h"
#include "game/world_state.h"
#include "net/game_connection.h"

namespace hailstone::app {

using Clock = std::chrono::steady_clock;

CommerceUi::CommerceUi() {
    if (!distribution::managed())
        if (const char* t = hailstone::core::env("COMMERCE_TEST")) test_ = t;
}

void CommerceUi::draw(net::GameConnection& conn, ui::ItemIconCache* icons) {
    if (conn.state() != net::ConnectionState::InZone) return;
    if (!test_.empty()) drive_test(conn);
    // The HAIL key is the key map's (app/client_app.cpp); /hail is GameConnection::hail().
    ui::CommerceWindowsInput in;
    in.inventory = &conn.inventory();
    in.coins = conn.coins();
    if (const auto& m = conn.merchant()) in.merchant = &*m;
    if (const auto& t = conn.trade()) in.trade = &*t;
    if (const auto& b = conn.bank()) in.bank = &*b;
    in.bank_coins = conn.bank_coins();
    if (const auto& q = conn.quantity_request()) in.quantity = &*q;
    const auto& world = conn.world();
    if (world.target_id && world.target_id != world.player_id) {
        auto it = world.spawns.find(world.target_id);
        if (it != world.spawns.end() && !it->second.is_corpse) {
            in.target_id = world.target_id;
            in.target_name = game::display_name(it->second.name);
        }
    }
    ui::CommerceWindowsActions act = ui::commerce_windows(state_, in, icons);
    if (act.buy) conn.merchant_buy(act.buy->first, act.buy->second);
    if (act.sell) conn.merchant_sell(act.sell->first, act.sell->second);
    if (act.close_merchant) conn.close_merchant();
    if (act.trade_drop) conn.trade_drop_cursor();
    if (act.trade_coin) conn.trade_coin(act.trade_coin->first, act.trade_coin->second);
    if (act.trade_accept) conn.trade_accept();
    if (act.trade_cancel) conn.trade_cancel();
    if (act.give_to) conn.give_to(*act.give_to);
    if (act.bank_click) conn.click_slot(*act.bank_click, ImGui::GetIO().KeyShift, ImGui::GetIO().KeyCtrl);
    if (act.deposit) conn.bank_deposit(act.deposit->first, act.deposit->second, act.shared_coin);
    if (act.withdraw) conn.bank_withdraw(act.withdraw->first, act.withdraw->second, act.shared_coin);
    if (act.auto_bank) conn.auto_bank();
    if (act.close_bank) conn.close_bank();
    if (act.quantity_accept) conn.finish_quantity(*act.quantity_accept);
    if (act.quantity_cancel) conn.cancel_quantity();
}

void CommerceUi::drive_test(net::GameConnection& conn) {
    if (distribution::managed()) return;
    auto now = Clock::now();
    if (test_since_ == Clock::time_point{}) test_since_ = now;
    if (now - test_since_ < std::chrono::seconds(5)) return;
    const auto& world = conn.world();
    const game::Spawn* me = world.player();
    if (!me) return;
    // The nearest NPC (merchants only when asked) within 20 units.
    auto nearest = [&](bool merchant, std::uint8_t cls = 41) -> std::uint32_t {
        std::uint32_t best = 0;
        float best_d = 20.0f;
        for (const auto& [id, s] : world.spawns) {
            if (!s.is_npc || s.is_corpse || id == world.player_id || (merchant && s.class_id != cls)) continue;
            float d = glm::distance(s.position, me->position);
            if (d < best_d) {
                best_d = d;
                best = id;
            }
        }
        return best;
    };
    if (test_ == "merchant") {
        if (test_step_ == 0) {
            if (std::uint32_t id = nearest(true)) {
                conn.set_target(id);
                conn.open_merchant(id);
                log::info("commerce test: opening merchant {}", id);
            } else {
                log::warn("commerce test: no merchant within reach");
            }
            test_step_ = 1;
        } else if (test_step_ == 1 && conn.merchant() && conn.merchant()->open && !conn.merchant()->items.empty() &&
                   now - test_since_ > std::chrono::seconds(7)) {
            state_.selected_buy = conn.merchant()->items.begin()->first;
            for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s) {
                const net::Item* it = conn.inventory().at(net::ItemSlot::possessions(s));
                if (it && !it->is_bag()) {
                    conn.merchant_select(net::ItemSlot::possessions(s));
                    break;
                }
            }
            test_step_ = 2;
        }
        return;
    }
    if (test_ == "bank") {
        if (test_step_ == 0) {
            if (std::uint32_t id = nearest(true, 40)) {
                conn.set_target(id);
                conn.open_bank(id);
                log::info("commerce test: opening bank {}", id);
            } else {
                log::warn("commerce test: no banker within reach");
            }
            test_step_ = 1;
        } else if (test_step_ == 1 && now - test_since_ > std::chrono::seconds(7)) {
            for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s) {
                const net::Item* it = conn.inventory().at(net::ItemSlot::possessions(s));
                if (it && it->stackable() && it->stack_count > 1) {
                    conn.click_slot(net::ItemSlot::possessions(s));
                    break;
                }
            }
            test_step_ = 2;
        }
        return;
    }
    if (test_.rfind("give", 0) == 0) {
        bool press = test_.rfind("give!:", 0) == 0;
        auto id = static_cast<std::uint32_t>(std::strtoul(test_.c_str() + (press ? 6 : 5), nullptr, 10));
        if (test_step_ == 0) {
            for (const auto& [key, item] : conn.inventory().items()) {
                if (key.first != net::kInvPossessions || key.second < net::kSlotGeneral1 || key.second > net::kSlotGeneral10)
                    continue;
                if (item.id == id) {
                    conn.click_slot(net::ItemSlot::possessions(key.second));
                    break;
                }
            }
            test_step_ = 1;
        } else if (test_step_ == 1 && now - test_since_ > std::chrono::seconds(6)) {
            if (std::uint32_t npc = nearest(false)) {
                conn.set_target(npc);
                conn.give_to(npc);
                log::info("commerce test: giving to {}", npc);
            } else {
                log::warn("commerce test: no NPC within reach");
            }
            test_step_ = 2;
        } else if (test_step_ == 2 && press && conn.trade() && conn.trade()->open &&
                   now - test_since_ > std::chrono::seconds(9)) {
            conn.trade_accept();
            test_step_ = 3;
        }
    }
}

} // namespace hailstone::app
