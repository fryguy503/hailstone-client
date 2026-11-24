#include "game/bazaar.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <limits>

#include "core/msvc_atoi.h"
#include "game/spells.h"

namespace hailstone::game {

std::string trader_price_key(const net::Item& item) {
    // CBazaarWnd_ReadTraderItemFromIni @0x63a6c0: IsValidSpellID(GetSpellID(item, 0)) and charges >= 1.
    const auto& click = item.effects[net::kEffectClick];
    auto charges = static_cast<std::int32_t>(item.charges);
    if (is_spell_id(static_cast<std::uint32_t>(click.effect)) && charges >= 1)
        return std::format("{}:{}", item.id, charges);
    return std::to_string(item.id);
}

void TraderPrices::load(const std::filesystem::path& path) {
    path_ = path;
    ini_ = {};
    ini_.load(path);
}

bool TraderPrices::save() const { return !path_.empty() && ini_.save(path_); }

std::optional<std::uint32_t> TraderPrices::get(const net::Item& item) const {
    const std::string key = trader_price_key(item);
    auto v = ini_.get("ItemToSell", key);
    // Older versions saved one item-wide price for charged spells above the old 16-bit cutoff.
    if (!v && item.effects[net::kEffectClick].effect > 0xffff && key.find(':') != std::string::npos)
        v = ini_.get("ItemToSell", std::to_string(item.id));
    if (!v || v->empty() || (*v)[0] == '*') return std::nullopt;
    return static_cast<std::uint32_t>(core::msvc_atoi(*v));
}

void TraderPrices::set(const net::Item& item, std::uint32_t copper) {
    // CBazaarWnd::sub_00638f70: "%d" of the price under the item's key.
    ini_.set("ItemToSell", trader_price_key(item), std::to_string(copper));
}

TraderLoad load_trader_items(const Inventory& inventory, const TraderPrices& prices) {
    TraderLoad out;
    for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s) {
        const net::Item* bag = inventory.at(net::ItemSlot::possessions(s));
        if (!bag || !bag->is_bag() || bag->bag_type != kBagTraderSatchel) continue;
        out.has_satchel = true;
        for (std::int16_t sub = 0; sub < static_cast<std::int16_t>(bag->bag_slots); ++sub) {
            const net::Item* it = inventory.at(net::ItemSlot::possessions(s, sub));
            if (!it || out.slots.size() >= static_cast<std::size_t>(net::kTraderSlots)) continue;
            TraderSlot t;
            t.where = net::ItemSlot::possessions(s, sub);
            t.serial = it->serial;
            t.item_id = it->id;
            t.name = it->name;
            t.icon = it->icon;
            t.count = it->stackable() ? it->stack_count : 1;
            t.price = prices.get(*it).value_or(0);
            out.slots.push_back(std::move(t));
        }
    }
    return out;
}

std::array<std::uint32_t, 4> split_price(std::uint32_t copper) {
    return {copper / 1000, (copper % 1000) / 100, (copper % 100) / 10, copper % 10};
}

std::uint32_t join_price(std::uint32_t platinum, std::uint32_t gold, std::uint32_t silver, std::uint32_t copper) {
    // In 64 bits and held at the largest 32-bit price: a field too large wrapped to some smaller price.
    const std::uint64_t v = std::uint64_t{copper} + (std::uint64_t{silver} + (std::uint64_t{gold} + std::uint64_t{platinum} * 10) * 10) * 10;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(v, std::numeric_limits<std::uint32_t>::max()));
}

std::string coin_compact(std::int64_t copper) {
    std::string s;
    std::int64_t v = copper;
    if (v >= 1000) {
        s += std::to_string(v / 1000) + "p";
        v -= (v / 1000) * 1000;
    }
    if (v < 0) {
        if (!s.empty()) return s;
    } else {
        if (v > 99) {
            if (!s.empty()) s += ' ';
            s += std::to_string(v / 100) + "g";
            v -= (v / 100) * 100;
        }
        if (v > 9) {
            if (!s.empty()) s += ' ';
            s += std::to_string(v / 10) + "s";
            v -= (v / 10) * 10;
        }
        if (v < 1 && !s.empty()) return s;
        if (!s.empty()) s += ' ';
    }
    s += std::to_string(v) + "c";
    return s;
}

TraderSelection select_trader_slot(const std::vector<TraderSlot>& slots, const Inventory& inventory, int slot) {
    TraderSelection out;
    if (slot < 0 || slot >= static_cast<int>(slots.size())) return out;
    const TraderSlot& at = slots[static_cast<std::size_t>(slot)];
    const net::Item* item = inventory.at(at.where);
    if (!item) return out;
    out.selected = slot;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const TraderSlot& s = slots[i];
        const net::Item* other = inventory.at(s.where);
        if (!other) continue;
        bool same = (!s.serial.empty() && s.serial == at.serial) || (other->id == item->id && other->charges == item->charges);
        if (!same) continue;
        out.selected = static_cast<int>(i);
        out.price = s.price;
        break;
    }
    return out;
}

} // namespace hailstone::game
