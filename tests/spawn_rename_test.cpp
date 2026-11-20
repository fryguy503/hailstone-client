// OP_GMLastName and OP_LevelAppearance in EQEmu's layouts (GMLastName_Struct,
// LevelAppearance_Struct) against the original's handlers (0x4bbcd0, 0x4d0053 / 0x58fed0).

#include <doctest/doctest.h>

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>

#include "app/actor_emitters.h"
#include "assets/character.h"
#include "assets/model_points.h"
#include "mock_server/harness.h"
#include "net/rename_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

constexpr std::uint32_t kSpawn = 301;

net::AppPacket truncated(net::AppPacket p) {
    p.data.pop_back();
    return p;
}

LevelAppearance eqemu_level_up(std::uint32_t id) {
    // Mob::SendLevelAppearance: parm 0x4d..0x51 with the values it fills in.
    LevelAppearance v;
    v.spawn_id = id;
    v.groups = {{{0x4d, 1, 0}, {0x4e, 2, 0}, {0x4f, 1, 1}, {0x50, 1, 1}, {0x51, 2, 0}}};
    return v;
}

} // namespace

TEST_CASE("GMLastName: wire layout and decode") {
    auto p = encode(GMLastName{"Orc_pawn", "Gm_name", "the Smith", 1});
    CHECK(p.data.size() == kGMLastNameSize);
    auto back = decode_gm_last_name(p);
    REQUIRE(back);
    CHECK(back->name == "Orc_pawn");
    CHECK(back->gm_name == "Gm_name");
    CHECK(back->last_name == "the Smith");
    CHECK(back->apply == 1);
    CHECK_FALSE(decode_gm_last_name(truncated(p)));
}

TEST_CASE("GMLastName sets the last name of a spawn and of the player") {
    Rig rig;
    REQUIRE(rig.to_zone());
    rig.conn.receive_zone_packet_for_test(net::encode(make_spawn("Orc_pawn", kSpawn, 1, 54, 0, 0, 3)));

    rig.conn.receive_zone_packet_for_test(encode(GMLastName{"Orc_pawn", "Gm", "Flag_zero", 0}));
    CHECK(rig.conn.world().spawns.at(kSpawn).last_name.empty());
    rig.conn.receive_zone_packet_for_test(encode(GMLastName{"Unknown_one", "Gm", "Nope", 1}));
    rig.conn.receive_zone_packet_for_test(truncated(encode(GMLastName{"Orc_pawn", "Gm", "Short", 1})));
    CHECK(rig.conn.world().spawns.at(kSpawn).last_name.empty());

    const auto serial = rig.conn.world().spawn_set_serial;
    rig.conn.receive_zone_packet_for_test(encode(GMLastName{"Orc_pawn", "Gm", "the Smith", 1}));
    CHECK(rig.conn.world().spawns.at(kSpawn).last_name == "the Smith");
    CHECK(rig.conn.world().spawn_set_serial != serial);

    rig.conn.receive_zone_packet_for_test(encode(GMLastName{rig.conn.world().player()->name, "Gm", "Own_last", 1}));
    CHECK(rig.conn.world().player()->last_name == "Own_last");
    REQUIRE(rig.conn.profile());
    CHECK(rig.conn.profile()->last_name == "Own_last");
}

TEST_CASE("LevelAppearance: wire layout and decode") {
    auto p = encode(eqemu_level_up(kSpawn));
    CHECK(p.data.size() == kLevelAppearanceSize);
    auto back = decode_level_appearance(p);
    REQUIRE(back);
    CHECK(back->spawn_id == kSpawn);
    CHECK(back->groups[0].effect_id == 0x4d);
    CHECK(back->groups[4].effect_id == 0x51);
    CHECK(back->groups[2].slot == 1);
    CHECK(back->groups[2].flag == 1);
    CHECK_FALSE(decode_level_appearance(truncated(p)));

    LevelAppearance sparse;
    sparse.groups[1].effect_id = 0x4e;
    CHECK_FALSE(level_appearance_group_used(sparse.groups[0]));
    CHECK(level_appearance_group_used(sparse.groups[1]));
    LevelAppearanceGroup negative;
    negative.effect_id = 0xffffffffu; // the original's test is a signed "above 0"
    CHECK_FALSE(level_appearance_group_used(negative));
}

TEST_CASE("LevelAppearance gives one cue per used group, for a known spawn only") {
    Rig rig;
    REQUIRE(rig.to_zone());
    rig.conn.receive_zone_packet_for_test(net::encode(make_spawn("Orc_pawn", kSpawn, 1, 54, 0, 0, 3)));
    rig.conn.receive_zone_packet_for_test(encode(eqemu_level_up(9999))); // unknown spawn
    CHECK(rig.conn.take_level_effect_cues().empty());
    rig.conn.receive_zone_packet_for_test(truncated(encode(eqemu_level_up(kSpawn))));
    CHECK(rig.conn.take_level_effect_cues().empty());
    rig.conn.receive_zone_packet_for_test(encode(eqemu_level_up(kSpawn)));
    const auto cues = rig.conn.take_level_effect_cues();
    REQUIRE(cues.size() == 5);
    for (std::uint32_t i = 0; i < 5; ++i) {
        CHECK(cues[i].spawn_id == kSpawn);
        CHECK(cues[i].effect_id == 77 + i); // Lvl_UpB_01..05
    }
    CHECK(cues[0].slot == 1);
    CHECK_FALSE(cues[0].flag);
    CHECK(cues[2].flag);
    CHECK(rig.conn.take_level_effect_cues().empty()); // taken
    LevelAppearance sparse;
    sparse.spawn_id = kSpawn;
    sparse.groups[3] = {77, 1, 0};
    rig.conn.receive_zone_packet_for_test(encode(sparse)); // effect 0 groups are skipped
    CHECK(rig.conn.take_level_effect_cues().size() == 1);
}

namespace {

// Hand-built bytes, not the encoder's: a field of `n` bytes filled by `text` without a terminator when it fills it.
void put_field(std::vector<std::uint8_t>& b, std::size_t at, const std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) b[at + i] = static_cast<std::uint8_t>(text[i]);
}

net::AppPacket raw(Op op, const std::vector<std::uint8_t>& b) {
    std::vector<std::byte> d;
    for (auto v : b) d.push_back(static_cast<std::byte>(v));
    return net::AppPacket(to_wire(op), std::move(d));
}

constexpr std::size_t kFlagAt = 196;

} // namespace

TEST_CASE("GMLastName from raw bytes: the flag at 196") {
    std::vector<std::uint8_t> b(200, 0);
    put_field(b, 0, "Orc_pawn");
    put_field(b, 128, "the Smith");
    b[kFlagAt] = 1;
    auto v = decode_gm_last_name(raw(Op::GMLastName, b));
    REQUIRE(v);
    CHECK(v->last_name == "the Smith");
    CHECK(v->apply == 1);
    b[kFlagAt] = 0;
    CHECK(decode_gm_last_name(raw(Op::GMLastName, b))->apply == 0);
}

TEST_CASE("LevelAppearance cues are bounded between two takes") {
    Rig rig;
    REQUIRE(rig.to_zone());
    rig.conn.receive_zone_packet_for_test(net::encode(make_spawn("Orc_pawn", kSpawn, 1, 54, 0, 0, 3)));
    for (std::uint32_t i = 0; i < 40; ++i) {
        auto v = eqemu_level_up(kSpawn);
        v.groups[0].slot = i;
        rig.conn.receive_zone_packet_for_test(encode(v));
    }
    const auto cues = rig.conn.take_level_effect_cues();
    REQUIRE(cues.size() == kLevelEffectCuesKept);
    CHECK(cues.front().slot == 24); // 16 packets kept: the first of them is the 25th
    CHECK(cues.back().effect_id == 0x51);
}

// ---------------------------------------------------------------- drawing the level-up effects (no GPU)

namespace {

assets::EmitterDefinition effect_dot() {
    assets::EmitterDefinition d;
    d.duration = 1.0f;
    d.lifetime = 2.0f;
    d.initial_count = 1;
    d.per_tick = 0;
    d.max_opacity = 1.0f;
    return d;
}

constexpr std::uint32_t kDrawnSpawn = 7;
constexpr int kChest = 1, kHead = 2;

struct EffectRig {
    // Index 0 is the file's placeholder; 1 and 2 are real definitions.
    app::ActorEmitters emitters{{assets::EmitterDefinition{}, effect_dot(), effect_dot()}};
    glm::mat4 chest = glm::translate(glm::mat4(1.0f), glm::vec3(10, 20, 30));
    EffectRig() {
        emitters.set_point_world([this](std::uint32_t id, int point, glm::mat4& world) {
            if (id != kDrawnSpawn) return false;            // not drawn
            if (point != kChest && point != kHead) return false; // the model lacks the point
            world = point == kChest ? chest : glm::translate(glm::mat4(1.0f), glm::vec3(10, 20, 36));
            return true;
        });
    }
};

} // namespace

TEST_CASE("level effects: a valid cue starts an emitter attached to its spell point") {
    EffectRig rig;
    CHECK(rig.emitters.start_effect({kDrawnSpawn, 1, kChest, false}));
    REQUIRE(rig.emitters.emitters() == 1);
    const auto& e = rig.emitters.simulation().emitters()[0];
    CHECK(e.actor == kDrawnSpawn);
    CHECK_FALSE(e.persistent); // it ends with the definition's duration
    rig.emitters.update(0.01f, {}, [](std::uint32_t, int, glm::mat4&) { return false; }, {});
    rig.chest[3].x += 5;
    rig.emitters.update(0.01f, {}, [](std::uint32_t, int, glm::mat4&) { return false; }, {});
    CHECK(rig.emitters.simulation().emitters()[0].origin.x == doctest::Approx(15.0f)); // it follows the point
}

TEST_CASE("level effects: the flag places the emitter once, on the floor below the point") {
    EffectRig rig;
    rig.emitters.set_floor([](const glm::vec3&) { return std::optional<float>(2.0f); });
    CHECK(rig.emitters.start_effect({kDrawnSpawn, 2, kChest, true}));
    REQUIRE(rig.emitters.emitters() == 1);
    const auto& e = rig.emitters.simulation().emitters()[0];
    CHECK(e.actor == 0);
    CHECK(e.origin.x == doctest::Approx(10.0f));
    CHECK(e.origin.z == doctest::Approx(2.05f)); // vf126's raise of 0.05 above the hit
}

TEST_CASE("level effects: a floor more than 1000 below the point is not taken") {
    // vf126 casts 1000 units down from the point (0x1013681c); farther, the point's own z stays.
    EffectRig rig;
    rig.emitters.set_floor([](const glm::vec3&) { return std::optional<float>(-2000.0f); });
    CHECK(rig.emitters.start_effect({kDrawnSpawn, 2, kChest, true}));
    REQUIRE(rig.emitters.emitters() == 1);
    CHECK(rig.emitters.simulation().emitters()[0].origin.z == doctest::Approx(30.0f));
}

TEST_CASE("level effects: an unknown effect, spawn, or slot starts nothing") {
    EffectRig rig;
    CHECK_FALSE(rig.emitters.start_effect({kDrawnSpawn, 3, kChest, false}));  // past the file's end
    CHECK_FALSE(rig.emitters.start_effect({kDrawnSpawn, 0, kChest, false}));  // the placeholder
    CHECK_FALSE(rig.emitters.start_effect({99, 1, kChest, false}));           // a spawn not drawn
    CHECK_FALSE(rig.emitters.start_effect({kDrawnSpawn, 1, 5, false}));       // a point the model lacks
    CHECK_FALSE(rig.emitters.start_effect({kDrawnSpawn, 1, 10, false}));      // past the ten spell points
    CHECK(rig.emitters.emitters() == 0);
    CHECK(rig.emitters.effect_count() == 0);
}

TEST_CASE("level effects are bounded and leave with their spawn") {
    EffectRig rig;
    for (std::size_t i = 0; i < app::ActorEmitters::kMaxEffects + 10; ++i)
        CHECK(rig.emitters.start_effect({kDrawnSpawn, 1, kChest, false}));
    CHECK(rig.emitters.effect_count() == app::ActorEmitters::kMaxEffects);
    CHECK(rig.emitters.emitters() == app::ActorEmitters::kMaxEffects);
    rig.emitters.prune_effects([](std::uint32_t) { return true; });
    CHECK(rig.emitters.emitters() == app::ActorEmitters::kMaxEffects);
    rig.emitters.prune_effects([](std::uint32_t) { return false; }); // the spawn is gone
    CHECK(rig.emitters.emitters() == 0);
    CHECK(rig.emitters.effect_count() == 0);
}

TEST_CASE("level effects: a slot far out of range or negative starts nothing") {
    EffectRig rig;
    for (std::uint32_t slot : {10u, 16u, 0x1000u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) // 0xffffffff is -1 signed
        CHECK_FALSE(rig.emitters.start_effect({kDrawnSpawn, 1, slot, false}));
    CHECK(rig.emitters.emitters() == 0);
}

TEST_CASE("level effects: the slot numbers are the model's spell points") {
    static_assert(assets::kSpellPointNames[kChest] == "SPELLPOINT_CHEST");
    static_assert(assets::kSpellPointNames[kHead] == "SPELLPOINT_HEAD");
    static_assert(assets::kSpellPointNames.size() == 10);
}

TEST_CASE("level effects: a fixed-length effect ends and its record goes") {
    // Lvl_UpB_01..05 of the shipped file have a negative duration (0.5 or 1 s): the emitter lasts that long.
    auto d = effect_dot();
    d.duration = -0.5f;
    d.lifetime = 0.25f;
    app::ActorEmitters emitters({assets::EmitterDefinition{}, d});
    emitters.set_point_world([](std::uint32_t, int, glm::mat4& w) { w = glm::mat4(1.0f); return true; });
    REQUIRE(emitters.start_effect({kDrawnSpawn, 1, kChest, false}));
    const auto no_pose = [](std::uint32_t, int, glm::mat4&) { return false; };
    for (int i = 0; i < 20; ++i) emitters.update(0.05f, {}, no_pose, {}); // 1 s
    CHECK(emitters.emitters() == 0);
    emitters.prune_effects([](std::uint32_t) { return true; });
    CHECK(emitters.effect_count() == 0);
}

TEST_CASE("level effects: an emitter that lasts 0 ms still counts against the limit and leaves with its spawn") {
    auto d = effect_dot();
    d.initial_count = 0;
    d.lifetime = 0.0f;
    app::ActorEmitters emitters({assets::EmitterDefinition{}, d});
    emitters.set_point_world([](std::uint32_t, int, glm::mat4& w) { w = glm::mat4(1.0f); return true; });
    for (std::size_t i = 0; i < app::ActorEmitters::kMaxEffects * 3; ++i)
        emitters.start_effect({kDrawnSpawn, 1, kChest, false});
    CHECK(emitters.effect_count() <= app::ActorEmitters::kMaxEffects);
    CHECK(emitters.emitters() <= app::ActorEmitters::kMaxEffects);
    emitters.prune_effects([](std::uint32_t) { return false; });
    CHECK(emitters.emitters() == 0);
}

TEST_CASE("level effects survive their actor's own mesh emitters being rebuilt") {
    auto d = effect_dot();
    d.duration = 0;
    assets::CharacterMesh mesh;
    assets::MeshEmitter own;
    own.bone = 0;
    own.emitter.emitter = 1;
    mesh.emitters.push_back(own);
    app::ActorEmitters emitters({assets::EmitterDefinition{}, d});
    emitters.set_point_world([](std::uint32_t, int, glm::mat4& w) { w = glm::mat4(1.0f); return true; });
    app::ActorEmitters::Actor actor{kDrawnSpawn, &mesh};
    const auto pose = [](std::uint32_t, int, glm::mat4& w) { w = glm::mat4(1.0f); return true; };
    emitters.update(0.01f, std::span(&actor, 1), pose, {});
    REQUIRE(emitters.start_effect({kDrawnSpawn, 1, kChest, false}));
    const std::size_t with_effect = emitters.emitters();
    mesh.serial = {}; // another look: the actor's own emitters are made again, in this same update
    emitters.update(0.01f, std::span(&actor, 1), pose, {});
    CHECK(emitters.emitters() == with_effect);
    const auto& list = emitters.simulation().emitters();
    CHECK(std::any_of(list.begin(), list.end(), [](const game::Emitter& e) { return e.server_effect; }));
}
