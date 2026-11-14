// The character rules (src/game/mechanics): hand-computed expectations from
// the original's formulas (see each case for the function), including
// the cases where the original's 24-bit x87 precision gives a different
// result than IEEE doubles.
#include <doctest/doctest.h>

#include <array>
#include <climits>
#include <limits>
#include <map>

#include "game/inventory.h"
#include "game/mechanics/character.h"
#include "game/mechanics/find_affect_slot.h"
#include "game/mechanics/item_stats.h"
#include "game/mechanics/sheet_builder.h"
#include "game/mechanics/tables.h"
#include "game/eq_strings.h"
#include "game/spell_display.h"
#include "game/spells.h"
#include "game/world_state.h"
#include "game_files.h"
#include "net/zone_packets.h"

using namespace hailstone;
using namespace hailstone::game;
using namespace hailstone::game::mechanics;

namespace {

constexpr const char* kBaseData =
    "1^1^25^0^15^2^5^0.083^0^0.075\n"
    "60^1^400^0^300^3^6^0.9^0^1.25\n"
    "60^2^500^800^300^3^5^0.75^14^1.25\n"
    "60^3^480^600^300^3^5^0.8^12^1.25\n"
    "70^1^500^0^320^3^6^1.0^0^1.3\n"
    "20^1^100^0^50^2^5^0.25^0^0.3\n"
    "30^1^150^0^80^2^5^0.3^0^0.4\n";
constexpr const char* kSkillCaps =
    "1^15^60^210^0\n"
    "1^15^1^5^0\n"
    "2^31^60^235^0\n"
    "2^15^60^200^0\n"
    "1^15^100^252^0\n"
    "2^31^100^235^0\n";

struct World {
    GameTables tables;
    std::map<std::int32_t, SpellData> spells;
    World() {
        tables.class_levels.parse(kBaseData);
        tables.skill_caps.parse(kSkillCaps);
    }
    SpellLookup lookup() {
        return [this](std::int32_t id) -> const SpellData* {
            auto it = spells.find(id);
            return it == spells.end() ? nullptr : &it->second;
        };
    }
    // A spell whose effect slots are blank (254) except the given {spa, base, formula} ones.
    SpellData& spell(std::int32_t id, std::initializer_list<std::array<std::int32_t, 3>> effects,
                     std::uint32_t duration_formula = 50) {
        SpellData s;
        s.id = static_cast<std::uint32_t>(id);
        s.effect.fill(254);
        s.formula.fill(100);
        s.duration_formula = duration_formula;
        s.spell_type = 1;
        s.class_level.fill(255);
        std::size_t k = 0;
        for (const auto& e : effects) {
            s.effect[k] = e[0];
            s.base[k] = e[1];
            s.formula[k] = e[2];
            ++k;
        }
        return spells[id] = s;
    }
};

CharacterSheet warrior(int level) {
    CharacterSheet s;
    s.race = 1;
    s.class_id = 1;
    s.level = level;
    s.highest_level = level;
    s.base_str = s.base_sta = s.base_cha = s.base_dex = s.base_int = s.base_agi = s.base_wis = 75;
    s.stored_hp = 100000;
    s.buffs.resize(kPlayerBuffSlots);
    return s;
}

net::Item item() {
    net::Item i;
    i.item_class = 0;
    i.races = 0xffff;
    i.classes = 0xffff;
    return i;
}

SheetBuff buff(std::int32_t spell, std::uint8_t level = 60) {
    SheetBuff b;
    b.type = 2;
    b.level = level;
    b.spell_id = spell;
    b.duration = 100;
    return b;
}

} // namespace

TEST_CASE("Fpu reproduces 24-bit and 53-bit x87 rounding") {
    Fpu single(FpuPrecision::Single24), dbl(FpuPrecision::Double53);
    // A double exactly halfway between two floats: the exact error decides (TwoSum), not round-to-even.
    CHECK(single.add(1.0 + 0x1p-24, 0x1p-80) == 1.0 + 0x1p-23);
    CHECK(single.add(1.0 + 0x1p-24, -0x1p-80) == 1.0);
    CHECK(single.add(1.0 + 0x1p-24, 0.0) == 1.0);
    CHECK(dbl.add(1.0 + 0x1p-24, 0x1p-80) == 1.0 + 0x1p-24);
    CHECK(single.mul(70.0, static_cast<double>(0.01f)) == static_cast<double>(0.69999999f));
    CHECK(Fpu::ftol(-1.9) == -1);
    CHECK(Fpu::ftol(2147483648.0) == INT_MIN);
    CHECK(Fpu::ftol(std::numeric_limits<double>::quiet_NaN()) == INT_MIN);
}

TEST_CASE("BaseData.txt and SkillCaps.txt load as the original reads them") {
    ClassLevelTable t;
    CHECK(t.parse("1^1^25^0^15^2^5^0.083^0^0.075\n1^1^30^0^15^2^5^0.1^0^0.075\r\n101^1^9^9^9^9^9^9^9^9\n") == 2);
    REQUIRE(t.row(1, 1));
    CHECK(t.row(1, 1)->base_hp == 30.0); // a later line for the same pair wins
    CHECK(t.row(1, 1)->hp_per_stamina == 0.1);
    CHECK(t.row(0, 1) == nullptr);
    CHECK(t.row(101, 1) == nullptr);
    CHECK(t.row(1, 2) == nullptr);

    SkillCapTable c;
    c.parse("1^15^1^5^0\n1^15^2^10^0\n0x1^0x1f^0x5^0x14^0.5\n");
    CHECK(c.cap(1, 15, 2) == 10);
    CHECK(c.cap(1, 31, 5) == 20);
    CHECK(c.min_level(15, 1) == 1);
    CHECK(c.min_level(31, 1) == 5);
    CHECK(c.min_level(31, 2) == 0);
    CHECK(skill_level_capped(15));
    CHECK_FALSE(skill_level_capped(57));
    CHECK(skill_held_at_252(6));
    CHECK_FALSE(skill_held_at_252(15));
}

TEST_CASE("CalcAffectChangeGeneric") {
    World w;
    CharacterSheet s = warrior(60);
    Character ch(s, w.tables, w.lookup());
    SpellData& sp = w.spell(100, {{0, 10, 102}});
    CHECK(ch.calc_affect_change(sp, 30, 0, nullptr, 10, true) == 40);   // + level
    sp.formula[0] = 111;
    CHECK(ch.calc_affect_change(sp, 20, 0, nullptr, 10, true) == 34);   // + (level - 16) * 6
    sp.formula[0] = 102;
    CHECK(ch.calc_affect_change(sp, 30, 0, nullptr, -10, true) == -40); // a negative base stays negative
    sp.max[0] = 25;
    CHECK(ch.calc_affect_change(sp, 30, 0, nullptr, 10, true) == 25);   // capped at |max|
    CHECK(ch.calc_affect_change(sp, 30, 0, nullptr, 10, false) == 40);
    // A slow (SPA 11 below 100) goes down by the change and is floored at max.
    SpellData& slow = w.spell(101, {{11, 70, 102}});
    slow.max[0] = 50;
    CHECK(ch.calc_affect_change(slow, 30, 0, nullptr, 70, true) == 50);
    // 1005: 5 per tick already gone, from a buff of 300 ticks (formula 3) with 290 left.
    SpellData& decay = w.spell(102, {{0, 100, 1005}}, 3);
    SheetBuff b = buff(102, 10);
    b.duration = 290;
    CHECK(ch.calc_affect_change(decay, 10, 0, &b, 100, true) == 50);
}

TEST_CASE("SpellDuration") {
    World w;
    CharacterSheet s = warrior(60);
    Character ch(s, w.tables, w.lookup());
    SpellData& sp = w.spell(1, {{0, 1, 100}}, 1);
    CHECK(ch.spell_duration(sp, 1) == 1);
    CHECK(ch.spell_duration(sp, 9) == 4);
    sp.duration_formula = 3;
    CHECK(ch.spell_duration(sp, 10) == 300);
    sp.duration_cap = 100;
    CHECK(ch.spell_duration(sp, 10) == 100);
    sp.duration_formula = 50;
    CHECK(ch.spell_duration(sp, 10) == -1);
    sp.duration_formula = 300;
    sp.duration_cap = 0;
    CHECK(ch.spell_duration(sp, 10) == 300);
    CHECK(ch.spell_duration(sp, 10, true) == -2);
    sp.duration_formula = 20;
    CHECK(ch.spell_duration(sp, 10) == 0);
}

TEST_CASE("TotalEffect: sums, maxima, snares, invisibility, haste") {
    World w;
    w.spell(200, {{4, 10, 100}});
    w.spell(201, {{98, 120, 100}});
    w.spell(202, {{98, 110, 100}});
    w.spell(203, {{3, 30, 100}});
    w.spell(204, {{3, -20, 100}});
    w.spell(205, {{12, 5000, 100}});
    w.spell(206, {{15, 10, 100}});
    w.spell(207, {{11, 140, 100}});
    w.spell(300, {{3, 20, 100}}); // worn
    CharacterSheet s = warrior(60);
    s.buffs[0] = buff(200);
    s.buffs[1] = buff(200);
    s.buffs[2] = buff(201);
    s.buffs[3] = buff(202);
    s.buffs[4] = buff(203);
    s.buffs[5] = buff(205);
    s.buffs[6] = buff(206);
    net::Item boots = item();
    boots.effects[net::kEffectWorn].effect = 300;
    boots.effects[net::kEffectWorn].level = 60;
    s.possessions[19] = boots;
    {
        Character ch(s, w.tables, w.lookup());
        CHECK(ch.total_effect(4) == 20);
        CHECK(ch.total_effect(98) == 120);
        CHECK(ch.total_effect(3) == 30); // the larger of buffs (30) and items (20)
        CHECK(ch.total_effect(3, false) == 30);
        CHECK(ch.total_effect(12) == 3000); // invisibility kinds: the largest, at most 3000
        CHECK(ch.total_effect(15) == 0);    // no mana effects for warriors
        CHECK(ch.total_effect(4, true, 0, true, false) == 0);
    }
    s.buffs[7] = buff(204);
    {
        Character ch(s, w.tables, w.lookup());
        CHECK(ch.total_effect(3) == -20); // a snare wins
    }
    // Haste at level 60: buff 140, item 41 (141 after CalcItemHaste): 141 + 140 - 100 = 181, cap 200.
    CharacterSheet h = warrior(60);
    h.buffs[0] = buff(207);
    net::Item gloves = item();
    gloves.haste = 41;
    h.possessions[12] = gloves;
    Character ch(h, w.tables, w.lookup());
    ch.process_all_stats();
    CHECK(ch.stats().attack_speed == 141);
    CHECK(ch.total_effect(11) == 181);
    CHECK(ch.total_effect(11, false) == 140);
}

TEST_CASE("CalcAffectChange's distance modifier (CalcSpellDistanceMod)") {
    World w;
    // Full strength up to 10, half from 60: (d - 10) x -0.01 + 1.
    SpellData& sp = w.spell(210, {{4, 100, 100}});
    sp.distance_mod_start_x = 10;
    sp.distance_mod_start_mod = 1.0f;
    sp.distance_mod_end_x = 60;
    sp.distance_mod_end_mod = 0.5f;
    sp.distance_mod_ratio = -0.01f;
    CharacterSheet s = warrior(60);
    Character ch(s, w.tables, w.lookup());
    CHECK(ch.spell_distance_mod(sp, 5.0f) == 1.0);           // clamped to the near end
    CHECK(ch.spell_distance_mod(sp, 100.0f) == doctest::Approx(0.5));
    CHECK(ch.spell_distance_mod(sp, std::nullopt) == 0.5);   // no spawn: the far modifier
    CHECK(ch.scale_by_distance(sp, 100, 35.0f) == 75);
    CHECK(ch.scale_by_distance(sp, 100, std::nullopt) == 50);
    CHECK(ch.scale_by_distance(sp, 3, std::nullopt) == 2);   // 1.5: half away from zero
    CHECK(ch.scale_by_distance(sp, -3, std::nullopt) == -2); // -1.5
    CHECK(ch.scale_by_distance(sp, -1, 1000.0f) == -1);      // -0.5 rounds to -1
    SpellData quarter = sp;
    quarter.distance_mod_end_mod = 0.25f;
    CHECK(ch.scale_by_distance(quarter, -1, std::nullopt) == -1); // -0.25 rounds to 0: a negative value stays -1
    CHECK(ch.scale_by_distance(quarter, 1, std::nullopt) == 0);
    // A negative modifier counts as 0.
    SpellData neg = sp;
    neg.distance_mod_ratio = -0.1f;
    CHECK(ch.spell_distance_mod(neg, 50.0f) == 0.0);
    // No distance range, or no spawn of the character: unchanged.
    SpellData& plain = w.spell(211, {{0, 100, 100}});
    CHECK(ch.scale_by_distance(plain, 100, std::nullopt) == 100);
    CharacterSheet nobody = warrior(60);
    nobody.has_spawn = false;
    Character ch2(nobody, w.tables, w.lookup());
    CHECK(ch2.scale_by_distance(sp, 100, std::nullopt) == 100);
    // The buff cache scales by the slot caster's distance (CacheBuffSlotEffects).
    s.buffs[0] = buff(210);
    s.buffs[0].caster_distance = 35.0f;
    {
        Character c(s, w.tables, w.lookup());
        CHECK(c.total_effect(4) == 75);
    }
    s.buffs[0].caster_distance.reset();
    {
        Character c(s, w.tables, w.lookup());
        CHECK(c.total_effect(4) == 50);
    }
}

TEST_CASE("The player's extra buff slots by level (vtable +0x2c)") {
    CHECK(affect::pc_extra_buff_slots(1) == 0);
    CHECK(affect::pc_extra_buff_slots(71) == 0);
    CHECK(affect::pc_extra_buff_slots(72) == 1);
    CHECK(affect::pc_extra_buff_slots(74) == 1);
    CHECK(affect::pc_extra_buff_slots(75) == 2);
    CHECK(affect::pc_extra_buff_slots(100) == 2);
}

TEST_CASE("Worn item effects: damage shield floor and low-level haste cap") {
    World w;
    w.spell(310, {{59, -10, 100}});
    w.spell(311, {{11, 130, 100}});
    CharacterSheet s = warrior(20);
    for (int slot : {1, 4}) {
        net::Item ring = item();
        ring.effects[net::kEffectWorn].effect = 310;
        ring.effects[net::kEffectWorn].level = 20;
        s.possessions[static_cast<std::size_t>(slot)] = ring;
    }
    net::Item belt = item();
    belt.effects[net::kEffectWorn].effect = 311;
    s.possessions[20] = belt;
    Character ch(s, w.tables, w.lookup());
    CHECK(EffectCache::get(ch.effect_cache().items, 59, 0) == -15);
    CHECK(EffectCache::get(ch.effect_cache().items, 11, 0) == 110);
    // A worn spell without a duration formula does not count.
    w.spells[310].duration_formula = 0;
    Character ch2(s, w.tables, w.lookup());
    CHECK(EffectCache::get(ch2.effect_cache().items, 59, 0) == 0);
}

TEST_CASE("Max HP, mana and endurance from the class/level table") {
    World w;
    CharacterSheet s = warrior(1);
    {
        Character ch(s, w.tables, w.lookup());
        ch.process_all_stats();
        CHECK(ch.max_hp() == 36);        // 25 + int(0.083 * 75) + 5
        CHECK(ch.max_endurance() == 20); // 15 + int(0.075 * 75)
        CHECK(ch.max_mana() == 0);
    }
    w.spell(400, {{214, 1000, 100}});
    w.spell(401, {{69, 50, 100}});
    CharacterSheet c = warrior(60);
    c.class_id = 2;
    c.base_sta = 100;
    c.base_wis = 200;
    c.base_int = 100;
    {
        Character ch(c, w.tables, w.lookup());
        ch.process_all_stats();
        CHECK(ch.max_hp() == 580);    // 500 + 0.75 * 100 + 5
        CHECK(ch.max_mana() == 5700); // 800 + 14 * (200 + 150)
    }
    net::Item helm = item();
    helm.hp = 100;
    c.possessions[2] = helm;
    c.buffs[0] = buff(400);
    c.buffs[1] = buff(401);
    Character ch(c, w.tables, w.lookup());
    ch.process_all_stats();
    CHECK(ch.max_hp() == 797);      // (500 + 100 + 75) * 1.1 + 50 + 5
    CHECK(ch.max_hp(false) == 680); // no buffs
}

TEST_CASE("Max HP and air supply: a product that wraps is divided as the original's idiv divides it") {
    World w;
    w.spell(400, {{214, 5000, 100}});
    w.spell(401, {{246, 100000000, 100}});
    CharacterSheet s = warrior(60);
    net::Item helm = item();
    helm.hp = 500000;
    s.possessions[2] = helm;
    s.buffs[0] = buff(400);
    s.buffs[1] = buff(401);
    Character ch(s, w.tables, w.lookup());
    ch.process_all_stats();
    // 400 + int(0.9 * 75) + 500000 = 500467; 5000 * 500467 wraps to -1792632296, / 10000 = -179263; + 5.
    CHECK(ch.max_hp() == 321209);
    // (75 - 30) * 100000000 wraps to 205032704, / 100.
    CHECK(ch.max_air_supply() == 2050327);
}

TEST_CASE("The sheet's stored HP is the server's current HP: Max_Endurance of a level-60 cleric") {
    // Live on the NMS server (nmscgap3, Nmscgapthree): STR 80, STA 85, DEX 75, AGI 75, WIS 110, the stock BaseData
    // row 60/2. Server and original: 1200 + 6 * ((80 + 85 + 75 + 75) / 4) = 1668. With profile +0x3394 left 0,
    // Cur_HP read 0, CalculateWeight's health slowdown (hp / max < 0.2 and hp < 50) gave 0.5, AGI 37 and 1614.
    GameTables tables;
    tables.class_levels.parse("60^2^1760^1200^1200^8^19^5.867^6^6\n");
    SpellLookup lookup = [](std::int32_t) -> const SpellData* { return nullptr; };
    net::PlayerProfile p;
    p.race = 1;
    p.class_ = 2;
    p.level = 60;
    p.str = 80;
    p.sta = 85;
    p.dex = 75;
    p.agi = 75;
    p.cha = 75;
    p.int_ = 75;
    p.wis = 110;
    Inventory inventory;
    SpellState spells;
    LiveState live;
    live.level = 60;
    live.hp = live.max_hp = 2263;
    CharacterSheet s = build_sheet(p, inventory, spells, live);
    CHECK(s.stored_hp == 2263);
    Character ch(s, tables, lookup);
    ch.process_all_stats();
    CHECK(ch.cur_hp() == 2263);
    CHECK(ch.max_hp() == 2263);           // 1760 + int(5.867 * 85) + 5
    CHECK(ch.stats().stats.agi == 75);
    CHECK(ch.max_endurance() == 1668);
    CHECK(ch.max_mana() == 1950);         // 1200 + 6 * (110 + (330 - 300) / 2)
    live.hp = 0;                          // dead or not yet known: the slowdown halves AGI, as in the original
    CharacterSheet dead = build_sheet(p, inventory, spells, live);
    Character ch0(dead, tables, lookup);
    ch0.process_all_stats();
    CHECK(ch0.stats().stats.agi == 37);
    CHECK(ch0.max_endurance() == 1614);
}

TEST_CASE("build_sheet: the profile's fist values, the buff timer hold and the buff casters' distances") {
    net::PlayerProfile p;
    p.race = 1;
    p.class_ = 1;
    p.level = 60;
    p.profile_3464 = {5, 0, 12, 2400};
    Inventory inventory;
    SpellState spells;
    spells.buff_timers_on_hold = true;
    spells.buffs[0].spell = 300;
    spells.buffs[0].caster_id = 7;
    spells.buffs[1].spell = 301;
    spells.buffs[1].caster_id = 99; // not in the zone
    WorldState w;
    w.player_id = 1;
    w.spawns[1].id = 1;
    w.spawns[1].position = {10, 20, 0};
    w.spawns[7].id = 7;
    w.spawns[7].position = {13, 24, 50}; // the height is left out
    LiveState live;
    live.level = 60;
    live.player_id = 1;
    CharacterSheet s = build_sheet(p, inventory, spells, live, nullptr, &w);
    CHECK(s.base_ac == 5);
    CHECK(s.fist_damage == 12);
    CHECK(s.fist_delay == 2400);
    CHECK(s.buff_timers_on_hold);
    REQUIRE(s.buffs[0].caster_distance);
    CHECK(*s.buffs[0].caster_distance == 5.0f);
    CHECK_FALSE(s.buffs[1].caster_distance);
}

TEST_CASE("build_sheet: current intoxication replaces the received profile, including fully sober") {
    net::PlayerProfile profile;
    profile.intoxication = 80;
    Inventory inventory;
    SpellState spells;
    LiveState live;
    CHECK(build_sheet(profile, inventory, spells, live).drunkenness == 80);
    live.drunkenness = 120;
    CHECK(build_sheet(profile, inventory, spells, live).drunkenness == 120);
    live.drunkenness = 0;
    CHECK(build_sheet(profile, inventory, spells, live).drunkenness == 0);
    CHECK(profile.intoxication == 80); // The received snapshot is independent from changing local state.
}

TEST_CASE("build_sheet: received personal and guild tribute items contribute until replaced or removed") {
    World world;
    world.spell(700, {{3, 5, 100}});
    net::PlayerProfile profile;
    profile.race = 1;
    profile.class_ = 1;
    profile.level = 60;
    profile.str = profile.sta = profile.cha = profile.dex = profile.int_ = profile.agi = profile.wis = 75;
    Inventory inventory;
    SpellState spells;
    LiveState live;
    live.level = 60;
    live.hp = live.max_hp = 100000;

    // Stock RoF2 item packets: ItemPacketTributeItem (108), type 6 / slots 0..4; ItemPacketGuildTribute (109),
    // type 8 / slots 0..1. Exercise their serialized item headers rather than filling the sheet directly.
    auto receive = [&](std::int16_t type, std::int16_t slot, std::int8_t strength, bool worn_effect = false) {
        net::Item received = item();
        received.id = static_cast<std::uint32_t>(1000 + type * 10 + slot);
        received.slot = {type, slot, -1, -1};
        received.astr = strength;
        received.stack_count = 7; // A received tribute item contributes once, independently of its stack count.
        received.max_stack = 20;
        if (worn_effect) {
            received.effects[net::kEffectWorn].effect = 700;
            received.effects[net::kEffectWorn].level = 60;
        }
        net::ItemPacket packet;
        packet.type = type == 8 ? 109u : 108u;
        packet.item = received;
        auto decoded = net::decode_item_packet(net::encode_item_packet(false, packet));
        REQUIRE(decoded);
        CHECK(decoded->item.slot.type == type);
        CHECK(decoded->item.slot.slot == slot);
        const net::ItemSlot at = decoded->item.slot;
        REQUIRE(inventory.put(at, std::move(decoded->item)));
    };
    for (std::int16_t slot = 0; slot < 5; ++slot)
        receive(6, slot, static_cast<std::int8_t>(slot + 1));
    receive(8, 0, 10);
    receive(8, 1, 20, true);
    // Unused trophy containers and out-of-range tribute slots cannot become bonus sources.
    receive(7, 0, 100);
    receive(22, 0, 100);
    receive(6, 5, 100);
    receive(8, 2, 100);

    CharacterSheet sheet = build_sheet(profile, inventory, spells, live);
    auto strength = [&] {
        Character character(sheet, world.tables, world.lookup());
        character.process_all_stats();
        return character.stats().stats.str;
    };
    auto speed_effect = [&] {
        Character character(sheet, world.tables, world.lookup());
        return character.total_effect(3);
    };
    for (std::size_t slot = 0; slot < 5; ++slot) {
        REQUIRE(sheet.extra_items[slot]);
        CHECK(sheet.extra_items[slot]->slot.type == 6);
        CHECK(sheet.extra_items[slot]->slot.slot == static_cast<std::int16_t>(slot));
    }
    REQUIRE(sheet.extra_items[15]);
    REQUIRE(sheet.extra_items[16]);
    CHECK(sheet.extra_items[15]->slot.type == 8);
    CHECK(sheet.extra_items[16]->slot.slot == 1);
    for (std::size_t slot = 5; slot < sheet.extra_items.size(); ++slot)
        if (slot != 15 && slot != 16) CHECK_FALSE(sheet.extra_items[slot]);
    CHECK(strength() == 120); // Base 75 + personal 1+2+3+4+5 + guild 10+20, without multiplying by seven.
    CHECK(speed_effect() == 5);

    // A server removal names the same type/slot in OP_MoveItem; rebuilding clears its previous contribution.
    const net::ItemSlot last_personal{6, 4, -1, -1};
    auto removal = net::decode_move_item(net::encode(net::MoveItem{last_personal, net::ItemSlot::invalid(), 0}));
    REQUIRE(removal);
    REQUIRE(inventory.take(removal->from));
    build_sheet_into(sheet, profile, inventory, spells, live, {});
    CHECK_FALSE(sheet.extra_items[4]);
    CHECK(strength() == 115);

    receive(8, 1, 7); // A tier/benefit replacement also replaces the previous worn effect.
    build_sheet_into(sheet, profile, inventory, spells, live, {});
    CHECK(strength() == 102);
    CHECK(speed_effect() == 0);

    inventory.clear(); // A zone/disconnect inventory reset cannot leave tribute bonuses in a reused sheet.
    build_sheet_into(sheet, profile, inventory, spells, live, {});
    for (const auto& extra : sheet.extra_items) CHECK_FALSE(extra);
    CHECK(strength() == 75);
}

TEST_CASE("build_sheet_into: a sheet built again in place is the sheet build_sheet makes") {
    // The per-frame character (StatsUi, GameConnection::with_character) is rebuilt into the last frame's sheet so
    // its items reuse their storage; nothing of the last state may stay.
    net::PlayerProfile p;
    p.race = 1;
    p.class_ = 1;
    p.level = 60;
    p.str = 90;
    auto possession = [](int slot) { return net::ItemSlot::possessions(static_cast<std::int16_t>(slot)); };
    Inventory before;
    net::Item helm;
    helm.name = "A helm with a name long enough to live on the heap";
    helm.ac = 10;
    helm.equip_slots = 1u << 2;
    REQUIRE(before.put(possession(2), helm));
    net::Item bag;
    bag.name = "Large Sewn Sack";
    bag.item_class = 1;
    bag.bag_slots = 10;
    REQUIRE(before.put(possession(24), bag));
    net::Item gem;
    gem.name = "Flawed Emerald";
    REQUIRE(before.put({net::kInvPossessions, 24, 3, -1, 0, 0}, gem));
    net::Item ring;
    ring.name = "Ring";
    REQUIRE(before.put(possession(15), ring));
    SpellState spells_before;
    spells_before.buffs[0].spell = 300;
    spells_before.buffs[3].spell = 301;
    spells_before.buff_timers_on_hold = true;
    LiveState live_before;
    live_before.level = 60;
    live_before.hp = 500;
    live_before.sitting = true;

    CharacterSheet reused;
    build_sheet_into(reused, p, before, spells_before, live_before, [](std::vector<SheetAbility>& out) {
        out.resize(2);
        out[0].id = 1;
        out[0].effects = {{326, 1, 0}};
        out[1].id = 2;
    });
    reused.prestige_limit = 7;
    reused.extra_items[0] = ring;

    // Now: the helm changed, the sack holds something else, the ring is gone, one buff less, standing.
    Inventory now;
    helm.name = "A shorter helm";
    helm.ac = 4;
    REQUIRE(now.put(possession(2), helm));
    REQUIRE(now.put(possession(24), bag));
    gem.name = "Ruby";
    REQUIRE(now.put({net::kInvPossessions, 24, 5, -1, 0, 0}, gem));
    SpellState spells_now;
    spells_now.buffs[0].spell = 302;
    LiveState live_now;
    live_now.level = 61;
    live_now.hp = 700;
    build_sheet_into(reused, p, now, spells_now, live_now, {}); // no abilities now: the last list does not stay
    CharacterSheet fresh = build_sheet(p, now, spells_now, live_now);

    CHECK(reused.level == fresh.level);
    CHECK(reused.stored_hp == fresh.stored_hp);
    CHECK(reused.sitting == fresh.sitting);
    CHECK(reused.buff_timers_on_hold == fresh.buff_timers_on_hold);
    CHECK(reused.prestige_limit == fresh.prestige_limit);
    CHECK_FALSE(reused.extra_items[0]);
    for (int slot = 0; slot < kPossessionSlots; ++slot) {
        CAPTURE(slot);
        const net::Item* a = reused.item(slot);
        const net::Item* b = fresh.item(slot);
        REQUIRE((a == nullptr) == (b == nullptr));
        if (!a) continue;
        CHECK(a->name == b->name);
        CHECK(a->ac == b->ac);
        REQUIRE(a->contents.size() == b->contents.size());
        for (std::size_t i = 0; i < a->contents.size(); ++i) {
            CHECK(a->contents[i].index == b->contents[i].index);
            CHECK(a->contents[i].item.name == b->contents[i].item.name);
        }
    }
    REQUIRE(reused.buffs.size() == fresh.buffs.size());
    for (std::size_t i = 0; i < reused.buffs.size(); ++i) {
        CHECK(reused.buffs[i].spell_id == fresh.buffs[i].spell_id);
        CHECK(reused.buffs[i].type == fresh.buffs[i].type);
    }
    CHECK(reused.abilities.empty());
    CHECK(fresh.abilities.empty());
    // An ability list set again into the last one: the new list only.
    build_sheet_into(reused, p, now, spells_now, live_now, [](std::vector<SheetAbility>& out) {
        REQUIRE(out.empty());
        out.resize(1);
        out[0].id = 3;
    });
    REQUIRE(reused.abilities.size() == 1);
    CHECK(reused.abilities[0].id == 3);
}

TEST_CASE("A server extension's vitals answer Max / Cur HP, mana and endurance (the NMS patch's detours)") {
    // The level-60 cleric of the stored-HP case. Stock: no CharacterVitals, the formulas.
    GameTables tables;
    tables.class_levels.parse("60^2^1760^1200^1200^8^19^5.867^6^6\n");
    SpellLookup lookup = [](std::int32_t) -> const SpellData* { return nullptr; };
    net::PlayerProfile p;
    p.race = 1;
    p.class_ = 2;
    p.level = 60;
    p.str = 80;
    p.sta = 85;
    p.dex = 75;
    p.agi = 75;
    p.cha = 75;
    p.int_ = 75;
    p.wis = 110;
    Inventory inventory;
    SpellState spells;
    LiveState live;
    live.level = 60;
    live.hp = 0; // not known yet: the stock slowdown halves AGI
    live.mana = 5000;
    live.endurance = 900;
    {
        CharacterSheet s = build_sheet(p, inventory, spells, live);
        CHECK_FALSE(s.vitals.any());
        Character ch(s, tables, lookup);
        ch.process_all_stats();
        CHECK(ch.max_hp() == 2263);
        CHECK(ch.max_mana() == 1950);
        CHECK(ch.cur_mana() == 1950); // clamped to Max_Mana
        CHECK(ch.cur_endurance() == 900);
        CHECK(ch.stats().stats.agi == 37);
    }
    // The extension answers HP only: Cur_HP / Max_HP everywhere, CalculateWeight's slowdown included (AGI 75 and the
    // endurance formula with it); mana and endurance keep the formulas.
    live.vitals.cur_hp = 2500;
    live.vitals.max_hp = 2775;
    {
        CharacterSheet s = build_sheet(p, inventory, spells, live);
        Character ch(s, tables, lookup);
        ch.process_all_stats();
        CHECK(ch.cur_hp() == 2500);
        CHECK(ch.cur_hp(false, 1) == 2500);
        CHECK(ch.max_hp(false) == 2775); // whatever the arguments
        CHECK(ch.stats().stats.agi == 75);
        CHECK(ch.max_endurance() == 1668);
        CHECK(ch.max_mana() == 1950);
        CHECK(ch.spawn_hp_max() == s.spawn_max_hp); // nothing stored
    }
    // All six, values the formulas would clamp: returned as they are.
    live.vitals.max_mana = 4000;
    live.vitals.cur_mana = 4500;
    live.vitals.max_endurance = 1700;
    live.vitals.cur_endurance = 0;
    {
        CharacterSheet s = build_sheet(p, inventory, spells, live);
        Character ch(s, tables, lookup);
        ch.process_all_stats();
        std::int32_t stored = -1;
        CHECK(ch.cur_mana(true, &stored) == 4500);
        CHECK(stored == 5000);
        CHECK(ch.max_mana(false) == 4000);
        CHECK(ch.max_endurance() == 1700);
        CHECK(ch.cur_endurance() == 0);
        // A test hook still comes first (it stands for the whole callee).
        ch.hooks.max_mana = [](bool) -> std::optional<std::int32_t> { return 7; };
        CHECK(ch.max_mana() == 7);
    }
}

TEST_CASE("Attributes, caps, base saves and the drunk INT/WIS scaling at both precisions") {
    World w;
    w.spell(500, {{4, 5, 100}});
    w.spell(501, {{262, 10, 100}});
    CharacterSheet s = warrior(70);
    net::Item ring = item();
    ring.astr = 10;
    s.possessions[15] = ring;
    s.buffs[0] = buff(500);
    s.buffs[1] = buff(501);
    {
        Character ch(s, w.tables, w.lookup());
        ch.process_all_stats();
        CHECK(ch.stats().stats.str == 90);
        CHECK(ch.stats().stats_no_buffs.str == 85);
        CHECK(ch.stat_cap(0) == 315); // 5 * 70 - 45 + SPA 262 (subindex 0) 10
        CHECK(ch.stat_cap(1) == 305);
        CHECK(ch.stat_cap(7) == 525); // 5 * 70 + 175
    }
    CharacterSheet e = warrior(50);
    e.race = 3; // Erudite: magic 30, then warriors + level / 2
    e.base_save_magic = 25;
    Character ce(e, w.tables, w.lookup());
    CHECK(ce.basesave_magic() == 55);
    CHECK(ce.calc_save_physical(true) == 16); // level / 3

    // INT 150, drunkenness 64: 150 * (1 - 64 * 0.005). At 24 bits the factor rounds to 0.68000001f and the
    // product to 102.0; with doubles it is 101.99999999999999, truncated to 101.
    CharacterSheet d = warrior(60);
    d.base_int = 150;
    d.drunkenness = 64;
    d.fpu = FpuPrecision::Single24;
    Character d24(d, w.tables, w.lookup());
    d24.process_all_stats();
    CHECK(d24.stats().stats.int_ == 102);
    d.fpu = FpuPrecision::Double53;
    Character d53(d, w.tables, w.lookup());
    d53.process_all_stats();
    CHECK(d53.stats().stats.int_ == 101);
}

TEST_CASE("Armor class, defense, item haste and carried weight") {
    World w;
    CharacterSheet s = warrior(60);
    s.skills[15] = 200;
    net::Item helm = item();
    helm.ac = 30;
    s.possessions[2] = helm;
    Character ch(s, w.tables, w.lookup());
    ch.process_all_stats();
    // 30 * 4 / 3 = 40, + defense 200 / 3 = 66, + AGI 75 / 20 = 3
    CHECK(ch.stats().ac == 109);
    CHECK(ch.compute_defense() == 362); // 200 * 400 / 225 + (75 - 40) * 8000 / 36000

    CharacterSheet h = warrior(30);
    net::Item gloves = item();
    gloves.haste = 41;
    h.possessions[12] = gloves;
    CHECK(Character(h, w.tables, w.lookup()).calc_item_haste(0) == 141);
    h.level = 20;
    CHECK(Character(h, w.tables, w.lookup()).calc_item_haste(0) == 110);

    CharacterSheet b = warrior(60);
    net::Item a = item();
    a.weight = 50;
    b.possessions[23] = a;
    net::Item bag;
    bag.item_class = 1;
    bag.weight = 20;
    bag.weight_reduction = 50;
    bag.bag_slots = 10;
    net::BagItem inside;
    inside.index = 0;
    inside.item = item();
    inside.item.weight = 100;
    bag.contents.push_back(inside);
    b.possessions[24] = bag;
    CHECK(Character(b, w.tables, w.lookup()).calc_carried_weight() == 12); // 5 + 2 + 50% of 10
}

TEST_CASE("ItemBase getters: weight, size, value, potion slots, damage bonus") {
    // GetWeight(1, 0, 1) recurses only when the definition is null: the own weight, charm-scaled, nothing else.
    net::Item sword = item();
    sword.weight = 80;
    sword.size = 2;
    sword.value = 1000;
    sword.purity = 50;
    sword.aug_sockets[0].unknown = 1; // socket 0 scales by the aug stat percent
    sword.extra_dmg_skill = 10;
    sword.extra_dmg_amt = 6;
    net::BagItem aug;
    aug.index = 0;
    aug.item = item();
    aug.item.weight = 30;
    aug.item.size = 4;
    aug.item.value = 250;
    aug.item.extra_dmg_skill = 10;
    aug.item.extra_dmg_amt = 10;
    aug.item.potion_belt_slots = 2;
    sword.contents.push_back(aug);
    CHECK(item_weight(sword) == 80);           // the augment's 30 is not added
    CHECK(item_size(sword) == 4);              // the largest size inside a Type 0 item
    CHECK(item_size(sword, false) == 2);
    CHECK(item_value(sword) == 1250);          // Cost plus the contents' GetItemValue
    CHECK(item_value(sword, false) == 1000);
    CHECK(item_dmg_bonus(sword, 10) == 6 + 5); // the augment's 10 at 50%
    CHECK(item_dmg_bonus(sword, 8) == 0);
    CHECK(item_potion_slots(sword) == 1);      // 2 at 50%, the enabled byte not read
    CHECK(item_potion_slots(sword, false) == 0);

    net::Item arrows = item();
    arrows.weight = 1;
    arrows.max_stack = 100;
    arrows.stack_count = 80;
    CHECK(item_weight(arrows) == 1); // +0x98 (the count) is never read

    net::Item charm = item();
    charm.weight = 50;
    charm.charm_file_id = 7;
    charm.scaled_value = static_cast<std::uint32_t>(-40);
    CHECK(item_weight(charm) == 20); // |ScriptIndex| percent

    net::Item bag;
    bag.item_class = 1;
    bag.weight = 25;
    bag.size = 1;
    bag.value = 10;
    net::BagItem inside;
    inside.item = item();
    inside.item.weight = 100;
    inside.item.size = 3;
    inside.item.value = 99;
    bag.contents.push_back(inside);
    CHECK(item_weight(bag) == 25); // the contents are CalcCarriedWeight's
    CHECK(item_size(bag) == 1);    // only Type 0 items look inside
    CHECK(item_value(bag) == 10);
}

TEST_CASE("Food and drink in the packs add their stats (AddPackItemStats)") {
    World w;
    CharacterSheet s = warrior(60);
    net::Item bread = item();
    bread.item_type = 14;
    bread.astr = 5;
    bread.hp = 20;
    bread.ac = 3;
    s.possessions[23] = bread;
    Character ch(s, w.tables, w.lookup());
    ch.process_all_stats();
    CHECK(ch.stats().stats.str == 80);
    CHECK(ch.stats().stats_no_buffs.str == 75);
    CHECK(ch.stats().hp_bonus == 20);
    CHECK(ch.stats().hp_bonus_no_buffs == 0);
    CHECK(ch.stats().ac == 4 + 3); // (3 * 4 / 3) + AGI 75 / 20
}

TEST_CASE("Regeneration") {
    World w;
    CharacterSheet s = warrior(1);
    {
        Character ch(s, w.tables, w.lookup());
        ch.process_all_stats();
        CHECK(ch.hp_regen() == 2);
        CHECK(ch.endurance_regen() == 7); // 5 + 5 / 2 when not attacking
    }
    s.sitting = true;
    s.innate_3258 = 0; // regeneration (trolls, iksar): the level bonus doubles
    {
        Character ch(s, w.tables, w.lookup());
        ch.process_all_stats();
        CHECK(ch.hp_regen() == 4);
    }
    CharacterSheet c = warrior(60);
    c.class_id = 2;
    c.skills[31] = 100;
    c.sitting = true;
    c.spawn_sitting = true;
    Character ch(c, w.tables, w.lookup());
    ch.process_all_stats();
    CHECK(ch.adjusted_skill(31) == 100);
    CHECK(ch.mana_regen() == 8); // sitting 0 + 1 + 1 + 100 / 15
}

TEST_CASE("Skill caps and adjusted skills") {
    World w;
    CharacterSheet s = warrior(60);
    s.skills[15] = 250;
    Character ch(s, w.tables, w.lookup());
    CHECK(ch.skill_cap(60, 1, 15) == 210);
    CHECK(ch.skill_with_item_mods(15, true) == 210); // the raw 250 held at the cap
    CHECK(ch.adjusted_skill(15) == 250);
    CHECK(ch.adjusted_skill(57) == 0); // tinkering: gnomes only
    s.race = 12;
    CHECK(Character(s, w.tables, w.lookup()).racial_skill_bonus(57) == 50);
}

TEST_CASE("Cast time: hybrid quick cast at both precisions, SPA 127 foci") {
    World w;
    SpellData& nuke = w.spell(600, {{0, -100, 100}});
    nuke.cast_time = 3000;
    nuke.spell_type = 0;
    nuke.class_level[1] = 50;
    nuke.class_level[2] = 50;
    CharacterSheet p = warrior(60);
    p.class_id = 3;
    // 3000 * (100 - 3 * 10) * 0.01f: 2100.0 at 24 bits, 2099.99995 (truncated to 2099) with doubles.
    p.fpu = FpuPrecision::Single24;
    CHECK(Character(p, w.tables, w.lookup()).casting_time_modifier(nuke) == -900);
    p.fpu = FpuPrecision::Double53;
    CHECK(Character(p, w.tables, w.lookup()).casting_time_modifier(nuke) == -901);

    SpellData& focus = w.spell(500, {{127, 10, 100}, {0x86, 70, 100}});
    w.spell(501, {{127, 20, 100}});
    CharacterSheet c = warrior(60);
    c.class_id = 2;
    net::Item ear = item();
    ear.effects[net::kEffectFocus].effect = 500;
    c.possessions[1] = ear;
    {
        Character ch(c, w.tables, w.lookup());
        CHECK(ch.focus_casting_time_modifier(nuke) == -300);
    }
    c.buffs[0] = buff(501);
    {
        Character ch(c, w.tables, w.lookup());
        CHECK(ch.focus_casting_time_modifier(nuke) == -900);
    }
    focus.base[1] = 40; // max level 40 < 50, no falloff: the item focus no longer applies
    {
        Character ch(c, w.tables, w.lookup());
        CHECK(ch.focus_casting_time_modifier(nuke) == -600);
    }
    c.class_id = 1; // warriors get no spell foci
    CHECK(Character(c, w.tables, w.lookup()).focus_casting_time_modifier(nuke) == 0);
    // ... unless a server extension tests focus over the player's classes (Triune's focus_classes): no class filter.
    c.focus_classes = 1u << 0;
    CHECK(Character(c, w.tables, w.lookup()).focus_casting_time_modifier(nuke) == -600);
}

TEST_CASE("Cast time: the hybrid quick cast's limits and the 50% floor; the Spell Display shows the foci") {
    World w;
    SpellData& nuke = w.spell(600, {{0, -100, 100}});
    nuke.cast_time = 4000;
    nuke.spell_type = 0; // detrimental
    nuke.class_level.fill(255);
    nuke.class_level[1] = 50;
    nuke.class_level[4] = 50;
    auto at = [&](std::uint8_t cls, int level) {
        CharacterSheet p = warrior(level);
        p.class_id = cls;
        p.fpu = FpuPrecision::Single24;
        return cast_time_ms(Character(p, w.tables, w.lookup()), nuke);
    };
    CHECK(at(2, 60) == 4000); // cleric: no modifier
    CHECK(at(5, 50) == 4000); // level 50: none
    CHECK(at(5, 55) == 3399); // 100 - 3 x 5 = 85%, times 0.01f, truncated: 3399.99, not the 3400 a rounding gives
    CHECK(at(3, 70) == 2200); // capped at 15 levels: 55%
    nuke.spell_type = 1;
    CHECK(at(3, 70) == 4000); // beneficial: none
    nuke.spell_type = 0;
    nuke.cast_time = 2999;
    CHECK(at(3, 70) == 2999); // under 3 s: none

    // A 75% SPA 127 focus takes a cleric's 4 s nuke to the 2 s floor; the Spell Display shows that, as the cast does.
    nuke.cast_time = 4000;
    w.spell(500, {{127, 75, 100}});
    CharacterSheet c = warrior(60);
    c.class_id = 2;
    net::Item ear = item();
    ear.effects[net::kEffectFocus].effect = 500;
    c.possessions[1] = ear;
    Character ch(c, w.tables, w.lookup());
    CHECK(cast_time_ms(ch, nuke) == 2000);
    EqStrings strings;
    strings.set(0xc22, "Cast: %1 %2");
    strings.set(0xc23, "Cast: %1");
    SpellDisplayContext ctx;
    ctx.strings = &strings;
    ctx.character = &ch;
    ctx.player_class = 2;
    ctx.player_level = 60;
    CHECK(build_spell_display(nuke, ctx).description.find("Cast: 4.0 (2.0 sec)<BR>") != std::string::npos);
    ctx.character = nullptr; // no character yet: the spell's own time
    CHECK(build_spell_display(nuke, ctx).description.find("Cast: 4.0<BR>") != std::string::npos);
}

TEST_CASE("Spell Display: \"(x sec)\" when a cast time modifier is non-zero, even when they cancel out") {
    // CSpellDisplayWnd::SetSpell @0x75f3c6..0x75f428: GetCastingTimeModifier != 0 || GetFocusCastingTimeModifier != 0.
    World w;
    SpellData& nuke = w.spell(600, {{0, -100, 100}});
    nuke.cast_time = 4000;
    nuke.spell_type = 0; // detrimental: the hybrid quick cast applies
    nuke.class_level.fill(255);
    nuke.class_level[4] = 50;
    CharacterSheet c = warrior(55);
    c.class_id = 5; // a hybrid at 55: 85% (-601 ms)
    c.fpu = FpuPrecision::Single24;
    EqStrings strings;
    strings.set(0xc22, "Cast: %1 %2");
    strings.set(0xc23, "Cast: %1");
    SpellDisplayContext ctx;
    ctx.strings = &strings;
    ctx.player_class = 5;
    ctx.player_level = 55;
    {
        Character ch(c, w.tables, w.lookup());
        REQUIRE(ch.casting_time_modifier(nuke) == -601);
        ctx.character = &ch;
        CHECK(build_spell_display(nuke, ctx).description.find("Cast: 4.0 (3.4 sec)<BR>") != std::string::npos);
    }
    // A worn +15% focus (SPA 127 base -15) offsets it: the time is 3999 ms, still shown as adjusted.
    w.spell(500, {{127, -15, 100}});
    net::Item ear = item();
    ear.effects[net::kEffectFocus].effect = 500;
    c.possessions[1] = ear;
    Character ch(c, w.tables, w.lookup());
    CHECK(ch.focus_casting_time_modifier(nuke) == 600);
    CHECK(cast_time_ms(ch, nuke) == 3999);
    ctx.character = &ch;
    CHECK(build_spell_display(nuke, ctx).description.find("Cast: 4.0 (4.0 sec)<BR>") != std::string::npos);
    // Nothing modifies a cleric's cast: the plain line.
    c.class_id = 2;
    c.possessions[1] = {};
    nuke.class_level[1] = 50;
    Character cleric(c, w.tables, w.lookup());
    ctx.character = &cleric;
    ctx.player_class = 2;
    CHECK(build_spell_display(nuke, ctx).description.find("Cast: 4.0<BR>") != std::string::npos);
}

TEST_CASE("Focus over a server extension's classes: SPA 411 and an AA's classes") {
    World w;
    SpellData& nuke = w.spell(600, {{0, -100, 100}});
    nuke.cast_time = 3000;
    nuke.class_level[1] = 50;
    nuke.class_level[11] = 50;
    // A focus for Wizards only (SPA 411's base: bit 1 << class).
    const SpellData& wiz_focus = w.spell(502, {{127, 30, 100}, {0x19b, 1 << 12, 0}});
    CharacterSheet c = warrior(60);
    c.class_id = 2; // a Cleric
    CHECK(Character(c, w.tables, w.lookup()).spell_focus_pct(nuke, wiz_focus, false) == 0);
    c.focus_classes = (1u << 1) | (1u << 11); // Cleric / Wizard
    CHECK(Character(c, w.tables, w.lookup()).spell_focus_pct(nuke, wiz_focus, false) == 100);

    // An AA focus for Wizards only counts for a Cleric / Wizard.
    CharacterSheet a = warrior(60);
    a.class_id = 2;
    SheetAbility aa;
    aa.class_mask = 1u << 12;
    aa.effects = {{127, 25, 0}};
    a.abilities.push_back(aa);
    CHECK_FALSE(Character(a, w.tables, w.lookup()).best_aa_focus(nuke, 127).found);
    a.focus_classes = (1u << 1) | (1u << 11);
    CHECK(Character(a, w.tables, w.lookup()).best_aa_focus(nuke, 127).found);
}

TEST_CASE("The installed BaseData.txt and SkillCaps.txt load") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    GameTables t;
    REQUIRE(t.load(*game));
    REQUIRE(t.class_levels.row(1, 1));
    CHECK(t.class_levels.row(1, 1)->base_hp > 0.0);
    CHECK(t.skill_caps.cap(1, 15, 60) > 0);
}
