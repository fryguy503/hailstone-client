// Numbers of the player as the original computes them that no stock label shows, for UIs with a fuller stats page
// (ui/modern/inventory.lua):
//
//   stats = hs.CharacterStats()   -- nil before the character exists; else { hpRegen, manaRegen, enduranceRegen (per
//                                    tick, with items: Get*Regen), hpRegenBase, manaRegenBase, enduranceRegenBase
//                                    (without items), maxHp, maxMana, maxEndurance (with buffs), maxHpBase,
//                                    maxManaBase, maxEnduranceBase (without), ac (CalcAC), defense (ComputeDefense),
//                                    weight (CalculateWeight), maxAir (MaxAirSupply), maxLevel }
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
    return 1;
}

} // namespace

void register_stats_api(lua_State* L, UiHost& h) {
    static const luaL_Reg fns[] = {{"CharacterStats", CharacterStats}, {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
