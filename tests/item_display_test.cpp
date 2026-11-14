// The item display's text (game/item_display.h): CItemDisplayWnd_BuildItemInfo's lines and grid.
#include <doctest/doctest.h>

#include "game/eq_strings.h"
#include "game/item_display.h"

using namespace hailstone;

namespace {
game::EqStrings strings() {
    game::EqStrings s;
    for (auto [id, t] : std::initializer_list<std::pair<std::uint32_t, const char*>>{
             {0xbf8, "Class: "}, {0xbe9, "Race: "}, {0x3395, "ALL"}, {0x4c0, "None"}, {3065, "WAR"}, {3067, "PAL"},
             {0xbea, "HUM"}, {0xbeb, "BAR"}, {569, "Primary"}, {570, "Secondary"}, {0x32cb, "Size:"}, {3130, "MEDIUM"},
             {12999, "Weight:"}, {0x32cf, "Base Dmg:"}, {0x32d1, "Delay:"}, {0x32dc, "Strength:"}, {0x32ce, "AC:"},
             {0xc16, "Magic"}, {0xc1b, "No Trade"}, {0x32f5, "Skill:"}, {0x3411, "Unmodified"}})
        s.set(id, t);
    return s;
}
} // namespace

TEST_CASE("build_item_display: info lines, then the grid in its columns and blocks") {
    auto s = strings();
    net::Item it;
    it.name = "Test Sword";
    it.magic = 1;
    it.no_drop = 0; // No Trade
    it.no_rent = 1; // not Temporary
    it.classes = 1 | 4;
    it.races = 1 | 2;
    it.equip_slots = (1u << 13) | (1u << 14);
    it.size = 2;
    it.weight = 50;
    it.damage = 4;
    it.delay = 29;
    it.astr = 3;
    it.heroic_str = 2;
    game::ItemDisplayContext ctx;
    ctx.strings = &s;
    ctx.can_equip = [](const net::Item&) { return true; };
    auto d = game::build_item_display(it, ctx);
    REQUIRE(d.info.size() == 5);
    CHECK(d.info[0].text == "Test Sword");
    CHECK(d.info[1].text == "Magic, No Trade");
    CHECK(d.info[2].text == "Class: WAR PAL ");
    CHECK(d.info[3].text == "Race: HUM BAR ");
    CHECK(d.info[4].text == "Primary Secondary ");
    CHECK(d.info[0].argb == 0xff00ff00u); // can_equip, and no character for the prestige check
    CHECK(d.grid[0][0].stat == "Size:");
    CHECK(d.grid[0][0].value == "MEDIUM");
    CHECK(d.grid[1][0].value == "       5.0"); // "%10.1f"
    CHECK(d.grid[0][2].stat == "Base Dmg:");
    CHECK(d.grid[1][2].value == "29");
    // The second block: one blank row below the longest first-block column (rows 1..3 → row 5).
    CHECK_FALSE(d.grid[3][0].shown);
    CHECK(d.grid[4][0].stat == "Strength:");
    CHECK(d.grid[4][0].value == "3");
    CHECK(d.grid[4][0].heroic == "+2");
    // The strings the window's width is measured from (@006a9e6b).
    CHECK(d.measured[0] == "Test Sword");
    CHECK(d.measured[1] == "Magic, No Trade");
    CHECK(d.measured[2] == "Primary Secondary ");
    CHECK(d.measured[3] == "Class: WAR PAL ");
    CHECK(d.measured[5] == "Race: HUM BAR ");
    CHECK(d.measured[7].empty()); // no deity
}

TEST_CASE("item_display_text_width: the longer of name and flags, then the other lines") {
    game::ItemDisplay d;
    d.measured[0] = "abc";
    d.measured[1] = "abcdef";
    auto extent = [](std::string_view s) { return static_cast<int>(s.size()) * 10 + (s == "abc" ? 1000 : 0); };
    CHECK(game::item_display_text_width(d, extent) == 60); // the name is not measured: the flags are longer
    d.measured[4] = "Class: SHM NEC WIZ ";
    CHECK(game::item_display_text_width(d, extent) == 190);
}

TEST_CASE("layout_item_display: the end of CItemDisplayWnd_BuildItemInfo") {
    game::ItemDisplay d;
    d.info.resize(5);
    d.last_row = 5;
    d.columns = 2;
    game::ItemDisplayFrameIn in;
    in.text_width = 100;
    auto l = game::layout_item_display(d, in);
    // Row 1 32 below the last info line (74); the Modified button on row 7 (16 a row); the description 15 + 8 below.
    CHECK(l.grid_top == 106);
    CHECK(l.mod_top == 202);
    CHECK(l.description_top == 225);
    CHECK(l.height == 225 + 150);
    CHECK(l.width == 240 + 50); // two value columns end at 240
    CHECK(l.socket_top[0] == -1);

    // Two sockets: 11 below the button's row, then 3 apart, 20 high; three columns; a long description widens.
    in.sockets[0] = in.sockets[1] = true;
    in.socket_text[0] = 0x55;
    in.socket_text[1] = 100;
    l = game::layout_item_display(d, in);
    CHECK(l.socket_top[0] == 228);
    CHECK(l.socket_top[1] == 251);
    CHECK(l.description_top == 279);
    CHECK(l.height == 279 + 150);
    CHECK(l.width == 360 + 98 + 50); // (100 - 85) * 6.6f = 98.99.., truncated

    // A wide info line: its width from IDW_ItemInfo1's x, and 70.
    in.text_width = 400;
    CHECK(game::layout_item_display(d, in).width == 60 + 400 + 70);

    // An appearance socket 5 below the last info line; the grid from its top + 24.
    in = {};
    in.appearance = true;
    l = game::layout_item_display(d, in);
    CHECK(l.appearance_top == 99);
    CHECK(l.grid_top == 99 + 24 + 32);
    CHECK(l.width == 360 + 50);

    // Past row 26 the button's row is a value label's (row - 26).
    in = {};
    d.last_row = 25;
    CHECK(game::layout_item_display(d, in).mod_top == 106);
}

TEST_CASE("weapon_damage_bonus: 0x7e8a10") {
    net::Item one;
    one.item_type = 0;
    one.delay = 30;
    CHECK(game::weapon_damage_bonus(27, 1, one) == 0);
    CHECK(game::weapon_damage_bonus(50, 2, one) == 0);     // a cleric gets none
    CHECK(game::weapon_damage_bonus(50, 1, one) == 8);     // (50 - 28) / 3 + 1
    net::Item two;
    two.item_type = 1;
    two.delay = 40;
    // 2H at 60: 11 + ((60-30)/5+1 = 7, +1 over 50, + 40 * (10 + 4 over 54/56/58/59) / 40 = 14) + delay 40: 1
    CHECK(game::weapon_damage_bonus(60, 1, two) == 11 + 7 + 1 + 14 + 1);
}

#include "game/spell_display.h"
#include "game/spells.h"

TEST_CASE("build_spell_display: CSpellDisplayWnd::SetSpell's lines") {
    game::EqStrings s;
    for (auto [id, t] : std::initializer_list<std::pair<std::uint32_t, const char*>>{
             {0xc1d, "Spell:"}, {0xc1e, "Duration: %1"}, {0xc1f, "Duration: Permanent"}, {0x1fa, "Level: %1"},
             {0xc21, "Skill: %1"}, {0xbbe, "Mana: %1"}, {0xc23, "Cast: %1"}, {0x317, "Timer: %1"}})
        s.set(id, t);
    game::SpellData sp;
    sp.name = "Minor Healing";
    sp.cast_time = 1500;
    sp.mana = 10;
    sp.class_level.fill(255);
    sp.class_level[1] = 1; // cleric
    game::SpellDisplayContext ctx;
    ctx.strings = &s;
    ctx.player_class = 2;
    ctx.player_level = 5;
    auto d = game::build_spell_display(sp, ctx);
    CHECK(d.title == "Spell:  Minor Healing");
    CHECK(d.description.find("Level: 1<BR>") != std::string::npos);
    CHECK(d.description.find("Mana: 10<BR>") != std::string::npos);
    CHECK(d.description.find("Cast: 1.5") != std::string::npos);
    CHECK(game::format_duration_hms(3600 + 60) == "1h 1m");
    CHECK(game::format_duration_hms(90) == "1m 30s");
    CHECK(game::format_duration_hms(7200) == "2h");
}

TEST_CASE("build_item_display: the meal size, the compass's direction and the evolving line (CreateEffectsString)") {
    auto s = strings();
    s.set(3081, "This is a meal.");
    s.set(3093, "This is a miraculous drink!");
    s.set(3009, "The compass points %1.");
    s.set(12433, "West");
    s.set(12427, "North");
    s.set(6146, "Evolving: Level %1/%2 %3%% %4");
    s.set(6194, "Final Result");
    game::ItemDisplayContext ctx;
    ctx.strings = &s;
    net::Item food;
    food.name = "Bread";
    food.item_type = 14;
    food.cast_time_ = 20;
    CHECK(game::build_item_display(food, ctx).description.find("This is a meal.<BR>") != std::string::npos);
    food.item_type = 15;
    food.cast_time_ = 61;
    CHECK(game::build_item_display(food, ctx).description.find("This is a miraculous drink!") != std::string::npos);
    net::Item compass;
    compass.name = "Compass";
    compass.item_type = 40;
    ctx.heading = 128.0f; // west
    CHECK(game::build_item_display(compass, ctx).description.find("The compass points West.") != std::string::npos);
    ctx.heading = 500.0f;
    CHECK(game::build_item_display(compass, ctx).description.find("The compass points North.") != std::string::npos);
    net::Item ev;
    ev.name = "Evolving Thing";
    ev.id = 1234;
    ev.is_evolving = 1;
    ev.evolving.evolve_level = 2;
    ev.evolving.evolve_max_level = 5;
    ev.evolving.progress = 42.7;
    ev.evolving.final_item_id = 999;
    auto text = game::build_item_display(ev, ctx).description;
    CHECK(text.find("Evolving: Level 2/5 42% <a WndNotify=\"27,1^1234^999^2^") != std::string::npos);
    CHECK(text.find("\">Final Result</a>") != std::string::npos);
}

TEST_CASE("build_item_display: placeable, collectible and trophy lines") {
    game::EqStrings s;
    for (auto [id, t] : std::initializer_list<std::pair<std::uint32_t, const char*>>{
             {6413, "Placeable"}, {6414, "This item is placeable in"}, {6415, "certain open zones"}, {6416, "houses"},
             {6417, "guild halls"}, {5265, "yards"}, {748, "guild yards"}, {7688, "Collectible"}, {7693, "No Destroy"},
             {6155, "No Storage"}, {7690, "Right click on this item to add it to a collection."}})
        s.set(id, t);
    game::ItemDisplayContext ctx;
    ctx.strings = &s;
    net::Item it;
    it.name = "Chair";
    it.no_drop = 1;
    it.no_rent = 1;
    it.placeable = 1; // EQEmu's Placeable byte: the low byte of the flags
    auto d = game::build_item_display(it, ctx);
    CHECK(d.info[1].text == "Placeable");
    CHECK(d.description.find("<BR>This item is placeable in certain open zones.<BR>") != std::string::npos);
    it.placeable = 0x0e; // houses, guild halls (and 8: yards)
    d = game::build_item_display(it, ctx);
    CHECK(d.description.find("This item is placeable in yards, houses and guild halls.") != std::string::npos);
    it.placeable = 0xff;
    it.unknown22 = {0xff, 0xff, 0xff}; // -1: everywhere
    d = game::build_item_display(it, ctx);
    CHECK(d.description.find("in yards, guild yards, houses, guild halls and certain open zones.") != std::string::npos);
    // Bytes 1 and 2 of unknown37: Collectible and No Destroy, after Copied in the flags; the collection hint.
    it = {};
    it.name = "Shiny";
    it.no_drop = 1;
    it.no_rent = 1;
    it.no_transfer = 1;
    it.unknown34_37[3] = 0x00010100;
    d = game::build_item_display(it, ctx);
    CHECK(d.info[1].text == "No Storage, Collectible, No Destroy");
    CHECK(d.description.find("Right click on this item to add it to a collection.<BR>") != std::string::npos);
    // A trophy: a benefit id and the house flag (personal); the benefit from the context.
    it = {};
    it.name = "Trophy";
    it.no_drop = 1;
    it.no_rent = 1;
    it.placeable = 4;
    it.unknown30 = 7;
    bool asked_guild = true;
    ctx.trophy_benefit = [&](std::int32_t id, bool guild) -> std::optional<std::pair<std::string, std::string>> {
        asked_guild = guild;
        if (id != 7) return std::nullopt;
        return std::pair<std::string, std::string>{"Mana Font", "Restores mana."};
    };
    d = game::build_item_display(it, ctx);
    CHECK_FALSE(asked_guild);
    CHECK(d.description.find("This trophy grants you access to Mana Font.<BR><BR><C \"#8AA3FF\">Mana Font</C><BR>"
                             "Restores mana.<BR>") != std::string::npos);
    it.placeable = 2; // guild halls: a guild trophy
    d = game::build_item_display(it, ctx);
    CHECK(asked_guild);
    CHECK(d.description.find("This guild trophy grants your guild access to Mana Font.") != std::string::npos);
    it.unknown30 = 0xffffffff; // EQEmu's: none
    CHECK(game::build_item_display(it, ctx).description.find("trophy") == std::string::npos);
}

TEST_CASE("build_item_display: the power percentage of a large power does not wrap") {
    auto s = strings();
    s.set(0x340b, "Power:");
    net::Item it;
    it.name = "Charged";
    it.power = 50000000; // * 100 is past 2^32
    game::ItemDisplayContext ctx;
    ctx.strings = &s;
    auto d = game::build_item_display(it, ctx);
    bool found = false;
    for (const auto& row : d.grid)
        for (const auto& cell : row)
            if (cell.stat == "Power:") {
                found = true;
                CHECK(cell.value == "100%");
                CHECK(cell.tooltip == "50000000/50000000");
            }
    CHECK(found);
}

TEST_CASE("build_item_display: a stat label the game folder's eqstr_us.txt lacks falls back to a plain one") {
    // A live game folder's table has 13034 (Mana Regen) but not 13036 (Spell Shield) or 13315 (Strikethrough).
    game::EqStrings s;
    s.set(0x32ea, "Mana Regen:");
    s.set(0x32ec, "Spell Shield:");
    net::Item it;
    it.name = "Test Staff";
    it.mana_regen = 2;
    it.spell_shield = 3;
    it.avoidance = 4;
    it.strike_through = 5;
    game::ItemDisplayContext ctx;
    ctx.strings = &s;
    auto d = game::build_item_display(it, ctx);
    std::vector<std::string> labels;
    for (const auto& row : d.grid)
        if (row[2].shown) labels.push_back(row[2].stat);
    REQUIRE(labels.size() == 4);
    CHECK(labels[0] == "Mana Regen:");
    CHECK(labels[1] == "Spell Shield:"); // the table's own text when it has the id
    CHECK(labels[2] == "Avoidance:");
    CHECK(labels[3] == "Strikethrough:");
    for (const auto& l : labels) CHECK(l.find('[') == std::string::npos);
}
