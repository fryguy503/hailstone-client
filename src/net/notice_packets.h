#pragma once

// Zone → client notices about the player and nearby spawns, in EQEmu's layouts (the RoF2 translator passes most of
// these through; noted per packet), and the answers to the server's offers. The original's handlers are not
// decoded; GameConnection mirrors what each one changes.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net/opcodes.h"
#include "net/packet.h"

namespace hailstone::net {

// OP_Stun (RoF2 Stun_Struct, 8 bytes): u32 duration (ms), then a byte the case @0x4c7093 hands StunMe as its last
// argument (the lean: the turn rate set to 32), then two EQEmu fills (163, 67). EQEmu sends the lean byte 0.
struct Stun {
    std::uint32_t duration_ms = 0;
    bool lean = false;
};
std::optional<Stun> decode_stun(const AppPacket& p);

// OP_ItemRecastDelay (12 bytes, ItemRecastDelay_Struct): u32 delay (s), u32 recast type, bool
// ignore_casting_requirement (the case @0x4c70e8 reads its byte at +8).
struct ItemRecastDelay {
    std::uint32_t delay_s = 0, type = 0;
    bool ignore_casting = false;
};
std::optional<ItemRecastDelay> decode_item_recast_delay(const AppPacket& p);

// OP_Charm (12 bytes, Charm_Struct): u32 owner, u32 pet, u32 command (1 the charm lands, 0 it breaks).
struct Charm {
    std::uint32_t owner = 0, pet = 0;
    bool on = false;
};
std::optional<Charm> decode_charm(const AppPacket& p);

// OP_Untargetable (8 bytes, Untargetable_Struct): u32 spawn, u32 targetable (0 not).
struct Untargetable {
    std::uint32_t spawn = 0;
    bool targetable = true;
};
std::optional<Untargetable> decode_untargetable(const AppPacket& p);

// OP_YellForHelp (4 bytes): the yeller's spawn id.
std::optional<std::uint32_t> decode_yell_for_help(const AppPacket& p);

// OP_Weblink (EQEmu's Weblink_Struct, SendWebLink): the address, NUL-terminated; the case @0x4c5b5d takes it as a C
// string. Bytes after the NUL are ignored; none at all ends the address at the packet's end; an empty one is nullopt.
std::optional<std::string> decode_weblink(const AppPacket& p);

// OP_DisciplineUpdate (RoF2 Disciplines_Struct): 300 × u32 spell ids (0 empty).
std::optional<std::vector<std::uint32_t>> decode_disciplines(const AppPacket& p);

// OP_Translocate (92 bytes, Translocate_Struct, not translated): u32 zone, u32 spell, u32, char[64] caster, f32 y,
// x, z, u32 complete. The client answers by echoing the offer with complete 1 (go) or 0.
struct Translocate {
    std::uint32_t zone = 0, spell = 0;
    std::string caster;
};
std::optional<Translocate> decode_translocate(const AppPacket& p);
AppPacket answer_translocate(const AppPacket& offer, bool go);

// OP_RezzRequest (236 bytes, RoF2 Resurrect_Struct): u32, u16 zone, u16 instance, f32 y, x, z, u32, char[64] us,
// u32, char[64] rezzer, u32 spell, char[64] corpse, u32 action, 2 × u32. OP_RezzAnswer echoes it with action 1
// (accept) or 0.
struct RezzRequest {
    std::uint32_t zone = 0, spell = 0;
    std::uint16_t instance = 0; // +6: with `zone`, the dword ConfirmationDialog_HandleResponse compares (+4)
    std::string rezzer, corpse;
};
std::optional<RezzRequest> decode_rezz_request(const AppPacket& p);
AppPacket answer_rezz(const AppPacket& offer, bool accept);

// OP_SharedTaskInvite (136 bytes, SharedTaskInvite_Struct): i32, i32 invitation id, char[64] task, char[64]
// inviter; OP_SharedTaskInviteResponse (12 bytes): i32 0, i32 the id, i8 accepted, 3.
struct SharedTaskInvite {
    std::int32_t invite_id = 0;
    std::string task, inviter;
};
std::optional<SharedTaskInvite> decode_shared_task_invite(const AppPacket& p);
AppPacket encode_shared_task_invite_response(std::int32_t invite_id, bool accepted);

// OP_GroupRoles (148 bytes, GroupRole_Struct): char[64] member, char[64] leader, 3 × u32, u32 role, u8 on.
struct GroupRole {
    std::string member, leader;
    std::int32_t role = 0;
    std::uint8_t on = 0;
};
std::optional<GroupRole> decode_group_role(const AppPacket& p);
// Send_OP_GroupRoles @0x525d30 (C→Z): the member, our name, +0x80 and +0x84 unset by the original (zero here), +0x88
// the group's id (CGroupBase m_id; the client does not know EQEmu's, 0 here), the role, the on byte, 3 bytes unset.
AppPacket encode_group_role(const GroupRole& v, std::uint32_t group_id = 0);

// EQEmu's live guild roster changes (zone/guild.cpp EntityList::SendGuildMember*), u32 guild first:
// OP_GuildMemberAdd (104 B: 3 × u32, level, class, rank, show, zone, last on, char[64] name), OP_GuildMemberDelete
// (char[64]), OP_GuildMemberLevel (char[64], level), OP_GuildMemberPublicNote (char[64], char[256]), the rank
// (OP_SetGuildRank and OP_GuildMemberRankAltBanker share 0x0b9c: rank, char[64], u32 alt << 1 | banker, offline),
// OP_GuildMemberDetails (char[64], zone, last on, offline).
struct GuildRosterChange {
    enum Kind { Add, Delete, Level, Note, Rank, Details } kind = Add;
    std::uint32_t guild_id = 0;
    std::string name, note;
    std::uint32_t level = 0, class_ = 0, rank = 0, zone_id = 0, last_on = 0;
    bool alt = false, banker = false, offline = false;
};
std::optional<GuildRosterChange> decode_guild_roster_change(const AppPacket& p);

// OP_Begging (20 bytes, BeggingResponse_Struct): 3 × u32, u32 result (0 fail, 1 platinum, 2 gold, 3 silver, 4
// copper), u32 amount. OP_PickPocket (sPickPocket_Struct): u32 to, u32 from, u32 skill, u32 type (1..4 platinum ..
// copper), u32 coin, char[64] item. Both add coin without a coin update.
struct CoinFind {
    std::uint32_t kind = 0, amount = 0;
};
std::optional<CoinFind> decode_begging(const AppPacket& p);
std::optional<CoinFind> decode_pick_pocket(const AppPacket& p);

// OP_Bind_Wound (RoF2 BindWound_Struct, 8 bytes): u16 target, u16, u16 type (3 start, 1 done, 4 target dead, 5 it
// left the zone, 7 no bandage), u16.
std::optional<std::uint16_t> decode_bind_wound(const AppPacket& p);

} // namespace hailstone::net
