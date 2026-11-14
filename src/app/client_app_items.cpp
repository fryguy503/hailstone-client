// ClientApp: Item input (loot, inventory), auto-scribe and the item windows.

#include "app/client_app_internal.h"
#include "game/interact.h"

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
    if (player_menu_ && eq_api_) {
        const auto& world = conn_->world();
        auto it = world.spawns.find(picked.id);
        if (it != world.spawns.end() && game::opens_player_menu(it->second, picked.id == world.player_id) &&
            eq_api_->open_player_menu(picked.id, right_down_pos_.x, right_down_pos_.y)) {
            conn_->set_target(picked.id);
            return;
        }
    }
    right_click_spawn(picked.id);
}

bool ClientApp::right_click_spawn(std::uint32_t id) {
    const auto& world = conn_->world();
    auto found = world.spawns.find(id);
    if (found == world.spawns.end()) return false;
    const std::optional<std::uint32_t> hit = id;
    const game::Spawn& sp = found->second;
    // CEverQuest::RightClickedOnPlayer: corpses loot, merchants (class 41) and bankers (40) open their window.
    if (sp.is_npc && !sp.is_corpse && sp.class_id == 41) {
        conn_->set_target(*hit);
        conn_->open_merchant(*hit);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id == 40) {
        conn_->set_target(*hit);
        conn_->open_bank(*hit);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id == game::kMercLiaisonClass) {
        // A mercenary liaison: the mercenary merchant window (within 20).
        conn_->set_target(*hit);
        conn_->open_merc_liaison(*hit);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id == 70) { // a point (alternate currency) merchant
        conn_->set_target(*hit);
        conn_->open_point_merchant(*hit);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && net::GameConnection::adventure_merchant_type(sp.class_id)) {
        // an adventure merchant (LDoN, Discord, Norrath's Keepers, Dark Reign)
        conn_->set_target(*hit);
        conn_->open_adventure_merchant(*hit);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id == 66) { // a guild banker: the guild bank (within 20)
        conn_->set_target(*hit);
        conn_->open_guild_bank(*hit);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id == 60) { // an adventure recruiter: its window (within 20)
        conn_->set_target(*hit);
        conn_->open_adventure_recruiter(*hit);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && (sp.class_id == 63 || sp.class_id == 64)) {
        // a tribute master (64: the guild's): its window, when the server agrees
        conn_->set_target(*hit);
        conn_->open_tribute_master(*hit);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id >= 20 && sp.class_id <= 35) { // guildmasters
        conn_->set_target(*hit);
        conn_->open_trainer(*hit);
        return true;
    }
    // An NPC whose vf58 holds (a small boat) is taken or let go, and nothing else happens.
    if (conn_->right_click_boat(id)) return true;
    if (!sp.is_corpse && (!sp.is_npc || sp.trader || sp.buyer)) {
        // A trader's shop or a buyer's barter window (a player's or an NPC's), or a player inspected (with
        // /toggleinspect on).
        conn_->set_target(*hit);
        conn_->right_click_player(*hit);
        return true;
    }
    if (!sp.is_corpse) return false;
    conn_->set_target(*hit);
    conn_->loot(*hit);
    return true;
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
    if (opts_.show_items && player_ready_) {
        // Testing aid (--show-items).
        if (items_since_ == Clock::time_point{}) items_since_ = Clock::now();
        if (Clock::now() - items_since_ > std::chrono::seconds(4)) {
            const auto& inv = conn_->inventory();
            if (opts_.show_items & 1) {
                item_ui_->toggle_inventory();
                if (eq_api_) eq_api_->show("InventoryWindow", true);
                for (std::int16_t s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s)
                    if (const auto* it = inv.at(net::ItemSlot::possessions(s)); it && it->is_bag()) {
                        item_ui_->open_bag(s);
                        if (eq_api_) eq_api_->right_click_item(net::ItemSlot::possessions(s));
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
            if (opts_.show_items & 32) eq_host_.show_spell(200, false); // a spell display (testing)
            if ((opts_.show_items & 16) && eq_api_) { // the EQ menu, its Character submenu open (testing)
                eq_api_->click_child("EQMainWnd", "EQButton");
                eq_api_->cascade_hover(0, 2);
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
    item_ui_->draw(*conn_);
    if (game_view_.chat.clicked_link) {
        item_ui_->link_clicked(*conn_, *game_view_.chat.clicked_link);
        game_view_.chat.clicked_link.reset();
    }
    // (INVENTORY, I by default, goes through the key map: key_command.)
}

} // namespace hailstone::app::client_internal
