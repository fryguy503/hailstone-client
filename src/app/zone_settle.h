#pragma once

// The loading screen's last stretch after a zone-in. The original loads everything it shows before it leaves the
// loading screen (ProcessGame's zone-in runs to "Initialization complete." with the zone, its actors and the UI built),
// so nothing appears afterwards. This client builds some of it as the
// world is first drawn: the characters' appearances (built on render::ActorRenderer's worker, put up within its build
// budget), the light shadows' cubes one a frame (render::kPointShadowBuildsPerFrame), the UI pack's windows on their
// first frame. So once the network's zone-in is done (OP_ClientReady sent) the game runs and draws as usual behind the
// loading screen until those are done, for at least kMinFrames frames and at most kTimeout seconds (a model or texture
// that never comes cannot keep the player out). The connection is polled every frame meanwhile, as always.

#include <algorithm>
#include <cstddef>
#include <string>

namespace hailstone::app {

// The loading cover keeps the in-game windows from taking mouse presses while the world settles. A native plugin
// question must remain answerable above it; /safelock still owns the mouse even while that question is open.
inline bool ui_mouse_locked(bool in_game, bool interface_locked, bool settling, bool consent_pending) {
    return in_game && (interface_locked || (settling && !consent_pending));
}

// What a frame drawn behind the loading screen still left undone.
struct ZoneSettleFrame {
    std::size_t actors_waiting = 0; // spawns in view drawn without their appearance (ActorRenderer stats, undressed)
    std::size_t shadow_builds = 0;  // light shadow cubes drawn this frame (render::plan_point_shadows' builds)
    bool scene = false;             // the world was drawn (the zone, its objects, flora, sky, spawns)
    bool ui = false;                // the UI pack ran its in-game frame
};

class ZoneSettle {
public:
    static constexpr int kMinFrames = 3;       // the world drawn this often (the frames in flight see it all)
    static constexpr double kTimeout = 15.0;   // seconds at most behind the loading screen

    void begin(double now) {
        *this = ZoneSettle{};
        active_ = true;
        start_ = now;
    }
    void cancel() { active_ = false; }
    bool active() const { return active_; }
    bool timed_out() const { return timed_out_; }
    double seconds(double now) const { return now - start_; }
    int frames() const { return frames_; }
    std::size_t most_waiting() const { return most_waiting_; } // the most spawns waiting for an appearance at once

    // One frame drawn behind the loading screen; true when the loading screen can go (and the settle ends).
    bool update(const ZoneSettleFrame& f, double now) {
        if (!active_) return false;
        if (f.scene && f.ui) ++frames_;
        waiting_ = f.actors_waiting;
        shadows_ = f.shadow_builds;
        most_waiting_ = std::max(most_waiting_, waiting_);
        const bool done = frames_ >= kMinFrames && waiting_ == 0 && shadows_ == 0;
        timed_out_ = !done && now - start_ >= kTimeout;
        if (done || timed_out_) active_ = false;
        // The bar never goes back: more spawns arriving add to what is left, not to what was done.
        float frames = static_cast<float>(std::min(frames_, kMinFrames)) / kMinFrames;
        float actors = most_waiting_ ? 1.0f - static_cast<float>(waiting_) / static_cast<float>(most_waiting_) : 1.0f;
        float shadows = shadows_ ? 0.0f : 1.0f;
        progress_ = std::max(progress_, (frames + actors + shadows) / 3.0f);
        if (!active_) progress_ = 1.0f;
        return !active_;
    }

    // 0..1 for the loading screen's bar.
    float progress() const { return progress_; }
    // What it is waiting for, for the loading screen's line.
    std::string detail() const {
        if (waiting_) return "Dressing characters (" + std::to_string(waiting_) + " left)";
        if (shadows_) return "Casting shadows";
        return "Entering the world";
    }

private:
    bool active_ = false;
    bool timed_out_ = false;
    double start_ = 0.0;
    int frames_ = 0;
    std::size_t waiting_ = 0, most_waiting_ = 0, shadows_ = 0;
    float progress_ = 0.0f;
};

} // namespace hailstone::app
