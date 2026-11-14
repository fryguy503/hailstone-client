#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "assets/regions.h"
#include "assets/wld.h"
#include "assets/zone.h"
#include "render/zone_renderer.h"

using namespace hailstone;
namespace wld = hailstone::assets::wld;

namespace {

wld::BspNode split(glm::vec3 n, float d, std::uint32_t front, std::uint32_t back) {
    wld::BspNode node;
    node.normal = n;
    node.distance = d;
    node.front = front;
    node.back = back;
    return node;
}

wld::BspNode leaf(std::uint32_t region) {
    wld::BspNode node;
    node.region = region;
    return node;
}

wld::RegionTag tag(std::string text, std::vector<std::uint32_t> regions) {
    wld::RegionTag t;
    t.tag = std::move(text);
    t.regions = std::move(regions);
    return t;
}

} // namespace

TEST_CASE("classic liquids: passable surfaces on a liquid region's top, carried through a dry zone line") {
    // North Ro's shore in miniature: under z 0 the sea (region 1, "WT") for x <= 100, a zone line's dry region
    // (region 2, "DR..TP") for 100 < x <= 120, plain ground (region 3) beyond and above.
    std::vector<wld::BspNode> nodes = {
        split({0, 0, -1}, 0, 2, 6),    // 1: z <= 0
        split({-1, 0, 0}, 100, 4, 3),  // 2: x <= 100
        split({-1, 0, 0}, 120, 5, 6),  // 3: x <= 120
        leaf(1),                       // 4
        leaf(2),                       // 5
        leaf(3),                       // 6
    };
    assets::Zone zone;
    zone.regions = assets::ZoneRegions::build(
        nodes, 3, {tag("WT_ZONE", {0}), tag("DRNTP00255000002000000000000000_ZONE", {1})});

    assets::Mesh m;
    auto tri = [&](float x0, float x1, float z0, float z1) {
        for (glm::vec3 p : {glm::vec3(x0, 0, z0), glm::vec3(x0, 10, z0), glm::vec3(x1, 0, z1)}) {
            assets::Vertex v;
            v.position = p;
            m.indices.push_back(static_cast<std::uint32_t>(m.vertices.size()));
            m.vertices.push_back(v);
        }
    };
    // The water material, passable (part 0): triangles 0..3.
    tri(10, 20, -0.5f, -0.5f);   // 0: on the sea's top
    tri(105, 115, -0.5f, -0.5f); // 1: the same surface over the zone line's dry region
    tri(105, 115, -10, -10);     // 2: the same material lower down, in the dry region: not the surface
    tri(30, 40, -0.5f, 9.5f);    // 3: sloped
    // Solid sand at the waterline (part 1): triangle 4.
    tri(50, 60, -0.5f, -0.5f);
    m.materials.resize(2);
    m.materials[0].name = "W1_MDF";
    m.materials[0].frames = {"w1.bmp"};
    m.materials[1].name = "SFS1_MDF";
    m.materials[1].frames = {"sfs1.bmp"};
    m.parts.push_back({0, 12, 0, false});
    m.parts.push_back({12, 3, 1, true});
    zone.terrain.push_back(m);

    const auto liquid = render::classic_liquid_triangles(zone);
    REQUIRE(liquid.size() == 1);
    REQUIRE(liquid[0].size() == 5);
    CHECK(liquid[0][0] == render::kLiquidWater);
    CHECK(liquid[0][1] == render::kLiquidWater);
    CHECK(liquid[0][2] == 0);
    CHECK(liquid[0][3] == 0);
    CHECK(liquid[0][4] == 0); // solid: the beach, not the sea

    // A passable sand triangle there would be the sea's.
    zone.terrain[0].parts[1].solid = false;
    CHECK(render::classic_liquid_triangles(zone)[0][4] == render::kLiquidWater);
}

TEST_CASE("zone renderer: a prepare that is cancelled stops and returns nothing") {
    // A neighbour zone's load thrown away on zoning (app/neighbour_zones.cpp) asks between placements and materials.
    assets::Zone zone;
    zone.placements.resize(3);
    render::ZoneRenderOptions options;
    options.cancelled = [] { return true; };
    CHECK_FALSE(render::ZoneRenderer::prepare(zone, options, 16));
    int asked = 0;
    options.cancelled = [&asked] {
        ++asked;
        return false;
    };
    CHECK(render::ZoneRenderer::prepare(zone, options, 16));
    CHECK(asked > 0);
}

namespace {

// A flat terrain mesh of `cell` squares over x0..x1, y0..y1 at height z, one plain material on `texture`.
assets::Mesh flat_terrain(float x0, float y0, float x1, float y1, float cell, float z, const char* texture) {
    assets::Mesh m;
    for (float x = x0; x < x1; x += cell)
        for (float y = y0; y < y1; y += cell)
            for (glm::vec3 p : {glm::vec3(x, y, z), glm::vec3(x + cell, y, z), glm::vec3(x + cell, y + cell, z),
                                glm::vec3(x, y, z), glm::vec3(x + cell, y + cell, z), glm::vec3(x, y + cell, z)}) {
                assets::Vertex v;
                v.position = p;
                v.normal = {0, 0, 1};
                v.uv = glm::vec2(p) / 64.0f;
                v.color = 0xff808080u;
                m.indices.push_back(static_cast<std::uint32_t>(m.vertices.size()));
                m.vertices.push_back(v);
            }
    m.materials.resize(1);
    m.materials[0].name = texture;
    m.materials[0].frames = {texture};
    m.parts.push_back({0, static_cast<std::uint32_t>(m.indices.size()), 0, true});
    return m;
}

} // namespace

TEST_CASE("zone renderer: the land round the zone, shaped by a neighbour's ground, and the neighbour's part of it") {
    assets::Zone zone;
    zone.name = "here";
    zone.terrain.push_back(flat_terrain(0, 0, 2000, 1500, 100, 50, "grass.bmp"));
    const auto plain = render::ZoneRenderer::prepare(zone, {}, 64);
    REQUIRE(plain);
    const auto land = render::ZoneRenderer::prepared_land(*plain);
    REQUIRE(land.build);
    CHECK(land.build->field.valid());
    CHECK(land.build->sources.empty());
    CHECK(land.build->ground.frames == std::vector<std::string>{"grass.bmp"});
    CHECK(land.build->ground.density == doctest::Approx(1.0f / 64.0f));
    CHECK(land.triangles[0] > 0);
    CHECK(land.triangles[1] + land.triangles[2] == 0);

    // A neighbour's ground east of it, from the cache: the land takes it in.
    std::vector<float> ground(40 * 30, 150.0f);
    auto source = std::make_shared<const render::HorizonSource>(
        render::horizon_source("east", {2000.0f, -400.0f}, render::kHorizonCell, 40, 30, ground));
    render::ZoneRenderOptions shaped;
    shaped.horizon_sources = {source};
    const auto with = render::ZoneRenderer::prepare(zone, shaped, 64);
    REQUIRE(with);
    const auto land2 = render::ZoneRenderer::prepared_land(*with);
    REQUIRE(land2.build);
    REQUIRE(land2.build->sources.size() == 1);
    CHECK(land2.build->sources[0]->name == "east");

    // The neighbour draws its part: each triangle with its own ground and with the current zone's.
    auto patch = std::make_shared<render::LandPatch>();
    patch->mesh = render::horizon_mesh(land2.build->field, land2.build->sources, 0);
    patch->ground = land2.build->ground;
    patch->rock = land2.build->rock;
    REQUIRE_FALSE(patch->mesh.squares.empty());
    const std::size_t patch_triangles = (patch->mesh.ground.size() + patch->mesh.rock.size()) / 3;
    assets::Zone east;
    east.name = "east";
    east.terrain.push_back(flat_terrain(-500, 0, 3000, 2000, 100, 0, "dirt.bmp"));
    render::ZoneRenderOptions neighbour;
    neighbour.offset = {2500.0f, 0.0f, 150.0f};
    neighbour.keep = {glm::vec4(1.0f, 0.0f, 0.0f, -2000.0f)}; // past x 2000
    neighbour.behind = true;
    neighbour.land = patch;
    const auto drawn = render::ZoneRenderer::prepare(east, neighbour, 64);
    REQUIRE(drawn);
    const auto part = render::ZoneRenderer::prepared_land(*drawn);
    CHECK_FALSE(part.build); // a neighbour keeps no land of its own
    CHECK(part.footprint == patch->mesh.squares.size());
    CHECK(part.triangles[1] == patch_triangles);
    CHECK(part.triangles[2] == patch_triangles);
    CHECK(part.triangles[0] == 0);

    // A neighbour with no plain ground of its own: the current zone's alone, not faded.
    assets::Zone bare;
    bare.name = "bare";
    const auto alone = render::ZoneRenderer::prepare(bare, neighbour, 64);
    REQUIRE(alone);
    const auto part2 = render::ZoneRenderer::prepared_land(*alone);
    CHECK(part2.triangles[0] == patch_triangles);
    CHECK(part2.triangles[1] + part2.triangles[2] == 0);
}
