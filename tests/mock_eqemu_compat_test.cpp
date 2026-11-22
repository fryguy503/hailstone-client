// What the client adds so that EQEmu, which lacks some of the original server's answers, still tells the other
// player: the duel refusal, a declined group invitation reaching the inviter, a failed bazaar purchase. And what it
// sends so that stock EQEmu's decoders get the sizes and string lengths they assume.

#include <doctest/doctest.h>
#include <initializer_list>
#include <string_view>

#include "mock_server/harness.h"
#include "net/group_packets.h"
#include "net/market_packets.h"
#include "net/player_packets.h"
#include "net/trade_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

constexpr std::uint32_t kRival = 50;

bool add_rival(Rig& rig) {
    rig.server.add_spawn(make_spawn("Rival", kRival, 0, 1, 10.0f, 10.0f, 3.0f, 0.0f));
    return rig.until([&] { return rig.conn.world().spawns.count(kRival) == 1; });
}

std::vector<const AppPacket*> sent(const Rig& rig, std::uint16_t opcode) {
    std::vector<const AppPacket*> out;
    for (const auto& p : rig.server.zone_log)
        if (p.opcode == opcode) out.push_back(&p);
    return out;
}

std::uint32_t u32_at(const AppPacket& p, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) v |= std::to_integer<std::uint32_t>(p.data[at + i]) << (8 * i);
    return v;
}

AppPacket formatted(std::uint32_t id, std::uint32_t color, std::initializer_list<std::string_view> args) {
    ByteWriter w;
    w.le(std::uint32_t{0});
    w.le(id);
    w.le(color);
    for (auto arg : args) w.fixed_string(arg, arg.size() + 1);
    w.le(std::uint8_t{0});
    return AppPacket(to_wire(Op::FormattedMessage), w.take());
}

} // namespace

TEST_CASE("mock server: considering corpses requests and displays the server's decay and resurrection timers") {
    Rig rig;
    REQUIRE(rig.to_zone());
    constexpr std::uint32_t kNpcCorpse = 51;
    constexpr std::uint32_t kPlayerCorpse = 52;
    constexpr std::uint16_t kCorpseConsider = 0x5204;
    constexpr std::uint16_t kLivingConsider = 0x742b;
    // EQEmu sends spawn type 2 for either kind of corpse.
    rig.server.add_spawn(make_spawn("a_rat's corpse", kNpcCorpse, 2, 1, 10.0f, 10.0f, 3.0f, 0.0f));
    rig.server.add_spawn(make_spawn("Rival's corpse", kPlayerCorpse, 2, 1, 12.0f, 12.0f, 3.0f, 0.0f));
    REQUIRE(rig.until([&] {
        const auto& spawns = rig.conn.world().spawns;
        return spawns.contains(kNpcCorpse) && spawns.contains(kPlayerCorpse);
    }));
    auto check_request = [&](std::uint16_t opcode, std::size_t previous_count, std::uint32_t target) {
        REQUIRE(rig.until([&] { return sent(rig, opcode).size() > previous_count; }));
        auto requests = sent(rig, opcode);
        REQUIRE(requests.size() == previous_count + 1);
        const auto& request = *requests.back();
        REQUIRE(request.data.size() == 20);
        CHECK(u32_at(request, 0) == rig.conn.world().player_id);
        CHECK(u32_at(request, 4) == target);
        CHECK(u32_at(request, 8) == 0);
        CHECK(u32_at(request, 12) == 0);
        CHECK(u32_at(request, 16) == 0);
    };

    rig.conn.set_target(kNpcCorpse);
    rig.conn.consider();
    check_request(kCorpseConsider, 0, kNpcCorpse);
    rig.server.zone.send_reliable(formatted(495, 10, {"7", "29"})); // NPCQuestSay

    rig.conn.set_target(kPlayerCorpse);
    rig.conn.consider();
    check_request(kCorpseConsider, 1, kPlayerCorpse);
    rig.server.zone.send_reliable(formatted(491, 0, {"2", "46", "9"})); // White
    rig.server.zone.send_reliable(formatted(493, 0, {"6", "19", "42", "8"}));

    std::vector<ChatEvent> lines;
    REQUIRE(rig.until([&] {
        for (auto& line : rig.chat()) lines.push_back(std::move(line));
        return has_line(lines, "CORPSE DECAY 6d 19h 42m 8s");
    }));
    CHECK_FALSE(has_line(lines, "[12241]")); // No local refusal for either corpse.
    CHECK(has_line(lines, "CORPSE DECAY 7m 29s"));
    CHECK(has_line(lines, "CORPSE REZ 2h 46m 9s"));
    for (const auto& line : lines) {
        if (line.text == "CORPSE DECAY 7m 29s") CHECK(line.color == 10);
    }

    CHECK(sent(rig, kLivingConsider).empty());
    REQUIRE(add_rival(rig));
    rig.conn.set_target(kRival);
    rig.conn.consider();
    check_request(kLivingConsider, 0, kRival);
    CHECK(sent(rig, kCorpseConsider).size() == 2);
}

TEST_CASE("mock server: /consider refuses without a target and for a ship or boat, as Cmd_consider does") {
    Rig rig;
    REQUIRE(rig.to_zone());
    constexpr std::uint32_t kBoat = 61;
    constexpr std::uint32_t kShipCorpse = 62;
    constexpr std::uint16_t kCorpseConsider = 0x5204;
    constexpr std::uint16_t kLivingConsider = 0x742b;
    // Race 141 (BOAT) and 72 (SHIP) carry the race table's vehicle flag (PlayerClient_IsVehicleRace @0x59e530).
    rig.server.add_spawn(make_spawn("a_boat", kBoat, 1, 141, 10.0f, 10.0f, 3.0f, 0.0f));
    rig.server.add_spawn(make_spawn("a_ship's corpse", kShipCorpse, 2, 72, 12.0f, 12.0f, 3.0f, 0.0f));
    REQUIRE(add_rival(rig));
    REQUIRE(rig.until([&] {
        const auto& spawns = rig.conn.world().spawns;
        return spawns.contains(kBoat) && spawns.contains(kShipCorpse);
    }));

    std::vector<ChatEvent> lines;
    auto wait_line = [&](const std::string& text) {
        return rig.until([&] {
            for (auto& line : rig.chat()) lines.push_back(std::move(line));
            return has_line(lines, text);
        });
    };

    // No target: 13063 "Consider whom?", not the reply handler's 12240.
    REQUIRE(rig.conn.world().target_id == 0);
    rig.conn.consider();
    REQUIRE(wait_line("[13063]"));
    CHECK_FALSE(has_line(lines, "[12240]"));

    // A living vehicle: 13064 "You can't consider that." and nothing sent.
    rig.conn.set_target(kBoat);
    rig.conn.consider();
    REQUIRE(wait_line("[13064]"));

    // The check is on the living branch only: a vehicle's corpse still sends OP_ConsiderCorpse.
    rig.conn.set_target(kShipCorpse);
    rig.conn.consider();
    REQUIRE(rig.until([&] { return !sent(rig, kCorpseConsider).empty(); }));
    CHECK(u32_at(*sent(rig, kCorpseConsider).back(), 4) == kShipCorpse);

    // An ordinary living target still sends OP_Consider; it is the only one sent.
    rig.conn.set_target(kRival);
    rig.conn.consider();
    REQUIRE(rig.until([&] { return !sent(rig, kLivingConsider).empty(); }));
    auto living = sent(rig, kLivingConsider);
    REQUIRE(living.size() == 1);
    CHECK(u32_at(*living.front(), 4) == kRival);
    CHECK(sent(rig, kCorpseConsider).size() == 1);
}

TEST_CASE("mock server: /decline sends the original's 0x52f7 and EQEmu's 12-byte OP_DuelDecline") {
    Rig rig;
    REQUIRE(rig.to_zone());
    REQUIRE(add_rival(rig));
    rig.server.zone.send_reliable(encode(DuelRequest{kRival, kRival}));
    REQUIRE(rig.until([&] { return rig.conn.duel_state().challenger == kRival; }));
    rig.conn.decline_duel();
    const std::uint16_t decline = to_wire(Op::DuelDecline);
    REQUIRE(rig.until([&] { return !sent(rig, decline).empty(); }));
    auto refusals = sent(rig, kOpDuelRefusal);
    REQUIRE(refusals.size() == 1);
    CHECK(u32_at(*refusals[0], 0) == kRival);
    auto declines = sent(rig, decline);
    REQUIRE(declines.size() == 1);
    REQUIRE(declines[0]->data.size() == kDuelResponseSize);
    CHECK(u32_at(*declines[0], 0) == kRival); // target_id: the challenger, who is told
    CHECK(u32_at(*declines[0], 4) == 7);      // entity_id: us
    CHECK(u32_at(*declines[0], 8) == 0);
    CHECK(rig.conn.duel_state().challenger == 0);
}

TEST_CASE("mock server: a refusal while our own duel runs sends no EQEmu decline") {
    Rig rig;
    REQUIRE(rig.to_zone());
    REQUIRE(add_rival(rig));
    rig.server.add_spawn(make_spawn("Other", 51, 0, 1, 12.0f, 12.0f, 3.0f, 0.0f));
    REQUIRE(rig.until([&] { return rig.conn.world().spawns.count(51) == 1; }));
    // We challenge Rival and Rival accepts: the duel is on.
    rig.conn.set_target(kRival);
    rig.conn.duel();
    rig.server.zone.send_reliable(encode(DuelRequest{kRival, kRival}));
    REQUIRE(rig.until([&] { return rig.conn.duel_state().active; }));
    // Another challenge meanwhile is refused with kind 1 only.
    rig.server.zone.send_reliable(encode(DuelRequest{51, 51}));
    REQUIRE(rig.until([&] { return !sent(rig, kOpDuelRefusal).empty(); }));
    rig.run_for(50ms);
    CHECK(sent(rig, to_wire(Op::DuelDecline)).empty());
    CHECK(rig.conn.duel_state().active);
}

TEST_CASE("mock server: EQEmu's 1383 line from the challenged player ends our challenge") {
    Rig rig;
    REQUIRE(rig.to_zone());
    REQUIRE(add_rival(rig));
    rig.conn.set_target(kRival);
    rig.conn.duel();
    REQUIRE(rig.conn.duel_state().challenged == kRival);
    // Someone else's name does not.
    rig.server.zone.send_reliable(formatted(1383, 15, {"Nobody"}));
    REQUIRE(rig.until([&] { return has_line(rig.chat(), "[1383] Nobody"); }));
    CHECK(rig.conn.duel_state().challenged == kRival);
    rig.server.zone.send_reliable(formatted(1383, 15, {"Rival"}));
    REQUIRE(rig.until([&] { return rig.conn.duel_state().challenged == 0; }));
    CHECK_FALSE(rig.conn.duel_state().pending());
}

TEST_CASE("mock server: EQEmu's 1383 comes without the name (Entity::GetName); the challenged player's is used") {
    Rig rig;
    REQUIRE(rig.to_zone());
    REQUIRE(add_rival(rig));
    rig.conn.set_target(kRival);
    rig.conn.duel();
    REQUIRE(rig.conn.duel_state().challenged == kRival);
    // What EQEmu sends: the id, the type and the closing NUL only (13 bytes).
    ByteWriter w;
    w.le(std::uint32_t{0});
    w.le(std::uint32_t{1383});
    w.le(std::uint32_t{10});
    w.le(std::uint8_t{0});
    rig.server.zone.send_reliable(AppPacket(to_wire(Op::FormattedMessage), w.take()));
    std::vector<ChatEvent> lines;
    REQUIRE(rig.until([&] {
        for (auto& e : rig.chat()) lines.push_back(std::move(e));
        return has_line(lines, "[1383] Rival");
    }));
    CHECK(rig.conn.duel_state().challenged == 0);
}

TEST_CASE("mock server: OP_CancelInvite from the server prints the declining player's line") {
    Rig rig;
    REQUIRE(rig.to_zone());
    GroupCancel c;
    c.inviter = "Gennro";
    c.invitee = "Rival";
    c.reason = kCancelDeclined;
    rig.server.zone.send_reliable(encode(c));
    std::vector<ChatEvent> lines;
    REQUIRE(rig.until([&] {
        for (auto& e : rig.chat()) lines.push_back(std::move(e));
        return has_line(lines, "[12266] Rival"); // %1 rejects your offer to join the group.
    }));
    c.reason = 9; // not one of the five: nothing
    rig.server.zone.send_reliable(encode(c));
    c.reason = 1;
    rig.server.zone.send_reliable(encode(c));
    REQUIRE(rig.until([&] {
        for (auto& e : rig.chat()) lines.push_back(std::move(e));
        return has_line(lines, "[12264] Rival");
    }));
    int count = 0;
    for (const auto& l : lines) count += l.text.rfind("[122", 0) == 0;
    CHECK(count == 2);
}

TEST_CASE("mock server: EQEmu's TradeRequestFailed answer reads as a failed purchase") {
    Rig rig;
    REQUIRE(rig.to_zone());
    const Coins before = rig.conn.coins();
    TraderBuy b;
    b.method = 2; // a search purchase straight to the inventory
    b.trader_id = 0xffffffffu;
    b.already_sold = 0xffffffffu;
    b.seller_name = "Seller";
    b.buyer_name = "Gennro";
    b.item_name = "Fine Steel Sword";
    b.price = 1000;
    b.quantity = 1;
    CHECK(is_eqemu_trade_failure(b));
    rig.server.zone.send_reliable(encode(b));
    std::vector<ChatEvent> lines;
    REQUIRE(rig.until([&] {
        for (auto& e : rig.chat()) lines.push_back(std::move(e));
        return has_line(lines, "Your attempt to purchase 1 Fine Steel Sword(s) from Seller was unsuccessful.");
    }));
    CHECK(rig.conn.coins().platinum == before.platinum);
    CHECK(rig.conn.coins().gold == before.gold);
    CHECK(rig.conn.coins().silver == before.silver);
    CHECK(rig.conn.coins().copper == before.copper);
    // A real success (the trader's id, nothing already sold) is still one.
    b.trader_id = 1234;
    b.already_sold = 0;
    CHECK_FALSE(is_eqemu_trade_failure(b));
}

namespace {

// The NUL-terminated string at `at`, and where the next field starts.
std::pair<std::string, std::size_t> cstring_at(const AppPacket& p, std::size_t at) {
    std::string s;
    while (at < p.data.size() && p.data[at] != std::byte{0}) s.push_back(static_cast<char>(p.data[at++]));
    return {s, at + 1};
}

} // namespace

TEST_CASE("mock server: a tell to an over-long name goes as the original's 63 + 2047 characters") {
    Rig rig;
    REQUIRE(rig.to_zone());
    // A quoted recipient may be any length; EQEmu's DECODE(OP_ChannelMessage) strcpy's it into char[64].
    const std::string to(100, 'x');
    const std::string text(3000, 'y');
    rig.conn.send_command("/tell " + to + " " + text);
    const std::uint16_t op = to_wire(Op::ChannelMessage);
    REQUIRE(rig.until([&] { return !sent(rig, op).empty(); }));
    const AppPacket& p = *sent(rig, op)[0];
    auto [sender, at] = cstring_at(p, 0);
    CHECK(sender == "Gennro");
    auto [target, at2] = cstring_at(p, at);
    CHECK(target == std::string(63, 'x'));
    CHECK(u32_at(p, at2 + 8) == 7); // the channel: tell
    auto [message, end] = cstring_at(p, at2 + 4 + 4 + 4 + 5 + 4);
    CHECK(message == std::string(0x7ff, 'y'));
    CHECK(p.data.size() == end + 15);
}

TEST_CASE("mock server: /guildstatus sends the original's 140-byte GuildStatus_Struct") {
    Rig rig;
    REQUIRE(rig.to_zone());
    rig.conn.send_command("/guildstatus Someone");
    const std::uint16_t op = to_wire(Op::GuildStatus);
    REQUIRE(rig.until([&] { return !sent(rig, op).empty(); }));
    const AppPacket& p = *sent(rig, op)[0];
    REQUIRE(p.data.size() == 140); // EQEmu's DECODE_LENGTH_EXACT(GuildStatus_Struct)
    CHECK(cstring_at(p, 0).first == "Someone");
    for (std::size_t i = 7; i < 140; ++i) CHECK(p.data[i] == std::byte{0}); // no second name (Send_OP_GuildStatus)
}

TEST_CASE("mock server: the trade partner leaving cancels and closes the trade (msgEQRmPlayer)") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& w = conn.world_for_test();
    const game::Spawn* me = w.player();
    REQUIRE(me);
    game::Spawn pc;
    pc.id = 4100;
    pc.name = "Partner";
    pc.position = me->position + glm::vec3(3.0f, 0.0f, 0.0f);
    pc.heights.avatar_height = 6.0f;
    w.spawns[pc.id] = pc;
    conn.destroy_cursor();
    conn.give_to(pc.id);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::TradeRequest) == 1; }));
    rig.server.zone.send_reliable(encode_trade_request_ack({w.player_id, pc.id}));
    REQUIRE(rig.until([&] { return conn.trade() && conn.trade()->open; }));
    // Someone else leaving changes nothing.
    rig.server.add_spawn(make_spawn("Other", 51, 0, 1, 12.0f, 12.0f, 3.0f, 0.0f));
    REQUIRE(rig.until([&] { return conn.world().spawns.count(51) == 1; }));
    rig.server.remove_spawn(51);
    REQUIRE(rig.until([&] { return conn.world().spawns.count(51) == 0; }));
    CHECK(conn.trade().has_value());
    CHECK(MockEqServer::count(rig.server.zone_log, Op::CancelTrade) == 0);
    // The partner leaving: OP_CancelTrade {partner, us} and the trade is closed.
    rig.server.remove_spawn(pc.id);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::CancelTrade) == 1; }));
    CHECK_FALSE(conn.trade().has_value());
    auto cancels = sent(rig, to_wire(Op::CancelTrade));
    REQUIRE(cancels[0]->data.size() == kCancelTradeSize);
    CHECK(u32_at(*cancels[0], 0) == pc.id);
    CHECK(u32_at(*cancels[0], 4) == w.player_id);
}

TEST_CASE("mock server: the partner's coin in a trade is held at the maximum rather than wrapping") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& w = conn.world_for_test();
    const game::Spawn* me = w.player();
    REQUIRE(me);
    game::Spawn pc;
    pc.id = 4100;
    pc.name = "Partner";
    pc.position = me->position + glm::vec3(3.0f, 0.0f, 0.0f);
    pc.heights.avatar_height = 6.0f;
    w.spawns[pc.id] = pc;
    conn.destroy_cursor();
    conn.give_to(pc.id);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::TradeRequest) == 1; }));
    rig.server.zone.send_reliable(encode_trade_request_ack({w.player_id, pc.id}));
    REQUIRE(rig.until([&] { return conn.trade() && conn.trade()->open; }));
    TradeCoins coins;
    coins.trader = w.player_id;
    coins.type = 3;
    coins.amount = 0xfffffff0u;
    conn.receive_zone_packet_for_test(encode(coins));
    coins.amount = 0x20;
    conn.receive_zone_packet_for_test(encode(coins));
    REQUIRE(conn.trade().has_value());
    CHECK(conn.trade()->their_coins[3] == 0xffffffffu);
}

TEST_CASE("mock server: the duel partner leaving mid-duel sends the original's 8-byte OP_DuelDecline") {
    // Not EQEmu's 12-byte form: its Handle_OP_DuelDecline dereferences entity_list.GetID(target_id) without a null
    // check, and the partner has left. EQEmu clears both duels itself when a client goes (Client::~Client).
    Rig rig;
    REQUIRE(rig.to_zone());
    REQUIRE(add_rival(rig));
    rig.conn.set_target(kRival);
    rig.conn.duel();
    rig.server.zone.send_reliable(encode(DuelRequest{kRival, kRival}));
    REQUIRE(rig.until([&] { return rig.conn.duel_state().active; }));
    rig.server.remove_spawn(kRival);
    const std::uint16_t decline = to_wire(Op::DuelDecline);
    REQUIRE(rig.until([&] { return !sent(rig, decline).empty(); }));
    auto declines = sent(rig, decline);
    REQUIRE(declines.size() == 1);
    CHECK(declines[0]->data.size() == kDuelRequestSize);
    CHECK(u32_at(*declines[0], 0) == kRival);
    CHECK_FALSE(rig.conn.duel_state().active);
}
