// The radial flora (assets/flora.h): the .eco *FLORAPART and RadialFloraDefs.rfd readers and 0x101084e0's placement.
#include <doctest/doctest.h>

#include <limits>
#include <string>
#include <vector>

#include "assets/eqg.h"
#include "assets/flora.h"
#include "assets/zone.h"
#include "game_files.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {

std::vector<std::byte> text(const std::string& s) {
    std::vector<std::byte> out;
    for (char c : s) out.push_back(static_cast<std::byte>(c));
    return out;
}

} // namespace

TEST_CASE("radial flora: the .eco flora layers and the .rfd definitions") {
    auto layers = eqg::parse_eco_flora(text(
        "*TEXTUREPART\n*LAYER grass\n*DETAILMAP a.dds\n*END_LAYER\n*END_TEXTUREPART\n*FLORAPART\n*LAYER heather\n"
        "*MINHEIGHT -10\n*MAXHEIGHT 140\n*HEIGHTTOL 5\n*MINSLOPE 25\n*MAXSLOPE 30\n*SLOPETOL 10\n*FLORA GrasslandFlora\n"
        "*TEXTURELAYER Flowers\n*DENSITY 700.5\n*ITERATIONS 100\n*MINSCALE 0.5\n*MAXSCALE 1.5\n*MINALPHA 128\n"
        "*END_LAYER\n*END_FLORAPART\n"));
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].name == "heather");
    CHECK(layers[0].flora == "grasslandflora");
    CHECK(layers[0].texture_layer == "flowers");
    CHECK(layers[0].min_height == -10.0f);
    CHECK(layers[0].height_tolerance == 5.0f);
    CHECK(layers[0].slope_tolerance == 10);
    CHECK(layers[0].density == 700.5f);
    CHECK(layers[0].iterations == 100);
    CHECK(layers[0].max_scale == 1.5f);
    CHECK(layers[0].min_alpha == 128);
    auto defs = eqg::parse_radial_flora(text(
        "*BEGIN_RADIALFLORA\n\t*FLORADEFINITION GrasslandGrass\n\t\t*TEXTURENAME Moors_Flower_Grass.dds\n"
        "\t\t*MINWIDTH 3.0\n\t\t*MAXWIDTH 6.0\n\t\t*MINHEIGHT 3.0\n\t\t*MAXHEIGHT 6.0\n\t\t*SWAY ENABLED\n"
        "\t\t*SWAYDISPLACEMENT 0.2\n\t\t*SWAYPERIOD 5.0\n\t\t*CROSSFLORA ENABLED\n\t\t*QUADTEXTURE ENABLED\n"
        "\t\t*TEXTUREINDEX 3\n\t\t*INHERITVERTEXCOLOR DISABLED\n\t*END_DEFINITION\n*END_RADIALFLORA\n"));
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].name == "grasslandgrass");
    CHECK(defs[0].texture == "moors_flower_grass.dds");
    CHECK(defs[0].max_width == 6.0f);
    CHECK(defs[0].sway);
    CHECK(defs[0].sway_period == 5.0f);
    CHECK(defs[0].cross);
    CHECK(defs[0].quad_texture);
    CHECK(defs[0].texture_index == 3);
    CHECK_FALSE(defs[0].inherit_vertex_color);
}

TEST_CASE("radial flora: placement by the mask's cover, density, iterations and the bands") {
    constexpr std::uint32_t S = 8;
    std::vector<float> heights(S * S, 10.0f);
    std::vector<std::int32_t> slopes(S * S, 5);
    std::vector<std::string> names = {"grass", "flowers"};
    // The grass covers the left half of the tile, the flowers nothing.
    std::vector<std::vector<std::uint8_t>> masks(2, std::vector<std::uint8_t>(S * S, 0));
    for (std::uint32_t y = 0; y < S; ++y)
        for (std::uint32_t x = 0; x < S / 2; ++x) masks[0][y * S + x] = 255;
    FloraTile tile;
    tile.origin = {128.0f, 256.0f};
    tile.tile_size = 128.0f;
    tile.texels = S;
    tile.heights = heights;
    tile.slopes = slopes;
    tile.layer_names = names;
    tile.layer_masks = masks;
    eqg::FloraDefinition def;
    def.name = "grass";
    def.min_width = def.max_width = 2.0f;
    def.min_height = def.max_height = 3.0f;
    std::vector<eqg::FloraDefinition> defs = {def};
    eqg::EcoFloraLayer layer;
    layer.flora = "grass";
    layer.texture_layer = "grass";
    layer.density = 40.0f; // half covered: 20 wanted
    layer.iterations = 1000;
    layer.min_alpha = 128;
    MsvcRand rand{7};
    std::vector<FloraInstance> out;
    place_flora(tile, std::span(&layer, 1), defs, rand, out);
    CHECK(out.size() == 20);
    for (const auto& f : out) {
        CHECK(f.position.x < 128.0f + 64.0f); // only on the covered half
        CHECK(f.position.y >= 256.0f);
        CHECK(f.position.z == 10.0f);
        CHECK(f.width == 2.0f);
        CHECK(f.height == 3.0f);
    }
    // ITERATIONS bounds the tries; a band the tile misses places nothing; an unknown flora nothing.
    layer.iterations = 4;
    out.clear();
    place_flora(tile, std::span(&layer, 1), defs, rand, out);
    CHECK(out.size() <= 4);
    layer.iterations = 1000;
    layer.max_slope = 2;
    out.clear();
    place_flora(tile, std::span(&layer, 1), defs, rand, out);
    CHECK(out.empty());
    layer.max_slope = 90;
    layer.texture_layer = "flowers";
    out.clear();
    place_flora(tile, std::span(&layer, 1), defs, rand, out);
    CHECK(out.empty());
    layer.texture_layer = "grass";
    layer.flora = "missing";
    out.clear();
    place_flora(tile, std::span(&layer, 1), defs, rand, out);
    CHECK(out.empty());
}

TEST_CASE("radial flora: a layer mask or DAT mask smaller than the tile places nothing, not read past its end") {
    constexpr std::uint32_t S = 8;
    std::vector<float> heights(S * S, 10.0f);
    std::vector<std::int32_t> slopes(S * S, 5);
    std::vector<std::string> names = {"grass"};
    std::vector<std::vector<std::uint8_t>> masks = {std::vector<std::uint8_t>(S * S, 255)};
    FloraTile tile;
    tile.tile_size = 128.0f;
    tile.texels = S;
    tile.heights = heights;
    tile.slopes = slopes;
    tile.layer_names = names;
    tile.layer_masks = masks;
    eqg::FloraDefinition def;
    def.name = "grass";
    std::vector<eqg::FloraDefinition> defs = {def};
    eqg::EcoFloraLayer layer;
    layer.flora = "grass";
    layer.texture_layer = "grass";
    layer.density = 10.0f;
    layer.iterations = 100;
    MsvcRand rand{7};
    std::vector<FloraInstance> out;
    place_flora(tile, std::span(&layer, 1), defs, rand, out);
    CHECK_FALSE(out.empty()); // whole masks: it grows
    const std::vector<std::uint8_t> full_dat(S * S, 255), short_dat(S * S - 1, 255);
    tile.dat_mask = full_dat;
    out.clear();
    place_flora(tile, std::span(&layer, 1), defs, rand, out);
    CHECK_FALSE(out.empty());
    tile.dat_mask = short_dat;
    out.clear();
    place_flora(tile, std::span(&layer, 1), defs, rand, out);
    CHECK(out.empty());
    tile.dat_mask = {};
    masks[0].resize(S * S - 1);
    tile.layer_masks = masks;
    out.clear();
    place_flora(tile, std::span(&layer, 1), defs, rand, out);
    CHECK(out.empty());
    masks[0].clear();
    tile.layer_masks = masks;
    place_flora(tile, std::span(&layer, 1), defs, rand, out);
    CHECK(out.empty());
}

TEST_CASE("radial flora: damaged .eco and .rfd numbers are bounded, not undefined") {
    auto layers = eqg::parse_eco_flora(text(
        "*FLORAPART\n*LAYER a\n*DENSITY 1e30\n*ITERATIONS 3e9\n*MINSLOPE -1e20\n*MAXSLOPE 1e20\n*SLOPETOL 1e20\n"
        "*MINALPHA 1e12\n*END_LAYER\n*LAYER b\n*DENSITY -5\n*ITERATIONS -7\n*END_LAYER\n"
        "*LAYER c\n*DENSITY 2800\n*ITERATIONS 500\n*END_LAYER\n*END_FLORAPART\n"));
    REQUIRE(layers.size() == 3);
    CHECK(layers[0].density == eqg::kMaxFloraDensity);
    CHECK(layers[0].iterations == eqg::kMaxFloraIterations);
    CHECK(layers[0].min_slope == std::numeric_limits<std::int32_t>::min());
    CHECK(layers[0].max_slope == std::numeric_limits<std::int32_t>::max());
    CHECK(layers[0].slope_tolerance == std::numeric_limits<std::int32_t>::max());
    CHECK(layers[0].min_alpha == 255);
    CHECK(layers[1].density == 0.0f);
    CHECK(layers[1].iterations == 0);
    // The game's largest values pass unchanged.
    CHECK(layers[2].density == 2800.0f);
    CHECK(layers[2].iterations == 500);
    auto defs = eqg::parse_radial_flora(text(
        "*BEGIN_RADIALFLORA\n*FLORADEFINITION g\n*TEXTUREINDEX 1e20\n*END_DEFINITION\n*END_RADIALFLORA\n"));
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].texture_index == std::numeric_limits<std::int32_t>::max());

    // A layer built by hand past the bounds, with slope bands at int32's limits: placement stays bounded.
    constexpr std::uint32_t S = 4;
    std::vector<float> heights(S * S, 0.0f);
    std::vector<std::int32_t> slopes(S * S, 0);
    std::vector<std::string> names = {"grass"};
    std::vector<std::vector<std::uint8_t>> masks(1, std::vector<std::uint8_t>(S * S, 255));
    FloraTile tile;
    tile.tile_size = 16.0f;
    tile.texels = S;
    tile.heights = heights;
    tile.slopes = slopes;
    tile.layer_names = names;
    tile.layer_masks = masks;
    eqg::FloraDefinition def;
    def.name = "grass";
    std::vector<eqg::FloraDefinition> d = {def};
    eqg::EcoFloraLayer layer;
    layer.flora = layer.texture_layer = "grass";
    layer.density = std::numeric_limits<float>::infinity();
    layer.iterations = std::numeric_limits<std::int32_t>::max();
    layer.min_slope = std::numeric_limits<std::int32_t>::min();
    layer.max_slope = std::numeric_limits<std::int32_t>::max();
    layer.slope_tolerance = std::numeric_limits<std::int32_t>::max();
    std::vector<eqg::EcoFloraLayer> many(10, layer);
    MsvcRand rand{1};
    std::vector<FloraInstance> out;
    place_flora(tile, many, d, rand, out);
    CHECK(out.size() == kMaxFloraPerTile);
    // The zone's cap: a second tile adds only up to it, and a third nothing.
    const std::size_t cap = kMaxFloraPerTile + 100;
    place_flora(tile, many, d, rand, out, cap);
    CHECK(out.size() == cap);
    place_flora(tile, many, d, rand, out, cap);
    CHECK(out.size() == cap);
    CHECK(kMaxFloraPerZone >= 15u * 262194u); // well above Moors
}

TEST_CASE("radial flora: a tile's seed is the signed products' wrapped value, worked in unsigned") {
    // The values the earlier signed form gave (in 64 bits, wrapped to 32: two's complement).
    auto old_seed = [](std::int64_t lng, std::int64_t lat, std::size_t e) {
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(lng * 73856093)) ^
               static_cast<std::uint32_t>(static_cast<std::uint64_t>(lat * 19349663)) ^
               static_cast<std::uint32_t>(e * 83492791);
    };
    CHECK(flora_tile_seed(0, 0, 0) == 0u);
    CHECK(flora_tile_seed(3, 7, 1) == (221568279u ^ 135447641u ^ 83492791u));
    CHECK(flora_tile_seed(-5, 2, 0) == old_seed(-5, 2, 0));
    CHECK(flora_tile_seed(-5, 2, 0) == (static_cast<std::uint32_t>(-369280465) ^ 38699326u));
    for (std::int32_t lng : {-100, -30, -1, 1, 29, 30, 99, 2000, std::numeric_limits<std::int32_t>::min(),
                             std::numeric_limits<std::int32_t>::max()})
        for (std::int32_t lat : {-77, 0, 12, 1000})
            for (std::size_t e : {std::size_t{0}, std::size_t{4}, std::size_t{10}}) {
                CAPTURE(lng);
                CAPTURE(lat);
                CHECK(flora_tile_seed(lng, lat, e) == old_seed(lng, lat, e));
            }
}

TEST_CASE("radial flora: Blightfire Moors' plants" * doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    Zone zone = load_zone(game, "moors");
    CHECK(zone.flora.definitions.size() >= 10);
    CHECK(zone.flora.instances.size() > 100000);
    for (const auto& t : zone.flora.tiles)
        for (std::uint32_t i = t.first; i < t.first + t.count; ++i) {
            const auto& p = zone.flora.instances[i];
            REQUIRE(p.definition < zone.flora.definitions.size());
            CHECK(p.position.x >= t.min.x);
            CHECK(p.position.x <= t.min.x + zone.flora.tile_size);
        }
}
