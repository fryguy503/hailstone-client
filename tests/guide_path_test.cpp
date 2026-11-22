// The Find window's guide path (app/guide_path.h) and the trail paths are drawn with (game/path_trail.h).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "app/guide_path.h"
#include "game/collision.h"
#include "game/path_trail.h"

using namespace hailstone;

namespace {

std::optional<float> flat_floor(const glm::vec3& p) { return p.z >= 0.0f ? std::optional<float>(0.0f) : std::nullopt; }

void ramp(std::vector<game::CollisionTriangle>& triangles, float x0, float x1, float z0, float z1,
          float y0 = -5.0f, float y1 = 5.0f, bool upward = true) {
    const glm::vec3 a(x0, y0, z0), b(x0, y1, z0), c(x1, y1, z1), d(x1, y0, z1);
    triangles.push_back(upward ? game::CollisionTriangle::make(a, b, c, 0) : game::CollisionTriangle::make(a, c, b, 0));
    triangles.push_back(upward ? game::CollisionTriangle::make(a, c, d, 0) : game::CollisionTriangle::make(a, d, c, 0));
}

} // namespace

TEST_CASE("Path trail: evenly spaced along the line, on the floor, faded near the player and far along") {
    const std::vector<glm::vec3> line{{0, 0, 0}, {10, 0, 0}, {10, 30, 0}};
    game::TrailStyle st;
    st.rainbow = false;
    st.spacing = 1.0f;
    st.near_start = 1.0f;
    st.near_end = 4.0f;
    st.far_start = 1000.0f;
    st.far_end = 2000.0f;
    st.beacon = false;
    st.lift = 0.5f;
    // the floor rises along y: a hill between the two corners
    auto floor = [](const glm::vec3& p) -> std::optional<float> { return p.y * 0.1f; };
    auto t = game::build_trail(line, st, 0.0, floor);
    // 1..40 along: 40 samples (the first faded out entirely is dropped)
    REQUIRE(t.size() >= 38);
    REQUIRE(t.size() <= 40);
    for (std::size_t i = 1; i < t.size(); ++i) {
        float d = glm::length(glm::vec2(t[i].position) - glm::vec2(t[i - 1].position));
        CHECK(d <= 1.0f + 1e-3f); // a corner cuts the step short, nothing is further apart
        CHECK(d >= 0.70f);
    }
    // on the floor, lifted
    const auto& last = t.back();
    CHECK(last.position.z == doctest::Approx(last.position.y * 0.1f + 0.5f));
    // dim at the player, bright further on
    CHECK(t.front().alpha < t[10].alpha + 0.5f);
    CHECK(t.front().alpha < 0.5f * st.alpha * (1.0f + st.band_boost));

    // An unsupported route is hidden, rather than floating above a gorge.
    auto deep = [](const glm::vec3&) -> std::optional<float> { return -50.0f; };
    auto b = game::build_trail(line, st, 0.0, deep);
    CHECK(b.empty());
}

TEST_CASE("Path trail: the far end fades it out, the bands move with time, and the beacon stands at the end") {
    std::vector<glm::vec3> line{{0, 0, 0}, {500, 0, 0}};
    game::TrailStyle st;
    st.rainbow = false;
    st.far_start = 50.0f;
    st.far_end = 60.0f;
    st.beacon = false;
    auto t = game::build_trail(line, st, 0.0, flat_floor);
    for (const auto& s : t) CHECK(s.position.x <= 60.0f + 1e-3f);

    // the bands: a band's peak moves towards the destination as time goes on (at 9 a second, 10 apart: the one at
    // 20 at time 0 is at 20.9 a tenth of a second later)
    auto brightest = [&](double time) {
        game::TrailStyle dense = st;
        dense.spacing = 0.1f;
        auto v = game::build_trail(line, dense, time, flat_floor);
        float x = 0.0f, best = -1.0f;
        for (const auto& s : v)
            if (s.position.x > 15.0f && s.position.x < 25.0f && s.size > best) { // (alpha is clamped at a band)
                best = s.size;
                x = s.position.x;
            }
        return x;
    };
    CHECK(brightest(0.0) == doctest::Approx(20.0f).epsilon(0.01));
    CHECK(brightest(0.1) == doctest::Approx(20.9f).epsilon(0.01));

    st.beacon = true;
    auto with = game::build_trail(line, st, 0.0, flat_floor);
    CHECK(with.size() == t.size() + 7);
    CHECK(with.back().position.x == doctest::Approx(500.0f));
    CHECK(with.back().position.z > 1.0f); // the column rises

    CHECK(game::build_trail(std::vector<glm::vec3>{{0, 0, 0}}, st, 0.0).empty());
    // The budget includes the beacon.
    st.far_end = 1000.0f;
    st.far_start = 900.0f;
    CHECK(game::build_trail(line, st, 0.0, flat_floor, 20).size() == 20);
}

TEST_CASE("Path trail: seven distinct rainbow strands sway sideways while staying on the ground") {
    const std::vector<glm::vec3> line{{0, 0, 0}, {60, 0, 0}};
    game::TrailStyle st;
    st.beacon = false;
    const auto a = game::build_trail(line, st, 0.0, flat_floor);
    const auto b = game::build_trail(line, st, 0.25, flat_floor);
    REQUIRE(a.size() > 700);
    REQUIRE(a.size() == b.size());
    std::vector<glm::vec3> colors;
    bool moved = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::find(colors.begin(), colors.end(), a[i].color) == colors.end()) colors.push_back(a[i].color);
        CHECK(a[i].position.z == doctest::Approx(st.lift));
        CHECK(b[i].position.z == doctest::Approx(st.lift));
        CHECK(std::abs(a[i].position.y) <= 3.0f * st.strand_spacing + st.sway + 0.0001f);
        CHECK(a[i].color == b[i].color);
        moved = moved || std::abs(a[i].position.y - b[i].position.y) > 0.001f;
    }
    CHECK(colors.size() == 7);
    CHECK(moved);
    st.rainbow = false;
    st.color = {0.2f, 0.6f, 0.9f};
    const auto solid = game::build_trail(line, st, 0.0, flat_floor);
    REQUIRE_FALSE(solid.empty());
    CHECK(a.size() == solid.size() * 7);
    for (const auto& s : solid) CHECK(s.color == st.color);
}

TEST_CASE("Path trail: terrain curvature follows a hill or valley far from the endpoint chord") {
    for (const float peak : {16.0f, -16.0f}) {
        CAPTURE(peak);
        std::vector<game::CollisionTriangle> triangles;
        ramp(triangles, 0, 20, 0, peak);
        ramp(triangles, 20, 40, peak, 0);
        const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
        const std::vector<glm::vec3> line{{0, 0, 0}, {40, 0, 0}};
        game::TrailStyle st;
        st.beacon = false;
        const auto trail = game::build_trail(line, st, 0.0, [&](const glm::vec3& p) { return mesh.ground_below(p); });
        REQUIRE(trail.size() > 450);
        bool reached_peak = false;
        for (const auto& s : trail) {
            const float want = peak * (1.0f - std::abs(s.position.x - 20.0f) / 20.0f);
            CHECK(s.position.z == doctest::Approx(want + st.lift).epsilon(0.001));
            reached_peak = reached_peak || std::abs(s.position.z - st.lift) > 15.0f;
        }
        CHECK(reached_peak);
    }
}

TEST_CASE("Path trail: bridges keep their deck and tunnels stay below the roof") {
    std::vector<game::CollisionTriangle> triangles;
    ramp(triangles, 0, 40, -40, -40);
    ramp(triangles, 0, 40, 0, 0);
    ramp(triangles, 0, 40, 2, 2, -5, 5, false); // a roof's underside cannot support a trail
    ramp(triangles, 0, 40, 10, 10);
    const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
    auto floor = [&](const glm::vec3& p) { return mesh.ground_below(p); };
    game::TrailStyle st;
    st.beacon = false;
    for (const float level : {0.0f, 10.0f}) {
        CAPTURE(level);
        const std::vector<glm::vec3> line{{0, 0, level}, {40, 0, level}};
        const auto trail = game::build_trail(line, st, 0.0, floor);
        REQUIRE(trail.size() > 450);
        for (const auto& s : trail) CHECK(s.position.z == doctest::Approx(level + st.lift));
    }
}

TEST_CASE("Path trail: missing support hides the segment and only a supported route vertex restarts it") {
    std::vector<game::CollisionTriangle> triangles;
    ramp(triangles, 0, 40, -40, -40); // ground far below a missing section of deck
    ramp(triangles, 0, 10, 0, 0);
    ramp(triangles, 20, 40, 0, 0);
    const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
    auto floor = [&](const glm::vec3& p) { return mesh.ground_below(p); };
    const std::vector<glm::vec3> line{{0, 0, 0}, {30, 0, 0}, {40, 0, 0}};
    game::TrailStyle st;
    st.beacon = false;
    const auto trail = game::build_trail(line, st, 0.0, floor);
    bool before = false, after = false;
    REQUIRE_FALSE(trail.empty());
    for (const auto& s : trail) {
        CHECK((s.position.x <= 10.0f || s.position.x >= 30.0f));
        CHECK(s.position.z == doctest::Approx(st.lift));
        before = before || s.position.x < 10.0f;
        after = after || s.position.x > 30.0f;
    }
    CHECK(before);
    CHECK(after);
    CHECK(game::build_trail(line, st, 0.0).empty());
    st.beacon = true;
    CHECK(game::build_trail(line, st, 0.0, [](const glm::vec3&) -> std::optional<float> { return std::nullopt; }).empty());
    const std::vector<glm::vec3> over_gap{{0, 0, 0}, {15, 0, 0}};
    const auto no_finish = game::build_trail(over_gap, st, 0.0, floor);
    for (const auto& s : no_finish) CHECK(s.position.x <= 10.0f); // no airborne beacon at the goal
}

TEST_CASE("Path trail: strands at the edge of a narrow ledge need their own ground") {
    std::vector<game::CollisionTriangle> triangles;
    ramp(triangles, 0, 40, -20, -20);
    ramp(triangles, 0, 40, 0, 0, -0.5f, 0.5f);
    const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
    const std::vector<glm::vec3> line{{0, 0, 0}, {40, 0, 0}};
    game::TrailStyle st;
    st.beacon = false;
    const auto trail = game::build_trail(line, st, 0.0, [&](const glm::vec3& p) { return mesh.ground_below(p); });
    REQUIRE(trail.size() > 100);
    CHECK(trail.size() < game::build_trail(line, st, 0.0, flat_floor).size() / 2);
    for (const auto& s : trail) {
        CHECK(std::abs(s.position.y) <= 0.5f + 0.0001f);
        CHECK(s.position.z == doctest::Approx(st.lift));
    }
}

TEST_CASE("Path trail: sprite and probe budgets are bounded, invalid and vertical links cannot float") {
    game::TrailStyle st;
    const std::vector<glm::vec3> line{{0, 0, 0}, {100, 0, 0}};
    for (const std::size_t budget : {std::size_t{0}, std::size_t{1}, std::size_t{23}, std::size_t{3200}})
        CHECK(game::build_trail(line, st, 0.0, flat_floor, budget).size() <= budget);
    st.far_start = 1e8f;
    st.far_end = 2e8f;
    std::size_t probes = 0;
    const std::vector<glm::vec3> huge{{0, 0, 0}, {1e8f, 0, 0}};
    const auto hidden = game::build_trail(huge, st, 0.0, [&](const glm::vec3&) -> std::optional<float> {
        ++probes;
        return std::nullopt;
    }, 20);
    CHECK(hidden.empty());
    CHECK(probes < 100);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(game::build_trail(std::vector<glm::vec3>{{0, 0, 0}, {nan, 0, 0}}, st, 0.0, flat_floor).empty());
    std::vector<game::CollisionTriangle> triangles;
    ramp(triangles, 0, 30, 0, 0);
    ramp(triangles, 0, 30, 20, 20);
    const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
    const std::vector<glm::vec3> vertical{{0, 0, 0}, {0, 0, 0}, {10, 0, 0}, {10, 0, 20}, {30, 0, 20}};
    st.beacon = false;
    const auto trail = game::build_trail(vertical, st, 0.0, [&](const glm::vec3& p) { return mesh.ground_below(p); });
    REQUIRE_FALSE(trail.empty());
    bool upper = false;
    for (const auto& s : trail) {
        CHECK(std::isfinite(s.position.x));
        CHECK(std::isfinite(s.position.y));
        CHECK((std::abs(s.position.z - st.lift) < 0.001f || std::abs(s.position.z - 20.0f - st.lift) < 0.001f));
        upper = upper || s.position.z > 20.0f;
    }
    CHECK(upper);
}

TEST_CASE("Find trail styles: navigation is the default and invalid choices fall back to it") {
    CHECK(game::normalize_find_trail_style(0) == game::FindTrailStyle::Navigation);
    CHECK(game::normalize_find_trail_style(1) == game::FindTrailStyle::Rainbow);
    CHECK(game::normalize_find_trail_style(-1) == game::FindTrailStyle::Navigation);
    CHECK(game::normalize_find_trail_style(2) == game::FindTrailStyle::Navigation);
    CHECK(game::normalize_find_trail_style(999) == game::FindTrailStyle::Navigation);
}

TEST_CASE("Navigation trail: a gold strip and dark border lie on the local ground plane") {
    auto elevation = [](float x, float y) { return x * 0.4f + y * 0.25f; };
    const glm::vec3 a(0, -5, elevation(0, -5)), b(0, 5, elevation(0, 5));
    const glm::vec3 c(40, 5, elevation(40, 5)), d(40, -5, elevation(40, -5));
    const auto mesh = game::CollisionMesh::from_triangles({game::CollisionTriangle::make(a, b, c, 0),
                                                         game::CollisionTriangle::make(a, c, d, 0)});
    const std::vector<glm::vec3> line{{0, 0, 0}, {40, 0, 16}};
    game::TrailStyle st;
    const auto pieces = game::build_navigation_trail(line, st, 0.0, [&](const glm::vec3& p) { return mesh.ground_below(p); });
    REQUIRE(pieces.size() > 100);
    REQUIRE(pieces.size() % 2 == 0);
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const auto& p = pieces[i];
        CAPTURE(i);
        CHECK(glm::length(p.across) == doctest::Approx(1.0f));
        CHECK(glm::length(p.forward) == doctest::Approx(1.0f));
        CHECK(glm::dot(p.across, p.forward) == doctest::Approx(0.0f).epsilon(0.0001));
        CHECK(p.length > 0.0f);
        CHECK(p.alpha > 0.0f);
        const float extra = i % 2 == 0 ? 0.0f : 0.02f;
        for (const float end : {-0.5f, 0.5f})
            for (const float side : {-0.5f, 0.5f}) {
                const auto corner = p.position + p.forward * (p.length * end) + p.across * (p.width * side);
                CHECK(corner.z == doctest::Approx(elevation(corner.x, corner.y) + st.lift + extra).epsilon(0.001));
            }
        if (i % 2 == 0) {
            CHECK(p.width == doctest::Approx(0.85f));
            CHECK(p.color.r < 0.1f);
        } else {
            CHECK(p.width == doctest::Approx(0.5f));
            CHECK(p.color.r > 0.9f);
            CHECK(p.color.g > 0.7f);
            CHECK(p.position.z == doctest::Approx(pieces[i - 1].position.z + 0.02f));
        }
    }
}

TEST_CASE("Navigation trail: shared terrain sampling follows curvature without joining unsupported gaps") {
    game::TrailStyle st;
    for (const float peak : {16.0f, -16.0f}) {
        std::vector<game::CollisionTriangle> triangles;
        ramp(triangles, 0, 20, 0, peak);
        ramp(triangles, 20, 40, peak, 0);
        const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
        const std::vector<glm::vec3> line{{0, 0, 0}, {40, 0, 0}};
        const auto pieces = game::build_navigation_trail(line, st, 0.0, [&](const glm::vec3& p) { return mesh.ground_below(p); });
        REQUIRE(pieces.size() > 100);
        bool curved = false;
        for (const auto& p : pieces) curved = curved || std::abs(p.position.z - st.lift) > 15.0f;
        CHECK(curved);
    }
    std::vector<game::CollisionTriangle> triangles;
    ramp(triangles, 0, 40, -40, -40);
    ramp(triangles, 0, 10, 0, 0);
    ramp(triangles, 20, 40, 0, 0);
    const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
    const std::vector<glm::vec3> line{{0, 0, 0}, {30, 0, 0}, {40, 0, 0}};
    const auto pieces = game::build_navigation_trail(line, st, 0.0, [&](const glm::vec3& p) { return mesh.ground_below(p); });
    REQUIRE_FALSE(pieces.empty());
    bool resumed = false;
    for (const auto& p : pieces) {
        const float start = p.position.x - p.forward.x * p.length * 0.5f;
        const float finish = p.position.x + p.forward.x * p.length * 0.5f;
        CHECK((finish <= 10.001f || start >= 29.999f));
        resumed = resumed || start > 30.0f;
    }
    CHECK(resumed);
    CHECK(game::build_navigation_trail(line, st, 0.0).empty());
    for (const std::size_t budget : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{100}})
        CHECK(game::build_navigation_trail(line, st, 0.0, flat_floor, budget).size() <= budget);
}

TEST_CASE("Navigation trail: discrete stairs do not become diagonal airborne strips") {
    std::vector<game::CollisionTriangle> triangles;
    ramp(triangles, 0, 10, 0, 0);
    ramp(triangles, 10, 40, 2, 2);
    const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
    const std::vector<glm::vec3> line{{0, 0, 0}, {40, 0, 2}};
    game::TrailStyle st;
    const auto pieces = game::build_navigation_trail(line, st, 0.0, [&](const glm::vec3& p) { return mesh.ground_below(p); });
    REQUIRE(pieces.size() > 100);
    bool lower = false, upper = false;
    for (const auto& p : pieces) {
        const glm::vec3 start = p.position - p.forward * (p.length * 0.5f);
        const glm::vec3 finish = p.position + p.forward * (p.length * 0.5f);
        CHECK_FALSE((start.x < 10.0f && finish.x >= 10.0f));
        CHECK(std::abs(p.forward.z) < 0.001f);
        lower = lower || p.position.x < 9.0f;
        upper = upper || p.position.x > 11.0f;
    }
    CHECK(lower);
    CHECK(upper);
}

TEST_CASE("Rainbow strands: thin connected threads hover and weave above their local ground") {
    const std::vector<glm::vec3> line{{0, 0, 0}, {60, 0, 0}};
    game::TrailStyle st;
    std::size_t queries = 0;
    const auto a = game::build_rainbow_strands(line, st, 0.0, [&](const glm::vec3& p) {
        ++queries;
        return flat_floor(p);
    });
    const auto b = game::build_rainbow_strands(line, st, 0.3, flat_floor);
    REQUIRE(a.size() > 1800);
    REQUIRE(a.size() % 14 == 0);
    REQUIRE(b.size() == a.size());
    CHECK(queries < 5000); // shared centre samples and cached endpoints, not fourteen copies of the ground walk
    std::vector<glm::vec3> colors;
    bool moving = false, copies_differ = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto& p = a[i];
        if (std::find(colors.begin(), colors.end(), p.color) == colors.end()) colors.push_back(p.color);
        CHECK(p.width == doctest::Approx(0.06f));
        CHECK(p.length > p.width);
        CHECK(glm::length(p.forward) == doctest::Approx(1.0f));
        CHECK(glm::dot(p.forward, p.across) == doctest::Approx(0.0f).epsilon(0.0001));
        CHECK(p.color == b[i].color);
        for (const float end : {-0.5f, 0.5f}) {
            const glm::vec3 endpoint = p.position + p.forward * (p.length * end);
            CHECK(endpoint.z >= 2.3f - 0.001f);
            CHECK(endpoint.z <= 3.7f + 0.001f);
            CHECK(std::abs(endpoint.y) <= 0.8f + 0.001f);
        }
        if (i >= 14) {
            const glm::vec3 previous_end = a[i - 14].position + a[i - 14].forward * (a[i - 14].length * 0.5f);
            const glm::vec3 next_start = p.position - p.forward * (p.length * 0.5f);
            CHECK(glm::distance(previous_end, next_start) < 0.0001f);
        }
        moving = moving || glm::distance(p.position, b[i].position) > 0.01f;
        if (i < 7) {
            CHECK(p.color == a[i + 7].color);
            copies_differ = copies_differ || glm::distance(p.position, a[i + 7].position) > 0.1f;
        }
    }
    CHECK(colors.size() == 7);
    CHECK(moving);
    CHECK(copies_differ);
}

TEST_CASE("Rainbow strands: hills valleys bridges and tunnels retain their hovering height") {
    game::TrailStyle st;
    for (const float peak : {16.0f, -16.0f}) {
        CAPTURE(peak);
        std::vector<game::CollisionTriangle> triangles;
        ramp(triangles, 0, 20, 0, peak);
        ramp(triangles, 20, 40, peak, 0);
        const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
        const std::vector<glm::vec3> line{{0, 0, 0}, {40, 0, 0}};
        const auto pieces = game::build_rainbow_strands(line, st, 0.0, [&](const glm::vec3& p) { return mesh.ground_below(p); });
        REQUIRE(pieces.size() > 1000);
        bool curved = false;
        for (const auto& p : pieces)
            for (const float end : {-0.5f, 0.5f}) {
                const glm::vec3 endpoint = p.position + p.forward * (p.length * end);
                const float ground = peak * (1.0f - std::abs(endpoint.x - 20.0f) / 20.0f);
                CHECK(endpoint.z - ground >= 2.3f - 0.001f);
                CHECK(endpoint.z - ground <= 3.7f + 0.001f);
                curved = curved || std::abs(endpoint.z - 3.0f) > 14.0f;
            }
        CHECK(curved);
    }
    std::vector<game::CollisionTriangle> triangles;
    ramp(triangles, 0, 40, -40, -40);
    ramp(triangles, 0, 40, 0, 0);
    ramp(triangles, 0, 40, 2, 2, -5, 5, false);
    ramp(triangles, 0, 40, 10, 10);
    const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
    for (const float level : {0.0f, 10.0f}) {
        const std::vector<glm::vec3> line{{0, 0, level}, {40, 0, level}};
        const auto pieces = game::build_rainbow_strands(line, st, 0.0, [&](const glm::vec3& p) { return mesh.ground_below(p); });
        REQUIRE(pieces.size() > 1000);
        for (const auto& p : pieces) {
            CHECK(p.position.z - level >= 2.3f - 0.001f);
            CHECK(p.position.z - level <= 3.7f + 0.001f);
        }
    }
}

TEST_CASE("Rainbow strands: ground gaps and ledge edges break the affected threads") {
    std::vector<game::CollisionTriangle> triangles;
    ramp(triangles, 0, 40, -40, -40);
    ramp(triangles, 0, 10, 0, 0);
    ramp(triangles, 20, 40, 0, 0);
    const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
    const std::vector<glm::vec3> line{{0, 0, 0}, {30, 0, 0}, {40, 0, 0}};
    game::TrailStyle st;
    const auto pieces = game::build_rainbow_strands(line, st, 0.0, [&](const glm::vec3& p) { return mesh.ground_below(p); });
    REQUIRE_FALSE(pieces.empty());
    bool before = false, after = false;
    for (const auto& p : pieces) {
        const glm::vec3 start = p.position - p.forward * (p.length * 0.5f);
        const glm::vec3 finish = p.position + p.forward * (p.length * 0.5f);
        CHECK((finish.x <= 10.001f || start.x >= 29.999f));
        before = before || p.position.x < 10.0f;
        after = after || p.position.x > 30.0f;
    }
    CHECK(before);
    CHECK(after);
    triangles.clear();
    ramp(triangles, 0, 40, -40, -40);
    ramp(triangles, 0, 40, 0, 0, -0.3f, 0.3f);
    const auto ledge = game::CollisionMesh::from_triangles(std::move(triangles));
    const auto narrow = game::build_rainbow_strands(line, st, 0.0, [&](const glm::vec3& p) { return ledge.ground_below(p); });
    REQUIRE(narrow.size() > 100);
    CHECK(narrow.size() < game::build_rainbow_strands(line, st, 0.0, flat_floor).size() * 3 / 4);
    for (const auto& p : narrow) {
        CHECK(p.length < 1.0f);
        for (const float end : {-0.5f, 0.5f}) {
            const glm::vec3 endpoint = p.position + p.forward * (p.length * end);
            CHECK(std::abs(endpoint.y) <= 0.3f + 0.001f);
            CHECK(endpoint.z >= 2.3f - 0.001f);
        }
    }
}

TEST_CASE("Rainbow strands: budgets invalid input and absent ground remain bounded") {
    const std::vector<glm::vec3> line{{0, 0, 0}, {500, 0, 0}};
    game::TrailStyle st;
    for (const std::size_t budget : {std::size_t{0}, std::size_t{1}, std::size_t{13}, std::size_t{14}, std::size_t{40}, std::size_t{7000}}) {
        const auto pieces = game::build_rainbow_strands(line, st, 0.0, flat_floor, budget);
        CHECK(pieces.size() <= budget);
        if (budget <= 40) CHECK(pieces.size() == budget);
        for (const auto& p : pieces) CHECK(p.position.x <= st.far_end);
    }
    CHECK(game::build_rainbow_strands(line, st, 0.0).empty());
    CHECK(game::build_rainbow_strands(line, st, 0.0, [](const glm::vec3&) -> std::optional<float> { return std::nullopt; }).empty());
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(game::build_rainbow_strands(std::vector<glm::vec3>{{0, 0, 0}, {nan, 0, 0}}, st, 0.0, flat_floor).empty());
    CHECK(game::build_rainbow_strands(line, st, std::numeric_limits<double>::infinity(), flat_floor).empty());
}

TEST_CASE("Find trails: feet or nav corners slightly inside the terrain can anchor both world styles") {
    std::vector<game::CollisionTriangle> triangles;
    ramp(triangles, 0, 40, 0, 0);
    const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
    const auto floor = [&](const glm::vec3& p) { return mesh.ground_below(p); };
    game::TrailStyle st;
    for (const float depth : {0.6f, 1.0f, 1.44f, 2.5f})
        for (const float end_depth : {0.0f, depth}) {
            CAPTURE(depth);
            CAPTURE(end_depth);
            const std::vector<glm::vec3> line{{0, 0, -depth}, {40, 0, -end_depth}};
            const auto nav = game::build_navigation_trail(line, st, 0.0, floor);
            const auto rainbow = game::build_rainbow_strands(line, st, 0.0, floor);
            REQUIRE(nav.size() > 100);
            REQUIRE(rainbow.size() > 1000);
            for (std::size_t i = 0; i < nav.size(); ++i)
                CHECK(nav[i].position.z == doctest::Approx(st.lift + (i % 2 ? 0.02f : 0.0f)));
            for (const auto& piece : rainbow) {
                CHECK(piece.position.z >= 2.3f - 0.001f);
                CHECK(piece.position.z <= 3.7f + 0.001f);
            }
        }
    const std::vector<glm::vec3> too_deep{{0, 0, -3.5f}, {40, 0, -3.5f}};
    CHECK(game::build_navigation_trail(too_deep, st, 0.0, floor).empty());
    CHECK(game::build_rainbow_strands(too_deep, st, 0.0, floor).empty());
}

TEST_CASE("Find trails: the first supported lower floor wins over an overhead deck and absent ground stays hidden") {
    std::vector<game::CollisionTriangle> triangles;
    ramp(triangles, 0, 40, 0, 0);
    ramp(triangles, 0, 40, 3.1f, 3.1f);
    const auto mesh = game::CollisionMesh::from_triangles(std::move(triangles));
    const auto floor = [&](const glm::vec3& p) { return mesh.ground_below(p); };
    // The existing +0.5 probe sees the lower floor. Casting from +3 immediately would instead see the deck.
    const std::vector<glm::vec3> line{{0, 0, 2}, {40, 0, 2}};
    REQUIRE(mesh.ground_below({10, 0, 5}).value() == doctest::Approx(3.1f));
    game::TrailStyle st;
    const auto nav = game::build_navigation_trail(line, st, 0.0, floor);
    const auto rainbow = game::build_rainbow_strands(line, st, 0.0, floor);
    REQUIRE(nav.size() > 100);
    REQUIRE(rainbow.size() > 1000);
    for (const auto& piece : nav) CHECK(piece.position.z < 0.4f);
    for (const auto& piece : rainbow) CHECK(piece.position.z <= 3.7f + 0.001f);

    std::vector<game::CollisionTriangle> gorge_triangles;
    ramp(gorge_triangles, 0, 40, -40, -40);
    const auto gorge = game::CollisionMesh::from_triangles(std::move(gorge_triangles));
    const auto low_floor = [&](const glm::vec3& p) { return gorge.ground_below(p); };
    CHECK(game::build_navigation_trail(line, st, 0.0, low_floor).empty());
    CHECK(game::build_rainbow_strands(line, st, 0.0, low_floor).empty());
    const auto no_floor = [](const glm::vec3&) -> std::optional<float> { return std::nullopt; };
    CHECK(game::build_navigation_trail(line, st, 0.0, no_floor).empty());
    CHECK(game::build_rainbow_strands(line, st, 0.0, no_floor).empty());
}

TEST_CASE("Guide path: found over the mesh, trimmed behind the player, found again off it, ends on arrival") {
    app::GuidePath g;
    int searches = 0;
    std::optional<std::vector<glm::vec3>> answer;
    auto path = [&](const glm::vec3& from, const glm::vec3& to) -> std::optional<std::vector<glm::vec3>> {
        ++searches;
        if (!answer) return std::nullopt;
        std::vector<glm::vec3> p{from, glm::vec3(50, 0, 0), to}; // round a corner at (50, 0)
        return p;
    };
    const glm::vec3 npc(50, 60, 0);
    g.start(7, "Banker Bob");
    REQUIRE(g.active());

    // the mesh not ready: said once, tried again after a second
    auto u = g.update({0, 0, 0}, npc, 0.0, path);
    CHECK(u.waiting_mesh);
    CHECK(g.line().empty());
    u = g.update({0, 0, 0}, npc, 0.5, path);
    CHECK_FALSE(u.waiting_mesh);
    CHECK(searches == 1);

    answer = std::vector<glm::vec3>{};
    u = g.update({0, 0, 0}, npc, 1.0, path);
    CHECK(searches == 2);
    REQUIRE(g.line().size() == 3);
    CHECK(g.line().front() == glm::vec3(0, 0, 0));
    CHECK(g.line().back() == npc);
    CHECK_FALSE(g.partial());

    // walking it: no new search, the line starts at the player, the corner passed is dropped
    u = g.update({30, 1, 0}, npc, 1.2, path);
    CHECK(searches == 2);
    CHECK(g.line().front() == glm::vec3(30, 1, 0));
    CHECK(g.line().size() == 3);
    u = g.update({50, 20, 0}, npc, 1.4, path);
    CHECK(searches == 2);
    CHECK(g.line().size() == 2);

    // off the line: found again (not sooner than kReplanGap after the last search)
    u = g.update({80, 20, 0}, npc, 1.45, path);
    CHECK(searches == 2);
    u = g.update({80, 20, 0}, npc, 2.0, path);
    CHECK(searches == 3);

    // the NPC moved: found again
    u = g.update({80, 20, 0}, glm::vec3(50, 80, 0), 3.0, path);
    CHECK(searches == 4);

    // arrived
    u = g.update({52, 75, 0}, glm::vec3(50, 80, 0), 3.2, path);
    CHECK(u.arrived);
    CHECK_FALSE(g.active());
    CHECK(g.line().empty());
}

TEST_CASE("Guide path: ends when the NPC is gone or the mesh has no way there") {
    app::GuidePath g;
    auto none = [](const glm::vec3&, const glm::vec3&) -> std::optional<std::vector<glm::vec3>> {
        return std::vector<glm::vec3>{};
    };
    g.start(7, "x");
    auto u = g.update({0, 0, 0}, glm::vec3(100, 0, 0), 0.0, none);
    CHECK(u.no_way);
    CHECK_FALSE(g.active());

    g.start(7, "x");
    u = g.update({0, 0, 0}, std::nullopt, 0.0, none);
    CHECK(u.gone);
    CHECK_FALSE(g.active());

    // a partial path: drawn as far as it goes
    auto part = [](const glm::vec3& from, const glm::vec3&) -> std::optional<std::vector<glm::vec3>> {
        return std::vector<glm::vec3>{from, glm::vec3(40, 0, 0)};
    };
    g.start(7, "x");
    u = g.update({0, 0, 0}, glm::vec3(100, 0, 0), 0.0, part);
    CHECK(g.active());
    CHECK(g.partial());
    CHECK(g.line().size() == 2);
}

TEST_CASE("Guide path: an explicit actor gets a live direction line until a route is available") {
    app::GuidePath guide;
    const glm::vec3 origin(0, 0, 0), nearby(9, 0, 0), other_floor(10, 0, 25);
    std::optional<std::vector<glm::vec3>> route;
    const app::GuidePath::PathFn plan = [&](const glm::vec3&, const glm::vec3&) { return route; };
    guide.start(7, "Target", {}, app::GuidePath::Mode::Actor);
    CHECK_FALSE(guide.update(origin, nearby, 0.0, plan).arrived);
    CHECK(guide.direction_only());
    REQUIRE(guide.line().size() == 2);
    CHECK(guide.line().back() == nearby);

    guide.update(origin, other_floor, 0.5, plan);
    CHECK(guide.line().back() == other_floor);
    route = std::vector<glm::vec3>{origin, glm::vec3(10, 0, 0)}; // reaches its XY on the wrong floor
    guide.update(origin, other_floor, 1.0, plan);
    CHECK(guide.direction_only());
    CHECK(guide.line().back() == other_floor);

    route = std::vector<glm::vec3>{origin, glm::vec3(0, 12, 5), other_floor};
    guide.update(origin, other_floor, 2.0, plan);
    CHECK_FALSE(guide.direction_only());
    REQUIRE(guide.line().size() == 3);
    CHECK(guide.line().back() == other_floor);
    CHECK(guide.update(origin, std::nullopt, 2.2, plan).gone);
    CHECK(guide.line().empty());
}
