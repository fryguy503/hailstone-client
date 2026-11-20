// Hailstone's overlap fade: projected positions do not enter the temporal state.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "game/nameplate_overlap.h"

using namespace hailstone::game;

namespace {
PlateBox box(float x, float y, float depth, bool front = false) { return {x, y, x + 100.0f, y + 20.0f, depth, front}; }
} // namespace

TEST_CASE("plate overlap: far names first, target last, equal depths stable") {
    const std::vector<PlateBox> b = {box(0, 0, 50), box(0, 0, 200), box(0, 0, 300, true), box(0, 0, 50)};
    CHECK(plate_draw_order(b) == std::vector<std::size_t>{1, 0, 3, 2});
}

TEST_CASE("plate overlap: empty and separate boxes remain visible") {
    CHECK(plate_draw_order({}).empty());
    CHECK(plate_target_alpha({}).empty());
    const std::vector<PlateBox> b = {box(0, 0, 100), box(300, 0, 20), box(0, 100, 5)};
    CHECK(plate_target_alpha(b) == std::vector<float>{1, 1, 1});
}

TEST_CASE("plate overlap: ten to twenty percent coverage fades to ten percent alpha") {
    std::vector<PlateBox> b = {box(0, 0, 100), box(95, 0, 20)};
    CHECK(plate_target_alpha(b)[0] == 1.0f);
    b[1].x0 = 90;
    CHECK(plate_target_alpha(b)[0] == doctest::Approx(1.0f));
    b[1].x0 = 85;
    CHECK(plate_target_alpha(b)[0] == doctest::Approx(0.55f));
    b[1].x0 = 80;
    CHECK(plate_target_alpha(b)[0] == doctest::Approx(0.1f));
    b[1] = box(0, 0, 20);
    const auto a = plate_target_alpha(b);
    CHECK(a[0] == doctest::Approx(0.1f));
    CHECK(a[1] == 1.0f);
}

TEST_CASE("plate overlap: target is never faded even behind the nearer name") {
    const std::vector<PlateBox> b = {box(0, 0, 10), box(0, 0, 300, true)};
    const auto a = plate_target_alpha(b);
    CHECK(a[0] == doctest::Approx(0.1f));
    CHECK(a[1] == 1.0f);
}

TEST_CASE("plate overlap: equal depths and degenerate boxes do not fade") {
    std::vector<PlateBox> b = {box(0, 0, 10), box(0, 0, 10)};
    CHECK(plate_target_alpha(b) == std::vector<float>{1, 1});
    b[0] = {0, 0, 0, 0, 100, false};
    CHECK(plate_target_alpha(b) == std::vector<float>{1, 1});
}

TEST_CASE("plate overlap: coverage is the largest single nearer box, input positions unchanged") {
    const std::vector<PlateBox> b = {box(0, 0, 100), box(-95, 0, 20), box(95, 0, 10)};
    CHECK(plate_target_alpha(b)[0] == 1.0f);
    CHECK(b[0].x0 == 0);
    CHECK(b[0].y0 == 0);
    CHECK(b[1].y0 == 0);
}

TEST_CASE("plate overlap: fading is smooth and independent of frame subdivision") {
    PlateFade a, b;
    CHECK(a.step(7, 0.1f, 0) == doctest::Approx(0.1f));
    b.step(7, 0.1f, 0);
    CHECK(a.step(7, 1.0f, 0) == doctest::Approx(0.1f));
    CHECK(a.step(7, 1.0f, -1.0f) == doctest::Approx(0.1f));
    const float once = a.step(7, 1.0f, 0.3f);
    CHECK(once > 0.1f);
    CHECK(once < 1.0f);
    b.step(7, 1.0f, 0.1f);
    b.step(7, 1.0f, 0.1f);
    CHECK(b.step(7, 1.0f, 0.1f) == doctest::Approx(once));
}

TEST_CASE("plate overlap: selecting a faded name restores full opacity immediately and stores it") {
    PlateFade f;
    CHECK(f.step(7, 0.1f, 0) == doctest::Approx(0.1f));
    CHECK(f.step(7, 0.1f, 0, true) == 1.0f);
    CHECK(f.step(7, 1.0f, 0) == 1.0f);
    CHECK(f.step(7, 0.1f, 0.016f) > 0.9f);
}

TEST_CASE("plate overlap: absent names and a cleared scene forget old opacity") {
    PlateFade f;
    f.step(7, 0.1f, 0);
    f.end_frame();
    f.end_frame();
    CHECK(f.step(7, 1.0f, 0) == 1.0f);
    f.clear();
    CHECK(f.step(7, 0.1f, 0) == doctest::Approx(0.1f));
}

TEST_CASE("plate overlap: no distance fade for isolated names") {
    const std::vector<PlateBox> b = {box(0, 0, 10), box(300, 0, 250)};
    CHECK(plate_target_alpha(b) == std::vector<float>{1, 1});
}

TEST_CASE("plate overlap: translating or scaling screen boxes preserves the coverage fade") {
    std::uint32_t rng = 7;
    auto number = [&] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 24);
    };
    for (int run = 0; run < 50; ++run) {
        std::vector<PlateBox> boxes;
        for (int i = 0; i < 20; ++i) {
            const float x = number(), y = number(), depth = number() + 1.0f;
            boxes.push_back(box(x, y, depth, i == 0));
        }
        auto transformed = boxes;
        for (auto& b : transformed) {
            b.x0 = b.x0 * 2.0f - 100.0f;
            b.x1 = b.x1 * 2.0f - 100.0f;
            b.y0 = b.y0 * 2.0f + 40.0f;
            b.y1 = b.y1 * 2.0f + 40.0f;
            b.depth *= 10.0f;
        }
        const auto alpha = plate_target_alpha(boxes), scaled = plate_target_alpha(transformed);
        CHECK(plate_draw_order(boxes) == plate_draw_order(transformed));
        for (std::size_t i = 0; i < alpha.size(); ++i) {
            CHECK(std::isfinite(alpha[i]));
            CHECK(alpha[i] >= kPlateCoveredAlpha - 1e-6f);
            CHECK(alpha[i] <= 1.0f);
            CHECK(alpha[i] == doctest::Approx(scaled[i]));
        }
    }
}

TEST_CASE("plate occlusion: the scene hides every name, the target's too unless target_plate_through_walls is on") {
    CHECK(plate_hidden_by_scene(false, false));
    CHECK(plate_hidden_by_scene(false, true)); // the option is the target's alone
    CHECK(plate_hidden_by_scene(true, false)); // the default: as the original, no sight of a target behind a wall
    CHECK_FALSE(plate_hidden_by_scene(true, true));
}

TEST_CASE("plate overlap: the lists filled in place match the returned ones, whatever the list held before") {
    const std::vector<PlateBox> b = {box(0, 0, 100), box(85, 0, 20), box(0, 0, 300, true)};
    std::vector<std::size_t> order{7, 7, 7, 7, 7};
    std::vector<float> alpha{0.5f};
    plate_draw_order(b, order);
    plate_target_alpha(b, alpha);
    CHECK(order == plate_draw_order(b));
    CHECK(alpha == plate_target_alpha(b));
    plate_draw_order({}, order);
    plate_target_alpha({}, alpha);
    CHECK(order.empty());
    CHECK(alpha.empty());
}

TEST_CASE("Name plates: a crowd's fade (the grid) is every pair's exactly") {
    // The pairwise rule as it was written, kept here as the reference.
    auto reference = [](const std::vector<PlateBox>& boxes) {
        auto nearer = [](const PlateBox& a, const PlateBox& b) {
            return a.front != b.front ? a.front : a.depth < b.depth;
        };
        std::vector<float> alpha(boxes.size(), 1.0f);
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            const auto& b = boxes[i];
            if (b.front) continue;
            const float own = std::max(b.x1 - b.x0, 0.0f) * std::max(b.y1 - b.y0, 0.0f);
            if (own <= 0.0f) continue;
            float covered = 0.0f;
            for (std::size_t j = 0; j < boxes.size(); ++j) {
                if (i == j || !nearer(boxes[j], b)) continue;
                const auto& o = boxes[j];
                const float w = std::min(b.x1, o.x1) - std::max(b.x0, o.x0);
                const float h = std::min(b.y1, o.y1) - std::max(b.y0, o.y0);
                covered = std::max(covered, std::max(w, 0.0f) * std::max(h, 0.0f) / own);
            }
            const float t = std::clamp((covered - kPlateFadeFrom) / (kPlateFadeTo - kPlateFadeFrom),
                                       0.0f, 1.0f);
            alpha[i] = 1.0f - (1.0f - kPlateCoveredAlpha) * t * t * (3.0f - 2.0f * t);
        }
        return alpha;
    };
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> pos(0.0f, 1920.0f), size(0.0f, 160.0f), depth(1.0f, 300.0f);
    for (std::size_t count : {5u, 31u, 32u, 64u, 300u, 900u}) {
        CAPTURE(count);
        std::vector<PlateBox> boxes;
        for (std::size_t k = 0; k < count; ++k) {
            PlateBox b;
            b.x0 = pos(rng) * (k % 3 == 0 ? 0.2f : 1.0f); // some crowded together
            b.y0 = pos(rng) * 0.5f * (k % 3 == 0 ? 0.2f : 1.0f);
            b.x1 = b.x0 + size(rng);
            b.y1 = b.y0 + size(rng) * 0.25f;
            if (k % 17 == 0) b.x1 = b.x0;          // no area
            if (k % 23 == 0) b.y1 = b.y0 - 4.0f;   // inverted
            b.depth = k % 11 == 0 && k > 0 ? boxes[k - 1].depth : depth(rng); // some equal depths
            b.front = k == count / 2;
            if (k % 29 == 5) b = boxes[k - 1]; // an exact duplicate
            boxes.push_back(b);
        }
        CHECK(plate_target_alpha(boxes) == reference(boxes));
    }
    // All in one cell row (no height spread), and a non-finite box among a crowd.
    std::vector<PlateBox> line;
    for (int k = 0; k < 100; ++k)
        line.push_back({static_cast<float>(k) * 7.0f, 10.0f, static_cast<float>(k) * 7.0f + 40.0f, 30.0f,
                        static_cast<float>(100 - k), false});
    CHECK(plate_target_alpha(line) == reference(line));
    line[3].x1 = std::numeric_limits<float>::infinity();
    CHECK(plate_target_alpha(line) == reference(line));
}
