#include "net/fellowship_packets.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/ascii_case.h"
#include "core/byte_io.h"
#include "net/packet_io.h"

namespace hailstone::net {

const FellowshipMember* FellowshipState::member(std::uint32_t character_id) const {
    for (const auto& m : members) if (m.character_id == character_id) return &m;
    return nullptr;
}

const FellowshipMember* FellowshipState::member(std::string_view value) const {
    for (const auto& m : members) if (iequals(m.name, value)) return &m;
    return nullptr;
}

bool FellowshipState::is_leader(std::string_view value) const {
    return active() && iequals(members.front().name, value);
}

AppPacket encode_fellowship_request(FellowshipAction action, std::uint32_t target,
                                    std::string_view motd, std::uint32_t camp_type) {
    // CFellowshipWnd::WndNotification @0x669440. Fields are protocol facts;
    // the server validates the action and every gameplay requirement.
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(action));
    w.le(target);
    w.zeros(12);
    w.fixed_string(motd.substr(0, 1023), 1024);
    w.zeros(20);
    w.le(camp_type);
    w.zeros(8);
    return AppPacket(kOpFellowshipRequest, w.take());
}

AppPacket encode_fellowship_kit_request(bool available_only, std::uint32_t category_mask) {
    ByteWriter w;
    w.le<std::uint32_t>(available_only ? 1u : 0u);
    w.le(category_mask);
    return AppPacket(kOpCampsiteKitRequest, w.take());
}

std::optional<FellowshipState> decode_fellowship_roster(const AppPacket& p) {
    // Version 5 native: 1100-byte header, 12 records of 84 bytes, a u32,
    // 12 player handles of 32 bytes, three 12-byte flag arrays, optional
    // 32-byte camp. Deliberate extension: all five arrays may have 13..64
    // slots. The exact packet length determines capacity, never its count.
    if (p.opcode != kOpFellowshipRoster) return std::nullopt;
    constexpr std::size_t header = 1100, per_slot = 119, trailer = 4;
    std::size_t slots = 0;
    bool has_camp = false;
    for (bool camp : {true, false}) {
        const auto fixed = header + trailer + (camp ? 32u : 0u);
        if (p.data.size() < fixed || (p.data.size() - fixed) % per_slot != 0) continue;
        const auto n = (p.data.size() - fixed) / per_slot;
        if (n < kNativeFellowshipMembers || n > kMaxFellowshipMembers) continue;
        slots = n;
        has_camp = camp;
        break;
    }
    if (!slots) return std::nullopt;
    return detail::decode_with(p, [slots, has_camp](ByteReader& r) {
        if (r.le<std::uint32_t>() != 5) throw std::out_of_range("fellowship version");
        FellowshipState v;
        v.id = r.le<std::uint32_t>();
        v.name = r.fixed_string(64);
        v.motd = r.fixed_string(1024);
        const auto count = r.le<std::uint32_t>();
        if (count > slots || (count && !v.id)) throw std::out_of_range("fellowship count");
        v.members.reserve(count);
        for (std::size_t i = 0; i < slots; ++i) {
            FellowshipMember m;
            m.character_id = r.le<std::uint32_t>();
            m.name = r.fixed_string(64);
            m.zone_id = r.le<std::uint32_t>();
            m.level = r.le<std::uint32_t>();
            m.class_ = r.le<std::uint32_t>();
            m.last_seen = r.le<std::uint32_t>();
            if (i >= count) continue;
            if (!m.character_id || m.name.empty() || v.member(m.character_id) || v.member(m.name))
                throw std::out_of_range("fellowship member identity");
            m.offline = m.zone_id == 0;
            v.members.push_back(std::move(m));
        }
        r.skip(4 + slots * 32);
        for (std::size_t i = 0; i < slots; ++i) {
            const bool flag = r.le<std::uint8_t>() != 0;
            if (i < count) v.members[i].sharing = flag;
        }
        for (std::size_t i = 0; i < slots; ++i) {
            const bool flag = r.le<std::uint8_t>() != 0;
            if (i < count) v.members[i].capped = flag;
        }
        for (std::size_t i = 0; i < slots; ++i) {
            const bool flag = r.le<std::uint8_t>() != 0;
            if (i < count) v.members[i].offline = flag || v.members[i].zone_id == 0;
        }
        if (has_camp) {
            v.camp.y = r.f32();
            v.camp.x = r.f32();
            v.camp.z = r.f32();
            v.camp.zone_id = r.le<std::uint32_t>();
            v.camp.expires = r.le<std::uint32_t>();
            r.skip(8);
            v.camp.active = r.le<std::uint32_t>() != 0 && v.camp.zone_id != 0;
            if (!std::isfinite(v.camp.x) || !std::isfinite(v.camp.y) || !std::isfinite(v.camp.z))
                throw std::out_of_range("fellowship camp coordinates");
        }
        // Keep an empty feed's id so the connection can discard an unrelated
        // fellowship's delayed disband without clearing current membership.
        if (!count) v.camp = {};
        return v;
    });
}

std::optional<FellowshipInvite> decode_fellowship_invite(const AppPacket& p) {
    if (p.opcode != kOpFellowshipInvite || (p.data.size() != 0x434 && p.data.size() != 1080)) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        if (r.le<std::uint32_t>() != 5) throw std::out_of_range("fellowship invitation action");
        FellowshipInvite v;
        v.inviter_id = r.le<std::uint32_t>();
        r.skip(4);
        const auto native_inviter = r.le<std::uint32_t>(); // CFellowshipWnd invite callback @0x667db0
        if (native_inviter) v.inviter_id = native_inviter;
        if (!v.inviter_id) throw std::out_of_range("fellowship inviter");
        return v;
    });
}

std::optional<std::vector<FellowshipKit>> decode_fellowship_kits(const AppPacket& p) {
    if (p.opcode != kOpCampsiteKitList || p.data.size() > 512 * 1024) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        const auto count = r.le<std::uint32_t>();
        if (count > 1024 || count > r.remaining() / 10) throw std::out_of_range("fellowship kit count");
        std::vector<FellowshipKit> v;
        v.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            FellowshipKit k;
            k.available = r.le<std::uint8_t>() != 0;
            k.camp_type_id = r.le<std::uint32_t>();
            k.description_id = r.le<std::uint32_t>();
            k.name = r.cstring();
            if (!k.camp_type_id || k.name.size() > 255) throw std::out_of_range("fellowship kit");
            v.push_back(std::move(k));
        }
        if (r.remaining()) throw std::out_of_range("fellowship kit tail");
        return v;
    });
}

std::optional<FellowshipVitality> decode_fellowship_vitality(const AppPacket& p) {
    if (p.opcode != kOpFellowshipVitality || p.data.size() != 28) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        FellowshipVitality v;
        v.normal = r.le<std::uint64_t>();
        v.normal_cap = r.le<std::uint64_t>();
        v.aa = r.le<std::uint32_t>();
        r.skip(4);
        v.aa_cap = r.le<std::uint32_t>();
        return v;
    });
}

} // namespace hailstone::net
