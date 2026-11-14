#include "game/mechanics/shown_stats.h"

#include <algorithm>

#include "game/item_moves.h"
#include "game/mechanics/character.h"
#include "game/mechanics/item_stats.h"

namespace hailstone::game::mechanics {

std::uint32_t attack_skill(const Character& ch) {
    const auto& primary = ch.sheet().possessions[13];
    if (primary && primary->id != 0 && primary->item_class == 0 &&
        game::item_is_weapon(primary->item_type, static_cast<std::int32_t>(primary->damage)))
        return item_type_to_skill(primary->item_type);
    return 28;
}

// Offense (33) with its item mods + 7, plus the weapon skill's when the character has it; for a PC, a drunk one
// (drunkenness / 2 above 20, with buffs) scaled by min(1, (110 - drunkenness / 2) * 0.01), else a berserking one +
// level * 2 / 5. At least 1.
std::int32_t offense_value(const Character& ch, std::uint32_t skill, bool buffs) {
    std::int32_t v = ch.skill_with_item_mods(33, buffs) + 7;
    if (ch.adjusted_skill(skill) > 0) v += ch.skill_with_item_mods(skill, buffs);
    const auto& sh = ch.sheet();
    if (sh.spawn_type == SpawnType::PC) {
        std::int32_t half = sh.drunkenness / 2;
        if (buffs && static_cast<double>(half) > 20.0) {
            double f = std::min(1.0, (110.0 - static_cast<double>(half)) * 0.01);
            v = static_cast<std::int32_t>(f * static_cast<double>(v));
        } else if (sh.berserk) {
            v += sh.level * 2 / 5;
        }
    }
    return std::max(v, 1);
}

// The weapon skill with its item mods (when the character has the skill), (2 * stat - 150) / 3 for STR (DEX for
// archery 7 and throwing 51) of 75 and above, the items' ATK (+0x110) and the spell cache's item ATK capped at 250 +
// SPA 341, and the buffs' ATK (SPA 2; spell 2434 moves 100 of it to the capped part). Not kept: the leadership
// offense enhancement (Leadership_GetOffenseEnhancementBonus, no packet source) and the profile's +0x3468 bonus.
std::int32_t attack_value(const Character& ch, std::uint32_t skill, bool spell_2434) {
    std::int32_t cap = ch.total_effect(341, true, 0, true, true) + 250;
    std::int32_t v = ch.adjusted_skill(skill) > 0 ? ch.skill_with_item_mods(skill, true) : 0;
    std::int32_t stat = skill == 7 || skill == 51 ? ch.stats().stats.dex : ch.stats().stats.str;
    if (stat >= 75) v += (2 * stat - 150) / 3;
    std::int32_t buff = ch.total_effect(2, true, 0, false, true);
    std::int32_t item = ch.cached_item(2, 0);
    if (spell_2434) {
        item += 100;
        buff -= 100;
    }
    item = std::min(item + ch.stats().attack, cap);
    return std::max(v + item + buff, 1);
}

std::int64_t shown_ac(const Character& ch) {
    std::int64_t x = static_cast<std::int64_t>(ch.compute_defense(true)) + ch.stats().ac;
    // x * 1000 / 847, the signed division by a multiply with 0x9abf9f49 and shifts, as the original computes it
    std::int64_t v = x * 1000;
    std::int64_t hi = (v * static_cast<std::int64_t>(static_cast<std::int32_t>(0x9abf9f49))) >> 32;
    std::int64_t q = (hi + v) >> 9;
    q += (q < 0) ? 1 : 0;
    return q;
}

std::int64_t shown_atk(const Character& ch, bool spell_2434) {
    std::uint32_t skill = attack_skill(ch);
    return (static_cast<std::int64_t>(offense_value(ch, skill, true)) + attack_value(ch, skill, spell_2434)) * 1000 / 744;
}

bool has_spell_2434(const Character& ch) {
    for (const auto& b : ch.sheet().buffs)
        if (!b.empty() && b.spell_id == 2434) return true;
    return false;
}

} // namespace hailstone::game::mechanics
