#pragma once

// /cast's spell name: MacroQuest's Cast (MQCommands.cpp), which E3 uses, adds a memorized spell's name to RoF2's
// /cast <gem> (Cmd_cast @0x4e8900). GameConnection runs it (the Cmd_cast handler).
//
//   /cast <gem 1..12>        RoF2's: a line that starts with a number is the gem, as the original takes it
//   /cast "<spell name>"     the first gem whose spell's name starts with it, in any case
//   /cast "=<spell name>"    the first gem whose spell has that whole name, in any case
//
// As MacroQuest: the name is the line's first word (GetArg; a quoted run is one word, its quotes dropped), matched
// by MaybeExactStartsWith; no gem's spell matching says "You do not seem to have that spell memorized."

#include <string_view>

#include "core/ascii_case.h"

namespace hailstone::net {

// The line is RoF2's gem form: empty, or starting with a number (a sign and a digit too, so "/cast -1" and
// "/cast 0" keep the original's invalid-gem message).
inline bool cast_line_is_gem(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    if (line.empty()) return true;
    if ((line.front() == '-' || line.front() == '+') && line.size() > 1) line.remove_prefix(1);
    return line.front() >= '0' && line.front() <= '9';
}

// MaybeExactStartsWith(spell name, needle): "=name" the whole name in any case; else the name starts with it.
inline bool cast_spell_name_matches(std::string_view spell_name, std::string_view needle) {
    if (!needle.empty() && needle.front() == '=') return iequals(spell_name, needle.substr(1));
    return !needle.empty() && istarts_with(spell_name, needle);
}

} // namespace hailstone::net
