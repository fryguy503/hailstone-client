// SPDX-License-Identifier: GPL-2.0-only
#pragma once

// MacroQuest's colour codes in a chat line a plugin or script wrote ("\agGreen \arred\ax green again"): BEL ('\a',
// what C#'s and C's "\a" is) and what follows it.
//   \a then y o g u r t m p w b    yellow, orange, green, blue, red, teal, magenta, purple, white, black
//   \a- then one of those          its dark shade (black has none)
//   \a#rrggbb                      that colour
//   \ax                            back to the colour before the last code
// Each code but \ax opens a colour; \ax closes the last one open. The colours and the rules are ported from
// MacroQuest (GPL-2.0, Copyright (C) 2002-present MacroQuest Authors; THIRD_PARTY.md): MQToSTML in
// src/main/MQ2Utilities.cpp, which turns them into STML's <c "#rrggbb"> and </c> as the chat windows here do too.
// Two deliberate differences, both where MacroQuest's output closes tags that are not its own:
//   - \ax with no colour open does nothing (MacroQuest's </c> would close the line's own colour);
//   - colours still open at the end of the text are closed by `finish`.
// Only the plugins' and scripts' lines keep the codes: the BEL of every other line is removed when it comes
// (net::GameConnection::chat), so a server's or a player's text cannot colour anyone's chat.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::game {

inline constexpr char kMqColorCode = '\a';

struct MqColorPiece {
    enum class Kind : std::uint8_t { Text, Open, Close };
    Kind kind = Kind::Text;
    std::string_view text; // Kind::Text: a part of the text given to parse_mq_colors (it must outlive the pieces)
    std::uint32_t rgb = 0; // Kind::Open: 0xRRGGBB
};

// Carried between the parts of one line (the text between its item links): the colour now and how many are open.
struct MqColorState {
    std::optional<std::uint32_t> current; // none after \ax or \a#: MacroQuest's CurrentColor -1
    int open = 0;
    explicit MqColorState(std::optional<std::uint32_t> line_rgb = std::nullopt) : current(line_rgb) {}
};

// `text` cut at its colour codes, in order. A letter code that names the colour already current opens nothing (as
// MacroQuest); an unknown letter, a \a#rrggbb without six hex digits, and a \a at the end change nothing.
std::vector<MqColorPiece> parse_mq_colors(std::string_view text, MqColorState& state);
// A Close for each colour still open, which leaves none open.
std::vector<MqColorPiece> finish_mq_colors(MqColorState& state);

// The text without its colour codes (the chat log, the client's log, plain-text copies).
std::string strip_mq_colors(std::string_view text);

} // namespace hailstone::game
