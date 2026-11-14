// Player, bazaar and mail packets byte for byte against the original's senders
// (Cmd_duel, Send_OP_InspectRequest, CBazaarWnd,
// CBazaarSearchWnd::doQuery, MailMgr_*) and EQEmu's RoF2 layouts
// (common/patches/rof2_structs.h, rof2.cpp, ucs/clientlist.cpp, ucs/database.cpp);
// and the rules in game/interact.h, game/bazaar.h, game/mail.h.

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include "game/bazaar.h"
#include "game/feature_restrictions.h"
#include "game/interact.h"
#include "game/mail.h"
#include "net/chat_server.h"
#include "net/market_packets.h"
#include "net/opcodes.h"
#include "net/player_packets.h"
#include "net/tribute_packets.h"
#include "net/player_packets.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

std::vector<std::uint8_t> bytes_of(const AppPacket& p) {
    std::vector<std::uint8_t> out;
    for (std::byte b : p.data) out.push_back(std::to_integer<std::uint8_t>(b));
    return out;
}

AppPacket packet(std::uint16_t op, const std::vector<std::uint8_t>& b) {
    std::vector<std::byte> d;
    for (auto v : b) d.push_back(std::byte{v});
    return AppPacket(op, std::move(d));
}
AppPacket packet(Op op, const std::vector<std::uint8_t>& b) { return packet(to_wire(op), b); }

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

} // namespace

// ---------------------------------------------------------------- duels

TEST_CASE("duel packets: OP_RequestDuel, OP_DuelAccept, OP_DuelDecline and the 0x52f7 refusal") {
    auto r = encode(DuelRequest{25, 7});
    CHECK(r.opcode == to_wire(Op::RequestDuel));
    CHECK(bytes_of(r) == Buf().u32(25).u32(7).b);
    // The server's forward {challenger, challenger}: the client reads +4.
    auto d = decode_duel_request(packet(Op::RequestDuel, Buf().u32(9).u32(9).b));
    REQUIRE(d);
    CHECK(d->second == 9);
    CHECK(bytes_of(encode_duel_accept(25, 7)) == Buf().u32(25).u32(7).b);
    CHECK(encode_duel_accept(25, 7).opcode == 0x68d3);
    CHECK(encode_duel_decline(25, 7).opcode == 0x6a46);
    CHECK(encode_duel_decline(25, 7).data.size() == 8);
    auto f = encode(DuelRefusal{25, 7, 2});
    CHECK(f.opcode == 0x52f7);
    CHECK(bytes_of(f) == Buf().u32(25).u32(7).u8(2).zeros(3).b);
    auto back = decode_duel_refusal(f);
    REQUIRE(back);
    CHECK(back->refuser == 7);
    CHECK(back->kind == 2);
}

TEST_CASE("duel rules follow Cmd_duel and Handle_OP_RequestDuel") {
    using C = game::DuelCommandOutcome::Kind;
    using R = game::DuelRequestOutcome::Kind;
    game::DuelState s;
    CHECK(game::duel_command(s, 0, false, false).eqstr == 13248);
    CHECK(game::duel_command(s, 5, false, false).eqstr == 13248); // an NPC
    CHECK(game::duel_command(s, 5, true, true).eqstr == 13249);   // ourselves
    auto c = game::duel_command(s, 5, true, false);
    CHECK(c.kind == C::Challenge);
    CHECK(c.eqstr == 13254);
    CHECK(s.challenged == 5);
    // Someone else challenges us meanwhile: refused, "considering another duel".
    auto o = game::duel_request(s, 6, false);
    CHECK(o.kind == R::Refuse);
    CHECK(o.refusal_kind == 2);
    // The target accepts: its request comes back.
    o = game::duel_request(s, 5, false);
    CHECK(o.kind == R::Accepted);
    CHECK(s.active);
    CHECK(game::duel_command(s, 5, true, false).eqstr == 13250);
    CHECK(game::duel_command(s, 6, true, false).eqstr == 13251);
    CHECK(game::duel_request(s, 6, false).refusal_kind == 1);

    game::DuelState t;
    o = game::duel_request(t, 9, false);
    CHECK(o.kind == R::Challenged);
    CHECK(t.challenger == 9);
    CHECK(game::duel_request(t, 9, false).kind == R::Ignore);
    CHECK(game::duel_command(t, 4, true, false).eqstr == 13253); // decline first
    c = game::duel_command(t, 9, true, false);
    CHECK(c.kind == C::Accept);
    CHECK(c.eqstr == 13252);
    CHECK(t.active);
    CHECK(t.challenged == 9);
    // Ignored players are refused with kind 0.
    game::DuelState u;
    o = game::duel_request(u, 3, true);
    CHECK(o.kind == R::Refuse);
    CHECK(o.refusal_kind == 0);
}

// ---------------------------------------------------------------- inspect

TEST_CASE("inspect packets: OP_InspectRequest and the 1860-byte OP_InspectAnswer") {
    CHECK(bytes_of(encode_inspect_request(30, 2)) == Buf().u32(30).u32(2).b);
    Buf a;
    a.u32(2).u32(30);
    for (int i = 0; i < 23; ++i) a.fixed(i == 13 ? "Rusty Short Sword" : "", 64);
    for (int i = 0; i < 23; ++i) a.u32(i == 13 ? 592 : 0xffffffffu);
    a.fixed("Hello there", 288);
    REQUIRE(a.b.size() == kInspectAnswerSize);
    auto v = decode_inspect_answer(packet(Op::InspectAnswer, a.b));
    REQUIRE(v);
    CHECK(v->requester == 2);
    CHECK(v->player == 30);
    CHECK(v->names[13] == "Rusty Short Sword");
    CHECK(v->names[0].empty());
    CHECK(v->icons[13] == 592);
    CHECK(v->icons[0] == 0xffffffffu);
    CHECK(v->text == "Hello there");
    CHECK(bytes_of(encode(*v)) == a.b);
    auto m = encode_inspect_message("my text");
    CHECK(m.opcode == to_wire(Op::InspectMessageUpdate));
    CHECK(bytes_of(m) == Buf().fixed("my text", 256).b);
}

// ---------------------------------------------------------------- player commands

TEST_CASE("player command packets: surname, corpse commands, shield, keys, rewind") {
    auto s = encode(Surname{"Nmscmnine", 0, "Smith"});
    CHECK(bytes_of(s) == Buf().fixed("Nmscmnine", 64).u32(0).fixed("Smith", 32).b);
    auto back = decode_surname(s);
    REQUIRE(back);
    CHECK(back->last_name == "Smith");

    CorpseCommand c;
    c.corpse_name = "Nmscmnine's corpse0";
    c.player_name = "Nmscmnine";
    c.zone_id = 202;
    c.y = 1.0f;
    c.x = 2.0f;
    c.z = -3.5f;
    auto p = encode_corpse_command(Op::CorpseDrag, c);
    CHECK(p.opcode == to_wire(Op::CorpseDrag));
    auto b = bytes_of(p);
    REQUIRE(b.size() == kCorpseCommandSize);
    CHECK(std::string(reinterpret_cast<const char*>(b.data())) == "Nmscmnine's corpse0");
    CHECK(std::string(reinterpret_cast<const char*>(b.data() + 64)) == "Nmscmnine");
    CHECK(u32_at(b, 128) == 0);
    CHECK(u32_at(b, 132) == 202);
    CHECK(u32_at(b, 136) == 0x3f800000u);
    CHECK(u32_at(b, 140) == 0x40000000u);
    CHECK(u32_at(b, 144) == 0xc0600000u);
    CHECK(encode_corpse_command(Op::GMSummon, c).opcode == 0x486f);

    CHECK(bytes_of(encode_corpse_drop("a corpse")) == Buf().str("a corpse").b);
    CHECK(bytes_of(encode_corpse_drop("")) == Buf().u8(0).b);
    CHECK(bytes_of(encode_shielding(44)) == Buf().u32(44).b);
    CHECK(bytes_of(encode_key_ring_request()) == Buf().str("KEY").b);
    CHECK(encode_rewind().data.empty());
    CHECK(encode_rewind().opcode == 0x1745);
    CHECK(encode_reset_aa().data.empty());
    CHECK(encode_reset_aa().opcode == 0x1669);
    auto petition = encode_petition("help me");
    REQUIRE(petition);
    CHECK(petition->opcode == 0x3de3);
    CHECK(bytes_of(*petition) == Buf().str("help me").b);
    CHECK_FALSE(encode_petition(std::string(0x400, 'x'))); // Send_OP_Petition's limit
    CHECK(encode_petition(std::string(0x3ff, 'x')));
}

TEST_CASE("the game commands' packets: shield group, player state, hide / decay corpse, start city, reveal") {
    // Cmd_shieldgroup: six ids (0 past the members) and the count, opcode 0x5a79.
    auto sg = encode_shield_group({11, 12, 13});
    CHECK(sg.opcode == 0x5a79);
    CHECK(bytes_of(sg) == Buf().u32(11).u32(12).u32(13).zeros(12).u32(3).b);
    CHECK(bytes_of(encode_shield_group({1, 2, 3, 4, 5, 6, 7})) ==
          Buf().u32(1).u32(2).u32(3).u32(4).u32(5).u32(6).u32(6).b);
    // Cmd_sheathe: {spawn id, bit 2} as OP_PlayerStateAdd (0x559a) or OP_PlayerStateRemove (0x2d25).
    auto add = encode_player_state(true, PlayerState{77, kPlayerStateSheathed});
    CHECK(add.opcode == 0x559a);
    CHECK(bytes_of(add) == Buf().u32(77).u32(2).b);
    CHECK(encode_player_state(false, PlayerState{77, 2}).opcode == 0x2d25);
    auto st = decode_player_state(add);
    REQUIRE(st);
    CHECK(st->spawn_id == 77);
    CHECK(st->state == 2);
    // cmd_hidecorpses / CLootWnd::AboutToHide: OP_HideCorpse (0x49e1) {action, corpse}.
    auto hc = encode_hide_corpse(kHideCorpseThis, 900);
    CHECK(hc.opcode == 0x49e1);
    CHECK(bytes_of(hc) == Buf().u32(4).u32(900).b);
    CHECK(bytes_of(encode_hide_corpse(kHideCorpseNpc)) == Buf().u32(5).u32(0).b);
    // /decaycorpse's yes: 0x7d50 {corpse, player}.
    auto dc = encode_decay_corpse(900, 77);
    CHECK(dc.opcode == 0x7d50);
    CHECK(bytes_of(dc) == Buf().u32(900).u32(77).b);
    auto sc = encode_set_start_city("3");
    CHECK(sc.opcode == 0x6326);
    CHECK(bytes_of(sc) == Buf().str("3").b);
    CHECK(bytes_of(encode_set_start_city("")) == Buf().u8(0).b);
    CHECK(encode_reveal().opcode == 0x15e7);
    CHECK(encode_reveal().data.empty());
    // CPlayerCustomizationWnd's Accept: OP_FaceChange (0x5578), 24 bytes; OP_SetFace reads the same layout.
    FaceChange f;
    f.hair_color = 1, f.beard_color = 2, f.eye1 = 3, f.eye2 = 4, f.hair_style = 5, f.beard = 6, f.face = 27;
    f.heritage = 7, f.tattoo = 8, f.details = 9, f.spawn_id = 77;
    auto fc = encode(f);
    CHECK(fc.opcode == 0x5578);
    CHECK(bytes_of(fc) == Buf().u8(1).u8(2).u8(3).u8(4).u8(5).u8(6).u8(27).u8(0).u32(7).u32(8).u32(9).u32(77).b);
    auto back = decode_face_change(fc);
    REQUIRE(back);
    CHECK(back->face == 27);
    CHECK(back->spawn_id == 77);
}

TEST_CASE("bandolier packets are 40 bytes: action, set, name") {
    auto p = encode_bandolier(kBandolierCreate, 3, "Melee");
    CHECK(p.opcode == 0x7677);
    CHECK(bytes_of(p) == Buf().u32(0).u8(3).fixed("Melee", 32).zeros(3).b);
    CHECK(bytes_of(encode_bandolier(kBandolierSet, 1)) == Buf().u32(2).u8(1).zeros(35).b);
    auto d = decode_bandolier(p);
    REQUIRE(d);
    CHECK(d->number == 3);
    CHECK(d->name == "Melee");
    CHECK(game::bandolier_set_count(0) == 4);
    CHECK(game::bandolier_set_count(30) == 20);
}

TEST_CASE("OP_ReadBook is BookRequest_Struct both ways") {
    ReadBook r;
    r.slot = 25;
    r.sub = 3;
    r.type = 1;
    r.target = 77;
    r.text = "BookFile1";
    auto p = encode(r);
    auto b = bytes_of(p);
    REQUIRE(b.size() == kReadBookSize);
    Buf head;
    head.u32(0xffffffffu).u16(25).u16(3).u16(0xffff).u16(0).u32(1).u32(77).u8(0).u8(0);
    CHECK(std::vector<std::uint8_t>(b.begin(), b.begin() + 22) == head.b);
    CHECK(std::string(reinterpret_cast<const char*>(b.data() + 22)) == "BookFile1");
    auto back = decode_read_book(p);
    REQUIRE(back);
    CHECK(back->text == "BookFile1");
    CHECK(back->slot == 25);
    CHECK(back->sub == 3);
}

// ---------------------------------------------------------------- the bazaar

TEST_CASE("trader mode packets: the 4404-byte start, end, listing request and price messages") {
    TraderBegin b;
    b.serials[0] = "0000000000001234";
    b.prices[0] = 1500;
    b.serials[1] = "0000000000001235";
    b.prices[1] = 7;
    auto p = encode(b);
    CHECK(p.opcode == to_wire(Op::Trader));
    auto v = bytes_of(p);
    REQUIRE(v.size() == kTraderBeginSize);
    CHECK(u32_at(v, 0) == 1);
    CHECK(std::string(reinterpret_cast<const char*>(v.data() + 4)) == "0000000000001234");
    CHECK(std::string(reinterpret_cast<const char*>(v.data() + 4 + 18)) == "0000000000001235");
    CHECK(v[4 + 18 * 2] == 0);
    CHECK(u32_at(v, 3604) == 1500);
    CHECK(u32_at(v, 3608) == 7);
    auto back = decode_trader_begin(p);
    REQUIRE(back);
    CHECK(back->serials[1] == "0000000000001235");

    CHECK(bytes_of(encode_trader_action(kTraderEnd)) == Buf().u32(2).u32(0).u32(0).b);
    CHECK(bytes_of(encode_trader_action(kTraderListItems)) == Buf().u32(11).u32(0).u32(0).b);
    auto pr = encode(TraderPrice{kTraderPriceUpdate, "0000000000001234", 0, 250});
    CHECK(bytes_of(pr) == Buf().u32(3).fixed("0000000000001234", 18).zeros(2).u32(0).u32(250).b);
    auto rc = decode_trader_price(encode(TraderPrice{kTraderReconcile, "0000000000000009", 20, 5}));
    REQUIRE(rc);
    CHECK(rc->action == 20);
    CHECK(rc->count == 20);
    CHECK(rc->price == 5);
}

TEST_CASE("OP_Trader server messages decode as HandleTraderMsg reads them") {
    auto ack = decode_trader_message(encode_trader_status(kTraderAck2, 0));
    REQUIRE(ack);
    CHECK(ack->action == 22);
    CHECK(ack->word1 == 0);
    auto pu = decode_trader_message(encode_trader_status(kTraderPriceUpdate, 1));
    REQUIRE(pu);
    CHECK(pu->word1 == 1);
    auto list = decode_trader_message(encode_trader_listing({{"0000000000001234", 1500}, {"0000000000001235", 7}}));
    REQUIRE(list);
    REQUIRE(list->listing.size() == 200);
    CHECK(list->listing[0].first == "0000000000001234");
    CHECK(list->listing[1].second == 7);
    CHECK(list->listing[5].first.empty());
    TraderBuy sale;
    sale.action = kTraderBuyItem;
    sale.buyer_name = "Buyer";
    sale.item_name = "Bone Chips";
    sale.price = 10;
    sale.quantity = 4;
    auto s = decode_trader_message(encode(sale, Op::Trader));
    REQUIRE(s);
    REQUIRE(s->sale);
    CHECK(s->sale->buyer_name == "Buyer");
    CHECK(s->sale->quantity == 4);
}

TEST_CASE("TraderBuy_Struct is 284 bytes with the fields where RoF2 has them") {
    TraderBuy t;
    t.method = 2;
    t.sub_action = 7;
    t.trader_id = 1234;
    t.buyer_name = "Me";
    t.seller_name = "Seller";
    t.item_name = "Fine Steel Sword";
    t.serial = "0000000000004321";
    t.item_id = 5023;
    t.price = 900;
    t.already_sold = 3;
    t.unknown276 = 2;
    t.quantity = 1;
    auto p = encode(t);
    CHECK(p.opcode == to_wire(Op::TraderShop));
    auto b = bytes_of(p);
    REQUIRE(b.size() == kTraderBuySize);
    CHECK(u32_at(b, 0) == 10);
    CHECK(u32_at(b, 4) == 2);
    CHECK(u32_at(b, 8) == 7);
    CHECK(u32_at(b, 16) == 1234);
    CHECK(std::string(reinterpret_cast<const char*>(b.data() + 20)) == "Me");
    CHECK(std::string(reinterpret_cast<const char*>(b.data() + 84)) == "Seller");
    CHECK(std::string(reinterpret_cast<const char*>(b.data() + 180)) == "Fine Steel Sword");
    CHECK(std::string(reinterpret_cast<const char*>(b.data() + 244)) == "0000000000004321");
    CHECK(u32_at(b, 264) == 5023);
    CHECK(u32_at(b, 268) == 900);
    CHECK(u32_at(b, 272) == 3);
    CHECK(u32_at(b, 276) == 2);
    CHECK(u32_at(b, 280) == 1);
    auto back = decode_trader_buy(p);
    REQUIRE(back);
    CHECK(back->seller_name == "Seller");
    CHECK(back->quantity == 1);
}

TEST_CASE("OP_TraderShop: click, end, first open, welcome, add / remove trader") {
    CHECK(bytes_of(encode(TraderClick{kTraderClick, 55, 0})) == Buf().u32(28).u32(55).u32(0).b);
    CHECK(bytes_of(encode_trader_shop_end()) == Buf().u32(4).b);
    CHECK(bytes_of(encode_trader_shop_action(kBazaarFirstOpen)) == Buf().u32(26).u32(0).u32(0).b);
    // The click answer: approval in the low byte of the third dword (EQEmu: unknown_008).
    auto c = decode_trader_shop(packet(Op::TraderShop, Buf().u32(28).u32(55).u32(1).b));
    REQUIRE(c);
    REQUIRE(c->click);
    CHECK(c->click->approval == 1);
    auto w = decode_trader_shop(encode_bazaar_welcome(12, 340));
    REQUIRE(w);
    CHECK(w->traders == 12);
    CHECK(w->items == 340);
    BazaarTrader t{151, 0, 789, 42, "Seller"};
    auto add = encode_bazaar_add_trader(t);
    CHECK(add.data.size() == 80);
    auto a = decode_trader_shop(add);
    REQUIRE(a);
    CHECK(a->action == 24);
    CHECK(a->trader.trader_id == 789);
    CHECK(a->trader.entity_id == 42);
    CHECK(a->trader.name == "Seller");
    auto r = decode_trader_shop(packet(Op::TraderShop, Buf().u32(25).u32(789).b));
    REQUIRE(r);
    CHECK(r->trader.trader_id == 789);
}

TEST_CASE("the bazaar search query is BazaarSearch_Struct with doQuery's defaults") {
    BazaarQuery q;
    q.name = "sword";
    q.max_cost = 50;
    auto p = encode(q);
    auto b = bytes_of(p);
    REQUIRE(b.size() == kBazaarQuerySize);
    CHECK(u32_at(b, 0) == 7);
    CHECK(b[4] == 1);                    // "All Traders" (data 0): scope 1
    CHECK(u32_at(b, 16) == 0);           // trader
    CHECK(u32_at(b, 20) == 0xffffffffu); // class
    CHECK(u32_at(b, 24) == 0xffffffffu); // race
    CHECK(u32_at(b, 28) == 0xffffffffu); // stat
    CHECK(u32_at(b, 32) == 0xffffffffu); // slot
    CHECK(u32_at(b, 36) == 0xffffffffu); // type
    CHECK(std::string(reinterpret_cast<const char*>(b.data() + 40)) == "sword");
    CHECK(u32_at(b, 104) == 0);
    CHECK(u32_at(b, 108) == 50);
    CHECK(u32_at(b, 112) == 1);
    CHECK(u32_at(b, 116) == 100);
    CHECK(u32_at(b, 120) == 200);
    CHECK(u32_at(b, 124) == 0);
    CHECK(u32_at(b, 128) == 0);
    auto back = decode_bazaar_query(p);
    REQUIRE(back);
    CHECK(back->name == "sword");
    CHECK(back->max_level == 100);
    CHECK(back->trader == 0);
    // The local traders choice (data -1): scope 0, no trader.
    q.trader = 0xffffffffu;
    b = bytes_of(encode(q));
    CHECK(b[4] == 0);
    CHECK(u32_at(b, 16) == 0);
    q.trader = 789;
    b = bytes_of(encode(q));
    CHECK(b[4] == 1);
    CHECK(u32_at(b, 16) == 789);
}

TEST_CASE("bazaar inspect, trader list, search results and item removal") {
    auto in = encode(BazaarInspect{789, "0000000000004321", 5023, 2});
    CHECK(bytes_of(in) == Buf().u32(18).u32(0).u32(789).fixed("0000000000004321", 18).zeros(2).u32(5023).u32(2).b);
    // OP_TraderBulkSend as SendBulkBazaarTraders writes it.
    Buf bulk;
    bulk.u32(2).u16(151).u16(0).u32(789).u32(42).str("Seller").u16(151).u16(3).u32(790).u32(0).str("Bazaar Shard 2");
    auto tl = decode_trader_bulk(packet(Op::TraderBulkSend, bulk.b));
    REQUIRE(tl);
    REQUIRE(tl->size() == 2);
    CHECK((*tl)[0].name == "Seller");
    CHECK((*tl)[1].zone_instance == 3);
    CHECK(bytes_of(encode_trader_bulk(*tl)) == bulk.b);
    // ENCODE(OP_BazaarSearch) BazaarSearch: u32 0, u16 zone, u32 trader, u32 count, entries.
    Buf res;
    res.u32(0).u16(151).u32(789).u32(1);
    res.u32(789).str("0000000000004321").u32(900).u32(1).u32(5023).u32(592).str("Fine Steel Sword").u32(7);
    auto r = decode_bazaar_results(packet(Op::BazaarSearch, res.b));
    REQUIRE(r);
    REQUIRE(r->results.size() == 1);
    CHECK(r->results[0].serial == "0000000000004321");
    CHECK(r->results[0].cost == 900);
    CHECK(r->results[0].name == "Fine Steel Sword");
    CHECK(r->results[0].stat == 7);
    CHECK(bytes_of(encode(*r)) == res.b);
    // The server's "done" (BazaarReturnDone_Struct) carries a count of -1: no results.
    Buf done;
    done.u32(12).u32(42).u32(0xffffffffu).u32(0xffffffffu).u32(0xffffffffu);
    auto d = decode_bazaar_results(packet(Op::BazaarSearch, done.b));
    REQUIRE(d);
    CHECK(d->results.empty());
    auto del = decode_trader_del_item(packet(Op::TraderDelItem, Buf().u32(0).u32(42).fixed("0000000000004321", 17).u32(0).b));
    REQUIRE(del);
    CHECK(del->trader_id == 42);
    CHECK(del->serial == "0000000000004321");
    auto bt = decode_become_trader(packet(Op::BecomeTrader, Buf().u32(42).u32(1).b));
    REQUIRE(bt);
    CHECK(bt->spawn_id == 42);
    CHECK(bt->trader == 1);
}

TEST_CASE("titles: the request, the list, choosing one and the reply (EQEmu's layouts)") {
    CHECK(encode_request_titles().data.empty());
    auto list = packet(Op::SendTitleList, Buf().u32(2).u32(5).str("Venerable").str("").u32(9).str("").str("the Brave").b);
    auto t = decode_title_list(list);
    REQUIRE(t);
    REQUIRE(t->size() == 2);
    CHECK((*t)[0].id == 5);
    CHECK((*t)[0].prefix == "Venerable");
    CHECK((*t)[1].suffix == "the Brave");
    CHECK(bytes_of(encode_title_list(*t)) == bytes_of(list));
    CHECK(!decode_title_list(packet(Op::SendTitleList, Buf().u32(1).u32(5).b)));
    CHECK(bytes_of(encode_set_title(true, 9)) == Buf().u32(1).u32(9).b);
    CHECK(bytes_of(encode_set_title(false, 0)) == Buf().u32(0).u32(0).b);
    Buf reply = Buf().u32(1);
    const char* text = "the Brave";
    reply.b.insert(reply.b.end(), text, text + std::strlen(text));
    reply.zeros(32 - std::strlen(text)).u32(42);
    auto r = decode_title_reply(packet(Op::SetTitleReply, reply.b));
    REQUIRE(r);
    CHECK(r->suffix);
    CHECK(r->text == "the Brave");
    CHECK(r->spawn_id == 42);
    CHECK(bytes_of(encode(*r)) == reply.b);
}

TEST_CASE("tribute packets in EQEmu's layouts (big-endian where the server writes so)") {
    CHECK(encode_send_tributes().data.size() == 4); // Tribute_SendPendingRequests sends 4 unused bytes
    CHECK(encode_request_guild_tributes().data.size() == 4);
    // SendTributes: htonl'd id, tier count and tiers, RoF2's unknown128, the name.
    auto be = [](Buf& b, std::uint32_t v) { b.u8(std::uint8_t(v >> 24)).u8(std::uint8_t(v >> 16)).u8(std::uint8_t(v >> 8)).u8(std::uint8_t(v)); };
    Buf info;
    be(info, 7);
    be(info, 2);
    be(info, 1); be(info, 31001); be(info, 50);
    be(info, 30); be(info, 31002); be(info, 120);
    info.zeros(8 * 12).u32(0).str("Might");
    auto a = decode_tribute_info(packet(Op::TributeInfo, info.b));
    REQUIRE(a);
    CHECK(a->id == 7);
    CHECK(a->tier_count == 2);
    CHECK(a->tiers[1].level == 30);
    CHECK(a->tiers[1].item_id == 31002);
    CHECK(a->tiers[1].cost == 120);
    CHECK(a->name == "Might");
    CHECK(a->tiers_used() == 2); // from the tiers, not the count field (EQEmu sends a DB column there)
    CHECK(bytes_of(encode(*a)) == info.b);

    TributeSettings st;
    st.active = true;
    st.tributes[0] = 7;
    st.tiers[0] = 1;
    st.master_id = 99;
    auto sb = bytes_of(encode(st));
    REQUIRE(sb.size() == kTributeSettingsSize);
    CHECK(u32_at(sb, 0) == 1);
    CHECK(u32_at(sb, 4) == 7);
    CHECK(u32_at(sb, 8) == 0xffffffffu);
    CHECK(u32_at(sb, 24) == 1);
    CHECK(u32_at(sb, 44) == 99);
    auto back = decode_tribute_settings(encode(st));
    REQUIRE(back);
    CHECK(back->tributes[0] == 7);
    CHECK(back->tiers[0] == 1);

    CHECK(decode_tribute_timer(packet(Op::TributeTimer, Buf().u32(600000).b)) == 600000u);
    auto pts = decode_tribute_points(packet(Op::TributePointUpdate, Buf().u32(345).u32(0).u32(1200).u32(0).b));
    REQUIRE(pts);
    CHECK(pts->points == 345);
    CHECK(pts->career == 1200);
    CHECK(bytes_of(encode_tribute_toggle(true)) == Buf().u32(1).b);
    CHECK(bytes_of(encode(StartTribute{5, 99, 0})) == Buf().u32(5).u32(99).u32(0).b);
    CHECK(decode_start_tribute(packet(Op::OpenTributeMaster, Buf().u32(5).u32(99).u32(1).b))->response == 1);
    CHECK(bytes_of(encode_select_tribute(5, 7)) == Buf().u32(5).u32(7).u32(0).b);
    auto d = decode_tribute_description(packet(Op::SelectTribute, Buf().u32(5).u32(7).str("Adds strength.").b));
    REQUIRE(d);
    CHECK(d->tribute_id == 7);
    CHECK(d->text == "Adds strength.");
    TributeItem ti;
    ti.slot = ItemSlot::possessions(kSlotCursor);
    ti.quantity = 1;
    ti.master_id = 99;
    auto tb = bytes_of(encode(ti));
    REQUIRE(tb.size() == 24);
    CHECK(tb[4] == 33);
    CHECK(u32_at(tb, 12) == 1);
    CHECK(u32_at(tb, 16) == 99);
    CHECK(decode_tribute_item(encode(ti))->master_id == 99);
    CHECK(bytes_of(encode(TributeMoney{10, 99, 0})) == Buf().u32(10).u32(99).u32(0).b);
    CHECK(decode_tribute_money(packet(Op::TributeMoney, Buf().u32(10).u32(99).u32(25).b))->points == 25);
}

TEST_CASE("potion belt: OP_PotionBelt is EQEmu's MovePotionToBelt_Struct") {
    CHECK(bytes_of(encode_potion_belt(false, 2, 13010)) == Buf().u32(0).u32(2).u32(13010).b);
    CHECK(bytes_of(encode_potion_belt(true, 4, 0)) == Buf().u32(1).u32(4).u32(0).b);
}

TEST_CASE("bazaar rules: prices, the INI key and the compact coin text") {
    CHECK(game::is_bazaar_zone(151));
    CHECK(!game::is_bazaar_zone(202));
    CHECK(game::split_price(12345) == std::array<std::uint32_t, 4>{12, 3, 4, 5});
    CHECK(game::join_price(12, 3, 4, 5) == 12345);
    CHECK(game::join_price(4294968u, 0, 0, 0) == 0xffffffffu); // held, not wrapped to 704
    // The price buttons reach the largest 32-bit price (a deliberate difference: the original stops at 1,000,000 pp).
    CHECK(game::trader_coin_max(0) == 4294967);
    CHECK(game::trader_coin_max(1) == 100);
    CHECK(game::trader_coin_max(3) == 100);
    CHECK(game::join_price(static_cast<std::uint32_t>(game::trader_coin_max(0)), 0, 0, 0) == 4294967000u);
    CHECK(game::join_price(static_cast<std::uint32_t>(game::trader_coin_max(0)), 9, 9, 9) == 4294967295u);
    CHECK(game::join_price(static_cast<std::uint32_t>(game::trader_coin_max(0)), 100, 100, 100) == 0xffffffffu);
    CHECK(game::coin_compact(12345) == "12p 3g 4s 5c");
    CHECK(game::coin_compact(1000) == "1p");
    CHECK(game::coin_compact(105) == "1g 5c");
    CHECK(game::coin_compact(0) == "0c");
    CHECK(game::coin_compact(7) == "7c");
    Item plain;
    plain.id = 5023;
    plain.charges = 1;
    CHECK(game::trader_price_key(plain) == "5023");
    Item clicky = plain;
    clicky.effects[kEffectClick].effect = 200;
    clicky.charges = 3;
    CHECK(game::trader_price_key(clicky) == "5023:3");
}

TEST_CASE("the trader slots come from Trader's Satchels with their stored prices") {
    game::Inventory inv;
    Item satchel;
    satchel.id = 17000;
    satchel.item_class = 1;
    satchel.bag_type = game::kBagTraderSatchel;
    satchel.bag_slots = 8;
    Item other = satchel;
    other.bag_type = 1;
    inv.put(ItemSlot::possessions(24), satchel);
    inv.put(ItemSlot::possessions(25), other);
    Item a;
    a.id = 5023;
    a.name = "Fine Steel Sword";
    a.serial = "0000000000000001";
    inv.put(ItemSlot::possessions(24, 0), a);
    Item b = a;
    b.id = 13073;
    b.name = "Bone Chips";
    b.serial = "0000000000000002";
    inv.put(ItemSlot::possessions(24, 3), b);
    inv.put(ItemSlot::possessions(25, 0), a); // not a satchel
    game::TraderPrices prices;
    prices.set(a, 900);
    auto load = game::load_trader_items(inv, prices);
    CHECK(load.has_satchel);
    REQUIRE(load.slots.size() == 2);
    CHECK(load.slots[0].name == "Fine Steel Sword");
    CHECK(load.slots[0].price == 900);
    CHECK(load.slots[1].price == 0);
    CHECK(load.slots[1].where.same(ItemSlot::possessions(24, 3)));
}

TEST_CASE("selecting a trader slot picks the first slot with the same item and loads its price (SelectBazaarSlotItem)") {
    game::Inventory inv;
    Item satchel;
    satchel.id = 17000;
    satchel.item_class = 1;
    satchel.bag_type = game::kBagTraderSatchel;
    satchel.bag_slots = 8;
    inv.put(ItemSlot::possessions(24), satchel);
    Item a;
    a.id = 13073, a.name = "Bone Chips", a.serial = "0000000000000001";
    Item b = a;
    b.serial = "0000000000000002";
    Item c = a;
    c.id = 5023, c.name = "Fine Steel Sword", c.serial = "0000000000000003";
    inv.put(ItemSlot::possessions(24, 0), c);
    inv.put(ItemSlot::possessions(24, 1), a);
    inv.put(ItemSlot::possessions(24, 2), b);
    game::TraderPrices prices;
    auto load = game::load_trader_items(inv, prices);
    REQUIRE(load.slots.size() == 3);
    load.slots[1].price = 40;
    load.slots[2].price = 55;
    auto sel = game::select_trader_slot(load.slots, inv, 2);
    CHECK(sel.selected == 1); // the first Bone Chips
    CHECK(sel.price == 40);
    sel = game::select_trader_slot(load.slots, inv, 0);
    CHECK(sel.selected == 0);
    CHECK(sel.price == 0);
    CHECK(game::select_trader_slot(load.slots, inv, 7).selected == -1);
}

// ---------------------------------------------------------------- mail

TEST_CASE("mail commands go to the chat server as MailMgr writes them") {
    auto p = encode_mail_bracket_command("[getheaders");
    CHECK(p.opcode == ucs::kMail);
    CHECK(bytes_of(p) == Buf().str(";getheaders").u8(0).b);
    CHECK(bytes_of(encode_mail_bracket_command("[setmessagestatus TRASH 5 6")) == Buf().str(";setmessagestatus TRASH 5 6").u8(0).b);
    CHECK(game::mail_qualify_address("Bob", "nms") == "SOE.EQ.nms.Bob");
    CHECK(game::mail_qualify_address("nms.Bob", "x") == "SOE.EQ.nms.Bob");
    CHECK(game::mail_qualify_address("EQ.nms.Bob", "x") == "SOE.EQ.nms.Bob");
    CHECK(game::mail_qualify_address("SOE.EQ.nms.Bob", "x") == "SOE.EQ.nms.Bob");
    CHECK(game::mail_qualify_address("Mr Bob", "nms") == "SOE.EQ.nms.Mr_Bob");
    CHECK(game::mail_expand_recipients("Bob, Ann,bob ,-Cid", "nms") ==
          "SOE.EQ.nms.Bob, SOE.EQ.nms.Ann, SOE.EQ.nms.bob, -SOE.EQ.nms.Cid");
    // The de-duplication looks for ", name" in what is written so far, so a repeat
    // of the first recipient stays (as in the original).
    CHECK(game::mail_expand_recipients("Ann, Bob, Bob", "nms") == "SOE.EQ.nms.Ann, SOE.EQ.nms.Bob");
    CHECK(game::mail_expand_recipients("Bob, Bob", "nms") == "SOE.EQ.nms.Bob, SOE.EQ.nms.Bob");
    CHECK(game::mail_to_command("Bob", "Hi there", "the body", "nms") == "[mailto \"SOE.EQ.nms.Bob\" \"Hi there\" the body");
    CHECK(game::mail_to_command("Bob", "Hi", "x y", "nms") == "[mailto \"SOE.EQ.nms.Bob\" Hi x y");
}

TEST_CASE("mail address book: entries, the file and the expansion") {
    game::MailAddressBook book;
    CHECK_FALSE(book.put({"  ", {"x"}, false, {}}));
    CHECK(book.put({"Friends", game::mail_split_addresses("Ann, Bob  nms2.Cid"), true, "my\tfriends"}));
    CHECK(book.put({"alts", {"Dee"}, false, {}}));
    CHECK(book.put({"Empty", {}, false, {}}));
    REQUIRE(book.entries.size() == 3);
    CHECK(book.entries[0].name == "alts"); // sorted ignoring case
    CHECK(book.entries[2].addresses == std::vector<std::string>{"Ann", "Bob", "nms2.Cid"});
    CHECK(book.entries[2].note == "my\tfriends");
    REQUIRE(book.find(" friends "));
    CHECK(book.find("FRIENDS")->is_private);
    CHECK(book.put({"FRIENDS", {"Eve"}, false, {}})); // replaces, keeping one entry
    CHECK(book.entries.size() == 3);
    CHECK(book.find("friends")->addresses == std::vector<std::string>{"Eve"});
    book.put({"Friends", {"Ann", "Bob", "nms2.Cid"}, true, {}});

    auto back = game::MailAddressBook::parse(book.save_text());
    REQUIRE(back.entries.size() == 3);
    CHECK(back.find("Friends")->addresses.size() == 3);
    CHECK(back.find("Friends")->is_private);
    CHECK(back.find("alts")->addresses == std::vector<std::string>{"Dee"});
    // This client's earlier file (one tab-separated entry a line) is still read.
    CHECK(game::MailAddressBook::parse("Solo\t0\tZed\r\n\n").entries.size() == 1);

    // As EQEmu's ParseRecipients reads them: "Name <addr addr>", '-' for hidden.
    CHECK(game::mail_expand_recipients("Friends, alts, Zed", "nms", &book) ==
          "-Friends <SOE.EQ.nms.Ann SOE.EQ.nms.Bob SOE.EQ.nms2.Cid>, alts <SOE.EQ.nms.Dee>, SOE.EQ.nms.Zed");
    CHECK(game::mail_expand_recipients("-alts", "nms", &book) == "-alts <SOE.EQ.nms.Dee>");
    CHECK(game::mail_expand_recipients("Empty, Zed", "nms", &book) == "SOE.EQ.nms.Zed"); // nothing to send to
    std::vector<game::MailAddressEntry> lists{{"Guild", {"Gus", "Hal"}, false, {}}};
    CHECK(game::mail_expand_recipients("guild", "nms", &book, &lists) == "Guild <SOE.EQ.nms.Gus SOE.EQ.nms.Hal>");
    CHECK(game::mail_to_command("alts", "Hi", "x", "nms", &book) == "[mailto \"alts <SOE.EQ.nms.Dee>\" Hi x");
}

TEST_CASE("mail address book: the original's file (AddressBook_Save / AddressBook_Parse)") {
    game::MailAddressBook book;
    // AddressBookEntry_Ctor qualifies the addresses and normalizes the note: trailing blanks and line breaks go,
    // an empty line inside gets a space.
    CHECK(book.put({"Friends", {"Ann", "nms2.Cid"}, true, "line one\n\nline three \n\n"}, "nms"));
    CHECK(book.put({"alts", {"Dee"}, false, {}}, "nms"));
    CHECK(book.find("friends")->addresses == std::vector<std::string>{"SOE.EQ.nms.Ann", "SOE.EQ.nms2.Cid"});
    CHECK(book.find("friends")->note == "line one\n \nline three");
    CHECK(game::mail_normalize_note("a\n\n\nb") == "a\n \n \nb");
    const std::string file = book.save_text();
    CHECK(file ==
          "EverQuest Address Book v1.0\n"
          "# WARNING: To avoid errors, you should modify your address book using the game interface.\n"
          "# For those of you who like to live dangerously, just be aware that empty lines separate address book "
          "entries.\n"
          "# The game normalizes notes by inserting a space in empty lines.\n"
          "Name: alts\nAddresses: SOE.EQ.nms.Dee\nFlags: \nNote: \n\n"
          "Name: Friends\nAddresses: SOE.EQ.nms.Ann SOE.EQ.nms2.Cid\nFlags: PRIVATE\nNote: line one\n \nline three\n\n");
    game::MailAddressBook back;
    CHECK(game::MailAddressBook::parse(file, {}, back));
    REQUIRE(back.entries.size() == 2);
    CHECK(back.entries[1].name == "Friends");
    CHECK(back.entries[1].is_private);
    CHECK(back.entries[1].note == "line one\n \nline three");
    CHECK(back.entries[1].addresses == book.entries[1].addresses);
    CHECK_FALSE(back.entries[0].is_private);
    // Written by the original on Windows: CRLF; "Flags:" may be left out; comments before the first entry.
    game::MailAddressBook win;
    CHECK(game::MailAddressBook::parse("EverQuest Address Book v1.0\r\n# c\r\n\r\nName: Bob\r\nAddresses: SOE.EQ.x.Bob\r\n"
                                       "Note: hi\r\nthere\r\n\r\n",
                                       {}, win));
    REQUIRE(win.entries.size() == 1);
    CHECK(win.entries[0].note == "hi\nthere");
    CHECK_FALSE(win.entries[0].is_private);
    // A wrong header reads nothing; a malformed entry stops the parse, keeping the entries before it.
    game::MailAddressBook bad;
    CHECK_FALSE(game::MailAddressBook::parse("Address Book\nName: A\n", {}, bad));
    CHECK(bad.entries.empty());
    CHECK_FALSE(game::MailAddressBook::parse("EverQuest Address Book v1.0\nName: A\nAddresses: a\nNote: \n\nName: B\nNote: x\n",
                                             {}, bad));
    CHECK(bad.entries.size() == 1);
    // An empty book (the header and the comments) is no entry, and AddressBook_Parse says so.
    game::MailAddressBook none;
    CHECK_FALSE(game::MailAddressBook::parse(game::MailAddressBook{}.save_text(), {}, none));
    CHECK(none.entries.empty());

}

TEST_CASE("mail packets from the chat server decode into their fields") {
    // Client::SendMailBoxes.
    Buf login;
    login.u8(1).u32(2).u32(0).str("SOE.EQ.nms.Nmscmnine,SOE.EQ.nms.Alt").u8(0);
    auto boxes = decode_chat_server_packet(packet(ucs::kMailLogin, login.b));
    REQUIRE(boxes);
    CHECK(boxes->kind == ChatServerEvent::Kind::MailboxList);
    CHECK(boxes->fields == std::vector<std::string>{"SOE.EQ.nms.Nmscmnine", "SOE.EQ.nms.Alt"});
    // UCSDatabase::SendHeaders: the count, then one header per mail.
    auto count = decode_chat_server_packet(encode_ucs_strings(ucs::kMailHeaderCount, {"0", "25015275", "1", "3"}));
    REQUIRE(count);
    CHECK(count->kind == ChatServerEvent::Kind::MailHeaderCount);
    CHECK(count->fields[3] == "3");
    auto h = decode_chat_server_packet(
        encode_ucs_strings(ucs::kMailHeader, {"0", "25015275", "0", "17", "1700000000", "1", "SOE.EQ.nms.Bob", "Hello"}));
    REQUIRE(h);
    CHECK(h->kind == ChatServerEvent::Kind::MailHeader);
    REQUIRE(h->fields.size() == 8);
    CHECK(h->fields[6] == "SOE.EQ.nms.Bob");
    // UCSDatabase::SendBody: mailbox, id, body, "1", 0, "\nTO:<to>\n" (no final NUL).
    Buf body;
    body.str("0").str("17").str("The body").str("1").u8(0);
    for (char c : std::string("\nTO:SOE.EQ.nms.Nmscmnine\n")) body.u8(static_cast<std::uint8_t>(c));
    auto bd = decode_chat_server_packet(packet(ucs::kMailSendBody, body.b));
    REQUIRE(bd);
    CHECK(bd->kind == ChatServerEvent::Kind::MailBody);
    REQUIRE(bd->fields.size() == 6);
    CHECK(bd->fields[2] == "The body");
    CHECK(bd->fields[5] == "\nTO:SOE.EQ.nms.Nmscmnine\n");
    // ProcessMailTo's delivery status: success, and a failure.
    auto ok = decode_chat_server_packet(encode_ucs_strings(ucs::kMailDeliveryStatus, {"1", "", "test", "1"}));
    REQUIRE(ok);
    CHECK(ok->fields[3] == "1");
    Buf fail;
    fail.str("1").u8(0x20).str("SOE.EQ.nms.Nobody").str("Subj").str("0").u16(0x3237).u8(0);
    auto bad = decode_chat_server_packet(packet(ucs::kMailDeliveryStatus, fail.b));
    REQUIRE(bad);
    CHECK(bad->fields[1] == " SOE.EQ.nms.Nobody");
    CHECK(bad->fields[3] == "0");
    auto nw = decode_chat_server_packet(encode_ucs_strings(ucs::kMailNew, {"0", "18", "1700000001", "1", "SOE.EQ.nms.Ann", "Yo"}));
    REQUIRE(nw);
    CHECK(nw->kind == ChatServerEvent::Kind::MailNew);
    auto ch = decode_chat_server_packet(encode_ucs_strings(ucs::kMailboxChange, {"1"}));
    REQUIRE(ch);
    CHECK(ch->kind == ChatServerEvent::Kind::MailboxChange);
    CHECK(ch->fields[0] == "1");
}

TEST_CASE("mail state: statuses, inbox and trash") {
    CHECK(game::mail_status_from_server(1) == game::MailStatus::New);
    CHECK(game::mail_status_from_server(3) == game::MailStatus::Read);
    CHECK(game::mail_status_from_server(4) == game::MailStatus::Trash);
    CHECK(std::string(game::mail_status_word(game::MailStatus::Deleted)) == "DELETED");
    game::MailState m;
    m.store({5, 100, game::MailStatus::Read, "SOE.EQ.nms.A", "a", {}, {}});
    m.store({6, 200, game::MailStatus::New, "SOE.EQ.nms.B", "b", {}, {}});
    m.store({7, 150, game::MailStatus::Trash, "SOE.EQ.nms.C", "c", {}, {}});
    CHECK(m.unread);
    CHECK(m.max_id == 7);
    auto in = m.inbox();
    REQUIRE(in.size() == 2);
    CHECK(in[0]->id == 6);
    CHECK(m.trash().size() == 1);
    CHECK(game::mail_short_name("SOE.EQ.nms.Bob") == "Bob");
}

TEST_CASE("guild tribute: the list, the benefits, opting in and donations") {
    TributeAbility a;
    a.id = 5;
    a.tier_count = 1;
    a.tiers[0] = {20, 1001, 300};
    a.name = "Guild Power";
    AppPacket info = encode_guild_tribute_info(a);
    CHECK(info.opcode == to_wire(Op::SendGuildTributes));
    auto d = decode_guild_tribute_info(info);
    REQUIRE(d);
    CHECK(d->id == 5);
    CHECK(d->name == "Guild Power");
    CHECK(d->tiers[0].cost == 300);

    CHECK(encode_guild_select_tribute(5, 1).data.size() == 16); // GuildTributeSelectReq_Struct, size checked exactly

    GuildTributeActive act;
    act.favor = 900;
    act.timer_ms = 600000;
    act.enabled = true;
    act.guild_id = 3;
    act.tributes = {5, kTributeNone};
    act.tiers = {1, 0};
    AppPacket ap = encode(act);
    REQUIRE(ap.data.size() == 40); // GuildTributeSendActive_Struct
    auto da = decode_guild_tribute_active(ap);
    REQUIRE(da);
    CHECK(da->enabled);
    CHECK(da->guild_id == 3);
    CHECK(da->tributes[0] == 5);
    CHECK(da->tiers[0] == 1);

    CHECK(encode_guild_save_tributes(77, act).data.size() == 48);       // GuildTributeSaveActive_Struct
    CHECK(encode_guild_modify_benefits(true, 77, act).data.size() == 80); // GuildTributeModifyBenefits_Struct
    CHECK(encode_guild_tribute_toggle(true).data.size() == 12);          // GuildTributeToggleReq_Struct
    CHECK(encode_guild_tribute_opt(3, true, "Me").data.size() == 80);    // GuildTributeOptInOutReq_Struct

    AppPacket optp = encode(GuildTributeOpt{"Me", true, true, 4, 1700000000, 0x2fb});
    CHECK(optp.data.size() == 88); // GuildTributeOptInOutReply_Struct
    auto opt = decode_guild_tribute_opt(optp);
    REQUIRE(opt);
    CHECK(opt->member == "Me");
    CHECK(opt->on);
    CHECK(opt->trophy_on);
    CHECK(opt->donations == 4);
    CHECK(opt->time == 1700000000);
    CHECK(opt->command == 0x2fb);
    optp.data.resize(87);
    CHECK_FALSE(decode_guild_tribute_opt(optp));
    // The trophy opt-out: opcode 0x02bc (not in EQEmu's table), OP_GuildOptInOut's request layout.
    AppPacket trophy = encode_guild_trophy_opt(3, false, "Me");
    CHECK(trophy.opcode == kOpGuildTrophyOptInOut);
    CHECK(trophy.data.size() == 80);
    CHECK(trophy.data == encode_guild_tribute_opt(3, false, "Me").data);

    AppPacket item = encode_guild_donate_item(ItemSlot::possessions(kSlotCursor), 2, 77, 3);
    REQUIRE(item.data.size() == 36); // RoF2 GuildTributeDonateItemRequest_Struct
    auto rep = decode_guild_donate_item(encode(GuildTributeDonation{ItemSlot::possessions(kSlotCursor), 2, 150}));
    REQUIRE(rep);
    CHECK(rep->slot.slot == kSlotCursor);
    CHECK(rep->favor == 150);
    CHECK(encode_guild_donate_platinum(10, 77, 3).data.size() == 20);
    std::vector<std::byte> plat(12);
    plat[0] = std::byte{10};
    plat[8] = std::byte{50};
    auto donation = decode_guild_donate_platinum(AppPacket(to_wire(Op::GuildTributeDonatePlat), plat));
    REQUIRE(donation);
    CHECK(donation->platinum == 10);
    CHECK(donation->favor == 50);
    for (std::size_t size = 0; size < 12; ++size) {
        plat.resize(size);
        CHECK_FALSE(decode_guild_donate_platinum(AppPacket(to_wire(Op::GuildTributeDonatePlat), plat)));
    }
}

#include "net/social_packets.h"

TEST_CASE("lfguild: the entries' fixed layouts and the searches' matches") {
    LFGuildPlayer pl{"LF raid guild", 14, 0, true};
    AppPacket p = encode(pl);
    REQUIRE(p.data.size() == 624); // LFGuild_PlayerToggle_Struct, size checked
    CHECK(std::string(reinterpret_cast<const char*>(p.data.data() + 72)) == "LF raid guild");
    auto dp = decode_lfguild(p);
    REQUIRE(dp);
    REQUIRE(dp->player);
    CHECK(dp->player->time_zone == 14);
    CHECK(dp->player->on);

    LFGuildGuild g{"Recruiting", 50, 100, 0x5, 100, 3, 0, true};
    AppPacket gp = encode(g);
    REQUIRE(gp.data.size() == 616); // LFGuild_GuildToggle_Struct
    auto dg = decode_lfguild(gp);
    REQUIRE(dg);
    REQUIRE(dg->guild);
    CHECK(dg->guild->classes == 5);
    CHECK(dg->guild->to_level == 100);

    CHECK(encode_lfguild_search_players(1, 100, 0, 0, 0xffff).data.size() == 28); // LFGuild_SearchPlayer_Struct
    CHECK(encode_lfguild_search_guilds(60, 50, 0, 1).data.size() == 24);          // LFGuild_SearchGuild_Struct

    auto pm = decode_lfguild(encode(std::vector<LFGuildPlayerMatch>{{"Seeker", "hi", 60, 3, 100, 2}}));
    REQUIRE(pm);
    REQUIRE(pm->players);
    REQUIRE(pm->players->size() == 1);
    CHECK((*pm->players)[0].class_ == 3);
    CHECK((*pm->players)[0].comment == "hi");
    auto gm = decode_lfguild(encode(std::vector<LFGuildGuildMatch>{{"The Guild", "join us", 5}}));
    REQUIRE(gm);
    REQUIRE(gm->guilds);
    CHECK((*gm->guilds)[0].time_zone == 5);
}

TEST_CASE("membership refusals: GameFeatureRestrictions_FormatMessage's lines") {
    std::optional<net::MembershipDetails> none;
    CHECK(game::feature_refused_message(game::kFeatureBuyLines, none) ==
          "Your ability to create buy lines in the bazaar has been disabled until you purchase this feature.");
    CHECK(game::feature_refused_message(0, none) == "Your AAs have been limited until you purchase this feature.");
    CHECK(game::feature_refused_message(22, none).empty());
    net::MembershipDetails d;
    d.settings = {{0, 0xf, 0}, {1, 0xf, 0}, {2, 0xf, -1}, {1, 6, 1}, {0, 0xe, 0}, {0, 0xe, 1}};
    std::optional<net::MembershipDetails> det = d;
    CHECK(game::feature_membership_level(0xf, det) == 2);
    CHECK(game::feature_membership_level(6, det) == 1);
    CHECK(game::feature_membership_level(0xe, det) == -1); // only a level's first entry counts
    CHECK(game::feature_refused_message(6, det) ==
          "Your ability to send mail has been disabled because you do not have at least a silver membership.");
}

TEST_CASE("bazaar: the trader stall switch types and the find opcode") {
    CHECK(game::is_trader_stall(0x99));
    CHECK(game::is_trader_stall(0x9b));
    CHECK_FALSE(game::is_trader_stall(0x9a));
    CHECK_FALSE(game::is_trader_stall(0x39));
    auto p = encode_find_market_spawn(0x1234);
    CHECK(p.opcode == 0x0e44);
    CHECK(bytes_of(p) == std::vector<std::uint8_t>{0x34, 0x12, 0, 0});
}
