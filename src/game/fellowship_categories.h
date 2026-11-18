#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone { class GameDir; }

namespace hailstone::game {

struct KitCategory {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t mask() const { return id > 0 && id < 32 ? std::uint32_t{1} << id : 0; }
};

inline constexpr std::size_t kMaxKitCategoryBytes = 64 * 1024;

// CampsiteCategories.txt and BannerCategories.txt use id^display text.
// Category labels remain in the player's game files; identifiers determine the query bit masks.
std::vector<KitCategory> parse_kit_categories(std::span<const std::byte> bytes);
std::vector<KitCategory> load_kit_categories(const GameDir& game, std::string_view resource);

} // namespace hailstone::game
