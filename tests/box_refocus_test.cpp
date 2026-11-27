// app/box_refocus.h: a box login's first window takes the focus back once the other windows have opened.
#include <doctest/doctest.h>

#include <chrono>

#include "app/box_refocus.h"

using hailstone::app::BoxRefocus;
using namespace std::chrono_literals;

TEST_CASE("Box refocus: once the other windows took the focus, the first takes it back after they settle") {
    const auto t0 = BoxRefocus::Clock::time_point{} + 100s;
    BoxRefocus r;
    CHECK_FALSE(r.watching());
    CHECK_FALSE(r.step(false, t0)); // nothing started: nothing to do

    r.start(2, t0);
    CHECK(r.watching());
    CHECK_FALSE(r.step(true, t0 + 200ms));                 // still focused: the others have not shown yet
    CHECK_FALSE(r.step(false, t0 + 1s));                   // the first other window took the focus
    CHECK_FALSE(r.step(false, t0 + 1s + 1400ms));          // settling
    CHECK(r.step(false, t0 + 1s + BoxRefocus::kSettle));   // raise
    CHECK_FALSE(r.step(true, t0 + 2600ms));                // back in front
    // The second window opened later and took the focus again: one more raise, then it is done.
    CHECK_FALSE(r.step(false, t0 + 4s));
    CHECK(r.step(false, t0 + 4s + BoxRefocus::kSettle));
    CHECK_FALSE(r.watching());
    CHECK_FALSE(r.step(false, t0 + 8s)); // the player's own change of focus is left alone
}

TEST_CASE("Box refocus: focus back before the settle time cancels that raise; it stops watching after a while") {
    const auto t0 = BoxRefocus::Clock::time_point{} + 100s;
    BoxRefocus r;
    r.start(1, t0);
    CHECK_FALSE(r.step(false, t0 + 1s));
    CHECK_FALSE(r.step(true, t0 + 1500ms)); // the player clicked back first: no raise owed for this loss
    CHECK_FALSE(r.step(false, t0 + 2s));    // lost again: the settle time starts over
    CHECK_FALSE(r.step(false, t0 + 2s + 1s));
    CHECK(r.step(false, t0 + 2s + BoxRefocus::kSettle));

    // A window that never takes the focus within the watch: nothing happens after it ends.
    r.start(1, t0);
    CHECK_FALSE(r.step(true, t0 + BoxRefocus::kWindow - 1s));
    CHECK_FALSE(r.step(false, t0 + BoxRefocus::kWindow));
    CHECK_FALSE(r.watching());
    CHECK_FALSE(r.step(false, t0 + BoxRefocus::kWindow + 5s));
}
