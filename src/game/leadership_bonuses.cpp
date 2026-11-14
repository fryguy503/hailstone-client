#include "game/leadership_bonuses.h"

namespace hailstone::game {

LeadershipBonuses leadership_passive_bonuses(const std::array<std::uint32_t, 32>& ranks,
                                            bool group_active, bool raid_active) {
    // Native leadership effects (0x55fd00 regeneration, 0x55fdf0 offense); GetHPRegen adds the former directly.
    constexpr std::array<std::int32_t, 4> vitals{0, 30, 60, 100}, regen{0, 1, 2, 3};
    constexpr std::array<std::int32_t, 6> offense{0, 7, 15, 22, 30, 37};
    auto effect = [&](const auto& values, std::size_t ability) {
        const auto rank = ranks[ability];
        return rank < values.size() ? values[rank] : 0;
    };
    LeadershipBonuses out;
    auto add = [&](std::size_t first) {
        out.offense += effect(offense, first);
        out.mana += effect(vitals, first + 1);
        out.hp += effect(vitals, first + 2);
        out.hp_regen += effect(regen, first + 3);
    };
    if (group_active) add(9);
    if (raid_active) add(24);
    return out;
}

} // namespace hailstone::game
