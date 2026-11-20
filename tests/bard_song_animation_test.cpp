// A bard's song animations: a singing song always plays; an instrument song on a classic or Luclin model plays only
// with an instrument in hand and not while sitting; a new-style (EQG) model has no such model block.

#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/appearance.h"
#include "game/spawn_animation.h"
#include "game/spells.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/player_packets.h"
#include "net/spell_packets.h"
#include "net/zone_packets.h"

using namespace hailstone;

namespace {

constexpr std::uint32_t kSong = 22;
constexpr std::uint32_t kInstrumentItemId = 1003;
constexpr int kSongSkill = 41;
constexpr std::uint8_t kInstrumentAnim = game::kAnimDrum;
constexpr std::uint8_t kSingingAnim = static_cast<std::uint8_t>(AnimationType::CastBuffs);
constexpr std::uint16_t kBard = 900;
constexpr std::uint16_t kClassicRace = 1; // HUM
constexpr std::uint8_t kWarrior = 1;
constexpr std::uint8_t kNoAction = 0xfe; // the spawn's action before the landing
constexpr int kOneSpell = 1;
constexpr int kCastStage = 0, kImpactStage = 2; // SpellEffectCue::stage
constexpr std::size_t kOneImpactCue = 1;
constexpr std::size_t kOnePacket = 1;

constexpr std::size_t kSpellFields = 237;
constexpr std::size_t kFirstEffectField = 86;
constexpr int kEffectSlots = 12;
constexpr char kEmptyEffect[] = "254";
constexpr std::size_t kSkillField = 100;
constexpr std::size_t kCastingAnimField = 120;
constexpr std::uint8_t kSpellActionType = 231;

std::string spell_line(int id, int skill, int casting_anim) {
    std::vector<std::string> fl(kSpellFields, "0");
    fl[0] = std::to_string(id);
    fl[1] = "Spell" + std::to_string(id);
    for (int k = 0; k < kEffectSlots; ++k) fl[kFirstEffectField + static_cast<std::size_t>(k)] = kEmptyEffect;
    fl[kSkillField] = std::to_string(skill);
    fl[kCastingAnimField] = std::to_string(casting_anim);
    std::string out;
    for (std::size_t i = 0; i < fl.size(); ++i) out += (i ? "^" : "") + fl[i];
    return out + "\n";
}

net::AppPacket landing(std::uint32_t spell, std::uint16_t source, std::uint16_t target) {
    net::Action a;
    a.source = source;
    a.target = target;
    a.type = kSpellActionType;
    a.spell_id = spell;
    return net::encode(a);
}

std::optional<std::size_t> player_state_index(const test::Rig& rig, net::Op op, std::uint32_t spawn_id,
                                              std::uint32_t state) {
    for (std::size_t i = 0; i < rig.server.zone_log.size(); ++i) {
        const auto& p = rig.server.zone_log[i];
        if (p.opcode != net::to_wire(op)) continue;
        if (auto s = net::decode_player_state(p); s && s->spawn_id == spawn_id && s->state == state) return i;
    }
    return std::nullopt;
}

std::size_t player_state_count(const test::Rig& rig, net::Op op, std::uint32_t spawn_id, std::uint32_t state) {
    std::size_t n = 0;
    for (const auto& p : rig.server.zone_log) {
        if (p.opcode != net::to_wire(op)) continue;
        if (auto s = net::decode_player_state(p); s && s->spawn_id == spawn_id && s->state == state) ++n;
    }
    return n;
}

std::optional<std::size_t> client_ready_index(const test::Rig& rig) {
    for (std::size_t i = 0; i < rig.server.zone_log.size(); ++i)
        if (rig.server.zone_log[i].opcode == net::to_wire(net::Op::ClientReady)) return i;
    return std::nullopt;
}

} // namespace

TEST_CASE("The instrument song animation rule") {
    using game::instrument_song_animation;
    CHECK(instrument_song_animation(kSingingAnim, game::kClassBard, game::kStandStateStanding, false));
    CHECK(instrument_song_animation(kInstrumentAnim, game::kClassBard, game::kStandStateStanding, true));
    CHECK_FALSE(instrument_song_animation(kInstrumentAnim, game::kClassBard, game::kStandStateStanding, false));
    CHECK_FALSE(instrument_song_animation(kInstrumentAnim, game::kClassBard, game::kStandStateSitting, true));
    CHECK_FALSE(instrument_song_animation(kInstrumentAnim, kWarrior, game::kStandStateSitting, false));
    CHECK(instrument_song_animation(kInstrumentAnim, kWarrior, game::kStandStateStanding, false));
}

TEST_CASE("The clip path applies the instrument song rule") {
    auto has_clip = [](std::string_view) { return true; };
    game::ActionGate blocked{game::kClassBard, game::kStandStateStanding, false};
    CHECK(game::action_clip(kInstrumentAnim, has_clip, false, {}, {}, nullptr, nullptr, nullptr, &blocked).empty());
    game::ActionGate allowed{game::kClassBard, game::kStandStateStanding, true};
    CHECK_FALSE(
        game::action_clip(kInstrumentAnim, has_clip, false, {}, {}, nullptr, nullptr, nullptr, &allowed).empty());
    // A new-style model has no such block.
    CHECK_FALSE(
        game::action_clip(kInstrumentAnim, has_clip, true, {}, {}, nullptr, nullptr, nullptr, &blocked).empty());
}

TEST_CASE("A bard entering the zone with an instrument publishes the bit after ClientReady") {
    auto config = test::MockConfig::standard();
    auto instrument = test::make_item(kInstrumentItemId, "Lute", net::kSlotPrimary);
    instrument.item_type = net::kItemTypeStringInstrument;
    config.characters.front().inventory = {instrument};
    test::Rig rig(config);
    REQUIRE(rig.to_zone());
    const auto me = static_cast<std::uint16_t>(rig.conn.world().player_id);
    REQUIRE(me != 0);
    CHECK((rig.conn.world().spawns.at(me).player_state & net::kPlayerStateInstrument) != 0);
    REQUIRE(rig.until([&] {
        return player_state_index(rig, net::Op::PlayerStateAdd, me, net::kPlayerStateInstrument).has_value();
    }));
    const auto ready = client_ready_index(rig);
    const auto add = player_state_index(rig, net::Op::PlayerStateAdd, me, net::kPlayerStateInstrument);
    REQUIRE(ready.has_value());
    REQUIRE(add.has_value());
    CHECK(*add > *ready);
}

TEST_CASE("A real worn-slot swap publishes the instrument bit") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    const auto me = static_cast<std::uint16_t>(conn.world().player_id);
    REQUIRE(me != 0);
    auto& spawn = conn.world_for_test().spawns.at(me);
    auto& inv = net::GameConnectionTestAccess::inventory(conn);
    net::Item instrument;
    instrument.item_type = net::kItemTypeStringInstrument;
    REQUIRE(inv.put(net::ItemSlot::possessions(net::kSlotGeneral1), instrument));
    rig.server.zone_log.clear();
    net::GameConnectionTestAccess::move_item(conn, net::ItemSlot::possessions(net::kSlotGeneral1),
                                             net::ItemSlot::possessions(net::kSlotPrimary), 1);
    CHECK((spawn.player_state & net::kPlayerStateInstrument) != 0);
    CHECK(rig.until([&] {
        return player_state_index(rig, net::Op::PlayerStateAdd, me, net::kPlayerStateInstrument).has_value();
    }));
    // The same state again sends nothing; the log stays for the count below.
    net::GameConnectionTestAccess::refresh_instrument_state(conn);
    // Swapping the instrument back out clears the bit and sends OP_PlayerStateRemove.
    net::GameConnectionTestAccess::move_item(conn, net::ItemSlot::possessions(net::kSlotPrimary),
                                             net::ItemSlot::possessions(net::kSlotGeneral1), 1);
    CHECK((spawn.player_state & net::kPlayerStateInstrument) == 0);
    CHECK(rig.until([&] {
        return player_state_index(rig, net::Op::PlayerStateRemove, me, net::kPlayerStateInstrument).has_value();
    }));
    // Since the first Add: no second Add, exactly one Remove.
    CHECK(player_state_count(rig, net::Op::PlayerStateAdd, me, net::kPlayerStateInstrument) == kOnePacket);
    CHECK(player_state_count(rig, net::Op::PlayerStateRemove, me, net::kPlayerStateInstrument) == kOnePacket);
}

TEST_CASE("A received instrument bit on another spawn lets its landing animate") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_line(kSong, kSongSkill, kInstrumentAnim)) == kOneSpell);
    conn.set_spell_table_for_test(table);
    const auto me = static_cast<std::uint16_t>(conn.world().player_id);
    REQUIRE(me != 0);
    auto& spawn = conn.world_for_test().spawns[kBard];
    spawn.id = kBard;
    spawn.class_id = game::kClassBard;
    spawn.race = kClassicRace;
    spawn.stand_state = game::kStandStateStanding;
    spawn.player_state = 0;
    spawn.action = kNoAction;
    auto has_clip = [](std::string_view) { return true; };
    auto gate = [&] { return game::ActionGate{spawn.class_id, spawn.stand_state, game::holds_instrument(spawn)}; };

    // The landing sets the action; without the bit the clip path blocks it.
    conn.receive_zone_packet_for_test(landing(kSong, kBard, me));
    CHECK(spawn.action == kInstrumentAnim);
    {
        auto g = gate();
        CHECK(game::action_clip(spawn.action, has_clip, false, {}, {}, nullptr, nullptr, nullptr, &g).empty());
    }
    // The server relays the bit for another player; the same action now passes the clip path.
    net::PlayerState add;
    add.spawn_id = kBard;
    add.state = net::kPlayerStateInstrument;
    conn.receive_zone_packet_for_test(net::encode_player_state(true, add));
    CHECK((spawn.player_state & net::kPlayerStateInstrument) != 0);
    {
        auto g = gate();
        CHECK_FALSE(game::action_clip(spawn.action, has_clip, false, {}, {}, nullptr, nullptr, nullptr, &g).empty());
    }
    auto cues = conn.take_spell_effect_cues();
    REQUIRE(cues.size() == kOneImpactCue);
    CHECK(cues.front().stage == kImpactStage);
    CHECK(cues.front().source == kBard);
    CHECK(cues.front().target == me);
    for (const auto& cue : cues) CHECK(cue.stage != kCastStage); // a song's pulse adds no cast stage
}
