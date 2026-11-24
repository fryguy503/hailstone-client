#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <thread>

#include <zlib.h>

#include "core/byte_io.h"
#include "core/log.h"
#include "manual_clock.h"
#include "net/session.h"
#include "net/session_codec.h"
#include "platform/udp_socket.h"

using namespace hailstone;
using namespace hailstone::net;
using soe::Bytes;
using soe::ByteSpan;

namespace {

Bytes b(std::initializer_list<int> v) {
    Bytes out;
    for (int x : v) out.push_back(static_cast<std::byte>(x));
    return out;
}

Bytes str(std::string_view s) {
    auto sp = std::as_bytes(std::span(s.data(), s.size()));
    return Bytes(sp.begin(), sp.end());
}

Bytes cat(Bytes a, const Bytes& c) {
    a.insert(a.end(), c.begin(), c.end());
    return a;
}

// Incompressible-ish bytes (a simple LCG), so fragments stay full size.
Bytes noise(std::size_t n, std::uint32_t seed) {
    Bytes out(n);
    for (auto& x : out) {
        seed = seed * 1103515245u + 12345u;
        x = static_cast<std::byte>(seed >> 24);
    }
    return out;
}

std::uint8_t at(const Bytes& v, std::size_t i) { return std::to_integer<std::uint8_t>(v[i]); }

} // namespace

TEST_CASE("keyed CRC32 matches EQEmu's table CRC (zlib CRC of key bytes + data)") {
    // Reference values: zlib.crc32(struct.pack('<I', key) + data).
    CHECK(soe::crc32({}, 0) == 0x2144df1cu);
    CHECK(soe::crc32(cat(b({0x00, 0x09, 0x00, 0x00}), str("hello")), 0x12345678u) == 0x6850357cu);
    Bytes seq(32);
    for (std::size_t i = 0; i < seq.size(); ++i) seq[i] = static_cast<std::byte>(i);
    CHECK(soe::crc32(seq, 0xdeadbeefu) == 0x19ebfab4u);
}

TEST_CASE("datagram encoding appends a big-endian 16-bit CRC and drops bad packets") {
    soe::SessionParams params;
    params.encode_key = 0x12345678u;
    params.crc_bytes = 2;
    Bytes packet = cat(b({0x00, 0x09, 0x00, 0x00}), str("hello"));
    Bytes wire = soe::encode_datagram(params, packet);
    REQUIRE(wire.size() == packet.size() + 2);
    CHECK(at(wire, wire.size() - 2) == 0x35);
    CHECK(at(wire, wire.size() - 1) == 0x7c);
    auto back = soe::decode_datagram(params, wire);
    REQUIRE(back);
    CHECK(*back == packet);
    wire[5] ^= std::byte{1};
    CHECK_FALSE(soe::decode_datagram(params, wire));
    // Session packets are never encoded.
    Bytes req = soe::make_session_request(7, 512);
    CHECK(soe::encode_datagram(params, req) == req);
}

TEST_CASE("session request and response layout") {
    Bytes req = soe::make_session_request(0x01020304u, 512);
    CHECK(req == b({0, 1, 0, 0, 0, 3, 1, 2, 3, 4, 0, 0, 2, 0}));
    Bytes resp = b({0, 2, 1, 2, 3, 4, 0xca, 0xfe, 0xba, 0xbe, 2, 1, 4, 0, 0, 2, 0});
    auto r = soe::parse_session_response(resp);
    REQUIRE(r);
    CHECK(r->connect_code == 0x01020304u);
    CHECK(r->params.encode_key == 0xcafebabeu);
    CHECK(r->params.crc_bytes == 2);
    CHECK(r->params.passes[0] == soe::EncodePass::Compression);
    CHECK(r->params.passes[1] == soe::EncodePass::Xor);
    CHECK(r->params.max_packet_size == 512);
}

TEST_CASE("compression pass: 0x5a zlib when smaller, 0xa5 otherwise") {
    Bytes small = str("short body");
    Bytes c = soe::compress_body(small);
    CHECK(at(c, 0) == 0xa5);
    CHECK(soe::decompress_body(c) == small);

    Bytes big(2000, std::byte{'x'});
    c = soe::compress_body(big);
    CHECK(at(c, 0) == 0x5a);
    CHECK(c.size() < 100);
    CHECK(soe::decompress_body(c) == big);
    CHECK_FALSE(soe::decompress_body(c, 1000)); // over the size cap

    Bytes rnd = noise(300, 1);
    c = soe::compress_body(rnd);
    CHECK(at(c, 0) == 0xa5);
    CHECK(soe::decompress_body(c) == rnd);
    CHECK_FALSE(soe::decompress_body(b({0x11, 1, 2})));
}

TEST_CASE("compression pass: the reused deflater gives compress2's level 6 bytes, packet after packet") {
    // compress_body keeps one deflater per thread; each body must come out exactly as a fresh compress2 would
    // make it (the server inflates it as zlib data), whatever was compressed before it.
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        Bytes body = noise(31 + seed * 37, seed);
        for (std::size_t i = 0; i < body.size(); i += 3) body[i] = std::byte{'a'}; // compressible
        uLongf len = compressBound(static_cast<uLong>(body.size()));
        Bytes want(len);
        REQUIRE(compress2(reinterpret_cast<Bytef*>(want.data()), &len, reinterpret_cast<const Bytef*>(body.data()),
                          static_cast<uLong>(body.size()), 6) == Z_OK);
        want.resize(len);
        Bytes c = soe::compress_body(body);
        if (len < body.size()) {
            REQUIRE(at(c, 0) == 0x5a);
            CHECK(Bytes(c.begin() + 1, c.end()) == want);
        } else {
            CHECK(at(c, 0) == 0xa5);
        }
        CHECK(soe::decompress_body(c) == body);
        (void)soe::compress_body(noise(500, seed)); // an incompressible body between them
    }
}

TEST_CASE("XOR pass round trip and full encode/decode with both passes") {
    Bytes data = noise(23, 9);
    Bytes x = data;
    soe::xor_encode(x, 0xa1b2c3d4u);
    CHECK(x != data);
    soe::xor_decode(x, 0xa1b2c3d4u);
    CHECK(x == data);

    soe::SessionParams params;
    params.encode_key = 0x55aa1234u;
    params.crc_bytes = 2;
    params.passes = {soe::EncodePass::Compression, soe::EncodePass::Xor};
    for (const Bytes& p : {cat(b({0, 9, 0, 1}), Bytes(400, std::byte{7})), cat(b({0x29, 0x00}), noise(40, 3)), b({0, 6})}) {
        auto wire = soe::encode_datagram(params, p);
        auto back = soe::decode_datagram(params, wire);
        REQUIRE(back);
        CHECK(*back == p);
    }
}

TEST_CASE("combined and app-combined packets split into their parts") {
    Bytes a = b({0, 0x15, 0, 5}), c = cat(b({0x29, 0x00}), str("abc"));
    Bytes comb = soe::make_combined(std::vector<Bytes>{a, c});
    CHECK(comb == cat(cat(b({0, 3, 4}), a), cat(b({5}), c)));
    auto parts = soe::split_combined(comb);
    REQUIRE(parts);
    REQUIRE(parts->size() == 2);
    CHECK(Bytes((*parts)[0].begin(), (*parts)[0].end()) == a);
    CHECK(Bytes((*parts)[1].begin(), (*parts)[1].end()) == c);
    comb.pop_back();
    CHECK_FALSE(soe::split_combined(comb));

    Bytes long_part = noise(300, 4);
    Bytes app = cat(cat(b({0, 0x19, 3}), str("xyz")), cat(b({0xff, 0x01, 0x2c}), long_part));
    auto ap = soe::split_app_combined(app);
    REQUIRE(ap);
    REQUIRE(ap->size() == 2);
    CHECK((*ap)[0].size() == 3);
    CHECK(Bytes((*ap)[1].begin(), (*ap)[1].end()) == long_part);
    Bytes huge = cat(b({0, 0x19, 0xff, 0xff, 0xff, 0, 0, 0, 2}), str("hi"));
    ap = soe::split_app_combined(huge);
    REQUIRE(ap);
    CHECK((*ap)[0].size() == 2);
    CHECK_FALSE(soe::split_app_combined(b({0, 0x19, 0xff, 0x00})));
}

TEST_CASE("application opcodes: little-endian with a 0x00 escape when the low byte is 0") {
    CHECK(soe::encode_app(AppPacket(0x7a09, b({1}))) == b({0x09, 0x7a, 1}));
    CHECK(soe::encode_app(AppPacket(0x0100, b({2}))) == b({0x00, 0x00, 0x01, 2}));
    auto p = soe::decode_app(b({0x00, 0x00, 0x01, 2}));
    REQUIRE(p);
    CHECK(p->opcode == 0x0100);
    CHECK(p->data == b({2}));
    p = soe::decode_app(b({0x22, 0x00}));
    REQUIRE(p);
    CHECK(p->opcode == 0x0022);
    CHECK(p->data.empty());
    CHECK_FALSE(soe::decode_app(b({0x00, 0x05, 1})));
}

TEST_CASE("application opcodes: decoding from a buffer given up reads the same as from a span") {
    for (const Bytes& wire : {b({0x00, 0x00, 0x01, 2}), b({0x22, 0x00}), b({0x09, 0x7a, 1, 2, 3}), b({0x00, 0x05, 1}),
                              b({0x00}), b({}), b({0x00, 0x00})}) {
        for (int opcode_bytes : {1, 2}) {
            CAPTURE(opcode_bytes);
            auto from_span = soe::decode_app(ByteSpan(wire), opcode_bytes);
            auto from_buffer = soe::decode_app(Bytes(wire), opcode_bytes);
            REQUIRE(from_span.has_value() == from_buffer.has_value());
            if (!from_span) continue;
            CHECK(from_span->opcode == from_buffer->opcode);
            CHECK(from_span->data == from_buffer->data);
        }
    }
}

TEST_CASE("sequence comparison wraps at 16 bits") {
    CHECK(soe::compare_sequence(5, 5) == soe::SeqOrder::Current);
    CHECK(soe::compare_sequence(65535, 0) == soe::SeqOrder::Future);
    CHECK(soe::compare_sequence(0, 65535) == soe::SeqOrder::Past);
    CHECK(soe::compare_sequence(65530, 3) == soe::SeqOrder::Future);
    CHECK(soe::sequence_at_or_before(65534, 1));
    CHECK_FALSE(soe::sequence_at_or_before(2, 65534));
}

TEST_CASE("fragments reassemble in order, out of order and across the sequence wrap") {
    soe::SessionParams params;
    params.crc_bytes = 2;
    params.passes = {soe::EncodePass::Compression, soe::EncodePass::None};
    Bytes payload = noise(2500, 77);
    std::uint16_t seq = 65533;
    auto packets = soe::make_reliable(payload, seq, params);
    REQUIRE(packets.size() == 5);
    CHECK(seq == 2);
    for (const auto& p : packets) CHECK(p.size() + params.overhead() <= 512);
    CHECK(at(packets[0], 1) == soe::op::Fragment);
    // First fragment: sequence 65533, total length 2500 (BE).
    CHECK(packets[0][2] == std::byte{0xff});
    CHECK(packets[0][3] == std::byte{0xfd});
    CHECK(at(packets[0], 6) == 0x09);
    CHECK(at(packets[0], 7) == 0xc4);

    SUBCASE("in order") {
        soe::InboundStream in(65533);
        std::vector<Bytes> out;
        for (const auto& p : packets) CHECK(in.push(p, out) == soe::SeqOrder::Current);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == payload);
        CHECK(in.next() == 2);
    }
    SUBCASE("reversed, with a duplicate") {
        soe::InboundStream in(65533);
        std::vector<Bytes> out;
        for (std::size_t i = packets.size(); i-- > 1;) CHECK(in.push(packets[i], out) == soe::SeqOrder::Future);
        CHECK(out.empty());
        CHECK(in.push(packets[3], out) == soe::SeqOrder::Future);
        CHECK(in.push(packets[0], out) == soe::SeqOrder::Current);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == payload);
        CHECK(in.push(packets[2], out) == soe::SeqOrder::Past);
        CHECK(in.buffered() == 0);
    }

    Bytes small = str("fits in one");
    auto single = soe::make_reliable(small, seq, params);
    REQUIRE(single.size() == 1);
    CHECK(single[0] == cat(b({0, 9, 0, 2}), small));
}

TEST_CASE("a fragmented payload over the cap is counted and dropped; the stream goes on after it") {
    auto frag = [](std::uint16_t seq, std::size_t n, std::optional<std::uint32_t> total) {
        Bytes p = b({0, soe::op::Fragment, seq >> 8, seq & 0xff});
        if (total) p = cat(p, b({int(*total >> 24), int(*total >> 16 & 0xff), int(*total >> 8 & 0xff), int(*total & 0xff)}));
        return cat(p, Bytes(n, std::byte{0x41}));
    };
    const std::uint32_t total = soe::kMaxFragmentedPayload + 1;
    const std::size_t piece = 1u << 20;
    soe::InboundStream in(0);
    std::vector<Bytes> out;
    std::uint16_t seq = 0;
    std::size_t sent = 0;
    CHECK(in.push(frag(seq++, piece, total), out) == soe::SeqOrder::Current);
    for (sent = piece; sent < total; sent += piece) {
        std::size_t n = std::min<std::size_t>(piece, total - sent);
        in.push(frag(seq++, n, std::nullopt), out);
    }
    CHECK(out.empty());
    // The next payload, fragmented and whole, arrives intact.
    Bytes small = noise(100, 3);
    CHECK(in.push(cat(frag(seq++, 0, 100), Bytes(small.begin(), small.begin() + 40)), out) == soe::SeqOrder::Current);
    CHECK(in.push(cat(frag(seq++, 0, std::nullopt), Bytes(small.begin() + 40, small.end())), out) ==
          soe::SeqOrder::Current);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == small);
}

TEST_CASE("early packets are bounded in bytes; one without room is not held and is taken when it comes again") {
    auto packet = [](std::uint16_t seq, std::size_t n, std::uint8_t fill) {
        return cat(b({0, soe::op::Packet, seq >> 8, seq & 0xff}), Bytes(n, std::byte{fill}));
    };
    // Each early packet is a little over 1 MiB (as a reassembled payload that is itself a 00 09 can be): three fit
    // under kMaxBufferedBytes, the fourth does not.
    const std::size_t big = 1u << 20;
    soe::InboundStream in(0);
    std::vector<Bytes> out;
    for (std::uint16_t seq = 1; seq <= 4; ++seq) CHECK(in.push(packet(seq, big, std::uint8_t(seq)), out) == soe::SeqOrder::Future);
    CHECK(in.buffered() == 3);
    CHECK(in.buffered_bytes() == 3 * (big + 4));
    CHECK(in.buffered_bytes() <= soe::kMaxBufferedBytes);
    CHECK(in.holds(1));
    CHECK(in.holds(3));
    CHECK_FALSE(in.holds(4)); // the session sends no OutOfOrderAck for it, so the server sends it again
    // A duplicate of a held one is still held (and acknowledged again), and counted once.
    CHECK(in.push(packet(2, big, 2), out) == soe::SeqOrder::Future);
    CHECK(in.buffered_bytes() == 3 * (big + 4));
    // The expected packet always goes through and frees the room.
    CHECK(in.push(packet(0, 10, 0), out) == soe::SeqOrder::Current);
    REQUIRE(out.size() == 4);
    CHECK(in.buffered() == 0);
    CHECK(in.buffered_bytes() == 0);
    CHECK(in.next() == 4);
    CHECK(in.push(packet(4, big, 4), out) == soe::SeqOrder::Current);
    REQUIRE(out.size() == 5);
    CHECK(out[4] == Bytes(big, std::byte{4}));
}

TEST_CASE("a first fragment too short for its total length breaks the stream: nothing more is delivered") {
    auto frag = [](std::uint16_t seq, const Bytes& body) {
        return cat(b({0, soe::op::Fragment, seq >> 8, seq & 0xff}), body);
    };
    soe::InboundStream in(0);
    std::vector<Bytes> out;
    std::uint16_t seq = 0;
    Bytes before = str("before");
    CHECK(in.push(cat(b({0, soe::op::Packet, 0, seq++}), before), out) == soe::SeqOrder::Current);
    CHECK_FALSE(in.broken());
    CHECK(in.push(frag(seq++, b({0, 0})), out) == soe::SeqOrder::Current); // no room for the 4-byte total
    CHECK(in.broken());
    // Neither the rest of that payload (its first bytes would be taken as a length) nor anything after it, whole or
    // fragmented: which pieces belong to which payload can no longer be told, and a silent gap would desync the game.
    CHECK(in.push(frag(seq++, b({0, 0, 0, 3, 0x41, 0x42, 0x43})), out) == soe::SeqOrder::Current);
    CHECK(in.push(cat(b({0, soe::op::Packet, 0, seq++}), str("whole")), out) == soe::SeqOrder::Current);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == before);
    CHECK(in.broken());
}

TEST_CASE("a fragment that runs past its payload's total breaks the stream instead of being cut short") {
    auto frag = [](std::uint16_t seq, const Bytes& body) {
        return cat(b({0, soe::op::Fragment, seq >> 8, seq & 0xff}), body);
    };
    SUBCASE("a first fragment longer than its own total") {
        soe::InboundStream in(0);
        std::vector<Bytes> out;
        CHECK(in.push(frag(0, b({0, 0, 0, 3, 0x41, 0x42, 0x43, 0x44, 0x45})), out) == soe::SeqOrder::Current);
        CHECK(in.broken());
        // The two extra bytes are not read as the start of a new payload, nor anything after.
        CHECK(in.push(frag(1, b({0, 0, 0, 2, 0x46, 0x47})), out) == soe::SeqOrder::Current);
        CHECK(out.empty());
    }
    SUBCASE("a later piece past the total") {
        soe::InboundStream in(0);
        std::vector<Bytes> out;
        CHECK(in.push(frag(0, b({0, 0, 0, 4, 0x41, 0x42})), out) == soe::SeqOrder::Current);
        CHECK_FALSE(in.broken());
        CHECK(in.push(frag(1, b({0x43, 0x44, 0x45})), out) == soe::SeqOrder::Current);
        CHECK(in.broken());
        CHECK(out.empty());
    }
    SUBCASE("pieces that add up to the total exactly are delivered") {
        soe::InboundStream in(0);
        std::vector<Bytes> out;
        CHECK(in.push(frag(0, b({0, 0, 0, 4, 0x41, 0x42})), out) == soe::SeqOrder::Current);
        CHECK(in.push(frag(1, b({0x43, 0x44})), out) == soe::SeqOrder::Current);
        CHECK_FALSE(in.broken());
        REQUIRE(out.size() == 1);
        CHECK(out[0] == b({0x41, 0x42, 0x43, 0x44}));
    }
}

TEST_CASE("decompress_body reuses its inflater: results stay right after a failure and between sizes") {
    Bytes big = noise(4000, 5);
    for (auto& x : big) x = std::byte{static_cast<std::uint8_t>(std::to_integer<int>(x) & 0x0f)}; // compressible
    Bytes c = soe::compress_body(big);
    REQUIRE(std::to_integer<int>(c[0]) == 0x5a);
    CHECK(soe::decompress_body(c) == big);
    CHECK_FALSE(soe::decompress_body(cat(b({0x5a}), b({1, 2, 3, 4, 5})))); // bad zlib data
    Bytes half(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(c.size() / 2));
    CHECK_FALSE(soe::decompress_body(half)); // cut short
    CHECK_FALSE(soe::decompress_body(c, 1000));
    CHECK(soe::decompress_body(c, 4000) == big); // exactly the cap
    CHECK_FALSE(soe::decompress_body(c, 3999));
    Bytes small = str("a short body that compresses well well well well well well well");
    Bytes cs = soe::compress_body(small);
    REQUIRE(std::to_integer<int>(cs[0]) == 0x5a);
    CHECK(soe::decompress_body(cs) == small);
    CHECK(soe::decompress_body(c) == big);
}

TEST_CASE("udp socket: a datagram larger than the buffer is dropped, not cut short") {
    std::string err;
    auto local = platform::Endpoint::v4(127, 0, 0, 1, 0);
    platform::UdpSocket a, rx;
    REQUIRE(a.open(platform::Endpoint::Family::V4, err, &local));
    REQUIRE(rx.open(platform::Endpoint::Family::V4, err, &local));
    CHECK_FALSE(rx.inherited_by_children()); // close-on-exec (Windows: not inheritable)
    auto to = *rx.local_endpoint();
    REQUIRE(a.send_to(to, noise(200, 1)));
    Bytes fits = noise(50, 2);
    REQUIRE(a.send_to(to, fits));
    std::array<std::byte, 100> buf;
    platform::Endpoint from;
    REQUIRE(rx.wait_readable(std::chrono::milliseconds(2000)));
    std::optional<std::size_t> n;
    // Loopback delivers both at once in practice; allow for a slow scheduler without sleeping on a fixed time.
    for (int i = 0; i < 200 && !n; ++i) {
        n = rx.recv_from(buf, from);
        if (!n) rx.wait_readable(std::chrono::milliseconds(10));
    }
    REQUIRE(n);
    CHECK(*n == fits.size());
    CHECK(Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)) == fits);
    CHECK(rx.last_error().empty());
}

TEST_CASE("udp socket: one recv_from skips a bounded number of dropped datagrams, then returns nothing") {
    std::string err;
    auto local = platform::Endpoint::v4(127, 0, 0, 1, 0);
    platform::UdpSocket a, rx;
    REQUIRE(a.open(platform::Endpoint::Family::V4, err, &local));
    REQUIRE(rx.open(platform::Endpoint::Family::V4, err, &local));
    auto to = *rx.local_endpoint();
    constexpr int kDropped = platform::UdpSocket::kMaxSkippedPerRecv + 6;
    for (int i = 0; i < kDropped; ++i) REQUIRE(a.send_to(to, noise(200, static_cast<std::uint32_t>(i))));
    Bytes fits = noise(50, 2);
    REQUIRE(a.send_to(to, fits));
    std::array<std::byte, 100> buf;
    platform::Endpoint from;
    REQUIRE(rx.wait_readable(std::chrono::milliseconds(2000)));
    int empty_calls = 0;
    std::optional<std::size_t> n;
    for (int i = 0; i < 400 && !n; ++i) {
        n = rx.recv_from(buf, from);
        if (!n) {
            ++empty_calls;
            CHECK(rx.last_error().empty());
            rx.wait_readable(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(n);
    CHECK(*n == fits.size());
    CHECK(empty_calls >= 1); // the first call gave up after kMaxSkippedPerRecv drops
}

// --- loopback: the real Session against a minimal fake server -------------

namespace {

struct FakeServer {
    platform::UdpSocket sock;
    platform::Endpoint client;
    soe::SessionParams params;
    bool have_client = false;
    std::uint32_t connect_code = 0;
    soe::InboundStream inbound;
    std::vector<AppPacket> received;
    std::set<std::uint16_t> acked;
    std::uint16_t last_cumulative_ack = 0xffff;
    int reliable_from_client = 0;
    int datagrams = 0; // decoded datagrams from the client, handshake excluded

    FakeServer() {
        std::string err;
        auto local = platform::Endpoint::v4(127, 0, 0, 1, 0);
        REQUIRE(sock.open(platform::Endpoint::Family::V4, err, &local));
        params.encode_key = 0x0badf00du;
        params.crc_bytes = 2;
        params.passes = {soe::EncodePass::Compression, soe::EncodePass::None};
        params.max_packet_size = 512;
    }

    std::uint16_t port() const { return sock.local_endpoint()->port; }

    void send(const Bytes& packet) { sock.send_to(client, soe::encode_datagram(params, packet)); }

    // drop_first_reliable: ignore the client's first reliable packet so it must resend.
    void poll(bool drop_first_reliable) {
        std::array<std::byte, 4096> buf;
        platform::Endpoint from;
        while (auto n = sock.recv_from(buf, from)) {
            ByteSpan d(buf.data(), *n);
            if (std::to_integer<int>(d[0]) == 0 && std::to_integer<int>(d[1]) == soe::op::SessionRequest) {
                REQUIRE(d.size() == 14);
                client = from;
                have_client = true;
                ByteReader request(d.subspan(6, 4));
                connect_code = request.be<std::uint32_t>();
                ByteWriter w;
                w.le<std::uint8_t>(0);
                w.le<std::uint8_t>(soe::op::SessionResponse);
                w.bytes(d.subspan(6, 4)); // connect code
                w.be<std::uint32_t>(params.encode_key);
                w.le<std::uint8_t>(params.crc_bytes);
                w.le<std::uint8_t>(1);
                w.le<std::uint8_t>(0);
                w.be<std::uint32_t>(params.max_packet_size);
                sock.send_to(client, w.data());
                continue;
            }
            auto p = soe::decode_datagram(params, d);
            REQUIRE(p);
            ++datagrams;
            handle(*p, drop_first_reliable);
        }
    }

    void handle(ByteSpan p, bool drop_first_reliable) {
        if (std::to_integer<int>(p[0]) != 0 || std::to_integer<int>(p[1]) == 0) {
            received.push_back(*soe::decode_app(p));
            return;
        }
        int op = std::to_integer<int>(p[1]);
        auto seq = static_cast<std::uint16_t>(std::to_integer<int>(p[2]) << 8 | std::to_integer<int>(p[3]));
        if (op == soe::op::Combined) {
            auto parts = soe::split_combined(p);
            REQUIRE(parts);
            for (auto part : *parts) handle(part, drop_first_reliable);
        } else if (op == soe::op::Ack) {
            last_cumulative_ack = seq;
        } else if (op == soe::op::OutOfOrderAck) {
            acked.insert(seq);
        } else if (op == soe::op::Packet) {
            if (reliable_from_client++ == 0 && drop_first_reliable) return;
            std::vector<Bytes> out;
            if (inbound.push(p, out) != soe::SeqOrder::Future) send(soe::make_ack(soe::op::Ack, static_cast<std::uint16_t>(inbound.next() - 1)));
            for (auto& o : out) received.push_back(*soe::decode_app(o));
        }
    }

    bool is_acked(std::uint16_t seq) const {
        return acked.count(seq) || (last_cumulative_ack != 0xffff && soe::sequence_at_or_before(seq, last_cumulative_ack));
    }

    void connect(Session& session) {
        session.connect("127.0.0.1", port());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (session.state() == SessionState::Connecting && std::chrono::steady_clock::now() < deadline) {
            session.update();
            poll(false);
            std::this_thread::yield();
        }
        REQUIRE(session.state() == SessionState::Connected);
        REQUIRE(have_client);
    }
};

} // namespace

TEST_CASE("session: only an exact current-session peer disconnect has peer provenance") {
    FakeServer server;
    SessionOptions options;
    options.service_interval = std::chrono::milliseconds(0);
    auto session = Session::create(options);
    CHECK_FALSE(session->peer_disconnected());
    server.connect(*session);
    CHECK_FALSE(session->peer_disconnected());
    auto packet = soe::make_disconnect(server.connect_code);
    bool valid = true;
    SUBCASE("matching six-byte packet") {}
    SUBCASE("another session's code") {
        packet = soe::make_disconnect(server.connect_code ^ 0x80000000u);
        valid = false;
    }
    SUBCASE("header alone") {
        packet.resize(2);
        valid = false;
    }
    SUBCASE("partial code") {
        packet.resize(5);
        valid = false;
    }
    SUBCASE("extra byte") {
        packet.push_back(std::byte{});
        valid = false;
    }
    SUBCASE("eight-byte size-helper padding") {
        packet.resize(8);
        valid = false;
    }
    SUBCASE("out of session is not a validated peer disconnect") {
        packet = b({0, soe::op::OutOfSession});
        valid = false;
    }
    server.send(packet);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session->state() != SessionState::Disconnected && std::chrono::steady_clock::now() < deadline) {
        session->update();
        std::this_thread::yield();
    }
    REQUIRE(session->state() == SessionState::Disconnected); // Existing close behavior is unchanged.
    CHECK(session->peer_disconnected() == valid);
}

TEST_CASE("session: corrupt disconnects do not establish peer provenance") {
    FakeServer server;
    SessionOptions options;
    options.service_interval = std::chrono::milliseconds(0);
    auto session = Session::create(options);
    server.connect(*session);
    auto wire = soe::encode_datagram(server.params, soe::make_disconnect(server.connect_code));
    REQUIRE_FALSE(wire.empty());
    wire.back() ^= std::byte{1};
    server.sock.send_to(server.client, wire);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session->stats().bad == 0 && std::chrono::steady_clock::now() < deadline) {
        session->update();
        std::this_thread::yield();
    }
    REQUIRE(session->stats().bad == 1);
    CHECK(session->state() == SessionState::Connected);
    CHECK_FALSE(session->peer_disconnected());
}

TEST_CASE("session: local close and connection loss are not peer disconnects") {
    FakeServer server;
    test::ManualClock clock;
    clock.hold();
    SessionOptions options;
    options.clock = clock.source();
    options.service_interval = std::chrono::milliseconds(0);
    options.idle_timeout = std::chrono::milliseconds(100);
    options.keepalive_interval = options.stat_interval = std::chrono::milliseconds(0);
    auto session = Session::create(options);
    server.connect(*session);
    SUBCASE("local disconnect") { session->disconnect(); }
    SUBCASE("receive timeout") { clock.advance(std::chrono::milliseconds(100)); }
    SUBCASE("closed socket") { session->simulate_socket_loss(); }
    SUBCASE("lost network even when a peer packet was sent") {
        session->simulate_link_loss();
        server.send(soe::make_disconnect(server.connect_code));
        session->update();
        CHECK(session->state() == SessionState::Connected);
        CHECK_FALSE(session->peer_disconnected());
        clock.advance(std::chrono::milliseconds(100));
    }
    session->update();
    CHECK(session->state() == SessionState::Disconnected);
    CHECK_FALSE(session->peer_disconnected());
}

TEST_CASE("session: reconnect clears a previous peer-disconnect receipt") {
    FakeServer server;
    SessionOptions options;
    options.service_interval = std::chrono::milliseconds(0);
    auto session = Session::create(options);
    server.connect(*session);
    server.send(soe::make_disconnect(server.connect_code));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session->state() != SessionState::Disconnected && std::chrono::steady_clock::now() < deadline) {
        session->update();
        std::this_thread::yield();
    }
    REQUIRE(session->state() == SessionState::Disconnected);
    REQUIRE(session->peer_disconnected());
    server.connect(*session);
    CHECK_FALSE(session->peer_disconnected());
    session->disconnect();
    session->update();
    CHECK(session->state() == SessionState::Disconnected);
    CHECK_FALSE(session->peer_disconnected());
}

TEST_CASE("session loopback: handshake, resend in both directions, fragment reassembly") {
    FakeServer server;
    auto session = Session::create();
    session->connect("127.0.0.1", server.port());
    CHECK(session->state() == SessionState::Connecting);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    auto tick = [&](bool drop) {
        session->update();
        server.poll(drop);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return std::chrono::steady_clock::now() < deadline;
    };
    while (session->state() == SessionState::Connecting && tick(true)) {}
    REQUIRE(session->state() == SessionState::Connected);

    // Client -> server reliable packet; the server drops the first copy.
    session->send(AppPacket(0x0100, str("ping")));
    session->send(AppPacket(0x4242, str("move")), false);
    while (server.received.size() < 2 && tick(true)) {}
    REQUIRE(server.received.size() == 2);
    // The unreliable one arrives first; the reliable one only after the resend.
    CHECK(server.received[0].opcode == 0x4242);
    CHECK(server.received[1].opcode == 0x0100);
    CHECK(server.received[1].data == str("ping"));
    CHECK(server.reliable_from_client >= 2);

    // Server -> client: a fragmented reliable packet with one datagram lost.
    Bytes big = noise(3000, 5);
    std::uint16_t seq = 0;
    auto frags = soe::make_reliable(soe::encode_app(AppPacket(0x1234, big)), seq, server.params);
    REQUIRE(frags.size() > 3);
    for (std::size_t i = 0; i < frags.size(); ++i)
        if (i != 2) server.send(frags[i]);
    // Wait until everything but the lost fragment is acked out of order, then resend it.
    while (!(server.is_acked(static_cast<std::uint16_t>(frags.size() - 1))) && tick(false)) {}
    CHECK_FALSE(server.is_acked(2));
    CHECK_FALSE(session->receive().has_value());
    server.send(frags[2]);

    std::optional<AppPacket> got;
    while (!got && tick(false)) got = session->receive();
    REQUIRE(got);
    CHECK(got->opcode == 0x1234);
    CHECK(got->data == big);
    while (!server.is_acked(static_cast<std::uint16_t>(frags.size() - 1)) && tick(false)) {}
    while (server.last_cumulative_ack != frags.size() - 1 && tick(false)) {}
    CHECK(server.last_cumulative_ack == frags.size() - 1);

    session->disconnect();
    for (int i = 0; i < 50 && session->state() != SessionState::Disconnected; ++i) tick(false);
    CHECK(session->state() == SessionState::Disconnected);
}

TEST_CASE("session: a server asking for a CRC size other than 0, 2 or 4 is refused at connect") {
    FakeServer server;
    server.params.crc_bytes = 3;
    auto session = Session::create();
    session->connect("127.0.0.1", server.port());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (session->state() == SessionState::Connecting && std::chrono::steady_clock::now() < deadline) {
        session->update();
        server.poll(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(session->state() == SessionState::Disconnected);
    CHECK(session->error().find("3-byte CRC") != std::string::npos);
}

TEST_CASE("session times out when nobody answers") {
    platform::UdpSocket silent;
    std::string err;
    auto local = platform::Endpoint::v4(127, 0, 0, 1, 0);
    REQUIRE(silent.open(platform::Endpoint::Family::V4, err, &local));
    SessionOptions opt;
    opt.connect_timeout = std::chrono::milliseconds(300);
    auto session = Session::create(opt);
    session->connect("127.0.0.1", silent.local_endpoint()->port);
    for (int i = 0; i < 200 && session->state() == SessionState::Connecting; ++i) {
        session->update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(session->state() == SessionState::Disconnected);
    CHECK_FALSE(session->error().empty());
    CHECK_FALSE(session->peer_disconnected());
}

TEST_CASE("session loopback: the service thread acks while the owner is busy; small packets are held") {
    FakeServer server;
    test::ManualClock clock;
    SessionOptions held_opt;
    held_opt.clock = clock.source();
    auto session = Session::create(held_opt);
    session->connect("127.0.0.1", server.port());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    auto alive = [&] { return std::chrono::steady_clock::now() < deadline; };
    while (session->state() == SessionState::Connecting && alive()) {
        session->update();
        server.poll(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(session->state() == SessionState::Connected);

    // The owner is stuck (a zone load) and calls nothing; the session still acknowledges what arrives
    // (UdpManagerThread), so the server never has to resend.
    std::uint16_t seq = 0;
    for (int i = 0; i < 5; ++i)
        for (auto& p : soe::make_reliable(soe::encode_app(AppPacket(0x0100, str("load"))), seq, server.params)) server.send(p);
    while (server.last_cumulative_ack != 4 && alive()) {
        server.poll(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(server.last_cumulative_ack == 4);
    int got = 0;
    while (session->receive()) ++got;
    CHECK(got == 5);

    // Two small packets sent together wait in the multi-buffer for the 150 ms hold and leave in one datagram,
    // without another update(). The session's clock is held, so a stall between the two sends cannot let the first
    // go alone; the service thread's passes (every 20 ms of real time) see only the time advanced here.
    server.poll(false);
    clock.hold();
    int before = server.datagrams;
    session->send(AppPacket(0x4242, str("one")), false);
    session->send(AppPacket(0x4243, str("two")), false);
    std::size_t first = server.received.size();
    clock.advance(std::chrono::milliseconds(149));
    for (int i = 0; i < 25; ++i) { // several service passes
        server.poll(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(server.received.size() == first);
    clock.advance(std::chrono::milliseconds(1));
    while (server.received.size() < first + 2 && alive()) {
        server.poll(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(server.received.size() == first + 2);
    CHECK(server.received[first].opcode == 0x4242);
    CHECK(server.received[first + 1].opcode == 0x4243);
    CHECK(server.datagrams == before + 1);

    // With no hold (the login server's UdpManagers) the next service pass sends it.
    SessionOptions opt;
    opt.hold_time = std::chrono::milliseconds(0);
    FakeServer login;
    auto quick = Session::create(opt);
    quick->connect("127.0.0.1", login.port());
    while (quick->state() == SessionState::Connecting && alive()) {
        login.poll(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(quick->state() == SessionState::Connected);
    auto sent = std::chrono::steady_clock::now();
    quick->send(AppPacket(0x4242, str("now")), false);
    while (login.received.empty() && alive()) {
        login.poll(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(login.received.size() == 1);
    // A loaded CI runner can stall this for over 150 ms, so the bound only separates it from a stuck send.
    CHECK(std::chrono::steady_clock::now() - sent < std::chrono::milliseconds(2000));

    session->disconnect();
    quick->disconnect();
    for (int i = 0; i < 100 && (session->state() != SessionState::Disconnected || quick->state() != SessionState::Disconnected); ++i) {
        server.poll(false);
        login.poll(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(session->state() == SessionState::Disconnected);
    CHECK(quick->state() == SessionState::Disconnected);
}

TEST_CASE("platform::resolve_async: a numeric address on the lookup thread, several lookups one after another") {
    // ChatServer::connect looks the chat server's name up this way. Each lookup's thread is kept (joined at exit,
    // never cut off inside the resolver); finished ones are joined as the next starts.
    for (int i = 0; i < 3; ++i) {
        auto result = std::make_shared<std::promise<std::pair<std::vector<platform::Endpoint>, std::string>>>();
        auto got = result->get_future();
        platform::resolve_async("127.0.0.1", static_cast<std::uint16_t>(9000 + i),
                                [result](std::vector<platform::Endpoint> endpoints, std::string error) {
                                    result->set_value({std::move(endpoints), std::move(error)});
                                });
        REQUIRE(got.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        auto [endpoints, error] = got.get();
        CHECK(error.empty());
        REQUIRE(endpoints.size() == 1);
        CHECK(endpoints[0] == platform::Endpoint::v4(127, 0, 0, 1, static_cast<std::uint16_t>(9000 + i)));
    }
}

TEST_CASE("udp socket: last_error is the last call's, and open() clears it") {
    std::string err;
    auto local = platform::Endpoint::v4(127, 0, 0, 1, 0);
    platform::UdpSocket a;
    REQUIRE(a.open(platform::Endpoint::Family::V4, err, &local));
    // An IPv6 address from an IPv4 socket: refused by the system on every platform.
    platform::Endpoint v6;
    v6.family = platform::Endpoint::Family::V6;
    v6.address[15] = 1;
    v6.port = 9;
    CHECK_FALSE(a.send_to(v6, noise(10, 1)));
    CHECK_FALSE(a.last_error().empty());
    CHECK(a.send_to(*a.local_endpoint(), noise(10, 2)));
    CHECK(a.last_error().empty());
    CHECK_FALSE(a.send_to(v6, noise(10, 1)));
    std::array<std::byte, 64> buf;
    platform::Endpoint from;
    a.wait_readable(std::chrono::milliseconds(2000));
    CHECK(a.recv_from(buf, from));
    CHECK(a.last_error().empty());
    CHECK_FALSE(a.recv_from(buf, from)); // nothing waiting: no error either
    CHECK(a.last_error().empty());
    CHECK_FALSE(a.send_to(v6, noise(10, 1)));
    REQUIRE(a.open(platform::Endpoint::Family::V4, err, &local));
    CHECK(a.last_error().empty());
}

TEST_CASE("endpoint: an IPv6 link-local address keeps its interface both ways") {
    auto ep = platform::numeric_endpoint("fe80::1%1", 5000);
    REQUIRE(ep);
    CHECK(ep->family == platform::Endpoint::Family::V6);
    CHECK(ep->scope_id == 1);
    CHECK(ep->to_string() == "[fe80:0:0:0:0:0:0:1%1]:5000");
    // The text form reads back the same, as ChatServer passes address_string() on.
    auto back = platform::numeric_endpoint(ep->address_string(), 5000);
    REQUIRE(back);
    CHECK(*back == *ep);
    auto plain = platform::numeric_endpoint("::1", 5000);
    REQUIRE(plain);
    CHECK(plain->scope_id == 0);
    CHECK(plain->to_string() == "[0:0:0:0:0:0:0:1]:5000");

    // A socket's own address carries it too: bound to ::1 there is none.
    std::string err;
    platform::UdpSocket s;
    if (!s.open(platform::Endpoint::Family::V6, err, &*plain)) {
        MESSAGE("no IPv6 loopback here: " << err);
        return;
    }
    CHECK(s.local_endpoint()->scope_id == 0);
}

TEST_CASE("platform::resolve_async: at most four lookups run at once; the rest wait and all finish") {
    // Each `done` blocks its thread until released, so the threads in use can be counted.
    std::atomic<int> inside{0}, most{0}, finished{0};
    std::promise<void> open_gate;
    std::shared_future<void> gate = open_gate.get_future().share();
    constexpr int kLookups = 9;
    for (int i = 0; i < kLookups; ++i)
        platform::resolve_async("127.0.0.1", 9100, [&, gate](std::vector<platform::Endpoint>, std::string) {
            int now = ++inside;
            int seen = most.load();
            while (now > seen && !most.compare_exchange_weak(seen, now)) {
            }
            gate.wait();
            --inside;
            ++finished;
        });
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (inside.load() < 4 && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    CHECK(inside.load() == 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // a fifth would have started by now
    CHECK(most.load() == 4);
    open_gate.set_value();
    until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (finished.load() < kLookups && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    CHECK(finished.load() == kLookups);
    CHECK(most.load() == 4);
}

TEST_CASE("session: a send the network refuses is logged once an outage, not once a datagram") {
    // A link-local address on an interface that does not exist: every SessionRequest the session sends fails.
    test::ManualClock clock;
    SessionOptions o;
    o.clock = clock.source();
    auto session = Session::create(o);
    clock.hold();
    session->connect("fe80::1%4000000", 9);
    session->update();
    if (session->state() == SessionState::Disconnected) {
        MESSAGE("no IPv6 here: " << session->error());
        return;
    }
    for (int i = 0; i < 4; ++i) {
        clock.advance(std::chrono::milliseconds(1100)); // the SessionRequest is sent again each second
        session->update();
    }
    char recent[4096];
    log::copy_recent_lines(recent, sizeof recent);
    std::string_view lines(recent);
    std::size_t count = 0;
    for (auto at = lines.find("further send errors are counted"); at != std::string_view::npos;
         at = lines.find("further send errors are counted", at + 1))
        ++count;
    if (count == 0) {
        MESSAGE("this system took the datagrams: the outage was not exercised");
        return;
    }
    CHECK(count == 1);
}
