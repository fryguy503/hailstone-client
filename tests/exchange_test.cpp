// /exchange and /unequip (MQ2Exchange's commands, game/exchange.h, GameConnection::exchange_command and
// unequip_command): the slot names and words, and the moves against the mock server.
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "game/exchange.h"
#include "mock_server/harness.h"
#include "net/item_packets.h"

using namespace hailstone;
using namespace hailstone::test;
using net::ItemSlot;

namespace {

std::uint32_t id_at(const net::GameConnection& conn, ItemSlot slot) {
    const net::Item* it = conn.inventory().at(slot);
    return it ? it->id : 0;
}

std::vector<std::string> chat_lines(net::GameConnection& conn) {
    std::vector<std::string> out;
    while (auto e = conn.poll_chat()) out.push_back(e->text);
    return out;
}

bool said(const std::vector<std::string>& lines, const std::string& text) {
    return std::find(lines.begin(), lines.end(), text) != lines.end();
}

constexpr std::int16_t kGeneral2 = net::kSlotGeneral1 + 1, kGeneral3 = net::kSlotGeneral1 + 2,
                       kGeneral4 = net::kSlotGeneral1 + 3;

} // namespace

TEST_CASE("exchange: MacroQuest's slot names, numbers and aliases; GetArg's quoted words") {
    CHECK(game::exchange_slot("mainhand") == std::int16_t{13});
    CHECK(game::exchange_slot("OffHand") == std::int16_t{14});
    CHECK(game::exchange_slot("face") == std::int16_t{3});
    CHECK(game::exchange_slot("ammo") == std::int16_t{22});
    CHECK(game::exchange_slot("hands") == std::int16_t{12});
    CHECK(game::exchange_slot("hand") == std::int16_t{12});
    CHECK(game::exchange_slot("legs") == std::int16_t{18});
    CHECK(game::exchange_slot("leg") == std::int16_t{18});
    CHECK(game::exchange_slot("ranged") == std::int16_t{11});
    CHECK(game::exchange_slot("range") == std::int16_t{11});
    CHECK(game::exchange_slot("0") == std::int16_t{0});
    CHECK(game::exchange_slot("22") == std::int16_t{22});
    CHECK_FALSE(game::exchange_slot("23")); // a general slot
    CHECK_FALSE(game::exchange_slot("pack1"));
    CHECK_FALSE(game::exchange_slot("primary"));
    CHECK_FALSE(game::exchange_slot(""));
    CHECK_FALSE(game::exchange_slot("-1"));
    CHECK(game::exchange_words("\"Mirrored Mask\" face") == std::vector<std::string>{"Mirrored Mask", "face"});
    CHECK(game::exchange_words("\"Mirrored Mask\" \"face\"") == std::vector<std::string>{"Mirrored Mask", "face"});
    CHECK(game::exchange_words("  Dagger   mainhand ") == std::vector<std::string>{"Dagger", "mainhand"});
    CHECK(game::exchange_words("\"Open quote") == std::vector<std::string>{"Open quote"});
    CHECK(game::exchange_words("").empty());
}

TEST_CASE("exchange: /exchange swaps an item into a worn slot, the old one into its place; /unequip takes it off") {
    MockConfig config = MockConfig::standard();
    auto& inv = config.characters[0].inventory;
    inv = {make_item(1002, "Rations", net::kSlotGeneral1, 5)}; // in place of the standard Short Sword (1001)
    inv.push_back(make_item(1101, "Cudgel", net::kSlotPrimary));
    inv.push_back(make_item(1102, "Dagger", kGeneral2));
    net::Item bag = make_item(500, "Backpack", kGeneral3);
    bag.item_class = 1;
    bag.bag_slots = 4;
    bag.bag_size = 4;
    bag.contents.push_back({0, make_item(1103, "Short Sword", kGeneral3)});
    inv.push_back(bag);
    Rig rig(std::move(config));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    for (int i = 0; i < 4; ++i) conn.drop_on_doll(); // the mock's character zones holding coin
    const ItemSlot primary = ItemSlot::possessions(net::kSlotPrimary);
    REQUIRE(id_at(conn, primary) == 1101u);
    REQUIRE(id_at(conn, ItemSlot::possessions(kGeneral3, 0)) == 1103u);
    chat_lines(conn);

    // From a general slot: the Dagger on, the Cudgel where the Dagger was. The name in any case.
    conn.send_command("/exchange dagger mainhand");
    CHECK(id_at(conn, primary) == 1102u);
    CHECK(id_at(conn, ItemSlot::possessions(kGeneral2)) == 1101u);
    CHECK_FALSE(conn.inventory().cursor());
    // From a bag, quoted, as E3 sends it: the Short Sword on, the Dagger into the bag's slot.
    conn.send_command("/exchange \"Short Sword\" \"mainhand\"");
    CHECK(id_at(conn, primary) == 1103u);
    CHECK(id_at(conn, ItemSlot::possessions(kGeneral3, 0)) == 1102u);
    CHECK_FALSE(conn.inventory().cursor());
    // By id: the Cudgel back on.
    conn.send_command("/exchange 1101 mainhand");
    CHECK(id_at(conn, primary) == 1101u);
    CHECK(id_at(conn, ItemSlot::possessions(kGeneral2)) == 1103u);
    chat_lines(conn);

    // Refusals leave everything where it was.
    conn.send_command("/exchange Dagger offhand"); // its slots are the primary only
    CHECK(said(chat_lines(conn), "Exchange: Dagger cannot go in the offhand slot."));
    CHECK(id_at(conn, ItemSlot::possessions(kGeneral3, 0)) == 1102u);
    CHECK_FALSE(conn.inventory().cursor());
    conn.send_command("/exchange Dagger elbow");
    CHECK(said(chat_lines(conn), "Exchange: elbow slot not found"));
    conn.send_command("/exchange \"Golden Idol\" mainhand");
    CHECK(said(chat_lines(conn), "Exchange: Couldn't find Golden Idol in your inventory"));
    conn.send_command("/exchange Cudgel mainhand"); // worn already, no copy in the packs
    CHECK(said(chat_lines(conn), "Exchange: Item is already in a worn slot."));
    conn.send_command("/exchange Dagger");
    CHECK(chat_lines(conn).back().starts_with("Usage: /exchange"));
    CHECK(id_at(conn, primary) == 1101u);

    // /unequip: into the first free general slot.
    conn.send_command("/unequip mainhand");
    CHECK(id_at(conn, primary) == 0u);
    CHECK(id_at(conn, ItemSlot::possessions(kGeneral4)) == 1101u);
    CHECK_FALSE(conn.inventory().cursor());
    conn.send_command("/unequip mainhand");
    CHECK(said(chat_lines(conn), "Unequip: There is nothing in the mainhand slot to unequip"));
    conn.send_command("/unequip elbow");
    CHECK(said(chat_lines(conn), "Unequip: elbow slot not found"));
    // Into an empty worn slot: nothing comes back out.
    conn.send_command("/exchange Cudgel mainhand");
    CHECK(id_at(conn, primary) == 1101u);
    CHECK(id_at(conn, ItemSlot::possessions(kGeneral4)) == 0u);
    CHECK_FALSE(conn.inventory().cursor());

    // The help and the slot list.
    conn.send_command("/exchange help");
    auto lines = chat_lines(conn);
    CHECK(said(lines, "Exchange commands:"));
    conn.send_command("/exchange help list");
    lines = chat_lines(conn);
    CHECK(said(lines, "Exchange slots:"));
    CHECK(said(lines, " mainhand | 13"));
    CHECK(said(lines, " ammo | 22"));
    conn.send_command("/exchange list");
    CHECK(said(chat_lines(conn), " charm | 0"));
}

TEST_CASE("exchange: an item on the cursor, or no room, stops it") {
    MockConfig config = MockConfig::standard();
    auto& inv = config.characters[0].inventory;
    inv = {make_item(1002, "Rations", net::kSlotGeneral1, 5)};
    inv.push_back(make_item(1101, "Cudgel", net::kSlotPrimary));
    inv.push_back(make_item(1102, "Dagger", net::kSlotGeneral1 + 1));
    Rig rig(std::move(config));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    for (int i = 0; i < 4; ++i) conn.drop_on_doll();
    rig.server.item_packet(net::kItemPacketLimbo, make_item(2001, "Summoned Bread", net::kSlotCursor));
    REQUIRE(rig.until([&] { return conn.inventory().cursor() != nullptr; }));
    chat_lines(conn);
    conn.send_command("/exchange Dagger mainhand");
    CHECK(said(chat_lines(conn), "Exchange: Your mouse pointer must be clear to move an item."));
    conn.send_command("/unequip mainhand");
    CHECK(said(chat_lines(conn), "Unequip: Your mouse pointer must be clear to move an item."));
    CHECK(id_at(conn, ItemSlot::possessions(net::kSlotPrimary)) == 1101u);
    conn.send_command("/autoinventory");
    REQUIRE_FALSE(conn.inventory().cursor());
    // Every general slot full and no bag: /unequip puts it back on and says so.
    for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s)
        if (!conn.inventory().at(ItemSlot::possessions(s)))
            rig.server.item_packet(net::kItemPacketCharInventory, make_item(3000u + static_cast<std::uint32_t>(s),
                                                                            "Rock", s));
    REQUIRE(rig.until([&] { return conn.inventory().at(ItemSlot::possessions(net::kSlotGeneral10)) != nullptr; }));
    chat_lines(conn);
    conn.send_command("/unequip mainhand");
    CHECK(said(chat_lines(conn), "Unequip: No room in any bags to unequip."));
    CHECK(id_at(conn, ItemSlot::possessions(net::kSlotPrimary)) == 1101u);
    CHECK_FALSE(conn.inventory().cursor());
}
