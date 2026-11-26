#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>

#include "game/coords.h"
#include "game/follow.h"
#include "game/mechanics/movement.h"
#include "game/world_state.h"

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

TEST_CASE("follow profile: remote swimmers keep their depth over a submerged floor") {
    WorldState world;
    world.player_id = 1;
    auto& target = world.spawns[2];
    target.id = 2;
    target.position = {40.0f, 0.0f, 20.0f};
    target.velocity = {1.0f, 0.0f, 0.0f};
    target.speed_run = 1.0f;
    target.heights.avatar_height = 5.0f;
    const WorldState::FloorFn floor = [](const glm::vec3&) { return 0.0f; };
    const WorldState::RegionFn region = [](const glm::vec3& p) -> std::uint8_t {
        return p.x < 60.0f && p.z < 21.0f ? 5 : 0;
    };
    constexpr float dt = 1.0f / 60;
    auto follow = [&](float follower_z) {
        const glm::vec3 to = world_to_server(target.position);
        auto in = ahead(to.y);
        in.target_x = to.x;
        in.z = follower_z;
        in.target_z = to.z;
        return profiled_follow_step(in, direct_profile(), true, 0.0f, dt);
    };

    world.smooth(dt, 1.0, floor, region);
    CHECK(target.position.x > 40.0f);
    CHECK(target.position.z == 20.0f);
    CHECK(target.body_wet == 5);
    const auto level = follow(20.0f);
    REQUIRE(level.move);
    CHECK(level.pitch == 0.0f);

    target.position.z = 23.0f; // At the surface, the body sample is dry but the feet sample is wet.
    world.smooth(dt, 1.0 + dt, floor, region);
    CHECK(target.position.z == 23.0f);
    CHECK(target.body_wet == 0);
    CHECK(target.feet_wet == 5);
    CHECK(follow(23.0f).pitch == 0.0f);

    const WorldState::FloorFn bank = [](const glm::vec3&) { return 17.0f; };
    world.smooth(dt, 1.0 + 2 * dt, bank, region);
    CHECK(target.position.z == 22.0f); // On a shallow bank only the feet are wet.

    target.position.z = 8.0f; // The next position update puts the leader deeper, then it stops.
    target.speed_run = 0.0f;
    target.velocity = glm::vec3(0.0f);
    world.smooth(dt, 1.0 + 3 * dt, floor, region);
    CHECK(target.position.z == 8.0f);
    CHECK(follow(20.0f).pitch < 0.0f);

    target.position.z = 4.0f;
    world.smooth(dt, 1.0 + 4 * dt, floor, region);
    CHECK(target.position.z == 5.0f); // Feet below the floor are still raised out of it.

    target.position = {70.0f, 0.0f, 25.0f};
    target.speed_run = 1.0f;
    world.smooth(dt, 1.0 + 5 * dt, floor, region);
    CHECK(target.position.z == 5.0f); // A moving player on dry land still finds the floor.
    CHECK(target.body_wet == 0);
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

namespace {

struct SwimmingFollower {
    static constexpr float kHeight = 6.0f;
    static constexpr float kStepMs = 16.0f;
    static constexpr float kFloor = 0.0f;

    mechanics::MovementController movement;
    mechanics::CharacterInput character;
    bool profiled = false;
    bool moving_forward = false;
    bool swim_jumped = false;
    std::uint32_t now = 1000;

    explicit SwimmingFollower(bool enhanced) : profiled(enhanced) {
        movement.reset({0.0f, 0.0f, 20.0f - kHeight}, 0.0f);
        movement.body.floor_height = kFloor;
        movement.body.body_wet = 5;
        movement.body.camera_angle = -64.0f;
        character.swimming_skill = 100;
    }

    float depth() const { return movement.body.feet.z + kHeight; }

    FollowSwimStep tick(float target_y, float target_z) {
        const glm::vec3& feet = movement.body.feet;
        auto in = ahead(target_y);
        const glm::vec3 from = world_to_server(feet);
        in.x = from.x;
        in.y = from.y;
        in.z = depth();
        in.height = kHeight;
        in.heading = movement.body.heading;
        in.speed = movement.body.speed_run;
        in.target_z = target_z;
        in.target_height = 3.0f;
        in.distance = std::hypot(in.target_x - in.x, in.target_y - in.y);
        if (profiled) {
            const auto step = profiled_follow_step(in, direct_profile(), moving_forward, movement.body.camera_angle,
                                                   kStepMs / 1000.0f);
            moving_forward = step.move;
            movement.body.heading = step.heading;
        } else {
            const auto step = follow_step(in);
            moving_forward = step.move;
            movement.body.heading = step.heading;
            if (step.move) character.run_walk = step.run ? 1 : 0;
        }
        const auto swim = follow_swim_step(in, moving_forward);
        movement.set_autorun(moving_forward ? 1 : 0);
        now += 16;
        mechanics::ControlInput input;
        input.follow_swim = mechanics::SwimSteering{swim.pitch, swim.vertical_only};
        swim_jumped = movement.controls(input, character, now).swim_jumped;
        movement.step({}, kStepMs, character, kFloor, input.follow_swim);
        return swim;
    }
};

} // namespace

TEST_CASE("follow swimming: stock and enhanced match the leader's depth while moving forward") {
    for (const bool profiled : {false, true}) {
        CAPTURE(profiled);
        SwimmingFollower swimmer(profiled);
        // The leader has a different avatar height. The follower started by looking down.
        const auto level = swimmer.tick(80.0f, 20.0f);
        REQUIRE(swimmer.moving_forward);
        CHECK_FALSE(level.vertical_only);
        CHECK(level.pitch == 0.0f);
        for (int frame = 0; frame < 90; ++frame) swimmer.tick(80.0f, 20.0f);
        CHECK(swimmer.movement.body.feet.x > 1.0f);
        CHECK(swimmer.depth() == 20.0f);
        CHECK(swimmer.movement.body.camera_angle == -64.0f);

        CHECK(swimmer.tick(80.0f, 8.0f).pitch < 0.0f);
        for (int frame = 0; frame < 120; ++frame) swimmer.tick(80.0f, 8.0f);
        const float after_dive = swimmer.depth();
        CHECK(after_dive < 19.5f);
        CHECK(after_dive > 8.0f);

        CHECK(swimmer.tick(80.0f, 24.0f).pitch > 0.0f);
        for (int frame = 0; frame < 120; ++frame) swimmer.tick(80.0f, 24.0f);
        CHECK(swimmer.depth() > after_dive + 0.5f);
        CHECK(swimmer.depth() < 24.0f);
    }
}

TEST_CASE("follow swimming: stock and enhanced correct depth inside the horizontal stop distance") {
    for (const bool profiled : {false, true}) {
        CAPTURE(profiled);
        SwimmingFollower swimmer(profiled);
        swimmer.tick(8.0f, 20.0f);
        CHECK_FALSE(swimmer.moving_forward);
        for (const float leader_depth : {28.0f, 12.0f}) {
            CAPTURE(leader_depth);
            const auto correction = swimmer.tick(8.0f, leader_depth);
            REQUIRE(correction.vertical_only);
            CHECK(swimmer.movement.autorun() == 0);
            for (int frame = 0; frame < 320; ++frame) swimmer.tick(8.0f, leader_depth);
            CHECK_FALSE(swimmer.tick(8.0f, leader_depth).vertical_only);
            CHECK_FALSE(swimmer.moving_forward);
            CHECK(std::abs(swimmer.depth() - leader_depth) <= 1.0f);
            CHECK(swimmer.movement.body.feet.x == 0.0f);
            CHECK(swimmer.movement.body.feet.y == 0.0f);
        }
    }
}

TEST_CASE("follow swimming: surface jumps use follow steering and preserve in-place correction") {
    for (const bool profiled : {false, true}) {
        CAPTURE(profiled);
        SwimmingFollower swimmer(profiled);
        swimmer.movement.body.camera_angle = 64.0f;
        swimmer.movement.swim_surface_clear = true;
        swimmer.tick(80.0f, 20.0f);
        CHECK_FALSE(swimmer.swim_jumped);
        CHECK(swimmer.depth() == 20.0f);

        SwimmingFollower vertical(profiled);
        vertical.movement.body.camera_angle = 64.0f;
        vertical.movement.swim_surface_clear = true;
        CHECK(vertical.tick(8.0f, 28.0f).vertical_only);
        CHECK_FALSE(vertical.swim_jumped);
        CHECK(vertical.movement.body.feet.x == 0.0f);

        swimmer.movement.body.camera_angle = -64.0f;
        swimmer.tick(80.0f, 30.0f);
        CHECK(swimmer.swim_jumped);
    }
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
