#pragma once

// /removebuff and /removepetbuff (MacroQuest's RemoveBuff and RemovePetBuff, which E3 uses to drop a buff from the
// player or the pet): what a line names. A Hailstone addition: the original client has no such commands.
// GameConnection::removebuff_command and removepetbuff_command run them.
//
//   /removebuff <name>          the player's first buff (long buffs, then songs) whose name matches
//   /removebuff -pet <name>     the focused pet's first buff (in its window's order) whose name matches
//   /removebuff -both <name>    the pet's, then the player's
//   /removepetbuff <name>       the same as /removebuff -pet (RemovePetBuff, which -pet calls in MQ)
//
// As MacroQuest: the name is the rest of the line, or the text between quotes when it starts with one. It matches a
// spell's name anywhere in it, in any case; "=name" matches the whole name only (MaybeExactCompare). Only the first
// match is removed, and nothing is said.

#include <string>
#include <string_view>

#include "core/ascii_case.h"

namespace hailstone::net {

struct RemoveBuff {
    bool self = true;
    bool pet = false;
    std::string name; // empty: the line names nothing, and nothing is removed
};

namespace detail {
inline std::string_view remove_buff_trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    // (MacroQuest keeps trailing spaces, which then match no name; they are dropped here)
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}
// GetMaybeQuotedArg: a quoted word's text, or else the rest of the line.
inline std::string remove_buff_name(std::string_view s) {
    s = remove_buff_trim(s);
    if (!s.empty() && s.front() == '"') {
        s.remove_prefix(1);
        if (const auto end = s.find('"'); end != std::string_view::npos) s = s.substr(0, end);
    }
    return std::string(s);
}
// MacroQuest's argument 2: past the first word.
inline std::string_view remove_buff_after_word(std::string_view s) {
    const auto space = s.find_first_of(" \t");
    return space == std::string_view::npos ? std::string_view{} : s.substr(space);
}
} // namespace detail

inline RemoveBuff parse_remove_buff(std::string_view line) {
    RemoveBuff out;
    line = detail::remove_buff_trim(line);
    // (MacroQuest tests the line's start, _strnicmp(line, "-pet", 4), not the whole word)
    if (istarts_with(line, "-pet")) {
        out.self = false;
        out.pet = true;
        out.name = detail::remove_buff_name(detail::remove_buff_after_word(line));
    } else if (istarts_with(line, "-both")) {
        out.pet = true;
        out.name = detail::remove_buff_name(detail::remove_buff_after_word(line));
    } else {
        out.name = detail::remove_buff_name(line);
    }
    return out;
}

// /removepetbuff's line: the pet's buff only, the name taken as /removebuff takes it (a leading "-pet" is part of the
// name here, as MQ's RemovePetBuff takes its whole line).
inline RemoveBuff parse_remove_pet_buff(std::string_view line) {
    RemoveBuff out;
    out.self = false;
    out.pet = true;
    out.name = detail::remove_buff_name(line);
    return out;
}

// MaybeExactCompare(spell name, needle): "=name" the whole name in any case; else the needle anywhere in it.
inline bool remove_buff_matches(std::string_view spell_name, std::string_view needle) {
    if (needle.empty()) return spell_name.empty();
    if (needle.front() == '=') return iequals(spell_name, needle.substr(1));
    if (needle.size() > spell_name.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= spell_name.size(); ++i)
        if (iequals(spell_name.substr(i, needle.size()), needle)) return true;
    return false;
}

} // namespace hailstone::net
