#pragma once

// Guild ranks and their permissions, as the original keeps them (vectors GuildRank_Compare, GuildRank_HasPermission,
// LocalPC_HasGuildPermission):
//   - ranks run 1 (leader) .. 8 (recruit); GuildRank_Compare @0x7ff390 compares them the other way round when
//     both are non-zero;
//   - the rank permission table is the guild manager's +0x9318: a leading dword, then two dwords of bits per rank
//     (dword[1 + rank * 2 + permission / 32], bit permission % 32). HandleWorldMessage_Guild @0x425d30 fills it from
//     OP_GuildUpdate action 5 {+0x50 rank, +0x54 permission, +0x58 byte value} with 0x7d95e0 (set or clear the
//     bit); EQEmu sends every rank's permissions 1..GUILD_MAX_FUNCTIONS that way on zoning in (Client::SendGuildRanks)
//     and again on a change.

#include <array>
#include <cstdint>

namespace hailstone::game {

// GuildRank_Compare @0x7ff390: b - a when both ranks are non-zero, else a - b (32-bit wrapping).
std::int32_t guild_rank_compare(std::int32_t a, std::int32_t b);
// 0x7d7360: compare(rank, other) > 0. Send_OP_GuildPromote refuses (eqstr 0x204) when it holds for (the new
// rank, your rank).
bool guild_rank_above(std::int32_t rank, std::int32_t other);
// 0x7d73c0: compare(rank, other) >= 0. Send_OP_GuildDemote stops silently when it holds for (rank + 1, rank).
bool guild_rank_at_least(std::int32_t rank, std::int32_t other);
// 0x7d7380: false unless both members are given, else compare(a's rank, b's rank) >= 0.
bool guild_member_rank_at_least(const std::int32_t* a_rank, const std::int32_t* b_rank);

// The dword 0x7d96c0 reads for (rank, permission): 1 + rank * 2 + permission / 32 (C division), wrapping in 32
// bits, and the bit of it: permission % 32 (C remainder), the shift taken mod 32.
std::int32_t guild_permission_word_index(std::int32_t rank, std::int32_t permission);
bool guild_permission_bit(std::uint32_t word, std::int32_t permission);

// The guild manager's permission table (+0x9318). The original's table is 0x58 bytes (22 dwords); an index outside
// it reads other memory there, here it is no permission.
struct GuildPermissions {
    std::array<std::uint32_t, 22> words{};

    // GuildRank_HasPermission @0x7d96c0.
    bool has(std::int32_t rank, std::int32_t permission) const;
    // 0x7d95e0 (OP_GuildUpdate action 5): sets or clears the bit.
    void set(std::int32_t rank, std::int32_t permission, bool value);
    void clear() { words = {}; }
};

// LocalPC_HasGuildPermission @0x4241f0: false without a local character; true for a GM (CharacterBase virtual
// +0x90: the spawn exists and PlayerBase::IsGm); else the rank's bit (the character's rank +0x205c).
bool local_pc_has_guild_permission(bool have_character, bool is_gm, std::int32_t rank, std::int32_t permission,
                                   const GuildPermissions& table);

// The permission numbers the client checks (EQEmu GuildAction names).
inline constexpr std::int32_t kGuildPermInvite = 7;          // Send_OP_GuildInvite
inline constexpr std::int32_t kGuildPermPromote = 8;         // Send_OP_GuildPromote (and the remove button)
inline constexpr std::int32_t kGuildPermDemote = 9;          // Send_OP_GuildDemote (another member)
inline constexpr std::int32_t kGuildPermBankDeposit = 13;     // CGuildBankWnd: the Deposit button
inline constexpr std::int32_t kGuildPermBankWithdraw = 14;    // withdrawing others' items
inline constexpr std::int32_t kGuildPermBankView = 15;        // seeing (and viewing) every item
inline constexpr std::int32_t kGuildPermBankPromote = 16;     // Put in Bank
inline constexpr std::int32_t kGuildPermBankPermissions = 17; // an item's permission, Change Member, Split, Merge
inline constexpr std::int32_t kGuildPermGuildChatSee = 0x13; // msg_new_text: channels 0 and 17
inline constexpr std::int32_t kGuildPermTributeOthers = 22;   // changing another member's tribute participation
inline constexpr std::int32_t kGuildPermTributeBenefits = 23; // choosing and activating the guild's tribute
inline constexpr std::int32_t kGuildPermBanker = 0x1a;       // Send_OP_GuildManageBanker
inline constexpr std::int32_t kGuildPermDemoteSelf = 0x1e;   // Send_OP_GuildDemote (yourself)

} // namespace hailstone::game
