#pragma once

// LDoN adventures in EQEmu's layouts (common/eq_packet_structs.h: the RoF2 translator passes these through;
// zone/client_packet.cpp Handle_OP_Adventure*, zone/client.cpp SendAdventure*), checked against the RoF2 senders:
// info 0x420030, request 0x4209f0, accept/decline 0x41fed0/0x41ff80, stats 0x619570, leaderboard 0x6173e0.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net/packet.h"

namespace hailstone::net {

// The themes as EQEmu counts them (AdventureStatsColumn_Struct, the leaderboard's theme): 0 all, then Deepest Guk,
// Miragul's Menagerie, Mistmoore Catacombs, Rujarkian Hills, Takish-Hiz.
inline constexpr int kAdventureThemes = 6;
inline constexpr int kAdventureLeaderboardRows = 100;

// OP_AdventureInfoRequest (C→Z, u32 the recruiter's spawn id): the server answers OP_AdventureInfo, the recruiter's
// text; OP_AdventureInfo also carries the server's refusals and OP_AdventureDetails the adventure offered (each a
// NUL-terminated string).
AppPacket encode_adventure_info_request(std::uint32_t npc);
std::optional<std::string> decode_adventure_text(const AppPacket& p);

// OP_AdventureRequest (C→Z, 12 bytes): u32 risk (1 normal, 2 hard), u32 the recruiter, u32 objective (0 any, 1
// assassinate, 2 kill, 3 collect, 4 rescue).
AppPacket encode_adventure_request(std::uint32_t risk, std::uint32_t npc, std::uint32_t type);

// OP_LDoNButton (C→Z, 1 byte): accept (1) or decline (0) the adventure offered.
AppPacket encode_ldon_button(bool accept);

// OP_AdventureData (Z→C, 2084 bytes): u32 (0 when the character has no adventure: EQEmu then sends the struct
// zeroed), char[2048] text, u32 seconds to enter (0 once inside), u32 seconds left, u32 risk, f32 x, y, z (the zone
// in, when it is this zone), u32 show the compass, u32.
struct AdventureData {
    bool assigned = false;
    std::string text;
    std::uint32_t time_to_enter = 0, time_left = 0, risk = 0;
    float x = 0, y = 0, z = 0;
    bool compass = false;
};
std::optional<AdventureData> decode_adventure_data(const AppPacket& p);
AppPacket encode(const AdventureData& v); // server direction, for tests

// OP_AdventureUpdate (Z→C, 8 bytes): u32 current, u32 total (the objective's count).
struct AdventureCount {
    std::uint32_t current = 0, total = 0;
};
std::optional<AdventureCount> decode_adventure_count(const AppPacket& p);

// OP_AdventureFinish (Z→C, 8 bytes): u32 won, u32 points.
struct AdventureFinish {
    bool won = false;
    std::uint32_t points = 0;
};
std::optional<AdventureFinish> decode_adventure_finish(const AppPacket& p);

// OP_AdventurePointsUpdate (Z→C, 140 bytes): u32 available, then each theme's points at +56, +76, +96, +116, +136.
struct AdventurePoints {
    std::uint32_t available = 0;
    std::array<std::uint32_t, kAdventureThemes - 1> theme{};
};
std::optional<AdventurePoints> decode_adventure_points(const AppPacket& p);
AppPacket encode(const AdventurePoints& v); // server direction, for tests

// OP_AdventureStatsRequest (C→Z, one unused byte) and OP_AdventureStatsReply (Z→C, 96 bytes): four columns (successes,
// failures, rank by wins, rank by percentage) of six u32s (total, then the themes).
AppPacket encode_adventure_stats_request();
struct AdventureStats {
    std::array<std::uint32_t, kAdventureThemes> success{}, failure{}, rank{}, rank_percent{};
};
std::optional<AdventureStats> decode_adventure_stats(const AppPacket& p);
AppPacket encode(const AdventureStats& v); // server direction, for tests

// OP_AdventureLeaderboardRequest (C→Z, 12 bytes): u32 order (0 wins, 1 percentage), u32 theme, u32 risk (0 all,
// 1 normal, 2 high; EQEmu
// does not read it). OP_AdventureLeaderboardReply (Z→C, 7220 bytes): u32, u32, our successes, failures and rank,
// 100 × {char[64] name, u32 successes, u32 failures} (unused rows have no name).
AppPacket encode_adventure_leaderboard_request(std::uint32_t order, std::uint32_t theme, std::uint32_t risk);
struct AdventureLeader {
    std::string name;
    std::uint32_t success = 0, failure = 0;
    std::uint32_t position = 0; // one-based wire slot; empty slots still count toward competition ranks
};
struct AdventureLeaderboard {
    std::uint32_t success = 0, failure = 0, rank = 0;
    std::vector<AdventureLeader> rows;
};
std::optional<AdventureLeaderboard> decode_adventure_leaderboard(const AppPacket& p);
AppPacket encode(const AdventureLeaderboard& v); // server direction, for tests

} // namespace hailstone::net
