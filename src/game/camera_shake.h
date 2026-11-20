#pragma once

// The camera shake of OP_CameraEffect (0x127f), the original's camera effect object (its instance @0x796620 holds
// {u32 remaining ms, u32 last time, f32 strength, f32 offset[3]}):
//   - 0x7965d0 Set(strength, duration): a strength below the current one is ignored, and so is an unordered (NaN)
//     compare; an equal one is not. When it is taken, the duration and its start move only when the duration is
//     longer than what is left; the strength is taken either way. The zone-in path @0x4939b0 resets it (0x7965b0).
//   - 0x796660 Update(position), called from the render path @0x796dd0: with nothing left the position is returned
//     unchanged; the first call after a set starts the clock; a clock that went backwards wipes the state and
//     returns the position. Otherwise each axis gets (rand() / 32767 - 0.5) x strength (the constants @0x9c53e0 and
//     @0x9c4998, rand() drawn x, then y, then z), and the state is wiped once the elapsed time is past the duration
//     (this frame's position is still shaken), else the elapsed time is taken off what is left.
// The shake moves the camera, never the player.

#include <cstdint>
#include <functional>
#include <utility>

#include <glm/glm.hpp>

namespace hailstone::game {

// The original's rand() scale and centre (0x9c53e0, 0x9c4998).
inline constexpr std::int32_t kCameraShakeRandMax = 32767;
inline constexpr float kCameraShakeRandScale = 1.0f / static_cast<float>(kCameraShakeRandMax); // 3.0518509e-05
inline constexpr float kCameraShakeCentre = 0.5f;

class CameraShake {
public:
    // `rand` returns 0..32767 like the C library's rand().
    explicit CameraShake(std::function<std::int32_t()> rand) : rand_(std::move(rand)) {}

    // Set @0x7965d0: a stronger (or equal) strength replaces; NaN and a weaker one are ignored.
    void set(float strength, std::uint32_t duration_ms, std::uint32_t now_ms);
    // Update @0x796660: `position` shaken this frame (the offset is the caller's to apply).
    glm::vec3 offset(const glm::vec3& position, std::uint32_t now_ms);
    // Reset @0x7965b0: the zone-in path @0x4939b0 clears the effect.
    void reset();

    bool active() const { return remaining_ms_ != 0; }
    float strength() const { return strength_; }
    std::uint32_t remaining_ms() const { return remaining_ms_; }

private:
    std::function<std::int32_t()> rand_;
    std::uint32_t remaining_ms_ = 0;  // +0x00
    std::uint32_t last_time_ms_ = 0;  // +0x04
    float strength_ = 0.0f;           // +0x08
};

} // namespace hailstone::game
