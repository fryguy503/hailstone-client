#pragma once

// The bazaar: trader mode (OP_Trader, OP_BecomeTrader), shopping at a trader
// and the bazaar search (OP_TraderShop, OP_BazaarSearch, OP_TraderBulkSend,
// OP_TraderDelItem), and the barter hall's buyer packets (OP_Barter,
// OP_BuyerItems). RoF2 has no OP_TraderBuy of its own: purchases travel as
// OP_TraderShop action 10. Layouts follow the original's senders and readers
// (CBazaarWnd, CBazaarSearchWnd, CBazaarConfirmationWnd, CMerchantWnd and
// HandleWorldMessage_Bazaar @0x4ba870 in eqgame.exe) and the NMS
// server's EQEmu RoF2 translator (common/patches/rof2.cpp ENCODE/DECODE of
// these opcodes, rof2_structs.h RoF2BazaarTraderBuyerActions, zone/
// trading.cpp). Per-packet notes: docs/protocol/rof2-packets.md "Bazaar".

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net/opcodes.h"
#include "net/packet.h"

namespace hailstone::net {

// RoF2BazaarTraderBuyerActions (the first dword of OP_Trader / OP_TraderShop).
inline constexpr std::uint32_t kTraderOff = 0, kTraderBegin = 1, kTraderEnd = 2, kTraderPriceUpdate = 3,
                               kTraderEndTransaction = 4, kBazaarSearchAction = 7, kBazaarWelcome = 9,
                               kTraderBuyItem = 10, kTraderListItems = 11, kTraderCustomerBrowsing = 13,
                               kBazaarInspectAction = 18, kTraderItemMove = 19, kTraderReconcile = 20,
                               kTraderItemMessage = 21, kTraderAck2 = 22, kBazaarAddTrader = 24,
                               kBazaarRemoveTrader = 25, kBazaarFirstOpen = 26, kTraderClick = 28;

inline constexpr int kTraderSlots = 200;          // EQ::invtype::BAZAAR_SIZE
inline constexpr std::size_t kItemGuidSize = 18;  // EqItemGuid: 17 characters and a NUL

// ---------------------------------------------------------------- trader mode (OP_Trader)

// The start (Send_OP_Trader_00639730 @0x639730): 4404 bytes, action 1, then
// the 200 trader slots' item GUIDs (18 bytes each, the server's 16-digit
// serial numbers; unused slots are zero) and their prices in copper.
struct TraderBegin {
    std::array<std::string, kTraderSlots> serials;
    std::array<std::uint32_t, kTraderSlots> prices{};
};
inline constexpr std::size_t kTraderBeginSize = 4404;
AppPacket encode(const TraderBegin& v);
std::optional<TraderBegin> decode_trader_begin(const AppPacket& p);

// A 12-byte {action, 0, 0}: action 2 ends trader mode (Send_OP_Trader_00638030
// @0x638030), 11 asks for the trader's own listing (after TraderAck2).
AppPacket encode_trader_action(std::uint32_t action);

// The 32-byte price message (0x6377c0 / CBazaarWnd::WndNotification and
// Send_OP_Trader_00638d80): action 3 (a price change; 0 removes the item) or
// 20 (reconcile, sent for every slot after a sale), the item GUID, a dword
// (the item's count for action 20), the price.
struct TraderPrice {
    std::uint32_t action = kTraderPriceUpdate;
    std::string serial;
    std::uint32_t count = 0;
    std::uint32_t price = 0;
};
inline constexpr std::size_t kTraderPriceSize = 32;
AppPacket encode(const TraderPrice& v);
std::optional<TraderPrice> decode_trader_price(const AppPacket& p);

// TraderBuy_Struct (284 bytes): OP_TraderShop action 10 both ways (the server
// also sends it to the trader as OP_Trader 10 when something sold).
struct TraderBuy {
    std::uint32_t action = kTraderBuyItem;
    std::uint32_t method = 0;     // 0 at the trader, 1 by parcel, 2 straight to the inventory
    std::uint32_t sub_action = 0; // the answer: 0 success, 1 failed, 3 data out of date, ... (see HandleWorldMessage_Bazaar)
    std::uint32_t unknown012 = 0;
    std::uint32_t trader_id = 0;  // the trader's spawn id (search purchases: the character id)
    std::string buyer_name, seller_name, item_name, serial;
    std::uint32_t item_id = 0;
    std::uint32_t price = 0;      // each
    std::uint32_t already_sold = 0;
    std::uint32_t unknown276 = 0; // low word: vouchers
    std::uint32_t quantity = 0;
};
inline constexpr std::size_t kTraderBuySize = 284;
inline constexpr std::uint32_t kTraderBuyFailed = 1; // sub action 1: "Your attempt to purchase ... was unsuccessful."
// EQEmu's Client::TradeRequestFailed answers a refused purchase with the request itself (sub action 0, which the
// original reads as a success) and the trader id and already-sold count set to 0xffffffff; no answer of the original
// server's has both. Such an answer is read as sub action 1.
inline bool is_eqemu_trade_failure(const TraderBuy& v) {
    return v.sub_action == 0 && v.trader_id == 0xffffffffu && v.already_sold == 0xffffffffu;
}
AppPacket encode(const TraderBuy& v, Op op = Op::TraderShop);
std::optional<TraderBuy> decode_trader_buy(const AppPacket& p);

// What the server sends on OP_Trader (CBazaarWnd::HandleTraderMsg @0x63c450).
struct TraderMessage {
    std::uint32_t action = 0;
    std::uint32_t word1 = 0, word2 = 0;        // +4 / +8: sub action, entity id, ...
    std::vector<std::pair<std::string, std::uint32_t>> listing; // action 11: (GUID, price) for the 200 slots
    std::optional<TraderBuy> sale;             // action 10
    std::string name;                          // action 21: +4 char[64]
    std::uint32_t code21 = 0;                  // action 21: +0x44
};
std::optional<TraderMessage> decode_trader_message(const AppPacket& p);
// Server direction encoders, for tests.
AppPacket encode_trader_status(std::uint32_t action, std::uint32_t word1, std::uint32_t word2 = 0);
AppPacket encode_trader_listing(const std::vector<std::pair<std::string, std::uint32_t>>& listing);

// OP_BecomeTrader (8 bytes, RoF2 BecomeTrader_Struct): {spawn id, trader flag};
// Handle_OP_BecomeTrader @0x4b6390 sets the spawn's Trader field (+800).
struct BecomeTrader {
    std::uint32_t spawn_id = 0;
    std::uint32_t trader = 0;
};
AppPacket encode(const BecomeTrader& v);
std::optional<BecomeTrader> decode_become_trader(const AppPacket& p);

// ---------------------------------------------------------------- shopping (OP_TraderShop)

// RightClickedOnPlayer @0x536d80 on a trader: {28, spawn id, a byte 0 and three
// bytes the original leaves uninitialised} (12 bytes). The answer is the same
// shape with the approval in the third dword's low byte; on approval the
// trader's items follow as merchant item packets (0x64) and CMerchantWnd opens.
struct TraderClick {
    std::uint32_t action = kTraderClick;
    std::uint32_t trader_id = 0;
    std::uint32_t approval = 0;
};
AppPacket encode(const TraderClick& v);
std::optional<TraderClick> decode_trader_click(const AppPacket& p);
// CMerchantWnd::AboutToHide @0x6efe60 at a trader: OP_TraderShop with action 4 only (4 bytes).
AppPacket encode_trader_shop_end();
// A 12-byte {action, 0, 0} on OP_TraderShop: 26 the bazaar search window's
// first opening (CBazaarSearchWnd::AboutToShow), 9 its welcome button.
AppPacket encode_trader_shop_action(std::uint32_t action);

// The bazaar search (CBazaarSearchWnd::doQuery @0x632e40): OP_TraderShop
// action 7, 132 bytes. The defaults are what doQuery starts from (its
// combo boxes' first choice keeps them): 0xffffffff is "any" for class,
// race, stat, slot and type, as EQEmu reads it.
struct BazaarQuery {
    // The trader combo's item data: 0 all traders ("All Traders", eqstr 6755),
    // 0xffffffff this zone's traders (eqstr 783), else a trader's character id.
    // The packet's scope byte is 1 unless it is 0xffffffff.
    std::uint32_t trader = 0;
    std::uint32_t class_ = 0xffffffff;
    std::uint32_t race = 0xffffffff;
    std::uint32_t stat = 0xffffffff;
    std::uint32_t slot = 0xffffffff;
    std::uint32_t type = 0xffffffff;
    std::string name;
    std::uint32_t min_cost = 0, max_cost = 0; // platinum
    std::uint32_t min_level = 1, max_level = 100;
    std::uint32_t max_results = 200;
    std::uint32_t prestige = 0;
    std::uint32_t augment = 0;
};
inline constexpr std::size_t kBazaarQuerySize = 132;
AppPacket encode(const BazaarQuery& v);
std::optional<BazaarQuery> decode_bazaar_query(const AppPacket& p);

// The item inspect of a search result (Send_OP_TraderShop_00633520 @0x633520):
// OP_TraderShop action 18, 40 bytes: {18, 0, trader id, GUID, item id, flags
// (2 with shift)}. The server answers with the item (packet type 0, a link view).
struct BazaarInspect {
    std::uint32_t trader_id = 0;
    std::string serial;
    std::uint32_t item_id = 0;
    std::uint32_t flags = 0;
};
inline constexpr std::size_t kBazaarInspectSize = 40;
AppPacket encode(const BazaarInspect& v);
std::optional<BazaarInspect> decode_bazaar_inspect(const AppPacket& p);

// OP_TraderShop from the server (HandleWorldMessage_Bazaar case OP_TraderShop):
// 9 welcome (traders at +8, items at +12), 10 a purchase answer, 24 / 25 a
// trader joins / leaves the search window's list, 28 the click answer.
struct BazaarTrader {
    std::uint16_t zone_id = 0, zone_instance = 0;
    std::uint32_t trader_id = 0; // the character id
    std::uint32_t entity_id = 0; // the spawn id (0: another bazaar shard)
    std::string name;
};
struct TraderShopMessage {
    std::uint32_t action = 0;
    std::uint32_t traders = 0, items = 0; // 9
    std::optional<TraderBuy> buy;         // 10
    BazaarTrader trader;                  // 24 (and 25: trader_id only)
    std::optional<TraderClick> click;     // 28
};
std::optional<TraderShopMessage> decode_trader_shop(const AppPacket& p);
AppPacket encode_bazaar_welcome(std::uint32_t traders, std::uint32_t items); // server direction, for tests
AppPacket encode_bazaar_add_trader(const BazaarTrader& t);                   // server direction, for tests

// OP_TraderBulkSend (CBazaarSearchWnd_HandleTraderList @0x635890): u32 count,
// then {u16 zone, u16 instance, u32 character id, u32 spawn id, name}.
std::optional<std::vector<BazaarTrader>> decode_trader_bulk(const AppPacket& p);
AppPacket encode_trader_bulk(const std::vector<BazaarTrader>& v); // server direction, for tests

// OP_BazaarSearch results (CBazaarSearchWnd::HandleSearchResults @0x635530):
// u32 action, u16 zone, u32 trader, u32 count, then per item {u32 trader
// (character id), GUID string, u32 cost, u32 quantity, u32 item id, u32 icon,
// name, u32 the searched stat} (BazaarSearchResult_Unserialize @0x636b30). The
// server's "done" and delivery cost packets share the opcode and carry a
// non-positive count.
struct BazaarResult {
    std::uint32_t trader_id = 0;
    std::string serial;
    std::uint32_t cost = 0;
    std::uint32_t quantity = 0;
    std::uint32_t item_id = 0;
    std::uint32_t icon = 0;
    std::string name;
    std::uint32_t stat = 0;
};
struct BazaarResults {
    std::uint32_t action = 0;
    std::uint16_t zone_id = 0;
    std::uint32_t trader_id = 0;
    std::vector<BazaarResult> results;
};
std::optional<BazaarResults> decode_bazaar_results(const AppPacket& p);
// The u16 after the action, which HandleSearchResults reads before anything else: EQEmu's delivery costs reply
// is not a valid result list after it, so this reads it alone.
std::optional<std::uint16_t> decode_bazaar_voucher_cost(const AppPacket& p);
AppPacket encode(const BazaarResults& v); // server direction, for tests

// OP_TraderDelItem (29 bytes): {u32, u32 trader id, char[17] GUID, u32}; the
// item leaves the open trader window (0x6ef450).
struct TraderDelItem {
    std::uint32_t trader_id = 0;
    std::string serial;
};
std::optional<TraderDelItem> decode_trader_del_item(const AppPacket& p);
AppPacket encode(const TraderDelItem& v); // server direction, for tests

// Opcode 0x0e44 {u32 spawn id} (not in EQEmu's RoF2 table): CBazaarSearchWnd's Find Trader and CBarterSearchWnd's
// Find (@0x635990 / @0x62d810, 6 bytes with the opcode), before the find path to that spawn.
inline constexpr std::uint16_t kOpFindMarketSpawn = 0x0e44;
AppPacket encode_find_market_spawn(std::uint32_t spawn_id);

// ---------------------------------------------------------------- barter (buyers)
//
// The original's senders and readers: Handle_OP_Barter @0x4222d0, Handle_OP_BuyerItems @0x422d60, the barter
// manager's senders (Send_OP_Barter_00420eb0 / 00420ff0 / 00421470 / 00421d20, Send_OP_BuyerItems_004229a0 /
// 00422b70) and CBarterWnd, CBarterSearchWnd and CBarterMerchantWnd's, checked against EQEmu's RoF2 translator
// (rof2.cpp ENCODE / DECODE of OP_Barter and OP_BuyerItems, zone/trading.cpp). Where the two differ the note says so.

// RoF2BuyerActions (the first dword of OP_Barter / OP_BuyerItems), as Handle_OP_Barter / Handle_OP_BuyerItems
// switch on them.
inline constexpr std::uint32_t kBuyerSearchResults = 0x00, kBarterSearchAction = 0x01, kBuyerModeOn = 0x02,
                               kBuyerModeOff = 0x03, kBuyerLinesText = 0x05, kBuyerBuyLine = 0x06,
                               kBuyerModifyBuyLine = 0x07, kBuyerRemoveItem = 0x08, kBuyerSellItem = 0x09,
                               kBuyerBuyItem = 0x0a, kBuyerInspectBegin = 0x0b, kBuyerInspectEnd = 0x0c,
                               kBuyerAppearance = 0x0d, kBuyerSendBuyLine = 0x0e, kBuyerItemInspect = 0x0f,
                               kBuyerBrowsingBuyLine = 0x10, kBarterWelcomeMessage = 0x11,
                               kBuyerWelcomeMessage = 0x13, kBuyerGreeting = 0x14, kBarterItemInspect = 0x15,
                               kBuyerInventoryFull = 0x16, kBarterOpenWindow = 0x17, kBarterBuyerList = 0x19,
                               kBarterAddBuyer = 0x1a, kBarterRemoveBuyer = 0x1b;

// One buy line, the original's 0xb8-byte BarterBuyLine: written by BarterBuyLine_WriteToBuffer @0x7d7060 (u32 slot,
// u8 enabled; when enabled u32 item id, the name, u32 icon, u32 quantity, u8 toggle, u32 cost, u32 n, n × {u32 id,
// u32 quantity, u32 icon, name}, u32 buyer spawn id, u32 buyer character id, u32 zone, buyer name) and read back by
// 0x7d6ef0 (each field only while the packet holds it, so EQEmu's lines without the last four read as zeros; a
// line that is not enabled stops after the flag and keeps the defaults, icon 500).
struct BuyLineTrade {
    std::uint32_t item_id = 0, quantity = 0, icon = 0; // +0, +4, +8
    std::string name;                                  // +0xc char[64]
};
struct BuyLine {
    std::uint32_t slot = 0;          // +0x00 the buyer's line number (0..199)
    bool enabled = false;            // +0x04 the line is in use
    std::uint32_t item_id = 0;       // +0x08
    std::string item_name;           // +0x0c char[64]
    std::uint32_t icon = 500;        // +0x4c
    std::uint32_t quantity = 0;      // +0x50 how many the buyer still wants
    bool toggle = false;             // +0x54 active (the buyer buys with it)
    std::uint32_t cost = 0;          // +0x58 copper each
    std::vector<BuyLineTrade> trade_items; // +0x5c the compensation items, each per item bought (at most 10)
    std::uint32_t buyer_spawn_id = 0; // +0x6c (search results)
    std::uint32_t buyer_id = 0;       // +0x70 the buyer's character id
    std::uint32_t zone = 0;           // +0x74 the buyer's zone (EQEmu: u16 zone, u16 instance)
    std::string buyer_name;           // +0x78 char[64]
};
// The most compensation items a received buy line may list. The window adds at most 10 (kMaxCompensation); this is
// the bound the decoders keep a server's line to so it cannot claim billions.
inline constexpr std::uint32_t kMaxBuyLineTrades = 64;
inline constexpr std::uint32_t kKronoItemId = 88888; // 0x15b38: a Krono buy line (CBarterWnd_AddCompensationItem)

// RightClickedOnPlayer @0x536d80 on a buyer: OP_Barter {11, spawn id, one byte 0 and three uninitialised}
// (12 bytes). EQEmu echoes it with the approval at +8 (ShowBuyLines: 0 when the buyer is busy) and sends the
// buyer's lines as OP_BuyerItems 14. Handle_OP_Barter 11 opens the barter merchant window on a nonzero approval and
// closes it on 0. The window's close (Send_OP_Barter_00420eb0) sends {12, spawn id, 4 uninitialised bytes}.
AppPacket encode_barter_inspect(std::uint32_t action, std::uint32_t buyer_id);
struct BarterInspectReply {
    std::uint32_t spawn_id = 0;
    std::uint32_t approval = 0;
};
std::optional<BarterInspectReply> decode_barter_inspect_reply(const AppPacket& p);

// OP_BuyerItems 14 {14, u32, the line} (a viewed buyer's line, CBarterMerchantWnd_UpdateBuyLine) and 7 {7, u32,
// the line} (our own line, 0x422970). EQEmu writes 14 as {14, slot, slot, line} and 7 as {7, 0, line}; its
// removals {7 / 14, 0, slot, 0} read as a line that is not enabled.
std::optional<BuyLine> decode_buy_line(const AppPacket& p);
AppPacket encode_buy_line(std::uint32_t action, const BuyLine& v); // server direction (EQEmu's layout), for tests

// OP_Barter 13 BuyerAppearance (EQEmu Buyer_SetAppearance_Struct, 76 bytes: {13, spawn id, 64 B, u32 enabled}):
// Handle_OP_Barter reads the byte at +72 (the struct's `enabled`) into the spawn's buyer flag.
struct BuyerAppearance {
    std::uint32_t spawn_id = 0;
    bool enabled = false;
};
std::optional<BuyerAppearance> decode_buyer_appearance(const AppPacket& p);
AppPacket encode_buyer_appearance(const BuyerAppearance& v); // server direction, for tests

// The ways a sale to a buyer is made (EQEmu BarterPurchaseMethods): at the buyer (CBarterMerchantWnd), or from the
// barter search window with the buyer in this zone (1) or elsewhere (2): CBarterSearchWnd::sub_0062cf70 sets
// 1 + (the line's zone != the current zone).
inline constexpr std::uint32_t kBarterAtBuyer = 0, kBarterInBazaar = 1, kBarterOutsideBazaar = 2;

// A sale's record, the original's 0x178-byte struct (constructor 0x4232b0, BarterBuyLine_Serialize @0x62ad20
// writes it, 0x423500 reads it): six u32 (+0 method, +4 result, +8 buyer spawn id, +0xc buyer character id,
// +0x10, +0x14), three strings (+0x18 buyer [64], +0x58 item [32], +0x78 seller [64]), the buy line (+0xb8), u32
// +0x170 and u32 +0x174.
struct BuyerSale {
    std::uint32_t method = kBarterAtBuyer; // +0x00
    std::uint32_t result = 1;              // +0x04 (the constructor's 1)
    std::uint32_t buyer_spawn_id = 0;      // +0x08
    std::uint32_t buyer_id = 0;            // +0x0c
    std::string buyer_name, item_name, seller_name;
    BuyLine line;                          // +0xb8
    std::uint32_t quantity = 0;            // +0x170: the quantity sold (sent)
    std::uint32_t count = 0;               // +0x174: the quantity in a result (read)
};
// OP_BuyerItems 9 BuyerSellItem, client → zone: {u32 9, the record}. CBarterMerchantWnd::WndNotification (the
// quantity dialog's answer) fills method 0, buyer spawn id and the line copied from the merchant's list;
// CBarterSearchWnd::sub_0062cf70 method 1 / 2, the line's buyer spawn id (+0x6c) and character id (+0x70) and the
// line with its buyer fields. Both leave the strings empty and put the quantity at +0x170. EQEmu's DECODE reads the
// same bytes: method, 4 B, buyer spawn id, buyer character id, 11 B (+0x10, +0x14 and the three empty strings), the
// line (with method 0, 13 bytes for the buyer fields), the quantity, 4 B.
AppPacket encode(const BuyerSale& v);
// The record after the action (the original's 0x423500), for a sale and its result.
std::optional<BuyerSale> decode_buyer_sale(const AppPacket& p);
// OP_BuyerItems 9 / 10, zone → client (Handle_OP_BuyerItems): 9 our sale's answer, 10 a buy made by our buyer mode.
// Case 9 reads +4 as the result: 0 success (Barter_ReportItemSold with +0x174 items, buyer name +0x18), 1 failed,
// 2 nothing, 3 pending, 4 out of date (+0 method 0: reopen the merchant window, else search again), 5 the buyer's
// inventory is full, 6 the seller lacks the quantity, 7 / 9 Krono, 8 compensation needs the same zone. Case 10 is
// Barter_ReportItemBought (seller name +0x78). EQEmu writes {action, sub action (+0), error code (+4), 16 B, buyer,
// item, seller, a line with the item's name, cost and the compensation (item id 0xffffff, no icon or quantity),
// 0, 0, 0, "\xff\xff\xff", 0 (+0x170), the quantity (+0x174)}, so its error code is the original's result and its
// sub action (1 on any failure) the method.
inline constexpr std::uint32_t kBarterSuccess = 0, kBarterFailure = 1, kBarterNoResult = 2, kBarterPending = 3,
                               kBarterOutOfDate = 4, kBarterBuyerFull = 5, kBarterSellerLacksItem = 6,
                               kBarterNoKrono = 7, kBarterSameZone = 8, kBarterKronoFailed = 9;
struct BuyerSaleResult {
    std::uint32_t action = 0; // kBuyerSellItem / kBuyerBuyItem
    BuyerSale sale;
};
std::optional<BuyerSaleResult> decode_buyer_sale_result(const AppPacket& p);
// Server direction (EQEmu's ENCODE of Barter_SellerTransactionComplete / BuyerTransactionComplete), for tests.
AppPacket encode_buyer_sale_result(std::uint32_t action, std::uint32_t sub_action, std::uint32_t error,
                                   const std::string& buyer, const std::string& item, const std::string& seller,
                                   std::uint32_t cost, const std::vector<BuyLineTrade>& trade, std::uint32_t quantity);

// OP_Barter 1, the barter search (Send_OP_Barter_0062bc90, 84 bytes): u32 1, char[64] text, u32 transaction id (the
// __EQGetTime of the search), u32 0, u32 buyer character id (the player combo's choice; 0 all), u32 scope (1 for
// "Local Barterers"). EQEmu reads BarterSearchRequest_Struct (83 bytes, packed): the same up to the scope byte, then
// a u16 zone that the original's scope dword leaves 0.
struct BarterSearch {
    std::string text;
    std::uint32_t transaction_id = 0;
    std::uint32_t buyer_id = 0;
    bool local = false;
};
inline constexpr std::size_t kBarterSearchSize = 84;
AppPacket encode(const BarterSearch& v);

// OP_BuyerItems 1 (CBarterSearchWnd_HandleSearchResults @0x62cdd0): {u32 1, the search text, u32 transaction id,
// u32, u32, u8 (0x62e430), u32 count, count × the line with its buyer fields}. EQEmu writes the zone as u16 zone
// and u16 instance, which the original reads as the u32 at +0x74.
struct BarterSearchResults {
    std::string text;
    std::uint32_t transaction_id = 0;
    std::vector<BuyLine> lines;
};
std::optional<BarterSearchResults> decode_barter_search_results(const AppPacket& p);
AppPacket encode_barter_search_results(const BarterSearchResults& v); // server direction, for tests

// OP_Barter {2} / {3}, 4 bytes, both ways. Send_OP_Barter_00420ff0 asks for buyer mode on (2) or off (3) when it
// differs from the barter manager's flag (+0x8fc0); asking for off clears the flag at once. The server's {2}
// (Handle_OP_Barter 2) sets the flag, sends the welcome text (0x13) and the active lines (OP_BuyerItems 6); {3}
// clears it.
AppPacket encode_buyer_mode(bool on);
std::optional<bool> decode_buyer_mode(const AppPacket& p);

// OP_Barter {0, char[64] text} (68 bytes, Send_OP_Barter_00631750): the items whose name holds the text, for a new
// buy line. The answer (Handle_OP_Barter 0): {0, u32 count, count × 76 B {char[64] name, u32 item id, u32 icon,
// u32}} (CBarterWnd_AddSearchResultItem).
AppPacket encode_buyer_item_search(const std::string& text);
struct BuyerItemResult {
    std::string name;
    std::uint32_t item_id = 0, icon = 0;
};
std::optional<std::vector<BuyerItemResult>> decode_buyer_item_results(const AppPacket& p);
AppPacket encode_buyer_item_results(const std::vector<BuyerItemResult>& v); // server direction, for tests

// OP_BuyerItems 6 (Handle_OP_Barter 2): {u32 6, u16 count, count × line} with the lines that are enabled and
// active; 7 (Send_OP_BuyerItems_004229a0 / 00422b70): {u32 7, line}. Our lines carry zero buyer fields (13 bytes).
// The server answers each with OP_BuyerItems 7.
AppPacket encode_buy_lines(const std::vector<BuyLine>& lines);
AppPacket encode_modify_buy_line(const BuyLine& line);

// OP_Barter 8 (Send_OP_Barter_00421470, 8 bytes): {8, slot}. EQEmu reads the slot at +4 and echoes it as
// OP_BuyerItems {7, 0, slot, 0}, which empties our line; Handle_OP_Barter 8 {8, slot} empties it too.
AppPacket encode_remove_buy_line(std::uint32_t slot);
std::optional<std::uint32_t> decode_remove_buy_line(const AppPacket& p);

// The action word of an OP_Barter / OP_BuyerItems packet, or nullopt when it is shorter than one.
std::optional<std::uint32_t> barter_action(const AppPacket& p);

// OP_Barter 0x13 (Send_OP_Barter_00421d20 and Handle_OP_Barter 2, 328 bytes): {u32 0x13, u32 0, u8 0, 63 B, char[256]
// message at +0x48}; RoF2 BuyerWelcomeMessageUpdate_Struct has the message at +72 too.
AppPacket encode_buyer_welcome(const std::string& message);

// OP_Barter 0x11 (CBarterSearchWnd's Welcome, 4 bytes): the barter hall's welcome; EQEmu answers with a chat line.
// OP_Barter 0x17 (CBarterSearchWnd::AboutToShow, 4 bytes, once per zone): the buyers for the player combo; EQEmu
// answers with OP_Barter 0x1a per buyer.
AppPacket encode_barter_action(std::uint32_t action);
// OP_Barter 0x14 (CBarterSearchWnd's Greeting, 8 bytes): {0x14, the line's buyer character id}.
AppPacket encode_buyer_greeting(std::uint32_t buyer_id);

// OP_Barter 0x0f (32 bytes): inspect a buy line's item or one of its compensation items. Send_OP_Barter_0062b200 (the
// barter search window): {0x0f, 0, the buyer's spawn id when the line's zone is this one else its character id, the
// line's zone, the line's slot, the compensation index (-1 the item bought), the item id, u32 2 with Shift else 0}.
// CBarterMerchantWnd (a right click on DetailList): {0x0f, 0, the merchant's spawn id, the zone, the line's slot,
// the compensation index, its item id, 2 with Shift else 0}. EQEmu reads the item id at +24 and sends its link.
struct BarterItemInspect {
    std::uint32_t buyer = 0, zone = 0, slot = 0;
    std::int32_t index = -1;
    std::uint32_t item_id = 0;
    bool shift = false;
};
AppPacket encode(const BarterItemInspect& v);
// OP_Barter 0x15 (CBarterWnd, a right click on ItemSearchList, 12 bytes): {0x15, item id, 2 with Shift else 0}.
AppPacket encode_barter_search_item_inspect(std::uint32_t item_id, bool shift);

// OP_Barter 0x10 (Handle_OP_Barter: eqstr 6060 "%1 is browsing your Buy Lines."): {0x10, char[64] name}.
std::optional<std::string> decode_buyer_browsing(const AppPacket& p);
// OP_Barter 0x14 (Handle_OP_Barter: eqstr 6070 "%1 greets you, '%2'" with the text, else 788): name at +8, text at
// +0x48.
struct BuyerGreeting {
    std::string name, text;
};
std::optional<BuyerGreeting> decode_buyer_greeting(const AppPacket& p);

// The barter search window's buyers (CBarterSearchWnd's hash +0x220, keyed by character id): OP_Barter 0x1a {0x1a,
// u32 zone, u32 character id, u32 spawn id, char[64] name} adds one (0x62d620; EQEmu
// BuyerAddBuyertoBarterWindow_Struct), 0x1b {0x1b, character id} removes one (0x62d670), OP_BuyerItems 0x19
// {0x19, u32 count, count × {u32 zone, u32 character id, u32 spawn id, name}} replaces them all
// (CBarterSearchWnd_HandleBuyerList; EQEmu never sends it).
struct BarterBuyer {
    std::uint32_t zone = 0, buyer_id = 0, spawn_id = 0;
    std::string name;
};
std::optional<BarterBuyer> decode_barter_add_buyer(const AppPacket& p);
std::optional<std::uint32_t> decode_barter_remove_buyer(const AppPacket& p);
std::optional<std::vector<BarterBuyer>> decode_barter_buyer_list(const AppPacket& p);

// OP_BuyerItems 5 (BarterBuyLines_ParseFromServer @0x422110): {5, a string "<count>$<record>$..."}, each record the
// BART ini's line format (game/barter.h). EQEmu never sends it.
std::optional<std::string> decode_buy_lines_text(const AppPacket& p);

} // namespace hailstone::net
