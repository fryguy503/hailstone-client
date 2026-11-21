#include <doctest/doctest.h>

#include "app/nameplate_canvas_context.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/packet_io.h"
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

TEST_CASE("nameplate canvas context: a rogue's cancel reply clears Hide for a group observer") {
    auto config = test::MockConfig::standard();
    config.own_spawn_id = 12;
    test::Rig rogue(std::move(config));
    REQUIRE(rogue.to_zone());
    const std::uint16_t rogue_id = 12;
    mq_test::World group_member("canvas_context_hide_cancel");
    auto& observer = *group_member.conn;
    game::NameplateCanvasCandidate candidate;
    const auto concealed = [&] {
        const auto& spawn = observer.world().spawns.at(rogue_id);
        app::fill_nameplate_canvas_candidate(candidate, observer, spawn, spawn.name, 10);
        CHECK(candidate.context.in_group);
        CHECK(observer.name_invisibility(spawn, false).normal == candidate.context.concealed);
        return candidate.context.concealed;
    };

    // Successful Hide is announced to observers only. Test with and without a matching local roll.
    for (bool local_hide : {false, true}) {
        CAPTURE(local_hide);
        observer.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{rogue_id, net::kAppearanceInvisibility, 1}));
        REQUIRE(concealed());
        rogue.conn.note_hide_state(local_hide, true);

        rogue.server.zone_log.clear();
        rogue.server.zone.send_reliable(net::make_empty(net::Op::CancelSneakHide));
        using net::Op;
        REQUIRE(rogue.until([&] {
            return test::MockEqServer::count(rogue.server.zone_log, Op::SpawnAppearance) >= 2;
        }));
        CHECK_FALSE(rogue.conn.invisible());
        CHECK_FALSE(rogue.conn.sneaking());
        std::vector<net::AppPacket> reply;
        for (const auto& packet : rogue.server.zone_log) {
            const Op op = net::from_wire(packet.opcode);
            if (op == Op::Hide || op == Op::SpawnAppearance) reply.push_back(packet);
        }
        REQUIRE(test::MockEqServer::ops(reply) == std::vector<Op>{Op::Hide, Op::SpawnAppearance, Op::SpawnAppearance});
        CHECK(reply[0].data == std::vector<std::byte>(4));
        const auto visibility = net::decode_spawn_appearance(reply[1]);
        const auto sneak = net::decode_spawn_appearance(reply[2]);
        REQUIRE(visibility);
        REQUIRE(sneak);
        CHECK(visibility->spawn_id == rogue_id);
        CHECK(visibility->type == net::kAppearanceInvisibility);
        CHECK(visibility->parameter == 0);
        CHECK(sneak->spawn_id == rogue_id);
        CHECK(sneak->type == net::kAppearanceSneak);
        CHECK(sneak->parameter == 0);

        // EQEmu forwards the appearance change to other clients using the same packet.
        observer.receive_zone_packet_for_test(reply[1]);
        CHECK_FALSE(concealed());
    }
}

TEST_CASE("nameplate canvas context: cancelling Hide preserves an active invisibility spell") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& connection = rig.conn;
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(mq_test::spell_line({{0, "1000"}, {1, "Test invisibility"}, {86, "12"}})) == 1);
    connection.set_spell_table_for_test(table);
    auto& buff = net::GameConnectionTestAccess::spells(connection).buffs[0];
    buff.spell = 1000;
    buff.tics = 10;
    buff.received_ms = net::GameConnection::now_ms();
    connection.note_hide_state(true, true);
    rig.server.zone_log.clear();
    rig.server.zone.send_reliable(net::make_empty(net::Op::CancelSneakHide));
    REQUIRE(rig.until([&] {
        return test::MockEqServer::count(rig.server.zone_log, net::Op::SpawnAppearance) > 0;
    }));
    CHECK(connection.invisible());
    CHECK_FALSE(connection.sneaking());
    CHECK(test::MockEqServer::count(rig.server.zone_log, net::Op::Hide) == 1);
    for (const auto& packet : rig.server.zone_log) {
        if (net::from_wire(packet.opcode) != net::Op::SpawnAppearance) continue;
        const auto appearance = net::decode_spawn_appearance(packet);
        REQUIRE(appearance);
        CHECK(appearance->type == net::kAppearanceSneak);
    }
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
