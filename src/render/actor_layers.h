#pragma once

// A character's two bone groups (game/anim_layers.h) as the renderer plays
// them: each group is a CBoneGroup, a pair of animation tracks that
// cross-fade (CBoneGroup::BlendToAnimation @0x1003fcc0: the new track's weight
// 0 -> 1 and the old one's 1 -> 0, linear over the blend time; asked for the
// animation it already plays, it does nothing), and ActorLayers decides each
// frame what each group plays: the movement (or rider, or corpse) clip on
// both, an OP_Animation action on both or on the upper body alone.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/character.h"

namespace hailstone::render {

// One bone group's tracks.
struct GroupTrack {
    const assets::AnimationClip* clip = nullptr;
    std::uint8_t anim = 0xfe; // the animation it plays (old-style number or new-style code; 0xfe none)
    std::int32_t column = 0; // new-style sequence's style column
    float time = 0.0f;        // seconds into `clip` (counting from its start even when reversed)
    float rate = 1.0f;
    bool loop = true;
    bool reverse = false;
    float elapsed = 0.0f;     // display seconds since it started (HasCompletedCycle)
    const assets::AnimationClip* previous = nullptr;
    float previous_time = 0.0f;
    bool previous_reverse = false;
    bool previous_loop = true; // the previous clip's loop flag: a played-out one-shot holds its last frame
    float blend = 1.0f;       // the new track's weight
    float blend_seconds = 0.1f;

    // BlendToAnimation (blend_seconds > 0 on a group that plays something) or SetCurrentAnimation. The clip it
    // already plays (same direction) only takes the new rate and loop flag, unless `restart`. True when it started.
    bool start(const assets::AnimationClip* c, std::uint8_t code, float speed, bool loops, bool reversed,
               float blend_s, bool restart = false, std::int32_t style_column = 0);
    void advance(float dt);
    // CBoneGroup::HasCompletedCycle @0x1003f4b0: nothing playing, or one length / |speed| has passed since it
    // started.
    bool completed() const;
    // CBoneGroup::SyncToBoneGroup @0x1003f660: the same animation as `other` takes its position.
    void sync_to(const GroupTrack& other);
    // The clip at the end of its playback, as if it had played through: no cross-fade left and nothing to complete.
    // Reversed, that is the first source frame; at rate 0 it stays incomplete (completed() is false).
    void finish();
    // The clip time to sample (reversed clips run from their end).
    float shown_time() const;
    float shown_previous_time() const;
};

// What a spawn's groups are asked this frame.
struct LayerRequest {
    const assets::AnimationClip* base = nullptr; // the movement, posture, rider or corpse clip
    std::uint8_t base_anim = 0xfe;               // its animation number / code
    float rate = 1.0f;
    bool loop = true;
    bool reverse = false;
    bool restart = false;    // asked again from its start
    float blend_ms = 100.0f; // the cross-fade into it (classic models: 100 ms)
    bool new_style = false;  // the new-style animation object (0x58afa0) rather than the classic one
    bool corpse = false;     // dead: the base clip on the whole body, any action dropped
    // The corpse clip starts at its last frame: the spawn was already dead when this state first drew it (a fresh
    // state after a zone change, or one that came into view dead), so the death is not played again.
    bool corpse_at_end = false;
    bool mounted = false;    // a rider
    bool humanoid = false;   // PlayerBase::HasProperty(1): body type 1
    float speed_run = 0.0f;  // +0x7c
    std::uint8_t follow_up_after = 0x20; // classic: what +0x10c4 becomes when the stand request plays the follow-up
                                         // (game::SpawnAnimChoice::follow_up)
    bool stand_request = false; // classic: 0x4b15b0's stand path (P01 / P08 on the base, the follow-up +0x10c4)
    std::int32_t base_column = 0;
    std::int32_t action_column = 0;
};

// What one actor's pose reads: the lower group's clip and, when `upper_clip` is set, the upper group's own (an
// action over the legs).
struct PoseInput {
    const assets::AnimationClip* clip = nullptr;
    float time = 0.0f;
    bool loop = true;
    const assets::AnimationClip* previous = nullptr;
    float previous_time = 0.0f;
    bool previous_loop = true;
    float blend = 1.0f;
    const assets::AnimationClip* upper_clip = nullptr;
    float upper_time = 0.0f;
    bool upper_loop = true;
    const assets::AnimationClip* upper_previous = nullptr;
    float upper_previous_time = 0.0f;
    bool upper_previous_loop = true;
    float upper_blend = 1.0f;
    const std::vector<std::uint8_t>* bone_groups = nullptr; // per bone (game/anim_layers.h)
};
// The actor's bone matrices: the lower group's clip, the upper group's own when `upper_clip` is set, each
// cross-faded from its previous clip by the linear weight. A played-out one-shot holds its last frame; only a
// looping clip wraps (0x1003fcc0's old track). `previous` is the scratch buffer for the previous pose.
void pose_actor(const assets::CharacterModel& model, const PoseInput& in, std::vector<assets::BonePose>& local,
                std::vector<assets::BonePose>& previous, std::vector<glm::mat4>& world);

// /debug_animinfo's readout of one actor's pose (Hailstone's debugging aid, not the original's), a line each: the
// model and its clip donor; what each bone group plays, by the clip's code in `model` ("?" for a clip the model does
// not hold), with its time and cross-fade; and the bones the pose leaves at their rest rotation and translation (the
// points left out, which most clips do not animate). A bone at rest in a clip that moves the body is where a clip
// lent nothing for it.
std::vector<std::string> describe_pose(const assets::CharacterModel& model, const PoseInput& in);

struct ActorLayers {
    GroupTrack group[2]; // game::kUpperBones, game::kLowerBones
    const assets::AnimationClip* action = nullptr; // the OP_Animation action playing on the upper body
    std::uint8_t action_anim = 0;
    bool action_on_legs = false; // the action plays on both groups
    // Classic models: a walk or run took the legs under the action (game::classic_legs_take_motion), so the
    // animation playing (+0x10c0) is that rather than the action.
    bool legs_took_motion = false;
    // Classic models: the follow-up action (+0x10c4, 0x20 none) an OP_Animation leaves when it may not interrupt
    // the unfinished one (game::classic_queues); the next stand request plays it.
    std::uint8_t follow_up = 0x20;
    const assets::AnimationClip* follow_clip = nullptr;
    float follow_rate = 1.0f;
    // The last step was a corpse request and its clip has played through: a changed clip then shows its end too.
    bool corpse_settled = false;

    // An action starts (0x4b15b0 / 0x58afa0 for an OP_Animation): on the upper body alone or on both
    // groups (game/anim_layers.h). `requested` is the number the packet asked for (0x4b0900's first argument);
    // `chosen` is what 0x4b1210 made of it, which the queue and the final state use. False when the model's object
    // plays nothing now: a new-style rider's action that is not of layer kind 1, a group mode (0x58ac90) of 0 or 1
    // (the new-style object records a mode 1 sequence at +0x10c4 / +0x10cc and never plays it), the same sequence
    // asked again (mode 3: it keeps playing), or a classic request that may not interrupt the unfinished action
    // (kept as the follow-up). Asked for the clip a group already plays, BlendToAnimation leaves it as it is (no
    // replay).
    bool start_action(const assets::AnimationClip* clip, std::uint8_t requested, std::uint8_t chosen, float rate,
                      const LayerRequest& req);
    // 0x4b15b0's stand path: may the stand replace what the upper body plays? A corpse and an unfinished action
    // the stand may not interrupt (0x4b0900, `requested` is the packet's number) refuse; an action that may yield
    // is dropped. The caller then asks for the stand clip as the base.
    bool stand_request(std::uint8_t requested, const LayerRequest& req);
    // 0x4b15b0's stand path: P01 / P08 on both groups, looping and forward, or the follow-up +0x10c4 (step()).
    // Fills `req`'s base and returns false for a corpse or an unfinished action the stand may not interrupt. The
    // caller keeps a posture change (a sit-down / stand-up clip) out of it. A group already playing the clip keeps
    // its phase (0x1003fd0c).
    bool start_stand(const assets::AnimationClip* clip, std::uint8_t requested, std::uint8_t anim, float rate,
                     LayerRequest& req);
    // One frame: the base clip where no action holds the group, the action ending when the upper body has played
    // it through, then both groups advance by dt. True when a classic stand request started the follow-up.
    bool step(const LayerRequest& req, float dt);
    // Whether the upper body plays something other than the lower body.
    bool split() const;
};

} // namespace hailstone::render
