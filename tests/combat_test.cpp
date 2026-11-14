// Combat packets (byte-exact against the server's struct layouts) and the
// client-side combat rules in game/combat.h.

#include <doctest/doctest.h>

#include <cstring>

#include <glm/gtc/constants.hpp>
#include <string>
#include <vector>

#include "core/byte_io.h"
#include "game/combat.h"
#include "game/eq_strings.h"
#include "game/world_state.h"
#include "net/opcodes.h"
#include "net/packet_io.h"
#include "net/zone_packets.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

std::vector<std::byte> bytes_of(ByteWriter&& w) { return w.take(); }

AppPacket packet(Op op, ByteWriter&& w) { return AppPacket(to_wire(op), w.take()); }

AppPacket respawn_choices(std::uint8_t validity = 1, bool duplicate_id = false) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(300000);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(2);
    const char* names[] = {"Bind Location", "Resurrect"};
    for (std::uint32_t i = 0; i < 2; ++i) {
        w.le<std::uint32_t>(duplicate_id ? 0 : i);
        w.le<std::uint32_t>(1);
        w.f32(1.0f);
        w.f32(2.0f);
        w.f32(3.0f);
        w.f32(4.0f);
        w.bytes(std::as_bytes(std::span(names[i], std::strlen(names[i]) + 1)));
        w.le<std::uint8_t>(i == 1 ? validity : 0);
    }
    return packet(Op::RespawnWindow, std::move(w));
}

} // namespace

TEST_CASE("OP_TargetMouse / OP_TargetCommand are ClientTarget_Struct (u32)") {
    AppPacket m = encode_target(false, 0x1234);
    CHECK(m.opcode == 0x075d);
    ByteWriter w;
    w.le<std::uint32_t>(0x1234);
    CHECK(m.data == bytes_of(std::move(w)));
    AppPacket c = encode_target(true, 7);
    CHECK(c.opcode == 0x58e2);
    CHECK(decode_target(c) == 7u);
    CHECK_FALSE(decode_target(AppPacket(0x58e2, std::vector<std::byte>(3))).has_value());
}

TEST_CASE("OP_AutoAttack and OP_AutoAttack2 carry a u32 (server reads byte 0)") {
    AppPacket a = encode_auto_attack(false, true);
    CHECK(a.opcode == 0x109d);
    REQUIRE(a.data.size() == 4);
    CHECK(std::to_integer<int>(a.data[0]) == 1);
    AppPacket b = encode_auto_attack(true, false);
    CHECK(b.opcode == 0x3526);
    CHECK(b.data == std::vector<std::byte>(4));
    CHECK(decode_auto_attack(a) == true);
}

TEST_CASE("OP_Consider is rof2_structs Consider_Struct, 20 bytes") {
    Consider c{10, 20, 5, 15, 1};
    AppPacket p = encode(c);
    CHECK(p.opcode == 0x742b);
    ByteWriter w;
    w.le<std::uint32_t>(10); // playerid
    w.le<std::uint32_t>(20); // targetid
    w.le<std::uint32_t>(5);  // faction
    w.le<std::uint32_t>(15); // level
    w.le<std::uint8_t>(1);   // pvpcon
    w.zeros(3);
    CHECK(p.data == bytes_of(std::move(w)));
    auto d = decode_consider(p);
    REQUIRE(d);
    CHECK(d->target_id == 20);
    CHECK(d->faction == 5);
    CHECK(d->level == 15);
    CHECK(d->pvp_con == 1);
}

TEST_CASE("OP_Damage is rof2_structs CombatDamage_Struct, 30 bytes, packed") {
    ByteWriter w; // as ENCODE(OP_Damage) lays it out
    w.le<std::uint16_t>(0x0102); // /* 00 */ target
    w.le<std::uint16_t>(0x0304); // /* 02 */ source
    w.le<std::uint8_t>(36);      // /* 04 */ type
    w.le<std::uint32_t>(0);      // /* 05 */ spellid
    w.le<std::int32_t>(-4);      // /* 09 */ damage
    w.f32(0.5f);                 // /* 13 */ force
    w.f32(128.0f);               // /* 17 */ hit_heading
    w.f32(0.0f);                 // /* 21 */ hit_pitch
    w.le<std::uint8_t>(0);       // /* 25 */ secondary
    w.le<std::uint32_t>(2);      // /* 26 */ special
    AppPacket p = packet(Op::Damage, std::move(w));
    REQUIRE(p.data.size() == kCombatDamageSize);
    auto d = decode_combat_damage(p);
    REQUIRE(d);
    CHECK(d->target == 0x0102);
    CHECK(d->source == 0x0304);
    CHECK(d->type == 36);
    CHECK(d->damage == -4);
    CHECK(d->force == doctest::Approx(0.5f));
    CHECK(d->hit_heading == doctest::Approx(128.0f));
    CHECK(d->special == 2);
    CHECK(encode(*d).data == p.data);
    CHECK_FALSE(decode_combat_damage(AppPacket(p.opcode, std::vector<std::byte>(29))).has_value());
}

TEST_CASE("OP_Death passes the server's Death_Struct through (bind zone at +12, skill at +20)") {
    ByteWriter w;
    w.le<std::uint32_t>(55);         // spawn_id
    w.le<std::uint32_t>(7);          // killer_id
    w.le<std::uint32_t>(55);         // corpseid
    w.le<std::uint32_t>(2);          // bindzoneid
    w.le<std::uint32_t>(0xffffffff); // spell_id
    w.le<std::uint32_t>(4);          // attack_skill (damage type)
    w.le<std::uint32_t>(9);          // damage
    w.le<std::uint32_t>(0);          // unknown028
    AppPacket p = packet(Op::Death, std::move(w));
    REQUIRE(p.data.size() == kDeathSize);
    auto d = decode_death(p);
    REQUIRE(d);
    CHECK(d->spawn_id == 55);
    CHECK(d->killer_id == 7);
    CHECK(d->bind_zone_id == 2);
    CHECK(d->spell_id == 0xffffffff);
    CHECK(d->attack_skill == 4);
    CHECK(d->damage == 9);
    CHECK(encode(*d).data == p.data);
}

TEST_CASE("OP_ManaChange, OP_ManaUpdate, OP_EnduranceUpdate, OP_ExpUpdate, OP_LevelUpdate") {
    ByteWriter mc;
    mc.le<std::uint32_t>(40); // new_mana
    mc.le<std::uint32_t>(60); // stamina
    mc.le<std::uint32_t>(0);  // spell_id
    mc.le<std::uint8_t>(0);   // keepcasting
    mc.zeros(3);
    mc.le<std::int32_t>(-1); // slot
    AppPacket m = packet(Op::ManaChange, std::move(mc));
    REQUIRE(m.data.size() == 20);
    auto md = decode_mana_change(m);
    REQUIRE(md);
    CHECK(md->mana == 40);
    CHECK(md->endurance == 60);
    CHECK(md->slot == -1);
    CHECK(encode(*md).data == m.data);

    ByteWriter eu;
    eu.le<std::uint32_t>(90);  // cur_end
    eu.le<std::uint32_t>(100); // max_end
    eu.le<std::uint16_t>(321); // spawn_id
    AppPacket e = packet(Op::EnduranceUpdate, std::move(eu));
    auto ed = decode_points_update(e);
    REQUIRE(ed);
    CHECK(ed->current == 90);
    CHECK(ed->maximum == 100);
    CHECK(ed->spawn_id == 321);
    CHECK(encode_points_update(true, *ed).data == e.data);
    CHECK(encode_points_update(true, *ed).opcode == 0x5f42);
    CHECK(encode_points_update(false, *ed).opcode == 0x3791);

    AppPacket x = encode(ExpUpdate{165, 3});
    CHECK(x.opcode == 0x20ed);
    REQUIRE(x.data.size() == 8);
    CHECK(decode_exp_update(x)->exp == 165);

    AppPacket l = encode(LevelUpdate{2, 1, 1000});
    REQUIRE(l.data.size() == 12);
    auto ld = decode_level_update(l);
    REQUIRE(ld);
    CHECK(ld->level == 2);
    CHECK(ld->old_level == 1);
    CHECK(ld->exp == 1000);
}

TEST_CASE("OP_Animation is rof2_structs Animation_Struct: id, action, speed") {
    ByteWriter w;
    w.le<std::uint16_t>(300); // spawnid
    w.le<std::uint8_t>(5);    // action
    w.le<std::uint8_t>(10);   // speed
    AppPacket p = packet(Op::Animation, std::move(w));
    auto a = decode_animation(p);
    REQUIRE(a);
    CHECK(a->spawn_id == 300);
    CHECK(a->action == 5);
    CHECK(a->speed == 10);
    CHECK(encode(*a).data == p.data);
    CHECK(std::string(game::animation_code(5)) == "C05");
    CHECK(std::string(game::animation_code(12)) == "D01");
    CHECK(std::string(game::animation_code(32)) == "P01");
    CHECK(game::animation_code(0) == nullptr);
    CHECK(game::animation_code(200) == nullptr);
}

TEST_CASE("OP_ZonePlayerToBind follows ENCODE(OP_ZonePlayerToBind)") {
    ByteWriter w;
    w.le<std::uint16_t>(0); // bind zone (0 = this zone)
    w.le<std::uint16_t>(0); // instance
    w.f32(10.0f);
    w.f32(-20.0f);
    w.f32(3.0f);
    w.f32(128.0f);
    w.bytes(std::as_bytes(std::span("qeynos", 7)));
    w.le<std::uint8_t>(1);
    w.zeros(12);
    AppPacket p = packet(Op::ZonePlayerToBind, std::move(w));
    CHECK(p.data.size() == 34 + 6);
    auto b = decode_zone_player_to_bind(p);
    REQUIRE(b);
    CHECK(b->zone_id == 0);
    CHECK(b->x == 10.0f);
    CHECK(b->y == -20.0f);
    CHECK(b->heading == 128.0f);
    CHECK(b->zone_name == "qeynos");
    CHECK(encode(*b).data == p.data);
}

TEST_CASE("OP_RespawnWindow decodes SendRespawnBinds with its zero padding; the answer is a u32") {
    auto p = respawn_choices();
    p.data.insert(p.data.end(), 3, std::byte{0}); // count + 1 bytes EQEmu allocates but does not write
    auto r = decode_respawn_window(p);
    REQUIRE(r);
    CHECK(r->time_remaining_ms == 300000);
    REQUIRE(r->options.size() == 2);
    CHECK(r->options[0].name == "Bind Location");
    CHECK(r->options[1].name == "Resurrect");
    CHECK(r->options[0].available());
    CHECK(r->options[1].validity == 1);
    CHECK_FALSE(r->options[1].available());
    AppPacket a = encode_respawn_selection(0);
    CHECK(a.opcode == 0x0ecb);
    CHECK(a.data == std::vector<std::byte>(4));
}

TEST_CASE("OP_RespawnWindow availability is independent of the destination's name") {
    for (std::uint8_t validity : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}}) {
        auto r = decode_respawn_window(respawn_choices(validity));
        REQUIRE(r);
        REQUIRE(r->options.size() == 2);
        CHECK(r->options[1].name == "Resurrect");
        CHECK(r->options[1].validity == validity);
        CHECK(r->options[1].available() == (validity == 0));
    }
}

TEST_CASE("OP_RespawnWindow rejects truncated options and ambiguous option ids") {
    auto p = respawn_choices();
    for (std::size_t size = 0; size < p.data.size(); ++size) {
        auto truncated = p;
        truncated.data.resize(size);
        CHECK_FALSE(decode_respawn_window(truncated).has_value());
    }
    CHECK_FALSE(decode_respawn_window(respawn_choices(0, true)).has_value());
}

TEST_CASE("sitting is OP_SpawnAppearance animation 110, standing 100") {
    SpawnAppearance a;
    a.spawn_id = 42;
    a.type = kAppearanceAnimation;
    a.parameter = kStandSitting;
    AppPacket p = encode(a);
    ByteWriter w;
    w.le<std::uint16_t>(42);
    w.le<std::uint16_t>(14);
    w.le<std::uint32_t>(110);
    CHECK(p.data == bytes_of(std::move(w)));
}

// ---------------------------------------------------------------- rules

TEST_CASE("standard con colours (0x7be930's table)") {
    using game::Con;
    using game::standard_con;
    CHECK(standard_con(1, 1) == Con::White);
    CHECK(standard_con(2, 1) == Con::Yellow);
    CHECK(standard_con(4, 1) == Con::Yellow);
    CHECK(standard_con(5, 1) == Con::Red);
    CHECK(standard_con(9, 10) == Con::DarkBlue);
    CHECK(standard_con(5, 10) == Con::DarkBlue);  // >= mine - 5
    CHECK(standard_con(4, 10) == Con::Gray);      // light = min(7, 5) = 5, green = min(6, 5) = 5
    CHECK(standard_con(20, 30) == Con::Green);    // light min(22, 25)=22, green min(20,22)=20
    CHECK(standard_con(22, 30) == Con::LightBlue);
    CHECK(standard_con(19, 30) == Con::Gray);
    CHECK(standard_con(50, 70) == Con::Green);    // above 60: light 55, green 50
    CHECK(standard_con(49, 70) == Con::Gray);
    CHECK(standard_con(106, 1) == Con::Red);
    CHECK(standard_con(1, 106) == Con::Gray);
}

TEST_CASE("consider, melee and death messages follow the original's templates") {
    game::EqStrings s;
    s.set(12209, "he");
    s.set(12210, "she");
    s.set(12211, "it");
    s.set(12212, "regards you as an ally");
    s.set(12217, "scowls at you, ready to attack");
    s.set(12216, "regards you indifferently");
    s.set(12229, "looks like an even fight.");
    s.set(12239, "%1 %2 -- %3");
    s.set(12160, "for %1 points of damage.");
    s.set(12161, "for %1 point of damage.");
    s.set(12162, "%1 %2 %3 %4");
    s.set(12158, "%1 try to %2 %3, but %4!");
    s.set(12159, "%1 tries to %2 %3, but %4!");
    s.set(12156, "miss");
    s.set(12157, "misses");
    s.set(12150, "%1 dodge");
    s.set(12151, "%1 dodges");
    s.set(12185, "You");
    s.set(12187, "YOU");
    s.set(12181, "punch");
    s.set(12182, "punches");
    s.set(12183, "hit");
    s.set(12184, "hits");
    s.set(12164, "bite");
    s.set(12166, "bites");
    s.set(12107, "You have been slain by %1!");
    s.set(12113, "You have slain %1!");
    s.set(12114, "%1 has been slain by %2!");

    game::WorldState w;
    w.player_id = 1;
    game::Spawn me;
    me.id = 1;
    me.name = "Nmsctester";
    me.race = 1;
    me.level = 1;
    w.spawns[1] = me;
    game::Spawn rat;
    rat.id = 2;
    rat.name = "a_rat01";
    rat.race = 34;
    rat.level = 1;
    rat.is_npc = true;
    rat.gender = 2;
    w.spawns[2] = rat;

    CHECK(game::consider_message(s, w.spawns[2], 1, 6, 0, 0) == "a rat scowls at you, ready to attack -- looks like an even fight.");
    CHECK(game::con_level(w.spawns[2], 1, 0) == game::Con::White);
    CHECK(game::con_chat_color(game::Con::White) == 10);

    // Warriors without a weapon hit with hand to hand: damage type 4 -> by race.
    CHECK(game::damage_message(s, w, 2, 1, 4, 3, 0) == "You punch a rat for 3 points of damage.");
    CHECK(game::damage_message(s, w, 2, 1, 4, 1, 0) == "You punch a rat for 1 point of damage.");
    CHECK(game::damage_message(s, w, 1, 2, 4, 2, 0) == "A rat bites YOU for 2 points of damage.");
    CHECK(game::damage_message(s, w, 2, 1, 4, 0, 0) == "You try to punch a rat, but miss!");
    CHECK(game::damage_message(s, w, 1, 2, 4, 0, 0) == "A rat tries to bite YOU, but misses!");
    CHECK(game::damage_message(s, w, 1, 2, 4, -4, 0) == "A rat tries to bite YOU, but YOU dodge!");
    CHECK(game::damage_message(s, w, 2, 1, 4, -4, 0) == "You try to punch a rat, but a rat dodges!");
    CHECK_FALSE(game::damage_message(s, w, 2, 1, 231, 10, 0).has_value()); // spell
    CHECK_FALSE(game::damage_message(s, w, 2, 99, 4, 10, 0).has_value());  // unknown attacker

    // ReportHit_*: modes 0 (full), 1 (short) and 2 (number only) per category.
    s.set(2130, "%1 %2 for %3");
    s.set(2108, "%1 %2 for %3");
    s.set(2128, "%1 %2 %3 for %4");
    s.set(15039, "missed %1");
    s.set(15040, "%1 missed");
    s.set(15041, "%1 missed %2");
    auto yours = game::damage_report(s, w, 2, 1, 4, 3, 0);
    REQUIRE(yours);
    CHECK(yours->kind == game::HitReport::YourHits);
    CHECK(game::report_hit_line(s, *yours, 0) == "You punch a rat for 3 points of damage.");
    CHECK(game::report_hit_line(s, *yours, 1) == "punch a rat for 3");
    CHECK(game::report_hit_line(s, *yours, 2) == "3");
    CHECK_FALSE(game::report_hit_line(s, *yours, 3));
    auto on_me = game::damage_report(s, w, 1, 2, 4, 2, 0);
    CHECK(on_me->kind == game::HitReport::YouBeingHit);
    CHECK(game::report_hit_line(s, *on_me, 1) == "a rat bites for 2");
    auto missed = game::damage_report(s, w, 1, 2, 4, 0, 0);
    CHECK(missed->kind == game::HitReport::YouBeingMissed);
    CHECK(game::report_hit_line(s, *missed, 1) == "a rat missed");
    CHECK(game::report_hit_line(s, *missed, 2) == "miss");
    auto my_miss = game::damage_report(s, w, 2, 1, 4, 0, 0);
    CHECK(my_miss->kind == game::HitReport::YourMisses);
    CHECK(game::report_hit_line(s, *my_miss, 1) == "missed a rat");
    // Others: the first character is upper-cased in the full sentence only.
    game::Spawn snake = rat;
    snake.id = 4;
    snake.name = "a_snake01";
    w.spawns[4] = snake;
    auto others = game::damage_report(s, w, 2, 4, 4, 5, 0);
    CHECK(others->kind == game::HitReport::OthersHits);
    CHECK(game::report_hit_line(s, *others, 0) == "A snake bites a rat for 5 points of damage.");
    CHECK(game::report_hit_line(s, *others, 1) == "a snake bites a rat for 5");
    auto others_miss = game::damage_report(s, w, 2, 4, 4, 0, 0);
    CHECK(others_miss->kind == game::HitReport::OthersMisses);
    CHECK(game::report_hit_line(s, *others_miss, 1) == "a snake missed a rat");

    CHECK(game::death_message(s, w, 2, 1, 4) == "You have slain a rat!");
    CHECK(game::death_message(s, w, 1, 2, 4) == "You have been slain by a rat!");
    game::Spawn guard = rat;
    guard.id = 3;
    guard.name = "Guard_Bob00";
    guard.race = 1;
    w.spawns[3] = guard;
    CHECK(game::death_message(s, w, 2, 3, 4) == "A rat has been slain by Guard Bob!");
}

TEST_CASE("ReportSuccessfulHit's chat filters 13 to 16 on melee lines") {
    game::HitLine yours{game::HitReport::YourHits, "", "", "", "", 3, true, false, false};
    game::HitLine my_miss{game::HitReport::YourMisses, "", "", "", "", 0, true, false, false};
    game::HitLine on_me{game::HitReport::YouBeingHit, "", "", "", "", 2, false, true, false};
    game::HitLine missed_me{game::HitReport::YouBeingMissed, "", "", "", "", -4, false, true, false};
    game::HitLine others{game::HitReport::OthersHits, "", "", "", "", 5, false, false, false};
    game::HitLine others_miss{game::HitReport::OthersMisses, "", "", "", "", 0, false, false, false};
    game::HitLine self_miss{game::HitReport::YouBeingMissed, "", "", "", "", 0, true, true, false};
    game::ChatFilters f; // every filter at its default: shown
    for (const auto* l : {&yours, &my_miss, &on_me, &missed_me, &others, &others_miss, &self_miss})
        CHECK(game::hit_line_shown(*l, false, f));
    CHECK_FALSE(game::hit_line_shown(others, false, f, false)); // not reported
    for (int k = 13; k <= 16; ++k) f.set(k, 0);                // setting 0: mode 1, hidden
    CHECK(game::hit_line_shown(yours, false, f));
    CHECK(game::hit_line_shown(on_me, false, f));
    CHECK_FALSE(game::hit_line_shown(my_miss, false, f));      // 13
    CHECK_FALSE(game::hit_line_shown(others_miss, false, f));  // 14
    CHECK_FALSE(game::hit_line_shown(others, false, f));       // 15
    CHECK_FALSE(game::hit_line_shown(missed_me, false, f));    // 16
    CHECK_FALSE(game::hit_line_shown(self_miss, false, f));
    // The player's pet gets through 14 and 15, even unreported.
    CHECK(game::hit_line_shown(others, true, f));
    CHECK(game::hit_line_shown(others_miss, true, f));
    CHECK(game::hit_line_shown(others, true, f, false));
    // Only the one filter counts.
    game::ChatFilters g;
    g.set(16, 0);
    CHECK(game::hit_line_shown(my_miss, false, g));
    CHECK(game::hit_line_shown(others_miss, false, g));
    CHECK_FALSE(game::hit_line_shown(missed_me, false, g));
    // An NPC hitting itself is not reported.
    game::HitLine npc_self = others;
    npc_self.npc_hit_itself = true;
    CHECK_FALSE(game::hit_line_shown(npc_self, false, game::ChatFilters{}));
}

TEST_CASE("damage verbs by type and attacker race (SetDefaultDamageDescByRace)") {
    game::EqStrings s;
    for (std::uint32_t id = 12163; id <= 12202; ++id) s.set(id, std::to_string(id));
    s.set(5838, "5838");
    CHECK(game::damage_verb(s, 0, 1, false) == "12191");  // crush
    CHECK(game::damage_verb(s, 1, 1, true) == "12180");   // slashes
    CHECK(game::damage_verb(s, 36, 1, false) == "12193"); // pierce
    CHECK(game::damage_verb(s, 30, 1, true) == "12196");  // kicks
    CHECK(game::damage_verb(s, 4, 1, false) == "12181");  // punch (PC race)
    CHECK(game::damage_verb(s, 4, 43, true) == "12165");  // bear mauls
    CHECK(game::damage_verb(s, 28, 50, true) == "12168"); // lion claws
    CHECK(game::damage_verb(s, 4, 85, false) == "12174"); // slice
    CHECK(game::damage_verb(s, 4, 85, true) == "12173");  // slices
    CHECK(game::damage_verb(s, 4, 60, true) == "12184");  // other races hit
    CHECK(game::damage_verb(s, 4, 330, false) == "12181");
}

TEST_CASE("targeting: nearest NPC in front, cycling, and ray picking") {
    game::WorldState w;
    w.player_id = 1;
    auto add = [&](std::uint32_t id, glm::vec3 p, bool npc = true, bool corpse = false) {
        game::Spawn s;
        s.id = id;
        s.position = p;
        s.is_npc = npc;
        s.is_corpse = corpse;
        s.size = 6.0f;
        s.heights.height = 6.0f;
        w.old_model_scale = false; // in zone: AvatarHeight 3.75, the feet 3.75 below z
        w.set_heights(s);
        w.spawns[id] = s;
    };
    add(1, {0, 0, 0}, false);
    add(2, {10, 0, 0});
    add(3, {20, 0, 0});
    add(4, {-5, 0, 0});              // behind
    add(5, {5, 1, 0}, true, true);   // corpse
    add(6, {8, 0, 0}, false);        // a player
    CHECK(game::nearest_npc(w, {0, 0, 0}, 0.0f, 200.0f, 0) == 2);
    CHECK(game::nearest_npc(w, {0, 0, 0}, 0.0f, 200.0f, 2) == 3);
    CHECK(game::nearest_npc(w, {0, 0, 0}, glm::pi<float>(), 200.0f, 0) == 4);
    CHECK(game::next_npc(w, {0, 0, 0}, 0.0f, 200.0f, 0) == 2);
    CHECK(game::next_npc(w, {0, 0, 0}, 0.0f, 200.0f, 2) == 3);
    CHECK(game::next_npc(w, {0, 0, 0}, 0.0f, 200.0f, 3) == 2);
    CHECK(game::heading_to({0, 0, 0}, {0, 10, 0}) == doctest::Approx(128.0f));

    // A ray at z 1 along +X passes over the low corpse at 5 and hits the player at 8.
    auto hit = game::pick_spawn(w, {0, 0, 1}, {1, 0, 0}, 500.0f, 1);
    REQUIRE(hit);
    CHECK(*hit == 6);
    auto miss = game::pick_spawn(w, {0, 0, 50}, {1, 0, 0}, 500.0f, 1);
    CHECK_FALSE(miss.has_value());
    auto down = game::pick_spawn(w, {20, 0, 50}, {0, 0, -1}, 500.0f, 1);
    REQUIRE(down);
    CHECK(*down == 3);
}

TEST_CASE("OP_SimpleMessage is SimpleMessage_Struct: string id, colour, unknown") {
    ByteWriter w;
    w.le<std::uint32_t>(138); // You gain experience!!
    w.le<std::uint32_t>(15);
    w.le<std::uint32_t>(0);
    AppPacket p = packet(Op::SimpleMessage, std::move(w));
    auto m = decode_simple_message(p);
    REQUIRE(m);
    CHECK(m->string_id == 138);
    CHECK(m->color == 15);
    CHECK(encode(*m).data == p.data);
}

TEST_CASE("npc_owner_line: an NPC's last name under its name; a pet's only with PetOwnerNames") {
    game::Spawn npc;
    npc.is_npc = true;
    npc.last_name = "Banker";
    CHECK(game::npc_owner_line(npc, false) == "(Banker)");
    game::Spawn pet = npc;
    pet.pet_owner_id = 7;
    pet.last_name = "Soandso`s_pet";
    CHECK(game::npc_owner_line(pet, true) == "(Soandso`s pet)");
    CHECK(game::npc_owner_line(pet, false).empty());
    game::Spawn player;
    player.last_name = "Smith";
    CHECK(game::npc_owner_line(player, true).empty());
    game::Spawn corpse = npc;
    corpse.is_corpse = true;
    CHECK(game::npc_owner_line(corpse, true).empty());
}

TEST_CASE("name_sprite_switched_on: the name switches, and the player's own name outside first person") {
    game::Spawn pc, npc, corpse;
    npc.is_npc = true;
    corpse.is_corpse = true;
    game::NameSpriteSwitches on;
    CHECK(game::name_sprite_switched_on(pc, false, false, on));
    CHECK(game::name_sprite_switched_on(pc, true, false, on)); // the original draws the local player's too
    CHECK_FALSE(game::name_sprite_switched_on(pc, true, true, on)); // first person: the model is hidden
    CHECK(game::name_sprite_switched_on(npc, false, false, on));
    game::NameSpriteSwitches own_off = on;
    own_off.own_name = false;
    CHECK_FALSE(game::name_sprite_switched_on(pc, true, false, own_off));
    CHECK(game::name_sprite_switched_on(pc, false, false, own_off)); // other players keep theirs
    game::NameSpriteSwitches pcs_off = on;
    pcs_off.pc_names = false; // /shownames off: the player's own goes with the rest
    CHECK_FALSE(game::name_sprite_switched_on(pc, false, false, pcs_off));
    CHECK_FALSE(game::name_sprite_switched_on(pc, true, false, pcs_off));
    CHECK(game::name_sprite_switched_on(npc, false, false, pcs_off));
    game::NameSpriteSwitches npcs_off = on;
    npcs_off.npc_names = false; // corpses go with the NPCs
    CHECK_FALSE(game::name_sprite_switched_on(npc, false, false, npcs_off));
    CHECK_FALSE(game::name_sprite_switched_on(corpse, false, false, npcs_off));
    CHECK(game::name_sprite_switched_on(pc, true, false, npcs_off));
}

TEST_CASE("name_sprite_shows_guild: SetNameSpriteState's guild line (bGuildShowAnim, not anonymous, /shownames 3 or 4)") {
    game::Spawn p;
    p.guild_id = 18;
    p.guild_show = true;
    CHECK(game::name_sprite_shows_guild(p, 3));
    CHECK(game::name_sprite_shows_guild(p, 4));
    CHECK_FALSE(game::name_sprite_shows_guild(p, 2));
    CHECK_FALSE(game::name_sprite_shows_guild(p, 5));
    game::Spawn hidden = p;
    hidden.guild_show = false; // the rank lacks Display Guild Name (SpawnAppearance 52 = 0)
    CHECK_FALSE(game::name_sprite_shows_guild(hidden, 3));
    game::Spawn anon = p;
    anon.anon = 1;
    CHECK_FALSE(game::name_sprite_shows_guild(anon, 3));
    anon.anon = 2; // roleplaying shows it
    CHECK(game::name_sprite_shows_guild(anon, 3));
    anon.anon = 3;
    CHECK(game::name_sprite_shows_guild(anon, 3));
    game::Spawn none = p;
    none.guild_id = 0xffffffff;
    CHECK_FALSE(game::name_sprite_shows_guild(none, 3));
    game::Spawn npc = p;
    npc.is_npc = true;
    CHECK_FALSE(game::name_sprite_shows_guild(npc, 3));
}

TEST_CASE("name_sprite_market_words: SetNameSpriteState's Offline, Trader and Buyer before a player's name") {
    game::EqStrings strings;
    strings.set(766, "OFF");
    strings.set(5503, "TRD");
    strings.set(6055, "BUY");
    game::Spawn p;
    CHECK(game::name_sprite_market_words(p, strings).prefix.empty());
    CHECK_FALSE(game::name_sprite_market_words(p, strings).replaces_title);
    p.trader = true;
    CHECK(game::name_sprite_market_words(p, strings).prefix == "TRD ");
    CHECK(game::name_sprite_market_words(p, strings).replaces_title);
    p.offline = true; // an offline trader: both words, offline first
    CHECK(game::name_sprite_market_words(p, strings).prefix == "OFF TRD ");
    p.buyer = true;   // the trader wins over the buyer
    CHECK(game::name_sprite_market_words(p, strings).prefix == "OFF TRD ");
    p.trader = false;
    CHECK(game::name_sprite_market_words(p, strings).prefix == "OFF BUY ");
    p.buyer = false;  // offline alone keeps the title
    CHECK(game::name_sprite_market_words(p, strings).prefix == "OFF ");
    CHECK_FALSE(game::name_sprite_market_words(p, strings).replaces_title);
    // No string table entry: the plain word, never "[5503]".
    game::Spawn t;
    t.trader = true;
    CHECK(game::name_sprite_market_words(t, game::EqStrings{}).prefix == "Trader ");
    // Only a player's: an NPC or a corpse the server marks as a trader keeps its plain name.
    game::Spawn npc = t;
    npc.is_npc = true;
    npc.offline = true;
    CHECK(game::name_sprite_market_words(npc, strings).prefix.empty());
    CHECK_FALSE(game::name_sprite_market_words(npc, strings).replaces_title);
    game::Spawn corpse = t;
    corpse.is_corpse = true;
    CHECK(game::name_sprite_market_words(corpse, strings).prefix.empty());
}

TEST_CASE("ReportSuccessfulHit's animation on the target") {
    using hailstone::game::HitAnimInput;
    using hailstone::game::hit_animation;
    HitAnimInput h;
    h.type = 1;
    h.damage = 5;
    CHECK(hit_animation(h) == std::optional<std::uint8_t>(0xd)); // D02
    h.damage = 0;
    CHECK_FALSE(hit_animation(h)); // a miss
    h.damage = 5;
    h.local_stand_state = 115;
    CHECK_FALSE(hit_animation(h)); // the local player's state, not the target's
    h.local_stand_state = 100;
    h.target_alive = false;
    CHECK_FALSE(hit_animation(h));
    h.target_alive = true;
    // A spell: its TargetAnim while the target stands, else D02 when detrimental and damaging.
    h.type = 231;
    h.spell_target_anim = 12;
    h.spell_detrimental = true;
    CHECK(hit_animation(h) == std::optional<std::uint8_t>(12));
    h.target_stand_state = 110;
    CHECK(hit_animation(h) == std::optional<std::uint8_t>(0xd));
    h.spell_detrimental = false;
    CHECK_FALSE(hit_animation(h));
}

TEST_CASE("ReportSuccessfulHit's shared tail: a landed hit") {
    using hailstone::game::HitAnimInput;
    using hailstone::game::hit_landed;
    HitAnimInput h;
    h.type = 1;
    h.damage = 5;
    CHECK(hit_landed(h));
    h.damage = 0;
    CHECK_FALSE(hit_landed(h)); // a miss
    h.damage = -1;
    CHECK_FALSE(hit_landed(h)); // a block
    h.damage = 5;
    h.target_alive = false;
    CHECK_FALSE(hit_landed(h)); // a corpse (Type 2 and above)
    h.target_alive = true;
    // Whatever the stand states (the animation's local check does not apply).
    h.local_stand_state = 115;
    CHECK(hit_landed(h));
    // A spell: only a detrimental one with damage.
    h.type = 231;
    CHECK_FALSE(hit_landed(h));
    h.spell_detrimental = true;
    CHECK(hit_landed(h));
    // Environmental and the other types above 231: the damage.
    h.type = 0xfc;
    CHECK(hit_landed(h));
    h.type = 0xe8;
    CHECK(hit_landed(h));
    h.damage = -3;
    CHECK_FALSE(hit_landed(h));
}

TEST_CASE("ReportSuccessfulHit's environmental hits: animation, line and sound") {
    using hailstone::game::EnvHitInput;
    using hailstone::game::environment_hit;
    using hailstone::game::hit_animation;
    using hailstone::game::HitAnimInput;
    HitAnimInput h;
    h.damage = 20;
    h.local_stand_state = 115; // the local player's state does not matter for these
    h.type = 0xf3;
    CHECK(hit_animation(h) == std::optional<std::uint8_t>(0xc)); // D01
    h.type = 0xf9;
    CHECK(hit_animation(h) == std::optional<std::uint8_t>(0xc));
    h.type = 0xfa;
    CHECK(hit_animation(h) == std::optional<std::uint8_t>(0xf)); // D04
    h.type = 0xfb;
    CHECK(hit_animation(h) == std::optional<std::uint8_t>(0xf));
    h.type = 0xfc;
    CHECK(hit_animation(h) == std::optional<std::uint8_t>(0xe)); // D03
    h.damage = -3; // only exactly 0 drops it
    CHECK(hit_animation(h) == std::optional<std::uint8_t>(0xe));
    h.damage = 0;
    CHECK_FALSE(hit_animation(h));
    h.damage = 20;
    h.target_alive = false;
    CHECK_FALSE(hit_animation(h));
    h.target_alive = true;
    // 0xfe and 0xff are no environmental types: D02 as for a melee hit.
    h.local_stand_state = 100;
    h.type = 0xfe;
    CHECK(hit_animation(h) == std::optional<std::uint8_t>(0xd));

    game::EqStrings s;
    s.set(12187, "YOU");
    s.set(12118, "%1 were injured by falling.");
    s.set(12119, "%1 was injured by falling.");
    s.set(12131, "YOU are burning!");
    s.set(12132, "%1 is burning.");
    s.set(12127, "YOU are suffocating!");
    s.set(12128, "YOU are drowning!");
    s.set(12130, "%1 is drowning.");
    s.set(12125, "YOU were crushed.");
    s.set(12126, "YOU were injured by a trap.");
    s.set(12120, "%1 was injured by a trap.");
    EnvHitInput in;
    in.type = 0xfc;
    in.damage = 30;
    in.on_me = true;
    auto e = environment_hit(s, in);
    CHECK(e.text == "YOU were injured by falling.");
    CHECK(e.color == 0x10a);
    CHECK(e.fall_hit_sound);
    in.on_me = false;
    in.victim = "soandso";
    e = environment_hit(s, in);
    CHECK(e.text == "Soandso was injured by falling.");
    CHECK(e.color == 0x117);
    in.type = 0xfa;
    e = environment_hit(s, in);
    CHECK(e.text == "Soandso is burning.");
    CHECK(e.color == 0x149);
    CHECK_FALSE(e.fall_hit_sound);
    in.filter_mode = 1; // chat filter 0x11 hides the others' 0xf3..0xfa lines
    CHECK(environment_hit(s, in).text.empty());
    in.type = 0xfb;
    CHECK(environment_hit(s, in).text == "Soandso is drowning.");
    in.on_me = true;
    CHECK(environment_hit(s, in).text == "YOU are drowning!");
    in.zone_id = 0xab;
    CHECK(environment_hit(s, in).text == "YOU are suffocating!");
    in.type = 0xfd;
    in.spell = 0x91;
    e = environment_hit(s, in);
    CHECK(e.text == "YOU were crushed.");
    CHECK(e.fall_hit_sound);
    in.spell = 0xffffffffu;
    CHECK(environment_hit(s, in).text == "YOU were injured by a trap.");
    in.damage = 0; // no line without damage; the fall sound still plays
    in.type = 0xfc;
    e = environment_hit(s, in);
    CHECK(e.text.empty());
    CHECK(e.fall_hit_sound);
}

TEST_CASE("npc_name_colour: an NPC's name takes its consider colour, and only with the option on") {
    game::Spawn rat;
    rat.id = 2;
    rat.name = "a_rat01";
    rat.race = 34;
    rat.level = 1;
    rat.is_npc = true;
    rat.gender = 2;
    // The rat is white to a level 1 character (see the consider test above).
    CHECK(game::npc_name_colour(rat, 1, 0, true) == game::con_rgb(game::Con::White));
    // Each con gives its own colour: a rat far above the player cons red.
    game::Spawn big = rat;
    big.level = 20;
    CHECK(game::npc_name_colour(big, 1, 0, true) == game::con_rgb(game::con_level(big, 1, 0)));
    CHECK(game::npc_name_colour(big, 1, 0, true) != game::npc_name_colour(rat, 1, 0, true));
    // The option off keeps the stock colours.
    CHECK_FALSE(game::npc_name_colour(rat, 1, 0, false).has_value());
    // Players and corpses are not NPC names: the caller keeps their colours.
    game::Spawn player = rat;
    player.is_npc = false;
    CHECK_FALSE(game::npc_name_colour(player, 1, 0, true).has_value());
    game::Spawn corpse = rat;
    corpse.is_corpse = true;
    CHECK_FALSE(game::npc_name_colour(corpse, 1, 0, true).has_value());
}
