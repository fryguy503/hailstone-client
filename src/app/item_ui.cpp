#include "app/item_ui.h"

#include <imgui.h>

#include "core/log.h"
#include "net/game_connection.h"
#include "game/server_ext.h"

namespace hailstone::app {

ItemUi::ItemUi(render::VkContext& vk, const GameDir& game) {
    try {
        icons_ = std::make_unique<ui::ItemIconCache>(vk, game);
    } catch (const std::exception& e) {
        log::warn("item icons unavailable: {}", e.what());
    }
}

ItemUi::~ItemUi() = default;

void ItemUi::inspect(const net::Item& item) { ui::show_item_info(state_, item); }

void ItemUi::open_bag(net::GameConnection& conn, std::int16_t general_slot) {
    if (!state_.open_bags.insert(general_slot).second) return;
    if (const net::Item* bag = conn.inventory().at(net::ItemSlot::possessions(general_slot));
        bag && bag->is_bag())
        open_bag_types_[general_slot] = bag->bag_type;
}

void ItemUi::open_all_bags(net::GameConnection& conn) {
    for (std::int16_t g = net::kSlotGeneral1; g <= net::kSlotGeneral10; ++g) {
        const net::Item* bag = conn.inventory().at(net::ItemSlot::possessions(g));
        if (bag && bag->is_bag() && bag->placeable_flags() == 0 && state_.open_bags.insert(g).second) {
            open_bag_types_[g] = bag->bag_type;
            conn.container_sound(bag->bag_type, true);
        }
    }
    bags_closed_ = false;
}

void ItemUi::close_all_bags(net::GameConnection& conn) {
    // CInvSlotMgr_CloseAllBags closes each open bag through CContainerMgr::CloseContainer, which sounds it.
    for (std::int16_t g : state_.open_bags) {
        auto it = open_bag_types_.find(g);
        if (it == open_bag_types_.end()) continue;
        conn.container_sound(it->second, false);
        open_bag_types_.erase(it);
    }
    state_.open_bags.clear();
    bags_closed_ = true;
}

void ItemUi::close_bags_when_bank_hides(net::GameConnection& conn) {
    const std::uint64_t serial = conn.bank() ? conn.bank()->serial : 0;
    if (bank_serial_ != 0 && serial != bank_serial_) close_all_bags(conn);
    bank_serial_ = serial;
}

void ItemUi::link_clicked(net::GameConnection& conn, const game::ItemLink& link) { conn.request_item(link); }

void ItemUi::draw(net::GameConnection& conn) {
    while (auto item = conn.take_item_display()) ui::show_item_info(state_, std::move(*item));
    while (auto item = conn.take_item_refresh()) ui::refresh_item_info(state_, *item);

    ui::ItemWindowsInput in;
    in.inventory = &conn.inventory();
    in.coins = conn.coins();
    in.class_mask = conn.player_class_mask();
    in.item_flags = [&conn](const net::Item& item, std::string_view flags) { return conn.server_ext().item_flags(conn, item, flags); };
    in.item_description_append = [&conn](const net::Item& item) { return conn.server_ext().item_description_append(conn, item); };
    if (const auto& loot = conn.loot_session(); loot && loot->open) {
        in.corpse_id = loot->corpse_id;
        in.corpse_name = loot->corpse_name;
        in.loot = &loot->items;
        in.loot_money = loot->money;
    }

    // Before the windows redraw: a bank hide closes every bag, so its close wave is not also owed to the open-set
    // diff below.
    close_bags_when_bank_hides(conn);
    const std::set<std::int16_t> was_open = state_.open_bags;
    ui::ItemWindowsActions act = ui::item_windows(state_, in, icons_.get());
    // A bag opened or closed in the windows (right click, its close button, or one that is no longer there):
    // CContainerMgr's sound, chosen by the type of the bag that was open. The item may already have left or been
    // replaced, so a close uses the type recorded when it opened, never whatever sits in the slot now.
    for (std::int16_t g : state_.open_bags)
        if (!was_open.contains(g))
            if (const net::Item* bag = conn.inventory().at(net::ItemSlot::possessions(g))) {
                open_bag_types_[g] = bag->bag_type;
                conn.container_sound(bag->bag_type, true);
            }
    for (std::int16_t g : was_open) {
        if (state_.open_bags.contains(g)) continue;
        auto it = open_bag_types_.find(g);
        if (it == open_bag_types_.end()) continue;
        conn.container_sound(it->second, false);
        open_bag_types_.erase(it);
    }
    if (act.click) conn.click_slot(*act.click, ImGui::GetIO().KeyShift, ImGui::GetIO().KeyCtrl);
    if (act.destroy) conn.destroy_held();
    if (act.auto_inventory) conn.auto_inventory();
    if (act.loot) conn.loot_item(act.loot->first, act.loot->second);
    if (act.loot_all) conn.loot_all();
    if (act.close_loot) conn.end_loot();
    commerce_.draw(conn, icons_.get());
}

void ItemUi::close_item_display(int index) {
    if (index < 0 || index >= static_cast<int>(state_.info.size())) return;
    state_.info.erase(state_.info.begin() + index);
    state_.info_ids.erase(state_.info_ids.begin() + index);
}

} // namespace hailstone::app
