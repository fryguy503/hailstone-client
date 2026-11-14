// Levels of detail: which level of a replacement model the scene and the shadow passes draw (render/lod_bands.h).

#include <doctest/doctest.h>

#include "render/lod_bands.h"

using namespace hailstone::render;

namespace {

// A generated tree's three levels (texpack/trees.h): near to 200, middle to 600, far beyond.
constexpr LodBand kNear{0.0f, 200.0f, 0, 3};
constexpr LodBand kMiddle{200.0f, 600.0f, 1, 3};
constexpr LodBand kFar{600.0f, 1e6f, 2, 3};

int shown(float dist, LodPick pick = {}, float scale = 1.0f) {
    int levels = 0, which = -1;
    for (const LodBand& b : {kNear, kMiddle, kFar})
        if (lod_shows(b, dist, pick, scale)) {
            ++levels;
            which = b.level;
        }
    return levels == 1 ? which : -1;
}

} // namespace

TEST_CASE("levels of detail: the scene shows the band holding the distance") {
    CHECK(shown(0.0f) == 0);
    CHECK(shown(199.0f) == 0);
    CHECK(shown(200.0f) == 1);
    CHECK(shown(599.0f) == 1);
    CHECK(shown(600.0f) == 2);
    CHECK(shown(50000.0f) == 2);
    CHECK(lod_shows(LodBand{}, 1e9f)); // not a level: always
}

TEST_CASE("levels of detail: a shadow draws at least its level, the camera's where coarser") {
    const LodPick middle{1, false}, last{255, false};
    CHECK(shown(10.0f, middle) == 1);
    CHECK(shown(300.0f, middle) == 1);
    CHECK(shown(700.0f, middle) == 2);
    CHECK(shown(10.0f, last) == 2);
    CHECK(shown(300.0f, last) == 2);
    CHECK(kShadowLodLevel[0] == 1);
    CHECK(kShadowLodLevel[3] == 255);
}

TEST_CASE("levels of detail: a light's shadow cube draws one level wherever the camera is") {
    for (float d : {0.0f, 300.0f, 5000.0f}) CHECK(shown(d, kPointShadowLod) == 1);
    // A mesh with two levels: the middle is its last.
    const LodBand a{0.0f, 100.0f, 0, 2}, b{100.0f, 1e6f, 1, 2};
    CHECK_FALSE(lod_shows(a, 0.0f, LodPick{5, true}));
    CHECK(lod_shows(b, 0.0f, LodPick{5, true}));
}

TEST_CASE("levels of detail: the detail setting scales the bands") {
    CHECK(shown(250.0f, {}, 1.5f) == 0);
    CHECK(shown(150.0f, {}, 0.5f) == 1);
    CHECK(shown(350.0f, {}, 0.5f) == 2);
}

TEST_CASE("levels of detail: the scene fades one level into the next") {
    // Where the near and middle levels meet (200), across 20 % of it: both drawn, adding up to the whole.
    for (float d : {181.0f, 190.0f, 200.0f, 210.0f, 219.0f}) {
        INFO(d);
        CHECK(lod_shows(kNear, d, kSceneLod));
        CHECK(lod_shows(kMiddle, d, kSceneLod));
        CHECK(lod_presence(kNear, d).out + lod_presence(kMiddle, d).in == doctest::Approx(1.0f));
    }
    CHECK(lod_presence(kNear, 180.0f).out == doctest::Approx(1.0f));
    CHECK(lod_presence(kMiddle, 220.0f).in == doctest::Approx(1.0f));
    CHECK_FALSE(lod_shows(kMiddle, 179.0f, kSceneLod));
    CHECK_FALSE(lod_shows(kNear, 221.0f, kSceneLod));
    // The first level fades in from nowhere: shown from the start.
    CHECK(lod_presence(kNear, 0.0f).in == 1.0f);
    // The far level's fade starts at 540.
    CHECK(lod_shows(kFar, 545.0f, kSceneLod));
    CHECK_FALSE(lod_shows(kFar, 535.0f, kSceneLod));
    // The shadows do not fade.
    CHECK_FALSE(lod_shows(kMiddle, 190.0f, LodPick{1, false}) == lod_shows(kNear, 190.0f, LodPick{1, false}));
}
