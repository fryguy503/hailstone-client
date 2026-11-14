#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

#include "assets/wld.h"
#include "assets/zone.h"
#include "core/byte_io.h"
#include "game_files.h"
#include "scratch_dir.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::assets;

using namespace hailstone::test::wld_fixtures;

TEST_CASE("WldFile rejects data that is not WLD") {
    std::vector<std::byte> junk(64, std::byte{0x11});
    CHECK_THROWS(wld::WldFile::parse(junk));
    WldBuilder b;
    auto data = b.build();
    data.resize(10);
    CHECK_THROWS(wld::WldFile::parse(data));
}

TEST_CASE("WldFile decodes the string table and resolves signed references") {
    WldBuilder b;
    auto name = b.str("TREE_ACTORDEF");
    ByteWriter body;
    body.le(std::uint32_t{0});
    auto first = b.add(0x99, name, body);
    auto second = b.add(0x2d, b.str("TREE_DMSPRITE"), refs({}, 0));
    auto w = wld::WldFile::parse(b.build());
    REQUIRE(w.fragments().size() == 2);
    CHECK(w.fragments()[0].name == "TREE_ACTORDEF");
    CHECK(w.string_at(name) == "TREE_ACTORDEF");
    CHECK(w.string_at(name - 5) == "ACTORDEF"); // a substring reference
    CHECK(w.resolve(first) == &w.fragments()[0]);
    CHECK(w.resolve(name) == &w.fragments()[0]);
    CHECK(w.resolve(second)->type == 0x2d);
    CHECK(w.find("TREE_DMSPRITE") == &w.fragments()[1]);
    CHECK(w.resolve(0) == nullptr);
    CHECK(w.resolve(3) == nullptr);
    CHECK(w.resolve(-12345) == nullptr);
    CHECK(w.resolve(std::numeric_limits<std::int32_t>::min()) == nullptr);
    CHECK(w.unknown_types().at(0x99) == 1);
}

TEST_CASE("WldFile cuts a name at kMaxStringLength: references into one long run copy a bounded name each") {
    WldBuilder b;
    auto run = b.str(std::string(200000, 'A'));
    ByteWriter body;
    body.le(std::uint32_t{0});
    // 20,000 fragments naming the run (and points inside it): once each copied the rest of the run, 4 GB in all.
    for (int i = 0; i < 20000; ++i) b.add(0x99, run - i, body);
    auto w = wld::WldFile::parse(b.build());
    REQUIRE(w.fragments().size() == 20000);
    for (const auto& f : w.fragments()) CHECK(f.name == std::string(wld::WldFile::kMaxStringLength, 'A'));
    CHECK(w.string_at(run - 199990) == std::string(10, 'A')); // a short tail is all there is
}

TEST_CASE("WldFile bounds each fragment to its declared size") {
    WldBuilder b;
    ByteWriter only_name;
    only_name.le(std::int32_t{0});
    b.add_raw(0x2d, only_name.take()); // a mesh reference with no fields
    ByteWriter next;
    next.le(std::int32_t{7});
    next.le(std::uint32_t{0});
    b.add(0x2d, 0, next);
    auto w = wld::WldFile::parse(b.build());
    REQUIRE(w.errors().size() == 1);
    CHECK(w.errors()[0].index == 0);
    CHECK(w.fragments()[0].holds<std::monostate>());
    CHECK(w.fragments()[1].as<wld::MeshRef>().mesh == 7);

    // A fragment whose size runs past the end of the file is a framing error.
    auto data = b.build();
    data.resize(data.size() - 2);
    CHECK_THROWS(wld::WldFile::parse(data));
}

TEST_CASE("build_mesh resolves runs positionally across empty slots and groups them per material") {
    for (bool new_format : {false, true}) {
        CAPTURE(new_format);
        auto data = mesh_wld(new_format);
        auto w = wld::WldFile::parse(data);
        REQUIRE(w.errors().empty());
        CHECK(w.new_format() == new_format);
        const auto& frag = w.fragments().back();
        auto m = wld::build_mesh(w, frag.as<wld::MeshDef>(), frag.name);

        REQUIRE(m.materials.size() == 2); // the empty middle slot is unused
        CHECK(m.materials[0].name == "WATER_MDF");
        CHECK(m.materials[0].blend == BlendMode::Opaque);
        CHECK(m.materials[0].frames == std::vector<std::string>{"water1.bmp", "water2.bmp"});
        CHECK(m.materials[0].frame_delay_ms == 150);
        CHECK(m.materials[0].raw_render_method == 0x80000001);
        CHECK(m.materials[1].name == "BOUND_MDF");
        CHECK(m.materials[1].blend == BlendMode::Invisible);

        REQUIRE(m.vertices.size() == 4);
        CHECK(m.vertices[1].position == glm::vec3(101.0f, 200.0f, 300.0f));
        CHECK(m.vertices[3].position == glm::vec3(99.0f, 200.0f, 303.0f));
        CHECK(m.vertices[2].uv.x == doctest::Approx(1.0f));
        CHECK(m.vertices[2].uv.y == doctest::Approx(-1.0f));
        CHECK(m.vertices[0].normal.z == doctest::Approx(1.0f));
        CHECK(m.vertices[0].color == 0x80332211u);

        REQUIRE(m.parts.size() == 2);
        CHECK(m.parts[0].material == 0);
        CHECK(m.parts[0].first_index == 0);
        CHECK(m.parts[0].index_count == 3);
        CHECK(m.parts[1].material == 1);
        CHECK(m.parts[1].index_count == 6);
        CHECK(m.indices == std::vector<std::uint32_t>{1, 2, 3, 0, 1, 2, 0, 2, 3});
    }
}

TEST_CASE("build_skeleton_pose composes parent and child rest frames") {
    WldBuilder b;
    auto list = b.add(0x31, b.str("TREE_MP"), refs({0, 0, 0}));
    auto mesh_def = b.add(0x36, b.str("TREE_DMSPRITEDEF"), mesh(list, false));
    ByteWriter mesh_ref;
    mesh_ref.le(mesh_def);
    mesh_ref.le(std::uint32_t{0});
    auto sprite = b.add(0x2d, 0, mesh_ref);
    // Packed keys: w, x, y, z (/16384), translation (/256), scale (/256).
    auto track = [&](std::int16_t w, std::int16_t z, std::int16_t tx, std::int16_t tz) {
        ByteWriter def;
        def.le(std::uint32_t{0x8});
        def.le(std::uint32_t{1});
        for (std::int16_t v : {w, std::int16_t{0}, std::int16_t{0}, z, tx, std::int16_t{0}, tz}) def.le(v);
        def.le(std::uint16_t{256});
        auto d = b.add(0x12, 0, def);
        ByteWriter ref;
        ref.le(d);
        ref.le(std::uint32_t{0});
        return b.add(0x13, 0, ref);
    };
    auto root = track(16384, 0, 0, 10 * 256);
    auto child = track(11585, 11585, 256, 0); // a quarter turn around Z, then x + 1
    ByteWriter sk;
    sk.le(std::uint32_t{0});
    sk.le(std::uint32_t{2});
    sk.le(std::int32_t{0});
    for (auto [name, tr, spr, kid] : {std::tuple{b.str("ROOT"), root, 0, 1}, std::tuple{b.str("BR1"), child, sprite, -1}}) {
        sk.le(name);
        sk.le(std::uint32_t{0});
        sk.le(tr);
        sk.le(static_cast<std::int32_t>(spr));
        sk.le(static_cast<std::uint32_t>(kid >= 0 ? 1 : 0));
        if (kid >= 0) sk.le(static_cast<std::int32_t>(kid));
    }
    auto skeleton = b.add(0x10, b.str("TREE_HS_DEF"), sk);
    auto w = wld::WldFile::parse(b.build());
    REQUIRE(w.errors().empty());
    const auto& def = w.at(skeleton)->as<wld::Skeleton>();
    CHECK(def.nodes[1].name == "BR1");
    auto m = wld::build_skeleton_pose(w, def, "TREE_ACTORDEF");
    REQUIRE(m);
    CHECK(m->name == "TREE_ACTORDEF");
    REQUIRE(m->vertices.size() == 4);
    // (101, 200, 300) -> Rz90 -> (-200, 101, 300) -> + (1, 0, 0) + (0, 0, 10)
    CHECK(m->vertices[1].position.x == doctest::Approx(-199.0f).epsilon(1e-3));
    CHECK(m->vertices[1].position.y == doctest::Approx(101.0f).epsilon(1e-3));
    CHECK(m->vertices[1].position.z == doctest::Approx(310.0f).epsilon(1e-3));

    // A child listed twice has two parents: malformed.
    auto bad = def;
    bad.nodes[1].children = {1};
    CHECK_FALSE(wld::build_skeleton_pose(w, bad, "X"));
}

TEST_CASE("blend_mode maps render methods") {
    CHECK(wld::blend_mode(0) == BlendMode::Invisible);
    CHECK(wld::blend_mode(0x80000001) == BlendMode::Opaque);
    CHECK(wld::blend_mode(0x80000013) == BlendMode::Masked);
    CHECK(wld::blend_mode(0x80000009) == BlendMode::Transparent25);
    CHECK(wld::blend_mode(0x80000005) == BlendMode::Transparent50);
    CHECK(wld::blend_mode(0x8000000a) == BlendMode::Transparent75);
    CHECK(wld::blend_mode(0x80000017) == BlendMode::Additive);
    CHECK(wld::blend_mode(0x80000000) == BlendMode::Invisible);
    CHECK(wld::blend_mode(0x80000014) == BlendMode::Opaque);
    CHECK(wld::blend_mode(0x53) == BlendMode::Invisible);
    CHECK(wld::blend_mode(0x4b) == BlendMode::Invisible);
    CHECK(wld::blend_mode(0x553) == BlendMode::Opaque);
    // As the DLL's legacy table decodes them, not as LanternExtractor names
    // them: qrg's TREE20 leaves (0x80000007) cut out, POJWATER01 (0x18) is
    // blended, 0x1a is not drawn.
    CHECK(wld::blend_mode(0x80000007) == BlendMode::Masked);
    CHECK(wld::blend_mode(0x80000008) == BlendMode::Masked);
    CHECK(wld::blend_mode(0x80000006) == BlendMode::Transparent25);
    CHECK(wld::blend_mode(0x80000018) == BlendMode::Transparent50);
    CHECK(wld::blend_mode(0x80000028) == BlendMode::Transparent75);
    CHECK(wld::blend_mode(0x80000029) == BlendMode::Additive);
    CHECK(wld::blend_mode(0x8000001a) == BlendMode::Invisible);
    CHECK(wld::blend_mode(0x80000003) == BlendMode::Opaque);
    CHECK(wld::blend_mode(0x5d3) == BlendMode::Masked);
    CHECK(wld::blend_mode(0x573) == BlendMode::Opaque);
    CHECK(wld::blend_mode(0x803) == BlendMode::Opaque);
    CHECK(wld::blend_mode(0x1280557) == BlendMode::Transparent50);
    CHECK(wld::blend_mode(0x13b0557) == BlendMode::Additive);
}

TEST_CASE("placement_transform composes translation, Rz*Ry*Rx and scale") {
    wld::ActorInstance inst;
    inst.position = {10.0f, 20.0f, 30.0f};
    inst.rotation_zyx = {128.0f, 0.0f, 0.0f}; // a quarter turn around Z
    inst.scale = 2.0f;
    auto p = wld::placement_transform(inst) * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    CHECK(p.x == doctest::Approx(10.0f));
    CHECK(p.y == doctest::Approx(22.0f));
    CHECK(p.z == doctest::Approx(30.0f));
    // Up stays up under a pure heading.
    auto up = wld::placement_transform(inst) * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    CHECK(up.z == doctest::Approx(2.0f));

    // A positive Y angle tilts +Z towards -X (the Y angle is negated), and
    // the tilt is applied before the heading.
    inst.position = {};
    inst.scale = 1.0f;
    inst.rotation_zyx = {0.0f, 128.0f, 0.0f};
    auto t = wld::placement_transform(inst) * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    CHECK(t.x == doctest::Approx(-1.0f));
    CHECK(t.z == doctest::Approx(0.0f).epsilon(1e-6));
    inst.rotation_zyx = {128.0f, 128.0f, 0.0f};
    t = wld::placement_transform(inst) * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    CHECK(t.y == doctest::Approx(-1.0f));
}

namespace {

void check_zone(const Zone& z) {
    CHECK_FALSE(z.terrain.empty());
    auto check_mesh = [](const Mesh& m) {
        for (const auto& part : m.parts) {
            CHECK(part.material < m.materials.size());
            CHECK(part.first_index + part.index_count <= m.indices.size());
        }
        for (auto i : m.indices) REQUIRE(i < m.vertices.size());
    };
    for (const auto& m : z.terrain) check_mesh(m);
    for (const auto& [name, m] : z.objects) check_mesh(m);
    for (const auto& p : z.placements) {
        auto it = z.objects.find(p.actor);
        REQUIRE(it != z.objects.end());
        if (!p.vertex_colors.empty()) CHECK(p.vertex_colors.size() == it->second.vertices.size());
    }
    for (int i = 0; i < 3; ++i) {
        CHECK(std::isfinite(z.bounds_min[i]));
        CHECK(std::isfinite(z.bounds_max[i]));
        CHECK(z.bounds_min[i] < z.bounds_max[i]);
    }
}

} // namespace

TEST_CASE("load_classic_zone loads qeynos2" * doctest::skip(!hailstone::test::game_dir())) {
    auto z = load_classic_zone(*hailstone::test::game_dir(), "qeynos2");
    check_zone(z);
    CHECK(z.objects.size() > 10);
    CHECK(z.placements.size() > 100);
    CHECK_FALSE(z.lights.empty());
    CHECK(z.read_file("qeynos2.wld"));
}

TEST_CASE("load_classic_zone loads gfaydark with upright trees" * doctest::skip(!hailstone::test::game_dir())) {
    auto z = load_classic_zone(*hailstone::test::game_dir(), "gfaydark");
    check_zone(z);
    REQUIRE(z.placements.size() > 1000);
    // Every placement keeps object +Z pointing up: Greater Faydark's trees
    // only turn around Z, so a wrong axis order would lay them flat.
    std::size_t upright = 0;
    for (const auto& p : z.placements) {
        glm::vec3 up = glm::normalize(glm::vec3(p.transform * glm::vec4(0, 0, 1, 0)));
        upright += up.z > 0.99f;
    }
    CHECK(upright == z.placements.size());
}

TEST_CASE("load_classic_zone poses skeletal trees in citymist" * doctest::skip(!hailstone::test::game_dir())) {
    // Witness from OpenEQ docs/WLD_PLACED_OBJECTS.md: 50 placements of a
    // seven-mesh, eight-node tree with 106 vertices and 72 polygons.
    auto z = load_classic_zone(*hailstone::test::game_dir(), "citymist");
    check_zone(z);
    const auto& tree = z.objects.at("JNTREE103_ACTORDEF");
    CHECK(tree.vertices.size() == 106);
    CHECK(tree.indices.size() == 72 * 3);
    std::size_t placed = 0;
    for (const auto& p : z.placements) placed += p.actor == "JNTREE103_ACTORDEF";
    CHECK(placed == 50);
}

TEST_CASE("load_classic_zone draws objects with their own archive's textures" * doctest::skip(!hailstone::test::game_dir())) {
    // qeytoqrg.s3d and qeytoqrg_obj.s3d both hold treea.bmp: the zone's an opaque DXT1 on the willows' green, the
    // object archive's the DXT5 cut-out the willows draw. The willows read the object archive's.
    auto z = load_classic_zone(*hailstone::test::game_dir(), "qeytoqrg");
    std::size_t own = 0;
    for (const auto& [name, mesh] : z.objects)
        for (const auto& m : mesh.materials)
            for (const auto& f : m.frames) {
                CHECK(f != "TREEA.BMP");
                CHECK(f != "treea.bmp");
                if (f != "qeytoqrg_obj/treea.bmp") continue;
                ++own;
                auto data = z.read_file(f);
                REQUIRE(data);
                REQUIRE(data->size() > 0x58);
                CHECK(std::string(reinterpret_cast<const char*>(data->data()) + 0x54, 4) == "DXT5");
            }
    CHECK(own > 0);
    // A file only the object archive holds keeps its name.
    bool pine = false;
    for (const auto& [name, mesh] : z.objects)
        for (const auto& m : mesh.materials)
            for (const auto& f : m.frames) pine |= f == "GPINE.BMP" || f == "gpine.bmp";
    CHECK(pine);
}

TEST_CASE("load_classic_zone reads StartWorldDisplay's object archives" * doctest::skip(!hailstone::test::game_dir())) {
    // <zone>_2_obj.s3d: grobb's signs and cannonball (doors in the server's table) are only there.
    auto grobb = load_classic_zone(*hailstone::test::game_dir(), "grobb");
    CHECK(grobb.objects.contains("GRBSIGN404_ACTORDEF"));
    CHECK(grobb.objects.contains("HFCANBALL400_ACTORDEF"));
    // OBEQUIP_LEXIT in the Lost Dungeons of Norrath zones (expansion 6): the adventure exit portals.
    auto guka = load_classic_zone(*hailstone::test::game_dir(), "guka");
    CHECK(guka.objects.contains("GKPORTAL701E_ACTORDEF"));
    // Not in other zones.
    CHECK_FALSE(grobb.objects.contains("GKPORTAL701E_ACTORDEF"));
}

TEST_CASE("load_classic_zone reports a missing zone" * doctest::skip(!hailstone::test::game_dir())) {
    CHECK_THROWS(load_classic_zone(*hailstone::test::game_dir(), "no_such_zone"));
}

TEST_CASE("load_zone stops between its files when its cancel check says so") {
    // Cancelled before it starts: nothing is opened (an empty folder would otherwise say "not found").
    hailstone::test::ScratchDir dir("zone_cancel");
    hailstone::GameDir empty(dir.path());
    CHECK_THROWS_AS(load_zone(empty, "qeynos2", [] { return true; }), ZoneLoadCancelled);
    CHECK_THROWS_WITH(load_zone(empty, "qeynos2", [] { return false; }), doctest::Contains("not found"));
}

TEST_CASE("load_zone's cancel check is asked between the files of a real zone" * doctest::skip(!hailstone::test::game_dir())) {
    int asked = 0;
    auto z = load_zone(*hailstone::test::game_dir(), "qeynos2", [&] { return ++asked < 0; });
    CHECK(asked > 3);
    CHECK_FALSE(z.terrain.empty());
    // Cancelled after a few checks: the load ends there.
    for (int stop_at : {1, 2, 3}) {
        int n = 0;
        CHECK_THROWS_AS(load_zone(*hailstone::test::game_dir(), "qeynos2", [&] { return ++n >= stop_at; }), ZoneLoadCancelled);
        CHECK(n == stop_at);
    }
}
