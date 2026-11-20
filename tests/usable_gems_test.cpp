// The usable spell gems on a stock server (no server extension): CanUseMemorizedSpellSlot @0x433bc0 over the
// connection's AA cache, re-evaluated as the owned AAs change (GameConnection::update_usable_gems).
#include <algorithm>

#include <doctest/doctest.h>

#include "game/alt_adv.h"
#include "game/mechanics/character.h"
#include "manual_clock.h"
#include "mq_test_world.h"
#include "net/spell_packets.h"

using namespace hailstone;

namespace {

using A = net::GameConnectionTestAccess;

// A rank of Mnemonic Retention: SPA 326 (SpellSlotIncrease) with `extra` gems, for a cleric (class 2).
game::AltAbility mnemonic_retention(std::uint32_t rank_id, std::int32_t extra) {
    game::AltAbility a;
    a.id = rank_id;
    a.classes = 1u << 2;
    a.level_req = 1;
    a.effects.push_back({326, extra, 0, 1});
    return a;
}

} // namespace

TEST_CASE("a stock server's character has 8 gems, and 8 + SPA 326 as Mnemonic Retention ranks are owned and lost") {
    mq_test::World w("hailstone_usable_gems_test");
    net::GameConnection& c = *w.conn;
    game::AltAdvState& aa = A::alt_adv(c);
    aa.add(mnemonic_retention(1071, 1));
    aa.add(mnemonic_retention(4764, 2));
    aa.add(mnemonic_retention(7681, 4));

    A::update_usable_gems(c);
    CHECK(c.usable_gems() == 8);

    aa.set_owned(0, {}, {{1071, 1, 0}}); // OP_RespondAA: rank 1
    A::update_usable_gems(c);
    CHECK(c.usable_gems() == 9);

    aa.set_owned(0, {}, {{4764, 2, 0}}); // trained to rank 2
    A::update_usable_gems(c);
    CHECK(c.usable_gems() == 10);

    aa.set_owned(0, {}, {{7681, 4, 0}}); // rank 4: all 12
    A::update_usable_gems(c);
    CHECK(c.usable_gems() == 12);

    aa.clear_owned(); // OP_ClearAA
    A::update_usable_gems(c);
    CHECK(c.usable_gems() == 8);
}

TEST_CASE("with_character: the reused sheet follows the inventory, and a call from inside a call has its own") {
    // with_character builds into one kept sheet (no per-frame copy of every item's storage); a nested call must not
    // rebuild the sheet the outer character is reading.
    mq_test::World w("hailstone_usable_gems_nested_test");
    net::GameConnection& c = *w.conn;
    net::Item ring;
    ring.name = "Ring of the Test";
    REQUIRE(A::inventory(c).put(net::ItemSlot::possessions(15), ring));
    int outer_seen = 0, inner_seen = 0;
    A::with_character(c, [&](game::mechanics::Character& outer) {
        A::inventory(c).take(net::ItemSlot::possessions(15));
        A::with_character(c, [&](game::mechanics::Character& inner) { inner_seen = inner.sheet().item(15) ? 1 : 2; });
        outer_seen = outer.sheet().item(15) && outer.sheet().item(15)->name == "Ring of the Test" ? 1 : 2;
    });
    CHECK(inner_seen == 2); // the ring is gone for the inner one
    CHECK(outer_seen == 1); // the outer one's sheet is as it was built
    A::with_character(c, [&](game::mechanics::Character& ch) { outer_seen = ch.sheet().item(15) ? 1 : 2; });
    CHECK(outer_seen == 2); // the kept sheet, built again, has no ring
}

TEST_CASE("memorize_tick: a bard's step takes DEX, INT and meditate from one character") {
    // CSpellBookWnd::GetSpellMemTicksLeft @0x75db20: a bard steps (DEX + INT) / 30 + 1, then meditate / 10 + 2 more.
    // memorize_tick reads all three from one sheet, built only on a tick whose interval has passed.
    mq_test::World w("hailstone_memorize_tick_test");
    net::GameConnection& c = *w.conn;
    auto& prof = *A::profile(c);
    prof.class_ = 8; // bard
    prof.dex = 100;
    prof.int_ = 80;
    std::int32_t dex = 0, intel = 0, meditate = 0;
    A::with_character(c, [&](game::mechanics::Character& ch) {
        meditate = ch.adjusted_skill(31);
        ch.process_all_stats();
        dex = ch.calc_dex(true);
        intel = ch.calc_int(true);
    });
    REQUIRE(dex > 0);
    REQUIRE(intel > 0);
    // This bard has no trained Meditate, so it takes the floor of 10.
    REQUIRE(meditate < 10);
    const int step = std::max(meditate, 10) / 10 + (dex + intel) / 30 + 1 + 2;

    game::SpellState& sp = A::spells(c);
    sp.memorizing = game::Memorizing{0, 0, 200, 1000};
    sp.memorize_last_tick = 0;
    A::memorize_tick(c, 100000);
    REQUIRE(sp.memorizing.has_value());
    CHECK(sp.memorizing->ticks_left == 1000 - step);
    CHECK(sp.memorize_last_tick == 100000);
    // The same millisecond: not due, nothing taken.
    A::memorize_tick(c, 100000);
    CHECK(sp.memorizing->ticks_left == 1000 - step);
}

TEST_CASE("spell gem menu: a level 60 cleric memorizes at the trained Meditate speed") {
    using namespace std::chrono_literals;
    mq_test::World w("hailstone_memorize_meditate_test");
    auto& c = *w.conn;
    auto& prof = *A::profile(c);
    prof.level = A::status(c).level = 60;
    prof.skills.resize(100);
    prof.skills[31] = 252;
    bool have_caps = true;
    int step = 28;
    SUBCASE("with the game skill caps") {}
    SUBCASE("with no skill-cap file") { have_caps = false; }
    SUBCASE("with no trained Meditate") {
        prof.skills[31] = 0;
        step = 4;
    }
    if (have_caps) {
        std::filesystem::create_directories(w.dir / "Resources");
        std::ofstream(w.dir / "Resources/SkillCaps.txt") << "2^31^4^20^0\n2^31^100^252^0\n";
        w.game->rescan();
    }

    constexpr std::uint32_t spell_id = 1447;
    constexpr int gem = 2;
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(mq_test::spell_line({{0, "1447"}, {1, "Aegolism"}, {105, "60"}})) == 1);
    c.set_spell_table_for_test(table);
    A::spells(c).book[0] = spell_id;

    c.memorize_from_cursor(spell_id, gem);
    REQUIRE(c.spell_book_open());
    CHECK(c.sitting());
    CHECK_FALSE(c.spell_state().memorizing);
    A::start_scheduled_memorization(c);
    REQUIRE(c.spell_state().memorizing);
    CHECK(c.spell_state().memorizing->ticks_left == game::kMemorizeTicks);

    test::ManualClock clock;
    clock.hold();
    auto now = clock.source();
    const auto started = now();
    auto tick = [&](std::chrono::milliseconds advance) {
        clock.advance(advance);
        A::memorize_tick(c, 1000 + std::chrono::duration_cast<std::chrono::milliseconds>(now() - started).count());
    };
    tick(0ms);
    for (int left = game::kMemorizeTicks - step; left > 0; left -= step) {
        REQUIRE(c.spell_state().memorizing);
        CHECK(c.spell_state().memorizing->ticks_left == left);
        tick(500ms);
        REQUIRE(c.spell_state().memorizing);
        CHECK(c.spell_state().memorizing->ticks_left == left);
        tick(1ms);
    }
    CHECK_FALSE(c.spell_state().memorizing);
    CHECK(c.spell_state().memorize_sent_gem == gem);
    CHECK(c.spell_state().gems[gem].spell == game::kNoSpell);

    c.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{gem, spell_id, net::kMemMemorize, 0}));
    CHECK(c.spell_state().gems[gem].spell == spell_id);
    CHECK_FALSE(c.spell_book_open());
}
