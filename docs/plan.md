# Hailstone: plan

A client for EQEmu servers written from scratch in C++ with Vulkan. It connects with the RoF2 protocol, reads the assets of an installed RoF2 game, and builds and runs on any OS and CPU architecture.

## 1. Goals and rules

- **Compatible with EQEmu RoF2 servers.** The client speaks EQEmu's RoF2 protocol unchanged, so no server patches are needed. It identifies as RoF2 (an `OP_SendLoginInfo` of 464 bytes).
- **Uses the player's own game files.** The client reads an existing RoF2 installation, or a current live one (`docs/layout.md` "RoF2 or live"), through `--game-dir` or the picker, and ships no game assets. The files are EverQuest's, Daybreak Game Company's work: we highly encourage players to subscribe to [EverQuest](https://www.everquest.com). With a live folder the server's `assets/` supplies what a RoF2 server must match (spells, strings, the zones live replaced).
- **Portable.** It builds with GCC, Clang and MSVC on Linux, Windows and macOS (Vulkan through MoltenVK), on x86-64 and arm64, and stays 32-bit clean.
  - Files and packets are read and written with explicit little-endian helpers (`ByteReader`/`ByteWriter`), never by casting a struct onto a buffer.
  - No `#pragma pack` wire structs (the plugin host's bulk reads, which hand an in-process plugin packed records, are the exception), no Win32 calls outside `platform/`, and no assumptions about `long`, `wchar_t` or path separators.
- **Clean code.** C++20, RAII, no global singletons except the explicit app context, and unit tests from the start.
- **Faithful mechanics, modern graphics.** Game behaviour (stats, combat, movement, spells, packets and every other rule) follows the RoF2 rules; deliberate changes (fixed bugs) are marked as changes and are switchable where it matters. Graphics are not meant to match the classic look: the world is drawn with a modern renderer (HDR, antialiasing, per-pixel lighting, shadows, atmosphere), see `docs/graphics.md`.
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
                   → zoning → in zone), loading screen, the eq.* API for the UI packs
  tools/           command-line utilities (pfs, wld, eqg, texture, chr and audio dumps, texpack,
                   net and play probes, hailstone_check, hailstone_sign), generators (gen_*.py),
                   make_update.py, make_addon.py
  tests/           unit tests; asset tests run when EQ_GAME_DIR is set
  shaders/         GLSL compiled to SPIR-V at build time
```

The client's own folder (the binary, shaders, UI packs, settings, saved servers with their files, scripts, plugins, logs) and the rules for the asset folder are in `docs/layout.md`: the RoF2 install is only read, and a server's files are layered over it.

The layers depend only downward: `app` → `game`/`ui`/`render`/`audio` → `assets`/`net`/`web` → `core`/`platform`. `game` does not include Vulkan headers. `nav` (the navigation meshes, `docs/navigation.md`) sits above `game` and `assets` and below `app`; the MQ layer reaches it only through `nav/nav_control.h`. `render` and `ui` read game state through narrow interfaces, so the zone viewer and tests can run without a network connection.

The main loop runs simulation and movement at a fixed step, polls the network on its own thread with a lock-free queue into the game thread, and renders with interpolation. Asset loading runs on worker threads behind a loading screen.

### Libraries

Each library is pinned by version and fetched by CMake (`FetchContent`), or found on the system when present.

| Need | Choice | Why |
|---|---|---|
| Window, input, audio device, Vulkan surface | SDL3 | portable, mature, has `SDL_Vulkan_*` |
| Vulkan loader and headers | Vulkan-Headers + volk | no link-time loader dependency |
| GPU memory | Vulkan Memory Allocator | standard |
| Shaders | GLSL → SPIR-V with `glslc` (shaderc) at build time | simple |
| Maths | glm | header-only |
| zlib | zlib-ng (compat mode) | PFS blocks and the session layer |
| UI | Dear ImGui, Lua 5.4 (`third_party/lua`) | windows drawn by Lua scripts, hot reload (`docs/dev-ui.md`) |
| Fonts | ImGui's font loader | the UI expects Arial-class fonts; uses a metric-compatible open font (Liberation Sans) or the system font by name |
| MP3 / WAV | dr_mp3, dr_wav | single header |
| MIDI synthesis | TinySoundFont + own XMI parser | portable; the player supplies a General MIDI SoundFont, with a small default |
| Navigation meshes | Recast/Detour | the standard; zlib licence |
| Tests | doctest | fast, header-only |
| DES (login) | own small implementation | only ECB/CBC with a zero key is needed |

Vulkan baseline: 1.3 with dynamic rendering and synchronization2. MoltenVK and every current desktop driver support it. Block-compressed textures (BC1–3) are uploaded directly when the device supports them, and decoded on the CPU otherwise (some arm64 GPUs). The renderers' bindless texture arrays are sized from the device's descriptor limits (`render/descriptor_limits.h`) and their sets are update-after-bind where the device supports it: Intel's Windows driver allows only 64 samplers per shader stage in plain sets, against about a million in update-after-bind ones.

## 3. Specs and references

Format and protocol documents live in `docs/formats/` and `docs/protocol/`; mechanics are in `docs/mechanics.md`. Packet layouts follow EQEmu's `rof2_structs.h` and `rof2.cpp`. The open-source OpenEQ project's documents are used as a format spec (it declares `MIT OR Apache-2.0` in its `Cargo.toml`, though it ships no LICENSE file); code written after studying its implementation must credit it in `THIRD_PARTY.md`.

## 4. Milestones

Each milestone ends with something you can run.

1. **M0, skeleton** (done). CMake project, dependency fetching, SDL3 window, Vulkan device and swapchain, shader build, test runner. Builds and tests run locally on Linux and on Windows (MSVC, also under Wine); GitHub Actions runs checks only, see `docs/ci.md`.
2. **M1, classic zone viewer** (done). PFS, WLD (meshes, materials, placed objects), DDS/BMP textures; `hailstone --zone qeynos2 --game-dir …` flies a free camera through a zone with fog, clip-plane, back-face culling and region vertex lighting rules, lights.wld point lights and the sky's cloud patches (`docs/formats/sky.md`). With `--frames` and `--screenshot` the viewer saves its view as a PNG.
3. **M2, network to character select** (done). SOE session, login, server list, world auth, character list, as a command-line `netprobe` first, then a minimal UI.
4. **M3, in zone** (done). Zone-in handshake, player profile, own position, spawns, movement with floor and wall collision, zone lines, chat, camp, spawn smoothing, third-person camera, sky and time of day.
5. **M4, characters** (done except the points in `docs/unknowns.md`). WLD skeletal models, the race-to-model table, animation (upper and lower bone groups: actions over the legs), equipment textures and tints, faces, helms, robes, held items, nameplates; Luclin models with layered textures and hair, beard, helm, robe and plate parts; EQG models with weighted skinning and EQGA animations; Drakkin features, hero's forge, the models' and items' own particle emitters. Open: the tint of one-texture Luclin face materials.
6. **M5, UI engine** (done except the remaining windows). Direction: the windows are Dear ImGui drawn by Lua scripts (hot reload, `ui.*` ImGui bindings, the `eq.*` API over the client's state), see `docs/dev-ui.md`; UI packs `modern` and `gamepad` (the `classic` pack, drawn from the SIDL layouts, is gone: `docs/dev-ui.md`). In place: layouts generated from the SIDL XML, fonts, art as textures, window persistence in the character's UI.ini. Windows: player, target, group, buffs, songs, spell gems, casting bar, hot bars, inventory with bags and tradeskill containers, loot, merchant, bank, trade, give, chat, pet, actions, compass, spell book, item and spell display (with augments), quantity, cursor attachment, main menu and options, friends, guild, raid, skills, training, AA, tasks, map (with toolbar and labels), inspect, confirmation dialog, book, note, bandolier, trader, bazaar search, barter, mail, extended target, respawn, context tips, colour picker, context menus, name change, help, load skin, find location, pre-game server select and character creation with the scene behind them, and NPC journal. Missing: the stock zone guide windows (no data on EQEmu; the modern map's World view has an atlas and zone finder of Hailstone's own, docs/navigation.md "Zone atlas"), and the remaining windows.
7. **M6, EQG zones** (mostly done). All 249 EQG zones load and draw through the classic renderer and collision with region boxes (`docs/formats/eqg.md`): shader classes with blend, alpha-test and fog states, water, waterfall, lava and terrain effects, heightmap tiles (coverage, colour and detail maps), DAT region boxes, indexed water. The modern lighting adds normal maps, specular highlights, sky reflections and its own water reflection (`docs/graphics.md`). Missing: the original's RegionLava2 and RegionLavaH formulas (the modern lighting draws its own lava) and the classic look's sky-cube reflection.
8. **M7, gameplay** (done except the points in `docs/unknowns.md`). Targeting, consider, melee, XP, death and respawn; inventory, equip and bag moves, destroy, looting, item links, stack splits and merges; bank (16 or 24 slots, shared slots, bank bags, coin); spells (scribing, memorizing, casting, fizzles, interrupts, gem recast, buffs and songs, spell icons); groups; pets; merchants; hail and say links; NPC hand-ins and player trades; doors, lifts and ground objects; tradeskill containers; item use (clickies, food, drink, books, scrolls); gem count and buff stacking rules (`docs/mechanics.md`); spell particles at the actors' spell points; missiles (arrows, thrown weapons, the server's bolts); environmental hits (falls, drowning, lava: lines, sounds, animations).
9. **M8, audio** (done except the open points in `docs/unknowns.md`; `docs/formats/audio.md`). SDL3 output with a small mixer (no device = silent; `--no-audio`); WAV and MP3 decoding; zone emitters from `.emt` or the old `_sounds.eff`/`_sndbnk.eff`; zone music from MP3s and `.xmi` (own XMI parser and sequencer, TinySoundFont with a SoundFont); 3D event sounds (melee, footsteps, jumps, doors, level up, spell cast and impact); reverb and listener environments; global music (login and character select theme, combat, death, bank, merchant, trainer); rain and wind emitters. Settings `audio.*` in hailstone.ini (master, music, effects, mute, environment, SoundFont).
10. **M9, everything else** (in progress). Social systems: the slash command table and line interpreter, chat with templates and echoes, tells, `/who`, emotes and socials, the UCS chat server with numbered channels, friends and ignore lists, guilds (invite, remove, leave, leader, MOTD, ranks and their permissions and names, public and personal notes, alt flags, the tribute page (members' tribute states and donations, opt-outs), the join, leave and rank lines, the name sprite's guild line, the management window and the guild bank, tried live on EQEmu; `/guildwar` and `/guildpeace` are sent as the original sends them, which EQEmu only logs), raids (invite, leadership, loot type, looters, group moves), duels, inspect, `/surname`, corpse summoning and dragging. Progress systems: AA, training, tradeskill combines, tasks, skill-ups, `/alternateadv`, `/tasktimers`. Market systems: the bazaar (trader window, shop, search), barter, UCS mail, the marketplace window (the original's store protocol, for a server that runs one; empty on stock EQEmu). Mercenaries (`game/mercenary.{h,cpp}`): the packets, manager states, commands, windows and nameplate colour; their right-click, dismissal question and merchant, manage and info windows are done. Missing: the AA window's own list logic, placed particles (the guild window's banner page is built as far as EQEmu allows: it has no banners).
11. **Improvements** (started). **Lua socials** (`docs/plugins.md` "Lua socials", `game/lua_social.h`): a social may hold Lua instead of its five lines, with modifiers, a `state` kept between presses and an optional face (label, colour, cooldown sweep, corner text, dimmed) drawn on the hot bars of both UIs. The server decides in its `hailstone.cfg`, sent with its client files along with its own `scripts/` and `plugins/` (`docs/plugins.md`); no cfg, no rules.

## 5. Testing

- Unit tests for every parser and the session layer, with recorded packet captures as fixtures. Captures are stored outside git if they contain account data.
- Asset tests run against a real game directory when `EQ_GAME_DIR` is set. One test walks every zone and reports parse failures.
- Golden-image tests for a few zones at fixed camera positions (not built yet; `hailstone --zone … --frames N --screenshot FILE` writes the image).
- Packet-level comparisons against an EQEmu test server for the same actions.

## 6. Slash commands

The client keeps a slash command table (`src/game/slash_commands.inc`, 383 entries) with its matching rules; the dispatch is `GameConnection::run_slash_command` (`net/game_connection_social.cpp`). A table command without a handler says "<name> is not supported by this client yet."

- **Handled:** information and social (`/who /whotarget /consider /consent /deny /played /time /location /random /motd /ignore /friends /anonymous /roleplay /afk /lfgroup /assist /follow /target /rtarget`); chat (`/say /tell /msg /send /ttell /reply /gsay /shout /auction /ooc /guildsay /fsay /rsay /emote /me`); the chat server (`/chat /join /leave /leaveall /list /announce /1../10`); guilds (`/guildinvite /guildremove /guildleader /guildstatus /guildmotd /getguildmotd /guildcreate /guildpromote /guilddemote /guilddelete /guildwar /guildpeace`); raids (`/raidinvite /raidaccept /raiddecline /raiddisband /raidwindow /makeraidleader /setloottype /addraidlooter /removeraidlooter`); groups and pets (`/invite /disband /makeleader /pet /grouproles /groupleader`); leadership abilities (`/marknpc /gmarknpc /rmarknpc /clearmarks /findpc /inspectbuffs`); expeditions (`/dz*`) and shared tasks (`/task*`); play (`/sit /stand /camp /attack /cast /stopcast /hail /loot /autoinventory /split /yell`); abilities and spells (`/discipline /doability /stopdisc /autofire /melody /dismount /memspellset /spellscribe /targetgroupbuff /tgb`; `/spellscribe` sends only when the world's OP_LogServer allows it, which EQEmu does not); progress (`/alternateadv /tasktimers`); windows (`/aggrometer /aggrolock /claim /bugreport /copylayout`); players (`/duel /decline /inspect /toggleinspect /surname /corpse /corpsedrag /corpsedrop /shield /keys /rewind /autosplit /bandolier /book /language /popmail /stopsong /resetaas /petition /survey`); the bazaar (`/trader /bazaar /barter /buyer /offlinemode`); mercenaries (`/mercwindows /stance /mercassist /mercswitch /merclog`).
- **Social emotes (91):** `/smile /wave /bow /cheer /dance ...`; their templates, pronoun slots and animations are generated into `src/game/socials.inc` by `tools/gen_socials.py`.
- **GM only (26):** `/zone /goto /summon /kill /kick /hideme /emotezone ...`; never matched for players.
- **Game commands:** `/shieldgroup /blockspell /sheathe /open /reveal /hidecorpses /decaycorpse /setstartcity /bodytint /stopsocial /facepick` (`/facepick` opens the player customization window in both packs).
- **Client options and windows:** `/quit /exit /safelock /charinfo /log /secondslog /netstats /trackplayers /trackpets /trackmercs /tracksort /trackfilter /stoptracking /invwinlabels /resetwindows /reverb /filter /note /map /loadskin /chatfontsize /clearchat /clearallchat /usercolor /hotbutton /shownames /showspelleffects /dynamiclights /showgrass /shadows /particledensity /viewport /indicator /fullscreen /fontface /combatmusic /reversesound /mixahead /mousespeed` (settings in hailstone.ini as the Options window shows them, game ones in the character's options.ini).
- **Client, help and report:** `/help /autojoin /uptime /servertransfer /report /vreport /feedback /guidehelp /veteranReward /acompare /aclearcompare /lfguild /paperdoll /channel /url /www /outputfile`. Commands for opcodes EQEmu does not have are sent and get no answer.
- **Client extensions:** `/destroy`.
- **Not yet:** `/gems` (the Gems mini-game window, CGemsGameWnd, is not drawn) and the rest of the table (voice chat, web and beta commands).
- **Not implemented, by choice:** voice macros and audio triggers (`/vplay /vtell /vgroup /vraid` are left out of the table, the Voice Macro and Audio Triggers windows, their menu rows and key commands are not offered), and a few commercial-service features the server side has no part of: the marketplace's funding and credit-card pages (the store itself is in), polls, reward selection, achievements, alerts and the event calendar, trading cards, social sharing, fellowships, and the customer service (petition) and welcome screen web pages (their keys are not offered). Real estate is built as the original has it (`/realestate`, `/leaverealestate`, its windows and keys; docs/protocol/rof2-packets.md "Real estate"): a stock EQEmu server never sends it, a server with housing (Project Triune) does.

The main menu keeps only the rows whose command the client runs with the UI pack in use (`app::menu_command_offered`, `app/lua_eq_menu.cpp`): a row for a command left out above, an unknown command name or a window the pack does not draw is dropped, then any submenu left empty and the separators that part nothing; the log lists what was left out.
