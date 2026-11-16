#pragma once

// Developer item windows drawn with Dear ImGui until the stock UI engine
// (M5): the inventory (equipment, general slots, coin, open bags), item
// information, the loot window, and the item held on the cursor. Like ui/dev_screens.h they draw from game state
// and return what the player asked for; none of them does networking.
//
// Item icons come from the stock UI's A_DragItem animation
// (uifiles/default/EQUI_Animations.xml): icon number n >= 500 is frame
// n - 500 of dragitem1.dds, dragitem2.dds, ... — 36 cells of 40 x 40 pixels
// per 256 x 256 sheet, a vertical grid (down each column first).

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "game/inventory.h"
#include "game/item_display.h"
#include "net/item_packets.h"

namespace hailstone { class GameDir; }
namespace hailstone::render { class VkContext; }

namespace hailstone::ui {

// Where an icon number lies in the drag-item sheets (pure arithmetic, tested).
struct IconCell {
    int sheet = 0; // 1-based: dragitem<sheet>.dds
    int x = 0, y = 0; // top-left pixel of the 40 x 40 cell
};
std::optional<IconCell> icon_cell(std::uint32_t icon);

// Lazily loads the drag-item sheets as ImGui textures.
class ItemIconCache {
public:
    ItemIconCache(render::VkContext& vk, const GameDir& game);
    ~ItemIconCache();
    ItemIconCache(const ItemIconCache&) = delete;
    ItemIconCache& operator=(const ItemIconCache&) = delete;

    // Draws the icon (or a placeholder with the item's initial) at the
    // cursor position as an ImGui item of the given size. Returns true when
    // an icon was drawn.
    bool draw(std::uint32_t icon, float size, const char* fallback_text = nullptr);
    // Draws into a draw list at a screen rectangle (cursor item).
    bool draw_at(void* draw_list, std::uint32_t icon, float x, float y, float size);

private:
    struct Sheet;
    const Sheet* sheet(int number);

    render::VkContext& vk_;
    const GameDir& game_;
    void* sampler_ = nullptr; // VkSampler
    std::map<int, std::unique_ptr<Sheet>> sheets_;
};

struct ItemWindowsState {
    bool show_inventory = false;
    std::set<std::int16_t> open_bags;          // general slots 23..32 whose bag window is open
    std::vector<net::Item> info;               // open item information windows
    std::uint64_t info_serial = 0;             // for unique window ids
    std::vector<std::uint64_t> info_ids;
};

struct ItemWindowsInput {
    const game::Inventory* inventory = nullptr;
    net::Coins coins;
    std::uint32_t class_mask = 0;
    // Loot window (open when corpse_id != 0).
    std::uint32_t corpse_id = 0;
    std::string corpse_name;
    const std::map<std::int16_t, net::Item>* loot = nullptr;
    net::Coins loot_money;
    game::ItemFlagsFormatter item_flags;
    game::ItemDescriptionAppender item_description_append;
};

struct ItemWindowsActions {
    std::optional<net::ItemSlot> click;          // left click on a slot (pick up / put down)
    bool destroy = false;                        // destroy the cursor item
    bool auto_inventory = false;
    std::optional<std::pair<std::int16_t, bool>> loot; // corpse slot, auto loot (right click)
    bool loot_all = false;
    bool close_loot = false;
};

// Opens an item information window for an item (from a link or a right click).
void show_item_info(ItemWindowsState& state, net::Item item);
// 0x6ae710(item, 0) @006ae710: every open item information window whose item has the same serial (+0xf8: the
// header's instance_id and parcel_item_id) shows `item` from now on (SetItem again, in place).
void refresh_item_info(ItemWindowsState& state, const net::Item& item);

ItemWindowsActions item_windows(ItemWindowsState& state, const ItemWindowsInput& in, ItemIconCache* icons);

// The stat lines of the item information window (CItemDisplayWnd_BuildItemInfo
// order, simplified), for tests and the window.
std::vector<std::string> item_info_lines(const net::Item& item, const game::ItemFlagsFormatter& flags = {},
                                         const game::ItemDescriptionAppender& description = {});

} // namespace hailstone::ui
