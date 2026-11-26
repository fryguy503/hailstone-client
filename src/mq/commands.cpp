#include "mq/commands.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <iterator>
#include <utility>

#include "game/coords.h"
#include "game/doors.h"
#include "game/world_state.h"
#include "mq/nav_commands.h"
#include "mq/map_filter.h"
#include "mq/parse.h"
#include "mq/spawn_search.h"
#include "mq/typed_escapes.h"
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
    if (!env_.map_filters) {
        own_map_filters_ = std::make_unique<MapFilters>();
        env_.map_filters = own_map_filters_.get();
    }
    plugin::Hooks& h = *env_.hooks;
    owner_ = h.new_owner("mq");
    h.on_command_line(owner_, [this](std::string& line) {
        plugin::Flow flow = plugin::Flow::Continue;
        filter(line, flow);
        return flow;
    });
    // /squelch: the chat lines its command prints are dropped.
    h.on_chat(owner_, [this](plugin::ChatLine&) { return squelch_ ? plugin::Flow::Drop : plugin::Flow::Continue; });

    // /echo (MQ's Echo): the line with MQ's typed escapes as colour codes (mq/typed_escapes.h; a line no script sent
    // loses the BEL a ${...} may have brought in first).
    h.add_command(owner_, "/echo", [this](std::string_view args) {
        say_coded(typed_line(args, !plugin::caller_name().empty()));
    });
    h.add_command(owner_, "/mapfilter", [this](std::string_view args) { map_filter(args); }, plugin::kCapCommands);
    h.add_command(owner_, "/noparse", [this](std::string_view args) {
        const bool before = std::exchange(noparse_, true);
        run_nested(args);
        noparse_ = before;
    });
    h.add_command(owner_, "/docommand", [this](std::string_view args) { run_nested(args); });
    // /invoke ${Window[...].DoOpen} (MQ's InvokeCmd sets bRunNextCommand and nothing else): the line's ${...} were
    // read before the command got it, and reading a window method is what acts (Window's LeftMouseUp, DoOpen ...).
    h.add_command(owner_, "/invoke", [](std::string_view) {});
    // /timed <deciseconds> <command> (MQ's DoTimedCmd): the command runs that many tenths of a second later, as
    // whoever sent it (a script's line stays held to its permissions). As in MQ the line is not ${}-expanded when it
    // is sent (filter skips it) but when it runs, so its values are those of that moment. The delay is MQ's
    // GetIntFromString: a number's whole part, anything else 0 (the next pulse). Kept across zoning, as MQ keeps it.
    h.add_command(owner_, "/timed", [this](std::string_view args) {
        std::string_view rest = trim(args);
        if (rest.empty()) {
            say("Usage: /timed <deciseconds> <command>");
            return;
        }
        const std::string n = first_word(rest);
        rest = trim(rest.substr(std::min(rest.size(), n.size())));
        if (rest.empty()) return;
        double d = 0;
        if (!parse_float(n, d) || !std::isfinite(d) || d < 0) d = 0;
        d = std::min(std::trunc(d), 1e9); // whole tenths; about 3 years at most
        if (timed_.size() >= kMaxTimedCommands) {
            say(std::format("[MQ] /timed holds at most {} commands at once: this one is dropped", kMaxTimedCommands));
            return;
        }
        timed_.push_back({now_ms() + static_cast<std::int64_t>(d) * 100, std::string(rest), plugin::caller_caps(),
                          std::string(plugin::caller_name()), plugin::caller_tlo_proof()});
    });
    h.on_pulse(owner_, [this] { run_timed(); });
    // /doortarget [id <n> | <name>] | clear (MQ's DoorTarget): the door with that id, or the nearest (in 3D) whose name
    // holds the text (any door with no text), becomes the switch target ${DoorTarget} reads. As in MQ the old one is
    // dropped first, so a door not found leaves none. MQ's Z filter (gZFilter, off unless set) has no counterpart here.
    h.add_command(owner_, "/doortarget", [this](std::string_view args) {
        if (env_.set_door_target) env_.set_door_target(std::nullopt);
        std::string_view rest = trim(args);
        std::string word = first_word(rest);
        if (iequals(word, "clear")) {
            say("Switch target cleared.");
            return;
        }
        const Context& ctx = env_.context();
        const net::GameConnection* conn = ctx.conn;
        const game::Spawn* me = conn ? conn->world().player() : nullptr;
        if (!conn || !me) return;
        const game::Door* found = nullptr;
        if (iequals(word, "id")) {
            rest = trim(rest.substr(word.size()));
            std::int64_t id = 0;
            if (rest.empty() || !parse_int(first_word(rest), id)) {
                say("[MQ] DoorTarget: id specified but no number provided.");
                return;
            }
            if (id >= 0 && id <= 255) found = conn->door(static_cast<std::uint8_t>(id));
        } else {
            const glm::vec3 at = spawn_position(*me);
            float best = 0;
            for (const game::Door& d : conn->doors()) {
                if (!word.empty() && !icontains(d.name, word)) continue;
                const glm::vec3 to = d.world_position() - at;
                const float dist = to.x * to.x + to.y * to.y + to.z * to.z;
                if (!found || dist < best) {
                    found = &d;
                    best = dist;
                }
            }
        }
        if (!found) {
            say(std::format("[MQ] Couldn't find switch '{}' to target.", rest));
            return;
        }
        if (env_.set_door_target) env_.set_door_target(found->id);
        say(std::format("Switch {} '{}' targeted.", found->id, found->name));
    }, plugin::kCapTarget);
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
            bool bad_number = false; // a heading or loc that is not a finite number: no turn rather than the target
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
                    // "nan", "inf" and 1e300 parse; only a finite heading counts, taken as one turn's degrees
                    double d = 0;
                    if (parse_float(n, d) && std::isfinite(d)) heading = std::fmod(std::fmod(d, 360.0) + 360.0, 360.0);
                    else bad_number = true;
                } else if (iequals(w, "loc")) {
                    // "loc y,x[,z]" in /loc order (the server's y first)
                    std::string n = first_word(rest);
                    rest = trim(rest.substr(std::min(rest.size(), n.size())));
                    auto parts = split_index(n);
                    double y = 0, x = 0;
                    std::optional<float> fy, fx; // (not NaN, an infinity or past float)
                    if (parts.size() >= 2 && parse_float(parts[0], y) && parse_float(parts[1], x)) {
                        fy = in_range<float>(y);
                        fx = in_range<float>(x);
                    }
                    if (fy && fx) loc = game::server_to_world({*fx, *fy, 0.0f});
                    else bad_number = true;
                } else {
                    search += (search.empty() ? "" : " ") + w;
                }
            }
            if (bad_number) return;
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

std::int64_t Commands::now_ms() const {
    if (env_.now_ms) return env_.now_ms();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Commands::run_timed() {
    if (timed_.empty() || !env_.run) return;
    const std::int64_t now = now_ms();
    // Taken out before any runs: a line run here may send /timed again, which adds to timed_.
    std::vector<Timed> due;
    auto keep = std::stable_partition(timed_.begin(), timed_.end(), [now](const Timed& t) { return t.due_ms > now; });
    std::move(keep, timed_.end(), std::back_inserter(due));
    timed_.erase(keep, timed_.end());
    std::stable_sort(due.begin(), due.end(), [](const Timed& a, const Timed& b) { return a.due_ms < b.due_ms; });
    for (const Timed& t : due) {
        if (t.tlo_caller) {
            plugin::CallerScope scope(t.caps, t.caller, *t.tlo_caller);
            env_.run(t.line);
        } else {
            env_.run(t.line);
        }
    }
}

void Commands::map_filter(std::string_view args) {
    const Context& ctx = env_.context();
    MapFilterCommandInput input;
    input.guild = ctx.conn ? &ctx.conn->guild() : nullptr;
    run_map_filter_command(*env_.map_filters, args, input, [this](std::string_view line) { say(std::string(line)); });
}

void Commands::say(const std::string& text) const {
    if (env_.echo && squelch_ == 0) env_.echo(text);
}

void Commands::say_coded(const std::string& text) const {
    if (squelch_ != 0) return;
    if (env_.echo_coded) env_.echo_coded(text);
    else if (env_.echo) env_.echo(text);
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
    // /timed's line is expanded when it runs, not now, and /nomodkey, /shiftkey and /ctrlkey run theirs through the
    // filters again, where it is expanded once (MQ registers all four unparsed).
    const bool unparsed = iequals(word, "/timed") || iequals(word, "/nomodkey") || iequals(word, "/shiftkey") ||
                          iequals(word, "/ctrlkey");
    if (!parsed && !noparse_ && g_literal_lines == 0 && !iequals(word, "/noparse") && !unparsed &&
        line.find("${") != std::string::npos) {
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
