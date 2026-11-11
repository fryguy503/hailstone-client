#pragma once

// The client's plugins and MacroQuest Lua scripts together (docs/plugins.md): the hook bus the connection raises
// (plugin/hooks.h), the server's rules (plugin/policy.h) taken from the connection, MQ's TLOs and commands
// (mq/tlo.h, mq/commands.h), the script host (mq/lua_mq.h) and the compiled plugins (plugin/native_plugins.h).
//
// Folders (platform/paths.h): scripts in the server's servers/<profile>/scripts, then <data>/scripts, then
// <install>/scripts when that is another folder and exists; mq.configDir is <data>/scripts/config. Plugins in the
// server's servers/<profile>/plugins, then <data>/plugins, then <install>/plugins likewise; a
// plugin's binary sits directly in plugins/ or in its own folder plugins/<name>/, and plugins/<name>/ under the data
// folder is the plugin's own folder for its settings (the host's config_dir). The loaded plugins' copies go to
// <data>/cache/plugins. Plugins with the 'login' capability load at the world login (the last login's rules,
// provisional: plugin/policy.h), the others at the zone-in; hailstone.ini plugins.autoload lists the
// player's to load when the rules allow them ("*", the default: all allowed).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct HailstoneBuiltinAddon;

#include "game/ability_book.h"
#include "mq/tlo.h"
#include "plugin/consent.h"
#include "plugin/hooks.h"
#include "plugin/policy.h"

namespace hailstone::net { class GameConnection; }
namespace hailstone::mq { class ScriptHost; class Registry; struct Context; }
namespace hailstone::game { class ExtensionChain; }
namespace hailstone::plugin { class UiBridge; class WebAccess; }
namespace hailstone::nav { class NavControl; }

namespace hailstone::app {

struct AutomationEnv {
    std::filesystem::path install_dir; // the folder of the binary (platform::install_dir())
    std::filesystem::path data_dir;    // the folder Hailstone writes to (platform::data_dir())
    std::string autoload = "*";       // plugins.autoload
    std::string script_autoload;      // scripts.autoload: the player's scripts started when the rules allow them
    std::function<net::GameConnection*()> connection;
    std::function<std::string()> server_name;
    std::function<std::string()> server_profile; // the saved server's id (servers/<id>/), "" for a test login
    std::function<std::string()> profile_name;   // the saved server's name as the player knows it ("" none)
    std::function<bool(std::string_view window)> window_open;  // a stock window shows (eq API)
    std::function<void(double degrees, bool fast)> face;       // MQ degrees, clockwise from north
    std::function<bool(std::string_view command, int mode)> keypress; // /keypress (0 tap, 1 hold, 2 release)
    std::function<nav::NavControl*()> nav; // /nav, /moveto and the Navigation TLO (app/navigator.h); null: none
    // The camera's zoom limit for plugins (plugin::NativeEnv); setting it cuts the zoom to it.
    std::function<float()> max_camera_distance;
    std::function<void(float)> set_max_camera_distance;
    // The in-game browser for the scripts (mq.web) and plugins (the host table's web_*); null: none.
    plugin::WebAccess* web = nullptr;
    // The hot buttons for plugins (plugin::NativeEnv hot_button ...).
    std::function<std::optional<std::string>(int bar, int page, int button)> hot_button;
    std::function<bool(int bar, int page, int button, const std::string& spec)> set_hot_button;
    std::function<int(int bar)> hot_bar_page;
    std::function<void(int bar, int page)> set_hot_bar_page;
    // The player's abilities for Me.AbilityReady and the rest (mq::Context::ability); null: none known.
    std::function<std::optional<mq::AbilityState>(mq::AbilityKind, std::string_view)> ability;
    // Line of sight for Spawn.LineOfSight and LineOfSight[...] (mq::Context::can_see, line_of_sight); null: none.
    // The player's autoskill list with each ability's name (hailstone.autoskill_list); null: none.
    std::function<std::vector<std::pair<game::AutoskillRule, std::string>>()> autoskill_list;
    std::function<bool(mq::AbilityKind, std::string_view)> use_ability; // hailstone.use_ability; null: none
    std::function<std::optional<bool>(std::uint32_t spawn)> can_see;
    std::function<std::optional<bool>(const std::array<float, 3>&, const std::array<float, 3>&)> line_of_sight;
};

class Automation {
public:
    explicit Automation(AutomationEnv env);
    ~Automation();
    Automation(const Automation&) = delete;
    Automation& operator=(const Automation&) = delete;

    plugin::Hooks& hooks();
    bool load_builtin(const HailstoneBuiltinAddon& addon, const std::filesystem::path& trusted_ui_dir = {},
                      std::string* why = nullptr);
    // The plugins' server extensions as one (game/server_ext_chain.h): a connection asks it
    // (net::ConnectionOptions::plugins).
    std::shared_ptr<game::ExtensionChain> extensions();
    // What the plugins give the Lua UI: their windows' folders, functions, options and menu entries.
    plugin::UiBridge& plugin_ui();
    // The script host (the Lua socials run there too) and MQ's TLOs and types.
    mq::ScriptHost& scripts();
    const mq::Registry& registry() const;
    const mq::Context& context(); // what the TLOs read now
    // An owner's capabilities by its tag (plugin::UiBridge::owner_tag): "plugin:<name>" a compiled plugin's while it
    // is loaded, "script:<name>" a running Lua plugin's; 0 otherwise (a bare name too). Its windows in the UI pack act
    // with them (LuaUi::caller_tag).
    std::uint32_t plugin_caps(std::string_view tag) const;
    // A browser tab's owner (plugin/web_access.h) is still there: "script:<name>" running, "plugin:<name>" loaded;
    // "" (the player) always.
    bool web_owner_alive(const std::string& owner) const;
    // The installed add-ons' capability ceilings (docs/addons.md, app/addon_install.h addon_ceilings): what runs is
    // checked again against them.
    void set_addon_ceilings(std::vector<plugin::Ceiling> ceilings);
    // The server's compiled plugins waiting for the player's word (plugin/consent.h, docs/plugins.md "The server's
    // compiled plugins ask first"), with the server's name; nullopt none. The UI packs show it (eq.PluginConsent),
    // else imgui() does.
    std::optional<plugin::ConsentPrompt> plugin_consent() const;
    // true: allowed (they load now), false: refused, both remembered for these bytes; nullopt: put away for now.
    void answer_plugin_consent(std::optional<bool> allow);
    // A running script holds the Ability Book's autoskill off (hailstone.hold_autoskill); a stopped one's hold goes.
    bool autoskill_held();
    // Once a frame after the connection's update: the server's rules, OnPulse, the scripts.
    void frame();
    // Inside the ImGui frame: the scripts' and plugins' windows, the server plugins' question when no UI pack window
    // stands in for it ("PluginConsent", eq.ReplaceWindow), and with `dev` the "Plugins & Lua" window.
    void imgui(bool dev);
    // Before the connection goes: the plugins unload, the scripts stop.
    void shutdown();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace hailstone::app
