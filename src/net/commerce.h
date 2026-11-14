#pragma once

// What the client keeps about an open merchant, an open trade and an open
// bank (GameConnection, net/game_connection_commerce.cpp and
// net/game_connection_bank.cpp). The original keeps the same in
// CMerchantWnd (pinstActiveMerchant, MerchantGreed, the item list),
// CTradeWnd (pinstTradeTarget, TradeItems, the coin fields and the two
// ready flags) and CBankWnd (pinstActiveBanker, BankSize); the quantity
// dialog is CQuantityWnd.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "net/item_packets.h"

namespace hailstone::net {

struct MerchantSession {
    std::uint64_t serial = 0;   // identifies this opening, including reopening the same NPC
    std::uint32_t npc_id = 0;
    std::string name;            // display name
    bool open = false;           // the server answered OP_ShopRequest with command 1
    float rate = 1.0f;           // CMerchantWnd::MerchantGreed: sell prices = value / rate
    std::int32_t tabs = 0;       // 1 buy/sell, 2 recover, 4 parcels
    bool trader = false;         // a player trader in the bazaar (OP_TraderShop): items keyed by arrival, see Item::serial
    std::map<std::uint32_t, Item> items; // the stock, by merchant slot
    std::optional<ItemSlot> selected; // an inventory item picked for selling
    int pending = 0;             // EverQuestinfo::IsItemPending: a buy or sell awaiting its answer
    // EQEmu's parcels waiting at a parcel merchant (tabs & 4), by parcel slot; `claiming`: the slots Claim all is still
    // to take (one request out at a time, `parcel_pending`).
    struct Parcel {
        Item item;
        std::uint32_t sent = 0; // the time it was sent (Unix seconds)
        std::string from, note;
    };
    std::map<std::uint32_t, Parcel> parcels;
    std::vector<std::uint32_t> claiming;
    bool parcel_pending = false;
    std::int64_t parcel_sent_ms = 0;
    bool parcel_sending = false; // a parcel sent (OP_ShopSendParcel) waits for its answer
    std::int64_t parcel_send_ms = 0;
    // The Parcels (mail) page and the inventory's coin buttons (0x698290, GameConnection::parcel_coin_button):
    // the coin button outlined (CInventoryWnd's button +0x259, 0 platinum .. 3 copper; -1 none; every SelectSlot
    // clears it), and the last money picked there for sending (Hailstone: it fills the page's money, see
    // set_parcel_money; `serial` counts the picks over the connection, so a pack sees each new one).
    int parcel_coin = -1;
    struct ParcelMoneyPick {
        int coin = 0; // 0 platinum .. 3 copper
        std::uint32_t amount = 0;
        std::uint32_t serial = 0;
    };
    std::optional<ParcelMoneyPick> parcel_money;
};

// Trade with a player (CTradeWnd, 8 slots a side) or an NPC (CGiveWnd,
// 4 slots, no partner side). Our offered items live in the inventory as
// type 3 (trade) slots, as the original's are in the trade container.
struct TradeSession {
    std::uint32_t partner_id = 0;
    std::string partner_name;
    bool with_npc = false;
    bool open = false;           // acknowledged: items and coin may be offered
    bool requested = false;      // our OP_TradeRequest is waiting for the Ack
    bool my_ready = false, their_ready = false;
    std::array<std::uint32_t, 4> my_coins{};    // by coin type: 0 copper .. 3 platinum
    std::array<std::uint32_t, 4> their_coins{};
    std::map<std::int16_t, Item> theirs;        // the partner's offer, by trade slot 0..7
    int slots() const { return with_npc ? 4 : 8; }
};

// The bank window (CBankWnd): open while pinstActiveBanker is set. The bank's
// items are in the inventory (types 1 and 2), its coin in
// GameConnection::bank_coins().
struct BankSession {
    std::uint64_t serial = 0;
    std::uint32_t banker_id = 0;
    std::string name;            // the banker's trimmed name (BW_BankerName)
    int slots = 16;              // CBankWnd::BankSize: 16 or 24 (game::bank_slot_count)
};

// CQuantityWnd opened by a click on a stack (CInvSlot::HandleLButtonUp):
// how many to pick up, 1..max; its answer goes to CInvSlot::SliderComplete.
// Or by CLootWnd::RequestLootSlot for a stack on someone else's corpse (`loot`): the answer completes the loot
// request (CLootWnd::WndNotification message 0x24).
struct QuantityRequest {
    ItemSlot slot;
    std::uint32_t max = 1;
    std::uint32_t start = 1; // the slider's starting value (CQuantityWnd::Open's `start` - min)
    bool loot = false;       // a corpse slot's loot request
    bool auto_loot = false;  // that request's auto loot
    bool centred = false;    // opened at (-1, -1): the dialog shows centred
};

} // namespace hailstone::net
