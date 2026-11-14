// GameConnection: the guild bank as CGuildBankWnd keeps it (its functions and its helpers at
// 0x67c9c0..0x67f860), over EQEmu's packets (net/guild_bank_packets.h). The window is app/lua_eq_social.cpp's
// GuildBankWnd. The server keeps the bank and sends each slot's change to the guild's members in the zone.

#include <algorithm>
#include <cctype>
#include <string_view>

#include "core/log.h"
#include "game/guild_ranks.h"
#include "game/zone_names.h"
#include "net/game_connection.h"

namespace hailstone::net {

namespace {

constexpr std::uint8_t kClassGuildBanker = 66;
constexpr float kBankerRange = 20.0f; // PlayerClient_IsWithinInteractRange(banker, 0, 20.0f)
constexpr std::int64_t kCheckMs = 300; // OnProcessFrame's interval

// _strnicmp(.., .., 0x40) == 0 on names already cut to 64.
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

} // namespace

const GuildBankItem* GameConnection::guild_bank_item(std::int32_t key) const {
    // 0x67cb30: the main area's slot 0..199 or the deposits' 0..39 the key names, else none.
    if (key == -1) return nullptr;
    auto slot = static_cast<std::size_t>(key & 0xffff);
    const auto& b = guild_bank_;
    const std::vector<std::optional<GuildBankItem>>* area = (key >> 16) == 1 ? &b.main : (key >> 16) == 0 ? &b.deposit : nullptr;
    if (!area || slot >= area->size() || !(*area)[slot]) return nullptr;
    return &*(*area)[slot];
}

bool GameConnection::guild_bank_mine(std::int32_t key) const {
    // 0x67d550: a deposit you made, or a main-area item for you alone (single member, its member your name).
    const GuildBankItem* it = guild_bank_item(key);
    if (!it || !profile_) return false;
    if (key != -1 && (key & 0xffff0000) == 0) return iequals(std::string_view(it->donator).substr(0, 64), character_);
    return it->permission == GuildBankPermission::SingleMember && iequals(std::string_view(it->who_for).substr(0, 64), character_);
}

bool GameConnection::guild_bank_can_withdraw(std::int32_t key) const {
    // 0x67d5c0: your own item; else with permission 14 a deposit, or a main-area item that is public, or public if
    // usable and usable by you.
    const GuildBankItem* it = guild_bank_item(key);
    if (!it) return false;
    if (guild_bank_mine(key)) return true;
    if (!has_guild_permission(game::kGuildPermBankWithdraw)) return false;
    if ((key & 0xffff0000) == 0) return true;
    if ((key & 0xffff0000) != 0x10000) return false;
    if (it->permission == GuildBankPermission::Public) return true;
    return it->permission == GuildBankPermission::PublicIfUsable && it->usable;
}

bool GameConnection::guild_bank_can_view(std::int32_t key) const {
    // 0x67d650: permission 15, or your own item.
    return has_guild_permission(game::kGuildPermBankView) || guild_bank_mine(key);
}

bool GameConnection::in_guild_hall_zone() const {
    // EQWorldData 0x7dc710: the zone's flag 0x80000000 (the guild hall 345 and the player guild halls 737, 738,
    // 751).
    std::uint32_t zone = zone_id();
    if (zone > 1000) zone &= 0x7fff; // the instance bit, as the predicates strip it
    return (game::zone_flags(zone) & 0x80000000u) != 0;
}

bool GameConnection::guild_bank_allowed() const {
    // 0x67e560: in a guild (or no player yet), and in a guild hall zone: one not instanced (0x7dc760: bit 15
    // of the zone word clear) always; an instanced one (0x7dc7b0) only when it is the player's guild's, the
    // guild of the root estate's guild plot (0x67f860: FindChildAtPosition(root, 0x80, 0) +0x24).
    const game::Spawn* me = world_.player();
    if (me && !guild_.in_guild()) return false;
    if (!in_guild_hall_zone()) return false;
    if ((zone_id() & 0x8000) == 0) return true;
    std::int32_t owner = -1;
    for (const ReRecord* r = real_estate_.record(real_estate_.occupancy().root); r; r = real_estate_.record(r->parent)) {
        if (real_estate_.definition_type(r->definition) == ReType::GuildPlot) {
            owner = r->guild_id;
            break;
        }
        if (r->parent == r->id) break;
    }
    return me && owner != -1 && static_cast<std::uint32_t>(owner) == guild_.guild_id;
}

bool GameConnection::open_guild_bank(std::uint32_t npc) {
    // CGuildBankWnd_OpenFromBanker @0x67e5d0: a class-66 banker in a guild hall zone; not allowed 5427, else 1465,
    // auto attack off (1466) and the window opens. Elsewhere nothing.
    auto it = world_.spawns.find(npc);
    const game::Spawn* me = world_.player();
    if (state_ != ConnectionState::InZone || it == world_.spawns.end() || !me || !it->second.is_npc ||
        it->second.class_id != kClassGuildBanker || !in_guild_hall_zone())
        return false;
    glm::vec3 at = motion_known_ ? position_ : me->position;
    if (glm::distance(at, it->second.position) >= kBankerRange) return false;
    std::string banker = game::display_name(it->second.name);
    if (!guild_bank_allowed()) {
        system(5427, {banker});
        return false;
    }
    guild_bank_.banker = npc;
    guild_bank_.serial = ++npc_session_serial_;
    guild_bank_.checked_ms = now_ms();
    if (guild_bank_.deposit.empty()) {
        guild_bank_.deposit.resize(kGuildBankDepositSlots);
        guild_bank_.main.resize(kGuildBankMainSlots);
    }
    system(1465, {banker});
    if (auto_attack_) {
        set_auto_attack(false);
        system(1466, {}, 0xd);
    }
    return true;
}

void GameConnection::close_guild_bank() {
    // AboutToHide: the active banker cleared.
    guild_bank_.banker = 0;
}

void GameConnection::guild_bank_tick() {
    // OnProcessFrame @0x67e930, every 300 ms: closes the window unless the bank may still be used and its banker is
    // still in range.
    auto& b = guild_bank_;
    if (!b.banker) return;
    std::int64_t now = now_ms();
    if (now - b.checked_ms <= kCheckMs) return;
    b.checked_ms = now;
    bool keep = state_ == ConnectionState::InZone && guild_bank_allowed();
    if (keep) {
        auto it = world_.spawns.find(b.banker);
        const game::Spawn* me = world_.player();
        glm::vec3 at = motion_known_ ? position_ : (me ? me->position : glm::vec3{});
        keep = it != world_.spawns.end() && it->second.class_id == kClassGuildBanker &&
               glm::distance(at, it->second.position) < kBankerRange;
    }
    if (!keep) close_guild_bank();
}

void GameConnection::guild_bank_select(std::int32_t key) {
    // CGuildBankWnd_SelectItem @0x67de60: the selection; the window's quantity and text entry questions close.
    guild_bank_.selected = key;
}

void GameConnection::guild_bank_deposit() {
    // WndNotification, DepositButton: {4, banker} (the button is enabled below 40 deposits with an item held, given
    // permission 13). The held item leaves at the server's acknowledgement.
    if (state_ != ConnectionState::InZone || !guild_bank_.banker) return;
    send_zone(encode_guild_bank_deposit(guild_bank_.banker));
    ++guild_bank_.pending;
}

void GameConnection::guild_bank_send(GuildBankAction action, std::uint32_t value) {
    // The selected item's requests (Send_OP_GuildBank_0067cc00 view, _0067ec10 withdraw; WndNotification's promote,
    // split and merge): {action, banker, key, item id, value}. All but the view wait for an answer.
    auto& b = guild_bank_;
    const GuildBankItem* it = guild_bank_item(b.selected);
    log::debug("guild bank: action {} on {:#x} ({}), value {}", static_cast<int>(action), b.selected, it ? it->name : "-", value);
    if (state_ != ConnectionState::InZone || !b.banker || !it) return;
    send_zone(encode_guild_bank_item(action, b.banker, b.selected, it->item_id, value));
    if (action != GuildBankAction::View) ++b.pending;
}

void GameConnection::guild_bank_set_permission(GuildBankPermission permission, const std::string& member) {
    // Send_OP_GuildBank_0067ea40: nothing unless the permission or (single member) the member changes, nor for a
    // single member without a name; {5, banker, key, item id, permission, member} (88 bytes, zeroed first).
    auto& b = guild_bank_;
    const GuildBankItem* it = guild_bank_item(b.selected);
    if (state_ != ConnectionState::InZone || !b.banker || !it) return;
    bool single = permission == GuildBankPermission::SingleMember;
    std::string name = single ? member.substr(0, 63) : std::string();
    if (it->permission == permission && it->who_for.substr(0, 64) == name) return;
    if (single && name.empty()) return;
    send_zone(encode_guild_bank_permissions(b.banker, b.selected, it->item_id, permission, name));
    ++b.pending;
}

void GameConnection::handle_guild_bank(Op op, const AppPacket& p) {
    auto& b = guild_bank_;
    if (op == Op::GuildBankItemList) {
        if (auto v = decode_guild_bank_list(p)) {
            b.deposit = std::move(v->deposit);
            b.main = std::move(v->main);
        }
        return;
    }
    // Handle_OP_GuildBank @0x67e470.
    auto v = decode_guild_bank(p);
    if (!v) return;
    switch (v->kind) {
    case GuildBankMessage::Update: {
        // 0x67c9c0 fills the slot (an empty one when not shown) and the list is redrawn; no answer counted.
        auto& area = v->area == GuildBankArea::Main ? b.main : b.deposit;
        std::size_t size = v->area == GuildBankArea::Main ? kGuildBankMainSlots : kGuildBankDepositSlots;
        if (area.size() < size) area.resize(size);
        if (v->slot < area.size()) area[v->slot] = std::move(v->item);
        return;
    }
    case GuildBankMessage::PromoteAck:
        if (v->key() >= 0) b.selected = v->key();
        break;
    case GuildBankMessage::DepositAck:
        // The key is selected and the held item goes (EQEmu puts it back on a refusal with an item packet).
        if (v->key() >= 0) {
            b.selected = v->key();
            if (inventory_.cursor()) inventory_.take(ItemSlot::possessions(kSlotCursor));
        }
        break;
    case GuildBankMessage::Ack:
        break;
    default:
        return;
    }
    if (b.pending > 0) --b.pending;
}

} // namespace hailstone::net
