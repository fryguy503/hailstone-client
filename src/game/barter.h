#pragma once

// The barter hall: the original's barter manager (the 200 buy lines at 0xb79320, the buyer mode flag +0x8fc0, the
// welcome text +0x8fc4, the BART ini) and the state of its three windows, CBarterWnd (/buyer: our own buy lines),
// CBarterSearchWnd (/barter: other buyers' lines) and CBarterMerchantWnd (a buyer's lines, from a right click).
// Follows the original's 0x420e10..0x423850, CBarterWnd, CBarterSearchWnd, CBarterMerchantWnd and the
// CBarter*Wnd_* helpers at 0x629c00..0x6318f0; addresses at each function. The packets
// are net/market_packets.h's, the rules that send them net/game_connection_market.cpp's.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/item_packets.h"
#include "net/market_packets.h"

namespace hailstone {
class IniFile;
}

namespace hailstone::game {

class Inventory;

// EQWorldData_IsBarterZone: the zone table's flag 0x2000000, which The Bazaar (151) and The Barter Hall (346) have.
inline bool is_barter_zone(std::uint32_t zone_id) {
    if (zone_id > 1000) zone_id &= 0x7fff;
    return zone_id == 151 || zone_id == 346;
}

inline constexpr int kBuyLineCount = 200;     // the barter manager's table
inline constexpr int kMaxCompensation = 10;   // CBarterWnd_AddCompensationItem: no 11th item
inline constexpr int kMaxSaleQuantity = 20;   // the quantity dialog's limit for a sale or a compensation item
inline constexpr std::uint32_t kMaxBuyPrice = 1000000000; // CBarterWnd_SetPriceCoin's cap (copper)
inline constexpr std::int32_t kMaxBuyCount = 999;          // the CountEdit's clamp

// BarterBuyLine_FormatForIni @0x7d6820: "*2^<slot>^<enabled>^" and, for an enabled line, "<active>^<item id>^<name>^
// <icon>^<price>^<quantity>^<n>" and "^<id>^<quantity>^<name>^<icon>" per compensation item.
std::string buy_line_ini(const net::BuyLine& line);
// BarterBuyLine_ParseFromString @0x7d6b50: the line reset, then an optional "*<version>^"; version 2 gives the slot
// and the enabled flag, version 1 (or none) an enabled line without a slot.
net::BuyLine parse_buy_line_ini(std::string_view text);
// BarterBuyLines_ParseFromServer @0x422110 (OP_BuyerItems 5): "<count>$<record>$...", each record a line as above
// stored at its own slot. Returns the (slot, line) pairs.
std::vector<std::pair<std::uint32_t, net::BuyLine>> parse_buy_lines_text(std::string_view text);

// Barter_FormatPriceText @0x4210b0: FormatCoinStringCompact(price, no leading, a trailing space), then "+ items" when
// the line offers compensation items.
std::string buy_line_price_text(const net::BuyLine& line);
// CBarterWnd_SetPriceCoin @0x630750: the quantity dialog's answer `v` for the platinum (0), gold, silver or copper
// (3) button, capped at 1,000,000,000.
std::uint32_t barter_set_price_coin(std::uint32_t price, int coin, std::uint32_t v);
// "<count> <item>" and the compensation items of Barter_ReportItemBought @0x421ae0 / Barter_ReportItemSold @0x421230:
// "<verb><count> <item><preposition><who> for <coin> [and the following: <n x count> <name>, ...]."
std::string barter_report(std::string_view verb, std::string_view preposition, const net::BuyLine& line,
                          std::string_view who, std::int32_t count);

// One row of the barter windows' inventory lists (0x50 bytes: item id, charges, count, icon, name).
struct BarterInventoryItem {
    std::uint32_t item_id = 0;
    std::uint32_t charges = 0; // +4: the charges of an item with a click spell
    std::uint32_t count = 0;   // +8: summed over the items of this id
    std::uint32_t icon = 0;    // +0xc
    std::string name;          // +0x10
};
// The item check both lists use (ItemBase @0x7b22d0): not a container, nothing inside, tradeable (not NO TRADE, not
// attuned), and a click item with all its charges.
bool barter_item_eligible(const net::Item& item);
// CBarterWnd_BuildInventoryItems @0x6315e0: the general slots (23..32) and their bags' contents, Krono left out
// (0x631240), each item id once with the counts summed.
std::vector<BarterInventoryItem> barter_window_inventory(const Inventory& inventory);
// CBarterSearchWnd_BuildInventoryItems @0x62d6a0: Krono first (when the character has some), then slots 0..33 and
// their bags' contents (0x62cb50).
std::vector<BarterInventoryItem> barter_search_inventory(const Inventory& inventory, std::uint32_t krono);
// MultipleItemMoveManager_CollectItemsByID(pc, id, count, 0x100): whether the general slots and bags hold `count` of
// the item (the eligible ones).
bool barter_has_items(const Inventory& inventory, std::uint32_t item_id, std::uint32_t count);

// A status text line (CBarterWnd_AppendStatusText @0x62eb90: "<c colour>text</c><br>").
struct BarterStatusLine {
    std::uint32_t color = 0; // chat colour index
    std::string text;
};

// CBarterWnd and the barter manager.
struct BarterWindowState {
    bool open = false;
    bool buyer_mode = false;                                // manager +0x8fc0 (0xb822e0)
    std::array<net::BuyLine, kBuyLineCount> lines;          // manager +0 (0xb79320), 0xb8 bytes each
    std::string welcome;                                    // manager +0x8fc4
    std::vector<BarterStatusLine> status;                   // StatusText (+0x220), 200 lines kept
    std::vector<net::BuyerItemResult> item_results;         // SearchedItems: the item search's answer
    std::vector<BarterInventoryItem> inventory;             // +0x298
    net::BuyLine editor;                                    // CurrentBuyLine (+0x2d0): the line being made
    int item_sel = -1, inventory_sel = -1, compensation_sel = -1, line_sel = -1; // the lists' selections
    std::uint32_t last_search = 0;                          // +0x290: Util::FastTime of the last item search
    bool welcome_changed = false;                           // UpdateWelcomeButton enabled

    // BarterBuyLines_FindFreeSlot @0x420e90: the first line without a name, or -1.
    int free_slot() const;
};

// CBarterWnd_UpdateStartStopButton @0x62e8f0: the buttons' states from the selections, the buyer mode and the time
// (Util::FastTime, seconds).
struct BarterWindowButtons {
    bool search = false;               // ItemSearchButton: 5 seconds after the last search
    bool inventory_add = false, inventory_remove = false;
    bool create_shown = false, create = false; // BuyLineAddButton: shown without a buy line selected
    bool modify_shown = false, modify = false; // BuyLineModifyButton: shown with one
    bool remove = false, toggle = false;
    bool offline = false;              // OfflineModeButton: in buyer mode
    std::string start_stop = "Start Barter";
    std::uint32_t start_stop_color = 0xff00ff00; // "Stop Barter" 0xffff0000
};
BarterWindowButtons barter_window_buttons(const BarterWindowState& s, std::uint32_t now_seconds);

// CBarterMerchantWnd: the buyer we look at.
struct BarterMerchantState {
    std::uint64_t serial = 0;
    std::uint32_t buyer_id = 0;         // +0x260: the buyer's spawn id while the window is open (pinstActiveMerchant)
    std::vector<net::BuyLine> lines;    // +0x23c, in the order the server sent them
    std::uint32_t selected_slot = 0xffffffff; // +0x238: the selected line's slot
    int pending = 0;                    // its share of EverQuestinfo::IsItemPending (a sale awaiting its answer)
    std::int64_t checked_ms = 0;        // OnProcessFrame's 300 ms refresh
    bool sell_enabled = false;          // SellButton (sub_0062a3d0)
};
// CBarterMerchantWnd_UpdateBuyLine @0x62a2a0: a line from OP_BuyerItems 14 replaces the line of its slot, or removes
// it when it is not active, or is added.
void barter_merchant_update(BarterMerchantState& m, const net::BuyLine& line);

// CBarterSearchWnd.
struct BarterSearchState {
    bool open = false;
    bool first_open_sent = false;              // +0x2c1: OP_Barter 0x17 went out in this zone
    std::vector<net::BuyLine> lines;           // BuyLines (+0x278): the matches with their buyers
    std::vector<BarterInventoryItem> inventory; // InventoryItems
    std::map<std::uint32_t, net::BarterBuyer> buyers; // the hash at +0x220, by character id
    std::string text;                          // ItemSearchEdit
    int combo = 0;                             // PlayerCombo: 0 all, 1 local, 2.. the buyers by name
    int selected = -1;                         // the selected line (BuyLines index, +0x2a4)
    int detail_sel = -1;                       // DetailsList's selection
    int sale_line = -1;                        // +0x2b8: the line the quantity dialog sells to (0..199)
    std::uint32_t transaction_id = 0;          // +0x2b4: the last search's __EQGetTime
    std::uint32_t last_search_ms = 0;          // +0x2b0
    std::uint32_t last_find_ms = 0;            // the Find button's time stamp
    bool hide_buyers = false;                  // +0x2c0: HideButton (UI ini BarterSearchWnd HideBuyers)
    // +0x2bc: the buyer Find last found (the ctor's, a Hide click's and AboutToHide's -1); PlayerZoneClient::IsInvisible
    // hides the other buyers while hide_buyers is on.
    std::uint32_t found_buyer = 0xffffffffu;
    int pending = 0;                           // a sale awaiting its answer

    // CBarterSearchWnd_FillPlayerCombo @0x62d320: "All Barterers", "Local Barterers", then the buyers by name; each
    // choice's data (0, -1, the character id).
    std::vector<std::pair<std::string, std::int64_t>> combo_choices() const;
};
// 0x62b9f0 (Barter_ReportItemSold): the matching line (same buyer character id and slot) loses `sold`.
void barter_search_sold(BarterSearchState& s, const net::BuyLine& line, std::int32_t sold);

} // namespace hailstone::game
