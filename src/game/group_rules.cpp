#include "game/group_rules.h"

#include <cctype>

namespace hailstone::game {

bool group_add_member(GroupSlotsModel& g, int member, std::int16_t kind, bool name_in_group, int owner, GroupCalls& c) {
    if (member < 0) return false;
    if (name_in_group) return false;
    bool added = false;
    for (int k = 0; k < 6; ++k) {
        if (g.slots[static_cast<std::size_t>(k)] != -1) continue;
        g.slots[static_cast<std::size_t>(k)] = member;
        if (kind == 1 && owner != -1) c.swap_members(member, owner);
        c.attach_spawn(member);
        c.message(1, member);
        added = true;
        break;
    }
    if (g.leader == -1) c.set_leader(member);
    return added;
}

bool group_remove_member(GroupSlotsModel& g, int member, GroupCalls& c) {
    if (member < 0) return false;
    bool removed = false;
    for (int k = 0; k < 6; ++k) {
        if (g.slots[static_cast<std::size_t>(k)] != member) continue;
        g.slots[static_cast<std::size_t>(k)] = -1;
        c.message(2, member);
        c.group_vf1c(member);
        removed = true;
        break;
    }
    c.group_vf18(member);
    if (removed) c.member_vf18(member);
    return removed;
}

bool group_set_leader(GroupSlotsModel& g, int member, GroupCalls& c) {
    if (member < 0) return false;
    g.leader = member;
    c.message(0, member);
    return true;
}

GroupLine group_message_line(std::uint32_t code, bool about_me) {
    switch (code) {
    case 0: return about_me ? GroupLine{kStrYouAreLeader, false} : GroupLine{kStrIsLeader, true};
    case 1: return {kStrJoinedGroup, true};
    case 2: return about_me ? GroupLine{} : GroupLine{kStrLeftGroup, true};
    default: return {};
    }
}

namespace {
bool iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
} // namespace

void handle_group_world_message(bool& grouped, const GroupWorldPacket& p, GroupWorldCalls& c) {
    if (p.opcode == kOpGroupAcknowledge) {
        c.say(kStrYouJoined);
    } else if (p.opcode == kOpGroupDisbandYou) {
        c.say(kStrYouWereRemoved);
        if (grouped) {
            c.delete_group();
            grouped = false;
            c.armor_filters_after_leaving();
        }
    }
    if (!grouped) return;
    switch (p.opcode) {
    case kOpGroupUpdate: {
        if (c.member_by_name(p.name2) != -1) return;
        int m = c.new_member(p);
        if (c.add_member(m)) c.apply_armor_filter(p.name2);
        return;
    }
    case kOpGroupLeaderChange: {
        int m = c.member_by_name(p.name2);
        if (m != -1) c.set_leader(m);
        return;
    }
    case kOpGroupDelete:
    case kOpGroupDelete2:
        c.pc_delete_group();
        return;
    case kOpGroupDisbandOther:
        if (c.remove_member_by_name(p.name2)) c.apply_armor_filter(p.name2);
        return;
    case kOpGroupRename: {
        int m = c.member_by_name(p.name);
        if (m != -1) c.rename(m, p.name2);
        return;
    }
    case kOpGroupRoles: {
        int m = c.member_by_name(p.name);
        if (m == -1) return;
        auto leader = c.leader_name();
        if (!leader || !iequal(*leader, p.name2)) return;
        c.set_member_role(m, p.role, p.role_on);
        return;
    }
    default:
        return;
    }
}

void group_target_member(GroupTargetWorld& w, std::int32_t n, bool target_pet) {
    w.selected = -1;
    int spawn = -1;
    if (n <= -1) {
        w.target = -1;
        return;
    }
    std::int32_t seen = 0;
    for (std::size_t k = 0; k < 6; ++k) {
        if (w.members[k] && ++seen > n) {
            w.selected = n;
            spawn = *w.members[k];
            break;
        }
    }
    if (target_pet && spawn != -1) {
        std::int32_t id = w.pet_id_of(spawn);
        if (id != -1) {
            int t = w.spawn_by_id(id);
            if (t != -1) {
                w.selected = -1;
                w.target = t;
                return;
            }
        }
    }
    w.target = spawn;
}

void target_self_or_pet(GroupTargetWorld& w, bool grouped, std::int32_t n, bool target_pet, int own) {
    if (grouped) return group_target_member(w, n, target_pet);
    if (n != 0) {
        w.selected = -1;
        w.target = -1;
        return;
    }
    if (target_pet) {
        std::int32_t id = own != -1 ? w.pet_id_of(own) : -1;
        if (own != -1 && id != -1) {
            int t = w.spawn_by_id(id);
            if (t != -1) {
                w.selected = -1;
                w.target = t;
            }
            return;
        }
    }
    w.selected = 0;
    w.target = w.player;
}

GroupInviteResult handle_group_invite(GroupInviteState& st, const GroupInviteInput& in) {
    using F = GroupInviteCall::Fn;
    GroupInviteResult r;
    r.calls.push_back({F::FormatMessage, static_cast<std::int32_t>(kStrInvitesYou)});
    r.calls.push_back({F::Chat, static_cast<std::int32_t>(kGroupChatColor)});
    std::uint8_t reason;
    if (st.invite_pending || in.player_invited) {
        reason = kInviteAlreadyInvited;
    } else if (in.raid_invited) {
        reason = kInviteRaidInvited;
    } else if (in.in_raid && (!in.inviter_raid_member || in.raid_group_leader)) {
        reason = kInviteRaid;
    } else {
        r.calls.push_back({F::String, static_cast<std::int32_t>(kStrToJoinFollow)});
        r.calls.push_back({F::Chat, static_cast<std::int32_t>(kGroupChatColor)});
        st.invite_pending = true;
        r.calls.push_back({F::PlayerSetInvited, 1});
        st.pending_inviter = in.inviter;
        st.pending_word_148 = in.word_148;
        if (in.opcode == kOpGroupInvite) st.follow_with_group_follow = true;
        return r;
    }
    r.cancel = GroupInviteCancel{in.inviter, in.my_name, reason};
    if (!in.world_connection) {
        if (!in.quiet_debug) r.calls.push_back({F::DebugLog, static_cast<std::int32_t>(kOpCancelInvite)});
    } else {
        r.calls.push_back({F::Send, 154});
        if (!in.send_ok) r.calls.push_back({F::CloseWorldConnection, 0});
    }
    return r;
}

} // namespace hailstone::game
