#pragma once

// A hotbutton's mouse and its context menu (0x5536b0, CHotButton's WndNotification for its
// button; the menu CHotButtonWnd_BuildContextMenus @0x54ef20 builds once into 0xade708, CHotButton_UpdateContextMenu
// @0x550e30 fits it to the button, CHotButton_ShowContextMenu @0x551360 pops it up at the mouse).
//
// The mouse (the button's CButtonWnd notifications, CButtonWnd vf18..vf21):
//  - a left (XWM_LCLICK) or quick right click (XWM_RCLICK, on the right button's release) on an empty button opens
//    the menu, unless something is on the cursor (CCursorAttachment::IsActive);
//  - else a left click puts the cursor's hotbutton there (0x552e00) or runs the button (DoHotButton(n, 1)), a
//    quick right click runs 0x54fc10 (game/hot_button_press.h) except on items (types 5, 12, 13: the item slot
//    child takes its own right click);
//  - the right button held 500 ms (XWM_RCLICKHOLD, __ProcessMouseEvents) opens the menu on any button.
//
// The menu's rows (eqstr; the item id the menu sends; UpdateContextMenu shows a row or hides it, 0x85bc70):
//   7661 Activate     0x1b58  a button other than a krono slot (13): DoHotButton(n, 0)
//   5371 Consume      0x1b61  a krono slot: DoHotButton(n, 0)
//   7668 Inspect      0x1b5d  a spell gem holding a spell (its spell display, CSpellDisplayManager::ShowSpell), an item
//                             (5, 12, 13: its item display, 0x6949d0)
//   7662 Clear        0x1b59  a button: ClearHotButtonSlot, saved (0x550c50)
//   7671 Change Size  0x1b60  always: the bar's button size (0x68a170: CQuantityWnd "Percent")
//   7664 Assign Icon  0x1b5a  a button: CIconSelectionWnd on the button's icon (0x68f330); its answer (0x28)
//                             sets the icon, saved (0x550040)
//   7672 Clear Icon   0x1b5b  a button with an icon (+0x64 != -1): icon (-1, -1), saved
//   7665 Assign Label 0x1b5c  a button: CTextEntryWnd "Enter label:" (7669), 0..15 characters, starting from the
//                             button's text; OK (0x26, 1) renames the social of a social button (< 120; SaveSocialsToIni)
//                             or sets the button's label (63 characters), saved
//   7663 Assign Social        always: "Page %1" (7666) 1..10, each with the page's 12 socials by name or "<New Social>"
//                             (7677); item 0x1b8a + n makes the button social n (type 4), saved, and an unnamed
//                             social opens the social editor on it (0x758bb0)
//   7667 Memorize             (with the cast spell window) a spell gem without a spell: the gem's spell select
//                             menu (CCastSpellWnd::MakeSpellSelectMenu); item 10000 + spell memorizes it there
//                             (CCastSpellWnd_TryMemorizeFromCursor)
//   7670 Unmemorize   0x1b5f  (with the cast spell window) a spell gem with a spell: CCastSpellWnd::Unmemorize
// The gem's spell is the profile's memorized spell (+0x3060 + 4 * value; a value above 15 is the original's int3).

#include <cstdint>
#include <optional>

#include "game/hotbuttons.h"
#include "game/inventory.h"

namespace hailstone::game {

inline constexpr std::uint32_t kStrHbActivate = 7661, kStrHbConsume = 5371, kStrHbInspect = 7668,
                               kStrHbClear = 7662, kStrHbChangeSize = 7671, kStrHbAssignIcon = 7664,
                               kStrHbClearIcon = 7672, kStrHbAssignLabel = 7665, kStrHbAssignSocial = 7663,
                               kStrHbSocialPage = 7666, kStrHbMemorize = 7667, kStrHbUnmemorize = 7670,
                               kStrHbEnterLabel = 7669, kStrHbNewSocial = 7677;
inline constexpr int kHotButtonLabelEntryMax = 15; // CTextEntryWnd::Activate(this, 0, 7669, 0, 15, text)
inline constexpr std::int32_t kSpellSelectMenuIdBase = 10000;  // a spell select menu item: 10000 + spell id

enum class HotButtonMouse { LeftClick, RightClick, RightHeld };
enum class HotButtonMouseAction {
    None,       // nothing here (the item slot's own right click, or a right click with nothing to do)
    Menu,       // CHotButton_ShowContextMenu
    Drop,       // 0x552e00: the cursor's hotbutton goes here
    Press,      // DoHotButton(n, 1)
    RightClick, // 0x54fc10
};
// 0x5536b0's choice for a mouse notification; `cursor` is CCursorAttachment::IsActive.
HotButtonMouseAction hot_button_mouse(const HotButton& b, HotButtonMouse m, bool cursor);

// Which of the menu's rows CHotButton_UpdateContextMenu leaves shown for the button. `gem_spell`: a spell gem button
// whose gem holds a valid spell; `cast_spell_window`: pinstCCastSpellWnd (the Memorize rows exist only with it).
struct HotButtonMenuRows {
    bool activate = false, consume = false, inspect = false, clear = false, change_size = true, assign_icon = false,
         clear_icon = false, assign_label = false, assign_social = true, memorize = false, unmemorize = false;
};
HotButtonMenuRows hot_button_menu_rows(const HotButton& b, bool gem_spell, bool cast_spell_window = true);

// Item 0x1b8a + social: the button becomes social `social` (0..119; type 4, the rest of the record kept, as the
// original only writes the type and the value). True when the social has no name: the social editor opens on it.
bool assign_social(HotButtonStore& store, HotButton& b, std::int32_t social);

// The label entry's OK (0x26, 1): a social button's social (value < 120) takes the text as its name (no cut but the
// entry's 15 characters; true: the socials are to be saved), any other button the text as its label (63
// characters). Either way the hotbuttons are saved.
bool assign_label(HotButtonStore& store, HotButton& b, const std::string& text);

// CInvSlotWnd::HandleLButtonHeld @0x699c30 (vtable 0x9eb920 slot 16; the button held down inside the slot) calls
// 0x6947f0, which puts a hotbutton for the slot on the cursor when nothing is on it (CCursorAttachment::
// RemoveAttachment @0x660f30 answers 1 only then) and the slot is a possessions slot (or the Krono slot, not here):
//  - an item there and neither Shift nor Ctrl (GetKeyboardFlags & 0xc): an item link (attachment type 14, hotbutton
//    type 12, 0x54ee90) with the item's GUID and item id and no slot (index -1), in a bag or not;
//  - else, a top-level slot only (sub-index -1): the slot itself (attachment 8, hotbutton 5), with or without an item.
// No label and no icon of its own (the attachment's text is empty, 0x552e00 then sets the icon to (-1, -1)).
// Nullopt where the original attaches nothing.
std::optional<HotButton> inventory_hot_button(const net::ItemSlot& slot, const net::Item* item, bool shift_or_ctrl);

// The possessions slot whose item a type 5 or 12 button stands for: type 5 its slot; type 12 the item with the
// button's GUID (the item's serial), else (no such serial: EQEmu gives an item a new serial in each zone) the first
// with its item id, top-level slots in order, each bag's contents after the bag; never the cursor. Nullopt: none.
std::optional<net::ItemSlot> hot_button_item_slot(const Inventory& inv, const HotButton& b);

// For the item resolved above, a link shows all carried stacks and a slot button shows its own stack.
// Zero means there is no quantity to display.
std::uint64_t hot_button_stack_count(const Inventory& inv, HotButtonType type, const net::Item& item);

} // namespace hailstone::game
