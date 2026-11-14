#include <doctest/doctest.h>

#include "manual_clock.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/tribute_packets.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::test;

namespace {
net::ConnectionOptions tribute_options(const ScratchDir& files) {
    auto options = quick_options();
    options.servers_dir = files.path() / "servers";
    return options;
}

MockConfig tribute_config() {
    auto config = MockConfig::standard();
    auto& items = config.characters.front().inventory;
    for (auto& item : items) {
        item.favor = 10;
        item.guild_favor = 10;
    }
    auto replacement = make_item(1002, "Replacement rations", 24, 5);
    replacement.serial = "0000000000042002";
    replacement.instance_id = 42002;
    items.push_back(replacement);
    return config;
}

void put_coin_away(net::GameConnection& c) {
    for (int i = 0; i < 4; ++i)
        c.drop_on_doll();
    for (auto n : c.cursor_coins())
        REQUIRE(n == 0);
}

void open_master(Rig& rig, bool guild) {
    auto master = *rig.conn.world().player();
    master.id = 77;
    master.is_npc = true;
    master.class_id = guild ? 64 : 63;
    rig.conn.world_for_test().spawns[77] = master;
    auto packet = net::encode(net::StartTribute{rig.conn.world().player_id, 77, 1});
    if (guild)
        packet.opcode = net::to_wire(net::Op::OpenGuildTributeMaster);
    rig.server.zone.send_reliable(packet);
    REQUIRE(
        rig.until([&] { return (guild ? rig.conn.guild_tribute().master_id : rig.conn.tribute().master_id) == 77; }));
}

bool donate(net::GameConnection& c, bool guild) {
    return guild ? c.guild_tribute_donate_cursor() : c.tribute_donate_cursor();
}

std::uint32_t answers(const net::GameConnection& c, bool guild) {
    return guild ? c.guild_tribute().donation_answers : c.tribute().donation_answers;
}

net::Op item_opcode(bool guild) { return guild ? net::Op::GuildTributeDonateItem : net::Op::TributeItem; }

net::AppPacket reply(bool guild, std::uint32_t quantity = 5, std::uint32_t favor = 10) {
    const auto slot = net::ItemSlot::possessions(net::kSlotCursor);
    return guild ? net::encode(net::GuildTributeDonation{slot, quantity, favor})
                 : net::encode(net::TributeItem{slot, quantity, 77, static_cast<std::int32_t>(favor)});
}

void take_rations(net::GameConnection& c) {
    c.click_slot(net::ItemSlot::possessions(23), true, false);
    REQUIRE(c.inventory().cursor());
    REQUIRE(c.inventory().cursor()->stack_count == 5);
}

void deliver_item(net::GameConnection& c, const net::Item& item) {
    net::ItemPacket packet;
    packet.type = net::kItemPacketCharInventory;
    packet.item = item;
    c.receive_zone_packet_for_test(net::encode_item_packet(false, packet));
}
} // namespace

TEST_CASE("tribute replies follow the donated instance and preserve a "
          "replacement cursor") {
    for (bool guild : {false, true}) {
        CAPTURE(guild);
        ScratchDir files("tribute_reply_identity");
        Rig rig(tribute_config(), tribute_options(files));
        REQUIRE(rig.to_zone());
        auto& c = rig.conn;
        put_coin_away(c);
        open_master(rig, guild);
        open_master(rig, !guild);
        take_rations(c);
        const std::string original = c.inventory().cursor()->serial;
        REQUIRE(donate(c, guild));
        REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, item_opcode(guild)) == 1; }));

        // Withhold the reply while ordinary clicks put the donated stack away and pick up another instance.
        // Clicking directly onto the replacement stack would merge it instead of replacing the cursor.
        c.click_slot(net::ItemSlot::possessions(25), true, false);
        REQUIRE_FALSE(c.inventory().cursor());
        c.click_slot(net::ItemSlot::possessions(24), true, false);
        REQUIRE(c.inventory().cursor());
        const std::string replacement = c.inventory().cursor()->serial;
        REQUIRE(replacement != original);
        REQUIRE(c.inventory().at(net::ItemSlot::possessions(25)));
        CHECK(c.inventory().at(net::ItemSlot::possessions(25))->serial == original);
        CHECK_FALSE(donate(c, guild));
        CHECK_FALSE(donate(c, !guild)); // the other channel cannot reuse the
                                        // outstanding cursor request

        const auto before = answers(c, guild);
        c.receive_zone_packet_for_test(reply(guild, 4)); // wrong quantity is not this request's answer
        CHECK(answers(c, guild) == before);
        CHECK(c.inventory().cursor()->serial == replacement);
        REQUIRE(c.inventory().at(net::ItemSlot::possessions(25)));
        rig.server.zone.send_reliable(reply(guild, 5, guild ? 0u : 10u));
        REQUIRE(rig.until([&] { return answers(c, guild) == before + 1; }));
        CHECK_FALSE(c.inventory().at(net::ItemSlot::possessions(25)));
        REQUIRE(c.inventory().cursor());
        CHECK(c.inventory().cursor()->serial == replacement);
        CHECK(c.inventory().cursor()->stack_count == 5);

        c.receive_zone_packet_for_test(reply(guild)); // an unsolicited repeat cannot delete the replacement
        CHECK(answers(c, guild) == before + 1);
        CHECK(c.inventory().cursor()->serial == replacement);
        CHECK(donate(c, !guild)); // the matching answer, including zero guild
                                  // favour, releases the fence
    }
}

TEST_CASE("tribute replies never remove a changed stack or an ambiguous serial") {
    for (bool guild : {false, true}) {
        for (bool duplicate : {false, true}) {
            CAPTURE(guild);
            CAPTURE(duplicate);
            ScratchDir files("tribute_reply_changed");
            Rig rig(tribute_config(), tribute_options(files));
            REQUIRE(rig.to_zone());
            auto& c = rig.conn;
            put_coin_away(c);
            open_master(rig, guild);
            take_rations(c);
            REQUIRE(donate(c, guild));
            REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, item_opcode(guild)) == 1; }));
            auto changed = *c.inventory().cursor();
            if (!duplicate) {
                changed.stack_count = 6;
                deliver_item(c, changed);
            } else {
                changed.slot = net::ItemSlot::possessions(25);
                deliver_item(c, changed);
            }
            const auto before = answers(c, guild);
            rig.server.zone.send_reliable(reply(guild));
            REQUIRE(rig.until([&] { return answers(c, guild) == before + 1; }));
            REQUIRE(c.inventory().cursor());
            CHECK(c.inventory().cursor()->serial == changed.serial);
            CHECK(c.inventory().cursor()->stack_count == (changed.slot.slot == 25 ? 5u : 6u));
            if (changed.slot.slot == 25) {
                REQUIRE(c.inventory().at(changed.slot));
                CHECK(c.inventory().at(changed.slot)->stack_count == 5);
            }
            bool warned = false;
            for (const auto& event : rig.chat())
                warned = warned || event.text.find("Other items were left alone") != std::string::npos;
            CHECK(warned);
        }
    }
}

TEST_CASE("a personal tribute refusal retains its item and ends the "
          "outstanding request") {
    ScratchDir files("tribute_reply_refusal");
    Rig rig(tribute_config(), tribute_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    put_coin_away(c);
    open_master(rig, false);
    take_rations(c);
    const std::string serial = c.inventory().cursor()->serial;
    REQUIRE(donate(c, false));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::TributeItem) == 1; }));
    const auto before = answers(c, false);
    rig.server.zone.send_reliable(reply(false, 5, 0));
    REQUIRE(rig.until([&] { return answers(c, false) == before + 1; }));
    REQUIRE(c.inventory().cursor());
    CHECK(c.inventory().cursor()->serial == serial);
    CHECK(c.inventory().cursor()->stack_count == 5);
    CHECK(donate(c, false));
}

TEST_CASE("a guild tribute reply preserves contents added to the donated bag") {
    auto config = tribute_config();
    auto bag = make_item(5001, "Donation bag", 26);
    bag.item_class = 1;
    bag.bag_slots = 4;
    config.characters.front().inventory.push_back(bag);
    ScratchDir files("tribute_reply_bag");
    Rig rig(std::move(config), tribute_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    put_coin_away(c);
    open_master(rig, true);
    c.click_slot(net::ItemSlot::possessions(26), true, false);
    REQUIRE(c.inventory().cursor());
    REQUIRE(donate(c, true));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::GuildTributeDonateItem) == 1; }));
    auto changed = *c.inventory().cursor();
    changed.contents.push_back({0, make_item(5002, "New bag content", 26)});
    deliver_item(c, changed);
    const auto before = answers(c, true);
    rig.server.zone.send_reliable(reply(true, 1));
    REQUIRE(rig.until([&] { return answers(c, true) == before + 1; }));
    REQUIRE(c.inventory().cursor());
    REQUIRE(c.inventory().cursor()->content(0));
    CHECK(c.inventory().cursor()->content(0)->id == 5002);
}

TEST_CASE("an item without a usable serial cannot start an uncorrelatable donation") {
    ScratchDir files("tribute_reply_missing_serial");
    Rig rig(tribute_config(), tribute_options(files));
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    put_coin_away(c);
    open_master(rig, false);
    open_master(rig, true);
    take_rations(c);
    auto item = *c.inventory().cursor();
    for (const auto& serial : {std::string{}, std::string{"0000000000000000"}}) {
        item.serial = serial;
        deliver_item(c, item);
        CHECK_FALSE(donate(c, false));
        CHECK_FALSE(donate(c, true));
        REQUIRE(c.inventory().cursor());
        CHECK(c.inventory().cursor()->stack_count == 5);
    }
}

TEST_CASE("a tribute batch timeout preserves correlation for its late reply") {
    for (bool guild : {false, true}) {
        CAPTURE(guild);
        ScratchDir files("tribute_reply_timeout");
        Rig rig(tribute_config(), tribute_options(files));
        REQUIRE(rig.to_zone());
        auto& c = rig.conn;
        put_coin_away(c);
        open_master(rig,
                    guild); // only this master is open when the batch chooses its channel
        REQUIRE(c.start_bulk_job(net::GameConnection::ItemJobMode::Tribute, {net::ItemSlot::possessions(23)}).empty());
        REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, item_opcode(guild)) == 1; }));
        REQUIRE(c.item_job());
        REQUIRE(c.inventory().cursor());
        const std::string original = c.inventory().cursor()->serial;
        net::GameConnectionTestAccess::update_bulk_job(c, net::GameConnection::now_ms() + 6000);
        CHECK_FALSE(c.item_job());
        CHECK_FALSE(c.inventory().cursor());
        REQUIRE(c.inventory().at(net::ItemSlot::possessions(23)));
        CHECK(c.inventory().at(net::ItemSlot::possessions(23))->serial == original);

        open_master(rig, !guild);
        c.click_slot(net::ItemSlot::possessions(24), true, false);
        REQUIRE(c.inventory().cursor());
        const std::string replacement = c.inventory().cursor()->serial;
        CHECK_FALSE(donate(c, guild));
        CHECK_FALSE(donate(c, !guild));
        const auto before = answers(c, guild);
        rig.server.zone.send_reliable(reply(guild));
        REQUIRE(rig.until([&] { return answers(c, guild) == before + 1; }));
        CHECK_FALSE(c.inventory().at(net::ItemSlot::possessions(23)));
        REQUIRE(c.inventory().cursor());
        CHECK(c.inventory().cursor()->serial == replacement);
        CHECK(donate(c, !guild));
    }
}

TEST_CASE("zone and disconnect discard old tribute reply identities") {
    for (bool guild : {false, true}) {
        for (bool disconnect : {false, true}) {
            CAPTURE(guild);
            CAPTURE(disconnect);
            ScratchDir files("tribute_reply_reset");
            Rig rig(tribute_config(), tribute_options(files));
            REQUIRE(rig.to_zone());
            auto& c = rig.conn;
            put_coin_away(c);
            open_master(rig, guild);
            take_rations(c);
            REQUIRE(donate(c, guild));
            REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, item_opcode(guild)) == 1; }));
            if (!disconnect) {
                c.receive_world_packet_for_test(net::encode(net::ZoneServerInfo{"127.0.0.1", 9000}));
            } else
                c.disconnect();
            const auto before = answers(c, guild);
            REQUIRE(c.inventory().cursor());
            const auto serial = c.inventory().cursor()->serial;
            c.receive_zone_packet_for_test(reply(guild));
            CHECK(answers(c, guild) == before);
            REQUIRE(c.inventory().cursor());
            CHECK(c.inventory().cursor()->serial == serial);

            // Set up the next session without a live reconnect: the prior request
            // cannot keep its donation fence.
            net::GameConnectionTestAccess::state(c) = net::ConnectionState::InZone;
            auto& world = c.world_for_test();
            if (!world.player()) {
                game::Spawn player;
                player.id = 1;
                world.player_id = player.id;
                world.spawns[player.id] = player;
            }
            auto master = *world.player();
            master.id = 77;
            master.is_npc = true;
            world.spawns[master.id] = master;
            auto open = net::encode(net::StartTribute{0, 77, 1});
            if (guild)
                open.opcode = net::to_wire(net::Op::OpenGuildTributeMaster);
            c.receive_zone_packet_for_test(open);
            CHECK(donate(c, guild));
        }
    }
}

TEST_CASE("tribute: a donation reply after walking away still reconciles the donated item") {
    for (bool guild : {false, true}) {
        ScratchDir files("tribute_reply_after_distance_close");
        Rig rig(tribute_config(), tribute_options(files));
        REQUIRE(rig.to_zone());
        auto& c = rig.conn;
        put_coin_away(c);
        open_master(rig, guild);
        take_rations(c);
        REQUIRE(donate(c, guild));
        REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, item_opcode(guild)) == 1; }));
        const auto before = answers(c, guild);
        c.set_player_motion(c.world().player()->position + glm::vec3{0, 0, 21}, 0, {}, true);
        // The donation does not raise IsItemPending (Send_OP_TributeItem @0x484190), so the window's next 500 ms
        // check closes it.
        net::GameConnectionTestAccess::npc_checks(c).tribute = 0;
        c.update();
        CHECK((guild ? c.guild_tribute().master_id : c.tribute().master_id) == 0);
        c.receive_zone_packet_for_test(reply(guild));
        CHECK(answers(c, guild) == before + 1);
        CHECK_FALSE(c.inventory().cursor());
    }
}

TEST_CASE("an unanswered tribute donation stops blocking when the master is opened again or after a long wait") {
    // Stock EQEmu sends no reply when the personal master is gone or out of range (Handle_OP_TributeItem returns),
    // nor for a guild donation without a guild. The mock never answers on its own, as that server.
    using namespace std::chrono_literals;
    for (bool guild : {false, true}) {
        for (bool reopen : {false, true}) {
            CAPTURE(guild);
            CAPTURE(reopen);
            ScratchDir files("tribute_reply_missing");
            auto options = tribute_options(files);
            options.session.idle_timeout = 10min; // only the donation's wait is under test
            ManualClock clock;
            options.session.clock = clock.source();
            Rig rig(tribute_config(), options);
            REQUIRE(rig.to_zone());
            clock.hold();
            auto& c = rig.conn;
            put_coin_away(c);
            open_master(rig, guild);
            take_rations(c);
            REQUIRE(donate(c, guild));
            REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, item_opcode(guild)) == 1; }));
            REQUIRE(c.inventory().cursor()); // no answer: nothing was taken
            CHECK_FALSE(donate(c, guild));
            if (reopen) {
                open_master(rig, guild);
            } else {
                clock.advance(29s);
                CHECK_FALSE(donate(c, guild));
                clock.advance(2s);
            }
            REQUIRE(c.inventory().cursor());
            CHECK(donate(c, guild));
            REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, item_opcode(guild)) == 2; }));
        }
    }
}

TEST_CASE("a tribute batch that timed out without an answer frees donations after the long wait") {
    using namespace std::chrono_literals;
    ScratchDir files("tribute_batch_missing");
    auto options = tribute_options(files);
    options.session.idle_timeout = 10min;
    ManualClock clock;
    options.session.clock = clock.source();
    Rig rig(tribute_config(), options);
    REQUIRE(rig.to_zone());
    clock.hold();
    auto& c = rig.conn;
    put_coin_away(c);
    open_master(rig, false);
    using M = net::GameConnection::ItemJobMode;
    REQUIRE(c.start_bulk_job(M::Tribute, {net::ItemSlot::possessions(23)}).empty());
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::TributeItem) == 1; }));
    net::GameConnectionTestAccess::update_bulk_job(c, net::GameConnection::now_ms() + 6000);
    REQUIRE_FALSE(c.item_job());
    CHECK(c.start_bulk_job(M::Tribute, {net::ItemSlot::possessions(23)}) ==
          "Wait for the previous tribute donation's answer.");
    clock.advance(31s);
    CHECK(c.start_bulk_job(M::Tribute, {net::ItemSlot::possessions(23)}).empty());
}
