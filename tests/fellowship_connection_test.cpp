#include <doctest/doctest.h>

#include <algorithm>

#include "fellowship_test_packets.h"
#include "manual_clock.h"
#include "mock_server/harness.h"
#include "mock_server/profile_builder.h"
#include "net/social_packets.h"
#include "plugin/hooks.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;
namespace fellowship = hailstone::test::fellowship_fixture;

namespace {
std::uint32_t field(const AppPacket& p, std::size_t offset = 0) {
    ByteReader r(p.data); r.skip(offset); return r.le<std::uint32_t>();
}
std::size_t requests(const Rig& rig, std::uint32_t action) {
    return static_cast<std::size_t>(std::count_if(rig.server.zone_log.begin(), rig.server.zone_log.end(),
        [&](const AppPacket& p) { return p.opcode == kOpFellowshipRequest && field(p) == action; }));
}
void barrier(Rig& rig) {
    const auto before = rig.server.zone_log.size();
    rig.conn.send_zone_packet(AppPacket(0x7f01));
    REQUIRE(rig.until([&] {
        return rig.server.zone_log.size() > before && rig.server.zone_log.back().opcode == 0x7f01;
    }));
}
} // namespace

TEST_CASE("fellowship connection: roster authority, vitality, kits and all native requests") {
    Rig rig;
    REQUIRE(rig.to_zone());
    rig.conn.fellowship_create();
    REQUIRE(rig.until([&] { return requests(rig, 1) == 1; }));
    CHECK_FALSE(rig.conn.fellowship().active());
    rig.conn.receive_zone_packet_for_test(fellowship::roster());
    REQUIRE(rig.conn.fellowship().active());
    CHECK(rig.conn.fellowship().is_leader("Gennro"));
    rig.conn.receive_zone_packet_for_test(fellowship::vitality());
    rig.conn.receive_zone_packet_for_test(fellowship::kits());
    rig.conn.receive_zone_packet_for_test(fellowship::roster(64, 64));
    REQUIRE(rig.conn.fellowship().members.size() == 64);
    CHECK(rig.conn.fellowship().vitality.normal == 900);
    CHECK(rig.conn.profile()->fellowship_vitality_normal == 900);
    CHECK(rig.conn.fellowship().vitality.aa_cap == 600);
    REQUIRE(rig.conn.fellowship().kits.size() == 1);
    CHECK(rig.conn.fellowship().camp.active);
    rig.conn.fellowship_set_motd("New meeting");
    auto& world = rig.conn.world_for_test();
    game::Spawn invitee; invitee.id = 333; invitee.name = "Invitee";
    world.spawns[333] = invitee; world.target_id = 333;
    rig.conn.fellowship_invite_target();
    rig.conn.fellowship_remove(9877);
    rig.conn.fellowship_promote(9877);
    rig.conn.fellowship_toggle_sharing(9877);
    rig.conn.fellowship_refresh_kits(true, 1u << 2);
    rig.conn.fellowship_create_camp(17);
    rig.conn.fellowship_destroy_camp();
    rig.conn.fellowship_leave();
    rig.conn.fellowship_end();
    barrier(rig);
    for (const auto action : {1u, 2u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u}) CHECK(requests(rig, action) == 1);
    for (const auto& p : rig.server.zone_log) {
        if (p.opcode == kOpFellowshipRequest) {
            CHECK(p.data.size() == 1076);
            if (field(p) == 5) CHECK(field(p, 4) == 333);
            if (field(p) == 6 || field(p) == 7 || field(p) == 12) CHECK(field(p, 4) == 9877);
            if (field(p) == 8) CHECK(field(p, 0x428) == 17);
        }
        if (p.opcode == kOpCampsiteKitRequest) {
            CHECK(field(p) == 1); CHECK(field(p, 4) == (1u << 2));
        }
    }
    CHECK(rig.conn.fellowship().motd == "Meet at the camp");
    CHECK(rig.conn.fellowship().member(9877)->sharing);
    CHECK(rig.conn.fellowship().members.size() == 64); // sends never grant/remove members locally
    rig.conn.receive_zone_packet_for_test(fellowship::roster(0, 12, 99)); // unrelated stale leave
    CHECK(rig.conn.fellowship().members.size() == 64);
    rig.conn.receive_zone_packet_for_test(fellowship::roster(0, 12, 0)); // a zero id does not identify our fellowship
    CHECK(rig.conn.fellowship().members.size() == 64);
    auto bad = fellowship::roster(); bad.data.pop_back();
    rig.conn.receive_zone_packet_for_test(bad);
    CHECK(rig.conn.fellowship().members.size() == 64);
    rig.conn.receive_zone_packet_for_test(fellowship::roster(0)); // server retains old id on removal
    CHECK_FALSE(rig.conn.fellowship().active());
    CHECK(rig.conn.fellowship().id == 0);
    CHECK(rig.conn.fellowship().kits.empty());
}

TEST_CASE("fellowship connection: invite accept, local decline, replacement and timeout") {
    ManualClock clock;
    auto options = quick_options(); options.session.clock = clock.source();
    Rig rig(MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    game::Spawn leader; leader.id = 333; leader.name = "Leader";
    rig.conn.world_for_test().spawns[333] = leader;
    rig.conn.receive_zone_packet_for_test(fellowship::invite());
    REQUIRE(rig.conn.confirmation());
    CHECK(rig.conn.confirmation()->id == kConfirmFellowshipInvite);
    CHECK(rig.conn.confirmation()->text.find("Leader") != std::string::npos);
    rig.conn.answer_confirmation(false);
    CHECK_FALSE(rig.conn.fellowship().invite);
    rig.conn.receive_zone_packet_for_test(fellowship::invite());
    rig.conn.receive_zone_packet_for_test(fellowship::invite(334));
    REQUIRE(rig.conn.fellowship().invite);
    CHECK(rig.conn.fellowship().invite->inviter_id == 334);
    rig.conn.answer_confirmation(true);
    REQUIRE(rig.until([&] { return requests(rig, 3) == 1; }));
    CHECK_FALSE(rig.conn.fellowship().invite);
    rig.conn.answer_fellowship_invite(true);
    barrier(rig);
    CHECK(requests(rig, 3) == 1);
    CHECK(requests(rig, 4) == 0); // local server interprets native decline4 as legacy destroy camp
    rig.conn.receive_zone_packet_for_test(fellowship::invite());
    clock.hold(); clock.advance(std::chrono::milliseconds(100000));
    rig.step();
    CHECK_FALSE(rig.conn.fellowship().invite);
    CHECK_FALSE(rig.conn.confirmation());
}

TEST_CASE("fellowship connection: zone changes preserve roster and retire zone offers; logout clears all") {
    Rig rig;
    REQUIRE(rig.to_zone());
    rig.conn.receive_zone_packet_for_test(fellowship::roster());
    rig.conn.receive_zone_packet_for_test(fellowship::kits());
    rig.server.request_zone_change(1, 20, 10, 3);
    REQUIRE(rig.until([&] { return rig.conn.state() == ConnectionState::Zoning; }));
    CHECK(rig.conn.fellowship().active());
    CHECK(rig.conn.fellowship().kits.empty());
    rig.conn.disconnect();
    CHECK_FALSE(rig.conn.fellowship().active());
    CHECK(rig.conn.fellowship().vitality.normal == 0);

    Rig invited;
    REQUIRE(invited.to_zone());
    invited.conn.notice("An earlier question");
    invited.conn.receive_zone_packet_for_test(fellowship::invite());
    invited.server.request_zone_change(1, 20, 10, 3);
    REQUIRE(invited.until([&] { return invited.conn.state() == ConnectionState::Zoning; }));
    CHECK_FALSE(invited.conn.fellowship().invite);
    invited.conn.answer_confirmation(true);
    CHECK_FALSE(invited.conn.confirmation());
}

TEST_CASE("fellowship connection: profile seeds vitality and chat requires received membership") {
    Rig rig;
    REQUIRE(rig.to_zone());
    ProfileSpec spec;
    spec.last_fellowship_join = 1712345678;
    spec.fellowship_vitality_normal = 0x100000012ull;
    spec.fellowship_vitality_aa = 1234;
    rig.conn.receive_zone_packet_for_test(build_player_profile(spec));
    REQUIRE(rig.conn.profile());
    CHECK(rig.conn.profile()->last_fellowship_join == 1712345678);
    CHECK(rig.conn.fellowship().vitality.normal == 0x100000012ull);
    CHECK(rig.conn.fellowship().vitality.aa == 1234);
    const auto before = MockEqServer::count(rig.server.zone_log, Op::ChannelMessage);
    rig.conn.send_command("/fsay No membership");
    barrier(rig);
    CHECK(MockEqServer::count(rig.server.zone_log, Op::ChannelMessage) == before);
    rig.conn.receive_zone_packet_for_test(fellowship::roster());
    rig.conn.send_command("/fsay Meet at the camp");
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::ChannelMessage) == before + 1; }));
}

TEST_CASE("fellowship connection: current membership and leadership guard delayed actions") {
    Rig rig;
    REQUIRE(rig.to_zone());
    rig.conn.receive_zone_packet_for_test(fellowship::roster(3));
    rig.conn.fellowship_remove(9876); // oneself uses Leave, not Remove
    rig.conn.fellowship_promote(9876);
    rig.conn.fellowship_remove(99999);
    rig.conn.receive_zone_packet_for_test(fellowship::roster(3, 12, 75, true));
    rig.conn.fellowship_set_motd("Old leader's pending edit");
    rig.conn.fellowship_promote(9878);
    rig.conn.fellowship_remove(9878);
    rig.conn.fellowship_end();
    rig.conn.fellowship_toggle_sharing(9878); // another member
    rig.conn.fellowship_toggle_sharing(9877); // oneself remains allowed
    barrier(rig);
    for (const auto action : {2u, 6u, 7u, 11u}) CHECK(requests(rig, action) == 0);
    CHECK(requests(rig, 12) == 1);
    rig.conn.receive_zone_packet_for_test(fellowship::roster(0));
    rig.conn.fellowship_create_camp(17);
    rig.conn.fellowship_destroy_camp();
    rig.conn.fellowship_leave();
    barrier(rig);
    for (const auto action : {8u, 9u, 10u}) CHECK(requests(rig, action) == 0);
}

TEST_CASE("fellowship connection: unrelated rosters cannot join or evict the local character") {
    Rig rig;
    REQUIRE(rig.to_zone());
    rig.conn.receive_zone_packet_for_test(fellowship::roster(1, 12, 99, true));
    CHECK_FALSE(rig.conn.fellowship().active()); // only OtherLeader is present
    rig.conn.receive_zone_packet_for_test(fellowship::roster());
    REQUIRE(rig.conn.fellowship().active());
    rig.conn.receive_zone_packet_for_test(fellowship::roster(1, 12, 99, true));
    CHECK(rig.conn.fellowship().id == 75);
    rig.conn.receive_zone_packet_for_test(fellowship::roster(1, 12, 75, true));
    CHECK_FALSE(rig.conn.fellowship().active()); // our own fellowship removed us
}

TEST_CASE("fellowship connection: outgoing hooks cannot transmit retired member actions") {
    plugin::Hooks hooks;
    auto options = quick_options(); options.hooks = &hooks;
    Rig rig(MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    rig.conn.receive_zone_packet_for_test(fellowship::roster());
    const auto owner = hooks.new_owner("fellowship role test");
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, kOpFellowshipRequest,
        [&](AppPacket& p) {
            if (field(p) == 6) rig.conn.receive_zone_packet_for_test(fellowship::roster(2, 12, 75, true));
            return plugin::Flow::Continue;
        });
    rig.conn.fellowship_remove(9877);
    barrier(rig);
    CHECK(requests(rig, 6) == 0);
    CHECK_FALSE(rig.conn.fellowship().is_leader("Gennro"));
    hooks.remove(owner);
}

TEST_CASE("fellowship connection: outgoing hooks cannot answer a replacement invite with the old acceptance") {
    plugin::Hooks hooks;
    auto options = quick_options(); options.hooks = &hooks;
    Rig rig(MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    rig.conn.receive_zone_packet_for_test(fellowship::invite());
    const auto owner = hooks.new_owner("fellowship invite test");
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, kOpFellowshipRequest,
        [&](AppPacket& p) {
            if (field(p) == 3) rig.conn.receive_zone_packet_for_test(fellowship::invite(334));
            return plugin::Flow::Continue;
        });
    rig.conn.answer_confirmation(true);
    barrier(rig);
    CHECK(requests(rig, 3) == 0);
    REQUIRE(rig.conn.fellowship().invite);
    CHECK(rig.conn.fellowship().invite->inviter_id == 334);
    REQUIRE(rig.conn.confirmation());
    CHECK(rig.conn.confirmation()->id == kConfirmFellowshipInvite);
    hooks.remove(owner);
}
