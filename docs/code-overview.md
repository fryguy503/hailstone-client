# Code overview

A map of the repository for someone new to it: what each folder holds, how the pieces connect at run time, and where
to look for a given feature. The goals and rules behind it are in [docs/plan.md](plan.md); where the built client
keeps its files on disk is in [docs/layout.md](layout.md). This page names files only as examples. When it disagrees
with the code, the code is right; fix this page.

## The top level

| Folder | What is in it |
|---|---|
| `src/` | The client: about 950 C++20 sources in layered folders (below). |
| `ui/` | The Lua UI packs: `ui/modern/` (mouse and keyboard) and `ui/gamepad/`. One `.lua` file per window, shared helpers in each pack's `lib/`. Copied beside the executable at build time (`cmake/SyncUiPacks.cmake`). |
| `shaders/` | GLSL, compiled to SPIR-V at build time (`cmake/Shaders.cmake`). Named after the renderer that uses them (`actor.*`, `sky.*`, `flora.*`, `post.frag`). |
| `sdk/` | The compiled-plugin SDK: `hailstone_plugin.h` (the C ABI) and `hailstone_byte_io.h`. The `nmsc_*.h` files are the old names, kept for existing plugins. |
| `plugins/example/` | The SDK's example plugin, also a test fixture. Server plugins are cloned into `plugins/<name>/` from their own repositories and ignored by git. |
| `server/` | The drop-in Lua quest script that gives an EQEmu server Hailstone support, its installer and `README.md`. |
| `tests/` | The doctest suite (`hailstone_tests`), the mock EQEmu server (`tests/mock_server/`), fuzz targets and test plugins. |
| `tools/` | Command-line tools (`pfsdump`, `wlddump`, `eqgdump`, `texdump`, `chrdump`, `audiodump`, `netprobe`, `playprobe`, `texpack`, `hailstone_check`, `hailstone_sign`), the browser helper (`tools/hailstone-web/`), generators (`gen_*.py`), release scripts (`make_update.py`, `make_addon.py`, `docker/`) and `sync_wiki.py`. |
| `cmake/` | Dependency fetching (`Dependencies.cmake`), shaders, build info, toolchains, Windows resources. |
| `third_party/` | Sources vendored into the tree: Lua 5.4 and AMD FSR 1. Everything else is fetched by CMake. |
| `flora/` | Plain-text grass and plant tables per zone or zone group (docs/graphics.md). |
| `docs/` | Design, protocol, format and mechanics documents; the source of this wiki. |
| `icons/`, `pictures/` | Hailstone's own icon and pictures. No game art is ever in the repository. |

The build makes one static library, `hailstone_engine`, from nearly all of `src/`. The client (`hailstone`), the tools
and the tests link against it. `CMakeLists.txt` lists every source by hand: a new `.cpp` that is not added to a list
compiles nowhere.

## The layers in `src/`

Each folder may include only the folders below it:

```
app                                      the client itself: main loop, screens, eq.* API for the UI packs
 ├─ mq, plugin, nav                      scripts, compiled plugins, navigation
 ├─ game   ui   render   audio           rules and state, UI host, Vulkan renderers, sound
 ├─ assets   net   web                   game files, network protocol, browser helper protocol
 └─ core   platform                      utilities; every OS call
```

`game/` never includes Vulkan headers, and only `platform/` calls the OS. That is what lets the tests run the game
rules, packet code and asset loaders without a window, a GPU or a network.

### `core/`: utilities with no OS calls

- `byte_io.h`: `ByteReader` / `ByteWriter`. Every packet and file is read and written through them. No struct casts.
- `config.h`, `ini_file.h`: `hailstone.ini` and the game's own `.ini` files.
- `game_dir.h`, `asset_folder.h`: the player's game folder, with a server's files layered over it, and the check for
  a folder the player picks.
- `log.h`, `step_clock.h` (the 60-a-second game clock), `frame_sections.h` (hitch log).
- Hashing and signatures (`sha1`, `sha256`, `ed25519`), zip reading and writing, atomic file writes.
- Add-on and update manifests (`addon_manifest.h`, `addon_index.h`, `update_manifest.h`).
- Small portability helpers: `float_chars.h` (number parsing Apple Clang lacks), `wrap_math.h`, `ascii_case.h`.

### `platform/`: the only place that talks to the OS

SDL3 window and input (`window.h`), gamepads, the audio device, UDP and TCP sockets, HTTP downloads, the system
keyring (`credentials.h`), paths (`paths.h`: `data_dir()` and friends), child and helper processes, crash logs,
folder dialogs and message boxes.

### `net/`: the network protocol

- `session.*`, `session_codec.*`, `reliable_channel.*`: the SOE UDP session (CRC, compression, acks, resends,
  fragments) with its own service thread. docs/protocol/session.md.
- `login.*`, `des.*`: the login server: credentials, server list, "play". docs/protocol/login.md.
- `opcodes.h`, `opcodes_rof2.inc` (generated): opcode names and wire values.
- `*_packets.*` (`item_packets`, `spell_packets`, `group_packets` and about 40 more): encode and decode one area's
  packets. Pure functions over bytes, each in the fuzz list.
- `game_connection.*`: the world and zone connection. `handle_world` and `handle_zone` in `game_connection.cpp` are
  the dispatch for every incoming opcode; each area's handlers and the requests the client sends live in
  `game_connection_<area>.cpp` (`_items`, `_spells`, `_group`, `_merchant`, `_movement` ...).
- `server_files.*`: the files a server hands its players (docs/layout.md).

### `assets/`: reading the player's game files

`pfs.*` (the `.s3d` / `.eqg` archives), `wld*` (classic zones, models and particles), `eqg*` (newer zones and models),
`zone.*` (one loaded zone, whichever format), `character*` and `model*` (skeletal models), `texture*` (DDS, BMP,
texture packs), `sky*`, `sound_files.*`, `xmi.*` (music), `spell_effects.*`, `regions.*` (water, lava, zone lines),
`flora.*`. Every parser treats its input as hostile (docs/hardening.md). The formats are in docs/formats/.

### `game/`: rules and state

The biggest folder. Nothing in it draws or sends packets: it holds what the client knows and computes what the original
client would.

- `world_state.*`: the zone's spawns and the local player, read by the renderer and the UI.
- `mechanics/`: the original client's rules, function by function, checked against recorded vectors: `movement`,
  `physics`, `collision_movement`, `melee`, `attack_rules`, `cast_spell`, `hit_by_spell`, `stats`, `regen`, `skills`,
  `item_stats`, `ladder`, `vehicle` and more. docs/mechanics.md.
- One file per feature for the rest: `inventory`, `spells`, `spell_book`, `group`, `guild_*`, `merchant`, `bazaar`,
  `trade_rules`, `tradeskill`, `tasks`, `chat_*`, `slash_commands`, `keymap`, `hotbuttons`, `map_data`, `atlas`,
  `zone_routes`, `weather`, `sky_clock`, `collision`, `picking`, `targeting` ...
- `server_ext.*`, `server_ext_chain.*`: the hooks where a server's own client differs from stock RoF2. The default of
  each hook is the stock answer; plugins override them.
- Generated tables (`slash_commands.inc`, `socials.inc`, `race_models.inc`, `zone_names.inc`, `keymap_tables.inc`,
  `mechanics/skill_timers.inc` ...): change the generator in `tools/`, never the `.inc`.

### `render/`: Vulkan

- `vk_context.*`: device, swapchain, frame pacing, the HDR scene target and the post pass. `gpu.*`, `vk_util.*`, VMA.
- One renderer per kind of thing: `zone_renderer`, `actor_renderer` (skinned models), `object_renderer`,
  `spawn_renderer`, `flora_renderer`, `sky_renderer`, `atmosphere`, `particle_renderer`, `spell_fx_renderer`,
  `fire_renderer`, `weather_renderer`, `target_ring_renderer`.
- Lighting and shadows: `scene_lighting`, `light_grid`, `shadows`, `point_shadows`, `ambient_probe`, `cover_map`.
- Culling, descriptor limits, the pipeline cache, deferred destruction (`retire_queue`).

docs/graphics.md describes what the renderers do and why.

### `audio/`

The mixer, 3D listener and zone emitters, the sound library, music (`music_manager`, `xmi_player`: XMI to MIDI to a
SoundFont synth) and environment presets. docs/formats/audio.md.

### `ui/`: the UI host

- `imgui_layer.*`: Dear ImGui with its SDL3 and Vulkan backends, the fonts.
- `lua/lua_host.*`: runs a UI pack: loads each window's script, reloads it when it changes, draws its windows every
  frame, and catches its errors. `lua/lua_imgui.cpp` is the `ui` table (ImGui for Lua).
- `ui_packs.*` (which packs exist and which one runs), `ui_ini.*` (window places per character), textures and icons.
- `dev_screens.*`, `item_windows.*`, `*_windows.*`: developer windows drawn in C++ (docs/dev-ui.md).

### `web/`

The client's side of the in-game browser: the messages to the helper process (`browser_protocol`), the tabs it keeps
(`browser_host`) and the address rules (`page_rules`). The helper itself is `tools/hailstone-web/`. docs/browser.md.

### `nav/`

Navigation meshes (Recast/Detour), path following and `nav_control.h`, the only door the MQ layer has into it.
docs/navigation.md.

### `plugin/` and `mq/`: add-ons

- `plugin/`: compiled plugins (`native_plugins.*`), the hooks they get, what each may do (`policy.*`, capabilities),
  the player's consent for a server's plugins (`consent.*`), and their access to sockets, UI and browser.
- `mq/`: MacroQuest-style Lua scripts: the `mq` module (`lua_mq.*`), Top-Level Objects (`tlo.*`, `tlo_game.*`),
  `${...}` parsing, `/` commands, events, `/nav`. `lua_hailstone.*` is the Lua-plugin side of the server extensions.

Both are described in docs/plugins.md. Add-ons themselves live in their own repositories.

### `app/`: the client

- `main.cpp`: command-line parsing. It runs the client, or one of the developer viewers: a zone viewer
  (`--zone`, with flags for sky, fog, weather, lighting and screenshots) and a model viewer (`model_viewer.*`).
- `ClientApp` (`client_app.h`, its internals in `client_app_internal.h`): the playable client. The class is split by
  topic over `client_app_*.cpp`: `_login`, `_pregame` (server and character select), `_zone` (loading and zoning),
  `_player`, `_input`, `_pick`, `_items`, `_draw`, `_ui`, `_commands`, `_options`, `_gamepad`, `_web`, `_update`,
  `_addons`, `_bugreport`.
- `lua_eq*.cpp`: the `eq` table the UI packs call, one file per area (`lua_eq_inventory_manager`, `lua_eq_spellbook`,
  `lua_eq_chat`, `lua_eq_map` ...). It answers from the client's state with the arguments and returns the original
  UI's code expected. Both packs and players' scripts depend on it, so change it with care.
- Per-feature glue between game state, the renderers and the UI: `player_movement`, `navigator`, `world_objects`,
  `spell_bolts`, `sound_events`, `nameplate_layer`, `*_ui.cpp` (state handed to windows).
- `updater.*`, `update_install.*` (docs/updater.md), `addon_manager.*`, `addon_install.*` (docs/addons.md),
  `web_service.*` (who may use which browser tab), `automation.*` (scripted test runs).

### `texpack/`

Built texture packs: upscaling the player's own textures into a pack (`texpack` tool and the separate Texture
Studio). docs/graphics.md "Built packs (tools/texpack)".

## How it runs

### Start-up

`main.cpp` parses the arguments and builds a `ClientApp`. If no game folder is set yet, the player picks one
(`core/asset_folder.h` checks it). `ClientApp::run()` (`client_app.cpp`) starts the updater and the add-on index,
then loops once per frame until the window closes.

### One frame

`ClientApp::run()` does, in order:

1. Pace the frame (`frame_pace.h`) and advance the game clock (`core/step_clock.h`: rules run at 60 steps a second).
2. Update the login screen, updater and add-ons, then the connection (`update_connection`): the network's packets are
   handled here, and a zone load starts here.
3. Poll the browser helper, run automation, move world objects (doors, boats, missiles).
4. Read the gamepad, start the UI frame, step the vehicles and the other spawns' physics, then the player's movement.
5. Draw: the renderers record the scene into `VkContext`'s frame, the Lua pack draws its windows over it, and the
   frame is presented.

### From the server to the screen

1. A UDP datagram arrives. `net/session` (on its own thread) checks, decompresses, orders and reassembles it.
2. `GameConnection::update()`, called each frame, takes the complete application packets and dispatches them by
   opcode in `handle_world` / `handle_zone`.
3. The handler decodes the packet with its `net/*_packets.h` decoder and updates state: `game::WorldState` for spawns,
   the inventory, spells, group and so on.
4. The renderers read that state when they draw, and the UI pack reads it through `eq.*` (`app/lua_eq*.cpp`).

The other way, a click in a Lua window calls an `eq.*` function, which calls a `GameConnection` request (in its
`game_connection_<area>.cpp`), which encodes the packet and hands it to the session.

### Screens

The client moves through login, server select, character select, zoning (behind a loading screen) and in zone. The
login screen is `net/login` plus `client_app_login.cpp`; server and character select are `client_app_pregame.cpp`;
zoning and the zone load are `client_app_zone.cpp`. In the UI packs these screens are `login.lua`, `charselect.lua`
and `charcreate.lua`.

## Where to look for...

| Task | Start here |
|---|---|
| A packet the client mishandles | the opcode in `handle_zone` (`net/game_connection.cpp`), its decoder in `net/*_packets.h`, the layout in docs/protocol/rof2-packets.md |
| A rule (damage, speed, regen, a stat) | `game/mechanics/`, docs/mechanics.md, `tests/*_vectors_test.cpp` |
| A window's look or layout | `ui/modern/<window>.lua` (and `ui/gamepad/` if it has one) |
| What a window shows, or what its button does | the `eq.*` function it calls, in `app/lua_eq_<area>.cpp` |
| A slash command | `game/slash_commands.inc` (generated by `tools/gen_slash_commands.py`), handled in `app/client_app_commands.cpp` |
| Movement, collision, jumping | `game/mechanics/movement.*`, `physics.*`, `collision_movement.*`, `app/player_movement.*` |
| How something is drawn | its renderer in `render/`, its shader in `shaders/`, docs/graphics.md |
| A zone or model that will not load | `assets/` (`pfs`, `wld`, `eqg`, `zone`), the dump tools in `tools/`, docs/formats/ |
| Sound or music | `audio/`, `app/sound_events.*` |
| Settings and where files go | `core/config.h`, `platform/paths.h`, docs/layout.md |
| Something a plugin or script needs | `sdk/hailstone_plugin.h`, `plugin/`, `mq/`, docs/plugins.md |
| Behaviour that differs on a custom server | `game/server_ext.h` (and the plugin that overrides it, in its own repository) |
| Login, updater, add-on manager | `net/login.*`, `app/updater.*`, `app/addon_manager.*` |

## Tests

`tests/` has roughly one `<area>_test.cpp` per feature, all built into the single `hailstone_tests` binary.

- `*_vectors_test.cpp` check `game/mechanics/` against vectors recorded from the original client. Without
  `HAILSTONE_VECTORS_DIR` set they pass without checking anything.
- Tests that read real game files (zones, models, sounds) need `EQ_GAME_DIR`, and likewise pass silently without it.
  The few that read a current live install's newer files (hero's forge layers) need `EQ_LIVE_DIR`.
- `mock_*_test.cpp` drive the real login and game connection against the in-process mock server
  (`tests/mock_server/README.md`).
- `fuzz_*`: every decoder in `src/net/` must be in `fuzz_decoders.inc`; `fuzz_assets_test.cpp` covers the asset
  parsers.
- Helpers: `scratch_dir.h` for temporary files, `manual_clock.h` for time, `mq_test_world.h` for script tests.

How to build, run and add tests is in [CONTRIBUTING.md](../CONTRIBUTING.md) and docs/ci.md.
