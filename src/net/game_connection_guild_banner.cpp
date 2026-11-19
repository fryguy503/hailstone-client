#include "net/game_connection.h"

#include <algorithm>
#include <cmath>

#include "game/guild_ranks.h"

namespace hailstone::net {

namespace {
// The Buried Sea's ownership bit in the local character's expansion mask.
constexpr std::uint32_t kBuriedSea = 1u << 12;
// RoF2 @0x54e340 accepts the request within ten units of the queried floor.
constexpr float kBannerFloorReach = 10.0f;
constexpr std::uint32_t kBannerMissingTokenNotice = 0x1000b;

std::vector<std::uint32_t> owned_banner_kits(const game::Inventory& inventory) {
    std::vector<std::uint32_t> ids;
    const auto add = [&ids](const Item& item) {
        if (ids.size() < kMaxOwnedBannerKits && item.item_type == kGuildBannerKitItemType &&
            item.id && std::ranges::find(ids, item.id) == ids.end()) {
            ids.push_back(item.id);
        }
    };
    for (const auto& [key, item] : inventory.items()) {
        if (key.first != kInvPossessions || key.second < 0 || key.second > kSlotCursor) continue;
        add(item);
        if (item.is_bag()) {
            for (const auto& content : item.contents) add(content.item);
        }
        if (ids.size() == kMaxOwnedBannerKits) break;
    }
    return ids;
}
}

bool GameConnection::guild_banner_context() const {
    return state_ == ConnectionState::InZone && guild_.in_guild() && profile_ && profile_->character_id;
}

void GameConnection::reset_guild_banner() {
    guild_banner_ = {};
    guild_banner_requested_ = false;
    guild_banner_editing_ = false;
    guild_banner_received_ms_ = 0;
    ++guild_banner_revision_;
    discard_questions([](const Confirmation& question) { return question.id == kBannerMissingTokenNotice; });
}

bool GameConnection::handle_guild_banner(Op op, const AppPacket& p) {
    if (op != Op::GuildBannerKitList && op != Op::GuildBannerStatus && op != Op::GuildBannerModificationToken) {
        return false;
    }
    if (!guild_banner_requested_ || !guild_banner_context()) return true;
    if (op == Op::GuildBannerKitList) {
        if (auto reply = decode_guild_banner_kits(p)) {
            static_cast<GuildBannerKitList&>(guild_banner_) = std::move(*reply);
            guild_banner_.have_kits = true;
        }
    } else if (op == Op::GuildBannerModificationToken) {
        if (auto reply = decode_guild_banner_token(p)) guild_banner_.modification_token = std::move(*reply);
    } else if (auto reply = decode_guild_banner_status(p)) {
        // The status handler @0x685670 stores the reply, then calls RequestBannerKitList (@0x684ad0, the kit list
        // and status requests both) unless it is mode 2 with the editor closed (+0x364 clear): while the editor is
        // open, a mode 2 reply asks again too.
        const bool ask_again = reply->kind != 2 || guild_banner_editing_;
        guild_banner_.status = std::move(*reply);
        guild_banner_received_ms_ = session_now_ms();
        if (ask_again) guild_banner_request();
    }
    return true;
}

void GameConnection::guild_banner_open() {
    if (!guild_banner_context()) return;
    const auto revision = guild_banner_revision_;
    if (send_zone(encode_guild_banner_open(guild_.guild_id, profile_->character_id), true, [this, revision] {
            return guild_banner_revision_ == revision && guild_banner_context();
        })) {
        guild_banner_requested_ = true;
    }
}

void GameConnection::guild_banner_close() {
    reset_guild_banner();
}

void GameConnection::guild_banner_refresh(unsigned filter, bool show_all) {
    if (!guild_banner_context() || filter > kLastGuildBannerFilter) return;
    guild_banner_filter_ = filter;
    guild_banner_show_all_ = show_all;
    guild_banner_request();
}

void GameConnection::guild_banner_set_editing(bool editing) {
    guild_banner_editing_ = editing;
}

// RequestBannerKitList @0x684ad0: the kit list request (0x0d92), then always the status request (0x4924).
void GameConnection::guild_banner_request() {
    const auto revision = guild_banner_revision_;
    const auto inventory_revision = inventory_.revision();
    const auto filter = guild_banner_filter_;
    const auto show_all = guild_banner_show_all_;
    const auto owned = owned_banner_kits(inventory_);
    if (send_zone(encode_guild_banner_kit_request(profile_->character_id, show_all, filter, owned), true,
                  [this, revision, inventory_revision, filter, show_all] {
                      return guild_banner_revision_ == revision && guild_banner_context() &&
                             inventory_.revision() == inventory_revision &&
                             filter == guild_banner_filter_ && show_all == guild_banner_show_all_;
                  })) {
        guild_banner_requested_ = true;
    }
    const auto current = [this, revision] { return guild_banner_revision_ == revision && guild_banner_context(); };
    if (current() &&
        send_zone(encode_guild_banner_status_request(guild_.guild_id, profile_->character_id), true, current)) {
        guild_banner_requested_ = true;
    }
}

bool GameConnection::guild_banner_permitted(std::int32_t permission) const {
    return guild_banner_context() && guild_banner_.received() && (profile_->expansions & kBuriedSea) &&
           received_guild_permission_context() == guild_.guild_id && has_guild_permission(permission);
}

bool GameConnection::guild_banner_can_plant(std::uint32_t kit_id, bool active) const {
    if (!guild_banner_permitted(game::kGuildPermBannerPlant) || !new_zone_ ||
        new_zone_->banner_placement == BannerPlacement::CannotPlace || !world_.player()) return false;
    if (active) {
        return guild_banner_.status && guild_banner_.status->active() && guild_banner_.status->kit_id == kit_id;
    }
    if (kit_id == kNoBannerKit) return false;
    return std::ranges::any_of(guild_banner_.kits, [kit_id](const GuildBannerKit& kit) {
        return kit.id == kit_id && kit.available;
    });
}

bool GameConnection::guild_banner_can_remove() const {
    return guild_banner_permitted(game::kGuildPermBannerRemove) &&
           guild_banner_.status && guild_banner_.status->can_remove();
}

bool GameConnection::guild_banner_can_edit() const {
    return guild_banner_permitted(game::kGuildPermBannerChange) &&
           guild_banner_.background_count && guild_banner_.emblem_count;
}

void GameConnection::guild_banner_plant(std::uint32_t kit_id, bool active) {
    if (!guild_banner_can_plant(kit_id, active)) return;
    const auto at = motion_known_ ? position_ : world_.player()->position;
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) return;
    if (path_floor_) {
        const auto floor = path_floor_(at, kBannerFloorReach);
        if (!floor || !(std::abs(at.z - *floor) <= kBannerFloorReach)) {
            return system(648);
        }
    }
    const auto revision = guild_banner_revision_;
    // The native button passes its actor's coordinate vector verbatim (server Y, X, Z).
    send_zone(encode_guild_banner_plant(kit_id, !active, at.x, at.y, at.z), true,
              [this, revision, kit_id, active, at] {
                  return guild_banner_revision_ == revision && guild_banner_can_plant(kit_id, active) &&
                         (motion_known_ ? position_ : world_.player()->position) == at;
              });
}

void GameConnection::guild_banner_remove() {
    if (!guild_banner_can_remove()) return;
    const auto revision = guild_banner_revision_;
    const auto planted = guild_banner_.status->planted_kit_id;
    send_zone(encode_guild_banner_remove(guild_.guild_id), true, [this, revision, planted] {
        return guild_banner_revision_ == revision && guild_banner_can_remove() &&
               guild_banner_.status->planted_kit_id == planted;
    });
}

bool GameConnection::guild_banner_save(const GuildBannerStyle& style) {
    if (!guild_banner_can_edit() || style.background >= guild_banner_.background_count ||
        style.emblem >= guild_banner_.emblem_count) return false;
    // SaveBannerSettings @0x6833a0 refuses only an item ID of -1 (the window's +0x384); any other ID is sent.
    const auto token = guild_banner_.modification_token.item_id;
    if (token == kNoBannerKit) {
        Confirmation notice;
        notice.id = kBannerMissingTokenNotice;
        const auto& item = guild_banner_.modification_token.item_name;
        // CGuildMgmtWnd_SaveBannerSettings @0x6833a0 builds this text around the item's name (in colour there).
        // Before any modification-item message the original's name is empty too; the fallback only names it.
        notice.text = "You must have a: " + (item.empty() ? std::string("guild banner modification item") : item) +
                      " to save change to banner settings.";
        notice.ok_only = true;
        notice.plain_text = true;
        discard_questions([](const Confirmation& question) { return question.id == kBannerMissingTokenNotice; });
        ask(std::move(notice));
        return false;
    }
    const auto revision = guild_banner_revision_;
    const auto current = [this, revision, style, token] {
        return guild_banner_revision_ == revision && guild_banner_can_edit() &&
               style.background < guild_banner_.background_count && style.emblem < guild_banner_.emblem_count &&
               guild_banner_.modification_token.item_id == token;
    };
    if (!send_zone(encode_guild_banner_save(profile_->character_id, style, token), true, current)) return false;

    // Save's follow-up asks the server to reapply the design to the active banner @0x683668.
    if (!current() || !guild_banner_.status || !guild_banner_.status->active()) return true;
    if (path_floor_) {
        const auto floor = path_floor_({}, kBannerFloorReach);
        if (!floor || !(std::abs(*floor) <= kBannerFloorReach)) return true;
    }
    const auto active = guild_banner_.status->kit_id;
    send_zone(encode_guild_banner_plant(active, false, 0, 0, 0, true), true, [this, &current, active] {
        return current() && guild_banner_.status && guild_banner_.status->kit_id == active;
    });
    return true;
}

std::int32_t GameConnection::guild_banner_remaining_seconds() const {
    if (!guild_banner_.status || !guild_banner_.status->active()) return -1;
    const auto age = std::max<std::int64_t>(0, session_now_ms() - guild_banner_received_ms_);
    return static_cast<std::int32_t>(std::max<std::int64_t>(0, guild_banner_.status->remaining_ms - age) / 1000);
}

} // namespace hailstone::net
