#pragma once

// Merchant prices and money as the original client computes them
// (checked against the original's merchant value and profile money
// functions, addresses below).
//
// Buy prices are the server's: the price field of the merchant item's
// header (ItemBase_GetPrice @0x7b0a00), times the quantity. The client does
// no charisma, faction or greed maths on them. Sell prices are the client's:
// ItemBase::ValueSellMerchant @0x7bcf80 with the rate the server sent in
// OP_ShopRequest (CMerchantWnd::MerchantGreed). The server pays what it
// computes itself; the shown price is the original's estimate.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "net/item_packets.h"

namespace hailstone::game {

// BaseProfile_GetTotalMoneyInCopper: p × 1000 + g × 100 + s × 10 + c.
std::int64_t total_copper(const net::Coins& coins);

// PcClient_TakeMoney @0x5819f0: takes `amount` copper, smallest coins first
// (copper as far as it goes, then whole silver or gold or one coin broken
// into change one size down, then platinum by the thousand with one broken
// into gold). Returns false, taking nothing, for a negative amount or more
// than the player has.
bool take_money(net::Coins& coins, std::int64_t amount);

// CharacterZoneClient_CreditCopper (0x4488c0): adds
// `credit` copper (0x7e7040's capped credit; uncapped when the cap is -1,
// as on EQEmu) to the purse and re-splits the total into the fewest coins:
// p = T/1000, g = T%1000/100, s = T%100/10, c = T%10, signed 64-bit, each
// count stored as its low 32 bits. FinishSellingItem calls it on a sale.
void credit_copper(net::Coins& coins, std::int64_t credit);
// EQEmu's Client::AddMoneyToPP(copper) (zone/client.cpp), mirrored where the server adds coin without telling the
// client: the amount split into platinum, gold, silver and copper, each added to its own count (no change made).
void server_add_copper(net::Coins& coins, std::uint64_t copper);

// ItemBase::GetItemValue(this, true) @0x7ba920: the item's value (body
// value, copper) plus, for a common item (item class 0), its contents'
// (augments') values.
std::int32_t item_value(const net::Item& item);

// ItemBase_IsDroppable(recursive, no_real_estate) @0x7b3c40: tradeable (NoDrop != 0), not
// attuned, no ornamentation (models, icon, hero's forge model), no armour
// type or tint override; with no_real_estate not a real estate item (+0x7c RealEstateID -1); and every content
// too (with the same no_real_estate) when recursive.
bool is_droppable(const net::Item& item, bool recursive = true, bool no_real_estate = false);

// ItemClient::CanDrop(message, contents, _, no_real_estate) @0x554e50 through ItemBase_CanDropImpl @0x7b5090. Under
// the FV rules (0xddfd34 set, or the rule set 0xddfd2c 6) ItemBase_IsFVNoDrop @0x7b3bc0 (the item's FV no-drop
// byte +0x54c, a real estate item with no_real_estate, a content's) decides instead of IsDroppable. The answer:
// 0 droppable; 1 no trade (12921); with contents, an item droppable itself but not its contents 2 for a bag
// (definition +0x11a ItemClass 1: 12922) and 3 otherwise (an augment: 5293). CanDrop shows the line (colour 15,
// the item's name) when asked to and returns droppable.
enum class CanDropResult : std::uint8_t { Droppable, NoTrade, BagContentNoTrade, AugmentNoTrade };
CanDropResult can_drop(const net::Item& item, bool fv_rules, bool contents, bool no_real_estate);
std::uint32_t can_drop_message(CanDropResult r); // 12921 / 12922 / 5293, 0 for Droppable

// ItemBase::ValueSellMerchant @0x7bcf80: the copper a merchant pays for
// `count` of the item at `rate`; 0 when the item cannot be sold (temporary,
// no trade, attuned, ornamented, a non-droppable content, value <= 0 or a
// rate of 0): (int)(value / rate + 0.5) × count, rounded to whole gold at
// 10,000,000 copper and more. Integer maths wraps like the original's.
std::int32_t sell_value(const net::Item& item, float rate, std::int32_t count);

// FormatMoneyWords @0x4dc5f0: copper as " 1 platinum 2 gold 3 silver 4
// copper" (eqstr 2409..2412, each with a leading space; empty parts left
// out, so 0 gives "").
std::string money_words(std::int64_t copper);

// The client's checks before it offers an item to a merchant
// (CMerchantWnd::PurchasePageHandler::RequestPutItem @0x6f6650): the eqstr
// the merchant answers with, or 0 when the item can be offered.
// 12062 not droppable ("I am not interested in buying that."), 12064 a bag
// that is not empty.
std::uint32_t sell_refusal(const net::Item& item);

// CMerchantWnd::PurchasePageHandler::UpdateList @0x6f0b70's Qty and Lvl columns: the merchant quantity
// (ItemBase_GetMerchantQuantity, the header's merchant count) below 1 is "--"; the level below 2 is "--". The level is
// ItemBase_GetRequiredLevel(1) (the item's and its augments' highest), or for a scroll (item type 20) -1 unless the
// spell exists and the scroll's classes take the character's class, then that class's level for the spell (0xff:
// -1). `spell_level` answers the last part (nullopt: -1).
std::string merchant_quantity_text(std::int32_t quantity);
std::int32_t merchant_list_level(const net::Item& item, const std::function<std::optional<std::int32_t>()>& spell_level);
std::string merchant_level_text(std::int32_t level);

// The purchase list's "Show only items I can use" (MW_UsableButton; UpdateList @0x6f0bb0 with PurchasePageHandler
// +0x60 clear): a common item (item class 0; bags and books always stay) is left out unless its class mask takes
// `class_id` and its race mask the race's bit (0x6f7160, the same bits as game::item_race_bit_of), and a spell
// scroll (item type 20) is left out when `scroll_known` says the character knows its spell or a higher rank of it.
// The level, deity and lore are not looked at. Hailstone extension: a nonzero `server_classes`
// (GameConnection::server_class_mask, a multiclass server's classes of the player) takes the place of `class_id`, and
// the item is kept when its class mask has any of them.
bool merchant_usable_hides(const net::Item& item, std::uint32_t class_id, std::uint32_t race,
                           const std::function<bool()>& scroll_known, std::uint32_t server_classes = 0);

} // namespace hailstone::game
