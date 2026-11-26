#include "app/spawn_interaction.h"

#include "game/race_models.h"
#include "game/targeting.h"
#include "net/game_connection.h"

namespace hailstone::app {

bool right_click_spawn(net::GameConnection& conn, std::uint32_t id, bool consider_npc) {
    const auto& world = conn.world();
    auto found = world.spawns.find(id);
    if (found == world.spawns.end()) return false;
    const game::Spawn& sp = found->second;
    // CEverQuest::RightClickedOnPlayer: corpses loot, merchants (class 41) and bankers (40) open their window.
    if (sp.is_npc && !sp.is_corpse && sp.class_id == 41) {
        conn.set_target(id);
        conn.open_merchant(id);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id == 40) {
        conn.set_target(id);
        conn.open_bank(id);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id == game::kMercLiaisonClass) {
        // A mercenary liaison: the mercenary merchant window (within 20).
        conn.set_target(id);
        conn.open_merc_liaison(id);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id == 70) { // a point (alternate currency) merchant
        conn.set_target(id);
        conn.open_point_merchant(id);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && net::GameConnection::adventure_merchant_type(sp.class_id)) {
        // an adventure merchant (LDoN, Discord, Norrath's Keepers, Dark Reign)
        conn.set_target(id);
        conn.open_adventure_merchant(id);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id == 66) { // a guild banker: the guild bank (within 20)
        conn.set_target(id);
        conn.open_guild_bank(id);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id == 60) { // an adventure recruiter: its window (within 20)
        conn.set_target(id);
        conn.open_adventure_recruiter(id);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && (sp.class_id == 63 || sp.class_id == 64)) {
        // a tribute master (64: the guild's): its window, when the server agrees
        conn.set_target(id);
        conn.open_tribute_master(id);
        return true;
    }
    if (sp.is_npc && !sp.is_corpse && sp.class_id >= 20 && sp.class_id <= 35) { // guildmasters
        conn.set_target(id);
        conn.open_trainer(id);
        return true;
    }
    // An NPC whose vf58 holds (a small boat) is taken or let go, and nothing else happens.
    if (conn.right_click_boat(id)) return true;
    if (!sp.is_corpse && (!sp.is_npc || sp.trader || sp.buyer)) {
        // A trader's shop or a buyer's barter window (a player's or an NPC's), or a player inspected (with
        // /toggleinspect on).
        conn.set_target(id);
        conn.right_click_player(id);
        return true;
    }
    if (sp.is_corpse) {
        conn.set_target(id);
        conn.loot(id);
        return true;
    }
    // CEverQuest::RightClickedOnPlayer @0x537cf8 considers the clicked spawn, preserving the current target.
    if (!consider_npc || id == world.player_id || !game::is_targetable(sp) ||
        game::is_vehicle_race(sp.race, sp.gender))
        return false;
    conn.consider(id);
    return true;
}

} // namespace hailstone::app
