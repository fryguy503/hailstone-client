#include "net/server_management_packets.h"

#include <algorithm>
#include <string_view>

#include "core/byte_io.h"
#include "net/packet_io.h"

namespace hailstone::net {

namespace {
constexpr std::uint32_t kMagic = 0x4d415348; // "HSAM"
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kMaxName = 48;
constexpr std::size_t kMaxValue = 8192;
constexpr std::size_t kReplyHeaderSize = 20;

void write_header(ByteWriter& w, ServerManagementAction action, std::uint32_t id) {
    w.le(kMagic);
    w.le(kVersion);
    w.le(static_cast<std::uint16_t>(action));
    w.le(id);
}

bool valid_name(std::string_view name) {
    return !name.empty() && name.size() <= kMaxName && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || c == '_';
    });
}

bool allowed_text(char c) {
    return static_cast<unsigned char>(c) >= 32 && c != 127;
}
} // namespace

AppPacket encode_server_management_open(std::uint32_t id) {
    ByteWriter w;
    write_header(w, ServerManagementAction::Open, id);
    return AppPacket(kOpServerManagementRequest, w.take());
}

std::optional<AppPacket> encode_server_management_save(std::uint32_t id, std::uint32_t revision,
                                                       std::span<const ServerManagementEdit> edits) {
    if (!id || !revision || edits.empty() || edits.size() > kServerManagementMaxEdits) return std::nullopt;
    std::size_t fields_size = 0;
    for (const auto& edit : edits) {
        if (!valid_name(edit.section) || !valid_name(edit.key) || edit.value.size() > kMaxValue ||
            !std::all_of(edit.value.begin(), edit.value.end(), allowed_text)) return std::nullopt;
        fields_size += 4 + edit.section.size() + edit.key.size() + edit.value.size();
        if (fields_size > kServerManagementMaxText) return std::nullopt;
    }

    ByteWriter w;
    w.reserve(18 + fields_size);
    write_header(w, ServerManagementAction::Save, id);
    w.le(revision);
    w.le(static_cast<std::uint16_t>(edits.size()));
    for (const auto& edit : edits) {
        w.le(static_cast<std::uint8_t>(edit.section.size()));
        w.le(static_cast<std::uint8_t>(edit.key.size()));
        w.le(static_cast<std::uint16_t>(edit.value.size()));
        w.fixed_string(edit.section, edit.section.size());
        w.fixed_string(edit.key, edit.key.size());
        w.fixed_string(edit.value, edit.value.size());
    }
    return AppPacket(kOpServerManagementRequest, w.take());
}

std::optional<ServerManagementReply> decode_server_management_reply(const AppPacket& p) {
    if (p.opcode != kOpServerManagementReply || p.data.size() < kReplyHeaderSize ||
        p.data.size() > kReplyHeaderSize + kServerManagementMaxText) return std::nullopt;
    auto reply = detail::decode_with(p, [](ByteReader& r) -> std::optional<ServerManagementReply> {
        if (r.le<std::uint32_t>() != kMagic || r.le<std::uint16_t>() != kVersion) return std::nullopt;
        auto action = static_cast<ServerManagementAction>(r.le<std::uint16_t>());
        if (action != ServerManagementAction::Open && action != ServerManagementAction::Save) return std::nullopt;
        const auto id = r.le<std::uint32_t>();
        const auto status = static_cast<ServerManagementStatus>(r.le<std::uint16_t>());
        const auto revision = r.le<std::uint32_t>();
        const auto size = r.le<std::uint16_t>();
        if (!id || status > ServerManagementStatus::Busy || size != r.remaining() ||
            (status == ServerManagementStatus::Ok && !revision)) return std::nullopt;
        std::string text = r.fixed_string(size);
        if (text.size() != size) return std::nullopt;
        if (status != ServerManagementStatus::Ok && !std::all_of(text.begin(), text.end(), allowed_text))
            return std::nullopt;
        return ServerManagementReply{action, id, status, revision, std::move(text)};
    });
    return reply ? std::move(*reply) : std::nullopt;
}

} // namespace hailstone::net
