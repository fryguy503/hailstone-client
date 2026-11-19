#include <doctest/doctest.h>

#include "app/lua_ui.h"
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

TEST_CASE("player trade distance: separation cancels once and items and coin return through the server") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    c.destroy_cursor(); // the mock character starts with cursor coin
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
               test::MockEqServer::count(rig.server.zone_log, net::Op::MoveCoin) >= 2 &&
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
