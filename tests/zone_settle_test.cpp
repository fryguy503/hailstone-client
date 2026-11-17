// app/zone_settle.h: the loading screen stays up after the zone-in until the world has been drawn whole.
#include <doctest/doctest.h>

#include "app/zone_settle.h"

using namespace hailstone::app;

namespace {
ZoneSettleFrame drawn(std::size_t waiting = 0, std::size_t shadows = 0) {
    ZoneSettleFrame f;
    f.actors_waiting = waiting;
    f.shadow_builds = shadows;
    f.scene = f.ui = true;
    return f;
}
} // namespace

TEST_CASE("zone settle: plugin questions accept the mouse without bypassing the interface lock") {
    struct Case {
        bool in_game, interface_locked, settling, consent_pending, mouse_locked;
    };
    const Case cases[] = {
        {false, false, false, false, false}, // Loading and the other pregame screens retain their mouse.
        {false, false, false, true, false},
        {false, false, true, false, false},
        {false, false, true, true, false},
        {false, true, false, false, false},
        {false, true, false, true, false},
        {false, true, true, false, false},
        {false, true, true, true, false},
        {true, false, false, false, false}, // Ordinary play remains unlocked.
        {true, false, false, true, false},
        {true, false, true, false, true},   // Settling keeps other windows from taking presses.
        {true, false, true, true, false},   // The native question is reachable during settling.
        {true, true, false, false, true},   // /safelock always wins in game, including during a question.
        {true, true, false, true, true},
        {true, true, true, false, true},
        {true, true, true, true, true},
    };
    for (const auto& c : cases) {
        CAPTURE(c.in_game);
        CAPTURE(c.interface_locked);
        CAPTURE(c.settling);
        CAPTURE(c.consent_pending);
        CHECK(ui_mouse_locked(c.in_game, c.interface_locked, c.settling, c.consent_pending) == c.mouse_locked);
    }
}

TEST_CASE("zone settle: at least kMinFrames drawn frames, even with nothing left to build") {
    ZoneSettle s;
    CHECK_FALSE(s.active());
    CHECK_FALSE(s.update(drawn(), 0.0)); // not begun: nothing ends
    s.begin(10.0);
    CHECK(s.active());
    ZoneSettleFrame blank; // no scene or UI drawn (a skipped frame): does not count
    CHECK_FALSE(s.update(blank, 10.0));
    for (int i = 1; i < ZoneSettle::kMinFrames; ++i) CHECK_FALSE(s.update(drawn(), 10.0 + i * 0.016));
    CHECK(s.update(drawn(), 10.1));
    CHECK_FALSE(s.active());
    CHECK_FALSE(s.timed_out());
    CHECK(s.progress() == 1.0f);
}

TEST_CASE("zone settle: waits for every spawn in view to be dressed and every shadow cube drawn") {
    ZoneSettle s;
    s.begin(0.0);
    CHECK_FALSE(s.update(drawn(40), 0.1));
    float p0 = s.progress();
    CHECK(s.detail().find("40 left") != std::string::npos);
    CHECK_FALSE(s.update(drawn(20), 0.2));
    CHECK(s.progress() > p0);
    float p1 = s.progress();
    CHECK_FALSE(s.update(drawn(30), 0.3)); // more spawns arrived: the bar does not go back
    CHECK(s.progress() >= p1);
    CHECK_FALSE(s.update(drawn(0, 2), 0.4)); // dressed; shadows still building
    CHECK(s.detail() == "Casting shadows");
    CHECK(s.update(drawn(0, 0), 0.5));
    CHECK(s.progress() == 1.0f);
}

TEST_CASE("zone settle: a model that never dresses cannot keep the loading screen up") {
    ZoneSettle s;
    s.begin(5.0);
    CHECK_FALSE(s.update(drawn(1), 5.0 + ZoneSettle::kTimeout - 0.01));
    CHECK(s.update(drawn(1), 5.0 + ZoneSettle::kTimeout));
    CHECK(s.timed_out());
    CHECK_FALSE(s.active());
    // A new zone-in starts afresh.
    s.begin(100.0);
    CHECK(s.active());
    CHECK_FALSE(s.timed_out());
    CHECK(s.frames() == 0);
}
