// The local player's fly mode through the connection: OP_SpawnAppearance 19 sets it outright
// (Handle_OP_SpawnAppearance @0x4b4b60) and it stands until the effect cache is next rebuilt, when
// CharacterZoneClient_UpdateLevitationState @0x433480 puts it back to what the buffs give (mode 1 excepted).
// The decision itself is tested in movement_test.cpp ("Local fly mode").

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "app/player_movement.h"
#include "game/spells.h"
#include "mock_server/harness.h"
#include "net/game_connection.h"
#include "net/spell_packets.h"
#include "net/zone_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using game::mechanics::FlyMode;

namespace {

AppPacket fly_appearance(std::uint32_t id, std::uint32_t mode) {
    SpawnAppearance a;
    a.spawn_id = static_cast<std::uint16_t>(id);
    a.type = 19;
    a.parameter = mode;
    return encode(a);
}

// A spells_us.txt line: one effect in slot 1 (the rest 254) with its base and Base2, a long duration.
std::string spell_line(int id, int spa, int base2 = 0) {
    std::vector<std::string> fl(237, "0");
    fl[0] = std::to_string(id);
    fl[1] = "Spell" + std::to_string(id);
    for (std::size_t k = 0; k < 12; ++k) {
        fl[86 + k] = "254";
        fl[70 + k] = "100";
    }
    fl[86] = std::to_string(spa);
    fl[20] = "1";
    fl[32] = std::to_string(base2);
    fl[16] = "1";
    fl[83] = "1";
    std::string out;
    for (std::size_t i = 0; i < fl.size(); ++i) out += (i ? "^" : "") + fl[i];
    return out + "\n";
}

void buff(GameConnection& conn, std::uint32_t slot, std::uint32_t spell, bool fade = false) {
    BuffPacket bp;
    bp.entity_id = conn.world().player_id;
    bp.slot = slot;
    bp.fade = fade ? 1u : 0u;
    bp.buff.effect_type = 2;
    bp.buff.level = 60;
    bp.buff.spell_id = spell;
    bp.buff.duration = 100;
    bp.buff.caster_id = conn.world().player_id;
    conn.receive_zone_packet_for_test(encode(bp));
}

struct FlyRig {
    test::Rig rig;
    app::PlayerMovement m;
    std::uint32_t now = 10000;

    FlyRig() {
        REQUIRE(rig.to_zone());
        auto table = std::make_shared<game::SpellTable>();
        REQUIRE(table->parse(spell_line(600, 57) + spell_line(601, 57, 1) + spell_line(602, 40)) == 3);
        rig.conn.set_spell_table_for_test(table);
        m.reset({0, 0, 0}, 0);
    }
    FlyMode frame() {
        now += 16;
        m.update({}, 0, 0, 16, now, rig.conn, nullptr, 3.75f, nullptr);
        return m.controller().body.fly_mode;
    }
    std::uint32_t me() const { return rig.conn.world().player_id; }
};

} // namespace

TEST_CASE("Fly mode: a server mode stands until the buffs change") {
    FlyRig r;
    auto& conn = r.rig.conn;
    CHECK(r.frame() == FlyMode::Ground);

    // #set flymode 3 with no levitation: kept frame after frame.
    conn.receive_zone_packet_for_test(fly_appearance(r.me(), 3));
    CHECK(r.frame() == FlyMode::Water);
    CHECK(r.frame() == FlyMode::Water);
    // An unrelated buff lands: the cache rebuild puts it back on the ground.
    buff(conn, 0, 602);
    CHECK(r.frame() == FlyMode::Ground);
    // The same value again is a new setting.
    conn.receive_zone_packet_for_test(fly_appearance(r.me(), 3));
    CHECK(r.frame() == FlyMode::Water);
    // A value above 5 (PlayerPhysics_SetFlyMode throws) changes nothing.
    conn.receive_zone_packet_for_test(fly_appearance(r.me(), 9));
    CHECK(r.frame() == FlyMode::Water);
}

TEST_CASE("Fly mode: #set flymode 0 while levitating lasts until the buffs change") {
    FlyRig r;
    auto& conn = r.rig.conn;
    r.frame();
    buff(conn, 0, 600);
    CHECK(r.frame() == FlyMode::Levitate);
    conn.receive_zone_packet_for_test(fly_appearance(r.me(), 0));
    CHECK(r.frame() == FlyMode::Ground);
    CHECK(r.frame() == FlyMode::Ground);
    buff(conn, 1, 602);
    CHECK(r.frame() == FlyMode::Levitate);
    // Levitation while running from Base2 1, after the first fades.
    buff(conn, 0, 600, true);
    CHECK(r.frame() == FlyMode::Ground);
    buff(conn, 2, 601);
    CHECK(r.frame() == FlyMode::LevitateRun);
}

TEST_CASE("Fly mode: a server-set flying mode survives buff changes until a levitate buff lands or fades") {
    FlyRig r;
    auto& conn = r.rig.conn;
    r.frame();
    conn.receive_zone_packet_for_test(fly_appearance(r.me(), 1));
    CHECK(r.frame() == FlyMode::Flying);
    buff(conn, 0, 602);
    CHECK(r.frame() == FlyMode::Flying);
    buff(conn, 0, 602, true);
    CHECK(r.frame() == FlyMode::Flying);
    // A levitate buff landing replaces it, and its fading leaves the player on the ground.
    buff(conn, 1, 600);
    CHECK(r.frame() == FlyMode::Levitate);
    conn.receive_zone_packet_for_test(fly_appearance(r.me(), 1));
    CHECK(r.frame() == FlyMode::Flying);
    buff(conn, 1, 600, true);
    CHECK(r.frame() == FlyMode::Ground);
}

TEST_CASE("Fly mode: zoning in takes the spawn record, then the buffs") {
    FlyRig r;
    auto& conn = r.rig.conn;
    r.frame();
    conn.receive_zone_packet_for_test(fly_appearance(r.me(), 4));
    CHECK(r.frame() == FlyMode::Floating);
    // A reset (zoning) rebuilds from the buffs: no levitation, so the ground.
    r.m.reset({0, 0, 0}, 0);
    CHECK(r.frame() == FlyMode::Ground);
    buff(conn, 0, 600);
    r.m.reset({0, 0, 0}, 0);
    CHECK(r.frame() == FlyMode::Levitate);
}
