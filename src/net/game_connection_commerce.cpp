// GameConnection: merchants, NPC dialogue (hail) and trades with players and
// NPCs (quest hand-ins). Packets in net/merchant_packets.h and
// net/trade_packets.h; prices and money in game/merchant.h. The client side
// follows the original's functions in eqgame.exe
// (names and addresses at each function below and in those headers).
//
// Cursor queue (the hand-in problem OpenEQ's docs/QUEST_HANDIN_PLAN.md
// describes): the server keeps the cursor as a queue and sends the head as
// an item packet of type 0x6a (Limbo) whenever it changes. The original only
// places such an item when the cursor slot is empty
// (ItemPacket_PlaceItemIfSlotEmpty @0x4b9b90) and otherwise drops the
// packet; when the cursor empties again the server sends the next head
// (SendCursorBuffer). game_connection_items.cpp does exactly that, so quest
// rewards and returned items that arrive while the cursor is full show up
// one at a time as the player puts each one down.

#include <algorithm>
#include <format>
#include <glm/geometric.hpp>

#include "core/log.h"
#include "core/distribution.h"
#include "game/item_options.h"
#include "game/merchant.h"
#include "game/world_state.h"
#include "game/trade_rules.h"
#include "net/game_connection.h"
#include "net/market_packets.h"
#include "net/merchant_packets.h"
#include "game/distance.h"
#include "net/opcodes.h"
#include "net/zone_packets.h"
#include "net/trade_packets.h"

namespace hailstone::net {

namespace {

constexpr std::uint8_t kClassMerchant = 41;
// PlayerClient_IsWithinInteractRange(target, 1, 20.0f) in RightClickedOnPlayer.
constexpr float kInteractRange = 20.0f;
// CEverQuest::DropItemOrMoneyOnPlayer refuses these service classes with eqstr 8505:
// 40 banker, 63 ('?'), 64 ('@'), 66 ('B'), 74 ('J').
bool refuses_items(std::uint8_t class_id) {
    return class_id == 40 || class_id == 63 || class_id == 64 || class_id == 66 || class_id == 74;
}

ItemSlot trade_slot(std::int16_t n) { return {kInvTrade, n, -1, -1, 0, 0}; }

} // namespace

// ---------------------------------------------------------------- merchants

void GameConnection::open_merchant(std::uint32_t npc_id) {
    if (state_ != ConnectionState::InZone || pstatus_.dead) return;
    auto it = world_.spawns.find(npc_id);
    const game::Spawn* me = world_.player();
    if (it == world_.spawns.end() || !me || it->second.class_id != kClassMerchant) return;
    // RightClickedOnPlayer: only without an active merchant and within 20 units.
    if (merchant_) return;
    glm::vec3 at = motion_known_ ? position_ : me->position;
    if (glm::distance(at, it->second.position) >= kInteractRange) return;
    // Opening a merchant stops attacking (eqstr 1466 "Auto attack is off.").
    if (auto_attack_) {
        set_auto_attack(false);
        chat(ChatEvent::Kind::System, strings_.format(1466, std::vector<std::string>{}));
    }
    merchant_ = MerchantSession{};
    merchant_->npc_id = npc_id;
    merchant_->name = game::display_name(it->second.name);
    MerchantClick m;
    m.npc_id = npc_id;
    m.player_id = world_.player_id;
    send_zone(encode(m));
}

void GameConnection::update_merchant() {
    // CMerchantWnd's refresh (export CMerchantWnd.c): every 500 ms, without the merchant or out of
    // PlayerClient_IsWithinInteractRange(merchant, 0, 20.0) (CDisplay::PlayerDistance with z scale 1), the
    // window closes unless a buy or sell is still waiting for the server.
    if (!merchant_) return;
    std::int64_t now = now_ms();
    if (merchant_->parcel_pending && now - merchant_->parcel_sent_ms > 5000) { // (Hailstone) no answer: go on
        merchant_->parcel_pending = false;
        next_parcel_claim();
    }
    if (merchant_->parcel_sending && now - merchant_->parcel_send_ms > 5000) merchant_->parcel_sending = false;
    if (now < merchant_check_ms_) return;
    merchant_check_ms_ = now + 500;
    const game::Spawn* me = world_.player();
    auto it = world_.spawns.find(merchant_->npc_id);
    bool in_range = false;
    if (me && it != world_.spawns.end()) {
        glm::vec3 at = motion_known_ ? position_ : me->position;
        in_range = !(game::player_distance(&at, &it->second.position) > kInteractRange);
    }
    if (!in_range && merchant_->pending < 1) {
        log::info("merchant: {} is out of reach, closing", merchant_->name);
        close_merchant();
    }
}

void GameConnection::merchant_buy(std::uint32_t merchant_slot, std::uint32_t quantity) {
    // CMerchantWnd::PurchasePageHandler::RequestGetItem @0x6f5be0.
    if (!merchant_ || !merchant_->open || quantity < 1) return;
    if (merchant_->pending > 0) return; // PendingRequestLog report 0x4b
    auto it = merchant_->items.find(merchant_slot);
    if (it == merchant_->items.end()) return;
    if (inventory_.cursor()) {
        chat(ChatEvent::Kind::System, strings_.format(8548, std::vector<std::string>{})); // not with an item on the cursor
        return;
    }
    const Item& item = it->second;
    // In 64 bits, as handle_trader_purchase: a signed 32-bit product of a large price and quantity overflows
    // (undefined), and wrapped negative it would skip the can-afford check. Whether RequestGetItem's own product wraps
    // is not traced (docs/unknowns.md); a product within 2^31 is the same either way.
    const std::int64_t cost = static_cast<std::int64_t>(item.price) * static_cast<std::int64_t>(quantity);
    if (merchant_->trader) {
        // A player trader: RequestGetItem's own check (12060 / 12061 with the trader's name), then OP_TraderShop 10.
        if (game::total_copper(coins_) < cost) {
            if (quantity == 1)
                chat(ChatEvent::Kind::System, strings_.format(12060, std::vector<std::string>{merchant_->name, item.name}), 0, 275);
            else
                chat(ChatEvent::Kind::System,
                     strings_.format(12061, std::vector<std::string>{merchant_->name, game::money_words(cost),
                                                                     std::to_string(quantity), item.name}),
                     0, 275); // the Merchants filter
            return;
        }
        trader_buy(item, quantity);
        return;
    }
    if (game::total_copper(coins_) < cost) {
        if (quantity == 1)
            chat(ChatEvent::Kind::System, strings_.format(12060, std::vector<std::string>{merchant_->name, item.name}), 0, 275);
        else
            chat(ChatEvent::Kind::System,
                 strings_.format(12061, std::vector<std::string>{merchant_->name, game::money_words(cost),
                                                                 std::to_string(quantity), item.name}),
                 0, 275);
        return;
    }
    MerchantBuy b;
    b.npc_id = merchant_->npc_id;
    b.player_id = world_.player_id;
    b.item_slot = item.instance_id; // the header's instance id carries the merchant slot
    b.unknown12 = item.parcel_item_id;
    b.quantity = quantity;
    send_zone(encode(b));
    ++merchant_->pending;
}

void GameConnection::merchant_select(const ItemSlot& slot) {
    if (!merchant_ || !merchant_->open) return;
    if (!inventory_.at(slot)) return;
    merchant_->selected = slot;
    merchant_->parcel_coin = -1; // CInvSlotMgr::SelectSlot clears the coin buttons' outline (0x690950)
}

void GameConnection::parcel_coin_button(int coin, std::int32_t have) {
    // 0x698290 (83 bytes; SelectSlot(0), then 0x6909a0(coin) and 0x6f2120 when have > 0).
    if (!merchant_ || !merchant_->open || coin < 0 || coin > 3) return;
    merchant_->selected.reset();
    merchant_->parcel_coin = -1;
    if (have > 0) merchant_->parcel_coin = coin;
}

void GameConnection::set_parcel_money(int coin, std::uint32_t amount) {
    if (!merchant_ || !merchant_->open || coin < 0 || coin > 3) return;
    merchant_->parcel_money = MerchantSession::ParcelMoneyPick{coin, amount, ++parcel_money_picks_};
}

void GameConnection::merchant_sell(const ItemSlot& slot, std::uint32_t quantity) {
    // CMerchantWnd::PurchasePageHandler::RequestPutItem @0x6f6650.
    if (!merchant_ || !merchant_->open || merchant_->trader || quantity < 1 || slot.type != kInvPossessions) return;
    const Item* item = inventory_.at(slot);
    if (!item) return;
    if (refuse_kept(item, "sell")) return; // Hailstone: game/kept_items.h
    if (merchant_->pending > 0) {
        chat(ChatEvent::Kind::System, strings_.format(12063, std::vector<std::string>{merchant_->name})); // one thing at a time
        return;
    }
    if (std::uint32_t refusal = game::sell_refusal(*item)) {
        chat(ChatEvent::Kind::System, strings_.format(refusal, std::vector<std::string>{merchant_->name}));
        return;
    }
    MerchantSell s;
    s.npc_id = merchant_->npc_id;
    s.slot = slot.slot;
    s.sub = slot.sub;
    s.aug = slot.aug;
    s.quantity = item->stackable() ? std::min(quantity, item->stack_count) : 1;
    send_zone(encode(s));
    ++merchant_->pending;
}

void GameConnection::retrieve_parcel(std::uint32_t slot) {
    if (state_ != ConnectionState::InZone || !merchant_ || !merchant_->open || merchant_->parcel_pending) return;
    auto it = merchant_->parcels.find(slot);
    if (it == merchant_->parcels.end()) return;
    ParcelRetrieve r;
    r.merchant_id = merchant_->npc_id;
    r.player_id = world_.player_id;
    r.parcel_slot = slot;
    r.item_id = it->second.item.id;
    send_zone(encode(r));
    merchant_->parcel_pending = true;
    merchant_->parcel_sent_ms = now_ms();
}

std::string GameConnection::send_parcel(const ItemSlot& slot, std::uint32_t quantity, const std::string& to,
                                        const std::string& note) {
    // MerchantMailPageHandler_RequestPutItem @0x6f6a30: a recipient (eqstr 6462), nothing pending, fewer than 50
    // parcels here (733), a count of at least one; then the packet, one request pending until the answer.
    if (auto why = parcel_send_blocked(to); !why.empty()) return why;
    const Item* item = inventory_.at(slot);
    if (slot.type != kInvPossessions || !item || slot.slot == kSlotCursor || (slot.sub < 0 && slot.slot < kSlotGeneral1))
        return "Pick an item from your bags to send.";
    if (item->no_trade()) return "That item cannot be sent.";
    if (kept_items_.holds_kept(*item)) return "That item is marked Keep: unmark it in the Advanced Inventory window first.";
    ParcelSend s;
    s.npc_id = merchant_->npc_id;
    s.slot = slot.slot;
    s.sub = slot.sub;
    s.quantity = item->stackable() ? std::clamp<std::uint32_t>(quantity, 1, item->stack_count) : 1;
    s.money_flag = 0;
    s.to = to;
    s.note = note;
    send_zone(encode(s));
    merchant_->parcel_sending = true;
    merchant_->parcel_send_ms = now_ms();
    return {};
}

std::string GameConnection::send_parcel_money(const std::array<std::uint32_t, 4>& coins, const std::string& to,
                                              const std::string& note) {
    // The money page: the coin goes to the trade location (EQEmu keeps it as the parcel's, m_parcel_*), then the
    // parcel with money_flag 1 and its copper value. A refused parcel gives the coin back (DoParcelCancel).
    if (auto why = parcel_send_blocked(to); !why.empty()) return why;
    const std::array<std::uint32_t, 4> have{coins_.platinum, coins_.gold, coins_.silver, coins_.copper};
    std::uint64_t copper = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        if (coins[i] > have[i]) return "You do not have that much money.";
        copper += static_cast<std::uint64_t>(coins[i]) * (i == 0 ? 1000 : i == 1 ? 100 : i == 2 ? 10 : 1);
    }
    if (copper == 0) return "Put in some money to send.";
    if (copper > 0x7fffffffu) return "That is too much to send at once.";
    for (std::size_t i = 0; i < 4; ++i) {
        if (!coins[i]) continue;
        const int type = 3 - static_cast<int>(i); // wire: 0 copper .. 3 platinum
        send_zone(encode(MoveCoin{kCoinInventory, kCoinTrade, type, type, static_cast<std::int32_t>(coins[i])}));
    }
    ParcelSend s;
    s.npc_id = merchant_->npc_id;
    s.quantity = static_cast<std::uint32_t>(copper);
    s.money_flag = 1;
    s.to = to;
    s.note = note;
    send_zone(encode(s));
    merchant_->parcel_sending = true;
    merchant_->parcel_send_ms = now_ms();
    merchant_->parcel_coin = -1; // the money has gone: no coin button stays outlined for it
    return {};
}

std::string GameConnection::parcel_send_blocked(const std::string& to) const {
    if (state_ != ConnectionState::InZone || !merchant_ || !merchant_->open || !(merchant_->tabs & 4))
        return "Open a parcel merchant first.";
    if (to.empty()) return "Who is it for? Type the recipient's name.";
    if (merchant_->parcel_sending || merchant_->parcel_pending || merchant_->pending) return "Wait for the merchant.";
    if (merchant_->parcels.size() >= 50)
        return "Retrieve " + std::to_string(merchant_->parcels.size() - 49) + " parcels here first.";
    if (inventory_.cursor()) return "Put down what you are holding first.";
    return {};
}

void GameConnection::claim_all_parcels() {
    if (distribution::managed()) {
        if (merchant_) merchant_->claiming.clear();
        return;
    }
    if (!merchant_ || !merchant_->open) return;
    merchant_->claiming.clear();
    for (const auto& [slot, parcel] : merchant_->parcels) merchant_->claiming.push_back(slot);
    next_parcel_claim();
}

void GameConnection::next_parcel_claim() {
    if (distribution::managed()) {
        if (merchant_) merchant_->claiming.clear();
        return;
    }
    if (!merchant_ || merchant_->parcel_pending) return;
    while (!merchant_->claiming.empty()) {
        std::uint32_t slot = merchant_->claiming.front();
        merchant_->claiming.erase(merchant_->claiming.begin());
        if (merchant_->parcels.count(slot)) {
            retrieve_parcel(slot);
            return;
        }
    }
}

void GameConnection::close_merchant() {
    // CMerchantWnd::AboutToHide: OP_ShopEnd {merchant, me}; the window goes now.
    if (!merchant_) return;
    // At a player trader the window sends OP_TraderShop {4} instead.
    if (state_ == ConnectionState::InZone)
        send_zone(merchant_->trader ? encode_trader_shop_end() : encode_shop_end(merchant_->npc_id, world_.player_id));
    merchant_.reset();
}

// ---------------------------------------------------------------- dialogue

void GameConnection::hail() {
    // Hail @0x4f3010: eqstr 13084 "Hail", or 13085 "Hail, %1" with the
    // target's trimmed name, said in /say (CEverQuest::send_chat).
    if (state_ != ConnectionState::InZone) return;
    std::string text;
    auto it = world_.spawns.find(world_.target_id);
    if (world_.target_id && it != world_.spawns.end())
        text = strings_.format(13085, std::vector<std::string>{game::display_name(it->second.name)});
    else
        text = strings_.format(13084, std::vector<std::string>{});
    say(text);
}

// ---------------------------------------------------------------- trade

void GameConnection::give_to(std::uint32_t spawn_id) {
    // CEverQuest::DropItemOrMoneyOnPlayer @0x5331f0.
    if (state_ != ConnectionState::InZone || pstatus_.dead) return;
    auto it = world_.spawns.find(spawn_id);
    const game::Spawn* me = world_.player();
    if (it == world_.spawns.end() || !me || it->second.is_corpse) return;
    const game::Spawn& sp = it->second;
    if (trade_ && trade_->open) {
        if (spawn_id == trade_->partner_id && inventory_.cursor()) trade_drop_cursor();
        return;
    }
    if (spawn_id == world_.player_id || trade_) return;
    if (sp.is_npc && refuses_items(sp.class_id)) {
        chat(ChatEvent::Kind::System, strings_.format(8505, std::vector<std::string>{}));
        return;
    }
    // Within the partner's AvatarHeight + 20 (eqstr 12294).
    glm::vec3 at = motion_known_ ? position_ : me->position;
    if (glm::distance(at, sp.position) > sp.heights.avatar_height + 20.0f) {
        chat(ChatEvent::Kind::System, strings_.format(12294, std::vector<std::string>{game::display_name(sp.name)}));
        return;
    }
    // A player partner needs a tradeable cursor item (CanDrop).
    if (!sp.is_npc) {
        if (const Item* held = inventory_.cursor(); held && !game::is_droppable(*held)) {
            chat(ChatEvent::Kind::System, strings_.format(12921, std::vector<std::string>{held->name}));
            return;
        }
    }
    trade_ = TradeSession{};
    trade_->partner_id = spawn_id;
    trade_->partner_name = game::display_name(sp.name);
    trade_->with_npc = sp.is_npc;
    trade_->requested = true;
    send_zone(encode_trade_request({spawn_id, world_.player_id}));
}

void GameConnection::trade_drop_cursor() {
    // CTradeWnd::DropItemIntoTrade @0x77f6d0: 4 slots for an NPC, 8 for a
    // player; a stack merges into a matching trade stack first.
    if (!trade_ || !trade_->open) return;
    Item* held = inventory_.at(ItemSlot::possessions(kSlotCursor));
    if (!held) return;
    if (refuse_kept(held, trade_->with_npc ? "give" : "trade")) return; // Hailstone: game/kept_items.h
    if (!trade_->with_npc && !game::is_droppable(*held)) {
        chat(ChatEvent::Kind::System, strings_.format(12921, std::vector<std::string>{held->name}));
        return;
    }
    for (std::int16_t n = 0; n < trade_->slots() && held->stackable(); ++n) {
        Item* there = inventory_.at(trade_slot(n));
        if (!there || !game::can_stack_with(*held, *there) || there->stack_count >= there->max_stack) continue;
        std::uint32_t moved = std::min(there->max_stack - there->stack_count, held->stack_count);
        there->stack_count += moved;
        held->stack_count -= moved;
        MoveItem m;
        m.from = ItemSlot::possessions(kSlotCursor);
        m.to = trade_slot(n);
        m.count = moved;
        send_zone(encode(m));
        if (held->stack_count == 0) inventory_.take(ItemSlot::possessions(kSlotCursor));
        trade_->my_ready = trade_->their_ready = false;
        return;
    }
    for (std::int16_t n = 0; n < static_cast<std::int16_t>(trade_->slots()); ++n) {
        if (inventory_.at(trade_slot(n))) continue;
        move_item(ItemSlot::possessions(kSlotCursor), trade_slot(n), 0);
        trade_->my_ready = trade_->their_ready = false;
        return;
    }
    chat(ChatEvent::Kind::System, strings_.format(12319, std::vector<std::string>{})); // All your trade slots are full!
}

void GameConnection::trade_coin(int type, std::uint32_t amount) {
    // The original picks the coin up (MoveMoney inventory → cursor) and drops
    // it on the trade (CTradeWnd_DropCursorMoneyIntoTrade: MoveMoney cursor → trade).
    if (!trade_ || !trade_->open || type < 0 || type > 3 || amount == 0) return;
    std::uint32_t* have = type == 0 ? &coins_.copper : type == 1 ? &coins_.silver : type == 2 ? &coins_.gold : &coins_.platinum;
    if (amount > *have || amount > 0x7fffffffu) return;
    auto n = static_cast<std::int32_t>(amount);
    send_zone(encode(MoveCoin{kCoinInventory, kCoinCursor, type, type, n}));
    send_zone(encode(MoveCoin{kCoinCursor, kCoinTrade, type, type, n}));
    *have -= amount;
    trade_->my_coins[static_cast<std::size_t>(type)] += amount;
    trade_->my_ready = trade_->their_ready = false;
}

void GameConnection::trade_accept() {
    // Send_OP_TradeAcceptClick @0x77f490 (the Trade button; CGiveWnd's Give
    // button sends the same after warning about augmented items).
    if (!trade_ || !trade_->open) return;
    trade_->my_ready = true;
    send_zone(encode(TradeAccept{trade_->partner_id, 1, 0, 0, 0}));
}

void GameConnection::trade_cancel() {
    // CTradeWnd_CancelAndClose → Send_OP_CancelTrade_00780550: {partner, me}.
    if (!trade_) return;
    if (state_ == ConnectionState::InZone) send_zone(encode(CancelTrade{trade_->partner_id, world_.player_id}));
    close_trade(false);
}

void GameConnection::close_trade(bool completed) {
    // CompleteTrade / the cancel path: the trade window's own items go; the
    // server sends what we keep or receive back as item packets (0x67, or
    // 0x6a onto the cursor) and the coin as OP_MoneyUpdate.
    for (std::int16_t n = 0; n < 8; ++n) inventory_.take(trade_slot(n));
    (void)completed;
    trade_.reset();
}

void GameConnection::reset_commerce_for_zone() {
    merchant_.reset();
    bank_.reset();
    quantity_.reset();
    point_merchant_ = {};
    adventure_merchant_ = {};
    if (trade_) close_trade(false);
}

// ---------------------------------------------------------------- packets

void GameConnection::commerce_item(std::uint32_t packet_type, Item item) {
    if (packet_type == kItemPacketMerchant) {
        // CMerchantWnd's list: the header slot (type 9) is the merchant slot.
        // A trader's items carry no usable slot (EQEmu sends non-stackables at
        // the cursor): they are numbered as they arrive, a known GUID updates.
        if (merchant_ && merchant_->trader) {
            for (auto& [n, it] : merchant_->items)
                if (it.serial == item.serial) {
                    it = std::move(item);
                    return;
                }
            std::uint32_t n = merchant_->items.empty() ? 1 : merchant_->items.rbegin()->first + 1;
            merchant_->items[n] = std::move(item);
            return;
        }
        if (merchant_) merchant_->items[static_cast<std::uint32_t>(item.slot.slot)] = std::move(item);
        return;
    }
    // CTradeWnd_AddItemToHisTradeArray @0x780320: the partner's slot 0..7; the
    // same item again only updates the stack count; either way both ready
    // flags drop.
    if (!trade_ || !trade_->open || item.id == 0 || item.slot.slot < 0 || item.slot.slot > 7 || item.slot.sub != -1) return;
    auto at = trade_->theirs.find(item.slot.slot);
    if (at != trade_->theirs.end() && at->second.id == item.id) {
        at->second.stack_count = item.stack_count;
    } else {
        chat(ChatEvent::Kind::System, strings_.format(9028, std::vector<std::string>{trade_->partner_name, item.name}));
        trade_->theirs[item.slot.slot] = std::move(item);
    }
    trade_->my_ready = trade_->their_ready = false;
}

void GameConnection::handle_commerce(Op op, const AppPacket& p) {
    auto say_str = [&](std::uint32_t id, std::vector<std::string> args = {}) {
        chat(ChatEvent::Kind::System, strings_.format(id, args));
    };
    auto spawn_name = [&](std::uint32_t id) -> std::string {
        auto it = world_.spawns.find(id);
        return it == world_.spawns.end() ? std::string() : game::display_name(it->second.name);
    };
    switch (op) {
    case Op::AltCurrency:
        if (auto a = decode_alt_currency(p)) {
            for (const auto& e : a->populate) alt_currencies_[e.id].entry = e;
            if (a->update) alt_currencies_[a->update->id].amount = a->update->amount;
        }
        break;
    case Op::AltCurrencyMerchantReply:
        if (auto l = decode_point_merchant_list(p); l && point_merchant_.npc) point_merchant_.list = std::move(*l);
        break;
    case Op::AltCurrencySellSelection:
        if (auto q = decode_alt_currency_quote(p); q && point_merchant_.npc) point_merchant_.quote = std::move(*q);
        break;
    case Op::AltCurrencySell:
        if (auto s = decode_alt_currency_sale(p)) point_sale(*s);
        break;
    case Op::AdventureMerchantResponse:
        if (auto l = decode_adventure_merchant_list(p); l && adventure_merchant_.npc) adventure_merchant_.items = std::move(*l);
        break;
    case Op::AdventureMerchantSell:
        if (auto s = decode_adventure_merchant_sale(p)) point_sale(*s);
        break;
    case Op::ShopRequest:
        // Handle_OP_ShopRequest @0x4b75f0: command 0 closes, else CMerchantWnd::Activate(rate, tabs).
        if (auto m = decode_merchant_click(p)) {
            if (!merchant_ || merchant_->npc_id != m->npc_id) break;
            if (m->command == 0) {
                merchant_.reset();
                break;
            }
            merchant_->open = true;
            merchant_->rate = m->rate;
            merchant_->tabs = m->tab_display;
            log::info("merchant {} open: rate {}, tabs {}", merchant_->name, m->rate, m->tab_display);
        }
        break;
    case Op::ShopPlayerBuy:
        // FinishBuyingItem @0x6ef220: on success (the 64-bit slot field > 0)
        // take the price (TakeMoney; all coin zeroed if it fails) and print 1437.
        if (auto b = decode_merchant_buy(p)) {
            if (merchant_ && merchant_->pending > 0) --merchant_->pending;
            std::int64_t ok = static_cast<std::int64_t>(b->item_slot) | (static_cast<std::int64_t>(b->unknown12) << 32);
            if (ok <= 0) break;
            std::int64_t price = static_cast<std::int64_t>(b->price) | (static_cast<std::int64_t>(b->unknown28) << 32);
            if (!game::take_money(coins_, price)) coins_ = {};
            say_str(1437, {game::money_words(price), merchant_ ? merchant_->name : std::string("the merchant")});
        }
        break;
    case Op::ShopPlayerSell:
        // FinishSellingItem @0x6f4200: slot -1 = the merchant refused (1364);
        // else 1361, the price is credited (CreditCopper @0x4488c0) and the
        // sold count leaves the stack (a whole item goes).
        if (auto s = decode_merchant_sell(p)) {
            if (merchant_ && merchant_->pending > 0) --merchant_->pending;
            std::string who = merchant_ ? merchant_->name : strings_.format(1365, std::vector<std::string>{});
            if (s->slot == -1) {
                say_str(1364, {who});
                break;
            }
            ItemSlot at{kInvPossessions, s->slot, s->sub, -1, 0, 0};
            Item* item = inventory_.at(at);
            if (!item) break;
            say_str(1361, {game::money_words(s->price), who, item->name});
            game::credit_copper(coins_, static_cast<std::int64_t>(s->price));
            if (item->stackable() && s->quantity < item->stack_count) {
                item->stack_count -= s->quantity;
            } else {
                inventory_.take(at);
                if (merchant_ && merchant_->selected && merchant_->selected->same(at)) merchant_->selected.reset();
                if (at.sub < 0 && at.slot < kEquipmentSlots) refresh_worn_slot(at.slot);
            }
        }
        break;
    case Op::ShopEnd:
    case Op::ShopEndConfirm:
        merchant_.reset();
        break;
    case Op::ShopDelItem:
        if (auto d = decode_merchant_del_item(p); d && merchant_) merchant_->items.erase(d->item_slot);
        break;
    case Op::ShopDeleteParcel:
        // a parcel taken (or gone): off the list
        if (auto d = decode_parcel_retrieve(p); d && merchant_) merchant_->parcels.erase(d->parcel_slot);
        break;
    case Op::ShopSendParcel:
        // CMerchantWnd::HandleSendParcelReply @0x6f34f0: the answer (slot -1; an empty one comes first for a parcel
        // sent) ends the request. The item left by the server's own delete; the coin by its money update.
        if (merchant_ && p.data.size() >= 8) merchant_->parcel_sending = false;
        break;
    case Op::ShopRetrieveParcel:
        // the answer to a retrieve, taken or not: Claim all asks for the next one
        if (merchant_) {
            merchant_->parcel_pending = false;
            next_parcel_claim();
        }
        break;
    case Op::TradeRequest:
        // Handle_OP_TradeRequest @0x4c23c0 (game/trade_rules.h): the Trade setting, the group-only check,
        // IsOkToTransact and auto attack; a refusal answers OP_TradeBusy {requester, us, reason} and says 1447.
        // Accepted, CTradeWnd::Activate(partner, 0) opens the window and acknowledges.
        if (auto r = decode_trade_request(p)) {
            std::string name = spawn_name(r->from_id);
            game::TradeRequestInput in;
            in.requester_id = r->from_id;
            in.requester_spawn = !name.empty();
            in.my_spawn_id = world_.player_id;
            in.setting = client_options_.get(game::ClientOption::TradeWith); // Options Trade (0xde0bb0)
            in.pc = profile_.has_value();
            in.group = group_.active;
            in.requester_in_group = group_.has_member(game::display_name(name)) || group_.has_member(name);
            in.ok_to_transact = ok_to_transact();
            in.attacking = auto_attack_;
            game::TradeRequestResult res = game::handle_trade_request(in);
            if (res.refusal) {
                log::info("trade: {}'s request refused (reason {})", name, res.refusal->reason);
                send_zone(encode(TradeBusy{r->from_id, world_.player_id, res.refusal->reason, 0, 0, 0}));
                say_str(game::kStrTradeRefused, {name});
                break;
            }
            if (!res.open_trade) break;
            trade_ = TradeSession{};
            trade_->partner_id = r->from_id;
            trade_->partner_name = name;
            trade_->open = true;
            send_zone(encode_trade_request_ack({r->from_id, world_.player_id}));
        }
        break;
    case Op::TradeRequestAck:
        // Handle_OP_TradeRequestAck @0x4c2070: with IsOkToTransact, CTradeWnd::Activate(partner, 1): the trade (or the
        // give window for an NPC) opens with the cursor item in it. Otherwise (PendingRequestLog_SendReport(0x3f)) a
        // player partner is told "<me> tells you, 'I'm busy right now.'" (1446 as a tell, language skill 100), 1447
        // "<partner> is interested in making a trade." is said (0x111), and OP_CancelTrade {partner, me} goes.
        if (auto r = decode_trade_request(p)) {
            auto it = world_.spawns.find(r->from_id);
            if (it == world_.spawns.end()) break;
            if (!trade_ || trade_->partner_id != r->from_id) break;
            if (!ok_to_transact()) {
                log::info("trade: the answer from {} came while busy", it->second.name);
                std::string partner = it->second.name;
                if (!it->second.is_npc) {
                    ChannelMessage m;
                    m.sender = character_;
                    m.target = partner;
                    m.message = strings_.format(1446, std::vector<std::string>{character_});
                    m.channel = 7;
                    m.language = chat_language_;
                    m.language_skill = 100;
                    send_zone(encode(m));
                }
                system(game::kStrTradeRefused, {game::display_name(partner)}, 0x111);
                send_zone(encode(CancelTrade{r->from_id, world_.player_id}));
                trade_.reset();
                break;
            }
            trade_->requested = false;
            trade_->open = true;
            if (inventory_.cursor()) trade_drop_cursor();
        }
        break;
    case Op::TradeAcceptClick:
        // CTradeWnd::SetHisReadyTrade (game/trade_rules.h): the partner un-readying clears ours too.
        if (auto a = decode_trade_accept(p); a && trade_) {
            game::TradeFlags f{trade_->their_ready, trade_->my_ready};
            game::trade_set_his_ready(f, a->ready != 0 ? 1 : 0);
            trade_->their_ready = f.his_ready != 0;
            trade_->my_ready = f.my_ready != 0;
        }
        break;
    case Op::TradeCoins:
        // Handle_OP_TradeCoins @0x4b76d0: the partner added coin (a delta).
        if (auto c = decode_trade_coins(p); c && trade_ && c->type < 4) {
            trade_->their_coins[c->type] += c->amount;
            trade_->my_ready = trade_->their_ready = false;
        }
        break;
    case Op::TradeBusy:
        // Handle_OP_TradeBusy @0x4b7800: 'b' 1444, 'c' 1445, else 1446.
        if (auto b = decode_trade_busy(p)) {
            std::string name = spawn_name(b->from_id);
            if (!name.empty()) say_str(b->type == 'b' ? 1444 : b->type == 'c' ? 1445 : 1446, {name});
            if (trade_ && trade_->partner_id == b->from_id && !trade_->open) trade_.reset();
        }
        break;
    case Op::CancelTrade:
        // Handle_OP_CancelTrade @0x4b7710: "<name> has cancelled the trade."
        // (1448; 1450 for our own id), then CancelAndClose, which sends our
        // own cancel back while a partner is still set.
        if (auto c = decode_cancel_trade(p)) {
            if (!trade_) break;
            if (c->from_id == world_.player_id) say_str(1450);
            else if (std::string n = spawn_name(c->from_id); !n.empty()) say_str(1448, {n});
            else say_str(1449);
            trade_cancel();
        }
        break;
    case Op::FinishTrade:
        // Handle_OP_FinishTrade @0x4b78b0 → CTradeWnd::CompleteTrade.
        if (trade_) close_trade(true);
        break;
    case Op::FinishWindow:
    case Op::FinishWindow2:
        break;
    default:
        break;
    }
}

bool GameConnection::commerce_command(const std::string& command, const std::string& args) {
    (void)args;
    if (command == "/hail") {
        hail();
        return true;
    }
    return false;
}

// CEverQuest::IsOkToTransact @0x51c970: the client has no vehicles, no 0xddf8a0 and no item request counter
// (EverQuestinfo__IsItemPending: 0); __CurrentlyInteracting counts the interaction windows the client keeps
// (trade, merchant, bank, loot, trainer), as their Activate sets it.
game::TransactState GameConnection::transact_state() const {
    game::TransactState s;
    s.respawn_timer = respawn_ ? 1u : 0u;
    // A trade counts once its window is open (CTradeWnd::Activate); a request still waiting for its answer does not.
    const bool trading = trade_ && trade_->open;
    s.currently_interacting = (trading ? 1 : 0) + (merchant_ ? 1 : 0) + (bank_ ? 1 : 0) + (loot_ ? 1 : 0) +
                              (training_ ? 1 : 0) + (poison_interacting_ ? 1 : 0);
    // EverQuestinfo::IsItemPending: the client's item requests awaiting their answers (item clicks, casts,
    // memorizing, augments: spells_.pending; the guild bank's).
    s.item_pending = spells_.pending + guild_bank_.pending;
    s.container_open = world_container_ ? 1u : 0u;
    auto set = [&](game::TransactWindow w, bool active) { s.windows[w] = active; };
    set(game::kTransactSpellbook, spellbook_open_ || spell_book_open_);
    set(game::kTransactTrain, training_.has_value());
    set(game::kTransactBazaar, trader_.window_open);
    set(game::kTransactBarter, barter_window_.open);
    set(game::kTransactTrade, trading);
    set(game::kTransactBank, bank_.has_value());
    set(game::kTransactMerchant, merchant_.has_value());
    set(game::kTransactLoot, loot_.has_value());
    set(game::kTransactBarterMerchant, barter_merchant_.buyer_id != 0);
    set(game::kTransactGuildBank, false);
    return s;
}

bool GameConnection::ok_to_transact() const { return game::is_ok_to_transact(transact_state()); }

// ---------------------------------------------------------------- alternate currencies and point merchants

void GameConnection::alt_currency_reclaim(std::uint32_t currency, std::uint32_t count, bool put_in) {
    if (state_ != ConnectionState::InZone || (!put_in && count == 0)) return;
    send_zone(encode_alt_currency_reclaim(currency, count, put_in));
}

bool GameConnection::open_point_merchant(std::uint32_t npc) {
    // A class 70 NPC within reach; the server answers with the goods (EQEmu Handle_OP_AltCurrencyMerchantRequest).
    auto it = world_.spawns.find(npc);
    const game::Spawn* me = world_.player();
    if (state_ != ConnectionState::InZone || it == world_.spawns.end() || !me || it->second.class_id != 70) return false;
    glm::vec3 at = motion_known_ ? position_ : me->position;
    if (glm::distance(at, it->second.position) >= kInteractRange) return false;
    point_merchant_ = {};
    point_merchant_.npc = npc;
    send_zone(encode_alt_currency_merchant_request(npc));
    return true;
}

void GameConnection::point_merchant_buy(std::uint32_t item_id) {
    if (state_ != ConnectionState::InZone || !point_merchant_.npc) return;
    send_zone(encode_alt_currency_purchase(point_merchant_.npc, item_id));
}

void GameConnection::point_merchant_purchase(std::uint32_t item_id) {
    // CNewPointMerchantWnd_ConfirmPurchase(1) @0x6fce90: at once with pointMerchantNoConfirm (0xde0d08), else
    // ConfirmationDialog(100, 8064: the cost, the currency's name (singular for a cost of 1, else plural) and the
    // item's, no time limit). The currencies' names are not known here (app/lua_eq_items.cpp: "Currency <n>").
    if (state_ != ConnectionState::InZone || !point_merchant_.npc) return;
    if (client_option(game::ClientOption::PointMerchantNoConfirm)) {
        point_merchant_buy(item_id);
        return;
    }
    const PointMerchantItem* item = nullptr;
    for (const auto& i : point_merchant_.list.items)
        if (i.item_id == item_id) item = &i;
    if (!item) return;
    Confirmation c;
    c.id = kConfirmPointBuy;
    c.text = strings_.format(game::kStrAltPurchaseOne,
                             std::vector<std::string>{std::to_string(item->cost), std::format("Currency {}", point_merchant_.list.currency), item->name});
    c.data = item_id;
    ask(std::move(c));
}

void GameConnection::point_merchant_quote(const ItemSlot& slot) {
    if (state_ != ConnectionState::InZone || !point_merchant_.npc || slot.type != kInvPossessions) return;
    point_merchant_.quote.reset();
    point_merchant_.quoted = slot;
    send_zone(encode_alt_currency_select(point_merchant_.npc, slot.slot, slot.sub));
}

void GameConnection::point_merchant_sell() {
    auto& m = point_merchant_;
    if (state_ != ConnectionState::InZone || !m.npc || !m.quote || m.quote->cost == 0) return;
    const Item* item = inventory_.at(m.quoted);
    if (!item || refuse_kept(item, "sell")) return; // Hailstone: game/kept_items.h
    send_zone(encode_alt_currency_sell(m.npc, m.quoted.slot, m.quoted.sub, item->stackable() ? item->stack_count : 1,
                                       m.quote->cost));
    m.quote.reset();
}

void GameConnection::point_sale(const PointSale& s) {
    // EQEmu deletes the sold charges (DeleteItemInInventory without a client update) and echoes the sale.
    ItemSlot at{kInvPossessions, s.slot, s.sub, -1, 0, 0};
    Item* item = inventory_.at(at);
    if (!item) return;
    if (item->stackable() && s.charges > 0 && s.charges < static_cast<std::uint32_t>(item->stack_count)) {
        item->stack_count -= static_cast<decltype(item->stack_count)>(s.charges);
    } else {
        inventory_.take(at);
        if (at.sub < 0 && at.slot < kEquipmentSlots) refresh_worn_slot(at.slot);
    }
}

std::uint32_t GameConnection::adventure_merchant_type(std::uint8_t class_id) {
    switch (class_id) {
    case 61: return 1; // LDoN
    case 59: return 2; // Discord
    case 67: return 4; // Norrath's Keepers
    case 68: return 5; // Dark Reign
    default: return 0;
    }
}

bool GameConnection::open_adventure_merchant(std::uint32_t npc) {
    auto it = world_.spawns.find(npc);
    const game::Spawn* me = world_.player();
    if (state_ != ConnectionState::InZone || it == world_.spawns.end() || !me || !it->second.is_npc) return false;
    std::uint32_t type = adventure_merchant_type(it->second.class_id);
    if (!type) return false;
    glm::vec3 at = motion_known_ ? position_ : me->position;
    if (glm::distance(at, it->second.position) >= kInteractRange) return false;
    adventure_merchant_ = {};
    adventure_merchant_.npc = npc;
    adventure_merchant_.type = type;
    send_zone(encode_adventure_merchant_request(type, npc));
    return true;
}

void GameConnection::adventure_merchant_buy(std::uint32_t item_id) {
    const auto& m = adventure_merchant_;
    if (state_ == ConnectionState::InZone && m.npc) send_zone(encode_adventure_merchant_purchase(m.type, m.npc, item_id));
}

void GameConnection::adventure_merchant_purchase(std::uint32_t item_id) {
    // CPointMerchantWnd_ConfirmPurchase(1) @0x71bba0: Send_OP_AdventureMerchantPurchase at once with
    // pointMerchantNoConfirm (0xde0d08), else ConfirmationDialog(100, the point system's question: the cost "%d" and
    // the item's name, no time limit).
    const auto& m = adventure_merchant_;
    if (state_ != ConnectionState::InZone || !m.npc) return;
    if (client_option(game::ClientOption::PointMerchantNoConfirm)) {
        adventure_merchant_buy(item_id);
        return;
    }
    const AdventureMerchantItem* item = nullptr;
    for (const auto& i : m.items)
        if (i.item_id == item_id) item = &i;
    if (!item) return;
    std::uint32_t question = game::adventure_purchase_question(m.type, 1, static_cast<std::int32_t>(item->cost));
    if (!question) { // a point system this client does not know
        adventure_merchant_buy(item_id);
        return;
    }
    Confirmation c;
    c.id = kConfirmAdventureBuy;
    c.text = strings_.format(question, std::vector<std::string>{std::to_string(item->cost), item->name});
    c.data = item_id;
    ask(std::move(c));
}

bool GameConnection::adventure_merchant_sell(const ItemSlot& slot) {
    const auto& m = adventure_merchant_;
    const Item* item = inventory_.at(slot);
    // EQEmu takes no sales from Discord merchants (nor does the client).
    if (state_ != ConnectionState::InZone || !m.npc || !item || m.type == 2) return false;
    if (refuse_kept(item, "sell")) return false; // Hailstone: game/kept_items.h
    if (!adventure_sell_price(item->ldon_price, item->ldon_sellback_rate, item->ldon_sold)) return false;
    send_zone(encode_adventure_merchant_sell(m.npc, slot.slot, slot.sub, item->stackable() ? item->stack_count : 1));
    return true;
}

} // namespace hailstone::net
