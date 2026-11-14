#pragma once

// The original client's character rules (CharacterZoneClient and its
// helpers in eqgame.exe), reimplemented on a CharacterSheet and checked
// against the original's vectors. One Character evaluates one sheet; the
// member functions are named after the original functions they reproduce,
// with their addresses in the comments, and keep the original's argument
// meanings (include_buffs = "with buffs" / "the live value",
// include_items, ...).
//
// Use:
//   Character ch(sheet, tables, spells);
//   ch.process_all_stats();           // CharacterZoneClient::ProcessAllStats
//   ch.max_hp(); ch.stats().stats.str; ch.total_effect(3); ...
// total_effect() and the effect caches work without process_all_stats().
//
// Every call the original makes to another of these functions goes through
// Hooks first, so that a test can replay a recorded call of the original
// (the recorded vectors: the original ran with its callees mocked) and
// check one function on its own. Without hooks the client computes everything.
//
// Sources: the original's functions named in each file, checked against its
// vectors, and the exe's disassembly for those without vectors
// (CacheBuffSlotEffects @0x436b30, CacheWornItemEffects @0x437780,
// ProcessAllStats @0x4547e0); see docs/mechanics.md.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>

#include "game/mechanics/character_sheet.h"
#include "game/mechanics/fpu.h"
#include "game/mechanics/item_sums.h"
#include "game/mechanics/tables.h"

namespace hailstone::game::mechanics {

// The SpellCache: buff, worn-item and AA totals by (SPA, subindex).
struct EffectCache {
    std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t> buffs, items, aas;
    std::int32_t endurance_upkeep = 0; // CharacterZoneClient +0x94
    static std::int32_t get(const std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t>& m, std::int32_t spa,
                            std::int32_t sub) {
        auto it = m.find({spa, sub});
        return it == m.end() ? 0 : it->second;
    }
};

// The stat block of CharacterBase (+0x4a0.., +0x50c.. without buffs).
struct StatBlock {
    std::int32_t str = 0, sta = 0, cha = 0, dex = 0, int_ = 0, agi = 0, wis = 0;
    std::int32_t sv_poison = 0, sv_magic = 0, sv_disease = 0, sv_corruption = 0, sv_fire = 0, sv_cold = 0;
    std::int32_t sv_physical = 0;
};

// What CharacterZoneClient::ProcessAllStats leaves behind.
struct DerivedStats {
    StatBlock items, items_no_buffs;      // item totals (the inputs of Calc*)
    StatBlock stats, stats_no_buffs;      // the capped results
    StatBlock uncapped;                   // CharacterBase +0x4d8..: the last Calc* call's uncapped totals
    std::int32_t hp_bonus = 0, mana_bonus = 0, endurance_bonus = 0;                   // +0x24..+0x2c
    std::int32_t hp_bonus_no_buffs = 0, mana_bonus_no_buffs = 0, endurance_bonus_no_buffs = 0; // +0x12c..+0x134
    std::int32_t combat_effects = 0, shielding = 0, spell_shield = 0, avoidance = 0, accuracy = 0;
    std::int32_t stun_resist = 0, strike_through = 0, dot_shielding = 0, damage_shield = 0, ds_mitigation = 0;
    std::int32_t avoidance_no_buffs = 0; // +0x144
    std::int32_t heroic_str = 0, heroic_int = 0, heroic_wis = 0, heroic_agi = 0, heroic_dex = 0, heroic_sta = 0,
                 heroic_cha = 0;                                                  // +0xc8..+0xe0
    std::int32_t heroic_sv_magic = 0, heroic_sv_fire = 0, heroic_sv_cold = 0, heroic_sv_disease = 0,
                 heroic_sv_poison = 0, heroic_sv_corruption = 0;                  // +0xe4..+0xf8
    std::int32_t heal_amount = 0, spell_damage = 0, clairvoyance = 0, attack = 0; // +0xfc, +0x100, +0x10c, +0x110
    std::int32_t hp_regen = 0, mana_regen = 0, endurance_regen = 0;              // +0x114, +0x118, +0x11c (items)
    std::int32_t attack_speed = 0;          // +0x124: item haste after CalcItemHaste (100 + %, 0 for none)
    std::int32_t attack_speed_no_buffs = 0; // +0x170
    std::int32_t weight = 0;                // +0x14 CurrWeight
    std::int32_t ac = 0;                    // CalcAC(1, 1)
    ItemSums sums;                          // every item sum as the original keeps it (item_sums.h)
};

// A focus lookup's result (GetBestItemFocus / GetBestBuffFocus / GetBestAAFocus).
struct FocusFound {
    bool found = false;
    std::int32_t percent = 100;
    std::int32_t extra = 0; // items: the item's spell slot holding the focus (3 or 5)
    std::int32_t value = 0; // buffs: the focus spell's first base (+0x2c); AAs: the effect's base (+4)
};

class Character {
public:
    template <typename R, typename... A> using Hook = std::function<std::optional<R>(A...)>;
    struct Hooks {
        Hook<std::int32_t, std::int32_t, bool, std::int32_t, bool, bool> total_effect; // spa, items, sub, aa, buffs
        Hook<std::int32_t, std::int32_t, std::int32_t> buff_effect, item_effect, aa_effect; // SpellCache readers
        Hook<std::int32_t, std::int32_t, int> cap_stat;               // vtable +0x38 (value, index)
        Hook<std::int32_t, int, bool> cur_hp, max_hp;                 // (arg, update / include_buffs)
        Hook<std::int32_t, bool> max_mana, max_endurance;             // vtable +0x24 / +0x28
        Hook<std::int32_t, std::uint32_t, bool> skill_with_item_mods; // vtable +0x18 (skill, flag)
        Hook<std::int32_t, std::uint32_t> adjusted_skill, racial_skill_bonus;
        Hook<std::int32_t, std::int32_t, std::int32_t, std::int32_t> skill_cap; // CSkillMgr::GetSkillCap(level, cls, skill)
        Hook<std::int32_t, std::uint32_t, std::uint32_t> skill_min_level;       // 0x5baa20(skill, cls)
        Hook<std::int32_t, std::uint32_t> skill_entry; // -1 no entry, else EQ_Skill::LevelCappedSkill
        Hook<std::int32_t, std::uint32_t, std::uint32_t, std::uint32_t> skill_cap_cell; // SkillCaps[cls][skill][level]
        Hook<std::int32_t> max_level, base_hp_regen, base_endurance_regen, heroic_sta_bonus, max_effects;
        Hook<std::int32_t, std::int32_t> class_ac_bonus, sum_worn_item_ac;     // (agility) / (item type)
        Hook<std::int32_t, std::int32_t, std::uint8_t> recommended_item_ac;    // vtable +0x34 (value, skill)
        Hook<bool, std::int32_t> is_valid_spell_id, has_spell_affect;          // (spell id) / (spa)
        Hook<FocusFound, std::int32_t> best_item_focus, best_aa_focus;          // (focus type)
        Hook<FocusFound, std::int32_t, bool> best_buff_focus;                   // (focus type, buff argument)
        Hook<std::pair<std::int32_t, bool>, std::int32_t> item_spell_id;        // (extra): spell id, IsValidSpellID
        Hook<std::pair<bool, std::int32_t>, std::int32_t> spell_focus_base;     // (spell id): found, base (+0x2c)
    };
    Hooks hooks;

    Character(const CharacterSheet& sheet, const GameTables& tables, SpellLookup spells);

    const CharacterSheet& sheet() const { return sheet_; }
    const GameTables& tables() const { return tables_; }
    const SpellLookup& spells() const { return spells_; }
    const Fpu& fpu() const { return fpu_; }
    const SpellData* spell(std::int32_t id) const;
    // The ProcessAllStats results, to read, or to set for a single function's check.
    const DerivedStats& stats() const { return d_; }
    DerivedStats& mutable_stats() { return d_; }
    // State the functions write: the spawn's HPMax / HPCurrent (Max_HP, Cur_HP), the profile's stored HP
    // (Cur_HP) and the encumbered message state (CalculateWeight). Start from the sheet.
    std::int32_t spawn_hp_max() const { return spawn_hp_max_; }
    std::int32_t spawn_hp_current() const { return spawn_hp_current_; }
    std::int32_t stored_hp() const { return stored_hp_; }
    std::int32_t encumbered_state() const { return encumbered_state_; }

    // ---- effects (effects.cpp) ----
    // CharacterZoneClient::TotalEffect @0x438990.
    std::int32_t total_effect(std::int32_t spa, bool include_items = true, std::int32_t subindex = 0,
                              bool include_aa = true, bool include_buffs = true) const;
    // The SpellCache (ReCacheSpellEffects @0x438910 + CacheAAEffects @0x4385d0), built on first use.
    const EffectCache& effect_cache() const;
    void invalidate_effects() { cache_.reset(); }
    std::int32_t cached_buff(std::int32_t spa, std::int32_t sub) const; // GetCachedBuffEffect @0x443440
    std::int32_t cached_item(std::int32_t spa, std::int32_t sub) const; // GetCachedItemEffect @0x443450
    std::int32_t cached_aa(std::int32_t spa, std::int32_t sub) const;   // GetCachedAAEffect @0x443480
    // CharacterZoneClient::CalcAffectChangeGeneric @0x434df0 for a buff (or no buff: an item).
    std::int32_t calc_affect_change(const SpellData& spell, std::uint32_t level, int slot, const SheetBuff* affect,
                                    std::int32_t base, bool cap_at_max) const;
    // PlayerClient::CalcSpellDistanceMod @0x59fbe0: 1 for a spell without a distance range (+0x21c / +0x224 both 0);
    // the far modifier (+0x228) when the spawn is not found (`distance` none); else the spawn's 2D distance from the
    // character clamped to the range, (d - start) x ratio (+0x4e9) + the near modifier (+0x220), at least 0.
    double spell_distance_mod(const SpellData& spell, std::optional<float> distance) const;
    // CharacterZoneClient::CalcAffectChange @0x436670's scaling of CalcAffectChangeGeneric's `value` once it has a
    // spawn to measure from (a target, or the buff slot's caster id looked up; `distance` none when not found):
    // unchanged without a distance range or without the character's spawn, else the modifier x value rounded half
    // away from zero, a negative value that rounds to 0 giving -1.
    std::int32_t scale_by_distance(const SpellData& spell, std::int32_t value, std::optional<float> distance) const;
    // CharacterZoneClient_GetNegateEffectMask: bit 1 buffs, 2 items, 4 AAs (SPA 382).
    std::uint32_t negate_mask(std::int32_t spa) const;
    // GetMaxEffects @0x432630.
    int max_effects() const;
    bool valid_spell(std::int32_t id) const; // IsValidSpellID @0x4ad150: 1..45000

    // ---- stats (stats.cpp) ----
    // CharacterZoneClient::ProcessAllStats @0x4547e0.
    void process_all_stats();
    std::int32_t calc_str(bool include_buffs);  // @0x445a50
    std::int32_t calc_sta(bool include_buffs);  // @0x445bc0
    std::int32_t calc_cha(bool include_buffs);  // @0x445cf0
    std::int32_t calc_dex(bool include_buffs);  // @0x445dc0
    std::int32_t calc_int(bool include_buffs);  // @0x445ef0
    std::int32_t calc_wis(bool include_buffs);  // @0x446040
    std::int32_t calc_agi(bool include_buffs);  // @0x454280
    std::int32_t calc_save_magic(bool include_buffs);      // @0x444f80
    std::int32_t calc_save_fire(bool include_buffs);       // @0x4451f0
    std::int32_t calc_save_cold(bool include_buffs);       // @0x445420
    std::int32_t calc_save_disease(bool include_buffs);    // @0x4456f0
    std::int32_t calc_save_poison(bool include_buffs);     // @0x445990
    std::int32_t calc_save_corruption(bool include_buffs); // @0x442970
    std::int32_t calc_save_physical(bool include_buffs) const; // @0x442860 (never capped)
    std::int32_t basesave_magic() const;   // @0x444e10
    std::int32_t basesave_fire() const;    // @0x445040
    std::int32_t basesave_cold() const;    // @0x4452b0
    std::int32_t basesave_disease() const; // @0x4454e0
    std::int32_t basesave_poison() const;  // @0x4457b0
    // CharacterZoneClient_GetStatCap @0x57f2c0: 0..6 STR STA AGI DEX WIS INT CHA, 7..13 magic cold fire poison
    // disease (12 none) corruption; -1 otherwise.
    std::int32_t stat_cap(int index) const;
    std::int32_t cap_stat(std::int32_t value, int index) const;  // @0x57f290 (vtable +0x38)
    static std::int32_t mod_cap(int index);                      // @0x57f550
    std::int32_t cap_mod(std::int32_t value, int index) const;   // @0x57f520
    // Heroic bonuses as the getters @0x442a30.. return them (players with expansion bit 0x2000).
    std::int32_t heroic(std::int32_t DerivedStats::*field) const;
    // CharacterZoneClient::Max_HP @0x443fa0 (arg: the first argument, 0 adds 5).
    std::int32_t max_hp(bool include_buffs = true, int arg = 0) const;
    std::int32_t max_mana(bool include_buffs = true) const;      // @0x581e60
    std::int32_t max_endurance(bool include_buffs = true) const; // @0x582020
    // Cur_HP @0x449e00, Cur_Mana @0x4442e0, Cur_Endurance @0x444170: the current values the original shows
    // (with update: clamped to the maxima and the SPA 408..410 limits; Cur_HP stores the clamped value, see
    // stored_hp(); the pools return their clamped stored value in *stored when given).
    std::int32_t cur_hp(bool update = true, int arg = 0) const;
    std::int32_t cur_mana(bool update = true, std::int32_t* stored = nullptr) const;
    std::int32_t cur_endurance(bool update = true, std::int32_t* stored = nullptr) const;
    // All six answer the sheet's CharacterVitals value when it has one (a server's client patch detours them: the
    // NMS patch returns the server's OP_ServerAuthStats values), after the hooks and before the formula.
    // BaseProfile_GetBase* @0x7ea8b0..: the class table values for the profile (or its own values).
    double base_hp() const;
    std::int32_t base_mana() const;
    std::int32_t base_endurance() const;
    std::int32_t base_hp_regen() const;
    std::int32_t base_endurance_regen() const;
    // CharacterZoneClient_CalcAC @0x4538d0.
    std::int32_t calc_ac(bool no_cap = true, bool extra = true) const;
    std::int32_t class_ac_bonus(std::int32_t agility) const; // GetClassACBonus @0x444470
    std::int32_t sum_worn_item_ac(std::int32_t item_type) const; // CharacterZoneClient_SumWornItemAC @0x44bd00
    std::int32_t compute_defense(bool include_buffs = true) const; // ComputeDefense @0x444ad0
    std::int32_t agility_defense_bonus(bool include_buffs) const; // @0x444a40
    std::int32_t calc_item_haste(std::int32_t haste) const;  // CalcItemHaste @0x44a860
    std::int32_t calc_carried_weight() const;                // CalcCarriedWeight @0x44d020
    float calculate_weight() const;                          // EQ_Character::CalculateWeight @0x44cdd0
    std::int32_t max_air_supply() const;                     // PcClient::MaxAirSupply @0x581da0
    std::int32_t max_level() const;                          // CharacterBase_GetMaxLevel @0x7e6850
    std::int32_t invis_level(int kind) const;                // CalcInvisLevel @0x442770
    // CharacterZoneClient::CanUseItem(item, prestige) @0x446470 (level, race, deity, prestige).
    bool can_use_item(const net::Item& item, bool prestige = true) const;
    // 0x585190 (vtable +0x34): an item stat scaled down for a recommended level or skill above the character's.
    std::int32_t recommended_item_value(const net::Item& item, std::int32_t value, std::uint8_t skill) const;

    // ---- regen (regen.cpp) ----
    // GetHPRegen @0x4527f0; *bleeding (the second argument) is set when an unconscious player bleeds (-1): the
    // tick prints "You are bleeding to death!" (0x318d) then.
    std::int32_t hp_regen(bool include_items = true, bool* bleeding = nullptr, bool no_rest = false) const;
    std::int32_t mana_regen(bool include_items = true, bool no_rest = false) const;      // @0x452df0
    std::int32_t endurance_regen(bool include_items = true, bool no_rest = false) const; // @0x448ec0

    // ---- skills (skills.cpp) ----
    std::int32_t racial_skill_bonus(std::uint32_t skill) const; // GetRacialSkillBonus @0x444bd0
    // CSkillMgr::GetSkillCap @0x5baec0 with this character as the PcBase (or none).
    std::int32_t skill_cap(std::int32_t level, std::int32_t cls, std::int32_t skill, bool with_character = true,
                           bool items = true, bool aa = true, bool buffs = true) const;
    // CSkillMgr_GetSkillCapWithEffects @0x5ba600.
    std::int32_t skill_cap_with_effects(std::uint32_t skill, bool items = true, bool aa = true, bool buffs = true) const;
    std::int32_t adjusted_skill(std::uint32_t skill) const;         // GetAdjustedSkill @0x449fe0
    // GetAdjustedSkill with the profile class read as `class_id` (the NMS patch's HasSkill class spoof).
    std::int32_t adjusted_skill_as(std::uint32_t skill, std::uint32_t class_id) const;
    // CharacterZoneClient_GetSkillWithItemMods @0x5826e0 (vtable +0x18).
    std::int32_t skill_with_item_mods(std::uint32_t skill, bool spell_reduction) const;

    // ---- spell math (spell_math.cpp) ----
    // CharacterZoneClient::SpellDuration @0x434420.
    std::int32_t spell_duration(const SpellData& spell, std::uint8_t level, bool special = false) const;
    // GetCastingTimeModifier @0x4336d0 (hybrid quick cast; new - old cast time).
    std::int32_t casting_time_modifier(const SpellData& spell) const;
    // GetFocusCastingTimeModifier @0x42c9a0 (focus type 127: item, buff and AA; negative = faster). The buff
    // argument (the original's 4th) lets the buff focus apply to the classes that otherwise get none.
    std::int32_t focus_casting_time_modifier(const SpellData& spell, bool buff_arg = false) const;
    // CharacterZoneClient::GetFocusReuseMod @0x42cec0 (focus type 310 ReduceReuseTimer: the item's focus spell's
    // first base, the buff's and the AA's, each scaled by its percent; at most the spell's recast time). What the
    // spell gem, hot button and combat ability cool-downs take off the recast time they show.
    std::int32_t focus_reuse_mod(const SpellData& spell) const;
    // The focus collectors' best (GetBestItemFocus / GetBestBuffFocus / GetBestAAFocus) for a focus type.
    FocusFound best_item_focus(const SpellData& spell, std::int32_t type) const;
    FocusFound best_buff_focus(const SpellData& spell, std::int32_t type, bool buff_arg) const;
    FocusFound best_aa_focus(const SpellData& spell, std::int32_t type) const;
    // CheckFocusLimit @0x428d00 / GetSpellFocusPct @0x4292f0 for a focus spell.
    std::int32_t check_focus_limit(const SpellData& spell, std::int32_t limit, std::int32_t base, std::int32_t base2,
                                   std::uint32_t slots) const;
    std::int32_t spell_focus_pct(const SpellData& cast, const SpellData& focus, bool bypass_class_check) const;

private:
    EffectCache build_cache() const;
    void cache_negates(EffectCache& c) const;
    void cache_buff_slot(EffectCache& c, int slot) const;
    void cache_worn_items(EffectCache& c) const;
    void cache_aas(EffectCache& c) const;
    std::int32_t total_haste(std::int32_t best_haste, std::int32_t item_haste, std::int32_t spell_haste,
                             std::int32_t haste2, std::int32_t haste3) const;
    std::int32_t stat_sum(bool include_buffs, std::int32_t spa, std::int32_t StatBlock::*field, std::int32_t base);
    std::int32_t calc_resist(bool include_buffs, std::int32_t base, std::int32_t spa, bool all_resists,
                             std::int32_t StatBlock::*field, std::int32_t DerivedStats::*heroic_field, int cap_index);
    std::int32_t store_and_cap(std::int32_t StatBlock::*field, std::int32_t total, int cap_index);
    std::int32_t drunk_scaled(bool include_buffs, std::int32_t total) const;
    std::int32_t drunk_stat_shift() const;
    bool usable_for_stats(const net::Item& item) const;
    bool owner_leadership() const;
    std::int32_t pool_current(bool update, std::int32_t max, std::int32_t stored, std::int32_t cap_spa,
                              std::int32_t* out) const;
    // The item lookups the original walks: worn slots 0..22, then the 21 extra items (sheet.extra_items).
    const net::Item* walk_item(int index) const;

    const CharacterSheet& sheet_;
    const GameTables& tables_;
    SpellLookup spells_;
    Fpu fpu_;
    mutable std::optional<EffectCache> cache_;
    DerivedStats d_;
    mutable std::int32_t spawn_hp_max_ = 0, spawn_hp_current_ = 0, stored_hp_ = 0, encumbered_state_ = 0;
    mutable const net::Item* last_focus_item_ = nullptr; // the item best_item_focus found
};

// CharacterZoneClient::CastSpell's cast time (@0x43c372): the spell's cast time plus GetCastingTimeModifier and
// GetFocusCastingTimeModifier, 0 when that takes it all, at least half the spell's cast time (@0x43c42a).
std::uint32_t cast_time_ms(const Character& ch, const SpellData& spell);

// The API other modules use (movement: SPA 3 run speed): the total of one
// spell effect for a sheet, building the effect cache once per call.
std::int32_t total_effect(const CharacterSheet& sheet, const GameTables& tables, const SpellLookup& spells,
                          std::int32_t spa, bool include_items = true, std::int32_t subindex = 0,
                          bool include_aa = true, bool include_buffs = true);

} // namespace hailstone::game::mechanics
