// MQ's popups (mq/popup.h): /popup, /popcustom and /popupecho's words as MQ's CustomPopup takes them, and what
// reaches the game's centre-screen text (game::TextOverlay).
#include <doctest/doctest.h>

#include "mq/popup.h"

using hailstone::mq::parse_popup;
using hailstone::mq::PopupKind;

TEST_CASE("popup: /popup takes the whole line, light blue for 3 s") {
    auto p = parse_popup("15 2 CANNOT CHARM", PopupKind::Plain);
    CHECK(p.text == "15 2 CANNOT CHARM");
    CHECK(p.colour == 18);
    CHECK(p.hold_ms == 3000);
    CHECK(p.usage.empty());
}

TEST_CASE("popup: /popupecho and /popcustom take a colour, then seconds, then the rest") {
    auto p = parse_popup("15 2 CANNOT CHARM", PopupKind::Echo);
    CHECK(p.colour == 15);
    CHECK(p.hold_ms == 2000);
    CHECK(p.text == "CANNOT CHARM");
    // A colour without seconds: the rest after the colour.
    p = parse_popup("13 Run away", PopupKind::Custom);
    CHECK(p.colour == 13);
    CHECK(p.hold_ms == 3000);
    CHECK(p.text == "Run away");
    // GetIntFromString: a word that starts with a digit is read as far as it is a number.
    p = parse_popup("15x 4s go", PopupKind::Custom);
    CHECK(p.colour == 15);
    CHECK(p.hold_ms == 4000);
    CHECK(p.text == "go");
    // Not a digit first: the whole line, the defaults.
    p = parse_popup("CANNOT CHARM 15 2", PopupKind::Echo);
    CHECK(p.text == "CANNOT CHARM 15 2");
    CHECK(p.colour == 18);
    CHECK(p.hold_ms == 3000);
    // GetArg drops a word's quotes: a quoted "3" is the seconds; the rest keeps its spacing.
    p = parse_popup("14 \"3\" two  spaces", PopupKind::Custom);
    CHECK(p.hold_ms == 3000);
    CHECK(p.text == "two  spaces");
    // A quoted run is one word for the rest too.
    p = parse_popup("14 \"two words\" left", PopupKind::Custom);
    CHECK(p.hold_ms == 3000); // "two words" does not start with a digit
    CHECK(p.text == "\"two words\" left");
    // A hold is at most 60 s (Hailstone's cap, saturating): 99999999 s would hold the centre of the screen for weeks.
    p = parse_popup("14 99999999 long", PopupKind::Custom);
    CHECK(p.hold_ms == hailstone::mq::kPopupMaxHoldMs);
    CHECK(hailstone::mq::kPopupMaxHoldMs == 60000u);
    CHECK(parse_popup("14 60 x", PopupKind::Custom).hold_ms == 60000u);
    CHECK(parse_popup("14 61 x", PopupKind::Custom).hold_ms == 60000u);
    CHECK(parse_popup("14 59 x", PopupKind::Custom).hold_ms == 59000u);
    CHECK(parse_popup("14 0 x", PopupKind::Custom).hold_ms == 0u);
    // Nothing given: MQ's usage, nothing shown.
    CHECK(parse_popup("", PopupKind::Echo).usage.find("/popupecho") != std::string::npos);
    CHECK(parse_popup("   ", PopupKind::Custom).usage.find("/popcustom") != std::string::npos);
}

TEST_CASE("popup: the overlay gets the text without MQ's colour codes; the echo keeps them") {
    hailstone::game::TextOverlay overlay;
    auto p = parse_popup("15 2 \ar>>^<<\ax CANNOT CHARM", PopupKind::Echo);
    hailstone::mq::show_popup(overlay, p, 1000);
    REQUIRE(overlay.active());
    auto f = overlay.frame(1000 + 600); // past the 500 ms fade-in
    REQUIRE(f);
    CHECK(f->text == ">>^<< CANNOT CHARM");
    CHECK((f->argb & 0xffffff) == hailstone::game::text_overlay_rgb(15));
    CHECK((f->argb >> 24) == 255);
    CHECK(overlay.frame(1000 + 2000 + 500 + 1)); // the hold, then the fade-out
    CHECK_FALSE(overlay.frame(1000 + 2000 + 500 + 600));
    // The echo keeps the codes: the client's own "Popup::" and the text's (after mq::typed_line).
    CHECK(hailstone::mq::popup_echo_line(p) == "\ayPopup\aw:: \ar>>^<<\ax CANNOT CHARM");
    // Priority 10, as the game's: a popup replaces one of the same priority.
    hailstone::mq::show_popup(overlay, parse_popup("first", PopupKind::Plain), 5000);
    hailstone::mq::show_popup(overlay, parse_popup("second", PopupKind::Plain), 5100);
    auto g = overlay.frame(5700);
    REQUIRE(g);
    CHECK(g->text == "second");
}
