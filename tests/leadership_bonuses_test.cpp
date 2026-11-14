#include <doctest/doctest.h>

#include "game/leadership_bonuses.h"
#include "game/mechanics/character.h"
#include "game/mechanics/sheet_builder.h"
#include "game/mechanics/shown_stats.h"
#include "game/mechanics/tables.h"
#include "mock_server/harness.h"
#include "net/group_packets.h"
#include "net/social_packets.h"

using namespace hailstone;

TEST_CASE("leadership bonuses: active leader ranks reach character sheets and expire with membership") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    auto group = [&](int count) {
        net::GroupUpdateB u;
        u.leader = "Gennro";
        for (int i = 0; i < count; ++i) {
            net::GroupUpdateMember m;
            m.slot = static_cast<std::uint32_t>(i);
            m.name = i == 0 ? "Gennro" : "Friend" + std::to_string(i);
            u.members.push_back(m);
        }
        c.receive_zone_packet_for_test(net::encode(u));
    };
    group(3);
    net::GroupLeadershipUpdate g;
    g.ranks[9] = 2;
    g.ranks[10] = 2;
    g.ranks[11] = 3;
    g.ranks[12] = 1;
    c.receive_zone_packet_for_test(net::encode(g));
    CHECK(c.leadership().ranks[11] == 0); // the member's own purchases are not the source
    auto live = c.live_state();
    CHECK(live.leadership.hp == 100);
    CHECK(live.leadership.mana == 60);
    CHECK(live.leadership.hp_regen == 1);
    CHECK(live.leadership.offense == 15);

    auto profile = *c.profile();
    profile.class_ = 2; // a mana-using class, so the mana formula exercises the leadership addition
    auto boosted = game::mechanics::build_sheet(profile, c.inventory(), c.spell_state(), live);
    live.leadership = {};
    auto ordinary = game::mechanics::build_sheet(profile, c.inventory(), c.spell_state(), live);
    game::mechanics::GameTables tables;
    game::mechanics::Character before(ordinary, tables, {}), after(boosted, tables, {});
    before.process_all_stats();
    after.process_all_stats();
    CHECK(after.max_hp() - before.max_hp() == 100);
    CHECK(after.max_mana() - before.max_mana() == 60);
    CHECK(after.hp_regen() - before.hp_regen() == 1);
    CHECK(boosted.leadership_offense_bonus == 15);
    const auto skill = game::mechanics::attack_skill(after);
    const auto base_attack = game::mechanics::attack_value(before, skill, false);
    CHECK(game::mechanics::attack_value(after, skill, false) - base_attack == 15);
    CHECK(game::mechanics::shown_atk(after, false) ==
          (static_cast<std::int64_t>(game::mechanics::offense_value(after, skill, true)) + base_attack + 15) * 1000 / 744);

    net::RaidUpdate r;
    r.general.action = net::kRaidAddMember;
    for (int i = 0; i < 18; ++i) {
        r.general.player_name = i == 0 ? "Gennro" : "Raider" + std::to_string(i);
        c.receive_zone_packet_for_test(net::encode_raid_update(r));
    }
    r = {};
    r.general.action = net::kRaidSetLeader;
    r.general.leader_name = "Gennro";
    r.leadership[24] = 5;
    r.leadership[25] = 1;
    r.leadership[26] = 2;
    r.leadership[27] = 3;
    c.receive_zone_packet_for_test(net::encode_raid_update(r));
    const auto both = c.live_state().leadership;
    CHECK(both.hp == 160);
    CHECK(both.mana == 90);
    CHECK(both.hp_regen == 4);
    CHECK(both.offense == 52);

    group(2); // the group effects stop immediately, even while raid effects remain
    CHECK(c.live_state().leadership.hp == 60);
    CHECK(c.live_state().leadership.offense == 37);
    r = {};
    r.general.action = net::kRaidDisband;
    c.receive_zone_packet_for_test(net::encode_raid_update(r));
    CHECK(c.live_state().leadership.hp == 0);
    CHECK(c.live_state().leadership.offense == 0);

    // Rebuilding the reused sheet clears old bonus inputs too.
    game::mechanics::build_sheet_into(boosted, profile, c.inventory(), c.spell_state(), c.live_state(), {});
    CHECK(boosted.leadership_hp_bonus == 0);
    CHECK(boosted.leadership_offense_bonus == 0);
}

TEST_CASE("leadership bonuses: invalid ranks and inactive contexts grant no effect") {
    std::array<std::uint32_t, 32> ranks;
    ranks.fill(0xffffffffu);
    const auto invalid = game::leadership_passive_bonuses(ranks, true, true);
    CHECK(invalid.hp == 0);
    CHECK(invalid.mana == 0);
    CHECK(invalid.hp_regen == 0);
    CHECK(invalid.offense == 0);
    ranks.fill(1);
    const auto inactive = game::leadership_passive_bonuses(ranks, false, false);
    CHECK(inactive.hp == 0);
    CHECK(inactive.offense == 0);
}
