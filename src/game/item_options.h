#pragma once

// Where the Options window's item and purchase settings act (COptionsWnd::GeneralPageWndNotification @0x702640 sets
// them; game/client_options.h keeps them):
//  - Fast Item Destroy (Options Destroy, 0xde0bf4): CInventoryWnd::DestroyHeld @0x692340.
//  - Confirm Loot All (Options LootAllConfirm, 0xde0d28): CLootWnd::WndNotification @0x6be180's Loot All button.
//  - Loot No Drop (Options LootNoDrop, 0xde0d44; /lootnodrop): CLootWnd::LootAll @0x6bdba0 and
//    CLootWnd::RequestLootSlot @0x6be3a0; Cmd_lootnodrop @0x4f2700.
//  - Fast Drop (Options NoSafeDrop, 0xde0d40; /fastdrop): Cmd_fastdrop @0x4e5200 (the drop itself: game/cursor_drop.h).
//  - Fast AA / Adv / LA Purchase (Options AANoConfirm 0xde0d04, pointMerchantNoConfirm 0xde0d08,
//    LeadershipNoConfirm 0xde0d0c): the questions of CAAWnd::WndNotification @0x60aa80,
//    CPointMerchantWnd_ConfirmPurchase @0x71bba0, CNewPointMerchantWnd_ConfirmPurchase @0x6fce90 and
//    CLeadershipWnd::WndNotification @0x6b7bf0.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::game {

// ---------------------------------------------------------------- destroying the cursor's item or coin

inline constexpr std::uint32_t kStrDestroyItem = 2043, kStrDestroyMoney = 2061, kStrDestroyRealEstate = 5222;
// DestroyHeld's own (literal) refusal: ConfirmationDialog(0, text, OK only (1), 10 s).
inline constexpr const char* kDestroySatchelText = "Cannot destroy a trader satchel while in active trade mode.";
// The question: ConfirmationDialog(100, text, Yes / No (0xc), 10 s); a yes runs DestroyHeldItemOrMoney.
inline constexpr int kDestroyConfirmMs = 10000;

enum class DestroyDecision : std::uint8_t {
    Nothing,        // nothing held
    SatchelRefused, // a trader satchel (bag type 0x33) while the trader window is in trader mode
    DestroyNow,     // Fast Item Destroy: PcZoneClient::DestroyHeldItemOrMoney @0x5862f0 at once
    Confirm,        // ask `eqstr` first
};
struct DestroyInput {
    bool item = false;              // an item on the cursor
    bool money = false;             // coin on the cursor (CDisplay DragMoney)
    bool satchel_in_trader = false; // the item is a trader satchel and CBazaarWnd is active in trader mode
    bool real_estate = false;       // ItemBase_IsOrContainsRealEstateItem (no such items on this client's servers)
    bool fast_destroy = false;      // 0xde0bf4
};
struct DestroyResult {
    DestroyDecision decision = DestroyDecision::Nothing;
    std::uint32_t eqstr = 0; // Confirm: 2043 (with the item's name), 2061 (coin) or 5222 (real estate, always asked)
};
// CInventoryWnd::DestroyHeld @0x692340: the satchel refusal, then a real-estate item is always asked about (5222),
// then Fast Item Destroy destroys at once, else coin asks 2061 and an item 2043. (Its no-destroy refusal, "This item
// may not be destroyed.", needs ItemBase_IsNoDestroy, which RoF2's item data here does not carry.)
DestroyResult destroy_held_decision(const DestroyInput& in);

// Parse an item ID for the plugin-safe /destroycursor and /stowcursor commands. No signs, zero,
// trailing junk or overflow: malformed commands must never destroy what is held.
std::optional<std::uint32_t> destroy_cursor_item_id(std::string_view argument);

// PcZoneClient::DestroyHeldItemOrMoney's coin part: OP_MoveCoin {cursor 0, to -1, type, type, amount} for the highest
// coin on the cursor (wire types 3 platinum .. 0 copper), then every cursor coin cleared. nullopt: no coin held.
struct CoinDestroy {
    std::int32_t type = 0;
    std::int32_t amount = 0;
};
std::optional<CoinDestroy> destroy_cursor_coin(const std::uint32_t (&cursor_by_wire_type)[4]);

// ---------------------------------------------------------------- looting

inline constexpr std::uint32_t kStrLootAllConfirm = 11018; // "Loot all, are you sure?" (dialog 0x65, 20 s)
inline constexpr int kLootConfirmMs = 20000;
// The no-drop question (dialog 100, Yes / No, 20 s): sprintf'd, not from the string table.
std::string loot_no_drop_question(std::string_view item_name);

// The Loot All button (CLootWnd::WndNotification @0x6be180): asked first unless the corpse is the player's own (its
// +0xe4 owner name equals the player's) or LootAllConfirm is 0.
bool loot_all_asks(bool own_corpse, std::int32_t loot_all_confirm);

// The no-drop question of LootAll / RequestLootSlot: never on the player's own corpse; then only for an item that
// fails ItemClient::CanDrop(0, 1, 0, 1) (vtable +0x1c: not droppable, a bag by its contents too), and by LootNoDrop
// (out of 0..2 counts as 0): 0 always, 1 when CharacterZoneClient::CanUseItem(item, 0, 0) says no (`usable` is
// asked only then), 2 never.
bool loot_no_drop_asks(bool own_corpse, bool droppable, std::int32_t loot_no_drop, const std::function<bool()>& usable);

// RequestLootSlot's quantity question (after the no-drop one): on someone else's corpse, for a stack count above 1
// (CQuantityWnd max = start = the count; the player's own corpse sends the whole item at once).
inline bool loot_asks_quantity(bool own_corpse, std::int32_t stack_count) { return !own_corpse && stack_count > 1; }

// CLootWnd::LootAll(close) @0x6bdba0 over the corpse's slots: two passes, bags (item class 1) first, then the rest,
// each in slot order. An item that asks (loot_no_drop_asks) is left out and asked about, the window's popup flag
// (+0x398) set; a second one while the flag is set drops the whole list, leaving the first question open. With
// nothing to loot the window closes unless a question is showing; else the first request goes out (OP_LootItem,
// auto loot) and each answer sends the next. `ask` must be opened even when `aborted` is true.
struct LootAllItem {
    std::int16_t slot = 0;
    bool bag = false;
    bool asks = false;
};
struct LootAllPlan {
    std::vector<std::int16_t> queue;  // the requests, in order
    std::optional<std::int16_t> ask;  // the item asked about (its request has auto loot set)
    bool aborted = false;             // a second question: the automatic queue is discarded
    bool close = false;               // nothing to loot and no question: the window closes
    bool popup_pending = false;       // the flag afterwards
};
LootAllPlan plan_loot_all(const std::vector<LootAllItem>& items, bool popup_pending);

// ---------------------------------------------------------------- /fastdrop and /lootnodrop

inline constexpr std::uint32_t kStrFastDropOn = 13116, kStrFastDropOff = 13117, kStrFastDropNever = 13118,
                               kStrFastDropUsage = 13119;
// Cmd_fastdrop @0x4e5200: "on" (eqstr 12947) 1, "off" (12946) 0, "never" (12989) -1, compared whole and without
// case; anything else: nullopt (the caller says 13119).
std::optional<std::int32_t> fastdrop_choice(std::string_view args, std::string_view on, std::string_view off,
                                            std::string_view never);
// The line Cmd_fastdrop says for a setting (13116 / 13117 / 13118; colour 0x111).
std::uint32_t fastdrop_message(std::int32_t value);
// The options window's Fast Drop combo runs /fastdrop with the choice's word: 0 "off", 1 "on", 2 "never".
std::int32_t fastdrop_combo_value(int choice);

inline constexpr std::uint32_t kStrLootNoDropUsage = 12990, kStrLootNoDropCurrent = 12991, kStrLootNoDropAlways = 12992,
                               kStrLootNoDropNotUsable = 12993, kStrLootNoDropNever = 12994, kStrLootNoDropNew = 12995;
// Cmd_lootnodrop @0x4f2700 with an argument (compared on its first 4 characters without case): "never" (12989) or
// '2' first, then "sometimes" (12988), "usable" (13925) or '1', then "always" (12987) or '0'; nullopt otherwise.
std::optional<std::int32_t> lootnodrop_choice(std::string_view args, std::string_view never, std::string_view sometimes,
                                              std::string_view usable, std::string_view always);
// The setting's name after "New Setting: " (12995): 0 12992, 1 12993, 2 12994.
std::uint32_t lootnodrop_set_name(std::int32_t value);
// The name after "Your Current Setting: " (12991) without an argument: the original's swap, 0 12994 and 2 12992
// (1 12993).
std::uint32_t lootnodrop_shown_name(std::int32_t value);

// ---------------------------------------------------------------- purchase questions

inline constexpr int kPurchaseConfirmMs = 20000; // the AA and leadership questions (dialog 0x64, Yes / No)
// CAAWnd's train question: 4041 for a cost of 1, else 4042 (%1 the cost, %2 the ability's name).
std::uint32_t aa_train_question(std::int32_t cost);
// CLeadershipWnd's buy question: group abilities 8582 / 8583, raid ones 8587 / 8588 by whether the next rank costs
// 1 (%1 the cost, %2 the ability's name).
std::uint32_t leadership_buy_question(bool group, std::int32_t cost);
// CPointMerchantWnd_ConfirmPurchase (the adventure merchants, no time limit): the merchant's point system's question
// (its vtable +0x18(quantity, cost): @0x410830 LDoN, @0x5aa240 PvP, @0x460b80 charity, @0x54c8b0 radiant crystals,
// @0x548530 ebon crystals): quantity 1 the "purchase the %2" one, else the "%2 of the %3 at %4 each" one, plus 1 when
// the cost is not 1 (for several, so "points each" goes with a cost of 1: the original's wording). `type` is the
// merchant's (1 LDoN, 2 Discord / PvP, 3 charity, 4 Norrath's Keepers / radiant, 5 Dark Reign / ebon); 0 unknown.
std::uint32_t adventure_purchase_question(std::uint32_t type, std::int32_t quantity, std::int32_t cost);
// CNewPointMerchantWnd_ConfirmPurchase (alternate currency merchants, no time limit): 8064 for one ("%1 %2 to
// purchase the %3": the cost, the currency's singular name for a cost of 1 else its plural, the item), else 8065.
inline constexpr std::uint32_t kStrAltPurchaseOne = 8064, kStrAltPurchaseSeveral = 8065;

} // namespace hailstone::game
