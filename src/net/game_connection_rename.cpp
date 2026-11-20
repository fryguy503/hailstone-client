// GameConnection: OP_GMLastName and OP_LevelAppearance (net/rename_packets.h).

#include "core/ascii_case.h"
#include "core/log.h"
#include "net/game_connection.h"

namespace hailstone::net {

std::uint32_t GameConnection::find_spawn_by_name(std::string_view name) const {
    // GetSpawnByName compares case-insensitively (FUN_00809250, _stricmp) and answers none for an empty name.
    if (name.empty()) return 0;
    for (const auto& [id, spawn] : world_.spawns)
        if (iequals(spawn.name, name)) return id;
    return 0;
}

bool GameConnection::handle_spawn_rename(const AppPacket& p) {
    switch (from_wire(p.opcode)) {
    case Op::GMLastName:
        if (auto r = decode_gm_last_name(p)) apply_gm_last_name(*r);
        return true;
    case Op::LevelAppearance:
        if (auto r = decode_level_appearance(p)) apply_level_appearance(*r);
        return true;
    default:
        return false;
    }
}

void GameConnection::apply_gm_last_name(const GMLastName& r) {
    // Handle_OP_GMLastName @0x4bbcd0: the spawn is looked up by name; no spawn or apply (@196) 0 ignores the packet.
    // The last name is copied to the spawn (+0x38), the profile's own too (0x4d5470) for the player, then the name
    // sprite is redrawn (0x58f050; here the views keyed on WorldState::spawn_set_serial make their copies again).
    if (!r.apply) return;
    const std::uint32_t id = find_spawn_by_name(r.name);
    if (id == 0) return;
    world_.spawns.at(id).last_name = r.last_name;
    if (id == world_.player_id && profile_) profile_->last_name = r.last_name;
    ++world_.spawn_set_serial;
}

void GameConnection::apply_level_appearance(const LevelAppearance& r) {
    // The case at 0x4d0053 / 0x58fed0: the spawn is found by id; each group with an effect id above 0 is an effect
    // for the spawn's model (CActor vf126): the effect id indexes the actor emitter file, the slot the model's spell
    // points, the flag decides how it is placed. The model, the slot and the file are the app's to check
    // (app/actor_emitters.h start_effect), which has the model; here only the packet and the spawn are.
    if (!world_.spawns.contains(r.spawn_id)) return;
    for (const auto& g : r.groups) {
        if (!level_appearance_group_used(g)) continue;
        log::debug("level appearance: spawn {} effect {} slot {} flag {}", r.spawn_id, g.effect_id, g.slot, g.flag);
        level_effect_cues_.push_back({r.spawn_id, g.effect_id, g.slot, g.flag != 0});
    }
    if (level_effect_cues_.size() > kLevelEffectCuesKept)
        level_effect_cues_.erase(level_effect_cues_.begin(),
                                 level_effect_cues_.end() - static_cast<std::ptrdiff_t>(kLevelEffectCuesKept));
}

} // namespace hailstone::net
