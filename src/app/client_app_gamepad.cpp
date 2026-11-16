// ClientApp: the gamepad (docs/gamepad.md). A client extension: RoF2 has no gamepad scheme, so the pad presses the
// original's key commands (game/keymap.h) through the keys' own paths: key_command and movement_.command get them
// with the keys' events in update_player, and every game rule stays the original's. Its bindings are
// game/gamepad_bindings.h; its mode and settings app/gamepad.h.

#include "app/client_app_internal.h"

namespace hailstone::app::client_internal {

namespace {

using platform::PadAxis;
using platform::PadButton;

// Interact (A) uses the target's right-click action within this distance, else USE (a client choice: about the
// range the servers allow a merchant, banker or corpse to be used from).
constexpr float kPadInteractRange = 15.0f;
// Cursor mode: the right stick scrolls this many wheel notches a second at full deflection.
constexpr float kPadWheelRate = 8.0f;

// The chat key that toggles the chat input (game/chat_input.h: slot 488).
int toggle_chat_slot() {
    for (int slot = game::kChatKeyFirst; slot <= game::kChatKeyLast; ++slot)
        if (game::chat_key(slot).action == game::ChatKeyAction::ToggleChatFocus) return slot;
    return 0;
}

} // namespace

void ClientApp::update_gamepad(float dt) {
    pad_events_.clear();
    if (!gamepad_) return;
    Gamepad& gp = *gamepad_;
    const platform::GamepadState& pad = window_.input().pad;
    const game::GamepadSettings& s = gp.settings();
    window_.set_gamepad_tuning(s.deadzone, s.trigger_threshold);
    ui::set_screen_gamepad(gp.active(), pad.type); // the login, server and character screens' buttons
    for (auto& name : gp.take_focus_requests())
        if (ui_.visible()) ui_.request_window_focus(std::move(name));
    if (!ui_.visible()) gp.set_mode(PadMode::Game);
    pad_window_takes_b_ = gp.take_window_takes_b(); // (the UI's last frame: B closes one of its windows)
    const bool in_world = screen_ == Screen::InGame && player_ready_ && conn_;
    const bool active = gp.active();
    // Menu mode lasts while a window that asked for the pad is open (the last gamepad window closing ends it). A mouse
    // click elsewhere takes ImGui's focus away from it but not the pad: its next button gives it the focus back.
    if (active && gp.mode() == PadMode::Menu) {
        const bool open = ui_.pad_window_open();
        pad_menu_idle_ = open || ui_.window_focus_pending() ? 0 : pad_menu_idle_ + 1;
        if (pad_menu_idle_ > 2) gp.set_mode(PadMode::Game);
        else if (open && !ui_.pad_window_focused() &&
                 std::any_of(pad.pressed.begin(), pad.pressed.end(), [](bool b) { return b; }))
            ui_.refocus_pad_window();
    } else {
        pad_menu_idle_ = 0;
    }
    // Cursor mode is for playing with the pad alone: the real mouse moving or clicking takes over from its pointer.
    if (gp.mode() == PadMode::Cursor && window_.pad_pointer().mouse_used) gp.set_mode(PadMode::Game);
    // With the interface locked (/safelock) the windows have no mouse (__ProcessMouseEvents), and the pad's pointer
    // and window driving stand in for it: back to game mode, but for the chat input's windows (pad_chat_window: the
    // pad's keyboard, which unlocks it).
    if (in_world && conn_->interface_locked() && gp.mode() != PadMode::Game &&
        !(gp.mode() == PadMode::Menu && pad_chat_window(ui_.pad_focus_window())))
        gp.set_mode(PadMode::Game);
    const PadMode mode = active ? gp.mode() : PadMode::Game;
    if (!active || mode != PadMode::Game || !in_world) release_pad_game_input();
    window_.set_pad_pointer(active && mode == PadMode::Cursor);
    // Outside the world (login, servers, characters) there is nothing to play: the pad drives the windows.
    ui_.set_pad_navigation(active && (mode == PadMode::Menu || (mode == PadMode::Game && !in_world)));
    if (!active) return;
    if (mode == PadMode::Cursor) {
        pad_cursor(pad, dt);
        return;
    }
    if (mode == PadMode::Menu) return; // the windows have the pad (ImGui's navigation, hs.gamepad)
    if (!in_world) {
        if (pad.was_released(PadButton::Back) && !pad.is_down(PadButton::Start)) gp.set_mode(PadMode::Cursor);
        return;
    }
    // Game mode: the buttons and chords (game/gamepad_bindings.h), the left stick's movement, the right stick's camera.
    std::vector<game::PadEvent> events;
    gp.bindings().update(pad, events);
    for (const auto& e : events) pad_action(e);
    const game::StickMove move =
        game::stick_move(pad.axis(PadAxis::LX), pad.axis(PadAxis::LY), s.move_threshold, s.strafe_threshold);
    auto edge = [&](bool now, bool was, game::KeyCommand c) {
        if (now != was) pad_events_.push_back({static_cast<int>(c), now});
    };
    edge(move.forward, pad_stick_.forward, game::KeyCommand::FORWARD);
    edge(move.back, pad_stick_.back, game::KeyCommand::BACK);
    edge(move.left, pad_stick_.left, game::KeyCommand::STRAFE_LEFT);
    edge(move.right, pad_stick_.right, game::KeyCommand::STRAFE_RIGHT);
    pad_stick_ = move;
    // The isometric camera moves the player along the stick's own angle (ClientApp::iso_key_override).
    const bool stick_moves = move.forward || move.back || move.left || move.right;
    pad_iso_stick_ = stick_moves ? glm::vec2(pad.axis(PadAxis::LX), -pad.axis(PadAxis::LY)) : glm::vec2(0.0f);
    pad_camera(pad, dt);
}

// Leaving game mode (or the world, or the pad going) lets go of the held key commands and the stick's movement.
void ClientApp::release_pad_game_input() {
    if (!gamepad_) return;
    std::vector<game::PadEvent> events;
    gamepad_->bindings().release_all(events);
    for (const auto& e : events)
        if (e.action.kind == game::PadAction::Kind::Command) pad_events_.push_back({e.action.command, false});
    const game::StickMove none;
    pad_iso_stick_ = glm::vec2(0.0f);
    if (pad_stick_.forward) pad_events_.push_back({static_cast<int>(game::KeyCommand::FORWARD), false});
    if (pad_stick_.back) pad_events_.push_back({static_cast<int>(game::KeyCommand::BACK), false});
    if (pad_stick_.left) pad_events_.push_back({static_cast<int>(game::KeyCommand::STRAFE_LEFT), false});
    if (pad_stick_.right) pad_events_.push_back({static_cast<int>(game::KeyCommand::STRAFE_RIGHT), false});
    pad_stick_ = none;
}

// A key command pressed and let go, as a tapped key.
void ClientApp::pad_key(game::KeyCommand c) {
    pad_events_.push_back({static_cast<int>(c), true});
    pad_events_.push_back({static_cast<int>(c), false});
}

void ClientApp::pad_action(const game::PadEvent& e) {
    using K = game::PadAction::Kind;
    using C = game::KeyCommand;
    const game::PadAction& a = e.action;
    if (a.kind == K::Command) { // held as long as the button
        pad_events_.push_back({a.command, e.down});
        return;
    }
    if (!e.down) return;
    // With the interface locked (/safelock) only the chat key acts: the hot buttons, the targeting, the hot pages and
    // the cancel are keys __ExecuteCmd refuses then, the camera, the menu and the pointer the mouse's
    // (__ProcessMouseEvents). The plain commands above go through key_command, which refuses as the keys do.
    if (conn_->interface_locked() && a.kind != K::Chat) return;
    switch (a.kind) {
    case K::Hot:
        // As the hot button keys do (HOT1_*): CHotButtonWnd::DoHotButton on the bar's current page.
        if (a.bar >= 1 && a.bar <= 10 && a.button >= 1 && a.button <= 12)
            actions_ui_->hot_button(*conn_, stats_ui_ ? stats_ui_->character() : nullptr, ability_view(), a.bar - 1,
                                    a.button - 1);
        break;
    case K::Menu: gamepad_->set_mode(PadMode::Menu); break;
    case K::Cursor: gamepad_->set_mode(PadMode::Cursor); break;
    case K::CameraToggle:
        // First person, or the camera behind the player at its last distance (the mouse wheel's camera_distance_).
        set_view(camera_type_ != game::kCameraWheelFollow);
        break;
    case K::CameraIso:
        // The isometric camera, or back to the camera behind the player.
        if (iso()) set_view(true);
        else set_camera_type(game::kCameraIsometric);
        break;
    case K::Interact: pad_interact(); break;
    case K::Cancel: pad_cancel(); break;
    case K::TargetPrev:
        // The CYCLENPCTARGETS list backward (game/targeting.h); refused while frozen, as the target keys are.
        prepare_target_cycle();
        if (conn_->player_status().stand_state != net::kStandFreeze)
            if (auto id = game::target_previous_npc(target_manager_, conn_->world(), eye_position())) conn_->set_target(*id);
        break;
    case K::TargetNext: pad_key(C::CYCLENPCTARGETS); break;
    case K::TargetGroup: pad_target_group(); break;
    case K::HotPagePrev:
    case K::HotPageNext: {
        // HOTPAGE1_1..10: hot bar 1's page before / after the current one, round.
        int page = actions_ui_->hot_buttons().page(0) + (a.kind == K::HotPageNext ? 1 : 9);
        pad_key(static_cast<C>(static_cast<int>(C::HOTPAGE1_1) + page % 10));
        break;
    }
    case K::Chat:
        if (int slot = toggle_chat_slot()) {
            pad_events_.push_back({slot, true});
            pad_events_.push_back({slot, false});
        }
        break;
    default: break;
    }
}

// D-pad up: yourself, then each group member in turn (TARGETME, PARTY1..5: the F1..F6 keys).
void ClientApp::pad_target_group() {
    const game::GroupState& g = conn_->group();
    int next = 0;
    for (int step = 1; step <= game::kGroupSlots; ++step) {
        int n = (pad_group_member_ + step) % game::kGroupSlots;
        if (n == 0 || (g.active && !g.slots[static_cast<std::size_t>(n)].name.empty())) {
            next = n;
            break;
        }
    }
    pad_group_member_ = next;
    pad_key(next == 0 ? game::KeyCommand::TARGETME
                      : static_cast<game::KeyCommand>(static_cast<int>(game::KeyCommand::PARTY1) + next - 1));
}

// A: the target's right-click action (CEverQuest::RightClickedOnPlayer: loot, merchant, banker, trainer ...) when it
// is near, else the USE key at the middle of the screen (doors, ground objects). Holding an item, a player or an NPC
// whose right click opens nothing (no merchant, banker, trainer ...) gets the left click instead, as the mouse gives
// an item (CEverQuest::DropItemOrMoneyOnPlayer: a trade with a player, the give window with an NPC, the item into
// the open trade).
void ClientApp::pad_interact() {
    const auto& world = conn_->world();
    if (auto it = world.spawns.find(world.target_id); it != world.spawns.end() && it->first != world.player_id) {
        glm::vec3 to = it->second.display_valid ? it->second.display_position : it->second.position;
        if (glm::distance(to, feet_ + glm::vec3(0, 0, floor_offset_)) <= kPadInteractRange) {
            const bool holding = conn_->inventory().cursor() != nullptr;
            const bool player = !it->second.is_npc && !it->second.is_corpse;
            if (holding && player) {
                left_click_spawn(it->first);
                return;
            }
            if (right_click_spawn(it->first)) return;
            if (holding && !it->second.is_corpse) {
                left_click_spawn(it->first);
                return;
            }
        }
    }
    use_key(false);
}

// B: stop casting (CMD_STOP_CAST), else close the top window (CLOSE_TOP_WINDOW), else clear the target
// (CLEAR_TARGET).
void ClientApp::pad_cancel() {
    if (conn_->spell_state().casting.casting()) {
        pad_key(game::KeyCommand::CMD_STOP_CAST);
        return;
    }
    if (actions_ui_->close_top_window() || pad_window_takes_b_) return;
    if (conn_->world().target_id) pad_key(game::KeyCommand::CLEAR_TARGET);
}

// The right stick as update_mouse_look's look: sideways turns the player (while it can move) or, in the camera behind
// the player, the camera alone; up and down tilt the view. Dead or knocked out, nothing (__ProcessMouseEvents).
void ClientApp::pad_camera(const platform::GamepadState& pad, float dt) {
    const float rx = pad.axis(PadAxis::RX), ry = pad.axis(PadAxis::RY);
    // The interface locked (/safelock): no mouse look (__ProcessMouseEvents) and no controls (ProcessControls).
    if ((rx == 0.0f && ry == 0.0f) || conn_->player_down() || conn_->interface_locked()) return;
    const bool camera6 = camera_type_ == game::kCameraWheelFollow;
    const std::uint32_t stand = conn_->player_status().stand_state;
    const bool can_move = (stand == net::kStandStanding || stand == net::kStandCrouching) && !conn_->stunned();
    if (iso()) {
        // The isometric camera: sideways turns the camera about the player, up and down zoom.
        const game::GamepadSettings& s = gamepad_->settings();
        game::iso_turn(iso_camera_, -rx * s.camera_speed * dt);
        game::iso_zoom(iso_camera_, -(s.invert_y ? -ry : ry) * 4.0f * dt, iso_settings_);
        return;
    }
    if (!camera6 && !can_move) return;
    const game::GamepadSettings& s = gamepad_->settings();
    const float k = s.camera_speed * dt;
    if (can_move) {
        // As CEverQuest::RMouseDown in the camera 6: the player takes the camera's heading, then turns.
        yaw_ += chase_heading_;
        chase_heading_ = 0.0f;
        yaw_ -= rx * k;
    } else {
        chase_heading_ -= rx * k;
    }
    pitch_ = std::clamp(pitch_ - (s.invert_y ? -ry : ry) * k, -1.5f, 1.5f);
}

// Cursor mode: the left stick moves the pointer, the right stick scrolls, A is the left button and X the right one;
// B or Back leaves.
void ClientApp::pad_cursor(const platform::GamepadState& pad, float dt) {
    const float lx = pad.axis(PadAxis::LX), ly = pad.axis(PadAxis::LY);
    // Slow near the centre for aiming, fast at full deflection.
    const float mag = std::sqrt(lx * lx + ly * ly);
    const float speed = gamepad_->settings().cursor_speed * ui_.scale() * dt * mag;
    window_.move_pad_pointer(lx * speed, ly * speed, -pad.axis(PadAxis::RY) * kPadWheelRate * dt);
    window_.set_pad_pointer_buttons(pad.is_down(PadButton::A), pad.is_down(PadButton::X));
    if (pad.was_pressed(PadButton::B) || (pad.was_released(PadButton::Back) && !pad.is_down(PadButton::Start)))
        gamepad_->set_mode(PadMode::Game);
}

} // namespace hailstone::app::client_internal
