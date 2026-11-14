// ClientApp: Login, server and character select, and the connection each frame.

#include <cstdlib>

#include "app/client_app_internal.h"
#include "app/benchmark_world.h"
#include "app/lua_eq.h"
#include "core/asset_folder.h"
#include "net/server_files.h"
#include "ui/ui_ini.h"
#include "platform/open_folder.h"
#include "plugin/policy.h"
#include "plugin/ui_bridge.h"
#include "core/env.h"
#include "core/wipe.h"

namespace hailstone::app::client_internal {

namespace {

// True when the folder holds a file at any depth (an overlay that changes nothing has none).
bool has_files(const std::optional<std::filesystem::path>& dir) {
    if (!dir) return false;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(*dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code type_ec;
        if (it->is_regular_file(type_ec)) return true;
    }
    return false;
}

} // namespace

void ClientApp::start_login() {
    login_view_.status = {"Connecting...", false};
    ServerProfile profile = ui::profile_from_view(login_view_);
    // Test auto-logins (--user) must not overwrite the player's saved servers, nor a box set's other windows
    // (--account) the one that started them.
    if (opts_.auto_user.empty() && opts_.account.empty() && !login_view_.transient) {
        auto saved = ui::store_login_settings(login_view_, config_);
        if (!saved) return; // the status says why
        config_.save(config_path_);
        profile = std::move(*saved);
    }
    login_view_.busy = true;
    apply_server_profile(profile);
    use_ui_pack(profile.ui);
    login_ = std::make_unique<net::LoginClient>();
    login_->start(profile.host, static_cast<std::uint16_t>(profile.port), login_view_.username,
                  login_view_.password);
    auto_play_sent_ = false;
}

void ClientApp::start_account_login() {
    if (ui::selected_profile_id(login_view_).empty()) {
        login_view_.status = {"--account " + opts_.account + ": no saved server to log in to (--server names one).",
                              true};
        return;
    }
    ui::pick_account(login_view_, opts_.account);
    if (login_view_.password.empty()) {
        login_view_.status = {"No saved password for " + opts_.account + " on this server: type it to log in.", true};
        return;
    }
    box_play_ = true;
    for (const auto& a : login_view_.accounts)
        if (a.username == login_view_.username) {
            box_character_ = a.character;
            box_world_ = a.world;
        }
    if (!opts_.auto_character.empty()) box_character_ = opts_.auto_character;
    start_login();
}

void ClientApp::start_box_login() {
    auto saved = ui::store_login_settings(login_view_, config_);
    if (!saved) return; // the status says why
    config_.save(config_path_);
    std::vector<SavedAccount> set = ui::box_accounts(login_view_);
    if (set.empty()) return;
    // Every password first: a set that cannot all log in starts none.
    std::vector<std::string> passwords;
    for (const auto& a : set) {
        auto pw = login_view_.credentials ? login_view_.credentials->load(ui::credential_server(login_view_), a.username)
                                          : std::nullopt;
        if (!pw || pw->empty()) {
            login_view_.status = {"No saved password for " + a.username + ": log in with it once with Save password "
                                  "ticked.",
                                  true};
            return;
        }
        passwords.push_back(std::move(*pw));
    }
    // The others, each its own Hailstone on the same server and game files.
#if defined(_WIN32)
    const std::filesystem::path exe = platform::install_dir() / "hailstone.exe";
#else
    const std::filesystem::path exe = platform::install_dir() / "hailstone";
#endif
    for (std::size_t i = 1; i < set.size(); ++i) {
        std::vector<std::string> args{exe.string(), "--server", saved->id, "--account", set[i].username};
        if (!set[i].character.empty()) args.insert(args.end(), {"--character", set[i].character});
        if (!opts_.game_dir.empty()) args.insert(args.end(), {"--game-dir", opts_.game_dir});
        if (!opts_.classic_dir.empty()) args.insert(args.end(), {"--classic-dir", opts_.classic_dir});
        std::string error;
        if (box_clients_.start(args, error)) log::info("box: started a window for {}", set[i].username);
        else log::warn("box: could not start a window for {}: {}", set[i].username, error);
    }
    login_view_.username = set.front().username;
    login_view_.password = passwords.front();
    for (auto& pw : passwords) core::wipe(pw);
    login_view_.save_password = set.front().save_password;
    box_play_ = true;
    box_character_ = set.front().character;
    box_world_ = set.front().world;
    start_login();
}

void ClientApp::play_character() {
    auto quick = ui::quick_characters(login_view_);
    if (login_view_.quick_selected < 0 || login_view_.quick_selected >= static_cast<int>(quick.size())) return;
    const ui::QuickCharacter q = quick[static_cast<std::size_t>(login_view_.quick_selected)];
    // The account's saved password, unless its password was just typed in the fields.
    if (login_view_.username != q.username || login_view_.password.empty()) ui::pick_account(login_view_, q.username);
    if (login_view_.password.empty()) {
        login_view_.status = {"Type the password for " + q.username + ", then Play.", true};
        return;
    }
    box_play_ = true;
    box_world_ = q.character.world;
    box_character_ = q.character.name;
    start_login();
}

void ClientApp::reload_saved_accounts() {
    auto fresh = load_profile(server_profile_.id, login_view_.servers_dir);
    if (!fresh) return;
    for (auto& p : login_view_.profiles)
        if (p.id == fresh->id) p.accounts = fresh->accounts;
    if (ui::selected_profile_id(login_view_) == fresh->id) login_view_.accounts = fresh->accounts;
}

void ClientApp::note_listed_characters() {
    if (server_profile_.id.empty() || !opts_.auto_user.empty() || login_view_.username.empty()) return;
    std::vector<SavedCharacter> list;
    std::string signature = char_view_.server_name;
    for (const auto& c : char_view_.characters) {
        list.push_back({c.name, char_view_.server_name, c.level, game::class_list_name(c.classes, true)});
        signature += "|" + c.name + ":" + std::to_string(c.level) + ":" + list.back().classes;
    }
    if (signature == listed_characters_) return;
    listed_characters_ = std::move(signature);
    if (note_account_characters(server_profile_.id, login_view_.username, char_view_.server_name, list,
                                login_view_.servers_dir))
        reload_saved_accounts();
}

void ClientApp::note_entered_character(const std::string& name) {
    // A test login's server is not saved; the account is the one this login used.
    if (server_profile_.id.empty() || !opts_.auto_user.empty() || login_view_.username.empty()) return;
    if (note_account_character(server_profile_.id, login_view_.username, char_view_.server_name, name,
                               login_view_.servers_dir))
        reload_saved_accounts();
}

void ClientApp::login_action(ui::LoginAction action) {
    switch (action) {
    case ui::LoginAction::LoginBox:
        start_box_login();
        break;
    case ui::LoginAction::PlayCharacter:
        play_character();
        break;
    case ui::LoginAction::MusicChanged:
        // Only the pre-game themes (the login one here; character select's from its next start).
        pregame_music_ = login_view_.pregame_music;
        config_.set_bool("audio.pregame_music", pregame_music_);
        config_.save(config_path_);
        if (audio_) audio_->music_selection(audio::kMusicLogin, pregame_music_);
        break;
    case ui::LoginAction::AccountsChanged:
        // A box tick or character: saved at once, quietly (the typed fields stay).
        if (ui::store_login_settings(login_view_, config_)) config_.save(config_path_);
        break;
    case ui::LoginAction::ForgetAccount:
        if (ui::forget_account(login_view_, config_, login_view_.account_choice)) {
            config_.save(config_path_);
            login_view_.status = {"Forgot " + login_view_.account_choice + ".", false};
        }
        break;
    case ui::LoginAction::Save:
        if (auto saved = ui::store_login_settings(login_view_, config_)) {
            config_.save(config_path_);
            login_view_.status = {"Saved " + saved->name + ".", false};
        }
        break;
    case ui::LoginAction::Delete:
        if (ui::delete_login_profile(login_view_, config_)) {
            config_.save(config_path_);
            login_view_.status = {"Server deleted.", false};
        }
        break;
    case ui::LoginAction::UseGameFolder: {
        // Only while nothing holds the old folder's files (the login screen is idle, as for the picker).
        if (login_ || conn_ || asset_source_ == AssetFolderSource::Override) break;
        std::filesystem::path folder(login_view_.game_choice);
        AssetFolderCheck check = validate_asset_folder(folder);
        if (!check.ok) {
            login_view_.status = {"That folder cannot be used: " + describe(check), true};
            refresh_game_folders();
        } else if (std::string why = data_folder_conflict(folder, platform::data_dir()); !why.empty()) {
            login_view_.status = {why, true};
        } else if (switch_asset_folder(folder)) {
            login_view_.status = {std::string("Playing with the ") + (check.live ? "live" : "RoF2") + " files in " +
                                      folder.string() + ".",
                                  false};
        } else {
            login_view_.status = {"Could not switch to " + folder.string() + ".", true};
        }
        break;
    }
    case ui::LoginAction::BrowseGameFolder:
        // The picker, from the next frame (change_asset_folder_at_login), as the Options window's button asks it.
        request_asset_folder_pick(config_, true);
        config_.save(config_path_);
        break;
    case ui::LoginAction::OpenAssets:
        if (auto dir = ui::selected_assets_dir(login_view_); !dir.empty() && !platform::open_folder(dir))
            login_view_.status = {"Could not open " + dir.string(), true};
        break;
    case ui::LoginAction::UpdateDownload:
    case ui::LoginAction::UpdateRestart:
    case ui::LoginAction::UpdateSkip:
    case ui::LoginAction::UpdateNotes:
        update_login_action(action);
        break;
    case ui::LoginAction::OpenProject:
        if (!login_view_.project_page.empty() && !platform::open_url(login_view_.project_page))
            login_view_.status = {"Could not open " + login_view_.project_page, true};
        break;
    case ui::LoginAction::AddonInspect:
    case ui::LoginAction::AddonInstall:
    case ui::LoginAction::AddonCancel:
    case ui::LoginAction::AddonUpdate:
    case ui::LoginAction::AddonRemove:
    case ui::LoginAction::AddonAuto:
    case ui::LoginAction::AddonFolder:
    case ui::LoginAction::AddonPage:
    case ui::LoginAction::AddonCheck:
    case ui::LoginAction::AddonRevoke:
    case ui::LoginAction::AddonAskAgain:
    case ui::LoginAction::AddonSuggestLook:
    case ui::LoginAction::AddonSuggestLater:
    case ui::LoginAction::AddonSuggestNever:
    case ui::LoginAction::AddonBrowse:
    case ui::LoginAction::AddonStoreLook:
        addons_login_action(action);
        break;
    default:
        break;
    }
}

void ClientApp::apply_server_profile(const ServerProfile& profile) {
    server_profile_ = profile;
    // The server's assets folder over the asset folder (GameDir's overlay); a test login's has none.
    std::optional<std::filesystem::path> overlay;
    if (!profile.id.empty()) overlay = profile_assets_dir(profile.id, login_view_.servers_dir);
    // What was read last came from an overlay with files (even if they are gone since), or this one has files.
    const bool with_files = has_files(overlay);
    const bool changed = overlay_loaded_ || with_files;
    if (overlay) apply_asset_rules(overlay->parent_path());
    else apply_asset_rules({});
    game_.set_overlay(overlay);
    if (overlay) log::info("server {}: assets {}", profile.id, overlay->string());
    if (auto missing = missing_server_files(game_); !missing.empty()) {
        std::string list;
        for (const auto& f : missing) list += (list.empty() ? "" : ", ") + f;
        log::warn("this server's assets folder lacks {}: the live game files' own are used, which do not quite match "
                  "a RoF2 server (spells, messages, skill caps)",
                  list);
    }
    if (changed) reload_server_files();
    overlay_loaded_ = with_files;
}

void ClientApp::use_ui_pack(std::string_view id) {
    const ui::UiPack* pack = ui::find_ui_pack(ui_packs_, id.empty() ? config_.get_string("ui.name", "modern") : id);
    if (!pack || pack->id == ui_name_) return;
    ui_name_ = pack->id;
    lua_ui_.reset(); // its settings saved; the eq.* functions keep nothing of a Lua state
    lua_ui_ = std::make_unique<ui::lua::LuaUi>(pack->dir, platform::ui_settings_file(ui_name_));
    lua_ui_->set_character_settings(CharacterFiles{ui_character_dir_}.ui_settings(ui_name_));
    lua_ui_->set_show_control(game_view_.dev_windows);
    lua_ui_->add_eq_api([this](lua_State* L) { register_eq_api(L); });
    lua_ui_->set_caller_scope(lua_caller_scope());
    lua_ui_->set_clipboard_check(lua_clipboard_check());
    ui_textures_->set_all_art(pack->game_art);
    if (eq_api_) eq_api_->reload_game_files(); // (the EQ menu's rows follow the pack's windows: read again)
    log::info("user interface: {}", ui_name_);
}

ui::lua::LuaUi::CallerScope ClientApp::lua_caller_scope() {
    return [this](std::string_view tag) -> std::shared_ptr<void> {
        const std::uint32_t caps = automation_ ? automation_->plugin_caps(tag) : 0u;
        return std::make_shared<plugin::CallerScope>(caps, tag);
    };
}

ui::lua::LuaUi::ClipboardCheck ClientApp::lua_clipboard_check() {
    return [this](std::string_view tag) { return automation_ && automation_->plugin_caps(tag) == plugin::kCapAll; };
}

void ClientApp::sync_plugin_windows() {
    plugin::UiBridge* bridge = automation_ ? &automation_->plugin_ui() : nullptr;
    eq_host_.plugin_ui = bridge;
    std::vector<ui::lua::LuaUi::Folder> folders;
    if (bridge)
        for (const auto& [name, folder] : bridge->folders()) folders.push_back({name, folder / "ui" / ui_name_});
    lua_ui_->set_folders(std::move(folders));
}

std::string ClientApp::wanted_ui_pack() const {
    if (!server_profile_.id.empty() && !server_profile_.ui.empty()) return server_profile_.ui;
    return config_.get_string("ui.name", ui_name_);
}

bool ClientApp::apply_asset_rules(const std::filesystem::path& profile_dir) {
    auto only = profile_dir.empty() ? std::nullopt : net::server_asset_only(profile_dir);
    if (only == asset_only_) return false;
    asset_only_ = std::move(only);
    if (asset_only_) {
        log::info("server assets: only the server's own files and textures in its assets folder are used (its {})",
                  plugin::kServerCfgName);
        game_.set_overlay_filter([only = *asset_only_](std::string_view name) { return net::asset_counts(only, name); });
    } else {
        game_.set_overlay_filter({});
    }
    return true;
}

void ClientApp::reload_server_files(bool at_login) {
    // What was read from the game files before this login and kept: by the constructor, or during an earlier login
    // (the character models, the world objects' archives, icons, the EQ menu). The connection reads its own files
    // (strings, spells, race data) when it is made, and each zone reads its own when it loads.
    log::info("server files changed: reading the login strings, stat tables, UI art, sounds, models and spell "
              "effects again");
    if (auto strings = game_.find("eqlsstr_us.txt")) net::load_login_strings(*strings);
    bool stats_open = stats_ui_ && stats_ui_->is_open();
    stats_ui_ = std::make_unique<StatsUi>(game_);
    stats_ui_->set_open(stats_open);
    // Called from the login screen's actions in draw_ui, before the screen picture is drawn beneath it
    // (draw_screen_picture_behind) and before the Lua UI draws this frame, or at a zone-in before draw_ui. The old
    // art still lives until this frame is submitted, so a caller that ran after something was drawn with it (none
    // does now) cannot free a texture a draw list names.
    bool all_art = ui_textures_ ? ui_textures_->all_art() : true;
    eq_host_.textures = nullptr;
    retired_ui_textures_ = std::move(ui_textures_);
    ui_textures_ = std::make_unique<ui::UiTextures>(vk_, game_);
    ui_textures_->set_all_art(all_art);
    eq_host_.textures = ui_textures_.get();
    if (audio_) {
        // The sound archives (snd*.pfs), soundassets.txt, mp3index.txt and racedata.txt are listed and read when the
        // audio system is made: a new one, with the settings the Options window left.
        audio::AudioSettings settings = audio_->settings();
        sound_events_.reset();
        audio_.reset();
        audio_ = std::make_unique<audio::AudioSystem>(game_, std::move(settings), !opts_.no_audio);
        sound_events_ = std::make_unique<SoundEvents>(*audio_, game_);
        if (at_login) audio_->music_selection(audio::kMusicLogin, pregame_music_);
    }
    characters_.clear(); // global_chr.s3d and the other global archives, the EQG models, the appearance tables
    actor_kept_.reset(); // the meshes and textures read from them
    if (world_objects_) world_objects_->reset_game_files();
    if (eq_api_) eq_api_->reload_game_files();
    vk_.wait_idle();
    // Their icon sheets (spells<nn>.tga, dragitem<n>.dds); the old ones' destructors wait for the GPU.
    spell_ui_.reset();
    spell_ui_ = std::make_unique<SpellUi>(vk_, game_);
    item_ui_.reset();
    item_ui_ = std::make_unique<ItemUi>(vk_, game_);
    spell_particles_ = std::make_unique<SpellParticles>(vk_, game_, shader_dir_, [this] {
        return collision_ ? &*collision_ : nullptr;
    });
    spell_bolts_ = std::make_unique<SpellBolts>(vk_, shader_dir_);
    actor_emitters_ = std::make_unique<ActorEmitters>(vk_, game_, shader_dir_);
}

void ClientApp::back_to_login(std::string message, bool is_error) {
    link_death_since_.reset();
    if (eq_api_) eq_api_->save_layout();
    // Hailstone: the chat's lines to the character's history (a link death, a camp to the server list, a lost
    // connection); entering the world again puts them back.
    save_chat_history(true);
    game_view_.chat.lines.clear();
    ++game_view_.chat.revision;
    if (stats_ui_) stats_ui_->forget(); // its player reads the connection's spell table
    conn_.reset();
    eq_host_.conn = nullptr; // (update_eq_host sets it each frame; the windows must not reach the freed one before)
    login_.reset();
    unload_zone();
    leave_character();
    player_ready_ = false;
    screen_ = Screen::Login;
    if (audio_) {
        // DoMainLoop stops both themes; the login screen's is the one CDisplay_InitDisplay starts.
        for (bool* on : {&music_attack_, &music_bank_, &music_merchant_, &music_trainer_, &music_dead_}) *on = false;
        for (std::int32_t id : {audio::kMusicCombat, audio::kMusicBank, audio::kMusicMerchant, audio::kMusicTrainer,
                                audio::kMusicDeath, audio::kMusicCharacterSelect})
            audio_->music_selection(id, false);
        music_select_ = false;
        audio_->music_selection(audio::kMusicLogin, pregame_music_);
    }
    login_view_.busy = false;
    box_play_ = false;
    box_world_.clear();
    box_character_.clear();
    login_view_.quick_selected = std::max(login_view_.quick_selected, ui::default_quick_character(login_view_));
    // A saved password is in the fields again (the next login needs no typing).
    ui::pick_account(login_view_, login_view_.username);
    login_view_.status = {std::move(message), is_error};
    window_.set_mouse_captured(false);
}

void ClientApp::leave_character() {
    // The character's UI settings are saved and let go: the next login's character writes its own, never these.
    if (actions_ui_) actions_ui_->save_character(); // its hot buttons' last changes (ActionsUi::save_if_due)
    ui_character_dir_.clear();
    if (lua_ui_) lua_ui_->set_character_settings({});
    use_character_settings({}); // hailstone.ini's settings again
    eq_host_.drop_session_dialogs();
    pre_game_failed_ = false;
}

void ClientApp::update_login() {
    if (!login_) return;
    login_->update();
    switch (login_->state()) {
    case net::LoginState::LoggedIn:
        // The server took the password: now it is kept, when the account saves it (a box window's already is).
        if (opts_.auto_user.empty() && opts_.account.empty()) ui::save_login_password(login_view_);
        // And it leaves the fields: nothing needs it again (a saved one comes back from the keyring on the login
        // screen, return_to_login), and a crash dump or bug report from in game must not hold it.
        core::wipe(login_view_.password);
        login_view_.status = {"Getting the server list...", false};
        login_->request_server_list();
        break;
    case net::LoginState::ServerList: {
        // The login client asks for the list again every 60 s while it is shown (eqmain's MainLoop). The screen is
        // built again when a list arrives, not each frame; the add-on hint also every few seconds (it reads the
        // server's folder).
        const bool fresh = login_->servers_serial() != servers_listed_serial_;
        if (fresh) take_server_list();
        if (fresh || Clock::now() - server_hint_at_ > std::chrono::seconds(3)) {
            update_server_select_hint();
            server_hint_at_ = Clock::now();
        }
        server_view_.busy = false;
        // 6018 (failed attempts) after the login, or 6028 when Play picked a server for another locale.
        if (!login_->notice().empty()) server_view_.status = {login_->notice(), login_->error_id() != 0};
        screen_ = Screen::Servers;
        // A test login plays the first world at once (HAILSTONE_SERVER_SELECT=1 keeps the server select up: testing); a
        // box login the world played last on this server (the server select's quick connect), else the first.
        if ((!opts_.auto_user.empty() || box_play_) && !auto_play_sent_ && !servers_.empty() &&
            !hailstone::core::env("SERVER_SELECT")) {
            const net::ServerInfo* world = &servers_.front();
            if (const char* requested = core::env("BENCHMARK_WORLD")) {
                const auto pinned = benchmark_world_id(servers_, requested);
                if (!pinned) {
                    server_view_.status = {"The requested world is unavailable or ambiguous.", true};
                    break;
                }
                for (const auto& candidate : servers_)
                    if (candidate.id == *pinned) world = &candidate;
                log::info("benchmark: requested world selected");
            } else if (box_play_) {
                // The account's world (the character's), else the world played last on this server.
                std::string last = !box_world_.empty() ? box_world_
                                                       : config_.get_string("login.last_world." + server_profile_.id);
                for (const auto& s : servers_)
                    if (!last.empty() && (s.name == last || saved_character_key_part(s.name) == last)) world = &s;
            }
            auto_play_sent_ = true;
            server_view_.busy = true;
            login_->request_play(world->id);
        }
        break;
    }
    case net::LoginState::KickOffered:
        // ServerSelectWnd::OnJoinServer: 302 / 114, a character of this account is still in the world. The question
        // is drawn over the server select (ClientApp::draw_kick_question) until it is answered.
        server_view_.busy = true;
        server_view_.status = {"", false};
        break;
    case net::LoginState::Kicking:
        server_view_.busy = true;
        server_view_.status = {login_->notice(), false}; // 6075
        break;
    case net::LoginState::PlayAccepted: {
        auto ticket = *login_->ticket();
        box_play_ = false;
        login_->close();
        login_.reset();
        start_game(ticket);
        auto_enter_sent_ = false; // --character enters once a login; after a camp the player chooses
        break;
    }
    case net::LoginState::Failed: {
        std::string error = login_->error();
        login_.reset();
        back_to_login(error, true);
        break;
    }
    default:
        break;
    }
}

// The login server's list, as it arrived: the worlds noted for the login screen and the server select's rows.
void ClientApp::take_server_list() {
    servers_listed_serial_ = login_->servers_serial();
    servers_ = login_->servers();
    if (!server_profile_.id.empty() && opts_.auto_user.empty() && opts_.account.empty()) {
        // The worlds for the login screen's account World lists (written only when they changed).
        std::vector<std::string> names;
        for (const auto& s : servers_) names.push_back(s.name);
        if (names != worlds_noted_) {
            worlds_noted_ = names;
            set_listed_worlds(server_profile_.id, names, login_view_.servers_dir);
            if (ui::selected_profile_id(login_view_) == server_profile_.id) {
                login_view_.worlds = names;
                for (const auto& a : login_view_.accounts)
                    for (const auto& c : a.characters)
                        if (std::none_of(names.begin(), names.end(), [&](const std::string& n) {
                                return n == c.world || saved_character_key_part(n) == c.world;
                            })) {
                            login_view_.worlds.push_back(c.world);
                            names.push_back(c.world);
                        }
            }
        }
    }
    list_worlds();
}

std::string ClientApp::world_key(std::string_view what) const {
    return "login." + std::string(what) + "." + (server_profile_.id.empty() ? std::string("_test") : server_profile_.id);
}

void ClientApp::list_worlds() {
    const int sel = server_view_.selected;
    const std::string picked = sel >= 0 && sel < static_cast<int>(server_view_.servers.size())
                                   ? server_view_.servers[static_cast<std::size_t>(sel)].name
                                   : std::string();
    const std::vector<std::string> favorites = ui::parse_favorite_worlds(config_.get_string(world_key("favorite_worlds")));
    std::vector<std::string> names;
    names.reserve(servers_.size());
    for (const auto& s : servers_) names.push_back(s.name);
    server_rows_ = ui::server_list_order(names, favorites);
    server_view_.servers.clear();
    server_view_.selected = -1;
    for (std::size_t i : server_rows_) {
        const net::ServerInfo& s = servers_[i];
        const bool fav = std::find(favorites.begin(), favorites.end(), s.name) != favorites.end();
        // Every world can be played: the original asks the server whatever the status (LoginClient::request_play)
        server_view_.servers.push_back({s.name, s.status_text(), true, fav});
        if (!picked.empty() && s.name == picked && server_view_.selected < 0)
            server_view_.selected = static_cast<int>(server_view_.servers.size()) - 1;
    }
    if (server_view_.selected < 0 && !server_view_.servers.empty()) server_view_.selected = 0;
}

void ClientApp::toggle_favorite_world(int row) {
    if (row < 0 || row >= static_cast<int>(server_view_.servers.size())) return;
    const std::string& name = server_view_.servers[static_cast<std::size_t>(row)].name;
    std::vector<std::string> favorites = ui::parse_favorite_worlds(config_.get_string(world_key("favorite_worlds")));
    auto it = std::find(favorites.begin(), favorites.end(), name);
    if (it != favorites.end()) favorites.erase(it);
    else favorites.push_back(name);
    if (favorites.empty()) config_.erase(world_key("favorite_worlds"));
    else config_.set(world_key("favorite_worlds"), ui::join_favorite_worlds(favorites));
    config_.save(config_path_);
    list_worlds();
}

void ClientApp::draw_kick_question() {
    // The stock Yes / No box of ServerSelectWnd::OnJoinServer: the reply's eqlsstr text (302, or 114 for an offline
    // trader). Pre-game, so it is the client's own box whatever the user interface.
    if (!login_ || login_->state() != net::LoginState::KickOffered) return;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(520.0f, display.x - 32.0f), 0), ImGuiCond_Always);
    if (ImGui::Begin("Character still logged in###kick_question", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", net::login_error_text(login_->play_result().error_id).c_str());
        if (ImGui::Button("Yes")) login_->answer_kick(true);
        ImGui::SameLine();
        if (ImGui::Button("No")) {
            login_->answer_kick(false);
            server_view_.busy = false;
            server_view_.status = {"", false};
        }
    }
    ImGui::End();
}

void ClientApp::start_game(const net::WorldTicket& ticket) {
    net::ConnectionOptions copts;
    if (automation_) {
        copts.hooks = &automation_->hooks();
        copts.plugins = automation_->extensions(); // the plugins' server extensions (sdk/hailstone_plugin.h, ABI 2)
    }
    // The character's files: the saved profile's folder, or for a test login one made from the world's name.
    files_profile_ = server_profile_.id.empty()
                         ? transient_profile_id(ticket.server_name, server_profile_.host, server_profile_.port)
                         : server_profile_.id;
    copts.profile_id = files_profile_;
    copts.bank_snapshot_file = true; // the bank as last seen, in the character's bank.dat
    // A saved server's client files come at the first zone-in, into its assets/ (net/server_files.h).
    if (!server_profile_.id.empty()) {
        copts.server_files_dir = profile_assets_dir(server_profile_.id, login_view_.servers_dir).parent_path();
        // Bounds on what the server may make this write (net/server_files.h), in MB; 0 or less keeps the default.
        auto mb = [&](std::string_view key, std::uint64_t fallback) {
            const int v = config_.get_int(key, 0);
            return v > 0 ? static_cast<std::uint64_t>(v) << 20 : fallback;
        };
        copts.server_files_limits.max_file = mb("assets.server_files_max_file_mb", copts.server_files_limits.max_file);
        copts.server_files_limits.max_total =
            mb("assets.server_files_max_total_mb", copts.server_files_limits.max_total);
    }
    if (stats_ui_) stats_ui_->forget(); // its player reads the last connection's spell table
    conn_ = std::make_unique<net::GameConnection>(game_, copts);
    eq_host_.conn = conn_.get(); // the last connection is gone
    // The new connection's rules are its own: the last one's serial must not hide them, and character select's
    // scene is tried again.
    asset_rules_serial_ = 0;
    pre_game_failed_ = false;
    apply_connection_settings();
    conn_->set_client_commands([this](std::string_view h, const std::string& args) { return client_command(h, args); });
    // The UI ini's [ChatManager] (/autojoin and the chat server's auto join) and the chat windows' text (/report).
    conn_->set_ui_ini(
        [](std::string_view section, std::string_view key) -> std::optional<std::string> {
            ui::UiIni* ini = chat_ui_ini();
            return ini ? ini->get(section, key) : std::nullopt;
        },
        [](std::string_view section, std::string_view key, const std::string& value) {
            if (ui::UiIni* ini = chat_ui_ini()) ini->put(section, key, value);
        });
    conn_->set_visible_chat_text([] { return chat_visible_text(); });
    conn_->start(ticket);
    if (audio_) {
        // CEverQuest::StartNetworkGame: the login theme gives way to the character select one.
        audio_->music_selection(audio::kMusicLogin, false);
        audio_->music_selection(audio::kMusicCharacterSelect, pregame_music_);
        music_select_ = true;
    }
    char_view_.server_name = ticket.server_name;
    loading_view_ = {"Connecting", "Logging in to " + ticket.server_name, -1.0f};
    screen_ = Screen::Loading;
}

void ClientApp::camp_finished(std::uint32_t kind) {
    if (kind == 1) {
        // /camp desk: exit reason 0x1e5 with the quit flag at 0xdd2664 1, and WinMain leaves its front-end loop.
        log::info("camped out to the desktop: quitting");
        quit_ = true; // the frame goes on: the connection stays until the client closes
        return;
    }
    if (kind == 2) {
        // /camp server: 0x1e5 with the quit flag at 0xdd2664 0; WinMain runs the front end (eqmain's login) again.
        log::info("camped out to the server list: the login screen");
        back_to_login("You have camped out.", false);
        return;
    }
    // Exit reason 0x1e6: ProcessGame's clean-up (SaveGameUI, the groups, the world display, the local PC deleted;
    // 0x03a4 "Sending Logout." only on a live connection, and WaitForWorldDisconnect has closed it), the character
    // name cleared, then WorldAuthenticate (OP_SendLoginInfo, zoning 0) on the same credentials and
    // StartNetworkGame: character select. Here a new connection on the same ticket.
    log::info("camped out: back to character select");
    net::WorldTicket ticket = conn_->ticket();
    if (eq_api_) eq_api_->save_layout();
    if (stats_ui_) stats_ui_->forget(); // its player reads the connection's spell table
    conn_.reset();
    eq_host_.conn = nullptr; // (update_eq_host sets it each frame; the windows must not reach the freed one before)
    unload_zone();
    leave_character();
    player_ready_ = false;
    window_.set_mouse_captured(false);
    if (audio_) {
        // DoMainLoop's exit stops the in-game themes; StartNetworkGame starts entry 4 again (start_game).
        for (bool* on : {&music_attack_, &music_bank_, &music_merchant_, &music_trainer_, &music_dead_}) *on = false;
        for (std::int32_t id : {audio::kMusicCombat, audio::kMusicBank, audio::kMusicMerchant, audio::kMusicTrainer,
                                audio::kMusicDeath})
            audio_->music_selection(id, false);
    }
    // StartNetworkGame's ReloadUI -> InitCharSelectUI -> CleanGameUI destroys the game windows, the chat windows
    // with their text among them; entering the world makes them again (InitGameUI) from the character's ini.
    // Hailstone: the text is kept in the character's chat history first, and comes back on entering the world.
    save_chat_history(true);
    game_view_.chat.lines.clear();
    ++game_view_.chat.revision; // the Lua chat windows ask again (eq.ChatText's version)
    char_view_.status = {};
    char_view_.busy = false;
    start_game(ticket);
}

void ClientApp::window_close_request() {
    // HandleWindowCloseRequest: GameState 1 or 2 (character select, creation) sets the quit flag (0xdd2664 = 1)
    // for WinMain; any other state is Cmd_camp(pinstLocalPlayer, "desktop"), whose first four letters are eqstr
    // 13258's "desk". The login and server screens are eqmain.dll's front end. The loading screens and the
    // disconnected one run no window procedure in the original (their loops wait or Sleep), so the request waits in
    // the message queue until the next screen handles it: in game a /camp desktop, at character select a quit. A
    // second request while still loading quits at once (the original's window would only be killed as hung).
    if (screen_ == Screen::Loading) {
        if (close_pending_) {
            log::info("window close request again on a loading screen: quitting");
            quit_ = true;
            return;
        }
        log::info("window close request on a loading screen: held until the next screen");
        close_pending_ = true;
        return;
    }
    if (screen_ == Screen::InGame && conn_ && !link_death_since_) {
        log::info("window close request in game: /camp desktop");
        conn_->camp_command("desktop");
        return;
    }
    log::info("window close request: quitting");
    quit_ = true;
}

void ClientApp::update_link_death() {
    const net::GameConnection::LinkDeath death = *conn_->link_death();
    const auto now = Clock::now();
    if (!link_death_since_) {
        // HandleWorldDisconnect: "disconnected at %s:%u (char. select) %s" with the reasons, then
        // CEverQuest::DisplayScreen("Discon.bmp"): screen mode 2, the screen cleared and eqstr 12353 drawn in the
        // middle. The world is not drawn any more.
        log::warn("link death{}: {}", death.in_game ? " in game" : "", conn_->error());
        link_death_since_ = now;
        loading_view_ = {};
        loading_view_.title = conn_->strings().format(12353, {});
        if (loading_view_.title.starts_with('[')) loading_view_.title = "YOU HAVE BEEN DISCONNECTED.";
        loading_view_.waiting = false;
        screen_ = Screen::Loading;
        window_.set_mouse_captured(false);
    }
    // HandleWorldDisconnect's Sleep(3000) @0x51bf13; in game DoMainLoop then sets 0xddfd5b = 1 and Sleeps 16000 more
    // (@0x53de04) before its exit path. A drop within 15 s of /offlinemode (@0x53ddfc) ends the client after the
    // 3 s instead (0xddfd5b = 2, exit(0) @0x53f46a).
    const auto pause = std::chrono::milliseconds(death.in_game && !death.offline_mode ? 3000 + 16000 : 3000);
    if (now - *link_death_since_ < pause) return;
    link_death_since_.reset();
    if (death.offline_mode) {
        log::info("link death after /offlinemode: exiting");
        quit_ = true;
        return;
    }
    if (death.in_game) {
        // DoMainLoop's exit as after a camp: exit reason 0x1e6 to character select, or 0x1e5 when the quit flag at 0xdd2664 is 1
        // (the desktop) or 2 (the front end). OP_SaveOnZoneReq / OP_DeleteSpawn are not sent: the link is down
        // (0x542380), and WaitForWorldDisconnect returns at once.
        camp_finished(death.camp_kind);
    } else {
        // HandleWorldDisconnect(…, 1): GameState 0xff and exit reason 0x1e5, the front end.
        back_to_login(conn_->error(), true);
    }
}

void ClientApp::update_window_title() {
    std::string server;
    if (screen_ == Screen::Login && !login_) {
        // the server picked in the list (a new one, not saved yet: none)
        if (login_view_.selected >= 0 && login_view_.selected < static_cast<int>(login_view_.profiles.size()))
            server = login_view_.profiles[static_cast<std::size_t>(login_view_.selected)].name;
    } else {
        // this login's server; a test login's (no saved profile) by the world's name once the login server gave it
        server = server_profile_.id.empty() && !char_view_.server_name.empty() ? char_view_.server_name
                                                                                : server_profile_.name;
    }
    std::string character;
    if (conn_ && conn_->state() == net::ConnectionState::InZone && !conn_->character_name().empty()) {
        // Hailstone's multiclass support: a character with several classes shows them, "Name [War/Clr]" (the NMS
        // patch's title).
        character = conn_->character_name();
        if (std::uint32_t mask = conn_->server_class_mask()) character += " [" + game::class_mask_name(mask) + "]";
    }
    if (std::string title = client_window_title(server, character); title != window_title_) {
        window_title_ = title;
        window_.set_title(title);
        log::info("window title: {}", title);
    }
}

void ClientApp::update_connection() {
    if (!conn_) return;
    if (window_.input().user_activity) conn_->note_user_activity();
    conn_->update();
    // A new hailstone.cfg may change which of the server's assets folder counts ([assets] player).
    bool assets_ruled = false;
    if (conn_->server_rules_serial() != asset_rules_serial_) {
        asset_rules_serial_ = conn_->server_rules_serial();
        // (Not from the provisional rules before the zone-in: the server's files are checked then.)
        if (conn_->server_rules() && !conn_->server_rules()->provisional && !conn_->server_rules()->dir.empty())
            assets_ruled = apply_asset_rules(conn_->server_rules()->dir);
    }
    if (conn_->take_server_files_changed() || assets_ruled) {
        // The server's files came at this zone-in: read again what was read from the old ones, before the zone
        // loads.
        game_.rescan();
        conn_->reload_game_files();
        reload_server_files(false);
        overlay_loaded_ = true;
    }
    // /loadskin's user interface, loaded between frames (its Lua state may have run the command).
    if (pending_ui_pack_) use_ui_pack(*std::exchange(pending_ui_pack_, std::nullopt));
    // /quit and /exit: the connection closes and the client unloads.
    if (auto q = conn_->take_quit_request(); q != net::GameConnection::QuitRequest::None) {
        log::info("{}: quitting", q == net::GameConnection::QuitRequest::Exit ? "/exit" : "/quit");
        conn_->disconnect();
        quit_ = true;
        return;
    }
    using S = net::ConnectionState;
    switch (conn_->state()) {
    case S::CharacterSelect: {
        // The rows are built again when the list or a character's classes change, not each frame.
        if (conn_->characters_serial() != characters_listed_serial_) {
            characters_listed_serial_ = conn_->characters_serial();
            char_view_.characters.clear();
            for (const auto& c : conn_->characters()) {
                ui::CharacterEntry e;
                e.name = c.name;
                e.level = c.level;
                e.race = c.race;
                std::uint32_t mask = conn_->classes_mask(c.name);
                for (std::uint32_t bit = 0; bit < 16; ++bit)
                    if (mask & (1u << bit)) e.classes.push_back(bit + 1);
                if (e.classes.empty()) e.classes.push_back(c.class_);
                e.zone = game::zone_long_name(c.zone);
                if (e.zone.empty()) e.zone = "Zone " + std::to_string(c.zone);
                // CCharacterListWnd::EnableButtons @0x42f4b0: bCanGoHome and bUseable (taken here as the list's
                // enabled byte; docs/unknowns.md). Stock EQEmu sets go_home from World:EnableReturnHomeButton and the
                // time since the last login, and enabled always.
                e.can_go_home = c.go_home != 0 && c.enabled != 0;
                e.home_question = conn_->strings().format(6137, std::vector<std::string>{c.name});
                char_view_.characters.push_back(std::move(e));
            }
            note_listed_characters();
        }
        if (char_view_.selected >= static_cast<int>(char_view_.characters.size())) char_view_.selected = -1;
        if (char_view_.selected < 0 && !char_view_.characters.empty()) char_view_.selected = 0;
        char_view_.busy = false;
        if (creating_) {
            creating_ = false;
            create_view_.busy = false;
            if (!conn_->error().empty()) {
                create_view_.status = {conn_->error(), true};
            } else {
                for (int i = 0; i < static_cast<int>(char_view_.characters.size()); ++i)
                    if (char_view_.characters[static_cast<std::size_t>(i)].name == create_view_.name) char_view_.selected = i;
                char_view_.status = {create_view_.name + " was created.", false};
                screen_ = Screen::Characters;
            }
        } else if (screen_ != Screen::Create) {
            if (!conn_->error().empty() && char_view_.status.text.empty()) char_view_.status = {conn_->error(), true};
            screen_ = Screen::Characters;
        }
        if (!opts_.auto_character.empty() && opts_.account.empty() && !auto_enter_sent_) {
            auto_enter_sent_ = true;
            conn_->enter_world(opts_.auto_character);
        } else if (!box_character_.empty() && !auto_enter_sent_) {
            // A box login's character, when the account has it (else character select, as the player chooses).
            auto_enter_sent_ = true;
            std::string name = std::exchange(box_character_, {});
            bool here = false;
            for (const auto& c : conn_->characters()) here |= c.name == name;
            if (here) {
                note_entered_character(name);
                conn_->enter_world(name);
            } else {
                char_view_.status = {name + " is not on this account: choose a character.", true};
            }
        }
        break;
    }
    case S::Creating:
        break;
    case S::EnteringWorld:
    case S::ZoneConnecting:
    case S::ZoneEntering:
        // Hailstone: the character's own settings from here (before its first zone loads), until it leaves.
        if (!conn_->character_name().empty())
            use_character_settings(character_files(files_profile(), char_view_.server_name, conn_->character_name()).dir);
        if (music_select_ && audio_) {
            // CEverQuest::DoCharacterSelection: the character enters the world.
            audio_->music_selection(audio::kMusicCharacterSelect, false);
            music_select_ = false;
        }
        [[fallthrough]];
    case S::WorldConnecting:
    case S::WorldAuth:
    case S::Zoning:
        if (screen_ == Screen::InGame && conn_->state() == S::Zoning) {
            unload_zone();
            player_ready_ = false;
        }
        if (screen_ != Screen::Loading) {
            loading_since_ = Clock::now();
            zone_loaded_ms_ = zone_in_ms_ = -1;
        }
        loading_view_.title = conn_->state() == S::Zoning ? "Zoning" : "Loading";
        loading_view_.detail = conn_->status();
        if (const auto* files = conn_->server_files_download()) {
            // Said plainly, so a long first zone-in on a server is not taken for a hang.
            loading_view_.title = "Downloading server files";
            loading_view_.detail = files->status();
            loading_view_.progress = files->progress();
            screen_ = Screen::Loading;
            break;
        }
        // The bar: the zone-in's steps up to the zone loaded (half), the rest is the settle's (begin_zone_settle).
        switch (conn_->state()) {
        case S::WorldConnecting: loading_view_.progress = 0.02f; break;
        case S::WorldAuth: loading_view_.progress = 0.04f; break;
        case S::EnteringWorld:
        case S::Zoning: loading_view_.progress = 0.06f; break;
        case S::ZoneConnecting: loading_view_.progress = 0.1f; break;
        default: loading_view_.progress = zone_loaded_ms_ >= 0 ? 0.5f : 0.2f; break;
        }
        screen_ = Screen::Loading;
        break;
    case S::InZone:
    case S::Camping:
        if (!player_ready_) {
            enter_zone();
            if (player_ready_) begin_zone_settle();
        }
        screen_ = Screen::InGame;
        break;
    case S::Disconnected: {
        if (auto kind = conn_->camped_out()) {
            camp_finished(*kind);
            return;
        }
        if (conn_->link_death()) {
            update_link_death();
            return;
        }
        back_to_login(conn_->error(), true);
        return;
    }
    case S::Idle:
        break;
    }
    if (auto z = conn_->take_zone_to_load()) load_zone(*z);
    while (auto c = conn_->poll_chat()) game_view_.chat.add(chat_channel(c->kind), c->text, c->rgb, c->color, c->peer);
    // Hailstone: the chat history every few minutes too (a crash keeps most of it).
    if (screen_ == Screen::InGame && Clock::now() - chat_saved_ > std::chrono::minutes(3)) {
        chat_saved_ = Clock::now();
        save_chat_history(false);
    }
}

} // namespace hailstone::app::client_internal
