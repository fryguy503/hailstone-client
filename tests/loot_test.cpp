// Looting a corpse (net/game_connection_items.cpp): CEverQuest::LootCorpse's checks, Handle_OP_MoneyOnCorpse's
// answers and coin text (FormatCoinAmountsText), Handle_OP_LootItem's refusal and Loot All's close, and
// unk_CLootWnd_ResetLootSession, against the mock EQEmu (whose string table prints "[id] args").

#include <doctest/doctest.h>

#include "game/inventory.h"
#include "game/item_options.h"
#include "mock_server/harness.h"
#include "net/item_packets.h"
#include "net/packet_io.h"
#include "net/trade_packets.h"
#include "plugin/hooks.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

std::vector<std::string> drain_chat(GameConnection& c) {
    std::vector<std::string> lines;
    while (auto e = c.poll_chat()) lines.push_back(e->text);
    return lines;
}

bool has_line(const std::vector<std::string>& lines, const std::string& text) {
    for (const auto& l : lines)
        if (l == text) return true;
    return false;
}

// A corpse `offset` units east of the player.
std::uint32_t add_corpse(GameConnection& conn, float offset) {
    auto& w = conn.world_for_test();
    const game::Spawn* me = w.player();
    REQUIRE(me);
    game::Spawn s;
    s.id = 4000;
    s.name = "a_rat's_corpse01";
    s.is_npc = true;
    s.is_corpse = true;
    s.position = me->position + glm::vec3(offset, 0.0f, 0.0f);
    s.heights.avatar_height = 2.0f;
    w.spawns[s.id] = s;
    return s.id;
}

AppPacket money(std::uint8_t response, std::uint32_t p = 0, std::uint32_t g = 0, std::uint32_t s = 0,
                std::uint32_t c = 0) {
    MoneyOnCorpse m;
    m.response = response;
    m.platinum = p;
    m.gold = g;
    m.silver = s;
    m.copper = c;
    return encode(m);
}

AppPacket loot_item_packet(std::int16_t slot, const std::string& name, std::uint8_t no_drop = 0, bool bag = false) {
    ItemPacket ip;
    ip.type = kItemPacketLoot;
    ip.item.id = 1001;
    ip.item.name = name;
    ip.item.slot.type = 0;
    ip.item.slot.slot = slot;
    ip.item.stack_count = 1;
    ip.item.no_drop = no_drop;
    ip.item.item_class = bag ? 1 : 0;
    return encode_item_packet(false, ip);
}

std::vector<std::uint16_t> loot_requests(const Rig& rig) {
    std::vector<std::uint16_t> slots;
    for (const auto& packet : rig.server.zone_log)
        if (is_op(packet, Op::LootItem)) {
            auto request = decode_loot_item(packet);
            REQUIRE(request);
            CHECK(request->auto_loot == 1);
            slots.push_back(request->slot);
        }
    return slots;
}

// A reliable marker after the clicks proves every preceding request reached the mock. LootItem has no automatic
// mock reply, so the tests can keep a request outstanding until they explicitly deliver its answer.
void flush_loot_requests(Rig& rig) {
    const auto before = MockEqServer::count(rig.server.zone_log, Op::ChannelMessage);
    rig.conn.say("loot-request-marker");
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::ChannelMessage) == before + 1; }));
}

} // namespace

TEST_CASE("loot: FormatCoinAmountsText's English coin text") {
    CHECK(game::coin_amounts_text(1, 2, 3, 4) == "1 platinum, 2 gold, 3 silver and 4 copper");
    CHECK(game::coin_amounts_text(1, 0, 0, 4) == "1 platinum and 4 copper");
    CHECK(game::coin_amounts_text(1, 2, 0, 0) == "1 platinum and 2 gold");
    CHECK(game::coin_amounts_text(1, 2, 0, 4) == "1 platinum, 2 gold and 4 copper");
    CHECK(game::coin_amounts_text(0, 0, 3, 0) == "3 silver");
    CHECK(game::coin_amounts_text(0, 0, 0, 0).empty());
}

TEST_CASE("loot: /loot without a target, a corpse too far, and the request") {
    Rig rig(MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    drain_chat(conn);
    conn.set_target(0);
    conn.send_command("/loot");
    CHECK(has_line(drain_chat(conn), "[13201]"));

    // Beyond AvatarHeight * 1.6 + 14 = 17.2: refused here.
    std::uint32_t corpse = add_corpse(conn, 30.0f);
    conn.loot(corpse);
    CHECK(has_line(drain_chat(conn), "[12390]"));
    CHECK_FALSE(conn.loot_session());

    conn.world_for_test().spawns[corpse].position = conn.world_for_test().player()->position + glm::vec3(10, 0, 0);
    conn.loot(corpse);
    REQUIRE(conn.loot_session());
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootRequest) == 1; }));
    // A second request while one corpse is open does nothing.
    conn.loot(corpse);
    rig.run_for(std::chrono::milliseconds(30));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::LootRequest) == 1);
}

TEST_CASE("loot: Handle_OP_MoneyOnCorpse's answers") {
    Rig rig(MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    std::uint32_t corpse = add_corpse(conn, 5.0f);

    SUBCASE("refusals say why and let the corpse go") {
        const std::pair<std::uint8_t, const char*> cases[] = {
            {0, "[12073]"},
            {2, "[12070]"},
            {4, "[5154]"},
            {5, "[12390]"},
        };
        for (const auto& [response, text] : cases) {
            conn.loot(corpse);
            REQUIRE(conn.loot_session());
            drain_chat(conn);
            conn.receive_zone_packet_for_test(money(response));
            CHECK(has_line(drain_chat(conn), text));
            CHECK_FALSE(conn.loot_session());
        }
        conn.loot(corpse);
        conn.receive_zone_packet_for_test(money(7)); // closes without a word
        CHECK(drain_chat(conn).empty());
        CHECK_FALSE(conn.loot_session());
    }

    SUBCASE("the coin, the looting stand state, then OP_LootComplete") {
        conn.set_target(corpse);
        conn.loot(corpse);
        auto before = conn.coins();
        drain_chat(conn);
        conn.receive_zone_packet_for_test(money(1, 1, 2, 3, 4));
        CHECK(has_line(drain_chat(conn), "[12072] 1 platinum, 2 gold, 3 silver and 4 copper"));
        CHECK(conn.coins().platinum == before.platinum + 1);
        CHECK(conn.coins().copper == before.copper + 4);
        REQUIRE(conn.loot_session());
        CHECK(conn.loot_session()->open);
        CHECK(conn.player_status().stand_state == kStandLooting);

        conn.receive_zone_packet_for_test(loot_item_packet(23, "Cloth Cap"));
        conn.receive_zone_packet_for_test(loot_item_packet(24, "Rusty Dagger"));
        CHECK(conn.loot_session()->items.size() == 2);
        // A refused item stays ("[1370]").
        LootItem refused;
        refused.lootee = corpse;
        refused.slot = 23;
        refused.auto_loot = -1;
        conn.receive_zone_packet_for_test(encode(refused));
        CHECK(has_line(drain_chat(conn), "[1370]"));
        CHECK(conn.loot_session()->items.size() == 2);
        LootItem taken = refused;
        taken.auto_loot = 1;
        conn.receive_zone_packet_for_test(encode(taken));
        CHECK(conn.loot_session()->items.size() == 1);

        conn.receive_zone_packet_for_test(AppPacket{to_wire(Op::LootComplete), {}});
        CHECK_FALSE(conn.loot_session());
        CHECK(conn.world().target_id == 0);
        CHECK(conn.player_status().stand_state == kStandStanding);
    }

    SUBCASE("with /autosplit on the coin is 'your split'") {
        conn.send_command("/autosplit on");
        conn.loot(corpse);
        drain_chat(conn);
        conn.receive_zone_packet_for_test(money(1, 0, 0, 5, 0));
        CHECK(has_line(drain_chat(conn), "[12071] 5 silver"));
    }
}

TEST_CASE("loot: Loot All waits for every item, then closes the window after the last answer") {
    Rig rig(MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    std::uint32_t corpse = add_corpse(conn, 5.0f);
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    conn.receive_zone_packet_for_test(loot_item_packet(23, "Cloth Cap"));
    conn.receive_zone_packet_for_test(loot_item_packet(24, "Rusty Dagger"));
    // Before the server's "every item sent" (6) Loot All only remembers it was asked for.
    conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 0);
    conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 2); // never asks about no-drop items
    conn.destroy_cursor(); // the mock character holds coin, which stops Loot All
    conn.loot_all();
    rig.run_for(std::chrono::milliseconds(30));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::LootItem) == 0);
    conn.receive_zone_packet_for_test(money(6));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootItem) == 1; }));
    LootItem answer;
    answer.lootee = corpse;
    answer.slot = 23;
    answer.auto_loot = 1;
    conn.receive_zone_packet_for_test(encode(answer));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootItem) == 2; }));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 0);
    answer.slot = 24;
    conn.receive_zone_packet_for_test(encode(answer));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 1; }));
}

TEST_CASE("loot: rapid Loot All activation preserves the active queue through its final delayed reply") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const auto corpse = add_corpse(conn, 5.0f);
    conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 0);
    conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 2);
    conn.destroy_cursor();
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    conn.receive_zone_packet_for_test(loot_item_packet(23, "Cloth Cap"));
    conn.receive_zone_packet_for_test(loot_item_packet(24, "Small Bag", 255, true));
    conn.receive_zone_packet_for_test(loot_item_packet(25, "Rusty Dagger"));
    conn.receive_zone_packet_for_test(money(6));

    conn.loot_all();
    for (int click = 0; click < 10; ++click) conn.loot_all();
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{24});

    // Reentry also ignores the button before it could open another Loot All confirmation.
    conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 1);
    for (int click = 0; click < 10; ++click) conn.loot_all();
    CHECK_FALSE(conn.confirmation());
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{24});

    LootItem answer;
    answer.lootee = corpse;
    answer.slot = 24;
    answer.auto_loot = 1;
    conn.receive_zone_packet_for_test(encode(answer));
    for (int click = 0; click < 10; ++click) conn.loot_all();
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{24, 23});
    CHECK_FALSE(conn.confirmation());

    answer.slot = 23;
    conn.receive_zone_packet_for_test(encode(answer));
    // No slots remain queued while the final request waits: its busy state still prevents a duplicate.
    for (int click = 0; click < 10; ++click) conn.loot_all();
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{24, 23, 25});
    CHECK_FALSE(conn.confirmation());
    CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 0);

    answer.slot = 25;
    conn.receive_zone_packet_for_test(encode(answer));
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{24, 23, 25});
    CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 1);
}

TEST_CASE("loot: Loot All does not wait for an individual item request still awaiting its reply") {
    // LootAll's guard (@0x6bdbf5) and the button (WndNotification @0x6be180) ignore an individual request in flight.
    // Each reply advances the queue, the individual one's too, and the queue was built while that item was still
    // listed, so it is asked for again, as the original's stored list does.
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const auto corpse = add_corpse(conn, 5.0f);
    conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 0);
    conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 2);
    conn.destroy_cursor();
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    conn.receive_zone_packet_for_test(loot_item_packet(23, "Cloth Cap"));
    conn.receive_zone_packet_for_test(loot_item_packet(24, "Rusty Dagger"));
    conn.receive_zone_packet_for_test(money(6));

    conn.loot_item(24, true);
    conn.loot_all();
    for (int click = 0; click < 10; ++click) conn.loot_all();
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{24, 23});

    LootItem answer;
    answer.lootee = corpse;
    answer.slot = 24;
    answer.auto_loot = 1;
    conn.receive_zone_packet_for_test(encode(answer));
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{24, 23, 24});
    answer.slot = 23;
    conn.receive_zone_packet_for_test(encode(answer));
    flush_loot_requests(rig);
    // The queue is done, but a reply is still owed: the window waits, and Loot All stays busy.
    CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 0);
    for (int click = 0; click < 10; ++click) conn.loot_all();
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{24, 23, 24});

    answer.slot = 24;
    SUBCASE("the repeated request's reply closes the window") {
        conn.receive_zone_packet_for_test(encode(answer));
        flush_loot_requests(rig);
        CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 1);
    }
    SUBCASE("a refusal keeps the window open and Loot All usable again") {
        answer.auto_loot = -1;
        conn.receive_zone_packet_for_test(encode(answer));
        flush_loot_requests(rig);
        CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 0);
        conn.loot_all(); // nothing left to loot: the window closes
        flush_loot_requests(rig);
        CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 1);
    }
}

TEST_CASE("loot: a dropped LootItem handoff leaves Loot All retryable") {
    plugin::Hooks hooks;
    auto options = quick_options();
    options.hooks = &hooks;
    Rig rig(MockConfig::standard(), options);
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const auto corpse = add_corpse(conn, 5.0f);
    conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 0);
    conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 2);
    conn.destroy_cursor();
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    conn.receive_zone_packet_for_test(loot_item_packet(23, "Cloth Cap"));
    conn.receive_zone_packet_for_test(loot_item_packet(24, "Rusty Dagger"));
    conn.receive_zone_packet_for_test(money(6));
    const auto owner = hooks.new_owner("loot handoff drop test");
    bool drop = true;
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, to_wire(Op::LootItem),
        [&](AppPacket&) { return drop ? plugin::Flow::Drop : plugin::Flow::Continue; });

    SUBCASE("the first request of a new auto-loot queue is dropped") {
        conn.loot_all();
    }
    SUBCASE("a preceding individual request is dropped") {
        conn.loot_item(24, true);
    }
    flush_loot_requests(rig);
    CHECK(loot_requests(rig).empty());
    drop = false;
    conn.loot_all();
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{23});

    LootItem answer;
    answer.lootee = corpse;
    answer.slot = 23;
    answer.auto_loot = 1;
    conn.receive_zone_packet_for_test(encode(answer));
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{23, 24});
    answer.slot = 24;
    conn.receive_zone_packet_for_test(encode(answer));
    flush_loot_requests(rig);
    CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 1);
    hooks.remove(owner);
}

TEST_CASE("loot: a delayed Loot All confirmation runs with an individual request started meanwhile") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const auto corpse = add_corpse(conn, 5.0f);
    conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 1);
    conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 2);
    conn.destroy_cursor();
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    conn.receive_zone_packet_for_test(loot_item_packet(23, "Cloth Cap"));
    conn.receive_zone_packet_for_test(loot_item_packet(24, "Rusty Dagger"));
    conn.receive_zone_packet_for_test(money(6));
    conn.loot_all();
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->id == kConfirmLootAll);
    conn.loot_item(24, true);
    conn.answer_confirmation(true);
    CHECK_FALSE(conn.confirmation());
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{24, 23});

    LootItem answer;
    answer.lootee = corpse;
    answer.slot = 24;
    answer.auto_loot = 1;
    conn.receive_zone_packet_for_test(encode(answer));
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{24, 23, 24});
    answer.slot = 23;
    conn.receive_zone_packet_for_test(encode(answer));
    flush_loot_requests(rig);
    CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 0);
    answer.slot = 24;
    conn.receive_zone_packet_for_test(encode(answer));
    flush_loot_requests(rig);
    CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 1);
}

TEST_CASE("loot: a second Loot All click while its question is pending asks nothing more") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const auto corpse = add_corpse(conn, 5.0f);
    conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 1);
    conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 2);
    conn.destroy_cursor();
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    conn.receive_zone_packet_for_test(loot_item_packet(23, "Cloth Cap"));
    conn.receive_zone_packet_for_test(money(6));

    conn.loot_all();
    conn.loot_all(); // a second click before the first question was answered
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->id == kConfirmLootAll);
    conn.answer_confirmation(true);
    CHECK_FALSE(conn.confirmation()); // one yes was enough: no second question waiting behind it
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{23});
}

TEST_CASE("loot: an accepted NO DROP question keeps Loot All open until both outstanding replies arrive") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const auto corpse = add_corpse(conn, 5.0f);
    conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 0);
    conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 0);
    conn.destroy_cursor();
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    conn.receive_zone_packet_for_test(loot_item_packet(23, "Cloth Cap", 255));
    conn.receive_zone_packet_for_test(loot_item_packet(24, "Relic"));
    conn.receive_zone_packet_for_test(money(6));
    conn.loot_all();
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->data == 24);
    conn.answer_confirmation(true);
    CHECK_FALSE(conn.confirmation());
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{23, 24});

    LootItem answer;
    answer.lootee = corpse;
    answer.auto_loot = 1;
    std::uint16_t first_slot = 23, last_slot = 24;
    SUBCASE("the ordinary item answers first") {}
    SUBCASE("the NO DROP item answers first") {
        first_slot = 24;
        last_slot = 23;
    }
    answer.slot = first_slot;
    conn.receive_zone_packet_for_test(encode(answer));
    for (int click = 0; click < 10; ++click) conn.loot_all();
    flush_loot_requests(rig);
    CHECK(loot_requests(rig) == std::vector<std::uint16_t>{23, 24});
    CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 0);
    answer.slot = last_slot;
    conn.receive_zone_packet_for_test(encode(answer));
    flush_loot_requests(rig);
    CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 1);
}

TEST_CASE("loot: multiple NO DROP items abort the queue but keep its first bag question answerable") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const auto corpse = add_corpse(conn, 5.0f);
    conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 0);
    conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 0);
    conn.destroy_cursor();
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    conn.receive_zone_packet_for_test(loot_item_packet(23, "Cloth Cap", 255));
    conn.receive_zone_packet_for_test(loot_item_packet(24, "Second Relic"));
    conn.receive_zone_packet_for_test(loot_item_packet(25, "First Relic Bag", 0, true));
    conn.receive_zone_packet_for_test(loot_item_packet(26, "Rusty Dagger", 255));
    conn.receive_zone_packet_for_test(money(6));

    conn.loot_all();
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->id == kConfirmLootNoDrop);
    CHECK(conn.confirmation()->data == 25);
    CHECK(conn.confirmation()->text == game::loot_no_drop_question("First Relic Bag"));
    for (int click = 0; click < 10; ++click) conn.loot_all();
    flush_loot_requests(rig);
    CHECK(loot_requests(rig).empty());
    CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 0);
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->data == 25);

    SUBCASE("yes loots only the item named by the surviving question") {
        conn.answer_confirmation(true);
        CHECK_FALSE(conn.confirmation());
        flush_loot_requests(rig);
        CHECK(loot_requests(rig) == std::vector<std::uint16_t>{25});
        LootItem answer;
        answer.lootee = corpse;
        answer.slot = 25;
        answer.auto_loot = 1;
        conn.receive_zone_packet_for_test(encode(answer));
        flush_loot_requests(rig);
        CHECK(loot_requests(rig) == std::vector<std::uint16_t>{25});
        REQUIRE(conn.loot_session());
        CHECK(conn.loot_session()->items.count(25) == 0);
        CHECK(conn.loot_session()->items.count(23) == 1);
        CHECK(conn.loot_session()->items.count(24) == 1);
        CHECK(conn.loot_session()->items.count(26) == 1);
    }

    SUBCASE("no releases the popup and allows a fresh scan or another individual question") {
        conn.answer_confirmation(false);
        CHECK_FALSE(conn.confirmation());
        flush_loot_requests(rig);
        CHECK(loot_requests(rig).empty());
        conn.loot_all();
        REQUIRE(conn.confirmation());
        CHECK(conn.confirmation()->data == 25);
        conn.answer_confirmation(false);
        CHECK_FALSE(conn.confirmation());
        conn.loot_item(24, true);
        REQUIRE(conn.confirmation());
        CHECK(conn.confirmation()->data == 24);
        conn.answer_confirmation(true);
        CHECK_FALSE(conn.confirmation());
        flush_loot_requests(rig);
        CHECK(loot_requests(rig) == std::vector<std::uint16_t>{24});
    }
}

TEST_CASE("loot: Loot All preserves an individual NO DROP question already pending before the scan") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const auto corpse = add_corpse(conn, 5.0f);
    conn.set_client_option_for_session(game::ClientOption::LootAllConfirm, 0);
    conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 0);
    conn.destroy_cursor();
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    conn.receive_zone_packet_for_test(loot_item_packet(23, "Original Relic"));
    conn.receive_zone_packet_for_test(loot_item_packet(24, "Another Relic Bag", 0, true));
    conn.receive_zone_packet_for_test(loot_item_packet(25, "Cloth Cap", 255));
    conn.receive_zone_packet_for_test(money(6));
    conn.loot_item(23, true);
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->data == 23);
    conn.loot_all();
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->data == 23);
    CHECK(conn.confirmation()->text == game::loot_no_drop_question("Original Relic"));
    flush_loot_requests(rig);
    CHECK(loot_requests(rig).empty());
    conn.answer_confirmation(false);
    CHECK_FALSE(conn.confirmation());
}

TEST_CASE("loot: walking away from the corpse closes the window (CLootWnd::OnProcessFrame)") {
    Rig rig(MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    std::uint32_t corpse = add_corpse(conn, 5.0f);
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    rig.run_for(std::chrono::milliseconds(600));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 0);
    // Beyond AvatarHeight * 1.5 + 20 = 23 (without z).
    conn.world_for_test().spawns[corpse].position += glm::vec3(30.0f, 0.0f, 0.0f);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::EndLootRequest) == 1; }));
}

TEST_CASE("loot: a stack on someone else's corpse asks how many (CLootWnd::RequestLootSlot)") {
    Rig rig(MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    std::uint32_t corpse = add_corpse(conn, 5.0f);
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    AppPacket stack = loot_item_packet(23, "Bone Chips");
    {
        ItemPacket ip;
        ip.type = kItemPacketLoot;
        ip.item.id = 13073;
        ip.item.name = "Bone Chips";
        ip.item.slot.slot = 23;
        ip.item.stack_count = 7;
        ip.item.max_stack = 20;
        stack = encode_item_packet(false, ip);
    }
    conn.receive_zone_packet_for_test(stack);
    conn.receive_zone_packet_for_test(loot_item_packet(24, "Rusty Dagger"));
    conn.set_client_option_for_session(game::ClientOption::LootNoDrop, 2); // never asks about no-drop items
    conn.destroy_cursor(); // the mock character holds coin, which stops looting
    rig.run_for(std::chrono::milliseconds(30));
    // A single item goes at once with quantity 0.
    conn.click_slot(ItemSlot::corpse(24));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootItem) == 1; }));
    CHECK_FALSE(conn.quantity_request().has_value());
    // The stack opens the quantity dialog centred, from 7 down; nothing is sent until the answer.
    conn.click_slot(ItemSlot::corpse(23));
    REQUIRE(conn.quantity_request().has_value());
    CHECK(conn.quantity_request()->op == QuantityOp::Loot);
    CHECK(conn.quantity_request()->centred);
    CHECK(conn.quantity_request()->max == 7);
    CHECK(conn.quantity_request()->start == 7);
    rig.run_for(std::chrono::milliseconds(30));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::LootItem) == 1);
    conn.finish_quantity(3);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootItem) == 2; }));
    std::optional<LootItem> sent;
    for (const auto& p : rig.server.zone_log)
        if (is_op(p, Op::LootItem)) sent = decode_loot_item(p);
    REQUIRE(sent);
    CHECK(sent->slot == 23);
    CHECK(sent->auto_loot == 0);
    CHECK(sent->unknown16 == 3);
    // Ctrl takes one at once.
    conn.click_slot(ItemSlot::corpse(23), false, true);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::LootItem) == 3; }));
    for (const auto& p : rig.server.zone_log)
        if (is_op(p, Op::LootItem)) sent = decode_loot_item(p);
    CHECK(sent->unknown16 == 1);
}

TEST_CASE("item options: RequestLootSlot's quantity question") {
    CHECK(game::loot_asks_quantity(false, 2));
    CHECK_FALSE(game::loot_asks_quantity(false, 1));
    CHECK_FALSE(game::loot_asks_quantity(true, 20)); // the player's own corpse
}

TEST_CASE("trade: an answer that comes while busy cancels the trade (Handle_OP_TradeRequestAck)") {
    Rig rig(MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
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
    // A request still waiting for its answer does not make the client busy; a loot window does.
    std::uint32_t corpse = add_corpse(conn, 5.0f);
    conn.loot(corpse);
    conn.receive_zone_packet_for_test(money(1));
    REQUIRE(conn.loot_session().has_value());
    conn.receive_zone_packet_for_test(encode_trade_request_ack({w.player_id, pc.id}));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::CancelTrade) == 1; }));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::ChannelMessage) >= 1);
    CHECK_FALSE(conn.trade().has_value());
}
