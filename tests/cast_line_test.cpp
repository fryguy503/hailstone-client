// /cast's spell name (net/cast_line.h): RoF2's gem form, MacroQuest's name and =name forms.

#include <doctest/doctest.h>

#include "net/cast_line.h"

using namespace hailstone::net;

TEST_CASE("cast line: a line starting with a number is RoF2's gem form") {
    CHECK(cast_line_is_gem(""));
    CHECK(cast_line_is_gem("3"));
    CHECK(cast_line_is_gem("  12"));
    CHECK(cast_line_is_gem("0"));  // the original's invalid gem, not a name
    CHECK(cast_line_is_gem("-1"));
    CHECK_FALSE(cast_line_is_gem("\"Minor Healing\""));
    CHECK_FALSE(cast_line_is_gem("Minor"));
    CHECK_FALSE(cast_line_is_gem("=Minor Healing"));
    CHECK_FALSE(cast_line_is_gem("-"));
}

TEST_CASE("cast line: a name starts the spell's name in any case; =name is the whole name in any case") {
    CHECK(cast_spell_name_matches("Minor Healing", "Minor"));
    CHECK(cast_spell_name_matches("Minor Healing", "minor heal"));
    CHECK_FALSE(cast_spell_name_matches("Minor Healing", "Healing")); // not its start
    CHECK(cast_spell_name_matches("Minor Healing", "=minor healing"));
    CHECK_FALSE(cast_spell_name_matches("Minor Healing Ward", "=Minor Healing"));
    CHECK_FALSE(cast_spell_name_matches("Minor Healing", ""));
    CHECK_FALSE(cast_spell_name_matches("Minor Healing", "="));
}
