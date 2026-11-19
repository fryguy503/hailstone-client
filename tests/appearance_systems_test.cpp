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

TEST_CASE("Name marks wrap a visible name for each kind of invisibility") {
    const auto marked = [](std::string name, game::NameInvisibility invisibility) {
        game::apply_name_invisibility(name, invisibility);
        return name;
    };
    CHECK(marked("Visitor", {}) == "Visitor");
    CHECK(marked("Visitor", {true, false}) == "(Visitor)");
    CHECK(marked("Visitor", {false, true}) == "[Visitor]");
    CHECK(marked("Visitor", {true, true}) == "([Visitor])");
    CHECK(marked("", {true, true}).empty());
}

TEST_CASE("Name marks use the spawn rank, own buffs and current target buffs") {
    Rig rig;
    REQUIRE(rig.to_zone());
    const auto own = rig.conn.world().player_id;
    REQUIRE(rig.conn.world().player());

    const auto spell_row = [](std::string id, std::string spa) {
        std::vector<std::string> fields(237, "0");
        fields[0] = std::move(id);
        fields[1] = "Fixture invisibility";
        std::fill(fields.begin() + 86, fields.begin() + 98, "254");
        fields[86] = std::move(spa);
        std::string row = fields.front();
        for (std::size_t i = 1; i < fields.size(); ++i) {
            row.push_back('^');
            row += fields[i];
        }
        return row;
    };
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_row("6701", "314") + "\n" + spell_row("6702", "28") + "\n" +
                         spell_row("6703", "315") + "\n" + spell_row("6704", "29")) == 4);
    rig.conn.set_spell_table_for_test(table);

    auto visitor = make_spawn("Visitor", 301, 0, 1, 0, 0, 3);
    visitor.hp_percent = 100;
    rig.conn.receive_zone_packet_for_test(net::encode(visitor));
    auto second = visitor;
    second.spawn_id = 302;
    rig.conn.receive_zone_packet_for_test(net::encode(second));

    const auto local_marks = [&](bool undead) {
        return rig.conn.name_invisibility(*rig.conn.world().player(), undead);
    };
    const auto visitor_marks = [&](bool undead) {
        return rig.conn.name_invisibility(rig.conn.world().spawns.at(visitor.spawn_id), undead);
    };
    CHECK_FALSE(local_marks(true).normal);
    CHECK_FALSE(local_marks(true).undead);
    // Hide is not among UpdateMyVisibleStatus's effects, and stock EQEmu tells the hider nothing: the own name
    // stays plain.
    rig.conn.note_hide_state(true, false);
    CHECK_FALSE(local_marks(true).normal);
    rig.conn.note_hide_state(false, false);

    net::BuffIcons own_buffs;
    own_buffs.entity_id = own;
    own_buffs.all_buffs = 1;
    own_buffs.entries = {{0, 6701, 100, 0, {}}, {1, 6702, 100, 0, {}}};
    rig.conn.receive_zone_packet_for_test(net::encode(own_buffs, false));
    CHECK(local_marks(false).normal);
    CHECK_FALSE(local_marks(false).undead);
    CHECK(local_marks(true).normal);
    CHECK(local_marks(true).undead);
    own_buffs.entries = {{0, 6704, 100, 0, {}}};
    rig.conn.receive_zone_packet_for_test(net::encode(own_buffs, false));
    CHECK_FALSE(local_marks(true).normal);
    CHECK_FALSE(local_marks(true).undead);

    CHECK_FALSE(visitor_marks(true).normal);
    CHECK_FALSE(visitor_marks(true).undead);
    rig.conn.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{301, 3, 2}));
    CHECK(visitor_marks(true).normal);
    CHECK_FALSE(visitor_marks(true).undead);

    rig.conn.set_target(visitor.spawn_id);
    net::BuffIcons target_buffs;
    target_buffs.entity_id = visitor.spawn_id;
    target_buffs.all_buffs = 1;
    target_buffs.type = 5;
    target_buffs.entries = {{0, 6703, 100, 0, {}}};
    rig.conn.receive_zone_packet_for_test(net::encode(target_buffs, true));
    CHECK(visitor_marks(true).undead);
    CHECK_FALSE(visitor_marks(false).undead);

    target_buffs.all_buffs = 0;
    target_buffs.entries[0].spell_id = 0xffffffffu;
    rig.conn.receive_zone_packet_for_test(net::encode(target_buffs, true));
    CHECK_FALSE(visitor_marks(true).undead);
    target_buffs.entries[0].spell_id = 6703;
    rig.conn.receive_zone_packet_for_test(net::encode(target_buffs, true));
    CHECK(visitor_marks(true).undead);

    rig.conn.receive_zone_packet_for_test(net::encode_target(true, second.spawn_id));
    CHECK_FALSE(visitor_marks(true).undead);
    rig.conn.receive_zone_packet_for_test(net::encode(target_buffs, true));
    CHECK_FALSE(visitor_marks(true).undead);
    // The list is the visitor's (as MQ's Spawn.Buff keeps it): it counts again once the visitor is the target.
    rig.conn.set_target(visitor.spawn_id);
    CHECK(visitor_marks(true).undead);

    // SetNameSpriteState marks by HideMode alone: a living spawn the server shows at 0 % still has it; a race from
    // 0x8cd up keeps its plain name.
    rig.conn.world_for_test().spawns.at(visitor.spawn_id).hp_percent = 0;
    CHECK(visitor_marks(false).normal);
    rig.conn.world_for_test().spawns.at(visitor.spawn_id).race = 0x8cd;
    CHECK_FALSE(visitor_marks(true).normal);
    CHECK_FALSE(visitor_marks(true).undead);
    rig.conn.world_for_test().spawns.at(visitor.spawn_id).race = 1;
    rig.conn.world_for_test().spawns.at(visitor.spawn_id).hp_percent = 100;
    visitor.name = "New_Visitor";
    rig.conn.receive_zone_packet_for_test(net::encode(visitor));
    CHECK_FALSE(visitor_marks(true).normal);
    CHECK_FALSE(visitor_marks(true).undead);

    visitor.npc = 2;
    visitor.invis = 2;
    rig.conn.receive_zone_packet_for_test(net::encode(visitor));
    rig.conn.receive_zone_packet_for_test(net::encode(target_buffs, true));
    CHECK_FALSE(visitor_marks(true).normal);
    CHECK_FALSE(visitor_marks(true).undead);
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
