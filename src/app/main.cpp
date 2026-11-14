#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <numbers>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <system_error>

#include "app/client_app.h"
#include "app/update_install.h"
#include "app/updater.h"
#include "core/build_info.h"
#include "app/model_viewer.h"
#include "app/spell_bolts.h"
#include "app/startup_error.h"
#include "app/ui_demo.h"
#include "app/zone_particles.h"
#include "game/spells.h"
#include "assets/sky.h"
#include "assets/texture_overrides.h"
#include "assets/zone_flora.h"
#include "assets/zone.h"
#include "audio/audio_system.h"
#include "core/asset_folder.h"
#include "core/config.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "core/wipe.h"
#include "game/scene_fog.h"
#include "game/light_list.h"
#include "game/scene_light.h"
#include "render/cover_map.h"
#include "render/fire_renderer.h"
#include "render/spell_fx_renderer.h"
#include "render/point_shadow_pass.h"
#include "render/flora_renderer.h"
#include "render/scene_lighting.h"
#include "game/view_distance.h"
#include "core/old_ui_settings.h"
#include "platform/crash_log.h"
#include "app/window_icon.h"
#include "platform/message_box.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "game/sky_clock.h"
#include "render/camera.h"
#include "render/vk_context.h"
#include "render/sky_renderer.h"
#include "render/zone_renderer.h"
#include "render/particle_renderer.h"
#include "render/atmosphere.h"
#include "render/weather_renderer.h"
#include "app/nameplate_layer.h"
#include "app/plate_test.h"
#include "game/collision.h"
#include "app/neighbour_zones.h"
#include "game/zone_names.h"
#include "game/sky_colours.h"
#include "game/particles.h"
#include "assets/spell_effects.h"
#include "core/ini_file.h"
#include "ui/imgui_layer.h"
#include "ui/item_windows.h"

namespace {

struct Options {
    bool validation = false;
    int frames = 0; // stop after this many frames; 0 runs until closed
    std::string game_dir;
    std::string classic_dir;
    std::string zone;
    int clip_plane = 15;
    std::optional<glm::vec3> position;
    std::optional<float> yaw_deg;
    std::optional<float> pitch_deg;
    std::string screenshot; // save this frame's image and exit (with --frames)
    // --neighbours "N:zone:x:y:z,...": the zone points OP_SendZonepoints would send (server coordinates, 999999
    // keeps), so the viewer draws the neighbour zones past the zone lines (app/neighbour_zones.h).
    std::optional<std::vector<hailstone::game::ZonePointTarget>> neighbours;
    std::optional<glm::vec2> fog; // override fog start/end (viewer only)
    std::optional<hailstone::game::ZoneFogInput> zone_fog; // viewer: OP_NewZone's fog values (--zone-fog)
    int sky = 1;                  // viewer sky index (0 = none, indoor)
    float time_hour = 12.0f;      // viewer sky time, hour 1..24
    float sky_timescale = 1.0f; // viewer: the sky's TimeScale (cloud motion)
    int spell_effect = 0;       // viewer: a spellsnew.eff effect to play in front of the camera
    int spell_bolt = 0;         // viewer: a spell whose bolt or comet to throw across the view (modern)
    bool plate_test = false;    // viewer: test name plates over the zone, for tools/probe_test.py (--plate-test)
    bool plate_plain = false;   // ... drawn with no depth test, the comparison's reference (--plate-test-no-depth)
    float plate_ghost = 0.0f;   // ... the alpha of what the scene hides, 0 drops it (--plate-test-ghost A)
    std::optional<std::pair<int, int>> weather; // viewer: OP_Weather {kind, intensity}, settled (--weather)
    float weather_age = -1.0f;     // --weather-age: seconds since it began (default settled, 600)
    float weather_stopped = -1.0f; // --weather-stopped: stopped this many seconds ago
    float moon_phase = -1.0f;   // --moon P: the moon's phase (0 new, 0.5 full); the viewer's default is full
    float blood_moon = -1.0f;   // --blood-moon B: the blood moon 0..1 (viewer default none)
    float aurora = -1.0f;       // --aurora A: the northern lights 0..1 (viewer default none)
    std::vector<hailstone::render::FireSource> fires; // --fire X Y Z H W: flames placed by hand (modern)
    bool no_fire = false;                         // --no-fire: the modern lighting without its flames (timing)
    bool no_grass = false;                        // --no-grass: the viewer without the zone's flora
    bool no_horizon = false;                      // --no-horizon: no land round the zone (render/horizon.h)
    float tree_detail = 1.0f;                     // --tree-detail PCT: display.tree_detail, as a scale
    float flora_distance = 400.0f;                // --flora-distance: how far the viewer draws the flora
    std::optional<glm::vec3> flora_push;          // --flora-push: a foot the viewer's flora bends away from
    std::optional<std::string> textures; // --textures DIR: replacement textures (assets/texture_overrides.h)
    bool no_material_maps = false;       // --no-material-maps: a pack's colour without its normal and roughness maps
    int time_type = -1;           // viewer OP_NewZone time_type; -1: 1 with a sky, 0 without
    int vision = 0;               // --vision: 1 infravision, 2 ultravision (the race's innate)
    hailstone::render::VertexLighting lighting = hailstone::render::VertexLighting::Classic;
    hailstone::render::SceneOptions scene; // --msaa, --tonemap, --exposure
    bool modern = false;              // --lighting modern (render/scene_lighting.h)
    bool tonemap_given = false;
    float view_distance = 2.5f;       // --view-distance (render::modern_view)
    std::optional<hailstone::app::DemoScreen> ui_demo; // show the developer UI demo
    float ui_scale = 0.0f;                        // > 0 overrides the display scale
    int window_width = 1280, window_height = 720;  // the client window (--window-size)
    std::string user, password, character;        // client auto-login (testing)
    std::string host;                              // --host for the auto-login
    std::string server;                            // --server: a saved server (servers/<id>), by id or name
    std::string account;                           // --account: a saved account of that server
    int port = 0;
    float auto_walk = 0.0f, camera_distance = 0.0f;
    std::string watch;
    bool auto_target = false;
    bool pick_test = false;
    int auto_cast = 0;
    bool auto_scribe = false;
    int show_items = 0;
    bool show_stats = false;
    std::string move_test;
    int max_fps = 0;
    int load_stall_ms = 0;
    int auto_door = 0;
    float watch_turn = 0.0f;
    std::string auto_commands;
    std::string auto_keys;
    std::string input_script;
    std::string abilities_test;
    int melee_test = 0;
    bool auto_accept_invite = false;
    bool no_audio = false;
    bool no_vsync = false; // present as fast as the GPU draws (frame-time measurements)
    float frame_dt = 0.0f; // > 0: the clock moves this many seconds a frame, not with the wall clock
    hailstone::app::ModelViewerOptions model; // --model viewer; active when model.codes is not empty
};

void usage() {
    std::printf(
        "usage: hailstone [options]\n"
        "Without --zone or --ui-demo, runs the client: log in, pick a server and a character, play.\n"
        "  --game-dir DIR     EverQuest installation, RoF2 or live (default: $EQ_GAME_DIR; the client:\n"
        "                     else the folder chosen in its asset-folder picker, hailstone.ini assets.path)\n"
        "  --classic-dir DIR  another install (EverQuest Legends) for the classic zones a live one lacks\n"
        "                     (the client: else hailstone.ini assets.classic)\n"
        "  --user U --password P --character C\n"
        "                     log in and enter the world without clicks (testing: the password shows in\n"
        "                     the process list to anyone on this computer; use a test account's)\n"
        "  --host H --port P  the login server for --user (default: the dev server; never the\n"
        "                     remembered one), as a transient server that is not saved\n"
        "  --server S         pick the saved server S (its id, the servers/<id> folder, or its name) on\n"
        "                     the login screen; with --user, log in to it with its asset overrides\n"
        "  --account A        log in to the saved server (--server, else the last used) with its saved\n"
        "                     account A and A's password from the system keyring, play the world played\n"
        "                     last there and enter A's character (or --character); the login screen's\n"
        "                     Log in box starts its other windows so\n"
        "  --auto-target      after zoning in, target and consider the nearest NPC (testing)\n"
        "  --pick-test        after zoning in, turn to the NPCs nearby and pick at the screen centre\n"
        "                     (logging which are hit and whether a wall is in the way), left click the\n"
        "                     first one hit, then turn to the nearest door in sight and click it (testing)\n"
        "  --abilities-test S put skills S (ids, comma-separated) on hot bar 1, open the hot bar, Actions and\n"
        "                     key windows, approach the target and press the hot buttons in turn (testing)\n"
        "  --auto-cast N      after zoning in, cast spell gem N, or with -1 open the spell book; with\n"
        "                     --screenshot and no --frames, save the screen during it and quit (testing)\n"
        "  --auto-scribe      before --auto-cast: sit, take the first spell scroll from the bags, scribe it,\n"
        "                     memorize it into gem 1 and wait for the gem (testing)\n"
        "  --watch door[:N]   point the camera at door N (default: the --auto-door door); --watch-turn D\n"
        "                     views it from D degrees around its heading (testing)\n"
        "  --auto-door N      after zoning in, walk to the nearest door and click it (2: hinged doors\n"
        "                     only; 3: also walk into it closed, then through it open; 4: drop an\n"
        "                     item on the ground and pick it up again; testing)\n"
        "  --auto-commands L  after zoning in, type the chat lines L (';'-separated), one every 3 s (testing)\n"
        "  --auto-keys L      after zoning in, press and release the key commands L (names, ','-separated,\n"
        "                     e.g. TOGGLETARGET,TARGETNPC,DUCK), one every 2 s; UI:<window>:<child> clicks a stock\n"
        "                     window's child (UI:<window>:<child>:<button> with another button, e.g. rightup),\n"
        "                     LIST:<window>:<list>:<row>[:<column>] a list row (DLIST:<window>:<list>:<row> a double\n"
        "                     click), COMBO:<window>:<child>:<index> picks a choice, TEXT:<window>:<edit>:<text> types\n"
        "                     into an edit box, RCLICK right-clicks the target, HOTBTN:<window>:<button> a hot bar\n"
        "                     button, INSPECT:<slot>[:<bag index>]\n"
        "                     (PREVIEW:<slot>[:<bag index>] the same with Alt: the item preview)\n"
        "                     right-clicks a possessions slot, CORPSE[:<name>] targets the nearest corpse (whose\n"
        "                     name starts with <name>), LOOT loots the target, OBJECT[:<name>] clicks the nearest\n"
        "                     ground object, PICK:<slot>[:<bag index>[:<keys>]] left-clicks a possessions slot\n"
        "                     (keys s Shift, c Ctrl; RPICK the same with the right button, a Alt),\n"
        "                     WORLDSLOT:<n> a world container's slot, WORLDCOMBINE its Combine, GIVE drops the\n"
        "                     cursor's item or coin on the target (testing)\n"
        "  --input-script S   after zoning in, play made-up input: ';'-separated \"<seconds> <step>\" (pad <button>,\n"
        "                     axis <stick> <value>, key <name>, text <text>, click <x> <y>, wheel <x> <y> <n>,\n"
        "                     shot <file>, state, close, droplink; app/input_script.h; a virtual pad is attached; testing)\n"
        "  --window-size WxH  the window's size in pixels (default 1280x720)\n"
        "  --auto-accept      accept group invitations (testing)\n"
        "  --max-fps N        cap the frame rate (the original's MaxFPS; movement physics runs per frame)\n"
        "  --load-stall MS    sleep MS milliseconds after loading each zone, as a slow disk would (testing)\n"
        "  --move-test T      after zoning in, drive and log the movement: speed, jump,\n"
        "                     fall:H (lift H units and drop) or swim (testing)\n"
        "  --stats            open the developer character / stats window\n"
        "  --show-items N     after zoning in: 1 open the inventory and bags, 2 pick up the first\n"
        "                     general item, 4 request the primary weapon's item info (bits; testing)\n"
        "  --watch NAME       point the camera at the nearest spawn whose name contains NAME, from\n"
        "                     --camera-distance (default 15) on the player's side (testing)\n"
        "  --zone NAME        zone viewer: fly through a zone (classic or EQG), e.g. qeynos2\n"
        "  --pos X Y Z        start position in EQ world coordinates\n"
        "  --look YAW PITCH   camera direction in degrees (yaw 0 = +X, 90 = +Y)\n"
        "  --clip-plane N     view distance setting 0..20 (default 15)\n"
        "  --screenshot FILE  save the last frame as PNG (use with --frames); a relative FILE goes to screenshots/\n"
        "  --neighbours N:ZONE:X:Y:Z,...  the zone points (server coordinates, 999999 keeps) the server would send:\n"
        "                     draws the neighbour zones past the outdoor zone lines (modern lighting)\n"
        "  --fog START END    override fog distances (default: the client's no-server fallback)\n"
        "  --zone-fog ZTYPE R G B FOGMIN FOGMAX DENSITY MINCLIP MAXCLIP\n"
        "                     viewer: OP_NewZone's fog (ztype 0: fog off; colour bytes; the first fog clips)\n"
        "  --sky N            viewer sky from sky.s3d: 0 none (indoor), 1..17 (default 1)\n"
        "  --vision KIND      viewer: see as a race with infra (infravision) or ultra (ultravision)\n"
        "  --time HH          viewer time of day, hour 1..24 (default 12; fractions allowed)\n"
        "  --plate-test       viewer, with --frames 8 or more: test name plates over the zone, their boxes in the log\n"
        "                     (--plate-test-no-depth: with no depth test; --plate-test-ghost A: the alpha of what\n"
        "                     the scene hides); tools/probe_test.py compares the two\n"
        "  --spell-bolt ID    viewer, modern lighting: throw spell ID's bolt or comet (app/spell_bolts.h)\n"
        "                     across the view every 2.5 s\n"
        "  --spell-effect N   viewer: play spellsnew.eff effect N's cast and impact stages 25 units in\n"
        "                     front of the camera every 4 s (eqclient.ini particle density and opacity)\n"
        "  --sky-timescale S  viewer: the procedural sky's TimeScale property (cloud motion; default 1)\n"
        "  --time-type N      viewer zone time type: 1, 2, 5 outdoor, 4 half lit, else dark (default 1 with a\n"
        "                     sky, 0 without)\n"
        "  --lighting MODE    classic (the original's vertex lighting, default), modern (docs/graphics.md) or\n"
        "                     none (full-bright)\n"
        "  --msaa N           samples per pixel (1, 2, 4, 8; default 4)\n"
        "  --tonemap NAME     none, agx or aces (default: agx with --lighting modern, else none)\n"
        "  --exposure EV      scene exposure in stops (default 0)\n"
        "  --shadows N        shadow map size per cascade (0 off; default 2048), with --lighting modern\n"
        "  --light-shadows N  how many lights cast shadows (0 off; default 8, at most 8), modern\n"
        "  --no-ao            no screen-space ambient occlusion (--lighting modern)\n"
        "  --depth-prepass    draw the opaque geometry depth-only first, then colour it (--lighting modern)\n"
        "  --bloom N          bloom strength 0..1 (--lighting modern; default 0.5)\n"
        "  --render-scale P   draw the 3D view at P percent of its size (50..100) and upscale it with FSR 1\n"
        "  --sharpness N      FSR 1's sharpening below a render scale of 100, 0..1 (default 0.5)\n"
        "  --fire X Y Z H W   viewer: a flame H high and W wide standing at X Y Z (modern; repeatable)\n"
        "  --no-fire          viewer: no flames at all in the modern lighting (to time them)\n"
        "  --no-grass         viewer: no flora (radial flora, and Hailstone's zone flora: flora/<zone>.txt); the\n"
        "                     viewer draws flora with --lighting modern only\n"
        "  --flora-distance U viewer: draw the flora U units round the camera (400 .. 1200; default 400)\n"
        "  --flora-push X Y Z viewer: the flora bends away from a foot standing there, as from a spawn (testing)\n"
        "  --moon P           the moon's phase, 0 new .. 0.5 full .. 1 (--lighting modern; client: instead of the calendar)\n"
        "  --blood-moon B     the blood moon, 0 none .. 1 (--lighting modern; client: instead of the calendar)\n"
        "  --aurora A         the northern lights by night, 0 none .. 1 (--lighting modern; client: instead of the\n"
        "                     calendar)\n"
        "  --textures DIR     viewer: replacement textures from DIR (a pack under textures/, or a path), or off\n"
        "                     (default: display.texture_folder)\n"
        "  --no-material-maps viewer: a texture pack's colour only, without its normal and roughness maps\n"
        "  --weather W N      viewer: rain or snow at intensity N (1..10), already soaked in or lying\n"
        "  --weather-age S    viewer: S seconds after --weather began (its transition; default 600, settled)\n"
        "  --weather-stopped S  viewer: --weather stopped S seconds ago (the clearing)\n"
        "  --no-grade         viewer: no colour grade (--lighting modern; display.color_grade)\n"
        "  --no-horizon       viewer: no land round the zone (--lighting modern; display.horizon_hills)\n"
        "  --tree-detail PCT  viewer: replacement models' levels of detail reach PCT %% as far (display.tree_detail)\n"
        "  --view-distance X  with --lighting modern: draw X times further than the original, into haze (2.5)\n"
        "  --model A,B,...    model viewer: classic character models by code (HUM) or race (r:RACE[:GENDER]);\n"
        "                     --anim CODE (default P01), --anim-time T, --size S, --heading H (0..512),\n"
        "                     --upper-anim CODE (played by the upper body's bone group over --anim),\n"
        "                     --model-at X Y Z (with --zone: the first model's origin in the zone);\n"
        "                     appearance per model as CODE/key=value/..., or for all with --dress key=value/...:\n"
        "                     tex (NPC body texture), helm, face, hair, beard, armor (slots 1-6), head chest\n"
        "                     arms wrists"
        " hands legs feet, tint (RRGGBB, all armour), pri, sec (item numbers IT<n>),\n"
        "                     haircolor, beardcolor, eyes, heritage, tattoo, details, showhelm (0/1),\n"
        "                     npctint (npct.ini index), class, hf (hero's forge set: slot s gets N + s),\n"
        "                     hf0..hf6 (one slot's hero's forge id);\n"
        "                     --luclin: Luclin player models (as eqclient.ini enables them);\n"
        "                     --ride: the models in pairs (mount,rider,...), each rider seated on its mount's\n"
        "                     dag 0xe in P09; size=S in a model's look gives it its own spawn size;\n"
        "                     --spell-effect N [--spell-stage S]: replay spellsnew.eff effect N's stage S\n"
        "                     (0 cast, 2 impact) on every model every 3 s, on their spell points, at full\n"
        "                     density and opacity\n"
        "  --ui-demo SCREEN   developer UI demo: login, servers, chars, nochars, loading, game or items\n"
        "  --ui-scale S       UI scale (default: the display's content scale)\n"
        "  --no-audio         never open an audio device (sound settings: audio.* in hailstone.ini)\n"
        "  --validation       enable the Vulkan validation layer\n"
        "  --frames N         exit after N frames; the log then gives the mean frame time\n"
        "  --no-vsync         present without waiting for the display (measuring frame times)\n"
        "  --frame-dt S       the viewer's clock moves S seconds each frame, not with the wall clock (screenshots\n"
        "                     that match from run to run: the wind, water and sky at the same moment)\n"
        "  --verbose          debug logging\n"
        "viewer controls: hold right mouse to look, WASD move, Space/Ctrl up/down, Shift faster, Esc quit\n");
}

Options parse_args(int argc, char** argv) {
    Options o;
    if (const char* env = std::getenv("EQ_GAME_DIR")) o.game_dir = env;
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* name) { return std::strcmp(argv[i], name) == 0; };
        auto need = [&](int n) {
            if (i + n >= argc) throw std::runtime_error(std::string(argv[i]) + " needs " + std::to_string(n) + " value(s)");
        };
        if (arg("--validation")) {
            o.validation = true;
        } else if (arg("--frames")) {
            need(1);
            o.frames = std::atoi(argv[++i]);
        } else if (arg("--game-dir")) {
            need(1);
            o.game_dir = argv[++i];
        } else if (arg("--classic-dir")) {
            need(1);
            o.classic_dir = argv[++i];
        } else if (arg("--zone")) {
            need(1);
            o.zone = argv[++i];
        } else if (arg("--clip-plane")) {
            need(1);
            o.clip_plane = std::atoi(argv[++i]);
        } else if (arg("--pos")) {
            need(3);
            float x = std::strtof(argv[i + 1], nullptr), y = std::strtof(argv[i + 2], nullptr),
                  z = std::strtof(argv[i + 3], nullptr);
            o.position = glm::vec3(x, y, z);
            i += 3;
        } else if (arg("--look")) {
            need(2);
            o.yaw_deg = std::strtof(argv[i + 1], nullptr);
            o.pitch_deg = std::strtof(argv[i + 2], nullptr);
            i += 2;
        } else if (arg("--neighbours")) {
            need(1);
            std::vector<hailstone::game::ZonePointTarget> points;
            std::string list = argv[++i];
            std::size_t at = 0;
            while (at < list.size()) {
                std::size_t end = list.find(',', at);
                std::string item = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
                hailstone::game::ZonePointTarget z;
                if (std::sscanf(item.c_str(), "%u:%u:%f:%f:%f", &z.number, &z.zone_id, &z.server_x, &z.server_y,
                                &z.server_z) == 5)
                    points.push_back(z);
                if (end == std::string::npos) break;
                at = end + 1;
            }
            o.neighbours = std::move(points);
        } else if (arg("--screenshot")) {
            need(1);
            o.screenshot = hailstone::platform::screenshot_path(argv[++i]).string();
        } else if (arg("--fog")) {
            need(2);
            o.fog = glm::vec2(std::strtof(argv[i + 1], nullptr), std::strtof(argv[i + 2], nullptr));
            i += 2;
        } else if (arg("--zone-fog")) {
            need(9);
            hailstone::game::ZoneFogInput z;
            z.fog_enabled = std::atoi(argv[i + 1]) != 0;
            z.fog_color = glm::vec3(std::strtof(argv[i + 2], nullptr), std::strtof(argv[i + 3], nullptr),
                                    std::strtof(argv[i + 4], nullptr)) / 255.0f;
            z.fog_minclip = std::strtof(argv[i + 5], nullptr);
            z.fog_maxclip = std::strtof(argv[i + 6], nullptr);
            z.fog_density = std::strtof(argv[i + 7], nullptr);
            z.minclip = std::strtof(argv[i + 8], nullptr);
            z.maxclip = std::strtof(argv[i + 9], nullptr);
            o.zone_fog = z;
            i += 9;
        } else if (arg("--sky")) {
            need(1);
            o.sky = std::atoi(argv[++i]);
        } else if (arg("--vision")) {
            need(1);
            std::string_view v = argv[++i];
            o.vision = v == "infra" ? 1 : v == "ultra" ? 2 : 0;
        } else if (arg("--time")) {
            need(1);
            o.time_hour = std::strtof(argv[++i], nullptr);
        } else if (arg("--spell-effect")) {
            need(1);
            o.spell_effect = std::atoi(argv[++i]);
        } else if (arg("--plate-test")) {
            o.plate_test = true;
        } else if (arg("--plate-test-ghost")) {
            need(1);
            o.plate_test = true;
            o.plate_ghost = std::strtof(argv[++i], nullptr);
        } else if (arg("--plate-test-no-depth")) {
            o.plate_test = o.plate_plain = true;
        } else if (arg("--spell-bolt")) {
            need(1);
            o.spell_bolt = std::atoi(argv[++i]);
        } else if (arg("--sky-timescale")) {
            need(1);
            o.sky_timescale = std::strtof(argv[++i], nullptr);
        } else if (arg("--time-type")) {
            need(1);
            o.time_type = std::atoi(argv[++i]);
        } else if (arg("--lighting")) {
            need(1);
            std::string m = argv[++i];
            using hailstone::render::VertexLighting;
            o.lighting = m == "none" ? VertexLighting::None : VertexLighting::Classic;
            o.modern = m == "modern";
        } else if (arg("--msaa")) {
            need(1);
            o.scene.msaa = static_cast<std::uint32_t>(std::clamp(std::atoi(argv[++i]), 1, 16));
        } else if (arg("--tonemap")) {
            need(1);
            o.scene.tonemap = hailstone::render::parse_tonemap(argv[++i]);
            o.tonemap_given = true;
        } else if (arg("--view-distance")) {
            need(1);
            o.view_distance = std::strtof(argv[++i], nullptr);
        } else if (arg("--no-grass")) {
            o.no_grass = true;
        } else if (arg("--flora-push")) {
            need(3);
            o.flora_push = glm::vec3(std::strtof(argv[i + 1], nullptr), std::strtof(argv[i + 2], nullptr),
                                     std::strtof(argv[i + 3], nullptr));
            i += 3;
        } else if (arg("--flora-distance")) {
            need(1);
            o.flora_distance = std::strtof(argv[++i], nullptr);
        } else if (arg("--no-ao")) {
            o.scene.ambient_occlusion = false;
        } else if (arg("--depth-prepass")) {
            o.scene.depth_prepass = true;
        } else if (arg("--weather-age")) {
            need(1);
            o.weather_age = std::strtof(argv[++i], nullptr);
        } else if (arg("--weather-stopped")) {
            need(1);
            o.weather_stopped = std::strtof(argv[++i], nullptr);
        } else if (arg("--no-grade")) {
            o.scene.color_grade = false;
        } else if (arg("--no-horizon")) {
            o.no_horizon = true;
        } else if (arg("--tree-detail")) {
            need(1);
            o.tree_detail = std::strtof(argv[++i], nullptr) / 100.0f;
        } else if (arg("--weather")) {
            need(2);
            std::string kind = argv[++i];
            o.weather = std::pair(kind == "snow" ? 1 : 0, std::atoi(argv[++i]));
        } else if (arg("--textures")) {
            need(1);
            o.textures = argv[++i];
        } else if (arg("--no-material-maps")) {
            o.no_material_maps = true;
        } else if (arg("--no-fire")) {
            o.no_fire = true;
        } else if (arg("--fire")) {
            need(5);
            hailstone::render::FireSource f;
            f.base = glm::vec3(std::strtof(argv[i + 1], nullptr), std::strtof(argv[i + 2], nullptr),
                               std::strtof(argv[i + 3], nullptr));
            f.height = std::strtof(argv[i + 4], nullptr);
            f.width = std::strtof(argv[i + 5], nullptr);
            f.seed = static_cast<float>(o.fires.size()) * 1.37f;
            o.fires.push_back(f);
            i += 5;
        } else if (arg("--moon")) {
            need(1);
            o.moon_phase = std::clamp(std::strtof(argv[++i], nullptr), 0.0f, 1.0f);
        } else if (arg("--blood-moon")) {
            need(1);
            o.blood_moon = std::clamp(std::strtof(argv[++i], nullptr), 0.0f, 1.0f);
        } else if (arg("--aurora")) {
            need(1);
            o.aurora = std::clamp(std::strtof(argv[++i], nullptr), 0.0f, 1.0f);
        } else if (arg("--bloom")) {
            need(1);
            o.scene.bloom = std::clamp(std::strtof(argv[++i], nullptr), 0.0f, 1.0f);
        } else if (arg("--render-scale")) {
            need(1);
            o.scene.render_scale = static_cast<float>(std::clamp(std::atoi(argv[++i]), 50, 100)) / 100.0f;
        } else if (arg("--sharpness")) {
            need(1);
            o.scene.sharpness = std::clamp(std::strtof(argv[++i], nullptr), 0.0f, 1.0f);
        } else if (arg("--shadows")) {
            need(1);
            o.scene.shadow_size = static_cast<std::uint32_t>(std::max(0, std::atoi(argv[++i])));
        } else if (arg("--light-shadows")) {
            need(1);
            o.scene.light_shadows = static_cast<std::uint32_t>(std::clamp(std::atoi(argv[++i]), 0, 8));
        } else if (arg("--exposure")) {
            need(1);
            o.scene.exposure = std::strtof(argv[++i], nullptr);
        } else if (arg("--ui-demo")) {
            need(1);
            hailstone::app::DemoScreen screen{};
            if (!hailstone::app::parse_demo_screen(argv[++i], screen))
                throw std::runtime_error(std::string("unknown --ui-demo screen: ") + argv[i]);
            o.ui_demo = screen;
        } else if (arg("--window-size")) {
            need(1);
            std::sscanf(argv[++i], "%dx%d", &o.window_width, &o.window_height);
        } else if (arg("--ui-scale")) {
            need(1);
            o.ui_scale = std::strtof(argv[++i], nullptr);
        } else if (arg("--host")) {
            need(1);
            o.host = argv[++i];
        } else if (arg("--server")) {
            need(1);
            o.server = argv[++i];
        } else if (arg("--account")) {
            need(1);
            o.account = argv[++i];
        } else if (arg("--port")) {
            need(1);
            o.port = std::atoi(argv[++i]);
        } else if (arg("--user")) {
            need(1);
            o.user = argv[++i];
        } else if (arg("--password")) {
            need(1);
            o.password = argv[++i];
        } else if (arg("--auto-walk")) {
            need(1);
            o.auto_walk = std::strtof(argv[++i], nullptr);
        } else if (arg("--watch")) {
            need(1);
            o.watch = argv[++i];
        } else if (arg("--auto-target")) {
            o.auto_target = true;
        } else if (arg("--pick-test")) {
            o.pick_test = true;
        } else if (arg("--auto-cast")) {
            need(1);
            o.auto_cast = std::atoi(argv[++i]);
        } else if (arg("--auto-scribe")) {
            o.auto_scribe = true;
        } else if (arg("--watch-turn")) {
            need(1);
            o.watch_turn = std::strtof(argv[++i], nullptr);
        } else if (arg("--auto-door")) {
            need(1);
            o.auto_door = std::atoi(argv[++i]);
        } else if (arg("--auto-commands")) {
            need(1);
            o.auto_commands = argv[++i];
        } else if (arg("--auto-keys")) {
            need(1);
            o.auto_keys = argv[++i];
        } else if (arg("--input-script")) {
            need(1);
            o.input_script = argv[++i];
        } else if (arg("--melee-test")) {
            need(1);
            o.melee_test = std::atoi(argv[++i]);
        } else if (arg("--abilities-test")) {
            need(1);
            o.abilities_test = argv[++i];
        } else if (arg("--no-audio")) {
            o.no_audio = true;
        } else if (arg("--no-vsync")) {
            o.no_vsync = true;
        } else if (arg("--frame-dt")) {
            need(1);
            o.frame_dt = std::strtof(argv[++i], nullptr);
        } else if (arg("--auto-accept")) {
            o.auto_accept_invite = true;
        } else if (arg("--max-fps")) {
            need(1);
            o.max_fps = std::atoi(argv[++i]);
        } else if (arg("--load-stall")) {
            need(1);
            o.load_stall_ms = std::atoi(argv[++i]);
        } else if (arg("--move-test")) {
            need(1);
            o.move_test = argv[++i];
        } else if (arg("--stats")) {
            o.show_stats = true;
        } else if (arg("--show-items")) {
            need(1);
            o.show_items = std::atoi(argv[++i]);
        } else if (arg("--camera-distance")) {
            need(1);
            o.camera_distance = std::strtof(argv[++i], nullptr);
        } else if (arg("--character")) {
            need(1);
            o.character = argv[++i];
        } else if (arg("--model")) {
            need(1);
            std::string list = argv[++i];
            for (std::size_t p = 0; p <= list.size();) {
                std::size_t q = std::min(list.find(',', p), list.size());
                if (q > p) o.model.codes.push_back(list.substr(p, q - p));
                p = q + 1;
            }
        } else if (arg("--luclin")) {
            o.model.luclin = true;
        } else if (arg("--ride")) {
            o.model.ride = true;
        } else if (arg("--dress")) {
            need(1);
            o.model.look = argv[++i];
        } else if (arg("--anim")) {
            need(1);
            o.model.anim = argv[++i];
        } else if (arg("--upper-anim")) {
            need(1);
            o.model.upper_anim = argv[++i];
        } else if (arg("--spell-stage")) {
            need(1);
            o.model.spell_stage = std::atoi(argv[++i]);
        } else if (arg("--anim-time")) {
            need(1);
            o.model.anim_time = std::strtof(argv[++i], nullptr);
        } else if (arg("--size")) {
            need(1);
            o.model.size = std::strtof(argv[++i], nullptr);
        } else if (arg("--heading")) {
            need(1);
            o.model.heading = std::strtof(argv[++i], nullptr);
        } else if (arg("--model-at")) {
            need(3);
            o.model.at = glm::vec3(std::strtof(argv[i + 1], nullptr), std::strtof(argv[i + 2], nullptr),
                                   std::strtof(argv[i + 3], nullptr));
            i += 3;
        } else if (arg("--verbose")) {
            hailstone::log::set_level(hailstone::log::Level::Debug);
        } else if (arg("--help") || arg("-h")) {
            usage();
            std::exit(0);
        } else {
            hailstone::log::warn("unknown argument: {}", argv[i]);
        }
    }
    return o;
}

// mouse/keys are false while the UI has the mouse or keyboard focus.
void update_camera(hailstone::render::FlyCamera& cam, hailstone::platform::Window& window, float dt, bool mouse = true,
                   bool keys = true) {
    using hailstone::platform::Key;
    const auto& in = window.input();
    bool look = mouse && in.right_button;
    window.set_mouse_captured(look);
    if (look) cam.rotate(-in.mouse_dx * 0.0025f, -in.mouse_dy * 0.0025f);
    if (!keys) return;

    float speed = in.is_down(Key::LeftShift) ? 400.0f : 100.0f; // EQ units per second
    glm::vec3 move(0.0f);
    if (in.is_down(Key::W)) move += cam.forward();
    if (in.is_down(Key::S)) move -= cam.forward();
    if (in.is_down(Key::D)) move += cam.right();
    if (in.is_down(Key::A)) move -= cam.right();
    if (in.is_down(Key::Space)) move.z += 1.0f;
    if (in.is_down(Key::LeftCtrl)) move.z -= 1.0f;
    if (glm::dot(move, move) > 0.0f) cam.position += glm::normalize(move) * speed * dt;
}

} // namespace

int main(int argc, char** argv) {
    using namespace hailstone;
    // An error that ends the run is shown in a dialog too (app/startup_error.h), but not in the test drives that stop
    // after some frames, which nobody is there to close.
    bool error_dialog = true;
    std::filesystem::path log_written;
    try {
        Options opts = parse_args(argc, argv);
        error_dialog = opts.frames == 0 && opts.screenshot.empty();
        // The data folder (platform/paths.h) is the Hailstone folder itself. One that cannot hold the settings (a
        // read-only folder, an EverQuest folder) stops the start with a message: nothing is ever written elsewhere.
        if (std::string problem = platform::data_dir_problem(); !problem.empty()) {
            log::error("{}", problem);
            platform::show_error_box("Hailstone", problem);
            return 1;
        }
        // The log goes to logs/hailstone.log too (the viewers' to logs/viewer.log, so a run of one never pushes the
        // client's last logs out).
        const bool viewer = !opts.zone.empty() || opts.ui_demo || !opts.model.codes.empty();
        const std::filesystem::path log_path = viewer ? platform::logs_dir() / "viewer.log" : platform::log_file();
        if (!log::open_file(log_path)) log::warn("could not create {}", log_path.string());
        log_written = log::file_path();
        log::info("data folder {}: {}", platform::data_dir().string(), platform::data_dir_reason());
        if (auto file = log::file_path(); !file.empty() && file != log_path)
            log::info("another Hailstone is writing {}: this one logs to {}", log_path.string(), file.string());
        // A crash leaves logs/crash-<time>.txt with the last log lines (platform/crash_log.h).
        platform::install_crash_handler(platform::logs_dir());
        move_old_ui_settings();
        rename_old_window_ids_in(platform::imgui_ini());
        // The folders a player drops plugins, scripts, textures and maps into exist from the first start.
        for (const auto& dir : platform::create_data_dirs()) log::warn("could not create {}", dir.string());
        if (opts.game_dir.empty() && viewer) {
            // The viewers have no picker: without --game-dir or EQ_GAME_DIR they read the asset folder the client's
            // picker saved (hailstone.ini assets.path). The client itself chooses in ClientApp (the picker).
            Config config;
            config.load(default_config_path());
            std::error_code ec;
            if (std::string saved = config.get_string("assets.path"); !saved.empty() && std::filesystem::is_directory(saved, ec)) {
                opts.game_dir = saved;
                if (opts.classic_dir.empty()) opts.classic_dir = config.get_string("assets.classic", "");
            }
        }
        if (viewer) {
            // The viewers' replacement textures: --textures, else hailstone.ini's (assets/texture_overrides.h).
            Config config;
            config.load(default_config_path());
            assets::texture_overrides().set_folder(assets::texture_folder_setting(
                opts.textures ? *opts.textures : config.get_string("display.texture_folder", ""),
                platform::textures_dir()));
            assets::texture_overrides().set_material_maps(!opts.no_material_maps &&
                                                          config.get_bool("display.material_maps", true));
        }
        if (!opts.model.codes.empty()) {
            auto m = opts.model;
            m.game_dir = opts.game_dir;
            m.classic_dir = opts.classic_dir;
            m.validation = opts.validation;
            m.frames = opts.frames;
            m.screenshot = opts.screenshot;
            m.scene = opts.scene;
            m.modern = opts.modern;
            m.hour = opts.time_hour;
            if (opts.modern && !opts.tonemap_given) m.scene.tonemap = render::Tonemap::AgX;
            m.zone = opts.zone;
            m.camera_pos = opts.position;
            m.yaw_deg = opts.yaw_deg;
            m.pitch_deg = opts.pitch_deg;
            m.clip_plane = opts.clip_plane;
            m.spell_effect = opts.spell_effect;
            return app::run_model_viewer(m);
        }
        if (opts.zone.empty() && !opts.ui_demo) {
            app::ClientOptions c;
            // The updater is for a player's own runs, not the test drives that stop after some frames.
            c.updates = opts.frames == 0 && opts.screenshot.empty();
            if (c.updates) {
                // A cut-off install, or an update that never reached the login screen, is put back first; when the
                // folder then holds another version than this one, that one is started instead.
                const app::StartNote note = app::update_at_start(platform::install_dir(), release_version());
                if (note.restart) {
                    app::start_installed_client();
                    return 0;
                }
                c.update_note = note.message;
            }
            c.game_dir = opts.game_dir;
            c.classic_dir = opts.classic_dir;
            c.validation = opts.validation;
            c.frames = opts.frames;
            c.screenshot = opts.screenshot;
            c.ui_scale = opts.ui_scale;
            c.window_width = opts.window_width;
            c.window_height = opts.window_height;
            c.clip_plane = opts.clip_plane;
            c.auto_user = opts.user;
            c.auto_password = opts.password;
            core::wipe(opts.password); // run_client wipes c's copy once the client has it
            c.auto_host = opts.host;
            c.auto_port = opts.port;
            c.server = opts.server;
            c.account = opts.account;
            c.auto_character = opts.character;
            c.auto_walk_seconds = opts.auto_walk;
            c.camera_distance = opts.camera_distance;
            c.watch = opts.watch;
            c.auto_target = opts.auto_target;
            c.pick_test = opts.pick_test;
            c.auto_cast = opts.auto_cast;
            c.auto_scribe = opts.auto_scribe;
            c.show_items = opts.show_items;
            c.show_stats = opts.show_stats;
            c.move_test = opts.move_test;
            c.max_fps = opts.max_fps;
            c.load_stall_ms = opts.load_stall_ms;
            c.auto_door = opts.auto_door;
            c.watch_turn = opts.watch_turn;
            c.auto_commands = opts.auto_commands;
            c.auto_keys = opts.auto_keys;
            c.input_script = opts.input_script;
            c.abilities_test = opts.abilities_test;
            c.melee_test = opts.melee_test;
            c.auto_accept_invite = opts.auto_accept_invite;
            c.no_audio = opts.no_audio;
            c.moon_phase = opts.moon_phase;
            c.blood_moon = opts.blood_moon;
            c.aurora = opts.aurora;
            return app::run_client(c);
        }

        std::optional<assets::Zone> zone;
        if (!opts.zone.empty()) {
            if (opts.game_dir.empty()) throw std::runtime_error("--zone needs --game-dir or EQ_GAME_DIR");
            GameDir game = open_game_dir(opts.game_dir, opts.classic_dir);
            auto t0 = std::chrono::steady_clock::now();
            zone = assets::load_zone(game, opts.zone);
            if (!opts.no_grass) {
                const int tt = opts.time_type >= 0 ? opts.time_type : (opts.sky > 0 ? 1 : 0); // as the fog below
                assets::add_zone_flora(game, *zone, platform::flora_dir(), tt == 1 || tt == 2 || tt == 5);
            }
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
            log::info("loaded {}: {} terrain meshes, {} objects, {} placements, {} lights in {} ms; bounds {:.0f} {:.0f} "
                      "{:.0f} to {:.0f} {:.0f} {:.0f}",
                      zone->name, zone->terrain.size(), zone->objects.size(), zone->placements.size(),
                      zone->lights.size(), ms.count(), zone->bounds_min.x, zone->bounds_min.y, zone->bounds_min.z,
                      zone->bounds_max.x, zone->bounds_max.y, zone->bounds_max.z);
        }

        platform::Window window({});
        app::apply_window_icon(window);
        render::VkContext vk(window, opts.validation);
        std::filesystem::path shader_dir = platform::shaders_dir();
        if (opts.modern && !opts.tonemap_given) opts.scene.tonemap = render::Tonemap::AgX;
        vk.set_scene_options(opts.scene, shader_dir);
        if (opts.no_vsync) vk.set_vsync(render::VkContext::Vsync::Off);

        // The zone's torch and lantern flames (the placed objects' particle clouds), and the modern lighting's own
        // flames in their place and the flame meshes' (with a light for those that have none, before the zone
        // renderer takes the lights).
        GameDir zone_fx_game = open_game_dir(opts.game_dir, opts.classic_dir);
        std::unique_ptr<app::ZoneParticles> zone_particles;
        std::unique_ptr<render::FireRenderer> fire_renderer;
        if (zone) {
            zone_particles = std::make_unique<app::ZoneParticles>(vk, zone_fx_game, shader_dir, *zone);
            auto fires = assets::merge_fires(assets::placement_fires(*zone), zone_particles->fires());
            std::size_t lights = assets::add_fire_lights(*zone, fires);
            for (const auto& fire : fires) render::add_fire_sources(fire, opts.fires);
            log::info("zone fires: {} flames, {} lights added for them", fires.size(), lights);
            if (opts.modern && !opts.no_fire) fire_renderer = std::make_unique<render::FireRenderer>(vk, shader_dir);
        }

        std::unique_ptr<render::FloraRenderer> flora_renderer;
        std::uint32_t flora_drawn = 0;
        if (zone && opts.modern && !opts.no_grass && !zone->flora.empty())
            flora_renderer = std::make_unique<render::FloraRenderer>(vk, *zone, shader_dir);
        if (flora_renderer) flora_renderer->set_reach(opts.flora_distance);
        if (flora_renderer && opts.flora_push) {
            const glm::vec4 push(*opts.flora_push, 6.0f);
            flora_renderer->set_pushers(std::span<const glm::vec4>(&push, 1));
        }
        std::unique_ptr<render::ZoneRenderer> zone_renderer;
        render::PointShadows point_shadows;
        render::CoverMapState cover_state, water_state; // what the cover and water maps last showed
        render::FlyCamera camera;
        if (zone) {
            const GameDir game = open_game_dir(opts.game_dir, opts.classic_dir);
            auto options = render::zone_render_options(game);
            // The neighbours' ground an earlier run measured (--neighbours) shapes the land round the zone.
            if (opts.neighbours && opts.modern)
                options.horizon_sources =
                    app::cached_horizon_sources(game, app::horizon_cache_dir("viewer", zone->name), zone->name);
            zone_renderer = std::make_unique<render::ZoneRenderer>(vk, *zone, shader_dir, options);
            zone_renderer->set_hide_flames(opts.modern);
            zone_renderer->set_lod_scale(opts.tree_detail);
            const auto& s = zone_renderer->stats();
            log::info("zone renderer: {} triangles, {} draws in {} chunks, {} textures ({} missing)", s.triangles,
                      s.draws, s.chunks, s.textures, s.missing_textures);
            camera.position = opts.position.value_or((zone->bounds_min + zone->bounds_max) * 0.5f);
        }
        // --neighbours: the zones past the zone lines, loaded before the first frame (the screenshot waits for them).
        std::unique_ptr<GameDir> neighbour_game;
        std::unique_ptr<app::NeighbourZones> neighbours;
        std::uint64_t distant_hidden_key = 0; // the drawn neighbours the land round the zone was last hidden for
        std::optional<game::CollisionMesh> neighbour_collision; // the zone's, which the neighbours read until they go
        if (zone && opts.neighbours && opts.modern) {
            neighbour_game = std::make_unique<GameDir>(open_game_dir(opts.game_dir, opts.classic_dir));
            neighbours = std::make_unique<app::NeighbourZones>(vk, *neighbour_game, shader_dir);
            neighbour_collision = game::CollisionMesh::build(*zone);
            app::NeighbourZones::ZoneIn in;
            in.zone = &*zone;
            in.collision = &*neighbour_collision;
            in.points = *opts.neighbours;
            in.zone_id = game::zone_id_by_short_name(opts.zone);
            const int tt = opts.time_type >= 0 ? opts.time_type : (opts.sky > 0 ? 1 : 0);
            in.outdoor = opts.sky > 0 && (tt == 1 || tt == 2 || tt == 5);
            in.horizon = zone_renderer->horizon_build();
            in.cache_dir = app::horizon_cache_dir("viewer", zone->name);
            neighbours->enter(in);
        }
        if (opts.yaw_deg) camera.yaw = glm::radians(*opts.yaw_deg);
        if (opts.pitch_deg) camera.pitch = glm::radians(*opts.pitch_deg);

        // Without a server: --zone-fog's OP_NewZone values, else the client's no-fog fallback (no fog clips:
        // 500/600 grey, density 0, so nothing fogs). The fog then runs per frame as in game (game::SceneFog).
        game::ZoneFogInput viewer_fog = opts.zone_fog.value_or(game::ZoneFogInput{});
        if (opts.fog) {
            viewer_fog.fog_minclip = opts.fog->x;
            viewer_fog.fog_maxclip = opts.fog->y;
        }
        viewer_fog.time_type = static_cast<std::uint8_t>(opts.time_type >= 0 ? opts.time_type : (opts.sky > 0 ? 1 : 0));
        game::SceneFog scene_fog;
        scene_fog.set_zone(viewer_fog, opts.sky > 0);
        game::ViewDistance view = game::compute_view_distance(viewer_fog, opts.clip_plane);
        std::uint8_t camera_region = 0;
        if (zone_renderer) zone_renderer->set_vertex_lighting(opts.lighting);

        // Sky and time of day (viewer: --sky, --time; sky 0 is an indoor zone).
        std::unique_ptr<render::SkyRenderer> sky_renderer;
        GameDir sky_game = open_game_dir(opts.game_dir, opts.classic_dir);
        game::SkyClock sky_clock;
        if (zone && opts.sky > 0) {
            if (auto sky = assets::load_sky(open_game_dir(opts.game_dir, opts.classic_dir), opts.sky))
                sky_renderer = std::make_unique<render::SkyRenderer>(vk, *sky, shader_dir, &sky_game, opts.zone);
            if (sky_renderer) sky_renderer->set_time_scale(opts.sky_timescale);
            else
                log::warn("sky {} not found in sky.s3d", opts.sky);
        }
        {
            float h = opts.time_hour <= 0.0f ? opts.time_hour + 24.0f : opts.time_hour;
            int hour = static_cast<int>(h);
            sky_clock.set(hour, static_cast<int>((h - static_cast<float>(hour)) * 60.0f), 0.0);
        }

        // --weather: as if it had been raining or snowing for a while.
        game::Weather weather;
        render::WeatherLook weather_look;
        render::HazeBlend haze_blend;
        std::unique_ptr<render::WeatherRenderer> weather_renderer;
        std::optional<game::CollisionMesh> weather_ground;
        game::SkyState weather_sky;
        std::minstd_rand weather_random{0x5eed};
        double weather_clock = 600.0; // the weather's own clock where the frames take over
        // One frame of the precipitation's particles (ClientApp::update_precipitation's, without a player).
        auto weather_particles = [&](const render::FlyCamera& cam) {
            game::PrecipitationField::Frame f;
            if (opts.modern) f.eased_intensity = weather_look.precipitation * render::kModernPrecipitation.density;
            f.camera = cam.position;
            f.heading = std::fmod(cam.yaw * (512.0f / (2.0f * std::numbers::pi_v<float>)) + 512.0f, 512.0f);
            const std::int32_t kind = weather.particles().kind();
            if (const auto& map = weather_sky.current_map(); map && kind >= 0 && kind < 12)
                f.colours = {game::colour_cell(*map, 4 + 2 * kind), game::colour_cell(*map, 5 + 2 * kind)};
            f.random = [&] { return std::uniform_int_distribution<std::int32_t>(0, 0x7fff)(weather_random); };
            f.ground = [&](const glm::vec3& a, const glm::vec3& b,
                           bool back_faces) -> std::optional<game::PrecipitationHit> {
                auto hit = weather_ground->first_hit(a, b, back_faces);
                if (!hit) return std::nullopt;
                return game::PrecipitationHit{hit->t, hit->normal.z};
            };
            weather.update_particles(f);
        };
        if (zone && opts.weather) {
            GameDir game = open_game_dir(opts.game_dir, opts.classic_dir);
            weather_renderer = std::make_unique<render::WeatherRenderer>(vk, shader_dir);
            if (auto defs = game::PrecipitationDefinitions::load(game)) {
                weather_renderer->load_textures(game, *defs);
                weather.set_precipitation_definitions(std::move(*defs));
            }
            weather_ground = game::CollisionMesh::build(*zone);
            weather_sky = game::SkyState(game::SkyDefinitions::load(game));
            weather_sky.set_sky(zone->name);
            float h = opts.time_hour <= 0.0f ? opts.time_hour + 24.0f : opts.time_hour;
            weather_sky.set_time_of_day(std::clamp(h / 24.0f, 0.0f, 1.0f));
            weather.handle_message(opts.weather->first, opts.weather->second);
            auto steady = [] { return std::int32_t{0x4000}; };
            // --weather-age S: S seconds after it began (default 600, settled); --weather-stopped S: stopped S seconds
            // ago after 600 seconds (the clearing).
            const double began = opts.weather_age >= 0.0f ? opts.weather_age : 600.0;
            const double run = began + std::max(opts.weather_stopped, 0.0f);
            for (double s = 0.0; s < run; s += 0.1) {
                if (opts.weather_stopped >= 0.0f && s >= began && s - 0.1 < began) weather.handle_message(0xff, 0);
                weather.update(s, true, steady);
                weather_look.update(weather, 0.1f);
                weather_sky.update(100.0f, weather.precipitation(), 0xff000000u);
                if (s > run - 30.0) weather_particles(camera);
            }
            weather_clock = run;
            weather_look.flash = 0.0f;
            weather_look.strike_age = 10.0f;
        }

        // --spell-effect: a spell effect's particle stages in front of the camera.
        GameDir fx_game = open_game_dir(opts.game_dir, opts.classic_dir);
        std::unique_ptr<game::ParticleSystem> fx_particles;
        std::unique_ptr<render::ParticleRenderer> fx_renderer;
        std::vector<assets::SpellEffect> fx_effects;
        game::SpellParticleSettings fx_settings;
        if (zone && opts.spell_effect > 0) {
            if (auto edd = fx_game.read("spellsnew.edd"))
                fx_particles = std::make_unique<game::ParticleSystem>(assets::parse_edd(*edd));
            if (auto eff = fx_game.read("spellsnew.eff")) fx_effects = assets::parse_spell_effects(*eff);
            IniFile ini;
            if (auto path = fx_game.find("eqclient.ini")) ini.load(*path);
            auto value = [&](std::string_view key) -> std::optional<float> {
                auto v = ini.get("Defaults", key);
                if (!v || v->empty()) return std::nullopt;
                return std::strtof(v->c_str(), nullptr);
            };
            fx_settings = game::spell_particle_settings(value("SpellParticleDensity"), value("SpellParticleOpacity"));
            if (fx_particles && static_cast<std::size_t>(opts.spell_effect) < fx_effects.size())
                fx_renderer = std::make_unique<render::ParticleRenderer>(vk, fx_game, shader_dir);
            else
                log::warn("spell effect {} not available", opts.spell_effect);
        }
        double fx_next = 0.0;
        // --spell-bolt: a spell's bolt or comet from the lower left to the right of the view, every 2.5 s.
        game::SpellTable bolt_spells;
        std::unique_ptr<game::SpellProjectiles> bolts;
        std::unique_ptr<render::SpellFxRenderer> bolt_renderer;
        std::vector<render::SpellFxPiece> bolt_pieces;
        game::ProjectileStyle bolt_style;
        glm::vec3 bolt_from(0.0f), bolt_to(0.0f);
        double bolt_next = 0.0;
        if (zone && opts.spell_bolt > 0) {
            if (bolt_spells.load(fx_game))
                if (const game::SpellData* sp = bolt_spells.get(static_cast<std::uint32_t>(opts.spell_bolt)))
                    bolt_style = game::projectile_style(*sp);
            if (bolt_style.form != game::ProjectileForm::None && opts.modern) {
                bolts = std::make_unique<game::SpellProjectiles>();
                bolt_renderer = std::make_unique<render::SpellFxRenderer>(vk, shader_dir);
                log::info("spell bolt {}: form {} element {}", opts.spell_bolt, static_cast<int>(bolt_style.form),
                          static_cast<int>(bolt_style.element));
            } else {
                log::warn("spell {} has no bolt or comet (or the lighting is not modern)", opts.spell_bolt);
            }
        }

        // --plate-test: name plates drawn as the client draws them (app/nameplate_layer.h) on ImGui's own layer, over
        // the zone, placed from the CPU's rays (app/plate_test.h) for the view the camera starts in (it must stand still).
        std::optional<game::CollisionMesh> plate_collision;
        std::unique_ptr<ui::ImGuiLayer> plate_ui;
        std::unique_ptr<app::NameplateLayer> plate_layer;
        std::vector<app::TestPlate> test_plates;
        if (zone && opts.plate_test) {
            plate_collision = game::CollisionMesh::build(*zone);
            plate_ui = std::make_unique<ui::ImGuiLayer>(window, vk, ui::ImGuiLayerDesc{});
            plate_layer = std::make_unique<app::NameplateLayer>(vk, shader_dir);
            plate_layer->set_depth_test(!opts.plate_plain);
            plate_layer->set_occluded_alpha(opts.plate_ghost);
        }

        // Developer UI demo (temporary until the stock UI engine, M5).
        std::unique_ptr<ui::ImGuiLayer> ui_layer;
        std::unique_ptr<app::UiDemo> ui_demo;
        if (opts.ui_demo) {
            ui::ImGuiLayerDesc desc;
            desc.scale_override = opts.ui_scale;
            ui_layer = std::make_unique<ui::ImGuiLayer>(window, vk, desc);
            ui_demo = std::make_unique<app::UiDemo>(*opts.ui_demo);
        }
        // Item icons for the demo's items screen; destroyed before the ImGui layer.
        std::unique_ptr<GameDir> icon_game;
        std::unique_ptr<ui::ItemIconCache> item_icons;
        if (ui_demo && !opts.game_dir.empty()) {
            icon_game = std::make_unique<GameDir>(open_game_dir(opts.game_dir, opts.classic_dir));
            item_icons = std::make_unique<ui::ItemIconCache>(vk, *icon_game);
            ui_demo->set_item_icons(item_icons.get());
        }

        // The zone's ambient sounds and music at the camera (audio/), the viewer's --time as the time of day.
        std::unique_ptr<GameDir> audio_game;
        std::unique_ptr<audio::AudioSystem> audio;
        if (zone) {
            Config config;
            config.load(default_config_path());
            audio_game = std::make_unique<GameDir>(open_game_dir(opts.game_dir, opts.classic_dir));
            audio = std::make_unique<audio::AudioSystem>(*audio_game, audio::load_audio_settings(config), !opts.no_audio);
            audio->set_zone(opts.zone);
            audio->set_hour(static_cast<int>(opts.time_hour <= 0.0f ? opts.time_hour + 24.0f : opts.time_hour));
        }

        auto start = std::chrono::steady_clock::now();
        auto last = start;
        double title_timer = 0.0;
        int frames_since_title = 0;
        // With --frames, the mean frame time over all but the first quarter of the frames (loading, the pipeline
        // cache and the swapchain settle in those).
        const int timed_from = opts.frames / 4;
        auto timed_start = start;
        render::ZoneDrawn drawn; // the last frame's
        for (int frame = 0; window.poll_events(); ++frame) {
            if (opts.frames && frame == timed_from) timed_start = std::chrono::steady_clock::now();
            if (opts.frames && frame >= opts.frames) {
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - timed_start).count();
                if (frame > timed_from)
                    log::info("frame time: {:.3f} ms mean over frames {}..{} ({:.0f} fps); the last drew {} zone "
                              "triangles in {} calls, {} into shadows in {} calls",
                              ms / (frame - timed_from), timed_from, frame - 1, 1000.0 * (frame - timed_from) / ms,
                              drawn.triangles, drawn.calls, drawn.shadow_triangles, drawn.shadow_calls);
                if (flora_renderer) log::info("flora: the last frame drew {} plants", flora_drawn);
                if (neighbours)
                    log::info("neighbour zones: {} loaded, the last frame drew {} triangles in {} calls, {:.3f} ms of CPU",
                              neighbours->loaded(), neighbours->drawn_triangles(), neighbours->drawn_calls(),
                              neighbours->draw_ms());
                break;
            }
            if (!opts.screenshot.empty() && opts.frames && frame == opts.frames - 1) vk.request_screenshot(opts.screenshot);
            bool ui_keys = ui_layer && ui_layer->wants_keyboard();
            bool ui_mouse = ui_layer && ui_layer->wants_mouse();
            if (window.input().was_pressed(platform::Key::Escape) && !ui_keys) break;

            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            double t = std::chrono::duration<double>(now - start).count();
            if (opts.frame_dt > 0.0f) {
                dt = opts.frame_dt;
                t = frame * static_cast<double>(opts.frame_dt);
            }
            // While the camera is being dragged the UI must not take the mouse.
            update_camera(camera, window, dt, !ui_mouse || window.input().right_button, !ui_keys);

            if (ui_layer) {
                ui_layer->new_frame();
                ui_demo->draw({dt, opts.zone, camera.position.x, camera.position.y, camera.position.z,
                               glm::degrees(camera.yaw)});
                if (ui_demo->quit_requested()) break;
            }

            title_timer += dt;
            ++frames_since_title;
            if (title_timer >= 0.5) {
                window.set_title(std::format("Hailstone {}  {:.0f} fps  pos {:.0f} {:.0f} {:.0f}", opts.zone,
                                             frames_since_title / title_timer, camera.position.x, camera.position.y,
                                             camera.position.z));
                title_timer = 0.0;
                frames_since_title = 0;
            }

            if (audio) {
                audio->set_listener(camera.position, camera.yaw);
                audio->update(static_cast<std::uint32_t>(t * 1000.0) + 1);
            }
            if (zone_particles) zone_particles->update(dt, camera.position);
            if (fx_renderer) {
                if (t >= fx_next) {
                    fx_next = t + 4.0;
                    glm::vec3 fwd(std::cos(camera.yaw) * std::cos(camera.pitch), std::sin(camera.yaw) * std::cos(camera.pitch),
                                  std::sin(camera.pitch));
                    game::StageActor at{1, camera.position + fwd * 25.0f, 100, 6.0f};
                    const auto& fx = fx_effects[static_cast<std::size_t>(opts.spell_effect)];
                    for (int stage : {0, 2})
                        for (const auto& r : game::spawn_stage_emitters(fx, stage, 1, &at, &at, at.position, 3000, fx_settings))
                            fx_particles->create(r);
                    log::info("spell effect {} ({}): {} emitters", opts.spell_effect, fx.name, fx_particles->emitters().size());
                }
                fx_particles->update(dt, {});
                fx_renderer->prepare(*fx_particles);
            }
            if (bolts) {
                glm::vec3 fwd(std::cos(camera.yaw) * std::cos(camera.pitch), std::sin(camera.yaw) * std::cos(camera.pitch),
                              std::sin(camera.pitch));
                glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 0.0f, 1.0f)));
                if (t >= bolt_next) {
                    bolt_next = t + 2.5;
                    bolt_from = camera.position + fwd * 14.0f - right * 6.0f - glm::vec3(0.0f, 0.0f, 3.0f);
                    bolt_to = camera.position + fwd * 75.0f + right * 8.0f - glm::vec3(0.0f, 0.0f, 2.0f);
                    bolts->landed(static_cast<std::uint32_t>(opts.spell_bolt), bolt_style, 1, 2, t,
                                  glm::length(bolt_to - bolt_from));
                }
                game::ProjectileWorld bw;
                bw.hands = [&](std::uint32_t id, const glm::vec3&) -> std::optional<glm::vec3> {
                    return id == 1 ? std::optional<glm::vec3>(bolt_from) : std::nullopt;
                };
                bw.chest = [&](std::uint32_t id) -> std::optional<glm::vec3> {
                    return id == 2 ? std::optional<glm::vec3>(bolt_to) : std::nullopt;
                };
                bw.feet = [&](std::uint32_t id) -> std::optional<glm::vec3> {
                    return id == 2 ? std::optional<glm::vec3>(bolt_to - glm::vec3(0.0f, 0.0f, 3.0f)) : std::nullopt;
                };
                bolts->update(t, bw);
                bolt_pieces.clear();
                app::projectile_pieces(*bolts, t, bolt_pieces);
            }
            float sky_hour = sky_clock.hour(t);
            if (weather_renderer) {
                weather.update(weather_clock + t, true, [] { return std::int32_t{0x4000}; });
                weather_look.update(weather, dt);
                weather_sky.set_time_of_day(std::clamp(sky_hour / 24.0f, 0.0f, 1.0f));
                weather_sky.update(dt * 1000.0f, weather.precipitation(), 0xff000000u);
                weather_particles(camera);
            }
            const float moon_phase = opts.moon_phase >= 0.0f ? opts.moon_phase : 0.5f;
            const float blood_moon = std::max(opts.blood_moon, 0.0f);
            game::SceneFogFrame fog_frame;
            {
                game::SceneFogInput in;
                float h = std::fmod(sky_hour, 24.0f);
                if (h < 0.0f) h += 24.0f;
                int hour = static_cast<int>(std::floor(h));
                in.minute = std::clamp(static_cast<int>((h - static_cast<float>(hour)) * 60.0f), 0, 59);
                in.hour = hour < 1 ? hour + 24 : hour;
                // The region type at the camera (outside the world the last one stays).
                if (zone && !zone->regions.empty() && zone->regions.in_world(camera.position))
                    camera_region = static_cast<std::uint8_t>(zone->regions.lookup(camera.position).flags & 0xff);
                in.camera_region = camera_region;
                in.clip_plane = opts.clip_plane; // no player actor: light level 1
                in.infravision_innate = opts.vision == 1;
                in.ultravision_innate = opts.vision == 2;
                fog_frame = scene_fog.frame(in);
                view = fog_frame.view;
            }
            auto f = vk.begin_frame_commands();
            if (!f) {
                if (ui_layer) ui_layer->end_frame_without_render();
                continue;
            }
            int width = static_cast<int>(f->extent.width), height = static_cast<int>(f->extent.height);
            // The modern mode draws further, into its haze (render::modern_view).
            render::ModernView modern_view{view, 0.0f};
            const std::uint8_t time_type = viewer_fog.time_type;
            const bool outdoor = time_type == 1 || time_type == 2 || time_type == 5;
            render::HazeTarget haze; // as the client's (render/atmosphere.h)
            if (opts.modern) {
                const render::Daylight sun = render::daylight(sky_hour, true, moon_phase, blood_moon);
                haze = haze_blend.update(render::haze_target(view, opts.view_distance, fog_frame.sky, outdoor,
                                                             sky_renderer != nullptr && fog_frame.sky, sun.to_sun,
                                                             weather_look.storm, weather_look.snowing),
                                         dt);
                modern_view = render::modern_view(view, opts.view_distance, haze.extend);
            }
            const game::ViewDistance& draw_view = modern_view.view;
            glm::vec4 horizon_haze(0.0f);
            render::SceneLighting frame_lighting{}; // the modern lighting as the zone took it, for the fire and particles
            render::CoverMap cover;
            bool covering = false;
            std::optional<glm::vec3> water_color; // under water (render::set_underwater)
            if (zone_renderer) {
                // The lighting and the shadow cascades come before the scene.
                game::SceneLightInput light;
                light.time_type = viewer_fog.time_type;
                light.hour = sky_hour;
                light.global_ambient = zone->global_ambient;
                light.player_light = fog_frame.light_level;
                light.ambient_colour = fog_frame.ambient;
                light.special_colour = fog_frame.special_ambient;
                light.vision_lift = fog_frame.vision_lift;
                game::SceneLight scene_light = game::compute_scene_light(light);
                zone_renderer->set_scene_light(scene_light);
                if (opts.modern) {
                    std::uint8_t tt = viewer_fog.time_type;
                    // --spell-effect: its glow lights the zone (game::particle_lights).
                    std::vector<game::LightSource> glows;
                    if (fx_particles && fx_renderer)
                        glows = game::particle_lights(
                            *fx_particles, [&](const std::string& texture) { return fx_renderer->texture_color(texture); }, 8);
                    if (bolts)
                        for (const auto& l : app::projectile_lights(*bolts, t, 6)) glows.push_back(l);
                    auto lights = game::select_lights(zone->lights, glows, camera.position, 600.0f, render::kMaxSceneLights);
                    render::Daylight sky = render::daylight(sky_hour, tt == 1 || tt == 2 || tt == 5, moon_phase, blood_moon);
                    render::apply_storm(sky, weather_look.storm);
                    auto lighting = render::make_scene_lighting(scene_light, lights, glows.size(), render::LightingMode::Modern, sky);
                    bool shadows = vk.shadows_enabled() && sky.light != glm::vec3(0.0f);
                    if (shadows) {
                        auto cascades = render::fit_shadow_cascades(camera.shadow_camera(width, height), sky.to_light,
                                                                    std::min(render::kShadowDistance, draw_view.far_plane),
                                                                    render::kShadowCasterReach, vk.shadow_size());
                        render::set_shadows(lighting, cascades, vk.shadow_size(), camera.position, camera.forward());
                    }
                    if (assets::is_water_type(camera_region)) {
                        water_color = render::underwater_color(sky);
                        render::set_underwater(lighting, camera.position, *water_color);
                    } else {
                        render::set_haze_linear(lighting, camera.position, haze.length,
                                                render::lightning_haze(haze.color, weather_look.flash, weather_look.storm));
                    }
                    render::set_screen(lighting, camera.ao_view(width, height, draw_view.far_plane).near_plane,
                                       draw_view.far_plane, f->extent.width, f->extent.height, t);
                    render::set_weather(lighting, weather_look);
                    lighting.wind = render::wind_at(t, weather_look.storm);
                    covering = vk.shadows_enabled() && weather_renderer != nullptr;
                    const bool watering = vk.shadows_enabled() && zone_renderer->has_water();
                    if (covering || watering) {
                        cover = render::fit_cover_map(camera.position, vk.shadow_size());
                        render::set_cover(lighting, cover, covering, watering);
                    }
                    horizon_haze = glm::vec4(glm::vec3(lighting.haze_color), 1.0f);
                    std::vector<std::uint32_t> point_builds;
                    if (vk.point_shadow_slots() > 0) {
                        // --spell-effect's glow casts shadows as a spell's does in the client.
                        render::DynamicShadowLights moving;
                        moving.spells = glows;
                        render::sync_point_shadows(point_shadows, vk);
                        point_shadows.set_dynamic(moving, camera.position);
                    }
                    if (vk.point_shadow_slots() > 0)
                        point_builds = render::plan_point_shadows(point_shadows, vk, zone->lights, camera.position, dt,
                                                                  nullptr, lighting);
                    zone_renderer->set_lighting(lighting);
                    frame_lighting = lighting;
                    if (neighbours) {
                        neighbours->update(camera.position, draw_view.far_plane, true);
                        // The first frame waits for the loads the camera's place starts (a screenshot shows them).
                        while (frame == 0 && neighbours->busy()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                            neighbours->update(camera.position, draw_view.far_plane, true);
                        }
                        neighbours->set_scene_light(scene_light);
                        neighbours->set_lighting(lighting);
                        // As the client: no land round the zone where a neighbour is drawn.
                        if (neighbours->drawn_key() != distant_hidden_key) {
                            distant_hidden_key = neighbours->drawn_key();
                            std::vector<std::int64_t> cells;
                            neighbours->footprint(cells);
                            zone_renderer->set_distant_hidden(std::move(cells));
                        }
                    }
                    if (vk.point_shadow_slots() > 0)
                        render::draw_point_shadows(vk, *f, point_shadows, zone->lights, point_builds, *zone_renderer,
                                                   nullptr);
                    if (shadows)
                        for (std::uint32_t c = 0; c < render::kShadowCascades; ++c) {
                            vk.begin_shadow_cascade(*f, c);
                            zone_renderer->draw_shadow(*f, c);
                            vk.end_shadow_cascade(*f);
                        }
                    // Each map only when what it shows has changed (render/cover_map.h).
                    const std::uint64_t contents =
                        render::cover_contents(zone_renderer->id(), vk.shadow_generation(), 0);
                    const bool draw_cover = render::cover_map_stale(
                        cover_state, covering, cover, contents,
                        covering && zone_renderer->map_moves(render::kCoverLayer));
                    const bool draw_water = render::cover_map_stale(
                        water_state, watering, cover, contents,
                        watering && zone_renderer->map_moves(render::kWaterLayer));
                    for (std::uint32_t layer : {render::kCoverLayer, render::kWaterLayer}) {
                        if (!(layer == render::kCoverLayer ? draw_cover : draw_water)) continue;
                        vk.begin_shadow_cascade(*f, layer);
                        zone_renderer->draw_shadow(*f, layer);
                        vk.end_shadow_cascade(*f);
                    }
                }
            }
            vk.begin_scene(*f, glm::vec4(water_color ? *water_color : view.fog_color, 1.0f));
            if (zone_renderer && opts.modern) {
                vk.set_ao_view(camera.ao_view(width, height, draw_view.far_plane));
                const render::Daylight day = render::daylight(sky_hour, outdoor, moon_phase, blood_moon);
                const auto g = render::color_grade(day, outdoor, render::linear_color(view.fog_color), weather_look.storm);
                vk.set_color_grade(glm::vec4(g.tint, g.saturation));
                if (fog_frame.sky) {
                    render::Daylight sun = render::daylight(sky_hour, true, moon_phase, blood_moon);
                    render::apply_storm(sun, weather_look.storm);
                    glm::mat4 vp = camera.projection(width, height, draw_view.far_plane) * camera.view();
                    render::SunShafts shafts = render::sun_shafts_for(vp, sun.to_sun, sun.light * 0.35f);
                    shafts.strength *= 1.0f - weather_look.storm;
                    vk.set_sun_shafts(shafts);
                }
            }
            if (zone_renderer) {
                glm::mat4 proj = camera.projection(width, height, draw_view.far_plane);
                glm::mat4 vp = proj * camera.view();
                render::SkyFrame sky_frame{sky_hour, t, horizon_haze};
                render::Daylight day = render::daylight(sky_hour, true, moon_phase, blood_moon);
                render::apply_storm(day, weather_look.storm);
                if (opts.modern) {
                    sky_frame.atmosphere = true;
                    sky_frame.to_sun = day.to_sun;
                    sky_frame.to_moon = day.to_moon;
                    sky_frame.blood_moon = day.blood_moon;
                    sky_frame.aurora = std::max(opts.aurora, 0.0f);
                    sky_frame.cloud_light = render::cloud_light(day);
                    sky_frame.storm = weather_look.storm;
                }
                const bool sky_drawn = sky_renderer && fog_frame.sky;
                if (sky_drawn) sky_renderer->draw(*f, proj, camera.view(), sky_frame);
                // As the client: the modern haze behind everything where no sky is drawn; under water, the water.
                if (water_color || (opts.modern && !sky_drawn)) zone_renderer->draw_backdrop(*f, vp);
                // The land round an outdoor zone, under an open sky or none (not the planes' painted skies).
                zone_renderer->set_distant(opts.modern && outdoor && !opts.no_horizon &&
                                           (!sky_renderer || sky_renderer->has_atmosphere()));
                if (opts.modern) {
                    // The water reads the opaque world (VkContext::begin_translucent).
                    zone_renderer->draw_depth(*f, vp, camera.position, draw_view, t);
                    zone_renderer->draw(*f, vp, camera.position, draw_view, t, render::ZonePhase::Opaque);
                    if (neighbours) neighbours->draw(*f, vp, camera.position, draw_view, t, render::ZonePhase::Opaque);
                    if (flora_renderer) flora_drawn = flora_renderer->draw(*f, vp, camera.position, t, frame_lighting, true);
                    vk.begin_translucent(*f);
                    zone_renderer->draw(*f, vp, camera.position, draw_view, t, render::ZonePhase::Translucent);
                    if (neighbours) neighbours->draw(*f, vp, camera.position, draw_view, t, render::ZonePhase::Translucent);
                } else {
                    zone_renderer->draw(*f, vp, camera.position, draw_view, t);
                }
                if (zone_particles)
                    zone_particles->draw(*f, vp, camera.view(), opts.modern ? render::kParticleGlow : 1.0f, !opts.modern,
                                         opts.modern ? &frame_lighting : nullptr);
                if (fire_renderer && opts.modern)
                    fire_renderer->draw(*f, vp, camera.position, t, opts.fires, frame_lighting, 1500.0f,
                                        render::kParticleGlow);
                if (bolt_renderer)
                    bolt_renderer->draw(*f, vp, camera.position, t, bolt_pieces, frame_lighting, render::kParticleGlow);
                if (fx_renderer)
                    fx_renderer->draw(*f, vp, camera.view(), *fx_particles, opts.modern ? render::kParticleGlow : 1.0f, {},
                                      opts.modern ? &frame_lighting : nullptr);
                if (weather_renderer)
                    weather_renderer->draw(*f, vp, camera.position, camera.forward(),
                                           2.0f / (static_cast<float>(std::max(1, width)) * proj[0][0]),
                                           weather.particles(), 1.0f,
                                           opts.modern ? render::kModernPrecipitation : render::kClassicPrecipitation);
            }
            if (plate_ui && frame >= 3) {
                plate_ui->new_frame();
                const ImVec2 disp = ImGui::GetIO().DisplaySize;
                const glm::vec2 scale(disp.x / static_cast<float>(width), disp.y / static_cast<float>(height));
                const std::string name = "TEST PLATE 00", line = "(a second line)";
                if (test_plates.empty()) {
                    app::ViewRays rays;
                    const auto ao = camera.ao_view(width, height, draw_view.far_plane);
                    rays.position = camera.position;
                    rays.forward = camera.forward();
                    rays.right = camera.right();
                    rays.up = glm::cross(rays.right, rays.forward);
                    rays.tan_half_x = ao.tan_half_x;
                    rays.tan_half_y = ao.tan_half_y;
                    rays.width = width;
                    rays.height = height;
                    rays.far_plane = draw_view.far_plane;
                    const ImVec2 ns = ImGui::CalcTextSize(name.c_str()), ls = ImGui::CalcTextSize(line.c_str());
                    const float bw = std::max(ns.x, ls.x) / scale.x, bh = ns.y * 2.0f / scale.y;
                    test_plates = app::make_test_plates(*plate_collision, rays, 5, 3, bw, bh);
                    static constexpr const char* kinds[] = {"front", "behind", "partial"};
                    for (const auto& p : test_plates)
                        log::info("test plate {} {} {} {} {} {}", p.id, kinds[static_cast<int>(p.kind)],
                                  static_cast<int>(p.top_middle.x - bw * 0.5f), static_cast<int>(p.top_middle.y),
                                  static_cast<int>(p.top_middle.x + bw * 0.5f), static_cast<int>(p.top_middle.y + bh));
                }
                plate_layer->begin(*f, ImGui::GetBackgroundDrawList(), glm::vec4(0.0f, 0.0f, disp.x, disp.y), camera.near_plane,
                                   draw_view.far_plane);
                for (const auto& tp : test_plates) {
                    app::NameplateText plate;
                    plate.pos = tp.top_middle * scale;
                    plate.depth = tp.depth;
                    plate.name = name;
                    plate.lines = {line};
                    plate.color = IM_COL32(255, 0, 255, 255);
                    plate_layer->draw(plate);
                }
                plate_ui->render(*f);
            }
            if (ui_layer) ui_layer->render(*f);
            vk.end_frame(*f);
            if (zone_renderer) drawn = zone_renderer->take_drawn();
        }
        return 0;
    } catch (const std::exception& e) {
        log::error("{}", e.what());
        if (error_dialog) platform::show_error_box("Hailstone", app::startup_error_text(e.what(), log_written));
        return 1;
    }
}
