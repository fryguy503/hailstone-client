#pragma once

// Typed RoF2 group packets: invitations, accepting and declining, the member
// list, leader changes, leaving and disbanding, and the group members' mana
// and endurance percentages. Layouts follow the NMS server's RoF2 translator
// (common/patches/rof2.cpp ENCODE/DECODE, rof2_structs.h) and, for
// pass-through opcodes, common/eq_packet_structs.h. What the client sends and
// when follows the original (DoGroupInvite @0x524ee0,
// Handle_OP_GroupInvite2 @0x525400, Send_OP_GroupFollow2 @0x525690,
// Cmd_disband @0x4e8c50, Cmd_makeleader @0x4ef340, HandleWorldMessage_Group
// @0x578000, CGroupBase_HandleGroupUpdateB @0x7d6320). Per-packet notes are in
// docs/protocol/rof2-packets.md ("Groups").

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/opcodes.h"
#include "net/packet.h"

namespace hailstone::net {

// OP_GroupInvite (0x6110) / OP_GroupInvite2 (0x32c2), both directions, 148
// bytes (rof2_structs.h GroupInvite_Struct): char[64] invitee, char[64]
// inviter, 5 × u32. The original sends OP_GroupInvite when it has no group yet
// and OP_GroupInvite2 when it adds to its group; EQEmu forwards either to the
// invitee as OP_GroupInvite. The original also reads a u32 at +148, past the
// 148 bytes EQEmu sends, and echoes it in its OP_GroupFollow (`follow_cookie`).
struct GroupInvite {
    std::string invitee;
    std::string inviter;
    std::uint32_t unknown[5] = {};
    std::uint32_t follow_cookie = 0; // the u32 at +148 when present
};
inline constexpr std::size_t kGroupInviteSize = 148;
AppPacket encode(const GroupInvite& v, bool second_opcode = false);
std::optional<GroupInvite> decode_group_invite(const AppPacket& p);

// OP_GroupFollow (0x1649) / OP_GroupFollow2 (0x2060), both directions, 152
// bytes (GroupFollow_Struct): char[64] inviter, char[64] invitee (the
// accepting player), 5 × u32, u32 at +148. Accepting answers an invite that
// came as OP_GroupInvite with OP_GroupFollow, else with OP_GroupFollow2
// (Send_OP_GroupFollow2). EQEmu echoes it to the inviter.
struct GroupFollow {
    std::string inviter;
    std::string invitee;
    std::uint32_t unknown[5] = {};
    std::uint32_t unknown148 = 0;
};
inline constexpr std::size_t kGroupFollowSize = 152;
AppPacket encode(const GroupFollow& v, bool second_opcode);
std::optional<GroupFollow> decode_group_follow(const AppPacket& p);

// OP_CancelInvite (0x2a50), client → zone, 152 bytes (GroupCancel_Struct):
// char[64] inviter, char[64] the declining player, 20 bytes, u32 reason at
// +148: 3 the player declined (/disband or the Decline button), 1 already
// invited, 4 / 5 raid states (Handle_OP_GroupInvite2's automatic refusals).
// EQEmu has no handler for this opcode (its OP_GroupCancelInvite is 0x0000
// for RoF2), so the inviter is never told; it is sent as the original does.
struct GroupCancel {
    std::string inviter;
    std::string invitee;
    std::uint32_t reason = 0;
};
inline constexpr std::size_t kGroupCancelSize = 152;
inline constexpr std::uint32_t kCancelDeclined = 3, kCancelAlreadyInvited = 1, kCancelInRaid = 4,
                               kCancelRaidMember = 5;
AppPacket encode(const GroupCancel& v);
std::optional<GroupCancel> decode_group_cancel(const AppPacket& p);

// GroupGeneric_Struct, 148 bytes: char[64] name1, char[64] name2, 5 × u32.
//  - OP_GroupDisband (0x4c10), client → zone: name1 the member to remove (a
//    leader kicking a member) or the player itself, name2 the leader or the
//    player itself. EQEmu decides by the sender's target (Handle_OP_GroupDisband).
//  - OP_GroupDisbandYou (0x1ae5), zone → client: you left or the group was
//    disbanded (name1 you, name2 the member that left).
//  - OP_GroupDisbandOther (0x74da), zone → client: name2 left the group.
struct GroupGeneric {
    std::string name1;
    std::string name2;
};
inline constexpr std::size_t kGroupGenericSize = 148;
AppPacket encode_group_generic(Op op, const GroupGeneric& v);
std::optional<GroupGeneric> decode_group_generic(const AppPacket& p);

// OP_GroupUpdate (0x3abb), zone → client, 148 bytes (rof2_structs.h
// GroupJoin_Struct): char[64] owner name (mercenaries), char[64] member name,
// u8 mercenary, 3 bytes, u32 level, 12 bytes. A member joined (EQEmu also
// sends it for leader changes and leadership-AA updates; the original only
// adds the name when it is not in the group yet).
struct GroupJoin {
    std::string owner_name;
    std::string member_name;
    std::uint8_t merc = 0;
    std::uint32_t level = 0;
};
inline constexpr std::size_t kGroupJoinSize = 148;
AppPacket encode(const GroupJoin& v); // server direction, for tests
std::optional<GroupJoin> decode_group_join(const AppPacket& p);

// OP_GroupUpdateB (0x6194), zone → client, variable: the whole member list.
// u32 group id, u32 count, cstr leader; count × {u32 slot (0..5), cstr name,
// u16 mercenary, cstr owner name, u32 level, u8 main tank, u8 main assist,
// u8 puller, u32 offline, u32 timestamp}. EQEmu (ENCODE(OP_GroupUpdate) from
// a GroupUpdate2_Struct) puts the receiver in slot 0 and sends fake levels
// (0x46 / 0x41); its "group created" packet (Client::SendGroupCreatePacket)
// has an empty leader name and the real level.
struct GroupUpdateMember {
    std::uint32_t slot = 0;
    std::string name;
    std::uint16_t merc = 0;
    std::string owner_name;
    std::uint32_t level = 0;
    std::uint8_t main_tank = 0, main_assist = 0, puller = 0;
    std::uint32_t offline = 0;
    std::uint32_t timestamp = 0;
};
struct GroupUpdateB {
    std::uint32_t group_id = 0;
    std::string leader;
    std::vector<GroupUpdateMember> members;
};
AppPacket encode(const GroupUpdateB& v); // server direction, for tests
std::optional<GroupUpdateB> decode_group_update_b(const AppPacket& p);

// OP_GroupLeaderChange (0x21b4), zone → client, 148 bytes
// (GroupLeaderChange_Struct): 64 bytes, char[64] new leader, 20 bytes.
AppPacket encode_group_leader_change(const std::string& leader); // server direction, for tests
std::optional<std::string> decode_group_leader_change(const AppPacket& p);

// OP_GroupMakeLeader (0x4229), client → zone, 456 bytes
// (GroupMakeLeader_Struct; Cmd_makeleader @0x4ef340): u32 8, char[64] the
// current leader (the sender), char[64] the new leader, 324 zero bytes.
struct GroupMakeLeader {
    std::string current_leader;
    std::string new_leader;
};
inline constexpr std::size_t kGroupMakeLeaderSize = 456;
AppPacket encode(const GroupMakeLeader& v);
std::optional<GroupMakeLeader> decode_group_make_leader(const AppPacket& p);

// OP_GroupMentor (0x3342), client → zone, 68 bytes (GroupMentor_Struct): i32
// percent, char[64] name; an empty name clears the mentor. Cmd_makeleader
// sends a cleared one after OP_GroupMakeLeader.
AppPacket encode_group_mentor(std::int32_t percent, const std::string& name);

// OP_MobManaUpdate (0x2404) / OP_MobEnduranceUpdate (0x1c81), zone → client,
// 3 bytes: u16 spawn id, u8 percent. EQEmu sends them for group members
// (Group::SendHPManaEndPacketsTo, SendManaPacketFrom, SendEndurancePacketFrom).
struct MobPercent {
    std::uint16_t spawn_id = 0;
    std::uint8_t percent = 0;
};
AppPacket encode_mob_percent(Op op, const MobPercent& v); // server direction, for tests
std::optional<MobPercent> decode_mob_percent(const AppPacket& p);

// ---------------------------------------------------------------- leadership (EQEmu's layouts)

// OP_LeadershipExpToggle (1 byte: 1 earn leadership experience, 0 not); the server answers in chat.
AppPacket encode_leadership_exp_toggle(bool on);
// OP_PurchaseLeadershipAA (u32: the ability's index, game/leadership.h).
AppPacket encode_purchase_leadership(std::uint32_t ability);
// OP_UpdateLeadershipAA (12 bytes, zone → client, passed through): u32 ability, u32 new rank, u32 points left.
struct LeadershipRank {
    std::uint32_t ability = 0, rank = 0, points_left = 0;
};
std::optional<LeadershipRank> decode_leadership_rank(const AppPacket& p);
// OP_LeadershipExpUpdate (28 bytes, RoF2 LeadershipExpUpdate_Struct): f64 group experience, u32 group points, u32,
// f64 raid experience, u32 raid points.
struct LeadershipExp {
    double group_exp = 0, raid_exp = 0;
    std::uint32_t group_points = 0, raid_points = 0;
};
std::optional<LeadershipExp> decode_leadership_exp(const AppPacket& p);
AppPacket encode(const LeadershipExp& v); // server direction, for tests

// ---------------------------------------------------------------- leadership abilities (HandleWorldMessage_LeadershipAbility
// @0x560260, Leadership_ActivateAbility @0x55f0f0)

// OP_GroupLeadershipAAUpdate (0x02cf, 320 bytes; Handle_OP_GroupLeadershipAAUpdate copies it whole to the
// character's +0x10c): u32, the group leader's 16 group ability ranks (+4), ..., EQEmu's NPCMarkerID (+264).
struct GroupLeadershipUpdate {
    std::array<std::uint32_t, 16> ranks{};
    std::uint32_t npc_marker = 0;
};
std::optional<GroupLeadershipUpdate> decode_group_leadership_update(const AppPacket& p);
AppPacket encode(const GroupLeadershipUpdate& v); // server direction, for tests
// OP_DelegateAbility (0x76b8, 92 bytes, DelegateAbility_Struct): u32 ability (1 mark NPC, 2 main assist), u32 member
// number, u32 action (1 removes), 2 × u32, u32 the delegate's spawn id, u32, char[64] its name. OP_RaidDelegateAbility
// (0x2b33) is the same 92 bytes with the raid's abilities (3 main assist, 4 marker, 5 the third set, CRaid_
// SetMemberWaypointer's). EQEmu numbers live assist slots 1..3, but sends 0 for its zone-in assist resend and all
// markers; names/entity IDs identify delegates, and a zero-number assist does not establish a slot number.
// The client sends both with the ability and the name only (Leadership_ActivateAbility 4 / 5: OP_DelegateAbility 1 /
// 2 with the target's name; Send_OP_RaidDelegateAbility @0x55e5b0: the ability and the member's name; the original
// leaves +8 and +0x18 as stack bytes, 0 here).
struct DelegateAbility {
    std::uint32_t ability = 0, member = 0, action = 0, spawn_id = 0;
    std::string name;
};
std::optional<DelegateAbility> decode_delegate_ability(const AppPacket& p);
AppPacket encode(const DelegateAbility& v, Op op = Op::DelegateAbility);
// OP_MarkNPC (0x1fb5) / OP_MarkRaidNPC (0x5a58), zone → client (MarkNPC_Struct): u32 spawn id, u32 number (0 unmarks,
// 1..3), char[64] name.
struct MarkNpc {
    std::uint32_t spawn_id = 0, number = 0;
    std::string name;
};
std::optional<MarkNpc> decode_mark_npc(const AppPacket& p);
AppPacket encode_mark_npc(Op op, const MarkNpc& v); // server direction, for tests
// OP_InspectBuffs (0x486c, 336 bytes): 42 spell ids, then their 42 tick counts.
struct InspectBuffs {
    std::array<std::int32_t, 42> spells{}, ticks{};
};
std::optional<InspectBuffs> decode_inspect_buffs(const AppPacket& p);
AppPacket encode(const InspectBuffs& v); // server direction, for tests
// The minutes the inspect line shows for a buff's tick count: (ticks x 3 + 15) x 2 / 60 in the original's 32 bits,
// so a tick count past 2^31 / 6 wraps there as it does here (core::wrapped_div, never a plain divide of a product).
std::int32_t inspect_buff_minutes(std::int32_t ticks);
// OP_DoGroupLeadershipAbility (0x6eae, 8 bytes): u32 ability, u32 parameter.
AppPacket encode_do_group_leadership_ability(std::uint32_t ability, std::uint32_t parameter);
// OP_XTargetResponse, zone → client (ExtendedTargetList_UnSerializeSlots @0x548a10): u32 max (the list's size), u32
// count, then per entry i32 slot, u8 status, u32 spawn id and a NUL-terminated name (kept to 63 characters). A packet
// cut short keeps the entries read before the cut; nothing without the max.
struct XTargetEntry {
    std::int32_t slot = 0;
    std::uint8_t status = 0;
    std::uint32_t spawn_id = 0;
    std::string name;
};
struct XTargetResponse {
    std::uint32_t max = 0;
    std::vector<XTargetEntry> entries;
};
std::optional<XTargetResponse> decode_xtarget_response(const AppPacket& p);
// OP_XTargetRequest, client → zone (ExtendedTargetList_SerializeSlot): u32 1 (one slot), u32 slot, u32 type, the
// slot's name NUL-terminated.
AppPacket encode_xtarget_request(std::uint32_t slot, std::uint32_t type, std::string_view name);

} // namespace hailstone::net
