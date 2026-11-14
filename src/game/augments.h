#pragma once

// Augments: the fit test (ItemBase::CanGemFitInSlot @0x7bcdb0 and the checks it calls), what the item display shows
// of an item's sockets (CItemDisplayWnd_UpdateAugmentSlots @0x69f240, the augment lines of the description) and the
// requests the display makes when a socket is clicked (CItemDisplayWnd::InsertAugmentRequest @006a8190,
// RemoveAugmentRequest @006a86b0, CItemDisplayWnd_HandleAugmentInsert @006a8b70 (swap), sub_006a92c0 (destroy)).
// The fit checks are checked against the original's vectors. The rules are
// plain functions over net::Item; GameConnection supplies the inventory searches and sends the result.
//
// Item fields (ItemDefinition offsets): item_class +0x11a (0 common, 1 bag), item_type
// +0x1d4, classes / races / deity +0x170 / +0x174 / +0x178, equip_slots +0xf0, delay +0x182, material = ArmorProps
// .Type +0x1d8, aug_sockets +0x1ec (8 bytes each: type, visible +4, and the +5 flag: the socket scales its augment's
// stats and colours the display's modifiers), aug_type +0x21c, aug_restrict2 = AugSkinTypeMask +0x220,
// aug_restrict +0x224, aug_distiller = SolventItemID +0x228, max_stack = StackSize +0x580, no_transfer =
// NoStorage +0x584, sub_type = SubClass +0x5a4, socket_subclasses = the socket subclass list (+0x609 count, +0x60c;
// not in EQEmu's RoF2 item serialization, so empty for the server's items).

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "net/augment_packets.h"
#include "net/item_packets.h"

namespace hailstone::game {

class EqStrings;

inline constexpr int kAugSockets = 6;
// ItemTypes: an augment, an old augmentation solvent (refused as a distiller on remove), a distiller, the Perfected
// Augmentation Distiller (removes any augment).
inline constexpr std::uint8_t kItemTypeAugment = 54, kItemTypeSolvent = 55, kItemTypeDistiller = 56,
                              kItemTypePerfectedDistiller = 64;

// ---- sockets

// ItemBase_IsAugSocketValid @007b0620: socket < 6 and its type is not 0. ItemBase_IsAugSocketVisible @007b05f0.
bool aug_socket_valid(const net::Item& item, int socket);
bool aug_socket_visible(const net::Item& item, int socket);
std::uint32_t aug_socket_type(const net::Item& item, int socket);
// AugType_IsOrnamentation @007afe10 (types 20 and 21, the appearance socket) and AugType_IsSpecialOrnamentation
// @007afe30 (21, which needs game feature 0x15).
inline bool is_ornament_socket_type(std::uint32_t type) { return type == 20 || type == 21; }
inline bool is_special_ornament_socket_type(std::uint32_t type) { return type == 21; }
// ItemBase_GetChildItem @007b1de0: the augment in a socket (child `socket`), or null.
const net::Item* augment_in(const net::Item& item, int socket);
// ItemBase_HasSolvent @007b07f0: the augment names a SolventItemID (EQEmu AugDistiller).
inline bool has_solvent(const net::Item& aug) { return aug.aug_distiller != 0; }
// ItemBase_IsStatsDisabled @007b0520: either of the instance's disable bytes, +0x101 (rank) or +0x100 (the aug
// texture), is set. Only update_stats_disabled writes them.
inline bool stats_disabled(const net::Item& item) { return item.rank_disabled || item.aug_texture_disabled; }
// 0x7b28a0 (thiscall, item; use_item, ornament), what ProcessAllStats runs on each worn item (slots 0..0x15) and
// the item display builders (CItemDisplayWnd_BuildItemInfo @0x6a9540, BuildItemDescriptionSTML @0x6a3360, 0x6a1c70)
// on the shown item, with the game features GetGameFeature(0x13 UseItem) and GetGameFeature(0x15 Ornament):
//  - +0x101 = use_item != -1 and use_item < the definition's Prestige (+0x1d0; 0 for a non-Type-0 item);
//  - a container (ItemClass 1): each item inside gets the same call, then its +0x100 = 0;
//  - a Type 0 item: each socket 0..5's augment (ItemBase_GetChildItem) gets the same call, then its +0x100 =
//    (ornament == 0 and the socket's type is 21, the special ornament socket);
//  - last, an item at a top-level index (ItemGlobalIndex slot word +0x3c not -1, sub word +0x3e -1) gets +0x100 = 0.
// `top_level` is that last test for `item` (the children are never top level).
void update_stats_disabled(net::Item& item, std::int32_t use_item, std::int32_t ornament, bool top_level);
// 0x69bf80: a valid, visible socket (ornaments included) holds an augment (the title's "(Augmented)").
bool is_augmented(const net::Item& item);

// ---- the fit test

// ItemDefinition_IsWeapon @0x7f68a0: damage above 0, not item types 0x1b, 0x1c, 0x2b, 0x2c.
bool is_weapon_type(const net::Item& item);
// ItemBase_CanAugmentGoInItem @0x7b2410: the augment's AugRestrictions allow the host's ItemType, a weapon host's
// delay is not below the augment's (when the augment has one), and (with a subclass list, never on EQEmu) the
// host's subclass (+0x5a4) is listed.
bool can_augment_go_in_item(const net::Item& host, const net::Item& aug);
// ItemBase_CheckAugmentCombinedRestrictions @0x7bbec0: 0, or 0xf not an augment (no augment, AugType 0), 0xb the
// NoStorage bytes differ, 1 no common race, 2 no common class, 3 the deities exclude each other, 4 no common equip
// slot, 9 can_augment_go_in_item fails, 0xe the host is stackable. The masks are the recursive getters (the item's
// children ANDed in; 0 for a non-Type-0 item, so a bag host gives 1).
// `worn_classes` (a server extension's, game/server_ext.h augment_classes: the player's classes, bit c-1; 0 the
// stock check): for a worn host an augment whose own Classes are set must share one of them (else 2), and the host
// takes them for the class test (a server patch's detour; the projecttriune plugin switches it on).
int check_augment_combined_restrictions(const net::Item& host, const net::Item* aug, std::uint32_t worn_classes = 0);
// ItemBase::CanGemFitInSlot(aug, socket, bCheckEmpty, bCheckDup) @0x7bcdb0: the first failing check, or 0:
// check_augment_combined_restrictions; 6 socket above 5 (unsigned) or of type 0; 7 the augment's AugType lacks bit
// (socket type - 1); 8 its AugSkinTypeMask lacks bit (the host's ArmorProps.Type); with check_empty, 5 the socket is
// occupied; with check_dup, 10 an item with the augment's ItemNumber is somewhere in the host's contents (depth
// first; ItemNumber 0 matches any item) and the last slot of its path is not this socket; 0x10 the host is worn
// (`host.slot`: possessions, top level, slot not -1 and at most 22 as a signed short) and the augment's equip slots
// (its children ANDed in) lack that slot. Shift counts are taken mod 32. Callers: insert (1, 1), swap (0, 1), the
// socket highlight (0, 0).
int can_gem_fit_in_slot(const net::Item& host, const net::Item* aug, std::uint32_t socket, bool check_empty,
                        bool check_dup, std::uint32_t worn_classes = 0);
// AugmentFitResult_GetStringID @007afe40: the eqstr for a result (0 none; 0xc 7686 and 0xd 5485 are set by the
// insert and swap requests themselves).
std::uint32_t augment_fit_string_id(int result);
// ItemClient_GetAugRestrictionStringId @00554c70: the "... Only" text of AugRestrictions 1..15, else 0.
std::uint32_t aug_restriction_string_id(std::uint32_t restriction);

// ---- the look

// What a worn item shows on the wearer's own model in material slot `material_slot` (0..8), ornament included
// (refresh_worn_slot, PcClient_RefreshWornSlotVisual):
//  - primary and secondary (7, 8): the model number of the ornament model the server names (ornament_main, or
//    ornament_off for the secondary when that is set; EQEmu writes the ornament augment's IDFile into both), else
//    the item's IDFile;
//  - armour (0..6): the ornament's hero's forge model the server names (ornament_hero_model, EQEmu's
//    GetOrnamentHeroModel for the slot) in place of the item's, and the Material of the augment in an ornament
//    socket (types 20, 21; the later socket, as the appearance area shows it) in place of the item's, when it has
//    one and its texture is not disabled (update_stats_disabled). A robe glamour is such a Material (10..16).
// The tint is the item's. The original's own choice of these was not traced (docs/unknowns.md).
struct WornLook {
    std::uint32_t material = 0, hero_forge = 0, tint = 0;
};
WornLook worn_look(const net::Item& item, int material_slot);

// ---- the item display

inline constexpr std::uint32_t kAugFitArgb = 0xffd6b228;      // the "fits" highlight (@0069f3e8)
inline constexpr std::uint32_t kAugDisabledArgb = 0xffff0000; // a stats-disabled augment's row

struct AugmentStrings {
    const EqStrings* strings = nullptr;
    // dbstr_us.txt (type 16: the socket type names); "" when missing.
    std::function<std::string(std::uint32_t id, std::uint32_t type)> db_string;
};

// One socket row as UpdateAugmentSlots leaves it: pItemSocketScreen[i] shown, the button's decal (the augment's
// icon, 0 none) and tooltip, pItemSocketDescription[i]'s STML.
struct AugmentSocketRow {
    bool shown = false;
    std::string text;          // STML: 3355 "Slot %1, type %2: %3", the augment's name a <a WndNotify="27,0^i"> link
    std::uint32_t icon = 0;
    bool filled = false;
    std::string tooltip;       // 4671 filled, 4670 empty (4673 for the appearance socket)
};
struct AugmentSockets {
    std::array<AugmentSocketRow, kAugSockets> rows{};
    // The appearance (ornament) socket: pAppearanceSocketScreen shown for a socket of type 20 or 21 (the later one
    // wins), its row in `appearance`; the Buy button shown when that socket is empty, of type 21 and the feature
    // is missing (tooltip 4672, else "").
    bool appearance_shown = false;
    int appearance_socket = -1;
    AugmentSocketRow appearance;
    bool buy_shown = false;
    std::string buy_tooltip;
};
// CItemDisplayWnd_UpdateAugmentSlots @0x69f240 for the window's item with `cursor` on the cursor (null: empty). Every
// valid, visible socket gets its row text; a row is wrapped in kAugFitArgb when the cursor item fits
// (can_gem_fit_in_slot(item, cursor, i, 0, 0) == 0), else in kAugDisabledArgb when its augment is stats-disabled.
// `appearance_feature`: game feature 0x15.
AugmentSockets build_augment_sockets(const net::Item& item, const net::Item* cursor, const AugmentStrings& s,
                                     bool appearance_feature, std::uint32_t worn_classes = 0);
// One row's text without the fit highlight (the description copy, BuildItemDescriptionSTML @006a3360 loop
// @006a5931: every valid, visible socket, ornaments included, each followed by <BR>).
std::string augment_socket_row_text(const net::Item& item, int socket, const AugmentStrings& s, bool highlight);
std::string augment_description_rows(const net::Item& item, const AugmentStrings& s);
// 0x69d6e0 (ItemType 54): 3357 "This Augmentation fits in slot types: %1" over the AugType bits 1..31 (dbstr
// type 16 names, " " before the first and ", " between; 3369 "Fits in all slot types." with 31 or more bits). No
// <BR>.
std::string augment_slot_types_line(const net::Item& aug, const AugmentStrings& s);
// BuildItemDescriptionSTML's Restrictions block (@006a5362, ItemType 54): "<BR>" 3358 " -<BR>", the AugRestrictions
// line, 9147 "Delay: %1" for a delay above 0, each with <BR>; `slots` (ItemDisplay_BuildSlotsSTML) is appended by
// the caller. The item display window's own effects text (CItemDisplayWnd_CreateEffectsString) has only the header
// and the restriction, and only when there is one (augment_effects_restriction).
std::string augment_restrictions_block(const net::Item& aug, const AugmentStrings& s);
std::string augment_effects_restriction(const net::Item& aug, const AugmentStrings& s);

// ---- requests

// An item the requests search for (PcZoneClient::GetItemByID @00585a10 / GetItemByItemClass @00586210) and where it
// is.
struct FoundItem {
    const net::Item* item = nullptr;
    net::ItemSlot slot;
};

// What the request functions read of the client.
struct AugmentContext {
    bool has_pc = true;
    const net::Item* cursor = nullptr; // possessions slot 33
    // CharacterBase::GetItemByGlobalIndex and PcZoneClient_GetItemBySerial (the 64-bit serial, +0xf8).
    std::function<const net::Item*(const net::ItemSlot& slot)> item_at;
    std::function<const net::Item*(std::uint32_t serial, std::uint32_t high)> item_by_serial;
    // The possessions searched recursively (bags and their items' children too): the first item with this id, the
    // first of this ItemType.
    std::function<std::optional<FoundItem>(std::uint32_t id)> item_by_id;
    std::function<std::optional<FoundItem>(std::uint8_t item_type)> item_by_type;
    // CharacterZoneClient_CanUseItemPrestige(item, 0, 0) and CharacterZoneClient::CanUseItem(item, 0, 0) (the class).
    std::function<bool(const net::Item&)> can_use_prestige;
    std::function<bool(const net::Item&)> can_use;
    bool appearance_feature = true; // game feature 0x15 (the PC sub-object's vtable slot 0)
    std::uint32_t worn_classes = 0; // check_augment_combined_restrictions' (a server extension's; 0 stock)
};

struct AugmentRequest {
    enum Kind : std::uint8_t {
        Nothing, // returned silently
        Refuse,  // eqstr / args to dsp_chat in colour 13
        Send,    // send `packet` now
        Confirm, // ConfirmationDialog(type `dialog`, text eqstr / args, 0xc, -1): yes sends `packet`; IsItemPending
                 // + 1 and pending reason 0x13 until it is answered
    } kind = Nothing;
    std::uint32_t eqstr = 0;
    std::vector<std::string> args;
    std::uint32_t dialog = 0;
    net::AugmentItem packet;
};
inline constexpr std::uint32_t kAugmentChatColor = 13;
inline constexpr std::uint32_t kConfirmAugInsert = 0x76, kConfirmAugRemove = 0x77, kConfirmAugSwap = 0x78,
                               kConfirmAugDestroy = 0x79;
inline constexpr std::int32_t kAugmentPendingReason = 0x13;

// The four requests, `window_item` the display's item. Each first returns Nothing without a PC or for a socket
// that is not valid and visible, then resolves the host (by its index, then its serial; else 5473).
AugmentRequest insert_augment_request(const net::Item& window_item, int socket, const AugmentContext& ctx);
AugmentRequest remove_augment_request(const net::Item& window_item, int socket, const AugmentContext& ctx);
AugmentRequest swap_augment_request(const net::Item& window_item, int socket, const AugmentContext& ctx);
AugmentRequest destroy_augment_request(const net::Item& window_item, int socket, const AugmentContext& ctx);
// sub_006ae010 @006ae010, a left click on a socket: empty → insert; filled with an empty cursor → remove; filled
// with something on the cursor → swap.
AugmentRequest augment_socket_click(const net::Item& window_item, int socket, const AugmentContext& ctx);

// The socket's context menu (sub_006a01b0 @006a01b0): only for a possessions item (Location 0) that resolves and
// holds an augment in the socket. Rows: the augment's name (id 1, 0xFF00FF00, disabled), then 5470 Remove (100) with
// an empty cursor or 5483 Swap (0x65) with something on it (0xFF8AA3FF), then 5471 Destroy (0x66, 0xFFC08080) when
// the augment has a SolventItemID.
struct AugmentMenuRow {
    std::string text;
    std::uint32_t id = 0;
    std::uint32_t argb = 0;
    bool enabled = true;
};
inline constexpr std::uint32_t kAugMenuRemove = 100, kAugMenuSwap = 0x65, kAugMenuDestroy = 0x66;
std::vector<AugmentMenuRow> augment_socket_menu(const net::Item& window_item, int socket, const AugmentContext& ctx,
                                                const EqStrings* strings);

} // namespace hailstone::game
