#include <doctest/doctest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "assets/pfs.h"
#include "assets/regions.h"
#include "assets/wld.h"
#include "assets/zone.h"
#include "core/byte_io.h"
#include "game/collision.h"
#include "game/coords.h"
#include "game/zone_lines.h"
#include "game_files.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {

// A unit box [0, 1]^3 as a BSP: six splits, each with the outside on the
// back and an empty (0) child, then a leaf for region 1 (the layout of
// OpenEQ's zone_lines.rs test fixture). A second leaf, region 2, is the
// front of a split at x = 5 hung off the first split's back side.
std::vector<wld::BspNode> unit_box_tree() {
    auto split = [](glm::vec3 n, float d, std::uint32_t front, std::uint32_t back) {
        wld::BspNode node;
        node.normal = n;
        node.distance = d;
        node.front = front;
        node.back = back;
        return node;
    };
    auto leaf = [](std::uint32_t region) {
        wld::BspNode node;
        node.region = region;
        return node;
    };
    return {
        split({1, 0, 0}, 0, 2, 8),   // 1: x > 0
        split({-1, 0, 0}, 1, 3, 0),  // 2: x < 1
        split({0, 1, 0}, 0, 4, 0),   // 3: y > 0
        split({0, -1, 0}, 1, 5, 0),  // 4: y < 1
        split({0, 0, 1}, 0, 6, 0),   // 5: z > 0
        split({0, 0, -1}, 1, 7, 0),  // 6: z < 1
        leaf(1),                     // 7
        split({-1, 0, 0}, -5, 0, 9), // 8: x <= 0 side; back of "-x - 5 > 0" means x > -5
        leaf(2),                     // 9
    };
}

wld::RegionTag tag(std::string text, std::vector<std::uint32_t> regions) {
    wld::RegionTag t;
    t.tag = std::move(text);
    t.regions = std::move(regions);
    return t;
}

} // namespace

TEST_CASE("BSP lookup walks a synthetic tree; split planes belong to no region") {
    auto r = ZoneRegions::build(unit_box_tree(), 3, {});
    CHECK(r.region_at({0.5f, 0.5f, 0.5f}) == 1);
    CHECK(r.region_at({1.5f, 0.5f, 0.5f}) == 0);
    CHECK(r.region_at({0.5f, -0.5f, 0.5f}) == 0);
    CHECK(r.region_at({0.5f, 0.5f, 2.0f}) == 0);
    CHECK(r.region_at({-2.0f, 0.5f, 0.5f}) == 2);
    CHECK(r.region_at({-6.0f, 0.5f, 0.5f}) == 0);
    CHECK(r.region_at({0.0f, 0.5f, 0.5f}) == 0); // exactly on the root plane
    CHECK(r.region_at({NAN, 0.5f, 0.5f}) == 0);
    CHECK(ZoneRegions().region_at({0, 0, 0}) == 0);

    // A leaf naming a region past the 0x22 count, and a cyclic child, give 0.
    auto bad = unit_box_tree();
    bad[6].region = 7;
    bad[7].back = 1; // node 8 -> node 1 -> node 8 ...
    auto b = ZoneRegions::build(bad, 3, {});
    CHECK(b.region_at({0.5f, 0.5f, 0.5f}) == 0);
    CHECK(b.region_at({-2.0f, 0.5f, 0.5f}) == 0);
}

TEST_CASE("region tags set the client's region flags (ParseRegionNameFlags)") {
    CHECK(region_flags_from_tag("WTN__01210000000000000000000000___000000000000") == 5);
    CHECK(region_flags_from_tag("WT_ZONE") == 5);
    CHECK(region_flags_from_tag("LAN__00000000000000000000000000___000000000000") == 7);
    CHECK(region_flags_from_tag("SLN__01943000000000000000000000___000000000000") == 6);
    CHECK(region_flags_from_tag("VWN__00000000000000000000000000___000000000000") == 8);
    CHECK(region_flags_from_tag("DRP__01945000000000000000000000___000000000000") == kRegionPvP);
    CHECK(region_flags_from_tag("DRP_ZONE") == kRegionPvP);
    CHECK(region_flags_from_tag("DRNTP00255000033000000000000000___000000000000") == kRegionTeleport);
    CHECK(region_flags_from_tag("WTNTP00255000003000000000000000___000000000000") == (kRegionTeleport | 5));
    CHECK(region_flags_from_tag("LANTP00255000009000000000000000___000000000000") == (kRegionTeleport | 7));
    // Long names: [0x1f] 'M', [0x20] 'S' (ice), [0x21] 'P' or 'F'.
    CHECK(region_flags_from_tag("DRN__00129000000000000000000000_S_000000000000") == kRegionSlippery);
    CHECK(region_flags_from_tag("DRN__00129000000000000000000000MSP000000000000") ==
          (kRegionFlagM | kRegionSlippery | kRegionFlagP));
    CHECK(region_flags_from_tag("DRN__00129000000000000000000000_N_000000000000") == 0);
    // DR clears the type; EQG names start with 'A'; short names change nothing.
    CHECK(region_flags_from_tag("DR_X", 5 | kRegionPvP) == kRegionPvP);
    CHECK(region_flags_from_tag("AWT_1") == 5);
    CHECK(region_flags_from_tag("ATP_12") == kRegionTeleport);
    CHECK(region_flags_from_tag("WT", 7) == 7);
}

TEST_CASE("zone-line tags decode as Teleport_ResolveTeleportIndex does") {
    auto ref = decode_zone_line("DRNTP00255000033000000000000000___000000000000");
    REQUIRE(ref);
    CHECK(ref->reference);
    CHECK(ref->zone_point == 33);
    auto short_ref = decode_zone_line("DRNTP00255000004_ZONE");
    REQUIRE(short_ref);
    CHECK(short_ref->zone_point == 4);

    // qeynos → qeynos2: zone 2, server y -30, x 357, z and heading kept.
    auto abs = decode_zone_line("DRNTP00002-00030000357999999999___000000000000");
    REQUIRE(abs);
    CHECK_FALSE(abs->reference);
    CHECK(abs->zone_id == 2);
    CHECK(abs->server_y == -30.0f);
    CHECK(abs->server_x == 357.0f);
    CHECK(abs->server_z == 999999.0f);
    CHECK(abs->heading == 999.0f);
    // Fractions (beholder) and a truncated name (airplane: zone 0, ignored by the client).
    auto frac = decode_zone_line("DRNTP000153626.9-02018003.35999_ZONE");
    REQUIRE(frac);
    CHECK(frac->zone_id == 15);
    CHECK(frac->server_y == doctest::Approx(3626.9f));
    CHECK(frac->server_x == -2018.0f);
    CHECK(frac->server_z == doctest::Approx(3.35f));
    auto bare = decode_zone_line("DRNTP_ZONE");
    REQUIRE(bare);
    CHECK(bare->zone_id == 0);
    auto eqg = decode_zone_line("ATP_17");
    REQUIRE(eqg);
    CHECK(eqg->reference);
    CHECK(eqg->zone_point == 170); // "DRNTP00255%05d0..." of 17: the zone point field reads 000170
    CHECK_FALSE(decode_zone_line("WTN__01210000000000000000000000___000000000000"));
}

TEST_CASE("zone-line tags: numbers past the fields' ranges are clamped, not cast out of range") {
    auto huge = decode_zone_line("ATP_99999999999999999999");
    REQUIRE(huge);
    CHECK(huge->zone_point == 2147483640); // the largest int / 10, times 10
    auto negative = decode_zone_line("ATP_-99999999999");
    REQUIRE(negative);
    CHECK(negative->zone_point == -2147483640);
    auto below = decode_zone_line("DRNTP-0002000030000357999999999___000000000000");
    REQUIRE(below);
    CHECK(below->zone_id == 0); // a negative zone: none
}

TEST_CASE("ZoneRegions combines BSP leaves and tags") {
    auto r = ZoneRegions::build(unit_box_tree(), 3,
                                {tag("WTN__0", {0}), tag("DRNTP00255000004_ZONE", {0}), tag("LA_ZONE", {1, 9})});
    auto in = r.lookup({0.5f, 0.5f, 0.5f});
    CHECK(in.region == 1);
    CHECK_FALSE(in.is_water()); // tags apply in order and "DR" clears the type
    CHECK(in.flags == kRegionTeleport);
    CHECK(in.is_zone_line());
    REQUIRE(in.zone_line);
    CHECK(in.zone_line->zone_point == 4);
    auto lava = r.lookup({-1.0f, 0.5f, 0.5f});
    CHECK(lava.region == 2);
    CHECK(lava.is_lava());
    CHECK_FALSE(lava.is_zone_line());
    CHECK(r.lookup({3, 3, 3}).region == 0);
    CHECK(r.tagged().size() == 2);
}

TEST_CASE("0x21, 0x22 and 0x29 fragments parse") {
    ByteWriter w;
    std::vector<std::uint8_t> strings{0};
    auto add_string = [&](const std::string& s) {
        auto off = static_cast<std::int32_t>(strings.size());
        strings.insert(strings.end(), s.begin(), s.end());
        strings.push_back(0);
        return -off;
    };
    std::int32_t name_tag = add_string("WT_ZONE");
    std::int32_t name_other = add_string("DRNTP_ZONE");
    std::vector<std::pair<std::uint32_t, ByteWriter>> frags;
    {
        ByteWriter f;
        f.le<std::int32_t>(0);
        auto tree = unit_box_tree();
        f.le(static_cast<std::uint32_t>(tree.size()));
        for (const auto& n : tree) {
            f.f32(n.normal.x);
            f.f32(n.normal.y);
            f.f32(n.normal.z);
            f.f32(n.distance);
            f.le(n.region);
            f.le(n.front);
            f.le(n.back);
        }
        frags.emplace_back(0x21, std::move(f));
    }
    for (int i = 0; i < 2; ++i) {
        ByteWriter f;
        f.le<std::int32_t>(0);
        f.le<std::uint32_t>(0x181);
        f.zeros(8);
        frags.emplace_back(0x22, std::move(f));
    }
    {
        // Tag in the name.
        ByteWriter f;
        f.le(name_tag);
        f.le<std::uint32_t>(0);
        f.le<std::uint32_t>(1);
        f.le<std::uint32_t>(1); // region 2
        f.le<std::uint32_t>(0);
        frags.emplace_back(0x29, std::move(f));
    }
    {
        // Tag in the payload, XOR-encoded; the payload wins over the name.
        std::string text = "DRNTP00255000004_ZONE";
        ByteWriter f;
        f.le(name_other);
        f.le<std::uint32_t>(0);
        f.le<std::uint32_t>(1);
        f.le<std::uint32_t>(0); // region 1
        f.le(static_cast<std::uint32_t>(text.size() + 1));
        for (std::size_t i = 0; i <= text.size(); ++i)
            f.le(static_cast<std::uint8_t>((i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0) ^
                                           wld::kStringKey[i % 8]));
        frags.emplace_back(0x29, std::move(f));
    }
    w.le(wld::kMagic);
    w.le(wld::kVersionOld);
    w.le(static_cast<std::uint32_t>(frags.size()));
    w.le<std::uint32_t>(2);
    w.le<std::uint32_t>(0);
    w.le(static_cast<std::uint32_t>(strings.size()));
    w.le<std::uint32_t>(0);
    for (std::size_t i = 0; i < strings.size(); ++i) w.le(static_cast<std::uint8_t>(strings[i] ^ wld::kStringKey[i % 8]));
    for (const auto& [type, f] : frags) {
        w.le(static_cast<std::uint32_t>(f.size()));
        w.le(type);
        w.bytes(f.data());
    }
    auto file = wld::WldFile::parse(w.data());
    CHECK(file.errors().empty());
    CHECK(file.unknown_types().empty());
    const auto* tree = file.fragments()[0].get<wld::BspTree>();
    REQUIRE(tree);
    CHECK(tree->nodes.size() == 9);
    CHECK(tree->nodes[0].front == 2);
    CHECK(tree->nodes[6].region == 1);
    const auto* t2 = file.fragments()[4].get<wld::RegionTag>();
    REQUIRE(t2);
    CHECK(t2->tag == "DRNTP00255000004_ZONE");

    auto r = ZoneRegions::from_wld(file);
    CHECK(r.region_count() == 2);
    auto in = r.lookup({0.5f, 0.5f, 0.5f});
    CHECK(in.is_zone_line());
    CHECK(in.zone_line->zone_point == 4);
    CHECK(r.lookup({-1.0f, 0.5f, 0.5f}).is_water());
}

TEST_CASE("zone lines: the teleport text; the trigger fires on entry") {
    CHECK(game::region_teleport_text("ATP_17") == "DRNTP0025500017000000000000000");
    CHECK(game::region_teleport_text("DRNTP00255000004_ZONE") == "DRNTP00255000004_ZONE");
    auto regions = ZoneRegions::build(unit_box_tree(), 2, {tag("DRNTP00255000004_ZONE", {0})});
    game::ZoneLineTrigger trigger;
    CHECK_FALSE(trigger.update({-1, 0.5f, 0.2f}, regions));
    auto fired = trigger.update({0.5f, 0.5f, 0.2f}, regions);
    REQUIRE(fired);
    CHECK(*fired == "DRNTP00255000004_ZONE");
    // Staying inside does not fire again; leaving and re-entering does.
    CHECK_FALSE(trigger.update({0.6f, 0.5f, 0.2f}, regions));
    CHECK_FALSE(trigger.update({2, 0.5f, 0.2f}, regions));
    CHECK(trigger.update({0.5f, 0.5f, 0.2f}, regions));
}

namespace {
// A flat floor at z over -500..500 (two triangles).
std::vector<game::CollisionTriangle> flat_floor(float z) {
    return {game::CollisionTriangle::make({500, -500, z}, {-500, -500, z}, {-500, 500, z}, 0),
            game::CollisionTriangle::make({-500, 500, z}, {500, 500, z}, {500, -500, z}, 0)};
}
} // namespace

TEST_CASE("buried zone lines: a box under the ground fires from the ground over it") {
    // toxxulia's line to barren: the box (z -127..-97) some 230 units under the ground.
    auto regions = ZoneRegions::from_boxes({{"AWT_pond", {200, 200, -20}, {0, 0, 0}, {50, 50, 30}},
                                            {"ATP_01_barren", {0, 0, -300}, {0, 0, 0}, {20, 30, 15}}});
    auto ground = game::CollisionMesh::from_triangles(flat_floor(0));
    auto buried = game::find_buried_zone_lines(regions, ground);
    REQUIRE(buried.size() == 1);
    CHECK(buried[0].region == 2);
    CHECK(buried[0].tag == "ATP_01_barren");
    CHECK(buried[0].box_lo_z == doctest::Approx(-315));
    CHECK(buried[0].box_hi_z == doctest::Approx(-285));
    CHECK(buried[0].ground_hi == doctest::Approx(0));
    CHECK(buried[0].top_z == doctest::Approx(game::kBuriedHeadroom));
    CHECK(regions.box_contains(2, {0, 0, -300}));
    CHECK_FALSE(regions.box_contains(2, {0, 0, 0}));

    game::ZoneLineTrigger trigger;
    trigger.set_buried(buried);
    CHECK_FALSE(trigger.update({100, 0, 0}, regions)); // beside the footprint
    auto fired = trigger.update({10, -25, 0}, regions); // on the ground over it
    REQUIRE(fired);
    CHECK(*fired == "DRNTP0025500001000000000000000");
    CHECK_FALSE(trigger.update({10, -20, 0}, regions)); // staying on it
    CHECK_FALSE(trigger.update({30, 0, 0}, regions));   // off it again (x half-size 20)
    CHECK(trigger.update({0, 0, 20}, regions));         // jumping onto it
    CHECK_FALSE(trigger.update({0, 0, 200}, regions));  // far over the ground: out of it
    CHECK(trigger.update({0, 0, -300}, regions));       // the box itself still counts
    trigger.reset();
    CHECK(trigger.buried().size() == 1); // a zone-in keeps them
    // Zoning in over the footprint (mistythicket's arrival from runnyeye): held until the feet leave it.
    trigger.reset();
    CHECK_FALSE(trigger.update({0, 0, 0}, regions));
    CHECK_FALSE(trigger.update({5, 5, 0}, regions));
    CHECK_FALSE(trigger.update({60, 0, 0}, regions));
    CHECK(trigger.update({0, 0, 0}, regions));
    // Zoning in beside it: armed at once.
    trigger.reset();
    CHECK_FALSE(trigger.update({60, 0, 0}, regions));
    CHECK(trigger.update({0, 0, 0}, regions));
    // Without the fallback the ground over it is no zone line (the original's behaviour).
    game::ZoneLineTrigger plain;
    CHECK_FALSE(plain.update({0, 0, 0}, regions));
}

TEST_CASE("buried zone lines: a box with a real floor at its depth, or a reachable one, is left alone") {
    auto regions = ZoneRegions::from_boxes({{"ATP_02_cave", {0, 0, -300}, {0, 0, 0}, {20, 20, 15}}});
    // A cave under the surface whose floor (-310) is in the box: walked into as it is.
    auto ground = flat_floor(0);
    for (const auto& t : flat_floor(-310)) ground.push_back(t);
    auto cave = game::CollisionMesh::from_triangles(ground);
    CHECK(game::find_buried_zone_lines(regions, cave).empty());
    // A floor just under the box bottom (-315 - 30) still reaches it.
    ground = flat_floor(0);
    for (const auto& t : flat_floor(-345)) ground.push_back(t);
    CHECK(game::find_buried_zone_lines(regions, game::CollisionMesh::from_triangles(ground)).empty());
    // Ground only a little over the box's top: not clearly buried.
    CHECK(game::find_buried_zone_lines(regions, game::CollisionMesh::from_triangles(flat_floor(-250))).empty());
    // No ground at all over it.
    CHECK(game::find_buried_zone_lines(regions, game::CollisionMesh::from_triangles(flat_floor(-1000))).empty());
    // A fall into the void: ground over only part of the box (thevoid's platforms, a pit's lip).
    auto lip = game::CollisionMesh::from_triangles({flat_floor(0)[0]}); // the half x + y < 0
    auto survey = game::survey_zone_lines(regions, lip);
    REQUIRE(survey.size() == 1);
    CHECK(survey[0].covered < survey[0].samples);
    CHECK_FALSE(survey[0].buried);
    // Under a roof (a down-facing surface over the box, the ground further up): a room, not the inside of the ground.
    ground = flat_floor(0);
    ground.push_back(game::CollisionTriangle::make({-500, 500, -200}, {-500, -500, -200}, {500, -500, -200}, 0));
    ground.push_back(game::CollisionTriangle::make({500, -500, -200}, {500, 500, -200}, {-500, 500, -200}, 0));
    survey = game::survey_zone_lines(regions, game::CollisionMesh::from_triangles(ground));
    REQUIRE(survey.size() == 1);
    CHECK(survey[0].inside == 0);
    CHECK_FALSE(survey[0].buried);

    // An ordinary box on the ground: not buried, and the trigger works as before.
    auto normal = ZoneRegions::from_boxes({{"ATP_03_erudin", {0, 0, 10}, {0, 0, 0}, {20, 20, 20}}});
    auto flat = game::CollisionMesh::from_triangles(flat_floor(0));
    auto buried = game::find_buried_zone_lines(normal, flat);
    CHECK(buried.empty());
    game::ZoneLineTrigger trigger;
    trigger.set_buried(buried);
    CHECK_FALSE(trigger.update({50, 0, 0}, normal));
    auto fired = trigger.update({0, 0, 0}, normal);
    REQUIRE(fired);
    CHECK(*fired == "DRNTP0025500003000000000000000");
    // Water and other boxes are never lifted, nor is a BSP zone's.
    auto water = ZoneRegions::from_boxes({{"AWT_deep", {0, 0, -300}, {0, 0, 0}, {20, 20, 15}}});
    CHECK(game::find_buried_zone_lines(water, flat).empty());
}

// zone_points rows on the EQEmu PEQ database (live NMS server, 2026-10-20):
//   qeynos #1: source server (x -442, y 464) → qeynos2 (2) at (-5, -151)
//   qeynos #2: source (-80, 586) → qeynos2 at (356, -26)
//   qeynos #3, #4, #33: → qcat (45)
TEST_CASE("qeynos zone lines match the server's zone_points table" * doctest::skip(!hailstone::test::game_dir())) {
    auto archive = PfsArchive::open(test::game_dir()->root() / "qeynos.s3d");
    auto data = archive.read("qeynos.wld");
    REQUIRE(data);
    auto regions = ZoneRegions::from_wld(wld::WldFile::parse(*data, "qeynos.wld"));
    REQUIRE_FALSE(regions.empty());
    std::set<std::int32_t> references;
    int to_qeynos2 = 0;
    for (const auto& r : regions.tagged()) {
        if (!r.is_zone_line()) continue;
        if (r.zone_line->reference) {
            references.insert(r.zone_line->zone_point);
        } else {
            CHECK(r.zone_line->zone_id == 2);
            ++to_qeynos2;
            // Targets match zone_points #1 (-5, -151) and #2 (356, -26) within a few units.
            bool near1 = std::abs(r.zone_line->server_x - -5.0f) < 5 && std::abs(r.zone_line->server_y - -151.0f) < 5;
            bool near2 = std::abs(r.zone_line->server_x - 356.0f) < 5 && std::abs(r.zone_line->server_y - -26.0f) < 5;
            CHECK((near1 || near2));
        }
    }
    CHECK(to_qeynos2 == 3); // DRNTP00002-00030... tags 2 regions, DRNTP00002-00155... 1
    CHECK(references == std::set<std::int32_t>{3, 4, 33});

    // The zone_points source positions of #1 and #2 lie in or next to the
    // zone-line regions (server x/y swapped into world coordinates).
    for (auto [sx, sy] : {std::pair{-442.0f, 464.0f}, std::pair{-80.0f, 586.0f}}) {
        bool found = false;
        for (float dz = -10; dz <= 40 && !found; dz += 1)
            for (float dx = -15; dx <= 15 && !found; dx += 1)
                for (float dy = -15; dy <= 15 && !found; dy += 1) {
                    auto info = regions.lookup(game::server_to_world({sx + dx, sy + dy, 1.5f + dz}));
                    found = info.is_zone_line() && !info.zone_line->reference && info.zone_line->zone_id == 2;
                }
        CHECK(found);
    }
}
