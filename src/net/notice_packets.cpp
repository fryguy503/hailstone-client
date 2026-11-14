#include "net/notice_packets.h"

#include "core/byte_io.h"
#include "net/packet_io.h"

namespace hailstone::net {

using detail::decode_with;

namespace {

std::string field(ByteReader& r, std::size_t n) { return r.fixed_string(n); }

// The offer echoed with a u32 flag at `at`.
AppPacket echo(const AppPacket& offer, std::size_t at, bool yes, Op op) {
    AppPacket a = offer;
    a.opcode = to_wire(op);
    if (a.data.size() >= at + 4) {
        a.data[at] = std::byte{static_cast<unsigned char>(yes ? 1 : 0)};
        a.data[at + 1] = a.data[at + 2] = a.data[at + 3] = std::byte{0};
    }
    return a;
}

} // namespace

std::optional<Stun> decode_stun(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        Stun s;
        s.duration_ms = r.le<std::uint32_t>();
        if (r.remaining() >= 1) s.lean = r.le<std::uint8_t>() != 0;
        return s;
    });
}

std::optional<ItemRecastDelay> decode_item_recast_delay(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        ItemRecastDelay v;
        v.delay_s = r.le<std::uint32_t>();
        v.type = r.le<std::uint32_t>();
        if (r.remaining() >= 1) v.ignore_casting = r.le<std::uint8_t>() != 0;
        return v;
    });
}

std::optional<Charm> decode_charm(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        Charm v;
        v.owner = r.le<std::uint32_t>();
        v.pet = r.le<std::uint32_t>();
        v.on = r.le<std::uint32_t>() == 1;
        return v;
    });
}

std::optional<Untargetable> decode_untargetable(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        Untargetable v;
        v.spawn = r.le<std::uint32_t>();
        v.targetable = r.le<std::uint32_t>() != 0;
        return v;
    });
}

std::optional<std::uint32_t> decode_yell_for_help(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) { return r.le<std::uint32_t>(); });
}

std::optional<std::string> decode_weblink(const AppPacket& p) {
    std::string url;
    for (std::byte b : p.data) {
        if (b == std::byte{0}) break;
        url.push_back(static_cast<char>(b));
    }
    if (url.empty()) return std::nullopt;
    return url;
}

std::optional<std::vector<std::uint32_t>> decode_disciplines(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        std::vector<std::uint32_t> v;
        while (r.remaining() >= 4) v.push_back(r.le<std::uint32_t>());
        return v;
    });
}

std::optional<Translocate> decode_translocate(const AppPacket& p) {
    if (p.data.size() < 92) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        Translocate v;
        v.zone = r.le<std::uint32_t>();
        v.spell = r.le<std::uint32_t>();
        r.skip(4);
        v.caster = field(r, 64);
        return v;
    });
}

AppPacket answer_translocate(const AppPacket& offer, bool go) { return echo(offer, 88, go, Op::Translocate); }

std::optional<RezzRequest> decode_rezz_request(const AppPacket& p) {
    if (p.data.size() < 236) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        RezzRequest v;
        r.skip(4);
        v.zone = r.le<std::uint16_t>();
        v.instance = r.le<std::uint16_t>();
        r.skip(12 + 4 + 64 + 4);
        v.rezzer = field(r, 64);
        v.spell = r.le<std::uint32_t>();
        v.corpse = field(r, 64);
        return v;
    });
}

AppPacket answer_rezz(const AppPacket& offer, bool accept) { return echo(offer, 224, accept, Op::RezzAnswer); }

std::optional<SharedTaskInvite> decode_shared_task_invite(const AppPacket& p) {
    if (p.data.size() < 136) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        SharedTaskInvite v;
        r.skip(4);
        v.invite_id = r.le<std::int32_t>();
        v.task = field(r, 64);
        v.inviter = field(r, 64);
        return v;
    });
}

AppPacket encode_shared_task_invite_response(std::int32_t invite_id, bool accepted) {
    ByteWriter w;
    w.le<std::int32_t>(0);
    w.le(invite_id);
    w.le<std::uint8_t>(accepted ? 1 : 0);
    w.zeros(3);
    return make_packet(Op::SharedTaskInviteResponse, std::move(w));
}

AppPacket encode_group_role(const GroupRole& v, std::uint32_t group_id) {
    ByteWriter w;
    w.fixed_string(std::string_view(v.member).substr(0, 63), 64);
    w.fixed_string(std::string_view(v.leader).substr(0, 63), 64);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le(group_id);
    w.le(v.role);
    w.le(v.on);
    w.zeros(3);
    return make_packet(Op::GroupRoles, std::move(w));
}

std::optional<GroupRole> decode_group_role(const AppPacket& p) {
    if (p.data.size() < 0x91) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GroupRole v;
        v.member = field(r, 64);
        v.leader = field(r, 64);
        r.skip(12);
        v.role = r.le<std::int32_t>();
        v.on = r.le<std::uint8_t>();
        return v;
    });
}

std::optional<GuildRosterChange> decode_guild_roster_change(const AppPacket& p) {
    Op op = from_wire(p.opcode);
    return decode_with(p, [&](ByteReader& r) {
        GuildRosterChange v;
        v.guild_id = r.le<std::uint32_t>();
        switch (op) {
        case Op::GuildMemberAdd:
            v.kind = GuildRosterChange::Add;
            r.skip(12);
            v.level = r.le<std::uint32_t>();
            v.class_ = r.le<std::uint32_t>();
            v.rank = r.le<std::uint32_t>();
            r.skip(4);
            v.zone_id = r.le<std::uint32_t>();
            v.last_on = r.le<std::uint32_t>();
            v.name = field(r, 64);
            break;
        case Op::GuildMemberDelete:
            v.kind = GuildRosterChange::Delete;
            v.name = field(r, 64);
            break;
        case Op::GuildMemberLevel:
            v.kind = GuildRosterChange::Level;
            v.name = field(r, 64);
            v.level = r.le<std::uint32_t>();
            break;
        case Op::GuildMemberPublicNote:
            v.kind = GuildRosterChange::Note;
            v.name = field(r, 64);
            v.note = r.fixed_string(std::min<std::size_t>(256, r.remaining()));
            break;
        case Op::GuildMemberDetails:
            v.kind = GuildRosterChange::Details;
            v.name = field(r, 64);
            v.zone_id = r.le<std::uint32_t>();
            v.last_on = r.le<std::uint32_t>();
            v.offline = r.le<std::uint32_t>() != 0;
            break;
        default: { // SetGuildRank / GuildMemberRankAltBanker (one wire value)
            v.kind = GuildRosterChange::Rank;
            v.rank = r.le<std::uint32_t>();
            v.name = field(r, 64);
            std::uint32_t flags = r.le<std::uint32_t>();
            v.alt = (flags & 2) != 0;
            v.banker = (flags & 1) != 0;
            break;
        }
        }
        return v;
    });
}

std::optional<CoinFind> decode_begging(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        r.skip(12);
        CoinFind v;
        v.kind = r.le<std::uint32_t>();
        v.amount = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<CoinFind> decode_pick_pocket(const AppPacket& p) { return decode_begging(p); } // the same offsets

std::optional<std::uint16_t> decode_bind_wound(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        r.skip(4);
        return r.le<std::uint16_t>();
    });
}

} // namespace hailstone::net
