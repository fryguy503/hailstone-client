// Texture packs (src/texpack): the scan, the upscaler's rules (as tools/texpack/upscale.py had them), the pack files
// and the build job, with stand-in models (the ncnn ones need a GPU: docs/texstudio.md "Accuracy" says how they were
// checked against PyTorch).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>

#include <doctest/doctest.h>
#include <imgui.h>
#include <lua.hpp>

#include "assets/texture.h"
#include "core/build_info.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "fuzz_support.h"
#include "pfs_fixtures.h"
#include "scratch_dir.h"
#include "texpack/model_catalog.h"
#include "texpack/pack_job.h"
#include "texpack/texpack.h"
#include "texpack/upscale.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;
using namespace hailstone::texpack;
namespace fs = std::filesystem;

namespace {

// An 8-bit BMP of w x h whose texel (x, y) (top row first) is palette index f(x, y); index i is (i, 255 - i, i / 2).
template <typename F> std::vector<std::byte> bmp8(std::uint32_t w, std::uint32_t h, F f) {
    const std::uint32_t row = (w + 3) / 4 * 4;
    ByteWriter out;
    const std::uint32_t data_at = 14 + 40 + 256 * 4;
    out.le<std::uint8_t>('B');
    out.le<std::uint8_t>('M');
    out.le<std::uint32_t>(data_at + row * h);
    out.le<std::uint32_t>(0);
    out.le<std::uint32_t>(data_at);
    out.le<std::uint32_t>(40);
    out.le<std::int32_t>(static_cast<std::int32_t>(w));
    out.le<std::int32_t>(static_cast<std::int32_t>(h));
    out.le<std::uint16_t>(1);
    out.le<std::uint16_t>(8);
    out.le<std::uint32_t>(0);
    out.le<std::uint32_t>(row * h);
    out.le<std::int32_t>(2835);
    out.le<std::int32_t>(2835);
    out.le<std::uint32_t>(256);
    out.le<std::uint32_t>(0);
    for (int i = 0; i < 256; ++i) {
        out.le<std::uint8_t>(static_cast<std::uint8_t>(i / 2)); // b
        out.le<std::uint8_t>(static_cast<std::uint8_t>(255 - i)); // g
        out.le<std::uint8_t>(static_cast<std::uint8_t>(i));       // r
        out.le<std::uint8_t>(0);
    }
    for (std::uint32_t y = 0; y < h; ++y) { // bottom row first
        for (std::uint32_t x = 0; x < row; ++x)
            out.le<std::uint8_t>(x < w ? static_cast<std::uint8_t>(f(x, h - 1 - y)) : 0);
    }
    return out.take();
}

// The game texture's colour for palette index i (as bmp8 writes it).
std::array<std::uint8_t, 3> palette(int i) {
    return {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(255 - i), static_cast<std::uint8_t>(i / 2)};
}

// Nearest-neighbour by `scale`: a stand-in for an upscaler, exact enough to check the rules around it.
class Nearest final : public Model {
public:
    explicit Nearest(std::uint32_t scale) : scale_(scale) {}
    std::uint32_t scale() const override { return scale_; }
    Planes run(const Planes& rgb) override {
        ++calls;
        largest = std::max({largest, rgb.width, rgb.height});
        Planes out(rgb.width * scale_, rgb.height * scale_, 3);
        for (std::uint32_t y = 0; y < out.height; ++y)
            for (std::uint32_t x = 0; x < out.width; ++x)
                for (std::uint32_t c = 0; c < 3; ++c) out.at(x, y, c) = rgb.at(x / scale_, y / scale_, c);
        return out;
    }
    int calls = 0;
    std::uint32_t largest = 0;

private:
    std::uint32_t scale_;
};

class FakeSet final : public ModelSet {
public:
    Nearest color{4}, normal{1}, rough{1};
    Models models() override { return {&color, &normal, &rough}; }
};

TextureInfo picture(std::uint32_t w, std::uint32_t h, std::string name = "wall01.bmp", std::string archive = "gfaydark.s3d") {
    TextureInfo t;
    t.hash = 0x1234;
    t.width = w;
    t.height = h;
    t.stored = "BMP 8-bit";
    t.bmp = true;
    t.names = {std::move(name)};
    t.archives = {std::move(archive)};
    return t;
}

assets::Image solid(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    assets::Image img;
    img.width = w;
    img.height = h;
    std::vector<std::byte> px(std::size_t(w) * h * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = std::byte{r};
        px[i + 1] = std::byte{g};
        px[i + 2] = std::byte{b};
        px[i + 3] = std::byte{a};
    }
    img.mips.push_back(std::move(px));
    return img;
}

void write(const fs::path& p, const std::vector<std::byte>& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

template <typename Pred> bool wait_for(Pred pred, int ms = 20000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

} // namespace

TEST_CASE("texpack: hash names and manifest lines round-trip") {
    CHECK(hash_name(0x00022f4a68ca8630ull) == "00022f4a68ca8630");
    CHECK(parse_hash_name("00022f4a68ca8630") == 0x00022f4a68ca8630ull);
    CHECK_FALSE(parse_hash_name("22f4a68ca8630"));
    CHECK_FALSE(parse_hash_name("00022f4a68ca863g"));

    TextureInfo t = picture(256, 128, "with \"quote\".bmp");
    t.alpha = AlphaKind::Cutout;
    t.masked_variant = true;
    t.archives.insert("gequip.s3d");
    auto back = parse_manifest_line(manifest_line(t));
    REQUIRE(back);
    CHECK(back->hash == t.hash);
    CHECK(back->width == 256);
    CHECK(back->height == 128);
    CHECK(back->stored == "BMP 8-bit");
    CHECK(back->bmp);
    CHECK(back->alpha == AlphaKind::Cutout);
    CHECK(back->masked_variant);
    CHECK_FALSE(back->keyed_variant);
    CHECK(back->names == t.names);
    CHECK(back->archives == t.archives);
    // texpack export's own lines (tools/texpack/upscale.py reads them) parse too.
    auto old = parse_manifest_line(R"({"hash":"0000626c3529d7e8","w":256,"h":256,"stored":"DDS DXT1","bmp":false,)"
                                   R"("alpha":"none","masked_variant":false,"keyed_variant":false,"names":["knlamp04.bmp"],)"
                                   R"("archives":["karnor_obj.s3d"]})");
    REQUIRE(old);
    CHECK(old->names.contains("knlamp04.bmp"));
    CHECK_FALSE(parse_manifest_line("{\"hash\":\"x\"}"));
    CHECK_FALSE(parse_manifest_line("not json"));
    // No name, or an empty one: refused (pack_job names a texture by its first).
    CHECK_FALSE(parse_manifest_line(R"({"hash":"0000626c3529d7e8","w":256,"h":256,"names":[]})"));
    CHECK_FALSE(parse_manifest_line(R"({"hash":"0000626c3529d7e8","w":256,"h":256})"));
    CHECK_FALSE(parse_manifest_line(R"({"hash":"0000626c3529d7e8","w":256,"h":256,"names":[""]})"));
}

TEST_CASE("texpack: what the models run on, and the files a texture gets") {
    TextureInfo t = picture(256, 256);
    CHECK(uses_models(t));
    CHECK_FALSE(reflects_edges(t));
    CHECK(pack_files(t, true) == std::vector<FileKind>{FileKind::Color, FileKind::Normal, FileKind::Rough});
    CHECK(pack_files(t, false) == std::vector<FileKind>{FileKind::Color});
    t.masked_variant = t.keyed_variant = true;
    CHECK(pack_files(t, false) == std::vector<FileKind>{FileKind::Color, FileKind::Masked, FileKind::Keyed});

    CHECK_FALSE(uses_models(picture(16, 16)));        // tiny
    CHECK(uses_models(picture(17, 8)));
    CHECK(is_data(picture(64, 64, "terrain_coverage.dds")));
    CHECK(is_data(picture(64, 64, "rock_n.dds")));
    CHECK(is_data(picture(64, 64, "fx_spark.dds")));
    CHECK_FALSE(is_data(picture(64, 64, "fxwall.dds")));
    TextureInfo bump = picture(64, 64);
    bump.stored = "DDS Q8W8V8U8";
    CHECK(is_data(bump));
    CHECK(pack_files(picture(64, 64, "rock_n.dds"), true) == std::vector<FileKind>{FileKind::Color});

    CHECK(reflects_edges(picture(64, 64, "humface.bmp", "global_chr.s3d")));
    CHECK(reflects_edges(picture(64, 64, "it1.bmp", "chequip.s3d")));
    TextureInfo both = picture(64, 64, "x.bmp", "global_chr.s3d");
    both.archives.insert("gfaydark.s3d");
    CHECK_FALSE(reflects_edges(both));
}

TEST_CASE("texpack: padding as numpy's wrap and reflect") {
    Planes row(4, 1, 1);
    for (std::uint32_t x = 0; x < 4; ++x) row.at(x, 0, 0) = static_cast<float>(x);
    // (np.pad's results, the 4 texels padded by 6 and by 5)
    const std::vector<float> reflect = {0, 1, 2, 3, 2, 1, 0, 1, 2, 3, 2, 1, 0, 1, 2, 3};
    const std::vector<float> wrap = {3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0};
    Planes r = pad(row, 6, Edge::Reflect);
    REQUIRE(r.width == 16);
    REQUIRE(r.height == 13);
    for (std::uint32_t x = 0; x < 16; ++x) CHECK(r.at(x, 6, 0) == reflect[x]);
    Planes w = pad(row, 5, Edge::Wrap);
    REQUIRE(w.width == 14);
    for (std::uint32_t x = 0; x < 14; ++x) CHECK(w.at(x, 5, 0) == wrap[x]);
    Planes back = crop(w, 5, 5, 4, 1);
    CHECK(back.px == row.px);
}

TEST_CASE("texpack: Lanczos matches PIL's") {
    Planes a(7, 5, 1);
    for (std::uint32_t y = 0; y < 5; ++y)
        for (std::uint32_t x = 0; x < 7; ++x) a.at(x, y, 0) = static_cast<float>((x * 7 + y * 13) % 17) / 16.0f;
    // PIL: Image.fromarray(a, "F").resize((11, 8), Image.LANCZOS), clipped to 0..1
    Planes up = resize_lanczos(a, 11, 8);
    const float first[] = {0.0f, 0.090581f, 0.565933f, 0.921309f, 0.676875f, 0.300897f, 0.614001f, 0.559125f, 0.0f,
                           0.223227f, 0.59441f, 0.349506f};
    for (std::size_t i = 0; i < 12; ++i) CHECK(up.px[i] == doctest::Approx(first[i]).epsilon(1e-4));
    double sum = 0;
    for (float v : up.px) sum += v;
    CHECK(sum == doctest::Approx(42.34377).epsilon(1e-5));
    Planes down = resize_lanczos(a, 3, 2);
    const float d[] = {0.513103f, 0.454602f, 0.418981f, 0.506702f, 0.574466f, 0.500347f};
    for (std::size_t i = 0; i < 6; ++i) CHECK(down.px[i] == doctest::Approx(d[i]).epsilon(1e-4));
}

TEST_CASE("texpack: transparent texels take their neighbours' colour") {
    Planes rgb(5, 5, 3, 0.0f), alpha(5, 5, 1, 0.0f);
    rgb.at(2, 2, 0) = 1.0f; // one opaque red texel in the middle; the key colour round it is black
    alpha.at(2, 2, 0) = 1.0f;
    bleed(rgb, alpha);
    for (std::uint32_t y = 0; y < 5; ++y)
        for (std::uint32_t x = 0; x < 5; ++x) {
            CHECK(rgb.at(x, y, 0) == doctest::Approx(1.0f));
            CHECK(rgb.at(x, y, 1) == doctest::Approx(0.0f));
        }
    // all opaque: nothing changes
    Planes same(3, 3, 3, 0.25f), opaque(3, 3, 1, 1.0f);
    bleed(same, opaque);
    for (float v : same.px) CHECK(v == 0.25f);
}

TEST_CASE("texpack: a model runs in feathered tiles that meet without a seam") {
    Planes img(900, 500, 3);
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = 0; x < img.width; ++x)
            for (std::uint32_t c = 0; c < 3; ++c) img.at(x, y, c) = static_cast<float>((x * 3 + y * 5 + c * 7) % 101) / 100.0f;
    Nearest model(2);
    Planes out = run_tiled(model, img);
    CHECK(model.calls == 6); // 3 x 2 tiles of 384
    CHECK(model.largest == kTile);
    REQUIRE(out.width == 1800);
    REQUIRE(out.height == 1000);
    Nearest direct(2);
    Planes whole = direct.run(img);
    float worst = 0;
    for (std::size_t i = 0; i < out.px.size(); ++i) worst = std::max(worst, std::abs(out.px[i] - whole.px[i]));
    CHECK(worst < 1e-5f);
}

TEST_CASE("texpack: a texture's pack images") {
    FakeSet set;
    const Models models = set.models();
    SUBCASE("a picture: the colour at the pack's scale, with its maps") {
        TextureInfo t = picture(64, 32);
        Sources s{solid(64, 32, 200, 100, 50)};
        Outputs o = upscale_texture(t, s, models, {2, true}, pack_files(t, true));
        REQUIRE(o.color);
        CHECK(o.color->width == 128);
        CHECK(o.color->height == 64);
        // 4x down to 2x by Lanczos keeps a flat colour
        CHECK(std::to_integer<int>(o.color->mips[0][0]) == 200);
        CHECK(std::to_integer<int>(o.color->mips[0][1]) == 100);
        CHECK(std::to_integer<int>(o.color->mips[0][3]) == 255);
        REQUIRE(o.normal);
        REQUIRE(o.rough);
        CHECK(o.normal->width == 128);
        // roughness is grey: its first channel in all three
        CHECK(o.rough->mips[0][0] == o.rough->mips[0][1]);
        CHECK(o.rough->mips[0][0] == o.rough->mips[0][2]);
        // the colour model ran on the padded image (64 + 2 x 16), the maps on the padded 2x colour
        CHECK(set.color.largest == 96);
        CHECK(set.normal.largest == 160);
    }
    SUBCASE("at 4x the colour model's output is used as it is") {
        TextureInfo t = picture(32, 32);
        Outputs o = upscale_texture(t, {solid(32, 32, 10, 20, 30)}, models, {4, false}, pack_files(t, false));
        REQUIRE(o.color);
        CHECK(o.color->width == 128);
        CHECK_FALSE(o.normal);
    }
    SUBCASE("data and tiny textures are only scaled, without maps") {
        TextureInfo t = picture(16, 16);
        Outputs o = upscale_texture(t, {solid(16, 16, 1, 2, 3)}, models, {2, true}, {FileKind::Color, FileKind::Normal});
        REQUIRE(o.color);
        CHECK(o.color->width == 32);
        CHECK_FALSE(o.normal);
        CHECK(set.color.calls == 0);
    }
    SUBCASE("a cut-out's alpha stays 0 or 255") {
        TextureInfo t = picture(32, 32);
        t.alpha = AlphaKind::Cutout;
        assets::Image img = solid(32, 32, 50, 60, 70);
        for (std::size_t i = 0; i < img.mips[0].size() / 2; i += 4) img.mips[0][i + 3] = std::byte{0};
        Outputs o = upscale_texture(t, {img}, models, {2, false}, {FileKind::Color});
        REQUIRE(o.color);
        for (std::size_t i = 3; i < o.color->mips[0].size(); i += 4) {
            int a = std::to_integer<int>(o.color->mips[0][i]);
            CHECK((a == 0 || a == 255));
        }
    }
    SUBCASE("the masked variant's key colour is bled over before the model") {
        TextureInfo t = picture(32, 32);
        t.masked_variant = true;
        assets::Image m = solid(32, 32, 0, 255, 0); // the key, green
        for (std::size_t i = 0; i < m.mips[0].size(); i += 4) {
            if ((i / 4) % 32 < 16) {
                m.mips[0][i + 3] = std::byte{0};
            } else {
                m.mips[0][i] = std::byte{255};
                m.mips[0][i + 1] = std::byte{0};
            }
        }
        Sources s{solid(32, 32, 9, 9, 9), m};
        Outputs o = upscale_texture(t, s, models, {2, false}, {FileKind::Color, FileKind::Masked});
        REQUIRE(o.masked);
        for (std::size_t i = 0; i < o.masked->mips[0].size(); i += 4) CHECK(std::to_integer<int>(o.masked->mips[0][i + 1]) < 8);
    }
    SUBCASE("an output past kMaxUpscaleTexels is refused before anything is made") {
        TextureInfo t = picture(8192, 8192);
        assets::Image big; // only its size is read before the refusal
        big.width = big.height = 8192;
        CHECK_THROWS_AS(upscale_texture(t, {big}, models, {8, false}, {FileKind::Color}), std::runtime_error);
        CHECK(set.color.calls == 0);
    }
}

TEST_CASE("texpack: pack files are DX10 DDS with their mips, in the WLD meshes' rows") {
    assets::Image img = solid(64, 32, 255, 0, 0);
    // the top half blue: a BMP's file is stored bottom row first
    for (std::size_t i = 0; i < img.mips[0].size() / 2; i += 4) {
        img.mips[0][i] = std::byte{0};
        img.mips[0][i + 2] = std::byte{255};
    }
    auto file = encode_pack_file(img, FileKind::Color, true);
    auto decoded = assets::decode_texture(file);
    REQUIRE(decoded);
    CHECK(decoded->format == assets::PixelFormat::BC7);
    CHECK(decoded->width == 64);
    CHECK(decoded->mips.size() == 7);
    assets::Image rgba = assets::to_rgba8(*decoded);
    CHECK(std::to_integer<int>(rgba.mips[0][0]) > 200); // row 0 is the picture's bottom: red
    CHECK(std::to_integer<int>(rgba.mips[0][2]) < 50);

    // a BMP's normal map: its rows flipped and its green turned with them
    assets::Image n = solid(16, 16, 128, 64, 255);
    auto nf = assets::decode_texture(encode_pack_file(n, FileKind::Normal, true));
    REQUIRE(nf);
    CHECK(nf->format == assets::PixelFormat::BC5);
    assets::Image nr = assets::to_rgba8(*nf);
    CHECK(std::abs(std::to_integer<int>(nr.mips[0][1]) - 191) <= 2);
    auto rf = assets::decode_texture(encode_pack_file(solid(16, 16, 77, 77, 77), FileKind::Rough, false));
    REQUIRE(rf);
    CHECK(rf->format == assets::PixelFormat::BC4);
    CHECK(rf->mips.size() == 5);
}

TEST_CASE("texpack: scan, the scan's cache and a whole build") {
    test::ScratchDir dir("texpack");
    const fs::path assets = dir.path() / "assets";
    // a zone with two pictures (one masked: palette index 0 used), and a second archive with a copy of one
    auto wall = bmp8(64, 64, [](std::uint32_t x, std::uint32_t y) { return 10 + (x / 8 + y / 8) % 2 * 100; });
    auto leaf = bmp8(32, 32, [](std::uint32_t x, std::uint32_t) { return x < 8 ? 0 : 200; });
    auto tiny = bmp8(8, 8, [](std::uint32_t, std::uint32_t) { return 50; });
    write(assets / "testzone.s3d", test::pfs_fixtures::make_pfs({{"wall.bmp", wall}, {"leaf.bmp", leaf}, {"dot.bmp", tiny}}));
    write(assets / "testzone_obj.s3d", test::pfs_fixtures::make_pfs({{"wall2.bmp", wall}}));
    write(assets / "readme.txt", {std::byte{'x'}});

    const auto archives = list_archives({assets});
    REQUIRE(archives.size() == 2);
    ScanResult r = scan(archives, {.threads = 2});
    CHECK(r.errors.empty());
    REQUIRE(r.textures.size() == 3);
    const TextureInfo* w = nullptr;
    const TextureInfo* l = nullptr;
    for (const TextureInfo& t : r.textures) {
        if (t.names.contains("wall.bmp")) w = &t;
        if (t.names.contains("leaf.bmp")) l = &t;
    }
    REQUIRE(w);
    REQUIRE(l);
    CHECK(w->names == std::set<std::string>{"wall.bmp", "wall2.bmp"});
    CHECK(w->archives == std::set<std::string>{"testzone.s3d", "testzone_obj.s3d"});
    CHECK(w->source_archive == assets / "testzone.s3d"); // the first archive by path
    // whichever thread meets the copy first, and while the other is still decoding it
    for (int run = 0; run < 50; ++run) {
        ScanResult again = scan(archives, {.threads = 2});
        for (const TextureInfo& t : again.textures)
            if (t.names.contains("wall.bmp")) CHECK(t.source_archive == assets / "testzone.s3d");
    }
    CHECK(w->bmp);
    CHECK(w->width == 64);
    CHECK_FALSE(w->masked_variant);
    CHECK(l->masked_variant);
    auto sources = read_sources(*l, nullptr);
    REQUIRE(sources);
    REQUIRE(sources->masked);
    const auto c = palette(200);
    CHECK(std::to_integer<int>(sources->color.mips[0][8 * 4]) == c[0]); // texel (8, 0), upright

    SUBCASE("the cache holds while the archives are the same") {
        const fs::path cache = dir.path() / "cache" / "scan.txt";
        REQUIRE(save_scan(cache, archives, r.textures));
        auto back = load_scan(cache, archives);
        REQUIRE(back);
        REQUIRE(back->size() == 3);
        CHECK((*back)[0].source_archive == r.textures[0].source_archive);
        CHECK((*back)[0].source_name == r.textures[0].source_name);
        write(assets / "testzone_obj.s3d", test::pfs_fixtures::make_pfs({{"wall2.bmp", wall}, {"extra.bmp", tiny}}));
        CHECK_FALSE(load_scan(cache, archives)); // its size changed
        CHECK_FALSE(load_scan(cache, list_archives({assets}, {"testzone_obj"})));
    }

    SUBCASE("a build makes every file, and a second makes none") {
        const fs::path pack = dir.path() / "textures" / "hd";
        GpuWorker gpu([](std::string*) { return std::make_unique<FakeSet>(); });
        {
            JobOptions o;
            o.pack_dir = pack;
            o.textures = r.textures;
            o.settings = {2, true};
            o.encoders = 2;
            o.info.title = "hd";
            o.info.color_model = kPbrifyColor;
            o.info.tool_version = "texstudio 1.0.0"; // (a tool of its own names itself)
            PackJob job(o, gpu);
            REQUIRE(wait_for([&] { return job.finished(); }));
            JobStatus s = job.status();
            CHECK(s.state == JobStatus::State::Done);
            CHECK(s.total == 3);
            CHECK(s.done == 3);
            CHECK(s.failures == 0);
            // wall: colour, normal, rough; leaf: colour, masked, normal, rough; dot (tiny): colour
            CHECK(s.files_written == 8);
            CHECK(s.texels_done == doctest::Approx(s.texels_total));
        }
        const auto contents = pack_contents(pack);
        CHECK(contents.size() == 8);
        CHECK(contents.contains(hash_name(l->hash) + ".m"));
        auto info = read_pack_info(pack);
        REQUIRE(info);
        CHECK(info->textures == 3);
        CHECK(info->files == 8);
        CHECK(info->color_model == kPbrifyColor);
        CHECK(info->maps);
        CHECK(info->tool_version == "texstudio 1.0.0");
        auto bytes = read_file(pack / "by-hash" / (hash_name(w->hash) + ".dds"));
        REQUIRE(bytes);
        auto dds = assets::decode_texture(*bytes);
        REQUIRE(dds);
        CHECK(dds->width == 128);
        CHECK(dds->format == assets::PixelFormat::BC7);

        // nothing left to make
        JobOptions again;
        again.pack_dir = pack;
        again.textures = r.textures;
        again.settings = {2, true};
        PackJob second(again, gpu);
        REQUIRE(wait_for([&] { return second.finished(); }));
        CHECK(second.status().total == 0);
        CHECK(second.status().complete == 3);
        CHECK(read_pack_info(pack)->tool_version == release_version()); // none given: this build's
        // a removed file is made again, alone
        fs::remove(pack / "by-hash" / (hash_name(l->hash) + ".n.dds"));
        CHECK(missing_files(*l, true, pack_contents(pack)) == std::vector<FileKind>{FileKind::Normal});
    }

    SUBCASE("a build stops when cancelled, and failing models fail each texture") {
        GpuWorker broken([](std::string* why) -> std::unique_ptr<ModelSet> {
            *why = "no GPU here";
            return nullptr;
        });
        REQUIRE(wait_for([&] { return broken.failed(); }));
        CHECK(broken.error() == "no GPU here");
        JobOptions o;
        o.pack_dir = dir.path() / "textures" / "broken";
        o.textures = r.textures;
        PackJob job(o, broken);
        REQUIRE(wait_for([&] { return job.finished(); }));
        CHECK(job.status().failures == 3);
        CHECK(job.status().files_written == 0);
        CHECK(job.status().errors.size() == 3);
        CHECK(job.status().errors[0].find("no GPU here") != std::string::npos);
    }
}

TEST_CASE("texpack: urgent GPU tasks go first") {
    std::vector<int> order;
    std::mutex m;
    std::atomic<bool> release{false};
    GpuWorker gpu([](std::string*) { return std::make_unique<FakeSet>(); });
    gpu.post([&](const Models&) {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    for (int i = 1; i <= 2; ++i) gpu.post([&, i](const Models&) { std::lock_guard l(m); order.push_back(i); });
    gpu.post([&](const Models&) { std::lock_guard l(m); order.push_back(0); }, true);
    release = true;
    REQUIRE(wait_for([&] { std::lock_guard l(m); return order.size() == 3; }));
    CHECK(order == std::vector<int>{0, 1, 2});
}

TEST_CASE("texpack: the model catalogue reads each model's .ini, the player's over the shipped") {
    test::ScratchDir dir("texmodels");
    auto put = [&](const fs::path& folder, const std::string& id, const std::string& ini) {
        fs::create_directories(folder);
        std::ofstream(folder / (id + ".param")) << "7767517\n";
        std::ofstream(folder / (id + ".bin")) << "x";
        if (!ini.empty()) std::ofstream(folder / (id + ".ini")) << ini;
    };
    put(dir.path() / "shipped", kPbrifyColor, "title=PBRify SPAN 4x\nkind=color\nscale=4\nlicense=CC0-1.0\n");
    put(dir.path() / "shipped", kPbrifyNormal, "title=Normal\nkind=normal\n");
    put(dir.path() / "user", kPbrifyColor, "title=Mine\nkind=color\n");
    put(dir.path() / "user", "plain", "");
    std::ofstream(dir.path() / "user" / "orphan.param") << "no bin";
    auto models = list_models({dir.path() / "shipped", dir.path() / "user"});
    REQUIRE(models.size() == 3);
    const ModelEntry* c = find_model(models, kPbrifyColor);
    REQUIRE(c);
    CHECK(c->title == "Mine");
    CHECK(c->user);
    CHECK(c->scale == 0);
    const ModelEntry* p = find_model(models, "plain");
    REQUIRE(p);
    CHECK(p->kind == ModelKind::Color);
    CHECK(find_model(models, kPbrifyNormal)->kind == ModelKind::Normal);
    CHECK_FALSE(find_model(models, "orphan"));
}

TEST_CASE("Lua UI: entry scripts load alone, from anywhere in the pack") {
    test::ScratchDir dir("lua_entry");
    const fs::path pack = dir.path() / "pack";
    fs::create_directories(pack / "tools");
    fs::create_directories(pack / "lib");
    std::ofstream(pack / "game.lua") << "game_loaded = true\n";
    std::ofstream(pack / "lib" / "shared.lua") << "return { answer = 42 }\n";
    std::ofstream(pack / "tools" / "tool.lua") << "tool_answer = dofile('lib/shared.lua').answer\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        ui::lua::LuaUi lua(pack, dir.path() / "ui.txt");
        lua.set_entry_scripts({"tools/tool.lua", "tools/missing.lua"});
        ImGui::GetIO().DisplaySize = ImVec2(800, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        lua.frame();
        ImGui::Render();
        lua_State* L = lua.state();
        lua_getglobal(L, "tool_answer");
        CHECK(lua_tointeger(L, -1) == 42);
        lua_getglobal(L, "game_loaded");
        CHECK(lua_isnil(L, -1));
        lua_pop(L, 2);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("fuzz: texpack manifest lines and the scan cache") {
    fuzz::QuietLog quiet;
    TextureInfo t = picture(64, 32);
    t.names.insert("a\\b\"c.bmp");
    const std::string line = manifest_line(t);
    auto bytes_of = [](const std::string& s) {
        fuzz::Bytes b;
        for (char c : s) b.push_back(static_cast<std::byte>(c));
        return b;
    };
    fuzz::Rng rng(0x7e4);
    auto parse = [](std::span<const std::byte> d) {
        (void)parse_manifest_line(std::string_view(reinterpret_cast<const char*>(d.data()), d.size()));
    };
    const fuzz::Bytes seed = bytes_of(line);
    for (std::size_t n = 0; n <= seed.size(); ++n)
        fuzz::run_case("manifest line", fuzz::Bytes(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(n)),
                       fuzz::Throws::Never, parse);
    for (int i = 0; i < 600; ++i) fuzz::run_case("manifest line", fuzz::mutate(seed, rng), fuzz::Throws::Never, parse);

    // a scan file cut and mutated loads or is refused, never more
    test::ScratchDir dir("texscan_fuzz");
    const fs::path archive = dir.path() / "a.s3d";
    write(archive, {std::byte{1}, std::byte{2}});
    t.source_archive = archive;
    t.source_name = "wall01.bmp";
    const fs::path file = dir.path() / "scan.txt";
    REQUIRE(save_scan(file, {archive}, {t}));
    auto text = read_file(file);
    REQUIRE(text);
    REQUIRE(load_scan(file, {archive}));
    const fuzz::Bytes scan_seed(text->begin(), text->end());
    auto load = [&](std::span<const std::byte> d) {
        write(file, std::vector<std::byte>(d.begin(), d.end()));
        (void)load_scan(file, {archive});
    };
    for (std::size_t n = 0; n <= scan_seed.size(); n += 3)
        fuzz::run_case("scan file", fuzz::Bytes(scan_seed.begin(), scan_seed.begin() + static_cast<std::ptrdiff_t>(n)),
                       fuzz::Throws::Never, load);
    for (int i = 0; i < 200; ++i) fuzz::run_case("scan file", fuzz::mutate(scan_seed, rng), fuzz::Throws::Never, load);
}
