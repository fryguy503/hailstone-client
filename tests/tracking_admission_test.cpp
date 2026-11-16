#include <doctest/doctest.h>

#include <lua.hpp>
#include <stdexcept>
#include <functional>
#include <algorithm>
#include <vector>
#include "plugin/hooks.h"

#include "app/lua_ui.h"
#include "app/lua_ui_api.h"
#include "core/byte_io.h"
#include "game/server_ext.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/packet_io.h"

using namespace hailstone;

namespace {
struct TrackingPolicy final : game::ServerExtension {
    bool allowed = true, skill_allowed = true, throw_reserved = false, throw_admission = false;
    std::uint64_t revision = 1, lifecycle = 1;
    std::uint32_t map_flags = 0;
    game::SkillRules rules;
    std::function<void()> inspect;
    TrackingPolicy() {
        rules.ctx = this;
        rules.tracking_allowed = [](const void* context, std::uint8_t, std::uint32_t, std::uint32_t) {
            const auto& policy = *static_cast<const TrackingPolicy*>(context);
            if (policy.throw_admission) throw std::runtime_error("fixture tracking admission failed");
            const auto observer = policy.inspect;
            if (observer) observer();
            return policy.allowed;
        };
    }
    const game::SkillRules* skill_rules(const net::GameConnection&) const override { return &rules; }
    std::uint64_t lifecycle_revision(const net::GameConnection& connection) const override {
        if (throw_reserved && net::GameConnectionTestAccess::has_tracking_request(connection))
            throw std::runtime_error("fixture tracking policy failed after reservation");
        return lifecycle;
    }
    game::MapPolicy map_policy(const net::GameConnection&) const override { return {map_flags, "fixture", revision}; }
    std::optional<bool> before_use_skill(net::GameConnection&, std::uint32_t) override { return skill_allowed; }
};
net::AppPacket list_reply(bool two = false) {
    ByteWriter bytes;
    bytes.le<std::uint16_t>(two ? 2 : 1);
    const auto row = [&](std::uint32_t id) {
        bytes.le(id); bytes.f32(20); bytes.le<std::uint8_t>(10); bytes.le<std::uint8_t>(1);
        net::detail::write_cstring(bytes, id == 101 ? "Tracking fixture" : "Replacement fixture");
        bytes.le<std::uint8_t>(0); bytes.le<std::uint8_t>(0);
    };
    row(101); if (two) row(102);
    return net::make_packet(net::Op::Track, std::move(bytes));
}
struct TrackingRig {
    std::shared_ptr<TrackingPolicy> policy = std::make_shared<TrackingPolicy>();
    plugin::Hooks hooks;
    test::Rig rig;
    static net::ConnectionOptions options(const std::shared_ptr<TrackingPolicy>& policy_source, plugin::Hooks* packet_hooks) {
        auto value = test::quick_options(); value.plugins = policy_source; value.hooks = packet_hooks; return value;
    }
    TrackingRig() : rig(test::MockConfig::standard(), options(policy, &hooks)) { REQUIRE(rig.to_zone()); }
    void publish() { rig.conn.receive_zone_packet_for_test(list_reply()); }
};
}

TEST_CASE("Tracking admission: window skill refresh and target share the same policy") {
    TrackingRig test;
    auto& connection = test.rig.conn;
    test.policy->allowed = false; ++test.policy->lifecycle;
    CHECK_FALSE(connection.tracking_admitted());
    app::UiHost host; host.in_game = host.ui_ready = true; host.conn = &connection;
    app::UiApi api(host);
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close); REQUIRE(state);
    luaL_openlibs(state.get()); lua_newtable(state.get()); api.register_functions(state.get()); lua_setglobal(state.get(), "hs");
    REQUIRE(luaL_dostring(state.get(), "hs.UI.windows.Show('TrackingWnd',true)") == LUA_OK);
    connection.send_track(); test.publish(); connection.track_spawn(101);
    CHECK_FALSE(connection.tracking().open); CHECK(connection.tracking().tracked == 0); CHECK(connection.tracking_lines().empty());
    test.policy->allowed = true; ++test.policy->lifecycle;
    test.policy->map_flags = game::MapPolicyDisableNpcFind; // Tracking is a separate skill/data service
    connection.open_tracking(); test.publish(); REQUIRE(connection.tracking_lines().size() == 1);
    test.policy->map_flags |= game::MapPolicyHideStockNpcs; ++test.policy->revision;
    CHECK(connection.tracking_lines().size() == 1); // unrelated map content cannot retire Tracking
    connection.track_spawn(101, connection.tracking().revision);
    CHECK(connection.tracking().tracked == 101);
    connection.close_tracking();
    CHECK(connection.tracking().tracked == 101); CHECK(connection.tracking_lines().size() == 1);
}

TEST_CASE("Tracking admission: stock eligibility uses real skill caps rather than effect-only empty tables") {
    mq_test::World world("tracking_stock_caps");
    auto& connection = *world.conn;
    CHECK_FALSE(connection.tracking_admitted());
    net::GameConnectionTestAccess::tracking_skill_fixture(connection);
    REQUIRE(connection.tracking_admitted());
    auto& profile = *net::GameConnectionTestAccess::profile(connection);
    profile.class_ = 1; CHECK_FALSE(connection.tracking_admitted()); // this fixture supplies no Warrior skill cap
    profile.class_ = 4; profile.skills[53] = 0; CHECK(connection.tracking_admitted()); // MinLevel 1 grants adjusted rank 1
    profile.skills[53] = 200; CHECK(connection.tracking_admitted());
}

TEST_CASE("Tracking admission: old source replies never reopen or lend a list to a replacement source") {
    TrackingRig test;
    auto& connection = test.rig.conn;
    connection.open_tracking(); REQUIRE(connection.tracking().open);
    test.policy->allowed = false; ++test.policy->lifecycle;
    test.policy->allowed = true; ++test.policy->lifecycle; // revoke/regrant without an intervening paint
    test.publish();
    CHECK_FALSE(connection.tracking().open); CHECK(connection.tracking_lines().empty());
    connection.open_tracking(); test.publish(); REQUIRE(connection.tracking_lines().size() == 1);
    const auto revision = connection.tracking().revision;
    ++test.policy->lifecycle;
    connection.track_spawn(101, revision);
    CHECK(connection.tracking().tracked == 0); CHECK(connection.tracking_lines().empty());
}

TEST_CASE("Tracking admission: malformed packets retain the request while empty lists and timers complete it") {
    TrackingRig test;
    auto& connection = test.rig.conn;
    connection.open_tracking();
    REQUIRE(net::GameConnectionTestAccess::has_tracking_request(connection));
    for (const auto& packet : {net::make_empty(net::Op::Track), list_reply()}) {
        auto broken = packet;
        if (!broken.data.empty()) broken.data.pop_back();
        connection.receive_zone_packet_for_test(broken);
        CHECK(net::GameConnectionTestAccess::has_tracking_request(connection));
        CHECK(connection.tracking_lines().empty());
        connection.send_track(); // it cannot overlap the still-pending real reply
    }
    test.publish();
    CHECK_FALSE(net::GameConnectionTestAccess::has_tracking_request(connection));
    REQUIRE(connection.tracking_lines().size() == 1);
    connection.send_track();
    ByteWriter empty; empty.le<std::uint16_t>(0);
    connection.receive_zone_packet_for_test(net::make_packet(net::Op::Track, std::move(empty)));
    CHECK_FALSE(net::GameConnectionTestAccess::has_tracking_request(connection));
    CHECK(connection.tracking_lines().empty());
    connection.send_track();
    ByteWriter timer; timer.le(game::kTrackReplyTimer);
    connection.receive_zone_packet_for_test(net::make_packet(net::Op::Track, std::move(timer)));
    CHECK_FALSE(net::GameConnectionTestAccess::has_tracking_request(connection));
    connection.send_track(); test.publish();
    CHECK(connection.tracking_lines().size() == 1);
}

TEST_CASE("Tracking admission: closes orphans timeout and actor reuse cannot dispatch stale rows") {
    TrackingRig test;
    auto& connection = test.rig.conn;
    test.publish(); CHECK(connection.tracking_lines().empty()); CHECK_FALSE(connection.tracking().open);
    connection.open_tracking(); connection.close_tracking(); test.publish();
    CHECK_FALSE(connection.tracking().open); CHECK(connection.tracking_lines().empty());
    connection.open_tracking(); test.publish(); REQUIRE(connection.tracking_lines().size() == 1);
    const auto revision = connection.tracking().revision;
    ++net::GameConnectionTestAccess::world(connection).spawns.at(101).incarnation;
    connection.track_spawn(101, revision); CHECK(connection.tracking().tracked == 0);
    connection.send_track(); net::GameConnectionTestAccess::expire_tracking_request(connection);
    test.publish(); CHECK(connection.tracking_lines().empty()); CHECK_FALSE(connection.tracking().open);
    connection.open_tracking(); test.publish(); CHECK(connection.tracking_lines().empty());
}

TEST_CASE("Tracking admission: throwing providers fail closed locally and recover without phantom requests") {
    TrackingRig test;
    test.policy->throw_admission = true;
    CHECK_FALSE(test.rig.conn.tracking_admitted());
    app::UiHost host; host.in_game = host.ui_ready = true; host.conn = &test.rig.conn;
    app::UiApi api(host);
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close); REQUIRE(state);
    luaL_openlibs(state.get()); lua_newtable(state.get()); api.register_functions(state.get()); lua_setglobal(state.get(), "hs");
    CHECK(luaL_dostring(state.get(), "hs.UI.windows.Show('TrackingWnd',true)") == LUA_OK);
    CHECK_FALSE(test.rig.conn.tracking().open);
    test.policy->throw_admission = false;
    test.policy->throw_reserved = true;
    CHECK_NOTHROW(test.rig.conn.open_tracking());
    CHECK_FALSE(net::GameConnectionTestAccess::has_tracking_request(test.rig.conn));
    CHECK_FALSE(test.rig.conn.tracking().open); CHECK(test.rig.conn.tracking_lines().empty());
    test.policy->throw_reserved = false;
    test.rig.conn.open_tracking(); test.publish(); CHECK(test.rig.conn.tracking_lines().size() == 1);
}

TEST_CASE("Tracking admission: a predicate changing the list during the send guard cannot send a stale target") {
    TrackingRig test;
    auto& connection = test.rig.conn;
    connection.open_tracking(); test.publish();
    const auto revision = connection.tracking().revision;
    bool armed = false, mutate = true;
    test.policy->inspect = [&] { if (armed) { armed = false; connection.set_track_sort(1); } };
    const auto owner = test.hooks.new_owner("tracking predicate fixture");
    test.hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, net::to_wire(net::Op::TrackTarget),
        [&](net::AppPacket&) { if (mutate) armed = true; return plugin::Flow::Continue; });
    connection.track_spawn(101, revision);
    CHECK(connection.tracking().revision != revision); CHECK(connection.tracking().tracked == 0);
    connection.send_zone_packet(net::AppPacket(0x7f08));
    REQUIRE(test.rig.until([&] {
        return std::any_of(test.rig.server.zone_log.begin(), test.rig.server.zone_log.end(),
            [](const auto& packet) { return packet.opcode == 0x7f08; });
    }));
    CHECK(std::none_of(test.rig.server.zone_log.begin(), test.rig.server.zone_log.end(),
        [](const auto& packet) { return packet.opcode == net::to_wire(net::Op::TrackTarget); }));
    mutate = false;
    connection.track_spawn(101, connection.tracking().revision);
    CHECK(connection.tracking().tracked == 101);
}

TEST_CASE("Tracking admission: an outgoing replacement target wins without an older packet or local commit") {
    TrackingRig test;
    auto& connection = test.rig.conn;
    connection.open_tracking(); connection.receive_zone_packet_for_test(list_reply(true));
    REQUIRE(connection.tracking_lines().size() == 2);
    const auto owner = test.hooks.new_owner("tracking replacement fixture");
    bool replaced = false;
    test.hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, net::to_wire(net::Op::TrackTarget),
        [&](net::AppPacket& packet) {
            ByteReader reader(packet.data);
            if (reader.le<std::uint32_t>() == 101 && !replaced) {
                replaced = true; connection.track_spawn(102, connection.tracking().revision);
            }
            return plugin::Flow::Continue;
        });
    connection.track_spawn(101, connection.tracking().revision);
    CHECK(replaced); CHECK(connection.tracking().tracked == 102);
    connection.send_zone_packet(net::AppPacket(0x7f09));
    REQUIRE(test.rig.until([&] {
        return std::any_of(test.rig.server.zone_log.begin(), test.rig.server.zone_log.end(),
            [](const auto& packet) { return packet.opcode == 0x7f09; });
    }));
    std::vector<std::uint32_t> targets;
    for (const auto& packet : test.rig.server.zone_log) if (packet.opcode == net::to_wire(net::Op::TrackTarget)) {
        ByteReader reader(packet.data); targets.push_back(reader.le<std::uint32_t>());
    }
    CHECK(targets == std::vector<std::uint32_t>{102});
}
