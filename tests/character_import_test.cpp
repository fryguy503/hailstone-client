#include <doctest/doctest.h>

#include "core/character_import_source.h"
#include "core/ini_file.h"
#include "game/character_import.h"

using namespace hailstone;

namespace {
constexpr game::CharacterImportOptions kImportBoth{.socials = true, .hot_buttons = true};
using Disposition = game::CharacterImportDisposition;

constexpr std::string_view kSource =
    "[Socials]\r\n"
    "Page1Button1Name=Assist\r\n"
    "Page1Button1Color=12\r\n"
    "Page1Button1Line1=/assist\r\n"
    "Page1Button1Line3=/pause 5, /cast 2\r\n"
    "[HotButtons]\r\n"
    "Page1Button2=E0,@-1,0000000000000000,0,Assist\r\n"
    "[HotButtons10]\r\n"
    "Page10Button12=G9,@-1,0000000000000000,0,Inventory\r\n";
} // namespace

TEST_CASE("Character import stages whole socials and resolves linked buttons before applying") {
    game::HotButtonStore destination, staged;
    const auto plan = game::plan_character_import(kSource, destination, kImportBoth, staged);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.rows.size() == 3);
    CHECK(plan.additions == 3);
    CHECK(plan.rows[0].disposition == Disposition::Add);
    REQUIRE(plan.rows[0].lines.size() >= 3);
    REQUIRE(plan.rows[1].lines.size() >= 3);
    CHECK(plan.rows[0].lines[2] == "/pause 5, /cast 2");
    CHECK(plan.rows[1].lines[2] == "/pause 5, /cast 2");
    CHECK(plan.rows[2].reason == "Adds the Inventory action.");
    CHECK(staged.social(0).color == 12);
    CHECK(staged.social(0).lines[1].empty());
    CHECK(staged.social(0).lines[2] == "/pause 5, /cast 2");
    CHECK(staged.at(0, 0, 1).type == game::HotButtonType::Social);
    CHECK(staged.at(0, 0, 1).slot == 0);
    CHECK(staged.at(9, 9, 11).type == game::HotButtonType::MenuButton);
    CHECK(destination.social(0).empty());
    CHECK(destination.at(0, 0, 1).empty());

    game::HotButtonStore repeated;
    const auto again = game::plan_character_import(kSource, staged, kImportBoth, repeated);
    REQUIRE(again.rows.size() == 3);
    CHECK(again.additions == 0);
    for (const auto& row : again.rows) CHECK(row.disposition == Disposition::Keep);
}

TEST_CASE("Character import checks matching socials and protects occupied or referenced slots") {
    game::HotButtonStore destination, staged;
    constexpr game::CharacterImportOptions buttons_only{.hot_buttons = true};
    auto plan = game::plan_character_import(kSource, destination, buttons_only, staged);
    REQUIRE(plan.rows.size() == 2);
    CHECK(plan.rows[0].disposition == Disposition::Skip);
    CHECK(plan.additions == 1);

    destination.social(0).name = "Assist";
    destination.social(0).lines[0] = "/assist";
    destination.social(0).lines[2] = "/pause 5, /cast 2";
    destination.social(0).color = 2;
    plan = game::plan_character_import(kSource, destination, kImportBoth, staged);
    CHECK(plan.rows[0].disposition == Disposition::Skip);
    CHECK(plan.rows[1].disposition == Disposition::Skip);
    CHECK(staged.social(0).color == 2);

    destination.social(0).color = 12;
    plan = game::plan_character_import(kSource, destination, buttons_only, staged);
    CHECK(plan.rows[0].disposition == Disposition::Add);

    auto protected_options = kImportBoth;
    protected_options.protected_socials.set(0);
    plan = game::plan_character_import(kSource, destination, protected_options, staged);
    CHECK(plan.rows[0].disposition == Disposition::Skip);
    CHECK(plan.rows[1].disposition == Disposition::Skip);

    destination.social(0) = {};
    destination.social(0).lua.face = "return {}";
    plan = game::plan_character_import(kSource, destination, kImportBoth, staged);
    CHECK(plan.rows[0].disposition == Disposition::Skip);
    CHECK(staged.social(0).is_lua());

    destination.social(0) = {};
    auto& existing_button = destination.at(0, 0, 0);
    existing_button.type = game::HotButtonType::Social;
    existing_button.slot = 0;
    plan = game::plan_character_import(kSource, destination, kImportBoth, staged);
    CHECK(plan.rows[0].disposition == Disposition::Skip);
    CHECK(plan.rows[1].disposition == Disposition::Skip);
    CHECK(staged.social(0).empty());
}

TEST_CASE("Character import skips unsafe scripts, unsupported references, and malformed records") {
    constexpr std::string_view source =
        "[SOcials]\n"
        "Page1Button1Name=Command chain\n"
        "Page1Button1Line1=/assist\n"
        "Page1Button1Line2=/pause 5, /lua run sample\n"
        "Page1Button2Name=Expansion\n"
        "Page1Button2Line1=/say %t\n"
        "Page1Button3Name=Invalid color\n"
        "Page1Button3Color=12abc\n"
        "Page1Button3Line1=/assist\n"
        "Page1Button4Name=*\n"
        "[hotbuttons2]\n"
        "Page1Button1=E120,@-1,0000000000000000,0,Ability\n"
        "Page1Button2=H0,@-1,0000000000000000,0,Spell slot\n"
        "Page1Button3=G9abc,@-1,0000000000000000,0,Malformed\n"
        "Page1Button4=G3,@-1,0000000000000000,0,Camp\n";
    game::HotButtonStore destination, staged;
    const auto plan = game::plan_character_import(source, destination, kImportBoth, staged);
    REQUIRE(plan.error.empty());
    CHECK(plan.additions == 0);
    REQUIRE(plan.rows.size() == 7);
    REQUIRE(plan.rows[0].lines.size() >= 2);
    CHECK(plan.rows[0].lines[1] == "/pause 5, /lua run sample");
    for (const auto& row : plan.rows) CHECK(row.disposition == Disposition::Skip);
    CHECK(staged.social(0).empty());
    CHECK(staged.at(1, 0, 0).empty());

    // An unlabelled button for a skipped social is named after that social in the preview.
    constexpr std::string_view unlabelled =
        "[Socials]\nPage1Button1Name=Pet attack\nPage1Button1Line1=/pet attack\n"
        "[HotButtons]\nPage1Button1=E0,@-1,0000000000000000,0,\n";
    const auto named = game::plan_character_import(unlabelled, destination, kImportBoth, staged);
    REQUIRE(named.rows.size() == 2);
    CHECK(named.rows[1].disposition == Disposition::Skip);
    CHECK(named.rows[1].name == "Pet attack");
}

TEST_CASE("Character import fails closed on ambiguous or oversized input") {
    game::HotButtonStore destination, staged;
    destination.social(0).name = "Existing";
    std::string source(kSource);
    source += "page10button12=G9,@-1,0000000000000000,0,Duplicate\n";
    auto plan = game::plan_character_import(source, destination, kImportBoth, staged);
    CHECK_FALSE(plan.error.empty());
    CHECK(plan.rows.empty());
    CHECK(staged.social(0).name == "Existing");
    CHECK(staged.at(9, 9, 11).empty());

    source = std::string(kSource) + "[SOCIALS]\n";
    plan = game::plan_character_import(source, destination, kImportBoth, staged);
    CHECK_FALSE(plan.error.empty());

    source.assign(kCharacterImportMaxBytes + 1, ' ');
    plan = game::plan_character_import(source, destination, kImportBoth, staged);
    CHECK_FALSE(plan.error.empty());
}

TEST_CASE("Character import writes only reviewed additions and clears orphaned social fields") {
    IniFile ini;
    ini.parse("[Socials]\nPage1Button1Line2=/old\nPage1Button1Line4=/old\nPage1Button2Name=Current\n"
              "[HotButtons]\nPage1Button2=G9,@-1,0000000000000000,0,Current button\n"
              "[Unrelated]\nKey=Keep value\n");
    game::HotButtonStore destination, staged;
    destination.load(ini);
    const auto plan = game::plan_character_import(kSource, destination, kImportBoth, staged);
    REQUIRE(plan.error.empty());
    CHECK(plan.additions == 2);
    game::write_character_import_additions(ini, staged, plan);
    CHECK(ini.get_string("Socials", "Page1Button1Name") == "Assist");
    CHECK_FALSE(ini.get("Socials", "Page1Button1Line2"));
    CHECK_FALSE(ini.get("Socials", "Page1Button1Line4"));
    CHECK(ini.get_string("Socials", "Page1Button2Name") == "Current");
    CHECK(ini.get_string("HotButtons", "Page1Button2") == "G9,@-1,0000000000000000,0,Current button");
    CHECK(ini.get_string("HotButtons10", "Page10Button12") == "G9,@-1,0000000000000000,0,Inventory");
    CHECK(ini.get_string("Unrelated", "Key") == "Keep value");
}
