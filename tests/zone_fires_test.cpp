// assets/zone_fires.h: which materials and particle clouds are flames, the mesh flames' boxes, the environment
// emitters, and the zone's flames merged and lit.
#include <doctest/doctest.h>

#include <cstring>
#include <fstream>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/zone.h"
#include "assets/zone_fires.h"
#include "core/game_dir.h"
#include "game_files.h"
#include "scratch_dir.h"

using namespace hailstone::assets;

namespace {

Material named(const char* name) {
    Material m;
    m.name = name;
    m.frames = {"x.bmp"};
    return m;
}

// A crossed-quad flame (two quads, four triangles) of `w` x `h` standing at `at`.
void add_flame(Mesh& mesh, glm::vec3 at, float w, float h, std::uint32_t material) {
    MeshPart part{static_cast<std::uint32_t>(mesh.indices.size()), 12, material};
    for (int q = 0; q < 2; ++q) {
        glm::vec3 side = q ? glm::vec3(0.0f, w * 0.5f, 0.0f) : glm::vec3(w * 0.5f, 0.0f, 0.0f);
        auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (glm::vec3 p : {at - side, at + side, at + side + glm::vec3(0, 0, h), at - side + glm::vec3(0, 0, h)}) {
            Vertex v;
            v.position = p;
            mesh.vertices.push_back(v);
        }
        for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + i);
    }
    mesh.parts.push_back(part);
}

} // namespace

TEST_CASE("flame materials by name") {
    CHECK(flame_material_hue(named("FIRE1_MDF")) == FlameHue::Fire);
    CHECK(flame_material_hue(named("fire1")) == FlameHue::Fire);
    CHECK(flame_material_hue(named("BLUEFLAME01_MDF")) == FlameHue::Blue);
    CHECK(flame_material_hue(named("BFIRE1_MDF")) == FlameHue::Blue);
    CHECK(flame_material_hue(named("GFIRE1_MDF")) == FlameHue::Green);
    // Holders, fuel and the look-alikes stay.
    for (const char* other : {"FIREWOOD_MDF", "COALS_MDF", "TORCHTOP_MDF", "TAKTORCH01_MDF", "YKFLAME01_MDF",
                              "FIREPLACE_MDF", "SMKE1_MDF"})
        CHECK(flame_material_hue(named(other)) == FlameHue::None);
}

TEST_CASE("flame clouds by sprite, hue and name") {
    EmitterDefinition d;
    d.texture = "GENG00.DDS";
    CHECK(flame_emitter_hue(d) == FlameHue::Fire);
    d.texture = "GENG10.DDS";
    CHECK(flame_emitter_hue(d) == FlameHue::Blue);
    d.texture = "CSMOKE.DDS";
    CHECK(flame_emitter_hue(d) == FlameHue::None);
    d.texture = "GENE01.DDS"; // the fire pots' sparkle
    CHECK(flame_emitter_hue(d) == FlameHue::None);
    // vald_bluetorch: the ordinary sprite in a blue colour.
    d.name = "vald_bluetorch";
    d.texture = "flamed.dds";
    d.color_start = {10, 10, 255};
    CHECK(flame_emitter_hue(d) == FlameHue::Blue);
    d.color_start = {255, 255, 255};
    d.name = "TorchFlame";
    CHECK(flame_emitter_hue(d) == FlameHue::Fire);
    // A grey sprite in grey: its name says.
    d.name = "cob_brazier_green";
    d.texture = "flamegray.dds";
    d.color_start = {128, 128, 128};
    CHECK(flame_emitter_hue(d) == FlameHue::Green);
    d.name = "purple_brazier";
    d.texture = "fire01_gray.dds";
    d.color_start = {73, 50, 112};
    CHECK(flame_emitter_hue(d) == FlameHue::Violet);
    // Lava is not a flame.
    d.name = "AGlava_flame";
    d.texture = "new_fire.dds";
    CHECK(flame_emitter_hue(d) == FlameHue::None);
}

TEST_CASE("a mesh's flames: touching triangles are one flame") {
    Mesh mesh;
    mesh.materials = {named("TORCHTOP_MDF"), named("FIRE1_MDF")};
    // A holder (not a flame), and two candles a unit apart: two flames.
    add_flame(mesh, {0, 0, -2}, 1.0f, 2.0f, 0);
    add_flame(mesh, {0, 0, 0}, 0.3f, 0.9f, 1);
    add_flame(mesh, {1.5f, 0, 0}, 0.3f, 0.9f, 1);
    auto flames = mesh_flames(mesh);
    REQUIRE(flames.size() == 2);
    CHECK(flames[0].hue == FlameHue::Fire);
    CHECK(flames[0].min.z == doctest::Approx(0.0f));
    CHECK(flames[0].max.z == doctest::Approx(0.9f));

    // Placed: scaled by 2 and moved, its foot at the lowest corner, the crossed quads' width taken in.
    glm::mat4 t = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(10, 20, 30)), glm::vec3(2.0f));
    ZoneFire f = place_flame(flames[0], t);
    CHECK(f.base.x == doctest::Approx(10.0f));
    CHECK(f.base.y == doctest::Approx(20.0f));
    CHECK(f.base.z == doctest::Approx(30.0f));
    CHECK(f.height == doctest::Approx(0.9f * 2.0f * 1.1f));
    CHECK(f.width == doctest::Approx(0.3f * 2.0f * 0.7f));
}

TEST_CASE("a mesh's flames: the grouping matches the pairwise merge, a chain of growing boxes and NaN included") {
    // The pairwise merge mesh_flames used before its sweep: the reference for the result and its order.
    auto pairwise = [](std::vector<FlameBox> boxes) {
        auto touch = [](const FlameBox& a, const FlameBox& b) {
            return glm::all(glm::lessThanEqual(a.min, b.max + 0.25f)) && glm::all(glm::lessThanEqual(b.min, a.max + 0.25f));
        };
        for (bool merged = true; merged;) {
            merged = false;
            for (std::size_t i = 0; i < boxes.size(); ++i)
                for (std::size_t j = i + 1; j < boxes.size();)
                    if (touch(boxes[i], boxes[j])) {
                        boxes[i].min = glm::min(boxes[i].min, boxes[j].min);
                        boxes[i].max = glm::max(boxes[i].max, boxes[j].max);
                        boxes.erase(boxes.begin() + static_cast<std::ptrdiff_t>(j));
                        merged = true;
                    } else {
                        ++j;
                    }
        }
        return boxes;
    };
    std::uint32_t seed = 12345;
    auto rnd = [&seed](float range) {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f * range;
    };
    for (int round = 0; round < 20; ++round) {
        Mesh mesh;
        mesh.materials = {named("FIRE1_MDF"), named("BFIRE1_MDF")};
        std::vector<FlameBox> tris;
        for (std::uint32_t m = 0; m < 2; ++m) {
            MeshPart part{static_cast<std::uint32_t>(mesh.indices.size()), 0, m};
            for (int t = 0; t < 150; ++t) {
                FlameBox b{glm::vec3(1e30f), glm::vec3(-1e30f), m == 0 ? FlameHue::Fire : FlameHue::Blue};
                glm::vec3 at(rnd(60.0f), rnd(60.0f), rnd(10.0f));
                for (int k = 0; k < 3; ++k) {
                    Vertex v;
                    v.position = at + glm::vec3(rnd(2.0f), rnd(2.0f), rnd(2.0f));
                    if (round == 0 && t == 7 && k == 1) v.position.y = std::numeric_limits<float>::quiet_NaN();
                    b.min = glm::min(b.min, v.position);
                    b.max = glm::max(b.max, v.position);
                    mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
                    mesh.vertices.push_back(v);
                }
                tris.push_back(b);
                part.index_count += 3;
            }
            mesh.parts.push_back(part);
        }
        auto want = pairwise(tris);
        auto got = mesh_flames(mesh);
        REQUIRE(got.size() == want.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK(got[i].hue == want[i].hue);
            // Bitwise equal: NaN compares unequal to itself.
            CHECK(std::memcmp(&got[i].min, &want[i].min, sizeof(glm::vec3)) == 0);
            CHECK(std::memcmp(&got[i].max, &want[i].max, sizeof(glm::vec3)) == 0);
        }
    }

    // A chain of candles, each touching only its neighbours, listed out of order: one flame.
    Mesh stairs;
    stairs.materials = {named("FIRE1_MDF")};
    for (int i : {4, 0, 3, 1, 2}) add_flame(stairs, {static_cast<float>(i) * 0.5f, 0, static_cast<float>(i)}, 0.4f, 0.9f, 0);
    CHECK(mesh_flames(stairs).size() == 1);
}

TEST_CASE("merged fires: one standing in another's flame goes") {
    std::vector<ZoneFire> meshes = {{{0, 0, 0}, 1.0f, 2.0f, FlameHue::Blue}, {{50, 0, 0}, 1.0f, 2.0f}};
    // An emitter on the first torch's flame, and one by itself.
    std::vector<ZoneFire> clouds = {{{0.3f, 0, 0.5f}, 0.8f, 1.5f}, {{100, 0, 0}, 0.8f, 1.5f}};
    auto all = merge_fires(meshes, clouds);
    CHECK(all.size() == 3);
    bool blue = false;
    for (const auto& f : all) blue |= f.hue == FlameHue::Blue;
    CHECK(blue);
}

TEST_CASE("merged fires: a non-finite or absurdly scaled flame is dropped, not searched around") {
    // A placement at scale 1e15 made a flame ~1e15 wide: every later fire's search walked (1e14)^2 grid cells. NaN
    // and infinite positions cast to int64 (undefined) and broke the sort's ordering.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<ZoneFire> meshes = {{{0, 0, 0}, 1e15f, 1e15f},   {{nan, 0, 0}, 1.0f, 2.0f}, {{inf, 0, 0}, 1.0f, 2.0f},
                                    {{0, 0, 0}, nan, 2.0f},      {{0, 0, 0}, 1.0f, inf},    {{10, 0, 0}, 1.0f, 2.0f},
                                    {{1e30f, -1e30f, 0}, 1.0f, 2.0f}};
    std::vector<ZoneFire> clouds = {{{20, 0, 0}, 0.8f, 1.5f}};
    auto all = merge_fires(meshes, clouds);
    REQUIRE(all.size() == 3);
    CHECK(all[0].base.x == doctest::Approx(10.0f));
    CHECK(all[1].base.x == doctest::Approx(1e30f));
    CHECK(all[2].base.x == doctest::Approx(20.0f));

    Zone zone;
    zone.lights.push_back({{inf, nan, 0}, {1, 1, 1}, 100.0f});
    CHECK(add_fire_lights(zone, all) == 2); // the fire at 10 lights the one at 20
}

TEST_CASE("fire lights: only for flames with no light near") {
    Zone zone;
    zone.lights.push_back({{0, 0, 1}, {1, 1, 1}, 100.0f});
    std::vector<ZoneFire> fires = {{{5, 0, 0}, 1.0f, 2.0f}, {{200, 0, 0}, 1.0f, 2.0f}, {{205, 0, 0}, 1.0f, 2.0f}};
    CHECK(add_fire_lights(zone, fires) == 1); // the second lights the third
    REQUIRE(zone.lights.size() == 2);
    CHECK(zone.lights[1].position.x == doctest::Approx(200.0f));
    CHECK(zone.lights[1].radius >= 30.0f);
}

TEST_CASE("environment emitter lines") {
    auto e = parse_environment_emitters("Name^EmitterDefIdx^X^Y^Z^AlwaysVisible^Lifespan\r\n"
                                        "torch01^106^736.5^-1180.1^-89.0^4000000^0\r\n"
                                        "broken^x^1^2^3\n"
                                        "short^57^1.5^2^3^0\n"
                                        "\n");
    REQUIRE(e.size() == 2);
    CHECK(e[0].name == "torch01");
    CHECK(e[0].definition == 106);
    CHECK(e[0].position.x == doctest::Approx(736.5f));
    CHECK(e[0].position.z == doctest::Approx(-89.0f));
    CHECK(e[1].definition == 57);
    // A definition index far past int's range (or below it) is clamped rather than cast out of range.
    auto big = parse_environment_emitters("a^1e30^0^0^0\nb^-1e30^0^0^0\nc^nan^0^0^0\n");
    REQUIRE(big.size() == 3);
    CHECK(big[0].definition == 2147483647);
    CHECK(big[1].definition == -2147483647 - 1);
    CHECK(big[2].definition == 0);
}

TEST_CASE("environment emitters: a revamped zone loaded as S3D takes none of the live .eqg's") {
    hailstone::test::ScratchDir dir("envemit");
    auto write = [&](const std::string& name, const std::string& body) {
        std::ofstream(dir.path() / name, std::ios::binary) << body;
    };
    // An EDD 110 of two empty records ("None" and one more).
    std::string edd = std::string("EDD\0" "110\0", 8) + std::string(2 * 0x1a0, '\0');
    write("environmentemittersnew.edd", edd);
    const std::string emitters = "Name^EmitterDefIdx^X^Y^Z^Lifespan\nfire1^1^10^20^30^4000000\n";
    for (const char* zone : {"nektulos", "somezone"}) {
        write(std::string(zone) + ".s3d", "x");
        write(std::string(zone) + ".eqg", "x");
        write(std::string(zone) + "_environmentemitters.txt", emitters);
    }
    hailstone::GameDir game(dir.path());
    Zone revamped;
    revamped.name = "nektulos"; // loads its RoF2 .s3d; the emitters file is the revamp's
    CHECK(add_environment_emitters(game, revamped) == 0);
    CHECK(revamped.particle_fixtures.empty());
    Zone ordinary;
    ordinary.name = "somezone";
    CHECK(add_environment_emitters(game, ordinary) == 1);
    REQUIRE(ordinary.particle_fixtures.size() == 1);
    CHECK(ordinary.particle_fixtures[0].transform[3].z == doctest::Approx(30.0f));
}

TEST_CASE("freeportwest's torches burn from its environment emitters" * doctest::skip(!hailstone::test::game_dir())) {
    auto z = load_zone(*hailstone::test::game_dir(), "freeportwest");
    std::size_t flames = 0;
    for (const auto& f : z.particle_fixtures)
        flames += flame_emitter_hue(z.particle_definitions[f.definition]) != FlameHue::None;
    CHECK(flames > 400);
}

TEST_CASE("qeynos2's lamps carry mesh flames" * doctest::skip(!hailstone::test::game_dir())) {
    auto z = load_zone(*hailstone::test::game_dir(), "qeynos2");
    auto fires = placement_fires(z);
    CHECK(fires.size() > 50);
    for (const auto& f : fires) {
        CHECK(f.height > 0.1f);
        CHECK(f.height < 30.0f);
    }
}
