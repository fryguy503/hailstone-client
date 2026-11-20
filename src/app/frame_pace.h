#pragma once

// The main loop's frame cap and whether a frame is drawn (app/client_app.cpp): display.max_fps, and
// display.background_fps while another window has the focus. Below kBackgroundTickHz in the background, the loop
// keeps going at kBackgroundTickHz (the game clock, the connection and the UI pack's scripts run on) and draws only
// as often as the setting says, or never at kBackgroundNoDraw. The game clock catches up at most 250 ms a frame
// (core/step_clock.h kMaxSteps), so a loop slower than 4 Hz would slow the rules down. On the login and loading screens
// (including the last stretch of a zone-in) an unlimited max_fps counts as kLightScreenCap, so they do not spin.

#include <algorithm>
#include <chrono>

namespace hailstone::app {

inline constexpr int kBackgroundNoDraw = -1; // display.background_fps: nothing drawn while unfocused
inline constexpr int kBackgroundTickHz = 10;
inline constexpr int kLightScreenCap = 240; // display.max_fps 0 on a login/loading screen counts as this

struct FramePace {
    int cap = 0;      // the loop's frames a second (0 none)
    bool draw = true; // this frame is drawn
};

// display.max_fps (and --max-fps auto): a cap just under the display's refresh rate (auto_frame_cap). Hailstone's own.
inline constexpr int kMaxFpsAuto = -1;

// The cap kMaxFpsAuto takes for a display refreshing `refresh_hz` times a second: refresh - refresh^2 / 3600, rounded
// down (59 at 60 Hz, 116 at 120, 138 at 144, 224 at 240). On a variable-refresh display that keeps every frame inside
// its range, a little longer than its shortest refresh, so vsync never takes over and its queue never fills; on a fixed
// one it is a frame rate that vsync shows with an occasional repeated frame. 0 (no cap) when the refresh is unknown.
inline int auto_frame_cap(float refresh_hz) {
    if (!(refresh_hz >= 10.0f) || !(refresh_hz <= 1000.0f)) return 0;
    return std::max(10, static_cast<int>(refresh_hz - refresh_hz * refresh_hz / 3600.0f));
}

// display.max_fps as it is kept: 0 (none), kMaxFpsAuto, or 10 .. 1000.
inline int max_fps_setting(int value) {
    return value == kMaxFpsAuto ? kMaxFpsAuto : value <= 0 ? 0 : std::clamp(value, 10, 1000);
}

// The cap this frame: display.max_fps, or under kMaxFpsAuto the cap for the window's display as it is now.
inline int effective_max_fps(int max_fps, float refresh_hz) {
    return max_fps == kMaxFpsAuto ? auto_frame_cap(refresh_hz) : std::max(max_fps, 0);
}

// display.background_fps as it is kept: 0 (as max_fps), kBackgroundNoDraw, or 1 .. 1000.
inline int background_fps_setting(int value) {
    return value < 0 ? kBackgroundNoDraw : value == 0 ? 0 : std::min(value, 1000);
}

// The light-screen condition, kept here so it is testable without the heavy client_app_internal.h. in_game is
// screen_ == Screen::InGame; settling is zone_settling(). The last stretch of a zone-in still draws the loading
// overlay while in_game is already true, so settling must keep the screen light.
inline bool is_light_screen(bool in_game, bool settling) {
    return !in_game || settling;
}

// max_fps 0: none. background_fps 0: as max_fps; kBackgroundNoDraw: none drawn; else drawn frames a second.
// since_drawn: the time since the last drawn frame.
// light_screen: a login/loading screen, where an unlimited cap would spin; max_fps 0 counts as kLightScreenCap there.
// Callers pass false during normal play (so max_fps 0 stays unlimited); a set max_fps is followed exactly either way.
// minimized: the window is minimised, so no frame can be drawn; the loop runs at kBackgroundTickHz whatever
// background_fps says (with the defaults it would otherwise spin at once through every frame the GPU has no image for).
inline FramePace frame_pace(int max_fps, int background_fps, bool focused, std::chrono::microseconds since_drawn,
    bool light_screen = false, bool minimized = false) {
    const int cap = light_screen && max_fps == 0 ? kLightScreenCap : max_fps;
    FramePace pace{cap, true};
    if (minimized) {
        pace.cap = cap > 0 ? std::min(cap, kBackgroundTickHz) : kBackgroundTickHz;
        return pace;
    }
    if (focused || background_fps == 0) return pace;
    const int loop = std::max(background_fps, kBackgroundTickHz);
    pace.cap = cap > 0 ? std::min(cap, loop) : loop;
    if (background_fps == kBackgroundNoDraw) pace.draw = false;
    else if (background_fps < kBackgroundTickHz)
        // Half a tick early: the loop's ticks land on the interval or just after it, not a whole tick late.
        pace.draw = since_drawn >= std::chrono::microseconds(1000000 / background_fps - 500000 / pace.cap);
    return pace;
}

// The moment the next frame may start under a cap of one frame per `period`: one period after the last such moment,
// or `now` when that has passed (a frame that ran long). Timed from the deadline, not from when the wait ended, so the
// wait's own lateness is not added every frame; a late frame is not made up by running the next ones back to back,
// which sent the GPU frames faster than it presented them and made a later frame wait (worse 1 % lows).
template <class TimePoint, class Duration>
TimePoint next_frame_deadline(TimePoint last_deadline, TimePoint now, Duration period) {
    const TimePoint next = last_deadline + period;
    return next < now ? now : next;
}

} // namespace hailstone::app
