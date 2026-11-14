#include <doctest/doctest.h>

#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "mock_server/harness.h"
#include "net/group_packets.h"
#include "net/social_packets.h"
#include "net/spell_packets.h"

using namespace hailstone;

TEST_CASE("Spell Awareness: active group or raid ranks identify nearby casts through player level plus five") {
    auto config = test::MockConfig::standard();
    for (const auto& [id, name, npc, level, class_id] :
         std::vector<std::tuple<std::uint32_t, std::string, std::uint8_t, std::uint8_t, std::uint8_t>>{
             {201, "Watcher", 1, 55, 2}, {202, "Stronger", 1, 56, 2},
             {203, "Caster", 0, 55, 2}, {204, "Singer", 1, 55, 8}}) {
        auto spawn = test::make_spawn(name, id, npc, 1, -285, -148, 3.5f);
        spawn.level = level;
        spawn.class_ = class_id;
        config.zones.at(2).spawns.push_back(std::move(spawn));
    }
    test::Rig rig(config);
    std::ofstream(rig.dir.path() / "eqstr_us.txt", std::ios::app)
        << "12205 SELF %1\n12206 CAST %1\n12207 SONG %1%2\n";
    rig.conn.reload_game_files();
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    REQUIRE(conn.world().player());
    REQUIRE(conn.world().player()->level == 50);
    auto spells = std::make_shared<game::SpellTable>();
    std::vector<std::string> fields(237, "0");
    fields[0] = "10";
    fields[1] = "Awareness fixture";
    fields[5] = " sings.";
    std::string record;
    for (const auto& field : fields) {
        if (!record.empty()) record += '^';
        record += field;
    }
    REQUIRE(spells->parse(record + '\n') == 1);
    conn.set_spell_table_for_test(spells);
    auto cast = [&](std::uint16_t caster, const std::string& expected) {
        rig.chat();
        conn.receive_zone_packet_for_test(net::encode(net::BeginCast{10, caster, 1500}));
        std::vector<std::string> messages;
        for (const auto& line : rig.chat())
            if (line.kind == net::ChatEvent::Kind::Spell) messages.push_back(line.text);
        CHECK(messages == std::vector<std::string>{expected});
    };

    net::GroupUpdateB group;
    group.group_id = 7;
    group.leader = conn.character_name();
    for (const std::string& name : {conn.character_name(), std::string("Friend"), std::string("Other")}) {
        net::GroupUpdateMember member;
        member.slot = static_cast<std::uint32_t>(group.members.size());
        member.name = name;
        group.members.push_back(member);
    }
    conn.receive_zone_packet_for_test(net::encode(group));
    cast(201, "CAST Watcher");
    net::GroupLeadershipUpdate group_ranks;
    group_ranks.ranks[8] = 1;
    conn.receive_zone_packet_for_test(net::encode(group_ranks));
    REQUIRE(conn.leadership_ability_active(8, 1));
    cast(201, "CAST Watcher <Awareness fixture>");
    cast(202, "CAST Stronger"); // six levels above the player
    cast(203, "CAST Caster <Awareness fixture>"); // the original also identifies another player's cast
    cast(204, "SONG Singer sings. <Awareness fixture>"); // the suffix preserves a bard's original message
    cast(static_cast<std::uint16_t>(config.own_spawn_id), "SELF Awareness fixture");
    group_ranks.ranks[8] = 0;
    conn.receive_zone_packet_for_test(net::encode(group_ranks));
    cast(201, "CAST Watcher");

    net::RaidUpdate raid;
    raid.general.action = net::kRaidAddMember;
    raid.general.leader_name = conn.character_name();
    for (int i = 0; i < 18; ++i) {
        raid.general.player_name = i ? "Member" + std::to_string(i) : conn.character_name();
        conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
    }
    raid.general.action = net::kRaidSetLeaderAbilities;
    raid.leadership[0x17] = 1;
    conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
    REQUIRE(conn.leadership_ability_active(0x17, 1));
    cast(201, "CAST Watcher <Awareness fixture>");
    raid.general.action = net::kRaidRemoveMember;
    raid.general.player_name = "Member17";
    conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
    REQUIRE_FALSE(conn.leadership_ability_active(0x17, 1));
    cast(201, "CAST Watcher"); // retaining the rank does not retain the benefit below eighteen members

    conn.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{
        static_cast<std::uint16_t>(config.own_spawn_id), 20, 1}));
    cast(202, "CAST Stronger (Awareness fixture)"); // the native GM spelling is independent of ranks and level
}
