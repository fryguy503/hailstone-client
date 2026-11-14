// The item comparison (game/item_compare.h): the worn slot swapped on a copy of the player, the numbers the
// inventory would show before and after, the slot choice and the class weighting's verdict.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <lua.hpp>

#include "app/lua_eq.h"
#include "core/game_dir.h"
#include "game/item_compare.h"
#include "game/mechanics/character.h"
#include "game/mechanics/tables.h"
#include "net/game_connection.h"
#include "net/item_packets.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace mech = hailstone::game::mechanics;
using game::CompareStat;
using game::Verdict;

namespace {

constexpr const char* kBaseData = "60^1^400^0^300^3^6^0.9^0^1.25\n60^12^100^500^300^1^3^0.9^1.2^1.25\n";
constexpr const char* kSkillCaps = "1^15^60^210^0\n";

struct Fixture {
    mech::GameTables tables;
    mech::SpellLookup no_spells = [](std::int32_t) -> const game::SpellData* { return nullptr; };
    mech::CharacterSheet sheet;
    mech::CharacterSheet scratch;
    game::CompareOptions options;

    explicit Fixture(std::uint8_t cls = 1) {
        tables.class_levels.parse(kBaseData);
        tables.skill_caps.parse(kSkillCaps);
        sheet.race = 1;
        sheet.class_id = cls;
        sheet.level = sheet.highest_level = 60;
        sheet.base_str = sheet.base_sta = sheet.base_cha = sheet.base_dex = sheet.base_int = sheet.base_agi =
            sheet.base_wis = 75;
        sheet.stored_hp = 100000;
        sheet.buffs.resize(mech::kPlayerBuffSlots);
    }

    std::optional<game::ItemComparison> compare(const net::Item& item) {
        mech::Character ch(sheet, tables, no_spells);
        ch.process_all_stats();
        return game::compare_item(ch, item, options, scratch);
    }
};

net::Item gear(std::string name, std::uint32_t slots) {
    net::Item i;
    i.name = std::move(name);
    i.id = 1000;
    i.races = i.classes = 0xffff;
    i.equip_slots = slots;
    return i;
}

const game::StatChange* change(const game::SlotComparison& s, CompareStat stat) {
    auto it = std::find_if(s.changes.begin(), s.changes.end(), [&](const auto& c) { return c.stat == stat; });
    return it == s.changes.end() ? nullptr : &*it;
}

std::string run(lua_State* L, const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

} // namespace

TEST_CASE("Item compare: an empty slot gains the item's HP, and that is an upgrade") {
    Fixture f;
    net::Item helm = gear("Iron Helm", 1u << 2);
    helm.hp = 50;
    auto r = f.compare(helm);
    REQUIRE(r);
    CHECK(r->usable);
    REQUIRE(r->slots.size() == 1);
    const auto& s = r->slots[0];
    CHECK(s.slot == 2);
    CHECK(s.current == std::vector<std::string>{""});
    const auto* hp = change(s, CompareStat::Hp);
    REQUIRE(hp);
    CHECK(hp->delta() == 50);
    CHECK(hp->weight > 0);
    CHECK(s.verdict == Verdict::Upgrade);
    CHECK(s.score > 0);
}

TEST_CASE("Item compare: a stat at its cap does not change, so the item is no upgrade") {
    Fixture f;
    f.sheet.base_str = 400; // far above the level 60 cap
    net::Item belt = gear("Belt of Might", 1u << 20);
    belt.astr = 10;
    auto r = f.compare(belt);
    REQUIRE(r);
    REQUIRE(r->slots.size() == 1);
    CHECK(change(r->slots[0], CompareStat::Str) == nullptr);
    CHECK(r->slots[0].verdict == Verdict::Same);
}

TEST_CASE("Item compare: the worn item's loss counts against the new one") {
    Fixture f;
    net::Item old_helm = gear("Old Helm", 1u << 2);
    old_helm.hp = 80;
    f.sheet.possessions[2] = old_helm;
    net::Item helm = gear("New Helm", 1u << 2);
    helm.hp = 30;
    auto r = f.compare(helm);
    REQUIRE(r);
    const auto& s = r->slots.at(0);
    CHECK(s.current == std::vector<std::string>{"Old Helm"});
    REQUIRE(change(s, CompareStat::Hp));
    CHECK(change(s, CompareStat::Hp)->delta() == -50);
    CHECK(s.verdict == Verdict::Downgrade);
}

TEST_CASE("Item compare: of a pair of slots the weaker one comes first") {
    Fixture f;
    net::Item strong = gear("Strong Earring", (1u << 1) | (1u << 4));
    strong.hp = 20;
    net::Item weak = gear("Weak Earring", (1u << 1) | (1u << 4));
    weak.hp = 5;
    f.sheet.possessions[1] = strong;
    f.sheet.possessions[4] = weak;
    net::Item ear = gear("New Earring", (1u << 1) | (1u << 4));
    ear.hp = 10;
    auto r = f.compare(ear);
    REQUIRE(r);
    REQUIRE(r->slots.size() == 2);
    CHECK(r->slots[0].slot == 4);
    CHECK(r->slots[0].verdict == Verdict::Upgrade);
    CHECK(r->slots[1].slot == 1);
    CHECK(r->slots[1].verdict == Verdict::Downgrade);
}

TEST_CASE("Item compare: a two-hander takes off the primary and the secondary") {
    Fixture f;
    net::Item sword = gear("Short Sword", (1u << 13) | (1u << 14));
    sword.item_type = 0;
    sword.damage = 10;
    sword.delay = 30;
    net::Item shield = gear("Round Shield", 1u << 14);
    shield.item_type = 8;
    shield.ac = 20;
    f.sheet.possessions[13] = sword;
    f.sheet.possessions[14] = shield;
    net::Item great = gear("Greatsword", 1u << 13);
    great.item_type = 1;
    great.damage = 30;
    great.delay = 40;
    auto r = f.compare(great);
    REQUIRE(r);
    REQUIRE(r->slots.size() == 1);
    const auto& s = r->slots[0];
    CHECK(s.replaced == std::vector<int>{13, 14});
    CHECK(s.current == std::vector<std::string>{"Short Sword", "Round Shield"});
    REQUIRE(s.weapon_before);
    REQUIRE(s.weapon_after);
    CHECK(s.weapon_before->damage == 10);
    CHECK(s.weapon_after->damage == 30);
    CHECK(s.weapon_after->delay == 40);
    const auto* ratio = change(s, CompareStat::WeaponRatio);
    REQUIRE(ratio);
    CHECK(ratio->before == 33);
    CHECK(ratio->after == 75);
    const auto* ac = change(s, CompareStat::Ac);
    REQUIRE(ac);
    CHECK(ac->delta() < 0); // the shield's AC goes
}

TEST_CASE("Item compare: what the player cannot use says why and is never an upgrade") {
    Fixture f;
    net::Item robe = gear("Wizard Robe", 1u << 17);
    robe.classes = 1u << 11; // wizard only
    robe.hp = 40;
    auto r = f.compare(robe);
    REQUIRE(r);
    CHECK_FALSE(r->usable);
    CHECK(r->why_not == "Your class cannot use this item");
    CHECK(r->slots.at(0).verdict == Verdict::CantUse);

    net::Item high = gear("High Helm", 1u << 2);
    high.req_level = 65;
    high.hp = 40;
    r = f.compare(high);
    REQUIRE(r);
    CHECK_FALSE(r->usable);
    CHECK(r->why_not == "Requires level 65");

    f.options.can_use_all = true; // a server extension's CanUseItem answers yes for the class
    r = f.compare(robe);
    REQUIRE(r);
    CHECK(r->usable);
}

TEST_CASE("Item compare: a recommended level above the player's is noted") {
    Fixture f;
    net::Item helm = gear("Fine Helm", 1u << 2);
    helm.rec_level = 70;
    helm.hp = 70;
    auto r = f.compare(helm);
    REQUIRE(r);
    REQUIRE(r->notes.size() == 1);
    CHECK(r->notes[0] == "Recommended level 70: its stats are reduced for you");
    REQUIRE(change(r->slots.at(0), CompareStat::Hp));
    CHECK(change(r->slots.at(0), CompareStat::Hp)->delta() == 60); // 60 / 70 x 70 (the recommended-level scaler)
}

TEST_CASE("Item compare: not worn gear has no comparison") {
    Fixture f;
    CHECK_FALSE(f.compare(gear("Rations", 0)));
    net::Item bag = gear("Backpack", 1u << 22);
    bag.item_class = 1;
    CHECK_FALSE(f.compare(bag));
    CHECK_FALSE(f.compare(gear("Charm-ish", 1u << 23))); // a general slot is not a worn one
}

TEST_CASE("Item compare: effects gained and lost") {
    Fixture f;
    net::Item old_boots = gear("Old Boots", 1u << 19);
    old_boots.effects[net::kEffectWorn].effect = 100;
    old_boots.effects[net::kEffectWorn].name = "Run Speed";
    f.sheet.possessions[19] = old_boots;
    net::Item boots = gear("New Boots", 1u << 19);
    boots.effects[net::kEffectFocus].effect = 200;
    boots.effects[net::kEffectFocus].name = "Improved Healing I";
    auto r = f.compare(boots);
    REQUIRE(r);
    const auto& s = r->slots.at(0);
    CHECK(s.gained == std::vector<std::string>{"Focus: Improved Healing I"});
    CHECK(s.lost == std::vector<std::string>{"Worn: Run Speed"});
}

TEST_CASE("Item compare: a server extension's vitals do not hide the HP change") {
    Fixture f;
    f.sheet.vitals.max_hp = 99999;
    net::Item helm = gear("Iron Helm", 1u << 2);
    helm.hp = 50;
    auto r = f.compare(helm);
    REQUIRE(r);
    REQUIRE(change(r->slots.at(0), CompareStat::Hp));
    CHECK(change(r->slots.at(0), CompareStat::Hp)->delta() == 50);
}

TEST_CASE("Item compare: the class weighting") {
    const std::uint32_t warrior = 1u << 0, wizard = 1u << 11, cleric = 1u << 1;
    CHECK(game::compare_weight(CompareStat::Mana, warrior) == 0.0);
    CHECK(game::compare_weight(CompareStat::Mana, wizard) > 0.0);
    CHECK(game::compare_weight(CompareStat::Str, wizard) == 0.0);
    CHECK(game::compare_weight(CompareStat::Int, wizard) > 0.0);
    CHECK(game::compare_weight(CompareStat::Int, cleric) == 0.0);
    CHECK(game::compare_weight(CompareStat::Wis, cleric) > 0.0);
    CHECK(game::compare_weight(CompareStat::Ac, warrior) > game::compare_weight(CompareStat::Ac, wizard));
    // A multiclass player counts what any of its classes would.
    CHECK(game::compare_weight(CompareStat::Mana, warrior | wizard) == game::compare_weight(CompareStat::Mana, wizard));

    // STR alone is an upgrade for a warrior and no change that counts for a wizard.
    net::Item belt = gear("Belt of Might", 1u << 20);
    belt.astr = 10;
    Fixture war(1);
    CHECK(war.compare(belt)->slots.at(0).verdict == Verdict::Upgrade);
    Fixture wiz(12);
    auto r = wiz.compare(belt);
    REQUIRE(r);
    REQUIRE(change(r->slots.at(0), CompareStat::Str));
    CHECK(r->slots.at(0).verdict == Verdict::Same);
}

TEST_CASE("Item compare: eq.CompareItem, eq.CompareSlotItem and eq.CompareBagSlotItem for the packs") {
    hailstone::test::ScratchDir dir("item_compare_eq");
    GameDir game(dir.path());
    net::GameConnection conn(game, {});

    // A helm worn, a better one in general slot 23.
    net::Item worn = gear("Old Helm", 1u << 2);
    worn.hp = 10;
    worn.slot = net::ItemSlot::possessions(2);
    net::Item carried = gear("Iron Helm", 1u << 2);
    carried.hp = 50;
    carried.ac = 5;
    carried.slot = net::ItemSlot::possessions(23);
    net::Item food = gear("Rations", 0);
    food.slot = net::ItemSlot::possessions(24);
    conn.receive_zone_packet_for_test(net::encode_char_inventory({worn, carried, food}));

    Fixture f;
    f.sheet.possessions[2] = worn;
    mech::Character ch(f.sheet, f.tables, f.no_spells);
    ch.process_all_stats();

    app::EqHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    std::vector<net::Item> displays{carried};
    host.item_displays = &displays;
    app::EqApi api(host);
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "eq");

    // Before the player's stats exist there is nothing to compare with.
    CHECK(run(L, "return eq.CompareItem(23)") == "nil");
    host.character = &ch;

    CHECK(run(L, "local c = eq.CompareItem(23) return tostring(c.usable) .. ' ' .. #c.slots") == "true 1");
    CHECK(run(L, "local s = eq.CompareItem(23).slots[1] return s.slot .. ' ' .. s.slotName .. ' ' .. s.current[1] .. ' ' .. s.verdict") ==
          "2 Head Old Helm upgrade");
    CHECK(run(L, "for _, c in ipairs(eq.CompareItem(23).slots[1].changes) do "
                 "if c.key == 'hp' then return c.name .. ' ' .. c.before .. ' ' .. c.after .. ' ' .. c.delta end end") ==
          "HP " + std::to_string(ch.max_hp(true)) + " " + std::to_string(ch.max_hp(true) + 40) + " 40");
    CHECK(run(L, "return eq.CompareItem(2)") == "nil");  // a worn item is what the others compare against
    CHECK(run(L, "return eq.CompareItem(24)") == "nil"); // not gear
    CHECK(run(L, "return eq.CompareItem(25)") == "nil"); // an empty slot

    // A linked item, in the item display's icon.
    CHECK(run(L, "return eq.CompareSlotItem('ItemDisplayWindow', 'Hailstone_Item').slots[1].verdict") == "upgrade");
    CHECK(run(L, "return eq.CompareSlotItem('ItemDisplayWindow#2', 'Hailstone_Item')") == "nil");
    CHECK(run(L, "return eq.CompareSlotItem('NoSuchWindow', 'x')") == "nil");
    CHECK(run(L, "return eq.CompareBagSlotItem('Bag1', 0)") == "nil");

    host.in_game = false;
    CHECK(run(L, "return eq.CompareSlotItem('ItemDisplayWindow', 'Hailstone_Item')") == "nil");
    lua_close(L);
}

namespace {
std::filesystem::path pack_lib(const char* pack) {
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "ui" / pack / "lib" / "compare_lines.lua";
}
} // namespace

// Both packs show the comparison with the same lib/compare_lines.lua: the gamepad pack's is a copy, kept the same.
TEST_CASE("Item compare: the packs' compare_lines.lua are the same file") {
    auto read = [](const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    const std::string modern = read(pack_lib("modern"));
    REQUIRE_FALSE(modern.empty());
    CHECK(modern == read(pack_lib("gamepad")));
}

TEST_CASE("Item compare: compare_lines.lua puts what counts for the class first and says the verdict") {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    REQUIRE(luaL_dofile(L, pack_lib("modern").string().c_str()) == LUA_OK);
    lua_setglobal(L, "CL");
    const char* fixture = R"(
        c = { usable = true, whyNot = "", notes = { "Recommended level 70: its stats are reduced for you" },
          slots = {
            { slot = 4, slotName = "Right Ear", current = { "Weak Earring" }, verdict = "mixed", score = 12,
              changes = {
                { key = "hp", name = "HP", before = 1000, after = 1020, delta = 20, weight = 1 },
                { key = "cha", name = "CHA", before = 80, after = 70, delta = -10, weight = 0 },
                { key = "ac", name = "AC", before = 300, after = 298, delta = -2, weight = 4 },
                { key = "haste", name = "Haste", before = 0, after = 10, delta = 10, weight = 5 },
              },
              gained = { "Worn: Haste" }, lost = {} },
            { slot = 1, slotName = "Left Ear", current = { "" }, verdict = "upgrade", score = 99, changes = {},
              gained = {}, lost = {} },
          } }
        function joined(index, opts)
          local t = {}
          for _, l in ipairs(CL.lines(c, index, opts)) do t[#t + 1] = l[2] .. ":" .. l[1] end
          return table.concat(t, "|")
        end)";
    REQUIRE(luaL_dostring(L, fixture) == LUA_OK);
    CHECK(run(L, "return joined(1, { verdict = true })") ==
          "muted:Compared with your Weak Earring (Right Ear)|mixed:Mixed: better overall|"
          "good:+10% Haste (0% to 10%)|good:+20 HP (1000 to 1020)|bad:-2 AC (300 to 298)|muted:-10 CHA (80 to 70)|"
          "good:Gains Worn: Haste|muted:Recommended level 70: its stats are reduced for you");
    CHECK(run(L, "return joined(2, {})") ==
          "muted:Compared with your empty Left Ear slot|muted:No change to your stats|"
          "muted:Recommended level 70: its stats are reduced for you");
    CHECK(run(L, "return CL.otherSlotName(c, 1) .. ' ' .. CL.otherSlotName(c, 2)") == "Left Ear Right Ear");
    // The tooltip shows MAX_CHANGES and counts the rest; the item display shows them all.
    CHECK(run(L, "CL.MAX_CHANGES = 2 return joined(1, {})") ==
          "muted:Compared with your Weak Earring (Right Ear)|good:+10% Haste (0% to 10%)|good:+20 HP (1000 to 1020)|"
          "muted:... and 2 more|good:Gains Worn: Haste|muted:Recommended level 70: its stats are reduced for you");
    CHECK(run(L, "return select(2, joined(1, { full = true }):gsub('%(', ''))") == "5"); // the heading's and 4 changes
    // An item the player cannot use says why first, with no verdict line.
    CHECK(run(L, "c.usable = false c.whyNot = 'Requires level 65' c.slots[1].verdict = 'cantuse' "
                 "return CL.lines(c, 1, { verdict = true })[1][1] .. '|' .. CL.lines(c, 1, { verdict = true })[3][1]") ==
          "Can't use: Requires level 65|+10% Haste (0% to 10%)");
    CHECK(run(L, "return CL.change({ key = 'weaponRatio', name = 'Weapon Ratio', before = 33, after = 75, delta = 42 })") ==
          "+0.42 Weapon Ratio (0.33 to 0.75)");
    lua_close(L);
}
