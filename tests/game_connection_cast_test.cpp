// CastSpell over the live adapter (net/game_connection_cast.cpp), against the mock EQEmu: the client's own state
// answers CastSpell's queries, and only a cast that passes them goes out as OP_CastSpell.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "game/collision.h"
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

TEST_CASE("Spell casts require sight only for spells that check the selected target") {
    const char* target_type = "5";
    const char* spell_type = "0";
    const char* effect = "0";
    bool requires_sight = true;
    SUBCASE("direct damage") {}
    SUBCASE("targeted area effect") { target_type = "8"; }
    SUBCASE("dispel") { spell_type = "1"; effect = "27"; }
    SUBCASE("heal") { spell_type = "1"; requires_sight = false; }
    SUBCASE("optional-target projectile") { target_type = "1"; requires_sight = false; }
    SUBCASE("self with a target selected") { target_type = "6"; spell_type = "1"; requires_sight = false; }
    SUBCASE("point-blank area with a target selected") { target_type = "4"; requires_sight = false; }

    constexpr std::int32_t kSpell = 6810;
    constexpr std::uint32_t kTarget = 101;
    MockConfig cfg = MockConfig::standard();
    cfg.characters[0].class_ = 2;
    Rig rig(cfg);
    std::ofstream(rig.dir.path() / "eqstr_us.txt", std::ios::app) << "108 You cannot see your target.\n";
    rig.game.rescan();
    auto& conn = rig.conn;
    conn.reload_game_files();
    REQUIRE(rig.to_zone());

    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_line({{0, std::to_string(kSpell)}, {1, "Sight Test"}, {9, "100"}, {13, "1000"},
                                     {20, "-10"}, {83, spell_type}, {86, effect}, {98, target_type}, {105, "1"},
                                     {122, "3"}, {213, "1"}, {214, "1"}})) == 1);
    conn.set_spell_table_for_test(table);

    auto& world = A::world(conn);
    const auto here = A::position(conn);
    world.spawns.at(kTarget).position = here + glm::vec3(6.0f, 0.0f, 0.0f);
    world.target_id = kTarget;
    auto& spells = A::spells(conn);
    spells.gems[0].spell = kSpell;
    spells.gems[0].ready = 0;
    spells.recovery_ready = 0;

    const auto wall = game::CollisionMesh::from_triangles({
        game::CollisionTriangle::make(here + glm::vec3(3.0f, -3.0f, -3.0f), here + glm::vec3(3.0f, 3.0f, -3.0f),
                                      here + glm::vec3(3.0f, 0.0f, 4.0f), 0),
    });
    bool obstructed = true;
    int sight_checks = 0;
    conn.set_line_of_sight([&](const glm::vec3& from, const glm::vec3& to) {
        ++sight_checks;
        return !obstructed || wall.line_of_sight(from, to);
    });
    const auto casts = [&] { return MockEqServer::count(rig.server.zone_log, Op::CastSpell); };

    rig.chat();
    conn.cast_gem(0);
    const auto messages = rig.chat();
    const auto refused = std::any_of(messages.begin(), messages.end(), [](const ChatEvent& e) {
        return e.text == "You cannot see your target.";
    });
    CHECK(refused == requires_sight);
    CHECK(sight_checks == (requires_sight ? 1 : 0));
    if (requires_sight) {
        CHECK_FALSE(spells.casting.casting());
        CHECK(spells.gems[0].ready == 0);
        CHECK(spells.recovery_ready == 0);
        rig.run_for(std::chrono::milliseconds(50));
        CHECK(casts() == 0);
        obstructed = false;
        conn.cast_gem(0);
    }
    CHECK(spells.casting.spell == kSpell);
    REQUIRE(rig.until([&] { return casts() == 1; }));
}

TEST_CASE("/cast by gem, by the start of a memorized spell's name, or =its whole name (MacroQuest's Cast)") {
    MockConfig cfg = MockConfig::standard();
    cfg.characters[0].class_ = 2;
    Rig rig(cfg);
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    // Two self-cast heals (no target, no sight), the first's name the start of the second's.
    auto table = std::make_shared<game::SpellTable>();
    auto heal = [](int id, const char* name) {
        // (122, 213, 214: the travel type and "can cast in / out of combat", as the cast tests above set them)
        return spell_line({{0, std::to_string(id)}, {1, name}, {9, "100"}, {13, "1000"}, {83, "1"}, {98, "6"}, {105, "1"},
                           {122, "3"}, {213, "1"}, {214, "1"}});
    };
    REQUIRE(table->parse(heal(6901, "Minor Healing") + "\n" + heal(6902, "Minor Healing Ward")) == 2);
    conn.set_spell_table_for_test(table);
    auto& spells = A::spells(conn);
    spells.gems[0].spell = 6901;
    spells.gems[2].spell = 6902;
    auto cast = [&](const std::string& line) { // the spell the line started casting, 0 none
        spells.casting = {};
        spells.pending = 0; // the last cast's request, which would hold the next one (cast_gem refuses while pending)
        for (auto& gem : spells.gems) gem.ready = 0;
        spells.recovery_ready = 0;
        rig.chat();
        conn.send_command(line);
        return spells.casting.casting() ? spells.casting.spell : 0u;
    };
    CHECK(cast("/cast 3") == 6902u);                           // RoF2's gem number
    CHECK(cast("/cast \"Minor\"") == 6901u);                   // the first gem whose spell starts with it
    CHECK(cast("/cast minor healing w") == 6901u);             // unquoted: only the first word, "minor", ...
    CHECK(cast("/cast \"minor healing w\"") == 6902u);         // ... quoted: the whole run, any case
    CHECK(cast("/cast \"=Minor Healing Ward\"") == 6902u);     // =: the whole name
    CHECK(cast("/cast \"=minor healing\"") == 6901u);
    CHECK(cast("/cast \"Healing\"") == 0u);                    // not the start of either name
    const auto said = rig.chat();
    CHECK(std::any_of(said.begin(), said.end(), [](const ChatEvent& e) {
        return e.text == "You do not seem to have that spell memorized.";
    }));
    CHECK(cast("/cast \"=Minor\"") == 0u);                     // =: no whole name is "Minor"
}

TEST_CASE("CastSpell over the live adapter: SPA 129 range focus from a worn item lets a cast past Range go out") {
    // A detrimental single-target nuke with Range 100 (cleric level 1), and a 5% range focus (SPA 129) on an
    // earring: GetFocusRangeModifier gives 100 x 5 x 0.01 = 5, so the reach is 105 (under the 5 x Range cap).
    std::uint32_t spell_id = 6810;
    SUBCASE("stock spell IDs") {}
    SUBCASE("spell IDs above 16 bits") { spell_id = 106810; }
    const auto focus_id = static_cast<std::int32_t>(spell_id + 1);
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_line({{0, std::to_string(spell_id)}, {1, "Range Bolt"}, {9, "100"}, {13, "1000"}, {20, "-10"},
                                     {83, "0"}, {86, "0"}, {98, "5"}, {105, "1"}, {213, "1"}, {214, "1"}}) +
                         "\n" + spell_line({{0, std::to_string(focus_id)}, {1, "Range Focus"}, {20, "5"}, {83, "1"}, {86, "129"}})) ==
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
        sp.gems[0].spell = spell_id;
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
    ear.effects[kEffectFocus].effect = focus_id;
    REQUIRE(A::inventory(conn).put(ItemSlot::possessions(1), ear));

    // 106 is still past 105 (the bonus is the focus's 5, not the cap's 400).
    cast_at(106.0f);
    CHECK_FALSE(A::spells(conn).casting.casting());
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(casts() == 0);

    // 101 is now in reach: the cast starts and OP_CastSpell goes out for the spell on the target.
    cast_at(101.0f);
    CHECK(A::spells(conn).casting.spell == spell_id);
    REQUIRE(rig.until([&] { return casts() == 1; }));
    const AppPacket* sent = nullptr;
    for (const auto& p : rig.server.zone_log)
        if (from_wire(p.opcode) == Op::CastSpell) sent = &p;
    REQUIRE(sent);
    auto packet = decode_cast_spell(*sent);
    REQUIRE(packet);
    CHECK(packet->spell_id == spell_id);
    CHECK(packet->spell_crc == table->spell_crc(spell_id));
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
    // As for a spawn record, the body type is the list's first entry.
    CHECK(world.spawns.at(set_target).body_type == 1);
    CHECK(MockEqServer::count(rig.server.zone_log, Op::CastSpell) == 1);
    cast_at(set_target);
    REQUIRE(sent_to(set_target));

    // An unknown set clears the list (0x8cfd90), so the undead-only spell is refused again.
    ByteWriter unknown;
    unknown.le(set_target);
    unknown.le(std::uint32_t{9999});
    rig.server.zone.send_reliable(make_packet(Op::SetBodyType, std::move(unknown)));
    REQUIRE(rig.until([&] { return world.spawns.at(set_target).properties.empty(); }));
    CHECK(world.spawns.at(set_target).body_type == 0);
    CHECK_FALSE(world.spawns.at(set_target).has_property(0));
    cast_at(set_target);
    CHECK_FALSE(A::spells(conn).casting.casting());
    CHECK(MockEqServer::count(rig.server.zone_log, Op::CastSpell) == 2);
}

TEST_CASE("SetBodyType without CharacterPropertySets.txt keeps the set ID as the property") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& world = A::world(rig.conn);
    std::uint32_t target = 0;
    for (const auto& [id, spawn] : world.spawns)
        if (id != world.player_id) { target = id; break; }
    REQUIRE(target != 0);
    ByteWriter update;
    update.le(target);
    update.le(std::uint32_t{9100});
    rig.server.zone.send_reliable(make_packet(Op::SetBodyType, std::move(update)));
    REQUIRE(rig.until([&] { return world.spawns.at(target).properties == std::vector<std::uint32_t>{9100}; }));
    CHECK(world.spawns.at(target).body_type == 9100);
    CHECK(world.spawns.at(target).has_property(9100));
}
