#pragma once

// MacroQuest Lua scripts, built in: /lua run <name> [args] runs lua/<name>.lua (or lua/<name>/init.lua) with MQ's
// `mq` module, so scripts written for MacroQuest run here: mq.TLO.Me.PctHPs(), mq.cmd / mq.cmdf, mq.delay(ms|'2s'
// [, condition]), mq.event / mq.doevents, mq.bind, mq.imgui.init with MQ's ImGui table, mq.parse, mq.pickle,
// mq.getAllSpawns, print / printf, require of the script's own modules. Only Lua scripts: MQ's old .mac macros are not
// supported.
//
// Every script runs in its own Lua state as a coroutine that the client resumes once a frame; mq.delay yields, and a
// script that runs too long without yielding is paused until the next frame (MQ's turbo). The server decides what
// runs (plugin/policy.h, its hailstone.cfg): a script's file, and every module it requires, must be one of the server's
// own (by SHA-256) or the player's when the server allows those, and the script may do only what the rules grant
// (TLO reads, commands, targeting, actions, movement, chat, chat events, windows). The server's scripts are looked
// for first (set_server_dir); those its hailstone.cfg names start at each zone-in, and the player cannot stop them.
// When the rules change, scripts they no longer allow stop.
//
// The scripts get no io or os.execute: files only through mq.pickle (writes under the config directory) and
// loadfile / dofile of data files there (run with an empty environment).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/ability_book.h"
#include "game/lua_social.h"
#include "mq/tlo.h"
#include "plugin/hooks.h"
#include "plugin/policy.h"

namespace hailstone::game { class ExtensionChain; class NameplateCanvasRegistry; }
namespace hailstone::net { struct AppPacket; class GameConnection; }
namespace hailstone::plugin { class UiBridge; class WebAccess; }

namespace hailstone::mq {

// An installed, explicitly managed Lua add-on. The installer supplies its exact entry and recorded bytes;
// a disabled or unverified record remains present so an ordinary autorun cannot bypass activation.
struct ManagedScript {
    std::string name;
    std::filesystem::path entry;
    Sha256Digest hash{}, revision{};
    bool enabled = false, verified = false;
};

struct ScriptEnv {
    plugin::Hooks* hooks = nullptr;
    const plugin::Policy* policy = nullptr;
    const Registry* registry = nullptr;
    std::function<const Context&()> context;
    std::function<void(const std::string& text)> print; // a chat line
    std::function<void(const std::string& line)> run;   // a command line through the client (with the hooks)
    std::vector<std::filesystem::path> dirs;            // where scripts are looked for, in order
    std::filesystem::path config_dir;                   // mq.configDir: mq.pickle and data loadfile
    std::function<std::int64_t()> now_ms;               // the clock mq.delay and mq.gettime read (null: steady)
    // Scripts as plugins (the hailstone module, mq/lua_hailstone.cpp; docs/plugins.md "Lua plugins"): the chain a
    // script's server extension joins, where its windows, functions, options and menu entries go, and packets to the
    // server. Any may be null.
    game::ExtensionChain* extensions = nullptr;
    game::NameplateCanvasRegistry* nameplates = nullptr;
    plugin::UiBridge* ui = nullptr;
    std::function<void(plugin::Stream stream, const net::AppPacket& p)> send;
    std::function<net::GameConnection*()> connection; // what the setters change (class masks, pets)
    // The camera's zoom limit (hailstone.max_camera_distance / set_max_camera_distance); either may be empty.
    std::function<float()> max_camera_distance;
    std::function<void(float)> set_max_camera_distance;
    // The in-game browser (mq.web; plugin/web_access.h): the script's own tabs, as "script:<name>". Null: none.
    plugin::WebAccess* web = nullptr;
    // Whether a compiled plugin (installed, loaded or the add-on manager's) has this name, any case: such a script's
    // hailstone.server_dir and character_dir are not the plugin's plugin-config/<name>/ (lua_hailstone.cpp). Asked
    // once a script; null: none has.
    std::function<bool(std::string_view name)> plugin_named;
    // The Ability Book's autoskill (game/ability_book.h): a script holds it off while it runs
    // (hailstone.hold_autoskill; a stopped script's hold goes), and reads the player's list with each ability's
    // name (hailstone.autoskill_list). Either may be empty.
    std::function<void(const std::string& script, bool hold)> hold_autoskill;
    std::function<std::vector<std::pair<game::AutoskillRule, std::string>>()> autoskill_list;
    // Uses a skill, discipline or AA as its hot button does (hailstone.use_ability); null: none.
    std::function<bool(AbilityKind, std::string_view name)> use_ability;
    // How long a script may run without a chance to yield before it is stopped, in real time; 0: kRunawayMs (5 s).
    // Only tests set it, so a runaway is stopped in milliseconds.
    std::int64_t runaway_ms = 0;
};

// A Lua social's hot button pressed (game/lua_social.h). The social's Lua sees `button` = {social, bar, button,
// shift, ctrl, alt, presses}.
struct ButtonPress {
    int social = -1;  // 0..119
    std::string name; // the social's name, for messages
    game::LuaSocial lua;
    int bar = -1, button = -1; // the hot button (-1: the Actions window, a command)
    bool shift = false, ctrl = false, alt = false;
    int state = -1; // the Lua state to use when not the social's own (>= 1000: an editor's, for unsaved code)
};

// What a social's `face` returned: how its button looks.
struct ButtonFace {
    bool has = false;                  // the face ran and returned something
    std::string label;                 // replaces the name when not empty
    std::optional<std::uint32_t> color; // the label's 0xRRGGBB
    float cooldown = 0.0f;             // 0..1: the part of a cooldown sweep left
    std::string overlay;               // a short text in the corner (a count)
    bool enabled = true;               // false: drawn dimmed
    std::string tooltip;
    std::string error;                 // the face's last error
};

struct ButtonDryRun {
    bool ok = false;
    std::vector<std::string> lines; // the commands it would send (with notes) and waits, in order
    std::string error;
    plugin::ButtonRules rules;
};

class ScriptHost {
public:
    explicit ScriptHost(ScriptEnv env);
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // /lua run: false with the reason when the script cannot start.
    bool run(const std::string& name, const std::vector<std::string>& args, std::string* why = nullptr);
    bool stop(std::string_view name);
    void stop_all();
    // Pauses or resumes (toggles when `paused` is unset).
    bool pause(std::string_view name, std::optional<bool> paused = {});

    struct Info {
        std::string name;
        std::string status; // "RUNNING", "PAUSED", "EXITED", "ERROR"
        std::uint32_t caps = 0;
        std::string sha256;
        std::string error;
        bool kept = false; // the server's own under its hailstone.cfg: the player cannot stop or pause it
    };
    std::vector<Info> list() const;
    bool running(std::string_view name) const;
    // The capabilities of the script named `name` (exactly) while it runs or is paused, else 0: list()'s caps without
    // building the list (asked at every switch to a script's code in the Lua UI).
    std::uint32_t caps_of(std::string_view name) const;

    // The scripts on disk (the Scripts window): <name>.lua and <name>/init.lua in each folder in the order run()
    // looks (the server's first; a name found twice is the first folder's), config/ left out. Each with whether it
    // may run under the current rules (`why` says why not) and whether it is in the server's folder.
    struct Found {
        std::string name; // lower case, as run() takes it
        std::filesystem::path entry;
        bool server = false;    // in the server's scripts/ folder
        std::uint32_t caps = 0; // 0: it may not run now
        std::string why;        // when caps is 0
    };
    std::vector<Found> found() const;
    // The folders scripts are looked for in, in order (the server's first when there is one).
    std::vector<std::filesystem::path> folders() const;

    // The character's autorun list (Hailstone's own; docs/plugins.md "The Scripts window"): the character's
    // scripts.ini ([Autorun] name = 1); an empty path for none. Its scripts start with hailstone.ini's
    // scripts.autoload ones whenever policy_changed() runs (the zone-in after a login), and at once when a new file
    // is set while the rules allow them. Setting the file it already has does nothing, so a zone change does not
    // start again what the player stopped.
    void set_autorun_file(const std::filesystem::path& file);
    bool autorun(std::string_view name) const;
    // Adds or removes `name` and writes the file; false when there is no file (no character yet) or it cannot be
    // written.
    bool set_autorun(std::string_view name, bool on);
    std::vector<std::string> autorun_list() const;
    bool autorun_available() const; // a character's file is set

    // Once a frame: resumes the scripts.
    void pulse();
    // Once a frame inside the ImGui frame: the scripts' mq.imgui windows.
    void imgui();
    // After the server's rules changed: stops the scripts they no longer allow, updates the others' capabilities,
    // and starts the server's scripts its hailstone.cfg names ([scripts] run) and the allowed ones `autoload` names
    // (hailstone.ini scripts.autoload) or the character's autorun list does.
    void policy_changed(const std::vector<std::string>& autoload = {});
    // Required scripts are running, and no running script is forbidden by the current add-on policy.
    bool required_addons_ready(std::string* problem = nullptr) const;
    void set_managed_addons(std::vector<ManagedScript> addons);
    // The server's scripts/ folder (servers/<profile>/scripts), looked in before env.dirs; empty for none.
    void set_server_dir(const std::filesystem::path& dir);

    // Lua socials. Each social's Lua lives in its own Lua state while its code stays the same, so its globals (a
    // `state` table) last from press to press. press_button runs `press` under the server's rules
    // (plugin::ButtonRules): in press mode to its end at once, in full mode as a coroutine like a script's bind.
    // Pressing a social whose Lua still runs stops it instead. False with the reason when nothing ran.
    bool press_button(const ButtonPress& p, std::string* why = nullptr);
    bool stop_button(int social);
    bool button_running(int social) const;
    // `face` under read-only rights, at most every 200 ms; an empty face when the server does not allow faces.
    const ButtonFace& button_face(int social, const std::string& name, const game::LuaSocial& lua);
    // Runs `press` without sending anything: the commands it would send, waits not waited.
    ButtonDryRun dry_run_button(const ButtonPress& p);
    plugin::ButtonRules button_rules(const game::LuaSocial& lua) const;
    // After a social changed or was cleared: its Lua state goes (-1: every social's).
    void forget_button(int social);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// The hex SHA-256 of a script file (for the developer window).
std::optional<std::string> script_file_hash(const std::filesystem::path& file);
// `relative` (a path inside `root`) as it is spelled on disk: the exact spelling if it exists, otherwise each part's one
// match that differs only in case (two such matches: nullopt). Rooted paths, ".", "..", ':' and NUL are refused. The
// caller checks the file's kind, and that it stays inside its folder once links are followed.
std::optional<std::filesystem::path> script_spelled_path(const std::filesystem::path& root,
                                                         const std::filesystem::path& relative);
// A script's name as one folder name (its mq.configDir under config/, its hailstone.server_dir and character_dir under
// plugin-config/), the same for no two names: bytes no system keeps as written are %XX (lua_mq.cpp).
std::string script_folder_name(std::string_view name);

} // namespace hailstone::mq
