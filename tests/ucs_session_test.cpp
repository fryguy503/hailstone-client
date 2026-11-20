// Independent UCS application payloads over real loopback SOE sessions. The peer shares transport framing with
// the existing mock, but never uses the client's UCS encoders to make its replies or expected command bytes.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "core/byte_io.h"
#include "manual_clock.h"
#include "mock_server/harness.h"
#include "net/chat_server.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;
using namespace std::chrono_literals;

namespace {

AppPacket strings(std::uint16_t opcode, std::initializer_list<std::string_view> fields) {
    ByteWriter writer;
    for (auto field : fields) {
        writer.bytes(std::as_bytes(std::span(field.data(), field.size())));
        writer.le<std::uint8_t>(0);
    }
    return AppPacket(opcode, writer.take());
}

AppPacket mailbox_reply(std::string_view list = "SOE.EQ.test.Hero,SOE.EQ.test.Alt", std::uint32_t count = 2) {
    ByteWriter writer;
    writer.le<std::uint8_t>(1);
    writer.le<std::uint32_t>(count);
    writer.le<std::uint32_t>(0);
    writer.bytes(std::as_bytes(std::span(list.data(), list.size())));
    writer.le<std::uint8_t>(0);
    writer.le<std::uint8_t>(0); // EQEmu's additional trailing byte
    return AppPacket(0x01, writer.take());
}

AppPacket buddy_reply(std::string_view name) {
    auto packet = strings(0x06, {name});
    packet.data.insert(packet.data.begin(), std::byte{1});
    return packet;
}

std::string raw_text(const AppPacket& packet) {
    return std::string(reinterpret_cast<const char*>(packet.data.data()), packet.data.size());
}

ChatServerInfo identity(std::uint16_t port) {
    return {"127.0.0.1", port, "test", "Hero", 'S', "1234ABCD"};
}

ChatServer::Options options(const ManualClock& clock) {
    ChatServer::Options out;
    out.session.clock = clock.source();
    out.session.service_interval = 0ms;
    return out;
}

struct UcsRig {
    ManualClock clock;
    SoeEndpoint peer{SoeEndpoint::default_params(), 1};
    ChatServer chat{options(clock)};
    std::vector<AppPacket> received;

    UcsRig() { clock.hold(); }
    void step() {
        chat.update();
        peer.poll();
        while (auto packet = peer.take()) received.push_back(std::move(*packet));
    }
    bool until(const std::function<bool()>& done) { return pump([&] { step(); }, done); }
    bool start() {
        chat.connect(identity(peer.port()));
        return until([&] { return !received.empty(); });
    }
    bool authenticate() {
        peer.send_reliable(mailbox_reply());
        return until([&] { return chat.connected(); });
    }
    std::vector<ChatServerEvent> events() {
        std::vector<ChatServerEvent> out;
        while (auto event = chat.poll()) out.push_back(std::move(*event));
        return out;
    }
};

} // namespace

TEST_CASE("UCS transport uses the separately observed chat profile") {
    const auto profile = ucs_session_options();
    CHECK(profile.opcode_bytes == 1);
    CHECK(profile.window_slots == 24);
    CHECK(profile.window_bytes == 10000);
    CHECK(profile.reliable_timeout_ms == 120000);
    CHECK(profile.connect_timeout == 30s);
    CHECK(profile.request_interval == 5s);
    CHECK(profile.idle_timeout == 60s); // Hailstone's own: the original sets none
    CHECK(profile.hold_time == 0ms);
    CHECK(profile.stat_interval == 0ms);
    CHECK(profile.keepalive_interval == 44s);
    CHECK(ucs_session_options(true).keepalive_interval == 14800ms);
}

TEST_CASE("UCS login replies require complete counted mailbox lists and expose refusal") {
    auto success = decode_chat_server_packet(mailbox_reply());
    REQUIRE(success);
    CHECK(success->accepted);
    CHECK(success->fields == std::vector<std::string>{"SOE.EQ.test.Hero", "SOE.EQ.test.Alt"});
    auto empty = decode_chat_server_packet(mailbox_reply("", 0));
    REQUIRE(empty);
    CHECK(empty->accepted);
    CHECK(empty->fields.empty());
    CHECK_FALSE(decode_chat_server_packet(mailbox_reply("SOE.EQ.test.Hero", 2)));
    CHECK_FALSE(decode_chat_server_packet(mailbox_reply("SOE.EQ.test.Hero,", 2)));
    auto partial = mailbox_reply();
    partial.data.resize(8);
    CHECK_FALSE(decode_chat_server_packet(partial));
    AppPacket denied(0x01, {std::byte{0}});
    auto refusal = decode_chat_server_packet(denied);
    REQUIRE(refusal);
    CHECK_FALSE(refusal->accepted);
}

TEST_CASE("UCS session requests retry at five seconds and an unanswered connection ends at thirty") {
    ManualClock clock;
    clock.hold();
    platform::UdpSocket peer;
    std::string error;
    auto local = platform::Endpoint::v4(127, 0, 0, 1, 0);
    REQUIRE(peer.open(platform::Endpoint::Family::V4, error, &local));
    auto opt = options(clock);
    opt.reconnect_delay = 0ms;
    ChatServer chat(std::move(opt));
    chat.connect(identity(peer.local_endpoint()->port));
    int requests = 0;
    auto step = [&] {
        chat.update();
        std::array<std::byte, 65536> buffer;
        platform::Endpoint from;
        while (auto length = peer.recv_from(buffer, from)) {
            if (*length >= 14 && buffer[0] == std::byte{0} && buffer[1] == std::byte{1}) ++requests;
        }
    };
    REQUIRE(pump(step, [&] { return requests == 1; }));
    clock.advance(4999ms);
    step();
    CHECK(requests == 1);
    clock.advance(1ms);
    step();
    CHECK(requests == 2);
    clock.advance(25s);
    step();
    CHECK(chat.failed());
    CHECK(chat.error().find("connection timed out") != std::string::npos);
}

TEST_CASE("UCS holds commands and pre-login friends until authentication, then releases each once") {
    UcsRig rig;
    REQUIRE(rig.start());
    REQUIRE(rig.received.size() == 1);
    CHECK(rig.received[0].opcode == 0x01);
    ByteReader login(rig.received[0].data);
    CHECK(login.le<std::uint8_t>() == 0);
    CHECK(login.cstring() == "SOE.EQ.test.Hero");
    CHECK(login.cstring() == "S1234ABCD");
    CHECK_FALSE(rig.chat.connected());
    CHECK(rig.chat.send_command("join general"));
    CHECK(rig.chat.send_mail_command("[getheaders"));
    rig.peer.send_reliable(buddy_reply("Friend")); // stock UCS sends these before SendMailBoxes
    auto ignored = strings(0x15, {"SOE.EQ.test.Ignored"});
    ignored.data.insert(ignored.data.begin(), std::byte{0}); // zero means add ignore
    rig.peer.send_reliable(ignored);
    REQUIRE(rig.until([&] { return rig.peer.unacked() == 0; }));
    CHECK(rig.events().empty());
    CHECK(rig.received.size() == 1);
    REQUIRE(rig.authenticate());
    REQUIRE(rig.until([&] { return rig.received.size() == 3; }));
    CHECK(rig.received[1].opcode == 0x02);
    CHECK(raw_text(rig.received[1]) == std::string("\0join general\0", 14));
    CHECK(raw_text(rig.received[2]) == std::string(";getheaders\0\0", 13));
    auto events = rig.events();
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == ChatServerEvent::Kind::MailboxList);
    CHECK(events[1].kind == ChatServerEvent::Kind::Buddy);
    CHECK(events[1].text == "Friend");
    CHECK(events[2].kind == ChatServerEvent::Kind::Ignore);
    CHECK(events[2].added);
    CHECK(events[2].text == "SOE.EQ.test.Ignored");
    rig.peer.send_reliable(mailbox_reply()); // repeated application reply, not merely a transport duplicate
    REQUIRE(rig.until([&] { return rig.peer.unacked() == 0; }));
    CHECK(rig.events().empty());
    CHECK(rig.received.size() == 3);
}

TEST_CASE("UCS denial drops queued work and requires fresh credentials before another attempt") {
    UcsRig rig;
    REQUIRE(rig.start());
    CHECK(rig.chat.send_command("join stale"));
    rig.peer.send_reliable(buddy_reply("Stale"));
    rig.peer.send_reliable(AppPacket(0x01, {std::byte{0}}));
    REQUIRE(rig.until([&] { return rig.chat.failed(); }));
    CHECK_FALSE(rig.chat.connected());
    CHECK(rig.chat.error().find("refused") != std::string::npos);
    auto events = rig.events();
    REQUIRE(events.size() == 1);
    CHECK(events.front().kind == ChatServerEvent::Kind::Disconnected);
    rig.clock.advance(1min);
    rig.step();
    CHECK(rig.peer.sessions() == 1);
    CHECK(rig.received.size() == 1);
    rig.chat.connect(identity(rig.peer.port()));
    REQUIRE(rig.until([&] { return rig.peer.sessions() == 2 && rig.received.size() == 2; }));
    REQUIRE(rig.authenticate());
    CHECK(rig.received.size() == 2); // the refused attempt's command is gone
    CHECK(rig.chat.error().empty());
}

TEST_CASE("UCS link closed before any login reply is a refusal, as stock UCS sends no refusal packet") {
    // EQEmu's UCS answers a bad key by closing the stream (ucs/clientlist.cpp: KeyValid = false, Close()).
    UcsRig rig;
    REQUIRE(rig.start());
    CHECK(rig.chat.send_command("join stale"));
    rig.peer.send_disconnect();
    REQUIRE(rig.until([&] { return rig.chat.failed(); }));
    CHECK(rig.chat.error().find("before answering the login") != std::string::npos);
    auto events = rig.events();
    REQUIRE(events.size() == 1);
    CHECK(events.front().kind == ChatServerEvent::Kind::Disconnected);
    for (int i = 0; i < 20; ++i) {
        rig.clock.advance(1min);
        rig.step();
    }
    CHECK(rig.peer.sessions() == 1);
    CHECK(rig.received.size() == 1);
    rig.chat.connect(identity(rig.peer.port())); // the next OP_SetChatServer2
    REQUIRE(rig.until([&] { return rig.peer.sessions() == 2 && rig.received.size() == 2; }));
    REQUIRE(rig.authenticate());
    CHECK(rig.received.size() == 2);
}

TEST_CASE("UCS automatic retries back off, stop after the limit and restart their count on a login") {
    ManualClock clock;
    clock.hold();
    SoeEndpoint peer(SoeEndpoint::default_params(), 1);
    auto opt = options(clock);
    opt.reconnect_delay = 5s;
    opt.max_reconnect_delay = 15s;
    opt.max_reconnect_attempts = 3;
    ChatServer chat(std::move(opt));
    std::size_t logins = 0;
    auto step = [&] {
        chat.update();
        peer.poll();
        while (auto packet = peer.take())
            if (packet->opcode == 0x01) ++logins;
    };
    chat.connect(identity(peer.port()));
    REQUIRE(pump(step, [&] { return logins == 1; }));
    // Each attempt is acknowledged and never answered: the login timeout fails it with a retry.
    auto expire_and_wait = [&](std::chrono::milliseconds delay) {
        clock.advance(30s);
        step();
        REQUIRE(chat.failed());
        const auto before = peer.sessions();
        clock.advance(delay - 1ms);
        step();
        CHECK(peer.sessions() == before);
        clock.advance(1ms);
        return pump(step, [&] { return peer.sessions() == before + 1 && logins == static_cast<std::size_t>(before + 1); });
    };
    REQUIRE(expire_and_wait(5s));
    REQUIRE(expire_and_wait(10s));
    REQUIRE(expire_and_wait(15s)); // 20 s capped at 15 s
    clock.advance(30s);
    step();
    REQUIRE(chat.failed());
    for (int i = 0; i < 10; ++i) {
        clock.advance(1min);
        step();
    }
    CHECK(peer.sessions() == 4); // three retries, then it waits for new credentials
    chat.connect(identity(peer.port()));
    REQUIRE(pump(step, [&] { return peer.sessions() == 5 && logins == 5; }));
    REQUIRE(expire_and_wait(5s));
    peer.send_reliable(mailbox_reply());
    REQUIRE(pump(step, [&] { return chat.connected(); }));
    peer.send_disconnect(); // after an accepted login: a lost link, retried from the first delay again
    REQUIRE(pump(step, [&] { return chat.failed(); }));
    clock.advance(4999ms);
    step();
    CHECK(peer.sessions() == 6);
    clock.advance(1ms);
    REQUIRE(pump(step, [&] { return peer.sessions() == 7; }));
}

TEST_CASE("UCS failure keeps authenticated events not yet polled and their channel numbers") {
    UcsRig rig;
    REQUIRE(rig.start());
    REQUIRE(rig.authenticate());
    rig.events();
    rig.peer.send_reliable(strings(0x02, {"general"}));
    REQUIRE(rig.until([&] { return rig.chat.channels() == std::vector<std::string>{"general"}; }));
    rig.peer.drop_next_acks(1000);
    CHECK(rig.chat.send_command("join slow"));
    REQUIRE(rig.until([&] { return rig.received.size() == 2; }));
    rig.peer.send_reliable(strings(0x03, {"$SOE.EQ.test.Hero", "SOE.EQ.test.Friend", "a tell", "SPAM:0:"}));
    rig.peer.send_reliable(strings(0x03, {"general", "SOE.EQ.test.Friend", "channel text", "SPAM:0:"}));
    REQUIRE(rig.until([&] { return rig.peer.unacked() == 0; }));
    rig.clock.advance(35001ms);
    rig.peer.send_keepalive();
    rig.step(); // the unacknowledged-command check fails the link in the same update
    REQUIRE(rig.chat.failed());
    auto first = rig.chat.poll();
    REQUIRE(first);
    CHECK(first->kind == ChatServerEvent::Kind::Channels);
    auto tell = rig.chat.poll();
    REQUIRE(tell);
    CHECK(tell->kind == ChatServerEvent::Kind::Message);
    CHECK(tell->text == "a tell");
    auto text = rig.chat.poll();
    REQUIRE(text);
    CHECK(text->text == "channel text");
    CHECK(rig.chat.channels() == std::vector<std::string>{"general"}); // still numbered as /1
    auto gone = rig.chat.poll();
    REQUIRE(gone);
    CHECK(gone->kind == ChatServerEvent::Kind::Disconnected);
    CHECK(rig.chat.channels().empty());
    CHECK_FALSE(rig.chat.poll());
}

TEST_CASE("UCS acknowledged login without a reply expires and retries without stale commands") {
    UcsRig rig;
    REQUIRE(rig.start());
    CHECK(rig.chat.send_mail_command("[mailto old message"));
    rig.clock.advance(29999ms);
    rig.step();
    CHECK_FALSE(rig.chat.failed());
    rig.clock.advance(1ms);
    rig.step();
    CHECK(rig.chat.failed());
    CHECK(rig.chat.error().find("answer the login") != std::string::npos);
    rig.clock.advance(4999ms);
    rig.step();
    CHECK(rig.peer.sessions() == 1);
    rig.clock.advance(1ms);
    REQUIRE(rig.until([&] { return rig.peer.sessions() == 2 && rig.received.size() == 2; }));
    REQUIRE(rig.authenticate());
    CHECK(rig.received.size() == 2);
}

TEST_CASE("UCS unacknowledged commands time out despite incoming keepalives") {
    UcsRig rig;
    REQUIRE(rig.start());
    REQUIRE(rig.authenticate());
    rig.events();
    rig.peer.drop_next_acks(1000);
    CHECK(rig.chat.send_command("join slow"));
    REQUIRE(rig.until([&] { return rig.received.size() == 2; }));
    rig.clock.advance(30s);
    rig.peer.send_keepalive();
    rig.step();
    CHECK_FALSE(rig.chat.failed()); // original's check is strictly greater than 30000
    rig.clock.advance(5001ms);
    rig.peer.send_keepalive();
    rig.step();
    CHECK(rig.chat.failed());
    CHECK(rig.chat.error().find("acknowledge reliable") != std::string::npos);
    CHECK(rig.chat.channels().empty());
}

TEST_CASE("UCS link that goes silent ends after 60 s and is retried, with nothing reliable in flight") {
    UcsRig rig;
    REQUIRE(rig.start());
    REQUIRE(rig.authenticate());
    rig.events();
    // A server that keeps sending its keepalives keeps an idle chat link up.
    for (int i = 0; i < 3; ++i) {
        rig.clock.advance(50s);
        rig.peer.send_keepalive();
        rig.step();
        REQUIRE(rig.chat.connected());
    }
    // Then nothing more comes (the server dropped the session, or the way back is gone). The client sends only its
    // unreliable keepalives, so no unacknowledged data ever ages.
    rig.clock.advance(59999ms);
    rig.step();
    CHECK(rig.chat.connected());
    CHECK(rig.peer.unacked() == 0);
    rig.clock.advance(1ms);
    rig.step();
    REQUIRE(rig.chat.failed());
    CHECK(rig.chat.error().find("nothing received") != std::string::npos);
    bool disconnected = false;
    for (auto& e : rig.events()) disconnected |= e.kind == ChatServerEvent::Kind::Disconnected;
    CHECK(disconnected);
    rig.clock.advance(4999ms);
    rig.step();
    CHECK(rig.peer.sessions() == 1);
    rig.clock.advance(1ms);
    REQUIRE(rig.until([&] { return rig.peer.sessions() == 2 && rig.received.size() == 2; }));
    CHECK(rig.received.back().opcode == 0x01); // a fresh login
    REQUIRE(rig.authenticate());
}

TEST_CASE("UCS link deaf both ways is retried on a new socket") {
    UcsRig rig;
    REQUIRE(rig.start());
    REQUIRE(rig.authenticate());
    rig.events();
    rig.chat.simulate_link_loss();
    rig.peer.send_keepalive(); // never heard
    rig.clock.advance(60s);
    rig.step();
    REQUIRE(rig.chat.failed());
    rig.clock.advance(5s);
    REQUIRE(rig.until([&] { return rig.peer.sessions() == 2 && rig.received.size() == 2; }));
    REQUIRE(rig.authenticate());
    CHECK(rig.chat.send_command("join back"));
    REQUIRE(rig.until([&] { return rig.received.size() == 3; }));
    CHECK(raw_text(rig.received.back()).find("join back") != std::string::npos);
}

TEST_CASE("UCS socket closed under the link fails at once and is retried") {
    UcsRig rig;
    REQUIRE(rig.start());
    REQUIRE(rig.authenticate());
    rig.events();
    rig.chat.simulate_socket_loss();
    CHECK(rig.chat.send_command("join lost")); // queued on the session, whose sends now fail
    rig.step();
    REQUIRE(rig.chat.failed());
    CHECK(rig.chat.error().find("socket was closed") != std::string::npos);
    rig.clock.advance(5s);
    REQUIRE(rig.until([&] { return rig.peer.sessions() == 2 && rig.received.size() == 2; }));
    REQUIRE(rig.authenticate());
    CHECK(rig.received.size() == 2); // the command sent on the lost link is not replayed
}

TEST_CASE("UCS queues are bounded and disconnect discards pending data even during DNS") {
    UcsRig rig;
    rig.chat.connect(identity(rig.peer.port()));
    int accepted = 0;
    for (int i = 0; i < 200; ++i)
        if (rig.chat.send_command("join test")) ++accepted;
    CHECK(accepted == 128);
    rig.chat.disconnect();
    CHECK(rig.chat.failed());
    CHECK_FALSE(rig.chat.resolving());
    CHECK_FALSE(rig.chat.send_command("join closed"));
    rig.chat.connect(identity(rig.peer.port()));
    CHECK(rig.chat.send_mail_command(std::string(200000, 'a')));
    CHECK_FALSE(rig.chat.send_mail_command(std::string(200000, 'b')));
    CHECK_FALSE(rig.chat.send_command(std::string("bad\0tail", 8)));
    rig.chat.disconnect();
    REQUIRE(rig.start());
    REQUIRE(rig.authenticate());
    CHECK(rig.received.size() == 1);
}

TEST_CASE("UCS excessive events before login fail without exposing untrusted friend state") {
    UcsRig rig;
    REQUIRE(rig.start());
    for (int i = 0; i < 257; ++i) rig.peer.send_reliable(buddy_reply("Friend"));
    REQUIRE(rig.until([&] { return rig.chat.failed(); }));
    auto events = rig.events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == ChatServerEvent::Kind::Disconnected);
    CHECK(rig.chat.error().find("before the login") != std::string::npos);
}

TEST_CASE("UCS pre-login byte limit rejects one large event independently of its count limit") {
    ManualClock clock;
    clock.hold();
    SoeEndpoint peer(SoeEndpoint::default_params(), 1);
    auto opt = options(clock);
    opt.max_before_login_bytes = 256;
    ChatServer chat(std::move(opt));
    chat.connect(identity(peer.port()));
    auto step = [&] { chat.update(); peer.poll(); };
    REQUIRE(pump(step, [&] { return !peer.inbox.empty(); }));
    peer.send_reliable(buddy_reply(std::string(256, 'x')));
    REQUIRE(pump(step, [&] { return chat.failed(); }));
    CHECK(chat.error().find("before the login") != std::string::npos);
    REQUIRE(chat.poll());
    CHECK_FALSE(chat.poll());
}

TEST_CASE("UCS late replies from an abandoned endpoint cannot authenticate its replacement") {
    UcsRig rig;
    REQUIRE(rig.start());
    CHECK(rig.chat.send_command("join abandoned"));
    SoeEndpoint next(SoeEndpoint::default_params(), 1);
    rig.chat.connect(identity(next.port()));
    rig.peer.send_reliable(mailbox_reply());
    std::vector<AppPacket> next_received;
    auto step = [&] {
        rig.chat.update();
        rig.peer.poll();
        next.poll();
        while (auto packet = next.take()) next_received.push_back(std::move(*packet));
    };
    REQUIRE(pump(step, [&] { return !next_received.empty(); }));
    CHECK_FALSE(rig.chat.connected());
    next.send_reliable(mailbox_reply("SOE.EQ.test.Hero", 1));
    REQUIRE(pump(step, [&] { return rig.chat.connected(); }));
    CHECK(next_received.size() == 1);
    auto events = rig.events();
    REQUIRE(events.size() == 1);
    CHECK(events.front().fields == std::vector<std::string>{"SOE.EQ.test.Hero"});
}

TEST_CASE("UCS mail integration authenticates, switches boxes, reads and deletes mail, and retires on loss") {
    auto config = MockConfig::standard();
    config.send_chat_server = true;
    Rig rig(std::move(config));
    REQUIRE(rig.to_zone());
    REQUIRE(rig.until([&] { return !rig.server.ucs_log.empty(); }));
    CHECK_FALSE(rig.conn.chat_server_connected());
    CHECK_FALSE(rig.conn.mail_send("Other", "Waiting", "Body"));
    rig.server.ucs.send_reliable(buddy_reply("Friend"));
    auto ignored = strings(0x15, {"SOE.EQ.test.Pest"});
    ignored.data.insert(ignored.data.begin(), std::byte{0});
    rig.server.ucs.send_reliable(ignored);
    rig.server.ucs.send_reliable(mailbox_reply());
    REQUIRE(rig.until([&] { return rig.conn.chat_server_connected() && rig.conn.mail().mailboxes.size() == 2; }));
    CHECK(rig.conn.social().friends == std::vector<std::string>{"Friend"});
    CHECK(rig.conn.social().ignored == std::vector<std::string>{"SOE.EQ.test.Pest"});
    rig.conn.mail_select_mailbox(1);
    rig.server.ucs.send_reliable(strings(0x14, {"1"}));
    REQUIRE(rig.until([&] { return rig.conn.mail().current == 1; }));
    rig.server.ucs.send_reliable(strings(0x0d, {"1", "0", "1", "1"}));
    rig.server.ucs.send_reliable(strings(0x0e, {"1", "0", "0", "42", "1700000000", "2", "SOE.EQ.test.Friend", "Hello"}));
    REQUIRE(rig.until([&] { return rig.conn.mail().headers.contains(42); }));
    CHECK(rig.conn.mail().expected == 1);
    CHECK(rig.conn.mail_request_body(42));
    rig.server.ucs.send_reliable(strings(0x0f, {"1", "42", "Contents", "1", "", "\nTO:SOE.EQ.test.Hero\n"}));
    REQUIRE(rig.until([&] { return rig.conn.mail().headers.at(42).body.has_value(); }));
    CHECK(*rig.conn.mail().headers.at(42).body == "Contents");
    CHECK(rig.conn.mail().headers.at(42).to == "SOE.EQ.test.Hero");
    // A long run of blanks before TO: is cut in one go (it used to be erased a character at a time).
    std::string padded;
    for (int i = 0; i < 100000; ++i) padded += " \n";
    rig.server.ucs.send_reliable(strings(0x0f, {"1", "42", "Again", "1", "", padded + "TO:SOE.EQ.test.Other\n"}));
    REQUIRE(rig.until([&] { return rig.conn.mail().headers.at(42).body == "Again"; }));
    CHECK(rig.conn.mail().headers.at(42).to == "SOE.EQ.test.Other");
    rig.server.ucs.send_reliable(strings(0x0e, {"oops", "0", "0", "7", "1", "2", "Bad", "Wrong box"}));
    rig.server.ucs.send_reliable(strings(0x0e, {"1", "0", "0", "7oops", "1", "2", "Bad", "Wrong id"}));
    rig.server.ucs.send_reliable(strings(0x02, {"general"})); // marker after the rejected inputs
    REQUIRE(rig.until([&] { return rig.conn.chat_channels() == std::vector<std::string>{"general"}; }));
    CHECK(rig.conn.mail().headers.size() == 1);
    rig.conn.mail_delete({42});
    CHECK(rig.conn.mail().headers.at(42).status == game::MailStatus::Trash);
    rig.conn.mail_delete({42});
    CHECK(rig.conn.mail().headers.empty());
    CHECK(rig.conn.mail_send("Other", "Subject", "Body"));
    rig.server.ucs.send_reliable(strings(0x12, {"1", "", "Subject", "1"}));
    REQUIRE(rig.until([&] { return rig.conn.mail().delivery.size() == 1; }));
    rig.server.ucs.send_disconnect();
    REQUIRE(rig.until([&] { return !rig.conn.chat_server_connected() && rig.conn.mail().mailboxes.empty(); }));
    CHECK(rig.conn.state() == ConnectionState::InZone);
    CHECK(rig.conn.mail().headers.empty());
    CHECK(rig.conn.mail().expected == -1);
    CHECK_FALSE(rig.conn.mail_send("Other", "Offline", "Body"));
    // The original's lists belong to the proxy, which ChatServerGiveTime deletes when its link is down.
    CHECK(rig.conn.social().friends.empty());
    CHECK(rig.conn.social().ignored.empty());
}
