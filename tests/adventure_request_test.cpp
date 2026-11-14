// An adventure recruiter's Request (net/game_connection_adventure.cpp): stock EQEmu drops one from a player in no
// group or raid unanswered, so the client stops it with its own line; a server extension's solo_adventure switch
// (game/server_ext.h) lets it go out.
#include <doctest/doctest.h>

#include <memory>

#include "core/byte_io.h"
#include "game/coords.h"
#include "game/server_ext.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/packet_io.h"

using namespace hailstone;

namespace {

constexpr const char* kRefusal = "You must be in a group or raid to request an adventure.";

net::AppPacket adventure_text(net::Op op, std::string_view text) {
    ByteWriter w;
    net::detail::write_cstring(w, text);
    return net::make_packet(op, std::move(w));
}

struct Solo final : game::ServerExtension {
    bool on = false;
    bool solo_adventure(const net::GameConnection&) const override { return on; }
};

// The test world with the group gone and a recruiter (class 60) 10 away.
void solo_at_recruiter(mq_test::World& w) {
    using A = net::GameConnectionTestAccess;
    A::group(*w.conn) = {};
    w.add(20, "Mannis_McGuyett000", true, 65, game::server_to_world({0, 10, 0}));
    A::world(*w.conn).spawns[20].class_id = 60;
    REQUIRE(w.conn->open_adventure_recruiter(20));
}

} // namespace

TEST_CASE("adventure: a solo request is stopped unless a server extension takes solo adventures") {
    auto ext = std::make_shared<Solo>();
    net::ConnectionOptions opts;
    opts.plugins = ext;
    mq_test::World w("hailstone_adventure_request_test", opts);
    solo_at_recruiter(w);

    w.conn->adventure_request(2, 1);
    CHECK(w.conn->adventure().npc_text == kRefusal);

    ext->on = true;
    REQUIRE(w.conn->open_adventure_recruiter(20)); // clears the text
    w.conn->adventure_request(2, 1);
    CHECK(w.conn->adventure().npc_text.empty()); // sent (no zone session here), not refused
}

TEST_CASE("adventure: a grouped request goes out without the switch") {
    mq_test::World w("hailstone_adventure_request_test");
    w.add(20, "Mannis_McGuyett000", true, 65, game::server_to_world({0, 10, 0}));
    net::GameConnectionTestAccess::world(*w.conn).spawns[20].class_id = 60;
    REQUIRE(w.conn->open_adventure_recruiter(20)); // the test world's player is in a group
    w.conn->adventure_request(1, 0);
    CHECK(w.conn->adventure().npc_text.empty());
}

TEST_CASE("adventure: status can open without a recruiter and a departed recruiter cannot be used") {
    mq_test::World w("hailstone_adventure_window");
    auto& c = *w.conn;
    c.set_adventure_window_open(true);
    CHECK(c.adventure().window_open);
    CHECK_FALSE(c.adventure_request_ready());
    solo_at_recruiter(w);
    CHECK(c.adventure_recruiter_available());
    c.close_adventure_recruiter();
    CHECK_FALSE(c.adventure().window_open);
    c.set_adventure_window_open(true);
    CHECK_FALSE(c.adventure_recruiter_available());
    REQUIRE(c.open_adventure_recruiter(20));
    auto& recruiter = net::GameConnectionTestAccess::world(c).spawns[20];
    recruiter.position = game::server_to_world({0, 20, 0});
    CHECK(c.adventure_recruiter_available());
    recruiter.position = game::server_to_world({0, 20.01f, 0});
    CHECK_FALSE(c.adventure_recruiter_available());
    c.receive_zone_packet_for_test(adventure_text(net::Op::AdventureInfo, "Greetings."));
    net::GameConnectionTestAccess::world(c).spawns.erase(20);
    c.adventure_frame();
    CHECK(c.adventure().window_open); // Refresh processing must not bypass the native range-check timer.
    net::GameConnectionTestAccess::npc_checks(c).recruiter = net::GameConnection::now_ms() - 501;
    c.update();
    CHECK_FALSE(c.adventure().window_open);
    CHECK(c.adventure().recruiter == 0);
    CHECK_FALSE(c.adventure_request_ready());
}

TEST_CASE("adventure: hiding assigned status stops recruiter checks without abandoning the mission") {
    mq_test::World w("hailstone_adventure_hide_assigned");
    solo_at_recruiter(w);
    auto& c = *w.conn;
    net::AdventureData active;
    active.assigned = true;
    active.text = "Complete the mission.";
    active.time_left = 600;
    c.receive_zone_packet_for_test(net::encode(active));
    c.receive_zone_packet_for_test(adventure_text(net::Op::AdventureInfo, "Recruiter notice."));
    c.set_adventure_window_open(false);
    CHECK(c.adventure().data.assigned);
    CHECK_FALSE(c.adventure().window_open);
    c.set_player_motion({100, 0, 0}, 0, {}, true);
    const auto check = net::GameConnection::now_ms() - 501;
    net::GameConnectionTestAccess::npc_checks(c).recruiter = check;
    c.update();
    CHECK(net::GameConnectionTestAccess::npc_checks(c).recruiter == check);
    CHECK(c.adventure().data.assigned);
}

namespace {

void mock_recruiter(test::Rig& rig) {
    const auto pos = rig.conn.world().player()->position;
    auto npc = test::make_spawn("Adventure_Recruiter", 77, 1, 1, pos.y, pos.x, pos.z);
    npc.class_ = 60;
    rig.server.add_spawn(npc);
    REQUIRE(rig.until([&] { return rig.conn.world().spawns.contains(77); }));
    REQUIRE(rig.conn.open_adventure_recruiter(77));
}

std::vector<net::AppPacket> requests(const test::Rig& rig, net::Op op) {
    std::vector<net::AppPacket> out;
    for (const auto& p : rig.server.zone_log)
        if (p.opcode == net::to_wire(op)) out.push_back(p);
    return out;
}

} // namespace

TEST_CASE("adventure: mock server receives requests and one-byte accept and decline") {
    test::ScratchDir files("adventure_wire_flow");
    auto ext = std::make_shared<Solo>();
    ext->on = true;
    auto options = test::quick_options();
    options.plugins = ext;
    options.servers_dir = files.path() / "servers";
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    mock_recruiter(rig);
    rig.conn.adventure_request(2, 3);
    rig.conn.adventure_request(1, 0); // the server silently drops requests during its five-second guard
    REQUIRE(rig.until([&] { return requests(rig, net::Op::AdventureRequest).size() == 1; }));
    auto sent = requests(rig, net::Op::AdventureRequest);
    ByteReader r(sent.front().data);
    CHECK(r.le<std::uint32_t>() == 2);
    CHECK(r.le<std::uint32_t>() == 77);
    CHECK(r.le<std::uint32_t>() == 3);

    rig.server.zone.send_reliable(adventure_text(net::Op::AdventureDetails, "Locate the scout."));
    REQUIRE(rig.until([&] { return !rig.conn.adventure().offer.empty(); }));
    rig.conn.adventure_answer(true);
    rig.conn.adventure_answer(true); // already answered
    REQUIRE(rig.until([&] { return requests(rig, net::Op::LDoNButton).size() == 1; }));
    CHECK(requests(rig, net::Op::LDoNButton).front().data == std::vector<std::byte>{std::byte{1}});

    rig.server.zone.send_reliable(adventure_text(net::Op::AdventureDetails, "A different scout."));
    REQUIRE(rig.until([&] { return !rig.conn.adventure().offer.empty(); }));
    rig.conn.adventure_answer(false);
    REQUIRE(rig.until([&] { return requests(rig, net::Op::LDoNButton).size() == 2; }));
    CHECK(requests(rig, net::Op::LDoNButton).back().data == std::vector<std::byte>{std::byte{0}});

    rig.server.zone.send_reliable(adventure_text(net::Op::AdventureDetails, "An offer to close."));
    REQUIRE(rig.until([&] { return !rig.conn.adventure().offer.empty(); }));
    rig.conn.set_adventure_window_open(false);
    REQUIRE(rig.until([&] { return requests(rig, net::Op::LDoNButton).size() == 3; }));
    CHECK(requests(rig, net::Op::LDoNButton).back().data == std::vector<std::byte>{std::byte{0}});
    CHECK(rig.conn.adventure().offer.empty());
    CHECK(rig.conn.adventure().recruiter == 0);
    CHECK_FALSE(rig.conn.adventure().window_open);

    net::AdventureData active;
    active.assigned = true;
    active.text = "Complete the adventure.";
    active.time_left = 900;
    rig.server.zone.send_reliable(net::encode(active));
    REQUIRE(rig.until([&] { return rig.conn.adventure().data.assigned; }));
    CHECK_FALSE(rig.conn.adventure_request_ready());
    rig.conn.adventure_leave();
    REQUIRE(rig.until([&] { return requests(rig, net::Op::LeaveAdventure).size() == 1; }));
    CHECK(requests(rig, net::Op::LeaveAdventure).front().data.empty());
    rig.server.zone.send_reliable(net::encode(net::AdventureData{}));
    REQUIRE(rig.until([&] { return !rig.conn.adventure().data.assigned; }));
    CHECK(rig.conn.adventure().count.total == 0);
}

TEST_CASE("adventure: stats and leaderboard use native cooldowns and retain the latest queued category") {
    using namespace std::chrono_literals;
    test::ScratchDir files("adventure_refresh");
    test::ManualClock clock;
    auto options = test::quick_options();
    options.session.clock = clock.source();
    options.servers_dir = files.path() / "servers";
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    mock_recruiter(rig);
    clock.hold();
    rig.conn.set_adventure_stats_open(true);
    rig.conn.set_adventure_leaderboard_open(true);
    REQUIRE(rig.until([&] {
        return requests(rig, net::Op::AdventureStatsRequest).size() == 1 &&
               requests(rig, net::Op::AdventureLeaderboardRequest).size() == 1;
    }));
    CHECK_FALSE(rig.conn.adventure_stats_ready());
    CHECK_FALSE(rig.conn.adventure_leaderboard_ready());
    rig.conn.adventure_stats_request();
    rig.conn.adventure_leaderboard_request(1, 1, 0);
    rig.conn.adventure_leaderboard_request(0, 5, 2); // latest selection replaces the intermediate one
    CHECK(rig.conn.adventure().leaderboard_queued);
    clock.advance(9999ms);
    rig.conn.adventure_frame();
    CHECK_FALSE(rig.conn.adventure_stats_ready());
    clock.advance(1ms);
    CHECK(rig.conn.adventure_stats_ready());
    rig.conn.adventure_stats_request();
    REQUIRE(rig.until([&] { return requests(rig, net::Op::AdventureStatsRequest).size() == 2; }));
    CHECK(requests(rig, net::Op::AdventureLeaderboardRequest).size() == 1); // still awaiting the old reply

    net::AdventurePoints points;
    points.available = 123;
    rig.server.zone.send_reliable(net::encode(points));
    REQUIRE(rig.until([&] { return rig.conn.adventure().points.available == 123; }));
    CHECK(rig.conn.adventure().stats_queued);
    clock.advance(9999ms);
    rig.step();
    CHECK(requests(rig, net::Op::AdventureStatsRequest).size() == 2);
    clock.advance(1ms);
    REQUIRE(rig.until([&] { return requests(rig, net::Op::AdventureStatsRequest).size() == 3; }));
    CHECK_FALSE(rig.conn.adventure().stats_queued);

    net::AdventureLeaderboard old;
    old.rows = {{"Old category", 1, 1}};
    rig.server.zone.send_reliable(net::encode(old));
    REQUIRE(rig.until([&] { return requests(rig, net::Op::AdventureLeaderboardRequest).size() == 2; }));
    CHECK_FALSE(rig.conn.adventure().leaderboard); // old category cannot appear under the new selection
    const auto sent = requests(rig, net::Op::AdventureLeaderboardRequest);
    ByteReader query(sent.back().data);
    CHECK(query.le<std::uint32_t>() == 0);
    CHECK(query.le<std::uint32_t>() == 5);
    CHECK(query.le<std::uint32_t>() == 2);
    net::AdventureLeaderboard current;
    current.rows = {{"Selected category", 7, 2}};
    rig.server.zone.send_reliable(net::encode(current));
    REQUIRE(rig.until([&] { return rig.conn.adventure().leaderboard.has_value(); }));
    CHECK(rig.conn.adventure().leaderboard->rows.front().name == "Selected category");
    rig.conn.set_adventure_leaderboard_open(false);
    clock.advance(10000ms);
    rig.conn.set_adventure_leaderboard_open(true);
    REQUIRE(rig.until([&] { return requests(rig, net::Op::AdventureLeaderboardRequest).size() == 3; }));
    CHECK(rig.conn.adventure().leaderboard_inflight);
    CHECK(rig.conn.adventure().leaderboard->rows.front().name == "Selected category"); // cache stays until replaced

    net::AdventureData data;
    data.assigned = true;
    data.text = "Timed objective";
    data.time_left = 600;
    rig.server.zone.send_reliable(net::encode(data));
    REQUIRE(rig.until([&] { return rig.conn.adventure().data.assigned; }));
    clock.advance(1500ms);
    CHECK(rig.conn.adventure_elapsed_seconds() == 1);
    rig.conn.reset_adventure_for_zone();
    CHECK(rig.conn.adventure().recruiter == 0);
    CHECK_FALSE(rig.conn.adventure().leaderboard_inflight);
    CHECK_FALSE(rig.conn.adventure().data.assigned);
    CHECK(rig.conn.adventure().stats_open);
    CHECK(rig.conn.adventure().leaderboard_open);
}

TEST_CASE("adventure: a missing or malformed leaderboard reply recovers after thirty seconds") {
    using namespace std::chrono_literals;
    test::ManualClock clock;
    net::ConnectionOptions options;
    options.session.clock = clock.source();
    mq_test::World w("adventure_reply_timeout", options);
    clock.hold();
    auto& c = *w.conn;
    c.set_adventure_leaderboard_open(true);
    REQUIRE(c.adventure().leaderboard_inflight);
    c.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::AdventureLeaderboardReply),
                                               std::vector<std::byte>(7219)));
    CHECK(c.adventure().leaderboard_inflight);
    clock.advance(29999ms);
    c.adventure_frame();
    CHECK_FALSE(c.adventure_leaderboard_ready());
    clock.advance(1ms);
    c.adventure_frame();
    CHECK(c.adventure_leaderboard_ready());
    CHECK(c.adventure().leaderboard_failed);
    c.adventure_leaderboard_request(1, 3, 1);
    CHECK(c.adventure().leaderboard_inflight);
    CHECK_FALSE(c.adventure().leaderboard_failed);
    c.adventure_leaderboard_request(0, 5, 2);
    clock.advance(30000ms);
    c.adventure_frame();
    REQUIRE(c.adventure().leaderboard_inflight);
    CHECK(c.adventure().leaderboard_inflight->theme == 5);
    CHECK_FALSE(c.adventure().leaderboard_queued);
}

TEST_CASE("adventure: win and failure finish packets clear the mission without another data packet") {
    mq_test::World w("adventure_finish");
    auto& c = *w.conn;
    net::AdventurePoints points;
    points.available = 100;
    c.receive_zone_packet_for_test(net::encode(points));
    net::AdventureStats stats;
    stats.success[0] = 10;
    c.receive_zone_packet_for_test(net::encode(stats));
    for (bool won : {true, false}) {
        net::AdventureData active;
        active.assigned = true;
        active.text = "A finishing adventure.";
        active.time_left = 90;
        active.time_to_enter = 30;
        active.compass = true;
        c.receive_zone_packet_for_test(net::encode(active));
        c.set_adventure_window_open(true);
        ByteWriter count;
        count.le<std::uint32_t>(5);
        count.le<std::uint32_t>(5);
        const auto update = net::make_packet(net::Op::AdventureUpdate, std::move(count));
        c.receive_zone_packet_for_test(update);
        CHECK(c.adventure().count.total == 0); // no objective progress before entering
        active.time_to_enter = 0;
        c.receive_zone_packet_for_test(net::encode(active));
        c.receive_zone_packet_for_test(update);
        CHECK(c.adventure().count.total == 5);
        REQUIRE(c.adventure().data.assigned);
        ByteWriter finish;
        finish.le<std::uint32_t>(won ? 1 : 0);
        finish.le<std::uint32_t>(won ? 50 : 0);
        c.receive_zone_packet_for_test(net::make_packet(net::Op::AdventureFinish, std::move(finish)));
        CHECK_FALSE(c.adventure().data.assigned);
        CHECK_FALSE(c.adventure().data.compass);
        CHECK_FALSE(c.adventure().window_open);
        CHECK(c.adventure().count.total == 0);
        c.receive_zone_packet_for_test(update); // a late progress packet cannot revive the completed mission
        CHECK(c.adventure().count.total == 0);
        CHECK(c.adventure().recruiter == 0);
        CHECK(c.adventure().points.available == 100);
        REQUIRE(c.adventure().stats);
        CHECK(c.adventure().stats->success[0] == 10);
    }
}
