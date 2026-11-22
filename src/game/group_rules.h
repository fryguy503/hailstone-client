#pragma once

// The group's member list as the original changes it, and the chat lines it
// prints, following the original and checked against CGroupBase_AddMember /
// RemoveMember / SetLeader.jsonl and HandleWorldMessage_Group.jsonl.
//  - AddMember (CGroupBase vf1 @0x7d6150): refuses a null member or a name
//    already in the group; takes the first free of six slots, a mercenary
//    then changes places with its owner (CGroupBase_SwapMembers); "%1 has
//    joined the group." (1399); a group without a leader makes it the leader.
//  - RemoveMember (vf2 @0x7d5b90): "%1 has left the group." (12005), nothing
//    for ourselves.
//  - SetLeader (vf5 @0x7d5a60): "You are now the leader of your group." (5040)
//    or "%1 is now the leader of your group." (5041).
//  - HandleWorldMessage_Group @0x578000: OP_GroupAcknowledge says 12004,
//    OP_GroupDisbandYou 12001 and drops the group, colour 0x111.
// Members are opaque handles (-1 none); GroupCalls carries the virtual calls.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hailstone::game {

inline constexpr std::uint32_t kGroupChatColor = 0x111;
inline constexpr std::uint32_t kStrJoinedGroup = 1399, kStrLeftGroup = 12005, kStrYouAreLeader = 5040,
                               kStrIsLeader = 5041, kStrYouJoined = 12004, kStrYouWereRemoved = 12001;

struct GroupSlotsModel {
    std::array<int, 6> slots{-1, -1, -1, -1, -1, -1}; // CGroupBase +4
    int leader = -1;                                   // CGroupBase +0x1c
};

class GroupCalls {
public:
    virtual ~GroupCalls() = default;
    virtual void attach_spawn(int member) = 0;            // vtable +0x2c
    virtual void message(std::uint32_t code, int member) = 0; // vtable +0x20 (0x54dd70): 0 leader, 1 joined, 2 left
    virtual void set_leader(int member) = 0;              // vtable +0x14 (CGroup: SetLeader)
    virtual void group_vf18(int member) = 0;
    virtual void group_vf1c(int member) = 0;
    virtual void member_vf18(int member) = 0;             // the removed member's own +0x18
    virtual void swap_members(int member, int owner) = 0; // CGroupBase_SwapMembers
};

// `name_in_group`: GetMemberByName(member's name) found one; `owner`: the
// member GetMemberByName(member's owner name) returns, -1 none; `kind` the
// member's +8 (1: a mercenary).
bool group_add_member(GroupSlotsModel& g, int member, std::int16_t kind, bool name_in_group, int owner, GroupCalls& c);
bool group_remove_member(GroupSlotsModel& g, int member, GroupCalls& c);
bool group_set_leader(GroupSlotsModel& g, int member, GroupCalls& c);

// 0x54dd70's line for a code: the eqstr id and whether it takes the
// member's name; 0 for nothing (code 2 about ourselves prints an empty line).
struct GroupLine {
    std::uint32_t eqstr = 0;
    bool with_name = false;
};
GroupLine group_message_line(std::uint32_t code, bool about_me);

// HandleWorldMessage_Group's packet (GroupGeneric / GroupJoin, 148 bytes).
struct GroupWorldPacket {
    std::uint32_t opcode = 0;
    std::string name;          // +0
    std::string name2;         // +0x40
    std::int8_t merc = 0;      // +0x80
    std::int32_t field_84 = 0; // +0x84
    std::int32_t role = 0;     // +0x8c
    std::uint8_t role_on = 0;  // +0x90
};

inline constexpr std::uint32_t kOpGroupAcknowledge = 0x7323, kOpGroupDisbandYou = 0x1ae5, kOpGroupDelete = 0xf6c,
                               kOpGroupDelete2 = 0x4ced, kOpGroupLeaderChange = 0x21b4, kOpGroupUpdate = 0x3abb,
                               kOpGroupRename = 0x5c24, kOpGroupRoles = 0x70e2, kOpGroupDisbandOther = 0x74da;

class GroupWorldCalls {
public:
    virtual ~GroupWorldCalls() = default;
    virtual void say(std::uint32_t eqstr) = 0;                      // BuildStringFromEQSTR + dsp_chat, colour 0x111
    virtual void delete_group() = 0;                                // the group's virtual +0 with 1
    virtual void armor_filters_after_leaving() = 0;                 // 0x597750 (local PcClient)
    virtual int member_by_name(const std::string& name) = 0;        // CGroup_GetMemberByName, -1 none
    // A new CGroupMember: name2, merc flag into +8, +0x84 into +0x10, owner name, its spawn by name.
    virtual int new_member(const GroupWorldPacket& p) = 0;
    virtual bool add_member(int member) = 0;                        // the group's virtual +4
    virtual void apply_armor_filter(const std::string& name) = 0;  // PlayerClient_ApplyNewArmorFilterGroupMember
    virtual void set_leader(int member) = 0;                        // the group's virtual +0x14
    virtual void pc_delete_group() = 0;                             // PcClient_DeleteGroup
    virtual bool remove_member_by_name(const std::string& name) = 0; // 0x54daf0
    virtual void rename(int member, const std::string& name) = 0;
    virtual std::optional<std::string> leader_name() = 0;           // CGroupBase +0x1c's name
    virtual void set_member_role(int member, std::int32_t role, std::uint8_t on) = 0; // CGroupBase_SetMemberRole
};

// HandleWorldMessage_Group @0x578000 for the local PcClient; `grouped` is +0x31cc non-null.
void handle_group_world_message(bool& grouped, const GroupWorldPacket& p, GroupWorldCalls& c);

// ---- targeting members (group window clicks, F1..F6) and the selected member
// (PlayerClient +0x1214) /disband reads. Spawns are handles (-1 none).
struct GroupTargetWorld {
    // Per slot: nullopt for an empty slot, else the member's spawn (+0x28; -1 none).
    std::array<std::optional<int>, 6> members{};
    int selected = -1;   // PlayerClient +0x1214
    int target = -1;     // pinstTarget
    int player = -1;     // the local player's spawn
    // A spawn's PetID (+0x2b4; -1 none) and GetSpawnByID.
    std::function<std::int32_t(int owner)> pet_id_of;
    std::function<int(std::int32_t id)> spawn_by_id;
};

// 0x54da00 (CGroup: target member n): the (n+1)-th occupied slot's spawn,
// selecting n (the ordinal among occupied slots); with `target_pet`, that
// spawn's pet if present (selection cleared).
void group_target_member(GroupTargetWorld& w, std::int32_t n, bool target_pet);
// LocalPlayer_TargetSelfOrPet @0x58d230: in a group CGroup_TargetMember;
// without one n 0 targets ourselves (selecting 0) or, with `target_pet`, our
// pet (`own` is the character's +0x2dd0 spawn).
void target_self_or_pet(GroupTargetWorld& w, bool grouped, std::int32_t n, bool target_pet, int own);

// ---- an incoming invitation (Handle_OP_GroupInvite2.jsonl)

inline constexpr std::uint32_t kOpGroupInvite = 0x6110, kOpGroupInvite2 = 0x32c2, kOpCancelInvite = 0x2a50;
inline constexpr std::uint32_t kStrInvitesYou = 12280, kStrToJoinFollow = 12281;
inline constexpr std::uint8_t kInviteAlreadyInvited = 1, kInviteRaidInvited = 4, kInviteRaid = 5;

// The globals the accept path fills for the Follow button (Send_OP_GroupFollow2).
struct GroupInviteState {
    bool invite_pending = false;           // 0xe634e0
    bool follow_with_group_follow = false; // 0xe63520: Follow answers with OP_GroupFollow (0x1649), else 0x2060
    std::uint32_t pending_word_148 = 0;    // 0xe63524: echoed at +0x94 of the 152-byte GroupFollow
    std::string pending_inviter;           // __Inviter (0xe63528)
};
struct GroupInviteInput {
    std::uint32_t opcode = kOpGroupInvite; // 0x6110 or 0x32c2
    std::string inviter;                   // packet +0x40
    std::string my_name;                   // the local player's name
    std::uint32_t word_148 = 0;            // packet +0x94 (read as is; past EQEmu's 148 bytes: 0 here)
    bool player_invited = false;           // PlayerClient::IsInvited (+0x1210)
    bool raid_invited = false;             // CRaid::IsInvited
    bool in_raid = false;                  // CRaid::IsInRaid
    bool inviter_raid_member = false;      // CRaid::IsRaidMember(inviter)
    bool raid_group_leader = false;        // CRaid::IsRaidGroupLeader
    bool world_connection = true, send_ok = true, quiet_debug = false;
};
struct GroupInviteCall {
    enum class Fn : std::uint8_t { FormatMessage, Chat, String, PlayerSetInvited, Send, CloseWorldConnection, DebugLog } fn{};
    std::int32_t value = 0; // eqstr, colour, set value, send length or debug opcode
};
// OP_CancelInvite (0x2a50, 152 bytes): the inviter, our name and the reason at +0x94.
struct GroupInviteCancel {
    std::string inviter, me;
    std::uint8_t reason = 0;
};
struct GroupInviteResult {
    std::vector<GroupInviteCall> calls;
    std::optional<GroupInviteCancel> cancel;
};
// Handle_OP_GroupInvite2 @0x525400 (both opcodes): "%1 invites you to join a group." (12280), then a pending
// invitation or PlayerClient::IsInvited declines with 1, CRaid::IsInvited with 4, being in a raid when the inviter
// is not a member (or when we are a raid group leader) with 5; otherwise 12281, the pending invitation, SetInvited(1),
// the inviter and the u32 at +0x94 kept, and OP_GroupInvite (0x6110) makes Follow answer with OP_GroupFollow.
GroupInviteResult handle_group_invite(GroupInviteState& st, const GroupInviteInput& in);

} // namespace hailstone::game
