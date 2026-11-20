#include "net/rename_packets.h"

#include "net/packet_io.h"

namespace hailstone::net {

namespace {

constexpr std::size_t kNewNameOffset = 128, kApplyOffset = 196; // the last name's offset; the apply dword

} // namespace

std::optional<GMLastName> decode_gm_last_name(const AppPacket& p) {
    if (p.data.size() < kGMLastNameSize) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        GMLastName v;
        v.name = r.fixed_string(kRenameNameSize);
        v.gm_name = r.fixed_string(kRenameNameSize);
        v.last_name = r.fixed_string(kRenameNameSize);
        r.skip(kApplyOffset - kNewNameOffset - kRenameNameSize);
        v.apply = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const GMLastName& v) {
    ByteWriter w;
    w.fixed_string(v.name.substr(0, kRenameNameSize - 1), kRenameNameSize);
    w.fixed_string(v.gm_name.substr(0, kRenameNameSize - 1), kRenameNameSize);
    w.fixed_string(v.last_name.substr(0, kRenameNameSize - 1), kRenameNameSize);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(v.apply);
    return make_packet(Op::GMLastName, std::move(w));
}

std::optional<LevelAppearance> decode_level_appearance(const AppPacket& p) {
    if (p.data.size() < kLevelAppearanceSize) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        LevelAppearance v;
        v.spawn_id = r.le<std::uint32_t>();
        for (auto& g : v.groups) {
            g.effect_id = r.le<std::uint32_t>();
            g.slot = r.le<std::uint32_t>();
            g.flag = r.le<std::uint32_t>();
        }
        return v;
    });
}

AppPacket encode(const LevelAppearance& v) {
    ByteWriter w;
    w.le<std::uint32_t>(v.spawn_id);
    for (const auto& g : v.groups) {
        w.le<std::uint32_t>(g.effect_id);
        w.le<std::uint32_t>(g.slot);
        w.le<std::uint32_t>(g.flag);
    }
    return make_packet(Op::LevelAppearance, std::move(w));
}

bool level_appearance_group_used(const LevelAppearanceGroup& g) { return static_cast<std::int32_t>(g.effect_id) > 0; }

} // namespace hailstone::net
