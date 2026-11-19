#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net/packet.h"

namespace hailstone::net {

inline constexpr std::uint32_t kNoBannerKit = 0xffffffffu;
inline constexpr std::size_t kMaxOwnedBannerKits = 32;
inline constexpr std::size_t kMaxGuildBannerKits = 1023;
inline constexpr std::uint8_t kGuildBannerKitItemType = 58;
inline constexpr unsigned kLastGuildBannerFilter = 31; // bit index in the category mask

struct GuildBannerStyle {
    std::uint32_t background = 0, emblem = 0;
    std::uint32_t background_color = 0xffffffffu, emblem_color = 0xffffffffu;
    bool operator==(const GuildBannerStyle&) const = default;
};

struct GuildBannerKit {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t description_id = 0;
    bool available = false;
};

struct GuildBannerKitList {
    std::uint32_t background_count = 0, emblem_count = 0;
    std::vector<GuildBannerKit> kits;
};

struct GuildBannerStatus {
    std::uint32_t kind = 0;
    std::uint32_t kit_id = kNoBannerKit, planted_kit_id = kNoBannerKit;
    std::uint32_t description_id = 0;
    std::int32_t remaining_ms = 0;
    std::string name;
    GuildBannerStyle style;

    bool active() const { return kit_id != kNoBannerKit; }
    bool can_remove() const { return active() && planted_kit_id != kNoBannerKit; }
};

struct GuildBannerModificationToken {
    std::uint32_t item_id = kNoBannerKit;
    std::string item_name;
};

struct GuildBannerState : GuildBannerKitList {
    std::optional<GuildBannerStatus> status;
    GuildBannerModificationToken modification_token;
    bool have_kits = false;

    bool received() const { return have_kits || status.has_value(); }
};

AppPacket encode_guild_banner_open(std::uint32_t guild_id, std::uint32_t character_id);
AppPacket encode_guild_banner_kit_request(std::uint32_t character_id, bool show_all, unsigned filter,
                                         std::span<const std::uint32_t> owned);
AppPacket encode_guild_banner_status_request(std::uint32_t guild_id, std::uint32_t character_id);
AppPacket encode_guild_banner_save(std::uint32_t character_id, const GuildBannerStyle& style, std::uint32_t token_id);
// Position is actor/world XYZ, in server Y, X, Z order.
AppPacket encode_guild_banner_plant(std::uint32_t kit_id, bool from_list, float x, float y, float z,
                                   bool reapply_style = false);
AppPacket encode_guild_banner_remove(std::uint32_t guild_id);

std::optional<GuildBannerKitList> decode_guild_banner_kits(const AppPacket& p);
std::optional<GuildBannerStatus> decode_guild_banner_status(const AppPacket& p);
std::optional<GuildBannerModificationToken> decode_guild_banner_token(const AppPacket& p);

} // namespace hailstone::net
