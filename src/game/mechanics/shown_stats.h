#pragma once

// The numbers the inventory window shows for the player (__GetLabelFromEQ's AC and ATK cases), from a Character
// that has run process_all_stats. The stock labels (app/lua_eq_values.cpp) and the item comparison
// (game/item_compare.h) read them here so both show the same values.

#include <cstdint>

namespace hailstone::game::mechanics {

class Character;

// The worn primary's skill as 0x584f60 picks it: a common (type 0) item that is a weapon gives ItemTypeToSkill of
// its item type, anything else hand to hand (28).
std::uint32_t attack_skill(const Character& ch);

// CharacterZoneClient_GetOffenseValue @0x453e30 (skill, include buffs).
std::int32_t offense_value(const Character& ch, std::uint32_t skill, bool buffs);

// 0x453f50 (skill, include buffs) with buffs: `spell_2434` whether the character has spell 2434's buff, which moves
// 100 of the buffs' ATK to the capped item part.
std::int32_t attack_value(const Character& ch, std::uint32_t skill, bool spell_2434);

// Label case 0x15: the displayed AC (0x580330), (ComputeDefense + CalcAC) x 1000 / 847.
std::int64_t shown_ac(const Character& ch);

// Label case 0x16: ATK, 0x584f60(1): (offense + attack) x 1000 / 744.
std::int64_t shown_atk(const Character& ch, bool spell_2434);

// Whether any of the sheet's buff slots holds spell 2434 (the sheet's own buffs, for a copy with no connection).
bool has_spell_2434(const Character& ch);

} // namespace hailstone::game::mechanics
