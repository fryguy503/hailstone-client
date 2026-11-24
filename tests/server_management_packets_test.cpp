#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <string>

#include "core/byte_io.h"
#include "mock_server/harness.h"
#include "net/server_management_packets.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

AppPacket management_reply(ServerManagementAction action, std::uint32_t id, ServerManagementStatus status,
                           std::uint32_t revision, std::string_view text) {
    ByteWriter w;
    w.fixed_string("HSAM", 4);
    w.le<std::uint16_t>(1);
    w.le(static_cast<std::uint16_t>(action));
    w.le(id);
    w.le(static_cast<std::uint16_t>(status));
    w.le(revision);
    w.le(static_cast<std::uint16_t>(text.size()));
    w.fixed_string(text, text.size());
    return AppPacket(kOpServerManagementReply, w.take());
}

std::size_t management_requests(const test::Rig& r) {
    return static_cast<std::size_t>(std::count_if(r.server.zone_log.begin(), r.server.zone_log.end(),
        [](const AppPacket& p) { return p.opcode == kOpServerManagementRequest; }));
}

} // namespace

TEST_CASE("server management uses bounded typed request and reply packets") {
    auto open = encode_server_management_open(17);
    CHECK(open.opcode == 0x143a);
    REQUIRE(open.data.size() == 12);
    ByteReader r(open.data);
    CHECK(r.fixed_string(4) == "HSAM");
    CHECK(r.le<std::uint16_t>() == 1);
    CHECK(r.le<std::uint16_t>() == 1);
    CHECK(r.le<std::uint32_t>() == 17);

    const std::array edits{ServerManagementEdit{"commands", "blocked_message", "Ask a GM; see #help."},
                           ServerManagementEdit{"addons", "allow", ""}};
    auto packet = encode_server_management_save(18, 321, edits);
    REQUIRE(packet);
    r = ByteReader(packet->data);
    CHECK(r.fixed_string(4) == "HSAM");
    CHECK(r.le<std::uint16_t>() == 1);
    CHECK(r.le<std::uint16_t>() == 2);
    CHECK(r.le<std::uint32_t>() == 18);
    CHECK(r.le<std::uint32_t>() == 321);
    CHECK(r.le<std::uint16_t>() == 2);
    for (const auto& edit : edits) {
        const auto a = r.le<std::uint8_t>();
        const auto b = r.le<std::uint8_t>();
        const auto c = r.le<std::uint16_t>();
        CHECK(r.fixed_string(a) == edit.section);
        CHECK(r.fixed_string(b) == edit.key);
        CHECK(r.fixed_string(c) == edit.value);
    }
    CHECK(r.remaining() == 0);
    CHECK_FALSE(encode_server_management_save(0, 1, edits));
    CHECK_FALSE(encode_server_management_save(1, 0, edits));
    CHECK_FALSE(encode_server_management_save(1, 1, {}));
    const std::array invalid{ServerManagementEdit{"commands", "blocked_message", "two\nlines"}};
    CHECK_FALSE(encode_server_management_save(1, 1, invalid));
    const std::array oversized{ServerManagementEdit{"commands", "allow", std::string(8192, 'a')},
                               ServerManagementEdit{"commands", "block", std::string(8192, 'a')}};
    CHECK_FALSE(encode_server_management_save(1, 1, oversized));

    auto valid = management_reply(ServerManagementAction::Save, 3, ServerManagementStatus::Ok, 55, "[addons]\n");
    auto reply = decode_server_management_reply(valid);
    REQUIRE(reply);
    CHECK(reply->action == ServerManagementAction::Save);
    CHECK(reply->id == 3);
    CHECK(reply->revision == 55);
    CHECK(reply->text == "[addons]\n");

    for (std::size_t size = 0; size < valid.data.size(); ++size) {
        auto truncated = valid;
        truncated.data.resize(size);
        CHECK_FALSE(decode_server_management_reply(truncated));
    }
    valid.data.push_back(std::byte{0});
    CHECK_FALSE(decode_server_management_reply(valid));
    CHECK_FALSE(decode_server_management_reply(management_reply(ServerManagementAction::Open, 3,
        ServerManagementStatus::Ok, 0, "")));
    CHECK_FALSE(decode_server_management_reply(management_reply(ServerManagementAction::Open, 3,
        ServerManagementStatus::Invalid, 0, "invalid\nfield")));
    CHECK_FALSE(decode_server_management_reply(management_reply(ServerManagementAction::Open, 3,
        ServerManagementStatus::Ok, 1, std::string(kServerManagementMaxText + 1, 'a'))));
}

TEST_CASE("settings packets carry an optional server account status") {
    auto legacy = encode(HailstoneSettings{kServerManagementOffered});
    CHECK(legacy.data.size() == 10);
    auto parsed = decode_hailstone_settings(legacy);
    REQUIRE(parsed);
    CHECK_FALSE(parsed->account_status);
    CHECK(parsed->flags == kServerManagementOffered);

    auto ranked = encode(HailstoneSettings{kServerManagementOffered, 200});
    REQUIRE(ranked.data.size() == 12);
    auto parsed_ranked = decode_hailstone_settings(ranked);
    REQUIRE(parsed_ranked);
    CHECK(parsed_ranked->account_status == 200);
    CHECK(parsed_ranked->flags == (kServerManagementOffered | kServerAccountStatus));
    auto rank_zero = decode_hailstone_settings(encode(HailstoneSettings{0, 0}));
    REQUIRE(rank_zero);
    CHECK(rank_zero->account_status == 0);
    CHECK_FALSE(decode_hailstone_settings(encode(HailstoneSettings{0, 32768})));
    ranked.data.pop_back();
    CHECK_FALSE(decode_hailstone_settings(ranked));
}

TEST_CASE("mock server management gates the editor and correlates snapshots and saves") {
    test::ManualClock clock;
    auto options = test::quick_options();
    options.session.clock = clock.source();
    test::Rig r(test::MockConfig::standard(), std::move(options));
    REQUIRE(r.to_zone());
    r.server.zone.send_reliable(encode(HailstoneSettings{}));
    CHECK_FALSE(r.conn.open_server_management());
    CHECK(management_requests(r) == 0);

    r.server.zone.send_reliable(encode(HailstoneSettings{kServerManagementOffered}));
    REQUIRE(r.until([&] { return r.conn.server_management_available(); }));
    REQUIRE(r.conn.open_server_management());
    CHECK_FALSE(r.conn.server_management_snapshot());
    REQUIRE(r.until([&] { return management_requests(r) == 1; }));
    const std::string cfg = "[commands]\nblock=/nav\n; " + std::string(4096, 'x');
    r.server.zone.send_reliable(management_reply(ServerManagementAction::Save, 1, ServerManagementStatus::Ok, 41, "bad"));
    r.server.zone.send_reliable(management_reply(ServerManagementAction::Open, 2, ServerManagementStatus::Ok, 41, "bad"));
    r.server.zone.send_reliable(management_reply(ServerManagementAction::Open, 1, ServerManagementStatus::Ok, 42, cfg));
    REQUIRE(r.until([&] { return r.conn.server_management_snapshot().has_value(); }));
    CHECK(r.conn.server_management_snapshot()->revision == 42);
    CHECK(r.conn.server_management_snapshot()->text == cfg);
    CHECK(r.conn.server_management_serial() == 1);
    CHECK_FALSE(r.conn.server_management_reload_required());
    CHECK_FALSE(r.conn.server_management_reloaded());

    const std::array edits{ServerManagementEdit{"commands", "blocked_message", "Ask a GM."}};
    CHECK_FALSE(r.conn.save_server_management(41, edits));
    REQUIRE(r.conn.save_server_management(42, edits));
    CHECK(r.conn.server_management_saving());
    CHECK_FALSE(r.conn.save_server_management(42, edits));
    REQUIRE(r.until([&] { return management_requests(r) == 2; }));
    r.server.zone.send_reliable(management_reply(ServerManagementAction::Save, 2, ServerManagementStatus::Conflict, 0,
                                                 "The policy changed. Reopen the editor."));
    REQUIRE(r.until([&] { return !r.conn.server_management_saving(); }));
    CHECK(r.conn.server_management_reload_required());
    CHECK(r.conn.server_management_snapshot()->revision == 42);
    CHECK(r.conn.server_management_message() == "The policy changed. Reopen the editor.");
    CHECK(r.conn.server_management_serial() == 1);

    CHECK_FALSE(r.conn.save_server_management(42, edits));
    REQUIRE(r.conn.open_server_management());
    CHECK(r.conn.server_management_reloading());
    CHECK(r.conn.server_management_snapshot()->revision == 42);
    CHECK_FALSE(r.conn.save_server_management(42, edits));
    CHECK(r.conn.open_server_management()); // A second click reuses the outstanding Open.
    REQUIRE(r.until([&] { return management_requests(r) == 3; }));
    r.server.zone.send_reliable(management_reply(ServerManagementAction::Save, 2, ServerManagementStatus::Ok, 99, "old"));
    r.server.zone.send_reliable(management_reply(ServerManagementAction::Open, 3, ServerManagementStatus::Ok, 43, cfg));
    REQUIRE(r.until([&] { return !r.conn.server_management_reloading(); }));
    CHECK_FALSE(r.conn.server_management_reload_required());
    CHECK(r.conn.server_management_reloaded());
    CHECK(r.conn.server_management_snapshot()->revision == 43);
    CHECK_FALSE(r.conn.save_server_management(42, edits));
    REQUIRE(r.conn.save_server_management(43, edits));
    REQUIRE(r.until([&] { return management_requests(r) == 4; }));
    const std::string changed = cfg + "\nblocked_message=\"Ask a GM.\"\n";
    r.server.zone.send_reliable(management_reply(ServerManagementAction::Save, 4, ServerManagementStatus::Ok, 44, changed));
    REQUIRE(r.until([&] { return !r.conn.server_management_saving(); }));
    REQUIRE(r.conn.server_management_snapshot());
    CHECK(r.conn.server_management_snapshot()->revision == 44);
    CHECK(r.conn.server_management_snapshot()->text == changed);
    CHECK(r.conn.server_management_serial() == 3);
    CHECK_FALSE(r.conn.server_management_reloaded());

    REQUIRE(r.conn.save_server_management(44, edits));
    REQUIRE(r.until([&] { return management_requests(r) == 5; }));
    clock.hold();
    clock.advance(std::chrono::seconds(11));
    REQUIRE(r.until([&] { return !r.conn.server_management_saving(); }));
    CHECK(r.conn.server_management_message().find("Save was not confirmed") == 0);
    CHECK(r.conn.server_management_snapshot()->revision == 44);
    CHECK(r.conn.server_management_reload_required());
    CHECK_FALSE(r.conn.save_server_management(44, edits));
    REQUIRE(r.conn.open_server_management());
    REQUIRE(r.until([&] { return management_requests(r) == 6; }));
    r.server.zone.send_reliable(management_reply(ServerManagementAction::Open, 6, ServerManagementStatus::Unavailable, 0,
                                                 "Could not read hailstone.cfg."));
    REQUIRE(r.until([&] { return !r.conn.server_management_reloading(); }));
    CHECK(r.conn.server_management_reload_required());
    CHECK(r.conn.server_management_snapshot()->revision == 44);
    CHECK_FALSE(r.conn.save_server_management(44, edits));
    REQUIRE(r.conn.open_server_management());
    REQUIRE(r.until([&] { return management_requests(r) == 7; }));
    r.server.zone.send_reliable(management_reply(ServerManagementAction::Open, 7, ServerManagementStatus::Denied, 0,
                                                 "Your account no longer has access."));
    REQUIRE(r.until([&] { return !r.conn.server_management_available(); }));
    CHECK_FALSE(r.conn.server_management_snapshot());
    CHECK(test::has_line(r.chat(), "Your account no longer has access."));
}
