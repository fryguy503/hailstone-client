// DoPassageOfTime's timed blocks (game/passage_of_time.h): the 6 s buff tick (ProcessAffects with the client's
// buffs), the HP / endurance / mana regen after it, the 1 s endurance upkeep, and the connection running them.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "game/inventory.h"
#include "game/mechanics/character.h"
#include "game/mechanics/sheet_builder.h"
#include "game/mechanics/tables.h"
#include "game/passage_of_time.h"
#include "game/spells.h"
#include "game/world_state.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/spell_packets.h"
#include "net/zone_packets.h"

using namespace hailstone;
using namespace hailstone::game;
using namespace hailstone::game::mechanics;

namespace {

// Level 60 cleric (class 2) and warrior (class 1) rows: base HP, mana, endurance, HP regen 3, endurance regen 5.
constexpr const char* kBaseData = "60^1^400^0^300^3^6^0.9^0^1.25\n"
                                  "60^2^500^800^300^3^5^0.75^14^1.25\n";

struct Host final : PassageHost {
    std::vector<std::uint32_t> lines;
    std::vector<std::pair<int, bool>> removed;
    SpellState* spells = nullptr;
    int interrupts = 0;
    void chat(std::uint32_t eqstr, std::uint32_t) override { lines.push_back(eqstr); }
    void remove_buff(int slot, bool notify) override {
        removed.emplace_back(slot, notify);
        if (spells) spells->buffs[static_cast<std::size_t>(slot)] = {};
    }
    std::int32_t rand() override { return 0; }
    void stop_cast() override { ++interrupts; }
    void end_stun() override { lines.push_back(0x30c0); }
    void summon_mount() override {}
};

struct Fixture {
    GameTables tables;
    std::map<std::int32_t, SpellData> spells;
    SpellState state;
    net::PlayerProfile profile;
    Inventory inventory;
    WorldState world;
    TickPools pools;
    Host host;
    Fixture(std::uint8_t class_id = 2) {
        tables.class_levels.parse(kBaseData);
        profile.class_ = class_id;
        profile.level = 60;
        profile.race = 1;
        profile.profile_344c[3] = 4; // sitting mana regen
        profile.profile_344c[4] = 2; // standing mana regen
        profile.str = profile.sta = profile.cha = profile.dex = profile.int_ = profile.agi = profile.wis = 75;
        state.last_buff_tick_ms = 1000;
        world.player_id = 1;
        world.spawns[1].name = "Me";
        world.spawns[2].name = "a_rat01";
        world.spawns[2].is_npc = true;
        pools.hp = 500;
        pools.max_hp = 1000;
        pools.mana = 100;
        pools.endurance = 50;
        host.spells = &state;
    }
    SpellLookup lookup() {
        return [this](std::int32_t id) -> const SpellData* {
            auto it = spells.find(id);
            return it == spells.end() ? nullptr : &it->second;
        };
    }
    // A spell whose slots are blank (254) but the given {spa, base}; formula 100 (the base itself).
    SpellData& spell(std::int32_t id, std::initializer_list<std::array<std::int32_t, 2>> effects,
                     std::uint32_t duration_formula = 1) {
        SpellData s;
        s.id = static_cast<std::uint32_t>(id);
        s.effect.fill(254);
        s.formula.fill(100);
        s.duration_formula = duration_formula;
        s.spell_type = 1;
        s.class_level.fill(255);
        std::size_t k = 0;
        for (const auto& e : effects) {
            s.effect[k] = e[0];
            s.base[k] = e[1];
            ++k;
        }
        return spells[id] = s;
    }
    void buff(int slot, std::uint32_t spell, std::int32_t tics, std::uint32_t caster = 2) {
        BuffSlot& b = state.buffs[static_cast<std::size_t>(slot)];
        b.spell = spell;
        b.tics = tics;
        b.level = 60;
        b.caster_id = caster;
        b.received_ms = 0;
    }
    CharacterSheet sheet(std::int64_t now) {
        LiveState live;
        live.hp = pools.hp;
        live.max_hp = pools.max_hp;
        live.mana = static_cast<std::uint32_t>(pools.mana);
        live.endurance = static_cast<std::uint32_t>(pools.endurance);
        live.player_id = 1;
        live.now_ms = now;
        return build_sheet(profile, inventory, state, live, nullptr, &world);
    }
    bool tick(std::int64_t now) {
        CharacterSheet s = sheet(now);
        return six_second_tick(state, s, tables, lookup(), now, pools, host);
    }
    // The regen values the ported formulas give for the character as it is now.
    struct Regen {
        std::int32_t hp = 0, mana = 0, endurance = 0;
    };
    Regen regen(std::int64_t now) {
        CharacterSheet s = sheet(now);
        Character ch(s, tables, lookup());
        ch.process_all_stats();
        return {ch.hp_regen(true, nullptr, false), ch.mana_regen(true, false), ch.endurance_regen(true, false)};
    }
};

} // namespace

TEST_CASE("Buff tick: one 6 s timer per character; a duration already 0 is removed") {
    Fixture f;
    f.spell(100, {{0x7a, 1}});
    f.spell(101, {{0x7a, 1}});
    f.spell(102, {{0x7a, 1}});
    f.buff(0, 100, 3);
    f.state.buffs[0].received_ms = 4000; // a packet does not restart the character's timer
    f.buff(1, 101, -2);                 // permanent: not positive, not 0 or -1
    f.buff(2, 102, 0);                  // runs out at this tick
    CHECK_FALSE(f.tick(7000)); // 6000 ms is not more than 6000
    CHECK(f.state.buffs[0].tics == 3);
    CHECK(f.tick(7001));
    CHECK(f.state.buffs[0].tics == 2);
    CHECK(f.state.buffs[0].received_ms == 7001);
    CHECK(f.state.buffs[1].tics == -2);
    CHECK(f.state.buffs[2].empty()); // CharacterZoneClient_RemoveAffect(effect, 0, 0): no packet
    REQUIRE(f.host.removed.size() == 1);
    CHECK(f.host.removed[0] == std::pair<int, bool>{2, false});
    CHECK(f.state.last_buff_tick_ms == 7001);
    // A duration of -1 goes unless the spell's duration formula is 50 (permanent).
    f.buff(3, 101, -1);
    f.spell(103, {{0x7a, 1}}, 50);
    f.buff(4, 103, -1);
    CHECK(f.tick(13002));
    CHECK(f.state.buffs[3].empty());
    CHECK(f.state.buffs[4].tics == -1);
}

TEST_CASE("Buff tick: held countdowns (Spell_IsBuffCountdownHeld, SPA 0x1b4)") {
    Fixture f;
    SpellData& suspendable = f.spell(1, {{0x7a, 1}});
    suspendable.suspendable = true;
    f.spell(2, {{0x7a, 1}});
    f.spell(4, {{0x1b4, 1}});
    SpellData& suspendable_hold = f.spell(3, {{0x1b4, 1}});
    suspendable_hold.suspendable = true;
    CHECK(buff_countdown_held(&f.spells[1], true, false));
    CHECK_FALSE(buff_countdown_held(&f.spells[1], false, false));
    CHECK_FALSE(buff_countdown_held(&f.spells[2], true, false));
    CHECK_FALSE(buff_countdown_held(&f.spells[3], true, false)); // SPA 0x1b4 on the spell itself
    CHECK_FALSE(buff_countdown_held(nullptr, true, false));
    f.buff(0, 1, 10);
    f.buff(1, 2, 10);
    f.state.buff_timers_on_hold = true;
    REQUIRE(f.tick(7001));
    CHECK(f.state.buffs[0].tics == 10); // held
    CHECK(f.state.buffs[1].tics == 9);
    // A SPA 0x1b4 buff holds every countdown and puts the timers on hold.
    f.state.buff_timers_on_hold = false;
    f.buff(5, 4, 10);
    REQUIRE(f.tick(13002));
    CHECK(f.state.buffs[0].tics == 10);
    CHECK(f.state.buffs[1].tics == 9);
    CHECK(f.state.buffs[5].tics == 10);
    CHECK(f.state.buff_timers_on_hold);
    // Once it is gone the timers stay on hold: only the suspendable buff waits.
    f.state.buffs[5] = {};
    REQUIRE(f.tick(19003));
    CHECK(f.state.buffs[0].tics == 10);
    CHECK(f.state.buffs[1].tics == 8);
    // GetPCSpellAffect stops at the first blank slot: a SPA 0x1b4 after one holds nothing.
    SpellData& late = f.spell(5, {{0x7a, 1}});
    late.effect[3] = 0x1b4;
    f.state.buff_timers_on_hold = false;
    f.buff(6, 5, 10);
    REQUIRE(f.tick(25004));
    CHECK(f.state.buffs[1].tics == 7);
}

TEST_CASE("Buff tick: a zone that suspends buffs holds the suspendable ones (OP_NewZone +876, 0xddfc5c)") {
    Fixture f;
    SpellData& suspendable = f.spell(1, {{0x7a, 1}});
    suspendable.suspendable = true;
    f.spell(2, {{0x7a, 1}});
    SpellData& suspendable_hold = f.spell(3, {{0x1b4, 1}});
    suspendable_hold.suspendable = true;
    CHECK(buff_countdown_held(&f.spells[1], false, true));
    CHECK_FALSE(buff_countdown_held(&f.spells[2], false, true)); // not suspendable: EQEmu ticks it too
    CHECK(buff_countdown_held(&f.spells[3], false, true));       // SPA 0x1b4 excuses only the spawn's hold
    CHECK_FALSE(buff_countdown_held(nullptr, false, true));
    f.buff(0, 1, 2);
    f.buff(1, 2, 10);
    f.state.zone_suspends_buffs = true;
    REQUIRE(f.tick(7001));
    REQUIRE(f.tick(13002));
    REQUIRE(f.tick(19003));
    CHECK(f.state.buffs[0].tics == 2); // held, and not run out and removed here
    CHECK(f.state.buffs[1].tics == 7);
    // The time the displays show stays at the count as received while it is held.
    CHECK(buff_ms_left(f.state.buffs[0], &f.spells[1], false, true, 19003 + 5000) == 2 * kBuffTickMs);
    CHECK(buff_ms_left(f.state.buffs[1], &f.spells[2], false, true, 19003 + 5000) == 7 * kBuffTickMs - 5000);
    // Out of such a zone the count runs again.
    f.state.zone_suspends_buffs = false;
    REQUIRE(f.tick(25004));
    CHECK(f.state.buffs[0].tics == 1);
}

TEST_CASE("Buff tick: the buffs' HP, mana and endurance, then the regen of the ported formulas") {
    Fixture f; // a cleric
    f.spell(10, {{0, -20}});    // a DoT from an NPC
    f.spell(11, {{15, 10}});    // mana per tick
    f.spell(12, {{0xbd, 5}});   // endurance per tick
    f.buff(0, 10, 5);
    f.buff(1, 11, 5);
    f.buff(2, 12, 5);
    // The regen as the formulas give it, on the character after the buff tick (the HP regen does not depend on
    // the pools here).
    const auto r = f.regen(7001);
    REQUIRE(r.hp > 0);
    REQUIRE(r.mana > 0);
    REQUIRE(r.endurance > 0);
    REQUIRE(f.tick(7001));
    CHECK(f.pools.hp == 500 - 20 + r.hp);
    CHECK(f.pools.mana == 100 + 10 + r.mana);
    CHECK(f.pools.endurance == 50 + 5 + r.endurance);
    CHECK(f.host.lines.empty());

    // A warrior gets no mana from the buff (classes 1, 7, 8, 9, 16), only its regen (0 for a warrior).
    Fixture w(1);
    w.spells = f.spells;
    w.buff(1, 11, 5);
    w.pools.mana = 0;
    REQUIRE(w.tick(7001));
    CHECK(w.pools.mana == 0);
}

TEST_CASE("Buff tick: an HP loss breaks the first mesmerize buff, told to the server") {
    Fixture f;
    f.spell(10, {{0, -20}});
    f.spell(20, {{0x1f, 1}}); // mesmerize
    f.buff(0, 10, 5);
    f.buff(3, 20, 5);
    REQUIRE(f.tick(7001));
    CHECK(f.state.buffs[3].empty());
    REQUIRE(f.host.removed.size() == 1);
    CHECK(f.host.removed[0] == std::pair<int, bool>{3, true}); // RemoveAffect(effect, 1, 0): OP_Buff
    CHECK(f.host.lines == std::vector<std::uint32_t>{0x30c0}); // EndStun
}

TEST_CASE("Buff tick: a pool at its maximum gets no mana regen; dead, the HP regen waits but the pools do not") {
    Fixture f;
    // The cleric's maximum mana from the formulas.
    std::int32_t max_mana = 0;
    {
        CharacterSheet s = f.sheet(7001);
        Character ch(s, f.tables, f.lookup());
        ch.process_all_stats();
        max_mana = ch.max_mana(true);
    }
    REQUIRE(max_mana > 0);
    f.pools.mana = max_mana + 50; // the stored value is clamped (Cur_Mana(1)) and not raised
    REQUIRE(f.tick(7001));
    CHECK(f.pools.mana == max_mana);

    Fixture d;
    d.pools.dead = true;
    const auto r = d.regen(7001);
    REQUIRE(d.tick(7001));
    CHECK(d.pools.hp == 500);
    CHECK(d.pools.endurance == 50 + r.endurance);
    CHECK(d.pools.mana == 100 + r.mana);
}

TEST_CASE("Buff tick: an unconscious player at -3 HP is told it bleeds to death") {
    Fixture f;
    f.pools.hp = -3;
    CharacterSheet s = f.sheet(7001);
    // CharacterBase::Status 3 with no regen left (starving): GetHPRegen gives -1 and sets its flag.
    s.owner_status = 3;
    s.owner_food = s.food = 0;
    REQUIRE(six_second_tick(f.state, s, f.tables, f.lookup(), 7001, f.pools, f.host));
    CHECK(f.host.lines == std::vector<std::uint32_t>{kStrBleedingToDeath});
    CHECK(f.pools.hp == -4);
}

TEST_CASE("The 1 s block: the endurance upkeep, and the discipline goes when the endurance runs out") {
    Fixture f(1);
    f.spell(30, {{0x7a, 1}}).endurance_upkeep = 20;
    f.buff(kBuffSlots - 1, 30, 10, 1); // the discipline slot
    auto run = [&](std::int64_t now) {
        CharacterSheet s = f.sheet(now);
        return one_second_tick(f.state, s, f.tables, f.lookup(), now, f.pools, f.host);
    };
    CHECK(one_second_tick_due(f.state, 5000)); // 0: set at the first frame
    CHECK_FALSE(run(5000));
    CHECK(f.state.last_upkeep_tick_ms == 5000);
    CHECK_FALSE(run(6000));
    CHECK(run(6001));
    CHECK(f.pools.endurance == 30);
    CHECK(run(7002));
    CHECK(f.pools.endurance == 10);
    CHECK(f.host.removed.empty());
    CHECK(run(8003)); // 10 - 20 < 0: 0, RemovePCAffectex on the discipline (notify 1)
    CHECK(f.pools.endurance == 0);
    REQUIRE(f.host.removed.size() == 1);
    CHECK(f.host.removed[0] == std::pair<int, bool>{kBuffSlots - 1, true});
}

TEST_CASE("The connection runs the ticks: the buffs and regen move the pools between server updates") {
    test::Rig rig;
    auto& conn = rig.conn;
    // The game folder's BaseData.txt (a cleric's row at every level): without the character's row the ticks wait.
    {
        std::filesystem::create_directories(rig.dir.path() / "Resources");
        std::ofstream base(rig.dir.path() / "Resources" / "BaseData.txt");
        for (int level = 1; level <= 100; ++level) base << level << "^2^500^800^300^3^5^0.75^14^1.25\n";
    }
    rig.game.rescan(); // the folder's index was made with the rig
    REQUIRE(rig.to_zone());
    net::PlayerProfile pp = *conn.profile();
    pp.class_ = 2;
    pp.level = 60;
    conn.set_profile_for_test(pp);
    auto table = std::make_shared<SpellTable>();
    auto line = [](int id, int spa, int base) {
        std::vector<std::string> fl(237, "0");
        fl[0] = std::to_string(id);
        fl[1] = "Spell" + std::to_string(id);
        for (int k = 0; k < 12; ++k) {
            fl[86 + static_cast<std::size_t>(k)] = "254";
            fl[70 + static_cast<std::size_t>(k)] = "100";
        }
        fl[86] = std::to_string(spa);
        fl[20] = std::to_string(base);
        fl[16] = "1";
        fl[83] = "1";
        std::string out;
        for (std::size_t i = 0; i < fl.size(); ++i) out += (i ? "^" : "") + fl[i];
        return out + "\n";
    };
    REQUIRE(table->parse(line(11, 15, 10) + line(12, 0xbd, 5)) == 2);
    conn.set_spell_table_for_test(table);
    const std::uint32_t me = conn.world().player_id;
    REQUIRE(me != 0);
    // The server's pools.
    conn.receive_zone_packet_for_test(net::encode(net::HpUpdate{static_cast<std::int16_t>(me), 500, 1000}));
    conn.receive_zone_packet_for_test(net::encode_points_update(false, {100, 5000, static_cast<std::uint16_t>(me)}));
    conn.receive_zone_packet_for_test(net::encode_points_update(true, {50, 5000, static_cast<std::uint16_t>(me)}));
    // A mana buff and an endurance buff (OP_Buff).
    for (auto [slot, spell] : {std::pair{0u, 11u}, std::pair{1u, 12u}}) {
        net::BuffPacket bp;
        bp.entity_id = me;
        bp.slot = slot;
        bp.buff.effect_type = 2;
        bp.buff.level = 60;
        bp.buff.spell_id = spell;
        bp.buff.duration = 10;
        bp.buff.caster_id = me;
        conn.receive_zone_packet_for_test(net::encode(bp));
    }
    REQUIRE(conn.spell_state().buffs[0].spell == 11);
    const auto before = conn.player_status();
    const std::int64_t now = net::GameConnection::now_ms() + 7000;
    conn.passage_of_time_for_test(now);
    const auto& after = conn.player_status();
    CHECK(after.mana > before.mana + 10 - 1);           // the buff's 10 and the regen
    CHECK(after.endurance > before.endurance + 5 - 1);  // the buff's 5 and the regen
    CHECK(after.hp > before.hp);                        // the HP regen below the maximum
    CHECK(conn.spell_state().buffs[0].tics == 9);
    // The server's next update still wins.
    conn.receive_zone_packet_for_test(net::encode_points_update(false, {100, 5000, static_cast<std::uint16_t>(me)}));
    CHECK(conn.player_status().mana == 100);
}

TEST_CASE("Mana ticks use the local maximum until the server reports its own") {
    auto config = test::MockConfig::standard();
    config.characters.front().class_ = 2;
    config.characters.front().level = 60;
    test::Rig rig(std::move(config));
    {
        std::filesystem::create_directories(rig.dir.path() / "Resources");
        std::ofstream(rig.dir.path() / "Resources" / "BaseData.txt") << kBaseData;
    }
    rig.game.rescan();
    auto& conn = rig.conn;
    conn.set_game_steps(std::vector<std::int64_t>{});
    REQUIRE(rig.to_zone());
    auto profile = *conn.profile();
    profile.profile_344c[3] = profile.profile_344c[4] = 5;
    conn.set_profile_for_test(profile);
    conn.set_spell_table_for_test(std::make_shared<SpellTable>());

    GameTables tables;
    tables.class_levels.parse(kBaseData);
    const auto sheet = build_sheet(profile, conn.inventory(), conn.spell_state(), conn.live_state());
    Character character(sheet, tables, spell_lookup(conn.spell_table()));
    character.process_all_stats();
    const auto local_max = static_cast<std::uint32_t>(character.max_mana());
    REQUIRE(local_max > 2);
    REQUIRE(local_max < 4860);
    auto tick = [&] {
        const auto now = conn.spell_state().last_buff_tick_ms + 6001;
        conn.set_game_steps(std::vector<std::int64_t>{now});
        rig.step();
        REQUIRE(conn.spell_state().last_buff_tick_ms == now);
        conn.set_game_steps(std::vector<std::int64_t>{});
    };
    auto update_mana = [&](std::uint32_t current, std::uint32_t maximum) {
        const auto me = static_cast<std::uint16_t>(conn.world().player_id);
        rig.server.zone.send_reliable(net::encode_points_update(false, {current, maximum, me}));
        REQUIRE(rig.until([&] {
            const auto& status = conn.player_status();
            return status.mana == current && status.max_mana == maximum;
        }));
    };

    // ManaChange carries the current pool only, so the first tick uses the local formula.
    conn.receive_zone_packet_for_test(net::encode(net::ManaChange{local_max + 1, 777, 0, false, 0}));
    REQUIRE(conn.player_status().max_mana == 0);
    tick();
    CHECK(conn.player_status().mana == local_max);

    // The report's unequip and re-equip maxima survive the client tick without another server packet.
    update_mana(4860, 4860);
    tick();
    CHECK(conn.player_status().mana == 4860);
    update_mana(4907, 4907);
    tick();
    CHECK(conn.player_status().mana == 4907);
    tick();
    CHECK(conn.player_status().mana == 4907);

    // The latest maximum also caps regeneration when it is lower than the formula's result.
    update_mana(local_max - 2, local_max - 1);
    tick();
    CHECK(conn.player_status().mana == local_max - 1);

    // Zero is also an explicit maximum when it comes from the server.
    update_mana(0, 0);
    tick();
    CHECK(conn.player_status().mana == 0);

    // A new zone uses the local formula until its server reports a maximum for the new spawn.
    const int entries = rig.server.zone_entries;
    rig.server.request_zone_change(1, 20.0f, 10.0f, 3.0f);
    REQUIRE(rig.until([&] { return rig.server.zone_entries == entries + 1 && conn.in_zone(); }));
    conn.receive_zone_packet_for_test(net::encode(net::ManaChange{local_max + 1, 777, 0, false, 0}));
    tick();
    CHECK(conn.player_status().mana == local_max);
}

TEST_CASE("The connection's endurance tick follows actual running, walking and stopping") {
    test::Rig rig;
    {
        std::filesystem::create_directories(rig.dir.path() / "Resources");
        std::ofstream(rig.dir.path() / "Resources" / "BaseData.txt") << kBaseData;
    }
    rig.game.rescan();
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    net::PlayerProfile pp = *conn.profile();
    pp.class_ = 2;
    pp.level = 60;
    pp.str = pp.sta = pp.cha = pp.dex = pp.int_ = pp.agi = pp.wis = 75;
    conn.set_profile_for_test(pp);
    conn.set_spell_table_for_test(std::make_shared<SpellTable>());
    conn.player_status_mut().level = 60;
    conn.player_status_mut().endurance = 50;
    std::int64_t now = net::GameConnection::now_ms() + 7000;
    auto tick = [&] {
        conn.passage_of_time_for_test(now);
        now += 6001;
        return conn.player_status().endurance;
    };

    conn.set_running(true);
    conn.set_speed_run(0.0f);
    CHECK_FALSE(conn.live_state().running);
    CHECK(tick() == 57); // Base 5, plus half while not attacking; standing in run mode is not running.

    conn.set_speed_run(0.7f);
    CHECK(conn.live_state().running);
    CHECK(tick() == 60); // Running costs level / 15 = 4 per tick.

    conn.set_running(false);
    CHECK_FALSE(conn.live_state().running);
    CHECK(tick() == 67); // The same nonzero SpeedRun in walk mode has no running penalty.

    conn.set_running(true);
    conn.set_speed_run(-0.7f);
    CHECK(conn.live_state().running);
    CHECK(tick() == 70); // Backward movement also has a nonzero SpeedRun.

    conn.set_speed_run(0.0f);
    CHECK_FALSE(conn.live_state().running);
    CHECK(tick() == 77);

    conn.world_for_test().spawns.at(conn.world().player_id).mount_id = 987;
    conn.set_speed_run(0.7f); // The mount moves; the rider's own SpeedRun stays zero.
    CHECK_FALSE(conn.live_state().running);
    CHECK(tick() == 84);
}

TEST_CASE("Drunkenness decays once per buff tick and compares the new value with raw tolerance") {
    CHECK(drunkenness_tick(0, 0, true).value == 0);
    CHECK_FALSE(drunkenness_tick(0, 0, true).changed);
    CHECK(drunkenness_tick(1, 0, true).value == 0);
    CHECK_FALSE(drunkenness_tick(50, 0, true).remove_mount); // 49 is below tolerance 0's threshold50.
    CHECK(drunkenness_tick(51, 0, true).remove_mount);
    CHECK_FALSE(drunkenness_tick(51, 0, false).remove_mount);
    CHECK_FALSE(drunkenness_tick(101, 102, true).remove_mount); // threshold101, value100.
    CHECK(drunkenness_tick(151, 250, true).remove_mount); // threshold caps at150.
    // The widening is an offset on the player's own FOV, 0 below 70 (a reset to 0 restores the chosen FOV).
    CHECK(drunkenness_fov_offset(0) == 0);
    CHECK(drunkenness_fov_offset(69) == 0);
    CHECK(drunkenness_fov_offset(70) == 0);
    CHECK(drunkenness_fov_offset(120) == 50);

    // The timed update runs even when BaseData is missing; a skipped regen must not accelerate sobering.
    mq_test::World w("hailstone_drunkenness_tick_test");
    auto& c = *w.conn;
    net::GameConnectionTestAccess::drunkenness(c) = 80;
    const auto start = net::GameConnection::now_ms() + 7000;
    c.passage_of_time_for_test(start);
    CHECK(c.drunkenness() == 79);
    CHECK(c.live_state().drunkenness == 79);
    const auto serial = c.drunkenness_updates();
    c.passage_of_time_for_test(start + 6000);
    CHECK(c.drunkenness() == 79);
    CHECK(c.drunkenness_updates() == serial);
    c.passage_of_time_for_test(start + 6001);
    CHECK(c.drunkenness() == 78);
    CHECK(c.drunkenness_updates() == serial + 1);

    // Removing the local summon buff before ProcessAffects prevents it from remounting in the same tick.
    auto mount_spells = std::make_shared<SpellTable>();
    REQUIRE(mount_spells->parse(mq_test::spell_line({{0, "1130"}, {1, "Fixture Mount"}, {86, "113"}, {17, "10"}})) == 1);
    c.set_spell_table_for_test(mount_spells);
    auto& world = net::GameConnectionTestAccess::world(c);
    auto& mount = world.spawns[99];
    mount.id = 99;
    mount.race = 216;
    mount.rider_id = world.player_id;
    world.spawns.at(world.player_id).mount_id = 99;
    world.controlled_id = 99;
    auto& summon = net::GameConnectionTestAccess::spells(c).buffs[1];
    summon.spell = 1130;
    summon.tics = 10;
    net::GameConnectionTestAccess::drunkenness(c) = 51;
    c.passage_of_time_for_test(start + 12002);
    CHECK(c.drunkenness() == 50);
    CHECK(summon.empty());
    CHECK(world.spawns.at(world.player_id).mount_id == 0);
    CHECK(world.controlled_id == 0);
}

TEST_CASE("The live buff adapter interrupts only an interruptible cast") {
    Fixture f;
    f.spell(11, {{343, 100}});
    f.spell(12, {});
    f.buff(0, 11, 10);
    f.state.casting.spell = 12;
    REQUIRE(f.tick(7001));
    CHECK(f.host.interrupts == 1);
    f.spells.at(12).uninterruptable = true;
    REQUIRE(f.tick(13002));
    CHECK(f.host.interrupts == 1);
    f.state.casting = {};
    REQUIRE(f.tick(19003));
    CHECK(f.host.interrupts == 1);
}

TEST_CASE("The live unconscious status feeds bleeding regen and recovery waits for the six-second tick") {
    test::Rig rig;
    {
        std::filesystem::create_directories(rig.dir.path() / "Resources");
        std::ofstream(rig.dir.path() / "Resources" / "BaseData.txt") << kBaseData;
    }
    rig.game.rescan();
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    net::PlayerProfile pp = *c.profile();
    pp.class_ = 2;
    pp.level = 60;
    c.set_profile_for_test(pp);
    c.set_spell_table_for_test(std::make_shared<SpellTable>());
    c.player_status_mut().level = 60;
    c.player_status_mut().hp = -6;
    c.update_player_consciousness(true);
    REQUIRE(c.player_status().owner_status == 3);
    const auto now = net::GameConnection::now_ms() + 7000;
    c.passage_of_time_for_test(now);
    CHECK(c.player_status().hp == -7);
    CHECK(c.player_status().owner_status == 3);
    c.player_status_mut().hp = 1; // A server heal is immediately readable, but the status is still unconscious.
    c.passage_of_time_for_test(now + 6000);
    CHECK(c.player_status().owner_status == 3);
    c.passage_of_time_for_test(now + 6001);
    CHECK(c.player_status().owner_status == 0);
    CHECK(c.player_status().stand_state == net::kStandStanding);
}
