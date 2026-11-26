#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include "app/lua_ui.h"
#include "game/item_links.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/packet_io.h"
#include "net/trade_packets.h"

using namespace hailstone;

namespace {
constexpr std::uint32_t kPartner = 4100;

void add_partner(net::GameConnection& c) {
    game::Spawn pc;
    pc.id = kPartner;
    pc.name = "Partner";
    pc.position = c.world().player()->position + glm::vec3{3, 0, 0};
    pc.heights.avatar_height = 6;
    c.world_for_test().spawns[kPartner] = pc;
}

void open_trade(net::GameConnection& c, bool incoming = false) {
    add_partner(c);
    if (incoming) {
        c.set_auto_attack(false);
        c.receive_zone_packet_for_test(net::encode_trade_request({c.world().player_id, kPartner}));
    } else {
        c.give_to(kPartner);
        c.receive_zone_packet_for_test(net::encode_trade_request_ack({c.world().player_id, kPartner}));
    }
    REQUIRE(c.trade());
    REQUIRE(c.trade()->open);
    REQUIRE_FALSE(c.trade()->with_npc);
}

void check_now(net::GameConnection& c) {
    net::GameConnectionTestAccess::npc_checks(c).trade = 0;
    c.update();
}

void put_mock_cursor_coins_away(net::GameConnection& c) {
    for (std::size_t type = 0; type < c.cursor_coins().size(); ++type) c.drop_on_doll();
}
} // namespace

TEST_CASE("player trade distance: both activation paths use the partner's height and 3D distance") {
    for (bool incoming : {false, true}) {
        for (bool move_partner : {false, true}) {
            CAPTURE(incoming);
            CAPTURE(move_partner);
            mq_test::World world("player_trade_distance");
            auto& c = *world.conn;
            open_trade(c, incoming);
            c.world_for_test().spawns[c.world().player_id].heights.avatar_height = 100;
            // The partner's 6 + 20 reach includes all three axes, and exactly 26 stays open.
            auto place = [&](glm::vec3 offset) {
                if (move_partner) c.world_for_test().spawns[kPartner].position = offset;
                else {
                    c.world_for_test().spawns[kPartner].position = {};
                    c.set_player_motion(offset, 0, {}, true);
                }
                check_now(c);
            };
            place({0, 0, 25});
            REQUIRE(c.trade());
            place({10, 0, 24});
            REQUIRE(c.trade());
            place({10, 0, 24.1f});
            CHECK_FALSE(c.trade());
        }
    }
}

TEST_CASE("player trade distance: checks wait for their deadline and schedule another 500 ms") {
    mq_test::World world("player_trade_distance_timer");
    auto& c = *world.conn;
    open_trade(c);
    auto& next = net::GameConnectionTestAccess::npc_checks(c).trade;
    const auto before = net::GameConnection::now_ms();
    check_now(c);
    const auto after = net::GameConnection::now_ms();
    CHECK(next >= before + 500);
    CHECK(next <= after + 500);

    // GameConnection uses the steady clock directly; force its deadline instead of sleeping.
    next = net::GameConnection::now_ms() + 60000;
    c.world_for_test().spawns[kPartner].position = {100, 0, 0};
    c.update();
    REQUIRE(c.trade());
    check_now(c);
    CHECK_FALSE(c.trade());
}

TEST_CASE("player trade distance: a missing or dead partner closes an open trade") {
    for (bool missing : {false, true}) {
        CAPTURE(missing);
        mq_test::World world("player_trade_distance_partner");
        auto& c = *world.conn;
        open_trade(c);
        if (missing) c.world_for_test().spawns.erase(kPartner);
        else c.world_for_test().spawns[kPartner].is_corpse = true;
        check_now(c);
        CHECK_FALSE(c.trade());
    }
}

TEST_CASE("player trade distance: a request waiting for its answer is not checked") {
    mq_test::World world("player_trade_distance_request");
    auto& c = *world.conn;
    add_partner(c);
    c.give_to(kPartner);
    REQUIRE(c.trade());
    REQUIRE(c.trade()->requested);
    REQUIRE_FALSE(c.trade()->open);
    c.world_for_test().spawns[kPartner].position = {100, 0, 0};
    check_now(c);
    REQUIRE(c.trade());
    CHECK(c.trade()->requested);
    CHECK_FALSE(c.trade()->open);
}

TEST_CASE("player trade: incoming offer chat links to the offered item") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    open_trade(c, true);
    rig.chat();

    net::Item offered;
    offered.slot = {net::kInvTrade, 0};
    offered.id = 13073;
    offered.name = "Trade test blade";
    offered.ornament_icon = 533;
    net::Item augment;
    augment.id = 40042;
    augment.name = "Trade test augment";
    offered.contents.push_back({2, augment});
    rig.server.item_packet(net::kItemPacketTradeView, offered);
    REQUIRE(rig.until([&] { return c.trade() && c.trade()->theirs.contains(offered.slot.slot); }));

    const auto lines = rig.chat();
    const auto announcement = std::find_if(lines.begin(), lines.end(), [&](const net::ChatEvent& event) {
        return game::strip_links(event.text) == "[9028] Partner " + offered.name;
    });
    REQUIRE(announcement != lines.end());
    CHECK(announcement->kind == net::ChatEvent::Kind::System);
    const auto parts = game::split_links(announcement->text);
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[1].link);
    CHECK(parts[1].text == offered.name);
    CHECK(parts[1].link->item_id == offered.id);
    CHECK(parts[1].link->augments[2] == augment.id);
    CHECK(parts[1].link->ornament_icon == offered.ornament_icon);

    c.request_item(*parts[1].link);
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::ItemLinkClick) == 1; }));
    const auto request = std::find_if(rig.server.zone_log.begin(), rig.server.zone_log.end(),
                                      [](const net::AppPacket& p) { return net::is_op(p, net::Op::ItemLinkClick); });
    REQUIRE(request != rig.server.zone_log.end());
    const auto click = net::decode_item_link_click(*request);
    REQUIRE(click);
    CHECK(click->item_id == offered.id);
    CHECK(click->augments[2] == augment.id);
    CHECK(click->ornament_icon == offered.ornament_icon);
}

TEST_CASE("player trade: updating an incoming stack does not repeat the offer chat") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    open_trade(c, true);

    net::Item offered;
    offered.slot = {net::kInvTrade, 0};
    offered.id = 13074;
    offered.name = "Trade test stack";
    offered.max_stack = 20;
    rig.server.item_packet(net::kItemPacketTradeView, offered);
    REQUIRE(rig.until([&] { return c.trade() && c.trade()->theirs.contains(offered.slot.slot); }));
    rig.chat();

    offered.stack_count = 2;
    rig.server.item_packet(net::kItemPacketTradeView, offered);
    REQUIRE(rig.until([&] {
        return c.trade() && c.trade()->theirs.contains(offered.slot.slot) &&
               c.trade()->theirs.at(offered.slot.slot).stack_count == offered.stack_count;
    }));
    CHECK(rig.chat().empty());
}

TEST_CASE("player trade: cursor coin opens a coin-only offer after acknowledgement") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    put_mock_cursor_coins_away(c);
    const auto baseline_moves = static_cast<int>(c.cursor_coins().size());
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::MoveCoin) == baseline_moves; }));
    const auto start = rig.server.zone_log.size();
    const net::Coins original = c.coins();
    const auto player = c.world().player_id;
    add_partner(c);

    REQUIRE(c.move_coin(net::kCoinInventory, net::kCoinCursor, net::kCoinGold, net::kCoinGold, 2));
    c.give_to(kPartner);
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::TradeRequest) == 1; }));
    REQUIRE(c.trade());
    CHECK(c.trade()->requested);
    CHECK_FALSE(c.trade()->open);
    CHECK(c.cursor_coins()[net::kCoinGold] == 2);
    CHECK(c.trade()->my_coins[net::kCoinGold] == 0);
    CHECK(test::MockEqServer::count(rig.server.zone_log, net::Op::MoveCoin) == baseline_moves + 1);

    rig.server.zone.send_reliable(net::encode_trade_request_ack({player, kPartner}));
    REQUIRE(rig.until([&] { return c.trade() && c.trade()->open; }));
    CHECK_FALSE(c.trade()->requested);
    CHECK(c.cursor_coins()[net::kCoinGold] == 0);
    CHECK(c.trade()->my_coins[net::kCoinGold] == 2);
    CHECK(c.coins().gold == original.gold - 2);

    rig.server.zone.send_reliable(net::encode(net::TradeCoins{player, net::kCoinSilver, 0, 0, 1}));
    rig.server.zone.send_reliable(net::encode(net::TradeAccept{player, 1}));
    REQUIRE(rig.until([&] { return c.trade() && c.trade()->their_ready; }));
    CHECK(c.trade()->their_coins[net::kCoinSilver] == 1);
    c.trade_accept();
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::TradeAcceptClick) == 1; }));

    std::vector<const net::AppPacket*> sent;
    for (auto i = start; i < rig.server.zone_log.size(); ++i) {
        const auto& p = rig.server.zone_log[i];
        if (net::is_op(p, net::Op::MoveCoin) || net::is_op(p, net::Op::TradeRequest) || net::is_op(p, net::Op::TradeAcceptClick))
            sent.push_back(&p);
    }
    const std::array expected{
        net::encode(net::MoveCoin{net::kCoinInventory, net::kCoinCursor, net::kCoinGold, net::kCoinGold, 2}),
        net::encode_trade_request({kPartner, player}),
        net::encode(net::MoveCoin{net::kCoinCursor, net::kCoinTrade, net::kCoinGold, net::kCoinGold, 2}),
        net::encode(net::TradeAccept{kPartner, 1}),
    };
    REQUIRE(sent.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(sent[i]->opcode == expected[i].opcode);
        CHECK(sent[i]->data == expected[i].data);
    }
    CHECK(test::MockEqServer::count(rig.server.zone_log, net::Op::MoveItem) == 0);

    CHECK(c.coins().silver == original.silver);
    net::Coins settled = original;
    settled.gold -= 2;
    settled.silver += 1;
    rig.server.zone.send_reliable(net::encode_money_update(settled));
    rig.server.zone.send_reliable(net::AppPacket{net::to_wire(net::Op::FinishTrade)});
    REQUIRE(rig.until([&] { return !c.trade(); }));
    CHECK(c.coins().gold == settled.gold);
    CHECK(c.coins().silver == settled.silver);
}

TEST_CASE("player trade: the trade money button offers cursor coin directly and clears acceptance") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    put_mock_cursor_coins_away(c);
    const auto baseline_moves = static_cast<int>(c.cursor_coins().size());
    open_trade(c);
    const net::Coins original = c.coins();
    REQUIRE(c.move_coin(net::kCoinInventory, net::kCoinCursor, net::kCoinGold, net::kCoinGold, 1));
    c.give_to(kPartner);
    REQUIRE(c.trade()->my_coins[net::kCoinGold] == 1);
    c.trade_accept();
    rig.server.zone.send_reliable(net::encode(net::TradeAccept{c.world().player_id, 1}));
    REQUIRE(rig.until([&] { return c.trade() && c.trade()->their_ready; }));
    REQUIRE(c.trade()->my_ready);

    REQUIRE(c.move_coin(net::kCoinInventory, net::kCoinCursor, net::kCoinGold, net::kCoinGold, 2));
    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &c;
    app::UiApi api(host);
    REQUIRE(api.click_child("TradeWnd", "TRDW_MyMoney1"));
    REQUIRE(c.trade());
    CHECK_FALSE(c.trade()->my_ready);
    CHECK_FALSE(c.trade()->their_ready);
    CHECK(c.cursor_coins()[net::kCoinGold] == 0);
    CHECK(c.trade()->my_coins[net::kCoinGold] == 3);
    CHECK(c.coins().gold == original.gold - 3);

    // A later reliable packet bounds the coin moves produced by the click.
    c.trade_cancel();
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::CancelTrade) == 1; }));
    CHECK(test::MockEqServer::count(rig.server.zone_log, net::Op::MoveCoin) == baseline_moves + 4);
    CHECK_FALSE(c.trade());
    CHECK(c.coins().gold == original.gold - 3);
    rig.server.zone.send_reliable(net::encode_money_update(original));
    REQUIRE(rig.until([&] { return c.coins().gold == original.gold; }));
    CHECK(c.cursor_coins()[net::kCoinGold] == 0);
}

TEST_CASE("player trade: a large coin offer transfers the exact amount") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    add_partner(c);
    put_mock_cursor_coins_away(c);
    const auto baseline_moves = static_cast<int>(c.cursor_coins().size());
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::MoveCoin) == baseline_moves; }));
    const auto start = rig.server.zone_log.size();

    constexpr std::uint32_t amount = 16'777'217;
    net::Coins original = c.coins();
    original.gold = amount + 5;
    rig.server.zone.send_reliable(net::encode_money_update(original));
    REQUIRE(rig.until([&] { return c.coins().gold == original.gold; }));

    REQUIRE(c.move_coin(net::kCoinInventory, net::kCoinCursor, net::kCoinGold, net::kCoinGold, amount));
    c.give_to(kPartner);
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::TradeRequest) == 1; }));
    CHECK(c.cursor_coins()[net::kCoinGold] == amount);
    CHECK(test::MockEqServer::count(rig.server.zone_log, net::Op::MoveCoin) == baseline_moves + 2);
    rig.server.zone.send_reliable(net::encode_trade_request_ack({c.world().player_id, kPartner}));
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::MoveCoin) == baseline_moves + 4; }));
    REQUIRE(c.trade());
    CHECK(c.trade()->my_coins[net::kCoinGold] == amount);
    CHECK(c.cursor_coins()[net::kCoinGold] == 0);
    CHECK(c.coins().gold == 5);

    std::uint32_t pickup = 0, offered = 0;
    for (auto i = start; i < rig.server.zone_log.size(); ++i) {
        const auto& packet = rig.server.zone_log[i];
        if (!net::is_op(packet, net::Op::MoveCoin)) continue;
        auto move = net::decode_move_coin(packet);
        REQUIRE(move);
        CHECK(static_cast<std::int32_t>(static_cast<float>(move->amount)) == move->amount);
        CHECK(move->type_from == net::kCoinGold);
        CHECK(move->type_to == net::kCoinGold);
        if (move->from == net::kCoinInventory && move->to == net::kCoinCursor)
            pickup += static_cast<std::uint32_t>(move->amount);
        if (move->from == net::kCoinCursor && move->to == net::kCoinTrade)
            offered += static_cast<std::uint32_t>(move->amount);
    }
    CHECK(pickup == amount);
    CHECK(offered == amount);
}

TEST_CASE("player trade: a refused request leaves the coin on the cursor") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    add_partner(c);
    put_mock_cursor_coins_away(c);
    REQUIRE(c.move_coin(net::kCoinInventory, net::kCoinCursor, net::kCoinGold, net::kCoinGold, 2));

    const auto me = c.world().player_id;
    c.give_to(kPartner);
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::TradeRequest) == 1; }));
    rig.server.zone.send_reliable(net::encode(net::TradeBusy{me, kPartner, 'b'}));
    REQUIRE(rig.until([&] { return !c.trade(); }));
    CHECK(c.cursor_coins()[net::kCoinGold] == 2);
    for (const auto& packet : rig.server.zone_log) {
        if (net::is_op(packet, net::Op::MoveCoin)) {
            auto move = net::decode_move_coin(packet);
            REQUIRE(move);
            CHECK(move->to != net::kCoinTrade);
        }
    }
}

TEST_CASE("player trade: an answer from out of reach offers the coin, then the reach check cancels") {
    // Handle_OP_TradeRequestAck @0x4c2070 checks only the spawn and IsOkToTransact; CTradeWnd::Activate drops the
    // cursor money, and CTradeWnd::OnProcessFrame's 500 ms reach check cancels so the server returns it.
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    add_partner(c);
    put_mock_cursor_coins_away(c);
    const auto baseline_moves = static_cast<int>(c.cursor_coins().size());
    REQUIRE(c.move_coin(net::kCoinInventory, net::kCoinCursor, net::kCoinGold, net::kCoinGold, 2));

    const auto me = c.world().player_id;
    c.give_to(kPartner);
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::TradeRequest) == 1; }));
    c.world_for_test().spawns[kPartner].position = c.world().player()->position + glm::vec3{0, 0, 27};
    rig.server.zone.send_reliable(net::encode_trade_request_ack({me, kPartner}));
    // The reach check may run in the same update that opens the trade, so look at what was sent.
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::CancelTrade) == 1; }));
    CHECK_FALSE(c.trade());
    CHECK(c.cursor_coins()[net::kCoinGold] == 0);
    CHECK(test::MockEqServer::count(rig.server.zone_log, net::Op::MoveCoin) == baseline_moves + 2);
    bool offered = false;
    for (const auto& packet : rig.server.zone_log) {
        if (net::is_op(packet, net::Op::CancelTrade)) {
            CHECK(offered);
            break;
        }
        if (!net::is_op(packet, net::Op::MoveCoin)) continue;
        auto move = net::decode_move_coin(packet);
        REQUIRE(move);
        if (move->from == net::kCoinCursor && move->to == net::kCoinTrade && move->amount == 2) offered = true;
    }
    CHECK(offered);
}

TEST_CASE("player trade distance: separation cancels once and items and coin return through the server") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    put_mock_cursor_coins_away(c);
    const auto baseline_moves = static_cast<int>(c.cursor_coins().size());
    open_trade(c);
    const net::Coins original = c.coins();
    REQUIRE(original.gold >= 2);

    net::Item offered;
    offered.id = 13073;
    offered.name = "Trade test item";
    offered.no_drop = 255;
    offered.stack_count = 7;
    offered.max_stack = 20;
    net::GameConnectionTestAccess::inventory(c).put(net::ItemSlot::possessions(net::kSlotCursor), offered);
    c.trade_drop_cursor();
    const net::ItemSlot trade_slot{net::kInvTrade, 0, -1, -1, 0, 0};
    REQUIRE(c.inventory().at(trade_slot));
    REQUIRE_FALSE(c.inventory().cursor());
    c.trade_coin(net::kCoinGold, 2);
    REQUIRE(c.trade()->my_coins[net::kCoinGold] == 2);
    REQUIRE(c.coins().gold == original.gold - 2);
    c.trade_accept();
    REQUIRE(c.trade()->my_ready);
    REQUIRE(rig.until([&] {
        return test::MockEqServer::count(rig.server.zone_log, net::Op::MoveItem) >= 1 &&
               test::MockEqServer::count(rig.server.zone_log, net::Op::MoveCoin) == baseline_moves + 2 &&
               test::MockEqServer::count(rig.server.zone_log, net::Op::TradeAcceptClick) == 1;
    }));

    c.world_for_test().spawns[kPartner].position = c.world().player()->position + glm::vec3{0, 27, 0};
    check_now(c);
    REQUIRE_FALSE(c.trade());
    CHECK_FALSE(c.inventory().at(trade_slot));
    CHECK_FALSE(c.inventory().cursor()); // cancellation itself must not invent the returned item or money
    CHECK(c.coins().gold == original.gold - 2);
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::CancelTrade) == 1; }));
    for (const auto& p : rig.server.zone_log) {
        if (!net::is_op(p, net::Op::CancelTrade)) continue;
        auto cancel = net::decode_cancel_trade(p);
        REQUIRE(cancel);
        CHECK(cancel->from_id == kPartner);
        CHECK(cancel->action == c.world().player_id);
    }

    // Script the server's normal cancellation echo and refund packets after it receives CancelTrade.
    rig.server.zone.send_reliable(net::encode(net::CancelTrade{c.world().player_id, 0}));
    offered.slot = net::ItemSlot::possessions(net::kSlotCursor);
    rig.server.item_packet(net::kItemPacketLimbo, offered);
    rig.server.zone.send_reliable(net::encode_money_update(original));
    REQUIRE(rig.until([&] { return c.inventory().cursor() && c.coins().gold == original.gold; }));
    CHECK(c.inventory().cursor()->id == offered.id);
    CHECK(c.inventory().cursor()->stack_count == offered.stack_count);
    CHECK(c.coins().copper == original.copper);
    CHECK(c.coins().silver == original.silver);
    CHECK(c.coins().platinum == original.platinum);
    // A later reliable packet proves all the client's cancellation work reached the mock in order.
    for (int i = 0; i < 4; ++i) check_now(c);
    c.send_command("/say trade distance check complete");
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::ChannelMessage) >= 1; }));
    CHECK(test::MockEqServer::count(rig.server.zone_log, net::Op::CancelTrade) == 1);
    CHECK_FALSE(c.trade());
}

TEST_CASE("trade windows: hiding the window not showing the trade leaves it open") {
    // CTradeWnd::AboutToHide / the Give window's hiding cancel the trade they show; a window that is not shown has
    // nothing to hide. Hiding the player trade window (a layout, a script's hs.ShowWindow) while giving to an NPC
    // used to cancel the hand-in all the same.
    mq_test::World world("trade_window_hide");
    auto& c = *world.conn;
    game::Spawn npc;
    npc.id = kPartner;
    npc.name = "Guard_Hanlon000";
    npc.is_npc = true;
    npc.position = c.world().player()->position + glm::vec3{3, 0, 0};
    npc.heights.avatar_height = 6;
    c.world_for_test().spawns[kPartner] = npc;
    c.give_to(kPartner);
    c.receive_zone_packet_for_test(net::encode_trade_request_ack({c.world().player_id, kPartner}));
    REQUIRE(c.trade());
    REQUIRE(c.trade()->open);
    REQUIRE(c.trade()->with_npc);

    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &c;
    app::UiApi api(host);
    CHECK(api.shown("GiveWnd"));
    CHECK_FALSE(api.shown("TradeWnd"));
    api.show("TradeWnd", false);
    CHECK(c.trade()); // the hand-in stays
    api.show("GiveWnd", false);
    CHECK_FALSE(c.trade()); // its own window hidden: cancelled (OP_CancelTrade, the server returns the items)
}

TEST_CASE("player trade: another player's request is refused as busy while our own waits for its answer") {
    // A deliberate difference from IsOkToTransact (see Op::TradeRequest in game_connection_commerce.cpp).
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    add_partner(c);
    constexpr std::uint32_t kOther = 4101;
    game::Spawn other = c.world().spawns.at(kPartner);
    other.id = kOther;
    other.name = "Other";
    c.world_for_test().spawns[kOther] = other;
    c.set_auto_attack(false);
    c.give_to(kPartner);
    REQUIRE(c.trade());
    REQUIRE(c.trade()->requested);

    c.receive_zone_packet_for_test(net::encode_trade_request({c.world().player_id, kOther}));
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::TradeBusy) == 1; }));
    REQUIRE(c.trade());
    CHECK(c.trade()->requested);
    CHECK(c.trade()->partner_id == kPartner);
    CHECK(test::MockEqServer::count(rig.server.zone_log, net::Op::TradeRequestAck) == 0);

    c.receive_zone_packet_for_test(net::encode_trade_request_ack({c.world().player_id, kPartner}));
    REQUIRE(c.trade());
    CHECK(c.trade()->open);
    CHECK(c.trade()->partner_id == kPartner);
}
