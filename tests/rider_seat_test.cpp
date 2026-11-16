#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/character.h"
#include "game_files.h"
#include "game/coords.h"
#include "game/world_state.h"
#include "render/actor_renderer.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {

bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) { return glm::length(a - b) < eps; }

// A root and one child bone `name` at (1, 0, 2), at rest.
CharacterModel model_with(std::string code, std::vector<std::string> names) {
    CharacterModel m;
    m.code = std::move(code);
    m.bone_names = std::move(names);
    m.parents.assign(m.bone_names.size(), 0);
    m.parents[0] = -1;
    for (std::uint32_t i = 0; i < m.bone_names.size(); ++i) m.order.push_back(i);
    AnimationClip rest;
    rest.tracks.push_back({BonePose{}});
    for (std::size_t i = 1; i < m.bone_names.size(); ++i)
        rest.tracks.push_back({BonePose{glm::quat(1, 0, 0, 0), glm::vec3(1, 0, 2), glm::vec3(1.0f)}});
    m.clips.emplace("", rest);
    return m;
}

} // namespace

TEST_CASE("a mount's rider dag: <rider>_MOUNT01, else CHEST03_MOUNT01, else <code>RIDER") {
    // SetBone (EQGraphicsDX9 @0x10041140) keeps the dag's bone when the name is not found, so CHEST03_MOUNT01
    // (set first by PlayerClient_AttachRiderToMount) stays when there is no "<rider>_MOUNT01".
    CHECK(model_with("HRS", {"ROOT_BONE", "CHEST03_MOUNT01", "HUM_MOUNT01"}).mount_point_bone("HUM") == 2);
    CHECK(model_with("HRS", {"ROOT_BONE", "CHEST03_MOUNT01", "HUM_MOUNT01"}).mount_point_bone("OGM") == 1);
    CHECK(model_with("HSM", {"HSM_TRACK", "HSMRIDER_TRACK"}).mount_point_bone("OGM") == 1);
    CHECK(model_with("HSM", {"HSM_TRACK", "HSMNECK_TRACK"}).mount_point_bone("OGM") == -1);
}

TEST_CASE("a rider is drawn in its mount's bone frame, without its own scale") {
    auto horse = model_with("HRS", {"ROOT_BONE", "CHEST03_MOUNT01"});
    auto wld_rider = model_with("OGM", {"OGM_TRACK"});
    auto eqg_rider = model_with("DKM", {"ROOT_BONE"});
    eqg_rider.eqg = true;

    render::ActorDraw mount;
    mount.model = &horse;
    mount.clip = horse.clip("");
    mount.position = glm::vec3(10.0f, 0.0f, 0.0f);
    mount.scale = 2.0f;
    glm::mat4 dag;
    REQUIRE(render::ActorRenderer::bone_world(mount, 1, dag));
    CHECK(near(glm::vec3(dag[3]), {12.0f, 0.0f, 4.0f}));

    // The bone's world matrix becomes the rider model's world matrix (CHierarchicalActor @0x10041680, model
    // vf+0x10 @0x1004a6a0): the mount's scale 2 applies, the rider's own 1.76 does not.
    render::ActorDraw rider;
    rider.model = &wld_rider;
    rider.clip = wld_rider.clip("");
    rider.scale = 1.76f;
    REQUIRE(render::ActorRenderer::seat_rider(mount, rider, nullptr));
    CHECK(rider.attached);
    glm::mat4 root;
    REQUIRE(render::ActorRenderer::bone_world(rider, 0, root));
    for (int c = 0; c < 4; ++c) CHECK(near(glm::vec3(root[c]), glm::vec3(dag[c])));

    // An EQG rider's ROOT_BONE keys come down by its ROffset (3.125 without moddat.ini) in its model units, which
    // the mount's scale then sizes.
    render::ActorDraw eqg;
    eqg.model = &eqg_rider;
    eqg.clip = eqg_rider.clip("");
    eqg.scale = 1.2f;
    REQUIRE(render::ActorRenderer::seat_rider(mount, eqg, nullptr));
    REQUIRE(render::ActorRenderer::bone_world(eqg, 0, root));
    CHECK(near(glm::vec3(root[3]), {12.0f, 0.0f, 4.0f - 3.125f * 2.0f}));

    // A mount without the dag leaves the rider unseated.
    auto plain = model_with("HSM", {"HSM_TRACK", "HSMNECK_TRACK"});
    render::ActorDraw other = mount;
    other.model = &plain;
    render::ActorDraw standing;
    standing.model = &wld_rider;
    standing.clip = wld_rider.clip("");
    CHECK_FALSE(render::ActorRenderer::seat_rider(other, standing, nullptr));
    CHECK_FALSE(standing.attached);
}

TEST_CASE("a current render pose uses the display transform and model origin even when the actor is hidden") {
    game::Spawn spawn;
    spawn.position = {100, 200, 300};
    spawn.heading = 384;
    spawn.display_valid = true;
    spawn.display_position = {10, 20, 30};
    spawn.display_heading = 128;
    spawn.heights.model_scale = 2.0f;
    auto classic = model_with("HUM", {"HUM_TRACK"});
    const auto pose = render::ActorRenderer::render_pose(spawn, classic, nullptr);
    const auto transform = render::ActorRenderer::draw_matrix(pose);
    CHECK(near(glm::vec3(transform[3]), {10, 20, 30}));
    CHECK(near(glm::vec3(transform[0]), {0, 2, 0}));
    CHECK(pose.yaw == doctest::Approx(game::heading_to_yaw(128)));

    auto eqg = model_with("DKM", {"ROOT_BONE"});
    eqg.eqg = true;
    const auto eqg_pose = render::ActorRenderer::render_pose(spawn, eqg, nullptr);
    CHECK(near(glm::vec3(render::ActorRenderer::draw_matrix(eqg_pose)[3]), {10, 20, 23.75f}));
    CHECK(eqg_pose.scale == 2.0f);
}

TEST_CASE("a hidden first-person rider uses the drawn mount seat rather than its independent spawn transform") {
    auto horse = model_with("HRS", {"ROOT_BONE", "CHEST03_MOUNT01"});
    auto rider_model = model_with("DKM", {"ROOT_BONE"});
    rider_model.eqg = true;
    game::Spawn mount_spawn;
    mount_spawn.display_valid = true;
    mount_spawn.display_position = {10, 20, 30};
    mount_spawn.display_heading = 128;
    mount_spawn.heights.model_scale = 2;
    auto mount = render::ActorRenderer::render_pose(mount_spawn, horse, nullptr);
    mount.clip = horse.clip(""); // the mount's actual prepared animation pose
    game::Spawn rider_spawn;
    rider_spawn.display_valid = true;
    rider_spawn.display_position = {100, 200, 300};
    rider_spawn.display_heading = 384;
    rider_spawn.heights.model_scale = 6;
    auto hidden = render::ActorRenderer::render_pose(rider_spawn, rider_model, nullptr);
    auto visible = hidden;
    visible.clip = rider_model.clip("");
    REQUIRE(render::ActorRenderer::seat_rider(mount, hidden, nullptr));
    REQUIRE(render::ActorRenderer::seat_rider(mount, visible, nullptr));
    const auto hidden_transform = render::ActorRenderer::draw_matrix(hidden);
    const auto visible_transform = render::ActorRenderer::draw_matrix(visible);
    CHECK(near(glm::vec3(hidden_transform[3]), {10, 22, 27.75f}));
    CHECK(near(glm::vec3(hidden_transform[0]), {0, 2, 0}));
    for (int column = 0; column < 4; ++column)
        CHECK(near(glm::vec3(hidden_transform[column]), glm::vec3(visible_transform[column])));
}

TEST_CASE("the shipped mounts' rider dags" * doctest::skip(!hailstone::test::game_dir())) {
    auto lib = CharacterLibrary::load(*hailstone::test::game_dir(), "");
    // The Luclin horse names its rider dag after itself; the EQG mounts have CHEST03_MOUNT01.
    for (const char* code : {"HSM", "HSN"}) {
        auto m = lib.model(code);
        REQUIRE(m);
        int b = m->mount_point_bone("HUM");
        REQUIRE(b >= 0);
        CHECK(m->bone_names[static_cast<std::size_t>(b)] == std::string(code) + "RIDER_TRACK");
    }
    for (const char* code : {"HRS", "CWR", "UNM", "MWR"}) {
        auto m = lib.model(code);
        REQUIRE(m);
        int b = m->mount_point_bone("HUM");
        REQUIRE(b >= 0);
        CHECK(m->bone_names[static_cast<std::size_t>(b)] == "CHEST03_MOUNT01");
    }
}

TEST_CASE("a seated rider's top is its model's top in the mount's bone frame (its name plate's height)") {
    auto horse = model_with("HRS", {"ROOT_BONE", "CHEST03_MOUNT01"});
    auto ogre = model_with("OGM", {"OGM_TRACK"});
    ogre.bounds_max = glm::vec3(1.0f, 1.0f, 3.0f);
    render::ActorDraw mount;
    mount.model = &horse;
    mount.clip = horse.clip("");
    mount.position = glm::vec3(10.0f, 0.0f, 0.0f);
    mount.scale = 2.0f;
    render::ActorDraw rider;
    rider.model = &ogre;
    rider.clip = ogre.clip("");
    glm::vec3 top;
    CHECK_FALSE(render::ActorRenderer::rider_top(rider, top)); // standing: the spawn's own place gives it
    REQUIRE(render::ActorRenderer::seat_rider(mount, rider, nullptr));
    REQUIRE(render::ActorRenderer::rider_top(rider, top));
    CHECK(near(top, {12.0f, 0.0f, 4.0f + 3.0f * 2.0f})); // the seat (12, 0, 4), the top 3 at the mount's scale 2
}
