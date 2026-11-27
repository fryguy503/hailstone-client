#pragma once

// /useitem's line: RoF2's own slot form (Cmd_useitem @0x479200), MacroQuest's name form (UseItemCmd, which E3 sends:
// /useitem "Manastone", /useitem <item name>) and an item id. GameConnection::useitem_command runs it.
//
//   /useitem <slot 0..32> [bag index]   RoF2's: numbers, as the original takes them
//   /useitem <item name>                MacroQuest's: the first carried item whose name holds it, in any case
//   /useitem "<item name>"              MacroQuest's: the first carried item with that whole name, in any case
//   /useitem <item id>                  Hailstone's (MacroQuest has none): one number past the slots that a carried
//                                       item's id is; with no such item it stays RoF2's slot form (an invalid slot)
//
// As MacroQuest: a first word that is a number is the slot form; otherwise the whole line is the name, its quotes
// taken out (StripQuotes), and quoted means the whole name (FindItemByName's bExact, ci_equals). An unquoted name
// no carried item has shows RoF2's usage text, as any word that was not a number did before the name form.

#include <cstdint>
#include <string>
#include <string_view>

#include "core/ascii_case.h"

namespace hailstone::net {

struct UseItemLine {
    enum class Kind { Usage, Slot, Name };
    Kind kind = Kind::Usage;
    // Slot: the first number, and whether it is alone on the line (an item id when a carried item has it and it is
    // past the slots). A number past 32 bits is kept as one no item has.
    std::uint64_t number = 0;
    bool alone = false;
    // Name: the text to find, and whether it was quoted (the whole name then).
    std::string name;
    bool exact = false;
    // Name, not quoted: when no carried item matches, RoF2's usage text is shown (a mistyped slot, "-1", is still
    // told how the command goes; MacroQuest says nothing).
};

namespace detail {
inline bool use_item_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline std::string_view use_item_trim(std::string_view s) {
    while (!s.empty() && use_item_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && use_item_space(s.back())) s.remove_suffix(1);
    return s;
}
} // namespace detail

inline UseItemLine parse_use_item(std::string_view line) {
    UseItemLine out;
    line = detail::use_item_trim(line);
    if (line.empty()) return out;
    // The first word as RoF2's GetNextCmdArg reads it (game/slash_commands.h next_cmd_arg): one in ' or " quotes runs
    // to the matching quote, the quotes dropped (as MQ's GetArg drops them, so "12" and '12' are numbers too);
    // otherwise it ends at a space, a tab or an item link's \x12.
    std::string_view digits;
    std::string_view after;
    if (line.front() == '\'' || line.front() == '"') {
        const std::size_t close = line.find(line.front(), 1);
        digits = line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1);
        after = close == std::string_view::npos ? std::string_view{} : detail::use_item_trim(line.substr(close + 1));
    } else {
        const std::size_t end = line.find_first_of(" \t\x12");
        digits = line.substr(0, end);
        if (end != std::string_view::npos) after = detail::use_item_trim(line.substr(end));
    }
    bool numeric = !digits.empty();
    for (char c : digits)
        if (c < '0' || c > '9') numeric = false;
    if (numeric) {
        out.kind = UseItemLine::Kind::Slot;
        for (char c : digits) {
            if (out.number > (UINT64_MAX - 9) / 10) { // past 64 bits: no slot and no item's id
                out.number = UINT64_MAX;
                break;
            }
            out.number = out.number * 10 + static_cast<std::uint64_t>(c - '0');
        }
        out.alone = after.empty();
        return out;
    }
    out.kind = UseItemLine::Kind::Name;
    for (char c : line) {
        if (c == '"') out.exact = true;
        else out.name += c;
    }
    out.name = std::string(detail::use_item_trim(out.name));
    if (out.name.empty()) out.kind = UseItemLine::Kind::Usage;
    return out;
}

// ci_equals(item name, name, exact): the whole name in any case, or the name anywhere in it.
inline bool use_item_name_matches(std::string_view item_name, std::string_view name, bool exact) {
    if (exact) return iequals(item_name, name);
    if (name.empty() || name.size() > item_name.size()) return false;
    for (std::size_t i = 0; i + name.size() <= item_name.size(); ++i)
        if (iequals(item_name.substr(i, name.size()), name)) return true;
    return false;
}

} // namespace hailstone::net
