#pragma once

// /itemnotify (MacroQuest's ItemNotify, which E3 uses for its bag, loot, sell and trade moves): which slot or item a
// line names and which mouse notification to give it. A Hailstone addition: the original client has no such command.
// The app runs the click (ClientApp::itemnotify_command).
//
//   /itemnotify <slot|#> <notification>              a slot by number (0..33) or name: the worn slots' names,
//                                                    pack1..10, cursor, loot1.., bank1.., sharedbank1..2, trade1..,
//                                                    enviro1..
//   /itemnotify in <bag slot> <slot # in bag> <notification>   pack1..10, bank1..24 or sharedbank1..2; 1-based
//   /itemnotify <item name | #id> <notification>     an item carried, by its exact name (any case) or id
//   notifications: leftmouseup, rightmouseup, rightmouseheld

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/ascii_case.h"
#include "game/exchange.h"
#include "net/item_packets.h"

namespace hailstone::game {

enum class ItemNotification : std::uint8_t { LeftMouseUp, RightMouseUp, RightMouseHeld };

struct ItemNotify {
    std::optional<net::ItemSlot> slot; // a place, or else
    std::string item_name;             // an item by its exact name (any case), or else
    std::uint32_t item_id = 0;         // an item by its id ("#id")
    ItemNotification notification = ItemNotification::LeftMouseUp;
    std::string error;                 // what MacroQuest says when the line names nothing; empty when it does
};

inline std::optional<ItemNotification> item_notification(std::string_view word) {
    if (iequals(word, "leftmouseup")) return ItemNotification::LeftMouseUp;
    if (iequals(word, "rightmouseup")) return ItemNotification::RightMouseUp;
    if (iequals(word, "rightmouseheld")) return ItemNotification::RightMouseHeld;
    return std::nullopt;
}

namespace detail {
// A whole number of at most 6 digits; nothing for anything else.
inline std::optional<int> item_notify_number(std::string_view s) {
    if (s.empty() || s.size() > 6 || s.find_first_not_of("0123456789") != std::string_view::npos) return std::nullopt;
    int n = 0;
    for (char c : s) n = n * 10 + (c - '0');
    return n;
}
// "<prefix><n>" with n 1..count: n - 1; nothing for anything else.
inline std::optional<int> item_notify_numbered(std::string_view word, std::string_view prefix, int count) {
    if (word.size() <= prefix.size() || !iequals(word.substr(0, prefix.size()), prefix)) return std::nullopt;
    auto n = item_notify_number(word.substr(prefix.size()));
    if (!n || *n < 1 || *n > count) return std::nullopt;
    return *n - 1;
}
} // namespace detail

inline ItemNotify parse_item_notify(std::string_view line) {
    using net::ItemSlot;
    ItemNotify out;
    const std::vector<std::string> w = exchange_words(line);
    if (w.size() < 2) {
        out.error = "Syntax: /itemnotify <slot|#> <notification>, /itemnotify in <bag slot> <slot # in bag> "
                    "<notification> or /itemnotify <itemname> <notification>";
        return out;
    }
    constexpr int kPacks = net::kSlotGeneral10 - net::kSlotGeneral1 + 1;
    constexpr int kBank = 24, kShared = 2, kBagSlots = 255;
    auto in = [](std::int16_t type, int slot, int sub) {
        return ItemSlot{type, static_cast<std::int16_t>(slot), static_cast<std::int16_t>(sub), -1, 0, 0};
    };
    std::string_view note;
    if (iequals(w[0], "in")) {
        if (w.size() < 4) {
            out.error = "Syntax: /itemnotify in <bag slot> <slot # in bag> <notification>";
            return out;
        }
        const auto sub = detail::item_notify_number(w[2]);
        if (sub && *sub >= 1 && *sub <= kBagSlots) {
            if (auto n = detail::item_notify_numbered(w[1], "sharedbank", kShared)) out.slot = in(net::kInvSharedBank, *n, *sub - 1);
            else if (auto b = detail::item_notify_numbered(w[1], "bank", kBank)) out.slot = in(net::kInvBank, *b, *sub - 1);
            else if (auto p = detail::item_notify_numbered(w[1], "pack", kPacks))
                out.slot = in(net::kInvPossessions, net::kSlotGeneral1 + *p, *sub - 1);
        }
        if (!out.slot) {
            out.error = "Could not find slot to send notification to " + w[1] + " " + w[2];
            return out;
        }
        note = w[3];
    } else {
        const std::string& a = w[0];
        if (auto n = detail::item_notify_number(a)) {
            if (*n <= net::kSlotCursor) out.slot = in(net::kInvPossessions, *n, -1);
        } else if (auto worn = exchange_slot(a)) {
            out.slot = in(net::kInvPossessions, *worn, -1);
        } else if (iequals(a, "cursor")) {
            out.slot = in(net::kInvPossessions, net::kSlotCursor, -1);
        } else if (auto p = detail::item_notify_numbered(a, "pack", kPacks)) {
            out.slot = in(net::kInvPossessions, net::kSlotGeneral1 + *p, -1);
        } else if (auto l = detail::item_notify_numbered(a, "loot", net::kCorpseSlotEnd - net::kCorpseSlotBegin + 1)) {
            out.slot = in(net::kInvCorpse, net::kCorpseSlotBegin + *l, -1);
        } else if (auto s = detail::item_notify_numbered(a, "sharedbank", kShared)) {
            out.slot = in(net::kInvSharedBank, *s, -1);
        } else if (auto b = detail::item_notify_numbered(a, "bank", kBank)) {
            out.slot = in(net::kInvBank, *b, -1);
        } else if (auto t = detail::item_notify_numbered(a, "trade", 16)) {
            out.slot = in(net::kInvTrade, *t, -1);
        } else if (auto e = detail::item_notify_numbered(a, "enviro", 10)) {
            out.slot = in(net::kInvWorld, *e, -1);
        } else if (a.size() > 1 && a[0] == '#') {
            auto id = detail::item_notify_number(std::string_view(a).substr(1));
            if (id && *id > 0) out.item_id = static_cast<std::uint32_t>(*id);
        } else {
            out.item_name = a;
        }
        if (!out.slot && out.item_id == 0 && out.item_name.empty()) {
            out.error = "Invalid item slot '" + a + "'";
            return out;
        }
        note = w[1];
    }
    if (auto n = item_notification(note)) {
        out.notification = *n;
    } else {
        out.error = "Could not send notification to " + w[0] + " " + std::string(note);
        out.slot.reset();
        out.item_name.clear();
        out.item_id = 0;
    }
    return out;
}

} // namespace hailstone::game
