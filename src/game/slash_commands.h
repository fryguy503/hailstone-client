#pragma once

// The original client's slash command table and how it reads a typed line:
// CEverQuest::InterpretCmd
// @0x51fce0 matches the first word against the 391 commands of
// __CommandList @0x00acd5a8 (384 here: the voice macro commands /vplay
// /vtell /vgroup /vraid are left out, as this client has no voice macros,
// and so are Daybreak's services /legendsofnorrath /tcg;
// they answer as unknown commands)
// in table order, first by the localised name
// (the eqstr text of the entry's string id), then by the built-in name. A
// typed word matches when it is a case-insensitive prefix of the command
// name, so "/wh" runs /who and "/g" runs /gsay. GM-only commands (restriction
// > 0) never match for other players. GetNextCmdArg @0x477f60 splits the
// arguments.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace hailstone::game {

class EqStrings;

struct SlashCommand {
    const char* name;      // built-in name ("/who"; "'" and ":" have no slash)
    std::int32_t eqstr;    // id of the localised name, -1 = none
    const char* handler;   // the original's handler ("Cmd_who")
    std::uint8_t restriction; // > 0: GM only
    std::uint8_t category;    // /help class: 0 hidden, 1 normal, 2 emote, 3 guild, 4 voice, 6 chat
    std::uint8_t flags;       // bit 0: usable while dead
};

// The table in the original's order, followed by Hailstone additions.
std::span<const SlashCommand> slash_commands();

// GetNextCmdArg: skips spaces, then reads one word. A word that starts with
// ' or " runs to the matching quote (the quotes are dropped); otherwise it
// stops at a space or at an item link's \x12. Advances `rest` past the word.
std::string next_cmd_arg(std::string_view& rest);

// The original's atol on a command argument, with its 32-bit long (core/msvc_atoi.h): leading white space, a sign,
// then digits, accumulated in 32 bits; independent of the platform's long.
std::int32_t cmd_atol(std::string_view text);

// The table index for a typed word (InterpretCmd's two loops), or nullopt.
// `strings` supplies the localised names (may be null: built-in names only).
std::optional<std::size_t> find_slash_command(std::string_view word, bool gm, const EqStrings* strings = nullptr);

// One typed line, as InterpretCmd splits it.
struct CommandLine {
    enum class Kind : std::uint8_t {
        Empty,       // nothing to do
        ChatServer,  // ';' (text after it) or '[' (the whole line): straight to the chat server
        Command,     // a table entry: index, args
        Invalid,     // a '/' word with no match: eqstr 12380
        Text,        // plain text for the default channel
    };
    Kind kind = Kind::Empty;
    std::size_t index = 0;
    std::string word; // the command word as typed
    std::string args; // everything after the command word (leading spaces kept as the original passes them)
    std::string text; // ChatServer / Text: the text
};
CommandLine parse_command_line(std::string_view line, bool gm, const EqStrings* strings = nullptr);

} // namespace hailstone::game
