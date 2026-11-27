#pragma once

// MacroQuest's own commands and its command-line handling, on the hook bus (plugin/hooks.h):
//  - every line, typed or run by a script or plugin, has its ${...} replaced (mq/parse.h) unless it is /noparse or
//    the client's own windows send it (LiteralLine);
//  - a line a plugin or script runs is checked against its capabilities (plugin/policy.h caller_caps) and refused
//    with a chat line when the server did not grant them, and checked again as it finally is, after every other line
//    filter (a plugin's command rewrite) has had it;
//  - /echo, /target (id, clear, mycorpse, myself, or a spawn search; next and prev cycle), /face, /keypress,
//    /squelch, /docommand, /noparse;
//  - /nav and /moveto on the client's navigation (mq/nav_commands.h).
// The rest of MQ's commands come from the script host (/lua) and the plugin manager (/plugin).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mq/tlo.h"
#include "plugin/hooks.h"
#include "plugin/policy.h"

namespace hailstone::mq {

class MapFilters;

// A .cfg file to run (app/autoexec.h CfgFile).
struct CfgLoad {
    std::string file;               // its canonical path: a file already running is not run again inside itself
    std::vector<std::string> lines;
    std::string refused;            // not empty: the file does not run, and this chat line says why
};

struct CommandEnv {
    plugin::Hooks* hooks = nullptr;
    const Registry* registry = nullptr;
    MapFilters* map_filters = nullptr; // shared with the map presenter; a private default when a test leaves it null
    std::function<const Context&()> context;
    std::function<void(const std::string& text)> echo;    // a chat line (MQ's WriteChatColor)
    // A chat line the client writes with MQ's colour codes it means to keep (GameConnection::client_message: /echo's
    // line after mq::typed_line); null: echo.
    std::function<void(const std::string& text)> echo_coded;
    std::function<void(const std::string& line)> run;     // a command line through the client (GameConnection)
    std::function<void(std::uint32_t spawn_id)> set_target;
    std::function<void(std::optional<std::uint8_t> door)> set_door_target; // /doortarget; null: none
    std::function<void(double degrees, bool fast)> face;  // MQ degrees: clockwise from north
    // /keypress: an EQ key command by its name ("forward", "jump", "hot1_1" ...), tapped (0), held (1) or let go (2);
    // false when the name is unknown.
    std::function<bool(std::string_view command, int mode)> keypress;
    // /timed's clock, in milliseconds; null: the steady clock.
    std::function<std::int64_t()> now_ms;
    // /loadcfg: the .cfg file a name or path gives (app/autoexec.h load_cfg); nullopt: no such file. `typed`: the
    // player sent the line (no script, plugin or window did), so a path may be taken. Null: /loadcfg finds none.
    std::function<std::optional<CfgLoad>(std::string_view name, bool typed)> load_cfg;
};

// /timed's limit on lines waiting at once: one more is refused with a chat line, so a script that queues without end
// cannot grow the queue without end.
inline constexpr std::size_t kMaxTimedCommands = 1000;

// While one is held, command lines are not ${}-expanded: a window's button sends text the player did not type as a
// command (a guild's URL, channel or MOTD, a member's name: what other players and the server wrote). MQ parses the
// lines typed and the macros' lines; a window's line is neither.
class LiteralLine {
public:
    LiteralLine();
    ~LiteralLine();
    LiteralLine(const LiteralLine&) = delete;
    LiteralLine& operator=(const LiteralLine&) = delete;
};

// Whether a key command is movement (kCapMove) rather than an action (kCapActions) for /keypress.
bool movement_key(std::string_view command);

// How deep /docommand, /noparse, /squelch and /loadcfg may run each other in one line.
inline constexpr int kMaxNestedCommands = 16;

// How many .cfg lines one /loadcfg the player or a script sent, or one batch of files the client runs at a moment
// (Commands::run_cfg_files), may run in all, the lines of the files they /loadcfg in turn included: nesting alone
// would let a file of a few /loadcfg lines run millions.
inline constexpr int kMaxCfgLines = 1000;

class Commands {
public:
    explicit Commands(CommandEnv env);
    ~Commands();
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;

    // .cfg files due at one moment (app/automation.cpp: AutoExec, InGame, zoned ...), in order, as the player's lines,
    // under one budget of kMaxCfgLines. A refused file's chat line is said instead.
    void run_cfg_files(const std::vector<CfgLoad>& files);

private:
    void filter(std::string& line, plugin::Flow& flow);
    // The capabilities `line` needs from a plugin or script; false (with a chat line) when the caller lacks them.
    bool check(const std::string& line);
    // The last line filter: the line as it finally goes, after the plugins' rewrites.
    void final_check(std::string& line, plugin::Flow& flow);
    void say(const std::string& text) const; // a chat line, unless /squelch is running
    void say_coded(const std::string& text) const; // the same, its MQ colour codes kept (CommandEnv::echo_coded)
    void map_filter(std::string_view args);
    // /docommand, /noparse and /squelch: the rest of the line run as a command, at most kMaxNestedCommands deep.
    void run_nested(std::string_view args);
    // /loadcfg (MacroQuest's LoadCfgCommand): each of the file's lines run as a typed line (parsed), one level deeper.
    void load_cfg(std::string_view args);
    // A file's lines, in the current budget; not when the file is already running further out.
    void run_cfg(const CfgLoad& file);
    bool too_deep() const; // kMaxNestedCommands reached: says so
    // /timed: a line waiting for its time, with who sent it (run as them: CallerScope).
    struct Timed {
        std::int64_t due_ms = 0;
        std::string line;
        std::uint32_t caps = 0;
        std::string caller;
        std::optional<plugin::WeakTloCaller> tlo_caller;
    };
    std::int64_t now_ms() const;
    void run_timed(); // each pulse: the lines due, in the order they fall due (sent order for the same time)
    std::vector<Timed> timed_;
    CommandEnv env_;
    std::unique_ptr<MapFilters> own_map_filters_;
    plugin::Hooks::Owner owner_ = 0;
    plugin::Hooks::Owner final_owner_ = 0; // final_check's, registered again so it stays the last filter
    bool redispatched_ = false;            // the next line through filter() is the one it sent on itself
    bool nested_parsed_ = false;           // the next line through filter() is run_nested's: parsed with its outer line
    bool noparse_ = false;
    int squelch_ = 0;
    int nested_ = 0; // run_nested calls in progress
    std::vector<std::string> cfg_stack_; // the .cfg files running, outermost first
    int cfg_budget_ = 0;                 // lines the outermost /loadcfg or batch may still run
    bool cfg_budget_said_ = false;       // its end has been said
};

} // namespace hailstone::mq
