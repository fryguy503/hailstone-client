#include "net/tribute_packets.h"

#include "core/byte_io.h"
#include "net/opcodes.h"
#include "net/packet_io.h"

namespace hailstone::net {

using detail::decode_with;

namespace {
// Tribute_SendPendingRequests sends both with a 4-byte payload: the caller's stack slot, which holds the tribute
// manager's address (0xabfcd0), so nothing a server reads. Sent as zeros.
AppPacket tribute_request(Op op) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    return make_packet(op, std::move(w));
}
} // namespace

AppPacket encode_send_tributes() { return tribute_request(Op::SendTributes); }

std::optional<TributeAbility> decode_tribute_info(const AppPacket& p) {
    if (p.data.size() < 132) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        TributeAbility v;
        v.id = r.be<std::uint32_t>();
        v.tier_count = r.be<std::uint32_t>();
        for (auto& t : v.tiers) {
            t.level = r.be<std::uint32_t>();
            t.item_id = r.be<std::uint32_t>();
            t.cost = r.be<std::uint32_t>();
        }
        r.le<std::uint32_t>();
        v.name = r.cstring();
        return v;
    });
}

std::uint32_t TributeAbility::tiers_used() const {
    std::uint32_t n = 0;
    while (n < tiers.size() && (tiers[n].level || tiers[n].item_id || tiers[n].cost)) ++n;
    return n;
}

AppPacket encode(const TributeAbility& v) {
    ByteWriter w;
    w.be(v.id);
    w.be(v.tier_count);
    for (const auto& t : v.tiers) {
        w.be(t.level);
        w.be(t.item_id);
        w.be(t.cost);
    }
    w.le<std::uint32_t>(0);
    detail::write_cstring(w, v.name);
    return make_packet(Op::TributeInfo, std::move(w));
}

AppPacket encode(const TributeSettings& v) {
    ByteWriter w;
    w.le<std::uint32_t>(v.active ? 1 : 0);
    for (auto t : v.tributes) w.le(t);
    for (auto t : v.tiers) w.le(t);
    w.le(v.master_id);
    return make_packet(Op::TributeUpdate, std::move(w));
}

std::optional<TributeSettings> decode_tribute_settings(const AppPacket& p) {
    if (p.data.size() < kTributeSettingsSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        TributeSettings v;
        v.active = r.le<std::uint32_t>() != 0;
        for (auto& t : v.tributes) t = r.le<std::uint32_t>();
        for (auto& t : v.tiers) t = r.le<std::uint32_t>();
        v.master_id = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<std::uint32_t> decode_tribute_timer(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return decode_with(p, [](ByteReader& r) { return r.le<std::uint32_t>(); });
}

std::optional<TributePoints> decode_tribute_points(const AppPacket& p) {
    if (p.data.size() < 16) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        TributePoints v;
        v.points = r.le<std::int32_t>();
        r.le<std::uint32_t>();
        v.career = r.le<std::int32_t>();
        return v;
    });
}

AppPacket encode_tribute_toggle(bool on) {
    ByteWriter w;
    w.le<std::uint32_t>(on ? 1 : 0);
    return make_packet(Op::TributeToggle, std::move(w));
}

AppPacket encode(const StartTribute& v) {
    ByteWriter w;
    w.le(v.client_id);
    w.le(v.master_id);
    w.le(v.response);
    return make_packet(Op::OpenTributeMaster, std::move(w));
}

std::optional<StartTribute> decode_start_tribute(const AppPacket& p) {
    if (p.data.size() < 12) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        StartTribute v;
        v.client_id = r.le<std::uint32_t>();
        v.master_id = r.le<std::uint32_t>();
        v.response = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode_select_tribute(std::uint32_t client_id, std::uint32_t tribute_id) {
    ByteWriter w;
    w.le(client_id);
    w.le(tribute_id);
    w.le<std::uint32_t>(0);
    return make_packet(Op::SelectTribute, std::move(w));
}

std::optional<TributeDescription> decode_tribute_description(const AppPacket& p) {
    if (p.data.size() < 9) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        TributeDescription v;
        r.le<std::uint32_t>();
        v.tribute_id = r.le<std::uint32_t>();
        v.text = r.cstring();
        return v;
    });
}

AppPacket encode(const TributeItem& v) {
    ByteWriter w;
    write_slot(w, v.slot);
    w.le(v.quantity);
    w.le(v.master_id);
    w.le(v.points);
    return make_packet(Op::TributeItem, std::move(w));
}

std::optional<TributeItem> decode_tribute_item(const AppPacket& p) {
    if (p.data.size() < 24) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        TributeItem v;
        v.slot = read_slot(r);
        v.quantity = r.le<std::uint32_t>();
        v.master_id = r.le<std::uint32_t>();
        v.points = r.le<std::int32_t>();
        return v;
    });
}

AppPacket encode(const TributeMoney& v) {
    ByteWriter w;
    w.le(v.platinum);
    w.le(v.master_id);
    w.le(v.points);
    return make_packet(Op::TributeMoney, std::move(w));
}

std::optional<TributeMoney> decode_tribute_money(const AppPacket& p) {
    if (p.data.size() < 12) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        TributeMoney v;
        v.platinum = r.le<std::uint32_t>();
        v.master_id = r.le<std::uint32_t>();
        v.points = r.le<std::int32_t>();
        return v;
    });
}

AppPacket encode_request_guild_tributes() { return tribute_request(Op::RequestGuildTributes); }

std::optional<TributeAbility> decode_guild_tribute_info(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    AppPacket rest(p.opcode, std::vector<std::byte>(p.data.begin() + 4, p.data.end()));
    return decode_tribute_info(rest);
}

AppPacket encode_guild_tribute_info(const TributeAbility& v) {
    AppPacket a = encode(v);
    a.data.insert(a.data.begin(), 4, std::byte{0});
    a.opcode = to_wire(Op::SendGuildTributes);
    return a;
}

AppPacket encode_guild_select_tribute(std::uint32_t tribute_id, std::uint32_t tier) {
    ByteWriter w;
    w.le(tribute_id);
    w.le(tier);
    w.le(tribute_id);
    w.le<std::uint32_t>(0);
    return make_packet(Op::GuildSelectTribute, std::move(w));
}

std::optional<TributeDescription> decode_guild_tribute_description(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        TributeDescription d;
        d.tribute_id = r.le<std::uint32_t>();
        r.skip(8);
        d.text = r.fixed_string(r.remaining());
        return d;
    });
}

std::optional<GuildTributeActive> decode_guild_tribute_active(const AppPacket& p) {
    if (p.data.size() < 40) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GuildTributeActive v;
        v.guild_id = r.le<std::uint32_t>();
        v.favor = r.le<std::uint32_t>();
        v.timer_ms = r.le<std::uint32_t>();
        v.enabled = r.le<std::uint32_t>() != 0;
        r.skip(8);
        for (auto& t : v.tributes) t = r.le<std::uint32_t>();
        for (auto& t : v.tiers) t = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const GuildTributeActive& v, Op op) {
    ByteWriter w;
    w.le(v.guild_id);
    w.le(v.favor);
    w.le(v.timer_ms);
    w.le<std::uint32_t>(v.enabled ? 1 : 0);
    w.zeros(8);
    for (auto t : v.tributes) w.le(t);
    for (auto t : v.tiers) w.le(t);
    return make_packet(op, std::move(w));
}

std::optional<GuildTributeFavor> decode_guild_tribute_favor(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        GuildTributeFavor v;
        r.skip(4);
        v.favor = r.le<std::uint32_t>();
        v.timer_ms = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode_guild_save_tributes(std::uint32_t master_id, const GuildTributeActive& v) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.zeros(16);
    w.le(master_id);
    for (auto t : v.tributes) w.le(t);
    for (auto t : v.tiers) w.le(t);
    w.zeros(8);
    return make_packet(Op::GuildSaveActiveTributes, std::move(w));
}

AppPacket encode_guild_modify_benefits(bool save, std::uint32_t master_id, const GuildTributeActive& v) {
    ByteWriter w;
    w.le<std::uint32_t>(save ? 0 : 1);
    w.le<std::uint32_t>(1);
    w.zeros(12);
    w.le(master_id);
    for (auto t : v.tributes) w.le(t);
    for (auto t : v.tiers) w.le(t);
    w.zeros(40);
    return make_packet(Op::GuildModifyBenefits, std::move(w));
}

AppPacket encode_guild_tribute_toggle(bool on) {
    ByteWriter w;
    w.le<std::uint32_t>(on ? 1 : 0);
    w.zeros(8);
    return make_packet(Op::GuildTributeToggleReq, std::move(w));
}

namespace {
// OP_GuildOptInOut's request (Send_OP_GuildOptInOut @0x423f00): the guild, the state and the member's name; the
// original copies the name into a 72-byte stack buffer and sends the 8 bytes after the 64 (EQEmu's command and
// master) unset, here 0.
ByteWriter opt_request(std::uint32_t guild_id, bool on, std::string_view member) {
    ByteWriter w;
    w.le(guild_id);
    w.le<std::uint32_t>(on ? 1 : 0);
    w.fixed_string(member.substr(0, 63), 64);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    return w;
}
} // namespace

AppPacket encode_guild_tribute_opt(std::uint32_t guild_id, bool on, std::string_view member) {
    return make_packet(Op::GuildOptInOut, opt_request(guild_id, on, member));
}

AppPacket encode_guild_trophy_opt(std::uint32_t guild_id, bool on, std::string_view member) {
    return AppPacket(kOpGuildTrophyOptInOut, opt_request(guild_id, on, member).take());
}

std::optional<GuildTributeOpt> decode_guild_tribute_opt(const AppPacket& p) {
    if (p.data.size() < 88) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GuildTributeOpt v;
        r.skip(4);
        v.member = r.fixed_string(64);
        v.on = r.le<std::uint32_t>() != 0;
        v.trophy_on = r.le<std::uint32_t>() != 0;
        v.donations = r.le<std::uint32_t>();
        v.time = r.le<std::uint32_t>();
        v.command = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const GuildTributeOpt& v) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.fixed_string(std::string_view(v.member).substr(0, 63), 64);
    w.le<std::uint32_t>(v.on ? 1 : 0);
    w.le<std::uint32_t>(v.trophy_on ? 1 : 0);
    w.le(v.donations);
    w.le(v.time);
    w.le(v.command);
    return make_packet(Op::GuildOptInOut, std::move(w));
}

AppPacket encode_guild_donate_item(const ItemSlot& slot, std::uint32_t quantity, std::uint32_t master_id, std::uint32_t guild_id) {
    ByteWriter w;
    w.le<std::uint32_t>(static_cast<std::uint32_t>(slot.type));
    w.le(static_cast<std::uint16_t>(slot.slot));
    w.le(static_cast<std::uint16_t>(slot.sub));
    w.le<std::uint16_t>(0xffff);
    w.le<std::uint16_t>(0);
    w.le(quantity);
    w.le(master_id);
    w.le<std::uint32_t>(0);
    w.le(guild_id);
    w.zeros(8);
    return make_packet(Op::GuildTributeDonateItem, std::move(w));
}

std::optional<GuildTributeDonation> decode_guild_donate_item(const AppPacket& p) {
    if (p.data.size() < 24) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GuildTributeDonation v;
        std::uint32_t type = r.le<std::uint32_t>();
        std::int16_t slot = r.le<std::int16_t>();
        std::int16_t sub = r.le<std::int16_t>();
        r.skip(4);
        v.slot = ItemSlot{static_cast<std::int16_t>(type), slot, sub, -1, 0, 0};
        v.quantity = r.le<std::uint32_t>();
        r.skip(4);
        v.favor = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const GuildTributeDonation& v) {
    ByteWriter w;
    w.le<std::uint32_t>(static_cast<std::uint32_t>(v.slot.type));
    w.le(static_cast<std::uint16_t>(v.slot.slot));
    w.le(static_cast<std::uint16_t>(v.slot.sub));
    w.le<std::uint16_t>(0xffff);
    w.le<std::uint16_t>(0);
    w.le(v.quantity);
    w.le<std::uint32_t>(0);
    w.le(v.favor);
    return make_packet(Op::GuildTributeDonateItem, std::move(w));
}

AppPacket encode_guild_donate_platinum(std::uint32_t platinum, std::uint32_t master_id, std::uint32_t guild_id) {
    ByteWriter w;
    w.le(platinum);
    w.le(master_id);
    w.le<std::uint32_t>(0);
    w.le(guild_id);
    w.le<std::uint32_t>(0);
    return make_packet(Op::GuildTributeDonatePlat, std::move(w));
}

std::optional<GuildTributePlatinum> decode_guild_donate_platinum(const AppPacket& p) {
    if (p.data.size() < 12) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        GuildTributePlatinum v;
        v.platinum = r.le<std::uint32_t>();
        r.skip(4);
        v.favor = r.le<std::uint32_t>();
        return v;
    });
}

} // namespace hailstone::net
