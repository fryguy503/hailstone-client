// The buff flow over a live GameConnection: a curse's counters (BuffSlot::slot_data) must survive the landing of a
// weaker cure and the server's zeroed OP_Buff refresh, so the buff window keeps drawing them
// (game/hit_landing.cpp, net/game_connection_spells.cpp, game/buff_overlays.cpp).

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "game/buff_overlays.h"
#include "game/spells.h"
#include "mq_test_world.h"
#include "net/spell_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

using A = GameConnectionTestAccess;

// A 237-column spells_us.txt line: base, max and effect slots k are at 20+k, 44+k and 86+k (game/spells.cpp). The
// effect slots start as the unused SPA 254 so only the effects a test sets are applied.
std::string spell_line(std::initializer_list<std::pair<int, std::string>> set) {
    std::vector<std::string> cols(237, "0");
    for (int i = 1; i <= 8; ++i) cols[static_cast<std::size_t>(i)].clear();
    for (int i = 86; i <= 97; ++i) cols[static_cast<std::size_t>(i)] = "254";
    for (const auto& [c, v] : set) cols[static_cast<std::size_t>(c)] = v;
    std::string line;
    for (std::size_t i = 0; i < cols.size(); ++i) line += (i ? "^" : "") + cols[i];
    return line;
}

// 7001 Curse of Flames: a buff whose first effect is a curse counter of 12.
// 7002 Weak Cure: an instant spell that takes 3 curse counters off.
// 7003 Strong Cure: an instant spell that takes 12 curse counters off.
// 7004 Poison of Flames: a buff whose first effect is a poison counter of 12 (a different counter type).
std::shared_ptr<game::SpellTable> curse_and_cure() {
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_line({{0, "7001"}, {1, "Curse of Flames"}, {16, "3"}, {17, "600"}, {20, "12"},
                                     {44, "12"}, {70, "100"}, {86, "116"}}) +
                         "\n" +
                         spell_line({{0, "7002"}, {1, "Weak Cure"}, {20, "-3"}, {70, "100"}, {83, "1"},
                                     {86, "116"}}) +
                         "\n" +
                         spell_line({{0, "7003"}, {1, "Strong Cure"}, {20, "-12"}, {70, "100"}, {83, "1"},
                                     {86, "116"}}) +
                         "\n" +
                         spell_line({{0, "7004"}, {1, "Poison of Flames"}, {16, "3"}, {17, "600"}, {20, "12"},
                                     {44, "12"}, {70, "100"}, {86, "36"}})) == 4);
    return table;
}

// The opcode's server packet: the slot's buff with the counters left it carries.
BuffPacket curse_buff(std::uint32_t entity, std::uint32_t slot, std::uint32_t spell, std::int32_t counters) {
    BuffPacket p;
    p.entity_id = entity;
    p.slot = slot;
    p.fade = 0;
    p.buff.effect_type = 2;
    p.buff.level = 50;
    p.buff.spell_id = spell;
    p.buff.duration = 600;
    p.buff.caster_id = 10; // the rat, an NPC
    p.buff.slot_data[0] = counters;
    return p;
}

} // namespace

TEST_CASE("OP_Action places curse counters and a weak cure lowers them before OP_Buff refresh") {
    mq_test::World w("game_connection_buff_cure");
    auto& conn = *w.conn;
    auto table = curse_and_cure();
    conn.set_spell_table_for_test(table);
    auto& spells = A::spells(conn);
    const std::uint32_t me = A::world(conn).player_id;

    spells.buffs.fill({});
    Action action;
    action.target = static_cast<std::uint16_t>(me);
    action.source = 10; // the rat
    action.type = 231;
    action.spell_id = 7001;
    action.spell_level = 50;
    action.effect_flag = 4;
    conn.receive_zone_packet_for_test(encode(action));
    constexpr std::uint32_t kSlot = 0; // first free slot
    REQUIRE(spells.buffs[kSlot].spell == 7001);
    REQUIRE(spells.buffs[kSlot].counters_known);
    REQUIRE(spells.buffs[kSlot].slot_data[0] == 12);
    auto before = game::buff_counter_overlay(game::spell_counters(*table->get(7001), spells.buffs));
    REQUIRE(before);
    CHECK(before->text == "12");

    // A -3 cure lands on the player through HitBySpell's landing path.
    action.spell_id = 7002;
    conn.receive_zone_packet_for_test(encode(action));

    // The debuff stays (its 9 counters are left) and the window draws those, not an empty buff.
    CHECK(spells.buffs[kSlot].spell == 7001);
    CHECK(spells.buffs[kSlot].slot_data[0] == 9);
    auto after = game::buff_counter_overlay(game::spell_counters(*table->get(7001), spells.buffs));
    REQUIRE(after);
    CHECK(after->text == "9");

    auto refresh = curse_buff(me, kSlot, 7001, 0);
    refresh.fade = 2;
    conn.receive_zone_packet_for_test(encode(refresh));
    CHECK(spells.buffs[kSlot].slot_data[0] == 9);
    CHECK(spells.buffs[kSlot].counters_known);
}

// The reported bug: a cure that is too weak for the server's real counter pool but looks complete against the
// client's stale local data must not make the debuff vanish. The old local remove fired here (3 <= 12).
TEST_CASE("a cure too weak for the server's real counters leaves the icon standing") {
    mq_test::World w("game_connection_buff_weak_cure");
    auto& conn = *w.conn;
    auto table = curse_and_cure();
    conn.set_spell_table_for_test(table);
    auto& buffs = A::spells(conn).buffs;
    buffs.fill({});
    const std::uint32_t me = A::world(conn).player_id;
    constexpr std::uint32_t kSlot = 2;

    // The client only knows 3 counters (a stale refresh); the server's pool is larger.
    conn.receive_zone_packet_for_test(encode(curse_buff(me, kSlot, 7001, 3)));
    REQUIRE(buffs[kSlot].counters_known);
    REQUIRE(buffs[kSlot].slot_data[0] == 3);

    Action action;
    action.target = static_cast<std::uint16_t>(me);
    action.source = 10;
    action.type = 231;
    action.spell_id = 7003; // -12, more than the client knows about
    action.spell_level = 50;
    action.effect_flag = 4;
    conn.receive_zone_packet_for_test(encode(action));

    REQUIRE(buffs[kSlot].spell == 7001); // the icon still stands
    CHECK(buffs[kSlot].slot_data[0] == 0);
    CHECK_FALSE(game::buff_counter_overlay(game::spell_counters(*table->get(7001), buffs)).has_value());

    // The server still has the buff and restores the real count; that is the authority the client waits for.
    auto refresh = curse_buff(me, kSlot, 7001, 9);
    refresh.fade = 2;
    conn.receive_zone_packet_for_test(encode(refresh));
    CHECK(buffs[kSlot].spell == 7001);
    CHECK(buffs[kSlot].slot_data[0] == 9);
}

TEST_CASE("a full cure after the server's fade leaves the next curse's counters alone") {
    mq_test::World w("game_connection_buff_server_fade");
    auto& conn = *w.conn;
    conn.set_spell_table_for_test(curse_and_cure());
    auto& buffs = A::spells(conn).buffs;
    buffs.fill({});
    const std::uint32_t me = A::world(conn).player_id;
    constexpr std::uint32_t kFirst = 0, kSecond = 1;

    for (std::uint32_t slot : {kFirst, kSecond})
        conn.receive_zone_packet_for_test(encode(curse_buff(me, slot, 7001, 12)));
    REQUIRE(buffs[kFirst].spell == 7001);
    REQUIRE(buffs[kSecond].spell == 7001);

    // EQEmu sends the fade for the cured buff in the same network update, just before the cure's OP_Action.
    auto fade = curse_buff(me, kFirst, 7001, 0);
    fade.fade = 1;
    conn.receive_zone_packet_for_test(encode(fade));
    REQUIRE(buffs[kFirst].empty());

    Action action;
    action.target = static_cast<std::uint16_t>(me);
    action.source = 10;
    action.type = 231;
    action.spell_id = 7003; // -12: enough for either buff on its own
    action.spell_level = 50;
    action.effect_flag = 4;
    conn.receive_zone_packet_for_test(encode(action));
    conn.update(); // the end of the update clears the fade mask

    // The server cured only the first buff; the second was never touched and keeps all 12 counters. The old code
    // spent this cure against it and emptied the icon, and it stayed wrong until the buff expired.
    REQUIRE(buffs[kSecond].spell == 7001);
    CHECK(buffs[kSecond].slot_data[0] == 12);
}

TEST_CASE("a fade of one counter type does not block a cure of another type") {
    mq_test::World w("game_connection_buff_fade_type_isolation");
    auto& conn = *w.conn;
    conn.set_spell_table_for_test(curse_and_cure());
    auto& buffs = A::spells(conn).buffs;
    buffs.fill({});
    const std::uint32_t me = A::world(conn).player_id;
    constexpr std::uint32_t kCurse = 0, kPoison = 4;

    conn.receive_zone_packet_for_test(encode(curse_buff(me, kCurse, 7001, 12)));
    conn.receive_zone_packet_for_test(encode(curse_buff(me, kPoison, 7004, 12)));
    REQUIRE(buffs[kCurse].slot_data[0] == 12);
    REQUIRE(buffs[kPoison].slot_data[0] == 12);

    // Only a poison buff fades, so only the poison counter type is suppressed.
    auto fade = curse_buff(me, kPoison, 7004, 0);
    fade.fade = 1;
    conn.receive_zone_packet_for_test(encode(fade));
    REQUIRE(buffs[kPoison].empty());

    Action action;
    action.target = static_cast<std::uint16_t>(me);
    action.source = 10;
    action.type = 231;
    action.spell_id = 7003; // a curse cure (SPA 116), not a poison cure (SPA 36)
    action.spell_level = 50;
    action.effect_flag = 4;
    conn.receive_zone_packet_for_test(encode(action));
    conn.update();

    // The curse cure is predicted normally: the curse is drained, not left standing because a poison faded.
    REQUIRE(buffs[kCurse].spell == 7001);
    CHECK(buffs[kCurse].slot_data[0] == 0);
}

TEST_CASE("a cure in a later update predicts counters normally after the fade mask resets") {
    mq_test::World w("game_connection_buff_fade_reset");
    auto& conn = *w.conn;
    conn.set_spell_table_for_test(curse_and_cure());
    auto& buffs = A::spells(conn).buffs;
    buffs.fill({});
    const std::uint32_t me = A::world(conn).player_id;
    constexpr std::uint32_t kFirst = 0, kSecond = 1;

    for (std::uint32_t slot : {kFirst, kSecond})
        conn.receive_zone_packet_for_test(encode(curse_buff(me, slot, 7001, 12)));

    // One update: a curse fades but no cure follows it.
    auto fade = curse_buff(me, kFirst, 7001, 0);
    fade.fade = 1;
    conn.receive_zone_packet_for_test(encode(fade));
    conn.update();
    REQUIRE(buffs[kFirst].empty());
    REQUIRE(buffs[kSecond].slot_data[0] == 12);

    // A later, separate cure must work normally; the old fade may not suppress it.
    Action action;
    action.target = static_cast<std::uint16_t>(me);
    action.source = 10;
    action.type = 231;
    action.spell_id = 7003;
    action.spell_level = 50;
    action.effect_flag = 4;
    conn.receive_zone_packet_for_test(encode(action));

    REQUIRE(buffs[kSecond].spell == 7001);
    CHECK(buffs[kSecond].slot_data[0] == 0);
}

TEST_CASE("OP_BuffCreate with unknown counters leaves curses for the server to remove") {
    mq_test::World w("game_connection_buff_unknown_cure");
    auto& conn = *w.conn;
    conn.set_spell_table_for_test(curse_and_cure());
    auto& buffs = A::spells(conn).buffs;
    buffs.fill({});
    const std::uint32_t me = A::world(conn).player_id;
    constexpr std::uint32_t kFirst = 4, kSecond = 6;

    BuffIcons full;
    full.entity_id = me;
    full.all_buffs = 1;
    full.entries = {{kFirst, 7001, 599, 0, "Caster"}};
    SUBCASE("one unknown curse") {}
    SUBCASE("two unknown curses") { full.entries.push_back({kSecond, 7001, 599, 0, "Caster"}); }
    conn.receive_zone_packet_for_test(encode(full, false));
    for (const auto& e : full.entries) {
        REQUIRE(buffs[e.slot].spell == 7001);
        REQUIRE_FALSE(buffs[e.slot].counters_known);
    }

    Action action;
    action.target = static_cast<std::uint16_t>(me);
    action.source = 10;
    action.type = 231;
    action.spell_id = 7002;
    action.spell_level = 50;
    action.effect_flag = 4;
    conn.receive_zone_packet_for_test(encode(action));
    for (const auto& e : full.entries) {
        CHECK(buffs[e.slot].spell == 7001);
        CHECK_FALSE(buffs[e.slot].counters_known);
    }

    // Nonzero OP_Buff data makes this slot predictable again, but an earlier unknown slot still stops
    // prediction: the client leaves the cure to the server instead of guessing which buff it removes.
    conn.receive_zone_packet_for_test(encode(curse_buff(me, kSecond, 7001, 9)));
    REQUIRE(buffs[kSecond].counters_known);
    conn.receive_zone_packet_for_test(encode(action));
    CHECK(buffs[kSecond].slot_data[0] == 9);

    auto fade = curse_buff(me, kFirst, 7001, 0);
    fade.fade = 1;
    conn.receive_zone_packet_for_test(encode(fade));
    conn.update();
    CHECK(buffs[kFirst].empty());
    conn.receive_zone_packet_for_test(encode(action));
    REQUIRE(buffs[kSecond].spell == 7001);
    CHECK(buffs[kSecond].slot_data[0] == 6);
    // A strong cure spends the last counters locally but leaves the buff standing; EQEmu's fade=1 is the only
    // thing that clears the icon.
    action.spell_id = 7003;
    conn.receive_zone_packet_for_test(encode(action));
    REQUIRE(buffs[kSecond].spell == 7001);
    CHECK(buffs[kSecond].slot_data[0] == 0);

    auto cured_fade = curse_buff(me, kSecond, 7001, 0);
    cured_fade.fade = 1;
    conn.receive_zone_packet_for_test(encode(cured_fade));
    conn.update();
    CHECK(buffs[kSecond].empty());
}

TEST_CASE("OP_Buff with a zeroed slot_data does not wipe counters the client already knows") {
    mq_test::World w("game_connection_buff_guard");
    auto& conn = *w.conn;
    auto table = curse_and_cure();
    conn.set_spell_table_for_test(table);
    auto& spells = A::spells(conn);
    const std::uint32_t me = A::world(conn).player_id;

    constexpr std::uint32_t kSlot = 4;
    conn.receive_zone_packet_for_test(encode(curse_buff(me, kSlot, 7001, 9)));
    REQUIRE(spells.buffs[kSlot].spell == 7001);
    REQUIRE(spells.buffs[kSlot].slot_data[0] == 9);
    spells.buffs[kSlot].caster_name = "Old caster";

    // EQEmu's refresh (fade 2) sends the slot_data as zeros; the counters the client knows are not the server's to
    // erase (a cure already lowered them locally and the next OP_Buff must not empty the icon).
    BuffPacket refresh = curse_buff(me, kSlot, 7001, 0);
    refresh.fade = 2;
    refresh.buff.duration = 601;
    conn.receive_zone_packet_for_test(encode(refresh));
    CHECK(spells.buffs[kSlot].spell == 7001);
    CHECK(spells.buffs[kSlot].slot_data[0] == 9);
    CHECK(spells.buffs[kSlot].tics == 601); // the rest of the refresh did arrive
    CHECK(spells.buffs[kSlot].caster_name == "Old caster");
    auto overlay = game::buff_counter_overlay(game::spell_counters(*table->get(7001), spells.buffs));
    REQUIRE(overlay);
    CHECK(overlay->text == "9");

    // Same spell with fade 2 but a real server value must overwrite local counters (guard only for zeroed).
    BuffPacket with_data = curse_buff(me, kSlot, 7001, 5);
    with_data.fade = 2;
    conn.receive_zone_packet_for_test(encode(with_data));
    CHECK(spells.buffs[kSlot].spell == 7001);
    CHECK(spells.buffs[kSlot].slot_data[0] == 5);

    // fade 0 with zeroed slot_data is not a refresh and must overwrite: guard only keeps at fade 2.
    conn.receive_zone_packet_for_test(encode(curse_buff(me, kSlot, 7001, 9)));
    REQUIRE(spells.buffs[kSlot].slot_data[0] == 9);
    BuffPacket zero_fade0 = curse_buff(me, kSlot, 7001, 0);
    zero_fade0.fade = 0;
    conn.receive_zone_packet_for_test(encode(zero_fade0));
    CHECK(spells.buffs[kSlot].spell == 7001);
    CHECK(spells.buffs[kSlot].slot_data[0] == 0);

    // A different spell in the slot is a new buff: nothing of the old counters applies to it.
    // fade 2 here exercises the same_spell guard (zeroed slot_data of another spell must not keep the old).
    conn.receive_zone_packet_for_test(encode(curse_buff(me, kSlot, 7001, 9)));
    REQUIRE(spells.buffs[kSlot].slot_data[0] == 9);
    BuffPacket other = curse_buff(me, kSlot, 7002, 0);
    other.fade = 2;
    conn.receive_zone_packet_for_test(encode(other));
    CHECK(spells.buffs[kSlot].spell == 7002);
    CHECK(spells.buffs[kSlot].slot_data[0] == 0);
    CHECK(spells.buffs[kSlot].caster_name.empty());

    // The fade still clears the slot.
    BuffPacket fade = curse_buff(me, kSlot, 7002, 0);
    fade.fade = 1;
    conn.receive_zone_packet_for_test(encode(fade));
    CHECK(spells.buffs[kSlot].empty());
}

TEST_CASE("OP_BuffCreate full list preserves counters only for unchanged buff slots") {
    mq_test::World w("game_connection_buff_full_list");
    auto& conn = *w.conn;
    conn.set_spell_table_for_test(curse_and_cure());
    const auto& buffs = A::spells(conn).buffs;
    const std::uint32_t me = A::world(conn).player_id;
    constexpr std::uint32_t kKept = 4, kMissing = 5, kReplaced = 6;
    for (const auto slot : {kKept, kMissing, kReplaced}) {
        conn.receive_zone_packet_for_test(encode(curse_buff(me, slot, 7001, 9)));
        REQUIRE(buffs[slot].spell == 7001);
        REQUIRE(buffs[slot].slot_data[0] == 9);
    }

    BuffIcons full;
    full.entity_id = me;
    full.all_buffs = 1;
    full.entries = {{kKept, 7001, 599, 0, "Caster"}, {kReplaced, 7003, 20, 0, "Caster"}};
    conn.receive_zone_packet_for_test(encode(full, false));

    CHECK(buffs[kKept].spell == 7001);
    CHECK(buffs[kKept].tics == 599);
    CHECK(buffs[kKept].slot_data[0] == 9);
    CHECK(buffs[kKept].counters_known);
    CHECK(buffs[kMissing].empty());
    CHECK(buffs[kMissing].slot_data[0] == 0);
    CHECK(buffs[kReplaced].spell == 7003);
    CHECK(buffs[kReplaced].slot_data[0] == 0);
    CHECK_FALSE(buffs[kReplaced].counters_known);
}

