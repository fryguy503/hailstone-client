// The movement key handlers of __ExecuteCmd (game/mechanics/controls.h)
// and the targeting commands (game/targeting.h).
#include <doctest/doctest.h>

#include <algorithm>
#include <numbers>
#include <utility>

#include "game/mechanics/controls.h"

using namespace hailstone::game::mechanics;

TEST_CASE("PITCHUP / PITCHDOWN hold the look keys and clear the ramps, even while frozen") {
    ControlsState s;
    s.look_up_count = 5;
    s.look_down_count = 7;
    // Case 0x11 runs before the frozen gate.
    CHECK(execute_movement_command(s, kCmdPitchUp, true, false));
    CHECK(s.keys.look_up);
    CHECK(s.look_up_count == 5);   // a press keeps its own ramp
    CHECK(s.look_down_count == 0); // and clears the other's
    // PITCHDOWN releases PITCHUP and clears its ramp.
    s.look_up_count = 3;
    CHECK(execute_movement_command(s, kCmdPitchDown, true, true));
    CHECK(s.keys.look_down);
    CHECK_FALSE(s.keys.look_up);
    CHECK(s.look_up_count == 0);
    s.look_down_count = 4;
    CHECK(execute_movement_command(s, kCmdPitchDown, false, true));
    CHECK_FALSE(s.keys.look_down);
    CHECK(s.look_down_count == 0);
    s.look_up_count = 2;
    execute_movement_command(s, kCmdPitchUp, true, true);
    CHECK(execute_movement_command(s, kCmdPitchUp, false, true));
    CHECK_FALSE(s.keys.look_up);
    CHECK(s.look_up_count == 0);
}

TEST_CASE("CENTERVIEW releases the look and camera pitch keys on a press, unless frozen") {
    ControlsState s;
    s.keys.look_up = s.keys.camera_pitch_plus = s.keys.camera_pitch_minus = true;
    s.look_up_count = 6;
    s.look_down_count = 2;
    CHECK_FALSE(execute_movement_command(s, kCmdCenterView, true, false));
    CHECK(s.keys.look_up);
    CHECK_FALSE(execute_movement_command(s, kCmdCenterView, false, true));
    CHECK(s.keys.look_up);
    CHECK(execute_movement_command(s, kCmdCenterView, true, true));
    CHECK_FALSE(s.keys.look_up);
    CHECK_FALSE(s.keys.camera_pitch_plus);
    CHECK_FALSE(s.keys.camera_pitch_minus);
    CHECK(s.look_up_count == 0);
    CHECK(s.look_down_count == 0);
}

TEST_CASE("Case 20: the zoom and camera keys hold their flag, even while frozen") {
    ControlsState s;
    const std::pair<int, bool ControlKeys::*> keys[] = {
        {kCmdZoomIn, &ControlKeys::zoom_in},
        {kCmdZoomOut, &ControlKeys::zoom_out},
        {kCmdForwardCam, &ControlKeys::camera_pan_plus},
        {kCmdBackCam, &ControlKeys::camera_pan_minus},
        {kCmdRightCam, &ControlKeys::camera_heading_plus},
        {kCmdLeftCam, &ControlKeys::camera_heading_minus},
        {kCmdPitchUpCam, &ControlKeys::camera_pitch_plus},
        {kCmdPitchDownCam, &ControlKeys::camera_pitch_minus},
    };
    for (auto [command, flag] : keys) {
        CAPTURE(command);
        CHECK(is_movement_command(command));
        CHECK(execute_movement_command(s, command, true, false));
        CHECK(s.keys.*flag);
        CHECK(execute_movement_command(s, command, false, false));
        CHECK_FALSE(s.keys.*flag);
    }
    for (int c = 0; c <= 7; ++c) CHECK(is_movement_command(c));
    CHECK(is_movement_command(kCmdPitchUp));
    CHECK(is_movement_command(kCmdCenterView));
    CHECK_FALSE(is_movement_command(8));  // MOUSELOOK
    CHECK_FALSE(is_movement_command(12)); // DUCK
    CHECK_FALSE(is_movement_command(22)); // TOGGLECAM
    CHECK_FALSE(is_movement_command(327));
    CHECK_FALSE(is_movement_command(334));
}

TEST_CASE("Key-up handlers clear the held flag and the key's ramp, never the speed") {
    ControlsState s;
    s.controlled.speed_run = 0.7f;
    CHECK(execute_movement_command(s, kCmdForward, true, true));
    CHECK(s.keys.forward);
    s.forward_count = 5;
    execute_movement_command(s, kCmdForward, false, true);
    CHECK_FALSE(s.keys.forward);
    CHECK(s.forward_count == 0);
    CHECK(s.controlled.speed_run == 0.7f);

    s.keys.turn_left = true;
    s.turn_left_count = 7;
    s.turn_right_count = 3;
    s.keys.strafe_left = true;
    execute_movement_command(s, kCmdLeft, false, true);
    CHECK_FALSE(s.keys.turn_left);
    CHECK(s.turn_left_count == 0);
    CHECK(s.turn_right_count == 3);
    CHECK_FALSE(s.keys.strafe_left); // the turn keys release the strafe keys too
}

TEST_CASE("Pressing FORWARD ends forward/back autorun and releases BACK") {
    ControlsState s;
    s.autorun = 1;
    s.keys.backward = true;
    s.back_count = 4;
    execute_movement_command(s, kCmdForward, true, true);
    CHECK(s.autorun == 0);
    CHECK_FALSE(s.keys.backward);
    CHECK(s.back_count == 0);
    // Turn autorun (4 / 8) is left alone by FORWARD.
    s.autorun = 8;
    execute_movement_command(s, kCmdBack, true, true);
    CHECK(s.autorun == 8);
    CHECK_FALSE(s.keys.forward);
}

TEST_CASE("Pressing a turn key releases the other one and clears its ramp") {
    ControlsState s;
    s.keys.turn_left = true;
    s.turn_left_count = 9;
    s.autorun = 4 | 1;
    execute_movement_command(s, kCmdRight, true, true);
    CHECK(s.keys.turn_right);
    CHECK_FALSE(s.keys.turn_left);
    CHECK(s.turn_left_count == 0);
    CHECK(s.autorun == 0); // bits 0xc set: autorun ends
    s.autorun = 1;
    execute_movement_command(s, kCmdLeft, true, true);
    CHECK(s.autorun == 1);
    CHECK_FALSE(s.keys.turn_right);
}

TEST_CASE("AUTORUN toggles forward autorun; refused commands change nothing") {
    ControlsState s;
    s.keys.backward = true;
    execute_movement_command(s, kCmdAutorun, true, true);
    CHECK(s.autorun == 1);
    CHECK_FALSE(s.keys.backward);
    execute_movement_command(s, kCmdAutorun, false, true);
    CHECK(s.autorun == 1);
    execute_movement_command(s, kCmdAutorun, true, true);
    CHECK(s.autorun == 0);
    CHECK_FALSE(execute_movement_command(s, kCmdForward, true, false));
    CHECK_FALSE(s.keys.forward);
    execute_movement_command(s, kCmdJump, true, true);
    CHECK(s.jump_key == 1);
}

#include "game/collision.h"
#include "game/targeting.h"
#include "game/world_state.h"
#include "render/camera.h"

namespace {

hailstone::game::WorldState world_with(std::initializer_list<std::tuple<std::uint32_t, float, bool, bool>> spawns) {
    hailstone::game::WorldState w;
    w.player_id = 1;
    w.spawns[1].id = 1;
    for (auto [id, x, npc, corpse] : spawns) {
        auto& s = w.spawns[id];
        s.id = id;
        s.position = {x, 0.0f, 0.0f};
        s.is_npc = npc;
        s.is_corpse = corpse;
    }
    return w;
}

} // namespace

TEST_CASE("TARGETNPC takes the nearest NPC, CYCLENPCTARGETS the next unvisited, then around again") {
    using namespace hailstone::game;
    WorldState w = world_with({{10, 30.0f, true, false}, {11, 10.0f, true, false}, {12, 20.0f, true, false},
                               {13, 600.0f, true, false}, {14, 5.0f, false, false}, {15, 1.0f, true, true}});
    TargetManager tm;
    glm::vec3 eye(0.0f);
    auto t = target_key_command(tm, 32, w, eye);
    REQUIRE(t);
    CHECK(*t == 11u);
    w.target_id = *t;
    CHECK(*target_key_command(tm, 33, w, eye) == 12u);
    w.target_id = 12;
    CHECK(tm.previous == 11u);
    CHECK(*target_key_command(tm, 33, w, eye) == 10u);
    w.target_id = 10;
    // All visited: the first again.
    CHECK(*target_key_command(tm, 33, w, eye) == 11u);
    w.target_id = 11;
    // TARGETNPC restarts at the nearest (already the target: no change).
    CHECK_FALSE(target_key_command(tm, 32, w, eye));
    CHECK(*target_key_command(tm, 34, w, eye) == 15u); // TARGETCORPSE
    CHECK(*target_key_command(tm, 30, w, eye) == 14u); // TARGETPC
}

TEST_CASE("The PC and NPC cycles skip the spawns IsInvisible hides (Hide Traders), corpses do not") {
    using namespace hailstone::game;
    WorldState w = world_with({{20, 5.0f, false, false}, {21, 10.0f, false, false}, {22, 3.0f, true, false},
                               {23, 4.0f, true, false}, {24, 2.0f, false, true}});
    TargetManager tm;
    glm::vec3 eye(0.0f);
    // 0x5c58c0 asks the local player's IsInvisible for modes 0 and 1 (0x5c5a04): a hidden trader (20) is never
    // offered to TARGETPC / CYCLEPCTARGETS, so the next player is.
    tm.invisible = [](const Spawn& s) { return s.id == 20 || s.id == 22 || s.id == 24; };
    CHECK(*target_key_command(tm, 30, w, eye) == 21u);
    w.target_id = 21;
    CHECK_FALSE(target_key_command(tm, 31, w, eye)); // only 21 left: no change
    CHECK(*target_key_command(tm, 32, w, eye) == 23u);
    CHECK(*target_key_command(tm, 34, w, eye) == 24u); // mode 2 returns before the test
    tm.invisible = nullptr;
    CHECK(*target_key_command(tm, 30, w, eye) == 20u);
}

TEST_CASE("The red con tip's test: level 10 or below, the target more than two above, once per target") {
    using namespace hailstone::game;
    TargetManager tm;
    CHECK_FALSE(red_con_tip_due(tm, 50, 10, 12)); // two above: no
    CHECK_FALSE(red_con_tip_due(tm, 50, 11, 20)); // above level 10: no
    CHECK(red_con_tip_due(tm, 50, 10, 13));
    CHECK(tm.red_con == 50u);
    CHECK_FALSE(red_con_tip_due(tm, 50, 10, 13)); // the same target again
    CHECK(red_con_tip_due(tm, 51, 1, 4));
    CHECK(red_con_tip_due(tm, 50, 1, 4));         // back to the first: offered again
}

TEST_CASE("TOGGLETARGET swaps between yourself and the remembered target") {
    using namespace hailstone::game;
    WorldState w = world_with({{10, 30.0f, true, false}});
    TargetManager tm;
    glm::vec3 eye(0.0f);
    w.target_id = 10;
    CHECK(*target_key_command(tm, 273, w, eye) == 1u);
    CHECK(tm.remembered == 10u);
    w.target_id = 1;
    CHECK(*target_key_command(tm, 273, w, eye) == 10u);
    w.target_id = 10;
    CHECK(*target_key_command(tm, 273, w, eye) == 1u);
    // Nothing remembered (or gone): no change; keyboard in chat mode: nothing.
    w.target_id = 1;
    w.spawns.erase(10);
    CHECK_FALSE(target_key_command(tm, 273, w, eye));
    w.target_id = 0;
    CHECK_FALSE(target_key_command(tm, 273, w, eye, false));
    CHECK(*target_key_command(tm, 273, w, eye) == 1u);
}

TEST_CASE("TOGGLETWOTARGETS swaps the target and the one before it") {
    using namespace hailstone::game;
    WorldState w = world_with({{10, 30.0f, true, false}, {11, 40.0f, true, false}});
    TargetManager tm;
    glm::vec3 eye(0.0f);
    CHECK_FALSE(target_key_command(tm, 343, w, eye)); // nothing before
    CHECK(tm.previous == 0u);
    w.target_id = 11;
    tm.previous = 10; // (a target key moved from 10 to 11)
    CHECK(*target_key_command(tm, 343, w, eye) == 10u);
    CHECK(tm.previous == 11u);
    w.target_id = 10;
    CHECK(*target_key_command(tm, 343, w, eye) == 11u);
    // The one before is gone: no change, but the current target becomes the one before.
    w.target_id = 11;
    tm.previous = 10;
    w.spawns.erase(10);
    CHECK_FALSE(target_key_command(tm, 343, w, eye));
    CHECK(tm.previous == 11u);
}

TEST_CASE("Untargetable, banner and non-cyclable spawns are not cycled") {
    using namespace hailstone::game;
    WorldState w = world_with({{10, 3.0f, true, false}, {11, 4.0f, true, false}, {12, 5.0f, true, false},
                               {13, 9.0f, true, false}});
    w.spawns[10].targetable = false;
    w.spawns[11].race = 586;
    w.spawns[12].cyclable = false;
    TargetManager tm;
    CHECK(*target_key_command(tm, 32, w, glm::vec3(0.0f)) == 13u);
}

TEST_CASE("Hailstone: the NPC cycle skips players' pets and mercenaries unless skip_pets is off") {
    using namespace hailstone::game;
    WorldState w = world_with({{10, 30.0f, true, false}, {11, 5.0f, true, false}, {12, 8.0f, true, false},
                               {13, 9.0f, true, false}, {14, 12.0f, true, false}, {20, 3.0f, false, false}});
    w.spawns[11].pet_owner_id = 1;  // our pet
    w.spawns[12].pet_owner_id = 20; // another player's
    w.spawns[13].mercenary = true;
    w.spawns[14].pet_owner_id = 10; // an NPC's pet stays
    glm::vec3 eye(0.0f);
    CHECK(is_player_pet(w, w.spawns[11]));
    CHECK(is_player_pet(w, w.spawns[12]));
    CHECK(is_player_pet(w, w.spawns[13]));
    CHECK_FALSE(is_player_pet(w, w.spawns[14]));
    TargetManager tm;
    CHECK(tm.skip_pets);
    CHECK(*target_key_command(tm, 32, w, eye) == 14u);
    w.target_id = 14;
    CHECK(*target_key_command(tm, 33, w, eye) == 10u);
    w.target_id = 10;
    CHECK(*target_previous_npc(tm, w, eye) == 14u);
    // The original's cycle: pets included.
    TargetManager orig;
    orig.skip_pets = false;
    w.target_id = 0;
    CHECK(*target_key_command(orig, 32, w, eye) == 11u);
}

TEST_CASE("Hailstone: the NPC cycle takes only the NPCs in sight when in_sight is set") {
    using namespace hailstone::game;
    WorldState w = world_with({{10, 5.0f, true, false}, {11, 8.0f, true, false}, {12, 12.0f, true, false},
                               {20, 3.0f, false, false}});
    glm::vec3 eye(0.0f);
    TargetManager tm;
    CHECK_FALSE(tm.in_sight); // unset: every NPC, as the original
    CHECK(*target_key_command(tm, 32, w, eye) == 10u);
    // 10 and 12 behind a wall; the player 20 is not asked about (the PC cycle is not filtered).
    std::vector<std::uint32_t> asked;
    tm.in_sight = [&](const Spawn& s) {
        asked.push_back(s.id);
        return s.id == 11;
    };
    w.target_id = 0;
    CHECK(*target_key_command(tm, 32, w, eye) == 11u);
    w.target_id = 11;
    CHECK_FALSE(target_key_command(tm, 33, w, eye)); // the only one in sight is the target already
    CHECK_FALSE(target_previous_npc(tm, w, eye));
    CHECK(std::find(asked.begin(), asked.end(), 20u) == asked.end());
    CHECK(*target_key_command(tm, 30, w, eye) == 20u);
    // Nobody in sight: the target stays.
    tm.in_sight = [](const Spawn&) { return false; };
    w.target_id = 0;
    CHECK_FALSE(target_key_command(tm, 32, w, eye));
}

TEST_CASE("Hailstone: NPC target keys filter camera view and walls independently") {
    using namespace hailstone::game;
    auto world = world_with({{10, 20.0f, true, false}, {11, -6.0f, true, false}, {12, 10.0f, true, false},
                             {13, 6.0f, true, false}, {14, 120.0f, true, false}, {15, 501.0f, true, false}});
    world.spawns[12].position.y = 4.0f;
    world.spawns[13].position.y = 10.0f;
    // The middle of this wall is on the ray to 12, while the ray to 10 passes beside it.
    const auto wall = CollisionMesh::from_triangles({
        CollisionTriangle::make({5, 1, -3}, {5, 3, -3}, {5, 3, 3}, 0),
        CollisionTriangle::make({5, 1, -3}, {5, 3, 3}, {5, 1, 3}, 0),
    });
    hailstone::render::FlyCamera camera;
    auto view_projection = camera.projection(1920, 1080, 100.0f) * camera.view();
    const std::function<bool(const glm::vec3&)> clear_wall = [&](const glm::vec3& point) {
        return wall.line_of_sight(camera.position, point);
    };
    TargetManager targets;
    const auto cycle = [&](bool offscreen, bool through_walls) {
        targets.in_sight = [&, offscreen, through_walls](const Spawn& spawn) {
            return npc_in_target_view(spawn, offscreen ? nullptr : &view_projection,
                                      through_walls ? std::function<bool(const glm::vec3&)>{} : clear_wall);
        };
        std::vector<std::uint32_t> ids;
        for (const auto& candidate : cycle_candidates(world, 1, camera.position, 500.0f, false, {}, targets.in_sight))
            ids.push_back(candidate.id);
        return ids;
    };
    CHECK(cycle(false, false) == std::vector<std::uint32_t>{10});
    CHECK(target_key_command(targets, 32, world, camera.position) == 10u);
    CHECK(cycle(false, true) == std::vector<std::uint32_t>{10, 12});
    CHECK(target_key_command(targets, 33, world, camera.position) == 12u);
    CHECK(cycle(true, false) == std::vector<std::uint32_t>{10, 11, 13, 14});
    CHECK(target_previous_npc(targets, world, camera.position) == 11u);
    CHECK(cycle(true, true) == std::vector<std::uint32_t>{10, 11, 12, 13, 14});
    camera.yaw = std::numbers::pi_v<float>;
    view_projection = camera.projection(1920, 1080, 100.0f) * camera.view();
    CHECK(cycle(false, false) == std::vector<std::uint32_t>{11});
}

TEST_CASE("Hailstone: a partly visible NPC needs a clear point inside the camera view") {
    using namespace hailstone::game;
    const hailstone::render::FlyCamera camera;
    const auto view_projection = camera.projection(1920, 1080, 100.0f) * camera.view();
    Spawn spawn;
    spawn.position = {-10.0f, 0.0f, 0.0f};
    spawn.display_valid = true;
    spawn.display_position = {10.0f, 0.0f, 0.0f};
    spawn.heights.avatar_height = 3.0f;
    spawn.heights.view_height = 6.0f;
    const auto over_wall = [](const glm::vec3& point) { return point.z > 2.0f; };
    CHECK(npc_in_target_view(spawn, &view_projection, over_wall));

    spawn.display_position.x = 5.0f; // its visible center is blocked and its clear eye is above the screen
    CHECK_FALSE(npc_in_target_view(spawn, &view_projection, over_wall));
    CHECK(npc_in_target_view(spawn, nullptr, over_wall));
}
