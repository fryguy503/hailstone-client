// A placement (zone-in, a teleport) stands the player on the ground under the placed spawn z, even when the spawn
// is only a little above it (app/player_movement.cpp: the placement's floor test runs before the first physics step,
// as the EQPlayer constructor's FloorHeight does).

#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <vector>

#include "app/player_movement.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {

// Flat floors at the given heights (the ground and, under it, another floor), nothing else.
class Floors final : public app::MoveWorld {
public:
    explicit Floors(std::vector<float> heights) : heights_(std::move(heights)) {
        std::sort(heights_.begin(), heights_.end(), std::greater<float>());
    }
    bool collision_loaded() const override { return true; }
    std::optional<std::pair<glm::vec3, glm::vec3>> world_box() const override { return std::nullopt; }
    Sweep sweep(const glm::vec3&, const glm::vec3& to, float) const override {
        Sweep s;
        s.out.position = to;
        return s;
    }
    Sweep world_sweep(const glm::vec3& from, const glm::vec3& to, float r) const override { return sweep(from, to, r); }
    bool in_world(const glm::vec3&) const override { return true; }
    std::optional<float> floor_below(const glm::vec3& p, float step_up) const override {
        for (float h : heights_)
            if (h <= p.z + step_up) return h;
        return std::nullopt;
    }
    // The sphere going down stops on the first floor its bottom is above.
    float floor_sweep(const glm::vec3& from, const glm::vec3& to, float r, std::uint32_t* = nullptr,
                      std::vector<FloorSwitchHit>* = nullptr) const override {
        for (float h : heights_)
            if (from.z - r >= h && to.z - r <= h) return h + r;
        return to.z;
    }
    std::optional<float> ceiling_above(const glm::vec3&) const override { return std::nullopt; }

private:
    std::vector<float> heights_;
};

constexpr float kAvatarHeight = 3.75f;

} // namespace

TEST_CASE("Player placement: a spawn just above the ground lands on it, not on the floor under it") {
    test::ScratchDir scratch("player_placement");
    GameDir game(scratch.path());
    net::GameConnection conn(game, {});
    // East Commonlands from West Freeport: the zone point's z puts the spawn 1.2 above the ground, the feet 2.5 under
    // it; a floor 8 units further down. One 100 ms physics step falls 1.58 units.
    const Floors world({0.0f, -8.4f});
    app::PlayerMovement m;
    m.reset(glm::vec3(10.0f, 20.0f, 1.2f - kAvatarHeight), 0.0f);
    app::MovementKeys keys;
    std::uint32_t now = 5000;
    auto frame = m.update(keys, 0.0f, 0.0f, 100.0f, now, conn, nullptr, kAvatarHeight, &world);
    CHECK(frame.feet.z == doctest::Approx(0.0f).epsilon(0.01));
    for (int i = 0; i < 20; ++i) {
        now += 100;
        frame = m.update(keys, 0.0f, 0.0f, 100.0f, now, conn, nullptr, kAvatarHeight, &world);
    }
    CHECK(frame.feet.z == doctest::Approx(0.0f).epsilon(0.01));
    REQUIRE(m.floor_height());
    CHECK(*m.floor_height() == doctest::Approx(0.0f).epsilon(0.01));

    SUBCASE("a placement later in the zone (a teleport, a height change) after a long frame") {
        m.place(glm::vec3(30.0f, 20.0f, 0.5f - kAvatarHeight));
        now += 400; // a slow frame behind the loading screen: a longer first step
        frame = m.update(keys, 0.0f, 0.0f, 100.0f, now, conn, nullptr, kAvatarHeight, &world);
        CHECK(frame.feet.z == doctest::Approx(0.0f).epsilon(0.01));
        now += 100;
        frame = m.update(keys, 0.0f, 0.0f, 100.0f, now, conn, nullptr, kAvatarHeight, &world);
        CHECK(frame.feet.z == doctest::Approx(0.0f).epsilon(0.01));
    }
}

TEST_CASE("Player placement: respawn reset clears old movement and fall state before the new floor test") {
    test::ScratchDir scratch("player_respawn_placement");
    GameDir game(scratch.path());
    net::GameConnection conn(game, {});
    const Floors world({0.0f, -8.4f});
    app::PlayerMovement m;
    m.reset(glm::vec3(10.0f, 20.0f, 100.0f), 0.0f);
    m.command(game::mechanics::kCmdForward, true, true);
    m.set_autorun(1);
    m.push(glm::vec3(2.0f, 3.0f, -4.0f));
    std::uint32_t now = 5000;
    (void)m.update({}, 0.0f, 0.0f, 16.0f, now, conn, nullptr, kAvatarHeight, &world);
    REQUIRE(m.moving_keys());
    REQUIRE(m.controller().body.velocity.z < 0.0f);

    const glm::vec3 destination(30.0f, 40.0f, 0.5f - kAvatarHeight);
    const float yaw = 1.0f;
    m.reset(destination, yaw);
    CHECK_FALSE(m.moving_keys());
    CHECK(m.controller().autorun() == 0);
    CHECK(m.controller().body.velocity == glm::vec3(0.0f));
    CHECK_FALSE(m.floor_height().has_value());
    // Time spent dead must not resume the old fall or carry a held forward command to the destination.
    now += 100000;
    auto frame = m.update({}, yaw, 0.0f, 16.0f, now, conn, nullptr, kAvatarHeight, &world);
    CHECK(frame.feet.x == doctest::Approx(destination.x));
    CHECK(frame.feet.y == doctest::Approx(destination.y));
    CHECK(frame.feet.z == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(frame.yaw == doctest::Approx(yaw));
    CHECK(frame.fall_damage.value_or(0) == 0);
    CHECK_FALSE(frame.jumped);
    now += 16;
    frame = m.update({}, yaw, 0.0f, 16.0f, now, conn, nullptr, kAvatarHeight, &world);
    CHECK(frame.feet.x == doctest::Approx(destination.x));
    CHECK(frame.feet.y == doctest::Approx(destination.y));
    CHECK(frame.fall_damage.value_or(0) == 0);
}
