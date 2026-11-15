#include "game/item_options.h"

#include <cctype>
#include <charconv>

namespace hailstone::game {

namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// SameNoCase: the whole strings, without case.
bool same_no_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

// Utf8StrNICmp(a, b, n) == 0: the first n characters without case, a string's end counting as a character.
bool starts_no_case(std::string_view a, std::string_view b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        char x = i < a.size() ? lower(a[i]) : '\0', y = i < b.size() ? lower(b[i]) : '\0';
        if (x != y) return false;
        if (x == '\0') return true;
    }
    return true;
}

} // namespace

DestroyResult destroy_held_decision(const DestroyInput& in) {
    DestroyResult r;
    if (in.item && in.satchel_in_trader) {
        r.decision = DestroyDecision::SatchelRefused;
        return r;
    }
    if (in.item && in.real_estate) {
        r.decision = DestroyDecision::Confirm;
        r.eqstr = kStrDestroyRealEstate;
        return r;
    }
    if (in.fast_destroy) {
        r.decision = in.item || in.money ? DestroyDecision::DestroyNow : DestroyDecision::Nothing;
        return r;
    }
    if (in.item) {
        r.decision = DestroyDecision::Confirm;
        r.eqstr = kStrDestroyItem;
    } else if (in.money) {
        r.decision = DestroyDecision::Confirm;
        r.eqstr = kStrDestroyMoney;
    }
    return r;
}

std::optional<std::uint32_t> destroy_cursor_item_id(std::string_view argument) {
    while (!argument.empty() && std::isspace(static_cast<unsigned char>(argument.front()))) argument.remove_prefix(1);
    while (!argument.empty() && std::isspace(static_cast<unsigned char>(argument.back()))) argument.remove_suffix(1);
    if (argument.empty()) return std::nullopt;
    std::uint32_t id = 0;
    const auto [end, error] = std::from_chars(argument.data(), argument.data() + argument.size(), id);
    if (error != std::errc{} || end != argument.data() + argument.size() || id == 0) return std::nullopt;
    return id;
}

std::optional<CoinDestroy> destroy_cursor_coin(const std::uint32_t (&cursor)[4]) {
    for (int type = 3; type >= 0; --type)
        if (static_cast<std::int32_t>(cursor[type]) > 0) return CoinDestroy{type, static_cast<std::int32_t>(cursor[type])};
    return std::nullopt;
}

std::string loot_no_drop_question(std::string_view item_name) {
    return std::string(item_name) + " is a NO DROP item, are you sure you wish to loot it?";
}

bool loot_all_asks(bool own_corpse, std::int32_t loot_all_confirm) { return !own_corpse && loot_all_confirm != 0; }

bool loot_no_drop_asks(bool own_corpse, bool droppable, std::int32_t mode, const std::function<bool()>& usable) {
    if (mode < 0 || mode > 2) mode = 0;
    if (own_corpse || droppable || mode == 2) return false;
    if (mode == 0) return true;
    return !usable();
}

LootAllPlan plan_loot_all(const std::vector<LootAllItem>& items, bool popup_pending) {
    LootAllPlan p;
    p.popup_pending = popup_pending;
    for (int pass = 0; pass < 2; ++pass) {
        for (const LootAllItem& it : items) {
            if (it.bag != (pass == 0)) continue;
            if (!it.asks) {
                p.queue.push_back(it.slot);
                continue;
            }
            if (p.popup_pending) { // the list is freed, but an earlier question stays open (@0x6bdf93)
                p.queue.clear();
                p.aborted = true;
                return p;
            }
            p.popup_pending = true;
            p.ask = it.slot;
        }
    }
    p.close = p.queue.empty() && !p.popup_pending;
    return p;
}

std::optional<std::int32_t> fastdrop_choice(std::string_view args, std::string_view on, std::string_view off,
                                            std::string_view never) {
    if (same_no_case(args, on)) return 1;
    if (same_no_case(args, off)) return 0;
    if (same_no_case(args, never)) return -1;
    return std::nullopt;
}

std::uint32_t fastdrop_message(std::int32_t value) {
    return value == 1 ? kStrFastDropOn : value == 0 ? kStrFastDropOff : kStrFastDropNever;
}

std::int32_t fastdrop_combo_value(int choice) { return choice == 0 ? 0 : choice == 1 ? 1 : -1; }

std::optional<std::int32_t> lootnodrop_choice(std::string_view args, std::string_view never, std::string_view sometimes,
                                              std::string_view usable, std::string_view always) {
    char first = args.empty() ? '\0' : args.front();
    if (starts_no_case(args, never, 4) || first == '2') return 2;
    if (starts_no_case(args, sometimes, 4) || starts_no_case(args, usable, 4) || first == '1') return 1;
    if (starts_no_case(args, always, 4) || first == '0') return 0;
    return std::nullopt;
}

std::uint32_t lootnodrop_set_name(std::int32_t value) {
    return value == 2 ? kStrLootNoDropNever : value == 1 ? kStrLootNoDropNotUsable : kStrLootNoDropAlways;
}

std::uint32_t lootnodrop_shown_name(std::int32_t value) {
    return value == 2 ? kStrLootNoDropAlways : value == 1 ? kStrLootNoDropNotUsable : kStrLootNoDropNever;
}

std::uint32_t aa_train_question(std::int32_t cost) { return 4041u + (cost != 1 ? 1u : 0u); }

std::uint32_t leadership_buy_question(bool group, std::int32_t cost) {
    return (group ? 8582u : 8587u) + (cost != 1 ? 1u : 0u);
}

std::uint32_t adventure_purchase_question(std::uint32_t type, std::int32_t quantity, std::int32_t cost) {
    struct Strings {
        std::uint32_t one, several;
    };
    static constexpr Strings kByType[] = {{0, 0}, {5862, 5931}, {5933, 5947}, {5949, 5951}, {5953, 5955}, {5957, 5959}};
    if (type == 0 || type >= std::size(kByType)) return 0;
    const Strings& s = kByType[type];
    return (quantity == 1 ? s.one : s.several) + (cost != 1 ? 1u : 0u);
}

} // namespace hailstone::game
