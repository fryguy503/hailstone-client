// Numbers of the player as the original computes them that no stock label shows, for UIs with a fuller stats page
// (ui/modern/inventory.lua):
//
//   stats = hs.CharacterStats()   -- nil before the character exists; else { hpRegen, manaRegen, enduranceRegen (per
//                                    tick, with items: Get*Regen), hpRegenBase, manaRegenBase, enduranceRegenBase
//                                    (without items), maxHp, maxMana, maxEndurance (with buffs), maxHpBase,
//                                    maxManaBase, maxEnduranceBase (without), ac (CalcAC), defense (ComputeDefense),
//                                    weight (CalculateWeight), maxAir (MaxAirSupply), maxLevel, bonusCaps }
//   bonusCaps: the RoF2 cap of each capped item bonus, by its label (EQType 215 spell shielding .. 224
//   strikethrough; CharacterZoneClient_GetModCap). The labels give the sums before the caps (a server may allow
//   more); heal amount, spell damage and clairvoyance (225..227) have none.
#include <lua.hpp>

#include "app/lua_ui.h"
#include "game/mechanics/character.h"

namespace hailstone::app {

namespace {

int CharacterStats(lua_State* L) {
    auto& h = *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    const game::mechanics::Character* ch = h.character;
    if (!ch || !h.in_game) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 18);
    auto set = [&](const char* k, lua_Number v) {
        lua_pushnumber(L, v);
        lua_setfield(L, -2, k);
    };
    set("hpRegen", ch->hp_regen(true));
    set("manaRegen", ch->mana_regen(true));
    set("enduranceRegen", ch->endurance_regen(true));
    set("hpRegenBase", ch->hp_regen(false));
    set("manaRegenBase", ch->mana_regen(false));
    set("enduranceRegenBase", ch->endurance_regen(false));
    set("maxHp", ch->max_hp(true));
    set("maxMana", ch->max_mana(true));
    set("maxEndurance", ch->max_endurance(true));
    set("maxHpBase", ch->max_hp(false));
    set("maxManaBase", ch->max_mana(false));
    set("maxEnduranceBase", ch->max_endurance(false));
    set("ac", ch->calc_ac());
    set("defense", ch->compute_defense(true));
    set("weight", ch->calculate_weight());
    set("maxAir", ch->max_air_supply());
    set("maxLevel", ch->max_level());
    // EQType 215..224 and their mod cap kinds: spell shielding 4, combat effects 5, shielding 6, damage shield 7,
    // DoT shielding 8, damage shield mitigation 9, avoidance 10, accuracy 11, stun resist 12, strikethrough 13.
    lua_createtable(L, 0, 10);
    for (int i = 0; i < 10; ++i) {
        lua_pushinteger(L, game::mechanics::Character::mod_cap(4 + i));
        lua_rawseti(L, -2, 215 + i);
    }
    lua_setfield(L, -2, "bonusCaps");
    return 1;
}

} // namespace

void register_stats_api(lua_State* L, UiHost& h) {
    static const luaL_Reg fns[] = {{"CharacterStats", CharacterStats}, {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
