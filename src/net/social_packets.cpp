#include "net/social_packets.h"

#include "core/byte_io.h"
#include "net/packet_io.h"

#include <algorithm>
#include <stdexcept>

namespace hailstone::net {

using detail::decode_with;
using detail::write_cstring;

namespace {

void name64(ByteWriter& w, const std::string& s) { w.fixed_string(s.substr(0, 63), 64); }

std::size_t pad_to(ByteWriter& w, std::size_t size) {
    std::size_t n = w.size();
    if (n < size) w.zeros(size - n);
    return size;
}

} // namespace

// ---------------------------------------------------------------- /who

AppPacket encode(const WhoRequest& v) {
    ByteWriter w;
    name64(w, v.name);
    w.zeros(64);
    w.le<std::uint32_t>(v.race);
    w.le<std::uint32_t>(v.class_);
    w.le<std::uint32_t>(v.level_low);
    w.le<std::uint32_t>(v.level_high);
    w.le<std::uint32_t>(v.gm);
    w.le<std::uint32_t>(v.guild);
    w.le<std::uint32_t>(v.type);
    return make_packet(Op::WhoAllRequest, std::move(w));
}

std::optional<WhoRequest> decode_who_request(const AppPacket& p) {
    if (p.data.size() < kWhoRequestSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        WhoRequest v;
        v.name = r.fixed_string(64);
        r.skip(64);
        v.race = r.le<std::uint32_t>();
        v.class_ = r.le<std::uint32_t>();
        v.level_low = r.le<std::uint32_t>();
        v.level_high = r.le<std::uint32_t>();
        v.gm = r.le<std::uint32_t>();
        v.guild = r.le<std::uint32_t>();
        v.type = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const WhoResponse& v) {
    ByteWriter w;
    w.le<std::uint32_t>(v.id);
    w.le<std::uint32_t>(v.title_string);
    w.fixed_string(v.line.substr(0, 27), 27);
    w.le<std::uint8_t>(0x0a);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(v.footer_string);
    w.le<std::uint32_t>(v.footer_count);
    for (std::uint32_t x : v.footer_values) w.le<std::uint32_t>(x);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(v.entries.size()));
    for (const auto& e : v.entries) {
        w.le<std::uint32_t>(e.format);
        w.le<std::uint32_t>(e.flags);
        w.le<std::uint32_t>(e.pid_string);
        write_cstring(w, e.name);
        w.le<std::uint32_t>(e.rank_string);
        write_cstring(w, e.guild);
        w.le<std::uint32_t>(e.unknown1);
        w.le<std::uint32_t>(e.extra_string);
        w.le<std::uint32_t>(e.zone_string);
        w.le<std::uint32_t>(e.zone_id);
        if (e.flags & 0x200) write_cstring(w, e.zone_name);
        w.le<std::uint32_t>(e.class_);
        w.le<std::uint32_t>(e.level);
        w.le<std::uint32_t>(e.race);
        write_cstring(w, e.account);
        w.le<std::uint32_t>(e.unknown2);
    }
    return make_packet(Op::WhoAllResponse, std::move(w));
}

std::optional<WhoResponse> decode_who_response(const AppPacket& p) {
    if (p.data.size() < 64) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        WhoResponse v;
        v.id = r.le<std::uint32_t>();
        v.title_string = r.le<std::uint32_t>();
        v.line = r.fixed_string(27);
        r.skip(1 + 4);
        v.footer_string = r.le<std::uint32_t>();
        v.footer_count = r.le<std::uint32_t>();
        for (std::uint32_t& x : v.footer_values) x = r.le<std::uint32_t>();
        std::uint32_t count = r.le<std::uint32_t>();
        if (count > 5000) throw std::out_of_range("who count");
        for (std::uint32_t i = 0; i < count; ++i) {
            WhoEntry e;
            e.format = r.le<std::uint32_t>();
            e.flags = r.le<std::uint32_t>();
            e.pid_string = r.le<std::uint32_t>();
            e.name = r.cstring();
            e.rank_string = r.le<std::uint32_t>();
            e.guild = r.cstring();
            e.unknown1 = r.le<std::uint32_t>();
            e.extra_string = r.le<std::uint32_t>();
            e.zone_string = r.le<std::uint32_t>();
            e.zone_id = r.le<std::uint32_t>();
            if (e.flags & 0x200) e.zone_name = r.cstring();
            e.class_ = r.le<std::uint32_t>();
            e.level = r.le<std::uint32_t>();
            e.race = r.le<std::uint32_t>();
            e.account = r.cstring();
            e.unknown2 = r.le<std::uint32_t>();
            v.entries.push_back(std::move(e));
        }
        return v;
    });
}

// ---------------------------------------------------------------- /random

AppPacket encode_random_request(std::uint32_t low, std::uint32_t high) {
    ByteWriter w;
    w.le<std::uint32_t>(low);
    w.le<std::uint32_t>(high);
    return make_packet(Op::RandomReq, std::move(w));
}

AppPacket encode(const RandomReply& v) {
    ByteWriter w;
    w.le<std::uint32_t>(v.low);
    w.le<std::uint32_t>(v.high);
    w.le<std::uint32_t>(v.result);
    name64(w, v.name);
    return make_packet(Op::RandomReply, std::move(w));
}

std::optional<RandomReply> decode_random_reply(const AppPacket& p) {
    if (p.data.size() < 12) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        RandomReply v;
        v.low = r.le<std::uint32_t>();
        v.high = r.le<std::uint32_t>();
        v.result = r.le<std::uint32_t>();
        v.name = r.fixed_string(std::min<std::size_t>(64, r.remaining()));
        return v;
    });
}

// ---------------------------------------------------------------- emotes

AppPacket encode(const Emote& v) {
    ByteWriter w;
    w.le<std::uint32_t>(v.type);
    write_cstring(w, v.text);
    return make_packet(Op::Emote, std::move(w));
}

AppPacket encode_social_emote(const std::string& text) {
    ByteWriter w;
    w.le<std::uint32_t>(0xffffffff);
    w.fixed_string(text.substr(0, 1023), 1024);
    return make_packet(Op::Emote, std::move(w));
}

std::optional<Emote> decode_emote(const AppPacket& p) {
    if (p.data.size() < 5) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        Emote v;
        v.type = r.le<std::uint32_t>();
        v.text = r.fixed_string(r.remaining());
        return v;
    });
}

// ---------------------------------------------------------------- /assist, /lfg, consent

AppPacket encode_assist(std::uint32_t spawn_id) {
    ByteWriter w;
    w.le<std::uint32_t>(spawn_id);
    return make_packet(Op::Assist, std::move(w));
}

std::optional<std::uint32_t> decode_assist(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return decode_with(p, [](ByteReader& r) { return r.le<std::uint32_t>(); });
}

AppPacket encode(const LfgCommand& v) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint8_t>(v.on ? 1 : 0);
    w.le<std::uint8_t>(v.match_filter ? 1 : 0);
    w.le<std::uint16_t>(0);
    w.le<std::uint32_t>(v.from_level);
    w.le<std::uint32_t>(v.to_level);
    name64(w, v.comments);
    return make_packet(Op::LFGCommand, std::move(w));
}

AppPacket encode(const LfpCommand& v) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint8_t>(v.on ? 1 : 0);
    w.le<std::uint8_t>(v.match_filter ? 1 : 0);
    w.le<std::uint16_t>(0);
    w.le<std::uint32_t>(v.from_level);
    w.le<std::uint32_t>(v.to_level);
    w.le<std::uint32_t>(v.classes);
    name64(w, v.comments);
    return make_packet(Op::LFPCommand, std::move(w));
}

AppPacket encode_lfg_matches_request(std::uint32_t from_level, std::uint32_t to_level, std::uint32_t classes) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(from_level);
    w.le<std::uint32_t>(to_level);
    w.le<std::uint32_t>(classes);
    return make_packet(Op::LFGGetMatchesRequest, std::move(w));
}

AppPacket encode_lfp_matches_request(std::uint32_t from_level, std::uint32_t to_level) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(from_level);
    w.le<std::uint32_t>(to_level);
    return make_packet(Op::LFPGetMatchesRequest, std::move(w));
}

namespace {

std::string lfg_string(ByteReader& r) {
    std::string s = r.cstring();
    if (s.size() > 63) s.resize(63);
    return s;
}

std::int32_t lfg_short(ByteReader& r) { return static_cast<std::int16_t>(r.le<std::uint16_t>()); }

} // namespace

std::optional<std::vector<LfgMatch>> decode_lfg_matches(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    ByteReader r(p.data.data(), p.data.size());
    r.skip(4);
    std::vector<LfgMatch> out;
    try {
        while (!r.empty()) {
            LfgMatch m;
            m.comments = lfg_string(r);
            m.name = lfg_string(r);
            m.class_ = lfg_short(r);
            m.level = lfg_short(r);
            m.zone = lfg_short(r);
            m.guild = lfg_short(r);
            m.anon = lfg_short(r);
            out.push_back(std::move(m));
        }
    } catch (const std::out_of_range&) {
        // a cut record: the ones before it stand
    }
    return out;
}

AppPacket encode(const std::vector<LfgMatch>& v) {
    ByteWriter w;
    w.le<std::uint32_t>(0x74af); // EQEmu's leading word
    for (const auto& m : v) {
        write_cstring(w, m.comments);
        write_cstring(w, m.name);
        for (std::int32_t n : {m.class_, m.level, m.zone, m.guild, m.anon}) w.le<std::uint16_t>(static_cast<std::uint16_t>(n));
    }
    return make_packet(Op::LFGGetMatchesResponse, std::move(w));
}

std::optional<std::vector<LfpMatch>> decode_lfp_matches(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    ByteReader r(p.data.data(), p.data.size());
    r.skip(4);
    std::vector<LfpMatch> out;
    try {
        while (!r.empty()) {
            LfpMatch m;
            m.comments = lfg_string(r);
            m.from_level = lfg_short(r);
            m.to_level = lfg_short(r);
            m.classes = r.le<std::uint32_t>();
            m.count = lfg_short(r);
            for (std::int32_t i = 0; i < m.count; ++i) {
                LfpMember e;
                e.name = lfg_string(r);
                e.class_ = lfg_short(r);
                e.level = lfg_short(r);
                e.zone = lfg_short(r);
                e.guild = lfg_short(r);
                if (i < kLfpMatchMembers) m.members.push_back(std::move(e));
            }
            out.push_back(std::move(m));
        }
    } catch (const std::out_of_range&) {
    }
    return out;
}

AppPacket encode(const std::vector<LfpMatch>& v) {
    ByteWriter w;
    w.le<std::uint32_t>(0x74af);
    for (const auto& m : v) {
        write_cstring(w, m.comments);
        w.le<std::uint16_t>(static_cast<std::uint16_t>(m.from_level));
        w.le<std::uint16_t>(static_cast<std::uint16_t>(m.to_level));
        w.le<std::uint32_t>(m.classes);
        w.le<std::uint16_t>(static_cast<std::uint16_t>(m.members.size()));
        for (const auto& e : m.members) {
            write_cstring(w, e.name);
            for (std::int32_t n : {e.class_, e.level, e.zone, e.guild}) w.le<std::uint16_t>(static_cast<std::uint16_t>(n));
        }
    }
    return make_packet(Op::LFPGetMatchesResponse, std::move(w));
}

AppPacket encode_consent(bool deny, const std::string& name) {
    ByteWriter w;
    write_cstring(w, name.substr(0, 63));
    return make_packet(deny ? Op::ConsentDeny : Op::Consent, std::move(w));
}

std::optional<ConsentResponse> decode_consent_response(const AppPacket& p) {
    if (p.data.size() < 0x81) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ConsentResponse v;
        v.grant_name = r.fixed_string(64);
        v.owner_name = r.fixed_string(64);
        v.permission = r.le<std::uint8_t>();
        v.zone = r.fixed_string(std::min<std::size_t>(32, r.remaining()));
        return v;
    });
}

AppPacket encode(const ConsentResponse& v, bool deny_opcode) {
    ByteWriter w;
    w.fixed_string(v.grant_name, 64);
    w.fixed_string(v.owner_name, 64);
    w.le<std::uint8_t>(v.permission);
    w.fixed_string(v.zone, 32);
    return make_packet(deny_opcode ? Op::DenyResponse : Op::ConsentResponse, std::move(w));
}

// ---------------------------------------------------------------- guilds

namespace {
void guild_command_body(ByteWriter& w, const GuildCommand& v) {
    name64(w, v.other_name);
    name64(w, v.my_name);
    w.le<std::uint16_t>(static_cast<std::uint16_t>(v.guild_id));
    w.zeros(2);
    w.le<std::uint32_t>(v.rank);
}
} // namespace

AppPacket encode_guild_invite(const GuildCommand& v) {
    ByteWriter w;
    guild_command_body(w, v);
    return make_packet(Op::GuildInvite, std::move(w));
}

AppPacket encode_guild_remove(const GuildCommand& v) {
    ByteWriter w;
    guild_command_body(w, v);
    w.le<std::uint32_t>(0);
    return make_packet(Op::GuildRemove, std::move(w));
}

std::optional<GuildCommand> decode_guild_command(const AppPacket& p) {
    if (p.data.size() < kGuildInviteSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GuildCommand v;
        v.other_name = r.fixed_string(64);
        v.my_name = r.fixed_string(64);
        v.guild_id = r.le<std::uint16_t>();
        r.skip(2);
        v.rank = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const GuildInviteAccept& v) {
    ByteWriter w;
    name64(w, v.inviter);
    name64(w, v.new_member);
    w.le<std::uint32_t>(v.response);
    w.le<std::uint32_t>(v.guild_id);
    return make_packet(Op::GuildInviteAccept, std::move(w));
}

std::optional<GuildInviteAccept> decode_guild_invite_accept(const AppPacket& p) {
    if (p.data.size() < 136) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GuildInviteAccept v;
        v.inviter = r.fixed_string(64);
        v.new_member = r.fixed_string(64);
        v.response = r.le<std::uint32_t>();
        v.guild_id = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode_hailstone_guild(const HailstoneGuild& v, bool reply) {
    ByteWriter w;
    w.le(kHailstoneGuildMagic);
    w.le(kHailstoneGuildVersion);
    w.le(static_cast<std::uint16_t>(v.action));
    name64(w, v.name);
    w.le(v.guild_id);
    w.le(v.value);
    return AppPacket(reply ? kOpHailstoneGuildReply : kOpHailstoneGuild, w.take());
}

std::optional<HailstoneGuild> decode_hailstone_guild(const AppPacket& p, bool reply) {
    if (p.opcode != (reply ? kOpHailstoneGuildReply : kOpHailstoneGuild) || p.data.size() < kHailstoneGuildSize) return std::nullopt;
    auto got = decode_with(p, [](ByteReader& r) -> std::optional<HailstoneGuild> {
        if (r.le<std::uint32_t>() != kHailstoneGuildMagic || r.le<std::uint16_t>() != kHailstoneGuildVersion) return std::nullopt;
        HailstoneGuild v;
        v.action = static_cast<HailstoneGuildAction>(r.le<std::uint16_t>());
        v.name = r.fixed_string(64);
        v.guild_id = r.le<std::uint32_t>();
        v.value = r.le<std::uint32_t>();
        return v;
    });
    return got ? *got : std::nullopt;
}

namespace {
ByteWriter guild_motd_body(const GuildMotd& v) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    name64(w, v.name);
    name64(w, v.set_by);
    w.le<std::uint32_t>(0);
    return w;
}
} // namespace

AppPacket encode_set_guild_motd(const GuildMotd& v) {
    ByteWriter w = guild_motd_body(v);
    w.fixed_string(v.motd.substr(0, 511), 512);
    return make_packet(Op::SetGuildMOTD, std::move(w));
}

AppPacket encode_guild_motd(const GuildMotd& v) {
    ByteWriter w = guild_motd_body(v);
    w.fixed_string(v.motd.substr(0, 511), 512);
    return make_packet(Op::GuildMOTD, std::move(w));
}

std::optional<GuildMotd> decode_guild_motd(const AppPacket& p) {
    if (p.data.size() < 136) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GuildMotd v;
        r.skip(4);
        v.name = r.fixed_string(64);
        v.set_by = r.fixed_string(64);
        r.skip(4);
        v.motd = r.fixed_string(r.remaining());
        return v;
    });
}

AppPacket encode_get_guild_motd(std::uint32_t guild_id, const std::string& name) {
    ByteWriter w;
    w.le<std::uint32_t>(guild_id);
    name64(w, name);
    pad_to(w, 0x288);
    return make_packet(Op::GetGuildMOTD, std::move(w));
}

AppPacket encode(const GuildMemberList& v) {
    ByteWriter w;
    write_cstring(w, v.player_name);
    w.be<std::uint32_t>(v.guild_id);
    w.be<std::uint32_t>(static_cast<std::uint32_t>(v.members.size()));
    for (const auto& m : v.members) {
        write_cstring(w, m.name);
        w.be<std::uint32_t>(m.level);
        w.be<std::uint32_t>(m.banker);
        w.be<std::uint32_t>(m.class_);
        w.be<std::uint32_t>(m.rank);
        w.be<std::uint32_t>(m.last_on);
        w.be<std::uint32_t>(m.tribute_enable);
        w.be<std::uint32_t>(m.trophy_enable);
        w.be<std::uint32_t>(m.total_tribute);
        w.be<std::uint32_t>(m.last_tribute);
        w.be<std::uint32_t>(m.tribute_flag);
        write_cstring(w, m.public_note);
        w.be<std::uint16_t>(m.zone_instance);
        w.be<std::uint16_t>(m.zone_id);
        w.be<std::uint32_t>(1);
        w.be<std::uint32_t>(0);
    }
    return make_packet(Op::GuildMemberList, std::move(w));
}

std::optional<GuildMemberList> decode_guild_member_list(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        GuildMemberList v;
        v.player_name = r.cstring();
        v.guild_id = r.be<std::uint32_t>();
        std::uint32_t count = r.be<std::uint32_t>();
        if (count > 20000) throw std::out_of_range("guild member count");
        for (std::uint32_t i = 0; i < count; ++i) {
            GuildMember m;
            m.name = r.cstring();
            m.level = r.be<std::uint32_t>();
            m.banker = r.be<std::uint32_t>();
            m.class_ = r.be<std::uint32_t>();
            m.rank = r.be<std::uint32_t>();
            m.last_on = r.be<std::uint32_t>();
            m.tribute_enable = r.be<std::uint32_t>();
            m.trophy_enable = r.be<std::uint32_t>();
            m.total_tribute = r.be<std::uint32_t>();
            m.last_tribute = r.be<std::uint32_t>();
            m.tribute_flag = r.be<std::uint32_t>();
            m.public_note = r.cstring();
            m.zone_instance = r.be<std::uint16_t>();
            m.zone_id = r.be<std::uint16_t>();
            r.skip(8);
            v.members.push_back(std::move(m));
        }
        return v;
    });
}

std::optional<GuildMemberUpdate> decode_guild_member_update(const AppPacket& p) {
    if (p.data.size() < 76) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GuildMemberUpdate v;
        v.guild_id = r.le<std::uint32_t>();
        v.member = r.fixed_string(64);
        v.zone_id = r.le<std::uint16_t>();
        v.instance_id = r.le<std::uint16_t>();
        v.last_seen = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode_guild_leader(const std::string& me, const std::string& new_leader) {
    ByteWriter w;
    name64(w, me);
    name64(w, new_leader);
    return make_packet(Op::GuildLeader, std::move(w));
}

AppPacket encode_guild_public_note(std::uint32_t guild_id, const std::string& me, const std::string& member,
                                   const std::string& note) {
    ByteWriter w;
    w.le<std::uint32_t>(guild_id);
    name64(w, me);
    name64(w, member);
    w.fixed_string(note.substr(0, 255), 256);
    return make_packet(Op::GuildPublicNote, std::move(w));
}

AppPacket encode_guild_manage_banker(const std::string& member, std::uint32_t flags) {
    ByteWriter w;
    w.zeros(0x44);
    name64(w, member);
    w.le<std::uint32_t>(flags);
    return make_packet(Op::GuildManageBanker, std::move(w));
}

namespace {
ByteWriter guild_update(std::uint32_t action, std::uint32_t guild_id = 0) {
    ByteWriter w;
    w.le(action);
    w.zeros(0x48);
    w.le(guild_id);
    return w;
}
} // namespace

AppPacket encode_guild_update_text(std::uint32_t action, const std::string& text) {
    ByteWriter w = guild_update(action);
    w.fixed_string(text.substr(0, 0xfff), 0x1000);
    return make_packet(Op::GuildUpdate, std::move(w));
}

AppPacket encode_guild_rank_name(std::uint32_t rank, const std::string& name) {
    ByteWriter w = guild_update(4);
    w.le(rank);
    w.fixed_string(name.substr(0, 99), 100);
    return make_packet(Op::GuildUpdate, std::move(w));
}

AppPacket encode_guild_rank_permission(std::uint32_t rank, std::uint32_t permission, bool on, std::uint32_t guild_id) {
    ByteWriter w = guild_update(5, guild_id);
    w.le(rank);
    w.le(permission);
    w.le<std::uint8_t>(on ? 1 : 0);
    w.zeros(3);
    return make_packet(Op::GuildUpdate, std::move(w));
}

std::optional<GuildUpdate> decode_guild_update(const AppPacket& p) {
    const std::size_t n = p.data.size();
    if (n < 0x54) return std::nullopt;
    return decode_with(p, [n](ByteReader& r) -> std::optional<GuildUpdate> {
        GuildUpdate v;
        v.action = r.le<std::uint32_t>();
        r.skip(0x48);
        v.guild_id = r.le<std::uint32_t>();
        if (v.action == 5) {
            if (n < 0x59) return std::nullopt;
            v.rank = r.le<std::uint32_t>();
            v.permission = r.le<std::uint32_t>();
            v.on = r.le<std::uint8_t>() != 0;
        } else if (v.action == 4) {
            if (n < 0x55) return std::nullopt;
            v.rank = r.le<std::uint32_t>();
            v.text = r.fixed_string(r.remaining());
        } else if (v.action == 0 || v.action == 1) {
            v.text = r.fixed_string(r.remaining());
        }
        return v;
    }).value_or(std::nullopt);
}

AppPacket encode(const GuildPromote& v) {
    ByteWriter w;
    name64(w, v.target);
    name64(w, v.name);
    w.le<std::uint32_t>(v.guild_id);
    w.le<std::uint32_t>(v.new_rank);
    return make_packet(Op::GuildPromote, std::move(w));
}

std::optional<GuildPromote> decode_guild_promote(const AppPacket& p) {
    if (p.data.size() < kGuildPromoteSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GuildPromote v;
        v.target = r.fixed_string(64);
        v.name = r.fixed_string(64);
        v.guild_id = r.le<std::uint32_t>();
        v.new_rank = r.le<std::uint32_t>();
        return v;
    });
}

namespace {
AppPacket guild_war_peace(Op op, const GuildWarPeace& v) {
    ByteWriter w;
    name64(w, v.other);
    name64(w, v.name);
    w.le<std::uint32_t>(v.guild_id);
    w.le<std::int32_t>(v.days);
    w.le<std::uint32_t>(1);
    return make_packet(op, std::move(w));
}
} // namespace

AppPacket encode_guild_war(const GuildWarPeace& v) { return guild_war_peace(Op::GuildWar, v); }
AppPacket encode_guild_peace(const GuildWarPeace& v) { return guild_war_peace(Op::GuildPeace, v); }

std::optional<GuildWarPeace> decode_guild_war_peace(const AppPacket& p) {
    if (p.data.size() < kGuildWarSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GuildWarPeace v;
        v.other = r.fixed_string(64);
        v.name = r.fixed_string(64);
        v.guild_id = r.le<std::uint32_t>();
        v.days = r.le<std::int32_t>();
        return v;
    });
}

// ---------------------------------------------------------------- raids

namespace {
void raid_general(ByteWriter& w, const RaidGeneral& v) {
    w.le<std::uint32_t>(v.action);
    name64(w, v.player_name);
    w.le<std::uint32_t>(v.raid_id);
    name64(w, v.leader_name);
    w.le<std::uint32_t>(v.parameter);
}
} // namespace

AppPacket encode(const RaidGeneral& v) {
    ByteWriter w;
    raid_general(w, v);
    return make_packet(Op::RaidInvite, std::move(w));
}

AppPacket encode_set_raid_motd(const std::string& me, const std::string& motd) {
    ByteWriter w;
    raid_general(w, RaidGeneral{kRaidSetMotd, me, me, 0});
    w.fixed_string(motd.substr(0, 1023), 1024);
    return make_packet(Op::RaidInvite, std::move(w));
}

AppPacket encode_set_raid_note(const std::string& me, std::uint32_t raid_id, const std::string& member,
                               const std::string& note) {
    // Send_OP_RaidInvite_0047c430: zeroed, then {0x24, own name, raidId, the member}, the note (Utf8SafeStrncpy 64).
    ByteWriter w;
    raid_general(w, RaidGeneral{kRaidSetNote, me.substr(0, 63), member.substr(0, 63), 0, raid_id});
    w.fixed_string(note.substr(0, 63), 64);
    return make_packet(Op::RaidInvite, std::move(w));
}

AppPacket encode_raid_update(const RaidUpdate& v) {
    ByteWriter w;
    if (v.general.action == kRaidNoRaid) {
        // ZoneInSendName_Struct: u32 action, char[64] name, char[64] name, u32.
        w.le<std::uint32_t>(v.general.action);
        name64(w, v.general.player_name);
        name64(w, v.general.leader_name);
        w.le<std::uint32_t>(0);
        return make_packet(Op::RaidUpdate, std::move(w));
    }
    raid_general(w, v.general);
    if (v.general.action == kRaidAddMember) {
        w.le<std::uint8_t>(v.class_);
        w.le<std::uint8_t>(v.level);
        w.le<std::uint8_t>(v.group_leader);
        w.zeros(5);
    } else if (v.general.action == kRaidSetMotd) {
        w.fixed_string(v.motd.substr(0, 1023), 1024);
    } else if (v.general.action == kRaidSetNote) {
        w.fixed_string(v.note.substr(0, 63), 64);
    } else if (v.general.action == kRaidSetLeaderAbilities || v.general.action == kRaidSetLeader) {
        // RaidLeadershipUpdate_Struct, 392 bytes: the block from +136 (its first dword where the parameter was).
        ByteWriter l;
        RaidGeneral g = v.general;
        g.parameter = v.leadership[0];
        raid_general(l, g);
        for (std::size_t i = 1; i < v.leadership.size(); ++i) l.le(v.leadership[i]);
        return make_packet(Op::RaidUpdate, std::move(l));
    }
    return make_packet(Op::RaidUpdate, std::move(w));
}

std::optional<RaidUpdate> decode_raid_update(const AppPacket& p) {
    if (p.data.size() < 136) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        RaidUpdate v;
        v.general.action = r.le<std::uint32_t>();
        v.general.player_name = r.fixed_string(64);
        if (v.general.action == kRaidNoRaid) {
            v.general.leader_name = r.fixed_string(64);
            return v;
        }
        v.general.raid_id = r.le<std::uint32_t>();
        v.general.leader_name = r.fixed_string(64);
        v.general.parameter = r.le<std::uint32_t>();
        if (v.general.action == kRaidAddMember && r.remaining() >= 3) {
            v.class_ = r.le<std::uint8_t>();
            v.level = r.le<std::uint8_t>();
            v.group_leader = r.le<std::uint8_t>();
        } else if (v.general.action == kRaidSetMotd && r.remaining() > 0) {
            v.motd = r.fixed_string(r.remaining());
        } else if (v.general.action == kRaidSetNote && r.remaining() > 0) {
            // Handle_OP_RaidUpdate 0x24: Utf8SafeStrncpy(note, +0x8c, 0x40).
            v.note = r.fixed_string(std::min<std::size_t>(r.remaining(), 64)).substr(0, 63);
        }
        if (v.general.action == kRaidSetLeaderAbilities || v.general.action == kRaidSetLeaderData ||
            v.general.action == kRaidSetLeader) {
            // The block from +136 (the parameter just read is its first dword); what the packet lacks stays 0.
            v.leadership[0] = v.general.parameter;
            for (std::size_t i = 1; i < v.leadership.size() && r.remaining() >= 4; ++i) v.leadership[i] = r.le<std::uint32_t>();
        }
        return v;
    });
}

namespace {

constexpr std::size_t kLFGuildPlayerSize = 624, kLFGuildGuildSize = 616;

void put_at(std::vector<std::byte>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
}
std::uint32_t get_at(const std::vector<std::byte>& b, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[at + static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}
void put_text(std::vector<std::byte>& b, std::size_t at, std::string_view s, std::size_t n) {
    s = s.substr(0, n - 1);
    for (std::size_t i = 0; i < s.size(); ++i) b[at + i] = static_cast<std::byte>(s[i]);
}
std::string get_text(const std::vector<std::byte>& b, std::size_t at, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n && b[at + i] != std::byte{0}; ++i) s += static_cast<char>(b[at + i]);
    return s;
}

} // namespace

std::optional<LFGuildReply> decode_lfguild(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    const auto& b = p.data;
    std::uint32_t cmd = get_at(b, 0);
    LFGuildReply v;
    if (cmd == 0) {
        if (b.size() < kLFGuildPlayerSize) return std::nullopt;
        LFGuildPlayer e;
        e.comment = get_text(b, 72, 256);
        e.time_zone = get_at(b, 596);
        e.on = b[600] != std::byte{0};
        e.posted = get_at(b, 608);
        v.player = std::move(e);
        return v;
    }
    if (cmd == 1) {
        if (b.size() < kLFGuildGuildSize) return std::nullopt;
        LFGuildGuild e;
        e.comment = get_text(b, 12, 256);
        e.from_level = get_at(b, 524);
        e.to_level = get_at(b, 528);
        e.classes = get_at(b, 532);
        e.aa = get_at(b, 536);
        e.time_zone = get_at(b, 540);
        e.on = b[544] != std::byte{0};
        e.posted = get_at(b, 548);
        v.guild = std::move(e);
        return v;
    }
    if (cmd != 3 && cmd != 4) return v;
    return decode_with(p, [&](ByteReader& r) {
        r.skip(8);
        std::uint32_t n = r.le<std::uint32_t>();
        if (cmd == 3) {
            std::vector<LFGuildPlayerMatch> m;
            for (std::uint32_t i = 0; i < n; ++i) {
                LFGuildPlayerMatch e;
                e.name = r.cstring();
                e.level = r.le<std::uint32_t>();
                e.class_ = r.le<std::uint32_t>();
                e.aa = r.le<std::uint32_t>();
                e.time_zone = r.le<std::uint32_t>();
                e.comment = r.cstring();
                m.push_back(std::move(e));
            }
            v.players = std::move(m);
        } else {
            std::vector<LFGuildGuildMatch> m;
            for (std::uint32_t i = 0; i < n; ++i) {
                LFGuildGuildMatch e;
                e.name = r.cstring();
                e.time_zone = r.le<std::uint32_t>();
                e.comment = r.cstring();
                m.push_back(std::move(e));
            }
            v.guilds = std::move(m);
        }
        return v;
    });
}

AppPacket encode(const LFGuildPlayer& v) {
    std::vector<std::byte> b(kLFGuildPlayerSize);
    put_at(b, 0, 0);
    put_text(b, 72, v.comment, 256);
    put_at(b, 596, v.time_zone);
    b[600] = std::byte{static_cast<unsigned char>(v.on ? 1 : 0)};
    put_at(b, 608, v.posted);
    return AppPacket(to_wire(Op::LFGuild), std::move(b));
}

AppPacket encode(const LFGuildGuild& v) {
    std::vector<std::byte> b(kLFGuildGuildSize);
    put_at(b, 0, 1);
    put_text(b, 12, v.comment, 256);
    put_at(b, 524, v.from_level);
    put_at(b, 528, v.to_level);
    put_at(b, 532, v.classes);
    put_at(b, 536, v.aa);
    put_at(b, 540, v.time_zone);
    b[544] = std::byte{static_cast<unsigned char>(v.on ? 1 : 0)};
    put_at(b, 548, v.posted);
    return AppPacket(to_wire(Op::LFGuild), std::move(b));
}

AppPacket encode_lfguild_search_players(std::uint32_t from_level, std::uint32_t to_level, std::uint32_t min_aa,
                                        std::uint32_t time_zone, std::uint32_t classes) {
    ByteWriter w;
    w.le<std::uint32_t>(3);
    w.le<std::uint32_t>(0);
    for (auto x : {from_level, to_level, min_aa, time_zone, classes}) w.le(x);
    return make_packet(Op::LFGuild, std::move(w));
}

AppPacket encode_lfguild_search_guilds(std::uint32_t level, std::uint32_t aa, std::uint32_t time_zone, std::uint32_t class_) {
    ByteWriter w;
    w.le<std::uint32_t>(4);
    w.le<std::uint32_t>(0);
    for (auto x : {level, aa, time_zone, class_}) w.le(x);
    return make_packet(Op::LFGuild, std::move(w));
}

AppPacket encode(const std::vector<LFGuildPlayerMatch>& v) {
    ByteWriter w;
    w.le<std::uint32_t>(3);
    w.le<std::uint32_t>(0xeb63);
    w.le(static_cast<std::uint32_t>(v.size()));
    for (const auto& e : v) {
        detail::write_cstring(w, e.name);
        for (auto x : {e.level, e.class_, e.aa, e.time_zone}) w.le(x);
        detail::write_cstring(w, e.comment);
    }
    return make_packet(Op::LFGuild, std::move(w));
}

AppPacket encode(const std::vector<LFGuildGuildMatch>& v) {
    ByteWriter w;
    w.le<std::uint32_t>(4);
    w.le<std::uint32_t>(0xeb63);
    w.le(static_cast<std::uint32_t>(v.size()));
    for (const auto& e : v) {
        detail::write_cstring(w, e.name);
        w.le(e.time_zone);
        detail::write_cstring(w, e.comment);
    }
    return make_packet(Op::LFGuild, std::move(w));
}

} // namespace hailstone::net
