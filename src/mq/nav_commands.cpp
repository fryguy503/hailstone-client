#include "mq/nav_commands.h"

#include "core/distribution.h"

#include <format>
#include <memory>
#include <optional>
#include <vector>

#include "core/ascii_case.h"
#include "game/coords.h"
#include "game/world_state.h"
#include "mq/spawn_search.h"
#include "nav/nav_control.h"
#include "net/game_connection.h"
#include "plugin/policy.h"

namespace hailstone::mq {

namespace {

std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ') ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string join(const std::vector<std::string>& w, std::size_t from) {
    std::string s;
    for (std::size_t i = from; i < w.size(); ++i) s += (s.empty() ? "" : " ") + w[i];
    return s;
}

// A distance or coordinate: nullopt for no number, NaN, an infinity or one beyond float.
std::optional<float> number(const std::vector<std::string>& w, std::size_t i) {
    double d = 0;
    if (i >= w.size() || !parse_float(w[i], d)) return std::nullopt;
    return in_range<float>(d);
}

// A spawn id or door number: nullopt for one no spawn or door can have (negative, too large, NaN).
template <typename T> std::optional<T> id_number(const std::vector<std::string>& w, std::size_t i) {
    double d = 0;
    if (i >= w.size() || !parse_float(w[i], d)) return std::nullopt;
    return in_range<T>(d);
}

const game::Spawn* find_spawn(const net::GameConnection& conn, std::string_view search) {
    const game::WorldState& world = conn.world();
    for (const game::Spawn* s : search_spawns(parse_spawn_search(search), world, &conn.group()))
        if (s->id != world.player_id) return s;
    return nullptr;
}

const char* on_off(bool b) { return b ? "on" : "off"; }

} // namespace

void add_nav_commands(plugin::Hooks& h, plugin::Hooks::Owner owner, const CommandEnv& env,
                      const std::function<void(const std::string&)>& say) {
    if (distribution::managed()) return;
    auto context = env.context;
    auto out = [say](const std::string& t) { say("[Nav] " + t); };

    h.add_command(
        owner, "/nav",
        [context, out](std::string_view args) {
            const Context& ctx = context();
            nav::NavControl* nav = ctx.nav;
            if (!nav) {
                out("Navigation is not available.");
                return;
            }
            // Options anywhere on the line: distance=N (dist=N).
            std::vector<std::string> w;
            float distance = 0.0f;
            for (std::string& t : words(args)) {
                std::string_view tv = t;
                if (istarts_with(tv, "distance=") || istarts_with(tv, "dist=")) {
                    double d = 0;
                    if (parse_float(tv.substr(tv.find('=') + 1), d)) distance = in_range<float>(d).value_or(0.0f);
                } else {
                    w.push_back(std::move(t));
                }
            }
            std::string sub = w.empty() ? std::string("help") : w[0];
            // A word of three letters or more that begins only one subcommand stands for it ("tar" for target).
            {
                static constexpr std::string_view kSubs[] = {"help",   "stop",    "pause",  "reload", "rebuild", "ui",
                                                             "overlay", "status", "option", "options", "id",
                                                             "target", "spawn",   "loc",    "locyxz", "locxyz",
                                                             "locyx",  "door",    "item",   "waypoint", "wp",
                                                             "recordwaypoint"};
                bool exact = false;
                std::string_view only;
                int matches = 0;
                for (std::string_view k : kSubs) {
                    if (iequals(k, sub)) exact = true;
                    else if (sub.size() >= 3 && istarts_with(k, sub)) {
                        only = k;
                        ++matches;
                    }
                }
                if (!exact && matches == 1) sub = std::string(only);
            }
            auto started = [&](const std::string& err, const std::string& what) {
                if (err.empty()) out("Navigating to " + what + ".");
                else out("Cannot navigate to " + what + ": " + err + ".");
            };
            const net::GameConnection* conn = ctx.conn;
            const game::Spawn* me = conn ? conn->world().player() : nullptr;

            if (iequals(sub, "help")) {
                for (const char* line :
                     {"/nav id <#> | target | spawn <search> | loc <y> <x> <z> | locxyz <x> <y> <z> | locyx <y> <x>",
                      "/nav door <name> [click] | door id <#> [click]   (options: distance=<n>)",
                      "/nav stop | pause | status | ui | reload",
                      "/nav option water|lava|pvp|zonelines|doors|follow on|off | clearance <0..6>"})
                    out(line);
                return;
            }
            if (iequals(sub, "stop")) {
                nav->stop("");
                out("Stopped.");
                return;
            }
            if (iequals(sub, "pause")) {
                nav->set_paused(!nav->status().paused);
                out(nav->status().paused ? "Paused." : "Resumed.");
                return;
            }
            if (iequals(sub, "reload") || iequals(sub, "rebuild")) {
                nav->rebuild();
                out("Building this zone's navigation mesh again.");
                return;
            }
            if (iequals(sub, "ui") || iequals(sub, "overlay")) {
                nav->set_overlay(!nav->overlay());
                out(std::string("Overlay ") + on_off(nav->overlay()) + ".");
                return;
            }
            if (iequals(sub, "status")) {
                nav::NavStatus s = nav->status();
                out(std::format("Mesh: {} of {} tiles{}. {}", s.tiles_done, s.tiles_total,
                                s.mesh_complete ? " (complete)" : "",
                                s.active ? std::format("Going to {} ({:.0f} left){}.", s.goal, s.remaining,
                                                       s.paused ? ", paused" : "")
                                         : std::string("Idle.")));
                return;
            }
            if (iequals(sub, "option") || iequals(sub, "options")) {
                nav::PathOptions& o = nav->options();
                if (w.size() >= 3 && iequals(w[1], "clearance")) {
                    auto v = number(w, 2);
                    if (!v || *v < 0.0f || *v > 6.0f) {
                        out("Clearance is a distance from 0 to 6.");
                        return;
                    }
                    o.wall_clearance = *v;
                } else if (w.size() >= 3) {
                    bool on = iequals(w[2], "on") || w[2] == "1" || iequals(w[2], "true");
                    if (iequals(w[1], "water")) o.avoid_water = !on;
                    else if (iequals(w[1], "lava")) o.allow_hazards = on;
                    else if (iequals(w[1], "pvp")) o.avoid_pvp = !on;
                    else if (iequals(w[1], "zonelines")) o.allow_zone_lines = on;
                    else if (iequals(w[1], "doors")) o.open_doors = on;
                    else if (iequals(w[1], "passdoors")) o.pass_open_doors = on;
                    else if (iequals(w[1], "follow")) o.assist_follow = on;
                    else {
                        out("Unknown option: " + w[1]);
                        return;
                    }
                }
                out(std::format("water {}, lava {}, pvp {}, zonelines {}, doors {}, passdoors {}, follow {}, clearance {:g}",
                                on_off(!o.avoid_water), on_off(o.allow_hazards), on_off(!o.avoid_pvp),
                                on_off(o.allow_zone_lines), on_off(o.open_doors), on_off(o.pass_open_doors),
                                on_off(o.assist_follow), o.wall_clearance));
                return;
            }
            if (!conn || !me) {
                out("Not in a zone.");
                return;
            }
            if (iequals(sub, "id") || iequals(sub, "target") || iequals(sub, "spawn")) {
                const game::Spawn* s = nullptr;
                if (iequals(sub, "id")) {
                    auto id = id_number<std::uint32_t>(w, 1);
                    if (id) {
                        auto it = conn->world().spawns.find(*id);
                        if (it != conn->world().spawns.end()) s = &it->second;
                    }
                } else if (iequals(sub, "target")) {
                    auto it = conn->world().spawns.find(conn->world().target_id);
                    if (it != conn->world().spawns.end() && it->first != conn->world().player_id) s = &it->second;
                } else {
                    s = find_spawn(*conn, join(w, 1));
                }
                if (!s) {
                    out("No such spawn.");
                    return;
                }
                started(nav->go_to_spawn(s->id, distance), s->name);
                return;
            }
            if (iequals(sub, "loc") || iequals(sub, "locyxz") || iequals(sub, "locxyz") || iequals(sub, "locyx")) {
                auto a = number(w, 1), b = number(w, 2), c = number(w, 3);
                bool yx = iequals(sub, "locyx");
                if (!a || !b || (!c && !yx)) {
                    out("Usage: /nav " + sub + (iequals(sub, "locxyz") ? " <x> <y> <z>" : yx ? " <y> <x>" : " <y> <x> <z>"));
                    return;
                }
                // /loc order is the server's y, x, z; locxyz is x, y, z.
                float z = c ? *c : game::world_to_server(spawn_position(*me)).z;
                glm::vec3 server = iequals(sub, "locxyz") ? glm::vec3(*a, *b, z) : glm::vec3(*b, *a, z);
                glm::vec3 world = game::server_to_world(server);
                started(nav->go_to_point(world, distance, std::format("{:.0f}, {:.0f}, {:.0f}", server.y, server.x, server.z),
                                         false),
                        std::format("{:.0f}, {:.0f}, {:.0f}", server.y, server.x, server.z));
                return;
            }
            if (iequals(sub, "door")) {
                bool click = !w.empty() && iequals(w.back(), "click");
                if (click) w.pop_back();
                const game::Door* door = nullptr;
                if (w.size() >= 3 && iequals(w[1], "id")) {
                    if (auto id = id_number<std::uint8_t>(w, 2)) door = conn->door(*id);
                } else if (w.size() >= 2) {
                    // The nearest door whose name contains the text.
                    std::string name = join(w, 1);
                    float best = 1e30f;
                    glm::vec3 at = spawn_position(*me);
                    for (const game::Door& d : conn->doors()) {
                        if (!icontains(d.name, name)) continue;
                        float dd = glm::distance(glm::vec3(d.y, d.x, d.z), at);
                        if (dd < best) {
                            best = dd;
                            door = &d;
                        }
                    }
                }
                if (!door) {
                    out("No such door.");
                    return;
                }
                started(nav->go_to_door(door->id, click), door->name);
                return;
            }
            if (iequals(sub, "item") || iequals(sub, "waypoint") || iequals(sub, "wp") || iequals(sub, "recordwaypoint")) {
                out("/nav " + sub + " is not supported yet.");
                return;
            }
            out("Unknown /nav command: " + sub + " (/nav help)");
        },
        plugin::kCapMove);

    // /moveto: a straight walk, no mesh (MQ2MoveUtils). mdist sets the distance it stops at.
    auto mdist = std::make_shared<float>(10.0f);
    h.add_command(
        owner, "/moveto",
        [context, out, mdist](std::string_view args) {
            const Context& ctx = context();
            nav::NavControl* nav = ctx.nav;
            const net::GameConnection* conn = ctx.conn;
            const game::Spawn* me = conn ? conn->world().player() : nullptr;
            if (!nav || !me) return;
            std::vector<std::string> w = words(args);
            if (w.empty() || iequals(w[0], "off") || iequals(w[0], "stop")) {
                nav->stop("");
                return;
            }
            if (iequals(w[0], "mdist")) {
                if (auto d = number(w, 1)) *mdist = std::max(*d, 1.0f);
                out(std::format("/moveto stops within {:.0f}.", *mdist));
                return;
            }
            if (iequals(w[0], "loc")) {
                // "loc y x [z]" or "loc y,x"
                std::string joined = join(w, 1);
                for (char& ch : joined)
                    if (ch == ',') ch = ' ';
                std::vector<std::string> n = words(joined);
                auto y = number(n, 0), x = number(n, 1), z = number(n, 2);
                if (!y || !x) {
                    out("Usage: /moveto loc <y> <x> [z]");
                    return;
                }
                glm::vec3 server(*x, *y, z ? *z : game::world_to_server(spawn_position(*me)).z);
                std::string err = nav->go_to_point(game::server_to_world(server), *mdist,
                                                   std::format("{:.0f}, {:.0f}", server.y, server.x), true);
                if (!err.empty()) out(err);
                return;
            }
            const game::Spawn* s = nullptr;
            if (iequals(w[0], "id")) {
                auto id = w.size() >= 2 ? id_number<std::uint32_t>(w, 1) : std::optional<std::uint32_t>(conn->world().target_id);
                if (id) {
                    auto it = conn->world().spawns.find(*id);
                    if (it != conn->world().spawns.end() && it->first != conn->world().player_id) s = &it->second;
                }
            }
            if (!s) {
                out("Usage: /moveto loc <y> <x> [z] | id [#] | mdist <n> | off");
                return;
            }
            std::string err = nav->go_to_point(spawn_position(*s), *mdist, s->name, true);
            if (!err.empty()) out(err);
        },
        plugin::kCapMove);

    // /stick (MQ2MoveUtils' words): keep to the target, or a spawn by id, until /stick off, the player moves, or it
    // is gone. [<distance>] (default: inside melee range), [behind], [id <#>]. Hailstone's own (NavControl::stick).
    h.add_command(
        owner, "/stick",
        [context, say](std::string_view args) {
            const Context& ctx = context();
            nav::NavControl* nav = ctx.nav;
            const net::GameConnection* conn = ctx.conn;
            if (!nav || !conn || !conn->world().player()) return;
            const std::vector<std::string> w = words(args);
            if (!w.empty() && (iequals(w[0], "off") || iequals(w[0], "stop"))) {
                nav->unstick("You are no longer sticking to anything.");
                return;
            }
            nav::StickOptions opt;
            std::uint32_t id = conn->world().target_id;
            for (std::size_t i = 0; i < w.size(); ++i) {
                if (iequals(w[i], "behind")) {
                    opt.behind = true;
                } else if (iequals(w[i], "id") && id_number<std::uint32_t>(w, i + 1)) {
                    id = *id_number<std::uint32_t>(w, ++i);
                } else if (auto d = number(w, i); d && *d > 0.0f) {
                    opt.distance = *d;
                } else {
                    say("Usage: /stick [<distance>] [behind] [id <#>] | off");
                    return;
                }
            }
            if (id == 0 || id == conn->world().player_id) {
                say("You must have a target to stick to.");
                return;
            }
            const std::string err = nav->stick(id, opt);
            if (!err.empty()) {
                say("[Stick] " + err + ".");
                return;
            }
            const auto st = nav->status();
            const auto& spawns = conn->world().spawns;
            const auto it = spawns.find(id);
            say(std::format("You are now sticking to {}{} ({:.0f}).",
                            it != spawns.end() ? game::display_name(it->second.name) : std::string("it"),
                            opt.behind ? ", behind" : "", st.stick_distance));
        },
        plugin::kCapMove);
}

} // namespace hailstone::mq
