#include "game/camera_shake.h"

namespace hailstone::game {

void CameraShake::set(float strength, std::uint32_t duration_ms, std::uint32_t now_ms) {
    // fcomp's flags: a current strength greater than the new one skips the update, as does an unordered compare
    // (a NaN new strength); equal falls through.
    if (!(strength >= strength_)) return;
    if (duration_ms > remaining_ms_) {
        remaining_ms_ = duration_ms;
        last_time_ms_ = now_ms;
    }
    strength_ = strength;
}

glm::vec3 CameraShake::offset(const glm::vec3& position, std::uint32_t now_ms) {
    if (remaining_ms_ == 0) return position;
    if (last_time_ms_ == 0) last_time_ms_ = now_ms;
    if (now_ms < last_time_ms_) {
        reset();
        return position;
    }
    // Three rand() draws, x then y then z.
    const auto shaken_axis = [this](float axis) {
        const float r = static_cast<float>(rand_()) * kCameraShakeRandScale - kCameraShakeCentre;
        return axis + r * strength_;
    };
    glm::vec3 shaken;
    shaken.x = shaken_axis(position.x);
    shaken.y = shaken_axis(position.y);
    shaken.z = shaken_axis(position.z);
    const std::uint32_t elapsed = now_ms - last_time_ms_;
    if (elapsed > remaining_ms_) {
        // The last frame's offset still shows; the state is gone (last_time set, unlike reset()).
        remaining_ms_ = 0;
        strength_ = 0.0f;
        last_time_ms_ = now_ms;
        return shaken;
    }
    last_time_ms_ = now_ms;
    remaining_ms_ -= elapsed;
    return shaken;
}

void CameraShake::reset() {
    remaining_ms_ = 0;
    last_time_ms_ = 0;
    strength_ = 0.0f;
}

} // namespace hailstone::game
