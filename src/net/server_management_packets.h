#pragma once

// GM management over the encounter's raw zone opcodes (server/README.md).
// Request and reply start with {u32 "HSAM"; u16 version; u16 action; u32 id}.
// Save continues with {u32 revision; u16 count; count x {u8 section size;
// u8 key size; u16 value size; section bytes; key bytes; value bytes}}.
// Replies continue with {u16 status; u32 revision; u16 size; cfg or error bytes}.

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "net/packet.h"

namespace hailstone::net {

inline constexpr std::uint16_t kOpServerManagementRequest = 0x143a;
inline constexpr std::uint16_t kOpServerManagementReply = 0x143b;
inline constexpr std::size_t kServerManagementMaxText = 16384;
inline constexpr std::size_t kServerManagementMaxEdits = 64;

enum class ServerManagementAction : std::uint16_t { Open = 1, Save = 2 };
enum class ServerManagementStatus : std::uint16_t { Ok, Denied, Invalid, Conflict, Unavailable, Busy };

struct ServerManagementEdit {
    std::string section;
    std::string key;
    std::string value;
};

struct ServerManagementReply {
    ServerManagementAction action = ServerManagementAction::Open;
    std::uint32_t id = 0;
    ServerManagementStatus status = ServerManagementStatus::Ok;
    std::uint32_t revision = 0;
    std::string text; // The complete INI on success, or a short error.
};

AppPacket encode_server_management_open(std::uint32_t id);
std::optional<AppPacket> encode_server_management_save(std::uint32_t id, std::uint32_t revision,
                                                       std::span<const ServerManagementEdit> edits);
std::optional<ServerManagementReply> decode_server_management_reply(const AppPacket& p);

} // namespace hailstone::net
