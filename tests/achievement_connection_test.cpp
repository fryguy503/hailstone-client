#include <doctest/doctest.h>

#include <algorithm>

#include "achievement_test_packets.h"
#include "manual_clock.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "plugin/hooks.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;
namespace fixture = hailstone::test::achievement_fixture;

namespace {
std::size_t requests(const Rig& rig, std::uint16_t opcode) {
    return static_cast<std::size_t>(std::count_if(rig.server.zone_log.begin(), rig.server.zone_log.end(),
        [&](const AppPacket& packet) { return packet.opcode == opcode; }));
}
const AppPacket& last_request(const Rig& rig, std::uint16_t opcode) {
    const auto it = std::find_if(rig.server.zone_log.rbegin(), rig.server.zone_log.rend(),
        [&](const AppPacket& packet) { return packet.opcode == opcode; });
    REQUIRE(it != rig.server.zone_log.rend()); return *it;
}
void received(Rig& rig, bool primary = true) {
    rig.conn.receive_zone_packet_for_test(fixture::catalog());
    REQUIRE(rig.conn.achievements().catalog());
    if (primary) {
        rig.conn.receive_zone_packet_for_test(fixture::primary());
        REQUIRE(rig.conn.achievements().primary_ready());
    }
}
void barrier(Rig& rig) {
    const auto before = requests(rig, 0x7f01);
    rig.conn.send_zone_packet(AppPacket(0x7f01));
    REQUIRE(rig.until([&] { return requests(rig, 0x7f01) != before; }));
}
void peer(Rig& rig, const std::string& name = "Peer") {
    REQUIRE(rig.conn.world().player());
    const auto position = game::world_to_server(rig.conn.world().player()->position);
    rig.server.add_spawn(make_spawn(name, 77, 0, 1, position.x, position.y, position.z));
    const bool arrived = rig.until([&] {
        const auto found = rig.conn.world().spawns.find(77);
        return found != rig.conn.world().spawns.end() && found->second.name == name;
    });
    REQUIRE(arrived);
    GameConnectionTestAccess::world(rig.conn).target_id = 77;
    REQUIRE(rig.conn.world().spawns.at(77).incarnation != 0);
}
} // namespace

TEST_CASE("achievement connection: received catalog indices, unknown counts, exact refresh and reward requests") {
    Rig rig;
    REQUIRE(rig.to_zone());
    CHECK(requests(rig, kOpAchievementWindowRequest) == 0);
    CHECK_FALSE(rig.conn.open_achievement(0, 1));
    CHECK_FALSE(rig.conn.inspect_achievement_link(fixture::metadata()));
    REQUIRE(rig.conn.request_achievements());
    REQUIRE(rig.until([&] { return requests(rig, kOpAchievementWindowRequest) == 1; }));
    REQUIRE(last_request(rig, kOpAchievementWindowRequest).data.size() == 1);
    CHECK(last_request(rig, kOpAchievementWindowRequest).data[0] == std::byte{0x30});
    CHECK_FALSE(rig.conn.request_achievements());
    received(rig);
    const auto token = rig.conn.achievements().catalog_token();
    const auto revision = rig.conn.achievements().revision();
    REQUIRE(rig.conn.achievements().states().size() == 2);
    REQUIRE(rig.conn.achievements().states()[1]);
    CHECK_FALSE(rig.conn.achievements().states()[1]->counts[1][0]);
    CHECK(rig.conn.achievements().states()[1]->counts[3].empty());
    CHECK_FALSE(rig.conn.achievement_chat_link(token, 1)); // satisfaction bits do not invent shareable counts
    auto invalid = fixture::catalog(); invalid.data.pop_back();
    rig.conn.receive_zone_packet_for_test(invalid);
    CHECK(rig.conn.achievements().catalog_token() == token);
    CHECK(rig.conn.achievements().revision() == revision);
    rig.conn.receive_zone_packet_for_test(fixture::progress(2, 70000));
    rig.conn.receive_zone_packet_for_test(fixture::progress(3, 70001));
    REQUIRE(rig.conn.achievements().states()[1]->counts[1][0]);
    CHECK(*rig.conn.achievements().states()[1]->counts[1][0] == 5);
    CHECK(rig.conn.achievements().requirements(900).size() == 2);
    CHECK(rig.conn.achievements().states()[1]->status == AchievementStatus::Open); // counts never grant completion
    REQUIRE(rig.conn.open_achievement(token, 1));
    REQUIRE(rig.conn.achievements().selection());
    CHECK(rig.conn.achievements().selection()->definition_index == 1);
    CHECK(rig.conn.achievements().selection()->source == game::AchievementDetailSource::Own);
    const auto shared = rig.conn.achievement_chat_link(token, 1);
    REQUIRE(shared);
    CHECK(shared->starts_with(std::string("\x12") + "3" + rig.conn.character_name() + "^900^"));
    CHECK(shared->find("'Second & <Achievement>\x12") != std::string::npos);
    CHECK_FALSE(rig.conn.achievement_reward(token, 0)); // no received reward flag
    CHECK_FALSE(rig.conn.achievement_reward(token, 900)); // achievement ID is not definition index
    REQUIRE(rig.conn.achievement_reward(token, 1));
    REQUIRE(rig.until([&] { return requests(rig, 0x6411) == 1; }));
    ByteReader reward(last_request(rig, 0x6411).data);
    CHECK(reward.le<std::uint32_t>() == 5); CHECK(reward.le<std::uint32_t>() == 1); CHECK(reward.empty());
    rig.conn.receive_zone_packet_for_test(fixture::catalog("Replacement catalog"));
    CHECK_FALSE(rig.conn.open_achievement(token, 1));
    CHECK_FALSE(rig.conn.achievement_reward(token, 1));
    CHECK_FALSE(rig.conn.achievement_chat_link(token, 1));
    CHECK_FALSE(rig.conn.achievements().primary_ready());
}

TEST_CASE("achievement connection: comparison is target incarnation scoped and late snapshots drain safely") {
    ManualClock clock;
    auto options = quick_options(); options.session.clock = clock.source();
    Rig rig(MockConfig::standard(), options);
    REQUIRE(rig.to_zone()); received(rig); peer(rig);
    clock.hold();
    const auto own_status = rig.conn.achievements().states()[1]->status;
    REQUIRE(rig.conn.compare_achievements());
    REQUIRE(rig.until([&] { return requests(rig, 0x1665) == 1; }));
    ByteReader request(last_request(rig, 0x1665).data);
    CHECK(request.le<std::uint32_t>() == 77); CHECK(request.le<std::uint32_t>() == rig.conn.world().player_id);
    CHECK(request.empty());
    CHECK_FALSE(rig.conn.compare_achievements());
    CHECK(rig.conn.achievement_message() == "Waiting for the selected player's achievement comparison.");
    rig.conn.receive_zone_packet_for_test(fixture::comparison());
    REQUIRE(rig.conn.achievements().comparison().size() == 2);
    CHECK(rig.conn.achievement_message().empty()); // the reply shown: no longer waiting
    CHECK(rig.conn.achievements().comparison_name() == "Peer");
    CHECK(rig.conn.achievements().comparison()[1].status == AchievementStatus::Completed);
    CHECK(rig.conn.achievements().states()[1]->status == own_status);
    rig.conn.clear_achievement_comparison();
    CHECK(rig.conn.achievements().comparison().empty());
    clock.advance(std::chrono::milliseconds(1000));
    REQUIRE(rig.conn.compare_achievements());
    REQUIRE(rig.until([&] { return requests(rig, 0x1665) == 2; }));
    const auto incarnation = rig.conn.world().spawns.at(77).incarnation;
    const auto position = game::world_to_server(rig.conn.world().player()->position);
    rig.server.add_spawn(make_spawn("Peer", 77, 0, 1, position.x, position.y, position.z)); // same name/ID, new full spawn
    const bool replaced = rig.until([&] { return rig.conn.world().spawns.at(77).incarnation != incarnation; });
    REQUIRE(replaced);
    rig.conn.receive_zone_packet_for_test(fixture::comparison());
    CHECK(rig.conn.achievements().comparison().empty());
    CHECK_FALSE(rig.conn.achievements().compare_pending());
    clock.advance(std::chrono::milliseconds(1000));
    REQUIRE(rig.conn.compare_achievements());
    REQUIRE(rig.until([&] { return requests(rig, 0x1665) == 3; }));
    clock.advance(std::chrono::milliseconds(game::Achievements::kRequestTimeoutMs)); rig.step();
    CHECK(rig.conn.achievements().compare_pending()); // no invented wire nonce or timeout reuse
    CHECK(rig.conn.achievement_message().starts_with("The comparison timed out."));
    CHECK_FALSE(rig.conn.compare_achievements());
    rig.conn.receive_zone_packet_for_test(fixture::comparison());
    CHECK_FALSE(rig.conn.achievements().compare_pending());
    CHECK(rig.conn.achievement_message().empty()); // the late reply drained
    CHECK(rig.conn.achievements().comparison().empty());
    CHECK(rig.conn.achievements().states()[1]->status == own_status);
}

TEST_CASE("achievement connection: clicked snapshots are independent of target and never replace own progress") {
    ManualClock clock;
    auto options = quick_options(); options.session.clock = clock.source();
    Rig rig(MockConfig::standard(), options);
    REQUIRE(rig.to_zone()); received(rig);
    rig.conn.receive_zone_packet_for_test(fixture::progress(2));
    GameConnectionTestAccess::world(rig.conn).target_id = 0;
    REQUIRE(rig.conn.inspect_achievement_link(fixture::metadata())); // offline snapshot has no wire request
    REQUIRE(rig.conn.achievements().detail_link());
    CHECK_FALSE(rig.conn.achievements().link_pending());
    CHECK(requests(rig, kOpAchievementLinkRequest) == 0);
    peer(rig);
    GameConnectionTestAccess::world(rig.conn).target_id = 0;
    REQUIRE(rig.conn.inspect_achievement_link(fixture::metadata())); // local Peer need not be selected
    REQUIRE(rig.until([&] { return requests(rig, kOpAchievementLinkRequest) == 1; }));
    REQUIRE(rig.conn.achievements().detail_link());
    CHECK(rig.conn.achievements().detail_link()->player_name == "Peer");
    CHECK(rig.conn.achievements().detail_state()->counts[1][0] == 4);
    CHECK(rig.conn.achievements().states()[1]->counts[1][0] == 2);
    GameConnectionTestAccess::world(rig.conn).target_id = 101; // unrelated target changes do not retire chat metadata
    rig.conn.receive_zone_packet_for_test(fixture::inspect("SomeoneElse", 900, 9));
    CHECK(rig.conn.achievements().link_pending());
    CHECK(rig.conn.achievements().detail_state()->counts[1][0] == 4);
    rig.conn.receive_zone_packet_for_test(fixture::inspect("Peer", 900, 9));
    CHECK_FALSE(rig.conn.achievements().link_pending());
    CHECK(rig.conn.achievements().detail_state()->counts[1][0] == 9);
    CHECK(rig.conn.achievements().states()[1]->counts[1][0] == 2);
    rig.conn.receive_zone_packet_for_test(fixture::inspect("Peer", 900, 99));
    CHECK(rig.conn.achievements().detail_state()->counts[1][0] == 9); // unsolicited duplicate cannot mutate view
    clock.hold(); clock.advance(std::chrono::milliseconds(1000));
    REQUIRE(rig.conn.inspect_achievement_link(fixture::metadata()));
    REQUIRE(rig.until([&] { return requests(rig, kOpAchievementLinkRequest) == 2; }));
    rig.conn.close_achievement_detail();
    CHECK_FALSE(rig.conn.achievements().detail_shown());
    clock.advance(std::chrono::milliseconds(game::Achievements::kRequestTimeoutMs)); rig.step();
    CHECK(rig.conn.achievements().link_pending());
    CHECK(rig.conn.achievements().link_retired());
    CHECK(requests(rig, kOpAchievementLinkRequest) == 2);
    rig.conn.receive_zone_packet_for_test(fixture::inspect());
    CHECK_FALSE(rig.conn.achievements().link_pending());
    CHECK_FALSE(rig.conn.achievements().detail_shown());
    rig.server.request_zone_change(2, 10, 20, 30);
    REQUIRE(rig.until([&] { return rig.conn.state() == ConnectionState::Zoning; }));
    CHECK_FALSE(rig.conn.achievements().catalog());
    CHECK(rig.conn.achievements().states().empty());
    CHECK_FALSE(rig.conn.request_achievements());
    CHECK_FALSE(rig.conn.inspect_achievement_link(fixture::metadata()));
}

TEST_CASE("achievement connection: chat links show snapshots and receive updates during the camp countdown") {
    Rig rig;
    REQUIRE(rig.to_zone());
    received(rig);
    peer(rig);
    rig.conn.camp();
    REQUIRE(rig.conn.state() == ConnectionState::Camping);

    REQUIRE(rig.conn.inspect_achievement_link(fixture::metadata("Elsewhere")));
    REQUIRE(rig.conn.achievements().detail_shown());
    REQUIRE(rig.conn.achievements().detail_state());
    CHECK(rig.conn.achievements().detail_state()->counts[1][0] == 4);
    barrier(rig);
    CHECK(requests(rig, kOpAchievementLinkRequest) == 0);

    REQUIRE(rig.conn.inspect_achievement_link(fixture::metadata()));
    REQUIRE(rig.until([&] { return requests(rig, kOpAchievementLinkRequest) == 1; }));
    REQUIRE(rig.conn.achievements().link_pending());
    rig.server.zone.send_reliable(fixture::inspect("Peer", 900, 9));
    REQUIRE(rig.until([&] { return !rig.conn.achievements().link_pending(); }));
    REQUIRE(rig.conn.achievements().detail_state());
    CHECK(rig.conn.achievements().detail_state()->counts[1][0] == 9);
    CHECK(rig.conn.state() == ConnectionState::Camping);

    rig.server.drop_on_logout = false;
    rig.conn.set_fast_camp(true);
    REQUIRE(rig.until([&] { return rig.server.logouts == 1; }));
    REQUIRE(rig.conn.state() == ConnectionState::Camping);
    CHECK_FALSE(rig.conn.inspect_achievement_link(fixture::metadata()));
}

TEST_CASE("achievement connection: old-schema replies drain across catalog count and component changes without adoption") {
    for (const bool change_components : {false, true}) {
        CAPTURE(change_components);
        Rig rig;
        REQUIRE(rig.to_zone()); received(rig); peer(rig);
        REQUIRE(rig.conn.compare_achievements());
        REQUIRE(rig.conn.inspect_achievement_link(fixture::metadata()));
        REQUIRE(rig.until([&] { return requests(rig, 0x1665) == 1 && requests(rig, kOpAchievementLinkRequest) == 1; }));
        rig.conn.receive_zone_packet_for_test(fixture::catalog("Intermediate catalog", 1, 17));
        const auto* retired = rig.conn.achievements().comparison_reply_catalog();
        REQUIRE(retired);
        CHECK(retired == rig.conn.achievements().inspection_reply_catalog());
        const std::uint32_t definitions = change_components ? 2 : 3;
        const std::uint32_t components = change_components ? 17 : 1;
        rig.conn.receive_zone_packet_for_test(fixture::catalog("Current catalog", definitions, components));
        rig.conn.receive_zone_packet_for_test(fixture::primary(1, false, definitions, components));
        ByteWriter progress; progress.le<std::uint32_t>(0);
        rig.conn.receive_zone_packet_for_test({kOpAchievementProgress, progress.take()});
        const auto token = rig.conn.achievements().catalog_token();
        REQUIRE(rig.conn.open_achievement(token, 0));
        CHECK(rig.conn.achievements().comparison_reply_catalog() == retired);
        CHECK(rig.conn.achievements().compare_retired()); CHECK(rig.conn.achievements().link_retired());
        CHECK_FALSE(rig.conn.compare_achievements());

        auto malformed_comparison = fixture::comparison(); malformed_comparison.data.pop_back();
        auto malformed_inspection = fixture::inspect(); malformed_inspection.data.pop_back();
        rig.conn.receive_zone_packet_for_test(malformed_comparison);
        rig.conn.receive_zone_packet_for_test(malformed_inspection);
        rig.conn.receive_zone_packet_for_test(fixture::inspect("Other"));
        CHECK(rig.conn.achievements().compare_pending()); CHECK(rig.conn.achievements().link_pending());
        rig.conn.receive_zone_packet_for_test(fixture::comparison());
        rig.conn.receive_zone_packet_for_test(fixture::inspect("Peer", 900, 99));
        CHECK_FALSE(rig.conn.achievements().compare_pending()); CHECK_FALSE(rig.conn.achievements().link_pending());
        CHECK(rig.conn.achievements().comparison().empty());
        REQUIRE(rig.conn.achievements().selection());
        CHECK(rig.conn.achievements().selection()->source == game::AchievementDetailSource::Own);
        CHECK(rig.conn.achievements().selection()->definition_index == 0);
        CHECK(rig.conn.achievements().detail_state()->counts[1][0] == 0);
        CHECK(rig.conn.achievements().states()[1]->status == AchievementStatus::Open);
        CHECK(rig.conn.achievements().states()[1]->counts[1][0] == 0);

        REQUIRE(rig.conn.compare_achievements());
        REQUIRE(rig.conn.inspect_achievement_link(fixture::metadata("Peer", 900, 4, components)));
        REQUIRE(rig.until([&] { return requests(rig, 0x1665) == 2 && requests(rig, kOpAchievementLinkRequest) == 2; }));
        rig.conn.receive_zone_packet_for_test(fixture::comparison(true, definitions, components));
        rig.conn.receive_zone_packet_for_test(fixture::inspect("Peer", 900, 9, components));
        CHECK_FALSE(rig.conn.achievements().compare_pending()); CHECK_FALSE(rig.conn.achievements().link_pending());
        CHECK(rig.conn.achievements().comparison().size() == definitions);
        CHECK(rig.conn.achievements().comparison_name() == "Peer");
        REQUIRE(rig.conn.achievements().detail_link());
        CHECK(rig.conn.achievements().detail_link()->state.counts[1].size() == components);
        CHECK(rig.conn.achievements().detail_state()->counts[1][0] == 9);
        CHECK(rig.conn.achievements().states()[1]->status == AchievementStatus::Open);
        CHECK(rig.conn.achievements().states()[1]->counts[1][0] == 0);
    }
}

TEST_CASE("achievement connection: outgoing link hooks cannot send or reopen a retired detail") {
    for (const int mode : {0, 1, 2, 3}) {
        CAPTURE(mode);
        plugin::Hooks hooks;
        auto options = quick_options(); options.hooks = &hooks;
        Rig rig(MockConfig::standard(), options);
        REQUIRE(rig.to_zone()); received(rig); peer(rig);
        const auto owner = hooks.new_owner("achievement link lifecycle test");
        hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, kOpAchievementLinkRequest,
            [&](AppPacket&) {
                if (mode == 0) return plugin::Flow::Drop;
                if (mode == 1) rig.conn.close_achievement_detail();
                if (mode == 2) rig.conn.receive_zone_packet_for_test(fixture::catalog("Replaced during handoff"));
                if (mode == 3) rig.conn.disconnect();
                return plugin::Flow::Continue;
            });
        CHECK_FALSE(rig.conn.inspect_achievement_link(fixture::metadata()));
        CHECK_FALSE(rig.conn.achievements().link_pending());
        CHECK(rig.conn.achievements().detail_shown() == (mode == 0)); // dropped refresh leaves only the admitted snapshot
        hooks.remove(owner);
        if (mode != 3) barrier(rig);
        CHECK(requests(rig, kOpAchievementLinkRequest) == 0);
    }
}

TEST_CASE("achievement connection: a late inspection cannot replace a different offline linked detail") {
    Rig rig;
    REQUIRE(rig.to_zone()); received(rig); peer(rig);
    REQUIRE(rig.conn.inspect_achievement_link(fixture::metadata()));
    REQUIRE(rig.until([&] { return requests(rig, kOpAchievementLinkRequest) == 1; }));
    REQUIRE(rig.conn.inspect_achievement_link(fixture::metadata("Offline", 42, 1)));
    REQUIRE(rig.conn.achievements().detail_link());
    CHECK(rig.conn.achievements().detail_link()->achievement_id == 42);
    CHECK(rig.conn.achievements().detail_link()->player_name == "Offline");
    CHECK(rig.conn.achievements().link_pending());
    CHECK(rig.conn.achievements().link_retired());
    rig.server.remove_spawn(77);
    const bool removed = rig.until([&] { return !rig.conn.world().spawns.contains(77); });
    REQUIRE(removed);
    REQUIRE(rig.conn.achievements().detail_link());
    CHECK(rig.conn.achievements().detail_link()->player_name == "Offline"); // retiring A's actor cannot clear B's view
    rig.conn.receive_zone_packet_for_test(fixture::inspect("Peer", 900, 99));
    CHECK_FALSE(rig.conn.achievements().link_pending());
    REQUIRE(rig.conn.achievements().detail_link());
    CHECK(rig.conn.achievements().detail_link()->achievement_id == 42);
    CHECK(rig.conn.achievements().detail_link()->player_name == "Offline");
    CHECK(rig.conn.achievements().detail_state()->counts[1][0] == 1);
    CHECK_FALSE(rig.conn.achievements().states()[0]->counts[1][0]);
    barrier(rig);
    CHECK(requests(rig, kOpAchievementLinkRequest) == 1);
}

TEST_CASE("achievement connection: old local owner incarnation cannot replace the current own detail") {
    Rig rig;
    REQUIRE(rig.to_zone()); received(rig); peer(rig);
    REQUIRE(rig.conn.inspect_achievement_link(fixture::metadata()));
    REQUIRE(rig.until([&] { return requests(rig, kOpAchievementLinkRequest) == 1; }));
    REQUIRE(rig.conn.open_achievement(rig.conn.achievements().catalog_token(), 0));
    const auto old = rig.conn.world().spawns.at(77).incarnation;
    const auto position = game::world_to_server(rig.conn.world().player()->position);
    rig.server.add_spawn(make_spawn("Peer", 77, 0, 1, position.x, position.y, position.z));
    const bool replaced = rig.until([&] { return rig.conn.world().spawns.at(77).incarnation != old; });
    REQUIRE(replaced);
    rig.conn.receive_zone_packet_for_test(fixture::inspect("Peer", 900, 99));
    CHECK_FALSE(rig.conn.achievements().link_pending());
    REQUIRE(rig.conn.achievements().selection());
    CHECK(rig.conn.achievements().selection()->source == game::AchievementDetailSource::Own);
    CHECK(rig.conn.achievements().selection()->definition_index == 0);
    CHECK_FALSE(rig.conn.achievements().detail_link());
    CHECK_FALSE(rig.conn.achievements().states()[0]->counts[1][0]);
}

TEST_CASE("achievement connection: earned chat callbacks retire old overlay sound and event work") {
    for (const bool disconnect : {false, true}) {
        CAPTURE(disconnect);
        plugin::Hooks hooks;
        auto options = quick_options(); options.hooks = &hooks;
        Rig rig(MockConfig::standard(), options);
        REQUIRE(rig.to_zone()); received(rig);
        rig.conn.take_sound_cues(); rig.conn.text_overlay().clear();
        const auto owner = hooks.new_owner("achievement earned lifecycle test");
        bool called = false;
        hooks.on_chat(owner, [&](plugin::ChatLine& line) {
            if (line.text.find(" completed ") != std::string::npos) {
                called = true;
                if (disconnect) rig.conn.disconnect();
                else rig.conn.receive_zone_packet_for_test(fixture::catalog("Changed inside earned chat"));
            }
            return plugin::Flow::Continue;
        });
        rig.conn.receive_zone_packet_for_test(fixture::earned(rig.conn.world().player_id, rig.conn.character_name()));
        CHECK(called);
        CHECK_FALSE(rig.conn.text_overlay().active());
        CHECK(rig.conn.take_sound_cues().empty());
        // Consume the test-only observation queue without adding a mutable product accessor.
        CHECK_FALSE(const_cast<game::Achievements&>(rig.conn.achievements()).take_earned());
        hooks.remove(owner);
    }
}

TEST_CASE("achievement connection: outgoing reward hook cannot send a replaced catalog index") {
    plugin::Hooks hooks;
    auto options = quick_options(); options.hooks = &hooks;
    Rig rig(MockConfig::standard(), options);
    REQUIRE(rig.to_zone()); received(rig);
    const auto owner = hooks.new_owner("achievement reward lifecycle test");
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, 0x6411, [&](AppPacket& packet) {
        ByteReader reader(packet.data);
        if (reader.le<std::uint32_t>() == 5)
            rig.conn.receive_zone_packet_for_test(fixture::catalog("Replaced during reward handoff"));
        return plugin::Flow::Continue;
    });
    CHECK_FALSE(rig.conn.achievement_reward(rig.conn.achievements().catalog_token(), 1));
    hooks.remove(owner); barrier(rig);
    CHECK(requests(rig, 0x6411) == 0);
    CHECK_FALSE(rig.conn.rewards().lane(RewardLane::Pending).shown);
}

TEST_CASE("achievement connection: unsolicited guild achievements are filtered display without own-state authority") {
    Rig rig;
    REQUIRE(rig.to_zone()); received(rig);
    rig.chat(); rig.conn.take_sound_cues(); rig.conn.text_overlay().clear();
    for (const int setting : {0, 1, 2}) {
        CAPTURE(setting);
        rig.conn.set_chat_filter(32, setting); // show all, own only, hide
        rig.conn.receive_zone_packet_for_test(fixture::guild("Offline"));
        const auto other = rig.chat();
        CHECK(other.size() == (setting == 0 ? 1u : 0u));
        if (!other.empty()) {
            CHECK(other.front().kind == ChatEvent::Kind::Guild);
            CHECK(other.front().color == 349);
            CHECK(other.front().peer == "Offline");
            CHECK(other.front().text.find(std::string("\x12") + "3Offline^900^") != std::string::npos);
        }
        rig.conn.receive_zone_packet_for_test(fixture::guild(rig.conn.character_name()));
        const auto own = rig.chat();
        CHECK(own.size() == (setting != 2 ? 1u : 0u));
        if (!own.empty()) CHECK(own.front().color == 348);
        CHECK_FALSE(rig.conn.text_overlay().active()); // guild path has no local earned presentation
        CHECK(rig.conn.take_sound_cues().empty());
        CHECK_FALSE(const_cast<game::Achievements&>(rig.conn.achievements()).take_earned());
        CHECK(rig.conn.achievements().states()[1]->status == AchievementStatus::Open);
        CHECK_FALSE(rig.conn.achievements().states()[1]->counts[1][0]);
    }
    rig.conn.set_chat_filter(32, 0);
    rig.conn.receive_zone_packet_for_test(fixture::guild("Offline", "Different"));
    CHECK(rig.chat().empty()); // header and caret metadata must name the same player
    auto malformed = fixture::guild("Offline"); malformed.data.pop_back();
    rig.conn.receive_zone_packet_for_test(malformed);
    CHECK(rig.chat().empty());
    rig.conn.disconnect();
    rig.conn.receive_zone_packet_for_test(fixture::guild("Offline"));
    CHECK(rig.chat().empty());
}

TEST_CASE("achievement connection: earned chat filters do not invent completion or suppress received sound identity") {
    Rig rig;
    REQUIRE(rig.to_zone()); received(rig); peer(rig);
    rig.chat(); rig.conn.take_sound_cues(); rig.conn.text_overlay().clear();
    for (const int setting : {0, 1, 2}) {
        CAPTURE(setting);
        rig.conn.set_chat_filter(32, setting);
        rig.conn.receive_zone_packet_for_test(fixture::earned(77, "WrongName"));
        CHECK(rig.chat().empty());
        CHECK(rig.conn.take_sound_cues().empty());
        CHECK_FALSE(const_cast<game::Achievements&>(rig.conn.achievements()).take_earned());
        rig.conn.receive_zone_packet_for_test(fixture::earned(77, "Peer"));
        const auto other = rig.chat();
        CHECK(other.size() == (setting == 0 ? 1u : 0u));
        if (!other.empty()) CHECK(other.front().color == 349);
        CHECK_FALSE(rig.conn.text_overlay().active());
        const auto sounds = rig.conn.take_sound_cues();
        REQUIRE(sounds.size() == 1);
        CHECK(sounds.front().kind == SoundCue::Kind::SpawnWave);
        CHECK(sounds.front().target == 77); CHECK(sounds.front().spell == 17);
        REQUIRE(const_cast<game::Achievements&>(rig.conn.achievements()).take_earned());
        rig.conn.receive_zone_packet_for_test(fixture::earned(rig.conn.world().player_id, rig.conn.character_name()));
        const auto own = rig.chat();
        CHECK(own.size() == (setting != 2 ? 1u : 0u));
        if (!own.empty()) CHECK(own.front().color == 348);
        CHECK(rig.conn.text_overlay().active() == (setting != 2));
        rig.conn.text_overlay().clear();
        const auto own_sounds = rig.conn.take_sound_cues();
        REQUIRE(own_sounds.size() == 1);
        CHECK(own_sounds.front().target == rig.conn.world().player_id);
        REQUIRE(const_cast<game::Achievements&>(rig.conn.achievements()).take_earned());
        CHECK(rig.conn.achievements().states()[1]->status == AchievementStatus::Open);
        CHECK_FALSE(rig.conn.achievements().states()[1]->counts[1][0]);
    }
}
