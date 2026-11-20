// SPDX-License-Identifier: GPL-2.0-only
//
// Ported from MacroQuest's src/main/MQ2Spells.cpp (GPL-2.0, Copyright (C) 2002-present MacroQuest Authors;
// THIRD_PARTY.md): ParseSpellEffect and the helpers it calls, and the target and resist type names of
// ${Spell.TargetType} and ${Spell.ResistType} (src/main/datatypes/MQ2SpellType.cpp). The text is MacroQuest's, word for word, so that a
// plugin or script written against MacroQuest's slot text reads the same lines here; game/spell_effect_text.h says
// where the names come from instead of the live client's memory.

#include "game/spell_effect_text.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <string_view>

#include "game/eq_names.h"
#include "game/spa_ids.h"
#include "game/spells.h"
#include "game/zone_names.h"

namespace hailstone::game {

namespace {

// eqlib's MAX_PC_LEVEL in the MacroQuest build this is ported from: CalcMaxSpellLevel's answer for an effect with no
// max, and CalcMinSpellLevel's cut-off, both shown as "(L<n>)" in the text.
constexpr int kMaxPcLevel = 125;

// eqlib's eSpellValueRangeCalc.
constexpr int kCalcDecayTick1 = 107;
constexpr int kCalcDecayTick2 = 108;
constexpr int kCalcDecayTick5 = 120;
constexpr int kCalcDecayTick12 = 122;
constexpr int kCalcRandom = 123;

constexpr std::string_view kHeadingNormal[16] = {
    "north", "north by northeast", "northeast", "east by northeast", "east", "east by southeast", "southeast",
    "south by southeast", "south", "south by southwest", "southwest", "west by southwest", "west",
    "west by northwest", "northwest", "north by northwest",
};

std::int64_t iabs(std::int64_t v) { return v < 0 ? -v : v; }

std::string_view inc_dec(std::int64_t v) { return v < 0 ? "Decrease" : "Increase"; }

// GetSpellEffectName: a negative id (an exclusion in a limit) names the same effect.
std::string effect_name(std::int64_t id) {
    const std::int64_t a = iabs(id);
    if (a < static_cast<std::int64_t>(std::size(SPA_NAMES))) return std::string(SPA_NAMES[static_cast<std::size_t>(a)]);
    return std::format("Unknown SPA[{:03}]", a);
}

// EQSpellHeading: an EQ heading (0..511) as one of 16 compass points. MacroQuest indexes its table with the C++
// remainder, which goes negative for a heading above 512 (an index out of its table); here it wraps instead.
std::string_view heading_text(std::int64_t heading) {
    const std::int64_t h = (((512 - heading) % 512) + 512) % 512;
    return kHeadingNormal[static_cast<std::size_t>(h / 32)];
}

std::string zone_text(const std::string& short_name) {
    // GetFullZone(GetZoneID(extra)): an unknown short name gives "UNKNOWN_ZONE".
    const std::uint32_t id = zone_id_by_short_name(short_name);
    std::string name = id ? zone_long_name(id) : std::string();
    return name.empty() ? std::string("UNKNOWN_ZONE") : name;
}

struct Names {
    const SpellEffectTextContext& ctx;

    // GetSpellNameByID.
    std::string spell(std::int64_t id) const {
        if (ctx.spells && id > 0 && id <= static_cast<std::int64_t>(kMaxSpellId))
            if (const SpellData* s = ctx.spells->get(static_cast<std::uint32_t>(id)); s && !s->name.empty())
                return s->name;
        return "Unknown Spell";
    }
    // GetSpellNameBySpellGroupID: MacroQuest takes the first spell of the group whose SpellSubGroup is 0 (in its hash
    // chain's order), else the first of any subgroup. The SpellTable gives the group's highest rank of subgroup 0.
    std::string spell_group(std::int64_t group) const {
        if (ctx.spells && group > 0 && group <= INT32_MAX)
            if (const SpellData* s = ctx.spells->by_group_and_rank(static_cast<std::int32_t>(group), 0, -1, false);
                s && !s->name.empty())
                return s->name;
        return "Unknown Spell";
    }
    // szSkills[skill], "All Skills" for a negative one.
    std::string skill(std::int64_t id) const {
        if (id < 0) return "All Skills";
        if (ctx.skill_name && id <= INT32_MAX)
            if (std::string n = ctx.skill_name(static_cast<std::int32_t>(id)); !n.empty()) return n;
        return std::format("Unknown[{}]", id);
    }
    // MQ2Spells' GetFactionName: the faction's name in parentheses.
    std::string faction(std::int64_t id) const {
        std::string n;
        if (ctx.faction_name && id >= INT32_MIN && id <= INT32_MAX) n = ctx.faction_name(static_cast<std::int32_t>(id));
        if (n.empty()) n = std::format("Unknown Faction[{}]", static_cast<std::int32_t>(id));
        return "(" + n + ")";
    }
};

std::string resist_type_name(std::int64_t t) {
    switch (t) {
    case 1: return "Magic";
    case 2: return "Fire";
    case 3: return "Cold/Ice";
    case 4: return "Poison";
    case 5: return "Disease";
    case 6: return "Chromatic";
    case 7: return "Prismatic";
    case 8: return "Physical";
    case 9: return "Corruption";
    default: return std::format("Unknown[{}]", static_cast<std::int32_t>(t));
    }
}

std::string spell_type_name(std::int64_t t) {
    switch (t) {
    case 0: return "Detrimental only";
    case 1: return "Beneficial only";
    case 2: return "Beneficial - Group Only";
    default: return std::format("Unknown[{}]", static_cast<std::int32_t>(t));
    }
}

std::string target_type_limits_name(std::int64_t t) {
    switch (iabs(static_cast<std::int32_t>(t))) {
    case 50: return "Target AE No Players Pets";
    case 52: return "Single Friendly (or Target's Target";
    case 47: return "Pet Owner";
    case 46: return "Target of Target";
    case 45: return "Free Target";
    case 44: return "Beam";
    case 43: return "Single in Group";
    case 42: return "Directional AE";
    case 39: return "No Pets";
    case 38: return "Pet2";
    case 37: return "Caster PB NPC";
    case 36: return "Caster PB PC";
    case 35: return "Special Muramites";
    case 34: return "Chest";
    case 33: return "Hatelist2";
    case 32: return "Hatelist";
    case 41: return "Group v2";
    case 40: return "AE PC v2";
    case 25: return "AE Summoned";
    case 24: return "AE Undead";
    case 20: return "Targeted AE Tap";
    case 18: return "Uber Dragons";
    case 17: return "Uber Giants";
    case 16: return "Plant";
    case 15: return "Corpse";
    case 14: return "Pet";
    case 13: return "LifeTap";
    case 11: return "Summoned";
    case 10: return "Undead";
    case 9: return "Animal";
    case 8: return "Targeted AE";
    case 6: return "Self";
    case 5: return "Single";
    case 4: return "PB AE";
    case 3: return "Group v1";
    case 2: return "AE PC v1";
    case 1: return "Line of Sight";
    default: return std::format("Unknown[{}]", iabs(static_cast<std::int32_t>(t)));
    }
}

std::string stat_short_name(std::int64_t t) {
    switch (t) {
    case 0: return "STR";
    case 1: return "STA";
    case 2: return "AGI";
    case 3: return "DEX";
    case 4: return "WIS";
    case 5: return "INT";
    case 6: return "CHA";
    case 7: return "MR";
    case 8: return "CR";
    case 9: return "FR";
    case 10: return "PR"; // either PR or DR
    case 11: return "DR"; // either DR or PR
    default: return std::format("Unknown[{}]", static_cast<std::int32_t>(t));
    }
}

// GetSpellRestrictions: an HP/mana effect's or a doom's condition (its base2).
std::string restrictions(std::int64_t base2) {
    switch (base2) {
    case 0: return "None";
    case 100: return "Only works on Animal or Humanoid";
    case 101: return "Only works on Dragon";
    case 102: return "Only works on Animal or Insect";
    case 104: return "Only works on Animal";
    case 105: return "Only works on Plant";
    case 106: return "Only works on Giant";
    case 108: return "Doesn't work on Animals or Humanoids";
    case 109: return "Only works on Bixie";
    case 110: return "Only works on Harpy";
    case 111: return "Only works on Gnoll";
    case 112: return "Only works on Sporali";
    case 113: return "Only works on Kobold";
    case 114: return "Only works on Shade";
    case 115: return "Only works on Drakkin";
    case 117: return "Only works on Animals or Plants";
    case 118: return "Only works on Summoned";
    case 119: return "Only works on Fire_Pet";
    case 120: return "Only works on Undead";
    case 121: return "Only works on Living";
    case 122: return "Only works on Fairy";
    case 123: return "Only works on Humanoid";
    case 124: return "Undead HP Less Than 10%";
    case 125: return "Clockwork HP Less Than 45%";
    case 126: return "Wisp HP Less Than 10%";
    case 190: return "Doesn't work on Raid Bosses";
    case 191: return "Only works on Raid Bosses";
    case 201: return "HP Above 75%";
    case 203: return "HP Less Than 20%";
    case 204: return "HP Less Than 50%";
    case 216: return "Not In Combat";
    case 250: return "HP Less Than 35%";
    case 304: return "Chain Plate Classes";
    case 399: return "HP Between 15 and 25%";
    case 400: return "HP Between 1 and 25%";
    case 401: return "HP Between 25 and 35%";
    case 402: return "HP Between 35 and 45%";
    case 403: return "HP Between 45 and 55%";
    case 404: return "HP Between 55 and 65%";
    case 412: return "HP Above 99%";
    case 521: return "Mana Below X%";
    case 522: return "End Below 40%";
    case 523: return "Mana Below 40%";
    case 603: return "Only works on Undead2";
    case 608: return "Only works on Undead3";
    case 624: return "Only works on Summoned2";
    case 701: return "Doesn't work on Pets";
    case 818: return "Only works on Undead4";
    case 819: return "Doesn't work on Undead4";
    case 825: return "End Below 21%";
    case 826: return "End Below 25%";
    case 827: return "End Below 29%";
    case 836: return "Only works on Regular Servers";
    case 837: return "Doesn't work on Progression Servers";
    case 842: return "Only works on Humanoid Level 84 Max";
    case 843: return "Only works on Humanoid Level 86 Max";
    case 844: return "Only works on Humanoid Level 88 Max";
    case 1000: return "Between Level 1 and 75";
    case 1001: return "Between Level 76 and 85";
    case 1002: return "Between Level 86 and 95";
    case 1003: return "Between Level 96 and 100";
    case 1004: return "HP Less Than 80%";
    case 38311: return "Mana Below 20%";
    case 38312: return "Mana Below 10%";
    default: break;
    }
    // 221..240: "At Least <n> Pets On Hatelist"; 501..519: "HP Below <5n>%".
    if (base2 >= 221 && base2 <= 240)
        return std::format("At Least {} Pet{} On Hatelist", base2 - 220, base2 == 221 ? "" : "s");
    if (base2 >= 501 && base2 <= 519) return std::format("HP Below {}%", (base2 - 500) * 5);
    return std::format("Unknown[{}]", base2);
}

// GetClassesFromMask: bit c for class c (Warrior 1 .. Berserker 16).
std::string classes_from_mask(std::int32_t mask) {
    constexpr int kClasses = 16;
    int matching = 0;
    for (int c = 1; c <= kClasses; ++c)
        if (mask & (1 << c)) ++matching;
    const int excluding = kClasses - matching;
    if (matching == kClasses) return "ALL";
    const bool listExcluded = excluding <= 4;
    std::string out = listExcluded ? "ALL EXCEPT: " : "";
    bool first = true;
    for (int c = 1; c <= kClasses; ++c) {
        const bool set = (mask & (1 << c)) != 0;
        if (set == listExcluded) continue;
        if (!first) out += ',';
        first = false;
        out += class_short_name(static_cast<std::uint32_t>(c));
    }
    return out;
}

// CalcMinSpellLevel: the lowest level any class gets the spell, 1 when none of them can.
int min_spell_level(const SpellData& s) {
    int lowest = 255;
    for (int c = 1; c <= 16; ++c) lowest = std::min<int>(lowest, s.level_needed(static_cast<std::uint8_t>(c)));
    return lowest > kMaxPcLevel ? 1 : lowest;
}

int max_spell_level(int calc, std::int64_t base, std::int64_t max, int tick, int minlevel, int level) {
    if (iabs(max) > 0) {
        for (int l = 1; l <= level; ++l) {
            const int value = static_cast<int>(spell_calc_value(calc, base, max, tick, minlevel, l));
            if (iabs(value) >= iabs(max)) return l;
        }
        return level;
    }
    return kMaxPcLevel;
}

std::string value_range(int calc, std::int64_t base, std::int64_t max, int duration, int minlevel, int level,
                        std::string_view pct) {
    const std::int64_t start = spell_calc_value(calc, base, max, 1, minlevel, minlevel);
    const std::int64_t finish = spell_calc_value(calc, base, max, duration, minlevel, level);
    const std::string_view type = iabs(start) < iabs(finish) ? "Growing" : "Decaying";
    switch (calc) {
    case kCalcDecayTick1: return std::format(" ({} to {} @ 1/tick)", type, finish);
    case kCalcDecayTick2: return std::format(" ({} to {} @ 2/tick)", type, finish);
    case kCalcDecayTick5: return std::format(" ({} to {} @ 5/tick)", type, finish);
    case kCalcDecayTick12: return std::format(" ({} to {} @ 12/tick)", type, finish);
    case kCalcRandom: return std::format(" (Random: {} to {})", start, finish * (start >= 0 ? 1 : -1));
    default:
        if (calc > 0 && calc < 1000) return std::format(" to {}{}", start, pct);
        if (calc >= 1000 && calc < 2000) return std::format(" ({} to {} @ {}/tick)", type, finish, calc - 1000);
        return {};
    }
}

std::string extended_range(int calc, std::int64_t start, std::int64_t finish, int minlevel, int maxlevel,
                           std::string_view pct, bool ac_mod) {
    if (calc == kCalcRandom) return std::format(" (Random: {} to {})", start, finish * (start >= 0 ? 1 : -1));
    // An AC effect shows its value divided by 10/3, as float, as the client's AC display does.
    auto shown = [ac_mod](std::int64_t v) -> std::int64_t {
        return ac_mod ? static_cast<int>(static_cast<float>(iabs(v)) / (10.0f / 3.0f)) : iabs(v);
    };
    if (iabs(start) < iabs(finish))
        return std::format(" by {}{} (L{}) to {}{} (L{})", shown(start), pct, minlevel, shown(finish), pct, maxlevel);
    return std::format(" by {}{}", shown(finish), pct);
}

// ---- the Format* helpers

std::string f_at(std::string_view n, std::int64_t v, std::string_view prep = "by", std::string_view pct = "") {
    return std::format("{} {} {}{}", n, prep, iabs(v), pct);
}
std::string f_base(std::string_view n, std::int64_t b) { return std::format("{} ({})", n, b); }
std::string f_base(std::string_view n, std::int64_t b, std::string_view opt) { return std::format("{} {} ({})", n, opt, b); }
std::string f_base_pct(std::string_view n, std::int64_t b) { return std::format("{} ({}%)", n, b); }
std::string f_minmax_base(std::string_view n, std::int64_t b, std::int64_t spa) {
    return std::format("{} ({} {})", n, iabs(b), effect_name(spa));
}
std::string f_count(std::string_view n, std::int64_t v, std::string_view prep = "by", std::string_view pct = "") {
    return std::format("{} {} {} {}{}", inc_dec(v), n, prep, iabs(v), pct);
}
std::string f_extra(std::string_view n, std::string_view extra, std::string_view trigger = "",
                    std::string_view colon = ":") {
    return std::format("{}{} {}{}", n, colon, extra, trigger);
}
std::string f_limits(std::string_view n, std::int64_t v, std::string_view extra) {
    return std::format("{} ({} {})", n, extra, v < 0 ? "excluded" : "allowed");
}
std::string f_max(std::string_view n, std::int64_t v, std::int64_t max) {
    return std::format("{} {} by {} ({}% max)", inc_dec(max), n, iabs(max), v);
}
std::string f_penalty(std::string_view n, std::int64_t v, std::string_view what) {
    return v < 100 ? std::format("{} ({}% {})", n, v, what) : std::string(n);
}
std::string f_percent(std::string_view n, std::int64_t v, std::int64_t max, bool scaling = true, bool hundreds = false,
                      bool usepct = true) {
    const std::string_view pct = usepct ? "%" : "";
    if (hundreds) {
        const float fv = std::fabs(static_cast<float>(v) / 100.0f);
        const float fm = std::fabs(static_cast<float>(max) / 100.0f);
        if (v == max) {
            return scaling ? std::format("{} {} by {:.2f}{}", inc_dec(max), n, fm, pct)
                           : std::format("{} by {:.2f}{}", n, fm, pct);
        }
        return scaling ? std::format("{} {} by {:.2f}{} to {:.2f}{}", inc_dec(max), n, fv, pct, fm, pct)
                       : std::format("{} by {:.2f}{} to {:.2f}{}", n, fv, pct, fm, pct);
    }
    if (v == max) {
        return scaling ? std::format("{} {} by {}{}", inc_dec(max), n, iabs(max), pct)
                       : std::format("{} by {}{}", n, iabs(max), pct);
    }
    return scaling ? std::format("{} {} by {}{} to {}{}", inc_dec(max), n, iabs(v), pct, iabs(max), pct)
                   : std::format("{} by {}{} to {}{}", n, iabs(v), pct, iabs(max), pct);
}
std::string f_range(std::string_view n, std::int64_t v, std::string_view range, std::string_view extra = "") {
    return std::format("{} {}{}{}", inc_dec(v), n, range, extra);
}
std::string f_rate_mod(const Names& names, std::int64_t spell, std::int64_t rate) {
    return rate > 0 ? std::format("{} (rate mod {})", names.spell(spell), rate) : names.spell(spell);
}
std::string f_refresh_timer(std::string_view n, std::int64_t v, std::int64_t max, std::string_view skill,
                            std::string_view prep = "with") {
    if (v == max) return std::format("{} {} by {} sec {} {}", inc_dec(max), n, iabs(max), prep, skill);
    return std::format("{} {} by {} sec to {} sec {} {}", inc_dec(max), n, iabs(v), iabs(max), prep, skill);
}
std::string f_resists(std::string_view n, std::int64_t chance, std::int64_t spa) {
    return chance < 100 ? std::format("{} ({}% Chance)", effect_name(spa), chance) : std::string(n);
}
std::string f_seconds(std::string_view n, std::int64_t v, bool tens = false) {
    return tens ? std::format("{} ({}0.00 sec)", n, v) : std::format("{} ({} sec)", n, v);
}
std::string f_seconds(std::string_view n, float v) { return std::format("{} ({:.2f} sec)", n, v); }
std::string f_seconds_count(std::string_view n, float v, std::string_view prep = "by") {
    return std::format("{} {} {} {:.2f} sec", v < 0 ? "Decrease" : "Increase", n, prep, std::fabs(v));
}
std::string f_skills(std::string_view n, std::int64_t v, std::int64_t max, std::string_view skill, bool usepct = true,
                     std::string_view prep = "with") {
    return std::format("{} {} {}", f_percent(n, v, max, true, false, usepct), prep, skill);
}
// FormatSkillAttack's short form: the percent is base2's, the damage the value.
std::string f_skill_attack(std::string_view n, std::int64_t v, std::int64_t base2, std::string_view skill,
                           std::string_view prep = "with") {
    return std::format("{} {} {} for {} damage", f_percent(n, base2, base2), prep, skill, v);
}
std::string f_spell_chance(const Names& names, std::int64_t chance, std::int64_t spell) {
    return chance < 100 ? std::format(" ({}% Chance, Spell: {})", chance, names.spell(spell))
                        : std::format(" (Spell: {})", names.spell(spell));
}
std::string f_spell_group_chance(const Names& names, std::int64_t chance, std::int64_t group) {
    return chance < 100 ? std::format(" ({}% Chance, Spell: {})", chance, names.spell_group(group))
                        : std::format(" (Spell: {})", names.spell_group(group));
}
std::string f_stacking(std::string_view n, std::int64_t slot, std::int64_t v, std::int64_t max, int spa,
                       std::string_view extra) {
    const std::string_view which = spa == SPA_STACKING_BLOCK ? "new" : "existing";
    if (max > 0) return std::format("{} {} spell if slot {} is effect '{}' and < {}", n, which, slot, extra, v);
    return std::format("{} {} spell if slot {} is effect '{}'", n, which, slot, extra);
}
std::string f_stats_cap_range(std::string_view n, std::int64_t v, std::string_view stat, std::string_view range) {
    return std::format("{} {} {}{}", inc_dec(v), stat, n, range);
}
std::string f_string(std::string_view n, std::string_view extra, std::string_view trigger = "") {
    return std::format("{} {}{}", n, extra, trigger);
}
std::string f_timer(std::string_view n, float v) { return std::format("{} by {:.2f} sec", n, v); }

} // namespace

std::string_view spell_target_type_desc(int target_type) {
    switch (target_type) {
    case 0: return "None";
    case 1: return "Line of Sight";
    case 2: return "AE PC v1";
    case 3: return "Group v1";
    case 4: return "PB AE";
    case 5: return "Single";
    case 6: return "Self";
    case 8: return "Targeted AE";
    case 9: return "Animal";
    case 10: return "Undead";
    case 11: return "Summoned";
    case 13: return "LifeTap";
    case 14: return "Pet";
    case 15: return "Corpse";
    case 16: return "Plant";
    case 17: return "Uber Giants";
    case 18: return "Uber Dragons";
    case 20: return "Targeted AE Tap";
    case 24: return "AE Undead";
    case 25: return "AE Summoned";
    case 32: return "Hatelist";
    case 33: return "Hatelist2";
    case 34: return "Chest";
    case 35: return "Special Muramites";
    case 36: return "Caster PB PC";
    case 37: return "Caster PB NPC";
    case 38: return "Pet2";
    case 39: return "No Pets";
    case 40: return "AE PC v2";
    case 41: return "Group v2";
    case 42: return "Directional AE";
    case 43: return "Single in Group";
    case 44: return "Beam";
    case 45: return "Free Target";
    case 46: return "Target of Target";
    case 47: return "Pet Owner";
    case 50: return "Target_AE_No_Players_Pets";
    case 52: return "Single Friendly (or Target's Target"; // MacroQuest's text, its parenthesis unclosed
    default: return "Unknown";
    }
}

std::string_view spell_resist_type_desc(int resist) {
    switch (resist) {
    case 0: return "Unresistable";
    case 1: return "Magic";
    case 2: return "Fire";
    case 3: return "Cold";
    case 4: return "Poison";
    case 5: return "Disease";
    case 6: return "Chromatic";
    case 7: return "Prismatic";
    case 8: return "Physical";
    case 9: return "Corruption";
    default: return "Unknown";
    }
}

std::int64_t spell_calc_value(int calc, std::int64_t base, std::int64_t max, int tick, int minlevel, int level) {
    if (calc == 0) return base;
    if (calc == 100) {
        if (max > 0 && ((base > max) || (level > minlevel))) return max;
        return base;
    }
    // In 64 bits: MacroQuest's int products overflow (undefined) for a formula above 2000 + INT_MAX / level.
    const std::int64_t lv = level;
    const std::int64_t tk = tick;
    std::int64_t change = 0;
    switch (calc) {
    case 101: change = lv / 2; break;
    case 102: change = lv; break;
    case 103: change = lv * 2; break;
    case 104: change = lv * 3; break;
    case 105: change = lv * 4; break;
    case 106: change = lv * 5; break;
    case 107: change = -1 * tk; break;
    case 108: change = -2 * tk; break;
    case 109: change = lv / 4; break;
    case 110: change = lv / 6; break;
    case 111: if (lv > 16) change = (lv - 16) * 6; break;
    case 112: if (lv > 24) change = (lv - 24) * 8; break;
    case 113: if (lv > 34) change = (lv - 34) * 10; break;
    case 114: if (lv > 44) change = (lv - 44) * 15; break;
    case 115: if (lv > 15) change = (lv - 15) * 7; break;
    case 116: if (lv > 24) change = (lv - 24) * 10; break;
    case 117: if (lv > 34) change = (lv - 34) * 13; break;
    case 118: if (lv > 44) change = (lv - 44) * 20; break;
    case 119: change = lv / 8; break;
    case 120: change = -5 * tk; break;
    case 121: change = lv / 3; break;
    case 122: change = -12 * tk; break;
    case 123: if (tk > 1) change = iabs(max) - iabs(base); break; // random in range
    case 124: if (lv > 50) change = lv - 50; break;
    case 125: if (lv > 50) change = (lv - 50) * 2; break;
    case 126: if (lv > 50) change = (lv - 50) * 3; break;
    case 127: if (lv > 50) change = (lv - 50) * 4; break;
    case 128: if (lv > 50) change = (lv - 50) * 5; break;
    case 129: if (lv > 50) change = (lv - 50) * 10; break;
    case 130: if (lv > 50) change = (lv - 50) * 15; break;
    case 131: if (lv > 50) change = (lv - 50) * 20; break;
    case 132: if (lv > 50) change = (lv - 50) * 25; break;
    case 139: if (lv > 30) change = (lv - 30) / 2; break;
    case 140: if (lv > 30) change = lv - 30; break;
    case 141: if (lv > 30) change = 3 * (lv - 30) / 2; break;
    case 142: if (lv > 30) change = 2 * (lv - 30); break;
    case 143: change = 3 * lv / 4; break;
    case 3000: return base;
    default:
        if (calc > 0 && calc < 1000) change = lv * calc;
        if (calc >= 1000 && calc < 2000) change = tk * (calc - 1000) * -1;
        if (calc >= 2000) change = lv * (calc - 2000);
    }
    std::int64_t value = iabs(base) + change;
    if (max != 0 && value > iabs(max)) value = iabs(max);
    if (base < 0) value = -value;
    return value;
}

std::string spell_effect_text(const SpellData& spell, int slot, const SpellEffectTextContext& ctx) {
    if (slot < 0 || slot >= static_cast<int>(spell.effect.size())) return {};
    const auto i = static_cast<std::size_t>(slot);
    const Names names{ctx};
    const int level = ctx.level;

    const std::int64_t id = spell.id;
    const int spa = spell.effect[i];
    std::int64_t base = spell.base[i];
    std::int64_t base2 = spell.base2[i];
    std::int64_t max = spell.max[i];
    const int calc = spell.formula[i];
    const std::int32_t spellgroup = spell.spell_group;
    const int ticks = static_cast<int>(spell.duration_cap);
    const std::int32_t targets = spell.max_targets;
    const int targettype = spell.target_type;
    const int skill = spell.skill;
    // The raw base of slots 1..4, which the teleport effects take as x, y, z and heading.
    auto slot_base = [&spell](std::size_t k) -> std::int64_t { return spell.base[k]; };

    // Effects that show nothing.
    switch (spa) {
    case SPA_CHA:
        if (base <= 1 || base > 255) return {};
        break;
    case SPA_NOSPELL:          // not a spell effect
    case SPA_PORTAL_LOCATIONS: // the teleport's parameters, shown with it
        return {};
    default: break;
    }

    switch (spa) {
    case SPA_HASTE:
    case SPA_HEIGHT:
    case SPA_BARD_HASTE: // base 100 is none
        base -= 100;
        max -= 100;
        break;
    case SPA_SUMMON_CORPSE: // base and max swapped
        max = base;
        base = 0;
        break;
    case SPA_FOCUS_DAMAGE_MOD:
    case SPA_FOCUS_HEAL_MOD:
    case SPA_FOCUS_MANACOST_MOD: // base2 is the max
        max = base2;
        break;
    case SPA_FOCUS_REAGENT_MOD:
    case SPA_FOCUS_DAMAGE_AMT_DETRIMENTAL: // base2 is the base
        base = base2;
        break;
    default: break;
    }

    const std::string name = effect_name(spa);
    const std::string& extra = spell.teleport_zone;

    const int minspelllvl = min_spell_level(spell);
    const int maxspelllvl = max_spell_level(calc, base, max, ticks, minspelllvl, level);
    const std::int64_t value =
        spell_calc_value(calc, spa == SPA_STACKING_BLOCK ? max : base, max, 1, minspelllvl, minspelllvl);
    const std::int64_t finish =
        spell_calc_value(calc, spa == SPA_FOCUS_INCOMING_DMG_MOD ? base2 : base, max, ticks, minspelllvl, level);

    const bool usePercent =
        spa == SPA_MOVEMENT_RATE || spa == SPA_HASTE || spa == SPA_BARD_HASTE || spa == SPA_FOCUS_DAMAGE_MOD ||
        spa == SPA_FOCUS_HEAL_MOD || spa == SPA_DOUBLE_ATTACK || spa == SPA_STUN_RESIST || spa == SPA_PROC_CHANCE ||
        spa == SPA_DIVINE_SAVE || spa == SPA_METABOLISM || spa == SPA_TRIPLE_BACKSTAB || spa == SPA_DOTCRIT ||
        spa == SPA_HEALCRIT || spa == SPA_MENDCRIT || spa == SPA_FLURRY || spa == SPA_PET_FLURRY ||
        spa == SPA_SPELL_CRIT_CHANCE || spa == SPA_SHIELD_BLOCK_CHANCE || spa == SPA_FOCUS_DAMAGE_MOD_CRIT ||
        spa == SPA_FOCUS_INCOMING_DMG_MOD || spa == SPA_CANCEL_NEGATIVE_MAGIC;
    // eqlib's TT_PBAE, TT_TARGETED_AE, TT_AE_PC_V2 and TT_DIRECTIONAL: an AE one ("on up to N enemies").
    const bool aeEffect = targettype == kTargetAECaster || targettype == kTargetAETarget ||
                          targettype == kTargetAEClientV2 || targettype == kTargetDirectional;
    const std::string_view pct = usePercent ? "%" : "";

    const std::string range = value_range(calc, base, max, ticks, minspelllvl, level, pct);
    const std::string extendedrange =
        extended_range(calc, value, finish, minspelllvl, maxspelllvl, pct, spa == SPA_AC || spa == SPA_AC2);
    const std::string repeating = ticks ? " per tick " : "";
    const std::string maxlevel = max ? std::format(" up to level {}", max) : std::string();
    const std::string maxtargets = targets && aeEffect ? std::format(" on up to {} enemies", targets) : std::string();

    std::string out = std::format("Slot {}: ", slot + 1);
    auto with_restrictions = [&] {
        if (base2) out += " -- Restrictions: " + restrictions(base2);
    };
    // The teleports' " to x, y, z in <zone> facing <direction>".
    auto location = [&](std::string_view zone) {
        return std::format(" to {}, {}, {} in {} facing {}", slot_base(0), slot_base(1), slot_base(2), zone,
                           heading_text(slot_base(3)));
    };

    switch (spa) {
    case SPA_HP: // heals, regen, direct damage
    case SPA_MANA:
        out += f_range(name, value, extendedrange);
        out += repeating;
        out += maxtargets;
        with_restrictions();
        break;
    case SPA_AC:
    case SPA_ATTACK_POWER:
    case SPA_MOVEMENT_RATE:
    case SPA_STR:
    case SPA_DEX:
    case SPA_AGI:
    case SPA_STA:
    case SPA_INT:
    case SPA_WIS:
    case SPA_CHA:
    case SPA_HASTE:
    case SPA_NPC_FACTION:
    case SPA_DISEASE:
    case SPA_POISON:
    case SPA_STONESKIN:
    case SPA_MAX_HP:
    case SPA_MAX_MANA:
    case SPA_BARD_HASTE:
    case SPA_AMPLIFICATION:
    case SPA_MODIFY_ALL_STATS:
    case SPA_DOUBLE_ATTACK:
    case SPA_INCREASE_MAX_ENDURANCE:
    case SPA_STUN_RESIST:
    case SPA_STRIKETHROUGH1:
    case SPA_PROC_CHANCE:
    case SPA_DIVINE_SAVE:
    case SPA_DOTCRIT:
    case SPA_HEALCRIT:
    case SPA_MENDCRIT:
    case SPA_FLURRY:
    case SPA_PET_FLURRY:
    case SPA_FOCUS_DAMAGE_AMT:
    case SPA_SPELL_CRIT_CHANCE:
    case SPA_FOCUS_DAMAGE_MOD_CRIT:
    case SPA_FOCUS_DAMAGE_AMT_CRIT:
    case SPA_CRITICAL_HP_REGEN:
    case SPA_SHIELD_BLOCK_CHANCE:
    case SPA_AC2:
    case SPA_MANA2:
        out += f_range(name, value, extendedrange);
        break;
    case SPA_INVISIBILITY:
    case SPA_NPC_FRENZY:
    case SPA_NPC_AWARENESS:
    case SPA_NPC_AGGRO:
    case SPA_BLINDNESS:
    case SPA_INVIS_VS_UNDEAD:
    case SPA_INVIS_VS_ANIMALS:
    case SPA_CONFUSE:
    case SPA_DETECT_HOSTILE:
    case SPA_DETECT_MAGIC:
    case SPA_NO_TWINCAST:
    case SPA_INVULNERABILITY:
    case SPA_BANISH:
    case SPA_SHADOW_STEP:
    case SPA_BERSERK:
    case SPA_LYCANTHROPY:
    case SPA_VAMPIRISM:
    case SPA_DETECT_TRAPS:
    case SPA_DETECT_UNDEAD:
    case SPA_DETECT_SUMMONED:
    case SPA_DETECT_ANIMALS:
    case SPA_TRUE_NORTH:
    case SPA_TRANSFER_ITEM:
    case SPA_ITEM_LORE:
    case SPA_ITEM_IDENTIFY:
    case SPA_SPIN_STUN:
    case SPA_INFRAVISION:
    case SPA_ULTRAVISION:
    case SPA_EYE_OF_ZOMM:
    case SPA_RECLAIM_ENERGY:
    case SPA_CORPSE_BOMB:
    case SPA_PRESERVE_CORPSE:
    case SPA_BIND_SIGHT:
    case SPA_VENTRILOQUISM:
    case SPA_SENTINEL:
    case SPA_LOCATE_CORPSE:
    case SPA_ENCHANT_LIGHT:
    case SPA_SUMMON_TARGET:
    case SPA_WEATHER_CONTROL:
    case SPA_FRAGILE:
    case SPA_SACRIFICE:
    case SPA_ROOT:
    case SPA_COMPLETEHEAL:
    case SPA_PET_FEARLESS:
    case SPA_CALL_PET:
    case SPA_ALTER_PET_LEVEL:
    case SPA_CORNUCOPIA:
    case SPA_HIT_MAGIC:
    case SPA_IMMUNITY:
    case SPA_FOCUS_INSTANT_ONLY:
    case SPA_POP_RESURRECT:
    case SPA_MIRROR:
    case SPA_OBJECT_SENSE_TRAP:
    case SPA_OBJECT_DISARM_TRAP:
    case SPA_OBJECT_PICKLOCK:
    case SPA_AMNESIA:
    case SPA_FADE:
    case SPA_TAUNT:
    case SPA_ILLUSION_OTHERS:
    case SPA_MASS_GROUP_BUFF:
    case SPA_RAMPAGE:
    case SPA_AE_TAUNT:
    case SPA_FLESH_TO_BONE:
    case SPA_NO_FIZZLE:
    case SPA_DOPPELGANGER:
    case SPA_APPRAISAL:
    case SPA_ZONE_SUSPEND_MINION:
    case SPA_TELEPORT_CASTERS_BINDPOINT:
    case SPA_FOCUS_COMBAT_SKILL:
    case SPA_IMPROVED_INVIS:
    case SPA_IMPROVED_INVIS_UNDEAD:
    case SPA_IMPROVED_INVIS_ANIMALS:
    case SPA_GATE_STARTING_CITY:
    case SPA_SUMMON_TO_CORPSE:
    case SPA_FOCUSED_IMMUNITY:
    case SPA_EXPEDIENT_RECOVERY:
    case SPA_NO_PANIC:
    case SPA_FLY:
    case SPA_OVERRIDE_NOT_FOCUSABLE:
    case SPA_AA_BUY_NEXT_RANK:
        out += name;
        break;
    case SPA_SEE_INVIS:
    case SPA_ENDURING_BREATH:
        out += f_base(name, value);
        break;
    case SPA_STUN: // base: ms
        if (base2 != 0 && base != base2)
            out += f_string(name, std::format(" NPC for {:.0f}s (PC for {:.0f}s){}", static_cast<float>(base) / 1000.0f,
                                              static_cast<float>(base2) / 1000.0f, maxlevel));
        else
            out += f_string(name, std::format(" for {:.0f}s{}", static_cast<float>(base) / 1000.0f, maxlevel));
        out += maxtargets;
        break;
    case SPA_CHARM:
    case SPA_FEAR:
    case SPA_ENTHRALL:
        out += f_string(name, maxlevel);
        out += maxtargets;
        break;
    case SPA_FATIGUE:
    case SPA_ARCHERY:
    case SPA_FIZZLE_SKILL:
    case SPA_INSTANT_ENDURANCE:
    case SPA_INCREASE_CASTING_LEVEL:
    case SPA_FOCUS_DAMAGE_AMT_DETRIMENTAL:
    case SPA_INCREASE_NEGATIVE_HP_LIMIT:
    case SPA_WORN_ATTACK_CAP:
    case SPA_INSTANT_MANA:
    case SPA_CORRUPTION:
    case SPA_RESIST_CORRUPTION:
    case SPA_DIRECTIONAL_TELEPORT:
    case SPA_FOCUS_HEAL_AMT:
    case SPA_FOCUS_HEAL_AMT_BENEFICIAL:
    case SPA_FOCUS_HEAL_AMT_CRIT:
    case SPA_FOCUS_INCREASE_SKILL_DMG_2:
    case SPA_FOCUS_ENDURANCE_MIN:
    case SPA_FOCUS_ENDURANCE_MAX:
    case SPA_PET_ADD_ATK:
    case SPA_MELEE_DMG_POSITION_AMT:
    case SPA_DMG_TAKEN_POSITION_AMT:
    case SPA_AMPLIFY_AMT:
    case SPA_FOCUS_RESIST_INCOMING:
        out += f_count(name, value);
        break;
    case SPA_BIND_AFFINITY:
        out += f_string(name, base == 2 ? " (Secondary Bind Point)" : base == 3 ? " (Tertiary Bind Point)" : "");
        break;
    case SPA_GATE:
        out += f_string(name, base == 2 ? " to Secondary Bind Point" : base == 3 ? " (Tertiary Bind Point)" : "");
        break;
    case SPA_DISPEL_MAGIC:
    case SPA_LEVITATION:
    case SPA_NPC_ANTI_GATE:
    case SPA_FOCUS_LEVEL_MIN:
    case SPA_FOCUS_PET:
    case SPA_PURGE_POISON:
    case SPA_CANCEL_BENEFICIAL:
    case SPA_DESTRUCTIVE_FORCE:
    case SPA_SLAY_UNDEAD:
    case SPA_REDUCE_WEIGHT:
    case SPA_BLOCK_BEHIND:
    case SPA_ACROBATICS:
    case SPA_CAST_THROUGH_STUN:
    case SPA_EXTENDED_SHIELDING:
    case SPA_POISON_MASTERY:
    case SPA_FOCUS_CHANNELING:
    case SPA_FREE_PET:
    case SPA_PET_AFFINITY:
    case SPA_PERM_ILLUSION:
    case SPA_STONEWALL:
    case SPA_STRING_UNBREAKABLE:
    case SPA_IMPROVE_RECLAIM_ENERGY:
    case SPA_INCREASE_CHANGE_MEMWIPE:
    case SPA_ENHANCED_ROOT:
    case SPA_TRAP_CIRCUMVENTION:
    case SPA_INCREASE_MAX_SKILL:
    case SPA_EXTRA_SPECIALIZATION:
    case SPA_OFFHAND_MIN_WEAPON_DAMAGE:
    case SPA_ENDLESS_QUIVER:
    case SPA_CHAOTIC_STAB:
    case SPA_SHROUD_OF_STEALTH:
    case SPA_GIVE_PET_HOLD:
    case SPA_TRADESKILL_MASTERY:
    case SPA_REDUCE_AA_TIMER:
    case SPA_ADD_2H_ATTACK_CHANCE:
    case SPA_ADD_PET_COMMANDS:
    case SPA_FIRST_AID:
    case SPA_DUAL_WIELD_AMT:
    case SPA_EXTRA_DI_CHANCE:
    case SPA_FINISHING_BLOW:
    case SPA_PET_FEIGN:
    case SPA_INCREASE_BANDAGE_AMT:
    case SPA_WU_ATTACK:
    case SPA_IMPROVE_LOH:
    case SPA_NIMBLE_EVASION:
    case SPA_ADD_PROC_HIT:
    case SPA_INCREASE_RUN_SPEED_CAP:
    case SPA_PURIFY:
    case SPA_STRIKETHROUGH:
    case SPA_STUN_RESIST2:
    case SPA_REDUCE_SPECIAL_TIMER:
    case SPA_WAKE_DEAD:
    case SPA_INCREASE_RANGE_DMG:
    case SPA_WEAK_DEAD_2:
    case SPA_OBSERVER:
    case SPA_FORAGE_MASTER:
    case SPA_INCREASE_WORN_HP_REGEN_CAP:
    case SPA_INCREASE_WORN_MANA_REGEN_CAP:
    case SPA_NO_BREAK_AE_SNEAK:
    case SPA_ADD_SPELL_SLOTS:
    case SPA_ADD_BUFF_SLOTS:
    case SPA_ILLUSIONARY_TARGET:
    case SPA_ITEM_CHANNELING:
    case SPA_ASSASSINATE_MAX_LEVEL:
    case SPA_HEADSHOT_MAX_LEVEL:
    case SPA_DOUBLE_RANGED_ATTACK:
    case SPA_FOCUS_MANA_MIN:
    case SPA_INCREASE_SHIELD_DMG:
    case SPA_INCREASE_TRAP_COUNT:
    case SPA_INCREASE_SOI_COUNT:
    case SPA_DEACTIVATE_ALL_TRAPS:
    case SPA_LEARN_TRAP:
    case SPA_CHANGE_TRIGGER_TYPE:
    case SPA_FOCUS_MUTE:
    case SPA_PASSIVE_SENSE_TRAP:
    case SPA_POTION_BELT:
    case SPA_BANDOLIER:
    case SPA_GROUP_SHIELDING:
    case SPA_GRANT_FORAGING:
    case SPA_FLING:
    case SPA_FLING_TOWARD:
    case SPA_FLING_AT:
    case SPA_SUMMON_ALL_CORPSES:
    case SPA_LOCKOUT_SPELL_TIMER:
    case SPA_FOCUS_MANA_MAX:
    case SPA_FOCUS_HEAL_MOD_CRIT:
    case SPA_ADD_PET_AC:
    case SPA_FOCUS_SPELL_CLASS:
    case SPA_FOCUS_SPELL_SUBCLASS:
    case SPA_STAFF_BLOCK_CHANCE:
    case SPA_FOCUS_LIMIT_RACE: // MacroQuest: "needs work (base2 bitmask of races)"
    case SPA_FOCUS_LIMIT_SKILL:
    case SPA_FOCUS_LIMIT_ITEM_CLASS:
    case SPA_FOCUS_LIMIT_USE:
    case SPA_FOCUS_LIMIT_USE_AMT:
    case SPA_FOCUS_LIMIT_USE_MIN:
    case SPA_FOCUS_LIMIT_USE_TYPE:
    case SPA_ADD_EXTENDED_TARGET_SLOTS:
    case SPA_POST_EFFECT:
    case SPA_POST_EFFECT_DATA:
    case SPA_EXPAND_MAX_ACTIVE_TROPHY_BENEFITS:
    case SPA_FREEZE_BUFF_TIMER:
    case SPA_ASSASSINATE:
    case SPA_FINISHING_BLOW_MAX:
    case SPA_DISTANCE_REMOVAL:
    case SPA_ADD_MERC_SLOT:
    case SPA_STACKER_A:
    case SPA_STACKER_B:
    case SPA_STACKER_C:
    case SPA_STACKER_D:
    case SPA_FOCUS_DAMAGE_MOD_2:
    case SPA_FOCUS_DAMAGE_AMT_2:
    case SPA_SHIELD:
    case SPA_DAMAGE_SHIELD_MITIGATION_AMT:
    case SPA_DOUBLE_BACKSTAB_FRONT:
    case SPA_PET_MELEE_CRIT_DMG_MOD:
    case SPA_WEAPON_STANCE:
    case SPA_HATELIST_TO_TOP:
    case SPA_WORN_ENDURANCE_REGEN_CAP:
        out += f_base(name, base);
        break;
    case SPA_NPC_AGGRO_RADIUS: // the reverse sign from normal base values
        out += f_count(name, -value);
        out += maxlevel;
        break;
    case SPA_NPC_HELP_RADIUS: // the reverse sign from normal base values
        out += f_count(name, -value);
        out += maxlevel;
        out += maxtargets;
        break;
    case SPA_CREATE_ITEM: // no item database: MacroQuest's text for an item not in its own
        out += f_extra(name, std::format("[{:5}] (Qty:{})", base, calc));
        break;
    case SPA_CREATE_ITEM_IN_BAG:
        out += f_extra(name, std::format("[{:5}]", base));
        break;
    case SPA_SUMMON_PET:
    case SPA_CREATE_UNDEAD:
    case SPA_BEASTLORD_PET:
    case SPA_FAMILIAR:
    case SPA_SUMMON_MOUNT:
        out += f_extra(name, extra);
        break;
    case SPA_RESIST_FIRE:
    case SPA_RESIST_COLD:
    case SPA_RESIST_POISON:
    case SPA_RESIST_DISEASE:
    case SPA_RESIST_MAGIC:
    case SPA_SPELL_SHIELD:
    case SPA_INSTANT_HP:
    case SPA_HATE:
    case SPA_RESIST_ALL:
    case SPA_HATE_OVER_TIME:
        out += f_range(name, value, extendedrange);
        out += maxtargets;
        break;
    case SPA_CHANGE_FORM: // base: the race
        out += f_extra(name, race_name(static_cast<std::uint32_t>(base)));
        break;
    case SPA_DAMAGE_SHIELD: // the reverse sign from normal base values
    case SPA_FEEDBACK:
    case SPA_METABOLISM:
        out += f_range(name, -value, extendedrange);
        break;
    case SPA_NPC_WIPE_HATE_LIST:
        out += f_penalty(name, value + 40, "Chance");
        out += maxtargets;
        break;
    case SPA_FEIGN_DEATH:
        out += f_penalty(name, value, "Chance");
        break;
    case SPA_RESURRECT:
        out += f_string(name, std::format(" and restore {}% experience", value));
        out += maxtargets;
        break;
    case SPA_PORTAL: {
        const std::string where = location(zone_text(extra));
        out += f_string(name, (targettype == kTargetSelf ? " Self" : " Group") + where);
        break;
    }
    case SPA_HP_NPC_ONLY: // a toss on the z axis
        out += f_base(name, iabs(base), base >= 0 ? " Down" : " Up");
        break;
    case SPA_MELEE_PROC:
    case SPA_RANGE_ABILITY:
    case SPA_DEFENSIVE_PROC:
    case SPA_PROC_EFFECT_2:
    case SPA_SKILL_PROC:
    case SPA_SKILL_PROC_SUCCESS:
    case SPA_ADD_NORMALIZED_SKILL_MIN_DMG_AMT:
        out += f_extra(name, f_rate_mod(names, base, base2));
        break;
    case SPA_MAGNIFICATION:
    case SPA_HEIGHT:
    case SPA_MODIFY_HATE:
    case SPA_ATTACK_SPEED_MAX:
    case SPA_HEALMOD:
    case SPA_FOCUS_DAMAGE_MOD:
    case SPA_FOCUS_HEAL_MOD:
    case SPA_FOCUS_DURATION_MOD:
    case SPA_FOCUS_RANGE_MOD:
    case SPA_FOCUS_HATE_MOD:
    case SPA_REFLECT:
    case SPA_CRITICAL_SPELL:
    case SPA_CRIPPLING_BLOW:
    case SPA_EVASION:
    case SPA_RIPOSTE:
    case SPA_DODGE:
    case SPA_PARRY:
    case SPA_DUAL_WIELD:
    case SPA_PURETONE:
    case SPA_SANCTIFICATION:
    case SPA_FEARLESS:
    case SPA_HUNDRED_HANDS:
    case SPA_BLOCK:
    case SPA_DOUBLE_RIPOSTE:
    case SPA_ADD_RIPOSTE:
    case SPA_GIVE_DOUBLE_ATTACK:
    case SPA_2H_BASH:
    case SPA_BASH_CHANCE:
    case SPA_ENHANCED_CHARM:
    case SPA_INCREASE_PROC_CHANCE:
    case SPA_BACKSTAB_FRONT:
    case SPA_TRIPLE_BACKSTAB:
    case SPA_FOCUS_DAMAGE_MOD_DETRIMENTAL:
    case SPA_FAIL_ALCHEMY_ITEM_RECOVERY:
    case SPA_INCREASE_EXP_MOD:
    case SPA_ADD_TRIPLE_ATTACK_CHANCE:
    case SPA_SLOW:
    case SPA_CRIT_DOT_DMG_MOD:
    case SPA_FOCUS_HEAL_MOD_BENEFICIAL:
    case SPA_FOCUS_TWINCAST_CHANCE:
    case SPA_ADD_NORMALIZED_SKILL_MIN_DMG_AMT_2:
    case SPA_FRAGILE_DEFENSE:
    case SPA_ADD_HATE_PCT:
    case SPA_ADD_HATE_OVER_TIME_PCT:
    case SPA_FACTION_MOD:
    case SPA_PC_PET_RAMPAGE:
    case SPA_PC_PET_AE_RAMPAGE:
    case SPA_PC_PET_FLURRY:
    case SPA_DAMAGE_SHIELD_MITIGATION_PCT:
    case SPA_DOUBLE_MELEE_ATTACKS:
    case SPA_SKILL_BASE_DAMAGE_MOD:
    case SPA_ADD_EXTRA_PRIMARY_ATTACK_PCT:
    case SPA_ADD_EXTRA_SECONDARY_ATTACK_PCT:
    case SPA_FOCUS_CAST_TIME_MOD2:
    case SPA_ATTACK_ACCURACY_MAX:
        out += f_percent(name, value, finish);
        break;
    case SPA_EVACUATE:
        out += f_string(name, location(extra));
        break;
    case SPA_IGNORE_PET:
    case SPA_SUMMON_CORPSE:
        out += f_string(name, maxlevel);
        break;
    case SPA_SILENCE:
        out += name;
        out += maxtargets;
        break;
    case SPA_HEALDOT: // pet heals, regen
    case SPA_ENDURANCE:
        out += f_range(name, value, extendedrange);
        out += repeating;
        break;
    case SPA_TRANSLOCATE:
        // As MacroQuest tests it: the zone only for a short name that starts with '0' (std::string::rfind('0', 0)
        // is 0, negated true), "to Bind Point" for any other.
        if (!extra.empty() && extra.rfind('0', 0) == 0)
            out += f_string(name, location(zone_text(extra)));
        else
            out += f_string(name, " to Bind Point");
        break;
    case SPA_CURSE:
        out += f_count(name, value);
        out += maxtargets;
        break;
    case SPA_IRONMAIDEN:
    case SPA_DEFENDER_MELEE_FORCE_PCT:
        out += f_base(name, -base);
        break;
    case SPA_REDUCESKILL:
    case SPA_SKILL_INCREASE_CHANCE:
    case SPA_ACCURACY:
    case SPA_SKILL_DAMAGE_MOD:
    case SPA_MIN_DAMAGE_DONE_MOD:
    case SPA_CRIT_ATTACK_MODIFIER:
    case SPA_SKILL_DAMAGE_MOD_2:
        out += f_skills(name, value, finish, names.skill(base2));
        break;
    case SPA_FOCUS_RESIST_MOD:
    case SPA_FOCUS_CAST_TIME_MOD:
    case SPA_FOCUS_REAGENT_MOD:
    case SPA_FOCUS_MANACOST_MOD:
    case SPA_FOCUS_STUNTIME_MOD:
    case SPA_DEFENSIVE:
    case SPA_TINY_COMPANION:
    case SPA_DAMAGE_SHIELD_MOD:
        out += f_percent(name, -value, -finish);
        break;
    case SPA_FOCUS_LEVEL_MAX:
        if (base2 > 0)
            out += std::format("{} ({}) (lose {}% per level over cap)", name, base, base2);
        else
            out += f_base(name, base);
        break;
    case SPA_FOCUS_RESIST_TYPE:
        out += f_limits(name, value, resist_type_name(base));
        break;
    case SPA_FOCUS_TARGET_TYPE:
        out += f_limits(name, value, target_type_limits_name(base));
        break;
    case SPA_FOCUS_WHICH_SPA:
        out += f_limits(name, value, effect_name(base));
        break;
    case SPA_FOCUS_BENEFICIAL:
        out += f_limits(name, value, spell_type_name(base));
        break;
    case SPA_FOCUS_WHICH_SPELL:
        out += f_limits(name, value, names.spell(base));
        break;
    case SPA_FOCUS_DURATION_MIN: // base: ticks
    case SPA_FOCUS_DURATION_MAX:
        out += f_seconds(name, value * 6);
        break;
    case SPA_FOCUS_CASTTIME_MIN: // base: ms
    case SPA_FOCUS_CASTTIME_MAX:
    case SPA_FOCUS_MIN_REUSE_TIME:
    case SPA_FOCUS_MAX_REUSE_TIME:
    case SPA_FOCUS_CAST_TIME_AMT:
    case SPA_FEARSTUN:
        out += f_seconds(name, static_cast<float>(value) / 1000.0f);
        break;
    case SPA_NPC_PORTAL_WARDER_BANISH:
        out += f_string(name, location(zone_text(extra)));
        break;
    case SPA_PERCENT_HEAL:
        out += f_max(name, value, max);
        break;
    case SPA_STACKING_BLOCK:
        out += f_stacking(name, base2, value, max, spa, effect_name(base));
        break;
    case SPA_STRIP_VIRTUAL_SLOT:
        out += f_stacking(name, calc - 200, value, max > 1000 ? max - 1000 : max, spa, effect_name(base));
        break;
    case SPA_DIVINE_INTERVENTION:
        out += f_extra(name,
                       std::format("Restore {} Health", base == 1 ? "Partial" : base == 2 ? "Full" : "Unknown"));
        break;
    case SPA_POCKET_PET:
        out += f_extra(name, std::format("({})", base == 0   ? "Current HP Only"
                                                 : base == 1 ? "Current HP, Buffs, Weapons"
                                                             : "Unknown"));
        break;
    case SPA_PET_SWARM:
        out += f_extra(name, std::format("{} x{} for {}sec", extra, value, finish));
        break;
    case SPA_HEALTH_BALANCE:
    case SPA_MANA_BALANCE:
        out += f_penalty(name, value, "Penalty");
        break;
    case SPA_CANCEL_NEGATIVE_MAGIC:
        out += f_penalty(name, base / 10, "Chance");
        break;
    case SPA_CHANGE_SOBRIETY:
        out += f_at(name, value, "if Alcholol Tolerance is below");
        break;
    case SPA_SPELL_GUARD:
    case SPA_MELEE_GUARD:
        out += f_percent(name, value, value, false);
        if (max > 0) out += std::format(" until {} absorbed", max);
        break;
    case SPA_ABSORB_HIT:
        out += f_string(name,
                        std::format(" up to {} from the next {} melee strikes or direct damage spells", max, value));
        break;
    case SPA_CRITICAL_MELEE:
        out += f_skills(name, value, finish, names.skill(base2), true, "for");
        break;
    case SPA_MELEE_LIFETAP:
        out += f_percent(name, value, finish);
        out += " heal";
        break;
    case SPA_SKILL_ATTACK:
        out += f_skill_attack(name, value, base2, names.skill(skill));
        out += maxtargets;
        break;
    case SPA_SKILL_DAMAGE_TAKEN:
        out += f_skills(name, value, finish, names.skill(base2));
        out += maxtargets;
        break;
    case SPA_GROUP_FEAR_IMMUNITY:
    case SPA_FOCUS_FRENZIED_DEVASTATION:
        out += f_seconds(name, value, true);
        break;
    case SPA_SHIELD_CASTER:
        out += f_seconds(name, static_cast<float>(value) * 1.0f);
        break;
    case SPA_PET_PCT_MAX_HP:
    case SPA_HP_MAX_HP:
        out += f_percent(name, value, finish, false, true);
        break;
    case SPA_PET_PCT_AVOIDANCE:
    case SPA_MELEE_ACCURACY:
    case SPA_HEADSHOT:
    case SPA_PET_CRIT_MELEE:
    case SPA_SPELL_INTERRUPT:
        out += f_percent(name, value, finish);
        out += maxtargets;
        break;
    case SPA_INCREASE_SKILL_DAMAGE:
        out += f_skills(name, value, finish, names.skill(base2), false);
        break;
    case SPA_REDUCE_SKILL_TIMER:
        out += f_refresh_timer(name, -value, -finish, names.skill(base2));
        break;
    case SPA_INCREASE_AIR_SUPPLY:
        out += f_range(name, value, range);
        break;
    case SPA_SHIELDING_DURATION_MOD:
    case SPA_AC_LIMIT_MOD:
    case SPA_ADD_INSTRUMENT_MOD:
    case SPA_SONG_MOD_CAP:
    case SPA_BASE_RUN_MOD:
    case SPA_SECONDARY_RIPOSTE_MOD:
    case SPA_REDUCE_TARGET_HATE:
    case SPA_HP_FOR_MANA:
        out += f_base_pct(name, base);
        break;
    case SPA_INCREASE_STAT_CAP:
        out += f_stats_cap_range(name, value, stat_short_name(base2), extendedrange);
        break;
    case SPA_ALCHEMY_FAIL_RATE:
        out += f_skills(name, -value, -finish, names.skill(base2), true, "for");
        break;
    case SPA_EXTEND_SONG_RANGE:
    case SPA_LIMIT_HP:
    case SPA_LIMIT_MANA:
    case SPA_LIMIT_ENDURANCE:
        out += f_count(name, value, "to");
        break;
    case SPA_FOCUS_DURATION_AMT:
        out += f_count(name, value);
        out += " tick(s)";
        break;
    case SPA_DOOM_EFFECT:
    case SPA_DOOM_RUNE_EFFECT:
    case SPA_DOOM_ALWAYS:
    case SPA_DOOM_ENTITY:
    case SPA_DOOM_DISPELLEE:
        out += f_extra(name, names.spell(base), " on Fade");
        break;
    case SPA_FOCUS_REUSE_TIMER:
        out += f_timer(name, static_cast<float>(-base) / 1000.0f);
        break;
    case SPA_MANA_ABSORB_PCT_DMG:
        out += f_count(name, value, "up to", "%");
        break;
    case SPA_NO_MOVE_HP: // a song's DoT
        out += f_range(name, value, extendedrange);
        out += repeating;
        out += " if target is not moving";
        break;
    case SPA_FOCUS_CASTING_PROC:
    case SPA_CHANCE_SPELL:
    case SPA_FOCUS_CASTING_PROC_NORMALIZED:
        out += f_extra(name, f_spell_chance(names, base, base2), " on Cast");
        break;
    case SPA_MANABURN:
        out += f_count(name, value * 4, "for");
        break;
    case SPA_SPAWN_INTERACTIVE_OBJECT:
        out += f_extra(name, names.spell(id + (spellgroup ? 3 : 1)));
        break;
    case SPA_PROC_ON_KILL_SHOT:
    case SPA_PROC_ON_DEATH:
    case SPA_PROC_ON_SPELL_KILL_SHOT:
    case SPA_TRIGGER_SPELL:
        out += f_extra(name, f_spell_chance(names, base, base2));
        break;
    case SPA_MODIFY_BODY_TYPE:
        out += f_string(name, std::format(" to {}", base == 25   ? "Plant"
                                                    : base == 21 ? "Animal"
                                                    : base == 3  ? "Undead"
                                                                 : "Unknown"));
        break;
    case SPA_MODIFY_FACTION:
        out += f_extra(name, names.faction(base));
        break;
    case SPA_RESIST_OTHER_SPA:
        out += f_extra(name, f_resists(name, base, base2));
        break;
    case SPA_EXPLOSIVE_KNOCKBACK:
        out += f_string(name, std::format(" ({}) and Toss Up ({})", base, base2));
        break;
    case SPA_SUPPRESSION:
        out += f_extra(name, std::format(" {} Effect", effect_name(base2)));
        break;
    case SPA_FOCUS_WHICH_GROUP:
        out += f_extra(name, names.spell_group(base));
        break;
    case SPA_DOOM_DISPELLER:
        out += f_extra(name, names.spell(base), " on Curer");
        break;
    case SPA_REFRESH_SPELL_TIMER:
        out += f_count(name, -value, "to");
        break;
    case SPA_FOCUS_SWARM_PET_DURATION:
        out += f_seconds_count(name, static_cast<float>(value) / 1000.0f);
        break;
    case SPA_HEALBURN:
        out += f_string(name, std::format(" use up to {} mana to heal your group", value));
        break;
    case SPA_MANA_IGNITE:
    case SPA_ENDURANCE_IGNITE:
        out += f_count(name, -value, "by up to");
        break;
    case SPA_DOOM_LIMIT_USE:
        out += f_extra(name, names.spell(base), " on Max Hits");
        break;
    case SPA_DOOM_FOCUS_USED:
        out += f_extra(name, names.spell(base), " on Focus Used");
        break;
    case SPA_FOCUS_LIMIT_CLASS:
    case SPA_FOCUS_LIMIT_CASTER_CLASS:
        out += f_extra(name, classes_from_mask(static_cast<std::int32_t>(base)));
        break;
    case SPA_FOCUS_BASE_EFFECTS:
        out += f_count(name, value, "by", "%");
        break;
    case SPA_GRAVITATE:
        out += f_base(name, base);
        out += maxtargets;
        break;
    case SPA_PROC_SKILL_MODIFIER:
        out += f_extra(name, names.skill(base));
        break;
    case SPA_TELEPORT_TO_ANCHOR:
    case SPA_TRANSLOCATE_TO_ANCHOR:
        out += f_string(name, std::format(" to {} Anchor", base == 50874   ? "Guild Hall"
                                                           : base == 52584 ? "Primary"
                                                           : base == 52585 ? "Secondary"
                                                                           : "Unknown"));
        break;
    case SPA_REQUIRE_TARGET_DOOM:
    case SPA_REQUIRE_CASTER_DOOM:
        out += f_extra(name, names.spell(base));
        with_restrictions();
        break;
    case SPA_IMPROVED_TAUNT:
        out += f_string(name, std::format(" up to L{} and Reduce Ally Hate Generation by {}%", base, base2));
        break;
    case SPA_DOT_GUARD:
        out += f_string(name, std::format(" absorbing {}% damage to a total of {}", value, max));
        break;
    case SPA_MELEE_THRESHOLD_GUARD:
    case SPA_SPELL_THRESHOLD_GUARD:
        out += f_string(name, std::format(" absorbing {}% of incoming {} damage in excess of {} to a total of {}", value,
                                          spa == SPA_MELEE_THRESHOLD_GUARD ? "melee" : "spell", base2, max));
        break;
    case SPA_MELEE_THRESHOLD_DOOM:
        out += f_extra(name, names.spell(base), std::format(" on {} Melee Damage Taken", base2));
        break;
    case SPA_SPELL_THRESHOLD_DOOM:
        out += f_extra(name, names.spell(base), std::format(" on {} Spell Damage Taken", base2));
        break;
    case SPA_RESOURCE_TAP:
        out += std::format("Return {:.2f}% of direct damage as {}", static_cast<float>(value) / 10.0f,
                           base2 == 0   ? "hit points"
                           : base2 == 1 ? "mana"
                           : base2 == 2 ? "endurance"
                                        : "unknown");
        break;
    case SPA_CHANCE_BEST_IN_SPELL_GROUP:
    case SPA_TRIGGER_BEST_IN_SPELL_GROUP:
        out += f_extra(name, f_spell_group_chance(names, base, base2), " on Cast");
        break;
    case SPA_TRIGGER_SPELL_NON_ITEM:
        out += f_extra(name, names.spell(base2), " on Cast");
        break;
    case SPA_HATELIST_TO_TAIL:
        out += f_extra(name, names.spell(base), " on Cast");
        break;
    case SPA_FOCUS_LIMIT_MIN_VALUE:
        out += f_minmax_base(name + (base < 0 ? " Max" : " Min"), base, base2);
        break;
    case SPA_FOCUS_LIMIT_MAX_VALUE:
        out += f_minmax_base(name + (base < 0 ? " Min" : " Max"), base, base2);
        break;
    case SPA_FOCUS_CAST_SPELL_ON_LAND:
        out += f_extra(name, names.spell(base2), " on Land and conditions are met");
        break;
    case SPA_FOCUS_INCOMING_DMG_MOD:
    case SPA_FOCUS_INCOMING_DMG_AMT:
        out += f_range(name, value, extendedrange, " (after crit)");
        break;
    case SPA_FOCUS_LIMIT_SAME_CASTER:
        out += f_extra(name, base ? "(Same)" : "(Different)", "", "");
        break;
    case SPA_EXTEND_TRADESKILL_CAP:
    case SPA_FOCUS_CAST_PROC_NO_BYPASS:
    case SPA_HEALTH_TRANSFER:
        out += std::format("{} ({}, {}, {})", name, base, base2, max);
        break;
    case SPA_CRIT_MELEE_DMG_MOD_MAX:
        out += f_percent(name, value, finish);
        out += " of Base Damage (Non Stacking)";
        break;
    case SPA_MELEE_DMG_POSITION_MOD:
    case SPA_DMG_TAKEN_POSITION_MOD:
        out += f_seconds(name, static_cast<float>(value) / 10.0f);
        break;
    case SPA_AMPLIFY_MOD:
        out += f_percent(name, value, finish);
        out += " (Before DoT Crit, After Nuke Crit)";
        break;
    default: // an effect MacroQuest has no text for
        out += std::format("{} (base={}, base2={}, max={}, calc={}, value={})", name, base, base2, max, calc, value);
        break;
    }
    return out;
}

std::vector<std::string> spell_effect_lines(const SpellData& spell, const SpellEffectTextContext& ctx) {
    std::vector<std::string> lines;
    for (int k = 0; k < static_cast<int>(spell.effect.size()); ++k)
        if (std::string line = spell_effect_text(spell, k, ctx); !line.empty()) lines.push_back(std::move(line));
    return lines;
}

} // namespace hailstone::game
