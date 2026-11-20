#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "game/spa_ids.h"
#include "game/spell_effect_text.h"
#include "game/spells.h"

using namespace hailstone;
using namespace hailstone::game;

namespace {

std::string spell_line(std::initializer_list<std::pair<int, std::string>> set) {
    std::vector<std::string> cols(237, "0");
    for (int i = 1; i <= 8; ++i) cols[static_cast<std::size_t>(i)].clear();
    for (const auto& [c, v] : set) cols[static_cast<std::size_t>(c)] = v;
    std::string line;
    for (std::size_t i = 0; i < cols.size(); ++i) line += (i ? "^" : "") + cols[i];
    return line;
}

// A spell with every slot unused (254) but the first.
SpellData one_slot(int spa, std::int32_t base, std::int32_t base2 = 0, std::int32_t max = 0, std::int32_t calc = 0) {
    SpellData s;
    s.id = 5000;
    s.name = "Test Spell";
    s.effect.fill(SPA_NOSPELL);
    s.effect[0] = spa;
    s.base[0] = base;
    s.base2[0] = base2;
    s.max[0] = max;
    s.formula[0] = calc;
    return s;
}

} // namespace

TEST_CASE("spell effect text: MacroQuest's CalcValue formulas") {
    CHECK(spell_calc_value(0, 25, 0, 1, 1, 50) == 25);       // none: the base
    CHECK(spell_calc_value(102, 10, 20, 0, 1, 5) == 15);     // + level
    CHECK(spell_calc_value(102, 10, 20, 0, 1, 50) == 20);    // capped at max
    CHECK(spell_calc_value(102, -10, -20, 0, 1, 5) == -15);  // a negative base keeps its sign
    CHECK(spell_calc_value(107, 100, 0, 3, 1, 1) == 97);     // decays 1 a tick
    CHECK(spell_calc_value(123, 10, 50, 2, 1, 1) == 50);     // random: the top of the range after a tick
    CHECK(spell_calc_value(123, 10, 50, 1, 1, 1) == 10);     // ... and the bottom on the first
    CHECK(spell_calc_value(100, 40, 60, 1, 20, 20) == 40);   // at the minimum level: the base
    CHECK(spell_calc_value(100, 40, 60, 1, 20, 21) == 60);   // above it: the max
    CHECK(spell_calc_value(3000, 7, 0, 1, 1, 1) == 7);
}

TEST_CASE("spell effect text: a slot's line as ParseSpellEffect writes it") {
    SpellTable table;
    REQUIRE(table.parse(spell_line({{0, "300"}, {1, "Test Proc"}})) == 1);
    SpellEffectTextContext ctx;
    ctx.spells = &table;

    SUBCASE("a level-scaled heal: the range from the lowest class level to where it caps") {
        SpellData s = one_slot(SPA_HP, 10, 0, 20, 102);
        s.class_level[1] = 1; // cleric 1
        CHECK(spell_effect_text(s, 0, ctx) == "Slot 1: Increase HP by 11 (L1) to 20 (L10)");
    }
    SUBCASE("an AE nuke with a target cap and a restriction") {
        SpellData s = one_slot(SPA_HP, -100, 120);
        s.target_type = 4; // PB AE
        s.max_targets = 5;
        CHECK(spell_effect_text(s, 0, ctx) ==
              "Slot 1: Decrease HP by 100 on up to 5 enemies -- Restrictions: Only works on Undead");
    }
    SUBCASE("a DoT names its ticks") {
        SpellData s = one_slot(SPA_HP, -30);
        s.duration_cap = 10;
        CHECK(spell_effect_text(s, 0, ctx) == "Slot 1: Decrease HP by 30 per tick ");
    }
    SUBCASE("haste counts from base 100, as a percent") {
        SpellData s = one_slot(SPA_HASTE, 140, 0, 140, 100);
        s.class_level[13] = 20; // enchanter 20
        CHECK(spell_effect_text(s, 0, ctx) == "Slot 1: Increase Melee Speed by 40%");
    }
    SUBCASE("a self teleport: x, y, z and heading from the first four slots' bases") {
        SpellData s = one_slot(SPA_PORTAL, 100);
        s.base[1] = 200;
        s.base[2] = -50;
        s.base[3] = 128;
        s.target_type = 6;
        s.teleport_zone = "nosuchzone";
        CHECK(spell_effect_text(s, 0, ctx) == "Slot 1: Teleport  Self to 100, 200, -50 in UNKNOWN_ZONE facing west");
    }
    SUBCASE("a proc names its spell from the table, and its rate mod") {
        CHECK(spell_effect_text(one_slot(SPA_MELEE_PROC, 300), 0, ctx) == "Slot 1: Add Proc: Test Proc");
        CHECK(spell_effect_text(one_slot(SPA_MELEE_PROC, 300, 50), 0, ctx) ==
              "Slot 1: Add Proc: Test Proc (rate mod 50)");
        CHECK(spell_effect_text(one_slot(SPA_MELEE_PROC, 301), 0, ctx) == "Slot 1: Add Proc: Unknown Spell");
        CHECK(spell_effect_text(one_slot(SPA_FOCUS_WHICH_SPELL, 300), 0, ctx) ==
              "Slot 1: Limit: Spell (Test Proc allowed)");
        // An excluded spell's negative id names no spell, as in MacroQuest.
        CHECK(spell_effect_text(one_slot(SPA_FOCUS_WHICH_SPELL, -300), 0, ctx) ==
              "Slot 1: Limit: Spell (Unknown Spell excluded)");
    }
    SUBCASE("a class limit lists the classes, or the ones left out when few are") {
        CHECK(spell_effect_text(one_slot(SPA_FOCUS_LIMIT_CLASS, 0x6), 0, ctx) == "Slot 1: Limit: PlayerClass: WAR,CLR");
        CHECK(spell_effect_text(one_slot(SPA_FOCUS_LIMIT_CLASS, 0x1fffe), 0, ctx) == "Slot 1: Limit: PlayerClass: ALL");
        CHECK(spell_effect_text(one_slot(SPA_FOCUS_LIMIT_CLASS, 0x1fffc), 0, ctx) ==
              "Slot 1: Limit: PlayerClass: ALL EXCEPT: WAR");
    }
    SUBCASE("stacking names the effect it blocks") {
        CHECK(spell_effect_text(one_slot(SPA_STACKING_BLOCK, 0, 3), 0, ctx) ==
              "Slot 1: Stacking: Block new spell if slot 3 is effect 'HP'");
    }
    SUBCASE("an effect with no text of its own shows its numbers") {
        CHECK(spell_effect_text(one_slot(9999, 5), 0, ctx) ==
              "Slot 1: Unknown SPA[9999] (base=5, base2=0, max=0, calc=0, value=5)");
    }
    SUBCASE("names the context lacks") {
        CHECK(spell_effect_text(one_slot(SPA_MODIFY_FACTION, 123), 0, ctx) ==
              "Slot 1: Modify Faction: (Unknown Faction[123])");
        ctx.faction_name = [](std::int32_t f) { return f == 123 ? std::string("Guards of Qeynos") : std::string(); };
        CHECK(spell_effect_text(one_slot(SPA_MODIFY_FACTION, 123), 0, ctx) ==
              "Slot 1: Modify Faction: (Guards of Qeynos)");
        CHECK(spell_effect_text(one_slot(SPA_PROC_SKILL_MODIFIER, 8), 0, ctx) == "Slot 1: Limit Skill: Unknown[8]");
        ctx.skill_name = [](std::int32_t k) { return k == 8 ? std::string("Backstab") : std::string(); };
        CHECK(spell_effect_text(one_slot(SPA_PROC_SKILL_MODIFIER, 8), 0, ctx) == "Slot 1: Limit Skill: Backstab");
        CHECK(spell_effect_text(one_slot(SPA_PROC_SKILL_MODIFIER, -1), 0, ctx) ==
              "Slot 1: Limit Skill: All Skills");
    }
    SUBCASE("slots with nothing to show") {
        CHECK(spell_effect_text(one_slot(SPA_NOSPELL, 0), 0, ctx).empty());
        CHECK(spell_effect_text(one_slot(SPA_PORTAL_LOCATIONS, 5), 0, ctx).empty());
        CHECK(spell_effect_text(one_slot(SPA_CHA, 0), 0, ctx).empty()); // CHA 0: a placeholder
        CHECK(spell_effect_text(one_slot(SPA_HP, 1), -1, ctx).empty());
        CHECK(spell_effect_text(one_slot(SPA_HP, 1), 12, ctx).empty());
    }
}

TEST_CASE("spell effect text: spell_effect_lines skips the empty slots, in slot order") {
    SpellData s = one_slot(SPA_HP, 50);
    s.effect[3] = SPA_MANA;
    s.base[3] = 20;
    const SpellEffectTextContext ctx;
    const std::vector<std::string> lines = spell_effect_lines(s, ctx);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "Slot 1: Increase HP by 50");
    CHECK(lines[1] == "Slot 4: Increase Mana by 20");
}

TEST_CASE("spell effect text: MacroQuest's target type names") {
    CHECK(spell_target_type_desc(0) == "None");
    CHECK(spell_target_type_desc(5) == "Single");
    CHECK(spell_target_type_desc(6) == "Self");
    CHECK(spell_target_type_desc(13) == "LifeTap");
    CHECK(spell_target_type_desc(41) == "Group v2");
    CHECK(spell_target_type_desc(52) == "Single Friendly (or Target's Target");
    CHECK(spell_target_type_desc(7) == "Unknown");   // a gap in the list
    CHECK(spell_target_type_desc(255) == "Unknown");
}

TEST_CASE("spell effect text: MacroQuest's resist type names") {
    CHECK(spell_resist_type_desc(0) == "Unresistable");
    CHECK(spell_resist_type_desc(3) == "Cold");
    CHECK(spell_resist_type_desc(9) == "Corruption");
    CHECK(spell_resist_type_desc(10) == "Unknown");
}

TEST_CASE("spell effect text: spells_us.txt column 218 is the AE target cap") {
    auto s = SpellTable::parse_line(spell_line({{0, "77"}, {1, "Test AE"}, {218, "8"}}));
    REQUIRE(s);
    CHECK(s->max_targets == 8);
}
