// ClientApp: Mouse picking, clicks on spawns, doors and ground objects, zone-line teleports.

#include "app/client_app_internal.h"

namespace hailstone::app::client_internal {

SpawnPickSource ClientApp::spawn_pick_source(std::uint32_t id, const game::Spawn& spawn) const {
    SpawnPickSource source;
    source.position = spawn.display_valid ? spawn.display_position : spawn.position;
    source.size = spawn.size;
    if (!actor_renderer_) return source;
    // A body-less skeleton (race 127's IVM): its model, drawn or past the actor clip.
    const auto model = actor_renderer_->model_for(spawn);
    source.bodiless = model && assets::bodiless_skeleton(*model);
    source.bounds = [this, id](glm::vec3& origin, float& radius) { return actor_renderer_->spawn_bounds(id, origin, radius); };
    return source;
}

bool ClientApp::plate_input_blocked(bool ignore_right_button) const {
    return !ui_.visible() || !window_.focused() || !conn_ || screen_ != Screen::InGame || !player_ready_ || zone_settling() ||
           conn_->state() != net::ConnectionState::InZone || conn_->interface_locked() || conn_->player_down() ||
           conn_->player_status().stand_state == net::kStandFreeze || conn_->placement().active() ||
           conn_->inventory().cursor() || ui_host_.cursor_button || ui_host_.cursor_spell ||
           ui_.wants_mouse() || mouse_look_.active || (!ignore_right_button && window_.input().right_button) ||
           ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

bool ClientApp::nameplate_spawn_clickable(const game::Spawn& spawn) const {
    if (!conn_ || spawn.id == 0) return false;
    game::MaskSpawn mask;
    mask.type = spawn.is_corpse ? 2 : spawn.is_npc ? 1 : 0;
    mask.click_through = spawn.click_through;
    mask.mercenary = spawn.mercenary;
    if (spawn.pet_owner_id != 0) {
        const auto owner = conn_->world().spawns.find(spawn.pet_owner_id);
        mask.owner_is_player = owner != conn_->world().spawns.end() && !owner->second.is_npc && !owner->second.is_corpse;
    }
    return nameplate_mouse_eligible(mask, conn_->click_through_mask(), spawn.id == conn_->world().player_id, camera_type_);
}

std::optional<float> ClientApp::nameplate_pixel_depth(glm::vec2 at) const {
    const auto unknown = std::numeric_limits<float>::quiet_NaN();
    if (!collision_ || plate_hit_view_.z <= 0.0f || plate_hit_view_.w <= 0.0f) return unknown;
    const auto query = nameplate_click_ray(plate_inverse_view_, plate_hit_view_, plate_camera_, at);
    if (!query) return unknown;
    const auto& ray = *query;
    std::vector<game::PickActor> actors;
    const auto see_invisible = viewer_see_invisible_rank();
    const auto hidden = hidden_spawn();
    for (const auto& [id, spawn] : conn_->world().spawns) {
        if (id == hidden || conn_->spawn_invisible(spawn, see_invisible)) continue;
        SpawnPickSource source = spawn_pick_source(id, spawn);
        source.posed = [this, id](game::PickActor& actor, const glm::vec3& origin) {
            render::ActorRenderer::SpawnPose pose;
            if (!actor_renderer_->spawn_pose(id, pose)) return false;
            actor.position = origin;
            actor.bound_center = pose.center;
            actor.bound_radius = pose.radius;
            auto triangles = std::make_shared<std::vector<glm::vec3>>(std::move(pose.triangles));
            actor.triangles = [triangles](const game::PickTriangleSink& sink) {
                for (std::size_t i = 0; i + 2 < triangles->size(); i += 3)
                    sink({(*triangles)[i], (*triangles)[i + 1], (*triangles)[i + 2], {}, 0});
            };
            return true;
        };
        add_nameplate_depth_spawn(actors, source, ray);
    }
    if (world_objects_) {
        std::vector<WorldObjects::PickRef> unused;
        world_objects_->pick_actors(actors, unused);
    }
    game::PickScene scene;
    scene.world = pick_world();
    scene.actors = actors;
    return nameplate_scene_depth(ray, scene, plate_forward_);
}

std::optional<std::uint32_t> ClientApp::nameplate_at(glm::vec2 at) const {
    if (plate_input_blocked() || !pick_camera_valid_ || !view_area_.contains(at.x, at.y) ||
        plate_hit_connection_ != conn_.get() || plate_hit_zone_ != conn_->zone_id() ||
        plate_hit_serial_ != conn_->world().spawn_set_serial || Clock::now() - plate_hit_time_ > std::chrono::milliseconds(250) ||
        plate_hit_view_ != glm::vec4(view_area_.x, view_area_.y, view_area_.w, view_area_.h))
        return std::nullopt;
    const auto see_invisible = viewer_see_invisible_rank();
    std::optional<float> scene_depth;
    bool scene_read = false;
    return pick_nameplate(plate_hits_, at, [&](const NameplateHit& hit) {
        const auto spawn = conn_->world().spawns.find(hit.id);
        if (spawn == conn_->world().spawns.end() || !nameplate_spawn_clickable(spawn->second) || !game::is_targetable(spawn->second) || !spawn->second.show_name ||
            spawn->second.body_type == 11 || conn_->spawn_invisible(spawn->second, see_invisible)) return false;
        if (!automation_ || !automation_->nameplates().owner_valid(hit.owner) || hit.incarnation != spawn->second.incarnation) return false;
        if (hit.through_walls) return true;
        if (!scene_read) { scene_depth = nameplate_pixel_depth(at); scene_read = true; }
        if (!nameplate_depth_visible(hit.depth, scene_depth)) return false;
        return collision_ && collision_->line_of_sight(pick_camera_.position, hit.head) &&
               (!world_objects_ || world_objects_->line_of_sight(pick_camera_.position, hit.head));
    });
}

bool ClientApp::activate_nameplate(std::uint32_t id) {
    if (plate_input_blocked() || plate_hit_connection_ != conn_.get() || plate_hit_zone_ != conn_->zone_id() ||
        plate_hit_serial_ != conn_->world().spawn_set_serial) return false;
    const auto spawn = conn_->world().spawns.find(id);
    if (spawn == conn_->world().spawns.end() || !nameplate_spawn_clickable(spawn->second) || !game::is_targetable(spawn->second) || !spawn->second.show_name ||
        spawn->second.body_type == 11 || conn_->spawn_invisible(spawn->second, viewer_see_invisible_rank())) return false;
    const auto hit = std::find_if(plate_hits_.begin(), plate_hits_.end(), [id](const NameplateHit& h) { return h.id == id; });
    if (hit == plate_hits_.end()) return false;
    const NameplateHit observation = *hit;
    if (!automation_ || !automation_->nameplates().owner_valid(observation.owner) ||
        observation.incarnation != spawn->second.incarnation) return false;
    if (!observation.through_walls && !nameplate_depth_visible(observation.depth,
        nameplate_pixel_depth({window_.input().mouse_x, window_.input().mouse_y}))) return false;
    if (!observation.through_walls && (!collision_ || !collision_->line_of_sight(pick_camera_.position, observation.head) ||
        (world_objects_ && !world_objects_->line_of_sight(pick_camera_.position, observation.head)))) return false;
    const std::uint32_t old = conn_->world().target_id;
    if (old != id && conn_->world().spawns.contains(old)) target_manager_.previous = old;
    conn_->set_target(id); // a physical click selects only: never give an item, attack, or run a right-click action
    return true;
}

bool ClientApp::click_world(const PickTarget& hit) {
    if (!world_objects_ || !conn_) return false;
    // CEverQuest::LMouseUp without a spawn: an item on the cursor is a key for ClickedSwitch when it is a key
    // or a lockpick (item types 12, 33 and 39), else it goes to the ground; with an empty cursor, a ground item
    // under the cursor is picked up (within 20), anything else goes to ClickedSwitch (doors).
    if (const net::Item* held = conn_->inventory().cursor()) {
        bool key = held->item_type == 12 || held->item_type == 33 || held->item_type == 39;
        return key && hit.kind == PickTarget::Kind::Door && use_door(static_cast<std::uint8_t>(hit.id));
    }
    if (hit.kind == PickTarget::Kind::Door) return use_door(static_cast<std::uint8_t>(hit.id));
    // CDisplay::GetClickedActor with climb: a ladder (actor type 6) is climbed (Ladder_MountClickedLadder).
    if (hit.kind == PickTarget::Kind::Ladder) {
        if (hit.id >= zone_ladders_.size()) return false;
        climb_ladder(zone_ladders_[hit.id].ladder);
        return true;
    }
    if (hit.kind != PickTarget::Kind::Object) return false;
    if (auto ladder = world_objects_->ladder(*conn_, hit.id)) {
        climb_ladder(*ladder);
        return true;
    }
    return conn_->click_object(hit.id);
}

void ClientApp::climb_ladder(const game::mechanics::LadderSwitch& ladder) {
    ZoneMoveWorld world(collision_ ? &*collision_ : nullptr, world_objects_.get(), zone_ ? &zone_->regions : nullptr);
    world.set_animated(animated_objects_ ? &*animated_objects_ : nullptr);
    bool too_far = false;
    if (movement_.mount_ladder(ladder, &world, floor_offset_, too_far)) {
        yaw_ = game::heading_to_yaw(movement_.controller().body.heading);
        pitch_ = game::heading_to_yaw(movement_.controller().body.camera_angle);
        feet_ = movement_.controller().body.feet;
    }
    if (too_far) game_view_.chat.add(ui::ChatChannel::System, conn_->strings().format(game::mechanics::kStrLadderTooFar, {}));
}

bool ClientApp::use_door(std::uint8_t id) {
    if (!conn_ || !world_objects_) return false;
    const game::Door* d = conn_->door(id);
    if (!d) return false;
    game::SwitchClickInput in;
    in.switch_damage = game::switch_damage(d->type);
    if (const net::Item* held = conn_->inventory().cursor()) {
        in.item_on_cursor = true;
        in.item_base_type = held->item_class;
        in.item_type = held->item_type;
        in.item_id = static_cast<std::int32_t>(held->id);
    }
    const game::mechanics::Character* ch = stats_ui_ ? stats_ui_->character() : nullptr;
    auto use = game::clicked_switch(in, [&](std::int32_t skill) {
        return ch ? ch->adjusted_skill(static_cast<std::uint32_t>(skill)) : conn_->skill_value(static_cast<std::uint32_t>(skill));
    });
    return conn_->click_door(id, world_objects_->door_radius(id), use.key, use.skill);
}

bool ClientApp::door_reachable(std::uint8_t id) const {
    return conn_ && world_objects_ && conn_->door_in_reach(id, world_objects_->door_radius(id));
}

ClientApp::PickTarget ClientApp::pick_at(float x, float y, std::uint32_t required, std::uint32_t skip, const char* why) {
    PickTarget out;
    if (!conn_ || !pick_camera_valid_) return out;
    // The point in the 3D view (/viewport): nothing is picked outside it.
    if (!view_area_.contains(x, y)) return out;
    x -= view_area_.x;
    y -= view_area_.y;
    const auto& world = conn_->world();
    // 0x48b5c0: the view actor (the player's) is excluded in first person or with /clickthrough bit 0;
    // mounts are not modelled.
    game::PickCallInput call_in;
    call_in.world_state = player_ready_ ? 1 : 0;
    call_in.camera_type = camera_type_;
    call_in.click_through = conn_->click_through_mask();
    call_in.have_local_player = world.player() != nullptr;
    call_in.x = static_cast<std::int32_t>(x);
    call_in.y = static_cast<std::int32_t>(y);
    game::PickCall call = game::pick_call(call_in);
    if (!call.call_dll) return out;
    game::PickSegment seg = game::screen_ray(pick_camera_, static_cast<float>(call.x), static_cast<float>(call.y));

    // The scene: the spawns' actors (posed as drawn; spawns drawn as boxes are spheres), then the doors and
    // ground objects.
    std::vector<game::PickActor> actors;
    std::vector<PickTarget> refs;
    int exclude = -1;
    std::vector<std::uint32_t> ids;
    ids.reserve(world.spawns.size());
    for (const auto& [id, s] : world.spawns) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    const std::uint8_t option = conn_->click_through_mask();
    const auto see_invisible = viewer_see_invisible_rank();
    for (std::uint32_t id : ids) {
        const game::Spawn& s = world.spawns.at(id);
        game::MaskSpawn ms;
        ms.type = s.is_corpse ? 2 : (s.is_npc ? 1 : 0);
        ms.click_through = s.click_through;
        ms.mercenary = s.mercenary;
        if (s.pet_owner_id != 0)
            if (auto o = world.spawns.find(s.pet_owner_id); o != world.spawns.end())
                ms.owner_is_player = !o->second.is_npc && !o->second.is_corpse;
        game::PickActor a;
        a.mask = game::spawn_pick_mask(ms, option);
        a.invisible = conn_->spawn_invisible(s, see_invisible); // IsInvisible (slot 2 skips it)
        SpawnPickSource source = spawn_pick_source(id, s);
        source.posed = [this, id](game::PickActor& actor, const glm::vec3& origin) {
            render::ActorRenderer::SpawnPose pose;
            if (!actor_renderer_->spawn_pose(id, pose)) return false;
            actor.position = origin;
            actor.bound_center = pose.center;
            actor.bound_radius = pose.radius;
            actor.hierarchical = true;
            actor.bones = std::move(pose.bones);
            auto tris = std::make_shared<std::vector<glm::vec3>>(std::move(pose.triangles));
            actor.triangles = [tris](const game::PickTriangleSink& sink) {
                for (std::size_t i = 0; i + 2 < tris->size(); i += 3) {
                    game::PickTriangle t;
                    t.v0 = (*tris)[i];
                    t.v1 = (*tris)[i + 1];
                    t.v2 = (*tris)[i + 2];
                    t.normal = game::normalize_or_zero(glm::cross(t.v0 - t.v1, t.v2 - t.v1));
                    sink(t);
                }
            };
            return true;
        };
        if (add_world_click_spawn(actors, exclude, std::move(a), source, seg, id == world.player_id && call.exclude_view))
            refs.push_back({PickTarget::Kind::Spawn, id, {}});
    }
    if (world_objects_) {
        std::vector<WorldObjects::PickRef> wrefs;
        world_objects_->pick_actors(actors, wrefs);
        for (const auto& r : wrefs) {
            if (r.is_placed) refs.push_back({PickTarget::Kind::Placed, 0, {}, r.re, r.item});
            else refs.push_back({r.is_door ? PickTarget::Kind::Door : PickTarget::Kind::Object, r.id, {}});
        }
    }
    // The zone's ladders (game/zone_ladders.h): EQObjects, static polygon actors like the ground objects; the pick
    // mask a ground object gets (2) is assumed for them.
    for (std::size_t i = 0; i < zone_ladders_.size(); ++i) {
        const game::ZoneLadder* l = &zone_ladders_[i];
        if (game::point_segment_distance_squared(seg, l->center) > (l->bound + 2.0f) * (l->bound + 2.0f)) continue;
        game::PickActor a;
        a.mask = 2;
        a.position = l->center;
        a.bound_center = l->center;
        a.bound_radius = l->bound;
        a.static_mesh = true;
        a.triangles = [l](const game::PickTriangleSink& sink) {
            for (const auto& r : l->shape.records) {
                game::PickTriangle t;
                t.v0 = glm::vec3(l->transform * glm::vec4(r.v0, 1.0f));
                t.v1 = glm::vec3(l->transform * glm::vec4(r.v1, 1.0f));
                t.v2 = glm::vec3(l->transform * glm::vec4(r.v2, 1.0f));
                t.normal = game::normalize_or_zero(glm::cross(t.v0 - t.v1, t.v2 - t.v1));
                t.flags = r.flags;
                sink(t);
            }
        };
        actors.push_back(std::move(a));
        refs.push_back({PickTarget::Kind::Ladder, static_cast<std::uint32_t>(i), {}});
    }
    game::PickScene scene;
    scene.world = pick_world();
    scene.actors = actors;
    scene.exclude_a = exclude;
    game::PickHit hit = game::pick_actor(seg, scene, required, skip);
    if (hit.actor >= 0) {
        out = refs[static_cast<std::size_t>(hit.actor)];
        out.point = hit.point;
    }
    if (!why) return out; // quiet (the isometric camera's hover, each frame)
    static constexpr const char* kKinds[] = {"nothing", "spawn", "door", "object", "placed object", "ladder"};
    std::string name;
    if (out.kind == PickTarget::Kind::Spawn)
        if (auto it = world.spawns.find(out.id); it != world.spawns.end()) name = " " + it->second.name;
    log::info("pick ({}) at ({:.0f}, {:.0f}): {} {}{} t {:.4f}, world t {:.4f}, {} passes, {} actors tested", why, x, y,
              kKinds[static_cast<int>(out.kind)], out.id, name, hit.t, hit.world_t, hit.passes, actors.size());
    return out;
}

std::optional<glm::vec3> ClientApp::world_point_at(float x, float y) {
    if (!conn_ || !pick_camera_valid_ || !collision_ || !view_area_.contains(x, y)) return std::nullopt;
    game::PickSegment seg = game::screen_ray(pick_camera_, x - view_area_.x, y - view_area_.y);
    game::PickScene scene;
    scene.world = pick_world();
    game::PickHit hit = game::pick_actor(seg, scene, 0, 0);
    if (hit.world_t >= 1.0f) return std::nullopt;
    return seg.start + seg.dir * hit.world_t;
}

std::function<void(const game::PickSegment&, const game::PickTriangleSink&)> ClientApp::pick_world() const {
    if (!collision_) return {};
    const game::CollisionMesh* mesh = &*collision_;
    // The isometric camera: a surface the cut takes away (game/iso_occlusion.h) is not there for a click, which goes
    // on through it to what shows behind (a spawn, a door, the ground).
    const game::IsoReveal reveal = iso_reveal();
    const bool cut = reveal.origin.w > 0.0f || reveal.roof.w > 0.0f;
    return [mesh, reveal, cut](const game::PickSegment& sg, const game::PickTriangleSink& sink) {
        mesh->for_each_on_segment(sg.start, sg.start + sg.dir, [&](const game::CollisionTriangle& c) {
            const game::PickTriangle t{c.v0, c.v1, c.v2, c.normal, c.flags};
            if (cut && game::iso_pick_skips(reveal, sg, t)) return;
            sink(t);
        });
    };
}

void ClientApp::update_placement() {
    if (!conn_ || !world_objects_ || !conn_->placement().active()) return;
    const auto& in = window_.input();
    const game::Placement& pl = conn_->placement();
    glm::vec3 at = game::placed_world_position(pl.ghost());
    // Collision, wall and cursor modes: the ghost on the surface under the mouse. ItemPlacement_UpdatePosition's
    // own placement (CollideWithWorld @0x558030, 0x557630 / 0x557c40: the wall fit, the box resting on the
    // surface) and free mode's mouse-delta moves (ItemPlacement_HandleMouseMove) are not built (docs/unknowns.md).
    if (pl.mode() != static_cast<int>(game::PlaceMode::Free) && !ui_.wants_mouse())
        if (auto p = world_point_at(in.mouse_x, in.mouse_y)) at = *p;
    glm::vec3 lo{-1.0f}, hi{1.0f};
    if (auto b = world_objects_->ghost_bounds()) lo = b->lo, hi = b->hi;
    // 0x505210: a player inside the object's box refuses it (0x1c); here the local player.
    std::uint32_t extra = 0;
    if (!pl.ghost().npc_item) {
        net::RePlacedItem moved = pl.ghost();
        moved.server_pos = at;
        glm::vec3 local = glm::vec3(glm::inverse(game::placed_transform(moved)) * glm::vec4(feet_, 1.0f));
        if (glm::all(glm::greaterThanEqual(local, lo)) && glm::all(glm::lessThanEqual(local, hi))) extra = 0x1c;
    }
    conn_->placement_update(at, lo, hi, extra);
}

void ClientApp::region_teleport(const std::string& text) {
    glm::vec3 pos = feet_ + glm::vec3(0, 0, floor_offset_); // the player's +0x64.. (world x, y, z)
    const auto rows = conn_->zone_point_rows();
    const std::uint32_t zone = conn_->current_zone_dword();
    game::DoTeleportInput in;
    in.index = -1;
    in.point[0] = pos.x;
    in.point[1] = pos.y;
    in.point[2] = pos.z;
    in.waiting_for_port = conn_->waiting_for_port() ? 1 : 0;
    game::DoTeleportHooks h;
    h.resolve = [&](std::int32_t index, bool& has_description) {
        has_description = true;
        float loc[3] = {pos.x, pos.y, pos.z};
        auto t = game::resolve_teleport_index(text, index, static_cast<std::int32_t>(zone), loc, rows);
        // A few zone pairs move the player itself (34 <-> 10, 3 -> 4).
        feet_ += glm::vec3(loc[0], loc[1], loc[2]) - pos;
        pos = glm::vec3(loc[0], loc[1], loc[2]);
        log::info("zone line {}: {} zone {:#x} zone point {} index request {} ({:.1f}, {:.1f}, {:.1f}) heading {:.1f}", text,
                  t.ok ? "to" : "unresolved", t.found, t.zone_point, t.index_request, t.x, t.y, t.z, t.heading);
        return t;
    };
    h.teleport_b = [&](std::int32_t to_zone, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t heading) {
        game::TeleportBArgs a;
        a.zone = to_zone;
        a.x = std::bit_cast<float>(x);
        a.y = std::bit_cast<float>(y);
        a.z = std::bit_cast<float>(z);
        a.heading = std::bit_cast<float>(heading);
        a.reason = 0;
        a.arg8 = -1;
        a.arg9 = 0;
        return teleport_b(a, ("zone line " + text).c_str());
    };
    h.send_index_request = [&](std::int32_t zp) { conn_->send_teleport_index_request(zp); };
    h.position_sample = [&](int kind, const float* point) {
        const glm::vec3 at = point ? glm::vec3(point[0], point[1], point[2]) : pos;
        conn_->record_position_sample(static_cast<std::uint32_t>(kind), &at);
    };
    h.free_description = [] {};
    h.get_spell_affect = [](std::int32_t) { return 0; };
    h.remove_affect = [](std::int32_t, std::int32_t) {};
    game::do_teleport(in, h);
    if (in.waiting_for_port == 0 && conn_->waiting_for_port()) conn_->clear_waiting_for_port();
}

// --pick-test: turns to each of the nearest NPCs and picks at the screen centre (is each hit, or a wall in the
// way?), left clicks the first one hit, then turns to the nearest door in sight and clicks it.
void ClientApp::drive_pick_test(MovementKeys& mk) {
    if (!opts_.pick_test || !conn_ || !player_ready_ || !pick_camera_valid_) return;
    auto now = Clock::now();
    auto turn_to = [&](const glm::vec3& p) {
        glm::vec3 to = p - pick_camera_.position;
        yaw_ = std::atan2(to.y, to.x);
        pitch_ = std::atan2(to.z, glm::length(glm::vec2(to)));
    };
    if (pick_test_step_ == 0) {
        if (now - zone_entered_ < std::chrono::seconds(6)) return;
        // The NPCs within 200, nearest first: each is turned to, then picked at the screen centre.
        std::vector<std::pair<float, std::uint32_t>> near;
        for (const auto& [id, s] : conn_->world().spawns)
            if (s.is_npc && !s.is_corpse && glm::distance(s.position, feet_) < 200.0f)
                near.emplace_back(glm::distance(s.position, feet_), id);
        std::sort(near.begin(), near.end());
        pick_test_ids_.clear();
        for (std::size_t i = 0; i < near.size() && i < 8; ++i) pick_test_ids_.push_back(near[i].second);
        log::info("pick-test: {} NPCs within 200", near.size());
        pick_test_step_ = 10;
        pick_test_at_ = now;
        return;
    }
    if (pick_test_step_ >= 10 && pick_test_step_ < 100) {
        std::size_t k = static_cast<std::size_t>(pick_test_step_ - 10) / 2;
        bool aim = (pick_test_step_ - 10) % 2 == 0;
        if (k >= pick_test_ids_.size()) {
            pick_test_step_ = 1;
            return;
        }
        if (now - pick_test_at_ < std::chrono::milliseconds(aim ? 200 : 700)) return;
        pick_test_at_ = now;
        ++pick_test_step_;
        auto it = conn_->world().spawns.find(pick_test_ids_[k]);
        if (it == conn_->world().spawns.end()) return;
        const game::Spawn& s = it->second;
        glm::vec3 centre = s.display_valid ? s.display_position : s.position;
        if (aim) {
            turn_to(centre);
            return;
        }
        bool clear = !collision_ || collision_->line_of_sight(pick_camera_.position, centre);
        PickTarget hit = pick_at(view_area_.x + pick_camera_.half_width - 0.5f,
                                 view_area_.y + pick_camera_.half_height - 0.5f, game::kClickRequired,
                                 game::kClickSkip, "pick-test");
        if (k == 0 && !opts_.screenshot.empty()) {
            std::string shot = opts_.screenshot;
            auto dot = shot.rfind('.');
            vk_.request_screenshot(dot == std::string::npos ? shot + "-first" : shot.substr(0, dot) + "-first" + shot.substr(dot));
        }
        log::info("pick-test: from ({:.1f} {:.1f} {:.1f}) yon {:.0f}", pick_camera_.position.x, pick_camera_.position.y,
                  pick_camera_.position.z, pick_camera_.yon);
        log::info("pick-test: {} {} {:.0f} away, {}: picked {} {}", s.id, s.name,
                  glm::distance(centre, pick_camera_.position), clear ? "in sight" : "behind a wall",
                  static_cast<int>(hit.kind), hit.id);
        if (!pick_test_clicked_ && hit.kind == PickTarget::Kind::Spawn) {
            pick_test_clicked_ = true;
            left_click_spawn(hit.id);
            log::info("pick-test: left click on {}: target now {}", hit.id, conn_->world().target_id);
            if (!opts_.screenshot.empty()) vk_.request_screenshot(opts_.screenshot);
        }
        return;
    }
    if (pick_test_step_ == 1 && now - pick_test_at_ > std::chrono::seconds(2) && world_objects_) {
        const game::Door* best = nullptr;
        float best_d = 1e9f;
        for (const auto& d : conn_->doors()) {
            if (!game::door_can_be_used(d) || game::door_is_invisible(d) || world_objects_->door_radius(d.id) <= 0.0f) continue;
            glm::vec3 c = world_objects_->door_center(d.id).value_or(d.world_position());
            if (collision_ && !collision_->line_of_sight(pick_camera_.position, c)) continue;
            float dist = glm::distance(feet_, d.world_position());
            if (dist < best_d) best = &d, best_d = dist;
        }
        if (!best) {
            log::info("pick-test: no door");
            pick_test_step_ = 4;
            return;
        }
        turn_to(world_objects_->door_center(best->id).value_or(best->world_position()));
        pick_test_door_ = best->id;
        pick_test_step_ = 2;
        pick_test_at_ = now;
        log::info("pick-test: turning to door {} {} ({:.0f} away)", best->id, best->name, best_d);
        return;
    }
    if (pick_test_step_ == 2) {
        // Walk up to the door (UseSwitch's reach), facing it, then click the screen centre.
        const game::Door* d = conn_->door(pick_test_door_);
        if (!d) {
            pick_test_step_ = 4;
            return;
        }
        glm::vec3 c = world_objects_->door_center(d->id).value_or(d->world_position());
        glm::vec3 me = feet_ + glm::vec3(0, 0, floor_offset_);
        const game::Spawn* self = conn_->world().player();
        float height = self && self->size > 0.0f ? self->size : 6.0f;
        bool near = game::door_within_reach(*d, me, height, world_objects_->door_radius(d->id) - 3.0f);
        turn_to(c);
        if (!near && now - pick_test_at_ < std::chrono::seconds(25)) {
            mk.forward = true;
            return;
        }
        if (now - pick_test_at_ < std::chrono::seconds(1)) return;
        PickTarget hit = pick_at(view_area_.x + pick_camera_.half_width - 0.5f, view_area_.y + pick_camera_.half_height - 0.5f, game::kClickRequired,
                                 game::kClickSkip, "pick-test door");
        bool used = click_world(hit);
        log::info("pick-test: door click at the screen centre: {} {}, used {} (door {} state {})", static_cast<int>(hit.kind),
                  hit.id, used, d->id, d->state);
        pick_test_step_ = 3;
        pick_test_at_ = now;
        return;
    }
    if (pick_test_step_ == 3 && now - pick_test_at_ > std::chrono::seconds(2)) {
        if (const game::Door* d = conn_->door(pick_test_door_)) log::info("pick-test: door {} state now {}", d->id, d->state);
        if (!opts_.screenshot.empty()) {
            std::string shot = opts_.screenshot;
            auto dot = shot.rfind('.');
            vk_.request_screenshot(dot == std::string::npos ? shot + "-door" : shot.substr(0, dot) + "-door" + shot.substr(dot));
        }
        pick_test_step_ = 4;
    }
}

void ClientApp::left_click_spawn(std::uint32_t id) {
    const auto& world = conn_->world();
    auto it = world.spawns.find(id);
    if (it == world.spawns.end()) return;
    const game::Spawn& s = it->second;
    game::LeftClickInput in;
    in.click_lines = conn_->client_option(game::ClientOption::LeftClickTargetHelp); // 0xde0d52
    std::uint32_t old = world.target_id;
    in.has_old_target = old != 0 && world.spawns.contains(old);
    in.old_target_is_clicked = old == id;
    in.spawn.is_local_player = id == world.player_id;
    in.spawn.type = s.is_corpse ? 2 : (s.is_npc ? 1 : 0);
    in.spawn.class_id = s.class_id;
    in.spawn.eb9 = s.gender;
    in.spawn.race = s.race;
    in.spawn.targetable = game::is_targetable(s);
    // Spawn virtual +0x58: PlayerClient_IsSmallBoatRace (race flag 0x01 without 0x02).
    in.spawn.virtual_58 = game::is_small_boat_race(s.race, s.gender);
    // PcClient_IsGuildmasterForMyClass: the spawn's class minus 19 is the player's.
    const game::Spawn* me = world.player();
    in.spawn.guildmaster_for_my_class = me && static_cast<int>(s.class_id) - 19 == static_cast<int>(me->class_id);
    in.cursor_holds_something = conn_->inventory().cursor() != nullptr;
    game::LeftClickResult r = game::left_clicked_on_player(in, game::guildmaster_string_id(s.class_id));
    if (!r.handled) return;
    if (r.save_old_target) target_manager_.previous = old; // CTargetManager +0x168
    if (r.target_is_clicked) conn_->set_target(id);
    if (r.show_tip && ui_api_) ui_api_->activate_tip(0x22); // the interactive object's context tip (class 62)
    if (r.lines) {
        const auto& strings = conn_->strings();
        std::vector<std::string> kind{strings.format(r.kind, {})};
        std::string first = r.named ? strings.format(0x1a7c, std::vector<std::string>{kind[0], game::display_name(s.name)})
                                    : strings.format(0x1a87, kind);
        conn_->ui_message(first);
        conn_->ui_message(strings.format(r.what, kind));
    }
    if (r.drop) conn_->give_to(id); // CEverQuest::DropItemOrMoneyOnPlayer
}

// The USE key (__ExecuteCmd 16, default U): what is under the mouse cursor,
// as a left click there; without anything there, the nearest door in front
// (a convenience of this client: the original only uses the cursor). The gamepad's interact looks at the middle of
// the screen instead of the cursor (`at_cursor` false; a client extension).
void ClientApp::use_key(bool at_cursor) {
    if (!conn_ || !world_objects_) return;
    const auto& in = window_.input();
    if (at_cursor) {
        if (!ui_.wants_mouse() && click_world(pick_at(in.mouse_x, in.mouse_y, game::kClickRequired, game::kClickSkip, "use key")))
            return;
    } else {
        if (click_world(pick_at(view_area_.x + view_area_.w * 0.5f, view_area_.y + view_area_.h * 0.5f,
                                game::kClickRequired, game::kClickSkip, "pad use")))
            return;
    }
    if (auto id = world_objects_->nearest_door(feet_, yaw_, 40.0f)) use_door(*id);
}

// The open tradeskill container (OP_ClickObjectAction): its ten slots, Combine and Close.
void ClientApp::draw_world_container() {
    if (!conn_) return;
    if (drop_confirm_ && !(ui_api_ && ui_api_->replaces("ConfirmationDialogBox"))) {
        const net::Item* item = conn_->inventory().cursor();
        if (!item) {
            drop_confirm_ = false;
        } else {
            ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (ImGui::Begin("Drop item###drop_confirm", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
                ImGui::TextUnformatted(conn_->drop_confirm_text().c_str()); // 9057
                if (ImGui::Button("Yes")) {
                    conn_->drop_cursor_on_ground();
                    drop_confirm_ = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("No")) drop_confirm_ = false;
            }
            ImGui::End();
        }
    }
    // The fallback when no UI pack draws it: its own window (game::WorldContainer::window), or with no pack's
    // tradeskill window to show it in, any open container.
    const auto& wc = conn_->world_container();
    const bool lua_tradeskill = ui_api_ && ui_api_->replaces("TradeskillWnd");
    if (!wc || (ui_api_ && ui_api_->replaces("WorldContainer")) || (!wc->window && lua_tradeskill)) return;
    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_FirstUseEver);
    bool open = true;
    std::string title = (wc->name.empty() ? std::string("Container") : wc->name) + "###world_container";
    if (ImGui::Begin(title.c_str(), &open)) {
        const auto& inv = conn_->inventory();
        for (std::int16_t i = 0; i < 10; ++i) {
            net::ItemSlot slot{net::kInvWorld, i, -1, -1, 0, 0};
            const net::Item* item = inv.at(slot);
            std::string label = std::to_string(i + 1) + ": " + (item ? item->name : std::string("(empty)")) + "##ws" +
                                std::to_string(i);
            if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) conn_->click_slot(slot);
        }
        if (ImGui::Button("Combine")) conn_->combine_world_container();
        ImGui::SameLine();
        if (ImGui::Button("Done")) open = false;
    }
    ImGui::End();
    if (!open) conn_->close_world_container();
}

bool ClientApp::teleport_b(const game::TeleportBArgs& a, const char* what) {
    const glm::vec3 pos = local_body_movement_
        ? local_body_movement_->controller().body.feet + glm::vec3(0, 0, local_body_height_)
        : feet_ + glm::vec3(0, 0, floor_offset_); // the player's +0x64.. (world x, y, z)
    game::TeleportBPlayer p;
    p.x = pos.x;
    p.y = pos.y;
    p.z = pos.z;
    p.heading = game::yaw_to_heading(local_body_movement_ ? local_body_yaw_ : yaw_);
    p.stand_state = conn_->player_status().stand_state;
    p.zone = static_cast<std::int32_t>(conn_->current_zone_dword());
    if (const game::Spawn* me = conn_->world().player()) p.race = me->race;
    bool local = false;
    game::TeleportBHooks b;
    b.send_client_update = [&] { conn_->send_position_now(); };
    // Sample 2 goes with the OP_ClientUpdate send_client_update makes (GameConnection::send_position records it).
    b.position_sample = [&](int kind) {
        if (kind != game::kPositionUpdate) conn_->record_position_sample(static_cast<std::uint32_t>(kind));
    };
    b.save_physics = [] {};
    b.floor_height = [&](float fx, float fy, float fz, int) -> double {
        local = true;
        auto f = collision_ ? collision_->floor_below({fx, fy, fz}, 0.0f) : std::nullopt;
        return f ? static_cast<double>(*f) : static_cast<double>(game::kNoFloor);
    };
    b.update_actor = [] {};
    b.find_person_request = [&] { conn_->find_person_request(); };
    b.rewind_reset = [] {};
    b.move_to_zone = [&](const game::MoveToZoneCall& m) { conn_->move_to_zone(m); };
    bool ok = game::do_teleport_b(p, a, 5, b);
    if (ok && local) {
        // The same zone: the player is moved here; nothing is sent.
        if (local_body_movement_) {
            local_body_movement_->place(glm::vec3(p.x, p.y, p.z - local_body_height_));
            local_body_yaw_ = game::heading_to_yaw(p.heading);
            conn_->set_player_motion({p.x, p.y, p.z}, p.heading, glm::vec3(0), false);
        } else {
            feet_ = glm::vec3(p.x, p.y, p.z - floor_offset_);
            yaw_ = game::heading_to_yaw(p.heading);
            movement_.place(feet_); // the body moves with it (else the next movement step puts the player back)
        }
        log::info("{}: moved within the zone to ({:.1f}, {:.1f}, {:.1f})", what, p.x, p.y, p.z);
    }
    return ok;
}

} // namespace hailstone::app::client_internal
