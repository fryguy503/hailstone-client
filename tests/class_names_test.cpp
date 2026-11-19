// The server's names for the player classes (game/eq_names.h): an extension past RoF2 that a plugin or script sets
// (set_class_name, hailstone.set_class_name), for display only.
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "game/eq_names.h"
#include "game/eq_strings.h"
#include "game/social.h"
#include "mq/parse.h"
#include "mq/spawn_search.h"
#include "mq/tlo.h"
#include "mq_test_world.h"

using namespace hailstone;

namespace {

// Every test leaves the process-wide table as it found it: empty.
struct Clean {
    Clean() { game::clear_class_name_overrides(); }
    ~Clean() { game::clear_class_name_overrides(); }
};

} // namespace

TEST_CASE("class names: a server's name and short name, and back to the stock ones") {
    Clean clean;
    const std::uint64_t owner = 101;
    CHECK(game::class_name(1) == "Warrior");
    CHECK_FALSE(game::class_name_override(1));
    const auto before = game::class_names_revision();

    REQUIRE(game::set_class_name_override(owner, 1, "Vanguard", "VAN"));
    CHECK(game::class_names_revision() != before);
    CHECK(game::class_name(1) == "Vanguard");
    CHECK(game::class_short_name(1) == "VAN");
    CHECK(game::class_name_override(1) == "Vanguard");
    CHECK(game::class_name_override(1, true) == "VAN");
    // The stock names stay for what names a class to the game.
    CHECK(game::stock_class_name(1) == "Warrior");
    CHECK(game::stock_class_short_name(1) == "WAR");
    // Other classes are untouched.
    CHECK(game::class_name(2) == "Cleric");

    // An empty short name: the stock one, the name stays the server's.
    REQUIRE(game::set_class_name_override(owner, 1, "Vanguard", ""));
    CHECK(game::class_name(1) == "Vanguard");
    CHECK(game::class_short_name(1) == "WAR");
    CHECK_FALSE(game::class_name_override(1, true));
    // Both empty: the stock names again.
    REQUIRE(game::set_class_name_override(owner, 1, "", ""));
    CHECK(game::class_name(1) == "Warrior");
    CHECK(game::class_short_name(1) == "WAR");
    CHECK_FALSE(game::class_name_override(1));
}

TEST_CASE("class names: lists, masks and guildmasters show the server's names") {
    Clean clean;
    const std::uint64_t owner = 102;
    REQUIRE(game::set_class_name_override(owner, 1, "Vanguard", "VAN"));
    REQUIRE(game::set_class_name_override(owner, 6, "Mystic", "MYS"));
    REQUIRE(game::set_class_name_override(owner, 12, "Arcanist", "ARC"));
    REQUIRE(game::set_class_name_override(owner, 8, "Skirmisher", "SKR"));

    const std::vector<std::uint32_t> ids{1, 2, 12};
    CHECK(game::class_list_name(ids) == "Vanguard / Cleric / Arcanist");
    CHECK(game::class_list_name(ids, true) == "VAN/CLR/ARC");
    // bit c-1 = class c: Warrior, Druid, Bard.
    CHECK(game::class_mask_name(0x1u | 0x20u | 0x80u) == "Van/Mys/Skr");
    REQUIRE(game::set_class_name_override(owner, 6, "Mystic", "MYS"));
    CHECK(game::class_mask_name(0x1u | 0x20u) == "Van/Mys");

    // Guildmasters (20-35) follow their class; their short name is the long one, as stock.
    CHECK(game::class_name(20) == "Vanguard Guildmaster");
    CHECK(game::class_name(25) == "Mystic Guildmaster");
    CHECK(game::class_name(21) == "Cleric Guildmaster");
    CHECK(game::class_short_name(20) == "Vanguard Guildmaster");
    CHECK(game::stock_class_name(20) == "Warrior Guildmaster");
    // NPC-only classes are not renamed.
    CHECK(game::class_name(40) == "Banker");
    CHECK(game::class_name(99) == "Class 99");

    game::clear_class_name_overrides(owner);
    CHECK(game::class_list_name(ids) == "Warrior / Cleric / Wizard");
    CHECK(game::class_mask_name(0x1u | 0x20u | 0x80u) == "War/Dru/Brd");
    CHECK(game::class_name(20) == "Warrior Guildmaster");
}

TEST_CASE("class names: the latest call wins; clearing its owner shows the one before") {
    Clean clean;
    REQUIRE(game::set_class_name_override(1, 1, "Vanguard", "VAN"));
    REQUIRE(game::set_class_name_override(2, 1, "Bulwark", "BLW"));
    CHECK(game::class_name(1) == "Bulwark");
    // The first owner names it again: now the latest.
    REQUIRE(game::set_class_name_override(1, 1, "Vanguard", "VAN"));
    CHECK(game::class_name(1) == "Vanguard");
    game::clear_class_name_overrides(1);
    CHECK(game::class_name(1) == "Bulwark");
    CHECK(game::class_short_name(1) == "BLW");
    game::clear_class_name_overrides(2);
    CHECK(game::class_name(1) == "Warrior");

    // Clearing every owner (the session left the server).
    REQUIRE(game::set_class_name_override(1, 1, "Vanguard", "VAN"));
    REQUIRE(game::set_class_name_override(2, 6, "Mystic", "MYS"));
    game::clear_class_name_overrides();
    CHECK(game::class_name(1) == "Warrior");
    CHECK(game::class_name(6) == "Druid");
}

TEST_CASE("class names: refused input changes nothing") {
    Clean clean;
    REQUIRE(game::set_class_name_override(7, 1, "Vanguard", "VAN"));
    const auto revision = game::class_names_revision();
    const std::string longest(game::kClassNameMax, 'x');
    CHECK_FALSE(game::set_class_name_override(7, 0, "Nobody", "NOB"));                   // not a player class
    CHECK_FALSE(game::set_class_name_override(7, 17, "Nobody", "NOB"));
    CHECK_FALSE(game::set_class_name_override(7, 20, "Nobody", "NOB"));                  // a guildmaster follows its class
    CHECK_FALSE(game::set_class_name_override(0, 1, "Nobody", "NOB"));                   // no owner
    CHECK_FALSE(game::set_class_name_override(7, 1, longest + "x", "VAN"));              // 33 bytes
    CHECK_FALSE(game::set_class_name_override(7, 1, "Vanguard", "VANGD"));               // 5 bytes
    CHECK_FALSE(game::set_class_name_override(7, 1, "Van\nguard", "VAN"));               // a control character
    CHECK_FALSE(game::set_class_name_override(7, 1, std::string("Van\0g", 5), "VAN"));   // an embedded NUL
    CHECK(game::class_names_revision() == revision);
    CHECK(game::class_name(1) == "Vanguard");
    // The longest allowed (UTF-8 counts its bytes).
    CHECK(game::set_class_name_override(7, 1, longest, "VANG"));
    CHECK(game::class_name(1) == longest);
    CHECK(game::set_class_name_override(7, 2, "Pr\xC3\xAAtre", "PR\xC3\x8A")); // "PRÊ": 4 bytes
    CHECK(game::class_short_name(2) == "PR\xC3\x8A");
}

TEST_CASE("class names: /who shows and takes the server's names") {
    Clean clean;
    game::EqStrings strings;
    game::WhoContext ctx;
    REQUIRE(game::set_class_name_override(9, 1, "Vanguard", "VAN"));
    REQUIRE(game::set_class_name_override(9, 6, "Mystic", "MYS"));
    // The class column: the server's name, without the stock class's level titles.
    CHECK(game::who_class_text(strings, 1, 60) == "Vanguard");
    CHECK(game::who_class_text(strings, 1, 10) == "Vanguard");
    // A word names a class by the start of its stock name first, as on a stock server, then of the server's.
    CHECK(game::parse_who_args("vanguard", ctx, strings).class_ == 1);
    CHECK(game::parse_who_args("war", ctx, strings).class_ == 1);
    CHECK(game::parse_who_args("mys", ctx, strings).class_ == 6);
    CHECK(game::parse_who_args("m", ctx, strings).class_ == 7); // Monk, as stock
    game::clear_class_name_overrides(9);
    CHECK(game::parse_who_args("vanguard", ctx, strings).class_ == 0xffffffffu); // a name again
}

TEST_CASE("class names: MQ's Class members and spawn searches") {
    Clean clean;
    mq_test::World world{"hailstone_class_names_test"};
    mq::Registry reg;
    mq::register_game_tlos(reg);
    mq::Context ctx;
    ctx.registry = &reg;
    ctx.conn = world.conn.get();
    auto p = [&](std::string_view text) { return mq::parse_macro_data(ctx, text); };

    // The test world's player is a Cleric, its NPCs Warriors.
    REQUIRE(game::set_class_name_override(11, 2, "Mender", "MND"));
    REQUIRE(game::set_class_name_override(11, 1, "Vanguard", "VAN"));
    CHECK(p("${Me.Class}") == "Mender");
    CHECK(p("${Me.Class.Name}") == "Mender");
    CHECK(p("${Me.Class.ShortName}") == "MND");
    CHECK(p("${Me.Class.ID}") == "2");
    CHECK(p("${Spawn[10].Class}") == "Vanguard");

    // A search takes the stock and the server's names, long or short, for the same class.
    CHECK(mq::parse_spawn_search("vanguard").class_name == "Warrior");
    CHECK(mq::parse_spawn_search("npc 2 van").class_name == "Warrior");
    CHECK(mq::parse_spawn_search("npc war").class_name == "Warrior");
    CHECK(p("${SpawnCount[npc vanguard]}") == "2");
    CHECK(p("${SpawnCount[npc warrior]}") == "2");
    CHECK(p("${SpawnCount[npc class Vanguard]}") == "2");
    CHECK(p("${SpawnCount[npc class Warrior]}") == "2");
    CHECK(p("${SpawnCount[npc class Cleric]}") == "0");

    game::clear_class_name_overrides(11);
    CHECK(p("${Me.Class}") == "Cleric");
    CHECK(p("${SpawnCount[npc class Vanguard]}") == "0");
}
