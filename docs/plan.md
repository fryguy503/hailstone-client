# Hailstone: plan

A client for EQEmu servers written from scratch in C++ with Vulkan. It connects with the RoF2 protocol, reads the assets of an installed RoF2 game, and builds and runs on any OS and CPU architecture.

## 1. Goals and rules

- **Compatible with EQEmu RoF2 servers.** The client speaks EQEmu's RoF2 protocol unchanged, so no server patches are needed. It identifies as RoF2 (an `OP_SendLoginInfo` of 464 bytes).
- **Uses the player's own game files.** The client reads an existing RoF2 installation, or a current live one (`docs/layout.md` "RoF2 or live"), through `--game-dir` or the picker. It ships no game assets.
  - The files are EverQuest's, Daybreak Game Company's work: we highly encourage players to subscribe to [EverQuest](https://www.everquest.com).
  - With a live folder, the server's `assets/` supplies what a RoF2 server must match (spells, strings, the zones live replaced).
- **Portable.** It builds with GCC, Clang and MSVC on Linux, Windows and macOS (Vulkan through MoltenVK), on x86-64 and arm64, and stays 32-bit clean.
  - Files and packets are read and written with explicit little-endian helpers (`ByteReader`/`ByteWriter`), never by casting a struct onto a buffer.
  - No `#pragma pack` wire structs (except the plugin area, `src/plugin/`, for records handed to an in-process plugin), no Win32 calls outside `platform/`, and no assumptions about `long`, `wchar_t` or path separators.
- **Clean code.** C++20, RAII, no global singletons except the explicit app context, and unit tests from the start.
- **RoF2 as the baseline, modern graphics.**
  - Game behaviour (stats, combat, movement, spells, packets and every other rule) starts from the RoF2 rules. Against a server that speaks only RoF2, the client behaves as RoF2 does.
  - The client may go past RoF2's hard limits (a roster's size, a slot count, a list's length) where a server offers more. Such an extension is marked as one and leaves the RoF2 path unchanged, so a stock EQEmu RoF2 server keeps working.
  - Deliberate changes to a RoF2 rule (fixed bugs) are marked as changes and are switchable where it matters.
  - Graphics are not meant to match the classic look: a modern renderer (HDR, antialiasing, per-pixel lighting, shadows), see `docs/graphics.md`.
- **No guesswork.** Behaviour that cannot be confirmed is listed in `docs/unknowns.md`.

## 2. Architecture

```
hailstone/
  CMakeLists.txt, cmake/          build, dependency fetching, shader compilation
  src/core/        logging, byte reader/writer, ini, virtual file system, timers, config
  src/platform/    SDL3: window, input, Vulkan surface, audio device, UDP sockets, paths
  src/net/         SOE session (CRC, compression, sequencing, fragments, combined),
                   login (DES), world, zone, typed packet encode/decode per opcode
  src/web/         the browser helper's messages, the tabs the client keeps (docs/browser.md)
  src/assets/      PFS (s3d/eqg), WLD, EQG (MOD/TER/ZON/LIT/ANI), EQTZP heightmaps,
                   DDS/BMP, EFF/EMT sounds, XMI music, spell effects, ini/txt tables
  src/render/      Vulkan device, swapchain, frame sync, resource upload, materials,
                   zone renderer, actors/skinning, particles, sky, UI batches
  src/ui/          Dear ImGui layer, Lua host and its ImGui bindings, UI packs, fonts,
                   window persistence (UI.ini)
  src/game/        world state, spawns, own character, inventory, spells, buffs,
                   combat, movement and collision, chat and slash commands
  src/audio/       mixer, 3D emitters, MP3/WAV, XMI → MIDI → SoundFont synth
  src/nav/         navigation meshes (Recast/Detour), paths and the walk (docs/navigation.md)
  src/plugin/      compiled plugins, their hooks, capabilities and consent (docs/plugins.md)
  src/mq/          MacroQuest-style scripts: the mq module, TLOs, commands (docs/plugins.md)
  src/app/         main loop, state machine (login → server select → char select
                   → zoning → in zone), loading screen, the hs.* API for the UI packs
  src/texpack/     built texture packs (upscaling the player's textures), for tools/texpack only
  tools/           command-line utilities (pfs, wld, eqg, texture, chr and audio dumps, texpack,
                   net and play probes, hailstone_check, hailstone_sign), generators (gen_*.py),
                   make_update.py, make_addon.py
  tests/           unit tests; asset tests run when EQ_GAME_DIR is set
  shaders/         GLSL compiled to SPIR-V at build time
```

The client's own folder and the asset folder's rules are in `docs/layout.md`. The RoF2 install is only read; a server's files are layered over it.

The layers depend only downward: `app` → `game`/`ui`/`render`/`audio` → `assets`/`net`/`web` → `core`/`platform`.

- `game` does not include Vulkan headers.
- `nav` (the navigation meshes, `docs/navigation.md`) sits above `game` and `assets` and below `app`. The MQ layer reaches it only through `nav/nav_control.h`.
- `render` and `ui` read game state through narrow interfaces, so the zone viewer and tests can run without a network connection.

The main loop runs simulation and movement at a fixed step and renders with interpolation. The network session has its own service thread, which hands complete packets to the game thread under the session's lock (`net/session.h`). Asset loading runs on worker threads behind a loading screen.

### Libraries

Each library is pinned by version and fetched by CMake (`FetchContent`), or found on the system when present.

| Need | Choice | Why |
|---|---|---|
| Window, input, audio device, Vulkan surface | SDL3 | portable, mature, has `SDL_Vulkan_*` |
| Vulkan loader and headers | Vulkan-Headers + volk | no link-time loader dependency |
| GPU memory | Vulkan Memory Allocator | standard |
| Shaders | GLSL → SPIR-V with `glslc` (shaderc) or `glslangValidator` at build time | simple |
| Maths | glm | header-only |
| zlib | zlib-ng (compat mode) | PFS blocks and the session layer |
| UI | Dear ImGui, Lua 5.5 (`third_party/lua`) | windows drawn by Lua scripts, hot reload (`docs/dev-ui.md`) |
| Fonts | ImGui's font loader | the UI expects Arial-class fonts; uses a metric-compatible open font (Liberation Sans) or the system font by name |
| MP3 / WAV | dr_mp3, dr_wav | single header |
| MIDI synthesis | TinySoundFont + own XMI parser | portable; the player supplies a General MIDI SoundFont, with a small default |
| Navigation meshes | Recast/Detour | the standard; zlib licence |
| Tests | doctest | fast, header-only |
| DES (login) | own small implementation | only ECB/CBC with a zero key is needed |
| Signatures | Monocypher (Ed25519) | the update manifest's and add-ons' signatures (`docs/updater.md`) |
| Images and models | bcdec, stb_image, cgltf | BC7 decoding without device support, JPEG pictures, glTF replacement models |
| In-game browser | CEF (`HAILSTONE_BROWSER`) | a separate helper process (`docs/browser.md`) |
| Vulkan on macOS | MoltenVK | shipped beside the client, as macOS has no Vulkan of its own |

Vulkan baseline: 1.3 with dynamic rendering and synchronization2. MoltenVK and every current desktop driver support it.

- Block-compressed textures (BC1–3) are uploaded directly when the device supports them, and decoded on the CPU otherwise (some arm64 GPUs).
- The renderers' bindless texture arrays are sized from the device's descriptor limits (`render/descriptor_limits.h`). Their sets are update-after-bind where the device supports it: Intel's Windows driver allows only 64 samplers per shader stage in plain sets, against about a million in update-after-bind ones.

## 3. Specs and references

Format and protocol documents live in `docs/formats/` and `docs/protocol/`; mechanics are in `docs/mechanics.md`. Packet layouts follow EQEmu's `rof2_structs.h` and `rof2.cpp`.

The open-source OpenEQ project's documents are used as a format spec. It declares `MIT OR Apache-2.0` in its `Cargo.toml`, though it ships no LICENSE file. Code written after studying its implementation must credit it in `THIRD_PARTY.md`.

## 4. Milestones

Each milestone ends with something you can run. What each release added is in `CHANGELOG.md`.

1. **M0, skeleton**: done. Builds and tests run locally on Linux and Windows (MSVC, also under Wine); GitHub Actions runs checks only (`docs/ci.md`).
2. **M1, classic zone viewer**: done. `hailstone --zone qeynos2 --game-dir …` flies a free camera through a zone (sky: `docs/formats/sky.md`); `--frames` and `--screenshot` save the view as a PNG.
3. **M2, network to character select**: done.
4. **M3, in zone**: done.
5. **M4, characters**: done except the points in `docs/unknowns.md` (open: the tint of one-texture Luclin face materials).
6. **M5, UI engine**: done except the remaining windows. Windows are Dear ImGui drawn by Lua scripts, in the `modern` and `gamepad` packs (`docs/dev-ui.md`). Missing: the stock zone guide windows (no data on EQEmu; the modern map has its own atlas, docs/navigation.md "Zone atlas").
7. **M6, EQG zones**: mostly done; all 249 EQG zones load (`docs/formats/eqg.md`, modern lighting in `docs/graphics.md`). Missing: the original's RegionLava2 and RegionLavaH formulas and the classic look's sky-cube reflection.
8. **M7, gameplay**: done except the points in `docs/unknowns.md` (rules: `docs/mechanics.md`; the bank has 16 or 24 slots).
9. **M8, audio**: done except the open points in `docs/unknowns.md` (`docs/formats/audio.md`).
10. **M9, everything else**: in progress. Social, guild, progress, market and mercenary systems are in. Missing: the AA window's own list logic and placed particles.
11. **Improvements**: started. **Lua socials** (`docs/plugins.md` "Lua socials", `game/lua_social.h`): a social may hold Lua instead of its five lines, with a face drawn on the hot bars. The server allows it in its `hailstone.cfg`, sent with its client files (`docs/plugins.md`); no cfg, no rules.

### Road to 1.0

Set 2026-11-18. 1.0 promises that the plugin ABI, the `hs.*` and `mq` script API, the add-on manifest and the folder layout (`docs/layout.md`) stop breaking. So every breaking change lands first, then the platforms, then live play.

Each step is one beta release; its items are in `docs/todo.md` "Road to 1.0".

1. **0.8, freeze the surfaces.** Everything that breaks scripts, plugins or saved files lands here, and only here:
   - Lua 5.5 (already in, with 0.7.2);
   - a review of `hs.*` across both packs (rename or remove before the freeze);
   - the last plugin ABI change;
   - the `nmsc` compatibility layer and the older builds' fallbacks removed;
   - the add-on safety items and the layer order fixed.

   At the end the stable API is written down (`docs/ui-api.md`, `docs/plugins.md`) with the rules for growing it after 1.0.
2. **0.9, platforms and releases.** Windows end to end (the updater, add-ons, the Lua fixes under MSVC, UTF-8 paths) and a scripted release. macOS stays a test build until someone can test and maintain its releases; it does not block 1.0.
3. **0.10, live play on stock EQEmu.** A written checklist played on the stock PEQ test server in both packs and with a controller, and the bugs it finds fixed.
4. **0.11, real estate.** Player housing and guild halls are a RoF2 feature, so part of 1.0: close the placement and item-move gaps, then play it live on a server with housing.
5. **0.12, memory and soak.** The memory items (`docs/todo.md` "Memory"), an hour in a busy hub, and the golden-image tests (section 5) so rendering regressions after the freeze are caught.
6. **1.0 RC, then 1.0.** Bug fixes only. The rows left in `docs/unknowns.md` become a known-issues page.

Not part of 1.0:

- the "trace the original" rows that change nothing a player sees (they stay in `docs/unknowns.md`);
- the features listed under "After 1.0" in `docs/todo.md`.

Systems stock EQEmu never sends (Spirit Shrouds and missions, LDoN, fellowships, guild banners, achievements, reward selection, evolving items) ship as built, marked as working where a server supports them; their live checks stay open.

## 5. Testing

- Unit tests for every parser and the session layer, with recorded packet captures as fixtures. Captures are stored outside git if they contain account data.
- Asset tests run against a real game directory when `EQ_GAME_DIR` is set. One test walks every zone and reports parse failures.
- Golden-image tests for a few zones at fixed camera positions (not built yet; `hailstone --zone … --frames N --screenshot FILE` writes the image).
- Packet-level comparisons against an EQEmu test server for the same actions.

## 6. Slash commands

Every command a player can type is listed in [docs/commands.md](commands.md). For a developer:

- The table is `src/game/slash_commands.inc` (384 entries) with its matching rules; the dispatch is `GameConnection::run_slash_command` (`net/game_connection_social.cpp`). A table command without a handler says "<name> is not supported by this client yet."
- Social emotes (91) are generated into `src/game/socials.inc` by `tools/gen_socials.py`. GM-only commands (26) are never matched for players.
- Commands for opcodes EQEmu does not have are sent and get no answer. `/spellscribe` sends only when the world's OP_LogServer allows it, which EQEmu does not.
- Client extensions: `/destroy`, and for plugins `/destroycursor <id>` and `/stowcursor <id>` (docs/plugins.md).
- Not implemented, by choice: the voice macros (`/vplay /vtell /vgroup /vraid`, their windows and keys) and the commercial-service features the server side has no part of (marketplace funding pages, polls, alerts, the event calendar, trading cards, social sharing, the petition and welcome web pages).
- Real estate is built as the original has it (docs/protocol/rof2-packets.md "Real estate"). Stock EQEmu never sends it; a server with housing (Project Triune) does.

The main menu keeps only the rows whose command the client runs with the UI pack in use (`app::menu_command_offered`, `app/lua_ui_menu.cpp`). Dropped, in order:

1. a row for a command left out above, an unknown command name or a window the pack does not draw;
2. then any submenu left empty and the separators that part nothing.

The log lists what was left out.
