#include "net/adventure_packets.h"

#include "core/byte_io.h"
#include "net/opcodes.h"
#include "net/packet_io.h"

namespace hailstone::net {

using detail::decode_with;

namespace {

constexpr std::size_t kDataSize = 2084, kDataText = 2048, kPointsSize = 140, kStatsSize = 96, kName = 64;
constexpr std::size_t kLeaderboardSize = 20 + kAdventureLeaderboardRows * (kName + 8);

} // namespace

AppPacket encode_adventure_info_request(std::uint32_t npc) {
    ByteWriter w;
    w.le(npc);
    return make_packet(Op::AdventureInfoRequest, std::move(w));
}

std::optional<std::string> decode_adventure_text(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) { return r.cstring(); });
}

AppPacket encode_adventure_request(std::uint32_t risk, std::uint32_t npc, std::uint32_t type) {
    ByteWriter w;
    w.le(risk);
    w.le(npc);
    w.le(type);
    return make_packet(Op::AdventureRequest, std::move(w));
}

AppPacket encode_ldon_button(bool accept) {
    ByteWriter w;
    w.le<std::uint8_t>(accept ? 1 : 0);
    return make_packet(Op::LDoNButton, std::move(w));
}

std::optional<AdventureData> decode_adventure_data(const AppPacket& p) {
    if (p.data.size() < kDataSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        AdventureData v;
        std::uint32_t tag = r.le<std::uint32_t>();
        v.text = r.fixed_string(kDataText);
        v.time_to_enter = r.le<std::uint32_t>();
        v.time_left = r.le<std::uint32_t>();
        v.risk = r.le<std::uint32_t>();
        v.x = r.f32();
        v.y = r.f32();
        v.z = r.f32();
        v.compass = r.le<std::uint32_t>() != 0;
        v.assigned = tag != 0 || !v.text.empty();
        return v;
    });
}

AppPacket encode(const AdventureData& v) {
    ByteWriter w;
    w.le<std::uint32_t>(v.assigned ? 0xbfc40100 : 0); // EQEmu's SendAdventureDetails value
    w.fixed_string(std::string_view(v.text).substr(0, kDataText - 1), kDataText);
    w.le(v.time_to_enter);
    w.le(v.time_left);
    w.le(v.risk);
    w.f32(v.x);
    w.f32(v.y);
    w.f32(v.z);
    w.le<std::uint32_t>(v.compass ? 1 : 0);
    w.le<std::uint32_t>(v.assigned ? 0x0a : 0);
    return make_packet(Op::AdventureData, std::move(w));
}

std::optional<AdventureCount> decode_adventure_count(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        AdventureCount v;
        v.current = r.le<std::uint32_t>();
        v.total = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<AdventureFinish> decode_adventure_finish(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        AdventureFinish v;
        v.won = r.le<std::uint32_t>() != 0;
        v.points = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<AdventurePoints> decode_adventure_points(const AppPacket& p) {
    if (p.data.size() < kPointsSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        AdventurePoints v;
        v.available = r.le<std::uint32_t>();
        r.skip(52);
        for (std::size_t i = 0; i < v.theme.size(); ++i) {
            if (i) r.skip(16);
            v.theme[i] = r.le<std::uint32_t>();
        }
        return v;
    });
}

AppPacket encode(const AdventurePoints& v) {
    ByteWriter w;
    w.le(v.available);
    w.zeros(52);
    for (std::size_t i = 0; i < v.theme.size(); ++i) {
        if (i) w.zeros(16);
        w.le(v.theme[i]);
    }
    return make_packet(Op::AdventurePointsUpdate, std::move(w));
}

AppPacket encode_adventure_stats_request() {
    ByteWriter w;
    w.le<std::uint8_t>(0); // CAdventureStatsWnd::SendRequest @0x619570 sends one unused byte.
    return make_packet(Op::AdventureStatsRequest, std::move(w));
}

std::optional<AdventureStats> decode_adventure_stats(const AppPacket& p) {
    if (p.data.size() < kStatsSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        AdventureStats v;
        for (auto* col : {&v.success, &v.failure, &v.rank, &v.rank_percent})
            for (auto& n : *col) n = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const AdventureStats& v) {
    ByteWriter w;
    for (const auto* col : {&v.success, &v.failure, &v.rank, &v.rank_percent})
        for (auto n : *col) w.le(n);
    return make_packet(Op::AdventureStatsReply, std::move(w));
}

AppPacket encode_adventure_leaderboard_request(std::uint32_t order, std::uint32_t theme, std::uint32_t risk) {
    ByteWriter w;
    w.le(order);
    w.le(theme);
    w.le(risk);
    return make_packet(Op::AdventureLeaderboardRequest, std::move(w));
}

std::optional<AdventureLeaderboard> decode_adventure_leaderboard(const AppPacket& p) {
    if (p.data.size() < kLeaderboardSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        AdventureLeaderboard v;
        r.skip(8);
        v.success = r.le<std::uint32_t>();
        v.failure = r.le<std::uint32_t>();
        v.rank = r.le<std::uint32_t>();
        for (int i = 0; i < kAdventureLeaderboardRows; ++i) {
            AdventureLeader l;
            l.name = r.fixed_string(kName);
            l.success = r.le<std::uint32_t>();
            l.failure = r.le<std::uint32_t>();
            l.position = static_cast<std::uint32_t>(i + 1);
            if (!l.name.empty()) v.rows.push_back(std::move(l));
        }
        return v;
    });
}

AppPacket encode(const AdventureLeaderboard& v) {
    ByteWriter w;
    w.zeros(8);
    w.le(v.success);
    w.le(v.failure);
    w.le(v.rank);
    for (int i = 0; i < kAdventureLeaderboardRows; ++i) {
        const AdventureLeader* l = i < static_cast<int>(v.rows.size()) ? &v.rows[static_cast<std::size_t>(i)] : nullptr;
        w.fixed_string(l ? std::string_view(l->name).substr(0, kName - 1) : std::string_view(), kName);
        w.le(l ? l->success : 0u);
        w.le(l ? l->failure : 0u);
    }
    return make_packet(Op::AdventureLeaderboardReply, std::move(w));
}

} // namespace hailstone::net
