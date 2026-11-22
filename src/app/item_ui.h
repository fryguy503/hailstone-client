#pragma once

// Glue between net::GameConnection's items and the item windows
// (ui/item_windows.h): builds the window input each frame, draws them and
// passes the player's clicks back to the connection.

#include <cstdint>
#include <map>
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
    // Without item icons (a headless run or a test): the windows draw, an icon shows its name instead.
    ItemUi() = default;
    ~ItemUi();

    void draw(net::GameConnection& conn);
    void toggle_inventory() { state_.show_inventory = !state_.show_inventory; }
    // The items the item display windows show (CItemDisplayWnd; the Lua windows read them).
    std::vector<net::Item>& item_displays() { return state_.info; }
    std::vector<std::uint64_t>& item_display_ids() { return state_.info_ids; }
    void close_item_display(int index);
    // The item icons (null when the sheets could not be loaded).
    ui::ItemIconCache* icons() { return icons_.get(); }
    void open_bag(net::GameConnection& conn, std::int16_t general_slot);
    // The bag keys on the temporary bag windows (CInvSlotMgr_OpenAllBags / CloseAllBags; the Lua windows' are
    // UiApi::open_all_bags and the rest).
    void open_all_bags(net::GameConnection& conn);
    void close_all_bags(net::GameConnection& conn);
    // The bank window hiding (CBankWnd::OnShow @0x627760 -> CContainerMgr::CloseAllContainers @0x65d4e0) closes
    // every open bag, the carried ones too, each with its close wave. Called once a frame before the windows draw.
    void close_bags_when_bank_hides(net::GameConnection& conn);
    void toggle_all_bags(net::GameConnection& conn) {
        if (bags_closed_) open_all_bags(conn);
        else close_all_bags(conn);
    }
    // An item information window for the item (a right click on it).
    void inspect(const net::Item& item);
    // A clicked chat link: asks the server for the item (its answer opens an
    // item information window).
    void link_clicked(net::GameConnection& conn, const game::ItemLink& link);

private:
    std::unique_ptr<ui::ItemIconCache> icons_;
    ui::ItemWindowsState state_;
    // The type of each bag that is open, recorded when it opened: a close wave uses the bag that stood there, even
    // when the slot has since lost or replaced it.
    std::map<std::int16_t, std::uint32_t> open_bag_types_;
    std::uint64_t bank_serial_ = 0; // the bank session last seen; 0 while the window is hidden
    bool bags_closed_ = true; // CInvSlotMgr +0x2010 (app/lua_ui_items.cpp's Containers::all_closed)
    CommerceUi commerce_; // merchant, trade and give windows (app/commerce_ui.h)
};

} // namespace hailstone::app
