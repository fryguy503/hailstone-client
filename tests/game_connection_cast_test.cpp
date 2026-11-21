// CastSpell over the live adapter (net/game_connection_cast.cpp), against the mock EQEmu: the client's own state
// answers CastSpell's queries, and only a cast that passes them goes out as OP_CastSpell.

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "game/spells.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/item_packets.h"
#include "net/packet_io.h"
#include "net/spell_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

using A = GameConnectionTestAccess;

// A spells_us.txt line with every column "0" except the given ones (no effects in slots 1..12: 254).
std::string spell_line(std::initializer_list<std::pair<int, std::string>> set) {
    std::vector<std::string> cols(237, "0");
    for (int i = 1; i <= 8; ++i) cols[static_cast<std::size_t>(i)].clear();
    for (int i = 86; i <= 97; ++i) cols[static_cast<std::size_t>(i)] = "254";
    for (const auto& [c, v] : set) cols[static_cast<std::size_t>(c)] = v;
    std::string line;
    for (std::size_t i = 0; i < cols.size(); ++i) line += (i ? "^" : "") + cols[i];
    return line;
}

} // namespace

TEST_CASE("CastSpell over the live adapter: SPA 129 range focus from a worn item lets a cast past Range go out") {
    // A detrimental single-target nuke with Range 100 (cleric level 1), and a 5% range focus (SPA 129) on an
    // earring: GetFocusRangeModifier gives 100 x 5 x 0.01 = 5, so the reach is 105 (under the 5 x Range cap).
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_line({{0, "6810"}, {1, "Range Bolt"}, {9, "100"}, {13, "1000"}, {20, "-10"},
                                     {83, "0"}, {86, "0"}, {98, "5"}, {105, "1"}, {213, "1"}, {214, "1"}}) +
                         "\n" + spell_line({{0, "6811"}, {1, "Range Focus"}, {20, "5"}, {83, "1"}, {86, "129"}})) ==
            2);
    MockConfig cfg = MockConfig::standard();
    cfg.characters[0].class_ = 2; // a cleric: pure melee classes get no focus
    Rig rig(cfg);
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    conn.set_spell_table_for_test(table);
    REQUIRE(conn.profile()->class_ == 2);

    auto& world = A::world(conn);
    const std::uint32_t me = world.player_id;
    constexpr std::uint32_t kTarget = 101; // a_rat001
    REQUIRE(world.spawns.count(kTarget) == 1);
    REQUIRE(world.spawns.at(kTarget).is_npc);

    const auto casts = [&] { return MockEqServer::count(rig.server.zone_log, Op::CastSpell); };
    // Places the target `distance` units from the player along one axis and clicks gem 1 (CastSpell runs at once).
    auto cast_at = [&](float distance) {
        auto& sp = A::spells(conn);
        sp.casting = {};
        sp.pending = 0;
        sp.gems[0].spell = 6810;
        sp.gems[0].ready = 0;
        sp.recovery_ready = 0;
        const glm::vec3 here = A::position(conn);
        if (world.spawns.count(me)) world.spawns.at(me).position = here;
        world.spawns.at(kTarget).position = here + glm::vec3(distance, 0.0f, 0.0f);
        world.target_id = kTarget;
        rig.chat();
        conn.cast_gem(0);
    };

    rig.server.zone_log.clear();
    // No focus: 101 is past Range 100, refused locally.
    cast_at(101.0f);
    CHECK_FALSE(A::spells(conn).casting.casting());
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(casts() == 0);

    // The earring in ear slot 1: the reach is 105.
    Item ear = make_item(2001, "Range Earring", 1);
    ear.effects[kEffectFocus].effect = 6811;
    REQUIRE(A::inventory(conn).put(ItemSlot::possessions(1), ear));

    // 106 is still past 105 (the bonus is the focus's 5, not the cap's 400).
    cast_at(106.0f);
    CHECK_FALSE(A::spells(conn).casting.casting());
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(casts() == 0);

    // 101 is now in reach: the cast starts and OP_CastSpell goes out for the spell on the target.
    cast_at(101.0f);
    CHECK(A::spells(conn).casting.spell == 6810);
    REQUIRE(rig.until([&] { return casts() == 1; }));
    const AppPacket* sent = nullptr;
    for (const auto& p : rig.server.zone_log)
        if (from_wire(p.opcode) == Op::CastSpell) sent = &p;
    REQUIRE(sent);
    auto packet = decode_cast_spell(*sent);
    REQUIRE(packet);
    CHECK(packet->spell_id == 6810);
    CHECK(packet->target_id == kTarget);
}

TEST_CASE("CastSpell checks direct spawn properties and expands explicit body type sets") {
    constexpr std::uint32_t kSet = 9100;
    constexpr std::int32_t kSpell = 6810;
    const std::vector<std::uint32_t> undead_properties{1, 3};
    MockConfig cfg = MockConfig::standard();
    cfg.characters[0].class_ = 2;
    auto& spawns = cfg.zones.at(cfg.characters[0].zone_id).spawns;
    const std::uint32_t set_target = spawns[0].spawn_id;
    const std::uint32_t direct_target = spawns[1].spawn_id;
    spawns[0].properties = {kSet};
    spawns[1].properties = undead_properties;

    Rig rig(std::move(cfg));
    auto& conn = rig.conn;
    const auto resources = rig.dir.path() / "Resources";
    std::filesystem::create_directories(resources);
    {
        std::ofstream file(resources / "characterpropertysets.txt");
        file << "9100^1\n9100^3\n9100^1\n9100^8junk\n3^25\n";
        REQUIRE(file.good());
    }
    rig.game.rescan();
    conn.reload_game_files();
    REQUIRE(rig.to_zone());

    auto spells = std::make_shared<game::SpellTable>();
    REQUIRE(spells->parse(spell_line({{0, std::to_string(kSpell)}, {1, "Undead Bolt"}, {9, "100"}, {13, "1000"},
                                      {20, "-10"}, {83, "0"}, {86, "0"}, {98, "10"}, {105, "1"}, {213, "1"},
                                      {214, "1"}})) == 1);
    conn.set_spell_table_for_test(spells);

    auto& world = A::world(conn);
    REQUIRE(world.spawns.contains(set_target));
    REQUIRE(world.spawns.contains(direct_target));
    CHECK(world.spawns.at(set_target).body_type == kSet);
    CHECK(world.spawns.at(set_target).properties == std::vector<std::uint32_t>{kSet});
    CHECK(world.spawns.at(direct_target).body_type == 1);
    CHECK(world.spawns.at(direct_target).properties == undead_properties);

    const auto cast_at = [&](std::uint32_t target_id) {
        auto& state = A::spells(conn);
        state.casting = {};
        state.pending = 0;
        state.gems[0].spell = kSpell;
        state.gems[0].ready = 0;
        state.recovery_ready = 0;
        const auto here = A::position(conn);
        world.spawns.at(world.player_id).position = here;
        world.spawns.at(target_id).position = here + glm::vec3(5.0f, 0.0f, 0.0f);
        world.target_id = target_id;
        conn.cast_gem(0);
    };
    const auto sent_to = [&](std::uint32_t target_id) {
        return rig.until([&] {
            for (const auto& p : rig.server.zone_log) {
                if (!is_op(p, Op::CastSpell)) continue;
                if (auto cast = decode_cast_spell(p); cast && cast->spell_id == kSpell && cast->target_id == target_id)
                    return true;
            }
            return false;
        });
    };

    cast_at(set_target);
    CHECK_FALSE(A::spells(conn).casting.casting());

    cast_at(direct_target);
    REQUIRE(sent_to(direct_target));

    ByteWriter update;
    update.le(set_target);
    update.le(kSet);
    rig.server.zone.send_reliable(make_packet(Op::SetBodyType, std::move(update)));
    REQUIRE(rig.until([&] { return world.spawns.at(set_target).properties == undead_properties; }));
    CHECK(world.spawns.at(set_target).body_type == kSet);
    CHECK(MockEqServer::count(rig.server.zone_log, Op::CastSpell) == 1);
    cast_at(set_target);
    REQUIRE(sent_to(set_target));
}
