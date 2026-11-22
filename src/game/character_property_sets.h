#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hailstone::game {

// Resources/CharacterPropertySets.txt: set ID -> member property IDs (loaded by the table reader @0x7e4ac0; looked up
// by set ID @0x7e4940).
class CharacterPropertySets {
public:
    static CharacterPropertySets parse(std::string_view text);
    std::span<const std::uint32_t> find(std::uint32_t set_id) const;
    // No sets: the file is absent or has no rows (a RoF2 install always carries it).
    bool empty() const { return sets_.empty(); }

private:
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> sets_;
};

} // namespace hailstone::game
