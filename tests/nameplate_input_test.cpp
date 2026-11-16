#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include "app/nameplate_input.h"
#include "game/mouse_look.h"

TEST_CASE("nameplate clicks: a held gesture cannot target a replacement actor or owner") {
    hailstone::app::NameplateGesture gesture;
    int first_session = 0, next_session = 0;
    const auto press = [&] { gesture.begin(7, {30, 20}, 10, 100, &first_session, 3); };
    press();
    CHECK_FALSE(gesture.release({30, 20}, 7, false, 10, 101, &first_session, 3));
    press();
    CHECK_FALSE(gesture.release({30, 20}, 7, false, 11, 100, &first_session, 3));
    press();
    CHECK_FALSE(gesture.release({30, 20}, 7, false, 10, 100, &next_session, 3));
    press();
    CHECK_FALSE(gesture.release({30, 20}, 7, false, 10, 100, &first_session, 4));
    press();
    CHECK(gesture.release({31, 20}, 7, false, 10, 100, &first_session, 3) == 7);
    press();
    gesture.update({30, 20}, true);
    CHECK_FALSE(gesture.release({30, 20}, 7, false, 10, 100, &first_session, 3));
}

TEST_CASE("nameplate clicks: topmost eligible plate wins and only a short same-plate release selects") {
    using hailstone::app::NameplateHit;
    using hailstone::app::NameplateGesture;
    std::vector<NameplateHit> hits{{1, {10, 10, 100, 40}, {}, false}, {2, {20, 10, 110, 40}, {}, true}};
    auto visible = [](const NameplateHit& h) { return h.through_walls; };
    CHECK(hailstone::app::pick_nameplate(hits, {30, 20}, visible) == 2);
    CHECK_FALSE(hailstone::app::pick_nameplate(hits, {15, 20}, visible));
    CHECK_FALSE(hailstone::app::pick_nameplate(hits, {500, 500}, visible));
    NameplateGesture gesture;
    gesture.begin(2, {30, 20});
    CHECK(gesture.release({32, 21}, 2, false) == 2);
    CHECK_FALSE(gesture.id);
    gesture.begin(2, {30, 20});
    gesture.update({40, 20}, false);
    CHECK_FALSE(gesture.release({30, 20}, 2, false)); // returning after a drag is still cancelled
    gesture.begin(2, {30, 20});
    CHECK_FALSE(gesture.release({30, 20}, 1, false));
    gesture.begin(2, {30, 20});
    CHECK_FALSE(gesture.release({30, 20}, 2, true)); // UI capture, a modal or mouse look
    gesture.begin(2, {30, 20});
    gesture.clear();
    CHECK_FALSE(gesture.release({30, 20}, 2, false)); // world transition or actor removal
}

TEST_CASE("nameplate clicks: an uncaptured drag transfers to ordinary camera look without selecting") {
    hailstone::app::NameplateGesture gesture;
    hailstone::game::MouseLook look;
    gesture.begin(2, {30, 20});
    gesture.update({40, 20}, false);
    REQUIRE(gesture.transfer_drag(true, true));
    look.left_down({}, true, 100);
    CHECK(look.active);
    CHECK_FALSE(gesture.id);
    CHECK_FALSE(gesture.release({40, 20}, 2, false));
    look.left_up(500);
    CHECK_FALSE(look.active);
    gesture.begin(2, {30, 20});
    gesture.update({40, 20}, true);
    CHECK_FALSE(gesture.transfer_drag(true, true)); // UI/modal capture cannot turn into a camera drag later
    gesture.update({45, 20}, false);
    CHECK_FALSE(gesture.transfer_drag(true, true));
    gesture.begin(2, {30, 20});
    gesture.update({40, 20}, false);
    CHECK_FALSE(gesture.transfer_drag(true, false)); // isometric left drags retain their existing behavior
}

TEST_CASE("nameplate clicks: native click-through masks and self-view exclusions remain authoritative") {
    hailstone::game::MaskSpawn npc;
    npc.type = 1;
    CHECK(hailstone::app::nameplate_mouse_eligible(npc, 0, false, 6));
    npc.click_through = true;
    CHECK_FALSE(hailstone::app::nameplate_mouse_eligible(npc, 0, false, 6));
    npc.click_through = false;
    npc.mercenary = true;
    CHECK_FALSE(hailstone::app::nameplate_mouse_eligible(npc, 4, false, 6));
    npc.mercenary = false;
    npc.owner_is_player = true;
    CHECK_FALSE(hailstone::app::nameplate_mouse_eligible(npc, 2, false, 6));
    CHECK(hailstone::app::nameplate_mouse_eligible(npc, 0, false, 6));
    CHECK_FALSE(hailstone::app::nameplate_mouse_eligible({}, 0, true, 0));
    CHECK_FALSE(hailstone::app::nameplate_mouse_eligible({}, 1, true, 6));
    CHECK(hailstone::app::nameplate_mouse_eligible({}, 0, true, 6));
}

TEST_CASE("nameplate clicks: viewport mapping uses the displayed scene projection including jitter") {
    glm::mat4 inverse(1.0f);
    inverse[3] = {0, 0, 100, 1};
    const glm::vec4 view(100, 50, 400, 200);
    auto center = hailstone::app::nameplate_click_ray(inverse, view, {}, {300, 150});
    REQUIRE(center);
    CHECK(center->dir == glm::vec3(0, 0, 100));
    const auto corner = hailstone::app::nameplate_click_ray(inverse, view, {}, {100, 50});
    REQUIRE(corner);
    CHECK(corner->dir == glm::vec3(-1, 1, 100));
    inverse[3].x = 0.125f;
    center = hailstone::app::nameplate_click_ray(inverse, view, {}, {300, 150});
    REQUIRE(center);
    CHECK(center->dir.x == 0.125f);
    CHECK_FALSE(hailstone::app::nameplate_click_ray(inverse, view, {}, {99, 150}));
    CHECK_FALSE(hailstone::app::nameplate_click_ray(glm::mat4(0), view, {}, {300, 150}));
}

TEST_CASE("nameplate clicks: a pillar or visible foreground actor blocks the clicked pixel even with a clear head ray") {
    using hailstone::game::PickTriangle;
    using hailstone::game::PickTriangleSink;
    using hailstone::game::PickSegment;
    using hailstone::game::PickScene;
    const PickSegment pixel{{0, 0, 0}, {100, 50, 0}};
    const PickSegment head{{0, 0, 0}, {30, 0, 0}};
    PickScene scene;
    scene.world = [](const PickSegment&, const PickTriangleSink& sink) {
        sink(PickTriangle{{10, 4, -2}, {10, 6, -2}, {10, 6, 2}, {}, 0});
        sink(PickTriangle{{10, 4, -2}, {10, 6, 2}, {10, 4, 2}, {}, 0});
    };
    CHECK_FALSE(hailstone::app::nameplate_scene_depth(head, scene, {1, 0, 0}));
    const auto pillar = hailstone::app::nameplate_scene_depth(pixel, scene, {1, 0, 0});
    REQUIRE(pillar);
    CHECK(*pillar == doctest::Approx(10));
    CHECK_FALSE(hailstone::app::nameplate_depth_visible(30, pillar));
    scene.world = {};
    std::vector<hailstone::game::PickActor> actors(1);
    actors[0].mask = hailstone::game::kPickMaskClickThrough; // unclickable actors still occlude the scene
    actors[0].bound_center = {20, 10, 0};
    actors[0].bound_radius = 4;
    actors[0].triangles = [](const PickTriangleSink& sink) {
        sink(PickTriangle{{20, 8, -2}, {20, 12, -2}, {20, 12, 2}, {}, 0});
        sink(PickTriangle{{20, 8, -2}, {20, 12, 2}, {20, 8, 2}, {}, 0});
    };
    scene.actors = actors;
    const auto actor = hailstone::app::nameplate_scene_depth(pixel, scene, {1, 0, 0});
    REQUIRE(actor);
    CHECK(*actor == doctest::Approx(20)); // axial depth, not the longer slanted-ray distance
    CHECK_FALSE(hailstone::app::nameplate_depth_visible(30, actor));
    actors[0].invisible = true;
    CHECK_FALSE(hailstone::app::nameplate_scene_depth(pixel, scene, {1, 0, 0}));
    CHECK(hailstone::app::nameplate_depth_visible(30, 27.0f));
    CHECK_FALSE(hailstone::app::nameplate_depth_visible(30, 26.9f));
    CHECK_FALSE(hailstone::app::nameplate_depth_visible(30, std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("nameplate clicks: a right-button join preserves normal two-button camera state without a plate click") {
    hailstone::app::NameplateGesture gesture;
    hailstone::game::MouseLook look;
    gesture.begin(2, {30, 20});
    look.left_down({}, true, 100); // replay held plate press before ordinary right_down
    gesture.clear();
    look.right_down({}, 120);
    CHECK(look.active);
    CHECK(look.both_buttons);
    CHECK_FALSE(gesture.release({30, 20}, 2, false));
    look.right_up(200);
    look.left_up(220);
    CHECK_FALSE(look.active);
    CHECK_FALSE(look.both_buttons);
}
