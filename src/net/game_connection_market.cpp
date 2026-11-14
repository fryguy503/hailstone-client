// GameConnection: the bazaar. Trader mode (CBazaarWnd), shopping at a
// trader (CMerchantWnd in trader mode, OP_TraderShop) and the bazaar search
// (CBazaarSearchWnd, CBazaarConfirmationWnd). Packets in
// net/market_packets.h, state and rules in game/bazaar.h; each function names
// the original it follows (in eqgame.exe).

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>

#include "core/character_files.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "game/bazaar.h"
#include "game/distance.h"
#include "game/feature_restrictions.h"
#include "game/merchant.h"
#include "game/world_state.h"
#include "net/game_connection.h"
#include "net/market_packets.h"
#include "net/opcodes.h"

namespace hailstone::net {

namespace {

constexpr float kInteractRange = 20.0f; // PlayerClient_IsWithinInteractRange(.., 1, 20.0f)

std::string eq(const game::EqStrings& s, std::uint32_t id, std::vector<std::string> args = {}) {
    return s.format(id, args);
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// The selected trader slot, or none when the selection is unset or no longer names a slot (the slots were cleared by
// a zone change or reloaded shorter).
game::TraderSlot* selected_slot(game::TraderState& t) {
    if (t.selected < 0 || static_cast<std::size_t>(t.selected) >= t.slots.size()) return nullptr;
    return &t.slots[static_cast<std::size_t>(t.selected)];
}

} // namespace

// ---------------------------------------------------------------- the trader window

void GameConnection::bazaar_text(std::uint32_t color, std::string text) {
    // CBazaarWnd::AddBazaarText @0x6377f0 (it also writes the bazaar log file).
    log::debug("bazaar: {}", text);
    trader_.log.push_back({color, std::move(text)});
    if (trader_.log.size() > 200) trader_.log.erase(trader_.log.begin());
}

bool GameConnection::open_trader_window() {
    // Cmd_trader @0x4eab40 → CBazaarWnd::AboutToShow @0x63c2a0: GetGameFeature(0xe) == 0 prints
    // GameFeatureRestrictions_FormatMessage(0xe) in colour 0x14f and refuses; else only in the bazaar (and while no
    // trade is pending).
    if (state_ != ConnectionState::InZone) return false;
    if (!trader_.window_open && feature_refused(game::kFeatureBazaarSell)) return false;
    if (!game::is_bazaar_zone(zone_id())) return false;
    if (trader_.window_open) return true;
    trader_.window_open = true;
    if (!trader_.trader) {
        // sub_0063a5b0: the slots and prices reset, the INI is (re)named.
        trader_prices_.load(own_files().trader_prices()); // the original's BZR_<name>_<server>.ini
        trader_.slots.clear();
        trader_.selected = -1;
        trader_.entered_price = 0;
        bazaar_text(10, eq(strings_, 6750)); // Welcome to your Bazaar Manager Window.
        load_trader_slots();
        if (trader_.start_pending) {
            trader_.start_pending = false;
            trader_start();
        }
    }
    return true;
}

void GameConnection::close_trader_window() { trader_.window_open = false; }

void GameConnection::load_trader_slots() {
    // CBazaarWnd_LoadTraderItems @0x63bcf0.
    auto load = game::load_trader_items(inventory_, trader_prices_);
    trader_.slots = std::move(load.slots);
    if (!trader_.slots.empty()) {
        bazaar_text(10, eq(strings_, 6746, {std::to_string(trader_.slots.size())}));
        bazaar_text(10, eq(strings_, 6747));
    } else {
        bazaar_text(10, eq(strings_, load.has_satchel ? 6748 : 6749));
    }
}

void GameConnection::trader_select(int slot) {
    // CBazaarWnd::SelectBazaarSlotItem @0x6393e0 (from its disassembly, loop included): the price
    // goes to 0; with an item in the slot the first of the 200 slots holding the same item (ItemBase +0xd8..+0xe4,
    // the GUID, or the same ItemBase_GetID and +0x44) becomes the selection and its price goes into the money
    // buttons; an empty slot leaves no selection.
    auto sel = game::select_trader_slot(trader_.slots, inventory_, slot);
    trader_.selected = sel.selected;
    trader_.entered_price = sel.price;
}

void GameConnection::trader_enter_price(std::uint32_t copper) { trader_.entered_price = copper; }

bool GameConnection::store_trader_price(std::uint32_t price) {
    // CBazaarWnd::sub_00638f70 @0x638f70: every slot holding the same item
    // (same merchant slot, or the same id and charges, or the same stackable
    // id) takes the price; the INI is updated and eqstr 6751 printed.
    const game::TraderSlot* picked = selected_slot(trader_);
    if (!picked) return false;
    const game::TraderSlot sel = *picked;
    const Item* sel_item = inventory_.at(sel.where);
    if (!sel_item) return false;
    if ((!sel.serial.empty() && sel.serial != sel_item->serial) ||
        (sel.item_id != 0 && sel.item_id != sel_item->id)) return false;
    if (price != 0 && refuse_kept(sel_item, "offer for sale")) return false;
    bool any = false;
    for (auto& s : trader_.slots) {
        const Item* it = inventory_.at(s.where);
        if (!it) continue;
        bool same = it->id == sel_item->id && (it->charges == sel_item->charges || sel_item->stackable());
        if (!same) continue;
        s.price = price;
        any = true;
    }
    if (!any) return false;
    trader_prices_.set(*sel_item, price);
    trader_prices_.save();
    bazaar_text(10, eq(strings_, 6751, {sel_item->name, game::coin_compact(price)}));
    return true;
}

void GameConnection::trader_set_price() {
    // CBazaarWnd::WndNotification, the Set Price button.
    std::uint32_t price = trader_.entered_price;
    if (trader_.selected == -1 || !store_trader_price(price) || !trader_.trader) return;
    const auto& s = *selected_slot(trader_); // store_trader_price checked it
    if (!s.active) {
        bazaar_text(15, eq(strings_, 6753)); // Click Add to add this item to your bazaar list.
        return;
    }
    send_zone(encode(TraderPrice{kTraderPriceUpdate, s.serial, 0, price}));
    bazaar_text(15, eq(strings_, 6752));
}

void GameConnection::trader_add() {
    // The Add button: every inactive slot with the selected item is offered at the price.
    std::uint32_t price = trader_.entered_price;
    if (price == 0 || !trader_.trader || !selected_slot(trader_)) return;
    const game::TraderSlot sel = *selected_slot(trader_);
    if (refuse_kept_id(sel.item_id, "sell")) return;
    const Item* selected = inventory_.at(sel.where);
    if (!selected || selected->id != sel.item_id || selected->serial != sel.serial) return;
    for (auto& s : trader_.slots) {
        if (s.active || s.item_id != sel.item_id) continue;
        const Item* item = inventory_.at(s.where);
        if (!item || item->id != s.item_id || item->serial != s.serial || refuse_kept(item, "sell")) continue;
        send_zone(encode(TraderPrice{kTraderPriceUpdate, s.serial, 0, price}));
    }
}

void GameConnection::trader_remove() {
    // The Remove button: price 0 for the selected item.
    const game::TraderSlot* s = selected_slot(trader_);
    if (!s || !trader_.trader) return;
    send_zone(encode(TraderPrice{kTraderPriceUpdate, s->serial, 0, 0}));
}

void GameConnection::trader_clear_price() {
    // The Clear button: the price goes to 0, is stored, and an active item is withdrawn.
    trader_.entered_price = 0;
    if (trader_.selected == -1 || !store_trader_price(0) || !trader_.trader) return;
    const auto& s = *selected_slot(trader_); // store_trader_price checked it
    send_zone(encode(TraderPrice{kTraderPriceUpdate, s.serial, 0, 0}));
}

bool GameConnection::stall_under_player() const {
    if (!touching_switch_) return false;
    for (const auto& d : doors_)
        if (d.id == *touching_switch_) return game::is_trader_stall(d.type);
    return false;
}

int GameConnection::trader_start() {
    // Send_OP_Trader_00639730 @0x639730 (the Begin Trader button): -1 when
    // not in a trader spot (eqstr 6743), -2 when nothing has a price (6754).
    // The spot is 0x4431a0: a bazaar zone, and the switch under the player (pTouchingSwitch, +0x2d4, set by the
    // floor test's CollsnCallbackForFloor) has type 0x99 or 0x9b (0x5101b0: EQSwitch +0x26), the stalls.
    if (!game::is_bazaar_zone(zone_id()) || !stall_under_player()) {
        bazaar_text(15, eq(strings_, 6743));
        return -1;
    }
    TraderBegin b;
    int n = 0;
    for (std::size_t i = 0; i < trader_.slots.size(); ++i) {
        auto& s = trader_.slots[i];
        s.active = false;
        if (s.price == 0) continue;
        const Item* item = inventory_.at(s.where);
        if (refuse_kept_id(s.item_id, "offer for sale")) continue;
        if (!item || item->id != s.item_id || item->serial != s.serial) continue;
        if (refuse_kept(item, "offer for sale")) continue;
        if (n >= static_cast<int>(b.serials.size())) break;
        b.serials[static_cast<std::size_t>(n)] = s.serial;
        b.prices[static_cast<std::size_t>(n)] = s.price;
        s.active = true;
        ++n;
    }
    if (n == 0) {
        bazaar_text(15, eq(strings_, 6754));
        return -2;
    }
    send_zone(encode(b));
    trader_.start_pending = true;
    return 1;
}

void GameConnection::trader_end() {
    // Send_OP_Trader_00638030 @0x638030: {2, 0, 0}; the active flags clear.
    send_zone(encode_trader_action(kTraderEnd));
    trader_.start_pending = false;
    for (auto& s : trader_.slots) s.active = false;
}

void GameConnection::trader_list() {
    // The List button (trader mode): {11, 0, 0}.
    if (trader_.trader) send_zone(encode_trader_action(kTraderListItems));
}

void GameConnection::handle_trader_message(const TraderMessage& m) {
    // CBazaarWnd::HandleTraderMsg @0x63c450 (HandleWorldMessage_Bazaar; action 21 there).
    auto sel_name = [&]() -> std::string {
        const game::TraderSlot* s = selected_slot(trader_);
        return s ? s->name : std::string();
    };
    switch (m.action) {
    case kTraderEnd:
        bazaar_text(13, eq(strings_, 6741));
        system(6741);
        trader_.trader = false;
        for (auto& s : trader_.slots) s.active = false;
        break;
    case kTraderPriceUpdate: {
        game::TraderSlot* picked = selected_slot(trader_);
        if (!picked) break; // no selection, or the slots were cleared since (a zone change)
        auto& sel = *picked;
        switch (m.word1) {
        case 0: sel.active = false; bazaar_text(15, eq(strings_, 6733)); break;
        case 1: sel.active = true; bazaar_text(15, eq(strings_, 6736, {sel_name()})); break;
        case 2: sel.active = false; bazaar_text(15, eq(strings_, 6735, {sel_name()})); break;
        case 3: sel.active = true; bazaar_text(15, eq(strings_, 6734, {sel_name()})); break;
        default: break;
        }
        break;
    }
    case kTraderBuyItem:
        if (m.sale && m.sale->sub_action == 0) {
            // "%1 purchased %2 %3 for (%4)." and the cash register (wave 0x8a).
            std::int64_t total = static_cast<std::int64_t>(m.sale->price) * m.sale->quantity;
            bazaar_text(15, eq(strings_, 6744, {m.sale->buyer_name, std::to_string(m.sale->quantity), m.sale->item_name,
                                                game::coin_compact(total)}));
            SoundCue c;
            c.kind = SoundCue::Kind::Wave;
            c.spell = 0x8a;
            sound_cues_.push_back(c);
        }
        trader_reconcile();
        break;
    case kTraderListItems: {
        bazaar_text(15, eq(strings_, 6737));
        int found = 1;
        for (std::size_t i = 0; i < m.listing.size(); ++i) {
            const auto& [serial, price] = m.listing[i];
            if (serial.empty() || serial.find_first_not_of('0') == std::string::npos) continue;
            auto it = std::find_if(trader_.slots.begin(), trader_.slots.end(), [&, &ser = serial](const auto& s) { return s.serial == ser; });
            if (it == trader_.slots.end()) {
                bazaar_text(10, eq(strings_, 6739, {std::to_string(i + 1)}));
            } else {
                it->active = true;
                it->price = price;
                bazaar_text(10, eq(strings_, 6738, {std::to_string(found++), it->name, game::coin_compact(price)}));
            }
        }
        bazaar_text(10, eq(strings_, 6740));
        break;
    }
    case kTraderCustomerBrowsing: {
        auto it = world_.spawns.find(m.word1);
        if (it != world_.spawns.end()) {
            bazaar_text(15, eq(strings_, 6745, {it->second.name}));
            trader_.customers.push_back(it->second.name);
        }
        break;
    }
    case kTraderAck2:
        if (m.word1 == 0) {
            bazaar_text(13, eq(strings_, 6742));
            trader_.trader = true;
            trader_.start_pending = false;
            send_zone(encode_trader_action(kTraderListItems));
        } else if (m.word1 == 12) {
            bazaar_text(13, eq(strings_, 6743));
        } else {
            system(m.word1 == 10 ? 784 : m.word1 == 5 ? 786 : 785);
        }
        break;
    case kTraderItemMessage:
        if (m.code21 == 1) bazaar_text(15, eq(strings_, 6735, {m.name}));
        else if (m.code21 == 2) bazaar_text(15, eq(strings_, 6736, {m.name}));
        break;
    default:
        break;
    }
}

void GameConnection::trader_reconcile() {
    // Send_OP_Trader_00638d80 @0x638d80: action 20 {GUID, count, price} for
    // every slot, then CBazaarWnd_RebuildTraderItems refills the slots.
    for (const auto& s : trader_.slots)
        send_zone(encode(TraderPrice{kTraderReconcile, s.serial, s.count, s.price}));
    auto prices = std::map<std::string, std::pair<std::uint32_t, bool>>{};
    for (const auto& s : trader_.slots) prices[s.serial] = {s.price, s.active};
    auto load = game::load_trader_items(inventory_, trader_prices_);
    trader_.slots = std::move(load.slots);
    for (auto& s : trader_.slots)
        if (auto it = prices.find(s.serial); it != prices.end()) {
            s.price = it->second.first;
            s.active = it->second.second;
        }
    if (trader_.selected >= static_cast<int>(trader_.slots.size())) trader_.selected = -1;
}

// ---------------------------------------------------------------- shopping at a trader

bool GameConnection::open_trader(std::uint32_t spawn_id) {
    // CEverQuest::RightClickedOnPlayer @0x536d80, a trader: no merchant open,
    // within 20; attack and autofire stop; not ourselves (eqstr 1469).
    auto it = world_.spawns.find(spawn_id);
    if (state_ != ConnectionState::InZone || it == world_.spawns.end() || merchant_) return false;
    if (distance_to(spawn_id) > kInteractRange) return false;
    if (auto_attack_) {
        set_auto_attack(false);
        system(1466);
    }
    if (auto_fire_) set_auto_fire(false);
    if (spawn_id == world_.player_id) {
        system(1469);
        return false;
    }
    trader_click_ = spawn_id;
    send_zone(encode(TraderClick{kTraderClick, spawn_id, 0}));
    return true;
}

void GameConnection::trader_buy(const Item& item, std::uint32_t quantity) {
    // CMerchantWnd::PurchasePageHandler::RequestGetItem @0x6f5be0, a player
    // merchant: OP_TraderShop action 10 with the trader, its name, the item's
    // name, GUID, id, price and the count.
    TraderBuy b;
    b.action = kTraderBuyItem;
    b.trader_id = merchant_->npc_id;
    auto it = world_.spawns.find(merchant_->npc_id);
    b.seller_name = it == world_.spawns.end() ? merchant_->name : it->second.name;
    b.item_name = item.name;
    b.serial = item.serial;
    b.item_id = item.id;
    b.price = item.price;
    b.quantity = quantity;
    send_zone(encode(b));
    ++merchant_->pending;
}

void GameConnection::handle_trader_purchase(const TraderBuy& in) {
    // HandleWorldMessage_Bazaar, OP_TraderShop action 10.
    TraderBuy b = in;
    if (is_eqemu_trade_failure(b)) b.sub_action = kTraderBuyFailed;
    if (b.method == 0) {
        if (merchant_ && merchant_->pending > 0) --merchant_->pending;
        if (b.sub_action == 0) {
            // CMerchantWnd::FinishBuyingItem: the price leaves the purse (1437).
            std::int64_t total = static_cast<std::int64_t>(b.price) * b.quantity;
            if (!game::take_money(coins_, total)) coins_ = {};
            system(1437, {game::money_words(total), merchant_ ? merchant_->name : b.seller_name}, 276); // Merchants
        }
    } else {
        --search_pending_;
        if (b.sub_action == 0) {
            std::int64_t total = static_cast<std::int64_t>(b.quantity) * b.price + b.already_sold;
            if (!game::take_money(coins_, total)) coins_ = {};
            std::string money = game::money_words(total);
            if (std::uint16_t vouchers = static_cast<std::uint16_t>(b.unknown276); vouchers != 0)
                money += std::format(" and {} vouchers", vouchers);
            system(787, {money, b.seller_name}, 276); // You send%1 to %2.
        }
    }
    if (b.sub_action == 2) return;
    std::string head = std::format("{} {}(s) from {}", b.quantity, b.item_name, b.seller_name);
    std::string text;
    switch (b.sub_action) {
    case 0:
        text = std::format("You successfully purchased {} {}(s) from {}.", b.quantity, b.item_name, b.seller_name);
        text += b.method == 1 ? " The item has been sent to your parcels." : " The item has been added to your inventory.";
        break;
    case 1: text = "Your attempt to purchase " + head + " was unsuccessful."; break;
    case 3:
        text = "Your attempt to purchase " + head + " failed because your bazaar data is out of date.";
        text += b.method == 0 ? " Please close and re-open the merchant window to get the latest trader data."
                              : " Please run the search again to get the latest bazaar data.";
        break;
    case 4: text = "Your attempt to purchase " + head + " failed because you already possess that lore item."; break;
    case 5:
        text = "Your attempt to purchase " + head +
               " failed because you are at the parcel cap and will be unable to have items mailed to you until you "
               "retrieve items from your parcels.";
        break;
    case 6:
        text = "Your attempt to purchase " + head +
               " failed because you already have a bazaar transaction pending. Please wait a few seconds and try again.";
        break;
    case 7: text = "Your attempt to purchase " + head + " failed because you do not have the required funds."; break;
    case 8: text = "Your attempt to purchase " + head + " failed because you do not have the required tokens."; break;
    case 9:
        text = "Your attempt to purchase " + head + " failed because the Krono was not successfully transferred.";
        break;
    default: return;
    }
    chat(ChatEvent::Kind::System, text);
}

// ---------------------------------------------------------------- the bazaar search

bool GameConnection::open_bazaar_search() {
    // Cmd_bazaar @0x4eab10 → CBazaarSearchWnd::AboutToShow @0x636670: the
    // first opening after zoning asks for the trader list (action 26).
    if (state_ != ConnectionState::InZone) return false;
    bazaar_search_open_ = true;
    if (!bazaar_.first_open_sent) {
        bazaar_.first_open_sent = true;
        send_zone(encode_trader_shop_action(kBazaarFirstOpen));
    }
    return true;
}

void GameConnection::close_bazaar_search() { bazaar_search_open_ = false; }

void GameConnection::bazaar_welcome() { send_zone(encode_trader_shop_action(kBazaarWelcome)); }

void GameConnection::bazaar_search(const BazaarQuery& q) {
    // CBazaarSearchWnd::doQuery @0x632e40: the result list empties, the query goes out.
    bazaar_.results.clear();
    bazaar_.searching = true;
    send_zone(encode(q));
}

void GameConnection::bazaar_inspect(std::size_t result, bool shift) {
    // Send_OP_TraderShop_00633520 @0x633520.
    if (result >= bazaar_.results.size()) return;
    const auto& r = bazaar_.results[result];
    send_zone(encode(BazaarInspect{r.trader_id, r.serial, r.item_id, shift ? 2u : 0u}));
}

void GameConnection::bazaar_buy(std::size_t result, std::uint32_t quantity, bool parcel) {
    // CBazaarConfirmationWnd::WndNotification @0x41d6a0: method 2 (to the
    // inventory) or 1 (by parcel), the struct filled by SetPurchaseInfo.
    if (result >= bazaar_.results.size() || quantity == 0) return;
    const auto& r = bazaar_.results[result];
    TraderBuy b;
    b.action = kTraderBuyItem;
    b.method = parcel ? 1 : 2;
    b.trader_id = r.trader_id;
    auto t = bazaar_.traders.find(r.trader_id);
    b.seller_name = t == bazaar_.traders.end() ? std::string() : t->second.name;
    b.buyer_name = character_;
    b.item_name = r.name;
    b.serial = r.serial;
    b.item_id = r.item_id;
    b.price = r.cost;
    b.quantity = quantity;
    ++search_pending_;
    send_zone(encode(b));
}

namespace {
constexpr std::uint32_t kExpansionRoFBazaar = 0x40000; // the character's flag a row's Buy needs (eqstr 7728)
constexpr std::uint32_t kFindWaitMs = 16000;           // "(now - +0x240) / 1000 > 15"
} // namespace

bool GameConnection::bazaar_row_buy_allowed() {
    // CBazaarSearchWnd::WndNotification @0x635990, a click on a button inside the item list (a row's Buy, made from
    // the BZR_BuyButton template by AddItemtoList into column 8): without the RoF expansion flag 7728 in colour 13;
    // nothing while an item request is pending (IsItemPending: the search purchases and the item uses waiting).
    if (!profile_ || (profile_->expansions & kExpansionRoFBazaar) == 0) {
        system(7728, {}, 13);
        return false;
    }
    return search_pending_ < 1 && spells_.pending < 1;
}

bool GameConnection::bazaar_find_trader_enabled(int result) const {
    // CBazaarSearchWnd_UpdateButtons @0x632a40 disables the button and sets +0x256 to "the selected row's trader is
    // in this zone"; OnProcessFrame @0x6336f0 enables it to +0x256 while no find path is shown and 15 s have passed.
    if (result < 0 || result >= static_cast<int>(bazaar_.results.size())) return false;
    auto t = bazaar_.traders.find(bazaar_.results[static_cast<std::size_t>(result)].trader_id);
    if (t == bazaar_.traders.end() || t->second.zone_id != zone_id()) return false;
    if (path_.active()) return false;
    return !bazaar_.find_sent || eq_time() - bazaar_.last_find_ms >= kFindWaitMs;
}

void GameConnection::bazaar_find_trader(std::size_t result, bool throttled) {
    // BZR_FindTraderButton (and a right click on a row's Trader column, the path then at most every 15 s): the row's
    // trader (AddItemtoList's sellerID and zoneID: the trader list's spawn id and zone) when it has a spawn id and is
    // in this zone: opcode 0x0e44 {spawn id}, PlayerPath_FindSpawnByID(spawn, 1, 0), the time and the found trader.
    if (result >= bazaar_.results.size()) return;
    auto t = bazaar_.traders.find(bazaar_.results[result].trader_id);
    if (t == bazaar_.traders.end()) return;
    std::uint32_t spawn = t->second.entity_id;
    if (spawn == 0 || t->second.zone_id != zone_id()) return;
    send_zone(encode_find_market_spawn(spawn));
    if (throttled) {
        bazaar_.found_trader = spawn;
        if (bazaar_.find_sent && eq_time() - bazaar_.last_find_ms < kFindWaitMs) return;
    }
    if (auto it = world_.spawns.find(spawn); it != world_.spawns.end()) send_find_path(it->second, true);
    bazaar_.last_find_ms = eq_time();
    bazaar_.find_sent = true;
    bazaar_.found_trader = spawn;
}

void GameConnection::bazaar_set_hide_traders(bool hide, bool clicked) {
    // The ctor reads HideTraders into the button; a click writes it (WriteBoolToUIINI, the caller) and sets +0x24c 0.
    bazaar_.hide_traders = hide;
    if (clicked) bazaar_.found_trader = 0;
}

bool GameConnection::market_hidden(const game::Spawn& s) const {
    // PlayerZoneClient::IsInvisible @0x5a1850 for another spawn of type < 2 with HP above 0, after its property 100
    // check: a trader (+0x320) while CBazaarSearchWnd's Hide Traders is checked (0x6334f0) and it is not the
    // found trader (+0x24c); else a buyer (+0x3d4) while CBarterSearchWnd's +0x2c0 is set and it is not +0x2bc.
    if (s.id == world_.player_id || s.is_corpse || s.hp_percent == 0) return false;
    if (s.trader) return bazaar_.hide_traders && s.id != bazaar_.found_trader;
    if (s.buyer) return barter_search_.hide_buyers && s.id != barter_search_.found_buyer;
    return false;
}

bool GameConnection::market_hides_any() const { return bazaar_.hide_traders || barter_search_.hide_buyers; }

bool GameConnection::feature_refused(std::int32_t feature) {
    if (game_feature(feature) != 0) return false;
    chat(ChatEvent::Kind::System, game::feature_refused_message(feature, membership_details_), 0,
         game::kFeatureRefusedColor);
    return true;
}


// ---------------------------------------------------------------- barter: the manager

namespace {

constexpr std::uint32_t kBarterSound = 0x8a;   // EqSoundManager::WavePlay(0x8a): the reports' cash register
constexpr std::uint32_t kExpansionBarter = 0x100; // 0xde09c8: the expansion bit 0x420e50 needs
constexpr std::uint32_t kExpansionRoF = 0x40000;  // the character's flag CBarterSearchWnd's Sell needs (eqstr 7728)

std::uint32_t fast_time() { return static_cast<std::uint32_t>(std::time(nullptr)); } // Util::FastTime

} // namespace

void GameConnection::barter_log(const std::string& text) {
    // Barter_WriteLog @0x421630: "[ctime] text" appended to Logs\btrlog_<server>_<character>.txt (here btrlog.txt in
    // the character's folder).
    auto path = own_files().barter_log();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) return;
    std::time_t t = std::time(nullptr);
    char stamp[32] = {};
    std::strftime(stamp, sizeof stamp, "%a %b %d %H:%M:%S %Y", std::localtime(&t)); // __ctime32 without its newline
    out << '[' << stamp << "] " << text << '\n';
}

void GameConnection::barter_status(std::uint32_t color, const std::string& text, bool log) {
    // CBarterWnd_AppendStatusText @0x62eb90 (the STML control keeps 200 lines, pStmlReport = 0xc8).
    log::debug("barter: {}", text);
    barter_window_.status.push_back({color, text});
    if (barter_window_.status.size() > 200) barter_window_.status.erase(barter_window_.status.begin());
    if (log) barter_log(text);
}

void GameConnection::barter_load_ini() {
    // BarterBuyLines_LoadFromIni @0x421f00: [BuyLines] BuyLine00.. until the first missing one (a line with a
    // quantity is enabled), then [General] Welcome.
    IniFile ini;
    ini.load(own_files().barter_lines());
    auto& w = barter_window_;
    for (auto& l : w.lines) l = BuyLine{};
    for (int i = 0; i < game::kBuyLineCount; ++i) {
        std::string text = ini.get_string("BuyLines", std::format("BuyLine{:02d}", i));
        if (text.empty()) break;
        auto& l = w.lines[static_cast<std::size_t>(i)];
        l = game::parse_buy_line_ini(text);
        if (l.quantity > 0) l.enabled = true;
    }
    w.welcome = ini.get_string("General", "Welcome").substr(0, 255);
}

void GameConnection::barter_save_ini(int slot) {
    // BarterBuyLines_SaveToIni @0x422040: one line (or all with -1), then the welcome text.
    auto path = own_files().barter_lines();
    if (path.empty()) return;
    IniFile ini;
    ini.load(path);
    int first = slot < 0 ? 0 : slot, last = slot < 0 ? game::kBuyLineCount - 1 : slot;
    for (int i = first; i <= last && i < game::kBuyLineCount; ++i)
        ini.set("BuyLines", std::format("BuyLine{:02d}", i), game::buy_line_ini(barter_window_.lines[static_cast<std::size_t>(i)]));
    ini.set("General", "Welcome", barter_window_.welcome);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    ini.save(path);
}

void GameConnection::barter_send_line(const BuyLine& line) {
    // Send_OP_BuyerItems_004229a0: out of buyer mode the line is stored at its slot; in buyer mode it goes to the
    // server, whose answer (OP_BuyerItems 7) stores it.
    if (!barter_window_.buyer_mode) {
        if (line.slot < static_cast<std::uint32_t>(game::kBuyLineCount)) barter_window_.lines[line.slot] = line;
        return;
    }
    if (line.toggle)
        for (const auto& item : line.trade_items)
            if (refuse_kept_id(item.item_id, "offer as compensation")) return;
    if (state_ == ConnectionState::InZone) send_zone(encode_modify_buy_line(line));
}

void GameConnection::barter_reset_editor() {
    // The editor's reset after a create, modify, toggle or remove: the buy line selection cleared, CurrentBuyLine
    // reset (0x7d6920).
    barter_window_.line_sel = -1;
    barter_window_.compensation_sel = -1;
    barter_window_.editor = BuyLine{};
}

// ---------------------------------------------------------------- barter: CBarterWnd (/buyer)

bool GameConnection::open_barter_window() {
    // Cmd_buyer @0x4e6f60 shows the window (its feature check, GameFeatureRestrictions 0xf, is not modelled);
    // CBarterWnd::AboutToShow @0x6320f0 refuses unless 0x420e50 (the expansion bit 0x100, or a server named
    // "test") and the zone is a barter zone; CBarterWnd_InitOnShow @0x6318f0 then loads the lines and the welcome,
    // builds the inventory, clears the item search and the status text.
    if (state_ != ConnectionState::InZone) return false;
    bool allowed = (profile_ && (profile_->expansions & kExpansionBarter) != 0) || iequals(ticket_.server_name, "test");
    if (!allowed || !game::is_barter_zone(zone_id())) return false;
    auto& w = barter_window_;
    if (w.open) return true;
    w.open = true;
    barter_load_ini();
    w.editor = BuyLine{};
    w.inventory = game::barter_window_inventory(inventory_);
    w.inventory_sel = w.compensation_sel = w.line_sel = w.item_sel = -1;
    w.item_results.clear(); // 0x62ef10
    w.last_search = fast_time() - 2;
    w.status.clear();
    w.welcome_changed = false;
    return true;
}

void GameConnection::close_barter_window() {
    // CBarterWnd::AboutToHide @0x62eeb0: buyer mode off (Send_OP_Barter_00420ff0(0)), the lines saved.
    auto& w = barter_window_;
    if (!w.open) return;
    w.open = false;
    if (w.buyer_mode && state_ == ConnectionState::InZone) send_zone(encode_buyer_mode(false));
    w.buyer_mode = false;
    barter_save_ini(-1);
}

game::BarterWindowButtons GameConnection::barter_window_buttons() const {
    return game::barter_window_buttons(barter_window_, fast_time());
}

void GameConnection::barter_item_search(const std::string& text) {
    // Send_OP_Barter_00631750: the selections and the editor cleared, the result list emptied (0x6313d0),
    // OP_Barter {0, text}, eqstr 6057 to the status text and the log, the search time stamped.
    if (state_ != ConnectionState::InZone) return;
    auto& w = barter_window_;
    w.item_sel = w.inventory_sel = -1;
    barter_reset_editor();
    w.item_results.clear();
    send_zone(encode_buyer_item_search(text.substr(0, 63)));
    barter_status(15, eq(strings_, 6057, {text}));
    w.last_search = fast_time();
}

void GameConnection::barter_item_click(int row, bool shift, bool ctrl, bool alt) {
    // CBarterWnd::WndNotification, a click on ItemSearchList: Shift + Ctrl searches the bazaar for the item, Shift +
    // Alt searches the buyers (Send_OP_Barter_0062bc90), else the item becomes the editor's line (sub_00630be0).
    auto& w = barter_window_;
    if (row < 0 || row >= static_cast<int>(w.item_results.size())) {
        w.item_sel = -1;
        return;
    }
    const auto& e = w.item_results[static_cast<std::size_t>(row)];
    if (shift && ctrl) {
        open_bazaar_search();
        return;
    }
    if (shift && alt) {
        barter_search(e.name);
        return;
    }
    barter_reset_editor();
    w.item_sel = row;
    w.editor = BuyLine{};
    w.editor.enabled = true; // +0x2d4
    w.editor.toggle = true;  // +0x324: a new line is active
    w.editor.item_id = e.item_id;
    w.editor.icon = e.icon;
    w.editor.item_name = e.name.substr(0, 63);
    w.inventory_sel = -1;
}

void GameConnection::barter_item_inspect(int row, bool shift) {
    // A right click on ItemSearchList: OP_Barter {0x15, item id, 2 with Shift}.
    const auto& r = barter_window_.item_results;
    if (row < 0 || row >= static_cast<int>(r.size()) || state_ != ConnectionState::InZone) return;
    send_zone(encode_barter_search_item_inspect(r[static_cast<std::size_t>(row)].item_id, shift));
}

void GameConnection::barter_select_inventory(int row) {
    auto& w = barter_window_;
    w.inventory_sel = row >= 0 && row < static_cast<int>(w.inventory.size()) ? row : -1;
}

void GameConnection::barter_select_compensation(int row) {
    auto& w = barter_window_;
    w.compensation_sel = row >= 0 && row < static_cast<int>(w.editor.trade_items.size()) ? row : -1;
}

void GameConnection::barter_select_line(int row) {
    // sub_00630ce0: the selections cleared and the line copied into the editor.
    auto& w = barter_window_;
    w.item_sel = w.inventory_sel = -1;
    barter_reset_editor();
    if (row < 0 || row >= game::kBuyLineCount || !w.lines[static_cast<std::size_t>(row)].enabled) return;
    w.line_sel = row;
    w.editor = w.lines[static_cast<std::size_t>(row)];
}

void GameConnection::barter_set_count(std::int32_t count) {
    // WndNotification 0xe on CountEdit: atoi clamped to 0..999.
    barter_window_.editor.quantity = static_cast<std::uint32_t>(std::clamp(count, 0, game::kMaxBuyCount));
}

void GameConnection::barter_set_price_coin(int coin, std::uint32_t value) {
    // CBarterWnd_SetPriceCoin @0x630750, the quantity dialog's answer for a coin button.
    auto& e = barter_window_.editor;
    e.cost = game::barter_set_price_coin(e.cost, coin, value);
}

std::optional<std::pair<std::int32_t, std::int32_t>> GameConnection::barter_compensation_add_start() {
    // CBarterWnd_AddCompensationItem @0x630e20 (-1): a Krono line takes no compensation (5367), the item bought cannot
    // be offered (6127), nor Krono (5368); else the quantity dialog for up to 20 of the item, starting at that.
    auto& w = barter_window_;
    if (w.editor.item_id == kKronoItemId) {
        system(5367, {}, 15);
        return std::nullopt;
    }
    if (w.inventory_sel < 0 || w.inventory_sel >= static_cast<int>(w.inventory.size())) return std::nullopt;
    const auto& item = w.inventory[static_cast<std::size_t>(w.inventory_sel)];
    if (item.item_id == w.editor.item_id) {
        system(6127, {}, 15);
        return std::nullopt;
    }
    if (item.item_id == kKronoItemId) {
        system(5368, {}, 15);
        return std::nullopt;
    }
    auto n = static_cast<std::int32_t>(std::min<std::uint32_t>(item.count, game::kMaxSaleQuantity));
    return std::pair{n, n};
}

void GameConnection::barter_compensation_add(std::uint32_t quantity) {
    // CBarterWnd_AddCompensationItem (the dialog's answer): added to the same item's entry, or a new entry while
    // there are fewer than ten.
    auto& w = barter_window_;
    if (w.inventory_sel < 0 || w.inventory_sel >= static_cast<int>(w.inventory.size())) return;
    const auto& item = w.inventory[static_cast<std::size_t>(w.inventory_sel)];
    for (auto& t : w.editor.trade_items)
        if (t.item_id == item.item_id) {
            t.quantity += quantity;
            return;
        }
    if (static_cast<int>(w.editor.trade_items.size()) >= game::kMaxCompensation) return;
    w.editor.trade_items.push_back({item.item_id, quantity, item.icon, item.name});
}

std::optional<std::pair<std::int32_t, std::int32_t>> GameConnection::barter_compensation_remove_start() {
    // CBarterWnd_OpenCompensationQuantity @0x630620 (-1): the dialog for up to the entry's quantity.
    auto& w = barter_window_;
    if (w.compensation_sel < 0 || w.compensation_sel >= static_cast<int>(w.editor.trade_items.size())) return std::nullopt;
    auto n = static_cast<std::int32_t>(w.editor.trade_items[static_cast<std::size_t>(w.compensation_sel)].quantity);
    return std::pair{n, n};
}

void GameConnection::barter_compensation_remove(std::uint32_t quantity) {
    // The dialog's answer: the entry goes when the answer is its whole quantity, else it loses that much.
    auto& w = barter_window_;
    if (w.compensation_sel < 0 || w.compensation_sel >= static_cast<int>(w.editor.trade_items.size())) return;
    auto& t = w.editor.trade_items[static_cast<std::size_t>(w.compensation_sel)];
    if (t.quantity <= quantity) {
        w.editor.trade_items.erase(w.editor.trade_items.begin() + w.compensation_sel);
        w.compensation_sel = -1;
    } else {
        t.quantity -= quantity;
    }
}

void GameConnection::barter_create_line() {
    // sub_00630ab0 (Create Buy): a quantity (6122) and a price or compensation (6123) are needed, and a free line
    // (6124); the line takes the free slot, is sent or stored (Send_OP_BuyerItems_004229a0) and saved.
    auto& w = barter_window_;
    std::uint32_t refused = 0;
    if (w.editor.quantity < 1) refused = 6122;
    else if (w.editor.trade_items.empty() && w.editor.cost == 0) refused = 6123;
    else if (w.free_slot() < 0) refused = 6124;
    if (refused) {
        barter_status(15, eq(strings_, refused), false);
        return;
    }
    int slot = w.free_slot();
    w.editor.slot = static_cast<std::uint32_t>(slot);
    barter_send_line(w.editor);
    barter_save_ini(slot);
    w.item_sel = w.inventory_sel = -1;
    barter_reset_editor();
}

void GameConnection::barter_modify_line() {
    // sub_006309c0 (Modify): the same checks but the free line's; the edited line is sent or stored.
    auto& w = barter_window_;
    std::uint32_t refused = 0;
    if (w.editor.quantity < 1) refused = 6122;
    else if (w.editor.trade_items.empty() && w.editor.cost == 0) refused = 6123;
    if (refused) {
        barter_status(15, eq(strings_, refused), false);
        return;
    }
    barter_send_line(w.editor);
    barter_reset_editor();
}

void GameConnection::barter_toggle_line() {
    // sub_0062ece0 → Send_OP_BuyerItems_00422b70: out of buyer mode the line's active flag flips; in buyer mode the
    // flipped line is sent (the server's answer changes it) and the editor is reset.
    auto& w = barter_window_;
    if (w.line_sel < 0 || w.line_sel >= game::kBuyLineCount) return;
    auto& l = w.lines[static_cast<std::size_t>(w.line_sel)];
    if (!w.buyer_mode) {
        l.toggle = !l.toggle;
        return;
    }
    BuyLine flipped = l;
    flipped.toggle = !flipped.toggle;
    if (flipped.toggle)
        for (const auto& item : flipped.trade_items)
            if (refuse_kept_id(item.item_id, "offer as compensation")) return;
    if (state_ == ConnectionState::InZone) send_zone(encode_modify_buy_line(flipped));
    barter_reset_editor();
}

void GameConnection::barter_remove_line() {
    // sub_00630dc0 → Send_OP_Barter_00421470: out of buyer mode the line is emptied; in buyer mode OP_Barter {8, slot}.
    auto& w = barter_window_;
    if (w.line_sel < 0 || w.line_sel >= game::kBuyLineCount) return;
    auto slot = static_cast<std::uint32_t>(w.line_sel);
    if (!w.buyer_mode) w.lines[slot] = BuyLine{};
    else if (state_ == ConnectionState::InZone) send_zone(encode_remove_buy_line(slot));
    barter_reset_editor();
}

void GameConnection::barter_list_lines() {
    // CBarterWnd_ListActiveLines @0x62f700: "Listing Active Lines:" and each enabled, active line as eqstr 6066
    // "Buying %1 %2, offering %3 each." (Barter_FormatBuyLineText @0x421550).
    barter_status(0, "Listing Active Lines:");
    for (const auto& l : barter_window_.lines)
        if (l.enabled && l.toggle)
            barter_status(0, eq(strings_, 6066, {std::to_string(l.quantity), l.item_name, game::buy_line_price_text(l)}));
}

void GameConnection::barter_start_stop() {
    // StartStopButton: Send_OP_Barter_00420ff0(!buyer mode): {2} or {3}; asking for off clears the flag at once.
    auto& w = barter_window_;
    if (state_ != ConnectionState::InZone) return;
    if (!w.buyer_mode)
        for (const auto& line : w.lines)
            if (line.enabled && line.toggle)
                for (const auto& item : line.trade_items)
                    if (refuse_kept_id(item.item_id, "offer as compensation")) return;
    send_zone(encode_buyer_mode(!w.buyer_mode));
    if (w.buyer_mode) w.buyer_mode = false;
}

void GameConnection::barter_update_welcome(const std::string& text) {
    // UpdateWelcomeButton → Send_OP_Barter_00421d20: the text kept (256 bytes), sent in buyer mode (0x13), eqstr 6069
    // to the status text and the log, saved.
    auto& w = barter_window_;
    w.welcome_changed = false;
    w.welcome = text.substr(0, 255);
    if (w.buyer_mode && state_ == ConnectionState::InZone) send_zone(encode_buyer_welcome(w.welcome));
    barter_status(0, eq(strings_, 6069, {w.welcome}));
    auto path = own_files().barter_lines();
    if (path.empty()) return;
    IniFile ini;
    ini.load(path);
    ini.set("General", "Welcome", w.welcome);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    ini.save(path);
}

void GameConnection::barter_refresh_inventory() {
    // InventoryRefreshButton → CBarterWnd_BuildInventoryItems @0x6315e0.
    barter_window_.inventory = game::barter_window_inventory(inventory_);
    barter_window_.inventory_sel = -1;
}

// ---------------------------------------------------------------- barter: CBarterMerchantWnd

bool GameConnection::open_buyer(std::uint32_t spawn_id) {
    // RightClickedOnPlayer @0x536d80, a buyer: no merchant open, within 20; attack stops; not ourselves. OP_Barter
    // {11, spawn id}; the window opens on the answer (Handle_OP_Barter 11).
    auto it = world_.spawns.find(spawn_id);
    if (state_ != ConnectionState::InZone || it == world_.spawns.end() || merchant_ || barter_merchant_.buyer_id) return false;
    if (distance_to(spawn_id) > kInteractRange) {
        log::info("barter: buyer {} is {:.1f} away", spawn_id, distance_to(spawn_id));
        return false;
    }
    if (auto_attack_) {
        set_auto_attack(false);
        system(1466);
    }
    if (spawn_id == world_.player_id) {
        system(1469);
        return false;
    }
    log::info("barter: inspecting buyer {}", spawn_id);
    send_zone(encode_barter_inspect(kBuyerInspectBegin, spawn_id));
    return true;
}

void GameConnection::close_buyer() {
    // CBarterMerchantWnd::AboutToHide → Send_OP_Barter_00420eb0: {12, spawn id} while the buyer is still a buyer, the
    // active merchant cleared, a pending sale's count given back.
    auto& m = barter_merchant_;
    if (m.buyer_id == 0) return;
    auto it = world_.spawns.find(m.buyer_id);
    if (it != world_.spawns.end() && it->second.buyer && state_ == ConnectionState::InZone)
        send_zone(encode_barter_inspect(kBuyerInspectEnd, m.buyer_id));
    m = game::BarterMerchantState{};
}

void GameConnection::barter_merchant_select(std::uint32_t slot) { barter_merchant_.selected_slot = slot; }

std::optional<std::uint32_t> GameConnection::barter_merchant_sell_start() {
    // CBarterMerchantWnd::WndNotification, Sell: while a sale is waiting, "%1 tells you, 'Whoa! ...'" (12063,
    // colour 0x111); else the quantity dialog for up to the line's quantity, at most 20.
    auto& m = barter_merchant_;
    if (m.buyer_id == 0) return std::nullopt;
    if (m.pending > 0) {
        auto it = world_.spawns.find(m.buyer_id);
        system(12063, {it == world_.spawns.end() ? std::string() : game::display_name(it->second.name)}, 0x111);
        return std::nullopt;
    }
    for (const auto& l : m.lines)
        if (l.slot == m.selected_slot) {
            if (refuse_kept_id(l.item_id, "sell")) return std::nullopt;
            return std::min<std::uint32_t>(l.quantity, game::kMaxSaleQuantity);
        }
    return std::nullopt;
}

void GameConnection::barter_merchant_sell(std::uint32_t quantity) {
    // The dialog's answer (WndNotification 0x24): a pending sale, OP_BuyerItems 9 with method 0, the buyer's spawn id
    // and the line as the buyer sent it, the quantity at +0x170; Sell is disabled until the next refresh.
    auto& m = barter_merchant_;
    if (m.buyer_id == 0 || state_ != ConnectionState::InZone) return;
    for (const auto& l : m.lines) {
        if (l.slot != m.selected_slot) continue;
        if (refuse_kept_id(l.item_id, "sell")) return;
        ++m.pending;
        BuyerSale sale;
        sale.method = kBarterAtBuyer;
        sale.buyer_spawn_id = m.buyer_id;
        sale.line = l;
        sale.quantity = quantity;
        send_zone(encode(sale));
        break;
    }
    m.sell_enabled = false;
}

void GameConnection::barter_merchant_inspect(int compensation, bool shift) {
    // A right click on DetailList: OP_Barter 0x0f for the selected line's compensation item.
    auto& m = barter_merchant_;
    if (m.buyer_id == 0 || state_ != ConnectionState::InZone) return;
    for (const auto& l : m.lines) {
        if (l.slot != m.selected_slot) continue;
        if (compensation < 0 || compensation >= static_cast<int>(l.trade_items.size())) return;
        BarterItemInspect v;
        v.buyer = m.buyer_id;
        v.zone = zone_id();
        v.slot = l.slot;
        v.index = compensation;
        v.item_id = l.trade_items[static_cast<std::size_t>(compensation)].item_id;
        v.shift = shift;
        send_zone(encode(v));
        return;
    }
}

void GameConnection::update_barter() {
    // CBarterMerchantWnd::OnProcessFrame @0x62a500: every 300 ms Sell is enabled when nothing is pending, a line is
    // selected and the player has its item (sub_0062a3d0; a Krono line needs Krono); the window closes when the
    // buyer is gone or beyond PlayerClient_IsWithinInteractRange(buyer, 0, 20.0).
    auto& m = barter_merchant_;
    if (m.buyer_id == 0) return;
    std::int64_t now = now_ms();
    if (now > m.checked_ms + 300) {
        m.checked_ms = now;
        m.sell_enabled = false;
        if (m.pending < 1)
            for (const auto& l : m.lines)
                if (l.slot == m.selected_slot)
                    m.sell_enabled = l.item_id != kKronoItemId && game::barter_has_items(inventory_, l.item_id, 1);
    }
    const game::Spawn* me = world_.player();
    auto it = world_.spawns.find(m.buyer_id);
    bool in_range = false;
    if (me && it != world_.spawns.end()) {
        glm::vec3 at = motion_known_ ? position_ : me->position;
        in_range = !(game::player_distance(&at, &it->second.position) > kInteractRange);
    }
    if (!in_range) close_buyer();
}

// ---------------------------------------------------------------- barter: CBarterSearchWnd (/barter)

void GameConnection::set_barter_search_open(bool open) {
    // Cmd_barter @0x4e6f30 toggles the window. CBarterSearchWnd::AboutToShow @0x62e340 (sub_0062e2c0): the selection
    // and the matches cleared, the inventory built, and once per zone OP_Barter 0x17 (the buyers for the combo).
    // AboutToHide @0x62b060 forgets the Find target.
    auto& s = barter_search_;
    if (open == s.open) return;
    s.open = open;
    if (!open) {
        s.found_buyer = 0xffffffffu; // AboutToHide @0x62b060
        return;
    }
    s.selected = s.detail_sel = s.sale_line = -1;
    s.lines.clear();
    s.inventory = game::barter_search_inventory(inventory_, 0);
    if (!s.first_open_sent && state_ == ConnectionState::InZone) {
        s.first_open_sent = true;
        send_zone(encode_barter_action(kBarterOpenWindow));
    }
}

void GameConnection::barter_search(const std::string& text) {
    // Send_OP_Barter_0062bc90: the window shown, the edit set to the text, OP_Barter 1 with the time as the
    // transaction id and the combo's choice (a buyer's character id, or the scope 1 for "Local Barterers"), the
    // selection and the matches cleared.
    if (state_ != ConnectionState::InZone) return;
    auto& s = barter_search_;
    if (!s.open) set_barter_search_open(true);
    s.text = text.substr(0, 63);
    BarterSearch q;
    q.text = s.text;
    q.transaction_id = static_cast<std::uint32_t>(now_ms());
    s.transaction_id = q.transaction_id;
    s.last_search_ms = q.transaction_id;
    auto choices = s.combo_choices();
    if (s.combo > 0 && s.combo < static_cast<int>(choices.size())) {
        std::int64_t data = choices[static_cast<std::size_t>(s.combo)].second;
        if (data == -1) q.local = true;
        else q.buyer_id = static_cast<std::uint32_t>(data);
    }
    send_zone(encode(q));
    s.selected = s.detail_sel = -1;
    s.lines.clear();
}

void GameConnection::barter_search_select(int line) {
    // CBarterSearchWnd_SelectBuyLine @0x62b480: the line's compensation items fill DetailsList.
    auto& s = barter_search_;
    if (line != s.selected) s.detail_sel = -1;
    s.selected = line >= 0 && line < static_cast<int>(s.lines.size()) ? line : -1;
}

void GameConnection::barter_search_refresh_inventory() {
    barter_search_.inventory = game::barter_search_inventory(inventory_, 0);
}

std::optional<std::uint32_t> GameConnection::barter_search_sell_start(int line) {
    // WndNotification 1 on a line's Sell button: the RoF flag (7728), no pending sale, not our own line, a line with
    // compensation needs its buyer in this zone, the item (or Krono) on hand; then the line is selected and the
    // quantity dialog asks for up to its quantity, at most 20.
    auto& s = barter_search_;
    if (line < 0 || line >= static_cast<int>(s.lines.size())) return std::nullopt;
    if (!profile_ || (profile_->expansions & kExpansionRoF) == 0) {
        system(7728, {}, 13);
        return std::nullopt;
    }
    if (s.pending > 0) return std::nullopt;
    const auto& l = s.lines[static_cast<std::size_t>(line)];
    if (refuse_kept_id(l.item_id, "sell")) return std::nullopt;
    if (profile_ && l.buyer_id == profile_->character_id) {
        chat(ChatEvent::Kind::System, "You cannot sell an item to yourself!", 0, 13);
        return std::nullopt;
    }
    if (l.zone != zone_id() && !l.trade_items.empty()) {
        chat(ChatEvent::Kind::System,
             "You must be in the same zone as the buyer when selling items to a buyer providing item compensation!", 0, 13);
        return std::nullopt;
    }
    if (l.item_id != kKronoItemId && !game::barter_has_items(inventory_, l.item_id, 1)) {
        chat(ChatEvent::Kind::System, "You cannot sell an item that isn't in your inventory!", 0, 13);
        return std::nullopt;
    }
    if (l.item_id == kKronoItemId) {
        chat(ChatEvent::Kind::System, "You cannot sell any Krono because you don't have any in your Krono slot!", 0, 13);
        return std::nullopt;
    }
    s.sale_line = line;
    barter_search_select(line);
    return std::min<std::uint32_t>(l.quantity, game::kMaxSaleQuantity);
}

void GameConnection::barter_search_sell(std::uint32_t quantity) {
    // sub_0062cf70: at most 20, the quantity on hand (else the failure text), a pending sale, OP_BuyerItems 9 with
    // method 1 (the buyer's zone is this one) or 2, the line's buyer spawn and character ids and the whole line.
    auto& s = barter_search_;
    if (s.sale_line < 0 || s.sale_line >= static_cast<int>(s.lines.size()) || state_ != ConnectionState::InZone) return;
    quantity = std::min<std::uint32_t>(quantity, game::kMaxSaleQuantity);
    const auto& l = s.lines[static_cast<std::size_t>(s.sale_line)];
    if (refuse_kept_id(l.item_id, "sell")) return;
    if (l.item_id != kKronoItemId && !game::barter_has_items(inventory_, l.item_id, quantity)) {
        chat(ChatEvent::Kind::System,
             "Your transaction failed because you don't possess the specified quantity of the item you wish to sell.", 0, 13);
        return;
    }
    if (l.item_id == kKronoItemId) {
        chat(ChatEvent::Kind::System,
             "Your transaction failed because you don't possess the specified quantity of Krono in your Krono slot.", 0, 13);
        return;
    }
    ++s.pending;
    BuyerSale sale;
    sale.method = l.zone != zone_id() ? kBarterOutsideBazaar : kBarterInBazaar;
    sale.buyer_spawn_id = l.buyer_spawn_id;
    sale.buyer_id = l.buyer_id;
    sale.line = l;
    sale.quantity = quantity;
    send_zone(encode(sale));
}

void GameConnection::barter_search_inspect(int compensation, bool shift) {
    // Send_OP_Barter_0062b200: OP_Barter 0x0f for the selected line's item (-1) or a compensation item; the buyer by
    // spawn id when the line's zone is this one, else by character id.
    auto& s = barter_search_;
    if (s.selected < 0 || s.selected >= static_cast<int>(s.lines.size()) || state_ != ConnectionState::InZone) return;
    const auto& l = s.lines[static_cast<std::size_t>(s.selected)];
    BarterItemInspect v;
    v.buyer = l.zone == zone_id() ? l.buyer_spawn_id : l.buyer_id;
    v.zone = l.zone;
    v.slot = l.slot;
    v.index = compensation;
    if (compensation < 0) v.item_id = l.item_id;
    else if (compensation < static_cast<int>(l.trade_items.size())) v.item_id = l.trade_items[static_cast<std::size_t>(compensation)].item_id;
    else return;
    v.shift = shift;
    send_zone(encode(v));
}

void GameConnection::barter_search_greeting() {
    // GreetingButton: OP_Barter {0x14, the line's buyer character id}.
    auto& s = barter_search_;
    if (s.selected < 0 || s.selected >= static_cast<int>(s.lines.size()) || state_ != ConnectionState::InZone) return;
    send_zone(encode_buyer_greeting(s.lines[static_cast<std::size_t>(s.selected)].buyer_id));
}

void GameConnection::barter_search_welcome() {
    // WelcomeButton: OP_Barter {0x11}.
    if (state_ == ConnectionState::InZone) send_zone(encode_barter_action(kBarterWelcomeMessage));
}

void GameConnection::barter_search_find() {
    // FindButton: for a line whose buyer is in this zone, opcode 0x0e44 {spawn id} (no EQEmu opcode; not sent) and
    // PlayerPath_FindSpawnByID(spawn, 1, 0): the path to the buyer; the button waits 15 seconds.
    auto& s = barter_search_;
    if (s.selected < 0 || s.selected >= static_cast<int>(s.lines.size())) return;
    const auto& l = s.lines[static_cast<std::size_t>(s.selected)];
    if (l.buyer_spawn_id == 0 || l.zone != zone_id()) return;
    s.found_buyer = l.buyer_spawn_id;
    send_zone(encode_find_market_spawn(l.buyer_spawn_id));
    auto it = world_.spawns.find(l.buyer_spawn_id);
    if (it != world_.spawns.end()) send_find_path(it->second, true);
    s.last_find_ms = static_cast<std::uint32_t>(now_ms());
}

void GameConnection::barter_search_set_hide(bool hide, bool clicked) {
    // The ctor reads HideBuyers into the button and +0x2c0; a click writes it (the caller) and sets +0x2bc to -1.
    barter_search_.hide_buyers = hide;
    if (clicked) barter_search_.found_buyer = 0xffffffffu;
}

// ---------------------------------------------------------------- barter: the packets

void GameConnection::handle_barter(const AppPacket& p) {
    // Handle_OP_Barter @0x4222d0.
    auto action = barter_action(p);
    if (!action) return;
    auto& w = barter_window_;
    switch (*action) {
    case kBuyerSearchResults:
        // CBarterWnd_AddSearchResultItem per entry, after the selections and editor reset (0x6313d0).
        if (auto items = decode_buyer_item_results(p)) {
            w.item_sel = w.inventory_sel = -1;
            barter_reset_editor();
            w.item_results = std::move(*items);
        }
        break;
    case kBuyerModeOn: {
        // 6058, the flag, the welcome text (0x13) and the enabled, active lines (OP_BuyerItems 6).
        w.buyer_mode = true;
        // Keep may have changed while the start request was in flight. Retire the session before publishing lines.
        for (const auto& line : w.lines)
            if (line.enabled && line.toggle)
                for (const auto& item : line.trade_items)
                    if (refuse_kept_id(item.item_id, "trade")) {
                        barter_start_stop();
                        return;
                    }
        barter_status(0, eq(strings_, 6058));
        send_zone(encode_buyer_welcome(w.welcome));
        std::vector<BuyLine> lines;
        for (const auto& l : w.lines)
            if (l.enabled && l.toggle) lines.push_back(l);
        send_zone(encode_buy_lines(lines));
        break;
    }
    case kBuyerModeOff:
        barter_status(0, eq(strings_, 6059));
        w.buyer_mode = false;
        break;
    case kBuyerRemoveItem:
        // 0x420fc0: the line emptied.
        if (auto slot = decode_remove_buy_line(p); slot && *slot < static_cast<std::uint32_t>(game::kBuyLineCount))
            w.lines[*slot] = BuyLine{};
        break;
    case kBuyerInspectBegin:
        // The answer to our inspect: approval 0 closes the merchant window; else it opens for that buyer.
        if (auto r = decode_barter_inspect_reply(p)) {
            log::info("barter: buyer {} answered {}", r->spawn_id, r->approval);
            if (r->approval == 0) {
                barter_merchant_ = game::BarterMerchantState{};
            } else if (barter_merchant_.buyer_id == 0 && world_.spawns.contains(r->spawn_id)) {
                barter_merchant_ = game::BarterMerchantState{};
                barter_merchant_.buyer_id = r->spawn_id;
                barter_merchant_.serial = ++npc_session_serial_;
            }
        }
        break;
    case kBuyerAppearance:
        // The spawn's buyer flag; the active merchant's window closes on any change of it.
        if (auto a = decode_buyer_appearance(p)) {
            auto it = world_.spawns.find(a->spawn_id);
            if (it != world_.spawns.end()) {
                it->second.buyer = a->enabled;
                if (a->spawn_id == barter_merchant_.buyer_id) barter_merchant_ = game::BarterMerchantState{};
            }
        }
        break;
    case kBuyerBrowsingBuyLine:
        if (auto name = decode_buyer_browsing(p)) barter_status(0, eq(strings_, 6060, {*name}));
        break;
    case kBuyerGreeting:
        // 6070 "%1 greets you, '%2'" with a text, else 788 "%1 greets you warmly." (colour 0x111).
        if (auto g = decode_buyer_greeting(p)) {
            if (!g->text.empty()) system(6070, {g->name, g->text}, 0x111);
            else system(788, {g->name}, 0x111);
        }
        break;
    case kBuyerInventoryFull:
        if (w.open) barter_status(0, "Your inventory is full!");
        break;
    case kBarterAddBuyer:
        if (auto b = decode_barter_add_buyer(p)) barter_search_.buyers[b->buyer_id] = *b;
        break;
    case kBarterRemoveBuyer:
        if (auto id = decode_barter_remove_buyer(p)) barter_search_.buyers.erase(*id);
        break;
    default:
        break;
    }
}

void GameConnection::handle_buyer_items(const AppPacket& p) {
    // Handle_OP_BuyerItems @0x422d60.
    auto action = barter_action(p);
    if (!action) return;
    auto& w = barter_window_;
    switch (*action) {
    case kBarterSearchAction: {
        // CBarterSearchWnd_HandleSearchResults @0x62cdd0: for the open window and the last search: 6063 when there are
        // 200 or more, 6064 with the count and the text, then the lines.
        auto& s = barter_search_;
        auto r = decode_barter_search_results(p);
        if (!r || !s.open || r->transaction_id != s.transaction_id) break;
        if (r->lines.size() > 199) system(6063, {}, 15);
        system(6064, {std::to_string(r->lines.size()), r->text});
        for (auto& l : r->lines) s.lines.push_back(std::move(l));
        break;
    }
    case kBuyerLinesText:
        // BarterBuyLines_ParseFromServer @0x422110: the lines stored by slot and saved.
        if (auto text = decode_buy_lines_text(p)) {
            auto lines = game::parse_buy_lines_text(*text);
            for (auto& [slot, l] : lines)
                if (slot < static_cast<std::uint32_t>(game::kBuyLineCount)) w.lines[slot] = std::move(l);
            if (!lines.empty()) barter_save_ini(-1);
        }
        break;
    case kBuyerModifyBuyLine:
        // 0x422970: our line stored at its slot.
        if (auto l = decode_buy_line(p); l && l->slot < static_cast<std::uint32_t>(game::kBuyLineCount)) w.lines[l->slot] = *l;
        break;
    case kBuyerSellItem: {
        // Our sale's answer: result 0 is Barter_ReportItemSold @0x421230 (the search window's line loses the count,
        // the price added to our coin, the cash register, the chat line in colour 0x114); 2 says nothing; the
        // others their failure text (colour 13). The pending sale ends (0x4232f0).
        auto r = decode_buyer_sale_result(p);
        if (!r) break;
        const auto& sale = r->sale;
        auto count = static_cast<std::int32_t>(sale.count);
        if (sale.result == kBarterSuccess) {
            barter_search_sold(barter_search_, sale.line, count);
            game::server_add_copper(coins_, static_cast<std::uint64_t>(sale.line.cost) * static_cast<std::uint64_t>(count));
            SoundCue c;
            c.kind = SoundCue::Kind::Wave;
            c.spell = kBarterSound;
            sound_cues_.push_back(c);
            chat(ChatEvent::Kind::System, game::barter_report("You've sold ", " to ", sale.line, sale.buyer_name, count), 0, 0x114);
        } else if (sale.result != kBarterNoResult) {
            std::string text;
            switch (sale.result) {
            case kBarterFailure: text = "Your transaction failed."; break;
            case kBarterPending:
                text = "Your transaction failed because you already have a transaction pending. Please wait a few "
                       "seconds and try again.";
                break;
            case kBarterOutOfDate:
                text = "Your transaction failed because your barter data is out of date.";
                text += sale.method == 0 ? " Please close and re-open the merchant window to get the latest barter data."
                                         : " Please run the search again to get the latest barter data.";
                break;
            case kBarterBuyerFull: text = "Your transaction failed because the buyer's inventory is full."; break;
            case kBarterSellerLacksItem:
                text = "Your transaction failed because you don't possess the specified quantity of the item you wish "
                       "to sell.";
                break;
            case kBarterNoKrono:
                text = "Your transaction failed because you don't have the required Krono quantity in your Krono slot.";
                break;
            case kBarterSameZone:
                text = "Your transaction failed because you must be in the same zone as the buyer when the buyline has "
                       "compensation items.";
                break;
            case kBarterKronoFailed: text = "Your transaction failed because the krono were not successfully transfered."; break;
            default: break;
            }
            chat(ChatEvent::Kind::System, text, 0, 13);
        }
        if (barter_merchant_.pending > 0) --barter_merchant_.pending;
        if (barter_search_.pending > 0) --barter_search_.pending;
        break;
    }
    case kBuyerBuyItem: {
        // Barter_ReportItemBought @0x421ae0: the price taken from our coin, the cash register, the line to the status
        // text and the log; the barter window's inventory rebuilt.
        auto r = decode_buyer_sale_result(p);
        if (!r) break;
        const auto& sale = r->sale;
        auto count = static_cast<std::int32_t>(sale.count);
        std::int64_t price = static_cast<std::int64_t>(sale.line.cost) * count;
        if (!game::take_money(coins_, price)) coins_ = {};
        SoundCue c;
        c.kind = SoundCue::Kind::Wave;
        c.spell = kBarterSound;
        sound_cues_.push_back(c);
        barter_status(0, game::barter_report("You've bought ", " from ", sale.line, sale.seller_name, count));
        if (w.open) w.inventory = game::barter_window_inventory(inventory_);
        break;
    }
    case kBuyerSendBuyLine:
        // CBarterMerchantWnd_UpdateBuyLine, while the merchant window is open.
        if (barter_merchant_.buyer_id != 0)
            if (auto l = decode_buy_line(p)) game::barter_merchant_update(barter_merchant_, *l);
        break;
    case kBarterBuyerList:
        if (auto list = decode_barter_buyer_list(p)) {
            barter_search_.buyers.clear();
            for (auto& b : *list) barter_search_.buyers[b.buyer_id] = std::move(b);
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- packets

void GameConnection::handle_market(Op op, const AppPacket& p) {
    switch (op) {
    case Op::Trader:
        if (auto m = decode_trader_message(p)) handle_trader_message(*m);
        break;
    case Op::BecomeTrader:
        // Handle_OP_BecomeTrader @0x4b6390.
        if (auto b = decode_become_trader(p)) {
            auto it = world_.spawns.find(b->spawn_id);
            if (it != world_.spawns.end()) it->second.trader = b->trader != 0;
        }
        break;
    case Op::TraderShop:
        if (auto m = decode_trader_shop(p)) {
            switch (m->action) {
            case kBazaarWelcome:
                if (game::is_bazaar_zone(zone_id())) system(6762);
                system(6763, {std::to_string(m->traders), std::to_string(m->items)});
                if (game::is_bazaar_zone(zone_id())) system(6764);
                break;
            case kTraderBuyItem:
                if (m->buy) handle_trader_purchase(*m->buy);
                break;
            case kBazaarAddTrader:
                bazaar_.traders[m->trader.trader_id] = m->trader;
                break;
            case kBazaarRemoveTrader:
                bazaar_.traders.erase(m->trader.trader_id);
                break;
            case kTraderClick:
                // A trader answered our click: on approval the shop opens (CMerchantWnd::Activate).
                if (m->click && trader_click_ == m->click->trader_id) {
                    trader_click_ = 0;
                    auto it = world_.spawns.find(m->click->trader_id);
                    if (m->click->approval && it != world_.spawns.end() && !merchant_) {
                        merchant_ = MerchantSession{};
                        merchant_->serial = ++npc_session_serial_;
                        merchant_check_ms_ = 0;
                        merchant_->npc_id = m->click->trader_id;
                        merchant_->name = game::display_name(it->second.name);
                        merchant_->open = true;
                        merchant_->trader = true;
                        merchant_->tabs = 1;
                    }
                }
                break;
            default:
                break;
            }
        }
        break;
    case Op::TraderBulkSend:
        if (auto v = decode_trader_bulk(p)) {
            bazaar_.traders.clear();
            for (auto& t : *v) bazaar_.traders[t.trader_id] = std::move(t);
        }
        break;
    case Op::BazaarSearch:
        // HandleSearchResults reads the u16 before anything else; EQEmu's delivery costs reply is not a valid
        // result list after it (its float lands in the count), so take it from the raw packet.
        if (p.data.size() >= 6) {
            bazaar_.voucher_cost =
                static_cast<std::uint16_t>(std::to_integer<unsigned>(p.data[4]) | std::to_integer<unsigned>(p.data[5]) << 8);
        }
        if (auto r = decode_bazaar_results(p)) {
            if (r->results.empty()) {
                bazaar_.searching = false; // the server's "done" (and the delivery costs)
                break;
            }
            for (auto& e : r->results) bazaar_.results.push_back(std::move(e));
        }
        break;
    case Op::Barter:
        handle_barter(p);
        break;
    case Op::BuyerItems:
        handle_buyer_items(p);
        break;
    case Op::TraderDelItem:
        // Handle_OP_TraderDelItem @0x4b7990 → 0x6ef450: the item leaves the trader's window.
        if (auto d = decode_trader_del_item(p); d && merchant_ && merchant_->trader) {
            for (auto it = merchant_->items.begin(); it != merchant_->items.end(); ++it)
                if (it->second.serial == d->serial) {
                    merchant_->items.erase(it);
                    break;
                }
        }
        break;
    default:
        break;
    }
}

void GameConnection::reset_market_for_zone() {
    trader_.window_open = false;
    trader_.trader = false;
    trader_.start_pending = false;
    trader_.slots.clear();
    trader_.selected = -1; // it indexed the slots just cleared
    trader_.customers.clear();
    // CBazaarSearchWnd::OnZone @0x633780 forgets the traders and the first opening; the window and its Hide Traders
    // state, the found trader and the find time stay (the window is hidden by the zone change).
    game::BazaarSearchState kept = std::move(bazaar_);
    bazaar_ = {};
    bazaar_.hide_traders = kept.hide_traders;
    bazaar_.found_trader = kept.found_trader;
    bazaar_.last_find_ms = kept.last_find_ms;
    bazaar_.find_sent = kept.find_sent;
    bazaar_search_open_ = false;
    trader_click_ = 0;
    search_pending_ = 0;
    // CBarterSearchWnd::OnZone @0x62b1e0: 0x17 goes out again and the buyers are forgotten; the merchant window and
    // buyer mode end with the zone.
    barter_merchant_ = {};
    barter_search_.first_open_sent = false;
    barter_search_.buyers.clear();
    barter_search_.lines.clear();
    barter_search_.pending = 0;
    barter_window_.buyer_mode = false;
    barter_window_.item_results.clear();
}

bool GameConnection::market_command(std::string_view h, const std::string& args) {
    (void)args;
    if (h == "Cmd_trader") {
        // Cmd_trader @0x4eab40: activates the bazaar window when it is not open.
        if (!trader_.window_open) open_trader_window();
        return true;
    }
    if (h == "Cmd_barter") {
        // Cmd_barter @0x4e6f30: toggles the barter search window.
        set_barter_search_open(!barter_search_.open);
        return true;
    }
    if (h == "Cmd_buyer") {
        // Cmd_buyer @0x4e6f60: GetGameFeature(0xf) == 0 prints GameFeatureRestrictions_FormatMessage(0xf) in colour
        // 0x14f; else it shows the barter window (CBarterWnd), which only opens where AboutToShow allows.
        if (!feature_refused(game::kFeatureBuyLines)) open_barter_window();
        return true;
    }
    if (h == "Cmd_marketplace") {
        // Cmd_marketplace @0x4ed930: Show(!shown).
        set_marketplace_open(!marketplace_.open);
        return true;
    }
    if (h == "Cmd_bazaar") {
        // Cmd_bazaar @0x4eab10: toggles the search window.
        if (bazaar_search_open_) close_bazaar_search();
        else open_bazaar_search();
        return true;
    }
    return false;
}

} // namespace hailstone::net
