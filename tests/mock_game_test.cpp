// Offline integration tests: a real GameConnection (world and zone sessions, the SOE layer under them) against the
// in-process mock EQEmu in tests/mock_server/. The scripts follow docs/protocol/rof2-packets.md section 1 and
// docs/protocol/session.md; where the client does something those documents do not settle, the test says so and
// docs/unknowns.md has the row.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

#include "core/ini_file.h"
#include "game/coords.h"
#include "mock_server/harness.h"
#include "net/chat_server.h"
#include "net/door_packets.h"
#include "net/packet_io.h"
#include "platform/udp_socket.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

// True when `want` occurs in `log` in this order (other packets may sit between).
bool in_order(const std::vector<AppPacket>& log, std::initializer_list<Op> want) {
    auto it = want.begin();
    for (const auto& p : log)
        if (it != want.end() && from_wire(p.opcode) == *it) ++it;
    return it == want.end();
}

bool near(float a, float b, float eps = 0.2f) { return std::fabs(a - b) <= eps; }

} // namespace

// ---------------------------------------------------------------- world and zone-in

namespace {

void check_full_zone_in(Rig& rig) {
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    CHECK(conn.error().empty());

    // World: OP_SendLoginInfo first, with the ticket's account id as text and the key; then the three checksum
    // packets and the character-create table request once the list is in (docs/protocol/rof2-packets.md, World).
    REQUIRE(server.login_infos.size() == 1);
    CHECK(server.login_infos[0].account_id == "77");
    CHECK(server.login_infos[0].session_key == "ABCDEFGHIJ");
    CHECK_FALSE(server.login_infos[0].zoning);
    REQUIRE_FALSE(server.world_log.empty());
    CHECK(from_wire(server.world_log[0].opcode) == Op::SendLoginInfo);
    CHECK(server.world_log[0].data.size() == kLoginInfoSize);
    for (Op crc : {Op::World_Client_CRC1, Op::World_Client_CRC2, Op::World_Client_CRC3})
        CHECK(MockEqServer::count(server.world_log, crc) >= 1);
    CHECK(in_order(server.world_log, {Op::SendLoginInfo, Op::World_Client_CRC1, Op::EnterWorld}));
    REQUIRE(conn.characters().size() == 1);
    CHECK(conn.characters()[0].name == "Gennro");
    CHECK(conn.characters()[0].level == 50);
    CHECK(conn.characters()[0].zone == 2);

    // Zone: OP_ZoneEntry (76 bytes, the character's name), then the client's own requests in the documented order.
    REQUIRE_FALSE(server.zone_log.empty());
    CHECK(from_wire(server.zone_log[0].opcode) == Op::ZoneEntry);
    CHECK(server.zone_log[0].data.size() == kZoneEntryRequestSize);
    auto entry = decode_zone_entry_request(server.zone_log[0]);
    REQUIRE(entry);
    CHECK(entry->name == "Gennro");
    CHECK(in_order(server.zone_log, {Op::ZoneEntry, Op::ReqClientSpawn, Op::SendExpZonein, Op::WorldObjectsSent,
                                     Op::ClientReady}));
    CHECK(MockEqServer::count(server.zone_log, Op::ReqClientSpawn) == 1);
    CHECK(MockEqServer::count(server.zone_log, Op::SendExpZonein) == 1);
    CHECK(MockEqServer::count(server.zone_log, Op::WorldObjectsSent) == 1);
    CHECK(MockEqServer::count(server.zone_log, Op::ClientReady) == 1);
    CHECK(server.zone_entries == 1);

    // Own character: the profile, own spawn and HP.
    REQUIRE(conn.profile());
    CHECK(conn.profile()->name == "Gennro");
    CHECK(conn.profile()->zone_id == 2);
    CHECK(conn.profile()->cur_hp == 4321);
    CHECK(conn.profile()->trailing_bytes == 0);
    CHECK(conn.world().player_id == 7);
    CHECK(conn.player_status().level == 50);
    // OP_HPUpdate after OP_ClientReady carries the maxima the profile lacks.
    CHECK(rig.until([&] { return conn.player_status().max_hp == 5000; }));
    CHECK(conn.player_status().hp == 4321);

    // Spawns: our own and the zone's two, at their server coordinates swapped into world x/y (game/coords.h).
    const auto& spawns = conn.world().spawns;
    CHECK(spawns.size() == 3);
    REQUIRE(spawns.count(7));
    REQUIRE(spawns.count(101));
    REQUIRE(spawns.count(102));
    CHECK(spawns.at(101).name == "a_rat001");
    CHECK(spawns.at(101).is_npc);
    CHECK(spawns.at(101).race == 54);
    CHECK(near(spawns.at(101).position.x, -140.0f));
    CHECK(near(spawns.at(101).position.y, -300.0f));
    CHECK(near(spawns.at(101).position.z, 3.0f));
    CHECK(spawns.at(102).name == "Guard_Bob");
    CHECK_FALSE(spawns.at(7).is_npc);
    CHECK(near(spawns.at(7).position.x, -148.0f));
    CHECK(near(spawns.at(7).position.y, -285.0f));

    // Zone data: OP_NewZone, OP_TimeOfDay, zone points, and the zone to load.
    REQUIRE(conn.zone_info());
    CHECK(conn.zone_info()->zone_short_name == "qeynos2");
    CHECK(conn.zone_info()->zone_long_name == "North Qeynos");
    CHECK(conn.zone_id() == 2);
    CHECK(conn.world().zone_short_name == "qeynos2");
    CHECK(conn.take_zone_to_load() == std::optional<std::string>("qeynos2"));
    CHECK_FALSE(conn.take_zone_to_load().has_value());
    REQUIRE(conn.time_of_day());
    CHECK(conn.time_of_day()->hour == 12);
    REQUIRE(conn.zone_points().size() == 1);
    CHECK(conn.zone_points()[0].zone_id == 1);

    // Inventory from OP_CharInventory.
    const auto* sword = conn.inventory().at(ItemSlot::possessions(13));
    REQUIRE(sword);
    CHECK(sword->name == "Short Sword");
    CHECK(sword->id == 1001);
    const auto* rations = conn.inventory().at(ItemSlot::possessions(23));
    REQUIRE(rations);
    CHECK(rations->stack_count == 5);
    // Food and drink from the profile, before any OP_Stamina.
    CHECK(conn.spell_state().hunger == 5999);
    CHECK(conn.spell_state().thirst == 6000);

    // docs/protocol/rof2-packets.md (World): the checksums, then OP_WorldClientReady once (DoCharacterSelection);
    // OP_WorldComplete after OP_ZoneServerInfo, and the zone is connected after world dropped the link.
    CHECK(MockEqServer::count(server.world_log, Op::WorldClientReady) == 1);
    CHECK(in_order(server.world_log, {Op::World_Client_CRC3, Op::WorldClientReady, Op::EnterWorld, Op::WorldComplete}));
    // The membership world sent (EQEmu: gold, 25 entries of which the original keeps 22) and its feature table.
    REQUIRE(conn.membership());
    CHECK(conn.membership()->level == 2);
    CHECK(conn.membership()->races == 0x1ffffu);
    CHECK(conn.membership()->entries.size() == kMembershipEntryCap);
    REQUIRE(conn.membership_details());
    CHECK(conn.membership_details()->settings.size() == 66);
    CHECK(conn.membership_details()->classes.size() == 1);
    // The tribute lists are asked for on the first zone-in: personal, then guild (not in a guild here), then the
    // client spawn (ProcessGame, then DoMainLoop).
    CHECK(MockEqServer::count(server.zone_log, Op::SendTributes) == 1);
    CHECK(MockEqServer::count(server.zone_log, Op::RequestGuildTributes) == 1);
    CHECK(in_order(server.zone_log, {Op::UpdateAA, Op::SendTributes, Op::RequestGuildTributes, Op::ReqClientSpawn}));

    // The MOTD and the zone message reached the chat window.
    auto lines = rig.chat();
    CHECK(has_line(lines, "Welcome to the mock server."));
    CHECK(has_line(lines, "You have entered North Qeynos."));
}

} // namespace

TEST_CASE("mock server: world auth, character select and zone-in, with the original's session timers") {
    // The only test that keeps the default SessionOptions: 150 ms multi-buffer hold, 20 ms service pass.
    Rig rig(MockConfig::standard(), ConnectionOptions{});
    check_full_zone_in(rig);
}

TEST_CASE("mock server: a session key too long for OP_SendLoginInfo fails the world login, nothing sent") {
    Rig rig;
    auto ticket = rig.server.ticket();
    ticket.session_key = std::string(61, 'K'); // "77\0" + 61 + "\0" is 65 bytes: one too many
    rig.conn.start(ticket);
    REQUIRE(rig.until_state(net::ConnectionState::Disconnected));
    CHECK(rig.conn.error().find("session key is too long") != std::string::npos);
    rig.run_for(50ms);
    CHECK(rig.server.login_infos.empty());
}

TEST_CASE("mock server: the same zone-in with the zone stream compressed and XORed") {
    // EQEmu may compress the zone stream (Network:CompressZoneStream); the profile alone is ~25 KB, so this also
    // reassembles dozens of compressed fragments.
    MockConfig config = MockConfig::standard();
    config.zone_params.passes = {soe::EncodePass::Compression, soe::EncodePass::Xor};
    Rig rig(std::move(config));
    check_full_zone_in(rig);
}

// ---------------------------------------------------------------- gameplay traffic

TEST_CASE("mock server: chat, spawns, HP, items and position updates in zone") {
    Rig rig;
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    rig.chat(); // the zone-in lines

    // ---- chat from the server: the channel picks the event kind and the eqstr template (1410 say, 1420 shout).
    server.chat("Guard_Bob", 8, "Halt!");
    server.chat("Gennro", 8, "my own line");  // the server's copy of our own line is dropped (msg_new_text)
    server.chat("Guard_Bob", 3, "Intruder!"); // shout; reliable delivery is in order, so once it shows, the rest has run
    std::vector<ChatEvent> lines;
    REQUIRE(rig.until([&] {
        for (auto& e : rig.chat()) lines.push_back(std::move(e));
        return lines.size() >= 2;
    }));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].kind == ChatEvent::Kind::Say);
    CHECK(lines[0].text == "SAY Guard Bob: Halt!"); // display_name: underscores become spaces
    CHECK(lines[1].kind == ChatEvent::Kind::Shout);
    CHECK(lines[1].text == "SHOUT Guard Bob: Intruder!");
    // Each line keeps the original's colour index (what the chat windows' filters route by): 256 say, 262 shout.
    CHECK(lines[0].color == 256);
    CHECK(lines[1].color == 262);
    // A tell names its sender (the tell windows' tab).
    server.chat("Guard_Bob", 7, "psst", "Gennro");
    lines.clear();
    REQUIRE(rig.until([&] {
        for (auto& e : rig.chat()) lines.push_back(std::move(e));
        return !lines.empty();
    }));
    CHECK(lines[0].kind == ChatEvent::Kind::Tell);
    CHECK(lines[0].color == 257);
    CHECK(lines[0].peer == "Guard Bob");

    // ---- chat from the client: OP_ChannelMessage on the say channel (8).
    conn.say("Hail, Guard_Bob");
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, Op::ChannelMessage) == 1; }));
    auto sent = std::find_if(server.zone_log.begin(), server.zone_log.end(),
                             [](const AppPacket& p) { return from_wire(p.opcode) == Op::ChannelMessage; });
    auto said = decode_channel_message(*sent);
    REQUIRE(said);
    CHECK(said->sender == "Gennro");
    CHECK(said->channel == 8);
    CHECK(said->message == "Hail, Guard_Bob");

    // ---- spawns come and go
    server.add_spawn(make_spawn("a_bat001", 103, 1, 36, -100.0f, -50.0f, 5.0f, 128.0f));
    REQUIRE(rig.until([&] { return conn.world().spawns.count(103) == 1; }));
    CHECK(conn.world().spawns.at(103).name == "a_bat001");
    CHECK(conn.world().spawns.at(103).race == 36);
    CHECK(near(conn.world().spawns.at(103).position.x, -50.0f)); // world x is the server's y
    CHECK(near(conn.world().spawns.at(103).position.y, -100.0f));
    CHECK(conn.world().spawns.size() == 4);

    server.remove_spawn(101);
    REQUIRE(rig.until([&] { return conn.world().spawns.count(101) == 0; }));
    CHECK(conn.world().spawns.size() == 3);

    // OP_ClientUpdate from the server (unreliable, 24 bytes, packed): the spawn snaps to it.
    Motion m;
    m.x = -120.0f;
    m.y = -60.0f;
    m.z = 6.5f;
    m.heading = 64.0f;
    m.delta_x = 1.0f;
    server.move_spawn(103, m);
    REQUIRE(rig.until([&] { return near(conn.world().spawns.at(103).position.x, -60.0f); }));
    CHECK(near(conn.world().spawns.at(103).position.y, -120.0f));
    CHECK(near(conn.world().spawns.at(103).position.z, 6.5f));
    CHECK(near(conn.world().spawns.at(103).heading, 64.0f, 1.0f));
    // An update for a spawn we do not know is ignored.
    server.move_spawn(999, m);

    // ---- HP: our own (the points window) and another spawn's percentage
    server.hp_update(static_cast<std::int16_t>(conn.world().player_id), 3000, 5000);
    server.hp_update(102, 50, 100);
    REQUIRE(rig.until([&] { return conn.player_status().hp == 3000; }));
    CHECK(conn.player_status().max_hp == 5000);
    REQUIRE(rig.until([&] { return conn.world().spawns.at(102).hp_percent == 50; }));
    server.hp_update(102, 30000000, 40000000); // a raid boss: * 100 is past 2^31
    REQUIRE(rig.until([&] { return conn.world().spawns.at(102).hp_percent == 75; }));
    server.hp_update(102, 1, 40000000); // alive: at least 1% (SetSpawnHP)
    REQUIRE(rig.until([&] { return conn.world().spawns.at(102).hp_percent == 1; }));
    server.hp_update(102, 0, 40000000);
    REQUIRE(rig.until([&] { return conn.world().spawns.at(102).hp_percent == 0; }));

    // ---- items: OP_ItemPacket puts an item in its slot, OP_DeleteItem with a negative count takes from a stack
    server.item_packet(kItemPacketCharInventory, make_item(1003, "Silk Mask", 2));
    REQUIRE(rig.until([&] { return conn.inventory().at(ItemSlot::possessions(2)) != nullptr; }));
    CHECK(conn.inventory().at(ItemSlot::possessions(2))->name == "Silk Mask");
    MoveItem take;
    take.from = ItemSlot::possessions(23);
    take.count = static_cast<std::uint32_t>(-3);
    server.zone.send_reliable(encode_delete_item(take));
    REQUIRE(rig.until([&] { return conn.inventory().at(ItemSlot::possessions(23))->stack_count == 2; }));
    take.count = static_cast<std::uint32_t>(-2);
    server.zone.send_reliable(encode_delete_item(take));
    REQUIRE(rig.until([&] { return conn.inventory().at(ItemSlot::possessions(23)) == nullptr; }));
    CHECK(conn.inventory().at(ItemSlot::possessions(13)) != nullptr);

    // ---- the player's position goes out as OP_ClientUpdate, and the server's copy of it can be echoed back
    server.echo_client_updates_as = 102;
    conn.set_player_motion(glm::vec3(-90.0f, -270.0f, 4.0f), 100.0f, glm::vec3(0.0f), false);
    REQUIRE(rig.until([&] { return !server.client_updates.empty(); }));
    const auto& u = server.client_updates.front();
    CHECK(u.spawn_id == 7);
    CHECK(u.motion.x == -270.0f); // world (x, y) -> server (y, x) (game/coords.h)
    CHECK(u.motion.y == -90.0f);
    CHECK(u.motion.z == 4.0f);
    CHECK(u.motion.heading == 100.0f);
    CHECK(u.motion.animation == 0);
    REQUIRE(rig.until([&] { return near(conn.world().spawns.at(102).position.x, -90.0f); }));
    CHECK(near(conn.world().spawns.at(102).position.y, -270.0f));
    CHECK(conn.error().empty());
    CHECK(conn.state() == ConnectionState::InZone);
}

// ---------------------------------------------------------------- OP_ClientUpdate cadence

TEST_CASE("mock server: OP_ClientUpdate is sent at most every 280 ms, with the current position, only when it changed") {
    // docs/protocol/rof2-packets.md: "OP_ClientUpdate (46 B) when moving"; the rate is the client's kPositionInterval
    // (280 ms, net/game_connection.cpp). The client has no clock seam (game_connection.cpp reads
    // steady_clock directly), so this test runs for real time, with bounds that hold however slow the machine is.
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& updates = rig.server.client_updates;
    // A player who has not moved sends nothing.
    CHECK(updates.empty());

    auto start = std::chrono::steady_clock::now();
    int i = 0;
    rig.conn.set_speed_run(0.7f); // the controls' SpeedRun: the animation field carries it x 40
    while (std::chrono::steady_clock::now() - start < 450ms) {
        // The player keeps running east: the position changes every pass.
        rig.conn.set_player_motion(glm::vec3(-90.0f + 0.5f * static_cast<float>(i++), -270.0f, 4.0f), 100.0f,
                                   glm::vec3(0.5f, 0.0f, 0.0f), true);
        rig.step();
        std::this_thread::sleep_for(2ms);
    }
    float last_x = -90.0f + 0.5f * static_cast<float>(i - 1);
    // The final position is always delivered: sent once the interval allows.
    REQUIRE(rig.until([&] { return !updates.empty() && updates.back().motion.y == last_x; }));
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Sends are at least 280 ms apart, so n updates need (n - 1) * 280 ms.
    auto n = static_cast<long long>(updates.size());
    CHECK(n >= 2);
    CHECK((n - 1) * 280 <= std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    for (std::size_t k = 0; k < updates.size(); ++k) {
        CHECK(updates[k].spawn_id == 7);
        CHECK(updates[k].motion.animation == 28); // SpeedRun 0.7 x 40, truncated (PackPlayerPositionUpdate)
        CHECK(updates[k].motion.x == -270.0f);   // world y is the server's x (game/coords.h)
        CHECK(updates[k].motion.z == 4.0f);
        if (k > 0) {
            CHECK(static_cast<std::uint16_t>(updates[k].sequence - updates[k - 1].sequence) == 1);
            CHECK(updates[k].motion.y > updates[k - 1].motion.y); // each carries the position at its own send
        }
    }

    // Nothing changed since: no more updates (the next one could not come sooner than 280 ms after the last).
    rig.run_for(320ms);
    CHECK(static_cast<long long>(updates.size()) == n);
}

TEST_CASE("mock server: a slow walk at a high frame rate still sends OP_ClientUpdate") {
    // Each frame moves the player less than 0.01 (sneaking at hundreds of frames a second); the steps add up, and the
    // change is measured from the place last sent, not from the frame before.
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& updates = rig.server.client_updates;
    glm::vec3 p(-90.0f, -270.0f, 4.0f);
    rig.conn.set_player_motion(p, 100.0f, glm::vec3(0.0f), true);
    REQUIRE(rig.until([&] { return updates.size() == 1; }));
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 400ms) {
        p.x += 0.004f;
        rig.conn.set_player_motion(p, 100.0f, glm::vec3(0.004f, 0.0f, 0.0f), true);
        rig.step();
        std::this_thread::sleep_for(2ms);
    }
    REQUIRE(rig.until([&] { return updates.size() >= 2; }));
    CHECK(updates.back().motion.y > -90.0f + 0.01f);
}

// ---------------------------------------------------------------- zoning

TEST_CASE("mock server: zone lines and server-requested zoning go through world again") {
    // docs/protocol/rof2-packets.md, "Zoning, camp": C->Z OP_ClientUpdate and OP_ZoneChange (success 0); the zone
    // answers success 1; the client leaves, reconnects to world with LoginInfo::zoning and enters the new zone.
    Rig rig;
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    rig.chat();

    // A refused request (a negative answer other than -1, EQEmu subtracts one more from its own negatives) prints
    // a line and frees the request.
    server.zone_change_answers = {-2};
    REQUIRE(conn.request_zone_change(1, 0, glm::vec3(10.0f, 20.0f, 3.0f)));
    CHECK(conn.zone_change_pending());
    CHECK_FALSE(conn.request_zone_change(1, 0, glm::vec3(10.0f, 20.0f, 3.0f))); // ignored while one is pending
    REQUIRE(rig.until([&] { return !conn.zone_change_pending(); }));
    CHECK(has_line(rig.chat(), "You can't go there right now."));
    CHECK(conn.state() == ConnectionState::InZone);
    CHECK(MockEqServer::count(server.zone_log, Op::ZoneChange) == 1);

    // "Not ready" (-1) within 7 s: the same request again 500 ms later (RequestZoneChangeStatusBlocking); then yes.
    server.zone_change_answers = {-1, 1};
    REQUIRE(conn.request_zone_change(1, 0, glm::vec3(10.0f, 20.0f, 3.0f)));
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, Op::ZoneChange) == 3; }));
    // Each request is OP_ClientUpdate (the zone point is chosen from it), then OP_ZoneChange.
    std::vector<ZoneChange> requests;
    for (const auto& p : server.zone_log)
        if (is_op(p, Op::ZoneChange)) requests.push_back(*decode_zone_change(p));
    REQUIRE(requests.size() == 3);
    for (const auto& r : requests) {
        CHECK(r.char_name == "Gennro");
        CHECK(r.zone_id == 1);
        CHECK(r.instance_id == 0);
        CHECK(r.success == 0);
        CHECK(r.zone_reason == 0);
        CHECK(r.x == 20.0f); // world (10, 20, 3) -> server x = 20, y = 10
        CHECK(r.y == 10.0f);
        CHECK(r.z == 3.0f);
    }
    CHECK(in_order(server.zone_log, {Op::ClientUpdate, Op::ZoneChange}));

    // The answer: world again, with zoning set, then the new zone's zone-in.
    REQUIRE(rig.until([&] { return server.zone_entries == 2 && conn.state() == ConnectionState::InZone; }));
    REQUIRE(server.login_infos.size() == 2);
    CHECK(server.login_infos[1].zoning);
    CHECK(server.login_infos[1].account_id == "77");
    CHECK(server.login_infos[1].session_key == "ABCDEFGHIJ");
    CHECK(MockEqServer::count(server.world_log, Op::EnterWorld) == 2);
    auto last_enter = std::find_if(server.world_log.rbegin(), server.world_log.rend(),
                                   [](const AppPacket& p) { return is_op(p, Op::EnterWorld); });
    REQUIRE(last_enter != server.world_log.rend());
    CHECK(decode_enter_world(*last_enter)->name == "Gennro"); // the name world sent while zoning
    CHECK_FALSE(conn.zone_change_pending());
    CHECK(has_line(rig.chat(), "Zoning..."));
    // MoveToZone: on the success answer OP_DeleteSpawn with our own id (4 bytes); the zone drops the link, then world.
    auto own_delete = std::find_if(server.zone_log.begin(), server.zone_log.end(),
                                   [](const AppPacket& p) { return is_op(p, Op::DeleteSpawn); });
    REQUIRE(own_delete != server.zone_log.end());
    CHECK(own_delete->data.size() == 4);
    CHECK(decode_delete_spawn(*own_delete) == 7u);
    CHECK(server.own_delete_spawns == 1);
    CHECK(in_order(server.zone_log, {Op::ZoneChange, Op::DeleteSpawn}));
    // The tribute lists are not asked for again.
    CHECK(MockEqServer::count(server.zone_log, Op::SendTributes) == 1);
    CHECK(conn.zone_id() == 1);
    REQUIRE(conn.zone_info());
    CHECK(conn.zone_info()->zone_short_name == "qeynos");
    CHECK(conn.world().zone_long_name == "South Qeynos");
    // The old zone's spawns are gone, the new zone's are in.
    CHECK(conn.world().spawns.count(101) == 0);
    CHECK(conn.world().spawns.count(102) == 0);
    CHECK(conn.world().spawns.count(201) == 1);
    CHECK(conn.world().player_id == 7);
    CHECK(near(conn.world().spawns.at(7).position.x, 10.0f));
    CHECK(conn.zone_points().empty()); // qeynos has none in the mock
    CHECK(server.zone.sessions() == 2); // a fresh zone session

    // The server may ask too (death, gate, #zone): OP_RequestClientZoneChange, answered with OP_ZoneChange.
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, Op::ClientReady) == 2; }));
    server.request_zone_change(2, -285.0f, -148.0f, 3.5f);
    REQUIRE(rig.until([&] { return server.zone_entries == 3 && conn.state() == ConnectionState::InZone; }));
    CHECK(conn.zone_id() == 2);
    CHECK(server.login_infos.size() == 3);
    CHECK(conn.world().spawns.count(101) == 1);
    CHECK(conn.world().spawns.count(201) == 0);
    auto last_change = std::find_if(server.zone_log.rbegin(), server.zone_log.rend(),
                                    [](const AppPacket& p) { return is_op(p, Op::ZoneChange); });
    REQUIRE(last_change != server.zone_log.rend());
    auto asked = decode_zone_change(*last_change);
    REQUIRE(asked);
    CHECK(asked->zone_id == 2);
    CHECK(asked->zone_reason == 0x0b); // the request's type is passed back
    CHECK(conn.error().empty());
}

// ---------------------------------------------------------------- world containers

TEST_CASE("mock server: a tradeskill station opens in the tradeskill window, its own window only by Experiment") {
    Rig rig;
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    ClickObjectAction forge;
    forge.drop_id = 12;
    forge.type = 16; // a forge: a combine container
    forge.name = "Forge";
    GroundObject station;
    station.drop_id = forge.drop_id;
    station.object_type = 16;
    const auto at = conn.world().player()->position;
    station.x = at.y;
    station.y = at.x;
    station.z = at.z;
    conn.receive_zone_packet_for_test(encode(station));
    REQUIRE(conn.click_object(forge.drop_id));
    conn.receive_zone_packet_for_test(encode(forge));
    REQUIRE(conn.world_container());
    CHECK(conn.tradeskill().open);
    CHECK(conn.tradeskill().world);
    CHECK_FALSE(conn.world_container()->window);
    // The tradeskill window's close button hides it; the container gets no window of its own.
    conn.close_tradeskill();
    REQUIRE(conn.world_container());
    CHECK_FALSE(conn.world_container()->window);
    // Experiment: the container in its own window, the tradeskill window closed.
    REQUIRE(conn.click_object(forge.drop_id));
    conn.receive_zone_packet_for_test(encode(forge));
    REQUIRE(conn.tradeskill().open);
    conn.show_world_container_window();
    CHECK(conn.world_container()->window);
    CHECK_FALSE(conn.tradeskill().open);
    conn.close_world_container();
    CHECK_FALSE(conn.world_container());
    // A container that is not a combine container opens in its own window.
    ClickObjectAction box = forge;
    box.type = 1;
    box.name = "Box";
    REQUIRE(conn.click_object(box.drop_id));
    conn.receive_zone_packet_for_test(encode(box));
    REQUIRE(conn.world_container());
    CHECK_FALSE(conn.tradeskill().open);
    CHECK(conn.world_container()->window);
}

// ---------------------------------------------------------------- camp

TEST_CASE("mock server: camping sits, sends OP_Camp and ends when the zone drops the link") {
    // docs/protocol/rof2-packets.md: C->Z OP_Camp, then after the client's own 30 s timer OP_Logout; the zone drops
    // the link. The 30 s timer cannot be waited out here (no clock seam), so the test covers what happens before it
    // and the link drop; the OP_Logout step was checked live (2026-10-25).
    Rig rig;
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    rig.chat();

    CHECK(conn.in_zone());
    conn.camp();
    CHECK(conn.state() == ConnectionState::Camping);
    CHECK(conn.in_zone()); // the plugins' in_zone: their windows stay up while camping
    CHECK(conn.sitting());
    REQUIRE(rig.until([&] { return server.camp_requests == 1; }));
    // Sitting down comes first: OP_SpawnAppearance {me, 14, 110}, then OP_Camp.
    CHECK(in_order(server.zone_log, {Op::SpawnAppearance, Op::Camp}));
    auto appearance = std::find_if(server.zone_log.begin(), server.zone_log.end(),
                                   [](const AppPacket& p) { return is_op(p, Op::SpawnAppearance); });
    REQUIRE(appearance != server.zone_log.end());
    auto a = decode_spawn_appearance(*appearance);
    REQUIRE(a);
    CHECK(a->spawn_id == 7);
    CHECK(a->type == 14);
    CHECK(a->parameter == 110);
    CHECK(has_line(rig.chat(), "CAMPING")); // eqstr 12293
    CHECK(server.logouts == 0);

    // Standing up abandons the camp (PlayerZoneClient::ChangePosition: 12290).
    conn.set_sitting(false);
    CHECK(conn.state() == ConnectionState::InZone);
    CHECK(has_line(rig.chat(), "[12290]")); // no template in the fixture file: the id in brackets

    // So does the sit key (and moving forward while sitting, ProcessControls' auto stand): CEverQuest::Sit.
    conn.camp();
    REQUIRE(conn.state() == ConnectionState::Camping);
    REQUIRE(conn.sitting());
    rig.chat();
    conn.sit_toggle();
    CHECK(conn.state() == ConnectionState::InZone);
    CHECK(!conn.sitting());
    CHECK(has_line(rig.chat(), "[12290]"));

    conn.camp();
    REQUIRE(conn.state() == ConnectionState::Camping);
    REQUIRE(rig.until([&] { return server.camp_requests == 3; }));
    // The camp saves first (Save(0, 1, 0x21): OP_Save), then OP_Camp {0}.
    CHECK(in_order(server.zone_log, {Op::Save, Op::Camp}));
    auto save = std::find_if(server.zone_log.rbegin(), server.zone_log.rend(),
                             [](const AppPacket& p) { return is_op(p, Op::Save); });
    REQUIRE(save != server.zone_log.rend());
    auto sv = decode_save(*save);
    REQUIRE(sv);
    CHECK(sv->kind == 0x21);
    CHECK(MockEqServer::count(server.zone_log, Op::Save) == 3);
    // The zone dropping the link during the camp's 30 s is the original's link death (DoMainLoop's
    // HandleWorldDisconnect), not a camp's end: the app shows the disconnected screen, then goes the camp's way.
    server.zone.send_disconnect();
    REQUIRE(rig.until_state(ConnectionState::Disconnected));
    CHECK(!conn.camped_out());
    REQUIRE(conn.link_death());
    CHECK(conn.link_death()->in_game);
    CHECK(conn.link_death()->camp_kind == 0);
}

TEST_CASE("mock server: fast camp sends OP_Logout right after OP_Camp and ends the camp") {
    // Hailstone's fast_camp option (not the original's): no 30 s wait. Off, the camp waits (the test above).
    Rig rig;
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    CHECK_FALSE(conn.fast_camp());
    conn.set_fast_camp(true);
    conn.say("/camp desk");
    REQUIRE(rig.until([&] { return server.logouts == 1; }));
    CHECK(server.camp_requests == 1);
    CHECK(in_order(server.zone_log, {Op::Save, Op::Camp, Op::Logout}));
    REQUIRE(rig.until_state(ConnectionState::Disconnected));
    REQUIRE(conn.camped_out());
    CHECK(*conn.camped_out() == 1);
    CHECK(!conn.link_death());
}

TEST_CASE("mock server: /camp on a mount camps from the saddle; getting off cancels it") {
    // Send_OP_Camp @0x5317a0 refuses a mounted player (12291 standing, 12292 sitting). Hailstone's deliberate
    // difference (docs/mechanics.md "Camp"): the camp goes ahead on a mount without sitting, and holds while mounted.
    Rig rig;
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    rig.chat();
    auto& w = conn.world_for_test();
    REQUIRE(w.player());
    const std::uint32_t me = w.player_id;
    auto& horse = w.spawns[9000];
    horse.id = 9000;
    horse.name = w.player()->name + "`s_Mount00";
    horse.race = 216;
    horse.is_npc = true;
    horse.rider_id = me;
    w.spawns[me].mount_id = 9000;
    conn.camp();
    CHECK(conn.state() == ConnectionState::Camping);
    CHECK(!conn.sitting());
    REQUIRE(rig.until([&] { return server.camp_requests == 1; }));
    CHECK(in_order(server.zone_log, {Op::Save, Op::Camp}));
    CHECK(MockEqServer::count(server.zone_log, Op::SpawnAppearance) == 0); // no sit
    CHECK(has_line(rig.chat(), "CAMPING")); // eqstr 12293
    CHECK_FALSE(has_line(rig.chat(), "[12291]"));
    // Still mounted and standing: the camp tick keeps it.
    rig.step();
    CHECK(conn.state() == ConnectionState::Camping);
    // Off the mount, standing: cancelled as any other stand (12683).
    w.spawns[me].mount_id = 0;
    rig.step();
    CHECK(conn.state() == ConnectionState::InZone);
    CHECK(has_line(rig.chat(), "[12683]"));

    // Sitting, then mounted (12292 in the original): camps too.
    conn.set_sitting(true);
    REQUIRE(conn.sitting());
    w.spawns[me].mount_id = 9000;
    conn.camp();
    CHECK(conn.state() == ConnectionState::Camping);
    REQUIRE(rig.until([&] { return server.camp_requests == 2; }));
    CHECK_FALSE(has_line(rig.chat(), "[12292]"));
}

TEST_CASE("mock server: on a boat the position goes out in its frame; a right click asks for the helm") {
    // PackPlayerPositionUpdateOnVehicle (DoMainLoop @0x53e0de): the physics block in the vehicle's frame with its
    // id; RightClickedOnPlayer's boat case: OP_ControlBoat {id, 1} for the boat stood on; ProcessControls lets go
    // ({id, 0}) when the player steps off.
    Rig rig;
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    auto& w = conn.world_for_test();
    auto& boat = w.spawns[9001];
    boat.id = 9001;
    boat.name = "a_boat000";
    boat.race = 141;
    boat.gender = 2;
    boat.is_npc = true;
    boat.position = glm::vec3(100.0f, 200.0f, -10.0f);
    boat.heading = 128.0f; // a quarter turn
    conn.set_standing_on(9001);
    // Two units ahead of the boat along its heading (world +y at heading 128), on its deck.
    conn.set_player_motion(glm::vec3(100.0f, 202.0f, -5.0f), 128.0f, glm::vec3(0.0f), true);
    conn.send_position_now();
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, Op::ClientUpdate) > 0; }));
    std::optional<ClientPositionUpdate> u;
    for (const auto& p : server.zone_log)
        if (is_op(p, Op::ClientUpdate)) u = decode_client_position_update(p);
    REQUIRE(u);
    CHECK(u->vehicle_id == 9001);
    // In the boat's frame: 2 forward (the frame's first axis is world x, sent as the server's y), 5 up, heading 0.
    CHECK(u->motion.y == doctest::Approx(2.0f).epsilon(0.01));
    CHECK(u->motion.x == doctest::Approx(0.0f).scale(1.0));
    CHECK(u->motion.z == doctest::Approx(5.0f));
    CHECK(u->motion.heading == doctest::Approx(0.0f).scale(1.0));
    CHECK(conn.right_click_boat(9001));
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, Op::ControlBoat) == 1; }));
    auto take = std::find_if(server.zone_log.rbegin(), server.zone_log.rend(),
                             [](const AppPacket& p) { return is_op(p, Op::ControlBoat); });
    REQUIRE(take->data.size() == 8);
    CHECK(std::to_integer<int>(take->data[0]) == 0x29); // 9001 = 0x2329
    CHECK(std::to_integer<int>(take->data[4]) == 1);
}

TEST_CASE("mock server: /camp desk and /camp server send their kind; a camp ends at character select again") {
    // Cmd_camp: the argument's first four letters against eqstr 13258 "desk" and 13259 "server".
    Rig rig;
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    auto last_camp_kind = [&] {
        auto it = std::find_if(server.zone_log.rbegin(), server.zone_log.rend(),
                               [](const AppPacket& p) { return is_op(p, Op::Camp); });
        std::uint32_t kind = 0xffffffff;
        if (it != server.zone_log.rend() && it->data.size() >= 4) std::memcpy(&kind, it->data.data(), 4);
        return kind;
    };
    conn.say("/camp DESKTOP");
    REQUIRE(rig.until([&] { return server.camp_requests == 1; }));
    CHECK(last_camp_kind() == 1);
    conn.say("/camp"); // a second /camp cancels, and the kind goes back to 0
    CHECK(conn.state() == ConnectionState::InZone);
    conn.say("/camp serv");
    REQUIRE(rig.until([&] { return server.camp_requests == 2; }));
    CHECK(last_camp_kind() == 2);
    conn.say("/camp");
    conn.say("/camp desk");
    REQUIRE(rig.until([&] { return server.camp_requests == 3; }));
    CHECK(last_camp_kind() == 1);
    // A link death during it keeps the kind: DoMainLoop's exit goes to the desktop (0x1e5 with the quit flag at 0xdd2664 1).
    server.zone.send_disconnect();
    REQUIRE(rig.until_state(ConnectionState::Disconnected));
    CHECK(!conn.camped_out());
    REQUIRE(conn.link_death());
    CHECK(conn.link_death()->in_game);
    CHECK(conn.link_death()->camp_kind == 1);

    // The app's camp_finished: a new connection on the same ticket logs in with zoning 0 and reaches character
    // select, and the character enters the world again.
    GameConnection again(rig.game, quick_options());
    again.start(conn.ticket());
    REQUIRE(rig.until([&] { again.update(); return again.state() == ConnectionState::CharacterSelect; }));
    again.enter_world("Gennro");
    REQUIRE(rig.until([&] { again.update(); return again.state() == ConnectionState::InZone; }));
}

// ---------------------------------------------------------------- world and zone failures, reconnecting

TEST_CASE("mock server: character list changes and a zone that is not available") {
    MockConfig config = MockConfig::standard();
    config.zone_unavailable = true;
    Rig rig(std::move(config));
    REQUIRE(rig.to_character_select());
    REQUIRE(rig.conn.characters().size() == 1);

    // OP_ZoneUnavail answers OP_EnterWorld: back to character select with the zone named.
    rig.conn.enter_world("Gennro");
    CHECK(rig.conn.state() == ConnectionState::EnteringWorld);
    REQUIRE(rig.until_state(ConnectionState::CharacterSelect));
    CHECK(rig.conn.error() == "That zone is not available (qeynos2).");
    CHECK(rig.server.zone_entries == 0);
    CHECK(rig.server.zone.sessions() == 0); // no zone connection was made

    // OP_DeleteCharacter: world sends a fresh list.
    rig.conn.delete_character("Gennro");
    REQUIRE(rig.until([&] { return rig.conn.characters().empty(); }));
    CHECK(rig.conn.state() == ConnectionState::CharacterSelect);
    CHECK(MockEqServer::count(rig.server.world_log, Op::DeleteCharacter) == 1);
}

TEST_CASE("mock server: Return Home sends OP_EnterWorld with the home flag, a plain enter without it") {
    MockConfig config = MockConfig::standard();
    config.zone_unavailable = true; // each enter comes back to character select
    Rig rig(std::move(config));
    REQUIRE(rig.to_character_select());
    auto last_enter = [&] {
        auto it = std::find_if(rig.server.world_log.rbegin(), rig.server.world_log.rend(),
                               [](const AppPacket& p) { return is_op(p, Op::EnterWorld); });
        REQUIRE(it != rig.server.world_log.rend());
        return *it;
    };

    rig.conn.enter_world("Gennro");
    REQUIRE(rig.until_state(ConnectionState::CharacterSelect));
    CHECK(decode_enter_world(last_enter())->return_home == 0);

    rig.conn.enter_world("Gennro", true);
    CHECK(rig.conn.status() == "Returning home as Gennro");
    REQUIRE(rig.until_state(ConnectionState::CharacterSelect));
    AppPacket p = last_enter();
    auto e = decode_enter_world(p);
    REQUIRE(e);
    CHECK(e->name == "Gennro");
    CHECK(e->tutorial == 0);
    // DoCharacterSelection: byte 0x44 the home flag, byte 0x45 set with it
    CHECK(e->return_home == 0x101);
    CHECK(p.data[0x44] == std::byte{1});
    CHECK(p.data[0x45] == std::byte{1});
}

TEST_CASE("mock server: a wrong session key gets the world connection dropped") {
    Rig rig;
    WorldTicket ticket = rig.server.ticket();
    ticket.session_key = "WRONGKEY00";
    rig.conn.start(ticket);
    REQUIRE(rig.until_state(ConnectionState::Disconnected));
    CHECK(rig.conn.error() == "lost the connection to the world server: the server closed the session");
    CHECK(rig.conn.characters().empty());
    REQUIRE(rig.conn.link_death()); // HandleWorldDisconnect(…, 1): the front end
    CHECK(!rig.conn.link_death()->in_game);
}

TEST_CASE("mock server: losing the zone connection ends the session, and start() connects again") {
    Rig rig;
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());

    // The zone server closes the session.
    server.zone.send_disconnect();
    REQUIRE(rig.until_state(ConnectionState::Disconnected));
    CHECK(conn.error() == "lost the connection to the zone server: the server closed the session");
    REQUIRE(conn.link_death()); // in game, no camp: character select after the disconnected screen
    CHECK(conn.link_death()->in_game);
    CHECK(conn.link_death()->camp_kind == 0);

    // start() with the same ticket runs the whole sequence again on fresh sessions.
    REQUIRE(rig.to_zone());
    CHECK(conn.error().empty());
    CHECK(server.login_infos.size() == 2);
    CHECK(server.zone_entries == 2);
    CHECK(server.world.sessions() == 2);
    CHECK(server.zone.sessions() == 2);
    CHECK(conn.world().spawns.size() == 3); // the zone's spawns, not twice
    CHECK(conn.world().player_id == 7);

    // The zone does not know the session any more (it restarted): OP_OutOfSession.
    server.zone.send_out_of_session();
    REQUIRE(rig.until_state(ConnectionState::Disconnected));
    CHECK(conn.error().find("lost the connection to the zone server") == 0);
    CHECK(conn.error().find("OP_OutOfSession") != std::string::npos);
}

TEST_CASE("mock server: the chat server named by OP_SetChatServer2 is connected once in zone") {
    MockConfig config = MockConfig::standard();
    config.send_chat_server = true;
    Rig rig(std::move(config));
    REQUIRE(rig.to_zone());
    // The UCS connection is its own session on the address in the packet (one-byte opcodes), and its first packet
    // is the mail login. The mock only records what arrives (its protocol is not scripted).
    REQUIRE(rig.until([&] { return !rig.server.ucs_log.empty(); }));
    CHECK(rig.server.ucs.sessions() == 1);
    std::string text;
    for (std::byte b : rig.server.ucs_log.front().data) text.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
    CHECK(text.find("mock.Gennro") != std::string::npos);
    CHECK(text.find("12345678") != std::string::npos);
}

TEST_CASE("chat server: the host is looked up off the frame; the connection is neither up nor failed meanwhile") {
    // ChatServerConnect runs while the player is in the zone: a slow DNS must not stop the frame. connect() returns
    // at once; update() opens the session once the lookup has answered.
    net::ChatServer chat;
    net::ChatServerInfo info;
    info.host = "127.0.0.1";
    info.port = 9; // nothing answers: the session stays Connecting
    info.server_short_name = "mock";
    info.character = "Gennro";
    chat.connect(info);
    CHECK(chat.resolving()); // until an update() has taken the answer, however fast it came
    CHECK_FALSE(chat.failed());
    CHECK_FALSE(chat.connected());
    chat.send_command("join test"); // kept for the login
    auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (chat.resolving() && std::chrono::steady_clock::now() < give_up) {
        chat.update();
        std::this_thread::yield();
    }
    CHECK_FALSE(chat.resolving());
    CHECK_FALSE(chat.failed()); // a session, connecting
    // A lookup given up on is dropped: a new connect, then a disconnect before it answers.
    chat.connect(info);
    chat.disconnect();
    CHECK_FALSE(chat.resolving());
    CHECK(chat.failed());
}

// ---------------------------------------------------------------- lossy, duplicating, reordering links

namespace {

// The texts of the say lines the client sent, in the order the server received them.
std::vector<std::string> said(const std::vector<AppPacket>& log) {
    std::vector<std::string> out;
    for (const auto& p : log)
        if (is_op(p, Op::ChannelMessage))
            if (auto m = decode_channel_message(p)) out.push_back(m->message);
    return out;
}

// Pumps until `n` more chat lines have arrived, appending them to `lines`.
bool chat_lines(Rig& rig, std::vector<std::string>& lines, std::size_t n) {
    std::size_t want = lines.size() + n;
    return rig.until([&] {
        for (auto& e : rig.chat()) lines.push_back(e.text);
        return lines.size() >= want;
    });
}

} // namespace

TEST_CASE("mock server: the zone link loses, duplicates and reorders datagrams in both directions") {
    Rig rig;
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    rig.chat();
    std::vector<std::string> lines;

    // ---- server -> client
    // A duplicated datagram shows its line once. (Reliable delivery is in order, so the marker behind it proves the
    // duplicate has been through the client too.)
    server.zone.script_outgoing({Fate::Duplicate});
    server.chat("Guard_Bob", 8, "dup");
    server.chat("Guard_Bob", 8, "marker");
    REQUIRE(chat_lines(rig, lines, 2));
    CHECK(lines == std::vector<std::string>{"SAY Guard Bob: dup", "SAY Guard Bob: marker"});
    lines.clear();

    // Two datagrams held back and delivered after the third, in reverse: the third is acknowledged out of order,
    // nothing shows until the first arrives, and then all three show in order.
    server.zone.script_outgoing({Fate::Hold, Fate::Hold});
    server.chat("Guard_Bob", 8, "r1");
    server.chat("Guard_Bob", 8, "r2");
    server.chat("Guard_Bob", 8, "r3");
    REQUIRE(rig.until([&] { return server.zone.is_acked(static_cast<std::uint16_t>(server.zone.next_sequence() - 1)); }));
    CHECK(rig.chat().empty());
    server.zone.release_held(true);
    REQUIRE(chat_lines(rig, lines, 3));
    CHECK(lines == std::vector<std::string>{"SAY Guard Bob: r1", "SAY Guard Bob: r2", "SAY Guard Bob: r3"});
    lines.clear();

    // A lost datagram: later ones are acknowledged out of order and wait; the server's resend (EQEmu resends the
    // unacknowledged on any ack) releases all three in order.
    REQUIRE(rig.until([&] { return server.zone.unacked() == 0; }));
    server.zone.script_outgoing({Fate::Drop});
    server.chat("Guard_Bob", 8, "l1");
    server.chat("Guard_Bob", 8, "l2");
    server.chat("Guard_Bob", 8, "l3");
    REQUIRE(rig.until([&] { return server.zone.unacked() == 1; }));
    CHECK(rig.chat().empty());
    CHECK(server.zone.resend_unacked() == 1);
    REQUIRE(chat_lines(rig, lines, 3));
    CHECK(lines == std::vector<std::string>{"SAY Guard Bob: l1", "SAY Guard Bob: l2", "SAY Guard Bob: l3"});
    REQUIRE(rig.until([&] { return server.zone.unacked() == 0; }));
    lines.clear();

    // ---- client -> server
    // The client's datagram is lost; the next one gets through, the server acknowledges it out of order, and the
    // client sends the lost one again (fast resend). Both arrive in order, once each.
    server.zone.script_incoming({Fate::Drop});
    conn.say("c1");
    REQUIRE(rig.until([&] { return server.zone.stats.dropped_in == 1; }));
    conn.say("c2");
    REQUIRE(rig.until([&] { return said(server.zone_log) == std::vector<std::string>{"c1", "c2"}; }));

    // The server's acknowledgement is lost: the client times out, sends the packet again, the server sees a
    // duplicate (acknowledges it again, does not deliver it twice) and the client goes quiet.
    server.zone.drop_next_acks(1);
    int duplicates = server.zone.stats.duplicates_in;
    conn.say("c3");
    REQUIRE(rig.until([&] { return server.zone.stats.duplicates_in == duplicates + 1; }));
    REQUIRE(rig.until([&] {
        return !server.zone_log.empty() && said(server.zone_log) == std::vector<std::string>{"c1", "c2", "c3"};
    }));
    rig.run_for(50ms);
    CHECK(said(server.zone_log) == std::vector<std::string>{"c1", "c2", "c3"});

    CHECK(conn.state() == ConnectionState::InZone);
    CHECK(conn.error().empty());
    CHECK(server.zone.stats.bad_datagrams == 0);
}

// ---------------------------------------------------------------- timeouts

TEST_CASE("mock server: a world server that never answers fails the connection") {
    // ConnectionOptions::session shortens the 10 s connect timeout; nothing listens on the mock side.
    platform::UdpSocket silent;
    std::string error;
    auto local = platform::Endpoint::v4(127, 0, 0, 1, 0);
    REQUIRE(silent.open(platform::Endpoint::Family::V4, error, &local));
    ConnectionOptions options = quick_options();
    options.session.connect_timeout = 250ms;
    EmptyGameDir dir;
    GameDir game(dir.path());
    GameConnection conn(game, options);
    WorldTicket ticket;
    ticket.account_id = 77;
    ticket.session_key = "ABCDEFGHIJ";
    ticket.world_host = "127.0.0.1";
    ticket.world_port = silent.local_endpoint()->port;
    conn.start(ticket);
    CHECK(conn.state() == ConnectionState::WorldConnecting);
    REQUIRE(pump([&] { conn.update(); }, [&] { return conn.state() == ConnectionState::Disconnected; }));
    CHECK(conn.error().find("lost the connection to the world server: no session response from 127.0.0.1:") == 0);
    CHECK(!conn.link_death()); // it never connected
}

TEST_CASE("mock server: a silent zone link dies after the idle timeout, a keepalive keeps it alive") {
    // The client's limit is 180 s without a datagram (the original's); ConnectionOptions::session shortens it. EQEmu
    // sends a keepalive after 9 s of silence (docs/protocol/session.md, Timings). The sessions run on a held clock
    // from the zone-in on, so only the test's advances count.
    ConnectionOptions options = quick_options();
    options.session.idle_timeout = 400ms;
    ManualClock clock;
    options.session.clock = clock.source();
    Rig rig(MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    clock.hold();

    for (int i = 0; i < 8; ++i) {
        clock.advance(80ms);
        auto before = rig.conn.zone_session_stats()->datagrams_received;
        rig.server.zone.send_keepalive();
        REQUIRE(rig.until([&] { return rig.conn.zone_session_stats()->datagrams_received > before; }));
    }
    CHECK(rig.conn.state() == ConnectionState::InZone);

    clock.advance(401ms);
    REQUIRE(rig.until_state(ConnectionState::Disconnected));
    CHECK(rig.conn.error().find("lost the connection to the zone server: connection to 127.0.0.1:") == 0);
    CHECK(rig.conn.error().find("timed out") != std::string::npos);
}

TEST_CASE("mock server: the Options' chat filters go to the zone (OP_SetServerFilter) and to the character's chat.ini") {
    // The character's files under a scratch servers/ folder; the asset folder's eqclient.ini is only read.
    EmptyGameDir servers;
    ConnectionOptions options = quick_options();
    options.profile_id = "test-server";
    options.servers_dir = servers.path();
    Rig rig(MockConfig::standard(), options);
    auto& conn = rig.conn;
    auto& server = rig.server;
    const std::string eqclient = "[Options]\nBadWord=0\n";
    { std::ofstream(rig.dir.path() / "eqclient.ini", std::ios::binary) << eqclient; }
    rig.game.rescan();
    // An earlier build's characters/<name>/ (no world folder): copied to the world's folder on entering the world.
    const std::filesystem::path unsorted = servers.path() / "test-server" / "characters" / "Gennro" / "chat.ini";
    std::filesystem::create_directories(unsorted.parent_path());
    { std::ofstream(unsorted, std::ios::binary) << "[TextColors]\nUser_1_Red=10\n"; }
    REQUIRE(rig.to_zone());
    // Sent once the zone-in completes: [Defaults] ServerFilter, then the 34 settings, the defaults (guild chat shown:
    // 1, at EQEmu's FilterGuildChat 1).
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, Op::SetServerFilter) == 1; }));
    auto filter_packet = [&](int n) {
        int seen = 0;
        for (const auto& p : server.zone_log)
            if (from_wire(p.opcode) == Op::SetServerFilter && ++seen == n) return p;
        return AppPacket{};
    };
    auto setting = [](const AppPacket& p, int f) {
        std::uint32_t v = 0;
        std::memcpy(&v, p.data.data() + (f + 1) * 4, 4);
        return v;
    };
    AppPacket first = filter_packet(1);
    CHECK(first.data.size() == 140);
    std::uint32_t server_filter = 1;
    std::memcpy(&server_filter, first.data.data(), 4);
    CHECK(server_filter == 0); // no [Defaults] ServerFilter
    CHECK(setting(first, 0) == 1);
    CHECK(setting(first, game::kFilterBadWord) == 0);
    // PC spells to "group only" (2): sent again, kept for the next start.
    conn.change_chat_filter(7, 2);
    REQUIRE(rig.until([&] { return MockEqServer::count(server.zone_log, Op::SetServerFilter) == 2; }));
    CHECK(setting(filter_packet(2), 7) == 2);
    CHECK(conn.chat_filters().mode(7) == 4);
    IniFile ini;
    REQUIRE(ini.load(servers.path() / "test-server" / "characters" / "Mock EQEmu" / "Gennro" / "chat.ini"));
    CHECK(ini.get_int("Options", "PCSpells", -1) == 2);
    CHECK(ini.get_int("TextColors", "User_1_Red", -1) == 10); // the copied file's
    IniFile old_ini;
    REQUIRE(old_ini.load(unsorted));
    CHECK_FALSE(old_ini.get("Options", "PCSpells")); // the earlier build's file is left as it was
    std::ifstream in(rig.dir.path() / "eqclient.ini", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == eqclient);
}

TEST_CASE("game clock: elapsed minutes carry through the EQ calendar") {
    ManualClock clock;
    clock.hold();
    ConnectionOptions options;
    options.session.clock = clock.source();
    EmptyGameDir dir;
    GameDir game(dir.path());
    GameConnection conn(game, options);
    CHECK_FALSE(conn.time_of_day());
    CHECK_FALSE(conn.current_time_of_day());
    auto check_time = [&](int hour, int minute, int day, int month, std::uint32_t year) {
        const auto time = conn.current_time_of_day();
        REQUIRE(time);
        CHECK(time->hour == hour);
        CHECK(time->minute == minute);
        CHECK(time->day == day);
        CHECK(time->month == month);
        CHECK(time->year == year);
    };
    conn.receive_zone_packet_for_test(encode(TimeOfDay{14, 58, 27, 12, 3200}));
    check_time(14, 58, 27, 12, 3200);
    clock.advance(2999ms);
    check_time(14, 58, 27, 12, 3200);
    clock.advance(1ms);
    check_time(14, 59, 27, 12, 3200);
    clock.advance(3000ms);
    check_time(15, 0, 27, 12, 3200);
    REQUIRE(conn.time_of_day());
    CHECK(conn.time_of_day()->hour == 14); // the sky's packet snapshot stays unchanged
    CHECK(conn.time_of_day()->minute == 58);

    conn.receive_zone_packet_for_test(encode(TimeOfDay{24, 59, 28, 12, 3200}));
    clock.advance(2999ms);
    check_time(24, 59, 28, 12, 3200);
    clock.advance(1ms);
    check_time(1, 0, 1, 1, 3201);
    // A long gap carries whole years too: a year and one day, hour and minute.
    clock.advance(std::chrono::milliseconds{(12LL * 28 * 24 * 60 + 24 * 60 + 61) * 3000});
    check_time(2, 1, 2, 1, 3202);
}

TEST_CASE("game clock: every server time packet replaces the elapsed-time reference") {
    ManualClock clock;
    clock.hold();
    ConnectionOptions options;
    options.session.clock = clock.source();
    EmptyGameDir dir;
    GameDir game(dir.path());
    GameConnection conn(game, options);
    const auto packet = encode(TimeOfDay{8, 59, 5, 6, 3200});
    conn.receive_zone_packet_for_test(packet);
    clock.advance(2999ms);
    conn.receive_zone_packet_for_test(packet); // even an identical packet starts a new reference
    clock.advance(1ms);
    auto time = conn.current_time_of_day();
    REQUIRE(time);
    CHECK(time->hour == 8);
    CHECK(time->minute == 59);
    clock.advance(2999ms);
    time = conn.current_time_of_day();
    REQUIRE(time);
    CHECK(time->hour == 9);
    CHECK(time->minute == 0);

    conn.receive_zone_packet_for_test(encode(TimeOfDay{4, 10, 2, 3, 3199}));
    time = conn.current_time_of_day();
    REQUIRE(time);
    CHECK(time->hour == 4);
    CHECK(time->minute == 10);
    CHECK(time->day == 2);
    CHECK(time->month == 3);
    CHECK(time->year == 3199);
    clock.advance(3000ms);
    time = conn.current_time_of_day();
    REQUIRE(time);
    CHECK(time->hour == 4);
    CHECK(time->minute == 11);
}

TEST_CASE("mock server: /time reports the advancing game hour and the local time") {
    ManualClock clock;
    ConnectionOptions options = quick_options();
    options.session.clock = clock.source();
    Rig rig(MockConfig::standard(), options);
    {
        std::ofstream strings(rig.dir.path() / "eqstr_us.txt", std::ios::app);
        strings << "6706 GAME %1\n12389 LOCAL %1\n";
    }
    rig.conn.reload_game_files();
    REQUIRE(rig.to_zone());
    clock.hold();
    rig.chat();
    rig.conn.receive_zone_packet_for_test(encode(TimeOfDay{13, 59, 28, 12, 3200}));
    rig.conn.send_command("/time");
    auto lines = rig.chat();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].kind == ChatEvent::Kind::System);
    CHECK(lines[0].text.starts_with("GAME "));
    CHECK(lines[0].text.ends_with("December 28, 3200 - 12 PM"));
    CHECK(lines[1].text.starts_with("LOCAL "));
    CHECK(lines[1].text.size() > std::strlen("LOCAL "));

    clock.advance(3000ms);
    rig.conn.send_command("/time");
    lines = rig.chat();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text.starts_with("GAME "));
    CHECK(lines[0].text.ends_with("December 28, 3200 - 1 PM"));
    CHECK(lines[1].text.starts_with("LOCAL "));
    CHECK(lines[1].text.size() > std::strlen("LOCAL "));
}
