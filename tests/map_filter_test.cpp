#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "game/combat.h"
#include "game/doors.h"
#include "game/server_ext.h"
#include "game/world_state.h"
#include "mq/map_filter.h"
#include "mock_server/harness.h"
#include "manual_clock.h"
#include "mq_test_world.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::mq;

namespace {

std::vector<std::string> run(MapFilters& filters, std::string_view command, const MapFilterCommandInput& input = {}) {
    std::vector<std::string> lines;
    run_map_filter_command(filters, command, input, [&](std::string_view line) { lines.emplace_back(line); });
    return lines;
}

// One update, then another once the target has settled and the request interval has passed.
test::ManualClock& target_path_clock() {
    static test::ManualClock clock = [] {
        test::ManualClock c;
        c.hold();
        return c;
    }();
    return clock;
}
void settle(MapFilters& filters, net::GameConnection& conn, MapFilterTargetPathState& state) {
    auto& clock = target_path_clock();
    update_map_filter_target_path(filters, conn, state, clock.source()());
    clock.advance(std::chrono::duration_cast<std::chrono::milliseconds>(kTargetPathInterval));
    update_map_filter_target_path(filters, conn, state, clock.source()());
}

struct NoNpcFind final : game::ServerExtension {
    game::MapPolicy map_policy(const net::GameConnection&) const override {
        return {game::MapPolicyDisableNpcFind, "No NPC Find", 1};
    }
};

} // namespace

TEST_CASE("Map filters: MacroQuest's options, defaults and dependencies") {
    CHECK(map_filter_specs().size() == 34);
    CHECK_FALSE(find_map_filter("vanilla")); // Hailstone's mode is deliberately outside MacroQuest's options
    CHECK(find_map_filter("all") == MapFilter::All);
    CHECK(find_map_filter("UNTARGETABLE") == MapFilter::Untargetable);
    CHECK_FALSE(find_map_filter("untargettable")); // MQ's command name is Untargetable; the docs' typo is not an alias
    CHECK_FALSE(find_map_filter("menu"));
    CHECK_FALSE(find_map_filter("campradius"));
    CHECK_FALSE(find_map_filter("pullradius"));
    CHECK_FALSE(find_map_filter("missing"));

    MapFilters filters;
    CHECK(filters.enabled(MapFilter::All));
    CHECK(filters.enabled(MapFilter::NormalLabels)); // Hailstone keeps its existing map labels by default
    CHECK_FALSE(filters.vanilla_enabled());
    CHECK_FALSE(filters.vanilla_locked());
    CHECK_FALSE(filters.vanilla_mode());
    CHECK_FALSE(filters.enabled(MapFilter::Pc));
    CHECK(filters.state(MapFilter::Pc).rgb == 0xff00ffu);
    CHECK(filters.state(MapFilter::Npc).rgb == 0x404040u);
    CHECK(filters.state(MapFilter::Corpse).rgb == 0x00c000u);
    CHECK(filters.state(MapFilter::TargetLine).rgb == 0x800000u);

    Config empty_config;
    MapFilters configured(&empty_config);
    CHECK(configured.state(MapFilter::Pc).rgb == 0xff00ffu);
    CHECK(configured.state(MapFilter::Npc).rgb == 0x404040u);
    CHECK(configured.state(MapFilter::TargetLine).rgb == 0x800000u);

    CHECK(filters.requirements_met(MapFilter::Pc));
    CHECK_FALSE(filters.requirements_met(MapFilter::PcConColor));
    CHECK_FALSE(filters.requirements_met(MapFilter::Group));
    filters.set_enabled(MapFilter::Pc, true);
    CHECK(filters.requirements_met(MapFilter::PcConColor));
    CHECK(filters.requirements_met(MapFilter::Group));
    CHECK_FALSE(filters.requirements_met(MapFilter::TargetLine));
    filters.set_enabled(MapFilter::Target, true);
    CHECK(filters.requirements_met(MapFilter::TargetLine));
    CHECK_FALSE(filters.requirements_met(MapFilter::PcCorpse));
    filters.set_enabled(MapFilter::Corpse, true);
    CHECK(filters.requirements_met(MapFilter::PcCorpse));
    CHECK(filters.requirements_met(MapFilter::NpcCorpse));
}

TEST_CASE("Map filters: /mapfilter toggles, All and requirement messages") {
    MapFilters filters;
    auto lines = run(filters, "pc");
    REQUIRE(lines.size() == 1);
    CHECK(lines.back() == "PC is now set to: show");
    CHECK(filters.enabled(MapFilter::Pc));
    run(filters, "pc show");
    CHECK(filters.enabled(MapFilter::Pc));
    run(filters, "pc hide");
    CHECK_FALSE(filters.state(MapFilter::Pc).enabled);

    lines = run(filters, "pcconcolor show");
    REQUIRE(lines.size() == 1);
    CHECK(lines.back().find("requires 'PC'") != std::string::npos);
    CHECK_FALSE(filters.state(MapFilter::PcConColor).enabled);

    run(filters, "pc show");
    run(filters, "group show");
    run(filters, "corpse show");
    run(filters, "pccorpse show");
    run(filters, "all hide");
    CHECK_FALSE(filters.enabled(MapFilter::All));
    CHECK(filters.state(MapFilter::Pc).enabled); // All does not forget the individual choices
    CHECK(filters.state(MapFilter::Group).enabled);
    run(filters, "all show");
    CHECK(filters.enabled(MapFilter::All));
    CHECK(filters.enabled(MapFilter::Pc));
    CHECK(filters.enabled(MapFilter::Group));

    // An unknown value toggles, as MQ2Map does.
    run(filters, "npc perhaps");
    CHECK(filters.enabled(MapFilter::Npc));
    run(filters, "npc off");
    CHECK_FALSE(filters.state(MapFilter::Npc).enabled);
    run(filters, "npc on");
    CHECK(filters.state(MapFilter::Npc).enabled);
    run(filters, "marker off");
    CHECK_FALSE(filters.state(MapFilter::Marker).enabled);
    run(filters, "marker on");
    CHECK(filters.state(MapFilter::Marker).enabled);
    lines = run(filters, "not-an-option show");
    CHECK(lines.back() == "Usage: /mapfilter [option|help]");
}

TEST_CASE("Map filters: status and help list the available settings") {
    MapFilters filters;
    auto lines = run(filters, "");
    REQUIRE(lines.size() > 3);
    CHECK(lines.front() == "Map filtering settings:");
    CHECK(std::find(lines.begin(), lines.end(), "Vanilla: hide") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "All: show") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "NormalLabels: show") != lines.end());

    lines = run(filters, "help");
    REQUIRE(lines.size() == 37); // heading, 34 options, vanilla and the colour hint
    CHECK(lines.front() == "Map filtering options:");
    CHECK(std::find(lines.begin(), lines.end(), "TargetRadius #: Sets a circle radius around your target") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "Vanilla: Shows only the map background, without marks or overlays") != lines.end());
    CHECK(lines.back() == "'option' color [r g b]: set its display colour; omit the numbers to reset");

    run(filters, "all hide");
    lines = run(filters, "");
    CHECK(std::find(lines.begin(), lines.end(), "All: hide") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "PC: show") == lines.end());
}

TEST_CASE("Map filters: vanilla is a persistent client choice with a separate server lock") {
    MapFilters filters;
    auto lines = run(filters, "vanilla");
    REQUIRE(lines.size() == 1);
    CHECK(lines.back() == "Vanilla is now set to: show");
    CHECK(filters.vanilla_enabled());
    CHECK(filters.vanilla_mode());
    run(filters, "vanilla off");
    CHECK_FALSE(filters.vanilla_enabled());
    CHECK_FALSE(filters.vanilla_mode());
    run(filters, "vanilla perhaps"); // an unknown value toggles, like the other switches
    CHECK(filters.vanilla_enabled());

    filters.set_vanilla_locked(true);
    CHECK(filters.vanilla_locked());
    lines = run(filters, "vanilla hide");
    REQUIRE(lines.size() == 2);
    CHECK(lines.front() == "Vanilla is now set to: hide");
    CHECK(lines.back() == "This server keeps vanilla mode on.");
    CHECK_FALSE(filters.vanilla_enabled()); // the lock does not overwrite the saved choice
    CHECK(filters.vanilla_mode());
    lines = run(filters, "");
    CHECK(std::find(lines.begin(), lines.end(), "Vanilla: show (server)") != lines.end());
    filters.set_vanilla_locked(false);
    CHECK_FALSE(filters.vanilla_mode());
}

TEST_CASE("Map filters: colours clamp, reset and refuse options without a colour") {
    MapFilters filters;
    run(filters, "pc color 300 -2 nope");
    CHECK(filters.state(MapFilter::Pc).rgb == 0xff00ffu); // 255, 0, 255
    run(filters, "pc color 1");
    CHECK(filters.state(MapFilter::Pc).rgb == 0x01ffffu);
    run(filters, "pc color");
    CHECK(filters.state(MapFilter::Pc).rgb == 0xff00ffu);

    auto lines = run(filters, "pcconcolor color 1 2 3");
    REQUIRE(lines.size() == 1);
    CHECK(lines.back() == "Option 'PCConColor' does not have a color.");
    CHECK(filters.state(MapFilter::PcConColor).rgb == 0u);
}

TEST_CASE("Map filters: radii disable safely and clamp oversized values") {
    MapFilters filters;
    run(filters, "castradius nan");
    CHECK(filters.state(MapFilter::CastRadius).radius == 0.0f);
    run(filters, "spellradius 2000000");
    CHECK(filters.state(MapFilter::SpellRadius).radius == kMapFilterMaxRadius);
    run(filters, "target show");
    run(filters, "targetradius 30");
    CHECK(filters.enabled(MapFilter::TargetRadius));
    CHECK(filters.state(MapFilter::TargetRadius).radius == 30.0f);
    run(filters, "targetradius 45 trailing text");
    CHECK(filters.state(MapFilter::TargetRadius).radius == 45.0f); // MQ reads the leading number
    run(filters, "targetradius hide");
    CHECK_FALSE(filters.enabled(MapFilter::TargetRadius));
}

TEST_CASE("Map filters: marker shapes, sizes and the master switch") {
    MapFilters filters;
    auto lines = run(filters, "marker npc triangle 12");
    REQUIRE(lines.size() == 1);
    CHECK(lines.back() == "Marker 'NPC' is now set to 'Triangle' with size 12.");
    CHECK(filters.enabled(MapFilter::Marker));
    CHECK(filters.state(MapFilter::Npc).marker == MapMarker::Triangle);
    CHECK(filters.state(MapFilter::Npc).marker_size == 12);

    run(filters, "marker pc ring");
    CHECK(filters.state(MapFilter::Pc).marker == MapMarker::Ring);
    CHECK(filters.state(MapFilter::Pc).marker_size == kMapFilterDefaultMarkerSize);
    run(filters, "marker hide");
    CHECK_FALSE(filters.enabled(MapFilter::Marker));
    run(filters, "marker show");
    CHECK(filters.enabled(MapFilter::Marker));

    lines = run(filters, "marker npc unknown");
    CHECK(lines.back().find("unknown shape") != std::string::npos);
    CHECK(filters.state(MapFilter::Npc).marker == MapMarker::Triangle);
    lines = run(filters, "marker missing square");
    CHECK(lines.back().find("unknown spawn type") != std::string::npos);
    lines = run(filters, "marker npc square 0");
    CHECK(lines.back().find("invalid size") != std::string::npos);
    CHECK(filters.state(MapFilter::Npc).marker == MapMarker::Triangle);
}

TEST_CASE("Map filters: custom search is temporary and uses the spawn search parser") {
    test::ScratchDir scratch("map_filter_settings");
    const auto file = scratch.path() / "hailstone.ini";
    Config config;
    MapFilters filters(&config);
    const auto revision = filters.revision();
    run(filters, "custom npc radius 50 range 60 65");
    CHECK(filters.revision() > revision);
    CHECK(filters.enabled(MapFilter::Custom));
    REQUIRE(filters.custom_search());
    CHECK(filters.custom_search()->kind == SpawnSearch::Kind::Npc);
    CHECK(filters.custom_search()->radius == 50.0);
    CHECK(filters.custom_search()->min_level == 60);
    CHECK(filters.custom_search()->max_level == 65);
    CHECK(filters.custom_text().value() == "npc radius 50 range 60 65");

    run(filters, "custom");
    CHECK_FALSE(filters.custom_enabled());
    CHECK_FALSE(filters.custom_search());
    run(filters, "custom pc named");
    REQUIRE(config.save(file));
    Config disk;
    REQUIRE(disk.load(file));
    MapFilters restored(&disk);
    CHECK_FALSE(restored.custom_enabled());
    CHECK_FALSE(restored.custom_text());
}

TEST_CASE("Map filters: settings round-trip globally and clamp damaged values") {
    test::ScratchDir scratch("map_filter_roundtrip");
    const auto file = scratch.path() / "hailstone.ini";
    Config config;
    config.parse("mapfilter.PC.enabled=1\n"
                 "mapfilter.PC.color=123456\n"
                 "mapfilter.CastRadius.radius=42.5\n"
                 "mapfilter.NPC.marker=Diamond\n"
                 "mapfilter.NPC.marker-size=9\n"
                 "mapfilter.Marker.enabled=1\n"
                 "mapfilter.NormalLabels.enabled=0\n"
                 "mapfilter.Vanilla.enabled=1\n");
    MapFilters filters(&config);
    CHECK(filters.enabled(MapFilter::Pc));
    CHECK(filters.state(MapFilter::Pc).rgb == 0x1e240u);
    CHECK(filters.state(MapFilter::CastRadius).radius == 42.5f);
    CHECK(filters.enabled(MapFilter::Marker));
    CHECK(filters.state(MapFilter::Npc).marker == MapMarker::Diamond);
    CHECK(filters.state(MapFilter::Npc).marker_size == 9);
    CHECK_FALSE(filters.enabled(MapFilter::NormalLabels));
    CHECK(filters.vanilla_enabled());
    CHECK(filters.vanilla_mode());

    filters.set_color(MapFilter::Target, 0xabcdef);
    filters.set_radius(MapFilter::TargetRadius, 75.0f);
    REQUIRE(config.save(file));
    Config disk;
    REQUIRE(disk.load(file));
    MapFilters restored(&disk);
    CHECK(restored.state(MapFilter::Target).rgb == 0xabcdefu);
    CHECK(restored.state(MapFilter::TargetRadius).radius == 75.0f);
    CHECK(restored.state(MapFilter::Npc).marker == MapMarker::Diamond);
    CHECK(restored.state(MapFilter::Npc).marker_size == 9);
    CHECK(restored.vanilla_enabled());
    CHECK(Config::is_shared_key("mapfilter.PC.enabled"));
    CHECK(Config::is_shared_key("mapfilter.Vanilla.enabled"));

    Config damaged;
    damaged.parse("mapfilter.SpellRadius.radius=nan\n"
                  "mapfilter.Ground.color=999999999\n"
                  "mapfilter.PC.marker-size=-8\n");
    MapFilters clamped(&damaged);
    CHECK(clamped.state(MapFilter::SpellRadius).radius == 0.0f);
    CHECK(clamped.state(MapFilter::Ground).rgb == 0x9ac9ffu);
    CHECK(clamped.state(MapFilter::Pc).marker_size == 0);
}

TEST_CASE("Map filters: TargetPath is silent and yields to player-started paths") {
    test::Rig rig; REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& world = net::GameConnectionTestAccess::world(conn);
    REQUIRE(world.spawns.contains(101));
    REQUIRE(world.spawns.contains(102));
    world.target_id = 101;
    const auto request_base = net::GameConnectionTestAccess::find_request_count(conn);
    MapFilters filters;
    filters.set_enabled(MapFilter::Target, true);
    filters.set_enabled(MapFilter::TargetPath, true);
    filters.set_vanilla_enabled(true); // navigation still runs when its line is hidden
    MapFilterTargetPathState state;
    (void)conn.take_sound_cues();

    settle(filters, conn, state);
    REQUIRE(state.requested);
    CHECK(state.owns_path);
    CHECK(state.generation != 0);
    CHECK(conn.player_path().kind() == game::FindKind::Spawn);
    CHECK(conn.player_path().target_id() == 101);
    CHECK(net::GameConnectionTestAccess::find_request_count(conn) == request_base + 1);
    CHECK(conn.take_sound_cues().empty());

    settle(filters, conn, state);
    CHECK(state.owns_path);
    CHECK(net::GameConnectionTestAccess::find_request_count(conn) == request_base + 1);

    world.target_id = 102;
    settle(filters, conn, state);
    CHECK(state.owns_path);
    CHECK(conn.player_path().target_id() == 102);
    CHECK(net::GameConnectionTestAccess::find_request_count(conn) == request_base + 2);
    CHECK(conn.take_sound_cues().empty()); // replacing our own silent path is silent too

    // A path the player starts takes ownership back, even across a later target change.
    conn.find_map_location({100.0f, 0.0f, 0.0f});
    (void)conn.take_sound_cues();
    const auto manual_requests = net::GameConnectionTestAccess::find_request_count(conn);
    settle(filters, conn, state);
    CHECK_FALSE(state.owns_path);
    CHECK(conn.player_path().kind() == game::FindKind::MapLocation);
    world.target_id = 101;
    settle(filters, conn, state);
    CHECK_FALSE(state.owns_path);
    CHECK(conn.player_path().kind() == game::FindKind::MapLocation);
    CHECK(net::GameConnectionTestAccess::find_request_count(conn) == manual_requests);
    filters.set_enabled(MapFilter::TargetPath, false);
    settle(filters, conn, state);
    CHECK(conn.player_path().kind() == game::FindKind::MapLocation); // not ours to stop
    conn.stop_find_path();
    (void)conn.take_sound_cues();
    filters.set_enabled(MapFilter::TargetPath, true);
    settle(filters, conn, state);
    REQUIRE(state.owns_path);
    CHECK(conn.player_path().target_id() == 101);

    // The same kind/id is not enough to keep ownership: the player's new request has a new generation.
    REQUIRE(conn.find_spawn(101));
    (void)conn.take_sound_cues();
    settle(filters, conn, state);
    CHECK_FALSE(state.owns_path);
    filters.set_enabled(MapFilter::TargetPath, false);
    settle(filters, conn, state);
    CHECK(conn.player_path().kind() == game::FindKind::Spawn);
    CHECK(conn.player_path().target_id() == 101);
}

TEST_CASE("Map filters: TargetPath refreshes for a new target incarnation and interaction context") {
    mq_test::World world("hailstone_map_filter_target_identity");
    MapFilters filters;
    filters.set_enabled(MapFilter::Target, true);
    filters.set_enabled(MapFilter::TargetPath, true);
    MapFilterTargetPathState state;
    settle(filters, *world.conn, state);
    REQUIRE(state.owns_path);
    const auto first_generation = state.generation;

    auto& spawn = net::GameConnectionTestAccess::world(*world.conn).spawns.at(10);
    ++spawn.incarnation;
    settle(filters, *world.conn, state);
    CHECK(state.owns_path);
    CHECK(state.incarnation == spawn.incarnation);
    CHECK(state.generation != first_generation);

    const auto generation = state.generation;
    const auto context = state.context;
    ++net::GameConnectionTestAccess::world(*world.conn).spawns.at(1).incarnation;
    settle(filters, *world.conn, state);
    CHECK(state.owns_path);
    CHECK(state.context != context);
    CHECK(state.generation != generation);
}

TEST_CASE("Map filters: a denied TargetPath request is not retried every frame") {
    net::ConnectionOptions options;
    options.plugins = std::make_shared<NoNpcFind>();
    mq_test::World world("hailstone_map_filter_target_denied", options);
    MapFilters filters;
    filters.set_enabled(MapFilter::Target, true);
    filters.set_enabled(MapFilter::TargetPath, true);
    MapFilterTargetPathState state;

    settle(filters, *world.conn, state);
    CHECK(state.requested);
    CHECK_FALSE(state.owns_path);
    CHECK(net::GameConnectionTestAccess::find_request_count(*world.conn) == 0);
    settle(filters, *world.conn, state);
    CHECK(net::GameConnectionTestAccess::find_request_count(*world.conn) == 0);
}

TEST_CASE("Map filters: TargetPath waits for a settled target and paces its requests") {
    test::Rig rig; REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& w = net::GameConnectionTestAccess::world(conn);
    REQUIRE(w.spawns.contains(101));
    REQUIRE(w.spawns.contains(102));
    const auto base = net::GameConnectionTestAccess::find_request_count(conn);
    test::ManualClock clock;
    clock.hold();
    const auto now = clock.source();
    auto requests = [&] { return net::GameConnectionTestAccess::find_request_count(conn) - base; };
    MapFilters filters;
    filters.set_enabled(MapFilter::Target, true);
    filters.set_enabled(MapFilter::TargetPath, true);
    MapFilterTargetPathState state;
    auto update = [&](std::chrono::milliseconds after) {
        clock.advance(after);
        update_map_filter_target_path(filters, conn, state, now());
    };
    using std::chrono::milliseconds;

    w.target_id = 101;
    update(milliseconds(0));
    update(kTargetPathSettle - milliseconds(1));
    CHECK(requests() == 0);
    update(milliseconds(1));
    CHECK(requests() == 1);
    CHECK(state.owns_path);

    // Tabbing through targets asks for none of them until one stays selected.
    for (int i = 0; i < 6; ++i) {
        w.target_id = i % 2 ? 101 : 102;
        update(milliseconds(200));
    }
    CHECK(requests() == 1);
    update(kTargetPathSettle);
    CHECK(requests() == 2);
    CHECK(conn.player_path().target_id() == 101);

    // A settled target still waits for the interval since the last request.
    w.target_id = 102;
    update(milliseconds(0));
    update(kTargetPathSettle);
    CHECK(requests() == 2);
    update(kTargetPathInterval - kTargetPathSettle - milliseconds(1));
    CHECK(requests() == 2);
    update(milliseconds(1));
    CHECK(requests() == 3);
    CHECK(conn.player_path().target_id() == 102);

    // A new interaction context (zoning, a reconnect) is paced the same way.
    conn.stop_find_path(false); // as zoning drops the old zone's path
    ++w.spawns.at(w.player_id).incarnation;
    update(milliseconds(0));
    CHECK(requests() == 3);
    update(kTargetPathInterval);
    CHECK(requests() == 4);
    for (int i = 0; i < 10; ++i) update(kTargetPathInterval); // nothing more while the path is ours
    CHECK(requests() == 4);
}

TEST_CASE("Map filters: spawn types, named filtering and target precedence") {
    MapFilters filters;
    MapFilterSpawnInput input;
    input.player_level = 50;

    game::Spawn rat;
    rat.id = 10;
    rat.name = "a_rat01";
    rat.is_npc = true;
    CHECK_FALSE(filters.spawn_mark(rat, input));
    filters.set_enabled(MapFilter::Npc, true);
    auto mark = filters.spawn_mark(rat, input);
    REQUIRE(mark);
    CHECK(mark->color_filter == MapFilter::Npc);
    CHECK(mark->rgb == 0x404040u);

    game::Spawn named = rat;
    named.id = 11;
    named.name = "#Lord_Rat00";
    filters.set_enabled(MapFilter::Named, true);
    CHECK_FALSE(filters.spawn_mark(rat, input));
    mark = filters.spawn_mark(named, input);
    REQUIRE(mark);
    CHECK(mark->color_filter == MapFilter::Named);

    filters.set_enabled(MapFilter::Npc, false);
    filters.set_enabled(MapFilter::Named, false);
    filters.set_enabled(MapFilter::Target, true);
    input.target = true;
    mark = filters.spawn_mark(rat, input);
    REQUIRE(mark);
    CHECK(mark->target);
    CHECK(mark->color_filter == MapFilter::Target);
    CHECK(mark->rgb == 0xc00000u);

    game::Spawn mount = rat;
    mount.name = "Rat`s_Mount00";
    mount.rider_id = rat.id;
    filters.set_enabled(MapFilter::Mount, true);
    input.target = false;
    mark = filters.spawn_mark(mount, input);
    REQUIRE(mark);
    CHECK(mark->color_filter == MapFilter::Mount);

    game::Spawn pet = rat;
    pet.name = "Rat_pet00";
    pet.pet_owner_id = rat.id;
    filters.set_enabled(MapFilter::Pet, true);
    mark = filters.spawn_mark(pet, input);
    REQUIRE(mark);
    CHECK(mark->color_filter == MapFilter::Pet);

    game::Spawn merc = rat;
    merc.name = "Mercenary01";
    merc.mercenary = true;
    filters.set_enabled(MapFilter::Mercenary, true);
    mark = filters.spawn_mark(merc, input);
    REQUIRE(mark);
    CHECK(mark->color_filter == MapFilter::Mercenary);
}

TEST_CASE("Map filters: corpses, consider colours and group colour precedence") {
    MapFilters filters;
    MapFilterSpawnInput input;
    input.player_level = 50;
    game::Spawn corpse;
    corpse.id = 20;
    corpse.name = "Tester`s_corpse00";
    corpse.is_npc = true;
    corpse.is_corpse = true;
    corpse.deity = 1;
    filters.set_enabled(MapFilter::Corpse, true);
    CHECK_FALSE(filters.spawn_mark(corpse, input));
    filters.set_enabled(MapFilter::PcCorpse, true);
    auto mark = filters.spawn_mark(corpse, input);
    REQUIRE(mark);
    CHECK(mark->color_filter == MapFilter::PcCorpse);
    CHECK(mark->marker_filter == MapFilter::Corpse);
    corpse.deity = 0;
    CHECK_FALSE(filters.spawn_mark(corpse, input));
    filters.set_enabled(MapFilter::NpcCorpse, true);
    mark = filters.spawn_mark(corpse, input);
    REQUIRE(mark);
    CHECK(mark->color_filter == MapFilter::NpcCorpse);

    game::Spawn player;
    player.id = 30;
    player.name = "OtherPlayer";
    player.level = 1;
    filters.set_enabled(MapFilter::Pc, true);
    filters.set_enabled(MapFilter::PcConColor, true);
    mark = filters.spawn_mark(player, input);
    REQUIRE(mark);
    CHECK(mark->rgb == game::con_rgb(game::standard_con(1, 50)));
    filters.set_enabled(MapFilter::Group, true);
    input.group = true;
    mark = filters.spawn_mark(player, input);
    REQUIRE(mark);
    CHECK(mark->color_filter == MapFilter::Group);
    CHECK(mark->rgb == 0x0080c0u);
    filters.set_enabled(MapFilter::Target, true);
    input.target = true;
    mark = filters.spawn_mark(player, input);
    REQUIRE(mark);
    CHECK(mark->color_filter == MapFilter::Target);
}

TEST_CASE("Map filters: custom search replaces type filters but not the target") {
    game::WorldState world;
    world.player_id = 1;
    game::Spawn me;
    me.id = 1;
    me.name = "Tester";
    world.spawns.emplace(1, me);
    game::Spawn rat;
    rat.id = 10;
    rat.name = "a_rat01";
    rat.is_npc = true;
    rat.position = glm::vec3(5.0f, 0.0f, 0.0f);
    world.spawns.emplace(10, rat);
    game::Spawn player;
    player.id = 11;
    player.name = "OtherPlayer";
    player.position = glm::vec3(5.0f, 0.0f, 0.0f);
    world.spawns.emplace(11, player);

    MapFilters filters;
    filters.set_enabled(MapFilter::Pc, true);
    filters.set_custom("npc radius 10");
    const mq::SpawnSearchMatcher matcher(*filters.custom_search(), world, {});
    MapFilterSpawnInput input;
    input.custom = &matcher;
    CHECK(filters.spawn_mark(rat, input).has_value());
    CHECK_FALSE(filters.spawn_mark(player, input).has_value());
    filters.set_enabled(MapFilter::Target, true);
    input.target = true;
    CHECK(filters.spawn_mark(player, input).has_value());
}

TEST_CASE("Map filters: ground objects use their actor label without inventing an item name") {
    MapFilters filters;
    CHECK_FALSE(filters.ground_visible());
    filters.set_enabled(MapFilter::Ground, true);
    CHECK(filters.ground_visible());
    game::GroundObject object;
    object.name = "IT63_ACTORDEF";
    CHECK(MapFilters::ground_label(object) == "IT63");
    object.name = "FORGE_ACTORDEF";
    CHECK(MapFilters::ground_label(object) == "FORGE");
    object.name.clear();
    CHECK(MapFilters::ground_label(object) == "Ground item");
}

TEST_CASE("Spawn search matcher: owns its search across custom-filter replacement") {
    game::WorldState world;
    world.player_id = 1;
    game::Spawn rat;
    rat.id = 10;
    rat.name = "a_rat01";
    rat.is_npc = true;
    world.spawns.emplace(10, rat);
    game::Spawn player;
    player.id = 11;
    player.name = "OtherPlayer";
    world.spawns.emplace(11, player);

    auto search = parse_spawn_search("npc");
    const SpawnSearchMatcher matcher(search, world, {});
    search = parse_spawn_search("pc"); // a reentrant /mapfilter custom replaces the original
    CHECK(matcher.matches(rat));
    CHECK_FALSE(matcher.matches(player));
}

TEST_CASE("Spawn search matcher: prepared nopcnear candidates survive spawn replacement") {
    game::WorldState world;
    world.player_id = 1;
    game::Spawn me;
    me.id = 1;
    me.name = "Tester";
    me.position = glm::vec3(100.0f, 0.0f, 0.0f);
    world.spawns.emplace(1, me);
    game::Spawn nearby;
    nearby.id = 2;
    nearby.name = "Nearby";
    nearby.position = glm::vec3(1.0f, 0.0f, 0.0f);
    world.spawns.emplace(2, nearby);
    game::Spawn rat;
    rat.id = 10;
    rat.name = "a_rat01";
    rat.is_npc = true;
    world.spawns.emplace(10, rat);

    const auto search = parse_spawn_search("npc nopcnear 5");
    const SpawnSearchMatcher matcher(search, world, {});
    CHECK_FALSE(matcher.matches(rat));
    world.spawns.erase(2);
    nearby.id = 3;
    nearby.position = glm::vec3(100.0f, 0.0f, 0.0f);
    world.spawns.emplace(3, nearby);
    CHECK(matcher.matches(rat));
    const auto found = search_spawns(search, world, static_cast<const game::GroupState*>(nullptr));
    REQUIRE(found.size() == 1);
    CHECK(found.front()->id == rat.id);
}
