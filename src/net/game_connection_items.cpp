// GameConnection: items, inventory moves, looting and item links. The client
// rules follow the original's inventory functions (named
// below); the packets from net/item_packets.h.

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>

#include "core/atomic_file.h"
#include "core/log.h"
#include "game/appearance.h"
#include "game/augments.h"
#include "game/bag_move.h"
#include "game/distance.h"
#include "game/inventory.h"
#include "game/item_options.h"
#include "game/merchant.h"
#include "game/quantity_dialog.h"
#include "game/trade_rules.h"
#include "net/game_connection.h"
#include "net/opcodes.h"
#include "net/trade_packets.h"

namespace hailstone::net {

namespace {

ItemSlot cursor_slot() { return ItemSlot::possessions(kSlotCursor); }

bool is_worn(const ItemSlot& s) { return s.type == kInvPossessions && s.sub < 0 && s.slot >= 0 && s.slot < kEquipmentSlots; }

} // namespace

// ---------------------------------------------------------------- the cursor queue

// CEverQuest_ProcessCursorQueue @0x5321a0 (from the
// disassembly; OP_MoveItem carries two unset
// padding bytes of ItemGlobalIndex_Make, sent as 0 here). With no request
// pending (IsItemPending < 1):
//  - coin from opcode 0x3ed4 goes to the purse (the copper total credited),
//    the same four values go back as 0x3ed4, sound 0x8a plays and eqstr 12071
//    (0x2f27) "You receive %1 as your split." is printed;
//  - with the cursor empty and no coin or hot button held, the first item of
//    the limbo container (slots 0..35) moves onto the cursor with OP_MoveItem
//    {from its slot, to Possessions 33, count 0}; one item a frame.
void GameConnection::process_cursor_queue() {
    if (spells_.pending > 0 || !profile_) return;
    Coins& split = pending_split_;
    if (static_cast<std::int32_t>(split.copper) > 0 || static_cast<std::int32_t>(split.silver) > 0 ||
        static_cast<std::int32_t>(split.gold) > 0 || static_cast<std::int32_t>(split.platinum) > 0) {
        std::int64_t total = static_cast<std::int64_t>(static_cast<std::int32_t>(split.platinum)) * 1000 +
                             static_cast<std::int64_t>(static_cast<std::int32_t>(split.gold)) * 100 +
                             static_cast<std::int64_t>(static_cast<std::int32_t>(split.silver)) * 10 +
                             static_cast<std::int64_t>(static_cast<std::int32_t>(split.copper));
        game::credit_copper(coins_, total);
        send_zone(encode_split_coin(split));
        SoundCue cue;
        cue.kind = SoundCue::Kind::Wave;
        cue.spell = 0x8a;
        sound_cues_.push_back(cue);
        std::vector<std::string> args{game::coin_amounts_text(split.platinum, split.gold, split.silver, split.copper)};
        chat(ChatEvent::Kind::System, strings_.format(0x2f27, args), 0, 285); // the Money Splits filter
        split = {};
    }
    if (inventory_.at(cursor_slot())) return;
    for (std::uint32_t c : cursor_coins_)
        if (c != 0) return; // CDisplay DragMoney
    for (std::int16_t i = 0; i < 0x24; ++i) {
        ItemSlot from;
        from.type = kInvLimbo;
        from.slot = i;
        if (!inventory_.at(from)) continue;
        std::optional<Item> item = inventory_.take(from);
        ItemSlot origin = item->slot; // the item's own global index (+0x38..+0x40)
        inventory_.put(cursor_slot(), std::move(*item));
        MoveItem m;
        m.from = origin;
        m.to = cursor_slot();
        m.count = 0;
        send_zone(encode(m));
        return;
    }
}

// ---------------------------------------------------------------- packets

void GameConnection::handle_item(Op op, const AppPacket& p) {
    auto say = [&](std::uint32_t id, std::vector<std::string> args = {}) {
        chat(ChatEvent::Kind::System, strings_.format(id, args));
    };
    switch (op) {
    case Op::CharInventory:
        if (auto items = decode_char_inventory(p)) {
            log::info("inventory: {} items ({} bytes)", items->size(), p.data.size());
            // Hailstone: with EQEmu's Inventory:LazyLoadBank (on by default) the bank is not in it and comes later,
            // near a banker: the bank as it was goes into the copy listed until then (game/bank_snapshot.h).
            capture_bank();
            const bool with_bank = std::any_of(items->begin(), items->end(), [](const Item& it) {
                return it.slot.type == kInvBank || it.slot.type == kInvSharedBank;
            });
            inventory_.load(std::move(*items));
            set_bank_live(with_bank);
        } else {
            log::warn("could not decode OP_CharInventory ({} bytes)", p.data.size());
        }
        break;
    case Op::ItemPacket:
    case Op::ItemLinkResponse: {
        auto ip = decode_item_packet(p);
        if (!ip) {
            log::warn("could not decode {} ({} bytes)", op_name(op), p.data.size());
            break;
        }
        Item& item = ip->item;
        if (op == Op::ItemLinkResponse || ip->type == kItemPacketViewLink) {
            item_displays_.push_back(std::move(item));
            if (item_displays_.size() > 16) item_displays_.pop_front();
            break;
        }
        // CEverQuest_HandleItemPacket @004c2570.
        if (item.item_type == 13 || item.id < 1) {
            say(1366); // Got a bogus item, deleting it
            break;
        }
        if (item.stackable() && item.stack_count < 1) {
            say(1368);
            break;
        }
        switch (ip->type) {
        case kItemPacketLoot:
            if (loot_) loot_->items[item.slot.slot] = std::move(item);
            break;
        case kItemPacketParcel:
            // a parcel waiting at the merchant (its header slot is the parcel slot)
            if (merchant_) {
                const auto slot = static_cast<std::uint32_t>(item.slot.slot);
                merchant_->parcels[slot] = {std::move(item), ip->parcel_sent_time, ip->parcel_from, ip->parcel_note};
            }
            break;
        case kItemPacketMerchant:
        case kItemPacketTradeView:
            commerce_item(ip->type, std::move(item)); // game_connection_commerce.cpp
            break;
        case kItemPacketLimbo: {
            // ItemPacket_PlaceItemIfSlotEmpty @0x4b9b90: the server's cursor queue sends the next item when the
            // cursor is free; one for an occupied slot is dropped without a word.
            ItemSlot at = item.slot;
            if (!inventory_.at(at)) inventory_.put(at, std::move(item));
            break;
        }
        default: {
            log::debug("item packet {:#x}: {} at {}/{}/{}", ip->type, item.name, item.slot.type, item.slot.slot, item.slot.sub);
            ItemSlot at = item.slot;
            at.aug = -1;
            // Type 0x67 (Trade): 0x6ae710(item, 0) re-runs SetItem on every display showing this serial (the
            // server keeps the host's serial when it rebuilds it after an augment change, so it refreshes in place).
            if (ip->type == kItemPacketTrade) {
                item_refreshes_.push_back(item);
                if (item_refreshes_.size() > 16) item_refreshes_.pop_front();
            }
            const bool banked = at.type == kInvBank || at.type == kInvSharedBank;
            if (!inventory_.put(at, std::move(item))) log::debug("item packet for unusable slot {}/{}", at.type, at.slot);
            // Hailstone: an item for a bank slot is the server's bank now (EQEmu's lazy load sends the bank this
            // way, then the shared bank, all in one pass): the copy as last seen is no longer listed.
            else if (banked) set_bank_live(true);
            if (is_worn(at)) refresh_worn_slot(at.slot);
            break;
        }
        }
        break;
    }
    case Op::ItemVerifyReply:
        if (auto r = decode_item_verify_reply(p)) handle_item_verify_reply(*r);
        break;
    case Op::ApplyPoison:
        if (auto r = decode_apply_poison(p)) handle_apply_poison(*r);
        break;
    case Op::MoveItem:
    case Op::DeleteCharge:
        if (auto m = decode_move_item(p)) {
            log::debug("server {}: {}/{}/{} -> {}/{}/{} count {}", op_name(op), m->from.type, m->from.slot, m->from.sub,
                       m->to.type, m->to.slot, m->to.sub, m->count);
            apply_server_move(*m);
        }
        break;
    case Op::DeleteItem:
        // Handle_OP_DeleteItem @0x4b9900: a negative count comes off the stack;
        // an emptied stack is removed.
        if (auto m = decode_move_item(p)) {
            auto count = static_cast<std::int32_t>(m->count);
            Item* it = inventory_.at(m->from);
            if (count > -1 || !it) break;
            std::int64_t left = static_cast<std::int64_t>(it->stack_count) + count;
            if (left > 0) it->stack_count = static_cast<std::uint32_t>(left);
            else inventory_.take(m->from);
            if (is_worn(m->from)) refresh_worn_slot(m->from.slot);
        }
        break;
    case Op::MoneyOnCorpse:
        // Handle_OP_MoneyOnCorpse @0x6be950 (only while a corpse is being looted).
        if (auto m = decode_money_on_corpse(p); m && loot_) {
            switch (m->response) {
            case 6: // every item has been sent: a Loot All asked for before this runs now
                loot_->complete = true;
                if (loot_all_deferred_) {
                    loot_all_deferred_ = false;
                    loot_all_now();
                }
                break;
            case 1:
            case 3: {
                loot_->open = true;
                loot_->money = {m->platinum, m->gold, m->silver, m->copper};
                coins_.platinum += m->platinum;
                coins_.gold += m->gold;
                coins_.silver += m->silver;
                coins_.copper += m->copper;
                if (static_cast<std::int64_t>(static_cast<std::int32_t>(m->platinum)) + static_cast<std::int32_t>(m->gold) +
                        static_cast<std::int32_t>(m->silver) + static_cast<std::int32_t>(m->copper) > 0) {
                    SoundCue cue;
                    cue.kind = SoundCue::Kind::Wave;
                    cue.spell = 0x8a;
                    sound_cues_.push_back(cue);
                    // With /autosplit on (the profile's +0x46c) "You receive %1 as your split.", else
                    // "You receive %1 from the corpse." (colour 0x11d).
                    system(autosplit_ ? 12071 : 12072,
                           {game::coin_amounts_text(m->platinum, m->gold, m->silver, m->copper)}, 0x11d);
                }
                loot_->complete = false;
                loot_->items.clear();
                // unk_SetStandState(0x69): the player kneels to loot.
                set_stand_state(kStandLooting);
                break;
            }
            case 0: loot_failed(12073); break; // Someone is already looting that corpse.
            case 2: loot_failed(12070); break; // You may not loot this corpse at this time.
            case 4: loot_failed(5154); break;  // You cannot loot while a hostile creature is aware of your presence.
            case 5: loot_failed(12390); break; // You are too far away to loot that corpse.
            case 7: loot_failed(0); break;     // closes without a word
            default: loot_->open = true; break; // the window shows
            }
        }
        break;
    case Op::LootRequest:
        // The server echoes the request after the items (the original has no handler for it).
        break;
    case Op::LootItem:
        if (auto l = decode_loot_item(p)) {
            if (!loot_) break;
            // Handle_OP_LootItem @0x6bd210: a negative auto loot or slot is a refusal ("You may not loot that item
            // from this corpse.", 0x111); otherwise the looted item leaves the window, or with a count below its
            // stack only that many; then the loot-all queue sends its next request.
            auto slot = static_cast<std::int32_t>(static_cast<std::uint32_t>(l->slot) | (static_cast<std::uint32_t>(l->unknown10) << 16));
            if (l->auto_loot < 0 || slot < 0) {
                loot_close_on_complete_ = false;
                system(1370, {}, 0x111);
            } else if (auto it = loot_->items.find(static_cast<std::int16_t>(slot)); it != loot_->items.end()) {
                auto count = static_cast<std::int32_t>(l->unknown16);
                auto stack = static_cast<std::int32_t>(it->second.stack_count);
                if (stack > 1 && count > 0 && count < stack) it->second.stack_count = static_cast<std::uint32_t>(stack - count);
                else loot_->items.erase(it);
            }
            if (!loot_all_queue_.empty()) {
                std::int16_t next = loot_all_queue_.front();
                loot_all_queue_.pop_front();
                send_loot_item(next, true);
                break;
            }
            // The last answer of a Loot All closes the window (bCloseOnComplete, unless a no-drop question is open).
            if (loot_close_on_complete_ && !loot_popup_pending_) end_loot();
        }
        break;
    case Op::LootComplete:
        // unk_CLootWnd_ResetLootSession @0x6bd0b0: the target goes, a looting player stands up, the window hides.
        reset_loot_session();
        break;
    case Op::MoneyUpdate:
        if (auto c = decode_money_update(p)) coins_ = *c;
        break;
    case Op::Sound:
    case Op::CashReward: {
        // EQEmu's QuestReward / CashReward add the coin (AddMoneyToPP, per denomination) without a coin update.
        auto r = op == Op::Sound ? decode_quest_reward(p) : decode_cash_reward(p);
        if (!r || (op == Op::Sound && r->target_id != world_.player_id)) break;
        coins_.platinum += r->coins.platinum;
        coins_.gold += r->coins.gold;
        coins_.silver += r->coins.silver;
        coins_.copper += r->coins.copper;
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------- moves

void GameConnection::refresh_worn_slot(int slot) {
    // PcClient_RefreshWornSlotVisual: our own model shows what we wear, its
    // ornament included (game::worn_look). The server sends OP_WearChange to
    // everyone else, not to us.
    int ms = game::material_slot(slot);
    if (ms < 0) return;
    auto it = world_.spawns.find(world_.player_id);
    if (it == world_.spawns.end()) return;
    const Item* item = inventory_.at(ItemSlot::possessions(static_cast<std::int16_t>(slot)));
    game::WornLook look;
    if (item) look = game::worn_look(*item, ms);
    game::apply_wear_change(it->second.appearance, static_cast<std::uint8_t>(ms), look.material, look.hero_forge,
                            look.tint);
}

void GameConnection::move_item(const ItemSlot& from, const ItemSlot& to, std::uint32_t count) {
    // CInvSlotMgr::MoveItem @00698d80: swap locally, then tell the server.
    if (!inventory_.swap(from, to)) return;
    log::debug("move item {}/{}/{} -> {}/{}/{} count {}", from.type, from.slot, from.sub, to.type, to.slot, to.sub, count);
    MoveItem m;
    m.from = from;
    m.to = to;
    m.count = count;
    send_zone(encode(m));
    if (is_worn(from)) refresh_worn_slot(from.slot);
    if (is_worn(to)) refresh_worn_slot(to.slot);
    // CTradeWnd_OnTradeItemMoved (game/trade_rules.h): a move into or out of a trade slot clears both ready flags.
    if (trade_) {
        game::TradeFlags f{trade_->their_ready, trade_->my_ready};
        if (game::trade_item_moved(f, from.type == kInvTrade ? 3 : 0, to.type == kInvTrade ? 3 : 0)) {
            trade_->their_ready = f.his_ready != 0;
            trade_->my_ready = f.my_ready != 0;
        }
    }
}

void GameConnection::click_slot(const ItemSlot& slot, bool shift, bool ctrl) {
    if (state_ != ConnectionState::InZone || pstatus_.dead) return;
    if (slot.type == kInvCorpse) {
        loot_item(slot.slot, false, shift, ctrl);
        return;
    }
    if (slot.type == kInvTrade) {
        // A click on an own trade slot drops the cursor item there (items never leave the trade).
        if (inventory_.cursor()) trade_drop_cursor();
        return;
    }
    // CInvSlot::HandleLButtonUp. Bank slots exist only in the
    // open bank window; one beyond CBankWnd::GetNumBankSlots is ignored
    // (InvSlot_IsBankSlotBeyondLimit @00694140), as are shared bank slots
    // beyond the window's two.
    if (slot.type == kInvBank || slot.type == kInvSharedBank) {
        if (!bank_ || slot.slot < 0) return;
        if (slot.type == kInvBank && slot.slot >= bank_->slots) return;
        if (slot.type == kInvSharedBank && slot.slot >= game::kSharedBankSlots) return;
    } else if (slot.type == kInvWorld) {
        // An open tradeskill container's slots (OP_ClickObjectAction; EQEmu's world containers hold 10).
        if (!world_container_ || slot.slot < 0 || slot.slot >= game::kWorldContainerSlots || slot.sub >= 0) return;
    } else if (slot.type != kInvPossessions || slot.slot < 0 || slot.slot > kSlotCursor) {
        return;
    }
    const ItemSlot cursor = cursor_slot();
    if (slot.same(cursor)) return;
    if (quantity_) return; // the quantity dialog is open
    // While a merchant window is open a click selects the item for it and
    // nothing moves (step 3).
    if (merchant_ && merchant_->open) {
        if (slot.type == kInvPossessions) merchant_select(slot);
        return;
    }
    Item* held = inventory_.at(cursor);
    Item* there = inventory_.at(slot);
    auto say = [&](std::uint32_t id, std::vector<std::string> args) {
        chat(ChatEvent::Kind::System, strings_.format(id, args));
    };
    if (!held) {
        // Pick up (step 4).
        if (!there) return;
        if (there->stackable()) {
            // CQuantityWnd::Open(max = min(StackCount, MaxStackSize), start max, min 0, useKeys)
            // (game/quantity_dialog.h): Shift takes all, Ctrl one, a stack of one goes at once;
            // otherwise the dialog opens at the maximum.
            game::QuantityOpen q;
            q.max = static_cast<std::int32_t>(std::min(there->stack_count, there->max_stack));
            q.start = q.max;
            q.shift = shift;
            q.ctrl = ctrl;
            game::QuantityResult r = game::quantity_dialog_open(q);
            if (r.closed) return;
            quantity_ = QuantityRequest{slot, static_cast<std::uint32_t>(q.max), static_cast<std::uint32_t>(r.slider)};
            if (r.accepted) finish_quantity(static_cast<std::uint32_t>(r.quantity));
            return;
        }
        if (slot.type == kInvSharedBank) {
            if (auto r = game::lore_pickup_refusal(*there, inventory_)) {
                say(r.eqstr, {there->name});
                return;
            }
        }
        move_item(slot, cursor, 0);
        return;
    }
    // Put down (step 5).
    if (slot.type == kInvPossessions && slot.sub < 0 && slot.slot < kEquipmentSlots) {
        const Item* primary = inventory_.at(ItemSlot::possessions(kSlotPrimary));
        const Item* secondary = inventory_.at(ItemSlot::possessions(kSlotSecondary));
        bool dual_wield = profile_ && profile_->skills.size() > 22 && profile_->skills[22] > 0;
        // CInvSlot::HandleLButtonUp: CanItemGoInEquipSlot false does nothing, without
        // a word; then CanUseItem(cursor, 0, 0), the class check only, else eqstr 6611 (colour 0xd).
        if (!game::can_go_in_equip_slot(*held, slot.slot, primary, secondary, dual_wield, inv_slot_available(slot.slot)))
            return;
        // A server's client may answer CanUseItem true for everything (the NMS patch's detour; only the server checks).
        if (!server_ext().can_use_all_items(*this) && !game::can_use_class(*held, player_class_mask())) {
            say(game::kEqstrCannotEquipClass, {held->name}); // Your class, deity, and/or race may not equip %1
            return;
        }
        // A worn slot and an Attuneable item whose NoDrop flag is clear: ConfirmationDialog type 0x7a with eqstr
        // 5912 "Are you sure you wish to Attune your %1 to your character?"; nothing moves until the answer.
        if (held->attuneable && held->attuned == 0) {
            ask(Confirmation{kConfirmAttune, strings_.format(5912, std::vector<std::string>{held->name}), {}, slot});
            return;
        }
    }
    if (slot.type == kInvBank) {
        if (auto r = game::bank_refusal(*held)) {
            say(r.eqstr, {held->name});
            return;
        }
    } else if (slot.type == kInvSharedBank) {
        // The item coming out (a swap) must pass the lore test first.
        if (there) {
            if (auto r = game::lore_pickup_refusal(*there, inventory_)) {
                say(r.eqstr, {there->name});
                return;
            }
        }
        if (auto r = game::shared_bank_refusal(*held, inventory_)) {
            say(r.eqstr, {held->name, r.arg});
            return;
        }
    }
    // MoveItem with bMoveBagContents (Ctrl held, bForceBag 0): a bag at a top-level slot outside a world container
    // takes the cursor item, or the cursor bag's contents, through OP_MoveMultipleItems.
    if (ctrl && there && there->is_bag() && slot.sub < 0 && slot.type != kInvWorld) {
        move_into_bag(slot);
        return;
    }
    if (slot.sub >= 0) {
        const Item* bag = inventory_.at(slot.parent());
        if (!bag || !bag->is_bag() || slot.sub >= bag->bag_slots) return;
        int fit = game::bag_fit(*bag, *held);
        if (fit != 1) {
            if (auto id = game::bag_fit_message(fit)) say(id, {held->name, bag->name});
            return;
        }
    }
    // Send_OP_MoveItem / ItemContainer_MergeStacks: as much as fits. A full
    // stack of the same item does not swap (MoveItem fails on CanStackWith).
    if (there && game::can_stack_with(*held, *there)) {
        merge_stacks(cursor, slot);
        return;
    }
    move_item(cursor, slot, 0);
}

void GameConnection::finish_quantity(std::uint32_t quantity) {
    // CInvSlot::SliderComplete @00695270: 0 does nothing; less than the
    // stack clones the item with that count onto the cursor, takes it off
    // the source and sends OP_MoveItem {slot, cursor, quantity}; the whole
    // stack is an ordinary move (count 0).
    if (!quantity_) return;
    QuantityRequest q = *quantity_;
    quantity_.reset();
    if (q.loot) {
        // CLootWnd::WndNotification @0x6be180 message 0x24: the kept request with the quantity, sent as it is (0
        // included).
        if (state_ == ConnectionState::InZone) send_loot_item(q.slot.slot, q.auto_loot, quantity);
        return;
    }
    if (state_ != ConnectionState::InZone || quantity == 0) return;
    Item* item = inventory_.at(q.slot);
    if (!item) return;
    const ItemSlot cursor = cursor_slot();
    if (quantity < item->stack_count) {
        // The clone goes into the cursor slot without a check (an item already there is
        // overwritten).
        Item part = *item;
        part.stack_count = quantity;
        item->stack_count -= quantity;
        inventory_.put(cursor, std::move(part));
        MoveItem m;
        m.from = q.slot;
        m.to = cursor;
        m.count = quantity;
        send_zone(encode(m));
        return;
    }
    if (inventory_.cursor()) return;
    move_item(q.slot, cursor, 0);
}

void GameConnection::destroy_cursor() {
    if (state_ != ConnectionState::InZone) return;
    if (!inventory_.cursor()) {
        // DestroyHeldItemOrMoney's coin part: OP_MoveCoin {cursor, -1, the highest coin held twice, its amount}, then
        // the cursor's coin is gone.
        std::uint32_t coins[4] = {cursor_coins_[0], cursor_coins_[1], cursor_coins_[2], cursor_coins_[3]};
        if (auto d = game::destroy_cursor_coin(coins)) {
            send_zone(encode(MoveCoin{kCoinCursor, -1, d->type, d->type, d->amount}));
            cursor_coins_ = {};
        }
        return;
    }
    if (refuse_kept(inventory_.cursor(), "destroy")) return; // Hailstone: a kept item stays (game/kept_items.h)
    // DestroyHeldItemOrMoney @005862f0: OP_MoveItem {cursor, nowhere, 0}.
    MoveItem m;
    m.from = cursor_slot();
    m.to = ItemSlot::invalid();
    send_zone(encode(m));
    inventory_.take(cursor_slot());
}

void GameConnection::destroy_held() {
    // CInventoryWnd::DestroyHeld @0x692340 (game/item_options.h).
    if (state_ != ConnectionState::InZone || !profile_) return;
    const Item* held = inventory_.cursor();
    if (refuse_kept(held, "destroy")) return; // Hailstone: before the confirmation, not after it
    game::DestroyInput in;
    in.item = held != nullptr;
    in.money = std::any_of(cursor_coins_.begin(), cursor_coins_.end(), [](std::uint32_t c) { return c != 0; });
    in.satchel_in_trader = held && held->is_bag() && held->bag_type == 0x33 && trader_.window_open && trader_.trader;
    in.fast_destroy = client_option(game::ClientOption::FastItemDestroy);
    game::DestroyResult r = game::destroy_held_decision(in);
    switch (r.decision) {
    case game::DestroyDecision::Nothing:
        return;
    case game::DestroyDecision::SatchelRefused: {
        Confirmation c;
        c.id = kConfirmNotice;
        c.text = game::kDestroySatchelText;
        c.timeout_ms = game::kDestroyConfirmMs;
        c.ok_only = true;
        ask(std::move(c));
        return;
    }
    case game::DestroyDecision::DestroyNow:
        destroy_cursor();
        return;
    case game::DestroyDecision::Confirm: {
        Confirmation c;
        c.id = kConfirmDestroy;
        c.text = r.eqstr == game::kStrDestroyMoney ? strings_.format(r.eqstr, std::vector<std::string>{})
                                                   : strings_.format(r.eqstr, std::vector<std::string>{held->name});
        c.timeout_ms = game::kDestroyConfirmMs;
        ask(std::move(c));
        return;
    }
    }
}

bool GameConnection::auto_inventory() {
    // AutoInventoryCursorItem @004fe090: a stack first merges into the worn slots (0..22) holding the same item that
    // CanItemGoInEquipSlot takes, when CharacterZoneClient::CanUseItem(item, 0, 0) (the class check) passes
    // (Send_OP_MoveItem; done when all of it went); then a common item that is not an unattuned Attuneable goes into
    // the first empty worn slot taking it with the same class check; else the first free general slot, else the
    // first bag with room (PcZoneClient_AutoInventoryOrDropItem).
    if (state_ != ConnectionState::InZone) return false;
    const Item* held = inventory_.cursor();
    if (!held) return false;
    const ItemSlot cursor = cursor_slot();
    const Item* primary = inventory_.at(ItemSlot::possessions(kSlotPrimary));
    const Item* secondary = inventory_.at(ItemSlot::possessions(kSlotSecondary));
    bool dual_wield = profile_ && profile_->skills.size() > 22 && profile_->skills[22] > 0;
    if (held->stackable() && can_use_item_class(*held)) {
        for (std::int16_t s = 0; s < kEquipmentSlots && held; ++s) {
            const Item* there = inventory_.at(ItemSlot::possessions(s));
            if (!there || there->id != held->id ||
                !game::can_go_in_equip_slot(*held, s, primary, secondary, dual_wield, inv_slot_available(s)))
                continue;
            if (merge_stacks(cursor, ItemSlot::possessions(s)) == 0) return true;
            held = inventory_.cursor();
        }
    }
    if (held->item_class == 0 && !(held->attuneable && !held->attuned)) {
        for (std::int16_t s = 0; s < kEquipmentSlots; ++s) {
            ItemSlot to = ItemSlot::possessions(s);
            if (inventory_.at(to) ||
                !game::can_go_in_equip_slot(*held, s, primary, secondary, dual_wield, inv_slot_available(s)) ||
                !can_use_item_class(*held))
                continue;
            move_item(cursor, to, 0);
            return true;
        }
    }
    return stow_cursor_in_packs();
}

bool GameConnection::stow_cursor_in_packs() {
    if (state_ != ConnectionState::InZone) return false;
    const Item* held = inventory_.cursor();
    if (!held) return false;
    const ItemSlot cursor = cursor_slot();
    for (std::int16_t s = kSlotGeneral1; s <= kSlotGeneral10; ++s) {
        if (!inventory_.at(ItemSlot::possessions(s))) {
            move_item(cursor, ItemSlot::possessions(s), 0);
            return true;
        }
    }
    for (std::int16_t s = kSlotGeneral1; s <= kSlotGeneral10; ++s) {
        const Item* bag = inventory_.at(ItemSlot::possessions(s));
        if (!bag || !bag->is_bag() || game::bag_fit(*bag, *held) != 1) continue;
        for (std::int16_t i = 0; i < bag->bag_slots; ++i) {
            ItemSlot to = ItemSlot::possessions(s, i);
            if (!inventory_.at(to)) {
                move_item(cursor, to, 0);
                return true;
            }
        }
    }
    return false;
}

void GameConnection::drop_on_doll() {
    if (state_ != ConnectionState::InZone) return;
    if (inventory_.cursor()) {
        auto_inventory();
        if (const Item* still = inventory_.cursor(); still && still->id != kKronoItemId) system(5290, {}, 0xd);
        return;
    }
    // CDisplay +0x11: 1 + the first cursor coin counter above zero, platinum first (cursor_coins_ is copper first).
    for (int c = 0; c < 4; ++c)
        if (cursor_coins_[static_cast<std::size_t>(3 - c)] > 0) {
            inventory_money_button(c, -1, nullptr);
            return;
        }
}

bool GameConnection::inv_slot_available(int slot) const {
    // CharacterBase_IsInvSlotAvailable: the profile's slot mask (+0x341c, PcBase_SetInventorySlotBitmask, from the
    // player profile); the power source needs expansion flag 0x1000; the general slots go up to 32
    // (GetLastUsableInvSlot with the full inventory).
    return game::inv_slot_available(slot, profile_ ? profile_->inventory_slot_mask : 0xffffffffu,
                                    profile_ ? profile_->expansions : 0, 32);
}

bool GameConnection::can_use_item_full(const Item& item) const {
    // CharacterZoneClient::CanUseItem(item, 1, 0): class, then deity, level, race (not for scrolls) and prestige
    // (taken as allowed: the free-to-play restriction does not apply on this server). A server's client may answer
    // true for everything (the NMS patch's detour).
    if (server_ext().can_use_all_items(*this)) return true;
    const game::Spawn* me = world_.player();
    std::int32_t level = static_cast<std::int32_t>(pstatus_.level);
    std::int32_t race = profile_ ? static_cast<std::int32_t>(profile_->race) : (me ? static_cast<std::int32_t>(me->race) : 0);
    std::int32_t deity = profile_ ? static_cast<std::int32_t>(profile_->deity) : 0;
    return game::can_use_item(
        item.item_class, item.classes, player_class_mask(), true, item.item_type,
        [&] { return game::can_use_item_deity(true, deity, item.deity, false, true, nullptr); },
        [&] { return game::can_use_item_level(level, static_cast<std::int32_t>(item.req_level), false, true, nullptr); },
        [&] { return game::can_use_item_race(race, item.races, false, true, nullptr); }, [] { return true; });
}

void GameConnection::equip_attuned(const ItemSlot& slot) {
    // ConfirmationDialog_HandleResponse case 0x7a (yes, with a local PC): MoveItem(cursor 33, slot, 1, 1, 0, 0),
    // which sets NoDropFlag 1 for an attuneable item landing in a worn slot and sends the usual OP_MoveItem
    // {from 33, to slot, count 0}; then sound 0x92. A no leaves the item on the cursor.
    if (state_ != ConnectionState::InZone) return;
    Item* held = inventory_.at(cursor_slot());
    if (!held || !is_worn(slot)) return;
    held->attuned = 1;
    move_item(cursor_slot(), slot, 0);
    SoundCue cue;
    cue.kind = SoundCue::Kind::Wave;
    cue.spell = 0x92;
    sound_cues_.push_back(cue);
}

void GameConnection::move_into_bag(const ItemSlot& to, bool force_bag) {
    // CInvSlotMgr::MoveItem's bag path: a placeable target or cursor bag prints eqstr
    // 5311 and fails. Then the list (the cursor item, or each item of a non-empty cursor bag, which stays on the
    // cursor; with bForceBag the target bag's items into the cursor bag too) is resolved as ProcessMove resolves it
    // (game/bag_move.h: stacks first, then the first free slot) and sent as OP_MoveMultipleItems only when every
    // entry found a place; else 12934 / 5215 / 5214.
    const ItemSlot cursor = cursor_slot();
    const Item* held = inventory_.at(cursor);
    const Item* bag = inventory_.at(to);
    if (!held || !bag || !bag->is_bag()) return;
    auto say = [&](std::uint32_t id, std::vector<std::string> args) {
        chat(ChatEvent::Kind::System, strings_.format(id, args));
    };
    if (bag->placeable_flags() != 0 || (held->is_bag() && held->placeable_flags() != 0)) {
        say(5311, {bag->placeable ? bag->name : held->name});
        return;
    }
    game::BagMovePlan plan = game::plan_bag_move(*held, *bag, force_bag);
    if (plan.eqstr) {
        say(plan.eqstr, plan.args);
        return;
    }
    if (plan.moves.empty()) return;
    auto source = [&](const game::BagMove& m) {
        switch (m.from) {
        case game::BagMove::Where::Cursor: return cursor;
        case game::BagMove::Where::CursorBag: return ItemSlot::possessions(kSlotCursor, static_cast<std::int16_t>(m.from_sub));
        default: return ItemSlot{to.type, to.slot, static_cast<std::int16_t>(m.from_sub), -1, 0, 0};
        }
    };
    auto destination = [&](const game::BagMove& m) {
        return m.to_cursor_bag ? ItemSlot::possessions(kSlotCursor, static_cast<std::int16_t>(m.to_sub))
                               : ItemSlot{to.type, to.slot, static_cast<std::int16_t>(m.to_sub), -1, 0, 0};
    };
    std::vector<MultiMoveEntry> moves;
    for (const game::BagMove& m : plan.moves) {
        MultiMoveEntry e;
        e.from = source(m);
        e.to = destination(m);
        e.count = m.count;
        moves.push_back(e);
    }
    auto packet = encode_move_multiple(moves);
    if (!packet) return;
    log::debug("move {} item(s) into the bag at {}/{}", moves.size(), to.type, to.slot);
    send_zone(*packet);
    // Placed as PlaceItems does: every source out first, then each part onto its slot (merged onto a stack there).
    std::vector<std::pair<ItemSlot, Item>> out;
    for (const MultiMoveEntry& e : moves) {
        bool have = std::any_of(out.begin(), out.end(), [&](const auto& o) { return o.first.same(e.from); });
        if (have) continue;
        if (auto item = inventory_.take(e.from)) out.emplace_back(e.from, std::move(*item));
    }
    for (const MultiMoveEntry& e : moves) {
        auto it = std::find_if(out.begin(), out.end(), [&](const auto& o) { return o.first.same(e.from); });
        if (it == out.end()) continue;
        Item part = it->second;
        part.stack_count = e.count;
        if (Item* there = inventory_.at(e.to); there && game::can_stack_with(part, *there)) there->stack_count += e.count;
        else inventory_.put(e.to, std::move(part));
    }
}

void GameConnection::apply_server_move(const MoveItem& m) {
    auto index = [](const ItemSlot& s) {
        game::GlobalIndex g;
        g.location = static_cast<std::int32_t>(static_cast<std::uint16_t>(s.type) |
                                               (static_cast<std::uint32_t>(static_cast<std::uint16_t>(s.unknown02)) << 16));
        g.slot = s.slot;
        g.sub = s.sub;
        g.aug = s.aug;
        return g;
    };
    game::GlobalIndex from = index(m.from), to = index(m.to);
    // CEverQuest::HandleWorldMessage @0x4c88cb: an invalid `to` with `from` in the trade or a world container is
    // Handle_OP_MoveItem_RemoveFromTradeOrWorld @0x4b9a30: `count` comes off the stack, or the item goes.
    if (!game::global_index_valid(to) && (from.location == kInvTrade || from.location == kInvWorld)) {
        Item* it = inventory_.at(m.from);
        if (it && it->stackable() && m.count > 0 && m.count < it->stack_count) it->stack_count -= m.count;
        else inventory_.take(m.from);
        return;
    }
    // Handle_OP_MoveItem_Apply @0x4c1ba0: only possessions to possessions moves; an invalid
    // `to` deletes a possessions, bank or limbo item; any false result prints eqstr 1389.
    Item* src = inventory_.at(m.from);
    Item* dst = inventory_.at(m.to);
    game::MoveApply plan = game::plan_move_item_apply(
        from, to, src != nullptr, dst != nullptr, [&] { return game::can_stack_with(*src, *dst); },
        [&] { return src->stackable(); });
    bool ok = false;
    auto count = static_cast<std::int32_t>(m.count);
    switch (plan) {
    case game::MoveApply::NoItem:
    case game::MoveApply::Refused:
    case game::MoveApply::WrongLocation:
        break;
    case game::MoveApply::Delete:
        // An open bag closes with it (the bag windows follow the inventory).
        inventory_.take(m.from);
        ok = true;
        break;
    case game::MoveApply::MergeCount:
        if (auto n = game::move_item_count(count, src->stackable(), static_cast<std::int32_t>(src->stack_count),
                                           static_cast<std::int32_t>(dst->stack_count),
                                           static_cast<std::int32_t>(dst->max_stack))) {
            dst->stack_count += static_cast<std::uint32_t>(*n);
            src->stack_count -= static_cast<std::uint32_t>(*n);
            if (src->stack_count == 0) inventory_.take(m.from);
            ok = true;
        }
        break;
    case game::MoveApply::AdjustCount: {
        game::StackAdjust a = game::adjust_item_stack_count(src->stackable(), src->is_lore(),
                                                            static_cast<std::int32_t>(src->stack_count), count);
        if (a.applied) {
            if (a.remove) inventory_.take(m.from);
            else src->stack_count = static_cast<std::uint32_t>(a.new_count);
        }
        ok = !a.applied; // the original's true result counts as failure
        break;
    }
    case game::MoveApply::Swap:
        inventory_.swap(m.from, m.to);
        ok = true;
        break;
    }
    if (!ok) chat(ChatEvent::Kind::System, strings_.format(game::kEqstrMoveItemFailed, std::vector<std::string>{}));
    if (from.location == kInvPossessions && is_worn(m.from)) refresh_worn_slot(m.from.slot);
    if (to.location == kInvPossessions && game::global_index_valid(to) && is_worn(m.to)) refresh_worn_slot(m.to.slot);
}

// ---------------------------------------------------------------- loot

void GameConnection::loot(std::uint32_t corpse_id) {
    // CEverQuest::LootCorpse @0x536720 (__do_loot, a right click on a corpse): a corpse (type above 1), not while
    // dead, an item request is pending or another corpse is open; beyond its AvatarHeight * 1.6 + 14 (PlayerDistance
    // with z scale 1.2) "You are too far away to loot that corpse." (colour 0xd); else OP_LootRequest.
    if (state_ != ConnectionState::InZone || pstatus_.dead || loot_) return;
    auto it = world_.spawns.find(corpse_id);
    if (it == world_.spawns.end() || !it->second.is_corpse) return;
    const game::Spawn* me = world_.player();
    if (!me) return;
    glm::vec3 at = motion_known_ ? position_ : me->position;
    if (game::player_distance(&it->second.position, &at, 1.2f) > it->second.heights.avatar_height * 1.6f + 14.0f) {
        system(12390, {}, 0xd);
        return;
    }
    loot_ = LootSession{};
    loot_->corpse_id = corpse_id;
    loot_->corpse_name = game::display_name(it->second.name);
    loot_all_queue_.clear();
    loot_all_deferred_ = false;
    loot_close_on_complete_ = false;
    send_zone(encode_loot_request(corpse_id));
}

void GameConnection::loot_failed(std::uint32_t eqstr) {
    // Handle_OP_MoneyOnCorpse's refusals: the line (0x111), and the corpse is let go without showing the window.
    if (eqstr) system(eqstr, {}, 0x111);
    loot_.reset();
    loot_all_queue_.clear();
}

void GameConnection::reset_loot_session() {
    // unk_CLootWnd_ResetLootSession @0x6bd0b0 (OP_LootComplete): no target, no corpse, a looting player stands
    // (100), the window hides. The target is cleared without telling the server (pinstTarget = 0).
    if (loot_) world_.target_id = 0;
    loot_.reset();
    loot_all_queue_.clear();
    loot_all_deferred_ = false;
    loot_close_on_complete_ = false;
    // A NO DROP or Loot All question still open was about this corpse: a later yes would loot that slot of the next
    // corpse opened. Whether the original's dialog outlives the session is not known (docs/unknowns.md); it is
    // dropped here, and with it the window's popup flag.
    discard_questions([](const Confirmation& c) { return c.id == kConfirmLootNoDrop || c.id == kConfirmLootAll; });
    loot_popup_pending_ = false;
    if (pstatus_.stand_state == kStandLooting) set_stand_state(kStandStanding);
}

void GameConnection::update_loot() {
    // CLootWnd::OnProcessFrame @0x6bc910: every 500 ms, the corpse beyond its AvatarHeight * 1.5 + 20 (PlayerDistance
    // without z) closes the window.
    if (!loot_ || !loot_->open) return;
    std::int64_t now = now_ms();
    if (now < loot_check_ms_) return;
    loot_check_ms_ = now + 500;
    auto it = world_.spawns.find(loot_->corpse_id);
    const game::Spawn* me = world_.player();
    if (it == world_.spawns.end() || !me) return;
    glm::vec3 at = motion_known_ ? position_ : me->position;
    if (game::player_distance(&it->second.position, &at, 0.0f) > it->second.heights.avatar_height * 1.5f + 20.0f)
        end_loot();
}

bool GameConnection::loot_asks_no_drop(const Item& item) const {
    // LootAll / RequestLootSlot (game::loot_no_drop_asks): CanDrop, LootNoDrop, CanUseItem(item, 0, 0).
    return game::loot_no_drop_asks(looting_own_corpse(), game::is_droppable(item),
                                   client_options_.get(game::ClientOption::LootNoDrop),
                                   [&] { return can_use_item_class(item); });
}

bool GameConnection::can_use_item_class(const Item& item) const {
    // CharacterZoneClient::CanUseItem(item, 0, 0): the class check alone (a bag or book always).
    if (server_ext().can_use_all_items(*this)) return true;
    return game::can_use_item(item.item_class, item.classes, player_class_mask(), false, item.item_type, {}, {}, {}, {});
}

void GameConnection::loot_item(std::int16_t corpse_slot, bool auto_loot, bool shift, bool ctrl) {
    // CLootWnd::RequestLootSlot(slot, auto loot) @0x6be3a0: nothing while coin is held, or an item without auto loot;
    // a no-drop item may ask first ("%s is a NO DROP item, ...", 20 s; the window's popup flag set until answered).
    // Then, on someone else's corpse, a stack (StackCount +0x98 above 1) keeps the request (+0x39c) and opens
    // CQuantityWnd::Open(owner the loot window, max = start = the stack, x = y = -1, use keys, min 0); the answer
    // (WndNotification 0x24) puts the quantity in the request's last dword and sends it. The player's own corpse,
    // and a single item, send at once with quantity 0.
    if (!loot_ || !loot_->open) return;
    loot_close_on_complete_ = false;
    if (std::any_of(cursor_coins_.begin(), cursor_coins_.end(), [](std::uint32_t c) { return c != 0; })) return;
    if (inventory_.cursor() && !auto_loot) return;
    auto it = loot_->items.find(corpse_slot);
    if (it == loot_->items.end()) return;
    if (loot_asks_no_drop(it->second)) {
        loot_popup_pending_ = true;
        Confirmation c;
        c.id = kConfirmLootNoDrop;
        c.text = game::loot_no_drop_question(it->second.name);
        c.timeout_ms = game::kLootConfirmMs;
        c.data = static_cast<std::uint16_t>(corpse_slot);
        c.data2 = auto_loot ? 1 : 0;
        ask(std::move(c));
        return;
    }
    if (game::loot_asks_quantity(looting_own_corpse(), static_cast<std::int32_t>(it->second.stack_count))) {
        game::QuantityOpen q;
        q.max = static_cast<std::int32_t>(it->second.stack_count);
        q.start = q.max;
        q.shift = shift;
        q.ctrl = ctrl;
        game::QuantityResult r = game::quantity_dialog_open(q);
        if (r.closed) return;
        if (r.accepted) {
            send_loot_item(corpse_slot, auto_loot, static_cast<std::uint32_t>(r.quantity));
            return;
        }
        QuantityRequest req;
        req.slot = ItemSlot::corpse(corpse_slot);
        req.max = static_cast<std::uint32_t>(q.max);
        req.start = static_cast<std::uint32_t>(r.slider);
        req.loot = true;
        req.auto_loot = auto_loot;
        req.centred = true;
        quantity_ = req;
        return;
    }
    send_loot_item(corpse_slot, auto_loot);
}

void GameConnection::send_loot_item(std::int16_t corpse_slot, bool auto_loot, std::uint32_t quantity) {
    if (!loot_ || !loot_->open) return;
    LootItem l;
    l.lootee = loot_->corpse_id;
    l.looter = world_.player_id;
    l.slot = static_cast<std::uint16_t>(corpse_slot);
    l.auto_loot = auto_loot ? 1 : 0;
    l.unknown16 = quantity; // the quantity asked (EQEmu loots the whole stack whatever it says)
    send_zone(encode(l));
}

void GameConnection::loot_all() {
    // CLootWnd::WndNotification @0x6be180, Loot All: at once from the player's own corpse or without LootAllConfirm,
    // else "Loot all, are you sure?" (11018, 20 s) first.
    if (!loot_ || !loot_->open) return;
    if (!game::loot_all_asks(looting_own_corpse(), client_options_.get(game::ClientOption::LootAllConfirm))) {
        loot_all_now();
        return;
    }
    Confirmation c;
    c.id = kConfirmLootAll;
    c.text = strings_.format(game::kStrLootAllConfirm, std::vector<std::string>{});
    c.timeout_ms = game::kLootConfirmMs;
    ask(std::move(c));
}

void GameConnection::loot_all_now() {
    // CLootWnd::LootAll(1) @0x6bdba0 (game::plan_loot_all): nothing while coin is held; bags first, a no-drop item
    // asked about (a second one drops the whole list); one OP_LootItem at a time, the next on each answer; with
    // nothing to loot and no question the window closes.
    if (!loot_ || !loot_->open) return;
    if (std::any_of(cursor_coins_.begin(), cursor_coins_.end(), [](std::uint32_t c) { return c != 0; })) return;
    if (!loot_->complete) { // before the server's "every item sent" (6): runs when that arrives
        loot_all_deferred_ = true;
        return;
    }
    loot_close_on_complete_ = true; // LootAll(1): the window closes once the last answer is in
    std::vector<game::LootAllItem> items;
    for (const auto& [slot, item] : loot_->items) items.push_back({slot, item.is_bag(), loot_asks_no_drop(item)});
    game::LootAllPlan plan = game::plan_loot_all(items, loot_popup_pending_);
    if (plan.ask) {
        Confirmation c;
        c.id = kConfirmLootNoDrop;
        c.text = game::loot_no_drop_question(loot_->items.at(*plan.ask).name);
        c.timeout_ms = game::kLootConfirmMs;
        c.data = static_cast<std::uint16_t>(*plan.ask);
        c.data2 = 1;
        ask(std::move(c));
    }
    loot_popup_pending_ = plan.popup_pending;
    if (plan.aborted) return;
    if (plan.close) {
        end_loot();
        return;
    }
    if (plan.queue.empty()) return;
    loot_all_queue_.assign(plan.queue.begin() + 1, plan.queue.end());
    send_loot_item(plan.queue.front(), true);
}

void GameConnection::end_loot() {
    if (!loot_) return;
    send_zone(encode_end_loot_request(loot_->corpse_id));
    hide_looted_corpse(); // /hidecorpses looted (net/game_connection_game_commands.cpp)
    // The server answers OP_LootComplete, which closes the session.
}

// ---------------------------------------------------------------- links

void GameConnection::request_item(const game::ItemLink& link) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode(link.click_request()));
}

void GameConnection::request_item_link(std::uint32_t item_id, const std::string& name, std::uint32_t flags, bool shift) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode(ItemLinkRequest{item_id, shift ? flags | 2 : flags, name}));
}

std::optional<Item> GameConnection::take_item_display() {
    if (item_displays_.empty()) return std::nullopt;
    Item it = std::move(item_displays_.front());
    item_displays_.pop_front();
    return it;
}

bool GameConnection::item_command(const std::string& command, const std::string& args) {
    if (command == "/loot") {
        // A server's client may take it first (the NMS patch: its Personal Loot window's /loot).
        if (server_ext().handler_command(*this, "__do_loot", args)) return true;
        // __do_loot @0x4e7880: without a target "You must first target a corpse to loot!" (0x111).
        if (!world_.target_id || !world_.spawns.count(world_.target_id)) system(13201, {}, 0x111);
        else loot(world_.target_id);
        return true;
    }
    if (command == "/destroy") { // this client's own command: the inventory's Destroy button
        destroy_held();
        return true;
    }
    if (command == "/autoinventory" || command == "/autoinv") {
        // cmd_autoinventory @0x4fe8c0: an item still held afterwards (not a Krono, 88888) says "There are no open
        // slots for the held item in your inventory." (0xd); the windows' auto-inventory says nothing.
        const Item* held = inventory_.cursor();
        if (held && !auto_inventory() && held->id != 88888) system(5290, {}, 0xd);
        return true;
    }
    return false;
}

bool GameConnection::consume(const ItemSlot& slot, bool automatic) {
    // Only what the character carries (EQEmu would eat from the bank too).
    if (slot.type != kInvPossessions) return false;
    Item* item = inventory_.at(slot);
    if (state_ != ConnectionState::InZone || !item || pstatus_.dead) return false;
    bool drink = item->item_type == kItemTypeDrink;
    if (item->item_type != kItemTypeFood && !drink) return false;
    // EQEmu answers a full character with OP_Stamina only and eats nothing (Handle_OP_Consume over 6000).
    if ((drink ? spells_.thirst : spells_.hunger) >= 6000) return false;
    send_zone(encode_consume(slot, automatic, drink));
    consume_pending_ = true;
    // EQEmu's Consume deletes one without a client update.
    if (item->stackable() && item->stack_count > 1) --item->stack_count;
    else inventory_.take(slot);
    return true;
}

void GameConnection::auto_consume() {
    if (consume_pending_ || state_ != ConnectionState::InZone) return;
    bool hungry = spells_.hunger < kAutoEatLevel, thirsty = spells_.thirst < kAutoEatLevel;
    if (!hungry && !thirsty) return;
    auto wanted = [&](const Item& it) {
        return (hungry && it.item_type == kItemTypeFood) || (thirsty && it.item_type == kItemTypeDrink);
    };
    for (std::int16_t s = kSlotGeneral1; s <= kSlotGeneral10; ++s) {
        const Item* it = inventory_.at(ItemSlot::possessions(s));
        if (!it) continue;
        if (wanted(*it)) {
            consume(ItemSlot::possessions(s), true);
            return;
        }
        for (const auto& b : it->contents)
            if (wanted(b.item)) {
                consume(ItemSlot::possessions(s, static_cast<std::int16_t>(b.index)), true);
                return;
            }
    }
}

// ---------------------------------------------------------------- kept items (Hailstone; game/kept_items.h)

void GameConnection::load_kept_items() {
    kept_items_.clear();
    ++kept_revision_;
    std::filesystem::path path = own_files().kept_items();
    if (path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::ostringstream text;
    text << in.rdbuf();
    kept_items_.parse(text.str());
}

void GameConnection::set_item_kept(std::uint32_t id, std::string_view name, bool on) {
    set_items_kept({{id, std::string(name)}}, on);
}

void GameConnection::set_items_kept(const std::vector<std::pair<std::uint32_t, std::string>>& items, bool on) {
    std::vector<std::uint32_t> marked; // the items this call marks
    for (const auto& [id, name] : items)
        if (kept_items_.set(id, name, on)) marked.push_back(id);
    if (marked.empty()) return;
    ++kept_revision_;
    if (on) {
        const auto newly = [&](std::uint32_t id) { return std::find(marked.begin(), marked.end(), id) != marked.end(); };
        // An existing server-side offer must stop too: guarding only the next button press leaves it for sale. Only
        // an offered item (listed with the server) ends it: a newly kept satchel item without a price is not for sale.
        if ((trader_.trader || trader_.start_pending) &&
            std::any_of(trader_.slots.begin(), trader_.slots.end(), [&](const auto& slot) {
                if (!slot.active) return false;
                if (newly(slot.item_id)) return true;
                const Item* item = inventory_.at(slot.where);
                return item && std::any_of(item->contents.begin(), item->contents.end(),
                                           [&](const auto& c) { return newly(c.item.id); });
            })) {
            trader_end();
            chat(ChatEvent::Kind::System, "Trader mode stopped because an offered item is now marked Keep.");
        }
        if (barter_window_.buyer_mode &&
            std::any_of(barter_window_.lines.begin(), barter_window_.lines.end(), [&](const auto& line) {
                return line.enabled && line.toggle &&
                    std::any_of(line.trade_items.begin(), line.trade_items.end(), [&](const auto& item) {
                        return kept_items_.kept(item.item_id);
                    });
            })) {
            barter_start_stop();
            chat(ChatEvent::Kind::System, "Buyer mode stopped because a compensation item is now marked Keep.");
        }
    }
    std::filesystem::path path = own_files().kept_items();
    if (path.empty()) return; // no character yet: kept for this session only
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (!write_file_atomic(path, kept_items_.format())) log::warn("cannot write {}", path.string());
}

bool GameConnection::refuse_kept_id(std::uint32_t id, std::string_view verb) {
    if (!kept_items_.kept(id)) return false;
    Item item;
    item.id = id;
    item.name = kept_items_.items().at(id);
    return refuse_kept(&item, verb);
}

bool GameConnection::refuse_kept(const Item* item, std::string_view verb) {
    if (!item || !kept_items_.holds_kept(*item)) return false;
    const bool inside = !kept_items_.kept(item->id); // a bag holding a kept item
    chat(ChatEvent::Kind::System,
         inside ? std::format("Your {} holds an item marked Keep: unmark it in the Advanced Inventory window to {} the bag.",
                              item->name, verb)
                : std::format("Your {} is marked Keep: unmark it in the Advanced Inventory window to {} it.", item->name,
                              verb));
    return true;
}

} // namespace hailstone::net
