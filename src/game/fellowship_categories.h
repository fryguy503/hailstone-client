#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hailstone { class GameDir; }

namespace hailstone::game {

struct FellowshipCategory {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t mask() const { return id > 0 && id < 32 ? std::uint32_t{1} << id : 0; }
};

inline constexpr std::size_t kMaxFellowshipCategoryBytes = 64 * 1024;

// Resources/campsitecategories.txt: id^display text. Category labels remain in
// the player's game files; identifiers determine the native query bit masks.
std::vector<FellowshipCategory> parse_fellowship_categories(std::span<const std::byte> bytes);
std::vector<FellowshipCategory> load_fellowship_categories(const GameDir& game);

} // namespace hailstone::game
