// Stats, resists, caps, hit points / mana / endurance, armor class, haste,
// weight: CharacterZoneClient::ProcessAllStats and the functions it runs.
//
// Checked against the original's vectors unless noted:
//  - the heroic getters, basesave_*, CalcSavePhysical, the racial bonus, the mod caps
//  - CalcSave*, CalcSTR..CalcWIS, Cur_Mana/Cur_Endurance, ComputeDefense,
//    GetStatCap, MaxAirSupply, GetMaxLevel, GetClassACBonus
//  - the class/level table, Max_HP, Max_Mana, Max_Endurance, Cur_HP
//  - CalcCarriedWeight, CalculateWeight, CalcAGI
//  - CalcAC, CalcItemHaste,
//    the food and drink stats, CanUseItem
//  - ProcessAllStats @0x4547e0 itself: no vectors; worked from the disassembly
//    (0x585190, the recommended-level scaling, too).
// The x87 formulas follow the original's instructions one by one through Fpu.
#include "game/mechanics/character.h"
#include "core/wrap_math.h"
#include "game/mechanics/item_sums.h"

#include <algorithm>

#include "game/mechanics/item_stats.h"
#include "game/spells.h"

namespace hailstone::game::mechanics {

namespace {

std::int32_t add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
// a - b with the original's 32-bit wrap (negating INT_MIN first would be undefined).
std::int32_t sub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
std::int32_t mul(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
double d(std::int32_t v) { return static_cast<double>(v); }
double f(float v) { return static_cast<double>(v); }

constexpr std::uint32_t kExpansionLeadershipBonus = 0x40;
constexpr std::uint32_t kExpansionHeroicStats = 0x2000;

// HasBit of the race / deity checks: bit n (1-based) of the mask.
bool has_bit(std::uint32_t mask, std::int32_t bit) {
    return (mask & (1u << ((static_cast<std::uint32_t>(bit) - 1u) & 31))) != 0;
}
// CharacterZoneClient_CanUseItemRace @0x446200: the race's item bit.
std::int32_t race_bit(std::uint32_t race) {
    auto r = static_cast<std::int32_t>(race);
    if (r < 0xd) return r;
    if (r > 0x14a) return r == 0x20a ? 0x10 : 0x11;
    if (r == 0x14a) return 0xf;
    if (r == 0x80) return 0xd;
    if (r == 0x82) return 0xe;
    return 0x11;
}

// Diminishing returns of the mana and endurance stats (Max_Mana, Max_Endurance).
std::int32_t stat_with_diminishing_returns(std::int32_t stat) {
    if (stat > 100) {
        if (stat > 200) stat = add(stat, -(add(stat, -200) / 2));
        stat = add(stat, core::wrapped_div(static_cast<std::uint32_t>(stat) * 3u - 300u, 2));
    }
    return stat;
}

// SelectByWisdomCasterClass @0x443520.
std::int32_t select_by_wisdom_caster(std::int32_t cls, std::int32_t wisdom, std::int32_t intelligence) {
    switch (cls) {
    case 2: case 3: case 4: case 6: case 10: case 15: return wisdom;
    default: return intelligence;
    }
}

std::int32_t I(std::int8_t v) { return v; }

} // namespace

// ---- base profile values and the class/level table ----

double Character::base_hp() const {
    if (sheet_.profile_type != 0) return d(sheet_.own_base_hp);
    const ClassLevelRow* r = tables_.class_levels.row(sheet_.profile_level.value_or(sheet_.level), sheet_.class_id);
    return r ? r->base_hp : 0.0;
}
std::int32_t Character::base_mana() const {
    if (sheet_.profile_type != 0) return sheet_.own_base_mana;
    const ClassLevelRow* r = tables_.class_levels.row(sheet_.profile_level.value_or(sheet_.level), sheet_.class_id);
    return Fpu::ftol(r ? r->base_mana : 0.0);
}
std::int32_t Character::base_endurance() const {
    if (sheet_.profile_type != 0) return sheet_.own_base_endurance;
    const ClassLevelRow* r = tables_.class_levels.row(sheet_.profile_level.value_or(sheet_.level), sheet_.class_id);
    return Fpu::ftol(r ? r->base_endurance : 0.0);
}
std::int32_t Character::base_hp_regen() const {
    if (hooks.base_hp_regen)
        if (auto v = hooks.base_hp_regen()) return *v;
    if (sheet_.profile_type != 0) return sheet_.own_hp_regen;
    const ClassLevelRow* r = tables_.class_levels.row(sheet_.profile_level.value_or(sheet_.level), sheet_.class_id);
    return Fpu::ftol(r ? r->hp_regen : 0.0);
}
std::int32_t Character::base_endurance_regen() const {
    if (hooks.base_endurance_regen)
        if (auto v = hooks.base_endurance_regen()) return *v;
    if (sheet_.profile_type != 0) return sheet_.own_endurance_regen;
    const ClassLevelRow* r = tables_.class_levels.row(sheet_.profile_level.value_or(sheet_.level), sheet_.class_id);
    return Fpu::ftol(r ? r->endurance_regen : 0.0);
}

// ---- item use ----

// CharacterZoneClient::CanUseItem @0x446470 (the "light" variant: level, race, deity, prestige).
bool Character::can_use_item(const net::Item& item, bool prestige) const {
    if (item_required_level(item) > sheet_.level) return false;
    if (sheet_.ignores_item_restrictions) return true;
    if (!has_bit(item_races(item), race_bit(sheet_.race))) return false;
    // CanUseItemDeity @0x4462c0: no owner object or no deity mask passes; deity 396 is bit 1, others deity - 199.
    if (std::uint32_t deities = item_deity(item); deities != 0 && sheet_.has_owner) {
        auto deity = static_cast<std::int32_t>(sheet_.deity);
        if (!has_bit(deities, deity == 0x18c ? 1 : deity - 0xc7)) return false;
    }
    if (prestige && sheet_.prestige_limit != -1 && static_cast<std::int32_t>(item.prestige) > sheet_.prestige_limit)
        return false; // ItemBase_GetPrestige(0): the item's own
    return true;
}

// ProcessAllStats' worn-slot check (inline there): level, then (unless the base allows any item) race, deity
// and prestige.
bool Character::usable_for_stats(const net::Item& item) const { return can_use_item(item, true); }

// 0x585190 (CharacterZoneClient vtable +0x34): an item stat of an item with a recommended level or skill
// above the character's, scaled down: floor(level / recLevel * value + 0.5), at least 1; or with the skill
// (GetSkillWithItemMods) below the recommended skill, the raw skill (at least 1) / recSkill instead.
std::int32_t Character::recommended_item_value(const net::Item& item, std::int32_t value, std::uint8_t skill) const {
    if (hooks.recommended_item_ac)
        if (auto v = hooks.recommended_item_ac(value, skill)) return *v;
    if (value <= 0) return value;
    double ratio;
    std::int32_t rec = item_recommended_level(item);
    if (rec != 0 && sheet_.level < rec) {
        // level (exact as a float) / rec
        ratio = fpu_.div(d(sheet_.level), d(rec));
    } else {
        std::int32_t rec_skill = item_recommended_skill(item);
        if (rec_skill == 0) return value;
        if (skill_with_item_mods(skill, true) >= rec_skill) return value;
        std::int32_t raw = skill < kSkills ? sheet_.skills[skill] : 0;
        if (raw < 1) raw = 1;
        ratio = fpu_.div(d(raw), d(rec_skill));
    }
    // ratio * value + 0.5f, stored as a double, _floor, truncated
    double x = fpu_.add(fpu_.mul(ratio, d(value)), 0.5);
    std::int32_t r = Fpu::ftol(std::floor(x));
    return r < 1 ? 1 : r;
}

// ---- heroic bonuses, caps ----

std::int32_t Character::heroic(std::int32_t DerivedStats::*field) const {
    if (!sheet_.is_pc()) return 0;
    return (sheet_.expansions & kExpansionHeroicStats) ? d_.*field : 0;
}

std::int32_t Character::stat_cap(int index) const {
    static constexpr std::int32_t DerivedStats::*kStatHeroic[7] = {
        &DerivedStats::heroic_str, &DerivedStats::heroic_sta, &DerivedStats::heroic_agi, &DerivedStats::heroic_dex,
        &DerivedStats::heroic_wis, &DerivedStats::heroic_int, &DerivedStats::heroic_cha};
    static constexpr std::int32_t DerivedStats::*kResistHeroic[7] = {
        &DerivedStats::heroic_sv_magic, &DerivedStats::heroic_sv_cold, &DerivedStats::heroic_sv_fire,
        &DerivedStats::heroic_sv_poison, &DerivedStats::heroic_sv_disease, nullptr,
        &DerivedStats::heroic_sv_corruption};
    if (static_cast<std::uint32_t>(index) > 13) return -1;
    std::int32_t level = sheet_.level;
    std::int32_t cap;
    std::int32_t DerivedStats::*h;
    if (index >= 7) {
        cap = level > 65 ? add(mul(level, 5), 175) : 500;
        cap = add(cap, total_effect(262, true, index, true, true));
        h = kResistHeroic[index - 7];
    } else {
        cap = level > 60 ? add(mul(level, 5), -45) : 255;
        std::int32_t own = total_effect(262, true, index, true, true);
        cap = add(add(cap, total_effect(262, true, -1, true, true)), own);
        h = kStatHeroic[index];
    }
    if ((sheet_.expansions & kExpansionHeroicStats) && h) cap = add(cap, d_.*h);
    return cap;
}

std::int32_t Character::cap_stat(std::int32_t value, int index) const {
    if (hooks.cap_stat)
        if (auto v = hooks.cap_stat(value, index)) return *v;
    std::int32_t cap = stat_cap(index);
    if (value > cap) value = cap;
    return value < 0 ? 0 : value;
}

std::int32_t Character::mod_cap(int index) {
    // The caps of the mod indices 4..25; -1 (none) for the others.
    static constexpr std::int32_t kCaps[22] = {
        35,  100, 35,  35,  35,  25,  100, 150, 35,  35,  // 4..13
        -1,  -1,  -1,  100, 125,                          // 14..18
        100, 100, 100, 100, 100, 100, 125,                // 19..25
    };
    if (index < 4 || index > 25) return -1;
    return kCaps[static_cast<std::size_t>(index - 4)];
}

std::int32_t Character::cap_mod(std::int32_t value, int index) const {
    std::int32_t cap = mod_cap(index);
    if (value > cap) value = cap;
    return value < 0 ? 0 : value;
}

// ---- base saves (basesave_* @0x444e10..) ----

namespace {
std::int32_t level_scaled_bonus(std::int32_t value, std::int32_t level, std::int32_t bonus, std::int32_t base) {
    return level > 49 ? add(add(value, -base), level) : add(value, bonus);
}
} // namespace

std::int32_t Character::basesave_magic() const {
    std::int32_t v = sheet_.base_save_magic;
    std::uint32_t race = sheet_.race;
    if (race == 3 || race == 8 || race == 330) v = 30;
    if (race == 522) v = sheet_.heritage == 2 ? add(v, 10) : sheet_.heritage == 5 ? add(v, 2) : v;
    if (sheet_.has_spawn && sheet_.spawn_type == SpawnType::NPC && sheet_.level > 24) v = add(v, 10);
    if (sheet_.is_pc() && (sheet_.class_id == 1 || sheet_.class_id == 16)) return add(sheet_.level / 2, v);
    return v;
}
std::int32_t Character::basesave_fire() const {
    std::int32_t v = sheet_.base_save_fire;
    std::uint32_t race = sheet_.race;
    if (race == 9) v = 5;
    if (race == 128) v = add(v, 5);
    if (race == 522) v = sheet_.heritage == 0 ? add(v, 10) : sheet_.heritage == 5 ? add(v, 2) : v;
    if (sheet_.has_spawn && sheet_.spawn_type == SpawnType::NPC && sheet_.level > 24) v = add(v, 10);
    if (!sheet_.is_pc()) return v;
    if (sheet_.class_id == 7) v = level_scaled_bonus(v, sheet_.level, 8, 41);
    if (sheet_.class_id == 4) return level_scaled_bonus(v, sheet_.level, 4, 45);
    return v;
}
std::int32_t Character::basesave_cold() const {
    std::int32_t v = sheet_.base_save_cold;
    std::uint32_t race = sheet_.race;
    if (race == 2) v = 35;
    if (race == 128) v = add(v, -10);
    if (race == 522) v = sheet_.heritage == 4 ? add(v, 10) : sheet_.heritage == 5 ? add(v, 2) : v;
    if (sheet_.has_spawn && sheet_.spawn_type == SpawnType::NPC && sheet_.level > 24) v = add(v, 10);
    if (sheet_.is_pc() && (sheet_.class_id == 4 || sheet_.class_id == 15))
        return level_scaled_bonus(v, sheet_.level, 4, 45);
    return v;
}
std::int32_t Character::basesave_disease() const {
    std::int32_t v = sheet_.base_save_disease;
    std::uint32_t race = sheet_.race;
    if (race == 11) v = 20;
    if (race == 3) v = 10;
    if (race == 522) v = sheet_.heritage == 1 ? add(v, 10) : sheet_.heritage == 5 ? add(v, 2) : v;
    if (sheet_.class_id == 7 && sheet_.level > 50) v = add(add(v, -50), sheet_.level);
    if (!sheet_.is_pc()) return v;
    if (sheet_.class_id == 3) v = level_scaled_bonus(v, sheet_.level, 8, 41);
    if (sheet_.class_id == 15) v = level_scaled_bonus(v, sheet_.level, 4, 45);
    if (sheet_.class_id == 5) return level_scaled_bonus(v, sheet_.level, 4, 45);
    return v;
}
std::int32_t Character::basesave_poison() const {
    std::int32_t v = sheet_.base_save_poison;
    std::uint32_t race = sheet_.race;
    if (race == 11 || race == 8) v = 20;
    if (race == 330) v = 30;
    if (race == 522) v = sheet_.heritage == 3 ? add(v, 10) : sheet_.heritage == 5 ? add(v, 2) : v;
    if (sheet_.class_id == 7 && sheet_.level > 50) v = add(add(v, -50), sheet_.level);
    if (!sheet_.is_pc()) return v;
    if (sheet_.class_id == 9) v = level_scaled_bonus(v, sheet_.level, 8, 41);
    if (sheet_.class_id == 5) return level_scaled_bonus(v, sheet_.level, 4, 45);
    return v;
}

std::int32_t Character::calc_save_physical(bool include_buffs) const {
    std::int32_t v = sheet_.base_save_physical;
    if (sheet_.is_pc()) {
        std::int32_t level = sheet_.level;
        v = add(v, level > 50 ? add(add(mul(level, 3), -150), level / 3) : level / 3);
    }
    return add(include_buffs ? d_.items.sv_physical : d_.items_no_buffs.sv_physical, v);
}

// ---- Calc* ----

std::int32_t Character::store_and_cap(std::int32_t StatBlock::*field, std::int32_t total, int cap_index) {
    d_.uncapped.*field = total;
    return cap_stat(total, cap_index);
}

std::int32_t Character::calc_resist(bool include_buffs, std::int32_t base, std::int32_t spa, bool all_resists,
                                    std::int32_t StatBlock::*field, std::int32_t DerivedStats::*heroic_field,
                                    int cap_index) {
    std::int32_t items = include_buffs ? d_.items.*field : d_.items_no_buffs.*field;
    std::int32_t total;
    if (all_resists) {
        std::int32_t own = total_effect(spa, true, 0, true, include_buffs);
        std::int32_t all = total_effect(111, true, 0, true, include_buffs);
        total = add(add(add(own, all), items), base);
    } else {
        total = add(add(items, total_effect(spa, true, 0, true, include_buffs)), base);
    }
    if (sheet_.is_pc() && (sheet_.expansions & kExpansionHeroicStats) && include_buffs)
        total = add(total, d_.*heroic_field);
    return store_and_cap(field, total, cap_index);
}

std::int32_t Character::calc_save_magic(bool b) {
    return calc_resist(b, basesave_magic(), 50, true, &StatBlock::sv_magic, &DerivedStats::heroic_sv_magic, 7);
}
std::int32_t Character::calc_save_fire(bool b) {
    return calc_resist(b, basesave_fire(), 46, true, &StatBlock::sv_fire, &DerivedStats::heroic_sv_fire, 9);
}
std::int32_t Character::calc_save_cold(bool b) {
    return calc_resist(b, basesave_cold(), 47, true, &StatBlock::sv_cold, &DerivedStats::heroic_sv_cold, 8);
}
std::int32_t Character::calc_save_disease(bool b) {
    return calc_resist(b, basesave_disease(), 49, true, &StatBlock::sv_disease, &DerivedStats::heroic_sv_disease, 11);
}
std::int32_t Character::calc_save_poison(bool b) {
    return calc_resist(b, basesave_poison(), 48, true, &StatBlock::sv_poison, &DerivedStats::heroic_sv_poison, 10);
}
std::int32_t Character::calc_save_corruption(bool b) {
    return calc_resist(b, sheet_.base_save_corruption, 370, false, &StatBlock::sv_corruption,
                       &DerivedStats::heroic_sv_corruption, 13);
}

std::int32_t Character::drunk_stat_shift() const {
    if (sheet_.drunkenness <= 55) return 0;
    std::int32_t shift = add(sheet_.drunkenness, -40) / 16;
    return shift > 0 ? shift : 0;
}

std::int32_t Character::stat_sum(bool include_buffs, std::int32_t spa, std::int32_t StatBlock::*field,
                                 std::int32_t base) {
    std::int32_t all = total_effect(159, true, 0, true, include_buffs);
    std::int32_t own = total_effect(spa, true, 0, true, include_buffs);
    std::int32_t items = include_buffs ? d_.items.*field : d_.items_no_buffs.*field;
    return add(add(base, items), add(all, own));
}

// x87 ScaleByMaxFactor (inlined in CalcINT @0x445ef0, at 0x445fd2, and the same in CalcWIS @0x446040): value *
// max(1 - drunk * 0.005, 0.2), truncated.
std::int32_t Character::drunk_scaled(bool include_buffs, std::int32_t total) const {
    if (!sheet_.is_pc() || !include_buffs || sheet_.drunkenness == 0) return total;
    double factor = fpu_.sub(1.0, fpu_.mul(d(sheet_.drunkenness), 0.005));
    if (!(0.2 < factor || 0.2 == factor)) factor = 0.2; // keep the factor unless below 0.2
    return Fpu::ftol(fpu_.mul(factor, d(total)));
}

std::int32_t Character::calc_str(bool b) {
    std::int32_t extra = 0;
    if (sheet_.is_pc()) {
        if (sheet_.drunkenness > 55) {
            if (b) extra = drunk_stat_shift();
        } else if (sheet_.berserk) {
            extra = sheet_.level / 10 + 9;
        }
    }
    std::int32_t total = add(stat_sum(b, 4, &StatBlock::str, sheet_.base_str), extra);
    if (b) total = add(total, heroic(&DerivedStats::heroic_str));
    return store_and_cap(&StatBlock::str, total, 0);
}
std::int32_t Character::calc_sta(bool b) {
    std::int32_t extra = sheet_.is_pc() && b ? drunk_stat_shift() : 0;
    std::int32_t total = add(stat_sum(b, 7, &StatBlock::sta, sheet_.base_sta), extra);
    if (b) total = add(total, heroic(&DerivedStats::heroic_sta));
    return store_and_cap(&StatBlock::sta, total, 1);
}
std::int32_t Character::calc_cha(bool b) {
    std::int32_t total = stat_sum(b, 10, &StatBlock::cha, sheet_.base_cha);
    if (b) total = add(total, heroic(&DerivedStats::heroic_cha));
    return store_and_cap(&StatBlock::cha, total, 6);
}
std::int32_t Character::calc_dex(bool b) {
    std::int32_t extra = sheet_.is_pc() && b ? -drunk_stat_shift() : 0;
    std::int32_t total = add(stat_sum(b, 5, &StatBlock::dex, sheet_.base_dex), extra);
    if (b) total = add(total, heroic(&DerivedStats::heroic_dex));
    return store_and_cap(&StatBlock::dex, total, 3);
}
std::int32_t Character::calc_int(bool b) {
    std::int32_t total = stat_sum(b, 8, &StatBlock::int_, sheet_.base_int);
    if (b) total = add(total, heroic(&DerivedStats::heroic_int));
    return store_and_cap(&StatBlock::int_, drunk_scaled(b, total), 5);
}
std::int32_t Character::calc_wis(bool b) {
    std::int32_t total = stat_sum(b, 9, &StatBlock::wis, sheet_.base_wis);
    if (b) total = add(total, heroic(&DerivedStats::heroic_wis));
    return store_and_cap(&StatBlock::wis, drunk_scaled(b, total), 4);
}

// CalcAGI @0x454280: base + items + SPA 159 + SPA 6 - drunk + heroic, times CalculateWeight's factor.
std::int32_t Character::calc_agi(bool b) {
    std::int32_t drunk = 0;
    if (sheet_.is_pc() && b && sheet_.drunkenness > 0x37) {
        std::int32_t over = (sheet_.drunkenness - 0x28) / 16;
        if (over > 0) drunk = -over;
    }
    auto effects = static_cast<std::uint32_t>(total_effect(159, true, 0, true, b));
    effects += static_cast<std::uint32_t>(total_effect(6, true, 0, true, b));
    auto stat = static_cast<std::uint32_t>(b ? d_.items.agi : d_.items_no_buffs.agi);
    std::uint32_t agility = static_cast<std::uint32_t>(sheet_.base_agi) + stat + effects + static_cast<std::uint32_t>(drunk);
    if (b && sheet_.is_pc() && (sheet_.expansions & kExpansionHeroicStats))
        agility += static_cast<std::uint32_t>(d_.heroic_agi);
    float factor = calculate_weight();
    // factor * agility, truncated
    std::int32_t result = Fpu::ftol(fpu_.mul(f(factor), d(static_cast<std::int32_t>(agility))));
    d_.uncapped.agi = result;
    return cap_stat(result, 2);
}

// ---- hit points, mana, endurance ----

bool Character::owner_leadership() const {
    return sheet_.leadership_active && (!sheet_.has_owner || (sheet_.owner_expansions & kExpansionLeadershipBonus));
}

std::int32_t Character::max_hp(bool include_buffs, int arg) const {
    if (hooks.max_hp)
        if (auto v = hooks.max_hp(arg, include_buffs)) return *v;
    if (sheet_.vitals.max_hp) return *sheet_.vitals.max_hp; // a server extension's detour (CharacterVitals)
    std::int32_t hp = Fpu::ftol(base_hp());
    hp = add(hp, include_buffs ? d_.hp_bonus : d_.hp_bonus_no_buffs);
    std::int32_t stamina = include_buffs ? d_.stats.sta : d_.stats_no_buffs.sta;
    if (stamina > 255) stamina = add(stamina, -255) / 2 + 255;
    const ClassLevelRow* r = tables_.class_levels.row(sheet_.level, sheet_.class_id);
    hp = add(hp, Fpu::ftol(fpu_.mul(r ? r->hp_per_stamina : 0.0, d(stamina)))); // a double times the integer, truncated
    std::int32_t percent = total_effect(214, true, 0, true, include_buffs);
    hp = add(hp, core::wrapped_mul_div(percent, hp, 10000));
    // Leadership: the owner (virtual +0x4c) missing or with expansion bit 0x40.
    if (sheet_.has_spawn && include_buffs && sheet_.leadership_active &&
        (!sheet_.has_owner || (sheet_.owner_expansions & kExpansionLeadershipBonus)))
        hp = add(hp, sheet_.leadership_hp_bonus);
    hp = add(hp, total_effect(69, true, 0, true, include_buffs));
    if (arg == 0) hp = add(hp, 5);
    std::int32_t h = 0;
    if (sheet_.is_pc() && (sheet_.expansions & kExpansionHeroicStats)) h = d_.heroic_sta;
    hp = add(hp, mul(h, 10));
    if (sheet_.has_spawn && include_buffs) spawn_hp_max_ = hp;
    return hp > 10000000 ? 10000000 : hp;
}

std::int32_t Character::max_mana(bool include_buffs) const {
    if (hooks.max_mana)
        if (auto v = hooks.max_mana(include_buffs)) return *v;
    if (sheet_.vitals.max_mana) return *sheet_.vitals.max_mana;
    switch (sheet_.class_id) {
    case 1: case 7: case 9: case 16: case 40: case 41: return 0;
    default: break;
    }
    std::int32_t mana = base_mana();
    std::int32_t intelligence = include_buffs ? d_.stats.int_ : d_.stats_no_buffs.int_;
    std::int32_t wisdom = include_buffs ? d_.stats.wis : d_.stats_no_buffs.wis;
    std::int32_t cls = sheet_.class_id;
    // ManaFromStat @0x580160
    std::int32_t stat = stat_with_diminishing_returns(select_by_wisdom_caster(cls, wisdom, intelligence));
    const ClassLevelRow* r = tables_.class_levels.row(sheet_.level, sheet_.class_id);
    mana = add(mana, Fpu::ftol(fpu_.mul(r ? r->mana_per_stat : 0.0, d(stat))));
    std::int32_t bonus;
    if (include_buffs) {
        if (sheet_.leadership_active && (sheet_.expansions & kExpansionLeadershipBonus))
            mana = add(mana, sheet_.leadership_mana_bonus);
        bonus = d_.mana_bonus;
    } else {
        bonus = d_.mana_bonus_no_buffs;
    }
    mana = add(mana, add(total_effect(97, true, 0, true, include_buffs), bonus));
    std::int32_t h = select_by_wisdom_caster(cls, heroic(&DerivedStats::heroic_wis), heroic(&DerivedStats::heroic_int));
    return add(mana < 0 ? 0 : mana, mul(h, 10));
}

std::int32_t Character::max_endurance(bool include_buffs) const {
    if (hooks.max_endurance)
        if (auto v = hooks.max_endurance(include_buffs)) return *v;
    if (sheet_.vitals.max_endurance) return *sheet_.vitals.max_endurance;
    std::int32_t base = base_endurance();
    const StatBlock& s = include_buffs ? d_.stats : d_.stats_no_buffs;
    // EnduranceFromStats @0x442620 (a = AGI, b = DEX, c = STA, d = STR)
    std::int32_t average = add(add(add(s.dex, s.agi), s.sta), s.str) / 4;
    std::int32_t stat = stat_with_diminishing_returns(average);
    const ClassLevelRow* r = tables_.class_levels.row(sheet_.level, sheet_.class_id);
    std::int32_t from_stats = Fpu::ftol(fpu_.mul(r ? r->endurance_per_stat : 0.0, d(stat)));
    std::int32_t endurance = add(base, add(total_effect(190, true, 0, true, include_buffs), from_stats));
    std::int32_t bonus = include_buffs ? d_.endurance_bonus : d_.endurance_bonus_no_buffs;
    std::int32_t h = add(add(add(heroic(&DerivedStats::heroic_agi), heroic(&DerivedStats::heroic_dex)),
                             heroic(&DerivedStats::heroic_str)),
                         heroic(&DerivedStats::heroic_sta)) / 4;
    endurance = add(endurance, add(bonus, mul(h, 10)));
    return endurance < 0 ? 0 : endurance;
}

namespace {
// x87 PercentOf (Cur_Mana @0x4443ac): pct * 0.01f * max + 0.99f, truncated.
std::int32_t percent_of(const Fpu& fpu, std::int32_t pct, std::int32_t max) {
    double x = fpu.mul(d(pct), f(0.01f));
    x = fpu.mul(x, d(max));
    x = fpu.add(x, f(0.99f));
    return Fpu::ftol(x);
}
} // namespace

std::int32_t Character::cur_hp(bool update, int arg) const {
    if (hooks.cur_hp)
        if (auto v = hooks.cur_hp(arg, update)) return *v;
    if (sheet_.vitals.cur_hp) return *sheet_.vitals.cur_hp; // nothing clamped or stored
    bool clamp = update && !sheet_.zoning;
    std::int32_t bonus = d_.hp_bonus;
    std::int32_t hp = add(stored_hp_, bonus);
    if (clamp) {
        std::int32_t percent_cap = total_effect(408, true, 0, true, true);
        std::int32_t value_cap = total_effect(408, true, 1, true, true);
        if (percent_cap > 0) {
            std::int32_t limit = percent_of(fpu_, percent_cap, max_hp(true, arg));
            if (hp > limit) {
                hp = limit;
                stored_hp_ = std::max(sub(limit, bonus), 1);
            }
        }
        if (value_cap > 0 && hp > value_cap) {
            hp = value_cap;
            stored_hp_ = std::max(sub(value_cap, bonus), 1);
        }
    }
    if (sheet_.has_spawn) spawn_hp_current_ = hp;
    if (clamp) {
        std::int32_t max = max_hp(true, arg);
        if (hp > max) {
            stored_hp_ = sub(max_hp(true, arg), d_.hp_bonus); // Max_HP asked again, as the original does
            hp = max;
        }
    }
    return hp > 10000000 ? 10000000 : hp;
}

// Cur_Mana / Cur_Endurance.
std::int32_t Character::pool_current(bool update, std::int32_t max, std::int32_t stored, std::int32_t cap_spa,
                                     std::int32_t* out) const {
    bool clamp = update && !sheet_.zoning;
    if (clamp && stored > max) stored = max;
    else if (stored < 0) stored = 0;
    if (clamp) {
        std::int32_t percent_cap = total_effect(cap_spa, true, 0, true, true);
        std::int32_t value_cap = total_effect(cap_spa, true, 1, true, true);
        if (percent_cap > 0) {
            std::int32_t limit = percent_of(fpu_, percent_cap, max);
            if (stored > limit) stored = limit;
        }
        if (value_cap > 0 && stored > value_cap) stored = value_cap;
    }
    if (out) *out = stored;
    return stored;
}
std::int32_t Character::cur_mana(bool update, std::int32_t* stored) const {
    if (sheet_.vitals.cur_mana) { // a server extension's detour: nothing clamped
        if (stored) *stored = sheet_.mana;
        return *sheet_.vitals.cur_mana;
    }
    return pool_current(update, max_mana(true), sheet_.mana, 409, stored);
}
std::int32_t Character::cur_endurance(bool update, std::int32_t* stored) const {
    if (sheet_.vitals.cur_endurance) {
        if (stored) *stored = sheet_.endurance;
        return *sheet_.vitals.cur_endurance;
    }
    return pool_current(update, max_endurance(true), sheet_.endurance, 410, stored);
}

// ---- weight ----

std::int32_t Character::calc_carried_weight() const {
    // Each step rounds to a float where the original stores one.
    auto tenth = [&](std::int32_t w) { return Fpu::to_float(fpu_.mul(d(w), f(0.1f))); };
    double total = 0.0;
    for (int slot = 0; slot < kPossessionSlots; ++slot) {
        const net::Item* item = sheet_.item(slot);
        if (!item) continue;
        total = Fpu::to_float(fpu_.add(tenth(item_weight(*item)), total));
        if (item->item_class != 1) continue;
        double remaining = Fpu::to_float(fpu_.sub(100.0, d(item->weight_reduction)));
        for (int index = 0; index < item->bag_slots; ++index) {
            const net::Item* child = item->content(static_cast<std::uint32_t>(index));
            if (!child) continue;
            double w = tenth(item_weight(*child));
            total = Fpu::to_float(fpu_.add(fpu_.mul(fpu_.mul(remaining, f(0.01f)), w), total));
        }
    }
    std::int32_t effect = total_effect(221, true, 0, true, true);
    // total - effect * total * 0.01f
    double x = fpu_.sub(total, fpu_.mul(fpu_.mul(d(effect), total), f(0.01f)));
    if (x > 32766.0) x = 32766.0;
    else if (x < 0.0) x = 0.0; // NaN is kept, as the x87 compares leave it
    return Fpu::ftol(x);
}

float Character::calculate_weight() const {
    auto carried = static_cast<float>(d_.weight);
    auto capacity = static_cast<float>(d_.stats.str); // CharacterBase +0x4a0
    float factor = 1.0f;
    if (!sheet_.has_spawn) return 1.0f;
    std::int32_t arg = static_cast<std::int32_t>(sheet_.spawn_type);
    std::int32_t hp = cur_hp(true, arg);
    auto hpf = static_cast<float>(hp);
    std::int32_t max = max_hp(true, arg);
    auto max_hpf = static_cast<float>(max);
    std::int32_t threshold = sheet_.slowdown_any_level ? 0 : 3;
    if (d(max) > f(hpf) && sheet_.level > threshold) {
        // x87 HealthSlowdown (@0x44ce7a): hp / max at the FPU's precision against 0.2f / 0.33f
        double ratio = fpu_.div(f(hpf), f(max_hpf));
        if (ratio < f(0.2f) && f(hpf) < 50.0) factor = 0.5f;
        else if (ratio < f(0.33f) && f(hpf) < 100.0) factor = 0.75f;
    }
    if (!(carried > capacity)) {
        // the "encumbered" message state clears (the original prints eqstr 0x3067 when it was 1)
        if (encumbered_state_ >= 0 && encumbered_state_ != 1) return factor;
        encumbered_state_ = 0;
        return factor;
    }
    if (encumbered_state_ <= 0) encumbered_state_ = 1; // and prints eqstr 0x3068
    if (capacity == 0.0f) return factor;
    // x87 OverweightFactor (@0x44cfa1): factor - (carried - capacity) / (capacity + capacity), stored as a float,
    // 0 when below 0
    double v = fpu_.sub(f(factor), fpu_.div(fpu_.sub(f(carried), f(capacity)), fpu_.add(f(capacity), f(capacity))));
    factor = v < 0.0 ? 0.0f : static_cast<float>(v);
    if (sheet_.bazaar_zone) factor = 1.0f;
    if (sheet_.barter_zone) return 1.0f;
    return factor;
}

// ---- armor class, defense, haste ----

namespace {
// The first plain item of an item type in the general slots 23..32 or the bags there
// (FindPackItemOfType).
const net::Item* find_pack_item(const CharacterSheet& s, std::uint32_t item_type) {
    for (int slot = net::kSlotGeneral1; slot <= net::kSlotGeneral10; ++slot) {
        const net::Item* item = s.item(slot);
        if (!item) continue;
        if (item->item_class == 0) {
            if (item->item_type == item_type) return item;
            continue;
        }
        if (item->item_class != 1) continue;
        for (int index = 0; index < item->bag_slots; ++index) {
            const net::Item* child = item->content(static_cast<std::uint32_t>(index));
            if (child && child->item_class == 0 && child->item_type == item_type) return child;
        }
    }
    return nullptr;
}
// A monk's weight limit and the light-weight threshold under it, by the highest level band passed.
struct MonkWeight {
    std::int32_t limit;
    std::int32_t threshold;
};
MonkWeight monk_weight(std::int32_t level) {
    struct Band {
        std::int32_t above_level;
        MonkWeight weight;
    };
    static constexpr Band kBands[] = {
        {99, {58, 35}}, {94, {57, 34}}, {89, {56, 33}}, {84, {55, 32}}, {79, {54, 31}}, {74, {53, 30}},
        {69, {53, 28}}, {64, {53, 26}}, {63, {50, 24}}, {61, {47, 24}}, {59, {45, 24}}, {54, {40, 20}},
        {50, {38, 18}}, {44, {36, 17}}, {29, {34, 16}}, {14, {32, 15}},
    };
    for (const Band& band : kBands)
        if (level > band.above_level) return band.weight;
    return {30, 14};
}

// The rogue's and beastlord's agility factor: 1 below 80, then one more at 85, 90 and 100.
std::int32_t agility_factor(std::int32_t agility) {
    if (agility >= 100) return 5;
    if (agility >= 90) return 4;
    if (agility >= 85) return 3;
    if (agility >= 80) return 2;
    return 1;
}

} // namespace

std::int32_t Character::class_ac_bonus(std::int32_t agility) const {
    if (hooks.class_ac_bonus)
        if (auto v = hooks.class_ac_bonus(agility)) return *v;
    std::int32_t bonus = 0;
    std::int32_t level = sheet_.level;
    if (sheet_.class_id == 7) {
        const MonkWeight band = monk_weight(level);
        const std::int32_t limit = band.limit;
        const std::int32_t threshold = band.threshold;
        std::int32_t weight = d_.weight;
        double base = d(level + 5);
        if (weight < limit - 1) {
            // x87 MonkWeightBonus (@0x44476e), the exe's double constants
            double v = base;
            if (weight > threshold) {
                double over = fpu_.mul(d(weight - threshold), 6.66667);
                double keep = over > 100.0 ? 100.0 : over;
                double scale = fpu_.mul(fpu_.sub(100.0, keep), 0.01);
                v = fpu_.mul(base, scale);
                if (v < 0.0) v = 0.0;
            }
            v = fpu_.mul(fpu_.mul(v, 4.0), 1.0 / 3.0);
            bonus = Fpu::ftol(v);
        }
        if (weight > limit + 1) {
            // x87 MonkWeightPenalty (@0x44484e)
            double fraction = fpu_.mul(fpu_.sub(d(weight), fpu_.sub(d(limit), 10.0)), 0.01);
            if (1.0 < fraction) fraction = 1.0;
            double v = fpu_.mul(fpu_.mul(fpu_.mul(base, 4.0), 1.0 / 3.0), fraction);
            bonus -= Fpu::ftol(v);
        }
    } else if (sheet_.class_id == 9 && agility > 75 && level > 29) { // rogue
        bonus = std::min((level - 26) * agility_factor(agility) / 4, 12);
    } else if (sheet_.class_id == 15 && level > 10) { // beastlord
        bonus = std::min((level - 6) * agility_factor(agility) / 5, 16);
    }
    if (sheet_.race != 128) return bonus;
    return std::clamp(level, 10, 35) + bonus; // Iksar
}

std::int32_t Character::sum_worn_item_ac(std::int32_t item_type) const {
    if (hooks.sum_worn_item_ac)
        if (auto v = hooks.sum_worn_item_ac(item_type)) return *v;
    if (sheet_.has_spawn && sheet_.spawn_type == SpawnType::NPC) return 0;
    const net::Item* found = find_pack_item(sheet_, static_cast<std::uint32_t>(item_type));
    return found ? item_ac(*found, false) : 0; // ItemBase_GetAC(0, 0, 1)
}

std::int32_t Character::calc_ac(bool no_cap, bool extra) const {
    std::int32_t total = sheet_.base_ac;
    std::int32_t bonus = extra ? d_.stats.agi : d_.stats_no_buffs.agi;
    for (int slot = 0; slot < 0x16; ++slot) {
        const net::Item* item = sheet_.item(slot);
        if (!item || item->item_class != 0) continue;
        if (!usable_for_stats(*item)) continue;
        // (slot 14 holding item type 8 also asks for its AC and the heroic STR bonus, unused)
        bool plain = item->charm_file_id != 0 ||
                     (item_recommended_skill(*item) <= 0 && item_recommended_level(*item) <= 0);
        if (plain) total = add(total, item_ac(*item));
        else if (sheet_.has_spawn)
            total = add(total, recommended_item_value(*item, item_ac(*item), item_type_to_skill(item->item_type)));
    }
    std::int32_t sum = add(total, sum_worn_item_ac(0xe));
    sum = add(sum, sum_worn_item_ac(0xf));
    total = sum;
    if (extra)
        for (std::size_t k = 0; k < sheet_.extra_items.size(); ++k)
            if (sheet_.extra_items[k]) total = add(total, item_ac(*sheet_.extra_items[k]));
    std::int32_t ac = core::wrapped_div(static_cast<std::uint32_t>(total) * 4, 3);
    if (!no_cap && sheet_.has_spawn && sheet_.spawn_type == SpawnType::PC && sheet_.level < 0x32) {
        std::int32_t cap = add(mul(sheet_.level, 6), 0x19);
        if (ac > cap) ac = cap;
    }
    ac = add(ac, class_ac_bonus(bonus));
    if (ac < 0) ac = 0;
    if (static_cast<std::uint8_t>(sheet_.class_id - 0xb) <= 3) {
        ac = add(ac, skill_with_item_mods(15, extra) / 2);
        ac = add(ac, total_effect(1, true, 0, true, extra) / 3);
        ac = add(ac, total_effect(416, true, 0, true, extra) / 3);
    } else {
        ac = add(ac, skill_with_item_mods(15, extra) / 3);
        ac = add(ac, total_effect(1, true, 0, true, extra) / 4);
        ac = add(ac, total_effect(416, true, 0, true, extra) / 4);
    }
    if (bonus > 70) ac = add(ac, bonus / 20);
    return ac < 0 ? 0 : ac;
}

std::int32_t Character::agility_defense_bonus(bool include_buffs) const {
    std::int32_t agility = include_buffs ? d_.stats.agi : d_.stats_no_buffs.agi;
    std::int32_t from_agility = core::wrapped_mul_div(add(agility, -40), 8000, 36000);
    return heroic(&DerivedStats::heroic_agi) / 10 + from_agility;
}

std::int32_t Character::compute_defense(bool include_buffs) const {
    if (!sheet_.has_spawn) return 0;
    std::int32_t skill = skill_with_item_mods(15, include_buffs);
    if (skill < 0) skill = 0;
    std::int32_t agility = agility_defense_bonus(include_buffs);
    std::int32_t avoidance = include_buffs ? d_.avoidance : d_.avoidance_no_buffs;
    std::int32_t value = add(add(agility, core::wrapped_mul_div(skill, 400, 225)), avoidance);
    if (sheet_.spawn_type == SpawnType::PC && include_buffs) {
        std::int32_t half_drunk = sheet_.drunkenness / 2;
        if (half_drunk > 20) {
            // x87 ScaleByMinFactor (@0x444b7e): value * min((110 - n) * 0.01, 1.0)
            double factor = fpu_.mul(fpu_.sub(110.0, d(half_drunk)), 0.01);
            if (1.0 < factor) factor = 1.0;
            value = Fpu::ftol(fpu_.mul(factor, d(value)));
        }
    }
    return value < 1 ? 1 : value;
}

std::int32_t Character::calc_item_haste(std::int32_t haste) const {
    std::int32_t best = haste;
    std::int32_t cap = sheet_.level < 0x1a ? 10 : 100;
    for (int slot = 0; slot < 0x16; ++slot) {
        const net::Item* item = sheet_.item(slot);
        if (!item || item_haste(*item, 0) <= best) continue;
        if (!can_use_item(*item, true)) continue;
        best = item_haste(*item, 0);
    }
    for (const auto& extra : sheet_.extra_items)
        if (extra && extra->item_class == 0 && item_haste(*extra, 0) > best) best = item_haste(*extra, 0);
    if (best > cap) best = cap;
    return best != 0 ? best + 100 : 0;
}

// ---- the other small rules ----

std::int32_t Character::max_air_supply() const {
    std::int32_t stamina = add(d_.stats.sta, -30);
    std::int32_t percent = total_effect(246, true, 0, true, true);
    if (percent <= 0) percent = 100;
    std::int32_t air = core::wrapped_mul_div(stamina, percent, 100);
    if (sheet_.race == 128) return 127;
    if (sheet_.race == 330) return 256;
    if (air < 10) return 10;
    return air > percent ? percent : air;
}

namespace {
// The level cap the highest expansion bit set brings (bits 0..18; higher bits count for nothing): 50 without one.
std::int32_t expansion_level_cap(std::uint32_t expansions) {
    static constexpr std::uint8_t kCapByBit[19] = {60, 60, 60, 65, 65, 65, 65, 70, 70, 70,
                                                   70, 75, 75, 80, 85, 85, 90, 95, 100};
    for (std::size_t bit = 19; bit-- > 0;)
        if ((expansions >> bit) & 1u) return kCapByBit[bit];
    return 50;
}
} // namespace

std::int32_t Character::max_level() const {
    if (hooks.max_level)
        if (auto v = hooks.max_level()) return *v;
    const std::int32_t cap = sheet_.level_cap_100 ? 100 : expansion_level_cap(sheet_.expansions);
    const std::int32_t lower = sheet_.level_cap_override;
    if (lower <= 0 || lower >= cap) return cap;
    return lower;
}

std::int32_t Character::invis_level(int kind) const {
    // Each kind's classic and newer SPA: 0 (and anything but 1, 2) normal, 1 vs undead, 2 vs animals.
    struct InvisSpas {
        std::int32_t classic;
        std::int32_t newer;
    };
    static constexpr InvisSpas kKinds[3] = {{12, 314}, {28, 315}, {29, 316}};
    const InvisSpas& spas = kKinds[kind == 1 || kind == 2 ? static_cast<std::size_t>(kind) : 0];
    // The newer one is totalled first, as the original asks.
    const std::int32_t newer_total = total_effect(spas.newer);
    const std::int32_t classic_total = total_effect(spas.classic);
    return total_effect(classic_total > newer_total ? spas.classic : spas.newer);
}

namespace {

// The item sums' questions answered from the sheet.
class SheetItemSums final : public ItemSumQueries {
public:
    explicit SheetItemSums(const Character& ch) : ch_(ch) {}
    std::int32_t level() override { return ch_.sheet().level; }
    bool any_race_deity() override { return ch_.sheet().ignores_item_restrictions; }
    std::int32_t race() override { return static_cast<std::int32_t>(ch_.sheet().race); }
    bool has_pc_base() override { return ch_.sheet().has_owner; }
    std::int32_t deity() override { return static_cast<std::int32_t>(ch_.sheet().deity); }
    std::int32_t limit(std::int32_t which) override { return which == 0x13 ? ch_.sheet().prestige_limit : -1; }
    std::uint8_t item_type_to_skill(std::int32_t type) override {
        return mechanics::item_type_to_skill(static_cast<std::uint32_t>(type));
    }
    std::int32_t recommended_scale(const StatItem& item, std::int32_t value, std::uint32_t skill) override {
        return item.source ? ch_.recommended_item_value(*item.source, value, static_cast<std::uint8_t>(skill)) : value;
    }
    std::uint32_t damage_slot(std::int32_t skill) override { return damage_bonus_slot(skill); }

private:
    const Character& ch_;
};

} // namespace

// ---- ProcessAllStats @0x4547e0 ----

void Character::process_all_stats() {
    d_ = {};
    // The item sums (item_sums.h, following the original exactly): the worn slots 0..0x15,
    // then the 21 other lookups (0x42def0 x5, 0x42df20 x10, the
    // virtuals +0x4c x2 and +0x54 x4).
    std::array<StatItem, 0x16> worn_items;
    std::array<const StatItem*, 0x16> worn{};
    for (int slot = 0; slot < 0x16; ++slot)
        if (const net::Item* item = sheet_.item(slot)) {
            worn_items[static_cast<std::size_t>(slot)] = stat_item(*item);
            worn[static_cast<std::size_t>(slot)] = &worn_items[static_cast<std::size_t>(slot)];
        }
    std::array<StatItem, 21> extra_items;
    std::array<const StatItem*, 21> extra{};
    for (std::size_t k = 0; k < extra.size(); ++k)
        if (const auto& item = sheet_.extra_items[k]) {
            extra_items[k] = stat_item(*item);
            extra[k] = &extra_items[k];
        }
    std::span<const StatItem* const> others(extra);
    ItemSums sums;
    SheetItemSums q(*this);
    sum_item_stats(sums, ItemSources{worn, others.subspan(0, 5), others.subspan(5, 10), others.subspan(15, 2),
                                     others.subspan(17, 4)},
                   q);
    // AddPackItemStats(14) and (15): the first food and drink item in the general slots or their bags.
    if (!(sheet_.has_spawn && sheet_.spawn_type == SpawnType::NPC)) {
        for (std::uint32_t type : {14u, 15u}) {
            const net::Item* item = find_pack_item(sheet_, type);
            if (!item) continue;
            auto own = [&](std::int32_t (*field)(const net::Item&)) { return item_stat(*item, field, false); };
            auto rec = [&](std::int32_t (*field)(const net::Item&)) { return item_stat(*item, field, true); };
            sums.base_at(0x4a4) = add(sums.base_at(0x4a4), own([](const net::Item& i) { return I(i.astr); }));
            sums.base_at(0x4a8) = add(sums.base_at(0x4a8), own([](const net::Item& i) { return I(i.asta); }));
            sums.base_at(0x4b0) = add(sums.base_at(0x4b0), own([](const net::Item& i) { return I(i.adex); }));
            sums.base_at(0x4b8) = add(sums.base_at(0x4b8), own([](const net::Item& i) { return I(i.aagi); }));
            sums.base_at(0x4b4) = add(sums.base_at(0x4b4), own([](const net::Item& i) { return I(i.aint); }));
            sums.base_at(0x4bc) = add(sums.base_at(0x4bc), own([](const net::Item& i) { return I(i.awis); }));
            sums.base_at(0x4ac) = add(sums.base_at(0x4ac), own([](const net::Item& i) { return I(i.acha); }));
            sums.base_at(0x4c4) = add(sums.base_at(0x4c4), own([](const net::Item& i) { return I(i.mr); }));
            sums.base_at(0x4d4) = add(sums.base_at(0x4d4), own([](const net::Item& i) { return I(i.cr); }));
            sums.base_at(0x4d0) = add(sums.base_at(0x4d0), own([](const net::Item& i) { return I(i.fr); }));
            sums.base_at(0x4c8) = add(sums.base_at(0x4c8), own([](const net::Item& i) { return I(i.dr); }));
            sums.base_at(0x4c0) = add(sums.base_at(0x4c0), own([](const net::Item& i) { return I(i.pr); }));
            sums.base_at(0x4cc) = add(sums.base_at(0x4cc), own([](const net::Item& i) { return I(i.sv_corruption); }));
            sums.at(0x24) = add(sums.at(0x24), own([](const net::Item& i) { return i.hp; }));
            sums.at(0x28) = add(sums.at(0x28), own([](const net::Item& i) { return i.mana; }));
            sums.at(0x2c) = add(sums.at(0x2c), own([](const net::Item& i) { return static_cast<std::int32_t>(i.endurance); }));
            sums.at(0x30) = add(sums.at(0x30), own([](const net::Item& i) { return i.combat_effects; }));
            sums.at(0x34) = add(sums.at(0x34), own([](const net::Item& i) { return i.shielding; }));
            sums.at(0x38) = add(sums.at(0x38), own([](const net::Item& i) { return i.spell_shield; }));
            sums.at(0x3c) = add(sums.at(0x3c), own([](const net::Item& i) { return i.avoidance; }));
            sums.at(0x40) = add(sums.at(0x40), own([](const net::Item& i) { return i.accuracy; }));
            sums.at(0x44) = add(sums.at(0x44), own([](const net::Item& i) { return i.stun_resist; }));
            sums.at(0xc8) = add(sums.at(0xc8), rec([](const net::Item& i) { return i.heroic_str; }));
            sums.at(0xcc) = add(sums.at(0xcc), rec([](const net::Item& i) { return i.heroic_int; }));
            sums.at(0xd0) = add(sums.at(0xd0), rec([](const net::Item& i) { return i.heroic_wis; }));
            sums.at(0xd4) = add(sums.at(0xd4), rec([](const net::Item& i) { return i.heroic_agi; }));
            sums.at(0xd8) = add(sums.at(0xd8), rec([](const net::Item& i) { return i.heroic_dex; }));
            sums.at(0xdc) = add(sums.at(0xdc), rec([](const net::Item& i) { return i.heroic_sta; }));
            sums.at(0xe0) = add(sums.at(0xe0), rec([](const net::Item& i) { return i.heroic_cha; }));
            sums.at(0xe4) = add(sums.at(0xe4), rec([](const net::Item& i) { return i.heroic_mr; }));
            sums.at(0xe8) = add(sums.at(0xe8), rec([](const net::Item& i) { return i.heroic_fr; }));
            sums.at(0xec) = add(sums.at(0xec), rec([](const net::Item& i) { return i.heroic_cr; }));
            sums.at(0xf0) = add(sums.at(0xf0), rec([](const net::Item& i) { return i.heroic_dr; }));
            sums.at(0xf4) = add(sums.at(0xf4), rec([](const net::Item& i) { return i.heroic_pr; }));
            sums.at(0xf8) = add(sums.at(0xf8), rec([](const net::Item& i) { return i.heroic_sv_corrup; }));
            sums.at(0xfc) = add(sums.at(0xfc), rec([](const net::Item& i) { return i.heal_amt; }));
            sums.at(0x100) = add(sums.at(0x100), rec([](const net::Item& i) { return i.spell_dmg; }));
            sums.at(0x10c) = add(sums.at(0x10c), rec([](const net::Item& i) { return i.clairvoyance; }));
            sums.at(0x44) = add(sums.at(0x44), rec([](const net::Item& i) { return i.stun_resist; }));
            sums.at(0x48) = add(sums.at(0x48), rec([](const net::Item& i) { return i.strike_through; }));
            sums.at(0x110) = add(sums.at(0x110), own([](const net::Item& i) { return i.attack; }));
            sums.at(0x124) = add(sums.at(0x124), item_haste(*item, 0, false));
            sums.at(0x120) = add(sums.at(0x120), own([](const net::Item& i) { return i.damage_shield; }));
            sums.at(0x114) = add(sums.at(0x114), own([](const net::Item& i) { return i.regen; }));
            sums.at(0x118) = add(sums.at(0x118), own([](const net::Item& i) { return i.mana_regen; }));
            sums.at(0x11c) = add(sums.at(0x11c), own([](const net::Item& i) { return i.end_regen; }));
        }
    }
    derive_item_bonuses(sums, [this](std::int32_t value, int kind) { return cap_mod(value, kind); });
    sums.at(0x124) = calc_item_haste(sums.at(0x124));
    auto block = [&](StatBlock& b, std::uint32_t from) {
        std::int32_t* f[] = {&b.str, &b.sta, &b.cha, &b.dex, &b.int_, &b.agi, &b.wis, &b.sv_poison, &b.sv_magic,
                             &b.sv_disease, &b.sv_corruption, &b.sv_fire, &b.sv_cold, &b.sv_physical};
        for (std::uint32_t k = 0; k < std::size(f); ++k) *f[k] = sums.base_at(from + k * 4);
    };
    block(d_.items, 0x4a4);
    block(d_.items_no_buffs, 0x510);
    d_.hp_bonus = sums.at(0x24);
    d_.mana_bonus = sums.at(0x28);
    d_.endurance_bonus = sums.at(0x2c);
    d_.combat_effects = sums.at(0x30);
    d_.shielding = sums.at(0x34);
    d_.spell_shield = sums.at(0x38);
    // derive_item_bonuses' copies from before the caps (the *Bonus2 fields, +0xa0..+0xc4)
    d_.combat_effects_uncapped = sums.at(0xa0);
    d_.spell_shield_uncapped = sums.at(0xa4);
    d_.shielding_uncapped = sums.at(0xa8);
    d_.damage_shield_uncapped = sums.at(0xac);
    d_.dot_shielding_uncapped = sums.at(0xb0);
    d_.ds_mitigation_uncapped = sums.at(0xb4);
    d_.avoidance_uncapped = sums.at(0xb8);
    d_.accuracy_uncapped = sums.at(0xbc);
    d_.stun_resist_uncapped = sums.at(0xc0);
    d_.strike_through_uncapped = sums.at(0xc4);
    d_.avoidance = sums.at(0x3c);
    d_.accuracy = sums.at(0x40);
    d_.stun_resist = sums.at(0x44);
    d_.strike_through = sums.at(0x48);
    d_.dot_shielding = sums.at(0x98);
    d_.ds_mitigation = sums.at(0x9c);
    d_.heroic_str = sums.at(0xc8);
    d_.heroic_int = sums.at(0xcc);
    d_.heroic_wis = sums.at(0xd0);
    d_.heroic_agi = sums.at(0xd4);
    d_.heroic_dex = sums.at(0xd8);
    d_.heroic_sta = sums.at(0xdc);
    d_.heroic_cha = sums.at(0xe0);
    d_.heroic_sv_magic = sums.at(0xe4);
    d_.heroic_sv_fire = sums.at(0xe8);
    d_.heroic_sv_cold = sums.at(0xec);
    d_.heroic_sv_disease = sums.at(0xf0);
    d_.heroic_sv_poison = sums.at(0xf4);
    d_.heroic_sv_corruption = sums.at(0xf8);
    d_.heal_amount = sums.at(0xfc);
    d_.spell_damage = sums.at(0x100);
    d_.clairvoyance = sums.at(0x10c);
    d_.attack = sums.at(0x110);
    d_.hp_regen = sums.at(0x114);
    d_.mana_regen = sums.at(0x118);
    d_.endurance_regen = sums.at(0x11c);
    d_.damage_shield = sums.at(0x120);
    d_.attack_speed = sums.at(0x124);
    // The worn-only copies (KeepWornSums).
    d_.hp_bonus_no_buffs = sums.at(0x12c);
    d_.mana_bonus_no_buffs = sums.at(0x130);
    d_.endurance_bonus_no_buffs = sums.at(0x134);
    d_.avoidance_no_buffs = sums.at(0x144);
    d_.attack_speed_no_buffs = sums.at(0x170);
    d_.sums = sums;
    StatBlock& nb = d_.stats_no_buffs;
    nb.str = calc_str(false);
    nb.sta = calc_sta(false);
    nb.cha = calc_cha(false);
    nb.dex = calc_dex(false);
    nb.int_ = calc_int(false);
    nb.wis = calc_wis(false);
    nb.sv_magic = calc_save_magic(false);
    nb.sv_cold = calc_save_cold(false);
    nb.sv_fire = calc_save_fire(false);
    nb.sv_poison = calc_save_poison(false);
    nb.sv_disease = calc_save_disease(false);
    nb.sv_corruption = calc_save_corruption(false);
    nb.sv_physical = calc_save_physical(false);
    StatBlock& s = d_.stats;
    s.str = calc_str(true);
    s.sta = calc_sta(true);
    s.cha = calc_cha(true);
    s.dex = calc_dex(true);
    s.int_ = calc_int(true);
    s.wis = calc_wis(true);
    s.sv_magic = calc_save_magic(true);
    s.sv_cold = calc_save_cold(true);
    s.sv_fire = calc_save_fire(true);
    s.sv_poison = calc_save_poison(true);
    s.sv_disease = calc_save_disease(true);
    s.sv_corruption = calc_save_corruption(true);
    s.sv_physical = calc_save_physical(true);
    d_.weight = calc_carried_weight();
    nb.agi = calc_agi(false);
    s.agi = calc_agi(true);
    d_.ac = calc_ac(true, true);
}

} // namespace hailstone::game::mechanics
