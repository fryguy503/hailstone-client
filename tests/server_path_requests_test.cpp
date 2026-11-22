#include <doctest/doctest.h>
#include <algorithm>
#include <stdexcept>

#include "core/byte_io.h"
#include "game/server_ext.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/movement_packets.h"
#include "plugin/hooks.h"

using namespace hailstone;

namespace {
net::AppPacket route(float destination) {
    return net::encode_find_person_reply({{0, 0, 0}, {destination, 0, 0}});
}
struct FindPolicy final : game::ServerExtension {
    std::uint32_t flags = 0;
    std::uint64_t revision = 1;
    bool throw_reserved = false;
    std::function<void(const net::GameConnection&)> inspect;
    game::MapPolicy map_policy(const net::GameConnection& connection) const override {
        if (inspect) inspect(connection);
        if (throw_reserved && net::GameConnectionTestAccess::find_request_count(connection))
            throw std::runtime_error("fixture policy failed after reservation");
        return {flags, "fixture", revision};
    }
};
}

// The server answers in the order asked (docs/unknowns.md, Navigation): two quiet routes in flight together each take
// their own reply, the first the first. Only the newest quiet route is kept until taken, as before leases.
TEST_CASE("server path leases: overlapping routes each take their own reply, in the order asked") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    const auto first = rig.conn.request_quiet_path(101, {11, 1});
    REQUIRE(first);
    rig.conn.receive_zone_packet_for_test(route(100));
    const auto second = rig.conn.request_quiet_path(102, {11, 2});
    const auto third = rig.conn.request_quiet_path(102, {11, 3});
    REQUIRE(second); REQUIRE(third);
    rig.conn.receive_zone_packet_for_test(route(200));
    auto result = rig.conn.take_quiet_path(second); REQUIRE(result); REQUIRE_FALSE(result->empty());
    CHECK(result->front().x == doctest::Approx(200));
    rig.conn.receive_zone_packet_for_test(route(300));
    CHECK_FALSE(rig.conn.take_quiet_path(second)); // an old handle must not consume the new result
    result = rig.conn.take_quiet_path(third); REQUIRE(result); REQUIRE_FALSE(result->empty());
    CHECK(result->front().x == doctest::Approx(300));
    CHECK_FALSE(rig.conn.take_quiet_path(third));
    const auto old = rig.conn.take_quiet_path(first); // the first reply was replaced by the next request's
    CHECK_FALSE(old);
}

TEST_CASE("server path leases: a guarded Find fallback cannot become a replacement manual route") {
    auto policy = std::make_shared<FindPolicy>();
    auto options = test::quick_options(); options.plugins = policy;
    test::Rig rig(test::MockConfig::standard(), options); REQUIRE(rig.to_zone());
    const auto owned = rig.conn.request_quiet_path(101, {21, 1, game::ServerPathOrigin::Purpose::NpcFind}); REQUIRE(owned);
    policy->flags = game::MapPolicyDisableNpcFind; ++policy->revision;
    const auto manual = rig.conn.request_quiet_path(102, {21, 2}); REQUIRE(manual);
    // The owned route's reply is dropped (the policy now forbids it); the manual route keeps its own.
    rig.conn.receive_zone_packet_for_test(route(111)); CHECK_FALSE(rig.conn.take_quiet_path(owned));
    rig.conn.receive_zone_packet_for_test(route(222));
    const auto kept = rig.conn.take_quiet_path(manual); REQUIRE(kept); REQUIRE_FALSE(kept->empty());
    CHECK(kept->front().x == doctest::Approx(222));
    const auto fresh = rig.conn.request_quiet_path(101, {21, 3}); REQUIRE(fresh); // ordinary navigation remains available
    rig.conn.receive_zone_packet_for_test(route(333)); CHECK(rig.conn.take_quiet_path(fresh).has_value());
}

TEST_CASE("Server-only NPC Find sends an advertised spawn's request and retires its route on removal") {
    auto policy = std::make_shared<FindPolicy>();
    policy->flags = game::MapPolicyServerNpcFindOnly;
    auto options = test::quick_options();
    options.plugins = policy;
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& server = rig.server;

    CHECK_FALSE(conn.find_spawn(101));
    CHECK(net::GameConnectionTestAccess::find_request_count(conn) == 0);
    const auto advertise = [&](bool remove) {
        net::FindableNpc npc;
        npc.spawn_id = 101;
        npc.remove = remove;
        server.zone.send_reliable(net::encode_findable_npc(npc));
        return rig.until([&] { return conn.findable_npcs().contains(101) != remove; });
    };
    REQUIRE(advertise(false));
    REQUIRE(conn.find_spawn(101));
    REQUIRE(rig.until([&] { return test::MockEqServer::count(server.zone_log, net::Op::FindPersonRequest) == 1; }));
    const auto request = std::find_if(server.zone_log.begin(), server.zone_log.end(), [](const auto& p) {
        return p.opcode == net::to_wire(net::Op::FindPersonRequest);
    });
    REQUIRE(request != server.zone_log.end());
    ByteReader reader(request->data);
    CHECK(reader.le<std::uint32_t>() == static_cast<std::uint32_t>(game::FindKind::Spawn));
    CHECK(reader.le<std::uint32_t>() == 101);

    server.zone.send_reliable(route(100));
    REQUIRE(rig.until([&] { return conn.find_replies() == 1; }));
    CHECK_FALSE(conn.find_path().empty());
    REQUIRE(advertise(true));
    CHECK(conn.find_path().empty());
    CHECK_FALSE(conn.find_spawn(101));

    REQUIRE(advertise(false));
    REQUIRE(conn.find_spawn(101));
    REQUIRE(rig.until([&] { return test::MockEqServer::count(server.zone_log, net::Op::FindPersonRequest) == 2; }));
    REQUIRE(advertise(true));
    server.zone.send_reliable(route(200));
    REQUIRE(rig.until([&] { return net::GameConnectionTestAccess::find_request_count(conn) == 0; }));
    CHECK(conn.find_replies() == 1);
    CHECK(conn.find_path().empty());

    REQUIRE(advertise(false));
    server.remove_spawn(101);
    REQUIRE(rig.until([&] { return !conn.world().spawns.contains(101); }));
    server.add_spawn(server.config.zones.at(server.current_zone()).spawns.front());
    REQUIRE(rig.until([&] { return conn.world().spawns.contains(101); }));
    CHECK_FALSE(conn.find_spawn(101));
}

TEST_CASE("server path leases: stops, orphans and an unanswered request; later requests still get their replies") {
    test::Rig rig; REQUIRE(rig.to_zone());
    const auto replies = rig.conn.find_replies();
    rig.conn.receive_zone_packet_for_test(route(123));
    CHECK(rig.conn.find_replies() == replies); CHECK_FALSE(rig.conn.player_path().active());
    rig.conn.find_map_location({100, 0, 0}); rig.conn.stop_find_path();
    rig.conn.receive_zone_packet_for_test(route(100));
    CHECK(rig.conn.find_replies() == replies); CHECK_FALSE(rig.conn.player_path().active());
    const auto request = rig.conn.request_quiet_path(101, {31, 1}); REQUIRE(request);
    // Never answered (EQEmu sends no reply to a /find of a Bazaar trader: it warps the player): after the wait it is
    // dropped, as before, and every later request is answered. A reply that comes late anyway has no owner.
    net::GameConnectionTestAccess::expire_find_batch(rig.conn);
    rig.conn.receive_zone_packet_for_test(route(200)); CHECK_FALSE(rig.conn.take_quiet_path(request));
    const auto later = rig.conn.request_quiet_path(102, {31, 2}); REQUIRE(later);
    rig.conn.receive_zone_packet_for_test(route(250)); CHECK(rig.conn.take_quiet_path(later));
    rig.conn.find_map_location({300, 0, 0}); rig.conn.receive_zone_packet_for_test(route(300));
    CHECK(rig.conn.find_replies() == replies + 1); CHECK(rig.conn.player_path().active());
}

TEST_CASE("server path leases: context actor replacement and source epochs retire outstanding results") {
    auto policy = std::make_shared<FindPolicy>();
    auto options = test::quick_options(); options.plugins = policy;
    test::Rig rig(test::MockConfig::standard(), options); REQUIRE(rig.to_zone());
    auto actor = rig.conn.captureActorRef(101); REQUIRE(actor); CHECK(rig.conn.matches(*actor));
    const auto request = rig.conn.request_quiet_path(101, {41, 1}); REQUIRE(request);
    ++net::GameConnectionTestAccess::world(rig.conn).spawns.at(101).incarnation;
    CHECK_FALSE(rig.conn.matches(*actor));
    rig.conn.receive_zone_packet_for_test(route(100)); CHECK_FALSE(rig.conn.take_quiet_path(request));
    const auto owned = rig.conn.request_quiet_path(101, {41, 2, game::ServerPathOrigin::Purpose::NpcFind}); REQUIRE(owned);
    ++policy->revision; // same flags/name, new provider generation
    rig.conn.receive_zone_packet_for_test(route(200)); CHECK_FALSE(rig.conn.take_quiet_path(owned));
    actor = rig.conn.captureActorRef(101); REQUIRE(actor);
    const auto before = rig.conn.interaction_context();
    auto& world = net::GameConnectionTestAccess::world(rig.conn);
    ++world.spawns.at(world.player_id).incarnation;
    CHECK(rig.conn.interaction_context() != before); CHECK_FALSE(rig.conn.matches(*actor));
}

TEST_CASE("server path leases: dropped outgoing requests do not leave phantom replies") {
    plugin::Hooks hooks;
    auto options = test::quick_options(); options.hooks = &hooks;
    test::Rig rig(test::MockConfig::standard(), options); REQUIRE(rig.to_zone());
    const auto owner = hooks.new_owner("path drop fixture");
    bool drop = true;
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, net::to_wire(net::Op::FindPersonRequest),
        [&](net::AppPacket&) { return drop ? plugin::Flow::Drop : plugin::Flow::Continue; });
    CHECK_FALSE(rig.conn.request_quiet_path(101, {51, 1}));
    drop = false;
    const auto admitted = rig.conn.request_quiet_path(102, {51, 2}); REQUIRE(admitted);
    rig.conn.receive_zone_packet_for_test(route(400)); REQUIRE(rig.conn.take_quiet_path(admitted));
}

TEST_CASE("server path leases: unsent rejected requests preserve an earlier valid reply") {
    for (const int rejection : {0, 1, 2}) {
        auto policy = std::make_shared<FindPolicy>();
        plugin::Hooks hooks;
        auto options = test::quick_options(); options.plugins = policy; options.hooks = &hooks;
        test::Rig rig(test::MockConfig::standard(), options); REQUIRE(rig.to_zone());
        const auto first = rig.conn.request_quiet_path(101, {81, 1}); REQUIRE(first);
        const auto owner = hooks.new_owner("unsent replacement fixture");
        auto live = std::make_shared<bool>(true);
        hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, net::to_wire(net::Op::FindPersonRequest),
            [&](net::AppPacket&) {
                if (rejection == 2) *live = false;
                return rejection == 0 ? plugin::Flow::Drop : plugin::Flow::Continue;
            });
        policy->inspect = [&](const net::GameConnection& connection) {
            if (rejection == 1 && net::GameConnectionTestAccess::find_request_count(connection) > 1)
                throw std::runtime_error("unsent replacement rejected");
        };
        const auto second = [&] { return rig.conn.request_quiet_path(102,
            {81, 2, game::ServerPathOrigin::Purpose::NpcFind, live, true}); };
        if (rejection == 1) CHECK_THROWS_AS(second(), std::runtime_error);
        else CHECK_FALSE(second());
        REQUIRE(net::GameConnectionTestAccess::find_request_count(rig.conn) == 1);
        policy->inspect = {};
        rig.conn.receive_zone_packet_for_test(route(100));
        const auto reply = rig.conn.take_quiet_path(first); REQUIRE(reply); REQUIRE_FALSE(reply->empty());
        CHECK(reply->front().x == doctest::Approx(100));
    }
}

TEST_CASE("server path leases: outgoing callbacks can receive and nest before the reserved request is sent") {
    plugin::Hooks hooks;
    auto options = test::quick_options(); options.hooks = &hooks;
    test::Rig rig(test::MockConfig::standard(), options); REQUIRE(rig.to_zone());
    const auto first = rig.conn.request_quiet_path(101, {82, 1}); REQUIRE(first);
    const auto owner = hooks.new_owner("nested path receipt fixture");
    bool armed = true, first_received = false, nested_received = false;
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, net::to_wire(net::Op::FindPersonRequest),
        [&](net::AppPacket&) {
            if (armed) {
                armed = false;
                rig.conn.receive_zone_packet_for_test(route(100));
                first_received = rig.conn.take_quiet_path(first).has_value();
                const auto nested = rig.conn.request_quiet_path(101, {82, 3}); REQUIRE(nested);
                rig.conn.receive_zone_packet_for_test(route(300));
                nested_received = rig.conn.take_quiet_path(nested).has_value();
            }
            return plugin::Flow::Continue;
        });
    const auto second = rig.conn.request_quiet_path(102, {82, 2}); REQUIRE(second);
    CHECK(first_received); CHECK(nested_received);
    rig.conn.receive_zone_packet_for_test(route(200));
    CHECK(rig.conn.take_quiet_path(second).has_value());
}

TEST_CASE("server path leases: unsent reservations neither time out nor consume synchronous packets") {
    plugin::Hooks hooks;
    auto options = test::quick_options(); options.hooks = &hooks;
    test::Rig rig(test::MockConfig::standard(), options); REQUIRE(rig.to_zone());
    const auto owner = hooks.new_owner("reserved path timeout fixture");
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, net::to_wire(net::Op::FindPersonRequest),
        [&](net::AppPacket&) {
            rig.conn.receive_zone_packet_for_test(route(999)); // no request has reached the transport
            net::GameConnectionTestAccess::expire_find_batch(rig.conn);
            return plugin::Flow::Continue;
        });
    const auto request = rig.conn.request_quiet_path(101, {83, 1}); REQUIRE(request);
    rig.conn.receive_zone_packet_for_test(route(100));
    const auto reply = rig.conn.take_quiet_path(request); REQUIRE(reply); REQUIRE_FALSE(reply->empty());
    CHECK(reply->front().x == doctest::Approx(100));
}

TEST_CASE("server path leases: admission callbacks cannot reuse a removed player or replaced destination") {
    for (const bool remove_player : {false, true}) {
        auto policy = std::make_shared<FindPolicy>();
        net::ConnectionOptions options; options.plugins = policy;
        mq_test::World world("path_admission_actors", options); // no motion sample: request origin reads the player
        bool mutate = true;
        policy->inspect = [&](const net::GameConnection&) {
            if (!mutate) return;
            mutate = false;
            auto& actors = net::GameConnectionTestAccess::world(*world.conn);
            if (remove_player) actors.spawns.erase(actors.player_id);
            else ++actors.spawns.at(10).incarnation;
        };
        CHECK_FALSE(world.conn->request_quiet_path(10, {84, 1, game::ServerPathOrigin::Purpose::NpcFind}));
        CHECK_FALSE(mutate);
        CHECK(net::GameConnectionTestAccess::find_request_count(*world.conn) == 0);
    }
}

TEST_CASE("server path leases: an owner retired inside an outgoing callback cannot send or consume a fallback") {
    plugin::Hooks hooks;
    auto options = test::quick_options(); options.hooks = &hooks;
    test::Rig rig(test::MockConfig::standard(), options); REQUIRE(rig.to_zone());
    const auto owner = hooks.new_owner("path lifetime fixture");
    auto live = std::make_shared<bool>(true);
    bool retire = true;
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, net::to_wire(net::Op::FindPersonRequest),
        [&](net::AppPacket&) { if (retire) *live = false; return plugin::Flow::Continue; });
    CHECK_FALSE(rig.conn.request_quiet_path(101, {61, 1, game::ServerPathOrigin::Purpose::ManualNavigation, live, true}));
    retire = false; live = std::make_shared<bool>(true);
    const auto request = rig.conn.request_quiet_path(102, {61, 2, game::ServerPathOrigin::Purpose::ManualNavigation, live, true});
    REQUIRE(request);
    rig.conn.receive_zone_packet_for_test(route(400));
    live.reset(); // no hook method needs to run for the retired owner to lose its completed result
    CHECK_FALSE(rig.conn.take_quiet_path(request));
}

TEST_CASE("server path leases: exceptions after reservation roll back only the unsent request") {
    auto policy = std::make_shared<FindPolicy>();
    auto options = test::quick_options(); options.plugins = policy;
    test::Rig rig(test::MockConfig::standard(), options); REQUIRE(rig.to_zone());
    policy->throw_reserved = true;
    CHECK_THROWS_AS(rig.conn.request_quiet_path(101, {71, 1, game::ServerPathOrigin::Purpose::NpcFind}), std::runtime_error);
    CHECK(net::GameConnectionTestAccess::find_request_count(rig.conn) == 0);
    policy->throw_reserved = false;
    const auto fresh = rig.conn.request_quiet_path(102, {71, 2}); REQUIRE(fresh);
    rig.conn.receive_zone_packet_for_test(route(500)); CHECK(rig.conn.take_quiet_path(fresh).has_value());
}

TEST_CASE("server path leases: a reentrant policy cannot retire its replacement shown goal") {
    auto policy = std::make_shared<FindPolicy>();
    auto options = test::quick_options(); options.plugins = policy;
    test::Rig rig(test::MockConfig::standard(), options); REQUIRE(rig.to_zone());
    bool replace = true;
    policy->inspect = [&](const net::GameConnection& connection) {
        if (replace && connection.player_path().kind() == game::FindKind::Spawn && connection.player_path().target_id() == 101) {
            replace = false;
            rig.conn.find_map_location({600, 0, 0});
        }
    };
    rig.conn.find_spawn(101);
    CHECK_FALSE(replace); CHECK(rig.conn.player_path().kind() == game::FindKind::MapLocation);
    CHECK(net::GameConnectionTestAccess::find_request_count(rig.conn) == 1);
    rig.conn.receive_zone_packet_for_test(route(600));
    CHECK(rig.conn.player_path().active()); CHECK(rig.conn.find_replies() == 1);
}

TEST_CASE("server path leases: a reply floor callback cannot overwrite a replacement route") {
    test::Rig rig; REQUIRE(rig.to_zone());
    rig.conn.find_map_location({100, 0, 0});
    bool replace = true;
    rig.conn.set_path_floor([&](const glm::vec3&, float) -> std::optional<float> {
        if (replace) { replace = false; rig.conn.find_map_location({700, 0, 0}); }
        return 0.0f;
    });
    rig.conn.receive_zone_packet_for_test(route(100));
    CHECK_FALSE(replace); CHECK(rig.conn.find_replies() == 0); CHECK_FALSE(rig.conn.player_path().active());
    rig.conn.set_path_floor({});
    rig.conn.receive_zone_packet_for_test(route(700));
    CHECK(rig.conn.find_replies() == 1); CHECK(rig.conn.player_path().active());
}
