// Hailstone's flora for zones without any (assets/zone_flora.h): the rules file and the placement over the terrain.
#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <string>
#include <vector>

#include "assets/flora.h"
#include "assets/zone.h"
#include "assets/zone_flora.h"
#include "render/flora_renderer.h"
#include "game_files.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {

eqg::FloraDefinition definition(const std::string& name, float size) {
    eqg::FloraDefinition d;
    d.name = name;
    d.texture = name + ".dds";
    d.min_width = d.max_width = size;
    d.min_height = d.max_height = size;
    return d;
}

// A square of side `size` at height `z`, its far corner raised by `rise` (a slope), facing up (or down when `flip`),
// textured `texture`.
Mesh square(const std::string& texture, float size, float rise = 0.0f, bool flip = false) {
    Mesh m;
    glm::vec3 up = flip ? glm::vec3(0, 0, -1) : glm::vec3(0, 0, 1);
    for (auto p : {glm::vec3(0, 0, 0), glm::vec3(size, 0, 0), glm::vec3(size, size, rise), glm::vec3(0, size, rise)}) {
        Vertex v;
        v.position = p;
        v.normal = up;
        v.color = 0xff808080u;
        m.vertices.push_back(v);
    }
    m.indices = {0, 1, 2, 0, 2, 3};
    Material mat;
    mat.frames = {texture};
    m.materials.push_back(mat);
    m.parts.push_back({0, 6, 0, true});
    return m;
}

} // namespace

TEST_CASE("zone flora: the rules file") {
    std::vector<std::string> errors;
    auto greenish = parse_zone_flora_rules("ground *grass* 12 +greenish grass\n");
    REQUIRE(greenish.ground.size() == 1);
    CHECK(greenish.ground[0].green_only);
    CHECK(greenish.ground[0].greenish);
    auto rules = parse_zone_flora_rules(R"(# East Commonlands
donor Commonlands.eqg
max_slope 30
scale 0.8 1.2
ground cpath* 0          # the roads stay bare
ground CPG* 12.5 grassblades:3 grass2
ground sand 2 DesertPlant1:0.5
ground bad x grass
ground bad 1 grass:-1
nonsense here
)", &errors);
    REQUIRE(rules.donors.size() == 1);
    CHECK(rules.donors[0] == "commonlands.eqg");
    CHECK(rules.max_slope == 30.0f);
    CHECK(rules.min_scale == doctest::Approx(0.8f));
    CHECK(rules.max_scale == doctest::Approx(1.2f));
    REQUIRE(rules.ground.size() == 3);
    CHECK(rules.ground[0].texture == "cpath*");
    CHECK(rules.ground[0].density == 0.0f);
    CHECK(rules.ground[1].density == 12.5f);
    REQUIRE(rules.ground[1].kinds.size() == 2);
    CHECK(rules.ground[1].kinds[0].definition == "grassblades");
    CHECK(rules.ground[1].kinds[0].weight == 3.0f);
    CHECK(rules.ground[1].kinds[1].weight == 1.0f);
    CHECK(rules.ground[2].kinds[0].weight == 0.5f);
    CHECK(errors.size() == 3);
}

TEST_CASE("zone flora: several donors, and a kind by its donor") {
    auto rules = parse_zone_flora_rules("donor commonlands.eqg\ndonor OldKithicor.eqg\ndonor commonlands.eqg\n");
    REQUIRE(rules.donors.size() == 2); // in order, once each
    CHECK(rules.donors[0] == "commonlands.eqg");
    CHECK(rules.donors[1] == "oldkithicor.eqg");
    CHECK(flora_kind_matches("fern", "oldkithicor/fern"));
    CHECK(flora_kind_matches("FERN", "oldkithicor/fern"));
    CHECK(flora_kind_matches("oldkithicor/fern", "oldkithicor/fern"));
    CHECK_FALSE(flora_kind_matches("moors/fern", "oldkithicor/fern"));
    CHECK_FALSE(flora_kind_matches("fern", "oldkithicor/fernA"));
    CHECK(flora_kind_matches("grass", "grass")); // a definition of no donor (tests, an EQG zone's own)
    // The first donor's definition of a plain name; another's by its donor.
    std::vector<eqg::FloraDefinition> defs = {definition("commonlands/grass", 2.0f), definition("elddar/grass", 6.0f)};
    Zone zone;
    zone.terrain.push_back(square("grass.bmp", 100.0f));
    ZoneFlora first, second;
    place_zone_flora(zone, parse_zone_flora_rules("ground grass* 10 grass\n"), defs, first);
    place_zone_flora(zone, parse_zone_flora_rules("ground grass* 10 elddar/grass\n"), defs, second);
    REQUIRE(!first.instances.empty());
    REQUIRE(!second.instances.empty());
    CHECK(first.instances[0].definition == 0);
    CHECK(second.instances[0].definition == 1);
}

TEST_CASE("zone flora: include reads another rules file in place") {
    auto include = [](std::string_view name) -> std::optional<std::string> {
        if (name == "default") return std::string("donor shared.eqg\nmax_slope 45\nground * 12 +green grass\n");
        if (name == "loop") return std::string("include loop\n");
        return std::nullopt;
    };
    std::vector<std::string> errors;
    // The zone's own lines first: its exceptions win over the shared catch-all; a setting after the include wins.
    auto rules = parse_zone_flora_rules("ground road* 0\ninclude default\nmax_slope 30\n", &errors, include);
    CHECK(errors.empty());
    REQUIRE(rules.donors.size() == 1);
    CHECK(rules.donors[0] == "shared.eqg");
    CHECK(rules.max_slope == 30.0f);
    REQUIRE(rules.ground.size() == 2);
    CHECK(rules.ground[0].texture == "road*");
    CHECK(rules.ground[1].texture == "*");
    CHECK(rules.ground[1].green_only);
    // A missing file, an include without a reader and a loop are errors, not hangs.
    errors.clear();
    parse_zone_flora_rules("include nothing\ninclude loop\n", &errors, include);
    CHECK(errors.size() == 2);
    errors.clear();
    parse_zone_flora_rules("include default\n", &errors);
    CHECK(errors.size() == 1);
}

TEST_CASE("zone flora: a zone's own rules, else default.txt when it is outdoor") {
    test::ScratchDir dir("zone_flora");
    auto write = [&](const char* name, const char* text) {
        std::ofstream(dir.path() / name, std::ios::binary) << text;
    };
    write("default.txt", "donor shared.eqg\nground * 12 +green grass\n");
    write("ecommons.txt", "ground sand* 1 cactus\ninclude default\n");
    std::string source;
    auto own = load_zone_flora_rules(dir.path(), "EComMons", false, &source);
    REQUIRE(own);
    CHECK(source == "ecommons");
    CHECK(own->ground.size() == 2);
    auto shared = load_zone_flora_rules(dir.path(), "gfaydark", true, &source);
    REQUIRE(shared);
    CHECK(source == "default");
    CHECK_FALSE(load_zone_flora_rules(dir.path(), "guktop", false)); // indoor: only a file of its own
    // A name that is not a zone's never reads a path of its own: indoor nothing, outdoor the default.
    CHECK(load_zone_flora_rules(dir.path(), "../ecommons", false) == std::nullopt);
    REQUIRE(load_zone_flora_rules(dir.path(), "../ecommons", true, &source));
    CHECK(source == "default");
}

TEST_CASE("zone flora: placement by texture, density and slope") {
    ZoneFloraRules rules = parse_zone_flora_rules("donor x.eqg\nmax_slope 30\nground path* 0\n"
                                                  "ground grass* 10 tall:1 short:1\n");
    std::vector<eqg::FloraDefinition> defs = {definition("short", 2.0f), definition("tall", 5.0f)};
    Zone zone;
    zone.name = "test";
    zone.terrain.push_back(square("GRASS01.BMP", 100.0f));         // 10,000 units: about 1,000 plants
    zone.terrain.push_back(square("path1.bmp", 100.0f));           // bare
    zone.terrain.push_back(square("rock.bmp", 100.0f));            // no rule
    zone.terrain.push_back(square("grass02.bmp", 100.0f, 100.0f)); // 45 degrees: too steep
    zone.terrain.push_back(square("grass03.bmp", 100.0f, 0.0f, true)); // a ceiling
    // Side by side, so none lies over another (that is cover).
    for (std::size_t i = 1; i < zone.terrain.size(); ++i)
        for (auto& v : zone.terrain[i].vertices) v.position.x += 200.0f * static_cast<float>(i);
    ZoneFlora flora;
    place_zone_flora(zone, rules, defs, flora);
    CHECK(flora.definitions.size() == 2);
    CHECK(flora.instances.size() > 900);
    CHECK(flora.instances.size() < 1100);
    std::size_t tall = 0;
    for (const auto& p : flora.instances) {
        CHECK(p.position.x >= 0.0f);
        CHECK(p.position.x <= 100.0f); // the grass square alone (the slope comes in below)
        CHECK(p.position.z == 0.0f);
        CHECK(p.color == 0xff808080u); // no texel lookup: neutral
        if (p.definition == 1) {
            ++tall;
            CHECK(p.height == 5.0f);
        }
    }
    CHECK(tall > 400);
    CHECK(tall < 600);
    // Binned by kZoneFloraTile: the 100-unit square lies in one tile.
    REQUIRE(flora.tiles.size() == 1);
    CHECK(flora.tiles[0].count == flora.instances.size());
    CHECK(flora.tile_size == kZoneFloraTile);

    // The same plants at every load.
    ZoneFlora again;
    place_zone_flora(zone, rules, defs, again);
    REQUIRE(again.instances.size() == flora.instances.size());
    CHECK(again.instances.front().position == flora.instances.front().position);
    CHECK(again.instances.back().rotation == flora.instances.back().rotation);

    // A gentler limit lets the slope grow too; a kind the definitions lack places nothing.
    rules.max_slope = 50.0f;
    ZoneFlora steep;
    place_zone_flora(zone, rules, defs, steep);
    CHECK(steep.instances.size() > flora.instances.size() + 900);
    ZoneFlora none;
    place_zone_flora(zone, parse_zone_flora_rules("ground grass* 10 missing\n"), defs, none);
    CHECK(none.instances.empty());
}

TEST_CASE("zone flora: no plants under a roof or a ceiling") {
    std::vector<eqg::FloraDefinition> defs = {definition("grass", 2.0f)};
    auto rules = parse_zone_flora_rules("ground grass* 10 grass\nground roof* 0\n");
    Zone zone;
    zone.terrain.push_back(square("grass.bmp", 100.0f));
    // A roof 20 units over the square's left half (x 0 .. 50): bare ground under it.
    Mesh roof = square("roof.bmp", 50.0f);
    for (auto& v : roof.vertices) {
        v.position.y *= 2.0f;
        v.position.z = 20.0f;
    }
    zone.terrain.push_back(roof);
    ZoneFlora flora;
    place_zone_flora(zone, rules, defs, flora);
    CHECK(flora.instances.size() > 400);
    CHECK(flora.instances.size() < 600);
    for (const auto& p : flora.instances) CHECK(p.position.x >= 50.0f);
    // A roof that is never drawn (an outdoor zone's invisible collision ceiling) is no cover.
    zone.terrain[1].materials[0].blend = BlendMode::Invisible;
    ZoneFlora open;
    place_zone_flora(zone, rules, defs, open);
    CHECK(open.instances.size() > 900);
    // Nothing grows on a translucent surface (the classic water).
    zone.terrain[0].materials[0].blend = BlendMode::Transparent50;
    ZoneFlora water;
    place_zone_flora(zone, rules, defs, water);
    CHECK(water.instances.empty());
}

TEST_CASE("zone flora: roofs across the whole zone still cover, and their cell references stay bounded") {
    std::vector<eqg::FloraDefinition> defs = {definition("grass", 2.0f)};
    auto rules = parse_zone_flora_rules("ground grass* 10 grass\nground roof* 0\n");
    Zone zone;
    zone.terrain.push_back(square("grass.bmp", 100.0f));
    // 600 roof triangles each across 180,000 units at z 20: every one lies over 7.9 million cover cells, which once
    // wrapped the 32-bit cell totals and wrote past the cell index. They are tested at every point instead.
    Mesh huge = square("roof.bmp", 180000.0f);
    for (auto& v : huge.vertices) v.position += glm::vec3(-90000.0f, -90000.0f, 20.0f);
    for (int i = 0; i < 600; ++i) zone.terrain.push_back(huge);
    ZoneFlora flora;
    place_zone_flora(zone, rules, defs, flora);
    CHECK(flora.instances.empty());

    // 20,000 roof pieces of 31 x 31 cells over the ground: more references than the grid holds. The first fill it
    // and cover the ground; those past the bound are no cover, and nothing is written out of range.
    Zone many;
    many.terrain.push_back(square("grass.bmp", 100.0f));
    Mesh roof = square("roof.bmp", 1980.0f);
    for (auto& v : roof.vertices) v.position += glm::vec3(-940.0f, -940.0f, 30.0f);
    Mesh roofs = roof;
    roofs.indices.clear();
    roofs.vertices.clear();
    for (int i = 0; i < 20000; ++i) {
        const auto base = static_cast<std::uint32_t>(roofs.vertices.size());
        roofs.vertices.insert(roofs.vertices.end(), roof.vertices.begin(), roof.vertices.end());
        for (std::uint32_t k : {0u, 1u, 2u}) roofs.indices.push_back(base + k);
    }
    roofs.parts = {{0, static_cast<std::uint32_t>(roofs.indices.size()), 0, true}};
    many.terrain.push_back(roofs);
    ZoneFlora covered;
    place_zone_flora(many, rules, defs, covered);
    // Each roof is the square's first triangle (0, 1, 2), over the ground's half y <= x: plants grow on the rest.
    CHECK(covered.instances.size() > 400);
    CHECK(covered.instances.size() < 600);
    for (const auto& p : covered.instances) CHECK(p.position.x < p.position.y);
}

TEST_CASE("zone flora: non-finite and absurd vertices grow nothing and cover nothing") {
    std::vector<eqg::FloraDefinition> defs = {definition("grass", 2.0f)};
    auto rules = parse_zone_flora_rules("ground grass* 10 grass\nground roof* 0\n");
    Zone zone;
    zone.terrain.push_back(square("grass.bmp", 100.0f));
    ZoneFlora plain;
    place_zone_flora(zone, rules, defs, plain);
    REQUIRE(plain.instances.size() > 900);
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    // Damaged ground: NaN, infinity, and a finite triangle a billion units across (once split into 4^10 pieces,
    // each walked over millions of tiles).
    for (glm::vec3 bad : {glm::vec3(nan, 0, 0), glm::vec3(inf, 0, 0), glm::vec3(0, -inf, 0), glm::vec3(1e9f, 1e9f, 0)}) {
        Mesh m = square("grass.bmp", 100.0f);
        m.vertices[2].position = bad; // in both its triangles
        zone.terrain.push_back(m);
        Mesh r = square("roof.bmp", 100.0f);
        for (auto& v : r.vertices) v.position.z = 10.0f;
        r.vertices[1].position = bad;
        zone.terrain.push_back(r);
    }
    ZoneFloraGrower grower(zone, rules, defs, {});
    CHECK(grower.triangles() < 100);
    ZoneFlora flora;
    place_zone_flora(zone, rules, defs, flora);
    // The roofs' sane triangle (0, 2, 3) still covers the ground's half y >= x.
    CHECK(flora.instances.size() > 400);
    CHECK(flora.instances.size() < 600);
    for (const auto& p : flora.instances) CHECK(p.position.x > p.position.y);
}

TEST_CASE("zone flora: tile by tile, each plant once, in the tile its foot is in") {
    std::vector<eqg::FloraDefinition> defs = {definition("grass", 2.0f)};
    auto rules = parse_zone_flora_rules("ground grass* 10 grass\n");
    Zone zone;
    zone.terrain.push_back(square("grass.bmp", 300.0f)); // two triangles over 3 x 3 tiles
    for (auto& v : zone.terrain[0].vertices) v.position -= glm::vec3(100.0f, 100.0f, 0.0f);
    ZoneFloraGrower grower(zone, rules, defs, {});
    CHECK(grower.triangles() >= 2 * 16); // each split into pieces no larger than a quarter tile
    REQUIRE(grower.tiles().size() == 9);
    std::size_t total = 0;
    std::set<std::tuple<float, float>> feet;
    for (std::size_t t = 0; t < grower.tiles().size(); ++t) {
        std::vector<FloraInstance> plants;
        grower.grow(t, plants);
        const glm::vec2 lo = grower.tiles()[t];
        for (const auto& p : plants) {
            CHECK(p.position.x >= lo.x);
            CHECK(p.position.x < lo.x + kZoneFloraTile);
            CHECK(p.position.y >= lo.y);
            CHECK(p.position.y < lo.y + kZoneFloraTile);
            feet.insert({p.position.x, p.position.y});
        }
        total += plants.size();
        // The same plants when grown again.
        std::vector<FloraInstance> again;
        grower.grow(t, again);
        REQUIRE(again.size() == plants.size());
        if (!plants.empty()) CHECK(again.back().position == plants.back().position);
    }
    CHECK(feet.size() == total); // no plant in two tiles
    CHECK(total > 8500);         // 90,000 units at 10 per 100
    CHECK(total < 9500);
}

TEST_CASE("zone flora: +green keeps the plants on the grass of a blended texture") {
    CHECK(is_green_texel(60, 90, 40));
    CHECK_FALSE(is_green_texel(200, 170, 120)); // sand
    CHECK_FALSE(is_green_texel(150, 110, 70));  // cliff
    CHECK_FALSE(is_green_texel(10, 14, 8));     // too dark to tell
    CHECK_FALSE(is_green_texel(80, 80, 80));    // grey
    CHECK_FALSE(is_green_texel(50, 90, 82));    // the classic water's teal
    CHECK_FALSE(is_green_texel(70, 72, 40));    // olive mud
    CHECK(is_green_texel(140, 170, 120));       // Luclin's pale grass
    // +greenish takes the dry, olive grass of a texture named grass; still not its dirt, nor water.
    CHECK_FALSE(is_green_texel(92, 95, 48));
    CHECK(is_greenish_texel(92, 95, 48));
    CHECK_FALSE(is_greenish_texel(120, 96, 64)); // dirt
    CHECK_FALSE(is_greenish_texel(50, 90, 82));  // water
    std::vector<eqg::FloraDefinition> defs = {definition("grass", 2.0f)};
    Zone zone;
    zone.terrain.push_back(square("blend.bmp", 100.0f));
    for (auto& v : zone.terrain[0].vertices) v.uv = glm::vec2(v.position) / 100.0f;
    // The picture's left half is grass, its right half sand.
    GroundTextures textures = [](std::string_view) -> std::optional<Image> {
        Image im;
        im.width = 2;
        im.height = 1;
        im.mips.push_back({std::byte{0x3c}, std::byte{0x5a}, std::byte{0x28}, std::byte{0xff},
                           std::byte{0xc8}, std::byte{0xaa}, std::byte{0x78}, std::byte{0xff}});
        return im;
    };
    ZoneFlora all, masked, unreadable;
    place_zone_flora(zone, parse_zone_flora_rules("ground blend* 10 grass\n"), defs, all, textures);
    place_zone_flora(zone, parse_zone_flora_rules("ground blend* 10 +green grass\n"), defs, masked, textures);
    CHECK(all.instances.size() > 900);
    CHECK(masked.instances.size() > 400);
    CHECK(masked.instances.size() < 600);
    for (const auto& p : masked.instances) {
        CHECK(p.position.x < 50.0f);
        CHECK(p.color == 0xff285a3cu); // the ground's texel
    }
    // A texture the lookup cannot read grows nothing (no telling grass from stone), nor does one without a lookup.
    place_zone_flora(zone, parse_zone_flora_rules("ground blend* 10 +green grass\n"), defs, unreadable,
                     [](std::string_view) { return std::optional<Image>(); });
    CHECK(unreadable.instances.empty());
    ZoneFlora blind;
    place_zone_flora(zone, parse_zone_flora_rules("ground blend* 10 +green grass\n"), defs, blind);
    CHECK(blind.instances.empty());
}

TEST_CASE("zone flora: East Commonlands' ground (game files)") {
    const GameDir* game = test::game_dir();
    if (!game || !game->exists("ecommons.s3d")) return;
    Zone zone = load_zone(*game, "ecommons");
    // The terrain's area by texture and how much of it faces up, for tuning flora/ecommons.txt.
    struct Stat {
        double area = 0.0, flat = 0.0;
        double r = 0, g = 0, b = 0;
    };
    std::map<std::string, Stat> stats;
    for (const auto& mesh : zone.terrain)
        for (const auto& part : mesh.parts) {
            if (part.material >= mesh.materials.size() || mesh.materials[part.material].frames.empty()) continue;
            auto& s = stats[mesh.materials[part.material].frames.front()];
            for (std::uint32_t i = part.first_index; i + 3 <= part.first_index + part.index_count; i += 3) {
                const auto &a = mesh.vertices[mesh.indices[i]], &b = mesh.vertices[mesh.indices[i + 1]],
                           &c = mesh.vertices[mesh.indices[i + 2]];
                glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);
                double area = glm::length(n) * 0.5;
                if (glm::dot(n, a.normal + b.normal + c.normal) < 0.0f) n = -n;
                s.area += area;
                if (glm::length(n) > 0 && glm::normalize(n).z > 0.766f) s.flat += area;
                s.r += area * (a.color & 0xff);
                s.g += area * (a.color >> 8 & 0xff);
                s.b += area * (a.color >> 16 & 0xff);
            }
        }
    for (const auto& [tex, s] : stats)
        if (s.area > 0)
            MESSAGE(tex << " area " << static_cast<long long>(s.area) << " flat " << static_cast<long long>(s.flat)
                        << " colour " << static_cast<int>(s.r / s.area) << "," << static_cast<int>(s.g / s.area) << ","
                        << static_cast<int>(s.b / s.area));
    CHECK(!stats.empty());

    // The shipped rules: plants on the meadows, none in the water.
    REQUIRE(add_zone_flora(*game, zone, HAILSTONE_SOURCE_DIR "/flora", true) > 500);
    CHECK(!zone.flora.donors.empty());
    REQUIRE(zone.flora.grower);
    std::vector<FloraInstance> plants;
    for (std::size_t t = 0; t < zone.flora.grower->tiles().size(); ++t) zone.flora.grower->grow(t, plants);
    CHECK(plants.size() > 300000);
    std::size_t wet = 0;
    for (const auto& p : plants)
        if (zone.regions.lookup(p.position + glm::vec3(0, 0, 1)).is_water()) ++wet;
    CHECK(wet == 0);
    for (std::size_t i = 0; i < plants.size(); i += plants.size() / 8) {
        const auto& p = plants[i];
        MESSAGE("plant " << p.position.x << " " << p.position.y << " " << p.position.z << " colour " << std::hex
                         << p.color << std::dec);
    }
}

TEST_CASE("flora distance: all the plants within kFloraReach, fewer by the square of the distance past it") {
    CHECK(render::flora_keep(0.0f) == 1.0f);
    CHECK(render::flora_keep(render::kFloraReach) == 1.0f);
    CHECK(render::flora_keep(render::kFloraReach * 2.0f) == doctest::Approx(0.25f));
    CHECK(render::flora_keep(render::kMaxFloraReach) == doctest::Approx(1.0f / 9.0f));
    // Thinner the further out.
    float last = 1.0f;
    for (float d = render::kFloraReach; d <= render::kMaxFloraReach; d += 50.0f) {
        CHECK(render::flora_keep(d) <= last);
        last = render::flora_keep(d);
    }
}

TEST_CASE("zone flora: the lawn's rules") {
    std::vector<std::string> errors;
    auto rules = parse_zone_flora_rules("lawn_height 1.5 4\nground *grass* 0 +greenish lawn\n"
                                        "ground * 2 +green lawn fern:1\nground road* 0\nground dirt* 3\n"
                                        "lawn_height 3 2\n",
                                        &errors);
    CHECK(rules.lawn_min == 1.5f);
    CHECK(rules.lawn_max == 4.0f);
    REQUIRE(rules.ground.size() == 3);
    CHECK(rules.ground[0].lawn);
    CHECK(rules.ground[0].green_only);
    CHECK(rules.ground[0].kinds.empty());
    CHECK(rules.ground[0].grows());
    CHECK(rules.ground[1].lawn);
    REQUIRE(rules.ground[1].kinds.size() == 1);
    CHECK(rules.ground[1].kinds[0].definition == "fern");
    CHECK_FALSE(rules.ground[2].lawn);
    CHECK_FALSE(rules.ground[2].grows()); // bare
    // A density without kinds and a band upside down are errors.
    CHECK(errors.size() == 2);
    // Defaults.
    auto plain = parse_zone_flora_rules("ground * 0 lawn\n");
    CHECK(plain.lawn_min == 1.8f);
    CHECK(plain.lawn_max == 3.0f);
}

TEST_CASE("zone flora: the lawn is an even carpet, one patch per square, without donors") {
    Zone zone;
    zone.name = "test";
    zone.terrain.push_back(square("grass.bmp", 128.0f));         // the whole of tile (0, 0)
    zone.terrain.push_back(square("road.bmp", 64.0f));           // bare
    for (auto& v : zone.terrain[1].vertices) v.position.x += 300.0f;
    ZoneFlora flora;
    place_zone_flora(zone, parse_zone_flora_rules("ground road* 0\nground grass* 0 lawn\n"), {}, flora);
    // 40 x 40 squares over the tile, every one once: the two triangles' shared edge grows each square on it once.
    REQUIRE(flora.instances.size() == 1600);
    std::set<std::pair<long, long>> cells;
    for (const auto& p : flora.instances) {
        CHECK(p.definition == kLawnDefinition);
        CHECK(p.width == kLawnCell);
        CHECK(p.height >= 1.8f * 0.55f); // the edge tapers (soften_lawn_edges)
        CHECK(p.height <= 3.0f);
        CHECK(p.color == 0xff3f6b3au); // no texel lookup: the lawn's own green
        CHECK(p.slope == glm::vec2(0.0f));
        const float fx = p.position.x / kLawnCell - 0.5f, fy = p.position.y / kLawnCell - 0.5f;
        CHECK(fx == doctest::Approx(std::round(fx)).epsilon(1e-4));
        CHECK(fy == doctest::Approx(std::round(fy)).epsilon(1e-4));
        cells.emplace(std::lround(fx), std::lround(fy));
    }
    CHECK(cells.size() == 1600);
    // The same at every load.
    ZoneFlora again;
    place_zone_flora(zone, parse_zone_flora_rules("ground road* 0\nground grass* 0 lawn\n"), {}, again);
    REQUIRE(again.instances.size() == flora.instances.size());
    CHECK(again.instances[700].height == flora.instances[700].height);

    // On a slope a patch keeps the ground's: the square's far side rises 32 over 128.
    Zone hill;
    hill.terrain.push_back(square("grass.bmp", 128.0f, 32.0f));
    ZoneFlora sloped;
    place_zone_flora(hill, parse_zone_flora_rules("ground grass* 0 lawn\n"), {}, sloped);
    REQUIRE(sloped.instances.size() == 1600);
    for (const auto& p : sloped.instances) {
        CHECK(p.slope.x == doctest::Approx(0.0f));
        CHECK(p.slope.y == doctest::Approx(0.25f));
        CHECK(p.position.z == doctest::Approx(p.position.y * 0.25f));
    }

    // +green: only over the green texels, which give the patch its colour.
    Image half;
    half.width = half.height = 2;
    half.mips.push_back({});
    for (int px : {0, 1, 0, 1}) {
        const std::uint32_t c = px == 0 ? 0xff285a3cu : 0xff808080u; // green left, grey right
        for (int k = 0; k < 4; ++k) half.mips[0].push_back(static_cast<std::byte>(c >> (8 * k) & 0xff));
    }
    for (auto& v : zone.terrain[0].vertices) v.uv = glm::vec2(v.position) / 128.0f;
    ZoneFlora green;
    place_zone_flora(zone, parse_zone_flora_rules("ground grass* 0 +green lawn\n"), {}, green,
                     [&](std::string_view) { return std::optional<Image>(half); });
    CHECK(green.instances.size() == 800);
    for (const auto& p : green.instances) {
        CHECK(p.position.x < 64.0f);
        CHECK(p.color == 0xff285a3cu);
    }
}

TEST_CASE("zone flora: the lawn tapers where it meets bare ground") {
    Zone zone;
    zone.terrain.push_back(square("grass.bmp", 64.0f)); // 20 x 20 squares inside tile (0, 0)
    for (auto& v : zone.terrain[0].vertices) v.position += glm::vec3(32.0f, 32.0f, 0.0f);
    auto rules = parse_zone_flora_rules("lawn_height 2 2\nground grass* 0 lawn\n");
    ZoneFlora flora;
    place_zone_flora(zone, rules, {}, flora);
    REQUIRE(flora.instances.size() == 400);
    auto at = [&](int x, int y) {
        for (const auto& p : flora.instances)
            if (std::lround(p.position.x / kLawnCell - 0.5f) == x && std::lround(p.position.y / kLawnCell - 0.5f) == y)
                return p.height;
        return -1.0f;
    };
    // Squares 10 .. 29 each way: inside, the whole height; at the edge the least, rising over kLawnEdgeCells.
    CHECK(at(20, 20) == doctest::Approx(2.0f));
    CHECK(at(10, 20) == doctest::Approx(2.0f * lawn_edge_factor(1.0f)));
    CHECK(at(10, 10) == doctest::Approx(2.0f * lawn_edge_factor(1.0f)));
    CHECK(at(11, 20) == doctest::Approx(2.0f * lawn_edge_factor(2.0f)));
    CHECK(at(10 + kLawnEdgeCells, 20) == doctest::Approx(2.0f));
    CHECK(lawn_edge_factor(1.0f) == doctest::Approx(0.55f));
    CHECK(lawn_edge_factor(static_cast<float>(kLawnEdgeCells) + 0.5f) == doctest::Approx(1.0f));
    for (float d = 1.0f; d < 5.0f; d += 0.25f) CHECK(lawn_edge_factor(d + 0.25f) >= lawn_edge_factor(d));
    // Across a tile's border too: ground of 256 units over four tiles tapers at its own edge only.
    Zone whole;
    whole.terrain.push_back(square("grass.bmp", 256.0f));
    for (auto& v : whole.terrain[0].vertices) v.position -= glm::vec3(128.0f, 128.0f, 0.0f);
    ZoneFlora full;
    place_zone_flora(whole, rules, {}, full);
    REQUIRE(full.tiles.size() == 4);
    REQUIRE(full.instances.size() == 6400);
    std::size_t edge = 0;
    for (const auto& p : full.instances) {
        const float band = 128.0f - kLawnCell * static_cast<float>(kLawnEdgeCells);
        const bool at_edge = std::abs(p.position.x) > band || std::abs(p.position.y) > band;
        if (at_edge) ++edge;
        CHECK((p.height < 1.99f) == at_edge);
    }
    CHECK(edge == 6400 - 74 * 74);
}

TEST_CASE("zone flora: the lawn grows no lone tufts") {
    auto rules = parse_zone_flora_rules("ground grass* 0 +green lawn\n");
    // Ground a little over one square: its patch has no neighbours and goes.
    Zone speck;
    speck.terrain.push_back(square("grass.bmp", kLawnCell * 1.5f));
    for (auto& v : speck.terrain[0].vertices) v.position += glm::vec3(32.0f, 32.0f, 0.0f);
    ZoneFlora none;
    place_zone_flora(speck, parse_zone_flora_rules("ground grass* 0 lawn\n"), {}, none);
    CHECK(none.instances.empty());

    // A texture green but for a scatter of single grey texels (one in nine): a square's majority keeps the lawn whole
    // over them. And one grey but for a scatter of green ones grows nothing.
    auto scatter = [](bool green_ground) {
        Image img;
        img.width = img.height = 96;
        img.mips.push_back({});
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x) {
                const bool odd = x % 3 == 1 && y % 3 == 1;
                const std::uint32_t c = (odd != green_ground) ? 0xff285a3cu : 0xff808080u;
                for (int k = 0; k < 4; ++k) img.mips[0].push_back(static_cast<std::byte>(c >> (8 * k) & 0xff));
            }
        return img;
    };
    Zone zone;
    zone.terrain.push_back(square("grass.bmp", 128.0f));
    for (auto& v : zone.terrain[0].vertices) v.uv = glm::vec2(v.position) / 128.0f;
    for (bool green_ground : {true, false}) {
        const Image img = scatter(green_ground);
        ZoneFlora flora;
        place_zone_flora(zone, rules, {}, flora, [&](std::string_view) { return std::optional<Image>(img); });
        if (green_ground) CHECK(flora.instances.size() == 1600);
        else CHECK(flora.instances.empty());
    }
}

TEST_CASE("zone flora: the lawn's height wanders smoothly between 0 and 1") {
    float lo = 1.0f, hi = 0.0f;
    for (float x = -500.0f; x < 500.0f; x += 7.0f)
        for (float y = -300.0f; y < 300.0f; y += 11.0f) {
            const float h = lawn_height_noise({x, y});
            CHECK(h >= 0.0f);
            CHECK(h <= 1.0f);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
            // Neighbouring patches differ by little.
            CHECK(std::abs(lawn_height_noise({x + kLawnCell, y}) - h) < 0.2f);
        }
    CHECK(hi - lo > 0.5f);
}

TEST_CASE("lawn: levels of detail and the far patches' stride") {
    CHECK(render::lawn_lod(0.0f) == 0);
    CHECK(render::lawn_lod(render::kLawnNear + 1.0f) == 1);
    CHECK(render::lawn_lod(render::kLawnMid + 1.0f) == 2);
    // Each level fewer vertices, its blades wider, so a patch keeps about its cover.
    for (std::size_t k = 1; k < render::kLawnLods.size(); ++k) {
        CHECK(render::kLawnLods[k].vertices() < render::kLawnLods[k - 1].vertices());
        CHECK(render::kLawnLods[k].blade_width > render::kLawnLods[k - 1].blade_width);
    }
    CHECK(render::kLawnLods[0].vertices() == 49 * 15);
    CHECK(render::kLawnLods[2].vertices() == 9 * 3);
    CHECK(render::lawn_stride(0.0f) == 1);
    CHECK(render::lawn_stride(render::kFloraReach) == 1);
    CHECK(render::lawn_stride(render::kFloraReach + 1.0f) == 2);
    CHECK(render::lawn_stride(render::kFloraReach * 2.0f) == 2);
    CHECK(render::lawn_stride(render::kMaxFloraReach) == 4);
    // A patch's own stride: its square's column and row, negative ones too.
    const float c = kLawnCell;
    CHECK(render::lawn_patch_stride({0.5f * c, 0.5f * c}) == 4);
    CHECK(render::lawn_patch_stride({2.5f * c, 0.5f * c}) == 2);
    CHECK(render::lawn_patch_stride({2.5f * c, 1.5f * c}) == 1);
    CHECK(render::lawn_patch_stride({-3.5f * c, -7.5f * c}) == 4); // squares -4 and -8
    CHECK(render::lawn_patch_stride({-1.5f * c, -3.5f * c}) == 2); // -2 and -4
    CHECK(render::lawn_patch_stride({-0.5f * c, 0.5f * c}) == 1);  // -1
    CHECK(render::lawn_stride(std::numeric_limits<float>::infinity()) >= 1);
}
