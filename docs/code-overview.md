# Code overview

A map of the repository for newcomers: what each folder holds, how the pieces connect at run time, and where to look
for a feature. Goals and rules are in [docs/plan.md](plan.md); files on disk in [docs/layout.md](layout.md).

Files named here are examples. Where this page disagrees with the code, fix the page.

## The top level

| Folder | What is in it |
|---|---|
| `src/` | The client: about 600 C++20 sources and their headers in layered folders (below). |
| `ui/` | The Lua UI packs: `ui/modern/` (mouse and keyboard) and `ui/gamepad/`. One `.lua` per window, helpers in each pack's `lib/`. Copied beside the executable at build time (`cmake/SyncUiPacks.cmake`). |
| `shaders/` | GLSL, compiled to SPIR-V at build time (`cmake/Shaders.cmake`), named after their renderer (`actor.*`, `sky.*`, `flora.*`, `post.frag`). |
| `sdk/` | The compiled-plugin SDK: `hailstone_plugin.h` (the C ABI) and `hailstone_byte_io.h`. The `nmsc_*.h` files are the old names, kept for existing plugins. |
| `plugins/example/` | The SDK's example plugin, also a test fixture. Server plugins are cloned into `plugins/<name>/` from their own repositories and ignored by git. |
| `server/` | The drop-in Lua quest script that gives an EQEmu server Hailstone support, its installer and `README.md`. |
| `tests/` | The doctest suite (`hailstone_tests`), the mock EQEmu server (`tests/mock_server/`), fuzz targets and test plugins. |
| `tools/` | Command-line tools (`pfsdump`, `wlddump`, `eqgdump`, `texdump`, `chrdump`, `audiodump`, `netprobe`, `playprobe`, `texpack`, `hailstone_check`, `hailstone_sign`), the browser helper (`tools/hailstone-web/`), generators (`gen_*.py`), release scripts (`make_update.py`, `make_addon.py`, `docker/`) and `sync_wiki.py`. |
| `cmake/` | Dependency fetching (`Dependencies.cmake`), shaders, build info, toolchains, Windows resources. |
| `third_party/` | Sources vendored into the tree: Lua 5.5, AMD FSR 1 and the AMD FSR 3.1 upscaler's shaders. Everything else is fetched by CMake. |
| `flora/` | Plain-text grass and plant tables per zone or zone group (docs/graphics.md). |
| `docs/` | Design, protocol, format and mechanics documents; the source of this wiki. |
| `icons/`, `pictures/` | Hailstone's own icon and pictures. No game art is ever in the repository. |

The build makes one static library, `hailstone_engine`, from nearly all of `src/`; the client (`hailstone`), tools and
tests link it. `CMakeLists.txt` lists every source by hand: an unlisted `.cpp` compiles nowhere.

## The layers in `src/`

Each folder may include only the folders below it:

```
app                                      the client itself: main loop, screens, hs.* API for the UI packs
 ├─ mq, plugin, nav                      scripts, compiled plugins, navigation
 ├─ game   ui   render   audio           rules and state, UI host, Vulkan renderers, sound
 ├─ assets   net   web                   game files, network protocol, browser helper protocol
 └─ core   platform                      utilities; every OS call
```

`game/` never includes Vulkan headers and only `platform/` calls the OS, so tests run rules, packet code and asset
loaders without a window, GPU or network.

### `core/`: utilities with no OS calls

- `byte_io.h`: `ByteReader` / `ByteWriter`, for every packet and file. No struct casts.
- `config.h`, `ini_file.h`: `hailstone.ini` and the game's own `.ini` files.
- `game_dir.h`, `asset_folder.h`: the game folder with a server's files layered over it, and the check of a picked
  folder.
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
- `*_packets.*` (`item_packets`, `spell_packets`, `group_packets` and about 40 more): one area's packets, pure
  functions over bytes, each in the fuzz list.
- `game_connection.*`: the world and zone connection. `handle_world` and `handle_zone` in `game_connection.cpp`
  dispatch every incoming opcode; each area's handlers and requests are in `game_connection_<area>.cpp` (`_items`,
  `_spells`, `_group`, `_merchant`, `_movement` ...).
- `server_files.*`: the files a server hands its players (docs/layout.md).

### `assets/`: reading the player's game files

- `pfs.*`: the `.s3d` / `.eqg` archives.
- `wld*`: classic zones, models and particles. `eqg*`: newer zones and models.
- `zone.*`: one loaded zone, whichever format.
- `character*` and `model*`: skeletal models. `texture*`: DDS, BMP, texture packs.
- `sky*`, `sound_files.*`, `xmi.*` (music), `spell_effects.*`, `regions.*` (water, lava, zone lines), `flora.*`.

Every parser treats its input as hostile (docs/hardening.md). The formats are in docs/formats/.

### `game/`: rules and state

The biggest folder. Nothing in it draws or sends packets: it holds state and computes what the original client would.

- `world_state.*`: the zone's spawns and the local player, read by the renderer and the UI.
- `mechanics/`: the original client's rules, function by function, checked against recorded vectors: `movement`,
  `physics`, `collision_movement`, `melee`, `attack_rules`, `cast_spell`, `hit_by_spell`, `stats`, `regen`, `skills`,
  `item_stats`, `ladder`, `vehicle` and more. docs/mechanics.md.
- One file per feature for the rest: `inventory`, `spells`, `spell_book`, `group`, `guild_*`, `merchant`, `bazaar`,
  `trade_rules`, `tradeskill`, `tasks`, `chat_*`, `slash_commands`, `keymap`, `hotbuttons`, `map_data`, `atlas`,
  `zone_routes`, `weather`, `sky_clock`, `collision`, `picking`, `targeting` ...
- `server_ext.*`, `server_ext_chain.*`: hooks where a server's client differs from stock RoF2; each defaults to the
  stock answer and plugins override them.
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
- `lua/lua_host.*`: runs a UI pack (loads, hot-reloads and draws each window's script, catching its errors).
  `lua/lua_imgui.cpp` is the `ui` table (ImGui for Lua).
- `ui_packs.*` (which packs exist and which one runs), `ui_ini.*` (window places per character), textures and icons.
- `dev_screens.*`, `item_windows.*`, `*_windows.*`: developer windows drawn in C++ (docs/dev-ui.md).

### `web/`

The client's side of the in-game browser: the messages to the helper process (`browser_protocol`), the tabs it keeps
(`browser_host`) and the address rules (`page_rules`). The helper itself is `tools/hailstone-web/`. docs/browser.md.

### `nav/`

Navigation meshes (Recast/Detour), path following and `nav_control.h`, the only door the MQ layer has into it.
docs/navigation.md.

### `plugin/` and `mq/`: add-ons

- `plugin/`: compiled plugins (`native_plugins.*`), their hooks, permissions (`policy.*`, capabilities), the player's
  consent for a server's plugins (`consent.*`), and their socket, UI and browser access.
- `mq/`: MacroQuest-style Lua scripts: the `mq` module (`lua_mq.*`), Top-Level Objects (`tlo.*`, `tlo_game.*`),
  `${...}` parsing, `/` commands, events, `/nav`. `lua_hailstone.*` is the Lua-plugin side of the server extensions.

Both: docs/plugins.md. Add-ons live in their own repositories.

### `app/`: the client

- `main.cpp`: command line; runs the client or a developer viewer: zone (`--zone`, with flags for sky, fog, weather,
  lighting and screenshots) or model (`model_viewer.*`).
- `ClientApp` (`client_app.h`, internals in `client_app_internal.h`): the playable client, split by topic over
  `client_app_*.cpp`:
  `_login`, `_pregame` (server and character select), `_zone` (loading and zoning),
  `_player`, `_input`, `_pick`, `_iso` (isometric camera and click-to-move), `_items`, `_draw`, `_ui`, `_commands`,
  `_info`, `_options`, `_gamepad`, `_web`, `_update`, `_addons`, `_addon_upscaler`, `_bugreport`, `_testdrive`.
- `lua_ui*.cpp`: the `hs` table the UI packs call, one file per area (`lua_ui_inventory_manager`, `lua_ui_spellbook`,
  `lua_ui_chat`, `lua_ui_map` ...).
  It answers from client state with the arguments and returns the original UI's code expected. Both packs and players'
  scripts depend on it: change it with care.
- Per-feature glue between game state, the renderers and the UI: `player_movement`, `navigator`, `world_objects`,
  `spell_bolts`, `sound_events`, `nameplate_layer`, `*_ui.cpp` (state handed to windows).
- `updater.*`, `update_install.*` (docs/updater.md), `addon_manager.*`, `addon_install.*` (docs/addons.md),
  `web_service.*` (who may use which browser tab), `automation.*` (scripted test runs).

### `texpack/`

Built texture packs: upscaling the player's own textures into a pack (`texpack` tool and the separate Texture
Studio); docs/graphics.md "Built packs (tools/texpack)". It uses `assets/`, `core/` and `platform/`; only the tools
link it.

## How it runs

### Start-up

`main.cpp` builds a `ClientApp`; with no game folder set, the player picks one (`core/asset_folder.h` checks it).
`ClientApp::run()` (`client_app.cpp`) starts the updater and add-on index, then loops once per frame.

### One frame

`ClientApp::run()` does, in order:

1. Pace the frame (`frame_pace.h`) and advance the game clock (`core/step_clock.h`: rules run at 60 steps a second).
2. Update the login screen, updater and add-ons, then the connection (`update_connection`), where packets are handled
   and zone loads start.
3. Poll the browser helper, run automation, move world objects (doors, boats, missiles).
4. Read the gamepad, start the UI frame, step the vehicles and the other spawns' physics, then the player's movement.
5. Draw: the renderers record the scene into `VkContext`'s frame, the Lua pack draws over it, and it is presented.

### From the server to the screen

1. A UDP datagram arrives. `net/session` (on its own thread) checks, decompresses, orders and reassembles it.
2. `GameConnection::update()`, each frame, dispatches complete application packets by opcode in `handle_world` /
   `handle_zone`.
3. The handler decodes with its `net/*_packets.h` decoder and updates state (`game::WorldState` for spawns, the
   inventory, spells, group ...).
4. The renderers read that state when they draw, and the UI pack reads it through `hs.*` (`app/lua_ui*.cpp`).

The other way, a click in a Lua window calls an `hs.*` function, which calls a `GameConnection` request (in
`game_connection_<area>.cpp`) that encodes the packet for the session.

### Screens

Login, server select, character select, zoning (behind a loading screen), in zone:

- The login screen is `net/login` plus `client_app_login.cpp`.
- Server and character select are `client_app_pregame.cpp`.
- Zoning and the zone load are `client_app_zone.cpp`.
- In the packs: `login.lua`, `serverselect.lua`, `charselect.lua`, `charcreate.lua`; their `hs.*` side is
  `app/lua_ui_pregame.cpp`.

## Where to look for...

| Task | Start here |
|---|---|
| A packet the client mishandles | the opcode in `handle_zone` (`net/game_connection.cpp`), its decoder in `net/*_packets.h`, the layout in docs/protocol/rof2-packets.md |
| A rule (damage, speed, regen, a stat) | `game/mechanics/`, docs/mechanics.md, `tests/*_vectors_test.cpp` |
| A window's look or layout | `ui/modern/<window>.lua` (and `ui/gamepad/` if it has one) |
| What a window shows, or what its button does | the `hs.*` function it calls, in `app/lua_ui_<area>.cpp` |
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

`tests/` has roughly one `<area>_test.cpp` per feature, all in the `hailstone_tests` binary.

- `*_vectors_test.cpp` check `game/mechanics/` against vectors recorded from the original client; without
  `HAILSTONE_VECTORS_DIR` they pass without checking anything.
- Tests reading real game files need `EQ_GAME_DIR` and likewise pass silently without it; the few reading a live
  install's newer files (hero's forge layers) need `EQ_LIVE_DIR`.
- `mock_*_test.cpp` drive the real login and game connection against the in-process mock server
  (`tests/mock_server/README.md`).
- `fuzz_*`: every decoder in `src/net/` must be in `fuzz_decoders.inc`; `fuzz_assets_test.cpp` covers the asset
  parsers.
- Helpers: `scratch_dir.h` for temporary files, `manual_clock.h` for time, `mq_test_world.h` for script tests.

How to build, run and add tests is in [CONTRIBUTING.md](../CONTRIBUTING.md) and docs/ci.md.
