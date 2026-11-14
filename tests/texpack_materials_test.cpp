// Photo materials (src/texpack/materials.h): the rules files, the tile sizes measured on a zone, the compositor, the
// fetch with a stand-in for the network, and a build that makes a tile from materials instead of upscaling it.

#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>

#include <doctest/doctest.h>

#include "assets/zone.h"
#include "core/atomic_file.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "game_files.h"
#include "pfs_fixtures.h"
#include "scratch_dir.h"
#include "texpack/materials.h"
#include "texpack/pack_job.h"

using namespace hailstone;
using namespace hailstone::texpack;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kMaterials = R"(
[grass]
asset = leafy_grass
metres = 2
magnify = 3
detail = 2
shading = 0.35
colour = 0.24 0.27 0.13

[rock]
asset = dark_rock_02
metres = 2
magnify = 3
detail = 1
shading = 0.5
colour = 0.35 0.35 0.35
)";

MaterialRules rules_for_tests() {
    auto r = parse_material_rules(kMaterials, {{"testzone", "grass = grass\nedge = grass rock  # a transition\n"}});
    REQUIRE(r);
    return *r;
}

// Deterministic noise 0..1.
float noise(std::uint32_t x, std::uint32_t y, std::uint32_t seed) {
    std::uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

// A material of side `n`: noisy colour about `base`, a flat normal, constant roughness, noisy height.
MaterialLibrary::Maps material(std::uint32_t n, float base, float roughness, std::uint32_t seed) {
    MaterialLibrary::Maps m;
    m.diff = Planes(n, n, 3);
    m.nor = Planes(n, n, 3);
    m.rough = Planes(n, n, 1, roughness);
    m.disp = Planes(n, n, 1);
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const float v = base + 0.2f * (noise(x, y, seed) - 0.5f);
            for (std::uint32_t c = 0; c < 3; ++c) m.diff.at(x, y, c) = v;
            m.nor.at(x, y, 0) = 0.5f;
            m.nor.at(x, y, 1) = 0.5f;
            m.nor.at(x, y, 2) = 1.0f;
            m.disp.at(x, y, 0) = noise(x, y, seed + 7);
        }
    }
    return m;
}

// Left half grass-coloured, right half grey, with a little noise: an RGBA8 image.
assets::Image half_and_half(std::uint32_t n) {
    assets::Image img;
    img.width = img.height = n;
    std::vector<std::byte> px(std::size_t(n) * n * 4);
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const float g = x < n / 2 ? 1.0f : 0.0f;
            const float rgb[3] = {0.24f * g + 0.35f * (1 - g), 0.27f * g + 0.35f * (1 - g), 0.13f * g + 0.35f * (1 - g)};
            for (int c = 0; c < 3; ++c) {
                const float v = std::clamp(rgb[c] + 0.04f * (noise(x, y, 3) - 0.5f), 0.0f, 1.0f);
                px[(std::size_t(y) * n + x) * 4 + c] = std::byte(static_cast<std::uint8_t>(v * 255.0f + 0.5f));
            }
            px[(std::size_t(y) * n + x) * 4 + 3] = std::byte{255};
        }
    }
    img.mips.push_back(std::move(px));
    return img;
}

// The mean of channel c of an RGBA8 image over columns [x0, x1).
double column_mean(const assets::Image& img, std::uint32_t x0, std::uint32_t x1, int c) {
    double s = 0;
    for (std::uint32_t y = 0; y < img.height; ++y)
        for (std::uint32_t x = x0; x < x1; ++x) s += std::to_integer<int>(img.mips[0][(std::size_t(y) * img.width + x) * 4 + c]);
    return s / (double(img.height) * (x1 - x0) * 255.0);
}

// A 24-bit BMP of n x n, grass-coloured on its left half and grey on its right.
std::vector<std::byte> bmp24(std::uint32_t n) {
    ByteWriter out;
    const std::uint32_t row = (n * 3 + 3) / 4 * 4;
    out.le<std::uint8_t>('B');
    out.le<std::uint8_t>('M');
    out.le<std::uint32_t>(54 + row * n);
    out.le<std::uint32_t>(0);
    out.le<std::uint32_t>(54);
    out.le<std::uint32_t>(40);
    out.le<std::int32_t>(static_cast<std::int32_t>(n));
    out.le<std::int32_t>(static_cast<std::int32_t>(n));
    out.le<std::uint16_t>(1);
    out.le<std::uint16_t>(24);
    out.le<std::uint32_t>(0);
    out.le<std::uint32_t>(row * n);
    out.le<std::int32_t>(2835);
    out.le<std::int32_t>(2835);
    out.le<std::uint32_t>(0);
    out.le<std::uint32_t>(0);
    for (std::uint32_t y = 0; y < n; ++y) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const bool grass = x < n / 2;
            out.le<std::uint8_t>(grass ? 33 : 90); // b
            out.le<std::uint8_t>(grass ? 69 : 90); // g
            out.le<std::uint8_t>(grass ? 61 : 90); // r
        }
        for (std::uint32_t pad = n * 3; pad < row; ++pad) out.le<std::uint8_t>(0);
    }
    return out.take();
}

class Nearest final : public Model {
public:
    explicit Nearest(std::uint32_t scale) : scale_(scale) {}
    std::uint32_t scale() const override { return scale_; }
    Planes run(const Planes& rgb) override {
        Planes out(rgb.width * scale_, rgb.height * scale_, 3);
        for (std::uint32_t y = 0; y < out.height; ++y)
            for (std::uint32_t x = 0; x < out.width; ++x)
                for (std::uint32_t c = 0; c < 3; ++c) out.at(x, y, c) = rgb.at(x / scale_, y / scale_, c);
        return out;
    }

private:
    std::uint32_t scale_;
};

class FakeSet final : public ModelSet {
public:
    Nearest color{4}, normal{1}, rough{1};
    Models models() override { return {&color, &normal, &rough}; }
};

void write(const fs::path& p, const std::vector<std::byte>& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

template <typename Pred> bool wait_for(Pred pred, int ms = 30000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

} // namespace

TEST_CASE("photo materials: the rules files") {
    MaterialRules r = rules_for_tests();
    REQUIRE(r.classes.size() == 2);
    const MaterialClass* grass = r.find("grass");
    REQUIRE(grass);
    CHECK(grass->asset == "leafy_grass");
    CHECK(grass->detail == doctest::Approx(2.0));
    CHECK(grass->colour[2] == doctest::Approx(0.13));
    REQUIRE(r.tiles.size() == 2);
    CHECK(r.tiles[1].zone == "testzone");
    CHECK(r.tiles[1].texture == "edge");
    CHECK(r.tiles[1].first == "grass");
    CHECK(r.tiles[1].second == "rock");
    CHECK_FALSE(r.tiles[1].sharp);
    // a name with an extension and any case, and the sharp flag
    auto sharp = parse_material_rules(kMaterials, {{"TestZone", "XPATH.BMP = grass rock sharp"}});
    REQUIRE(sharp);
    CHECK(sharp->tiles[0].texture == "xpath");
    CHECK(sharp->tiles[0].zone == "testzone");
    CHECK(sharp->tiles[0].sharp);
    // the signature follows what shapes the tile
    MaterialRules changed = r;
    changed.classes[1].shading = 0.6f;
    CHECK(changed.signature(r.tiles[1]) != r.signature(r.tiles[1]));
    CHECK(changed.signature(r.tiles[0]) == r.signature(r.tiles[0])); // (grass alone: the rock is not in it)

    std::string error;
    CHECK_FALSE(parse_material_rules(kMaterials, {{"z", "edge = grass sand"}}, &error));
    CHECK(error == "z.txt:1: no material [sand] in materials.txt");
    CHECK_FALSE(parse_material_rules(kMaterials, {{"z", "\n# c\nedge grass"}}, &error));
    CHECK(error.starts_with("z.txt:3: "));
    CHECK_FALSE(parse_material_rules(kMaterials, {{"z", "edge = grass rock grass"}}, &error));
    CHECK_FALSE(parse_material_rules("[a]\nasset = ../up\n", {}, &error));
    CHECK_FALSE(parse_material_rules("[a]\nasset = x\nmetres = -1\n", {}, &error));
    CHECK_FALSE(parse_material_rules("[a]\nasset = x\ncolour = 1 2\n", {}, &error));

    SUBCASE("a folder of them") {
        test::ScratchDir dir("materials-rules");
        REQUIRE(write_file_atomic(dir.path() / "materials.txt", std::string(kMaterials)));
        REQUIRE(write_file_atomic(dir.path() / "qeytoqrg.txt", "hhdrock = rock\n"));
        auto loaded = load_material_rules(dir.path());
        REQUIRE(loaded);
        REQUIRE(loaded->tiles.size() == 1);
        CHECK(loaded->tiles[0].zone == "qeytoqrg");
        CHECK_FALSE(load_material_rules(dir.path() / "none", &error));
    }
}

TEST_CASE("photo materials: repeats keep one scale across tile sizes") {
    MaterialRules r = rules_for_tests();
    const MaterialClass& grass = *r.find("grass");
    // Qeynos Hills: the grass tile is 56 units, its transitions about 108: the grass shows at one scale on both.
    CHECK(material_repeats(grass, 56.2) == 2);
    CHECK(material_repeats(grass, 108.0) == 4);
    CHECK(material_repeats(grass, 5.0) == 1);
}

TEST_CASE("photo materials: tile sizes measured on a zone's ground") {
    assets::Zone zone;
    assets::Mesh mesh;
    // two 100 x 100 quads: one with UVs 0..1 (100 units a repeat), one with 0..2 (50)
    auto quad = [&](float x0, float uv) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (int i = 0; i < 4; ++i) {
            assets::Vertex v;
            v.position = {x0 + (i & 1 ? 100.0f : 0.0f), i & 2 ? 100.0f : 0.0f, 0.0f};
            v.uv = {i & 1 ? uv : 0.0f, i & 2 ? uv : 0.0f};
            mesh.vertices.push_back(v);
        }
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
    };
    quad(0, 1.0f);
    quad(200, 2.0f);
    mesh.parts.push_back({0, 6, 0});
    mesh.parts.push_back({6, 6, 1});
    assets::Material a, b;
    a.frames = {"XGRASS.BMP"};
    b.frames = {"rock.bmp"};
    mesh.materials = {a, b};
    zone.terrain.push_back(mesh);
    auto sizes = measure_tile_sizes(zone);
    REQUIRE(sizes.size() == 2);
    CHECK(sizes["xgrass"] == doctest::Approx(100.0));
    CHECK(sizes["rock"] == doctest::Approx(50.0));
}

TEST_CASE("photo materials: a tile keeps the original's layout and takes the photos' detail") {
    MaterialRules r = rules_for_tests();
    MaterialLibrary lib;
    // photos far darker and far brighter than the art, as real ones are
    lib.add("grass", material(64, 0.15f, 0.3f, 1));
    lib.add("rock", material(64, 0.8f, 0.8f, 2));
    const assets::Image orig = half_and_half(64);

    MaterialTile edge{r.tiles[1], 100.0, "sig"};
    Outputs out = compose_tile(edge, r, orig, lib, 128, true);
    REQUIRE(out.color);
    REQUIRE(out.normal);
    REQUIRE(out.rough);
    CHECK(out.color->width == 128);
    // the colour is the original's on each side (away from the edges, which wrap)
    CHECK(column_mean(*out.color, 16, 48, 1) == doctest::Approx(0.27).epsilon(0.08));
    CHECK(column_mean(*out.color, 16, 48, 2) == doctest::Approx(0.13).epsilon(0.12));
    CHECK(column_mean(*out.color, 80, 112, 2) == doctest::Approx(0.35).epsilon(0.08));
    // the roughness is each material's, split where the original is
    CHECK(column_mean(*out.rough, 16, 48, 0) == doctest::Approx(0.3).epsilon(0.05));
    CHECK(column_mean(*out.rough, 80, 112, 0) == doctest::Approx(0.8).epsilon(0.05));
    // the photo's detail is there (the original's noise alone is much finer and fainter at this size)
    double var = 0;
    const double mean = column_mean(*out.color, 80, 112, 1);
    for (std::uint32_t y = 0; y < 128; ++y)
        for (std::uint32_t x = 80; x < 112; ++x) {
            const double v = std::to_integer<int>(out.color->mips[0][(std::size_t(y) * 128 + x) * 4 + 1]) / 255.0 - mean;
            var += v * v;
        }
    CHECK(std::sqrt(var / (128.0 * 32)) > 0.01);
    // no halo where the materials meet: the texels beside the edge are as bright as those away from it on each side
    // (without each photo scaled to its material's colour, the grass darkens to a quarter and the rock overshoots)
    CHECK(column_mean(*out.color, 54, 62, 1) > column_mean(*out.color, 16, 48, 1) * 0.8);
    CHECK(column_mean(*out.color, 66, 72, 1) < column_mean(*out.color, 80, 112, 1) * 1.15);
    // flat normals stay flat and unit length
    CHECK(column_mean(*out.normal, 0, 128, 2) > 0.99);

    // one material: all grass's roughness, and no maps when they are off
    MaterialTile pure{r.tiles[0], 56.0, "sig"};
    Outputs one = compose_tile(pure, r, orig, lib, 64, false);
    REQUIRE(one.color);
    CHECK_FALSE(one.normal);
    CHECK_FALSE(one.rough);
    CHECK(column_mean(*one.color, 40, 56, 2) == doctest::Approx(0.35).epsilon(0.08)); // still the original's colour

    // a uniform tile stays uniform to its edges (the original is not wrapped round onto itself)
    assets::Image flat = half_and_half(64);
    for (std::size_t i = 0; i < flat.mips[0].size(); i += 4) {
        flat.mips[0][i] = std::byte{90};
        flat.mips[0][i + 1] = std::byte{90};
        flat.mips[0][i + 2] = std::byte{90};
    }
    Outputs even = compose_tile(pure, r, flat, lib, 128, false);
    CHECK(column_mean(*even.color, 0, 2, 0) == doctest::Approx(column_mean(*even.color, 32, 96, 0)).epsilon(0.05));

    MaterialLibrary empty;
    CHECK_THROWS(compose_tile(edge, r, orig, empty, 64, true));
}

TEST_CASE("photo materials: fetching what is missing") {
    test::ScratchDir dir("materials-fetch");
    MaterialRules r = rules_for_tests();
    CHECK(missing_materials(r, dir.path()).size() == 8); // two assets, four maps each
    CHECK(material_url("leafy_grass", "nor_dx") ==
          "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/leafy_grass/leafy_grass_nor_dx_1k.jpg");
    std::vector<std::string> asked;
    FetchOptions o;
    o.get = [&](const std::string& url, std::vector<std::byte>& body, std::string&) {
        asked.push_back(url);
        body = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff}, std::byte{0xe0}};
        return true;
    };
    std::size_t last = 0;
    o.progress = [&](std::size_t done, std::size_t) { last = done; };
    REQUIRE(fetch_materials(r, dir.path(), o));
    CHECK(asked.size() == 8);
    CHECK(last == 8);
    CHECK(fs::exists(material_file(dir.path(), "dark_rock_02", "disp")));
    CHECK(missing_materials(r, dir.path()).empty());
    asked.clear();
    REQUIRE(fetch_materials(r, dir.path(), o));
    CHECK(asked.empty());

    // a failure stops it and says which file; a body that is not a JPEG is one
    fs::remove(material_file(dir.path(), "leafy_grass", "diff"));
    std::string error;
    o.get = [](const std::string&, std::vector<std::byte>& body, std::string&) {
        body = {std::byte{'<'}, std::byte{'h'}};
        return true;
    };
    CHECK_FALSE(fetch_materials(r, dir.path(), o, &error));
    CHECK(error.find("leafy_grass_diff_1k.jpg: not a JPEG") != std::string::npos);
    o.get = [](const std::string&, std::vector<std::byte>&, std::string& why) {
        why = "HTTP 404";
        return false;
    };
    CHECK_FALSE(fetch_materials(r, dir.path(), o, &error));
    CHECK(error.ends_with(": HTTP 404"));
}

TEST_CASE("photo materials: a build composes the planned tiles and remembers the rules they were made by") {
    test::ScratchDir dir("materials-build");
    const fs::path assets = dir.path() / "assets";
    write(assets / "testzone.s3d", test::pfs_fixtures::make_pfs({{"edge.bmp", bmp24(64)}, {"other.bmp", bmp24(32)}}));
    ScanResult scanned = scan(list_archives({assets}), {.threads = 1});
    REQUIRE(scanned.textures.size() == 2);
    const TextureInfo* edge = nullptr;
    for (const TextureInfo& t : scanned.textures)
        if (t.names.contains("edge.bmp")) edge = &t;
    REQUIRE(edge);

    MaterialRules r = rules_for_tests();
    auto plan = std::make_shared<MaterialPlan>();
    plan->rules = r;
    plan->size = 256; // (the upscaler would make 128)
    plan->tiles[edge->hash] = {r.tiles[1], 100.0, "rules-1"};
    auto lib = std::make_shared<MaterialLibrary>();
    lib->add("grass", material(64, 0.5f, 0.3f, 1));
    lib->add("rock", material(64, 0.5f, 0.8f, 2));

    const fs::path pack = dir.path() / "textures" / "hd";
    GpuWorker gpu([](std::string*) { return std::make_unique<FakeSet>(); });
    auto build = [&](std::shared_ptr<const MaterialPlan> p) {
        JobOptions o;
        o.pack_dir = pack;
        o.textures = scanned.textures;
        o.settings = {2, true};
        o.encoders = 2;
        o.materials = std::move(p);
        o.material_library = lib;
        PackJob job(o, gpu);
        REQUIRE(wait_for([&] { return job.finished(); }));
        return job.status();
    };
    auto width_of = [&](std::string_view suffix) {
        auto bytes = read_file(pack / "by-hash" / (hash_name(edge->hash) + std::string(suffix) + ".dds"));
        REQUIRE(bytes);
        auto img = assets::decode_texture(*bytes);
        REQUIRE(img);
        return img->width;
    };

    JobStatus s = build(plan);
    CHECK(s.state == JobStatus::State::Done);
    CHECK(s.failures == 0);
    CHECK(s.files_written == 6); // each texture's colour, normal and roughness
    CHECK(width_of("") == 256);
    CHECK(width_of(".n") == 256);
    CHECK(read_material_record(pack) == std::map<std::uint64_t, std::string>{{edge->hash, "rules-1"}});

    // nothing to make while the rules are the same
    CHECK(build(plan).total == 0);
    // changed rules make the tile again
    auto changed = std::make_shared<MaterialPlan>(*plan);
    changed->tiles[edge->hash].signature = "rules-2";
    s = build(changed);
    CHECK(s.total == 1);
    CHECK(read_material_record(pack).at(edge->hash) == "rules-2");
    // dropped from the plan: the upscaler makes it again and the record forgets it
    s = build(std::make_shared<MaterialPlan>());
    CHECK(s.total == 1);
    CHECK(width_of("") == 128);
    CHECK(read_material_record(pack).empty());

    // a plan without its images fails the build
    JobOptions o;
    o.pack_dir = pack;
    o.textures = scanned.textures;
    o.materials = plan;
    PackJob broken(o, gpu);
    REQUIRE(wait_for([&] { return broken.finished(); }));
    CHECK(broken.status().state == JobStatus::State::Failed);
}

TEST_CASE("photo materials: Qeynos Hills' tiles measured from the game files") {
    const GameDir* game = test::game_dir();
    if (!game || !fs::exists(game->root() / "qeytoqrg.s3d")) return;
    auto sizes = measure_tile_sizes(assets::load_classic_zone(*game, "qeytoqrg"));
    CHECK(sizes["xgrassdirt2"] == doctest::Approx(56.2).epsilon(0.02));
    CHECK(sizes["hhdrock"] == doctest::Approx(112.1).epsilon(0.02));
}
