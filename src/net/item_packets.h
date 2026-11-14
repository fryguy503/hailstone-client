#pragma once

// RoF2 items on the wire: the recursive item serialization the server sends
// in OP_CharInventory, OP_ItemPacket and OP_ItemLinkResponse, the
// InventorySlot_Struct addressing, and the inventory, loot and item-link
// packets. Layouts follow the NMS server's EQEmu RoF2 translator
// (common/patches/rof2.cpp SerializeItem, ENCODE(OP_CharInventory),
// ENCODE(OP_ItemPacket), ENCODE/DECODE(OP_MoveItem), (OP_LootItem),
// DECODE(OP_ItemLinkClick); rof2_structs.h, all #pragma pack(1)) field by
// field; nothing is read by casting a struct. The client side of each packet
// follows the original client. Per-packet notes are in
// docs/protocol/rof2-packets.md section "Items".
//
// OpenEQ crates/openeq-net/src/inventory.rs (MIT OR Apache-2.0) was used to
// cross-check the block sizes (THIRD_PARTY.md); the code follows rof2.cpp.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/byte_io.h"
#include "net/packet.h"

namespace hailstone::net {

// ---------------------------------------------------------------- slots

// InventorySlot_Struct (12 bytes): the client's ItemGlobalIndex copied as is:
// i16 type (ItemContainerInstance),
// i16 unknown, i16 slot, i16 bag sub-slot (-1 = none), i16 augment (-1),
// i16 unknown.
struct ItemSlot {
    std::int16_t type = 0;  // kInv* below
    std::int16_t slot = -1;
    std::int16_t sub = -1;  // index inside the bag in `slot`, -1 for the slot itself
    std::int16_t aug = -1;
    std::int16_t unknown02 = 0;
    std::int16_t unknown01 = 0;

    static constexpr ItemSlot possessions(std::int16_t slot, std::int16_t sub = -1) { return {0, slot, sub, -1, 0, 0}; }
    static constexpr ItemSlot corpse(std::int16_t slot) { return {11, slot, -1, -1, 0, 0}; }
    // The "nowhere" slot the client sends to destroy an item: Location -1 as
    // an int32 (so the unknown word is -1 too) and every slot -1
    // (PcZoneClient::DestroyHeldItemOrMoney @005862f0).
    static constexpr ItemSlot invalid() { return {-1, -1, -1, -1, -1, 0}; }

    bool valid() const { return type >= 0 && type < 0x18 && slot != -1; } // ItemGlobalIndex_IsValid @00441a00
    bool top_level() const { return sub == -1; }
    ItemSlot parent() const { return {type, slot, -1, -1, 0, 0}; }
    // Same location (the two unknown words are ignored).
    bool same(const ItemSlot& o) const { return type == o.type && slot == o.slot && sub == o.sub && aug == o.aug; }
};
inline constexpr std::size_t kItemSlotSize = 12;

void write_slot(ByteWriter& w, const ItemSlot& s);
ItemSlot read_slot(ByteReader& r);

// ItemContainerInstance values (rof2_limits.h invtype).
inline constexpr std::int16_t kInvPossessions = 0, kInvBank = 1, kInvSharedBank = 2, kInvTrade = 3, kInvWorld = 4,
                              kInvLimbo = 5, kInvTribute = 6, kInvGuildTribute = 8,
                              kInvMerchant = 9, kInvCorpse = 11, kInvRealEstate = 14,
                              kInvAltStorage = 19, kInvArchived = 20, kInvMail = 21;

// Possessions slot numbers (rof2_limits.h invslot): 0..22 equipment,
// 23..32 general inventory, 33 cursor.
inline constexpr std::int16_t kSlotCharm = 0, kSlotEar1 = 1, kSlotHead = 2, kSlotFace = 3, kSlotEar2 = 4, kSlotNeck = 5,
                              kSlotShoulders = 6, kSlotArms = 7, kSlotBack = 8, kSlotWrist1 = 9, kSlotWrist2 = 10,
                              kSlotRange = 11, kSlotHands = 12, kSlotPrimary = 13, kSlotSecondary = 14,
                              kSlotFinger1 = 15, kSlotFinger2 = 16, kSlotChest = 17, kSlotLegs = 18, kSlotFeet = 19,
                              kSlotWaist = 20, kSlotPowerSource = 21, kSlotAmmo = 22, kSlotGeneral1 = 23,
                              kSlotGeneral10 = 32, kSlotCursor = 33;
inline constexpr int kEquipmentSlots = 23;
inline constexpr int kGeneralSlots = 10;
// Corpse slots the server uses for loot: CORPSE_BEGIN 23 .. CORPSE_END 56.
inline constexpr std::int16_t kCorpseSlotBegin = 23, kCorpseSlotEnd = 56;

// Display name of a possessions slot ("Primary", "General 3", "Cursor").
const char* possessions_slot_name(int slot);

// ---------------------------------------------------------------- items

// One spell block of an item (ClickEffectStruct / ProcEffectStruct /
// WornEffectStruct, 30 bytes each, then a NUL-terminated name and a u32).
// The five dwords after `level` mean, for the click block: max charges,
// cast time (ms), recast delay (s), recast type, unknown; for the proc block:
// four unknowns and the proc rate; for worn, focus, scroll and bard: unknowns.
struct ItemEffect {
    std::int32_t effect = 0; // spell id (-1 / 0 = none)
    std::uint8_t level2 = 0;
    std::uint32_t type = 0;
    std::uint8_t level = 0;
    std::array<std::uint32_t, 5> extra{};
    std::string name;
    std::uint32_t trailer = 0;

    std::int32_t max_charges() const { return static_cast<std::int32_t>(extra[0]); }
    std::int32_t cast_time() const { return static_cast<std::int32_t>(extra[1]); }
    std::uint32_t recast_delay() const { return extra[2]; }
    std::int32_t recast_type() const { return static_cast<std::int32_t>(extra[3]); }
    std::uint32_t proc_rate() const { return extra[4]; }
    bool present() const { return effect > 0; }
};
enum ItemEffectKind : std::uint8_t { kEffectClick, kEffectProc, kEffectWorn, kEffectFocus, kEffectScroll, kEffectBard, kEffectCount };

// EvolvingItem_Struct (25 bytes), present when the header's is_evolving is set.
struct EvolvingInfo {
    std::uint32_t final_item_id = 0;
    std::int32_t evolve_level = 0;
    double progress = 0.0;
    std::uint8_t activated = 0;
    std::int32_t evolve_max_level = 0;
    std::array<std::uint8_t, 4> unknown005{};
};

struct BagItem;

// One serialized item instance, in SerializeItem's order. Field names follow
// rof2_structs.h; unknowns are kept so a decoded item re-encodes to the same
// bytes.
struct Item {
    // ItemSerializationHeader (77 bytes)
    std::string serial;               // char[17]: the server's 16-digit serial number
    std::uint32_t stack_count = 1;    // "stacksize": the count of a stackable item, else 1
    std::uint32_t unknown004 = 0;
    ItemSlot slot;                    // header slot_type (u8), main_slot, sub_slot, aug_slot (u16 each)
    std::uint32_t price = 0;          // merchant quote
    std::uint32_t merchant_slot = 1;  // merchant count for merchant items, else 1
    std::uint32_t scaled_value = 0;
    std::uint32_t instance_id = 0;
    std::uint32_t parcel_item_id = 0;
    std::uint32_t last_cast_time = 0;
    std::uint32_t charges = 0;        // 0xffffffff = unlimited
    std::uint32_t attuned = 0;        // inst_nodrop
    std::uint32_t unknown044 = 0, unknown048 = 0, unknown052 = 0;
    std::uint8_t is_evolving = 0;
    EvolvingInfo evolving;            // written only when is_evolving
    std::string ornament_main, ornament_off; // "IT<n>" ornamentation models, usually empty

    // ItemSerializationHeaderFinish (26 bytes)
    std::uint32_t ornament_icon = 0;
    std::int32_t unknowna1 = -1;
    std::uint32_t ornament_hero_model = 0;
    std::int32_t unknown063 = 0;
    std::uint8_t copied = 0;
    std::int32_t unknowna4 = -1;
    std::int32_t unknowna5 = 0;
    std::uint8_t item_class = 0;      // 0 common, 1 container, 2 book

    std::string name, lore, id_file, id_file2;

    // ItemBodyStruct (255 bytes)
    std::uint32_t id = 0;
    std::int32_t weight = 0;          // tenths
    std::uint8_t no_rent = 0;         // 0 = temporary
    std::uint8_t no_drop = 0;         // 0 = NO TRADE (EQEmu's NoDrop: 255 tradeable)
    std::uint8_t attuneable = 0;
    std::uint8_t size = 0;            // 0 tiny .. 4 giant
    std::uint32_t equip_slots = 0;    // bit n = possessions slot n
    std::uint32_t value = 0;          // base price in copper
    std::uint32_t icon = 0;
    std::uint8_t unknown1 = 1, unknown2 = 1;
    std::uint32_t benefit_flag = 0;
    std::uint8_t tradeskills = 0;
    std::int8_t cr = 0, dr = 0, pr = 0, mr = 0, fr = 0, sv_corruption = 0;
    std::int8_t astr = 0, asta = 0, aagi = 0, adex = 0, acha = 0, aint = 0, awis = 0;
    std::int32_t hp = 0, mana = 0;
    std::uint32_t endurance = 0;
    std::int32_t ac = 0, regen = 0, mana_regen = 0, end_regen = 0;
    std::uint32_t classes = 0, races = 0, deity = 0;
    std::int32_t skill_mod_value = 0, skill_mod_max = 0, skill_mod_type = 0;
    std::uint32_t skill_mod_extra = 0;
    std::uint32_t bane_dmg_race = 0, bane_dmg_body = 0, bane_dmg_race_amt = 0;
    std::int32_t bane_dmg_amt = 0;
    std::uint8_t magic = 0;
    std::int32_t cast_time_ = 0;      // food/drink duration
    std::uint32_t req_level = 0, rec_level = 0, rec_skill = 0, bard_type = 0;
    std::int32_t bard_value = 0;
    std::uint8_t light = 0, delay = 0, elem_dmg_type = 0, elem_dmg_amt = 0, range = 0;
    std::uint32_t damage = 0, color = 0, prestige = 0;
    std::uint8_t item_type = 0;       // EQEmu ItemType (0 1HS, 1 2HS, 10 armor, 11 misc, 14 food, ...)
    std::uint32_t material = 0, material_unknown1 = 0, elite_material = 0, heros_forge_model = 0, material_unknown2 = 0;
    float sell_rate = 0.0f;
    std::int32_t combat_effects = 0, shielding = 0, stun_resist = 0, strike_through = 0, extra_dmg_skill = 0,
                 extra_dmg_amt = 0, spell_shield = 0, avoidance = 0, accuracy = 0;
    std::uint32_t charm_file_id = 0;
    std::array<std::uint32_t, 4> faction_mod{};
    std::array<std::int32_t, 4> faction_amt{};
    std::string charm_file;

    // ItemSecondaryBodyStruct (74 bytes)
    std::uint32_t aug_type = 0;
    std::int32_t aug_restrict2 = -1;
    std::uint32_t aug_restrict = 0;
    struct AugSocket { std::uint32_t type = 0; std::uint8_t visible = 0, unknown = 0; };
    std::array<AugSocket, 6> aug_sockets{};
    std::uint32_t ldon_point_type = 0, ldon_theme = 0, ldon_price = 0, ldon_sellback_rate = 0, ldon_sold = 0;
    std::uint8_t bag_type = 0, bag_slots = 0, bag_size = 0, weight_reduction = 0;
    std::uint8_t book = 0, book_type = 0;
    std::string book_file;

    // ItemTertiaryBodyStruct (76 bytes)
    std::int32_t lore_group = 0;      // 0 none, -1 lore by item id, > 0 lore group
    std::uint8_t artifact = 0, summoned = 0;
    std::uint32_t favor = 0;
    std::uint8_t fv_no_drop = 0;
    std::int32_t dot_shield = 0, attack = 0, haste = 0, damage_shield = 0;
    std::uint32_t guild_favor = 0, aug_distiller = 0;
    std::int32_t unknown3 = -1;
    std::uint32_t unknown4 = 0;
    std::uint8_t no_pet = 0, unknown5 = 0;
    std::uint8_t potion_belt_enabled = 0;
    std::uint32_t potion_belt_slots = 0;
    std::uint32_t max_stack = 0;      // "stacksize": the stack limit (0 = not stackable)
    std::uint8_t no_transfer = 0;
    std::uint16_t expendable_arrow = 0;
    std::array<std::uint32_t, 4> unknown8_11{};
    std::array<std::uint8_t, 3> unknown12_14{};

    std::array<ItemEffect, kEffectCount> effects{};

    // ItemQuaternaryBodyStruct (171 bytes)
    std::uint32_t script_file_id = 0;
    std::uint8_t quest_item = 0;
    std::uint32_t power = 0, purity = 0;
    std::uint8_t unknown16 = 0;
    std::uint32_t backstab_dmg = 0, ds_mitigation = 0;
    std::int32_t heroic_str = 0, heroic_int = 0, heroic_wis = 0, heroic_agi = 0, heroic_dex = 0, heroic_sta = 0,
                 heroic_cha = 0, heroic_mr = 0, heroic_fr = 0, heroic_cr = 0, heroic_dr = 0, heroic_pr = 0,
                 heroic_sv_corrup = 0;
    std::int32_t heal_amt = 0, spell_dmg = 0, clairvoyance = 0, sub_type = 0;
    std::uint8_t evolve_string = 0, unknown19 = 0;
    std::uint16_t unknown20 = 0;
    std::uint8_t unknown21 = 0, heirloom = 0, placeable = 0;
    std::array<std::uint8_t, 3> unknown22{};
    std::array<std::uint32_t, 3> unknown23_25{};
    float unknown26 = 0.0f, unknown27 = 0.0f;
    std::uint32_t unknown_rof_6 = 0;
    std::uint32_t unknown28 = 0xffffffff;
    std::uint16_t unknown29 = 0;
    std::uint32_t unknown30 = 0xffffffff;
    std::uint16_t unknown31 = 0;
    std::uint32_t unknown32 = 0;
    float unknown33 = 0.0f;
    std::array<std::uint32_t, 4> unknown34_37{};
    std::uint8_t no_zone = 0;
    std::array<std::uint8_t, 4> unknown_rof_7{};
    std::uint8_t no_ground = 0;
    std::array<std::uint8_t, 2> unknown_rof_8{};
    std::uint8_t unknown37a = 0, unknown38 = 0, unknown39 = 1;

    // Bag contents: each preceded on the wire by its u32 index in the bag.
    // SerializeItem gives every content the header slot of the bag's first
    // sub-slot, so the index is the content's real position.
    std::vector<BagItem> contents;

    // The instance's stats-disabled bytes, ItemBase +0x100 (eqlib bDisableAugTexture) and +0x101 (bRankDisabled):
    // client state, not in the packet (ItemClient_UnSerializeItemTree @0x5553b0 never writes them, the constructor
    // clears them); 0x7b28a0 sets them (game::update_stats_disabled).
    bool aug_texture_disabled = false;
    bool rank_disabled = false;

    // ItemDefinition +0x60c (count +0x609): the host subclasses an augment may go into (ItemBase_CanAugmentGoInItem).
    // Read by the original's unserializer but not part of EQEmu's RoF2 serialization, so always empty from the
    // server; not written by write_item.
    std::vector<std::int32_t> socket_subclasses;

    bool stackable() const { return max_stack > 1; }
    bool is_bag() const { return item_class == 1; }
    bool is_lore() const { return lore_group != 0; } // ItemBase::IsLore (the item part)
    bool no_trade() const { return no_drop == 0; }
    // Fields ItemDefinition_UnSerialize @0x7f6bf0 reads where EQEmu's RoF2 struct has unknowns:
    // the Placeable byte and the three after it are one int, the placement flags
    // (+0x5ac: 1 open zones, 2 guild halls, 4 houses, 8 yards, 0x80 guild yards; -1 all); unknown30 is the trophy
    // benefit id (+0x5ec, -1 none); bytes 1 and 2 of unknown37 are Collectible (+0x110) and No Destroy (+0x111).
    std::int32_t placeable_flags() const {
        return static_cast<std::int32_t>(placeable | (std::uint32_t{unknown22[0]} << 8) |
                                         (std::uint32_t{unknown22[1]} << 16) | (std::uint32_t{unknown22[2]} << 24));
    }
    std::int32_t trophy_benefit() const { return static_cast<std::int32_t>(unknown30); }
    bool collectible() const { return ((unknown34_37[3] >> 8) & 0xff) != 0; }
    bool no_destroy() const { return ((unknown34_37[3] >> 16) & 0xff) != 0; }
    const Item* content(std::uint32_t index) const;
    Item* content(std::uint32_t index);
};

struct BagItem {
    std::uint32_t index = 0;
    Item item;
};

// The most contents a bag may hold, and one past the highest bag sub-slot read_item and game::Inventory accept.
// EQEmu's invbag::SLOT_COUNT is 200; Hailstone accepts up to 1000 so a server with larger bags is not refused
// (a deliberate difference; the item's own BagSlots is a u8 on the wire, so a bag still declares at most 255).
inline constexpr std::uint32_t kMaxBagContents = 1000;

// One item (with its contents) in SerializeItem's format. read_item throws
// std::out_of_range on short or malformed data (more than 4 nested levels or
// kMaxBagContents contents).
Item read_item(ByteReader& r);
void write_item(ByteWriter& w, const Item& item);

// ItemPacketType (eq_packet_structs.h), the u32 before an item in OP_ItemPacket.
inline constexpr std::uint32_t kItemPacketViewLink = 0x00, kItemPacketMerchant = 0x64, kItemPacketTradeView = 0x65,
                               kItemPacketLoot = 0x66, kItemPacketTrade = 0x67, kItemPacketCharInventory = 0x69,
                               kItemPacketLimbo = 0x6a, kItemPacketWorldContainer = 0x6b, kItemPacketRecovery = 0x71,
                               kItemPacketParcel = 0x73;

// OP_CharInventory (zone → client at zone-in): u32 count, then count items
// (ENCODE(OP_CharInventory); an empty inventory is 4 zero bytes). nullopt
// unless the payload is consumed exactly.
std::optional<std::vector<Item>> decode_char_inventory(const AppPacket& p);
AppPacket encode_char_inventory(const std::vector<Item>& items); // server direction, for tests

// OP_ItemPacket / OP_ItemLinkResponse (zone → client): u32 ItemPacketType,
// one item; parcels (0x73) add u32 sent time, u32 + name, u32 + note
// (ENCODE(OP_ItemPacket); OP_ItemLinkResponse is forwarded to it).
struct ItemPacket {
    std::uint32_t type = 0;
    Item item;
    std::uint32_t parcel_sent_time = 0;
    std::string parcel_from, parcel_note;
};
std::optional<ItemPacket> decode_item_packet(const AppPacket& p);
AppPacket encode_item_packet(bool link_response, const ItemPacket& v); // server direction, for tests

// OP_ItemPreviewRequest (C->S, 80 bytes; Send_OP_ItemPreviewRequest @0x6fff10 / @0x6ffc20): u32 item id, then for
// an item with an ornamentation (0x7b0700: either IT string, the ornament icon, unknowna1 >= 0, the hero's
// forge model or unknown063 set) char[30] ornament_main (+4), char[30] ornament_off (+0x22), unknowna1 (+0x40),
// ornament_hero_model (+0x44), unknown063 (+0x48) and the byte +0x4d = 1; otherwise empty strings, -1, 0, 0, 0.
// EQEmu reads only the id. Without `item` (the id-only sender @0x6ffc20) the defaults.
AppPacket encode_item_preview_request(std::uint32_t item_id, const Item* item = nullptr);
// The reply (S->C on the same opcode, HandleWorldMessage case 0x7f80): the serialized item, then one byte the
// original reads if it is there (EQEmu sends none: 0).
struct ItemPreviewReply {
    Item item;
    std::uint8_t trailing = 0;
};
std::optional<ItemPreviewReply> decode_item_preview(const AppPacket& p);
AppPacket encode(const ItemPreviewReply& v); // server direction, for tests

// ---------------------------------------------------------------- moves

// OP_MoveItem (28 bytes, both directions; MoveItem_Struct): from, to,
// u32 count. The client sends count 0 for a whole-item move or swap and a
// count only for stack merges and splits; to = invalid
// destroys the item. The server's OP_MoveItem moves or (to invalid) deletes.
// OP_DeleteItem (DeleteItem_Struct) and OP_DeleteCharge (encoded as
// OP_MoveItem) share the layout.
struct MoveItem {
    ItemSlot from;
    ItemSlot to;
    std::uint32_t count = 0;
};
inline constexpr std::size_t kMoveItemSize = 28;
AppPacket encode(const MoveItem& v);                        // OP_MoveItem
AppPacket encode_delete_item(const MoveItem& v);            // OP_DeleteItem
std::optional<MoveItem> decode_move_item(const AppPacket& p); // either opcode

// OP_MoveMultipleItems (client → zone, Send_OP_MoveMultipleItems @0x56fef0; the recorded vectors
// Send_OP_MoveMultipleItems.jsonl): u32 count, then count × 0x24-byte MultiMoveEntry {from (12), to (12), u32
// count, u32 flags (+0x1c), u32 0 (the room mode +0x20, cleared in the list before sending)}. The Ctrl bag moves
// (MoveItem with bMoveBagContents). Each index's last two bytes (ItemSlot::unknown01) are padding the original
// never writes; the client sends 0. An empty list sends nothing (nullopt). EQEmu: Client::Handle_OP_MoveMultipleItems (MultiMoveItem_Struct).
struct MultiMoveEntry {
    ItemSlot from;
    ItemSlot to;
    std::uint32_t count = 0;
    std::uint32_t flags = 0;
};
inline constexpr std::size_t kMultiMoveEntrySize = 0x24;
std::optional<AppPacket> encode_move_multiple(const std::vector<MultiMoveEntry>& moves);

// ---------------------------------------------------------------- loot

// OP_LootRequest (client → zone, u32 corpse spawn id; the server echoes it
// after the items) and OP_EndLootRequest (u32 corpse id; answered with an
// empty OP_LootComplete).
AppPacket encode_loot_request(std::uint32_t corpse_id);
AppPacket encode_end_loot_request(std::uint32_t corpse_id);
std::optional<std::uint32_t> decode_loot_request(const AppPacket& p);

// OP_LootItem (20 bytes, LootingItem_Struct, both directions): corpse,
// looter, u16 corpse slot (23..56), u16, i32 auto loot, u32. The server
// echoes it once the item is looted.
struct LootItem {
    std::uint32_t lootee = 0;   // corpse spawn id
    std::uint32_t looter = 0;   // our spawn id
    std::uint16_t slot = 0;     // corpse slot from the loot item packet
    std::uint16_t unknown10 = 0;
    std::int32_t auto_loot = 0; // > 0: the server places the item itself, else on the cursor
    std::uint32_t unknown16 = 0;
};
inline constexpr std::size_t kLootItemSize = 20;
AppPacket encode(const LootItem& v);
std::optional<LootItem> decode_loot_item(const AppPacket& p);

// OP_MoneyOnCorpse (20 bytes, moneyOnCorpseStruct, passed through): the
// loot answer. response 0 someone else is looting, 1 / 3 ok, 2 not at this
// time, 4 hostiles nearby, 5 too far, 6 every item has been sent (SoD+).
struct MoneyOnCorpse {
    std::uint8_t response = 0;
    std::uint8_t unknown1 = 0, unknown2 = 0, unknown3 = 0;
    std::uint32_t platinum = 0, gold = 0, silver = 0, copper = 0;
};
inline constexpr std::size_t kMoneyOnCorpseSize = 20;
AppPacket encode(const MoneyOnCorpse& v); // server direction, for tests
std::optional<MoneyOnCorpse> decode_money_on_corpse(const AppPacket& p);

// OP_MoneyUpdate (zone → client, 16 bytes, MoneyUpdate_Struct): the coin the
// player carries now (Client::SendMoneyUpdate, after money is added).
struct Coins {
    std::uint32_t platinum = 0, gold = 0, silver = 0, copper = 0;
};
// Opcode 0x3ed4 (both directions; no name in EQEmu's RoF2 table): {platinum,
// gold, silver, copper} as int32s. The server's adds coin the client credits
// to the purse in CEverQuest_ProcessCursorQueue @0x5321a0, which sends the
// same four values back.
inline constexpr std::uint16_t kOpSplitCoin = 0x3ed4;
AppPacket encode_split_coin(const Coins& v);
std::optional<Coins> decode_split_coin(const AppPacket& p); // the opcode, and at least 16 bytes
std::optional<Coins> decode_money_update(const AppPacket& p);
AppPacket encode_money_update(const Coins& v); // server direction, for tests

// ---------------------------------------------------------------- item links

// OP_ItemLinkClick (52 bytes, ItemViewRequest_Struct; the original fills it
// in ParseItemLinkBody @0x4ed020 and sends it from CChatWindow::
// WndNotification): item id, 6 augment ids, link hash, u32 flags (4, plus 2 /
// 8 / 0x10 with shift / ctrl / alt), is evolving, evolve group, evolve level
// (one dword each), u32 ornament icon. The server answers with
// OP_ItemLinkResponse (an item packet of type 0) or, for a say link, says
// the phrase.
struct ItemLinkClick {
    std::uint32_t item_id = 0;
    std::array<std::uint32_t, 6> augments{};
    std::uint32_t hash = 0;
    std::uint32_t flags = 4;
    std::uint32_t is_evolving = 0, evolve_group = 0, evolve_level = 0;
    std::uint32_t ornament_icon = 0;
};
inline constexpr std::size_t kItemLinkClickSize = 52;
AppPacket encode(const ItemLinkClick& v);
std::optional<ItemLinkClick> decode_item_link_click(const AppPacket& p);

// OP_ItemLinkResponse sent by the client (Send_OP_ItemLinkResponse @0x69baf0; 0x48 bytes): u32 item id, u32 flags
// (| 2 with shift held), char name[64]; the krono slot's Inspect sends {88888, "Krono", 0} (0x6949d0). The
// server answers with an item packet (OP_ItemLinkResponse).
struct ItemLinkRequest {
    std::uint32_t item_id = 0;
    std::uint32_t flags = 0;
    std::string name;
};
inline constexpr std::size_t kItemLinkRequestSize = 0x48;
AppPacket encode(const ItemLinkRequest& v);


// OP_Consume (C→Z, 24 bytes, RoF2 Consume_Struct): the item's slot (12 B), u32 0xffffffff when eaten automatically
// (999 on a right click: force-fed, half as filling), u32 1 food / 2 drink, u32 2. EQEmu deletes one of the item
// without telling the client and answers with OP_Stamina.
// Coin the server adds without a coin update (EQEmu AddMoneyToPP with no client update): OP_Sound carrying a
// QuestReward_Struct (68 bytes: u32 giver, u32 our id, u32 exp, u32 faction, i32 mod, then u32 copper, silver,
// gold, platinum at +20; also used for plain sounds, with no coin), and OP_CashReward (16 bytes: copper, silver,
// gold, platinum).
struct CoinReward {
    std::uint32_t target_id = 0; // OP_Sound only; 0 for OP_CashReward
    Coins coins;
};
std::optional<CoinReward> decode_quest_reward(const AppPacket& p);
std::optional<CoinReward> decode_cash_reward(const AppPacket& p);

inline constexpr std::uint8_t kItemTypeFood = 14, kItemTypeDrink = 15;
AppPacket encode_consume(const ItemSlot& slot, bool automatic, bool drink);
// OP_Consume as Handle_OP_ItemVerifyReply @0x4bf800 sends it (a right click on food or drink): auto_consumed
// 0xffffffff, type 1 food / 2 drink, and a last dword whose low byte is 1 (its upper bytes are stack left-overs in
// the original; 0 here).
AppPacket encode_consume_reply(const ItemSlot& slot, bool drink);

// ---------------------------------------------------------------- using items

// The other item types Handle_OP_ItemVerifyReply tests (ItemClient::GetItemType).
inline constexpr std::uint8_t kItemTypeScroll = 20, kItemTypePotion = 21, kItemTypeTome = 31, kItemTypeAlcohol = 38,
                              kItemTypePoison = 42;

// OP_ItemVerifyRequest (C→Z, 16 bytes; CInvSlot::HandleRButtonUp @0x697250, Cmd_useitem @0x479200,
// PcZoneClient_UsePotionBeltSlot): the item's ItemGlobalIndex (12 bytes) and the target's spawn id (0: none).
inline constexpr std::size_t kItemVerifyRequestSize = 16;
AppPacket encode_item_verify_request(const ItemSlot& slot, std::uint32_t target_id);

// OP_ItemVerifyReply (Z→C, 20 bytes, RoF2 ItemVerifyReply_Struct): the slot, the spell to cast (0: the item's own
// click spell; negative: nothing, the dispatch @0x4c579a skips the handler) and the target. EQEmu answers every
// request with spell 0 before it acts on it.
struct ItemVerifyReply {
    ItemSlot slot;
    std::int32_t spell = 0;
    std::uint32_t target_id = 0;
};
inline constexpr std::size_t kItemVerifyReplySize = 20;
std::optional<ItemVerifyReply> decode_item_verify_reply(const AppPacket& p);
AppPacket encode(const ItemVerifyReply& v); // server direction, for tests

// OP_ApplyPoison (both ways, 12 bytes, RoF2 ApplyPoison_Struct): a TypelessInventorySlot {i16 slot, i16 sub, i16 aug,
// i16 unknown} and u32 success. DoPassageOfTime @0x45e1e8 sends the poison's top-level slot as ItemIndex(slot, -1, -1)
// (0x460160); its unknown word and success are stack left-overs there (0 here, and EQEmu reads neither). The
// reply (Handle_OP_ApplyPoison @0x4b5c20) reads only success.
struct ApplyPoison {
    std::int16_t slot = -1;
    std::int16_t sub = -1;
    std::int16_t aug = -1;
    std::int16_t unknown = 0;
    std::uint32_t success = 0;
};
inline constexpr std::size_t kApplyPoisonSize = 12;
AppPacket encode(const ApplyPoison& v);
std::optional<ApplyPoison> decode_apply_poison(const AppPacket& p);

} // namespace hailstone::net
