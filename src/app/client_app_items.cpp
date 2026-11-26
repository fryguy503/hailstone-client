// ClientApp: Item input (loot, inventory), auto-scribe and the item windows.

#include "app/client_app_internal.h"
#include "app/lua_ui_windows.h"
#include "app/spawn_interaction.h"
#include "game/interact.h"
#include "game/item_notify.h"

namespace hailstone::app::client_internal {

// Items: a right click on a corpse loots it, as in the original; I toggles the inventory (the original's default
// key); clicked chat links ask for the item.
void ClientApp::update_item_input() {
    // CEverQuest::RMouseUp's click (update_mouse_look: no look, or a right-button look of at most 500 ms), at the
    // place the button went down.
    if (!right_click_) return;
    right_click_ = false;
    // CEverQuest::RMouseUp: ClickedPlayer, then RightClickedOnPlayer.
    PickTarget picked = pick_at(right_down_pos_.x, right_down_pos_.y, game::kClickRequired, game::kClickSkip, "right click");
    auto sx = static_cast<std::int32_t>(right_down_pos_.x), sy = static_cast<std::int32_t>(right_down_pos_.y);
    // RMouseUp: nothing while placing; a placed object asks for its menu (0x21ec); so does a spawn the server marked
    // as a real estate NPC item, before its own right click.
    if (conn_->placement().active()) return;
    // Click to move (the isometric camera's, or mouse.click_move): an NPC or a corpse out of reach is walked to, then
    // right clicked.
    if (click_moves() && iso_click(right_down_pos_, picked, true)) return;
    if (picked.kind == PickTarget::Kind::Placed && world_objects_) {
        if (auto b = world_objects_->placed_bounds(picked.re, picked.item))
            conn_->re_request_placed_menu(picked.re, picked.item, b->center, b->radius, sx, sy);
        return;
    }
    if (picked.kind == PickTarget::Kind::Spawn && conn_->re_npc_item(picked.id) &&
        conn_->re_request_npc_item_menu(picked.id, sx, sy))
        return;
    if (picked.kind != PickTarget::Kind::Spawn) return;
    // Hailstone (a deliberate difference, game::opens_player_menu): another player's menu over them, where the
    // original inspects; with the switch off, context menus off or the player down, the original's right click.
    // The gamepad's Interact keeps the original's (right_click_spawn).
    if (player_menu_ && ui_api_) {
        const auto& world = conn_->world();
        auto it = world.spawns.find(picked.id);
        if (it != world.spawns.end() && game::opens_player_menu(it->second, picked.id == world.player_id) &&
            ui_api_->open_player_menu(picked.id, right_down_pos_.x, right_down_pos_.y)) {
            conn_->set_target(picked.id);
            return;
        }
    }
    right_click_spawn(picked.id, right_click_consider_);
}

bool ClientApp::right_click_spawn(std::uint32_t id, bool consider_npc) {
    return app::right_click_spawn(*conn_, id, consider_npc);
}

void ClientApp::auto_scribe(Clock::time_point now) {
    // Testing (--auto-scribe): what a player does with a new scroll, one step at a time: sit, pick the scroll up,
    // scribe it from the cursor into the first free book slot, memorize it into gem 1, wait until the gem is ready.
    if (scribe_step_ == 4 || now - scribe_at_ < std::chrono::milliseconds(1500)) return;
    const auto& spells = conn_->spell_state();
    const auto& inv = conn_->inventory();
    switch (scribe_step_) {
    case 0:
        for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s)
            if (const auto* it = inv.at(net::ItemSlot::possessions(s)); it && it->name.rfind("Spell: ", 0) == 0) {
                if (!conn_->sitting()) conn_->set_sitting(true);
                log::info("auto-scribe: picking up {} from general slot {}", it->name, s);
                conn_->click_slot(net::ItemSlot::possessions(s));
                scribe_step_ = 1;
                break;
            }
        if (scribe_step_ == 0) {
            log::warn("auto-scribe: no spell scroll in the bags");
            scribe_step_ = 4;
        }
        break;
    case 1:
        if (!inv.cursor()) break;
        log::info("auto-scribe: scribing {}", inv.cursor()->name);
        conn_->scribe_cursor(-1);
        scribe_step_ = 2;
        break;
    case 2:
        if (spells.scribing) break;
        for (int i = 0; i < game::kSpellBookSize; ++i)
            if (std::uint32_t id = spells.book[static_cast<std::size_t>(i)]; id != game::kNoSpell && id != 0) {
                log::info("auto-scribe: book slot {} holds spell {}; memorizing it into gem 1", i, id);
                conn_->memorize(i, 0);
                scribe_step_ = 3;
                break;
            }
        break;
    case 3:
        if (spells.memorizing || spells.memorize_sent_gem != -1 || spells.gems[0].spell == game::kNoSpell ||
            !spells.gem_castable(0, net::GameConnection::eq_time()))
            break;
        log::info("auto-scribe: gem 1 holds spell {} and is ready", spells.gems[0].spell);
        scribe_step_ = 4;
        break;
    default: break;
    }
    scribe_at_ = now;
}

void ClientApp::draw_items() {
    if (!item_ui_ || !conn_) return;
    // A bank window hiding closes every bag window (CBankWnd::OnShow @0x627760 -> CContainerMgr::CloseAllContainers
    // @0x65d4e0), carried bags too, each with its close wave. Checked every frame so a hide still closes them when
    // no key or click follows.
    prune_bag_windows(*conn_);
    if (opts_.show_items && player_ready_) {
        // Testing aid (--show-items).
        if (items_since_ == Clock::time_point{}) items_since_ = Clock::now();
        if (Clock::now() - items_since_ > std::chrono::seconds(4)) {
            const auto& inv = conn_->inventory();
            if (opts_.show_items & 1) {
                item_ui_->toggle_inventory();
                if (ui_api_) ui_api_->show("InventoryWindow", true);
                for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s)
                    if (const auto* it = inv.at(net::ItemSlot::possessions(s)); it && it->is_bag()) {
                        item_ui_->open_bag(*conn_, s);
                        if (ui_api_) ui_api_->right_click_item(net::ItemSlot::possessions(s));
                    }
            }
            if (opts_.show_items & 2) {
                for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s)
                    if (inv.at(net::ItemSlot::possessions(s))) {
                        conn_->click_slot(net::ItemSlot::possessions(s));
                        break;
                    }
            }
            if (opts_.show_items & 8) { // the first general-slot item onto the cursor, a stack whole (shift)
                for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s)
                    if (inv.at(net::ItemSlot::possessions(s))) {
                        conn_->click_slot(net::ItemSlot::possessions(s), true, false);
                        break;
                    }
            }
            if (opts_.show_items & 32) ui_host_.show_spell(200, false); // a spell display (testing)
            if ((opts_.show_items & 16) && ui_api_) { // the EQ menu, its Character submenu open (testing)
                ui_api_->click_child("EQMainWnd", "EQButton");
                ui_api_->cascade_hover(0, 2);
            }
            if (opts_.show_items & 4) {
                if (const auto* it = inv.at(net::ItemSlot::possessions(net::kSlotPrimary))) {
                    game::ItemLink link;
                    link.item_id = it->id;
                    item_ui_->link_clicked(*conn_, link);
                }
            }
            opts_.show_items = 0;
        }
    }
    item_ui_->draw(*conn_, stats_ui_ ? stats_ui_->character() : nullptr);
    if (game_view_.chat.clicked_link) {
        item_ui_->link_clicked(*conn_, *game_view_.chat.clicked_link);
        game_view_.chat.clicked_link.reset();
    }
    // (INVENTORY, I by default, goes through the key map: key_command.)
}


net::SlotClickKeys ClientApp::command_keys() const {
    if (forced_keys_) return *forced_keys_;
    const ImGuiIO& io = ImGui::GetIO();
    return {io.KeyShift, io.KeyCtrl, io.KeyAlt};
}

void ClientApp::run_with_keys(std::string_view line, net::SlotClickKeys keys) {
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    if (!conn_ || line.empty()) return;
    const std::optional<net::SlotClickKeys> before = forced_keys_; // (nested: /shiftkey /nomodkey ...)
    const auto mods_before = ui_host_.forced_mods;
    forced_keys_ = keys;
    ui_host_.forced_mods = std::array<bool, 2>{keys.shift, keys.ctrl};
    conn_->send_command(std::string(line));
    forced_keys_ = before;
    ui_host_.forced_mods = mods_before;
}

void ClientApp::notify_command(std::string_view args) {
    if (conn_) run_notify_command(ui_host_, ui_api_.get(), args);
}

bool ClientApp::window_act(std::string_view window, std::string_view child, std::string_view action) {
    return run_window_act(ui_host_, ui_api_.get(), window, child, action);
}

// MQ's ItemNotify on Hailstone's own clicks: leftmouseup is the slot's left click (CInvSlot::HandleLButtonUp: pick up,
// put down, loot), rightmouseup its right click (use, read, open or close a bag), rightmouseheld the item's display.
// MQ clicks the slot's window, so a closed bag's slot works only through its fallbacks; here every slot works as if
// its window were open.
void ClientApp::itemnotify_command(std::string_view args) {
    if (!conn_) return;
    const game::ItemNotify n = game::parse_item_notify(args);
    if (!n.error.empty()) {
        conn_->ui_message(n.error);
        return;
    }
    const auto& inv = conn_->inventory();
    std::optional<net::ItemSlot> slot = n.slot;
    if (!slot) { // an item carried, by id or exact name: the first in slot order, bags' contents after their bag
        auto match = [&](const net::Item& it) {
            return n.item_id ? it.id == n.item_id : iequals(it.name, n.item_name);
        };
        for (const auto& [key, it] : inv.items()) {
            if (key.first != net::kInvPossessions) continue;
            if (match(it)) {
                slot = net::ItemSlot::possessions(key.second);
                break;
            }
            for (const auto& b : it.contents)
                if (match(b.item)) {
                    slot = net::ItemSlot::possessions(key.second, static_cast<std::int16_t>(b.index));
                    break;
                }
            if (slot) break;
        }
        if (!slot) {
            conn_->ui_message(n.item_id ? std::format("Item '#{}' not found.", n.item_id)
                                        : std::format("Item '{}' not found.", n.item_name));
            return;
        }
    }
    // A slot in a bag: the bag must be there, and the slot one it has (MQ's messages).
    if (slot->sub >= 0) {
        const net::Item* bag = inv.at(slot->parent());
        if (!bag || !bag->is_bag()) {
            conn_->ui_message(std::format("There was no container in slot {}", slot->slot));
            return;
        }
        if (slot->sub >= bag->bag_slots) {
            conn_->ui_message(std::format("{} is not a valid slot for this container.", slot->sub));
            return;
        }
    }
    const net::SlotClickKeys keys = command_keys();
    switch (n.notification) {
    case game::ItemNotification::LeftMouseUp: conn_->click_slot(*slot, keys.shift, keys.ctrl); break;
    case game::ItemNotification::RightMouseUp:
        if (ui_api_) ui_api_->right_click_item(*slot, keys);
        else conn_->right_click_item(*slot, keys);
        break;
    case game::ItemNotification::RightMouseHeld: {
        const net::Item* it = nullptr;
        if (slot->type == net::kInvCorpse) {
            const auto& loot = conn_->loot_session();
            if (loot && loot->open)
                if (auto f = loot->items.find(slot->slot); f != loot->items.end()) it = &f->second;
        } else {
            it = inv.at(*slot);
        }
        if (it && item_ui_) item_ui_->inspect(*it);
        break;
    }
    }
}

} // namespace hailstone::app::client_internal
