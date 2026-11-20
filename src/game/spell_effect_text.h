// SPDX-License-Identifier: GPL-2.0-only
#pragma once

// A spell's effect slots as text, one line per slot ("Slot 1: Increase Hitpoints by 30"), as MacroQuest's
// ParseSpellEffect gives them for ${Spell.Attrib}-style displays and /spellslotinfo. For plugins and scripts that were
// written against MacroQuest's text; the client's own windows use game/spell_description.h.
//
// Ported from MacroQuest (GPL-2.0, Copyright (C) 2002-present MacroQuest Authors; THIRD_PARTY.md):
// ParseSpellEffect, CalcValue, CalcMinSpellLevel, CalcMaxSpellLevel, CalcValueRange, CalcExtendedRange,
// GetSpellRestrictions, GetClassesFromMask and the Format* helpers of src/main/MQ2Spells.cpp. Where MacroQuest reads
// the live client's memory, the text comes from here instead:
//   - spell names from the client's SpellTable ("Unknown Spell" when it has none);
//   - skill and faction names from the callbacks below (the game folder's string tables), "Unknown[n]" and
//     "Unknown Faction[n]" without one;
//   - race names from game::race_name, zone names from game/zone_names.h ("UNKNOWN_ZONE" for an unknown short name),
//     class short names from game::class_short_name;
//   - item names never: the client holds no item database, so a summoned item reads "[  id]" as MacroQuest's text
//     does for an item missing from its own.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::game {

struct SpellData;
class SpellTable;

struct SpellEffectTextContext {
    // The spells an effect names (procs, triggers, limits). Null: every one reads "Unknown Spell".
    const SpellTable* spells = nullptr;
    // A skill's name; null or "" reads "Unknown[n]".
    std::function<std::string(std::int32_t skill)> skill_name;
    // A faction's name (dbstr type 45); null or "" reads "Unknown Faction[n]".
    std::function<std::string(std::int32_t faction)> faction_name;
    // The level the value ranges run up to: ParseSpellEffect's `level`, 100 by default as ShowSpellSlotInfo calls it.
    std::int32_t level = 100;
};

// ParseSpellEffect for effect slot `slot` (0..11): "Slot <slot+1>: ..." or "" for a slot with nothing to show
// (SPA 254, the portal location slots, a CHA slot outside 2..255).
std::string spell_effect_text(const SpellData& spell, int slot, const SpellEffectTextContext& ctx);

// Every slot's text, in slot order, without the empty ones (ShowSpellSlotInfo's lines).
std::vector<std::string> spell_effect_lines(const SpellData& spell, const SpellEffectTextContext& ctx);

// MacroQuest's ${Spell.TargetType} text for a spells_us.txt target type (column 98), "Unknown" for one it does not
// name (src/main/datatypes/MQ2SpellType.cpp).
std::string_view spell_target_type_desc(int target_type);

// MacroQuest's ${Spell.ResistType} text for a spells_us.txt resist type (column 85): "Unresistable" for 0, "Unknown"
// for one it does not name (src/main/datatypes/MQ2SpellType.cpp).
std::string_view spell_resist_type_desc(int resist);

// MacroQuest's CalcValue: an effect's value at `level` after `tick` ticks, by its formula `calc`.
std::int64_t spell_calc_value(int calc, std::int64_t base, std::int64_t max, int tick, int minlevel, int level);

} // namespace hailstone::game
