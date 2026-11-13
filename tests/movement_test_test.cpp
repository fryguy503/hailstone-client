#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <tuple>

#include "app/movement_test.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {

auto key_state(const app::MovementKeys& k) {
    return std::tie(k.forward, k.back, k.turn_left, k.turn_right, k.strafe_left, k.strafe_right, k.jump,
                    k.mouse_look, k.move_down, k.both_buttons, k.camera_index, k.camera.pan, k.camera.pitch,
                    k.camera.distance, k.camera.heading, k.camera_active);
}

struct ObserveFixture {
    test::ScratchDir scratch{"movement_observe"};
    GameDir game{scratch.path()};
    net::GameConnection conn{game, {}};
    app::PlayerMovement movement;
    app::MovementKeys keys;
    float yaw = 0.25f, pitch = 0.5f;

    ObserveFixture() {
        movement.reset({7.0f, 11.0f, 3.0f}, yaw);
        conn.player_status_mut().stand_state = net::kStandStanding;
    }

    void drive(app::MovementTest& observer, float seconds) {
        observer.drive(seconds, keys, yaw, pitch, movement, conn, nullptr, nullptr,
                       movement.controller().body.feet);
    }

    app::PlayerMovement::Frame update(std::uint32_t now) {
        return movement.update({}, yaw, pitch, 16.0f, now, conn, nullptr, 3.75f, nullptr);
    }
};

} // namespace

TEST_CASE("movement observation fixes orientation and traces without injecting keys or moving the body") {
    ObserveFixture f;
    app::MovementTest observer("observe:90:25");
    REQUIRE(observer.active());
    f.keys.back = f.keys.turn_right = f.keys.strafe_left = f.keys.jump = f.keys.mouse_look = true;
    f.keys.both_buttons = f.keys.camera_active = true;
    f.keys.camera_index = 4;
    f.keys.camera = {2.0f, 0.3f, 8.0f, 0.4f};
    const auto before = f.keys;
    f.movement.push({0.1f, -0.2f, 0.3f});
    const auto feet = f.movement.controller().body.feet;
    const auto velocity = f.movement.controller().body.velocity;
    f.drive(observer, 12.0f);
    CHECK(f.yaw == doctest::Approx(1.57079632679));
    CHECK(f.pitch == 0.0f);
    CHECK((key_state(f.keys) == key_state(before)));
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(f.movement.controller().body.feet[axis] == feet[axis]);
        CHECK(f.movement.controller().body.velocity[axis] == velocity[axis]);
    }
    CHECK_FALSE(f.update(10000).trace.empty());

    // Once the duration expires, normal input owns the orientation and trace collection ends.
    f.yaw = 0.7f;
    f.pitch = -0.2f;
    f.drive(observer, 25.0f);
    CHECK(f.yaw == 0.7f);
    CHECK(f.pitch == -0.2f);
    CHECK((key_state(f.keys) == key_state(before)));
    CHECK(f.update(10016).trace.empty());
    f.drive(observer, 26.0f);
    CHECK(f.yaw == 0.7f);
    CHECK(f.pitch == -0.2f);
}

TEST_CASE("movement observation defaults to 25 seconds and accepts the bounded duration endpoints") {
    for (const auto& spec : {std::string("observe:-90"), std::string("observe:-90:120")}) {
        CAPTURE(spec);
        ObserveFixture f;
        app::MovementTest observer(spec);
        REQUIRE(observer.active());
        const float end = spec == "observe:-90" ? 25.0f : 120.0f;
        f.drive(observer, end - 0.01f);
        CHECK(f.yaw == doctest::Approx(-1.57079632679));
        CHECK_FALSE(f.keys.forward);
        CHECK_FALSE(f.keys.jump);
        f.yaw = 0.1f;
        f.pitch = 0.2f;
        f.drive(observer, end);
        CHECK(f.yaw == 0.1f);
        CHECK(f.pitch == 0.2f);
    }
    ObserveFixture f;
    app::MovementTest zero("observe:180:0");
    REQUIRE(zero.active());
    f.drive(zero, 0.0f);
    CHECK(f.yaw == 0.25f);
    CHECK(f.pitch == 0.5f);
    CHECK(f.update(10000).trace.empty());
}

TEST_CASE("movement observation keeps huge finite angles safe through the normal movement update") {
    for (const std::string degrees : {"1e38", "-1e38", "3.4028234663852886e38", "-3.4028234663852886e38"}) {
        CAPTURE(degrees);
        ObserveFixture f;
        app::MovementTest observer("observe:" + degrees + ":25");
        REQUIRE(observer.active());
        f.keys.forward = f.keys.strafe_right = f.keys.jump = true;
        const auto before = f.keys;
        f.movement.push({0.1f, -0.2f, 0.3f});
        const auto feet = f.movement.controller().body.feet;
        const auto velocity = f.movement.controller().body.velocity;
        f.drive(observer, 1.0f);
        CHECK(std::isfinite(f.yaw));
        CHECK(std::fabs(f.yaw) <= 6.2831855f);
        CHECK((key_state(f.keys) == key_state(before)));
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(f.movement.controller().body.feet[axis] == feet[axis]);
            CHECK(f.movement.controller().body.velocity[axis] == velocity[axis]);
        }
        const auto frame = f.update(10000);
        CHECK(std::isfinite(frame.yaw));
        CHECK(std::isfinite(f.movement.controller().body.heading));
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(std::isfinite(frame.feet[axis]));
            CHECK(std::isfinite(f.movement.controller().body.velocity[axis]));
        }
    }
}

TEST_CASE("movement observation refuses invalid numeric inputs and any placement") {
    for (const std::string spec : {
             "observe", "observe:", "observe:bad", "observe:0:", "observe:nan", "observe:inf", "observe:-inf",
             "observe:1e100", "observe:0:nan", "observe:0:inf", "observe:0:-0.01", "observe:0:120.001",
             "observe:0:1:2", "observe:0@1,2,3", "observe:0:25@bad", "observe:0:1@10,20,30+trace"}) {
        CAPTURE(spec);
        ObserveFixture f;
        app::MovementTest observer(spec);
        CHECK_FALSE(observer.active());
        const auto feet = f.movement.controller().body.feet;
        f.drive(observer, 1.0f);
        CHECK(f.yaw == 0.25f);
        CHECK(f.pitch == 0.5f);
        CHECK_FALSE(f.keys.forward);
        CHECK_FALSE(f.keys.jump);
        for (int axis = 0; axis < 3; ++axis) CHECK(f.movement.controller().body.feet[axis] == feet[axis]);
    }
}
