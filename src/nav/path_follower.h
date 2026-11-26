#pragma once

// Walking a path the way a player would: each step gives the heading to face and the movement keys to hold, never a
// position. The character still moves by the client's own movement rules (game/mechanics: speed, collision,
// gravity), so a navigating character moves exactly as fast and as far as one under a player's hands, and the
// server sees ordinary movement. The heading is set at once, as /follow's FollowPlayerAI sets it (game/follow.h).
//
// Getting unstuck: when the distance to the next corner stops shrinking, the follower jumps and strafes, one side
// then the other; when that does not help it asks for a new path (NeedsPath), and after a few new paths with no
// corner reached it gives up (Failed) rather than running into a wall for ever.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::nav {

enum class FollowState { Idle, Moving, Arrived, NeedsPath, Failed };

struct SteerOutput {
    bool active = false; // false: leave the movement alone
    float yaw = 0.0f;    // world yaw (radians, counterclockwise from +X, game/coords.h)
    bool forward = false;
    bool jump = false;   // pressed this step
    int strafe = 0;      // -1 left, +1 right
    std::optional<float> pitch; // the look pitch (radians, up positive) while swimming; none: leave it
};

struct FollowerTuning {
    float corner_reach = 2.0f;      // a corner counts as reached this close (flat)
    // ... and this close in height, as the end does: a corner on the floor over or under the character is not
    // reached by standing beneath or above it (a ramp that winds over itself, a balcony over a hall).
    float vertical_reach = 8.0f;
    float progress = 0.5f;          // the distance gained that counts as progress
    double stall_seconds = 1.0;     // no progress for this long: stuck
    double unstick_seconds = 0.6;   // how long a jump and strafe lasts
    int unsticks_per_path = 3;      // jumps and strafes before asking for a new path
    int paths_without_corner = 3;   // new paths with no corner reached before giving up
    // A frame longer than this (a hitch: a zone loading, a new model built) gives the character no more time to
    // move than this much; the rest is not counted toward a stall or against a jump and strafe.
    double max_frame_seconds = 0.25;
};

class PathFollower {
public:
    explicit PathFollower(FollowerTuning t = {}) : tune_(t) {}

    // A new goal: the path from the character (points[0]) to the end, finished within `arrive` (flat) of the end,
    // and within the tuning's vertical_reach in height. `check_height` false: flat only (a path whose heights are
    // not the mesh's, the server's).
    void start(std::vector<glm::vec3> points, float arrive, bool check_height = true);
    // A fresh path to the same goal (a moving target, or after NeedsPath): the unstick counters carry on.
    void replace(std::vector<glm::vec3> points, bool check_height = true);
    void stop();
    // A path that grows at its end as what it leads to moves on (/afollow's trail): more points after the last, and
    // how near its new end it finishes. The walk carries on as it is: its corner, stall clock and counters kept.
    void extend(const std::vector<glm::vec3>& more) { points_.insert(points_.end(), more.begin(), more.end()); }
    void set_arrive(float arrive) { arrive_ = arrive; }
    // Moves the last point (a goal that is taken again as it moves, Navigator::moveto_spawn).
    void set_end(const glm::vec3& end) {
        if (!points_.empty()) points_.back() = end;
    }
    // Progress of another kind (a door clicked and opened): the stall clock and the unstick and new-path counts
    // start again, and a walk that asked for a new path carries on along this one.
    void forgive(double now);

    // One frame: where the character's feet are and the time in seconds.
    SteerOutput step(const glm::vec3& feet, double now);

    FollowState state() const { return state_; }
    const std::vector<glm::vec3>& points() const { return points_; }
    std::size_t next() const { return next_; }
    float arrive_distance() const { return arrive_; }
    bool checks_height() const { return check_height_; }
    // The point `distance` (flat) on along the path from where `feet` is level with on the leg being walked (the
    // previous corner to the next), with the path's height there; the next corner when that leg is straight up or
    // down. What a swimmer steers its pitch by, so it keeps to the path's height (a walked path's corners are on the
    // floor, and a straight line at one from above can run into the top of a low opening).
    glm::vec3 ahead(const glm::vec3& feet, float distance) const;
    // The flat length left: from `feet` through the corners still ahead.
    float remaining(const glm::vec3& feet) const;
    const std::string& failure() const { return failure_; }

private:
    void target_changed(double now);
    FollowerTuning tune_;
    FollowState state_ = FollowState::Idle;
    std::vector<glm::vec3> points_;
    std::size_t next_ = 0;
    float arrive_ = 1.0f;
    bool check_height_ = true;
    float best_ = 0.0f;           // nearest the current corner has been
    double progress_at_ = -1.0;   // when best_ last improved; < 0: set on the next step
    double unstick_until_ = -1.0;
    double last_step_ = -1.0;     // the time of the previous step; < 0: none since the walk started
    int unstick_side_ = 1;
    int unsticks_ = 0;
    int paths_ = 0;               // replacements since a corner was last reached
    std::string failure_;
};

} // namespace hailstone::nav
