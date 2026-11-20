#pragma once

// MacroQuest's own commands and its command-line handling, on the hook bus (plugin/hooks.h):
//  - every line, typed or run by a script or plugin, has its ${...} replaced (mq/parse.h) unless it is /noparse or
//    the client's own windows send it (LiteralLine);
//  - a line a plugin or script runs is checked against its capabilities (plugin/policy.h caller_caps) and refused
//    with a chat line when the server did not grant them, and checked again as it finally is, after every other line
//    filter (a plugin's command rewrite) has had it;
//  - /echo, /target (id, clear, mycorpse, or a spawn search), /face, /keypress, /squelch, /docommand, /noparse;
//  - /nav and /moveto on the client's navigation (mq/nav_commands.h).
// The rest of MQ's commands come from the script host (/lua) and the plugin manager (/plugin).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "mq/tlo.h"
#include "plugin/hooks.h"

namespace hailstone::mq {

struct CommandEnv {
    plugin::Hooks* hooks = nullptr;
    const Registry* registry = nullptr;
    std::function<const Context&()> context;
    std::function<void(const std::string& text)> echo;    // a chat line (MQ's WriteChatColor)
    std::function<void(const std::string& line)> run;     // a command line through the client (GameConnection)
    std::function<void(std::uint32_t spawn_id)> set_target;
    std::function<void(double degrees, bool fast)> face;  // MQ degrees: clockwise from north
    // /keypress: an EQ key command by its name ("forward", "jump", "hot1_1" ...), tapped (0), held (1) or let go (2);
    // false when the name is unknown.
    std::function<bool(std::string_view command, int mode)> keypress;
};

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

// How deep /docommand, /noparse and /squelch may run each other in one line.
inline constexpr int kMaxNestedCommands = 16;

class Commands {
public:
    explicit Commands(CommandEnv env);
    ~Commands();
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;

private:
    void filter(std::string& line, plugin::Flow& flow);
    // The capabilities `line` needs from a plugin or script; false (with a chat line) when the caller lacks them.
    bool check(const std::string& line);
    // The last line filter: the line as it finally goes, after the plugins' rewrites.
    void final_check(std::string& line, plugin::Flow& flow);
    void say(const std::string& text) const; // a chat line, unless /squelch is running
    // /docommand, /noparse and /squelch: the rest of the line run as a command, at most kMaxNestedCommands deep.
    void run_nested(std::string_view args);
    CommandEnv env_;
    plugin::Hooks::Owner owner_ = 0;
    plugin::Hooks::Owner final_owner_ = 0; // final_check's, registered again so it stays the last filter
    bool redispatched_ = false;            // the next line through filter() is the one it sent on itself
    bool nested_parsed_ = false;           // the next line through filter() is run_nested's: parsed with its outer line
    bool noparse_ = false;
    int squelch_ = 0;
    int nested_ = 0; // run_nested calls in progress
};

} // namespace hailstone::mq
