#include "nav/path_follower.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hailstone::nav {

namespace {
float flat(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(b - a)); }
} // namespace

void PathFollower::start(std::vector<glm::vec3> points, float arrive, bool check_height) {
    paths_ = 0;
    unsticks_ = 0;
    arrive_ = std::max(arrive, 0.5f);
    failure_.clear();
    replace(std::move(points), check_height);
}

void PathFollower::replace(std::vector<glm::vec3> points, bool check_height) {
    check_height_ = check_height;
    points_ = std::move(points);
    if (points_.empty()) {
        state_ = FollowState::Failed;
        failure_ = "no path";
        return;
    }
    if (state_ == FollowState::NeedsPath) ++paths_;
    // points[0] is where the character stood when the path was found.
    next_ = points_.size() > 1 ? 1 : 0;
    state_ = FollowState::Moving;
    progress_at_ = -1.0;
    unstick_until_ = -1.0;
    last_step_ = -1.0;
}

void PathFollower::stop() {
    state_ = FollowState::Idle;
    points_.clear();
    next_ = 0;
}

void PathFollower::forgive(double now) {
    if (state_ == FollowState::NeedsPath) state_ = FollowState::Moving;
    unsticks_ = 0;
    paths_ = 0;
    unstick_until_ = -1.0;
    target_changed(now);
}

void PathFollower::target_changed(double now) {
    best_ = std::numeric_limits<float>::max();
    progress_at_ = now;
}

glm::vec3 PathFollower::ahead(const glm::vec3& feet, float distance) const {
    if (next_ >= points_.size()) return feet;
    if (next_ == 0) return points_[0];
    glm::vec3 a = points_[next_ - 1], b = points_[next_];
    glm::vec2 ab(b - a);
    float len = glm::length(ab);
    if (len < 0.5f) return b; // straight up or down
    float t = std::clamp(glm::dot(glm::vec2(feet - a), ab) / (len * len), 0.0f, 1.0f);
    float left = distance;
    float along = (1.0f - t) * len; // flat length from the level point to b
    if (left <= along) return a + (b - a) * (t + left / len);
    left -= along;
    for (std::size_t i = next_ + 1; i < points_.size(); ++i) {
        glm::vec3 p = points_[i - 1], q = points_[i];
        float l = flat(p, q);
        if (l < 0.5f) return p; // a vertical leg next: hold its top (or foot) until there
        if (left <= l) return p + (q - p) * (left / l);
        left -= l;
    }
    return points_.back();
}

float PathFollower::remaining(const glm::vec3& feet) const {
    if (next_ >= points_.size()) return 0.0f;
    float d = flat(feet, points_[next_]);
    for (std::size_t i = next_ + 1; i < points_.size(); ++i) d += flat(points_[i - 1], points_[i]);
    return d;
}

SteerOutput PathFollower::step(const glm::vec3& feet, double now) {
    SteerOutput out;
    if (state_ != FollowState::Moving || points_.empty()) return out;
    // A hitch: the time past max_frame_seconds was not time to move in.
    if (last_step_ >= 0.0 && now - last_step_ > tune_.max_frame_seconds) {
        double lost = now - last_step_ - tune_.max_frame_seconds;
        if (progress_at_ >= 0.0) progress_at_ += lost;
        if (unstick_until_ > last_step_) unstick_until_ += lost;
    }
    last_step_ = now;
    if (progress_at_ < 0.0) target_changed(now);

    // Corners passed: the next one is within reach (the last one only within the arrival distance).
    auto level = [&](const glm::vec3& p) { return !check_height_ || std::abs(p.z - feet.z) <= tune_.vertical_reach; };
    while (next_ + 1 < points_.size() && flat(feet, points_[next_]) <= tune_.corner_reach && level(points_[next_])) {
        ++next_;
        paths_ = 0;
        unsticks_ = 0;
        target_changed(now);
    }
    const glm::vec3& target = points_[next_];
    float d = flat(feet, target);
    if (next_ + 1 == points_.size() && d <= arrive_ && level(target)) {
        state_ = FollowState::Arrived;
        return out;
    }
    // Within the arrival distance of the end along the path, with corners still ahead: done there, not at the last
    // corner (a /nav distance=45 stops 45 short, however the path bends round the goal). Along the path, never
    // straight across: a goal behind a wall is not reached through it. In height the end may be as far off as the
    // distance itself, or vertical_reach when that is more.
    if (next_ + 1 < points_.size() && remaining(feet) <= arrive_ &&
        (!check_height_ || std::abs(points_.back().z - feet.z) <= std::max(tune_.vertical_reach, arrive_))) {
        state_ = FollowState::Arrived;
        return out;
    }

    // Progress counts height too (a fall, a dive, a climb up a column of water are progress), except for the
    // server's path, whose heights are not ours.
    float gap = check_height_ ? glm::distance(feet, target) : d;
    if (gap < best_ - tune_.progress || best_ == std::numeric_limits<float>::max()) {
        best_ = gap;
        progress_at_ = now;
    }
    out.active = true;
    out.yaw = std::atan2(target.y - feet.y, target.x - feet.x);
    out.forward = true;

    if (now < unstick_until_) {
        out.strafe = unstick_side_;
        return out;
    }
    if (now - progress_at_ >= tune_.stall_seconds) {
        if (unsticks_ >= tune_.unsticks_per_path) {
            unsticks_ = 0;
            if (paths_ >= tune_.paths_without_corner) {
                state_ = FollowState::Failed;
                failure_ = "stuck";
                out = {};
                return out;
            }
            state_ = FollowState::NeedsPath;
            return out;
        }
        ++unsticks_;
        unstick_side_ = -unstick_side_;
        unstick_until_ = now + tune_.unstick_seconds;
        progress_at_ = now;
        out.jump = true;
        out.strafe = unstick_side_;
    }
    return out;
}

} // namespace hailstone::nav
