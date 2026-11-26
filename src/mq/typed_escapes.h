#pragma once

// MacroQuest's escapes in a typed line (MQCommands.cpp Echo): what lets the player colour their own /echo, which
// Hailstone gives its other echoing commands too (/popupecho, /popcustom, /popup). A typed line holds a backslash and
// a letter where a script's holds the BEL MQ's colour codes start with (game/mq_colors.h); this turns one into the
// other:
//   \a          BEL, so "\ayHello" is a yellow "Hello"
//   \n          a line break
//   \\          one backslash
//   a backslash at the end of the line is dropped, as MQ drops it.
// MQ turns every other \<letter> into the control character letter - 'a' + 7 (\b backspace, \t the character 26 ...)
// and an upper-case one into a value below zero; those are left as typed here, a deliberate difference (they would
// only draw as boxes).
//
// typed_line: the BEL already in the line is taken out first unless a script or plugin sent it (`from_code`): a ${...}
// in the player's line may have brought in a server's text (a name), which must not colour the chat; then the
// player's own escapes become codes. Text a server wrote with a backslash and an "a" in it still turns into a code, as
// in MQ.

#include <string>
#include <string_view>

#include "game/mq_colors.h"

namespace hailstone::mq {

inline std::string typed_escapes(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '\\') {
            out += line[i];
            continue;
        }
        if (++i >= line.size()) break; // a lone backslash at the end
        switch (line[i]) {
        case 'a': out += game::kMqColorCode; break;
        case 'n': out += '\n'; break;
        case '\\': out += '\\'; break;
        default:
            out += '\\';
            out += line[i];
            break;
        }
    }
    return out;
}

inline std::string typed_line(std::string_view line, bool from_code) {
    std::string text(line);
    if (!from_code) std::erase(text, game::kMqColorCode);
    return typed_escapes(text);
}

} // namespace hailstone::mq
