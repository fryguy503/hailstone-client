#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "assets/regions.h"
#include "assets/wld.h"
#include "game/neighbour_zones.h"

using namespace hailstone;
using namespace hailstone::game;
namespace wld = hailstone::assets::wld;

namespace {

wld::BspNode split(glm::vec3 n, float d, std::uint32_t front, std::uint32_t back) {
    wld::BspNode node;
    node.normal = n;
    node.distance = d;
    node.front = front;
    node.back = back;
    return node;
}

wld::BspNode leaf(std::uint32_t region) {
    wld::BspNode node;
    node.region = region;
    return node;
}

wld::RegionTag tag(std::string text, std::vector<std::uint32_t> regions) {
    wld::RegionTag t;
    t.tag = std::move(text);
    t.regions = std::move(regions);
    return t;
}

// East Commonlands' west line in miniature (world coordinates: world y is the server's x): a slab y in [-1610, -1600]
// across x in [-577, 583], the rest of the zone region 2. Region 1 is tagged as a reference to zone point 3.
assets::ZoneRegions west_line_zone() {
    std::vector<wld::BspNode> nodes = {
        split({0, 1, 0}, 1610, 2, 7),   // 1: y > -1610
        split({0, -1, 0}, -1600, 3, 7), // 2: y < -1600
        split({1, 0, 0}, 577, 4, 7),    // 3: x > -577
        split({-1, 0, 0}, 583, 5, 7),   // 4: x < 583
        leaf(1),                        // 5
        leaf(2),                        // 6 (unused)
        leaf(2),                        // 7
    };
    return assets::ZoneRegions::build(nodes, 2, {tag("DRNTP00255000003000000000000000_ZONE", {0})});
}

const glm::vec3 kZoneLo(-3177, -2230, -271), kZoneHi(1908, 5601, 720);

} // namespace

TEST_CASE("neighbour zones: a BSP leaf's cell is its split planes cut from the bounds") {
    auto regions = west_line_zone();
    auto cells = regions.leaf_cells(kZoneLo, kZoneHi, [](std::uint32_t r) { return r == 1; });
    REQUIRE(cells.size() == 1);
    CHECK(cells[0].region == 1);
    CHECK(cells[0].lo.x == doctest::Approx(-577));
    CHECK(cells[0].hi.x == doctest::Approx(583));
    CHECK(cells[0].lo.y == doctest::Approx(-1610));
    CHECK(cells[0].hi.y == doctest::Approx(-1600));
    CHECK(cells[0].lo.z == doctest::Approx(-271));
    CHECK(cells[0].hi.z == doctest::Approx(720));

    // An oblique split: the cell's box is the cut polytope's, not the bounds'.
    std::vector<wld::BspNode> oblique = {split(glm::normalize(glm::vec3(1, 1, 0)), 0, 2, 3), leaf(1), leaf(2)};
    auto o = assets::ZoneRegions::build(oblique, 2, {});
    auto back = o.leaf_cells(glm::vec3(-10), glm::vec3(10), [](std::uint32_t r) { return r == 2; });
    REQUIRE(back.size() == 1);
    CHECK(back[0].lo.x == doctest::Approx(-10));
    CHECK(back[0].hi.x == doctest::Approx(10));
    CHECK(back[0].hi.y == doctest::Approx(10));
    // x + y <= 0 inside [-10, 10]^2: the corner (10, 10) is cut off, (10, -10) is not.
    auto none = o.leaf_cells(glm::vec3(-10), glm::vec3(10), [](std::uint32_t) { return false; });
    CHECK(none.empty());
}

TEST_CASE("neighbour zones: zone point targets in world coordinates, 999999 kept") {
    auto t = line_target_from_server(775.0f, -28.0f, 999999.0f);
    CHECK(t.position.x == doctest::Approx(-28.0f)); // world x = server y
    CHECK(t.position.y == doctest::Approx(775.0f)); // world y = server x
    CHECK(!t.keep.x);
    CHECK(!t.keep.y);
    CHECK(t.keep.z);
    auto k = line_target_from_server(-1592.0f, 999999.0f, -54.0f);
    CHECK(k.keep.x);
    CHECK(!k.keep.y);
}

TEST_CASE("neighbour zones: the lines by destination, their seam and the offset") {
    auto regions = west_line_zone();
    // ecommons' zone point 3: freportw (9) at server (775, -28, -24); point 5 leads elsewhere and has no line.
    std::vector<ZonePointTarget> points = {{3, 9, 775.0f, -28.0f, -24.0f}, {5, 25, 0.0f, 0.0f, 0.0f}};
    auto lines = collect_neighbour_lines(regions, kZoneLo, kZoneHi, points, 22);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].zone_id == 9);
    CHECK(lines[0].targets.size() == 1);
    // Lines back into the current zone are left out.
    CHECK(collect_neighbour_lines(regions, kZoneLo, kZoneHi, points, 9).empty());

    Seam seam = seam_of(lines[0], kZoneLo, kZoneHi);
    CHECK(seam.axis == 1);
    CHECK(seam.sign == -1.0f);
    CHECK(seam.face == doctest::Approx(-1600.0f)); // the inner face: where the player crosses walking west

    auto first = offset_from_targets(lines[0], seam);
    CHECK(first.offset.y == doctest::Approx(-1600.0f - 775.0f));
    CHECK(first.offset.x == doctest::Approx(3.0f + 28.0f)); // the line's middle less the arrival

    // freportw's own line back: y in [807, 1387] over x in [-564, 576]; and a far line that is not it.
    std::vector<LineCell> there = {{{-564, 807, -114}, {576, 1387, 150}}, {{-724, -1700, -94}, {-700, -1600, -80}}};
    auto back = find_return_line(there, seam, seam.centre - first.offset);
    REQUIRE(back.has_value());
    CHECK(back->cells == 1);
    CHECK(back->face == doctest::Approx(807.0f));
    // The line back as the neighbour's own seam: facing the other way, over its own cells, tested for open sky the
    // same way (a tunnel seen from its open mouth is still a tunnel).
    Seam rs = return_seam(*back, seam);
    CHECK(rs.axis == seam.axis);
    CHECK(rs.sign == -seam.sign);
    CHECK(rs.face == doctest::Approx(807.0f));
    CHECK(rs.lo.y == doctest::Approx(807.0f));
    CHECK(rs.hi.y == doctest::Approx(1387.0f));
    for (const auto& p : seam_points(rs, 3)) CHECK(p.y == doctest::Approx(807.0f));
    auto ground = [](const glm::vec3&) -> std::optional<float> { return 0.0f; };
    auto roof = [](const glm::vec3&) -> std::optional<float> { return 30.0f; };
    CHECK(open_sky_share(seam_points(rs, 3), ground, roof) == doctest::Approx(0.0f));
    NeighbourCheck closed;
    closed.outdoor_here = true;
    closed.cells = 1;
    closed.open_here = 1.0f;  // the desert side's mouth
    closed.open_there = 0.0f; // the line back, under the tunnel's roof
    closed.return_line = 1;
    CHECK(neighbour_verdict(closed) == NeighbourVerdict::ClosedThere);

    auto points_across = seam_points(seam, 5);
    REQUIRE(points_across.size() == 5);
    for (const auto& p : points_across) CHECK(p.y == doctest::Approx(-1600.0f));
    auto here = [](const glm::vec3&) { return std::vector<float>{-51.0f}; };
    auto far = [](const glm::vec3&) { return std::vector<float>{-24.0f}; };
    auto refined = refine_offset(first, seam, lines[0], back, points_across, here, far);
    CHECK(refined.return_line);
    CHECK(refined.ground);
    CHECK(refined.offset.y == doctest::Approx(-1600.0f - 807.0f)); // the two faces lined up
    CHECK(refined.offset.x == doctest::Approx(3.0f - 6.0f));       // the two middles
    CHECK(refined.offset.z == doctest::Approx(-27.0f));            // the ground matched at the seam

    // Several floors (an invisible lid over the line at 80 on this side): the pair nearest the first estimate.
    auto lid = [](const glm::vec3&) { return std::vector<float>{80.0f, -51.0f}; };
    auto two = [](const glm::vec3&) { return std::vector<float>{60.0f, -24.0f}; };
    NeighbourOffset hinted = first;
    hinted.offset.z = -30.0f;
    auto picked = refine_offset(hinted, seam, lines[0], back, points_across, lid, two);
    CHECK(picked.offset.z == doctest::Approx(-27.0f));
    // No ground on either side: z stays the first estimate.
    auto none = [](const glm::vec3&) { return std::vector<float>{}; };
    CHECK(!refine_offset(hinted, seam, lines[0], back, points_across, none, two).ground);

    // A return line far off the targets' estimate is not taken.
    std::vector<LineCell> wrong = {{{-564, 1100, -114}, {576, 1387, 150}}};
    auto off = find_return_line(wrong, seam, seam.centre - first.offset, 400.0f);
    REQUIRE(off.has_value());
    auto kept = refine_offset(first, seam, lines[0], off, {}, nullptr, nullptr, 120.0f);
    CHECK(kept.offset.y == doctest::Approx(first.offset.y));
    CHECK(!kept.ground);
}

TEST_CASE("neighbour zones: a target that keeps a coordinate fixes that axis at 0") {
    // Two zone points to the same zone over the one line: one absolute, one keeping the server's y (world x).
    std::vector<wld::BspNode> nodes = {
        split({0, 1, 0}, 1610, 2, 8), split({0, -1, 0}, -1600, 3, 8), split({1, 0, 0}, 577, 4, 8),
        split({-1, 0, 0}, 583, 5, 8), split({1, 0, 0}, 0, 6, 7),      leaf(1), leaf(3), leaf(2)};
    auto two = assets::ZoneRegions::build(nodes, 3,
                                          {tag("DRNTP00255000003000000000000000_ZONE", {0}),
                                           tag("DRNTP00255000004000000000000000_ZONE", {2})});
    std::vector<ZonePointTarget> points = {{3, 9, 775.0f, -28.0f, -24.0f}, {4, 9, 780.0f, 999999.0f, -24.0f}};
    auto lines = collect_neighbour_lines(two, kZoneLo, kZoneHi, points, 22);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].cells.size() == 2);
    CHECK(lines[0].targets.size() == 2);
    Seam seam = seam_of(lines[0], kZoneLo, kZoneHi);
    auto o = offset_from_targets(lines[0], seam);
    CHECK(o.offset.x == 0.0f);
    CHECK(o.offset.y == doctest::Approx(-1600.0f - 777.5f)); // the median of two
    CHECK(o.spread.y == doctest::Approx(5.0f));
    CHECK(o.estimates == 2);
}

TEST_CASE("neighbour zones: the clip keeps what lies past the seam, triangles split along the plane") {
    Seam seam;
    seam.axis = 1;
    seam.sign = -1.0f;
    seam.face = -1600.0f;
    glm::vec4 plane = seam_clip_plane(seam, 0.0f);
    CHECK(plane_side(plane, glm::vec3(0, -1700, 0)) > 0.0f);
    CHECK(plane_side(plane, glm::vec3(0, -1500, 0)) < 0.0f);
    CHECK(plane_side(plane, glm::vec3(0, -1600, 0)) == doctest::Approx(0.0f));
    glm::vec4 further = seam_clip_plane(seam, 50.0f);
    CHECK(plane_side(further, glm::vec3(0, -1620, 0)) < 0.0f);

    auto position = [](const glm::vec3& v) { return v; };
    auto lerp = [](const glm::vec3& a, const glm::vec3& b, float t) { return a + (b - a) * t; };
    std::vector<glm::vec3> scratch;
    // A triangle across the plane: one corner cut off leaves a quad.
    std::vector<glm::vec3> tri = {{0, -1700, 0}, {100, -1700, 0}, {0, -1500, 0}};
    clip_polygon(tri, plane, position, lerp, scratch);
    REQUIRE(tri.size() == 4);
    for (const auto& v : tri) CHECK(v.y <= -1600.0f + 1e-3f);
    // Wholly on the near side: nothing.
    std::vector<glm::vec3> near = {{0, -1550, 0}, {100, -1550, 0}, {0, -1500, 0}};
    clip_polygon(near, plane, position, lerp, scratch);
    CHECK(near.empty());
    // Wholly past it: unchanged.
    std::vector<glm::vec3> past = {{0, -1650, 0}, {100, -1650, 0}, {0, -1700, 0}};
    clip_polygon(past, plane, position, lerp, scratch);
    CHECK(past.size() == 3);
}

TEST_CASE("neighbour zones: which lines get their neighbour drawn") {
    NeighbourCheck c;
    c.outdoor_here = true;
    c.cells = 3;
    c.open_here = 1.0f;
    c.open_there = 0.8f;
    CHECK(neighbour_verdict(c) == NeighbourVerdict::Draw);
    c.open_there = -1.0f; // not tested yet
    CHECK(neighbour_verdict(c) == NeighbourVerdict::Draw);

    auto with = [&](auto change) {
        NeighbourCheck x = c;
        change(x);
        return neighbour_verdict(x);
    };
    CHECK(with([](NeighbourCheck& x) { x.enabled = false; }) == NeighbourVerdict::Off);
    CHECK(with([](NeighbourCheck& x) { x.outdoor_here = false; }) == NeighbourVerdict::IndoorHere);
    CHECK(with([](NeighbourCheck& x) { x.cells = 0; }) == NeighbourVerdict::NoLine);
    CHECK(with([](NeighbourCheck& x) { x.files = false; }) == NeighbourVerdict::NoFiles);
    CHECK(with([](NeighbourCheck& x) { x.open_here = 0.2f; }) == NeighbourVerdict::ClosedHere);
    CHECK(with([](NeighbourCheck& x) { x.open_there = 0.4f; }) == NeighbourVerdict::ClosedThere);
    CHECK(with([](NeighbourCheck& x) { x.spread = 800.0f; }) == NeighbourVerdict::Disagree);
    CHECK(with([](NeighbourCheck& x) { x.spread = 40.0f; }) == NeighbourVerdict::Draw);
    CHECK(with([](NeighbourCheck& x) { x.return_line = 0; }) == NeighbourVerdict::NoReturn);
    CHECK(with([](NeighbourCheck& x) { x.return_line = 1; }) == NeighbourVerdict::Draw);

    // Open sky: no ceiling within kOpenSkyHeight of the ground.
    std::vector<glm::vec3> pts = {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}};
    auto ground = [](const glm::vec3& p) -> std::optional<float> {
        if (p.x > 25.0f) return std::nullopt; // no ground: not counted
        return 0.0f;
    };
    auto roof = [](const glm::vec3& p) -> std::optional<float> {
        if (p.x < 5.0f) return 20.0f;   // a tunnel
        if (p.x < 15.0f) return 900.0f; // a high arch: open
        return std::nullopt;
    };
    CHECK(open_sky_share(pts, ground, roof) == doctest::Approx(2.0f / 3.0f));
    CHECK(distance_xy({0, 0, 0}, {10, -5, 0}, {20, 5, 0}) == doctest::Approx(10.0f));
    CHECK(distance_xy({15, 0, 99}, {10, -5, 0}, {20, 5, 0}) == 0.0f);
}

namespace {

// The area of the kept pieces of triangle (a, b, c), in the horizontal plane.
float kept_area(const std::vector<BaryPolygon>& pieces, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    float area = 0.0f;
    for (const auto& poly : pieces)
        for (std::size_t i = 1; i + 1 < poly.size(); ++i) {
            const glm::vec3 p = a * poly[0].x + b * poly[0].y + c * poly[0].z;
            const glm::vec3 q = a * poly[i].x + b * poly[i].y + c * poly[i].z;
            const glm::vec3 r = a * poly[i + 1].x + b * poly[i + 1].y + c * poly[i + 1].z;
            area += 0.5f * std::abs((q.x - p.x) * (r.y - p.y) - (r.x - p.x) * (q.y - p.y));
        }
    return area;
}

} // namespace

TEST_CASE("Neighbour zones: the neighbour's ground goes where the current zone has its own, piece by piece") {
    // The current zone's ground: flat at z 0 over x in [0, 240), none beyond (its edge), on a 24-unit grid.
    GroundGrid g = sample_ground({0.0f, 0.0f}, {480.0f, 240.0f}, 24.0f, [](float x, float) -> std::optional<float> {
        if (x < 240.0f) return 0.0f;
        return std::nullopt;
    });
    REQUIRE(g.width == 20);
    CHECK(g.height_at(100.0f, 100.0f) == doctest::Approx(0.0f));
    CHECK_FALSE(g.height_at(300.0f, 100.0f));
    CHECK(g.inside_ground(100.0f, 100.0f));
    CHECK_FALSE(g.inside_ground(230.0f, 100.0f)); // the last cell before the edge
    CHECK_FALSE(g.inside_ground(100.0f, 10.0f));  // the grid's own edge

    // Wholly over the ground, 3 above it: all of it goes.
    {
        auto parts = split_off_ground(g, {50, 50, 3}, {150, 50, 3}, {50, 150, 3});
        REQUIRE(parts);
        CHECK(parts->empty());
    }
    // 40 above (a hill, a roof): it stays whole. A wall (facing sideways) stays whole.
    CHECK_FALSE(split_off_ground(g, {50, 50, 40}, {150, 50, 40}, {50, 150, 40}));
    CHECK_FALSE(split_off_ground(g, {50, 50, 0}, {150, 50, 0}, {150, 50, 80}));
    // Wholly past the ground's edge: stays whole.
    CHECK_FALSE(split_off_ground(g, {300, 50, 0}, {400, 50, 0}, {300, 150, 0}));
    // Across the edge: the part over the ground goes, the part past it (and the cell along the edge) stays, so no gap
    // opens where the current zone's ground ends.
    {
        const glm::vec3 a{100, 60, 1}, b{400, 60, 1}, c{100, 180, 1};
        auto parts = split_off_ground(g, a, b, c);
        REQUIRE(parts);
        REQUIRE_FALSE(parts->empty());
        const float whole = 0.5f * 300.0f * 120.0f;
        const float kept = kept_area(*parts, a, b, c);
        CHECK(kept < whole);
        CHECK(kept > 0.0f);
        // Every point past x = 216 (the cells within one of the edge) is still covered by a kept piece.
        for (float x = 220.0f; x < 390.0f; x += 7.0f) {
            const glm::vec2 p(x, 62.0f);
            bool covered = false;
            for (const auto& poly : *parts) {
                bool inside = true;
                for (std::size_t i = 0; i < poly.size() && inside; ++i) {
                    const glm::vec3 u = a * poly[i].x + b * poly[i].y + c * poly[i].z;
                    const glm::vec3 w = a * poly[(i + 1) % poly.size()].x + b * poly[(i + 1) % poly.size()].y +
                                        c * poly[(i + 1) % poly.size()].z;
                    const float side = (w.x - u.x) * (p.y - u.y) - (w.y - u.y) * (p.x - u.x);
                    inside = side >= -1e-3f;
                }
                if (inside) covered = true;
            }
            CHECK_MESSAGE(covered, "x = ", x);
        }
        // The pieces' weights stay on the triangle.
        for (const auto& poly : *parts)
            for (const glm::vec3& w : poly) {
                CHECK(w.x + w.y + w.z == doctest::Approx(1.0f));
                CHECK(std::min({w.x, w.y, w.z}) >= -1e-4f);
            }
    }
}

TEST_CASE("neighbour zones: where the land stands in for the neighbour's ground, its ground goes and what stands stays") {
    // The neighbour's ground at 100 over 0..256 x 0..128 on 32-unit cells; past x 192 the land lies lower than it,
    // so nothing may stand there (a house would float).
    LandMask m;
    m.origin = {0.0f, 0.0f};
    m.cell = 32.0f;
    m.width = 8;
    m.height = 4;
    m.ground.assign(32, 100.0f);
    m.stands.assign(32, 1);
    for (int y = 0; y < m.height; ++y)
        for (int x = 6; x < m.width; ++x) m.stands[static_cast<std::size_t>(y * m.width + x)] = 0;
    REQUIRE(m.ground_at(50.0f, 50.0f));
    CHECK(*m.ground_at(50.0f, 50.0f) == doctest::Approx(100.0f));
    CHECK_FALSE(m.ground_at(300.0f, 50.0f));
    CHECK(m.stands_at(50.0f, 50.0f));
    CHECK_FALSE(m.stands_at(200.0f, 50.0f));
    CHECK(m.stands_at(400.0f, 50.0f)); // outside the grid

    // Its ground, either way up: it goes, the land is drawn there.
    for (bool flip : {false, true}) {
        const glm::vec3 a{10, 10, 104}, b{150, 10, 104}, c{10, 100, 104};
        auto parts = flip ? split_off_land(m, a, c, b) : split_off_land(m, a, b, c);
        REQUIRE(parts);
        CHECK(parts->empty());
    }
    // A roof well over the ground where things may stand: it stays whole. A wall too.
    CHECK_FALSE(split_off_land(m, {10, 10, 200}, {150, 10, 200}, {10, 100, 200}));
    CHECK_FALSE(split_off_land(m, {20, 40, 100}, {150, 40, 100}, {150, 40, 300}));
    // A wall running into where nothing may stand: the part there goes, the rest stays.
    {
        const glm::vec3 a{100, 40, 100}, b{250, 40, 100}, c{250, 40, 300};
        auto parts = split_off_land(m, a, b, c);
        REQUIRE(parts);
        REQUIRE_FALSE(parts->empty());
        for (const auto& poly : *parts)
            for (const glm::vec3& w : poly) CHECK((a * w.x + b * w.y + c * w.z).x <= 192.0f + 1e-3f);
    }
    // Outside the mask: whole. An empty mask: whole.
    CHECK_FALSE(split_off_land(m, {300, 10, 100}, {400, 10, 100}, {300, 100, 100}));
    CHECK_FALSE(split_off_land(LandMask{}, {10, 10, 104}, {150, 10, 104}, {10, 100, 104}));
}

TEST_CASE("neighbour zones: a thin line box faces along its thin axis, a squarer one the nearest edge") {
    // East Commonlands' tunnel to North Ro: 139 across in x, 20 thick in y, but nearer the zone's west edge (711)
    // than its south one (2070). The line faces south (-y), as North Ro's own end of the tunnel faces north.
    NeighbourLines tunnel;
    tunnel.lo = {-2535, -170, -5};
    tunnel.hi = {-2396, -150, 50};
    Seam s = seam_of(tunnel, kZoneLo, kZoneHi);
    CHECK(s.axis == 1);
    CHECK(s.sign == -1.0f);
    CHECK(s.face == doctest::Approx(-150.0f));
    // North Ro's end: (2535 2903 -5)-(2673 2923 50) in a zone (-2524 -1661 -140)-(4776 3976 900): north (+y).
    NeighbourLines nro;
    nro.lo = {2535, 2903, -5};
    nro.hi = {2673, 2923, 50};
    Seam n = seam_of(nro, {-2524, -1661, -140}, {4776, 3976, 900});
    CHECK(n.axis == 1);
    CHECK(n.sign == 1.0f);
    CHECK(n.face == doctest::Approx(2903.0f));
    // A box under twice as long one way as the other: the nearest edge.
    NeighbourLines square;
    square.lo = {-2535, -200, -5};
    square.hi = {-2396, -100, 50};
    Seam q = seam_of(square, kZoneLo, kZoneHi);
    CHECK(q.axis == 0);
    CHECK(q.sign == -1.0f);
}

namespace {

// Floors per seam point, matched to the point by its side coordinate (the y of a seam facing -x); `shift` the side
// offset taken off the neighbour's probes.
Floors floors_by_point(const std::vector<glm::vec3>& points, std::vector<std::vector<float>> floors, float shift) {
    return [points, floors = std::move(floors), shift](const glm::vec3& p) {
        for (std::size_t i = 0; i < points.size(); ++i)
            if (std::abs(points[i].y - shift - p.y) < 0.5f) return floors[i];
        return std::vector<float>{};
    };
}

} // namespace

TEST_CASE("neighbour zones: z is the ground difference most seam points agree on, not the one nearest the estimate") {
    // North Ro's line to Oasis (seam -x at -1889.2), from the log. North Ro has an invisible plane at -111 under its
    // shore, and the targets' first estimate (-115.9, from the arrival's spawn height) is near it: the pair nearest the
    // estimate gave -122.2 and drew Oasis 120 units low. Its sand meets North Ro's within a few units at every point.
    Seam seam;
    seam.axis = 0;
    seam.sign = -1.0f;
    seam.face = -1889.2f;
    seam.lo = {-1925, -1457, -72};
    seam.hi = {-1889, 1569, 180};
    seam.centre = 0.5f * (seam.lo + seam.hi);
    NeighbourLines lines;
    lines.targets.push_back({});
    NeighbourOffset first;
    first.offset = {-4434.7f, -36.7f, -115.9f};
    const auto points = seam_points(seam);
    REQUIRE(points.size() == 9);
    auto here = floors_by_point(points,
                                {{-33}, {-31, -111}, {6, -111}, {48, -111}, {50, -111}, {-12, -111}, {14, -111},
                                 {29, -111}, {50, 10, -111}},
                                0.0f);
    auto there = floors_by_point(points, {{-33}, {-26}, {10}, {44}, {46}, {-5}, {11}, {26}, {30, 11, -31}},
                                 first.offset.y);
    auto o = refine_offset(first, seam, lines, std::nullopt, points, here, there);
    REQUIRE(o.ground);
    CHECK(std::abs(o.offset.z) < 2.0f);
    CHECK(o.spread.z < 16.0f);

    // East Commonlands' line to Nektulos: no ground at two points, hillsides at four where the two zones differ, the
    // road at three where they meet: z from those three.
    auto here2 = floors_by_point(points, {{}, {}, {52}, {-22}, {-22}, {-22}, {60}, {127}, {151}}, 0.0f);
    auto there2 = floors_by_point(points, {{}, {}, {95}, {-22}, {-22}, {-22}, {-22}, {-22}, {-22}}, first.offset.y);
    NeighbourOffset hinted = first;
    hinted.offset.z = -24.7f;
    auto road = refine_offset(hinted, seam, lines, std::nullopt, points, here2, there2);
    REQUIRE(road.ground);
    CHECK(road.offset.z == doctest::Approx(0.0f));
    // Too few points agree: z stays the estimate.
    auto here3 = floors_by_point(points, {{}, {}, {}, {}, {}, {0}, {40}, {80}, {120}}, 0.0f);
    auto there3 = floors_by_point(points, {{}, {}, {}, {}, {}, {0}, {0}, {0}, {0}}, first.offset.y);
    CHECK(!refine_offset(hinted, seam, lines, std::nullopt, points, here3, there3).ground);
}

TEST_CASE("neighbour zones: the ground past the seam sampled a row at a time is the grid sampled at once") {
    auto floor = [](float x, float y) -> std::optional<float> {
        if (x > 40.0f && y > 40.0f) return std::nullopt; // a hole
        return x * 0.25f - y;
    };
    const glm::vec2 lo(-10.0f, 5.0f), hi(110.0f, 77.0f);
    const GroundGrid whole = sample_ground(lo, hi, 16.0f, floor);
    GroundSampler sampler(lo, hi, 16.0f);
    int steps = 0;
    while (!sampler.step(floor, [] { return false; })) ++steps; // one row each
    CHECK(steps + 1 == whole.height);
    const GroundGrid rows = sampler.take();
    REQUIRE(rows.width == whole.width);
    REQUIRE(rows.height == whole.height);
    REQUIRE(rows.heights.size() == whole.heights.size());
    for (std::size_t i = 0; i < rows.heights.size(); ++i) {
        if (std::isnan(whole.heights[i])) CHECK(std::isnan(rows.heights[i]));
        else CHECK(rows.heights[i] == whole.heights[i]);
    }
    // An empty band: done at once, an empty grid.
    GroundSampler none(lo, lo, 16.0f);
    CHECK(none.done());
    CHECK(none.step(floor, [] { return true; }));
    CHECK(none.take().empty());
}
