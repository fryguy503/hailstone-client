// The barter hall: the packets as the original's senders and readers build them (net/market_packets.h, checked
// against EQEmu's RoF2 translator) and the barter manager's rules (game/barter.h).

#include <doctest/doctest.h>

#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "game/barter.h"
#include "game/inventory.h"
#include "net/market_packets.h"
#include "net/opcodes.h"

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
    for (auto v : b) d.push_back(std::byte{v});
    return AppPacket(to_wire(op), std::move(d));
}

struct Buf {
    std::vector<std::uint8_t> b;
    Buf& u8(std::uint8_t v) { b.push_back(v); return *this; }
    Buf& u16(std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); return *this; }
    Buf& u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Buf& str(const char* s) {
        b.insert(b.end(), s, s + std::strlen(s));
        b.push_back(0);
        return *this;
    }
    Buf& fixed(const char* s, std::size_t n) {
        std::size_t l = std::strlen(s);
        b.insert(b.end(), s, s + l);
        b.insert(b.end(), n - l, 0);
        return *this;
    }
    Buf& zeros(std::size_t n) { b.insert(b.end(), n, 0); return *this; }
};

std::uint32_t u32_at(const std::vector<std::uint8_t>& b, std::size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

BuyLine bone_chips() {
    BuyLine l;
    l.slot = 2;
    l.enabled = true;
    l.item_id = 13073;
    l.item_name = "Bone Chips";
    l.icon = 600;
    l.quantity = 20;
    l.toggle = true;
    l.cost = 15;
    l.trade_items.push_back({1001, 1, 500, "Cloth Cap"});
    return l;
}

// The line as BarterBuyLine_WriteToBuffer writes it, without the buyer fields.
Buf& line_bytes(Buf& b) {
    return b.u32(2).u8(1).u32(13073).str("Bone Chips").u32(600).u32(20).u8(1).u32(15).u32(1).u32(1001).u32(1).u32(500)
        .str("Cloth Cap");
}

} // namespace

TEST_CASE("barter: the inspect and its answer, buy lines from the server, the buyer appearance") {
    CHECK(bytes_of(encode_barter_inspect(kBuyerInspectBegin, 42)) == Buf().u32(11).u32(42).u32(0).b);
    CHECK(bytes_of(encode_barter_inspect(kBuyerInspectEnd, 42)) == Buf().u32(12).u32(42).u32(0).b);
    // EQEmu's ShowBuyLines echoes the request with the approval at +8 (Handle_OP_Barter 11 reads the packet's third dword).
    auto r = decode_barter_inspect_reply(packet(Op::Barter, Buf().u32(11).u32(42).u32(7).b));
    REQUIRE(r);
    CHECK(r->spawn_id == 42);
    CHECK(r->approval == 7);
    CHECK(decode_barter_inspect_reply(packet(Op::Barter, Buf().u32(11).u32(42).u32(0).b))->approval == 0);

    // OP_BuyerItems 14 as EQEmu writes it ({14, slot, slot, line}); 0x7d6ef0 reads the missing buyer fields as 0.
    BuyLine l = bone_chips();
    auto back = decode_buy_line(encode_buy_line(kBuyerSendBuyLine, l));
    REQUIRE(back);
    CHECK(back->slot == 2);
    CHECK(back->enabled);
    CHECK(back->item_name == "Bone Chips");
    CHECK(back->toggle);
    CHECK(back->cost == 15);
    CHECK(back->buyer_id == 0);
    REQUIRE(back->trade_items.size() == 1);
    CHECK(back->trade_items[0].name == "Cloth Cap");
    // A removal ({7 / 14, 0, slot, 0}) reads as a line that is not enabled; it keeps the defaults (icon 500).
    auto gone = decode_buy_line(packet(Op::BuyerItems, Buf().u32(14).u32(0).u32(4).u32(0).b));
    REQUIRE(gone);
    CHECK(gone->slot == 4);
    CHECK(!gone->enabled);
    CHECK(!gone->toggle);
    CHECK(gone->icon == 500);
    CHECK(!decode_buy_line(packet(Op::BuyerItems, Buf().u32(9).u32(0).b)));

    // Buyer_SetAppearance_Struct (76 bytes): Handle_OP_Barter reads the byte at +72.
    auto a = decode_buyer_appearance(packet(Op::Barter, Buf().u32(13).u32(42).zeros(64).u32(1).b));
    REQUIRE(a);
    CHECK(a->spawn_id == 42);
    CHECK(a->enabled);
    CHECK(!decode_buyer_appearance(packet(Op::Barter, Buf().u32(13).u32(42).zeros(64).u32(0).b))->enabled);
    CHECK(bytes_of(encode_buyer_appearance({42, true})) == Buf().u32(13).u32(42).zeros(64).u32(1).b);
    CHECK(!decode_buyer_appearance(packet(Op::Barter, Buf().u32(13).u32(42).u8(1).b))); // too short
}

TEST_CASE("barter: selling at the buyer and from the search window as the original serializes the sale") {
    BuyLine l = bone_chips();
    // CBarterMerchantWnd: method 0, the constructor's result 1, the buyer's spawn id at +8, three empty strings, the
    // line with zero buyer fields (EQEmu skips their 13 bytes), the quantity at +0x170, 0 at +0x174.
    BuyerSale at;
    at.buyer_spawn_id = 42;
    at.line = l;
    at.quantity = 3;
    Buf want = Buf().u32(9).u32(0).u32(1).u32(42).u32(0).zeros(8).zeros(3);
    line_bytes(want).u32(0).u32(0).u32(0).u8(0).u32(3).u32(0);
    CHECK(bytes_of(encode(at)) == want.b);

    // CBarterSearchWnd: method 1 (the buyer in this zone) or 2, the buyer's spawn and character ids, the line's
    // buyer fields as the search result had them.
    BuyLine m = l;
    m.buyer_spawn_id = 42;
    m.buyer_id = 7001;
    m.zone = 346;
    m.buyer_name = "Buyerbob";
    BuyerSale far;
    far.method = kBarterOutsideBazaar;
    far.buyer_spawn_id = 42;
    far.buyer_id = 7001;
    far.line = m;
    far.quantity = 5;
    Buf want2 = Buf().u32(9).u32(2).u32(1).u32(42).u32(7001).zeros(8).zeros(3);
    line_bytes(want2).u32(42).u32(7001).u32(346).str("Buyerbob").u32(5).u32(0);
    CHECK(bytes_of(encode(far)) == want2.b);
    // EQEmu's DECODE(OP_BuyerItems) BuyerSellItem reads: method at +4, 4 B, buyer spawn at +12, buyer id at +16, 11 B.
    auto wb = bytes_of(encode(far));
    CHECK(u32_at(wb, 4) == 2);
    CHECK(u32_at(wb, 12) == 42);
    CHECK(u32_at(wb, 16) == 7001);
    CHECK(u32_at(wb, 20 + 11) == 2); // the line's slot after the 11 skipped bytes
    auto back = decode_buyer_sale(encode(far));
    REQUIRE(back);
    CHECK(back->method == 2);
    CHECK(back->line.buyer_name == "Buyerbob");
    CHECK(back->quantity == 5);
}

TEST_CASE("barter: a sale's answer as EQEmu writes it, read with 0x423500") {
    // Barter_SellerTransactionComplete: {9, sub action, error code, 16 B, buyer, item, seller, a line, ..., quantity}.
    std::vector<BuyLineTrade> trade{{0, 1, 0, "Cloth Cap"}};
    auto ok = decode_buyer_sale_result(
        encode_buyer_sale_result(kBuyerSellItem, 0, 0, "Buyerbob", "Bone Chips", "Seller", 15, trade, 3));
    REQUIRE(ok);
    CHECK(ok->action == kBuyerSellItem);
    CHECK(ok->sale.result == kBarterSuccess);
    CHECK(ok->sale.buyer_name == "Buyerbob");
    CHECK(ok->sale.item_name == "Bone Chips");
    CHECK(ok->sale.seller_name == "Seller");
    CHECK(ok->sale.line.item_name == "Bone Chips");
    CHECK(ok->sale.line.cost == 15);
    REQUIRE(ok->sale.line.trade_items.size() == 1);
    CHECK(ok->sale.line.trade_items[0].name == "Cloth Cap");
    CHECK(ok->sale.count == 3);
    // A failure: EQEmu's error code is the original's result (+4), its sub action (1) the method (+0).
    auto old = decode_buyer_sale_result(
        encode_buyer_sale_result(kBuyerSellItem, 1, kBarterOutOfDate, "Buyerbob", "Bone Chips", "Seller", 15, {}, 3));
    REQUIRE(old);
    CHECK(old->sale.result == kBarterOutOfDate);
    CHECK(old->sale.method == 1);
    auto bought = decode_buyer_sale_result(
        encode_buyer_sale_result(kBuyerBuyItem, 0, 0, "Me", "Bone Chips", "Seller", 15, {}, 2));
    REQUIRE(bought);
    CHECK(bought->action == kBuyerBuyItem);
    CHECK(bought->sale.seller_name == "Seller");
    CHECK(bought->sale.count == 2);
    // An item name longer than the record's 32 bytes is cut to 31.
    auto lng = decode_buyer_sale_result(encode_buyer_sale_result(kBuyerSellItem, 0, 0, "B",
                                                                 "An Item Name Longer Than Thirty One Characters", "S",
                                                                 1, {}, 1));
    REQUIRE(lng);
    CHECK(lng->sale.item_name.size() == 31);
    CHECK(lng->sale.seller_name == "S");
}

TEST_CASE("barter: the search request, its results and the buyers of the combo") {
    auto q = bytes_of(encode(BarterSearch{"bone", 9, 7001, false}));
    REQUIRE(q.size() == kBarterSearchSize); // 84: the scope is a dword
    CHECK(u32_at(q, 0) == 1);
    CHECK(std::string(reinterpret_cast<const char*>(q.data() + 4)) == "bone");
    CHECK(u32_at(q, 68) == 9);
    CHECK(u32_at(q, 72) == 0);
    CHECK(u32_at(q, 76) == 7001);
    CHECK(u32_at(q, 80) == 0);
    CHECK(u32_at(bytes_of(encode(BarterSearch{"bone", 9, 0, true})), 80) == 1);

    // ENCODE(OP_BuyerItems) Barter_BuyerSearch: the zone as u16 zone, u16 instance (read as the u32 at +0x74).
    Buf found = Buf().u32(1).str("bone").u32(9).u32(0).u32(0).u8(1).u32(1);
    line_bytes(found).u32(42).u32(7001).u16(346).u16(0).str("Buyerbob");
    auto m = decode_barter_search_results(packet(Op::BuyerItems, found.b));
    REQUIRE(m);
    CHECK(m->text == "bone");
    CHECK(m->transaction_id == 9);
    REQUIRE(m->lines.size() == 1);
    CHECK(m->lines[0].item_name == "Bone Chips");
    CHECK(m->lines[0].buyer_spawn_id == 42);
    CHECK(m->lines[0].buyer_id == 7001);
    CHECK(m->lines[0].zone == 346);
    CHECK(m->lines[0].buyer_name == "Buyerbob");
    CHECK(bytes_of(encode_barter_search_results(*m)) == found.b);

    // OP_Barter 0x1a / 0x1b (EQEmu BuyerAddBuyertoBarterWindow_Struct), OP_BuyerItems 0x19.
    auto add = decode_barter_add_buyer(packet(Op::Barter, Buf().u32(0x1a).u32(346).u32(7001).u32(42).fixed("Buyerbob", 64).b));
    REQUIRE(add);
    CHECK(add->zone == 346);
    CHECK(add->buyer_id == 7001);
    CHECK(add->spawn_id == 42);
    CHECK(add->name == "Buyerbob");
    CHECK(decode_barter_remove_buyer(packet(Op::Barter, Buf().u32(0x1b).u32(7001).b)) == 7001u);
    auto list = decode_barter_buyer_list(packet(Op::BuyerItems, Buf().u32(0x19).u32(1).u32(346).u32(7001).u32(42).str("Buyerbob").b));
    REQUIRE(list);
    REQUIRE(list->size() == 1);
    CHECK((*list)[0].name == "Buyerbob");
}

TEST_CASE("barter: our buy lines, buyer mode, the item search, removal, welcome and the small requests") {
    CHECK(bytes_of(encode_buyer_mode(true)) == Buf().u32(2).b);
    CHECK(bytes_of(encode_buyer_mode(false)) == Buf().u32(3).b);
    CHECK(decode_buyer_mode(packet(Op::Barter, Buf().u32(2).b)) == true);
    CHECK(decode_buyer_mode(packet(Op::Barter, Buf().u32(3).b)) == false);
    CHECK(!decode_buyer_mode(packet(Op::Barter, Buf().u32(13).b)));

    auto q = bytes_of(encode_buyer_item_search("bone"));
    REQUIRE(q.size() == 68);
    CHECK(u32_at(q, 0) == 0);
    CHECK(std::string(reinterpret_cast<const char*>(q.data() + 4)) == "bone");
    Buf res = Buf().u32(0).u32(1).fixed("Bone Chips", 64).u32(13073).u32(600).u32(0);
    auto items = decode_buyer_item_results(packet(Op::Barter, res.b));
    REQUIRE(items);
    REQUIRE(items->size() == 1);
    CHECK((*items)[0].item_id == 13073);
    CHECK(bytes_of(encode_buyer_item_results(*items)) == res.b);
    CHECK(!decode_buyer_item_results(packet(Op::Barter, Buf().u32(0).u32(3).b))); // count past the end

    // OP_BuyerItems 6 / 7: BarterBuyLine_WriteToBuffer with the buyer fields (13 zero bytes for our lines), which
    // EQEmu's DECODE skips.
    BuyLine l = bone_chips();
    Buf six = Buf().u32(6).u16(2);
    line_bytes(six).zeros(13);
    line_bytes(six).zeros(13);
    CHECK(bytes_of(encode_buy_lines({l, l})) == six.b);
    Buf seven = Buf().u32(7);
    line_bytes(seven).zeros(13);
    CHECK(bytes_of(encode_modify_buy_line(l)) == seven.b);
    CHECK(bytes_of(encode_buy_lines({})) == Buf().u32(6).u16(0).b);
    // A line that is not enabled is only its slot and flag.
    BuyLine off;
    off.slot = 5;
    CHECK(bytes_of(encode_modify_buy_line(off)) == Buf().u32(7).u32(5).u8(0).b);

    // Send_OP_Barter_00421470: {8, slot}; EQEmu reads +4.
    CHECK(bytes_of(encode_remove_buy_line(4)) == Buf().u32(8).u32(4).b);
    CHECK(decode_remove_buy_line(packet(Op::Barter, Buf().u32(8).u32(4).b)) == 4u);

    auto w = bytes_of(encode_buyer_welcome("Buying bone chips!"));
    REQUIRE(w.size() == 328);
    CHECK(u32_at(w, 0) == 0x13);
    CHECK(std::string(reinterpret_cast<const char*>(w.data() + 0x48)) == "Buying bone chips!");

    CHECK(bytes_of(encode_barter_action(kBarterWelcomeMessage)) == Buf().u32(0x11).b);
    CHECK(bytes_of(encode_barter_action(kBarterOpenWindow)) == Buf().u32(0x17).b);
    CHECK(bytes_of(encode_buyer_greeting(7001)) == Buf().u32(0x14).u32(7001).b);
    BarterItemInspect in;
    in.buyer = 42;
    in.zone = 346;
    in.slot = 2;
    in.index = -1;
    in.item_id = 13073;
    in.shift = true;
    CHECK(bytes_of(encode(in)) == Buf().u32(0xf).u32(0).u32(42).u32(346).u32(2).u32(0xffffffff).u32(13073).u32(2).b);
    CHECK(bytes_of(encode_barter_search_item_inspect(13073, false)) == Buf().u32(0x15).u32(13073).u32(0).b);

    CHECK(decode_buyer_browsing(packet(Op::Barter, Buf().u32(0x10).fixed("Seller", 64).b)) == std::string("Seller"));
    auto g = decode_buyer_greeting(packet(Op::Barter, Buf().u32(0x14).u32(0).fixed("Buyerbob", 64).fixed("Hi!", 256).b));
    REQUIRE(g);
    CHECK(g->name == "Buyerbob");
    CHECK(g->text == "Hi!");
}

TEST_CASE("barter manager: the BART ini line format both ways") {
    BuyLine l = bone_chips();
    CHECK(game::buy_line_ini(l) == "*2^2^1^1^13073^Bone Chips^600^15^20^1^1001^1^Cloth Cap^500");
    BuyLine empty;
    CHECK(game::buy_line_ini(empty) == "*2^0^0^");
    auto back = game::parse_buy_line_ini(game::buy_line_ini(l));
    CHECK(back.slot == 2);
    CHECK(back.enabled);
    CHECK(back.toggle);
    CHECK(back.item_id == 13073);
    CHECK(back.item_name == "Bone Chips");
    CHECK(back.icon == 600);
    CHECK(back.cost == 15);
    CHECK(back.quantity == 20);
    REQUIRE(back.trade_items.size() == 1);
    CHECK(back.trade_items[0].item_id == 1001);
    CHECK(back.trade_items[0].quantity == 1);
    CHECK(back.trade_items[0].name == "Cloth Cap");
    CHECK(back.trade_items[0].icon == 500);
    auto off = game::parse_buy_line_ini("*2^7^0^");
    CHECK(off.slot == 7);
    CHECK(!off.enabled);
    // Version 1 (no "*"): an enabled line without slot fields.
    auto v1 = game::parse_buy_line_ini("0^5023^Fine Steel Sword^592^1000^1^0");
    CHECK(v1.enabled);
    CHECK(!v1.toggle);
    CHECK(v1.item_id == 5023);
    CHECK(v1.cost == 1000);
    CHECK(v1.quantity == 1);
    // OP_BuyerItems 5's text.
    auto lines = game::parse_buy_lines_text("2$*2^3^1^1^1^A^500^5^1^0$*2^9^1^0^2^B^500^6^2^0$");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].first == 3);
    CHECK(lines[0].second.item_name == "A");
    CHECK(lines[1].first == 9);
    CHECK(lines[1].second.cost == 6);
}

TEST_CASE("barter manager: a buy line's trade-item count is bounded by its text and kMaxBuyLineTrades") {
    // A count near a billion with two entries: the two, nothing made up after the text ends.
    auto huge = game::parse_buy_line_ini("*2^1^1^1^5^X^500^5^1^999999999^7^1^Cap^500^8^2^Hat^501");
    REQUIRE(huge.trade_items.size() == 2);
    CHECK(huge.trade_items[1].name == "Hat");
    // More entries than the bound: the first kMaxBuyLineTrades.
    std::string many = "*2^1^1^1^5^X^500^5^1^100";
    for (int i = 0; i < 100; ++i) many += std::format("^{}^1^T^500", i + 1);
    CHECK(game::parse_buy_line_ini(many).trade_items.size() == net::kMaxBuyLineTrades);
    CHECK(game::parse_buy_line_ini("*2^1^1^1^5^X^500^5^1^-3").trade_items.empty());
    auto lines = game::parse_buy_lines_text("1$*2^3^1^1^1^A^500^5^1^999999999$");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].second.trade_items.empty());
}

TEST_CASE("barter manager: OP_BuyerItems 5 counting more lines than its text holds makes no empty ones") {
    // An empty record would be slot 0 and replace (and save over) the line there.
    auto lines = game::parse_buy_lines_text("40$*2^3^1^1^1^A^500^5^1^0$*2^9^1^0^2^B^500^6^2^0$");
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].first == 9);
    CHECK(game::parse_buy_lines_text("3$").empty());
}

TEST_CASE("barter manager: an overflowing number field saturates at the 32-bit range") {
    // 4000000000 does not fit an int: it reads as INT32_MAX (MSVC's atoi), so the count is bounded, not wrapped.
    auto big = game::parse_buy_line_ini("*2^1^1^1^4000000000^X^500^5^1^4000000000^7^1^Cap^500");
    CHECK(big.item_id == 0x7fffffffu);
    REQUIRE(big.trade_items.size() == 1);
    CHECK(big.trade_items[0].name == "Cap");
    auto neg = game::parse_buy_line_ini("*2^1^1^1^-99999999999^X^500");
    CHECK(neg.item_id == 0x80000000u);
    CHECK(game::parse_buy_line_ini("*2^1^1^1^0xffffffffff^X^500").item_id == 0x7fffffffu);
}

TEST_CASE("barter manager: the price buttons, the price text and the reports") {
    // CBarterWnd_SetPriceCoin: platinum keeps the lower three digits, gold and silver add tens to the coin above.
    CHECK(game::barter_set_price_coin(12345, 0, 7) == 7345);
    CHECK(game::barter_set_price_coin(12345, 1, 5) == 12545);
    CHECK(game::barter_set_price_coin(12345, 2, 9) == 12395);
    CHECK(game::barter_set_price_coin(12345, 3, 0) == 12340);
    CHECK(game::barter_set_price_coin(0, 1, 15) == 1500);
    CHECK(game::barter_set_price_coin(0, 0, 1000000) == game::kMaxBuyPrice);
    // A platinum count that wraps in 32 bits (4294968 x 1000 = 2^32 + 704) is still over the cap.
    CHECK(game::barter_set_price_coin(0, 0, 4294968u) == game::kMaxBuyPrice);
    CHECK(game::barter_set_price_coin(0, 1, 0xffffffffu) == game::kMaxBuyPrice);
    BuyLine l = bone_chips();
    CHECK(game::buy_line_price_text(l) == "1s 5c + items");
    l.trade_items.clear();
    CHECK(game::buy_line_price_text(l) == "1s 5c ");
    l = bone_chips();
    CHECK(game::barter_report("You've sold ", " to ", l, "Buyerbob", 2) ==
          "You've sold 2 Bone Chips to Buyerbob for 3s  and the following: 2 Cloth Cap.");
    l.trade_items.clear();
    CHECK(game::barter_report("You've bought ", " from ", l, "Seller", 3) == "You've bought 3 Bone Chips from Seller for 4s 5c .");
}

TEST_CASE("barter windows: the buttons, the free line, the merchant's lines and the combo") {
    game::BarterWindowState s;
    CHECK(s.free_slot() == 0);
    s.lines[0] = bone_chips();
    CHECK(s.free_slot() == 1);
    s.last_search = 100;
    auto b = game::barter_window_buttons(s, 105);
    CHECK(!b.search);
    CHECK(game::barter_window_buttons(s, 106).search);
    CHECK(b.create_shown);
    CHECK(!b.create);
    CHECK(!b.modify_shown);
    CHECK(b.start_stop == "Start Barter");
    s.item_sel = 0;
    s.editor.enabled = true;
    s.inventory_sel = 1;
    b = game::barter_window_buttons(s, 200);
    CHECK(b.create);
    CHECK(b.inventory_add);
    CHECK(!b.inventory_remove);
    s.item_sel = -1;
    s.line_sel = 0;
    s.buyer_mode = true;
    b = game::barter_window_buttons(s, 200);
    CHECK(!b.create_shown);
    CHECK(b.modify_shown);
    CHECK(b.modify);
    CHECK(b.toggle);
    CHECK(b.remove);
    CHECK(b.offline);
    CHECK(b.start_stop == "Stop Barter");
    CHECK(b.start_stop_color == 0xffff0000);

    // CBarterMerchantWnd_UpdateBuyLine: replaced, removed when not active, or added.
    game::BarterMerchantState m;
    BuyLine l = bone_chips();
    game::barter_merchant_update(m, l);
    REQUIRE(m.lines.size() == 1);
    l.quantity = 5;
    game::barter_merchant_update(m, l);
    REQUIRE(m.lines.size() == 1);
    CHECK(m.lines[0].quantity == 5);
    BuyLine other = l;
    other.slot = 3;
    game::barter_merchant_update(m, other);
    CHECK(m.lines.size() == 2);
    BuyLine gone;
    gone.slot = 2;
    game::barter_merchant_update(m, gone);
    REQUIRE(m.lines.size() == 1);
    CHECK(m.lines[0].slot == 3);

    game::BarterSearchState q;
    q.buyers[7001] = {346, 7001, 42, "Zed"};
    q.buyers[7002] = {346, 7002, 43, "Abe"};
    auto c = q.combo_choices();
    REQUIRE(c.size() == 4);
    CHECK(c[0].first == "All Barterers");
    CHECK(c[1].second == -1);
    CHECK(c[2].first == "Abe");
    CHECK(c[3].second == 7001);
    BuyLine sold = bone_chips();
    sold.buyer_id = 7001;
    q.lines.push_back(sold);
    game::barter_search_sold(q, sold, 4);
    CHECK(q.lines[0].quantity == 16);
    CHECK(game::is_barter_zone(346));
    CHECK(game::is_barter_zone(151));
    CHECK(!game::is_barter_zone(202));
}

TEST_CASE("barter windows: the inventory lists and what the player has") {
    auto item = [](std::uint32_t id, const char* name, std::uint32_t count, std::uint32_t max_stack) {
        Item i;
        i.id = id;
        i.name = name;
        i.no_drop = 255;
        i.max_stack = max_stack;
        i.stack_count = count;
        return i;
    };
    game::Inventory inv;
    inv.put(ItemSlot::possessions(kSlotGeneral1), item(13073, "Bone Chips", 12, 1000));
    Item bag = item(17001, "Backpack", 1, 1);
    bag.item_class = 1;
    bag.contents.push_back({0, item(13073, "Bone Chips", 5, 1000)});
    bag.contents.push_back({1, item(5013, "Rusty Short Sword", 1, 1)});
    inv.put(ItemSlot::possessions(kSlotGeneral1 + 1), bag);
    Item nt = item(13073, "Bone Chips", 7, 1000);
    nt.no_drop = 0; // NO TRADE
    inv.put(ItemSlot::possessions(kSlotGeneral1 + 2), nt);
    Item clicky = item(9000, "Wand", 1, 1);
    clicky.effects[kEffectClick].effect = 200;
    clicky.effects[kEffectClick].extra[0] = 5; // max charges
    clicky.charges = 3;                        // used: not offered
    inv.put(ItemSlot::possessions(kSlotGeneral1 + 3), clicky);
    inv.put(ItemSlot::possessions(kSlotPrimary), item(5014, "Rusty Axe", 1, 1)); // worn
    auto w = game::barter_window_inventory(inv);
    REQUIRE(w.size() == 2); // the bag itself, the NO TRADE stack, the used wand and the worn axe are left out
    CHECK(w[0].name == "Bone Chips");
    CHECK(w[0].count == 17);
    CHECK(w[1].item_id == 5013);
    auto s = game::barter_search_inventory(inv, 2);
    REQUIRE(s.size() == 4); // Krono first, then the worn axe (slots 0..33), the chips, the sword
    CHECK(s[0].item_id == kKronoItemId);
    CHECK(s[0].count == 2);
    CHECK(s[1].name == "Rusty Axe");
    CHECK(game::barter_has_items(inv, 13073, 17));
    CHECK(!game::barter_has_items(inv, 13073, 18));
    CHECK(!game::barter_has_items(inv, 5014, 1));
}

TEST_CASE("barter: a count with no data behind it makes no empty lines or buyers") {
    // A search result that claims 10000 lines and carries none: nothing past the data (it used to give 10000 defaults).
    auto m = decode_barter_search_results(packet(Op::BuyerItems, Buf().u32(1).str("").u32(9).u32(0).u32(0).u8(1).u32(10000).b));
    REQUIRE(m);
    CHECK(m->lines.empty());
    auto list = decode_barter_buyer_list(packet(Op::BuyerItems, Buf().u32(0x19).u32(10000).b));
    REQUIRE(list);
    CHECK(list->empty());
    // One real buyer, then the claimed rest is missing: the one is kept.
    auto one = decode_barter_buyer_list(packet(Op::BuyerItems, Buf().u32(0x19).u32(500).u32(346).u32(7001).u32(42).str("Buyerbob").b));
    REQUIRE(one);
    CHECK(one->size() == 1);
}

TEST_CASE("barter: names with no terminator in a large packet read as empty, in one scan of the tail") {
    // GetString's rule: a string without its NUL in the packet reads as empty and moves nothing. Each buyer here
    // reads its three words and then a name that never ends; the tail is looked through once, not once a buyer
    // (10000 scans of 8 MiB would take minutes).
    Buf buf;
    buf.u32(0x19).u32(10000);
    buf.b.insert(buf.b.end(), std::size_t{8} << 20, std::uint8_t{1});
    auto list = decode_barter_buyer_list(packet(Op::BuyerItems, buf.b));
    REQUIRE(list);
    REQUIRE(list->size() == 10000);
    CHECK(list->front().zone == 0x01010101u);
    CHECK(list->front().name.empty());
    CHECK(list->back().spawn_id == 0x01010101u);
    CHECK(list->back().name.empty());
    // A name that does end is still read, and one without its NUL after it is empty.
    auto two = decode_barter_buyer_list(packet(Op::BuyerItems, Buf().u32(0x19).u32(2).u32(1).u32(2).u32(3).str("Bob").u32(4).u32(5).u32(6).u8('x').b));
    REQUIRE(two);
    REQUIRE(two->size() == 2);
    CHECK(two->at(0).name == "Bob");
    CHECK(two->at(1).spawn_id == 6);
    CHECK(two->at(1).name.empty());
}

TEST_CASE("barter: a buy-lines text length near 2^32 is refused") {
    // The length check is written as n <= size - pos so that it cannot wrap where size_t is 32 bits.
    CHECK(!decode_buy_lines_text(packet(Op::Barter, Buf().u32(kBuyerLinesText).u32(0xFFFFFFFCu).u8('a').b)));
    CHECK(!decode_buy_lines_text(packet(Op::Barter, Buf().u32(kBuyerLinesText).u32(0xFFFFFFFFu).b)));
}
