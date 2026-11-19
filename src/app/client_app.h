#pragma once

#include <string>
#include <utility>
#include <vector>

#include "app/login_launch.h"

namespace hailstone::app {

struct ClientOptions {
    std::string game_dir;
    std::string classic_dir; // --classic-dir: the classic folder, over hailstone.ini assets.classic
    bool validation = false;
    int frames = 0;          // exit after this many frames (0 = until closed)
    std::string screenshot;  // with frames: save the last frame as PNG
    bool screenshot_generated = false; // --screenshot-generated: screenshots save frame generation's in-between frame
    float ui_scale = 0.0f;   // > 0 overrides the display scale
    int window_width = 1280, window_height = 720; // --window-size WxH
    bool window_size_given = false; // --window-size was given (any size, 1280x720 too): the saved window is not used
    int clip_plane = 15;     // eqclient.ini ClipPlane, 0..20
    // The display flags given (--lighting, --msaa ...: app/display_flags.h), as hailstone.ini settings for this run
    // only: they go over the saved ones and are never saved (Config::set_run_override).
    std::vector<std::pair<std::string, std::string>> display_settings;

    // Testing aids: log in, take the first server and enter the world with
    // this character without any clicks. Empty = interactive.
    std::string auto_user;
    std::string auto_password;
    LoginLaunch login_launch; // one-use launcher ticket and optional world; never persisted
    std::string auto_host;  // test logins: this server (default: the dev server, never the remembered one)
    int auto_port = 0;
    std::string server;     // --server: the saved server (id or name) picked on the login screen and by test logins
    // --account: log in to the saved server (--server, else the last used) with this saved account and its password
    // from the system keyring, play the world played last there and enter --character, else the account's own (a
    // box set's other windows are started so). Not a test login: the server's files and character files are used.
    std::string account;
    std::string auto_character;
    float auto_walk_seconds = 0.0f; // walk forward this long after zoning in
    float camera_distance = 0.0f;   // start in third person at this distance
    std::string watch;              // testing: point the camera at the nearest spawn whose name contains this
    bool auto_target = false;       // target the nearest NPC and consider it after zoning in
    bool pick_test = false;         // testing: mouse picks at spawns' and the nearest door's screen points
    std::string auto_keys;          // key commands by name (','-separated), one press and release every 2 s (testing)
    std::string input_script;       // testing: made-up pad, key and mouse input after zoning in (app/input_script.h)
    int auto_cast = 0;              // testing: cast gem N (1..12), or -1 open the spell book, after zoning in
    bool auto_scribe = false;       // testing: scribe and memorize the first scroll in the bags into gem 1 first
    int show_items = 0;             // testing, after zoning in: 1 open the inventory and bags, 2 pick up the first
                                    // general item, 4 ask for the primary weapon's info by item link
    std::string move_test;          // testing (app/movement_test.h): speed, jump, fall:H, swim
    bool show_stats = false;        // open the developer character / stats window
    int max_fps = 0;                // > 0: at most this many frames a second (the physics is per frame)
    int load_stall_ms = 0;          // > 0: sleep this long after loading a zone (testing slow loads)
    float watch_turn = 0.0f;        // testing: with --watch door..., view from this many degrees around
    int auto_door = 0;              // testing: walk to and click the nearest door after zoning in (2: hinged doors only)
    std::string auto_commands;      // testing: chat lines (';'-separated) typed one every 3 s after zoning in
    bool auto_accept_invite = false; // testing: accept group invitations 2 s after they come
    std::string abilities_test;     // testing: skill ids put on hot bar 1 and pressed in turn (app/actions_ui.h)
    int melee_test = 0;             // testing: seconds of auto attack on a nearby rat, then the range / facing refusals
    bool no_audio = false;          // never open an audio device (the sound still runs, silently)
    float moon_phase = -1.0f;       // testing: >= 0 the moon's phase (0 new, 0.5 full) instead of the calendar's
    float blood_moon = -1.0f;       // testing: >= 0 the blood moon (0..1) instead of the calendar's
    float aurora = -1.0f;           // testing: >= 0 the northern lights (0..1) instead of the calendar's
    // The updater (app/updater.h): on for a player's own run (main.cpp: no viewer, no test frames or screenshot),
    // and what update_at_start found (a rolled-back update), for the login screen.
    bool updates = false;
    std::string update_note;
};

// The playable client: login, server and character select, then the zone. options.auto_password is wiped once the
// client has taken it (core/wipe.h).
int run_client(ClientOptions& options);

} // namespace hailstone::app
