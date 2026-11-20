#pragma once

// The item display window's contents (CItemDisplayWnd), built as CItemDisplayWnd_BuildItemInfo @0x6a9540 builds
// them: the info lines (IDW_ItemInfo1..12: the name, the flags, the classes, races, deities and slots), the stat
// grid (IDW_Row<r>Col<c>Stat / Value, 26 rows of 3 columns, filled column by column in two blocks), the heroic
// bonuses beside their values (IDW_Heroic<n>) and the description STML (CItemDisplayWnd_CreateEffectsString
// @0x6a6810 with UpdateStrings @0x69ae30), read from the disassembly; docs/unknowns.md lists
// what is not ported.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

#include "net/item_packets.h"

namespace hailstone::game {

class EqStrings;
struct SpellData;
namespace mechanics { class Character; }

using ItemFlagsFormatter = std::function<std::optional<std::string>(const net::Item&, std::string_view)>;
using ItemDescriptionAppender = std::function<std::optional<std::string>(const net::Item&)>;

struct ItemDisplayContext {
    const EqStrings* strings = nullptr;
    // The player: CanUseItem, the "modified" stats (vtable +0x34), the damage bonus. Null outside the game.
    const mechanics::Character* character = nullptr;
    std::int32_t player_level = 1;
    std::uint8_t player_class = 0;  // the class the original reads (profile +0x3374)
    std::uint32_t player_classes = 0; // the classes bit mask (bit 0 Warrior; NMS multiclass may set several)
    // PcZoneClient::CanEquipItem(item, slot, 0) for any possessions slot (the name line's colour).
    std::function<bool(const net::Item&)> can_equip;
    // CEverQuest::GetDeityDesc(id) for deity ids 201..217.
    std::function<std::string(std::uint32_t deity)> deity_name;
    // The spell table (null when the spell is not known).
    std::function<const SpellData*(std::int32_t id)> spell;
    // dbstr_us.txt (lore groups: type 7; spell descriptions: type 6).
    std::function<std::string(std::uint32_t id, std::uint32_t type)> db_string;
    // From the server extension (game/server_ext.h; stock: false and empty): CanUseItem and CanEquipItem return
    // true for everything, and the scroll's "Level Needed: " line is rebuilt (the NMS patch, with a class mask;
    // nullopt for a spell keeps the stock line).
    bool can_use_all = false;
    std::function<std::optional<std::string>(const SpellData&)> level_needed_line;
    ItemFlagsFormatter item_flags;
    ItemDescriptionAppender item_description_append;
    bool modified = false;               // the window's Modified view (+0x2e8)
    bool show_spell_descriptions = true; // eqclient.ini Defaults/ShowSpellDescriptions (default 1)
    // The player's heading (0..512, the compass's; pinstLocalPlayer +0x80): a compass item's direction. < 0 none.
    float heading = -1.0f;
    // A trophy tribute benefit by id (personal, or the guild's with `guild`): its name and description, for the
    // trophy lines. Unset or nullopt: no lines (EQEmu sends no trophy tribute lists).
    std::function<std::optional<std::pair<std::string, std::string>>(std::int32_t id, bool guild)> trophy_benefit;
};

// The heroic values' colour (0x69b3d0).
inline constexpr std::uint32_t kHeroicArgb = 0xffd6b228;

struct ItemDisplayCell {
    bool shown = false;
    std::string stat;                 // IDW_Row<r>Col<c>Stat
    std::string value;                // IDW_Row<r>Col<c>Value
    std::uint32_t value_argb = 0xffffffff;
    std::string tooltip;              // the stat label's tooltip (Power)
    std::string heroic;               // "+n" beside the value (IDW_Heroic<n>, in kHeroicArgb)
    // Hailstone: the stat's item comparison key (game/item_compare.h compare_stat_key: "ac", "str", "svFire", ...),
    // "" for a row the comparison has no stat for. The window's own IDW_Row<r>Col<c>Key child gives it to the UI.
    const char* key = "";
};

struct ItemDisplayLine {
    std::string text;
    std::uint32_t argb = 0xffffffff;
};

struct ItemDisplay {
    static constexpr int kRows = 26;
    static constexpr int kInfoLines = 12;
    std::string title;                         // the window title (the item's name)
    std::vector<ItemDisplayLine> info;         // IDW_ItemInfo1.. in order
    std::array<std::array<ItemDisplayCell, 3>, kRows> grid{}; // [row - 1][column - 1]
    int columns = 1;                           // how many stat columns are used (CStack_194)
    int last_row = 0;                          // the lowest row any column uses
    int heroic_count = 0;
    std::string description; // the description STML (ItemInfo + "<BR>Item information:<BR>" + ...)
    std::string mod_button;  // IDW_ModButtonLabel: "Unmodified" / "Modified"
    // The strings BuildItemInfo measures for the window's width (@006a9e6b): the name, the flags, the first slots
    // line, the two class, race and deity lines (empty when not built).
    std::array<std::string, 9> measured;
};

ItemDisplay build_item_display(const net::Item& item, const ItemDisplayContext& ctx);

// The source/final template ids, current level and unaugmented link hash used by the native Final Result link.
// No request for a non-evolving, complete or unusable evolution record.
std::optional<net::EvolvingFinalResult> evolving_final_result(const net::Item& item);

// @006a9e6b: the widest of `measured` in pSystemFont (FontsArray[2], CDisplay @0048e71b); of the name and the
// flags only the longer string (CXStr length) is measured.
int item_display_text_width(const ItemDisplay& d, const std::function<int(std::string_view)>& extent);

// What the window's layout reads besides the grid: the shown socket screens and the lengths of their description
// STMLs (CXStr length, tags included), and the width above.
struct ItemDisplayFrameIn {
    bool appearance = false; // IDW_Appearance_Socket_Screen shown
    int appearance_text = 0;
    std::array<bool, 6> sockets{}; // IDW_Socket_Slot_<n>_Screen shown
    std::array<int, 6> socket_text{};
    int text_width = 0;
};

// Where CItemDisplayWnd places its children (0x69b590 and the end of BuildItemInfo, @006ad8e4..@006adcf4), in
// the description page's pixels, and the size it gives itself: CXWnd::Resize(width, height) @006adce8, the whole
// window with its frame and title bar.
struct ItemDisplayLayout {
    static constexpr int kValueRight[3] = {120, 240, 360}; // IDW_Row1Col<c>Value's right (EQUI_ItemDisplay.xml)
    int appearance_top = 0;          // IDW_Appearance_Socket_Screen (40 high)
    int grid_top = 0;                // row 1 of the grid; each row 16 below the one above
    int mod_top = 0;                 // IDW_ModButton (15 x 15 at x 10) and its label (100 wide, 10 right of it)
    std::array<int, 6> socket_top{}; // the shown socket screens (20 high); -1 for the others
    int description_top = 0;         // ItemDescription's TopAnchorOffset (left 10, right 0, bottom 3)
    int width = 0, height = 0;
    int row_top(int r) const { return grid_top + 16 * (r - 1); }
};
ItemDisplayLayout layout_item_display(const ItemDisplay& d, const ItemDisplayFrameIn& in);

// 0x7e8a10: the damage bonus the player's class and level give a weapon (the Dmg Bon line).
std::int32_t weapon_damage_bonus(std::int32_t level, std::uint8_t player_class, const net::Item& item);

} // namespace hailstone::game
