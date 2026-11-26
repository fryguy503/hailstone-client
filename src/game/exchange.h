#pragma once

// /exchange and /unequip (MQ2Exchange's commands, which E3 uses; GameConnection::item_command): the worn slots by
// MacroQuest's names, and the command line's words. A Hailstone addition: the original client has neither command.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ascii_case.h"

namespace hailstone::game {

// MacroQuest's names for possessions slots 0..22 (szItemSlot), in slot order.
inline constexpr std::array<std::string_view, 23> kExchangeSlotNames = {
    "charm",     "leftear",   "head",       "face",       "rightear", "neck",   "shoulder", "arms",
    "back",      "leftwrist", "rightwrist", "ranged",     "hands",    "mainhand", "offhand", "leftfinger",
    "rightfinger", "chest",   "legs",       "feet",       "waist",    "powersource", "ammo"};

// A worn slot (0..22) by MacroQuest's name or its number, any case; also "hand", "leg" and "range" for "hands",
// "legs" and "ranged". Nothing for any other word, a general slot or a bag.
inline std::optional<std::int16_t> exchange_slot(std::string_view word) {
    if (!word.empty() && word.size() <= 2 && word.find_first_not_of("0123456789") == std::string_view::npos) {
        int n = 0;
        for (char c : word) n = n * 10 + (c - '0');
        if (n < static_cast<int>(kExchangeSlotNames.size())) return static_cast<std::int16_t>(n);
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kExchangeSlotNames.size(); ++i)
        if (iequals(word, kExchangeSlotNames[i])) return static_cast<std::int16_t>(i);
    if (iequals(word, "hand")) return std::int16_t{12};
    if (iequals(word, "leg")) return std::int16_t{18};
    if (iequals(word, "range")) return std::int16_t{11};
    return std::nullopt;
}

// The command line's words as MacroQuest's GetArg reads them: split at spaces, a "quoted part" one word without its
// quotes (E3 sends /exchange "Mirrored Mask" face, and the slot quoted too).
inline std::vector<std::string> exchange_words(std::string_view line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size()) break;
        std::string word;
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') word += line[i++];
            if (i < line.size()) ++i; // the closing quote
        } else {
            while (i < line.size() && line[i] != ' ') word += line[i++];
        }
        out.push_back(std::move(word));
    }
    return out;
}

} // namespace hailstone::game
