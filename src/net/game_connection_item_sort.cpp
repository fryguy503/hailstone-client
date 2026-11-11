// Hailstone: sorting, stacking and tidying bags (game/item_sort.h), run as clicks through click_slot; selling the
// items marked as junk to the open merchant; and the Advanced Inventory's bulk sell, destroy and tribute of the slots
// the player chose.

#include <algorithm>

#include "core/log.h"
#include "core/distribution.h"
#include "game/merchant.h"
#include "net/game_connection.h"

namespace hailstone::net {

namespace {
constexpr std::int64_t kItemJobStepMs = 60; // between two clicks
constexpr std::int64_t kSellStepMs = 150;   // between two sales (each also waits for the merchant's answer)
constexpr std::int64_t kTributeWaitMs = 5000; // a donation the tribute master has not answered by then stops the job

bool carried_slot(const ItemSlot& at) {
    return at.type == kInvPossessions && at.slot >= kSlotGeneral1 && at.slot <= kSlotGeneral10 && at.aug < 0;
}
}

std::string GameConnection::item_job_blocked(ItemJobScope scope) const {
    if (distribution::managed()) return "Item automation is disabled in this distribution.";
    if (state_ != ConnectionState::InZone || pstatus_.dead) return "You cannot move items now.";
    if (scope == ItemJobScope::Bank && !bank_) return "The bank is not open.";
    if (inventory_.cursor()) return "Put down what you are holding first.";
    for (std::uint32_t c : cursor_coins_)
        if (c) return "Put down the coin you are holding first.";
    if (quantity_) return "Close the quantity window first.";
    if (merchant_ && merchant_->open) return "Close the merchant first.";
    if (trade_) return "Finish the trade first.";
    return {};
}

std::string GameConnection::start_item_job(ItemJobScope scope, const ItemSlot& bag, ItemJobMode mode,
                                           const ItemJobOptions& options) {
    if (distribution::managed()) return "Item automation is disabled in this distribution.";
    if (item_job_) return "Already sorting.";
    if (mode == ItemJobMode::Sell) return "Use start_sell_job.";
    if (auto why = item_job_blocked(scope); !why.empty()) return why;
    std::vector<game::SortPosition> positions;
    if (scope == ItemJobScope::Bag) {
        if (bag.type == kInvBank && !bank_) return "The bank is not open.";
        if (bag.type != kInvPossessions && bag.type != kInvBank) return "Only bags you carry or bank can be sorted.";
        positions = game::bag_positions(inventory_, bag);
    } else if (scope == ItemJobScope::Inventory) {
        std::vector<std::int16_t> slots;
        for (int s = kSlotGeneral1; s <= kSlotGeneral10; ++s)
            if (inv_slot_available(s)) slots.push_back(static_cast<std::int16_t>(s));
        positions = game::sort_positions(inventory_, kInvPossessions, slots);
    } else {
        std::vector<std::int16_t> slots;
        for (int s = 0; s < bank_->slots; ++s) slots.push_back(static_cast<std::int16_t>(s));
        positions = game::sort_positions(inventory_, kInvBank, slots);
    }
    // the filters, and the locked slots out
    for (auto& p : positions)
        for (const auto& [top, category] : options.filters)
            if (top.same(p.slot.parent())) p.filter = category;
    std::erase_if(positions, [&](const game::SortPosition& p) {
        return std::any_of(options.locked.begin(), options.locked.end(), [&](const ItemSlot& l) { return l.same(p.slot); });
    });
    std::vector<game::SortClick> clicks;
    if (mode == ItemJobMode::Stack) {
        clicks = game::plan_stack(inventory_, positions);
    } else if (mode == ItemJobMode::Tidy) {
        clicks = game::plan_tidy(inventory_, positions);
    } else if (auto plan = game::plan_sort(inventory_, positions)) {
        clicks = std::move(*plan);
    } else {
        return "Some items have nowhere they fit.";
    }
    if (clicks.empty()) return {};
    ItemJob job;
    job.clicks = std::move(clicks);
    job.status = {0, job.clicks.size(), mode, scope};
    log::info("item job (mode {}): {} clicks", static_cast<int>(mode), job.clicks.size());
    item_job_ = std::move(job);
    return {};
}

std::string GameConnection::start_sell_job(const std::vector<std::uint32_t>& ids) {
    if (distribution::managed()) return "Item automation is disabled in this distribution.";
    if (item_job_) return "Already busy with your items.";
    if (state_ != ConnectionState::InZone || pstatus_.dead) return "You cannot sell now.";
    if (!merchant_ || !merchant_->open || merchant_->trader) return "Open a merchant first.";
    // the general slots, then their bags' contents (a bag's own slot never: selling it would sell what it holds)
    ItemJob job;
    auto take = [&](const ItemSlot& at) {
        const Item* it = inventory_.at(at);
        if (it && !it->is_bag() && std::find(ids.begin(), ids.end(), it->id) != ids.end() && game::sell_refusal(*it) == 0 &&
            !kept_items_.kept(it->id)) // Hailstone: a kept item is never sold (game/kept_items.h)
            job.clicks.push_back({at, it->id});
    };
    for (int s = kSlotGeneral1; s <= kSlotGeneral10; ++s) {
        const ItemSlot top = ItemSlot::possessions(static_cast<std::int16_t>(s));
        const Item* it = inventory_.at(top);
        if (!it) continue;
        if (!it->is_bag()) {
            take(top);
            continue;
        }
        for (int i = 0; i < it->bag_slots; ++i) take(ItemSlot::possessions(static_cast<std::int16_t>(s), static_cast<std::int16_t>(i)));
    }
    if (job.clicks.empty()) return "You carry nothing marked to sell that this merchant buys.";
    job.status = {0, job.clicks.size(), ItemJobMode::Sell, ItemJobScope::Inventory};
    item_job_ = std::move(job);
    return {};
}

std::string GameConnection::bulk_items_blocked(ItemJobMode mode) const {
    if (distribution::managed()) return "Item automation is disabled in this distribution.";
    if (item_job_) return "Already busy with your items.";
    if (state_ != ConnectionState::InZone || pstatus_.dead) return "You cannot do that now.";
    if (mode == ItemJobMode::Sell) {
        if (!merchant_ || !merchant_->open || merchant_->trader) return "Open a merchant first.";
        return {};
    }
    if (mode != ItemJobMode::Destroy && mode != ItemJobMode::Tribute) return "That is not a bulk action.";
    if (mode == ItemJobMode::Tribute && !tribute_.master_id) return "Open a tribute master first.";
    return item_job_blocked(ItemJobScope::Inventory); // each item goes through the cursor
}

std::vector<ItemSlot> GameConnection::bulk_items_plan(ItemJobMode mode, const std::vector<ItemSlot>& slots) const {
    std::vector<ItemSlot> out;
    for (const ItemSlot& at : slots) {
        if (!carried_slot(at)) continue;
        if (at.sub >= 0) { // a bag's slot: the bag must be in that general slot
            const Item* bag = inventory_.at(ItemSlot::possessions(at.slot));
            if (!bag || !bag->is_bag() || at.sub >= bag->bag_slots) continue;
        }
        const Item* it = inventory_.at(at);
        if (!it || kept_items_.kept(it->id)) continue;
        if (std::any_of(out.begin(), out.end(), [&](const ItemSlot& o) { return o.same(at); })) continue;
        bool ok = false;
        switch (mode) {
        case ItemJobMode::Sell:
            ok = !it->is_bag() && game::sell_refusal(*it) == 0; // as start_sell_job
            break;
        case ItemJobMode::Destroy: {
            // a bag that holds anything would take its contents with it; No Destroy is the item's own word
            bool full = false;
            if (it->is_bag() && at.sub < 0)
                for (int i = 0; i < it->bag_slots && !full; ++i)
                    full = inventory_.at(ItemSlot::possessions(at.slot, static_cast<std::int16_t>(i))) != nullptr;
            ok = !full && !it->no_destroy();
            break;
        }
        case ItemJobMode::Tribute:
            // EQEmu's Handle_OP_TributeItem gives an item's Favor and refuses one worth none
            ok = !it->is_bag() && it->favor > 0;
            break;
        default:
            break;
        }
        if (ok) out.push_back(at);
    }
    return out;
}

std::string GameConnection::start_bulk_job(ItemJobMode mode, const std::vector<ItemSlot>& slots) {
    if (auto why = bulk_items_blocked(mode); !why.empty()) return why;
    std::vector<ItemSlot> plan = bulk_items_plan(mode, slots);
    if (plan.empty()) {
        return mode == ItemJobMode::Sell      ? "Nothing chosen is something you carry that this merchant buys."
               : mode == ItemJobMode::Tribute ? "Nothing chosen is something you carry that is worth tribute."
                                              : "Nothing chosen is something you carry that can be destroyed.";
    }
    ItemJob job;
    for (const ItemSlot& at : plan) job.clicks.push_back({at, inventory_.at(at)->id});
    job.status = {0, job.clicks.size(), mode, ItemJobScope::Inventory};
    log::info("bulk item job (mode {}): {} items", static_cast<int>(mode), job.clicks.size());
    item_job_ = std::move(job);
    return {};
}

void GameConnection::update_bulk_job(std::int64_t now) {
    if (distribution::managed()) {
        item_job_.reset();
        return;
    }
    ItemJob& job = *item_job_;
    const game::SortClick& c = job.clicks[job.next];
    const bool tribute = job.status.mode == ItemJobMode::Tribute;
    auto next_item = [&] {
        job.phase = 0;
        ++job.next;
        job.status.done = job.next;
    };
    auto stop = [&] {
        log::info("bulk item job stopped at item {} of {}", job.next, job.clicks.size());
        item_job_.reset();
        if (inventory_.cursor()) auto_inventory();
        chat(ChatEvent::Kind::System, tribute ? "Tribute stopped: your items changed while it ran."
                                              : "Destroying stopped: your items changed while it ran.");
    };
    if (state_ != ConnectionState::InZone || pstatus_.dead || quantity_ || trade_ || (merchant_ && merchant_->open) ||
        (tribute && !tribute_.master_id))
        return stop();
    job.next_ms = now + kItemJobStepMs;
    const Item* held = inventory_.cursor();
    switch (job.phase) {
    case 0: { // pick it up (the whole stack)
        if (held) return stop();
        const Item* it = inventory_.at(c.slot);
        if (!it || it->id != c.cursor_id || kept_items_.kept(it->id)) return next_item(); // gone or moved: skipped
        click_slot(c.slot, true, false);
        job.phase = 1;
        return;
    }
    case 1: // destroy it, or donate it
        if (!held || held->id != c.cursor_id) return stop();
        if (!tribute) {
            destroy_cursor();
            if (inventory_.cursor()) return stop(); // refused (kept since)
            return next_item();
        }
        job.answers = tribute_.donation_answers;
        if (!tribute_donate_cursor()) return stop();
        job.deadline_ms = now + kTributeWaitMs;
        job.phase = 2;
        return;
    case 2: // the tribute master's answer
        if (tribute_.donation_answers == job.answers) {
            if (now >= job.deadline_ms) return stop();
            return;
        }
        if (!held) return next_item(); // donated
        if (held->id != c.cursor_id) return stop();
        // refused (0 favour): back where it was
        if (inventory_.at(c.slot)) return stop();
        click_slot(c.slot, true, false);
        job.phase = 3;
        return;
    default: // after putting it back
        if (held) return stop();
        return next_item();
    }
}

std::optional<GameConnection::ItemJobStatus> GameConnection::item_job() const {
    if (!item_job_) return std::nullopt;
    return item_job_->status;
}

void GameConnection::update_item_job() {
    if (distribution::managed()) {
        item_job_.reset();
        return;
    }
    if (!item_job_) return;
    ItemJob& job = *item_job_;
    std::int64_t now = now_ms();
    if (now < job.next_ms) return;
    if (job.next >= job.clicks.size()) {
        if (job.status.mode != ItemJobMode::Sell || !merchant_ || merchant_->pending == 0) item_job_.reset();
        return;
    }
    if (job.status.mode == ItemJobMode::Destroy || job.status.mode == ItemJobMode::Tribute) return update_bulk_job(now);
    const game::SortClick& c = job.clicks[job.next];
    if (job.status.mode == ItemJobMode::Sell) {
        // one sale at a time: the merchant's answer clears pending (CMerchantWnd's "one thing at a time")
        if (state_ != ConnectionState::InZone || !merchant_ || !merchant_->open) {
            item_job_.reset();
            return;
        }
        if (merchant_->pending > 0) return;
        job.next_ms = now + kSellStepMs;
        ++job.next;
        job.status.done = job.next;
        const Item* it = inventory_.at(c.slot);
        if (!it || it->id != c.cursor_id) return; // gone or moved since: skipped
        merchant_sell(c.slot, it->stackable() ? it->stack_count : 1);
        return;
    }
    job.next_ms = now + kItemJobStepMs;
    const Item* held = inventory_.cursor();
    // Something else moved (the server put an item back, the player clicked, a window opened): stop, and put away
    // what the cursor holds.
    bool blocked = state_ != ConnectionState::InZone || pstatus_.dead || quantity_ || (merchant_ && merchant_->open) ||
                   trade_ || (job.status.scope == ItemJobScope::Bank && !bank_) ||
                   (c.slot.type == kInvBank && !bank_);
    if (blocked || (held ? held->id : 0u) != c.cursor_id) {
        log::info("item job stopped at click {} of {}", job.next, job.clicks.size());
        item_job_.reset();
        if (inventory_.cursor()) auto_inventory();
        chat(ChatEvent::Kind::System, "Sorting stopped: your items changed while it ran.");
        return;
    }
    ++job.next;
    job.status.done = job.next;
    click_slot(c.slot, true, false);
}

} // namespace hailstone::net
