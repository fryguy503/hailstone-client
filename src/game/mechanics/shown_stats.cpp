#include "game/mechanics/shown_stats.h"

#include <algorithm>

#include "core/wrap_math.h"
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
// SPA 341, and the buffs' ATK (SPA 2; spell 2434 moves 100 of it to the capped part), plus the active leaders'
// offense enhancement. The profile's +0x3468 bonus is not kept.
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
    return std::max(v + item + buff + ch.sheet().leadership_offense_bonus, 1);
}

std::int32_t shown_ac_value(std::int32_t defense, std::int32_t ac) {
    // 0x580330 adds the two, multiplies by 1000 (imul r32, so the product keeps its low 32 bits and wraps past
    // x = 2147483) and divides by 847 with the compiler's multiply by 0x9abf9f49, add, sar 9 and sign fix-up, which is
    // the exact truncating signed division for every 32-bit dividend.
    const std::uint32_t sum = static_cast<std::uint32_t>(defense) + static_cast<std::uint32_t>(ac);
    return core::wrapped_div(sum * 1000u, 847);
}

std::int64_t shown_ac(const Character& ch) { return shown_ac_value(ch.compute_defense(true), ch.stats().ac); }

std::int32_t shown_atk_value(std::int32_t offense, std::int32_t attack) {
    // 0x584f60 adds the two (lea), multiplies by 1000 (imul r32, wrapping past a sum of 2147483) and divides by 744
    // with the compiler's multiply by 0x2c0b02c1, sar 7 and sign fix-up: the truncating signed division, as for AC.
    const std::uint32_t sum = static_cast<std::uint32_t>(offense) + static_cast<std::uint32_t>(attack);
    return core::wrapped_div(sum * 1000u, 744);
}

std::int64_t shown_atk(const Character& ch, bool spell_2434) {
    // 0x584f60 returns 0 when the character's +8 (its own player spawn) is null; the player always has one here.
    std::uint32_t skill = attack_skill(ch);
    return shown_atk_value(offense_value(ch, skill, true), attack_value(ch, skill, spell_2434));
}

bool has_spell_2434(const Character& ch) {
    for (const auto& b : ch.sheet().buffs)
        if (!b.empty() && b.spell_id == 2434) return true;
    return false;
}

} // namespace hailstone::game::mechanics
