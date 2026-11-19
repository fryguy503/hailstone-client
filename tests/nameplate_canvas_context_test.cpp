#include <doctest/doctest.h>

#include "app/nameplate_canvas_context.h"
#include "mq_test_world.h"
#include "net/zone_packets.h"

using namespace hailstone;

TEST_CASE("nameplate canvas context: unknown resources remain absent and a server zero remains known") {
    mq_test::World world("canvas_context_resources");
    auto& connection = *world.conn;
    game::NameplateCanvasCandidate candidate;
    app::fill_nameplate_canvas_candidate(candidate, connection, connection.world().spawns.at(10), "caption", 50);
    CHECK(candidate.spawn.id == 10);
    CHECK(candidate.context.name == "caption");
    CHECK(candidate.context.distance == 50);
    CHECK(candidate.context.hp_percent.has_value());
    CHECK_FALSE(candidate.context.mana_percent);
    CHECK_FALSE(candidate.context.endurance_percent);
    CHECK_FALSE(candidate.context.casting);
    auto& group = net::GameConnectionTestAccess::group(connection);
    group.mana_percent[12] = 37;
    group.endurance_percent[12] = 0;
    app::fill_nameplate_canvas_candidate(candidate, connection, connection.world().spawns.at(12), "member", 20);
    REQUIRE(candidate.context.mana_percent);
    REQUIRE(candidate.context.endurance_percent);
    CHECK(*candidate.context.mana_percent == 37);
    CHECK(*candidate.context.endurance_percent == 0);
    CHECK(candidate.context.in_group);
    app::fill_nameplate_canvas_candidate(candidate, connection, connection.world().spawns.at(10), "caption", 50);
    CHECK_FALSE(candidate.context.mana_percent);
    CHECK_FALSE(candidate.context.endurance_percent);
}

TEST_CASE("nameplate canvas context: own HP includes items and cast observations have bounded lifetimes") {
    mq_test::World world("canvas_context_casts");
    auto& connection = *world.conn;
    connection.set_own_hp_items(250, 250);
    net::GameConnectionTestAccess::status(connection).mana = 0;
    auto& casts = net::GameConnectionTestAccess::spells(connection);
    casts.casting.spell = 200;
    casts.casting.cast_time = 60000;
    casts.casting.eta_ms = net::GameConnection::now_ms() + 60000;
    game::NameplateCanvasCandidate candidate;
    app::fill_nameplate_canvas_candidate(candidate, connection, connection.world().spawns.at(1), "self", 0);
    REQUIRE(candidate.context.hp_percent);
    REQUIRE(candidate.context.mana_percent);
    CHECK(*candidate.context.hp_percent == 80);
    CHECK(*candidate.context.mana_percent == 0);
    CHECK(candidate.context.self);
    REQUIRE(candidate.context.casting);
    CHECK(candidate.context.casting->spell_id == 200);
    CHECK(candidate.context.casting->name == "Minor Healing");
    CHECK(candidate.context.casting->duration_ms == 60000);
    CHECK(candidate.context.casting->elapsed_ms >= 0);
    casts.others[12] = {200, net::GameConnection::now_ms() - 120000, 1000};
    app::fill_nameplate_canvas_candidate(candidate, connection, connection.world().spawns.at(12), "member", 20);
    CHECK_FALSE(candidate.context.casting);
    auto& aggro = net::GameConnectionTestAccess::aggro(connection);
    aggro.target_id = 12; aggro.pct[net::kAggroPlayer] = 0;
    app::fill_nameplate_canvas_candidate(candidate, connection, connection.world().spawns.at(12), "member", 20);
    CHECK_FALSE(candidate.context.aggro_percent);
    aggro.received[net::kAggroPlayer] = true;
    app::fill_nameplate_canvas_candidate(candidate, connection, connection.world().spawns.at(12), "member", 20);
    REQUIRE(candidate.context.aggro_percent);
    CHECK(*candidate.context.aggro_percent == 0);
}

TEST_CASE("nameplate canvas context: typed concealment requires current observed spell effects") {
    mq_test::World world("canvas_context_concealment");
    auto& connection = *world.conn;
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(mq_test::spell_line({{0, "1000"}, {1, "Test regular invis"}, {86, "12"}}) + "\n" +
                         mq_test::spell_line({{0, "1001"}, {1, "Test improved undead invis"}, {87, "315"}})) == 2);
    connection.set_spell_table_for_test(table);
    auto& state = net::GameConnectionTestAccess::world(connection);
    auto& spells = net::GameConnectionTestAccess::spells(connection);
    game::NameplateCanvasCandidate candidate;
    const auto context = [&](std::uint32_t id) -> const game::CanvasContext& {
        app::fill_nameplate_canvas_candidate(candidate, connection, state.spawns.at(id), "caption", 50);
        return candidate.context;
    };
    auto& target = state.spawns.at(10);
    CHECK_FALSE(context(10).concealed);
    target.invis_rank = 1;
    CHECK(context(10).concealed);
    CHECK_FALSE(candidate.context.invisible);
    CHECK_FALSE(candidate.context.invisible_undead);

    game::BuffSlot regular, undead;
    regular.spell = 1000;
    regular.tics = 10;
    regular.received_ms = net::GameConnection::now_ms();
    undead = regular;
    undead.spell = 1001;
    spells.target_buffs_entity = 11;
    spells.target_buffs = {{0, regular}, {1, undead}};
    CHECK_FALSE(context(10).invisible);
    CHECK_FALSE(context(11).concealed);
    spells.target_buffs_entity = 10;
    CHECK(context(10).invisible);
    CHECK(candidate.context.invisible_undead);
    CHECK(candidate.context.concealed);

    spells.target_buffs[0].second.received_ms -= 120000;
    target.invis_rank = 0;
    CHECK(context(10).invisible_undead);
    CHECK_FALSE(candidate.context.invisible);
    CHECK(candidate.context.concealed);
    state.target_id = 11;
    CHECK_FALSE(context(10).concealed);

    spells.buffs[1] = regular;
    CHECK(context(1).invisible);
    CHECK_FALSE(candidate.context.invisible_undead);
    spells.buffs[2] = undead;
    CHECK(context(1).invisible);
    CHECK(candidate.context.invisible_undead);
    spells.buffs[1] = {};
    spells.buffs[2] = {};
    connection.note_hide_state(true, false);
    CHECK(context(1).concealed);
    CHECK_FALSE(candidate.context.invisible);
    CHECK_FALSE(candidate.context.invisible_undead);
    target.is_corpse = true;
    target.invis_rank = 1;
    CHECK_FALSE(context(10).concealed);
}

TEST_CASE("nameplate canvas context: full identity replacement and removal retire prior observations") {
    mq_test::World world("canvas_context_identity");
    auto& connection = *world.conn;
    auto& group = net::GameConnectionTestAccess::group(connection);
    auto& casts = net::GameConnectionTestAccess::spells(connection);
    auto& aggro = net::GameConnectionTestAccess::aggro(connection);
    const auto seed = [&] {
        group.mana_percent[12] = 37; group.endurance_percent[12] = 65;
        casts.others[12] = {200, net::GameConnection::now_ms(), 60000};
        casts.target_buffs_entity = 12;
        game::BuffSlot buff; buff.spell = 200; casts.target_buffs.emplace_back(0, buff);
        aggro.target_id = 12; aggro.received[net::kAggroPlayer] = true;
    };
    const auto cleared = [&] {
        CHECK_FALSE(group.mana_percent.contains(12)); CHECK_FALSE(group.endurance_percent.contains(12));
        CHECK_FALSE(casts.others.contains(12)); CHECK(casts.target_buffs.empty());
        CHECK(casts.target_buffs_entity == 0); CHECK_FALSE(aggro.received[net::kAggroPlayer]);
    };
    const auto old = connection.world().spawns.at(12).incarnation;
    seed();
    net::Spawn replacement{};
    replacement.spawn_id = 12; replacement.name = "Groupie";
    net::GameConnectionTestAccess::replace_spawn(connection, replacement);
    CHECK(connection.world().spawns.at(12).incarnation != old);
    cleared();
    seed();
    connection.receive_zone_packet_for_test(net::encode_delete_spawn(12));
    cleared();
    CHECK_FALSE(connection.world().spawns.contains(12));
}
