// Fuzz-style tests for the network parsers: every typed per-opcode decoder, the SOE session codec (datagram decode,
// decompression, combined/fragment splitting and reassembly, session response) and the login-server reply parsers.
// Inputs are deterministic: truncations of fixed patterns at many lengths plus seeded mutations. Run the suite with
// -DHAILSTONE_SANITIZE=ON to also catch out-of-bounds reads and undefined behaviour.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>

#include "core/byte_io.h"
#include "fuzz_game_decoders.h"
#include "fuzz_achievements.h"
#include "fuzz_support.h"
#include "net/aa_packets.h"
#include "net/achievement_packets.h"
#include "net/ability_packets.h"
#include "net/adventure_packets.h"
#include "net/aggro_packets.h"
#include "net/augment_packets.h"
#include "net/chat_server.h"
#include "net/door_packets.h"
#include "net/dz_packets.h"
#include "net/group_packets.h"
#include "net/fellowship_packets.h"
#include "net/guild_bank_packets.h"
#include "net/item_packets.h"
#include "net/login.h"
#include "net/market_packets.h"
#include "net/marketplace_packets.h"
#include "net/merchant_packets.h"
#include "net/movement_packets.h"
#include "net/notice_packets.h"
#include "net/quest_npc_packets.h"
#include "net/settings_packets.h"
#include "net/zone_link_packets.h"
#include "net/opcodes.h"
#include "net/pet_packets.h"
#include "net/real_estate_packets.h"
#include "net/reward_packets.h"
#include "net/player_packets.h"
#include "net/session.h"
#include "net/server_files_packets.h"
#include "net/session_codec.h"
#include "net/social_packets.h"
#include "net/spell_packets.h"
#include "net/task_packets.h"
#include "net/trade_packets.h"
#include "net/training_packets.h"
#include "net/tribute_packets.h"
#include "net/world_packets.h"
#include "net/zone_packets.h"
#if defined(HAILSTONE_HAS_NMS)
#include "nms_packets.h"
#endif
#if defined(HAILSTONE_HAS_TRIUNE)
#include "triune_packets.h"
#include "triune_windows.h"
#endif
#include "platform/udp_socket.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::net::login_codec;
using fuzz::Bytes;
using fuzz::Rng;
using fuzz::Throws;

namespace {

struct Decoder {
    const char* name;
    std::function<void(const AppPacket&)> run;
};

const std::vector<Decoder>& decoders() {
    static const std::vector<Decoder> list = {
#define HAILSTONE_FUZZ_DECODER(fn) {#fn, [](const AppPacket& p) { (void)fn(p); }},
#define HAILSTONE_FUZZ_PLUGIN_DECODER(ns, fn) {#fn, [](const AppPacket& p) { (void)::ns::fn(p.data); }},
#define HAILSTONE_FUZZ_GAME_DECODER(fn) {#fn, [](const AppPacket& p) { ::hailstone::fuzz_game::feed(&::hailstone::game::fn, p.data); }},
#include "fuzz_decoders.inc"
#undef HAILSTONE_FUZZ_GAME_DECODER
#undef HAILSTONE_FUZZ_PLUGIN_DECODER
#undef HAILSTONE_FUZZ_DECODER
    };
    return list;
}

// Patterns the truncation sweep cuts at every length: all zeros, all 0xff, a counting pattern and random data with
// plenty of boundary values (so counts and string lengths come out large, negative or zero).
std::vector<Bytes> patterns() {
    std::vector<Bytes> out;
    // 24 KB: long enough for the largest packets (the player profile, inventories).
    constexpr std::size_t kLen = 24000;
    out.emplace_back(kLen, std::byte{0});
    out.emplace_back(kLen, std::byte{0xff});
    Bytes counting(kLen);
    for (std::size_t i = 0; i < counting.size(); ++i) counting[i] = static_cast<std::byte>(i);
    out.push_back(std::move(counting));
    Rng rng(0xd1ce);
    out.push_back(fuzz::random_bytes(rng, kLen));
    // Short fields of printable text, as in names and messages.
    Bytes text(kLen);
    for (std::size_t i = 0; i < text.size(); ++i) text[i] = static_cast<std::byte>(i % 17 == 16 ? 0 : 'a' + i % 23);
    out.push_back(std::move(text));
    return out;
}

// 0..plenty at every length, then sparser.
std::vector<std::size_t> truncation_lengths(std::size_t max) {
    std::vector<std::size_t> v;
    for (std::size_t n = 0; n <= max; n += (n < 160 ? 1 : n < 600 ? 5 : n < 2048 ? 53 : 997)) v.push_back(n);
    return v;
}

// Every opcode value RoF2 defines, plus the low values the one-byte chat-server and login opcodes use.
std::vector<std::uint16_t> all_opcodes() {
    std::set<std::uint16_t> s;
    for (std::uint16_t i = 0; i < 0x40; ++i) s.insert(i);
    for (std::uint16_t i = 0; i < static_cast<std::uint16_t>(Op::Count_); ++i) s.insert(to_wire(static_cast<Op>(i)));
    return {s.begin(), s.end()};
}

} // namespace

TEST_CASE("fuzz: the decoder list covers every decode_*(const AppPacket&) declaration") {
    fuzz::QuietLog quiet;
    std::set<std::string> listed;
    for (const auto& d : decoders()) listed.insert(d.name);
    CHECK(listed.size() >= 150);
    // The headers are in the source tree next to tests/ (this file is compiled from there).
    std::string dir = std::string(__FILE__);
    dir = dir.substr(0, dir.rfind('/') == std::string::npos ? 0 : dir.rfind('/')) + "/../src/net/";
    // Every header in src/net/ (*_packets.h, chat_server.h and any added later), so a new one cannot be missed.
    // A decoder may take more parameters after the packet if they have defaults (decode_hailstone_guild's reply);
    // decode_with's body has none and is a helper, not a decoder.
    std::regex decl(R"(\b(decode_[a-z_0-9]+)\(const AppPacket& p(?:, [^,)=]+=[^,)]+)*\))");
    int scanned = 0;
    std::vector<std::filesystem::path> headers;
    std::error_code net_ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, net_ec))
        if (entry.path().extension() == ".h") headers.push_back(entry.path());
    std::sort(headers.begin(), headers.end());
    for (const auto& h : headers) {
        std::ifstream f(h);
        if (!f) continue;
        ++scanned;
        std::stringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        for (std::sregex_iterator it(text.begin(), text.end(), decl), end; it != end; ++it)
            CHECK_MESSAGE(listed.count((*it)[1]) == 1, (*it)[1].str() << " is missing from tests/fuzz_decoders.inc");
    }
    // Run from the source tree (net_ec set when it is not: an installed test binary has nothing to compare), the
    // scan must have found the headers it is meant to check.
    if (!net_ec) CHECK(scanned >= 28);
    // The in-tree plugins' decoders take the bytes OnPacket hands them.
    std::regex plugin_decl(R"(\b(decode_[a-z_0-9]+)\(Bytes p\))");
    for (const char* h : {"../../plugins/nms/nms_packets", "../../plugins/projecttriune/triune_packets",
                          "../../plugins/projecttriune/triune_windows"}) {
        std::ifstream f(dir + h + ".h");
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        for (std::sregex_iterator it(text.begin(), text.end(), plugin_decl), end; it != end; ++it)
            CHECK_MESSAGE(listed.count((*it)[1]) == 1, (*it)[1].str() << " is missing from tests/fuzz_decoders.inc");
    }
    // The game-side decoders of server data over raw bytes.
    std::regex game_decl(R"(\b(decode_[a-z_0-9]+)\(std::span<const std::(?:byte|uint8_t)>)");
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir + "../game", ec)) {
        if (entry.path().extension() != ".h") continue;
        std::ifstream f(entry.path());
        std::stringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        for (std::sregex_iterator it(text.begin(), text.end(), game_decl), end; it != end; ++it)
            CHECK_MESSAGE(listed.count((*it)[1]) == 1, (*it)[1].str() << " is missing from tests/fuzz_decoders.inc");
    }
}

TEST_CASE("fuzz: typed decoders never throw, hang or over-allocate on truncated and mutated packets") {
    fuzz::QuietLog quiet;
    const auto pats = patterns();
    const auto lengths = truncation_lengths(24000);
    for (const auto& dec : decoders()) {
        std::size_t failures = 0;
        auto run = [&](std::uint16_t opcode, const Bytes& data) {
            AppPacket p(opcode, data);
            if (!fuzz::run_case(dec.name, data, Throws::Never, [&](std::span<const std::byte>) { dec.run(p); })) ++failures;
        };
        for (const auto& pat : pats)
            for (auto n : lengths) run(0x1234, Bytes(pat.begin(), pat.begin() + static_cast<std::ptrdiff_t>(n)));
        Rng rng(0x5eed0000u + decoders().size());
        for (int i = 0; i < 300 && failures == 0; ++i) {
            Bytes base = fuzz::random_bytes(rng, rng.below(400));
            run(0x1234, fuzz::mutate(base, rng));
        }
        CHECK_MESSAGE(failures == 0, dec.name << " failed " << failures << " cases");
    }
}

TEST_CASE("fuzz: achievement variable layouts run with a received catalog and independent wire seeds") {
    fuzz::QuietLog quiet;
    namespace fixture = test::achievement_fixture;
    std::vector<Bytes> seeds;
    const auto add = [&](unsigned choice, const AppPacket& packet) {
        Bytes bytes{static_cast<std::byte>(choice)};
        bytes.insert(bytes.end(), packet.data.begin(), packet.data.end()); seeds.push_back(std::move(bytes));
    };
    add(0, fixture::primary()); add(1, fixture::comparison()); add(2, fixture::inspect());
    ByteWriter earned; earned.le<std::uint32_t>(77); earned.le<std::uint32_t>(900); earned.le<std::uint32_t>(3695);
    fixture::string(earned, fixture::metadata()); add(3, {kOpAchievementEarned, earned.take()});
    ByteWriter guild; guild.le<std::uint32_t>(3); guild.le<std::uint64_t>(0); guild.fixed_string("Peer", 64);
    guild.le<std::uint32_t>(0); guild.le<std::uint32_t>(900); fixture::string(guild, fixture::metadata());
    add(4, {to_wire(Op::GuildUpdate), guild.take()});
    Bytes metadata{std::byte{5}};
    for (const auto c : fixture::metadata()) metadata.push_back(static_cast<std::byte>(c));
    seeds.push_back(std::move(metadata)); add(6, fixture::catalog());
    (void)fuzz_achievements::catalog(); // Build the fixed context outside each allocation/timing guard.
    ByteWriter forged; forged.le(static_cast<std::uint32_t>(kMaxAchievementCatalogBytes));
    forged.le<std::uint16_t>(0); const auto forged_bytes = forged.take();
    fuzz::reset_largest_allocation();
    REQUIRE(fuzz::run_case("achievement forged inflate size", forged_bytes, Throws::Never,
        [&](std::span<const std::byte>) { CHECK_FALSE(decode_achievement_catalog({kOpAchievementDefinitions, forged_bytes})); }));
    CHECK(fuzz::largest_allocation() < (1u << 20)); // declared size alone cannot reserve 64 MiB
    Rng rng(0xac41e);
    std::size_t failures = 0;
    for (const auto& seed : seeds) {
        for (std::size_t n = 0; n <= seed.size(); ++n) {
            const Bytes prefix(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(n));
            if (!fuzz::run_case("achievement catalog context", prefix, Throws::Never,
                [&](std::span<const std::byte> bytes) { fuzz_achievements::feed(bytes); })) ++failures;
        }
        for (int i = 0; i < 256 && failures == 0; ++i) {
            const auto bytes = fuzz::mutate(seed, rng);
            if (!fuzz::run_case("achievement catalog context", bytes, Throws::Never,
                [&](std::span<const std::byte> input) { fuzz_achievements::feed(input); })) ++failures;
        }
    }
    CHECK(failures == 0);
}

TEST_CASE("fuzz: decoders that look at the opcode, over every known opcode value") {
    fuzz::QuietLog quiet;
    const auto ops = all_opcodes();
    const auto pats = patterns();
    for (const char* only : {"decode_chat_server_packet", "decode_duel_refusal", "decode_task_select", "decode_skill_timers",
                             "decode_move_item", "decode_trade_request", "decode_waypoint_list", "decode_server_auth_stats"}) {
        for (const auto& dec : decoders()) {
            if (std::string(dec.name) != only) continue;
            for (auto op : ops)
                for (std::size_t n : {0, 1, 4, 9, 12, 33, 64, 300})
                    for (const auto& pat : {pats[0], pats[1], pats[3]}) {
                        Bytes data(pat.begin(), pat.begin() + static_cast<std::ptrdiff_t>(n));
                        AppPacket p(op, data);
                        fuzz::run_case(dec.name, data, Throws::Never, [&](std::span<const std::byte>) { dec.run(p); });
                    }
        }
    }
}

// ---- login-server replies --------------------------------------------------------------------------------------

TEST_CASE("fuzz: login reply parsers report errors by exception and never crash") {
    fuzz::QuietLog quiet;
    // Login replies: u32 sequence, flags (byte 4 = 1: the body is zlib-compressed), then the body; the callers catch
    // std::exception and fail the login (LoginClient::handle). Compressed seeds make the inflate path reachable.
    auto compressed = [](Rng& rng, std::size_t body_size, std::size_t fill) {
        Bytes body = fuzz::random_bytes(rng, body_size);
        for (std::size_t i = fill; i < body.size(); ++i) body[i] = std::byte{0};
        uLongf cap = compressBound(static_cast<uLong>(body.size()));
        std::vector<Bytef> z(cap);
        compress(z.data(), &cap, reinterpret_cast<const Bytef*>(body.data()), static_cast<uLong>(body.size()));
        Bytes pkt(10, std::byte{0});
        pkt[4] = std::byte{1};
        pkt.insert(pkt.end(), reinterpret_cast<std::byte*>(z.data()), reinterpret_cast<std::byte*>(z.data()) + cap);
        return pkt;
    };
    Rng seed_rng(3);
    std::vector<Bytes> seeds = patterns();
    for (std::size_t size : {40, 200, 3000}) seeds.push_back(compressed(seed_rng, size, size / 2));
    seeds.push_back(compressed(seed_rng, 100000, 0)); // a ratio-1000 stream: inflation must stay bounded
    auto sweep = [&](const char* name, auto&& parse) {
        for (const auto& pat : seeds)
            for (auto n : truncation_lengths(std::min<std::size_t>(pat.size(), 1024)))
                fuzz::run_case(name, Bytes(pat.begin(), pat.begin() + static_cast<std::ptrdiff_t>(std::min(n, pat.size()))), Throws::Errors,
                               [&](std::span<const std::byte> d) { (void)parse(d); });
        Rng rng(0x10610);
        for (int i = 0; i < 600; ++i) {
            Bytes in = fuzz::mutate(seeds[rng.below(seeds.size())], rng);
            in.resize(std::min<std::size_t>(in.size(), 4096));
            fuzz::run_case(name, in, Throws::Errors, [&](std::span<const std::byte> d) { (void)parse(d); });
        }
    };
    sweep("parse_handshake_reply", [](std::span<const std::byte> d) { return parse_handshake_reply(d); });
    sweep("parse_login_reply", [](std::span<const std::byte> d) { return parse_login_reply(d); });
    sweep("parse_server_list", [](std::span<const std::byte> d) { return parse_server_list(d); });
    sweep("parse_play_reply", [](std::span<const std::byte> d) { return parse_play_reply(d); });
    sweep("reply_body", [](std::span<const std::byte> d) { return reply_body(login_op::LoginAccepted, d); });
}

// ---- SOE session layer -----------------------------------------------------------------------------------------

namespace {

using soe::ByteSpan;

soe::SessionParams params_with(bool compressed, bool xor_pass, std::uint8_t crc_bytes) {
    soe::SessionParams p;
    p.encode_key = 0x12345678u;
    p.crc_bytes = crc_bytes;
    p.passes = {compressed ? soe::EncodePass::Compression : soe::EncodePass::None,
                xor_pass ? soe::EncodePass::Xor : soe::EncodePass::None};
    p.max_packet_size = 512;
    return p;
}

// Known-good protocol packets (before the encode passes): the seeds the mutations start from.
std::vector<Bytes> soe_seeds() {
    std::vector<Bytes> out;
    auto params = params_with(true, true, 2);
    Rng rng(77);
    Bytes payload = fuzz::random_bytes(rng, 1300);
    std::uint16_t seq = 0;
    for (auto& f : soe::make_reliable(payload, seq, params)) out.push_back(f);
    Bytes small = soe::encode_app(AppPacket(0x1234, fuzz::random_bytes(rng, 20)));
    out.push_back(soe::make_reliable(small, seq, params).front());
    out.push_back(soe::make_ack(soe::op::Ack, 5));
    out.push_back(soe::make_ack(soe::op::OutOfOrderAck, 9));
    out.push_back(soe::make_session_request(0xabcdef01u, 512));
    out.push_back(soe::make_disconnect(0x01020304u));
    std::vector<Bytes> parts = {out[0], soe::make_ack(soe::op::Ack, 1), soe::make_ack(soe::op::Ack, 2)};
    for (auto& p : parts) p.resize(std::min<std::size_t>(p.size(), 200));
    out.push_back(soe::make_combined(parts));
    // 00 19 app-combined: u8 length, then 0xff + u16 BE, then 0xff 0xff 0xff + u32 BE.
    Bytes ac = {std::byte{0}, std::byte{0x19}, std::byte{3}, std::byte{1}, std::byte{2}, std::byte{3}, std::byte{0xff},
                std::byte{0}, std::byte{2}, std::byte{4}, std::byte{5}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
                std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{6}};
    out.push_back(ac);
    return out;
}

void feed_all(const Bytes& packet, soe::InboundStream& stream, std::vector<Bytes>& delivered) {
    (void)stream.push(packet, delivered);
    (void)soe::split_combined(packet);
    (void)soe::split_app_combined(packet);
    (void)soe::decode_app(packet);
    (void)soe::decode_app(packet, 1);
    (void)soe::parse_session_response(packet);
    (void)soe::is_encodable(packet);
}

} // namespace

TEST_CASE("fuzz: SOE framing functions survive truncated and mutated packets") {
    fuzz::QuietLog quiet;
    const auto seeds = soe_seeds();
    Rng rng(0x50e);
    for (const auto& seed : seeds) {
        std::vector<Bytes> inputs;
        for (std::size_t n = 0; n <= seed.size(); ++n) inputs.emplace_back(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(n));
        for (int i = 0; i < 400; ++i) inputs.push_back(fuzz::mutate(seed, rng));
        for (const auto& in : inputs)
            fuzz::run_case("soe framing", in, Throws::Never, [&](std::span<const std::byte> d) {
                Bytes copy(d.begin(), d.end());
                // Fresh stream each time, and one that is already mid-sequence, so both the in-order and the
                // buffered-out-of-order paths run.
                for (std::uint16_t first : {std::uint16_t{0}, std::uint16_t{0xfffe}, std::uint16_t{5}}) {
                    soe::InboundStream stream(first);
                    std::vector<Bytes> delivered;
                    feed_all(copy, stream, delivered);
                    feed_all(copy, stream, delivered);
                }
            });
    }
}

TEST_CASE("fuzz: a stream fed random fragments and out-of-order packets stays bounded") {
    fuzz::QuietLog quiet;
    Rng rng(0xf4a6);
    soe::InboundStream stream(0);
    std::vector<Bytes> delivered;
    for (int i = 0; i < 20000; ++i) {
        ByteWriter w;
        w.le<std::uint8_t>(0);
        w.le<std::uint8_t>(rng.chance(50) ? soe::op::Fragment : soe::op::Packet);
        w.be<std::uint16_t>(static_cast<std::uint16_t>(stream.next() + rng.below(6)));
        if (rng.chance(30)) w.be<std::uint32_t>(rng.chance(50) ? 0xffffffffu : static_cast<std::uint32_t>(rng.below(100000)));
        auto body = fuzz::random_bytes(rng, rng.below(300));
        w.bytes(body);
        Bytes pkt = w.take();
        fuzz::run_case("inbound stream", pkt, Throws::Never, [&](std::span<const std::byte> d) {
            (void)stream.push(d, delivered);
        });
        delivered.clear();
        CHECK(stream.buffered() < 20000);
    }
}

TEST_CASE("fuzz: datagram decode (CRC, XOR and compression passes) rejects garbage without throwing") {
    fuzz::QuietLog quiet;
    const auto seeds = soe_seeds();
    Rng rng(0xda7a);
    for (bool compressed : {false, true})
        for (bool x : {false, true})
            for (std::uint8_t crc : std::initializer_list<std::uint8_t>{0, 2, 4}) {
                auto params = params_with(compressed, x, crc);
                for (const auto& seed : seeds) {
                    Bytes wire = soe::encode_datagram(params, seed);
                    std::vector<Bytes> inputs;
                    for (std::size_t n = 0; n <= wire.size(); n += 1 + n / 40) inputs.emplace_back(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(n));
                    for (int i = 0; i < 150; ++i) inputs.push_back(fuzz::mutate(wire, rng));
                    // Mutations of the *plain* packet, re-encoded with a valid CRC, reach the passes behind the CRC check.
                    for (int i = 0; i < 150; ++i) {
                        Bytes plain = fuzz::mutate(seed, rng);
                        if (!plain.empty()) inputs.push_back(soe::encode_datagram(params, plain));
                    }
                    for (const auto& in : inputs)
                        fuzz::run_case("decode_datagram", in, Throws::Never, [&](std::span<const std::byte> d) {
                            (void)soe::decode_datagram(params, d);
                        });
                }
            }
}

TEST_CASE("fuzz: decompress_body honours its size limit for bogus zlib data") {
    fuzz::QuietLog quiet;
    Rng rng(0x21b);
    Bytes big(100000, std::byte{0}); // compresses to a few hundred bytes: a decompression bomb
    Bytes bomb = soe::compress_body(big);
    auto limited = soe::decompress_body(bomb, 8192);
    CHECK_FALSE(limited.has_value());
    for (int i = 0; i < 3000; ++i) {
        Bytes in = fuzz::random_bytes(rng, rng.below(120));
        if (!in.empty() && rng.chance(80)) in[0] = std::byte{0x5a};
        fuzz::run_case("decompress_body", in, Throws::Never, [&](std::span<const std::byte> d) { (void)soe::decompress_body(d); });
    }
    for (std::size_t n = 0; n <= bomb.size(); ++n)
        fuzz::run_case("decompress_body", Bytes(bomb.begin(), bomb.begin() + static_cast<std::ptrdiff_t>(n)), Throws::Never,
                       [&](std::span<const std::byte> d) { (void)soe::decompress_body(d); });
}

TEST_CASE("fuzz: a live Session receiving corrupt datagrams from its server keeps running") {
    fuzz::QuietLog quiet;
    platform::UdpSocket sock;
    std::string err;
    auto local = platform::Endpoint::v4(127, 0, 0, 1, 0);
    REQUIRE(sock.open(platform::Endpoint::Family::V4, err, &local));
    soe::SessionParams params = params_with(true, false, 2);

    SessionOptions options;
    options.service_interval = std::chrono::milliseconds(0);
    auto session = Session::create(options);
    session->connect("127.0.0.1", sock.local_endpoint()->port);

    platform::Endpoint client;
    bool have_client = false;
    auto pump = [&] {
        session->update();
        std::array<std::byte, 4096> buf;
        platform::Endpoint from;
        while (auto n = sock.recv_from(buf, from)) {
            ByteSpan d(buf.data(), *n);
            if (*n >= 10 && std::to_integer<int>(d[0]) == 0 && std::to_integer<int>(d[1]) == soe::op::SessionRequest && !have_client) {
                client = from;
                have_client = true;
                ByteWriter w;
                w.le<std::uint8_t>(0);
                w.le<std::uint8_t>(soe::op::SessionResponse);
                w.bytes(d.subspan(6, 4));
                w.be<std::uint32_t>(params.encode_key);
                w.le<std::uint8_t>(params.crc_bytes);
                w.le<std::uint8_t>(1);
                w.le<std::uint8_t>(0);
                w.be<std::uint32_t>(params.max_packet_size);
                sock.send_to(client, w.data());
            }
        }
    };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session->state() == SessionState::Connecting && std::chrono::steady_clock::now() < deadline) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(session->state() == SessionState::Connected);

    const auto seeds = soe_seeds();
    Rng rng(0x5e55);
    for (int i = 0; i < 3000; ++i) {
        const Bytes& seed = seeds[rng.below(seeds.size())];
        Bytes plain = fuzz::mutate(seed, rng);
        Bytes wire;
        if (rng.chance(80) && !plain.empty() && std::to_integer<int>(plain[0]) == 0) wire = soe::encode_datagram(params, plain);
        else wire = fuzz::mutate(soe::encode_datagram(params, seed), rng);
        // Never disconnect packets: keep the session alive to exercise its handlers.
        if (wire.size() > 1 && std::to_integer<int>(wire[0]) == 0 && std::to_integer<int>(wire[1]) == soe::op::SessionDisconnect) continue;
        sock.send_to(client, wire);
        if (i % 16 == 0) {
            pump();
            while (session->receive()) {}
        }
    }
    for (int i = 0; i < 20; ++i) {
        pump();
        while (session->receive()) {}
    }
    session->disconnect();
    for (int i = 0; i < 10; ++i) pump();
    CHECK(true);
}
