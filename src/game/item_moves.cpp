#include "game/item_moves.h"

#include <limits>

#include "net/item_packets.h"

namespace hailstone::game {

namespace {

std::int32_t wrap(std::uint32_t v) { return static_cast<std::int32_t>(v); }
std::uint32_t u(std::int32_t v) { return static_cast<std::uint32_t>(v); }

bool two_handed(const EquipItem& i) { return i.item_type == 1 || i.item_type == 4 || i.item_type == 35; }
bool instrument(const EquipItem& i) {
    return i.item_type >= 0 && i.item_type <= std::numeric_limits<std::uint8_t>::max() &&
           net::is_instrument_item_type(static_cast<std::uint8_t>(i.item_type));
}

} // namespace

// ---------------------------------------------------------------- equip slots

bool item_is_weapon(std::int32_t item_type, std::int32_t damage) {
    if (item_type == 27 || item_type == 28 || item_type == 43 || item_type == 44) return false;
    return damage > 0;
}

bool inv_slot_available(std::int32_t slot, std::uint32_t slot_mask, std::uint32_t expansions, std::int32_t last_usable) {
    if (slot < 0 || slot > 32) return false;
    if (slot >= 23) return slot <= last_usable;
    if ((slot_mask & (1u << slot)) == 0) return false;
    return slot != 21 || (expansions & 0x1000u) != 0;
}

bool can_item_go_in_equip_slot(std::int32_t slot, const EquipItem* item, bool slot_available, const EquipItem* primary,
                               const EquipItem* secondary, bool dual_wield) {
    if (!item || !slot_available) return false;
    if (u(slot) - 23u <= 9u) return true;
    if (item->item_class == 1) return false;
    if (item->item_class == 2 || item->item_type == 54) return false;
    if (slot == 11) {
        if (item->item_type == 27 || item->item_type == 28) return false;
        if (item->range > 0) return true;
    } else if (slot == 22) {
        if (item->item_type == 19 || item->item_type == 27 || item->item_type == 28) return true;
    }
    if ((item->equip_slots & (1u << (u(slot) & 31u))) == 0) return false;
    if (slot == 13) {
        if (secondary) {
            if (two_handed(*item)) return false;
            if (instrument(*secondary) && instrument(*item)) return false;
        }
    } else if (slot == 14) {
        if (two_handed(*item)) return false;
        if (item->weapon && !dual_wield) return false;
        if (primary && ((instrument(*primary) && instrument(*item)) || two_handed(*primary))) return false;
    }
    return true;
}

// ---------------------------------------------------------------- use checks

std::int32_t item_race_bit_of(std::int32_t race) {
    if (race < 13) return race;
    // Iksar, Vah Shir, Froglok and Drakkin take the bits after the twelve
    // classic races; any other race the one after them.
    static constexpr std::int32_t kLaterRaces[] = {128, 130, 330, 522};
    std::int32_t bit = 13;
    for (std::int32_t later : kLaterRaces) {
        if (later == race) return bit;
        ++bit;
    }
    return bit;
}

bool item_mask_has_bit(std::uint32_t mask, std::int32_t bit) { return (mask & (1u << ((u(bit) - 1u) & 31u))) != 0; }

namespace {
bool refuse(bool tell, bool spawn_is_player, std::uint32_t id, std::uint32_t* message) {
    if (message) *message = tell && spawn_is_player ? id : 0;
    return false;
}
bool allow(std::uint32_t* message) {
    if (message) *message = 0;
    return true;
}
} // namespace

bool can_use_item_race(std::int32_t race, std::uint32_t item_races, bool tell, bool spawn_is_player,
                       std::uint32_t* message) {
    if (item_mask_has_bit(item_races, item_race_bit_of(race))) return allow(message);
    return refuse(tell, spawn_is_player, kEqstrCannotUseRace, message);
}

bool can_use_item_deity(bool has_deity, std::int32_t deity, std::uint32_t item_deities, bool tell, bool spawn_is_player,
                        std::uint32_t* message) {
    if (!has_deity || item_deities == 0) return allow(message);
    std::int32_t bit = deity == 396 ? 1 : wrap(u(deity) - 199u);
    if (item_mask_has_bit(item_deities, bit)) return allow(message);
    return refuse(tell, spawn_is_player, kEqstrCannotUseDeity, message);
}

bool can_use_item_level(std::int32_t level, std::int32_t required_level, bool tell, bool spawn_is_player,
                        std::uint32_t* message) {
    if (required_level <= level) return allow(message);
    return refuse(tell, spawn_is_player, kEqstrCannotUseLevel, message);
}

bool can_use_item(std::int32_t item_class, std::uint32_t usable_classes, std::uint32_t class_bits, bool full,
                  std::int32_t item_type, const std::function<bool()>& deity_ok, const std::function<bool()>& level_ok,
                  const std::function<bool()>& race_ok, const std::function<bool()>& prestige_ok) {
    if (item_class != 0) return true;
    bool usable = (usable_classes & class_bits) != 0;
    if (!full || !usable) return usable;
    if (!deity_ok() || !level_ok()) return false;
    if (!race_ok() && item_type != 20) return false;
    return prestige_ok();
}

// ---------------------------------------------------------------- server OP_MoveItem

bool global_index_valid(const GlobalIndex& index) {
    return index.location >= 0 && index.location < 0x18 && index.slot != -1;
}

MoveApply plan_move_item_apply(const GlobalIndex& from, const GlobalIndex& to, bool has_from_item, bool has_to_item,
                               const std::function<bool()>& stacks_with, const std::function<bool()>& from_stackable) {
    if (!has_from_item) return MoveApply::NoItem;
    if (!global_index_valid(to)) {
        if (from.location != 0 && from.location != 1 && from.location != 5) return MoveApply::Refused;
        return MoveApply::Delete;
    }
    if (from.location != 0 || to.location != 0) return MoveApply::WrongLocation;
    if (has_to_item) return stacks_with() ? MoveApply::MergeCount : MoveApply::Swap;
    return from_stackable() ? MoveApply::AdjustCount : MoveApply::Swap;
}

bool move_apply_clears_cursor(const GlobalIndex& from) {
    return from.location == 0 && static_cast<std::uint16_t>(from.slot) == 0x21 && from.sub == -1;
}

std::optional<std::int32_t> move_item_count(std::int32_t count, bool from_stackable, std::int32_t from_count,
                                            std::int32_t to_count, std::int32_t to_max) {
    if (count < 0 || !from_stackable || count > from_count) return std::nullopt;
    std::int32_t room = wrap(u(to_max) - u(to_count));
    if (room <= 0) return std::nullopt;
    return from_count <= room ? from_count : room;
}

StackAdjust adjust_item_stack_count(bool stackable, bool lore, std::int32_t count, std::int32_t delta) {
    StackAdjust a;
    if (!stackable && !(count != 0 && lore)) return a;
    a.applied = true;
    a.new_count = wrap(u(count) + u(delta));
    a.remove = a.new_count < 1;
    return a;
}

// ---------------------------------------------------------------- coin

std::int32_t coin_factor(std::int32_t exponent) {
    if (exponent < 0) return 0; // 10^-2^31 underflows to 0
    if (exponent >= 10) return std::numeric_limits<std::int32_t>::min(); // __ftol2 of a value above INT_MAX
    std::int32_t f = 1;
    for (std::int32_t i = 0; i < exponent; ++i) f *= 10;
    return f;
}

MoneyMoveResult move_money(MoneyState& s, const MoneyEnv& env, std::int32_t from, std::int32_t to,
                           std::uint32_t from_coin, std::uint32_t to_coin, std::int32_t amount, std::uint8_t quiet) {
    MoneyMoveResult r;
    if (amount == 0 || !env.has_local_pc) return r;
    std::int32_t diff = wrap(to_coin - from_coin);
    std::int32_t factor = coin_factor(diff < 0 ? wrap(0u - u(diff)) : diff);
    if (factor != 1) {
        r.asked_bank_window = true;
        if (!env.bank_window_active) return r;
    }
    auto counter = [&](std::int32_t location, std::uint32_t coin) -> std::int32_t* {
        if (coin > 3 || location < 0 || location > 4 || !s.present[static_cast<std::size_t>(location)]) return nullptr;
        return &s.coins[static_cast<std::size_t>(location)][coin];
    };
    std::int32_t* source = counter(from, from_coin);
    std::int32_t* target = nullptr;
    if (to == kMoneyCursor) {
        if (quiet == 0) {
            r.asked_cursor_free = true;
            if (!env.cursor_attachment_free) return r;
        }
        r.played_sound = true;
        target = counter(kMoneyCursor, to_coin);
    } else {
        target = counter(to, to_coin);
        if (to == kMoneyInventory && to_coin <= 3 && env.has_inventory_window) r.cleared_coin_buttons = true;
    }
    if (!source || source == target) return r;
    std::int32_t have = *source;
    if (amount > have) return r;
    // The original faults here without a destination counter or with a factor of 0; the client refuses.
    if (!target || factor == 0) return r;
    if (diff >= 0) {
        if (wrap(u(kCoinCap) - u(*target)) / factor < amount) return r;
        *source = wrap(u(have) - u(amount));
        *target = wrap(u(*target) + u(factor) * u(amount));
    } else {
        if (wrap(u(kCoinCap) - u(*target)) < amount / factor) return r;
        std::int32_t whole = wrap(u(amount) - u(amount % factor));
        *source = wrap(u(have) - u(whole));
        *target = wrap(u(*target) + u(whole / factor));
        if (from == kMoneyCursor && s.attachment_type == 3) s.attachment_count = *source;
    }
    r.wire_from_coin = wrap(from_coin <= 3 ? 3u - from_coin : from_coin);
    r.wire_to_coin = wrap(to_coin <= 3 ? 3u - to_coin : to_coin);
    if (amount > 0) {
        r.send = true;
        s.inventory_changed = 1;
    }
    s.cursor_coin_type = 0;
    for (std::uint32_t coin = 0; coin < 4; ++coin) {
        if (s.coins[kMoneyCursor][coin] > 0) {
            s.cursor_coin_type = static_cast<std::uint8_t>(coin + 1);
            break;
        }
    }
    r.value = 1;
    return r;
}

MoneyButtonOut clicked_money_button(const MoneyButtonIn& in) {
    MoneyButtonOut out;
    if (in.cursor_coin_type != 0) {
        std::uint32_t from = static_cast<std::uint32_t>(in.cursor_coin_type) - 1u;
        out.kind = MoneyButtonOut::Kind::DropCursorCoins;
        out.from = kMoneyCursor;
        out.to = kMoneyInventory;
        out.from_coin = from;
        out.to_coin = u(in.coin);
        out.amount = from <= 3 ? in.cursor_coins[from] : 0;
        return out;
    }
    if (in.amount > -1) {
        out.kind = MoneyButtonOut::Kind::TakeAmount;
        out.from = kMoneyInventory;
        out.to = kMoneyCursor;
        out.from_coin = out.to_coin = u(in.coin);
        out.amount = in.amount;
        return out;
    }
    out.kind = MoneyButtonOut::Kind::AskQuantity;
    out.pending_coin = in.coin;
    std::array<std::int32_t, 4> r{};
    if (in.has_button) r = in.button_rect;
    auto centre = [](std::int32_t low, std::int32_t high) { return wrap(u(wrap(u(high) - u(low)) / 2) + u(low)); };
    out.x = centre(r[0], r[2]);
    out.y = centre(r[1], r[3]);
    return out;
}

} // namespace hailstone::game
