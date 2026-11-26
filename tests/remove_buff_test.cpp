// /removebuff's line (net/remove_buff.h): who it names, the name as MacroQuest takes it and how a name matches.
#include <doctest/doctest.h>

#include "net/remove_buff.h"

using hailstone::net::parse_remove_buff;
using hailstone::net::remove_buff_matches;

TEST_CASE("removebuff: the player's, the pet's or both; the rest of the line or a quoted name") {
    auto self = parse_remove_buff("Spirit of Wolf");
    CHECK(self.self);
    CHECK_FALSE(self.pet);
    CHECK(self.name == "Spirit of Wolf");
    CHECK(parse_remove_buff("  Spirit of Wolf  ").name == "Spirit of Wolf");
    CHECK(parse_remove_buff("\"Spirit of Wolf\" and more").name == "Spirit of Wolf");
    auto pet = parse_remove_buff("-pet Spirit of Wolf");
    CHECK_FALSE(pet.self);
    CHECK(pet.pet);
    CHECK(pet.name == "Spirit of Wolf");
    CHECK(parse_remove_buff("-PET \"Burnout IV\"").name == "Burnout IV");
    auto both = parse_remove_buff("-both =Haste");
    CHECK(both.self);
    CHECK(both.pet);
    CHECK(both.name == "=Haste");
    // Nothing named: nothing to remove.
    CHECK(parse_remove_buff("").name.empty());
    CHECK(parse_remove_buff("-pet").name.empty());
    CHECK(parse_remove_buff("-pet   ").name.empty());
    CHECK(parse_remove_buff("\"\"").name.empty());
}

TEST_CASE("removepetbuff: the pet's only; the whole line or a quoted name") {
    auto p = hailstone::net::parse_remove_pet_buff("Spirit of Wolf");
    CHECK_FALSE(p.self);
    CHECK(p.pet);
    CHECK(p.name == "Spirit of Wolf");
    CHECK(hailstone::net::parse_remove_pet_buff("\"Burnout IV\" more").name == "Burnout IV");
    // As MQ's RemovePetBuff, the whole line is the name: "-pet" is no option here.
    CHECK(hailstone::net::parse_remove_pet_buff("-pet Haste").name == "-pet Haste");
    CHECK(hailstone::net::parse_remove_pet_buff("  ").name.empty());
}

TEST_CASE("removebuff: a name matches anywhere in any case; =name the whole name") {
    CHECK(remove_buff_matches("Spirit of Wolf", "wolf"));
    CHECK(remove_buff_matches("Spirit of Wolf", "SPIRIT OF WOLF"));
    CHECK_FALSE(remove_buff_matches("Spirit of Wolf", "Spirit of the Wolf"));
    CHECK(remove_buff_matches("Spirit of Wolf", "=spirit of wolf"));
    CHECK_FALSE(remove_buff_matches("Spirit of Wolf", "=Spirit"));
    CHECK_FALSE(remove_buff_matches("Spirit of Wolf", ""));
    CHECK_FALSE(remove_buff_matches("Spirit of Wolf", "="));
}
