// What the server keys by ids or names of its own choosing stays bounded: per spawn the client knows (and forgotten
// with it), per window that asked, or under a generous cap well above anything a server legitimately sends.

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "game/bazaar.h"
#include "game/spells.h"
#include "game/tasks.h"
#include "core/byte_io.h"
#include "mock_server/harness.h"
#include "net/augment_packets.h"
#include "net/group_packets.h"
#include "net/market_packets.h"
#include "net/spell_packets.h"
#include "net/task_packets.h"
#include "net/tribute_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

constexpr std::uint32_t kKnown = 50;
constexpr std::uint16_t kUnknown = 4321;

bool add_known(Rig& rig) {
    rig.server.add_spawn(make_spawn("Known", kKnown, 0, 1, 10.0f, 10.0f, 3.0f, 0.0f));
    return rig.until([&] { return rig.conn.world().spawns.count(kKnown) == 1; });
}

AppPacket select_tribute(Op op, std::uint32_t id, const std::string& text) {
    ByteWriter w;
    if (op == Op::SelectTribute) {
        w.le(std::uint32_t{0}); // the spawn id
        w.le(id);
        w.fixed_string(text, text.size() + 1);
    } else { // guild: u32 id, 8 bytes, the text
        w.le(id);
        w.le(std::uint32_t{0});
        w.le(std::uint32_t{0});
        w.fixed_string(text, text.size() + 1);
    }
    return AppPacket(to_wire(op), w.take());
}

} // namespace

TEST_CASE("server keyed state: group mana and endurance are kept only for a known spawn") {
    // Handle_OP_MobEnduranceUpdate @0x4bd350 returns for an id with no spawn.
    Rig rig;
    REQUIRE(rig.to_zone());
    REQUIRE(add_known(rig));
    rig.conn.receive_zone_packet_for_test(encode_mob_percent(Op::MobManaUpdate, {static_cast<std::uint16_t>(kKnown), 40}));
    rig.conn.receive_zone_packet_for_test(encode_mob_percent(Op::MobEnduranceUpdate, {static_cast<std::uint16_t>(kKnown), 30}));
    for (std::uint16_t id = 1000; id < 1100; ++id) {
        rig.conn.receive_zone_packet_for_test(encode_mob_percent(Op::MobManaUpdate, {id, 10}));
        rig.conn.receive_zone_packet_for_test(encode_mob_percent(Op::MobEnduranceUpdate, {id, 10}));
    }
    CHECK(rig.conn.group().mana_percent.size() == 1);
    CHECK(rig.conn.group().mana_percent.at(kKnown) == 40);
    CHECK(rig.conn.group().endurance_percent.size() == 1);
    CHECK(rig.conn.group().endurance_percent.at(kKnown) == 30);
}

TEST_CASE("server keyed state: another caster's cast is kept only for a known spawn and goes with it") {
    Rig rig;
    REQUIRE(rig.to_zone());
    REQUIRE(add_known(rig));
    std::vector<std::string> cols(237, "0");
    for (int i = 1; i <= 8; ++i) cols[static_cast<std::size_t>(i)].clear();
    for (int i = 86; i <= 97; ++i) cols[static_cast<std::size_t>(i)] = "254";
    cols[0] = "300";
    cols[1] = "Bolt";
    std::string line;
    for (std::size_t i = 0; i < cols.size(); ++i) line += (i ? "^" : "") + cols[i];
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(line) == 1);
    rig.conn.set_spell_table_for_test(table);
    // A huge cast time keeps an entry for weeks: unknown casters add no cast, and the cast stages stop at the cap.
    rig.conn.receive_zone_packet_for_test(encode(BeginCast{300, static_cast<std::uint16_t>(kKnown), 0x7fffffffu}));
    CHECK(rig.conn.spell_state().others.count(kKnown) == 1);
    const std::size_t cap = GameConnection::cast_stages_kept_for_test();
    for (std::uint16_t id = 1000; id < 1000 + cap + 10; ++id)
        rig.conn.receive_zone_packet_for_test(encode(BeginCast{300, id, 0x7fffffffu}));
    CHECK(rig.conn.spell_state().others.size() == 1);
    CHECK(rig.conn.cast_stages_shown_for_test() == cap);
    // The known caster's go with its spawn.
    rig.server.remove_spawn(kKnown);
    REQUIRE(rig.until([&] { return rig.conn.world().spawns.count(kKnown) == 0; }));
    CHECK(rig.conn.spell_state().others.empty());
    CHECK(rig.conn.cast_stages_shown_for_test() == cap - 1);
}

TEST_CASE("server keyed state: an augment's solvent text is taken only for a window that asked") {
    Rig rig;
    REQUIRE(rig.to_zone());
    for (std::uint32_t window = 100; window < 200; ++window)
        rig.conn.receive_zone_packet_for_test(encode(AugmentInfo{0, window, "Solvent", 0}));
    CHECK(rig.conn.augment_solvent_text(150).empty());
    rig.conn.request_augment_info(1234, 7);
    CHECK(rig.conn.augment_solvent_text(7).empty()); // asked, no answer yet
    rig.conn.receive_zone_packet_for_test(encode(AugmentInfo{0, 7, "Solvent", 0}));
    CHECK_FALSE(rig.conn.augment_solvent_text(7).empty());
    CHECK(rig.conn.augment_solvent_text(150).empty());
}

TEST_CASE("server keyed state: tributes and their descriptions stop at kTributesKept new ids") {
    Rig rig;
    REQUIRE(rig.to_zone());
    for (std::uint32_t id = 1; id <= kTributesKept + 10; ++id) {
        TributeAbility a;
        a.id = id;
        a.name = "T";
        rig.conn.receive_zone_packet_for_test(encode(a));
        rig.conn.receive_zone_packet_for_test(encode_guild_tribute_info(a));
        rig.conn.receive_zone_packet_for_test(select_tribute(Op::SelectTribute, id, "d"));
        rig.conn.receive_zone_packet_for_test(select_tribute(Op::GuildSelectTribute, id, "d"));
    }
    const auto& t = rig.conn.tribute();
    const auto& g = rig.conn.guild_tribute();
    CHECK(t.abilities.size() == kTributesKept);
    CHECK(t.descriptions.size() == kTributesKept);
    CHECK(g.abilities.size() == kTributesKept);
    CHECK(g.descriptions.size() == kTributesKept);
    CHECK_FALSE(t.abilities.contains(kTributesKept + 1));
    // An id already kept is still updated.
    TributeAbility a;
    a.id = 1;
    a.name = "Renamed";
    rig.conn.receive_zone_packet_for_test(encode(a));
    rig.conn.receive_zone_packet_for_test(select_tribute(Op::SelectTribute, 1, "new"));
    CHECK(t.abilities.at(1).name == "Renamed");
    CHECK(t.descriptions.at(1) == "new");
}

TEST_CASE("server keyed state: bazaar traders and search rows stop at their caps") {
    Rig rig;
    REQUIRE(rig.to_zone());
    for (std::uint32_t id = 1; id <= game::kBazaarTradersKept + 10; ++id) {
        BazaarTrader t;
        t.trader_id = id;
        t.name = "Trader";
        rig.conn.receive_zone_packet_for_test(encode_bazaar_add_trader(t));
    }
    CHECK(rig.conn.bazaar_search_state().traders.size() == game::kBazaarTradersKept);
    BazaarTrader again;
    again.trader_id = 1;
    again.name = "Moved";
    rig.conn.receive_zone_packet_for_test(encode_bazaar_add_trader(again));
    CHECK(rig.conn.bazaar_search_state().traders.at(1).name == "Moved");

    BazaarResults r;
    r.trader_id = 1;
    for (int i = 0; i < 100; ++i) {
        BazaarResult e;
        e.trader_id = 1, e.serial = "0000000000000001", e.cost = 1, e.quantity = 1, e.item_id = 1001, e.name = "Thing";
        r.results.push_back(e);
    }
    const std::size_t packets = game::kBazaarResultsKept / r.results.size() + 2;
    for (std::size_t i = 0; i < packets; ++i) rig.conn.receive_zone_packet_for_test(encode(r));
    CHECK(rig.conn.bazaar_search_state().results.size() == game::kBazaarResultsKept);
}

TEST_CASE("server keyed state: the shared task's members stop at kSharedTaskMembersKept") {
    Rig rig;
    REQUIRE(rig.to_zone());
    for (std::size_t i = 0; i < game::kSharedTaskMembersKept + 10; ++i)
        rig.conn.receive_zone_packet_for_test(encode(SharedTaskMemberChange{true, "M" + std::to_string(i)}));
    CHECK(rig.conn.tasks().shared_members.size() == game::kSharedTaskMembersKept);
    // A removal still works at the cap, and makes room.
    rig.conn.receive_zone_packet_for_test(encode(SharedTaskMemberChange{false, "M0"}));
    rig.conn.receive_zone_packet_for_test(encode(SharedTaskMemberChange{true, "Late"}));
    CHECK(rig.conn.tasks().shared_members.size() == game::kSharedTaskMembersKept);
    CHECK(rig.conn.tasks().shared_members.back() == "Late");
}
