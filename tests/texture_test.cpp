#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "assets/pfs.h"
#include "assets/texture.h"
#include "core/byte_io.h"
#include "game_files.h"
#include "render/gpu.h"
#include "ui/ui_textures.h"

using namespace hailstone::assets;
using hailstone::ByteWriter;
using hailstone::render::TextureUse;
using hailstone::render::prepare_texture_image;

TEST_CASE("UI animation art: XML frame crops retain original dimensions and aspect") {
    const std::string xml = R"xml(<XML><Ui2DAnimation item="FixtureBadge"><Frames>
      <Texture>fixture-atlas.tga</Texture><Location><X>64</X><Y>32</Y></Location>
      <Size><CX>128</CX><CY>30</CY></Size></Frames><Frames>
      <Texture>not-the-first-frame.tga</Texture><Location><X>0</X><Y>0</Y></Location>
      <Size><CX>4</CX><CY>4</CY></Size></Frames></Ui2DAnimation></XML>)xml";
    const auto frame = hailstone::ui::find_ui_animation_frame(xml, "FixtureBadge");
    REQUIRE(frame); CHECK(frame->file == "fixture-atlas.tga");
    CHECK_FALSE(hailstone::ui::find_ui_animation_frame(xml, "MissingBadge"));
    const hailstone::ui::UiTextures::Texture atlas{reinterpret_cast<void*>(std::uintptr_t{1}), 256, 256};
    const auto image = hailstone::ui::ui_animation_crop(atlas, *frame);
    REQUIRE(image); CHECK(image->width == 128); CHECK(image->height == 30);
    CHECK(image->cell.id == atlas.id); CHECK(image->cell.u0 == doctest::Approx(0.25));
    CHECK(image->cell.v0 == doctest::Approx(0.125)); CHECK(image->cell.u1 == doctest::Approx(0.75));
    CHECK(image->cell.v1 == doctest::Approx(62.0 / 256.0));
}

TEST_CASE("UI animation art: malformed atlas paths and crops never become fallback origin images") {
    using Textures = hailstone::ui::UiTextures;
    const Textures::Texture atlas{reinterpret_cast<void*>(std::uintptr_t{1}), 256, 256};
    const Textures::AnimationFrame valid{"badge.tga", 0, 0, 128, 32};
    for (const auto& name : std::vector<std::string>{"", ".", "..", "../badge.tga", "dir/badge.tga", "dir\\badge.tga",
             "/badge.tga", "C:badge.tga", "https:badge.tga", std::string("x\0.tga", 6), "bad\n.tga"}) {
        INFO(name);
        auto frame = valid; frame.file = name;
        CHECK_FALSE(hailstone::ui::ui_animation_crop(atlas, frame));
    }
    for (const auto& frame : std::vector<Textures::AnimationFrame>{
             {"badge.tga", -1, 0, 128, 32}, {"badge.tga", 0, -1, 128, 32},
             {"badge.tga", 0, 0, 0, 32}, {"badge.tga", 0, 0, 128, -1},
             {"badge.tga", 200, 0, 128, 32}, {"badge.tga", 0, 240, 128, 32},
             {"badge.tga", std::numeric_limits<int>::max(), 0, std::numeric_limits<int>::max(), 32}})
        CHECK_FALSE(hailstone::ui::ui_animation_crop(atlas, frame));
    CHECK_FALSE(hailstone::ui::ui_animation_crop({nullptr, 256, 256}, valid));
    CHECK_FALSE(hailstone::ui::ui_animation_crop({atlas.id, 0, 256}, valid));
    for (const auto& x : {"2147483648", "1garbage", "+-1", ""}) {
        const std::string xml = "<Ui2DAnimation item=\"Bad\"><Frames><Texture>badge.tga</Texture><Location><X>" +
            std::string(x) + "</X><Y>0</Y></Location><Size><CX>128</CX><CY>32</CY></Size></Frames></Ui2DAnimation>";
        CHECK_FALSE(hailstone::ui::find_ui_animation_frame(xml, "Bad"));
    }
}

TEST_CASE("UI animation art: installed stock badges resolve XML rectangles against actual atlases" *
          doctest::skip(!hailstone::test::game_dir())) {
    const auto* game = hailstone::test::game_dir();
    REQUIRE(game);
    const auto data = game->read("uifiles/default/EQUI_Animations.xml");
    REQUIRE(data);
    const std::string xml(reinterpret_cast<const char*>(data->data()), data->size());
    struct Badge { const char* name; const char* file; int x, y, width, height; };
    for (const auto& badge : std::array<Badge, 3>{{
             {"A_EQ", "EQ_expansion_logos.tga", 0, 0, 128, 32},
             {"A_ROK", "EQ_expansion_logos.tga", 0, 96, 128, 32},
             {"A_General", "Achievement_subcat_base.tga", 0, 0, 128, 30}}}) {
        INFO(badge.name);
        const auto frame = hailstone::ui::find_ui_animation_frame(xml, badge.name);
        REQUIRE(frame); CHECK(frame->file == badge.file); CHECK(frame->x == badge.x); CHECK(frame->y == badge.y);
        CHECK(frame->w == badge.width); CHECK(frame->h == badge.height);
        const auto bytes = game->read("uifiles/default/" + frame->file); REQUIRE(bytes);
        const auto decoded = decode_texture(*bytes); REQUIRE(decoded);
        REQUIRE(decoded->width <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()));
        REQUIRE(decoded->height <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()));
        const hailstone::ui::UiTextures::Texture atlas{reinterpret_cast<void*>(std::uintptr_t{1}),
            static_cast<int>(decoded->width), static_cast<int>(decoded->height)};
        const auto crop = hailstone::ui::ui_animation_crop(atlas, *frame); REQUIRE(crop);
        CHECK(crop->width == badge.width); CHECK(crop->height == badge.height);
        CHECK(crop->cell.u0 == doctest::Approx(static_cast<double>(badge.x) / decoded->width));
        CHECK(crop->cell.v0 == doctest::Approx(static_cast<double>(badge.y) / decoded->height));
        CHECK(crop->cell.u1 == doctest::Approx(static_cast<double>(badge.x + badge.width) / decoded->width));
        CHECK(crop->cell.v1 == doctest::Approx(static_cast<double>(badge.y + badge.height) / decoded->height));
    }
}

namespace {

using Px = std::array<int, 4>;

Px pixel(const Image& img, std::size_t level, std::uint32_t x, std::uint32_t y) {
    std::uint32_t w = std::max<std::uint32_t>(1, img.width >> level);
    const auto& m = img.mips.at(level);
    std::size_t i = (std::size_t(y) * w + x) * 4;
    return {std::to_integer<int>(m.at(i)), std::to_integer<int>(m.at(i + 1)), std::to_integer<int>(m.at(i + 2)),
            std::to_integer<int>(m.at(i + 3))};
}

void put_chars(ByteWriter& w, const char* s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) w.le<std::uint8_t>(static_cast<std::uint8_t>(s[i]));
}

struct DdsSpec {
    std::uint32_t width = 4, height = 4, mips = 1;
    const char* fourcc = nullptr; // compressed when set
    std::uint32_t flags = 0x40, bits = 32, r = 0, g = 0, b = 0, a = 0;
};

ByteWriter dds_header(const DdsSpec& s) {
    ByteWriter w;
    put_chars(w, "DDS ", 4);
    w.le<std::uint32_t>(124);
    w.le<std::uint32_t>(0x1007 | (s.mips > 1 ? 0x20000u : 0u));
    w.le<std::uint32_t>(s.height);
    w.le<std::uint32_t>(s.width);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(s.mips);
    w.zeros(44);
    w.le<std::uint32_t>(32);
    if (s.fourcc) {
        w.le<std::uint32_t>(0x4);
        put_chars(w, s.fourcc, 4);
        w.zeros(20);
    } else {
        w.le<std::uint32_t>(s.flags);
        w.le<std::uint32_t>(0);
        w.le<std::uint32_t>(s.bits);
        w.le<std::uint32_t>(s.r);
        w.le<std::uint32_t>(s.g);
        w.le<std::uint32_t>(s.b);
        w.le<std::uint32_t>(s.a);
    }
    w.le<std::uint32_t>(0x1000);
    w.zeros(16);
    REQUIRE(w.size() == 128);
    return w;
}

void bc1_block(ByteWriter& w, std::uint16_t c0, std::uint16_t c1, std::uint32_t indices) {
    w.le(c0);
    w.le(c1);
    w.le(indices);
}

// A DX10-header DDS of the given DXGI format (texture packs).
ByteWriter dx10_header(std::uint32_t width, std::uint32_t height, std::uint32_t mips, std::uint32_t dxgi) {
    ByteWriter w = dds_header({.width = width, .height = height, .mips = mips, .fourcc = "DX10"});
    w.le<std::uint32_t>(dxgi);
    w.le<std::uint32_t>(3); // TEXTURE2D
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(0);
    return w;
}

// A BC4 block (BC3's alpha block): endpoints and one 3-bit index per texel.
void bc4_block(ByteWriter& w, std::uint8_t e0, std::uint8_t e1, std::uint64_t indices) {
    w.le(e0);
    w.le(e1);
    for (int i = 0; i < 6; ++i) w.le<std::uint8_t>(static_cast<std::uint8_t>(indices >> (8 * i)));
}

// Selector t%4 for texel t: row-major 0,1,2,3 in every row.
constexpr std::uint32_t kColumnSelectors = 0xE4E4E4E4;

Image decode_ok(const std::vector<std::byte>& data, const TextureOptions& options = {}) {
    std::string error;
    auto img = decode_texture(data, options, &error);
    INFO(error);
    REQUIRE(img);
    return *img;
}

// BMP with a 40-byte info header. `pixels` is the stored pixel array.
std::vector<std::byte> bmp(std::int32_t width, std::int32_t height, std::uint16_t bits,
                           const std::vector<std::uint32_t>& palette, const std::vector<std::uint8_t>& pixels,
                           std::uint32_t compression = 0) {
    ByteWriter w;
    std::uint32_t offset = 14 + 40 + static_cast<std::uint32_t>(palette.size()) * 4;
    put_chars(w, "BM", 2);
    w.le<std::uint32_t>(offset + static_cast<std::uint32_t>(pixels.size()));
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(offset);
    w.le<std::uint32_t>(40);
    w.le<std::int32_t>(width);
    w.le<std::int32_t>(height);
    w.le<std::uint16_t>(1);
    w.le<std::uint16_t>(bits);
    w.le<std::uint32_t>(compression);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(pixels.size()));
    w.zeros(8);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(palette.size()));
    w.le<std::uint32_t>(0);
    for (auto c : palette) w.le(c); // 0x00RRGGBB stored as B G R 0
    for (auto p : pixels) w.le(p);
    return w.take();
}

} // namespace

TEST_CASE("mip_size rounds BCn to whole blocks") {
    CHECK(mip_size(PixelFormat::BC1, 4, 4) == 8);
    CHECK(mip_size(PixelFormat::BC1, 1, 1) == 8);
    CHECK(mip_size(PixelFormat::BC1, 5, 3) == 16);
    CHECK(mip_size(PixelFormat::BC3, 8, 2) == 32);
    CHECK(mip_size(PixelFormat::RGBA8, 3, 2) == 24);
    CHECK(mip_size(PixelFormat::BC4, 5, 3) == 16);
    CHECK(mip_size(PixelFormat::BC5, 4, 4) == 16);
    CHECK(mip_size(PixelFormat::BC7, 8, 8) == 64);
}

TEST_CASE("BC4 and BC5, under ATI1/ATI2 FourCCs and DX10 headers, decode as the GPU samples them") {
    std::uint64_t idx = 0;
    for (int t = 0; t < 16; ++t) idx |= std::uint64_t(t % 2) << (3 * t); // e0, e1, e0, ...
    for (const char* cc : {"ATI1", "BC4U"}) {
        auto w = dds_header({.fourcc = cc});
        bc4_block(w, 200, 40, idx);
        auto img = decode_ok(w.take());
        CHECK(img.format == PixelFormat::BC4);
        auto rgba = to_rgba8(img);
        CHECK(pixel(rgba, 0, 0, 0) == Px{200, 0, 0, 255});
        CHECK(pixel(rgba, 0, 1, 0) == Px{40, 0, 0, 255});
    }
    auto w = dx10_header(4, 4, 1, 83);
    bc4_block(w, 10, 250, idx);
    bc4_block(w, 128, 0, 0);
    auto img = decode_ok(w.take());
    CHECK(img.format == PixelFormat::BC5);
    auto rgba = to_rgba8(img);
    CHECK(pixel(rgba, 0, 0, 3) == Px{10, 128, 0, 255});
    CHECK(pixel(rgba, 0, 1, 3) == Px{250, 128, 0, 255});

    auto w4 = dx10_header(4, 4, 1, 80);
    bc4_block(w4, 77, 77, 0);
    CHECK(decode_ok(w4.take()).format == PixelFormat::BC4);
    auto bad = dx10_header(4, 4, 1, 2); // R32G32B32A32_FLOAT
    bad.zeros(256);
    std::string error;
    CHECK_FALSE(decode_texture(bad.take(), {}, &error));
    CHECK(error.find("DXGI") != std::string::npos);
}

TEST_CASE("BC7 through a DX10 header, with its mips; RGBA8 and BC1 DXGI formats too") {
    // Mode 6 (bit 6 set): 7-bit RGBA endpoints, one p-bit each, 4-bit indices. Red endpoints 0, the rest 127, both
    // p-bits 1: every texel (1, 255, 255, 255) whatever its index.
    std::array<std::uint8_t, 16> block{};
    auto set = [&](int bit) { block[bit / 8] |= static_cast<std::uint8_t>(1u << (bit % 8)); };
    set(6);
    for (int bit = 7 + 14; bit < 7 + 56 + 2; ++bit) set(bit);
    for (std::uint32_t dxgi : {98u, 99u}) {
        auto w = dx10_header(8, 4, 2, dxgi);
        for (int b = 0; b < 3; ++b) // 8x4: two blocks; 4x2: one
            for (auto v : block) w.le(v);
        auto img = decode_ok(w.take());
        CHECK(img.format == PixelFormat::BC7);
        REQUIRE(img.mips.size() == 2);
        auto rgba = to_rgba8(img);
        CHECK(pixel(rgba, 0, 7, 3) == Px{1, 255, 255, 255});
        CHECK(pixel(rgba, 1, 3, 1) == Px{1, 255, 255, 255});
    }
    auto w = dx10_header(1, 1, 1, 28);
    w.le<std::uint32_t>(0x80402010);
    CHECK(pixel(decode_ok(w.take()), 0, 0, 0) == Px{0x10, 0x20, 0x40, 0x80});
    auto w1 = dx10_header(4, 4, 1, 71);
    bc1_block(w1, 0xF800, 0xF800, 0);
    CHECK(decode_ok(w1.take()).format == PixelFormat::BC1);
}

TEST_CASE("BC1 four-colour block decodes exactly") {
    auto w = dds_header({.fourcc = "DXT1"});
    bc1_block(w, 0xF800, 0x001F, kColumnSelectors); // red, blue
    auto img = decode_ok(w.take());
    CHECK(img.format == PixelFormat::BC1);
    REQUIRE(img.mips.size() == 1);
    auto rgba = to_rgba8(img);
    for (std::uint32_t y = 0; y < 4; ++y) {
        CHECK(pixel(rgba, 0, 0, y) == Px{255, 0, 0, 255});
        CHECK(pixel(rgba, 0, 1, y) == Px{0, 0, 255, 255});
        CHECK(pixel(rgba, 0, 2, y) == Px{170, 0, 85, 255});
        CHECK(pixel(rgba, 0, 3, y) == Px{85, 0, 170, 255});
    }
}

TEST_CASE("BC1 punch-through block: colour0 <= colour1 makes selector 3 transparent") {
    auto w = dds_header({.width = 8, .height = 4, .fourcc = "DXT1"});
    bc1_block(w, 0x001F, 0xF800, kColumnSelectors); // blue < red
    bc1_block(w, 0x0000, 0x0000, 0xFFFF0000);       // equal: rows 0-1 black, rows 2-3 transparent
    auto rgba = to_rgba8(decode_ok(w.take()));
    CHECK(pixel(rgba, 0, 0, 0) == Px{0, 0, 255, 255});
    CHECK(pixel(rgba, 0, 1, 0) == Px{255, 0, 0, 255});
    CHECK(pixel(rgba, 0, 2, 0) == Px{127, 0, 127, 255});
    CHECK(pixel(rgba, 0, 3, 0) == Px{0, 0, 0, 0});
    // Opaque black stays opaque.
    CHECK(pixel(rgba, 0, 4, 1) == Px{0, 0, 0, 255});
    CHECK(pixel(rgba, 0, 7, 3) == Px{0, 0, 0, 0});
}

TEST_CASE("BC2 explicit alpha and four-colour mode") {
    auto w = dds_header({.fourcc = "DXT3"});
    // Alpha nibble = column * 5 for every row (0, 5, 10, 15).
    for (int row = 0; row < 4; ++row) w.le<std::uint16_t>(0xFA50);
    bc1_block(w, 0x001F, 0xF800, kColumnSelectors); // colour0 < colour1 but still four-colour
    auto rgba = to_rgba8(decode_ok(w.take()));
    CHECK(pixel(rgba, 0, 0, 2) == Px{0, 0, 255, 0});
    CHECK(pixel(rgba, 0, 1, 2) == Px{255, 0, 0, 85});
    CHECK(pixel(rgba, 0, 2, 2) == Px{85, 0, 170, 170});
    CHECK(pixel(rgba, 0, 3, 2) == Px{170, 0, 85, 255});
}

TEST_CASE("BC3 interpolated alpha, both modes") {
    auto w = dds_header({.width = 8, .height = 4, .fourcc = "DXT5"});
    // Block 0: a0=255 > a1=0, eight-value mode. Texel t uses index t%8.
    std::uint64_t idx = 0;
    for (int t = 0; t < 16; ++t) idx |= std::uint64_t(t % 8) << (3 * t);
    w.le<std::uint8_t>(255);
    w.le<std::uint8_t>(0);
    for (int i = 0; i < 6; ++i) w.le<std::uint8_t>(static_cast<std::uint8_t>(idx >> (8 * i)));
    bc1_block(w, 0xFFFF, 0x0000, 0);
    // Block 1: a0=0 <= a1=100, six-value mode with 0 and 255.
    w.le<std::uint8_t>(0);
    w.le<std::uint8_t>(100);
    for (int i = 0; i < 6; ++i) w.le<std::uint8_t>(static_cast<std::uint8_t>(idx >> (8 * i)));
    bc1_block(w, 0xFFFF, 0x0000, 0);
    auto rgba = to_rgba8(decode_ok(w.take()));
    const int eight[8] = {255, 0, 218, 182, 145, 109, 72, 36};
    const int six[8] = {0, 100, 20, 40, 60, 80, 0, 255};
    for (std::uint32_t t = 0; t < 8; ++t) {
        CHECK(pixel(rgba, 0, t % 4, t / 4)[3] == eight[t]);
        CHECK(pixel(rgba, 0, 4 + t % 4, t / 4)[3] == six[t]);
    }
    CHECK(pixel(rgba, 0, 0, 0) == Px{255, 255, 255, 255});
}

TEST_CASE("DDS mip chain is kept and clamped to the data present") {
    auto w = dds_header({.width = 8, .height = 8, .mips = 4, .fourcc = "DXT1"});
    // 8x8 -> 4 blocks, 4x4 -> 1, 2x2 -> 1, 1x1 -> 1: supply only three levels.
    for (int i = 0; i < 6; ++i) bc1_block(w, 0xFFFF, 0, 0);
    auto img = decode_ok(w.take());
    CHECK(img.mips.size() == 3);
    CHECK(img.mips[0].size() == 32);
    CHECK(img.mips[2].size() == 8);
    auto rgba = to_rgba8(img);
    REQUIRE(rgba.mips.size() == 3);
    CHECK(rgba.mips[1].size() == 64);
    CHECK(rgba.mips[2].size() == 16);
}

TEST_CASE("DDS with non-multiple-of-4 size uses partial blocks") {
    auto w = dds_header({.width = 5, .height = 3, .fourcc = "DXT1"});
    bc1_block(w, 0xF800, 0xF800, 0);
    bc1_block(w, 0x07E0, 0x07E0, 0);
    auto rgba = to_rgba8(decode_ok(w.take()));
    REQUIRE(rgba.mips[0].size() == 5 * 3 * 4);
    CHECK(pixel(rgba, 0, 3, 2) == Px{255, 0, 0, 255});
    CHECK(pixel(rgba, 0, 4, 2) == Px{0, 255, 0, 255});
}

TEST_CASE("Uncompressed DDS formats convert through their masks") {
    SUBCASE("A8R8G8B8") {
        auto w = dds_header({.width = 1, .height = 1, .flags = 0x41, .bits = 32, .r = 0xff0000, .g = 0xff00,
                             .b = 0xff, .a = 0xff000000});
        w.le<std::uint32_t>(0x80112233);
        CHECK(pixel(decode_ok(w.take()), 0, 0, 0) == Px{0x11, 0x22, 0x33, 0x80});
    }
    SUBCASE("X8R8G8B8 ignores the unused byte") {
        auto w = dds_header({.width = 1, .height = 1, .flags = 0x40, .bits = 32, .r = 0xff0000, .g = 0xff00,
                             .b = 0xff, .a = 0xff000000});
        w.le<std::uint32_t>(0x00112233);
        CHECK(pixel(decode_ok(w.take()), 0, 0, 0) == Px{0x11, 0x22, 0x33, 0xff});
    }
    SUBCASE("A8B8G8R8") {
        auto w = dds_header({.width = 1, .height = 1, .flags = 0x41, .bits = 32, .r = 0xff, .g = 0xff00,
                             .b = 0xff0000, .a = 0xff000000});
        w.le<std::uint32_t>(0x40332211);
        CHECK(pixel(decode_ok(w.take()), 0, 0, 0) == Px{0x11, 0x22, 0x33, 0x40});
    }
    SUBCASE("R8G8B8") {
        auto w = dds_header({.width = 1, .height = 1, .bits = 24, .r = 0xff0000, .g = 0xff00, .b = 0xff});
        w.le<std::uint8_t>(0x33);
        w.le<std::uint8_t>(0x22);
        w.le<std::uint8_t>(0x11);
        CHECK(pixel(decode_ok(w.take()), 0, 0, 0) == Px{0x11, 0x22, 0x33, 0xff});
    }
    SUBCASE("R5G6B5") {
        auto w = dds_header({.width = 2, .height = 1, .bits = 16, .r = 0xf800, .g = 0x07e0, .b = 0x001f});
        w.le<std::uint16_t>(0xF800);
        w.le<std::uint16_t>(0x07FF);
        auto img = decode_ok(w.take());
        CHECK(pixel(img, 0, 0, 0) == Px{255, 0, 0, 255});
        CHECK(pixel(img, 0, 1, 0) == Px{0, 255, 255, 255});
    }
    SUBCASE("A1R5G5B5") {
        auto w = dds_header(
            {.width = 2, .height = 1, .flags = 0x41, .bits = 16, .r = 0x7c00, .g = 0x03e0, .b = 0x001f, .a = 0x8000});
        w.le<std::uint16_t>(0x801F);
        w.le<std::uint16_t>(0x7C00);
        auto img = decode_ok(w.take());
        CHECK(pixel(img, 0, 0, 0) == Px{0, 0, 255, 255});
        CHECK(pixel(img, 0, 1, 0) == Px{255, 0, 0, 0});
    }
    SUBCASE("A4R4G4B4") {
        auto w = dds_header(
            {.width = 1, .height = 1, .flags = 0x41, .bits = 16, .r = 0x0f00, .g = 0x00f0, .b = 0x000f, .a = 0xf000});
        w.le<std::uint16_t>(0x8F05);
        CHECK(pixel(decode_ok(w.take()), 0, 0, 0) == Px{255, 0, 85, 136});
    }
}

TEST_CASE("8-bit BMP with palette key, bottom-up, row padding") {
    // 3x2, stored bottom row first; each row padded from 3 to 4 bytes.
    std::vector<std::uint32_t> pal = {0x000000, 0xff0000, 0x00ff00, 0x0000ff};
    std::vector<std::uint8_t> px = {1, 2, 3, 0xEE, // bottom row
                                    0, 1, 2, 0xEE};
    auto data = bmp(3, 2, 8, pal, px);
    auto img = decode_ok(data);
    CHECK(img.format == PixelFormat::RGBA8);
    CHECK(img.mips.size() == 1);
    CHECK(pixel(img, 0, 0, 0) == Px{0, 0, 0, 255});
    CHECK(pixel(img, 0, 1, 0) == Px{255, 0, 0, 255});
    CHECK(pixel(img, 0, 2, 1) == Px{0, 0, 255, 255});
    auto keyed = decode_ok(data, {.palette_index0_transparent = true});
    CHECK(pixel(keyed, 0, 0, 0) == Px{0, 0, 0, 0});
    CHECK(pixel(keyed, 0, 1, 0) == Px{255, 0, 0, 255});
}

TEST_CASE("24-bit BMP bottom-up with padding, and top-down") {
    // 2x2: row stride 6 -> 8 bytes. Stored B G R.
    std::vector<std::uint8_t> px = {0, 0, 255, 0, 255, 0, 0, 0,    // bottom: red, green
                                    255, 0, 0, 255, 255, 255, 0, 0}; // top: blue, white
    auto img = decode_ok(bmp(2, 2, 24, {}, px));
    CHECK(pixel(img, 0, 0, 0) == Px{0, 0, 255, 255});
    CHECK(pixel(img, 0, 1, 0) == Px{255, 255, 255, 255});
    CHECK(pixel(img, 0, 0, 1) == Px{255, 0, 0, 255});
    CHECK(pixel(img, 0, 1, 1) == Px{0, 255, 0, 255});
    auto flipped = decode_ok(bmp(2, -2, 24, {}, px));
    CHECK(pixel(flipped, 0, 0, 0) == Px{255, 0, 0, 255});
}

TEST_CASE("WLD BMP rows: flipped to the meshes' bottom-origin UVs; DDS untouched") {
    std::vector<std::uint8_t> px = {0, 0, 255, 0, 255, 0, 0, 0,    // bottom: red, green
                                    255, 0, 0, 255, 255, 255, 0, 0}; // top: blue, white
    auto img = decode_texture(bmp(2, 2, 24, {}, px), {.wld_bmp_rows = true});
    REQUIRE(img);
    CHECK(pixel(*img, 0, 0, 0) == Px{255, 0, 0, 255}); // the file's bottom row first
    CHECK(pixel(*img, 0, 0, 1) == Px{0, 0, 255, 255});
}

TEST_CASE("RLE8 BMP") {
    std::vector<std::uint32_t> pal = {0x000000, 0xff0000, 0x00ff00};
    // 4x2: bottom row run of 4 x index1; top row absolute 2,1,2 then encoded 1 x index0; end.
    std::vector<std::uint8_t> px = {4, 1, 0, 0, 0, 3, 2, 1, 2, 0, 1, 0, 0, 1};
    auto img = decode_ok(bmp(4, 2, 8, pal, px, 1));
    CHECK(pixel(img, 0, 0, 1) == Px{255, 0, 0, 255});
    CHECK(pixel(img, 0, 3, 1) == Px{255, 0, 0, 255});
    CHECK(pixel(img, 0, 0, 0) == Px{0, 255, 0, 255});
    CHECK(pixel(img, 0, 1, 0) == Px{255, 0, 0, 255});
    CHECK(pixel(img, 0, 3, 0) == Px{0, 0, 0, 255});
}

TEST_CASE("RLE8 BMP: a size far past what its runs could fill is refused before it allocates") {
    std::vector<std::uint32_t> pal = {0x000000, 0xff0000};
    // 8192 x 256 from two bytes: an end of bitmap at once leaves every pixel unwritten (index 0). (Its rows pass
    // the older check: one delta could skip 255 of them.)
    std::vector<std::uint8_t> px = {0, 1};
    std::string error;
    CHECK_FALSE(decode_texture(bmp(8192, 256, 8, pal, px, 1), {}, &error));
    CHECK(error.find("too short") != std::string::npos);
    // A small one ended early is still a picture of index 0 where nothing was written.
    auto img = decode_ok(bmp(64, 64, 8, pal, {2, 1, 0, 1}, 1));
    CHECK(pixel(img, 0, 0, 63) == Px{255, 0, 0, 255});
    CHECK(pixel(img, 0, 2, 63) == Px{0, 0, 0, 255});
    CHECK(pixel(img, 0, 5, 0) == Px{0, 0, 0, 255});
    // Runs that do fill a large image: 1024 x 256, each row four runs of 255 and one of 4.
    std::vector<std::uint8_t> rows;
    for (int y = 0; y < 256; ++y) {
        for (int k = 0; k < 4; ++k) rows.insert(rows.end(), {255, 1});
        rows.insert(rows.end(), {4, 1, 0, 0});
    }
    rows.insert(rows.end(), {0, 1});
    auto full = decode_ok(bmp(1024, 256, 8, pal, rows, 1));
    CHECK(pixel(full, 0, 1023, 0) == Px{255, 0, 0, 255});
    CHECK(pixel(full, 0, 0, 255) == Px{255, 0, 0, 255});
}

TEST_CASE("Truncated and corrupt textures are rejected") {
    std::string error;
    auto w = dds_header({.width = 8, .height = 8, .fourcc = "DXT5"});
    w.zeros(16 * 3); // one block short
    auto dds = w.take();
    CHECK_FALSE(decode_texture(dds, {}, &error));
    CHECK_FALSE(error.empty());
    for (std::size_t n : {std::size_t(0), std::size_t(3), std::size_t(60), std::size_t(127)}) {
        std::vector<std::byte> cut(dds.begin(), dds.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK_FALSE(decode_texture(cut));
    }
    auto b = bmp(3, 2, 8, {0, 0xffffff}, {1, 1, 1, 0, 1, 1, 1, 0});
    for (std::size_t n : {std::size_t(2), std::size_t(30), std::size_t(60), b.size() - 2}) {
        std::vector<std::byte> cut(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK_FALSE(decode_texture(cut));
    }
    auto huge = dds_header({.width = 1u << 20, .height = 1u << 20, .fourcc = "DXT1"}).take();
    CHECK_FALSE(decode_texture(huge));
    std::vector<std::byte> junk(256, std::byte{0x42});
    CHECK_FALSE(decode_texture(junk));
}

TEST_CASE("Textures over 8192 x 8192 texels are refused before decoding (no 1 GiB RGBA from a 128 MiB BC1)") {
    std::string error;
    // Each side within 16384, the area past 8192 x 8192: refused for its size, not for missing data.
    auto big = dds_header({.width = 16384, .height = 4097, .fourcc = "DXT1"}).take();
    CHECK_FALSE(decode_texture(big, {}, &error));
    CHECK(error.find("dimensions") != std::string::npos);
    // 8192 x 8192 is allowed: this one fails only because its data is missing.
    auto limit = dds_header({.width = 8192, .height = 8192, .fourcc = "DXT1"}).take();
    CHECK_FALSE(decode_texture(limit, {}, &error));
    CHECK(error.find("truncated") != std::string::npos);

    auto b = bmp(16384, -8193, 8, {0}, {0, 0, 0, 0});
    CHECK_FALSE(decode_texture(b, {}, &error));
    CHECK(error.find("dimensions") != std::string::npos);

    ByteWriter t;
    t.le<std::uint8_t>(0);
    t.le<std::uint8_t>(0);
    t.le<std::uint8_t>(2);
    t.zeros(9);
    t.le<std::uint16_t>(16384);
    t.le<std::uint16_t>(8193);
    t.le<std::uint8_t>(32);
    t.le<std::uint8_t>(8);
    t.zeros(64);
    CHECK_FALSE(decode_texture(t.take(), {}, &error));
    CHECK(error.find("dimensions") != std::string::npos);
}

TEST_CASE("Real textures from a classic zone decode" * doctest::skip(!hailstone::test::game_dir())) {
    auto archive = PfsArchive::open(hailstone::test::game_dir()->find("qeynos2.s3d").value());
    int count = 0;
    for (const auto& name : archive.names()) {
        if (!name.ends_with(".bmp") && !name.ends_with(".dds")) continue;
        auto data = archive.read(name);
        REQUIRE(data);
        std::string error;
        auto img = decode_texture(*data, {}, &error);
        INFO(name, ": ", error);
        REQUIRE(img);
        auto rgba = to_rgba8(*img);
        CHECK(rgba.mips.size() == img->mips.size());
        CHECK(rgba.mips[0].size() == std::size_t(img->width) * img->height * 4);
        ++count;
    }
    CHECK(count > 50);
}

TEST_CASE("Real textures from an EQG archive decode" * doctest::skip(!hailstone::test::game_dir())) {
    auto archive = PfsArchive::open(hailstone::test::game_dir()->find("broodlands.eqg").value());
    auto data = archive.read("swmp_canopy_trim.dds");
    REQUIRE(data);
    auto img = decode_ok(*data);
    CHECK(img.format == PixelFormat::BC1);
    CHECK(img.width == 256);
    CHECK(img.height == 256);
    // OpenEQ DDS_BC1_ALPHA.md: 22,977 transparent base texels.
    auto rgba = to_rgba8(img);
    int transparent = 0;
    for (std::size_t i = 3; i < rgba.mips[0].size(); i += 4) transparent += rgba.mips[0][i] == std::byte{0};
    CHECK(transparent == 22977);
}

TEST_CASE("4-bit BMP") {
    // 3x1: indices 1, 2, 0 packed high nibble first, row padded to 4 bytes.
    auto img = decode_ok(bmp(3, 1, 4, {0x000000, 0xff0000, 0x00ff00}, {0x12, 0x00, 0, 0}));
    CHECK(pixel(img, 0, 0, 0) == Px{255, 0, 0, 255});
    CHECK(pixel(img, 0, 1, 0) == Px{0, 255, 0, 255});
    CHECK(pixel(img, 0, 2, 0) == Px{0, 0, 0, 255});
}

TEST_CASE("Signed Q8W8V8U8 DDS is stored biased by 128") {
    auto w = dds_header({.width = 1, .height = 1, .flags = 0x4, .bits = 0});
    // Overwrite the FourCC field (offset 84) with D3DFMT_Q8W8V8U8 = 63.
    auto data = w.take();
    data[84] = std::byte{63};
    ByteWriter px;
    px.le<std::uint32_t>(0x007f8130); // U=0x30 V=0x81 W=0x7f Q=0x00
    for (auto b : px.data()) data.push_back(b);
    auto img = decode_ok(data);
    CHECK(pixel(img, 0, 0, 0) == Px{0xb0, 0x01, 0xff, 0x80});
}

TEST_CASE("TGA truecolour and RLE") {
    auto header = [](std::uint8_t type, std::uint16_t w, std::uint16_t h, std::uint8_t bits, std::uint8_t desc) {
        ByteWriter t;
        t.le<std::uint8_t>(0);
        t.le<std::uint8_t>(0);
        t.le<std::uint8_t>(type);
        t.zeros(9);
        t.le(w);
        t.le(h);
        t.le(bits);
        t.le(desc);
        return t;
    };
    SUBCASE("type 2, 32-bit, bottom-up") {
        auto t = header(2, 2, 2, 32, 8);
        for (std::uint32_t v : {0x80ff0000u, 0x8000ff00u, 0x400000ffu, 0x00ffffffu}) t.le(v); // BGRA
        auto img = decode_ok(t.take());
        CHECK(pixel(img, 0, 0, 1) == Px{255, 0, 0, 128});
        CHECK(pixel(img, 0, 1, 1) == Px{0, 255, 0, 128});
        CHECK(pixel(img, 0, 0, 0) == Px{0, 0, 255, 64});
        CHECK(pixel(img, 0, 1, 0) == Px{255, 255, 255, 0});
    }
    SUBCASE("type 10, 24-bit RLE, top-down") {
        auto t = header(10, 3, 1, 24, 0x20);
        t.le<std::uint8_t>(0x81); // run of 2
        for (int b : {0, 0, 255}) t.le(static_cast<std::uint8_t>(b));
        t.le<std::uint8_t>(0x00); // raw 1
        for (int b : {255, 0, 0}) t.le(static_cast<std::uint8_t>(b));
        auto img = decode_ok(t.take());
        CHECK(pixel(img, 0, 0, 0) == Px{255, 0, 0, 255});
        CHECK(pixel(img, 0, 1, 0) == Px{255, 0, 0, 255});
        CHECK(pixel(img, 0, 2, 0) == Px{0, 0, 255, 255});
    }
    SUBCASE("truncated") {
        auto t = header(2, 4, 4, 32, 8);
        t.zeros(4 * 15);
        CHECK_FALSE(decode_texture(t.take()));
    }
}

TEST_CASE("Real TGA and 8-bit BMP decode" * doctest::skip(!hailstone::test::game_dir())) {
    auto sky = PfsArchive::open(hailstone::test::game_dir()->find("sky.s3d").value());
    auto tga = decode_ok(sky.read("glare.tga").value());
    CHECK(tga.format == PixelFormat::RGBA8);
    auto chr = PfsArchive::open(hailstone::test::game_dir()->find("gfaydark_chr.s3d").value());
    auto data = chr.read("batch0001.bmp").value();
    CHECK(describe_texture(data) == "BMP 8-bit");
    auto img = decode_ok(data);
    CHECK(img.width == 64);
    CHECK(img.height == 128);
    CHECK(img.mips.size() == 1);
}

TEST_CASE("build_color_mips: linear filtering, alpha-weighted colour, cut-out coverage") {
    auto rgba = [](std::uint32_t w, std::uint32_t h, auto texel) {
        Image img;
        img.width = w;
        img.height = h;
        std::vector<std::byte> level(std::size_t(w) * h * 4);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                Px p = texel(x, y);
                for (int k = 0; k < 4; ++k) level[(std::size_t(y) * w + x) * 4 + k] = std::byte(p[k]);
            }
        img.mips.push_back(std::move(level));
        return img;
    };

    SUBCASE("a full chain, averaged in linear light") {
        // Black and white checks average to linear 0.5, sRGB 188 (a gamma-space average would give 128).
        Image img = rgba(4, 2, [](std::uint32_t x, std::uint32_t y) {
            int v = (x + y) % 2 ? 255 : 0;
            return Px{v, v, v, 255};
        });
        build_color_mips(img);
        REQUIRE(img.mips.size() == 3); // 4x2, 2x1, 1x1
        CHECK(pixel(img, 1, 0, 0) == Px{188, 188, 188, 255});
        CHECK(pixel(img, 2, 0, 0) == Px{188, 188, 188, 255});
    }

    SUBCASE("transparent texels give no colour") {
        // Opaque green beside transparent magenta (a palette index 0 cut-out): the mip and level 0's hidden texel
        // are green, not pink.
        Image img = rgba(2, 2, [](std::uint32_t x, std::uint32_t) {
            return x == 0 ? Px{0, 200, 0, 255} : Px{255, 0, 255, 0};
        });
        build_color_mips(img);
        CHECK(pixel(img, 0, 1, 0) == Px{0, 200, 0, 0});
        Px m = pixel(img, 1, 0, 0);
        CHECK(m[0] == 0);
        CHECK(m[1] == 200);
        CHECK(m[2] == 0);
    }

    SUBCASE("a DDS's colour under alpha 0 is kept when it is not a key") {
        // generic_plate_blue_c.dds (the hero's forge plates): blue cloth at alpha 255 beside plate drawn at alpha 0
        // by an Opaque_* material. Not grown over, level 0 keeps the plate's colour.
        Image img = rgba(2, 2, [](std::uint32_t x, std::uint32_t) {
            return x == 0 ? Px{20, 30, 90, 255} : Px{150, 160, 170, 0};
        });
        build_color_mips(img, 0.5f, false);
        CHECK(pixel(img, 0, 1, 0) == Px{150, 160, 170, 0});
        CHECK(pixel(img, 0, 1, 1) == Px{150, 160, 170, 0});
    }

    SUBCASE("a colour key is taken out of the edges and the clear texels") {
        // dstree312.dds's way: leaves (100, 110, 40) on a cyan (0, 170, 156) background, the edge texels blended over
        // it. The cyan leaves level 0, the soft edge and the mips.
        const Px leaf{100, 110, 40, 255}, cyan{0, 170, 156, 0};
        auto over = [&](int a) {
            Px p{0, 0, 0, a};
            for (int k = 0; k < 3; ++k) p[k] = (leaf[k] * a + cyan[k] * (255 - a) + 127) / 255;
            return p;
        };
        Image img = rgba(8, 8, [&](std::uint32_t x, std::uint32_t) {
            return x < 3 ? leaf : x == 3 ? over(224) : x == 4 ? over(64) : cyan;
        });
        CHECK(remove_color_key(img));
        build_color_mips(img, 0.5f, false);
        auto green = [&](Px p) { return std::abs(p[0] - leaf[0]) <= 6 && std::abs(p[1] - leaf[1]) <= 6 && std::abs(p[2] - leaf[2]) <= 6; };
        CHECK(green(pixel(img, 0, 3, 0)));
        CHECK(pixel(img, 0, 3, 0)[3] == 224);
        CHECK(green(pixel(img, 0, 4, 0)));
        CHECK(green(pixel(img, 0, 5, 0)));
        CHECK(pixel(img, 0, 5, 0)[3] == 0);
        CHECK(green(pixel(img, 1, 1, 0)));
    }

    SUBCASE("a picture under alpha 0 is not a key") {
        // Many colours under alpha 0 (a plate drawn by an opaque material): kept.
        Image img = rgba(8, 8, [](std::uint32_t x, std::uint32_t y) {
            return x < 2 ? Px{20, 30, 90, 255} : Px{int(x * 30), int(y * 30), 100, 0};
        });
        Image before = img;
        CHECK_FALSE(remove_color_key(img));
        CHECK(img.mips[0] == before.mips[0]);
    }

    SUBCASE("a material that never reads alpha draws the colour under it") {
        // make_opaque, then the chain: the plate's colour reaches every level, averaged with the cloth's.
        Image img = rgba(2, 2, [](std::uint32_t x, std::uint32_t) {
            return x == 0 ? Px{0, 0, 0, 255} : Px{255, 255, 255, 0};
        });
        make_opaque(img);
        build_color_mips(img);
        CHECK(pixel(img, 0, 1, 0) == Px{255, 255, 255, 255});
        CHECK(pixel(img, 1, 0, 0) == Px{188, 188, 188, 255});
    }

    SUBCASE("a cut-out keeps its coverage") {
        // A quarter of an 8x8 texture is opaque in scattered texels; plain averaging would leave every 2x2 mip texel
        // at alpha 0.25, under the 0.5 reference, and the leaves would vanish.
        Image img = rgba(8, 8, [](std::uint32_t x, std::uint32_t y) {
            return (x % 2 == 0 && y % 2 == 0) ? Px{10, 120, 10, 255} : Px{0, 0, 0, 0};
        });
        build_color_mips(img);
        REQUIRE(img.mips.size() == 4);
        int above = 0;
        for (std::uint32_t y = 0; y < 4; ++y)
            for (std::uint32_t x = 0; x < 4; ++x) above += pixel(img, 1, x, y)[3] >= 128;
        CHECK(above >= 4);
    }

    SUBCASE("other formats are left alone") {
        Image img;
        img.width = img.height = 4;
        img.format = PixelFormat::BC1;
        img.mips.push_back(std::vector<std::byte>(8));
        build_color_mips(img);
        CHECK(img.mips.size() == 1);
    }
}

TEST_CASE("octahedral maps round-trip directions") {
    for (glm::vec3 d : {glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::vec3(1, 0, 0), glm::vec3(0.3f, -0.5f, 0.8f),
                        glm::vec3(-0.6f, 0.2f, -0.7f), glm::vec3(0.1f, 0.9f, -0.1f)}) {
        d = glm::normalize(d);
        glm::vec3 back = octahedral_direction(octahedral_uv(d));
        CHECK(glm::dot(back, d) == doctest::Approx(1.0f).epsilon(1e-4));
    }
    CHECK(octahedral_uv(glm::vec3(0, 0, 1)).x == doctest::Approx(0.5f));
}

TEST_CASE("a DDS cube map becomes an octahedral map, each direction from its face") {
    // Six 4x4 faces (+X, -X, +Y, -Y, +Z, -Z), each one flat colour.
    const std::array<Px, 6> colors{{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255},
                                    {255, 255, 0, 255}, {255, 255, 255, 255}, {0, 0, 0, 255}}};
    ByteWriter w = dds_header({.r = 0xff, .g = 0xff00, .b = 0xff0000, .a = 0xff000000u});
    for (const Px& c : colors)
        for (int i = 0; i < 16; ++i)
            for (int k = 0; k < 4; ++k) w.le<std::uint8_t>(static_cast<std::uint8_t>(c[static_cast<std::size_t>(k)]));
    std::vector<std::byte> data = w.take();
    CHECK_FALSE(decode_cube_as_octahedral(data, 16).has_value()); // no cube bit
    data[113] = std::byte{0xfe};                                   // DDSCAPS2 0xfe00: a cube with all six faces
    auto oct = decode_cube_as_octahedral(data, 16);
    REQUIRE(oct);
    CHECK(oct->width == 16);
    auto at = [&](glm::vec3 d) {
        glm::vec2 uv = octahedral_uv(glm::normalize(d));
        return pixel(*oct, 0, std::min(15u, static_cast<std::uint32_t>(uv.x * 16.0f)),
                     std::min(15u, static_cast<std::uint32_t>(uv.y * 16.0f)));
    };
    // The cubes stand with +Y up: the world's up (+z) is the +Y face, the world's +y the +Z face.
    CHECK(at({0, 0, 1}) == colors[2]);
    CHECK(at({0, 0, -1}) == colors[3]);
    CHECK(at({1, 0.1f, 0.1f}) == colors[0]);
    CHECK(at({-1, 0.1f, 0.1f}) == colors[1]);
    CHECK(at({0.1f, 1, 0.1f}) == colors[4]);
    CHECK(at({0.1f, -1, -0.1f}) == colors[5]);
}

TEST_CASE("is_opaque tells a texture that needs no coverage-aware chain from one that does") {
    Image img;
    img.width = 2;
    img.height = 1;
    img.format = PixelFormat::RGBA8;
    img.mips.push_back(std::vector<std::byte>(8, std::byte{0xff}));
    CHECK(is_opaque(img));
    img.mips[0][7] = std::byte{0x00}; // one clear texel
    CHECK_FALSE(is_opaque(img));
    img.mips[0][7] = std::byte{0x80}; // half clear
    CHECK_FALSE(is_opaque(img));
    img.mips[0][7] = std::byte{0xff};

    // A compressed image is not one the chain rule applies to: it keeps the compression it was stored in.
    Image bc;
    bc.width = 4;
    bc.height = 4;
    bc.format = PixelFormat::BC3;
    bc.mips.push_back(std::vector<std::byte>(32, std::byte{0xff}));
    CHECK_FALSE(is_opaque(bc));
}

TEST_CASE("An opaque texture goes up with the GPU building its mips; a cut-out one keeps the CPU's chain") {
    Image opaque;
    opaque.width = 4;
    opaque.height = 4;
    opaque.format = PixelFormat::RGBA8;
    opaque.mips.push_back(std::vector<std::byte>(4 * 4 * 4, std::byte{0xff}));

    // Left alone: the upload generates the levels itself (render/gpu.cpp UploadBatch::texture).
    CHECK_FALSE(prepare_texture_image(opaque, TextureUse::Color).has_value());
    CHECK_FALSE(prepare_texture_image(opaque, TextureUse::OpaqueColor).has_value());

    // A cut-out keeps the CPU's chain: its coverage is scaled so alpha-tested foliage does not thin out.
    Image cutout = opaque;
    cutout.mips[0][3] = std::byte{0x00};
    auto prepared = prepare_texture_image(cutout, TextureUse::Color);
    REQUIRE(prepared.has_value());
    CHECK(prepared->mips.size() > 1); // 4x4, 2x2, 1x1
    CHECK(prepared->mips[1].size() == 2 * 2 * 4);

    // A texture with a chain of its own keeps it (a game DDS with mips).
    Image with_mips = opaque;
    with_mips.mips.push_back(std::vector<std::byte>(2 * 2 * 4, std::byte{0xff}));
    CHECK_FALSE(prepare_texture_image(with_mips, TextureUse::Color).has_value());
}

namespace {

Image opaque_image(std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    Image img;
    img.width = w;
    img.height = h;
    img.format = PixelFormat::RGBA8;
    std::vector<std::byte> px(std::size_t(w) * h * 4);
    std::uint32_t x = seed;
    for (std::size_t i = 0; i < px.size(); ++i) {
        x = x * 1664525u + 1013904223u; // a fixed pseudo-random pattern, high contrast between neighbours
        px[i] = (i % 4 == 3) ? std::byte{0xff} : static_cast<std::byte>(x >> 24);
    }
    img.mips.push_back(std::move(px));
    return img;
}

double to_linear(int c) {
    const double v = c / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

int to_srgb(double v) {
    v = std::clamp(v, 0.0, 1.0);
    const double s = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return static_cast<int>(std::lround(s * 255.0));
}

} // namespace

TEST_CASE("The GPU builds a texture's mips only where they come out as the CPU's linear-light chain") {
    using hailstone::render::gpu_builds_mips;
    CHECK(gpu_builds_mips(opaque_image(16, 8, 1), TextureUse::Color));
    CHECK(gpu_builds_mips(opaque_image(16, 8, 1), TextureUse::OpaqueColor));
    CHECK(gpu_builds_mips(opaque_image(1, 8, 1), TextureUse::Color));
    CHECK_FALSE(gpu_builds_mips(opaque_image(16, 8, 1), TextureUse::Data)); // data: its own (UNORM) blit
    CHECK_FALSE(gpu_builds_mips(opaque_image(1, 1, 1), TextureUse::Color)); // nothing to build
    // An odd size: the CPU's 2x2 box and a 2:1 linear blit read different texels, so the CPU keeps it.
    auto npot = opaque_image(12, 8, 2);
    CHECK_FALSE(gpu_builds_mips(npot, TextureUse::Color));
    auto chain = prepare_texture_image(npot, TextureUse::Color);
    REQUIRE(chain.has_value());
    CHECK(chain->mips.size() == 4); // 12x8, 6x4, 3x2, 1x1
    auto cutout = opaque_image(16, 8, 3);
    cutout.mips[0][3] = std::byte{0};
    CHECK_FALSE(gpu_builds_mips(cutout, TextureUse::Color));
    // Asked to, prepare_texture_image builds the chain the GPU would have built.
    auto pot = opaque_image(16, 8, 4);
    CHECK_FALSE(prepare_texture_image(pot, TextureUse::Color).has_value());
    REQUIRE(prepare_texture_image(pot, TextureUse::Color, false).has_value());
}

TEST_CASE("Immutable external textures finish colour mips before the UNORM upload") {
    // The private path leaves this eligible opaque chain for an sRGB image blit. The shared path exports an UNORM
    // image, so its actual preparation entry point must supply final linear-light mip bytes instead.
    const auto source = opaque_image(16, 8, 37);
    CHECK(hailstone::render::gpu_builds_mips(source, TextureUse::Color));
    CHECK_FALSE(prepare_texture_image(source, TextureUse::Color).has_value());
    const auto prepared = hailstone::render::prepare_immutable_texture_image(source, TextureUse::Color, true);
    REQUIRE(prepared.has_value());
    Image expected = source;
    build_color_mips(expected);
    CHECK(prepared->format == expected.format);
    CHECK(prepared->width == expected.width);
    CHECK(prepared->height == expected.height);
    CHECK(prepared->mips == expected.mips);
    CHECK(prepared->mips.size() == 5);
    CHECK(source.mips.size() == 1); // canonicalization does not alter the retained asset image
    CHECK_FALSE(hailstone::render::prepare_immutable_texture_image(source, TextureUse::Data, true).has_value());

    Image bc;
    bc.width = bc.height = 4;
    bc.format = hailstone::assets::PixelFormat::BC1;
    bc.mips.emplace_back(8, std::byte{0}); // one synthetic BC1 block
    CHECK_FALSE(hailstone::render::prepare_immutable_texture_image(bc, TextureUse::Data, true).has_value());
    const auto decoded = hailstone::render::prepare_immutable_texture_image(bc, TextureUse::Data, false);
    REQUIRE(decoded.has_value());
    CHECK(decoded->format == hailstone::assets::PixelFormat::RGBA8);
    CHECK(decoded->mips == hailstone::assets::to_rgba8(bc).mips);
    CHECK(bc.format == hailstone::assets::PixelFormat::BC1);
}

TEST_CASE("An sRGB blit chain of an opaque texture matches assets::build_color_mips") {
    // What vkCmdBlitImage does on the R8G8B8A8_SRGB image (render/gpu.cpp UploadBatch::texture): each level from the
    // one above, each texel the 2x2 average of its sources decoded to linear, encoded back to 8-bit sRGB. The CPU
    // chain keeps full precision between levels, so a level may differ by a rounding step; gamma-space averaging (the
    // UNORM blit) is off by far more on this high-contrast pattern.
    for (auto [w, h] : {std::pair{16u, 16u}, std::pair{32u, 8u}, std::pair{4u, 64u}}) {
        const auto source = opaque_image(w, h, w * 31 + h);
        REQUIRE(hailstone::render::gpu_builds_mips(source, TextureUse::Color));
        Image cpu = source;
        build_color_mips(cpu);
        std::vector<std::vector<int>> levels{{}};
        for (auto b : source.mips[0]) levels[0].push_back(std::to_integer<int>(b));
        std::uint32_t lw = w, lh = h;
        int worst = 0, worst_gamma = 0;
        while (lw > 1 || lh > 1) {
            const std::uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
            const auto& prev = levels.back();
            std::vector<int> next(std::size_t(nw) * nh * 4);
            for (std::uint32_t y = 0; y < nh; ++y)
                for (std::uint32_t x = 0; x < nw; ++x)
                    for (int k = 0; k < 4; ++k) {
                        double lin = 0.0, gamma = 0.0;
                        int n = 0;
                        for (std::uint32_t sy = y * 2; sy < std::min(lh, y * 2 + 2); ++sy)
                            for (std::uint32_t sx = x * 2; sx < std::min(lw, x * 2 + 2); ++sx, ++n) {
                                const int c = prev[(std::size_t(sy) * lw + sx) * 4 + std::size_t(k)];
                                lin += k == 3 ? c / 255.0 : to_linear(c);
                                gamma += c;
                            }
                        const int blit = k == 3 ? static_cast<int>(std::lround(lin / n * 255.0)) : to_srgb(lin / n);
                        next[(std::size_t(y) * nw + x) * 4 + std::size_t(k)] = blit;
                        const std::size_t level = levels.size();
                        const int want = std::to_integer<int>(cpu.mips.at(level).at((std::size_t(y) * nw + x) * 4 + std::size_t(k)));
                        worst = std::max(worst, std::abs(blit - want));
                        if (level == 1)
                            worst_gamma = std::max(worst_gamma, std::abs(static_cast<int>(std::lround(gamma / n)) - want));
                    }
            levels.push_back(std::move(next));
            lw = nw;
            lh = nh;
        }
        CHECK(levels.size() == cpu.mips.size());
        CHECK(worst <= 2);
        CHECK(worst_gamma > 8); // the difference the sRGB view is there to avoid
    }
}

TEST_CASE("A capped texture gets its chain on the CPU and loses the levels above the cap, opaque or not") {
    using hailstone::render::prepare_capped_texture;
    auto image = opaque_image(64, 32, 5);
    auto use = TextureUse::Color;
    prepare_capped_texture(image, use, 16);
    CHECK(use == TextureUse::Data);
    CHECK(image.width == 16);
    CHECK(image.height == 8);
    CHECK(image.mips.size() == 5); // 16x8 .. 1x1
    // No cap: left as it is, for the upload to build the chain.
    auto plain = opaque_image(64, 32, 5);
    auto plain_use = TextureUse::Color;
    prepare_capped_texture(plain, plain_use, 0);
    CHECK(plain_use == TextureUse::Color);
    CHECK(plain.mips.size() == 1);
    CHECK(plain.width == 64);
}
