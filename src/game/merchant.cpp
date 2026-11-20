#include "game/merchant.h"

#include <algorithm>
#include <limits>

#include "core/wrap_math.h"
#include "game/item_moves.h"

namespace hailstone::game {

using net::Item;

namespace {

// Two's complement wrap for the original's 32-bit IMUL/ADD.
std::int32_t wrap32(std::uint32_t v) { return static_cast<std::int32_t>(v); }

// The item header's ItemBase fields (the original's
// ItemClient_UnSerializeItemTree reader @0x5553b0): ornament icon → +0x84,
// unknowna1 → +0xf0 ArmorType, ornament hero model → +0x48 NewArmorID,
// unknown063 → +0x138 Tint, unknowna4 → +0x7c RealEstateID.
bool instance_overrides(const Item& it) {
    return it.attuned != 0 || !it.ornament_main.empty() || !it.ornament_off.empty() || it.ornament_icon != 0 ||
           it.unknowna1 >= 0 || it.ornament_hero_model != 0 || it.unknown063 != 0;
}

// ItemContainer find with ItemBase_IsNoRent as the predicate (any depth).
bool has_temporary_content(const Item& it) {
    for (const auto& b : it.contents)
        if (b.item.no_rent == 0 || has_temporary_content(b.item)) return true;
    return false;
}

} // namespace

std::int64_t total_copper(const net::Coins& c) {
    return static_cast<std::int64_t>(c.platinum) * 1000 + static_cast<std::int64_t>(c.gold) * 100 +
           static_cast<std::int64_t>(c.silver) * 10 + static_cast<std::int64_t>(c.copper);
}

bool take_money(net::Coins& coins, std::int64_t amount) {
    // The profile keeps the coins as int32; so does this client.
    auto copper = static_cast<std::int32_t>(coins.copper), silver = static_cast<std::int32_t>(coins.silver),
         gold = static_cast<std::int32_t>(coins.gold), plat = static_cast<std::int32_t>(coins.platinum);
    auto store = [&] {
        coins.copper = static_cast<std::uint32_t>(copper);
        coins.silver = static_cast<std::uint32_t>(silver);
        coins.gold = static_cast<std::uint32_t>(gold);
        coins.platinum = static_cast<std::uint32_t>(plat);
    };
    if (amount < 0) return false;
    // BaseProfile_GetTotalMoneyInCopper: the counts are signed (as credit_copper reads them). Read unsigned, a count of
    // 2^31 or more would pass here and spend as a negative one below.
    const std::int64_t purse = static_cast<std::int64_t>(copper) + static_cast<std::int64_t>(silver) * 10 +
                               static_cast<std::int64_t>(gold) * 100 + static_cast<std::int64_t>(plat) * 1000;
    if (purse < amount) return false;
    if (amount <= 0) return true;
    do {
        // A client guard: a purse that mixes negative and positive counts could still leave nothing to take from; the
        // loop would never end. Nothing is taken then.
        if (copper == 0 && silver == 0 && gold == 0 && plat == 0) return false;
        if (copper != 0) {
            std::int32_t take = static_cast<std::int64_t>(copper) <= amount ? copper : static_cast<std::int32_t>(amount);
            amount -= take;
            copper -= take;
        } else if (silver != 0) {
            if (amount >= wrap32(static_cast<std::uint32_t>(silver) * 10u)) {
                amount -= static_cast<std::int64_t>(silver) * 10;
                silver = 0;
            } else {
                --silver;
                if (amount < 10) {
                    copper += 10 - static_cast<std::int32_t>(amount);
                    store();
                    return true;
                }
                amount -= 10;
            }
        } else if (gold != 0) {
            if (amount >= wrap32(static_cast<std::uint32_t>(gold) * 100u)) {
                amount -= static_cast<std::int64_t>(gold) * 100;
                gold = 0;
            } else {
                --gold;
                if (amount >= 100) amount -= 100;
                else silver += 10;
            }
        } else if (plat != 0) {
            auto whole = static_cast<std::int32_t>(amount / 1000);
            plat -= whole;
            amount -= static_cast<std::int64_t>(whole) * 1000;
            if (amount <= 0) {
                store();
                return true;
            }
            --plat;
            gold += 10;
        }
    } while (amount > 0);
    store();
    return true;
}

std::int32_t item_value(const Item& item) {
    std::uint32_t v = item.value;
    if (item.item_class == 0)
        for (const auto& b : item.contents) v += static_cast<std::uint32_t>(item_value(b.item));
    return wrap32(v);
}

bool is_droppable(const Item& item, bool recursive, bool no_real_estate) {
    if (item.no_drop == 0 || instance_overrides(item)) return false;
    if (no_real_estate && item.unknowna4 != -1) return false; // +0x7c RealEstateID
    if (recursive)
        for (const auto& b : item.contents)
            if (!is_droppable(b.item, true, no_real_estate)) return false;
    return true;
}

namespace {
// ItemBase_IsFVNoDrop(recursive, no_real_estate) @0x7b3bc0.
bool is_fv_no_drop(const Item& item, bool recursive, bool no_real_estate) {
    if (item.fv_no_drop != 0) return true;
    if (no_real_estate && item.unknowna4 != -1) return true;
    if (recursive)
        for (const auto& b : item.contents)
            if (is_fv_no_drop(b.item, true, no_real_estate)) return true;
    return false;
}
} // namespace

CanDropResult can_drop(const Item& item, bool fv_rules, bool contents, bool no_real_estate) {
    // ItemBase_CanDropImpl @0x7b5090: setne on ItemClass (+0x11a) == 1 picks 2 (a bag) or 3.
    auto content_result = [&] {
        return item.item_class == 1 ? CanDropResult::BagContentNoTrade : CanDropResult::AugmentNoTrade;
    };
    if (fv_rules) {
        if (!is_fv_no_drop(item, contents, no_real_estate)) return CanDropResult::Droppable;
        if (!contents || item.fv_no_drop != 0 || (no_real_estate && item.unknowna4 != -1)) return CanDropResult::NoTrade;
        return content_result();
    }
    if (is_droppable(item, contents, no_real_estate)) return CanDropResult::Droppable;
    if (contents && is_droppable(item, false, no_real_estate)) return content_result();
    return CanDropResult::NoTrade;
}

std::uint32_t can_drop_message(CanDropResult r) {
    switch (r) {
    case CanDropResult::NoTrade: return 12921;
    case CanDropResult::BagContentNoTrade: return 12922;
    case CanDropResult::AugmentNoTrade: return 5293;
    case CanDropResult::Droppable: break;
    }
    return 0;
}

std::int32_t sell_value(const Item& item, float rate, std::int32_t count) {
    bool rentable = item.no_rent != 0; // NoRent 0 = Temporary
    if ((item.item_class == 1 || item.item_class == 0) && (!rentable || has_temporary_content(item))) return 0;
    if (!rentable || item.no_drop == 0) return 0;
    if (instance_overrides(item) || item.unknowna4 != -1) return 0; // +0x7c RealEstateID
    for (const auto& b : item.contents)
        if (!is_droppable(b.item)) return 0;
    std::int32_t value = item_value(item);
    if (value <= 0 || rate == 0.0f) return 0;
    // The original truncates value / rate + 0.5; kept as is. A result past int32's range or NaN (a tiny, negative
    // tiny or NaN rate from the server) is undefined to cast in C++; the x86 conversion the original's compiler
    // emits (cvttsd2si, as the x87 fistp) gives the "integer indefinite" 0x80000000 for it, so that is what it is.
    const double unit = static_cast<double>(value) / static_cast<double>(rate) + 0.5;
    const std::int32_t each = unit > -2147483649.0 && unit < 2147483648.0
                                  ? static_cast<std::int32_t>(unit) // NOLINT(bugprone-incorrect-roundings)
                                  : std::numeric_limits<std::int32_t>::min();
    std::int32_t price = wrap32(static_cast<std::uint32_t>(each) * static_cast<std::uint32_t>(count));
    if (price >= 10000000) price = core::wrapped_div(static_cast<std::uint32_t>(price) + 50u, 100) * 100;
    return price;
}

std::string money_words(std::int64_t copper) {
    std::int64_t plat = copper / 1000;
    std::int64_t rest = copper - plat * 1000;
    std::int64_t gold = rest / 100;
    rest -= gold * 100;
    std::int64_t silver = rest / 10;
    std::int64_t cp = rest - silver * 10;
    std::string s;
    auto add = [&](std::int64_t n, const char* what) {
        if (n >= 1) s += " " + std::to_string(n) + " " + what;
    };
    add(plat, "platinum");
    add(gold, "gold");
    add(silver, "silver");
    add(cp, "copper");
    return s;
}

std::uint32_t sell_refusal(const Item& item) {
    if (!is_droppable(item)) return 12062;
    if (item.is_bag() && !item.contents.empty()) return 12064;
    if (item.item_class == 0 && !item.contents.empty()) return 5360; // augmented
    return 0;
}

void credit_copper(net::Coins& coins, std::int64_t credit) {
    auto wrap = [](std::int64_t a, std::int64_t b) { // 64-bit wrapping arithmetic, as __allmul and the adds
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
    };
    auto signed32 = [](std::uint32_t v) { return static_cast<std::int64_t>(static_cast<std::int32_t>(v)); };
    // BaseProfile_GetTotalMoneyInCopper: the counts are signed.
    std::int64_t carried = signed32(coins.platinum) * 1000 + signed32(coins.gold) * 100 + signed32(coins.silver) * 10 +
                           signed32(coins.copper);
    std::int64_t total = wrap(credit, carried);
    std::int64_t platinum = total / 1000;
    std::int64_t gold = wrap(total, -(platinum * 1000)) / 100;
    std::int64_t silver = wrap(total, -((total / 100) * 100)) / 10;
    std::uint32_t copper = static_cast<std::uint32_t>(total) - static_cast<std::uint32_t>(total / 10) * 10;
    coins.platinum = static_cast<std::uint32_t>(platinum);
    coins.gold = static_cast<std::uint32_t>(gold);
    coins.silver = static_cast<std::uint32_t>(silver);
    coins.copper = copper;
}

void server_add_copper(net::Coins& coins, std::uint64_t copper) {
    coins.platinum += static_cast<std::uint32_t>(copper / 1000);
    coins.gold += static_cast<std::uint32_t>(copper % 1000 / 100);
    coins.silver += static_cast<std::uint32_t>(copper % 100 / 10);
    coins.copper += static_cast<std::uint32_t>(copper % 10);
}

std::string merchant_quantity_text(std::int32_t quantity) {
    return quantity < 1 ? std::string("--") : std::to_string(quantity);
}

std::int32_t merchant_list_level(const net::Item& item, const std::function<std::optional<std::int32_t>()>& spell_level) {
    if (item.item_type == 20) return spell_level().value_or(-1);
    auto level = static_cast<std::int32_t>(item.req_level);
    if (item.item_class == 0)
        for (const auto& c : item.contents)
            if (c.index < 6) level = std::max(level, static_cast<std::int32_t>(c.item.req_level));
    return level;
}

std::string merchant_level_text(std::int32_t level) { return level < 2 ? std::string("--") : std::to_string(level); }

bool merchant_usable_hides(const net::Item& item, std::uint32_t class_id, std::uint32_t race,
                           const std::function<bool()>& scroll_known, std::uint32_t server_classes) {
    if (item.item_class != 0) return false;
    // UpdateList: 1 << ((class - 1) & 31) against GetUsableClasses(item, 0, 0), 1 << ((0x6f7160 - 1) & 31) against
    // ItemBase_GetRaces(item, 0, 0). Hailstone extension: a server that gives the player several classes
    // (server_classes, 0 on a stock server) has any of them match.
    if (server_classes != 0 ? (item.classes & server_classes) == 0
                            : !item_mask_has_bit(item.classes, static_cast<std::int32_t>(class_id)))
        return true;
    if (!item_mask_has_bit(item.races, item_race_bit_of(static_cast<std::int32_t>(race)))) return true;
    return item.item_type == net::kItemTypeScroll && scroll_known();
}

} // namespace hailstone::game
