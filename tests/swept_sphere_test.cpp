// The original's swept-sphere wall collision (game/swept_sphere.h) and
// DoCollisionMovement (game/mechanics/collision_movement.h) driven over
// synthetic walls and real zones: the sphere stops short of walls, slides
// along them at an angle, passable and back faces do not block.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "assets/zone.h"
#include "game/collision.h"
#include "game/mechanics/collision_movement.h"
#include "game/sphere_sweep.h"
#include "game/swept_sphere.h"
#include "game_files.h"

using namespace hailstone::game;
namespace mech = hailstone::game::mechanics;

TEST_CASE("A sphere parallel to a triangle's plane has no vertex edge-region crossing") {
    auto check = []<class F>() {
        using V = sweep::Vec3<F>;
        const V a(0, 0, 0), b(10, 0, 0), c(0, 10, 0), n(0, 0, -1);
        const V delta(4, 4, 0);
        const F inverse = F(1) / std::sqrt(F(32));
        // The sphere crosses the triangle plane but starts outside both edges meeting at A. Its centre path
        // never crosses that plane, so the edge-region selector refuses it before choosing a capsule.
        const auto outside = sweep::sphere_triangle<F>(V(-2, -2, 0), delta, inverse, F(1), F(1), n, a, b, c);
        CHECK_FALSE(outside.hit);
        // A sphere already inside the triangle still overlaps immediately; the parallel rejection is local to
        // the vertex's edge-region case.
        const auto inside = sweep::sphere_triangle<F>(V(1, 1, 0), delta, inverse, F(1), F(1), n, a, b, c);
        CHECK(inside.hit);
        CHECK(inside.t == F(0));
    };
    check.operator()<float>();
    check.operator()<double>();
}

TEST_CASE("An edge of no length is met as the sphere around its end") {
    auto check = []<class F>(sweep::Vec3<F> e) {
        using V = sweep::Vec3<F>;
        const V s(0, 0, 0), delta(10, 0, 0);
        const auto hit = sweep::sphere_edge<F>(V(-5, 0, 0), delta, F(1), F(1), s, e);
        REQUIRE(hit.hit);
        CHECK(hit.t == doctest::Approx(0.4)); // the near side of the sphere of radius 1 around s
        CHECK(hit.normal.x == doctest::Approx(-1.0));
        // A path that passes farther than the radius from it misses.
        CHECK_FALSE(sweep::sphere_edge<F>(V(-5, 2, 0), delta, F(1), F(1), s, e).hit);
        // And a move that ends short of it.
        CHECK_FALSE(sweep::sphere_edge<F>(V(-5, 0, 0), delta, F(0.3), F(1), s, e).hit);
    };
    check.operator()<float>(sweep::Vec3<float>(0));
    check.operator()<double>(sweep::Vec3<double>(0));
    // A length whose square underflows a float (1e-46 is below its smallest denormal).
    check.operator()<float>(sweep::Vec3<float>(1e-23f, 0, 0));
}

TEST_CASE("A path tangent to an edge's cylinder touches it, its far end included") {
    auto check = []<class F>() {
        using V = sweep::Vec3<F>;
        // The edge up the z axis from 0 to 10; a sphere of radius 1 moving along x at y = 1 grazes the cylinder at
        // t = 0.5 (a discriminant of exactly 0).
        const V s(0, 0, 0), e(0, 0, 10), delta(10, 0, 0);
        for (F z : {F(5), F(10)}) { // halfway up, and level with the far end (u = 1)
            CAPTURE(z);
            const auto hit = sweep::sphere_edge<F>(V(-5, 1, z), delta, F(1), F(1), s, e);
            REQUIRE(hit.hit);
            CHECK(hit.t == F(0.5));
            CHECK(hit.normal == V(0, 1, 0));
        }
        // Just clear of the cylinder: no hit.
        CHECK_FALSE(sweep::sphere_edge<F>(V(-5, F(1.01), 5), delta, F(1), F(1), s, e).hit);
    };
    check.operator()<float>();
    check.operator()<double>();
}

TEST_CASE("A capsule keeps native candidate order for a centre already inside its side") {
    auto check = []<class F>() {
        using V = sweep::Vec3<F>;
        const V a(0, 0, 0), b(10, 0, 0), c(0, 10, 0), n(0, 0, -1);
        auto hit = [&](V delta) {
            return sweep::sphere_triangle<F>(V(2, F(-0.5), 0), delta, F(1) / glm::length(delta),
                                              F(1), F(1), n, a, b, c);
        };
        // The cylinder exit is generated before a start-cap root behind the move. Only one root is within the
        // move, so the selector retains that first cylinder candidate instead of sorting the negative cap first.
        const auto side = hit(V(4, -1, 0));
        REQUIRE(side.hit);
        CHECK(side.t == doctest::Approx(0.5));
        // Along the edge toward its start, the initial start-sphere crossing is inside the cylinder. The capsule
        // boundary is the far side of that sphere, at x=-sqrt(1-0.5^2), rather than its near side at +sqrt(...).
        const auto start = hit(V(-12, 0, 0));
        REQUIRE(start.hit);
        CHECK(start.t == doctest::Approx((F(2) + std::sqrt(F(0.75))) / F(12)));
        // The reverse direction retains the original's asymmetric first-candidate selection, clamped to zero.
        const auto far = hit(V(12, 0, 0));
        REQUIRE(far.hit);
        CHECK(far.t == F(0));
    };
    check.operator()<float>();
    check.operator()<double>();
}

namespace {

// A wall in the plane x = 10 from y -100..100, z -50..50, wound so that its
// record normal faces -x (toward a sphere coming from x < 10).
std::vector<CollisionTriangle> wall(std::uint8_t flags = 0) {
    glm::vec3 a(10, -100, -50), b(10, 100, -50), c(10, 100, 50), d(10, -100, 50);
    std::vector<CollisionTriangle> t{CollisionTriangle::make(a, b, c, flags), CollisionTriangle::make(a, c, d, flags)};
    return t;
}

// The local player's answers with the zone mesh as the world (no doors, no
// BSP): what app/player_movement.cpp answers, minus the floors.
struct Walker final : mech::CollisionQueries {
    const CollisionMesh& mesh;
    mech::CollisionSpawn spawn;
    glm::vec3 actor{0.0f};
    float radius = 2.0f, lift = 2.0f;
    std::uint32_t now = 0;
    explicit Walker(const CollisionMesh& m) : mesh(m) {}

    mech::Vec3f actor_location() override { return {actor.x, actor.y, actor.z}; }
    bool zone_collision_loaded() override { return true; }
    bool scene_box(mech::Vec3f& lo, mech::Vec3f& hi) override {
        if (!mesh.bounds()) return false;
        lo = {mesh.bounds()->first.x, mesh.bounds()->first.y, mesh.bounds()->first.z};
        hi = {mesh.bounds()->second.x, mesh.bounds()->second.y, mesh.bounds()->second.z};
        return true;
    }
    void zone_client() override {}
    bool mounted() override { return false; }
    bool is_vehicle_race() override { return false; }
    bool has_property(std::uint32_t) override { return false; }
    double height() override { return lift; }
    double collision_radius() override { return radius; }
    bool moving_state() override { return false; }
    bool rider() override { return false; }
    void set_rider_stand_time(std::uint32_t) override {}
    std::uint32_t display_time() override { return now; }
    bool sweep_request(const std::array<float, 7>&) override { return true; }
    bool is_controlled() override { return true; }
    void sweep(const mech::Vec3f& from, const mech::Vec3f& to, bool, bool, mech::SweepResult& out) override {
        SphereSweep s = SphereSweep::make({from[0], from[1], from[2]}, {to[0], to[1], to[2]}, radius);
        if (s.valid()) mesh.sweep_sphere(s);
        SweepOutput o = sweep_output(s);
        out.position = {o.position.x, o.position.y, o.position.z};
        out.skitter_dir = {o.skitter_dir.x, o.skitter_dir.y, o.skitter_dir.z};
        out.skitter_dist = o.skitter_dist;
    }
    void no_collide() override {}
    void move_without_collision(const mech::Vec3f& t, mech::Vec3f& o) override { o = t; }
    bool in_scene_region(const mech::Vec3f&) override { return true; }
    bool can_i_fit_here(const mech::Vec3f&, std::uint32_t) override { return true; }
    void clear_stuck(std::uint32_t) override {}
    bool check_stuck() override { return false; }
    bool collision_test(const mech::CollisionTestInfo&) override { return false; }
    void set_actor_location(const mech::Vec3f& p) override { actor = {p[0], p[1], p[2]}; }
    bool is_player_spawn() override { return true; }
    void after_place() override {}
    bool can_swim_jump_now() override { return false; }
    bool on_ladder() override { return false; }
    void finish_move() override { mech::keep_in_scene(spawn, false, {}, {}); }
    void after_move(const mech::Vec3f&, const mech::Vec3f&) override {}

    // One frame: the body moved by `step` from the actor; returns where it ended.
    glm::vec3 move(const glm::vec3& step) {
        glm::vec3 to = actor + step;
        spawn.physics = {to.x, to.y, to.z, step.x, step.y, step.z, 1.0f, 0, 0, 0, 0, 0};
        mech::Vec3f target{to.x, to.y, to.z};
        ++now;
        mech::do_collision_movement(spawn, target, *this);
        return {spawn.physics[0], spawn.physics[1], spawn.physics[2]};
    }
    void place(const glm::vec3& p) {
        actor = p;
        spawn = {};
        spawn.last_good = {p.x, p.y, p.z};
    }
};

} // namespace

TEST_CASE("the swept sphere stops 0.001 short of a wall and skitters along it") {
    SphereSweep s = SphereSweep::make({0, 0, 0}, {20, 0, 0}, 2.0f);
    for (const auto& t : wall()) collide_triangle(s, t);
    REQUIRE(s.collided);
    CHECK(s.t == doctest::Approx(8.0f / 20.0f));
    CHECK(s.normal.x == doctest::Approx(-1.0f));
    SweepOutput o = sweep_output(s);
    CHECK(o.position.x == doctest::Approx(8.0f - 0.001f));
    // Head on: u - n (u.n) is zero, so ComputeSkitter leaves the distance 0.
    CHECK(o.skitter_dist == 0.0f);

    // At 45 degrees: the skitter runs along the wall (plus 0.01 n).
    SphereSweep a = SphereSweep::make({0, 0, 0}, {20, 20, 0}, 2.0f);
    for (const auto& t : wall()) collide_triangle(a, t);
    REQUIRE(a.collided);
    SweepOutput ao = sweep_output(a);
    CHECK(ao.skitter_dir.y > 0.99f);
    CHECK(ao.skitter_dir.x < 0.0f); // the 0.01 n pushes off the wall
    float length = std::sqrt(800.0f);
    CHECK(ao.skitter_dist == doctest::Approx((1.0f - a.t) * length * std::sqrt(0.5f)).epsilon(1e-4));
}

TEST_CASE("the sphere sweep's face, edge and vertex cases") {
    namespace sw = hailstone::game::sweep;
    // The triangle A (0,0,0) B (10,0,0) C (5,10,0) in the plane z = 0; its record normal faces -z.
    const glm::vec3 a(0, 0, 0), b(10, 0, 0), c(5, 10, 0), n(0, 0, -1);
    auto hit = [&](glm::vec3 from, glm::vec3 delta, float r) {
        return sw::sphere_triangle<float>(from, delta, 1.0f / glm::length(delta), 1.0f, r, n, a, b, c);
    };
    // Face: the sphere's lowest point reaches the plane at z = -1.
    auto face = hit({5, 3, -5}, {0, 0, 8}, 1.0f);
    REQUIRE(face.hit);
    CHECK(face.t == doctest::Approx(0.5f));
    CHECK(face.normal.z == doctest::Approx(-1.0f));
    // Edge: the centre path passes 0.5 outside edge AB and meets its cylinder where z = -sqrt(1 - 0.25).
    auto edge = hit({5, -0.5f, -5}, {0, 0, 5}, 1.0f);
    REQUIRE(edge.hit);
    CHECK(edge.t == doctest::Approx((5.0f - std::sqrt(0.75f)) / 5.0f));
    CHECK(edge.normal.y == doctest::Approx(-0.5f));
    // Vertex: beyond B and outside both its edges, the path meets the sphere around B.
    auto vertex = hit({10.6f, -0.5f, -5}, {0, 0, 5}, 1.0f);
    REQUIRE(vertex.hit);
    CHECK(vertex.t == doctest::Approx((5.0f - std::sqrt(1.0f - 0.36f - 0.25f)) / 5.0f));
    // Missing it altogether.
    CHECK_FALSE(hit({5, -3, -5}, {0, 0, 8}, 1.0f).hit);
    // Already across the plane over the face: a hit at once, the record normal.
    auto across = hit({5, 3, -0.5f}, {1, 0, 0}, 1.0f);
    REQUIRE(across.hit);
    CHECK(across.t == 0.0f);
    // A centre already inside an edge's capsule: touching at once when it leaves the cylinder within the move,
    // nothing when it does not; moving out through the far end touches at once, through the start end when it leaves.
    CHECK(hit({5, -0.5f, -0.3f}, {0, 10, 0}, 1.0f).t == 0.0f);
    CHECK_FALSE(hit({5, -0.5f, -0.3f}, {0, 1, 0}, 1.0f).hit);
    CHECK(hit({5, -0.3f, -0.3f}, {6, 0.3f, 0}, 1.0f).t == 0.0f);
    auto out_start = hit({5, -0.3f, -0.3f}, {-6, 0.3f, 0}, 1.0f);
    REQUIRE(out_start.hit);
    CHECK(out_start.t == doctest::Approx(0.9923f).epsilon(1e-3));
    // A sphere holding the edge's start vertex, moving toward it: a hit at once, its contact assumed.
    auto held = hit({0.5f, -0.5f, -0.3f}, {-1, 1, 0}, 1.0f);
    CHECK(held.hit);
    CHECK(held.contact_assumed);
    // The double version agrees.
    auto d = sw::sphere_triangle<double>({5, -0.5, -5}, {0, 0, 5}, 0.2, 1.0, 1.0, {0, 0, -1}, {0, 0, 0}, {10, 0, 0},
                                         {5, 10, 0});
    CHECK(d.t == doctest::Approx(static_cast<double>(edge.t)));
}

TEST_CASE("the clamp point and the skitter keep NaN and huge inputs as the original does") {
    namespace sw = hailstone::game::sweep;
    // A NaN path gives a NaN stop, not the start.
    glm::vec3 nan(std::nanf(""), 0, 0);
    CHECK(std::isnan(sw::clamp_point<float>({1, 2, 3}, nan, 0.5f).x));
    // Lengths of vectors near the float range do not overflow.
    glm::vec3 stop = sw::clamp_point<float>({0, 2e37f, 0}, {0, -6e37f, 0}, 1.0f);
    CHECK(stop.y == doctest::Approx(-4e37f));
    // Moving straight along the normal: no slide, not a slide along n.
    sw::Skitter<float> k = sw::skitter<float>({0, 0, -1}, {0, 0, -5}, 0.5f, {0, 0, -1});
    CHECK(k.distance == 0.0f);
    CHECK(k.direction == glm::vec3(0.0f));
}

TEST_CASE("passable polygons and back faces do not block the sweep") {
    SphereSweep p = SphereSweep::make({0, 0, 0}, {20, 0, 0}, 2.0f);
    for (const auto& t : wall(kTrianglePassable)) collide_triangle(p, t);
    CHECK_FALSE(p.collided);
    // Transparent ones do.
    SphereSweep q = SphereSweep::make({0, 0, 0}, {20, 0, 0}, 2.0f);
    for (const auto& t : wall(kTriangleTransparent)) collide_triangle(q, t);
    CHECK(q.collided);
    // From behind (moving along the normal): skipped.
    SphereSweep b = SphereSweep::make({20, 0, 0}, {0, 0, 0}, 2.0f);
    for (const auto& t : wall()) collide_triangle(b, t);
    CHECK_FALSE(b.collided);
    // A zero-length sweep is an overlap test, back faces included.
    SphereSweep o = SphereSweep::make({11, 0, 0}, {11, 0, 0}, 2.0f);
    for (const auto& t : wall()) collide_triangle(o, t);
    CHECK(o.collided);
    CHECK(o.t == 0.0f);
}

TEST_CASE("the floor and ceiling queries' floor-only mode skips the terrain's walls") {
    // A sphere already overlapping the wall (centre 1 from it, radius 2), swept up as GetCeilingHeight does:
    // the movement sweep stops at once (t = 0, the ceiling would be its own top); the floor-only one passes.
    SphereSweep m = SphereSweep::make({9, 0, 0}, {9, 0, 100}, 2.0f);
    for (const auto& t : wall()) collide_triangle(m, t);
    CHECK(m.collided);
    CHECK(m.t == 0.0f);
    SphereSweep f = SphereSweep::make({9, 0, 0}, {9, 0, 100}, 2.0f);
    f.floor_only = true;
    for (const auto& t : wall()) collide_triangle(f, t);
    CHECK_FALSE(f.collided);
    // A placed object's wall is an actor in the original, whose test does not ask the mode: it still blocks.
    SphereSweep o = SphereSweep::make({9, 0, 0}, {9, 0, 100}, 2.0f);
    o.floor_only = true;
    for (const auto& t : wall(kTriangleObject)) collide_triangle(o, t);
    CHECK(o.collided);
    // A sloped face (n.z != 0) still stops it.
    auto roof = CollisionTriangle::make({-50, -50, 10}, {50, -50, 10.5f}, {0, 50, 10}, 0);
    SphereSweep r = SphereSweep::make({0, 0, 0}, {0, 0, 100}, 2.0f);
    r.floor_only = true;
    bool hit = collide_triangle(r, roof) || collide_triangle(r, CollisionTriangle::make({-50, -50, 10}, {0, 50, 10}, {50, -50, 10.5f}, 0));
    CHECK(hit);
}

TEST_CASE("DoCollisionMovement slides a player along a wall walked into at an angle") {
    auto mesh = CollisionMesh::from_triangles(wall());
    Walker w(mesh);
    w.place({0, 0, 0});
    glm::vec3 p{0.0f};
    for (int i = 0; i < 40; ++i) p = w.move(glm::vec3(0.7f, 0.7f, 0.0f));
    // Stopped short of the wall (radius 2), still moving along it.
    CHECK(p.x < 8.0f);
    CHECK(p.x > 7.9f);
    CHECK(p.y > 25.0f);
    CHECK(w.spawn.possibly_stuck == 0);
    // Straight into it: held off the wall, not through.
    for (int i = 0; i < 10; ++i) p = w.move(glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(p.x < 8.0f);
}

namespace {
// Walks from `start` along `dir` until a wall stops the walk, then along
// `dir` turned 45 degrees toward the wall's side: the walk must keep moving
// (slide) and never cross the wall (the sphere never overlaps it).
void walk_into_walls(const CollisionMesh& mesh, glm::vec3 start, float radius, int* slid, int* tried) {
    for (int k = 0; k < 16; ++k) {
        float a = static_cast<float>(k) * 0.3926991f;
        glm::vec3 dir(std::cos(a), std::sin(a), 0.0f);
        Walker w(mesh);
        w.radius = radius;
        w.lift = std::max(1.9f, radius);
        w.place(start);
        glm::vec3 p = start;
        int still = 0;
        for (int i = 0; i < 300 && still < 3; ++i) {
            glm::vec3 q = w.move(dir);
            still = glm::length(q - p) < 0.5f ? still + 1 : 0;
            p = q;
        }
        if (still < 3) continue; // no wall within reach this way
        ++*tried;
        // Now at the wall: 45 degrees off the wall's normal (both sides tried).
        for (float turn : {0.7853982f, -0.7853982f}) {
            glm::vec3 d(std::cos(a + turn), std::sin(a + turn), 0.0f);
            Walker v(mesh);
            v.radius = radius;
            v.lift = std::max(1.9f, radius);
            v.place(p);
            glm::vec3 r = p;
            glm::vec3 lift(0, 0, std::max(1.9f, radius)); // the sphere's centre above the spawn's z
            for (int i = 0; i < 20; ++i) {
                glm::vec3 next = v.move(d * 0.7f);
                // Never through a front face: each frame's straight move of the centre is clear of them
                // (back faces never block the original's sweep).
                SphereSweep thin = SphereSweep::make(r + lift, next + lift, 0.01f);
                mesh.sweep_sphere(thin);
                CHECK_FALSE(thin.collided);
                r = next;
            }
            if (glm::length(glm::vec2(r - p)) > 3.0f) {
                ++*slid;
                break;
            }
        }
    }
}
} // namespace

TEST_CASE("walls of a WLD and an EQG zone block and slide" * doctest::skip(!hailstone::test::game_dir())) {
    struct Case {
        const char* zone;
        bool eqg;
        glm::vec3 feet;
    };
    for (Case c : {Case{"qeynos", false, {180, -220, 0}}, Case{"qeynos", false, {60, -280, 0}},
                   Case{"guildhall", true, {-34.65f, -8.39f, 0}}}) {
        auto zone = c.eqg ? hailstone::assets::load_eqg_zone(*hailstone::test::game_dir(), c.zone)
                          : hailstone::assets::load_classic_zone(*hailstone::test::game_dir(), c.zone);
        auto mesh = CollisionMesh::build(zone);
        glm::vec3 feet = c.feet;
        if (auto f = mesh.floor_below(feet + glm::vec3(0, 0, 5.0f), 0.0f)) feet.z = *f;
        // A human: the spawn's z 3.75 (AvatarHeight) above the feet; the collision volume radius
        // 3.75 x 0.5 x 0.95; the centre 1.9 (PlayerClient vf94) above the spawn's z.
        float radius = mech::sweep_radius({3.75f, 1});
        glm::vec3 spawn_z = feet + glm::vec3(0, 0, 3.75f);
        int slid = 0, tried = 0;
        walk_into_walls(mesh, spawn_z, radius, &slid, &tried);
        MESSAGE(std::string(c.zone) << " at (" << c.feet.x << ", " << c.feet.y << "): " << tried << " walls reached, slid along " << slid);
        CHECK(tried > 0);
        CHECK(slid > 0);
    }
}

TEST_CASE("a type 1 volume's radius is the model's +0x38 x the model scale x COFactor") {
    // The model definitions' +0x38 read from the shipped archives: HUM's 5.5696 (classic), 5.1781 (Luclin); a human's scale 1.2.
    CHECK(mech::model_sweep_radius(5.5696f, 1.2f) == doctest::Approx(6.6835).epsilon(1e-4));
    CHECK(mech::model_sweep_radius(5.1781f, 1.2f) == doctest::Approx(6.2137).epsilon(1e-4));
    CHECK(mech::model_sweep_radius(5.5696f, 1.2f, 0.5f) == doctest::Approx(3.3418).epsilon(1e-4));
    // 0x5062a0's player value.
    CHECK(mech::sweep_radius({3.75f, 1}) == doctest::Approx(1.78125));
}

TEST_CASE("player sweep radii agree with the two measured native type-2 forms within one float ULP") {
    // Scalar observations only, no captured geometry. Native local-player probes selected type 2 with
    // CW 0x0e7f (53-bit, toward zero); Hailstone's formula uses nearest even. Do not force its last bit
    // to match a different rounding mode. Each value stayed unchanged through standing/crouched samples.
    struct Case {
        float height;
        std::uint32_t race;
        float native_radius;
    };
    for (const auto& c : {Case{1.875f, 12, 0.8906249403953552f}, Case{3.3125f, 7, 1.5734374523162842f}}) {
        CAPTURE(c.height);
        CAPTURE(c.race);
        const float radius = mech::sweep_radius({c.height, c.race});
        CHECK(radius >= std::nextafter(c.native_radius, 0.0f));
        CHECK(radius <= std::nextafter(c.native_radius, 2.0f));
    }
}

TEST_CASE("South Qeynos' doorways pass the player radius but not the model radius" *
          doctest::skip(!hailstone::test::game_dir())) {
    // The doorway west of the docks: a 2-thick wall at x 26..28, open for y -95.97..-85.97 and z 0..10. A human
    // walks through it in the original; a sphere swept straight through its middle, its centre vf94 = max(1.9, r)
    // above the spawn's z (the feet + AvatarHeight 3.75), must not hit anything for the human to pass.
    auto zone = hailstone::assets::load_classic_zone(*hailstone::test::game_dir(), "qeynos");
    auto mesh = CollisionMesh::build(zone);
    auto passes = [&](float r) {
        glm::vec3 p(45.0f, -90.97f, 3.75f + std::max(1.9f, r));
        for (int i = 0; i < 70; ++i, p.x -= 0.5f) {
            SphereSweep s = SphereSweep::make(p, p - glm::vec3(0.5f, 0, 0), r);
            mesh.sweep_sphere(s);
            if (s.collided) return false;
        }
        return true;
    };
    CHECK(passes(mech::sweep_radius({3.75f, 1})));
    // 6.68 (classic HUM) and 6.21 (Luclin HUM) are wider than the 10-unit opening (and taller, lifted by r):
    // blocked whichever way the walls are wound. Native probes instead selected type 2 for two player forms.
    CHECK_FALSE(passes(static_cast<float>(mech::model_sweep_radius(5.5696f, 1.2f))));
    CHECK_FALSE(passes(static_cast<float>(mech::model_sweep_radius(5.1781f, 1.2f))));
    CHECK_FALSE(passes(4.9f)); // narrow enough across, but its top (3.75 + 2 x 4.9) is above the lintel
    CHECK(passes(3.1f));       // the largest that clears both: r < 5 across, 3.75 + 2r <= 10 up
}
