// render/horizon.h: what a zone's terrain says about the land round it, and that land's mesh.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
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
    all.insert(all.end(), mesh.rock.begin(), mesh.rock.end());
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
    all.insert(all.end(), mesh.rock.begin(), mesh.rock.end());
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
        CHECK(mesh.positions[i].x < 100.0f); // off the west shore
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

namespace {

// A neighbour east of a flat zone (0..2000 x 0..1500 at 50): its ground at 150 from x 2000 to 6000, with a house
// (100 across, 400 high) at (3000..3100, 500..600) and a plateau (600 across, at 300) at (4000..4600, 0..600).
struct Neighbour {
    std::vector<HorizonTriangle> tris;
    render::HorizonLand land;
    render::HorizonField field;
    std::shared_ptr<const render::HorizonSource> source;
};

float neighbour_ground(float x, float y) {
    if (x >= 3000.0f && x < 3100.0f && y >= 500.0f && y < 600.0f) return 400.0f;
    if (x >= 4000.0f && x < 4600.0f && y >= 0.0f && y < 600.0f) return 300.0f;
    return 150.0f;
}

Neighbour make_neighbour() {
    Neighbour n;
    add_grid(n.tris, 0.0f, 0.0f, 2000.0f, 1500.0f, 100.0f, [](float, float) { return 50.0f; });
    n.land = render::horizon_land(n.tris, 3.0f);
    n.field = render::horizon_field(n.tris, n.land);
    const glm::vec2 lo(2000.0f, -512.0f);
    const float cell = render::kHorizonCell;
    const int nx = 63, ny = 40;
    std::vector<float> ground;
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x)
            ground.push_back(neighbour_ground(lo.x + (static_cast<float>(x) + 0.5f) * cell,
                                              lo.y + (static_cast<float>(y) + 0.5f) * cell));
    std::vector<float> top = ground;
    for (float& h : top) h += 5.0f;
    n.source =
        std::make_shared<const render::HorizonSource>(render::horizon_source("east", lo, cell, nx, ny, ground, top));
    return n;
}

} // namespace

TEST_CASE("horizon: a neighbour's ground loses what stands narrow on it, and fades out past it") {
    const Neighbour n = make_neighbour();
    REQUIRE(n.source->valid());
    const auto& s = *n.source;
    // The house is narrower than the opening: the ground runs under it. The plateau is wider: it stays.
    REQUIRE(render::source_ground(s, {3050.0f, 550.0f}));
    CHECK(near(*render::source_ground(s, {3050.0f, 550.0f}), 150.0f, 0.5f));
    CHECK(near(*render::source_ground(s, {4300.0f, 300.0f}), 300.0f, 0.5f));
    CHECK(near(*render::source_ground(s, {2500.0f, 1000.0f}), 150.0f, 0.5f));
    // Its terrain's highest points are kept as they are, the house's too.
    REQUIRE(render::source_top(s, {3050.0f, 550.0f}));
    CHECK(*render::source_top(s, {3050.0f, 550.0f}) == doctest::Approx(405.0f));
    CHECK_FALSE(render::source_top(s, {1900.0f, 700.0f}));
    // No ground before its edge (the zone is there), nor in its padding.
    CHECK_FALSE(render::source_ground(s, {1900.0f, 700.0f}));
    CHECK_FALSE(render::source_ground(s, {6500.0f, 700.0f}));
    // Its cover: whole on its ground, part way past it, none beyond the fade.
    CHECK(render::sample_source(s, {2500.0f, 700.0f}).cover == doctest::Approx(1.0f));
    const float half = render::sample_source(s, {6032.0f + 0.5f * render::kHorizonSourceFade, 700.0f}).cover;
    CHECK(half > 0.2f);
    CHECK(half < 0.8f);
    CHECK(render::sample_source(s, {6100.0f + render::kHorizonSourceFade, 700.0f}).cover == 0.0f);
    CHECK(render::sample_source(s, {20000.0f, 700.0f}).cover == 0.0f);
    // Past it, the nearest ground's height.
    CHECK(near(render::sample_source(s, {6400.0f, 700.0f}).height, 150.0f, 0.5f));
}

TEST_CASE("horizon: past the zone's edge the land eases onto a neighbour's ground, and back into its hills past it") {
    const Neighbour n = make_neighbour();
    REQUIRE(n.field.valid());
    const render::HorizonSources sources{n.source};
    // At the zone's edge, its height (tucked a little under it); across the band, the neighbour's.
    CHECK(render::horizon_height(n.field, sources, {2140.0f, 700.0f}) < 90.0f);
    CHECK(near(render::horizon_height(n.field, sources, {3500.0f, 1000.0f}), 150.0f, 1.0f));
    CHECK(near(render::horizon_height(n.field, sources, {2700.0f, 1000.0f}), 150.0f, 1.0f));
    // Under the house the ground; on the plateau the plateau.
    CHECK(near(render::horizon_height(n.field, sources, {3050.0f, 550.0f}), 150.0f, 1.0f));
    CHECK(near(render::horizon_height(n.field, sources, {4300.0f, 300.0f}), 300.0f, 1.0f));
    // Between the edge and the band's end it climbs steadily.
    float last = -1e9f;
    for (float x = 2100.0f; x <= 2600.0f; x += 50.0f) {
        const float h = render::horizon_height(n.field, sources, {x, 1000.0f});
        CHECK(h >= last - 0.5f);
        last = h;
    }
    // Beyond its fade the land is the zone's own hills, as without the neighbour; on the zone's other sides too.
    for (glm::vec2 p : {glm::vec2(9000.0f, 700.0f), glm::vec2(-1500.0f, 700.0f), glm::vec2(1000.0f, -2500.0f)})
        CHECK(render::horizon_height(n.field, sources, p) == render::horizon_height(n.field, {}, p));
    // Over the zone, tucked under it, the neighbour or not.
    CHECK(render::horizon_height(n.field, sources, {1000.0f, 700.0f}) == render::horizon_height(n.field, {}, {1000.0f, 700.0f}));
}

TEST_CASE("horizon: a neighbour's part of the land is the land's own squares, meeting the rest edge to edge") {
    const Neighbour n = make_neighbour();
    const render::HorizonSources sources{n.source};
    const auto full = render::horizon_mesh(n.field, sources);
    const auto part = render::horizon_mesh(n.field, sources, 0);
    const auto without = render::horizon_mesh(n.field, {});
    REQUIRE_FALSE(part.squares.empty());
    CHECK(std::is_sorted(part.squares.begin(), part.squares.end()));
    CHECK(part.water.empty());
    CHECK(part.weight.size() == part.positions.size());
    std::vector<std::uint32_t> mine = part.ground;
    mine.insert(mine.end(), part.rock.begin(), part.rock.end());
    REQUIRE_FALSE(mine.empty());
    std::vector<bool> in_part(full.positions.size(), false);
    for (std::size_t i = 0; i < mine.size(); i += 3) {
        const glm::vec3 a = part.positions[mine[i]], b = part.positions[mine[i + 1]], c = part.positions[mine[i + 2]];
        // Its triangles lie in its squares, wound as the land's, at the land's own heights.
        CHECK(std::binary_search(part.squares.begin(), part.squares.end(), n.field.mesh_square(glm::vec2(a + b + c) / 3.0f)));
        CHECK(glm::cross(b - a, c - a).z > 0.0f);
        // At the land's own heights, but past the band sunk under the neighbour's own terrain, which is drawn there.
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t v = mine[i + k];
            CHECK(part.positions[v].x == full.positions[v].x);
            CHECK(part.positions[v].y == full.positions[v].y);
            CHECK(part.positions[v].z == doctest::Approx(full.positions[v].z - render::kHorizonTuck * part.settled[v]));
            in_part[v] = true;
        }
    }
    // Where the rest of the land meets it, their shared corners are where the land is without the neighbour: the
    // current zone's land, built before the neighbour was measured, meets it with no crack.
    std::vector<std::uint32_t> rest = full.ground;
    rest.insert(rest.end(), full.rock.begin(), full.rock.end());
    std::size_t shared = 0;
    for (std::size_t i = 0; i < rest.size(); i += 3) {
        const glm::vec2 middle = glm::vec2(full.positions[rest[i]] + full.positions[rest[i + 1]] + full.positions[rest[i + 2]]) / 3.0f;
        if (std::binary_search(part.squares.begin(), part.squares.end(), n.field.mesh_square(middle))) continue;
        for (int k = 0; k < 3; ++k)
            if (in_part[rest[i + k]]) {
                ++shared;
                CHECK(full.positions[rest[i + k]] == without.positions[rest[i + k]]);
            }
    }
    CHECK(shared > 0);
    // Its own ground shows across the band and on: little at the zone's edge, all of it well past.
    auto weight_near = [&](glm::vec2 p) {
        float best = 1e9f, w = -1.0f;
        for (std::uint32_t v : mine)
            if (const float d = glm::distance(glm::vec2(part.positions[v]), p); d < best) {
                best = d;
                w = part.weight[v];
            }
        return w;
    };
    CHECK(weight_near({2100.0f, 1000.0f}) < 0.3f);
    // Sunk under its terrain well past the band, not at the zone's edge; horizon_point says the same.
    CHECK(render::horizon_point(n.field, sources, {3500.0f, 1000.0f}, 0).settled == doctest::Approx(1.0f));
    CHECK(render::horizon_point(n.field, sources, {3500.0f, 1000.0f}, 0).height ==
          doctest::Approx(render::horizon_height(n.field, sources, {3500.0f, 1000.0f}) - render::kHorizonTuck));
    CHECK(render::horizon_point(n.field, sources, {2100.0f, 1000.0f}, 0).settled == 0.0f);
    CHECK(render::horizon_point(n.field, sources, {3500.0f, 1000.0f}, -1).settled == 0.0f);
    // By the neighbour's own edge (its boundary walls stand there) the land does not sink: its terrain goes there.
    CHECK(render::sample_source(*n.source, {6000.0f, 1000.0f}).inner < render::kHorizonInnerFrom);
    CHECK(render::sample_source(*n.source, {3500.0f, 1000.0f}).inner > render::kHorizonInnerTo);
    CHECK(render::horizon_point(n.field, sources, {6000.0f, 1000.0f}, 0).settled == 0.0f);
    CHECK(render::horizon_point(n.field, sources, {3500.0f, 2010.0f}, 0).settled == 0.0f);
    CHECK(weight_near({3500.0f, 1000.0f}) == doctest::Approx(1.0f));
    for (float w : part.weight) {
        CHECK(w >= 0.0f);
        CHECK(w <= 1.0f);
    }
    // Asked for a source it does not have: nothing.
    CHECK(render::horizon_mesh(n.field, sources, 3).empty());
}

TEST_CASE("horizon: the sources are kept by name, once each") {
    const Neighbour n = make_neighbour();
    auto other = std::make_shared<render::HorizonSource>(*n.source);
    other->name = "alpha";
    render::HorizonSources sources{n.source, other, n.source, std::make_shared<const render::HorizonSource>()};
    render::sort_sources(sources);
    REQUIRE(sources.size() == 2);
    CHECK(sources[0]->name == "alpha");
    CHECK(sources[1]->name == "east");
}

TEST_CASE("horizon: a neighbour's ground is cached and read back; a damaged file is refused") {
    const Neighbour n = make_neighbour();
    render::HorizonCacheEntry e;
    e.zone = "ecommons";
    e.neighbour = "east";
    e.key = 0x1234567890abcdefull;
    e.offset = {2.5f, -2399.4f, -26.5f};
    e.source = *n.source;
    const auto bytes = render::write_horizon_cache(e);
    const auto back = render::read_horizon_cache(bytes);
    REQUIRE(back);
    CHECK(back->zone == "ecommons");
    CHECK(back->neighbour == "east");
    CHECK(back->key == e.key);
    CHECK(back->offset == e.offset);
    const auto& s = back->source;
    REQUIRE(s.valid());
    CHECK(s.name == "east");
    CHECK(s.lo == n.source->lo);
    CHECK(s.nx == n.source->nx);
    CHECK(s.ny == n.source->ny);
    for (std::size_t i = 0; i < s.height.size(); ++i) {
        CHECK(std::isnan(s.height[i]) == std::isnan(n.source->height[i]));
        if (!std::isnan(s.height[i])) CHECK(s.height[i] == n.source->height[i]);
    }
    CHECK(s.fill == n.source->fill);
    REQUIRE(s.top.size() == n.source->top.size());
    for (std::size_t i = 0; i < s.top.size(); ++i) {
        CHECK(std::isnan(s.top[i]) == std::isnan(n.source->top[i]));
        if (!std::isnan(s.top[i])) CHECK(s.top[i] == n.source->top[i]);
    }
    CHECK(s.dist == n.source->dist);
    // Cut short, too long, another version, an absurd size.
    CHECK_FALSE(render::read_horizon_cache(std::span(bytes).first(bytes.size() - 1)));
    auto longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(render::read_horizon_cache(longer));
    auto other = bytes;
    other[4] = std::byte{9};
    CHECK_FALSE(render::read_horizon_cache(other));
    CHECK_FALSE(render::read_horizon_cache({}));
}
