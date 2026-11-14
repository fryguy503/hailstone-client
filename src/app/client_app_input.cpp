// ClientApp: Key commands, chat keys, combat input and the remaining test drivers.

#include "app/client_app_internal.h"
#include "app/lua_eq_menu.h"
#include "game/mechanics/abilities.h"
#include "game/boats.h"
#include "game/slash_commands.h"
#include "core/env.h"
#include "core/float_chars.h"

namespace hailstone::app::client_internal {

// CanIFitHere and the room above for crouch to stand (unk_SetStandState): nothing between the feet and the
// standing height.
bool ClientApp::room_to_stand() const {
    if (!collision_) return true;
    return collision_->line_of_sight(feet_ + glm::vec3(0, 0, 0.5f), feet_ + glm::vec3(0, 0, floor_offset_ + 0.5f));
}

void ClientApp::face_target() {
    const auto& world = conn_->world();
    auto it = world.spawns.find(world.target_id);
    if (it == world.spawns.end() || it->first == world.player_id) return;
    glm::vec3 to = it->second.display_valid ? it->second.display_position : it->second.position;
    yaw_ = game::heading_to_yaw(game::heading_to(feet_, to));
}

void ClientApp::face_degrees(double degrees) {
    // MQ's degrees are clockwise from north; EQ's heading units count counter-clockwise (game/coords.h).
    yaw_ = game::heading_to_yaw(static_cast<float>((360.0 - degrees) * 512.0 / 360.0));
}

bool ClientApp::script_keypress(std::string_view command, int mode) {
    std::string name(command);
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto cmd = game::command_by_name(name);
    if (!cmd) cmd = game::command_by_name("CMD_" + name);
    if (!cmd) return false;
    if (mode != 2) script_keys_.push_back({*cmd, true});
    if (mode == 2) script_keys_.push_back({*cmd, false});
    if (mode == 0) script_key_release_.push_back({*cmd, false});
    return true;
}

// The key map's commands (game/keymap.h: the original's 479 mappable commands with
// KeypressHandler::ResetKeysToEqDefaults' default keys,
// rebindable in the key window). Movement is read as held commands in update_player.
void ClientApp::key_command(const game::KeyMap::Event& e) {
    if (!e.down) return;
    // __ExecuteCmd with the interface locked (/safelock): the commands that need control of the player do nothing.
    if (conn_->interface_locked() && game::key_command_needs_control(e.command)) return;
    using C = game::KeyCommand;
    const auto& world = conn_->world();
    const game::mechanics::Character* ch = stats_ui_ ? stats_ui_->character() : nullptr;
    int cmd = e.command;
    if (cmd == kAbilityBookCommand || cmd == kInventoryManagerCommand) { // the EQ menu's windows of Hailstone's own
        if (eq_api_) eq_api_->toggle(toggle_command_window(cmd));
        return;
    }
    if (cmd >= game::kChatKeyFirst) {
        chat_key(cmd);
        return;
    }
    // __ExecuteCmd case 32: HOT1_1 .. HOT10_12, CHotButtonWnd::DoHotButton on bar (command - 37) / 12.
    if (cmd >= static_cast<int>(C::HOT1_1) && cmd <= static_cast<int>(C::HOT10_12)) {
        const int i = cmd - static_cast<int>(C::HOT1_1);
        actions_ui_->hot_button(*conn_, ch, ability_view(), i / 12, i % 12);
        return;
    }
    // Case 33: HOTPAGE1_1 .. HOTPAGE10_10, bar (command - 157) / 10's page.
    if (cmd >= static_cast<int>(C::HOTPAGE1_1) && cmd <= static_cast<int>(C::HOTPAGE10_10)) {
        const int i = cmd - static_cast<int>(C::HOTPAGE1_1);
        actions_ui_->set_page(i / 10, i % 10);
        return;
    }
    if (cmd == static_cast<int>(C::CMD_TOGGLE_FIND_WINDOW)) {
        // Hailstone's Find window (app/lua_eq_find.cpp) in place of the original's FindLocationWnd, toggled as
        // __ExecuteCmd case 100 toggles that: shown, it goes; else a shown find path stops (0x66bf80:
        // PlayerPath_Stop); else it shows.
        if (eq_host_.find_open) eq_host_.find_open = false;
        else if (guide_.active()) guide_.stop(); // its guide path likewise
        else if (conn_ && conn_->player_path().active()) conn_->stop_find_path();
        else eq_host_.find_open = true;
        return;
    }
    // The window keys and EQ menu entries of the Lua windows: the stock window's show / hide.
    if (const char* screen = toggle_command_window(cmd); screen && eq_api_ && eq_api_->replaces(screen)) {
        eq_api_->toggle(screen);
        return;
    }
    if (cmd >= static_cast<int>(C::TARGET_XTARGET_1) && cmd <= static_cast<int>(C::TARGET_XTARGET_20)) {
        conn_->target_xtarget(cmd - static_cast<int>(C::TARGET_XTARGET_1)); // ExtendedTargetList_TargetSlot
        return;
    }
    // __ExecuteCmd cases 0x89..0x8c: the extended target keys.
    if (cmd == static_cast<int>(C::ADD_XTARGET)) return conn_->add_xtarget();
    if (cmd == static_cast<int>(C::REMOVE_XTARGET)) return conn_->remove_last_xtarget();
    if (cmd == static_cast<int>(C::CYCLE_XTARGET)) return conn_->target_next_xtarget();
    if (cmd == static_cast<int>(C::REMOVE_XTARGET_BY_SLOT)) return conn_->remove_target_xtargets();
    if (cmd >= static_cast<int>(C::CMD_OPENING_ATTACK1) && cmd <= static_cast<int>(C::CMD_OPENING_ATTACK8)) {
        // __ExecuteCmd case 103: the combat ability window's button n (0x659c80).
        actions_ui_->use_combat_ability_button(*conn_, cmd - static_cast<int>(C::CMD_OPENING_ATTACK1));
        return;
    }
    if (cmd >= static_cast<int>(C::CAST1) && cmd <= static_cast<int>(C::CAST12)) {
        conn_->cast_gem(cmd - static_cast<int>(C::CAST1));
        return;
    }
    switch (static_cast<C>(cmd)) {
    case C::TARGETME: conn_->target_group_member(0); break; // LocalPlayer_TargetSelfOrPet(0)
    case C::PARTY1:
    case C::PARTY2:
    case C::PARTY3:
    case C::PARTY4:
    case C::PARTY5: conn_->target_group_member(cmd - static_cast<int>(C::PARTY1) + 1); break;
    case C::TARGETPC:
    case C::CYCLEPCTARGETS:
    case C::TARGETNPC:
    case C::CYCLENPCTARGETS:
    case C::TARGETCORPSE:
    case C::CYCLECORPSETARGETS:
    case C::TOGGLETARGET:
    case C::TOGGLETWOTARGETS:
        // __ExecuteCmd's target keys (game/targeting.h): Tab swaps between yourself and the remembered target;
        // the others cycle the spawns within 500 units, nearest first. A frozen player's keys are refused.
        prepare_target_cycle();
        if (conn_->player_status().stand_state != net::kStandFreeze)
            if (auto id = game::target_key_command(target_manager_, cmd, world, eye_position()))
                conn_->set_target(*id);
        break;
    case C::CLOSE_TOP_WINDOW:
        // The client's own windows first, then the UI pack's (a window with "Close with Esc" ticked: eq.TakeCloseTopWindow).
        if (!actions_ui_->close_top_window()) eq_host_.close_top_window = true;
        break;
    case C::REPLY: reply_to_last_tell(); break; // __ExecuteCmd case 0x23 (default key R)
    case C::CLEAR_TARGET:
        // Escape while placing ends the placement (this client's: the original's cancel paths are not traced).
        if (conn_->placement().active()) conn_->placement_cancel();
        else conn_->set_target(0);
        break;
    // The item placement keys (__ExecuteCmd 146..157: ItemPlacement_SetMode, ResetRotation, ResetScale, the
    // 90-degree turns of 0x558820 by 128).
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_TOGGLE_MODE: conn_->placement_set_mode(-1); break;
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_SET_MODE_COLLISION: conn_->placement_set_mode(0); break;
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_SET_MODE_WALL: conn_->placement_set_mode(1); break;
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_SET_MODE_CURSOR: conn_->placement_set_mode(2); break;
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_SET_MODE_FREE: conn_->placement_set_mode(3); break;
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_RESET_ITEM_MOUSE_X_ROTATION: conn_->placement_reset_rotation(true, false, false); break;
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_RESET_ITEM_MOUSE_Y_ROTATION: conn_->placement_reset_rotation(false, true, false); break;
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_RESET_ITEM_WHEEL_ROTATION: conn_->placement_reset_rotation(false, false, true); break;
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_RESET_ITEM_SIZE: conn_->placement_reset_scale(); break;
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_ROTATE_ITEM_MOUSE_X_ROTATION_90_DEGREES: conn_->placement_rotate(128.0f, 0, 0); break;
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_ROTATE_ITEM_MOUSE_Y_ROTATION_90_DEGREES: conn_->placement_rotate(0, 128.0f, 0); break;
    case C::CMD_REAL_ESTATE_ITEM_PLACEMENT_ROTATE_ITEM_WHEEL_ROTATION_90_DEGREES: conn_->placement_rotate(0, 0, 128.0f); break;
    case C::CMD_LOCK_AGGRO: conn_->aggro_lock_toggle(); break;
    case C::CONSIDER: conn_->consider(); break;
    case C::HAIL:
        if (auto it = world.spawns.find(world.target_id); it != world.spawns.end() && it->first != world.player_id)
            conn_->say("Hail, " + game::display_name(it->second.name));
        else
            conn_->say("Hail");
        break;
    case C::USE: use_key(); break;
    case C::SIT_STAND: conn_->sit_toggle(); break;
    case C::DUCK:
        // __ExecuteCmd case 12: standing crouches, crouching stands (unk_SetStandState), when no jump is pending.
        if (!movement_.controller().jump_pending()) {
            if (conn_->player_status().stand_state == net::kStandStanding) conn_->set_stand_state(net::kStandCrouching);
            else if (conn_->player_status().stand_state == net::kStandCrouching)
                conn_->set_stand_state(net::kStandStanding, room_to_stand());
        }
        break;
    case C::RUN_WALK: conn_->set_running(!conn_->running()); break;
    case C::MOUSELOOK: conn_->toggle_mouse_look_option(); break;
    case C::MOUSELOOK_ENGAGE:
        // __ExecuteCmd case 9: __MouseLook flips (the cursor hides or comes back with it), unless frozen.
        if (conn_->player_status().stand_state != net::kStandFreeze) mouse_look_.active = !mouse_look_.active;
        break;
    // Case 21 (CDisplay::ToggleView) and cases 91..96 (SetCameraType 0..5), unless frozen. (Cases 91..96 also wait
    // while the chat input has the keys, 0xddf66c: key commands do not come then.)
    case C::TOGGLECAM:
        if (conn_->player_status().stand_state != net::kStandFreeze) toggle_view();
        break;
    case C::FIRST_PERSON_CAMERA:
    case C::OVERHEAD_CAMERA:
    case C::CHASE_CAMERA:
    case C::USER1_CAMERA:
    case C::USER2_CAMERA:
    case C::TETHER_CAMERA:
        if (conn_->player_status().stand_state != net::kStandFreeze)
            set_camera_type(cmd - static_cast<int>(C::FIRST_PERSON_CAMERA));
        break;
    case C::CAMP: conn_->camp(); break;
    case C::CMD_MERCENARIES: conn_->merc_windows_command(); break; // __ExecuteCmd case 131: Cmd_mercwindows
    case C::TOGGLE_GUILDMGMTWIN: social_ui_.toggle_guild(); break;
    case C::TOGGLE_RAIDWIN: social_ui_.toggle_raid(); break;
    case C::TOGGLE_FRIENDSWIN: social_ui_.toggle_friends(); break;
    case C::TOGGLE_POTION_BELT: conn_->set_potion_belt_open(!conn_->potion_belt_open()); break;
    case C::POTION_SLOT_1: conn_->potion_belt_use(0); break;
    case C::POTION_SLOT_2: conn_->potion_belt_use(1); break;
    case C::POTION_SLOT_3: conn_->potion_belt_use(2); break;
    case C::POTION_SLOT_4: conn_->potion_belt_use(3); break;
    case C::POTION_SLOT_5: conn_->potion_belt_use(4); break;
    case C::AUTOPRIM: {
        bool was = conn_->auto_attack();
        // __ExecuteCmd AUTOPRIM (0x4d8d91): turning auto attack on, standing, against another spawn: the red con
        // context tip (game/targeting.h); while the tip window shows it, attack stays off.
        if (!was && conn_->player_status().stand_state == net::kStandStanding)
            if (const game::Spawn* me = world.player())
                if (auto t = world.spawns.find(world.target_id); t != world.spawns.end() && t->first != world.player_id)
                    if (game::red_con_tip_due(target_manager_, t->first, me->level, t->second.level) && eq_api_ &&
                        eq_api_->activate_tip(0x20))
                        break;
        if (conn_->toggle_auto_attack()) face_target();
        break;
    }
    case C::INVENTORY:
        if (eq_api_ && eq_api_->replaces("InventoryWindow")) eq_api_->toggle("InventoryWindow");
        else if (item_ui_) item_ui_->toggle_inventory();
        break;
    // __ExecuteCmd cases 115..117: CInvSlotMgr_OpenAllBags, CloseAllBags and their toggle (0x6989e0).
    case C::OPEN_INV_BAGS:
        if (eq_api_ && eq_api_->replaces("Bag1")) eq_api_->open_all_bags();
        else if (item_ui_) item_ui_->open_all_bags(*conn_);
        break;
    case C::CLOSE_INV_BAGS:
        if (eq_api_ && eq_api_->replaces("Bag1")) eq_api_->close_all_bags();
        else if (item_ui_) item_ui_->close_all_bags();
        break;
    case C::TOGGLE_INV_BAGS:
        if (eq_api_ && eq_api_->replaces("Bag1")) eq_api_->toggle_all_bags();
        else if (item_ui_) item_ui_->toggle_all_bags(*conn_);
        break;
    case C::SPELLBOOK: if (spell_ui_) spell_ui_->toggle_book(); break;
    case C::CMD_STOP_CAST: conn_->stop_cast(); break;
    case C::MAIN:
        if (eq_api_ && eq_api_->replaces("ActionsWindow")) eq_api_->actions_key(0);
        else actions_ui_->toggle_actions(0);
        break;
    case C::ABILITIES:
        if (eq_api_ && eq_api_->replaces("ActionsWindow")) eq_api_->actions_key(1);
        else actions_ui_->toggle_actions(1);
        break;
    case C::COMBAT:
        if (eq_api_ && eq_api_->replaces("ActionsWindow")) eq_api_->actions_key(2);
        else actions_ui_->toggle_actions(2);
        break;
    case C::SOCIALS:
        if (eq_api_ && eq_api_->replaces("ActionsWindow")) eq_api_->actions_key(3);
        else actions_ui_->toggle_actions(3);
        break;
    case C::TOGGLE_MAINMENUWIN:
    case C::TOGGLE_SKILLS_WND:
    case C::CMD_TOGGLE_COMBAT_ABILITY_WIN: actions_ui_->toggle_actions(2); break;
    case C::TOGGLE_HOTBOX1WIN: actions_ui_->toggle_hotbar(); break;
    case C::TOGGLE_OPTIONSWIN: actions_ui_->toggle_keys(); break;
    case C::SCREENCAP: take_screenshot(); break;
    // The chat keys, while the keys go to the game (a chat input that has them takes its own keys, so these never
    // come then): the active chat window's input is filled and takes the keyboard (app/lua_eq_chat.cpp).
    case C::TELL: tell_key(); break;                       // __ExecuteCmd case 15: "/tell "
    case C::RTARGET: reply_target_key(); break;            // case 69: "/rtarget <the player at the reply index>"
    case C::CMDMODE_SAY: put_chat_input("'"); break;       // case 57 (@0x4d7f88): the say command's "'"
    case C::CMDMODE_EMOTE: put_chat_input(":"); break;     // and the emote command's ":"
    case C::HISTORY_UP: chat_history_key(true); break;     // cases 53 / 54: HistoryBack / HistoryForward
    case C::HISTORY_DOWN: chat_history_key(false); break;
    case C::PAGEUP: chat_page_key(-1); break;              // cases 55 / 56: the active window's output, a page
    case C::PAGEDOWN: chat_page_key(1); break;
    // Case 36 acts only while the chat input has the keys: there the input's Tab / Shift+Tab (eq.ChatCycleReply).
    case C::CYCLEREPLY:
    case C::CYCLEREPLY_BACK: break;
    // __ExecuteCmd cases 44..46: Who_BuildAndSendRequest (as the Actions window's Who button), the invite or follow
    // of Cmd_invite (GameConnection::group_invite: follows while invited), CEverQuest::Disband.
    case C::WHO: conn_->send_command("/who"); break;
    case C::INVITE_FOLLOW: conn_->group_invite(); break;
    case C::DISBAND: conn_->group_disband(); break;
    // __ExecuteCmd cases 50 / 51: ClipPlane one step nearer / farther (0..20), saved, the view distance at once.
    case C::CLIP_IN: set_option_value("clip_plane", opts_.clip_plane - 1); break;
    case C::CLIP_OUT: set_option_value("clip_plane", opts_.clip_plane + 1); break;
    // __ExecuteCmd case 90: the ContextMenus option (UI [Main], 0xde0d51) flips.
    case C::TOGGLE_CONTEXTMENUS:
        conn_->change_client_option(game::ClientOption::ContextMenus,
                                    conn_->client_option(game::ClientOption::ContextMenus) ? 0 : 1);
        break;
    // __ExecuteCmd case 161: Cmd_stopcast, Cmd_stopsong and Social_Stop(-1, 0) (the queued social).
    case C::CMD_STOP_ACTION:
        conn_->stop_cast();
        conn_->stop_song();
        actions_ui_->stop_social();
        break;
    // __ExecuteCmd case 163 (0x698d60): the bank's and the shared bank's bags open or close.
    case C::CMD_TOGGLE_BANK_BAGS:
        if (eq_api_ && eq_api_->replaces("Bag1")) eq_api_->toggle_all_bank_bags();
        break;
    default: break;
    }
}

void ClientApp::movement_command(int command, bool down, bool accept) {
    // __ExecuteCmd case 19 (CENTERVIEW): in the camera 0 the controlled and the local player's CameraAngle = 0; the
    // overhead camera's pitch -100, the chase, user and tether cameras' +0x44 set (game::center_view); in the camera 6
    // only the look keys are released.
    if (!movement_.command(command, down, accept) || command != game::mechanics::kCmdCenterView) return;
    if (camera_type_ == game::kCameraFirstPerson) pitch_ = 0.0f;
    else if (camera_type_ < game::kCameraWheelFollow) game::center_view(camera_type_, cameras_[camera_type_]);
}

void ClientApp::set_camera_type(int type) {
    game::CameraSelection s{camera_type_, camera_distance_, pad_camera_distance_};
    game::select_camera(s, type);
    apply_camera_selection(s);
}

// Every camera change ends here, so going into the isometric camera or out of it keeps the field of view right
// (it has its own, and the one it replaced comes back).
void ClientApp::apply_camera_selection(const game::CameraSelection& s) {
    if (s.type == game::kCameraIsometric && !iso()) {
        iso_saved_fov_ = camera_.fov_units;
        iso_camera_.placed = false;
        // It starts looking the way the player faces.
        iso_camera_.yaw = yaw_;
    } else if (s.type != game::kCameraIsometric && iso()) {
        camera_.fov_units = iso_saved_fov_;
        iso_focus_distance_ = 0.0f;
    }
    camera_type_ = s.type;
    camera_distance_ = s.distance;
    pad_camera_distance_ = s.pad_distance;
}

void ClientApp::update_iso_camera(const glm::vec3& feet, float dt) {
    camera_.fov_units = game::iso_fov_units(iso_settings_.fov_degrees);
    const glm::vec3 target = feet + glm::vec3(0.0f, 0.0f, iso_focus_height());
    const game::IsoPose pose = game::update_iso_camera(iso_camera_, target, dt, iso_settings_);
    camera_.position = pose.position;
    camera_.yaw = pose.yaw;
    camera_.pitch = pose.pitch;
    iso_focus_distance_ = pose.focus_distance;
    update_iso_reveal(dt);
}

void ClientApp::camera_command(const std::string& args) {
    // /camera                 which camera, and the isometric camera's settings
    // /camera iso|first|behind|toggle
    // /camera pitch <deg> | fov <deg> | distance <units>   (the isometric camera; saved)
    // /camera wasd camera|classic   (its movement keys: relative to the camera, or as in the other cameras)
    // /camera reveal <units>        (the cut through what stands in front of the player: its radius; 0 off)
    // /camera roof|silhouette|marker|hover|always on|off   (the cut's roof, the silhouette, the click marker, the
    //                                hover ring, the cut even when nothing is in the way)
    std::string_view rest = args;
    std::string word = game::next_cmd_arg(rest);
    for (char& ch : word) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    auto say = [this](const std::string& text) { conn_->show_chat(text, 15); };
    auto save = [this] { save_iso_settings(); };
    if (word.empty()) {
        if (iso()) {
            // What the cut is doing now (for tuning it against a place).
            const game::IsoReveal r = iso_reveal();
            say(std::format("Cut now: circle {:.0f}% (radius {:.0f} at you), roof {:.0f}% (above {:.0f}, {:.0f} round you; "
                            "your eyes at {:.0f}).",
                            iso_cone_strength_ * 100.0f, r.shape.x, iso_roof_strength_ * 100.0f, iso_roof_height_,
                            r.roof.y, eye_position().z));
        }
        say(std::format("Camera {}{}. Isometric: pitch {:.0f}, fov {:.0f}, distance {:.0f}, wasd {}, reveal {:.0f}. "
                        "Usage: /camera [iso|first|behind|toggle], /camera pitch|fov|distance|reveal <value>, "
                        "/camera wasd camera|classic",
                        camera_type_, iso() ? " (isometric)" : "", glm::degrees(iso_settings_.pitch),
                        iso_settings_.fov_degrees, iso_camera_.distance, iso_wasd_camera_ ? "camera" : "classic",
                        iso_reveal_radius_));
        return;
    }
    if (word == "iso" || word == "isometric") {
        set_camera_type(game::kCameraIsometric);
    } else if (word == "first") {
        set_camera_type(game::kCameraFirstPerson);
    } else if (word == "behind") {
        set_camera_type(game::kCameraFirstPerson);
        set_view(true);
    } else if (word == "toggle") {
        set_camera_type(iso() ? game::kCameraFirstPerson : game::kCameraIsometric);
        if (!iso()) set_view(true);
    } else if (word == "roof" || word == "silhouette" || word == "marker" || word == "hover" || word == "always") {
        std::string how = game::next_cmd_arg(rest);
        for (char& ch : how) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (how != "on" && how != "off") {
            say(std::format("Usage: /camera {} on|off", word));
            return;
        }
        set_iso_option(word == "always" ? "iso_reveal_always" : "iso_" + word, how == "on" ? 1 : 0);
        return; // set_iso_option saves
    } else if (word == "wasd") {
        std::string how = game::next_cmd_arg(rest);
        for (char& ch : how) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (how == "camera") iso_wasd_camera_ = true;
        else if (how == "classic") iso_wasd_camera_ = false;
        else {
            say("Usage: /camera wasd camera|classic");
            return;
        }
    } else if (word == "pitch" || word == "fov" || word == "distance" || word == "reveal") {
        std::string value = game::next_cmd_arg(rest);
        float v = 0.0f;
        if (value.empty() || float_from_chars(value.data(), value.data() + value.size(), v).ec != std::errc{}) {
            say(std::format("Usage: /camera {} <value>", word));
            return;
        }
        if (word == "pitch") iso_settings_.pitch = glm::radians(std::clamp(v, 20.0f, 89.0f));
        else if (word == "fov") iso_settings_.fov_degrees = std::clamp(v, 10.0f, 90.0f);
        else if (word == "reveal") iso_reveal_radius_ = std::clamp(v, 0.0f, 60.0f);
        else iso_camera_.distance = std::clamp(v, iso_settings_.min_distance, iso_settings_.max_distance);
    } else {
        say("Usage: /camera [iso|first|behind|toggle], /camera pitch|fov|distance|reveal <value>, "
            "/camera wasd camera|classic, /camera roof|silhouette|marker|hover|always on|off");
        return;
    }
    save();
}

void ClientApp::toggle_view() { set_camera_type(game::toggle_view_next(camera_type_)); }

void ClientApp::set_view(bool behind) {
    if (!behind) {
        set_camera_type(game::kCameraFirstPerson);
        return;
    }
    game::CameraSelection s{camera_type_, camera_distance_, pad_camera_distance_};
    game::select_behind(s, max_camera_distance_);
    apply_camera_selection(s); // (out of the isometric camera: its own field of view goes)
}

void ClientApp::take_screenshot() {
    if (screenshot_this_frame_) return;
    screenshot_this_frame_ = true;
    if (!vk_.request_screenshot(platform::new_screenshot_path(platform::screenshots_dir(), std::time(nullptr))) && conn_)
        conn_->ui_message("Screenshots are not available: the graphics driver does not let the screen be copied.");
}

// SCREENCAP has no key in the original's usual default sets (ResetKeysToEqDefaults binds it only in the third, to
// NPAD -), so Print Screen also takes one, on every screen, unless the player gave it to another command.
bool ClientApp::print_screen_takes_screenshot() const {
    constexpr std::uint8_t kPrintScreen = 0xb7; // DIK_SYSRQ (platform/window.cpp kKeyCodes)
    if (!actions_ui_) return true;
    const game::KeyCombo plain{kPrintScreen};
    for (int bank = 0; bank < 2; ++bank)
        for (int slot = 0; slot < game::kKeySlots; ++slot)
            if (slot != static_cast<int>(game::KeyCommand::SCREENCAP) && actions_ui_->keymap().binding(slot, bank) == plain)
                return false;
    return true;
}

game::CameraTarget ClientApp::camera_target(const glm::vec3& feet) const {
    game::CameraTarget t;
    t.position = feet + glm::vec3(0, 0, floor_offset_); // the spawn's z: the feet plus AvatarHeight
    t.heading = game::yaw_to_heading(yaw_);
    const game::Spawn* me = conn_ ? conn_->world().player() : nullptr;
    if (conn_ && conn_->world().view_id)
        if (auto it = conn_->world().spawns.find(conn_->world().view_id); it != conn_->world().spawns.end()) me = &it->second;
    if (!me) return t;
    t.eye_height = game::eye_height(me->heights, me->race, me->mount_id != 0);
    const std::uint32_t stand = me->id == conn_->world().player_id ? conn_->player_status().stand_state : me->stand_state;
    t.ducking = stand == net::kStandCrouching;
    t.sitting = stand == net::kStandSitting;
    if (me->mount_id) {
        // 0x58c8a0(mount, 0xe): the mount's seat as drawn (eye_position's).
        t.riding = true;
        auto it = conn_->world().spawns.find(me->mount_id);
        auto rider = actor_renderer_ ? actor_renderer_->choose_model(*me).model : nullptr;
        if (actor_renderer_ && it != conn_->world().spawns.end() && rider) {
            const auto* mm = actor_renderer_->drawn_model(it->first);
            int bone = mm ? mm->mount_point_bone(rider->code) : -1;
            glm::mat4 w;
            if (bone >= 0 && actor_renderer_->drawn_bone(it->first, bone, w)) t.mount_seat_z = w[3].z;
        }
    }
    glm::mat4 root;
    if (t.sitting && actor_renderer_ && actor_renderer_->drawn_bone(me->id, 0, root)) t.root_bone_z = root[3].z;
    return t;
}

game::CameraSweep ClientApp::camera_sweep() const {
    // unk_EQCamera_SetPosition's sweep: the zone and its objects, not spawns (filter 0x476410 lets a spawn collide
    // only as a vehicle; vehicles are not in this sweep). No collision: straight to the point.
    return [this](const glm::vec3& from, const glm::vec3& to, float radius) -> std::optional<glm::vec3> {
        if (!collision_) return to;
        game::SphereSweep s = game::SphereSweep::make(from, to, radius);
        if (!s.valid()) return std::nullopt;
        collision_->sweep_sphere(s);
        if (world_objects_) world_objects_->sweep_sphere(s);
        return s.collided ? s.start + s.delta * s.t : to;
    };
}

void ClientApp::update_game_camera(const glm::vec3& feet) {
    const game::CameraPose pose =
        game::update_camera(camera_type_, cameras_[camera_type_], camera_target(feet), camera_sweep());
    camera_.position = pose.position;
    camera_.yaw = game::heading_to_yaw(pose.heading);
    // A look-at pitch below the horizon is 512 - the angle (CMath's arctangent).
    const float pitch = pose.pitch > 256.0f ? pose.pitch - 512.0f : pose.pitch;
    camera_.pitch = std::clamp(pitch * glm::pi<float>() / 256.0f, -1.55f, 1.55f);
}

glm::vec3 ClientApp::eye_position() const {
    const game::Spawn* me = conn_ ? conn_->world().player() : nullptr;
    if (conn_ && conn_->world().view_id)
        if (auto it = conn_->world().spawns.find(conn_->world().view_id); it != conn_->world().spawns.end()) me = &it->second;
    // PlayerClient vfd0 for a classic model on foot: ViewHeight - AvatarHeight above z (7.44 above the feet for a
    // human). New-style models use the actor definition's +0x48, whose writer is not traced (docs/unknowns.md).
    float eye = me ? game::eye_height(me->heights, me->race, me->mount_id != 0) : 0.0f;
    if (me && me->mount_id) {
        // EQCamera_FirstPerson_UpdateFromActor @0x797620 on a mount: at the mount's x, y and its dag 0xe's height
        // (the rider's seat as drawn), else its z + 3.5, plus the rider's eye height; then forward along the
        // rider's heading by its actor definition's +0x40 (moddat.ini FPCOffset of its model code), and again by
        // 3.5 for race 367 on race 472, 2 for anyone else on race 472, else 1. The eye then turns about the seat by
        // the mount actor's pitch (vf8c, its +0x84) plus its +0x21c (vf22c): neither is kept here (0), which
        // leaves it where it is.
        const game::Spawn* m = nullptr;
        if (auto it = conn_->world().spawns.find(me->mount_id); it != conn_->world().spawns.end()) m = &it->second;
        glm::vec3 at = m ? (m->display_valid ? m->display_position : m->position) : feet_ + glm::vec3(0, 0, floor_offset_);
        float seat = at.z + 3.5f;
        auto rider = actor_renderer_ ? actor_renderer_->choose_model(*me).model : nullptr;
        if (actor_renderer_ && m) {
            const auto* mm = actor_renderer_->drawn_model(m->id);
            int bone = mm && rider ? mm->mount_point_bone(rider->code) : -1;
            glm::mat4 w;
            if (bone >= 0 && actor_renderer_->drawn_bone(m->id, bone, w)) seat = w[3].z;
        }
        const auto& tables = conn_->world().height_tables;
        float forward = rider && tables ? tables->fpc_offset(rider->code) : 0.0f;
        if (m && me->race == 367 && m->race == 472) forward += 3.5f;
        else if (m && m->race == 472) forward += 2.0f;
        else forward += 1.0f;
        glm::vec2 dir(std::cos(yaw_), std::sin(yaw_)); // heading 0 faces +x (game/coords.h)
        return glm::vec3(glm::vec2(at.x, at.y) + dir * forward, seat + eye);
    }
    return feet_ + glm::vec3(0, 0, floor_offset_ + eye);
}

// What the abilities code needs from the client this frame (app/actions_ui.h).
AbilityView ClientApp::ability_view() const {
    AbilityView v;
    v.position = local_body_movement_
        ? local_body_movement_->controller().body.feet + glm::vec3(0, 0, local_body_height_)
        : feet_ + glm::vec3(0, 0, floor_offset_);
    v.heading = game::yaw_to_heading(local_body_movement_ ? local_body_yaw_ : yaw_);
    v.velocity = local_body_movement_ ? local_body_movement_->controller().body.velocity : last_velocity_;
    if (auto seen = can_see_spawn(conn_->world().target_id)) v.can_see_target = *seen;
    return v;
}

// The player's melee range of a spawn (__get_melee_range over both spawns' MeleeRadius, model scale and
// AvatarHeight), for /stick's default distance; nullopt without such a spawn.
std::optional<float> ClientApp::melee_range_to(std::uint32_t id) const {
    const auto& world = conn_->world();
    const game::Spawn* me = world.player();
    auto it = world.spawns.find(id);
    if (!me || it == world.spawns.end()) return std::nullopt;
    auto actor = [](const game::Spawn& s) {
        game::mechanics::SkillActor a;
        a.melee_radius = s.melee_radius;
        a.scale = s.heights.model_scale;
        a.avatar_height = s.heights.avatar_height;
        return a;
    };
    const auto a = actor(*me), b = actor(it->second);
    return game::mechanics::melee_range(&a, &b);
}

// The player's eyes to a point 1 over the spawn's (shown) position, through the zone's collision; nullopt without
// collision or such a spawn. The abilities' line of sight, and Spawn.LineOfSight for scripts.
std::optional<bool> ClientApp::can_see_spawn(std::uint32_t id) const {
    const auto& world = conn_->world();
    auto it = world.spawns.find(id);
    if (!collision_ || it == world.spawns.end()) return std::nullopt;
    glm::vec3 to = (it->second.display_valid ? it->second.display_position : it->second.position) + glm::vec3(0, 0, 1.0f);
    return collision_->line_of_sight(eye_position(), to);
}

// The target keys' filters, set before each press. Hailstone: the NPC cycle skips players' pets and mercenaries
// unless the option tab_skip_pets is off, and takes only the NPCs in sight (can_see_spawn: the eyes to a point 1 over
// the spawn) unless tab_line_of_sight is off; without the zone's collision every NPC counts as in sight.
void ClientApp::prepare_target_cycle() {
    target_manager_.skip_pets = option_value("tab_skip_pets").value_or(1) != 0;
    const auto see_invisible = viewer_see_invisible_rank();
    target_manager_.invisible = [this, see_invisible](const game::Spawn& s) { return conn_->spawn_invisible(s, see_invisible); };
    if (option_value("tab_line_of_sight").value_or(1) != 0)
        target_manager_.in_sight = [this](const game::Spawn& s) { return can_see_spawn(s.id).value_or(true); };
    else
        target_manager_.in_sight = nullptr;
}

// --abilities-test: 6 s after zoning in walk up to the target (--auto-target picks one), then press hot buttons
// 1..n of bar 1 two seconds apart, through the same path as the keys (DoHotButton).
// The unnamed key slots 479..499 in game (game/chat_input.h): they arrive only while no window has the keyboard, so
// the chat input mode is 1 (the game) here. The character select and creation slots (493..498) are screen_keys'.
void ClientApp::chat_key(int slot) {
    using A = game::ChatKeyAction;
    const game::ChatKey k = game::chat_key(slot);
    switch (k.action) {
    case A::ToggleChatFocus:
    case A::InsertCharacter: {
        game::ChatInputWorld w;
        w.mode = 1;
        w.active_chat_window = eq_api_ != nullptr;
        const auto& held = movement_.controller().held_keys();
        w.forward = held.forward;
        w.backward = held.backward;
        w.turn_left = held.turn_left;
        w.turn_right = held.turn_right;
        w.held_extra = movement_.controls_state().strafing;
        std::vector<game::ChatInputCall> calls;
        if (k.action == A::ToggleChatFocus) game::toggle_chat_input_mode(w, calls);
        else game::set_chat_input_mode(w, 0, calls);
        const bool accept = conn_->player_status().stand_state != net::kStandFreeze && !conn_->interface_locked();
        for (const auto& c : calls) {
            if (c.fn == game::ChatInputCall::Fn::InputSetFocus)
                request_chat_focus(k.action == A::InsertCharacter ? std::string(1, k.character) : std::string());
            else if (c.fn == game::ChatInputCall::Fn::ExecuteCmd)
                movement_.command(game::mechanics::kCmdAutorun, true, accept, static_cast<std::uint8_t>(c.held_mask));
        }
        break;
    }
    case A::ClearChatBuffer: clear_chat_buffer(); break;
    case A::Exit: log::info("key 485 (exit): the client has no exit prompt"); break;
    case A::RecentreMouse: window_.center_mouse(); break;
    case A::PlayerCustomization: player_customization_key(); break;
    default: break; // character select / creation keys
    }
}

void ClientApp::player_customization_key() {
    // __ExecuteCmd case 0x1ec @0x4da0e0 (key 492, /facepick), pressed: with the player's actor, for an account with
    // the Luclin or Planes of Power expansion (bits 0x4 / 0x8; a PcClient vtable +0x8c answer and a non-English
    // client pass too, docs/unknowns.md); a model neither Luclin nor new-style gets 12377 (colour 0xd), else the
    // player customization window shows (vtable +0x90: shown, never toggled off, as 0xde0a2c is always 1).
    if (!conn_ || !eq_api_ || !actor_renderer_) return;
    const game::Spawn* me = conn_->world().player();
    auto model = me ? actor_renderer_->model_for(*me) : nullptr;
    if (!model) return;
    const std::uint32_t expansions = conn_->profile() ? conn_->profile()->expansions : 0;
    if (!(expansions & 0xc)) return;
    if (!model->luclin && !model->eqg) {
        conn_->system_message(game::kStrPreLuclinCustomization, 0xd);
        return;
    }
    eq_api_->show("PlayerCustomizationWnd", true);
}

// The character select and creation key slots (493..498) outside the game.
void ClientApp::screen_keys() {
    if (!actions_ui_ || ui_.wants_keyboard()) return;
    using A = game::ChatKeyAction;
    for (const auto& e : actions_ui_->process_keys(window_.input(), true)) {
        if (!e.down) continue;
        const A a = game::chat_key(e.command).action;
        if (screen_ == Screen::Characters && !char_view_.busy) {
            const int n = static_cast<int>(char_view_.characters.size());
            if (a == A::CharSelectNext && n > 0) char_view_.selected = std::min(char_view_.selected + 1, n - 1);
            else if (a == A::CharSelectPrevious && n > 0) char_view_.selected = std::max(char_view_.selected - 1, 0);
            else if (a == A::CharSelectQuit) back_to_login("", false);
            else if (a == A::EnterWorld && char_view_.selected >= 0 && char_view_.selected < n) {
                // as the Enter World button: the character is the account's last played (the login's Characters list)
                const std::string& name = char_view_.characters[static_cast<std::size_t>(char_view_.selected)].name;
                char_view_.status = {};
                note_entered_character(name);
                conn_->enter_world(name);
            }
        } else if (screen_ == Screen::Create && a == A::CreationEscape) {
            screen_ = Screen::Characters; // CCharacterCreation_Escape
        }
    }
}

void ClientApp::abilities_test(MovementKeys& mk) {
    if (opts_.abilities_test.empty() || !player_ready_) return;
    auto now = Clock::now();
    if (now - zone_entered_ < std::chrono::seconds(6)) return;
    const auto& world = conn_->world();
    auto it = world.spawns.find(world.target_id);
    if (abilities_test_step_ == 0) {
        // A level 1-2 NPC (the rodents and rats around the cities), nearest first.
        std::uint32_t best = 0;
        float best_d = 1e9f;
        for (const auto& [id, sp] : world.spawns) {
            float d = glm::distance(sp.position, feet_);
            if (sp.is_npc && !sp.is_corpse && sp.targetable && sp.level <= 2 && sp.pet_owner_id == 0 && d < best_d &&
                sp.name.rfind("camp", 0) != 0) {
                best = id;
                best_d = d;
            }
        }
        if (best && best != world.target_id) conn_->set_target(best);
        it = world.spawns.find(world.target_id);
    }
    if (it != world.spawns.end() && it->first != world.player_id) {
        glm::vec3 to = it->second.display_valid ? it->second.display_position : it->second.position;
        float d = glm::length(glm::vec2(to - feet_));
        face_target();
        if (d > 10.0f && abilities_test_step_ == 0 && now - zone_entered_ < std::chrono::seconds(21)) {
            mk.forward = true;
            return;
        }
    }
    std::size_t buttons = static_cast<std::size_t>(std::count(opts_.abilities_test.begin(), opts_.abilities_test.end(), ',') + 1);
    if (abilities_test_step_ >= buttons || now - abilities_test_at_ < std::chrono::seconds(2)) return;
    abilities_test_at_ = now;
    log::info("abilities test: hot button {}", abilities_test_step_ + 1);
    actions_ui_->hot_button(*conn_, stats_ui_ ? stats_ui_->character() : nullptr, ability_view(), 0,
                            static_cast<int>(abilities_test_step_));
    ++abilities_test_step_;
}

// --melee-test N: 6 s after zoning in, target the nearest level 1-2 NPC (the next one when it dies, for N s), walk up to it and turn auto attack on
// (the attack key's path); after N seconds walk backward out of range for 2 s (124), walk back in, turn around
// for 4 s (108), then turn attack off. The swings and refusals are in the log ("melee: DoAttack").
void ClientApp::melee_test(MovementKeys& mk) {
    if (opts_.melee_test <= 0 || !player_ready_ || melee_test_phase_ > 5) return;
    auto now = Clock::now();
    if (now - zone_entered_ < std::chrono::seconds(6)) return;
    const auto& world = conn_->world();
    auto since = [&](int s) { return now - melee_test_at_ >= std::chrono::seconds(s); };
    auto next = [&](const char* what) {
        ++melee_test_phase_;
        melee_test_at_ = now;
        log::info("melee test: phase {} ({})", melee_test_phase_, what);
    };
    if (melee_test_phase_ == 0) {
        std::uint32_t best = 0;
        float best_d = 1e9f;
        for (const auto& [id, sp] : world.spawns) {
            float d = glm::distance(sp.position, feet_);
            if (sp.is_npc && !sp.is_corpse && sp.targetable && sp.level <= 2 && sp.pet_owner_id == 0 && d < best_d &&
                sp.name.rfind("camp", 0) != 0 && sp.position != glm::vec3(0.0f)) {
                best = id;
                best_d = d;
            }
        }
        if (best && best != world.target_id) conn_->set_target(best);
        auto it = world.spawns.find(world.target_id);
        if (it == world.spawns.end()) return;
        glm::vec3 to = it->second.display_valid ? it->second.display_position : it->second.position;
        face_target();
        if (glm::length(glm::vec2(to - feet_)) > 6.0f && now - zone_entered_ < std::chrono::seconds(60)) {
            mk.forward = true;
            return;
        }
        log::info("melee test: target {} '{}' at ({}, {}, {}), feet ({}, {}, {})", it->first, it->second.name, to.x, to.y,
                  to.z, feet_.x, feet_.y, feet_.z);
        if (!conn_->auto_attack()) conn_->toggle_auto_attack();
        next("attacking");
        return;
    }
    if (melee_test_phase_ == 1) {
        if (melee_test_fight_ == Clock::time_point{}) melee_test_fight_ = now;
        auto it = world.spawns.find(world.target_id);
        bool alive = it != world.spawns.end() && !it->second.is_corpse && it->first != world.player_id;
        if (alive) face_target();
        if (now - melee_test_fight_ >= std::chrono::seconds(opts_.melee_test)) {
            next("backing off");
        } else if (!alive && since(2)) {
            melee_test_phase_ = 0; // the next one
            log::info("melee test: target gone, the next one");
        }
        return;
    }
    if (melee_test_phase_ == 2) {
        face_target();
        mk.back = !since(2);
        if (since(4)) next("walking back in");
        return;
    }
    if (melee_test_phase_ == 3) {
        face_target();
        auto it = world.spawns.find(world.target_id);
        bool close = it == world.spawns.end() ||
                     glm::length(glm::vec2((it->second.display_valid ? it->second.display_position : it->second.position) - feet_)) <= 6.0f;
        mk.forward = !close && !since(6);
        if (close || since(6)) next("turned away");
        return;
    }
    if (melee_test_phase_ == 4) {
        face_target();
        yaw_ += glm::pi<float>();
        if (since(4)) next("attack off");
        return;
    }
    if (conn_->auto_attack()) conn_->toggle_auto_attack();
    next("done");
}

void ClientApp::update_combat_input() {
    for (const auto& e : key_events_) key_command(e);
    // CEverQuest::LMouseUp (update_mouse_look: a world click, not during mouse look): ClickedPlayer, then
    // LeftClickedOnPlayer; no spawn: a door or a ground object (click_world), or, with an item on the cursor, the
    // ground: drop it after asking (DropHeldItemOnGround without /fastdrop).
    if (auto at = std::exchange(left_click_, std::nullopt)) {
        // ItemPlacement_OnLeftMouseUp first: while placing, a click confirms a valid spot (0x711a) and does nothing
        // else. The box sent is the ghost's world box (0x4907f0's, max then min).
        if (conn_->placement().active()) {
            glm::vec3 lo{0.0f}, hi{0.0f};
            if (world_objects_)
                if (auto b = world_objects_->ghost_bounds()) {
                    glm::mat4 t = game::placed_transform(conn_->placement().ghost());
                    lo = glm::vec3(std::numeric_limits<float>::max());
                    hi = -lo;
                    for (int i = 0; i < 8; ++i) {
                        glm::vec3 c{(i & 1) ? b->hi.x : b->lo.x, (i & 2) ? b->hi.y : b->lo.y, (i & 4) ? b->hi.z : b->lo.z};
                        glm::vec3 w = glm::vec3(t * glm::vec4(c, 1.0f));
                        lo = glm::min(lo, w);
                        hi = glm::max(hi, w);
                    }
                }
            conn_->placement_click(hi, lo);
            return;
        }
        PickTarget hit = pick_at(at->x, at->y, game::kClickRequired, game::kClickSkip, "left click");
        // Click to move (game/click_intent.h; the isometric camera's, or mouse.click_move in any camera): the ground,
        // or a thing out of reach, is walked to.
        if (click_moves() && iso_click(*at, hit, false)) return;
        // A mount race with a rider (PlayerBase_IsMountRace, 0x58cfb0) gives the rider (LMouseUp @0x53875b).
        if (hit.kind == PickTarget::Kind::Spawn) {
            const auto& spawns = conn_->world().spawns;
            if (auto m = spawns.find(hit.id); m != spawns.end() && game::is_mount_race(m->second.race) &&
                                              m->second.rider_id != 0 && spawns.contains(m->second.rider_id))
                hit.id = m->second.rider_id;
            left_click_spawn(hit.id);
        }
        else if (hit.kind != PickTarget::Kind::Door && conn_->inventory().cursor() &&
                 conn_->re_place_cursor_item(static_cast<std::int32_t>(at->x), static_cast<std::int32_t>(at->y))) {
            // LMouseUp: a placeable item on the cursor asks to be placed instead of dropping (0x626e).
        } else if (!click_world(hit) && conn_->inventory().cursor() && conn_->request_drop_cursor()) drop_confirm_ = true;
    }
}

// HAILSTONE_TRACE_SPAWN=name: log one spawn's server and drawn positions ten
// times a second (to check how smoothly spawns move).
void ClientApp::trace_spawn(float dt) {
    // HAILSTONE_TRACE_VEHICLES: every vehicle spawn every 2 s (testing boats, game/boats.h).
    static const bool vehicles = hailstone::core::env("TRACE_VEHICLES") != nullptr;
    static float vehicle_timer = 0.0f;
    if (vehicles && conn_ && (vehicle_timer += dt) >= 2.0f) {
        vehicle_timer = 0.0f;
        for (const auto& [id, s] : conn_->world().spawns) {
            if (!game::is_vehicle_race(s.race, s.gender)) continue;
            log::info("vehicle {} {} race {} gender {}: ({:.1f} {:.1f} {:.1f} h {:.1f}) {:.0f} away, model {}, {}",
                      id, game::display_name(s.name), s.race, s.gender, s.display_position.x, s.display_position.y,
                      s.display_position.z, s.display_heading, glm::distance(s.display_position, feet_),
                      actor_renderer_ && actor_renderer_->drawn_model(id) ? "drawn" : "not drawn",
                      vehicle_models_.contains(id) ? std::to_string(vehicle_models_.at(id).triangles.size() / 3) +
                                                         " triangles"
                                                   : "no triangles");
        }
    }
    static const char* want = hailstone::core::env("TRACE_SPAWN");
    static float timer = 0.0f;
    if (!want || !conn_ || (timer += dt) < 0.1f) return;
    timer = 0.0f;
    for (const auto& [id, s] : conn_->world().spawns) {
        if (game::display_name(s.name).rfind(want, 0) != 0) continue;
        if (conn_->standing_on() && conn_->standing_on() != id) continue; // the boat the player is on
        log::info("trace {} {} race {}/{}: physics ({:.1f} {:.1f} {:.1f} h {:.1f}) drawn ({:.1f} {:.1f} {:.1f} h {:.1f}) SpeedRun "
                  "{:.3f} vel {:.3f}/tick, last update moved it {:.2f}, rides {}; player feet ({:.1f} {:.1f} {:.1f}) on {}",
                  id, game::display_name(s.name), s.race, s.gender, s.position.x, s.position.y, s.position.z, s.heading,
                  s.display_position.x, s.display_position.y, s.display_position.z, s.display_heading, s.speed_run,
                  glm::length(s.velocity), s.snap_error, s.on_vehicle, feet_.x, feet_.y, feet_.z,
                  conn_->standing_on());
        // Grounding (game::WorldState::ground_npcs): the floor the spawn is measured against and what was made of it.
        auto floor = collision_ ? grounding_floor(s.position) : std::nullopt;
        log::info("trace {} floor {} avatar height {:.2f} height {:.2f} size {:.2f} model scale {:.2f} fly mode {} "
                  "body wet {} drawn {:.2f} lower, last update {:.1f} s ago",
                  id, floor ? std::format("{:.2f}", *floor) : std::string("none"), s.heights.avatar_height,
                  s.heights.height, s.size, s.heights.model_scale, s.fly_mode, s.body_wet, s.ground_drop,
                  std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() -
                      s.update_time);
        if (conn_->standing_on() == id) {
            glm::vec3 local = game::boat_to_local(game::drawn_pose(s), feet_);
            log::info("trace on deck: ({:.2f} {:.2f} {:.2f}) yaw {:.1f}", local.x, local.y, local.z,
                      game::yaw_to_heading(yaw_) - s.display_heading);
        }
        break;
    }
}

std::optional<float> ClientApp::grounding_floor(const glm::vec3& p) const {
    std::optional<float> floor = collision_ ? collision_->floor_below(p, 4.0f) : std::nullopt;
    if (world_objects_)
        if (auto door = world_objects_->floor_below(p, 4.0f); door && (!floor || *door > *floor)) floor = door;
    return floor;
}

} // namespace hailstone::app::client_internal
