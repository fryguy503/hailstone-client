// GameConnection::merchant_buy (CMerchantWnd::PurchasePageHandler::RequestGetItem @0x6f5be0): the cost against the
// purse, before the request goes out.
#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include "net/item_packets.h"
#include "net/merchant_packets.h"
#include "mq_test_world.h"

using namespace hailstone;

TEST_CASE("merchant_buy: a price times quantity past 2^31 copper is not taken as affordable") {
    // 300,000 platinum each, 8 of them: a signed 32-bit product overflowed (undefined) and wrapped negative, so the
    // can-afford check passed and the purchase went out. In 64 bits it is refused with the can't-afford message.
    mq_test::World w("hailstone_merchant_buy_cost_test");
    auto& c = *w.conn;
    auto& world = c.world_for_test();
    game::Spawn m = *world.player();
    m.id = 5001;
    m.name = "Merchant00";
    m.class_id = 41;
    m.is_npc = true;
    world.spawns[m.id] = m;
    c.open_merchant(m.id);
    REQUIRE(c.merchant());
    net::MerchantClick open;
    open.npc_id = m.id;
    open.player_id = world.player_id;
    open.rate = 1.0f;
    c.receive_zone_packet_for_test(net::encode(open));
    REQUIRE(c.merchant()->open);
    net::ItemPacket ip;
    ip.type = net::kItemPacketMerchant;
    ip.item.id = 1001;
    ip.item.name = "Costly Thing";
    ip.item.price = 300000000u;
    ip.item.slot.slot = 1;
    c.receive_zone_packet_for_test(net::encode_item_packet(false, ip));
    REQUIRE(c.merchant()->items.count(1));
    while (c.poll_chat()) {}
    c.merchant_buy(1, 8);
    CHECK(c.merchant()->pending == 0);
    CHECK(c.poll_chat().has_value()); // 12061, can't afford
    // One the purse holds goes out.
    ip.item.price = 10;
    ip.item.slot.slot = 2;
    c.receive_zone_packet_for_test(net::encode_item_packet(false, ip));
    c.merchant_buy(2, 1);
    CHECK(c.merchant()->pending == 1);
}

TEST_CASE("merchant buy and sell: the cash register plays and the line is the Merchants colour") {
    // FinishBuyingItem @0x6ef220 and FinishSellingItem @0x6f4200: EqSoundManager::WavePlay(0x8a) (BuyItem.wav,
    // soundassets 138) once the coin moved, and eqstr 1437 / 1361 in colour 0x114 (276, the Merchants filter).
    // A buy the purse cannot pay for (TakeMoney fails) stays silent, as the original's does.
    mq_test::World w("hailstone_merchant_cash_test");
    // The message templates the buy and sell replies print (the scratch game dir has none).
    {
        std::ofstream f(w.dir / "eqstr_us.txt");
        f << "1361 You receive%1 from %2 for the %3(s).\n"
          << "1364 %1 tells you, 'I am not interested.'\n"
          << "1437 You give%1 to %2.\n";
    }
    w.game->rescan(); // the index was built before the file existed
    w.conn->reload_game_files();
    auto& c = *w.conn;
    auto& world = c.world_for_test();
    game::Spawn m = *world.player();
    m.id = 5001;
    m.name = "Bob";
    m.class_id = 41;
    m.is_npc = true;
    m.position = world.player()->position;
    world.spawns[m.id] = m;
    c.open_merchant(m.id);
    net::MerchantClick open;
    open.npc_id = m.id;
    open.player_id = world.player_id;
    open.rate = 1.0f;
    c.receive_zone_packet_for_test(net::encode(open));
    REQUIRE(c.merchant());
    REQUIRE(c.merchant()->open);

    using A = net::GameConnectionTestAccess;
    A::coins(c) = net::Coins{1, 0, 0, 0}; // one platinum, 1000 copper
    while (c.poll_chat()) {}
    c.take_sound_cues();

    // A purchase: 500 copper leaves the purse, the line names the amount, one wave.
    net::MerchantBuy b;
    b.npc_id = m.id;
    b.player_id = world.player_id;
    b.item_slot = 1;
    b.quantity = 1;
    b.price = 500;
    c.receive_zone_packet_for_test(net::encode(b));
    auto line = c.poll_chat();
    REQUIRE(line);
    CHECK(line->text == "You give 5 gold to Bob.");
    CHECK(line->color == 276u);
    CHECK(A::coins(c).platinum == 0u);
    CHECK(A::coins(c).gold == 5u); // 500 copper of change from the platinum
    auto cues = c.take_sound_cues();
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].kind == net::SoundCue::Kind::Wave);
    CHECK(cues[0].spell == 0x8a);

    // The same purchase with an empty purse: the line still comes, the sound does not.
    A::coins(c) = net::Coins{};
    while (c.poll_chat()) {}
    c.receive_zone_packet_for_test(net::encode(b));
    line = c.poll_chat();
    REQUIRE(line);
    CHECK(line->color == 276u);
    CHECK(c.take_sound_cues().empty());

    // A sale: 25 copper is credited, 1361 names the amount and the item, one wave.
    net::Item sword;
    sword.id = 1300;
    sword.name = "Rusty Sword";
    sword.slot = net::ItemSlot::possessions(22);
    REQUIRE(A::inventory(c).put(net::ItemSlot::possessions(22), sword));
    while (c.poll_chat()) {}
    c.take_sound_cues();
    net::MerchantSell s;
    s.npc_id = m.id;
    s.slot = 22;
    s.quantity = 1;
    s.price = 25;
    c.receive_zone_packet_for_test(net::encode(s));
    line = c.poll_chat();
    REQUIRE(line);
    CHECK(line->text == "You receive 2 silver 5 copper from Bob for the Rusty Sword(s).");
    CHECK(line->color == 276u);
    CHECK(A::coins(c).silver == 2u);
    CHECK(A::coins(c).copper == 5u);
    cues = c.take_sound_cues();
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].kind == net::SoundCue::Kind::Wave);
    CHECK(cues[0].spell == 0x8a);

    // A refusal: 1364 in the refusal colour, no wave, and the empty purse stays empty.
    A::coins(c) = net::Coins{};
    while (c.poll_chat()) {}
    c.take_sound_cues();
    net::MerchantSell refused;
    refused.npc_id = m.id;
    refused.slot = -1;
    c.receive_zone_packet_for_test(net::encode(refused));
    line = c.poll_chat();
    REQUIRE(line);
    CHECK(line->text == "Bob tells you, 'I am not interested.'");
    CHECK(line->color == 275u);
    CHECK(c.take_sound_cues().empty());
    CHECK(A::coins(c).platinum == 0u);
    CHECK(A::coins(c).gold == 0u);
    CHECK(A::coins(c).silver == 0u);
    CHECK(A::coins(c).copper == 0u);

    // A trader purchase (OP_TraderShop action 10, method 0) goes through the same FinishBuyingItem.
    net::TraderBuy t;
    t.action = net::kTraderBuyItem;
    t.method = 0;
    t.sub_action = 0;
    t.trader_id = m.id;
    t.seller_name = "Trader Sam";
    t.item_name = "Rusty Sword";
    t.item_id = 1300;
    t.price = 100;
    t.quantity = 1;
    A::coins(c) = net::Coins{0, 0, 0, 500};
    while (c.poll_chat()) {}
    c.take_sound_cues();
    c.receive_zone_packet_for_test(net::encode(t));
    line = c.poll_chat();
    REQUIRE(line);
    CHECK(line->text == "You give 1 gold to Bob.");
    CHECK(line->color == 276u);
    CHECK(A::coins(c).platinum == 0u);
    CHECK(A::coins(c).gold == 0u);
    CHECK(A::coins(c).silver == 0u);
    CHECK(A::coins(c).copper == 400u);
    cues = c.take_sound_cues();
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].kind == net::SoundCue::Kind::Wave);
    CHECK(cues[0].spell == 0x8a);
    while (c.poll_chat()) {} // the "You successfully purchased ..." line of the same reply

    // The same purchase with an empty purse: the line still comes, the sound does not.
    A::coins(c) = net::Coins{};
    c.receive_zone_packet_for_test(net::encode(t));
    line = c.poll_chat();
    REQUIRE(line);
    CHECK(line->color == 276u);
    CHECK(c.take_sound_cues().empty());
    CHECK(A::coins(c).platinum == 0u);
    CHECK(A::coins(c).gold == 0u);
    CHECK(A::coins(c).silver == 0u);
    CHECK(A::coins(c).copper == 0u);
    while (c.poll_chat()) {} // the "You successfully purchased ..." line of the same reply
}
