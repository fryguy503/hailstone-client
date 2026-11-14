// ClientApp: The in-game UI frame (draw_ui).

#include <cstdlib>

#include "app/client_app_internal.h"
#include "app/lua_eq_windows.h"
#include "core/env.h"
#include "core/web_url.h"
#include "platform/open_folder.h"

namespace hailstone::app::client_internal {

void ClientApp::update_screen_picture() {
    // The server of the pictures: the one picked on the login screen, else this login's.
    const std::string id = screen_ == Screen::Login ? ui::selected_profile_id(login_view_) : server_profile_.id;
    const std::filesystem::path server_dir =
        id.empty() ? std::filesystem::path() : profile_assets_dir(id, login_view_.servers_dir) / ui::kPictureFolder;
    bool pick = server_dir.string() != pictures_source_;
    if (screen_ != picture_screen_) {
        pick |= (screen_ == Screen::Login) || (screen_ == Screen::Loading && picture_screen_ == Screen::InGame);
        picture_screen_ = screen_;
    }
    if (pick) {
        pictures_source_ = server_dir.string();
        pictures_ = ui::screen_pictures(platform::ui_packs_dir() / ui::kPictureFolder, server_dir, // files added since
                                        platform::pictures_dir());
        picture_.clear();
        if (!pictures_.empty())
            picture_ = pictures_[std::uniform_int_distribution<std::size_t>(0, pictures_.size() - 1)(picture_rng_)];
    }
    loading_view_.picture = {};
    picture_behind_ = false;
    // Not in the game (but while the zone settles behind the loading overlay), nor over character select's scene.
    if ((screen_ == Screen::InGame && !zone_settling()) || pre_game_scene() || picture_.empty() || !ui_textures_)
        return;
    // The loading overlay draws over the zone settling behind it; every other screen has the picture beneath it,
    // drawn last in draw_ui (draw_screen_picture_behind).
    if (screen_ == Screen::InGame || (screen_ == Screen::Loading && loading_view_.waiting)) {
        if (const ui::UiTextures::Texture* t = ui_textures_->picture(picture_))
            loading_view_.picture = {t->id, t->width, t->height};
    } else {
        picture_behind_ = true;
    }
}

void ClientApp::draw_screen_picture_behind() {
    // After the screen's actions: Log in and another game folder read the server files again
    // (reload_server_files), which frees the UI art the picture's texture belongs to. A draw list given the old
    // texture before then would be rendered with it freed (a crash in the Vulkan driver).
    if (!picture_behind_ || !ui_textures_) return;
    if (const ui::UiTextures::Texture* t = ui_textures_->picture(picture_))
        ui::draw_screen_picture(ImGui::GetBackgroundDrawList(), {t->id, t->width, t->height});
}

void ClientApp::draw_ui(float fps) {
    fps_ = fps;
    update_screen_picture();
    switch (screen_) {
    case Screen::Login: {
        // The interface picked for the server on this screen is the one it shows in: the picked server's (or the one
        // chosen in its Interface list), switched to at once rather than at the login (its Lua state is not running
        // here: draw_ui comes before the UI's frame).
        if (!login_view_.transient && !login_view_.busy)
            if (const ui::UiPack* pack = ui::find_ui_pack(ui_packs_, login_view_.ui); pack && pack->id != ui_name_)
                use_ui_pack(pack->id);
        // The user interface's own login screen (app/lua_eq_pregame.cpp) stands in for the client's when its script
        // draws it; its buttons come back as the same actions.
        ui::LoginAction action = ui::LoginAction::None;
        if (eq_api_ && eq_api_->replaces("LoginScreen")) {
            if (eq_host_.login_action) action = static_cast<ui::LoginAction>(*eq_host_.login_action);
            eq_host_.login_action.reset();
        } else {
            action = ui::login_screen(login_view_);
        }
        switch (action) {
        case ui::LoginAction::Submit: start_login(); break;
        case ui::LoginAction::Quit: quit_ = true; break;
        case ui::LoginAction::None: break;
        default: login_action(action); break;
        }
        break;
    }
    case Screen::Servers: {
        // The user interface's stock server select (app/lua_eq_login.cpp) stands in for the client's own when its
        // script runs; its buttons come back as the same actions.
        ui::ServerSelectAction action = ui::ServerSelectAction::None;
        if (eq_api_ && eq_api_->replaces("serverselect")) {
            if (eq_host_.server_select_action) action = static_cast<ui::ServerSelectAction>(*eq_host_.server_select_action);
            eq_host_.server_select_action.reset();
        } else {
            action = ui::server_select_screen(server_view_);
        }
        switch (action) {
        case ui::ServerSelectAction::Play:
            if (login_ && server_view_.selected >= 0 && server_view_.selected < static_cast<int>(server_rows_.size())) {
                const auto row = static_cast<std::size_t>(server_view_.selected);
                // The world played last on this login server: the stock server select's quick connect.
                config_.set(world_key("last_world"), server_view_.servers[row].name);
                config_.save(config_path_);
                server_view_.busy = true;
                server_view_.status = {"Asking the server to let you in...", false};
                login_->request_play(servers_[server_rows_[row]].id);
            }
            break;
        case ui::ServerSelectAction::Favorite:
            toggle_favorite_world(server_view_.favorite_row);
            server_view_.favorite_row = -1;
            break;
        case ui::ServerSelectAction::Back: back_to_login("", false); break;
        default: break;
        }
        draw_kick_question();
        break;
    }
    case Screen::Characters: {
        screen_keys();
        if (screen_ != Screen::Characters) break;
        char_view_.scene = pre_game_scene();
        // The user interface's own character select (app/lua_eq_pregame.cpp) stands in for the client's when its
        // script draws it.
        auto action = ui::CharacterSelectAction::None;
        if (eq_api_ && eq_api_->replaces("CharacterListWnd")) {
            if (eq_host_.char_select_action)
                action = static_cast<ui::CharacterSelectAction>(*eq_host_.char_select_action);
            eq_host_.char_select_action.reset();
        } else {
            action = ui::character_select_screen(char_view_);
        }
        // HAILSTONE_CHAR_CREATE=1 (testing): the character creation once the creation tables are in.
        static bool test_create = hailstone::core::env("CHAR_CREATE") != nullptr;
        if (test_create && conn_->char_create_data()) {
            test_create = false;
            action = ui::CharacterSelectAction::Create;
        }
        auto selected = [&]() -> std::string {
            return char_view_.selected >= 0 && char_view_.selected < static_cast<int>(char_view_.characters.size())
                       ? char_view_.characters[static_cast<std::size_t>(char_view_.selected)].name
                       : std::string();
        };
        switch (action) {
        case ui::CharacterSelectAction::EnterWorld:
            char_view_.status = {};
            note_entered_character(selected());
            conn_->enter_world(selected());
            break;
        case ui::CharacterSelectAction::ReturnHome:
            // only for a character the list lets go home: world drops the connection otherwise
            if (char_view_.selected >= 0 && char_view_.selected < static_cast<int>(char_view_.characters.size()) &&
                char_view_.characters[static_cast<std::size_t>(char_view_.selected)].can_go_home) {
                char_view_.status = {};
                note_entered_character(selected());
                conn_->enter_world(selected(), true);
            }
            break;
        case ui::CharacterSelectAction::Delete:
            conn_->delete_character(selected());
            char_view_.busy = true;
            break;
        case ui::CharacterSelectAction::Create:
            create_view_ = {};
            create_choice_ = {}; // the model is dressed for creation again
            char_create_look_ = {};
            // The combinations the account's expansions allow (0x7f2d00, as the stock page's): the race, class,
            // deity and start zone lists all come from these.
            if (const auto& data = conn_->char_create_data()) {
                for (const auto& c : data->combos) {
                    if (!net::char_create_allowed(c, conn_->world_expansions()) || !game::is_playable_race(c.race)) continue;
                    ui::CreateOption o{c.race, c.class_, c.deity, c.zone, {}, {}};
                    for (const auto& a : data->allocations)
                        if (a.index == c.allocation_index) o.base = a.base, o.points = a.points;
                    create_view_.options.push_back(o);
                }
            }
            create_view_.deity_name = [this](std::uint32_t d) { return conn_ ? deity_desc(*conn_, d) : std::string(); };
            if (create_view_.options.empty())
                create_view_.status = {"The server has not sent its character creation tables.", true};
            screen_ = Screen::Create;
            break;
        case ui::CharacterSelectAction::Back:
        case ui::CharacterSelectAction::Logout: back_to_login("", false); break;
        default: break;
        }
        break;
    }
    case Screen::Create: {
        screen_keys();
        if (screen_ != Screen::Create) break;
        // The user interface's stock character creation (app/lua_eq_login.cpp) stands in for the client's own when its
        // script runs: it sends the name for approval itself (EqHost::char_create_submit) or goes back.
        if (eq_api_ && eq_api_->replaces("CharacterCreation")) break;
        // OP_RandomNameGenerator's answer goes in the name.
        if (auto random = conn_->take_random_name()) {
            create_view_.name = *random;
            create_view_.name_pending = false;
        }
        create_view_.scene = pre_game_scene();
        ui::create_normalize(create_view_);
        // Hidden while the customization window shows on the model (CC_Appearance_Button's Show(0, 1, 1)).
        if (eq_host_.customization_shown) break;
        // The user interface's own creation window (app/lua_eq_pregame.cpp) stands in for the client's screen when
        // its script draws it; its buttons come back as the screen's actions.
        auto create_action = ui::CharacterCreateAction::None;
        if (eq_api_ && eq_api_->replaces("CharacterCreateScreen")) {
            if (eq_host_.char_create_action)
                create_action = static_cast<ui::CharacterCreateAction>(*eq_host_.char_create_action);
            eq_host_.char_create_action.reset();
        } else {
            create_action = ui::character_create_screen(create_view_);
        }
        switch (create_action) {
        case ui::CharacterCreateAction::Create: {
            auto& v = create_view_;
            if (!ui::create_ready(v)) break;
            // Names are capitalised: first letter upper case, the rest lower case.
            std::string name;
            for (char ch : v.name)
                if (std::isalpha(static_cast<unsigned char>(ch))) name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            v.name = name;
            const auto& o = v.options[static_cast<std::size_t>(v.option)];
            const auto& data = *conn_->char_create_data();
            for (const auto& c : data.combos) {
                if (c.race == o.race && c.class_ == o.class_id && c.deity == o.deity && c.zone == o.start_zone) {
                    if (auto cc = net::default_char_create(data, c, v.gender)) {
                        // the stats as the player spent them (STR, DEX, AGI, STA, INT, WIS, CHA)
                        auto stat = [&](std::size_t i) { return static_cast<std::uint32_t>(v.stats[i]); };
                        cc->str = stat(0), cc->dex = stat(1), cc->agi = stat(2), cc->sta = stat(3);
                        cc->int_ = stat(4), cc->wis = stat(5), cc->cha = stat(6);
                        // Send_OP_CharacterCreate: the face, hair, beard, eyes, heritage, tattoo and details are the
                        // model player's.
                        if (const game::Spawn* m = model_player()) {
                            const game::Appearance& a = m->appearance;
                            cc->face = a.face, cc->hair_style = a.hair_style, cc->hair_color = a.hair_color;
                            cc->beard = a.beard, cc->beard_color = a.beard_color;
                            cc->eye_color1 = a.eye_color1, cc->eye_color2 = a.eye_color2;
                            cc->drakkin_heritage = a.drakkin_heritage, cc->drakkin_tattoo = a.drakkin_tattoo;
                            cc->drakkin_details = a.drakkin_details;
                        }
                        conn_->create_character(name, *cc);
                        creating_ = true;
                        v.busy = true;
                        v.status = {"Creating " + name + "...", false};
                    }
                    break;
                }
            }
            break;
        }
        case ui::CharacterCreateAction::Cancel: screen_ = Screen::Characters; break;
        case ui::CharacterCreateAction::Appearance:
            if (eq_api_) eq_api_->show("PlayerCustomizationWnd", true);
            break;
        default: break;
        }
        break;
    }
    case Screen::Loading:
        // The same screen as the zone settling behind it (ui::loading_overlay), so it goes on unchanged after the
        // zone-in; the disconnected screen (no waiting) stays a window.
        if (loading_view_.waiting) ui::loading_overlay(loading_view_);
        else ui::loading_screen(loading_view_);
        break;
    case Screen::InGame: {
        const auto& world = conn_->world();
        auto& hud = game_view_.hud;
        hud.zone = world.zone_long_name.empty() ? world.zone_short_name : world.zone_long_name;
        glm::vec3 loc = game::world_to_server(feet_ + glm::vec3(0, 0, floor_offset_));
        hud.x = loc.x;
        hud.y = loc.y;
        hud.z = loc.z;
        hud.heading = game::yaw_to_heading(yaw_) * 360.0f / 512.0f;
        hud.fps = fps;
        hud.connected = conn_->state() == net::ConnectionState::InZone || conn_->state() == net::ConnectionState::Camping;
        hud.connection = conn_->state() == net::ConnectionState::Camping ? "Camping" : "Connected";
        // Only spawn_list_window reads it: not built (a name string per spawn) while that window is closed.
        game_view_.spawns.spawns.clear();
        if (game_view_.dev_windows && game_view_.show_spawns) {
            for (const auto& [id, s] : world.spawns) {
                if (id == world.player_id) continue;
                game_view_.spawns.spawns.push_back(
                    {id, game::display_name(s.name), s.level, glm::distance(s.position, camera_.position)});
            }
        }
        const auto& ps = conn_->player_status();
        auto& pv = game_view_.player;
        if (const auto* me = world.player()) pv.name = me->name;
        pv.level = ps.level;
        pv.hp = conn_->own_hp().cur; // the items' HP included (GameConnection::own_hp)
        pv.max_hp = conn_->own_hp().max;
        pv.mana = ps.mana;
        pv.max_mana = ps.max_mana;
        pv.endurance = ps.endurance;
        pv.max_endurance = ps.max_endurance;
        pv.exp = ps.exp;
        pv.sitting = conn_->sitting();
        pv.attacking = conn_->auto_attack();
        pv.dead = ps.dead;
        auto& tv = game_view_.target;
        tv = {};
        if (auto it = world.spawns.find(world.target_id); it != world.spawns.end()) {
            const auto& t = it->second;
            tv.has_target = true;
            tv.name = game::spawn_label(t);
            tv.level = t.level;
            tv.hp_percent = t.hp_percent;
            tv.con_rgb = game::con_rgb(game::con_level(t, ps.level, conn_->pvp_rules()));
            tv.is_corpse = t.is_corpse;
            tv.is_self = it->first == world.player_id;
            if (tv.is_self) tv.con_rgb = 0xffffff;
            tv.distance = glm::distance(t.position, feet_ + glm::vec3(0, 0, floor_offset_));
        }
        game_view_.respawn = {};
        if (!(eq_api_ && eq_api_->replaces("RespawnWnd")))
            if (const auto& rw = conn_->respawn_window()) {
                for (const auto& o : rw->options)
                    if (conn_->respawn_option_available(o.id)) game_view_.respawn.options.emplace_back(o.id, o.name);
                game_view_.respawn.seconds_left = static_cast<std::uint32_t>((conn_->respawn_ms_left() + 999) / 1000);
                game_view_.respawn.ready = conn_->respawn_ready();
            }
        game_view_.spawns.selected_id = world.target_id;
        auto actions = ui::in_game_overlay(game_view_);
        draw_items();
        draw_world_container();
        if (stats_ui_) stats_ui_->draw(*conn_);
        progress_ui_.draw(*conn_);
        market_ui_.draw(*conn_, game_view_.dev_windows);
        if (actions_ui_ && player_ready_) {
            const game::mechanics::Character* ch = stats_ui_ ? stats_ui_->character() : nullptr;
            AbilityView av = ability_view();
            actions_ui_->update(*conn_, ch, av);
            actions_ui_->draw(*conn_, ch, av);
            for (auto& [text, color] : actions_ui_->take_messages())
                game_view_.chat.add(color == 0xd ? ui::ChatChannel::Error : ui::ChatChannel::System, text, 0, color);
            if (actions_ui_->take_attack_toggle()) {
                if (conn_->toggle_auto_attack()) face_target();
            }
        }
        group_ui_.draw(*conn_);
        conn_->set_target_of_target_open(eq_api_ && eq_api_->shown("TargetOfTargetWindow"));
        if (auto t = conn_->take_window_toggle()) {
            if (*t == "spellbook") {
                // /book: the book follows the command's open flag through CSpellBookWnd's rules.
                if (conn_->spellbook_open()) {
                    if (!conn_->spell_book_open()) conn_->open_spell_book();
                } else if (conn_->spell_book_open()) {
                    conn_->close_spell_book();
                }
            } else if (t->rfind("tip:", 0) == 0) {
                // CTipWnd::Activate(tip, 0) on the context tip window: the duel tip (0x1a), the death tip (0x1f).
                int tip = *t == "tip:duel" ? 0x1a : std::atoi(t->c_str() + 4);
                if (eq_api_) eq_api_->activate_tip(tip);
            } else if (*t == "raid") {
                // /raidwindow: the raid key's toggle, the UI pack's RaidWindow when it draws one.
                if (eq_api_ && eq_api_->replaces("RaidWindow")) eq_api_->toggle("RaidWindow");
                else social_ui_.toggle_raid();
            }
            else if (t->rfind("targetoftarget:", 0) == 0) {
                // Leadership_ActivateAbility 0xe / 0x1d: CTargetOfTargetWnd's Show.
                if (eq_api_) eq_api_->show("TargetOfTargetWindow", t->back() == '1');
            } else if (t->rfind("show:", 0) == 0) {
                // A window the game shows itself (OP_TaskDescription's open flag: CTaskWnd's Show(1, 1, 1)).
                if (eq_api_) eq_api_->show(t->substr(5), true);
            }
        }
        conn_->set_spellbook_open(conn_->spell_book_open()); // /book's flag follows the book (it closes itself)
        for (int tip : conn_->take_context_tips()) // the connection's events' context tips
            if (eq_api_) eq_api_->activate_tip(tip);
        for (const std::string& url : conn_->take_web_links()) open_server_link(url);
        social_ui_.draw(*conn_);
        if (spell_ui_) {
            spell_ui_->draw(*conn_, stats_ui_ ? stats_ui_->character() : nullptr);
            if (opts_.auto_cast != 0 && player_ready_) {
                // Testing: 6 s after zoning in cast gem N (or, for -1, open the
                // book); with --screenshot and no --frames, save the screen
                // 0.8 s later and quit.
                auto now = Clock::now();
                if (in_zone_since_ == Clock::time_point{}) in_zone_since_ = now;
                bool scribed = true;
                if (opts_.auto_scribe && now - in_zone_since_ > std::chrono::seconds(3)) {
                    auto_scribe(now);
                    scribed = scribe_step_ == 4 && now - scribe_at_ > std::chrono::seconds(2);
                }
                if (now - in_zone_since_ > std::chrono::seconds(6) && !auto_cast_done_ && scribed) {
                    if (opts_.auto_cast > 0) {
                        if (conn_->world().target_id == 0) conn_->set_target(conn_->world().player_id);
                        conn_->cast_gem(opts_.auto_cast - 1);
                    }
                    else spell_ui_->toggle_book();
                    auto_cast_done_ = true;
                }
                if (auto_cast_done_ && !opts_.screenshot.empty() && opts_.frames == 0) {
                    if (now - in_zone_since_ > std::chrono::milliseconds(6800) && !auto_shot_) {
                        vk_.request_screenshot(opts_.screenshot);
                        auto_shot_ = true;
                    }
                    if (now - in_zone_since_ > std::chrono::milliseconds(7600)) quit_ = true;
                }
            }
        }
        if (actions.chat_submit) conn_->send_command(*actions.chat_submit);
        if (actions.target_spawn) conn_->set_target(*actions.target_spawn);
        if (actions.clear_target) conn_->set_target(0);
        if (actions.consider) conn_->consider();
        if (actions.toggle_sit) conn_->sit_toggle();
        if (actions.respawn) conn_->choose_respawn(*actions.respawn);
        if (actions.toggle_attack) {
            if (conn_->toggle_auto_attack()) face_target();
        }
        if (actions.camp) conn_->camp();
        if (actions.disconnect) back_to_login("Disconnected.", false);
        break;
    }
    }
    draw_screen_picture_behind();
}

} // namespace hailstone::app::client_internal
