#pragma once

// Which movement animation a character plays, and how fast.
//
// The original has two animation objects:
//  - new-style models (NpcManager_IsNewStyleModelRace: EQG races 197..732,
//    Drakkin; vtable 0x9d70a0): the update 0x58af10 sends a still player
//    to PlayerAnimation_ChooseIdleAnimation and a moving one to
//    PlayerAnimation_SelectMovementAnim @0x5888a0. Codes are 0x4f + the
//    track of the tag table at 0xaae3d8 (WALK 0x68, NRUN 0x6a, ...);
//  - old-style (classic WLD) models (vtable 0x9ca640): 0x4afe60 picks an
//    old-numbered animation (0x11 L01 walk, 0x12 L02 run, 0x20 P01 stand,
//    ...) and 0x4afc90 scales its playback rate per race on the way to
//    the actor.
//
// Both follow the original exactly: every float step follows the original's x87
// instructions at the FPU's precision (game/mechanics/fpu.h; in game 24-bit).
// select_movement_anim and old_model_anim_speed are checked against the
// original's vectors SelectMovementAnim / OldModelAnimSpeed
// (tests/anim_vectors_test.cpp); select_old_style_anim has no vectors.

#include <cstdint>
#include <functional>
#include <string_view>

#include "game/animation_types.h"
#include "game/mechanics/fpu.h"

namespace hailstone::game::mechanics {

// The animation-start call both objects end in (their slot 0xc: anim,
// play_once, reverse, speed, replay, 0xff, 0). The flags are the low bytes
// the original pushes; a choice of 0xfe ("no animation", the dword at
// 0xa1e344) starts nothing.
inline constexpr std::uint8_t kNoAnimation = 0xfe;
// The new-style sequence codes (0x4f..0xb6): the first is XXXX, the last SATK.
inline constexpr std::uint8_t kFirstNewStyleCode = 0x4f, kLastNewStyleCode = 0xb6;
struct AnimStart {
    bool played = false;    // false: nothing started (the choice was 0xfe, or the path starts nothing)
    std::uint8_t anim = kNoAnimation;
    bool play_once = false; // argument 2 (unused by the new-style player 0x58afa0)
    bool reverse = false;   // argument 3: play backward
    float speed = 1.0f;     // argument 4: the playback rate
    bool replay = false;    // argument 5: 0x58ac90's layer flag
};

// ---- new-style models

// New-style animation codes (0x4f + track; tags from 0xaae3d8).
enum NewStyleAnim : std::uint8_t {
    kAnimWalk = 0x68, kAnimBackWalk = 0x69, kAnimRun = 0x6a, kAnimStand = 0x6e, kAnimIdle = 0x6f,
    kAnimSit = 0x76, kAnimSitIdle = 0x77, kAnimStandUp = 0x78, kAnimSitDown = 0x79, kAnimJumpRun = 0x7b,
    kAnimJumpStand = 0x7c, kAnimDuckWalk = 0x7d, kAnimClimb = 0x7e, kAnimTread = 0x7f, kAnimSwim = 0x80,
    kAnimFall = 0x97,
};

// What PlayerAnimation_SelectMovementAnim reads (PlayerBase offsets).
struct MovementAnimInput {
    // The motion source: the mount's when GetMount() answers one, else the player's.
    bool mounted = false;
    float speed_y = 0.0f, speed_x = 0.0f, speed_z = 0.0f; // +0x70 / +0x74 / +0x78, units per tick
    float speed_run = 0.0f;                               // +0x7c: signed forward speed
    // The player's own fields.
    std::int32_t colliding_type = 0; // +0x24: 6 climbing
    std::uint8_t feet_wet = 0;       // +0xa1
    std::uint8_t body_wet = 0;       // +0xa2
    std::uint8_t spawn_type = 0;     // +0x125: 1 NPC
    std::uint8_t stand_state = 100;  // +0x35c: 0x6f ducking
    std::int32_t animation = 0;      // +0x10c0: the animation playing
    float jump_strength = 1.35f;     // +0x10 (SetHeights)
    float walk_speed = 0.0f;         // MyWalkSpeed +0x330
    float run_speed = 0.0f;          // RunSpeed +0x350
    float model_scale = 1.0f;        // +0x31c (SetHeights)
    float anim_scale = 1.0f;         // +0x228 (SetHeights)
    bool old_model_scale = false;    // dword 0xddfba4 nonzero: the stride is not scaled by 1/1.3
    // Callee answers.
    bool local_player = false;       // the player is pinstLocalPlayer
    bool spell_book_active = false;  // CXWnd::IsActive on pinstCSpellBookWnd (asked for the local player)
    bool mount_race = false;         // PlayerBase_IsMountRace
    bool new_style_race = false;     // NpcManager_IsNewStyleModelRace(race) (asked for an NPC on land)
};

struct MovementAnimResult {
    AnimStart start;
    bool spell_book_slot_0x94 = false; // the open spell book's slot 0x94 ran
    // Which callees were asked (the vectors list them).
    bool asked_spell_book = false, asked_mount_race = false, asked_new_style_race = false;
};

// PlayerAnimation_SelectMovementAnim @0x5888a0. (It also clears the player's
// LastTimeStoodStill +0x360.)
MovementAnimResult select_movement_anim(const MovementAnimInput& in, const Fpu& fpu = Fpu{});

// 0x58af10's still test: (vy*vy + vx*vx) + vz*vz below 0.01 (and no
// mount): the player then goes to ChooseIdleAnimation instead.
bool movement_still(float speed_y, float speed_x, float speed_z, const Fpu& fpu = Fpu{});

// What PlayerAnimation_ChooseIdleAnimation reads (PlayerBase offsets).
struct IdleAnimInput {
    bool turning = false;           // the update's argument: sub_00509340 passes 1 while +0x8c is not 0 or
                                    // the heading differs from the actor's
    std::int32_t state_1198 = 0;    // +0x1198 (1: plain STND; not identified)
    std::uint32_t player_state = 0; // +0x14c PlayerState (0x20 stunned: STUN)
    std::uint32_t display_time = 0; // CDisplay +0x154
    std::uint32_t still_since = 0;  // +0x360 LastTimeStoodStill (the last fidget roll; SelectMovementAnim clears it)
    std::uint8_t stand_state = 100; // +0x35c: 0x6e sitting, 0x6f ducking
    std::uint8_t body_wet = 0;      // +0xa2
    std::int32_t animation = 0;     // +0x10c0: the animation playing
    float speed_heading = 0.0f;     // +0x8c
    std::uint32_t owner_38c = 0;    // +0x38c (set: a fidget needs a roll above 99)
    std::uint8_t byte_1229 = 0;     // +0x1229 (set: never fidgets)
    bool attacking = false;         // the global auto attack switch (0xe635aa)
    bool local_first_person = false; // the local player with CDisplay::cameraType 0
    std::function<std::int32_t()> random_percent; // RandomPercentRoll (1..100)
};
struct IdleAnimResult {
    AnimStart start;
    bool set_still_since = false; // +0x360 was written with display_time (a fidget roll)
};
// PlayerAnimation_ChooseIdleAnimation @0x58a1e0: a still new-style player's
// animation. Stunned (0x20 in +0x14c): STUN; treading water (BodyWet >= 5, or
// TWTR/SWIM playing): TWTR; turning: LTRN (+0x8c > 0) or TURN, CWLK while
// ducking; ducking: CRCH at rate 2; every 5 s (from +0x360) a roll: IDLE
// (the fidget) above 75 (99 with +0x38c) while standing, not attacking and
// STND/STDA/NSIT playing (the object's slot 0x38), else STND. Sitting turns
// STND/IDLE into NSIT/SIDL (nothing for the local player in first person).
IdleAnimResult choose_idle_animation(const IdleAnimInput& in);
// The new-style object's slot 0x38 (0x588cc0): STND, STDA or NSIT.
bool new_style_can_idle(std::int32_t animation);

// ---- attacks

// PlayerZoneClient_PlayAttackAnimation @0x5a19d0's arguments and the spawn
// fields its two choosers read.
struct AttackAnimInput {
    std::uint32_t skill = 0;     // a1: the attack's skill (Send_OP_Animation's param 3, the packet's skill)
    std::uint32_t item_type = 0; // a2: the weapon's or the missile's item type (9: a spell)
    std::int32_t range = 0;      // a3: the missile's range (the range byte; 0 for a melee swing)
    std::uint8_t slot = 0;       // a5's low byte: 13 primary, 14 secondary, 7 a missile's launch
    std::uint32_t race = 0;      // the spawn's vf88 (0x80 Iksar: bash and dragon punch differ)
    std::uint8_t class_id = 0;   // +0xeb8 (7 monk: intimidation is a round kick)
    std::uint8_t body_wet = 0;   // +0xa2 (new-style models: nonzero swims, WATK)
    bool new_style = false;      // NpcManager_IsNewStyleModelRace(race)
};
// The animation (old-style number below 0x4f, else a new-style code); the
// speed it writes is always 1. Old-style models (0x59f750): item type 9
// T06 (0x2c); with a range arrows (0x1b) C09, 0x1c C10, others C05; else by
// skill: 1H blunt / slashing and frenzy C05, 2H C03, bash C07, dragon punch
// and tiger claw T08, eagle strike T09, flying kick T07, hand to hand C08,
// kick and intimidation C01 (C11 for a monk's intimidation), round kick C11,
// the rest C02 (C04 with item type 0x23, 2H piercing); the secondary hand
// (slot 14) turns all of them into C06 but a monk's intimidation; an Iksar's
// C07 is C06 and dragon punch C07. New-style models (0x59f8d0): MCST
// for item type 9; with a range BATK for arrows, WATK for 0x1c, BLPR (WATK
// in water); in water WATK; by skill SLPR / BLPR (primary) or SLSC / BLSC,
// SL2H, BASH, MHA1, MHA2, MKCK, PATK / SATK, KICK, RKCK, the rest STPR (ST2H
// for 2H piercing); slot 14 SLSC; an Iksar's BASH is SLSC and dragon punch
// BASH.
std::uint8_t attack_animation(const AttackAnimInput& in);

// The sequence table at 0xaae3d8 (codes 0x4f..0xb6). The tag of a code
// ("WALK"), or empty (0x4f is "XXXX", no sequence).
std::string_view new_style_tag(std::uint8_t code);
// The old-style number the new-to-old map (0xaaf428) gives a code (WALK 0x68
// -> 0x11; 0 for the sheathe and stun codes), or 0.
std::uint8_t new_to_old_anim(std::uint8_t code);
// The code the old-to-new map (0xaaf5c8, the new-style object's slot 0x14)
// gives an old-style number (C05 5 -> SLPR 0x50; 0x4f for none); a code of
// 0x4f or above is returned as it is.
std::uint8_t old_to_new_anim(std::uint8_t old);
// A sequence's fallback (-8: SLSC -> SLPR -> STND; 0x4f ends the chain).
std::uint8_t new_style_fallback(std::uint8_t code);
// A sequence's flags: +0x19 (the attacks) and +0x1a.
bool new_style_attack_flag(std::uint8_t code);
bool new_style_mount_fallback_flag(std::uint8_t code);
// A sequence's layer kind (-0xc): 1 the attacks, sheathing and casting, 2
// the walk / run / turn sequences, 0 the rest.
std::uint8_t new_style_layer_kind(std::uint8_t code);
// A sequence's priority (-4; 0x58ac90 keeps a looping sequence of higher
// priority playing). 0 for codes outside the table.
std::int32_t new_style_priority(std::uint8_t code);
// The cross-fade the bone groups use when `to` replaces `from`:
// (to.blend_in + from.blend_out) / 2 ms from the table (100 ms each, 250 ms
// into STND/IDLE).
float new_style_blend_ms(std::uint8_t from, std::uint8_t to);

// ---- old-style (classic) models

// The animation object's vf24 on +0x10c0: a jump. Old-style objects (PlayerAnimationBase @0x4afd90) L03 / L04
// (0x13 / 0x14), new-style ones (@0x588cf0) JMPA / JMPU (0x7b / 0x7c).
bool is_jump_animation(std::uint8_t anim, bool new_style);
// The classic clip code of an old-style number ("L01"): the names table at
// 0xb787a0 (C01..C11, D01..D05, L01..L12, O01..O03, P01..P09, S01..S29,
// T01..T09), or empty for 0 and numbers from 0x4f.
std::string_view old_anim_clip(std::uint8_t old);

// What 0x4afe60 reads (PlayerBase offsets).
struct OldStyleAnimInput {
    float speed_y = 0.0f, speed_x = 0.0f, speed_z = 0.0f; // +0x70 / +0x74 / +0x78 (the player's own)
    float speed_run = 0.0f;       // +0x7c
    float speed_heading = 0.0f;   // +0x8c
    float y = 0.0f;               // +0x64
    bool has_mount = false;       // EQPlayer::GetMount
    float mount_speed_y = 0.0f, mount_speed_x = 0.0f;
    std::int32_t state_1198 = 0;  // +0x1198 (1: the ship/zone-84 branch; not identified)
    std::uint32_t display_time = 0; // CDisplay +0x154 (the frame clock, ms)
    std::uint32_t still_since = 0;  // +0x360 LastTimeStoodStill (written back, see result)
    std::uint8_t stand_state = 100; // +0x35c
    std::int32_t animation = 0;     // +0x10c0
    std::uint8_t feet_wet = 0, body_wet = 0; // +0xa1 / +0xa2
    std::int32_t race = 0;          // the player's slot 0x88
    bool flag = false;              // the update's argument (low byte)
    std::uint8_t byte_163 = 0;      // +0x163 (moving: plays 0x15)
    std::int32_t colliding_type = 0; // +0x24
    float jump_strength = 1.35f;    // +0x10
    std::uint32_t dword_180 = 0;    // +0x180 (falling only when 0)
    std::int32_t zone_id = 0;       // unk_CurrentZoneID
    float walk_speed = 0.0f;        // the player's slot 0x64 (MyWalkSpeed)
    std::uint8_t spawn_type = 0;    // +0x125
    std::uint32_t owner_38c = 0;    // +0x38c
    std::uint8_t byte_1229 = 0;     // +0x1229
    bool attacking = false;         // the global auto attack switch (0xe635aa)
    bool local_player = false, spell_book_active = false;
    // Asked only when they matter.
    std::function<std::int32_t()> random_percent;            // RandomPercentRoll (1..100)
    std::function<bool(std::int32_t animation)> can_idle;    // the object's slot 0x38(+0x10c0)
};

struct OldStyleAnimResult {
    AnimStart start;
    bool idle_sounds = false;         // 0x58a480 ran
    bool spell_book_slot_0x94 = false;
    bool set_still_since = false;     // +0x360 was written with `still_since`
    std::uint32_t still_since = 0;
};

// 0x4afe60: the old-style object's per-update choice.
OldStyleAnimResult select_old_style_anim(const OldStyleAnimInput& in, const Fpu& fpu = Fpu{});

// 0x4afc90: an old-style model's playback rate for old
// animation `anim`: doubled for 0x14, 0x13 (not race 8 males), 0x10 with the
// animation set's new-model flag (+0x34), race 216, races 209..212 while
// CDisplay +0x129 is set, and with the flag for gender 1 animations 2 and 4
// (and race 2's 5); race 330's 0x14 at 0.75. The x87 result, unrounded.
double old_model_anim_speed(std::int32_t anim, float speed, std::int32_t race, std::int32_t gender, bool new_model,
                            bool display_129, const Fpu& fpu = Fpu{});

// 0x4b15b0's pre-processing of the packet's speed before 0x4afc90: the mount races (0x8cf970's list, is_mount_race)
// run at 0.4 and their other animations at 0.5, races 42 and 120's neutral gender (2) non-walk at 0.5, race 330 at
// 0.65, and a styled model's stand / idle at 0.5 and social (an S clip) at 0.75 when the packet asked 1.0.
float old_action_speed(std::uint8_t anim, float speed, std::int32_t race, std::int32_t gender, bool new_model);

} // namespace hailstone::game::mechanics
