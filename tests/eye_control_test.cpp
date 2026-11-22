#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "app/player_movement.h"
#include "core/byte_io.h"
#include "game/coords.h"
#include "game/eye_control.h"
#include "game/mechanics/sheet_builder.h"
#include "game/world_state.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/door_packets.h"
#include "net/spell_packets.h"

using namespace hailstone;

namespace {
net::Spawn eye_packet(const std::string& player, std::uint32_t id = 987) {
    net::Spawn eye;
    eye.spawn_id = id;
    eye.name = game::eye_name_for(player) + "000";
    eye.npc = 1;
    eye.race = 108;
    eye.size = 1;
    eye.fly_mode = 1;
    eye.run_speed = 0.7f;
    eye.walk_speed = 0.46f;
    eye.stand_state = net::kStandStanding;
    eye.motion.x = 10;
    eye.motion.y = 20;
    eye.motion.z = 30;
    return eye;
}

struct EyeRig : test::Rig {
    std::int64_t next_tick = 0;
    void enter() {
        std::filesystem::create_directories(dir.path() / "Resources");
        std::ofstream(dir.path() / "Resources" / "BaseData.txt") << "60^2^500^800^300^3^5^0.75^14^1.25\n";
        game.rescan();
        REQUIRE(to_zone());
        auto profile = *conn.profile();
        profile.class_ = 2;
        profile.level = 60;
        conn.set_profile_for_test(profile);
        conn.player_status_mut().level = 60;
        auto spells = std::make_shared<game::SpellTable>();
        REQUIRE(spells->parse(mq_test::spell_line({{0, "6700"}, {1, "Fixture Eye"}, {86, "67"},
                                                 {16, "1"}, {17, "10"}, {83, "1"}})) == 1);
        conn.set_spell_table_for_test(spells);
        next_tick = net::GameConnection::now_ms() + 7000;
    }
    void buff(bool fade = false) {
        net::BuffPacket packet;
        packet.entity_id = conn.world().player_id;
        packet.slot = 2;
        packet.fade = fade ? 1u : 0u;
        packet.buff.effect_type = 2;
        packet.buff.spell_id = 6700;
        packet.buff.level = 60;
        packet.buff.duration = 10;
        conn.receive_zone_packet_for_test(net::encode(packet));
    }
    void spawn(net::Spawn eye) {
        auto packet = net::encode(eye);
        packet.opcode = net::to_wire(net::Op::NewSpawn);
        conn.receive_zone_packet_for_test(packet);
    }
    void tick() {
        conn.passage_of_time_for_test(next_tick);
        next_tick += 6001;
    }
};
}

TEST_CASE("Eye ownership uses a terminated 24-byte name and refuses ambiguous partial matches") {
    game::WorldState world;
    world.player_id = 1;
    auto& player = world.spawns[1];
    player.id = 1;
    player.name = "abcdefghijklmnopqrstuvwxTAIL";
    CHECK(game::eye_name_for(player.name) == "Eye_of_abcdefghijklmnopqrstuvwx");
    auto& eye = world.spawns[2];
    eye.id = 2;
    eye.name = "eye_OF_abcdefghijklmnopqrstuvwx000";
    CHECK(game::find_eye_summon(world) == 2);
    player.position = {1, 2, 3};
    eye.position = {100, 200, 300};
    player.speed_run = 0.7f;
    player.speed_heading = 5;
    CHECK(game::take_eye_control(world, 2));
    CHECK(player.position == glm::vec3(1, 2, 3));
    CHECK(player.speed_run == 0);
    CHECK(player.speed_heading == 0);
    CHECK(world.eye_id == 2);
    CHECK(world.view_id == 2);
    CHECK(world.controlled_id == 2);
    CHECK_FALSE(game::take_eye_control(world, 2));
    world.set_local_drawn({101, 201, 301}, 10);
    CHECK_FALSE(player.display_valid);
    CHECK(eye.display_position == glm::vec3(101, 201, 301));
    CHECK(game::release_eye_control(world));
    CHECK(world.controlled_id == 0);
    CHECK(world.view_id == 0);
    world.spawns[3] = eye;
    world.spawns[3].id = 3;
    CHECK(game::find_eye_summon(world) == 0);
    world.spawns[3].is_corpse = true;
    CHECK(game::find_eye_summon(world) == 2);
}

TEST_CASE("An Eye buff controls and reports the Eye while preserving the player's body") {
    EyeRig rig;
    rig.enter();
    auto& c = rig.conn;
    const auto body = c.world().player()->position;
    rig.spawn(eye_packet(c.world().player()->name));
    rig.buff();
    CHECK(c.eye_id() == 0); // ProcessAffects selects the summon at its normal cadence.
    rig.tick();
    REQUIRE(c.eye_id() == 987);
    CHECK(c.world().view_id == 987);
    CHECK(c.world().player()->position == body);
    CHECK_FALSE(c.live_state().running);
    c.set_controlled_motion({100, 200, 30}, 128, {0.5f, 0, 0}, 0.7f);
    c.send_position_now();
    REQUIRE(rig.until([&] {
        for (const auto& update : rig.server.client_updates) if (update.spawn_id == 987) return true;
        return false;
    }));
    bool moved_eye = false;
    for (const auto& update : rig.server.client_updates)
        if (update.spawn_id == 987) {
            moved_eye = true;
            CHECK(update.motion.x == 200);
            CHECK(update.motion.y == 100);
            CHECK(update.motion.z == 30);
        } else if (update.spawn_id == c.world().player_id) {
            const auto expected = game::world_to_server(body);
            CHECK(update.motion.x == expected.x);
            CHECK(update.motion.y == expected.y);
            CHECK(update.motion.z == expected.z);
        }
    CHECK(moved_eye);
    CHECK(c.world().player()->position == body);
    REQUIRE(test::MockEqServer::count(rig.server.zone_log, net::Op::AckPacket) > 0);
    for (const auto& packet : rig.server.zone_log)
        if (packet.opcode == net::to_wire(net::Op::AckPacket)) {
            REQUIRE(packet.data.size() == 4);
            ByteReader reader(packet.data);
            CHECK(reader.le<std::uint32_t>() == 987);
        }
    rig.buff(true);
    CHECK(c.eye_id() == 0);
    CHECK(c.controlled_id() == 0);
    CHECK(c.world().view_id == 0);
    CHECK(c.world().player()->position == body);
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::AckPacket) >= 2; }));
    const net::AppPacket* restored = nullptr;
    for (const auto& packet : rig.server.zone_log)
        if (packet.opcode == net::to_wire(net::Op::AckPacket)) restored = &packet;
    REQUIRE(restored);
    ByteReader restored_view(restored->data);
    CHECK(restored_view.le<std::uint32_t>() == 0);
}

TEST_CASE("Mouse and gamepad turning follow the Eye's posture while its owner sits") {
    EyeRig rig;
    rig.enter();
    auto& c = rig.conn;
    constexpr std::uint16_t eye_id = 987;
    rig.spawn(eye_packet(c.world().player()->name, eye_id));
    rig.buff();
    rig.tick();
    REQUIRE(c.eye_id() == eye_id);

    REQUIRE(c.set_stand_state(net::kStandSitting));
    CHECK(app::can_turn_controlled_actor(c));
    c.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{eye_id, net::kAppearanceAnimation, net::kStandFreeze}));
    CHECK_FALSE(app::can_turn_controlled_actor(c));
    c.receive_zone_packet_for_test(net::encode(net::SpawnAppearance{eye_id, net::kAppearanceAnimation, net::kStandCrouching}));
    CHECK(app::can_turn_controlled_actor(c));

    ByteWriter stun;
    stun.le<std::uint32_t>(1000);
    c.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::Stun), stun.take()));
    REQUIRE(c.stunned());
    CHECK_FALSE(app::can_turn_controlled_actor(c));
    rig.tick();
    REQUIRE_FALSE(c.stunned());
    CHECK(app::can_turn_controlled_actor(c));

    rig.buff(true);
    REQUIRE(c.eye_id() == 0);
    CHECK(c.sitting());
    CHECK_FALSE(app::can_turn_controlled_actor(c));
    REQUIRE(c.set_stand_state(net::kStandStanding));
    CHECK(app::can_turn_controlled_actor(c));
}

TEST_CASE("An Eye's view, control and physics incarnation retire at a zone hand-off") {
    EyeRig rig;
    rig.enter();
    auto& c = rig.conn;
    rig.spawn(eye_packet(c.world().player()->name));
    rig.buff();
    rig.tick();
    REQUIRE(c.eye_id() == 987);
    c.receive_world_packet_for_test(net::encode(net::ZoneServerInfo{"127.0.0.1", 9000}));
    CHECK(c.eye_id() == 0);
    CHECK(c.controlled_id() == 0);
    CHECK(c.world().view_id == 0);
    CHECK(c.world().spawns.empty());
}

TEST_CASE("Door clicks follow the controlled Eye and return to the player's body when it fades") {
    EyeRig rig;
    rig.enter();
    auto& c = rig.conn;
    const glm::vec3 body = c.world().player()->position;
    c.set_player_motion(body, 0, {}, false);
    const glm::vec3 nearby_hit = body + glm::vec3{2, 0, 0};
    const glm::vec3 distant_hit = body + glm::vec3{100, 0, 0};
    const auto door_at = [](std::uint8_t id, glm::vec3 position) {
        const auto server = game::world_to_server(position);
        net::DoorRecord door;
        door.name = "TESTDOOR";
        door.door_id = id;
        door.x = server.x;
        door.y = server.y;
        door.z = server.z;
        return door;
    };
    c.receive_zone_packet_for_test(net::encode_spawn_doors({
        door_at(1, nearby_hit), door_at(2, distant_hit + glm::vec3{30, 0, 0})}));
    REQUIRE(c.door_in_reach(1, 0, nearby_hit));
    CHECK_FALSE(c.door_in_reach(2, 0, distant_hit));

    rig.spawn(eye_packet(c.world().player()->name));
    rig.buff();
    rig.tick();
    REQUIRE(c.eye_id() == 987);
    CHECK(c.world().view_spawn_id() == 987);
    c.set_controlled_motion(distant_hit, 0, {}, 0);
    CHECK(c.door_in_reach(2, 0, distant_hit)); // The picked surface is in reach even when the pivot is farther away.
    CHECK_FALSE(c.door_in_reach(2, 0));
    REQUIRE(c.click_door(2, 0, 123, 35, distant_hit));
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::ClickDoor) == 1; }));
    for (const auto& packet : rig.server.zone_log) {
        if (packet.opcode != net::to_wire(net::Op::ClickDoor)) continue;
        const auto click = net::decode_click_door(packet);
        REQUIRE(click);
        CHECK(click->door_id == 2);
        CHECK(click->player_id == 987);
        CHECK(click->item_id == 123);
        CHECK(click->pick_skill == 35);
    }

    c.set_controlled_motion(nearby_hit + glm::vec3{0, 0, 22}, 0, {}, 0);
    CHECK_FALSE(c.door_in_reach(1, 0, nearby_hit)); // The size-one Eye reaches 21 units, including vertically.
    CHECK_FALSE(c.click_door(1, 0, -1, 0, nearby_hit));
    rig.buff(true);
    REQUIRE(c.eye_id() == 0);
    CHECK(c.world().view_spawn_id() == c.world().player_id);
    CHECK(c.door_in_reach(1, 0, nearby_hit));
    CHECK_FALSE(c.door_in_reach(2, 0, distant_hit));
    REQUIRE(c.click_door(1, 0, -1, 0, nearby_hit));
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::ClickDoor) == 2; }));
    for (auto i = rig.server.zone_log.rbegin(); i != rig.server.zone_log.rend(); ++i) {
        if (i->opcode != net::to_wire(net::Op::ClickDoor)) continue;
        const auto click = net::decode_click_door(*i);
        REQUIRE(click);
        CHECK(click->door_id == 1);
        CHECK(click->player_id == c.world().player_id);
        break;
    }
}

TEST_CASE("Eye depop, reused spawn IDs and death retire control and the summon buff") {
    EyeRig rig;
    rig.enter();
    auto& c = rig.conn;
    auto eye = eye_packet(c.world().player()->name);
    rig.spawn(eye);
    rig.buff();
    rig.tick();
    REQUIRE(c.eye_id() == 987);
    c.receive_zone_packet_for_test(net::encode_delete_spawn(987));
    CHECK(c.eye_id() == 0);
    CHECK(c.spell_state().buffs[2].empty());
    eye.name = "a_rat000";
    rig.spawn(eye); // The same numeric ID is a different spawn now.
    rig.tick();
    CHECK(c.eye_id() == 0);
    eye.name = game::eye_name_for(c.world().player()->name) + "001";
    rig.spawn(eye);
    rig.buff();
    rig.tick();
    REQUIRE(c.eye_id() == 987);
    net::Death death;
    death.spawn_id = 987;
    death.corpse_id = 987;
    c.receive_zone_packet_for_test(net::encode(death));
    CHECK(c.eye_id() == 0);
    CHECK_FALSE(c.player_status().dead);
    CHECK(c.spell_state().buffs[2].empty());
    rig.spawn(eye);
    rig.buff();
    rig.tick();
    REQUIRE(c.eye_id() == 987);
    death.spawn_id = c.world().player_id;
    death.corpse_id = death.spawn_id;
    c.receive_zone_packet_for_test(net::encode(death));
    CHECK(c.eye_id() == 0);
    CHECK(c.player_status().dead);
}

TEST_CASE("A passive local body retains its own physics while an Eye receives movement") {
    mq_test::World fixture("hailstone_eye_body_test");
    auto& c = *fixture.conn;
    auto& world = net::GameConnectionTestAccess::world(c);
    world.spawns[987].id = 987;
    world.spawns[987].name = game::eye_name_for(world.player()->name) + "000";
    world.spawns[987].run_speed = 0.7f;
    world.spawns[987].fly_mode = 1;
    REQUIRE(game::take_eye_control(world, 987));
    app::PlayerMovement local, eye;
    local.reset({1, 2, 3}, 0);
    eye.reset({100, 200, 300}, 0);
    local.command(game::mechanics::kCmdForward, true, true);
    app::MovementKeys keys;
    keys.forward = true;
    const auto body = local.update({}, 0, 0, 50, 1000, c, nullptr, 0, nullptr, nullptr, 50, true);
    const auto viewed = eye.update(keys, 0, 0, 50, 1000, c, nullptr, 0, nullptr, nullptr, 50);
    CHECK(body.feet.x == 1);
    CHECK(body.feet.y == 2);
    CHECK(body.speed_run == 0);
    CHECK(viewed.speed_run > 0);
    CHECK(viewed.feet.x > 100);
    CHECK_FALSE(eye.controls_state().controlled_is_local);
}

TEST_CASE("Another player's Eye whose name extends the player's name is not this player's Eye") {
    game::WorldState world;
    world.player_id = 1;
    world.spawns[1].id = 1;
    world.spawns[1].name = "Bob";
    world.spawns[2].id = 2;
    world.spawns[2].name = "Eye_of_Bobby000";
    CHECK(game::find_eye_summon(world) == 0);
    CHECK_FALSE(game::is_own_eye(world, 2));
    CHECK_FALSE(game::take_eye_control(world, 2));
    world.spawns[3].id = 3;
    world.spawns[3].name = "Eye_of_Bob000";
    CHECK(game::find_eye_summon(world) == 3);
    CHECK(game::is_own_eye(world, 3));
    world.spawns[1].name = "Bobby";
    CHECK(game::find_eye_summon(world) == 2);
}

TEST_CASE("A held Eye stays held when a same-named Eye appears after control is taken") {
    EyeRig rig;
    rig.enter();
    auto& c = rig.conn;
    const auto name = c.world().player()->name;
    rig.spawn(eye_packet(name));
    rig.buff();
    rig.tick();
    REQUIRE(c.eye_id() == 987);
    auto other = eye_packet(name, 988);
    other.name = game::eye_name_for(name) + "001";
    rig.spawn(other); // find_eye_summon is ambiguous now, but the held Eye still exists.
    CHECK(game::find_eye_summon(c.world()) == 0);
    rig.tick();
    CHECK(c.eye_id() == 987);
    CHECK_FALSE(c.spell_state().buffs[2].empty());
    CHECK(test::MockEqServer::count(rig.server.zone_log, net::Op::BuffRemoveRequest) == 0);
}
