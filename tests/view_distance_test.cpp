#include <doctest/doctest.h>

#include <cmath>

#include "game/view_distance.h"

using hailstone::game::compute_view_distance;
using hailstone::game::ZoneFogInput;

TEST_CASE("Actor clip: min((ActorClipPlane x 5 + 25) x 2, Yon)") {
    using hailstone::game::actor_clip_distance;
    CHECK(hailstone::game::kDefaultActorClipPlane == 50);
    CHECK(actor_clip_distance(50, 2400.0f) == 550.0f); // the default ActorClipPlane
    CHECK(actor_clip_distance(0, 2400.0f) == 50.0f);
    CHECK(actor_clip_distance(99, 2400.0f) == 1040.0f);
    CHECK(actor_clip_distance(99, 600.0f) == 600.0f); // never past the far plane
}

TEST_CASE("A spawn with an actor is never a placeholder box, even past the actor clip") {
    using hailstone::game::spawn_draw;
    using hailstone::game::SpawnDraw;
    // Nektulos at night: fog end 400, the default actor clip 550. Will-o-wisps (race 69, WIL) at 460..600 away
    // were drawn as boxes when the actor renderer culled them at the fog end + 50.
    const float clip = hailstone::game::actor_clip_distance(50, 1500.0f);
    CHECK(spawn_draw(true, true, 30.0f, clip) == SpawnDraw::Model);
    CHECK(spawn_draw(true, true, 476.0f, clip) == SpawnDraw::Model); // past the fog end, inside the clip: drawn
    CHECK(spawn_draw(true, true, 591.0f, clip) == SpawnDraw::Clipped);
    CHECK(spawn_draw(true, true, 5000.0f, 0.0f) == SpawnDraw::Model); // no clip (the viewers)
    CHECK(spawn_draw(true, false, 30.0f, clip) == SpawnDraw::Empty);  // an actor whose model failed to build
    CHECK(spawn_draw(true, false, 900.0f, clip) == SpawnDraw::Empty);
    CHECK(spawn_draw(false, false, 30.0f, clip) == SpawnDraw::Box);   // no actor, not even HUM / HUF
}

TEST_CASE("No server fog falls back to 500/600 grey and a 2400 maxclip") {
    auto v = compute_view_distance(ZoneFogInput{}, 20);
    CHECK(v.fog_color.r == doctest::Approx(192.0 / 255.0));
    CHECK(v.fog_start == 500.0f);
    CHECK(v.fog_end == 600.0f);
    // minclip floor 50, maxclip = fog_maxclip * 4 = 2400; ClipPlane 20 reaches maxclip.
    CHECK(v.far_plane == doctest::Approx(2400.0));
}

TEST_CASE("ClipPlane scales between minclip and maxclip in 5% steps") {
    ZoneFogInput z;
    z.fog_minclip = 100.0f;
    z.fog_maxclip = 800.0f;
    z.minclip = 200.0f;
    z.maxclip = 1200.0f;
    CHECK(compute_view_distance(z, 0).far_plane == doctest::Approx(200.0));
    CHECK(compute_view_distance(z, 15).far_plane == doctest::Approx(950.0));
    CHECK(compute_view_distance(z, 20).far_plane == doctest::Approx(1200.0));
    CHECK(compute_view_distance(z, 99).far_plane == doctest::Approx(1200.0));
}

TEST_CASE("Clip floors: minclip at least 50, maxclip above minclip") {
    ZoneFogInput z;
    z.fog_minclip = 10.0f;
    z.fog_maxclip = 300.0f;
    z.minclip = 10.0f;
    z.maxclip = 40.0f; // below 51: fog_maxclip * 4
    CHECK(compute_view_distance(z, 20).far_plane == doctest::Approx(1200.0));

    z.minclip = 900.0f;
    z.maxclip = 600.0f; // minclip >= maxclip: maxclip = minclip * 4
    CHECK(compute_view_distance(z, 20).far_plane == doctest::Approx(3600.0));
}

TEST_CASE("Fog end stays inside the far plane") {
    ZoneFogInput z;
    z.fog_minclip = 400.0f;
    z.fog_maxclip = 5000.0f;
    z.minclip = 100.0f;
    z.maxclip = 1000.0f;
    auto v = compute_view_distance(z, 20);
    CHECK(v.fog_end < v.far_plane);
    CHECK(v.fog_start <= v.fog_end);
}

TEST_CASE("Camera FOV: 90 units give 63.28 degrees across with square pixels below an aspect of 2") {
    using hailstone::game::camera_fov;
    constexpr double kDeg = 180.0 / 3.14159265358979323846;
    auto f43 = camera_fov(90.0f, 1024, 768);
    CHECK(f43.h == 90.0f);
    CHECK(f43.v == doctest::Approx(67.5));
    CHECK(2.0 * std::atan(f43.tan_half_x) * kDeg == doctest::Approx(63.28125).epsilon(1e-5));
    CHECK(2.0 * std::atan(f43.tan_half_y) * kDeg == doctest::Approx(49.6).epsilon(1e-3));
    auto f169 = camera_fov(90.0f, 1920, 1080);
    CHECK(2.0 * std::atan(f169.tan_half_y) * kDeg == doctest::Approx(38.2).epsilon(1e-3));
    CHECK(f169.tan_half_x / f169.tan_half_y == doctest::Approx(16.0 / 9.0));
}

TEST_CASE("Camera FOV: at an aspect of 2 or more the vertical angle is fixed and h is widened") {
    using hailstone::game::camera_fov;
    auto f = camera_fov(90.0f, 2560, 1080);
    CHECK(f.v == doctest::Approx(90.0 * 0.7501876));
    // h = 2 atan(tan(v/2) * aspect) in 512ths of a circle.
    double v = 90.0 * 0.7501876 * 3.14159265358979323846 / 256.0;
    double h = 2.0 * std::atan(std::tan(v * 0.5) * 2560.0 / 1080.0) * 256.0 / 3.14159265358979323846;
    CHECK(f.h == doctest::Approx(h).epsilon(1e-4));
    CHECK(camera_fov(150.0f, 4000, 1000).h == 200.0f); // capped at 200 units
}

TEST_CASE("SetFog: the far-plane margin is 15% of the span from fog start to far") {
    using hailstone::game::set_fog_distances;
    auto f = set_fog_distances(400.0f, 5000.0f, 1000.0f, true);
    CHECK(f.x == 400.0f);
    CHECK(f.y == doctest::Approx(1000.0 - 0.15 * 600.0));
    // Within 0.01 of far counts as reaching it.
    CHECK(set_fog_distances(100.0f, 999.995f, 1000.0f, true).y == doctest::Approx(1000.0 - 0.15 * 900.0));
    // Start at far: 15% of far.
    CHECK(set_fog_distances(1000.0f, 1200.0f, 1000.0f, true).y == doctest::Approx(850.0));
    // Below far: untouched.
    CHECK(set_fog_distances(100.0f, 900.0f, 1000.0f, true).y == 900.0f);
}

TEST_CASE("SetFog: no margin with fog off, in region types 5..10 or in rain") {
    using hailstone::game::set_fog_distances;
    CHECK(set_fog_distances(400.0f, 5000.0f, 1000.0f, false).y == 5000.0f);
    CHECK(set_fog_distances(400.0f, 5000.0f, 1000.0f, true, {.special_region = true}).y == 5000.0f);
    CHECK(set_fog_distances(400.0f, 5000.0f, 1000.0f, true, {.raining = true}).y == 5000.0f);
}

TEST_CASE("SetFog: start and end fixes") {
    using hailstone::game::set_fog_distances;
    // Both below 0.1: Yon - 50 and Yon - 1, then the margin from start = Yon - 50.
    auto z = set_fog_distances(0.05f, 0.0f, 1000.0f, false);
    CHECK(z.x == 950.0f);
    CHECK(z.y == 999.0f);
    auto zm = set_fog_distances(0.05f, 0.0f, 1000.0f, true);
    CHECK(zm.y == 999.0f); // 999 is not within 0.01 of 1000
    // Negative start clamps to 0; end below start becomes start + 50.
    auto n = set_fog_distances(-20.0f, 10.0f, 1000.0f, true);
    CHECK(n.x == 0.0f);
    CHECK(n.y == 10.0f);
    auto e = set_fog_distances(300.0f, 200.0f, 1000.0f, true);
    CHECK(e.x == 300.0f);
    CHECK(e.y == 350.0f);
}
