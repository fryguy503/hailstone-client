// assets/png_read.h and assets/texture_overrides.h: replacement textures.
#include <doctest/doctest.h>

#include <array>
#include <cstdio>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <zlib.h>

#include "assets/png_read.h"
#include "assets/texture_overrides.h"
#include "core/byte_io.h"
#include "core/png_write.h"
#include "fuzz_support.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {

using Px = std::array<int, 4>;

Px pixel(const Image& img, std::uint32_t x, std::uint32_t y) {
    std::size_t i = (std::size_t(y) * img.width + x) * 4;
    const auto& m = img.mips.at(0);
    return {std::to_integer<int>(m.at(i)), std::to_integer<int>(m.at(i + 1)), std::to_integer<int>(m.at(i + 2)),
            std::to_integer<int>(m.at(i + 3))};
}

void put32(std::vector<unsigned char>& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<unsigned char>(v >> s));
}

void chunk(std::vector<unsigned char>& out, const char* type, const std::vector<unsigned char>& body) {
    put32(out, static_cast<std::uint32_t>(body.size()));
    std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    put32(out, static_cast<std::uint32_t>(crc32(0, out.data() + start, static_cast<uInt>(out.size() - start))));
}

// A PNG of the given header and filtered scanlines (each starting with its filter type), plus extra chunks before
// the data.
std::vector<std::byte> png(std::uint32_t w, std::uint32_t h, int depth, int color, int interlace,
                           const std::vector<unsigned char>& scanlines,
                           const std::vector<std::pair<const char*, std::vector<unsigned char>>>& extra = {}) {
    std::vector<unsigned char> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<unsigned char> ihdr;
    put32(ihdr, w);
    put32(ihdr, h);
    ihdr.insert(ihdr.end(), {static_cast<unsigned char>(depth), static_cast<unsigned char>(color), 0, 0,
                             static_cast<unsigned char>(interlace)});
    chunk(out, "IHDR", ihdr);
    for (const auto& [type, body] : extra) chunk(out, type, body);
    uLongf size = compressBound(static_cast<uLong>(scanlines.size()));
    std::vector<unsigned char> packed(size);
    compress(packed.data(), &size, scanlines.data(), static_cast<uLong>(scanlines.size()));
    packed.resize(size);
    chunk(out, "IDAT", packed);
    chunk(out, "IEND", {});
    std::vector<std::byte> bytes(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) bytes[i] = static_cast<std::byte>(out[i]);
    return bytes;
}

// A 24-bit BMP, `rows` given top first (stored bottom first, as BMPs are).
std::vector<std::byte> bmp(std::uint32_t w, const std::vector<std::vector<Px>>& rows) {
    std::uint32_t h = static_cast<std::uint32_t>(rows.size());
    std::uint32_t stride = (w * 3 + 3) & ~3u;
    std::vector<unsigned char> out;
    auto le32 = [&](std::uint32_t v) { for (int s = 0; s < 32; s += 8) out.push_back(static_cast<unsigned char>(v >> s)); };
    auto le16 = [&](std::uint16_t v) { out.push_back(static_cast<unsigned char>(v & 0xff)); out.push_back(static_cast<unsigned char>(v >> 8)); };
    out.push_back('B');
    out.push_back('M');
    le32(54 + stride * h);
    le32(0);
    le32(54);
    le32(40);
    le32(w);
    le32(h);
    le16(1);
    le16(24);
    le32(0);
    le32(stride * h);
    le32(2835);
    le32(2835);
    le32(0);
    le32(0);
    for (std::uint32_t y = h; y-- > 0;) {
        for (const Px& p : rows[y]) out.insert(out.end(), {static_cast<unsigned char>(p[2]), static_cast<unsigned char>(p[1]),
                                                           static_cast<unsigned char>(p[0])});
        out.resize(out.size() + (stride - w * 3), 0);
    }
    std::vector<std::byte> bytes(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) bytes[i] = static_cast<std::byte>(out[i]);
    return bytes;
}

std::vector<std::byte> rgba_png(std::uint32_t w, const std::vector<std::vector<Px>>& rows) {
    std::vector<std::byte> px;
    for (const auto& row : rows)
        for (const Px& p : row)
            for (int c : p) px.push_back(static_cast<std::byte>(c));
    auto out = encode_png_rgba(px, w, static_cast<std::uint32_t>(rows.size()));
    std::vector<std::byte> bytes(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) bytes[i] = static_cast<std::byte>(out[i]);
    return bytes;
}

void write(const std::filesystem::path& p, const std::vector<std::byte>& data) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

const Px kRed{255, 0, 0, 255}, kBlue{0, 0, 255, 255}, kGreen{0, 255, 0, 255};

// A built pack's file: a 4x4 BC4 DDS (ATI1) with its 2x2 and 1x1 mips, every texel `value`.
std::vector<std::byte> bc4_dds(std::uint8_t value) {
    ByteWriter w;
    for (char c : std::string_view("DDS ")) w.le(static_cast<std::uint8_t>(c));
    w.le<std::uint32_t>(124);
    w.le<std::uint32_t>(0x21007);
    w.le<std::uint32_t>(4);
    w.le<std::uint32_t>(4);
    w.zeros(8);
    w.le<std::uint32_t>(3);
    w.zeros(44);
    w.le<std::uint32_t>(32);
    w.le<std::uint32_t>(0x4);
    for (char c : std::string_view("ATI1")) w.le(static_cast<std::uint8_t>(c));
    w.zeros(20);
    w.le<std::uint32_t>(0x1000);
    w.zeros(16);
    for (int level = 0; level < 3; ++level) {
        w.le(value);
        w.le(value);
        w.zeros(6);
    }
    return w.take();
}

int red_of(const Image& img, std::uint32_t x, std::uint32_t y) { return pixel(to_rgba8(img), x, y)[0]; }

} // namespace

TEST_CASE("png: RGBA round trip through the writer, and decode_texture takes it") {
    auto data = rgba_png(2, {{kRed, {10, 20, 30, 40}}, {kBlue, kGreen}});
    CHECK(is_png(data));
    auto img = decode_texture(data);
    REQUIRE(img);
    CHECK(img->width == 2);
    CHECK(img->height == 2);
    CHECK(pixel(*img, 0, 0) == kRed);
    CHECK(pixel(*img, 1, 0) == Px{10, 20, 30, 40});
    CHECK(pixel(*img, 0, 1) == kBlue);
    CHECK(describe_texture(data) == "PNG");
}

TEST_CASE("png: the filters, grey at 1 bit, a palette with tRNS, 16-bit RGB") {
    // RGB 8-bit, 2 x 2: row 0 Sub-filtered, row 1 Paeth-filtered.
    //   row 0: (10,20,30) (15,25,35)  -> sub: 10 20 30 | 5 5 5
    //   row 1: (12,22,32) (20,30,40)  -> paeth with a=left, b=up, c=up-left:
    //     x0: a=0,b=10,c=0 -> pred b=10 -> 2; ... computed below
    std::vector<unsigned char> lines = {1, 10, 20, 30, 5, 5, 5};
    {
        int up[6] = {10, 20, 30, 15, 25, 35}, cur[6] = {12, 22, 32, 20, 30, 40};
        lines.push_back(4);
        for (int i = 0; i < 6; ++i) {
            int a = i >= 3 ? cur[i - 3] : 0, b = up[i], c = i >= 3 ? up[i - 3] : 0;
            int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
            int pred = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            lines.push_back(static_cast<unsigned char>(cur[i] - pred));
        }
    }
    auto rgb = decode_png(png(2, 2, 8, 2, 0, lines));
    REQUIRE(rgb);
    CHECK(pixel(*rgb, 1, 0) == Px{15, 25, 35, 255});
    CHECK(pixel(*rgb, 0, 1) == Px{12, 22, 32, 255});
    CHECK(pixel(*rgb, 1, 1) == Px{20, 30, 40, 255});

    // Grey, 1 bit, 3 x 1: 1 0 1; Up and Average on a single row act on zeros above.
    auto grey = decode_png(png(3, 1, 1, 0, 0, {2, 0b10100000}));
    REQUIRE(grey);
    CHECK(pixel(*grey, 0, 0) == Px{255, 255, 255, 255});
    CHECK(pixel(*grey, 1, 0) == Px{0, 0, 0, 255});

    // A 2-bit palette with index 1 see-through.
    auto pal = decode_png(png(2, 1, 2, 3, 0, {0, 0b01100000}, {{"PLTE", {1, 2, 3, 4, 5, 6, 7, 8, 9}}, {"tRNS", {255, 0}}}));
    REQUIRE(pal);
    CHECK(pixel(*pal, 0, 0) == Px{4, 5, 6, 0});
    CHECK(pixel(*pal, 1, 0) == Px{7, 8, 9, 255});

    // 16-bit RGB keeps the high bytes; tRNS keys a colour out.
    auto deep = decode_png(png(2, 1, 16, 2, 0, {0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0, 1, 0, 2, 0, 3},
                               {{"tRNS", {0, 1, 0, 2, 0, 3}}}));
    REQUIRE(deep);
    CHECK(pixel(*deep, 0, 0) == Px{0x12, 0x56, 0x9a, 255});
    CHECK(pixel(*deep, 1, 0) == Px{0, 0, 0, 0});

    std::string why;
    CHECK_FALSE(decode_png(png(2, 1, 8, 0, 0, {9, 1, 2}), &why)); // filter type 9
    CHECK(why == "bad filter type");
}

TEST_CASE("png: a header the image data cannot fill, or too many pixels, is refused before it allocates") {
    std::string why;
    // 16384 x 16384 RGBA at 16 bits wants 2 GiB of scanlines from a few bytes of IDAT.
    CHECK_FALSE(decode_png(png(16384, 16384, 16, 6, 0, {0, 0, 0}), &why));
    CHECK(why == "bad size");
    // 8192 x 8192 is allowed by size, but 64 MiB of 1-bit rows cannot come from a dozen bytes of deflate.
    CHECK_FALSE(decode_png(png(8192, 8192, 1, 0, 0, {0, 0}), &why));
    CHECK(why == "image data too short for its size");
    CHECK_FALSE(decode_png(png(8193, 8192, 1, 0, 0, {0, 0}), &why));
    CHECK(why == "bad size");
    // A real 1024 x 1024 1-bit image of zeros: 128 KiB of rows deflate to about 150 bytes, within the ratio.
    std::vector<unsigned char> rows;
    for (int y = 0; y < 1024; ++y) {
        rows.push_back(0);
        rows.insert(rows.end(), 128, 0);
    }
    auto big = decode_png(png(1024, 1024, 1, 0, 0, rows), &why);
    REQUIRE(big);
    CHECK(pixel(*big, 1023, 1023) == Px{0, 0, 0, 255});
}

TEST_CASE("png: rows are inflated one at a time, the filtered stream never held whole") {
    // 4096 x 2048 RGBA at 16 bits: 64 MiB of filtered rows for a 32 MiB image. Decoding once held the rows twice
    // (inflated and unfiltered) besides the image; the allocation guard fails any single request over 64 MiB.
    const std::uint32_t w = 4096, h = 2048;
    std::vector<unsigned char> rows;
    rows.reserve((std::size_t{w} * 8 + 1) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        rows.push_back(y == 0 ? 0 : 2); // none, then up
        for (std::uint32_t x = 0; x < w * 8; ++x) rows.push_back(y == 0 ? static_cast<unsigned char>(x * 7) : 0);
    }
    auto file = png(w, h, 16, 6, 0, rows);
    rows = {};
    std::string why;
    hailstone::fuzz::arm_allocation_guard(true);
    std::optional<Image> img;
    bool huge = false;
    try {
        img = decode_png(file, &why);
    } catch (const hailstone::fuzz::HugeAllocation&) {
        huge = true;
    }
    hailstone::fuzz::arm_allocation_guard(false);
    CHECK_FALSE(huge);
    REQUIRE_MESSAGE(img, why);
    // Each channel's high byte: pixel x's bytes are 8x .. 8x + 7, so R, G, B, A are (8x) * 7, (8x + 2) * 7, ...
    CHECK(pixel(*img, 1, h - 1) == Px{56, 70, 84, 98});
    CHECK(pixel(*img, 0, 0) == Px{0, 14, 28, 42});
}

TEST_CASE("png: a stream that ends before the image, or runs on past it, is refused") {
    std::string why;
    std::vector<unsigned char> lines = {0, 1, 2, 0, 3, 4};
    REQUIRE(decode_png(png(2, 2, 8, 0, 0, lines)));
    auto short_lines = lines;
    short_lines.pop_back();
    CHECK_FALSE(decode_png(png(2, 2, 8, 0, 0, short_lines), &why));
    CHECK(why == "bad image data");
    auto long_lines = lines;
    long_lines.push_back(9);
    CHECK_FALSE(decode_png(png(2, 2, 8, 0, 0, long_lines), &why));
    CHECK(why == "bad image data");
}

TEST_CASE("png: Adam7 interlacing puts each pass's pixels back in place") {
    // 3 x 3 grey: value = 10 y + x. Passes (x0, y0, dx, dy): 1 (0,0,8,8): (0,0); 2 (4,0): none; 3 (0,4): none;
    // 4 (2,0,4,4): (2,0); 5 (0,2,2,4): (0,2) (2,2); 6 (1,0,2,2): (1,0) (1,2); 7 (0,1,1,2): row 1.
    std::vector<unsigned char> lines = {0, 0,      // pass 1
                                        0, 2,      // pass 4
                                        0, 20, 22, // pass 5
                                        0, 1, 0, 21, // pass 6: two rows of one pixel
                                        0, 10, 11, 12}; // pass 7
    auto img = decode_png(png(3, 3, 8, 0, 1, lines));
    REQUIRE(img);
    for (std::uint32_t y = 0; y < 3; ++y)
        for (std::uint32_t x = 0; x < 3; ++x) {
            int v = static_cast<int>(10 * y + x);
            CHECK(pixel(*img, x, y) == Px{v, v, v, 255});
        }
}

TEST_CASE("texture overrides: by name in any case and extension, a zone's own first, turned as the original") {
    hailstone::test::ScratchDir dir_scratch("texture_overrides_test");
    auto dir = dir_scratch.path();
    write(dir / "kbarkd.png", rgba_png(1, {{kRed}, {kBlue}}));
    write(dir / "gfaydark" / "KBARKD.PNG", rgba_png(1, {{kGreen}, {kGreen}}));
    write(dir / "pack" / "leaf.png", rgba_png(2, {{kRed, kRed}, {kRed, kRed}}));
    write(dir / "notes.txt", {});

    TextureOverrides& o = texture_overrides();
    o.set_folder(dir);
    REQUIRE(o.find("KBARKD.BMP"));
    CHECK(o.find("KBARKD.BMP")->filename() == "kbarkd.png");
    CHECK(o.find("kbarkd.dds", "gfaydark")->filename() == "KBARKD.PNG");
    CHECK(o.find("kbarkd.bmp", "qeynos2")->filename() == "kbarkd.png");
    CHECK(o.find("leaf.bmp"));
    CHECK_FALSE(o.find("notes.bmp"));
    CHECK_FALSE(o.find("other.bmp"));

    // The override is upright; a BMP original read for a WLD mesh has its rows bottom first, so the override is
    // turned the same way. Without the flag (a character's texture: the caller turns it) it stays upright.
    std::optional<std::vector<std::byte>> original = bmp(1, {{kGreen}, {kGreen}});
    std::size_t before = o.applied();
    auto wld = decode_game_texture("kbarkd.bmp", original, {.wld_bmp_rows = true});
    REQUIRE(wld);
    CHECK(pixel(*wld, 0, 0) == kBlue);
    CHECK(pixel(*wld, 0, 1) == kRed);
    auto plain = decode_game_texture("kbarkd.bmp", original);
    REQUIRE(plain);
    CHECK(pixel(*plain, 0, 0) == kRed);
    CHECK(o.applied() == before + 2);

    // A masked texture's fully opaque override takes the original's cut-out (its palette index 0) at its own size:
    // here the original is a 24-bit BMP, all opaque, so nothing changes; and no override: the original.
    auto masked = decode_game_texture("leaf.bmp", original, {.palette_index0_transparent = true});
    REQUIRE(masked);
    CHECK(masked->width == 2);
    CHECK(pixel(*masked, 1, 1) == kRed);
    auto none = decode_game_texture("rock.bmp", original);
    REQUIRE(none);
    CHECK(pixel(*none, 0, 0) == kGreen);
    std::string why;
    CHECK_FALSE(decode_game_texture("rock.bmp", std::optional<std::vector<std::byte>>{}, {}, &why));
    CHECK(why == "not found");

    const std::filesystem::path packs = std::filesystem::temp_directory_path() / "hs" / "textures";
    CHECK(texture_folder_setting("", packs) == packs / "default");
    CHECK(texture_folder_setting("OFF", packs).empty());
    CHECK(texture_folder_setting("hd", packs) == packs / "hd");
    const std::filesystem::path elsewhere = std::filesystem::temp_directory_path() / "packs" / "hd";
    CHECK(texture_folder_setting(elsewhere.string(), packs) == elsewhere);

    o.set_folder({});
    CHECK_FALSE(o.find("KBARKD.BMP"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("texture packs: by content hash before name, the masked variant, maps, files with mips used as stored") {
    hailstone::test::ScratchDir dir_scratch("texture_pack_test");
    auto dir = dir_scratch.path();
    const std::vector<std::byte> city1 = bmp(1, {{kRed}}), city2 = bmp(1, {{kBlue}});
    const std::uint64_t h1 = texture_content_hash(city1);
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h1));
    write(dir / "w1.dds", bc4_dds(10));
    write(dir / "by-hash" / (std::string(hex) + ".dds"), bc4_dds(20));
    write(dir / "by-hash" / (std::string(hex) + ".n.dds"), bc4_dds(30));
    write(dir / "leaf.dds", bc4_dds(40));
    write(dir / "leaf.m.dds", bc4_dds(50));
    write(dir / "leaf.r.dds", bc4_dds(60));

    TextureOverrides& o = texture_overrides();
    o.set_folder(dir);
    CHECK(o.has_hashed());
    CHECK(texture_content_hash({}) == 0xcbf29ce484222325ull);
    CHECK_FALSE(o.find(hex)); // by-hash/ files are found only by hash

    // The archive whose w1 hashes to the by-hash file gets it; another w1 gets the named one. Both keep their mips
    // and compression: a built pack's rows are already the WLD meshes'.
    std::optional<std::vector<std::byte>> a = city1, b = city2;
    auto first = decode_game_texture("w1.bmp", a, {.wld_bmp_rows = true});
    REQUIRE(first);
    CHECK(first->format == PixelFormat::BC4);
    CHECK(first->mips.size() == 3);
    CHECK(red_of(*first, 0, 0) == 20);
    auto second = decode_game_texture("w1.bmp", b, {.wld_bmp_rows = true});
    REQUIRE(second);
    CHECK(red_of(*second, 0, 0) == 10);
    // A caller wanting a BMP's rows upright gets it decoded and turned.
    auto upright = decode_game_texture("w1.bmp", b);
    REQUIRE(upright);
    CHECK(upright->format == PixelFormat::RGBA8);
    CHECK(upright->mips.size() == 1);

    // Masked materials take the .m variant of an 8-bit BMP's texture (here any BMP); others the plain one.
    CHECK(red_of(*decode_game_texture("leaf.bmp", b, {.palette_index0_transparent = true, .wld_bmp_rows = true}), 0, 0) == 50);
    CHECK(red_of(*decode_game_texture("leaf.bmp", b, {.wld_bmp_rows = true}), 0, 0) == 40);

    // Maps are found as the colour is.
    auto normal = decode_texture_map("w1.bmp", a, TextureMap::Normal, true);
    REQUIRE(normal);
    CHECK(red_of(*normal, 0, 0) == 30);
    CHECK_FALSE(decode_texture_map("w1.bmp", b, TextureMap::Normal, true));
    auto rough = decode_texture_map("LEAF.BMP", b, TextureMap::Roughness, true);
    REQUIRE(rough);
    CHECK(red_of(*rough, 0, 0) == 60);
    CHECK_FALSE(decode_texture_map("leaf.bmp", b, TextureMap::Normal, true));

    o.set_folder({});
    CHECK_FALSE(o.has_hashed());
    std::filesystem::remove_all(dir);
}

TEST_CASE("texture overrides: the Options window's texture pack choice") {
    hailstone::test::ScratchDir dir("texture_packs");
    const std::filesystem::path packs = dir.path() / "textures";
    std::filesystem::create_directories(packs / "hd");
    std::filesystem::create_directories(packs / "default");
    std::filesystem::create_directories(packs / "hd-high");
    std::ofstream(packs / "notes.txt") << "not a pack";
    const auto names = texture_pack_names(packs);
    CHECK(names == std::vector<std::string>{"default", "hd", "hd-high"});
    CHECK(texture_pack_names(dir.path() / "missing").empty());
    // the choice list is "Off", then the names
    CHECK(texture_pack_choice("off", names) == 0);
    CHECK(texture_pack_choice("OFF", names) == 0);
    CHECK(texture_pack_choice("", names) == 1); // the pack "default"
    CHECK(texture_pack_choice("hd-high", names) == 3);
    CHECK(texture_pack_choice("/somewhere/else", names) == -1);
    CHECK(texture_pack_choice("gone", names) == -1);
    CHECK(texture_pack_value(0, names) == "off");
    CHECK(texture_pack_value(1, names) == "");
    CHECK(texture_pack_value(2, names) == "hd");
    CHECK_FALSE(texture_pack_value(4, names));
    CHECK_FALSE(texture_pack_value(-1, names));
    // each choice stores what reads back as itself
    for (int c = 0; c <= 3; ++c) CHECK(texture_pack_choice(*texture_pack_value(c, names), names) == c);
}

TEST_CASE("texture overrides: the folder changes while other threads look textures up") {
    // A character's own settings change the pack at zone-in while models load on other threads: a lookup sees the
    // old index or the new one, never one being built or freed.
    hailstone::test::ScratchDir dir_scratch("texture_overrides_swap");
    const auto dir = dir_scratch.path();
    for (int i = 0; i < 300; ++i) write(dir / ("zone" + std::to_string(i % 7)) / ("tex" + std::to_string(i) + ".png"), {});
    TextureOverrides& o = texture_overrides();
    o.set_folder({});
    std::atomic<bool> done{false};
    std::atomic<int> found{0}, wrong{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t)
        readers.emplace_back([&, t] {
            for (int i = 0; !done; ++i) {
                const std::string name = "TEX" + std::to_string((i * 7 + t) % 300) + ".BMP";
                auto file = o.find(name, "zone" + std::to_string(i % 7));
                if (!file) continue;
                ++found;
                if (file->stem() != "tex" + std::to_string((i * 7 + t) % 300)) ++wrong;
            }
        });
    for (int i = 0; i < 40; ++i) o.set_folder(i % 2 ? std::filesystem::path() : dir);
    done = true;
    for (auto& r : readers) r.join();
    o.set_folder({});
    CHECK(found > 0);
    CHECK(wrong == 0);
}
