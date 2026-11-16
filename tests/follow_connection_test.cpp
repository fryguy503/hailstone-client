#include <doctest/doctest.h>

#include <functional>
#include <utility>

#include "mq_test_world.h"

using namespace hailstone;
namespace {
struct ProfileExtension final : game::ServerExtension {
    bool enabled = true;
    mutable std::function<void()> query;
    std::optional<game::FollowProfile> follow_profile(const net::GameConnection&) const override {
        if (auto call = std::exchange(query, {})) call();
        if (!enabled) return {};
        return game::FollowProfile{15, 15, 3, 150, 8, 12, 20, 64, 96, 15, .1f, .75f, 64};
    }
};
struct FollowWorld {
    std::shared_ptr<ProfileExtension> extension = std::make_shared<ProfileExtension>();
    mq_test::World world;
    FollowWorld() : world("follow_profile_connection", options()) {
        auto& w = net::GameConnectionTestAccess::world(*world.conn);
        for (auto& [id, spawn] : w.spawns) spawn.incarnation = 1000u + id;
        w.spawns.at(1).incarnation = 100; w.spawns.at(12).incarnation = 200; w.target_id = 12;
        net::GameConnectionTestAccess::status(*world.conn).stand_state = net::kStandStanding;
    }
    net::ConnectionOptions options() { net::ConnectionOptions result; result.plugins = extension; return result; }
};
}

TEST_CASE("follow connection: settings alone cannot start movement and manual toggle stop are explicit") {
    FollowWorld f; auto& c = *f.world.conn;
    CHECK(c.follow_target() == 0); CHECK_FALSE(c.enhanced_follow_active());
    REQUIRE(c.manual_follow("start")); CHECK(c.follow_target() == 12); CHECK(c.enhanced_follow_active());
    REQUIRE(c.active_follow_profile());
    CHECK(c.manual_follow("start")); // idempotent, never silently retargets
    CHECK(c.manual_follow("toggle")); CHECK(c.follow_target() == 0);
    CHECK_FALSE(c.manual_follow("unknown"));
}

TEST_CASE("follow connection: enhanced admission rejects NPC self corpse range and unknown actor identities") {
    FollowWorld f; auto& c = *f.world.conn; auto& w = net::GameConnectionTestAccess::world(c);
    for (auto target : {1u, 10u, 13u, 999u}) { w.target_id = target; CHECK_FALSE(c.manual_follow("start")); }
    w.target_id = 12; w.spawns.at(12).position.x = 151; w.spawns.at(12).position.y = 0;
    CHECK_FALSE(c.manual_follow("start"));
    w.spawns.at(12).position.x = 150; CHECK(c.manual_follow("start")); c.stop_follow(false);
    w.spawns.at(12).incarnation = 0; CHECK_FALSE(c.manual_follow("start"));
}

TEST_CASE("follow connection: owner target replacement death and profile revocation retire the link") {
    for (int retirement = 0; retirement != 5; ++retirement) {
        FollowWorld f; auto& c = *f.world.conn; auto& w = net::GameConnectionTestAccess::world(c);
        REQUIRE(c.manual_follow("start"));
        CHECK(c.enhanced_follow_current());
        if (retirement == 0) ++w.spawns.at(1).incarnation;
        if (retirement == 1) ++w.spawns.at(12).incarnation;
        if (retirement == 2) net::GameConnectionTestAccess::status(c).dead = true;
        if (retirement == 3) f.extension->enabled = false;
        if (retirement == 4) net::GameConnectionTestAccess::state(c) = net::ConnectionState::CharacterSelect;
        if (retirement != 3) CHECK_FALSE(c.enhanced_follow_current());
        CHECK_FALSE(c.active_follow_profile()); CHECK(c.follow_target() == 0); CHECK_FALSE(c.enhanced_follow_active());
    }
}

TEST_CASE("follow connection: reentrant profile query cannot start or revive an obsolete actor or session") {
    FollowWorld f; auto& c = *f.world.conn; auto& w = net::GameConnectionTestAccess::world(c);
    f.extension->query = [&] { ++w.spawns.at(12).incarnation; };
    CHECK_FALSE(c.manual_follow("start")); CHECK(c.follow_target() == 0);
    REQUIRE(c.manual_follow("start"));
    f.extension->query = [&] { c.stop_follow(false); };
    CHECK_FALSE(c.active_follow_profile()); CHECK(c.follow_target() == 0);
    f.extension->query = [&] { net::GameConnectionTestAccess::state(c) = net::ConnectionState::CharacterSelect; };
    CHECK_FALSE(c.manual_follow("start")); CHECK(c.follow_target() == 0);
}
