#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "game/eq_strings.h"
#include "game_files.h"

TEST_CASE("eqstr templates fill %n, %Tn and %Bn(type)" * doctest::skip(!hailstone::test::game_dir())) {
    hailstone::game::EqStrings s;
    s.load(*hailstone::test::game_dir());
    REQUIRE(s.loaded());
    std::vector<std::string> args{"1597", "0", "1215"};
    // %T3: argument 3 is an eqstr id (1215 "points"), expanded (FormatEqStrTemplate @0x4e1540).
    CHECK(s.format(410, args) == "You have gained the ability \"Call of Challenge\" at a cost of 0 ability points.");
    // NPC SayString: 554 "%1 says '%T2'" with the inner string's own %3/%4 from the same arguments.
    std::vector<std::string> say{"Rabley Trumend", "1147", "Nmscshoptest", "Iron Ration"};
    // The wording is the folder's (a live install has "%1 says, '%T2'" and other punctuation); every folder fills
    // the same arguments into both strings.
    const std::string said = s.format(554, say);
    CHECK(said.starts_with("Rabley Trumend says"));
    CHECK(said.find("Nmscshoptest") != std::string::npos);
    CHECK(said.find("Iron Ration") != std::string::npos);
    CHECK(said.find('%') == std::string::npos);
    if (hailstone::test::pinned_values("eqstr 554 and 1147 wording"))
        CHECK(said ==
              "Rabley Trumend says 'Hi there Nmscshoptest, just browsing?  Have you seen the Iron Ration I just got in?'");
    CHECK(s.format(999999999, args) == "[999999999] 1597 0 1215");
}
