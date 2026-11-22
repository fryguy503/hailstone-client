#pragma once

// Which sound the original plays for which event (wave ids of soundassets.txt;
// the playing itself is audio/). Sources in the original:
//   PlayerClient::SetSounds @0x58d7f0 and
//   RaceManager_GetSoundID @0x7f46d0: a spawn's 44 sound ids from racedata.txt;
//   PlayerAnimation_PlayAnimationSound @0x4b09d0: the sound of an animation
//   (attacks, steps, jumps, swimming, death);
//   CEverQuest::ReportSuccessfulHit @0x52ee40 and 0x514920: hit and block sounds;
//   EQSwitch::ChangeState @0x50f540: door sounds;
//   Handle_OP_LevelUpdate @0x4b9c10: level up;
//   PlayerZoneClient_LandingAndFallCheck @0x506e20 and IHaveFallen @0x58cd20: landing.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "assets/sound_files.h"

namespace hailstone::game {

// RaceManager_GetSoundID's kinds 0..43 (eqlib PlayerClient *_SoundID names).
enum class SoundKind : std::uint8_t {
    Loop3d = 0, Idle1, Idle2, Jump, Hit1, Hit2, Hit3, Hit4, Gasp1, Gasp2, Death, Drown, Walk, Run, Crouch,
    Attack1, Attack2, Attack3, Swim, TreadWater, Climb, Sit, Kick, Bash, FireBow, MonkAttack1, MonkAttack2,
    MonkSpecial, PrimaryBlunt, PrimarySlash, PrimaryStab, Punch, Roundhouse, SecondaryBlunt, SecondarySlash,
    SecondaryStab, SwimAttack, TwoHandedBlunt, TwoHandedSlash, TwoHandedStab, SecondaryPunch, JumpAcross,
    WalkBackwards, CrouchWalk,
};
inline constexpr std::size_t kSoundKinds = 44;

struct SpawnSounds {
    std::array<std::int32_t, kSoundKinds> ids{};
    std::int32_t operator[](SoundKind k) const { return ids[static_cast<std::size_t>(k)]; }
};

// PlayerClient::SetSounds for a race and gender (1 = female, else male):
// 13 races borrow another's sounds; missing ids take the original's defaults.
SpawnSounds spawn_sounds(const std::unordered_map<std::int32_t, assets::RaceSoundIds>& races, std::int32_t race,
                         std::uint8_t gender);

// ---- doors ----------------------------------------------------------------

// EQSwitch::ChangeState: the sounds for a switch of `type` that just changed to
// `state` (opening when it becomes 2, or 3 for some types; closing at 3; closed at 0).
std::vector<std::int32_t> door_sounds(std::uint8_t type, std::uint8_t state);

// ---- combat ---------------------------------------------------------------

// The defender's hit sound(s) when a melee hit lands (ReportSuccessfulHit,
// damage > 0, only for players and NPCs, not corpses): for a player-race hit
// voice (ids 32..89) first FlshHit1/2 (0x93 / 0x94 when hp / damage < 10,
// hp = the defender's HP if it is the local player, else its level x 30), then
// one of Hit1..4 at random (`pick`: 1..4). An environmental hit plays only
// the Hit1..4 (the FlshHit is in the branch for types below 231).
struct HitInput {
    std::int32_t damage = 0;
    bool defender_is_me = false;
    std::int32_t defender_hp = 0;     // the local player's current HP
    std::int32_t defender_level = 0;
    bool defender_corpse = false;
    bool has_animation_set = true;    // GetThisField_0x1024 (the actor's animation set)
    bool environment = false;         // an environmental hit (0xf3..0xfd): only the shared tail's Hit1..4
};
std::vector<std::int32_t> hit_sounds(const SpawnSounds& defender, const HitInput& in, std::int32_t pick);

// The sound of a blocked hit (0x514920, damage -1): StaffBlk, SwordBlk when
// both hold a type-2 item, and ShldBlk half of the time (`coin` 0 or 1) when the
// defender's left hand holds a shield (type 3).
std::int32_t block_sound(std::int32_t attacker_right, std::int32_t defender_right, std::int32_t defender_left,
                         std::int32_t coin);
// Other misses: dodge (-4) plays fall_hit (0x73), rune (-6) StaffBlk (0x84); others none.
std::optional<std::int32_t> miss_sound(std::int32_t damage);

// 0x489ec0 (PlayerClient::SetHeldModel's RightHolding / LeftHolding): the
// kind of item model IT<n> held: 0 none, 1 other, 2 blade (and the table's
// other 1..62 entries), 3 shield (IT200..300, some low numbers), 4, 5 whip (IT10778).
std::int32_t held_type(std::uint32_t item_model);

// ---- animations ------------------------------------------------------------

// PlayerAnimation_PlayAnimationSound for one spawn. `action` is the classic
// animation number (OP_Animation's: 1..11 attacks C01..C11, 17 walk, 18 run,
// 19/20 jump, 22 crouch walk, 23 climb, 25 swim, 26 idle, 33/36 sit,
// 37 tread water, 15 drown, 16 death). Call it whenever the spawn's animation
// is (re)started or, while walking/running/swimming, every frame.
struct AnimationSoundInput {
    std::int32_t action = 0;
    bool local = false;            // the local player
    bool sneaking = false;         // +0x264
    bool flying = false;           // the physics mode at vtable +0x78 is not 0
    std::int32_t race = 0;
    std::int32_t right_holding = 0; // +0x119c
    std::int32_t left_holding = 0;  // +0x11a0
    bool legal_player_race = true;
    std::uint8_t spawn_type = 0;    // 0 player, 1 NPC
    std::uint8_t stand_state = 100;
    std::int32_t hp = 1;
};
struct AnimationSoundState {
    std::int32_t animation = -1;     // the last animation (PlayerClient +Animation)
    std::uint32_t last_walk_ms = 0;  // LastWalkTime
    bool ladder_left = false;        // +0x10e0: climbing alternates two step sounds
};
struct AnimationSounds {
    std::vector<std::int32_t> play;            // played at the spawn (PlaySound), in order
    std::optional<std::int32_t> loop;          // SetCurrentLoopSound(id) (0 = Loop3d)
    bool loop_is_step = false;                 // `loop` is the spawn's own walk or run sound (a footstep), not its ambient loop
    bool only_if_not_playing = false;          // local player: skipped while the same wave plays
};
// Whether an animation's sound is a footstep in the sense of the "Individual Sounds" volumes: walking, running,
// jumping, crouch walking, climbing, swimming and treading water (the animations with a step, jump or stroke).
bool is_movement_animation(std::int32_t action);
// `random(low, high)` is RandomNumberLowHigh.
using RandomLowHigh = std::function<std::int32_t(std::int32_t, std::int32_t)>;
AnimationSounds animation_sounds(const SpawnSounds& s, const AnimationSoundInput& in, AnimationSoundState& st,
                                 std::uint32_t now_ms, const RandomLowHigh& random);

// ---- spell effect stages (DoSpellEffect @0x522500) ------------------------

// One stage of a spell's effect (0 cast, 1 projectile, 2 impact), as DoSpellEffect plays it (the recorded vectors
// DoSpellEffect.jsonl). Its callers: CEverQuest::StartCasting (stage
// 0, caster = target = the casting spawn), EQPlayer::TriggerSpellEffect from PlayerZoneClient::IWasHit for every
// OP_Action of type 231, both copies (stage 2, caster = the packet's word +2 spawn, target = the struck spawn), and
// the missiles (stage 1 / 2 without a target).
//
// The spell's effect pair is {old spells.eff record (spaindex +0x160, 0..255), new SpellsNew.eff record (SpellAnim
// +0x164, 1..count with a name)} (SpellMgr_SetSpellExtraData @0x467260). The stage's sound is the first word of the
// stage: new record +0x40 + stage * 0x44, else old record +0x224 + stage * 0x3a0; -1 = none.
struct SpellEffectStageInput {
    std::uint32_t stage = 0;
    bool have_spell = true;
    bool have_effect = true;          // the spell's extra data pair
    bool have_old_record = false;
    bool have_new_record = false;
    bool have_caster = false;
    bool have_target = false;
    std::array<std::int32_t, 3> stage_sounds{-1, -1, -1}; // of the record in use (new, else old)
    bool particles_filtered = false;  // 0x514bd0(stage, caster, target)
    std::int32_t dragon_breath = 0;   // EQ_Spell::SpellUsesDragonBreathEffect (asked only when needed)
    float particle_density = 1.0f;    // 0xe634e8
    float particle_opacity = 1.0f;    // 0xe634ec
    std::int32_t spell_effects_option = 1; // 0xde0a5c
};
struct SpellEffectStage {
    std::uint32_t value = 0;                  // 0: nothing done (no spell, effect or caster, or stage > 2)
    std::optional<std::int32_t> sound;        // PlayerClient::PlaySound(this) at ...
    bool sound_at_target = false;             // ... the target (stage 2), else the caster (stage 0)
    bool filter_asked = false;                // 0x514bd0 called
    bool dragon_breath_asked = false;         // SpellUsesDragonBreathEffect called
    bool spawn_stage_emitters = false;        // SpellEffect_SpawnStageEmitters (new record)
    bool do_particle_effect = false;          // DoParticleEffect (old record only)
};
SpellEffectStage spell_effect_stage(const SpellEffectStageInput& in);

// The stage sounds of an old spells.eff record (0xae8 bytes; 256 of them).
inline constexpr std::size_t kOldSpellEffectSize = 0xae8;
std::array<std::int32_t, 3> old_spell_effect_sounds(std::span<const std::byte> record);

// ---- containers -----------------------------------------------------------

// CContainerMgr::OpenContainer @0x65cae0 / CloseContainer @0x65d2f0 (its wave at 0x65d180): the
// sound of a bag of `bag_type` opening or closing. Two one-byte tables (open
// 0x65cda8, close 0x65d2d4), 17 entries at bag_type - 6: 0 is the bag's own
// sound (0x86 opening, 0x85 closing), anything else the shared container
// sound (0x91). A type outside 6..22 takes the shared sound too.
inline constexpr std::int32_t kSoundBagOpen = 0x86;
inline constexpr std::int32_t kSoundBagClose = 0x85;
inline constexpr std::int32_t kSoundContainer = 0x91;
// The first bag type with its own wave and how many types the tables cover (6..22).
inline constexpr std::uint32_t kFirstOwnBagType = 6;
inline constexpr std::size_t kOwnBagTypeCount = 17;
std::int32_t container_sound(std::uint32_t bag_type, bool open);

// The four step sounds a walking spawn picks from (StepWlkR, StepWlkL, StepWlk2, StepWlk3).
inline constexpr std::int32_t kWalkSteps[4] = {0x5d, 0x5c, 0x97, 0x98};

// ---- other events ----------------------------------------------------------

inline constexpr std::int32_t kSoundLevelUp = 0x8b;     // LevelUp.WAV, 2D
inline constexpr std::int32_t kSoundJumpLand = 0x72;    // jumpland.WAV
inline constexpr std::int32_t kSoundFallHurt = 0x73;    // fall_hit.WAV
inline constexpr std::int32_t kSoundCoin = 0x8a;        // the cash register: every sale, purchase, loot and coin move
inline constexpr std::int32_t kSoundTeleport = 0x89;    // Teleport.WAV
inline constexpr std::int32_t kSoundItemMoved = 0x92;   // wearclth.WAV, 2D: an item moved by a click (0x697148)
inline constexpr std::int32_t kSoundEndQuest = 0x8d;    // EndQuest.WAV, 2D (0x4207a7 won, 0x4c7ff6 coin)
inline constexpr std::int32_t kSoundAdventureFailed = 0x8c; // LevDn.WAV, 2D (0x4207e7)
inline constexpr std::int32_t kSoundStruckDown = 0x8f;  // thunder1.wav, 2D (0x4c8aab)
inline constexpr std::int32_t kSoundWaterOut = 0x63;    // WaterOut.WAV: leaving a liquid region (0x59ec30)
inline constexpr std::int32_t kSoundWaterIn = 0x64;     // WaterIn.WAV: entering one at a vertical speed of at least 1

// The landing sounds: LandingAndFallCheck's jumpland (`land_sound`: out of
// water at 2 x SpeedZ <= -2.5, game/mechanics/physics.cpp), then IHaveFallen's
// fall_hit when the fall did damage, else jumpland.
std::vector<std::int32_t> landing_sounds(bool land_sound, std::optional<std::int32_t> fall_damage);

} // namespace hailstone::game
