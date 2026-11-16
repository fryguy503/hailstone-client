#include <doctest/doctest.h>

#include <algorithm>

#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/evolving_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

constexpr std::uint64_t source_id = 0xfedcba9876543210ull, destination_id = 0x8123456789abcdefull,
                        other_id = 0x8000000000000001ull;

Item evolving(std::uint64_t uid, std::uint32_t item_id, std::int16_t slot) {
    auto item = make_item(item_id, "Evolving " + std::to_string(item_id), slot);
    item.is_evolving = 1;
    item.instance_id = static_cast<std::uint32_t>(uid);
    item.parcel_item_id = static_cast<std::uint32_t>(uid >> 32);
    item.evolving.evolve_level = 2;
    item.evolving.evolve_max_level = 5;
    item.evolving.progress = 40.5;
    return item;
}

MockConfig evolving_config() {
    auto config = MockConfig::standard();
    config.characters[0].inventory = {evolving(source_id, 10, 24), evolving(destination_id, 20, 25),
                                      evolving(other_id, 30, 26)};
    return config;
}

EvolvingTransferDetails preview(std::uint64_t to = destination_id, std::uint32_t compatibility = 100) {
    EvolvingTransferDetails details;
    details.source_id = source_id;
    details.destination_id = to;
    details.compatibility = compatibility;
    details.max_transfer_level = 4;
    // Projections can carry different templates and zero instance IDs; never use them as request identifiers.
    details.source = evolving(0, 11, 0);
    details.destination = evolving(0, 21, 0);
    return details;
}

void select_pair(GameConnection& conn, std::uint64_t to = destination_id) {
    conn.open_evolving_transfer();
    REQUIRE(conn.select_evolving_transfer(true, source_id));
    REQUIRE(conn.select_evolving_transfer(false, to));
}

std::vector<EvolvingRequest> requests(const Rig& rig) {
    std::vector<EvolvingRequest> result;
    for (const auto& packet : rig.server.zone_log)
        if (packet.opcode == to_wire(Op::EvolveItem))
            if (auto request = decode_evolving_request(packet)) result.push_back(*request);
    return result;
}

} // namespace

TEST_CASE("evolving transfer: the two request IDs retain all 64 bits at the RoF2 offsets") {
    const auto packet = encode(EvolvingRequest{EvolvingAction::Details, source_id, destination_id});
    CHECK(packet.opcode == 0x7cfb);
    REQUIRE(packet.data.size() == 24);
    const std::vector<std::byte> expected = {
        std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0x10}, std::byte{0x32}, std::byte{0x54}, std::byte{0x76}, std::byte{0x98}, std::byte{0xba}, std::byte{0xdc}, std::byte{0xfe},
        std::byte{0xef}, std::byte{0xcd}, std::byte{0xab}, std::byte{0x89}, std::byte{0x67}, std::byte{0x45}, std::byte{0x23}, std::byte{0x81}};
    CHECK(packet.data == expected);
    const auto request = decode_evolving_request(packet);
    REQUIRE(request);
    CHECK(request->source_id == source_id);
    CHECK(request->destination_id == destination_id);
    CHECK(evolving_item_id(evolving(source_id, 10, 24)) == source_id);
    auto ordinary = evolving(source_id, 10, 24);
    ordinary.is_evolving = 0;
    CHECK(evolving_item_id(ordinary) == 0);
    for (std::size_t size = 0; size < packet.data.size(); ++size) {
        auto short_packet = packet;
        short_packet.data.resize(size);
        CHECK_FALSE(decode_evolving_request(short_packet));
    }
}

TEST_CASE("evolving transfer: the unaligned 30-byte details header precedes two independent item projections") {
    auto packet = encode(preview());
    ByteReader reader(packet.data);
    CHECK(reader.le<std::uint32_t>() == 2);
    CHECK(reader.le<std::uint64_t>() == source_id);
    CHECK(reader.le<std::uint64_t>() == destination_id);
    CHECK(reader.le<std::uint32_t>() == 100);
    CHECK(reader.le<std::uint32_t>() == 4);
    CHECK(reader.le<std::uint8_t>() == 1);
    CHECK(reader.le<std::uint8_t>() == 1);
    CHECK(reader.position() == 30);
    const auto details = decode_evolving_details(packet);
    REQUIRE(details);
    REQUIRE(details->source);
    REQUIRE(details->destination);
    CHECK(details->source->id == 11);
    CHECK(details->destination->id == 21);
    CHECK(details->source_id == source_id);
    for (std::size_t size = 0; size < packet.data.size(); ++size) {
        auto short_packet = packet;
        short_packet.data.resize(size);
        CHECK_FALSE(decode_evolving_details(short_packet));
    }
    auto invalid = packet;
    invalid.data[28] = std::byte{2};
    CHECK_FALSE(decode_evolving_details(invalid));
    packet.data.push_back(std::byte{0});
    CHECK_FALSE(decode_evolving_details(packet));
    auto absent = preview();
    absent.source.reset();
    absent.destination.reset();
    CHECK(encode(absent).data.size() == 30);
    CHECK(decode_evolving_details(encode(absent)).has_value());
    // Action 1 shares action 0's 24-byte envelope; action 0 itself is net/item_packets.h's decoder.
    CHECK(decode_evolving_open(encode(EvolvingUpdate{EvolvingAction::Open, 0, 0, 0})));
    CHECK_FALSE(decode_evolving_open(encode(EvolvingUpdate{EvolvingAction::Update, source_id, 83, 1})));
    CHECK_FALSE(decode_evolving_open(encode(EvolvingRequest{EvolvingAction::Details, source_id, destination_id})));
}

TEST_CASE("mock evolving transfer: server preview enables an explicit single transfer, with no optimistic item replacement") {
    Rig rig(evolving_config());
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    select_pair(conn);
    CHECK(conn.evolving_transfer().pending);
    CHECK_FALSE(conn.can_transfer_evolving());
    REQUIRE(rig.until([&] { return requests(rig).size() == 1; }));
    CHECK(requests(rig)[0].action == EvolvingAction::Details);
    conn.receive_zone_packet_for_test(encode(preview()));
    CHECK(conn.can_transfer_evolving());
    REQUIRE(conn.evolving_transfer().details);
    CHECK(conn.evolving_transfer().details->max_transfer_level == 4);
    const auto revision = conn.evolving_transfer().revision;
    CHECK_FALSE(conn.request_evolving_transfer(revision - 1));
    CHECK(conn.request_evolving_transfer(revision));
    CHECK_FALSE(conn.request_evolving_transfer(revision));
    CHECK(conn.evolving_transfer().submitted);
    conn.receive_zone_packet_for_test(encode(EvolvingUpdate{EvolvingAction::Update, source_id, 41, 1}));
    CHECK(conn.evolving_transfer().submitted); // periodic progress is not a transfer completion acknowledgement
    CHECK_FALSE(conn.request_evolving_transfer(conn.evolving_transfer().revision));
    CHECK(conn.inventory().at(ItemSlot::possessions(24))->id == 10);
    REQUIRE(rig.until([&] { return requests(rig).size() == 2; }));
    CHECK(requests(rig)[1].action == EvolvingAction::Transfer);
    CHECK(requests(rig)[1].source_id == source_id);
    CHECK(requests(rig)[1].destination_id == destination_id);
    conn.receive_zone_packet_for_test(encode_item_packet(false, ItemPacket{kItemPacketTrade, evolving(100, 11, 24)}));
    rig.step();
    CHECK_FALSE(conn.evolving_transfer().submitted);
    CHECK(conn.evolving_transfer().source_id == 0);
    CHECK(conn.inventory().at(ItemSlot::possessions(24))->id == 11);
}

TEST_CASE("mock evolving transfer: unknown, same-item, incompatible and incomplete selections cannot send Transfer") {
    Rig rig(evolving_config());
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    conn.open_evolving_transfer();
    CHECK_FALSE(conn.select_evolving_transfer(true, 123));
    REQUIRE(conn.select_evolving_transfer(true, source_id));
    CHECK_FALSE(conn.select_evolving_transfer(false, source_id));
    CHECK_FALSE(conn.request_evolving_transfer(conn.evolving_transfer().revision));
    REQUIRE(conn.select_evolving_transfer(false, destination_id));
    conn.receive_zone_packet_for_test(encode(preview(destination_id, 0)));
    CHECK_FALSE(conn.can_transfer_evolving());
    REQUIRE(conn.select_evolving_transfer(false, destination_id));
    auto missing = preview();
    missing.destination.reset();
    conn.receive_zone_packet_for_test(encode(missing));
    CHECK_FALSE(conn.can_transfer_evolving());
    CHECK_FALSE(conn.request_evolving_transfer(conn.evolving_transfer().revision));
    REQUIRE(conn.select_evolving_transfer(false, destination_id));
    auto zero_level = preview();
    zero_level.max_transfer_level = 0; // a valid transfer need not cross a destination tier
    conn.receive_zone_packet_for_test(encode(zero_level));
    CHECK(conn.can_transfer_evolving());
}

TEST_CASE("mock evolving transfer: delayed previews for other pairs and closed windows are ignored") {
    Rig rig(evolving_config());
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    select_pair(conn);
    REQUIRE(conn.select_evolving_transfer(false, other_id));
    conn.receive_zone_packet_for_test(encode(preview())); // old pair, not the current pair
    CHECK(conn.evolving_transfer().pending);
    CHECK_FALSE(conn.can_transfer_evolving());
    conn.receive_zone_packet_for_test(encode(preview(other_id)));
    REQUIRE(conn.can_transfer_evolving());
    REQUIRE(conn.select_evolving_transfer(false, destination_id));
    conn.receive_zone_packet_for_test(encode(preview()));
    REQUIRE(conn.can_transfer_evolving());
    REQUIRE(conn.select_evolving_transfer(false, destination_id));
    conn.close_evolving_transfer();
    conn.receive_zone_packet_for_test(encode(preview()));
    CHECK_FALSE(conn.evolving_transfer().shown);
    select_pair(conn);
    conn.receive_zone_packet_for_test(encode(preview()));
    CHECK(conn.can_transfer_evolving());
    // A fresh server-open notification resets selections even if the window is already shown.
    conn.receive_zone_packet_for_test(encode(EvolvingUpdate{EvolvingAction::Open, 0, 0, 0}));
    CHECK(conn.evolving_transfer().shown);
    CHECK(conn.evolving_transfer().source_id == 0);
    CHECK(conn.evolving_transfer().destination_id == 0);
    conn.disconnect();
    CHECK_FALSE(conn.evolving_transfer().shown);
    CHECK_FALSE(conn.can_transfer_evolving());
    CHECK(conn.evolving_transfer_items().empty());
}

TEST_CASE("mock evolving transfer: a progress tick keeps the approval; a changed item asks again") {
    Rig rig(evolving_config());
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    select_pair(conn);
    conn.receive_zone_packet_for_test(encode(preview()));
    REQUIRE(conn.can_transfer_evolving());
    REQUIRE(rig.until([&] { return requests(rig).size() == 1; }));
    const auto revision = conn.evolving_transfer().revision;
    conn.receive_zone_packet_for_test(encode(EvolvingUpdate{EvolvingAction::Update, source_id, 60, 1}));
    CHECK(conn.inventory().at(ItemSlot::possessions(24))->evolving.progress == 60.0);
    CHECK(conn.inventory().at(ItemSlot::possessions(24))->evolving.activated == 1);
    rig.step();
    rig.run_for(std::chrono::milliseconds(20));
    CHECK(conn.evolving_transfer().revision == revision); // an open confirmation stays valid
    CHECK_FALSE(conn.evolving_transfer().pending);
    CHECK(conn.can_transfer_evolving());
    CHECK(requests(rig).size() == 1); // no new Details request
    // A replaced original (a new template or level) does invalidate it, at once and in the next frame.
    auto evolved = evolving(source_id, 10, 24);
    evolved.evolving.evolve_level = 3;
    conn.receive_zone_packet_for_test(encode_item_packet(false, ItemPacket{kItemPacketTrade, evolved}));
    CHECK_FALSE(conn.can_transfer_evolving());
    CHECK_FALSE(conn.request_evolving_transfer(revision));
    rig.step();
    CHECK(conn.evolving_transfer().pending);
    CHECK(conn.evolving_transfer().revision != revision);
    REQUIRE(rig.until([&] { return requests(rig).size() == 2; }));
    CHECK(requests(rig)[1].action == EvolvingAction::Details);
    conn.receive_zone_packet_for_test(encode(preview()));
    CHECK(conn.can_transfer_evolving());
    conn.receive_zone_packet_for_test(encode_item_packet(false, ItemPacket{kItemPacketTrade, evolving(source_id, 99, 24)}));
    CHECK_FALSE(conn.can_transfer_evolving());
    CHECK_FALSE(conn.request_evolving_transfer(conn.evolving_transfer().revision));
}

TEST_CASE("mock evolving transfer: bank and socketed items are excluded while bag contents can be selected") {
    auto config = evolving_config();
    auto bank = evolving(400, 40, 1);
    bank.slot.type = kInvBank;
    auto bag = make_item(50, "Bag", 27);
    bag.item_class = 1;
    bag.bag_slots = 10;
    bag.contents.push_back({0, evolving(500, 50, 0)});
    config.characters[0].inventory.push_back(bank);
    config.characters[0].inventory.push_back(bag);
    config.characters[0].inventory[0].contents.push_back({0, evolving(600, 60, 0)});
    Rig rig(std::move(config));
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    conn.receive_zone_packet_for_test(encode(EvolvingUpdate{EvolvingAction::Open, 0, 0, 0}));
    CHECK(conn.evolving_transfer().shown);
    const auto items = conn.evolving_transfer_items();
    CHECK(items.size() == 4);
    CHECK(conn.select_evolving_transfer(true, 500));
    CHECK_FALSE(conn.select_evolving_transfer(false, 400));
    CHECK_FALSE(conn.select_evolving_transfer(false, 600));
}

// EQEmu refuses a transfer with a red chat line only and changes no items (DoEvolveTransferXP), so the wait has to end
// on the player's next selection.
TEST_CASE("mock evolving transfer: after a refused transfer a new selection starts over") {
    Rig rig(evolving_config());
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    select_pair(conn);
    conn.receive_zone_packet_for_test(encode(preview()));
    REQUIRE(conn.request_evolving_transfer(conn.evolving_transfer().revision));
    REQUIRE(rig.until([&] { return requests(rig).size() == 2; }));
    CHECK(conn.evolving_transfer().status.find("Select an item to start again") != std::string::npos);
    rig.step();
    rig.step();
    CHECK(conn.evolving_transfer().submitted); // no items changed: still waiting
    CHECK(conn.select_evolving_transfer(true, source_id));
    CHECK_FALSE(conn.evolving_transfer().submitted);
    CHECK(conn.evolving_transfer().pending);
    CHECK(conn.evolving_transfer().destination_id == destination_id);
    REQUIRE(rig.until([&] { return requests(rig).size() == 3; }));
    CHECK(requests(rig)[2].action == EvolvingAction::Details);
    conn.receive_zone_packet_for_test(encode(preview()));
    CHECK(conn.can_transfer_evolving());
    CHECK(conn.request_evolving_transfer(conn.evolving_transfer().revision));
    REQUIRE(rig.until([&] { return requests(rig).size() == 4; }));
    CHECK(requests(rig)[3].action == EvolvingAction::Transfer);
}

// update(), hs.ItemExpTransfer and each item display ask every frame; the item list and the snapshot comparison are
// built again only when the inventory changes.
TEST_CASE("mock evolving transfer: an unchanged inventory costs no rebuild per frame") {
    Rig rig(evolving_config());
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    select_pair(conn);
    conn.receive_zone_packet_for_test(encode(preview()));
    REQUIRE(conn.can_transfer_evolving());
    const auto before = GameConnectionTestAccess::evolving_cache_work(conn);
    for (int frame = 0; frame < 20; ++frame) {
        rig.step();
        CHECK(conn.can_transfer_evolving());
        CHECK(conn.evolving_transfer_items().size() == 3);
    }
    CHECK(GameConnectionTestAccess::evolving_cache_work(conn) == before);
    // A changed original is still noticed: one build and one comparison, then a fresh preview request.
    conn.receive_zone_packet_for_test(encode_item_packet(false, ItemPacket{kItemPacketTrade, evolving(source_id, 99, 24)}));
    CHECK_FALSE(conn.can_transfer_evolving());
    rig.step();
    CHECK(conn.evolving_transfer().pending);
    const auto after = GameConnectionTestAccess::evolving_cache_work(conn);
    CHECK(after.first == before.first + 1);
    CHECK(after.second == before.second + 1);
    for (int frame = 0; frame < 5; ++frame) rig.step();
    CHECK(GameConnectionTestAccess::evolving_cache_work(conn) == after);
}
