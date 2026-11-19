#include "net/guild_banner_packets.h"

#include <algorithm>

#include "core/byte_io.h"
#include "net/opcodes.h"

namespace hailstone::net {

AppPacket encode_guild_banner_open(std::uint32_t guild_id, std::uint32_t character_id) {
    ByteWriter w;
    w.le(guild_id);
    w.le(character_id);
    return {to_wire(Op::GuildOpenGuildWindow), w.take()};
}

AppPacket encode_guild_banner_kit_request(std::uint32_t character_id, bool show_all, unsigned filter,
                                         std::span<const std::uint32_t> owned) {
    ByteWriter w;
    w.le(character_id);
    w.le<std::uint8_t>(!show_all);
    const auto count = std::min(owned.size(), kMaxOwnedBannerKits);
    for (const auto id : owned.first(count)) w.le(id);
    w.zeros((kMaxOwnedBannerKits - count) * sizeof(std::uint32_t));
    w.le(static_cast<std::uint32_t>(count));
    w.le(filter && filter <= kLastGuildBannerFilter ? 1u << filter : 0xffffffffu);
    return {to_wire(Op::GuildBannerKitRequest), w.take()};
}

AppPacket encode_guild_banner_status_request(std::uint32_t guild_id, std::uint32_t character_id) {
    ByteWriter w;
    w.le(character_id);
    w.le(guild_id);
    w.zeros(72);
    return {to_wire(Op::GuildBannerStatusRequest), w.take()};
}

AppPacket encode_guild_banner_save(std::uint32_t character_id, const GuildBannerStyle& style, std::uint32_t token_id) {
    ByteWriter w;
    w.le(character_id);
    w.le(style.background);
    w.le(style.emblem);
    w.le(style.background_color & 0x00ffffffu);
    w.le(style.emblem_color & 0x00ffffffu);
    w.le(token_id);
    return {to_wire(Op::GuildBannerSave), w.take()};
}

AppPacket encode_guild_banner_plant(std::uint32_t kit_id, bool from_list, float x, float y, float z,
                                   bool reapply_style) {
    ByteWriter w;
    w.le(kit_id);
    w.zeros(64);
    w.f32(x);
    w.f32(y);
    w.f32(z);
    w.le<std::uint8_t>(from_list);
    w.le<std::uint8_t>(reapply_style);
    w.zeros(5);
    return {to_wire(Op::GuildBannerPlant), w.take()};
}

AppPacket encode_guild_banner_remove(std::uint32_t guild_id) {
    ByteWriter w;
    w.le(kNoBannerKit);
    w.le(guild_id);
    w.zeros(4);
    return {to_wire(Op::GuildBannerRemove), w.take()};
}

std::optional<GuildBannerKitList> decode_guild_banner_kits(const AppPacket& p) {
    // Guild window receive @0x6836c0. Every record has a fixed 128-byte name. The original reads fixed offsets
    // whatever the message's length and adds at most 1023 rows (0x3ff), stepping over the rest.
    constexpr std::size_t header_size = 16, record_size = 137;
    if (p.data.size() < header_size) return std::nullopt;
    ByteReader r(p.data);
    r.skip(4);
    const auto count = std::min<std::size_t>(r.le<std::uint32_t>(), kMaxGuildBannerKits);
    if (r.remaining() < 8 + count * record_size) return std::nullopt;
    GuildBannerKitList list;
    list.background_count = r.le<std::uint32_t>();
    list.emblem_count = r.le<std::uint32_t>();
    list.kits.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        GuildBannerKit kit;
        kit.id = r.le<std::uint32_t>();
        kit.name = r.fixed_string(128);
        kit.available = r.le<std::uint8_t>() != 0;
        kit.description_id = r.le<std::uint32_t>();
        list.kits.push_back(std::move(kit));
    }
    return list;
}

std::optional<GuildBannerStatus> decode_guild_banner_status(const AppPacket& p) {
    // Guild window receive @0x685670, preview @0x680c00 and countdown @0x686b2f.
    // Status mode 1 leaves the previous snapshot in place. The original copies 188 bytes (0x2f dwords) and
    // ignores anything after them.
    constexpr std::size_t status_size = 188;
    if (p.data.size() < status_size) return std::nullopt;
    ByteReader r(p.data);
    r.skip(4);
    GuildBannerStatus status;
    status.kind = r.le<std::uint32_t>();
    if (status.kind == 1) return std::nullopt;
    r.skip(4);
    status.kit_id = r.le<std::uint32_t>();
    status.description_id = r.le<std::uint32_t>();
    status.remaining_ms = r.le<std::int32_t>();
    status.name = r.fixed_string(128);
    r.seek(168);
    status.planted_kit_id = r.le<std::uint32_t>();
    status.style.background = r.le<std::uint32_t>();
    status.style.emblem = r.le<std::uint32_t>();
    status.style.background_color = r.le<std::uint32_t>() | 0xff000000u;
    status.style.emblem_color = r.le<std::uint32_t>() | 0xff000000u;
    return status;
}

std::optional<GuildBannerModificationToken> decode_guild_banner_token(const AppPacket& p) {
    // Native receive @0x6830c0 copies up to 63 characters from +16.
    constexpr std::size_t name_offset = 16, name_size = 64;
    if (p.data.size() <= name_offset) return std::nullopt;
    ByteReader r(p.data);
    r.skip(8);
    GuildBannerModificationToken token;
    token.item_id = r.le<std::uint32_t>();
    r.seek(name_offset);
    token.item_name = r.fixed_string(std::min(name_size - 1, r.remaining()));
    return token;
}

} // namespace hailstone::net
