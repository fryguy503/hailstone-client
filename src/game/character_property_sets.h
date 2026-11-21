#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hailstone::game {

class CharacterPropertySets {
public:
    static CharacterPropertySets parse(std::string_view text);
    std::span<const std::uint32_t> find(std::uint32_t set_id) const;

private:
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> sets_;
};

} // namespace hailstone::game
