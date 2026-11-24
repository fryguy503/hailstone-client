#include "net/game_connection.h"

#include <limits>

#include "core/log.h"

namespace hailstone::net {

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kManagementTimeout = std::chrono::seconds(10);

std::uint32_t next_id(std::uint32_t id) {
    return id == std::numeric_limits<std::uint32_t>::max() ? 1 : id + 1;
}
} // namespace

bool GameConnection::server_management_available() const {
    return server_management_offered_ && state_ == ConnectionState::InZone && !zone_change_pending_ &&
           !interface_locked();
}

bool GameConnection::server_management_saving() const {
    return server_management_pending_ && server_management_pending_->action == ServerManagementAction::Save;
}

bool GameConnection::open_server_management() {
    if (!server_management_available()) return false;
    if (server_management_snapshot_ || server_management_pending_) return true;
    const auto id = next_id(server_management_request_id_);
    return send_server_management(encode_server_management_open(id), id, ServerManagementAction::Open);
}

bool GameConnection::save_server_management(std::uint32_t revision, std::span<const ServerManagementEdit> edits) {
    if (!server_management_available() || server_management_pending_ || !server_management_snapshot_ ||
        server_management_snapshot_->revision != revision) return false;
    const auto id = next_id(server_management_request_id_);
    auto packet = encode_server_management_save(id, revision, edits);
    if (!packet) {
        server_management_message_ = "The draft contains an invalid or oversized setting.";
        return false;
    }
    return send_server_management(*packet, id, ServerManagementAction::Save);
}

void GameConnection::close_server_management() {
    server_management_pending_.reset();
    server_management_snapshot_.reset();
    server_management_message_.clear();
}

bool GameConnection::send_server_management(const AppPacket& packet, std::uint32_t id, ServerManagementAction action) {
    const auto now = options_.session.clock ? options_.session.clock() : Clock::now();
    server_management_request_id_ = id;
    server_management_pending_ = PendingManagement{id, action, now + kManagementTimeout};
    server_management_message_.clear();
    if (send_zone(packet, true, [this, id] {
        return server_management_available() && server_management_pending_ && server_management_pending_->id == id;
    })) return true;
    if (server_management_pending_ && server_management_pending_->id == id) {
        server_management_pending_.reset();
        server_management_message_ = "Could not send the server management request.";
    }
    return false;
}

void GameConnection::update_server_management() {
    if (!server_management_available()) {
        close_server_management();
        return;
    }
    const auto now = options_.session.clock ? options_.session.clock() : Clock::now();
    if (!server_management_pending_ || now < server_management_pending_->deadline) return;
    const bool saving = server_management_saving();
    server_management_pending_.reset();
    server_management_message_ = saving
        ? "Save was not confirmed. Reopen Server Management to check the current policy."
        : "The server did not answer the management request.";
    if (!saving) ui_message(server_management_message_);
}

void GameConnection::handle_server_management(const AppPacket& packet) {
    if (!server_management_available() || !server_management_pending_) return;
    auto reply = decode_server_management_reply(packet);
    if (!reply) {
        log::warn("server management: ignored a malformed reply ({} bytes)", packet.data.size());
        return;
    }
    if (reply->id != server_management_pending_->id || reply->action != server_management_pending_->action) return;
    server_management_pending_.reset();
    if (reply->status == ServerManagementStatus::Ok) {
        server_management_message_ = reply->action == ServerManagementAction::Save
            ? "Policy saved. Players receive it at their next login."
            : std::string{};
        server_management_snapshot_ = std::move(reply);
        server_management_serial_ = next_id(server_management_serial_);
        return;
    }
    server_management_message_ = reply->text.empty() ? "The server declined the management request." : reply->text;
    if (reply->status == ServerManagementStatus::Denied) {
        server_management_offered_ = false;
        server_management_snapshot_.reset();
    }
    if (!server_management_snapshot_) ui_message(server_management_message_);
}

} // namespace hailstone::net
