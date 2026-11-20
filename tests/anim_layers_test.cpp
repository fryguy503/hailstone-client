#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "assets/character.h"
#include "core/worker_pool.h"
#include "game/anim_layers.h"
#include "game/animation_types.h"
#include "game/world_state.h"
#include "render/actor_layers.h"
#include "render/actor_renderer.h"
#include "render/corpse_pose.h"

using namespace hailstone;
using namespace hailstone::game;
using hailstone::render::ActorLayers;
using hailstone::render::LayerRequest;

TEST_CASE("bone groups: the classic skeleton's callback") {
    CHECK(classic_bone_group("HUMCH_TRACK") == kUpperBones);
    CHECK(classic_bone_group("HUMNE_TRACK") == kUpperBones);
    CHECK(classic_bone_group("HUMHE_TRACK") == kUpperBones);
    CHECK(classic_bone_group("HUMHEAD_POINT_TRACK") == kUpperBones);
    CHECK(classic_bone_group("HUMBI_L_TRACK") == kUpperBones);
    CHECK(classic_bone_group("HUMFO_R_TRACK") == kUpperBones);
    CHECK(classic_bone_group("HUMFI_R2_TRACK") == kUpperBones);
    CHECK(classic_bone_group("HUMFA_DAG") == kUpperBones);
    CHECK(classic_bone_group("HUMHI_DAG") == kUpperBones);
    CHECK(classic_bone_group("HUMCHCHEST_DAG") == kLowerBones);
    CHECK(classic_bone_group("HUM_TRACK") == kLowerBones);
    CHECK(classic_bone_group("HUMPE_TRACK") == kLowerBones);
    CHECK(classic_bone_group("HUMTH_R_TRACK") == kLowerBones);
    CHECK(classic_bone_group("HUMR_POINT_TRACK") == kLowerBones); // not named: the lower group
    CHECK(classic_bone_group("HUMSHIELD_POINT_TRACK") == kLowerBones);
    CHECK(classic_bone_group("HU") == kLowerBones);
}

TEST_CASE("bone groups: the new-style skeleton's callback") {
    CHECK(new_style_bone_group("CHEST_CHEST01") == kLowerBones);
    CHECK(new_style_bone_group("CHEST_CHEST02") == kUpperBones);
    CHECK(new_style_bone_group("CHEST03_TORCH01") == kUpperBones);
    CHECK(new_style_bone_group("NECK_NECK") == kUpperBones);
    CHECK(new_style_bone_group("HEAD_HAIR01") == kUpperBones);
    CHECK(new_style_bone_group("ARML_HAND") == kUpperBones);
    CHECK(new_style_bone_group("ROOT_BONE") == kLowerBones);
    CHECK(new_style_bone_group("PELV") == kLowerBones);
    CHECK(new_style_bone_group("LEGL_THIGH") == kLowerBones);
}

TEST_CASE("classic layering tables") {
    CHECK_FALSE(classic_action_over_legs(1)); // C01 kick
    CHECK(classic_action_over_legs(2));
    CHECK(classic_action_over_legs(5)); // C05
    CHECK(classic_action_over_legs(9));
    CHECK_FALSE(classic_action_over_legs(10));
    CHECK_FALSE(classic_action_over_legs(11));
    CHECK(classic_action_over_legs(0x2b)); // casting
    CHECK_FALSE(classic_action_over_legs(0x2d));
    CHECK(classic_legs_under_action(0x11));
    CHECK(classic_legs_under_action(0x12));
    CHECK(classic_legs_under_action(0x20));
    CHECK(classic_legs_under_action(0x4b));
    CHECK_FALSE(classic_legs_under_action(0x4d));
    CHECK(classic_legs_under_action(0x4e));
    CHECK_FALSE(classic_interruptible(5));
    CHECK(classic_interruptible(0x12));
    CHECK(classic_interruptible(0x20));
    CHECK_FALSE(classic_interruptible(0x24));

    CHECK(classic_action_upper_only(5, false, true, 0.7f));
    CHECK_FALSE(classic_action_upper_only(5, false, true, 0.0f));  // standing: the whole body
    CHECK_FALSE(classic_action_upper_only(5, false, false, 0.7f)); // not humanoid
    CHECK_FALSE(classic_action_upper_only(1, false, true, 0.7f));  // a kick
    CHECK(classic_action_upper_only(1, true, false, 0.0f));        // a rider: the legs keep the rider animation
    CHECK(classic_legs_take_motion(5, 0x12, true));
    CHECK_FALSE(classic_legs_take_motion(5, 0x20, true)); // standing waits for the action
    CHECK_FALSE(classic_legs_take_motion(1, 0x12, true));
    CHECK_FALSE(classic_legs_take_motion(5, 0x12, false));
}

TEST_CASE("new-style layer modes (0x58ac90)") {
    LayerModeInput in;
    in.wanted = 0x6a; // NRUN
    in.playing = 0x6a;
    CHECK(new_style_layer_mode(in) == 3); // the same sequence and column: continue
    in.playing = 0xfe;
    CHECK(new_style_layer_mode(in) == 2);
    in.playing = 0x50; // SLPR: an action (hold 0)
    CHECK(new_style_layer_mode(in) == 1);
    in.completed = true;
    CHECK(new_style_layer_mode(in) == 2);
    in.completed = false;
    in.forced = true;
    CHECK(new_style_layer_mode(in) == 2);
    in.forced = false;
    in.playing = 0x68; // WALK, looping
    CHECK(new_style_layer_mode(in) == 2);
    in.wanted = 0x6e; // STND (priority 25) under a walk (100): a movement sequence gives way
    CHECK(new_style_layer_mode(in) == 2);
    in.playing = 0x6f; // IDLE (50, not movement) keeps playing against STND
    CHECK(new_style_layer_mode(in) == 1);
    in.playing = 0x79; // sitting down is not cut by a stand
    CHECK(new_style_layer_mode(in) == 1);
    in.playing = 0x92; // CRMP: hold 1, but a living PC holds it as 3
    in.stand_state = 0x73;
    CHECK(new_style_layer_mode(in) == 0);
    in.group = false;
    CHECK(new_style_layer_mode(in) == 3);
}

namespace {
constexpr std::uint32_t kFrameMs = 100; // the clip helper's frame time
constexpr std::uint32_t kStandFrames = 10, kActionFrames = 8;
constexpr float kStep = 0.1f;
constexpr float kHalfStep = kStep * 0.5f;
constexpr float kHalfBlend = 0.5f; // the blend weight halfway through
constexpr float kHalfPoseZ = 0.5f; // halfway between the last frame's z = 1 and the stand's z = 0
constexpr float kActionSeconds = static_cast<float>(kActionFrames * kFrameMs) / assets::AnimationClip::kMsPerSecond;
constexpr float kRunSpeed = 0.7f;
constexpr float kPacketRate = 0.5f; // a packet speed of 5 / 10
constexpr std::uint8_t kStanding = 100, kSitting = 110;
// New-style sequence codes the layer tests use (the sequence table at 0xaae3d8).
constexpr std::uint8_t kSlpr = 0x50, kKick = 0x5f;
constexpr std::uint8_t anim_code(hailstone::AnimationType a) { return static_cast<std::uint8_t>(a); }
assets::AnimationClip clip(std::uint32_t frames) {
    assets::AnimationClip c;
    c.frame_count = frames;
    c.frame_ms = kFrameMs;
    return c;
}

constexpr std::uint8_t anim(AnimationType a) { return static_cast<std::uint8_t>(a); }
} // namespace

TEST_CASE("classic: an attack while running plays over the legs") {
    auto run = clip(10), attack = clip(8), stand = clip(10);
    ActorLayers l;
    LayerRequest req;
    req.base = &run;
    req.base_anim = 0x12;
    req.humanoid = true;
    req.speed_run = 0.7f;
    for (int i = 0; i < 3; ++i) l.step(req, 0.1f);
    CHECK(l.group[kLowerBones].clip == &run);
    REQUIRE(l.start_action(&attack, anim_code(AnimationType::ThrowOrOneHandSlash), anim_code(AnimationType::ThrowOrOneHandSlash), 1.0f, req));
    CHECK_FALSE(l.action_on_legs);
    l.step(req, 0.1f);
    CHECK(l.group[kUpperBones].clip == &attack);
    CHECK(l.group[kLowerBones].clip == &run);
    CHECK(l.split());
    // Stopping under the attack: the legs keep their clip until it ends (P01 waits).
    req.base = &stand;
    req.base_anim = 0x20;
    req.stand_request = true;
    req.speed_run = 0.0f;
    l.step(req, 0.1f);
    CHECK(l.group[kLowerBones].clip == &run);
    for (int i = 0; i < 8; ++i) l.step(req, 0.1f);
    CHECK(l.action == nullptr);
    CHECK(l.group[kUpperBones].clip == &stand);
    CHECK(l.group[kLowerBones].clip == &stand);
}

TEST_CASE("classic: the interrupt test uses the requested number, the queue and state the chosen one") {
    using hailstone::AnimationType;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    auto stand = clip(kStandFrames), c05 = clip(kActionFrames), t05 = clip(kActionFrames);
    ActorLayers l;
    LayerRequest req;
    req.base = &stand;
    req.base_anim = anim(AnimationType::Stand);
    req.stand_request = true;
    l.step(req, kStep);
    // The upper body plays C05, which CastHeals may not interrupt.
    REQUIRE(l.start_action(&c05, anim(AnimationType::ThrowOrOneHandSlash), anim(AnimationType::ThrowOrOneHandSlash),
                           1.0f, req));
    l.step(req, kStep);
    REQUIRE(l.group[kUpperBones].clip == &c05);
    // CastNuke always interrupts; its clip is missing and 0x4b1210 makes it CastHeals. The test must use the
    // requested CastNuke, not the chosen CastHeals, or the action is refused and disappears.
    CHECK(l.start_action(&t05, anim(AnimationType::CastNuke), anim(AnimationType::CastHeals), 1.0f, req));
    CHECK(l.action_anim == anim(AnimationType::CastHeals));
}

TEST_CASE("classic: the stand path's clip reaches the layer, forward and keeping its phase") {
    using hailstone::AnimationType;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    auto has = [](std::string_view c) { return c == "P01" || c == "P08"; };
    // After a run the stand path picks P08; a model without P08 plays P01's clip and code.
    const auto p08_stand = hailstone::game::classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Run), kStanding, has);
    REQUIRE(p08_stand.clip == "P08");
    CHECK(p08_stand.anim == anim(AnimationType::StandAfter));
    auto has2 = [](std::string_view c) { return c == "P01"; };
    const auto p01_stand = hailstone::game::classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Run), kStanding, has2);
    CHECK(p01_stand.clip == "P01");
    CHECK(p01_stand.anim == anim(AnimationType::Stand));
    auto has3 = [](std::string_view) { return false; };
    CHECK(hailstone::game::classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Run), kStanding, has3).clip.empty());

    auto walk = clip(kStandFrames), p08 = clip(kStandFrames);
    ActorLayers l;
    LayerRequest req;
    req.base = &walk;
    req.base_anim = anim(AnimationType::Walk);
    req.reverse = true; // backward walking
    l.step(req, kStep);
    REQUIRE(l.group[kLowerBones].clip == &walk);
    // The real stand path replaces the walk on both groups, forward, with P08's own code.
    REQUIRE(l.start_stand(&p08, anim(AnimationType::Stand), p08_stand.anim, 1.0f, req));
    l.step(req, kStep);
    CHECK(l.group[kLowerBones].clip == &p08);
    CHECK(l.group[kUpperBones].clip == &p08);
    CHECK(l.group[kLowerBones].anim == p08_stand.anim);
    CHECK(l.group[kUpperBones].anim == p08_stand.anim);
    CHECK_FALSE(l.group[kLowerBones].reverse);
    // The same clip asked again keeps its phase (0x4b1ab3 clears the restart flag).
    const float t = l.group[kLowerBones].time;
    REQUIRE(l.start_stand(&p08, anim(AnimationType::Stand), p08_stand.anim, 1.0f, req));
    l.step(req, kStep);
    CHECK(l.group[kLowerBones].time > t);
    CHECK(l.group[kLowerBones].anim == p08_stand.anim);
}

TEST_CASE("classic: the stand path uses the posture and the play state before the movement choice") {
    using hailstone::AnimationType;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    auto has = [](std::string_view c) { return c == "P01" || c == "P07" || c == "P08"; };
    // Sitting: the stand request becomes the sit clip before the lookup, at the packet's speed.
    auto stand = hailstone::game::classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Run), kSitting, has);
    CHECK(stand.clip == "P07");
    CHECK(stand.anim == anim(AnimationType::Sit));
    CHECK(stand.sit);
    // A model without P07 plays nothing (0x4b1a06): the actor stays sitting.
    auto has2 = [](std::string_view c) { return c == "P01" || c == "P08"; };
    stand = hailstone::game::classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Run), kSitting, has2);
    CHECK(stand.clip.empty());
    // Standing: the play state before the movement choice decides P01 or P08 (a run gives P08, a walk P01).
    stand = hailstone::game::classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Run), kStanding, has);
    CHECK(stand.clip == "P08");
    CHECK_FALSE(stand.sit);
    stand = hailstone::game::classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Walk), kStanding, has);
    CHECK(stand.clip == "P01");
}

TEST_CASE("classic: the stand path off its feet (0x4b1917, 0x4b1b97)") {
    using hailstone::AnimationType;
    using hailstone::game::ClassicStandContext;
    using hailstone::game::classic_stand_request;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    constexpr std::uint8_t kDucking = hailstone::game::kStandStateDucking;
    constexpr std::uint16_t kRace3c = 0x3c, kHuman = 1;
    auto has = [](std::string_view c) { return c == "P01" || c == "P07" || c == "P08"; };
    // Race 0x3c and the local player in first person never take the sit clip: nothing plays.
    CHECK(classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Run), kSitting, has,
                                ClassicStandContext{std::nullopt, kRace3c, false})
              .clip.empty());
    CHECK(classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Run), kSitting, has,
                                ClassicStandContext{std::nullopt, kHuman, true})
              .clip.empty());
    CHECK(classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Run), kSitting, has,
                                ClassicStandContext{std::nullopt, kHuman, false})
              .clip == "P07");
    // Ducking: a P01 request is an idle request, so nothing plays.
    CHECK(classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Run), kDucking, has).clip.empty());
    // An action whose clip and replacement are missing is not an idle request: the stand plays, ducking or not.
    CHECK(classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Walk), kDucking, has,
                                ClassicStandContext{anim(AnimationType::MainhandSlash), kHuman, false})
              .clip == "P01");
    // A sitting actor's missing action is not converted to the sit clip either.
    CHECK(classic_stand_request(anim(AnimationType::MainhandSlash), anim(AnimationType::Walk), kSitting, has,
                                ClassicStandContext{anim(AnimationType::MainhandSlash), kHuman, false})
              .clip == "P01");
    CHECK(hailstone::game::classic_idle_request(anim(AnimationType::Turn)));
    CHECK(hailstone::game::classic_idle_request(0xc));
    CHECK(hailstone::game::classic_idle_request(0xf));
    CHECK_FALSE(hailstone::game::classic_idle_request(0x10));
    CHECK_FALSE(hailstone::game::classic_idle_request(anim(AnimationType::Sit)));
}

TEST_CASE("classic: a sitting actor's P07 plays as the packet action, not the stand queue") {
    using hailstone::AnimationType;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    auto p07 = clip(kStandFrames), social = clip(kActionFrames);
    ActorLayers l;
    LayerRequest req;
    req.base = &p07;
    req.base_anim = anim(AnimationType::Sit);
    l.step(req, kStep);
    // A follow-up is queued (as if an action was refused).
    l.follow_up = anim(AnimationType::Wave);
    l.follow_clip = &social;
    l.follow_rate = 1.0f;
    // The packet's P07 goes through the action path: one-shot, packet speed, and the queue stays.
    REQUIRE(l.start_action(&p07, anim(AnimationType::Stand), anim(AnimationType::Sit), kPacketRate, req));
    CHECK(l.action == &p07);
    CHECK(l.action_anim == anim(AnimationType::Sit));
    CHECK(l.follow_up == anim(AnimationType::Wave));
    l.step(req, kStep);
    CHECK(l.group[kUpperBones].clip == &p07);
    CHECK_FALSE(l.group[kUpperBones].loop);
    CHECK(l.group[kUpperBones].rate == doctest::Approx(kPacketRate));
}

TEST_CASE("classic: a stand packet after a waiting action leaves P01/P08, not the action") {
    using hailstone::AnimationType;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    auto walk = clip(kStandFrames), p08 = clip(kStandFrames), bash = clip(kActionFrames);
    ActorLayers l;
    LayerRequest req;
    req.base = &walk;
    req.base_anim = anim(AnimationType::Walk);
    l.step(req, kStep);
    // A waiting action (C02) is queued.
    l.follow_up = anim(AnimationType::Piercing);
    l.follow_clip = &bash;
    l.follow_rate = 1.0f;
    // The movement choice asks for L01, so its follow_up_after would still be C02; the stand packet replaces it
    // with P08 (the run before the movement choice).
    req.follow_up_after = anim(AnimationType::Piercing);
    REQUIRE(l.start_stand(&p08, anim(AnimationType::Stand), anim(AnimationType::StandAfter), 1.0f, req));
    req.follow_up_after = hailstone::game::classic_follow_up_after(anim(AnimationType::Run));
    l.step(req, kStep);
    CHECK(l.group[kUpperBones].clip == &bash); // the waiting action played
    CHECK(l.follow_up == anim(AnimationType::StandAfter));
    CHECK(l.follow_clip == nullptr);
}

TEST_CASE("classic: a stand request with only P08 on the legs keeps their phase") {
    using hailstone::AnimationType;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    auto p08 = clip(kStandFrames), action = clip(kActionFrames);
    ActorLayers l;
    LayerRequest req;
    req.base = &p08;
    req.base_anim = anim(AnimationType::StandAfter);
    req.humanoid = true;
    req.speed_run = kRunSpeed; // a moving humanoid: the action plays on the upper body alone
    l.step(req, kStep);
    // The upper body plays an action; the legs stay on P08.
    REQUIRE(l.start_action(&action, anim(AnimationType::ThrowOrOneHandSlash),
                           anim(AnimationType::ThrowOrOneHandSlash), 1.0f, req));
    l.step(req, kStep);
    REQUIRE(l.group[kLowerBones].clip == &p08);
    const float t = l.group[kLowerBones].time;
    // The action's cycle is through (the stand may replace it); the legs are still on P08.
    l.group[kUpperBones].advance(kActionSeconds);
    REQUIRE(l.group[kUpperBones].completed());
    // The real stand path asks for P08 again: the legs keep their phase, the action is dropped.
    auto has = [](std::string_view c) { return c == "P01" || c == "P08"; };
    const auto stand = hailstone::game::classic_stand_request(anim(AnimationType::Stand), anim(AnimationType::Run), kStanding, has);
    REQUIRE(stand.clip == "P08");
    REQUIRE(l.start_stand(&p08, anim(AnimationType::Stand), stand.anim, 1.0f, req));
    l.step(req, kStep);
    CHECK(l.group[kLowerBones].clip == &p08);
    CHECK(l.group[kLowerBones].time > t);
    CHECK(l.action == nullptr);
}

TEST_CASE("classic: a stand request refuses a corpse and an unfinished action it may not interrupt") {
    using hailstone::AnimationType;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    auto stand = clip(kStandFrames), c05 = clip(kActionFrames);
    ActorLayers l;
    LayerRequest req;
    req.base = &stand;
    req.base_anim = anim(AnimationType::Stand);
    l.step(req, kStep);
    REQUIRE(l.start_action(&c05, anim(AnimationType::ThrowOrOneHandSlash), anim(AnimationType::ThrowOrOneHandSlash),
                           1.0f, req));
    l.step(req, kStep);
    // C05 is not interruptible and its cycle is not done: the stand request is refused and the action stays.
    CHECK_FALSE(l.stand_request(anim(AnimationType::Stand), req));
    CHECK(l.action == &c05);
    // A corpse refuses too.
    req.corpse = true;
    CHECK_FALSE(l.stand_request(anim(AnimationType::Stand), req));
    req.corpse = false;
    // Once the cycle is done, the stand may replace it.
    l.step(req, kActionSeconds);
    CHECK(l.stand_request(anim(AnimationType::Stand), req));
    CHECK(l.action == nullptr);
}

TEST_CASE("classic: running under a whole-body attack takes the legs (humanoids)") {
    auto run = clip(10), attack = clip(8), stand = clip(10);
    ActorLayers l;
    LayerRequest req;
    req.base = &stand;
    req.base_anim = 0x20;
    req.stand_request = true;
    req.humanoid = true;
    l.step(req, 0.1f);
    REQUIRE(l.start_action(&attack, anim_code(AnimationType::ThrowOrOneHandSlash), anim_code(AnimationType::ThrowOrOneHandSlash), 1.0f, req));
    CHECK(l.action_on_legs);
    l.step(req, 0.1f);
    CHECK(l.group[kLowerBones].clip == &attack);
    req.base = &run;
    req.base_anim = 0x12;
    req.speed_run = 0.7f;
    l.step(req, 0.1f);
    CHECK(l.group[kLowerBones].clip == &run);
    CHECK(l.group[kUpperBones].clip == &attack);
    // Not a humanoid: the action holds the whole body until it ends.
    ActorLayers m;
    req.humanoid = false;
    req.base = &stand;
    req.base_anim = 0x20;
    req.stand_request = true;
    req.speed_run = 0.0f;
    m.step(req, 0.1f);
    REQUIRE(m.start_action(&attack, anim_code(AnimationType::ThrowOrOneHandSlash), anim_code(AnimationType::ThrowOrOneHandSlash), 1.0f, req));
    req.base = &run;
    req.base_anim = 0x12;
    req.speed_run = 0.7f;
    m.step(req, 0.1f);
    CHECK(m.group[kLowerBones].clip == &attack);
    for (int i = 0; i < 8; ++i) m.step(req, 0.1f);
    CHECK(m.group[kLowerBones].clip == &run);
    CHECK(m.group[kUpperBones].clip == &run);
    // The upper body comes back in step with the legs (SyncToBoneGroup).
    CHECK(m.group[kUpperBones].time == doctest::Approx(m.group[kLowerBones].time));
}

TEST_CASE("classic: a rider acts with the upper body, the legs keep the rider animation") {
    auto rider = clip(10), cast = clip(12);
    ActorLayers l;
    LayerRequest req;
    req.base = &rider;
    req.base_anim = 0x4a;
    req.mounted = true;
    l.step(req, 0.1f);
    REQUIRE(l.start_action(&cast, anim_code(AnimationType::CastHeals), anim_code(AnimationType::CastHeals), 1.0f, req));
    l.step(req, 0.1f);
    CHECK(l.group[kLowerBones].clip == &rider);
    CHECK(l.group[kUpperBones].clip == &cast);
}

TEST_CASE("new-style: an attack over a walk; a stand cuts it") {
    auto walk = clip(10), slash = clip(8), stand = clip(10), kick = clip(8);
    ActorLayers l;
    LayerRequest req;
    req.new_style = true;
    req.base = &walk;
    req.base_anim = 0x68;
    req.blend_ms = 100.0f;
    l.step(req, 0.1f);
    REQUIRE(l.start_action(&slash, kSlpr, kSlpr, 1.0f, req)); // SLPR: layer kind 1
    CHECK_FALSE(l.action_on_legs);
    l.step(req, 0.1f);
    CHECK(l.group[kUpperBones].clip == &slash);
    CHECK(l.group[kLowerBones].clip == &walk);
    req.base = &stand;
    req.base_anim = 0x6e;
    l.step(req, 0.1f);
    CHECK(l.action == nullptr);
    CHECK(l.group[kUpperBones].clip == &stand);
    CHECK(l.group[kLowerBones].clip == &stand);
    // A kick (kind 0) is a whole-body sequence.
    REQUIRE(l.start_action(&kick, kKick, kKick, 1.0f, req));
    CHECK(l.action_on_legs);
    // A rider plays only kind 1 sequences, and not over the unfinished kick (its mode is 1: queued, never played).
    req.mounted = true;
    CHECK_FALSE(l.start_action(&kick, kKick, kKick, 1.0f, req));
    CHECK_FALSE(l.start_action(&slash, kSlpr, kSlpr, 1.0f, req));
    for (int i = 0; i < 9; ++i) l.step(req, 0.1f);
    CHECK(l.start_action(&slash, kSlpr, kSlpr, 1.0f, req));
    CHECK_FALSE(l.action_on_legs);
    // The same sequence asked again keeps playing (mode 3), even once it is through.
    l.step(req, 0.1f);
    const float t = l.group[kUpperBones].time;
    CHECK_FALSE(l.start_action(&slash, kSlpr, kSlpr, 1.0f, req));
    CHECK(l.group[kUpperBones].time == doctest::Approx(t));
}

TEST_CASE("New-style layers distinguish the same sequence in another style column") {
    auto stand = clip(10), one_hand = clip(2), mounted = clip(2);
    ActorLayers l;
    LayerRequest req;
    req.new_style = true;
    req.base = &stand;
    req.base_anim = 0x6e;
    req.base_column = 5;
    req.mounted = true;
    l.step(req, 0.1f);
    CHECK(l.group[kLowerBones].column == 5);
    req.action_column = 1;
    REQUIRE(l.start_action(&one_hand, kSlpr, kSlpr, 1.0f, req));
    CHECK(l.group[kUpperBones].column == 1);
    CHECK(l.group[kLowerBones].clip == &stand);
    l.step(req, 0.2f);
    CHECK_FALSE(l.start_action(&one_hand, kSlpr, kSlpr, 1.0f, req)); // same sequence/style keeps its phase
    req.action_column = 5;
    CHECK(l.start_action(&mounted, kSlpr, kSlpr, 1.0f, req)); // finished sequence, new style can start
    CHECK(l.group[kUpperBones].clip == &mounted);
    CHECK(l.group[kUpperBones].column == 5);
    CHECK(l.group[kLowerBones].clip == &stand);
    CHECK(l.group[kLowerBones].column == 5);
}

TEST_CASE("classic: an action asked during an unfinished one follows it (+0x10c4)") {
    using hailstone::game::classic_may_interrupt;
    CHECK_FALSE(classic_may_interrupt(5, 5));   // never over itself
    CHECK_FALSE(classic_may_interrupt(2, 5));   // C05 is not interruptible
    CHECK(classic_may_interrupt(2, 0x20));      // P01 is
    CHECK(classic_may_interrupt(0x10, 5));      // D05 always
    CHECK_FALSE(classic_may_interrupt(0x20, 0x1a)); // a stand over a blend animation
    CHECK(classic_may_interrupt(0x1a, 5));
    auto stand = clip(10), slash = clip(8), bash = clip(6), run = clip(10);
    ActorLayers l;
    LayerRequest req;
    req.base = &stand;
    req.base_anim = 0x20;
    req.stand_request = true;
    req.humanoid = true;
    l.step(req, 0.1f);
    REQUIRE(l.start_action(&slash, anim_code(AnimationType::ThrowOrOneHandSlash), anim_code(AnimationType::ThrowOrOneHandSlash), 1.0f, req));
    l.step(req, 0.3f);
    CHECK_FALSE(l.start_action(&bash, anim_code(AnimationType::Piercing), anim_code(AnimationType::Piercing), 1.0f, req)); // waits
    CHECK(l.follow_up == 2);
    CHECK(l.group[kUpperBones].clip == &slash);
    l.step(req, 0.6f);
    l.step(req, 0.1f); // the slash is through: the stand request plays the follow-up
    CHECK(l.group[kUpperBones].clip == &bash);
    CHECK(l.follow_up == 0x20);
    for (int i = 0; i < 7; ++i) l.step(req, 0.1f);
    CHECK(l.group[kUpperBones].clip == &stand);
    // A movement request during the action drops the follow-up.
    REQUIRE(l.start_action(&slash, anim_code(AnimationType::ThrowOrOneHandSlash), anim_code(AnimationType::ThrowOrOneHandSlash), 1.0f, req));
    l.step(req, 0.1f);
    CHECK_FALSE(l.start_action(&bash, anim_code(AnimationType::Piercing), anim_code(AnimationType::Piercing), 1.0f, req));
    req.base = &run;
    req.base_anim = 0x12;
    req.speed_run = 0.7f;
    l.step(req, 0.1f);
    CHECK(l.follow_up == 0x20);
    // The same action asked again while it plays follows it, and does not restart it now.
    ActorLayers m;
    req.base = &stand;
    req.base_anim = 0x20;
    req.stand_request = true;
    req.speed_run = 0.0f;
    m.step(req, 0.1f);
    REQUIRE(m.start_action(&slash, anim_code(AnimationType::ThrowOrOneHandSlash), anim_code(AnimationType::ThrowOrOneHandSlash), 1.0f, req));
    m.step(req, 0.4f);
    CHECK_FALSE(m.start_action(&slash, anim_code(AnimationType::ThrowOrOneHandSlash), anim_code(AnimationType::ThrowOrOneHandSlash), 1.0f, req));
    CHECK(m.group[kUpperBones].time == doctest::Approx(0.4f));
}

TEST_CASE("classic: the follow-up leaves +0x10c4 as the stand request says; legs taking a run") {
    auto stand = clip(10), slash = clip(8), bash = clip(6), run = clip(10);
    ActorLayers l;
    LayerRequest req;
    req.base = &stand;
    req.base_anim = 0x20;
    req.stand_request = true;
    req.humanoid = true;
    l.step(req, 0.1f);
    REQUIRE(l.start_action(&slash, anim_code(AnimationType::ThrowOrOneHandSlash), anim_code(AnimationType::ThrowOrOneHandSlash), 1.0f, req));
    l.step(req, 0.3f);
    CHECK_FALSE(l.start_action(&bash, anim_code(AnimationType::Piercing), anim_code(AnimationType::Piercing), 1.0f, req));
    // The stand request plays the follow-up; +0x10c4 becomes what game::choose_spawn_animation said (0x47 here).
    req.follow_up_after = 0x47;
    CHECK_FALSE(l.step(req, 0.6f));
    CHECK(l.step(req, 0.1f));
    CHECK(l.group[kUpperBones].clip == &bash);
    CHECK(l.follow_up == 0x47);
    CHECK(l.follow_clip == nullptr);
    CHECK_FALSE(l.legs_took_motion);
    // A run under an upper-body action takes the legs: the run is then the animation playing.
    ActorLayers m;
    req.follow_up_after = 0x20;
    m.step(req, 0.1f);
    req.speed_run = 0.7f;
    REQUIRE(m.start_action(&slash, anim_code(AnimationType::ThrowOrOneHandSlash), anim_code(AnimationType::ThrowOrOneHandSlash), 1.0f, req));
    CHECK_FALSE(m.legs_took_motion);
    req.base = &run;
    req.base_anim = 0x12;
    m.step(req, 0.1f);
    CHECK(m.legs_took_motion);
    CHECK(m.action == &slash);
}

TEST_CASE("cross-fades are per bone group, linear over the blend time") {
    auto a = clip(10), b = clip(10);
    render::GroupTrack g;
    CHECK(g.start(&a, 0x11, 1.0f, true, false, 0.1f)); // nothing playing: switched at once
    CHECK(g.blend == 1.0f);
    CHECK_FALSE(g.start(&a, 0x11, 2.0f, true, false, 0.1f)); // the same clip keeps playing
    CHECK(g.rate == 2.0f);
    CHECK(g.start(&b, 0x12, 1.0f, true, false, 0.1f));
    CHECK(g.previous == &a);
    g.advance(0.05f);
    CHECK(g.blend == doctest::Approx(0.5f));
    g.advance(0.05f);
    CHECK(g.blend == doctest::Approx(1.0f));
    CHECK(g.previous == nullptr);
    CHECK(g.completed() == false);
    g.advance(0.9f);
    CHECK(g.completed());
}

TEST_CASE("a played-out one-shot's blend keeps its last frame") {
    using hailstone::AnimationType;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    auto one_shot = clip(kActionFrames), loop = clip(kStandFrames);
    render::GroupTrack g;
    CHECK(g.start(&one_shot, anim(AnimationType::SitStand), 1.0f, false, false, kStep)); // a one-shot
    CHECK(g.start(&loop, anim(AnimationType::Sit), 1.0f, true, false, kStep));           // blending to a loop
    CHECK_FALSE(g.previous_loop);
    render::GroupTrack h;
    CHECK(h.start(&loop, anim(AnimationType::Sit), 1.0f, true, false, kStep));           // a loop
    CHECK(h.start(&one_shot, anim(AnimationType::SitStand), 1.0f, false, false, kStep)); // blending to a one-shot
    CHECK(h.previous_loop);
}

TEST_CASE("a played-out one-shot's blend holds its last frame in the pose") {
    // A one-shot whose last frame differs from its first; frame_count comes from the track.
    assets::AnimationClip one_shot;
    one_shot.frame_ms = kFrameMs;
    assets::BonePose first, last;
    last.translation = {0, 0, 1};
    one_shot.tracks = {assets::AnimationTrack{first, first, first, last}};
    one_shot.frame_count = static_cast<std::uint32_t>(one_shot.tracks[0].size());
    assets::AnimationClip loop;
    loop.frame_ms = kFrameMs;
    loop.frame_count = 1;
    assets::BonePose stand;
    loop.tracks = {assets::AnimationTrack{stand}};
    assets::CharacterModel model;
    std::vector<assets::BonePose> local, previous;
    std::vector<glm::mat4> world;
    // The one-shot has played out; the blend is halfway to the stand.
    const float after = one_shot.duration_seconds() + kStep;
    render::PoseInput in;
    in.clip = &loop;
    in.loop = true;
    in.previous = &one_shot;
    in.previous_time = after;
    in.previous_loop = false;
    in.blend = kHalfBlend;
    render::pose_actor(model, in, local, previous, world);
    CHECK(local[0].translation.z == doctest::Approx(kHalfPoseZ));
    // With the old hardcoded loop, the one-shot wraps to frame 0 (z = 0) and the blend is 0.
    in.previous_loop = true;
    render::pose_actor(model, in, local, previous, world);
    CHECK(local[0].translation.z == doctest::Approx(0.0f));
    // The upper body's own blend uses its own previous_loop.
    std::vector<std::uint8_t> groups = {game::kUpperBones};
    in.upper_clip = &loop;
    in.upper_loop = true;
    in.upper_previous = &one_shot;
    in.upper_previous_time = after;
    in.upper_previous_loop = false;
    in.upper_blend = kHalfBlend;
    in.bone_groups = &groups;
    render::pose_actor(model, in, local, previous, world);
    CHECK(local[0].translation.z == doctest::Approx(kHalfPoseZ));
}

TEST_CASE("describe_pose names the clips and the bones a clip leaves at rest") {
    assets::CharacterModel model;
    model.code = "TST";
    model.source = "test.s3d:test.wld";
    model.animation_donor = "ELF";
    model.bone_names = {"TST_TRACK", "TSTBI_L_TRACK", "TSTBI_R_TRACK", "TSTL_POINT_TRACK"};
    model.parents = {-1, 0, 0, 1};
    model.order = {0, 1, 2, 3};
    const assets::BonePose rest;
    assets::BonePose raised;
    raised.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    assets::AnimationClip rest_clip;
    rest_clip.frame_count = 1;
    for (int b = 0; b < 4; ++b) rest_clip.tracks.push_back(assets::AnimationTrack{rest});
    // The right arm moves; the left arm and the point keep their rest frames.
    assets::AnimationClip stand;
    stand.frame_count = 1;
    stand.frame_ms = kFrameMs;
    stand.borrowed = true;
    stand.tracks = {assets::AnimationTrack{rest}, assets::AnimationTrack{rest}, assets::AnimationTrack{raised},
                    assets::AnimationTrack{rest}};
    model.clips.emplace("", std::move(rest_clip));
    model.clips.emplace("P01", std::move(stand));

    render::PoseInput in;
    in.clip = &model.clips.at("P01");
    auto lines = render::describe_pose(model, in);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "model TST from test.s3d:test.wld, clips lent by ELF");
    CHECK(lines[1].starts_with("whole body: P01 (lent) at 0.00"));
    CHECK(lines[2] == "bones at rest: BI_L"); // not the root, not the point

    // A clip the model does not hold, on the upper body alone.
    assets::AnimationClip foreign = model.clips.at("P01");
    std::vector<std::uint8_t> groups = {game::kLowerBones, game::kUpperBones, game::kUpperBones, game::kUpperBones};
    in.upper_clip = &foreign;
    in.bone_groups = &groups;
    lines = render::describe_pose(model, in);
    REQUIRE(lines.size() == 4);
    CHECK(lines[1].starts_with("lower body: P01 (lent)"));
    CHECK(lines[2].starts_with("upper body: ? at"));
}

TEST_CASE("bone_world blends a played-out one-shot's last frame") {
    // A one-shot whose last frame differs from its first; frame_count comes from the track.
    assets::AnimationClip one_shot;
    one_shot.frame_ms = kFrameMs;
    assets::BonePose first, last;
    last.translation = {0, 0, 1};
    one_shot.tracks = {assets::AnimationTrack{first, first, first, last}};
    one_shot.frame_count = static_cast<std::uint32_t>(one_shot.tracks[0].size());
    assets::AnimationClip loop;
    loop.frame_ms = kFrameMs;
    loop.frame_count = 1;
    assets::BonePose stand;
    loop.tracks = {assets::AnimationTrack{stand}};
    assets::CharacterModel model;
    model.bone_names = {"TST_TRACK"};
    model.parents = {-1};
    model.order = {0};
    render::ActorDraw a;
    a.model = &model;
    a.clip = &loop;
    a.loop = true;
    a.previous_clip = &one_shot;
    a.previous_time = one_shot.duration_seconds() + kStep;
    a.previous_loop = false;
    a.blend = kHalfBlend;
    glm::mat4 world;
    REQUIRE(render::ActorRenderer::bone_world(a, 0, world));
    CHECK(world[3][2] == doctest::Approx(kHalfPoseZ));
    // The old hardcoded loop would wrap the one-shot to frame 0 and blend to 0.
    a.previous_loop = true;
    REQUIRE(render::ActorRenderer::bone_world(a, 0, world));
    CHECK(world[3][2] == doctest::Approx(0.0f));
}

TEST_CASE("split() sees a previous clip that only one group let loop") {
    using hailstone::AnimationType;
    const auto anim = [](AnimationType a) { return static_cast<std::uint8_t>(a); };
    auto one_shot = clip(kActionFrames), loop = clip(kStandFrames);
    ActorLayers l;
    LayerRequest req;
    req.base = &one_shot;
    req.base_anim = anim(AnimationType::SitStand);
    req.loop = false;
    l.step(req, kStep);
    req.base = &loop;
    req.base_anim = anim(AnimationType::Sit);
    req.loop = true;
    l.step(req, kHalfStep); // the blend is still running
    // Both groups blend from the same one-shot, but only the upper body's previous clip loops.
    REQUIRE(l.group[kUpperBones].previous == l.group[kLowerBones].previous);
    l.group[kUpperBones].previous_loop = true;
    l.group[kLowerBones].previous_loop = false;
    CHECK(l.split());
}

TEST_CASE("GroupTrack::finish puts the clip at the end of its playback without a cross-fade") {
    auto a = clip(10), b = clip(10);
    render::GroupTrack g;
    CHECK(g.start(&a, anim(AnimationType::Stand), 1.0f, true, false, 0.1f));
    CHECK(g.start(&b, anim(AnimationType::Death), 1.0f, false, false, 0.1f));
    REQUIRE(g.previous == &a);
    g.finish();
    CHECK(g.time == doctest::Approx(b.duration_seconds()));
    CHECK(g.elapsed == doctest::Approx(b.duration_seconds()));
    CHECK(g.blend == 1.0f);
    CHECK(g.previous == nullptr);
    CHECK(g.completed());
    // Reversed, the end of playback is the first source frame.
    g.start(&b, anim(AnimationType::Death), 1.0f, false, true, 0.0f, true);
    g.finish();
    CHECK(g.shown_time() == doctest::Approx(0.0f));
    // A faster clip is through sooner; a stopped one never is.
    g.start(&b, anim(AnimationType::Death), 2.0f, false, false, 0.0f, true);
    g.finish();
    CHECK(g.elapsed == doctest::Approx(b.duration_seconds() / 2.0f));
    CHECK(g.completed());
    g.start(&b, anim(AnimationType::Death), 0.0f, false, false, 0.0f, true);
    g.finish();
    CHECK_FALSE(g.completed());
}

TEST_CASE("spawn_dead: a corpse or a spawn lying down") {
    CHECK(render::spawn_dead(true, game::kStandStanding));
    CHECK(render::spawn_dead(false, game::kStandDead)); // feign death
    CHECK_FALSE(render::spawn_dead(false, game::kStandStanding));
}

TEST_CASE("corpse_starts_at_end: only a spawn already dead when first drawn") {
    // A fresh state that is dead (zone-in, coming into view, back from hiding): the end pose.
    CHECK(render::corpse_starts_at_end(true, false, render::spawn_dead(true, game::kStandStanding)));
    CHECK(render::corpse_starts_at_end(true, false, render::spawn_dead(false, game::kStandDead)));
    // Drawn alive last frame: the fall plays, also when the model changes in the same frame.
    CHECK_FALSE(render::corpse_starts_at_end(true, true, render::spawn_dead(true, game::kStandStanding)));
    CHECK_FALSE(render::corpse_starts_at_end(false, true, render::spawn_dead(false, game::kStandDead)));
    // A living spawn.
    CHECK_FALSE(render::corpse_starts_at_end(true, false, render::spawn_dead(false, game::kStandStanding)));
}

TEST_CASE("a corpse first drawn dead is posed at the death clip's last frame") {
    // A death clip whose first and last frame differ: the fall ends lying down.
    assets::CharacterModel m;
    m.code = "HUM";
    m.bone_names = {"HUM_TRACK"};
    m.parents = {-1};
    m.order = {0};
    assets::AnimationClip death;
    death.frame_count = 2;
    death.frame_ms = 100;
    death.tracks.resize(1);
    for (float x : {0.0f, 10.0f}) {
        assets::BonePose p;
        p.translation = glm::vec3(x, 0.0f, 0.0f);
        death.tracks[0].push_back(p);
    }
    std::vector<assets::BonePose> first, last;
    m.sample_local(death, 0.0f, false, first);
    m.sample_local(death, death.duration_seconds(), false, last);
    REQUIRE(first[0].translation.x != last[0].translation.x);

    // The request prepare_spawns builds, from the same derivation.
    auto request = [&](bool fresh, bool was_alive, bool is_corpse, std::uint8_t stand_state) {
        LayerRequest req;
        const bool dead = render::spawn_dead(is_corpse, stand_state);
        render::corpse_request(req, death, render::corpse_starts_at_end(fresh, was_alive, dead));
        return req;
    };
    auto posed_x = [&](const ActorLayers& l) {
        std::vector<assets::BonePose> posed;
        m.sample_local(*l.group[kLowerBones].clip, l.group[kLowerBones].shown_time(), l.group[kLowerBones].loop, posed);
        return posed[0].translation.x;
    };
    // Before the clip could have played (0.01 s of its 0.2 s): the end pose already.
    ActorLayers corpse;
    corpse.step(request(true, false, true, game::kStandStanding), 0.01f);
    CHECK(posed_x(corpse) == doctest::Approx(last[0].translation.x));
    // Feign death (stand state 115) at zone-in: the same.
    ActorLayers feigned;
    feigned.step(request(true, false, false, game::kStandDead), 0.01f);
    CHECK(posed_x(feigned) == doctest::Approx(last[0].translation.x));
    // Drawn alive last frame, then dead (also with a model change): the fall starts at its first frame.
    ActorLayers dying;
    dying.step(request(true, true, true, game::kStandStanding), 0.01f);
    CHECK(posed_x(dying) < last[0].translation.x);
}

TEST_CASE("a lying corpse takes a changed death clip at its end, not from the fall") {
    auto death = clip(10), held = clip(10);
    ActorLayers l;
    LayerRequest req;
    render::corpse_request(req, death, render::corpse_starts_at_end(true, false, true));
    l.step(req, 0.1f);
    REQUIRE(l.group[kLowerBones].completed());
    // A changed appearance gives another death clip (a held variant): it shows its end at once.
    render::corpse_request(req, held, false);
    l.step(req, 0.1f);
    CHECK(l.group[kLowerBones].clip == &held);
    CHECK(l.group[kLowerBones].time == doctest::Approx(held.duration_seconds()));
    CHECK(l.group[kLowerBones].completed());
}

TEST_CASE("a spawn that dies while drawn plays the death clip from its start") {
    auto stand = clip(10), death = clip(10);
    ActorLayers l;
    LayerRequest req;
    req.base = &stand;
    req.base_anim = anim(AnimationType::Stand);
    l.step(req, 0.1f);
    // Drawn alive last frame, then dead: the derived flag is false, so the fall plays.
    render::corpse_request(req, death,
                           render::corpse_starts_at_end(false, true, render::spawn_dead(true, game::kStandStanding)));
    l.step(req, 0.1f);
    CHECK(l.group[kLowerBones].clip == &death);
    CHECK(l.group[kLowerBones].time == doctest::Approx(0.1f));
    CHECK_FALSE(l.group[kLowerBones].completed());
}

TEST_CASE("a hidden spawn's return is not a visible living -> dead transition") {
    CHECK(render::drawn_alive_last_frame(true, false));        // drawn alive
    CHECK_FALSE(render::drawn_alive_last_frame(true, true));   // hidden: its state was kept, not drawn
    CHECK_FALSE(render::drawn_alive_last_frame(false, false)); // drawn dead
    // Back from hiding, dead: the end pose; a visible model change while dying: the fall.
    CHECK(render::corpse_starts_at_end(true, render::drawn_alive_last_frame(true, true), true));
    CHECK_FALSE(render::corpse_starts_at_end(true, render::drawn_alive_last_frame(true, false), true));
}

TEST_CASE("standing up from a corpse cross-fades from its last frame") {
    auto death = clip(10), stand = clip(10);
    ActorLayers l;
    LayerRequest req;
    render::corpse_request(req, death, render::corpse_starts_at_end(true, false, true));
    l.step(req, 0.1f);
    REQUIRE(l.group[kLowerBones].clip == &death);
    // The spawn stands up: the death clip is left as the one-shot it was.
    req = LayerRequest{};
    req.base = &stand;
    req.base_anim = anim(AnimationType::Stand);
    l.step(req, 0.01f);
    CHECK(l.group[kLowerBones].clip == &stand);
    CHECK(l.group[kLowerBones].previous == &death);
    CHECK_FALSE(l.group[kLowerBones].previous_loop);
}

TEST_CASE("a corpse's shared P01 fallback fades out as the one-shot it became") {
    auto p01 = clip(10), walk = clip(10);
    ActorLayers l;
    LayerRequest req;
    req.base = &p01;
    req.base_anim = anim(AnimationType::Stand);
    l.step(req, 0.1f);
    // A model without D05/D04 plays P01 as its death clip: the same clip, not restarted, but now played once.
    render::corpse_request(req, p01, render::corpse_starts_at_end(false, true, true));
    l.step(req, 0.1f);
    CHECK(l.group[kLowerBones].clip == &p01);
    CHECK_FALSE(l.group[kLowerBones].loop);
    // A later clip change fades out of the death clip as the one-shot it was.
    req = LayerRequest{};
    req.base = &walk;
    req.base_anim = anim(AnimationType::Walk);
    l.step(req, 0.01f);
    CHECK(l.group[kLowerBones].clip == &walk);
    CHECK(l.group[kLowerBones].previous == &p01);
    CHECK_FALSE(l.group[kLowerBones].previous_loop);
}

TEST_CASE("a cross-fade from a one-shot samples its last frame, not its start") {
    assets::CharacterModel m;
    m.code = "HUM";
    m.bone_names = {"HUM_TRACK"};
    m.parents = {-1};
    m.order = {0};
    auto one_bone = [](std::initializer_list<float> xs) {
        assets::AnimationClip c;
        c.frame_count = static_cast<std::uint32_t>(xs.size());
        c.frame_ms = 100;
        c.tracks.resize(1);
        for (float x : xs) {
            assets::BonePose p;
            p.translation = glm::vec3(x, 0.0f, 0.0f);
            c.tracks[0].push_back(p);
        }
        return c;
    };
    auto death = one_bone({0.0f, 10.0f}); // the fall: upright first, lying last
    auto stand = one_bone({20.0f});
    render::ActorDraw a;
    a.model = &m;
    a.clip = &stand;
    a.loop = true;
    a.previous_clip = &death;
    a.previous_time = death.duration_seconds(); // the fall played through
    a.previous_loop = false;
    a.blend = 0.5f;
    std::vector<assets::BonePose> local, previous;
    std::vector<glm::mat4> world;
    render::ActorRenderer::pose_bones(a, local, previous, world);
    // Halfway from the lying pose (10) to standing (20), not from the fall's first frame (0).
    CHECK(local[0].translation.x == doctest::Approx(15.0f));
}

TEST_CASE("an upper-body clip poses only the upper group's bones") {
    assets::CharacterModel m;
    m.code = "HUM";
    m.bone_names = {"HUM_TRACK", "HUMPE_TRACK", "HUMCH_TRACK"};
    m.parents = {-1, 0, 1};
    m.order = {0, 1, 2};
    auto pose = [](float x) {
        assets::AnimationClip c;
        c.frame_count = 1;
        c.tracks.resize(3);
        for (auto& t : c.tracks) {
            assets::BonePose p;
            p.translation = glm::vec3(x, 0.0f, 0.0f);
            t.push_back(p);
        }
        return c;
    };
    auto lower = pose(1.0f), upper = pose(10.0f);
    std::vector<std::uint8_t> groups;
    for (const auto& n : m.bone_names) groups.push_back(classic_bone_group(n));
    render::ActorDraw a;
    a.model = &m;
    a.clip = &lower;
    a.upper_clip = &upper;
    a.bone_groups = &groups;
    std::vector<assets::BonePose> local, previous;
    std::vector<glm::mat4> world;
    render::ActorRenderer::pose_bones(a, local, previous, world);
    REQUIRE(local.size() == 3);
    CHECK(local[0].translation.x == doctest::Approx(1.0f));
    CHECK(local[1].translation.x == doctest::Approx(1.0f));
    CHECK(local[2].translation.x == doctest::Approx(10.0f));
}

namespace {

// A three-bone chain whose root moves by `x`, with an inverse bind on its last bone.
struct PoseFixture {
    assets::CharacterModel model;
    assets::AnimationClip clip;
    PoseFixture() {
        model.parents = {-1, 0, 1};
        model.order = {0, 1, 2};
        model.inverse_bind.assign(3, glm::mat4(1.0f));
        model.inverse_bind[2][3] = {0, 0, -4, 1};
        clip.frame_count = 1;
        clip.tracks.resize(3);
        for (std::size_t b = 0; b < 3; ++b) {
            assets::BonePose p;
            p.translation = glm::vec3(1.0f, static_cast<float>(b), 2.0f);
            p.rotation = glm::angleAxis(0.3f * static_cast<float>(b + 1), glm::vec3(0, 0, 1));
            clip.tracks[b].push_back(p);
        }
    }
};

bool near_mat(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > 1e-4f) return false;
    return true;
}

} // namespace

TEST_CASE("Actor posing on the workers writes each instance's bones into its own slice, as posed one by one") {
    PoseFixture f;
    assets::CharacterMesh plain, skinned, broken;
    skinned.skin_bindings.push_back({2, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 3))});
    broken.skin_bindings.push_back({9, glm::mat4(1.0f)}); // a host bone past the skeleton: not posed
    std::vector<render::ActorDraw> actors(11);
    for (std::size_t i = 0; i < actors.size(); ++i) {
        actors[i].model = &f.model;
        actors[i].clip = &f.clip;
        actors[i].position = {10.0f * static_cast<float>(i), 5.0f, 1.0f};
        actors[i].yaw = 0.3f * static_cast<float>(i);
        actors[i].scale = 1.0f + 0.1f * static_cast<float>(i);
    }
    // As prepare plans them: each its slice, one after another, in the actors' order.
    auto plan = [&](std::vector<render::ActorRenderer::PoseJob>& jobs) {
        jobs.clear();
        std::uint32_t used = 0;
        for (std::size_t i = 0; i < actors.size(); ++i) {
            const assets::CharacterMesh* mesh = i == 8 ? &broken : i % 3 == 1 ? &skinned : &plain;
            auto count = static_cast<std::uint32_t>(3 + mesh->skin_bindings.size());
            if (i == 4) ++count; // planned for more bones than it makes: not posed
            jobs.push_back({i, mesh, used, count, true});
            used += count;
        }
        return used;
    };
    // The one-by-one pose each slice must hold.
    auto expected = [&](const render::ActorRenderer::PoseJob& job) {
        std::vector<assets::BonePose> local, previous;
        std::vector<glm::mat4> world;
        render::ActorRenderer::pose_bones(actors[job.actor], local, previous, world);
        f.model.skin(world);
        REQUIRE(job.mesh->append_skin_bones(world));
        for (auto& w : world) w = render::ActorRenderer::draw_matrix(actors[job.actor]) * w;
        return world;
    };
    const glm::mat4 untouched(-7.0f);
    for (unsigned workers : {0u, 3u}) {
        CAPTURE(workers);
        core::WorkerPool pool(workers);
        core::set_workers(&pool);
        std::vector<render::ActorRenderer::PoseJob> jobs;
        std::vector<glm::mat4> bones(plan(jobs), untouched);
        render::ActorRenderer::pose_instances(actors, jobs, bones);
        core::set_workers(nullptr);
        for (const auto& job : jobs) {
            CAPTURE(job.actor);
            const bool fails = job.actor == 4 || job.actor == 8;
            CHECK(job.posed == !fails);
            if (fails) {
                for (std::uint32_t b = 0; b < job.count; ++b) CHECK(bones[job.first + b] == untouched);
                continue;
            }
            const auto want = expected(job);
            REQUIRE(want.size() == job.count);
            for (std::uint32_t b = 0; b < job.count; ++b) CHECK(near_mat(bones[job.first + b], want[b]));
        }
    }
    // A slice past the end of the frame's bones is not written.
    std::vector<render::ActorRenderer::PoseJob> past{{0, &plain, 2, 3, true}};
    std::vector<glm::mat4> small(4, untouched);
    render::ActorRenderer::pose_instances(actors, past, small);
    CHECK_FALSE(past[0].posed);
    for (const auto& m : small) CHECK(m == untouched);
}

TEST_CASE("A drawn actor's bones are posed once and kept until the pose is marked stale") {
    PoseFixture f;
    render::ActorDraw a;
    a.model = &f.model;
    a.clip = &f.clip;
    a.position = {10, 20, 30};
    a.scale = 2.0f;
    render::ActorRenderer::BonePoseCache cache;
    std::vector<assets::BonePose> local, previous;
    glm::mat4 world(0.0f), direct(0.0f);
    for (int b = 0; b < 3; ++b) {
        REQUIRE(render::ActorRenderer::cached_bone_world(a, b, cache, local, previous, world));
        REQUIRE(render::ActorRenderer::bone_world(a, b, direct));
        CHECK(near_mat(world, direct));
    }
    CHECK(cache.valid);
    // Kept: the instance moving does not change what the cache gives until it is marked stale.
    const glm::mat4 kept = world;
    a.position.x += 50.0f;
    REQUIRE(render::ActorRenderer::cached_bone_world(a, 2, cache, local, previous, world));
    CHECK(near_mat(world, kept));
    cache.valid = false;
    REQUIRE(render::ActorRenderer::cached_bone_world(a, 2, cache, local, previous, world));
    REQUIRE(render::ActorRenderer::bone_world(a, 2, direct));
    CHECK(near_mat(world, direct));
    CHECK_FALSE(near_mat(world, kept));
    // As bone_world: a bone past the skeleton, a negative one or no clip give nothing.
    CHECK_FALSE(render::ActorRenderer::cached_bone_world(a, 3, cache, local, previous, world));
    CHECK_FALSE(render::ActorRenderer::cached_bone_world(a, -1, cache, local, previous, world));
    a.clip = nullptr;
    CHECK_FALSE(render::ActorRenderer::cached_bone_world(a, 0, cache, local, previous, world));
}

TEST_CASE("Actor role zero position uses the posed name bone before inverse bind") {
    assets::CharacterModel model;
    model.name_bone = 1;
    model.parents = {-1, 0};
    model.order = {0, 1};
    model.inverse_bind.assign(2, glm::mat4(1.0f));
    model.inverse_bind[1][3] = {0, 0, -5, 1};
    assets::AnimationClip clip;
    clip.tracks.resize(2);
    clip.tracks[0].push_back(assets::BonePose{});
    assets::BonePose head;
    head.translation = {0, 0, 5};
    clip.tracks[1].push_back(head);
    render::ActorDraw actor;
    actor.model = &model;
    actor.clip = &clip;
    actor.position = {10, 20, 30};
    actor.scale = 2;
    glm::vec3 position(0.0f);
    REQUIRE(render::ActorRenderer::name_position(actor, position));
    CHECK(position.z == doctest::Approx(40.0f));
    model.name_bone = -1;
    position = {7, 8, 9};
    CHECK_FALSE(render::ActorRenderer::name_position(actor, position));
    CHECK(position == glm::vec3(7, 8, 9));
}

TEST_CASE("other spawns' water: the body and feet regions are probed with each step") {
    WorldState w;
    w.player_id = 9;
    Spawn swimmer;
    swimmer.heights.avatar_height = 3.0f;
    swimmer.update_time = 1.0;
    swimmer.fly_mode = 1;
    swimmer.position = glm::vec3(0.0f, 0.0f, 12.0f); // feet at 9: water up to z 10
    w.spawns[1] = swimmer;
    Spawn stale = swimmer; // no update for more than 12 s: not stepped, its levels kept
    stale.update_time = -20.0;
    w.spawns[2] = stale;
    w.spawns[9] = swimmer; // the local player: its controller's
    auto water = [](const glm::vec3& p) { return static_cast<std::uint8_t>(p.z < 10.0f ? 5 : 0); };
    w.smooth(0.016f, 2.0, {}, water);
    CHECK(w.spawns[1].body_wet == 0);
    CHECK(w.spawns[1].feet_wet == 5);
    CHECK(w.spawns[2].feet_wet == 0);
    CHECK(w.spawns[9].feet_wet == 0);
    w.spawns[1].position.z = 9.0f;
    w.spawns[1].update_time = 2.0;
    w.smooth(0.016f, 2.0, {}, water);
    CHECK(w.spawns[1].body_wet == 5);
}
