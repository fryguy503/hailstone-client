// The main loop's frame cap and drawn frames (app/frame_pace.h): display.max_fps, and display.background_fps while
// another window has the focus, plus the 240 cap on login/loading screens when max_fps is unlimited. The loop itself
// (app/client_app.cpp) is not driven here.

#include <doctest/doctest.h>

#include <chrono>
#include <limits>

#include "app/frame_pace.h"
#include "platform/precise_sleep.h"

using namespace hailstone;
using std::chrono::milliseconds;

TEST_CASE("Frame pace: focused, or a background rate of 0, keeps the frame limit and draws every frame") {
    for (int bg : {0, app::kBackgroundNoDraw, 1, 5, 30}) {
        auto p = app::frame_pace(144, bg, true, milliseconds(0));
        CHECK(p.cap == 144);
        CHECK(p.draw);
    }
    auto p = app::frame_pace(0, 0, false, milliseconds(0));
    CHECK(p.cap == 0);
    CHECK(p.draw);
}

TEST_CASE("Frame pace: a background rate of 10 or more caps the loop and draws every frame") {
    CHECK(app::frame_pace(0, 30, false, milliseconds(0)).cap == 30);
    CHECK(app::frame_pace(144, 60, false, milliseconds(0)).cap == 60);
    CHECK(app::frame_pace(30, 60, false, milliseconds(0)).cap == 30); // the lower of the two
    CHECK(app::frame_pace(0, 10, false, milliseconds(0)).draw);
}

TEST_CASE("Frame pace: below 10 in the background the loop runs at 10 and draws that many frames a second") {
    for (int bg : {1, 5, app::kBackgroundNoDraw}) CHECK(app::frame_pace(0, bg, false, milliseconds(0)).cap == 10);
    // 1: once a second, on the loop's tenth tick (not the eleventh after a late wake).
    CHECK_FALSE(app::frame_pace(0, 1, false, milliseconds(900)).draw);
    CHECK(app::frame_pace(0, 1, false, milliseconds(1000)).draw);
    CHECK(app::frame_pace(0, 1, false, milliseconds(990)).draw);
    // 5: every second tick.
    CHECK_FALSE(app::frame_pace(0, 5, false, milliseconds(100)).draw);
    CHECK(app::frame_pace(0, 5, false, milliseconds(200)).draw);
    // 0 (kBackgroundNoDraw): never.
    CHECK_FALSE(app::frame_pace(0, app::kBackgroundNoDraw, false, milliseconds(60000)).draw);
}

TEST_CASE("Frame pace: display.background_fps as it is kept") {
    CHECK(app::background_fps_setting(0) == 0);
    CHECK(app::background_fps_setting(-1) == app::kBackgroundNoDraw);
    CHECK(app::background_fps_setting(-50) == app::kBackgroundNoDraw);
    CHECK(app::background_fps_setting(1) == 1);
    CHECK(app::background_fps_setting(5) == 5);
    CHECK(app::background_fps_setting(5000) == 1000);
}

TEST_CASE("Frame pace: the auto cap sits just under the display's refresh rate") {
    CHECK(app::auto_frame_cap(60.0f) == 59);
    CHECK(app::auto_frame_cap(59.94f) == 58);
    CHECK(app::auto_frame_cap(120.0f) == 116);
    CHECK(app::auto_frame_cap(144.0f) == 138);
    CHECK(app::auto_frame_cap(165.0f) == 157);
    CHECK(app::auto_frame_cap(240.0f) == 224);
    CHECK(app::auto_frame_cap(360.0f) == 324);
    // A refresh the system does not give (0) or nonsense: no cap.
    CHECK(app::auto_frame_cap(0.0f) == 0);
    CHECK(app::auto_frame_cap(-60.0f) == 0);
    CHECK(app::auto_frame_cap(5000.0f) == 0);
    CHECK(app::auto_frame_cap(std::numeric_limits<float>::quiet_NaN()) == 0);

    // display.max_fps as kept, and the cap a frame takes from it.
    CHECK(app::max_fps_setting(app::kMaxFpsAuto) == app::kMaxFpsAuto);
    CHECK(app::max_fps_setting(0) == 0);
    CHECK(app::max_fps_setting(-7) == 0);
    CHECK(app::max_fps_setting(5) == 10);
    CHECK(app::max_fps_setting(144) == 144);
    CHECK(app::max_fps_setting(5000) == 1000);
    CHECK(app::effective_max_fps(app::kMaxFpsAuto, 144.0f) == 138);
    CHECK(app::effective_max_fps(app::kMaxFpsAuto, 0.0f) == 0);
    CHECK(app::effective_max_fps(144, 60.0f) == 144); // a number is taken as it is, whatever the display
    CHECK(app::effective_max_fps(0, 144.0f) == 0);
}

TEST_CASE("Frame pace: the cap's schedule runs from the last deadline, and from now after a long frame") {
    using std::chrono::microseconds;
    using T = std::chrono::steady_clock::time_point;
    const T start{};
    const microseconds period(6944); // 144 a second
    // On time: one period after the last deadline, not after the moment the frame began its work.
    CHECK(app::next_frame_deadline(start, start + microseconds(100), period) == start + period);
    // A frame that ran past its deadline starts at once, and the schedule goes on from then: no catch-up burst.
    const T late = start + period + microseconds(10);
    CHECK(app::next_frame_deadline(start, late, period) == late);
    const T hitch = start + 5 * period;
    CHECK(app::next_frame_deadline(start, hitch, period) == hitch);
    CHECK(app::next_frame_deadline(hitch, hitch + microseconds(500), period) == hitch + period);
    // A wait that ends a little late is not carried into the next frame: the deadlines stay on the period's grid
    // as long as each frame's work fits in its period.
    T deadline = start;
    for (int i = 1; i <= 1000; ++i) {
        deadline = app::next_frame_deadline(deadline, deadline + microseconds(300), period);
        REQUIRE(deadline == start + i * period);
    }
}

TEST_CASE("Frame pace: an unlimited cap on a light screen counts as 240") {
    auto p = app::frame_pace(0, 0, true, milliseconds(0), true);
    CHECK(p.cap == 240);
    CHECK(p.draw);
}

TEST_CASE("Frame pace: a set max_fps on a light screen is followed exactly") {
    CHECK(app::frame_pace(60, 0, true, milliseconds(0), true).cap == 60);
    CHECK(app::frame_pace(144, 0, true, milliseconds(0), true).cap == 144);
    CHECK(app::frame_pace(500, 0, true, milliseconds(0), true).cap == 500); // not capped at 240
}

TEST_CASE("Frame pace: outside a light screen max_fps 0 stays unlimited and set caps are followed") {
    CHECK(app::frame_pace(0, 0, true, milliseconds(0), true).cap == 240); // for contrast
    CHECK(app::frame_pace(0, 0, true, milliseconds(0), false).cap == 0);
    CHECK(app::frame_pace(144, 0, true, milliseconds(0), false).cap == 144);
    CHECK(app::frame_pace(1000, 0, true, milliseconds(0), false).cap == 1000);
}

TEST_CASE("Frame pace: the background limit still works on a light screen") {
    CHECK(app::frame_pace(0, 30, false, milliseconds(0), true).cap == 30);
    CHECK(app::frame_pace(0, 5, false, milliseconds(0), true).cap == 10);
    CHECK_FALSE(app::frame_pace(0, 5, false, milliseconds(100), true).draw);
    CHECK(app::frame_pace(0, 5, false, milliseconds(200), true).draw);
    CHECK_FALSE(app::frame_pace(0, app::kBackgroundNoDraw, false, milliseconds(60000), true).draw);
    CHECK(app::frame_pace(60, 144, false, milliseconds(0), true).cap == 60); // min of all three
}

TEST_CASE("Frame pace: an unlimited cap on a light screen stays capped in the background") {
    // background_fps 0 follows max_fps (the 240 light-screen cap); a high background rate cannot raise it.
    CHECK(app::frame_pace(0, 0, false, milliseconds(0), true).cap == 240);
    CHECK(app::frame_pace(0, 1000, false, milliseconds(0), true).cap == 240);
    CHECK(app::frame_pace(500, 1000, false, milliseconds(0), true).cap == 500); // a set max_fps is still followed
}

TEST_CASE("Frame pace: the light-screen condition keeps the zone-in settle light") {
    CHECK_FALSE(app::is_light_screen(true, false)); // InGame without settle: normal play
    CHECK(app::is_light_screen(true, true));        // InGame while settling: the loading overlay is still drawn
    CHECK(app::is_light_screen(false, false));      // a pre-game screen
    CHECK(app::is_light_screen(false, true));
}

TEST_CASE("Frame pace: the light-screen cap holds through the deadline schedule and the precise wait") {
    // As the loop takes it: display.max_fps through effective_max_fps, then frame_pace on a light screen.
    CHECK(app::frame_pace(app::effective_max_fps(0, 144.0f), 0, true, milliseconds(0), true).cap == 240);
    // Auto with no refresh rate from the system is unlimited, so a light screen holds it at 240 too.
    CHECK(app::frame_pace(app::effective_max_fps(app::kMaxFpsAuto, 0.0f), 0, true, milliseconds(0), true).cap == 240);
    // Auto with a refresh rate is a set cap, followed as it is.
    CHECK(app::frame_pace(app::effective_max_fps(app::kMaxFpsAuto, 144.0f), 0, true, milliseconds(0), true).cap ==
          138);
    // A few frames of the loop's wait at that cap take at least their periods: the light screen does not spin.
    const auto pace = app::frame_pace(app::effective_max_fps(0, 0.0f), 0, true, milliseconds(0), true);
    REQUIRE(pace.cap == app::kLightScreenCap);
    const std::chrono::microseconds period(1000000 / pace.cap);
    const auto start = std::chrono::steady_clock::now();
    auto deadline = start;
    constexpr int kFrames = 12;
    for (int i = 0; i < kFrames; ++i) {
        deadline = app::next_frame_deadline(deadline, std::chrono::steady_clock::now(), period);
        platform::precise_sleep_until(deadline);
    }
    // Only the lower bound: how late a busy machine wakes is not this test's business.
    CHECK(std::chrono::steady_clock::now() - start >= kFrames * period);
}

TEST_CASE("Frame pace: a minimised window runs at the background tick whatever the settings") {
    // The defaults (max_fps 0, background_fps 0) would be unlimited; minimised they are held at 10.
    CHECK(app::frame_pace(0, 0, false, milliseconds(0), false, true).cap == app::kBackgroundTickHz);
    CHECK(app::frame_pace(0, 0, true, milliseconds(0), false, true).cap == app::kBackgroundTickHz);
    CHECK(app::frame_pace(0, 1000, false, milliseconds(0), false, true).cap == app::kBackgroundTickHz);
    CHECK(app::frame_pace(240, 144, false, milliseconds(0), true, true).cap == app::kBackgroundTickHz);
    CHECK(app::frame_pace(0, app::kBackgroundNoDraw, false, milliseconds(0), false, true).cap ==
          app::kBackgroundTickHz);
    // Not minimised: unchanged.
    CHECK(app::frame_pace(0, 0, false, milliseconds(0), false, false).cap == 0);
}
