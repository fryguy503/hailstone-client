// Offline whole-player movement in the native capture's Guild Lobby hallway. Only the start pose and player
// dimensions are observations; inputs and profile are authored here, and all geometry comes from EQ_GAME_DIR.
// This checks the production collision/floor/region adapter, not rendering, networking, dynamic server actors,
// or equivalence to the native trajectory. There is deliberately no captured endpoint oracle.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "app/player_movement.h"
#include "app/zone_move_world.h"
#include "assets/zone.h"
#include "core/game_dir.h"
#include "core/step_clock.h"
#include "game/coords.h"
#include "game_files.h"
#include "net/game_connection.h"
#include "net/zone_packets.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace mech = game::mechanics;

namespace {

// Forward every query to the actual adapter. A second, read-only mesh query identifies the normal of a body hit,
// which MoveWorld::Sweep intentionally does not expose; counts must be positive rather than a fixed native count.
class ObservedWorld final : public app::MoveWorld {
public:
    ObservedWorld(const game::CollisionMesh& mesh, const assets::ZoneRegions& regions, float radius)
        : mesh_(mesh), actual_(&mesh, nullptr, &regions), radius_(radius) {
        actual_.apply_rules(std::nullopt, {}, false);
    }
    bool collision_loaded() const override { return actual_.collision_loaded(); }
    std::optional<std::pair<glm::vec3, glm::vec3>> world_box() const override { return actual_.world_box(); }
    Sweep sweep(const glm::vec3& from, const glm::vec3& to, float radius) const override {
        const auto hit = actual_.sweep(from, to, radius);
        if (hit.collided && std::fabs(radius - radius_) < 1e-5f) {
            auto probe = game::SphereSweep::make(from, to, radius);
            if (mesh_.sweep_sphere_hit(probe) >= 0 && std::fabs(probe.normal.z) < 0.02f &&
                glm::length(glm::vec2(to - from)) > 1e-5f)
                ++body_wall_hits;
        }
        return hit;
    }
    Sweep world_sweep(const glm::vec3& from, const glm::vec3& to, float radius) const override {
        return actual_.world_sweep(from, to, radius);
    }
    bool in_world(const glm::vec3& point) const override { return actual_.in_world(point); }
    std::optional<float> floor_below(const glm::vec3& point, float step_up) const override {
        return actual_.floor_below(point, step_up);
    }
    float floor_sweep(const glm::vec3& from, const glm::vec3& to, float radius, std::uint32_t* vehicle = nullptr,
                      std::vector<FloorSwitchHit>* switches = nullptr) const override {
        return actual_.floor_sweep(from, to, radius, vehicle, switches);
    }
    std::optional<float> ceiling_above(const glm::vec3& point) const override { return actual_.ceiling_above(point); }
    std::vector<std::uint8_t> touch_overlap(const glm::vec3& point, float radius) const override {
        return actual_.touch_overlap(point, radius);
    }

    mutable int body_wall_hits = 0;

private:
    const game::CollisionMesh& mesh_;
    app::ZoneMoveWorld actual_;
    float radius_;
};

} // namespace

TEST_CASE("Player movement: Guild Lobby hallway basic capture acceptance" * doctest::skip(!test::game_dir())) {
    const assets::Zone zone = assets::load_zone(*test::game_dir(), "guildlobby");
    auto mesh = game::CollisionMesh::build(zone);
    mesh.set_solid_ladders(true);
    constexpr float kHeight = 3.3125f;
    const float radius = mech::sweep_radius({kHeight, 7});
    ObservedWorld world(mesh, zone.regions, radius);

    // Isolate connection data from the player's files. This is an unencumbered synthetic warrior, not the
    // capture's account/profile. No connection is started, so no packets leave this process.
    test::ScratchDir scratch("movement_capture_acceptance");
    GameDir fixture(scratch.path());
    net::GameConnection conn(fixture, {});
    net::NewZone info;
    info.zone_id = 344;
    info.zone_short_name = "guildlobby";
    info.gravity = mech::kDefaultGravity;
    conn.receive_zone_packet_for_test(net::encode(info));
    net::PlayerProfile profile;
    profile.race = 7;
    profile.class_ = 1;
    profile.level = 60;
    profile.str = profile.sta = profile.agi = profile.dex = profile.int_ = profile.wis = profile.cha = 75;
    conn.set_profile_for_test(profile);
    auto& status = conn.player_status_mut();
    status.hp = status.max_hp = 1000;
    status.endurance = status.max_endurance = 1000;
    status.level = 60;
    status.stand_state = net::kStandStanding;
    auto& state = conn.world_for_test();
    state.player_id = 1;
    auto& player = state.spawns[1];
    player.id = 1;
    player.race = 7;
    player.run_speed = 0.7f;
    player.walk_speed = 0.3f;
    player.heights.avatar_height = kHeight;
    player.heights.view_height = 6.5f;
    player.heights.jump_strength = mech::kJumpStrength;
    player.heights.swim_strength = mech::kSwimStrength;
    player.stand_state = net::kStandStanding;

    // Observed native X/Y use server axes (coords.h); Z is the spawn's, AvatarHeight above the feet.
    const glm::vec3 start = game::server_to_world({19.919815f, -41.670567f, 4.810117f}) - glm::vec3(0, 0, kHeight);
    const float yaw = game::heading_to_yaw(490.22113f);
    player.position = start + glm::vec3(0, 0, kHeight);
    player.heading = game::yaw_to_heading(yaw);
    app::PlayerMovement movement;
    movement.set_collision_radius(radius);
    movement.set_race(7);
    movement.reset(start, yaw);

    // 1 s idle, four 700 ms movement pulses with 600 ms releases, jump, duck, restore: 9.2 s.
    // Jump is a press edge in MovementKeys; crouch notifications mirror ClientApp's accepted posture changes.
    core::StepClock clock;
    constexpr std::uint64_t kStartMs = 10'000;
    bool jump_pressed = false, ducked = false, restored = false, airborne = false, landed_after_jump = false;
    bool observed_crouch = false;
    int jump_count = 0, frames = 0;
    float travel = 0.0f;
    for (std::uint64_t draw = kStartMs * 1000; draw <= (kStartMs + 10'200) * 1000; draw += 16'667) {
        for (const auto& step : clock.advance(draw)) {
            const std::uint64_t at = step.ms - kStartMs;
            CAPTURE(at);
            app::MovementKeys keys;
            keys.forward = at >= 1000 && at < 1700;
            keys.back = at >= 2300 && at < 3000;
            keys.strafe_left = at >= 3600 && at < 4300;
            keys.strafe_right = at >= 4900 && at < 5600;
            if (!jump_pressed && at >= 6200) {
                keys.jump = true;
                jump_pressed = true;
            }
            if (!ducked && at >= 7200) {
                status.stand_state = net::kStandCrouching;
                ducked = true;
            }
            if (!restored && at >= 8200) {
                status.stand_state = net::kStandStanding;
                restored = true;
            }
            const auto frame = movement.update(keys, yaw, 0.0f, 1000.0f / 60.0f,
                static_cast<std::uint32_t>(step.ms), conn, &zone.regions, kHeight, &world, nullptr, step.elapsed_ms);
            if (frame.stand_state) status.stand_state = static_cast<std::uint8_t>(*frame.stand_state);
            player.stand_state = status.stand_state;
            if (at >= 7200 && at < 8200 && status.stand_state == net::kStandCrouching) observed_crouch = true;
            player.position = frame.feet + glm::vec3(0, 0, kHeight);
            const auto& body = movement.controller().body;
            for (int axis = 0; axis < 3; ++axis) {
                REQUIRE(std::isfinite(frame.feet[axis]));
                REQUIRE(std::isfinite(body.velocity[axis]));
            }
            REQUIRE(std::isfinite(body.speed_run));
            const auto floor = movement.floor_height();
            REQUIRE(floor.has_value());
            REQUIRE(std::isfinite(*floor));
            REQUIRE(*floor > -1e20f); // the native no-floor sentinel is -1e27
            CHECK(world.in_world(player.position));
            CHECK(movement.collision_state().possibly_stuck == 0);
            jump_count += frame.jumped;
            if (jump_count && frame.feet.z > *floor + 0.1f) airborne = true;
            if (airborne && frame.landed) landed_after_jump = true;
            travel = std::max(travel, glm::length(glm::vec2(frame.feet - start)));
            ++frames;
        }
    }
    CHECK(frames > 600);
    CHECK(travel > 1.0f);
    CHECK(world.body_wall_hits > 0);
    CHECK(jump_count == 1);
    CHECK(airborne);
    CHECK(landed_after_jump);
    CHECK(ducked);
    CHECK(observed_crouch);
    CHECK(restored);
    CHECK(status.stand_state == net::kStandStanding);
    CHECK(std::fabs(movement.controller().body.speed_run) < 1e-5f);
    CHECK(glm::length(movement.controller().body.velocity) < 1e-5f);
}
