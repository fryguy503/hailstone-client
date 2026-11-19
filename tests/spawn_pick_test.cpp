// app/spawn_pick.h: a spawn's actor in the world click and in the nameplate's scene depth, a body-less skeleton
// (race 127's IVM) among them.
#include <doctest/doctest.h>

#include <vector>

#include "app/nameplate_input.h"
#include "app/spawn_pick.h"
#include "game/clicks.h"

using namespace hailstone;

namespace {

// A size-6 spawn standing 2.5 units beside a click segment along +X: inside the sphere a spawn without a model
// gets (half its size), but farther than the reach a drawn model of radius 0 is posed within.
constexpr float kSpawnSize = 6.0f;
constexpr float kOldSphereRadius = 3.0f;
constexpr float kClickOffset = 2.5f;
constexpr float kSpawnAhead = 10.0f;
constexpr float kSegmentLength = 100.0f;

const game::PickSegment kSegment{{0.0f, 0.0f, 0.0f}, {kSegmentLength, 0.0f, 0.0f}};
const glm::vec3 kSpawnAt{kSpawnAhead, kClickOffset, 0.0f};

// A spawn the actor renderer drew (with a model of `radius` about its feet) or not, posed or not; its pose has no
// triangles, as a bare IVM's dressed mesh has none.
app::SpawnPickSource source(bool bodiless, bool drawn, bool posed, float drawn_radius = 0.0f) {
    app::SpawnPickSource s;
    s.position = kSpawnAt;
    s.size = kSpawnSize;
    s.bodiless = bodiless;
    s.bounds = [drawn, drawn_radius](glm::vec3& origin, float& radius) {
        origin = kSpawnAt;
        radius = drawn_radius;
        return drawn;
    };
    s.posed = [posed](game::PickActor& actor, const glm::vec3& origin) {
        if (!posed) return false;
        actor.position = origin;
        actor.triangles = [](const game::PickTriangleSink&) {};
        return true;
    };
    return s;
}

int clicked(const std::vector<game::PickActor>& actors, int exclude = -1) {
    game::PickScene scene;
    scene.actors = actors;
    scene.exclude_a = exclude;
    return game::pick_actor(kSegment, scene, game::kClickRequired, game::kClickSkip).actor;
}

game::PickActor clickable() {
    game::PickActor a;
    a.mask = game::kClickRequired;
    return a;
}

} // namespace

TEST_CASE("spawn pick: a bare IVM keeps the old click sphere, drawn or past the actor clip") {
    for (bool drawn : {true, false}) {
        CAPTURE(drawn);
        std::vector<game::PickActor> actors;
        int exclude = -1;
        REQUIRE(app::add_world_click_spawn(actors, exclude, clickable(), source(true, drawn, true), kSegment, false));
        REQUIRE(actors.size() == 1);
        CHECK(actors.front().sphere_radius == doctest::Approx(kOldSphereRadius));
        CHECK(actors.front().mask == game::kClickRequired); // the caller's mask is kept
        CHECK(exclude == -1);
        CHECK(clicked(actors) == 0);
    }
    // A drawn model with a body is posed instead, and one of radius 0 is out of reach 2.5 away.
    std::vector<game::PickActor> actors;
    int exclude = -1;
    CHECK_FALSE(app::add_world_click_spawn(actors, exclude, clickable(), source(false, true, true), kSegment, false));
    CHECK(actors.empty());
}

TEST_CASE("spawn pick: exclude_view still leaves out the player's own IVM") {
    std::vector<game::PickActor> actors;
    int exclude = -1;
    // Another spawn first, so the index is not trivially 0.
    REQUIRE(app::add_world_click_spawn(actors, exclude, clickable(), source(false, false, false), kSegment, false));
    REQUIRE(app::add_world_click_spawn(actors, exclude, clickable(), source(true, true, true), kSegment, true));
    REQUIRE(actors.size() == 2);
    CHECK(exclude == 1);
    CHECK(clicked(actors) == 0);
    CHECK(clicked(actors, exclude) == 0); // the other spawn, not the excluded one
    std::vector<game::PickActor> alone(actors.begin() + 1, actors.end());
    CHECK(clicked(alone) == 0);
    CHECK(clicked(alone, 0) == -1);
}

TEST_CASE("spawn pick: a bare or clipped IVM never hides a nameplate behind it") {
    const glm::vec3 forward(1.0f, 0.0f, 0.0f);
    const auto depth = [&](const app::SpawnPickSource& s, bool added) {
        std::vector<game::PickActor> actors;
        CHECK(app::add_nameplate_depth_spawn(actors, s, kSegment) == added);
        game::PickScene scene;
        scene.actors = actors;
        return app::nameplate_scene_depth(kSegment, scene, forward);
    };
    // Bare and drawn: only its posed items count, and it holds none.
    CHECK_FALSE(depth(source(true, true, true, kOldSphereRadius), true));
    // Past the actor clip, or not posed yet: nothing of it is drawn.
    CHECK_FALSE(depth(source(true, false, false), false));
    CHECK_FALSE(depth(source(true, true, false, kOldSphereRadius), false));
    // A spawn with a body keeps its conservative sphere in both cases, as before.
    CHECK(depth(source(false, false, false), true));
    CHECK(depth(source(false, true, false, kOldSphereRadius), true));
}
