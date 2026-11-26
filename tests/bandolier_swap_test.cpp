// The bandolier's swap (GameConnection::bandolier_activate, PcZoneClient::BandolierSwap @0x585aa0) against the mock
// server: it moves the items the way EQEmu's Client::SetBandolier does, and is all or nothing like the original's
// MultipleItemMoveManager::ProcessMove. A weapon that has no room to go back to cancels the whole swap with eqstr
// 9342 and no OP_Bandolier, so the client never loses an item the server still has (a player's bug report: with full
// bags, swapping sets and back emptied the primary slot until the next login).
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "game/appearance.h"
#include "game/inventory.h"
#include "mock_server/harness.h"
#include "net/packet_io.h"
#include "net/player_packets.h"
#include "net/zone_packets.h"

using namespace hailstone;
using namespace hailstone::test;
using net::ItemSlot;

namespace {

int swaps_sent(const MockEqServer& server, std::uint8_t set) {
    return static_cast<int>(std::count_if(server.zone_log.begin(), server.zone_log.end(), [&](const net::AppPacket& p) {
        if (!net::is_op(p, net::Op::Bandolier)) return false;
        auto b = net::decode_bandolier(p);
        return b && b->action == net::kBandolierSet && b->number == set;
    }));
}

std::uint32_t id_at(const net::GameConnection& conn, std::int16_t slot) {
    const net::Item* it = conn.inventory().at(ItemSlot::possessions(slot));
    return it ? it->id : 0;
}

std::vector<std::string> chat_lines(net::GameConnection& conn) {
    std::vector<std::string> out;
    while (auto e = conn.poll_chat()) out.push_back(e->text);
    return out;
}

} // namespace

TEST_CASE("bandolier swap: a weapon with no room to go back cancels the swap; with room it goes through") {
    MockConfig config = MockConfig::standard();
    auto& inv = config.characters[0].inventory;
    inv.push_back(make_item(1001, "Cudgel", 13));
    for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s)
        inv.push_back(make_item(2000u + static_cast<std::uint32_t>(s), "Rock", s));
    Rig rig(std::move(config));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    REQUIRE(conn.bandolier_sets() >= 2);

    // Set 0 "1h" holds the Cudgel; set 1 "nodmg" a Dagger, made while the server had it in the primary slot.
    REQUIRE(conn.bandolier_create(0, "1h"));
    rig.server.item_packet(net::kItemPacketCharInventory, make_item(1002, "Dagger", 13));
    REQUIRE(rig.until([&] { return id_at(conn, 13) == 1002; }));
    REQUIRE(conn.bandolier_create(1, "nodmg"));
    rig.server.item_packet(net::kItemPacketCharInventory, make_item(1001, "Cudgel", 13));
    REQUIRE(rig.until([&] { return id_at(conn, 13) == 1001; }));
    chat_lines(conn);

    // No Dagger anywhere, the bags full: the Cudgel would have to go back to the inventory and cannot, so nothing
    // moves and nothing is sent (the original's error 0x11).
    const std::uint64_t revision = conn.inventory().revision();
    CHECK_FALSE(conn.bandolier_activate(1));
    CHECK(id_at(conn, 13) == 1001);
    for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s)
        CHECK(id_at(conn, s) == 2000u + static_cast<std::uint32_t>(s));
    CHECK(conn.inventory().revision() == revision);
    auto lines = chat_lines(conn);
    CHECK(std::find(lines.begin(), lines.end(), "NO ROOM FOR THE BANDOLIER") != lines.end());
    rig.run_for(std::chrono::milliseconds(30));
    CHECK(swaps_sent(rig.server, 1) == 0);
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::WearChange) == 0);

    // The Dagger in the last general slot: pulling it makes the room the Cudgel needs, so both swaps go through and
    // the two weapons trade places each time.
    rig.server.item_packet(net::kItemPacketCharInventory, make_item(1002, "Dagger", net::kSlotGeneral10));
    REQUIRE(rig.until([&] { return id_at(conn, net::kSlotGeneral10) == 1002; }));
    REQUIRE(conn.bandolier_activate(1));
    CHECK(id_at(conn, 13) == 1002);
    CHECK(id_at(conn, net::kSlotGeneral10) == 1001);
    REQUIRE(conn.bandolier_activate(0));
    CHECK(id_at(conn, 13) == 1001);
    CHECK(id_at(conn, net::kSlotGeneral10) == 1002);
    REQUIRE(rig.until([&] { return swaps_sent(rig.server, 1) == 1 && swaps_sent(rig.server, 0) == 1; }));
}

TEST_CASE("bandolier swap: publishes the equipped primary and cleared secondary after activation") {
    MockConfig config = MockConfig::standard();
    auto dagger = make_item(1002, "Dagger", net::kSlotPrimary);
    dagger.id_file = "IT2";
    config.characters.front().inventory = {dagger};
    Rig rig(std::move(config));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    REQUIRE(conn.bandolier_create(0, "Dagger only"));

    // Equip a different primary and a shield, and put the saved dagger in general inventory.
    auto cudgel = make_item(1001, "Cudgel", net::kSlotPrimary);
    cudgel.id_file = "IT63";
    auto shield = make_item(1003, "Shield", net::kSlotSecondary);
    shield.id_file = "IT200";
    dagger.slot = ItemSlot::possessions(net::kSlotGeneral1);
    for (const net::Item& item : {cudgel, shield, dagger}) rig.server.item_packet(net::kItemPacketCharInventory, item);
    REQUIRE(rig.until([&] {
        return id_at(conn, net::kSlotPrimary) == cudgel.id && id_at(conn, net::kSlotSecondary) == shield.id &&
               id_at(conn, net::kSlotGeneral1) == dagger.id &&
               MockEqServer::count(rig.server.zone_log, net::Op::Bandolier) == 1;
    }));
    const auto& appearance = conn.world().player()->appearance;
    REQUIRE(appearance.material[game::kSlotSecondary] == 200);
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::WearChange) == 0);
    rig.server.zone_log.clear();

    REQUIRE(conn.bandolier_activate(0));
    CHECK(id_at(conn, net::kSlotPrimary) == dagger.id);
    CHECK(id_at(conn, net::kSlotSecondary) == 0);
    CHECK(appearance.material[game::kSlotPrimary] == 2);
    CHECK(appearance.material[game::kSlotSecondary] == 0);
    REQUIRE(rig.until([&] {
        return MockEqServer::count(rig.server.zone_log, net::Op::WearChange) >= 2 &&
               MockEqServer::count(rig.server.zone_log, net::Op::Bandolier) >= 1;
    }));

    std::vector<const net::AppPacket*> sent;
    for (const auto& packet : rig.server.zone_log) {
        if (net::is_op(packet, net::Op::Bandolier) || net::is_op(packet, net::Op::WearChange)) sent.push_back(&packet);
    }
    // PcZoneClient::BandolierSwap @0x585aa0: SetHeldModel 7, then 8, then OP_Bandolier.
    REQUIRE(sent.size() == 3);
    REQUIRE(net::is_op(*sent[0], net::Op::WearChange));
    REQUIRE(net::is_op(*sent[1], net::Op::WearChange));
    REQUIRE(net::is_op(*sent[2], net::Op::Bandolier));
    auto swap = net::decode_bandolier(*sent[2]);
    REQUIRE(swap);
    CHECK(swap->action == net::kBandolierSet);
    CHECK(swap->number == 0);
    auto primary = net::decode_wear_change(*sent[0]);
    auto secondary = net::decode_wear_change(*sent[1]);
    REQUIRE(primary);
    REQUIRE(secondary);
    CHECK(primary->spawn_id == conn.world().player_id);
    CHECK(primary->slot == game::kSlotPrimary);
    CHECK(primary->armor.material == 2);
    CHECK(secondary->spawn_id == conn.world().player_id);
    CHECK(secondary->slot == game::kSlotSecondary);
    CHECK(secondary->armor.material == 0);
}

TEST_CASE("bandolier swap: with the general slots full, a returned weapon goes into the first bag that takes it") {
    // A player's bug report: swapping only worked with a free general slot, though whole giant bags stood empty. The
    // weapon goes where EQEmu's MoveItemToInventory puts it: the first free general slot, or the first free slot of
    // a bag that fits it by the server's test (CanItemFitInContainer), in slot order.
    MockConfig config = MockConfig::standard();
    auto& inv = config.characters[0].inventory;
    inv.push_back(make_item(1001, "Cudgel", 13));
    net::Item quiver = make_item(500, "Quiver", net::kSlotGeneral1);
    quiver.item_class = 1;
    quiver.bag_type = 2; // arrows only
    quiver.bag_slots = 10;
    quiver.bag_size = 4;
    inv.push_back(quiver);
    net::Item bag = make_item(501, "Giant Bag", net::kSlotGeneral1 + 1);
    bag.item_class = 1;
    bag.bag_slots = 10;
    bag.bag_size = 4;
    inv.push_back(bag);
    for (std::int16_t s = net::kSlotGeneral1 + 2; s <= net::kSlotGeneral10; ++s)
        inv.push_back(make_item(2000u + static_cast<std::uint32_t>(s), "Rock", s));
    Rig rig(std::move(config));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());

    REQUIRE(conn.bandolier_create(0, "1h"));
    rig.server.item_packet(net::kItemPacketCharInventory, make_item(1002, "Dagger", 13));
    REQUIRE(rig.until([&] { return id_at(conn, 13) == 1002; }));
    REQUIRE(conn.bandolier_create(1, "nodmg"));
    rig.server.item_packet(net::kItemPacketCharInventory, make_item(1001, "Cudgel", 13));
    REQUIRE(rig.until([&] { return id_at(conn, 13) == 1001; }));

    // No Dagger anywhere: the Cudgel comes off and goes past the quiver into the giant bag's first slot.
    REQUIRE(conn.bandolier_activate(1));
    CHECK(id_at(conn, 13) == 0);
    const net::Item* in_bag = conn.inventory().at(ItemSlot::possessions(net::kSlotGeneral1 + 1, 0));
    REQUIRE(in_bag);
    CHECK(in_bag->id == 1001);
    CHECK(conn.inventory().at(ItemSlot::possessions(net::kSlotGeneral1, 0)) == nullptr);
    // And back: the Cudgel is found in the bag (InventoryProfile::HasItem's order) and equipped again.
    REQUIRE(conn.bandolier_activate(0));
    CHECK(id_at(conn, 13) == 1001);
    CHECK(conn.inventory().at(ItemSlot::possessions(net::kSlotGeneral1 + 1, 0)) == nullptr);
    REQUIRE(rig.until([&] { return swaps_sent(rig.server, 1) == 1 && swaps_sent(rig.server, 0) == 1; }));
}

TEST_CASE("bandolier swap: the server's bag test, not the original's") {
    net::Item weapon;
    weapon.size = 3;
    weapon.item_type = 0;
    net::Item b;
    b.item_class = 1;
    b.bag_size = 4;
    b.bag_slots = 10;
    CHECK(game::server_bag_accepts(b, weapon));
    b.bag_size = 2;
    CHECK_FALSE(game::server_bag_accepts(b, weapon)); // too big
    b.bag_size = 4;
    b.bag_type = 2;
    CHECK_FALSE(game::server_bag_accepts(b, weapon)); // a quiver: arrows only
    weapon.item_type = 27;
    CHECK(game::server_bag_accepts(b, weapon));
    b.bag_type = 8;
    CHECK_FALSE(game::server_bag_accepts(b, weapon)); // a bandolier bag: small throwing items only
    weapon.item_type = 19;
    CHECK(game::server_bag_accepts(b, weapon));
    // A tradeskill bag (58) refuses a weapon by the original's CanGoInBag; EQEmu's MoveItemToInventory takes it.
    b.bag_type = 58;
    weapon.item_type = 0;
    CHECK(game::bag_fit(b, weapon) == 8);
    CHECK(game::server_bag_accepts(b, weapon));
    CHECK_FALSE(game::server_bag_accepts(weapon, weapon)); // not a bag
}
