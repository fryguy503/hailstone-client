#include "net/group_packets.h"

#include <algorithm>
#include <stdexcept>

#include "core/byte_io.h"
#include "core/wrap_math.h"
#include "net/packet_io.h"

namespace hailstone::net {

using detail::decode_with;

namespace {

// A char[64] name: at most 63 characters, so it stays NUL-terminated.
void name64(ByteWriter& w, const std::string& s) { w.fixed_string(s.substr(0, 63), 64); }

} // namespace

// ---------------------------------------------------------------- invite

AppPacket encode(const GroupInvite& v, bool second_opcode) {
    ByteWriter w;
    name64(w, v.invitee);
    name64(w, v.inviter);
    for (std::uint32_t u : v.unknown) w.le<std::uint32_t>(u);
    return make_packet(second_opcode ? Op::GroupInvite2 : Op::GroupInvite, std::move(w));
}

std::optional<GroupInvite> decode_group_invite(const AppPacket& p) {
    if (p.data.size() < kGroupInviteSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GroupInvite v;
        v.invitee = r.fixed_string(64);
        v.inviter = r.fixed_string(64);
        for (std::uint32_t& u : v.unknown) u = r.le<std::uint32_t>();
        if (r.remaining() >= 4) v.follow_cookie = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const GroupFollow& v, bool second_opcode) {
    ByteWriter w;
    name64(w, v.inviter);
    name64(w, v.invitee);
    for (std::uint32_t u : v.unknown) w.le<std::uint32_t>(u);
    w.le<std::uint32_t>(v.unknown148);
    return make_packet(second_opcode ? Op::GroupFollow2 : Op::GroupFollow, std::move(w));
}

std::optional<GroupFollow> decode_group_follow(const AppPacket& p) {
    if (p.data.size() < kGroupFollowSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GroupFollow v;
        v.inviter = r.fixed_string(64);
        v.invitee = r.fixed_string(64);
        for (std::uint32_t& u : v.unknown) u = r.le<std::uint32_t>();
        v.unknown148 = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const GroupCancel& v) {
    ByteWriter w;
    name64(w, v.inviter);
    name64(w, v.invitee);
    w.zeros(20);
    w.le<std::uint32_t>(v.reason);
    return make_packet(Op::CancelInvite, std::move(w));
}

std::optional<GroupCancel> decode_group_cancel(const AppPacket& p) {
    if (p.data.size() < kGroupCancelSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GroupCancel v;
        v.inviter = r.fixed_string(64);
        v.invitee = r.fixed_string(64);
        r.skip(20);
        v.reason = r.le<std::uint32_t>();
        return v;
    });
}

// ---------------------------------------------------------------- membership

AppPacket encode_group_generic(Op op, const GroupGeneric& v) {
    ByteWriter w;
    name64(w, v.name1);
    name64(w, v.name2);
    w.zeros(20);
    return make_packet(op, std::move(w));
}

std::optional<GroupGeneric> decode_group_generic(const AppPacket& p) {
    if (p.data.size() < 128) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GroupGeneric v;
        v.name1 = r.fixed_string(64);
        v.name2 = r.fixed_string(64);
        return v;
    });
}

AppPacket encode(const GroupJoin& v) {
    ByteWriter w;
    name64(w, v.owner_name);
    name64(w, v.member_name);
    w.le<std::uint8_t>(v.merc);
    w.zeros(3);
    w.le<std::uint32_t>(v.level);
    w.zeros(12);
    return make_packet(Op::GroupUpdate, std::move(w));
}

std::optional<GroupJoin> decode_group_join(const AppPacket& p) {
    if (p.data.size() < 136) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GroupJoin v;
        v.owner_name = r.fixed_string(64);
        v.member_name = r.fixed_string(64);
        v.merc = r.le<std::uint8_t>();
        r.skip(3);
        v.level = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const GroupUpdateB& v) {
    ByteWriter w;
    w.le<std::uint32_t>(v.group_id);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(v.members.size()));
    detail::write_cstring(w, v.leader);
    for (const auto& m : v.members) {
        w.le<std::uint32_t>(m.slot);
        detail::write_cstring(w, m.name);
        w.le<std::uint16_t>(m.merc);
        detail::write_cstring(w, m.owner_name);
        w.le<std::uint32_t>(m.level);
        w.le<std::uint8_t>(m.main_tank);
        w.le<std::uint8_t>(m.main_assist);
        w.le<std::uint8_t>(m.puller);
        w.le<std::uint32_t>(m.offline);
        w.le<std::uint32_t>(m.timestamp);
    }
    return make_packet(Op::GroupUpdateB, std::move(w));
}

std::optional<GroupUpdateB> decode_group_update_b(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        GroupUpdateB v;
        v.group_id = r.le<std::uint32_t>();
        std::uint32_t count = r.le<std::uint32_t>();
        v.leader = r.cstring();
        // A group has at most six members; anything more is a broken packet.
        if (count > 6) throw std::length_error("group update: member count");
        for (std::uint32_t i = 0; i < count; ++i) {
            GroupUpdateMember m;
            m.slot = r.le<std::uint32_t>();
            m.name = r.cstring();
            m.merc = r.le<std::uint16_t>();
            m.owner_name = r.cstring();
            m.level = r.le<std::uint32_t>();
            m.main_tank = r.le<std::uint8_t>();
            m.main_assist = r.le<std::uint8_t>();
            m.puller = r.le<std::uint8_t>();
            m.offline = r.le<std::uint32_t>();
            m.timestamp = r.le<std::uint32_t>();
            v.members.push_back(std::move(m));
        }
        return v;
    });
}

AppPacket encode_group_leader_change(const std::string& leader) {
    ByteWriter w;
    w.zeros(64);
    name64(w, leader);
    w.zeros(20);
    return make_packet(Op::GroupLeaderChange, std::move(w));
}

std::optional<std::string> decode_group_leader_change(const AppPacket& p) {
    if (p.data.size() < 128) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        r.skip(64);
        return r.fixed_string(64);
    });
}

AppPacket encode(const GroupMakeLeader& v) {
    ByteWriter w;
    w.le<std::uint32_t>(8);
    name64(w, v.current_leader);
    name64(w, v.new_leader);
    w.zeros(kGroupMakeLeaderSize - 132);
    return make_packet(Op::GroupMakeLeader, std::move(w));
}

std::optional<GroupMakeLeader> decode_group_make_leader(const AppPacket& p) {
    if (p.data.size() < 132) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GroupMakeLeader v;
        r.skip(4);
        v.current_leader = r.fixed_string(64);
        v.new_leader = r.fixed_string(64);
        return v;
    });
}

AppPacket encode_group_mentor(std::int32_t percent, const std::string& name) {
    ByteWriter w;
    w.le<std::int32_t>(percent);
    name64(w, name);
    return make_packet(Op::GroupMentor, std::move(w));
}

AppPacket encode_mob_percent(Op op, const MobPercent& v) {
    ByteWriter w;
    w.le<std::uint16_t>(v.spawn_id);
    w.le<std::uint8_t>(v.percent);
    return make_packet(op, std::move(w));
}

std::optional<MobPercent> decode_mob_percent(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        MobPercent v;
        v.spawn_id = r.le<std::uint16_t>();
        v.percent = r.le<std::uint8_t>();
        return v;
    });
}

AppPacket encode_leadership_exp_toggle(bool on) {
    ByteWriter w;
    w.le<std::uint8_t>(on ? 1 : 0);
    return make_packet(Op::LeadershipExpToggle, std::move(w));
}

AppPacket encode_purchase_leadership(std::uint32_t ability) {
    ByteWriter w;
    w.le(ability);
    return make_packet(Op::PurchaseLeadershipAA, std::move(w));
}

std::optional<LeadershipRank> decode_leadership_rank(const AppPacket& p) {
    if (p.data.size() < 12) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        LeadershipRank v;
        v.ability = r.le<std::uint32_t>();
        v.rank = r.le<std::uint32_t>();
        v.points_left = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<LeadershipExp> decode_leadership_exp(const AppPacket& p) {
    if (p.data.size() < 28) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        LeadershipExp v;
        v.group_exp = r.f64();
        v.group_points = r.le<std::uint32_t>();
        r.le<std::uint32_t>();
        v.raid_exp = r.f64();
        v.raid_points = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const LeadershipExp& v) {
    ByteWriter w;
    w.f64(v.group_exp);
    w.le(v.group_points);
    w.le<std::uint32_t>(0);
    w.f64(v.raid_exp);
    w.le(v.raid_points);
    return make_packet(Op::LeadershipExpUpdate, std::move(w));
}

std::optional<GroupLeadershipUpdate> decode_group_leadership_update(const AppPacket& p) {
    if (p.data.size() < 268) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GroupLeadershipUpdate v;
        r.skip(4);
        for (auto& x : v.ranks) x = r.le<std::uint32_t>();
        r.skip(264 - 68);
        v.npc_marker = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const GroupLeadershipUpdate& v) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    for (auto x : v.ranks) w.le(x);
    w.zeros(264 - 68);
    w.le(v.npc_marker);
    w.zeros(320 - 268);
    return make_packet(Op::GroupLeadershipAAUpdate, std::move(w));
}

std::optional<DelegateAbility> decode_delegate_ability(const AppPacket& p) {
    if (p.data.size() < 92) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        DelegateAbility v;
        v.ability = r.le<std::uint32_t>();
        v.member = r.le<std::uint32_t>();
        v.action = r.le<std::uint32_t>();
        r.skip(8);
        v.spawn_id = r.le<std::uint32_t>();
        r.skip(4);
        v.name = r.fixed_string(64);
        return v;
    });
}

AppPacket encode(const DelegateAbility& v, Op op) {
    ByteWriter w;
    w.le(v.ability);
    w.le(v.member);
    w.le(v.action);
    w.zeros(8);
    w.le(v.spawn_id);
    w.zeros(4);
    name64(w, v.name);
    return make_packet(op, std::move(w));
}

std::optional<MarkNpc> decode_mark_npc(const AppPacket& p) {
    if (p.data.size() < 8) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        MarkNpc v;
        v.spawn_id = r.le<std::uint32_t>();
        v.number = r.le<std::uint32_t>();
        v.name = r.fixed_string(std::min<std::size_t>(64, r.remaining()));
        return v;
    });
}

AppPacket encode_mark_npc(Op op, const MarkNpc& v) {
    ByteWriter w;
    w.le(v.spawn_id);
    w.le(v.number);
    name64(w, v.name);
    return make_packet(op, std::move(w));
}

std::int32_t inspect_buff_minutes(std::int32_t ticks) {
    return core::wrapped_div((static_cast<std::uint32_t>(ticks) * 3u + 15u) * 2u, 60);
}

std::optional<InspectBuffs> decode_inspect_buffs(const AppPacket& p) {
    if (p.data.size() < 336) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        InspectBuffs v;
        for (auto& x : v.spells) x = r.le<std::int32_t>();
        for (auto& x : v.ticks) x = r.le<std::int32_t>();
        return v;
    });
}

AppPacket encode(const InspectBuffs& v) {
    ByteWriter w;
    for (auto x : v.spells) w.le(x);
    for (auto x : v.ticks) w.le(x);
    return make_packet(Op::InspectBuffs, std::move(w));
}

AppPacket encode_do_group_leadership_ability(std::uint32_t ability, std::uint32_t parameter) {
    ByteWriter w;
    w.le(ability);
    w.le(parameter);
    return make_packet(Op::DoGroupLeadershipAbility, std::move(w));
}

std::optional<XTargetResponse> decode_xtarget_response(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    ByteReader r(p.data);
    XTargetResponse v;
    v.max = r.le<std::uint32_t>();
    try {
        auto count = r.le<std::uint32_t>();
        for (std::uint32_t i = 0; i < count && r.remaining() >= 9; ++i) {
            XTargetEntry e;
            e.slot = r.le<std::int32_t>();
            e.status = r.le<std::uint8_t>();
            e.spawn_id = r.le<std::uint32_t>();
            if (r.remaining()) e.name = r.cstring();
            if (e.name.size() > 63) e.name.resize(63);
            v.entries.push_back(std::move(e));
        }
    } catch (const std::out_of_range&) {
        // Cut short: the entries before the cut stand.
    }
    return v;
}

AppPacket encode_xtarget_request(std::uint32_t slot, std::uint32_t type, std::string_view name) {
    ByteWriter w;
    w.le<std::uint32_t>(1);
    w.le(slot);
    w.le(type);
    w.fixed_string(name, name.size() + 1); // NUL-terminated
    return make_packet(Op::XTargetRequest, std::move(w));
}

} // namespace hailstone::net
