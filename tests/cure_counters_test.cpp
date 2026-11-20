// A cure's counters against a buff's slot data (game/hit_landing.cpp -> HitBySpell's counter case, SPA 116).
//
// HitBySpell spends the cure against the first matching buff's counter total; a surplus is not carried to
// another buff, as in RoF2. The landing path hands the buff's EQ_Affect to it, so the slot data must go both
// ways: counters dropped on the way in look empty, and a cure then spends that debuff's counters. The client
// never removes the buff itself: EQEmu's OP_Buff fade=1 is authoritative for that.
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "game/buff_overlays.h"
#include "game/hit_landing.h"
#include "game/spells.h"
#include "game/world_state.h"

using namespace hailstone::game;

namespace {

// A 237-column spell line: the base, max and effect slots for k=0..11 are at 20+k, 44+k and 86+k (game/spells.cpp).
std::string spell_line(std::initializer_list<std::pair<int, std::string>> set) {
    std::vector<std::string> cols(237, "0");
    for (int i = 1; i <= 8; ++i) cols[static_cast<std::size_t>(i)].clear();
    for (int i = 86; i <= 97; ++i) cols[static_cast<std::size_t>(i)] = "254";
    for (const auto& [c, v] : set) cols[static_cast<std::size_t>(c)] = v;
    std::string line;
    for (std::size_t i = 0; i < cols.size(); ++i) line += (i ? "^" : "") + cols[i];
    return line;
}

// The player (id 1) with the curses in its buff slots and the casting NPC (id 2).
struct Landing {
    SpellTable table;
    WorldState world;
    SpellState state;
    HitLandingContext ctx;

    Landing(const std::string& spells, std::size_t spell_count = 2) {
        REQUIRE(table.parse(spells) == spell_count);
        world.player_id = 1;
        world.spawns[1].id = 1;
        world.spawns[1].level = 10;
        world.spawns[2].id = 2;
        world.spawns[2].is_npc = true;
        ctx.spells = &table;
        ctx.world = &world;
        ctx.spell_state = &state;
        ctx.level = 10;
    }

    HitLandingResult cure(std::uint32_t spell_id) {
        return land_spell(hit_record(2, 0, 1.0f, 0.0f, 0, 0, spell_id, 10), ctx);
    }
};

} // namespace

TEST_CASE("a cure weaker than the counters leaves the curse and its counters") {
    Landing l(spell_line({{0, "1238"}, {1, "Curse of Flames"}, {16, "3"}, {20, "12"}, {44, "12"}, {86, "116"}}) + "\n" +
              spell_line({{0, "1240"}, {1, "Weak Cure"}, {20, "-3"}, {83, "1"}, {86, "116"}}));
    BuffSlot& curse = l.state.buffs[0];
    curse.spell = 1238;
    curse.effect_type = 2;
    curse.level = 10;
    curse.tics = 600;
    curse.caster_id = 2;
    curse.slot_data[0] = 12;
    curse.counters_known = true;

    auto r = l.cure(1240);

    CHECK(r.value == 1);
    CHECK(r.removed_buffs.empty());
    CHECK(l.state.buffs[0].spell == 1238); // the debuff stays
    CHECK(l.state.buffs[0].slot_data[0] == 9); // and its counters went down by the cure's 3
}

TEST_CASE("a full cure zeroes the counters but leaves the buff for the server") {
    Landing l(spell_line({{0, "1238"}, {1, "Curse of Flames"}, {16, "3"}, {20, "12"}, {44, "12"}, {86, "116"}}) + "\n" +
              spell_line({{0, "1241"}, {1, "Full Cure"}, {20, "-12"}, {83, "1"}, {86, "116"}}));
    BuffSlot& curse = l.state.buffs[0];
    curse.spell = 1238;
    curse.effect_type = 2;
    curse.level = 10;
    curse.tics = 600;
    curse.caster_id = 2;
    curse.slot_data[0] = 12;
    curse.counters_known = true;

    auto r = l.cure(1241);

    CHECK(r.value == 1);
    CHECK(r.removed_buffs.empty()); // the client never clears the slot; the server's fade=1 does
    CHECK(l.state.buffs[0].spell == 1238);
    CHECK(l.state.buffs[0].slot_data[0] == 0);
}

// The reported bug: a cure that looks complete against the client's local counters must not remove the buff
// itself. The server may hold more counters and keep the debuff without sending a fade.
TEST_CASE("an over-cure leaves the buff standing with zeroed counters") {
    Landing l(spell_line({{0, "1238"}, {1, "Curse of Flames"}, {16, "3"}, {20, "12"}, {44, "12"}, {86, "116"}}) + "\n" +
              spell_line({{0, "1243"}, {1, "Overshoot Cure"}, {20, "-13"}, {83, "1"}, {86, "116"}}));
    BuffSlot& curse = l.state.buffs[0];
    curse.spell = 1238;
    curse.effect_type = 2;
    curse.level = 10;
    curse.tics = 600;
    curse.caster_id = 2;
    curse.slot_data[0] = 12;
    curse.counters_known = true;

    auto r = l.cure(1243);

    CHECK(r.value == 1);
    CHECK(r.removed_buffs.empty()); // overshooting (< 1 left) still leaves the removal to the server
    CHECK(l.state.buffs[0].spell == 1238);
    CHECK(l.state.buffs[0].slot_data[0] == 0);
}

// Curse of Flames regression: six effects of 9 counters are one 54-counter curse, not six separate curses.
TEST_CASE("a cure spends its counters over every effect of the curse") {
    Landing l(spell_line({{0, "1238"}, {1, "Curse of Flames"}, {16, "3"}, {20, "9"}, {21, "9"}, {22, "9"}, {23, "9"},
                          {24, "9"}, {25, "9"}, {44, "9"}, {45, "9"}, {46, "9"}, {47, "9"}, {48, "9"}, {49, "9"},
                          {86, "116"}, {87, "116"}, {88, "116"}, {89, "116"}, {90, "116"}, {91, "116"}}) + "\n" +
              spell_line({{0, "1242"}, {1, "Curse Cure"}, {20, "-9"}, {83, "1"}, {86, "116"}}));
    BuffSlot& curse = l.state.buffs[0];
    curse.spell = 1238;
    curse.effect_type = 2;
    curse.level = 10;
    curse.tics = 600;
    curse.caster_id = 2;
    curse.slot_data = {9, 9, 9, 9, 9, 9};
    curse.counters_known = true;

    for (std::int32_t remaining = 45; remaining > 0; remaining -= 9) {
        auto r = l.cure(1242);
        CHECK(r.removed_buffs.empty());
        CHECK(curse.spell == 1238);
        std::int32_t total = 0;
        for (std::int32_t v : curse.slot_data) total += v;
        CHECK(total == remaining);
    }
    auto last = l.cure(1242);
    CHECK(last.removed_buffs.empty());
    CHECK(curse.spell == 1238); // the curse stays until the server fades it
    for (std::int32_t v : curse.slot_data) CHECK(v == 0);
}

TEST_CASE("a cure spends matching effect slots before other counter SPAs") {
    Landing l(spell_line({{0, "1239"}, {16, "3"}, {20, "3"}, {21, "7"}, {22, "9"},
                          {86, "116"}, {87, "36"}, {88, "116"}}) + "\n" +
              spell_line({{0, "1242"}, {1, "Curse Cure"}, {20, "-9"}, {83, "1"}, {86, "116"}}));
    BuffSlot& curse = l.state.buffs[0];
    curse.spell = 1239;
    curse.effect_type = 2;
    curse.level = 10;
    curse.tics = 600;
    curse.caster_id = 2;
    curse.slot_data = {3, 7, 9};
    curse.counters_known = true;

    CHECK(l.cure(1242).removed_buffs.empty());
    CHECK(curse.slot_data[0] == 0);
    CHECK(curse.slot_data[1] == 7);
    CHECK(curse.slot_data[2] == 3);
    CHECK(l.cure(1242).removed_buffs.empty());
    CHECK(curse.slot_data[1] == 1);
    CHECK(curse.slot_data[2] == 0);
    auto last = l.cure(1242);
    CHECK(last.removed_buffs.empty());
    CHECK(curse.spell == 1239);
    for (std::int32_t v : curse.slot_data) CHECK(v == 0);
}

TEST_CASE("a duration spell's cure spends the whole mixed counter pool when known") {
    Landing l(spell_line({{0, "1239"}, {16, "3"}, {20, "6"}, {21, "6"}, {86, "116"}, {87, "36"}}) + "\n" +
              spell_line({{0, "1242"}, {16, "3"}, {17, "5"}, {20, "-8"}, {83, "1"}, {86, "116"}}));
    BuffSlot& mixed = l.state.buffs[0];
    mixed.spell = 1239;
    mixed.effect_type = 2;
    mixed.level = 10;
    mixed.tics = 600;
    mixed.caster_id = 2;
    mixed.slot_data = {6, 6};

    SUBCASE("known") { mixed.counters_known = true; }
    SUBCASE("unknown") { mixed.slot_data = {}; }

    auto partial = l.cure(1242);
    CHECK(partial.value == 1);
    REQUIRE_FALSE(partial.placed_buffs.empty());
    CHECK(partial.removed_buffs.empty());
    if (mixed.counters_known) {
        CHECK(mixed.slot_data == std::array<std::int32_t, 12>{0, 4});
    } else {
        CHECK(mixed.slot_data == std::array<std::int32_t, 12>{});
    }
}
