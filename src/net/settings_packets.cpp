#include "net/settings_packets.h"

#include "core/byte_io.h"
#include "net/packet_io.h"

namespace hailstone::net {

using detail::decode_with;

AppPacket encode_hailstone_settings_request() {
    ByteWriter w;
    w.le(kHailstoneSettingsMagic);
    w.le(kHailstoneSettingsVersion);
    return AppPacket(kOpHailstoneSettingsRequest, w.take());
}

std::optional<HailstoneSettings> decode_hailstone_settings(const AppPacket& p) {
    if (p.opcode != kOpHailstoneSettings) return std::nullopt;
    auto got = decode_with(p, [](ByteReader& r) -> std::optional<HailstoneSettings> {
        if (r.le<std::uint32_t>() != kHailstoneSettingsMagic || r.le<std::uint16_t>() != kHailstoneSettingsVersion) return std::nullopt;
        HailstoneSettings v;
        v.flags = r.le<std::uint32_t>();
        if (v.flags & kServerAccountStatus) {
            const auto status = r.le<std::uint16_t>();
            if (status > 32767) return std::nullopt;
            v.account_status = status;
        }
        return v;
    });
    return got ? *got : std::nullopt;
}

AppPacket encode(const HailstoneSettings& v) {
    ByteWriter w;
    w.le(kHailstoneSettingsMagic);
    w.le(kHailstoneSettingsVersion);
    w.le((v.flags & ~kServerAccountStatus) | (v.account_status ? kServerAccountStatus : 0u));
    if (v.account_status) w.le(*v.account_status);
    return AppPacket(kOpHailstoneSettings, w.take());
}

} // namespace hailstone::net
