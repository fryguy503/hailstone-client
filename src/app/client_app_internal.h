#pragma once

// The playable client's internals, shared by the client_app*.cpp files: the ClientApp class
// and its collision world. Only those files include it.

#include <format>

#include "app/updater.h"
#include "platform/process.h"
#include "core/step_clock.h"
#include "app/addon_manager.h"
#include "app/addon_suggest.h"
#include "app/window_icon.h"
#include "app/automation.h"
#include "app/progress_ui.h"
#include "app/market_ui.h"
#include "app/client_app.h"
#include "app/input_script.h"
#include "app/zone_settle.h"
#include "app/server_links.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <thread>
#include <unordered_set>
#include <utility>

#include <SDL3/SDL_filesystem.h>
#include <imgui.h>

#include "assets/character.h"
#include "assets/sky.h"
#include "assets/texture_overrides.h"
#include "assets/zone.h"
#include "core/character_files.h"
#include "core/config.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "core/wipe.h"
#include "app/nameplate_layer.h"
#include "game/nameplate_overlap.h"
#include "game/sky_colours.h"
#include "game/option_commands.h"
#include "ui/font_faces.h"
#include "game/boats.h"
#include "game/npc_collision.h"
#include "game/clicks.h"
#include "game/mouse_look.h"
#include "game/collision.h"
#include "game/zone_ladders.h"
#include "game/combat.h"
#include "game/picking.h"
#include "game/zoning.h"
#include "game/mechanics/character.h"
#include "game/chat_input.h"
#include "game/targeting.h"
#include "game/target_indicator.h"
#include "game/trade_rules.h"
#include "game/sky_clock.h"
#include "game/coords.h"
#include "game/distance.h"
#include "game/follow.h"
#include "game/race_models.h"
#include "game/char_create_look.h"
#include "game/cameras.h"
#include "game/iso_camera.h"
#include "game/iso_occlusion.h"
#include "game/char_select_camera.h"
#include "game/eq_names.h"
#include "game/mechanics/character.h"
#include "game/scaled_ambient.h"
#include "game/light_list.h"
#include "render/scene_lighting.h"
#include "game/scene_fog.h"
#include "game/scene_light.h"
#include "game/view_distance.h"
#include "game/zone_lines.h"
#include "game/zone_names.h"
#include "game/mechanics/touching_switch.h"
#include "net/game_connection.h"
#include "net/login.h"
#include "platform/child_processes.h"
#include "platform/credentials.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/ambient_probe.h"
#include "render/atmosphere.h"
#include "render/camera.h"
#include "render/cover_map.h"
#include "render/fire_renderer.h"
#include "render/flora_renderer.h"
#include "render/target_ring_renderer.h"
#include "render/point_shadow_pass.h"
#include "render/actor_renderer.h"
#include "render/sky_renderer.h"
#include "render/spawn_renderer.h"
#include "render/vk_context.h"
#include "render/weather_renderer.h"
#include "render/zone_renderer.h"
#include "app/movement_test.h"
#include "app/player_movement.h"
#include "app/spell_ui.h"
#include "app/item_ui.h"
#include "app/item_preview_view.h"
#include "app/perf_ui.h"
#include "app/stats_ui.h"
#include "app/actions_ui.h"
#include "app/neighbour_zones.h"
#include "app/world_objects.h"
#include "app/zone_move_world.h"
#include "app/group_ui.h"
#include "app/social_ui.h"
#include "app/sound_events.h"
#include "app/actor_emitters.h"
#include "app/spell_bolts.h"
#include "app/spell_particles.h"
#include "app/zone_particles.h"
#include "audio/audio_system.h"
#include "audio/listener.h"
#include "game/server_ext.h"
#include "game/tracking.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"
#include "ui/ui_packs.h"
#include "ui/ui_textures.h"
#include "app/lua_eq.h"
#include "app/gamepad.h"
#include "game/hotbuttons.h"
#include "game/option_commands.h"

#include <bit>
#include <lua.hpp>
#include "ui/imgui_layer.h"
#include "core/web_url.h"
#include "ui/web_textures.h"
#include "web/browser_host.h"
#include "app/web_service.h"
#include "app/guide_path.h"
#include "app/navigator.h"
#include "ui/asset_picker.h"
#include "core/asset_folder_choice.h"

namespace hailstone::app {

namespace client_internal {

using Clock = std::chrono::steady_clock;

// Movement itself is the original's (app/player_movement.h,
// game/mechanics/movement.h); app/zone_move_world.h is the collision probes around it.
constexpr float kNameplateRange = 250.0f;
constexpr float kMaxCameraDistance = 60.0f;
// Hailstone: the Graphics options' Max Camera Zoom (display.max_camera_distance) has the NMS /fov's range.
constexpr int kMinCameraZoomSetting = 10;
constexpr int kMaxCameraZoomSetting = 150;


// The game window as it was when the client last closed (hailstone.ini window.*), unless --window-size asked for
// a size: the UI's places are kept for that screen, so starting it smaller piles the windows up.
inline platform::WindowDesc window_desc(const ClientOptions& o) {
    platform::WindowDesc d{.width = o.window_width, .height = o.window_height};
    d.pad_input_in_background = !o.input_script.empty();
    if (o.window_width != 1280 || o.window_height != 720) return d; // --window-size
    Config c;
    if (!c.load(default_config_path())) return d;
    d.width = std::max(320, c.get_int("window.width", d.width));
    d.height = std::max(240, c.get_int("window.height", d.height));
    if (c.get("window.x") && c.get("window.y")) d.position = {{c.get_int("window.x", 0), c.get_int("window.y", 0)}};
    d.maximized = c.get_bool("window.maximized", false);
    return d;
}

// The UI scale: --ui-scale, else hailstone.ini ui.scale (percent; 0 or none: the display's own scale). The fonts
// are built for it at start, so a change in the Options window takes effect at the next start.
inline float ui_scale_setting(const ClientOptions& o) {
    if (o.ui_scale > 0.0f) return o.ui_scale;
    Config c;
    if (!c.load(default_config_path())) return 0.0f;
    int percent = c.get_int("ui.scale", 0);
    return percent > 0 ? static_cast<float>(std::clamp(percent, 50, 300)) / 100.0f : 0.0f;
}

inline ui::ChatChannel chat_channel(net::ChatEvent::Kind k) {
    using K = net::ChatEvent::Kind;
    using C = ui::ChatChannel;
    switch (k) {
    case K::Say: return C::Say;
    case K::Tell: return C::Tell;
    case K::Group: return C::Group;
    case K::Guild: return C::Guild;
    case K::Raid: return C::Raid;
    case K::Shout: return C::Shout;
    case K::Auction: return C::Auction;
    case K::OOC: return C::OOC;
    case K::Emote: return C::Emote;
    case K::Combat: return C::Combat;
    case K::Spell: return C::Spell;
    default: return C::System;
    }
}

enum class Screen { Login, Servers, Characters, Create, Loading, InGame };

class ClientApp {
public:
    // Sparse lifecycle snapshots, at debug level (`summary`: info, the one line per zone-in). Heap allocation bytes
    // and RSS overlap on shared-memory GPUs.
    void log_memory(std::string_view phase, bool summary = false) const {
        const log::Level level = summary ? log::Level::Info : log::Level::Debug;
        if (!log::enabled(level)) return;
        constexpr double mib = 1024.0 * 1024.0;
        auto line = [&](std::string text) {
            if (summary) log::info("{}", text);
            else log::debug("{}", text);
        };
        if (auto rss = platform::resident_memory())
            line(std::format("memory checkpoint: {}: resident {:.2f} MiB", phase, double(*rss) / mib));
        if (auto commit = platform::private_commit_memory())
            line(std::format("memory checkpoint: {}: private commit {:.2f} MiB", phase, double(*commit) / mib));
        const auto heaps = vk_.heap_use();
        for (std::size_t i = 0; i < heaps.size(); ++i)
            line(std::format("memory GPU: {}: heap {} ({}), allocation bytes {:.2f} MiB, {} allocations", phase, i,
                             heaps[i].device_local ? "device-local" : "host", double(heaps[i].allocated) / mib,
                             heaps[i].allocations));
    }

    // The Lua UI goes first: its state's finalizers may still reach what the eq.* functions hold (eq_host_, eq_api_,
    // the views), which members declared after it would already have destroyed.
    ~ClientApp() { lua_ui_.reset(); }
    ClientApp(const ClientApp&) = delete;
    ClientApp& operator=(const ClientApp&) = delete;
    explicit ClientApp(const ClientOptions& options)
        : opts_(options), window_(window_desc(options)), vk_(window_, options.validation),
          ui_(window_, vk_,
              ui::ImGuiLayerDesc{.scale_override = ui_scale_setting(options),
                                 .ini_path = platform::imgui_ini()}) {
        log_memory("startup: window, Vulkan, fonts");
        apply_window_icon(window_);
        shader_dir_ = platform::shaders_dir();
        config_path_ = default_config_path();
        config_.load(config_path_);
        if (opts_.clip_plane != 15) clip_plane_arg_ = opts_.clip_plane;
        if (opts_.max_fps > 0) max_fps_arg_ = opts_.max_fps;
        // The asset folder: --game-dir or EQ_GAME_DIR, else hailstone.ini assets.path, else the picker asks.
        if (!choose_asset_folder()) {
            quit_ = true;
            return;
        }
        apply_classic_folder();
        // The settings (hailstone.ini's until a character enters, then the character's: apply_settings), before the
        // renderers: their pipelines take the sample count.
        apply_settings();
        log_memory("startup: settings and render targets");
        // The saved servers (core/server_profile.h), the last one used picked (--server another); the old
        // login.host/port/username keys become the first.
        // The saved passwords (the system keyring); test logins never read or write them.
        if (opts_.auto_user.empty()) login_view_.credentials = &platform::system_credentials();
        // A fresh install with no saved server starts on the one the asset folder's eqhost.txt names.
        const std::filesystem::path eqhost = game_.find_base("eqhost.txt").value_or(std::filesystem::path());
        if (ui::load_login_settings(login_view_, config_, platform::servers_dir(), eqhost)) config_.save(config_path_);
        bool named_server = !opts_.server.empty() && ui::select_profile_named(login_view_, opts_.server);
        if (!opts_.server.empty() && !named_server)
            log::warn("--server {}: no saved server has this id or name", opts_.server);
        if (auto strings = game_.find("eqlsstr_us.txt")) net::load_login_strings(*strings);
        spawn_renderer_ = std::make_unique<render::SpawnRenderer>(vk_, shader_dir_);
        world_objects_ = std::make_unique<WorldObjects>(vk_, game_, shader_dir_);
        neighbours_ = std::make_unique<NeighbourZones>(vk_, game_, shader_dir_);
        log_memory("startup: world objects");
        spell_ui_ = std::make_unique<SpellUi>(vk_, game_);
        item_ui_ = std::make_unique<ItemUi>(vk_, game_);
        stats_ui_ = std::make_unique<StatsUi>(game_);
        log_memory("startup: icons and stat tables");
        // The user interface (ui/ui_packs.h): the picked server's (ServerProfile::ui), else hailstone.ini ui.name, a
        // folder of <client>/ui; its scripts' settings in ui/settings/<name>.txt of the data folder (the first-start
        // migration brings the old ones).
        ui_packs_ = ui::list_ui_packs(platform::ui_packs_dir());
        const ui::UiPack* pack = ui::find_ui_pack(ui_packs_, login_view_.ui);
        ui_name_ = pack ? pack->id : "modern";
        lua_ui_ = std::make_unique<ui::lua::LuaUi>(pack ? pack->dir : platform::ui_packs_dir() / "modern",
                                                   platform::ui_settings_file(ui_name_));
        login_view_.ui_packs = ui_packs_; // the login screen picks one per server (use_ui_pack at the login)
        refresh_game_folders();
        ui_textures_ = std::make_unique<ui::UiTextures>(vk_, game_);
        ui_textures_->set_all_art(pack && pack->game_art);
        log_memory("startup: Lua and UI textures");
        eq_host_.textures = ui_textures_.get();
        eq_host_.keep_cpp_windows = config_.get_bool("ui.cpp_windows", false);
        // the layout copy brings the UI's window layout of the character copied from (none: the shared layout)
        eq_host_.copy_ui_layout = [this](const std::filesystem::path& dir) {
            if (lua_ui_) lua_ui_->copy_character_layout(CharacterFiles{dir}.ui_settings(ui_name_));
        };
        apply_video_options();
        game_view_.dev_windows = config_.get_bool("ui.dev_windows", false);
        lua_ui_->set_show_control(game_view_.dev_windows);
        eq_api_ = std::make_unique<EqApi>(eq_host_);
        ui::lua::set_replaced_check([this](std::string_view screen) { return eq_api_ && eq_api_->replaces(screen); });
        lua_ui_->add_eq_api([this](lua_State* L) { register_eq_api(L); });
        lua_ui_->set_caller_scope(lua_caller_scope());
        lua_ui_->set_clipboard_check(lua_clipboard_check());
        // Plugins and MacroQuest Lua scripts (app/automation.h): under the server's hailstone.cfg, from the zone-in.
        web_service_ = std::make_unique<WebService>(WebServiceEnv{
            [this] { return browser(); }, [this] { return web_textures_.get(); },
            [this] {
                return (platform::data_dir() / "web" / (server_profile_.id.empty() ? "default" : server_profile_.id))
                    .string();
            },
            [this] {
                const auto& rules = conn_ ? conn_->server_rules() : std::nullopt;
                return rules ? rules->web_allow : std::vector<std::string>{};
            },
            [this] { web_text_input_ = true; }});
        Navigator::Env nenv;
        nenv.spawn = [this](std::uint32_t id) -> const game::Spawn* {
            if (!conn_) return nullptr;
            auto it = conn_->world().spawns.find(id);
            return it == conn_->world().spawns.end() ? nullptr : &it->second;
        };
        nenv.melee_range = [this](std::uint32_t id) { return conn_ ? melee_range_to(id) : std::nullopt; };
        nenv.line_of_sight = [this](const glm::vec3& a, const glm::vec3& b) {
            return collision_ && collision_->line_of_sight(a, b);
        };
        nenv.door = [this](std::uint8_t id) -> const game::Door* { return conn_ ? conn_->door(id) : nullptr; };
        nenv.doors = [this] {
            std::vector<const game::Door*> v;
            if (conn_)
                for (const game::Door& d : conn_->doors()) v.push_back(&d);
            return v;
        };
        nenv.use_door = [this](std::uint8_t id) { return use_door(id); };
        nenv.door_reachable = [this](std::uint8_t id) { return door_reachable(id); };
        nenv.door_in_way = [this](const glm::vec3& from, const glm::vec3& to) -> std::optional<std::uint8_t> {
            if (!world_objects_) return std::nullopt;
            game::SphereSweep s = game::SphereSweep::make(from, to, 2.0f);
            if (!s.valid()) return std::nullopt;
            std::vector<WorldObjects::DoorHit> hits;
            world_objects_->sweep_sphere(s, &hits);
            // Passing the doors not shut (Navigator::passes_open_doors): the first shut one, behind any open leaf.
            const bool pass = nav_ && nav_->options().pass_open_doors && conn_;
            std::erase_if(hits, [&](const WorldObjects::DoorHit& h) {
                const game::Door* d = pass ? conn_->door(h.id) : nullptr;
                return d && d->state != game::kSwitchClosed;
            });
            if (hits.empty()) return std::nullopt;
            auto first = std::min_element(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.t < b.t; });
            return first->id;
        };
        // The same way as MQ's own lines (/nav's), so they reach the chat and the log alike.
        nenv.chat = [this](const std::string& t) {
            if (conn_) conn_->ui_message(t);
            else game_view_.chat.add(ui::ChatChannel::System, t);
        };
        nenv.request_server_path = [this](std::uint32_t id) {
            if (conn_) conn_->request_quiet_path(id);
        };
        nenv.server_path = [this] { return conn_ ? conn_->take_quiet_path() : std::nullopt; };
        nenv.started = [this] {
            if (conn_ && conn_->follow_target()) conn_->stop_follow(false);
        };
        nenv.threads = std::clamp(config_.get_int("nav.threads", 2), 1, 8);
        // On by default: a zone with no cached mesh starts building it at the zone-in, so it is there when first wanted.
        nenv.prebuild = config_.get_bool("nav.prebuild", true);
        nenv.assist_follow = config_.get_bool("nav.follow", true);
        nav_ = std::make_unique<Navigator>(std::move(nenv));
        AutomationEnv aenv;
        aenv.nav = [this]() -> nav::NavControl* { return nav_.get(); };
        aenv.web = web_service_.get();
        aenv.install_dir = platform::install_dir();
        aenv.data_dir = platform::data_dir();
        aenv.autoload = config_.get("plugins.autoload").value_or("*");
        aenv.script_autoload = config_.get("scripts.autoload").value_or("");
        aenv.connection = [this] { return conn_.get(); };
        aenv.server_name = [this] { return char_view_.server_name; };
        aenv.server_profile = [this] { return server_profile_.id; };
        aenv.profile_name = [this] { return server_profile_.name; };
        aenv.window_open = [this](std::string_view w) { return eq_api_ && eq_api_->shown(w); };
        aenv.face = [this](double degrees, bool) { face_degrees(degrees); };
        aenv.keypress = [this](std::string_view command, int mode) { return script_keypress(command, mode); };
        aenv.max_camera_distance = [this] { return max_camera_distance_; };
        aenv.set_max_camera_distance = [this](float d) {
            max_camera_distance_ = d;
            camera_distance_ = std::min(camera_distance_, d);
        };
        aenv.hot_button = [this](int bar, int page, int button) -> std::optional<std::string> {
            return actions_ui_ ? actions_ui_->hot_button_spec(bar, page, button) : std::nullopt;
        };
        aenv.set_hot_button = [this](int bar, int page, int button, const std::string& spec) {
            return actions_ui_ && actions_ui_->set_hot_button_spec(bar, page, button, spec);
        };
        aenv.hot_bar_page = [this](int bar) { return actions_ui_ ? actions_ui_->hot_bar_page(bar) : -1; };
        aenv.set_hot_bar_page = [this](int bar, int page) {
            if (actions_ui_) actions_ui_->set_hot_bar_page(bar, page);
        };
        aenv.ability = [this](mq::AbilityKind kind, std::string_view name) {
            return ability_state(eq_host_, kind, name);
        };
        aenv.autoskill_list = [this] { return autoskill_list(eq_host_); };
        aenv.use_ability = [this](mq::AbilityKind kind, std::string_view name) { return use_ability(eq_host_, kind, name); };
        aenv.can_see = [this](std::uint32_t id) -> std::optional<bool> {
            return conn_ ? can_see_spawn(id) : std::nullopt;
        };
        aenv.line_of_sight = [this](const std::array<float, 3>& a, const std::array<float, 3>& b) -> std::optional<bool> {
            if (!collision_) return std::nullopt;
            return collision_->line_of_sight(glm::vec3(a[0], a[1], a[2]), glm::vec3(b[0], b[1], b[2]));
        };
        automation_ = std::make_unique<Automation>(aenv);
        automation_->hooks().add_command(plugin::Hooks::kClient, "/lighting",
                                         [this](std::string_view args) { lighting_command(args); });
        stats_ui_->set_open(opts_.show_stats);
        // The performance window (app/perf_ui.h): Ctrl+Shift+P, /perf, or ui.perf_window=1.
        perf_ui_.set_open(config_.get_bool("ui.perf_window", false));
        perf_ui_.set_log_hitches(config_.get_bool("ui.perf_log_hitches", false));
        automation_->hooks().add_command(plugin::Hooks::kClient, "/perf", [this](std::string_view) { perf_ui_.toggle(); });
        // /neighbourzones [on | off]: the zones past the outdoor zone lines (display.neighbour_zones).
        automation_->hooks().add_command(plugin::Hooks::kClient, "/neighbourzones", [this](std::string_view args) {
            while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
            bool on = args.empty() ? !neighbour_zones_ : !(args.starts_with("off") || args.starts_with("0"));
            set_display_option("neighbour_zones", on ? 1 : 0);
            if (conn_)
                conn_->ui_message(std::format("Neighbour zones: {} ({} loaded).", on ? "on" : "off",
                                              neighbours_ ? neighbours_->loaded() : 0));
        });
        automation_->hooks().add_command(plugin::Hooks::kClient, "/screenshot", [this](std::string_view) { take_screenshot(); });
        // /web [address]: the in-game browser (docs/browser.md; a client extension), on the address in a new tab.
        // The player's own: a script or plugin opens pages with /url, under its 'web' permission.
        automation_->hooks().add_command(plugin::Hooks::kClient, "/web", [this](std::string_view args) {
            if (!plugin::caller_name().empty()) {
                if (conn_) conn_->ui_message(std::format("[MQ] {}: /web is the player's; scripts use /url", plugin::caller_name()));
                return;
            }
            while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
            if (args.empty()) {
                if (eq_api_) eq_api_->toggle("BrowserWnd");
                return;
            }
            std::string url(args);
            if (url.find("://") == std::string::npos) url = "https://" + url;
            if (!web::url_allowed(url, {})) {
                if (conn_) conn_->ui_message(url + " is not an http or https address.");
                return;
            }
            if (!open_web_page(url) && conn_) conn_->ui_message("Opening " + url + " in your web browser.");
        });
        // /luasocial [n | new]: the Lua social editor on social n (1..120), or on the first empty social.
        automation_->hooks().add_command(plugin::Hooks::kClient, "/luasocial", [this](std::string_view args) {
            lua_social_command(args);
        });
        actions_ui_ = std::make_unique<ActionsUi>(config_, config_path_);
        // The gamepad (app/gamepad.h, docs/gamepad.md; a client extension): hailstone.ini gamepad.*.
        gamepad_ = std::make_unique<Gamepad>(config_, config_path_);
        gamepad_->set_state(&window_.input().pad);
        gamepad_->set_rumble([this](float low, float high, int ms) { return window_.rumble_gamepad(low, high, ms); });
        eq_host_.gamepad = gamepad_.get();
        if (!opts_.input_script.empty())
            input_script_ = std::make_unique<InputScript>(
                opts_.input_script, window_,
                InputScript::Hooks{[this](const std::string& path) { vk_.request_screenshot(path); },
                                   [this] {
                                       return std::string("pad ") + (gamepad_->active() ? "active" : "inactive") +
                                              ", mode " + pad_mode_name(gamepad_->mode()) + ", " + ui_.debug_state();
                                   },
                                   [this] {
                                       if (conn_) conn_->simulate_link_loss();
                                   }});
        // HandleWindowCloseRequest: the window's close button camps in game (window_close_request).
        window_.set_hold_close_requests(true);
        group_ui_.set_testing(opts_.auto_commands, opts_.auto_accept_invite);
        social_ui_.set_testing(opts_.auto_accept_invite);
        // Sound (audio/): the settings are hailstone.ini's audio.* keys; no device = silent.
        audio_ = std::make_unique<audio::AudioSystem>(game_, audio::load_audio_settings(config_), !opts_.no_audio);
        log_memory("startup: audio scheduled");
        sound_events_ = std::make_unique<SoundEvents>(*audio_, game_);
        // A crash last run: its report is made now and its window shown (app/client_app_bugreport.cpp).
        report_last_crash();
        // audio.pregame_music: the login, server and character select themes (the login screen's Music box).
        pregame_music_ = config_.get_bool("audio.pregame_music", true);
        login_view_.pregame_music = pregame_music_;
        {
            // ui/loadingbackgrounds, made empty so it can be found: the client's pictures (ui/screen_pictures.h).
            // Never shipped from the source tree, so an update or build never removes the player's.
            std::error_code ec;
            std::filesystem::create_directories(platform::ui_packs_dir() / ui::kPictureFolder, ec);
        }
        audio_->music_selection(audio::kMusicLogin, pregame_music_); // CDisplay_InitDisplay
        spell_particles_ = std::make_unique<SpellParticles>(vk_, game_, shader_dir_, [this] {
            return collision_ ? &*collision_ : nullptr;
        });
        log_memory("startup: spell particles");
        spell_bolts_ = std::make_unique<SpellBolts>(vk_, shader_dir_);
        actor_emitters_ = std::make_unique<ActorEmitters>(vk_, game_, shader_dir_);
        apply_particle_densities();
        weather_renderer_ = std::make_unique<render::WeatherRenderer>(vk_, shader_dir_);
        load_precipitation();
        view_ = game::compute_view_distance({}, opts_.clip_plane);
        camera_distance_ = opts_.camera_distance;
        camera_type_ = camera_distance_ > 0.0f ? game::kCameraWheelFollow : game::kCameraFirstPerson;
        // The isometric camera when it was the one in use (hailstone.ini camera.iso, /camera).
        if (config_.get_bool("camera.iso", false)) set_camera_type(game::kCameraIsometric);
        game::reset_camera_defaults(cameras_);
        if (!opts_.auto_user.empty()) {
            // Test logins never use the player's remembered server: a test account's password must not reach a
            // server someone else runs. Only a server named with --server is used; else --host/--port (or the dev
            // server) as a transient profile, never saved.
            if (!named_server) {
                ui::LoginView fresh;
                login_view_.transient = true;
                login_view_.selected = -1;
                login_view_.host = opts_.auto_host.empty() ? fresh.host : opts_.auto_host;
                login_view_.port = opts_.auto_port > 0 ? opts_.auto_port : fresh.port;
            } else if (!opts_.auto_host.empty() || opts_.auto_port > 0) {
                // --server with --host / --port: the address typed is the one the test password goes to; the
                // profile gives the assets folder and the characters' files. A test login saves nothing.
                if (!opts_.auto_host.empty()) login_view_.host = opts_.auto_host;
                if (opts_.auto_port > 0) login_view_.port = opts_.auto_port;
                log::warn("--server {} with --host/--port: logging in to {}:{} (not the profile's address)",
                          opts_.server, login_view_.host, login_view_.port);
            }
            login_view_.username = opts_.auto_user;
            login_view_.password = opts_.auto_password;
            core::wipe(opts_.auto_password); // the copy in the fields is the one start_login uses and lets go
            start_login();
        } else if (!opts_.account.empty()) {
            start_account_login();
        }
    }

    int run();

private:
    void start_login();
    // The login screen's Log in box: saves the server, starts a Hailstone for each box account after the first
    // (--server --account --character) and logs this window in to the first (client_app_login.cpp).
    void start_box_login();
    // --account: the saved account's keyring password, then start_login(); else the login screen says why.
    void start_account_login();
    // The login screen's Characters list: logs in to the picked character's account (its saved password, or the
    // one typed), plays its world and enters it.
    void play_character();
    // The picture behind the pre-game and loading screens (ui/screen_pictures.h): a new one at random on coming to
    // the login screen, on zoning, and when the server whose pictures they are changes. Called first in draw_ui; the
    // picture beneath a screen is drawn last in it (draw_screen_picture_behind), after the screen's actions.
    void update_screen_picture();
    void draw_screen_picture_behind();
    // Records the character entered in the account's SavedAccount::character and world, for the next box login and
    // the Characters list.
    void note_entered_character(const std::string& name);
    // Records the characters this account's character select lists on this world (the Characters list), when they
    // changed since the last frame's.
    void note_listed_characters();
    // Reads the server's accounts again after a note_* wrote them, for the login screen.
    void reload_saved_accounts();
    // The login screen's Save, Delete and Open folder (client_app_login.cpp).
    void login_action(ui::LoginAction action);
    // The updater (client_app_update.cpp): started with the run, its news put in the login view each frame, the
    // login screen's update buttons, and what the end of the run does (run_client).
    void start_updater();
    void update_updater();
    void update_login_action(ui::LoginAction action);
    // The add-on manager (app/client_app_addons.cpp, docs/addons.md).
    void start_addons();
    void update_addons();
    void addons_login_action(ui::LoginAction action);
    // Starts the installed tool add-on `name` (tools/<name>/, docs/addons.md), told the data folder; false with
    // *error why not (not installed, its program gone, or the system would not start it).
    bool start_addon_tool(std::string_view name, std::string* error);
    // Hailstone Texture Studio's add-on name (projecthailstone/hailstone-texstudio): Options' "Texture Studio..."
    // starts it once it is installed, from that repository and signed with the project's add-on key
    // (kTextureStudioKey, the key the add-on index lists for the project's add-ons): the button starts a program by
    // name, so another add-on that took the name, or an unsigned one, is not started from it (its row's Start still
    // works, where the player sees what it is).
    static constexpr std::string_view kTextureStudio = "texstudio";
    static constexpr std::string_view kTextureStudioRepo = "projecthailstone/hailstone-texstudio";
    static constexpr std::string_view kTextureStudioKey =
        "1cec1c02c81c578912f0b8dcae84277c60ddb80271cfa28793cc06bc7ae51608";
    bool texture_studio_installed() const;
    bool start_texture_studio(std::string* error);
public:
    struct UpdateExit {
        bool install = false; // a downloaded update is installed now
        bool restart = false; // and started
    };
    UpdateExit update_exit();
private:
    // Logs in with this saved server: its assets folder becomes the GameDir's overlay, and what was read from the
    // game files before login is read again when the layers change (reload_server_files).
    void apply_server_profile(const ServerProfile& profile);
    // at_login false: the server's files came during the zone-in (net/server_files.h); the login music stays off.
    void reload_server_files(bool at_login = true);
    // The server's [assets] rule (net::server_asset_only) on the overlay, from what its folder holds now; true when
    // what counts changed.
    bool apply_asset_rules(const std::filesystem::path& profile_dir);
    std::optional<std::set<std::string>> asset_only_; // the overlay files that count (nullopt: all)
    std::uint32_t asset_rules_serial_ = 0;             // the connection's server_rules_serial() last seen
    // Makes the user interface `id` (a folder of ui/; empty: hailstone.ini ui.name) the running one, when it is not
    // already: a new Lua state with its scripts and settings. Only before character select, where no Lua window is up.
    void use_ui_pack(std::string_view id);
    // The loaded plugins' windows for this UI pack (<plugin folder>/ui/<pack>/) into the Lua UI, and the bridge into
    // the eq API (plugin/ui_bridge.h).
    void sync_plugin_windows();
    // What a plugin's code in the Lua UI runs under: a plugin::CallerScope with that plugin's capabilities
    // (LuaUi::set_caller_scope).
    ui::lua::LuaUi::CallerScope lua_caller_scope();
    // Whether a plugin's code in the Lua UI may use the player's clipboard: only an owner with every capability, as
    // for a script (LuaUi::set_clipboard_check).
    ui::lua::LuaUi::ClipboardCheck lua_clipboard_check();
    // The user interface the current server uses from its next login (its profile's, else ui.name).
    std::string wanted_ui_pack() const;
    void update_login();
    // The login server's worlds into server_view_: favourites first, then by name; the picked world stays picked.
    void take_server_list();
    void list_worlds();
    // Marks or unmarks the server select's row as a favourite (hailstone.ini login.favorite_worlds.<profile>).
    void toggle_favorite_world(int row);
    std::string world_key(std::string_view what) const;
    // The 302 / 114 play reply's question ("remove the character still logged in?"), Yes / No to the login client.
    void draw_kick_question();
    void update_connection();
    // The OS window's title: the picked server's name, and the character's in the world (client_window_title).
    void update_window_title();
    // The original's slash commands that act on the client (app/client_app_commands.cpp), by handler name; true =
    // handled. GameConnection::run_slash_command hands them over.
    bool client_command(std::string_view handler, const std::string& args);
    void xtarget_command(const std::string& args); // /xtarget (client_app_commands.cpp)
    // /channel, /url, /www and /outputfile (app/client_app_info.cpp).
    bool client_info_command(std::string_view handler, const std::string& args);
    void outputfile_command(const std::string& args);
    void load_zone(const std::string& short_name);
    void unload_zone();
    // The precipitation's definitions (Resources/Precipitation/default.ini) and textures, and the sky's colour maps
    // (Resources/Sky), once the asset folder is known.
    void load_precipitation();
    // One frame of the falling drops and flakes (game/precipitation.h): after weather_.update.
    void update_precipitation(float dt, float sky_hour);
    // The scene behind character select and creation (app/client_app_pregame.cpp): CEverQuest::StartNetworkGame's
    // zone 190 (clz) with the model player (the local player of GameState 1 / 2) at 0, 0 on the floor, noon, and the
    // EQCamera_User of game/char_select_camera.h. update_pre_game loads it, keeps the model on the character selected
    // or being made and steps the camera; pre_game_scene() says whether the frame draws it.
    void update_pre_game(float dt);
    bool pre_game_scene() const { return pre_game_loaded_ && pre_game_world_ && (screen_ == Screen::Characters || screen_ == Screen::Create); }
    std::uint32_t pre_game_hidden_id() const { return pre_game_hidden_ ? kModelPlayerId : 0; }
    void load_pre_game_scene();
    void show_select_model();        // CCharacterListWnd::SelectCharacter's model (SetPlayerAppearanceFromPInfo)
    void sync_create_model();        // the client's own creation screen (no stock CharacterCreation): its choices
    void switch_model_player();      // CharSelect_SwitchPlayerModel's heights, floor and camera
    game::CCreateInputs ccreate_inputs() const;
    // What CDisplay::SetCCreateCamera reads of a spawn (the model player, or the player in game).
    game::CCreateInputs ccreate_inputs_for(const game::Spawn& m, const game::HeightTables* tables) const;
    game::Spawn* model_player();
    static constexpr std::uint32_t kModelPlayerId = 1;
    // The missiles in flight (game/missiles.h): the server's new ones launched (their source's attack animation,
    // its length the delay), stepped, and handed to the world objects to draw. Once a frame in game.
    void update_missiles();
    // The spell bolts' clock (app/spell_bolts.h), in seconds.
    static double bolt_clock() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
    game::MissileList missiles_;
    void enter_zone();
    void update_player(float dt);
    // The world's mouse presses and releases (__ProcessMouseEvents): the mouse look (game/mouse_look.h), the left
    // click to act on (left_click_) and whether the right release clicks (right_click_).
    void update_mouse_look();
    // A movement key command (game/mechanics/controls.h execute_movement_command) to the controls; CENTERVIEW's
    // CameraAngle = 0 in the first person camera is here, as the pitch is pitch_.
    void movement_command(int command, bool down, bool accept);
    // SetCameraType @0x48adf0 (game/cameras.h): another type than 6 puts the camera behind the player's distance
    // to 0. TOGGLECAM's cycle (CDisplay::ToggleView) is game::toggle_view_next. Into or out of the isometric camera
    // (7) the field of view changes to its own and back.
    void set_camera_type(int type);
    void apply_camera_selection(const game::CameraSelection& s);
    // /camera (Hailstone's own): which camera, and the isometric camera's settings.
    void camera_command(const std::string& args);
    // CDisplay::ToggleView @0x494160 (TOGGLECAM, the hot button's): the next camera of game::toggle_view_next.
    void toggle_view();
    // The gamepad's camera button (Hailstone's own): first person, or the camera behind the player at its last
    // distance.
    void set_view(bool behind);
    // Cameras 1..5: the spawn they follow (the player, as drawn this frame), and their update into camera_.
    game::CameraTarget camera_target(const glm::vec3& feet) const;
    void update_game_camera(const glm::vec3& feet);
    // unk_EQCamera_SetPosition's sweep for the cameras 1..6 (game/cameras.h).
    game::CameraSweep camera_sweep() const;
    // The player's eye: its z plus PlayerClient_GetEyeHeight (game/spawn_heights.h).
    glm::vec3 eye_position() const;
    void draw_ui(float fps);
    // The listener's place, environment and room (audio/listener.h) and the global music selections
    // (audio/music_manager.h), once a frame.
    void update_listener();
    game::SceneFogFrame scene_fog_frame(float sky_hour, double t);
    void update_music();
    // The `eq` table of the Lua UI over this client's state.
    void register_eq_api(lua_State* L);
    void trace_spawn(float dt);
    // The floor game::WorldState::ground_npcs measures a spawn against: the zone's and the doors', the higher.
    std::optional<float> grounding_floor(const glm::vec3& p) const;
    void draw_nameplates(const render::FrameInfo& frame, const glm::mat4& view_proj, VkExtent2D extent, float far_plane);
    // The 3D target indicator (app/target_ring.cpp, game/target_indicator.h): the ring at the target's feet, drawn
    // over the scene's depth (render/target_ring_renderer.h), so in the scene after begin_translucent (the classic
    // lighting, which has no other use for the copy, takes it when target_ring_shown).
    void draw_target_ring(const render::FrameInfo& frame, const glm::mat4& view_proj, float far_plane, bool modern,
                          double seconds);
    bool target_ring_shown() const; // the option on and a target in the world
    mutable std::optional<game::TargetIndicatorSettings> target_indicator_; // TargetIndicator.ini, read on first use
    // Hailstone's look (game::target_ring_look): the target it last showed and since when, the dashes' turn.
    std::uint32_t ring_target_ = 0;
    double ring_target_since_ = -1.0;
    double ring_last_seconds_ = -1.0;
    float ring_spin_ = 0.0f;
    bool ring_cues_ = true;  // display.target_ring_cues: the sweep on a new target, the pulse while attacking
    bool ring_xray_ = false; // display.target_ring_xray: the hidden part shows faintly through
    game::TargetRingColors ring_colors_; // display.target_ring_color.*, _opacity, _glow (Options > Display "Target Ring")
    int ring_max_size_ = game::kTargetRingMaxSizeOff; // display.target_ring_max_size, 0 off (Hailstone's cap)
    void load_ring_colors();
    const game::TargetIndicatorSettings& target_indicator() const; // read on first use
    std::optional<int> ring_option(std::string_view name) const;
    bool set_ring_option(std::string_view name, int value);
    void draw_text_overlay();
    void update_combat_input();
    // The gamepad (app/client_app_gamepad.cpp; docs/gamepad.md, a client extension), once a frame before the UI
    // frame: its mode, and in game mode its key commands (pad_events_, run with the keys' in update_player), its own
    // actions, the camera; in cursor mode the pad's pointer.
    void update_gamepad(float dt);
    void pad_action(const game::PadEvent& e);
    void pad_key(game::KeyCommand c);
    void pad_camera(const platform::GamepadState& pad, float dt);
    void pad_cursor(const platform::GamepadState& pad, float dt);
    void pad_interact();
    void pad_cancel();
    bool pad_window_takes_b_ = false; // a UI window closed itself on this B (eq.gamepad.takes_b)
    void pad_target_group();
    void release_pad_game_input();
    // The original's key map (app/actions_ui.h, game/keymap.h): one command started or ended by a key.
    void key_command(const game::KeyMap::Event& e);
    void chat_key(int slot);
    void screen_keys();
    // __ExecuteCmd(0x1ec) (key 492, /facepick): the player customization window.
    void player_customization_key();
    AbilityView ability_view() const;
    std::optional<bool> can_see_spawn(std::uint32_t id) const;
    void prepare_target_cycle();
    std::optional<float> melee_range_to(std::uint32_t id) const;
    void abilities_test(MovementKeys& mk);
    void melee_test(MovementKeys& mk);
    void face_target();
    // The frame's lighting for every renderer: the scene light, and in the modern mode the daylight at the sky clock's
    // `hour`, the player's carried light and the zone's lights near the camera.
    // `moving`, when given: the carried light and the spells' as the light list has them, for their shadows
    // (render::PointShadows::set_dynamic).
    render::SceneLighting scene_lighting(const game::SceneLight& scene, float hour,
                                         render::DynamicShadowLights* moving = nullptr) const;
    void lighting_command(std::string_view args);
    void lua_social_command(std::string_view args); // /luasocial (app/client_app_draw.cpp)
    float config_float(std::string_view key, float fallback) const {
        auto v = config_.get(key);
        if (!v || v->empty()) return fallback;
        char* end = nullptr;
        float f = std::strtof(v->c_str(), &end);
        return end != v->c_str() ? f : fallback;
    }
    // The Options window's Display page (app/lua_eq_dialogs.cpp OptionsWindow): its controls as whole steps (combo box
    // choices, slider ticks 0..20); nullopt for a name that is not a display setting.
    std::optional<int> display_option(std::string_view name) const;
    bool set_display_option(std::string_view name, int value);
    // Every option the Options window (ui/<pack>/options.lua: eq.Option / eq.SetOption) shows, by name
    // (app/client_app_options.cpp): the display ones above, the window and frame ones (video_option), the names,
    // mouse and sound settings; nullopt for a name the client does not have.
    std::optional<int> option_value(std::string_view name) const;
    void set_option_value(std::string_view name, int value);
    // A combo box's choices the client fills ("resolution": the display's modes); empty for others.
    std::vector<std::string> option_choices(std::string_view name) const;
    // The window mode, resolution, vertical sync, frame caps, field of view (hailstone.ini display.*); at start
    // and as the Options window changes them.
    std::optional<int> video_option(std::string_view name) const;
    bool set_video_option(std::string_view name, int value);
    // The window's mode at start (display.window_mode, display.resolution: shared, so only then).
    void apply_video_options();
    // Vertical sync, the frame caps and the field of view (apply_settings).
    void apply_frame_options();
    // Sets game_'s base folder (core/asset_folder_choice.h), running the asset-folder picker when nothing usable is
    // known; false when the player quit it (app/client_app_options.cpp).
    bool choose_asset_folder();
    // The Options window's "Change Asset Folder" (assets.pick) once the login screen is idle: the picker again, its
    // folder made game_'s base and the game files read again (reload_server_files); Cancel keeps the folder. Not
    // with --game-dir or EQ_GAME_DIR, which the picker does not change.
    void change_asset_folder_at_login();
    // Makes `folder` the asset folder while the login screen is idle: remembered as assets.path once the game runs
    // with it, and the game files read before the login read again. False when it cannot be used (logged).
    bool switch_asset_folder(const std::filesystem::path& folder);
    // The login screen's Game files row: the folder in use, then the ones used before, each checked (RoF2, live,
    // gone).
    void refresh_game_folders();
    // hailstone.ini assets.classic as game_'s classic folder (GameDir::set_classic): with a live base its copies of
    // the zones live changed come first (classic_preferred_file). Not with --game-dir / EQ_GAME_DIR.
    void apply_classic_folder();
    // The screenshot key (the key map's SCREENCAP, or Print Screen while no other command has it) and /screenshot:
    // the next frame goes to a new screenshots/hailstone-<date>-<time>.png (platform::new_screenshot_path).
    void take_screenshot();
    // Bug reports (app/client_app_bugreport.cpp): the EqHost hooks, a report with the client's facts and files
    // added, and the report made at the start from the last run's crash.
    void wire_bug_reports(EqHost& h);
    void leave_character(); // the connection gone (camp, logout, link death): the character's state let go
    // The settings read into the client and applied: at start, and again when a character's settings take over from
    // hailstone.ini or give way to it (use_character_settings). The settings taken only at start are shared
    // (Config::is_shared_key), so they are not read again.
    void apply_settings();
    // Hailstone: the character's settings (its hailstone.ini, Config::attach_character) over the shared ones from
    // the entry into the world; an empty folder goes back to the shared ones. Nothing happens for the folder in use.
    void use_character_settings(const std::filesystem::path& character_dir);
    // What the command line set, which the saved settings do not change (apply_settings).
    std::optional<int> clip_plane_arg_, max_fps_arg_;
    bool settings_applied_ = false; // apply_settings has run once
    // The connection's settings (the combat and camp switches, ground_npcs, illusion_mounts): as it is made, and again
    // with a character's settings.
    void apply_connection_settings();
    std::future<std::optional<SavedBugReport>> save_bug_report(BugReport report);
    // The client's facts, files and the screenshot's bytes into a report, as they are now (at its Send).
    void fill_bug_report(BugReport& report);
    // The account names and home folder a report takes out of its files (BugReport::accounts, home).
    void add_private_names(BugReport& report) const;
    void report_last_crash();
    bool print_screen_takes_screenshot() const;
    // The spawns PlayerZoneClient::IsInvisible hides for Hide Traders and Hide Buyers (GameConnection::market_hidden),
    // gathered each frame: not drawn, named or clicked.
    void update_market_hidden(bool in_game);
    // The viewer's see-invisible rank for a batch of spawn_invisible tests: GameConnection::see_invisible_rank
    // builds the character sheet, so it is asked only when some spawn is hidden at a rank (0 otherwise, which no
    // test then reads). Drawing asks once a frame; lighting reads market_hidden_.
    std::int32_t viewer_see_invisible_rank() const;
    std::unordered_set<std::uint32_t> market_hidden_;
    game::PlateFade plate_fade_; // each overlapped name's fade toward its target alpha (game/nameplate_overlap.h)
    // The spawn not to draw: the player's own in first person (camera 0's activate hides it; no other camera does).
    std::uint32_t hidden_spawn() const {
        return camera_type_ != game::kCameraFirstPerson || eq_host_.customization_zoom >= 0 ? 0u
            : (conn_->world().view_id ? conn_->world().view_id : conn_->world().player_id);
    }
    // display.msaa (samples, read at start-up), display.tonemap (auto | none | agx | aces; auto is AgX with the modern
    // lighting and none with the classic), display.exposure (EV), display.shadows (the shadow map's size per cascade,
    // 0 off; the modern lighting only), display.ao (screen-space ambient occlusion, the modern lighting only),
    // display.distance_blur, display.depth_prepass (the zone's depth pre-pass, the modern lighting only),
    // display.bloom (0 off .. 1), display.brightness / contrast / gamma (the grade after the
    // tonemap), display.render_scale (percent, 50..100: below 100 FSR 1 upscales) and display.sharpness (0..1). The
    // Options window's Display page sets them (display_option / set_display_option).
    void apply_scene_options() {
        render::SceneOptions scene;
        // The sample count stays the one of the first call: the renderers' pipelines are built for it, and scene
        // targets of another count under them lose the device on some drivers (Intel on Windows). A change in the
        // Options window is saved and shows at the next start, as the window says.
        if (msaa_at_start_ == 0)
            msaa_at_start_ = static_cast<std::uint32_t>(std::clamp(config_.get_int("display.msaa", 4), 1, 16));
        scene.msaa = msaa_at_start_;
        std::string tonemap = config_.get_string("display.tonemap", "auto");
        scene.tonemap = tonemap != "auto" ? render::parse_tonemap(tonemap)
                        : lighting_mode_ == render::LightingMode::Modern ? render::Tonemap::AgX
                                                                         : render::Tonemap::None;
        scene.exposure = std::strtof(config_.get_string("display.exposure", "0").c_str(), nullptr);
        scene.shadow_size = static_cast<std::uint32_t>(std::clamp(config_.get_int("display.shadows", 2048), 0, 8192));
        scene.light_shadows = static_cast<std::uint32_t>(
            std::clamp(config_.get_int("display.light_shadows", 8), 0, static_cast<int>(render::kMaxPointShadows)));
        scene.light_shadow_size =
            static_cast<std::uint32_t>(std::clamp(config_.get_int("display.light_shadow_size", 512), 128, 1024));
        scene.ambient_occlusion = config_.get_bool("display.ao", true);
        scene.distance_blur = config_.get_bool("display.distance_blur", true);
        scene.depth_prepass = config_.get_bool("display.depth_prepass", false);
        scene.color_grade = config_.get_bool("display.color_grade", true);
        scene.bloom = std::clamp(config_float("display.bloom", 0.5f), 0.0f, 1.0f);
        scene.brightness = config_float("display.brightness", 0.0f);
        scene.contrast = config_float("display.contrast", 1.0f);
        scene.gamma = config_float("display.gamma", 1.0f);
        scene.render_scale = static_cast<float>(std::clamp(config_.get_int("display.render_scale", 100), 50, 100)) / 100.0f;
        scene.sharpness = std::clamp(config_float("display.sharpness", 0.5f), 0.0f, 1.0f);
        vk_.set_scene_options(scene, shader_dir_);
    }
    std::uint32_t msaa_at_start_ = 0; // apply_scene_options: display.msaa as the client started, 0 before
    // /face: MQ's heading, degrees clockwise from north.
    void face_degrees(double degrees);
    // /keypress: a key command by name, tapped (0), held (1) or let go (2); false for an unknown name.
    bool script_keypress(std::string_view command, int mode);
    bool room_to_stand() const;
    void back_to_login(std::string message, bool is_error);
    // A finished camp (GameConnection::camped_out): character select through world again on the same ticket
    // (kind 0), the client closes (1, /camp desk) or the login screen (2, /camp server: the front end).
    void camp_finished(std::uint32_t kind);
    // HandleWindowCloseRequest @0x5f9660: the window's close button. In game it is Cmd_camp(local player, "desktop")
    // (/camp desk, with its checks; a second close during the camp abandons it); elsewhere the client quits.
    void window_close_request();
    // A link death (GameConnection::link_death): HandleWorldDisconnect's disconnected screen (CEverQuest::DisplayScreen
    // "Discon.bmp": eqstr 12353 on an empty screen) for its 3 s, and in game DoMainLoop's 16 s more; then on as a camp
    // ends (camp_finished with the camp kind) in game, else the front end.
    void update_link_death();
    // The world and zone connection for a world ticket (the server list's Play, or a camp back to character select).
    void start_game(const net::WorldTicket& ticket);
    void update_item_input();
    void draw_items();
    // Mouse picking as the original does it (game/picking.h, game/clicks.h):
    // the actor under a window point, from last frame's camera.
    struct PickTarget {
        enum class Kind : std::uint8_t { None, Spawn, Door, Object, Placed, Ladder };
        Kind kind = Kind::None;
        std::uint32_t id = 0; // Ladder: the index in zone_ladders_
        glm::vec3 point{0.0f};
        std::uint32_t re = 0, item = 0; // Placed: the real estate object's ids
    };
    // The zone's surface under a window point (the pick ray against the zone mesh), world coordinates.
    std::optional<glm::vec3> world_point_at(float x, float y);
    // Item placement mode (game/placement.h) each frame: the ghost under the mouse, re-validated.
    void update_placement();
    // `why` names the pick in the log; null picks quietly.
    PickTarget pick_at(float x, float y, std::uint32_t required, std::uint32_t skip, const char* why);
    // CEverQuest::LeftClickedOnPlayer.
    void left_click_spawn(std::uint32_t id);
    // CEverQuest::RightClickedOnPlayer (update_item_input): false when the spawn has no right-click action.
    bool right_click_spawn(std::uint32_t id);
    // Doors and ground objects (app/world_objects.h): a left click or the
    // USE key on one, the open tradeskill container, the --auto-door test.
    bool click_world(const PickTarget& hit);
    // CEverQuest::ClickedSwitch's key and skill, then EQSwitch::UseSwitch.
    bool use_door(std::uint8_t id);
    bool door_reachable(std::uint8_t id) const; // within reach for use_door's click
    // Ladder_MountClickedLadder on a clicked ladder (a ground object or one of the zone's).
    void climb_ladder(const game::mechanics::LadderSwitch& ladder);
    // `at_cursor` false: what is at the middle of the screen instead of under the mouse (the gamepad's interact).
    void use_key(bool at_cursor = true);
    void draw_world_container();
    void drive_auto_door(MovementKeys& mk);
    void drive_auto_board();
    Clock::time_point auto_board_zone_{}; // the zone-in HAILSTONE_AUTO_BOARD last boarded after
    bool auto_board_helm_ = false;
    // PlayerZoneClient::DoTeleport for a zone-line region's teleport text (game/zoning.h).
    void region_teleport(const std::string& text);
    // PlayerZoneClient::DoTeleportB @0x507ca0 for the local player (game/zoning.h): the same zone moves the player
    // here, nothing sent; another zone goes through MoveToZone. `what` names the caller in the log.
    bool teleport_b(const game::TeleportBArgs& a, const char* what);
    void drive_pick_test(MovementKeys& mk);
    int pick_test_step_ = 0;
    std::vector<std::uint32_t> pick_test_ids_;
    bool pick_test_clicked_ = false;
    std::uint8_t pick_test_door_ = 0;
    Clock::time_point pick_test_at_{};
    void drive_door_walk(MovementKeys& mk, const game::Door& door);
    void drive_drop_pickup();

    ClientOptions opts_;
    GameDir game_; // empty until choose_asset_folder() gives it its base, before anything holds it
    // The saved server of this login (core/server_profile.h): its id names servers/<id>/ for the per-server and
    // per-character files; empty before the first login and for a test login's transient one.
    ServerProfile server_profile_;
    Config config_;
    std::filesystem::path config_path_;
    std::filesystem::path shader_dir_;
    platform::Window window_;
    std::string window_title_; // what update_window_title() set last
    render::VkContext vk_;
    // Declared after vk_, so it is destroyed before the Vulkan device it holds objects of: draw_nameplates makes it at its
    // first use.
    std::unique_ptr<NameplateLayer> plate_layer_; // draw_nameplates, made at its first use
    // draw_nameplates' lists, kept for their capacity (and the plates' strings for theirs): the first plate_count_.
    struct PlateEntry {
        std::uint32_t id = 0;
        NameplateText plate;
    };
    std::vector<PlateEntry> plates_;
    std::size_t plate_count_ = 0;
    std::vector<game::PlateBox> plate_boxes_;
    std::vector<float> plate_alpha_;
    std::vector<std::size_t> plate_order_;
    ui::ImGuiLayer ui_;
    bool quit_ = false;
    bool close_pending_ = false; // a window close request held through a loading screen

    Screen screen_ = Screen::Login;
    ui::LoginView login_view_;
    ui::ServerSelectView server_view_;
    ui::CharacterSelectView char_view_;
    ui::CharacterCreateView create_view_;
    ui::LoadingView loading_view_;
    ui::InGameView game_view_;

    // The in-game browser, before automation_: plugins and scripts close their tabs as they shut down.
    std::unique_ptr<WebService> web_service_;      // who may do what with the tabs (plugin/web_access.h)
    std::unique_ptr<web::BrowserHost> browser_;      // browser(); its helper starts with the first tab
    std::unique_ptr<ui::WebTextures> web_textures_; // the tabs' images
    // Before conn_: the connection raises its hooks, so they outlive it.
    std::unique_ptr<Automation> automation_;
    std::unique_ptr<net::LoginClient> login_;
    std::vector<net::ServerInfo> servers_;
    // The server select's rows in the order it lists them (ui::server_list_order): row -> index into servers_.
    std::vector<std::size_t> server_rows_;
    bool auto_play_sent_ = false;
    // A box login (Log in box, --account): play the world played last on the server without the server select, and
    // enter box_character_ (one time: after a camp the player chooses).
    bool box_play_ = false;
    std::string box_world_; // the world to play (empty: the last played on the server, else the first)
    std::string box_character_;
    std::string listed_characters_; // what note_listed_characters() wrote last (a signature)
    // The pre-game screens are built again when what they show changes, not each frame: the connection's
    // characters_serial() and the login's servers_serial() last built from, and when the server select's add-on
    // hint (which reads the server's folder) was last made.
    std::uint32_t characters_listed_serial_ = 0;
    std::uint32_t servers_listed_serial_ = 0;
    Clock::time_point server_hint_at_{};
    std::vector<std::string> worlds_noted_; // the worlds the login server listed, as set_listed_worlds() last had them
    platform::ChildProcesses box_clients_; // the box set's other windows
    platform::ChildProcesses tools_;       // the tool add-ons the player started (left running when Hailstone quits)
    // The updater (app/client_app_update.cpp, docs/updater.md): null when updates are off for this run.
    std::unique_ptr<Updater> updater_;
    UpdateSettings update_settings_;
    bool update_restart_ = false;   // Restart now: install at the end of the run, then start the new version
    bool update_asked_ = false;     // the player pressed Download this run (a quit may then install it)
    bool update_installs_at_quit() const;
    bool update_confirmed_ = false; // the login screen was shown: a pending update started fine
    // The add-on manager (app/client_app_addons.cpp): null when nothing can be fetched this run. The lists beside the
    // add-ons (the picked server's own, the ones put in by hand, that server's rules) are read every few seconds.
    std::unique_ptr<AddonManager> addons_;
    std::uint32_t addons_serial_ = ~0u;
    std::uint32_t addons_message_serial_ = 0;
    bool addons_dirty_ = false; // installed.txt changed without the manager (a removal): the ceilings again
    std::string addons_profile_;
    Clock::time_point addons_listed_{};
    std::vector<ServerAddon> server_addons_;
    std::vector<ManualAddon> manual_addons_;
    plugin::Rules addons_rules_;
    // Suggested add-ons (app/addon_suggest.h): the picked server's, as the login screen shows them, and those put
    // away for this run (by profile and addon_repo_key).
    std::vector<AddonSuggestion> suggestions_;
    std::set<std::string> suggest_later_;
    std::vector<std::string> addons_listed_worlds_; // the picked server's worlds (worlds.ini), read with the lists
    void update_server_select_hint();
    // The suggestions for a login to `host` on profile `profile` whose login server lists `worlds`, with that server's
    // cached rules and own files (none without the add-on manager, or with hailstone.ini addons.suggest = 0).
    std::vector<AddonSuggestion> suggestions_for(std::string_view profile, std::string_view host,
                                                 const std::vector<std::string>& worlds, const AddonIndex* index,
                                                 const plugin::Rules& rules, const std::vector<ServerAddon>& server_own);
    bool pregame_music_ = true;             // hailstone.ini audio.pregame_music
    // update_screen_picture(): the pictures of the server in question (and which folder they came from), the one
    // shown, and the screen of the last frame.
    std::vector<std::filesystem::path> pictures_;
    std::string pictures_source_ = "?";
    std::filesystem::path picture_;
    Screen picture_screen_ = Screen::InGame;
    bool picture_behind_ = false; // picture_ goes beneath this frame's screen
    std::mt19937 picture_rng_{std::random_device{}()};
    std::unique_ptr<net::GameConnection> conn_;
    bool creating_ = false;
    bool auto_enter_sent_ = false;

    std::optional<assets::Zone> zone_;
    std::unique_ptr<render::ZoneRenderer> zone_renderer_;
    std::unique_ptr<render::SkyRenderer> sky_renderer_; // outdoor zones (OP_NewZone sky > 0)
    bool outdoor_light_ = false;                        // time-of-day light (time type 1, 2 or 5)
    int time_type_ = 0;                                 // OP_NewZone time_type (scene lighting)
    game::SkyClock sky_clock_;                          // from OP_TimeOfDay
    float sky_hour_ = -1.0f;                            // its hour this frame (1.0 .. 24.99), -1 before the first
    std::optional<net::TimeOfDay> sky_time_seen_;
    std::unique_ptr<render::SpawnRenderer> spawn_renderer_;
    std::unique_ptr<SpellUi> spell_ui_; // spell gems, book, cast bar, buffs (app/spell_ui.h)
    game::TargetManager target_manager_; // the target keys' cycle lists and remembered targets
    std::uint32_t frame_target_ = 0;     // DoPassageOfTime's last frame's target (0xb827f8)
    Clock::time_point auto_key_time_{};  // --auto-keys
    std::unique_ptr<ItemUi> item_ui_;   // inventory, bags, loot, item info (app/item_ui.h)
    std::unique_ptr<StatsUi> stats_ui_; // character / stats window (app/stats_ui.h)
    PerfUi perf_ui_;                    // performance window (app/perf_ui.h)
    std::unique_ptr<ui::lua::LuaUi> lua_ui_; // the Lua script windows (ui/<name>, src/ui/lua)
    std::vector<ui::UiPack> ui_packs_;      // the user interfaces installed (ui/ui_packs.h)
    std::string ui_name_;                    // the one running
    AssetFolderSource asset_source_ = AssetFolderSource::Picker; // where game_'s base folder came from
    std::string layout_for_;                 // the character's UI.ini the windows are placed from
    std::filesystem::path ui_character_dir_; // the character in the world: its folder (its UI layout, ui-<pack>.txt)
    // The profile whose servers/<id>/ holds the character's files: the active server profile's id
    // (server_profile_), or for a test login's transient one the folder made from the world's name
    // (transient_profile_id). Set when the login server accepts the world.
    std::string files_profile_;
    std::string files_profile() const {
        return files_profile_.empty() ? transient_profile_id(char_view_.server_name, server_profile_.host,
                                                             server_profile_.port)
                                      : files_profile_;
    }
    std::filesystem::path bug_capture_;              // the bug report window's screenshot (cache/), empty: none
    std::optional<BugReport> crash_report_;          // the last run's crash, as reported at this start
    std::optional<SavedBugReport> crash_saved_;      // its file; reset when the player closes the window
    float fps_ = 0.0f;                               // the frame rate draw_ui last had, for bug reports
    bool screenshot_this_frame_ = false; // take_screenshot() ran this frame (the key and the key map's both)
    bool overlay_loaded_ = false; // the game files read last came from a server assets folder with files
    std::unique_ptr<ui::UiTextures> ui_textures_; // the stock UI art for them (ui/ui_textures.h)
    // The art reload_server_files replaced, kept until the frame whose draw lists may still name it is submitted.
    std::unique_ptr<ui::UiTextures> retired_ui_textures_;
    EqHost eq_host_;                         // what their eq.* API reads (app/lua_eq.h)
    std::unique_ptr<EqApi> eq_api_;
    void update_eq_host();
    // The in-game browser (docs/browser.md): its tab host, made on first use when browser/hailstone-web is installed
    // (null without it: pages go to the system's browser); open_web_page opens one in its window (true), else there.
    web::BrowserHost* browser();
    // `tab`: the tab it opened, 0 when it went to the system's browser.
    bool open_web_page(const std::string& url, std::uint32_t* tab = nullptr);
    ServerLinks server_links_; // OP_Weblink's limits (app/server_links.h)
    void open_server_link(const std::string& url);
    // Closes the tabs of scripts that stopped and plugins that unloaded (once a frame).
    void close_orphan_tabs();
    // The item previews' subjects (app/item_preview_view.h) from the connection's last replies, before the UI draws:
    // the object preview window's (CObjectPreviewWnd) and the marketplace's pane (CMarketplaceWnd's preview page).
    struct PreviewSlot {
        std::unique_ptr<ItemPreviewView> view;
        std::uint32_t serial = 0xffffffffu; // the reply the view shows
        game::Appearance look;              // the player's look it was built from
    };
    void sync_item_preview();
    void sync_item_preview(PreviewSlot& slot, const net::GameConnection::ItemPreview* ip);
    ProgressUi progress_ui_;            // AA, training and task debug windows (app/progress_ui.h)
    MarketUi market_ui_;                // duel, inspect, bazaar and mail debug windows (app/market_ui.h)
    std::unique_ptr<ActionsUi> actions_ui_; // key map, hot bar, Actions window, skills (app/actions_ui.h)
    std::vector<game::KeyMap::Event> key_events_; // this frame's key commands
    // The gamepad (app/gamepad.h): its mode, settings and bindings; this frame's key commands from it; the left
    // stick's movement as last sent; the group member its target_group gave last (0 yourself); the camera distance
    // R3 brings back; the frames menu mode has had no window that asked for the pad open.
    std::unique_ptr<Gamepad> gamepad_;
    std::vector<game::KeyMap::Event> pad_events_;
    game::StickMove pad_stick_;
    int pad_group_member_ = 0;
    std::unique_ptr<InputScript> input_script_; // --input-script (testing)
    float pad_camera_distance_ = 20.0f;
    int pad_menu_idle_ = 0;
    // /keypress's commands for the next frame, and a tap's release for the one after.
    std::vector<game::KeyMap::Event> script_keys_, script_key_release_;
    render::LightingMode lighting_mode_ = render::LightingMode::Classic;
    float view_distance_ = 2.5f; // display.view_distance
    glm::vec3 last_velocity_{0.0f};               // the last frame's SpeedX/Y/Z (hide checks)
    bool follow_detour_ = false;          // /follow's last step went round by the mesh (for the log)
    bool follow_assist_strafed_ = false;  // this frame's strafe was /follow's mesh help, not the player's
    float last_speed_run_ = 0.0f;                 // the last frame's SpeedRun (+0x7c, /follow's bands)
    bool follow_move_ = false, follow_run_ = false; // the last follow step (for the log)
    std::uint32_t follow_logged_ = 0;
    std::size_t abilities_test_step_ = 0;         // --abilities-test
    Clock::time_point abilities_test_at_{};
    int melee_test_phase_ = 0;                   // --melee-test
    Clock::time_point melee_test_at_{}, melee_test_fight_{};
    GroupUi group_ui_;                  // group window, invitation, pet window (app/group_ui.h)
    SocialUi social_ui_;                // guild, raid and friends windows (app/social_ui.h)
    Clock::time_point items_since_{};   // --show-items timer
    glm::vec2 right_down_pos_{0.0f};    // where the right button went down (the right click's place)
    // The mouse look (game/mouse_look.h) and the buttons as update_mouse_look last saw them; the left click to act
    // on this frame (at the press, or at the release after MouseTurnZoom's look) and its place; the right release
    // that clicks (RMouseUp); the wheel-follow camera's heading apart from the player's (EQCamera_WheelFollow +0x38
    // less the heading, radians), which a left-button look turns.
    game::MouseLook mouse_look_;
    bool mouse_left_held_ = false, mouse_right_held_ = false;
    std::optional<glm::vec2> left_click_pending_;
    std::optional<glm::vec2> left_click_;
    bool right_click_ = false;
    float chase_heading_ = 0.0f;
    Clock::time_point in_zone_since_{};
    Clock::time_point chat_saved_{}; // the chat history's last save (save_chat_history, every few minutes)
    bool auto_cast_done_ = false, auto_shot_ = false;
    int scribe_step_ = 0; // --auto-scribe: 0 sit, 1 scroll on the cursor, 2 scribing, 3 memorizing, 4 done
    Clock::time_point scribe_at_{};
    void auto_scribe(Clock::time_point now);
    // Global character archives, kept between zones; the Luclin player
    // models load as eqclient.ini [Defaults] UseLuclin* enables them.
    assets::CharacterArchiveCache characters_{true};
    std::unique_ptr<render::ActorRenderer> actor_renderer_; // character models; boxes for the rest
    // The last zone's character meshes and textures from the global archives, for the next zone's renderer
    // (render::ActorRenderer::release_kept): a zone change does not build and upload them again.
    std::unique_ptr<render::ActorRenderer::Kept> actor_kept_;
    std::shared_ptr<const assets::CharacterLibrary> actor_library_; // the zone's, for the item preview
    std::array<PreviewSlot, 2> previews_; // 0 the object preview window, 1 the marketplace's (EqHost's view index)
    bool browser_tried_ = false;
    bool web_text_input_ = false;                    // a page has the keyboard this frame
    std::unique_ptr<WorldObjects> world_objects_;           // doors and ground objects (app/world_objects.h)
    std::uint32_t grounded_doors_ = 0;                      // world_objects_' floor_generation the spawns were grounded on
    // The zones past the outdoor zone lines, drawn in the distance in the modern lighting (app/neighbour_zones.h);
    // display.neighbour_zones.
    std::unique_ptr<NeighbourZones> neighbours_;
    bool neighbour_zones_ = true;
    // The land round an outdoor zone in the modern lighting (render/horizon.h); display.horizon_hills. The drawn
    // neighbours (NeighbourZones::drawn_key) it was last hidden for.
    bool horizon_hills_ = true;
    std::uint64_t distant_hidden_key_ = 0;
    // How far the replacement models' (a texture pack's generated trees') levels of detail reach, in percent of their
    // own distances (render::ZoneRenderer::set_lod_scale); display.tree_detail, 50 to 200.
    int tree_detail_ = 100;
    std::unique_ptr<audio::AudioSystem> audio_;             // emitters, music, event sounds (audio/)
    std::unique_ptr<SoundEvents> sound_events_;             // what plays which sound (app/sound_events.h)
    // The listener (audio/listener.h): PlayerClient +0x11b4 (the last ceiling found above the player), CDisplay
    // +0x28ec (indoors) with its 500 ms check, HeadWet, and CDisplay +0x2d68 (a special room is set).
    std::optional<float> listener_ceiling_;
    bool listener_indoors_ = false;
    std::uint8_t head_wet_ = 0;
    bool special_room_ = false;
    bool stun_room_ = false; // the stun's room 0x17 is set (StunMe), until the stun ends
    // The global music selections made (EqSoundManager::SetMusicSelection's callers).
    bool music_attack_ = false, music_bank_ = false, music_merchant_ = false, music_trainer_ = false, music_loot_ = false;
    bool music_dead_ = false, music_select_ = false;
    std::uint8_t motion_head_wet_ = 0; // this frame's HeadWet (PlayerMovement)
    std::uint8_t motion_body_wet_ = 0; // this frame's BodyWet (+0xa2)
    std::unique_ptr<ZoneParticles> zone_particles_;         // the zone's torch and lantern flames (app/zone_particles.h)
    // The modern lighting's flames in place of the zone's own (render/fire_renderer.h): the zone's, found when it
    // loads (assets/zone_fires.h), and this frame's with the doors' and ground objects' added.
    std::unique_ptr<render::FireRenderer> fire_renderer_;
    // The EQG heightmap zones' radial flora (render/flora_renderer.h), drawn while ShowGrass is on (/showgrass, the
    // Options window's Radial Flora; display.grass).
    std::unique_ptr<render::FloraRenderer> flora_renderer_;
    bool grass_ = true;
    // Hailstone's Flora Distance (display.flora_distance): how far the flora is drawn, in units.
    float flora_reach_ = render::kFloraReach;
    std::vector<render::FireSource> zone_fires_;
    std::vector<render::FireSource> zone_mesh_fires_; // the flame meshes' alone (no particle clouds)
    std::vector<render::FireSource> frame_fires_;            // the zone's with the moving ones', when there are any
    std::span<const render::FireSource> frame_fire_list_;    // what the flames draw this frame
    std::vector<assets::ZoneFire> moving_fires_;             // the doors' and objects' this frame (capacity kept)
    std::vector<std::pair<float, glm::vec4>> flora_near_;    // the spawns standing in the grass (capacity kept)
    std::vector<glm::vec4> flora_pushers_;
    render::SceneLighting frame_lighting_{}; // this frame's lighting, for the flames' haze and depth terms
    std::unique_ptr<SpellParticles> spell_particles_;       // spell stage particles (app/spell_particles.h)
    std::unique_ptr<SpellBolts> spell_bolts_;               // the modern lighting's bolts and comets (app/spell_bolts.h)
    std::unique_ptr<render::TargetRingRenderer> target_ring_renderer_; // the target ring, made on first use
    std::unique_ptr<ActorEmitters> actor_emitters_;         // the models' and held items' own (app/actor_emitters.h)
    std::vector<ActorEmitters::Actor> actor_emitter_list_;  // this frame's actors that have some
    game::Weather weather_;                                 // OP_Weather played out (game/weather.h)
    render::WeatherLook weather_look_;                      // its wet ground, snow and storm light
    render::HazeBlend haze_blend_;                          // the modern haze, eased between places (zone-in resets)
    render::AmbientProbe ambient_probe_;                    // the zone's baked light round a point (characters, doors)
    render::ProbeBlend probe_blend_;
    std::unique_ptr<render::WeatherRenderer> weather_renderer_;
    std::minstd_rand weather_random_{0x5eed};               // the lightning's timing and thunder, the drops' rand()
    bool precipitation_loaded_ = false;                     // Resources/Precipitation/default.ini read (load_precipitation)
    // The procedural sky's colour maps (game/sky_colours.h), for the precipitation's colours (CSky +0x38).
    game::SkyState sky_state_;
    render::CoverMap cover_;                                // this frame's cover map (render/cover_map.h)
    bool cover_drawn_ = false;
    render::CoverMapState cover_state_, water_state_; // what the cover and water maps last showed
    render::PointShadows point_shadows_;                    // the zone lights' shadow cubes (render/point_shadows.h)
    float moon_phase_ = 0.5f;                               // game::SkyClock::moon_phase (or --moon)
    float blood_moon_ = 0.0f;                               // game::SkyClock::blood_moon (or --blood-moon)
    SoundEvents::LocalMotion motion_;                       // this frame's local movement, for its sounds
    int auto_door_step_ = 0;                                // --auto-door progress
    std::uint8_t auto_door_id_ = 0;
    std::uint8_t auto_door_state_ = 0;
    glm::vec3 auto_door_center_{0.0f}, auto_door_last_{0.0f}, auto_door_beyond_{0.0f};
    Clock::time_point auto_door_moved_at_{}, auto_door_logged_{};
    int auto_door_retries_ = 0;
    float auto_door_min_ = 0.0f;
    bool drop_confirm_ = false;               // "drop the cursor item?" is showing
    // The options window's Display page (eqclient.ini PCNames / NPCNames / ClipPlane in the original; here
    // hailstone.ini display.*): the name sprites by kind, and the zone's fog for the view distance.
    bool pc_names_ = true, npc_names_ = true;
    bool own_name_ = true; // Hailstone's switch for the player's own name sprite (display.own_name; game::NameSpriteSwitches)
    bool player_menu_ = true; // Hailstone's right-click menu on players (mouse.player_menu; game::opens_player_menu)
    bool con_name_colour_ = true;     // Hailstone: NPC names in their consider colour (display.con_name_colour)
    bool plate_overlap_fade_ = true; // Hailstone: a covered name fades (display.plate_overlap_fade)
    bool target_plate_through_walls_ = false; // Hailstone: the target's name shown behind walls (display.target_plate_through_walls)
    bool target_plate_highlight_ = true; // Hailstone: the target's name on a dark plate with a marker (display.target_plate_highlight)
    game::PlateStyle plate_style_;    // Hailstone: the name plates' font, size, edge and shrink (game/nameplate_style.h)
    int show_names_ = 4;              // __ShowNames (/shownames 1..6, display.show_names): the player name sprite's parts
    bool spell_effects_ = true;       // ShowSpellEffects (/showspelleffects, display.spell_effects): spell particles
    // EnvironmentParticleDensity (/particledensity and the Options window's combo; display.environment_particle_density,
    // else eqclient.ini's): what the zone's particle clouds emit, 0 none .. 1 all.
    float environment_particle_density_ = 1.0f;
    // ShowDynamicLights (/dynamiclights, display.dynamic_lights) as the zone was entered: the carried light and the
    // spells' lights.
    bool dynamic_lights_ = true;
    // ShadowClipPlane (/shadows on <n>, the Options window; display.shadow_clip_plane, 0..100): the actors within
    // game::shadow_clip_distance of the camera cast shadows.
    int shadow_clip_plane_ = game::kDefaultShadowClipPlane;
    // The 3D view's rectangle (/viewport, display.viewport; the original keeps it in the UI ini's ViewPort section):
    // kept as typed, and where it falls this frame in window coordinates (the whole window without one, or out of the
    // game). The scene is drawn there (render::VkContext::set_scene_rect); the windows stay over the whole window.
    std::optional<game::Viewport> viewport_;
    struct ViewArea {
        float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f;
        bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    };
    ViewArea view_area_;
    void update_view_area(); // before the frame: view_area_ and the scene rectangle
    // The UI font's face (/fontface, the Options window's font list; ui.font_face, the original's Fonts /
    // Font.<set>.0): every UI font is drawn in it (ui::ImGuiLayer::set_font_face); empty or not found, the Arial-class
    // font. The system's faces are read the first time they are needed.
    const std::vector<ui::FontFace>& font_faces() const;
    mutable std::optional<std::vector<ui::FontFace>> font_faces_;
    void apply_font_face();
    // The name plates' font (plate_style_) to the UI's atlas (ui::ImGuiLayer::set_plate_font): its face looked up in
    // font_faces() like the UI's; empty or not found, the UI's font.
    void apply_plate_font();
    // The particle densities by kind ("spell", "environment", "actor"): the setting, applied to its emitters (and saved
    // as display.<kind>_particle_density, 0..1).
    float particle_density(std::string_view kind) const;
    void set_particle_density(std::string_view kind, float density);
    // At the start: the hailstone.ini densities over the ones eqclient.ini gave.
    void apply_particle_densities();
    // ActorClipPlane (display.actor_clip_plane, default 50): how far spawns are drawn (game::actor_clip_distance).
    int actor_clip_plane_ = game::kDefaultActorClipPlane;
    std::optional<std::string> pending_ui_pack_; // /loadskin: the UI pack loaded before the next frame
    // The options window's Mouse page (hailstone.ini mouse.*; eqclient.ini's keys are not read): the look's
    // vertical direction, its speed (0..20, 10 the stock 0.0025 radians a pixel) and the wheel's camera zoom.
    bool mouse_invert_y_ = false, mouse_wheel_zoom_ = true;
    int mouse_sensitivity_ = 10;
    int background_fps_ = 0; // display.background_fps: the frame cap while another window has the focus (0 none)
    // MousePointerSpeedMod (/mousespeed, mouse.pointer_speed): the cursor's speed (platform::Window::set_pointer_speed);
    // test runs (--input-script, --auto-user) never take the desktop's cursor.
    void set_pointer_speed(int speed) {
        window_.set_pointer_speed(speed, opts_.input_script.empty() && opts_.auto_user.empty());
    }
    float mouse_look_scale() const { return 0.0025f * static_cast<float>(mouse_sensitivity_ + 1) / 11.0f; }
    game::ZoneFogInput zone_fog_{};
    std::set<std::uint32_t> auto_drop_seen_;  // --auto-door 4: ground objects before the drop
    Clock::time_point auto_door_at_{};
    std::optional<game::CollisionMesh> collision_;
    std::vector<game::ZoneLadder> zone_ladders_; // the zone's own ladders (game/zone_ladders.h), clickable
    // Navigation (app/navigator.h): /nav, /moveto and the Navigation TLO; after collision_, which it reads.
    std::unique_ptr<Navigator> nav_;
    // The Find window's guide path (app/guide_path.h) over nav_'s mesh, drawn by spell_particles_ and the maps.
    GuidePath guide_;
    std::string guide_zone_; // the zone it was started in: zoning ends it (spawn ids are the zone's)
    void update_guide();
    game::ZoneLineTrigger zone_line_;
    game::ViewDistance view_;
    // The per-frame fog and scene light (CDisplay_UpdateSkyWeatherAndFog and SetFog), the player's light level
    // (CActor::GetScaledAmbient) and the region type at the camera (0xd1f3c0).
    game::SceneFog scene_fog_;
    game::ScaledAmbient player_light_;
    std::uint8_t camera_region_ = 0;

    bool player_ready_ = false;
    glm::vec3 feet_{0.0f};
    float yaw_ = 0.0f, pitch_ = 0.0f;
    // The game clock (core/step_clock.h, game.fixed_step): this drawn frame's steps, and what the last step moved and
    // turned the player by (the camera is drawn between the last two steps).
    core::StepClock step_clock_;
    std::span<const core::StepClock::Step> frame_steps_;
    glm::vec3 step_move_{0.0f};
    float step_turn_ = 0.0f, step_pitch_ = 0.0f;
    std::array<game::mechanics::SwitchSpawn, 1> rider_spawns_{};
    game::DoorRiders door_riders_;
    PlayerMovement movement_;
    std::optional<PlayerMovement> local_body_movement_; // the passive body while an Eye has the controls
    std::uint32_t driven_eye_ = 0;
    float local_body_height_ = 0.0f, local_body_yaw_ = 0.0f, local_body_pitch_ = 0.0f;
    // Riding (game/mounts.h): the link's questions answered from the models and the movement's room test
    // (client_app_player.cpp), and the mount the controls moved last frame (0: the player itself).
    struct MountAnswers final : game::MountQueries {
        ClientApp* app = nullptr;
        bool new_models_loaded() override;
        bool mount_point(const game::Spawn& rider, const game::Spawn& mount) override;
        bool can_i_fit_here() override;
        double room_above() override;
        bool ceiling_and_floor(float& ceiling, float& floor) override;
        double room = 1e30; // the room do_stand's CanIFitHere left (+0x11b4 - +0x1070)
    };
    MountAnswers mount_answers_;
    std::uint32_t riding_id_ = 0;
    Clock::time_point riding_logged_{};
    void update_riding();
    // The vehicle spawns' models for the floor test and the walls (ZoneMoveWorld::Vehicle), placed each frame at the
    // pose they are drawn at from their triangles in the vehicle's frame, taken from the drawn model and taken again
    // every kVehicleRefreshSeconds, so a model's own animation moves its collision too (game/boats.h).
    std::vector<ZoneMoveWorld::Vehicle> vehicles_;
    struct VehicleModel {
        const void* model = nullptr; // the drawn model they came from (an illusion takes them again)
        std::vector<glm::vec3> triangles;
        glm::vec3 center{0.0f};
        float radius = 0.0f;
        std::vector<glm::vec3> probes; // the helm's (game::hull_probes)
        Clock::time_point taken{};
        // The take and pose its vehicles_ entry was last placed with: the same again reuses it as it is.
        Clock::time_point placed_taken{};
        game::BoatPose placed_pose{};
    };
    std::vector<ZoneMoveWorld::Vehicle> vehicles_spare_; // the last step's vehicles_, their storage reused
    std::unordered_map<std::uint32_t, VehicleModel> vehicle_models_;
    std::optional<glm::vec3> boat_hold_; // where a zone-in onto a boat holds the player until the boat is ready
    void update_vehicles();
    // Hailstone: the live NPCs near the player as the movement sweep's walls (game/npc_collision.h), unless the
    // server's hailstone.cfg turns them off ([movement] npc_collision).
    std::vector<game::NpcBody> npc_bodies_;
    // The other spawns' contact probes' door hits (ZoneMoveWorld::set_contact_probe), kept so a probe does not
    // allocate.
    std::vector<WorldObjects::DoorHit> remote_door_hits_;
    void update_npc_bodies();
    // PlayerClient::TouchingSwitch for the local player on door `id` (game/mechanics/touching_switch.h); returns
    // whether pTouchingSwitch is cleared afterwards.
    bool touching_switch(std::uint8_t id);
    std::uint32_t touch_time_ = 0;                     // PlayerClient +0x3d8
    std::optional<std::uint8_t> last_damage_switch_;   // 0xe6418c
    std::minstd_rand touch_rng_{std::random_device{}()};
    MovementTest move_test_{opts_.move_test};
    Clock::time_point start_time_ = Clock::now();
    Clock::time_point zone_entered_{};
    // The loading screen's last stretch (app/zone_settle.h): after the zone-in the world runs and draws behind it
    // until it has been drawn whole. settle_frame_ gathers what this frame left undone; loading_since_ is when the
    // loading screen came up, zone_loaded_ms_ / zone_in_ms_ when the zone was loaded and the zone-in done (the times
    // logged when it goes).
    ZoneSettle zone_settle_;
    ZoneSettleFrame settle_frame_;
    std::optional<Clock::time_point> loading_since_;
    std::int64_t zone_loaded_ms_ = -1, zone_in_ms_ = -1;
    bool zone_settling() const { return zone_settle_.active(); }
    void begin_zone_settle();
    void update_zone_settle();
    // When the character select screen last appeared (the input script's "c<seconds>" clock); unset off it.
    std::optional<Clock::time_point> select_shown_;
    // When the link death's disconnected screen appeared (update_link_death; the input script's "d<seconds>" clock).
    std::optional<Clock::time_point> link_death_since_;
    // The player's AvatarHeight (+0x138, its spawn's heights): its z is feet_ + this.
    float floor_offset_ = 0.0f;
    float camera_distance_ = 0.0f; // the camera behind the player's (6) distance; 0 outside it (the mouse wheel's)
    // CDisplay::cameraType (game/cameras.h): 0 first person, 1..5 the original's other cameras (cameras_), 6 the
    // camera behind the player (camera_distance_ > 0), 7 the isometric camera (iso_camera_).
    int camera_type_ = game::kCameraFirstPerson;
    std::uint32_t consciousness_changes_ = 0;
    std::uint32_t drunkenness_updates_ = 0;
    float drunk_fov_offset_ = 0.0f; // on top of camera_.fov_units (game::drunkenness_fov_offset)
    std::optional<int> unconscious_camera_;
    std::array<game::GameCamera, 6> cameras_;
    // The isometric camera (game/iso_camera.h): its state, its settings (hailstone.ini camera.iso_*), the field of
    // view the other cameras use while it has its own, and this frame's distance from the player (the view and the
    // shadow cascades are measured from the player with it).
    game::IsoCamera iso_camera_;
    game::IsoCameraSettings iso_settings_;
    float iso_saved_fov_ = game::kStockFovUnits;
    float iso_focus_distance_ = 0.0f;
    bool iso() const { return camera_type_ == game::kCameraIsometric; }
    // Places camera_ for the isometric camera this frame, looking at the player drawn at `feet`.
    void update_iso_camera(const glm::vec3& feet, float dt);
    // The isometric camera's click to move and movement relative to the camera (app/client_app_iso.cpp): WASD
    // relative to the camera or classic (hailstone.ini camera.iso_wasd); the gamepad's stick as last read in game mode
    // (x right, y up; zero when it moves nothing); a click's walk under way, and what to click when it arrives; the
    // left button held on the ground (the walk follows the cursor).
    bool iso_wasd_camera_ = true;
    glm::vec2 pad_iso_stick_{0.0f};
    bool iso_walking_ = false;
    struct IsoPending {
        PickTarget hit;
        bool right = false;
    };
    std::optional<IsoPending> iso_pending_;
    bool iso_hold_ = false;
    glm::vec3 iso_hold_point_{0.0f};
    Clock::time_point iso_hold_at_{};
    // A click in the isometric camera (game/click_intent.h): true when it walks instead of the original's click.
    bool iso_click(const glm::vec2& at, const PickTarget& hit, bool right);
    // Each frame: a key ends a click's walk, the held button aims it, an arrival makes its click.
    void update_iso_clicks(bool player_moved);
    // How near a walk to spawn `id` goes before its click.
    float iso_spawn_arrive(std::uint32_t id) const;
    // Each step: the movement keys as a heading relative to the camera (turns the player) and the forward key the
    // controls see (PlayerMovement::set_key_override); nullopt outside the isometric camera or with no key held.
    std::optional<bool> iso_key_override();
    // Each frame: the turn keys turn the isometric camera.
    void iso_turn_keys(float dt);
    // The cut through what stands in front of the player this frame (game/iso_occlusion.h): off outside the isometric
    // camera or with camera.iso_reveal 0 (its radius at the player).
    float iso_reveal_radius_ = 20.0f;
    game::IsoReveal iso_reveal() const;
    // The cut's parts (game/iso_occlusion.h): the cone only while something is in the way (or always), the roof while
    // something is over the player (camera.iso_roof); their strengths eased each frame, and the roof's height.
    bool iso_reveal_always_ = false;
    bool iso_roof_ = true;
    float iso_roof_radius_ = 60.0f;
    float iso_cone_strength_ = 0.0f, iso_roof_strength_ = 0.0f, iso_roof_height_ = 0.0f;
    void update_iso_reveal(float dt);
    // The player's silhouette where something the cut leaves still hides them (camera.iso_silhouette).
    bool iso_silhouette_ = true;
    // The click marker and the hover ring, each switchable (camera.iso_marker, camera.iso_hover).
    bool iso_marker_on_ = true, iso_hover_on_ = true;
    // What is under the cursor (a spawn, a door, a ground object) and its ring on the floor; picked ten times a
    // second or when the mouse moves.
    struct IsoHover {
        bool shown = false;
        std::uint32_t spawn = 0;
        glm::vec3 feet{0.0f};
        float radius = 2.0f;
    };
    IsoHover iso_hover_;
    glm::vec2 iso_hover_at_{-1.0f};
    Clock::time_point iso_hover_time_{};
    std::unique_ptr<render::TargetRingRenderer> iso_hover_renderer_;
    void update_iso_hover();
    void draw_iso_hover(const render::FrameInfo& frame, const glm::mat4& view_proj, float far_plane, bool modern,
                        double seconds);
    // The isometric camera's options (eq.Option / eq.SetOption "iso_*") and settings (hailstone.ini camera.iso_*).
    std::optional<int> iso_option(std::string_view o) const;
    bool set_iso_option(std::string_view o, int v);
    void load_iso_settings();
    void save_iso_settings();
    // The zone's triangles for a pick, less what the isometric camera's cut takes away there.
    std::function<void(const game::PickSegment&, const game::PickTriangleSink&)> pick_world() const;
    // How far above the feet the isometric camera looks: the middle of the body.
    float iso_focus_height() const { return floor_offset_ * 0.5f + 2.0f; }
    // The mark where a click's walk ends (a ring on the ground, drawn as the target ring is): where, since when (the
    // click's flash), and when the walk ended (it fades); its own ring renderer, made on first use.
    struct IsoMarker {
        glm::vec3 point{0.0f};
        bool active = false;
        bool click = false;      // a new click, its flash timed from the next draw (not the held button's aim)
        bool ending = false;     // the walk ended, its fade timed from the next draw
        double since = 0.0;      // the click (the draw's clock)
        double ended = -1.0;     // < 0: the walk goes on
    };
    IsoMarker iso_marker_;
    std::unique_ptr<render::TargetRingRenderer> iso_marker_renderer_;
    void set_iso_marker(const glm::vec3& point, bool click);
    bool iso_marker_shown() const { return iso() && iso_marker_.active; }
    void draw_iso_marker(const render::FrameInfo& frame, const glm::mat4& view_proj, float far_plane, bool modern,
                         double seconds);
    Clock::time_point last_mouse_look_{}; // the last mouse look's time (the chase camera's height rate)
    // The Options window's Max Camera Zoom sets it (10..150, saved); the NMS patch's /fov too (not saved).
    float max_camera_distance_ = kMaxCameraDistance;
    render::FlyCamera camera_;
    std::optional<game::WorldState> pre_game_world_; // the model player's world (one spawn, kModelPlayerId)
    bool pre_game_loaded_ = false;                   // clz is the loaded zone
    bool pre_game_failed_ = false;                   // clz did not load: not tried again until the next connection
    bool pre_game_hidden_ = true;                    // no character selected: the actor hidden (OnProcessFrame)
    game::UserCamera pre_game_camera_;
    double pre_game_step_ = 0.0;                     // the 50 ms camera step's clock
    float pre_game_saved_fov_ = 0.0f;                // the in-game camera's field of view, put back after
    std::string pre_game_shown_;                     // what the model shows ("" nothing yet)
    std::pair<std::uint32_t, int> pre_game_shown_for_{}; // the characters_serial() and row it was built from
    std::array<std::uint32_t, 3> pre_game_model_key_{}; // race, gender, class it was switched to
    std::array<std::uint32_t, 3> create_choice_{};      // the client's own creation screen's race, gender, class
    int pre_game_zoom_ = -1, pre_game_feature_ = 0;      // the customization camera last placed
    // The player customization window in game: Cameras[7] while it shows (update_camera), and the client's field of
    // view to put back when it hides (RestoreSavedCameraType).
    game::UserCamera customization_camera_;
    bool customization_camera_on_ = false;
    float customization_saved_fov_ = 0.0f;
    game::CharCreateLook char_create_look_;          // creation's copy of the model's look (EqHost::char_create_look)
    std::mt19937 pre_game_rng_{std::random_device{}()};
    game::PickCamera pick_camera_;   // last frame's, for mouse picking (CCamera as GetScreenRay reads it)
    bool pick_camera_valid_ = false;
};

} // namespace client_internal

} // namespace hailstone::app
