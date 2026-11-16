// Generic item metadata may add a loot question, independently of the stock
// NO DROP preference. Exercise the connection and its actual outgoing wire.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/byte_io.h"
#include "game/item_options.h"
#include "game/server_ext.h"
#include "mock_server/harness.h"
#include "net/item_packets.h"
#include "net/opcodes.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

struct ItemMetadataExtension final : game::ServerExtension {
    struct Visit {
        std::uint32_t root = 0;
        std::vector<std::uint32_t> descendants;
        bool complete = false;
    };
    std::set<std::uint32_t> warned_items;
    bool always_question = false;
    bool return_empty = false;
    std::string question = "Metadata <binding> & 'consequence'\nContinue looting?";
    mutable std::vector<Visit> visits;

    std::optional<std::string> loot_confirmation(const GameConnection&, const Item& root,
                                                 std::span<const std::uint32_t> descendants,
                                                 bool complete) const override {
        visits.push_back({root.id, {descendants.begin(), descendants.end()}, complete});
        if (always_question || warned_items.contains(root.id) ||
            std::any_of(descendants.begin(), descendants.end(),
                        [&](std::uint32_t id) { return warned_items.contains(id); }))
            return question;
        return return_empty ? std::optional(std::string()) : std::nullopt;
    }
};

ConnectionOptions metadata_options(const std::shared_ptr<ItemMetadataExtension>& extension) {
    auto options = quick_options();
    options.plugins = extension;
    return options;
}

Item sample_item(std::uint32_t id = 70001) {
    Item item;
    item.id = id;
    item.name = "Metadata loot fixture " + std::to_string(id);
    item.serial = std::to_string(id);
    item.slot = ItemSlot::corpse(23);
    item.stack_count = 1;
    item.no_drop = item.no_rent = 255;
    item.classes = item.races = 0xffff;
    return item;
}

AppPacket money_response(std::uint8_t response) {
    // MoneyOnCorpse's response byte, three padding bytes, and four zero coins.
    std::vector<std::byte> bytes(20, std::byte{0});
    bytes[0] = static_cast<std::byte>(response);
    return {to_wire(Op::MoneyOnCorpse), std::move(bytes)};
}

void install_loot_item(GameConnection& connection, Item item) {
    ItemPacket packet;
    packet.type = kItemPacketLoot;
    packet.item = std::move(item);
    connection.receive_zone_packet_for_test(encode_item_packet(false, packet));
    REQUIRE(connection.loot_session());
    REQUIRE(connection.loot_session()->items.contains(23));
}

std::uint32_t open_corpse(Rig& rig, bool own = false) {
    REQUIRE(rig.to_zone());
    auto& connection = rig.conn;
    connection.destroy_cursor(); // the standard profile starts with cursor money
    REQUIRE(connection.world().player());
    game::Spawn corpse;
    corpse.id = 4500;
    corpse.name = own ? "gEnNrO's corpse01" : "a_metadata_fixture's corpse01";
    corpse.is_corpse = true;
    corpse.is_npc = !own;
    corpse.heights.avatar_height = 2.0f;
    corpse.position = connection.world().player()->position + glm::vec3(2.0f, 0.0f, 0.0f);
    connection.world_for_test().spawns[corpse.id] = corpse;
    connection.loot(corpse.id);
    connection.receive_zone_packet_for_test(money_response(1));
    REQUIRE(connection.loot_session());
    REQUIRE(connection.loot_session()->open);
    return corpse.id;
}

void no_loot_sent(Rig& rig) {
    rig.run_for(std::chrono::milliseconds(15));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::LootItem) == 0);
}

void require_metadata_question(const GameConnection& connection, const ItemMetadataExtension& extension) {
    REQUIRE(connection.confirmation());
    CHECK(connection.confirmation()->id == kConfirmLootNoDrop);
    CHECK(connection.confirmation()->text == extension.question);
    CHECK(connection.confirmation()->plain_text);
    CHECK(connection.confirmation()->timeout_ms == game::kLootConfirmMs);
}

void require_sent_loot(Rig& rig, std::uint32_t corpse, bool automatic) {
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootItem) == 1; }));
    for (const auto& packet : rig.server.zone_log) {
        if (packet.opcode != to_wire(Op::LootItem)) continue;
        REQUIRE(packet.data.size() == 20);
        ByteReader reader(packet.data);
        CHECK(reader.le<std::uint32_t>() == corpse);
        CHECK(reader.le<std::uint32_t>() == rig.conn.world().player_id);
        CHECK(reader.le<std::uint16_t>() == 23);
        CHECK(reader.le<std::uint16_t>() == 0);
        CHECK(reader.le<std::int32_t>() == (automatic ? 1 : 0));
        CHECK(reader.le<std::uint32_t>() == 0);
        CHECK(reader.empty());
    }
}

Item mixed_tree() {
    Item root = sample_item();
    root.contents.push_back({2, sample_item(70002)}); // root augment
    Item bag = sample_item(70003);
    bag.item_class = 1;
    bag.bag_slots = 10;
    Item content = sample_item(70004);
    content.contents.push_back({4, sample_item(70005)}); // an augment of a bag's item
    bag.contents.push_back({0, std::move(content)});
    root.contents.push_back({7, std::move(bag)});
    root.contents.push_back({9, sample_item(70006)});
    return root;
}

Item wide_tree(std::size_t descendants) {
    Item root = sample_item();
    root.item_class = 1;
    std::uint32_t id = 80000;
    while (descendants > 0) {
        Item bag = sample_item(id++);
        bag.item_class = 1;
        --descendants; // the container itself is a descendant too
        const auto children = std::min<std::size_t>(1000, descendants);
        for (std::size_t index = 0; index < children; ++index)
            bag.contents.push_back({static_cast<std::uint32_t>(index), sample_item(id++)});
        descendants -= children;
        root.contents.push_back({static_cast<std::uint32_t>(root.contents.size()), std::move(bag)});
    }
    return root;
}

Item deep_tree(unsigned depth) {
    Item root = sample_item();
    Item* parent = &root;
    for (unsigned level = 1; level <= depth; ++level) {
        parent->contents.push_back({0, sample_item(90000 + level)});
        parent = &parent->contents.front().item;
    }
    return root;
}

} // namespace

TEST_CASE("Item metadata loot: an extra root question is mandatory under all NO DROP preferences") {
    for (int preference : {0, 1, 2}) {
        INFO(preference);
        auto extension = std::make_shared<ItemMetadataExtension>();
        extension->warned_items.insert(70001);
        Rig rig(MockConfig::standard(), metadata_options(extension));
        const auto corpse = open_corpse(rig);
        auto& connection = rig.conn;
        connection.set_client_option_for_session(game::ClientOption::LootNoDrop, preference);
        install_loot_item(connection, sample_item()); // fully droppable without the extra policy

        connection.loot_item(23, false);
        require_metadata_question(connection, *extension);
        no_loot_sent(rig);
        connection.answer_confirmation(false);
        CHECK_FALSE(connection.confirmation());
        no_loot_sent(rig);

        connection.loot_item(23, false);
        require_metadata_question(connection, *extension);
        no_loot_sent(rig);
        REQUIRE(extension->visits.size() == 2);
        CHECK(extension->visits.front().root == 70001);
        CHECK(extension->visits.front().descendants.empty());
        CHECK(extension->visits.front().complete);
        connection.answer_confirmation(true);
        require_sent_loot(rig, corpse, false);
    }
}

TEST_CASE("Item metadata loot: augments and nested container items join the root's confirmation") {
    std::uint32_t warned = 0;
    SUBCASE("a root augment") { warned = 70002; }
    SUBCASE("a container descendant") { warned = 70003; }
    SUBCASE("an item inside a container") { warned = 70004; }
    SUBCASE("an augment inside a container item") { warned = 70005; }
    auto extension = std::make_shared<ItemMetadataExtension>();
    extension->warned_items.insert(warned);
    Rig rig(MockConfig::standard(), metadata_options(extension));
    const auto corpse = open_corpse(rig);
    auto& connection = rig.conn;
    connection.set_client_option_for_session(game::ClientOption::LootNoDrop, 2);
    install_loot_item(connection, mixed_tree());
    connection.loot_item(23, true);
    require_metadata_question(connection, *extension);
    REQUIRE(extension->visits.size() == 1);
    CHECK(extension->visits.front().root == 70001);
    CHECK(extension->visits.front().descendants == std::vector<std::uint32_t>{70002, 70003, 70004, 70005, 70006});
    CHECK(extension->visits.front().complete);
    no_loot_sent(rig);
    connection.answer_confirmation(true);
    require_sent_loot(rig, corpse, true);
}

TEST_CASE("Item metadata loot: the player's own corpse bypasses metadata and stock questions") {
    for (int preference : {0, 1, 2}) {
        INFO(preference);
        auto extension = std::make_shared<ItemMetadataExtension>();
        extension->always_question = true;
        Rig rig(MockConfig::standard(), metadata_options(extension));
        const auto corpse = open_corpse(rig, true);
        auto& connection = rig.conn;
        connection.set_client_option_for_session(game::ClientOption::LootNoDrop, preference);
        Item item = mixed_tree();
        item.no_drop = 0;
        item.classes = 0; // would be unusable and NO DROP on another corpse
        install_loot_item(connection, std::move(item));
        connection.loot_item(23, false);
        CHECK(extension->visits.empty());
        CHECK_FALSE(connection.confirmation());
        require_sent_loot(rig, corpse, false);
    }
}

TEST_CASE("Item metadata loot: absent or empty extension text cannot bypass the native NO DROP warning") {
    bool empty = false;
    SUBCASE("no extension decision") {}
    SUBCASE("an empty extension string") { empty = true; }
    for (int preference : {0, 1}) {
        INFO(preference);
        auto extension = std::make_shared<ItemMetadataExtension>();
        extension->return_empty = empty;
        Rig rig(MockConfig::standard(), metadata_options(extension));
        open_corpse(rig);
        auto& connection = rig.conn;
        connection.set_client_option_for_session(game::ClientOption::LootNoDrop, preference);
        Item item = sample_item();
        item.no_drop = 0;
        item.classes = 0;
        const auto name = item.name;
        install_loot_item(connection, std::move(item));
        connection.loot_item(23, false);
        REQUIRE(connection.confirmation());
        CHECK(connection.confirmation()->text == name + " is a NO DROP item, are you sure you wish to loot it?");
        CHECK_FALSE(connection.confirmation()->plain_text);
        REQUIRE(extension->visits.size() == 1);
        no_loot_sent(rig);
        connection.answer_confirmation(false);
        no_loot_sent(rig);
    }
}

TEST_CASE("Item metadata loot: descendant count is bounded and reports an omitted tail") {
    std::size_t descendants = 0;
    SUBCASE("exactly 4096 descendants are complete") { descendants = 4096; }
    SUBCASE("the 4097th descendant is omitted") { descendants = 4097; }
    auto extension = std::make_shared<ItemMetadataExtension>();
    extension->always_question = true;
    Rig rig(MockConfig::standard(), metadata_options(extension));
    open_corpse(rig);
    rig.conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 2);
    // Five containers, each with <=1000 children and only two nesting levels:
    // this exercises the real ItemPacket decoder, not an oversized root count.
    install_loot_item(rig.conn, wide_tree(descendants));
    rig.conn.loot_item(23, true);
    require_metadata_question(rig.conn, *extension);
    REQUIRE(extension->visits.size() == 1);
    const auto& visit = extension->visits.front();
    REQUIRE(visit.descendants.size() == 4096);
    CHECK(visit.complete == (descendants == 4096));
    CHECK(visit.descendants.front() == 80000);
    CHECK(visit.descendants.back() == 84095);
    CHECK(std::find(visit.descendants.begin(), visit.descendants.end(), 84096) == visit.descendants.end());
    no_loot_sent(rig);
    rig.conn.answer_confirmation(false);
    no_loot_sent(rig);
}

TEST_CASE("Item metadata loot: synthetic deeper trees report the depth-eight traversal boundary") {
    unsigned depth = 0;
    SUBCASE("depth eight is complete") { depth = 8; }
    SUBCASE("depth nine is incomplete") { depth = 9; }
    auto extension = std::make_shared<ItemMetadataExtension>();
    extension->always_question = true;
    Rig rig(MockConfig::standard(), metadata_options(extension));
    open_corpse(rig);
    rig.conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 2);
    install_loot_item(rig.conn, sample_item());
    // The wire decoder intentionally rejects depth >4. Test the separate
    // metadata walker with an in-memory tree on this non-const test connection.
    auto& shown = const_cast<Item&>(rig.conn.loot_session()->items.at(23));
    shown = deep_tree(depth);
    rig.conn.loot_item(23, true);
    require_metadata_question(rig.conn, *extension);
    REQUIRE(extension->visits.size() == 1);
    const auto& visit = extension->visits.front();
    CHECK(visit.descendants == std::vector<std::uint32_t>{90001, 90002, 90003, 90004, 90005, 90006, 90007, 90008});
    CHECK(visit.complete == (depth == 8));
    no_loot_sent(rig);
    rig.conn.answer_confirmation(false);
    no_loot_sent(rig);
}

TEST_CASE("Item metadata loot: an approval cannot loot a replaced item or a closing corpse") {
    auto extension = std::make_shared<ItemMetadataExtension>(); extension->always_question = true;
    Rig rig(MockConfig::standard(), metadata_options(extension)); open_corpse(rig);
    install_loot_item(rig.conn, sample_item());
    rig.conn.loot_item(23, true); require_metadata_question(rig.conn, *extension);
    SUBCASE("same item ID with a replaced instance") {
        auto replacement = sample_item(); replacement.serial = "replacement";
        install_loot_item(rig.conn, std::move(replacement));
    }
    SUBCASE("closing the current session") { rig.conn.end_loot(); }
    rig.conn.answer_confirmation(true);
    no_loot_sent(rig);
}

TEST_CASE("Item metadata loot: an extra-only stack chooses quantity before the binding approval") {
    bool shift = false, ctrl = false;
    SUBCASE("choose an ordinary quantity") {}
    SUBCASE("shift takes the full stack") { shift = true; }
    SUBCASE("control takes one") { ctrl = true; }
    auto extension = std::make_shared<ItemMetadataExtension>(); extension->always_question = true;
    Rig rig(MockConfig::standard(), metadata_options(extension)); open_corpse(rig);
    auto item = sample_item(); item.stack_count = 10; item.max_stack = 20;
    install_loot_item(rig.conn, std::move(item));
    rig.conn.loot_item(23, false, shift, ctrl);
    if (!shift && !ctrl) {
        REQUIRE(rig.conn.quantity_request());
        CHECK(rig.conn.quantity_request()->op == QuantityOp::ExtensionLoot);
        CHECK_FALSE(rig.conn.confirmation());
        CHECK(rig.conn.quantity_request()->max == 10);
        no_loot_sent(rig);
        rig.conn.finish_quantity(3);
    } else CHECK_FALSE(rig.conn.quantity_request());
    REQUIRE(rig.conn.confirmation());
    CHECK(rig.conn.confirmation()->text.find(extension->question) != std::string::npos);
    CHECK(rig.conn.confirmation()->text.find("Quantity: " + std::to_string(shift ? 10 : ctrl ? 1 : 3)) != std::string::npos);
    no_loot_sent(rig);
    rig.conn.answer_confirmation(true);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootItem) == 1; }));
    for (const auto& packet : rig.server.zone_log) if (packet.opcode == to_wire(Op::LootItem)) {
        const auto request = decode_loot_item(packet); REQUIRE(request);
        CHECK(request->unknown16 == (shift ? 10u : ctrl ? 1u : 3u));
    }
}

TEST_CASE("Item metadata loot: the final quantity approval is invalidated by instance replacement") {
    auto extension = std::make_shared<ItemMetadataExtension>(); extension->always_question = true;
    Rig rig(MockConfig::standard(), metadata_options(extension)); open_corpse(rig);
    auto item = sample_item(); item.stack_count = 10; item.max_stack = 20;
    install_loot_item(rig.conn, item);
    rig.conn.loot_item(23, false); REQUIRE(rig.conn.quantity_request());
    rig.conn.finish_quantity(3); REQUIRE(rig.conn.confirmation());
    item.serial = "replacement"; install_loot_item(rig.conn, std::move(item));
    rig.conn.answer_confirmation(true); no_loot_sent(rig);
}

TEST_CASE("Item metadata loot: Loot All keeps an extra-only stack as a whole-stack request") {
    auto extension = std::make_shared<ItemMetadataExtension>(); extension->always_question = true;
    Rig rig(MockConfig::standard(), metadata_options(extension)); open_corpse(rig);
    rig.conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 0);
    auto item = sample_item(); item.stack_count = 10; item.max_stack = 20;
    install_loot_item(rig.conn, std::move(item));
    rig.conn.receive_zone_packet_for_test(money_response(6));
    rig.conn.loot_all(); require_metadata_question(rig.conn, *extension);
    CHECK_FALSE(rig.conn.quantity_request());
    rig.conn.answer_confirmation(true);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootItem) == 1; }));
    for (const auto& packet : rig.server.zone_log) if (packet.opcode == to_wire(Op::LootItem))
        CHECK(decode_loot_item(packet)->unknown16 == 0);
}

TEST_CASE("Item metadata loot: mixed Loot All asks each binding and preserves the ordinary warning") {
    auto extension = std::make_shared<ItemMetadataExtension>();
    extension->warned_items = {70001, 70002};
    Rig rig(MockConfig::standard(), metadata_options(extension));
    const auto corpse = open_corpse(rig);
    auto& c = rig.conn;
    c.set_client_option_for_session(game::ClientOption::LootAllConfirm, 0);
    c.set_client_option_for_session(game::ClientOption::LootNoDrop, 0);
    auto put = [&](std::int16_t slot, std::uint32_t id, bool bag, bool nodrop) {
        auto item = sample_item(id); item.slot.slot = slot;
        item.item_class = bag ? 1 : 0; item.no_drop = nodrop ? 0 : 255;
        if (id == 70002) { item.stack_count = 10; item.max_stack = 20; }
        ItemPacket packet; packet.type = kItemPacketLoot; packet.item = std::move(item);
        c.receive_zone_packet_for_test(encode_item_packet(false, packet));
    };
    put(23, 70001, true, false); put(24, 70003, false, false); put(25, 70002, false, true);
    c.receive_zone_packet_for_test(money_response(6));
    c.loot_all(); require_metadata_question(c, *extension); no_loot_sent(rig);
    c.loot_all(); // no duplicate question while the serial workflow is active
    c.answer_confirmation(false);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootItem) == 1; }));
    REQUIRE_FALSE(c.confirmation());
    LootItem answer; answer.lootee = corpse; answer.looter = c.world().player_id; answer.slot = 24; answer.auto_loot = 1;
    c.receive_zone_packet_for_test(encode(answer));
    REQUIRE(c.confirmation());
    CHECK(c.confirmation()->text.find(extension->question) != std::string::npos);
    CHECK(c.confirmation()->text.find("NO DROP") != std::string::npos);
    CHECK(c.confirmation()->plain_text);
    CHECK_FALSE(c.quantity_request());
    c.answer_confirmation(true);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootItem) == 2; }));
    std::vector<std::uint16_t> requested;
    for (const auto& packet : rig.server.zone_log) if (packet.opcode == to_wire(Op::LootItem)) {
        const auto request = decode_loot_item(packet); REQUIRE(request);
        requested.push_back(request->slot);
        CHECK(request->unknown16 == 0); // Loot All still requests the whole stack
    }
    CHECK(requested == std::vector<std::uint16_t>{24, 25});
    answer.slot = 25; c.receive_zone_packet_for_test(encode(answer));
    CHECK(c.loot_session()); // declined bag remains available
    CHECK_FALSE(c.confirmation());
}
