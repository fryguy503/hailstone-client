#pragma once

#include <cstdint>

namespace hailstone::game {

inline constexpr int kSpaDiseaseCounter = 35;
inline constexpr int kSpaPoisonCounter = 36;
inline constexpr int kSpaCurseCounter = 116;
inline constexpr int kSpaCorruptionCounter = 369;

constexpr bool is_counter_spa(std::int32_t spa) {
    switch (spa) {
        case kSpaDiseaseCounter:
        case kSpaPoisonCounter:
        case kSpaCurseCounter:
        case kSpaCorruptionCounter:
            return true;
        default:
            return false;
    }
}

// Bit of a counter SPA in the connection's per-update fade mask, or -1 for a non-counter SPA.
constexpr int counter_spa_index(std::int32_t spa) {
    switch (spa) {
        case kSpaDiseaseCounter:
            return 0;
        case kSpaPoisonCounter:
            return 1;
        case kSpaCurseCounter:
            return 2;
        case kSpaCorruptionCounter:
            return 3;
        default:
            return -1;
    }
}

} // namespace hailstone::game
