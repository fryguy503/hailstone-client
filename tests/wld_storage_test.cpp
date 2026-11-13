#include <doctest/doctest.h>

#include <string_view>
#include <utility>
#include <variant>

#include "assets/wld.h"
#include "core/byte_io.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::assets;
using namespace hailstone::test::wld_fixtures;

TEST_CASE("WLD storage: rare payloads stay compact and fragment copies own independent mesh arrays") {
    CHECK(sizeof(wld::Payload) < sizeof(wld::MeshDef));
    CHECK(sizeof(wld::Fragment) < sizeof(wld::MeshDef));
    const auto original = wld::WldFile::parse(mesh_wld(false));
    const auto& source = original.fragments().back();
    REQUIRE(source.holds<wld::MeshDef>());
    const auto& source_mesh = source.as<wld::MeshDef>();
    REQUIRE_FALSE(source_mesh.positions.empty());

    auto copy = source;
    auto& changed = copy.as<wld::MeshDef>();
    CHECK(&changed != &source_mesh);
    CHECK(changed.positions.data() != source_mesh.positions.data());
    CHECK(changed.polygons.data() != source_mesh.polygons.data());
    changed.positions.front().x += 10.0f;
    changed.polygons.front().flags ^= wld::kPolygonPassable;
    CHECK(changed.positions.front().x != source_mesh.positions.front().x);
    CHECK(changed.polygons.front().flags != source_mesh.polygons.front().flags);

    wld::Fragment assigned;
    assigned = source;
    assigned.as<wld::MeshDef>().positions.clear();
    CHECK_FALSE(source_mesh.positions.empty());
    CHECK(assigned.as<wld::MeshDef>().positions.empty());
    const float changed_x = changed.positions.front().x;
    auto moved = std::move(copy);
    CHECK(moved.as<wld::MeshDef>().positions.front().x == changed_x);
    CHECK_FALSE(copy.holds<wld::MeshDef>());
    CHECK(copy.get<wld::MeshDef>() == nullptr);
    CHECK_THROWS_AS(copy.as<wld::MeshDef>(), std::bad_variant_access);
    // Copying a moved-from fragment is valid and does not invent a payload.
    assigned = copy;
    CHECK(assigned.get<wld::MeshDef>() == nullptr);
}

TEST_CASE("WLD storage: boxed cloud copies and replacement preserve value semantics") {
    wld::ParticleCloud cloud;
    cloud.capacity = 41;
    cloud.ranges[1].z = 7.0f;
    wld::Fragment source;
    source.data = cloud;
    const auto& view = source;
    REQUIRE(view.get<wld::ParticleCloud>());
    CHECK(view.as<wld::ParticleCloud>().capacity == 41);
    CHECK_FALSE(view.holds<wld::MeshDef>());
    CHECK_THROWS_AS(view.as<wld::MeshDef>(), std::bad_variant_access);

    auto copy = source;
    CHECK(copy.get<wld::ParticleCloud>() != view.get<wld::ParticleCloud>());
    copy.as<wld::ParticleCloud>().ranges[1].z = -2.0f;
    CHECK(view.as<wld::ParticleCloud>().ranges[1].z == 7.0f);
    auto* alias = &copy;
    copy = *alias;
    CHECK(copy.as<wld::ParticleCloud>().ranges[1].z == -2.0f);
    wld::Fragment moved;
    moved = std::move(copy);
    CHECK(moved.as<wld::ParticleCloud>().ranges[1].z == -2.0f);
    CHECK(copy.get<wld::ParticleCloud>() == nullptr);
    moved.data = wld::MeshRef{9, 0};
    CHECK_FALSE(moved.holds<wld::ParticleCloud>());
    CHECK(moved.as<wld::MeshRef>().mesh == 9);
    moved.data = std::monostate{};
    CHECK(moved.holds<std::monostate>());
}

TEST_CASE("WLD storage: sorted names keep case-sensitive first definitions and bounded string views") {
    WldBuilder builder;
    const auto zeta_name = builder.str("ZETA");
    const auto alpha_name = builder.str("ALPHA");
    const auto lower_name = builder.str("alpha");
    const auto broken_name = builder.str("BROKEN");
    ByteWriter body;
    body.le<std::int32_t>(17);
    body.le<std::uint32_t>(0);
    const auto zeta = builder.add(0x2d, zeta_name, body);
    const auto alpha = builder.add(0x2d, alpha_name, body);
    builder.add(0x2d, zeta_name, body);
    const auto lower = builder.add(0x2d, lower_name, body);
    builder.add(0x2d, 0, body);
    ByteWriter empty;
    const auto broken = builder.add(0x2d, broken_name, empty);
    builder.add(0x2d, broken_name, body);
    const auto file = wld::WldFile::parse(builder.build());
    REQUIRE(file.errors().size() == 1);
    CHECK(file.find("ZETA") == file.at(zeta));
    CHECK(file.resolve(zeta_name) == file.at(zeta));
    CHECK(file.find("ALPHA") == file.at(alpha));
    CHECK(file.find("alpha") == file.at(lower));
    CHECK(file.find("Alpha") == nullptr);
    CHECK(file.find("BROKEN") == file.at(broken));
    CHECK(file.find("BROKEN")->holds<std::monostate>());
    CHECK(file.find("") == nullptr);
    CHECK(file.find("A") == nullptr);
    CHECK(file.find("ZZZ") == nullptr);
    CHECK(file.find(std::string_view("ALPHA-trailing", 5)) == file.at(alpha));
}

TEST_CASE("WLD storage: copied and moved files resolve their own names after the original is destroyed") {
    auto source = wld::WldFile::parse(mesh_wld(true));
    auto copied = source;
    wld::WldFile assigned;
    assigned = source;
    const auto name = source.fragments().back().name;
    const auto* original = source.find(name);
    REQUIRE(original);
    REQUIRE(copied.find(name));
    REQUIRE(assigned.find(name));
    CHECK(copied.find(name) != original);
    CHECK(copied.find(name)->get<wld::MeshDef>() != original->get<wld::MeshDef>());
    CHECK(assigned.find(name)->get<wld::MeshDef>() != original->get<wld::MeshDef>());
    source = {};
    auto moved = std::move(copied);
    REQUIRE(moved.find(name));
    REQUIRE(assigned.find(name));
    CHECK(moved.find(name) == &moved.fragments().back());
    CHECK(assigned.find(name) == &assigned.fragments().back());
    CHECK(moved.find(name)->as<wld::MeshDef>().positions.size() == 4);
    CHECK(assigned.find(name)->as<wld::MeshDef>().positions.size() == 4);
    assigned = std::move(moved);
    CHECK(assigned.find(name) == &assigned.fragments().back());
    CHECK(assigned.find(name)->as<wld::MeshDef>().positions.size() == 4);
    CHECK(wld::WldFile{}.find(name) == nullptr);
}
