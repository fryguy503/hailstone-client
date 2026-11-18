#include <doctest/doctest.h>
#include <algorithm>
#include "core/byte_io.h"
#include "game/appearance.h"
#include "game/light_list.h"
#include "mock_server/harness.h"
#include "net/opcodes.h"
#include "net/group_packets.h"
#include "net/spell_packets.h"
using namespace hailstone;
using namespace hailstone::test;

TEST_CASE("Guild banner appearance follows spawn, in-place updates and replacement") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto spawn = make_spawn("Guild_Banner", 301, 1, 553, 20, 30, 3);
    auto& interactive = spawn.interactive.emplace();
    interactive.object_type = 4;
    interactive.banner = core::BannerAppearance{{1, 2}, {0xff123456u, 0xffabcdefu}};
    rig.conn.receive_zone_packet_for_test(net::encode(spawn));
    const auto live = [&]() -> const auto& { return rig.conn.world().spawns.at(301).appearance.banner; };
    CHECK(live() == interactive.banner);
    const auto incarnation = rig.conn.world().spawns.at(301).incarnation;
    const auto update = [](std::uint32_t id, const std::string& name) {
        ByteWriter w;
        w.zeros(8);
        w.le(id);
        w.le<std::int32_t>(10);
        w.le<std::int32_t>(26);
        w.le<std::uint32_t>(0xff669944u);
        w.le<std::uint32_t>(0xff112266u);
        w.bytes(std::as_bytes(std::span{name.c_str(), name.size() + 1}));
        return net::AppPacket(net::to_wire(net::Op::GuildBannerAppearance), w.take());
    };
    auto packet = update(301, "Renamed_Guild_Banner");
    rig.conn.receive_zone_packet_for_test(packet);
    const core::BannerAppearance expected{{10, 26}, {0xff669944u, 0xff112266u}};
    CHECK(live() == expected);
    CHECK(rig.conn.world().spawns.at(301).name == "Renamed_Guild_Banner");
    CHECK(rig.conn.world().spawns.at(301).incarnation == incarnation);
    auto unterminated = update(301, "Unterminated");
    unterminated.data.pop_back();
    rig.conn.receive_zone_packet_for_test(unterminated);
    rig.conn.receive_zone_packet_for_test(update(301, std::string(64, 'A')));
    CHECK(rig.conn.world().spawns.at(301).name == "Renamed_Guild_Banner");
    rig.conn.receive_zone_packet_for_test(update(rig.conn.world().player_id, "Non_Banner"));
    CHECK_FALSE(rig.conn.world().player()->appearance.banner);
    spawn.interactive.reset();
    rig.conn.receive_zone_packet_for_test(net::encode(spawn));
    CHECK_FALSE(live());
}

TEST_CASE("Visibility: corpse and zero-HP gates, group awareness and GM ranks") {
    game::Spawn viewer, target;
    viewer.id = 1;
    target.id = 2;
    target.hp_percent = 100;
    target.invis_rank = 2;
    CHECK(game::spawn_invisible(viewer, target, 1, false));
    CHECK_FALSE(game::spawn_invisible(viewer, target, 2, false));
    CHECK_FALSE(game::spawn_invisible(viewer, target, 0, true));
    target.gm = true;
    CHECK(game::spawn_invisible(viewer, target, 2, true));
    CHECK_FALSE(game::spawn_invisible(viewer, target, 1, true)); // awareness override precedes rank/GM test
    viewer.gm = true;
    CHECK_FALSE(game::spawn_invisible(viewer, target, 3001, false));
    target.is_corpse = true;
    CHECK_FALSE(game::spawn_invisible(viewer, target, 0, false));
    target.is_corpse = false;
    target.hp_percent = 0; // remote signed HP is represented by the server's percentage
    CHECK_FALSE(game::spawn_invisible(viewer, target, 0, false));
    target.hp_percent = 100;
    target.invis_rank = 0;
    target.properties = {1, 100};
    CHECK(game::spawn_invisible(viewer, target, 3001, true));
    viewer.invis_rank = 1;
    CHECK_FALSE(game::spawn_invisible(viewer, viewer, 0, false));
    viewer.invis_rank = 3001;
    CHECK(game::spawn_invisible(viewer, viewer, 3001, true));
}

TEST_CASE("Visibility integration: spawn, appearance, see-invisible buff, group and reused spawn id") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto s = make_spawn("Hidden_Visitor", 301, 0, 1, 0, 0, 3);
    s.hp_percent = 100;
    const auto hidden = [&] {
        return rig.conn.spawn_invisible(rig.conn.world().spawns.at(301), rig.conn.see_invisible_rank());
    };
    rig.conn.receive_zone_packet_for_test(net::encode(s));
    CHECK_FALSE(hidden());
    s.invis = 2;
    rig.conn.receive_zone_packet_for_test(net::encode(s));
    CHECK(rig.conn.world().spawns.at(301).invis_rank == 2);
    CHECK(hidden());
    rig.conn.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{301, 3, 0}));
    CHECK_FALSE(hidden());
    rig.conn.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{301, 3, 2}));
    CHECK(hidden());
    const auto own = rig.conn.world().player_id;
    rig.conn.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{static_cast<std::uint16_t>(own), 3, 2}));
    CHECK_FALSE(rig.conn.spawn_invisible(*rig.conn.world().player(), 0));

    std::vector<std::string> fields(237, "0");
    fields[0] = "6701";
    fields[1] = "Fixture see-invisible";
    fields[17] = "100";
    fields[20] = "2";
    fields[70] = "100";
    fields[83] = "1";
    for (std::size_t i = 86; i <= 97; ++i) fields[i] = "254";
    fields[86] = "13";
    std::string line;
    for (const auto& field : fields) line += (line.empty() ? "" : "^") + field;
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(line) == 1);
    rig.conn.set_spell_table_for_test(table);
    net::BuffPacket buff;
    buff.entity_id = own;
    buff.slot = 2;
    buff.buff.effect_type = 2;
    buff.buff.spell_id = 6701;
    buff.buff.level = 60;
    buff.buff.duration = 100;
    rig.conn.receive_zone_packet_for_test(net::encode(buff));
    CHECK(rig.conn.see_invisible_rank() == 2);
    CHECK_FALSE(hidden());
    buff.fade = 1;
    rig.conn.receive_zone_packet_for_test(net::encode(buff));
    CHECK(rig.conn.see_invisible_rank() == 0);
    CHECK(hidden());

    net::GroupUpdateB group;
    group.group_id = 7;
    group.leader = rig.conn.world().player()->name;
    net::GroupUpdateMember member;
    member.slot = 0;
    member.name = group.leader;
    group.members.push_back(member);
    member.slot = 1;
    member.name = s.name;
    group.members.push_back(member);
    rig.conn.receive_zone_packet_for_test(net::encode(group));
    CHECK_FALSE(hidden());
    group.members.pop_back();
    rig.conn.receive_zone_packet_for_test(net::encode(group));
    CHECK(hidden());

    rig.conn.receive_zone_packet_for_test(net::encode_delete_spawn(301));
    CHECK_FALSE(rig.conn.world().spawns.contains(301));
    s.invis = 0;
    rig.conn.receive_zone_packet_for_test(net::encode(s));
    CHECK_FALSE(hidden());
    s.npc = 2;
    s.invis = 2;
    rig.conn.receive_zone_packet_for_test(net::encode(s));
    CHECK_FALSE(hidden());
}

TEST_CASE("Carried light integration: active spawn light and updates survive equipment changes and id reuse") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto s = make_spawn("Lantern_Carrier", 301, 1, 1, 10, 20, 3);
    s.light = 2;
    rig.conn.receive_zone_packet_for_test(net::encode(s));
    const auto& spawn = rig.conn.world().spawns.at(301);
    CHECK(spawn.light_type == 2);
    auto light = game::spawn_carried_light(spawn);
    REQUIRE(light);
    CHECK(light->radius == game::carried_light(2, {})->radius);
    rig.conn.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{301, 5, 11}));
    CHECK(spawn.light_type == 11);
    CHECK(game::spawn_carried_light(spawn)->radius == game::carried_light(11, {})->radius);
    net::WearChange wear;
    wear.spawn_id = 301;
    wear.slot = game::kSlotPrimary;
    rig.conn.receive_zone_packet_for_test(net::encode(wear));
    CHECK(spawn.light_type == 11); // active light can also be innate or spell-derived
    rig.conn.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{301, 5, 0}));
    CHECK_FALSE(game::spawn_carried_light(spawn));
    rig.conn.receive_zone_packet_for_test(net::encode_delete_spawn(301));
    s.light = 0;
    rig.conn.receive_zone_packet_for_test(net::encode(s));
    CHECK_FALSE(game::spawn_carried_light(rig.conn.world().spawns.at(301)));
}

TEST_CASE("Carried light: the player's follows its own equipment, not the spawn record") {
    // EQEmu sets a client's spawn light only when it builds the spawn packet and sends no SpawnAppearance 5 for a
    // client, so the player's light comes from its inventory (game::carrier_light_value).
    Rig rig;
    REQUIRE(rig.to_zone());
    const std::uint32_t me = rig.conn.world().player_id;
    REQUIRE(rig.conn.world().spawns.contains(me));
    const game::Spawn& self = rig.conn.world().spawns.at(me);
    CHECK(self.light_type == 0);
    CHECK(game::carrier_light_value(self, me, rig.conn.inventory()) == 0);

    net::Item torch = make_item(5001, "Torch", net::kSlotSecondary);
    torch.light = 2;
    rig.server.item_packet(net::kItemPacketCharInventory, torch);
    REQUIRE(rig.until([&] { return rig.conn.inventory().at(net::ItemSlot::possessions(net::kSlotSecondary)) != nullptr; }));
    CHECK(self.light_type == 0); // the server sends nothing for it
    CHECK(game::carrier_light_value(self, me, rig.conn.inventory()) == 2);

    net::MoveItem gone;
    gone.from = net::ItemSlot::possessions(net::kSlotSecondary);
    gone.count = 0xffffffffu; // -1: the whole stack of one
    rig.conn.receive_zone_packet_for_test(net::encode_delete_item(gone));
    REQUIRE(rig.conn.inventory().at(net::ItemSlot::possessions(net::kSlotSecondary)) == nullptr);
    CHECK(game::carrier_light_value(self, me, rig.conn.inventory()) == 0);

    // Another spawn's light is its record's, whatever the player carries.
    rig.server.item_packet(net::kItemPacketCharInventory, torch);
    REQUIRE(rig.until([&] { return rig.conn.inventory().at(net::ItemSlot::possessions(net::kSlotSecondary)) != nullptr; }));
    auto other = make_spawn("Lantern_Carrier", 302, 1, 1, 10, 20, 3);
    other.light = 6;
    rig.conn.receive_zone_packet_for_test(net::encode(other));
    CHECK(game::carrier_light_value(rig.conn.world().spawns.at(302), me, rig.conn.inventory()) == 6);
}
