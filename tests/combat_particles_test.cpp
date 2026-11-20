#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "app/actor_emitters.h"
#include "app/world_objects.h"
#include "app/spell_particles.h"
#include "assets/character.h"
#include "assets/object_animation.h"
#include "core/byte_io.h"
#include "core/process_serial.h"
#include "core/worker_pool.h"
#include "game/collision.h"
#include "game/mechanics/anim_select.h"
#include "game/missiles.h"
#include "game/path_trail.h"
#include "game/spells.h"
#include "mock_server/harness.h"
#include "net/movement_packets.h"
#include "net/spell_packets.h"
#include "net/zone_packets.h"

using namespace hailstone;

namespace {

assets::EmitterDefinition dot() {
    assets::EmitterDefinition d;
    d.duration = 1.0f;
    d.lifetime = 2.0f;
    d.initial_count = 1;
    d.per_tick = 0;
    d.max_opacity = 1.0f;
    return d;
}

bool near(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-4f; }

std::string spell_line(std::uint32_t id, unsigned target_type = 0, unsigned ae_duration = 0, bool npc_no_los = false,
                       int first_effect = 254, bool lasting_buff = false) {
    std::vector<std::string> columns(237, "0");
    columns[0] = std::to_string(id);
    columns[1] = "Fixture spell";
    columns[145] = "1";
    columns[98] = std::to_string(target_type);
    columns[18] = std::to_string(ae_duration);
    columns[159] = npc_no_los ? "1" : "0";
    for (std::size_t i = 86; i <= 97; ++i) columns[i] = "254";
    columns[86] = std::to_string(first_effect);
    if (lasting_buff) {
        columns[16] = "7";
        columns[17] = "10";
        columns[83] = "1";
        columns[20] = "10";
        columns[70] = "100";
        for (std::size_t i = 104; i < 120; ++i) columns[i] = "255";
        columns[105] = "1";
    }
    std::string line;
    for (const auto& c : columns) line += (line.empty() ? "" : "^") + c;
    return line;
}

} // namespace

static_assert(!std::is_constructible_v<app::SpellParticles, render::VkContext&, const GameDir&,
                                       const std::filesystem::path&>,
              "The GPU runtime must be given the current collision source at construction");

TEST_CASE("Find trails bind collision at startup and follow zone replacements without an asset reload") {
    test::Rig rig;
    std::optional<game::CollisionMesh> collision;
    auto source = [&]() -> const game::CollisionMesh* { return collision ? &*collision : nullptr; };
    auto ground = [](float z) {
        const glm::vec3 a(-10, -10, z), b(-10, 10, z), c(100, 10, z), d(100, -10, z);
        return game::CollisionMesh::from_triangles({game::CollisionTriangle::make(a, b, c, 0),
                                                    game::CollisionTriangle::make(a, c, d, 0)});
    };
    app::SpellParticles particles({assets::EmitterDefinition{}}, {}, source);
    particles.set_guide_line({{0, 0, 0}, {60, 0, 0}});
    auto hidden = [&](app::SpellParticles& runtime) {
        for (const auto style : {game::FindTrailStyle::Navigation, game::FindTrailStyle::Rainbow}) {
            runtime.set_find_trail_style(style);
            runtime.update(rig.conn, {}, {}, nullptr, 0.01f);
            CHECK(runtime.simulation().emitters().empty());
        }
    };
    auto shown = [&](app::SpellParticles& runtime, float floor) {
        for (const auto style : {game::FindTrailStyle::Navigation, game::FindTrailStyle::Rainbow}) {
            CAPTURE(static_cast<int>(style));
            CAPTURE(floor);
            runtime.set_find_trail_style(style);
            runtime.update(rig.conn, {}, {}, nullptr, 0.01f);
            const auto& emitters = runtime.simulation().emitters();
            REQUIRE(emitters.size() == 1);
            REQUIRE(emitters.front().still);
            REQUIRE(emitters.front().particles.size() > 100);
            for (const auto& p : emitters.front().particles) {
                const float clearance = p.position.z - floor;
                CHECK(clearance >= (style == game::FindTrailStyle::Navigation ? 0.3f : 2.0f));
                CHECK(clearance <= (style == game::FindTrailStyle::Navigation ? 0.4f : 4.0f));
            }
        }
    };
    hidden(particles); // construction precedes the first zone's collision
    collision = ground(0);
    REQUIRE(collision->ground_below({0, 0, 0.5f}) == std::optional(0.0f));
    shown(particles, 0); // no set_floor/set_trail_floor or asset reload is needed

    collision.reset();
    hidden(particles); // zoning takes the old terrain away, including the old trail
    collision = ground(20);
    hidden(particles); // the previous zone's route is unsupported in this zone
    particles.set_guide_line({{0, 0, 20}, {60, 0, 20}});
    shown(particles, 20);

    app::SpellParticles replacement({assets::EmitterDefinition{}}, {}, source);
    replacement.set_guide_line({{0, 0, 20}, {60, 0, 20}});
    shown(replacement, 20); // the asset-reload construction path uses the same binding
    collision.reset();
    hidden(particles);
    hidden(replacement);
    collision = ground(-25);
    hidden(replacement); // no cached floor from the zone at z=20
    replacement.set_guide_line({{0, 0, -25}, {60, 0, -25}});
    shown(replacement, -25);
}

TEST_CASE("Find's continuous rainbow strands retain terrain clearance and motion through the runtime") {
    test::Rig rig;
    app::SpellParticles particles({assets::EmitterDefinition{}}, {});
    particles.set_find_trail_style(game::FindTrailStyle::Rainbow);
    const std::vector<glm::vec3> line{{0, 0, 0}, {60, 0, 0}};
    particles.set_guide_line(line);
    // A spell's attachment floor is a separate query and cannot supply the Find trail's surface.
    particles.set_floor([](const glm::vec3&) -> std::optional<float> { return -1000.0f; });
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    CHECK(particles.simulation().emitters().empty());

    auto height = [](float x) { return 12.0f * std::sin(x * 3.14159265f / 60.0f); };
    bool supported = true;
    particles.set_trail_floor([&](const glm::vec3& p) -> std::optional<float> {
        const float z = height(p.x);
        return supported && z <= p.z ? std::optional(z) : std::nullopt;
    });
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    const auto& emitters = particles.simulation().emitters();
    REQUIRE(emitters.size() == 1);
    CHECK(emitters.front().still);
    const auto& definition = particles.simulation().definitions().at(emitters.front().definition);
    CHECK(definition.texture.empty());
    CHECK(definition.billboard == game::kTrailStrandBillboard);
    CHECK(definition.additive == 1);
    REQUIRE(emitters.front().particles.size() > 200);
    bool red = false, green = false, blue = false, on_hill = false;
    for (const auto& p : emitters.front().particles) {
        const float clearance = p.position.z - height(p.position.x);
        CHECK(clearance >= 2.0f);
        CHECK(clearance <= 4.0f);
        CHECK(p.width > 0.0f);
        CHECK(p.width <= 0.1f);
        CHECK(p.height > p.width * 2.0f);
        CHECK(glm::length(p.axes[1]) == doctest::Approx(1.0f));
        red |= p.color.r > 0.8f && p.color.g < 0.5f && p.color.b < 0.5f;
        green |= p.color.g > 0.8f && p.color.r < 0.5f && p.color.b < 0.5f;
        blue |= p.color.b > 0.8f && p.color.r < 0.5f && p.color.g < 0.5f;
        on_hill |= p.position.z > 10.0f;
    }
    CHECK(red);
    CHECK(green);
    CHECK(blue);
    CHECK(on_hill);

    std::vector<glm::vec3> before;
    for (const auto& p : emitters.front().particles) before.push_back(p.position);
    particles.update(rig.conn, {}, {}, nullptr, 0.5f);
    REQUIRE(emitters.size() == 1);
    bool moved = false;
    for (std::size_t i = 0; i < std::min(before.size(), emitters.front().particles.size()); ++i) {
        const auto& p = emitters.front().particles[i];
        moved |= glm::length(p.position - before[i]) > 0.02f;
        const float clearance = p.position.z - height(p.position.x);
        CHECK(clearance >= 2.0f);
        CHECK(clearance <= 4.0f);
    }
    CHECK(moved);

    supported = false;
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    CHECK(particles.simulation().emitters().empty());
    supported = true;
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    REQUIRE(particles.simulation().emitters().size() == 1);
    particles.set_guide_line({});
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    CHECK(particles.simulation().emitters().empty());
}

TEST_CASE("Server Find and guide trails each use rainbow particles and stop independently") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    rig.conn.set_path_floor([](const glm::vec3& p, float step_up) -> std::optional<float> {
        return p.z + step_up >= 2.0f ? std::optional(2.0f) : std::nullopt;
    });
    app::SpellParticles particles({assets::EmitterDefinition{}}, {});
    particles.set_find_trail_style(game::FindTrailStyle::Rainbow);
    particles.set_trail_floor([](const glm::vec3& p) -> std::optional<float> {
        return p.z >= 2.0f ? std::optional(2.0f) : std::nullopt;
    });
    particles.set_guide_line({{0, 0, 2}, {60, 0, 2}});
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    REQUIRE(particles.simulation().emitters().size() == 1);
    const auto guide_key = particles.simulation().emitters().front().key;

    rig.conn.find_map_location({0, 60, 2});
    rig.conn.receive_zone_packet_for_test(net::encode_find_person_reply({{0, 60, 2}, {0, 0, 2}, {0, 60, 2}}));
    REQUIRE(rig.conn.player_path().active());
    REQUIRE_FALSE(rig.conn.player_path().points().empty());
    CHECK(rig.conn.player_path().points().front().z - rig.conn.player_path().floor_offset() == doctest::Approx(2.0f));
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    REQUIRE(particles.simulation().emitters().size() == 2);
    for (const auto& emitter : particles.simulation().emitters()) {
        CHECK(emitter.still);
        bool red = false, blue = false;
        for (const auto& p : emitter.particles) {
            red |= p.color.r > 0.8f && p.color.g < 0.5f && p.color.b < 0.5f;
            blue |= p.color.b > 0.8f && p.color.r < 0.5f && p.color.g < 0.5f;
        }
        CHECK(red);
        CHECK(blue);
    }
    rig.conn.stop_find_path();
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    REQUIRE(particles.simulation().emitters().size() == 1);
    CHECK(particles.simulation().emitters().front().key == guide_key);
    particles.set_guide_line({});
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    CHECK(particles.simulation().emitters().empty());
}

TEST_CASE("Find trail visibility retains both routes and keeps ordinary spell effects running") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    rig.conn.set_path_floor([](const glm::vec3& p, float step_up) -> std::optional<float> {
        return p.z + step_up >= 2.0f ? std::optional(2.0f) : std::nullopt;
    });
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_line(600)) == 1);
    rig.conn.set_spell_table_for_test(table);
    assets::SpellEffect effect;
    effect.stages[0].slots[0] = {1, 0, 1, 8}; // ordinary cast effect attached to the player
    app::SpellParticles particles({assets::EmitterDefinition{}, dot()}, {assets::SpellEffect{}, effect});
    particles.set_trail_floor([](const glm::vec3& p) -> std::optional<float> {
        return p.z >= 2.0f ? std::optional(2.0f) : std::nullopt;
    });
    particles.set_guide_line({{0, 0, 2}, {60, 0, 2}});
    rig.conn.find_map_location({0, 60, 2});
    rig.conn.receive_zone_packet_for_test(net::encode_find_person_reply({{0, 60, 2}, {0, 0, 2}, {0, 60, 2}}));
    REQUIRE(rig.conn.player_path().active());
    const auto points = rig.conn.player_path().points();
    net::SpellEffectCue cast;
    cast.caster = rig.conn.world().player_id; cast.spell = 600; cast.duration_ms = 1000;
    particles.update(rig.conn, std::span(&cast, 1), {}, nullptr, 0.01f);
    const auto& emitters = particles.simulation().emitters();
    REQUIRE(emitters.size() == 3);
    CHECK(std::count_if(emitters.begin(), emitters.end(), [](const auto& emitter) { return emitter.still; }) == 2);
    const auto spell_key = (cast.caster << 16) | cast.spell;
    particles.set_paths_visible(false);
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    REQUIRE(emitters.size() == 1);
    CHECK(emitters.front().key == spell_key);
    CHECK_FALSE(emitters.front().still);
    REQUIRE_FALSE(emitters.front().particles.empty());
    const auto age = emitters.front().age;
    particles.update(rig.conn, {}, {}, nullptr, 0.02f);
    REQUIRE(emitters.size() == 1);
    CHECK(emitters.front().age > age);
    CHECK(rig.conn.player_path().active());
    CHECK(rig.conn.player_path().points() == points);
    particles.set_paths_visible(true);
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    REQUIRE(emitters.size() == 3); // guide input and server route both reappear without being replaced
    CHECK(std::count_if(emitters.begin(), emitters.end(), [](const auto& emitter) { return emitter.still; }) == 2);
    CHECK(std::any_of(emitters.begin(), emitters.end(), [&](const auto& emitter) { return emitter.key == spell_key; }));
    CHECK(rig.conn.player_path().points() == points);
}

TEST_CASE("Find's rainbow replaces elevated travelling clouds and grounds large avatars and finish effects") {
    test::Rig rig;
    std::filesystem::create_directories(rig.dir.path() / "SpellEffects");
    {
        std::ofstream ini(rig.dir.path() / "SpellEffects" / "Path.ini");
        ini << "[Path]\nFloorOffset=1\nMaxEmitterCount=1\nEmitterPeriod=0\nEmitterSpeed=0\n"
               "EmitterStartCoordinate=-30\nEmitter0=Cloud\nFinishEmitter0=Cloud\n"
               "[Cloud]\nDefinition=1\nLifeSpan=1000\n";
        REQUIRE(ini.good());
    }
    rig.game.rescan();
    REQUIRE(rig.to_zone());
    auto& world = rig.conn.world_for_test();
    auto& me = world.spawns.at(world.player_id);
    me.gm = false;
    me.heights.avatar_height = 12.0f;
    me.position = {0, 0, 14};
    rig.conn.set_player_motion(me.position, 0, {}, false);
    game::Spawn target;
    target.id = 333;
    target.incarnation = core::next_process_identity();
    target.name = "FixtureGuide";
    target.is_npc = true;
    target.position = {60, 0, 14};
    world.spawns.emplace(target.id, target);
    ++world.spawn_set_serial;
    auto ground = [](const glm::vec3& p, float step_up) -> std::optional<float> {
        return p.z + step_up >= 2.0f ? std::optional(2.0f) : std::nullopt;
    };
    rig.conn.set_path_floor(ground);
    REQUIRE(rig.conn.find_spawn(333));
    REQUIRE(rig.conn.player_path().floor_offset() == 12.0f);
    REQUIRE(rig.conn.player_path().settings().emitters[0].definition == 1);
    rig.conn.receive_zone_packet_for_test(net::encode_find_person_reply({{60, 0, 14}, {0, 0, 14}, {60, 0, 14}}));
    // Drive this owned component's frame clock explicitly; the connection's session clock does not drive it.
    auto& path = const_cast<game::PlayerPath&>(rig.conn.player_path());
    const auto tick = net::GameConnection::eq_time();
    path.update({0, 0, 14}, tick + 1, rig.conn.zone_id(), false, ground);
    path.update({0, 0, 14}, tick + 2, rig.conn.zone_id(), false, ground);
    REQUIRE(path.active());
    REQUIRE_FALSE(path.movers().empty());
    REQUIRE(std::any_of(path.movers().begin(), path.movers().end(),
                        [&](const auto& mover) { return path.mover_position(mover).has_value(); }));
    REQUIRE(path.points().front().z == doctest::Approx(14.0f));

    bool supported = true;
    app::SpellParticles particles({assets::EmitterDefinition{}, dot()}, {});
    particles.set_find_trail_style(game::FindTrailStyle::Rainbow);
    particles.set_trail_floor([&](const glm::vec3& p) -> std::optional<float> {
        return supported && p.z >= 2.0f ? std::optional(2.0f) : std::nullopt;
    });
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    const auto& emitters = particles.simulation().emitters();
    REQUIRE(emitters.size() == 1); // the configured moving cloud is replaced by thin, connected rainbow segments
    REQUIRE(emitters.front().still);
    REQUIRE(emitters.front().particles.size() > 100);
    for (const auto& p : emitters.front().particles) {
        CHECK(p.position.z >= 4.0f);
        CHECK(p.position.z <= 6.0f);
        CHECK(p.width <= 0.1f);
        CHECK(p.height > p.width * 2.0f);
    }

    bool visible = true;
    SUBCASE("completion effects start on supported ground") { supported = true; }
    SUBCASE("unsupported completion effects are omitted") { supported = false; }
    SUBCASE("hidden completion effects are consumed without replay on reveal") {
        visible = false; particles.set_paths_visible(false);
    }
    rig.conn.set_player_motion({60, 0, 14}, 0, {}, false);
    rig.conn.update();
    REQUIRE_FALSE(path.active());
    REQUIRE(path.finished());
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    if (supported && visible) {
        REQUIRE(emitters.size() == 1);
        CHECK_FALSE(emitters.front().still);
        CHECK(near(emitters.front().origin, {60, 0, 2.35f}));
        particles.set_paths_visible(false);
        particles.update(rig.conn, {}, {}, nullptr, 0.01f);
        CHECK(emitters.empty()); // hide an already-playing completion emitter too
    } else {
        CHECK(emitters.empty());
    }
    if (!visible) {
        CHECK_FALSE(rig.conn.take_path_finish());
        particles.set_paths_visible(true);
        particles.update(rig.conn, {}, {}, nullptr, 0.01f);
        CHECK(emitters.empty());
        CHECK(path.finished());
    }
}

TEST_CASE("Find defaults to a bordered navigation line and style changes replace the active geometry") {
    test::Rig rig;
    app::SpellParticles particles({assets::EmitterDefinition{}}, {});
    CHECK(particles.find_trail_style() == game::FindTrailStyle::Navigation);
    particles.set_trail_floor([](const glm::vec3& p) -> std::optional<float> {
        return p.z >= 2.0f ? std::optional(2.0f) : std::nullopt;
    });
    particles.set_guide_line({{0, 0, 2}, {60, 0, 2}});
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    const auto& simulation = particles.simulation();
    const auto& emitters = simulation.emitters();
    REQUIRE(emitters.size() == 1);
    const auto key = emitters.front().key;
    const auto navigation_definition = emitters.front().definition;
    auto check_navigation = [&] {
        REQUIRE(emitters.size() == 1);
        CHECK(emitters.front().key == key);
        CHECK(emitters.front().still);
        CHECK(emitters.front().definition == navigation_definition);
        const auto& definition = simulation.definitions().at(emitters.front().definition);
        CHECK(definition.additive == 0);
        CHECK(definition.billboard == game::kWorldBasisBillboard);
        bool gold = false, border = false;
        REQUIRE_FALSE(emitters.front().particles.empty());
        for (const auto& p : emitters.front().particles) {
            gold |= p.color.r > 0.7f && p.color.g > 0.4f && p.color.b < 0.35f;
            border |= p.color.r < 0.2f && p.color.g < 0.2f && p.color.b < 0.2f;
            CHECK_FALSE((p.color.b > 0.8f && p.color.r < 0.5f && p.color.g < 0.5f));
            CHECK(p.position.z == doctest::Approx(2.35f).epsilon(0.02));
        }
        CHECK(gold);
        CHECK(border);
    };
    check_navigation();
    for (int pass = 0; pass < 2; ++pass) {
        particles.set_find_trail_style(game::FindTrailStyle::Rainbow);
        particles.update(rig.conn, {}, {}, nullptr, 0.01f);
        REQUIRE(emitters.size() == 1);
        CHECK(emitters.front().key == key);
        CHECK(emitters.front().definition != navigation_definition);
        CHECK(simulation.definitions().at(emitters.front().definition).additive == 1);
        CHECK(simulation.definitions().at(emitters.front().definition).texture.empty());
        CHECK(simulation.definitions().at(emitters.front().definition).billboard == game::kTrailStrandBillboard);
        bool blue = false;
        for (const auto& p : emitters.front().particles) {
            blue |= p.color.b > 0.8f && p.color.r < 0.5f && p.color.g < 0.5f;
            CHECK_FALSE((p.color.r < 0.2f && p.color.g < 0.2f && p.color.b < 0.2f));
        }
        CHECK(blue);
        particles.set_find_trail_style(game::FindTrailStyle::Navigation);
        particles.update(rig.conn, {}, {}, nullptr, 0.01f);
        check_navigation();
    }
    particles.set_guide_line({});
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    CHECK(emitters.empty());
}

TEST_CASE("Spell landing packets use the source actor and its level for impact-stage emitters") {
    using namespace net;
    using namespace test;
    Rig rig;
    REQUIRE(rig.to_zone());
    auto caster = make_spawn("FixtureCaster", 333, 1, 1, 50, 60, 7);
    caster.level = 50;
    auto struck = make_spawn("FixtureTarget", 334, 1, 1, 80, 90, 8);
    struck.level = 1;
    rig.server.add_spawn(caster);
    rig.server.add_spawn(struck);
    REQUIRE(rig.until([&] { return rig.conn.world().spawns.contains(333) && rig.conn.world().spawns.contains(334); }));
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_line(600)) == 1);
    rig.conn.set_spell_table_for_test(table);
    rig.conn.take_spell_effect_cues();
    Action hit;
    hit.source = 333;
    hit.target = 334;
    hit.type = 231;
    hit.spell_id = 600;
    hit.spell_level = 50;
    hit.effect_flag = 4;
    rig.conn.receive_zone_packet_for_test(encode(hit));
    auto cues = rig.conn.take_spell_effect_cues();
    REQUIRE(std::any_of(cues.begin(), cues.end(), [](const auto& c) { return c.stage == 2; }));
    for (auto& cue : cues)
        if (cue.stage == 2) cue.delay = 0.05;
    assets::SpellEffect effect;
    effect.stages[2].slots[0] = {1, 20, 1, 8}; // caster-level gate and caster attachment
    effect.stages[2].slots[1] = {1, 0, 3, 8};  // separately attach to the struck actor
    app::SpellParticles particles({assets::EmitterDefinition{}, dot()}, {assets::SpellEffect{}, effect});
    particles.update(rig.conn, cues, {}, nullptr, 0.01f);
    CHECK(particles.simulation().emitters().empty());
    particles.update(rig.conn, {}, {}, nullptr, 0.04f);
    CHECK(particles.simulation().emitters().empty());
    particles.update(rig.conn, {}, {}, nullptr, 0.02f);
    const auto& emitters = particles.simulation().emitters();
    REQUIRE(emitters.size() == 2);
    CHECK(emitters[0].actor == 333);
    CHECK(emitters[1].actor == 334);
    CHECK(emitters[0].key == ((333u << 16) | 600u));
    CHECK(near(emitters[0].origin, rig.conn.world().spawns.at(333).position));
    CHECK(near(emitters[1].origin, rig.conn.world().spawns.at(334).position));
    REQUIRE(emitters[0].particles.size() == 1);
    particles.clear();
    CHECK(particles.simulation().emitters().empty());
}

TEST_CASE("A spell landing without a known source plays no impact stage, as DoSpellEffect without a caster") {
    using namespace net;
    using namespace test;
    Rig rig;
    REQUIRE(rig.to_zone());
    auto struck = make_spawn("FixtureTarget", 334, 1, 1, 80, 90, 8);
    struck.level = 50;
    rig.server.add_spawn(struck);
    REQUIRE(rig.until([&] { return rig.conn.world().spawns.contains(334); }));
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_line(600)) == 1);
    rig.conn.set_spell_table_for_test(table);
    assets::SpellEffect effect;
    effect.stages[2].slots[0] = {1, 20, 1, 8}; // caster-level gate and caster attachment
    effect.stages[2].slots[1] = {1, 0, 3, 8};
    for (std::uint16_t source : {std::uint16_t{0}, std::uint16_t{999}}) {
        CAPTURE(source);
        rig.conn.take_spell_effect_cues();
        Action hit;
        hit.source = source;
        hit.target = 334;
        hit.type = 231;
        hit.spell_id = 600;
        hit.effect_flag = 4;
        rig.conn.receive_zone_packet_for_test(encode(hit));
        app::SpellParticles particles({assets::EmitterDefinition{}, dot()}, {assets::SpellEffect{}, effect});
        particles.update(rig.conn, rig.conn.take_spell_effect_cues(), {}, nullptr, 0.01f);
        CHECK(particles.simulation().emitters().empty()); // not even the slot on the struck spawn
    }
}

TEST_CASE("Attached spell particles take the actor's scale once and update their owner opacity") {
    auto d = dot();
    d.follow_attachment = 1;
    d.actor_scales_offsets = 1;
    d.offset = {0, 1, 0}; // transverse 1
    game::ParticleSystem particles({assets::EmitterDefinition{}, d});
    game::SpellEmitterRequest request;
    request.emitter = 1;
    request.actor = 7;
    request.position = {10, 20, 30};
    request.opacity = 0.5f;
    REQUIRE(particles.create(request) > 0);
    game::EmitterAttachment attachment;
    // A large race: its scale is in its world matrix's rows and in its GetScaleFactor. The offset scales by the
    // latter alone; the basis is only the direction (the rows' scale is not known to reach it, docs/unknowns.md).
    attachment.actor_axes = glm::mat3(glm::vec3(2, 0, 0), glm::vec3(0, 2, 0), glm::vec3(0, 0, 2));
    attachment.actor_scale = 2.0f;
    attachment.alpha = 0.25f;
    auto resolve = [&](std::uint32_t, std::int32_t) { return std::optional(attachment); };
    particles.update(0.01f, resolve);
    const auto& first = particles.emitters().front().particles.front();
    CHECK(near(first.position, {12, 20, 30})); // not 14: the scale once
    CHECK(first.color.a == doctest::Approx(0.125f));
    (*attachment.actor_axes)[0] = {5, 0, 0};
    attachment.alpha = 0.75f;
    particles.update(0.01f, resolve);
    const auto& second = particles.emitters().front().particles.front();
    CHECK(near(second.position, {12, 20, 30}));
    CHECK(second.color.a == doctest::Approx(0.375f));
}

TEST_CASE("Actor emitters use point-owner PESFactor, keep floor entries on the actor and try an entry once a mesh") {
    auto d = dot();
    d.duration = 0;
    d.follow_attachment = 1;
    d.bone_orientation = 1;
    d.actor_scales_size = 1;
    d.actor_scales_offsets = 0;
    assets::CharacterMesh mesh;
    assets::MeshEmitter attached;
    attached.bone = 0;
    attached.emitter.emitter = 1;
    attached.model_code = "IT42";
    mesh.emitters.push_back(attached);
    auto floor = attached;
    floor.emitter.floor = 1;
    floor.local = glm::translate(glm::mat4(1.0f), glm::vec3(2, 0, 0));
    mesh.emitters.push_back(floor);
    auto missing = attached; // an index past the file's end makes nothing
    missing.bone = 1;
    missing.emitter.emitter = 5;
    mesh.emitters.push_back(missing);
    auto heights = game::parse_height_tables({}, "[IT42]\nPESFactor=3\n");
    app::ActorEmitters particles({assets::EmitterDefinition{}, d}, heights);
    glm::mat4 world = glm::translate(glm::mat4(1), glm::vec3(100, 20, 30));
    world = glm::scale(world, glm::vec3(2, 4, 6));
    app::ActorEmitters::Actor actor{7, &mesh, false, 0.25f};
    int missing_tries = 0;
    auto pose = [&](std::uint32_t id, int bone, glm::mat4& out) {
        if (id != 7) return false;
        if (bone == 1) ++missing_tries;
        out = world;
        return bone == 0 || bone == 1;
    };
    particles.update(0.01f, std::span(&actor, 1), pose, {});
    REQUIRE(particles.simulation().emitters().size() == 2);
    const auto& attached_particle = particles.simulation().emitters()[0].particles.front();
    CHECK(attached_particle.size_factor == doctest::Approx(6.0f)); // first row length 2 x item PESFactor 3
    CHECK(attached_particle.color.a == doctest::Approx(0.25f));
    CHECK(near(particles.simulation().emitters()[0].axes[2], {0, 0, 1}));
    // The floor entry: what makes the original's again as the actor moves was not traced, so it stays on the actor.
    CHECK(particles.simulation().emitters()[1].actor == 7);
    CHECK(near(particles.simulation().emitters()[1].origin, {104, 20, 30}));
    world[3].x += 20;
    actor.alpha = 0.5f;
    particles.update(0.01f, std::span(&actor, 1), pose, {});
    particles.update(0.01f, std::span(&actor, 1), pose, {});
    CHECK(near(particles.simulation().emitters()[0].origin, {120, 20, 30}));
    CHECK(near(particles.simulation().emitters()[1].origin, {124, 20, 30}));
    CHECK(particles.simulation().emitters()[0].particles.front().color.a == doctest::Approx(0.5f));
    CHECK(missing_tries == 1); // not tried again each frame
    mesh.serial = {};           // another look: its entries are tried afresh
    particles.update(0.01f, std::span(&actor, 1), pose, {});
    CHECK(missing_tries == 2);
    particles.update(0.01f, {}, pose, {});
    CHECK(particles.simulation().emitters().empty());
}

TEST_CASE("WLD item clouds follow their held node without an actor EDD file and leave with the mesh") {
    auto definition = std::make_shared<assets::EmitterDefinition>(dot());
    definition->duration = -9999.0f;
    definition->follow_attachment = 1;
    definition->bone_orientation = 1;
    definition->actor_scales_size = 1;
    definition->scale2 = definition->max_opacity = 0.5f;
    definition->per_tick = 1;
    definition->ticks_per_second = 10.0f;
    definition->texture = "fixture.dds";
    assets::CharacterMesh mesh;
    assets::MeshEmitter cloud;
    cloud.bone = 2;
    cloud.local = glm::translate(glm::mat4(1.0f), glm::vec3(3, 0, 0));
    cloud.model_code = "IT42";
    cloud.cloud = definition;
    mesh.emitters.push_back(cloud);
    auto heights = game::parse_height_tables({}, "[IT42]\nPESFactor=3\n");
    app::ActorEmitters particles({}, heights); // WLD clouds do not need ActorEmittersNew.edd
    particles.set_density(0.0f);               // the EDD list's setting does not disable the WLD list
    glm::mat4 world = glm::translate(glm::mat4(1), glm::vec3(100, 20, 30));
    world = glm::scale(world, glm::vec3(2));
    std::array<app::ActorEmitters::Actor, 2> actors{{{7, &mesh, false, 0.25f}, {8, &mesh}}};
    auto pose = [&](std::uint32_t id, int bone, glm::mat4& out) {
        out = world;
        if (id == 8) out[3].y += 50;
        return bone == 2;
    };
    particles.update(0.01f, actors, pose, {});
    CHECK(particles.simulation().emitters().empty());
    REQUIRE(particles.cloud_simulation().emitters().size() == 2);
    CHECK(particles.cloud_simulation().definitions().size() == 2); // one shared definition plus the sentinel
    const auto& first = particles.cloud_simulation().emitters()[0];
    CHECK(first.persistent);
    CHECK(near(first.origin, {106, 20, 30}));
    REQUIRE(first.particles.size() == 1);
    CHECK(first.particles.front().color.a == doctest::Approx(0.0625f)); // WLD creator opacity x maximum x owner
    CHECK(first.particles.front().size_factor == doctest::Approx(2.0f)); // node scale once, no EDD PESFactor
    world[3].x += 10;
    actors[0].alpha = 0.75f;
    particles.update(0.01f, actors, pose, {});
    CHECK(near(particles.cloud_simulation().emitters()[0].origin, {116, 20, 30}));
    CHECK(near(particles.cloud_simulation().emitters()[0].particles.front().position, {116, 20, 30}));
    CHECK(particles.cloud_simulation().emitters()[0].particles.front().color.a == doctest::Approx(0.1875f));
    // A new appearance drops the old item's cloud, while another actor using the shared mesh keeps it.
    assets::CharacterMesh plain;
    actors[0].mesh = &plain;
    particles.update(0.01f, actors, pose, {});
    REQUIRE(particles.cloud_simulation().emitters().size() == 1);
    CHECK(particles.cloud_simulation().emitters().front().actor == 8);
    particles.update(0.01f, {}, pose, {});
    CHECK(particles.cloud_simulation().emitters().empty());
    particles.clear();
    CHECK(particles.cloud_simulation().definitions().size() == 1);
}

TEST_CASE("An actor can carry independent EQG and WLD held-item effects") {
    auto definition = std::make_shared<assets::EmitterDefinition>(dot());
    assets::CharacterMesh mesh;
    assets::MeshEmitter eqg;
    eqg.bone = 0;
    eqg.emitter.emitter = 1;
    mesh.emitters.push_back(eqg);
    auto cloud = eqg;
    cloud.cloud = definition;
    mesh.emitters.push_back(cloud);
    app::ActorEmitters particles({assets::EmitterDefinition{}, dot()});
    app::ActorEmitters::Actor actor{7, &mesh};
    auto pose = [](std::uint32_t, int, glm::mat4& out) { out = glm::mat4(1); return true; };
    particles.update(0.01f, std::span(&actor, 1), pose, {});
    REQUIRE(particles.simulation().emitters().size() == 1);
    REQUIRE(particles.cloud_simulation().emitters().size() == 1);
    CHECK(particles.emitters() == 2);
    mesh.serial = {};
    particles.update(0.01f, std::span(&actor, 1), pose, {});
    CHECK(particles.simulation().emitters().size() == 1);
    CHECK(particles.cloud_simulation().emitters().size() == 1);
    CHECK(particles.cloud_simulation().definitions().size() == 2);
}

TEST_CASE("Actor emitters stepped on a worker match the inline step, drawn a frame later") {
    // The scene's emitters step in simulate_async, on a worker, after the frame; the others step inside update. Both
    // read the same poses, so after each step the particles are the same; between update and the step (when the
    // scene draws) the worker's are the frame before's.
    core::WorkerPool pool(2);
    core::set_workers(&pool);
    struct Unset {
        ~Unset() { core::set_workers(nullptr); }
    } unset;
    auto d = dot();
    d.duration = -9999.0f;
    d.follow_attachment = 1;
    d.bone_orientation = 1;
    d.per_tick = 1;
    d.ticks_per_second = 30.0f;
    auto definition = std::make_shared<assets::EmitterDefinition>(d);
    assets::CharacterMesh mesh;
    assets::MeshEmitter eqg;
    eqg.bone = 0;
    eqg.emitter.emitter = 1;
    mesh.emitters.push_back(eqg);
    auto cloud = eqg;
    cloud.bone = 1;
    cloud.local = glm::translate(glm::mat4(1), glm::vec3(3, 0, 0));
    cloud.cloud = definition;
    mesh.emitters.push_back(cloud);
    app::ActorEmitters inline_step({assets::EmitterDefinition{}, d});
    app::ActorEmitters worker_step({assets::EmitterDefinition{}, d}, {}, true);
    std::array<app::ActorEmitters::Actor, 2> actors{{{7, &mesh}, {8, &mesh, false, 0.5f}}};
    glm::mat4 world(1.0f);
    auto pose = [&](std::uint32_t id, int bone, glm::mat4& out) {
        out = world;
        out[3].y += static_cast<float>(id) * 10.0f + static_cast<float>(bone);
        return true;
    };
    auto same = [](const game::ParticleSystem& a, const game::ParticleSystem& b) {
        REQUIRE(a.emitters().size() == b.emitters().size());
        for (std::size_t e = 0; e < a.emitters().size(); ++e) {
            const auto& ea = a.emitters()[e];
            const auto& eb = b.emitters()[e];
            CHECK(ea.actor == eb.actor);
            CHECK(near(ea.origin, eb.origin));
            REQUIRE(ea.particles.size() == eb.particles.size());
            for (std::size_t p = 0; p < ea.particles.size(); ++p) {
                CHECK(near(ea.particles[p].position, eb.particles[p].position));
                CHECK(ea.particles[p].color.a == doctest::Approx(eb.particles[p].color.a));
                CHECK(ea.particles[p].age == doctest::Approx(eb.particles[p].age));
            }
        }
    };
    std::vector<game::Emitter> before, before_clouds; // the inline step's particles as of the last frame
    for (int frame = 0; frame < 40; ++frame) {
        world = glm::rotate(glm::mat4(1.0f), 0.05f * static_cast<float>(frame), glm::vec3(0, 0, 1));
        world[3] = glm::vec4(2.0f * static_cast<float>(frame), 0, 0, 1);
        const float dt = frame % 3 == 0 ? 0.016f : 0.033f;
        inline_step.update(dt, actors, pose, {});
        worker_step.update(dt, actors, pose, {});
        if (frame > 0) {
            // As the scene draws it: the emitters this frame started, last frame's particles.
            const auto& drawn = worker_step.simulation().emitters();
            REQUIRE(drawn.size() == before.size());
            for (std::size_t e = 0; e < drawn.size(); ++e) {
                REQUIRE(drawn[e].particles.size() == before[e].particles.size());
                for (std::size_t p = 0; p < drawn[e].particles.size(); ++p)
                    CHECK(near(drawn[e].particles[p].position, before[e].particles[p].position));
            }
            CHECK(worker_step.cloud_simulation().emitters().size() == before_clouds.size());
        }
        worker_step.simulate_async(dt, pose);
        same(inline_step.simulation(), worker_step.simulation());
        same(inline_step.cloud_simulation(), worker_step.cloud_simulation());
        before = inline_step.simulation().emitters();
        before_clouds = inline_step.cloud_simulation().emitters();
    }
    CHECK(inline_step.emitters() == 4);
    CHECK_FALSE(inline_step.simulation().emitters().front().particles.empty());
    // An actor gone: both stop its emitters at the next update.
    inline_step.update(0.016f, std::span(actors.data(), 1), pose, {});
    worker_step.update(0.016f, std::span(actors.data(), 1), pose, {});
    worker_step.simulate_async(0.016f, pose);
    same(inline_step.simulation(), worker_step.simulation());
    same(inline_step.cloud_simulation(), worker_step.cloud_simulation());
    CHECK(worker_step.emitters() == 2);
}

TEST_CASE("Instanced item clouds keep independent spawn, object and missile attachment lifetimes") {
    auto definition = std::make_shared<assets::EmitterDefinition>(dot());
    definition->follow_attachment = 1;
    definition->bone_orientation = 1;
    assets::CharacterMesh mesh;
    assets::MeshEmitter cloud;
    cloud.bone = 0; // item meshes bake their node pose into the object's local frame
    cloud.local = glm::translate(glm::mat4(1), glm::vec3(2, 0, 0));
    cloud.cloud = definition;
    mesh.emitters.push_back(cloud);
    constexpr std::uint32_t spawn = 7, object = app::WorldObjects::kObjectActorBit | 7,
                            missile = app::WorldObjects::kMissileActorBit | 7;
    std::vector<app::ActorEmitters::Actor> actors{{spawn, &mesh}, {object, &mesh}, {missile, &mesh}};
    glm::mat4 object_world = glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 0, 1));
    object_world = glm::scale(object_world, glm::vec3(3));
    object_world[3] = {20, 30, 40, 1};
    auto pose = [&](std::uint32_t id, int bone, glm::mat4& world) {
        if (bone != 0) return false;
        if (id == object) world = object_world;
        else world = glm::translate(glm::mat4(1), glm::vec3(id == missile ? 100 : 10, 0, 0));
        return true;
    };
    app::ActorEmitters particles(std::vector<assets::EmitterDefinition>{});
    particles.update(0.01f, actors, pose, {});
    REQUIRE(particles.cloud_simulation().emitters().size() == 3);
    CHECK(near(particles.cloud_simulation().emitters()[0].origin, {12, 0, 0}));
    CHECK(near(particles.cloud_simulation().emitters()[1].origin, {20, 36, 40}));
    CHECK(near(particles.cloud_simulation().emitters()[2].origin, {102, 0, 0}));
    actors.erase(actors.begin() + 1); // the drop was picked up; the held and thrown instances remain
    particles.update(0.01f, actors, pose, {});
    REQUIRE(particles.cloud_simulation().emitters().size() == 2);
    CHECK(particles.cloud_simulation().emitters()[0].actor == spawn);
    CHECK(particles.cloud_simulation().emitters()[1].actor == missile);
    actors = {{object + 1, &mesh}}; // another dropped instance gets a fresh owner, even with the same source model
    particles.update(0.01f, actors, [](std::uint32_t, int, glm::mat4& world) {
        world = glm::translate(glm::mat4(1), glm::vec3(200, 0, 0));
        return true;
    }, {});
    REQUIRE(particles.cloud_simulation().emitters().size() == 1);
    const auto& last = particles.cloud_simulation().emitters().front();
    CHECK(last.actor == object + 1);
    REQUIRE(last.particles.size() == 1);
    CHECK(near(last.particles.front().position, {202, 0, 0}));
}

TEST_CASE("Authored held-item cloud tracks animate inside their host frame with an instance clock") {
    auto definition = std::make_shared<assets::EmitterDefinition>(dot());
    definition->follow_attachment = 1;
    definition->bone_orientation = 1;
    auto animation = std::make_shared<assets::ObjectAnimation>();
    animation->period_ms = 500;
    animation->nodes.resize(1);
    auto& node = animation->nodes.front();
    node.interval_ms = 250;
    node.moves = true;
    node.keys = {{glm::quat(1, 0, 0, 0), {2, 0, 0}, 1},
                 {glm::quat(1, 0, 0, 0), {4, 0, 0}, 1},
                 {glm::quat(1, 0, 0, 0), {2, 0, 0}, 1}};
    animation->rest_inverse = {glm::translate(glm::mat4(1), glm::vec3(-2, 0, 0))};
    assets::CharacterMesh mesh;
    assets::MeshEmitter cloud;
    cloud.bone = 0;
    cloud.local = glm::translate(glm::mat4(1), glm::vec3(2, 0, 0)); // baked first key's node matrix
    cloud.cloud = definition;
    cloud.cloud_animation = animation;
    mesh.emitters = {cloud, cloud}; // shared controller sampled once, with separate emitters on it
    constexpr std::uint32_t object = app::WorldObjects::kObjectActorBit | 10,
                            missile = app::WorldObjects::kMissileActorBit | 20;
    std::vector<app::ActorEmitters::Actor> actors{{object, &mesh}};
    const glm::mat4 host = glm::translate(glm::mat4(1), glm::vec3(20, 30, 40)) *
                           glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 0, 1)) *
                           glm::scale(glm::mat4(1), glm::vec3(3));
    auto pose = [&](std::uint32_t id, int bone, glm::mat4& world) {
        world = id == object ? host : glm::translate(glm::mat4(1), glm::vec3(100, 0, 0));
        return bone == 0;
    };
    app::ActorEmitters particles(std::vector<assets::EmitterDefinition>{});
    particles.update(0.125f, actors, pose, {});
    REQUIRE(particles.cloud_simulation().emitters().size() == 2);
    CHECK(near(particles.cloud_simulation().emitters()[0].origin, {20, 36, 40})); // new controller starts at phase 0
    actors.push_back({missile, &mesh});
    particles.update(0.125f, actors, pose, {});
    REQUIRE(particles.cloud_simulation().emitters().size() == 4);
    CHECK(near(particles.cloud_simulation().emitters()[0].origin, {20, 39, 40})); // 125 ms: node x=3, host turns/scales it
    CHECK(near(particles.cloud_simulation().emitters()[2].origin, {102, 0, 0})); // another instance starts at its own 0
    particles.update(0.125f, actors, pose, {});
    CHECK(near(particles.cloud_simulation().emitters()[0].origin, {20, 42, 40}));
    CHECK(near(particles.cloud_simulation().emitters()[2].origin, {103, 0, 0}));
    const auto key = particles.cloud_simulation().emitters()[0].key;
    particles.set_density(0.0f); // only the EDD list changes; the WLD controller and living particles stay
    particles.update(0.0f, actors, pose, {});
    CHECK(particles.cloud_simulation().emitters()[0].key == key);
    CHECK(near(particles.cloud_simulation().emitters()[0].origin, {20, 42, 40}));
    assets::CharacterMesh replacement = mesh; // another held item/appearance starts its own controller
    actors[0].mesh = &replacement;
    particles.update(0.125f, actors, pose, {});
    REQUIRE(particles.cloud_simulation().emitters().size() == 4);
    auto origin = [&](std::uint32_t id) {
        for (const auto& emitter : particles.cloud_simulation().emitters())
            if (emitter.actor == id) return emitter.origin;
        return glm::vec3(-1);
    };
    CHECK(near(origin(object), {20, 36, 40}));
    CHECK(near(origin(missile), {104, 0, 0}));
}

TEST_CASE("Local sharp-weapon damage produces asset-backed blood only at a supported actor point") {
    using namespace net;
    using namespace test;
    Rig rig;
    REQUIRE(rig.to_zone());
    rig.server.add_spawn(make_spawn("FixtureTarget", 334, 1, 1, 80, 90, 8));
    REQUIRE(rig.until([&] { return rig.conn.world().spawns.contains(334); }));
    app::SpellParticles particles({}, {});
    glm::mat4 point = glm::translate(glm::mat4(1), glm::vec3(10, 20, 30));
    bool supported = true;
    particles.set_blood_point([&](std::uint32_t id) -> std::optional<glm::mat4> {
        return supported && id == 334 ? std::optional(point) : std::nullopt;
    });
    CombatDamage hit;
    hit.source = static_cast<std::uint16_t>(rig.conn.world().player_id);
    hit.target = 334;
    hit.type = 1;
    hit.damage = 10;
    // Even with a valid point, an installation without the source sprite shows no fabricated fallback.
    rig.conn.receive_zone_packet_for_test(encode(hit));
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    CHECK(particles.simulation().emitters().empty());
    particles.set_blood_sprite(game::blood_spurt_definition("fixture.dds"));
    rig.conn.receive_zone_packet_for_test(encode(hit));
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    REQUIRE(particles.simulation().emitters().size() == 1);
    const auto& blood = particles.simulation().emitters().front();
    CHECK(blood.actor == 334);
    CHECK(blood.point_only);
    CHECK(near(blood.origin, {10, 20, 30}));
    CHECK(blood.duration == doctest::Approx(0.5f));
    CHECK(blood.counts.capacity == 100);
    REQUIRE(blood.particles.size() == 1);
    CHECK(blood.particles.front().color.a == doctest::Approx(0.5f));
    CHECK(blood.particles.front().size_factor == doctest::Approx(0.375f));
    point[3].x += 20;
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    CHECK(near(particles.simulation().emitters().front().origin, {30, 20, 30}));
    particles.clear();
    // Skill, ownership, successful hit and a living target are all part of the packet-to-cue contract.
    for (auto skill : {4u, 7u, 21u, 30u, 51u, 231u}) {
        hit.type = static_cast<std::uint8_t>(skill);
        rig.conn.receive_zone_packet_for_test(encode(hit));
    }
    hit.type = 1;
    hit.source = 333;
    rig.conn.receive_zone_packet_for_test(encode(hit));
    hit.source = static_cast<std::uint16_t>(rig.conn.world().player_id);
    hit.damage = 0;
    rig.conn.receive_zone_packet_for_test(encode(hit));
    CHECK(rig.conn.take_blood_spurts().empty());
    hit.damage = 10;
    for (auto skill : {0u, 1u, 2u, 3u, 8u, 10u, 36u, 77u}) {
        hit.type = static_cast<std::uint8_t>(skill);
        rig.conn.receive_zone_packet_for_test(encode(hit));
    }
    CHECK(rig.conn.take_blood_spurts().size() == 8);
    rig.conn.world_for_test().spawns.at(334).is_corpse = true;
    rig.conn.receive_zone_packet_for_test(encode(hit));
    CHECK(rig.conn.take_blood_spurts().empty());
    rig.conn.world_for_test().spawns.at(334).is_corpse = false;
    supported = false;
    rig.conn.receive_zone_packet_for_test(encode(hit));
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    CHECK(particles.simulation().emitters().empty());
    supported = true;
    rig.conn.receive_zone_packet_for_test(encode(hit));
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    REQUIRE(particles.simulation().emitters().size() == 1);
    rig.conn.world_for_test().spawns.erase(334);
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    CHECK(particles.simulation().emitters().empty());
    particles.clear();
    rig.conn.disconnect();
    CHECK(rig.conn.take_blood_spurts().empty());
}

TEST_CASE("A valid spell missile packet creates its stage at the missile and releases missile-owned emitters") {
    using namespace net;
    using namespace test;
    Rig rig;
    REQUIRE(rig.to_zone());
    rig.server.add_spawn(make_spawn("FixtureCaster", 333, 1, 1, 50, 60, 7));
    rig.server.add_spawn(make_spawn("FixtureTarget", 334, 1, 1, 80, 90, 8));
    REQUIRE(rig.until([&] { return rig.conn.world().spawns.contains(333) && rig.conn.world().spawns.contains(334); }));
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_line(600, 8, 1000, true)) == 1);
    REQUIRE(table->get(600));
    CHECK(table->get(600)->npc_no_los);
    REQUIRE(table->get(600)->spell_anim == 1);
    REQUIRE(table->get(600)->target_type == 8);
    rig.conn.set_spell_table_for_test(table);
    assets::SpellEffect effect;
    effect.stages[2].slots[0] = {1, 0, 2, 8}; // the actual missile actor
    effect.stages[2].slots[1] = {1, 0, 1, 8}; // source is still separately addressable
    effect.stages[2].slots[2] = {1, 0, 0, 0}; // the explicit world point
    effect.stages[2].slots[3] = {1, 0, 3, 8}; // native missile launch passes no target actor to DoSpellEffect
    app::SpellParticles particles({assets::EmitterDefinition{}, dot()}, {assets::SpellEffect{}, effect});
    ByteWriter packet;
    for (float value : {10.0f, 20.0f, 30.0f, 1.0f, 2.0f, 3.0f, 4.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}) packet.f32(value);
    for (auto value : {333u, 334u, 0u, 0u, 600u}) packet.le(value);
    packet.le<std::uint8_t>(0); // light
    packet.le<std::uint32_t>(0);
    packet.le<std::uint8_t>(1); // range
    packet.le<std::uint8_t>(9); // spell missile
    packet.le<std::uint8_t>(0); // skill
    packet.le<std::uint8_t>(0); // explicit destination flag
    for (int i = 0; i < 3; ++i) packet.f32(0);
    packet.fixed_string("IT123", 27);
    REQUIRE(packet.data().size() == 116);
    rig.conn.receive_zone_packet_for_test({to_wire(Op::SomeItemPacketMaybe), packet.take()});
    auto incoming = rig.conn.take_missiles();
    REQUIRE(incoming.size() == 1);
    REQUIRE(incoming.front().item_type == 9);
    REQUIRE(incoming.front().spell == 600);
    REQUIRE(incoming.front().source_id == 333);
    game::MissileList missiles;
    missiles.add(game::make_missile(incoming.front(), 1000, 0));
    game::MissileWorld world;
    world.spawn = [&](std::uint32_t id) -> std::optional<game::MissileSpawn> {
        const auto& spawns = rig.conn.world().spawns;
        auto it = spawns.find(id);
        if (it == spawns.end()) return std::nullopt;
        return game::MissileSpawn{id, it->second.position, 4, 6, it->second.race, false};
    };
    world.spell = [&](std::uint32_t id) { return table->get(id); };
    int played_stage = -1;
    world.effect = [&](const game::Missile& m, int stage) {
        played_stage = stage;
        particles.missile_effect(rig.conn, m, stage, nullptr);
    };
    world.ended = [&](const game::Missile& m) { particles.end_missile(m.serial); };
    particles.set_missile_pose([&](std::uint32_t serial) -> std::optional<glm::mat4> {
        for (const auto& m : missiles.missiles())
            if (m.serial == serial) return glm::translate(glm::mat4(1), m.actor_position);
        return std::nullopt;
    });
    missiles.update(1000, world);
    REQUIRE(missiles.missiles().size() == 1);
    REQUIRE(missiles.missiles().front().has_actor);
    REQUIRE(played_stage == 2);
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    const auto& emitters = particles.simulation().emitters();
    REQUIRE(emitters.size() == 3);
    CHECK(emitters[0].actor == (0x80000000u | missiles.missiles().front().serial));
    CHECK(near(emitters[0].origin, rig.conn.world().spawns.at(334).position));
    CHECK(emitters[1].actor == 333);
    CHECK(near(emitters[1].origin, rig.conn.world().spawns.at(333).position));
    CHECK(emitters[2].actor == 0);
    CHECK(near(emitters[2].origin, rig.conn.world().spawns.at(334).position));
    missiles.update(2033, world);
    REQUIRE(particles.simulation().emitters().size() == 2);
    CHECK(std::none_of(particles.simulation().emitters().begin(), particles.simulation().emitters().end(),
                       [](const auto& e) { return (e.actor & 0x80000000u) != 0; }));
    particles.clear();
    CHECK(particles.simulation().emitters().empty());
}

TEST_CASE("Spell 54's destruction impact outlives the missile it is played for") {
    using namespace net;
    using namespace test;
    Rig rig;
    REQUIRE(rig.to_zone());
    rig.server.add_spawn(make_spawn("FixtureCaster", 333, 1, 1, 50, 60, 7));
    REQUIRE(rig.until([&] { return rig.conn.world().spawns.contains(333); }));
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_line(54, 5)) == 1);
    rig.conn.set_spell_table_for_test(table);
    assets::SpellEffect effect;
    effect.stages[1].slots[0] = {1, 0, 2, 8}; // the flight: on the missile
    effect.stages[2].slots[0] = {1, 0, 2, 8}; // the impact: where the missile was
    app::SpellParticles particles({assets::EmitterDefinition{}, dot()}, {assets::SpellEffect{}, effect});
    game::MissilePacket packet;
    packet.source_id = 333;
    packet.position = {10, 20, 30};
    packet.velocity = {1, 2, 0};
    packet.item_type = 9;
    packet.spell = 54;
    game::MissileList missiles;
    missiles.add(game::make_missile(packet, 1000, 0));
    game::MissileWorld world;
    world.spawn = [&](std::uint32_t id) -> std::optional<game::MissileSpawn> {
        auto it = rig.conn.world().spawns.find(id);
        if (it == rig.conn.world().spawns.end()) return std::nullopt;
        return game::MissileSpawn{id, it->second.position, 4, 6, it->second.race, false};
    };
    world.spell = [&](std::uint32_t id) { return table->get(id); };
    world.effect = [&](const game::Missile& m, int stage) { particles.missile_effect(rig.conn, m, stage, nullptr); };
    glm::vec3 impact_at{};
    world.ended = [&](const game::Missile& m) {
        impact_at = m.position - m.velocity;
        particles.end_missile(m.serial);
    };
    particles.set_missile_pose([&](std::uint32_t serial) -> std::optional<glm::mat4> {
        for (const auto& m : missiles.missiles())
            if (m.serial == serial) return glm::translate(glm::mat4(1), m.actor_position);
        return std::nullopt;
    });
    missiles.update(1000, world);
    REQUIRE(missiles.missiles().size() == 1);
    REQUIRE(particles.simulation().emitters().size() == 1);
    CHECK(particles.simulation().emitters()[0].actor == (0x80000000u | missiles.missiles().front().serial));
    missiles.update(1066, world); // past its 50 ms
    REQUIRE(missiles.missiles().empty());
    particles.update(rig.conn, {}, {}, nullptr, 0.01f);
    const auto& emitters = particles.simulation().emitters();
    REQUIRE(emitters.size() == 1); // the flight's went with the missile, the impact stays
    CHECK(emitters[0].actor == 0);
    CHECK(near(emitters[0].origin, impact_at));
    CHECK(emitters[0].particles.size() == 1);
}

TEST_CASE("Spell missile duration reads the native AE clamp and its first-slot cloud exception") {
    game::SpellTable table;
    REQUIRE(table.parse(spell_line(600, 8, 20000) + "\n" + spell_line(601, 8, 20000, false, 76)) == 2);
    REQUIRE(table.get(600));
    REQUIRE(table.get(601));
    CHECK(table.get(600)->ae_duration == 14000);
    CHECK(table.get(601)->ae_duration == 20000);
}

TEST_CASE("Spell859 missile destruction invokes the local Action presentation without applying a buff") {
    using namespace net;
    using namespace test;
    Rig rig;
    REQUIRE(rig.to_zone());
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(spell_line(859, 5, 0, false, 1, true)) == 1);
    rig.conn.set_spell_table_for_test(table);
    const auto me = rig.conn.world().player_id;
    REQUIRE(rig.conn.world().player());
    const auto position = rig.conn.world().player()->position;
    rig.server.add_spawn(make_spawn("FixtureCaster", 333, 1, 1, 50, 60, 7));
    REQUIRE(rig.until([&] { return rig.conn.world().spawns.contains(333); }));
    BeginCast casting; // the source is casting something else, which the local presentation must not end
    casting.spell_id = 859;
    casting.caster_id = 333;
    casting.cast_time = 10000;
    rig.conn.receive_zone_packet_for_test(encode(casting));
    REQUIRE(rig.conn.spell_state().others.contains(333));
    rig.conn.take_spell_effect_cues();
    rig.conn.take_sound_cues();
    const auto hp = rig.conn.player_status().hp;
    const auto original_buffs = rig.conn.spell_state().buffs;
    game::MissilePacket packet;
    packet.source_id = 333;
    packet.item_type = 9;
    packet.spell = 859;
    packet.position = position + glm::vec3(39.0f, 0, 1000.0f); // destruction distance deliberately ignores height
    game::MissileWorld world;
    world.local_id = me;
    world.spawn = [&](std::uint32_t id) -> std::optional<game::MissileSpawn> {
        auto it = rig.conn.world().spawns.find(id);
        if (it == rig.conn.world().spawns.end()) return std::nullopt;
        return game::MissileSpawn{id, it->second.position, 4, 6, it->second.race, false};
    };
    world.spell = [&](std::uint32_t id) { return table->get(id); };
    int local_hits = 0;
    world.spell_hit = [&](const game::Missile& m) {
        ++local_hits;
        rig.conn.spell_missile_hit(m);
    };
    game::MissileList missiles;
    missiles.add(game::make_missile(packet, 1000, 0));
    missiles.update(1000, world);
    missiles.update(1066, world);
    REQUIRE(local_hits == 1);
    auto cues = rig.conn.take_spell_effect_cues();
    REQUIRE(cues.size() == 1);
    CHECK(cues.front().stage == 2);
    CHECK(cues.front().source == 333);
    CHECK(cues.front().target == me);
    CHECK_FALSE(cues.front().proc); // no invented proc cast stage or animation broadcast
    CHECK(rig.conn.spell_state().others.contains(333));
    auto sounds = rig.conn.take_sound_cues();
    REQUIRE(sounds.size() == 1);
    CHECK(sounds.front().kind == SoundCue::Kind::SpellLanded);
    CHECK(rig.conn.player_status().hp == hp);
    REQUIRE(rig.conn.spell_state().buffs.size() == original_buffs.size());
    for (std::size_t i = 0; i < original_buffs.size(); ++i) {
        CHECK(rig.conn.spell_state().buffs[i].spell == original_buffs[i].spell);
        CHECK(rig.conn.spell_state().buffs[i].tics == original_buffs[i].tics);
    }
    CHECK(std::none_of(rig.conn.spell_state().buffs.begin(), rig.conn.spell_state().buffs.end(),
                       [](const auto& buff) { return buff.spell == 859; }));
    assets::SpellEffect effect;
    effect.stages[2].slots[0] = {1, 0, 3, 8};
    app::SpellParticles particles({assets::EmitterDefinition{}, dot()}, {assets::SpellEffect{}, effect});
    particles.update(rig.conn, cues, {}, nullptr, 0.01f);
    REQUIRE(particles.simulation().emitters().size() == 1);
    CHECK(particles.simulation().emitters().front().actor == me);
    CHECK(near(particles.simulation().emitters().front().origin, position));
    packet.position.x = position.x + 40.0f;
    missiles.add(game::make_missile(packet, 2000, 0));
    missiles.update(2000, world);
    missiles.update(2066, world);
    CHECK(local_hits == 1); // strict <40 at the destruction site
    CHECK(rig.conn.take_spell_effect_cues().empty());

    // A real success Action for the same fixture must be capable of applying a lasting buff, so the assertion
    // above proves the synthetic path's missing success flag rather than an inert spell definition.
    Action success;
    success.source = 333;
    success.target = static_cast<std::uint16_t>(me);
    success.type = 231;
    success.spell_id = 859;
    success.level = success.spell_level = 50;
    success.effect_flag = 4;
    rig.conn.receive_zone_packet_for_test(encode(success));
    CHECK(std::any_of(rig.conn.spell_state().buffs.begin(), rig.conn.spell_state().buffs.end(),
                     [](const auto& buff) { return buff.spell == 859 && buff.tics > 0; }));
}

TEST_CASE("Actor Particle Density: a change keeps the running emitters' particles; Off stops them and tries none") {
    auto d = dot();
    d.duration = -9999.0f;
    d.per_tick = 20; // 20 and more a tick: the density scales the count, not the rate
    d.ticks_per_second = 10.0f;
    d.initial_count = 40;
    assets::CharacterMesh mesh;
    assets::MeshEmitter always;
    always.bone = 0;
    always.emitter.emitter = 1;
    mesh.emitters.push_back(always);
    auto standing = always;
    standing.emitter.trigger = 1;
    standing.emitter.animation = 31; // STND
    mesh.emitters.push_back(standing);
    app::ActorEmitters particles({assets::EmitterDefinition{}, d});
    app::ActorEmitters::Actor actor{7, &mesh, true};
    int poses = 0;
    auto pose = [&](std::uint32_t, int, glm::mat4& out) {
        ++poses;
        out = glm::mat4(1.0f);
        return true;
    };
    for (int i = 0; i < 5; ++i) particles.update(0.05f, std::span(&actor, 1), pose, {});
    REQUIRE(particles.simulation().emitters().size() == 2);
    const std::uint32_t key = particles.simulation().emitters()[0].key;
    const std::size_t before = particles.simulation().emitters()[0].particles.size();
    REQUIRE(before > 0u);

    // High to Medium: the same emitters, their particles kept (no longer all made again and emptied at once).
    particles.set_density(0.5f);
    CHECK(particles.simulation().emitters()[0].key == key);
    CHECK(particles.simulation().emitters()[0].density == doctest::Approx(0.5f));
    CHECK(particles.simulation().emitters()[0].particles.size() ==
          std::min(before, static_cast<std::size_t>(particles.simulation().emitters()[0].counts.capacity)));
    CHECK(particles.simulation().emitters()[1].density == doctest::Approx(0.5f)); // the standing one too
    particles.update(0.05f, std::span(&actor, 1), pose, {});
    CHECK(particles.simulation().emitters()[0].key == key);

    // Off: the EDD emitters go, and none is tried again (no pose asked for them) while it stays off.
    particles.set_density(0.0f);
    particles.update(0.05f, std::span(&actor, 1), pose, {});
    CHECK(particles.simulation().emitters().empty());
    const int poses_off = poses;
    for (int i = 0; i < 5; ++i) particles.update(0.05f, std::span(&actor, 1), pose, {});
    CHECK(poses == poses_off);
    CHECK(particles.simulation().emitters().empty());

    // On again: made afresh.
    particles.set_density(0.25f);
    particles.update(0.05f, std::span(&actor, 1), pose, {});
    REQUIRE(particles.simulation().emitters().size() == 2);
    CHECK(particles.simulation().emitters()[0].density == doctest::Approx(0.25f));
}

TEST_CASE("Actor emitters out of view emit nothing, ask for no pose and start no animation entries") {
    auto d = dot();
    d.duration = -9999.0f;
    d.per_tick = 1;
    d.ticks_per_second = 20.0f;
    d.lifetime = 0.2f;
    assets::CharacterMesh mesh;
    assets::MeshEmitter always;
    always.bone = 0;
    always.emitter.emitter = 1;
    mesh.emitters.push_back(always);
    auto step = always; // a footfall: started by its animation each time the animation starts
    step.emitter.trigger = 1;
    step.emitter.animation = 0; // the first row of the animation table with a clip (27, NRUN, in the shipped files)
    for (std::int32_t a = 1; a <= 0xb6 - 0x4f && step.emitter.animation == 0; ++a) {
        namespace m = game::mechanics;
        if (a != 31 && a != 32 && !m::old_anim_clip(m::new_to_old_anim(static_cast<std::uint8_t>(0x4f + a))).empty())
            step.emitter.animation = a;
    }
    REQUIRE(step.emitter.animation != 0);
    mesh.emitters.push_back(step);
    for (bool async : {false, true}) {
        CAPTURE(async);
        app::ActorEmitters particles({assets::EmitterDefinition{}, d}, {}, async);
        app::ActorEmitters::Actor actor{7, &mesh};
        int poses = 0;
        auto pose = [&](std::uint32_t, int, glm::mat4& out) {
            ++poses;
            out = glm::mat4(1.0f);
            return true;
        };
        float clip_time = 0.0f;
        auto playing = [&](std::uint32_t, std::string_view, float& time) {
            time = clip_time;
            return true;
        };
        auto frame = [&] {
            particles.update(0.05f, std::span(&actor, 1), pose, {}, playing);
            particles.simulate_async(0.05f, pose);
            clip_time = std::fmod(clip_time + 0.05f, 0.3f); // loops every 0.3 s: a new step
        };
        for (int i = 0; i < 4; ++i) frame();
        REQUIRE_FALSE(particles.simulation().emitters().empty());
        CHECK(particles.simulation().emitters().size() == 2); // the always-on one and a step

        actor.in_view = false;
        for (int i = 0; i < 10; ++i) frame(); // 0.5 s: what it emitted has ended, the step too
        const int poses_held = poses;
        for (int i = 0; i < 10; ++i) frame();
        CHECK(poses == poses_held); // not placed
        REQUIRE(particles.simulation().emitters().size() == 1); // no new step started
        CHECK(particles.simulation().emitters()[0].held);
        CHECK(particles.simulation().emitters()[0].particles.empty());

        // Back in view: it emits again from the next tick, not a second's worth of ticks at once.
        actor.in_view = true;
        frame();
        frame();
        CHECK(poses > poses_held);
        CHECK_FALSE(particles.simulation().emitters()[0].held);
        CHECK(particles.simulation().emitters()[0].particles.size() <= 3u);
        CHECK_FALSE(particles.simulation().emitters()[0].particles.empty());
    }
}
