// render/horizon.h: what a zone's terrain says about the hills round it, and how strongly they are drawn.
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include "assets/texture.h"
#include "render/atmosphere.h"
#include "render/horizon.h"

using namespace hailstone;
using render::HorizonTriangle;

namespace {

// A level square of ground (x0..x1, y0..y1) at height z, as two triangles of texture `texture`; `flip` winds them
// the other way (the EQG terrain's).
void add_square(std::vector<HorizonTriangle>& out, float x0, float y0, float x1, float y1, float z,
                std::uint32_t texture, bool flip = false) {
    glm::vec3 a(x0, y0, z), b(x1, y0, z), c(x1, y1, z), d(x0, y1, z);
    if (flip) {
        out.push_back({a, c, b, texture});
        out.push_back({a, d, c, texture});
    } else {
        out.push_back({a, b, c, texture});
        out.push_back({a, c, d, texture});
    }
}

// A grid of level squares, each `cell` across, over x0..x1, y0..y1, at the heights height(x, y).
template <typename Height>
void add_grid(std::vector<HorizonTriangle>& out, float x0, float y0, float x1, float y1, float cell, Height height,
              std::uint32_t texture) {
    for (float x = x0; x < x1; x += cell)
        for (float y = y0; y < y1; y += cell) add_square(out, x, y, x + cell, y + cell, height(x, y), texture);
}

bool near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

assets::Image rgba(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                   std::uint8_t a = 255) {
    assets::Image image;
    image.width = w;
    image.height = h;
    std::vector<std::byte> level(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < level.size(); i += 4) {
        level[i] = std::byte{r};
        level[i + 1] = std::byte{g};
        level[i + 2] = std::byte{b};
        level[i + 3] = std::byte{a};
    }
    image.mips.push_back(std::move(level));
    return image;
}

} // namespace

TEST_CASE("horizon: the hills stand round the level ground's middle, past its corners") {
    std::vector<HorizonTriangle> tris;
    // 2000 x 1000 units of flat ground at z 10, off the origin.
    add_grid(tris, 1000.0f, -500.0f, 3000.0f, 500.0f, 100.0f, [](float, float) { return 10.0f; }, 0);
    const std::vector<std::optional<glm::vec3>> colors{glm::vec3(0.1f, 0.3f, 0.05f)};
    const auto land = render::horizon_land(tris, colors, 5.0f);
    REQUIRE(land.valid);
    CHECK(near(land.centre.x, 2000.0f, 60.0f));
    CHECK(near(land.centre.y, 0.0f, 60.0f));
    // Half the diagonal (about 1118, less the 2 % cut each side) plus the margin.
    CHECK(land.radius > 1000.0f + render::kHorizonMargin);
    CHECK(land.radius < 1120.0f + render::kHorizonMargin);
    CHECK(land.ground == doctest::Approx(10.0f));
    CHECK(land.relief == doctest::Approx(render::kHorizonMinRelief)); // flat: the least relief
    CHECK(land.ground_color.g == doctest::Approx(0.3f));
    CHECK(land.seed == 5.0f);
}

TEST_CASE("horizon: a far-off scrap of ground does not move the hills") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, -1000.0f, -1000.0f, 1000.0f, 1000.0f, 100.0f, [](float, float) { return 0.0f; }, 0);
    const auto plain = render::horizon_land(tris, {}, 0.0f);
    // A dock 20000 units out, a tenth of a percent of the area.
    add_square(tris, 20000.0f, 0.0f, 20060.0f, 60.0f, 0.0f, 0);
    const auto with_dock = render::horizon_land(tris, {}, 0.0f);
    REQUIRE(with_dock.valid);
    CHECK(near(with_dock.centre.x, plain.centre.x, 50.0f));
    CHECK(near(with_dock.radius, plain.radius, 50.0f));
}

TEST_CASE("horizon: the relief is the level ground's spread in height, either way up") {
    std::vector<HorizonTriangle> tris;
    // Terraces: a quarter each at 0, 200, 400 and 600, wound either way.
    for (int k = 0; k < 4; ++k)
        for (float x = 0.0f; x < 1000.0f; x += 100.0f)
            for (float y = static_cast<float>(k) * 500.0f; y < static_cast<float>(k + 1) * 500.0f; y += 100.0f)
                add_square(tris, x, y, x + 100.0f, y + 100.0f, 200.0f * static_cast<float>(k), 0, k % 2 == 1);
    const auto land = render::horizon_land(tris, {}, 0.0f);
    REQUIRE(land.valid);
    CHECK(land.relief == doctest::Approx(600.0f));
    CHECK((land.ground == doctest::Approx(200.0f) || land.ground == doctest::Approx(400.0f)));
    // No colours given: the defaults.
    CHECK(land.ground_color == render::kHorizonGround);
    CHECK(land.slope_color.r < land.ground_color.r + 0.1f);
}

TEST_CASE("horizon: walls do not count; slopes give the rock colour") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 1000.0f, 1000.0f, 100.0f, [](float, float) { return 0.0f; }, 0);
    // A slope (45 degrees) of texture 1 and a sheer wall of texture 2 far off, which would move the extent.
    tris.push_back({{0, 0, 0}, {100, 0, 100}, {100, 100, 100}, 1});
    for (int i = 0; i < 200; ++i) {
        const float x = 50000.0f + static_cast<float>(i);
        tris.push_back({{x, 0, 0}, {x, 100, 0}, {x, 100, 500}, 2});
    }
    const std::vector<std::optional<glm::vec3>> colors{glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.4f, 0.4f, 0.4f),
                                                       glm::vec3(1.0f, 0.0f, 0.0f)};
    const auto land = render::horizon_land(tris, colors, 0.0f);
    REQUIRE(land.valid);
    CHECK(land.centre.x < 600.0f);
    CHECK(land.slope_color == glm::vec3(0.4f, 0.4f, 0.4f));
    CHECK(land.ground_color == glm::vec3(0.0f, 0.5f, 0.0f));
}

TEST_CASE("horizon: too little level ground places no hills") {
    std::vector<HorizonTriangle> tris;
    add_grid(tris, 0.0f, 0.0f, 300.0f, 300.0f, 100.0f, [](float, float) { return 0.0f; }, 0); // a room
    CHECK_FALSE(render::horizon_land(tris, {}, 0.0f).valid);
    CHECK_FALSE(render::horizon_land({}, {}, 0.0f).valid);
    // Degenerate and non-finite triangles are skipped.
    tris.push_back({{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, 0});
    tris.push_back({{NAN, 0, 0}, {1, 0, 0}, {0, 1, 0}, 0});
    CHECK_FALSE(render::horizon_land(tris, {}, 0.0f).valid);
}

TEST_CASE("horizon: a texture's mean colour, linear and by alpha") {
    // sRGB 188 is about linear 0.5.
    auto c = render::mean_texture_color(rgba(8, 8, 188, 0, 255));
    REQUIRE(c);
    CHECK(c->r == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(c->g == doctest::Approx(0.0f));
    // Clear texels count for nothing: half the texels opaque red, half clear green.
    auto image = rgba(4, 4, 255, 0, 0);
    for (std::size_t i = 0; i < 8; ++i) {
        image.mips[0][i * 4] = std::byte{0};
        image.mips[0][i * 4 + 1] = std::byte{255};
        image.mips[0][i * 4 + 3] = std::byte{0};
    }
    c = render::mean_texture_color(image);
    REQUIRE(c);
    CHECK(c->r == doctest::Approx(1.0f));
    CHECK(c->g == doctest::Approx(0.0f));
    CHECK_FALSE(render::mean_texture_color(rgba(4, 4, 255, 255, 255, 0)));
    CHECK_FALSE(render::mean_texture_color(assets::Image{}));
    // A short level is not read past.
    auto short_image = rgba(4, 4, 1, 2, 3);
    short_image.mips[0].resize(8);
    CHECK_FALSE(render::mean_texture_color(short_image));
}

TEST_CASE("horizon: BC1 and BC3 colours from their blocks' endpoints; BC7 is not read") {
    assets::Image bc1;
    bc1.width = bc1.height = 4;
    bc1.format = assets::PixelFormat::BC1;
    // One block: both endpoints pure green (RGB565 0x07e0).
    bc1.mips.push_back({std::byte{0xe0}, std::byte{0x07}, std::byte{0xe0}, std::byte{0x07}, std::byte{0}, std::byte{0},
                        std::byte{0}, std::byte{0}});
    auto c = render::mean_texture_color(bc1);
    REQUIRE(c);
    CHECK(c->g == doctest::Approx(1.0f));
    CHECK(c->r == doctest::Approx(0.0f));
    assets::Image bc3 = bc1;
    bc3.format = assets::PixelFormat::BC3;
    std::vector<std::byte> block(16, std::byte{0});
    block[8] = std::byte{0x00}; // colour0: pure red 0xf800
    block[9] = std::byte{0xf8};
    block[10] = std::byte{0x00}; // colour1: black
    block[11] = std::byte{0x00};
    bc3.mips = {block};
    c = render::mean_texture_color(bc3);
    REQUIRE(c);
    CHECK(c->r == doctest::Approx(0.5f));
    assets::Image bc7 = bc3;
    bc7.format = assets::PixelFormat::BC7;
    CHECK_FALSE(render::mean_texture_color(bc7));
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

TEST_CASE("horizon: the strength fades with a thick fog, out of the modern view's reach and under a storm") {
    CHECK(render::horizon_strength(render::kHorizonFogNone, 1.0f, 0.0f) == 0.0f);
    CHECK(render::horizon_strength(400.0f, 1.0f, 0.0f) < 0.05f); // nektulos
    CHECK(render::horizon_strength(800.0f, 1.0f, 0.0f) > 0.9f);  // ecommons
    CHECK(render::horizon_strength(render::kHorizonFogFull, 1.0f, 0.0f) == 1.0f);
    CHECK(render::horizon_strength(5000.0f, 1.0f, 0.0f) == 1.0f);
    const float mid = render::horizon_strength(600.0f, 1.0f, 0.0f);
    CHECK(mid > 0.0f);
    CHECK(mid < 1.0f);
    CHECK(render::horizon_strength(5000.0f, 0.0f, 0.0f) == 0.0f); // lava, a thick-fog region
    CHECK(render::horizon_strength(5000.0f, 0.5f, 0.0f) == doctest::Approx(0.5f));
    CHECK(render::horizon_strength(5000.0f, 1.0f, 1.0f) == doctest::Approx(0.25f));
    CHECK(render::horizon_strength(5000.0f, 2.0f, -1.0f) == 1.0f); // clamped
}
