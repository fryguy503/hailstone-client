#include <doctest/doctest.h>

#include "mock_server/harness.h"
#include "net/tribute_packets.h"
#include "scratch_dir.h"
#include "mq_test_world.h"

using namespace hailstone;
using namespace hailstone::test;

namespace {
net::ConnectionOptions commerce_options(const ScratchDir& files) {
    auto options = quick_options();
    options.servers_dir = files.path() / "servers";
    return options;
}
void empty_coin_cursor(net::GameConnection& c) {
    for (int i = 0; i < 4; ++i) c.drop_on_doll();
    for (auto n : c.cursor_coins()) REQUIRE(n == 0);
}
net::ItemSlot bank_at(std::int16_t slot, std::int16_t sub = -1, bool shared = false) {
    return {shared ? net::kInvSharedBank : net::kInvBank, slot, sub, -1};
}
void show_bank(Rig& rig) {
    const auto position = rig.conn.world().player()->position;
    rig.server.add_spawn(make_spawn("Banker", 60, 1, 1, position.y, position.x, position.z));
    REQUIRE(rig.until([&] { return rig.conn.world().spawns.contains(60); }));
    REQUIRE(rig.conn.activate_bank(60));
}
}

TEST_CASE("commerce: Keep blocks both barter sale paths, including a changed quantity dialog") {
    ScratchDir files("commerce_edges");
    Rig rig(MockConfig::standard(), commerce_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    auto& merchant = const_cast<game::BarterMerchantState&>(c.barter_merchant());
    auto buyer = *c.world().player();
    buyer.id = 77;
    buyer.buyer = true;
    c.world_for_test().spawns[77] = buyer;
    merchant.buyer_id = 77;
    net::BuyLine line;
    line.item_id = 1002;
    line.quantity = 5;
    line.slot = 0;
    merchant.lines.push_back(line);
    merchant.selected_slot = 0;
    REQUIRE(c.barter_merchant_sell_start());
    auto& search = const_cast<game::BarterSearchState&>(c.barter_search_state());
    line.buyer_id = 999;
    line.zone = c.zone_id();
    search.lines.push_back(line);
    REQUIRE(c.barter_search_sell_start(0));
    const int before = MockEqServer::count(rig.server.zone_log, net::Op::BuyerItems);
    c.set_item_kept(1002, "Rations", true);
    CHECK_FALSE(c.barter_merchant_sell_start());
    CHECK_FALSE(c.barter_search_sell_start(0));
    c.barter_merchant_sell(1);
    c.barter_search_sell(1);
    rig.run_for(std::chrono::milliseconds(70));
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::BuyerItems) == before);
    CHECK(merchant.pending == 0);
    CHECK(search.pending == 0);
    REQUIRE(c.inventory().at(net::ItemSlot::possessions(23)));
    CHECK(c.inventory().at(net::ItemSlot::possessions(23))->stack_count == 5);
}

TEST_CASE("commerce: marking an offered item Keep withdraws the trader offer and price changes cannot reoffer it") {
    ScratchDir files("commerce_edges");
    Rig rig(MockConfig::standard(), commerce_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    auto& trader = const_cast<game::TraderState&>(c.trader_state());
    game::TraderSlot slot;
    slot.where = net::ItemSlot::possessions(23);
    slot.serial = c.inventory().at(slot.where)->serial;
    slot.item_id = 1002;
    slot.price = 100;
    slot.active = true;
    trader.slots.push_back(slot);
    trader.selected = 0;
    trader.trader = trader.start_pending = true;
    c.set_item_kept(1002, "Rations", true);
    CHECK_FALSE(trader.start_pending);
    CHECK_FALSE(trader.slots.front().active);
    const auto price = trader.slots.front().price;
    c.trader_enter_price(200);
    c.trader_set_price();
    CHECK(trader.slots.front().price == price);
    c.trader_enter_price(200);
    c.trader_add();
    rig.run_for(std::chrono::milliseconds(70));
    // The only new packet can be the already-queued End; Add cannot publish a price for the kept item.
    for (const auto& p : rig.server.zone_log)
        if (p.opcode == net::to_wire(net::Op::Trader)) {
            if (auto update = net::decode_trader_price(p)) CHECK(update->price == 0);
        }
}

TEST_CASE("commerce: a late buyer-start answer cannot publish newly kept compensation") {
    ScratchDir files("commerce_edges");
    Rig rig(MockConfig::standard(), commerce_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    auto& buyer = const_cast<game::BarterWindowState&>(c.barter_window());
    auto& line = buyer.lines[0];
    line.enabled = line.toggle = true;
    line.trade_items.push_back({1002, 1, 0, "Rations"});
    c.barter_start_stop();
    REQUIRE_FALSE(buyer.buyer_mode);
    c.set_item_kept(1002, "Rations", true);
    c.receive_zone_packet_for_test(net::encode_buyer_mode(true));
    CHECK_FALSE(buyer.buyer_mode);
    rig.run_for(std::chrono::milliseconds(70));
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::BuyerItems) == 0);
}

TEST_CASE("commerce: bulk Keep changes one revision and donation guards respect it") {
    ScratchDir files("commerce_edges");
    Rig rig(MockConfig::standard(), commerce_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    empty_coin_cursor(c);
    const auto revision = c.kept_items_revision();
    c.set_items_kept({{1001, "Sword"}, {1002, "Rations"}, {1002, "Rations"}}, true);
    CHECK(c.kept_items_revision() == revision + 1);
    CHECK(c.kept_items().kept(1001));
    CHECK(c.kept_items().kept(1002));
    c.set_items_kept({{1002, "Rations"}}, true);
    CHECK(c.kept_items_revision() == revision + 1);
    auto master = *c.world().player();
    master.id = 77;
    master.is_npc = true;
    c.world_for_test().spawns[77] = master;
    auto personal = net::encode(net::StartTribute{c.world().player_id, 77, 1});
    auto guild = personal;
    guild.opcode = net::to_wire(net::Op::OpenGuildTributeMaster);
    rig.server.zone.send_reliable(personal);
    rig.server.zone.send_reliable(guild);
    REQUIRE(rig.until([&] { return c.tribute().master_id == 77 && c.guild_tribute().master_id == 77; }));
    c.click_slot(net::ItemSlot::possessions(23), true, false);
    REQUIRE(c.inventory().cursor());
    CHECK_FALSE(c.tribute_donate_cursor());
    CHECK_FALSE(c.guild_tribute_donate_cursor());
    CHECK(c.inventory().cursor()->id == 1002);
}

TEST_CASE("commerce: kept compensation cannot start a buyer or enable an active buy line") {
    ScratchDir files("commerce_edges");
    Rig rig(MockConfig::standard(), commerce_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    auto& buyer = const_cast<game::BarterWindowState&>(c.barter_window());
    auto& line = buyer.lines[0];
    line.enabled = line.toggle = true;
    line.trade_items.push_back({1002, 1, 0, "Rations"});
    c.set_item_kept(1002, "Rations", true);
    rig.run_for(std::chrono::milliseconds(70));
    const int modes = MockEqServer::count(rig.server.zone_log, net::Op::Barter);
    const int lines = MockEqServer::count(rig.server.zone_log, net::Op::BuyerItems);
    c.barter_start_stop();
    buyer.buyer_mode = true;
    buyer.line_sel = 0;
    line.toggle = false;
    c.barter_toggle_line();
    rig.run_for(std::chrono::milliseconds(70));
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::Barter) == modes);
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::BuyerItems) == lines);
    CHECK_FALSE(line.toggle);
}

TEST_CASE("commerce: bank selection withdraws kept items without destroying them and stops when the bank closes") {
    auto config = MockConfig::standard();
    auto sword = make_item(3001, "Stored sword", 0);
    sword.slot = bank_at(0);
    auto shield = make_item(3002, "Stored shield", 1);
    shield.slot = bank_at(1);
    config.characters.front().inventory.push_back(sword);
    config.characters.front().inventory.push_back(shield);
    ScratchDir files("commerce_edges");
    Rig rig(std::move(config), commerce_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    empty_coin_cursor(c);
    using M = net::GameConnection::ItemJobMode;
    CHECK(c.bulk_items_plan(M::Withdraw, {bank_at(0)}).empty());
    CHECK(c.start_bulk_job(M::Withdraw, {bank_at(0)}) == "The bank is not open.");
    show_bank(rig);
    c.set_item_kept(3001, "Stored sword", true);
    CHECK(c.bulk_items_plan(M::Destroy, {bank_at(0)}).empty());
    REQUIRE(c.start_bulk_job(M::Withdraw, {bank_at(0)}).empty());
    REQUIRE(rig.until([&] { return !c.item_job(); }));
    CHECK_FALSE(c.inventory().at(bank_at(0)));
    CHECK(c.kept_items().kept(3001));
    bool carried = false;
    for (int s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s)
        if (const auto* item = c.inventory().at(net::ItemSlot::possessions(static_cast<std::int16_t>(s))))
            carried = carried || item->id == 3001;
    CHECK(carried);
    REQUIRE(c.start_bulk_job(M::Destroy, {bank_at(1)}).empty());
    c.close_bank();
    REQUIRE(rig.until([&] { return !c.item_job(); }));
    REQUIRE(c.inventory().at(bank_at(1)));
    CHECK(c.inventory().at(bank_at(1))->id == 3002);
}

TEST_CASE("commerce: a replacement stack in a selected slot never inherits bulk destroy") {
    ScratchDir files("commerce_edges");
    Rig rig(MockConfig::standard(), commerce_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    empty_coin_cursor(c);
    using M = net::GameConnection::ItemJobMode;
    REQUIRE(c.start_bulk_job(M::Destroy, {net::ItemSlot::possessions(23)}).empty());
    auto& inventory = const_cast<game::Inventory&>(c.inventory());
    auto replacement = *inventory.at(net::ItemSlot::possessions(23));
    replacement.serial = "0000000000000042";
    inventory.put(net::ItemSlot::possessions(23), replacement);
    REQUIRE(rig.until([&] { return !c.item_job(); }));
    REQUIRE(c.inventory().at(net::ItemSlot::possessions(23)));
    CHECK(c.inventory().at(net::ItemSlot::possessions(23))->serial == replacement.serial);
    CHECK_FALSE(c.inventory().cursor());
}

TEST_CASE("commerce: bulk destroy rechecks an empty bag after it acquires contents") {
    auto config = MockConfig::standard();
    auto bag = make_item(5001, "Selected empty bag", 24);
    bag.item_class = 1;
    bag.bag_slots = 4;
    config.characters.front().inventory.push_back(bag);
    ScratchDir files("commerce_edges");
    Rig rig(std::move(config), commerce_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    empty_coin_cursor(c);
    const auto original = net::ItemSlot::possessions(24);
    auto location = original;
    REQUIRE(c.start_bulk_job(net::GameConnection::ItemJobMode::Destroy, {original}).empty());
    SUBCASE("the bag fills while queued") {}
    SUBCASE("the bag fills after pickup") {
        net::GameConnectionTestAccess::update_bulk_job(c, net::GameConnection::now_ms());
        REQUIRE(c.inventory().cursor());
        location = net::ItemSlot::possessions(net::kSlotCursor);
    }
    auto& inventory = const_cast<game::Inventory&>(c.inventory());
    auto changed = *inventory.at(location);
    changed.contents.push_back({0, make_item(5002, "Newly added item", 0)});
    inventory.put(location, changed);
    net::GameConnectionTestAccess::update_bulk_job(c, net::GameConnection::now_ms());
    REQUIRE(inventory.at(original));
    CHECK(inventory.at(original)->contents.size() == 1);
    CHECK_FALSE(inventory.cursor());
    rig.run_for(std::chrono::milliseconds(70));
    for (const auto& packet : rig.server.zone_log)
        if (packet.opcode == net::to_wire(net::Op::MoveItem))
            if (auto move = net::decode_move_item(packet)) CHECK(move->to.type >= 0);
}

TEST_CASE("commerce: bulk guild tribute waits for the guild answer and uses guild favor") {
    MockConfig config = MockConfig::standard();
    auto item = make_item(4001, "Guild offering", 24);
    item.favor = 0;
    item.guild_favor = 10;
    config.characters.front().inventory.push_back(item);
    ScratchDir files("commerce_edges");
    Rig rig(std::move(config), commerce_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    empty_coin_cursor(c);
    auto master = *c.world().player();
    master.id = 77;
    master.is_npc = true;
    c.world_for_test().spawns[77] = master;
    auto open = net::encode(net::StartTribute{c.world().player_id, 77, 1});
    open.opcode = net::to_wire(net::Op::OpenGuildTributeMaster);
    rig.server.zone.send_reliable(open);
    REQUIRE(rig.until([&] { return c.guild_tribute().master_id == 77; }));
    using M = net::GameConnection::ItemJobMode;
    REQUIRE(c.start_bulk_job(M::Tribute, {net::ItemSlot::possessions(24)}).empty());
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::GuildTributeDonateItem) == 1; }));
    REQUIRE(c.inventory().cursor());
    rig.run_for(std::chrono::milliseconds(100));
    REQUIRE(c.item_job());
    rig.server.zone.send_reliable(net::encode(net::GuildTributeDonation{net::ItemSlot::possessions(net::kSlotCursor), 1, 10}));
    REQUIRE(rig.until([&] { return !c.item_job(); }));
    CHECK_FALSE(c.inventory().cursor());
    CHECK_FALSE(c.inventory().at(net::ItemSlot::possessions(24)));
    CHECK(c.guild_tribute().last_donation == 10);
}

TEST_CASE("commerce: a bank withdrawal goes into the packs and never equips") {
    auto config = MockConfig::standard();
    auto arrows = make_item(3003, "Stored arrows", 0);
    arrows.slot = bank_at(0);
    arrows.equip_slots = 1u << net::kSlotAmmo; // the ammo slot is empty: auto-inventory would equip them there
    config.characters.front().inventory.push_back(arrows);
    ScratchDir files("commerce_edges");
    Rig rig(std::move(config), commerce_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    empty_coin_cursor(c);
    show_bank(rig);
    REQUIRE_FALSE(c.inventory().at(net::ItemSlot::possessions(net::kSlotAmmo)));
    using M = net::GameConnection::ItemJobMode;
    REQUIRE(c.start_bulk_job(M::Withdraw, {bank_at(0)}).empty());
    REQUIRE(rig.until([&] { return !c.item_job(); }));
    CHECK_FALSE(c.inventory().at(bank_at(0)));
    CHECK_FALSE(c.inventory().at(net::ItemSlot::possessions(net::kSlotAmmo)));
    REQUIRE(c.inventory().at(net::ItemSlot::possessions(24)));
    CHECK(c.inventory().at(net::ItemSlot::possessions(24))->id == 3003);
}

TEST_CASE("commerce: marking an unoffered satchel item Keep leaves the trader session running") {
    ScratchDir files("commerce_edges");
    Rig rig(MockConfig::standard(), commerce_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    auto& trader = const_cast<game::TraderState&>(c.trader_state());
    game::TraderSlot offered;
    offered.where = net::ItemSlot::possessions(23);
    offered.serial = c.inventory().at(offered.where)->serial;
    offered.item_id = 1002;
    offered.price = 100;
    offered.active = true;
    game::TraderSlot unpriced;
    unpriced.where = net::ItemSlot::possessions(13);
    unpriced.serial = c.inventory().at(unpriced.where)->serial;
    unpriced.item_id = 1001;
    trader.slots = {offered, unpriced};
    trader.trader = trader.start_pending = true;
    c.set_item_kept(1001, "Short Sword", true);
    CHECK(trader.start_pending);
    CHECK(trader.slots[0].active);
    c.set_item_kept(1002, "Rations", true);
    CHECK_FALSE(trader.start_pending);
    CHECK_FALSE(trader.slots[0].active);
}
