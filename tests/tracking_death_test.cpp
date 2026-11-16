// Tracking death cleanup through the loopback EQEmu: the tracking window closes when tracking begins, but its
// cached entries and active target still need the OP_Death handler's stop and removal actions.
#include <doctest/doctest.h>

#include <algorithm>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "core/byte_io.h"
#include "game/death_report.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/packet_io.h"
#include "net/player_packets.h"
#include "plugin/hooks.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

struct TrackRow {
    std::uint32_t id;
    std::string_view name;
    bool npc = true;
};

AppPacket tracking_reply(std::initializer_list<TrackRow> rows) {
    ByteWriter w;
    w.le(static_cast<std::uint16_t>(rows.size()));
    for (const auto& row : rows) {
        w.le(row.id);
        w.f32(20.0f);
        w.le<std::uint8_t>(10);
        w.le<std::uint8_t>(row.npc ? 1 : 0);
        detail::write_cstring(w, row.name);
        w.le<std::uint8_t>(0); // pet
        w.le<std::uint8_t>(0); // mercenary
    }
    return make_packet(Op::Track, std::move(w));
}

void publish_tracking(Rig& rig, std::initializer_list<TrackRow> rows) {
    // This fixture exercises accepted Tracking traffic: use an eligible tracker and issue its request first.
    GameConnectionTestAccess::tracking_skill_fixture(rig.conn);
    REQUIRE(rig.conn.tracking_admitted());
    rig.conn.send_track();
    const auto revision = rig.conn.tracking().revision;
    rig.server.zone.send_reliable(tracking_reply(rows));
    REQUIRE(rig.until([&] { return rig.conn.tracking().revision != revision; }));
}

void tracking_barrier(Rig& rig) {
    const auto before = rig.server.zone_log.size();
    rig.conn.send_zone_packet(AppPacket(0x7f01));
    REQUIRE(rig.until([&] {
        return rig.server.zone_log.size() > before && rig.server.zone_log.back().opcode == 0x7f01;
    }));
}

std::vector<std::uint32_t> tracked_packets(const Rig& rig) {
    std::vector<std::uint32_t> ids;
    for (const auto& p : rig.server.zone_log) {
        if (!is_op(p, Op::TrackTarget)) continue;
        REQUIRE(p.data.size() == 4);
        ByteReader r(p.data);
        ids.push_back(r.le<std::uint32_t>());
    }
    return ids;
}

std::vector<std::int32_t> tracking_ids(const GameConnection& c) {
    std::vector<std::int32_t> ids;
    for (const auto& line : c.tracking_lines()) ids.push_back(line.spawn_id);
    return ids;
}

Death death_of(std::uint32_t id) {
    Death d;
    d.spawn_id = d.corpse_id = id;
    d.killer_id = 102;
    d.damage = 5000;
    return d;
}

void receive_death(Rig& rig, std::uint32_t id) {
    rig.server.zone.send_reliable(encode(death_of(id)));
    REQUIRE(rig.until([&] { return rig.conn.world().spawns.at(id).is_corpse; }));
}

void begin_tracking(Rig& rig, std::uint32_t id) {
    rig.conn.track_spawn(static_cast<std::int32_t>(id));
    CHECK(rig.conn.tracking().tracked == static_cast<std::int32_t>(id));
    CHECK_FALSE(rig.conn.tracking().open);
    tracking_barrier(rig);
    CHECK(tracked_packets(rig) == std::vector<std::uint32_t>{id});
    rig.server.zone_log.clear();
    rig.chat();
}

} // namespace

TEST_CASE("Tracking death: a tracked spawn stops once and leaves the cached list") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    std::uint32_t dead = 101;
    bool player = false;
    SUBCASE("NPC") {}
    SUBCASE("another player, hidden by the player filter") { dead = 103; player = true; }
    SUBCASE("the local player") { dead = c.world().player_id; player = true; }
    if (dead == 103) {
        rig.server.add_spawn(make_spawn("Tracked_Player", dead, 0, 1, -300, -140, 3));
        REQUIRE(rig.until([&] { return c.world().spawns.contains(dead); }));
    }
    publish_tracking(rig, {{dead, "Tracked_Spawn", !player}, {102, "Guard_Bob"}});
    begin_tracking(rig, dead);
    if (dead == 103) {
        auto filter = c.tracking().filter;
        filter.players = 0;
        c.set_track_filter(filter);
        CHECK(tracking_ids(c) == std::vector<std::int32_t>{102});
    }
    const auto revision = c.tracking().revision;
    receive_death(rig, dead);
    CHECK(c.tracking().tracked == 0);
    CHECK(c.tracking().revision != revision);
    CHECK_FALSE(c.tracking().open);
    CHECK(tracking_ids(c) == (dead == c.world().player_id ? std::vector<std::int32_t>{} : std::vector<std::int32_t>{102}));
    CHECK(std::none_of(c.tracking().list.begin(), c.tracking().list.end(), [&](const game::TrackEntry& e) {
        return e.spawn_id == static_cast<std::int32_t>(dead);
    }));
    tracking_barrier(rig);
    CHECK(tracked_packets(rig) == std::vector<std::uint32_t>{0});
    const auto chat = rig.chat();
    const auto stopped = std::find_if(chat.begin(), chat.end(), [](const ChatEvent& e) { return e.text == "[12102]"; });
    REQUIRE(stopped != chat.end());
    CHECK(stopped->kind == ChatEvent::Kind::System);
    CHECK(stopped->color == game::kTrackingChatColor);

    // A filter/sort rebuild cannot offer the corpse again, nor may a second death report send another stop.
    c.set_track_filter({});
    c.set_track_sort(0);
    CHECK(tracking_ids(c) == (dead == c.world().player_id ? std::vector<std::int32_t>{} : std::vector<std::int32_t>{102}));
    c.track_spawn(static_cast<std::int32_t>(dead));
    c.receive_zone_packet_for_test(encode(death_of(dead)));
    tracking_barrier(rig);
    CHECK(tracked_packets(rig) == std::vector<std::uint32_t>{0});
    CHECK_FALSE(has_line(rig.chat(), "[12102]"));
}

TEST_CASE("Tracking death: an untracked death removes its row without stopping another target") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    std::uint32_t dead = 101;
    bool active = true;
    SUBCASE("another NPC dies") {}
    SUBCASE("the local player dies") { dead = c.world().player_id; }
    SUBCASE("tracking is inactive") { active = false; }
    publish_tracking(rig, {{dead, "Dying_Spawn", dead != c.world().player_id}, {102, "Guard_Bob"}});
    if (active) begin_tracking(rig, 102);
    else rig.chat();
    const auto revision = c.tracking().revision;
    receive_death(rig, dead);
    CHECK(c.tracking().revision != revision);
    CHECK(c.tracking().tracked == (active && dead != c.world().player_id ? 102 : 0));
    CHECK(tracking_ids(c) == (dead == c.world().player_id ? std::vector<std::int32_t>{} : std::vector<std::int32_t>{102}));
    CHECK(c.player_status().dead == (dead == c.world().player_id));
    tracking_barrier(rig);
    CHECK(tracked_packets(rig).empty());
    CHECK_FALSE(has_line(rig.chat(), "[12102]"));
}

TEST_CASE("Tracking death: the active target is stopped even after a reply omitted its row") {
    Rig rig;
    REQUIRE(rig.to_zone());
    publish_tracking(rig, {{101, "a_rat001"}, {102, "Guard_Bob"}});
    begin_tracking(rig, 101);
    publish_tracking(rig, {{102, "Guard_Bob"}});
    CHECK(rig.conn.tracking().tracked == 101);
    receive_death(rig, 101);
    CHECK(rig.conn.tracking().tracked == 0);
    CHECK(tracking_ids(rig.conn) == std::vector<std::int32_t>{102});
    tracking_barrier(rig);
    CHECK(tracked_packets(rig) == std::vector<std::uint32_t>{0});
    CHECK(has_line(rig.chat(), "[12102]"));
}

TEST_CASE("Tracking death: a death for an unknown world spawn leaves tracking alone") {
    Rig rig;
    REQUIRE(rig.to_zone());
    publish_tracking(rig, {{900, "Unknown_Spawn"}, {102, "Guard_Bob"}});
    begin_tracking(rig, 900);
    const auto revision = rig.conn.tracking().revision;
    rig.conn.receive_zone_packet_for_test(encode(death_of(900)));
    CHECK(rig.conn.tracking().tracked == 900);
    CHECK(rig.conn.tracking().revision == revision);
    CHECK(tracking_ids(rig.conn) == std::vector<std::int32_t>{900, 102});
    tracking_barrier(rig);
    CHECK(tracked_packets(rig).empty());
    CHECK_FALSE(has_line(rig.chat(), "[12102]"));
}

TEST_CASE("Tracking death: an outgoing hook can replace tracking but cannot revive the dead target") {
    plugin::Hooks hooks;
    auto options = quick_options();
    options.hooks = &hooks;
    Rig rig(MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    publish_tracking(rig, {{101, "a_rat001"}, {102, "Guard_Bob"}});
    begin_tracking(rig, 101);
    std::int32_t replacement = 102;
    SUBCASE("start tracking a living spawn") {}
    SUBCASE("attempt to track the dying spawn again") { replacement = 101; }
    const auto owner = hooks.new_owner("tracking replacement test");
    bool replaced = false;
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, to_wire(Op::TrackTarget),
        [&](AppPacket& p) {
            ByteReader r(p.data);
            if (r.le<std::uint32_t>() == 0 && !replaced) {
                replaced = true;
                rig.conn.track_spawn(replacement);
            }
            return plugin::Flow::Continue;
        });
    receive_death(rig, 101);
    CHECK(replaced);
    const std::uint32_t expected = replacement == 101 ? 0u : 102u;
    CHECK(rig.conn.tracking().tracked == static_cast<std::int32_t>(expected));
    CHECK(tracking_ids(rig.conn) == std::vector<std::int32_t>{102});
    tracking_barrier(rig);
    // The dead row cannot be tracked again; a living replacement must not be cancelled by the old stop.
    CHECK(tracked_packets(rig) == std::vector<std::uint32_t>{expected});
    hooks.remove(owner);
}
