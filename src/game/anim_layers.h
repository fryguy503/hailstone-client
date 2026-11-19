#pragma once

// The two bone groups a character actor animates (ActorClient +0x174 /
// +0x170, the actor's GetBoneGroup(0) / GetBoneGroup(1)), and which of them
// an animation plays on.
//
// The actor's bones are split once, when its animations are set up
// (0x4090f0 for classic skeletons, 0x409260 for new-style ones; the
// actor's HasNewStyleHierarchicalModel, vtable +0xbc, picks one): each bone's
// tag goes to a callback through EnumerateBones (CHierarchicalActor, vtable
// +0x194 @0x10040f80) and the bone joins the group it answers. Group 0 holds
// the chest, neck, head and arms (the upper body), group 1 the rest (root,
// pelvis, legs, and the points the callback does not name). Each group is a
// CBoneGroup with its own two-track cross-fade (BlendToAnimation
// @0x1003fcc0), so an action can play over the arms while the legs keep
// walking.
//
// Which group plays what:
//  - classic models (0x4b15b0): the animation asked ("target") goes to
//    group 0 and the "queued" one to group 1; they are the same animation
//    unless a mounted rider's own animation is queued (the legs sit, the arms
//    act), or a moving humanoid starts an action of the table at 0xdced38
//    (the legs keep moving). While an action of that table plays on group 0
//    and has not finished, a walk or run (table 0xdced88) asked for a
//    humanoid starts on group 1 only; other requests wait.
//  - new-style models (0x58afa0): a sequence of layer kind 1 (attacks,
//    casting) plays on group 0 alone while group 1 plays a kind 2 sequence
//    (walk, run, turn) or the player is mounted; otherwise on both groups,
//    each group taking it by 0x58ac90's mode (new_style_layer_mode).

#include <cstdint>
#include <string_view>

namespace hailstone::game {

inline constexpr std::uint8_t kUpperBones = 0; // GetBoneGroup(0): ActorClient +0x174
inline constexpr std::uint8_t kLowerBones = 1; // GetBoneGroup(1): ActorClient +0x170

// The classic layers' cross-fade (0x9c3988): 100 ms, whatever the animation.
inline constexpr float kClassicBlendMs = 100.0f;

// 0x408f80, the classic skeleton's callback: past the tag's three-letter
// model code, "CHCHEST_" is group 1 and FA, HE, FI, FO, BI, NE, HI, CH are
// group 0; anything else is group 1.
std::uint8_t classic_bone_group(std::string_view tag);
// 0x409050, the new-style (EQG) skeleton's callback: tags starting with
// CHEST (but CHEST_CHEST01 itself), HEAD, ARM or NECK are group 0; anything
// else is group 1.
std::uint8_t new_style_bone_group(std::string_view tag);

// The byte tables 0x4afb70 fills for 0x4b15b0 (old-style numbers
// below 0x4f):
//  - 0xdced38: actions that play on group 0 alone while a humanoid moves
//    (C02..C09 and 0x27..0x2c: the instruments and casting);
//  - 0xdced88: the movement animations group 1 may start under such an
//    action (L01 walk, L02 run, P01 stand, the rider animations 0x4a..0x4c
//    and 0x4e);
//  - 0xdcece8: the animations a forced request (0x4b0900's key 1) may
//    interrupt (the movement, idle and social ones; not the attacks).
bool classic_action_over_legs(std::uint8_t old);
bool classic_legs_under_action(std::uint8_t old);
bool classic_interruptible(std::uint8_t old);

// 0x4b15b0's split when a classic model starts `action` (an old-style
// number): true when it plays on group 0 alone, the legs keeping theirs. A
// mounted rider always keeps the rider animation on the legs; otherwise a
// humanoid (PlayerBase::HasProperty(1): body type 1) moving forward or back
// (+0x7c not 0) does for the actions of table 0xdced38.
bool classic_action_upper_only(std::uint8_t action, bool mounted, bool humanoid, float speed_run);
// The busy path of 0x4b15b0: group 0 still plays `playing` (an
// unfinished action) and `wanted` is asked. True when group 1 alone takes
// `wanted`: a humanoid, `wanted` in table 0xdced88 but not P01 or the rider
// animations 0x4a / 0x4e, and `playing` in table 0xdced38. Otherwise nothing
// changes until the action ends.
bool classic_legs_take_motion(std::uint8_t playing, std::uint8_t wanted, bool humanoid);

// 0x4b0900 with key 1 (0x4b15b0 for an OP_Animation:
// the classic object's slot 3 passes restart 1 and key 1): whether
// `requested` starts at once while `playing` (+0x10c0) is the animation
// playing. The blend and rider animations (0x1a, 0x48, 0x49, 0x4a, 0x4e),
// D05 / D04 (0x10, 0xf for class 0x3e), the sit / stand 0x21 and 0x2c do;
// a standing one (slot 0x38: 0x20, 0x26, 0x47, 0x4a) does not over a blend
// animation; otherwise only over an animation of table 0xdcece8 or above
// 0x4e, and never over itself. Refused, it waits for the upper body's
// group to finish its cycle (classic_queues).
bool classic_may_interrupt(std::uint8_t requested, std::uint8_t playing, bool class_0x3e = false);
// 0x4b15b0 when a request may not interrupt and the upper body has not
// played its animation through: the actions 1..0xb and 0x2d..0x2f are kept
// as the follow-up (+0x10c4), which the next stand request (P01, 0x20)
// plays once the upper body is free; the movement and posture animations
// (0x11..0x19, 0x21, 0x24, 0x4b, 0x4c) reset it to 0x20 (none). Nothing
// starts either way.
bool classic_queues(std::uint8_t anim);
bool classic_clears_queue(std::uint8_t anim);

// 0x4b0860 (read from the exe, which tests more than 0x20): true when `playing` (+0x10c0) leaves a classic model's
// stand and idle requests as they are: P01 (0x20), C01..D05 (1..0x10), the
// walk L01 (0x11), the turn P03 (0x22), T04..T09 (0x2a..0x2f), L11 (0x4b)
// and none (0xfe). After anything else (the run, a jump, a fall, a duck, a
// posture change, the instruments, a social, P08 itself) 0x4b15b0 plays
// a stand request as P08 (0x47) and an idle (O01, 0x1a) as O02 (0x48), each
// only when the model has that clip. When the stand request plays the
// follow-up instead, +0x10c4 becomes 0x20 after these, else 0x47 (so the
// stand after it is P08).
bool classic_plain_stand_after(std::uint8_t playing);

// New-style sequence fields the layer modes read (the table at 0xaae3d8):
// the hold kind (+8 from 0xaae3dc: 3 the looping movement and stand
// sequences, 1 CRMP, 0 the rest) and whether the sequence loops (+0xc, the
// EAnimationMode: 3 repeat, 1 once).
std::uint8_t new_style_hold_kind(std::uint8_t code);
bool new_style_loops(std::uint8_t code);

// What 0x58ac90 reads to choose how one bone group takes a new sequence.
struct LayerModeInput {
    bool group = true;           // the actor has this bone group
    std::uint8_t wanted = 0;     // the sequence asked (a code 0x4f..0xb6)
    std::int32_t column = 0;     // its style column
    std::uint8_t playing = 0xfe; // the group's sequence (+0x10c0 for group 0, +0x10c8 for group 1; 0xfe none)
    std::int32_t playing_column = 0;
    bool forced = false;         // the play's layer flag 2
    bool completed = false;      // the group's HasCompletedCycle (@0x1003f4b0: one length of it has passed)
    bool race_model_0x3e = false; // +0xeb8 == 0x3e: 0x91 is taken as 0x92
    bool dying_slot_0x98 = false; // the player's slot 0x98, asked when 0x92 (CRMP) is wanted
    std::uint8_t stand_state = 100; // +0x35c
    std::uint8_t spawn_type = 0;    // +0x125
};
// 0x58ac90's mode: 0 nothing, 1 queue it (the group keeps playing), 2
// start it (blend), 3 keep the playing one (only its speed is set).
std::uint8_t new_style_layer_mode(const LayerModeInput& in);

} // namespace hailstone::game
