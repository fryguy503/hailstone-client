// The original's vectors (recorded from eqgame.exe): the pick
// wrapper's exclusions, a left click on a spawn and a click on a door
// (game/clicks.h), plus checks of the DLL half (game/picking.h), which has no
// vectors.
#include <doctest/doctest.h>

#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "game/clicks.h"
#include "game/picking.h"
#include "game/sine_table.h"
#include "vector_files.h"

using namespace hailstone::game;
using hailstone::test::Json;

namespace {

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

std::string diff(const std::vector<std::string>& got, const std::vector<std::string>& want) {
    if (got == want) return {};
    return "calls: got [" + join(got) + "] want [" + join(want) + "]";
}

// ---- 0x48b5c0

std::string check_pick_call(const Json& rec) {
    const Json& in = rec["in"];
    const Json& out = rec["out"];
    PickCallInput p;
    p.world_state = in["world_state"].i32();
    p.camera_type = in["camera_type"].i32();
    p.click_through = static_cast<std::uint8_t>(in["click_through"].i32());
    p.have_local_player = in["have_local_player"].truthy();
    p.view_actor_is_local = in["view_actor_is_local"].truthy();
    p.have_mount = in["have_mount"].truthy();
    p.mount_has_actor = in["mount_has_actor"].truthy();
    p.x = in["x"].i32();
    p.y = in["y"].i32();
    p.screen_left = in["screen_left"].i32();
    p.screen_top = in["screen_top"].i32();
    // Whether the view actor (0xd1fd88) is null is not among the inputs; the record's argument says it.
    p.view_actor_nonnull = out["dll_args"]["view_arg_nonnull"].truthy() || !out["dll_called"].truthy();
    PickCall c = pick_call(p);
    if (c.mount_lookups != out["mount_lookups"].i32()) return std::format("mount_lookups {}", c.mount_lookups);
    if (c.call_dll != out["dll_called"].truthy()) return "dll_called";
    if (c.call_dll) {
        const Json& a = out["dll_args"];
        if (c.exclude_view != a["excludes_view_actor"].truthy()) return "excludes_view_actor";
        if (c.exclude_view != a["view_arg_nonnull"].truthy()) return "view_arg_nonnull";
        if (c.exclude_mount != a["excludes_mount_actor"].truthy()) return "excludes_mount_actor";
        if (c.exclude_mount != a["mount_arg_nonnull"].truthy()) return "mount_arg_nonnull";
        if (c.x != a["x"].i32() || c.y != a["y"].i32()) return std::format("point {} {}", c.x, c.y);
        if (in["required_mask"].u32() != a["required_mask"].u32()) return "required_mask";
        if (in["skip_mask"].u32() != a["skip_mask"].u32()) return "skip_mask";
    }
    const Json& dll = in["dll"];
    bool found = c.call_dll && pick_call_found(dll["result"].i32(), dll["wrote_actor"].truthy()) &&
                 dll["actor_nonnull"].truthy();
    if (found != out["value_nonnull"].truthy()) return "value_nonnull";
    if (found != out["value_is_dll_actor"].truthy()) return "value_is_dll_actor";
    return {};
}

// ---- CEverQuest::LeftClickedOnPlayer

std::vector<std::string> left_click_calls(const LeftClickResult& r) {
    using F = LeftClickCall::Fn;
    std::vector<std::string> out;
    for (const auto& c : r.calls) {
        switch (c.fn) {
        case F::TargetManagerGet: out.push_back("CTargetManager::Get"); break;
        case F::IsTargetable: out.push_back("IsTargetable"); break;
        case F::TipActivate: out.push_back(std::format("CTipWnd::Activate {} {}", c.a, c.b)); break;
        case F::IsGuildmasterForMyClass: out.push_back("IsGuildmasterForMyClass"); break;
        case F::GuildmasterString: out.push_back(std::format("0x5151b0 {}", c.a)); break;
        case F::TrimName: out.push_back("trimName"); break;
        case F::GetString: out.push_back(std::format("getString {} {}", c.a, c.b)); break;
        case F::BuildString: out.push_back(std::format("BuildStringFromEQSTR {}", c.a)); break;
        case F::DspChat: out.push_back(std::format("dsp_chat {} {} {}", c.a, c.b, c.b)); break;
        case F::DropItemOrMoney: out.push_back("DropItemOrMoneyOnPlayer"); break;
        }
    }
    return out;
}

std::vector<std::string> recorded_left_click_calls(const Json& calls) {
    std::vector<std::string> out;
    for (const Json& c : calls.items) {
        const std::string& fn = c["fn"].text;
        if (fn == "CTipWnd::Activate") out.push_back(std::format("{} {} {}", fn, c["tip"].i32(), c["a1"].i32()));
        else if (fn == "0x5151b0") out.push_back(std::format("{} {}", fn, c["class"].i32()));
        else if (fn == "getString") out.push_back(std::format("{} {} {}", fn, c["id"].i32(), c["a1"].i32()));
        else if (fn == "BuildStringFromEQSTR") out.push_back(std::format("{} {}", fn, c["id"].i32()));
        else if (fn == "dsp_chat")
            out.push_back(std::format("{} {} {} {}", fn, c["color"].i32(), c["a2"].i32(), c["a3"].i32()));
        else out.push_back(fn);
    }
    return out;
}

std::string check_left_click(const Json& rec) {
    const Json& in = rec["in"];
    const Json& out = rec["out"];
    LeftClickInput p;
    const std::string& before = in["target_before"].text;
    p.has_old_target = before != "none";
    p.old_target_is_clicked = before == "clicked";
    p.character_list_open = in["character_list_open"].truthy();
    p.click_lines = in["click_lines_option"].truthy();
    p.spawn.is_local_player = in["spawn_is_local_player"].truthy();
    p.spawn.type = static_cast<std::uint8_t>(in["spawn_type"].i32());
    p.spawn.class_id = static_cast<std::uint8_t>(in["class"].i32());
    p.spawn.eb9 = static_cast<std::uint8_t>(in["eb9"].i32());
    p.spawn.race = in["race"].i32();
    p.spawn.targetable = in["targetable"].truthy();
    p.spawn.virtual_58 = in["virtual_58"].truthy();
    p.spawn.guildmaster_for_my_class = in["guildmaster_for_my_class"].truthy();
    p.cursor_holds_something = in["item_on_cursor"].truthy() || in["money_on_cursor"].truthy();
    LeftClickResult r = left_clicked_on_player(p, in["guildmaster_string"].u32());
    std::string after = !r.handled ? before
                        : r.target_is_clicked ? "clicked"
                                              : before;
    if (after != out["target_after"].text) return "target_after " + after;
    bool saved = !out["saved_old_target_id"].is_null();
    if (r.save_old_target != saved) return "saved_old_target_id";
    if (saved && out["saved_old_target_id"].i32() != in["old_target_id"].i32()) return "saved id";
    return diff(left_click_calls(r), recorded_left_click_calls(out["calls"]));
}

// ---- CEverQuest::ClickedSwitch

std::string check_clicked_switch(const Json& rec) {
    const Json& in = rec["in"];
    const Json& out = rec["out"];
    bool is_switch = in["actor_under_cursor"].truthy() && in["actor_is_switch"].truthy();
    if (is_switch != out["value_is_switch"].truthy()) return "value_is_switch";
    std::vector<std::string> want;
    std::int32_t skill_ret = 0;
    for (const Json& c : out["calls"].items) {
        const std::string& fn = c["fn"].text;
        if (fn == "GetAdjustedSkill") {
            skill_ret = c["ret"].i32();
            want.push_back(std::format("{} {}", fn, c["skill"].i32()));
        } else if (fn == "UseSwitch") {
            want.push_back(std::format("{} {} {} {}", fn, c["user_spawn_id"].i32(), c["key_item_id"].i32(),
                                       c["skill_value"].i32()));
        } else {
            want.push_back(fn);
        }
    }
    std::vector<std::string> got;
    if (is_switch) {
        SwitchClickInput p;
        p.switch_damage = in["switch_damage"].u32();
        p.item_on_cursor = in["item_on_cursor"].truthy();
        p.item_base_type = in["item_base_type"].i32();
        p.item_type = in["item_type"].i32();
        p.item_id = in["item_id"].i32();
        SwitchClick s = clicked_switch(p, [&](std::int32_t) { return skill_ret; });
        using F = SwitchClickCall::Fn;
        for (const auto& c : s.calls) {
            switch (c.fn) {
            case F::GetSwitchDamage: got.push_back("GetSwitchDamage"); break;
            case F::ItemBaseGetType: got.push_back("ItemBase_GetType"); break;
            case F::GetItemType: got.push_back("ItemClient::GetItemType"); break;
            case F::ItemBaseGetId: got.push_back("ItemBase_GetID"); break;
            case F::GetAdjustedSkill: got.push_back(std::format("GetAdjustedSkill {}", c.arg)); break;
            }
        }
        // The user: the PcClient's +0x2dd0 player (the test's has spawn id 0x777), else the local player.
        std::int32_t user = in["user_is_pc_2dd0"].truthy() ? 0x777 : in["local_spawn_id"].i32();
        got.push_back(std::format("UseSwitch {} {} {}", user, s.key, s.skill));
    }
    return diff(got, want);
}

// ---- the DLL half

PickTriangle tri(glm::vec3 a, glm::vec3 b, glm::vec3 c) {
    PickTriangle t{a, b, c, {}, 0};
    // The terrain record's normal: normalize((v0 - v1) x (v2 - v1)).
    t.normal = glm::normalize(glm::cross(a - b, c - b));
    return t;
}

// A wall across the x axis at x = d, facing -x (toward a camera at the origin looking along +x).
std::vector<PickTriangle> wall(float d) {
    std::vector<PickTriangle> w;
    glm::vec3 p0(d, -50, -50), p1(d, 50, -50), p2(d, 50, 50), p3(d, -50, 50);
    for (auto t : {tri(p0, p1, p2), tri(p0, p2, p3)}) {
        if (t.normal.x > 0.0f) std::swap(t.v1, t.v2), t.normal = -t.normal;
        w.push_back(t);
    }
    return w;
}

PickActor box_actor(float x, std::uint32_t mask = 2) {
    PickActor a;
    a.mask = mask;
    a.position = {x, 0, 0};
    a.bound_center = a.position;
    a.bound_radius = 2.0f;
    auto faces = std::make_shared<std::vector<PickTriangle>>(wall(x - 1.0f));
    for (auto& t : *faces) {
        for (glm::vec3* v : {&t.v0, &t.v1, &t.v2}) v->y *= 0.02f, v->z *= 0.02f;
    }
    a.triangles = [faces](const PickTriangleSink& sink) {
        for (const auto& t : *faces) sink(t);
    };
    return a;
}

} // namespace

TEST_CASE("vectors: CDisplay_PickActorAtPoint (the pick wrapper's exclusions)") {
    hailstone::test::each_vector("CDisplay_PickActorAtPoint.jsonl", check_pick_call);
}

TEST_CASE("vectors: CEverQuest_LeftClickedOnPlayer") {
    hailstone::test::each_vector("CEverQuest_LeftClickedOnPlayer.jsonl", check_left_click);
}

TEST_CASE("vectors: CEverQuest_ClickedSwitch") {
    hailstone::test::each_vector("CEverQuest_ClickedSwitch.jsonl", check_clicked_switch);
}

TEST_CASE("pick masks (unk_BuildActorInstance)") {
    CHECK(spawn_pick_mask({1, false, false, false}, 0) == 6);
    CHECK(spawn_pick_mask({0, false, false, false}, 0) == 2);
    CHECK(spawn_pick_mask({3, false, false, false}, 0) == 2); // corpses
    CHECK(spawn_pick_mask({0, true, false, false}, 0) == 10);
    CHECK(spawn_pick_mask({1, false, true, false}, 4) == 14);  // a mercenary with /clickthrough 4
    CHECK(spawn_pick_mask({1, false, false, true}, 2) == 14);  // a player's pet with /clickthrough 2
    CHECK(spawn_pick_mask({1, true, false, true}, 0) == 6);    // ... and without: bit 8 cleared
    CHECK(spawn_pick_mask({1, true, true, true}, 2) == 14);
}

TEST_CASE("pick: the cot table and the screen ray") {
    // F = 90: h / 2 = 45, cot(45 units) = cos / sin of 31.64 degrees.
    float k = cmath_cot_folded(45.0f);
    CHECK(k == doctest::Approx(cmath_cos(45.0f) / cmath_sin(45.0f)));
    CHECK(cmath_cot_folded(0.0f) == 1.0e30f);
    CHECK(cmath_cot_folded(45.9f) == k);
    PickCamera cam;
    cam.position = {0, 0, 0};
    cam.right = {0, -1, 0};
    cam.up = {0, 0, 1};
    cam.forward = {1, 0, 0};
    cam.half_width = 400;
    cam.half_height = 300;
    cam.half_fov = 45;
    cam.fov_ratio = 90.0f / 67.5f;
    cam.yon = 500;
    // The pixel left of the centre (x = cx - 0.5) looks straight ahead.
    PickSegment s = screen_ray(cam, 399.5f, 299.5f);
    CHECK(glm::length(s.dir) == doctest::Approx(500.0f));
    CHECK(s.dir.x == doctest::Approx(500.0f));
    CHECK(std::abs(s.dir.y) < 1e-4f);
    // The right edge: the horizontal half angle is atan(1 / k).
    s = screen_ray(cam, 799.5f, 299.5f);
    CHECK(-s.dir.y / s.dir.x == doctest::Approx(1.0f / k).epsilon(1e-4));
}

TEST_CASE("pick: segment against triangle") {
    PickSegment s{{0, 0, 0}, {10, 0, 0}};
    auto w = wall(4.0f);
    auto d = segment_triangle_distance(s, w[0].v0, w[0].v1, w[0].v2);
    auto e = segment_triangle_distance(s, w[1].v0, w[1].v1, w[1].v2);
    CHECK(std::min(d.distance_squared, e.distance_squared) <= kPickHitDistanceSquared);
    CHECK((d.distance_squared <= kPickHitDistanceSquared ? d.t : e.t) == doctest::Approx(0.4f));
    PickSegment far{{0, 0, 0}, {3, 0, 0}};
    d = segment_triangle_distance(far, w[0].v0, w[0].v1, w[0].v2);
    CHECK(d.distance_squared > 0.9f);
    CHECK(d.t == doctest::Approx(1.0f));
}

TEST_CASE("pick: the nearest actor, walls block, masks and passes") {
    PickSegment seg{{0, 0, 0}, {100, 0, 0}};
    auto w = wall(20.0f);
    PickScene scene;
    std::vector<PickActor> actors{box_actor(30.0f), box_actor(10.0f), box_actor(15.0f)};
    scene.actors = actors;
    // Without the wall: the nearest (index 1, at x = 9).
    PickHit h = pick_actor(seg, scene, kClickRequired, kClickSkip);
    CHECK(h.actor == 1);
    CHECK(h.t == doctest::Approx(0.09f));
    CHECK(h.point.x == doctest::Approx(8.999f));
    // Click-through actors are skipped by clicks, not by hover.
    actors[1].mask = 2 | 8;
    scene.actors = actors;
    CHECK(pick_actor(seg, scene, kClickRequired, kClickSkip).actor == 2);
    CHECK(pick_actor(seg, scene, kHoverRequired, kHoverSkip).actor == 1);
    // A wall at x = 12 hides everything behind it; one at 20 only the far actor.
    scene.world = [&](const PickSegment&, const PickTriangleSink& sink) {
        for (const auto& t : wall(12.0f)) sink(t);
    };
    CHECK(pick_actor(seg, scene, kClickRequired, kClickSkip).actor == -1);
    actors[1].mask = 2;
    actors[2].position.x = 25.0f;
    scene.actors = actors;
    CHECK(pick_actor(seg, scene, kClickRequired, kClickSkip).actor == 1);
    // The view actor is excluded.
    scene.exclude_a = 1;
    CHECK(pick_actor(seg, scene, kClickRequired, kClickSkip).actor == -1);
    scene.world = nullptr;
    CHECK(pick_actor(seg, scene, kClickRequired, kClickSkip).actor == 2);
    scene.exclude_a = -1;
    // The nearest without the required bit: later passes find no better, so nothing.
    actors[1].mask = 0;
    scene.actors = actors;
    h = pick_actor(seg, scene, kClickRequired, kClickSkip);
    CHECK(h.actor == -1);
    CHECK(h.passes == 4);
    CHECK(pick_actor(seg, scene, 0, 0).actor == 1);
    // A wall facing away does not block (back faces are skipped).
    actors[1].mask = 2;
    scene.actors = actors;
    scene.world = [&](const PickSegment&, const PickTriangleSink& sink) {
        for (auto t : wall(5.0f)) {
            t.normal = -t.normal;
            sink(t);
        }
    };
    CHECK(pick_actor(seg, scene, kClickRequired, kClickSkip).actor == 1);
    // Passable world triangles block the first two passes only.
    scene.world = [&](const PickSegment&, const PickTriangleSink& sink) {
        for (auto t : wall(5.0f)) {
            t.flags = kTrianglePassable;
            sink(t);
        }
    };
    h = pick_actor(seg, scene, kClickRequired, kClickSkip);
    CHECK(h.actor == 1);
    CHECK(h.passes == 3);
}

TEST_CASE("pick: ties go to the first actor; spheres rank by their centre; bones in mode 0") {
    PickSegment seg{{0, 0, 0}, {100, 0, 0}};
    std::vector<PickActor> actors{box_actor(10.0f), box_actor(10.0f)};
    PickScene scene;
    scene.actors = actors;
    CHECK(pick_actor(seg, scene, 2, 8).actor == 0);
    // A sphere actor (type 2) whose surface is nearer but whose centre is farther loses.
    PickActor sphere;
    sphere.volume = 2;
    sphere.position = {12, 0, 0};
    sphere.sphere_radius = 5;
    actors = {sphere, box_actor(10.0f)};
    scene.actors = actors;
    CHECK(pick_actor(seg, scene, 2, 8).actor == 1);
    // A hierarchical actor needs a bone sphere near the segment in mode 0, not in mode 1.
    PickActor body = box_actor(10.0f);
    body.hierarchical = true;
    body.bones = {{{10, 0, 3}, {10, 0, 5}}}; // above the segment
    actors = {body};
    scene.actors = actors;
    PickHit h = pick_actor(seg, scene, 2, 8);
    CHECK(h.actor == 0);
    CHECK(h.passes == 2);
    actors[0].bones = {{{10, 0, -1}, {10, 0, 1}}};
    scene.actors = actors;
    h = pick_actor(seg, scene, 2, 8);
    CHECK(h.actor == 0);
    CHECK(h.passes == 1);
}

TEST_CASE("LMouseUp keeps lockpicks and keys on the cursor for ClickedSwitch") {
    using hailstone::game::cursor_item_is_switch_key;
    CHECK(cursor_item_is_switch_key(12));
    CHECK(cursor_item_is_switch_key(0x21));
    CHECK(cursor_item_is_switch_key(0x27));
    CHECK_FALSE(cursor_item_is_switch_key(0));
    CHECK_FALSE(cursor_item_is_switch_key(11));
    CHECK_FALSE(cursor_item_is_switch_key(14));
}
