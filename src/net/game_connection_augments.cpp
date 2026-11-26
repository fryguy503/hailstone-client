// GameConnection: the item display's augment requests and OP_AugmentInfo (the rules
// in game/augments.h, the packets in net/augment_packets.h).

#include <algorithm>

#include "core/log.h"
#include "game/augments.h"
#include "net/augment_packets.h"
#include "net/game_connection.h"
#include "net/opcodes.h"

namespace hailstone::net {

namespace {

// The index of a child: a bag's contents by their bag slot, a common item's children (augments) by their socket.
ItemSlot child_slot(const ItemSlot& parent, const Item& owner, std::uint32_t index) {
    ItemSlot s = parent;
    auto i = static_cast<std::int16_t>(index);
    if (s.sub == -1 && owner.is_bag()) s.sub = i;
    else s.aug = i;
    return s;
}

bool find_in(const Item& item, const ItemSlot& at, const std::function<bool(const Item&)>& match,
             game::FoundItem& out) {
    if (match(item)) {
        out = {&item, at};
        return true;
    }
    // ItemContainer_FindItemByID's depth-first walk: each item's children in index order.
    std::vector<const BagItem*> children;
    for (const auto& c : item.contents) children.push_back(&c);
    std::sort(children.begin(), children.end(), [](auto* a, auto* b) { return a->index < b->index; });
    for (const BagItem* c : children)
        if (find_in(c->item, child_slot(at, item, c->index), match, out)) return true;
    return false;
}

} // namespace

std::optional<game::FoundItem> GameConnection::find_possession(const std::function<bool(const Item&)>& match) const {
    for (const auto& [key, item] : inventory_.items()) {
        if (key.first != kInvPossessions) continue;
        game::FoundItem f;
        if (find_in(item, ItemSlot::possessions(key.second), match, f)) return f;
    }
    return std::nullopt;
}

game::AugmentContext GameConnection::augment_context() const {
    game::AugmentContext c;
    c.has_pc = state_ == ConnectionState::InZone && profile_.has_value();
    c.cursor = inventory_.cursor();
    c.worn_classes = server_ext().augment_classes(*this) ? player_class_mask() : 0;
    c.item_at = [this](const ItemSlot& s) -> const Item* {
        // CharacterBase::GetItemByGlobalIndex: an augment's index names its socket in the item above.
        if (s.aug >= 0) {
            ItemSlot parent = s;
            parent.aug = -1;
            const Item* owner = inventory_.at(parent);
            return owner ? owner->content(static_cast<std::uint32_t>(s.aug)) : nullptr;
        }
        return inventory_.at(s);
    };
    c.item_by_serial = [this](std::uint32_t serial, std::uint32_t high) -> const Item* {
        // PcZoneClient_GetItemBySerial: any of the player's items (possessions, bank, shared bank) and their
        // children.
        for (const auto& [key, item] : inventory_.items()) {
            game::FoundItem f;
            if (find_in(item, {key.first, key.second, -1, -1, 0, 0},
                        [&](const Item& i) { return i.instance_id == serial && i.parcel_item_id == high; }, f))
                return f.item;
        }
        return nullptr;
    };
    c.item_by_id = [this](std::uint32_t id) { return find_possession([id](const Item& i) { return i.id == id; }); };
    c.item_by_type = [this](std::uint8_t t) {
        return find_possession([t](const Item& i) { return i.item_type == t; });
    };
    // CharacterZoneClient_CanUseItemPrestige: the free-to-play restriction does not apply on this server.
    c.can_use_prestige = [](const Item&) { return true; };
    c.can_use = [this](const Item& i) { return can_use_item_class(i); };
    // Game feature 0x15 (the appearance slot feature): EQEmu accounts have every membership feature.
    c.appearance_feature = true;
    return c;
}

void GameConnection::run_augment_request(const game::AugmentRequest& r) {
    switch (r.kind) {
    case game::AugmentRequest::Nothing:
        break;
    case game::AugmentRequest::Refuse:
        log::info("augment: refused, eqstr {}", r.eqstr);
        system(r.eqstr, r.args, game::kAugmentChatColor);
        break;
    case game::AugmentRequest::Send:
        log::info("augment: send action {} socket {} host {}/{}/{}", static_cast<std::uint32_t>(r.packet.action), r.packet.socket,
                   r.packet.host.type, r.packet.host.slot, r.packet.host.sub);
        send_zone(encode(r.packet));
        break;
    case game::AugmentRequest::Confirm: {
        // The block goes to a heap copy the dialog keeps; IsItemPending + 1 and PendingRequestLog_PushPendingReason
        // (0x13) until ConfirmationDialog_HandleResponse answers (0x4232f0 takes both back).
        log::info("augment: confirmation {:#x}, eqstr {}", r.dialog, r.eqstr);
        ++spells_.pending;
        Confirmation c;
        c.id = r.dialog;
        c.text = strings_.format(r.eqstr, r.args);
        c.offer = encode(r.packet);
        ask(std::move(c));
        break;
    }
    }
}

void GameConnection::augment_click(const Item& window_item, int socket) {
    run_augment_request(game::augment_socket_click(window_item, socket, augment_context()));
}

void GameConnection::augment_menu_select(const Item& window_item, int socket, std::uint32_t id) {
    game::AugmentContext c = augment_context();
    if (id == game::kAugMenuRemove) run_augment_request(game::remove_augment_request(window_item, socket, c));
    else if (id == game::kAugMenuSwap) run_augment_request(game::swap_augment_request(window_item, socket, c));
    else if (id == game::kAugMenuDestroy) run_augment_request(game::destroy_augment_request(window_item, socket, c));
}

void GameConnection::request_augment_info(std::uint32_t solvent_id, std::uint32_t window) {
    augment_info_.erase(window); // SetItem clears SolventText when the item changes
    if (state_ != ConnectionState::InZone || solvent_id == 0) return;
    // The window waits for its answer with an empty text: the original's reply lands in an open window by its index,
    // and here only a window that asked takes one, so the server's window ids cannot add entries.
    augment_info_[window];
    send_zone(encode_augment_info_request(solvent_id, window));
}

std::string GameConnection::augment_solvent_text(std::uint32_t window) const {
    auto it = augment_info_.find(window);
    return it == augment_info_.end() ? std::string() : it->second;
}

void GameConnection::handle_augment_info(const AppPacket& p) {
    // CEverQuest::HandleWorldMessage case 0x0afb @004cb57c: 13325 when +0x48 is 55, else 5881 with the name;
    // 0x6a09e0 puts "<BR>" + text into that window's SolventText (0x69c020) and calls UpdateStrings.
    auto v = decode_augment_info(p);
    if (!v) return;
    std::string text = v->unknown072 == game::kItemTypeSolvent ? strings_.format(13325, {})
                                                                 : strings_.format(5881, std::vector<std::string>{v->name});
    auto window = augment_info_.find(v->window);
    if (window == augment_info_.end()) return; // no item display asked with that index
    window->second = "<BR>" + text;
    log::info("augment info for window {}: {}", v->window, text);
}

std::optional<Item> GameConnection::take_item_refresh() {
    if (item_refreshes_.empty()) return std::nullopt;
    Item it = std::move(item_refreshes_.front());
    item_refreshes_.pop_front();
    return it;
}

} // namespace hailstone::net
