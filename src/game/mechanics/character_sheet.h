#pragma once

// Everything the original client's character rules read about the local
// player, as plain data: identity, the profile's base values, skills, the
// items in the possessions slots (with their augments and bag contents),
// buffs, alternate abilities and the situational state (sitting, running,
// zone fast-regen divisors, ...). Built from the live connection by
// sheet_builder.h; tests and test vectors fill it directly.
//
// Field comments name the original's storage (PcProfile / CharacterBase /
// CharacterZoneClient offsets)
// so that a vector recorded from the original maps onto it one to one.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "game/mechanics/fpu.h"
#include "game/server_ext.h"
#include "net/item_packets.h"

namespace hailstone::game {
struct SpellData;
class SpellTable;
}

namespace hailstone::game::mechanics {

inline constexpr int kPossessionSlots = 34;  // CalcCarriedWeight: possessions 0..0x21 (worn, general, cursor)
inline constexpr int kWornSlots = 23;        // 0..22 (22 = ammo)
inline constexpr int kPlayerBuffSlots = 63;  // GetMaxEffects for a player character
inline constexpr int kSkills = 100;

// A buff slot (EQ_Affect, 0x58 bytes).
struct SheetBuff {
    std::uint8_t type = 0;        // +0x00: 0 empty; 2 normal; 4 inverted (its values are subtracted)
    std::uint8_t level = 0;       // +0x01: caster level
    float modifier = 1.0f;        // +0x04: bard instrument modifier (1.0 = none)
    std::int32_t spell_id = 0;    // +0x08
    std::int32_t duration = 0;    // +0x0c: ticks left
    std::uint32_t caster_id = 0;  // BuffCasterIDs[slot]
    // The caster spawn's 2D distance from the character (CDisplay::PlayerDistance, z scale 0) when the spawn is
    // known, none otherwise: CacheBuffSlotEffects scales distance-modified spells by it (spell_distance_mod).
    std::optional<float> caster_distance;
    bool caster_is_pc = false;    // the caster spawn's type is 0 (PvP DoT scaling)
    bool caster_is_self = false;  // the caster is this character
    bool empty() const { return type == 0; }
};

// One alternate ability the character owns (PcBase::GetAlternateAbilityId +
// AltAdvManager::GetAAById): the parts the rules read.
struct SheetAAEffect {
    std::int32_t spa = 0, base = 0, limit = 0;
};
struct SheetAbility {
    std::int32_t id = 0;
    std::uint32_t class_mask = 0xffffffffu; // +0x68: bit (1 << class) (AltAbility_IsClassAllowed @0x4a1f50)
    std::int32_t required_level = 0;        // +0x18
    bool ignores_level = false;             // +0x84
    std::vector<SheetAAEffect> effects;     // the effect list (+0x9c) in order
};

enum class SpawnType : std::uint8_t { PC = 0, NPC = 1, Corpse = 2, Other = 3 }; // PlayerBase::Type (+0x125)

struct CharacterSheet {
    // ---- identity (profile) ----
    std::uint32_t race = 1;           // +0x3370
    std::uint8_t class_id = 1;        // +0x3374 (the profile class; multiclass servers: see class_mask)
    std::uint32_t class_mask = 0;     // the server extension's classes, bit c-1 = class c (0 = class_id only)
    // A server extension's (game/server_ext.h focus_classes): the focus class tests take any of these classes (bit
    // c-1) and drop the Bard / pure melee filter (a server patch's; the projecttriune plugin switches it on). 0 stock.
    std::uint32_t focus_classes = 0;
    const SkillRules* skill_rules = nullptr; // the server extension's skill rules (game/server_ext.h); null = stock
    CharacterVitals vitals;           // the server extension's HP / mana / endurance answers (game/server_ext.h)
    std::int32_t level = 1;           // CharacterBase virtual +0x68 (the character's level)
    std::optional<std::int32_t> profile_level; // BaseProfile::Level +0x3388 (the base value lookups); unset = level
    std::int32_t highest_level = 1;   // PcBase_GetHighestLevel
    std::uint32_t deity = 396;        // GetDeity (396 agnostic)
    std::uint8_t heritage = 0;        // +0x3478: Drakkin heritage 0..5
    std::uint32_t expansions = 0;     // CharacterBase virtual +0x5c: 0x40 leadership bonus, 0x2000 heroic stats
    std::int32_t profile_type = 0;    // +0x340c (0 normal; non-zero: monster/mercenary profiles)

    // ---- spawn ----
    SpawnType spawn_type = SpawnType::PC; // +0x125
    bool has_spawn = true;                // CharacterZoneClient::me != null
    bool is_pet = false;                  // MasterID (+0x38c) != 0
    bool is_mercenary = false;            // +0x208
    bool summoned = false;                // bSummoned (+0x2e0): with an NPC type, a pet (TotalEffect haste cap)
    std::int32_t master_level = -1;       // the master spawn's level (-1: not found)
    std::int32_t spawn_hp = 0, spawn_max_hp = 0; // PlayerZoneClient HPCurrent / HPMax (formulas 137/138)
    float hp_regen_modifier = 1.0f;       // spawn +0x58
    float endurance_regen_modifier = 1.0f; // spawn +0x5c
    float mana_regen_modifier = 1.0f;     // spawn +0x60
    bool berserk = false;                 // spawn +0x438 (CalcSTR)
    bool spawn_sitting = false;           // PlayerZoneClient::StandState (+0x35c) == 110 (GetManaRegen)

    // ---- the owning PcBase (CharacterBase virtual +0x4c; the player's own in game) ----
    bool has_owner = true;
    std::uint32_t owner_expansions = 0;   // its expansion flags (Max_HP and GetHPRegen leadership checks)
    std::int32_t owner_status = 0;        // its CharacterBase::Status (+0xfc); 3: GetHPRegen gives -1 when hurt
    std::int32_t owner_food = 1, owner_drink = 1; // its profile's hunger / thirst (GetHPRegen)

    // ---- account / base virtuals with no packet source yet ----
    bool ignores_item_restrictions = false; // CharacterBase virtual +0x34 (skips race/deity/prestige checks)
    std::int32_t prestige_limit = -1;       // CharacterBase virtual +0 (0x13, GetGameFeature UseItem); -1 = none
    bool level_cap_100 = false;             // CharacterBase virtual +0x8c (GetMaxLevel)
    bool slowdown_any_level = false;        // CharacterBase +0x460 non-zero: CalculateWeight's level threshold 0 (else 3)
    bool profile_null = false;              // no current profile (GetAdjustedSkill's check)
    std::int32_t level_cap_override = 0;    // CharacterBase virtual +0 (1); used when > 0 and lower

    // ---- profile values ----
    std::int32_t base_str = 0, base_sta = 0, base_cha = 0, base_dex = 0, base_int = 0, base_agi = 0, base_wis = 0;
    std::int32_t base_save_magic = 0, base_save_fire = 0, base_save_cold = 0; // +0x3430, +0x3434, +0x3438
    std::int32_t base_save_disease = 0, base_save_poison = 0;                 // +0x343c, +0x3440
    std::int32_t base_save_physical = 0, base_save_corruption = 0;            // +0x3444, +0x3448
    std::int32_t base_ac = 0;                                                 // +0x3464
    std::int32_t fist_damage = 0, fist_delay = 0;                             // +0x346c / +0x3470 (DoAttack)
    std::int32_t own_base_hp = 0, own_base_endurance = 0, own_base_mana = 0;  // +0x3424/+0x3428/+0x342c (profile_type != 0)
    std::int32_t own_hp_regen = 0, own_endurance_regen = 0;                   // +0x3454 / +0x3460 (profile_type != 0)
    std::int32_t mana_regen_sitting = 0;  // +0x3458
    std::int32_t mana_regen_standing = 0; // +0x345c
    std::int32_t innate_3258 = 0xff;      // +0x3258 (an innate ability slot; 0xff = none): GetHPRegen
    std::array<std::int32_t, kSkills> skills{}; // +0x30a0
    std::int32_t stored_hp = 0;           // +0x3394 BaseHP (current HP without the HP bonus)
    std::int32_t mana = 0, endurance = 0; // +0x338c / +0x3390
    std::int32_t food = 1, drink = 1;     // +0x3404 / +0x3400 (0 = starving / dehydrated)
    std::int32_t drunkenness = 0;         // PcBase_GetDrunkenness

    // ---- gear and effects ----
    std::array<std::optional<net::Item>, kPossessionSlots> possessions{}; // 0..22 worn, 23..32 general, 33 cursor
    // The 21 other item lookups the original walks after the worn slots (0x42def0 0..4, 0x42df20 0..9, the
    // character's virtuals +0x4c 0..1 and +0x54 0..3). Received personal tribute items fill 0..4 and guild tribute
    // items fill 15..16. The trophy lookups (5..14 and 17..20) have no item source on stock EQEmu.
    std::array<std::optional<net::Item>, 21> extra_items{};
    std::vector<SheetBuff> buffs;          // index = buff slot (GetMaxEffects of them are read)
    std::vector<SheetAbility> abilities;   // owned alternate abilities

    // ---- situation ----
    bool sitting = false;            // CharacterBase +0x108 / spawn +0x35c == 110
    bool can_med_on_horse = false;   // CharacterZoneClient::CanMedOnHorse
    bool attacking = false;          // __Attack
    bool running = false;            // __RunWalkState == 1 and the spawn's speed (+0x7c) is not 0
    bool fast_regen_resting = false; // PcZoneClient_IsRestingForFastRegen
    bool buff_timers_on_hold = false; // the spawn's +0x22d (CharacterZoneClient_AreBuffTimersOnHold)
    std::uint32_t sitting_ms = 0;    // display time (+0x154) - spawn +0x374 (time since sitting down)
    bool tutorial = false;           // byte 0xdd25a1: low levels get a flat regen
    std::int32_t fast_regen_hp_divisor = 0, fast_regen_mana_divisor = 0, fast_regen_endurance_divisor = 0;
    bool zoning = false;             // CharacterZoneClient::zoningStatProcessing (+0xd)
    bool pvp_ruleset = false;        // global 0xdcee70: PvP resist-debuff and DoT scaling
    std::uint32_t zone_id = 0;       // unk_CurrentZoneID (bazaar / barter zones: CalculateWeight)
    bool bazaar_zone = false, barter_zone = false;
    std::int32_t encumbered_state = 0; // int 0xddf748: 1 while "You are encumbered" stands (CalculateWeight)

    // ---- group / raid leadership (Leadership_Get*Bonus; no packet source yet) ----
    bool leadership_active = false;  // pinstLeadership (0xe63b58) exists
    std::int32_t leadership_hp_bonus = 0, leadership_mana_bonus = 0, leadership_hp_regen_bonus = 0;

    // ---- arithmetic ----
    FpuPrecision fpu = FpuPrecision::Single24;

    bool is_pc() const { return has_spawn && spawn_type == SpawnType::PC; }
    const net::Item* item(int slot) const {
        return slot >= 0 && slot < kPossessionSlots && possessions[static_cast<std::size_t>(slot)]
                   ? &*possessions[static_cast<std::size_t>(slot)]
                   : nullptr;
    }
};

// Spell records by id (null for unknown ids).
using SpellLookup = std::function<const SpellData*(std::int32_t id)>;
SpellLookup spell_lookup(const SpellTable& table);

} // namespace hailstone::game::mechanics
