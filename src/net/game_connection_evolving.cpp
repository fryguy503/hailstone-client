// Evolving transfer previews and requests. The server owns compatibility and resulting item forms.

#include "net/game_connection.h"

#include <algorithm>
#include <set>

namespace hailstone::net {
namespace {

void collect_evolving(const Item& item, const ItemSlot& slot, std::vector<Item>& out, std::set<std::uint64_t>& seen) {
    const auto id = evolving_item_id(item);
    if (id && seen.insert(id).second) {
        out.push_back(item);
        out.back().slot = slot;
    }
    // Augments cannot be selected: the server searches worn, personal and cursor items, including bag contents.
    if (!item.is_bag() || slot.sub >= 0) return;
    for (const auto& child : item.contents) {
        if (child.index >= kMaxBagContents) continue;
        auto at = slot;
        at.sub = static_cast<std::int16_t>(child.index);
        collect_evolving(child.item, at, out, seen);
    }
}

const Item* by_id(const std::vector<Item>& items, std::uint64_t id) {
    if (!id) return nullptr;
    const auto it = std::find_if(items.begin(), items.end(), [id](const Item& item) { return evolving_item_id(item) == id; });
    return it == items.end() ? nullptr : &*it;
}

std::vector<std::byte> snapshot(const Item* item) {
    ByteWriter w;
    if (item) write_item(w, *item);
    return w.take();
}

} // namespace

std::vector<Item> GameConnection::evolving_transfer_items() const {
    std::vector<Item> items;
    std::set<std::uint64_t> seen;
    if (state_ != ConnectionState::InZone) return items;
    for (const auto& [key, item] : inventory_.items())
        if (key.first == kInvPossessions && key.second >= 0 && key.second <= kSlotCursor)
            collect_evolving(item, ItemSlot::possessions(key.second), items, seen);
    return items;
}

void GameConnection::close_evolving_transfer() {
    const auto revision = evolving_transfer_.revision + 1;
    evolving_transfer_ = {};
    evolving_transfer_.revision = revision;
    evolving_source_snapshot_.clear();
    evolving_destination_snapshot_.clear();
}

void GameConnection::open_evolving_transfer() {
    if (state_ != ConnectionState::InZone || evolving_transfer_.shown) return;
    close_evolving_transfer();
    evolving_transfer_.shown = true;
    evolving_transfer_.status = "Select source and destination evolving items.";
}

bool GameConnection::select_evolving_transfer(bool source, std::uint64_t id) {
    if (state_ != ConnectionState::InZone || !evolving_transfer_.shown || evolving_transfer_.submitted) return false;
    const auto items = evolving_transfer_items();
    if (id && !by_id(items, id)) return false;
    if (id && id == (source ? evolving_transfer_.destination_id : evolving_transfer_.source_id)) return false;
    (source ? evolving_transfer_.source_id : evolving_transfer_.destination_id) = id;
    request_evolving_details();
    return true;
}

void GameConnection::request_evolving_details() {
    auto& view = evolving_transfer_;
    ++view.revision;
    view.details.reset();
    view.pending = false;
    const auto items = evolving_transfer_items();
    const auto* source = by_id(items, view.source_id);
    const auto* destination = by_id(items, view.destination_id);
    evolving_source_snapshot_ = snapshot(source);
    evolving_destination_snapshot_ = snapshot(destination);
    if (!source || !destination) {
        view.status = "Select source and destination evolving items.";
        return;
    }
    const auto revision = view.revision;
    view.pending = true;
    view.status = "Waiting for the server's transfer preview.";
    if (!send_zone(encode(EvolvingRequest{EvolvingAction::Details, view.source_id, view.destination_id}), true,
                   [this, revision] { return evolving_transfer_.shown && evolving_transfer_.revision == revision; })) {
        if (view.revision == revision) {
            view.pending = false;
            view.status = "The transfer preview request was not sent.";
        }
    }
}

bool GameConnection::can_transfer_evolving() const {
    const auto& view = evolving_transfer_;
    if (state_ != ConnectionState::InZone || !view.shown || view.pending || view.submitted || !view.details ||
        !view.details->compatibility || !view.details->source || !view.details->destination ||
        !view.source_id || !view.destination_id || view.source_id == view.destination_id) return false;
    const auto items = evolving_transfer_items();
    const auto* source = by_id(items, view.source_id);
    const auto* destination = by_id(items, view.destination_id);
    return source && destination && snapshot(source) == evolving_source_snapshot_ &&
           snapshot(destination) == evolving_destination_snapshot_;
}

bool GameConnection::request_evolving_transfer(std::uint64_t expected_revision) {
    if (evolving_transfer_.revision != expected_revision || !can_transfer_evolving()) return false;
    auto& view = evolving_transfer_;
    const auto source = view.source_id, destination = view.destination_id;
    view.submitted = true;
    view.details.reset();
    ++view.revision;
    const auto revision = view.revision;
    view.status = "Transfer sent. Check your inventory and server messages.";
    if (!send_zone(encode(EvolvingRequest{EvolvingAction::Transfer, source, destination}), true,
                   [this, source, destination, revision] {
                       return state_ == ConnectionState::InZone && evolving_transfer_.shown &&
                              evolving_transfer_.submitted && evolving_transfer_.source_id == source &&
                              evolving_transfer_.destination_id == destination && evolving_transfer_.revision == revision && [&] {
                                  const auto items = evolving_transfer_items();
                                  return snapshot(by_id(items, source)) == evolving_source_snapshot_ &&
                                         snapshot(by_id(items, destination)) == evolving_destination_snapshot_;
                              }();
                   })) {
        if (view.revision == revision) {
            view.submitted = false;
            view.status = "The transfer request was not sent. Select the items again to retry.";
        }
        return false;
    }
    return true;
}

void GameConnection::preview_evolving_item(std::uint32_t item_id, std::uint32_t final_hint, std::uint32_t level,
                                          std::uint32_t hash, bool full) {
    if (state_ == ConnectionState::InZone && item_id)
        send_zone(encode(EvolvingFinalResult{item_id, final_hint, level, hash, full}));
}

void GameConnection::update_evolving_transfer() {
    if (state_ != ConnectionState::InZone) {
        if (evolving_transfer_.shown) close_evolving_transfer();
        return;
    }
    auto& view = evolving_transfer_;
    if (!view.shown) return;
    const auto items = evolving_transfer_items();
    const auto* source = by_id(items, view.source_id);
    const auto* destination = by_id(items, view.destination_id);
    if (view.submitted && source && destination) return;
    if (snapshot(source) == evolving_source_snapshot_ && snapshot(destination) == evolving_destination_snapshot_) return;
    if (!source) view.source_id = 0;
    if (!destination) view.destination_id = 0;
    if (view.submitted) {
        view.source_id = view.destination_id = 0;
        view.submitted = false;
    }
    request_evolving_details();
}

void GameConnection::handle_evolving_item(const AppPacket& p) {
    if (const auto update = decode_evolving_update(p)) {
        if (update->action == EvolvingAction::Open) {
            close_evolving_transfer(); // the native server-open notification clears the selected items
            open_evolving_transfer();
        }
        else if (update->action == EvolvingAction::Update && update->percentage <= 100) {
            // The update carries integral percent, unlike the serialized item's double progress.
            const auto items = evolving_transfer_items();
            if (const auto* found = by_id(items, update->id)) {
                if (auto* item = inventory_.at(found->slot)) {
                    item->evolving.progress = static_cast<double>(update->percentage);
                    item->evolving.activated = update->activated ? 1 : 0;
                    item_refreshes_.push_back(*item);
                    if (item_refreshes_.size() > 16) item_refreshes_.pop_front();
                }
            }
            update_evolving_transfer();
        }
        return;
    }
    const auto details = decode_evolving_details(p);
    if (!details) return;
    auto& view = evolving_transfer_;
    // CItemExpTransferWnd::OnReceiveDetails matches both original IDs. RoF2 has no request nonce, so an older
    // answer for the same pair cannot be distinguished. Revalidate live originals here and again on commit;
    // the server recalculates the transfer rather than accepting the projected items as a request.
    if (!view.shown || !view.pending || state_ != ConnectionState::InZone ||
        view.source_id != details->source_id || view.destination_id != details->destination_id) return;
    const auto items = evolving_transfer_items();
    if (snapshot(by_id(items, view.source_id)) != evolving_source_snapshot_ ||
        snapshot(by_id(items, view.destination_id)) != evolving_destination_snapshot_) {
        update_evolving_transfer();
        return;
    }
    view.details = *details;
    view.pending = false;
    ++view.revision;
    view.status = details->compatibility && details->source && details->destination ? "Transfer preview ready." :
                                                                                 "These items are not compatible for transfer.";
}

} // namespace hailstone::net
