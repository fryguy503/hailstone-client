// SPDX-License-Identifier: GPL-2.0-only
// Ported from MacroQuest's MQToSTML (src/main/MQ2Utilities.cpp; game/mq_colors.h, THIRD_PARTY.md).
#include "game/mq_colors.h"

namespace hailstone::game {

namespace {

// MQToSTML's colour for a letter code, bright or dark; none for a letter it does not know.
std::optional<std::uint32_t> letter_rgb(char letter, bool dark) {
    switch (letter) {
    case 'y': return dark ? 0x999900u : 0xFFFF00u; // yellow
    case 'o': return dark ? 0x996600u : 0xFF9900u; // orange
    case 'g': return dark ? 0x009900u : 0x00FF00u; // green
    case 'u': return dark ? 0x000099u : 0x0000FFu; // blue
    case 'r': return dark ? 0x990000u : 0xFF0000u; // red
    case 't': return dark ? 0x009999u : 0x00FFFFu; // teal
    case 'b': return 0x000000u;                    // black, which has no dark shade
    case 'm': return dark ? 0x990099u : 0xFF00FFu; // magenta
    case 'p': return dark ? 0x660099u : 0x9900FFu; // purple
    case 'w': return dark ? 0x999999u : 0xFFFFFFu; // white
    default: return std::nullopt;
    }
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::vector<MqColorPiece> parse_mq_colors(std::string_view text, MqColorState& state) {
    std::vector<MqColorPiece> out;
    auto open_colour = [&](std::uint32_t rgb) {
        out.push_back({MqColorPiece::Kind::Open, {}, rgb});
        ++state.open;
    };
    std::size_t start = 0;
    for (std::size_t at = text.find(kMqColorCode); at != std::string_view::npos; at = text.find(kMqColorCode, start)) {
        if (at > start) out.push_back({MqColorPiece::Kind::Text, text.substr(start, at - start), 0});
        std::size_t next = at + 1;
        if (next < text.size()) {
            const char c = text[next++];
            if (c == 'x') {
                state.current.reset();
                if (state.open > 0) {
                    out.push_back({MqColorPiece::Kind::Close, {}, 0});
                    --state.open;
                }
            } else if (c == '#') {
                // MacroQuest takes the next six characters whatever they are; a colour opens here only when they
                // are six hex digits (STML's parser would take anything else as no colour).
                std::uint32_t rgb = 0;
                std::size_t digits = 0;
                while (digits < 6 && next + digits < text.size()) {
                    const int d = hex_digit(text[next + digits]);
                    if (d < 0) break;
                    rgb = rgb << 4 | static_cast<std::uint32_t>(d);
                    ++digits;
                }
                const std::size_t taken = text.size() - next < 6 ? text.size() - next : 6;
                next += taken;
                if (digits == 6) {
                    state.current.reset();
                    open_colour(rgb);
                }
            } else {
                bool dark = false;
                char letter = c;
                if (c == '-') {
                    dark = true;
                    letter = next < text.size() ? text[next++] : '\0';
                }
                if (const auto rgb = letter_rgb(letter, dark); rgb && rgb != state.current) {
                    state.current = rgb;
                    open_colour(*rgb);
                }
            }
        }
        start = next;
    }
    if (start < text.size()) out.push_back({MqColorPiece::Kind::Text, text.substr(start), 0});
    return out;
}

std::vector<MqColorPiece> finish_mq_colors(MqColorState& state) {
    std::vector<MqColorPiece> out(static_cast<std::size_t>(state.open > 0 ? state.open : 0),
                                  MqColorPiece{MqColorPiece::Kind::Close, {}, 0});
    state.open = 0;
    return out;
}

std::string strip_mq_colors(std::string_view text) {
    if (text.find(kMqColorCode) == std::string_view::npos) return std::string(text);
    MqColorState state;
    std::string out;
    out.reserve(text.size());
    for (const MqColorPiece& p : parse_mq_colors(text, state))
        if (p.kind == MqColorPiece::Kind::Text) out += p.text;
    return out;
}

} // namespace hailstone::game
