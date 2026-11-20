// GameConnection state that must not outlive what it was about: questions across a zone change or a loot session,
// the trader selection after a zone change, spawn deletion and renaming, the auras, aggro meter and stun at
// zone-in; and the decoders of OP_XTargetResponse, OP_GuildUpdate and opcode 0x3ed4. Against the mock EQEmu.

#include <doctest/doctest.h>

#include "core/byte_io.h"
#include "game/bazaar.h"
#include "game/client_options.h"
#include "mock_server/harness.h"
#include "net/aggro_packets.h"
#include "net/group_packets.h"
#include "net/item_packets.h"
#include "net/market_packets.h"
#include "net/social_packets.h"
#include "net/spell_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

// The server sends the player to zone 1 (qeynos) and the client zones in there.
bool zone_again(Rig& rig, std::uint16_t zone = 1) {
    const int entries = rig.server.zone_entries;
    rig.server.request_zone_change(zone, 20.0f, 10.0f, 3.0f);
    return rig.until([&] { return rig.server.zone_entries == entries + 1 && rig.conn.state() == ConnectionState::InZone; }) &&
           rig.until([&] { return rig.conn.player_status().max_hp != 0; });
}

std::uint32_t add_corpse(GameConnection& conn) {
    auto& w = conn.world_for_test();
    const game::Spawn* me = w.player();
    REQUIRE(me);
    game::Spawn s;
    s.id = 4000;
    s.name = "a_rat's_corpse01";
    s.is_npc = true;
    s.is_corpse = true;
    s.position = me->position + glm::vec3(5.0f, 0.0f, 0.0f);
    s.heights.avatar_height = 2.0f;
    w.spawns[s.id] = s;
    return s.id;
}

AppPacket money(std::uint8_t response) {
    MoneyOnCorpse m;
    m.response = response;
    return encode(m);
}

AppPacket corpse_item(std::int16_t slot, const std::string& name, std::uint8_t no_drop) {
    ItemPacket ip;
    ip.type = kItemPacketLoot;
    ip.item.id = 1001;
    ip.item.name = name;
    ip.item.slot.slot = slot;
    ip.item.stack_count = 1;
    ip.item.no_drop = no_drop; // 0: NO DROP
    return encode_item_packet(false, ip);
}

AppPacket stun(std::uint32_t ms) {
    ByteWriter w;
    w.le(ms);
    return AppPacket(to_wire(Op::Stun), w.take());
}

AppPacket mob_rename(std::string_view old_name, std::string_view new_name, std::uint32_t enabled = 1) {
    ByteWriter w;
    w.fixed_string(old_name, 64);
    w.fixed_string(old_name, 64);
    w.fixed_string(new_name, 64);
    w.le<std::uint32_t>(0);
    w.le(enabled);
    return AppPacket(to_wire(Op::MobRename), w.take());
}

} // namespace

TEST_CASE("questions: the mercenary's dismiss question waits behind an open one") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    conn.send_command("/veteranReward");
    REQUIRE(conn.confirmation());
    REQUIRE(conn.confirmation()->id == kConfirmVeteranReward);
    conn.set_client_option_for_session(game::ClientOption::DismissMercenaryConfirm, 1);
    conn.merc_dismiss();
    // It used to replace the question on screen.
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->id == kConfirmVeteranReward);
    conn.answer_confirmation(false);
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->id == kConfirmMercDismiss);
}

TEST_CASE("questions: a zone change drops the one on screen and those waiting") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    conn.send_command("/veteranReward");
    conn.send_command("/veteranReward");
    REQUIRE(conn.confirmation());
    REQUIRE(zone_again(rig));
    CHECK_FALSE(conn.confirmation());
    // A new question is alone: no question from the last zone comes up after it.
    conn.send_command("/veteranReward");
    REQUIRE(conn.confirmation());
    conn.answer_confirmation(false);
    CHECK_FALSE(conn.confirmation());
}

TEST_CASE("questions: the end of a loot session drops its NO DROP question and the window's popup flag") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    std::uint32_t corpse = add_corpse(conn);
    conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 0); // always asks about no-drop items
    conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 0);
    conn.destroy_cursor(); // the mock character holds coin, which stops looting

    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    conn.receive_zone_packet_for_test(corpse_item(23, "Cloth Cap", 0));
    conn.loot_item(23, false);
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->id == kConfirmLootNoDrop);
    conn.receive_zone_packet_for_test(AppPacket{to_wire(Op::LootComplete), {}});
    CHECK_FALSE(conn.loot_session());
    CHECK_FALSE(conn.confirmation());

    // The next corpse: Loot All closes the window after the last answer, which a popup flag left set would stop.
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    conn.receive_zone_packet_for_test(corpse_item(24, "Rusty Dagger", 255));
    conn.receive_zone_packet_for_test(money(6));
    conn.loot_all();
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootItem) == 1; }));
    LootItem answer;
    answer.lootee = corpse;
    answer.slot = 24;
    answer.auto_loot = 1;
    conn.receive_zone_packet_for_test(encode(answer));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 1; }));
}

TEST_CASE("trader: a zone change forgets the selected slot with the slots") {
    MockConfig config = MockConfig::standard();
    MockZone bazaar;
    bazaar.id = static_cast<std::uint16_t>(game::kBazaarZoneId);
    bazaar.short_name = "bazaar";
    bazaar.long_name = "The Bazaar";
    config.zones[bazaar.id] = bazaar;
    MockCharacter& me = config.characters.front();
    me.zone_id = bazaar.id;
    Item satchel = make_item(17000, "Trader's Satchel", 24);
    satchel.item_class = 1;
    satchel.bag_type = game::kBagTraderSatchel;
    satchel.bag_slots = 8;
    Item sword = make_item(5023, "Fine Steel Sword", 24);
    sword.slot = ItemSlot::possessions(24, 0);
    satchel.contents.push_back({0, sword});
    me.inventory.push_back(satchel);
    Rig rig(config);
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    REQUIRE(conn.open_trader_window());
    REQUIRE(conn.trader_state().slots.size() == 1);
    conn.trader_select(0);
    REQUIRE(conn.trader_state().selected == 0);

    REQUIRE(zone_again(rig, 2));
    CHECK(conn.trader_state().slots.empty());
    CHECK(conn.trader_state().selected == -1);
    // A late price update has no slot to name (it indexed the cleared list before).
    conn.receive_zone_packet_for_test(encode_trader_status(kTraderPriceUpdate, 1));
    conn.trader_remove();
    conn.trader_set_price();
    CHECK(conn.trader_state().slots.empty());
}

TEST_CASE("OP_DeleteSpawn of the target clears it and stops auto attack (msgEQRmPlayer)") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    REQUIRE(conn.world().spawns.count(101));
    conn.set_target(101);
    conn.set_auto_attack(true);
    rig.server.remove_spawn(102); // another spawn leaving changes nothing
    REQUIRE(rig.until([&] { return conn.world().spawns.count(102) == 0; }));
    CHECK(conn.world().target_id == 101);
    CHECK(conn.auto_attack());
    rig.server.remove_spawn(101);
    REQUIRE(rig.until([&] { return conn.world().spawns.count(101) == 0; }));
    CHECK(conn.world().target_id == 0);
    CHECK_FALSE(conn.auto_attack());
}

TEST_CASE("OP_MobRename updates the spawn with the full old name and preserves its identity") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());

    constexpr std::uint32_t first = 4000, second = 4001;
    rig.server.add_spawn(make_spawn("a_shade000", first, 1, 1, 0, 0, 0));
    rig.server.add_spawn(make_spawn("a_shade001", second, 1, 1, 0, 0, 0));
    const auto& spawns = conn.world().spawns;
    REQUIRE(rig.until([&] { return spawns.contains(first) && spawns.contains(second); }));
    const auto incarnation = spawns.at(second).incarnation;
    REQUIRE(incarnation != 0);
    conn.set_target(second);

    conn.receive_zone_packet_for_test(mob_rename("a_shade", "Unknown000"));
    conn.receive_zone_packet_for_test(mob_rename("a_shade001", "Disabled000", 0));
    auto short_packet = mob_rename("a_shade001", "Malformed000");
    short_packet.data.pop_back();
    conn.receive_zone_packet_for_test(short_packet);
    auto long_packet = mob_rename("a_shade001", "Malformed000");
    long_packet.data.push_back(std::byte{0});
    conn.receive_zone_packet_for_test(long_packet);
    CHECK(spawns.at(first).name == "a_shade000");
    CHECK(spawns.at(second).name == "a_shade001");

    rig.server.zone.send_reliable(mob_rename("A_SHADE001", "Trial Guide000"));
    REQUIRE(rig.until([&] { return spawns.at(second).name == "Trial Guide000"; }));
    CHECK(spawns.at(first).name == "a_shade000");
    CHECK(spawns.at(second).incarnation == incarnation);
    CHECK(conn.target() == second);

    rig.server.zone.send_reliable(mob_rename("Trial Guide000", "Memory Keeper000"));
    REQUIRE(rig.until([&] { return spawns.at(second).name == "Memory Keeper000"; }));
    CHECK(spawns.at(second).id == second);
    CHECK(spawns.at(second).incarnation == incarnation);
    CHECK(conn.target() == second);
}

TEST_CASE("zone-in forgets the last zone's auras, aggro meter and stun") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    AuraUpdate aura;
    aura.action = kAuraAdd;
    aura.spawn_id = 300;
    aura.name = "Aura of the Zealot";
    conn.receive_zone_packet_for_test(encode(aura));
    conn.receive_zone_packet_for_test(encode(AggroTargetInfo{101, 101}));
    AggroUpdate up;
    up.secondary_id = 102;
    up.entries = {{static_cast<std::uint8_t>(kAggroPlayer), 80}};
    conn.receive_zone_packet_for_test(encode(up));
    conn.receive_zone_packet_for_test(stun(60000));
    REQUIRE(conn.auras().size() == 1);
    REQUIRE(conn.aggro().lock_id == 101);
    REQUIRE(conn.stunned());
    const bool shown = conn.aggro().window_open;

    REQUIRE(zone_again(rig));
    CHECK(conn.auras().empty());
    CHECK(conn.aggro().lock_id == 0);
    CHECK(conn.aggro().target_id == 0);
    CHECK(conn.aggro().secondary_id == 0);
    CHECK(conn.aggro().pct[kAggroPlayer] == 0);
    CHECK(conn.aggro().window_open == shown); // the window keeps its shown state
    CHECK_FALSE(conn.stunned());
}

TEST_CASE("decode_xtarget_response: the max, the entries, and a cut packet keeps what was read") {
    ByteWriter w;
    w.le<std::uint32_t>(5);
    w.le<std::uint32_t>(2);
    w.le<std::int32_t>(1);
    w.le<std::uint8_t>(1);
    w.le<std::uint32_t>(101);
    w.fixed_string("a_rat", 6);
    w.le<std::int32_t>(3);
    w.le<std::uint8_t>(0);
    w.le<std::uint32_t>(102);
    std::vector<std::byte> bytes = w.take();
    bytes.push_back(std::byte{'G'}); // the second name runs off the end
    auto x = decode_xtarget_response(AppPacket(to_wire(Op::XTargetResponse), bytes));
    REQUIRE(x);
    CHECK(x->max == 5);
    REQUIRE(x->entries.size() == 1);
    CHECK(x->entries[0].slot == 1);
    CHECK(x->entries[0].status == 1);
    CHECK(x->entries[0].spawn_id == 101);
    CHECK(x->entries[0].name == "a_rat");
    bytes.push_back(std::byte{0});
    x = decode_xtarget_response(AppPacket(to_wire(Op::XTargetResponse), bytes));
    REQUIRE(x);
    REQUIRE(x->entries.size() == 2);
    CHECK(x->entries[1].slot == 3);
    CHECK(x->entries[1].name == "G");
    CHECK_FALSE(decode_xtarget_response(AppPacket(to_wire(Op::XTargetResponse), std::vector<std::byte>(3))));
}

TEST_CASE("encode_xtarget_request: one slot, its slot number, type and NUL-terminated name") {
    auto p = encode_xtarget_request(4, 3, "a_rat00");
    CHECK(p.opcode == to_wire(Op::XTargetRequest));
    ByteReader r(p.data);
    CHECK(r.le<std::uint32_t>() == 1u);
    CHECK(r.le<std::uint32_t>() == 4u);
    CHECK(r.le<std::uint32_t>() == 3u);
    CHECK(r.cstring() == "a_rat00");
    CHECK(r.remaining() == 0);
    CHECK(encode_xtarget_request(0, 1, "").data.size() == 13);
}

TEST_CASE("decode_guild_update reads what CGuildMgmtWnd writes") {
    auto perm = decode_guild_update(encode_guild_rank_permission(2, 7, true));
    REQUIRE(perm);
    CHECK(perm->action == 5);
    CHECK(perm->rank == 2);
    CHECK(perm->permission == 7);
    CHECK(perm->on);
    auto name = decode_guild_update(encode_guild_rank_name(3, "Officer"));
    REQUIRE(name);
    CHECK(name->action == 4);
    CHECK(name->rank == 3);
    CHECK(name->text == "Officer");
    auto url = decode_guild_update(encode_guild_update_text(0, "http://example.invalid/"));
    REQUIRE(url);
    CHECK(url->text == "http://example.invalid/");
    // Too short for the action: nothing.
    AppPacket p = encode_guild_rank_permission(2, 7, true);
    p.data.resize(0x58);
    CHECK_FALSE(decode_guild_update(p));
    p.data.resize(0x53);
    CHECK_FALSE(decode_guild_update(p));
}

TEST_CASE("decode_split_coin: opcode 0x3ed4's four coin counts") {
    auto c = decode_split_coin(encode_split_coin({1, 2, 3, 4}));
    REQUIRE(c);
    CHECK(c->platinum == 1);
    CHECK(c->gold == 2);
    CHECK(c->silver == 3);
    CHECK(c->copper == 4);
    AppPacket short_one = encode_split_coin({1, 2, 3, 4});
    short_one.data.resize(15);
    CHECK_FALSE(decode_split_coin(short_one));
    CHECK_FALSE(decode_split_coin(AppPacket(static_cast<std::uint16_t>(kOpSplitCoin + 1), encode_split_coin({}).data)));
}
