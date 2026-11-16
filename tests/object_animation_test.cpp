// Placed objects that play their own animation (assets/object_animation.h): the loader's tracks, the sampler's keys,
// closing key and period, the moving parts' collision (game/animated_objects.h) and the static mesh without them.
// With EQ_GAME_DIR, Ssraeshza Temple's turning rings let a swimmer into the water shaft and out at the top.

#include <doctest/doctest.h>

#include <cmath>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "app/player_movement.h"
#include "app/world_geometry_queries.h"
#include "app/zone_move_world.h"
#include "assets/object_animation.h"
#include "assets/wld.h"
#include "assets/zone.h"
#include "game/animated_objects.h"
#include "game/collision.h"
#include "game/mechanics/collision_movement.h"
#include "game_files.h"
#include "net/game_connection.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::test::wld_fixtures;

namespace {

// A quarter turn about Z k times, as a packed key's w and z (/16384).
std::pair<std::int16_t, std::int16_t> turn(int quarters) {
    const float half = static_cast<float>(quarters) * 0.25f * glm::pi<float>();
    return {static_cast<std::int16_t>(std::lround(std::cos(half) * 16384.0f)),
            static_cast<std::int16_t>(std::lround(std::sin(half) * 16384.0f))};
}

// A two-node skeleton (a still root, a child carrying the mesh) whose child turns about Z through `keys` keys (a
// quarter turn each), the 0x13 flags and timing word as given.
struct Skeleton {
    WldBuilder b;
    std::int32_t skeleton = 0;
};
Skeleton turning_skeleton(int keys, std::uint32_t ref_flags, std::uint32_t timing) {
    Skeleton s;
    WldBuilder& b = s.b;
    auto list = b.add(0x31, b.str("RING_MP"), refs({0, 0, 0}));
    auto mesh_def = b.add(0x36, b.str("RING_DMSPRITEDEF"), mesh(list, false));
    ByteWriter mesh_ref;
    mesh_ref.le(mesh_def);
    mesh_ref.le(std::uint32_t{0});
    auto sprite = b.add(0x2d, 0, mesh_ref);
    auto track = [&](int count, std::uint32_t flags, std::uint32_t ms) {
        ByteWriter def;
        def.le(std::uint32_t{0x8});
        def.le(static_cast<std::uint32_t>(count));
        for (int k = 0; k < count; ++k) {
            const auto [w, z] = turn(k);
            for (std::int16_t v : {w, std::int16_t{0}, std::int16_t{0}, z, std::int16_t{0}, std::int16_t{0}, std::int16_t{0}})
                def.le(v);
            def.le(std::uint16_t{256});
        }
        auto d = b.add(0x12, 0, def);
        ByteWriter ref;
        ref.le(d);
        ref.le(flags);
        if (flags & 1) ref.le(ms);
        return b.add(0x13, 0, ref);
    };
    auto root = track(1, 4, 0);
    auto child = track(keys, ref_flags, timing);
    ByteWriter sk;
    sk.le(std::uint32_t{0});
    sk.le(std::uint32_t{2});
    sk.le(std::int32_t{0});
    for (auto [name, tr, spr, kid] : {std::tuple{b.str("ROOT"), root, 0, 1}, std::tuple{b.str("RING"), child, sprite, -1}}) {
        sk.le(name);
        sk.le(std::uint32_t{0});
        sk.le(tr);
        sk.le(static_cast<std::int32_t>(spr));
        sk.le(static_cast<std::uint32_t>(kid >= 0 ? 1 : 0));
        if (kid >= 0) sk.le(static_cast<std::int32_t>(kid));
    }
    s.skeleton = b.add(0x10, b.str("RING_HS_DEF"), sk);
    return s;
}

// Where `p` goes under `m`.
glm::vec3 apply(const glm::mat4& m, const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); }

void check_near(const glm::vec3& a, const glm::vec3& b) {
    CHECK(a.x == doctest::Approx(b.x).epsilon(1e-3));
    CHECK(a.y == doctest::Approx(b.y).epsilon(1e-3));
    CHECK(a.z == doctest::Approx(b.z).epsilon(1e-3));
}

// An object turning about its Z axis through four quarter turns, 100 ms apart (a 400 ms period), by hand.
assets::ObjectAnimation quarter_turns(std::size_t vertices) {
    assets::ObjectAnimation a;
    a.nodes.resize(2);
    a.nodes[0].keys.push_back({});
    a.nodes[1].parent = 0;
    a.nodes[1].interval_ms = 100;
    a.nodes[1].moves = true;
    for (int k = 0; k <= 4; ++k) {
        const float half = static_cast<float>(k) * 0.25f * glm::pi<float>();
        a.nodes[1].keys.push_back({glm::quat(std::cos(half), 0.0f, 0.0f, std::sin(half)), glm::vec3(0.0f), 1.0f});
    }
    a.rest_inverse.assign(2, glm::mat4(1.0f));
    a.vertex_node.assign(vertices, 1);
    a.period_ms = 400;
    return a;
}

// A slab 1 thick, 2 wide and 4 high at x 5..6, with outward triangles (WLD winding: clockwise from the front).
assets::Mesh slab() {
    assets::Mesh m;
    const glm::vec3 lo(5, -1, 0), hi(6, 1, 4);
    for (int i = 0; i < 8; ++i) {
        assets::Vertex v;
        v.position = {i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z};
        m.vertices.push_back(v);
    }
    // Each face's corners in a loop; both windings, so the slab blocks from either side.
    const std::uint32_t faces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (const auto& f : faces) {
        m.indices.insert(m.indices.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
        m.indices.insert(m.indices.end(), {f[0], f[2], f[1], f[0], f[3], f[2]});
    }
    m.materials.push_back(assets::Material{});
    m.parts.push_back({0, static_cast<std::uint32_t>(m.indices.size()), 0, true});
    return m;
}

// A zone of flat ground with one turning slab placed at the origin.
assets::Zone slab_zone() {
    assets::Zone z;
    assets::Mesh ground;
    for (glm::vec3 p : {glm::vec3(-50, -50, 0), glm::vec3(50, -50, 0), glm::vec3(50, 50, 0), glm::vec3(-50, 50, 0)}) {
        assets::Vertex v;
        v.position = p;
        ground.vertices.push_back(v);
    }
    ground.indices = {0, 2, 1, 0, 3, 2};
    ground.materials.push_back(assets::Material{});
    ground.parts.push_back({0, 6, 0, true});
    z.terrain.push_back(ground);
    z.objects["SLAB_ACTORDEF"] = slab();
    z.object_animations["SLAB_ACTORDEF"] = quarter_turns(8);
    assets::Placement p;
    p.actor = "SLAB_ACTORDEF";
    z.placements.push_back(p);
    return z;
}

bool blocked(const game::AnimatedObjects& a, const glm::vec3& from, const glm::vec3& to) {
    game::SphereSweep s = game::SphereSweep::make(from, to, 0.5f);
    return a.sweep_sphere(s) && s.t < 1.0f;
}

} // namespace

TEST_CASE("Object animation: a track's keys, its closing key and the set's period from the WLD") {
    // 0x13 flag 1 with a timing word of 100: four keys a quarter turn apart.
    auto s = turning_skeleton(4, 5, 100);
    auto w = wld::WldFile::parse(s.b.build());
    REQUIRE(w.errors().empty());
    const auto& sk = w.at(s.skeleton)->as<wld::Skeleton>();
    std::vector<std::uint16_t> vertex_node;
    auto mesh = wld::build_skeleton_pose(w, sk, "RING_ACTORDEF", &vertex_node);
    REQUIRE(mesh);
    CHECK(vertex_node == std::vector<std::uint16_t>(mesh->vertices.size(), 1));
    auto anim = wld::build_object_animation(w, sk);
    REQUIRE(anim);
    CHECK(anim->period_ms == 400);
    REQUIRE(anim->nodes.size() == 2);
    CHECK_FALSE(anim->nodes[0].moves);
    CHECK(anim->nodes[1].moves);
    CHECK(anim->nodes[1].interval_ms == 100);
    CHECK(anim->nodes[1].keys.size() == 5); // the four and the closing key

    anim->vertex_node = vertex_node;
    std::vector<glm::mat4> pose;
    const glm::vec3 p(10.0f, 0.0f, 3.0f);
    anim->pose(0, pose);
    check_near(apply(pose[0], p), p);
    check_near(apply(pose[1], p), p);
    anim->pose(100, pose); // the second key: a quarter turn
    check_near(apply(pose[1], p), {0.0f, 10.0f, 3.0f});
    anim->pose(50, pose); // half way: the components blended, then normalised (45 degrees here)
    check_near(apply(pose[1], p), {10.0f * std::sqrt(0.5f), 10.0f * std::sqrt(0.5f), 3.0f});
    anim->pose(350, pose); // from the last key back to the first across the seam, the short way round
    check_near(apply(pose[1], p), {10.0f * std::sqrt(0.5f), -10.0f * std::sqrt(0.5f), 3.0f});
    anim->pose(500, pose); // wrapped by the period
    check_near(apply(pose[1], p), {0.0f, 10.0f, 3.0f});
}

TEST_CASE("Object animation: the interval is 100 ms without a timing word, and none without keys to move") {
    {
        auto s = turning_skeleton(4, 4, 0);
        auto w = wld::WldFile::parse(s.b.build());
        auto anim = wld::build_object_animation(w, w.at(s.skeleton)->as<wld::Skeleton>());
        REQUIRE(anim);
        CHECK(anim->nodes[1].interval_ms == 100);
        CHECK(anim->period_ms == 400);
    }
    {
        // A timing word of 0: no time between the keys, left in its first pose.
        auto s = turning_skeleton(4, 5, 0);
        auto w = wld::WldFile::parse(s.b.build());
        CHECK_FALSE(wld::build_object_animation(w, w.at(s.skeleton)->as<wld::Skeleton>()));
    }
    {
        // One key everywhere: nothing moves.
        auto s = turning_skeleton(1, 5, 100);
        auto w = wld::WldFile::parse(s.b.build());
        CHECK_FALSE(wld::build_object_animation(w, w.at(s.skeleton)->as<wld::Skeleton>()));
    }
}

TEST_CASE("Object animation: a triangle moves with the first of its corners that moves") {
    const std::vector<std::uint16_t> nodes{assets::kStillVertex, 2, 3};
    CHECK(assets::moving_triangle_node(nodes, 0, 0, 0) == assets::kStillVertex);
    CHECK(assets::moving_triangle_node(nodes, 0, 2, 1) == 3);
    CHECK(assets::moving_triangle_node(nodes, 1, 2, 0) == 2);
    CHECK(assets::moving_triangle_node(nodes, 7, 8, 9) == assets::kStillVertex); // past the list: still
}

TEST_CASE("Animated objects: the moving parts block where the animation has them, not in the static mesh") {
    const assets::Zone zone = slab_zone();
    // The static mesh has the ground only: the slab's triangles all move.
    const game::CollisionMesh mesh = game::CollisionMesh::build(zone);
    CHECK(mesh.triangle_count() == 2);

    game::AnimatedObjects moving = game::AnimatedObjects::build(zone);
    REQUIRE_FALSE(moving.empty());
    CHECK(moving.definition_count() == 1);
    // At 0 the slab stands at x 5..6: a walk along -x from x 10 meets it.
    moving.set_time(0);
    CHECK(blocked(moving, {10, 0, 2}, {0, 0, 2}));
    CHECK_FALSE(blocked(moving, {-10, 0, 2}, {0, 0, 2}));
    // A quarter turn later it stands at y 5..6.
    moving.set_time(100);
    CHECK_FALSE(blocked(moving, {10, 0, 2}, {0, 0, 2}));
    CHECK(blocked(moving, {0, 10, 2}, {0, 0, 2}));
    // Half a turn: at x -6..-5.
    moving.set_time(200);
    CHECK_FALSE(blocked(moving, {10, 0, 2}, {0, 0, 2}));
    CHECK(blocked(moving, {-10, 0, 2}, {0, 0, 2}));
    // Its top is a floor where it stands.
    REQUIRE(moving.floor_below({-5.5f, 0.0f, 10.0f}, 1.0f));
    CHECK(*moving.floor_below({-5.5f, 0.0f, 10.0f}, 1.0f) == doctest::Approx(4.0f));
    CHECK_FALSE(moving.floor_below({5.5f, 0.0f, 10.0f}, 1.0f));
    // A full period later it is back.
    moving.set_time(400);
    CHECK(blocked(moving, {10, 0, 2}, {0, 0, 2}));
}

TEST_CASE("Animated objects: effect surfaces follow current passable moving scenery without changing movement") {
    auto zone=slab_zone();
    auto& mesh=zone.objects.at("SLAB_ACTORDEF");
    for(auto& part:mesh.parts)part.solid=false;
    auto moving=game::AnimatedObjects::build(zone);
    REQUIRE_FALSE(moving.empty()); // a decorative-only animation still supplies visual surfaces
    const auto stationary=game::CollisionMesh::build(zone);
    const auto sample=[&](float x,float y) {
        return app::world_geometry_surface_floor(stationary,&moving,
            [](const glm::vec3&) {return std::optional<float>{};},{x,y,10});
    };
    moving.set_time(0);
    REQUIRE(sample(5.5f,0));
    CHECK(*sample(5.5f,0)==doctest::Approx(4));
    CHECK_FALSE(moving.floor_below({5.5f,0,10},0));
    moving.set_time(100);
    REQUIRE(sample(0,5.5f));
    CHECK(*sample(0,5.5f)==doctest::Approx(4));
    REQUIRE(sample(5.5f,0));
    CHECK(*sample(5.5f,0)==doctest::Approx(0)); // no stale animation floor at its earlier position
    CHECK_FALSE(blocked(moving,{10,0,2},{0,0,2}));
}

TEST_CASE("Animated objects: a drawn static replacement cannot leak its original moving surface to effects") {
    auto zone=slab_zone();
    auto replacement=slab();
    for(auto& vertex:replacement.vertices)vertex.position.z+=2;
    zone.draw_objects["SLAB_ACTORDEF"]=replacement;
    auto moving=game::AnimatedObjects::build(zone);
    moving.set_time(100);
    REQUIRE(moving.floor_below({0,5.5f,10},0));
    CHECK(*moving.floor_below({0,5.5f,10},0)==doctest::Approx(4)); // original physics still animates
    CHECK_FALSE(moving.visual_surface_below({0,5.5f,10})); // original actor is not the drawn geometry
    const auto drawn=game::CollisionMesh::build_visual(zone);
    REQUIRE(drawn.visual_surface_below({5.5f,0,10}));
    CHECK(*drawn.visual_surface_below({5.5f,0,10})==doctest::Approx(6));
}

TEST_CASE("Animated objects: effect surfaces use the exact renderer clock between physics steps") {
    auto moving=game::AnimatedObjects::build(slab_zone());
    moving.set_time(0);
    const auto physics=moving.part_transform(0);
    moving.set_visual_time(100);
    CHECK(moving.time()==0);
    CHECK(moving.visual_time()==100);
    CHECK(moving.part_transform(0)==physics);
    CHECK(blocked(moving,{10,0,2},{0,0,2}));
    CHECK_FALSE(blocked(moving,{0,10,2},{0,0,2}));
    CHECK_FALSE(moving.visual_surface_below({5.5f,0,10}));
    REQUIRE(moving.visual_surface_below({0,5.5f,10}));
    CHECK(*moving.visual_surface_below({0,5.5f,10})==doctest::Approx(4));
}

TEST_CASE("Animated objects: the player's movement meets the moving part through ZoneMoveWorld") {
    const assets::Zone zone = slab_zone();
    const game::CollisionMesh mesh = game::CollisionMesh::build(zone);
    game::AnimatedObjects moving = game::AnimatedObjects::build(zone);
    app::ZoneMoveWorld world(&mesh, nullptr, &zone.regions);
    world.set_animated(&moving);
    moving.set_time(0);
    auto sweep = world.sweep({10, 0, 2}, {0, 0, 2}, 0.5f);
    CHECK(sweep.collided);
    CHECK(sweep.out.position.x > 6.0f);
    moving.set_time(200);
    sweep = world.sweep({10, 0, 2}, {0, 0, 2}, 0.5f);
    CHECK_FALSE(sweep.collided);
}

TEST_CASE("Ssraeshza Temple: the turning rings let a swimmer into the water shaft and out at the top") {
    const GameDir* game = test::game_dir();
    if (!game || !game->find("ssratemple.s3d")) return;
    const assets::Zone zone = assets::load_classic_zone(*game, "ssratemple");
    for (const char* ring : {"SSRARINGS301_ACTORDEF", "SSRARINGS302_ACTORDEF", "SSRARINGS303_ACTORDEF", "SSRARINGS304_ACTORDEF"}) {
        CAPTURE(ring);
        auto it = zone.object_animations.find(ring);
        REQUIRE(it != zone.object_animations.end());
        CHECK(it->second.period_ms == 121 * 33); // 121 keys 33 ms apart, then the closing key
    }
    const game::CollisionMesh mesh = game::CollisionMesh::build(zone);
    game::AnimatedObjects rings = game::AnimatedObjects::build(zone);
    app::ZoneMoveWorld world(&mesh, nullptr, &zone.regions);
    world.set_passable_ladders(true);
    world.set_animated(&rings);
    net::GameConnection conn(*game, {});
    conn.player_status_mut().endurance = 1000;
    conn.player_status_mut().max_endurance = 1000;
    conn.player_status_mut().stand_state = net::kStandStanding;
    constexpr float kAvatarHeight = 3.75f;
    const glm::vec2 axis(-458.0f, -11.1f); // the shaft's (world x = server y)
    // From the corridor of doors 21 to 23, up the ramp towards the shaft (-x), the rings frozen at `frozen` ms or
    // turning from `start`: in the water within `limit` ms?
    auto walk_in = [&](std::optional<std::uint64_t> frozen, std::uint64_t start, int limit, app::PlayerMovement& m) {
        m.set_collision_radius(game::mechanics::sweep_radius(game::mechanics::SweepRadiusSpawn{kAvatarHeight, 1}));
        m.set_race(1);
        m.reset(glm::vec3(-385.0f, -11.3f, 8.1f), glm::pi<float>());
        std::uint32_t now = 10000;
        for (int t = 0; t < limit; t += 16) {
            rings.set_time(frozen ? *frozen : start + static_cast<std::uint64_t>(t));
            app::MovementKeys keys;
            keys.forward = true;
            now += 16;
            m.update(keys, glm::pi<float>(), 0.0f, 16.0f, now, conn, &zone.regions, kAvatarHeight, &world);
            const auto& b = m.controller().body;
            if (b.body_wet >= 5 && glm::distance(glm::vec2(b.feet), axis) < 16.0f) return true;
        }
        return false;
    };
    {
        // As the rings stood before they turned (their first pose): no way in.
        app::PlayerMovement m;
        CHECK_FALSE(walk_in(std::uint64_t{0}, 0, 10000, m));
    }
    {
        // Half a turn on, both rings' gaps face the corridor.
        app::PlayerMovement m;
        CHECK(walk_in(std::uint64_t{2000}, 0, 10000, m));
    }
    // Turning: in within a turn of the rings, whenever the walk starts; then up the shaft and out over its top.
    for (std::uint64_t start : {0u, 1000u, 2000u, 3000u}) {
        CAPTURE(start);
        app::PlayerMovement m;
        REQUIRE(walk_in(std::nullopt, start, 10000, m));
    }
    app::PlayerMovement m;
    REQUIRE(walk_in(std::nullopt, 0, 10000, m));
    std::uint32_t now = 20000;
    bool surfaced = false, out = false;
    for (int t = 0; t < 120000 && !out; t += 16) {
        rings.set_time(5000 + static_cast<std::uint64_t>(t));
        const auto& b = m.controller().body;
        app::MovementKeys keys;
        keys.jump = true;
        float pitch = 0.0f;
        if (b.feet.z >= 146.4f) surfaced = true;
        if (surfaced) {
            // At the surface: forward and looking up, the swim jump over the platform's rim, towards the gap (-x).
            keys.forward = true;
            pitch = 0.6f;
        }
        now += 16;
        m.update(keys, glm::pi<float>(), pitch, 16.0f, now, conn, &zone.regions, kAvatarHeight, &world);
        out = b.body_wet == 0 && glm::distance(glm::vec2(b.feet), axis) > 30.0f && b.feet.z > 145.0f;
    }
    CHECK(surfaced);
    CHECK(out);
}
