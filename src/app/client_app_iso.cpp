// ClientApp: the isometric camera's click to move and movement relative to the camera (docs/isometric.md). A
// Hailstone addition: every walk goes through the navigator (app/navigator.h), which steers only the heading and the
// movement keys, so the movement rules and what the server sees are the original's.

#include "app/client_app_internal.h"

#include "game/click_intent.h"

namespace hailstone::app::client_internal {

namespace {

// How close a walk to a thing goes before its click. Close enough for every click it ends in (a ground item is picked
// up within 20; a door's reach is the player's height + 20 + its radius; a ladder's mount check is the original's).
constexpr float kIsoObjectArrive = 6.0f;
constexpr float kIsoObjectReach = 12.0f;
// The held left button aims the walk again this often, when the ground under the cursor has moved this far.
constexpr double kIsoHoldSeconds = 0.15;
constexpr float kIsoHoldMove = 2.0f;

} // namespace

float ClientApp::iso_spawn_arrive(std::uint32_t id) const {
    // Inside the player's melee range of it (/stick's share of it, Navigator::kStickMeleeShare): near enough for its
    // right click (a merchant's, a banker's, a corpse's loot) whatever its size. 5 when the range is not known.
    const std::optional<float> melee = melee_range_to(id);
    return std::max(4.0f, melee ? *melee * Navigator::kStickMeleeShare : 5.0f);
}

game::IsoReveal ClientApp::iso_reveal() const {
    if (!iso() || !iso_camera_.placed) return {};
    game::IsoRevealInput in;
    in.camera = camera_.position;
    in.focus = iso_camera_.focus;
    in.feet_z = iso_camera_.focus.z - iso_focus_height();
    in.radius = iso_reveal_radius_ * iso_camera_.distance / game::kIsoRevealAtDistance;
    in.strength = iso_cone_strength_;
    in.roof_height = iso_roof_height_;
    in.roof_radius = std::max(iso_roof_radius_, game::kIsoRoofViewShare * iso_camera_.distance);
    in.roof_strength = iso_roof_strength_;
    return game::iso_reveal(in);
}

void ClientApp::update_iso_reveal(float dt) {
    if (!iso()) {
        iso_cone_strength_ = iso_roof_strength_ = 0.0f;
        return;
    }
    const game::CameraSweep sweep = camera_sweep();
    // Whether the zone or its objects stand in the way of a straight line: the camera's own sweep (radius 0.25),
    // stopped short of its end.
    auto blocked = [&](const glm::vec3& from, const glm::vec3& to) {
        std::optional<glm::vec3> stop = sweep(from, to, 0.25f);
        return stop && glm::distance(*stop, to) > 0.5f;
    };
    // The cone: while the line from the player's feet, middle or eyes to the camera is blocked (or always, with
    // camera.iso_reveal_always).
    const glm::vec3 feet = iso_camera_.focus - glm::vec3(0.0f, 0.0f, iso_focus_height());
    const glm::vec3 eye = eye_position();
    bool in_way = iso_reveal_always_;
    if (!in_way && iso_reveal_radius_ > 0.0f)
        for (const glm::vec3& p : {feet + glm::vec3(0.0f, 0.0f, 1.5f), iso_camera_.focus, eye})
            if (blocked(p, camera_.position)) {
                in_way = true;
                break;
            }
    iso_cone_strength_ = game::iso_ease(iso_cone_strength_, in_way && iso_reveal_radius_ > 0.0f ? 1.0f : 0.0f, dt);
    // The roof: while something solid is over the player's head, cut a little under it (game::iso_roof_height), so a
    // room keeps its walls up to its ceiling and only the ceiling and what is over it goes.
    std::optional<float> cover;
    if (iso_roof_) {
        const glm::vec3 up = eye + glm::vec3(0.0f, 0.0f, game::kIsoCoverReach);
        if (std::optional<glm::vec3> stop = sweep(eye, up, 0.25f); stop && glm::distance(*stop, up) > 0.5f)
            cover = stop->z;
    }
    if (cover) {
        const float target = game::iso_roof_height(*cover, eye.z);
        // A new cover takes its height at once; under it the height follows an arch or a sloping roof smoothly.
        if (iso_roof_strength_ <= 0.0f) iso_roof_height_ = target;
        else iso_roof_height_ += (target - iso_roof_height_) * (1.0f - std::exp(-dt / game::kIsoRoofHeightTau));
    }
    iso_roof_strength_ = game::iso_ease(iso_roof_strength_, cover ? 1.0f : 0.0f, dt);
}

std::optional<int> ClientApp::iso_option(std::string_view o) const {
    if (o == "iso_camera") return iso() ? 1 : 0;
    if (o == "iso_pitch") return static_cast<int>(std::lround(glm::degrees(iso_settings_.pitch)));
    if (o == "iso_fov") return static_cast<int>(std::lround(iso_settings_.fov_degrees));
    if (o == "iso_reveal") return static_cast<int>(std::lround(iso_reveal_radius_));
    if (o == "iso_reveal_always") return iso_reveal_always_ ? 1 : 0;
    if (o == "iso_roof") return iso_roof_ ? 1 : 0;
    if (o == "iso_silhouette") return iso_silhouette_ ? 1 : 0;
    if (o == "iso_wasd") return iso_wasd_camera_ ? 0 : 1;
    if (o == "iso_marker") return iso_marker_on_ ? 1 : 0;
    if (o == "iso_hover") return iso_hover_on_ ? 1 : 0;
    if (o == "click_move") return click_move_ ? 1 : 0;
    return std::nullopt;
}

bool ClientApp::set_iso_option(std::string_view o, int v) {
    if (o == "iso_camera") {
        if (v != 0 && !iso()) set_camera_type(game::kCameraIsometric);
        else if (v == 0 && iso()) set_view(true);
    } else if (o == "iso_pitch") {
        iso_settings_.pitch = glm::radians(static_cast<float>(std::clamp(v, 20, 89)));
    } else if (o == "iso_fov") {
        iso_settings_.fov_degrees = static_cast<float>(std::clamp(v, 10, 90));
    } else if (o == "iso_reveal") {
        iso_reveal_radius_ = static_cast<float>(std::clamp(v, 0, 60));
    } else if (o == "iso_reveal_always") {
        iso_reveal_always_ = v != 0;
    } else if (o == "iso_roof") {
        iso_roof_ = v != 0;
    } else if (o == "iso_silhouette") {
        iso_silhouette_ = v != 0;
    } else if (o == "iso_wasd") {
        iso_wasd_camera_ = v == 0;
    } else if (o == "iso_marker") {
        iso_marker_on_ = v != 0;
        if (!iso_marker_on_) iso_marker_.active = false;
    } else if (o == "iso_hover") {
        iso_hover_on_ = v != 0;
        if (!iso_hover_on_) iso_hover_ = {};
    } else if (o == "click_move") {
        click_move_ = v != 0;
        if (!click_moves() && nav_ && iso_walking_) nav_->stop(""); // a click's walk does not outlive the option
    } else {
        return false;
    }
    save_iso_settings();
    return true;
}

void ClientApp::load_iso_settings() {
    iso_settings_.pitch = glm::radians(std::clamp(config_float("camera.iso_pitch", 45.0f), 20.0f, 89.0f));
    iso_settings_.fov_degrees = std::clamp(config_float("camera.iso_fov", 63.0f), 10.0f, 90.0f);
    iso_camera_.distance = std::clamp(config_float("camera.iso_distance", iso_settings_.distance),
                                      iso_settings_.min_distance, iso_settings_.max_distance);
    iso_wasd_camera_ = config_.get_string("camera.iso_wasd", "camera") != "classic";
    iso_reveal_radius_ = std::clamp(config_float("camera.iso_reveal", 20.0f), 0.0f, 60.0f);
    iso_reveal_always_ = config_.get_bool("camera.iso_reveal_always", false);
    iso_roof_ = config_.get_bool("camera.iso_roof", true);
    iso_silhouette_ = config_.get_bool("camera.iso_silhouette", true);
    iso_marker_on_ = config_.get_bool("camera.iso_marker", true);
    iso_hover_on_ = config_.get_bool("camera.iso_hover", true);
    click_move_ = config_.get_bool("mouse.click_move", false);
}

void ClientApp::save_iso_settings() {
    config_.set("camera.iso_pitch", std::format("{:.1f}", glm::degrees(iso_settings_.pitch)));
    config_.set("camera.iso_fov", std::format("{:.1f}", iso_settings_.fov_degrees));
    config_.set("camera.iso_distance", std::format("{:.0f}", iso_camera_.distance));
    config_.set_bool("camera.iso", iso());
    config_.set("camera.iso_wasd", iso_wasd_camera_ ? "camera" : "classic");
    config_.set("camera.iso_reveal", std::format("{:.0f}", iso_reveal_radius_));
    config_.set_bool("camera.iso_reveal_always", iso_reveal_always_);
    config_.set_bool("camera.iso_roof", iso_roof_);
    config_.set_bool("camera.iso_silhouette", iso_silhouette_);
    config_.set_bool("camera.iso_marker", iso_marker_on_);
    config_.set_bool("camera.iso_hover", iso_hover_on_);
    config_.set_bool("mouse.click_move", click_move_);
    config_.save(config_path_);
}

void ClientApp::update_iso_hover() {
    if (!iso() || !iso_hover_on_ || !conn_ || ui_.wants_mouse() || mouse_look_.active || conn_->placement().active()) {
        iso_hover_ = {};
        return;
    }
    // Ten times a second, or at once when the mouse moved: a pick is too dear for every frame.
    const auto& in = window_.input();
    const glm::vec2 at(in.mouse_x, in.mouse_y);
    const auto now = Clock::now();
    if (at == iso_hover_at_ && now - iso_hover_time_ < std::chrono::milliseconds(100)) return;
    iso_hover_at_ = at;
    iso_hover_time_ = now;
    const PickTarget hit = pick_at(at.x, at.y, game::kHoverRequired, game::kHoverSkip, nullptr);
    IsoHover h;
    const auto& world = conn_->world();
    if (hit.kind == PickTarget::Kind::Spawn) {
        // Not the player, and not the target (its ring is there already).
        if (hit.id == world.player_id || hit.id == world.target_id) return void(iso_hover_ = {});
        auto it = world.spawns.find(hit.id);
        if (it == world.spawns.end()) return void(iso_hover_ = {});
        const game::Spawn& s = it->second;
        const glm::vec3 base = s.display_valid ? s.display_position : s.position;
        h.shown = true;
        h.spawn = hit.id;
        h.feet = base - glm::vec3(0.0f, 0.0f, s.heights.avatar_height);
        const bool has_actor = actor_renderer_ && actor_renderer_->model_for(s);
        h.radius = game::cap_ring_radius(
            game::target_ring_radius(s.heights.avatar_height, s.heights.model_scale, has_actor), ring_max_size_);
    } else if (hit.kind == PickTarget::Kind::Door || hit.kind == PickTarget::Kind::Object) {
        glm::vec3 p = hit.point;
        if (hit.kind == PickTarget::Kind::Door)
            if (const game::Door* d = conn_->door(static_cast<std::uint8_t>(hit.id))) p = d->world_position();
        // On the floor under it.
        if (collision_)
            if (auto floor = collision_->floor_below(p + glm::vec3(0.0f, 0.0f, 1.0f), 12.0f)) p.z = *floor;
        h.shown = true;
        h.feet = p;
        h.radius = 2.0f;
    }
    iso_hover_ = h;
}

void ClientApp::draw_iso_hover(const render::FrameInfo& frame, const glm::mat4& view_proj, float far_plane,
                               bool modern, double seconds) {
    if (!iso() || !iso_hover_.shown) return;
    // A spawn that went or became the target since the pick: nothing.
    if (iso_hover_.spawn && (iso_hover_.spawn == conn_->world().target_id || !conn_->world().spawns.contains(iso_hover_.spawn)))
        return;
    if (!iso_hover_renderer_) iso_hover_renderer_ = std::make_unique<render::TargetRingRenderer>(vk_, shader_dir_);
    render::TargetRing ring;
    ring.feet = iso_hover_.feet;
    ring.radius = iso_hover_.radius;
    ring.color = glm::vec4(0.92f, 0.96f, 1.0f, 0.5f); // a pale white, quieter than the target's
    ring.width = 0.1f;
    ring.spin = static_cast<float>(std::fmod(seconds * 0.6, static_cast<double>(glm::two_pi<float>())));
    ring.glow = 0.3f;
    render::TargetRingView view;
    view.view_proj = view_proj;
    view.camera = camera_.position;
    view.forward = camera_.forward();
    view.near_plane = camera_.near_plane;
    view.far_plane = far_plane;
    view.haze = modern ? frame_lighting_.haze : glm::vec4(0.0f);
    view.seconds = seconds;
    iso_hover_renderer_->draw(frame, view, ring);
}

void ClientApp::set_iso_marker(const glm::vec3& point, bool click) {
    if (!iso_marker_on_) return;
    iso_marker_.point = point;
    if (click || !iso_marker_.active) iso_marker_.click = true;
    iso_marker_.active = true;
    iso_marker_.ending = false;
    iso_marker_.ended = -1.0;
}

void ClientApp::draw_iso_marker(const render::FrameInfo& frame, const glm::mat4& view_proj, float far_plane,
                                bool modern, double seconds) {
    if (!iso_marker_shown()) return;
    IsoMarker& m = iso_marker_;
    if (m.click) {
        m.click = false;
        m.since = seconds;
    }
    if (m.ending) {
        m.ending = false;
        m.ended = seconds;
    }
    // A click lands as a wider ring that closes in over a quarter second with a flash; it then spins slowly while
    // the walk goes on, and fades over a third of a second once it ends.
    constexpr double kLand = 0.25, kFade = 0.33;
    const double age = seconds - m.since;
    float fade = 1.0f;
    if (m.ended >= 0.0) {
        fade = 1.0f - static_cast<float>(std::clamp((seconds - m.ended) / kFade, 0.0, 1.0));
        if (fade <= 0.0f) {
            m.active = false;
            return;
        }
    }
    const float land = static_cast<float>(std::clamp(age / kLand, 0.0, 1.0));
    if (!iso_marker_renderer_) iso_marker_renderer_ = std::make_unique<render::TargetRingRenderer>(vk_, shader_dir_);
    render::TargetRing ring;
    ring.feet = m.point;
    ring.radius = 1.4f + 1.4f * (1.0f - land) * (1.0f - land);
    ring.color = glm::vec4(1.0f, 0.86f, 0.45f, 0.9f * fade); // a warm gold, apart from the con colours
    ring.width = 0.12f;
    ring.spin = static_cast<float>(std::fmod(age * 1.5, static_cast<double>(glm::two_pi<float>())));
    ring.pulse = 0.5f + 0.5f * static_cast<float>(std::sin(age * 6.0));
    ring.flash = 1.0f - land;
    ring.glow = 0.7f;
    render::TargetRingView view;
    view.view_proj = view_proj;
    view.camera = camera_.position;
    view.forward = camera_.forward();
    view.near_plane = camera_.near_plane;
    view.far_plane = far_plane;
    view.haze = modern ? frame_lighting_.haze : glm::vec4(0.0f);
    view.seconds = seconds;
    iso_marker_renderer_->draw(frame, view, ring);
}

bool ClientApp::iso_click(const glm::vec2& at, const PickTarget& hit, bool right) {
    if (!nav_ || !conn_) return false;
    const auto& world = conn_->world();
    game::IsoClickInput in;
    in.right = right;
    in.placing = conn_->placement().active();
    in.cursor_item = conn_->inventory().cursor() != nullptr;
    std::optional<glm::vec3> ground;
    const game::Spawn* spawn = nullptr;
    switch (hit.kind) {
    case PickTarget::Kind::None:
        ground = world_point_at(at.x, at.y);
        in.target = ground ? game::IsoClickTarget::Ground : game::IsoClickTarget::Nothing;
        break;
    case PickTarget::Kind::Spawn: {
        in.target = game::IsoClickTarget::Spawn;
        auto it = world.spawns.find(hit.id);
        if (it == world.spawns.end()) return false;
        spawn = &it->second;
        in.walk_to_spawn = (spawn->is_npc || spawn->is_corpse) && hit.id != world.player_id;
        const glm::vec3 p = spawn->display_valid ? spawn->display_position : spawn->position;
        in.in_reach = glm::length(glm::vec2(p - feet_)) <= iso_spawn_arrive(hit.id) + 2.0f;
        break;
    }
    case PickTarget::Kind::Door:
        in.target = game::IsoClickTarget::Door;
        in.in_reach = door_reachable(static_cast<std::uint8_t>(hit.id));
        break;
    case PickTarget::Kind::Object:
        in.target = game::IsoClickTarget::Object;
        in.in_reach = glm::length(glm::vec2(hit.point - feet_)) <= kIsoObjectReach;
        break;
    case PickTarget::Kind::Ladder:
        in.target = game::IsoClickTarget::Ladder;
        in.in_reach = glm::length(glm::vec2(hit.point - feet_)) <= kIsoObjectReach;
        break;
    case PickTarget::Kind::Placed:
        in.target = game::IsoClickTarget::Placed;
        break;
    }
    const game::IsoClickAction action = game::iso_click_action(in);
    if (action == game::IsoClickAction::Original) return false;
    iso_pending_.reset();
    iso_marker_.active = false; // a walk to a spawn or a door has the target ring or the door itself to show
    std::string why;
    if (action == game::IsoClickAction::WalkTo) {
        why = nav_->walk_to_click(*ground, 2.0f);
        if (why.empty()) set_iso_marker(*ground, true);
        iso_hold_ = !right;
        iso_hold_point_ = *ground;
        iso_hold_at_ = Clock::now();
    } else if (hit.kind == PickTarget::Kind::Door) {
        // The navigator clicks the door itself once it is within reach (Navigator::go_to_door).
        why = nav_->go_to_door(static_cast<std::uint8_t>(hit.id), true);
    } else if (hit.kind == PickTarget::Kind::Spawn) {
        why = nav_->go_to_spawn(hit.id, iso_spawn_arrive(hit.id));
        if (why.empty()) iso_pending_ = IsoPending{hit, right};
    } else {
        why = nav_->walk_to_click(hit.point, kIsoObjectArrive);
        if (why.empty()) {
            iso_pending_ = IsoPending{hit, right};
            set_iso_marker(hit.point, true);
        }
    }
    if (!why.empty()) {
        conn_->show_chat("Cannot walk there: " + why + ".", 15);
        iso_hold_ = false;
    } else {
        iso_walking_ = true;
    }
    return true;
}

void ClientApp::update_iso_clicks(bool player_moved) {
    if (!nav_ || !conn_) return;
    const nav::NavStatus status = nav_->status();
    // The player's own movement keys end a click's walk, as they end /nav's, but quietly (each click would say so).
    if (iso_walking_ && player_moved && status.active) {
        nav_->stop("");
        iso_pending_.reset();
        iso_hold_ = false;
    }
    // (Not during a left-button mouse look either, which the other cameras' MouseTurnZoom starts: that drag turns.)
    if (!mouse_left_held_ || !click_moves() || mouse_look_.active) iso_hold_ = false;
    // The left button held on the ground: the walk follows the cursor (not while the mesh is still building, which
    // would ask again and again).
    if (iso_hold_ && status.active && status.mesh_complete && !ui_.wants_mouse() &&
        std::chrono::duration<double>(Clock::now() - iso_hold_at_).count() >= kIsoHoldSeconds) {
        const auto& in = window_.input();
        if (auto p = world_point_at(in.mouse_x, in.mouse_y); p && glm::distance(*p, iso_hold_point_) >= kIsoHoldMove) {
            iso_hold_at_ = Clock::now();
            iso_hold_point_ = *p;
            if (nav_->walk_to_click(*p, 2.0f).empty()) set_iso_marker(*p, false);
        }
    }
    if (nav_->status().active) return;
    iso_walking_ = false;
    if (iso_marker_.active && iso_marker_.ended < 0.0) iso_marker_.ending = true;
    // The walk is over: arrived, its click; stopped short (keys, no path, stuck), nothing.
    if (!iso_pending_) return;
    const IsoPending pending = *std::exchange(iso_pending_, std::nullopt);
    const PickTarget& hit = pending.hit;
    if (hit.kind == PickTarget::Kind::Spawn) {
        const auto& spawns = conn_->world().spawns;
        auto it = spawns.find(hit.id);
        if (it == spawns.end()) return;
        const glm::vec3 p = it->second.display_valid ? it->second.display_position : it->second.position;
        if (glm::length(glm::vec2(p - feet_)) > iso_spawn_arrive(hit.id) + 2.0f) return;
        // Facing it, as a player walking up to it would be.
        yaw_ = game::heading_to_yaw(game::heading_to(feet_, p));
        if (pending.right) right_click_spawn(hit.id);
        else left_click_spawn(hit.id);
        return;
    }
    if (glm::length(glm::vec2(hit.point - feet_)) > kIsoObjectReach) return;
    click_world(hit);
}

std::optional<bool> ClientApp::iso_key_override() {
    if (!iso() || !iso_wasd_camera_) return std::nullopt;
    const auto& k = movement_.controller().held_keys();
    if (!(k.forward || k.backward || k.strafe_left || k.strafe_right || k.turn_left || k.turn_right))
        return std::nullopt;
    // The gamepad's stick by its angle; the keys by the eight ways they make.
    glm::vec2 v = pad_iso_stick_;
    if (v == glm::vec2(0.0f))
        v = {static_cast<float>(k.strafe_right) - static_cast<float>(k.strafe_left),
             static_cast<float>(k.forward) - static_cast<float>(k.backward)};
    const game::IsoMove m = game::iso_move(v.y, v.x, iso_camera_.yaw);
    if (m.move) yaw_ = m.yaw;
    return m.move;
}

void ClientApp::iso_turn_keys(float dt) {
    if (!iso() || !iso_wasd_camera_) return;
    const auto& k = movement_.controller().held_keys();
    const float turn = static_cast<float>(k.turn_left) - static_cast<float>(k.turn_right);
    if (turn != 0.0f) game::iso_turn(iso_camera_, turn * game::kIsoTurnRate * dt);
}

} // namespace hailstone::app::client_internal
