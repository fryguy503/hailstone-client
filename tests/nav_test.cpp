// nav/: the navigation mesh built from collision triangles, its cache, the path follower, app/navigator.h and the MQ
// side (/nav, /moveto, the Navigation TLO).
#include <doctest/doctest.h>

#include <chrono>
#include <map>
#include <cmath>
#include <fstream>
#include <iterator>
#include <thread>

#include "app/navigator.h"
#include "app/player_movement.h"
#include "game/swept_sphere.h"
#include "assets/regions.h"
#include "assets/zone.h"
#include "core/atomic_file.h"
#include "core/log.h"
#include "game/collision.h"
#include "game/doors.h"
#include "game/coords.h"
#include "mq/commands.h"
#include "mq/parse.h"
#include "mq/tlo.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/movement_packets.h"
#include "net/world_packets.h"
#include "nav/nav_mesh.h"
#include "nav/path_follower.h"
#include "nav/zone_nav.h"
#include <glm/gtc/matrix_transform.hpp>
#include "plugin/hooks.h"
#include "plugin/policy.h"
#include "game_files.h"
#include "scratch_dir.h"
#include "core/env.h"

using namespace hailstone;

namespace {

// World-space triangles; floors face up (CollisionTriangle's record normal, z > 0).
struct Scene {
    std::vector<glm::vec3> verts;
    void tri(glm::vec3 a, glm::vec3 b, glm::vec3 c, bool up = true) {
        glm::vec3 n = glm::cross(a - b, c - b); // CollisionTriangle::make's normal
        if (up && n.z < 0.0f) std::swap(b, c);
        verts.insert(verts.end(), {a, b, c});
    }
    void floor(float x0, float y0, float x1, float y1, float z) {
        tri({x0, y0, z}, {x1, y0, z}, {x1, y1, z});
        tri({x0, y0, z}, {x1, y1, z}, {x0, y1, z});
    }
    void wall_box(float x0, float y0, float x1, float y1, float z0, float z1) {
        auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
            tri(a, b, c, false);
            tri(a, c, d, false);
        };
        quad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1});
        quad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1});
        quad({x1, y1, z0}, {x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1});
        quad({x0, y1, z0}, {x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1});
        floor(x0, y0, x1, y1, z1);
    }
    std::shared_ptr<const nav::InputGeometry> geometry(const nav::BuildParams& p = {}) const {
        return nav::InputGeometry::from_world(verts, {}, p);
    }
    game::CollisionMesh collision() const {
        std::vector<game::CollisionTriangle> t;
        for (std::size_t i = 0; i + 2 < verts.size(); i += 3)
            t.push_back(game::CollisionTriangle::make(verts[i], verts[i + 1], verts[i + 2], 0));
        return game::CollisionMesh::from_triangles(std::move(t));
    }
};

// A 100 x 100 floor with a wall across the middle (x 45..55) from y = 0 to 80: the way round is past y = 80.
Scene walled_room() {
    Scene s;
    s.floor(0, 0, 100, 100, 0);
    s.wall_box(45, 0, 55, 80, 0, 20);
    return s;
}

nav::Area ground(const glm::vec3&) { return nav::Area::Ground; }

std::unique_ptr<nav::ZoneNav> built(const Scene& s, std::function<nav::Area(const glm::vec3&)> classify = ground,
                                    std::filesystem::path cache = {}) {
    nav::ZoneNav::Config cfg;
    cfg.threads = 2;
    cfg.cache_file = std::move(cache);
    auto z = std::make_unique<nav::ZoneNav>(s.geometry(), cfg, std::move(classify));
    z->finish();
    return z;
}

float length(const std::vector<glm::vec3>& p) {
    float d = 0.0f;
    for (std::size_t i = 1; i < p.size(); ++i) d += glm::distance(p[i - 1], p[i]);
    return d;
}

bool inside_wall(const glm::vec3& p) { return p.x > 45.0f && p.x < 55.0f && p.y < 80.0f; }

} // namespace

TEST_CASE("nav: a path goes round a wall, never through it") {
    auto z = built(walled_room());
    REQUIRE(z->complete());
    REQUIRE(z->mesh().tile_count() > 0);
    nav::PathResult r = z->mesh().find_path({20, 20, 0}, {80, 20, 0});
    REQUIRE(r.found());
    CHECK_FALSE(r.partial);
    CHECK(glm::distance(r.points.front(), glm::vec3(20, 20, 0)) < 1.0f);
    CHECK(glm::distance(glm::vec2(r.points.back()), glm::vec2(80, 20)) < 1.0f);
    // Round the end of the wall: up past y = 80 and back (about 146 at the least), much longer than the 60 straight
    // across.
    CHECK(length(r.points) > 140.0f);
    bool past_end = false;
    for (const glm::vec3& p : r.points) {
        CHECK_FALSE(inside_wall(p));
        if (p.y > 80.0f) past_end = true;
    }
    CHECK(past_end);
    // Paths keep the agent's radius from walls (BuildParams::agent_radius).
    for (const glm::vec3& p : r.points)
        if (p.y < 80.0f) CHECK((p.x < 45.0f - 1.5f || p.x > 55.0f + 1.5f));
    CHECK_FALSE(z->mesh().walkable_line({20, 20, 0}, {80, 20, 0}));
    CHECK(z->mesh().walkable_line({20, 20, 0}, {20, 60, 0}));
}

TEST_CASE("nav: a path's bends keep a little further from walls where there is room, and never leave the mesh") {
    // An L of floor 20 wide (x 0..20 by y 0..60, then x 0..60 by y 40..60): the way from the bottom of the L to its
    // right end bends round the inner corner at (20, 40).
    Scene s;
    s.floor(0, 0, 20, 40, 0);
    s.floor(0, 40, 60, 60, 0);
    auto z = built(s);
    const glm::vec2 corner(20, 40);
    auto nearest_bend = [&](const nav::PathResult& r) {
        float best = 1e9f;
        for (std::size_t i = 1; i + 1 < r.points.size(); ++i)
            best = std::min(best, glm::length(glm::vec2(r.points[i]) - corner));
        return best;
    };
    nav::PathOptions hug;
    hug.wall_clearance = 0.0f;
    nav::PathResult tight = z->mesh().find_path({10, 5, 0}, {55, 50, 0}, hug);
    REQUIRE(tight.found());
    REQUIRE(tight.points.size() >= 3);
    nav::PathResult kept = z->mesh().find_path({10, 5, 0}, {55, 50, 0});
    REQUIRE(kept.found());
    REQUIRE(kept.points.size() == tight.points.size());
    CHECK(nearest_bend(tight) < 3.5f);           // the search's bend, at the mesh's margin
    CHECK(nearest_bend(kept) >= nearest_bend(tight) + 1.5f); // moved off the corner
    // (a ray from a point on the mesh's edge can stop at once: the line is clear one way or the other)
    auto clear = [](const nav::NavMesh& m, const glm::vec3& a, const glm::vec3& b) {
        return m.walkable_line(a, b) || m.walkable_line(b, a);
    };
    for (std::size_t i = 0; i + 1 < kept.points.size(); ++i) CHECK(clear(z->mesh(), kept.points[i], kept.points[i + 1]));

    // A way only 8 wide (the margin leaves 4 of it): little room; the bends stay on the mesh.
    Scene n;
    n.floor(0, 0, 8, 40, 0);
    n.floor(0, 32, 40, 40, 0);
    auto zn = built(n);
    nav::PathResult narrow = zn->mesh().find_path({4, 3, 0}, {37, 36, 0});
    REQUIRE(narrow.found());
    CHECK_FALSE(narrow.partial);
    for (std::size_t i = 0; i + 1 < narrow.points.size(); ++i) CHECK(clear(zn->mesh(), narrow.points[i], narrow.points[i + 1]));
}

TEST_CASE("nav: ground that cannot be reached gives a partial path") {
    Scene s;
    s.floor(0, 0, 40, 40, 0);
    s.floor(70, 0, 110, 40, 0); // a 30-unit gap
    auto z = built(s);
    nav::PathResult r = z->mesh().find_path({10, 10, 0}, {90, 10, 0});
    REQUIRE(r.found());
    CHECK(r.partial);
    CHECK(r.points.back().x < 40.0f);
}

namespace {
// A hall (z 0) with two balconies at z 12 along its far side: the west one is reached by a ramp from the hall, the
// east one (x 50..100) by nothing, a 10-unit gap from the west one.
Scene balconies() {
    Scene s;
    s.floor(0, 0, 100, 100, 0);
    s.floor(0, 60, 40, 100, 12);
    s.floor(50, 60, 100, 100, 12);
    s.tri({0, 30, 0}, {20, 30, 0}, {20, 60, 12});
    s.tri({0, 30, 0}, {20, 60, 12}, {0, 60, 12});
    return s;
}
} // namespace

TEST_CASE("nav: a goal on a floor over the hall is reached by the way up, not by standing under it") {
    auto z = built(balconies());
    // The west balcony: up the ramp, and the path ends on the balcony, not on the hall floor under the goal.
    nav::PathResult up = z->mesh().find_path({60, 20, 0}, {30, 80, 12});
    REQUIRE(up.found());
    CHECK_FALSE(up.partial);
    CHECK(up.points.back().z == doctest::Approx(12.0f).epsilon(0.1));
    bool ramp = false;
    for (const glm::vec3& p : up.points)
        if (p.x < 22.0f && p.y > 28.0f && p.y < 62.0f) ramp = true;
    CHECK(ramp);
    // The east balcony cannot be reached. Detour's own fallback ends at the polygon nearest in a straight line: the
    // hall floor 12 under the goal. The reachable point round it on the goal's own floor is the west balcony's edge.
    nav::PathResult cut = z->mesh().find_path({60, 20, 0}, {55, 80, 12});
    REQUIRE(cut.found());
    CHECK(cut.partial);
    CHECK(cut.points.back().z == doctest::Approx(12.0f).epsilon(0.1));
    CHECK(cut.points.back().x < 41.0f);
}

TEST_CASE("nav: a swimmer aims a little on along the path, at its height there") {
    nav::PathFollower f;
    // A floor path under water: down a slope from (0, 0, 0) to (20, 0, -10), then flat to (40, 0, -10), then straight
    // up a column to the surface at (40, 0, 20).
    f.start({{0, 0, 0}, {20, 0, -10}, {40, 0, -10}, {40, 0, 20}}, 2.0f);
    f.step({0, 0, 0}, 0.0);
    REQUIRE(f.next() == 1);
    // 5 over the path at x 4: the point 4 on is at x 8, on the slope (z -4), not the corner's -10 far ahead.
    glm::vec3 a = f.ahead({4, 0, 3}, 4.0f);
    CHECK(a.x == doctest::Approx(8.0f));
    CHECK(a.z == doctest::Approx(-4.0f));
    // Past the corner's leg end it carries on into the next leg.
    glm::vec3 b = f.ahead({18, 0, -9}, 4.0f);
    CHECK(b.x == doctest::Approx(22.0f));
    CHECK(b.z == doctest::Approx(-10.0f));
    // A vertical leg next: it holds the column's foot until there.
    f.step({20, 0, -10}, 0.1);
    REQUIRE(f.next() == 2);
    glm::vec3 c = f.ahead({38, 0, -10}, 4.0f);
    CHECK(c.x == doctest::Approx(40.0f));
    CHECK(c.z == doctest::Approx(-10.0f));
    // On the column: the corner straight over.
    f.step({40, 0, -10}, 0.2);
    REQUIRE(f.next() == 3);
    CHECK(f.ahead({40, 0, -5}, 4.0f).z == doctest::Approx(20.0f));
}

TEST_CASE("nav: a corner or an end over or under the character is not reached by standing beneath it") {
    nav::PathFollower f;
    // A ramp that turns back over itself: the second corner is straight over the first.
    f.start({{0, 0, 0}, {10, 0, 0}, {10, 0, 12}, {30, 0, 12}}, 2.0f);
    nav::SteerOutput o = f.step({10, 0, 0}, 0.0);
    CHECK(o.active);
    CHECK(f.next() == 2); // past the first corner, aiming at the one over it, not past that too
    // The end straight over the feet is not an arrival.
    nav::PathFollower e;
    e.start({{0, 0, 0}, {5, 0, 15}}, 2.0f);
    e.step({5, 0, 0}, 0.0);
    CHECK(e.state() == nav::FollowState::Moving);
    e.step({5, 0, 14}, 0.1);
    CHECK(e.state() == nav::FollowState::Arrived);
    // The server's path is at its own mesh's heights: reached by distance across alone.
    nav::PathFollower s;
    s.start({{0, 0, 0}, {5, 0, 15}}, 2.0f, false);
    s.step({5, 0, 0}, 0.0);
    CHECK(s.state() == nav::FollowState::Arrived);
}

TEST_CASE("nav: ground steeper than the slope limit is not walkable") {
    Scene s;
    // 70 degrees: z rises 2.75 for each unit of x.
    s.tri({0, 0, 0}, {20, 0, 55}, {20, 20, 55});
    s.tri({0, 0, 0}, {20, 20, 55}, {0, 20, 0});
    auto z = built(s);
    CHECK(z->mesh().tile_count() == 0);
    CHECK_FALSE(z->mesh().nearest({10, 10, 27}).has_value());
}

TEST_CASE("nav: passable triangles are left out") {
    Scene s;
    s.floor(0, 0, 50, 50, 0);
    std::vector<std::uint8_t> solid(s.verts.size() / 3, 0); // every triangle passable
    auto geo = nav::InputGeometry::from_world(s.verts, solid, {});
    CHECK(geo->triangle_count() == 0);
}

TEST_CASE("nav: areas from the regions: water costs more and can be avoided, lava is never crossed") {
    Scene s;
    s.floor(0, 0, 100, 100, 0);
    // A band across the room at x 40..60.
    auto water = [](const glm::vec3& p) { return p.x > 40.0f && p.x < 60.0f ? nav::Area::Water : nav::Area::Ground; };
    auto z = built(s, water);
    nav::PathResult r = z->mesh().find_path({20, 50, 0}, {80, 50, 0});
    REQUIRE(r.found());
    CHECK_FALSE(r.partial);
    nav::PathOptions dry;
    dry.avoid_water = true;
    CHECK(z->mesh().find_path({20, 50, 0}, {80, 50, 0}, dry).partial);

    auto lava = [](const glm::vec3& p) { return p.x > 40.0f && p.x < 60.0f ? nav::Area::Lava : nav::Area::Ground; };
    auto zl = built(s, lava);
    CHECK(zl->mesh().find_path({20, 50, 0}, {80, 50, 0}).partial);
    nav::PathOptions brave;
    brave.allow_hazards = true;
    CHECK_FALSE(zl->mesh().find_path({20, 50, 0}, {80, 50, 0}, brave).partial);
}

TEST_CASE("nav: the cache is written once built and read back, and a stale or broken one is ignored") {
    test::ScratchDir dir("hailstone_nav_cache");
    Scene s = walled_room();
    std::filesystem::path file = dir.path() / nav::ZoneNav::cache_name("test zone/1");
    CHECK(file.filename() == "test_zone_1.navmesh");
    auto first = built(s, ground, file);
    REQUIRE(std::filesystem::exists(file));
    CHECK_FALSE(first->from_cache());

    auto second = built(s, ground, file);
    CHECK(second->from_cache());
    CHECK(second->complete());
    CHECK(second->mesh().tile_count() == first->mesh().tile_count());
    auto a = first->mesh().find_path({20, 20, 0}, {80, 20, 0});
    auto b = second->mesh().find_path({20, 20, 0}, {80, 20, 0});
    REQUIRE(a.points.size() == b.points.size());
    for (std::size_t i = 0; i < a.points.size(); ++i) CHECK(glm::distance(a.points[i], b.points[i]) < 0.01f);

    // Other geometry: the hash differs, the file is not used (and is replaced).
    Scene other = walled_room();
    other.floor(100, 0, 150, 100, 0);
    CHECK_FALSE(built(other, ground, file)->from_cache());

    // A truncated file is no cache.
    auto text = read_text_file(file);
    REQUIRE(text);
    REQUIRE(write_file_atomic(file, std::string_view(*text).substr(0, text->size() / 2)));
    auto third = built(other, ground, file);
    CHECK_FALSE(third->from_cache());
    CHECK(third->mesh().tile_count() > 0);
}

TEST_CASE("nav: the builder's fingerprint scene has one of each thing the builder makes") {
    // If a rule stops showing in the scene, a change to it could slip past the fingerprint: grow the scene with it.
    auto geo = nav::fingerprint_scene();
    nav::ZoneNav::Config cfg;
    nav::ZoneNav z(geo, cfg, [](const glm::vec3& p) {
        bool water = p.y >= 0 && p.y <= 40 && p.z <= -1 &&
                     ((p.x >= 60 && p.x <= 100 && p.z >= -30) || (p.x >= 140 && p.x <= 180 && p.z >= -10));
        return water ? nav::Area::Water : nav::Area::Ground;
    });
    z.finish();
    bool drop = false, column = false, flooded = false;
    z.mesh().for_each_link([&](const glm::vec3& a, const glm::vec3& b, bool both) {
        if (!both && a.z > 10.0f && b.z < 1.0f) drop = true;                          // off the ledge
        if (both && glm::length(glm::vec2(a - b)) < 0.1f && std::abs(a.z - b.z) > 20) column = true; // pool floor
        if (both && a.x > 130 && a.z > -1.0f && b.z < -8.0f) flooded = true;          // into the flooded room
    });
    CHECK(drop);
    CHECK(column);
    CHECK(flooded);
    CHECK_FALSE(z.mesh().find_path({25, 30, 0}, {35, 10, 3.6f}).partial); // up the step
    CHECK_FALSE(z.mesh().find_path({25, 30, 0}, {200, 20, 0}).partial);   // through both waters
    bool zone_line = false;
    z.mesh().for_each_polygon([&](std::span<const glm::vec3>, nav::Area a) { zone_line = zone_line || a == nav::Area::ZoneLine; });
    CHECK(zone_line);
    // Worked out once, the same each time, and something.
    CHECK(nav::builder_fingerprint() == nav::builder_fingerprint());
    CHECK(nav::builder_fingerprint() != 0);
}

TEST_CASE("nav: decode_cache checks the header; add_tile refuses data that is not a tile") {
    Scene s = walled_room();
    auto geo = s.geometry();
    std::vector<unsigned char> tile = nav::build_tile(*geo, 0, 0);
    REQUIRE_FALSE(tile.empty());
    nav::CacheHeader h;
    h.geometry_hash = geo->hash();
    h.params_hash = geo->params().hash();
    h.origin = geo->bmin();
    h.tile_size = geo->params().tile_size();
    h.tiles_x = geo->tiles_x();
    h.tiles_y = geo->tiles_y();
    std::vector<nav::CachedTile> tiles{{0, 0, tile}};
    auto bytes = nav::encode_cache(h, tiles);
    auto back = nav::decode_cache(bytes, h);
    REQUIRE(back);
    REQUIRE(back->size() == 1);
    CHECK((*back)[0].data == tile);
    nav::CacheHeader other = h;
    other.params_hash ^= 1;
    CHECK_FALSE(nav::decode_cache(bytes, other));
    // Written by a builder that makes something else of the same zone: built again.
    other = h;
    other.builder ^= 1;
    CHECK_FALSE(nav::decode_cache(bytes, other));
    for (std::size_t n = 0; n < bytes.size(); n += 7)
        CHECK_FALSE(nav::decode_cache(std::span(bytes).first(n), h)); // every truncation
    bytes.push_back(std::byte{0});
    CHECK_FALSE(nav::decode_cache(bytes, h)); // trailing bytes

    nav::NavMesh mesh;
    REQUIRE(mesh.init(geo->bmin(), geo->params().tile_size(), geo->tiles_x(), geo->tiles_y(), geo->params()));
    std::vector<unsigned char> garbage(tile.size(), 0x5a);
    CHECK_FALSE(mesh.add_tile(garbage));
    CHECK_FALSE(mesh.add_tile(std::span(tile).first(tile.size() - 4)));
    CHECK(mesh.add_tile(tile));
    CHECK(mesh.has_tile(0, 0));
}

namespace {
// Moves feet as the steer asks: forward along the yaw at `speed`, sideways for a strafe; `blocked` stops a move.
glm::vec3 move(const glm::vec3& feet, const nav::SteerOutput& o, float speed, float dt,
               const std::function<bool(const glm::vec3&)>& blocked = {}) {
    if (!o.active) return feet;
    glm::vec3 fwd(std::cos(o.yaw), std::sin(o.yaw), 0.0f), side(-fwd.y, fwd.x, 0.0f);
    glm::vec3 next = feet + (o.forward ? fwd : glm::vec3(0.0f)) * speed * dt - side * static_cast<float>(o.strafe) * speed * 0.5f * dt;
    return blocked && blocked(next) ? feet : next;
}
} // namespace

TEST_CASE("nav: the follower walks a path round the wall and arrives") {
    auto z = built(walled_room());
    nav::PathResult r = z->mesh().find_path({20, 20, 0}, {80, 20, 0});
    REQUIRE(r.found());
    nav::PathFollower f;
    f.start(r.points, 2.0f);
    glm::vec3 feet(20, 20, 0);
    double t = 0.0;
    while (f.state() == nav::FollowState::Moving && t < 60.0) {
        nav::SteerOutput o = f.step(feet, t);
        feet = move(feet, o, 15.0f, 0.05f);
        CHECK_FALSE(inside_wall(feet));
        t += 0.05;
    }
    CHECK(f.state() == nav::FollowState::Arrived);
    CHECK(glm::length(glm::vec2(feet) - glm::vec2(80, 20)) <= 2.5f);
}

TEST_CASE("nav: a walk with a long arrival distance stops that far along the path, whatever corners are left") {
    // /nav spawn <name> distance=45: a path whose last two legs bend round the goal (each 22.4 long). It used to
    // arrive only on the last leg, past the last corner: 22 from the end, not 45.
    nav::PathFollower f;
    f.start({{0, 0, 0}, {60, 0, 0}, {80, 10, 0}, {100, 0, 0}}, 45.0f);
    glm::vec3 feet(0, 0, 0);
    double t = 0.0;
    while (f.state() == nav::FollowState::Moving && t < 30.0) {
        nav::SteerOutput o = f.step(feet, t);
        feet = move(feet, o, 15.0f, 0.05f);
        t += 0.05;
    }
    CHECK(f.state() == nav::FollowState::Arrived);
    CHECK(f.next() == 2); // the last corner still ahead
    CHECK(f.remaining(feet) <= 45.0f);
    CHECK(f.remaining(feet) >= 44.0f);
    // Along the path, not straight across: a goal 20 away behind a wall the path goes round (120 long) is not near.
    nav::PathFollower w;
    w.start({{0, 0, 0}, {0, 50, 0}, {20, 50, 0}, {20, 0, 0}}, 30.0f);
    w.step({0, 0, 0}, 0.0);
    CHECK(w.state() == nav::FollowState::Moving);
    // In height the end may be the distance off; further is another floor, not an arrival.
    nav::PathFollower h;
    h.start({{0, 0, 0}, {10, 0, 0}, {10, 0, 60}, {20, 0, 60}}, 20.0f);
    h.step({10, 0, 0}, 0.0);
    CHECK(h.state() == nav::FollowState::Moving);
    CHECK(h.next() == 2);
}

TEST_CASE("nav: a stuck follower jumps and strafes, asks for new paths, then gives up") {
    nav::PathFollower f;
    f.start({{0, 0, 0}, {50, 0, 0}}, 2.0f);
    glm::vec3 feet(0, 0, 0);
    double t = 0.0;
    int jumps = 0, strafes = 0, replaced = 0;
    double first_jump = -1.0;
    while (t < 300.0) {
        nav::SteerOutput o = f.step(feet, t); // never moves
        if (o.jump) {
            ++jumps;
            if (first_jump < 0.0) first_jump = t;
        }
        if (o.strafe) ++strafes;
        if (f.state() == nav::FollowState::NeedsPath) {
            ++replaced;
            f.replace({{0, 0, 0}, {50, 0, 0}});
        }
        if (f.state() == nav::FollowState::Failed) break;
        t += 0.05;
    }
    CHECK(f.state() == nav::FollowState::Failed);
    CHECK(f.failure() == "stuck");
    CHECK(first_jump == doctest::Approx(1.0).epsilon(0.06));
    CHECK(replaced == 3);
    CHECK(jumps == 3 * 4); // three per path, four paths
    CHECK(strafes > 0);
}

TEST_CASE("nav: progress resets the stall clock") {
    nav::PathFollower f;
    f.start({{0, 0, 0}, {100, 0, 0}}, 2.0f);
    glm::vec3 feet(0, 0, 0);
    for (double t = 0.0; t < 5.0; t += 0.05) {
        nav::SteerOutput o = f.step(feet, t);
        CHECK_FALSE(o.jump);
        feet.x += 0.5f; // 10 units a second
    }
    CHECK(f.state() == nav::FollowState::Moving);
    CHECK(f.remaining(feet) == doctest::Approx(100.0f - feet.x));
}

TEST_CASE("nav: a hitch is not counted as being stuck") {
    nav::PathFollower f;
    f.start({{0, 0, 0}, {100, 0, 0}}, 2.0f);
    glm::vec3 feet(0, 0, 0);
    double t = 0.0;
    for (; t < 1.0; t += 0.05) {
        feet.x += 0.5f;
        f.step(feet, t);
    }
    // A 3 s frame (a zone's model being built): the character did not move in it, and no jump follows it.
    t += 3.0;
    CHECK_FALSE(f.step(feet, t).jump);
    for (int i = 0; i < 5; ++i) {
        t += 0.05;
        feet.x += 0.5f;
        CHECK_FALSE(f.step(feet, t).jump);
    }
    // Really stuck after it: the stall still counts (its time, not the hitch's).
    double stuck_from = t;
    double jumped_at = -1.0;
    while (t < stuck_from + 3.0 && jumped_at < 0.0) {
        t += 0.05;
        if (f.step(feet, t).jump) jumped_at = t;
    }
    CHECK(jumped_at - stuck_from == doctest::Approx(1.0).epsilon(0.1));
}

namespace {
struct NavHarness {
    Scene scene = walled_room();
    game::CollisionMesh collision = scene.collision();
    std::vector<std::string> chat;
    app::Navigator nav{[this] {
        app::Navigator::Env e;
        e.chat = [this](const std::string& t) { chat.push_back(t); };
        return e;
    }()};
    glm::vec3 feet{20, 20, 0};
    double now = 0.0;
    nav::SteerOutput frame(bool player_input = false) {
        app::Navigator::Frame f;
        f.feet = feet;
        f.now = now;
        f.player_input = player_input;
        nav::SteerOutput o = nav.frame(f);
        feet = move(feet, o, 15.0f, 0.05f);
        now += 0.05;
        return o;
    }
    // Frames until the walk ends. The tile builds run on their own threads: the game time stands still until they
    // are done, so how long they take does not matter.
    void run(double limit = 60.0) {
        auto wall_clock = std::chrono::steady_clock::now();
        while (!nav.status().mesh_complete && std::chrono::steady_clock::now() - wall_clock < std::chrono::seconds(30)) {
            app::Navigator::Frame f;
            f.feet = feet;
            f.now = now;
            nav.frame(f);
            std::this_thread::yield();
        }
        double end = now + limit;
        while (nav.status().active && now < end) frame();
    }
};
} // namespace

TEST_CASE("nav: the navigator waits for the mesh, walks round the wall and arrives") {
    NavHarness h;
    h.nav.zone_loaded(h.collision, nullptr, {}); // no cache: built when first wanted
    CHECK_FALSE(h.nav.status().mesh_loaded);
    CHECK(h.nav.go_to_point({80, 20, 0}, 2.0f, "the far side", false).empty());
    CHECK(h.nav.status().active);
    h.run();
    CHECK_FALSE(h.nav.status().active);
    CHECK(h.nav.status().mesh_complete);
    CHECK(glm::length(glm::vec2(h.feet) - glm::vec2(80, 20)) <= 2.5f);
    // A point with no ground near it, once the mesh is complete, has no path.
    CHECK_FALSE(h.nav.go_to_point({500, 500, 0}, 2.0f, "nowhere", false).empty());
    CHECK(h.nav.path_length({20, 20, 0}).has_value());
    CHECK_FALSE(h.nav.path_length({500, 500, 0}).has_value());
}

TEST_CASE("nav: with prebuild, a zone with no cache starts building its mesh at the zone-in") {
    Scene scene = walled_room();
    game::CollisionMesh collision = scene.collision();
    app::Navigator::Env e;
    e.prebuild = true; // the client's default (hailstone.ini nav.prebuild)
    app::Navigator nav{std::move(e)};
    nav.zone_loaded(collision, nullptr, {});
    CHECK(nav.status().tiles_total > 0); // nothing asked for it: no walk, no overlay
    CHECK_FALSE(nav.status().active);
    auto start = std::chrono::steady_clock::now();
    while (!nav.status().mesh_complete && std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
        app::Navigator::Frame f;
        f.feet = {20, 20, 0};
        nav.frame(f);
        std::this_thread::yield();
    }
    CHECK(nav.status().mesh_complete);
}

TEST_CASE("nav: a walk to ground the mesh cannot reach says so, and how far short it stopped") {
    NavHarness h;
    Scene s;
    s.floor(0, 0, 40, 40, 0);
    s.floor(70, 0, 110, 40, 0); // cut off by a 30-unit gap
    game::CollisionMesh islands = s.collision();
    h.feet = {10, 10, 0};
    h.nav.zone_loaded(islands, nullptr, {});
    REQUIRE(h.nav.go_to_point({90, 10, 0}, 2.0f, "the island", false).empty());
    h.run();
    CHECK_FALSE(h.nav.status().active);
    CHECK(h.feet.x < 40.0f);
    bool warned = false, short_by = false;
    for (const std::string& line : h.chat) {
        if (line.find("cannot be reached over the mesh") != std::string::npos) warned = true;
        if (line.find("short of the island") != std::string::npos) short_by = true;
    }
    CHECK(warned);
    CHECK(short_by);
}

TEST_CASE("nav: a walk that ends under a goal it has no way up to says so") {
    NavHarness h;
    Scene s;
    s.floor(0, 0, 100, 100, 0);
    s.floor(50, 60, 100, 100, 12); // a balcony with no way up
    game::CollisionMesh hall = s.collision();
    h.feet = {20, 20, 0};
    h.nav.zone_loaded(hall, nullptr, {});
    REQUIRE(h.nav.go_to_point({75, 80, 12}, 2.0f, "the balcony", false).empty());
    h.run();
    CHECK_FALSE(h.nav.status().active);
    bool below = false;
    for (const std::string& line : h.chat)
        if (line.find("12 below the balcony: the mesh has no way up there") != std::string::npos) below = true;
    CHECK(below);
}

namespace {
// The mesh built and every tile added (the navigator's frames add them).
void wait_for_mesh(NavHarness& h) {
    h.nav.build_now();
    auto start = std::chrono::steady_clock::now();
    while (!h.nav.status().mesh_complete && std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
        app::Navigator::Frame f;
        f.feet = h.feet;
        f.now = h.now;
        h.nav.frame(f);
        std::this_thread::yield();
    }
    REQUIRE(h.nav.status().mesh_complete);
}
void wait_for_mesh_of(app::Navigator& nav, const glm::vec3& feet) {
    nav.build_now();
    auto start = std::chrono::steady_clock::now();
    while (!nav.status().mesh_complete && std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
        app::Navigator::Frame f;
        f.feet = feet;
        nav.frame(f);
        std::this_thread::yield();
    }
    REQUIRE(nav.status().mesh_complete);
}
bool sight(const game::CollisionMesh& c, const glm::vec3& a, const glm::vec3& b) {
    return c.line_of_sight(a + glm::vec3(0, 0, 3), b + glm::vec3(0, 0, 3));
}
} // namespace

TEST_CASE("nav: /follow's help goes round a wall to a spawn behind it, then leaves the steering to the original") {
    NavHarness h;
    h.nav.zone_loaded(h.collision, nullptr, {});
    wait_for_mesh(h);
    const glm::vec3 target(80, 20, 0);
    REQUIRE_FALSE(sight(h.collision, h.feet, target));
    auto help = h.nav.assist_follow(h.feet, target, false, h.now);
    REQUIRE(help.detour);
    // Not at the spawn through the wall (yaw 0) but up towards the wall's end.
    CHECK(std::sin(help.steer.yaw) > 0.5f);
    CHECK(help.remaining > 140.0f);
    // Walk it: round the end of the wall until the line is clear and straight, never through the wall.
    bool handed_back = false;
    for (int i = 0; i < 2000 && !handed_back; ++i) {
        help = h.nav.assist_follow(h.feet, target, sight(h.collision, h.feet, target), h.now);
        if (!help.detour) {
            handed_back = true;
            break;
        }
        h.feet = move(h.feet, help.steer, 15.0f, 0.05f);
        CHECK_FALSE(inside_wall(h.feet));
        h.now += 0.05;
    }
    CHECK(handed_back);
    CHECK(h.feet.x > 55.0f); // past the wall, the spawn in plain sight
    CHECK(sight(h.collision, h.feet, target));
}

TEST_CASE("nav: /follow's help stays out of a clear straight follow, and can be turned off") {
    NavHarness h;
    h.nav.zone_loaded(h.collision, nullptr, {});
    wait_for_mesh(h);
    CHECK_FALSE(h.nav.assist_follow(h.feet, {20, 60, 0}, true, h.now).detour);
    h.nav.options().assist_follow = false;
    CHECK_FALSE(h.nav.assist_follow(h.feet, {80, 20, 0}, false, h.now).detour);
}

TEST_CASE("nav: /follow's help that is stuck hands back to the original's step for a while") {
    NavHarness h;
    h.nav.zone_loaded(h.collision, nullptr, {});
    wait_for_mesh(h);
    const glm::vec3 target(80, 20, 0);
    bool detoured = true;
    double t = h.now;
    // Never moves: the detour unsticks, re-paths, then gives up.
    for (; t < h.now + 120.0 && detoured; t += 0.05) detoured = h.nav.assist_follow(h.feet, target, false, t).detour;
    CHECK_FALSE(detoured);
    CHECK_FALSE(h.nav.assist_follow(h.feet, target, false, t + 1.0).detour); // the 5 s rest
    CHECK(h.nav.assist_follow(h.feet, target, false, t + 6.0).detour);        // then the mesh again
}

TEST_CASE("nav: the player's movement keys end a walk; pause holds it") {
    NavHarness h;
    h.nav.zone_loaded(h.collision, nullptr, {});
    REQUIRE(h.nav.go_to_point({20, 60, 0}, 2.0f, "north", true).empty()); // straight: no mesh needed
    nav::SteerOutput o = h.frame();
    CHECK(o.active);
    CHECK(o.forward);
    CHECK(o.yaw == doctest::Approx(std::atan2(40.0f, 0.0f)));
    h.nav.set_paused(true);
    CHECK_FALSE(h.frame().active);
    CHECK(h.nav.status().active);
    h.nav.set_paused(false);
    CHECK(h.frame().active);
    CHECK_FALSE(h.frame(true).active);
    CHECK_FALSE(h.nav.status().active);
    CHECK(h.chat.back() == "Navigation stopped.");
}

namespace {
// A spawn to stick to, in the walled room, with a melee range of 8 (the stick's default distance is 6).
struct StickHarness {
    Scene scene = walled_room();
    game::CollisionMesh collision = scene.collision();
    std::vector<std::string> chat;
    game::Spawn mob;
    std::uint64_t context = 1;
    app::Navigator nav{[this] {
        app::Navigator::Env e;
        e.chat = [this](const std::string& t) { chat.push_back(t); };
        e.spawn = [this](std::uint32_t id) -> const game::Spawn* { return id == mob.id ? &mob : nullptr; };
        e.interaction_context = [this] { return context; };
        e.melee_range = [](std::uint32_t) -> std::optional<float> { return 8.0f; };
        e.line_of_sight = [this](const glm::vec3& a, const glm::vec3& b) { return collision.line_of_sight(a, b); };
        return e;
    }()};
    glm::vec3 feet{20, 20, 0};
    double now = 0.0;
    StickHarness() {
        mob.id = 7;
        mob.name = "a_gnoll01";
        mob.is_npc = true;
        mob.position = {20, 50, 0};
    }
    nav::SteerOutput frame(bool player_input = false) {
        app::Navigator::Frame f;
        f.feet = feet;
        f.now = now;
        f.player_input = player_input;
        nav::SteerOutput o = nav.frame(f);
        feet = move(feet, o, 15.0f, 0.05f);
        now += 0.05;
        return o;
    }
    float away() const { return glm::length(glm::vec2(feet - mob.position)); }
};
} // namespace

TEST_CASE("nav: a Find-owned walk retires before movement while replacement navigation keeps its own lifetime") {
    StickHarness h;
    h.nav.zone_loaded(h.collision, nullptr, {});
    bool eligible = true;
    REQUIRE(h.nav.go_to_spawn_guarded(7, 5, [&] { return eligible; }).empty());
    REQUIRE(h.nav.status().active);
    h.nav.set_paused(true); eligible = false;
    CHECK_FALSE(h.frame().active); CHECK_FALSE(h.nav.status().active); // even paused/waiting for a mesh
    eligible = true;
    REQUIRE(h.nav.go_to_spawn_guarded(7, 5, [&] { return eligible; }).empty());
    REQUIRE(h.nav.go_to_spawn(7, 5).empty()); eligible = false;
    h.frame(); CHECK(h.nav.status().active); // a manual walk to the SAME spawn does not inherit Find ownership
    REQUIRE(h.nav.go_to_point({60, 20, 0}, 2, "Manual point", true).empty());
    h.frame(); CHECK(h.nav.status().active);
    CHECK_FALSE(h.nav.go_to_spawn_guarded(7, 5, []() -> bool { throw 7; }).empty());
    CHECK(h.nav.status().active); CHECK(h.nav.status().goal == "Manual point");
    REQUIRE(h.nav.stick(7, {}).empty()); h.frame(); CHECK(h.nav.status().stick_spawn == 7);
}

TEST_CASE("nav: eligibility lost during planning cannot start a quiet server fallback") {
    Scene scene = walled_room();
    auto collision = scene.collision();
    game::Spawn mob; mob.id = 7; mob.is_npc = true; mob.name = "Destination"; mob.position = {20, 50, 0};
    unsigned reads = 0, requests = 0, started = 0, checks = 0;
    app::Navigator::Env env;
    env.spawn = [&](std::uint32_t id) -> const game::Spawn* { ++reads; return id == mob.id ? &mob : nullptr; };
    env.request_server_path = [&](std::uint32_t, game::ServerPathOrigin) { ++requests; return game::ServerPathLease{}; };
    env.started = [&] { ++started; };
    app::Navigator nav{std::move(env)};
    nav.zone_loaded(collision, nullptr, {});
    // The entry check succeeds. The check before the planner's destination read then loses eligibility.
    CHECK_FALSE(nav.go_to_spawn_guarded(7, 5, [&] { return ++checks == 1; }).empty());
    CHECK_FALSE(nav.status().active);
    CHECK(reads == 1); CHECK(requests == 0); CHECK(started == 0);
    // A manual walk has no Find guard and can still use the same ordinary navigation fallback.
    CHECK(nav.go_to_spawn(7, 5).empty()); CHECK(nav.status().active); CHECK(started == 1);
}

TEST_CASE("nav: ordinary spawn goals stop on incarnation replacement while a deliberate new goal remains valid") {
    StickHarness h;
    h.mob.incarnation = 101;
    h.nav.zone_loaded(h.collision, nullptr, {});
    REQUIRE(h.nav.go_to_spawn(7, 5).empty());
    ++h.mob.incarnation;
    CHECK_FALSE(h.frame().active); CHECK_FALSE(h.nav.status().active);
    REQUIRE(h.nav.go_to_spawn(7, 5).empty());
    CHECK(h.nav.status().active);
}

TEST_CASE("nav: a stick retires actor and player-life replacements even while paused") {
    StickHarness h;
    h.mob.incarnation = 101;
    h.mob.position = h.feet + glm::vec3(3, 0, 0);
    h.nav.zone_loaded(h.collision, nullptr, {});
    REQUIRE(h.nav.stick(7, {}).empty());
    REQUIRE(h.frame().active); CHECK(h.nav.status().stick_spawn == 7);
    ++h.mob.incarnation;
    CHECK_FALSE(h.frame().active); CHECK(h.nav.status().stick_spawn == 0);
    REQUIRE(h.nav.stick(7, {}).empty()); // a new explicit intent may select the replacement actor
    REQUIRE(h.frame().active);
    h.nav.set_paused(true);
    ++h.context; // same zone and target ID, new player/connection life
    CHECK_FALSE(h.frame().active); CHECK(h.nav.status().stick_spawn == 0);
    REQUIRE(h.nav.stick(7, {}).empty()); CHECK(h.frame().active);
}

TEST_CASE("nav: unloading a zone clears a direct stick with no current path") {
    StickHarness h;
    h.mob.incarnation = 101;
    h.mob.position = h.feet + glm::vec3(3, 0, 0);
    h.nav.zone_loaded(h.collision, nullptr, {});
    REQUIRE(h.nav.stick(7, {}).empty()); REQUIRE(h.frame().active);
    REQUIRE(h.nav.status().stick_spawn == 7); REQUIRE_FALSE(h.nav.status().active);
    h.nav.zone_unloaded();
    CHECK(h.nav.status().stick_spawn == 0);
    ++h.context; ++h.mob.incarnation;
    h.nav.zone_loaded(h.collision, nullptr, {});
    CHECK_FALSE(h.frame().active); // a same-ID actor in the new zone does not reactivate the old stick
    REQUIRE(h.nav.stick(7, {}).empty()); CHECK(h.frame().active);
}

TEST_CASE("nav: a reentrant eligibility result cannot retire a replacement manual goal") {
    StickHarness h;
    h.nav.zone_loaded(h.collision, nullptr, {});
    bool replace = false;
    REQUIRE(h.nav.go_to_spawn_guarded(7, 5, [&] {
        if (replace) {
            replace = false;
            CHECK(h.nav.go_to_point({60, 20, 0}, 2, "Replacement", true).empty());
            return false;
        }
        return true;
    }).empty());
    replace = true;
    CHECK_FALSE(h.frame().active); // old goal does not drive this frame
    CHECK(h.nav.status().active); CHECK(h.nav.status().goal == "Replacement");
    h.frame(); CHECK(h.nav.status().active);
}

TEST_CASE("nav: /stick closes to inside melee range, holds facing the spawn, follows it, and the keys end it") {
    StickHarness h;
    h.nav.zone_loaded(h.collision, nullptr, {});
    REQUIRE(h.nav.stick(7, {}).empty());
    CHECK(h.nav.status().stick_spawn == 7);
    CHECK(h.nav.status().stick_distance == doctest::Approx(6.0f));
    CHECK_FALSE(h.nav.status().active); // not a /nav walk
    for (int i = 0; i < 200 && h.away() > 6.0f; ++i) CHECK(h.frame().active);
    CHECK(h.away() <= 6.0f);
    // Near enough: holds still, facing it, for as long as it stays.
    for (int i = 0; i < 20; ++i) {
        nav::SteerOutput o = h.frame();
        CHECK(o.active);
        CHECK_FALSE(o.forward);
        CHECK(o.yaw == doctest::Approx(std::atan2(h.mob.position.y - h.feet.y, h.mob.position.x - h.feet.x)));
    }
    // It walks off: after it again.
    h.mob.position = {30, 75, 0};
    CHECK(h.frame().forward);
    for (int i = 0; i < 200 && h.away() > 6.0f; ++i) h.frame();
    CHECK(h.away() <= 6.0f);
    CHECK(h.nav.status().stick_spawn == 7);
    // The player's own keys end it.
    CHECK_FALSE(h.frame(true).active);
    CHECK(h.nav.status().stick_spawn == 0);
    CHECK(h.chat.back() == "You are no longer sticking to anything.");
}

TEST_CASE("nav: /stick goes round a wall over the mesh, and ends when the spawn dies") {
    StickHarness h;
    h.nav.zone_loaded(h.collision, nullptr, {});
    wait_for_mesh_of(h.nav, h.feet);
    h.mob.position = {80, 20, 0};
    REQUIRE(h.nav.stick(7, {4.0f, false}).empty());
    CHECK(h.nav.status().stick_distance == 4.0f);
    for (int i = 0; i < 2000 && h.away() > 4.0f; ++i) {
        h.frame();
        CHECK_FALSE(inside_wall(h.feet));
    }
    CHECK(h.away() <= 4.0f);
    h.mob.is_corpse = true;
    CHECK_FALSE(h.frame().active);
    CHECK(h.nav.status().stick_spawn == 0);
    CHECK(h.chat.back() == "[Stick] a_gnoll01 is gone.");
    CHECK(h.nav.stick(7, {}) == "no such spawn");
    CHECK(h.nav.stick(99, {}) == "no such spawn");
}

TEST_CASE("nav: /stick does not run straight past a wall's corner it can see over but would catch on") {
    // The wall's end is at y 80; the player and the mob stand 1 past it on either side. The line between them is in
    // plain sight at the body's middle, 1 from the corner, but a body (radius 1.78) would catch on it.
    StickHarness h;
    h.nav.zone_loaded(h.collision, nullptr, {});
    wait_for_mesh_of(h.nav, h.feet);
    h.feet = {38, 81, 0};
    h.mob.position = {62, 81, 0};
    REQUIRE(h.collision.line_of_sight({38, 81, 3}, {62, 81, 3}));
    REQUIRE(h.nav.stick(7, {4.0f, false}).empty());
    float widest = 0.0f; // how far from the wall's end the walk passes it
    for (int i = 0; i < 2000 && h.away() > 4.0f; ++i) {
        h.frame();
        CHECK_FALSE(inside_wall(h.feet));
        if (h.feet.x > 44.0f && h.feet.x < 56.0f) widest = std::max(widest, h.feet.y - 80.0f);
    }
    CHECK(h.away() <= 4.0f);
    CHECK(widest >= 2.0f); // round the corner over the mesh, not along the straight line 1 from it
}

TEST_CASE("nav: /stick behind walks round to the spawn's back; a /nav walk ends a stick") {
    StickHarness h;
    h.nav.zone_loaded(h.collision, nullptr, {});
    h.mob.heading = 384.0f; // facing -y, towards the player at (20, 20)
    REQUIRE(h.nav.stick(7, {6.0f, true}).empty());
    bool behind = false;
    for (int i = 0; i < 400 && !behind; ++i) {
        h.frame();
        behind = h.away() <= 6.0f && h.feet.y > h.mob.position.y &&
                 std::abs(h.feet.x - h.mob.position.x) < (h.feet.y - h.mob.position.y); // within 45 degrees
    }
    CHECK(behind);
    CHECK_FALSE(h.frame().forward);
    REQUIRE(h.nav.go_to_point({20, 20, 0}, 2.0f, "back", true).empty());
    CHECK(h.nav.status().stick_spawn == 0);
    CHECK(h.nav.status().active);
}

namespace {
struct FakeNav : nav::NavControl {
    struct Call {
        std::string what;
        glm::vec3 at{0.0f};
        std::uint32_t id = 0;
        float distance = 0.0f;
        bool flag = false;
    };
    std::vector<Call> calls;
    nav::PathOptions opts;
    bool paused = false, shown = false;
    std::string go_to_point(const glm::vec3& w, float d, std::string, bool straight) override {
        calls.push_back({"point", w, 0, d, straight});
        return {};
    }
    std::string go_to_spawn(std::uint32_t id, float d) override {
        calls.push_back({"spawn", {}, id, d, false});
        return {};
    }
    std::string go_to_door(std::uint8_t id, bool click) override {
        calls.push_back({"door", {}, id, 0.0f, click});
        return {};
    }
    std::string stick(std::uint32_t id, const nav::StickOptions& o) override {
        calls.push_back({"stick", {}, id, o.distance, o.behind});
        return {};
    }
    void unstick(std::string_view) override { calls.push_back({"unstick"}); }
    void stop(std::string_view) override { calls.push_back({"stop"}); }
    void set_paused(bool p) override { paused = p; }
    nav::NavStatus status() const override {
        nav::NavStatus s;
        s.mesh_loaded = true;
        if (!calls.empty() && calls.back().what == "stick") {
            s.stick_spawn = calls.back().id;
            s.stick_distance = calls.back().distance > 0.0f ? calls.back().distance : 9.0f;
            return s;
        }
        s.active = !calls.empty() && calls.back().what != "stop" && calls.back().what != "unstick";
        s.paused = paused;
        s.goal = "a_rat01";
        s.tiles_done = 3;
        s.tiles_total = 4;
        return s;
    }
    std::optional<float> path_length(const glm::vec3& w) const override {
        return glm::length(w) < 1000.0f ? std::optional(glm::length(w)) : std::nullopt;
    }
    nav::PathOptions& options() override { return opts; }
    void rebuild() override { calls.push_back({"rebuild"}); }
    void set_overlay(bool on) override { shown = on; }
    bool overlay() const override { return shown; }
};

struct MqHost {
    mq_test::World world{"hailstone_nav_mq"};
    plugin::Hooks hooks;
    mq::Registry reg;
    mq::Context ctx;
    FakeNav nav;
    std::vector<std::string> printed;
    std::unique_ptr<mq::Commands> commands;
    MqHost() {
        mq::register_game_tlos(reg);
        ctx.registry = &reg;
        ctx.conn = world.conn.get();
        ctx.nav = &nav;
        mq::CommandEnv ce;
        ce.hooks = &hooks;
        ce.registry = &reg;
        ce.context = [this]() -> const mq::Context& { return ctx; };
        ce.echo = [this](const std::string& t) { printed.push_back(t); };
        ce.run = [this](const std::string& l) {
            std::string line = l;
            hooks.command(line);
        };
        commands = std::make_unique<mq::Commands>(ce);
    }
    ~MqHost() { commands.reset(); }
    void run(std::string line) { hooks.command(line); }
    std::string p(std::string_view text) { return mq::parse_macro_data(ctx, text); }
};
} // namespace

TEST_CASE("nav: /nav's destinations, options and controls") {
    MqHost h;
    h.run("/nav loc 10 20 3"); // /loc order: the server's y, x, z
    REQUIRE(h.nav.calls.size() == 1);
    CHECK(h.nav.calls[0].what == "point");
    CHECK(h.nav.calls[0].at == game::server_to_world({20, 10, 3}));
    CHECK_FALSE(h.nav.calls[0].flag);
    h.run("/nav locxyz 1 2 3");
    CHECK(h.nav.calls[1].at == game::server_to_world({1, 2, 3}));
    h.run("/nav id 10 distance=15");
    CHECK(h.nav.calls[2].what == "spawn");
    CHECK(h.nav.calls[2].id == 10);
    CHECK(h.nav.calls[2].distance == 15.0f);
    h.run("/nav tar"); // a word that begins only one subcommand stands for it
    CHECK(h.nav.calls[3].id == 10); // the test world's target
    h.run("/nav spawn npc Fippy");
    CHECK(h.nav.calls[4].id == 11);
    h.run("/nav pause");
    CHECK(h.nav.paused);
    h.run("/nav ui");
    CHECK(h.nav.shown);
    h.run("/nav option water off");
    CHECK(h.nav.opts.avoid_water);
    h.run("/nav option doors off");
    CHECK_FALSE(h.nav.opts.open_doors);
    h.run("/nav option clearance 4");
    CHECK(h.nav.opts.wall_clearance == 4.0f);
    h.run("/nav option clearance 40");
    CHECK(h.nav.opts.wall_clearance == 4.0f);
    CHECK(h.printed.back() == "[Nav] Clearance is a distance from 0 to 6.");
    h.run("/nav reload");
    CHECK(h.nav.calls.back().what == "rebuild");
    h.run("/nav stop");
    CHECK(h.nav.calls.back().what == "stop");
    h.run("/nav id 9999");
    CHECK(h.printed.back() == "[Nav] No such spawn.");
    // An id or door number no spawn or door can have is none, not a wrapped one (300 is not door 44).
    for (const char* line : {"/nav id -1", "/nav id 1e300", "/nav id nan"}) {
        h.run(line);
        CHECK(h.printed.back() == "[Nav] No such spawn.");
    }
    const std::size_t calls = h.nav.calls.size();
    h.run("/nav door id 300");
    h.run("/nav loc 1e300 0 0");
    h.run("/nav loc nan 0 0");
    CHECK(h.nav.calls.size() == calls);
    h.run("/nav bogus");
    CHECK(h.printed.back() == "[Nav] Unknown /nav command: bogus (/nav help)");
}

TEST_CASE("nav: /nav door's click on arrival needs 'actions'; the walk alone needs 'move'") {
    MqHost h;
    game::Door door;
    door.id = 5;
    door.name = "POKDOOR";
    net::GameConnectionTestAccess::doors(*h.world.conn).push_back(door);
    {
        plugin::CallerScope scope(plugin::kCapMove, "walker");
        h.run("/nav door id 5 click");
        CHECK(h.nav.calls.empty());
        CHECK(h.printed.back() == "[Nav] walker: /nav door ... click needs the server's 'actions' permission");
        h.run("/nav door pokdoor");
        REQUIRE(h.nav.calls.size() == 1);
        CHECK(h.nav.calls[0].what == "door");
        CHECK(h.nav.calls[0].id == 5);
        CHECK_FALSE(h.nav.calls[0].flag);
    }
    {
        plugin::CallerScope scope(plugin::kCapMove | plugin::kCapActions, "clicker");
        h.run("/nav door pokdoor click");
        REQUIRE(h.nav.calls.size() == 2);
        CHECK(h.nav.calls[1].flag);
    }
}

TEST_CASE("nav: /moveto walks straight") {
    MqHost h;
    h.run("/moveto loc 5,6");
    REQUIRE(h.nav.calls.size() == 1);
    CHECK(h.nav.calls[0].what == "point");
    CHECK(h.nav.calls[0].flag); // straight
    CHECK(h.nav.calls[0].at == game::server_to_world({6, 5, 0}));
    CHECK(h.nav.calls[0].distance == 10.0f);
    h.run("/moveto mdist 4");
    h.run("/moveto id 10");
    CHECK(h.nav.calls.back().distance == 4.0f);
    CHECK(h.nav.calls.back().flag);
    h.run("/moveto off");
    CHECK(h.nav.calls.back().what == "stop");
}

TEST_CASE("nav: /stick's words and the Stick TLO") {
    MqHost h;
    CHECK(h.p("${Stick.Active}") == "FALSE");
    CHECK(h.p("${Stick.Status}") == "OFF");
    h.run("/stick"); // the target, a_rat01 (10)
    REQUIRE(h.nav.calls.size() == 1);
    CHECK(h.nav.calls[0].what == "stick");
    CHECK(h.nav.calls[0].id == 10);
    CHECK(h.nav.calls[0].distance == 0.0f); // the default
    CHECK_FALSE(h.nav.calls[0].flag);
    CHECK(h.printed.back() == "You are now sticking to a rat (9).");
    CHECK(h.p("${Stick.Active}") == "TRUE");
    CHECK(h.p("${Stick}") == "ON");
    CHECK(h.p("${Stick.StickTarget}") == "10");
    CHECK(h.p("${Stick.StickTargetName}") == "a_rat01");
    CHECK(h.p("${Stick.Distance}") == "9.00");
    h.run("/stick 12 behind id 11");
    CHECK(h.nav.calls.back().id == 11);
    CHECK(h.nav.calls.back().distance == 12.0f);
    CHECK(h.nav.calls.back().flag);
    CHECK(h.p("${Stick.Distance}") == "12.00");
    h.run("/stick off");
    CHECK(h.nav.calls.back().what == "unstick");
    CHECK(h.p("${Stick.Status}") == "OFF");
    const std::size_t n = h.nav.calls.size();
    h.run("/stick sideways");
    CHECK(h.nav.calls.size() == n);
    CHECK(h.printed.back().starts_with("Usage: /stick"));
    net::GameConnectionTestAccess::world(*h.world.conn).target_id = 0;
    h.run("/stick");
    CHECK(h.nav.calls.size() == n);
    CHECK(h.printed.back() == "You must have a target to stick to.");
}

TEST_CASE("nav: the Navigation TLO") {
    MqHost h;
    CHECK(h.p("${Navigation.MeshLoaded}") == "TRUE");
    CHECK(h.p("${Navigation.Active}") == "FALSE");
    h.run("/nav id 10");
    CHECK(h.p("${Navigation.Active}") == "TRUE");
    CHECK(h.p("${Navigation.Destination}") == "a_rat01");
    CHECK(h.p("${Navigation.TilesBuilt}/${Navigation.TilesTotal}") == "3/4");
    CHECK(h.p("${Navigation.PathExists[id 10]}") == "TRUE");
    CHECK(h.p("${Navigation.PathExists[locxyz 5000 0 0]}") == "FALSE");
    CHECK(h.p("${Navigation.PathLength[locxyz 5000 0 0]}") == "-1.00");
    CHECK(h.p("${Navigation.PathLength[loc 0 3 4]}") == "5.00");
    h.ctx.nav = nullptr;
    CHECK(h.p("${Navigation}") == "NULL");
}

TEST_CASE("nav: an object parked far under the world is left out, so the mesh stays on the ground") {
    // Field of Bone parks the trees it does not use at z -32768. Taken in, they stretched the heightfield past
    // Recast's span heights and the whole mesh lay 32000 units under the zone.
    auto quad = [](assets::Mesh& m, float x0, float x1, float h) {
        auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (auto [x, y] : {std::pair{x0, x0}, {x1, x0}, {x1, x1}, {x0, x1}}) {
            assets::Vertex v;
            v.position = {x, y, h};
            m.vertices.push_back(v);
        }
        for (std::uint32_t i : {0u, 2u, 1u, 0u, 3u, 2u}) m.indices.push_back(base + i); // facing up
        m.materials.push_back({});
        m.materials[0].frames = {"x.bmp"};
        m.parts.push_back({0, static_cast<std::uint32_t>(m.indices.size()), 0});
    };
    assets::Zone zone;
    zone.terrain.emplace_back();
    quad(zone.terrain.back(), 0, 100, 0);
    quad(zone.objects["TREE"], -5, 5, 0);
    assets::Placement parked;
    parked.actor = "TREE";
    parked.transform = glm::translate(glm::mat4(1.0f), glm::vec3(50, 50, -32768));
    zone.placements.push_back(parked);
    game::CollisionMesh collision = game::CollisionMesh::build(zone);
    REQUIRE(collision.triangle_count() == 4); // movement still has the tree
    auto geo = nav::InputGeometry::from_collision(collision, {});
    CHECK(geo->triangle_count() == 2);
    CHECK(geo->bmin().y == doctest::Approx(0.0f)); // Recast's y is the world's z
    nav::ZoneNav::Config cfg;
    nav::ZoneNav z(geo, cfg, [](const glm::vec3&) { return nav::Area::Ground; });
    z.finish();
    std::optional<glm::vec3> on = z.mesh().nearest({50, 50, 0});
    REQUIRE(on.has_value());
    CHECK(std::abs(on->z) < 1.0f); // the ground, within a cell or two
}

// ---- Real zones (EQ_GAME_DIR) ----

TEST_CASE("nav: real zones build, and paths between their polygons keep a clear line") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    for (const char* name : {"qeynos2", "befallen", "anguish"}) {
        CAPTURE(name);
        assets::Zone zone = assets::load_zone(*game, name);
        game::CollisionMesh collision = game::CollisionMesh::build(zone);
        auto t0 = std::chrono::steady_clock::now();
        nav::ZoneNav::Config cfg;
        cfg.threads = 4;
        nav::ZoneNav z(nav::InputGeometry::from_collision(collision, {}), cfg,
                       [&zone](const glm::vec3& p) {
                           assets::RegionInfo r = zone.regions.lookup(p);
                           return r.is_water() ? nav::Area::Water : nav::Area::Ground;
                       });
        z.finish();
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE(std::string(name) << ": " << z.mesh().tile_count() << " of " << z.tiles_total() << " tiles in " << secs << " s");
        REQUIRE(z.mesh().tile_count() > 0);
        // Polygon middles, spread over the zone.
        std::vector<glm::vec3> mids;
        z.mesh().for_each_polygon([&](std::span<const glm::vec3> o, nav::Area) {
            glm::vec3 c(0.0f);
            for (const glm::vec3& v : o) c += v;
            mids.push_back(c / static_cast<float>(o.size()));
        });
        REQUIRE(mids.size() > 10);
        // The tiles arrive in whatever order the threads finish them: sorted, the same polygons are picked each run.
        std::sort(mids.begin(), mids.end(), [](const glm::vec3& a, const glm::vec3& b) {
            return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z;
        });
        int found = 0, segments = 0, clear = 0;
        for (std::size_t i = 0; i < 40; ++i) {
            const glm::vec3& a = mids[(i * 7919) % mids.size()];
            const glm::vec3& b = mids[(i * 104729 + 13) % mids.size()];
            nav::PathResult r = z.mesh().find_path(a, b);
            if (!r.found() || r.partial) continue;
            ++found;
            for (std::size_t k = 1; k < r.points.size(); ++k) {
                ++segments;
                // A standing body's middle, 3 above each corner.
                if (collision.line_of_sight(r.points[k - 1] + glm::vec3(0, 0, 3), r.points[k] + glm::vec3(0, 0, 3))) ++clear;
            }
        }
        MESSAGE(std::string(name) << ": " << found << " of 40 paths, " << clear << " of " << segments << " legs clear");
        CHECK(found > 0);
        if (segments > 0) CHECK(static_cast<double>(clear) / segments > 0.9);
    }
}

TEST_CASE("nav: the server's path is asked quietly: nothing drawn, and the reply goes to the navigation once") {
    test::Rig rig(test::MockConfig::standard(), test::quick_options());
    REQUIRE(rig.to_zone());
    auto requests = [&] {
        std::size_t n = 0;
        for (const auto& p : rig.server.zone_log)
            if (p.opcode == net::to_wire(net::Op::FindPersonRequest)) ++n;
        return n;
    };
    auto lease = rig.conn.request_quiet_path(101);
    REQUIRE(lease);
    REQUIRE(rig.until([&] { return requests() == 1; }));
    CHECK_FALSE(rig.conn.take_quiet_path(lease));
    std::vector<glm::vec3> path{{0, 0, 0}, {10, 5, 0}, {20, 5, 1}};
    rig.server.zone.send_reliable(net::encode_find_person_reply(path));
    std::optional<std::vector<glm::vec3>> got;
    REQUIRE(rig.until([&] {
        got = rig.conn.take_quiet_path(lease);
        return got.has_value();
    }));
    REQUIRE(got->size() == 4); // as sent: the destination first, EQEmu's repeated last point
    CHECK((*got)[0].x == doctest::Approx(20.0f));
    CHECK_FALSE(rig.conn.player_path().active()); // no find path shown
    CHECK(rig.conn.find_path().empty());
    CHECK_FALSE(rig.conn.take_quiet_path(lease)); // once
    // An empty reply: the server has no path.
    lease = rig.conn.request_quiet_path(102);
    REQUIRE(lease);
    REQUIRE(rig.until([&] { return requests() == 2; }));
    rig.server.zone.send_reliable(net::encode_find_person_reply({}));
    REQUIRE(rig.until([&] {
        got = rig.conn.take_quiet_path(lease);
        return got.has_value();
    }));
    CHECK(got->empty());
}

namespace {
// Two shores at z 0 either side of a trench (x 40..80) whose walls drop straight to a bottom at z -40, full of water
// up to z -1: the bottom cannot be walked to, so only swimming the surface crosses it.
struct WaterScene {
    Scene scene;
    std::vector<std::uint8_t> solid;
    assets::ZoneRegions regions =
        assets::ZoneRegions::from_boxes({{"AWT_trench", {60, 20, -20.5f}, {0, 0, 0}, {20, 20, 19.5f}}});
    WaterScene() {
        scene.floor(0, 0, 40, 40, 0);
        scene.floor(80, 0, 120, 40, 0);
        scene.floor(40, 0, 80, 40, -40);
        auto wall = [&](float x) {
            scene.tri({x, 0, -40}, {x, 40, -40}, {x, 40, 0}, false);
            scene.tri({x, 0, -40}, {x, 40, 0}, {x, 0, 0}, false);
        };
        wall(40);
        wall(80);
        solid.assign(scene.verts.size() / 3, 1);
        // The water's drawn surface: passable.
        scene.floor(40, 0, 80, 40, -1);
        solid.resize(scene.verts.size() / 3, 0);
    }
    bool water(const glm::vec3& p) const { return regions.lookup(p).is_water(); }
};
} // namespace

TEST_CASE("nav: water: a surface is swum across where the bottom cannot be walked to, and is marked Water") {
    WaterScene w;
    REQUIRE(w.water({60, 20, -5}));
    REQUIRE_FALSE(w.water({60, 20, 1}));
    auto classify = [&w](const glm::vec3& p) { return w.water(p) ? nav::Area::Water : nav::Area::Ground; };
    auto build = [&](bool with_water) {
        auto geo = nav::InputGeometry::from_world(w.scene.verts, w.solid, {},
                                                  with_water ? nav::InputGeometry::WaterTest(
                                                                   [&w](const glm::vec3& p) { return w.water(p); })
                                                             : nav::InputGeometry::WaterTest());
        nav::ZoneNav::Config cfg;
        auto z = std::make_unique<nav::ZoneNav>(geo, cfg, classify);
        z->finish();
        return z;
    };
    // Without the surface: no way across (the bottom is behind sheer walls).
    auto dry = build(false);
    CHECK(dry->mesh().find_path({20, 20, 0}, {100, 20, 0}).partial);
    // With it: across, along the surface.
    auto wet = build(true);
    nav::PathResult r = wet->mesh().find_path({20, 20, 0}, {100, 20, 0});
    REQUIRE(r.found());
    CHECK_FALSE(r.partial);
    for (const glm::vec3& p : r.points) CHECK(p.z > -3.0f); // never down to the bottom
    bool water_area = false;
    wet->mesh().for_each_polygon([&](std::span<const glm::vec3> o, nav::Area a) {
        glm::vec3 c(0.0f);
        for (const glm::vec3& v : o) c += v;
        c /= static_cast<float>(o.size());
        if (c.x > 45 && c.x < 75) {
            CHECK(a == nav::Area::Water);
            water_area = true;
        }
    });
    CHECK(water_area);
    nav::PathOptions dry_feet;
    dry_feet.avoid_water = true;
    CHECK(wet->mesh().find_path({20, 20, 0}, {100, 20, 0}, dry_feet).partial);
}

TEST_CASE("nav: links: a walk drops off a ledge it cannot climb back up, but not off one it would be hurt falling") {
    auto ledge = [](float height) {
        Scene s;
        s.floor(0, 0, 100, 100, 0);
        s.wall_box(0, 0, 40, 40, 0, height); // a block with a walkable top and sheer sides
        return built(s);
    };
    auto low = ledge(12.0f);
    nav::PathResult down = low->mesh().find_path({20, 20, 12}, {80, 20, 0});
    REQUIRE(down.found());
    CHECK_FALSE(down.partial);
    bool dropped = false;
    for (std::size_t i = 1; i < down.points.size(); ++i)
        if (down.points[i - 1].z - down.points[i].z > 10.0f) dropped = true;
    CHECK(dropped);
    CHECK(low->mesh().find_path({80, 20, 0}, {20, 20, 12}).partial); // one way: nothing goes back up
    bool drawn = false;
    low->mesh().for_each_link([&](const glm::vec3& a, const glm::vec3& b, bool both) {
        CHECK(a.z > b.z);
        CHECK_FALSE(both);
        drawn = true;
    });
    CHECK(drawn);
    // 40 onto dry ground hurts (the fall-damage formula): no link.
    auto high = ledge(40.0f);
    CHECK(high->mesh().find_path({20, 20, 40}, {80, 20, 0}).partial);
}

TEST_CASE("nav: links: a floor under deep water is joined to the surface over it, both ways") {
    WaterScene w;
    auto geo = nav::InputGeometry::from_world(w.scene.verts, w.solid, {},
                                              [&w](const glm::vec3& p) { return w.water(p); });
    nav::ZoneNav::Config cfg;
    nav::ZoneNav z(geo, cfg, [&w](const glm::vec3& p) { return w.water(p) ? nav::Area::Water : nav::Area::Ground; });
    z.finish();
    // The trench's floor, 40 under the surface.
    nav::PathResult down = z.mesh().find_path({20, 20, 0}, {60, 20, -40});
    REQUIRE(down.found());
    CHECK_FALSE(down.partial);
    CHECK(down.points.back().z == doctest::Approx(-40.0f).epsilon(0.05));
    nav::PathResult up = z.mesh().find_path({60, 20, -40}, {100, 20, 0});
    REQUIRE(up.found());
    CHECK_FALSE(up.partial);
    int swims = 0;
    z.mesh().for_each_link([&](const glm::vec3& a, const glm::vec3& b, bool both) {
        if (both && std::abs(a.z - b.z) > 30.0f) ++swims;
    });
    CHECK(swims > 0);
}

TEST_CASE("nav: a step a little over 3 is walked up") {
    Scene s;
    s.floor(0, 0, 50, 40, 0);
    s.wall_box(50, 0, 100, 40, 0, 3.6f);
    auto z = built(s);
    nav::PathResult r = z->mesh().find_path({20, 20, 0}, {80, 20, 3.6f});
    REQUIRE(r.found());
    CHECK_FALSE(r.partial);
}

TEST_CASE("nav: water: the look pitch while swimming") {
    WaterScene w;
    game::CollisionMesh collision = w.scene.collision();
    app::Navigator nav{app::Navigator::Env{}};
    nav.zone_loaded(collision, &w.regions, {});
    app::Navigator::Frame f;
    f.feet = {50, 20, -5};
    // Not swimming: the pitch is left alone.
    CHECK_FALSE(nav.swim_pitch(f, {70, 20, -1}).has_value());
    f.swimming = true;
    // At the surface (head out), the way ahead level: level.
    REQUIRE(nav.swim_pitch(f, {70, 20, -1}));
    CHECK(*nav.swim_pitch(f, {70, 20, -1}) == doctest::Approx(0.0f));
    // Head under: towards the corner (up to the surface ahead).
    f.head_under = true;
    f.feet = {50, 20, -15};
    CHECK(*nav.swim_pitch(f, {70, 20, -1}) > 0.3f);
    // A corner deeper down: dive.
    f.feet = {50, 20, -5};
    CHECK(*nav.swim_pitch(f, {60, 20, -30}) < -0.5f);
    // Out of breath: straight up, whatever the corner.
    f.air = 0.2f;
    CHECK(*nav.swim_pitch(f, {60, 20, -30}) > 0.9f);
    // A shore corner close ahead: up, for the swim jump out.
    f.air = 1.0f;
    f.head_under = false;
    f.feet = {75, 20, -5};
    CHECK(*nav.swim_pitch(f, {85, 20, 0}) > 0.5f);
    // ... but not one far off.
    f.feet = {45, 20, -5};
    CHECK(*nav.swim_pitch(f, {100, 20, 0}) == doctest::Approx(0.0f));
    // The head just under but no water a little over it (at the surface, it cannot rise): level towards a corner on
    // the surface, not pushing up against it for nothing.
    f.head_under = true;
    f.height = 3.5f;
    f.feet = {50, 20, -5};
    CHECK(*nav.swim_pitch(f, {60, 20, -1}) == doctest::Approx(0.0f));
    // Deeper, the same corner is risen to.
    f.feet = {50, 20, -15};
    CHECK(*nav.swim_pitch(f, {60, 20, -1}) > 0.3f);
}

TEST_CASE("nav: links: a drop into water that comes up near the ledge goes both ways, landing on the floor under it") {
    // A pool between two banks (z 0): its floor at -10, water from it to -1, and no surface drawn, so the drop off
    // a bank lands on the floor under the water. Out the far side is only by swimming up and climbing out.
    Scene s;
    s.floor(0, 0, 40, 40, 0);
    s.floor(80, 0, 120, 40, 0);
    s.floor(40, 0, 80, 40, -10);
    auto wall = [&](float x) {
        s.tri({x, 0, -10}, {x, 40, -10}, {x, 40, 0}, false);
        s.tri({x, 0, -10}, {x, 40, 0}, {x, 0, 0}, false);
    };
    wall(40);
    wall(80);
    assets::ZoneRegions regions =
        assets::ZoneRegions::from_boxes({{"AWT_pool", {60, 20, -5.5f}, {0, 0, 0}, {20, 20, 4.5f}}});
    auto geo = nav::InputGeometry::from_world(s.verts, {}, {},
                                              [&](const glm::vec3& p) { return regions.lookup(p).is_water(); });
    nav::ZoneNav::Config cfg;
    nav::ZoneNav z(geo, cfg, [&](const glm::vec3& p) { return regions.lookup(p).is_water() ? nav::Area::Water : nav::Area::Ground; });
    z.finish();
    nav::PathResult r = z.mesh().find_path({20, 20, 0}, {100, 20, 0});
    REQUIRE(r.found());
    CHECK_FALSE(r.partial);
    bool both = false;
    z.mesh().for_each_link([&](const glm::vec3& a, const glm::vec3& b, bool two_way) {
        if (two_way && a.z > -1.0f && b.z < -8.0f) both = true;
    });
    CHECK(both);
}

TEST_CASE("nav: under water: a shaft deeper than 200, swum by its open column, and a start floating high over the floor") {
    // All under water, as Kedge Keep is: a floor (z 0), and beside it a shaft down to a floor 300 under it, under a
    // roof over the shaft's far end, so the straight line from the shaft's floor to the ledge runs into the roof.
    Scene s;
    s.floor(0, 0, 40, 40, 0);
    s.floor(40, 0, 80, 40, -300);
    s.tri({40, 0, -300}, {40, 40, -300}, {40, 40, 0}, false);
    s.tri({40, 0, -300}, {40, 40, 0}, {40, 0, 0}, false);
    s.tri({80, 0, -300}, {80, 40, -300}, {80, 40, 50}, false);
    s.tri({80, 0, -300}, {80, 40, 50}, {80, 0, 50}, false);
    assets::ZoneRegions regions =
        assets::ZoneRegions::from_boxes({{"AWT_all", {50, 20, -125}, {0, 0, 0}, {100, 100, 200}}});
    auto water = [&](const glm::vec3& p) { return regions.lookup(p).is_water(); };
    auto geo = nav::InputGeometry::from_world(s.verts, {}, {}, water);
    nav::ZoneNav::Config cfg;
    nav::ZoneNav z(geo, cfg, [&](const glm::vec3& p) { return water(p) ? nav::Area::Water : nav::Area::Ground; });
    z.finish();
    // Down the shaft and back up.
    nav::PathResult down = z.mesh().find_path({20, 20, 0}, {60, 20, -300});
    REQUIRE(down.found());
    CHECK_FALSE(down.partial);
    nav::PathResult up = z.mesh().find_path({60, 20, -300}, {20, 20, 0});
    REQUIRE(up.found());
    CHECK_FALSE(up.partial);
    // By the open column: straight up over the shaft's floor to the ledge's height, then across.
    bool column = false;
    for (std::size_t i = 1; i < up.points.size(); ++i) {
        const glm::vec3& a = up.points[i - 1];
        const glm::vec3& b = up.points[i];
        if (a.x > 40.0f && b.x > 40.0f && glm::length(glm::vec2(a - b)) < 0.1f && b.z - a.z > 250.0f && std::abs(b.z) < 1.0f)
            column = true;
    }
    CHECK(column);
    // Avoiding water leaves it all out (every floor here is under water).
    nav::PathOptions dry;
    dry.avoid_water = true;
    nav::PathResult none = z.mesh().find_path({20, 20, 0}, {60, 20, -300}, dry);
    CHECK((!none.found() || none.partial));
    // A start floating 26 over the floor still finds the mesh under it.
    nav::PathResult floating = z.mesh().find_path({20, 20, 26}, {60, 20, -300});
    REQUIRE(floating.found());
    CHECK_FALSE(floating.partial);
}

TEST_CASE("nav: a stalled walk opens the door its way runs into (found by its model), and waits for it") {
    // A corridor (y 10..30) with a door across it at x 50. The door's origin is 40 off the doorway (a wide secret
    // wall's corner, as in Kedge Keep), out of the reach the origin test looks in; only its model is in the way.
    Scene s;
    s.floor(0, 10, 100, 30, 0);
    game::CollisionMesh collision = s.collision();
    game::Door door;
    door.id = 7;
    door.name = "SECRETWALL";
    door.state = game::kSwitchClosed;
    door.y = 50.0f; // world x
    door.x = 60.0f; // world y: 40 off the corridor's middle
    bool clicked = false;
    app::Navigator::Env env;
    env.door = [&](std::uint8_t id) -> const game::Door* { return id == door.id ? &door : nullptr; };
    env.doors = [&] { return std::vector<const game::Door*>{&door}; };
    double clicked_at = -1.0, t = 0.0;
    env.use_door = [&](std::uint8_t id) {
        if (id != door.id) return false;
        clicked = true;
        clicked_at = t;
        return true;
    };
    // The gate's model is in the way unless it has swung clear (gate_clear), whatever its state says.
    bool gate_clear = false;
    env.door_in_way = [&](const glm::vec3& a, const glm::vec3& b) -> std::optional<std::uint8_t> {
        bool crosses = (a.x - 50.0f) * (b.x - 50.0f) <= 0.0f;
        return !gate_clear && crosses ? std::optional<std::uint8_t>(door.id) : std::nullopt;
    };
    app::Navigator nav{env};
    nav.options().pass_open_doors = false; // the leaves block, as the original's collision has them
    nav.zone_loaded(collision, nullptr, {});
    glm::vec3 feet(20, 20, 0);
    REQUIRE(nav.go_to_point({80, 20, 0}, 2.0f, "past the door", false).empty());
    int unsticks_after_click = 0;
    auto wall_clock = std::chrono::steady_clock::now();
    while (nav.status().active && t < 60.0 && std::chrono::steady_clock::now() - wall_clock < std::chrono::seconds(30)) {
        app::Navigator::Frame f;
        f.feet = feet;
        f.now = t;
        // A gate as Kedge Keep's: its state says opening from the click and never open; it has swung clear 2.5 s
        // after the click, and closes again 6 s after it.
        if (clicked_at >= 0.0) {
            double since = t - clicked_at;
            door.state = since < 6.0 ? game::kSwitchOpening : game::kSwitchClosed;
            gate_clear = since >= 2.5 && since < 6.0;
        }
        nav::SteerOutput o = nav.frame(f);
        if (clicked_at >= 0.0 && !gate_clear && (o.jump || o.strafe != 0)) ++unsticks_after_click;
        if (nav.status().mesh_complete) {
            feet = move(feet, o, 15.0f, 0.05f, [&](const glm::vec3& p) { return !gate_clear && p.x > 48.0f && feet.x <= 50.0f; });
            t += 0.05;
        } else {
            std::this_thread::yield();
        }
    }
    CHECK(clicked);
    CHECK(unsticks_after_click == 0); // it held still for the gate
    CHECK_FALSE(nav.status().active);
    CHECK(glm::length(glm::vec2(feet) - glm::vec2(80, 20)) <= 2.5f);
    // Clicked at the first sign of the stall (at the door after about 1.9 s, stalled a second later), not after
    // rounds of jumping and strafing (they would put it past 6).
    CHECK(clicked_at < 4.0);

    SUBCASE("a door that never opens is named when the walk gives up") {
        std::vector<std::string> said;
        app::Navigator::Env locked = env;
        locked.chat = [&](const std::string& line) { said.push_back(line); };
        locked.use_door = [&](std::uint8_t) { return true; }; // clicked, stays shut
        door.state = game::kSwitchClosed;
        gate_clear = false;
        app::Navigator nav2{locked};
        nav2.zone_loaded(collision, nullptr, {});
        // The log of the stuck walk: where it stood and the doors about.
        hailstone::test::ScratchDir logs("nav_stuck_log");
        const std::filesystem::path log_file = logs.path() / "nav.log";
        REQUIRE(log::open_file(log_file));
        glm::vec3 at(20, 20, 0);
        REQUIRE(nav2.go_to_point({80, 20, 0}, 2.0f, "past the door", false).empty());
        double t2 = 0.0;
        auto clock2 = std::chrono::steady_clock::now();
        while (nav2.status().active && t2 < 120.0 && std::chrono::steady_clock::now() - clock2 < std::chrono::seconds(30)) {
            app::Navigator::Frame f;
            f.feet = at;
            f.now = t2;
            nav::SteerOutput o = nav2.frame(f);
            if (nav2.status().mesh_complete) {
                at = move(at, o, 15.0f, 0.05f, [](const glm::vec3& p) { return p.x > 48.0f; });
                t2 += 0.05;
            } else {
                std::this_thread::yield();
            }
        }
        log::close_file();
        CHECK_FALSE(nav2.status().active);
        std::ifstream in(log_file);
        std::string logged((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(logged.find("nav: stuck at (") != std::string::npos);
        CHECK(logged.find("door 7 (SECRETWALL)") != std::string::npos);
        CHECK(logged.find("the way on runs into door 7") != std::string::npos);
        bool named = false;
        for (const std::string& line : said)
            if (line.find("the door SECRETWALL did not open") != std::string::npos) named = true;
        CHECK(named);
    }
}

TEST_CASE("nav: a closed door on the path ahead is clicked once in reach, without stopping") {
    // The corridor (y 10..30) with a door across it at x 50 that is in reach within 25 of it (Env::door_reachable)
    // and swings clear `swing` seconds after its click, then stays open.
    Scene s;
    s.floor(0, 10, 100, 30, 0);
    game::CollisionMesh collision = s.collision();
    // trapped: a walker within 12 of the door while it swings is caught by the leaf and moves no more (bool* to
    // report it; null: the leaf passes through, as before).
    auto run = [&](double swing, double& clicked_at, float& clicked_from, int& clicks, int& unsticks, bool& arrived,
                   bool reach_hook, bool* trapped = nullptr) {
        game::Door door;
        door.id = 9;
        door.name = "DOOR1";
        door.state = game::kSwitchClosed;
        door.y = 50.0f; // world x
        door.x = 20.0f; // world y: the corridor's middle
        double t = 0.0;
        glm::vec3 feet(5, 20, 0);
        app::Navigator::Env env;
        env.door = [&](std::uint8_t id) -> const game::Door* { return id == door.id ? &door : nullptr; };
        env.doors = [&] { return std::vector<const game::Door*>{&door}; };
        env.use_door = [&](std::uint8_t id) {
            if (id != door.id) return false;
            ++clicks;
            if (clicked_at < 0.0) {
                clicked_at = t;
                clicked_from = feet.x;
            }
            door.state = game::kSwitchOpening;
            return true;
        };
        if (reach_hook) env.door_reachable = [&](std::uint8_t) { return std::abs(feet.x - 50.0f) <= 25.0f; };
        auto clear = [&] { return clicked_at >= 0.0 && t - clicked_at >= swing; };
        env.door_in_way = [&](const glm::vec3& a, const glm::vec3& b) -> std::optional<std::uint8_t> {
            bool crosses = (a.x - 50.0f) * (b.x - 50.0f) <= 0.0f;
            return !clear() && crosses ? std::optional<std::uint8_t>(door.id) : std::nullopt;
        };
        app::Navigator nav{env};
        nav.options().pass_open_doors = false; // the leaves block, as the original's collision has them
        nav.zone_loaded(collision, nullptr, {});
        REQUIRE(nav.go_to_point({90, 20, 0}, 2.0f, "past the door", false).empty());
        auto wall_clock = std::chrono::steady_clock::now();
        while (nav.status().active && t < 60.0 && std::chrono::steady_clock::now() - wall_clock < std::chrono::seconds(30)) {
            if (clear()) door.state = game::kSwitchOpen;
            app::Navigator::Frame f;
            f.feet = feet;
            f.now = t;
            nav::SteerOutput o = nav.frame(f);
            if (clicked_at >= 0.0 && (o.jump || o.strafe != 0)) ++unsticks;
            if (nav.status().mesh_complete) {
                bool swinging = clicked_at >= 0.0 && !clear();
                if (trapped && swinging && glm::length(glm::vec2(feet) - glm::vec2(50, 20)) < 12.0f) *trapped = true;
                if (!(trapped && *trapped))
                    feet = move(feet, o, 15.0f, 0.05f,
                                [&](const glm::vec3& p) { return !clear() && p.x > 48.0f && feet.x <= 50.0f; });
                t += 0.05;
            } else {
                std::this_thread::yield();
            }
        }
        arrived = !nav.status().active && glm::length(glm::vec2(feet) - glm::vec2(90, 20)) <= 2.5f;
        return t;
    };
    SUBCASE("still swinging: the walk keeps out of the door's swing until it has swung (a leaf traps a walker)") {
        // The Plane of Hate's side doors: clicked ahead, the walk went on into the doorway while the leaf swung,
        // and was caught by it.
        double at = -1.0;
        float from = 0.0f;
        int clicks = 0, unsticks = 0;
        bool arrived = false, trapped = false;
        run(2.5, at, from, clicks, unsticks, arrived, true, &trapped);
        CHECK_FALSE(trapped);
        CHECK(arrived);
        CHECK(clicks == 1);
    }
    SUBCASE("clicked at the stall, inside the swing: the walk steps back out of it while the door swings") {
        double at = -1.0;
        float from = 0.0f;
        int clicks = 0, unsticks = 0;
        bool arrived = false;
        run(2.5, at, from, clicks, unsticks, arrived, false);
        CHECK(from > 45.0f); // at the door
        CHECK(arrived);
        CHECK(clicks == 1);
    }
    SUBCASE("clicked from a distance, and open by the time the walk gets there") {
        double at = -1.0;
        float from = 0.0f;
        int clicks = 0, unsticks = 0;
        bool arrived = false;
        double took = run(1.0, at, from, clicks, unsticks, arrived, true);
        CHECK(arrived);
        CHECK(clicks == 1);
        CHECK(from <= 30.0f);  // as soon as it was in reach, well before the door
        CHECK(from >= 24.0f);  // not before (no "You can't reach that")
        CHECK(unsticks == 0);
        CHECK(took < 7.5);     // 85 units at 15 a second, with no stop at the door
    }
    SUBCASE("still swinging at the doorway: the walk holds for it, no jumping, no second click") {
        double at = -1.0;
        float from = 0.0f;
        int clicks = 0, unsticks = 0;
        bool arrived = false;
        run(3.5, at, from, clicks, unsticks, arrived, true);
        CHECK(arrived);
        CHECK(clicks == 1);
        CHECK(unsticks == 0);
    }
    SUBCASE("with no reach to go by, the door is clicked at the stall as before") {
        double at = -1.0;
        float from = 0.0f;
        int clicks = 0, unsticks = 0;
        bool arrived = false;
        run(1.0, at, from, clicks, unsticks, arrived, false);
        CHECK(arrived);
        CHECK(clicks == 1);
        CHECK(from > 45.0f);
    }
}

TEST_CASE("nav: an open door's leaf in the way is walked round, and a door a walk opened is never clicked shut") {
    // The corridor (y 10..30) with an open door at x 50 whose leaf stands across the side of it the walk takes
    // (y 10..19); the other side is clear.
    Scene s;
    s.floor(0, 10, 100, 30, 0);
    game::CollisionMesh collision = s.collision();
    game::Door door;
    door.id = 5;
    door.name = "HPMDR401";
    door.state = game::kSwitchOpen;
    door.y = 50.0f;
    door.x = 14.0f;
    int clicks = 0;
    bool swing = false; // a click's answer (OP_MoveDoor) comes on the next frame
    float leaf_top = 19.0f;
    app::Navigator::Env env;
    env.door = [&](std::uint8_t id) -> const game::Door* { return id == door.id ? &door : nullptr; };
    env.doors = [&] { return std::vector<const game::Door*>{&door}; };
    env.use_door = [&](std::uint8_t id) {
        if (id != door.id) return false;
        ++clicks;
        swing = true;
        return true;
    };
    // The sphere (radius 2) meets the leaf where the segment crosses x 50 below its top.
    env.door_in_way = [&](const glm::vec3& a, const glm::vec3& b) -> std::optional<std::uint8_t> {
        if ((a.x - 50.0f) * (b.x - 50.0f) > 0.0f && std::abs(a.x - 50.0f) > 2.0f && std::abs(b.x - 50.0f) > 2.0f)
            return std::nullopt;
        float t = std::abs(b.x - a.x) < 0.01f ? 0.0f : (50.0f - a.x) / (b.x - a.x);
        t = std::clamp(t, 0.0f, 1.0f);
        float y = a.y + (b.y - a.y) * t;
        return y < leaf_top + 2.0f ? std::optional<std::uint8_t>(door.id) : std::nullopt;
    };
    auto walk = [&](app::Navigator& nav, glm::vec3 feet, glm::vec3 to, double& t, double limit) {
        REQUIRE(nav.go_to_point(to, 2.0f, "past the door", false).empty());
        auto wall_clock = std::chrono::steady_clock::now();
        while (nav.status().active && t < limit && std::chrono::steady_clock::now() - wall_clock < std::chrono::seconds(30)) {
            if (swing) {
                door.state = door.state == game::kSwitchOpen ? game::kSwitchClosed : game::kSwitchOpen;
                swing = false;
            }
            app::Navigator::Frame f;
            f.feet = feet;
            f.now = t;
            nav::SteerOutput o = nav.frame(f);
            if (nav.status().mesh_complete) {
                feet = move(feet, o, 15.0f, 0.05f,
                            [&](const glm::vec3& p) { return std::abs(p.x - 50.0f) < 2.0f && p.y < leaf_top + 1.8f; });
                t += 0.05;
            } else {
                std::this_thread::yield();
            }
        }
        return feet;
    };
    SUBCASE("round the leaf, not clicked") {
        app::Navigator nav{env};
        nav.options().pass_open_doors = false; // the leaves block, as the original's collision has them
        nav.zone_loaded(collision, nullptr, {});
        double t = 0.0;
        glm::vec3 end = walk(nav, {20, 14, 0}, {80, 14, 0}, t, 30.0);
        CHECK(glm::length(glm::vec2(end) - glm::vec2(80, 14)) <= 2.5f);
        CHECK(clicks == 0);
    }
    SUBCASE("a door that blocks the way open and shut is opened, shut, opened again once, and then left") {
        // Shut at first, and once open its leaf blocks the whole corridor: no way round, so it is shut to give way;
        // shut, it is in the way again, so it is opened once more (a door shut to give way that is then in the way
        // shut is one being walked through), and no more (it is not swung open and shut for ever), also by the next
        // walk.
        door.state = game::kSwitchClosed;
        leaf_top = 40.0f;
        app::Navigator nav{env};
        nav.options().pass_open_doors = false; // the leaves block, as the original's collision has them
        nav.zone_loaded(collision, nullptr, {});
        double t = 0.0;
        walk(nav, {20, 20, 0}, {80, 20, 0}, t, 40.0);
        CHECK(clicks == 3);
        CHECK(door.state == game::kSwitchOpen);
        walk(nav, {40, 20, 0}, {80, 20, 0}, t, 80.0); // a script starts another walk
        CHECK(clicks == 3);
        // A zone (re)load forgets them: door ids are the zone's, so a door there is clicked afresh.
        nav.zone_loaded(collision, nullptr, {});
        walk(nav, {40, 20, 0}, {80, 20, 0}, t, 120.0);
        CHECK(clicks > 3);
    }
}

TEST_CASE("nav: a door a walk opened that then swings across the passage being walked is shut, once") {
    // A corridor (y 10..30) with a room off it to the north (x 10..30, y 30..50) through a door in the doorway at
    // y 30. Shut, the door fills the doorway; open, its leaf stands across the corridor at x 50 (the Plane of
    // Hate's side doors).
    Scene s;
    s.floor(0, 10, 100, 30, 0);
    s.floor(10, 30, 30, 50, 0);
    game::CollisionMesh collision = s.collision();
    game::Door door;
    door.id = 62;
    door.name = "HPSDR401SM";
    door.state = game::kSwitchClosed;
    door.y = 20.0f;
    door.x = 30.0f;
    int clicks = 0;
    bool swing = false;
    // The server shut the door after its close timer without a word (EQEmu does for most doors): the walk's click
    // to shut it opens it there, and the answer leaves it open here, with a new timestamp.
    bool quiet_shut = false;
    SUBCASE("the server answers each click") {}
    SUBCASE("the server had shut the door quietly: the click to shut it is answered open, and clicked again") {
        quiet_shut = true;
    }
    int extra = quiet_shut ? 1 : 0;
    app::Navigator::Env env;
    env.door = [&](std::uint8_t id) -> const game::Door* { return id == door.id ? &door : nullptr; };
    env.doors = [&] { return std::vector<const game::Door*>{&door}; };
    env.use_door = [&](std::uint8_t id) {
        if (id != door.id) return false;
        ++clicks;
        if (quiet_shut && clicks == 2) {
            ++door.timestamp; // answered open, as it was
            return true;
        }
        swing = true;
        return true;
    };
    auto crosses = [](float a, float b, float at) { return (a - at) * (b - at) <= 0.0f || std::abs(a - at) < 2.0f; };
    env.door_in_way = [&](const glm::vec3& a, const glm::vec3& b) -> std::optional<std::uint8_t> {
        bool open = door.state == game::kSwitchOpen;
        if (open && crosses(a.x, b.x, 50.0f) && std::min(a.y, b.y) < 30.0f) return door.id;
        if (!open && crosses(a.y, b.y, 30.0f) && std::max(a.x, b.x) > 8.0f && std::min(a.x, b.x) < 32.0f) return door.id;
        return std::nullopt;
    };
    // The corridor's and the room's walls: a line leaving their floors is out of sight.
    auto on_floor = [](const glm::vec3& p) {
        return (p.y >= 10.0f && p.y <= 30.0f) || (p.x >= 10.0f && p.x <= 30.0f && p.y >= 30.0f && p.y <= 50.0f);
    };
    env.line_of_sight = [&](const glm::vec3& a, const glm::vec3& b) {
        for (int i = 0; i <= 20; ++i)
            if (!on_floor(a + (b - a) * (static_cast<float>(i) / 20.0f))) return false;
        return true;
    };
    auto blocked = [&](const glm::vec3& p) {
        bool open = door.state == game::kSwitchOpen;
        if (open) return std::abs(p.x - 50.0f) < 2.0f && p.y < 30.0f;
        return std::abs(p.y - 30.0f) < 1.5f && p.x > 10.0f && p.x < 30.0f;
    };
    app::Navigator nav{env};
    nav.options().pass_open_doors = false; // the leaves block, as the original's collision has them
    nav.zone_loaded(collision, nullptr, {});
    double t = 0.0;
    auto walk = [&](glm::vec3 feet, glm::vec3 to, double limit) {
        REQUIRE(nav.go_to_point(to, 2.0f, "on", false).empty());
        auto wall_clock = std::chrono::steady_clock::now();
        while (nav.status().active && t < limit && std::chrono::steady_clock::now() - wall_clock < std::chrono::seconds(30)) {
            if (swing) {
                door.state = door.state == game::kSwitchOpen ? game::kSwitchClosed : game::kSwitchOpen;
                swing = false;
            }
            app::Navigator::Frame f;
            f.feet = feet;
            f.now = t;
            nav::SteerOutput o = nav.frame(f);
            if (nav.status().mesh_complete) {
                feet = move(feet, o, 15.0f, 0.05f, blocked);
                t += 0.05;
            } else {
                std::this_thread::yield();
            }
        }
        return feet;
    };
    // Into the room: the door is opened, and its leaf swings across the corridor.
    glm::vec3 at = walk({20, 20, 0}, {20, 42, 0}, 40.0);
    CHECK(glm::length(glm::vec2(at) - glm::vec2(20, 42)) <= 2.5f);
    CHECK(clicks == 1);
    CHECK(door.state == game::kSwitchOpen);
    // Out and along the corridor: the leaf is in the way with no way round it, so the door is shut, and the walk
    // gets there.
    at = walk(at, {80, 20, 0}, 100.0);
    CHECK(glm::length(glm::vec2(at) - glm::vec2(80, 20)) <= 2.5f);
    CHECK(clicks == 2 + extra);
    CHECK(door.state == game::kSwitchClosed);
    // Back into the room soon after (two clicks in kDoorOpenedSeconds already): the door shut to give way is shut
    // across the way now, so it is opened again, once, and the walk gets there (it stood shut against it before).
    REQUIRE(t < 100.0);
    at = walk(at, {20, 42, 0}, 160.0);
    CHECK(glm::length(glm::vec2(at) - glm::vec2(20, 42)) <= 2.5f);
    CHECK(clicks == 3 + extra);
    CHECK(door.state == game::kSwitchOpen);
}

TEST_CASE("nav: a gate in pieces and doors that block while reading open are each clicked, once") {
    // As in Kedge Keep: KEDGRATE2 in two pieces across the corridor (x 50, and x 52 resting open yet in the way),
    // then the secret door KEDBOX at x 70, resting open yet shut. Each swings clear a second after its click.
    Scene s;
    s.floor(0, 10, 100, 30, 0);
    game::CollisionMesh collision = s.collision();
    struct Piece {
        game::Door door;
        float x;
        double clicked_at = -1.0;
        int clicks = 0;
    };
    std::vector<Piece> pieces(3);
    auto setup = [](Piece& p, std::uint8_t id, const char* name, std::uint8_t state, float x) {
        p.door.id = id;
        p.door.name = name;
        p.door.state = state;
        p.door.y = x;
        p.door.x = 60.0f; // origin off the corridor
        p.x = x;
    };
    setup(pieces[0], 7, "KEDGRATE2", game::kSwitchClosed, 50.0f);
    setup(pieces[1], 8, "KEDGRATE2", game::kSwitchOpen, 52.0f);
    setup(pieces[2], 18, "KEDBOX", game::kSwitchOpen, 70.0f);
    double t = 0.0;
    auto clear = [&](const Piece& p) { return p.clicked_at >= 0.0 && t - p.clicked_at >= 1.0; };
    app::Navigator::Env env;
    env.door = [&](std::uint8_t id) -> const game::Door* {
        for (Piece& p : pieces)
            if (p.door.id == id) return &p.door;
        return nullptr;
    };
    env.doors = [&] {
        std::vector<const game::Door*> v;
        for (Piece& p : pieces) v.push_back(&p.door);
        return v;
    };
    env.use_door = [&](std::uint8_t id) {
        for (Piece& p : pieces)
            if (p.door.id == id) {
                ++p.clicks;
                p.clicked_at = t;
                p.door.state = p.door.state == game::kSwitchOpen ? game::kSwitchClosing : game::kSwitchOpening;
                return true;
            }
        return false;
    };
    env.door_in_way = [&](const glm::vec3& a, const glm::vec3& b) -> std::optional<std::uint8_t> {
        std::optional<std::uint8_t> hit;
        float nearest = 1e9f;
        for (const Piece& p : pieces)
            if (!clear(p) && (a.x - p.x) * (b.x - p.x) <= 0.0f && std::abs(p.x - a.x) < nearest) {
                nearest = std::abs(p.x - a.x);
                hit = p.door.id;
            }
        return hit;
    };
    app::Navigator nav{env};
    nav.options().pass_open_doors = false; // the leaves block, as the original's collision has them
    nav.zone_loaded(collision, nullptr, {});
    glm::vec3 feet(20, 20, 0);
    REQUIRE(nav.go_to_point({90, 20, 0}, 2.0f, "past the doors", false).empty());
    auto wall_clock = std::chrono::steady_clock::now();
    while (nav.status().active && t < 90.0 && std::chrono::steady_clock::now() - wall_clock < std::chrono::seconds(30)) {
        app::Navigator::Frame f;
        f.feet = feet;
        f.now = t;
        nav::SteerOutput o = nav.frame(f);
        if (nav.status().mesh_complete) {
            feet = move(feet, o, 15.0f, 0.05f, [&](const glm::vec3& p) {
                for (const Piece& piece : pieces)
                    if (!clear(piece) && feet.x <= piece.x - 2.0f && p.x > piece.x - 2.0f) return true;
                return false;
            });
            t += 0.05;
        } else {
            std::this_thread::yield();
        }
    }
    CHECK_FALSE(nav.status().active);
    CHECK(glm::length(glm::vec2(feet) - glm::vec2(90, 20)) <= 2.5f);
    for (const Piece& p : pieces) {
        CAPTURE(int(p.door.id));
        CHECK(p.clicks == 1); // each clicked, and only once (a second click would swing it back)
    }
}

TEST_CASE("nav: passing the doors not shut, a walk goes through an open leaf, and clicks a shut door once or twice") {
    // The give-way corridor: a corridor (y 10..30) with a room off it to the north (x 10..30, y 30..50) through a
    // door at y 30. Shut, the door fills the doorway; open, its leaf stands across the corridor at x 50. With the
    // doors not shut passed (the default, PathOptions::pass_open_doors), the movement goes through an open leaf, as
    // the client's sweep does then.
    Scene s;
    s.floor(0, 10, 100, 30, 0);
    s.floor(10, 30, 30, 50, 0);
    game::CollisionMesh collision = s.collision();
    game::Door door;
    door.id = 62;
    door.name = "HPSDR401SM";
    door.state = game::kSwitchClosed;
    door.y = 20.0f;
    door.x = 30.0f;
    int clicks = 0;
    bool swing = false;
    // The server had the door the other way (opened by an NPC, out of this client's hearing): the first click is
    // answered with the state the client shows.
    bool server_other_way = false;
    SUBCASE("the server agrees with the client") {}
    SUBCASE("the server had the door open: the first click shuts it there, and the walk clicks again") {
        server_other_way = true;
    }
    app::Navigator::Env env;
    env.door = [&](std::uint8_t id) -> const game::Door* { return id == door.id ? &door : nullptr; };
    env.doors = [&] { return std::vector<const game::Door*>{&door}; };
    env.use_door = [&](std::uint8_t id) {
        if (id != door.id) return false;
        ++clicks;
        if (server_other_way && clicks == 1) {
            ++door.timestamp; // answered shut, as it was
            return true;
        }
        swing = true;
        return true;
    };
    auto crosses = [](float a, float b, float at) { return (a - at) * (b - at) <= 0.0f || std::abs(a - at) < 2.0f; };
    env.door_in_way = [&](const glm::vec3& a, const glm::vec3& b) -> std::optional<std::uint8_t> {
        bool open = door.state == game::kSwitchOpen;
        if (open && crosses(a.x, b.x, 50.0f) && std::min(a.y, b.y) < 30.0f) return door.id;
        if (!open && crosses(a.y, b.y, 30.0f) && std::max(a.x, b.x) > 8.0f && std::min(a.x, b.x) < 32.0f) return door.id;
        return std::nullopt;
    };
    auto on_floor = [](const glm::vec3& p) {
        return (p.y >= 10.0f && p.y <= 30.0f) || (p.x >= 10.0f && p.x <= 30.0f && p.y >= 30.0f && p.y <= 50.0f);
    };
    env.line_of_sight = [&](const glm::vec3& a, const glm::vec3& b) {
        for (int i = 0; i <= 20; ++i)
            if (!on_floor(a + (b - a) * (static_cast<float>(i) / 20.0f))) return false;
        return true;
    };
    app::Navigator nav{env};
    REQUIRE(nav.options().pass_open_doors);
    // Only a shut door blocks while the walk passes the others.
    auto blocked = [&](const glm::vec3& p) {
        if (door.state == game::kSwitchOpen && nav.passes_open_doors()) return false;
        if (door.state == game::kSwitchOpen) return std::abs(p.x - 50.0f) < 2.0f && p.y < 30.0f;
        return std::abs(p.y - 30.0f) < 1.5f && p.x > 10.0f && p.x < 30.0f;
    };
    nav.zone_loaded(collision, nullptr, {});
    double t = 0.0;
    auto walk = [&](glm::vec3 feet, glm::vec3 to, double limit) {
        REQUIRE(nav.go_to_point(to, 2.0f, "on", false).empty());
        auto wall_clock = std::chrono::steady_clock::now();
        while (nav.status().active && t < limit && std::chrono::steady_clock::now() - wall_clock < std::chrono::seconds(30)) {
            if (swing) {
                door.state = door.state == game::kSwitchOpen ? game::kSwitchClosed : game::kSwitchOpen;
                ++door.timestamp;
                swing = false;
            }
            app::Navigator::Frame f;
            f.feet = feet;
            f.now = t;
            nav::SteerOutput o = nav.frame(f);
            if (nav.status().mesh_complete) {
                feet = move(feet, o, 15.0f, 0.05f, blocked);
                t += 0.05;
            } else {
                std::this_thread::yield();
            }
        }
        return feet;
    };
    const int extra = server_other_way ? 1 : 0;
    // Into the room: the shut door is clicked (twice when the server had it open), and its leaf swings across the
    // corridor.
    glm::vec3 at = walk({20, 20, 0}, {20, 42, 0}, 40.0);
    CHECK(glm::length(glm::vec2(at) - glm::vec2(20, 42)) <= 2.5f);
    CHECK(clicks == 1 + extra);
    CHECK(door.state == game::kSwitchOpen);
    // Out and along the corridor: through the open leaf, never clicked shut.
    at = walk(at, {80, 20, 0}, 100.0);
    CHECK(glm::length(glm::vec2(at) - glm::vec2(80, 20)) <= 2.5f);
    CHECK(clicks == 1 + extra);
    CHECK(door.state == game::kSwitchOpen);
    // Off again: the leaf blocks walks with the option off (and the player's own movement always).
    CHECK_FALSE(nav.passes_open_doors()); // no walk
}

TEST_CASE("nav: an unsticking strafe never steps off a walkway") {
    Scene s;
    s.floor(0, 0, 100, 100, 0);
    s.wall_box(0, 17, 100, 23, 0, 20); // a walkway 6 wide, 20 up
    game::CollisionMesh collision = s.collision();
    app::Navigator nav{app::Navigator::Env{}};
    nav.zone_loaded(collision, nullptr, {});
    app::Navigator::Frame f;
    nav::SteerOutput o;
    o.active = true;
    o.yaw = 0.0f; // along the walkway: its sides are either way in y
    o.strafe = 1;
    f.feet = {50, 20, 20};
    CHECK(nav.safe_strafe(f, o) == 0);
    o.strafe = -1;
    CHECK(nav.safe_strafe(f, o) == 0);
    // On open ground the strafe stands.
    f.feet = {50, 60, 0};
    CHECK(nav.safe_strafe(f, o) == -1);
    // Across the walkway's width (sides along it): it stands there too.
    f.feet = {50, 20, 20};
    o.yaw = 1.5707963f;
    CHECK(nav.safe_strafe(f, o) == -1);
}

TEST_CASE("nav: a zone line's volume is marked exactly, so paths go round it, not through a polygon that overlaps it") {
    Scene s;
    s.floor(0, 0, 100, 100, 0);
    // A thin zone line across the room (x 48..52) from y 0 to 60.
    std::vector<nav::AreaBox> boxes{{{48, 0, -5}, {52, 60, 20}, nav::Area::ZoneLine}};
    auto geo = nav::InputGeometry::from_world(s.verts, {}, {}, {}, boxes);
    nav::ZoneNav z(geo, {}, ground);
    z.finish();
    bool marked = false;
    z.mesh().for_each_polygon([&](std::span<const glm::vec3> o, nav::Area a) {
        if (a != nav::Area::ZoneLine) return;
        marked = true;
        for (const glm::vec3& v : o) CHECK((v.x > 47.0f && v.x < 53.0f && v.y < 61.0f)); // within the box
    });
    CHECK(marked);
    nav::PathResult r = z.mesh().find_path({20, 20, 0}, {80, 20, 0});
    REQUIRE(r.found());
    CHECK_FALSE(r.partial);
    // Round the end of the line (past y 60), not across it.
    for (std::size_t i = 1; i < r.points.size(); ++i) {
        const glm::vec3 &a = r.points[i - 1], &b = r.points[i];
        if ((a.x - 50.0f) * (b.x - 50.0f) < 0.0f) { // this leg crosses x = 50
            float t = (50.0f - a.x) / (b.x - a.x);
            CHECK(a.y + t * (b.y - a.y) > 60.0f);
        }
    }
    nav::PathOptions through;
    through.allow_zone_lines = true;
    CHECK(length(z.mesh().find_path({20, 20, 0}, {80, 20, 0}, through).points) < 65.0f);
}

TEST_CASE("nav: a walk stops before it steps into a zone line, unless zone lines are allowed") {
    Scene s;
    s.floor(0, 0, 100, 100, 0);
    game::CollisionMesh collision = s.collision();
    // A zone line the mesh does not know about (no AreaBox from these regions: an EQG box is found by lookup, and
    // the walk is straight), right across the way.
    assets::ZoneRegions regions = assets::ZoneRegions::from_boxes({{"ATP_1", {50, 50, 5}, {0, 0, 0}, {2, 50, 10}}});
    REQUIRE(regions.lookup({50, 20, 2}).is_zone_line());
    std::vector<std::string> chat;
    app::Navigator::Env env;
    env.chat = [&chat](const std::string& t) { chat.push_back(t); };
    app::Navigator nav{env};
    nav.zone_loaded(collision, &regions, {});
    glm::vec3 feet(20, 20, 0);
    REQUIRE(nav.go_to_point({80, 20, 0}, 2.0f, "the far side", true).empty());
    double t = 0.0;
    while (nav.status().active && t < 30.0) {
        app::Navigator::Frame f;
        f.feet = feet;
        f.now = t;
        feet = move(feet, nav.frame(f), 15.0f, 0.05f);
        t += 0.05;
    }
    CHECK(feet.x < 48.0f); // stopped short
    REQUIRE_FALSE(chat.empty());
    CHECK(chat.back().find("Stopped at a zone line") != std::string::npos);
    // Allowed: through.
    nav.options().allow_zone_lines = true;
    REQUIRE(nav.go_to_point({80, 20, 0}, 2.0f, "the far side", true).empty());
    while (nav.status().active && t < 60.0) {
        app::Navigator::Frame f;
        f.feet = feet;
        f.now = t;
        feet = move(feet, nav.frame(f), 15.0f, 0.05f);
        t += 0.05;
    }
    CHECK(feet.x > 75.0f);
}


namespace {
struct MeshMoveWorld final : app::MoveWorld {
    const game::CollisionMesh* mesh;
    const assets::ZoneRegions* regions;
    bool collision_loaded() const override { return true; }
    std::optional<std::pair<glm::vec3, glm::vec3>> world_box() const override { return mesh->bounds(); }
    Sweep sweep(const glm::vec3& from, const glm::vec3& to, float radius) const override {
        game::SphereSweep s = game::SphereSweep::make(from, to, radius);
        Sweep out;
        if (s.valid()) mesh->sweep_sphere(s);
        out.out = game::sweep_output(s);
        out.collided = s.collided;
        return out;
    }
    Sweep world_sweep(const glm::vec3& from, const glm::vec3& to, float radius) const override {
        game::SphereSweep s = game::SphereSweep::make(from, to, radius);
        s.floor_only = true;
        Sweep out;
        if (s.valid()) mesh->sweep_sphere(s);
        out.out = game::sweep_output(s);
        out.collided = s.collided;
        return out;
    }
    bool in_world(const glm::vec3& p) const override { return regions->in_world(p); }
    std::optional<float> floor_below(const glm::vec3& p, float step_up) const override { return mesh->floor_below(p, step_up); }
    float floor_sweep(const glm::vec3& from, const glm::vec3& to, float radius, std::uint32_t* = nullptr,
                      std::vector<FloorSwitchHit>* = nullptr) const override {
        game::SphereSweep s = game::SphereSweep::make(from, to, radius);
        s.floor_only = true;
        if (s.valid()) mesh->sweep_sphere(s);
        return game::sweep_output(s).position.z;
    }
    std::optional<float> ceiling_above(const glm::vec3& p) const override { return mesh->ceiling_above(p); }
};
} // namespace

namespace {
// A walk across a real zone by the movement rules themselves (app::PlayerMovement over the zone's collision): from
// `from` to `to` (server coordinates, /loc's x y z), once with frames of 8 ms (as the client runs) and a human's
// size, once with 16 ms and a bigger body. No monsters, no breath. Checks it arrives, unhurt, without a word.
void walk_real_zone(const GameDir& game, const char* name, glm::vec3 from, glm::vec3 to) {
    CAPTURE(name);
    assets::Zone zone = assets::load_zone(game, name);
    game::CollisionMesh collision = game::CollisionMesh::build(zone);
    std::vector<std::string> chat;
    app::Navigator::Env env;
    env.chat = [&](const std::string& t) { chat.push_back(t); };
    app::Navigator nav{env};
    nav.zone_loaded(collision, &zone.regions, {});
    nav.build_now();
    const glm::vec3 zone_in = game::server_to_world(from);
    auto start = std::chrono::steady_clock::now();
    while (!nav.status().mesh_complete && std::chrono::steady_clock::now() - start < std::chrono::seconds(120)) {
        app::Navigator::Frame f;
        f.feet = zone_in;
        nav.frame(f);
        std::this_thread::yield();
    }
    REQUIRE(nav.status().mesh_complete);
    test::ScratchDir scratch("navwalk");
    GameDir gd(scratch.path());
    net::GameConnection conn(gd, {});
    MeshMoveWorld world;
    world.mesh = &collision;
    world.regions = &zone.regions;
    const bool trace = hailstone::core::env("NAV_WALK_TRACE") != nullptr;
    struct Body {
        float frame_ms, radius, avatar;
    };
    for (Body body : {Body{8.0f, 1.78f, 5.0f}, Body{16.0f, 3.0f, 7.0f}}) {
        CAPTURE(body.frame_ms);
        CAPTURE(body.radius);
        chat.clear();
        glm::vec3 feet = zone_in;
        app::PlayerMovement m;
        const float avatar = body.avatar;
        const float dt = body.frame_ms;
        m.reset(feet, 0.0f);
        m.set_collision_radius(body.radius);
        REQUIRE(nav.go_to_point(game::server_to_world(to), 5.0f, "the goal", false).empty());
        std::uint32_t now = 10000;
        float yaw = 0.0f, pitch = 0.0f;
        app::PlayerMovement::Frame fr = m.update(app::MovementKeys{}, yaw, pitch, dt, now, conn, &zone.regions, avatar, &world);
        feet = fr.feet;
        double t = 0.0, traced = -10.0;
        int damage = 0;
        while (nav.status().active && t < 1800.0) {
            app::Navigator::Frame nf;
            nf.feet = feet;
            nf.now = t;
            nf.height = avatar;
            nf.swimming = fr.body_wet >= 5;
            nf.head_under = assets::is_water_type(fr.head_wet);
            nf.air = 1.0f;
            nav::SteerOutput st = nav.frame(nf);
            app::MovementKeys mk;
            if (st.active) {
                yaw = st.yaw;
                mk.forward = st.forward;
                mk.jump = st.jump;
                mk.strafe_left = st.strafe < 0;
                mk.strafe_right = st.strafe > 0;
                if (st.pitch) pitch = *st.pitch;
            }
            now += static_cast<std::uint32_t>(dt);
            t += dt / 1000.0;
            fr = m.update(mk, yaw, pitch, dt, now, conn, &zone.regions, avatar, &world);
            feet = fr.feet;
            if (fr.fall_damage) damage += *fr.fall_damage;
            if (trace && t - traced >= 1.0) {
                traced = t;
                glm::vec3 q = game::world_to_server(feet);
                std::printf("%s t %.0f at %.1f %.1f %.1f wet %d pitch %.2f\n", name, t, q.x, q.y, q.z, int(fr.body_wet), pitch);
            }
        }
        glm::vec3 end = game::world_to_server(feet);
        MESSAGE(name << ": " << t << " s of walking, ended at " << end.x << ", " << end.y << ", " << end.z);
        for (const std::string& line : chat) MESSAGE(line);
        CHECK_FALSE(nav.status().active);
        CHECK(glm::distance(glm::vec2(end), glm::vec2(to)) <= 6.0f);
        CHECK(std::abs(end.z - to.z) < 6.0f);
        CHECK(damage == 0);
        CHECK(chat.empty());
    }
}
} // namespace

// Old Sebilis from the zone-in to Trakanon's lair: down its ramps, off a ledge into the water, down to a floor under
// it, up again and through the lower halls, over the step into the lair. The parts of the mesh are joined only by
// the links (drops, swims) and a climb over 3.
TEST_CASE("nav: real zones: Old Sebilis, walked from the zone-in to Trakanon's lair") {
    if (const GameDir* game = test::game_dir()) walk_real_zone(*game, "sebilis", {0, 250, 36.04f}, {-705.5f, -2085.5f, -143.66f});
}

// Kedge Keep, all under water, from the arrival (floating over the entry room's floor) down its shaft of water some
// 270 deep and through the halls to Phinigel Autropos.
TEST_CASE("nav: real zones: Kedge Keep, walked from the arrival to Phinigel") {
    if (const GameDir* game = test::game_dir()) walk_real_zone(*game, "kedge", {30.0f, 100.0f, 326.0f}, {136.9f, -25.2f, -292.5f});
}

// Lower Guk from the undead side's entrance (from Upper Guk) to the ghoul lord: through the flooded passages.
TEST_CASE("nav: real zones: Lower Guk, walked from the undead side's entrance to the ghoul lord") {
    if (const GameDir* game = test::game_dir()) walk_real_zone(*game, "gukbottom", {-233.0f, 1157.0f, -80.0f}, {-749.0f, 1554.0f, -175.0f});
}



TEST_CASE("nav: a quiet path asked and not answered before a zone change does not take the next zone's /find reply") {
    test::Rig rig(test::MockConfig::standard(), test::quick_options());
    REQUIRE(rig.to_zone());
    const auto lease = rig.conn.request_quiet_path(101); REQUIRE(lease);
    // The zone change comes before the reply (the old zone's link dropped with it unanswered).
    rig.conn.receive_world_packet_for_test(net::encode(net::ZoneServerInfo{"127.0.0.1", 7001}));
    const auto finds = rig.conn.find_replies();
    std::vector<glm::vec3> path{{0, 0, 0}, {10, 5, 0}};
    rig.conn.receive_zone_packet_for_test(net::encode_find_person_reply(path));
    CHECK_FALSE(rig.conn.take_quiet_path(lease));
    CHECK(rig.conn.find_replies() == finds); // there is no current-zone request to own this orphan
    CHECK_FALSE(rig.conn.player_path().active());
}

TEST_CASE("nav: a shown find and a quiet path in flight together each get their own reply, in order") {
    // The server answers in the order asked: the find's reply first, then the navigation's. One flag for "the next
    // reply is quiet" gave the find's path to the navigation and drew the navigation's.
    test::Rig rig(test::MockConfig::standard(), test::quick_options());
    REQUIRE(rig.to_zone());
    rig.conn.find_map_location({30.0f, 40.0f, 0.0f}); // shown (the map's right click)
    auto lease = rig.conn.request_quiet_path(101);
    REQUIRE(lease);
    const auto finds = rig.conn.find_replies();
    std::vector<glm::vec3> shown{{0, 0, 0}, {30, 40, 0}};
    std::vector<glm::vec3> quiet{{0, 0, 0}, {10, 5, 0}, {20, 5, 1}};
    rig.conn.receive_zone_packet_for_test(net::encode_find_person_reply(shown));
    CHECK(rig.conn.find_replies() == finds + 1);
    CHECK_FALSE(rig.conn.take_quiet_path(lease));
    rig.conn.receive_zone_packet_for_test(net::encode_find_person_reply(quiet));
    auto got = rig.conn.take_quiet_path(lease);
    REQUIRE(got);
    CHECK(got->size() == 4);
    CHECK(rig.conn.find_replies() == finds + 1);
    // The other way round: the quiet one first.
    lease = rig.conn.request_quiet_path(101);
    REQUIRE(lease);
    rig.conn.find_map_location({30.0f, 40.0f, 0.0f});
    rig.conn.receive_zone_packet_for_test(net::encode_find_person_reply(quiet));
    CHECK(rig.conn.take_quiet_path(lease));
    rig.conn.receive_zone_packet_for_test(net::encode_find_person_reply(shown));
    CHECK(rig.conn.find_replies() == finds + 2);
    CHECK_FALSE(rig.conn.take_quiet_path(lease));
}
