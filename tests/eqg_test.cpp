#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <optional>

#include <cmath>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

#include "assets/eqg.h"
#include "assets/eqg_character.h"
#include "assets/pfs.h"
#include "assets/zone.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "game/collision.h"
#include "game/coords.h"
#include "game/revamped_zone.h"
#include "game_files.h"
#include "pfs_fixtures.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {

// A string table builder: offset of each added string.
struct Strings {
    std::vector<std::byte> bytes;
    std::int32_t add(const std::string& s) {
        auto at = static_cast<std::int32_t>(bytes.size());
        for (char c : s) bytes.push_back(static_cast<std::byte>(c));
        bytes.push_back(std::byte{0});
        return at;
    }
};

void vec3(ByteWriter& w, float x, float y, float z) {
    w.f32(x);
    w.f32(y);
    w.f32(z);
}

struct TestMaterial {
    std::string name, shader, diffuse;
};

struct TestTriangle {
    std::uint32_t a, b, c, material, flags;
};

// A .ter (terrain) or .mod file of version 1 or 3.
std::vector<std::byte> make_model(bool terrain, std::uint32_t version, const std::vector<TestMaterial>& mats,
                                  const std::vector<glm::vec3>& positions, const std::vector<glm::vec2>& uvs,
                                  const std::vector<TestTriangle>& tris) {
    Strings s;
    std::vector<std::array<std::int32_t, 4>> refs;
    for (const auto& m : mats) refs.push_back({s.add(m.name), s.add(m.shader), s.add("e_TextureDiffuse0"), s.add(m.diffuse)});
    ByteWriter w;
    w.le<std::uint32_t>(terrain ? eqg::kTerMagic : eqg::kModMagic);
    w.le<std::uint32_t>(version);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(s.bytes.size()));
    w.le<std::uint32_t>(static_cast<std::uint32_t>(mats.size()));
    w.le<std::uint32_t>(static_cast<std::uint32_t>(positions.size()));
    w.le<std::uint32_t>(static_cast<std::uint32_t>(tris.size()));
    if (!terrain) w.le<std::uint32_t>(0); // bones
    w.bytes(s.bytes);
    for (std::size_t i = 0; i < mats.size(); ++i) {
        w.le<std::uint32_t>(static_cast<std::uint32_t>(i + 10)); // authored id, unused
        w.le<std::int32_t>(refs[i][0]);
        w.le<std::int32_t>(refs[i][1]);
        w.le<std::uint32_t>(1);
        w.le<std::int32_t>(refs[i][2]);
        w.le<std::uint32_t>(2);
        w.le<std::int32_t>(refs[i][3]);
    }
    for (std::size_t i = 0; i < positions.size(); ++i) {
        vec3(w, positions[i].x, positions[i].y, positions[i].z);
        vec3(w, 0, 0, 1);
        if (version == 3) w.le<std::uint32_t>(0xff102030u);
        w.f32(uvs[i].x);
        w.f32(uvs[i].y);
        if (version == 3) {
            w.f32(0);
            w.f32(0);
        }
    }
    for (const auto& t : tris) {
        w.le(t.a);
        w.le(t.b);
        w.le(t.c);
        w.le(t.material);
        w.le(t.flags);
    }
    return w.take();
}

} // namespace

TEST_CASE("EQG models parse; triangle ordinals resolve to the first same-named material") {
    std::vector<glm::vec3> pos{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
    std::vector<glm::vec2> uv{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    auto data = make_model(false, 3,
                           {{"stone", "Opaque_MPLBasic.fx", "stone_c.dds"},
                            {"leaf", "Chroma_MPLBasicAT.fx", "leaf_c.dds"},
                            {"stone", "Opaque_MPLBump.fx", "other_c.dds"}},
                           pos, uv,
                           {{0, 1, 2, 2, 0}, {0, 2, 3, 1, eqg::kTrianglePassable}, {0, 1, 3, eqg::kNoMaterial, 0}});
    auto m = eqg::parse_model(data, false);
    CHECK(m.version == 3);
    REQUIRE(m.materials.size() == 3);
    CHECK(m.materials[1].shader == "Chroma_MPLBasicAT.fx");
    CHECK(m.materials[1].texture("E_TEXTUREDIFFUSE0") == "leaf_c.dds");
    CHECK(m.material_for(2) == &m.materials[0]); // ordinal 2 is "stone" -> the first "stone"
    CHECK(m.material_for(7) == nullptr);
    // Resolved once at parse time, not scanned per triangle.
    CHECK(m.first_material == std::vector<std::uint32_t>{0, 1, 0});
    auto scanned = m;
    scanned.first_material.clear();
    CHECK(scanned.material_for(2) == &scanned.materials[0]);
    REQUIRE(m.colors.size() == 4);
    CHECK(m.colors[0] == 0xff102030u);
    CHECK(m.triangles[1].flags == eqg::kTrianglePassable);
    CHECK_THROWS(eqg::parse_model(data, true)); // EQGM is not EQGT

    eqg::MeshStats stats;
    auto mesh = eqg::build_mesh(m, "OBJ", stats);
    CHECK(mesh.vertices.size() == 4);
    CHECK(mesh.vertices[0].color == 0xff302010u); // BGRA -> RGBA
    REQUIRE(mesh.parts.size() == 3);
    int invisible = 0, masked = 0, passable = 0;
    for (const auto& part : mesh.parts) {
        const auto& mat = mesh.materials[part.material];
        if (mat.blend == BlendMode::Invisible) {
            ++invisible;
            CHECK(part.solid); // material -1 still collides
        }
        if (mat.blend == BlendMode::Masked) {
            ++masked;
            CHECK_FALSE(part.solid);
            CHECK(mat.frames == std::vector<std::string>{"leaf_c.dds"});
        }
        if (mat.name == "stone") CHECK(mat.frames == std::vector<std::string>{"stone_c.dds"});
        passable += !part.solid;
    }
    CHECK(invisible == 1);
    CHECK(masked == 1);
    CHECK(passable == 1);
}

TEST_CASE("EQG meshes drop non-finite triangles; MaxCB1 terrain UVs go through SHORT2") {
    float nan = std::nanf("");
    std::vector<glm::vec3> pos{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {nan, 0, 0}};
    std::vector<glm::vec2> uv{{0.5f, 128.0f}, {1.0f / 512.0f, -0.75f}, {nan, 2.0f}, {0, 0}};
    auto data = make_model(true, 1, {{"ground", "Opaque_MaxCB1.fx", "dirt_c.dds"}}, pos, uv,
                           {{0, 1, 2, 0, 0}, {0, 1, 3, 0, 0}});
    auto m = eqg::parse_model(data, true);
    eqg::MeshStats stats;
    auto mesh = eqg::build_mesh(m, "TER", stats);
    CHECK(stats.bad_triangles == 1);
    CHECK(mesh.indices.size() == 3);
    // u * 256 truncated to 16 bits: 128 wraps to -128, 1/512 truncates to 0, NaN becomes 0.
    CHECK(mesh.vertices[0].uv == glm::vec2(0.5f, -128.0f));
    CHECK(mesh.vertices[1].uv == glm::vec2(0.0f, -0.75f));
    CHECK(mesh.vertices[2].uv == glm::vec2(0.0f, 2.0f));
}

TEST_CASE("shader names map to blend modes") {
    CHECK(eqg::shader_blend("Opaque_MPLBump.fx") == BlendMode::Opaque);
    CHECK(eqg::shader_blend("Alpha_MaxCBSG1.fx") == BlendMode::Masked);
    CHECK(eqg::shader_blend("Chroma_MPLBasicAT.fx") == BlendMode::Masked);
    CHECK(eqg::shader_blend("AddAlpha_MPLBasicA.fx") == BlendMode::Additive);
    using hailstone::assets::EqgShader;
    CHECK(eqg::shader_class("Alpha_MaxCBSG1.fx") == EqgShader::Alpha);
    CHECK(eqg::shader_class("AlphaSModelC1Max.fx") == EqgShader::Alpha);
    CHECK(eqg::shader_class("Chroma_MPLBasicAT.fx") == EqgShader::Chroma);
    CHECK(eqg::shader_class("AddAlpha_MPLBasicA.fx") == EqgShader::AddAlpha);
    // "AddAlpha" is tested before "Alpha" and the opaque suffixes.
    CHECK(eqg::shader_class("Opaque_AddAlphaC1Max.fx") == EqgShader::AddAlpha);
    CHECK(eqg::shader_blend("Opaque_AddAlphaC1Max.fx") == BlendMode::Additive);
    // Case-sensitive: a lower-case name is no class.
    CHECK(eqg::shader_class("alpha_maxc1.fx") == EqgShader::None);
    CHECK(eqg::shader_class("Opaque_MPLBump.fx") == EqgShader::None);
    CHECK(eqg::shader_blend("Opaque_MaxWater.fx") == BlendMode::Transparent50);
    CHECK(eqg::shader_blend("Opaque_MaxWaterFall.fx") == BlendMode::Opaque);
    CHECK(eqg::shader_blend("Opaque_MaxLava.fx") == BlendMode::Opaque);
    // The effect classes: WaterFall, Water, Terrain, Lava2, Lava in that order.
    CHECK(eqg::shader_class("Opaque_MaxWaterFall.fx") == EqgShader::WaterFall);
    CHECK(eqg::shader_class("Opaque_MaxWater.fx") == EqgShader::Water);
    CHECK(eqg::shader_class("Opaque_MaxTerrain.fx") == EqgShader::Terrain);
    CHECK(eqg::shader_class("Opaque_MaxSMLava2.fx") == EqgShader::Lava2);
    CHECK(eqg::shader_class("Opaque_MaxLava.fx") == EqgShader::Lava);
    // Zone-mesh classes without an effect draw nothing.
    CHECK(eqg::region_effect_unmapped("Alpha_MaxCB1.fx"));
    CHECK(eqg::region_effect_unmapped("Alpha_MaxCBS1.fx"));
    CHECK(eqg::region_effect_unmapped("AlphaSModelCB1Max.fx"));
    CHECK(eqg::region_effect_unmapped("Opaque_MaxCBS_2UV.fx"));
    CHECK_FALSE(eqg::region_effect_unmapped("Alpha_MaxCBSG1.fx"));
    CHECK_FALSE(eqg::region_effect_unmapped("Alpha_MaxC1.fx"));
    CHECK_FALSE(eqg::region_effect_unmapped("Opaque_MaxCB1.fx"));
    CHECK_FALSE(eqg::region_effect_unmapped("Opaque_MaxCB1_2UV.fx"));
}

TEST_CASE("EQG effect parameters come from the e_* properties") {
    eqg::MaterialDef def;
    def.shader = "Opaque_MaxWater.fx";
    auto prop = [&](std::string name, std::uint32_t type, float f, std::uint32_t u, std::string text) {
        eqg::Property p;
        p.name = std::move(name);
        p.type = type;
        p.f = f;
        p.u = u;
        p.text = std::move(text);
        def.properties.push_back(p);
    };
    prop("e_TextureNormal0", 2, 0, 0, "Water_N.dds");
    prop("e_fSlide1X", 0, 0.04f, 0, "");
    prop("e_fSlide2Y", 0, -0.5f, 0, "");
    prop("e_fFresnelBias", 0, 0.06f, 0, "");
    prop("e_fWaterColor1", 3, 0, 0xff3d5d64u, "");
    auto e = eqg::effect_params(def, hailstone::assets::EqgShader::Water);
    CHECK(e.slide == glm::vec4(0.04f, 0.0f, 0.0f, -0.5f));
    REQUIRE(e.textures.size() == 2);
    CHECK(e.textures[0] == "water_n.dds");
    CHECK(e.textures[1].empty());
    CHECK(e.fresnel_bias == doctest::Approx(0.06f));
    CHECK(e.water_color1.r == doctest::Approx(0x3d / 255.0f));
    CHECK(e.water_color1.b == doctest::Approx(0x64 / 255.0f));
    CHECK(e.water_color1.a == doctest::Approx(1.0f));
}

TEST_CASE("EQG packed glow and gloss maps follow the CBGG1 and CBSG1 material classes") {
    eqg::ModelFile model;
    model.positions = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
    model.normals.assign(3, glm::vec3(0, 0, 1));
    model.uvs = {{0, 0}, {1, 0}, {0, 1}};
    model.triangles.push_back({{0, 1, 2}, 0, 0});
    eqg::MaterialDef def;
    def.name = "crystal";
    auto texture = [&](std::string name, std::string value) {
        eqg::Property p;
        p.name = std::move(name);
        p.type = 2;
        p.text = std::move(value);
        def.properties.push_back(std::move(p));
    };
    texture("e_TextureDiffuse0", "Crystal_C.dds");
    texture("e_TextureNormal0", "Crystal_N.dds");
    eqg::Property shininess;
    shininess.name = "e_fShininess0";
    shininess.type = 0;
    shininess.f = 64.0f;
    def.properties.push_back(shininess);
    bool packed = true;
    bool specular = false;
    SUBCASE("static CBGG1 reads the packed normal and self-light mask") {
        def.shader = "OpaqueSModelCBGG1Max.fx";
    }
    SUBCASE("CBSG1 uses the same packed surface channels") {
        def.shader = "Opaque_MaxCBSG1.fx";
        specular = true;
    }
    SUBCASE("CBSGE1 keeps packed illumination alongside its environment reflection") {
        def.shader = "Opaque_MaxCBSGE1.fx";
        specular = true;
    }
    SUBCASE("CBGGE1 aliases the same native packed environment class") {
        def.shader = "Opaque_OpaqueSkinMeshCBGGE1Max.fx";
    }
    SUBCASE("CBE1 selects the same native environment render class") {
        def.shader = "Opaque_MaxCBE1.fx";
    }
    SUBCASE("CBSE1 selects the same native environment render class") {
        def.shader = "Opaque_MaxCBSE1.fx";
        specular = true;
    }
    SUBCASE("ordinary RGB normal maps retain their existing interpretation") {
        def.shader = "OpaqueSModelCBS1Max.fx";
        packed = false;
        specular = true;
    }
    SUBCASE("an unconfirmed suffix does not enable packed channels") {
        def.shader = "OpaqueSModelCBGG1Custom.fx";
        packed = false;
        specular = false;
    }
    SUBCASE("additive ray geometry keeps its texture-only material") {
        def.shader = "AddAlphaC1Max.fx";
        packed = false;
        specular = false;
    }
    model.materials.push_back(std::move(def));
    eqg::MeshStats stats;
    const auto mesh = eqg::build_mesh(model, "crystal", stats);
    REQUIRE(mesh.materials.size() == 1);
    const auto& effect = mesh.materials.front().effect;
    CHECK(effect.packed_normal == packed);
    CHECK(effect.glow == packed);
    CHECK(effect.specular == specular);
    if (mesh.materials.front().eqg_shader == EqgShader::AddAlpha) {
        CHECK(effect.normal_map.empty());
    } else {
        CHECK(effect.normal_map == "crystal_n.dds");
        CHECK(effect.shininess == 64.0f);
    }
}

TEST_CASE("heightmap light .def: *COLOR is ARGB, scaled by *INTENSITY; broken numbers saturate") {
    auto c = eqg::light_definition_color("*COLOR 4294934528\r\n*INTENSITY 0.5\r\n"); // 0xFFFF8000
    REQUIRE(c);
    CHECK(c->r == doctest::Approx(0.5f));
    CHECK(c->g == doctest::Approx(128.0f / 255.0f * 0.5f));
    CHECK(c->b == doctest::Approx(0.0f));
    CHECK_FALSE(eqg::light_definition_color("*INTENSITY 2\n"));
    // Out of an integer's range: once undefined behaviour in the cast. INT64_MAX's low 32 bits are white.
    auto huge = eqg::light_definition_color("*COLOR 1e400\n*INTENSITY 1e400\n");
    REQUIRE(huge);
    CHECK(huge->r == doctest::Approx(1.0f));
    CHECK(std::isfinite(huge->g));
    auto nan = eqg::light_definition_color("*COLOR nan\n");
    REQUIRE(nan);
    CHECK(nan->r == 0.0f);
}

TEST_CASE("water.dat: finite sheets and *WATERSHEETDATA by *INDEX") {
    const std::string text =
        "*BEGIN_WATERSHEETS\n"
        "\t*WATERSHEET WaterSheet1\n\t\t*MINX -10\n\t\t*MAXX 10\n\t\t*MINY 0\n\t\t*MAXY 5\n\t\t*ZHEIGHT -9\n"
        "\t\t*UVSCALE 3.0\n\t\t*FRESNELBIAS 0.15\n\t\t*WATERCOLOR1 0.5 0.55 0.5 1.0\n"
        "\t\t*NORMALMAP Resources\\WaterSwap\\water_n.dds\n\t*END_SHEET\n"
        "*END_WATERSHEETS\n"
        "*WATERSHEETDATA\n\t*INDEX 0x10\n\t*FRESNELPOWER 8\n*ENDWATERSHEETDATA\n"
        "*WATERSHEETDATA\n\t*FRESNELPOWER 2\n*ENDWATERSHEETDATA\n"
        "*WATERSHEETDATA\n\t*INDEX 16\n\t*FRESNELPOWER 4\n*ENDWATERSHEETDATA\n";
    std::vector<std::byte> data;
    for (char c : text) data.push_back(static_cast<std::byte>(c));
    auto w = eqg::parse_water_file(data);
    REQUIRE(w.sheets.size() == 1);
    CHECK(w.sheets[0].name == "WaterSheet1");
    CHECK(w.sheets[0].min == glm::vec2(-10, 0));
    CHECK(w.sheets[0].height == -9.0f);
    CHECK(w.sheets[0].uv_scale == 3.0f);
    CHECK(w.sheets[0].material.fresnel_bias == doctest::Approx(0.15f));
    CHECK(w.sheets[0].material.water_color1 == glm::vec4(0.5f, 0.55f, 0.5f, 1.0f));
    CHECK(w.sheets[0].material.normal_map == "Resources\\WaterSwap\\water_n.dds");
    REQUIRE(w.data.size() == 3);
    CHECK(w.data[0].index == 16); // hex
    CHECK(w.data[1].index == 1);  // the running counter
    // The first block with the index wins (FindDataByIndex).
    REQUIRE(w.find(16));
    CHECK(w.find(16)->material.fresnel_power == 8.0f);
    CHECK(w.find(1)->material.fresnel_power == 2.0f);
    CHECK(w.find(7) == nullptr);
}

TEST_CASE("DAT region boxes: zOffset is absolute, scale ignored, three angles") {
    eqg::HeightmapTile tile;
    tile.lng = 2;
    tile.lat = -1;
    eqg::HeightmapRegion g;
    g.name = "AWT_pond";
    g.position = {10, 20, -3.5f};
    g.rotation = {90, 45, 30}; // degrees about X, Y, Z as stored
    g.scale = {2, 2, 2};
    g.size = {100, 50, 20};
    auto box = eqg::dat_region_box(g, tile, 256.0f);
    CHECK(box.center == glm::vec3(512 + 10, -256 + 20, -3.5f));
    CHECK(box.extents == glm::vec3(50, 25, 10));
    // angles = (rot2, rot1, rot0) x 1.4222222 in 512ths.
    CHECK(box.angles.x == doctest::Approx(30 * 1.4222222f));
    CHECK(box.angles.y == doctest::Approx(45 * 1.4222222f));
    CHECK(box.angles.z == doctest::Approx(90 * 1.4222222f));
    // A yaw of 90 degrees (128 units) turns the box's long side onto Y.
    g.rotation = {0, 0, 90};
    auto r = ZoneRegions::from_boxes({eqg::dat_region_box(g, tile, 256.0f)});
    CHECK(r.lookup({522, -236 + 45, -3.5f}).is_water());
    CHECK_FALSE(r.lookup({522 + 45, -236, -3.5f}).is_water());
}

TEST_CASE("binary ZON v2 parses placements with lighting, regions and lights") {
    Strings s;
    auto ter = s.add("TER_test.TER");
    auto mod = s.add("OBJ_box.MOD");
    auto pname = s.add("OBJ_box01");
    auto region = s.add("AWT_pool");
    auto light = s.add("LIT_lamp");
    ByteWriter w;
    w.le<std::uint32_t>(eqg::kZonMagic);
    w.le<std::uint32_t>(2);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(s.bytes.size()));
    w.le<std::uint32_t>(2); // models
    w.le<std::uint32_t>(1); // placements
    w.le<std::uint32_t>(1); // regions
    w.le<std::uint32_t>(1); // lights
    w.bytes(s.bytes);
    w.le(ter);
    w.le(mod);
    w.le<std::int32_t>(1);
    w.le(pname);
    vec3(w, 100, 200, 30);
    vec3(w, 1.5707964f, 0, 0);
    w.f32(2.0f);
    w.le<std::uint32_t>(2);
    w.le<std::uint32_t>(0x11223344u);
    w.le<std::uint32_t>(0x55667788u);
    w.le(region);
    vec3(w, 10, 20, 30);
    vec3(w, -1.5707964f, 0, 0);
    vec3(w, 5, -6, 7);
    w.le(light);
    vec3(w, 1, 2, 3);
    vec3(w, 1, 0.5f, 0.25f);
    w.f32(40);
    auto z = eqg::parse_zon(w.data());
    CHECK(z.version == 2);
    CHECK(z.models == std::vector<std::string>{"TER_test.TER", "OBJ_box.MOD"});
    REQUIRE(z.placements.size() == 1);
    CHECK(z.placements[0].name == "OBJ_box01");
    CHECK(z.placements[0].lighting == std::vector<std::uint32_t>{0x11223344u, 0x55667788u});
    REQUIRE(z.regions.size() == 1);
    CHECK(z.regions[0].name == "AWT_pool");
    CHECK(z.regions[0].extents == glm::vec3(5, -6, 7));
    REQUIRE(z.lights.size() == 1);
    CHECK(z.lights[0].radius == 40.0f);

    // Rz(pi/2), scale 2: the model's +X ends up along +Y.
    auto m = eqg::placement_transform(z.placements[0]);
    glm::vec3 p(m * glm::vec4(1, 0, 0, 1));
    CHECK(p.x == doctest::Approx(100.0f));
    CHECK(p.y == doctest::Approx(202.0f));
    CHECK(p.z == doctest::Approx(30.0f));

    // A truncated file is an error, not a crash.
    auto bytes = w.take();
    bytes.resize(bytes.size() - 4);
    CHECK_THROWS(eqg::parse_zon(bytes));
}

TEST_CASE("EQG region boxes: first containing box wins, angles in 512ths, signed extents") {
    // -pi/2 truncates to -1 unit (0.7 degrees), not -90 degrees (OpenEQ EQGZ_NATIVE_REGIONS.md).
    std::vector<RegionBox> boxes{{"APV_skip", {0, 0, 0}, {0, 0, 0}, {100, 100, 100}},
                                 {"AWT_river", {0, 0, 0}, {-1.5707964f, 0, 0}, {6, -117, 35}},
                                 {"ATP_7_out", {0, 0, 0}, {0, 0, 0}, {200, 200, 200}}};
    auto r = ZoneRegions::from_boxes(boxes);
    CHECK_FALSE(r.empty());
    CHECK(r.region_count() == 3);
    auto in_river = r.lookup({0, 100, 0});
    CHECK(in_river.region == 2);
    CHECK(in_river.is_water());
    auto beside = r.lookup({100, 0, 0}); // outside the narrow river box, inside the teleport box
    CHECK(beside.region == 3);
    REQUIRE(beside.is_zone_line());
    CHECK(beside.zone_line->reference);
    CHECK(beside.zone_line->zone_point == 70); // ATP_7: "DRNTP00255%05d0..." reads back as zone point 70
    CHECK(r.lookup({500, 0, 0}).region == 0);
    // The prefix pass (no eqgame caller uses it): a matching box first, even an APV one.
    CHECK(r.region_at({100, 0, 0}, "APV") == 1);
    CHECK(r.region_at({0, 100, 0}, "ATP") == 3);
    CHECK(r.region_at({0, 100, 0}, "AT") == 2); // strncmp 3: "AT\0" != "ATP"; falls back
    CHECK(r.region_at({0, 100, 0}, "XYZ") == 2);
}

TEST_CASE("heightmap ZON, DAT, ECO and TOG parse") {
    const std::string zon_text = "EQTZP\n*NAME test\n*MINLNG -1 *MAXLNG 0\n*MINLAT 0 *MAXLAT 0\n*UNITSPERVERT 8.0\n*QUADSPERTILE 2\n";
    std::vector<std::byte> zon;
    for (char c : zon_text) zon.push_back(static_cast<std::byte>(c));
    REQUIRE(eqg::is_heightmap_zon(zon));
    auto o = eqg::parse_heightmap_zon(zon);
    CHECK(o.name == "test");
    CHECK(o.tile_size() == 16.0f);

    ByteWriter w;
    w.le<std::uint32_t>(20); // version 20: no water extension
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.fixed_string("base_c.dds", 11);
    w.le<std::uint32_t>(1); // tiles
    w.le<std::int32_t>(100000 - 1);
    w.le<std::int32_t>(100000);
    w.le<std::int32_t>(7);
    for (int i = 0; i < 9; ++i) w.f32(static_cast<float>(i)); // heights: row-major, 3x3
    for (int i = 0; i < 18; ++i) w.le<std::uint32_t>(0xff808080u);
    for (std::uint8_t f : std::initializer_list<std::uint8_t>{0x00, 0x80, 0x01, 0x00}) w.le(f);
    w.f32(-5.0f); // water level
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(2); // layers
    w.fixed_string("grass", 6);
    w.fixed_string("rock", 5);
    w.le<std::uint32_t>(2);
    for (std::uint8_t b : std::initializer_list<std::uint8_t>{0, 255, 255, 0}) w.le(b);
    w.le<std::uint32_t>(1); // one object
    w.fixed_string("OBJ_Rock", 9);
    w.fixed_string("grass", 6);
    w.zeros(8);
    vec3(w, 4, 4, 1); // z relative to the ground
    vec3(w, 0, 0, 90);
    vec3(w, 1, 1, 1);
    w.le<std::uint8_t>(0);
    w.le<std::uint32_t>(0); // regions
    w.le<std::uint32_t>(0); // lights
    w.le<std::uint32_t>(1); // groups
    w.fixed_string("camp", 5);
    w.zeros(8);
    vec3(w, 8, 8, 10);
    vec3(w, 0, 0, 0);
    vec3(w, 1, 1, 2);
    w.f32(3.0f); // z adjust, times scale z
    auto map = eqg::parse_heightmap(o, w.data());
    REQUIRE(map.tiles.size() == 1);
    const auto& t = map.tiles[0];
    CHECK(t.lng == -1);
    CHECK(t.lat == 0);
    CHECK(t.layers.size() == 2);
    CHECK(t.layers[1].mask_size == 2);
    CHECK(t.height_at(o, 0, 0) == doctest::Approx(0.0f));
    CHECK(t.height_at(o, 8, 8) == doctest::Approx(4.0f));
    CHECK(t.height_at(o, 4, 0) == doctest::Approx(0.5f));
    REQUIRE(map.objects.size() == 1);
    CHECK(map.objects[0].model == "obj_rock");
    glm::vec3 rock(map.objects[0].transform[3]);
    CHECK(rock.x == doctest::Approx(-16.0f + 4.0f)); // tile origin lng * 16
    CHECK(rock.z == doctest::Approx(t.height_at(o, 4, 4) + 1.0f));
    REQUIRE(map.groups.size() == 1);
    CHECK(glm::vec3(map.groups[0].transform[3]).z == doctest::Approx(16.0f));
    auto truncated = w.take();
    truncated.pop_back();
    CHECK_THROWS(eqg::parse_heightmap(o, truncated));

    const std::string eco_text =
        "*TEXTUREPART\n*LAYER\n*DETAILMAP Textures\\Grass_C.dds\n*DETAILREPEAT 4\n*END_LAYER\n"
        "*LAYER\n*DETAILMAP rock_c.dds\n*MINSLOPE 30\n*END_LAYER\n*END_TEXTUREPART\n*LAYER\n*DETAILMAP ignored.dds\n";
    std::vector<std::byte> eco;
    for (char c : eco_text) eco.push_back(static_cast<std::byte>(c));
    auto layers = eqg::parse_eco(eco);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].detail_map == "grass_c.dds");
    CHECK(layers[0].detail_repeat == 4u);
    CHECK(layers[1].min_slope == 30);

    const std::string tog_text = "*BEGIN_OBJECT\n*NAME Tent\n*POSITION 1 2 3\n*ROTATION 0 0 90\n*SCALE 2\n*END_OBJECT\n";
    std::vector<std::byte> tog;
    for (char c : tog_text) tog.push_back(static_cast<std::byte>(c));
    auto objs = eqg::parse_tog(tog);
    REQUIRE(objs.size() == 1);
    CHECK(objs[0].model == "tent");
    glm::vec3 x(objs[0].transform * glm::vec4(1, 0, 0, 1));
    CHECK(x.x == doctest::Approx(1.0f));
    CHECK(x.y == doctest::Approx(4.0f));

    // The Y angle is negated (CEQTerrainObject::UpdateActorTransform): a +90
    // tilt takes +X up, not down.
    const std::string tilt_text = "*BEGIN_OBJECT\n*NAME Tent\n*POSITION 0 0 0\n*ROTATION 0 90 0\n*SCALE 1\n*END_OBJECT\n";
    std::vector<std::byte> tilt;
    for (char c : tilt_text) tilt.push_back(static_cast<std::byte>(c));
    auto tilted = eqg::parse_tog(tilt);
    REQUIRE(tilted.size() == 1);
    glm::vec3 tx(tilted[0].transform * glm::vec4(1, 0, 0, 1));
    CHECK(tx.x == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(tx.z == doctest::Approx(1.0f));
}

TEST_CASE("heightmap DAT version 21: indexed water and area records") {
    const std::string zon_text = "EQTZP\n*NAME t\n*MINLNG 0 *MAXLNG 0\n*MINLAT 0 *MAXLAT 0\n*UNITSPERVERT 8.0\n*QUADSPERTILE 1\n";
    std::vector<std::byte> zon;
    for (char c : zon_text) zon.push_back(static_cast<std::byte>(c));
    auto o = eqg::parse_heightmap_zon(zon);
    ByteWriter w;
    w.le<std::uint32_t>(21);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.fixed_string("", 1);
    w.le<std::uint32_t>(1);
    w.le<std::int32_t>(100003);
    w.le<std::int32_t>(99999);
    w.le<std::int32_t>(0);
    for (int i = 0; i < 4; ++i) w.f32(1.0f);                  // heights
    for (int i = 0; i < 4; ++i) w.le<std::uint32_t>(0x11223344u); // colour0
    for (int i = 0; i < 4; ++i) w.le<std::uint32_t>(0x55667788u); // colour1
    w.le<std::uint8_t>(0);                                        // quad flags
    w.f32(-7.0f);                                                 // waterElevation
    w.le<std::int32_t>(5);                                        // waterDataIndex
    w.le<std::uint8_t>(1);                                        // hasSheet
    w.f32(1.0f); w.f32(7.0f); w.f32(2.0f); w.f32(6.0f);          // minX maxX minY maxY
    w.f32(0.0f);
    w.le<std::uint32_t>(1);
    w.fixed_string("eco", 4);
    w.le<std::uint32_t>(0); // objects
    w.le<std::uint32_t>(1); // areas
    w.fixed_string("AWT_x", 6);
    w.le<std::uint32_t>(1);
    w.fixed_string("AWT_x", 6);
    w.le<std::int32_t>(100003);
    w.le<std::int32_t>(99999);
    for (float f : {1.0f, 2.0f, -3.0f, 0.0f, 0.0f, 10.0f, 1.0f, 1.0f, 1.0f, 4.0f, 4.0f, 2.0f}) w.f32(f);
    w.le<std::uint32_t>(0); // lights
    w.le<std::uint32_t>(0); // groups
    auto map = eqg::parse_heightmap(o, w.data());
    REQUIRE(map.tiles.size() == 1);
    const auto& t = map.tiles[0];
    CHECK(t.water_level == -7.0f);
    CHECK(t.water_index == 5);
    CHECK(t.has_water_sheet);
    CHECK(t.water_min == glm::vec2(1, 2));
    CHECK(t.water_max == glm::vec2(7, 6));
    CHECK(t.colors2[0] == 0x55667788u);
    REQUIRE(map.regions.size() == 1);
    CHECK(map.regions[0].position == glm::vec3(1, 2, -3));
    CHECK(map.regions[0].rotation.z == 10.0f);
    CHECK(map.regions[0].size == glm::vec3(4, 4, 2));
}

TEST_CASE("heightmap coverage: .eco integers past int32 saturate and the weight does not overflow") {
    auto bytes = [](const std::string& s) {
        std::vector<std::byte> out;
        for (char c : s) out.push_back(static_cast<std::byte>(c));
        return out;
    };
    auto layers = eqg::parse_eco(bytes("*TEXTUREPART\n*LAYER a\n*BLENDSOFTNESS -1e20\n*LAYERINGAREA 1e20\n"
                                       "*MINSLOPE -3e9\n*MAXSLOPE 3e9\n*SLOPETOL 1e30\n*DETAILREPEAT -4\n"
                                       "*NORMALREPEAT 9e9\n*END_LAYER\n*END_TEXTUREPART\n"));
    REQUIRE(layers.size() == 1);
    const auto& l = layers[0];
    CHECK(l.blend_softness == std::numeric_limits<std::int32_t>::min());
    CHECK(l.layering_area == std::numeric_limits<std::int32_t>::max());
    CHECK(l.min_slope == std::numeric_limits<std::int32_t>::min());
    CHECK(l.max_slope == std::numeric_limits<std::int32_t>::max());
    CHECK(l.slope_tolerance == std::numeric_limits<std::int32_t>::max());
    CHECK(l.detail_repeat == 0u);
    CHECK(l.normal_repeat == static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));
    // Bands at the limits hold every slope; the blend's gain is large but its product is worked in 64 bits.
    auto w = eqg::eco_layer_weight(l, 0.0f, 0, -1, 255);
    REQUIRE(w);
    eqg::EcoLayer edge;
    edge.min_height = 0;
    edge.max_height = 100;
    edge.height_tolerance = 10;
    edge.min_slope = std::numeric_limits<std::int32_t>::min();
    edge.max_slope = std::numeric_limits<std::int32_t>::max();
    edge.slope_tolerance = std::numeric_limits<std::int32_t>::max();
    edge.blend_softness = std::numeric_limits<std::int32_t>::min();
    CHECK(eqg::eco_layer_weight(edge, 105.0f, std::numeric_limits<std::int32_t>::max(), -1, 255).value() == 1.0f);
    // w 0.5: gain = trunc(127 x (100 + 2^31) x 0.001 + 1), about 2.7e8; q = (0 + 93) x gain is past int32.
    CHECK(eqg::eco_layer_weight(edge, 105.0f, 0, -1, 0).value() == 1.0f);
}

TEST_CASE("heightmap coverage: CEcoTextureLayer_ComputeCoverage's steps") {
    eqg::EcoLayer l;
    l.min_height = 100;
    l.max_height = 200;
    l.height_tolerance = 10;
    l.min_slope = 20;
    l.max_slope = 40;
    l.slope_tolerance = 10;
    // Height band with linear tolerance ramps; outside the ramps the texel is skipped.
    CHECK(eqg::eco_layer_weight(l, 150, 30).value() == doctest::Approx(1.0f));
    CHECK(eqg::eco_layer_weight(l, 205, 30).value() == doctest::Approx(0.5f));
    CHECK(eqg::eco_layer_weight(l, 92, 30).value() == doctest::Approx(0.2f));
    CHECK_FALSE(eqg::eco_layer_weight(l, 211, 30));
    // Slope band (integers).
    CHECK(eqg::eco_layer_weight(l, 150, 15).value() == doctest::Approx(0.5f));
    CHECK(eqg::eco_layer_weight(l, 150, 47).value() == doctest::Approx(0.3f));
    CHECK_FALSE(eqg::eco_layer_weight(l, 150, 51));
    // LAYERINGMAP: 0 skips, below 255 scales.
    CHECK_FALSE(eqg::eco_layer_weight(l, 150, 30, 0));
    CHECK(eqg::eco_layer_weight(l, 150, 30, 51).value() == doctest::Approx(0.2f));
    // BLENDMAP only between 0.02 and 0.98: w 0.5 -> t = 127, base = trunc(161.29 - 255) = -93,
    // gain = trunc(127 * 100 * 0.001 + 1) = 13; q = (B + 93) * 13.
    CHECK(eqg::eco_layer_weight(l, 205, 30, -1, 255).value() == doctest::Approx(1.0f));
    l.blend_softness = 100; // gain = trunc(0 + 1) = 1
    CHECK(eqg::eco_layer_weight(l, 205, 30, -1, 0).value() == doctest::Approx(93.0f / 255.0f));
    CHECK(eqg::eco_layer_weight(l, 150, 30, -1, 0).value() == doctest::Approx(1.0f)); // w = 1: no blend
    // Accumulation: mask = trunc(w (255 - acc)).
    std::int32_t acc = 0;
    CHECK(eqg::eco_accumulate(0.5f, acc) == 127);
    CHECK(acc == 127);
    CHECK(eqg::eco_accumulate(1.0f, acc) == 128);
    CHECK(acc == 255);
    CHECK(eqg::eco_accumulate(1.0f, acc) == 0);
    acc = 0;
    CHECK(eqg::eco_accumulate(1.0f, acc, 0) == 0); // parent 0 skips
    CHECK(eqg::eco_accumulate(1.0f, acc, 51) == 51);
    CHECK(acc == 255); // trunc(255 / 51 * 51)
    // ColorMap bytes.
    CHECK(eqg::eco_color(10, 200, 2, true) == 0);
    CHECK(eqg::eco_color(10, 200, 2, false) == 10);
    CHECK(eqg::eco_color(10, 200, 0xfd, false) == 200);
    CHECK(eqg::eco_color(0, 200, 128, true) == 100);
    CHECK(eqg::eco_color(200, 200, 128, false) == static_cast<std::uint8_t>(300)); // byte wrap-around
}

// ---- Real files (EQ_GAME_DIR) ----

TEST_CASE("Crescent Reach loads as an EQG zone with water regions" * doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    CHECK(is_eqg_zone(game, "crescent"));
    CHECK_FALSE(is_eqg_zone(game, "qeynos2"));
    auto z = load_zone(game, "crescent");
    REQUIRE(z.terrain.size() == 1);
    CHECK(z.terrain[0].indices.size() / 3 == 67484);
    CHECK(z.placements.size() == 2341);
    CHECK(z.regions.region_count() == 58);
    // AWT_river30 (index 31): narrow in X, long in Y (OpenEQ's orientation fixture).
    glm::vec3 river(-1091.171875f, -2348.66162109375f, -204.40826416015625f);
    CHECK(z.regions.lookup(river + glm::vec3(0, 100, 0)).is_water());
    CHECK_FALSE(z.regions.lookup(river + glm::vec3(100, 0, 0)).is_water());
    // There is a floor below the city.
    auto mesh = game::CollisionMesh::build(z);
    CHECK(mesh.floor_below({-250.0f, -1300.0f, 400.0f}, 0.0f).has_value());
}

TEST_CASE("Anguish lights sit on the geometry after the (y, -x, z) swap" * doctest::skip(!hailstone::test::game_dir())) {
    auto z = load_eqg_zone(*hailstone::test::game_dir(), "anguish");
    CHECK(z.lights.size() == 452);
    std::size_t inside = 0;
    for (const auto& l : z.lights)
        inside += glm::all(glm::greaterThanEqual(l.position, z.bounds_min)) &&
                  glm::all(glm::lessThanEqual(l.position, z.bounds_max));
    CHECK(inside * 100 >= z.lights.size() * 95);
}

TEST_CASE("heightmap zones load: feerrott2 (feerrott.zon) and oldcommons (commonlands.zon)" *
          doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    auto f = load_zone(game, "feerrott2");
    CHECK(f.placements.size() == 10901); // OpenEQ's count for the same archive
    REQUIRE_FALSE(f.terrain.empty());
    CHECK(f.generated_files.size() >= f.terrain[0].materials.size());
    CHECK(f.read_file(f.terrain[0].materials[0].frames[0]).has_value());
    auto o = load_zone(game, "oldcommons");
    CHECK_FALSE(o.terrain.empty());
    CHECK(o.bounds_max.x > o.bounds_min.x);
}

TEST_CASE("heightmap regions: Moors ponds are water, its four zone lines decode" *
          doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    auto z = load_zone(game, "moors");
    auto archive = PfsArchive::open(*game.find("moors.eqg"));
    auto sheets = eqg::parse_water_sheets(*archive.read("water.dat"));
    REQUIRE(sheets.size() == 13);
    // WaterSheet1 (x -1200..-927, y 415..768, z -9) lies in AWT_pond3: its
    // zOffset -1.63 is the absolute box centre (no terrain height added), half
    // height 10, so the box spans z -11.6..8.4.
    glm::vec2 c = (sheets[0].min + sheets[0].max) * 0.5f;
    auto pond = z.regions.lookup({c.x, c.y, sheets[0].height - 2.0f});
    CHECK(pond.is_water());
    CHECK(pond.tag == "AWT_pond3");
    CHECK(z.regions.lookup({c.x, c.y, 8.0f}).is_water());
    CHECK_FALSE(z.regions.lookup({c.x, c.y, -12.0f}).is_water());
    CHECK_FALSE(z.regions.lookup({c.x, c.y, 9.0f}).is_water());
    // Zone-line boxes register first: the river's end at x 2704 is inside ATP_4_Mesa.
    auto mesa = z.regions.lookup({2704.0f, -152.0f, 110.0f});
    REQUIRE(mesa.is_zone_line());
    CHECK(mesa.zone_line->zone_point == 40); // ATP_4_Mesa: zone point 40 (the PEQ moors rows are 10, 20, 30, 40)
    std::size_t lines = 0, water = 0;
    for (const auto& r : z.regions.tagged()) {
        lines += r.is_zone_line();
        water += r.is_water();
    }
    CHECK(lines == 4);
    CHECK(water == 12);
}

TEST_CASE("Zones the live client revamped under the RoF2 name load as S3D, or not at all") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() /
                   ("hailstone_revamped_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto touch = [&](const char* name) {
        fs::create_directories(dir);
        std::ofstream(dir / name) << "x";
    };
    touch("spells_us_str.txt"); // a live folder
    touch("nektulos.s3d");
    touch("nektulos.eqg");
    touch("bazaar.eqg");
    touch("tutoriala.s3d");
    touch("tutoriala.eqg");
    GameDir game(dir);
    CHECK_FALSE(is_eqg_zone(game, "Nektulos")); // revamped: the RoF2 .s3d
    CHECK(is_eqg_zone(game, "tutoriala"));      // RoF2's own rule: the .eqg first
    CHECK(is_revamped_zone_name("bazaar"));
    CHECK_FALSE(is_revamped_zone_name("tutoriala"));
    CHECK_THROWS_WITH(load_zone(game, "bazaar"), doctest::Contains("revamped zone"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("A revamped zone's safe-point test: a floor at most 10 units under the point, or a step up over it") {
    // A floor at z 0 over x, y 0..100.
    std::vector<game::CollisionTriangle> tris;
    for (auto [a, b, c] : {std::array<glm::vec3, 3>{glm::vec3(0, 0, 0), glm::vec3(100, 0, 0), glm::vec3(100, 100, 0)},
                           std::array<glm::vec3, 3>{glm::vec3(0, 0, 0), glm::vec3(100, 100, 0), glm::vec3(0, 100, 0)}}) {
        tris.push_back(game::CollisionTriangle::make(a, b, c, 0));
    }
    auto mesh = game::CollisionMesh::from_triangles(tris);
    CHECK(game::safe_point_on_floor(mesh, {50, 50, 3.6f})); // PEQ's Bazaar version 1 on the live bazaar.eqg
    CHECK(game::safe_point_on_floor(mesh, {50, 50, 10.0f}));
    CHECK(game::safe_point_on_floor(mesh, {50, 50, -3.0f}));        // within the step up
    CHECK_FALSE(game::safe_point_on_floor(mesh, {50, 50, 15.9f}));  // Nektulos version 0 on the live nektulos.eqg
    CHECK_FALSE(game::safe_point_on_floor(mesh, {50, 50, -5.0f}));  // under the floor
    CHECK_FALSE(game::safe_point_on_floor(mesh, {150, 50, 3.0f}));  // no floor (Bazaar version 0)
    REQUIRE(game::safe_point_height(mesh, {50, 50, 3.0f}));
    CHECK(*game::safe_point_height(mesh, {50, 50, 3.0f}) == doctest::Approx(3.0f));
}

namespace {

// A binary-ZON zone archive <zone>.eqg in `dir` with one terrain: a 100 x 100 floor.
void write_floor_zone(const std::filesystem::path& dir, const std::string& zone) {
    std::vector<glm::vec3> pos{{0, 0, 0}, {100, 0, 0}, {100, 100, 0}, {0, 100, 0}};
    std::vector<glm::vec2> uv{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    auto ter = make_model(true, 1, {{"ground", "Opaque_MPLBasic.fx", "ground_c.dds"}}, pos, uv,
                          {{0, 1, 2, 0, 0}, {0, 2, 3, 0, 0}});
    Strings s;
    auto name = s.add("TER_floor.TER");
    ByteWriter w;
    w.le<std::uint32_t>(eqg::kZonMagic);
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(s.bytes.size()));
    w.le<std::uint32_t>(1); // models
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.bytes(s.bytes);
    w.le(name);
    auto eqg = hailstone::test::pfs_fixtures::make_pfs({{zone + ".zon", w.take()}, {"ter_floor.ter", ter}});
    std::ofstream f(dir / (zone + ".eqg"), std::ios::binary);
    f.write(reinterpret_cast<const char*>(eqg.data()), static_cast<std::streamsize>(eqg.size()));
}

} // namespace

TEST_CASE("A live folder's revamped .eqg loads when the server's safe point stands on its floor") {
    hailstone::test::ScratchDir dir("eqg_revamped_safe");
    write_floor_zone(dir.path(), "bazaar");
    write_floor_zone(dir.path(), "tutoriala");
    std::ofstream(dir.path() / "spells_us_str.txt") << "x"; // a live folder
    GameDir game(dir.path());
    // Where the floor is in world coordinates, from the zone itself.
    const Zone plain = load_eqg_zone(game, "bazaar");
    const glm::vec3 mid = (plain.bounds_min + plain.bounds_max) * 0.5f;
    const float floor_z = plain.bounds_max.z;

    // No word from the server: refused, as before.
    CHECK_THROWS_WITH(load_zone(game, "bazaar"), doctest::Contains("revamped zone"));
    // The safe point stands on the floor: the zone loads, and the test's collision mesh is there to keep.
    {
        std::optional<game::CollisionMesh> built;
        auto zone = load_zone(game, "Bazaar", {}, game::safe_point_test({mid.x, mid.y, floor_z + 3.0f}, built));
        CHECK(zone.terrain.size() == 1);
        REQUIRE(built);
        CHECK(built->floor_below({mid.x, mid.y, floor_z + 3.0f}, 0.0f));
    }
    // Too high over the floor, or off it: refused.
    {
        std::optional<game::CollisionMesh> built;
        CHECK_THROWS_WITH(load_zone(game, "bazaar", {}, game::safe_point_test({mid.x, mid.y, floor_z + 30.0f}, built)),
                          doctest::Contains("does not match the server's safe point"));
        CHECK_THROWS_WITH(load_zone(game, "bazaar", {},
                                    game::safe_point_test({plain.bounds_max.x + 50.0f, mid.y, floor_z + 3.0f}, built)),
                          doctest::Contains("does not match the server's safe point"));
    }
    // A zone that is not revamped loads by the original's rule; the test is not asked.
    bool asked = false;
    auto tutorial = load_zone(game, "tutoriala", {}, [&](const Zone&) { return asked = true; });
    CHECK(tutorial.terrain.size() == 1);
    CHECK_FALSE(asked);
    // The server's own files bring the .eqg: loaded without asking (GameDir::from_overlay).
    hailstone::test::ScratchDir overlay("eqg_revamped_overlay");
    write_floor_zone(overlay.path(), "bazaar");
    GameDir served(dir.path(), overlay.path());
    auto from_server = load_zone(served, "bazaar", {}, [&](const Zone&) { return asked = true; });
    CHECK(from_server.terrain.size() == 1);
    CHECK_FALSE(asked);
}

TEST_CASE("A revamped zone with both files: the server's safe point picks the .eqg or the .s3d") {
    hailstone::test::ScratchDir dir("eqg_revamped_both");
    write_floor_zone(dir.path(), "nektulos");
    std::ofstream(dir.path() / "nektulos.s3d") << "x"; // not a real archive: loading it throws, which marks the path
    std::ofstream(dir.path() / "spells_us_str.txt") << "x"; // a live folder
    GameDir game(dir.path());
    CHECK_FALSE(is_eqg_zone(game, "nektulos")); // by name alone the .s3d still wins
    const Zone plain = load_eqg_zone(game, "nektulos");
    CHECK(plain.from_eqg);
    const glm::vec3 mid = (plain.bounds_min + plain.bounds_max) * 0.5f;
    const float floor_z = plain.bounds_max.z;
    auto loads_s3d = [](auto&& load) {
        try {
            load();
        } catch (const std::exception& e) {
            return std::string(e.what()).find("revamped zone") == std::string::npos; // not the refusal
        }
        return false;
    };

    // The safe point stands on the revamp's floor (PEQ's Nektulos version 1): the .eqg.
    {
        std::optional<game::CollisionMesh> built;
        auto zone = load_zone(game, "Nektulos", {}, game::safe_point_test({mid.x, mid.y, floor_z + 3.0f}, built));
        CHECK(zone.from_eqg);
        CHECK(zone.terrain.size() == 1);
    }
    // It does not (Nektulos version 0, 15.9 units over the revamp's floor): the .s3d, not a refusal.
    {
        std::optional<game::CollisionMesh> built;
        CHECK(loads_s3d([&] {
            load_zone(game, "nektulos", {}, game::safe_point_test({mid.x, mid.y, floor_z + 15.9f}, built));
        }));
    }
    // No word from the server: the .s3d, without loading the .eqg to ask.
    CHECK(loads_s3d([&] { load_zone(game, "nektulos"); }));
    // The server's assets bring the .s3d: it loads, and the test is not asked.
    bool asked = false;
    {
        hailstone::test::ScratchDir overlay("eqg_revamped_both_s3d");
        std::ofstream(overlay.path() / "nektulos.s3d") << "x";
        GameDir served(dir.path(), overlay.path());
        CHECK(loads_s3d([&] { load_zone(served, "nektulos", {}, [&](const Zone&) { return asked = true; }); }));
        CHECK_FALSE(asked);
    }
    // They bring the .eqg: it loads without asking.
    {
        hailstone::test::ScratchDir overlay("eqg_revamped_both_eqg");
        write_floor_zone(overlay.path(), "nektulos");
        GameDir served(dir.path(), overlay.path());
        auto zone = load_zone(served, "nektulos", {}, [&](const Zone&) { return asked = true; });
        CHECK(zone.from_eqg);
        CHECK_FALSE(asked);
    }
}

TEST_CASE("The live bazaar.eqg: PEQ's version 1 safe point stands on it, version 0's does not" *
          doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    if (!game.exists("bazaar.eqg") || game.exists("bazaar.s3d")) return; // a RoF2 folder: nothing to test
    auto zone = load_eqg_zone(game, "bazaar");
    auto mesh = game::CollisionMesh::build(zone);
    // PEQ's zone table, server x y z: version 1 (expansion 9 on) and version 0 (expansions 3 to 8).
    CHECK(game::safe_point_on_floor(mesh, game::server_to_world({-71.0f, -250.0f, 33.0f})));
    CHECK_FALSE(game::safe_point_on_floor(mesh, game::server_to_world({140.0f, -821.0f, 5.0f})));
}

TEST_CASE("EQG character meshes: with bones v runs up from the texture's bottom (1 - v), without it is as stored") {
    std::vector<glm::vec3> pos{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    std::vector<glm::vec2> uv{{0.25f, 0.1f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    auto data = make_model(false, 1, {{"body", "Opaque_MaxCBS1.fx", "c_pma_bd_s00_c.dds"}}, pos, uv, {{0, 1, 2, 0, 0}});
    // A static piece (a held item, a hero's forge helm): v as stored, as the zones' objects read it.
    auto still = eqg_character::parse_mesh(data, "IT1");
    REQUIRE(still.pieces.size() == 1);
    REQUIRE(still.pieces[0].vertices.size() == 3);
    CHECK(still.pieces[0].vertices[0].uv == glm::vec2(0.25f, 0.1f));
    CHECK(still.pieces[0].vertices[2].uv == glm::vec2(0.5f, 1.0f));
    // The same mesh with one bone (the count after magic, version, string, material, vertex and triangle counts), the
    // bone's record and each vertex's weights: a skinned piece, v turned.
    ByteWriter n;
    n.le<std::uint32_t>(1);
    std::copy(n.data().begin(), n.data().end(), data.begin() + 24);
    ByteWriter extra;
    extra.le<std::int32_t>(0);  // name
    extra.le<std::int32_t>(-1); // next sibling
    extra.le<std::uint32_t>(0); // children
    extra.le<std::int32_t>(-1); // first child
    for (float f : {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f}) extra.f32(f);
    for (int i = 0; i < 3; ++i) {
        extra.le<std::uint32_t>(1);
        for (int k = 0; k < 4; ++k) {
            extra.le<std::int32_t>(0);
            extra.f32(k == 0 ? 1.0f : 0.0f);
        }
    }
    data.insert(data.end(), extra.data().begin(), extra.data().end());
    auto skinned = eqg_character::parse_mesh(data, "IT1");
    REQUIRE(skinned.bones.size() == 1);
    const auto& v = skinned.pieces.at(0).vertices;
    REQUIRE(v.size() == 3);
    CHECK(v[0].uv == glm::vec2(0.25f, 0.9f));
    CHECK(v[1].uv == glm::vec2(1.0f, 1.0f));
    CHECK(v[2].uv == glm::vec2(0.5f, 0.0f));
}

TEST_CASE("EQG string tables: a name is cut at 256 bytes, however many references share a long run") {
    ByteWriter w;
    const std::string run(100000, 'm');
    w.le(eqg::kZonMagic);
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(run.size() + 1));
    w.le<std::uint32_t>(10000); // models
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.fixed_string(run, run.size() + 1);
    for (std::int32_t i = 0; i < 10000; ++i) w.le(i);
    auto z = eqg::parse_zon(w.data());
    REQUIRE(z.models.size() == 10000);
    for (const auto& m : z.models) CHECK(m == std::string(256, 'm'));
}

TEST_CASE("unpainted heightmap tiles take the nearest painted tile") {
    std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> painted{{{0, 0}, 0}, {{3, 0}, 1}, {{2, 2}, 2}};
    CHECK(eqg::nearest_painted_tile(1, 0, painted) == 0u);
    CHECK(eqg::nearest_painted_tile(2, 0, painted) == 1u);  // (3, 0) is one away, (2, 2) two
    CHECK(eqg::nearest_painted_tile(3, 2, painted) == 2u);  // ring 1: (2, 2) straight beside it
    CHECK(eqg::nearest_painted_tile(1, 1, painted) == 0u);  // ring 1 holds (0, 0) and (2, 2), diagonal both: the first
    CHECK_FALSE(eqg::nearest_painted_tile(40, 40, painted).has_value());
    // A corrupt tile's index at the edge of int32: its neighbours past the edge are none, without overflowing.
    const std::int32_t lo = std::numeric_limits<std::int32_t>::min(), hi = std::numeric_limits<std::int32_t>::max();
    std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> edge{{{lo + 1, hi}, 7}};
    CHECK(eqg::nearest_painted_tile(lo, hi, edge) == 7u);
    CHECK_FALSE(eqg::nearest_painted_tile(hi, lo, edge).has_value());
}

TEST_CASE("animated texture sidecars list their frames") {
    auto bytes = [](std::string_view s) {
        std::vector<std::byte> out;
        for (char c : s) out.push_back(static_cast<std::byte>(c));
        return out;
    };
    auto a = eqg::parse_texture_animation(bytes("10\r\n200\r\nblood01_c.dds\r\nBlood02_C.dds\r\nblood03_c.dds\r\n"));
    REQUIRE(a);
    CHECK(a->delay_ms == 200);
    CHECK(a->frames == std::vector<std::string>{"blood01_c.dds", "blood02_c.dds", "blood03_c.dds"});
    // The count wins over a longer list.
    auto b = eqg::parse_texture_animation(bytes("02\n150\na.dds\nb.dds\nc.dds\n"));
    REQUIRE(b);
    CHECK(b->frames.size() == 2);
    CHECK_FALSE(eqg::parse_texture_animation(bytes("1\n100\na.dds\n")));
    CHECK_FALSE(eqg::parse_texture_animation(bytes("3\n0\na.dds\nb.dds\n")));
    CHECK_FALSE(eqg::parse_texture_animation(bytes("not a list\n")));
}

TEST_CASE("EQG part bones: parents from the child lists; a cyclic hierarchy is refused") {
    auto bone = [](std::int32_t next, std::uint32_t children, std::int32_t child) {
        eqg_character::Bone b;
        b.next = next;
        b.children = children;
        b.child = child;
        return b;
    };
    eqg_character::MeshFile m;
    m.bones = {bone(-1, 2, 1), bone(2, 1, 3), bone(-1, 0, -1), bone(-1, 0, -1)}; // 0 -> {1, 2}, 1 -> {3}
    auto p = eqg_character::part_bone_parents(m);
    REQUIRE(p);
    CHECK(*p == std::vector<std::int32_t>{-1, 0, 0, 1});
    // A bad child index ends the list; a bone claiming four billion children is followed only as far as the list goes.
    m.bones = {bone(-1, 0xffffffffu, 1), bone(7, 0, -1)};
    p = eqg_character::part_bone_parents(m);
    REQUIRE(p);
    CHECK(*p == std::vector<std::int32_t>{-1, 0});
    // 0 and 1 each other's child: no root to walk up to.
    m.bones = {bone(-1, 1, 1), bone(-1, 1, 0)};
    CHECK_FALSE(eqg_character::part_bone_parents(m));
    // A sibling list that loops on itself (1 -> 2 -> 1) is cut at the bone count.
    m.bones = {bone(-1, 0xffffffffu, 1), bone(2, 0, -1), bone(1, 0, -1)};
    p = eqg_character::part_bone_parents(m);
    REQUIRE(p);
    CHECK(*p == std::vector<std::int32_t>{-1, 0, 0});
}

TEST_CASE("EQG attachments on WLD bodies retain four independent bind-space influences") {
    eqg_character::MeshFile file;
    std::vector<std::string> body;
    for (int i = 0; i < 4; ++i) {
        eqg_character::Bone bone;
        bone.name = "Arm" + std::to_string(i);
        bone.bind.translation.x = static_cast<float>(i + 1);
        file.bones.push_back(bone);
        body.push_back("TSTARM" + std::to_string(i) + "_TRACK");
    }
    CharacterMesh mesh;
    SkinnedVertex vertex;
    vertex.position = {5, 7, 2};
    vertex.bones = {0, 1, 2, 3};
    vertex.weights = {0.1f, 0.2f, 0.3f, 0.4f};
    mesh.vertices.push_back(vertex);
    const auto adjustment = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 3));
    REQUIRE(eqg_character::bind_part_to_wld(file, "TST", body, 0, adjustment, mesh));
    REQUIRE(mesh.skin_bindings.size() == 4);
    CHECK(mesh.vertices[0].position == vertex.position);
    CHECK(mesh.vertices[0].weights == vertex.weights);
    const auto evaluate = [&](float motion) {
        std::vector<glm::mat4> posed;
        for (int i = 0; i < 4; ++i)
            posed.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(10.0f * static_cast<float>(i + 1) + motion, 0, 0)));
        REQUIRE(mesh.append_skin_bones(posed));
        glm::vec4 point(0.0f);
        const auto& v = mesh.vertices[0];
        for (int k = 0; k < 4; ++k) point += posed[v.bones[static_cast<std::size_t>(k)]] * glm::vec4(v.position, 1) * v.weights[k];
        return glm::vec3(point);
    };
    // Each body joint moved by 9*(i+1) from its own authored bind. The
    // weighted displacement is 27, rather than the dominant joint's 36.
    CHECK(glm::length(evaluate(0) - glm::vec3(32, 7, 5)) < 1e-4f);
    CHECK(glm::length(evaluate(10) - glm::vec3(42, 7, 5)) < 1e-4f);
}

TEST_CASE("EQG attachment bindings retain the ancestor rule and reject invalid palettes") {
    eqg_character::MeshFile file;
    eqg_character::Bone root, child;
    root.name = "Arm";
    root.bind.translation = {1, 0, 0};
    root.children = 1;
    root.child = 1;
    child.name = "Unknown";
    child.bind.translation = {3, 0, 0};
    file.bones = {root, child};
    const std::vector<std::string> body{"TSTARM_TRACK"};
    CharacterMesh mesh;
    mesh.vertices.resize(1);
    mesh.vertices[0].position = {5, 0, 0};
    mesh.vertices[0].bones[0] = 1;
    REQUIRE(eqg_character::bind_part_to_wld(file, "TST", body, 0, glm::mat4(1.0f), mesh));
    std::vector<glm::mat4> pose{glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0))};
    REQUIRE(mesh.append_skin_bones(pose));
    CHECK(glm::vec3(pose[mesh.vertices[0].bones[0]] * glm::vec4(5, 0, 0, 1)) == glm::vec3(14, 0, 0));
    mesh.skin_bindings[0].host_bone = 1; // an attachment cannot refer to another attachment entry
    pose.resize(1);
    CHECK_FALSE(mesh.append_skin_bones(pose));
    CHECK(pose.size() == 1);
    file.bones[1].children = 1;
    file.bones[1].child = 0;
    CharacterMesh refused;
    CHECK_FALSE(eqg_character::bind_part_to_wld(file, "TST", body, 0, glm::mat4(1.0f), refused));
}

TEST_CASE("EQG role zero resolves only an authored HEAD_NAME bone") {
    const std::vector<glm::vec3> positions{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const std::vector<glm::vec2> uv{{0, 0}, {1, 0}, {0, 1}};
    auto data = make_model(false, 1, {{"body", "Opaque.fx", "fixture.dds"}}, positions, uv, {{0, 1, 2, 0, 0}});
    auto file = eqg_character::parse_mesh(data, "TST");
    file.bones.resize(1);
    file.bones[0].name = "HEAD_NAME";
    CHECK(eqg_character::build_model(file, "TST", {}).name_bone == 0);
    file.bones[0].name = "SPELLPOINT_HEAD";
    CHECK(eqg_character::build_model(file, "TST", {}).name_bone == -1);
}

TEST_CASE("EQG character meshes: a vertex count the file cannot hold is refused before it is allocated") {
    std::vector<glm::vec3> pos{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    std::vector<glm::vec2> uv{{0, 0}, {1, 0}, {0, 1}};
    for (std::uint32_t version : {1u, 3u}) {
        auto data = make_model(false, version, {{"body", "Opaque_MaxCBS1.fx", "c_pma_bd_s00_c.dds"}}, pos, uv, {{0, 1, 2, 0, 0}});
        CHECK(eqg_character::parse_mesh(data, "IT1").pieces.at(0).vertices.size() == 3);
        // The vertex count (after magic, version, string and material counts): as large as the file itself.
        ByteWriter n;
        n.le(static_cast<std::uint32_t>(data.size()));
        std::copy(n.data().begin(), n.data().end(), data.begin() + 16);
        CHECK_THROWS_WITH(eqg_character::parse_mesh(data, "IT1"), doctest::Contains("vertex count"));
    }
}

TEST_CASE("binary ZON: a terrain listed many times is loaded once") {
    std::vector<glm::vec3> pos{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}};
    std::vector<glm::vec2> uv{{0, 0}, {1, 0}, {1, 1}};
    auto ter = make_model(true, 1, {{"ground", "Opaque_MPLBasic.fx", "ground_c.dds"}}, pos, uv, {{0, 1, 2, 0, 0}});
    Strings s;
    auto name = s.add("TER_dup.TER");
    ByteWriter w;
    w.le<std::uint32_t>(eqg::kZonMagic);
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(s.bytes.size()));
    w.le<std::uint32_t>(50); // models: the same terrain fifty times
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.bytes(s.bytes);
    for (int i = 0; i < 50; ++i) w.le(name);
    hailstone::test::ScratchDir dir("eqg_ter_dup");
    {
        auto eqg = hailstone::test::pfs_fixtures::make_pfs({{"terdup.zon", w.take()}, {"ter_dup.ter", ter}});
        std::ofstream f(dir.path() / "terdup.eqg", std::ios::binary);
        f.write(reinterpret_cast<const char*>(eqg.data()), static_cast<std::streamsize>(eqg.size()));
    }
    GameDir game(dir.path());
    auto zone = load_zone(game, "terdup");
    CHECK(zone.terrain.size() == 1);
}

TEST_CASE("binary ZON terrain keeps its file's baked colour and sky-light share without a placement transform") {
    const std::vector<std::uint32_t> sidecar_words{0x00102030u, 0x80405060u, 0xe0a0b0c0u};
    auto make_lit = [](const std::vector<std::uint32_t>& words) {
        ByteWriter w;
        w.le<std::uint32_t>(0x50475145u); // EQGP
        w.le<std::uint32_t>(static_cast<std::uint32_t>(words.size()));
        for (auto word : words) w.le(word);
        return w.take();
    };
    std::uint32_t version = 1, model_version = 1;
    std::vector<std::uint32_t> inline_words;
    auto sidecar = make_lit(sidecar_words);
    bool include_sidecar = true, include_placement = true;
    std::vector<std::uint32_t> expected{0x00302010u, 0x80605040u, 0xe0c0b0a0u};

    SUBCASE("v1 reads the sidecar named by the terrain file") {}
    SUBCASE("v2 terrain placement lighting does not replace the file's sidecar") {
        version = 2;
        inline_words = {0x00010203u, 0x40040506u, 0xc0070809u};
    }
    SUBCASE("a terrain file without a ZON placement still reads its sidecar") {
        include_placement = false;
    }
    SUBCASE("a sidecar with the wrong vertex count leaves the mesh uncoloured") {
        sidecar = make_lit({0x00112233u});
        expected.assign(3, 0xffffffffu);
    }
    SUBCASE("a truncated sidecar leaves the mesh uncoloured") {
        sidecar.pop_back();
        expected.assign(3, 0xffffffffu);
    }
    SUBCASE("an unrecognised sidecar leaves the mesh uncoloured") {
        sidecar[0] = std::byte{0};
        expected.assign(3, 0xffffffffu);
    }
    SUBCASE("a missing sidecar leaves the mesh uncoloured") {
        include_sidecar = false;
        expected.assign(3, 0xffffffffu);
    }
    SUBCASE("a missing sidecar preserves a v3 terrain's inline vertex colours") {
        include_sidecar = false;
        model_version = 3;
        expected.assign(3, 0xff302010u);
    }

    const std::vector<glm::vec3> pos{{5, 7, 9}, {15, 7, 9}, {15, 17, 9}};
    auto ter = make_model(true, model_version, {{"ground", "Opaque_MaxCB1.fx", "ground_c.dds"}}, pos,
                          {{0, 0}, {1, 0}, {1, 1}}, {{0, 1, 2, 0, 0}});
    Strings s;
    auto file = s.add("TER_Only.TER"), placement = s.add("TER_Lighting");
    ByteWriter w;
    w.le(eqg::kZonMagic);
    w.le(version);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(s.bytes.size()));
    w.le<std::uint32_t>(2); // duplicate model references still draw one terrain
    w.le<std::uint32_t>(include_placement ? 2 : 0);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.bytes(s.bytes);
    w.le(file);
    w.le(file);
    for (std::int32_t i = 0; include_placement && i < 2; ++i) {
        w.le(i);
        w.le(placement);
        vec3(w, 500, 700, 900);
        vec3(w, 1, 2, 3);
        w.f32(7.0f);
        if (version == 2) {
            w.le<std::uint32_t>(static_cast<std::uint32_t>(inline_words.size()));
            for (auto word : inline_words) w.le(word);
        }
    }
    std::vector<std::pair<std::string, std::vector<std::byte>>> files{
        {"terlit.zon", w.take()}, {"ter_only.ter", std::move(ter)},
        {"ter_lighting.lit", make_lit({0x00010203u, 0x40040506u, 0xc0070809u})}};
    if (include_sidecar) files.emplace_back("ter_only.lit", std::move(sidecar));
    hailstone::test::ScratchDir dir("eqg_terrain_lighting");
    {
        const auto archive = hailstone::test::pfs_fixtures::make_pfs(files);
        std::ofstream f(dir.path() / "terlit.eqg", std::ios::binary);
        f.write(reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size()));
    }
    const auto zone = load_zone(GameDir(dir.path()), "terlit");
    REQUIRE(zone.terrain.size() == 1);
    CHECK(zone.placements.empty());
    const auto& vertices = zone.terrain.front().vertices;
    REQUIRE(vertices.size() == 3);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        CHECK(vertices[i].color == expected[i]);
        CHECK(vertices[i].position == pos[i]);
    }
}

TEST_CASE("binary ZON: a .mod that is missing or does not parse is tried once however often it is listed") {
    Strings s;
    auto bad = s.add("bad.mod");
    auto gone = s.add("gone.mod");
    ByteWriter w;
    w.le<std::uint32_t>(eqg::kZonMagic);
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(s.bytes.size()));
    w.le<std::uint32_t>(100); // models: a broken .mod fifty times, then a missing one fifty times
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.bytes(s.bytes);
    for (int i = 0; i < 50; ++i) w.le(bad);
    for (int i = 0; i < 50; ++i) w.le(gone);
    hailstone::test::ScratchDir dir("eqg_mod_dup");
    {
        std::vector<std::byte> garbage(64, std::byte{0x5a});
        auto eqg = hailstone::test::pfs_fixtures::make_pfs({{"moddup.zon", w.take()}, {"bad.mod", garbage}});
        std::ofstream f(dir.path() / "moddup.eqg", std::ios::binary);
        f.write(reinterpret_cast<const char*>(eqg.data()), static_cast<std::streamsize>(eqg.size()));
    }
    GameDir game(dir.path());
    auto zone = load_zone(game, "moddup");
    CHECK(zone.objects.empty());
    // The log keeps its last 16 lines: one failed parse and one missing model, not one per entry.
    char recent[4096];
    hailstone::log::copy_recent_lines(recent, sizeof recent);
    std::string lines(recent);
    std::size_t parses = 0;
    for (auto at = lines.find("model bad.mod:"); at != std::string::npos; at = lines.find("model bad.mod:", at + 1))
        ++parses;
    CHECK(parses == 1);
    CHECK(lines.find("1 of 100 models missing") != std::string::npos);
}
