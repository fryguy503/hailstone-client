// render/horizon.h: what a zone's terrain says about the land round it, and that land's mesh.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "render/horizon.h"

using namespace hailstone;
using render::HorizonTriangle;

namespace {

// A level square of ground (x0..x1, y0..y1) at height z, as two triangles; `flip` winds them the other way (the EQG
// terrain's); `liquid` a water surface.
void add_square(std::vector<HorizonTriangle>& out, float x0, float y0, float x1, float y1, float z, bool flip = false,
                bool liquid = false) {
    glm::vec3 a(x0, y0, z), b(x1, y0, z), c(x1, y1, z), d(x0, y1, z);
    if (flip) {
        out.push_back({a, c, b, liquid});
        out.push_back({a, d, c, liquid});
    } else {
        out.push_back({a, b, c, liquid});
        out.push_back({a, c, d, liquid});
    }
}

// A grid of level squares, each `cell` across, over x0..x1, y0..y1, at the heights height(x, y).
template <typename Height>
void add_grid(std::vector<HorizonTriangle>& out, float x0, float y0, float x1, float y1, float cell, Height height,
              bool liquid = false) {
    for (float x = x0; x < x1; x += cell)
        for (float y = y0; y < y1; y += cell) add_square(out, x, y, x + cell, y + cell, height(x, y), false, liquid);
}

bool near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

// The mesh's vertices used by any triangle.
std::vector<glm::vec3> used(const render::HorizonMesh& m, const std::vector<std::uint32_t>& tris) {
    std::vector<glm::vec3> out;
    for (std::uint32_t i : tris) out.push_back(m.positions[i]);
    return out;
}

bool inside(const glm::vec3& p, float x0, float y0, float x1, float y1) {
    return p.x > x0 && p.x < x1 && p.y > y0 && p.y < y1;
}

} // namespace

TEST_CASE("horizon: the land is measured from the level ground") {
    std::vector<HorizonTriangle> tris;
    // 2000 x 1000 units of flat ground at z 10, off the origin.
    add_grid(tris, 1000.0f, -500.0f, 3000.0f, 500.0f, 100.0f, [](float, float) { return 10.0f; });
    const auto land = render::horizon_land(tris, 5.0f);
    REQUIRE(land.valid);
    CHECK(near(land.lo.x, 1000.0f, 100.0f));
    CHECK(near(land.hi.x, 3000.0f, 100.0f));
    CHECK(near(land.lo.y, -500.0f, 100.0f));
    CHECK(near(land.hi.y, 500.0f, 100.0f));
    CHECK(land.ground == doctest::Approx(10.0f));
    CHECK(land.relief == doctest::Approx(render::kHorizonMinRelief)); // flat: the least relief
    CHECK(land.seed == 5.0f);
}

TEST_CASE("horizon: a far-off scrap of ground does not move the extent") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, -1000.0f, -1000.0f, 1000.0f, 1000.0f, 100.0f, [](float, float) { return 0.0f; });
    const auto plain = render::horizon_land(tris, 0.0f);
    // A dock 20000 units out, a tenth of a percent of the area.
    add_square(tris, 20000.0f, 0.0f, 20060.0f, 60.0f, 0.0f);
    const auto with_dock = render::horizon_land(tris, 0.0f);
    REQUIRE(with_dock.valid);
    CHECK(near(with_dock.hi.x, plain.hi.x, 50.0f));
}

TEST_CASE("horizon: the relief is the level ground's spread in height, either way up; walls and water do not count") {
    std::vector<HorizonTriangle> tris;
    // Terraces: a quarter each at 0, 200, 400 and 600, wound either way.
    for (int k = 0; k < 4; ++k)
        for (float x = 0.0f; x < 1000.0f; x += 100.0f)
            for (float y = static_cast<float>(k) * 500.0f; y < static_cast<float>(k + 1) * 500.0f; y += 100.0f)
                add_square(tris, x, y, x + 100.0f, y + 100.0f, 200.0f * static_cast<float>(k), k % 2 == 1);
    // A sheer wall far off and a sea: neither moves the extent or the height.
    for (int i = 0; i < 200; ++i) {
        const float x = 50000.0f + static_cast<float>(i);
        tris.push_back({{x, 0, 0}, {x, 100, 0}, {x, 100, 500}, false});
    }
    add_grid(tris, -3000.0f, 0.0f, -1000.0f, 2000.0f, 200.0f, [](float, float) { return -100.0f; }, true);
    const auto land = render::horizon_land(tris, 0.0f);
    REQUIRE(land.valid);
    CHECK(land.relief == doctest::Approx(600.0f));
    CHECK((land.ground == doctest::Approx(200.0f) || land.ground == doctest::Approx(400.0f)));
    CHECK(land.lo.x > -50.0f);
    CHECK(land.hi.x < 1050.0f);
}

TEST_CASE("horizon: too little level ground gives no land") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 300.0f, 300.0f, 100.0f, [](float, float) { return 0.0f; }); // a room
    CHECK_FALSE(render::horizon_land(tris, 0.0f).valid);
    CHECK_FALSE(render::horizon_land({}, 0.0f).valid);
    // Degenerate and non-finite triangles are skipped.
    tris.push_back({{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, false});
    tris.push_back({{NAN, 0, 0}, {1, 0, 0}, {0, 1, 0}, false});
    CHECK_FALSE(render::horizon_land(tris, 0.0f).valid);
    const auto land = render::horizon_land(tris, 0.0f);
    CHECK(render::horizon_mesh(tris, land).empty());
}

TEST_CASE("horizon: each zone's seed is its own and the same every time") {
    CHECK(render::horizon_seed("qeytoqrg") == render::horizon_seed("qeytoqrg"));
    CHECK(render::horizon_seed("qeytoqrg") == render::horizon_seed("QeyToQrg"));
    CHECK(render::horizon_seed("qeytoqrg") != render::horizon_seed("ecommons"));
    for (const char* z : {"", "a", "qeytoqrg", "everfrost", "northro"}) {
        CHECK(render::horizon_seed(z) >= 0.0f);
        CHECK(render::horizon_seed(z) < 1001.0f);
    }
}

TEST_CASE("horizon: the land lies round the zone, under it where it reaches in, and out to the view's reach") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 2000.0f, 1500.0f, 100.0f, [](float, float) { return 50.0f; });
    const auto land = render::horizon_land(tris, 3.0f);
    REQUIRE(land.valid);
    const auto mesh = render::horizon_mesh(tris, land);
    REQUIRE_FALSE(mesh.ground.empty());
    CHECK(mesh.water.empty());
    CHECK(mesh.positions.size() == mesh.normals.size());
    CHECK(mesh.positions.size() == mesh.shade.size());
    std::vector<std::uint32_t> all = mesh.ground;
    REQUIRE(all.size() % 3 == 0);
    float reach = 0.0f;
    for (const glm::vec3& p : used(mesh, all)) {
        // Over the zone's ground, under it.
        if (inside(p, 0.0f, 0.0f, 2000.0f, 1500.0f)) CHECK(p.z < 50.0f);
        reach = std::max(reach, std::max(p.x - 2000.0f, -p.x));
        CHECK(std::isfinite(p.z));
    }
    CHECK(reach >= render::kHorizonReach);
    // Counter-clockwise seen from above, and the normals up.
    for (std::size_t i = 0; i < all.size(); i += 3) {
        const glm::vec3 a = mesh.positions[all[i]], b = mesh.positions[all[i + 1]], c = mesh.positions[all[i + 2]];
        CHECK(glm::cross(b - a, c - a).z > 0.0f);
    }
    for (const glm::vec3& n : mesh.normals) CHECK(n.z > 0.0f);
    for (float s : mesh.shade) {
        CHECK(s > 0.4f);
        CHECK(s <= 1.0f);
    }
    // Near the zone the land is as high as its edge; far out, hills and mountains stand over the ground.
    float near_top = -1e9f, far_top = -1e9f;
    for (const glm::vec3& p : used(mesh, all)) {
        const float out = std::max({-p.x, p.x - 2000.0f, -p.y, p.y - 1500.0f});
        if (out > 150.0f && out < 300.0f) near_top = std::max(near_top, p.z);
        if (out > 6000.0f) far_top = std::max(far_top, p.z);
    }
    CHECK(near(near_top, 50.0f, 120.0f));
    CHECK(far_top > 50.0f + 500.0f);
}

TEST_CASE("horizon: past a wall the land starts at its top") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 2000.0f, 2000.0f, 100.0f, [](float, float) { return 0.0f; });
    // A wall 400 high along the east edge, x 2000.
    for (float y = 0.0f; y < 2000.0f; y += 100.0f) {
        tris.push_back({{2000, y, 0}, {2000, y + 100, 0}, {2000, y + 100, 400}, false});
        tris.push_back({{2000, y, 0}, {2000, y + 100, 400}, {2000, y, 400}, false});
    }
    const auto mesh = render::horizon_mesh(tris, render::horizon_land(tris, 0.0f));
    std::vector<std::uint32_t> all = mesh.ground;
    float east = 0.0f, west = 0.0f;
    int ne = 0, nw = 0;
    for (const glm::vec3& p : used(mesh, all)) {
        if (p.y < 500.0f || p.y > 1500.0f) continue;
        if (p.x > 2250.0f && p.x < 2450.0f) {
            east += p.z;
            ++ne;
        }
        if (p.x < -250.0f && p.x > -450.0f) {
            west += p.z;
            ++nw;
        }
    }
    REQUIRE(ne > 0);
    REQUIRE(nw > 0);
    CHECK(east / static_cast<float>(ne) > 250.0f); // the wall's top, less a little
    CHECK(east / static_cast<float>(ne) < 400.0f);
    CHECK(west / static_cast<float>(nw) < 100.0f); // the open side: the ground's
}

TEST_CASE("horizon: where the zone's edge is water, the sea, its floor under the surface") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 2000.0f, 2000.0f, 100.0f, [](float, float) { return 20.0f; });
    // A sea along the west, its surface at 0.
    add_grid(tris, -1000.0f, 0.0f, 0.0f, 2000.0f, 100.0f, [](float, float) { return 0.0f; }, true);
    const auto mesh = render::horizon_mesh(tris, render::horizon_land(tris, 0.0f));
    REQUIRE_FALSE(mesh.water.empty());
    CHECK(mesh.water_level == doctest::Approx(0.0f));
    for (std::uint32_t i : mesh.water) {
        CHECK(mesh.positions[i].z == doctest::Approx(0.0f));
        // Off the west shore: beside the zone not past its edge, and past its corners only as far as a bay of the
        // coast wanders.
        const glm::vec3 p = mesh.positions[i];
        if (p.y > 0.0f && p.y < 2000.0f) CHECK(p.x < 100.0f);
        CHECK(p.x < 700.0f);
    }
    // Off the shore the floor lies under the surface.
    int floor = 0;
    for (const glm::vec3& p : used(mesh, mesh.ground))
        if (p.x < -2000.0f && p.x > -2500.0f && p.y > 500.0f && p.y < 1500.0f) {
            CHECK(p.z < -100.0f);
            ++floor;
        }
    CHECK(floor > 0);
}

TEST_CASE("horizon: past the zone's own shore the sea goes on to the land's reach, with no coast rising from it") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 2000.0f, 2000.0f, 100.0f, [](float, float) { return 20.0f; });
    add_grid(tris, -1000.0f, 0.0f, 0.0f, 2000.0f, 100.0f, [](float, float) { return 0.0f; }, true);
    const auto land = render::horizon_land(tris, 0.0f);
    const auto field = render::horizon_field(tris, land);
    for (float x : {-3000.0f, -6000.0f, -9000.0f})
        CHECK(near(render::horizon_height(field, {x, 1000.0f}), -render::kHorizonSeaDepth, 1.0f));
    const auto mesh = render::horizon_mesh(field);
    bool far_sea = false;
    for (std::uint32_t i : mesh.water) far_sea |= mesh.positions[i].x < -8000.0f;
    CHECK(far_sea);
}

TEST_CASE("horizon: a river's mouth at the zone's own edge is no shore") {
    std::vector<HorizonTriangle> tris;
    // A river 100 wide across the zone, out of its west edge; its bed under the surface.
    auto river = [](float y) { return y >= 900.0f && y < 1000.0f; };
    add_grid(tris, 0.0f, 0.0f, 2000.0f, 2000.0f, 100.0f, [&](float, float y) { return river(y) ? 0.0f : 20.0f; });
    for (float x = 0.0f; x < 2000.0f; x += 100.0f) add_square(tris, x, 900.0f, x + 100.0f, 1000.0f, 10.0f, false, true);
    const auto land = render::horizon_land(tris, 0.0f);
    const auto field = render::horizon_field(tris, land);
    CHECK(render::horizon_height(field, {-3000.0f, 950.0f}) > 0.0f);
    const auto mesh = render::horizon_mesh(field);
    for (std::uint32_t i : mesh.water) CHECK(mesh.positions[i].x > -1000.0f);
}

TEST_CASE("horizon: the land's sea covers a square the zone's water only touches along its side") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 2000.0f, 2048.0f, 64.0f, [](float, float) { return 20.0f; });
    // The sea's north edge lies on a square's side (y 2048): the square past it is marked, but holds none of it.
    add_grid(tris, -1024.0f, 0.0f, 0.0f, 2048.0f, 64.0f, [](float, float) { return 0.0f; }, true);
    const auto field = render::horizon_field(tris, render::horizon_land(tris, 0.0f));
    const auto mesh = render::horizon_mesh(field);
    bool sliver = false, overlap = false;
    for (std::size_t i = 0; i + 2 < mesh.water.size(); i += 3) {
        const glm::vec3 c =
            (mesh.positions[mesh.water[i]] + mesh.positions[mesh.water[i + 1]] + mesh.positions[mesh.water[i + 2]]) / 3.0f;
        sliver |= c.x > -900.0f && c.x < -100.0f && c.y > 2048.0f && c.y < 2112.0f;
        overlap |= c.x > -900.0f && c.x < -100.0f && c.y > 100.0f && c.y < 1900.0f; // the zone's own water
    }
    CHECK(sliver);
    CHECK_FALSE(overlap);
}

TEST_CASE("horizon: a pocket the zone closes round is filled at its rim's height") {
    std::vector<HorizonTriangle> tris;
    // A ring of ground at z 100 round an empty 1000-unit square.
    add_grid(tris, 0.0f, 0.0f, 3000.0f, 3000.0f, 100.0f, [](float, float) { return 100.0f; });
    std::erase_if(tris, [](const HorizonTriangle& t) {
        const glm::vec3 c = (t.a + t.b + t.c) / 3.0f;
        return c.x > 1000.0f && c.x < 2000.0f && c.y > 1000.0f && c.y < 2000.0f;
    });
    const auto mesh = render::horizon_mesh(tris, render::horizon_land(tris, 0.0f));
    int in_pocket = 0;
    for (const glm::vec3& p : used(mesh, mesh.ground))
        if (inside(p, 1300.0f, 1300.0f, 1700.0f, 1700.0f)) {
            CHECK(p.z < 100.0f); // a little under the rim
            CHECK(p.z > 50.0f);
            ++in_pocket;
        }
    CHECK(in_pocket > 0);
}

TEST_CASE("horizon: the same zone gives the same land; another seed other hills") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 1500.0f, 1500.0f, 100.0f, [](float, float) { return 0.0f; });
    auto land = render::horizon_land(tris, 1.0f);
    const auto a = render::horizon_mesh(tris, land), b = render::horizon_mesh(tris, land);
    REQUIRE(a.positions.size() == b.positions.size());
    CHECK(a.positions == b.positions);
    CHECK(a.ground == b.ground);
    land.seed = 500.0f;
    const auto c = render::horizon_mesh(tris, land);
    REQUIRE(c.positions.size() == a.positions.size());
    CHECK(c.positions != a.positions);
}

TEST_CASE("horizon: the land under the sea is darker with the depth") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 2000.0f, 2000.0f, 100.0f, [](float, float) { return 20.0f; });
    add_grid(tris, -1000.0f, 0.0f, 0.0f, 2000.0f, 100.0f, [](float, float) { return 0.0f; }, true);
    const auto field = render::horizon_field(tris, render::horizon_land(tris, 0.0f));
    const auto mesh = render::horizon_mesh(field);
    float deep = 1.0f;
    for (std::size_t v = 0; v < mesh.positions.size() && v < mesh.shade.size(); ++v) {
        const glm::vec3& p = mesh.positions[v];
        if (p.x < -5000.0f && p.x > -9000.0f && std::abs(p.y - 1000.0f) < 300.0f && p.z < -200.0f)
            deep = std::min(deep, mesh.shade[v]);
    }
    CHECK(deep <= render::kHorizonSeaFloorShade + 0.01f);
}

TEST_CASE("horizon: the land's surface is the mesh's, between its vertices too") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 2000.0f, 1500.0f, 100.0f, [](float, float) { return 50.0f; });
    const auto land = render::horizon_land(tris, 3.0f);
    const auto field = render::horizon_field(tris, land);
    const auto mesh = render::horizon_mesh(field);
    const std::size_t cols = field.xs.size();
    int checked = 0;
    for (std::size_t r = 10; r + 10 < field.ys.size(); r += 7)
        for (std::size_t c = 10; c + 10 < cols; c += 7) {
            const glm::vec3 v = mesh.positions[r * cols + c];
            const auto z = render::horizon_surface(field, mesh, glm::vec2(v));
            REQUIRE(z);
            CHECK(*z == doctest::Approx(v.z).epsilon(1e-4));
            // Half way to the next vertex: between the four corners' heights.
            const glm::vec3 n = mesh.positions[(r + 1) * cols + c + 1];
            const glm::vec3 e = mesh.positions[r * cols + c + 1], s = mesh.positions[(r + 1) * cols + c];
            glm::vec3 normal(0.0f);
            const auto mid = render::horizon_surface(field, mesh, 0.5f * (glm::vec2(v) + glm::vec2(n)) + 0.1f, &normal);
            REQUIRE(mid);
            CHECK(*mid >= std::min({v.z, n.z, e.z, s.z}) - 0.01f);
            CHECK(*mid <= std::max({v.z, n.z, e.z, s.z}) + 0.01f);
            CHECK(normal.z > 0.0f);
            ++checked;
        }
    CHECK(checked > 20);
    CHECK_FALSE(render::horizon_surface(field, mesh, {field.xs.front() - 10.0f, 0.0f}));
}

TEST_CASE("horizon: the land's trees stand on it past the zone's edge, off the sea, fewer far out") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 2000.0f, 2000.0f, 100.0f, [](float, float) { return 20.0f; });
    // A sea along the west, its surface at 0.
    add_grid(tris, -1000.0f, 0.0f, 0.0f, 2000.0f, 100.0f, [](float, float) { return 0.0f; }, true);
    const auto land = render::horizon_land(tris, 4.0f);
    const auto field = render::horizon_field(tris, land);
    const auto mesh = render::horizon_mesh(field);
    const auto trees = render::horizon_trees(field, mesh, 80.0f, 400.0f);
    REQUIRE(trees.size() > 500);
    std::size_t near_count = 0, far_count = 0;
    for (const auto& t : trees) {
        // Outside the zone's squares (the sea's too) by the trees' start, within their reach.
        const float out = std::max({-1000.0f - t.foot.x, t.foot.x - 2000.0f, -t.foot.y, t.foot.y - 2000.0f});
        CHECK(out >= render::kHorizonTreeFrom - render::kHorizonCell);
        CHECK(out <= render::kHorizonTreeReach + render::kHorizonCell);
        // On the land as its mesh draws it, sunk a little.
        glm::vec3 n(0.0f);
        const auto z = render::horizon_surface(field, mesh, glm::vec2(t.foot), &n);
        REQUIRE(z);
        CHECK(t.foot.z == doctest::Approx(*z - render::kHorizonTreeSink).epsilon(1e-4));
        CHECK(n.z >= render::kHorizonTreeSlope);
        // Not on the open sea straight off the shore, nor under its surface.
        CHECK_FALSE((t.foot.x < -1500.0f && t.foot.y > 300.0f && t.foot.y < 1700.0f));
        CHECK(t.scale >= 0.8f);
        CHECK(t.scale <= 1.2f);
        CHECK(t.pick >= 0.0f);
        CHECK(t.pick < 1.0f);
        if (out < 2000.0f) ++near_count;
        if (out > 5000.0f) ++far_count;
    }
    // Per square unit, far fewer far out (the band 5000..7000 out is far larger than 256..2000).
    const auto band = [](float a, float b) {
        return (3000.0f + 2.0f * b) * (2000.0f + 2.0f * b) - (3000.0f + 2.0f * a) * (2000.0f + 2.0f * a);
    };
    CHECK(static_cast<float>(far_count) / band(5000.0f, 7000.0f) <
          0.7f * static_cast<float>(near_count) / band(256.0f, 2000.0f));
    // The same land, the same trees.
    const auto again = render::horizon_trees(field, mesh, 80.0f, 400.0f);
    REQUIRE(again.size() == trees.size());
    for (std::size_t i = 0; i < trees.size(); i += 97) CHECK(again[i].foot == trees[i].foot);
    // Thinner spacing, more trees; none for a field that is not valid.
    CHECK(render::horizon_trees(field, mesh, 160.0f, 800.0f).size() < trees.size());
    CHECK(render::horizon_trees(render::HorizonField{}, mesh, 80.0f, 400.0f).empty());
}

TEST_CASE("horizon: the zones floating in the sky, by name in any case") {
    CHECK(render::horizon_open_below("airplane"));
    CHECK(render::horizon_open_below("AirPlane"));
    CHECK(render::horizon_open_below("poair"));
    CHECK_FALSE(render::horizon_open_below("qeytoqrg"));
    CHECK_FALSE(render::horizon_open_below("airplanes"));
    CHECK_FALSE(render::horizon_open_below(""));
}

TEST_CASE("horizon: past the zone's edge the land rises gently; the mountains stand far off") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 2000.0f, 2000.0f, 100.0f, [](float, float) { return 20.0f; });
    const auto field = render::horizon_field(tris, render::horizon_land(tris, 0.0f));
    REQUIRE(field.valid());
    // Along the east edge, 300 and 1000 units out: near the edge's height (up to 0.7 the hills stood full height
    // 800 units out).
    float near300 = -1e9f, near1000 = -1e9f, far = -1e9f;
    for (float y = 0.0f; y <= 2000.0f; y += 50.0f) {
        near300 = std::max(near300, render::horizon_height(field, {2300.0f, y}));
        near1000 = std::max(near1000, render::horizon_height(field, {3000.0f, y}));
    }
    // Far out, somewhere, mountains.
    for (float a = 0.0f; a < 6.28f; a += 0.05f)
        far = std::max(far, render::horizon_height(field, {1000.0f + 9000.0f * std::cos(a), 1000.0f + 9000.0f * std::sin(a)}));
    CHECK(near300 < 20.0f + 30.0f);
    CHECK(near1000 < 20.0f + 120.0f);
    CHECK(far > 20.0f + 400.0f);
}

TEST_CASE("horizon: the land's ground that grows the zone's grass lies just past its edge") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 2000.0f, 2000.0f, 100.0f, [](float, float) { return 20.0f; });
    const auto field = render::horizon_field(tris, render::horizon_land(tris, 0.0f));
    REQUIRE(field.valid());
    const auto mesh = render::horizon_mesh(field);
    const auto lawn = render::horizon_lawn_ground(field, mesh);
    REQUIRE(lawn.size() >= 3);
    CHECK(lawn.size() % 3 == 0);
    bool outside_only = true, near_only = true;
    float farthest = 0.0f;
    for (std::size_t i = 0; i < lawn.size(); i += 3) {
        const glm::vec3 mid = (lawn[i] + lawn[i + 1] + lawn[i + 2]) / 3.0f;
        // Past the zone's ground (0..2000), not tucked under it.
        if (mid.x > 1.0f && mid.x < 1999.0f && mid.y > 1.0f && mid.y < 1999.0f) outside_only = false;
        const float past = std::max({0.0f - mid.x, mid.x - 2000.0f, 0.0f - mid.y, mid.y - 2000.0f});
        farthest = std::max(farthest, past);
        if (past > render::kHorizonLawnReach + 2.0f * render::kHorizonCell) near_only = false;
    }
    CHECK(outside_only);
    CHECK(near_only);
    CHECK(farthest > 0.5f * render::kHorizonLawnReach); // it does reach out
}
