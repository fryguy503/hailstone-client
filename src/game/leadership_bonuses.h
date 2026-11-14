#pragma once

#include <array>
#include <cstdint>

namespace hailstone::game {

struct LeadershipBonuses {
    std::int32_t hp = 0, mana = 0, hp_regen = 0, offense = 0;
};

// Active leader ranks, rather than the character's purchased ranks. Group and raid effects stack when their
// respective population and expansion checks pass (Leadership_Get*Bonus, 0x55fb90..0x55fdf0).
LeadershipBonuses leadership_passive_bonuses(const std::array<std::uint32_t, 32>& ranks,
                                            bool group_active, bool raid_active);

} // namespace hailstone::game
