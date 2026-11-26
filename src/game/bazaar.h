#pragma once

// The bazaar's client state: the trader window (CBazaarWnd: the 200 trader
// slots filled from the Trader's Satchels, their prices from the
// BZR_<name>_<server>.ini file, trader mode), the bazaar search window
// (CBazaarSearchWnd: the trader list and the results) and the window's text
// log. GameConnection (net/game_connection_market.cpp) sends and receives;
// the rules here are the original's (addresses below).

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/ini_file.h"
#include "game/inventory.h"
#include "net/item_packets.h"
#include "net/market_packets.h"

namespace hailstone::game {

// EQWorldData_IsBazaarZone @0x7dc6c0: the zone table's flag 0x10000000, which
// only The Bazaar (151) has.
inline constexpr std::uint32_t kBazaarZoneId = 151;
// The traders and search result rows kept: well above a full bazaar zone and EQEmu's Bazaar:MaxSearchResults
// (200 by default). What the original holds is not known (docs/unknowns.md); past these, new ones are dropped.
inline constexpr std::size_t kBazaarTradersKept = 4096;
inline constexpr std::size_t kBazaarResultsKept = 4096;
inline bool is_bazaar_zone(std::uint32_t zone_id) {
    if (zone_id > 1000) zone_id &= 0x7fff;
    return zone_id == kBazaarZoneId;
}
inline constexpr std::uint8_t kBagTraderSatchel = 51; // ItemBase_GetContainerType 0x33

// 0x5101b0 (the trader spot test of 0x4431a0): the switch's type (EQSwitch +0x26) is 0x99 or 0x9b, the
// bazaar's trader stalls (0x9a is not one).
inline bool is_trader_stall(std::uint8_t switch_type) { return switch_type == 0x99 || switch_type == 0x9b; }

// The [ItemToSell] key of an item in the BZR ini (CBazaarWnd_ReadTraderItemFromIni
// @0x63a6c0): the item id, or "<id>:<charges>" for an item with a click
// spell and at least one charge.
std::string trader_price_key(const net::Item& item);

// The prices file. CBazaarWnd::CreateBZRIniFilename @0x6380f0 names it
// ".\BZR_<character>_<server>.ini"; this client keeps it as BZR.ini in the character's folder (core/character_files.h).
class TraderPrices {
public:
    void load(const std::filesystem::path& path);
    bool save() const;
    // The stored price in copper; nullopt when none (or "*", the original's "forget").
    std::optional<std::uint32_t> get(const net::Item& item) const;
    void set(const net::Item& item, std::uint32_t copper);
    const std::filesystem::path& path() const { return path_; }

private:
    IniFile ini_;
    std::filesystem::path path_;
};

// One of the 200 trader slots (CBazaarWnd +0x5c8 prices, +0x8e8 active flags).
struct TraderSlot {
    net::ItemSlot where;      // the satchel slot the item sits in
    std::string serial;       // its GUID (the server's serial number)
    std::uint32_t item_id = 0;
    std::string name;
    std::uint32_t icon = 0;
    std::uint32_t count = 1;  // the stack count
    std::uint32_t price = 0;  // copper; 0 = not for sale
    bool active = false;      // listed with the server (bItemActive)
};

// A line of the bazaar window's text (CBazaarWnd::AddBazaarText: an STML
// colour from the chat colour index, then the text).
struct BazaarLine {
    std::uint32_t color = 0; // chat colour index (10, 13, 15)
    std::string text;
};

struct TraderState {
    bool window_open = false;        // CBazaarWnd shown (it only opens in the bazaar)
    bool trader = false;             // bIsMerchant: trader mode is on
    bool start_pending = false;      // 0xf70d70: an OP_Trader start is waiting for TraderAck2
    std::vector<TraderSlot> slots;   // in satchel order, at most 200
    int selected = -1;               // selectedSlot
    std::uint32_t entered_price = 0; // curItemPrice: the money buttons, in copper
    std::vector<BazaarLine> log;     // the window's text (bazaarText)
    std::vector<std::string> customers; // "is browsing your wares" (eqstr 6745) names
};

// The bazaar search window (CBazaarSearchWnd).
struct BazaarSearchState {
    std::map<std::uint32_t, net::BazaarTrader> traders; // by character id (Traders hash)
    std::vector<net::BazaarResult> results;
    // UnknownPadding[0x30]: the u16 after the action of every OP_BazaarSearch reply (HandleSearchResults @0x635530),
    // shown by the purchase window as the vouchers "To Me" costs (BZC_TokenLabel "x %u"). EQEmu sends its
    // Bazaar:VoucherDeliveryCost there (SendBazaarDeliveryCosts) but its search results put the trader's zone id
    // in the same place (docs/unknowns.md).
    std::uint16_t voucher_cost = 0;
    bool first_open_sent = false; // UnknownPadding[0x33]: action 26 is sent once per zone
    bool searching = false;       // a query went out and its "done" has not come back
    // Find Trader and Hide Traders (CBazaarSearchWnd::WndNotification @0x635990): the trader Find Trader last found
    // (+0x24c, the ctor's -1, a Hide Traders click's 0), when (+0x240, __EQGetTime), and the button's checked state
    // (+0x9244, from the UI ini's HideTraders). PlayerZoneClient::IsInvisible hides the other traders while it is on.
    std::uint32_t found_trader = 0xffffffffu;
    std::uint32_t last_find_ms = 0;
    bool find_sent = false; // a find went out (the 15 s wait applies)
    bool hide_traders = false;
};

// CBazaarWnd_LoadTraderItems @0x63bcf0: the items in Trader's Satchels (bag
// type 51) in the general slots 23..32, in slot order, at most 200; each gets
// its stored price. Returns the eqstr lines the window prints (6746 with the
// count and 6747, or 6748 / 6749).
struct TraderLoad {
    std::vector<TraderSlot> slots;
    bool has_satchel = false;
};
TraderLoad load_trader_items(const Inventory& inventory, const TraderPrices& prices);

// CBazaarWnd::SelectBazaarSlotItem @0x6393e0 (from its disassembly): with an item in `slot` the first slot holding
// the same item (the same GUID, or the same item id and ItemBase +0x44) is selected and its price loaded; else none.
struct TraderSelection {
    int selected = -1;
    std::uint32_t price = 0;
};
TraderSelection select_trader_slot(const std::vector<TraderSlot>& slots, const Inventory& inventory, int slot);

// The price buttons (CBazaarWnd::UpdatePriceButtons @0x637b10 / GetPrice
// @0x637ec0): copper split into platinum, gold, silver and copper; the
// platinum button holds the thousands.
std::array<std::uint32_t, 4> split_price(std::uint32_t copper);
std::uint32_t join_price(std::uint32_t platinum, std::uint32_t gold, std::uint32_t silver, std::uint32_t copper);

// The most each price button's amount dialog takes. The original's platinum
// button (sub_006386a0) stops at 1,000,000; Hailstone's goes up to the largest
// price the 32-bit copper fields of OP_Trader carry (4,294,967,295 copper), a
// deliberate difference so that the whole range the server stores can be used.
// Gold, silver and copper keep the original's 100.
inline constexpr std::int32_t kTraderMaxPlatinum = 4294967;
inline constexpr std::int32_t kTraderMaxSmallCoin = 100;
inline constexpr std::int32_t trader_coin_max(int coin) { return coin == 0 ? kTraderMaxPlatinum : kTraderMaxSmallCoin; }

// FormatCoinStringCompact @0x4ee020: "12p 3g 4s 5c" (zero parts left out, "0c" for nothing).
std::string coin_compact(std::int64_t copper);

} // namespace hailstone::game
