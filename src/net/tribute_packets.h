#pragma once

// Personal tribute (the tribute master and benefit windows) in EQEmu's layouts (zone/tribute.cpp,
// common/patches/rof2*.{h,cpp}); the original's CTributeMasterWnd / CTributeBenefitWnd senders are not decoded.
// Several of the server's numbers are big-endian (EQEmu's "backwards byte order", noted per field).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/item_packets.h"
#include "net/opcodes.h"
#include "net/packet.h"

namespace hailstone::net {

inline constexpr int kTributeSlots = 5;         // RoF2 invtype::TRIBUTE_SIZE
inline constexpr int kTributeTiers = 10;        // MAX_TRIBUTE_TIERS
inline constexpr std::uint32_t kTributeNone = 0xffffffff;

// OP_SendTributes (4 bytes, unused; client → zone, on the first zone-in): the server answers one OP_TributeInfo per personal
// tribute.
AppPacket encode_send_tributes();

// OP_TributeInfo (zone → client, RoF2 TributeAbility_Struct): u32 id, u32 tier count (both big-endian), 10 × {u32
// level, u32 item id, u32 cost} (big-endian), u32, the name.
struct TributeTier {
    std::uint32_t level = 0, item_id = 0, cost = 0;
};
struct TributeAbility {
    std::uint32_t id = 0;
    std::uint32_t tier_count = 0;
    std::array<TributeTier, kTributeTiers> tiers{};
    std::string name;
    // The tiers the server has: EQEmu sends its tributes table's "unknown" column as the tier count
    // (SendTributes), so the count is the leading tiers with a level, item or cost.
    std::uint32_t tiers_used() const;
};
std::optional<TributeAbility> decode_tribute_info(const AppPacket& p);
AppPacket encode(const TributeAbility& v); // server direction, for tests

// OP_TributeUpdate, both ways (48 bytes, TributeInfo_Struct): u32 active, 5 × u32 tribute id (0xffffffff none),
// 5 × u32 tier, u32 tribute master id. The client sends it to change its benefits; the server answers with its own.
struct TributeSettings {
    bool active = false;
    std::array<std::uint32_t, kTributeSlots> tributes{kTributeNone, kTributeNone, kTributeNone, kTributeNone, kTributeNone};
    std::array<std::uint32_t, kTributeSlots> tiers{};
    std::uint32_t master_id = 0;
};
inline constexpr std::size_t kTributeSettingsSize = 48;
AppPacket encode(const TributeSettings& v);
std::optional<TributeSettings> decode_tribute_settings(const AppPacket& p);

// OP_TributeTimer (u32: the active time left in ms, or the full duration when inactive) and OP_TributePointUpdate
// (16 bytes: i32 favour, u32, i32 career favour, u32).
std::optional<std::uint32_t> decode_tribute_timer(const AppPacket& p);
struct TributePoints {
    std::int32_t points = 0, career = 0;
};
std::optional<TributePoints> decode_tribute_points(const AppPacket& p);

// OP_TributeToggle (u32: 1 activate, 0 deactivate).
AppPacket encode_tribute_toggle(bool on);

// OP_OpenTributeMaster, both ways (12 bytes, StartTribute_Struct): u32 our spawn id, u32 master id, u32 response
// (the server's: 1 open, 0 refused).
struct StartTribute {
    std::uint32_t client_id = 0, master_id = 0, response = 0;
};
AppPacket encode(const StartTribute& v);
std::optional<StartTribute> decode_start_tribute(const AppPacket& p);

// OP_SelectTribute: C→Z 12 bytes {u32 our spawn id, u32 tribute id, u32}; Z→C {u32 spawn id, u32 tribute id,
// the description}.
AppPacket encode_select_tribute(std::uint32_t client_id, std::uint32_t tribute_id);
struct TributeDescription {
    std::uint32_t tribute_id = 0;
    std::string text;
};
std::optional<TributeDescription> decode_tribute_description(const AppPacket& p);

// OP_TributeItem, both ways (24 bytes, RoF2 TributeItem_Struct): the item's slot (12 B), u32 quantity, u32 master
// id, i32 favour (the server's answer: what the donation gave). OP_TributeMoney (12 bytes): u32 platinum, u32
// master id, i32 favour.
struct TributeItem {
    ItemSlot slot = ItemSlot::invalid();
    std::uint32_t quantity = 0, master_id = 0;
    std::int32_t points = 0;
};
AppPacket encode(const TributeItem& v);
std::optional<TributeItem> decode_tribute_item(const AppPacket& p);
struct TributeMoney {
    std::uint32_t platinum = 0, master_id = 0;
    std::int32_t points = 0;
};
AppPacket encode(const TributeMoney& v);
std::optional<TributeMoney> decode_tribute_money(const AppPacket& p);

// ---- guild tribute (EQEmu zone/tribute.cpp, client_packet.cpp Handle_OP_GuildTribute*, worldserver.cpp; not
// translated for RoF2 but for the item donation). A guild has two benefits.

inline constexpr int kGuildTributeSlots = 2;

// OP_RequestGuildTributes (C→Z, 4 bytes, unused; right after OP_SendTributes) asks for OP_SendGuildTributes: u32 guild id, then a
// personal tribute's layout (decode_tribute_info).
AppPacket encode_request_guild_tributes();
std::optional<TributeAbility> decode_guild_tribute_info(const AppPacket& p);
AppPacket encode_guild_tribute_info(const TributeAbility& v); // server direction, for tests

// OP_GuildSelectTribute: C→Z 16 bytes {u32 id, u32 tier, u32 id, u32}; Z→C {u32 id, u32 tier, u32 id, the
// description}.
AppPacket encode_guild_select_tribute(std::uint32_t tribute_id, std::uint32_t tier);
std::optional<TributeDescription> decode_guild_tribute_description(const AppPacket& p);

// The guild's benefits: OP_GuildSendActiveTributes (at zone-in and on a change) and OP_GuildTributeToggleReply
// (on activation), 40 bytes: u32 guild, u32 favour, u32 timer (ms), u32 on, 8, 2 × u32 tribute id, 2 × u32 tier
// (id 0xffffffff none).
struct GuildTributeActive {
    std::uint32_t favor = 0, timer_ms = 0;
    bool enabled = false;
    std::array<std::uint32_t, kGuildTributeSlots> tributes{kTributeNone, kTributeNone};
    std::array<std::uint32_t, kGuildTributeSlots> tiers{};
    // The leading DWORD: toggle replies identify the guild; SendGuildActiveTributes may leave it zero.
    std::uint32_t guild_id = 0;
};
std::optional<GuildTributeActive> decode_guild_tribute_active(const AppPacket& p);
AppPacket encode(const GuildTributeActive& v, Op op = Op::GuildSendActiveTributes); // server direction, for tests

// OP_GuildTributeFavorAndTimer (Z→C, 16 bytes): u32 guild, u32 favour, u32 timer (ms), u32 trophy timer.
struct GuildTributeFavor {
    std::uint32_t favor = 0, timer_ms = 0;
};
std::optional<GuildTributeFavor> decode_guild_tribute_favor(const AppPacket& p);

// Choosing benefits (a guild tribute master's window): OP_GuildSaveActiveTributes (C→Z, 48 bytes: u32 command, 16,
// u32 master, 2 × u32 id, 2 × u32 tier, 8), then OP_GuildModifyBenefits (C→Z, 80 bytes: u32 command 0 save / 1
// modify, u32 data, 12, u32 master, 2 × id, 2 × tier, 40; the server echoes it). OP_GuildTributeToggleReq (C→Z, 12
// bytes: u32 1 on / 0 off).
AppPacket encode_guild_save_tributes(std::uint32_t master_id, const GuildTributeActive& v);
AppPacket encode_guild_modify_benefits(bool save, std::uint32_t master_id, const GuildTributeActive& v);
AppPacket encode_guild_tribute_toggle(bool on);

// OP_GuildOptInOut: C→Z 80 bytes {u32 guild, u32 on, char[64] member, u32 command, u32 master}; Z→C 88 bytes {u32
// guild, char[64] member, u32 on, u32 trophy on, u32 donations, u32 time, u32 command}. The original's handler
// (HandleWorldMessage_GuildTribute, 0x482cf0) gives the named member the two states, the donations, the time
// and the command's low byte (the tribute cells' flag). EQEmu echoes the request's command when a member opts in
// or out, sends 1 after a donation and 0x2fb when a member zones in.
AppPacket encode_guild_tribute_opt(std::uint32_t guild_id, bool on, std::string_view member);
// The trophy tribute's opt-out (Send_OP_GuildTrophyOptInOut @0x424120): opcode 0x02bc, not in EQEmu's RoF2 table,
// the layout of OP_GuildOptInOut's request. EQEmu has no trophy tribute (a member's trophy state is always off), so
// nothing sends it against a stock server.
inline constexpr std::uint16_t kOpGuildTrophyOptInOut = 0x02bc;
AppPacket encode_guild_trophy_opt(std::uint32_t guild_id, bool on, std::string_view member);
struct GuildTributeOpt {
    std::string member;
    bool on = false, trophy_on = false;
    std::uint32_t donations = 0, time = 0, command = 0;
};
std::optional<GuildTributeOpt> decode_guild_tribute_opt(const AppPacket& p);
AppPacket encode(const GuildTributeOpt& v); // server direction, for tests

// OP_GuildTributeDonateItem: C→Z 36 bytes (RoF2: u32 type, u16 slot, u16 sub, u16 aug, u16, u32 quantity, u32
// master, u32, u32 guild, 2 × u32); Z→C 24 bytes {u32 type, u16 slot, u16 sub, u16 aug, u16, u32 quantity, u32, u32
// favour}: EQEmu has deleted the item without telling the client. OP_GuildTributeDonatePlat: C→Z 20 bytes {u32
// platinum, u32 master, u32, u32 guild, u32}; Z→C 12 bytes {u32 platinum, u32, u32 favour}.
AppPacket encode_guild_donate_item(const ItemSlot& slot, std::uint32_t quantity, std::uint32_t master_id, std::uint32_t guild_id);
struct GuildTributeDonation {
    ItemSlot slot = ItemSlot::invalid();
    std::uint32_t quantity = 0, favor = 0;
};
std::optional<GuildTributeDonation> decode_guild_donate_item(const AppPacket& p);
AppPacket encode(const GuildTributeDonation& v); // server direction, for tests
AppPacket encode_guild_donate_platinum(std::uint32_t platinum, std::uint32_t master_id, std::uint32_t guild_id);
struct GuildTributePlatinum {
    std::uint32_t platinum = 0, favor = 0;
};
std::optional<GuildTributePlatinum> decode_guild_donate_platinum(const AppPacket& p);

} // namespace hailstone::net
