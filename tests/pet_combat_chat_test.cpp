#include <doctest/doctest.h>

#include <fstream>
#include <limits>
#include <string>
#include <utility>

#include "core/byte_io.h"
#include "core/config.h"
#include "game/death_report.h"
#include "mq_test_world.h"
#include "net/opcodes.h"

using namespace hailstone;

namespace {
struct PetCombatWorld : mq_test::World {
    PetCombatWorld() : World("hailstone_pet_combat_chat") {
        std::ofstream(dir / "eqstr_us.txt")
            << "EQST0002\n12185 You\n12187 YOU\n12162 %1 %2 %3 %4\n12160 for %1 damage.\n"
            << "12161 for %1 damage.\n12191 crush\n12192 crushes\n12158 %1 try to %2 %3, but %4!\n"
            << "12159 %1 tries to %2 %3, but %4!\n12156 miss\n";
        game->rescan();
        conn->reload_game_files();
    }

    void spawn(std::uint32_t id, std::uint8_t temporary, std::uint32_t owner,
               net::Op opcode = net::Op::NewSpawn) {
        net::Spawn s;
        s.spawn_id = id;
        s.name = "Pet" + std::to_string(id);
        s.npc = 1;
        s.race = 1;
        s.size = 6;
        s.is_pet = temporary == 0 && owner != 0;
        s.temp_pet = temporary;
        s.pet_owner_id = owner;
        auto packet = net::encode(s);
        packet.opcode = net::to_wire(opcode);
        conn->receive_zone_packet_for_test(packet);
    }

    void damage(std::uint16_t source, std::int32_t amount = 7, std::uint16_t target = 10) {
        net::CombatDamage hit;
        hit.target = target;
        hit.source = source;
        hit.type = 0;
        hit.damage = amount;
        conn->receive_zone_packet_for_test(net::encode(hit));
    }

    void remove(std::uint32_t id) {
        ByteWriter removed;
        removed.le(id);
        removed.le<std::uint8_t>(0);
        conn->receive_zone_packet_for_test({net::to_wire(net::Op::DeleteSpawn), removed.take()});
    }

    void expect_line(bool shown, std::uint32_t color = 0x148u) {
        auto event = conn->poll_chat();
        REQUIRE(event.has_value() == shown);
        if (event) {
            CHECK(event->kind == net::ChatEvent::Kind::Combat);
            CHECK(event->color == color);
            CHECK_FALSE(event->text.empty());
        }
        CHECK_FALSE(conn->poll_chat());
    }
};
} // namespace

TEST_CASE("Pet combat chat: zone and new spawns retain temporary state without replacing the window pet") {
    PetCombatWorld w;
    w.spawn(20, 0, 1, net::Op::ZoneEntry);
    REQUIRE(w.conn->pets().focused == 20);
    for (const auto& [id, temporary] : {std::pair{21u, std::uint8_t{1}}, std::pair{22u, std::uint8_t{0x80}}}) {
        CAPTURE(id);
        w.spawn(id, temporary, 1, id == 21 ? net::Op::ZoneEntry : net::Op::NewSpawn);
        const auto& spawn = w.conn->world().spawns.at(id);
        CHECK(spawn.temp_pet);
        CHECK(spawn.pet_owner_id == 1);
        CHECK_FALSE(spawn.summoned); // The independent is-pet bit is not needed for the temporary-pet rule.
        CHECK(w.conn->pets().focused == 20);
    }
    CHECK_FALSE(w.conn->world().spawns.at(20).temp_pet);
    w.conn->set_chat_filter(15, 0);
    for (auto id : {std::uint16_t{20}, std::uint16_t{21}, std::uint16_t{22}}) {
        w.damage(id);
        w.expect_line(true);
        CHECK(w.conn->pets().focused == 20);
    }
    w.remove(20);
    CHECK(w.conn->pets().focused == 0); // Remaining temporary pets do not become the pet-window fallback.
    w.damage(21);
    w.expect_line(true);
}

TEST_CASE("Pet combat chat: only the focused pet and locally owned temporary pets use pet filters and colour") {
    PetCombatWorld w;
    struct Case {
        std::uint16_t id;
        std::uint8_t temporary;
        std::uint32_t owner;
        bool owned;
    };
    const Case cases[] = {{20, 0, 1, true}, {21, 1, 1, true}, {22, 0x80, 1, true},
                          {23, 1, 12, false}, {24, 1, 0, false}, {25, 0, 1, false}, {26, 0, 0, false}};
    for (const auto& sample : cases) w.spawn(sample.id, sample.temporary, sample.owner);
    w.conn->set_window_pet(20);
    for (bool hide_others : {false, true}) {
        w.conn->set_chat_filter(14, hide_others ? 0 : 1);
        w.conn->set_chat_filter(15, hide_others ? 0 : 1);
        for (const auto& sample : cases) {
            for (std::int32_t amount : {7, 0}) {
                CAPTURE(hide_others);
                CAPTURE(sample.id);
                CAPTURE(amount);
                w.damage(sample.id, amount);
                w.expect_line(!hide_others || sample.owned, sample.owned ? 0x148u : (amount ? 279u : 280u));
            }
        }
    }
    // A pet attacking the local player keeps the normal you-being-hit colour and miss filter.
    w.damage(21, 7, 1);
    w.expect_line(true, 266);
    w.conn->set_chat_filter(16, 0);
    w.damage(21, 0, 1);
    w.expect_line(false);
}

TEST_CASE("Pet combat chat: current owner and replacement records determine temporary ownership") {
    PetCombatWorld w;
    w.spawn(20, 0, 1);
    w.spawn(21, 1, 1);
    w.conn->set_chat_filter(15, 0);
    for (std::uint32_t owner : {1u, 12u, 0u, 1u}) {
        w.conn->receive_zone_packet_for_test(net::encode(net::SpawnAppearance{21, 25, owner}));
        w.damage(21);
        w.expect_line(owner == 1);
        CHECK(w.conn->pets().focused == 20);
    }
    w.spawn(21, 0, 1); // A replacement record must clear the previous temporary-pet state.
    CHECK_FALSE(w.conn->world().spawns.at(21).temp_pet);
    w.conn->set_window_pet(20);
    w.damage(21);
    w.expect_line(false);
    w.spawn(21, 1, 1);
    w.damage(21);
    w.expect_line(true);

    w.remove(21);
    CHECK_FALSE(w.conn->world().spawns.contains(21));
    w.damage(21);
    w.expect_line(false);
    w.spawn(21, 0, 0); // Reuse of the id does not retain ownership from the deleted pet.
    w.damage(21);
    w.expect_line(false);

    w.spawn(21, 1, 0);
    w.conn->world_for_test().player_id = 0;
    w.conn->set_window_pet(0);
    w.damage(21);
    w.expect_line(false); // No local player must not make an ownerless temporary pet ours.
}

TEST_CASE("Pet combat chat: killing blows obey the death range with either others filter") {
    PetCombatWorld w;
    w.spawn(20, 0, 1);
    w.spawn(21, 1, 1);
    w.spawn(22, 1, 12);
    for (float setting : {0.0f, 75.0f, 200.0f, 300.0f}) {
        w.conn->set_combat_report_range(setting);
        // Unset: RoF2's 50 for OP_Death (game::report_death).
        const float range = setting > 0.0f ? setting : net::GameConnection::kRof2DeathReportRange;
        for (bool hide_others : {false, true}) {
            w.conn->set_chat_filter(15, hide_others ? 0 : 1);
            for (std::uint32_t killer : {20u, 21u, 22u}) {
                for (float distance : {range, range + 1.0f}) {
                    CAPTURE(range);
                    CAPTURE(hide_others);
                    CAPTURE(killer);
                    CAPTURE(distance);
                    w.spawn(30, 0, 0);
                    w.conn->world_for_test().spawns.at(30).position = {distance, 0, 0};
                    net::Death death;
                    death.spawn_id = 30;
                    death.killer_id = killer;
                    death.attack_skill = 0;
                    death.damage = 7;
                    w.conn->receive_zone_packet_for_test(net::encode(death));
                    const bool hit_shown = distance <= range && (!hide_others || killer != 22);
                    if (hit_shown) {
                        auto hit = w.conn->poll_chat();
                        REQUIRE(hit.has_value());
                        CHECK(hit->kind == net::ChatEvent::Kind::Combat);
                        CHECK(hit->color == (killer != 22 ? 0x148u : 279u));
                    }
                    // The separate death announcement follows range, not the others' melee filter.
                    auto announcement = w.conn->poll_chat();
                    REQUIRE(announcement.has_value() == (distance <= range));
                    if (announcement) CHECK(announcement->color == game::kOtherDeathColor);
                    CHECK_FALSE(w.conn->poll_chat());
                }
            }
        }
    }
}

TEST_CASE("Pet combat chat: the hit range defaults to RoF2's 75 from the target and filters cannot bypass it") {
    PetCombatWorld w;
    w.spawn(20, 0, 1);
    w.spawn(21, 1, 1);
    w.spawn(22, 0, 0);
    // Range is measured to the victim, not to the attacker.
    for (auto id : {20u, 21u, 22u}) w.conn->world_for_test().spawns.at(id).position = {1000, 0, 0};
    for (float setting : {0.0f, 200.0f}) {
        w.conn->set_combat_report_range(setting);
        const float range = setting > 0.0f ? setting : 75.0f;
        for (bool hide_others : {false, true}) {
            w.conn->set_chat_filter(14, hide_others ? 0 : 1);
            w.conn->set_chat_filter(15, hide_others ? 0 : 1);
            for (float distance : {50.0f, 75.0f, 75.25f, 135.0f, 200.0f, 200.25f}) {
                w.conn->world_for_test().spawns.at(10).position = {0, distance, 0};
                for (std::uint16_t source : {std::uint16_t{20}, std::uint16_t{21}, std::uint16_t{22}}) {
                    for (std::int32_t amount : {7, 0}) {
                        CAPTURE(setting);
                        CAPTURE(hide_others);
                        CAPTURE(distance);
                        CAPTURE(source);
                        CAPTURE(amount);
                        w.damage(source, amount);
                        w.expect_line(distance <= range && (!hide_others || source != 22),
                                      source != 22 ? 0x148u : (amount ? 279u : 280u));
                    }
                }
            }
        }
    }
    // Damage by or on the player is never range-limited.
    w.damage(1);
    w.expect_line(true, 265);
    w.damage(20, 7, 1);
    w.expect_line(true, 266);
}

TEST_CASE("Pet combat chat: the ini range overrides and invalid values keep RoF2's range") {
    PetCombatWorld w;
    w.spawn(20, 0, 1);
    for (const auto& [value, range] :
         {std::pair{"40", 40.0f}, {"300.5", 300.5f}, {"", 75.0f}, {"invalid", 75.0f},
          {"nan", 75.0f}, {"inf", 75.0f}, {"0", 75.0f}, {"-1", 75.0f}}) {
        CAPTURE(value);
        Config config;
        config.parse(std::string("combat.report_range = ") + value);
        w.conn->set_combat_report_range(config.get_float("combat.report_range", 0.0f));
        for (float distance : {range, range + 0.25f}) {
            w.conn->world_for_test().spawns.at(10).position = {distance, 0, 0};
            w.damage(20);
            w.expect_line(distance <= range);
        }
    }
    for (float range : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        w.conn->set_combat_report_range(range);
        w.conn->world_for_test().spawns.at(10).position = {75, 0, 0};
        w.damage(20);
        w.expect_line(true);
        w.conn->world_for_test().spawns.at(10).position.x = 76;
        w.damage(20);
        w.expect_line(false);
    }
}

TEST_CASE("Pet combat chat: death announcements use combat range and retain local exceptions") {
    PetCombatWorld w;
    {
        std::ofstream strings(w.dir / "eqstr_us.txt", std::ios::app);
        strings << "12114 %1 has been slain by %2!\n12113 You have slain %1!\n"
                << "12115 %1 died.\n12112 %1 has been slain!\n12107 You have been slain by %1!\n";
    }
    w.game->rescan();
    w.conn->reload_game_files();
    w.spawn(20, 0, 1);
    for (float setting : {0.0f, 75.0f, 200.0f, 300.0f}) {
        w.conn->set_combat_report_range(setting);
        const float range = setting > 0.0f ? setting : 50.0f; // unset: RoF2's 50
        for (std::uint32_t killer : {0u, 1u, 20u, 999u}) {
            for (float distance : {40.0f, range, range + 1.0f}) {
                CAPTURE(setting);
                CAPTURE(killer);
                CAPTURE(distance);
                w.spawn(30, 0, 0);
                w.conn->world_for_test().spawns.at(30).position = {distance, 0, 0};
                net::Death death;
                death.spawn_id = 30;
                death.killer_id = killer;
                death.attack_skill = 231; // Test death text without a second melee hit/miss line.
                death.damage = 7;
                w.conn->receive_zone_packet_for_test(net::encode(death));
                auto event = w.conn->poll_chat();
                REQUIRE(event.has_value() == (distance <= range || killer == 1));
                if (event) {
                    CHECK(event->kind == net::ChatEvent::Kind::Combat);
                    CHECK(event->color == game::kOtherDeathColor);
                    CHECK_FALSE(event->text.empty());
                }
                CHECK_FALSE(w.conn->poll_chat());
                CHECK(w.conn->world().spawns.at(30).is_corpse); // Range never suppresses death handling.
            }
        }
    }
    // The player's own death is never hidden, even with the killer outside the configured range.
    w.conn->set_combat_report_range(1);
    w.conn->world_for_test().spawns.at(20).position = {1000, 0, 0};
    net::Death death;
    death.spawn_id = 1;
    death.killer_id = 20;
    death.attack_skill = 231;
    death.damage = 7;
    w.conn->receive_zone_packet_for_test(net::encode(death));
    auto event = w.conn->poll_chat();
    REQUIRE(event.has_value());
    CHECK(event->color == game::kOwnDeathColor);
    CHECK(event->text == "You have been slain by Pet!"); // Display names strip the spawn's numeric suffix.
    CHECK(w.conn->world().spawns.at(1).is_corpse);
}
