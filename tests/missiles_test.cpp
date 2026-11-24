#include <doctest/doctest.h>

#include <cstring>
#include <limits>
#include <vector>

#include "game/missiles.h"
#include "game/spells.h"

using namespace hailstone;
using namespace hailstone::game;

namespace {

// An OP_SomeItemPacketMaybe as EQEmu's RoF2 translator writes it (rof2.cpp ENCODE(OP_SomeItemPacketMaybe),
// rof2_structs.h Arrow_Struct offsets), independent of the decoder.
std::vector<std::byte> eqemu_arrow(float src_y, float src_x, float src_z, float velocity, float launch_angle,
                                   float tilt, float arc, std::uint32_t source, std::uint32_t target,
                                   std::uint8_t skill, std::uint8_t item_type, const char* model) {
    std::vector<std::byte> b(116);
    auto putf = [&](std::size_t at, float f) { std::memcpy(&b[at], &f, 4); };
    auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(&b[at], &v, 4); };
    putf(0, src_y);
    putf(4, src_x);
    putf(8, src_z);
    putf(24, velocity);
    putf(28, launch_angle);
    putf(32, tilt);
    putf(44, arc);
    put32(48, source);
    put32(52, target);
    put32(56, 8005);
    b[70] = std::byte{175};
    b[73] = std::byte{skill};
    b[74] = std::byte{item_type};
    std::memcpy(&b[89], model, std::strlen(model));
    return b;
}

struct TestWorld {
    std::vector<MissileSpawn> spawns;
    bool solid = false;
    MissileWorld world() {
        MissileWorld w;
        w.spawn = [this](std::uint32_t id) -> std::optional<MissileSpawn> {
            for (const auto& s : spawns)
                if (s.id == id) return s;
            return std::nullopt;
        };
        w.sweep = [this](const glm::vec3&, const glm::vec3&, float) { return solid; };
        w.inside = [](const glm::vec3&) { return true; };
        return w;
    }
};

} // namespace

TEST_CASE("missile packet: EQEmu's skill lands in the range byte") {
    auto b = eqemu_arrow(10.0f, 20.0f, 3.0f, 4.0f, 128.0f, 125.0f, 50.0f, 7, 9, 7, 27, "IT10");
    auto p = decode_missile_packet(b);
    REQUIRE(p);
    CHECK(p->position == glm::vec3(10.0f, 20.0f, 3.0f));
    CHECK(p->speed == 4.0f);
    CHECK(p->heading == 128.0f);
    CHECK(p->tilt == 125.0f);
    CHECK(p->pitch == 50.0f);
    CHECK(p->source_id == 7);
    CHECK(p->target_id == 9);
    CHECK(p->item_id == 8005);
    CHECK(p->range == 7);
    CHECK(p->item_type == 27);
    CHECK(p->skill == 0);
    CHECK_FALSE(p->to_location);
    CHECK(p->model == "IT10");
    CHECK_FALSE(decode_missile_packet(std::span(b).first(115)));
}

TEST_CASE("missile packet: a NaN or infinite position, speed or angle is refused") {
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    CHECK_FALSE(decode_missile_packet(eqemu_arrow(nan, 0.0f, 0.0f, 4.0f, 0.0f, 125.0f, 50.0f, 7, 9, 7, 27, "IT10")));
    CHECK_FALSE(decode_missile_packet(eqemu_arrow(0.0f, 0.0f, 0.0f, inf, 0.0f, 125.0f, 50.0f, 7, 9, 7, 27, "IT10")));
    CHECK_FALSE(decode_missile_packet(eqemu_arrow(0.0f, 0.0f, 0.0f, 4.0f, -inf, 125.0f, 50.0f, 7, 9, 7, 27, "IT10")));
    CHECK_FALSE(decode_missile_packet(eqemu_arrow(0.0f, 0.0f, 0.0f, 4.0f, 0.0f, nan, 50.0f, 7, 9, 7, 27, "IT10")));
    CHECK_FALSE(decode_missile_packet(eqemu_arrow(0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 125.0f, nan, 7, 9, 7, 27, "IT10")));
    auto b = eqemu_arrow(0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 125.0f, 50.0f, 7, 9, 7, 27, "IT10");
    float v = nan;
    std::memcpy(&b[12], &v, 4); // velocity x
    CHECK_FALSE(decode_missile_packet(b));
}

TEST_CASE("missile launch animation and delay") {
    CHECK(missile_launch_animation(0, 27, 7, 1) == 9);  // a bow shot
    CHECK(missile_launch_animation(0, 28, 7, 1) == 10);
    CHECK(missile_launch_animation(0, 20, 14, 1) == 5); // an NMS bolt (item type 20, Conjuration)
    CHECK(missile_launch_animation(0, 9, 0, 1) == 44);
    CHECK(missile_launch_animation(10, 0, 0, 0x80) == 6);
    // New-style sources (0x59f8d0): BATK for arrows, BLPR for others, WATK in water.
    CHECK(missile_launch_animation(0, 27, 7, 522, 0, 0, true) == 0x5d);
    CHECK(missile_launch_animation(0, 20, 14, 522, 0, 0, true) == 0x51);
    CHECK(missile_launch_animation(0, 20, 14, 522, 0, 5, true) == 0x5e);
    CHECK(missile_launch_animation(0, 9, 0, 522, 0, 0, true) == 0x87);
    constexpr std::uint32_t kSecondMs = 1000, kWalkMs = 666, kTruncateMs = 668, kBelowOneMs = 1;
    constexpr std::uint8_t kRange = 7;
    constexpr std::uint32_t kDefaultDelayMs = 1500;
    // 0x58f5fe multiplies by the float 0.7 (0.699999988) at extended precision before truncating: 1000 ms is 699.
    constexpr std::uint32_t kSecondDelayMs = 699, kWalkDelayMs = 466, kTruncateDelayMs = 467;
    CHECK(missile_launch_delay(kSecondMs, kRange) == kSecondDelayMs);
    // An EQG clip's own period (a 666 ms walk): 70 % of it, not of the resampled grid's 693 ms.
    CHECK(missile_launch_delay(kWalkMs, kRange) == kWalkDelayMs);
    // 0x58f604-0x58f622 truncates (668 x 0.7 = 467.6 -> 467), and below 1 ms the 1500 ms default.
    CHECK(missile_launch_delay(kTruncateMs, kRange) == kTruncateDelayMs);
    CHECK(missile_launch_delay(kBelowOneMs, kRange) == kDefaultDelayMs);
    CHECK(missile_launch_delay(std::uint32_t{0}, kRange) == kDefaultDelayMs);
    CHECK(missile_launch_delay(kSecondMs, 0) == 0);
}

TEST_CASE("missile actor scale") {
    CHECK(missile_actor_scale("IT10", 1) == 1.0f);
    CHECK(missile_actor_scale("it66", 1) == 1.5f);
    CHECK(missile_actor_scale("IT74", 1) == 1.3f);
    CHECK(missile_actor_scale("IT74", 0x3c) == 0.8f);
}

TEST_CASE("cmath angle") {
    CHECK(cmath_angle512(10.0f, 0.0f) == 128.0f);
    CHECK(cmath_angle512(10.0f, 20.0f) == 37.0f); // atan(0.5) = 37.8 512ths
    CHECK(cmath_angle512(0.0f, 10.0f) == 0.0f);
}

TEST_CASE("an arrow waits for the launch, flies, homes and hits") {
    TestWorld tw;
    tw.spawns.push_back({7, {0.0f, 0.0f, 5.0f}, 3.75f, 6.0f, 1, false});
    tw.spawns.push_back({9, {0.0f, 150.0f, 5.0f}, 3.75f, 6.0f, 1, false});
    MissileWorld w = tw.world();
    // Launched along +x while the target stands along +y: the homing turns it.
    auto p = decode_missile_packet(eqemu_arrow(0.0f, 0.0f, 5.0f, 4.0f, 0.0f, 125.0f, 50.0f, 7, 9, 7, 27, "IT10"));
    REQUIRE(p);
    MissileList list;
    list.add(make_missile(*p, 1000, 700));
    list.update(1500, w);
    REQUIRE(list.missiles().size() == 1);
    CHECK_FALSE(list.missiles()[0].has_actor); // waiting for the bow
    list.update(1700, w);
    REQUIRE(list.missiles().size() == 1);
    CHECK(list.missiles()[0].has_actor);
    CHECK(list.missiles()[0].radius == 0.1f);
    CHECK(list.missiles()[0].ends_at == 7700);
    std::uint32_t now = 1700;
    float closest = 1e9f;
    while (!list.missiles().empty() && now < 9000) {
        now += 33;
        list.update(now, w);
        if (!list.missiles().empty()) {
            const auto& m = list.missiles()[0];
            closest = std::min(closest, glm::length(glm::vec2(m.position) - glm::vec2(0.0f, 150.0f)));
        }
    }
    CHECK(list.missiles().empty());
    CHECK(now < 7700); // it hit before its 6 s ran out
    CHECK(closest < 30.0f);
}

TEST_CASE("missiles end at their lifetime or on a wall") {
    TestWorld tw;
    tw.spawns.push_back({7, {0.0f, 0.0f, 5.0f}, 3.75f, 6.0f, 1, false});
    MissileWorld w = tw.world();
    auto p = decode_missile_packet(eqemu_arrow(0.0f, 0.0f, 5.0f, 1.0f, 0.0f, 125.0f, 0.0f, 7, 0, 7, 27, "IT10"));
    REQUIRE(p);
    MissileList list;
    list.add(make_missile(*p, 0, 0));
    std::uint32_t now = 0;
    while (!list.missiles().empty() && now < 20000) {
        now += 33;
        list.update(now, w);
    }
    CHECK(now > 6000);
    CHECK(now < 6100);

    tw.solid = true;
    w = tw.world();
    list.add(make_missile(*p, 0, 0));
    list.update(10, w);  // the actor
    list.update(50, w);  // the first step hits the wall
    CHECK(list.missiles().empty());

    // No source spawn: dropped.
    auto q = *p;
    q.source_id = 99;
    list.add(make_missile(q, 0, 0));
    list.update(10, w);
    CHECK(list.missiles().empty());
}

TEST_CASE("a missile with a target flies through walls until the target goes; its source going ends it") {
    TestWorld tw;
    tw.spawns.push_back({7, {0.0f, 0.0f, 5.0f}, 3.75f, 6.0f, 1, false});
    tw.spawns.push_back({9, {0.0f, 400.0f, 5.0f}, 3.75f, 6.0f, 1, false});
    tw.solid = true; // every sweep hits
    MissileWorld w = tw.world();
    auto p = decode_missile_packet(eqemu_arrow(0.0f, 0.0f, 5.0f, 4.0f, 0.0f, 125.0f, 50.0f, 7, 9, 7, 27, "IT10"));
    REQUIRE(p);
    MissileList list;
    list.add(make_missile(*p, 0, 0));
    list.update(10, w); // the actor
    for (std::uint32_t now = 50; now < 500; now += 40) list.update(now, w);
    REQUIRE(list.missiles().size() == 1); // MoveMissile sweeps only without a target
    CHECK(list.missiles()[0].target_id == 9);

    // DeleteMyMissiles: the target despawns, the missile loses it and the next sweep ends it.
    tw.spawns.pop_back();
    list.update(540, w);
    CHECK(list.missiles().empty());

    // A target not known when the missile comes: no target from the start.
    list.add(make_missile(*p, 0, 0));
    list.update(600, w);
    REQUIRE(list.missiles().size() == 1);
    CHECK(list.missiles()[0].target_id == 0);
    list.clear();

    // The source despawns in flight: the missile is deleted.
    tw.solid = false;
    tw.spawns.push_back({9, {0.0f, 400.0f, 5.0f}, 3.75f, 6.0f, 1, false});
    list.add(make_missile(*p, 0, 0));
    list.update(10, w);
    list.update(50, w);
    REQUIRE(list.missiles().size() == 1);
    tw.spawns.erase(tw.spawns.begin());
    list.update(90, w);
    CHECK(list.missiles().empty());
}

TEST_CASE("valid spell missiles launch the actual area actor and retire without a generic impact") {
    TestWorld tw;
    tw.spawns.push_back({7, {1, 2, 3}, 3.75f, 6.0f, 1, false});
    tw.spawns.push_back({9, {100, 200, 30}, 3.75f, 6.0f, 1, false});
    SpellData spell;
    spell.id = 100600;
    spell.target_type = 8;
    spell.ae_duration = 500;
    auto world = tw.world();
    world.spell = [&](std::uint32_t id) { return id == spell.id ? &spell : nullptr; };
    std::vector<int> stages;
    glm::vec3 effect_at{};
    int ended = 0;
    world.effect = [&](const Missile& m, int stage) { stages.push_back(stage); effect_at = m.actor_position; };
    world.ended = [&](const Missile&) { ++ended; };
    MissilePacket packet;
    packet.source_id = 7;
    packet.target_id = 9;
    packet.position = {10, 20, 30};
    packet.velocity = {1, 2, 3};
    packet.speed = 4;
    packet.item_type = 9;
    packet.spell = spell.id;
    MissileList missiles;
    missiles.add(make_missile(packet, 1000, 1500));
    missiles.update(1000, world); // spell missiles do not wait for a physical ranged animation
    REQUIRE(missiles.missiles().size() == 1);
    const auto& m = missiles.missiles().front();
    CHECK(m.has_actor);
    CHECK(m.position == glm::vec3(100, 200, 30));
    CHECK(effect_at == m.position);
    CHECK(m.velocity == glm::vec3(0));
    CHECK(m.speed == 0);
    CHECK(m.ends_at == 1500);
    CHECK(stages == std::vector<int>{2});
    missiles.update(1500, world);
    CHECK(missiles.missiles().size() == 1); // the original compares time strictly past the expiry
    missiles.update(1533, world);
    CHECK(missiles.missiles().empty());
    CHECK(ended == 1);
    CHECK(stages == std::vector<int>{2}); // no invented second impact at expiration

    spell.target_type = 45;
    packet.location = {300, 400, 50};
    missiles.add(make_missile(packet, 2000, 0));
    missiles.update(2000, world);
    CHECK(missiles.missiles().front().position == packet.location);
    missiles.clear();
    for (auto id : {0u, 601u, 45001u}) {
        packet.spell = id;
        missiles.add(make_missile(packet, 3000, 0));
        missiles.update(3000, world);
        CHECK(missiles.missiles().empty());
    }
}

TEST_CASE("spell projectile flight uses its duration and only spell54 emits a destruction impact") {
    TestWorld tw;
    tw.spawns.push_back({7, {1, 2, 3}, 3.75f, 6.0f, 1, false, 900u});
    tw.spawns.push_back({9, {1000, 2000, 30}, 3.75f, 6.0f, 1, false});
    SpellData spell;
    spell.id = 54;
    spell.target_type = 5;
    auto world = tw.world();
    world.spell = [&](std::uint32_t id) { return id == spell.id ? &spell : nullptr; };
    std::vector<std::pair<int, glm::vec3>> stages;
    world.effect = [&](const Missile& m, int stage) { stages.emplace_back(stage, m.actor_position); };
    MissilePacket packet;
    packet.source_id = 7;
    packet.position = {10, 20, 30};
    packet.velocity = {1, 2, 0};
    packet.item_type = 9;
    packet.spell = 54;
    MissileList missiles;
    missiles.add(make_missile(packet, 1000, 0));
    missiles.update(1000, world);
    REQUIRE(missiles.missiles().size() == 1);
    CHECK(missiles.missiles().front().position == glm::vec3(17.5f, 35.0f, 30.0f));
    CHECK(missiles.missiles().front().ends_at == 1050);
    REQUIRE(stages.size() == 1);
    CHECK(stages[0].first == 1);
    missiles.update(1066, world);
    CHECK(missiles.missiles().empty());
    REQUIRE(stages.size() == 2);
    CHECK(stages[1].first == 2);
    // The native destruction stage is one current-velocity step behind the final simulated position.
    CHECK(stages[1].second.x == doctest::Approx(17.772f));
    CHECK(stages[1].second.y == doctest::Approx(35.544f));

    spell.id = packet.spell = 600;
    spell.travel_type = 3;
    packet.velocity.z = 5;
    missiles.add(make_missile(packet, 2000, 0));
    missiles.update(2000, world);
    REQUIRE(missiles.missiles().size() == 1);
    CHECK(missiles.missiles().front().position == packet.position);
    CHECK(missiles.missiles().front().velocity.z == 0);
    CHECK(missiles.missiles().front().ends_at == 32000);
    tw.solid = true;
    packet.target_id = 9;
    missiles.clear();
    missiles.add(make_missile(packet, 3000, 0));
    missiles.update(3000, world);
    missiles.update(3033, world);
    CHECK(missiles.missiles().empty()); // a spell projectile sweeps even when it has a target

    tw.solid = false;
    tw.spawns[1].position = {10, 0, 0};
    Missile crossing = make_missile(packet, 4000, 0);
    crossing.has_actor = true;
    crossing.actor_position = {0, 0, 0};
    crossing.position = {20, 0, 0};
    crossing.velocity = {1, 0, 0};
    crossing.target_id = 9;
    crossing.ends_at = 10000;
    int sweeps = 0;
    world.sweep = [&](const glm::vec3&, const glm::vec3&, float) { ++sweeps; return false; };
    CHECK_FALSE(move_missile(crossing, 4000, world));
    CHECK(crossing.done); // a spell with a target checks that target after its clear terrain sweep
    CHECK(sweeps == 1);
}

TEST_CASE("a spell missile's launch extrapolation ignores a future source step and caps a stale one") {
    CHECK(spell_launch_steps(std::nullopt, 5000) == 0.0f);
    CHECK(spell_launch_steps(4900u, 5000) == doctest::Approx(2.0f));
    CHECK(spell_launch_steps(5000u, 5000) == 0.0f);
    CHECK(spell_launch_steps(5001u, 5000) == 0.0f);      // not 2^32 - 1 ms of steps
    CHECK(spell_launch_steps(1000u, 100000) == doctest::Approx(240.0f)); // 12 s at most
    CHECK(spell_launch_steps(0xfffff000u, 0x1000u) == doctest::Approx(163.84f)); // across the clock's wrap

    TestWorld tw;
    tw.spawns.push_back({7, {1, 2, 3}, 3.75f, 6.0f, 1, false, 6000u}); // stepped after the missile's now
    SpellData spell;
    spell.id = 600;
    spell.target_type = 5;
    auto world = tw.world();
    world.spell = [&](std::uint32_t id) { return id == spell.id ? &spell : nullptr; };
    MissilePacket packet;
    packet.source_id = 7;
    packet.position = {10, 20, 30};
    packet.velocity = {1, 2, 0};
    packet.item_type = 9;
    packet.spell = 600;
    MissileList missiles;
    missiles.add(make_missile(packet, 5000, 0));
    missiles.update(5000, world);
    REQUIRE(missiles.missiles().size() == 1);
    CHECK(missiles.missiles().front().position == glm::vec3(15.5f, 31.0f, 30.0f)); // 5.5 steps, none extra
    tw.spawns[0].stepped_at = 1000u; // a minute stale
    missiles.clear();
    missiles.add(make_missile(packet, 61000, 0));
    missiles.update(61000, world);
    REQUIRE(missiles.missiles().size() == 1);
    CHECK(missiles.missiles().front().position == glm::vec3(255.5f, 511.0f, 30.0f)); // 5.5 + 240 steps
}

TEST_CASE("stationary spell contacts use moving-area and local line-of-sight gates at their native cadence") {
    TestWorld tw;
    tw.spawns.push_back({7, {0, 0, 0}, 4, 6, 1, false});
    tw.spawns.push_back({9, {5, 0, 0}, 4, 6, 8, false});
    tw.spawns.push_back({10, {6, 0, 0}, 4, 6, 1, false}); // stationary: excluded by SpeedRun, not life state
    tw.spawns.push_back({11, {20, 0, 0}, 4, 6, 1, false}); // blocked
    tw.spawns.push_back({12, {0, 0, 25.1f}, 4, 6, 1, false}); // outside the 3D sphere
    tw.spawns.push_back({15, {5, 0, 0}, 4, 6, 1, false}); // rider gets its mount's notification
    for (auto i : {1u, 3u, 4u}) tw.spawns[i].speed_run = 1.0f;
    tw.spawns[1].rider_id = 15;
    SpellData spell;
    spell.id = 600;
    spell.ae_range = 10;
    spell.ae_duration = 10000;
    spell.target_type = 45;
    spell.effect.fill(254);
    spell.effect[0] = 76;
    auto world = tw.world();
    world.spell = [&](std::uint32_t id) { return id == spell.id ? &spell : nullptr; };
    world.spawns = [&] { return tw.spawns; };
    world.local_id = 9;
    bool line_clear = true;
    bool checked_small_race_height = false;
    world.visible = [&](const glm::vec3& from, const glm::vec3&) {
        if (from.x == 5) checked_small_race_height = from.z == 1.0f;
        return line_clear && from.x != 20;
    };
    std::vector<std::uint32_t> contacts;
    world.contact = [&](const Missile&, const MissileSpawn& target) { contacts.push_back(target.id); };
    MissilePacket packet;
    packet.source_id = 7;
    packet.item_type = 9;
    packet.spell = 600;
    MissileList missiles;
    missiles.add(make_missile(packet, 1000, 0));
    missiles.update(1000, world);
    CHECK(contacts.empty());
    missiles.update(1033, world);
    CHECK(contacts == std::vector<std::uint32_t>{15});
    CHECK(checked_small_race_height);
    missiles.update(3033, world);
    CHECK(contacts.size() == 1); // strict: next_contact < now, not <=
    missiles.update(3066, world);
    CHECK(contacts == std::vector<std::uint32_t>{15, 15});
    REQUIRE(missiles.missiles().size() == 1);
    CHECK(missiles.missiles().front().area_contact);

    missiles.clear();
    contacts.clear();
    spell.effect.fill(254);
    tw.spawns[1].rider_id = 0;
    line_clear = false;
    missiles.add(make_missile(packet, 4000, 0));
    missiles.update(4000, world);
    missiles.update(4033, world);
    CHECK(contacts.empty());
    spell.npc_no_los = true;
    missiles.update(4066, world);
    CHECK(contacts.empty()); // the bypass is restricted to an NPC source
    tw.spawns[0].is_npc = true;
    missiles.update(4099, world);
    CHECK(contacts == std::vector<std::uint32_t>{9});
    missiles.update(7099, world);
    CHECK(contacts.size() == 1);
    missiles.update(7132, world);
    CHECK(contacts == std::vector<std::uint32_t>{9, 9});
    CHECK_FALSE(missiles.missiles().front().area_contact); // these attempts never become SPA76 contact text
}

TEST_CASE("large-race spell missiles use the authored role-zero origin without inventing a missing point") {
    TestWorld tw;
    tw.spawns.push_back({7, {1, 2, 3}, 4, 6, 49, false, 1000u});
    SpellData spell;
    spell.id = 600;
    spell.target_type = 5;
    spell.travel_type = 3;
    auto world = tw.world();
    world.spell = [&](std::uint32_t id) { return id == 600 ? &spell : nullptr; };
    int queries = 0;
    world.launch_point = [&](std::uint32_t id) -> std::optional<glm::vec3> {
        ++queries;
        CHECK(id == 7);
        return glm::vec3(100, 200, 300);
    };
    MissilePacket packet;
    packet.source_id = 7;
    packet.item_type = 9;
    packet.spell = 600;
    packet.position = {10, 20, 30};
    packet.velocity = {1, 2, 3};
    packet.speed = 4;
    packet.range = 1;
    Missile m = make_missile(packet, 1000, 0);
    process_missile(m, 1000, world);
    REQUIRE(m.has_actor);
    CHECK(queries == 1);
    CHECK(m.actor_position == glm::vec3(132.5f, 265.0f, 288.0f));
    CHECK(m.pitch == -72.0f);
    CHECK(m.velocity.z == 0.0f);
    CHECK(m.radius == 1.0f);
    world.launch_point = [](std::uint32_t) { return std::optional<glm::vec3>{}; };
    m = make_missile(packet, 1000, 0);
    process_missile(m, 1000, world);
    CHECK(m.actor_position == packet.position); // safest fallback for unavailable authored/posed role data
}
