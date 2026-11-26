#pragma once

// MacroQuest's popup commands (MQCommands.cpp PopupText / PopupTextCustom / PopupTextEcho, MQ2Utilities.cpp
// CustomPopup), which E3 shows its alerts with, on the game's centre-screen text (game::TextOverlay, CTextOverlay):
//
//   /popup <text>                                 the whole line, light blue (colour 18), 3 s
//   /popcustom [colour] [seconds] <text>          a leading number is the colour, a second one the seconds
//   /popupecho [colour] [seconds] <text>          the same, and "Popup:: <text>" in chat
//
// As MQ: a first word that starts with a digit is the colour (GetIntFromString: "15x" is 15); then a second word
// that starts with a digit is the hold in seconds, and the text is the rest after the words taken (GetNextArg). A
// first word that does not start with a digit leaves the whole line as the text. An empty /popcustom or /popupecho
// line prints MQ's usage instead. The overlay is DisplayOverlayText's: priority 10 ("always 10 in eqgame.exe"),
// alpha 255 (100%), fades of 500 ms.
//
// Three deliberate differences: the hold is at most 60 s (kPopupMaxHoldMs); MQ's colour codes (\ar, \ay ...) are taken out of the overlay's text, which MQ hands the
// game as they are (shown as written), and the chat line of /popupecho keeps them; and the text takes MQ's typed
// escapes as /echo does (mq/typed_escapes.h), where MQ's popups take a typed "\ay" as written.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "game/mq_colors.h"
#include "game/text_overlay.h"
#include "mq/calc.h"

namespace hailstone::mq {

enum class PopupKind : std::uint8_t { Plain, Custom, Echo };

struct Popup {
    std::string text;
    std::int32_t colour = 18;      // CONCOLOR_LIGHTBLUE
    std::uint32_t hold_ms = 3000;
    std::string usage;             // MQ's usage line for an empty /popcustom or /popupecho; nothing is shown then
};

inline constexpr std::int32_t kPopupPriority = 10, kPopupAlpha = 255;
inline constexpr std::uint32_t kPopupFadeMs = 500;
// The longest hold a popup takes (Hailstone): 60 s. MQ takes any (an int of milliseconds, up to some 24 days), so a
// script could hold the centre of the screen for weeks; a longer one is held for 60 s.
inline constexpr std::uint32_t kPopupMaxHoldMs = 60000;

// MQ's GetNextArg(line, n) with its default separators: the rest of the line after its first n words (spaces and tabs
// separate them, a "quoted run" keeps them together), leading separators skipped. An unclosed quote takes the rest.
inline std::string_view popup_next_arg(std::string_view line, int n) {
    std::size_t i = 0;
    auto sep = [&](std::size_t k) { return k < line.size() && (line[k] == ' ' || line[k] == '\t'); };
    while (sep(i)) ++i;
    for (; n > 0; --n) {
        bool quoted = false;
        while (i < line.size() && (quoted || !sep(i))) {
            if (line[i] == '"') quoted = !quoted;
            ++i;
        }
        while (sep(i)) ++i;
    }
    return line.substr(i);
}

inline Popup parse_popup(std::string_view line, PopupKind kind) {
    Popup p;
    if (kind == PopupKind::Plain) {
        p.text = std::string(line);
        return p;
    }
    auto digit = [](const std::string& w) { return !w.empty() && w[0] >= '0' && w[0] <= '9'; };
    const std::string first = get_arg(line, 1);
    if (first.empty()) {
        p.usage = kind == PopupKind::Echo
                      ? "\awUsage: /popupecho [\agcolor\ax] [\agdisplaytime\ax(in seconds)] [\agmessage\ax]"
                      : "\awUsage: /popcustom [\agcolor\ax] [\agdisplaytime\ax(in seconds)] [\agmessage\ax]";
        return p;
    }
    if (!digit(first)) {
        p.text = std::string(line);
        return p;
    }
    p.colour = int_from_text(first, p.colour);
    const std::string second = get_arg(line, 2);
    if (digit(second)) {
        // MQ: GetIntFromString(word, 3000) * 1000 in an int; held here to 0..kPopupMaxHoldMs (saturating).
        const std::int64_t ms = std::int64_t{int_from_text(second, 3000)} * 1000;
        p.hold_ms = static_cast<std::uint32_t>(std::clamp<std::int64_t>(ms, 0, kPopupMaxHoldMs));
        p.text = std::string(popup_next_arg(line, 2));
    } else {
        p.text = std::string(popup_next_arg(line, 1));
    }
    return p;
}

// DisplayOverlayText(text, colour, 100, 500, 500, hold) on the game's overlay, the text without MQ's colour codes.
inline void show_popup(game::TextOverlay& overlay, const Popup& p, std::uint32_t now_ms) {
    overlay.display(game::strip_mq_colors(p.text), p.colour, kPopupPriority, kPopupAlpha, kPopupFadeMs, kPopupFadeMs,
                    p.hold_ms, now_ms);
}

// /popupecho's chat line (WriteChatf "\ayPopup\aw:: %s") for GameConnection::client_message, MQ's colour codes kept
// for the chat window. The text is the command's after mq::typed_line (the player's escapes made codes, the BEL a
// ${...} brought in taken out).
inline std::string popup_echo_line(const Popup& p) { return "\ayPopup\aw:: " + p.text; }

} // namespace hailstone::mq
