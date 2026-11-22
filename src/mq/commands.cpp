#include "mq/commands.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "game/coords.h"
#include "game/world_state.h"
#include "mq/nav_commands.h"
#include "mq/parse.h"
#include "mq/spawn_search.h"
#include "net/game_connection.h"
#include "plugin/policy.h"

namespace hailstone::mq {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// /target's own spaces: a tab ends its words too, as in a spawn search. Other commands keep a tab as part of a word
// (/keypress "hold\t" is no hold, but a tap).
std::string_view trim_target(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Whether one of the words of `s` (split at a space or a tab) is `word`, case-insensitively.
bool has_word(std::string_view s, std::string_view word) {
    while (!s.empty()) {
        const std::size_t start = s.find_first_not_of(" \t");
        if (start == std::string_view::npos) return false;
        s.remove_prefix(start);
        const std::size_t end = s.find_first_of(" \t");
        if (iequals(s.substr(0, end), word)) return true;
        if (end == std::string_view::npos) return false;
        s.remove_prefix(end);
    }
    return false;
}

std::string first_word(std::string_view line) {
    line = trim(line);
    return std::string(line.substr(0, line.find(' ')));
}

// Clockwise-from-north degrees from a to b (world coordinates; mq/tlo_game.cpp has the same).
double heading_between(const glm::vec3& a, const glm::vec3& b) {
    glm::vec3 sa = game::world_to_server(a), sb = game::world_to_server(b);
    double deg = std::atan2(-(sb.x - sa.x), sb.y - sa.y) * 180.0 / 3.14159265358979323846;
    return deg < 0 ? deg + 360.0 : deg;
}

// An untyped /target with a name follows RoF2's choice among the matches, not MQ's (its Target takes the nearest).
// RoF2's /target (Send_OP_TargetCommand_004e9790) calls GetNearestSpawnMaybe 0x5a04e0 twice, for players only (spawn
// Type 0) and for any type, and each call first asks PlayerManagerClient::GetSpawnByName 0x599b40 for the exact name
// (case-insensitive, any type), so an exact name wins outright: a rider over the mount EQEmu names after them.
// `found` is nearest first, the lower id at an equal distance (RoF2 keeps its spawn list's first there: docs/unknowns.md).
// Which spawns match is still MQ's (a part of the name, not RoF2's prefix).
const game::Spawn* rof2_target_choice(const std::vector<const game::Spawn*>& found, std::string_view name,
                                      const game::WorldState& world) {
    // The display form ("#Lord_Bob00" as "Lord Bob") is Hailstone's: RoF2 compares the internal name only.
    for (const game::Spawn* s : found)
        if (iequals(s->name, name) || game::display_name_iequals(s->name, name)) return s;
    // No exact name: the nearest player, unless the nearest match of any type is 50 or more units nearer (3D, from
    // the local player; CalcDistanceBetweenSpawns 0x5941d0, compared after a square root at 0x4e97fc).
    // A deliberate change: RoF2 counts the local player too, but MQ's part-of-a-name match would find your own name
    // far more often than RoF2's prefix does, so the player is skipped while there is another match, as MQ does.
    const game::Spawn *nearest = nullptr, *player = nullptr;
    for (const game::Spawn* s : found) {
        if (s->id == world.player_id && found.size() > 1) continue;
        if (!nearest) nearest = s;
        if (!player && !s->is_npc && !s->is_corpse) player = s; // RoF2's Type 0: a PC, not a corpse, pet or merc
    }
    const game::Spawn* me = world.player();
    if (!player || !me) return player ? player : nearest;
    const glm::vec3 from = spawn_position(*me);
    if (glm::distance(from, spawn_position(*nearest)) + 50.0f <= glm::distance(from, spawn_position(*player)))
        return nearest;
    return player;
}

bool allowed(std::uint32_t need, const CommandEnv& env, std::string_view what) {
    std::uint32_t have = plugin::caller_caps();
    if ((have & need) == need) return true;
    if (env.echo)
        env.echo(std::format("[MQ] {}: {} needs the server's '{}' permission", plugin::caller_name(), what,
                             plugin::caps_text(need & ~have)));
    return false;
}

int g_literal_lines = 0; // LiteralLine scopes held

} // namespace

LiteralLine::LiteralLine() { ++g_literal_lines; }
LiteralLine::~LiteralLine() { --g_literal_lines; }

bool movement_key(std::string_view c) {
    for (std::string_view m : {"forward", "back", "left", "right", "strafe_left", "strafe_right", "jump", "duck",
                               "run_walk", "autorun", "up", "down", "pitchup", "pitchdown", "center_view",
                               "look_up", "look_down", "turn_left", "turn_right"})
        if (iequals(c, m)) return true;
    return false;
}

Commands::Commands(CommandEnv env) : env_(std::move(env)) {
    plugin::Hooks& h = *env_.hooks;
    owner_ = h.new_owner("mq");
    h.on_command_line(owner_, [this](std::string& line) {
        plugin::Flow flow = plugin::Flow::Continue;
        filter(line, flow);
        return flow;
    });
    // /squelch: the chat lines its command prints are dropped.
    h.on_chat(owner_, [this](plugin::ChatLine&) { return squelch_ ? plugin::Flow::Drop : plugin::Flow::Continue; });

    h.add_command(owner_, "/echo", [this](std::string_view args) { say(std::string(args)); });
    h.add_command(owner_, "/noparse", [this](std::string_view args) {
        const bool before = std::exchange(noparse_, true);
        run_nested(args);
        noparse_ = before;
    });
    h.add_command(owner_, "/docommand", [this](std::string_view args) { run_nested(args); });
    h.add_command(owner_, "/squelch", [this](std::string_view args) {
        ++squelch_;
        run_nested(args);
        --squelch_;
    });
    h.add_command(
        owner_, "/target",
        [this](std::string_view args) {
            const Context& ctx = env_.context();
            if (!ctx.conn || !env_.set_target) return;
            args = trim_target(args);
            const game::WorldState& world = ctx.conn->world();
            // MQ: /target with nothing, or with "myself" among its words, targets the player (MQCommands.cpp
            // Target: myself anywhere wins over the rest of the search). Words split at a space or a tab, as in a
            // spawn search.
            if (args.empty() || has_word(args, "myself")) {
                env_.set_target(world.player_id);
                return;
            }
            if (iequals(args, "clear")) {
                env_.set_target(0);
                return;
            }
            if (iequals(args, "mycorpse")) {
                std::string mine = ctx.conn->character_name() + "'s corpse";
                SpawnSearch q = parse_spawn_search("pccorpse");
                for (const game::Spawn* s : search_spawns(q, world, sources_of(*ctx.conn)))
                    if (icontains(s->name, ctx.conn->character_name())) {
                        env_.set_target(s->id);
                        return;
                    }
                say("There are no spawns matching: (" + mine + ")");
                return;
            }
            SearchSources sources = sources_of(*ctx.conn);
            sources.can_see = ctx.can_see;
            const SpawnSearch search = parse_spawn_search(args, sources.guild);
            const auto found = search_spawns(search, world, sources);
            // RoF2's choice is a plain /target's; next / prev ask for cycling, which it cannot answer.
            if (search.direction == SpawnSearch::Direction::Nearest && search.kind == SpawnSearch::Kind::Any &&
                !search.id && !search.name.empty()) {
                if (const game::Spawn* s = rof2_target_choice(found, search.name, world)) {
                    env_.set_target(s->id);
                    return;
                }
            } else if (const game::Spawn* s = pick_target(search, found, world)) {
                env_.set_target(s->id);
                return;
            }
            say(std::format("There are no spawns matching: ({})", args));
        },
        plugin::kCapTarget);
    h.add_command(
        owner_, "/face",
        [this](std::string_view args) {
            const Context& ctx = env_.context();
            if (!ctx.conn || !env_.face) return;
            const game::WorldState& world = ctx.conn->world();
            const game::Spawn* me = world.player();
            if (!me) return;
            bool fast = false;
            std::string search;
            std::optional<double> heading;
            std::optional<glm::vec3> loc;
            std::string_view rest = trim(args);
            while (!rest.empty()) {
                std::string w = first_word(rest);
                rest = trim(rest.substr(std::min(rest.size(), w.size())));
                if (iequals(w, "fast")) fast = true;
                else if (iequals(w, "nolook") || iequals(w, "predict") || iequals(w, "away")) {
                    // accepted: this client faces flat and does not lead targets
                } else if (iequals(w, "heading")) {
                    std::string n = first_word(rest);
                    rest = trim(rest.substr(std::min(rest.size(), n.size())));
                    double d = 0;
                    if (parse_float(n, d)) heading = d;
                } else if (iequals(w, "loc")) {
                    // "loc y,x[,z]" in /loc order (the server's y first)
                    std::string n = first_word(rest);
                    rest = trim(rest.substr(std::min(rest.size(), n.size())));
                    auto parts = split_index(n);
                    double y = 0, x = 0;
                    if (parts.size() >= 2 && parse_float(parts[0], y) && parse_float(parts[1], x))
                        loc = game::server_to_world({static_cast<float>(x), static_cast<float>(y), 0.0f});
                } else {
                    search += (search.empty() ? "" : " ") + w;
                }
            }
            if (heading) {
                env_.face(*heading, fast);
                return;
            }
            glm::vec3 to;
            if (loc) {
                to = *loc;
            } else {
                const game::Spawn* s = nullptr;
                if (search.empty()) {
                    auto it = world.spawns.find(world.target_id);
                    if (it != world.spawns.end()) s = &it->second;
                } else {
                    for (const game::Spawn* f : find_spawns(search, *ctx.conn, ctx.can_see))
                        if (f->id != world.player_id) {
                            s = f;
                            break;
                        }
                }
                if (!s || s->id == world.player_id) return;
                to = spawn_position(*s);
            }
            env_.face(heading_between(spawn_position(*me), to), fast);
        },
        plugin::kCapMove);
    h.add_command(owner_, "/keypress", [this](std::string_view args) {
        std::string_view rest = trim(args);
        std::string key = first_word(rest);
        std::string mode = std::string(trim(rest.substr(std::min(rest.size(), key.size()))));
        if (key.empty()) {
            say("Usage: /keypress <eqcommand|key> [hold|release]");
            return;
        }
        if (!allowed(movement_key(key) ? plugin::kCapMove : plugin::kCapActions, env_, "/keypress " + key)) return;
        int m = iequals(mode, "hold") ? 1 : iequals(mode, "release") ? 2 : 0;
        if (!env_.keypress || !env_.keypress(key, m))
            say("Invalid key name: " + key);
    });
    add_nav_commands(h, owner_, env_, [this](const std::string& t) { say(t); });
}

// Each level is a few frames of the native stack (and two passes through the hooks for a script's line), so a line
// that nests them without end ("/docommand /docommand ...") is cut off here rather than overflowing it.
void Commands::run_nested(std::string_view args) {
    if (!env_.run) return;
    if (nested_ >= kMaxNestedCommands) {
        if (env_.echo)
            env_.echo(std::format("[MQ] /docommand, /noparse and /squelch nest at most {} deep: the rest of the line is "
                                  "dropped",
                                  kMaxNestedCommands));
        return;
    }
    // The outer line was parsed (or left as written) before its command got it: its rest is not parsed again, or a
    // value holding "${" would be read after all (docs/plugins.md "MQ commands"). A one-line flag rather than
    // noparse_, so that a bind's own mq.cmd lines under /docommand are still parsed.
    ++nested_;
    nested_parsed_ = true;
    env_.run(std::string(args));
    nested_parsed_ = false; // a run that did not reach the filters
    --nested_;
}

void Commands::say(const std::string& text) const {
    if (env_.echo && squelch_ == 0) env_.echo(text);
}

Commands::~Commands() {
    if (env_.hooks) {
        env_.hooks->remove(owner_);
        if (final_owner_) env_.hooks->remove(final_owner_);
    }
}

void Commands::filter(std::string& line, plugin::Flow& flow) {
    // The line this filter sent on itself (below): parsed and checked already; final_check has it last.
    const bool parsed = std::exchange(nested_parsed_, false);
    if (std::exchange(redispatched_, false)) return;
    std::string word = first_word(line);
    // ${...} (a read: scripts and plugins need the read permission) unless /noparse is running the line or a window
    // sends it (LiteralLine).
    if (!parsed && !noparse_ && g_literal_lines == 0 && !iequals(word, "/noparse") && line.find("${") != std::string::npos) {
        if ((plugin::caller_caps() & plugin::kCapRead) == 0) {
            if (env_.echo) env_.echo(std::format("[MQ] {}: ${{...}} needs the server's 'read' permission", plugin::caller_name()));
            flow = plugin::Flow::Drop;
            return;
        }
        line = parse_macro_data(env_.context(), line);
    }
    if (plugin::caller_caps() == plugin::kCapAll) return; // the player
    if (!check(line)) {
        flow = plugin::Flow::Drop;
        return;
    }
    // The filters after this one (a compiled plugin's command rewrite, registered as it loads) may make the line
    // another command, which the script or plugin may not run. So the line goes through the filters again, from the
    // start, with final_check placed after all of them, and this pass ends here.
    if (!env_.run || !env_.hooks) return;
    if (final_owner_) env_.hooks->remove(final_owner_);
    final_owner_ = env_.hooks->new_owner("mq");
    env_.hooks->on_command_line(final_owner_, [this](std::string& l) {
        plugin::Flow f = plugin::Flow::Continue;
        final_check(l, f);
        return f;
    });
    redispatched_ = true;
    env_.run(line);
    redispatched_ = false; // a run that did not reach the filters
    flow = plugin::Flow::Drop;
}

void Commands::final_check(std::string& line, plugin::Flow& flow) {
    if (plugin::caller_caps() == plugin::kCapAll) return;
    if (!check(line)) flow = plugin::Flow::Drop;
}

bool Commands::check(const std::string& line) {
    const std::string word = first_word(line);
    std::uint32_t need = 0;
    if (env_.hooks->has_command(word)) {
        need = env_.hooks->command_required(word);
    } else {
        const Context& ctx = env_.context();
        need = plugin::command_caps(line, ctx.conn ? &ctx.conn->strings() : nullptr);
    }
    return allowed(need, env_, word.empty() ? std::string("text") : word);
}

} // namespace hailstone::mq
