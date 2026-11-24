#pragma once

// The timed character updates CharacterZoneClient::DoPassageOfTime runs
// (docs/mechanics.md, Cadences): the 6 s buff tick (ProcessAffects) and
// mana regen tick, and the 1 s endurance upkeep. They follow the original
// exactly, checked against its vectors (tests/mechanics_vectors_test.cpp).

#include <array>
#include <cstdint>
#include <functional>

namespace hailstone::game::mechanics {

// CharacterZoneClient_ApplyManaRegenTick @0x4579e0: below the maximum
// (`current` is Cur_Mana(1), `maximum` the character's virtual +0x24 (1)),
// adds GetManaRegen(1, 0) (`regen`, asked only then) to the stored mana
// (profile +0x338c). Returns the stored mana.
std::int32_t apply_mana_regen_tick(std::int32_t stored, std::int32_t current, std::int32_t maximum,
                                   const std::function<std::int32_t()>& regen);

// CharacterZoneClient_ApplyEnduranceUpkeep @0x448bb0: the upkeep
// (CharacterZoneClient +0x94) comes off the stored endurance (profile
// +0x3390); when the endurance (`endurance`, the CharacterBase virtual
// +0x70, given the stored value) is then negative it becomes 0 and a player
// spawn drops its active discipline (RemovePCAffectex).
struct EnduranceUpkeep {
    std::int32_t endurance = 0;
    bool drop_discipline = false;
};
EnduranceUpkeep apply_endurance_upkeep(std::int32_t stored, std::int32_t upkeep,
                                       const std::function<std::int32_t(std::int32_t stored)>& endurance,
                                       bool player_spawn);

// ---- the buff tick

// The spell fields ProcessAffects reads.
struct AffectSpell {
    std::int32_t id = 0;
    std::int32_t duration_formula = 0; // +0x1c: 0x32 keeps a buff with duration -1
    float float_224 = 0.0f, float_21c = 0.0f; // either nonzero (NaN too): the slot's effects are cached again
    std::uint8_t drain_removes = 0;    // +0x26b: 1 removes a draining buff the pool cannot pay
    bool uninterruptible = false;      // +0x240
    std::array<std::int32_t, 12> spa{}, base{}, base2{}; // +0x11c, +0x2c, +0x5c
};

// One buff slot (CharacterZoneClient_GetEffect).
struct AffectBuff {
    std::uint8_t type = 0;        // +0: 0 empty; 2 and up tick; 4 negates its pool change
    std::uint8_t caster_level = 0; // +1
    std::int32_t spell_id = 0;    // +8
    std::int32_t duration = 0;    // +0xc: ticks left; -1 permanent (formula 0x32), 0 expires
};

// What ProcessAffects reads and writes besides the buffs.
struct AffectState {
    std::uint8_t class_id = 0;     // profile +0x3374: classes 1, 7, 8, 9, 16 skip the mana ticks
    std::int32_t mana = 0;         // profile +0x338c
    std::int32_t endurance = 0;    // profile +0x3390
    std::uint8_t spawn_type = 0;   // PlayerBase::Type
    float speed_run = 0.0f;        // +0x7c
    float speed_multiplier = 0.0f; // +0x18
    std::int32_t casting_spell_id = -1; // +0x448
    std::uint8_t hold_flag = 0;    // the spawn's +0x22d (SPA 0x1b4)
    std::uint8_t tick_cache_valid = 0; // +0x1a4: cleared when a slot's tick varies
    bool stats_dirty = false;      // +0xc
    bool controlled_present = false; // pinstControlledPlayer
    bool controlled_is_eye = false;  // an Eye of became the controlled player
};

// The callees ProcessAffects asks, in the order it asks them.
class AffectQueries {
public:
    virtual ~AffectQueries() = default;
    virtual int max_effects() = 0;                      // GetMaxEffects (asked every iteration)
    virtual AffectBuff& effect(int index) = 0;          // GetEffect
    virtual bool valid_spell(std::int32_t id) = 0;      // IsValidSpellID
    virtual const AffectSpell* spell(std::int32_t id) = 0; // the spell manager's lookup (+0xc)
    virtual void cache_buff_slot_effects(int index) = 0;
    virtual bool countdown_held(const AffectSpell* spell) = 0; // Spell_IsBuffCountdownHeld
    virtual bool has_affect(std::int32_t spa) = 0;      // GetPCSpellAffect
    virtual bool tick_varying(std::int32_t spa) = 0;    // SpellCache_IsTickVarying
    // CalcAffectChange(spell, level, slot, buff, index, 0, 0, -1, 1).
    virtual std::int32_t affect_change(const AffectSpell& spell, const AffectBuff& buff, int slot, int index) = 0;
    virtual std::int32_t cur_endurance() = 0;           // EQ_Character::Cur_Endurance(1)
    virtual std::int32_t cur_mana() = 0;                // Cur_Mana(1)
    virtual void remove_affect(AffectBuff& buff, int a, int b) = 0;
    virtual bool spawn_present() = 0;                   // CharacterZoneClient +8, read at each check
    virtual bool has_mount() = 0;                       // EQPlayer::GetMount
    virtual bool find_summon(bool eye) = 0;             // "%s`s_Mount" / "Eye_of_%s" by partial name
    virtual void ride_mount() = 0;                      // the spawn's virtual +0xcc on the mount found
    virtual void view_eye() = 0;                        // CDisplay::SetViewActorByName
    // EQ_Spell::GetSpellLevelNeeded on the spell returned by the lookup.
    virtual std::int32_t spell_level_needed(const AffectSpell* spell, int class_id) = 0;
    virtual void chat(std::uint32_t string_id) = 0;     // dsp_chat, colour 0x121
    virtual std::int32_t rand() = 0;
    virtual void stop_cast() = 0;                       // StopCurrentSpellCast(1)
    virtual bool hp_tick_allowed(std::int32_t base2) = 0; // the spell manager's +8 (spawn, base2)
    // CalcTickHPChange(profile +0x2218[index], buff, slot, spell).
    virtual std::int32_t tick_hp_change(int index, const AffectBuff& buff, int slot, const AffectSpell& spell) = 0;
    virtual bool timers_on_hold() = 0;                  // AreBuffTimersOnHold
    virtual bool spell_has_spa(const AffectSpell& spell, std::int32_t spa) = 0; // EQ_Spell::SpellAffects
    virtual std::int32_t recheck(std::int32_t spell_id) = 0; // 0x4326b0
    virtual void end_stun() = 0;
    virtual void modify_cur_hp(std::int32_t amount) = 0; // ModifyCurHP(amount, 0)
};

inline constexpr std::uint32_t kStrLevitationFading = 0x30c2; // SPA 0x39
inline constexpr std::uint32_t kStrEffectFading = 0x30c3;     // SPA 0x3a / 0x9c

// CharacterZoneClient::ProcessAffects @0x43ca30: one buff tick. Every buff
// of type 2 and up counts its duration down by 1 (unless held), is removed
// at 0 (or -1 unless its formula is 0x32) and applies its slots' per-tick
// effects: HP (summed, applied once with ModifyCurHP), mana, endurance, the
// mount and eye summons, the fading messages, the cast interrupt chance.
// Returns 0.
std::int32_t process_affects(AffectState& state, AffectQueries& q);

} // namespace hailstone::game::mechanics
