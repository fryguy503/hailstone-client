#pragma once

// Developer merchant, trade, give and bank windows and the quantity dialog drawn with Dear ImGui until the
// stock UI engine (M5): what CMerchantWnd, CTradeWnd and CGiveWnd show
// (CMerchantWnd's child items and the
// functions named in net/game_connection_commerce.cpp). Like
// ui/item_windows.h they draw from game state and return what the player
// asked for; none of them does networking.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "game/bank.h"
#include "game/inventory.h"
#include "net/commerce.h"
#include "net/item_packets.h"

namespace hailstone::ui {

class ItemIconCache;

struct CommerceWindowsState {
    std::optional<std::uint32_t> selected_buy; // merchant slot
    int buy_quantity = 1;
    int sell_quantity = 1;
    std::optional<net::ItemSlot> sell_for; // the item sell_quantity was set for (HandleSell: start = the stack)
    int coin_type = 3;   // 0 copper .. 3 platinum
    int coin_amount = 0;
    // Bank window (CBankWnd).
    int bank_coin_type = 3;
    int bank_coin_amount = 0;
    std::set<std::int16_t> open_bank_bags; // bank slots whose bag is shown
    // Quantity dialog (CQuantityWnd): the slider value, reset per request.
    int quantity = 0;
    std::optional<net::ItemSlot> quantity_for;
};

struct CommerceWindowsInput {
    const game::Inventory* inventory = nullptr;
    net::Coins coins;
    bool holding_coin = false;
    const net::MerchantSession* merchant = nullptr;
    const net::TradeSession* trade = nullptr;
    // The give helper's target, set only when the spawn accepts direct hand-ins or trades.
    std::uint32_t target_id = 0;
    std::string target_name;
    // Bank (open when bank != null).
    const net::BankSession* bank = nullptr;
    game::BankCoins bank_coins;
    const net::QuantityRequest* quantity = nullptr;
};

struct CommerceWindowsActions {
    std::optional<std::pair<std::uint32_t, std::uint32_t>> buy;  // merchant slot, quantity
    std::optional<std::pair<net::ItemSlot, std::uint32_t>> sell; // inventory slot, quantity
    bool close_merchant = false;
    bool trade_drop = false;                                     // drop the cursor item into the trade
    bool trade_drop_coin = false;
    std::optional<std::pair<int, std::uint32_t>> trade_coin;     // coin type, amount
    bool trade_accept = false;
    bool trade_cancel = false;
    std::optional<std::uint32_t> give_to;                        // spawn id
    std::optional<net::ItemSlot> bank_click;                     // a bank or shared bank slot
    std::optional<std::pair<int, std::uint32_t>> deposit;        // coin type, amount
    std::optional<std::pair<int, std::uint32_t>> withdraw;
    bool shared_coin = false;                                    // deposit / withdraw on the shared bank
    bool auto_bank = false;
    bool close_bank = false;
    std::optional<std::uint32_t> quantity_accept;
    bool quantity_cancel = false;
};

// Copper as the stock windows show a price: "1p 2g 3s 4c" (zero parts left out; "0c").
std::string short_price(std::int64_t copper);

CommerceWindowsActions commerce_windows(CommerceWindowsState& state, const CommerceWindowsInput& in,
                                        ItemIconCache* icons);

} // namespace hailstone::ui
