// game/mounts.h: the rider-mount link (do_stand, the dismount, MakeRiderMountUp, ClearVehicleMountLinks).
#include <doctest/doctest.h>

#include "game/mounts.h"
#include "game/race_models.h"
#include "game/targeting.h"
#include "game/world_state.h"

using namespace hailstone::game;

namespace {

struct Answers final : MountQueries {
    bool models = true, point = true, fits = false;
    double room = 1e30;
    bool new_models_loaded() override { return models; }
    bool mount_point(const Spawn&, const Spawn&) override { return point; }
    bool can_i_fit_here() override { return fits; }
    double room_above() override { return room; }
};

Spawn& add(WorldState& w, std::uint32_t id, const std::string& name, std::uint16_t race, bool npc) {
    Spawn& s = w.spawns[id];
    s.id = id;
    s.name = name;
    s.race = race;
    s.is_npc = npc;
    s.actor_made = true;
    s.tunic_point = true;
    s.heights.height = 6.0f;
    s.heights.avatar_height = 6.0f;
    s.heights.view_height = 7.0f;
    return s;
}

} // namespace

TEST_CASE("mount names: \"%s`s_Mount00\" and the rider from a mount's name") {
    CHECK(mount_name_for("Gennro") == "Gennro`s_Mount00");
    CHECK(rider_name_for_mount("Gennro`s_Mount00") == std::optional<std::string>("Gennro"));
    CHECK_FALSE(rider_name_for_mount("a_horse00").has_value());
    CHECK_FALSE(rider_name_for_mount("`s_Moun").has_value()); // shorter than 9
    CHECK(rider_name_for_mount("`s_Mount0") == std::optional<std::string>(""));
}

TEST_CASE("MakeRiderMountUp links a player and its mount from either side") {
    Answers q;
    for (bool mount_first : {false, true}) {
        WorldState w;
        Spawn& rider = add(w, 10, "Gennro", 1, false);
        Spawn& horse = add(w, 20, "Gennro`s_Mount00", 216, true);
        MountEvents ev;
        make_rider_mount_up(w, mount_first ? horse : rider, q, ev);
        CHECK(rider.mount_id == 20);
        CHECK(horse.rider_id == 10);
        CHECK_FALSE(ev.took_control); // not the local player
        CHECK(ev.messages.empty());
    }
    WorldState w;
    add(w, 10, "Gennro", 1, false);
    Spawn& other = add(w, 20, "Gennro`s_Mount00", 75, true); // not a mount race
    MountEvents ev;
    make_rider_mount_up(w, other, q, ev);
    CHECK(other.rider_id == 0);
}

TEST_CASE("do_stand on the local player takes control of the mount; the dismount gives it back") {
    Answers q;
    WorldState w;
    Spawn& me = add(w, 10, "Gennro", 1, false);
    Spawn& horse = add(w, 20, "Gennro`s_Mount00", 216, true);
    w.player_id = 10;
    me.position = {1.0f, 2.0f, 3.0f};
    me.heading = 100.0f;
    me.speed_run = 0.7f;
    horse.position = {50.0f, 50.0f, 50.0f};
    MountEvents ev;
    do_stand(w, me, &horse, q, ev);
    CHECK(ev.took_control);
    CHECK(w.controlled_id == 20);
    CHECK(horse.position == glm::vec3(1.0f, 2.0f, 3.0f)); // the player's physics block onto the mount
    CHECK(horse.heading == 100.0f);
    CHECK(me.speed_run == 0.0f);
    CHECK(me.heights.view_height == doctest::Approx(7.0f * 0.85f));
    CHECK(horse.heights.height == 6.0f); // ChangeHeight(the rider's Height)

    horse.position = {9.0f, 8.0f, 7.0f};
    horse.speed_run = 1.2f;
    MountEvents off;
    dismount(w, me, off);
    CHECK(off.released_control);
    CHECK(off.close_spell_book);
    CHECK(w.controlled_id == 0);
    CHECK(me.mount_id == 0);
    CHECK(horse.rider_id == 0);
    CHECK(me.position == glm::vec3(9.0f, 8.0f, 7.0f)); // the rider takes the mount's position
    CHECK(horse.speed_run == 0.0f);
}

TEST_CASE("do_stand's refusals: no room (4067) and no rider dag (5524) end the summon mount effect") {
    WorldState w;
    Spawn& me = add(w, 10, "Gennro", 1, false);
    Spawn& horse = add(w, 20, "Gennro`s_Mount00", 216, true);
    w.player_id = 10;
    Answers q;
    q.fits = true;
    q.room = 17.9; // below 3 x AvatarHeight
    MountEvents ev;
    do_stand(w, me, &horse, q, ev);
    CHECK(ev.messages == std::vector<std::uint32_t>{4067});
    CHECK(ev.remove_mount_affect);
    CHECK(me.mount_id == 0);
    q.room = 18.0; // exactly enough
    q.point = false;
    MountEvents ev2;
    do_stand(w, me, &horse, q, ev2);
    CHECK(ev2.messages == std::vector<std::uint32_t>{5524});
    CHECK(me.mount_id == 0);
    // A rider whose model cannot ride (no tunic point, not race 1 / 367 / 522) is refused too.
    q.point = true;
    me.race = 2;
    me.tunic_point = false;
    CHECK(riding_blocked(me));
    MountEvents ev3;
    do_stand(w, me, &horse, q, ev3);
    CHECK(me.mount_id == 0);
    CHECK(horse.rider_id == 10); // set before the attach (do_stand @0x593f7d)
}

TEST_CASE("illusion_mounts: a rider whose model cannot ride still mounts; the mount's dag is still needed") {
    WorldState w;
    w.illusion_mounts = true;
    Spawn& me = add(w, 10, "Gennro", 2, false);
    Spawn& horse = add(w, 20, "Gennro`s_Mount00", 709, true); // Onyx Skystrider
    w.player_id = 10;
    me.tunic_point = false; // an illusion's model with no tunic point
    REQUIRE(riding_blocked(me));
    Answers q;
    MountEvents ev;
    do_stand(w, me, &horse, q, ev);
    CHECK(ev.messages.empty());
    CHECK_FALSE(ev.remove_mount_affect);
    CHECK(me.mount_id == 20);
    CHECK(w.controlled_id == 20);

    // An illusion of a mount race drawn as the human fallback rides too.
    WorldState w2;
    w2.illusion_mounts = true;
    Spawn& me2 = add(w2, 10, "Gennro", 216, false);
    me2.human_actor = true;
    REQUIRE(human_actor_mount(me2));
    Spawn& horse2 = add(w2, 20, "Gennro`s_Mount00", 709, true);
    w2.player_id = 10;
    MountEvents ev2;
    do_stand(w2, me2, &horse2, q, ev2);
    CHECK(me2.mount_id == 20);

    // A mount without a seat for the rider still refuses it (the renderer has nowhere to draw it).
    WorldState w3;
    w3.illusion_mounts = true;
    Spawn& me3 = add(w3, 10, "Gennro", 2, false);
    me3.tunic_point = false;
    Spawn& horse3 = add(w3, 20, "Gennro`s_Mount00", 709, true);
    w3.player_id = 10;
    q.point = false;
    MountEvents ev3;
    do_stand(w3, me3, &horse3, q, ev3);
    CHECK(ev3.messages == std::vector<std::uint32_t>{5524});
    CHECK(me3.mount_id == 0);
}

TEST_CASE("ClearVehicleMountLinks: a mount that goes unseats its rider") {
    Answers q;
    WorldState w;
    Spawn& me = add(w, 10, "Gennro", 1, false);
    Spawn& horse = add(w, 20, "Gennro`s_Mount00", 216, true);
    w.player_id = 10;
    MountEvents ev;
    do_stand(w, me, &horse, q, ev);
    REQUIRE(w.controlled_id == 20);
    MountEvents gone;
    clear_vehicle_mount_links(w, 20, gone);
    CHECK(gone.released_control);
    CHECK(me.mount_id == 0);
    CHECK(w.controlled_id == 0);
}

TEST_CASE("A despawned mount's id is let go of by every spawn, so a new spawn with that id seats nobody") {
    WorldState w;
    Spawn& rider = add(w, 10, "Somebody", 1, false);
    Spawn& other = add(w, 11, "Another", 1, false);
    add(w, 20, "a_horse00", 216, true);
    // Links that name the mount while the mount itself names neither spawn: the despawn clears them anyway.
    rider.mount_id = 20;
    other.rider_id = 20;
    MountEvents ev;
    clear_vehicle_mount_links(w, 20, ev);
    CHECK(rider.mount_id == 0);
    CHECK(other.rider_id == 0);
    w.spawns.erase(20);
    add(w, 20, "a_bat00", 34, true).position = {500.0f, 0.0f, 0.0f}; // EQEmu reuses the id
    w.smooth(0.016f, 1.0);
    CHECK(rider.mount_id == 0);
    CHECK(rider.position.x == 0.0f); // not carried to the bat
}

TEST_CASE("The dismount from a mount that is gone clears the rider's link") {
    WorldState w;
    Spawn& rider = add(w, 10, "Somebody", 1, false);
    rider.mount_id = 20; // no spawn 20
    MountEvents ev;
    dismount(w, rider, ev);
    CHECK(rider.mount_id == 0);
    CHECK_FALSE(ev.released_control);
    CHECK_FALSE(ev.close_spell_book);
}

TEST_CASE("the buff tick mounts the local player on \"<name>`s_Mount\"; an illusion re-seats") {
    Answers q;
    WorldState w;
    Spawn& me = add(w, 10, "Gennro", 1, false);
    add(w, 20, "Gennro`s_Mount00", 216, true);
    w.player_id = 10;
    MountEvents ev;
    summon_mount_tick(w, q, ev);
    CHECK(me.mount_id == 20);
    MountEvents ev2;
    CHECK(change_form_unseat(w, me, ev2) == 20);
    CHECK(me.mount_id == 0);
}

TEST_CASE("the PC target cycle offers a ridden mount as its rider; the NPC cycle skips mounts and ridden spawns") {
    WorldState w;
    w.player_id = 1;
    add(w, 1, "Me", 1, false);
    Spawn& rider = add(w, 10, "Gennro", 1, false);
    Spawn& horse = add(w, 20, "Gennro`s_Mount00", 216, true);
    add(w, 30, "a_horse", 216, true);   // a mount race without a rider
    add(w, 40, "a_rat00", 36, true);
    rider.mount_id = 20;
    horse.rider_id = 10;
    for (auto* s : {&w.spawns[10], &w.spawns[20], &w.spawns[30], &w.spawns[40]}) s->targetable = true;
    auto pcs = cycle_candidates(w, 0, glm::vec3(0.0f), 500.0f, false);
    REQUIRE(pcs.size() == 2); // the rider and its mount
    CHECK(pcs[1].id == 20);
    CHECK(pcs[1].mount_race);
    CHECK(pcs[1].rider == 10);
    TargetManager tm;
    CHECK(get_next_cycle_target(tm, 0, true, pcs) == 10);
    auto npcs = cycle_candidates(w, 1, glm::vec3(0.0f), 500.0f, false);
    REQUIRE(npcs.size() == 1);
    CHECK(npcs[0].id == 40);
    // The local player's own mount (the controlled player) is never offered.
    w.controlled_id = 20;
    CHECK(cycle_candidates(w, 0, glm::vec3(0.0f), 500.0f, false).size() == 1);
}

TEST_CASE("PlayerClient_IsVehicleRace / IsSmallBoatRace: the race records' flags 0x01 and 0x02") {
    for (std::uint16_t race : std::initializer_list<std::uint16_t>{72, 73, 114, 141, 404, 502, 533, 544, 545, 546, 550, 551, 552, 691, 692, 693, 699})
        CHECK(is_vehicle_race(race, 2));
    for (std::uint16_t race : std::initializer_list<std::uint16_t>{1, 216, 547, 548, 549, 694, 698, 700}) CHECK_FALSE(is_vehicle_race(race, 2));
    CHECK(is_small_boat_race(141, 2));
    CHECK(is_small_boat_race(502, 2));
    CHECK_FALSE(is_small_boat_race(72, 0));
    CHECK_FALSE(is_small_boat_race(73, 2));
}

TEST_CASE("ChangeHeight reaches the mount chain: the spawn's mount takes the same height") {
    WorldState w;
    Spawn& rider = add(w, 10, "Gennro", 1, false);
    Spawn& horse = add(w, 20, "Gennro`s_Mount00", 216, true);
    Spawn& other = add(w, 30, "a_horse00", 216, true);
    rider.mount_id = 20;
    horse.rider_id = 10;
    w.change_height(rider, 9.0f, 0.0f, 1.0f);
    CHECK(rider.heights.height == 9.0f);
    CHECK(horse.heights.height == 9.0f);
    CHECK(other.heights.height == 6.0f);
    // The mount's own change does not reach its rider.
    w.change_height(horse, 7.0f, 0.0f, 1.0f);
    CHECK(horse.heights.height == 7.0f);
    CHECK(rider.heights.height == 9.0f);
    // A cycle stops.
    horse.mount_id = 10;
    w.change_height(rider, 5.0f, 0.0f, 1.0f);
    CHECK(horse.heights.height == 5.0f);
}

TEST_CASE("A rider of a rider is carried to the chain's bottom in one frame, whatever the map's order") {
    // Every order of three ids: the top rider, the rider it rides and the mount at the bottom.
    const std::uint32_t ids[][3] = {{1, 2, 3}, {1, 3, 2}, {2, 1, 3}, {2, 3, 1}, {3, 1, 2}, {3, 2, 1},
                                    {40, 7, 1000}, {1000, 40, 7}, {7, 1000, 40}};
    for (const auto& order : ids) {
        CAPTURE(order[0]);
        CAPTURE(order[1]);
        WorldState w;
        w.player_id = 999999;
        Spawn& top = add(w, order[0], "Top", 1, false);
        Spawn& middle = add(w, order[1], "Middle", 1, false);
        Spawn& bottom = add(w, order[2], "Bottom", 216, true);
        top.mount_id = order[1];
        middle.mount_id = order[2];
        top.position = {-50, 0, 0};
        middle.position = {-100, 0, 0};
        bottom.position = {300, 40, 0};
        bottom.heading = 128.0f;
        w.smooth(0.016f, 1.0);
        CHECK(middle.position == bottom.position);
        CHECK(top.position == bottom.position);
        CHECK(top.display_position == bottom.display_position);
        CHECK(top.heading == bottom.heading);
    }
    // Two spawns that name each other: no endless walk; each takes the other's place as before.
    WorldState w;
    w.player_id = 999999;
    Spawn& a = add(w, 1, "A", 1, false);
    Spawn& b = add(w, 2, "B", 1, false);
    a.mount_id = 2;
    b.mount_id = 1;
    a.position = {1, 0, 0};
    b.position = {2, 0, 0};
    w.smooth(0.016f, 1.0);
    CHECK(a.position == b.position);
}
