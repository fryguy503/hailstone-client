#include <doctest/doctest.h>

#include <array>
#include <limits>

#include "game/follow.h"
#include "game/mechanics/movement.h"

using namespace hailstone::game;
namespace {
FollowProfile direct_profile() {
    return {15, 15, 3, 150, 8, 12, 20, 64, 96, 15, .1f, .75f, 64};
}
FollowInput ahead(float distance) {
    FollowInput in;
    in.target_y = distance; in.distance = distance;
    return in;
}
}

TEST_CASE("follow profile: every field is bounded and nonfinite data is rejected") {
    const auto original = direct_profile();
    REQUIRE(valid_follow_profile(original));
    for (auto field : {&FollowProfile::stop_distance, &FollowProfile::restart_margin, &FollowProfile::start_range,
                       &FollowProfile::turn_snap, &FollowProfile::turn_slow, &FollowProfile::turn_fast,
                       &FollowProfile::turn_fast_angle, &FollowProfile::move_angle, &FollowProfile::move_far_margin,
                       &FollowProfile::pitch_gain, &FollowProfile::pitch_down_scale, &FollowProfile::pitch_limit}) {
        for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f, 1000.0f}) {
            auto p = original; p.*field = bad; CHECK_FALSE(valid_follow_profile(p));
        }
    }
    auto unknown = original; unknown.flags |= 16; CHECK_FALSE(valid_follow_profile(unknown));
    auto p = original; p.stop_distance = 1; p.start_range = 200; CHECK(valid_follow_profile(p));
    p.stop_distance = 50; CHECK(valid_follow_profile(p));
}

TEST_CASE("follow profile: absolute stop distance and restart band do not depend on model heights") {
    auto p = direct_profile();
    auto in = ahead(17); in.height = 100; in.target_height = 100;
    CHECK(profiled_follow_step(in, p, true, 0, 1.0f / 60).move);
    CHECK_FALSE(profiled_follow_step(in, p, false, 0, 1.0f / 60).move);
    CHECK_FALSE(profiled_follow_step(ahead(15), p, true, 0, 1.0f / 60).move);
    CHECK_FALSE(profiled_follow_step(ahead(18), p, false, 0, 1.0f / 60).move);
    CHECK(profiled_follow_step(ahead(18.01f), p, false, 0, 1.0f / 60).move);
    in = ahead(100); in.clear_line = false;
    CHECK(profiled_follow_step(in, p, false, 0, 1.0f / 60).move); // no route finding or invented LOS bypass in collision
}

TEST_CASE("follow profile: turning and pitch are time-scaled and shortest-way bounded") {
    const auto p = direct_profile();
    FollowInput in; in.target_x = 100; in.target_z = 100; in.distance = 100;
    auto full = profiled_follow_step(in, p, false, 0, 1.0f / 60);
    CHECK(full.heading == doctest::Approx(20));
    auto half = profiled_follow_step(in, p, false, 0, 1.0f / 120);
    in.heading = half.heading;
    half = profiled_follow_step(in, p, half.move, half.pitch, 1.0f / 120);
    CHECK(half.heading == doctest::Approx(full.heading));
    CHECK(half.pitch == doctest::Approx(full.pitch));
    in = ahead(100); in.heading = 508;
    CHECK(profiled_follow_step(in, p, false, 0, 1.0f / 60).heading == 0.0f);
    in.target_z = -100;
    CHECK(profiled_follow_step(in, p, false, 0, 1.0f / 60).pitch == doctest::Approx(-4.8f));
    in.distance = std::numeric_limits<float>::infinity();
    CHECK_FALSE(profiled_follow_step(in, p, false, 0, 1.0f / 60).move);
}

TEST_CASE("follow profile: actor identity is required for both ends and never follows reused numeric IDs") {
    FollowIdentity identity{1, 2, 100, 200};
    CHECK(identity.matches(1, 100, 2, 200));
    CHECK_FALSE(identity.matches(1, 101, 2, 200));
    CHECK_FALSE(identity.matches(1, 100, 2, 201));
    CHECK_FALSE(identity.matches(1, 0, 2, 200));
    identity.target = 1; CHECK_FALSE(identity.matches(1, 100, 1, 200));
}

TEST_CASE("follow profile: absent extension leaves original distance walk run and leash rules") {
    auto in = ahead(26); in.height = 6; in.target_height = 6;
    CHECK_FALSE(follow_step(in).move);
    in.distance = 28; in.speed = 1; in.target_speed = .5f;
    auto step = follow_step(in); CHECK(step.move); CHECK_FALSE(step.run);
    in.distance = 50; CHECK(follow_step(in).run);
    in.distance = 201; CHECK(follow_step(in).stop_message == kStrFollowTooFar);
    in.target_animation = kFollowDeadAnimation; CHECK(follow_step(in).stop_message == kStrFollowDead);
}

TEST_CASE("mount profile: instant acceleration preserves ordinary target speeds and stops") {
    using namespace hailstone::game::mechanics;
    auto run = [](bool instant, bool riding, bool forward, bool back, bool frozen, bool running) {
        MovementController controller; controller.reset({0, 0, 0}, 0); CharacterInput character;
        character.riding = riding; character.instant_ridden_acceleration = instant;
        character.run_speed = 1.2f; character.walk_speed = .4f;
        character.run_walk = running ? 1 : 0; character.frozen = frozen;
        ControlInput input; input.forward = forward; input.back = back;
        controller.controls(input, character, 1000);
        return controller.body.speed_run;
    };
    const float normal = run(false, true, true, false, false, true);
    const float instant = run(true, true, true, false, false, true);
    CHECK(normal > 0.0f); CHECK(normal < instant); CHECK(instant == doctest::Approx(1.2f));
    CHECK(run(true, true, true, false, true, true) == 0.0f);
    CHECK(run(true, true, false, true, false, true) < 0.0f);
    CHECK(run(true, true, true, false, false, false) < instant);
    CHECK(run(true, false, true, false, false, true) == run(false, false, true, false, false, true));
    CHECK(run(true, true, false, false, false, true) == 0.0f);
}
