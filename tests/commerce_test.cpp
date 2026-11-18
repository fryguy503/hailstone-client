// Merchants, trade and NPC dialogue: byte-exact packet layouts (the NMS
// server's rof2_structs.h / rof2.cpp and what the original client is seen
// sending), and the client's price and money maths, which follow the original's
// ItemBase::ValueSellMerchant @0x7bcf80, PcClient_TakeMoney
// @0x5819f0 and FormatMoneyWords @0x4dc5f0. Vectors from the original exe can
// be added later.

#include <doctest/doctest.h>

#include <cstring>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "game/item_links.h"
#include "game/merchant.h"
#include "net/item_packets.h"
#include "net/merchant_packets.h"
#include "net/opcodes.h"
#include "net/trade_packets.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

std::vector<std::uint8_t> bytes_of(const AppPacket& p) {
    std::vector<std::uint8_t> out;
    for (std::byte b : p.data) out.push_back(std::to_integer<std::uint8_t>(b));
    return out;
}

AppPacket packet(Op op, const std::vector<std::uint8_t>& b) {
    std::vector<std::byte> d;
    for (auto v : b) d.push_back(static_cast<std::byte>(v));
    return AppPacket(to_wire(op), std::move(d));
}

Item sellable(std::uint32_t value) {
    Item it;
    it.id = 13005;
    it.name = "Iron Ration";
    it.value = value;
    it.no_rent = 1;   // not Temporary
    it.no_drop = 255; // tradeable
    it.unknowna1 = -1;
    it.unknowna4 = -1;
    return it;
}

} // namespace

TEST_CASE("merchant and trade opcodes are RoF2's (patch_RoF2.conf)") {
    CHECK(to_wire(Op::ShopRequest) == 0x4fed);
    CHECK(to_wire(Op::ShopPlayerBuy) == 0x0ddd);
    CHECK(to_wire(Op::ShopPlayerSell) == 0x791b);
    CHECK(to_wire(Op::ShopEnd) == 0x30a8);
    CHECK(to_wire(Op::ShopEndConfirm) == 0x3196);
    CHECK(to_wire(Op::ShopDelItem) == 0x724f);
    CHECK(to_wire(Op::TradeRequest) == 0x77b5);
    CHECK(to_wire(Op::TradeRequestAck) == 0x14bf);
    CHECK(to_wire(Op::TradeAcceptClick) == 0x69e2);
    CHECK(to_wire(Op::TradeCoins) == 0x4206);
    CHECK(to_wire(Op::CancelTrade) == 0x354c);
    CHECK(to_wire(Op::FinishTrade) == 0x3993);
    CHECK(to_wire(Op::TradeBusy) == 0x5505);
    CHECK(to_wire(Op::MoveCoin) == 0x0bcf);
}

TEST_CASE("OP_ShopRequest (MerchantClick_Struct, 24 bytes)") {
    MerchantClick m;
    m.npc_id = 0x1234;
    m.player_id = 7;
    AppPacket p = encode(m);
    CHECK(p.opcode == 0x4fed);
    CHECK(bytes_of(p) == std::vector<std::uint8_t>{0x34, 0x12, 0, 0, 7, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                   0xff, 0xff, 0xff, 0xff});
    // The server's answer: command 1, rate 1/(BuyCostMod × CalcPriceMod) = 0.8f (0x3f4ccccd), tabs 1.
    auto r = decode_merchant_click(packet(Op::ShopRequest, {0x34, 0x12, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0xcd, 0xcc, 0x4c,
                                                            0x3f, 1, 0, 0, 0, 0, 0, 0, 0}));
    REQUIRE(r);
    CHECK(r->npc_id == 0x1234);
    CHECK(r->command == 1);
    CHECK(r->rate == doctest::Approx(0.8f));
    CHECK(r->tab_display == 1);
    CHECK_FALSE(decode_merchant_click(packet(Op::ShopRequest, std::vector<std::uint8_t>(23))));
}

TEST_CASE("OP_ShopPlayerBuy (Merchant_Sell_Struct, 32 bytes)") {
    MerchantBuy b;
    b.npc_id = 100;
    b.player_id = 5;
    b.item_slot = 6;
    b.quantity = 2;
    CHECK(bytes_of(encode(b)) == std::vector<std::uint8_t>{100, 0, 0, 0, 5, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0,
                                                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    // The server's acknowledgement (ENCODE(OP_ShopPlayerBuy)): price at +24.
    auto r = decode_merchant_buy(packet(Op::ShopPlayerBuy, {100, 0, 0, 0, 5, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0,
                                                            0, 0, 0, 0, 0x2e, 1, 0, 0, 0, 0, 0, 0}));
    REQUIRE(r);
    CHECK(r->quantity == 2);
    CHECK(r->price == 302);
}

TEST_CASE("OP_ShopPlayerSell (Merchant_Purchase_Struct with a typeless slot, 20 bytes)") {
    MerchantSell s;
    s.npc_id = 100;
    s.slot = 25;
    s.sub = 3;
    s.aug = -1;
    s.quantity = 4;
    CHECK(bytes_of(encode(s)) == std::vector<std::uint8_t>{100, 0, 0, 0, 25, 0, 3, 0, 0xff, 0xff, 0xff, 0xff, 4, 0, 0, 0,
                                                           0, 0, 0, 0});
    auto r = decode_merchant_sell(packet(Op::ShopPlayerSell, {100, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                              0xff, 1, 0, 0, 0, 0x26, 1, 0, 0}));
    REQUIRE(r);
    CHECK(r->slot == -1); // refused
    CHECK(r->price == 294);
}

TEST_CASE("OP_ShopEnd and OP_ShopDelItem") {
    CHECK(bytes_of(encode_shop_end(100, 5)) == std::vector<std::uint8_t>{100, 0, 0, 0, 5, 0, 0, 0});
    CHECK(encode_shop_end(1, 2).opcode == 0x30a8);
    auto d = decode_merchant_del_item(packet(Op::ShopDelItem, {100, 0, 0, 0, 5, 0, 0, 0, 9, 0, 0, 0}));
    REQUIRE(d);
    CHECK(d->item_slot == 9);
    CHECK(bytes_of(encode(*d)) == std::vector<std::uint8_t>{100, 0, 0, 0, 5, 0, 0, 0, 9, 0, 0, 0});
}

TEST_CASE("OP_ShopSendParcel (Parcel_Struct with a typeless slot, 224 bytes)") {
    ParcelSend s;
    s.npc_id = 100;
    s.slot = 24;
    s.sub = 2;
    s.quantity = 5;
    s.to = "Friend";
    s.note = "Hi";
    AppPacket p = encode(s);
    CHECK(p.opcode == 0x3a5d);
    auto b = bytes_of(p);
    REQUIRE(b.size() == kParcelSendSize);
    CHECK(std::vector<std::uint8_t>(b.begin(), b.begin() + 20) ==
          std::vector<std::uint8_t>{100, 0, 0, 0, 24, 0, 2, 0, 0xff, 0xff, 0xff, 0xff, 5, 0, 0, 0, 0, 0, 0, 0});
    CHECK(std::string(reinterpret_cast<const char*>(&b[20])) == "Friend");
    CHECK(std::string(reinterpret_cast<const char*>(&b[84])) == "Hi");
    ParcelSend m; // money: no slot, money_flag 1, the copper
    m.money_flag = 1;
    m.quantity = 1234;
    auto mb = bytes_of(encode(m));
    CHECK(mb[4] == 0xff);
    CHECK(mb[5] == 0xff);
    CHECK(mb[16] == 1);
    CHECK(mb[12] == 0xd2);
}

TEST_CASE("trade packets") {
    AppPacket req = encode_trade_request({300, 5});
    CHECK(req.opcode == 0x77b5);
    CHECK(bytes_of(req) == std::vector<std::uint8_t>{0x2c, 1, 0, 0, 5, 0, 0, 0});
    AppPacket ack = encode_trade_request_ack({300, 5});
    CHECK(ack.opcode == 0x14bf);
    // The server's Ack for an NPC: {us, the NPC}.
    auto a = decode_trade_request(packet(Op::TradeRequestAck, {5, 0, 0, 0, 0x2c, 1, 0, 0}));
    REQUIRE(a);
    CHECK(a->to_id == 5);
    CHECK(a->from_id == 300);

    CHECK(bytes_of(encode(TradeAccept{300, 1, 0, 0, 0})) == std::vector<std::uint8_t>{0x2c, 1, 0, 0, 1, 0, 0, 0});
    auto acc = decode_trade_accept(packet(Op::TradeAcceptClick, {0x2c, 1, 0, 0, 1, 0xcc, 0xcc, 0xcc}));
    REQUIRE(acc);
    CHECK(acc->ready == 1);

    CHECK(bytes_of(encode(CancelTrade{300, 5})) == std::vector<std::uint8_t>{0x2c, 1, 0, 0, 5, 0, 0, 0});

    auto c = decode_trade_coins(packet(Op::TradeCoins, {5, 0, 0, 0, 3, 0, 0, 0, 12, 0, 0, 0}));
    REQUIRE(c);
    CHECK(c->type == 3);
    CHECK(c->amount == 12);
    CHECK(bytes_of(encode(*c)) == std::vector<std::uint8_t>{5, 0, 0, 0, 3, 0, 0, 0, 12, 0, 0, 0});

    // Coin into the trade the original's way: inventory → cursor, cursor → trade.
    CHECK(bytes_of(encode(MoveCoin{kCoinInventory, kCoinCursor, kCoinGold, kCoinGold, 3})) ==
          std::vector<std::uint8_t>{1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0});
    CHECK(bytes_of(encode(MoveCoin{kCoinCursor, kCoinTrade, kCoinGold, kCoinGold, 3})) ==
          std::vector<std::uint8_t>{0, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0});

    AppPacket busy = encode(TradeBusy{300, 5, 1, 0, 0, 0});
    CHECK(busy.opcode == 0x5505);
    CHECK(bytes_of(busy) == std::vector<std::uint8_t>{0x2c, 1, 0, 0, 5, 0, 0, 0, 1, 0, 0, 0});
    auto bz = decode_trade_busy(packet(Op::TradeBusy, {5, 0, 0, 0, 0x2c, 1, 0, 0, 1, 0xef, 0xff, 0xff}));
    REQUIRE(bz);
    CHECK(bz->from_id == 300);
}

TEST_CASE("merchant sell price: ItemBase::ValueSellMerchant") {
    Item it = sellable(151);
    CHECK(game::sell_value(it, 1.0f, 1) == 151);
    CHECK(game::sell_value(it, 1.0f, 2) == 302);
    // (int)(value / rate + 0.5) per unit, then × count.
    CHECK(game::sell_value(it, 1.05f, 1) == 144);  // 143.81 + 0.5
    CHECK(game::sell_value(it, 1.05f, 3) == 432);
    CHECK(game::sell_value(sellable(7), 0.9f, 1) == 8); // 7.78 + 0.5
    // 10,000,000 copper and more: rounded to whole gold.
    CHECK(game::sell_value(sellable(5000033), 1.0f, 2) == 10000100);
    CHECK(game::sell_value(sellable(4999999), 1.0f, 2) == 9999998);
    // The rounding's + 50 wraps past INT_MAX, then divides as a signed number (core/wrap_math.h).
    CHECK(game::sell_value(sellable(1073741820), 1.0f, 2) == -2147483600);
    // A rate that puts the unit price past int32 (tiny, NaN, an infinity's 0 aside) is x86's integer indefinite,
    // 0x80000000, as the original's conversion gives, not an undefined cast; then times the count, wrapping.
    CHECK(game::sell_value(it, 1e-30f, 1) == INT32_MIN);
    CHECK(game::sell_value(it, -1e-30f, 1) == INT32_MIN);
    CHECK(game::sell_value(it, std::numeric_limits<float>::quiet_NaN(), 1) == INT32_MIN);
    CHECK(game::sell_value(it, 1e-30f, 2) == 0); // 0x80000000 * 2 wraps to 0
    CHECK(game::sell_value(it, std::numeric_limits<float>::infinity(), 1) == 0); // 151 / inf + 0.5 truncates to 0
    CHECK(game::sell_value(it, -2.0f, 1) == -75); // in range: -75.5 + 0.5 truncates, as the original
    // Nothing for: rate 0, no value, Temporary, No Trade, attuned, ornamented, real estate.
    CHECK(game::sell_value(it, 0.0f, 1) == 0);
    CHECK(game::sell_value(sellable(0), 1.0f, 1) == 0);
    Item temp = it;
    temp.no_rent = 0;
    CHECK(game::sell_value(temp, 1.0f, 1) == 0);
    Item notrade = it;
    notrade.no_drop = 0;
    CHECK(game::sell_value(notrade, 1.0f, 1) == 0);
    Item attuned = it;
    attuned.attuned = 1;
    CHECK(game::sell_value(attuned, 1.0f, 1) == 0);
    Item orn = it;
    orn.ornament_icon = 1234;
    CHECK(game::sell_value(orn, 1.0f, 1) == 0);
    Item house = it;
    house.unknowna4 = 5;
    CHECK(game::sell_value(house, 1.0f, 1) == 0);
    // A common item counts its augments' value; a bag's contents do not count,
    // and a Temporary or No Trade content makes it worthless.
    Item weapon = sellable(1000);
    weapon.contents.push_back({0, sellable(250)});
    CHECK(game::item_value(weapon) == 1250);
    CHECK(game::sell_value(weapon, 1.0f, 1) == 1250);
    Item bag = sellable(400);
    bag.item_class = 1;
    bag.contents.push_back({2, sellable(900)});
    CHECK(game::item_value(bag) == 400);
    CHECK(game::sell_value(bag, 1.0f, 1) == 400);
    bag.contents[0].item.no_drop = 0;
    CHECK(game::sell_value(bag, 1.0f, 1) == 0);
    bag.contents[0].item.no_drop = 255;
    bag.contents[0].item.no_rent = 0;
    CHECK(game::sell_value(bag, 1.0f, 1) == 0);
    // The client's own refusals before it asks.
    CHECK(game::sell_refusal(it) == 0);
    CHECK(game::sell_refusal(notrade) == 12062);
    Item full = sellable(400);
    full.item_class = 1;
    full.contents.push_back({0, sellable(1)});
    CHECK(game::sell_refusal(full) == 12064);
    CHECK(game::sell_refusal(weapon) == 5360);
}

TEST_CASE("buying: PcClient_TakeMoney takes the smallest coins first") {
    Coins c{99, 8, 18, 12};
    REQUIRE(game::take_money(c, 10));
    CHECK((c.platinum == 99 && c.gold == 8 && c.silver == 18 && c.copper == 2));
    // 302 copper from 100 platinum: one platinum broken into gold, gold into silver, silver into copper.
    Coins p{100, 0, 0, 0};
    REQUIRE(game::take_money(p, 302));
    CHECK((p.platinum == 99 && p.gold == 6 && p.silver == 9 && p.copper == 8));
    CHECK(game::total_copper(p) == 100000 - 302);
    // Whole platinum first when the amount is large.
    Coins q{10, 0, 3, 0};
    REQUIRE(game::take_money(q, 2530));
    CHECK((q.platinum == 7 && q.gold == 5 && q.silver == 0 && q.copper == 0));
    CHECK(game::total_copper(q) == 10030 - 2530);
    // Not enough, or negative: nothing taken.
    Coins r{0, 1, 0, 5};
    CHECK_FALSE(game::take_money(r, 106));
    CHECK((r.gold == 1 && r.copper == 5));
    CHECK_FALSE(game::take_money(r, -1));
    CHECK(game::take_money(r, 0));
}

TEST_CASE("buying: a coin count of 2^31 or more counts as negative, as the original's signed counts") {
    // The purse is checked as the loop spends it: signed. An unsigned check let 0xFFFFFFFF copper (-1) through, and
    // the loop then had nothing to take from and never ended.
    Coins c{0, 0, 0, 0xFFFFFFFFu};
    CHECK_FALSE(game::take_money(c, 100));
    CHECK(c.copper == 0xFFFFFFFFu);
    Coins s{0, 0, 0xFFFFFFFFu, 0};
    CHECK_FALSE(game::take_money(s, 100));
    Coins g{0, 0x80000000u, 0, 0};
    CHECK_FALSE(game::take_money(g, 100));
    // A negative count beside enough others: taken from the others, and the loop ends.
    Coins m{1, 0, 0, 0xFFFFFFFFu};
    CHECK(game::take_money(m, 100));
    CHECK(static_cast<std::int32_t>(m.platinum) * 1000 + static_cast<std::int32_t>(m.gold) * 100 +
              static_cast<std::int32_t>(m.silver) * 10 + static_cast<std::int32_t>(m.copper) == 999 - 100);
}

TEST_CASE("FormatMoneyWords") {
    CHECK(game::money_words(302) == " 3 gold 2 copper");
    CHECK(game::money_words(1001) == " 1 platinum 1 copper");
    CHECK(game::money_words(12345) == " 12 platinum 3 gold 4 silver 5 copper");
    CHECK(game::money_words(0).empty());
}

TEST_CASE("EQEmu say links are item links to item 0xF423F; clicking sends OP_ItemLinkClick") {
    // say_link.cpp GenerateLinkBody for RoF2 (NMS: 8-digit ids): the phrase id in augment 1.
    std::string body = "0";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", 0xF423Fu);
    body += buf;
    for (std::uint32_t a : {12u, 0u, 0u, 0u, 0u, 0u}) {
        std::snprintf(buf, sizeof buf, "%08X", a);
        body += buf;
    }
    body += "0" "0000" "00" "00000" "00000000";
    std::string line = "Tabure Ahendle says, 'A [\x12" + body + "member of the Steel Warriors\x12]?'";
    auto segs = game::split_links(line);
    REQUIRE(segs.size() == 3);
    REQUIRE(segs[1].link);
    CHECK(segs[1].link->text == "member of the Steel Warriors");
    CHECK(segs[1].link->item_id == 0xF423F);
    ItemLinkClick c = segs[1].link->click_request();
    CHECK(c.augments[0] == 12);
    AppPacket p = encode(c);
    CHECK(p.opcode == 0x4cef);
    REQUIRE(p.data.size() == kItemLinkClickSize);
    auto back = decode_item_link_click(p);
    REQUIRE(back);
    CHECK(back->item_id == 0xF423F);
    CHECK(back->augments[0] == 12);
    CHECK(back->flags == 4);
}

TEST_CASE("alternate currencies and point merchants in EQEmu's layouts") {
    std::vector<AltCurrencyEntry> entries{{31, 22000, 1500, 1000, true}, {32, 22001, 1501, 500, false}};
    auto pop = bytes_of(encode_alt_currency_populate(entries));
    REQUIRE(pop.size() == 8 + 2 * 25); // RoF2 AltCurrencyPopulateEntry_Struct is 25 bytes, packed
    auto a = decode_alt_currency(encode_alt_currency_populate(entries));
    REQUIRE(a);
    REQUIRE(a->populate.size() == 2);
    CHECK(a->populate[0].id == 31);
    CHECK(a->populate[0].item_id == 22000);
    CHECK(a->populate[1].stack_size == 500);
    CHECK_FALSE(a->populate[1].shown);
    auto upd = bytes_of(encode_alt_currency_update({31, 77}));
    REQUIRE(upd.size() == 88);
    auto u = decode_alt_currency(encode_alt_currency_update({31, 77}));
    REQUIRE(u);
    REQUIRE(u->update);
    CHECK(u->update->amount == 77);
    CHECK(!decode_alt_currency(packet(Op::AltCurrency, {8, 0, 0, 0, 5, 0, 0, 0}))); // a count past the end

    CHECK(bytes_of(encode_alt_currency_reclaim(31, 5, false)) ==
          std::vector<std::uint8_t>{31, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0});
    CHECK(bytes_of(encode_alt_currency_merchant_request(42)) == std::vector<std::uint8_t>{42, 0, 0, 0});

    std::string reply = "31|1|31|2^Shiny Ring|1001|25|0|1|65535|65535^Old Boot|1002|3|0|1|1|2";
    std::vector<std::uint8_t> rb(reply.begin(), reply.end());
    rb.push_back(0);
    auto l = decode_point_merchant_list(packet(Op::AltCurrencyMerchantReply, rb));
    REQUIRE(l);
    CHECK(l->currency == 31);
    REQUIRE(l->items.size() == 2);
    CHECK(l->items[0].name == "Shiny Ring");
    CHECK(l->items[0].item_id == 1001);
    CHECK(l->items[0].cost == 25);
    CHECK(l->items[1].classes == 2);
    std::string none = "31|1|31|0";
    auto empty = decode_point_merchant_list(packet(Op::AltCurrencyMerchantReply, {none.begin(), none.end()}));
    REQUIRE(empty);
    CHECK(empty->items.empty());

    CHECK(bytes_of(encode_alt_currency_purchase(42, 1001)).size() == 12);
    auto sel = bytes_of(encode_alt_currency_select(42, 24, 2));
    REQUIRE(sel.size() == 80); // RoF2 AltCurrencySelectItem_Struct, checked exactly
    CHECK(sel[4] == 24);
    CHECK(sel[6] == 2);
    // EQEmu's own reply struct: the name at +10, the cost at +76.
    std::vector<std::uint8_t> q(88, 0);
    std::memcpy(q.data() + 10, "Old Boot", 8);
    q[76] = 3;
    auto quote = decode_alt_currency_quote(packet(Op::AltCurrencySellSelection, q));
    REQUIRE(quote);
    CHECK(quote->item_name == "Old Boot");
    CHECK(quote->cost == 3);
    CHECK(bytes_of(encode_alt_currency_sell(42, 24, 2, 1, 3)).size() == 20);
}

TEST_CASE("adventure merchant: request, the goods, purchase, sale and its echo") {
    AppPacket rq = encode_adventure_merchant_request(1, 0x42);
    CHECK(rq.opcode == to_wire(Op::AdventureMerchantRequest));
    REQUIRE(rq.data.size() == 8); // AdventureMerchant_Struct
    CHECK(static_cast<std::uint8_t>(rq.data[4]) == 0x42);

    // Handle_OP_AdventureMerchantRequest: u8 count, then "^name|id|price|theme|stackable|lore|races|classes".
    std::string text = "^Cloak of Guk|1234|150|1|0|1|65535|65535^Ration|13005|5|0|1|0|1|2";
    AppPacket list(to_wire(Op::AdventureMerchantResponse), {std::byte{2}});
    for (char c : text) list.data.push_back(static_cast<std::byte>(c));
    list.data.push_back(std::byte{0});
    auto l = decode_adventure_merchant_list(list);
    REQUIRE(l);
    REQUIRE(l->size() == 2);
    CHECK((*l)[0].name == "Cloak of Guk");
    CHECK((*l)[0].cost == 150);
    CHECK((*l)[0].theme == 1);
    CHECK((*l)[0].lore);
    CHECK((*l)[1].stackable);
    CHECK((*l)[1].classes == 2);

    AppPacket buy = encode_adventure_merchant_purchase(1, 0x42, 1234);
    REQUIRE(buy.data.size() == 16); // Adventure_Purchase_Struct, size checked exactly
    std::uint32_t item = 0;
    std::memcpy(&item, buy.data.data() + 8, 4);
    CHECK(item == 1234);

    AppPacket sell = encode_adventure_merchant_sell(0x42, 25, -1, 3);
    CHECK(sell.opcode == to_wire(Op::AdventureMerchantSell));
    REQUIRE(sell.data.size() == 24); // RoF2 Adventure_Sell_Struct
    auto echo = decode_adventure_merchant_sale(sell);
    REQUIRE(echo);
    CHECK(echo->npc == 0x42);
    CHECK(echo->slot == 25);
    CHECK(echo->sub == -1);
    CHECK(echo->charges == 3);

    auto alt = decode_alt_currency_sale(encode_alt_currency_sell(7, 30, 2, 1, 99));
    REQUIRE(alt);
    CHECK(alt->slot == 30);
    CHECK(alt->sub == 2);
    CHECK(alt->price == 99);

    // The RoF2 sell-back price: (price + 1) × rate / 100, at most the price, nothing for an item it does not buy.
    CHECK(adventure_sell_price(100, 50, 1) == 50);
    CHECK(adventure_sell_price(100, 150, 1) == 100);
    CHECK(adventure_sell_price(100, 50, 0) == 0);
}

TEST_CASE("coin the server adds unannounced: quest rewards and AddMoneyToPP's split") {
    std::vector<std::byte> q(68);
    q[4] = std::byte{7};   // our id
    q[20] = std::byte{5};  // copper
    q[32] = std::byte{2};  // platinum
    auto r = decode_quest_reward(AppPacket(to_wire(Op::Sound), q));
    REQUIRE(r);
    CHECK(r->target_id == 7);
    CHECK(r->coins.copper == 5);
    CHECK(r->coins.platinum == 2);
    std::vector<std::byte> c(16);
    c[8] = std::byte{3}; // gold
    auto cr = decode_cash_reward(AppPacket(to_wire(Op::CashReward), c));
    REQUIRE(cr);
    CHECK(cr->coins.gold == 3);

    Coins coins{1, 9, 9, 9};
    game::server_add_copper(coins, 1234); // 1 pp, 2 gp, 3 sp, 4 cp, each added to its own count
    CHECK(coins.platinum == 2);
    CHECK(coins.gold == 11);
    CHECK(coins.silver == 12);
    CHECK(coins.copper == 13);
}

TEST_CASE("merchant list: the Qty and Lvl columns (PurchasePageHandler::UpdateList)") {
    CHECK(game::merchant_quantity_text(-1) == "--");
    CHECK(game::merchant_quantity_text(0) == "--");
    CHECK(game::merchant_quantity_text(1) == "1");
    CHECK(game::merchant_quantity_text(20) == "20");
    CHECK(game::merchant_level_text(1) == "--");
    CHECK(game::merchant_level_text(-1) == "--");
    CHECK(game::merchant_level_text(2) == "2");
    net::Item sword;
    sword.req_level = 10;
    net::Item aug;
    aug.req_level = 35;
    sword.contents.push_back({0, aug});
    auto never = []() -> std::optional<std::int32_t> { return 99; };
    CHECK(game::merchant_list_level(sword, never) == 35); // the augments' highest too
    net::Item scroll;
    scroll.item_type = 20;
    scroll.req_level = 1;
    CHECK(game::merchant_list_level(scroll, [] { return std::optional<std::int32_t>(12); }) == 12);
    CHECK(game::merchant_list_level(scroll, [] { return std::optional<std::int32_t>(); }) == -1);
}

TEST_CASE("merchant list: Show only items I can use (PurchasePageHandler::UpdateList, +0x60 clear)") {
    auto unknown = [] { return false; };
    auto known = [] { return true; };
    net::Item sword;
    sword.classes = 1u << 0; // Warrior
    sword.races = 1u << 0;   // Human
    CHECK_FALSE(game::merchant_usable_hides(sword, 1, 1, unknown));
    CHECK(game::merchant_usable_hides(sword, 2, 1, unknown)); // a Cleric's class bit is not in the mask
    CHECK(game::merchant_usable_hides(sword, 1, 2, unknown)); // nor a Barbarian's race bit
    // the later races take bits 13..16 (0x6f7160), any other race 17
    sword.races = 1u << 12;
    CHECK_FALSE(game::merchant_usable_hides(sword, 1, 128, unknown)); // Iksar
    sword.races = 1u << 15;
    CHECK_FALSE(game::merchant_usable_hides(sword, 1, 522, unknown)); // Drakkin
    CHECK(game::merchant_usable_hides(sword, 1, 128, unknown));
    // bags and books are never left out
    net::Item bag = sword;
    bag.item_class = 1;
    bag.classes = bag.races = 0;
    CHECK_FALSE(game::merchant_usable_hides(bag, 1, 1, unknown));
    net::Item book = bag;
    book.item_class = 2;
    CHECK_FALSE(game::merchant_usable_hides(book, 1, 1, unknown));
    // a scroll the character knows is left out; the known check runs only for a scroll it can use
    net::Item scroll;
    scroll.item_type = 20;
    scroll.classes = scroll.races = 0xffff;
    CHECK_FALSE(game::merchant_usable_hides(scroll, 1, 1, unknown));
    CHECK(game::merchant_usable_hides(scroll, 1, 1, known));
    bool asked = false;
    scroll.classes = 1u << 1;
    CHECK(game::merchant_usable_hides(scroll, 1, 1, [&] { asked = true; return false; }));
    CHECK_FALSE(asked);
    net::Item potion = sword;
    potion.classes = potion.races = 0xffff;
    CHECK_FALSE(game::merchant_usable_hides(potion, 1, 1, known)); // not a scroll: what it knows does not matter
    // Hailstone extension: a multiclass server's mask of the player's classes takes the place of the profile's class
    sword.races = 1u << 0;
    constexpr std::uint32_t cleric = 1u << 1, wizard = 1u << 11;
    CHECK_FALSE(game::merchant_usable_hides(sword, 2, 1, unknown, wizard | 1u)); // Warrior is one of them
    CHECK(game::merchant_usable_hides(sword, 1, 1, unknown, cleric | wizard));   // the profile's Warrior is not
    // a stock server (mask 0) keeps the original's bit, also for a class value past 16
    sword.classes = 1u << 19;
    CHECK_FALSE(game::merchant_usable_hides(sword, 20, 1, unknown, 0));
}
