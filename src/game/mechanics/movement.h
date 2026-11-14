#pragma once

// The original client's movement rules for the local player: run speed from
// spell effects, encumbrance, swimming, the controls (ProcessControls), the
// jump, swim jump, levitation, falling and fall damage, air supply. The
// physics integration is game/mechanics/physics.h.
//
// Follow the original exactly and are checked against its vectors:
//   CharacterZoneClient::eqspa_movement_rate @0x439570
//   EQ_Character::CalculateWeight @0x44cdd0
//   CharacterZoneClient_CalcCarriedWeight @0x44d020
//   PlayerClient::SetAccel @0x58b980
//   PhysicsObject vf14 (strafe) @0x59dd20
//   PlayerClient::DoSwimJump @0x58c940
//   PlayerClient_CanSwimJumpNow @0x59fb60
//   CharacterZoneClient_UpdateLevitationState @0x433480
//   PcClient::MaxAirSupply @0x581da0
//   PlayerZoneClient::IHaveFallen @0x58cd20
//   PlayerZoneClient_LandingAndFallCheck @0x506e20      [physics.h]
//   PlayerClient_IsSwimSurfaceClear @0x506bb0
//   EQ_Character::CanIBreathe @0x448b00
//   PlayerZoneClient::InitSneakMod @0x58b8f0
// with vectors (tests/movement_vectors_test.cpp): CEverQuest::ProcessControls
// @0x52bbd0 and GetSwimmingSpeed @0x5134c0 (game/mechanics/controls.h).
// Checked against the original's vectors too (its hit record leaves bytes
// unwritten):
//   CharacterZoneClient_ApplyFallDamage @0x45ac60
// Without vectors; worked from the original's disassembly
// (each step names its address), with unit tests worked from it:
//   CEverQuest::DoMainLoop's jump @0x53e84e..0x53edaf (main_loop_jump)
//   PcClient::ProcessAirSupply @0x584ac0
//   LocalPlayer_ApplyEnvironmentRegionDamage @0x459860
//
// Float math: the functions that follow the original exactly take the x87
// precision (game/mechanics/fpu.h; in game 24-bit, the default) and round where
// the original stores a float; the recorded vectors cover both precisions. The
// controller runs at 24-bit precision.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "game/mechanics/controls.h"
#include "game/mechanics/fpu.h"
#include "game/mechanics/physics.h"

namespace hailstone::game::mechanics {

// ---------------------------------------------------------------- constants

inline constexpr std::uint32_t kJumpCooldownMs = 1500;    // ProcessControls `CMP ECX,0x5dc` @0x52c661
inline constexpr std::uint32_t kJumpWindowMs = 500;       // DoMainLoop @0x53e9b8 (the jump animation's length when known)
inline constexpr std::uint32_t kSwimJumpCooldownMs = 1500; // DoSwimJump: +0x42c = now + 0x5dc
// JumpStrength (+0x10) and SwimStrength (+0x14): PlayerZoneClient::ResetVariables writes 1.6 and 3.2 (0x9c834c,
// 0x9c7db8), but EQPlayer::SetHeights @0x5a3f40, which runs after it for every new spawn and again on every size
// change, writes these (0x9d79c8, 0x9d79c4; game/spawn_heights.h).
inline constexpr float kJumpStrength = 1.35f;
inline constexpr float kSwimStrength = 3.375f;
inline constexpr float kSpeedCap = 1.092006f;             // ProcessControls @0x52be09
inline constexpr float kFallSpeedThreshold = -2.5f;       // LandingAndFallCheck (0x9c8328)
inline constexpr std::int32_t kLethalFall = 200000;       // ApplyFallDamage f >= 9
inline constexpr std::uint8_t kEnvDamageFalling = 0xfc;   // EnvDamage type: falling
inline constexpr std::uint8_t kEnvDamageDrowning = 0xfb;
inline constexpr std::uint8_t kEnvDamageLava = 0xfa;
inline constexpr std::uint8_t kEnvDamageFreezing = 0xf6;
inline constexpr std::uint8_t kEnvDamageFire = 0xf4;      // region 200 lava

// eqstr ids the movement code prints.
inline constexpr std::uint32_t kStrTooFatiguedToJump = 12362; // ProcessControls @0x52c6de
inline constexpr std::uint32_t kStrEncumbered = 12392;        // CalculateWeight 0x3068
inline constexpr std::uint32_t kStrNoLongerEncumbered = 12391; // 0x3067

// ------------------------------------------------------------ speed effects

// CharacterZoneClient::eqspa_movement_rate @0x439570: the spawn's speed
// modifier (+0x18 SpeedMultiplier) from a movement rate effect `rate`
// (TotalEffect SPA 99 when negative, else SPA 3). `running` is
// __RunWalkState != 0; `spa271` the cached SPA 271 (run speed AA, buffs,
// items). With a mount, a positive rate only zeroes the mount's modifier.
struct MovementRate {
    float speed_multiplier = 0.0f;
    bool rooted = false;   // rate <= -1000: speeds stop and auto follow is turned off
    bool changed_player = true; // false when a mount took a positive rate (the player's modifier is kept)
    std::optional<float> mount_multiplier;
};
MovementRate eqspa_movement_rate(std::int32_t rate, float run_speed, bool running, std::int32_t spa271,
                                 std::optional<float> mount_run_speed = std::nullopt, const Fpu& fpu = Fpu{});

// The same on the spawn's (and its mount's) speed fields, exactly as the
// original writes them: a root (rate <= -1000) stores the rate and zeroes
// SpeedRun, the velocity and SpeedHeading. `spa271` is the buff, AA and item
// caches; the item part counts for a player (type 0), a pet (MasterID) or a
// mercenary. Returns true when auto follow is turned off (a root).
struct SpawnSpeeds {
    float run_speed = 0.0f;        // +0x350 RunSpeed
    float speed_multiplier = 0.0f; // +0x18
    float speed_run = 0.0f;        // +0x7c
    float speed_y = 0.0f, speed_x = 0.0f; // +0x70 / +0x74
    float speed_heading = 0.0f;    // +0x8c
};
struct Spa271Caches {
    std::int32_t buff = 0, aa = 0, item = 0;
};
bool apply_movement_rate(std::int32_t rate, bool running, const Spa271Caches& spa271, bool items_count,
                         SpawnSpeeds& spawn, SpawnSpeeds* mount, const Fpu& fpu = Fpu{});

// The movement rate eqspa_movement_rate is called with each second
// (DoPassageOfTime @0x45ee7d): TotalEffect(99) when negative, else TotalEffect(3).
inline std::int32_t movement_rate_effect(std::int32_t spa99, std::int32_t spa3) { return spa99 < 0 ? spa99 : spa3; }

// PlayerClient::SetAccel @0x58b980: the new SpeedRun (+0x7c) toward
// `target`. Set at once with `immediate` or without a rider; a spawn with a
// rider (a mount) ramps by rate_scale (0xde0a68) * 50 * its run speed over
// 400 rising / 200 falling, or 1500 when the rider has no PcClient or the
// PcClient's virtual +0x80 holds (@0x57d190: the OP_RestState combat byte
// +0x31bc; the PcClient is 0x59f730's: the rider's +0x2cc, its
// CharacterBase virtual base's +0x4c thunk @0x57d1e0), stopping at the target.
struct AccelRider {
    bool has_pc = false;   // the rider's PcClient (0x59f730 on its +0x3c)
    bool pc_flag = false;  // that PcClient's virtual +0x80: in combat (OP_RestState)
};
float set_accel(float current, float target, bool immediate, std::optional<AccelRider> rider, float rate_scale,
                float run_speed, const Fpu& fpu = Fpu{});

// ProcessControls @0x52bde9..: above 1.092006 the modifier is capped at
// 1.092006 + SPA 290 / 100.
float cap_speed_multiplier(float speed_multiplier, std::int32_t spa290, const Fpu& fpu = Fpu{});

// GetSwimmingSpeed @0x5134c0: base * 2.15 rounded
// to float, + Swimming * 0.01 * 1.5, returned on the FPU stack unrounded.
// Both factors are data (0xad82e4, 0xad82e0). `scaled_base` gets the
// rounded product the original stores back into its argument.
double swimming_speed(float base, std::int32_t swimming_skill, const Fpu& fpu = Fpu{}, float speed_scale = 2.15f,
                      float skill_scale = 1.5f, float* scaled_base = nullptr);

// 0x516cd0: a physics block's pitch (+0x2c) += delta, then
// at most 128 and at least -128 (-64 when the local player's mount is the
// controlled player); the clamps compare the unrounded sum.
float add_physics_pitch(float pitch, float delta, bool riding_controlled_mount, const Fpu& fpu = Fpu{});

// CharacterZoneClient::HasSkill @0x44a1b0: GetAdjustedSkill(skill) > 0.
inline bool has_skill(std::int32_t adjusted_skill) { return adjusted_skill > 0; }

// PlayerZoneClient::InitSneakMod @0x58b8f0: the sneak speed (+0x11c8): 0.5,
// rogues above level 50 + 0.1 per level (at most 10 levels).
float sneak_speed(std::uint8_t class_id, std::uint8_t level);

// -------------------------------------------------------------- encumbrance

struct WeightInput {
    std::int32_t hp = 0, max_hp = 0;   // Cur_HP / Max_HP
    std::int32_t level = 1;            // the CharacterBase virtual +0x68
    bool low_level_exempt_off = false; // CharacterBase +0x460 (bSuperPKILL) set: the threshold is 0 not 3
    std::int32_t carried = 0;          // CurrWeight (+0x14), CalcCarriedWeight
    std::int32_t strength = 0;         // STR with buffs (CharacterBase +0x4a0): the capacity
    bool bazaar = false, barter = false; // EQWorldData_IsBazaarZone / IsBarterZone
};
// EQ_Character::CalculateWeight @0x44cdd0: the movement factor. 1.0;
// hurt above level 3 it is 0.5 below 20% and 50 HP, 0.75 below 33% and 100
// HP; carrying more than STR takes (carried - STR) / (2 STR) off (not below
// 0); 1 in the bazaar and barter zones.
float calculate_weight(const WeightInput& in, const Fpu& fpu = Fpu{});
// The encumbered message CalculateWeight prints when the carried weight
// crosses STR (the static at 0xddf748, starting at 0; 1 while shown):
// returns the eqstr to print, or 0.
std::uint32_t encumbrance_message(std::int32_t& shown, std::int32_t carried, std::int32_t strength);

// One item for CalcCarriedWeight: its weight (tenths, ItemBase_GetWeight),
// and for a bag its weight reduction and its contents' weights.
struct CarriedItem {
    std::int32_t weight = 0;
    bool bag = false;
    std::int32_t weight_reduction = 0; // percent
    std::span<const std::int32_t> contents; // weights (tenths) of the items in the bag
};
// CharacterZoneClient_CalcCarriedWeight @0x44d020 over the 34 possessions
// slots: each item's weight / 10, a bag's contents at (100 - reduction)%,
// less SPA 221 percent of the total, clamped to 0..32766.
std::int32_t calc_carried_weight(std::span<const CarriedItem> items, std::int32_t spa221, const Fpu& fpu = Fpu{});

// ------------------------------------------------------------ levitation

// CharacterZoneClient_UpdateLevitationState @0x433480: the fly mode from the
// buffs: SPA 57 in the buff cache (value 0) levitates (2), else its second
// value (5); mode 1 (flying mount) is kept; a rider follows its mount; a
// no-levitation zone (flag 8) or the cancel byte (0xddfc9c) forces 0 unless
// the spawn may fly anyway (its virtual +0xdc). It runs only when the effect
// cache is rebuilt (local_fly_mode below), so a mode set between rebuilds
// stands until the next one.
FlyMode levitation_mode(FlyMode current, bool spa57_value0, bool spa57_value1, std::optional<FlyMode> rider_mode,
                        bool no_levitate_zone, bool may_fly_anyway = false);
// The same on the raw mode values (the physics object's +0xc).
std::int32_t levitation_mode_value(std::int32_t current, bool spa57_value0, bool spa57_value1,
                                   std::optional<std::int32_t> rider_mode, bool no_levitate_zone, bool may_fly_anyway);

// What changed the local player's fly mode (the physics object's +0xc) since the last frame. Each writer sets the
// mode outright; only the cache rebuild applies the buff, flying-mount and no-levitation-zone rules.
struct FlyModeEvents {
    // CharacterZoneClient_RemoveAffect @0x439700, its SPA 57 case: a levitate buff removed while
    // PlayerZoneClient::Levitating @0x5a1140 (TotalEffect(57, 0 / 1)) finds no other levitation sets mode 0,
    // whatever it was (a flying mode 1 included). local_fly_mode reads "no other levitation" from the SPA 57
    // values it is given (the buff cache's; Levitating also counts worn items and AAs).
    bool levitate_faded = false;
    // EQ_Character::HitBySpell's SPA 57 case, FUN_00432b20 @0x432b20 with the effect's Base2: a levitate buff
    // landing sets mode 5 for a non-zero Base2, else 2 (over mode 1 too), or 0 in a no-levitation zone unless the
    // spawn may fly anyway.
    std::optional<std::int32_t> levitate_landed;
    // OP_SpawnAppearance type 19 (Handle_OP_SpawnAppearance @0x4b4b60) or the spawn record's flymode
    // (EQPlayer::UnSerializeSpawn): PlayerPhysics_SetFlyMode @0x8d0910 with no rules applied, on the spawn and its
    // mount.
    std::optional<FlyMode> server_mode;
    // CharacterZoneClient_ReCacheSpellEffects @0x438910, which ends with UpdateLevitationState: it runs at the next
    // effect read after anything clears the cache flag (+0x1a4), such as a buff landing, fading or refreshed.
    bool recache = false;
};
// The local player's fly mode after a frame's events, applied in the order above: the packets of a frame are
// handled before the frame's effect reads, so a rebuild in the same frame comes last. A server-set mode 2 to 5 (or
// 0) therefore lasts until the next rebuild, a server-set mode 1 is kept by it except in a no-levitation zone.
FlyMode local_fly_mode(FlyMode current, const FlyModeEvents& events, bool spa57_value0, bool spa57_value1,
                       std::optional<FlyMode> rider_mode, bool no_levitate_zone, bool may_fly_anyway = false);

// The levitation events in a change of the buff slots between two frames (the slots' spell ids, in slot order):
// `changed` when any slot's spell differs (the cache would be cleared), `faded` when a slot that held a levitate
// spell holds another or none (local_fly_mode checks that no levitation is left), `landed` the Base2 of the levitate effect of the last
// slot (in slot order) that newly holds a levitate spell. `levitate_base2` gives a spell's first SPA 57 Base2, or
// nothing for a spell without one.
inline constexpr std::uint32_t kNoBuffSpell = 0xffffffff; // an empty slot (game::kNoSpell)
struct BuffLevitationChange {
    bool changed = false;
    bool faded = false;
    std::optional<std::int32_t> landed;
};
BuffLevitationChange buff_levitation_change(std::span<const std::uint32_t> before, std::span<const std::uint32_t> after,
                                            const std::function<std::optional<std::int32_t>(std::uint32_t)>& levitate_base2);

// PlayerClient::DoSwimJump @0x58c940 with PlayerClient_CanSwimJumpNow
// @0x59fb60: when NextSwim is before now, the next swim jump waits 1.5 s;
// with `jump` and a clear surface (PlayerClient_IsSwimSurfaceClear, asked
// only then) SpeedZ += SwimStrength * 3 and the spawn is pushed 1.0 along
// its heading.
inline bool can_swim_jump_now(std::uint32_t next_swim, std::uint32_t now) { return next_swim < now; }
struct SwimJump {
    std::uint32_t next_swim = 0;
    float speed_z = 0.0f;
    bool surface_probed = false; // IsSwimSurfaceClear was asked
    bool jumped = false;         // SpeedZ raised and PushAlongHeading(1.0)
};
SwimJump do_swim_jump(bool jump, bool surface_clear, float swim_strength, float speed_z, std::uint32_t now,
                      std::uint32_t next_swim, const Fpu& fpu = Fpu{});

// PlayerClient_IsSwimSurfaceClear @0x506bb0 (checked against the original's vectors): the
// point it probes, one AvatarHeight (+0x138) ahead of the spawn along its Heading (+0x80) and 1 below its z: the
// original's y + round(table cos(heading) x height), table sin(heading) x height + x and z - 1, each stored as a
// float (in world coordinates: x gets the cosine). `spawn` is the spawn's position (its z: the feet plus
// AvatarHeight).
glm::vec3 swim_probe_point(const glm::vec3& spawn, float heading, float avatar_height, const Fpu& fpu = Fpu{});
// Then: clear unless the scene graph's +0x4c (IsPointInWorld) accepts the point and 0x48c890 (scene graph +0x1c,
// the region lookup without a prefix) gives a region type whose low byte is not 0 there. The type is asked only for
// a point in the world.
inline bool swim_surface_clear(bool in_world, std::uint8_t region_type) { return !in_world || region_type == 0; }

// ----------------------------------------------------------------- the jump

// What CEverQuest::DoMainLoop's jump (@0x53e84e..0x53edaf) asks of the controlled player, in the order it asks.
class JumpQueries {
public:
    virtual ~JumpQueries() = default;
    virtual std::int32_t race() = 0;              // PlayerClient vf +0x88 (the race shown): 0x8e and 0x8f never jump
    virtual bool mount_race() = 0;                // PlayerBase_IsMountRace @0x8cf970
    virtual std::uint8_t stand_state() = 0;       // +0x35c
    virtual void change_position(std::uint8_t state) = 0; // PlayerZoneClient::ChangePosition @0x5a0bc0 (may refuse)
    // With an actor (+0x1024): 0x406740, min(500, the crouch animation's length + 1) (animation 0x18 on
    // old-style models, 0x7a on new-style ones); 500 without one.
    virtual std::uint32_t crouch_window() = 0;
    virtual bool feet_on_ground() = 0;            // PlayerBase::MyFeetAreOnGround @0x8cfca0
    virtual double calculate_weight() = 0;        // EQ_Character::CalculateWeight (asked for the local player only)
    virtual void push_along_heading(float amount) = 0; // PlayerZoneClient::PushAlongHeading @0x506510
    virtual void mount_jump() = 0;                // a mount race: its jump animation (vf +0xc(0x14, ...)) and 0x7177
    virtual void send_jump() = 0;                 // OP_Jump (sent only while connected)
    virtual std::int32_t max_endurance() = 0;     // Max_Endurance(1), CharacterZoneClient virtual +0x28
    virtual void spend_endurance(std::int32_t cost) = 0; // PcBase +0x3390 -= cost
};

// The globals and spawn fields the jump reads and writes.
struct JumpState {
    std::uint32_t jump_started = 0; // 0xddf714: when JUMP went down (ProcessControls), 0 none
    std::uint32_t jump_ended = 0;   // 0xddf718: when the last jump left the ground
    std::uint8_t auto_duck = 0;     // 0xddf675: CanIFitHere's duck under a low ceiling
    bool local_dirty = false;       // 0xddfd59 / 0xddfd58: send the position (the local / another controlled spawn)
    bool other_dirty = false;
};
struct JumpSpawn {
    float speed_z = 0.0f;       // +0x78
    float speed_run = 0.0f;     // +0x7c
    float jump_strength = kJumpStrength; // +0x10
    std::uint8_t body_wet = 0;  // +0xa2
    bool sneaking = false;      // +0x264 Sneak: no push
    bool local = true;          // pinstControlledPlayer == pinstLocalPlayer
};

// CEverQuest::DoMainLoop's jump, each frame after ProcessControls, for the controlled player. A pending jump
// (jump_started) out of water within the crouch window: moving forward (SpeedRun > 0) jumps at once from the ground
// (SpeedZ += JumpStrength x 1.65 x CalculateWeight, a push of 1 along the heading unless sneaking); standing still
// crouches (ChangePosition 'o'; a jump moving back, or a spawn that could not crouch, is dropped); in water SpeedZ
// += 0.5 per frame. When the window ends a crouched player stands and jumps without the push; a mount race plays
// its own jump. A jump sends OP_Jump and costs min(50, Max_Endurance / 10) endurance.
void main_loop_jump(JumpState& state, JumpSpawn& spawn, JumpQueries& q, std::uint32_t now, const Fpu& fpu = Fpu{});

// 0x406740, the jump's crouch window: the actor definition's duck track (row 0x18 on old-style models, slot 0)
// gives its length (track vf0, truncated with a chop control word), plus 1, at most 500; 500 without the track.
std::uint32_t jump_crouch_window(std::optional<double> duck_track_length);
// The clip 0x406740 reads: by the actor's HasNewStyleHierarchicalModel (vtable +0xbc), row 0x7a - 0x4f (CRCH)
// of a new-style table or row 0x18 (L08) of a classic one, slot 0 (column 0, BA; variant 1), the track
// FindAnimationTrackForModel @0x406830 binds as "CRCH_BA_1".
std::string_view jump_duck_clip(bool new_style_hierarchy);

// ------------------------------------------------------------- air supply

// PcClient::MaxAirSupply @0x581da0: (STA - 30) * SPA 246 (100 when none) /
// 100, at least 10 and at most the SPA value; Iksar 127, Froglok 256.
std::int32_t max_air_supply(std::int32_t stamina, std::int32_t spa246, std::uint32_t race);
// EQ_Character::CanIBreathe @0x448b00: true unless the head is in a liquid
// (HeadWet > 4) or the zone has no air, for races other than 74, 24, 35
// (water breathers) without SPA 14.
bool can_breathe(std::uint8_t head_wet, bool no_air_zone, std::uint32_t race, std::int32_t spa14);
// PcClient::ProcessAirSupply @0x584ac0 (once a second, for a player spawn): breathing refills AirSupply (PcBase
// +0x2084, from the profile at zone-in) to MaxAirSupply; otherwise it counts down, and at 1 or less it becomes 0 and
// drowning does Max_HP x 0.125 (times a double, truncated) damage, type 0xfb, when that is above 0.
struct AirSupply {
    std::int32_t air = 0;
    std::int32_t drowning_damage = 0; // TakeEnvironmentalDamage when > 0
};
AirSupply process_air_supply(std::int32_t air, bool can_breathe, std::int32_t max_air, std::int32_t max_hp,
                             const Fpu& fpu = Fpu{});

// ----------------------------------------------------------- fall damage

struct FallDamageInput {
    float fall_speed = 0.0f;           // |SpeedZ + 2.5| at landing
    std::int32_t carried = 0;          // CurrWeight
    std::int32_t heroic_agi_bonus = 0; // CharacterZoneClient_GetHeroicAGIBonus
    std::int32_t safe_fall_adjusted = 0; // GetAdjustedSkill(39)
    std::int32_t safe_fall_base = 0;   // profile +0x313c (the profile's Safe Fall skill)
    std::int32_t spa228 = 0;           // TotalEffect(228): Safe Fall effectiveness
    bool mounted = false;
    bool no_fall_damage_zone = false;  // 0xddfc6e
};
struct FallDamage {
    std::int32_t damage = 0;   // reported with OP_EnvDamage when > 0
    bool safe_fall_used = false; // OP_SafeFallSuccess sent
};
// CharacterZoneClient_ApplyFallDamage @0x45ac60 (not counting its first
// call after zone-in, which only sets the flag and returns 0: the
// controller handles that).
FallDamage apply_fall_damage(const FallDamageInput& in, const Fpu& fpu = Fpu{});

// PlayerZoneClient::IHaveFallen @0x58cd20: ApplyFallDamage runs only for a
// spawn with a PcClient; a player spawn (type 0) then plays sound 0x73 when
// it returned 1 or more, else 0x72 (nullopt: no sound).
std::optional<std::int32_t> fall_sound(bool has_pc, std::uint8_t spawn_type, std::int32_t damage_result);

// A float copied through the FPU: the value a float becomes when the original passes it
// through the FPU (IHaveFallen's damage argument, SetAccel's immediate
// target): a signalling NaN comes out quiet.
float through_fpu(float v);

// LocalPlayer_ApplyEnvironmentRegionDamage @0x459860, once a second: lava
// (BodyWet 3 or 7) does the zone's lava damage (0xddfc60) less FR / 3, at
// least the zone's minimum (0xddfc64); in zone 200 (the spawn's Zone +0x210,
// masked with 0x7fff above 1000) with DR (+0x4c8) instead and type 0xf4, in
// zone 0x116 FR / 5. Freezing water (4 or 8) does 50 - CR / 3, at least 10,
// type 0xf6. Others nothing.
struct RegionDamage {
    std::int32_t damage = 0;
    std::uint8_t type = 0;
};
RegionDamage region_damage(std::uint8_t body_wet, std::uint32_t zone, std::int32_t fire_resist,
                           std::int32_t cold_resist, std::int32_t disease_resist, std::int32_t zone_lava_damage,
                           std::int32_t zone_min_lava_damage);

// ---------------------------------------------------------- the controller

// Keys and toggles for one frame (the original's command states).
struct ControlInput {
    bool forward = false, back = false;       // FORWARD / BACK (or auto run bits)
    bool turn_left = false, turn_right = false;
    bool strafe_left = false, strafe_right = false;
    bool jump = false;                        // JUMP went down this frame (0xde0d8c)
    bool mouse_look = false;                  // __MouseLook: turn keys strafe
    bool move_down = false;                   // CMD_MOVE_DOWN (cmd 449, 0xde148c) held
    bool both_buttons = false;                // 0xddf673: both mouse buttons down (moves forward)
    std::int32_t camera_index = 0;            // CDisplay::cameraType (0 first person, 6 the wheel-follow camera)
    // The active camera's fields the camera keys move (game/cameras.h), and its virtual +0x2c (the chase and user
    // cameras: the zoom and camera turn keys act).
    ControlCamera camera;
    bool camera_active = false;
    bool look_spring = false;                 // 0xde0bec (Options LookSpring): the pitch eases back to level
    bool forward_sits = false;                // 0xde0d5b (Defaults AllowAutoStand): forward stands up / ends loot
    bool loot_window_open = false;            // CLootWnd active
    bool auto_duck = false;                   // byte 0xddf675: CanIFitHere's duck under a low ceiling
};

// What the controller needs about the character each frame.
struct CharacterInput {
    float run_speed = 0.7f;     // +0x350 (the spawn's runspeed)
    float walk_speed = 0.46f;   // +0x330 MyWalkSpeed (the spawn's walkspeed)
    float sneak_speed = 0.5f;   // +0x11c8 (sneak_speed())
    float speed_multiplier = 0.0f; // from eqspa_movement_rate (capped in update())
    std::int32_t spa290 = 0;    // TotalEffect(290): the speed cap raise
    float weight_factor = 1.0f; // calculate_weight()
    std::int32_t swimming_skill = 0; // GetAdjustedSkill(50)
    std::int32_t endurance = 0, max_endurance = 0;
    std::uint8_t run_walk = 1;  // __RunWalkState: 0 walk, 1 run
    bool sneaking = false;      // PlayerZoneClient Sneak
    bool frozen = false;        // StandState 102, stunned, or sitting: no movement
    bool sitting = false;       // StandState 110 (ProcessControls' forward key stands it up)
    bool looting = false;       // StandState 105 (kneeling at a corpse: no movement)
    bool lying = false;         // StandState 115 (feigning death): no movement; /sit or /stand gets up
    bool dead = false;
    bool gm_flag = false;       // 0xddf654 / 0xddf790: no endurance check
    bool interface_locked = false; // /safelock (0xe15dac): ProcessControls does nothing
    std::int32_t race = 0;      // PlayerClient vf88, the race shown (0x8e / 0x8f never jump)
    std::uint32_t crouch_window = kJumpWindowMs; // 0x406740 (jump_crouch_window) on the controlled spawn's actor
    // The local player rides: the body is its mount (pinstControlledPlayer), which has the player as rider. The
    // speeds, the modifier and the race above are then the mount's, the rest the rider's.
    bool riding = false;
    bool remote_control = false; // an Eye: nonlocal controls without a rider/mount partner
    // The rider's PcClient virtual +0x80 (@0x57d190: the OP_RestState combat byte, +0x31bc), which slows a
    // ridden mount's SetAccel ramp.
    bool in_combat = false;
    float avatar_height = 0.0f; // the controlled spawn's AvatarHeight (+0x138): the zoom keys' nearest distance
};

// What happened this frame for the caller to act on.
struct ControlEvents {
    bool jumped = false;            // send OP_Jump
    std::int32_t endurance_cost = 0; // subtract from the local endurance (DoMainLoop @0x53ed48)
    std::uint32_t message = 0;      // eqstr to print (too fatigued)
    bool crouch_changed = false;    // the jump crouches and stands (StandState 111 / 100)
    bool swim_jumped = false;
    bool turn_off_auto_follow = false; // a turn, strafe or back key ended /follow (EQPlayer::TurnOffAutoFollow)
    bool sit_toggle = false;        // the forward key while sitting (AllowAutoStand): CEverQuest::Sit
    bool close_loot = false;        // the forward key while looting (AllowAutoStand): the loot window closes
    bool auto_duck_cleared = false; // the jump cleared byte 0xddf675 (the spawn could not crouch)
    bool mount_jump = false;        // a mount race's jump: its animation and OP_Animation (report_mount_jump)
    // ProcessControls' DoAttack(13 / 14, 0, pinstTarget) calls, in order: the caller runs them (melee.h).
    std::vector<std::int32_t> attacks;
    // The camera keys moved the active camera (its setters): its fields after, and whether the pitch setter ran
    // (the user cameras' also clears +0x44).
    std::optional<ControlCamera> camera;
    bool camera_pitch_set = false;
};

struct LandingEvents {
    bool landed = false;
    bool fell = false;              // IHaveFallen was called
    FallDamage damage;              // when fell (zero for the first fall after zone-in)
    bool land_sound = false;        // sound 0x72 (0x73 after damage)
};

// The local player's movement as CEverQuest::ProcessControls and the jump
// part of DoMainLoop run it each frame, then PhysicsObject::Step and the
// landing check. Owns the physics body.
class MovementController {
public:
    PhysicsBody body;
    bool crouching = false;      // StandState 111 (the standing jump's crouch)
    // PlayerClient_IsSwimSurfaceClear for the current position (set by the
    // caller before controls(); the swim jump needs it).
    bool swim_surface_clear = false;
    // The spawn's JumpStrength (+0x10) and SwimStrength (+0x14), SetHeights' (game/spawn_heights.h).
    float jump_strength = kJumpStrength;
    float swim_strength = kSwimStrength;

    // Zone-in: position, heading; clears the jump timers and the first-fall flag.
    void reset(const glm::vec3& feet, float heading);

    // A movement key pressed or released (execute_movement_command, controls.h):
    // the held keys, autorun and the ramps persist between frames as the
    // original's globals do.
    bool command(int command, bool down, bool accept, std::uint8_t autorun_bits = 0) {
        return execute_movement_command(controls_, command, down, accept, autorun_bits);
    }
    const ControlKeys& held_keys() const { return controls_.keys; }
    std::uint8_t autorun() const { return controls_.autorun; }
    // FollowPlayerAI writes the autorun variable (0xddf720) directly: 1 to move toward the followed spawn, 0 to stop.
    void set_autorun(std::uint8_t bits) { controls_.autorun = bits; }

    // ProcessControls and the jump (call once per frame before step()). The
    // keys in `in` are pressed on top of the held ones for this frame (test
    // and auto-walk drivers).
    ControlEvents controls(const ControlInput& in, const CharacterInput& ch, std::uint32_t now_ms);

    // The strafe push for this frame (PhysicsObject vf14 @0x59dd20).
    glm::vec2 strafe_push(const CharacterInput& ch) const;

    // PhysicsObject::Step; returns the displacement (see physics.h).
    glm::vec3 step(const Environment& env, float elapsed_ms, const CharacterInput& ch, float hover_floor);

    // PlayerZoneClient_LandingAndFallCheck @0x506e20 after the caller placed
    // the body: when the feet are below `floor` the body lands there; a fall
    // at SpeedZ <= -2.5 out of water calls IHaveFallen. `fall` carries the
    // character values ApplyFallDamage reads (fall_speed is filled in).
    LandingEvents land(float floor, FallDamageInput fall);

    // The swim jump (DoSwimJump @0x58c940): `surface_clear` is
    // PlayerClient_IsSwimSurfaceClear for the current position.
    bool swim_jump(bool jump, bool surface_clear, std::uint32_t now_ms);

    std::int32_t turn_count_left() const { return controls_.turn_left_count; }
    bool jump_pending() const { return controls_.jump_started != 0; } // 0xddf714
    std::uint32_t jump_ended() const { return controls_.jump_ended; }   // 0xddf718: when the last jump left the ground
    std::uint8_t strafe_mode() const { return controls_.strafing; }
    std::uint8_t stand_state() const { return controls_.controlled.stand_state; } // +0x35c
    std::uint32_t next_swim() const { return next_swim_; }                         // +0x42c NextSwim
    // ProcessControls' state, for the melee loop: __Attack, the local player's presence and the two swing-ready
    // flags (DoPassageOfTime sets them before controls(); ProcessControls clears one per DoAttack it asks for).
    ControlsState& controls_state() { return controls_; }

private:
    // ProcessControls' globals (game/mechanics/controls.h): the turn and look
    // counts, the strafe flag and scale, the jump timer (jump_started
    // 0xddf714, a jump waiting to happen; jump_ended 0xddf718), the last sent
    // heading and speed.
    ControlsState controls_;
    std::uint32_t next_swim_ = 0;    // +0x42c NextSwim
    bool fell_once_ = false;         // PlayerBase +0x16c: the first fall after zone-in does nothing
    // g_LocalPhysicsTimeFactor (0xde0a68, PhysicsObject::vf18): the controlled spawn's last step's time scale
    // (elapsed ms x 0.02), which SetAccel's ramp on a ridden mount multiplies in.
    float step_scale_ = 1.0f;
};

} // namespace hailstone::game::mechanics
