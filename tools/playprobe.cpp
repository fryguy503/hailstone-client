// playprobe HOST[:PORT] USER PASS [options]
// Logs in, picks the first server, and enters the world headlessly:
//   --char NAME        character to play (default: the first one)
//   --create NAME      create NAME (human warrior) if the account has no such character
//   --start-zone Z     with --create: the start zone id (e.g. 394 Crescent Reach)
//   --tutorial 1       with --create: press the tutorial button (EQEmu: start in tutorialb)
//   --race R --class C with --create: race and class ids (default 1 / 1, human warrior)
//   --seconds N        stay in the zone N seconds (default 20), then camp
//   --say TEXT         say TEXT once in the zone
//   --plugins DIR      the compiled plugins to load (default: hailstone/plugins beside this binary; the NMS
//                      plugin, an add-on in plugins/nms, answers an NMS server's checks), with every permission
//   --auth-key K       NMS Custom:ServerAuthKey for the NMS plugin's OP_CAuth check
//   --doors 1          print the zone's doors (OP_SpawnDoor) and ground objects (OP_GroundSpawn)
//   --zone-line Z      walk (straight, at run speed, through walls) into the
//                      nearest zone-line region leading to zone id Z (0 = any)
//                      and let the client's zone-line trigger zone us there
//   --debug 1          log unhandled packets
//   --load-stall MS    block MS milliseconds when the zone's files would load,
//                      as the client's zone loading does (testing the session)
//   --fight-nearest L  target the nearest NPC of level <= L, consider it, walk
//                      up to it (straight, through walls), auto-attack until it
//                      or we die, and report damage, experience and the kill
//                      (or our death and the return to the bind point)
//   --fight-min L      only NPCs of level >= L (to test dying)
//   item checks: --items, --dump-inventory, --move-test, --link-test, --loot,
//   --destroy-test (tools/item_probe.h)
//   spell checks: --spells, --spell-gem, --spell-med (tools/spell_probe.h)
//   group / pet checks: --group-accept, --group-invite, --gsay, --group-leave,
//   --pet-test (tools/group_probe.h)
//   merchants, hail and trade: --shop, --list-merchants, --hail, --handin,
//   --trade-with, --trade-accept (tools/shop_probe.h)
//   bank, coin moves and stack splits: --bank, --bank-keep; the bank as last seen: --bank-file (tools/bank_probe.h)
//   AAs, training, tasks and combines: --aa, --train, --task, --combine, --object (tools/progress_probe.h)
//   duels, inspect, commands, the bazaar and mail: --cmd, --duel, --inspect, --trader, --shop-trader,
//   --bazaar, --mail, --mail-send (tools/market_probe.h)
//   raids, guild tribute and parcel money: --raid-accept, --raid-size, --raid-motd, --tribute-opt-out,
//   --tribute-cycle, --guild-watch, --parcel-at, --parcel-money, --parcel-claim (tools/social_probe.h)
//   augments: --augment (tools/augment_probe.h)
//   scripted quests (a GM on a test server): --quest FILE (tools/quest_probe.h)
// Needs EQ_GAME_DIR for the file checksums world asks for.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <random>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "assets/zone.h"
#include "core/game_dir.h"
#include "game/collision.h"
#include "game/combat.h"
#include "game/coords.h"
#include "game/revamped_zone.h"
#include "game/zone_lines.h"
#include "core/log.h"
#include "net/game_connection.h"
#include "net/login.h"
#include "item_probe.h"
#include "spell_probe.h"
#include "group_probe.h"
#include "shop_probe.h"
#include "bank_probe.h"
#include "progress_probe.h"
#include "market_probe.h"
#include "social_probe.h"
#include "augment_probe.h"
#include "quest_probe.h"
#include "game/server_ext_chain.h"
#include "platform/paths.h"
#include "platform/private_file.h"
#include "plugin/hooks.h"
#include "plugin/native_plugins.h"
#include "plugin/policy.h"

using namespace hailstone;
using Clock = std::chrono::steady_clock;

namespace {
void tick() { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

// A new folder only this user can open, under the temp folder with a name of this run's; empty if none could be made.
std::filesystem::path private_scratch_dir() {
    std::random_device rd;
    for (int tries = 0; tries < 16; ++tries) {
        const auto tag = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
        const auto path = std::filesystem::temp_directory_path() / ("playprobe-" + std::to_string(tag));
        if (platform::make_private_dir(path)) return path;
    }
    return {};
}

struct RemoveOnExit {
    std::filesystem::path path;
    explicit RemoveOnExit(std::filesystem::path p) : path(std::move(p)) {}
    RemoveOnExit(const RemoveOnExit&) = delete;
    RemoveOnExit& operator=(const RemoveOnExit&) = delete;
    ~RemoveOnExit() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// The sample point (world, a feet position) nearest to `from` inside a
// zone-line region that resolves to zone `want` (0 = any).
std::optional<glm::vec3> find_zone_line(const assets::Zone& zone, const game::CollisionMesh& mesh,
                                        const net::GameConnection& conn, const glm::vec3& from, std::uint16_t want) {
    std::optional<glm::vec3> best;
    for (float x = zone.bounds_min.x; x <= zone.bounds_max.x; x += 4)
        for (float y = zone.bounds_min.y; y <= zone.bounds_max.y; y += 4)
            for (float z = zone.bounds_min.z; z <= zone.bounds_max.z; z += 4) {
                glm::vec3 p(x, y, z);
                auto info = zone.regions.lookup(p + glm::vec3(0, 0, game::kRegionProbeHeight));
                if (!info.is_zone_line()) continue;
                float loc[3] = {p.x, p.y, p.z};
                auto rows = conn.zone_point_rows();
                auto r = game::resolve_teleport_index(game::region_teleport_text(info.tag), -1,
                                                      static_cast<std::int32_t>(conn.current_zone_dword()), loc, rows);
                if (!r.ok || r.found == 0 || r.same_zone || (want && (r.found & 0xffff) != want)) continue;
                // Stand on the floor, as a walking player would.
                auto floor = mesh.floor_below(p, 0.0f);
                if (!floor || p.z - *floor > 4.0f) continue;
                p.z = *floor;
                auto at_floor = zone.regions.lookup(p + glm::vec3(0, 0, game::kRegionProbeHeight));
                if (!at_floor.is_zone_line() || at_floor.tag != info.tag) continue;
                if (!best || glm::distance(p, from) < glm::distance(*best, from)) best = p;
            }
    return best;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: playprobe HOST[:PORT] USER PASS [--char N] [--create N] [--seconds N] [--say T] [--plugins DIR] [--auth-key K]\n");
        return 2;
    }
    std::string host = argv[1];
    std::uint16_t port = 5997;
    if (auto c = host.rfind(':'); c != std::string::npos) {
        port = static_cast<std::uint16_t>(std::atoi(host.c_str() + c + 1));
        host.resize(c);
    }
    std::string user = argv[2], pass = argv[3], want, create, say;
    bool check_floor = false;
    float walk = 0;
    int zone_line = -1;
    int fight_max = -1, fight_min = 0;
    int seconds = 20;
    int load_stall = 0;
    int start_zone = -1;
    bool list_doors = false; // --doors 1: print the zone's doors and ground objects
    bool tutorial = false;
    bool book_test = false; // --book-test 1: open and close the spell book (game/spell_book.h)
    int book_step = 0;
    net::ConnectionOptions copts;
    std::string auth_key;                                              // --auth-key, for the NMS plugin
    std::filesystem::path plugin_dir = platform::install_dir() / "hailstone" / "plugins"; // --plugins
    probe::ItemProbe items;
    probe::SpellProbe spells;
    probe::GroupProbe group;
    probe::ShopProbe shop;
    probe::BankProbe bank;
    probe::ProgressProbe progress;
    probe::MarketProbe market;
    probe::SocialProbe social;
    probe::AugmentProbe augment;
    probe::QuestProbe quest;
    std::uint32_t create_race = 1, create_class = 1;
    for (int i = 4; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--char")) want = argv[i + 1];
        else if (!std::strcmp(argv[i], "--create")) create = argv[i + 1];
        else if (!std::strcmp(argv[i], "--start-zone")) start_zone = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--tutorial")) tutorial = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--load-stall")) load_stall = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--say")) say = argv[i + 1];
        else if (!std::strcmp(argv[i], "--book-test")) book_test = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--doors")) list_doors = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--check-floor")) check_floor = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--walk")) walk = std::strtof(argv[i + 1], nullptr);
        else if (!std::strcmp(argv[i], "--zone-line")) zone_line = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--fight-nearest")) fight_max = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--fight-min")) fight_min = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--debug") && std::atoi(argv[i + 1])) log::set_level(log::Level::Debug);
        else if (!std::strcmp(argv[i], "--auth-key")) auth_key = argv[i + 1];
        else if (!std::strcmp(argv[i], "--plugins")) plugin_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--race")) create_race = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--class")) create_class = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
        else if (!spells.option(argv[i], argv[i + 1]) && !group.option(argv[i], argv[i + 1]) && !shop.option(argv[i], argv[i + 1]) && !bank.option(argv[i], argv[i + 1]) && !progress.option(argv[i], argv[i + 1]) && !market.option(argv[i], argv[i + 1]) && !social.option(argv[i], argv[i + 1]) && !augment.option(argv[i], argv[i + 1]) && !quest.option(argv[i], argv[i + 1])) items.option(argv[i], argv[i + 1]);
    }
    const char* gd = std::getenv("EQ_GAME_DIR");
    if (!gd) { std::fprintf(stderr, "set EQ_GAME_DIR\n"); return 2; }
    GameDir game(gd);

    net::LoginClient login;
    login.start(host, port, user, pass);
    auto deadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < deadline) {
        login.update();
        if (login.state() == net::LoginState::LoggedIn) login.request_server_list();
        if (login.state() == net::LoginState::ServerList) {
            if (login.servers().empty()) { std::fprintf(stderr, "no servers\n"); return 1; }
            login.request_play(login.servers().front().id);
        }
        if (login.state() == net::LoginState::PlayAccepted || login.state() == net::LoginState::Failed) break;
        tick();
    }
    if (login.state() != net::LoginState::PlayAccepted) { std::fprintf(stderr, "login: %s\n", login.error().c_str()); return 1; }
    auto ticket = *login.ticket();
    login.close();
    std::printf("login ok, world %s:%u\n", ticket.world_host.c_str(), ticket.world_port);

    copts.zone_packet_tap = [&items, &spells, &group, &shop, &bank, &progress, &market, &social, &augment](const net::AppPacket& p) {
        items.tap(p);
        spells.tap(p);
        group.tap(p);
        shop.tap(p);
        bank.tap(p);
        progress.tap(p);
        market.tap(p);
        social.tap(p);
        augment.tap(p);
    };
    // The compiled plugins (plugin/native_plugins.h), every one in the folder loaded with every permission, as a
    // server without a hailstone.cfg lets a player's: the NMS plugin answers an NMS server's client checks.
    plugin::Hooks hooks;
    plugin::Policy policy;
    policy.set(plugin::Rules{});
    auto extensions = std::make_shared<game::ExtensionChain>();
    copts.hooks = &hooks;
    copts.plugins = extensions;
    bank.configure(copts);
    net::GameConnection conn(game, copts);
    // The plugins' cache and config (the NMS plugin's auth_key): a new owner-only folder of this run's in the shared
    // temp folder, removed at the end, never a fixed name another user could have made or linked first.
    const std::filesystem::path plugin_data = private_scratch_dir();
    if (plugin_data.empty()) { std::fprintf(stderr, "could not make a private folder in %s\n", std::filesystem::temp_directory_path().string().c_str()); return 1; }
    const RemoveOnExit remove_plugin_data{plugin_data};
    if (!auth_key.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(plugin_data / "config" / "nms", ec);
        if (ec || !platform::write_private_file(plugin_data / "config" / "nms" / "nms.ini", "[NMS]\nauth_key=" + auth_key + "\n")) {
            std::fprintf(stderr, "could not write the auth key\n");
            return 1;
        }
    }
    plugin::NativeEnv ne;
    ne.hooks = &hooks;
    ne.policy = &policy;
    ne.print = [](const std::string& t, std::uint32_t) { std::printf("  PLUGIN: %s\n", t.c_str()); };
    ne.run = [&conn](const std::string& line) { conn.send_command(line); };
    ne.send = [&conn](plugin::Stream st, const net::AppPacket& p) {
        if (st == plugin::Stream::World) conn.send_world_packet(p);
        else conn.send_zone_packet(p);
    };
    ne.extensions = extensions.get();
    ne.connection = [&conn] { return &conn; };
    ne.dirs = {plugin_dir};
    ne.cache_dir = plugin_data / "cache";
    ne.config_dir = plugin_data / "config";
    plugin::NativePlugins plugins(ne);
    for (const auto& info : plugins.list()) plugins.load(info.name);
    conn.start(ticket);
    bool created = false, entered = false, said = false;
    std::string playing;
    net::ConnectionState last = net::ConnectionState::Idle;
    Clock::time_point in_zone_at{};
    std::string zone_seen;
    std::optional<assets::Zone> zl_zone;
    std::optional<glm::vec3> zl_target;
    glm::vec3 zl_feet{0.0f};
    game::ZoneLineTrigger zl_trigger;
    bool zl_requested = false, zl_done = false;
    // --fight-nearest
    enum class Fight { Off, Pick, Approach, Attack, Done } fight = fight_max >= 0 ? Fight::Pick : Fight::Off;
    std::uint32_t foe = 0;
    std::string foe_name;
    glm::vec3 fight_pos{0.0f};
    Clock::time_point fight_started{}, fight_last_step{}, fight_ended{};
    std::uint32_t exp_before = 0;
    int hits = 0, misses = 0, hits_taken = 0;
    bool died = false, killed = false, rezoned_after_death = false;
    deadline = Clock::now() + std::chrono::seconds(90);
    while (Clock::now() < deadline) {
        conn.update();
        hooks.pulse();
        if (auto z = conn.take_zone_to_load(); z && load_stall > 0) {
            std::printf("  stalling %d ms as if loading %s\n", load_stall, z->c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(load_stall));
        }
        if (conn.state() != last) {
            last = conn.state();
            std::printf("[%s] %s\n", net::to_string(last), conn.status().c_str());
            if (!conn.error().empty()) std::printf("  error: %s\n", conn.error().c_str());
        }
        while (auto c = conn.poll_chat()) {
            std::printf("  chat: %s\n", game::strip_links(c->text).c_str());
            items.chat(c->text);
            spells.chat(c->text);
            shop.chat(c->text);
            quest.chat(game::strip_links(c->text));
            if (fight == Fight::Off) continue;
            const std::string& t = c->text;
            if (t.rfind("You ", 0) == 0 && t.find(" for ") != std::string::npos && t.find("damage") != std::string::npos) ++hits;
            else if (t.rfind("You try to", 0) == 0) ++misses;
            else if (t.find(" YOU for ") != std::string::npos) ++hits_taken;
            if (t.rfind("You have slain", 0) == 0) killed = true;
            if (t.rfind("You have been slain", 0) == 0 || t == "You died.") died = true;
        }
        if (conn.state() == net::ConnectionState::Disconnected) break;
        if (conn.state() == net::ConnectionState::CharacterSelect && !entered) {
            const auto& chars = conn.characters();
            std::printf("  %zu character(s):", chars.size());
            for (const auto& c : chars) std::printf(" %s(L%u r%u c%u z%u)", c.name.c_str(), c.level, c.race, c.class_, c.zone);
            std::printf("\n");
            bool have = false;
            for (const auto& c : chars) have |= (!create.empty() && c.name == create);
            if (!create.empty() && !have && !created) {
                if (!conn.char_create_data()) { tick(); continue; } // tables arrive just after the list
                const auto& data = *conn.char_create_data();
                const net::CharCreateCombo* combo = nullptr;
                for (const auto& c : data.combos)
                    if (c.race == create_race && c.class_ == create_class && c.expansion_required == 0 &&
                        (start_zone < 0 || c.zone == static_cast<std::uint32_t>(start_zone))) { combo = &c; break; }
                if (!combo && start_zone >= 0) {
                    // Any race and class that may start there (human warriors may not).
                    for (const auto& c : data.combos)
                        if (c.zone == static_cast<std::uint32_t>(start_zone)) { combo = &c; break; }
                    std::set<std::uint32_t> zones;
                    for (const auto& c : data.combos) zones.insert(c.zone);
                    std::printf("  combination zones:");
                    for (auto z : zones) std::printf(" %u", z);
                    std::printf("\n  start zone %d combinations:", start_zone);
                    for (const auto& c : data.combos)
                        if (c.zone == static_cast<std::uint32_t>(start_zone)) std::printf(" r%u/c%u", c.race, c.class_);
                    std::printf("\n");
                }
                if (!combo) { std::fprintf(stderr, "no human warrior combination\n"); return 1; }
                auto cc = net::default_char_create(data, *combo, 0);
                if (cc && tutorial) cc->tutorial = 1;
                std::printf("  creating %s: race %u class %u deity %u zone %u\n", create.c_str(), cc->race, cc->class_, cc->deity, cc->start_zone);
                conn.create_character(create, *cc);
                created = true;
                continue;
            }
            playing = !want.empty() ? want : !create.empty() ? create : chars.empty() ? "" : chars.front().name;
            if (playing.empty()) { std::fprintf(stderr, "no characters; use --create NAME\n"); return 1; }
            conn.enter_world(playing);
            entered = true;
        }
        if (conn.state() == net::ConnectionState::InZone) {
            if (!zone_seen.empty() && conn.world().zone_short_name != zone_seen && (!zl_done || quest.active())) {
                // Arrived in the next zone through the zone line (or a --quest step's #zone).
                zl_done = true;
                in_zone_at = {};
                std::printf("  ZONED: %s -> %s\n", zone_seen.c_str(), conn.world().zone_short_name.c_str());
            }
            if (in_zone_at == Clock::time_point{}) {
                zone_seen = conn.world().zone_short_name;
                in_zone_at = Clock::now();
                items.zone_in(conn);
                spells.zone_in(conn);
                group.zone_in(conn);
                shop.zone_in(conn);
                bank.zone_in(conn);
                progress.zone_in(conn);
                market.zone_in(conn);
                social.zone_in(conn);
                augment.zone_in(conn);
                quest.zone_in(conn);
                const auto& w = conn.world();
                std::printf("  in %s (%s): %zu spawns, me id %u", w.zone_short_name.c_str(), w.zone_long_name.c_str(), w.spawns.size(), w.player_id);
                if (const auto* me = w.player()) std::printf(" at %.1f %.1f %.1f heading %.1f", me->position.x, me->position.y, me->position.z, me->heading);
                std::printf("\n");
                if (const auto& z = conn.zone_info())
                    std::printf("  zone: safe %.1f %.1f %.1f, clip %.0f-%.0f, fog %.0f-%.0f, sky %u, time type %u, ztype %u\n", z->safe_x, z->safe_y, z->safe_z, z->minclip, z->maxclip, z->fog_minclip[0], z->fog_maxclip[0], z->sky, z->time_type, z->zone_type);
                if (const auto& t = conn.time_of_day())
                    std::printf("  time: hour %u minute %u day %u month %u year %u\n", t->hour, t->minute, t->day, t->month, t->year);
                // A live folder's revamped .eqg loads when the server's safe point stands on it (game/revamped_zone.h).
                auto revamped_test = [&](std::optional<game::CollisionMesh>& built) -> assets::RevampedZoneTest {
                    const auto& z = conn.zone_info();
                    if (!z) return {};
                    return game::safe_point_test(game::server_to_world({z->safe_x, z->safe_y, z->safe_z}), built);
                };
                int shown = 0;
                for (const auto& [id, s] : w.spawns)
                    if (shown++ < 8) std::printf("    %u %s L%u race %u at %.1f %.1f %.1f\n", id, s.name.c_str(), s.level, s.race, s.position.x, s.position.y, s.position.z);
                if (list_doors) {
                    std::printf("  %zu doors, %zu ground objects\n", conn.doors().size(), conn.ground_objects().size());
                    for (const auto& d : conn.doors())
                        std::printf("    door %u %s type %u state %u/%u at server (%.1f %.1f %.1f) heading %.0f angle %.0f size %d param %d\n",
                                    d.id, d.name.c_str(), d.type, d.state, d.default_state, d.x, d.y, d.z, d.default_heading,
                                    d.default_door_angle, d.scale_factor, d.param);
                    for (const auto& o : conn.ground_objects())
                        std::printf("    object %u %s type %d at world (%.1f %.1f %.1f) size %.2f\n", o.drop_id, o.name.c_str(),
                                    o.type, o.position.x, o.position.y, o.position.z, o.size);
                }
                if (check_floor) {
                    // Do server x/y match the zone files' x/y, and how high above the floor is z?
                    std::optional<game::CollisionMesh> tested;
                    auto zone = assets::load_zone(game, w.zone_short_name, {}, revamped_test(tested));
                    auto mesh = tested ? std::move(*tested) : game::CollisionMesh::build(zone);
                    int same = 0, swapped = 0, n = 0;
                    for (const auto& [id, s] : w.spawns) {
                        auto a = mesh.floor_below(s.position, 0.5f);
                        auto b = mesh.floor_below({s.position.y, s.position.x, s.position.z}, 0.5f);
                        same += a && s.position.z - *a <= 8.0f;
                        swapped += b && s.position.z - *b <= 8.0f;
                        if (n++ < 6)
                            std::printf("    floor %s: as-is %s, swapped %s\n", s.name.c_str(),
                                        a ? std::to_string(s.position.z - *a).c_str() : "none",
                                        b ? std::to_string(s.position.z - *b).c_str() : "none");
                    }
                    std::printf("  spawns 0-8 units above a floor: as-is %d, swapped %d of %zu\n", same, swapped, w.spawns.size());
                }
                deadline = Clock::now() + std::chrono::seconds(seconds + 60);
                if (zone_line >= 0 && !zl_done && w.player()) {
                    std::optional<game::CollisionMesh> tested;
                    zl_zone = assets::load_zone(game, w.zone_short_name, {}, revamped_test(tested));
                    zl_feet = w.player()->position - glm::vec3(0, 0, w.player()->heights.avatar_height);
                    zl_trigger.reset();
                    std::printf("  %zu zone points from the server\n", conn.zone_points().size());
                    for (const auto& p : conn.zone_points())
                        std::printf("    #%u -> zone %u at server (%.1f, %.1f, %.1f) heading %.0f\n", p.number,
                                    p.zone_id, p.x, p.y, p.z, p.heading);
                    auto mesh = game::CollisionMesh::build(*zl_zone);
                    zl_target = find_zone_line(*zl_zone, mesh, conn, zl_feet, static_cast<std::uint16_t>(zone_line));
                    if (zl_target)
                        std::printf("  walking from world (%.1f %.1f %.1f) to zone line at (%.1f %.1f %.1f), %.0f units\n",
                                    zl_feet.x, zl_feet.y, zl_feet.z, zl_target->x, zl_target->y, zl_target->z,
                                    glm::distance(zl_feet, *zl_target));
                    else
                        std::printf("  no zone line found\n");
                }
            }
            if (zl_target && !zl_done && !zl_requested) {
                // Straight towards the zone line at 30 units/s; the trigger decides when we are in.
                static auto last_step = Clock::now();
                float dt = std::min(0.1f, std::chrono::duration<float>(Clock::now() - last_step).count());
                last_step = Clock::now();
                glm::vec3 d = *zl_target - zl_feet;
                float len = glm::length(d);
                glm::vec3 v = len > 0.01f ? d / len * 30.0f : glm::vec3(0.0f);
                zl_feet += len > 30.0f * dt ? v * dt : d;
                float heading = game::yaw_to_heading(std::atan2(d.y, d.x));
                glm::vec3 reported = zl_feet + glm::vec3(0, 0, conn.world().player() ? conn.world().player()->heights.avatar_height : 0.0f);
                conn.set_player_motion(reported, heading, v, true);
                if (auto text = zl_trigger.update(zl_feet, zl_zone->regions)) {
                    float loc[3] = {reported.x, reported.y, reported.z};
                    auto rows = conn.zone_point_rows();
                    auto r = game::resolve_teleport_index(*text, -1, static_cast<std::int32_t>(conn.current_zone_dword()),
                                                          loc, rows);
                    std::printf("  zone line %s at world (%.1f %.1f %.1f): zone %#x, target (%.1f %.1f %.1f) zone point %d\n",
                                text->c_str(), zl_feet.x, zl_feet.y, zl_feet.z, static_cast<unsigned>(r.found), r.x, r.y,
                                r.z, r.zone_point);
                    if (r.ok && r.found != 0 && !r.same_zone) {
                        // DoTeleportB for another zone: keep coordinates at or above 999999, then MoveToZone.
                        game::MoveToZoneCall m;
                        m.zone = r.found;
                        m.x = r.x < 999999.0f ? r.x : loc[0];
                        m.y = r.y < 999999.0f ? r.y : loc[1];
                        m.z = r.z < 999999.0f ? r.z : loc[2];
                        m.heading = static_cast<std::int32_t>(r.heading < 513.0f ? r.heading : heading);
                        conn.record_position_sample(game::kPositionZoneLine, &reported);
                        conn.send_zone_line_position({loc[0], loc[1], loc[2]});
                        conn.move_to_zone(m);
                        zl_requested = true;
                    }
                }
            }
            if (walk > 0 && Clock::now() - in_zone_at < std::chrono::duration<float>(walk) && conn.world().player()) {
                // Walk straight along the current heading at 20 units/s, ignoring walls.
                static glm::vec3 pos = conn.world().player()->position;
                static auto last_step = Clock::now();
                float h = conn.world().player()->heading;
                float dt = std::chrono::duration<float>(Clock::now() - last_step).count();
                last_step = Clock::now();
                glm::vec3 dir(std::cos(game::heading_to_yaw(h)), std::sin(game::heading_to_yaw(h)), 0);
                pos += dir * 20.0f * dt;
                conn.set_player_motion(pos, h, dir * 20.0f, true);
            }
            if (fight != Fight::Off && fight != Fight::Done && Clock::now() - in_zone_at > std::chrono::seconds(3) &&
                conn.world().player()) {
                const auto& w = conn.world();
                const auto& st = conn.player_status();
                auto now = Clock::now();
                if (fight == Fight::Pick) {
                    // The nearest living NPC of a low enough level (named like a creature: "a_" / "an_").
                    glm::vec3 me = w.player()->position;
                    float best = 1e30f;
                    for (const auto& [id, sp] : w.spawns) {
                        if (!sp.is_npc || sp.is_corpse || !sp.targetable || sp.level > fight_max || sp.level < fight_min) continue;
                        if (sp.name.rfind("a_", 0) != 0 && sp.name.rfind("an_", 0) != 0) continue;
                        float d = glm::distance(sp.position, me);
                        if (d < best) { best = d; foe = id; }
                    }
                    if (!foe) {
                        std::printf("  FIGHT: no NPC of level %d..%d here; creatures:", fight_min, fight_max);
                        for (const auto& [id, sp] : w.spawns)
                            if (sp.is_npc && !sp.is_corpse && (sp.name.rfind("a_", 0) == 0 || sp.name.rfind("an_", 0) == 0))
                                std::printf(" %s(L%u)", sp.name.c_str(), sp.level);
                        std::printf("\n");
                        fight = Fight::Done;
                    } else {
                        const auto& sp = w.spawns.at(foe);
                        foe_name = game::display_name(sp.name);
                        fight_pos = me;
                        exp_before = st.exp;
                        std::printf("  FIGHT: target %s (id %u, level %u, race %u) %.0f units away; me L%u hp %lld/%lld exp %u/330\n",
                                    foe_name.c_str(), foe, sp.level, sp.race, best, st.level, static_cast<long long>(st.hp),
                                    static_cast<long long>(st.max_hp), st.exp);
                        conn.set_target(foe);
                        conn.consider();
                        fight = Fight::Approach;
                        fight_started = fight_last_step = now;
                    }
                } else {
                    auto it = w.spawns.find(foe);
                    bool foe_dead = it == w.spawns.end() || it->second.is_corpse;
                    float dt = std::min(0.1f, std::chrono::duration<float>(now - fight_last_step).count());
                    fight_last_step = now;
                    if (st.dead || died) {
                        if (fight_ended == Clock::time_point{}) {
                            fight_ended = now;
                            std::printf("  FIGHT: we died (hp %lld); waiting to return to the bind point\n", static_cast<long long>(st.hp));
                        }
                    } else if (foe_dead || killed) {
                        if (fight_ended == Clock::time_point{}) {
                            fight_ended = now;
                            std::printf("  FIGHT: %s is dead after %.1f s\n", foe_name.c_str(),
                                        std::chrono::duration<float>(now - fight_started).count());
                            items.fight_over(foe); // at once: an emptied NPC corpse decays after about 1 s
                        }
                        // Wait a moment for the experience update, then report.
                        if (now - fight_ended > std::chrono::seconds(3)) {
                            std::printf("  FIGHT RESULT: kill %s, hits %d, misses %d, hits taken %d, exp %u -> %u (of 330), level %u, hp %lld/%lld\n",
                                        killed ? "yes" : "no", hits, misses, hits_taken, exp_before, st.exp, st.level,
                                        static_cast<long long>(st.hp), static_cast<long long>(st.max_hp));
                            fight = Fight::Done;
                            in_zone_at = now - std::chrono::seconds(seconds) + std::chrono::seconds(4); // camp soon
                        }
                    } else if (now - fight_started > std::chrono::seconds(150)) {
                        std::printf("  FIGHT: gave up after 150 s\n");
                        conn.set_auto_attack(false);
                        fight = Fight::Done;
                    } else {
                        // Follow and face the foe; attack once in reach.
                        glm::vec3 to = it->second.position;
                        glm::vec3 d = to - fight_pos;
                        d.z = 0;
                        float len = glm::length(d);
                        float reach = std::max(3.5f, it->second.size * 0.5f + 2.0f);
                        glm::vec3 v(0.0f);
                        if (len > reach) {
                            v = d / len * 30.0f;
                            fight_pos += len - reach > 30.0f * dt ? v * dt : d / len * (len - reach);
                        }
                        fight_pos.z = to.z;
                        float heading = game::heading_to(fight_pos, to);
                        conn.set_player_motion(fight_pos, heading, v, len > reach);
                        if (len <= reach + 1.0f && !conn.auto_attack()) {
                            std::printf("  FIGHT: in reach (%.1f units), auto attack on\n", len);
                            conn.set_auto_attack(true);
                            fight = Fight::Attack;
                        }
                    }
                }
            }
            if (died && !rezoned_after_death && conn.player_status().hp > 0 && !conn.player_status().dead) {
                rezoned_after_death = true;
                const auto& st = conn.player_status();
                std::printf("  FIGHT RESULT: died; back in %s at the bind point, hp %lld/%lld, exp %u (was %u)\n",
                            conn.world().zone_short_name.c_str(), static_cast<long long>(st.hp),
                            static_cast<long long>(st.max_hp), st.exp, exp_before);
                fight = Fight::Done;
                in_zone_at = Clock::now() - std::chrono::seconds(seconds) + std::chrono::seconds(4);
            }
            if (!say.empty() && !said && Clock::now() - in_zone_at > std::chrono::seconds(2)) { conn.say(say); said = true; }
            if (book_test && book_step < 3 && Clock::now() - in_zone_at > std::chrono::seconds(3 + 2 * book_step)) {
                if (book_step == 0) {
                    bool ok = conn.open_spell_book();
                    std::printf("  BOOK: open -> %d, open %d, stand state %u\n", ok, conn.spell_book_open(), conn.player_status().stand_state);
                } else if (book_step == 1) {
                    conn.close_spell_book();
                    std::printf("  BOOK: close -> open %d, stand state %u\n", conn.spell_book_open(), conn.player_status().stand_state);
                } else {
                    conn.set_sitting(true);
                    bool ok = conn.open_spell_book();
                    conn.close_spell_book();
                    std::printf("  BOOK: sat first, open %d then close -> stand state %u (stays sitting)\n", ok, conn.player_status().stand_state);
                    conn.set_sitting(false);
                }
                ++book_step;
            }
            items.step(conn);
            spells.step(conn);
            group.step(conn);
            shop.step(conn);
            bank.step(conn);
            progress.step(conn);
            market.step(conn);
            social.step(conn);
            augment.step(conn);
            quest.step(conn);
            bool walking = (zl_target && !zl_done) || (fight != Fight::Off && fight != Fight::Done) || items.busy() || spells.busy() || group.busy() || shop.busy() || bank.busy() || progress.busy() || market.busy() || social.busy() || augment.busy() || quest.busy();
            // A --quest run camps as soon as its steps are done; --seconds is then only its time limit.
            if (!walking && (Clock::now() - in_zone_at > std::chrono::seconds(seconds) || quest.active())) conn.camp();
        }
        tick();
    }
    items.finish(conn);
    spells.finish(conn);
    group.finish(conn);
    shop.finish(conn);
    bank.finish(conn);
    progress.finish(conn);
    market.finish(conn);
    social.finish(conn);
    quest.finish(conn);
    // The class masks a plugin gave the core (the NMS plugin: OP_SendCharacterSets, OP_ServerAuthStats key 1).
    std::printf("class masks: character set 0x%x, server 0x%x, player 0x%x\n", conn.classes_mask(playing),
                conn.server_class_mask(), conn.player_class_mask());
    std::printf("final state: %s %s\n", net::to_string(conn.state()), conn.error().c_str());
    if (quest.active()) return quest.all_passed() ? 0 : 1;
    return conn.state() == net::ConnectionState::Disconnected && conn.error() == "Camped out." ? 0 : 1;
}
