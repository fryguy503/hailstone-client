#pragma once

#include <array>
#include <cstdint>

namespace hailstone::core {

struct BannerAppearance {
    std::array<std::int32_t, 2> pattern{};
    std::array<std::uint32_t, 2> tint{}; // packed ARGB, cloth then emblem

    bool operator==(const BannerAppearance&) const = default;
};

} // namespace hailstone::core
