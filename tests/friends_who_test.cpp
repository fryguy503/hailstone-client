// /who friends over the real loopback zone and UCS sessions. The who replies below are scripted protocol
// fixtures; they check dispatch and privacy formatting, not a live world's friend lookup or filtering.
#include <doctest/doctest.h>

#include <fstream>
#include <string_view>

#include "core/byte_io.h"
#include "mock_server/harness.h"
#include "net/chat_server.h"
#include "net/social_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

void fixture_strings(Rig& rig) {
    {
        std::ofstream out(rig.dir.path() / "eqstr_us.txt", std::ios::app);
        out << "3112 friend\n"
            << "12307 SAVED FRIENDS\n"
            << "12308 NO FRIENDS\n"
            << "5025 FULL %4 %2 %3 %5 %6 %7\n"
            << "5024 ANON %2\n"
            << "5023 ROLE %2 %3\n"
            << "5036 MATCHES %1\n"
            << "6357 %1%2%3%4%T5%6\n"
            << "1506 Cleric\n"
            << "5006 ZONE %1\n";
    }
    rig.game.rescan();
    rig.conn.reload_game_files();
}

AppPacket buddy(std::string_view name) {
    ByteWriter writer;
    writer.le<std::uint8_t>(1);
    writer.bytes(std::as_bytes(std::span(name.data(), name.size())));
    writer.le<std::uint8_t>(0);
    return AppPacket(ucs::kBuddy, writer.take());
}

AppPacket mailbox() {
    ByteWriter writer;
    writer.le<std::uint8_t>(1);
    writer.le<std::uint32_t>(1);
    writer.le<std::uint32_t>(0);
    const std::string_view address = "SOE.EQ.mock.Gennro";
    writer.bytes(std::as_bytes(std::span(address.data(), address.size())));
    writer.le<std::uint8_t>(0);
    writer.le<std::uint8_t>(0);
    return AppPacket(ucs::kMailLogin, writer.take());
}

MockConfig with_ucs() {
    auto config = MockConfig::standard();
    config.send_chat_server = true;
    return config;
}

void enter_with_friends(Rig& rig) {
    fixture_strings(rig);
    REQUIRE(rig.to_zone());
    REQUIRE(rig.until([&] { return !rig.server.ucs_log.empty(); }));
    // As with stock UCS, buddy rows may precede the login's mailbox response.
    for (std::string_view name : {"Online", "Anonymous", "Roleplayer", "Offline"})
        rig.server.ucs.send_reliable(buddy(name));
    rig.server.ucs.send_reliable(mailbox());
    REQUIRE(rig.until([&] { return rig.conn.chat_server_connected() && rig.conn.social().friends.size() == 4; }));
    rig.chat();
}

const AppPacket* last_packet(const MockEqServer& server, Op op) {
    for (auto it = server.zone_log.rbegin(); it != server.zone_log.rend(); ++it)
        if (from_wire(it->opcode) == op) return &*it;
    return nullptr;
}

} // namespace

TEST_CASE("mock server: who friends aliases with no friends send no lookup") {
    Rig rig;
    fixture_strings(rig);
    REQUIRE(rig.to_zone());
    rig.chat();
    for (const char* command : {"/who friends", "/who friend", "/who all friends", "/who all friend"}) {
        CAPTURE(command);
        rig.conn.send_command(command);
        CHECK(has_line(rig.chat(), "NO FRIENDS"));
        // A reliable marker behind the command makes the absence assertions independent of a sleep.
        const int before = MockEqServer::count(rig.server.zone_log, Op::RandomReq);
        rig.conn.send_zone_packet(encode_random_request(0, 100));
        REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::RandomReq) == before + 1; }));
        CHECK(MockEqServer::count(rig.server.zone_log, Op::FriendsWho) == 0);
        CHECK(MockEqServer::count(rig.server.zone_log, Op::WhoAllRequest) == 0);
    }
}

TEST_CASE("mock server: who friends sends the UCS friend roster instead of saved-name chat") {
    Rig rig(with_ucs());
    enter_with_friends(rig);
    int sent = 0;
    for (const char* command : {"/who friends", "/who friend", "/who all friends", "/who all friend"}) {
        CAPTURE(command);
        rig.conn.send_command(command);
        ++sent;
        REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::FriendsWho) == sent; }));
        const auto* packet = last_packet(rig.server, Op::FriendsWho);
        REQUIRE(packet);
        ByteReader reader(packet->data);
        const std::string scope = std::string_view(command).find("all") != std::string_view::npos ? "1" : "0";
        CHECK(reader.cstring() == scope + ",Online,Anonymous,Roleplayer,Offline");
        CHECK(reader.remaining() == 0);
        CHECK(MockEqServer::count(rig.server.zone_log, Op::WhoAllRequest) == 0);
        CHECK(rig.chat().empty());
    }
    // The ordinary world who path keeps its existing filter request.
    rig.conn.send_command("/who all");
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::WhoAllRequest) == 1; }));
    const auto* ordinary = last_packet(rig.server, Op::WhoAllRequest);
    REQUIRE(ordinary);
    auto request = decode_who_request(*ordinary);
    REQUIRE(request);
    CHECK(request->type == 3);
    CHECK(MockEqServer::count(rig.server.zone_log, Op::FriendsWho) == sent);
}

TEST_CASE("mock server: friends who replies retain online rows and server anonymity") {
    Rig rig(with_ucs());
    enter_with_friends(rig);
    rig.conn.send_command("/who friends");
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::FriendsWho) == 1; }));
    CHECK(rig.chat().empty());

    // EQEmu's friends lookup omits offline friends and returns normal who rows for the rest. Its anonymous
    // formats conceal class, level, race and zone, with the roleplayer's guild still visible.
    WhoResponse response;
    response.footer_string = 5036;
    response.footer_count = 3;
    WhoEntry online;
    online.format = 5025;
    online.name = "Online";
    online.class_ = 2;
    online.level = 30;
    online.race = 1;
    online.zone_string = 5006;
    online.zone_id = 2;
    WhoEntry anonymous;
    anonymous.format = 5024;
    anonymous.name = "Anonymous";
    WhoEntry roleplayer;
    roleplayer.format = 5023;
    roleplayer.name = "Roleplayer";
    roleplayer.guild = "<Role Guild>";
    response.entries = {online, anonymous, roleplayer};
    rig.server.zone.send_reliable(encode(response));
    REQUIRE(rig.until([&] { return rig.conn.who_count() == 1; }));
    const auto* received = rig.conn.last_who();
    REQUIRE(received);
    REQUIRE(received->entries.size() == 3);
    CHECK(received->entries[0].name == "Online");
    CHECK(received->entries[1].format == 5024);
    CHECK(received->entries[2].format == 5023);
    CHECK(rig.conn.social().friends.size() == 4);
    const auto lines = rig.chat();
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].text == "FULL Online 30 Cleric Human  ZONE qeynos2");
    CHECK(lines[1].text == "ANON Anonymous");
    CHECK(lines[2].text == "ROLE Roleplayer <Role Guild>");
    CHECK(lines[3].text == "MATCHES 3");
    for (const auto& line : lines) {
        CHECK(line.kind == ChatEvent::Kind::System);
        CHECK(line.color == 281);
        CHECK(line.text.find("Offline") == std::string::npos);
    }

    rig.conn.send_command("/who all friends");
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::FriendsWho) == 2; }));
    // A reply with all friends offline still completes and replaces the last populated result.
    response.entries.clear();
    response.footer_count = 0;
    rig.server.zone.send_reliable(encode(response));
    REQUIRE(rig.until([&] { return rig.conn.who_count() == 2; }));
    REQUIRE(rig.conn.last_who());
    CHECK(rig.conn.last_who()->entries.empty());
    CHECK(rig.conn.social().friends.size() == 4);
    const auto empty = rig.chat();
    REQUIRE(empty.size() == 1);
    CHECK(empty[0].text == "MATCHES 0");
    CHECK(empty[0].color == 281);
}

TEST_CASE("mock server: friends who strips only the exact current-world qualifier") {
    Rig rig(with_ucs());
    fixture_strings(rig);
    REQUIRE(rig.to_zone());
    REQUIRE(rig.until([&] { return !rig.server.ucs_log.empty(); }));
    const std::vector<std::string> saved = {"mock.Online", "Other.Alice", "Mock.Case"};
    for (const auto& name : saved) rig.server.ucs.send_reliable(buddy(name));
    rig.server.ucs.send_reliable(mailbox());
    REQUIRE(rig.until([&] { return rig.conn.chat_server_connected() && rig.conn.social().friends.size() == 3; }));
    rig.chat();

    int sent = 0;
    for (const char* command : {"/who friends", "/who all friends"}) {
        CAPTURE(command);
        rig.conn.send_command(command);
        ++sent;
        REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::FriendsWho) == sent; }));
        const auto* packet = last_packet(rig.server, Op::FriendsWho);
        REQUIRE(packet);
        ByteReader reader(packet->data);
        CHECK(reader.cstring() == (sent == 1 ? "0,Online,Other.Alice,Mock.Case" : "1,Online,Other.Alice,Mock.Case"));
        CHECK(reader.remaining() == 0);
        CHECK(rig.conn.social().friends == saved);
        CHECK(MockEqServer::count(rig.server.zone_log, Op::WhoAllRequest) == 0);
        CHECK(rig.chat().empty());
    }
}
