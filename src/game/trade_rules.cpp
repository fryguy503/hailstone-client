#include "game/trade_rules.h"

#include <cstdlib>

#include "game/world_state.h"

namespace hailstone::game {

namespace {
// The amount on the cursor for its coin type minus one: the profile's for 0..3, 0 for any other type.
std::int32_t cursor_amount(const std::array<std::int32_t, 4>& cursor, std::uint32_t index) {
    return index <= 3 ? cursor[index] : 0;
}
} // namespace

bool refuses_direct_items(const Spawn& spawn) {
    if (!spawn.is_npc) return false;
    switch (spawn.class_id) {
    case Class_Banker:
    case Class_GuildBanker:
    case Class_TributeMaster:
    case Class_GuildTributeMaster:
    case Class_TributeMaster2:
        return true;
    default:
        return false;
    }
}

void trade_set_his_ready(TradeFlags& f, std::uint8_t ready) {
    f.his_ready = ready;
    if (ready == 0) f.my_ready = 0;
}

bool trade_item_moved(TradeFlags& f, std::int32_t from_location, std::int32_t to_location) {
    if (from_location != 3 && to_location != 3) return false;
    f = {};
    return true;
}

bool trade_clicked_money_button(TradeFlags& f, std::uint8_t type, const std::array<std::int32_t, 4>& cursor,
                                std::uint32_t coin, TradeMoneyMove& move) {
    if (type == 0) return false;
    std::uint32_t from = static_cast<std::uint32_t>(type) - 1;
    move = {from, coin, cursor_amount(cursor, from)};
    f = {};
    return true;
}

bool trade_drop_cursor_money(TradeFlags& f, std::uint8_t type, const std::array<std::int32_t, 4>& cursor,
                             TradeMoneyMove& move) {
    if (type == 0) return false;
    std::uint32_t slot = static_cast<std::uint32_t>(type) - 1;
    move = {slot, slot, cursor_amount(cursor, slot)};
    f = {};
    return true;
}

bool is_ok_to_transact(const TransactState& s) {
    if (s.vehicle) return s.vehicle_allows;
    if (s.respawn_timer != 0 || s.busy_ddf8a0 != 0) return false;
    if (s.currently_interacting > 0 || s.item_pending > 0) return false;
    if (s.container_manager && s.container_open != 0) return false;
    for (const auto& w : s.windows)
        if (w && *w) return false;
    return true;
}

std::int32_t trade_setting(const std::optional<std::string>& eqclient, const std::optional<std::string>& defaults) {
    const std::optional<std::string>& v = eqclient ? eqclient : defaults;
    if (!v) return kTradeEveryone;
    return static_cast<std::int32_t>(std::strtol(v->c_str(), nullptr, 10));
}

TradeRequestResult handle_trade_request(const TradeRequestInput& in) {
    using F = TradeRequestCall::Fn;
    TradeRequestResult r;
    if (!in.requester_spawn) return r;
    std::uint8_t reason = 0;
    bool refuse = false;
    if (in.setting == kTradeNoOne) {
        reason = 'b';
        refuse = true;
    } else if (in.setting == kTradeGroupOnly) {
        reason = 'c';
        if (!in.pc || !in.group) {
            refuse = true;
        } else {
            r.calls.push_back({F::GroupMemberLookup, in.requester_in_group ? 1 : 0});
            if (!in.requester_in_group) refuse = true;
        }
    }
    if (!refuse) {
        r.calls.push_back({F::IsOkToTransact, in.ok_to_transact ? 1 : 0});
        if (!in.ok_to_transact) r.calls.push_back({F::PendingRequestReport, 0x40});
        r.calls.push_back({F::IsOkToTransact, in.ok_to_transact ? 1 : 0});
        if (in.ok_to_transact && !in.attacking) {
            r.calls.push_back({F::TradeWindowActivate, 0});
            r.open_trade = true;
            return r;
        }
        reason = 1;
    }
    r.refusal = TradeRefusal{in.requester_id, in.my_spawn_id, reason};
    if (!in.world_connection) {
        if (!in.quiet_debug) r.calls.push_back({F::DebugLog, static_cast<std::int32_t>(kOpTradeRequestAck)});
    } else {
        r.calls.push_back({F::Send, 14});
        if (!in.send_ok) r.calls.push_back({F::CloseWorldConnection, 0});
    }
    r.calls.push_back({F::FormatMessage, static_cast<std::int32_t>(kStrTradeRefused)});
    r.calls.push_back({F::Chat, 0x111});
    return r;
}

} // namespace hailstone::game
