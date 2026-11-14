#pragma once

// Builds a CharacterSheet from what the clean client knows about the local
// player: the player profile, the inventory, the buff slots and the live
// status. Fields the client has no source for yet keep the sheet's defaults
// (see docs/mechanics.md "Inputs").

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "game/mechanics/character_sheet.h"
#include "game/leadership_bonuses.h"

namespace hailstone::net {
struct PlayerProfile;
struct NewZone;
}

namespace hailstone::game {
class Inventory;
struct SpellState;
struct WorldState;

namespace mechanics {

struct LiveState {
    LeadershipBonuses leadership;
    std::int32_t level = 0;            // the current level (OP_LevelUpdate), 0 = the profile's
    std::uint32_t class_mask = 0;      // the server extension's class mask (bit c-1 = class c; 0 = the profile's)
    std::uint32_t focus_classes = 0;   // GameConnection::focus_class_mask (bit c-1; 0 = the stock focus class rules)
    const SkillRules* skill_rules = nullptr; // the server extension's skill rules (game/server_ext.h); null = stock
    CharacterVitals vitals;            // the server extension's HP / mana / endurance answers (game/server_ext.h)
    std::int64_t hp = 0, max_hp = 0;   // the server's (OP_HPUpdate)
    std::uint32_t mana = 0, endurance = 0;
    std::uint32_t hunger = 1, thirst = 1; // OP_Stamina (0 = starving / dehydrated)
    bool sitting = false;
    std::uint32_t sitting_ms = 0;      // how long the player has been sitting
    bool fast_regen_resting = false;   // PcZoneClient_IsRestingForFastRegen (OP_RestState and the buffs)
    bool attacking = false;            // auto attack on
    bool running = false;              // moving at run speed
    std::optional<std::int32_t> drunkenness; // current PcBase value; unset uses the received profile
    std::int32_t owner_status = 0;     // CharacterBase status (3: unconscious), used by bleeding HP regen
    std::uint32_t player_id = 0;       // own spawn id (caster checks)
    std::int64_t now_ms = 0;           // the buff clock (game::BuffSlot::received_ms)
    std::int32_t pvp_rules = 0;        // 0xdcee70 (OP_LogServer +4): any rule set scales PvP resist debuffs x 1.5
};

CharacterSheet build_sheet(const net::PlayerProfile& profile, const Inventory& inventory, const SpellState& spells,
                           const LiveState& live, const net::NewZone* zone = nullptr,
                           const WorldState* world = nullptr);
// The same into `s`, which it resets first, but reusing the storage of its items, buffs and abilities: a sheet
// built every frame copies the possessions (bags and their contents) without allocating while they stay the same.
// `abilities` sets the owned AA ranks into the last call's list, reusing its storage
// (AltAdvState::sheet_abilities_into); an empty function leaves none. Nothing of the last call's list stays.
using SheetAbilitiesInto = std::function<void(std::vector<SheetAbility>&)>;
void build_sheet_into(CharacterSheet& s, const net::PlayerProfile& profile, const Inventory& inventory,
                      const SpellState& spells, const LiveState& live, const SheetAbilitiesInto& abilities,
                      const net::NewZone* zone = nullptr, const WorldState* world = nullptr);

} // namespace mechanics
} // namespace hailstone::game
