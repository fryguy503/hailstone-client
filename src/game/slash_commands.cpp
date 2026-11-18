#include "game/slash_commands.h"

#include <array>
#include <cctype>

#include "core/msvc_atoi.h"
#include "game/eq_strings.h"

namespace hailstone::game {

namespace {

constexpr SlashCommand kCommands[] = {
#define HAILSTONE_SLASH(name, eqstr, handler, restriction, category, flags) {name, eqstr, handler, restriction, category, flags},
#include "game/slash_commands.inc"
#undef HAILSTONE_SLASH
    // Hailstone's client commands follow the original table to preserve its prefix priority.
    {"/chattabs", -1, "Hailstone_chattabs", 0, 1, 1},
};

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// InterpretCmd's test: the first characters are equal and the typed word is a
// case-insensitive prefix of the name (StrIsPrefixNoCase).
bool word_matches(std::string_view word, std::string_view name) {
    if (word.empty() || name.empty() || word.size() > name.size() || lower(word[0]) != lower(name[0])) return false;
    for (std::size_t i = 0; i < word.size(); ++i)
        if (lower(word[i]) != lower(name[i])) return false;
    return true;
}

bool is_space(char c) { return c == ' ' || c == '\t'; }

} // namespace

std::span<const SlashCommand> slash_commands() { return kCommands; }

std::string next_cmd_arg(std::string_view& rest) {
    std::size_t i = 0;
    while (i < rest.size() && is_space(rest[i])) ++i;
    std::string word;
    if (i < rest.size() && (rest[i] == '\'' || rest[i] == '"')) {
        char quote = rest[i++];
        while (i < rest.size() && rest[i] != quote) word.push_back(rest[i++]);
        if (i < rest.size()) ++i; // the closing quote
    } else {
        while (i < rest.size() && !is_space(rest[i]) && rest[i] != '\x12') word.push_back(rest[i++]);
    }
    rest.remove_prefix(i);
    return word;
}

std::int32_t cmd_atol(std::string_view text) { return core::msvc_atoi(text); }

std::optional<std::size_t> find_slash_command(std::string_view word, bool gm, const EqStrings* strings) {
    if (word.empty()) return std::nullopt;
    // First loop: the localised names (LoadStringTables fills them from
    // eqstr; entries without an id keep the built-in name). A GM-only hit
    // ends the loop for players.
    for (std::size_t i = 0; i < std::size(kCommands); ++i) {
        const auto& c = kCommands[i];
        std::string localised = c.name;
        if (strings && c.eqstr >= 0) {
            std::string s = strings->format(static_cast<std::uint32_t>(c.eqstr), {});
            if (!s.empty() && s[0] != '[') localised = s;
        }
        if (!word_matches(word, localised)) continue;
        if (c.restriction > 0 && !gm) break;
        return i;
    }
    for (std::size_t i = 0; i < std::size(kCommands); ++i) {
        const auto& c = kCommands[i];
        if (!word_matches(word, c.name)) continue;
        if (c.restriction > 0 && !gm) break;
        return i;
    }
    return std::nullopt;
}

CommandLine parse_command_line(std::string_view line, bool gm, const EqStrings* strings) {
    CommandLine out;
    std::size_t i = 0;
    while (i < line.size() && is_space(line[i])) ++i;
    line.remove_prefix(i);
    if (line.empty()) return out;
    if (line[0] == ';' || line[0] == '[') {
        out.kind = CommandLine::Kind::ChatServer;
        out.text = std::string(line[0] == ';' ? line.substr(1) : line);
        return out;
    }
    std::string_view rest = line;
    unsigned char first = static_cast<unsigned char>(line[0]);
    if (line[0] == '/' || std::isalnum(first)) {
        out.word = next_cmd_arg(rest);
    } else {
        out.word = std::string(1, line[0]); // ':' and "'" are commands of one character
        rest.remove_prefix(1);
    }
    while (!rest.empty() && is_space(rest.front())) rest.remove_prefix(1);
    if (auto idx = find_slash_command(out.word, gm, strings)) {
        out.kind = CommandLine::Kind::Command;
        out.index = *idx;
        out.args = std::string(rest);
        return out;
    }
    if (line[0] == '/') {
        out.kind = CommandLine::Kind::Invalid;
        return out;
    }
    out.kind = CommandLine::Kind::Text;
    out.text = std::string(line);
    return out;
}

} // namespace hailstone::game
