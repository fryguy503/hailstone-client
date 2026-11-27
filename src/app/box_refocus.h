#pragma once

// A box login's first window takes the focus back once the set's other windows have opened (ClientApp's
// start_box_login starts them; each takes the focus as it appears). The first window sees only its own focus: when
// it has lost it, it waits a moment for the others to finish appearing, then raises itself. It does so at most once
// for each window it started, and only for a short while after starting them, so a later change of focus (the
// player picking another window) is left alone.

#include <chrono>
#include <optional>

namespace hailstone::app {

class BoxRefocus {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kSettle{1500}; // after the focus went: the others' windows settle
    static constexpr std::chrono::seconds kWindow{20};        // from the start: when it stops watching

    // `windows`: how many other windows were started (0: nothing to watch).
    void start(int windows, Clock::time_point now) {
        remaining_ = windows;
        until_ = now + kWindow;
        raise_at_.reset();
    }
    bool watching() const { return remaining_ > 0; }

    // Once a frame with whether the window has the focus: true when it should raise itself now.
    bool step(bool focused, Clock::time_point now) {
        if (remaining_ <= 0) return false;
        if (now >= until_) {
            remaining_ = 0;
            raise_at_.reset();
            return false;
        }
        if (focused) {
            raise_at_.reset();
            return false;
        }
        if (!raise_at_) raise_at_ = now + kSettle;
        if (now < *raise_at_) return false;
        raise_at_.reset();
        --remaining_;
        return true;
    }

private:
    int remaining_ = 0;
    Clock::time_point until_{};
    std::optional<Clock::time_point> raise_at_;
};

} // namespace hailstone::app
