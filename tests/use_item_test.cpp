// /useitem's line (net/use_item.h): RoF2's slot form, MacroQuest's name form, and an item id.

#include <doctest/doctest.h>

#include <cstdint>

#include "net/use_item.h"

using namespace hailstone::net;
using Kind = UseItemLine::Kind;

TEST_CASE("useitem: a number first is the slot form, alone or with a bag index") {
    UseItemLine l = parse_use_item("24 2");
    CHECK(l.kind == Kind::Slot);
    CHECK(l.number == 24);
    CHECK_FALSE(l.alone);
    l = parse_use_item("  13  ");
    CHECK(l.kind == Kind::Slot);
    CHECK(l.number == 13);
    CHECK(l.alone);
    l = parse_use_item("1002"); // an item id when a carried item has it (the connection decides)
    CHECK(l.kind == Kind::Slot);
    CHECK(l.number == 1002);
    CHECK(l.alone);
    l = parse_use_item("\"40\""); // MQ's GetArg drops the quotes: still a number
    CHECK(l.kind == Kind::Slot);
    CHECK(l.number == 40);
    l = parse_use_item("'12' 3"); // RoF2's GetNextCmdArg takes single quotes as well
    CHECK(l.kind == Kind::Slot);
    CHECK(l.number == 12);
    CHECK_FALSE(l.alone);
    l = parse_use_item("'12'");
    CHECK(l.kind == Kind::Slot);
    CHECK(l.alone);
    l = parse_use_item("99999999999999999999999"); // past 64 bits: no slot and no item's id
    CHECK(l.kind == Kind::Slot);
    CHECK(l.number == UINT64_MAX);
}

TEST_CASE("useitem: anything else is a name; quoted is the whole name") {
    UseItemLine l = parse_use_item("Rations");
    CHECK(l.kind == Kind::Name);
    CHECK(l.name == "Rations");
    CHECK_FALSE(l.exact);
    l = parse_use_item("  Summoned: Black Bread  ");
    CHECK(l.kind == Kind::Name);
    CHECK(l.name == "Summoned: Black Bread");
    CHECK_FALSE(l.exact);
    l = parse_use_item("\"Manastone\"");
    CHECK(l.kind == Kind::Name);
    CHECK(l.name == "Manastone");
    CHECK(l.exact);
    l = parse_use_item("\"Short Sword\" ");
    CHECK(l.name == "Short Sword");
    CHECK(l.exact);
    l = parse_use_item("Wand 2"); // a word first, a number after: still a name
    CHECK(l.kind == Kind::Name);
    CHECK(l.name == "Wand 2");
    l = parse_use_item("-1"); // not a number: a name, unquoted (the connection shows the usage when nothing matches)
    CHECK(l.kind == Kind::Name);
    CHECK(l.name == "-1");
    CHECK_FALSE(l.exact);
}

TEST_CASE("useitem: nothing, or only quotes, asks for the usage") {
    CHECK(parse_use_item("").kind == Kind::Usage);
    CHECK(parse_use_item("   ").kind == Kind::Usage);
    CHECK(parse_use_item("\"\"").kind == Kind::Usage);
}

TEST_CASE("useitem: a name matches anywhere in any case; quoted, the whole name in any case") {
    CHECK(use_item_name_matches("Short Sword", "sword", false));
    CHECK(use_item_name_matches("Short Sword", "SHORT SWORD", false));
    CHECK_FALSE(use_item_name_matches("Short Sword", "long", false));
    CHECK_FALSE(use_item_name_matches("Sword", "Short Sword", false));
    CHECK(use_item_name_matches("Short Sword", "short sword", true));
    CHECK_FALSE(use_item_name_matches("Short Sword", "Sword", true));
}
