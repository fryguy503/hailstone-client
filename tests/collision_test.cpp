#include <doctest/doctest.h>

#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <vector>

#include "assets/character.h"
#include "assets/zone.h"
#include "game/collision.h"
#include "game/world_state.h"
#include "game_files.h"

using hailstone::game::CollisionMesh;

namespace {
hailstone::assets::Zone flat_zone() {
    // Two triangles: floor at z=0 over 0..100, a ledge at z=10 over 40..60.
    hailstone::assets::Zone z;
    hailstone::assets::Mesh m;
    auto quad = [&](float x0, float x1, float h) {
        auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (auto [x, y] : {std::pair{x0, x0}, {x1, x0}, {x1, x1}, {x0, x1}}) {
            hailstone::assets::Vertex v;
            v.position = {x, y, h};
            m.vertices.push_back(v);
        }
        for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
    };
    quad(0, 100, 0);
    quad(40, 60, 10);
    m.materials.push_back({});
    m.materials[0].frames = {"x.bmp"};
    m.parts.push_back({0, static_cast<std::uint32_t>(m.indices.size()), 0});
    z.terrain.push_back(m);
    return z;
}

// The original ground query without its spatial index. Keeping the same float intersection arithmetic isolates
// candidate-selection errors, including boundaries and triangles shared by several cells.
std::optional<float> reference_ground(std::span<const hailstone::game::CollisionTriangle> triangles, const glm::vec3& p,
                                      bool effect = false) {
    using namespace hailstone::game;
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return std::nullopt;
    std::optional<float> best;
    for (const auto& tri : triangles) {
        if (tri.flags & (kTriangleTransparent | 0x10 | kTriangleLadderSlab | kTriangleInvisible |
                        (effect ? kTriangleNoVisualSurface : kTrianglePassable))) continue;
        if (!(tri.normal.z > 0.0f)) continue;
        const glm::vec2 a(tri.v0), b(tri.v1), c(tri.v2), q(p);
        const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::abs(d) < 1e-6f) continue;
        const float u = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / d;
        const float v = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / d;
        const float w = 1.0f - u - v;
        if (u < 0.0f || v < 0.0f || w < 0.0f) continue;
        const float z = u * tri.v0.z + v * tri.v1.z + w * tri.v2.z;
        if (z <= p.z && (!best || z > *best)) best = z;
    }
    return best;
}

void ground_quad(std::vector<hailstone::game::CollisionTriangle>& triangles, float x, float y, float size, float z,
                 float slope_x, float slope_y, std::uint8_t flags = 0, bool upward = true) {
    using hailstone::game::CollisionTriangle;
    const glm::vec3 a(x, y, z), b(x, y + size, z + slope_y * size);
    const glm::vec3 c(x + size, y + size, z + (slope_x + slope_y) * size), d(x + size, y, z + slope_x * size);
    triangles.push_back(upward ? CollisionTriangle::make(a, b, c, flags) : CollisionTriangle::make(a, c, b, flags));
    triangles.push_back(upward ? CollisionTriangle::make(a, c, d, flags) : CollisionTriangle::make(a, d, c, flags));
}
} // namespace

TEST_CASE("floor_below picks the highest floor within the step height") {
    auto mesh = CollisionMesh::build(flat_zone());
    CHECK(mesh.triangle_count() == 4);
    CHECK(mesh.floor_below({10, 10, 5}, 2).value() == doctest::Approx(0));
    CHECK(mesh.floor_below({50, 50, 20}, 2).value() == doctest::Approx(10));
    // Standing below the ledge: the ledge is too high to step onto.
    CHECK(mesh.floor_below({50, 50, 1}, 2).value() == doctest::Approx(0));
    CHECK_FALSE(mesh.floor_below({500, 500, 0}, 2).has_value());
}

TEST_CASE("display_name strips digits and underscores") {
    CHECK(hailstone::game::display_name("a_rat01") == "a rat");
    CHECK(hailstone::game::display_name("Guard_Ahrens000") == "Guard Ahrens");
    CHECK(hailstone::game::display_name("#Lord_Bob00") == "Lord Bob");
    CHECK(hailstone::game::display_name("#") == "");
}

TEST_CASE("qeynos2 has floors under its streets" * doctest::skip(!hailstone::test::game_dir())) {
    auto zone = hailstone::assets::load_classic_zone(*hailstone::test::game_dir(), "qeynos2");
    auto mesh = CollisionMesh::build(zone);
    CHECK(mesh.triangle_count() > 10000);
    CHECK(mesh.floor_below({400, -300, 100}, 4).has_value());
}

TEST_CASE("line_of_sight is blocked by solid geometry") {
    auto mesh = CollisionMesh::build(flat_zone());
    CHECK(mesh.line_of_sight({10, 10, 5}, {90, 90, 5}));    // above the floor, beside the ledge
    CHECK_FALSE(mesh.line_of_sight({50, 50, 20}, {50, 50, -5})); // through the ledge and the floor
}

TEST_CASE("line_of_sight and the probes: huge or non-finite coordinates return at once") {
    // One axis 2^31 cells long: the int32 span wrapped negative, passed the 256-cell guard and the loop walked
    // about 2^31 cells (and the float-to-int32 casts were undefined).
    auto mesh = CollisionMesh::build(flat_zone());
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(mesh.line_of_sight({-1e30f, 50, 5}, {100, 50, 5}));
    CHECK(mesh.line_of_sight({-1.5e10f, 0, 5}, {0, 0, 5}));
    CHECK(mesh.line_of_sight({0, -1.37e11f, 5}, {0, 1.37e11f, 5}));
    CHECK(mesh.line_of_sight({-inf, 50, 5}, {100, 50, 5}));
    CHECK(mesh.line_of_sight({nan, 50, 5}, {100, 50, 5}));
    CHECK_FALSE(mesh.floor_below({1e30f, -1e30f, 5}, 2).has_value());
    CHECK_FALSE(mesh.floor_below({nan, nan, 5}, 2).has_value());
    CHECK_FALSE(mesh.ceiling_above({-1e30f, 50, 5}).has_value());
    CHECK_FALSE(mesh.first_hit({-1e30f, 50, 5}, {1e30f, 50, 5}, true).has_value());
    // In range nothing changed.
    CHECK_FALSE(mesh.line_of_sight({50, 50, 20}, {50, 50, -5}));
    CHECK(mesh.first_hit({50, 50, 20}, {50, 50, -5}, true).has_value());
}

TEST_CASE("line_of_sight: only the cells under the sight line are searched, with the same answers") {
    // Many small walls scattered over 20 x 20 cells, and sight lines in every direction: the answer is the one from
    // testing every triangle (the same test, Moller-Trumbore strictly inside the segment), for each line short enough
    // to be checked at all (a box of 256 cells or fewer).
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> at(0.0f, 1280.0f), z(-20.0f, 40.0f), size(2.0f, 40.0f);
    std::vector<hailstone::game::CollisionTriangle> tris;
    for (int i = 0; i < 3000; ++i) {
        glm::vec3 a(at(rng), at(rng), z(rng));
        glm::vec3 b = a + glm::vec3(size(rng), size(rng) - 20.0f, size(rng) - 20.0f);
        glm::vec3 c = a + glm::vec3(size(rng) - 20.0f, size(rng), size(rng));
        tris.push_back(hailstone::game::CollisionTriangle::make(a, b, c, 0));
    }
    auto mesh = CollisionMesh::from_triangles(tris);
    auto blocked = [&](const glm::vec3& a, const glm::vec3& b) {
        glm::vec3 d = b - a;
        for (const auto& tri : tris) {
            glm::vec3 e1 = tri.v1 - tri.v0, e2 = tri.v2 - tri.v0;
            glm::vec3 pv = glm::cross(d, e2);
            float det = glm::dot(e1, pv);
            if (std::abs(det) < 1e-8f) continue;
            float inv = 1.0f / det;
            glm::vec3 tv = a - tri.v0;
            float u = glm::dot(tv, pv) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            glm::vec3 qv = glm::cross(tv, e1);
            float v = glm::dot(d, qv) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            float hit = glm::dot(e2, qv) * inv;
            if (hit > 0.001f && hit < 0.999f) return true;
        }
        return false;
    };
    int checked = 0, seen = 0;
    std::uniform_real_distribution<float> reach(-600.0f, 600.0f);
    for (int i = 0; i < 3000; ++i) {
        glm::vec3 a(at(rng), at(rng), z(rng));
        glm::vec3 b = a + glm::vec3(reach(rng), reach(rng), reach(rng) * 0.1f);
        if (i % 7 == 0) b.y = a.y; // along the grid lines' directions too
        if (i % 11 == 0) b.x = a.x;
        auto cells = [](float p, float q) { return std::floor(std::max(p, q) / 64.0f) - std::floor(std::min(p, q) / 64.0f) + 1.0f; };
        if (cells(a.x, b.x) * cells(a.y, b.y) > 256.0f) continue;
        ++checked;
        const bool clear = mesh.line_of_sight(a, b);
        seen += clear;
        CHECK(clear == !blocked(a, b));
    }
    CHECK(checked > 1000);
    CHECK(seen > 100);           // some lines clear,
    CHECK(checked - seen > 100); // some blocked
}

TEST_CASE("qeynos: the Steel Warriors hall wall blocks line of sight" * doctest::skip(!hailstone::test::game_dir())) {
    auto zone = hailstone::assets::load_classic_zone(*hailstone::test::game_dir(), "qeynos");
    auto mesh = CollisionMesh::build(zone);
    // Server (-544, 58, 32.5) -> world (58, -544); the screenshot showed a wall ahead along +X.
    glm::vec3 eye(58.0f, -544.0f, 33.0f);
    auto floor = mesh.floor_below({58.0f, -544.0f, 32.5f}, 0.5f);
    REQUIRE(floor.has_value());
    MESSAGE("floor " << *floor);
    bool clear = true;
    float hit_at = 0;
    for (float d = 5; d <= 120; d += 5) {
        if (!mesh.line_of_sight(eye, eye + glm::vec3(d, 0, 0))) { clear = false; hit_at = d; break; }
    }
    MESSAGE("blocked within " << hit_at);
    CHECK_FALSE(clear);
}

namespace {
// Floors (near-horizontal solid terrain triangles) whose record normal
// normalize((v0 - v1) x (v2 - v1)) points up, and those pointing down: the
// back-face rule (skip when n . delta > 0) needs floors facing up.
std::pair<int, int> floor_facing(const hailstone::assets::Zone& zone) {
    int up = 0, down = 0;
    for (const auto& mesh : zone.terrain) {
        for (const auto& part : mesh.parts) {
            if (!part.solid) continue;
            for (std::uint32_t i = 0; i + 2 < part.index_count; i += 3) {
                const auto& v = mesh.vertices;
                std::uint32_t b = part.first_index + i + (mesh.front_ccw ? 2 : 1), c = part.first_index + i + (mesh.front_ccw ? 1 : 2);
                auto t = hailstone::game::CollisionTriangle::make(v[mesh.indices[part.first_index + i]].position,
                                                             v[mesh.indices[b]].position, v[mesh.indices[c]].position, 0);
                if (t.normal.z > 0.9f) ++up;
                if (t.normal.z < -0.9f) ++down;
            }
        }
    }
    return {up, down};
}
} // namespace

TEST_CASE("zone floors face up by the record normal (WLD and EQG)" * doctest::skip(!hailstone::test::game_dir())) {
    // Outdoor: few ceilings.
    auto [up, down] = floor_facing(hailstone::assets::load_classic_zone(*hailstone::test::game_dir(), "qeytoqrg"));
    MESSAGE("qeytoqrg floors: " << up << " up, " << down << " down");
    CHECK(up > 4 * down);
    for (const char* name : {"arena2", "barren"}) {
        auto [eup, edown] = floor_facing(hailstone::assets::load_eqg_zone(*hailstone::test::game_dir(), name));
        MESSAGE(std::string(name) << " floors: " << eup << " up, " << edown << " down");
        CHECK(eup > 4 * edown);
    }
}

TEST_CASE("player models carry their skeleton's bounding radius" * doctest::skip(!hailstone::test::game_dir())) {
    auto lib = hailstone::assets::CharacterLibrary::load(*hailstone::test::game_dir(), "qeynos2");
    for (const char* code : {"HUM", "HUF", "BAM", "DWM", "HOM", "OGM", "TRM", "ELM", "GNM", "IKM", "KEM", "ERM", "HIM", "HAM", "DAM", "FRM"}) {
        auto m = lib.model(code);
        if (!m) continue;
        MESSAGE(code << " radius " << m->bounding_radius << " bounds z " << m->bounds_min.z << ".." << m->bounds_max.z);
    }
}

TEST_CASE("ground_below and roof_above see what is drawn: a tunnel roof is no ground, an invisible lid no roof") {
    using hailstone::game::CollisionTriangle;
    // Up-facing by the record normal ((v0 - v1) x (v2 - v1)): v1 the corner, v0 along +x, v2 along +y.
    auto up = [](float z, std::uint8_t flags) {
        return CollisionTriangle::make({200, -100, z}, {-100, -100, z}, {-100, 200, z}, flags);
    };
    auto down = [](float z, std::uint8_t flags) {
        return CollisionTriangle::make({-100, 200, z}, {-100, -100, z}, {200, -100, z}, flags);
    };
    // North Ro's tunnel to East Commonlands: the floor at 0, the roof's underside at 30 (no top face over it).
    auto tunnel = CollisionMesh::from_triangles({up(0, 0), down(30, 0)});
    CHECK(tunnel.floor_below({0, 0, 70}, 0).value() == doctest::Approx(30)); // the roof, as a floor
    CHECK(tunnel.ground_below({0, 0, 70}).value() == doctest::Approx(0));
    CHECK(tunnel.roof_above({0, 0, 5}).value() == doctest::Approx(30));
    // Qeynos' line to the Hills: an invisible lid at 80 over the open ground.
    auto lid = CollisionMesh::from_triangles({up(0, 0), down(80, hailstone::game::kTriangleInvisible)});
    CHECK(lid.floor_below({0, 0, 100}, 0).value() == doctest::Approx(80));
    CHECK(lid.ground_below({0, 0, 100}).value() == doctest::Approx(0));
    CHECK(lid.ceiling_above({0, 0, 5}).value() == doctest::Approx(80));
    CHECK(!lid.roof_above({0, 0, 5}).has_value());
    // An invisible floor (North Ro's plane under its shore) is no ground either.
    auto plane = CollisionMesh::from_triangles({up(-111, hailstone::game::kTriangleInvisible)});
    CHECK(!plane.ground_below({0, 0, 0}).has_value());

    // From a zone: an Invisible material's triangles carry the flag.
    auto zone = [](hailstone::assets::BlendMode blend) {
        hailstone::assets::Zone z;
        hailstone::assets::Mesh m;
        for (glm::vec3 p : {glm::vec3(0, 0, 0), glm::vec3(0, 100, 0), glm::vec3(100, 0, 0)}) {
            hailstone::assets::Vertex v;
            v.position = p;
            m.vertices.push_back(v);
        }
        m.indices = {0, 1, 2}; // clockwise from above: a WLD floor
        m.materials.push_back({});
        m.materials[0].frames = {"x.bmp"};
        m.materials[0].blend = blend;
        m.parts.push_back({0, 3, 0});
        z.terrain.push_back(m);
        return z;
    };
    auto seen = CollisionMesh::build(zone(hailstone::assets::BlendMode::Opaque));
    CHECK(seen.ground_below({10, 10, 5}).value() == doctest::Approx(0));
    auto hidden = CollisionMesh::build(zone(hailstone::assets::BlendMode::Invisible));
    CHECK(hidden.floor_below({10, 10, 5}, 0).value() == doctest::Approx(0));
    CHECK(!hidden.ground_below({10, 10, 5}).has_value());
}

TEST_CASE("ground_below: dense subdivisions match a full triangle scan on slopes stacked floors and boundaries") {
    using namespace hailstone::game;
    std::vector<CollisionTriangle> triangles;
    const std::uint8_t flags[] = {0, kTrianglePassable, kTriangleTransparent, kTriangleInvisible, 0x10,
                                  kTriangleLadderSlab, kTriangleLadder, kTriangleObject};
    for (int x = -16; x < 16; ++x)
        for (int y = -16; y < 16; ++y) {
            const float px = static_cast<float>(x) * 4.0f, py = static_cast<float>(y) * 4.0f;
            const float z = px * 0.125f - py * 0.0625f;
            ground_quad(triangles, px, py, 4, z, 0.125f, -0.0625f);
            const auto f = flags[static_cast<std::size_t>((x + 16) * 32 + y + 16) % std::size(flags)];
            ground_quad(triangles, px, py, 4, z + 12, -0.0625f, 0.125f, f);
            if ((x + y) % 3 == 0) ground_quad(triangles, px, py, 4, z + 30, 0, 0, 0, false);
        }
    // Sparse cells use the coarse index, while these enormous triangles stay outside either grid.
    ground_quad(triangles, 300, -300, 12, 5, 0.125f, 0.25f);
    triangles.push_back(CollisionTriangle::make({10000, -10000, -80}, {-10000, -10000, -80}, {-10000, 10000, -80}, 0));
    triangles.push_back(CollisionTriangle::make({10000, -10000, -90}, {-10000, 10000, -90}, {10000, 10000, -90}, 0));
    triangles.push_back(CollisionTriangle::make({10000, -10000, 80}, {-10000, -10000, 80}, {-10000, 10000, 80}, kTriangleInvisible));
    const auto mesh = CollisionMesh::from_triangles(triangles);
    std::size_t found = 0, missing = 0;
    auto check = [&](const glm::vec3& p) {
        CAPTURE(p.x);
        CAPTURE(p.y);
        CAPTURE(p.z);
        const auto expected = reference_ground(triangles, p);
        const auto actual = mesh.ground_below(p);
        CHECK(actual.has_value() == expected.has_value());
        if (actual && expected) {
            CHECK(*actual == *expected);
            ++found;
        } else ++missing;
    };
    std::mt19937 rng(1783);
    std::uniform_real_distribution<float> xy(-80.0f, 80.0f), z(-100.0f, 60.0f);
    for (int i = 0; i < 5000; ++i) {
        glm::vec3 p;
        p.x = xy(rng);
        p.y = xy(rng);
        p.z = z(rng);
        check(p);
    }
    const float inf = std::numeric_limits<float>::infinity();
    for (int x = -8; x <= 8; ++x)
        for (int y = -8; y <= 8; ++y) {
            const float px = static_cast<float>(x) * 8.0f, py = static_cast<float>(y) * 8.0f;
            for (const float qx : {std::nextafter(px, -inf), px, std::nextafter(px, inf)})
                for (const float qy : {std::nextafter(py, -inf), py, std::nextafter(py, inf)}) {
                    check({qx, qy, 5});
                    check({qx, qy, 40});
                }
        }
    for (const auto& p : {glm::vec3(305, -295, 50), glm::vec3(-3000, -3000, 0), glm::vec3(3000, 3000, 0),
                          glm::vec3(1e30f, -1e30f, 50)}) check(p);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const float height : {inf, -inf, nan}) {
        check({4, -4, height});
        check({305, -295, height});
        check({-3000, -3000, height});
    }
    for (const auto& p : {glm::vec3(nan, 0, 5), glm::vec3(0, nan, 5), glm::vec3(inf, 0, 5), glm::vec3(0, -inf, 5)}) check(p);
    CHECK(found > 5000);
    CHECK(missing > 100);
}

TEST_CASE("ground_below: refined candidates leave movement and roof probes unchanged and reject malformed vertices") {
    using namespace hailstone::game;
    std::vector<CollisionTriangle> triangles;
    for (int x = 0; x < 16; ++x)
        for (int y = 0; y < 16; ++y)
            ground_quad(triangles, static_cast<float>(x) * 4, static_cast<float>(y) * 4, 4, 0, 0, 0);
    ground_quad(triangles, 0, 0, 64, 10, 0, 0, kTriangleInvisible);
    ground_quad(triangles, 0, 0, 64, 20, 0, 0, 0, false);
    ground_quad(triangles, 0, 0, 64, 30, 0, 0, kTrianglePassable);
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    for (const auto& v : {glm::vec3(nan, 0, 40), glm::vec3(0, inf, 40), glm::vec3(0, 0, nan)}) {
        auto malformed = CollisionTriangle::make({0, 0, 40}, {0, 64, 40}, {64, 64, 40}, 0);
        malformed.v0 = v;
        malformed.normal = {0, 0, 1}; // public synthetic triangle data can be inconsistent
        triangles.push_back(malformed);
    }
    const auto mesh = CollisionMesh::from_triangles(std::move(triangles));
    CHECK(mesh.ground_below({8, 8, 50}).value() == doctest::Approx(0));
    CHECK(mesh.ground_below({8, 8, inf}).value() == doctest::Approx(0));
    CHECK_FALSE(mesh.ground_below({8, 8, -inf}).has_value());
    CHECK_FALSE(mesh.ground_below({8, 8, nan}).has_value());
    CHECK(mesh.floor_below({8, 8, 50}, 0).value() == doctest::Approx(20));
    CHECK(mesh.ceiling_above({8, 8, 5}).value() == doctest::Approx(10));
    CHECK(mesh.roof_above({8, 8, 5}).value() == doctest::Approx(20));
}

TEST_CASE("visual_surface_below: dense spatial candidates preserve exact layered results including passable props") {
    using namespace hailstone::game;
    std::vector<CollisionTriangle> triangles;
    for(int x=-16;x<32;++x)
        for(int y=-16;y<32;++y) {
            ground_quad(triangles,static_cast<float>(x)*4,static_cast<float>(y)*4,4,
                static_cast<float>((x+y)%3),.08f,.025f);
            const auto flags=static_cast<std::uint8_t>((x-y)%4==0?kTriangleInvisible:
                (x-y)%4==1?kTriangleNoVisualSurface:kTrianglePassable);
            ground_quad(triangles,static_cast<float>(x)*4,static_cast<float>(y)*4,4,
                6+static_cast<float>((x+y)%3),.08f,.025f,flags);
        }
    ground_quad(triangles,-10000,-10000,20000,-40,0,0); // huge-triangle fallback is independent of fine cells
    const auto mesh=CollisionMesh::from_triangles(triangles);
    std::mt19937 random(29);
    std::uniform_real_distribution<float> xy(-70,135),z(-45,15);
    for(int sample=0;sample<2000;++sample) {
        const glm::vec3 point{sample%5?xy(random):static_cast<float>(sample%32)*4,
                              sample%7?xy(random):static_cast<float>(sample%32)*4,z(random)};
        const auto expected=reference_ground(triangles,point,true);
        const auto actual=mesh.visual_surface_below(point);
        REQUIRE(actual.has_value()==expected.has_value());
        if(actual)CHECK(*actual==doctest::Approx(*expected));
    }
}

TEST_CASE("a triangle too large for the grid is met by every query") {
    using hailstone::game::CollisionTriangle;
    // About 300 x 300 cells of 64: listed in no cell, only among the huge ones.
    auto up = [](float z) { return CollisionTriangle::make({10000, -10000, z}, {-10000, -10000, z}, {-10000, 10000, z}, 0); };
    auto down = [](float z) { return CollisionTriangle::make({-10000, 10000, z}, {-10000, -10000, z}, {10000, -10000, z}, 0); };
    auto mesh = CollisionMesh::from_triangles({up(0), down(40)});
    CHECK(mesh.floor_below({0, 0, 10}, 0).value() == doctest::Approx(0));
    CHECK(mesh.ceiling_above({0, 0, 10}).value() == doctest::Approx(40));
    auto above = mesh.surfaces_above({0, 0, 10});
    REQUIRE(above.size() == 1);
    CHECK(above[0].z == doctest::Approx(40));
    CHECK(mesh.ground_below({0, 0, 10}).value() == doctest::Approx(0));
    CHECK(mesh.roof_above({0, 0, 10}).value() == doctest::Approx(40));
    CHECK_FALSE(mesh.line_of_sight({0, 0, 10}, {0, 0, -10}));
    CHECK(mesh.line_of_sight({0, 0, 10}, {50, 50, 20}));
    auto hit = mesh.first_hit({0, 0, 10}, {0, 0, -10});
    REQUIRE(hit);
    CHECK(hit->t == doctest::Approx(0.5f));
}

TEST_CASE("for_each_on_segment and the sweeps meet each triangle once, also from inside another query") {
    using hailstone::game::CollisionTriangle;
    // A wall spanning several cells, crossed along its length.
    auto mesh = CollisionMesh::from_triangles({CollisionTriangle::make({0, 0, 0}, {300, 0, 0}, {300, 0, 50}, 0),
                                               CollisionTriangle::make({0, 0, 0}, {300, 0, 50}, {0, 0, 50}, 0)});
    int outer = 0, inner = 0;
    mesh.for_each_on_segment({-10, 0, 25}, {310, 0, 25}, [&](const CollisionTriangle&) {
        ++outer;
        mesh.for_each_on_segment({-10, 0, 25}, {310, 0, 25}, [&](const CollisionTriangle&) { ++inner; });
    });
    CHECK(outer == 2);
    CHECK(inner == 4);
    // Asked again, the marks of the last query do not hide anything.
    int again = 0;
    mesh.for_each_on_segment({-10, 0, 25}, {310, 0, 25}, [&](const CollisionTriangle&) { ++again; });
    CHECK(again == 2);
    auto s = hailstone::game::SphereSweep::make({150, 20, 25}, {150, -20, 25}, 2.0f); // into the walls' +y faces
    CHECK(mesh.sweep_sphere_hit(s) >= 0);
}
