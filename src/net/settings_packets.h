#pragma once

// The server's settings for the client (server/README.md), from Hailstone's server script. What plugins and scripts
// may do is not here: that is the hailstone.cfg the server sends with its files (plugin/policy.h).
// OP_HailstoneSettingsRequest 0x1432, client -> zone, at zone-in and while GM command rules are active:
//   {u32 magic 'NMSP'; u16 version 1}.
// OP_HailstoneSettings 0x1433, zone -> client: {u32 magic; u16 version 1; u32 flags}; later fields go after these, and a
//   reader ignores what it does not know. Bit 0 formerly hid the world's player count and is ignored. Bit 2
//   indicates a trailing u16 with the receiving player's account status (0 through 32767).

#include <cstdint>
#include <optional>

#include "net/packet.h"

namespace hailstone::net {

inline constexpr std::uint32_t kHailstoneSettingsMagic = 0x50534d4e; // "NMSP" on the wire
inline constexpr std::uint16_t kOpHailstoneSettingsRequest = 0x1432;
inline constexpr std::uint16_t kOpHailstoneSettings = 0x1433;
inline constexpr std::uint16_t kHailstoneSettingsVersion = 1;
inline constexpr std::uint32_t kServerManagementOffered = 1u << 1; // This client may request a management snapshot.
inline constexpr std::uint32_t kServerAccountStatus = 1u << 2;

struct HailstoneSettings {
    std::uint32_t flags = 0;
    std::optional<std::uint16_t> account_status = std::nullopt;
};

AppPacket encode_hailstone_settings_request();
// nullopt for a wrong opcode, magic or version, or a short packet; bytes after the known fields are ignored.
std::optional<HailstoneSettings> decode_hailstone_settings(const AppPacket& p);
AppPacket encode(const HailstoneSettings& v); // server direction, for tests

} // namespace hailstone::net
