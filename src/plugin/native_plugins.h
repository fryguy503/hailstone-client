#pragma once

// Compiled plugins (sdk/hailstone_plugin.h): shared libraries in the server's plugins/ folder and the player's plugin
// directories, loaded when the server's rules allow them (plugin/policy.h): the server's own always load, the
// player's when its hailstone.cfg lets them (or it has none). Loading copies the exact bytes that were hashed to the cache
// directory and loads the copy, so the file cannot change between the check and the load. A loaded plugin's MQ
// callbacks are hooked onto the bus (plugin/hooks.h), each run with the plugin's capabilities; a callback it may not
// use is left unhooked with a note in the chat window. /plugin list | /plugin <name> [load|unload].
// A plugin with HAILSTONE_CAP_EXTEND may also be a server extension (ABI 2): its hook table (HailstoneHooks), switches and
// vitals join env.extensions as one member, asked while the plugin says it is active.
// A library from the server's plugins/ folder also needs the player's word for its bytes (plugin/consent.h): until
// then it does not load, and consent() holds the question for the UI; answer_consent() takes the answer.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/packet.h"
#include "plugin/consent.h"
#include "plugin/hooks.h"
#include "plugin/policy.h"

struct HailstoneBuiltinAddon;

namespace hailstone::mq { class Registry; struct Context; }
namespace hailstone::net { class GameConnection; }
namespace hailstone::game { class ExtensionChain; }
namespace hailstone::plugin { class UiBridge; class WebAccess; }

namespace hailstone::plugin {

struct NativeEnv {
    Hooks* hooks = nullptr;
    const Policy* policy = nullptr;
    mq::Registry* registry = nullptr;
    std::function<const mq::Context&()> context;
    std::function<void(const std::string& text, std::uint32_t color)> print; // a chat line
    std::function<void(const std::string& line)> run;                        // a command line (through the hooks)
    std::function<void(Stream stream, const net::AppPacket& p)> send;        // a packet to the server
    std::function<void*()> imgui_context;
    // ABI 2: the chain a plugin's server extension joins (game/server_ext_chain.h; each connection asks it), and the
    // connection its setters change (class masks, the pet list). Either may be null.
    game::ExtensionChain* extensions = nullptr;
    std::function<net::GameConnection*()> connection;
    // ABI 2: where a plugin's Lua windows, their functions, options and menu entries go (plugin/ui_bridge.h); null:
    // none.
    UiBridge* ui = nullptr;
    // ABI 2: the camera's zoom limit (max_camera_distance / set_max_camera_distance; setting it cuts the zoom to it).
    // Empty: 0, and setting does nothing.
    std::function<float()> max_camera_distance;
    std::function<void(float)> set_max_camera_distance;
    // ABI 3: the in-game browser (web_*; plugin/web_access.h): the plugin's own tabs, as "plugin:<name>". Null: none.
    WebAccess* web = nullptr;
    // ABI 2: the hot buttons (hot_button / set_hot_button / hot_bar_page / set_hot_bar_page): a button's ini string
    // (nullopt out of range or none loaded), set one ("" clears; false not done), a bar's page (-1 none). Empty: none.
    std::function<std::optional<std::string>(int bar, int page, int button)> hot_button;
    std::function<bool(int bar, int page, int button, const std::string& spec)> set_hot_button;
    std::function<int(int bar)> hot_bar_page;
    std::function<void(int bar, int page)> set_hot_bar_page;
    std::vector<std::filesystem::path> dirs; // where plugins are looked for, and one level down (plugins/<name>/)
    std::filesystem::path cache_dir;         // the approved copies
    std::filesystem::path config_dir;        // each plugin's own directory is config_dir / name
    // The per-server and per-character folders (server_config_dir, character_config_dir) are under the connection's:
    // the server's rules' folder (servers/<profile>/) and its character's files.
};

class NativePlugins {
public:
    explicit NativePlugins(NativeEnv env);
    ~NativePlugins(); // unloads everything (ShutdownPlugin)
    NativePlugins(const NativePlugins&) = delete;
    NativePlugins& operator=(const NativePlugins&) = delete;

    struct Info {
        std::string name;
        std::filesystem::path file;
        std::string sha256;
        bool loaded = false;
        std::uint32_t caps = 0;     // what the rules grant (0: it may not run)
        bool server = false;        // one of the server's own
        bool kept = false;          // the server's under its hailstone.cfg: the player cannot unload it
        bool builtin = false;       // linked into the client, never loaded from a user or server folder
        bool asks = false;          // from the server's plugins/ folder: it loads only with the player's word
        Consent consent = Consent::Unasked; // that word, for these bytes (asks only)
        std::string error;
    };
    // The plugin files in the directories, with their hashes and what the policy says.
    std::vector<Info> list() const;
    // A plugin from the server's folder without the player's yes for its bytes does not load: it joins the
    // question (consent()) and load answers false. Only the player's own load (no plugin or script running:
    // plugin::caller_name() empty) asks; one from a script's or plugin's /plugin <name> load never shows the
    // question, so a script cannot put it up again after a "Don't allow" or after the player put it away.
    bool load(std::string_view name, std::string* why = nullptr);
    bool load_builtin(const HailstoneBuiltinAddon& addon, const std::filesystem::path& trusted_ui_dir = {},
                      std::string* why = nullptr);
    bool unload(std::string_view name);
    void unload_all();
    bool loaded(std::string_view name) const;
    // Whether a compiled plugin has this name (any case): loaded, or a file in the folders (the server's too). Scans
    // the folders without hashing anything.
    bool has_plugin(std::string_view name) const;
    // A loaded plugin's capabilities, by its exact name; 0 when none of that name is loaded. Cheap (no folder scan or
    // hashing, unlike list()): the Lua UI asks it at every switch to a plugin's code, many times a frame.
    std::uint32_t loaded_caps(std::string_view name) const;

    // After the server's rules changed: unloads what they no longer allow; loads the server's own plugins and the
    // allowed ones `autoload` names ("*" all) that are not loaded yet.
    void policy_changed(const std::vector<std::string>& autoload);
    // The server's plugins/ folder (servers/<profile>/plugins), looked in before env.dirs; empty for none.
    void set_server_dir(const std::filesystem::path& dir);
    // Inside the ImGui frame: OnUpdateImGui.
    void imgui();

    // The server's plugins waiting for the player's answer (server left empty: the caller knows its name), nullopt
    // none. Asked by the automatic loads for plugins never answered, and by the player's load() (/plugin <name> load
    // typed, the developer window) also for one the player said no to.
    std::optional<ConsentPrompt> consent() const;
    // The player's answer: true allows these bytes on this server and loads them, false keeps them unloaded, both
    // remembered (servers/<profile>/plugin_approvals.txt); nullopt (the prompt put away) only clears the question,
    // which comes back at the next zone-in.
    void answer_consent(std::optional<bool> allow);
    // The answers kept in the file again (another Hailstone of a box set answered the same question): a question
    // they settle goes. Cheap enough for once a second.
    void recheck_consent();

    struct Impl;

private:
    // Who loads: the player (asks, after a "Don't allow" too), the rules' automatic loads (ask only about a plugin
    // never answered) or a plugin or script (never asks).
    enum class Asker : std::uint8_t { Player, Rules, Script };
    bool load_file(std::string_view name, std::string* why, Asker asker);

    std::unique_ptr<Impl> impl_;
};

// The plugin name for a file: its stem without a "lib" prefix ("libMQ2Foo.so" → "MQ2Foo").
std::string plugin_name(const std::filesystem::path& file);

} // namespace hailstone::plugin
