// ClientApp: The player each frame: input, movement, camera.

#include "app/client_app_internal.h"

#include "game/boats.h"
#include "game/passage_of_time.h"

namespace hailstone::app::client_internal {

void ClientApp::update_mouse_look() {
    const auto& in = window_.input();
    const auto now = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_time_).count());
    // CDisplay::cameraType: this client's camera behind the player stands for the wheel-follow camera (6), its
    // first person for camera 0.
    const bool camera6 = camera_type_ == game::kCameraWheelFollow;
    const game::MouseLookOptions options{conn_->client_option(game::ClientOption::MouseLook),
                                         conn_->client_option(game::ClientOption::MouseTurnZoom)};
    left_click_.reset();
    right_click_ = false;
    // __ProcessMouseEvents with the interface locked (/safelock, game/option_commands.h): nothing reaches the world
    // or the windows (ui::ImGuiLayer::set_mouse_locked) and the buttons are not read; while the left or right one was
    // down when they last were, 12095 every frame. A frozen player (StandState 0x66) returns before that check.
    // Behind the loading screen (zone_settle_) the same, without the message.
    if (zone_settling()) {
        mouse_look_.cancel();
        left_click_pending_.reset();
        window_.set_mouse_captured(false);
        return;
    }
    if (conn_->state() == net::ConnectionState::InZone && conn_->interface_locked()) {
        mouse_look_.cancel();
        left_click_pending_.reset();
        window_.set_mouse_captured(false);
        if ((mouse_left_held_ || mouse_right_held_) && conn_->player_status().stand_state != net::kStandFreeze)
            conn_->system_message(game::kStrInterfaceLocked, game::kLockedColor);
        return;
    }
    // The presses and releases as ProcessMouseInput swaps them (platform::Window::set_swap_mouse_buttons).
    const game::ButtonEdges left = game::button_edges(in.left_clicked, in.left_button, mouse_left_held_);
    const game::ButtonEdges right = game::button_edges(in.right_clicked, in.right_button, mouse_right_held_);
    const bool left_pressed = left.pressed, left_released = left.released;
    const bool right_pressed = right.pressed, right_released = right.released;
    mouse_left_held_ = in.left_button;
    mouse_right_held_ = in.right_button;
    // __ProcessMouseEvents: dead or knocked out, mouse look ends and only the windows get the mouse.
    const bool down = conn_->state() == net::ConnectionState::InZone && conn_->player_down();
    if (down) {
        mouse_look_.cancel();
        left_click_pending_.reset();
    } else {
        // A press goes to the world when the windows do not take it (or during mouse look); a release during mouse
        // look always, else when the windows do not take it.
        if (right_pressed && (mouse_look_.active || !ui_.wants_mouse())) {
            right_down_pos_ = {in.mouse_x, in.mouse_y};
            // CEverQuest::RMouseDown in the camera 6: the player takes the camera's heading.
            if (camera6) {
                yaw_ += chase_heading_;
                chase_heading_ = 0.0f;
            }
            mouse_look_.right_down(options, now);
        }
        if (right_released && (mouse_look_.active || !ui_.wants_mouse())) {
            game::MouseLook::RightUp up = mouse_look_.right_up(now);
            // (up.center_cursor, CenterMouseCursor, sets only the game's copy of the cursor for the rest of the
            // frame: the cursor itself comes back where the look took it, platform/mouse_capture.h.)
            right_click_ = up.click;
        }
        if (left_pressed && iso() && !ui_.wants_mouse()) {
            // The isometric camera (Hailstone's own): the left button only clicks; it never starts a mouse look.
            if (!mouse_look_.active) left_click_ = glm::vec2(in.mouse_x, in.mouse_y);
        } else if (left_pressed && (mouse_look_.active || !ui_.wants_mouse())) {
            mouse_look_.left_down(options, camera6, now);
            // This client clicks at the press; during MouseTurnZoom's look the click waits for the release, as
            // CEverQuest::LMouseUp makes every click (held under 0x2ef ms, mouse look off by then).
            if (mouse_look_.active && !mouse_look_.by_right) left_click_pending_ = glm::vec2(in.mouse_x, in.mouse_y);
            else if (!mouse_look_.active) left_click_ = glm::vec2(in.mouse_x, in.mouse_y);
        }
        if (left_released && (mouse_look_.active || left_click_pending_ || !ui_.wants_mouse())) {
            bool click = mouse_look_.left_up(now);
            if (left_click_pending_ && click) left_click_ = left_click_pending_;
            left_click_pending_.reset();
        }
    }
    window_.set_mouse_captured(mouse_look_.active);
    // EQCamera_WheelFollow::UpdatePosition: after a right-button look the camera stays behind the player.
    if (mouse_look_.by_right) chase_heading_ = 0.0f;
    // ProcessControls calls HandleMouseLook @0x516d40 during mouse look in the camera 6, or standing or ducking in
    // first person. The mouse's sideways motion turns the player only when it can move (the move flag), or the
    // camera alone in the camera 6 during a left-button look; its up and down motion tilts the view.
    if (!mouse_look_.active) return;
    if (iso()) {
        // The isometric camera: the right button's look turns the camera about the player, never the player.
        game::iso_turn(iso_camera_, -in.mouse_dx * mouse_look_scale());
        last_mouse_look_ = Clock::now();
        return;
    }
    const std::uint32_t stand = conn_->player_status().stand_state;
    const bool can_move = (stand == net::kStandStanding || stand == net::kStandCrouching) && !conn_->stunned();
    if (camera_type_ == game::kCameraFirstPerson && !can_move) return;
    const float k = mouse_look_scale();
    if (camera_type_ != game::kCameraFirstPerson && !camera6) {
        // HandleMouseLook in the cameras 1..5 (game::camera_mouse_look): the overhead and chase cameras' sideways
        // motion turns the player (with the move flag), the user and tether cameras' the camera. The motion is this
        // client's mouse scaling in heading units (the same turn as the player's); the height and distance take it
        // as the original's raw motion, clamped by the sensitivity.
        constexpr float kUnits = 256.0f / glm::pi<float>();
        const float dy = mouse_invert_y_ ? -in.mouse_dy : in.mouse_dy;
        const float rate = std::min(std::chrono::duration<float>(Clock::now() - last_mouse_look_).count(), 0.1f) * 20.0f;
        last_mouse_look_ = Clock::now();
        if (game::camera_mouse_look(camera_type_, cameras_[camera_type_], in.mouse_dx * k * kUnits, dy * k * kUnits, dy,
                                    static_cast<float>(mouse_sensitivity_), rate, motion_body_wet_ == 5) &&
            can_move)
            yaw_ -= in.mouse_dx * k;
        return;
    }
    last_mouse_look_ = Clock::now();
    if (mouse_look_.turns_camera(camera6)) chase_heading_ -= in.mouse_dx * k;
    else if (can_move) yaw_ -= in.mouse_dx * k;
    pitch_ = std::clamp(pitch_ - (mouse_invert_y_ ? -in.mouse_dy : in.mouse_dy) * k, -1.5f, 1.5f);
}

// Riding (game/mounts.h): the link asks the models and the room; a change of the controlled spawn moves the
// controls to the mount or back (the body keeps the spawn z: update_player's AvatarHeight follow-up).
bool ClientApp::MountAnswers::new_models_loaded() { return true; }

bool ClientApp::MountAnswers::mount_point(const game::Spawn& rider, const game::Spawn& mount) {
    if (!app->actor_renderer_) return true;
    auto r = app->actor_renderer_->choose_model(rider).model;
    auto m = app->actor_renderer_->choose_model(mount).model;
    return r && m && m->mount_point_bone(r->code) >= 0;
}

// do_stand's own CanIFitHere(+0x64, 0) at the player's position, then the room between +0x11b4 and +0x1070.
bool ClientApp::MountAnswers::can_i_fit_here() {
    ZoneMoveWorld world(app->collision_ ? &*app->collision_ : nullptr, app->world_objects_.get(),
                        app->zone_ ? &app->zone_->regions : nullptr);
    const game::Spawn* me = app->conn_->world().player();
    auto now_ms = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - app->start_time_).count() + 1);
    PlayerMovement::RoomCheck r = app->movement_.can_i_fit_here_now(app->collision_ ? &world : nullptr,
                                                                     app->floor_offset_,
                                                                     me ? me->heights.length : 0.0f, now_ms);
    room = r.room;
    return r.fits;
}

double ClientApp::MountAnswers::room_above() { return room; }

bool ClientApp::MountAnswers::ceiling_and_floor(float& ceiling, float& floor) {
    auto c = app->movement_.player_ceiling();
    auto f = app->movement_.floor_height();
    if (!c || !f) return false;
    ceiling = *c;
    floor = *f;
    return true;
}

bool ClientApp::touching_switch(std::uint8_t id) {
    const game::Door* d = conn_ ? conn_->door(id) : nullptr;
    if (!d) return true;
    namespace mech = game::mechanics;
    mech::TouchedSwitch sw;
    sw.id = d->id;
    sw.type = d->type;
    sw.state = d->state;
    sw.param = d->param;
    sw.adventure_id = d->adventure_id;
    sw.dyn_id = d->dyn_door_id;
    sw.has_script = d->has_script;
    sw.repop_ms = game::repop_frequency(*d);
    sw.damage = static_cast<std::int32_t>(game::switch_damage(d->type));
    mech::TouchSpawn me;
    me.spawn_id = static_cast<std::uint16_t>(conn_->world().player_id);
    me.last_touch_ms = touch_time_;
    if (const net::Item* held = conn_->inventory().cursor()) me.cursor_item_id = static_cast<std::int32_t>(held->id);
    mech::TouchHooks h;
    h.random_percent = [this] { return 1 + static_cast<std::int32_t>(touch_rng_() % 100); };
    h.zone_point_zone = [this](std::int32_t param) {
        for (const auto& row : conn_->zone_point_rows())
            if (row.number == param) return row.zone;
        return -1;
    };
    // EQWorldData::GetGeometryNameFromIndex: indices above 1000 lose their instance bits; 1..1000 have a name.
    auto zone_file = [this](std::int32_t zone, const char* ext) {
        std::int32_t index = zone > 1000 ? (zone & 0x7fff) : zone;
        if (index <= 0 || index > 1000) return false;
        std::string name = game::zone_short_name(static_cast<std::uint32_t>(index));
        return !name.empty() && game_.find(name + ext).has_value();
    };
    h.zone_s3d_exists = [zone_file](std::int32_t z) { return zone_file(z, ".s3d"); };
    h.zone_eqg_exists = [zone_file](std::int32_t z) { return zone_file(z, ".eqg"); };
    auto now = static_cast<std::uint32_t>(net::GameConnection::now_ms());
    mech::TouchResult r = mech::touching_switch(sw, me, now, last_damage_switch_, h);
    touch_time_ = me.last_touch_ms;
    if (r.damage) {
        log::info("touching switch {} ({}, type {}): {} trap damage", d->id, d->name, d->type, *r.damage);
        conn_->report_env_damage(static_cast<std::uint32_t>(*r.damage), mech::kTrapDamageType);
    }
    if (r.use) {
        log::info("touching switch {} ({}, type {}): used by {} with key {}", d->id, d->name, d->type, r.use->user,
                  r.use->key);
        conn_->use_switch(id, r.use->user, world_objects_ ? world_objects_->door_radius(id) : 0.0f, r.use->key);
    }
    if (r.close_now) conn_->set_door_state_now(id, game::kSwitchClosed);
    return r.clear;
}

// The vehicle spawns near the player (PlayerClient_IsVehicleRace: ships, boats, launches) where they are drawn this
// frame, for the floor test (CollsnCallbackForFloor's Vehicle) and the walls: their models' triangles in the vehicle's
// frame, taken from the drawn model, placed at the drawn pose (game/boats.h), which WorldState::smooth_vehicles has
// already moved this frame. They are taken again a few times a second, so a model's own animation (whatever its
// clip moves) is in its collision; between two takes the pose stands still.
void ClientApp::update_vehicles() {
    // Each fixed step asks again; a hull whose take and pose have not changed is the last step's, and the others
    // reuse its triangle storage, so a boat at the dock costs no allocation and no transform per step.
    vehicles_spare_.swap(vehicles_);
    vehicles_.clear();
    auto reuse = [&](std::uint32_t id) -> std::optional<ZoneMoveWorld::Vehicle> {
        for (auto& v : vehicles_spare_)
            if (v.id == id) {
                v.id = 0;
                return std::move(v);
            }
        return std::nullopt;
    };
    if (!actor_renderer_ || !conn_) return;
    constexpr float kVehicleReach = 400.0f;
    constexpr auto kVehicleRefresh = std::chrono::milliseconds(200);
    const auto now = Clock::now();
    const auto& spawns = conn_->world().spawns;
    std::erase_if(vehicle_models_, [&](const auto& kv) { return !spawns.contains(kv.first); });
    for (const auto& [id, s] : spawns) {
        if (id == conn_->world().player_id || !game::is_vehicle_race(s.race, s.gender)) continue;
        const game::BoatPose pose = game::drawn_pose(s);
        if (glm::distance(pose.position, feet_) > kVehicleReach) continue;
        VehicleModel& m = vehicle_models_[id];
        const void* drawn = actor_renderer_->drawn_model(id);
        if (drawn && (drawn != m.model || now - m.taken >= kVehicleRefresh)) {
            VehicleModel fresh;
            if (actor_renderer_->spawn_local_triangles(id, fresh.triangles) && !fresh.triangles.empty()) {
                fresh.model = drawn;
                fresh.taken = now;
                glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
                for (const auto& p : fresh.triangles) {
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                }
                fresh.center = (lo + hi) * 0.5f;
                for (const auto& p : fresh.triangles) fresh.radius = std::max(fresh.radius, glm::length(p - fresh.center));
                fresh.probes = game::hull_probes(fresh.triangles);
                if (drawn != m.model)
                    log::info("vehicle: {} ({}) has {} triangles, {:.1f} across", game::display_name(s.name), id,
                              fresh.triangles.size() / 3, fresh.radius * 2.0f);
                m = std::move(fresh);
            }
        }
        if (m.triangles.empty()) continue;
        auto last = reuse(id);
        ZoneMoveWorld::Vehicle v = last ? std::move(*last) : ZoneMoveWorld::Vehicle{};
        v.id = id;
        if (!last || m.placed_taken != m.taken || m.placed_pose.position != pose.position ||
            m.placed_pose.heading != pose.heading) {
            v.center = game::boat_to_world(pose, m.center);
            v.radius = m.radius;
            v.triangles.clear();
            v.triangles.reserve(m.triangles.size() / 3);
            for (std::size_t i = 0; i + 2 < m.triangles.size(); i += 3)
                v.triangles.push_back(game::CollisionTriangle::make(game::boat_to_world(pose, m.triangles[i]),
                                                                    game::boat_to_world(pose, m.triangles[i + 1]),
                                                                    game::boat_to_world(pose, m.triangles[i + 2]), 0));
            m.placed_taken = m.taken;
            m.placed_pose = pose;
        }
        vehicles_.push_back(std::move(v));
    }
}

void ClientApp::update_npc_bodies() {
    npc_bodies_.clear();
    if (!conn_) return;
    const auto& rules = conn_->server_rules();
    if (rules && rules->configured && !rules->npc_collision) return;
    // Farther than this an NPC cannot meet the player's sweep in one frame.
    constexpr float kNpcReach = 100.0f;
    const std::uint32_t controlled = conn_->controlled_id();
    for (const auto& [id, s] : conn_->world().spawns) {
        if (!game::npc_blocks_player(s, controlled)) continue;
        // An NPC that is not drawn (invisible to the player, an unseen marker, past the clip plane) is no wall.
        if (actor_renderer_ && !actor_renderer_->drawn_model(id)) continue;
        const glm::vec3 at = s.display_valid ? s.display_position : s.position;
        if (glm::distance(at, feet_) > kNpcReach) continue;
        npc_bodies_.push_back(game::npc_body(s, at));
    }
}

void ClientApp::update_riding() {
    mount_answers_.app = this;
    conn_->set_mount_queries(&mount_answers_);
    std::uint32_t cid = conn_->eye_id() ? 0 : conn_->controlled_id();
    if (cid && cid == riding_id_ && Clock::now() - riding_logged_ > std::chrono::seconds(1)) {
        riding_logged_ = Clock::now();
        log::debug("riding: feet ({:.1f}, {:.1f}, {:.1f}) speed {:.3f}", feet_.x, feet_.y, feet_.z, last_speed_run_);
    }
    if (cid == riding_id_) return;
    if (cid) {
        // do_stand: the mount took the player's physics block and spawn z; the player's speeds are 0.
        log::info("riding: mounted spawn {} (feet {:.2f} {:.2f} {:.2f})", cid, feet_.x, feet_.y, feet_.z);
    } else {
        // The dismount: the rider took the mount's position (AttachRiderToMount on its own mount).
        log::info("riding: dismounted spawn {}", riding_id_);
    }
    riding_id_ = cid;
}

void ClientApp::update_player(float dt) {
    using platform::Key;
    // SendTeleportIndexRequestAndWait @0x506560: the original sleeps in a loop that only delivers the network's
    // packets until the server's 0xcadf ends the wait (+0x1f05), so nothing else of the game runs. Here the window
    // keeps drawing and the packets are handled, but the player neither acts nor moves until then.
    if (conn_ && conn_->waiting_for_port()) return;
    if (conn_ && driven_eye_ != conn_->eye_id()) {
        // An Eye takes only the controls and view. Keep the local physics body alive at its own position, with
        // gravity, air and environment damage, then return to that body on a fade, death or depop.
        if (local_body_movement_) {
            movement_ = std::move(*local_body_movement_);
            local_body_movement_.reset();
            feet_ = movement_.controller().body.feet;
            floor_offset_ = local_body_height_;
            yaw_ = local_body_yaw_;
            pitch_ = local_body_pitch_;
        }
        driven_eye_ = conn_->eye_id();
        if (auto eye = conn_->world().spawns.find(driven_eye_); driven_eye_ && eye != conn_->world().spawns.end()) {
            local_body_height_ = floor_offset_;
            local_body_yaw_ = yaw_;
            local_body_pitch_ = pitch_;
            local_body_movement_.emplace(std::move(movement_));
            movement_ = PlayerMovement{};
            floor_offset_ = eye->second.heights.avatar_height;
            feet_ = eye->second.position - glm::vec3(0, 0, floor_offset_);
            yaw_ = game::heading_to_yaw(eye->second.heading);
            pitch_ = 0.0f;
            movement_.reset(feet_, yaw_);
            movement_.controls_state().local.pitch = local_body_pitch_ * 256.0f / glm::pi<float>();
            movement_.push(eye->second.velocity);
        }
        step_move_ = glm::vec3(0.0f);
        step_turn_ = step_pitch_ = 0.0f;
    }
    if (conn_) {
        if (auto respawn = conn_->take_respawn_placement()) {
            // Same-zone hover recovery publishes a new local spawn without enter_zone(). Its physics and
            // interpolation must start at that spawn too, or the next step sends the corpse's old position back.
            if (const game::Spawn* me = conn_->world().player()) {
                floor_offset_ = me->heights.avatar_height;
                feet_ = respawn->position - glm::vec3(0, 0, floor_offset_);
                yaw_ = game::heading_to_yaw(respawn->heading);
                pitch_ = chase_heading_ = 0.0f;
                mouse_look_.cancel();
                left_click_pending_.reset();
                window_.set_mouse_captured(false);
                movement_.reset(feet_, yaw_);
                step_move_ = last_velocity_ = glm::vec3(0.0f);
                step_turn_ = step_pitch_ = last_speed_run_ = 0.0f;
                motion_ = {};
                if (actor_renderer_)
                    actor_renderer_->set_player_motion({true, glm::vec3(0.0f), 0.0f, 0, 0, 0.0f, false,
                                                        camera_type_ == game::kCameraFirstPerson, 0});
                motion_body_wet_ = motion_head_wet_ = head_wet_ = 0;
                listener_ceiling_.reset();
                boat_hold_.reset();
                riding_id_ = 0;
                conn_->set_standing_on(0);
                conn_->set_touching_switch(std::nullopt);
                if (nav_) nav_->stop("");
                // A stopped click walk must not run its pending interaction at the respawn destination.
                iso_pending_.reset();
                iso_walking_ = iso_hold_ = false;
                iso_marker_ = {};
                if (conn_->follow_target()) conn_->stop_follow(false);
                follow_move_ = follow_run_ = follow_detour_ = follow_assist_strafed_ = false;
                // Prime the new spawn's region state without treating the placement itself as walking into a line.
                zone_line_.reset();
                if (zone_) (void)zone_line_.update(feet_, zone_->regions);
                if (audio_ && music_dead_) audio_->music_selection(audio::kMusicDeath, false);
                music_dead_ = false;
                if (audio_ && special_room_) audio_->set_special_room(std::nullopt);
                special_room_ = stun_room_ = false;
            }
        }
    }
    // KnockedOut saves the current camera and selects overhead once. GetConscious restores it; a new zone's
    // active status also clears the saved unconscious view when the connection's status is reset.
    if (conn_ && conn_->consciousness_changes() != consciousness_changes_) {
        consciousness_changes_ = conn_->consciousness_changes();
        if (conn_->player_status().owner_status == 3) {
            if (!unconscious_camera_) unconscious_camera_ = camera_type_;
            set_camera_type(game::kCameraOverhead);
        }
    }
    if (conn_ && conn_->player_status().owner_status == 0 && unconscious_camera_) {
        game::CameraSelection s{camera_type_, camera_distance_, pad_camera_distance_};
        game::restore_camera(s, *unconscious_camera_, max_camera_distance_);
        apply_camera_selection(s);
        unconscious_camera_.reset();
    }
    if (conn_ && conn_->drunkenness_updates() != drunkenness_updates_) {
        drunkenness_updates_ = conn_->drunkenness_updates();
        drunk_fov_offset_ = game::drunkenness_fov_offset(conn_->drunkenness());
        if (conn_->drunkenness() >= 70) set_camera_type(game::kCameraFirstPerson);
    }
    // The widening follows the ticks, but a value below 70 (a sudden reset too) drops it at once.
    if (!conn_ || game::drunkenness_fov_offset(conn_->drunkenness()) == 0.0f) drunk_fov_offset_ = 0.0f;
    camera_.fov_offset_units = drunk_fov_offset_;
    // Behind the loading screen (zone_settle_) the keys and the pad do not reach the world.
    bool keys = !ui_.wants_keyboard() && !zone_settling();
    const auto& in = window_.input();
    update_mouse_look();

    // The original's movement commands (default keys: CMD_FORWARD W, CMD_BACK
    // S, Ctrl+R CMD_RUN_WALK; here A/D strafe and Q/E turn, see README).
    MovementKeys mk;
    if (opts_.auto_walk_seconds > 0.0f) {
        opts_.auto_walk_seconds -= dt;
        mk.forward = true;
    }
    drive_auto_door(mk);
    drive_auto_board();
    drive_pick_test(mk);
    // The key map's commands (KeypressHandler::HandleKeyDown / HandleKeyUp): movement is held while its key is
    // down; the rest acts once (key_command).
    key_events_ = actions_ui_->process_keys(in, keys);
    const std::size_t player_keys = key_events_.size(); // the player's own, before the scripts' and the gamepad's
    // /keypress from scripts and plugins: this frame's presses, then a tap's release on the next.
    key_events_.insert(key_events_.end(), script_keys_.begin(), script_keys_.end());
    script_keys_ = std::exchange(script_key_release_, {});
    // The gamepad's key commands this frame (app/client_app_gamepad.cpp): the same paths as the keys'.
    const std::size_t pad_first = key_events_.size();
    key_events_.insert(key_events_.end(), pad_events_.begin(), pad_events_.end());
    pad_events_.clear();
    if (zone_settling()) key_events_.clear();
    // __ExecuteCmd's movement commands (AUTORUN, JUMP, FORWARD, BACK, RIGHT, LEFT, STRAFE_LEFT/RIGHT, the look,
    // zoom and camera keys, CENTERVIEW) on each press and release (game/mechanics/controls.h): they hold the keys,
    // autorun and the ramps. The move and turn keys and CENTERVIEW are ignored while the player is frozen
    // (StandState 102) or the interface locked (/safelock); the look, zoom and camera keys are not.
    bool accept = conn_->player_status().stand_state != net::kStandFreeze && !conn_->interface_locked();
    for (const auto& e : key_events_)
        if (game::mechanics::is_movement_command(e.command)) movement_command(e.command, e.down, accept);
    mk.move_down = actions_ui_->keymap().held(game::KeyCommand::CMD_MOVE_DOWN);
    // CastSpell's line of sight test (PlayerBase::CanSee) against the zone's collision.
    // Each new connection is given its hooks once. (Not a test on the connection's address: a new connection can be
    // allocated where the freed one was, after /camp or a return to character select.)
    if (!conn_->has_line_of_sight()) {
        conn_->set_line_of_sight([this](const glm::vec3& a, const glm::vec3& b) {
            return !collision_ || collision_->line_of_sight(a, b);
        });
        // OP_RequestClientZoneChange: DoTeleportB on the player (a #goto in this zone moves without zoning).
        conn_->set_teleport_handler([this](const game::TeleportBArgs& a) { teleport_b(a, "zone change request"); });
        // The slash commands whose original works on the Actions window's state (net::GameConnection::
        // set_app_commands).
        conn_->set_app_commands([this](std::string_view handler, const std::string& args) {
            if (handler == "Cmd_stopsocial") { // Cmd_stopsocial @0x4e8a80: Social_Stop(-1, 0)
                if (actions_ui_) actions_ui_->stop_social();
                return true;
            }
            if (handler == "Cmd_bodytint") { // Cmd_bodytint @0x4ebca0: toggles CBodyTintWnd
                if (eq_api_) eq_api_->toggle("BodyTintWnd");
                return true;
            }
            if (handler == "Cmd_facepick") { // Cmd_facepick @0x4ebc80: __ExecuteCmd(0x1ec, 1, 0)
                player_customization_key();
                return true;
            }
            if (handler != "Cmd_doability" || !actions_ui_) return false;
            actions_ui_->doability_command(*conn_, stats_ui_ ? stats_ui_->character() : nullptr, ability_view(), args);
            return true;
        });
    }
    // The player's own move, turn or strafe keys this frame (not a script's /keypress): they end /nav's walk, and
    // they alone end /follow when its mesh help strafed (below).
    bool player_moved = false;
    for (std::size_t i = 0; i < key_events_.size(); ++i) {
        const auto& e = key_events_[i];
        if ((i < player_keys || i >= pad_first) && e.down && e.command >= game::mechanics::kCmdForward &&
            e.command <= game::mechanics::kCmdStrafeRight)
            player_moved = true;
    }
    // The keys and clicks act once a drawn frame: the key map's own commands, placing, the item clicks, the test
    // drivers. (Before the movement, as they ran before; ahead of /follow and /nav now, since those run per step.)
    if (keys) {
        update_combat_input();
    } else {
        // A window has the keyboard (typing in the chat): the scripts' /keypress and the pad are not the keyboard,
        // their commands still act (a script's press would be lost otherwise: script_keys_ is not kept).
        for (std::size_t i = player_keys; i < key_events_.size(); ++i) key_command(key_events_[i]);
    }
    update_placement();
    update_item_input();
    update_iso_clicks(player_moved);
    update_iso_hover();
    // --auto-keys: one key command pressed and released every 2 s, 6 s after zoning in.
    if (!opts_.auto_keys.empty() && std::chrono::duration<float>(Clock::now() - zone_entered_).count() > 6.0f &&
        Clock::now() - auto_key_time_ > std::chrono::seconds(2)) {
        auto_key_time_ = Clock::now();
        std::size_t comma = opts_.auto_keys.find(',');
        std::string name = opts_.auto_keys.substr(0, comma);
        opts_.auto_keys = comma == std::string::npos ? std::string() : opts_.auto_keys.substr(comma + 1);
        // "UI:<screen>:<child>" clicks a stock window's child, "LIST:<screen>:<child>:<row>[:<column>]" a list row
        // (the Lua windows' clicks, as a mouse click there makes them); MENU / MENUROW a context menu.
        auto parts = [](const std::string& v) {
            std::vector<std::string> out;
            for (std::size_t at = 0;;) {
                std::size_t c = v.find(':', at);
                out.push_back(v.substr(at, c == std::string::npos ? std::string::npos : c - at));
                if (c == std::string::npos) break;
                at = c + 1;
            }
            return out;
        };
        if (auto p = parts(name); eq_api_ && p.size() >= 4 && p[0] == "MENU") {
            // "MENU:<screen>:<x>:<y>" right-clicks a stock window there (its context menu)
            bool ok = eq_api_->context_menu(p[1], static_cast<float>(std::atof(p[2].c_str())),
                                            static_cast<float>(std::atof(p[3].c_str())));
            log::info("auto key {}: {}", name, ok ? "menu open" : "no menu");
        } else if (p.size() >= 2 && eq_api_ && p[0] == "INSPECT") {
            // "INSPECT:<slot>[:<bag index>]" right-clicks a possessions slot (an item's display, a bag's window)
            eq_api_->right_click_item(net::ItemSlot::possessions(static_cast<std::int16_t>(std::atoi(p[1].c_str())),
                                                                 static_cast<std::int16_t>(p.size() >= 3 ? std::atoi(p[2].c_str()) : -1)));
            log::info("auto key {}: inspected", name);
        } else if (p.size() >= 2 && conn_ && p[0] == "PREVIEW") {
            // "PREVIEW:<slot>[:<bag index>]" Alt + right-clicks a possessions slot (the item preview)
            conn_->right_click_item(net::ItemSlot::possessions(static_cast<std::int16_t>(std::atoi(p[1].c_str())),
                                                               static_cast<std::int16_t>(p.size() >= 3 ? std::atoi(p[2].c_str()) : -1)),
                                    {false, false, true});
            log::info("auto key {}: preview asked", name);
        } else if (eq_api_ && p[0] == "CORPSE") {
            // "CORPSE[:<name>]" targets the nearest corpse (whose name starts with <name>)
            std::uint32_t best = 0;
            float best_d = 1e9f;
            for (const auto& [id, sp] : conn_->world().spawns) {
                float d = glm::distance(sp.position, feet_);
                if (sp.is_corpse && (p.size() < 2 || sp.name.rfind(p[1], 0) == 0) && d < best_d) best = id, best_d = d;
            }
            if (best) conn_->set_target(best);
            log::info("auto key {}: target {}", name, best);
        } else if (p[0] == "OBJECT") {
            // "OBJECT[:<name>]" clicks the nearest ground object (whose name starts with <name>): a world container
            std::uint32_t best = 0;
            float best_d = 1e9f;
            for (const auto& o : conn_->ground_objects()) {
                float d = glm::distance(o.position, feet_);
                if ((p.size() < 2 || o.name.rfind(p[1], 0) == 0) && d < best_d) best = o.drop_id, best_d = d;
            }
            bool ok = best && conn_->click_object(best);
            log::info("auto key {}: object {} {:.1f} away{}", name, best, best_d, ok ? "" : ", not clicked");
        } else if (p.size() >= 2 && (p[0] == "PICK" || p[0] == "WORLDSLOT")) {
            // "PICK:<slot>[:<bag index>]" left-clicks a possessions slot, "WORLDSLOT:<n>" an open world container's
            net::ItemSlot s = p[0] == "PICK" ? net::ItemSlot::possessions(static_cast<std::int16_t>(std::atoi(p[1].c_str())),
                                                                         static_cast<std::int16_t>(p.size() >= 3 ? std::atoi(p[2].c_str()) : -1))
                                             : net::ItemSlot{net::kInvWorld, static_cast<std::int16_t>(std::atoi(p[1].c_str())), -1, -1, 0, 0};
            // a fourth part holds the keys held: s Shift, c Ctrl ("PICK:28:-1:c" a Ctrl+click on slot 28)
            const std::string held = p.size() >= 4 ? p[3] : std::string();
            conn_->click_slot(s, held.find('s') != std::string::npos, held.find('c') != std::string::npos);
            log::info("auto key {}: clicked", name);
        } else if (p.size() >= 2 && p[0] == "RPICK") {
            // "RPICK:<slot>[:<bag index>[:<keys>]]" right-clicks a possessions slot with the keys held (s, c, a)
            const std::string held = p.size() >= 4 ? p[3] : std::string();
            auto r = conn_->right_click_item(
                net::ItemSlot::possessions(static_cast<std::int16_t>(std::atoi(p[1].c_str())),
                                           static_cast<std::int16_t>(p.size() >= 3 ? std::atoi(p[2].c_str()) : -1)),
                {held.find('s') != std::string::npos, held.find('c') != std::string::npos, held.find('a') != std::string::npos});
            log::info("auto key {}: right-clicked ({})", name, static_cast<int>(r));
        } else if (p[0] == "WORLDCOMBINE") {
            // "WORLDCOMBINE" presses the open world container's Combine
            conn_->combine_world_container();
            log::info("auto key {}: combine", name);
        } else if (p[0] == "GIVE") {
            // "GIVE" drops the cursor's item or coin on the target, as a left click on it does (a trade's request)
            conn_->give_to(conn_->world().target_id);
            log::info("auto key {}: give to {}", name, conn_->world().target_id);
        } else if (p[0] == "LOOT") {
            // "LOOT" loots the target, as a right click on the corpse does (a server's client may take /loot over)
            conn_->loot(conn_->world().target_id);
            log::info("auto key {}: loot {}", name, conn_->world().target_id);
        } else if (p.size() >= 3 && eq_api_ && p[0] == "MENUROW") {
            // "MENUROW:<menu>:<row>" chooses a row of an open context menu (0-based)
            bool ok = eq_api_->menu_click(std::atoi(p[1].c_str()), std::atoi(p[2].c_str()));
            log::info("auto key {}: {}", name, ok ? "chosen" : "no such row");
        } else if (eq_api_ && p.size() >= 3 && p[0] == "HOTBTN") {
            // "HOTBTN:<screen>:<child>[:right|rightheld]" clicks a hot bar's button (places the cursor's hot button,
            // else presses it; an empty one's click and a held right button open its menu)
            bool ok = eq_api_->click_hot_button(p[1], p[2], p.size() >= 4 ? std::string_view(p[3]) : "left");
            log::info("auto key {}: {}", name, ok ? "clicked" : "no such button");
        } else if (p[0] == "RCLICK") {
            // "RCLICK" right-clicks the target (its window: a merchant, a buyer, ...)
            bool ok = right_click_spawn(conn_->world().target_id);
            log::info("auto key {}: {}", name, ok ? "right-clicked" : "nothing");
        } else if (eq_api_ && p.size() >= 4 && p[0] == "TEXT") {
            // "TEXT:<screen>:<child>:<text>" sets an edit box's text as typing it would
            bool ok = eq_api_->set_child_text(p[1], p[2], p[3]);
            log::info("auto key {}: {}", name, ok ? "typed" : "no such edit");
        } else if (eq_api_ && p.size() >= 4 && p[0] == "DLIST") {
            // "DLIST:<screen>:<list>:<row>" double-clicks a list row
            bool ok = eq_api_->double_click_list(p[1], p[2], std::atoi(p[3].c_str()));
            log::info("auto key {}: {}", name, ok ? "double-clicked" : "no such list");
        } else if (eq_api_ && p.size() >= 4 && p[0] == "COMBO") {
            // "COMBO:<screen>:<child>:<index>" picks a combo box choice (the child may hold ':', a list cell's box)
            std::string child = p[2];
            for (std::size_t i = 3; i + 1 < p.size(); ++i) child += ":" + p[i];
            bool ok = eq_api_->select_combo(p[1], child, std::atoi(p.back().c_str()));
            log::info("auto key {}: {}", name, ok ? "picked" : "no such combo box");
        } else if (eq_api_ && p.size() >= 3 && (p[0] == "UI" || p[0] == "LIST")) {
            // "UI:<screen>:<child>:<button>" another button phase ("rightup": a quick right click)
            bool ok = p[0] == "UI" ? eq_api_->click_child(p[1], p[2], p.size() >= 4 ? p[3] : std::string("left"))
                                   : p.size() >= 4 && eq_api_->click_list(p[1], p[2], std::atoi(p[3].c_str()),
                                                                          p.size() >= 5 ? std::atoi(p[4].c_str()) : 0);
            log::info("auto key {}: {}", name, ok ? "clicked" : "no such child");
        } else if (auto cmd = game::command_by_name(name)) {
            log::info("auto key {} ({}): target {} stand state {}", name, *cmd, conn_->world().target_id,
                      conn_->player_status().stand_state);
            for (bool down : {true, false}) {
                game::KeyMap::Event e{*cmd, down};
                if (game::mechanics::is_movement_command(*cmd))
                    movement_command(*cmd, down, conn_->player_status().stand_state != net::kStandFreeze);
                else
                    key_command(e);
            }
            log::info("auto key {} done: target {} stand state {}", name, conn_->world().target_id,
                      conn_->player_status().stand_state);
        } else {
            log::warn("auto key {}: no such command", name);
        }
    }
    if (opts_.auto_target && conn_->world().target_id == 0) {
        opts_.auto_target = false;
        auto id = game::nearest_npc(conn_->world(), feet_, yaw_, 1e6f, 0);
        if (!id) id = game::nearest_npc(conn_->world(), feet_, yaw_ + glm::pi<float>(), 1e6f, 0);
        if (id) {
            yaw_ = game::heading_to_yaw(game::heading_to(feet_, conn_->world().spawns.at(id).position));
            conn_->set_target(id);
            conn_->consider();
        }
    }
    // The game rules run once per step of the game clock (core/step_clock.h, docs/mechanics.md "Frame rate"): none,
    // one or several per drawn frame. A step is ProcessControls, /follow and /nav steering, the physics, the collision
    // pass and the position report, as the original ran them once per frame at 60 frames a second.
    const MovementKeys frame_keys = mk;
    motion_.jumped = motion_.land_sound = false; // this frame's events, gathered over its steps
    motion_.fall_damage.reset();
    auto run_step = [&](const core::StepClock::Step& st) {
        mk = frame_keys;
        const glm::vec3 step_from_feet = feet_;
        const float step_from_yaw = yaw_, step_from_pitch = pitch_;
        const float dt_step = st.elapsed_ms ? static_cast<float>(st.elapsed_ms) / 1000.0f : dt;
        const double nav_now = std::chrono::duration<double>(start_time_.time_since_epoch()).count() +
                               static_cast<double>(st.ms) / 1000.0;
        // What the navigation steers by (app/navigator.h): last frame's place, water and breath.
        Navigator::Frame nf;
        {
            std::uint8_t stand = conn_->player_status().stand_state;
            nf.feet = feet_;
            nf.now = nav_now;
            nf.can_move = !conn_->interface_locked() && !conn_->player_status().dead &&
                          (stand == net::kStandStanding || stand == net::kStandCrouching);
            nf.player_input = player_moved;
            nf.height = floor_offset_;
            nf.swimming = motion_body_wet_ >= 5;
            nf.head_under = assets::is_water_type(motion_head_wet_);
            nf.air = movement_.max_air_supply() > 0
                         ? std::clamp(static_cast<float>(movement_.air_supply()) / static_cast<float>(movement_.max_air_supply()),
                                      0.0f, 1.0f)
                         : 1.0f;
        }
        follow_assist_strafed_ = false;
        // /follow: ProcessControls runs PlayerClient::FollowPlayerAI (game/follow.h) while the player can move
        // (standing or ducking, not frozen); it turns the heading, sets autorun (0xddf720) and the run mode
        // (0xddf670). Turn, strafe and back keys end it (TurnOffAutoFollow, after the movement update).
        // ProcessControls does nothing while the interface is locked (/safelock).
        // A Hailstone change, switchable (nav.follow, /nav option follow): the navigation mesh helps where the straight
        // line fails (app/navigator.h assist_follow): round walls and corners, unstuck, and on up to the navigator's
        // leash instead of the original's 200.
        if (std::uint32_t follow = conn_->interface_locked() || conn_->eye_id() ? 0 : conn_->follow_target()) {
            auto it = conn_->world().spawns.find(follow);
            std::uint8_t stand = conn_->player_status().stand_state;
            if (nav_ && nav_->status().active) nav_->stop(""); // a /follow replaces a /nav walk
            if (it == conn_->world().spawns.end()) {
                conn_->stop_follow(false);
            } else if (stand == net::kStandStanding || stand == net::kStandCrouching) {
                const game::Spawn& t = it->second;
                glm::vec3 me = game::world_to_server(feet_ + glm::vec3(0, 0, floor_offset_));
                glm::vec3 to = game::world_to_server(t.position);
                game::FollowInput fi;
                fi.y = me.y;
                fi.x = me.x;
                fi.z = me.z;
                fi.heading = game::yaw_to_heading(yaw_);
                fi.height = floor_offset_;
                fi.speed = last_speed_run_;
                fi.target_y = to.y;
                fi.target_x = to.x;
                fi.target_z = to.z;
                fi.target_height = t.heights.avatar_height;
                fi.target_speed = t.speed_run;
                fi.target_animation = t.is_corpse ? game::kFollowDeadAnimation : t.stand_state;
                fi.distance = game::true_distance(me, to, 0.0f); // CDisplay::TrueDistance(..., 0): flat
                // __CastRay(target, player): the line between them is clear.
                fi.clear_line = !collision_ || collision_->line_of_sight(t.position, feet_ + glm::vec3(0, 0, floor_offset_));
                const bool assist = nav_ && nav_->follow_assist_on();
                const float distance = fi.distance;
                // With the mesh's help the follow keeps on past the original's 200, to the navigator's leash.
                if (assist && fi.distance > game::kFollowTooFar && fi.distance <= Navigator::kFollowLeash)
                    fi.distance = game::kFollowTooFar;
                game::FollowStep step = game::follow_step(fi);
                if (step.stop_message) {
                    conn_->stop_follow_with(*step.stop_message);
                    if (nav_) nav_->end_follow();
                } else {
                    Navigator::FollowAssist help;
                    if (assist)
                        help = nav_->assist_follow(feet_, t.position - glm::vec3(0, 0, t.heights.avatar_height), fi.clear_line,
                                                   nav_now);
                    if (help.detour) {
                        // The original's bands (stop inside 15 + both heights, walk inside 30 + both when the target is
                        // slower), measured along the path.
                        yaw_ = help.steer.yaw;
                        float heights = fi.target_height + fi.height;
                        step.move = help.remaining >= 15.0f + heights;
                        step.run = !(help.remaining < heights + 30.0f && (fi.target_speed < fi.speed || fi.speed == 0.0f));
                        if (help.steer.jump) mk.jump = true;
                        mk.strafe_left = help.steer.strafe < 0;
                        mk.strafe_right = help.steer.strafe > 0;
                        follow_assist_strafed_ = help.steer.strafe != 0;
                    } else {
                        yaw_ = game::heading_to_yaw(step.heading);
                    }
                    // Swimming: the look pitch towards the path's next corner, or the spawn (forward follows it).
                    if (assist && step.move) {
                        glm::vec3 aim = help.detour ? help.aim : t.position - glm::vec3(0, 0, t.heights.avatar_height);
                        if (auto p = nav_->swim_pitch(nf, aim, help.detour ? std::optional<glm::vec3>(help.along) : std::nullopt))
                            pitch_ = *p;
                    }
                    if (follow_logged_ != follow || step.move != follow_move_ || (step.move && step.run != follow_run_) ||
                        help.detour != follow_detour_)
                        log::info("follow: {} at distance {:.1f} (heading {:.1f}){}", step.move ? (step.run ? "run" : "walk") : "stop",
                                  distance, step.heading, help.detour ? std::format(", round by the mesh ({:.0f} to go)", help.remaining) : "");
                    follow_logged_ = follow;
                    follow_move_ = step.move;
                    follow_run_ = step.run;
                    follow_detour_ = help.detour;
                    movement_.set_autorun(step.move ? 1 : 0);
                    if (step.move && conn_->running() != step.run) conn_->set_running(step.run);
                }
            }
        } else if (nav_) {
            nav_->end_follow();
        }
        // Navigation (app/navigator.h): /nav and /moveto steer as /follow does, by the heading and the movement keys,
        // so the walk keeps the movement rules. The player's own turn, move or strafe keys end it, as they end /follow.
        if (nav_ && !conn_->eye_id()) {
            nav::SteerOutput steer = nav_->frame(nf);
            if (steer.active) {
                yaw_ = steer.yaw;
                mk.forward = steer.forward;
                mk.jump = mk.jump || steer.jump;
                mk.strafe_left = steer.strafe < 0;
                mk.strafe_right = steer.strafe > 0;
                if (steer.pitch) pitch_ = *steer.pitch;
            }
        }
        abilities_test(mk);
        melee_test(mk);
        // The isometric camera's mouse look turns the camera alone: to the controls it is the camera behind the
        // player (6) with no look, so the turn keys turn and the buttons do not run.
        mk.mouse_look = mouse_look_.active && !iso();
        mk.both_buttons = mouse_look_.both_buttons && !iso();
        mk.camera_index = iso() ? game::kCameraWheelFollow : camera_type_;
        if (camera_type_ > game::kCameraFirstPerson && camera_type_ < game::kCameraWheelFollow) {
            const game::UserCamera& f = cameras_[camera_type_].f;
            mk.camera = {f.height, f.pitch, f.distance, f.heading};
            mk.camera_active = game::camera_active(camera_type_);
        } else {
            mk.camera = {};
            mk.camera_active = false;
        }
        float since_zone_in = std::chrono::duration<float>(Clock::now() - zone_entered_).count();
        ZoneMoveWorld world(collision_ ? &*collision_ : nullptr, world_objects_.get(), zone_ ? &zone_->regions : nullptr);
        update_vehicles();
        world.set_vehicles(&vehicles_, conn_->standing_on());
        update_npc_bodies();
        world.set_npcs(&npc_bodies_);
        {
            const auto& rules = conn_->server_rules();
            if (collision_) collision_->set_solid_ladders(!(rules && rules->configured && !rules->solid_ladders));
        }
        // The passable ladders, and while /nav or /stick moves the player, the doors not shut do not block it
        // (Navigator::passes_open_doors). The other spawns' contact probes follow the same rules (update()).
        world.apply_rules(conn_->server_rules(), conn_->doors(), nav_ && nav_->passes_open_doors());
        move_test_.drive(since_zone_in, mk, yaw_, pitch_, movement_, *conn_, zone_ ? &*zone_ : nullptr, &world, feet_);
        // Dead: no moving until the server moves us to the bind point.
        if (conn_->player_status().dead) mk = MovementKeys{};
        // Walls and floors: the original's swept sphere (game/swept_sphere.h)
        // against the zone's mesh and the doors, through DoCollisionMovement
        // (app/player_movement.cpp).
        update_riding();
        if (const game::Spawn* me = game::controlled_spawn(conn_->world())) {
            // AvatarHeight follows the spawn's heights (OP_ChangeSize, an illusion, CreatePlayerActor's fix-up): the
            // spawn's z stays, so the feet move by the difference. The body is the controlled spawn: the player, or
            // the mount it rides (whose z it took when mounting, and gives back when getting off).
            if (me->heights.avatar_height != floor_offset_) {
                movement_.lift(floor_offset_ - me->heights.avatar_height);
                feet_.z += floor_offset_ - me->heights.avatar_height;
                floor_offset_ = me->heights.avatar_height;
            }
            // The actor's sweep radius (game/mechanics/collision_movement.h: sweep_radius, a stand-in until the
            // model definition's +0x38 is read).
            float radius = game::mechanics::sweep_radius(game::mechanics::SweepRadiusSpawn{floor_offset_, me->race});
            movement_.set_collision_radius(radius);
            movement_.set_race(me->race);
            // 0x406740: the duck track's length (L08, or CRCH_BA_1 on an EQG skeleton: the actor's vtable +0xbc, as
            // the bone groups ask it) caps the jump's crouch window.
            std::optional<double> duck;
            if (actor_renderer_)
                if (auto model = actor_renderer_->model_for(*me))
                    if (const assets::AnimationClip* c =
                            model->clip(game::mechanics::jump_duck_clip(model->eqg)))
                        duck = static_cast<double>(c->frame_count) * static_cast<double>(c->frame_ms);
            movement_.set_crouch_window(game::mechanics::jump_crouch_window(duck));
            movement_.set_jump_animation(actor_renderer_ ? actor_renderer_->jump_animation_playing(me->id) : std::nullopt);
            movement_.set_touch_context(me->heights.height, static_cast<std::int32_t>(conn_->zone_id()));
            movement_.set_touch_handler([this](std::uint8_t id) { return touching_switch(id); });
        }
        // The player as a rider for the doors' next ProcessSwitches frame (net/game_connection: update_doors).
        rider_spawns_[0] = movement_.rider();
        door_riders_.spawns = rider_spawns_;
        door_riders_.reset_collision_counter = [this](std::size_t) { movement_.reset_collision_counter(); };
        door_riders_.actor_radius = [this](const game::Door& d) {
            return world_objects_ ? static_cast<double>(world_objects_->door_radius(d.id)) : 0.0;
        };
        conn_->set_door_riders(&door_riders_);
        // The find path's points go on the zone's floor (PlayerPath's floor probes).
        conn_->set_path_floor([this](const glm::vec3& p, float step_up) -> std::optional<float> {
            return collision_ ? collision_->floor_below(p, step_up) : std::nullopt;
        });
        {
            // Spells that landed on us since the last frame (EQ_Character::HitBySpell).
            auto landing = conn_->take_landing_effects();
            auto& body = local_body_movement_ ? *local_body_movement_ : movement_;
            if (landing.velocity_change != glm::vec3(0.0f)) body.push(landing.velocity_change);
            if (landing.heading) {
                if (local_body_movement_) local_body_yaw_ = game::heading_to_yaw(*landing.heading);
                else yaw_ = game::heading_to_yaw(*landing.heading);
            }
        }
        const auto now_ms = static_cast<std::uint32_t>(st.ms + 1); // the step's time (as Clock::now() - start_time_ + 1)
        {
            // CharacterZoneClient::DoPassageOfTime (0x45dc59): the target changed since the last frame (0xb827f8) while
            // auto attack is on: the red con context tip (game/targeting.h), unless we are in a group that has the
            // target (CharacterZoneClient_IsSelfOrGroupMember); then auto attack and auto fire go off.
            const auto& w = conn_->world();
            if (w.target_id != frame_target_) {
                const game::Spawn* me = w.player();
                auto t = w.spawns.find(w.target_id);
                if (me && t != w.spawns.end() && conn_->auto_attack()) {
                    const auto& g = conn_->group();
                    const std::string& name = t->second.name;
                    bool grouped = g.active && (g.has_member(game::display_name(name)) || g.has_member(name));
                    if (!grouped && game::red_con_tip_due(target_manager_, t->first, me->level, t->second.level)) {
                        if (eq_api_) eq_api_->activate_tip(0x20);
                        conn_->set_auto_attack(false);
                        conn_->set_auto_fire(false);
                    }
                }
                frame_target_ = w.target_id;
            }
        }
        // Melee (game/mechanics/melee.h): DoPassageOfTime's swing timers set the ready flags, ProcessControls (in the
        // movement update) turns a set flag into DoAttack(13 / 14) while auto attack is on.
        const game::mechanics::Character* melee_ch = stats_ui_ ? stats_ui_->character() : nullptr;
        const bool melee_on = actions_ui_ && player_ready_ && melee_ch;
        AbilityView melee_view;
        {
            auto& cs = movement_.controls_state();
            if (melee_on) {
                melee_view = ability_view();
                actions_ui_->melee_timers(*conn_, melee_ch, melee_view);
                cs.primary_ready = actions_ui_->attack_state().primary_ready;
                cs.secondary_ready = actions_ui_->attack_state().secondary_ready;
            }
            cs.attack_on = melee_on && conn_->auto_attack(); // __Attack
            cs.local_null = conn_->world().player() == nullptr;
        }
        // At the helm of a small boat (OP_ControlBoat, game/boats.h steer_boat): the movement keys sail it; the player
        // stands still on its deck and is carried.
        const bool at_helm = !conn_->eye_id() && conn_->controlled_boat() && conn_->controlled_boat() == conn_->standing_on();
        movement_.set_at_helm(at_helm);
        if (at_helm) {
            const auto& held = movement_.controller().held_keys();
            const std::uint8_t autorun = movement_.controller().autorun();
            const std::uint32_t boat = conn_->controlled_boat();
            const auto model = vehicle_models_.find(boat);
            conn_->steer_boat(mk.forward || held.forward || (autorun & 1) || mk.both_buttons, mk.back || held.backward,
                              mk.turn_left || held.turn_left, mk.turn_right || held.turn_right, dt_step,
                              [&](const game::BoatPose& from, const game::BoatPose& to) {
                                  // In the water: the region a little under the boat's waterline.
                                  if (zone_ && !zone_->regions.empty() &&
                                      !zone_->regions.lookup(to.position - glm::vec3(0, 0, 1.0f)).is_water())
                                      return false;
                                  // Clear of docks, the zone's walls and other boats: the hull's probes swept along the
                                  // step (game/boats.h hull_probes; without a model yet, only the water counts).
                                  if (model == vehicle_models_.end()) return true;
                                  return game::hull_clear(from, to, model->second.probes,
                                                          [&](const glm::vec3& a, const glm::vec3& b, float r) {
                                                              return world.hull_blocked(a, b, r, boat);
                                                          });
                              });
        }
        // The isometric camera: the movement keys point relative to the camera (game::iso_move).
        movement_.set_key_override(at_helm ? std::nullopt : iso_key_override());
        PlayerMovement::Frame frame = movement_.update(mk, yaw_, pitch_, dt_step * 1000.0f, now_ms, *conn_,
                                                       zone_ ? &zone_->regions : nullptr, floor_offset_, &world, melee_ch,
                                                       st.elapsed_ms);
        if (local_body_movement_) {
            local_body_pitch_ = movement_.controls_state().local.pitch * glm::pi<float>() / 256.0f;
            auto body = local_body_movement_->update({}, local_body_yaw_, local_body_pitch_, dt_step * 1000.0f,
                now_ms, *conn_, zone_ ? &zone_->regions : nullptr, local_body_height_, &world, melee_ch,
                st.elapsed_ms, true);
            conn_->set_player_motion(body.feet + glm::vec3(0, 0, local_body_height_),
                                     game::yaw_to_heading(local_body_yaw_), body.velocity, body.moving);
            conn_->set_air_supply(local_body_movement_->air_supply());
            conn_->set_player_body_wet(body.body_wet);
            conn_->set_standing_on(local_body_movement_->standing_on());
            if (actor_renderer_)
                actor_renderer_->set_player_motion({true, body.velocity, body.speed_run, body.feet_wet, body.body_wet,
                    body.speed_heading, conn_->auto_attack(), false, body.colliding_type});
        }
        if (melee_on) {
            const auto& cs = movement_.controls_state();
            actions_ui_->attack_state().primary_ready = cs.primary_ready;
            actions_ui_->attack_state().secondary_ready = cs.secondary_ready;
            for (std::int32_t slot : frame.attacks) actions_ui_->melee_attack(*conn_, melee_ch, melee_view, slot);
        }
        for (const auto& line : frame.messages) game_view_.chat.add(ui::ChatChannel::System, line);
        conn_->set_speed_run(frame.speed_run); // the spell book refuses to open while it is above 0
        if (frame.stand_state && !local_body_movement_) conn_->set_stand_state(*frame.stand_state, room_to_stand());
        // ProcessControls' forward key while unable to move, with AllowAutoStand: CEverQuest::Sit stands a sitting
        // player up; a looting one has the loot window closed.
        if (frame.sit_toggle) conn_->sit_toggle();
        if (frame.close_loot) conn_->end_loot();
        pitch_ = std::clamp(frame.pitch, -1.5f, 1.5f); // LookSpring, the look keys
        // The camera keys moved the active camera (cameras 1..5; the camera behind the player is this client's and
        // takes none: its pan and pitch keys do nothing here).
        if (frame.camera && camera_type_ > game::kCameraFirstPerson && camera_type_ < game::kCameraWheelFollow) {
            game::GameCamera& cam = cameras_[camera_type_];
            cam.f.height = frame.camera->pan;
            cam.f.distance = frame.camera->distance;
            cam.f.heading = frame.camera->heading;
            if (frame.camera_pitch_set) game::set_camera_pitch(camera_type_, cam, frame.camera->pitch);
        }
        move_test_.observe(since_zone_in, frame, movement_, *conn_);
        const glm::vec3 feet_before = feet_;
        feet_ = frame.feet;
        // Zoning onto a boat (game/boats.h): while a vehicle near where the player was put has no collision model yet
        // (its model is drawn a frame or two after it spawns), the player is held there for up to kBoatHoldSeconds after
        // the zone-in instead of dropping through its deck into the sea.
        constexpr float kBoatHoldSeconds = 5.0f;
        const bool hold = since_zone_in < kBoatHoldSeconds && !conn_->standing_on() &&
                          std::any_of(conn_->world().spawns.begin(), conn_->world().spawns.end(), [&](const auto& kv) {
                              const game::Spawn& s = kv.second;
                              if (kv.first == conn_->world().player_id || !game::is_vehicle_race(s.race, s.gender))
                                  return false;
                              if (glm::length(glm::vec2(game::drawn_pose(s).position - feet_)) > 80.0f) return false;
                              auto m = vehicle_models_.find(kv.first);
                              return m == vehicle_models_.end() || m->second.triangles.empty();
                          });
        if (hold && !boat_hold_) {
            boat_hold_ = feet_before; // where the zone-in put it, before this frame's fall
            log::info("vehicle: holding the player at ({:.1f} {:.1f} {:.1f}) until the boat near it is ready",
                      boat_hold_->x, boat_hold_->y, boat_hold_->z);
        }
        if (!hold) boat_hold_.reset();
        if (boat_hold_) {
            feet_ = *boat_hold_;
            movement_.place(feet_);
        }
        // Climbing aboard from the water (game/boats.h): swimming forward against a boat whose deck is at most
        // kClimbAboardHeight above the feet, just past the hull, puts the player on that deck.
        if (frame.wet && !boat_hold_ && !vehicles_.empty() && (mk.forward || movement_.moving_keys())) {
            constexpr float kClimbAboardHeight = 30.0f;
            const glm::vec3 ahead = feet_ + glm::vec3(std::cos(yaw_), std::sin(yaw_), 0.0f) *
                                                (movement_.collision_radius() + 3.0f);
            std::uint32_t boat = 0;
            const float top = world.floor_sweep(ahead + glm::vec3(0, 0, kClimbAboardHeight + 1.0f), ahead, 0.5f, &boat) -
                              0.5f;
            if (boat && top > feet_.z && top - feet_.z <= kClimbAboardHeight) {
                feet_ = glm::vec3(ahead.x, ahead.y, top + 0.1f);
                movement_.place(feet_);
                log::info("vehicle: climbed aboard {} at ({:.1f} {:.1f} {:.1f})", boat, feet_.x, feet_.y, feet_.z);
            }
        }
        last_velocity_ = frame.velocity;
        last_speed_run_ = frame.speed_run;
        // EQPlayer::TurnOffAutoFollow; not for the strafe /follow's mesh help pressed itself (the player's own keys still).
        if (frame.follow_off && conn_->follow_target() && (!follow_assist_strafed_ || player_moved)) conn_->stop_follow(true);
        motion_body_wet_ = frame.body_wet;
        if (!local_body_movement_) conn_->set_player_body_wet(frame.body_wet);
        if (actor_renderer_ && !local_body_movement_)
            actor_renderer_->set_player_motion({true, frame.velocity, frame.speed_run, frame.feet_wet, frame.body_wet,
                                                frame.speed_heading, conn_->auto_attack(),
                                                camera_type_ == game::kCameraFirstPerson,
                                                frame.colliding_type});
        motion_.moving = frame.moving;
        motion_.running = frame.running;
        motion_.wet = frame.wet;
        motion_head_wet_ = frame.head_wet;
        motion_.flying = frame.flying;
        motion_.jumped = motion_.jumped || frame.jumped;
        motion_.land_sound = motion_.land_sound || frame.land_sound;
        if (frame.fall_damage && !motion_.fall_damage) motion_.fall_damage = frame.fall_damage;
        yaw_ = frame.yaw;

        // Riding: the body is the mount (pinstControlledPlayer), and the rider sits where it is (PhysicsObject::vf8);
        // DoMainLoop sends the mount's physics under the rider's id (@0x53e28b), which set_player_motion does.
        if (riding_id_ || conn_->eye_id())
            conn_->set_controlled_motion(feet_ + glm::vec3(0, 0, floor_offset_), game::yaw_to_heading(yaw_),
                                         frame.velocity, frame.speed_run, frame.body_wet, frame.feet_wet,
                                         frame.colliding_type, frame.speed_heading);
        // +0x150: the vehicle the floor test stood on; the position goes out in its frame
        // (PackPlayerPositionUpdateOnVehicle).
        if (!local_body_movement_ && movement_.standing_on() != conn_->standing_on())
            log::info("vehicle: standing on {} (was {})", movement_.standing_on(), conn_->standing_on());
        if (!local_body_movement_) conn_->set_standing_on(movement_.standing_on());
        // +0x2d4 (the movement's pTouchingSwitch, game/mechanics/touching_switch.h): the trader stall check reads it.
        if (std::int32_t t = movement_.touching_switch(); t >= 0) conn_->set_touching_switch(static_cast<std::uint8_t>(t));
        else conn_->set_touching_switch(std::nullopt);
        if (!local_body_movement_) {
            conn_->set_player_motion(feet_ + glm::vec3(0, 0, floor_offset_), game::yaw_to_heading(yaw_), frame.velocity,
                                     frame.moving);
            conn_->set_air_supply(movement_.air_supply());
        }
        // Zone lines (game/zone_lines.h): CheckRegionChange -> DoTeleport (game/zoning.h).
        if (zone_ && !local_body_movement_)
            if (auto text = zone_line_.update(feet_, zone_->regions)) region_teleport(*text);
        // What this step moved and turned, for the drawing between it and the next (below).
        step_move_ = feet_ - step_from_feet;
        if (glm::length(step_move_) > 20.0f) step_move_ = glm::vec3(0.0f); // a placement or a teleport: not drawn across
        step_turn_ = std::remainder(yaw_ - step_from_yaw, glm::two_pi<float>());
        step_pitch_ = pitch_ - step_from_pitch;
    };
    for (const auto& st : frame_steps_) run_step(st);
    // The wheel (CEverQuest::MouseWheelHandler) is __ProcessMouseEvents', which stops at a locked interface. It
    // zooms between first person and the camera behind the player; in the cameras 1..5 it does nothing.
    iso_turn_keys(dt);
    if (!ui_.wants_mouse() && mouse_wheel_zoom_ && !conn_->interface_locked() && iso()) {
        game::iso_zoom(iso_camera_, in.wheel, iso_settings_);
    } else if (!ui_.wants_mouse() && mouse_wheel_zoom_ && !conn_->interface_locked() &&
        (camera_type_ == game::kCameraFirstPerson || camera_type_ == game::kCameraWheelFollow)) {
        camera_distance_ = std::clamp(camera_distance_ - in.wheel * 4.0f, 0.0f, max_camera_distance_);
        camera_type_ = camera_distance_ > 0.0f ? game::kCameraWheelFollow : game::kCameraFirstPerson;
    }
    // Drawn between the last two steps (core/step_clock.h): the camera is where the player was a share of a step ago
    // (1 - alpha of what the last step moved and turned); the mouse's turn since then is in yaw_ already. In
    // per-frame mode alpha is 1: where the step left it.
    const float behind = 1.0f - step_clock_.alpha();
    const bool seated = conn_->world().player() && conn_->world().player()->mount_id; // the eye follows the mount
    // The body (and the mount it rides) is drawn there too, not eased toward it as other spawns are: eased, it trailed
    // the camera by a twelfth of a second, which showed most when strafing in the camera behind the player.
    conn_->set_player_drawn(feet_ + glm::vec3(0.0f, 0.0f, floor_offset_) - behind * step_move_,
                            game::yaw_to_heading(yaw_ - behind * step_turn_));
    camera_.yaw = yaw_ - behind * step_turn_ + (camera_type_ == game::kCameraWheelFollow ? chase_heading_ : 0.0f);
    camera_.pitch = std::clamp(pitch_ - behind * step_pitch_, -1.5f, 1.5f);
    glm::vec3 eye = eye_position() - (seated ? glm::vec3(0.0f) : behind * step_move_);
    camera_.position = eye;
    if (iso()) {
        update_iso_camera(feet_ - behind * step_move_, dt);
    } else if (camera_type_ > game::kCameraFirstPerson && camera_type_ < game::kCameraWheelFollow) {
        update_game_camera(feet_ - behind * step_move_);
    } else if (camera_distance_ > 0.0f) {
        // Behind and above the player; stopped where scenery is in the way, as the cameras 1..5 are.
        camera_.position = game::wheel_follow_position(eye, -camera_.forward(), camera_distance_, camera_sweep());
    }
    if (opts_.watch.starts_with("object:")) {
        // Testing: look at ground object N from --camera-distance.
        auto id = static_cast<std::uint32_t>(std::strtoul(opts_.watch.c_str() + 7, nullptr, 10));
        for (const auto& o : conn_->ground_objects()) {
            if (o.drop_id != id) continue;
            float face = game::heading_to_yaw(o.heading) + glm::radians(opts_.watch_turn);
            float dist = camera_distance_ > 0.0f ? camera_distance_ : 15.0f;
            camera_.position = o.position + glm::vec3(std::cos(face) * dist, std::sin(face) * dist, dist * 0.5f);
            glm::vec3 to = o.position - camera_.position;
            camera_.yaw = std::atan2(to.y, to.x);
            camera_.pitch = std::atan2(to.z, glm::length(glm::vec2(to.x, to.y)));
        }
    } else if (opts_.watch.starts_with("door")) {
        // Testing: "door" looks at the --auto-door door, "door:N" at door N,
        // from --camera-distance along the door's heading, a little above it.
        std::uint8_t id = opts_.watch.size() > 5 ? static_cast<std::uint8_t>(std::atoi(opts_.watch.c_str() + 5)) : auto_door_id_;
        if (const game::Door* d = conn_->door(id)) {
            glm::vec3 at = d->world_position() + glm::vec3(0, 0, 4.0f);
            float face = game::heading_to_yaw(d->default_heading) + glm::radians(opts_.watch_turn);
            float dist = camera_distance_ > 0.0f ? camera_distance_ : 25.0f;
            camera_.position = at + glm::vec3(std::cos(face) * dist, std::sin(face) * dist, 6.0f);
            glm::vec3 to = at - camera_.position;
            camera_.yaw = std::atan2(to.y, to.x);
            camera_.pitch = std::atan2(to.z, glm::length(glm::vec2(to.x, to.y)));
        }
    } else if (!opts_.watch.empty()) {
        // Testing: look at the nearest spawn whose name contains opts_.watch.
        const game::Spawn* best = nullptr;
        float best_d = 0.0f;
        for (const auto& [id, sp] : conn_->world().spawns) {
            if (id == conn_->world().player_id || sp.name.find(opts_.watch) == std::string::npos) continue;
            float d = glm::distance(sp.position, feet_);
            if (!best || d < best_d) {
                best = &sp;
                best_d = d;
            }
        }
        if (best) {
            glm::vec3 at = best->display_valid ? best->display_position : best->position;
            // In front of it, pulled in when scenery is in the way.
            float face = game::heading_to_yaw(best->display_valid ? best->display_heading : best->heading);
            glm::vec3 dir(std::cos(face), std::sin(face), 0.0f);
            float dist = camera_distance_ > 0.0f ? camera_distance_ : 15.0f;
            glm::vec3 eye_at = at + glm::vec3(0, 0, 2.0f);
            if (collision_)
                for (; dist > 2.0f && !collision_->line_of_sight(eye_at, eye_at + dir * (dist + 1.0f)); dist *= 0.8f) {}
            camera_.position = eye_at + dir * dist;
            glm::vec3 to_spawn = at - camera_.position;
            camera_.yaw = std::atan2(to_spawn.y, to_spawn.x);
            camera_.pitch = std::atan2(to_spawn.z, glm::length(glm::vec2(to_spawn.x, to_spawn.y)));
        }
    }
    const game::Spawn* me = conn_->world().player();
    if (eq_host_.customization_shown && me) {
        // The player customization window in game: AboutToShow's SetCameraType(7) puts the view on Cameras[7], the
        // EQCamera_User of character creation, placed by the window's calls as there (its face camera from the
        // player's heading, CharSelect_PositionCameraForModel when a step zooms; game/char_select_camera.h) and
        // updated by EQCamera_User::vf2 with GameState 5's clamps (distance 2..200, height 0..30). The player is
        // drawn (hidden_spawn). AboutToHide's RestoreSavedCameraType gives the client's camera back below.
        if (!customization_camera_on_) {
            customization_camera_on_ = true;
            customization_saved_fov_ = camera_.fov_units;
            customization_camera_ = {};
        }
        game::CCreateInputs cc_in = ccreate_inputs_for(*me, conn_->world().height_tables.get());
        cc_in.heading = game::yaw_to_heading(yaw_); // PlayerBase +0x80
        for (const auto& call : std::exchange(eq_host_.customization_camera, {}))
            if (auto fov = game::customization_camera(customization_camera_, call, cc_in).fov) camera_.fov_units = *fov;
        game::UserCameraTarget target;
        target.position = feet_ + glm::vec3(0, 0, floor_offset_);
        target.eye_height = game::eye_height(me->heights, me->race, me->mount_id != 0);
        game::UserCameraPose pose = game::user_camera_update(customization_camera_, target, true);
        camera_.position = pose.position;
        camera_.yaw = game::heading_to_yaw(pose.heading);
        camera_.pitch = pose.pitch * glm::pi<float>() / 256.0f;
    } else if (customization_camera_on_) {
        customization_camera_on_ = false;
        camera_.fov_units = customization_saved_fov_;
    }
}

} // namespace hailstone::app::client_internal
