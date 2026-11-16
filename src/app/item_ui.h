#pragma once

// Glue between net::GameConnection's items and the item windows
// (ui/item_windows.h): builds the window input each frame, draws them and
// passes the player's clicks back to the connection.

#include <memory>

#include "game/item_links.h"
#include "app/commerce_ui.h"
#include "ui/item_windows.h"

namespace hailstone { class GameDir; }
namespace hailstone::render { class VkContext; }
namespace hailstone::net { class GameConnection; }

namespace hailstone::app {

class ItemUi {
public:
    ItemUi(render::VkContext& vk, const GameDir& game);
    ~ItemUi();

    void draw(net::GameConnection& conn);
    void toggle_inventory() { state_.show_inventory = !state_.show_inventory; }
    // The items the item display windows show (CItemDisplayWnd; the Lua windows read them).
    std::vector<net::Item>& item_displays() { return state_.info; }
    std::vector<std::uint64_t>& item_display_ids() { return state_.info_ids; }
    void close_item_display(int index);
    // The item icons (null when the sheets could not be loaded).
    ui::ItemIconCache* icons() { return icons_.get(); }
    void open_bag(std::int16_t general_slot) { state_.open_bags.insert(general_slot); }
    // The bag keys on the temporary bag windows (CInvSlotMgr_OpenAllBags / CloseAllBags; the Lua windows' are
    // UiApi::open_all_bags and the rest).
    void open_all_bags(const net::GameConnection& conn);
    void close_all_bags() {
        state_.open_bags.clear();
        bags_closed_ = true;
    }
    void toggle_all_bags(const net::GameConnection& conn) {
        if (bags_closed_) open_all_bags(conn);
        else close_all_bags();
    }
    // An item information window for the item (a right click on it).
    void inspect(const net::Item& item);
    // A clicked chat link: asks the server for the item (its answer opens an
    // item information window).
    void link_clicked(net::GameConnection& conn, const game::ItemLink& link);

private:
    std::unique_ptr<ui::ItemIconCache> icons_;
    ui::ItemWindowsState state_;
    bool bags_closed_ = true; // CInvSlotMgr +0x2010 (app/lua_ui_items.cpp's Containers::all_closed)
    CommerceUi commerce_; // merchant, trade and give windows (app/commerce_ui.h)
};

} // namespace hailstone::app
