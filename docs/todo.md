# To do

What is left before the client is feature complete, in the order to work on it. The details of each gap are in `docs/unknowns.md`; this list only orders them. Tick an item when it is done and tested, and remove its row from `docs/unknowns.md` at the same time. Add new gaps to both.

Written 2026-10-28 from `docs/unknowns.md` and `docs/plan.md`.

## Road to 1.0

The steps are in `docs/plan.md` "Road to 1.0". This section says which items belong to which step; most of them are written out in full in the sections below, so tick them there and here together.

### 0.8, freeze the surfaces

- [ ] Lua 5.4.7 to 5.5.1 (the `lua-5.5` branch): the packs and the add-on repositories fixed, the break in the changelog.
- [ ] Review `hs.*` across both packs and the `mq` module: rename or remove what is not wanted for good, then write the stable set down (`docs/ui-api.md`, `docs/plugins.md`) with the rules for adding to it after 1.0.
- [ ] Plugin ABI: make the last breaking change, if any, and declare `HAILSTONE_PLUGIN_ABI` stable; write down how it grows after 1.0 (new functions at the end of the tables, no changed layouts).
- [ ] Remove the `nmsc` compatibility layer: `sdk/nmsc_*.h` and the `nmsc_*` exports, the `NMSC_*` environment variables (`core/env.h`) and CMake settings, the saved window names renamed at start (`core/old_ui_settings`), and the rule about them in AGENTS.md.
- [ ] Remove the older builds' fallbacks: the chat filters and colours read from the asset folder's `eqclient.ini` under `chat.ini`, and the unsorted character folders adopted per world (docs/layout.md).
- [ ] Add-on safety: wrap the plugin functions and tables passed to pack functions; the arming delay on the client's own consent prompt; the pack's consent window guarded from before its first draw; the core install skipping add-ons' folders; `server_file_destination` refusing `.so.N` as `make_addon.py` does (all under "Add-ons" and "Code health").
- [ ] The layer order: `net` and `assets` stop including `game/` headers ("Code health").
- [ ] The local-time calls outside `src/platform/` through `platform::local_time` ("UI").

### 0.9, platforms and releases

- [ ] Windows: the updater run (WinHTTP, the renames of the running `hailstone.exe`), add-ons run, the Lua review fixes built with MSVC under Wine, the `path::string()` sweep or the manifest recorded as required ("Updates", "Add-ons", "Code health").
- [ ] A scripted release (packages, manifest, signature, upload), local or in CI ("Updates").
- [ ] Downloads that resume; the rollback's `kept-<time>/` folders cleaned up.
- [ ] Add-ons against the real GitHub; XeSS and FSR 4 in the add-on index.
- macOS is on hold until someone can test and maintain its releases; it stays a test build and does not block 1.0.

### 0.10, live play on stock EQEmu

- [ ] Write the checklist (a doc of its own, played on the stock PEQ test server, both packs, a physical controller) from the live items of "Combat and spells", "Movement", "Items and commerce", "Social and friends", "Navigation", "Item comparison", "Isometric camera" and "Check live".
- [ ] Play it and fix what it finds.
- [ ] Shadow step (SPA 379): the zone collision `land_spell` needs ("Combat and spells").
- [ ] The collision radius, too wide for doorways ("Movement").
- [ ] Leaving an adventure clears the window ("Adventures").
- [ ] UCS reconnection and a wrong chat key ("Items and commerce").

### 0.11, real estate

- [ ] Placement: the ghost's wall and surface fit, free mode's mouse moves, the flatness test.
- [ ] The item-move pre-checks and button greying, alternate-currency deposits, the NPC-item binding, the item menu's unbuilt entries, the neighborhood polls and vote window.
- [ ] Played live on a server with housing, and the real estate rows of `docs/unknowns.md` settled.

### 0.12, memory and soak

- [ ] The character archives and the base cost ("Memory").
- [ ] The texture slots of freed appearances freed; the session-long caches evicting ("Check live", "Code health").
- [ ] An hour in a busy hub with zoning, and several clients on the shared asset cache.
- [ ] Golden-image tests for a few zones (docs/plan.md section 5).

### 1.0 RC

- [ ] Bug fixes only; a known-issues page from what is left in `docs/unknowns.md`; the wiki and changelog.

### After 1.0

Not blocking 1.0: `/gems`; `/travel`, waypoints and the mesh editor; levitation paths; tactical queries for bots; frame generation's pacing and 3x/4x; FSR's water mask; zone flora rules; an allocator of the client's own; shared GPU textures on Linux and past 16 clients; the MacroQuest alert lists and `/target next`; the "trace the original" rows that change nothing a player sees. The server-dependent systems stock EQEmu never sends (shrouds and missions, LDoN, fellowships, guild banners, achievements, reward selection, evolving items) keep their live checks open past 1.0.

## Spirit Shrouds and Monster Missions

- [x] Server-supplied form catalogs, stats and detail requests, character and observer form updates, shared-task template rows, shroud progress and Shroud Bank item recovery in both UI packs.
- [ ] Live acceptance on a RoF2 server with configured shroud forms and missions, including zoning or relogging in a form, mission completion, and withdrawal of stored possessions.

## Server add-on policy

- [x] Optional required/allow/block lists by GitHub repository (owner/repo; bare names refused) for every kind the add-on manager installs, intact package identities, required autostart that the player cannot stop, consent gates, and active upscaler checks; disabled unless `[addons] enforce = on`.
- [x] Server add-ons warning with Retry for a required plugin or script that failed in the world (Modern, Gamepad, the client's own window; closable, no emergency stop). From fryguy503's PR #93.
- [ ] Letting a required add-on run above `player_caps` (PR #93 asked for it) needs a load-time check that its files are the ones a signed manifest named; the client keeps neither the signed manifest nor per-file signed hashes, so naming a repository grants no capabilities for now.
- [x] Minimum versions on required add-ons (`required = owner/repo:1.0.0-preview.4`, SemVer order, highest minimum wins, malformed = invalid policy). From fryguy503's PR #93.
- [x] Decided at the zone-in by its own rules (the cached copy only informs the login screen); before the world shows a problem refuses entry, once in it a failure warns once in chat and never disconnects; readiness and approvals cached by the rules, approvals and running set; package hashes remembered by size and time.
- [x] Keep native plugin permission requests above the loading screen, with tests for drawing, mouse input and both answers in Modern and Gamepad.
- [ ] Live acceptance of a first visit and a return visit, a policy fixed by the server, missing/refused/failed requirements at the zone-in and mid-session, both UI packs and controller input, and an upscaler allowed/blocked/updated followed by restart.

## Launcher login

- [x] Explicit one-use token authentication, native-style `patchme /ticket:`, environment/argument transports, endpoint selection, optional world/character entry and memory-only returned-token session sign-in, with packet/launcher/mock-server and UI regression coverage.
- [ ] Live launcher/server acceptance: fresh, expired, consumed and refused tickets; world restrictions; character entry; return to login and obtain a fresh ticket. Patcher executable selection stays separate.

## 1. Gameplay a player will hit

### Legacy environmental particles

- [x] Draw explicit-position legacy particle switches from server-supplied door records and the player's `spells.eff`/global sprites, including waterfall cascades; parser, converter and lifecycle coverage.
- [x] Live waterfall rendering acceptance: seen on the stock PEQ test server in the Stonebrunt Mountains (its effect-142 switches, doors 41 and 44, at night in the rain), with the cascade absent from a client built without this change at the same view. A level-69 character, so the level gates of the second and third slots were not exercised live.
- [ ] Connect the native day/night switch mode-2 rain-screen override and one-second shutdown tail (unknowns.md).

### Server map ownership

- [x] Optional exclusive spawn-marker ownership, stock NPC overlay/Find suppression, shared UI metadata and native Find lifecycle guards, with synthetic ownership, identity and presentation regressions.
- [ ] Live acceptance across tracking-class changes, GM state, zoning, policy retirement and real server Find replies in both UI packs. No offline test establishes live gameplay acceptance.

### Modular interfaces

- [x] Generic owner-scoped method hooks with explicit exposure policies, protected callbacks and regression tests.

- [x] Generic owner-scoped world nameplate canvas, bounded drawing and measurements, whole-caption claims, guarded physical clicks, automatic Lua addon activation and color widgets.
- [x] Native SDK registration for the shared world-caption canvas, including scoped measurements, drawing and observed concealment fields.
- [ ] Live acceptance of custom nameplate callbacks, scene depth, dense layouts, click/drag/focus handling, reloads and zoning.
- [x] Nameplate canvas exposes observed concealment for already visible actors and identifies regular or undead invisibility when spell data supports it.

- [x] Shared versioned UI data/action API, module discovery, preserved guarded closures and a central method/capability catalog; Modern and Gamepad call the same contract.
- [x] Declared UI base chains, window/helper overrides, per-interface settings, sandboxed inherited resources and a starter interface.
- [x] Managed interface addons register in the native selector without script startup; Options exposes selected and
  loaded interface metadata, fallback state and loading diagnostics through the shared interface API.
- [x] Packaged interface raster artwork through a discoverable, owner-scoped API, approved base inheritance,
  confined paths and a bounded cache that refreshes replaced files.
- [ ] Live acceptance of both migrated interfaces and a derived interface, including zoning, switching, input focus and controller workflows. Local tests and offline rendering do not establish live acceptance.

### Adventures (LDoN)
- [x] Alt+V and recruiter request/offer windows in both UI packs, entry/completion timers and progress, profile point balances, seven-column theme stats and native leaderboard categories, risk choices and tied ranks; native protocol investigation, unit and mock-server coverage.
- [ ] Complete an adventure live: request, accept/decline, zone in, objective updates, finish, leave and cross-zone refresh; confirm stats, leaderboard standings and physical controller operation. Remaining protocol ambiguity is in unknowns.md.
- [ ] Leaving an adventure: stock EQEmu sends nothing after a granted leave, so the window stays on the adventure until zoning. Settle what RoF2 shows (unknowns.md) and clear the window to match.

### Fellowships

- [x] Native membership, invitations, leader actions, MOTD, personal/leader XP toggles, vitality, camp requests, `/fsay`, window key and MQ fellowship search; both UI packs support up to 64 server-supplied members (packet, mock-server and UI tests).
- [ ] Live acceptance on a fellowship server, including cross-zone invitations/roster updates, XP sharing and camp creation/destruction. Stock EQEmu sends no fellowship packets; EQEmu PR #5134 (unmerged) serializes twelve slots, so expanded rosters need matching server support.
- [x] Native vitality gauges and inventory bars in both packs, campsite categories loaded from the player's resources, DBString type-0 kit descriptions, gamepad leader markers and a server-expiry countdown. Category masks and description lookup were checked against the original handlers.

### Social and friends

- [x] `/who friends` sends OP_FriendsWho with RoF2's zone/all scope and saved names and displays the server's normal who results; native serialization checked, packet and mock-server coverage includes empty lists, online friends, anonymous/roleplaying friends and omitted offline friends.
- [ ] Compare `/who friends` and `/who all friends` live with RoF2: online/offline transitions, friends in other zones and anonymous/roleplaying friends. Seen live (stock PEQ test server, 2026-11-18): both list an online friend in another zone and nothing once it camps; no logon/logoff notice appeared. Open: anonymous and roleplaying friends, a comparison with RoF2.

### Combat and spells (do first)
- [ ] Shadow step (SPA 379) does nothing live: `land_spell` has no zone collision (point_in_world, collide_sphere, collide_line, floor_height and is_valid_teleport are stubbed in src/game/hit_landing.cpp) and the result has no position for the connection to apply. Needs collision in HitLandingContext, a port of IsValidTeleport and a teleport result applied with a position update.
- [x] Spell Hit Modes: Normal, Abbreviated and Number Only for direct damage, DoTs, heals and HoTs, preserving full spell and melee log lines and incoming-chat hooks. Structured, generic and plain-text combat messages and UI tests; live gameplay remains unverified (docs/mechanics.md "Spell hit modes"). Seen live (stock PEQ test server, 2026-11-18) with direct damage and heals in all three modes; DoTs and HoTs not tried.
- [x] Personal and guild tribute: received bonus items feed local stats/effects and retire on received deactivation, selection changes, opt-out and context changes; guild benefit controls enforce received permissions, and guild platinum donations wait for matching replies. Packet, mechanics and mock/UI regressions; full live tribute workflow acceptance remains open.
- [x] Raid leadership controls and leadership hotkey creation in both UI packs, correct trainable ability identities, delegate/mark synchronization and lifecycle cleanup, active leadership bonuses in local stats, and raid invitations from expedition/shared-task rosters (unit, mock-server and UI coverage).
- [x] Leadership window colours in both packs: native green for a supplied leader rank, gold for personal purchases, and white/grey training availability; native analysis, server-source comparison and rendered-text regressions distinguish these from population and permission checks.
- [ ] Raid leadership live acceptance: use member context menus with a mouse and controller, assign/remove multiple assists and markers, mark/clear NPCs, zone/rejoin, use leadership hotkeys, and accept expedition/shared-task roster invitations with real players and a controller.
- [x] Extended chat channels 57..107 and user colours 355..408 for servers that send a later client's chat colours (a Hailstone extension; stock routing and colours unchanged; docs/mechanics.md "Extended chat routing").
- [x] Missiles: a despawned source or target as DeleteMyMissiles handles it; flying through walls with a target is the original's behaviour (60b1934). Not tested live (no archer).
- [x] Valid spell missiles: spell lifetime, launch and ending stages, stationary area contact, source physics clock and spell 859 local presentation. Packet-to-emitter and missile fixtures cover these; live gameplay remains unverified. Invalid or absent spell ids still end immediately.
- [x] Environmental hits 0xf3 to 0xfd: animations, chat lines, sounds, and our own fall, drowning and lava damage reported like ApplyDamage (eb42f20; falls tested live).
- [x] ReportSuccessfulHit's shared tail: the spell book closes and a sitting or mounted player stands when damage lands; a spell heal from OP_Damage (ff39361; the stand checked live).
- [x] Blood spurts on supported WLD actors: landed local sharp-weapon hits, exact authored blood attachment and the player's single-frame blood sprite. Synthetic asset and packet-to-emitter coverage; missing attachments or unsupported sprite data safely omit the effect.
- [x] Spell particles: the impact stage's caster is the packet's source (EQPlayer::TriggerSpellEffect @0x593eb0; nothing when the source is unknown, as DoSpellEffect); the actor's opacity and the point model's PESFactor on actor emitters.
- [ ] Spell particles: whether an actor's world matrix rows carry its scale (normalised here), and what makes a floor-flag actor emitter again as the actor moves (kept on the actor here). unknowns.md.
- [x] Linked spell timers and discipline timers from the player profile.
- [x] Combat Abilities: active discipline duration gauge 26 and seconds display in both UIs, with icon cancellation regression coverage (unit, mock-server and UI tests; live acceptance open).
- [x] Combat Abilities: red reuse overlays and seconds in both UIs; shared timer groups remain marked even when the wait exceeds an ability's own recast, including discipline hot buttons (packet, UI and unit coverage; live acceptance open).
- [x] Item recast on cast items (already in 789134d).
- [x] The distance modifier of effect values, and the extra long-buff slots from level 72 and 75.
- [x] Buff tick timers held by SPA 0x1b4 and suspendable spells.
- [x] Buff countdowns held in zones that suspend buffs (OP_NewZone +876, 0xddfc5c); the order of the zone flag and the SPA 0x1b4 exception read again at @0x4dcf20; unit and connection tests, and live on a stock PEQ server (the player's own buffs held in poknowledge, the server's count the same on leaving).
- [ ] `get_pet_buff_data_bulk`: hold a suspendable pet buff in a zone that suspends buffs, as the pet windows do (unknowns.md; the method's author).
- [x] DoPassageOfTime's 6 s tick runs whole in the client (ProcessAffects' buff HP, mana and endurance, local expiry at 0, the mesmerize break; HP, endurance and mana regen) and its 1 s endurance upkeep; unit and connection tests, not tried live.
- [x] Passage-of-time owner status/bleeding, live running endurance penalty, flag-1 cast interruption, drunkenness decay/sway/camera and Eye summon control are implemented with regression coverage. Live acceptance remains open.
- [x] Combat line filters 13 to 16.
- [x] Temporary-pet combat lines preserve the spawn flag and use native focused-pet or temporary-pet-plus-owner classification (packet-driven regression tests).
- [ ] Check owned-pet line filters and colour live with a permanent pet and multiple temporary pets on a server that supplies the temporary flag and owner; stock EQEmu's RoF2 swarm records omit that information (unknowns.md).
- [x] Hand-to-hand delay and fist damage from the profile (EQEmu sends 0); the player's own BodyWet in the attack checks.
- [x] `AltAttack` and the attack lock match the original; the rank swap's limit and GM check ported (never swaps on EQEmu's gold membership).
- [x] Ranged attacks do not turn the player in the original either.
- [x] Held buffs seen live (a SPA 436 buff freezes Courage's countdown). Melee checked by hand.
- [ ] Distance-scaled spell effects live (needs a spell with a distance modifier cast at range). Buff caster ids are in unknowns.md.
- [x] Commands: `/discipline`, `/doability`, `/stopdisc`, `/memspellset`, `/spellscribe`, `/targetgroupbuff` / `/tgb`, `/autofire`, `/melody`, `/dismount`, `/aggrometer`, `/buyer`, `/claim` (f3c5c5d).
- [ ] `/gems`: the Gems mini-game (CGemsGameWnd, 0x6705f0 to 0x6734e0), a port of its own.
- [x] (358bf04, checked live: /discipline now runs the NMS reuse timers; the empty list after login matches the original) A second `/discipline` right after `/stopdisc` is not refused by the client (NMS disc timer map or a missing server timer packet); the reuse list is empty after zone-in until the server sends timers.

### Movement
- [x] Server flymodes: the local player takes the spawn record's flymode and `SpawnAppearance` 19 (all six modes) and keeps them until the effect cache is rebuilt, when `UpdateLevitationState`'s rules apply again (docs/mechanics.md "fly mode"; unit tests of the decision and through the connection's packets). Seen live (stock PEQ test server, 2026-11-18): all six modes, and levitation gained and lost under modes 0, 1 and 3.
- [x] The game rules on a 60 Hz game clock (`game.fixed_step`, docs/mechanics.md "Frame rate"): movement, other spawns, missiles, weather and fog steps, the frame-gated timers; the camera drawn between steps (unit tests at 15 to 1000 simulated frames a second).
- [ ] Collision radius: the static reading gives the model's bounding radius (6.2 for a human), too wide for doorways; settle it with a run-time read of the actor's vf44 in the real client (rof2re's radius_log, needs a new rof2re.dll in the game folder and a user-run session). The stand-in stays until then.
- [ ] Other spawns: bodies never block in the original (no per-bone meshes); only bone attachments such as held weapons could, and that test is not ported.
- [x] NPC collision (Hailstone's own, switchable by the server's hailstone.cfg `[movement] npc_collision`): live NPCs stop the player; players, pets, mercenaries, corpses and mounts do not (game/npc_collision.h; unit tests).
- [x] The jump's crouch window from the duck clip's length (453db92); the ceiling query and auto-duck match the original apart from the radius.
- [x] New-style models' duck clip for the crouch window (CRCH_BA_1, 0x406740's row 0x7a - 0x4f); the jump animation from the playing animation (+0x10c0, vf24) rather than 500 ms after a jump (anim-fixes; unit tests).
- [x] Placed objects that play their own animation (Ssraeshza Temple's turning rings, swaying tree branches in 75 zones): drawn moving (one instanced model per definition, a few matrices a frame) and solid where the animation has them, so the rings let a player into the water shaft and out at the top (animated-objects; unit tests and the game files' route through the shaft; the drawing seen in the viewer). Tried live with a non-GM player (stock PEQ test server, 2026-11-18): into the water shaft and out at the top (invulnerable to the temple's guards).
- [ ] Animated zone objects, open: the phase against the original's clock, D3DX's key compression, the other spawns' probes, sight, picking and /nav not meeting the moving parts (docs/unknowns.md).
- [x] Lifts carry riders (already worked; checked live on the Paineel lift).
- [x] Teleport doors (a Bazaar IT78 door, the PoK books' type, checked live; NMS sends PoK visitors to the Bazaar).
- [x] Touch-activated switches: PlayerClient::TouchingSwitch ported, traps checked live (f500930).
- [x] Boats, as our own design (docs/mechanics.md "Boats"): riding with hull floors and walls, smoothed boat motion, other riders, a zone-in onto a deck, climbing aboard, sailing a small boat at the helm (dee94fe..873439e; all checked live in Butcherblock and OOT).
- [x] Boats with a live asset folder: not the client's job. The classic boat models (LAUNCH etc.) are only in RoF2's per-zone `_chr` archives, which live dropped; a server that wants boats on live assets supplies those models in its `client_files/` (user's decision, 2026-10-29).
- [x] Boats: overlapping decks (the nearest, the one stood on in a tie), the boat's own animation in its collision (its triangles taken again every 200 ms), the helm against docks and other boats (hull probes; the step is refused and the boat stops, the turn alone may still swing it), rails on a moving deck (the LAUNCH skiff's flat one included); unit tests only (tests/boats_test.cpp).
- [ ] Boats, check live: walking from a dock onto a boat; the helm against a real dock and another boat; the rails on the LAUNCH skiff; the landing on the Timorous shuttle (NMS sent the character to the Bazaar: where a boat script puts the player is the server's, so that is for the NMS server's quest script, not the client). Seen live on the NMS dev server (2026-10-31, qeytoqrg's row boat): a zone-in onto the deck held the player until the hull loaded and stood it on the boat, A on the boat asked for and got the helm (OP_ControlBoat), the boat sailed and turned; walking off a still boat let go of it. Still open: the helm's stop against a dock or another boat (not seen to happen in the screenshots, night and no log line), boarding a moving boat from a dock, the LAUNCH skiff's rails (Butcherblock's Captains_Skiff, grids 232 / 233, not tried).
- [x] Zone points that request a teleport by index: sent already; the player is now held while waiting (untested, EQEmu never sets them).
- [x] Reachable zone lines in toxxulia, mistythicket and oldbloodfield: a client fallback fires buried EQG zone-line boxes from the ground over them (81d4a59; 39 boxes across the EQG zones; all 7 lines in the three zones checked live). oldbloodfield's ATP_3 has no zone point 30 in the server's table.
- [x] Position history (PositionTracker, OP_FloatListThing), the retried and the 30-minute saves, closing on loading screens (60069a0). Missing samples are listed in unknowns.md.
- [x] The live folder's revamped bazaar, lavastorm and highpasshold load when the server's safe point (OP_NewZone) stands on their floor (game/revamped_zone.h): PEQ's Bazaar and Lava Storm version 1 (expansion 9 on) and its only High Pass Hold row. Version 0's safe points have no floor there, so those stay refused.
- [ ] Arena and Nektulos with a live folder: its arena.s3d and nektulos.s3d win over the revamped .eqg, but PEQ's Arena (its only row) and Nektulos version 1 (expansion 10 on) run the revamp: their spawns stand on the live .eqg, not the .s3d (unknowns.md, Movement and collision).
- [x] IVM's animation (race 127/600, the enchanter pet): it has no named clips, and GetAlternateAnimTag @0x406a60 sends its code, like the rest of its wood-elf prefix group, to the wood elf's (ELM), so it plays ELM's clips and holds its weapons as an elf does, not spread in its T-shaped rest pose (game::animation_source).
- [ ] IVM's click volume: whether the original's click meets its masked body mesh, a sphere or only its held items (unknowns.md, Characters and models).

### Items and commerce
- [x] Evolving-item Final Result inspection: native action-4 request, ordinary read-only item reply, received instance progress updates and a focusable gamepad action; packet, bridge, mock-server and Lua-pack coverage.
- [ ] Evolving-item live acceptance: inspect Final Result from owned and linked items, receive progress through 100%, confirm the server's replacement level/template and use the gamepad action with a physical controller.
- [x] Stock achievements: bounded compressed catalog, primary state and counters, categories/search/status filters, linked details, nearby-player comparisons, explicit reward views and refresh, both UI packs; codec/model/bridge/mock-server and actual Lua-pack fixtures. Live use reported generally working (2026-11-12); targeted acceptance remains below.
- [x] Achievement browser usability: collapsible categories with deduplicated completion totals, compact status/points rows, inline selected objectives and reward actions, readable dates, exact full-width count text, and responsive layouts in both packs. Linked details stay independent; no request runs from filtering or drawing.
- [x] Catalog reloads retain the decoding schema of in-flight comparison/link requests: valid retired replies drain without updating newer views, even after definition/component changes; malformed replies and timeouts retain their guards.
- [x] Reward-window presentation: numbered choices with measured long-name captions, full item details and explicit tile/name inspection, prominent claim/confirmation controls, and separate maintenance actions in both packs.
- [x] Stock XML category badges with base/hover/selected variants, preserved native aspect and same-row right-aligned counts; missing art keeps the category name. Reward selection uses explicit full-card choices, grant summaries and a fixed claim/confirmation footer in both packs.
- [ ] Achievements: live catalog refresh, earned/guild messages, local/nonlocal links, comparison and reward delivery; native completion timing, icon resource selection and legacy achievement gauges (docs/unknowns.md).
- [x] Stock pending reward selection and task reward previews (`0x6411` / `0x6471`), both UI packs, explicit claims and item inspection; codec, model, bridge, mock-server and Lua UI tests. Live use reported generally working (2026-11-12); the refined reward window still needs targeted live claims/inspection acceptance below.
- [x] Task offers: native-confirmed Preview Reward in both UI packs and the starting-zone description hint; unit, bridge, mock-server and actual Lua-pack coverage. The old comment's "reward line" was eqstr 6029's starting-zone hint, not reward amounts.
- [ ] Reward selection: live offered-task previews, claims/delivery and item inspection on a RoF2 server (docs/unknowns.md).
- [x] NPC service distance closure and dependent quantity/confirmation cleanup in both UI packs; every container closes with the bank (unit, UI and mock-server coverage; docs/mechanics.md "Leaving an NPC interaction").
- [x] World tradeskill station distance closure in recipe, Experiment and hidden modes, out-of-reach open replies and busy replies, and world-only quantity/mold/batch cleanup; carried tradeskill bags stay when only the station closes.
- [x] The service windows' ranges and timers checked against the original (docs/mechanics.md "Leaving an NPC interaction"); still open: the mercenary liaison's check cadence (docs/unknowns.md) and walking away from each service live.
- [x] Trades with players: CTradeWnd::OnProcessFrame @0x780b50 cancels them beyond the partner's avatar height + 20 on the same 500 ms check as hand-ins; outstanding items and coin return through the server (unit and mock-server coverage).
- [ ] Trades with players: the merchant page-handler-2 exception (docs/mechanics.md "Leaving an NPC interaction"). Live separation with outstanding items and coin checked (stock PEQ test server, 2026-11-18): walking or teleporting out of reach cancels it, items and coin intact.
- [x] Partial-stack looting asks the quantity (EQEmu ignores it); Link All; the loot music (entry 5 has no asset).
- [x] Loot All preserves an active queue on repeated activation (an individual request in flight does not block it), and retains the first NO DROP question when a later confirmation aborts the queue (planner and mock-server regressions; docs/mechanics.md "Loot All").
- [x] Loot All: a second click while its "are you sure" question is up or queued asks nothing more (mock-server regression).
- [ ] Loot All: live activation with delayed replies (only LAN latency tried). Recovery from several NO DROP items and rapid clicks checked live (stock PEQ test server, 2026-11-18); a double click before the question showed asked twice, fixed since.
- [ ] Loot All: trace native auto-close timing when an accepted NO DROP question overlaps the last automatic request (docs/unknowns.md).
- [x] Ctrl + right-click bag swapping; Ctrl+click bag contents' slot order and refusal text (game/bag_move).
- [x] The tradeskill window: favourites, request waits, combine checks, trivial colours (experiment and auto-combine were already in).
- [x] Hailstone's tradeskill window: each component's carried count (green / red), how many can be made, Combine / Combine N / Combine All with each result put away and a stop when the inventory is full (docs/mechanics.md; unit and mock tests, not seen live).
- [x] Trades: the item request counter in IsOkToTransact; a busy answer cancels the trade.
- [x] Bazaar purchase: EQEmu's TradeRequestFailed reads as a failure (5cf3f63; mock test only).
- [x] Seen live (5ba3a3b and before): a stock-loot corpse with a partial stack, Link All (a crash without a loot session fixed) and Loot All, a two-client trade with coin and the busy refusals, Ctrl bag moves and the bag swap, the tradeskill search and favourites (the modern window's selected recipe fixed), the bandolier's Make Hot Button and Auto Swap, the merchant's Qty and Lvl.
- [x] The item display's icon links its item into the active chat window's input (CItemDisplayWnd::WndNotification, IconButton), both packs, X in the gamepad one (unit tests; seen live (stock PEQ test server, 2026-11-18)).
- [x] Evolving-item experience transfer: both packs select owned source/destination items, request server compatibility and projected results, show max-transfer level and confirm the approved pair (codec, mock-server, Lua bridge and pack tests; live acceptance pending).
- [x] Link All in the gamepad loot window (unit tests); the inventory doll drop in the modern pack (checked live). The bandolier's Activate swapping weapons seen live (stock PEQ test server, 2026-11-18).
- [x] Bazaar: Find Trader, Hide Traders, the result row's Buy button, the stall check, the price load of a selected slot (f7401f3; Hide Traders by unit tests only).
- [x] Barter: the feature check, offline mode, Hide Buyers (f7401f3; Hide Buyers and the feature check by unit tests only).
- [x] Mail: the ignore list (kept and edited only, as in the original); the composition window's From (351d492).
- [x] UCS authentication gates chat/mail commands and login-time events; bounded queues, denial (an explicit refusal or the stock UCS closing the link before its reply), timeout and the backed-off reconnect lifecycle covered by independent mock peers.
- [x] UCS: a chat link that goes silent (the server dropped the session, or the way back is gone) ends after 60 s with nothing received and is retried; a socket closed under the link ends it at once (mock peer; seen live before the fix: a deaf chat socket was never noticed).
- [ ] UCS: check reconnection after a lost chat link, and a wrong chat key, against a live stock EQEmu server (the 60 s silence limit not yet seen live).
- [x] Mail's From seen live after a mail login: "SOE.EQ.nmsdev.Nmscmistwo" (2026-10-29; the chat server answers logins again).
- [x] Bazaar: the PC and NPC target keys skip hidden traders and buyers (unit tests). The NMS bazaar's porch stalls (type 153) lie outside the zone geometry, so only the FRUGS302 rugs work as stalls (server data).
- [x] Bazaar: a right click on a trader or buyer the server sends as an NPC (Project Triune's offline traders) opens its shop or barter window: RightClickedOnPlayer tests the trader / buyer flag after the service classes for any spawn, not only players (mock test; the right click on the Triune bazaar traders not retried live).
- [x] Bandolier: Make Hot Button, Auto Swap saved, the slots' held right button (the original has no slot drag handler).
- [x] Merchant: the Qty and Lvl columns (checked live).
- [x] The marketplace window (CMarketplaceWnd) in the modern and gamepad packs: the original's store packets (catalog, states, wallet, purchase and its answer, the window's preview), the category tree, find, sort, the landing page and item of the week, quantity, the zip and confirmation pages, `/marketplace`, its key and the EQ button (unit and mock-server tests; live on Project Triune 2026-11-01: the catalog's 523 rows shown after Ready). Open: buying, and the rows docs/unknowns.md lists.
- [x] Merchant: the inventory money buttons' hook on the Parcels page (0x698290; the page itself was built 2026-10-28, 5a71bbc / dd8f339): the bag item let go, the coin outlined, the quantity answer picks the money to send (unit and mock-server tests). Checked live on the NMS dev server (2026-10-31): 5 pp picked through the coin-button path and sent by parcel left the purse once (500 to 495, one OP_MoneyUpdate), EQEmu stored it in `character_parcels` (5000 copper, the note) and the recipient claimed it (0 to 5 pp); in the gamepad pack's send page the purse row shows the coins and A on platinum, then the quantity, fills Send money ("495pp") with the purse unchanged (playprobe `--parcel-money`, `--parcel-claim`; screenshots).

### Network and session
- [x] Declining a group invite: the client handles 0x2a50; stock EQEmu maps it to nothing, so the server script relays it (`group_declines`, 5cf3f63).
- [x] The updated server script deployed on the NMS test server (`install.py`, `#reload quest global`); the group decline checked live: the inviter sees "Nmscmistwo rejects your offer to join the group." (2026-10-29).
- [ ] The server script's limits (files_rescan, files_chunks_per_second and files_chunks_per_second_zone with the requests over them held on a timer, eight files kept open, a file changed since its hashing, 5 group declines a sender in 10 s; Lua tests against a stand-in for EQEmu's API, tests/server_script_test.cpp): check live on an EQEmu server, a first download of a large client_files/ with two and three players at once (each served in turn under files_chunks_per_second_zone), and with a low files_chunks_per_second; and whether 768 pieces a second a player and 1024 a zone slow honest first downloads.
- [x] Server files: a piece that does not come is asked for again (at once when a later one comes, after 3 s of silence otherwise; 5 tries), so one dropped request no longer fails the download (unit tests against the fake script).
- [x] A refused duel is reported to the challenger (EQEmu's 12-byte OP_DuelDecline; checked live).
- [x] The duel partner leaving or dying keeps the original's 8-byte OP_DuelDecline: EQEmu's handler would dereference the departed partner's missing entity, and ends the duel itself (mock test).
- [x] Strings the client sends fit stock EQEmu's fields: a tell's recipient (63) and text (2047) as SendNewText cuts them (EQEmu's DECODE strcpy's the names into `char[64]` on its stack), the bandolier name (31), our own name in the world and zone requests, the name-change answer, the barter lines' names (mock and unit tests; the audit's list is in the commit).
- [x] /guildstatus sends the original's 140-byte GuildStatus_Struct, which EQEmu accepts (it dropped the 128-byte one) (mock test).
- [x] The trade partner leaving the zone cancels and closes the trade, as msgEQRmPlayer does (mock test).
- [x] Keep-alive packets: `00 06` after 15 s idle (14.8 s login, 44 s chat) (515e7ef; checked live, EQEmu dropped an idle client at 60 s without them).
- [x] Login: the kick request after 302/114 (eqmain's 0x16 / 0x32) with a Yes/No box (5c3a3cc; EQEmu never sends 302/114, mock tests only).
- [ ] Login: the reply's two leading bytes (unused by OnPlayerLogin), pushes 0x25 to 0x30 (EQEmu sends only 0x31).
- [x] The world and zone link-death timeout is the original's 180 s of silence (Params 0x12, UdpConnection +0x15c), not 15 s; 15 s is the keepalive delay (Params 0xe) and /offlinemode's exit window (0x3a98 @0x53ddc3). Login (14.8 s) and chat (44 s) keep the client's limits (docs/unknowns.md). Checked live on the NMS dev server (2026-10-31): a character idle in South Qeynos for 270 s stayed connected and camped out normally.
- [x] Session limits against a hostile or broken server: early packets bounded in bytes (4 MiB) as well as count, the delivered-but-unread queue bounded (64 MiB, then the session ends), an ack for a packet not yet sent ignored, a too-short first fragment ends the session instead of reassembling garbage or silently losing what follows, datagrams failing the CRC no longer keep the link alive, oversized datagrams dropped on every platform (unit and mock tests).
- [ ] What the original checks in OP_OutOfSession, and whether its last-receive time is set before its CRC check (unknowns.md). Until then a forged `00 1d` from the server's address ends a session (unknowns.md).
- [ ] Whether the original checks the address a SessionResponse comes from (the client takes any with its connect code, as multi-homed EQEmu hosts need; unknowns.md).
- [ ] eqhost.txt: the port the original uses for a `Host` without one, and reading the `Host1`.. fallback entries eqmain's `LoginClient::ReadHosts` reads (unknowns.md).
- [ ] The original CRT's atol on a number out of range: wrap or saturate (`/split`, `game::cmd_atol`; unknowns.md).

### Real estate (player housing, guild halls)
- [x] The packets, the manager (catalog, records, access, item rows, owned lists, areas, placed objects, occupancy), result messages, upkeep time left; unit and mock-server tests.
- [x] Buy / sell / transfer checks, the offer dialog, escrow deposit from and withdrawal to the cursor, /realestate, /leaverealestate, the windows' keys.
- [x] The guild bank in every guild hall zone; an instanced hall only for the guild whose plot holds it.
- [x] The plot, manage, items, neighborhood and plot search windows and the item context menu, in both packs.
- [x] Placed objects drawn in the world (collision, right-click menu), placement mode with its keys, the area check and the 0x711a confirm.
- [ ] Placement: the ghost's wall and surface fit, free mode's mouse moves, the flatness test (unknowns.md).
- [ ] Play it live against a server with housing (Project Triune) and settle the rows of the real estate section in unknowns.md.
- [ ] The item-move pre-checks and button greying, alternate-currency deposits, the NPC-item binding, the item menu's unbuilt entries, the neighborhood polls and vote window (unknowns.md).

## 2. Looks and sound

- [x] Zone MSAA coverage on Radeon RX 9070 XT: combine alpha and LOD sample masks consistently in the colour and depth shaders. [Qualification limits](unknowns.md#world-rendering).
- [x] Grass AO with MSAA on Radeon RX 9070 XT: zero-height lawn blades no longer send NaN; a global resolve-write to AO-read barrier (spec-redundant, an unconfirmed driver workaround) is kept while keeping grass depth writes and post effects. [Qualification limits](unknowns.md#world-rendering).
- [ ] Re-test the RX 9070 XT grass AO flicker with the lawn NaN fix alone and remove the AO memory barrier in `VkContext::begin_overlay` if it stays clear ([unknowns](unknowns.md#world-rendering)).
- [x] Shared actor/nameplate/pick/target visibility, idle texture-slot reclamation and capacity retry, four-weight EQG armor bindings, and server-selected carried lights; unit and mock packet tests. Live GPU acceptance remains pending.
- [x] Held-item WLD clouds and separate bitmap animation frames, animated actor textures, EQG held-item surface maps and authored additive lighting, character selection/item previews and instance cleanup; synthetic and real-asset regressions.
- [ ] Held-item effects: native WLD density setting and small-node suppression, animation-trigger timing and live comparison across equipment changes, zoning and supported model families (unknowns.md).
- [ ] Other players' carried lights after an equipment change: stock EQEmu sends nothing for them (docs/unknowns.md); trace how the original updates a remote player's light.
- [x] Name plates: same-frame per-pixel occlusion against the scene's depth (a hidden name is dropped, no ghost), nearer-over-farther draw order with overlap fading (10 %-20 % coverage to 10 % alpha, tau 0.3 s; the Fade Overlapping Names option), the target always full at its projected head, and a Highlight Target Name option; unit tests and an offline Bazaar viewer check.
- [x] Name plate text options (Options > Display "Name Plates"): font face, size, bold, shadow / outline / no edge, shrink with distance, guild and owner line size; unit tests for the settings and the distance shrink.
- [x] Invisibility markers on nameplates and HUD captions, with optional brackets for known invisibility versus undead (mock packets and UI tests).
- [x] Animation layering: the original's upper/lower bone groups, actions over the legs, per-group cross-fades and SyncToBoneGroup, rider actions on the arms, other spawns' water (d8c9fc2; model viewer and unit tests, live split not seen in a screenshot).
- [x] Animation: a busy upper body queues (classic, +0x10c4) or drops an action as the original does; the same action never replays over itself (84046fb; unit tests).
- [x] Animation: classic P08 / O02 after an animation 0x4b0860 does not name (the run, jumps, socials; not after an action, which keeps P01), the follow-up's +0x10c4 = 0x47; the local player's climbing animation on a ladder (anim-fixes; unit tests). Checked live on the NMS dev server (2026-10-31) on another player's Luclin human (HUM from globalhum_chr, an old-style model with P01 and P08) seen from the front by a second client: standing after zoning in and after a walk stop it plays P01 (elbows out, fists at the hips), after a run stop and after `/wave` P08 (arms hanging straight), matching the model viewer's P01 / P08. The local player's own stand was not seen from the front (the camera could not be turned round in a scripted run). The ladder climb could not be tried: this server has no ground object named LADDER*, the only kind the client climbs; its two OBJ_LADDER doors (xorbb door 8, pillarsalra door 7) are switches here (docs/unknowns.md).
- [ ] Movement collision: replay the reworked sphere sweep (`game/sphere_sweep.cpp`: capsule candidate order, tangent and zero-length edges, parallel edge-region refusal) against the recorded vectors (`HAILSTONE_VECTORS_DIR=... build/hailstone_tests -tc="*movement*"`, tests/movement_vectors_test.cpp) and update docs/mechanics.md's vector counts; it has only been checked with synthetic unit tests.
- [x] Remote ladder collision state reaches animation; new-style riders use column-5 sequences and sequence identity includes the style column. Regression coverage added; live ladder/rider acceptance remains open.
- [x] Riding while illusioned (Hailstone's own, switchable: `game.illusion_mounts`, on by default): the rider's model no longer refuses the mount; the mount's seat dag still must exist (docs/mechanics.md "Riding while illusioned"; unit tests). Seen live (stock PEQ test server, 2026-11-18): a gnome and a skeleton ride a summoned horse.
- [x] Mounted form changes unseat before rebuilding the actor and re-seat against the saved mount identity; departure and id-reuse are covered by mock packet tests.
- [x] Ladders placed in the zone (objects.wld LADDER* actors, type 6): walking into one climbs it (CollidingType 6 from the sweep's hit), a click from the top mounts it (zone-ladders; unit tests). Checked live (stock PEQ test server, 2026-11-18) (Upper Guk, Cabilis East's canal ladder, a gnome illusion).
- [x] Passable ladders (Cabilis's LADDERCB101, every polygon passable): clickable from the top as in the original, and climbed by walking or swimming into them by Hailstone's own rule, switched by `hailstone.cfg` `[movement] passable_ladders` (canal-ladder; unit tests, and the real Cabilis East zone with `EQ_GAME_DIR`). Checked live (stock PEQ test server, 2026-11-18) (Upper Guk, Cabilis East's canal ladder, a gnome illusion).
- [x] Solid ladders (Hailstone's own, switched by `hailstone.cfg` `[movement] solid_ladders`): the movement meets a ladder as one slab, so every race climbs the rung ladders, which small races fell through and every race could stick on (solid-ladders; unit tests, and Upper and Lower Guk with `EQ_GAME_DIR`). Checked live (stock PEQ test server, 2026-11-18) (Upper Guk, Cabilis East's canal ladder, a gnome illusion).
- [x] Complete five-word armor records across spawn/wear/save/preview paths, Hero's Forge category and membership filters, and Drakkin illusion feature updates; unit and mock packet tests.
- [x] Resolve modern Hero's Forge aliases in an armor-only cache and carry their native surface maps and layer parameters into modern actor lighting; collision, cache-order, material and asset regressions.
- [ ] Compare modern Hero's Forge fits across races/genders, animations, previews and illusion/equipment transitions against the original client; offline renders do not establish complete live acceptance.
- [x] Luclin eye colours and `SwapFace` (e7a5849; model viewer). Open: one-texture Luclin materials' tint (female dwarf faces 2 and 3).
- [x] Elementals: UseLuclinElementals (default on) draws race 75 as the typed Luclin elementals 209–212 by texture, and off draws those as race 75 (CreatePlayerActor's swap; unit tests and `chrdump` on the live asset folder, not seen live).
- [x] UseLuclinElementals off skips the implicit Global5_chr load ("Global NPC Models", @0x492780); synthetic archive-option tests. Hailstone loads neither Global5_chr2 nor frog_mount_chr implicitly.
- [ ] UseLuclinElementals off: whether the original also skips those archives when `GlobalLoad.txt` names them (docs/unknowns.md); they still load here.
- [ ] Hero's forge pieces without bones on Luclin models: narrowed (bracers' UVs land on the mail fill; gloves beside the hands); needs the DLL's rigid attach traced (ChangeBoneAttachment @0x10042c60).
- [x] Head items: the head's armour colour on helms (110650f); "keep head" has no effect in the original; the Velious helms beyond the table don't exist in the RoF2 files.
Fog, weather, sky, EQG surfaces, lighting and spell particles are a look-and-feel pass, not ports (user, 2026-10-29: "make it look good"); branches `look-atmosphere` and `look-surfaces`.

- [x] Fog and haze: eased between places, the sky's colour, a storm band, lightning (e069ae2). The original's area fog has no data in either install.
- [x] Weather transitions over tens of seconds (e069ae2).
- [x] Sky: a tilted sun path, ozone, ambient from the sky model, a colour grade (09624d4). Rough: dense forest shade, red dusk haze in lopingplains/commons, everfrost flat, befallen blue.
- [x] Volumetric clouds over the atmosphere (`display.volumetric_clouds`): marched at half size, a quarter of it a frame, the weather's storm closing them over (volumetric-clouds; unit tests for the maths, viewer screenshots; seen in a live session (stock PEQ test server, 2026-11-18)). Open: cloud shadows on the ground, god rays through the clouds, measuring the cost on an integrated GPU.
- [x] EQG: water reflects the drawn sky and the noswap environment maps, modern lava (crust, LavaH walls), blood/coloured falls, foam only on rising floors, animated texture sidecars, the one unpainted heightmap tile (scorchedwoods) (look-surfaces). Open: heightmap lighting colour, glow maps in the classic look, planar reflections, TAA.
- [x] Upscaling: a render scale (`display.render_scale`, 50 .. 100 %) with AMD FSR 1 (EASU + RCAS, `display.sharpness`) (render-scale; zone viewer with validation, not seen in game or with /viewport).
- [x] Hailstone Texture Studio: HD texture packs built from the player's files on their GPU (ncnn Vulkan, the PBRify models), a tool add-on of its own repository (projecthailstone/hailstone-texstudio) built on `src/texpack`; the add-on manager's tool kind (docs/addons.md "Tools") installs and starts it; Options and the login screen's More options pick the pack (from the next start); Options starts the studio (texstudio-addon; unit tests with stand-in models and GitHub in memory, one zone built on an RX 9060 XT and compared with the PyTorch pack). Open: an install from the real GitHub on Windows; measure an integrated GPU (Intel Iris Xe), NVIDIA and Windows' drivers; the 17 live textures the decoder cannot read; a pack's settings changing without a new pack; a pack applied without a restart.
- [x] Photo materials in built packs (docs/graphics.md "Photo materials"): a classic zone's ground tiles rebuilt from Poly Haven's CC0 photo textures by rules the texture studio ships, keeping the original's layout and colour (unit tests with stand-in materials; the 34 classic outdoor zones built and seen in the zone viewer). A zone file may set a material's colour in its art. Open: the expansions' zones; the photos' repeat on large flat areas; a preview of a material tile in the studio; the studio's size estimate ignores the material tiles' size.
- [x] Model overrides (docs/graphics.md "Model overrides"): binary glTF models in a texture pack drawn in place of a zone's objects, the collision kept on the game's mesh (unit tests; seen in the zone viewer). Open: doors and other moving objects (the object renderer); a scale or offset for models made by hand.
- [x] Generated trees (docs/graphics.md "Generated trees"): a classic zone's trees grown again as models fitted to the game's trunks and leaves, with photo bark, from the texture studio's rules (unit tests; the 34 classic outdoor zones built and seen in the zone viewer, 129 trees; dead trees and palms). Levels of detail done (near, middle, far cards with pictures; about a tenth of the triangles). Open: the expansions' zones; palms' fronds hang less low than the game's; cut-out leaves thin out in the smaller mips; EQG zones' trees. Shadows draw coarser levels than the scene (7,100 shadow triangles at the test spot, as the game's trees); each tree kind is stored once and drawn instanced; levels fade into each other with a dither; Options > Tree Detail Distance. Pines thinned and the middle level keeps every card (no leaf change between levels); open: Greater Faydark still draws about 1 ms slower than with the game's trees at 3124 x 1366, 8x MSAA (zone viewer), and the flicker the change targets was not reproduced with a still camera, only checked as the near and middle levels drawn alike. The needle flicker in the wind and the shimmer walking through a forest came from stark needle stripes in motion: needles now keep close to their spray's mean shade (zone viewer: Greater Faydark flight 11 % -> 5 % of pixels changing per frame, the game's trees 3.5 %; open: the rest of that gap, and broadleaf leaves, which keep their wider shades).
- [x] Horizon hills (docs/graphics.md "Horizon hills"): land round an outdoor zone past its edges, a mesh built from the zone's own terrain and drawn with its own ground, rock and water materials, starting at the zone's edge and rising into hills and mountains; the sea past a shore (unit tests for the measuring and the mesh; viewer screenshots of qeytoqrg, with its neighbours, and butcher, modern lighting; not yet in a session). Trees on its woods, the zone's own drawn far as pictures on crossed cards (unit tests for the placement, the surface, the picture and the atlas's tree counts; viewer screenshots of qeytoqrg, northkarana, butcher and eastkarana; frame time measured in southkarana; not yet in a session). The land as a generated terrain: warped hills worn by water drops and slumping, held off the zone's ground and the sea, cached per zone; coasts following the ground; rock, ground and sand splatted in one draw, the rock blended from three axes and its repeat broken up (unit tests for the wear's determinism and constraints, the coast, the cache; a fuzz case for the cache; viewer screenshots of the five comparison views and two clear overviews; not yet in a session). Open: no snow on it; the wear's gullies are finer than the 64-unit grid shows; one ground and one rock texture (the zone's most used); an EQG zone's terrain-tile materials are not used (no land when the zone has no plain one); the lid over a pocket the zone closes round is flat.
- [x] Zone edge fog (docs/graphics.md "Zone edge fog"): a fog bank on the ground past an outdoor zone's terrain, marched per pixel through a field from the land's, with a soft, swelling top and drifting patches (unit tests for the field and the mean density; viewer survey of 77 views across 17 zones; GPU time in North Karana; not yet in a session). Open: its colour is the haze's alone (no light from above on its top, no shadow under clouds); the depth it marches to is sample 0 of an MSAA scene, so an edge against the fog keeps that sample's depth; tuned by eye on the survey, not against a reference.
- [x] Neighbour zones, the world atlas and the distant copies removed: drawn past the zone lines (live, then as copies of the zones visited on the server, the land shaped by their ground), they showed walls, filler and seams at the lines that no rule fixed across the game's zones. The land round a zone is its own again (docs/graphics.md "Horizon hills"); the map's zone exits keep the zone-line grouping (`game/zone_exits.h`).
- [x] Smoothed ground (docs/graphics.md "Smoothed ground"): a classic zone's solid, opaque ground drawn with crease-aware smooth normals and curved PN triangles cut up to 8 segments a side, within 1 unit of the flat ground, without cracks; collision and everything the server measures keep the file's terrain; the zone flora stands on it (unit tests, qeytoqrg's crack count with the game folder, viewer screenshots and frame times of qeytoqrg; not yet in a session). Open: a larger offset (rounder outlines) would need feet and the server's spawns raised to the drawn ground; neighbour zones and EQG zones are drawn flat; no Options entry; the shadow cascades draw the refined triangles too.
- [x] Zone flora for classic zones (docs/graphics.md "Zone flora"): rules files of Hailstone's own, plants from a donor EQG archive, `+green` texel masks; East Commonlands first (unit tests and the zone files; seen in the zone viewer with the modern lighting, not yet in a session).
- [x] Zone flora for every outdoor classic zone (flora/default.txt): no plants under the zone's own roofs, ceilings and bridges, nor on water, built surfaces or rock; grown tile by tile round the camera (unit tests; tools/florascan over the 148 outdoor classic zones of an NMS asset folder; viewer screenshots of a dozen zones, modern lighting; not yet in a session).
- [x] Flora lit in the shaders (the sun's shadow per pixel, the sky, the place's light and the lamps per vertex), bending round the spawns standing in it, up to 262,144 plants a frame; climate families of donors for zone flora (unit tests; viewer screenshots and frame times, modern lighting; not yet in a session).
- [x] Zone flora's grass is a lawn built in the shaders (no game asset): an even carpet, a patch per 3.2-unit square, blades at three levels of detail, the wind, the bending, tapered edges; the donors' kinds scattered over it (unit tests; viewer screenshots and frame times in Qeynos Hills, South Karana and Greater Faydark, modern lighting; not yet in a session, nor with the classic lighting).
- [ ] Zone flora: a placed object's roof (a tent, a hut that is an object) still has grass under it; zones the default suits less well may want files of their own; a dev-UI view to try rules in a zone and save them; the classic lighting and the bending in a session.
- [x] Upscaling, temporal groundwork and TAA/TAAU (docs/graphics.md "Temporal upscaling", `display.upscaler = taa`): projection jitter (Halton 2,3, FSR 2's phase count) kept off picking and the name plates; camera motion vectors rebuilt from the depth; AO applied before the resolve; a resolve of our own at the view's size (proximity-weighted samples, YCoCg variance clip, Catmull-Rom history, luma-weighted); the post pass at the view's size, RCAS after it; history reset on zone-in, camera cuts and resize (unit tests for the maths; zone viewer with validation; not yet in a session).
- [x] Upscaling, temporal: motion written by the scene pipelines (the zone's wind sway, characters' bones, doors' and objects' transforms, flora and lawn) into a motion attachment, and a reactive mask from the blended and additive draws (docs/graphics.md "Temporal upscaling" 6) (unit tests for the blend states and keys; zone viewer with sync validation, and a debug view showing still ground matching the camera to under 0.01 px and swaying plants with motion of their own; not yet in a session, so characters and doors are untested on screen).
- [ ] Upscaling, temporal: a negative texture LOD bias below a render scale of 100; the bloom from the resolved picture; held items and other attachments keyed for their motion.
- [x] Upscaling: AMD FSR 3.1's upscaler (`display.upscaler = fsr3`, docs/graphics.md "AMD FSR 3.1"): the SDK v1.1.4's GLSL passes vendored (MIT), compiled by our CMake, run by a host side of our own on the temporal upscaler's inputs, its own RCAS at display.sharpness, auto exposure (unit tests for the option; zone viewer with validation and synchronization validation at render scales 50, 67 and 100; not yet in a session, nor on a GPU other than the developer's).
- [ ] Upscaling, FSR 3.1: try a transparency-and-composition mask for the water.
- [x] Upscaler providers (`display.upscaler = <name>`, docs/graphics.md "Upscaler providers", docs/plugins.md, `sdk/hailstone_upscaler.h`): temporal upscalers from outside the client as add-ons of the kind `upscaler` in `upscalers/<name>/`, loaded at the start from a copy, their instance and device requirements checked and added, run in FSR 3.1's place, FSR 3.1 or taa when one is missing, refused or failing; the example provider (a linear blit) (unit tests for the requirements, names, choices, loading, the copy and the add-on kind; zone viewer with validation on the developer's AMD GPU: the example at render scales 50 and 67, with frame generation, a provider refusing the GPU, one not installed and one failing a frame; the MSVC build under Wine on the same GPU with the XeSS and FSR 4 add-ons, both running; on Windows on an Intel Iris Xe tablet: XeSS running, FSR 4 refusing the Intel GPU for FSR 3.1, with and without frame generation; not yet in a session, with the camera moving, or on an NVIDIA GPU).
- [x] Upscaler add-ons, each in its own repository against `sdk/hailstone_upscaler.h`, released as 1.0.0 for clients from 0.7.2-beta: Intel XeSS (projecthailstone/hailstone-xess; Windows only: the SDK ships no Linux build; any vendor's GPU) and AMD FSR 4 (projecthailstone/hailstone-fsr4; Windows only; RX 7000 and 9000). NVIDIA DLSS is not planned: its licence forbids use under an open-source licence and puts logo and approval duties on the application.
- [ ] AMD FSR 4 add-on: the FidelityFX SDK's licence file lists its signed DLLs under MIT; confirm that with AMD.
- [ ] Upscaler add-ons: list XeSS and FSR 4 in the add-on index once 0.7.2-beta is released.
- [x] Frame generation (`display.frame_gen`, docs/graphics.md "Frame generation"): an in-between frame from the temporal upscaler's motion and depth (scatter at t = 0.5, holes from the last frame, reactive pixels from this one), the UI composited sharp over both, FIFO with one more swapchain image (unit tests for the decisions and the image count; zone viewer with synchronization validation under taa and fsr3, in-between frames checked against the real ones round them at 60, 30 and 20 fps of motion; a session on the stock test server with validation; not on a GPU other than the developer's, nor on a variable refresh rate screen).
- [ ] Frame generation: pace by the measured frame time (kettle-framegen's) instead of FIFO alone, so a game that cannot hold half the refresh is not stuck at a third; consider a 3x/4x mode for 240 Hz screens.
- [x] Doors: orientation already matched SetEulerRotation512 (655 of 665 hinged doors in 21 zones fit their doorways); invisible switches now block movement as in the original; harbingers' BCN loads from its .mds (c34ca9e, d59971c; walked live in halas and felwithea). Open: BCN's actordef name, piece and animation; the door-angle sign.
- [x] Lighting: characters and doors take the light of their place (ambient probe), tinted baked light balanced (192ec47).
- [x] EQG terrain: retain its baked RGB and sky-light alpha from a matching `<TER file basename>.lit` sidecar, without applying the terrain placement transform; synthetic asset regressions.
- [x] EQG static `CBGG1` / `CBSG1` materials: decode packed RG normals, blue glow and alpha gloss; modern HDR glow for the authored self-illumination and additive zone rays, with shared shader-kernel and material regressions. The channel layout is not confirmed (docs/unknowns.md, EQG packed surface maps).
- [ ] EQG packed surface maps on characters: the actor renderer ignores `EffectParams::packed_normal` and `glow`, so an EQG character or Hero's Forge piece on `CBSG1` reads its packed map as an RGB normal map and shows no glow.
- [ ] EQG `AddAlpha` HDR gain: decide whether the modern lighting's 2.5x applies to every EQG zone's additive surfaces or only where it was checked (Forgotten Halls).
- [ ] Indoor lighting: tune Forgotten Halls' overall readability after restoring terrain lighting and packed material channels, then check other indoor and outdoor zones (docs/graphics.md "Modern lighting"). The terrain correction restores lamp pools and removes sunlit bands but leaves the room darker overall.
- [x] Spell particles: the EDD billboard modes (flat and axis), animation-started actor emitters (look-surfaces; billboard meanings read from names, not traced).
- [x] Spell particles traced from EQGraphicsDX9.dll: CreateEmitter's counts and capacity, the update's emission, spawn shapes, motion and ramps, and the quad builder's billboard modes (what the update reads from the actor and the creator's arguments are in unknowns.md).
- [x] Audio: the local player has no loop sound in the original; `/reversesound` has no effect in RoF2, here it swaps the whole output's channels (our choice) (f7b19af).

### Updates (docs/updater.md)
- [x] The updater: the signed manifest from the release feed, the download (core, browser and the installed plugins as their own packages, an unchanged one skipped), the install at quit with Restart now, the rollback of a cut-off install or of a version that does not start, and the notice in both packs' login screens and the client's own. Unit tests; a real run on Linux against a local HTTP server (`hailstone_sign try`: packages from `make_update.py`, signed, downloaded with the system's libcurl, installed over a copy of an install).
- [x] The login screen's corner: this build, whether it is up to date, What's new (this version's part of the shipped CHANGELOG.md) and the GitHub page, in both packs and the client's own (tests/changelog_test.cpp, tests/lua_ui_pregame_test.cpp, tests/ui_pack_windows_test.cpp, tests/dev_ui_test.cpp). Open: seen with a real update check (only test runs and unit tests so far).
- [x] The repository (or a public one for releases) has to be public: GitHub answers 404 to the client for a private one's releases. Public from 0.5.0-beta (AGENTS.md "Git workflow").
- [x] A release key (`hailstone_sign keygen`, the public half in `update-key.pub`); until then builds have no updates.
- [ ] The updater on Windows: WinHTTP and the install's renames of the running `hailstone.exe` and its DLLs (compiles; not run).
- [ ] A release job in CI (packages, manifest, signature, upload).
- [x] macOS arm64 releases: a plain folder like Linux's with MoltenVK beside the binary, built, tested and signed ad hoc by the macOS workflow (docs/ci.md "macOS arm64"), packed and signed into the manifest locally. No Developer ID or notarization, by choice: players clear the quarantine (README.md).
- [ ] macOS on a Mac: the client run on Apple Silicon (MoltenVK's device features, the window, input, audio, the Keychain), and an update installed there. Only the build and the headless tests have run.
- [ ] macOS: the saved passwords use the Keychain's SecKeychain calls, deprecated since macOS 10.10 (Apple Clang warns, `platform/credentials.cpp`); SecItem's calls replace them.
- [ ] macOS: the startup error for a missing Vulkan names the graphics card makers' drivers (app/startup_error.cpp); on macOS it would be the missing `libMoltenVK.dylib`.
- [ ] Downloads that resume (HTTP range requests) instead of starting over.

### Add-ons (docs/addons.md)
- [x] The manifest and GitHub link parsers, the library header reader; unit and fuzz tests; `tools/make_addon.py`, and `hailstone_sign` for add-on manifests.
- [x] Install and uninstall from a GitHub link on the login screen, scripts and plugins (content rules, the Lua and library checks, `addons/installed.txt`, a journaled install undone when cut off); the server's own listed and not removable; the Modern pack's card, the Gamepad pack's page and the client's own window.
- [x] The add-on store: every add-on of the signed add-on index, with its manifest (held to the index's name and key), Install through its install card; the Modern pack's add-ons card beside the characters card (tests/addon_index_test.cpp, tests/lua_ui_pregame_test.cpp, tests/ui_pack_windows_test.cpp, tests/dev_ui_test.cpp). Open: listing the real index live, and lines in the index for the add-ons beyond Project Triune's.
- [x] Lua Scripts window (`ScriptsWnd`, both packs, the EQ menu's Lua Scripts row, Gamepad More > Lua Scripts): every script in the folders with its state, Run / Stop / Pause, the reason a script may not run, and a per-character Autorun list (`scripts.ini`) started at the login's zone-in, not again at a zone change. Unit tests (the scan, the autorun file, the rules through a zone change) and both packs drawn headless with a script host. Tried live in both packs (stock PEQ test server, 2026-11-18): an autorun script started once at login and kept running across a zone line; Run, Stop and Pause.
- [x] The manifest's `caps` as a ceiling on what an installed add-on gets (`plugin::Policy::set_ceilings`).
- [x] Plugin and add-on windows confined in the UI's Lua state: their own globals, `hs` / `ui` / the libraries read-only, their code (and every `hs` function they took) run under their capabilities, text-only loads, `dofile` kept to the plugin's and the pack's folders (tests/lua_folders_test.cpp, tests/lua_ui_pregame_test.cpp).
- [x] An update's approval holds only for the capabilities and version the player saw (tests/addon_manager_test.cpp).
- [x] The player's word wins over a check under way: an add-on removed meanwhile is not installed again, auto-update switched off meanwhile is not overridden, and an update keeps the switch as it is at install time (tests/addon_manager_test.cpp).
- [x] Scripts started by a script or plugin get no more than their starter; `scripts/config/` never runs as code (tests/mq_lua_test.cpp).
- [x] A server's compiled plugins load only once the player allowed their bytes for that server (`servers/<profile>/plugin_approvals.txt`, the prompt in both packs and the client's own, changed from the add-on list; tests/native_plugins_test.cpp, tests/plugin_consent_test.cpp, tests/ui_pack_windows_test.cpp).
- [x] A module runs only in a script it may do as much as: an add-on's module is refused in a script that may do more (tests/mq_lua_test.cpp).
- [x] Binding a command needs 'commands'; scripts and plugins cannot shadow the game's or the client's own commands (`Hooks::new_guest`, `reserved_command`; tests/plugin_hooks_test.cpp, tests/mq_lua_test.cpp, tests/native_plugins_test.cpp).
- [x] The clipboard (`ui.GetClipboardText` / `SetClipboardText`) only for scripts with every capability (tests/mq_lua_test.cpp).
- [x] A script's or plugin's `/plugin <name> load` never shows the consent question; only the player's own asks again (tests/native_plugins_test.cpp).
- [x] Values between Lua and plugins: cycles refused, a budget of values and bytes (`plugin/lua_value.h`; tests/lua_ui_args_test.cpp).
- [x] Script and module names stay inside the script folders (no absolute names, no links out); the config check canonical and case-insensitive (tests/mq_lua_test.cpp).
- [x] `mq.pickle`: a script without every capability gets `scripts/config/<script>/` (tests/mq_lua_test.cpp).
- [x] Scripts' tables cannot have `__gc` (a finalizer at the close registered hooks as the client's own, keeping the freed script); nothing is registered while a state closes (tests/mq_lua_test.cpp).
- [x] A Lua social's dry run does nothing but note its commands and waits; `hs.LuaSocialDryRun` needs `commands` and `actions` from a plugin (tests/mq_lua_test.cpp, tests/plugin_ui_sandbox_test.cpp).
- [x] The Lua bridge checks its arguments before it makes C++ strings (an error longjmps past them: Lua is built as C).
- [x] `mq.imgui` windows held to the 5 s runaway limit, also after a hook ran; a window that destroys windows no longer reads past the list (tests/mq_lua_test.cpp).
- [x] `mq.delay` and the hooks' number answers defined for NaN, infinities and huge numbers (tests/mq_lua_test.cpp, tests/lua_plugin_test.cpp).
- [x] `${Int[...]}` and `float.Int` defined for NaN, infinities and huge numbers (tests/mq_tlo_test.cpp, tests/mq_lua_test.cpp).
- [x] `mq.TLO` values' metatable locked, so a script cannot swap in a `__gc` (tests/mq_lua_test.cpp).
- [x] MQ's `Math` TLO (its Calculate arithmetic), `If` and `Range`, and the rest of MQ's int, float and string members, with MQ's edge cases (`mq/calc.h`; tests/mq_calc_test.cpp, tests/mq_tlo_test.cpp).
- [x] Spawn searches follow MacroQuest's words and matching rules: spawn types, pets, 3D radius, named test, group, raid, guild, xtarget, light, player state (`mq/spawn_search.h`; tests/mq_tlo_test.cpp).
- [x] The alerts key (CMD_HISTORIC_ALERTS, __ExecuteCmd case 168) toggles AlertHistoryWnd in both packs, listed with its Shift+H default (unit tests; seen live in the Modern pack on the stock PEQ test server).
- [ ] MacroQuest's alert lists (`/alert`, and the spawn search's `alert`, `noalert`, `nearalert`, `notnearalert`): none kept yet, so those words act as with an empty list.
- [ ] The spawn search's `next` / `prev` (MacroQuest's `/target next`, cycling from the current target through the matching spawns): accepted and ignored.
- [x] `mq.event` patterns match as MacroQuest's Lua events do (Blech): case-insensitive, every `#...#` up to the first place the next literal is, `#name#`, `|${...}|`, `##` / `||`, arguments by number with gaps, item links shown as their text unless `keepLinks` (`mq/event_pattern.h`; tests/mq_event_pattern_test.cpp).
- [x] Host functions only on the client's thread: off-thread calls do nothing (`MainOnly`), the caller's capabilities are none there (tests/native_plugins_test.cpp, tests/plugins/HostProbe.cpp).
- [x] C++ exceptions out of a plugin caught at every entry point (`guarded`); a failed `InitializePlugin` leaves nothing registered (tests/native_plugins_test.cpp).
- [x] `web_state` fills what a shorter `HailstoneWebState` holds; its `size_t` size kept for ABI 3 (tests/native_plugins_test.cpp).
- [ ] The server plugins' prompt live: a first visit to a server that ships a plugin (NMS), Allow mid-zone and Don't allow, in both packs and with a gamepad; whether a server extension allowed mid-zone needs the next zone-in.
- [x] Plugin windows' functions left in the tables the pack keeps in its globals (`GPSTATE`, `modern`, `ui_palette`) ran as the player when the pack called them: the plugin now sees those tables through views that wrap what it writes to run as the plugin (and to stop when it unloads), and refuse setmetatable, rawset and `__gc` (ui/lua/lua_host.cpp).
- [x] Plugin windows: the pack's functions are wrapped so what they return or raise reaches the plugin as a view; the pack's arguments to a plugin's callbacks are views; `getmetatable` gives a view of a metatable the plugin did not set (ui/lua/lua_host.cpp; tests/plugin_ui_sandbox_test.cpp).
- [ ] A function or table a plugin's code passes to a pack function as an argument, or returns to the pack from a callback, is not wrapped: a pack function that keeps it in its own tables and calls it later runs the plugin's code as the player. Wrapping every value that crosses into pack code (a full membrane) would close it, at a cost on every call; the packs' shared libraries that keep callbacks should keep them per window instead.
- [x] Auto-update: the check at start (`addons.check_hours`), locally modified files, files moved only at the login screen, approval when the caps grow.
- [x] Signatures: the key pinned at the first install, an update with another key (or unsigned) refused.
- [x] Suggested add-ons: the signed add-on index (projecthailstone/hailstone-addon-index) and a server's `[addons] suggest`; a line on the server select, Look at it / Not now / Don't ask again on the login screen in both packs and the client's own; a suggestion's card held to the index's name and key (tests/addon_index_test.cpp, tests/lua_ui_pregame_test.cpp, tests/ui_pack_windows_test.cpp).
- [ ] Suggested add-ons live: a first login to Project Triune from a fresh install, with the real index on GitHub.
- [ ] Add-ons against the real GitHub: a public repository's release and branch, through GitHub's redirects to its asset hosts (unit tests and an in-memory GitHub only).
- [ ] Add-ons on Windows (the WinHTTP final-address check compiles; not run) and a plugin add-on on macOS.
- [ ] A Hailstone release whose `core` package ships a plugin folder an add-on already installed would write over it: the core install does not yet skip add-ons' folders (only the optional `plugin-<name>` packages are skipped).

## 3. UI

- [ ] Trace and fix the upstream gamepad all-pack smoke test's cursor-extent assertion with ImGui debug checks enabled (docs/unknowns.md "UI").

- [x] Group role controls in both packs: right-click member names or the player's own name (Y on a gamepad Group window row), with server-confirmed role icons beside names in the Group and Player windows; leader-only Main Tank / Main Assist / Puller changes, with mercenaries restricted to tank. Unit and mock-server coverage; live gameplay remains unverified.
- [x] AA names missing from dbstr_us.txt (a live folder's renumbered titles, gaps in a server's export): the windows show the spell's name, else "Ability <group>" (GameConnection::aa_display_name; plugins and /alternateadv list keep the raw title). Unit test; seen live on the stock PEQ test server.
- [x] Modern AA window: aligned headers and rows at the dialog's scale, current-frame column bounds, saved column resizing, clipped cells and Reset columns (UI input tests; live acceptance pending).
- [x] Hover respawn: validate availability; recover same-zone player identity, placement and resources; handle resurrection confirmation, server timeout and disconnect safely. Both UI packs and loopback packet sequences have regressions; live gameplay acceptance remains outstanding.
- [x] Modern Quest Journal and independent Quest Tracker: compact tracked-first Active and History pages, with shared tasks included in Active; a Shared members popup for the native roster, member roles and permission-gated membership/leader actions; Objectives/Description/Rewards sections, individual hidden objective rows with native group separators, and per-character tracking flags. The tracker reads authoritative active tasks while the journal is closed and uses the shared HUD move/resize/lock/roll-up behavior. Packet-driven and headless UI tests; live quest updates across zoning, shared roster updates and membership actions, and mouse interaction still to check.

- [x] Labels: combat timer, breath, respawn gauge, aggro meter, title, ATK, regeneration, and more EQTypes, in the modern and gamepad packs (ui-gaps, merged up to 90aa43f; checked live). ATK leaves out profile +0x3468; the label and gauge types still not tracked are listed in unknowns.md.
- [x] Buff icon fading below 19 s; gauge colours for special values (unit tests, not seen live).
- [x] Hot buttons: the AA and gem right clicks, vertical bars (modern pack, checked live), the key-map text corner; the ranged attack was already in.
- [x] Hot buttons: vertical bars in the gamepad pack (checked live). Open: the AA and gem right clicks seen live.
- [x] Hot buttons: an inventory or bag slot held (or dragged, modern pack; LB in the gamepad pack) gives an item link or inventory slot button; item links find their item, show its icon and use it (unit tests and mock server). Open: seen live, the Krono slot's hold (type 13), the bank's slots.
- [x] The login, server select and character select screens drawn by the modern and gamepad packs (`app/lua_ui_pregame.cpp`, `ui/<pack>/login.lua`, `serverselect.lua`, `charselect.lua`); a redone login (Continue as, the server's settings and More options folded away). Unit tests, and the server and character select seen live against the local test server in both packs. Open: played through live: a world played, a character entered, created and deleted, a box login, a server saved, deleted and its interface switched from these screens.
- [x] Return Home on the character select (CLW_Home_Button: enabled by the list's go-home flag, asked first with eqstr 6137, then OP_EnterWorld with the home flag), in the ImGui screen and both packs (`hs.CharacterSelectDo("home")`, `canGoHome`, `homeQuestion`). Unit and mock-server tests. Open: tried live against a server with `World:EnableReturnHomeButton` on; the tutorial button (CLW_Tutorial_Button, the same packet's tutorial flag).
- [x] The character creation screen drawn by the modern and gamepad packs (`hs.CharacterCreate`, `ui/<pack>/charcreate.lua`): name with a random one, gender, race, class, deity, start city, the stats, the game's descriptions, Appearance. Unit tests and the packs drawn headless. Open: played through live in both packs (a character created, the random name, Appearance and back).
- [x] The spell book: memorize only into an empty gem; Mem. This Page does nothing in RoF2 either.
- [x] The compass's heading error below Sense Heading 200, reset by a successful Sense Heading (checked live); sense heading's raw heading and the South band match the original.
- [x] The item display's placeable, collectible, No Destroy, No Storage and trophy lines (stock EQEmu sends none of this data; unit tests).
- [x] Context tips for experience, invites, consent, red-con target, world containers and more; the tip table's ids fixed (two missing entries shifted every tip from 0x38); OP_ConsentResponse / OP_DenyResponse handled (unit tests). Open: the tips listed in unknowns.md.
- [x] The chat server's per-window channel state is saved (it already was; a test added).
- [ ] The chat log (/log): trace `CEverQuest::OutputTextToLog` @0x5130c0's file handling (opened and closed per line, or held open) and match it; the client holds the file open while logging (docs/unknowns.md).
- [x] Tracking window: the controls run the /track commands, auto update saved (modern checked live).
- [x] Tracking death cleanup (WORLD-007): tracked-target deaths send the stop request and tracking message;
  known deaths remove the cached entry, including local deaths (loopback packet regressions; live acceptance pending).
- [x] Tracking in the gamepad pack's More window for characters with the skill (checked live).
- [x] `/copylayout`, `/offlinemode`, `/bugreport` (f3c5c5d; `/offlinemode`'s yes path not seen live).
- [x] Ability Book (Hailstone's own, `game/ability_book.h`, `ui/modern/abilitybook.lua`): activated AAs, combat abilities and activated skills in one searchable, sortable list with descriptions and timer groups; double-click / drag to a bar / right-click menu; the EQ menu row after Combat Abilities and `/abilitybook`; client-side autoskill (ordered list; while fighting, any time or out of combat for beneficial abilities; only when its buff has worn off; endurance, mana, own health and target health above / below conditions; failure backoff; the switch kept per character across logins; `hailstone.cfg` `[abilities] autoskill = off`). Checked live on a cleric (autoskill fired Bash on a #spawn dummy). A class filter shows that class's abilities and the shared ones; the class dropdown sits after the filter chips. Open: the gamepad pack's page; the class filter and sort seen with a multiclass character (NMS sends every AA as every class's, docs/unknowns.md); a key binding for it.
- [x] Advanced Inventory (Hailstone's own, `InventoryManagerWnd`, `ui/modern/inventorymanager.lua`, `app/lua_ui_inventory_manager.cpp`): everything worn, carried, banked and in the shared bank in one list or grid; search, place / kind / mark / flag / stat filters with counts (a stat: the items that have it, with its column), Clear filters; sorting by a click on a column's heading (place grouped by bag, name, level, the stat, quantity, value, weight; a second click reverses; the grid has a Sort menu); multi-select in the list and the grid (click, Ctrl, Shift, Ctrl+A) with Sell / Tribute (personal or guild master) / Destroy (carried, or banked while the bank is open) / Withdraw (from the open bank into the packs, never equipped) of the selection, each checked against the item's serial and quantity at confirmation and while the job runs (`GameConnection::start_bulk_job`, docs/mechanics.md "The Advanced Inventory's bulk actions"); `/finditem [text]` and `/advi`; each icon is the item's slot (pick up, drop, use, inspect, hot button), banked ones while the bank is open; Keep and Sell marks per item, for everything shown at once too; Sell marked at a merchant; Stack / Sort / Tidy. The EQ menu row after Inventory and a button in the inventory window, whose search, Stack / Sort and ... menu moved here. Keep (`game/kept_items.h`, `kept_items.txt` per character) blocks destroying, dropping, trading, giving, selling, parcels, barter sales, bazaar offers, buyer compensation and tribute (docs/mechanics.md "Kept items"); bulk Keep marks write kept_items.txt once. Tested: unit, the window over Dear ImGui and the mock server, the refusals and the bulk sell, tribute, destroy and withdraw jobs on the mock server, delayed and missing tribute answers. Open: not checked live (the bulk tribute, withdraw and destroy against a real EQEmu especially); the gamepad pack's page; a key binding for the window.
- [x] The bank as last seen (`game/bank_snapshot.h`, `bank.dat` per character; docs/mechanics.md "The bank as last seen"): EQEmu's `Inventory:LazyLoadBank` sends the bank only near a banker, so the Advanced Inventory and `/finditem` list the last copy until then, marked stale and out of reach (`hs.BankSeen`). Tested: unit, the mock server (lazy load, zoning, the window opening, the file across sessions), the window over Dear ImGui. Live on the stock PEQ test server: the lazy load, bank.dat and the next session's copy (playprobe `--bank-file`). Open: a server with the rule off, the real client's window live (docs/unknowns.md); socketed augments are not listed or searched yet.
- [x] Modern UI item marks (junk, locked slots, bag filters; `ui/modern/lib/itemtools.lua`) are kept per server and character: `items.` keys are the character's settings (`LuaUi::character_setting`), in `servers/<profile>/.../ui-modern.txt`; marks from the shared file are read until the character changes them, and a layout copy leaves them.
- [x] The client's settings per character (docs/layout.md "A character's settings"): the character's own `hailstone.ini` from the entry into the world, made from the shared one at its first entry, and applied (graphics, frame caps, sound, mouse, keys, gamepad, the game switches) on entering and leaving. Unit tests (the layer, the shared keys). Open: not seen live with two clients; a way to copy one character's settings to another or back to the shared ones; the window's place per character; the character's file in a bug report (it would share `settings/hailstone.ini` with the shared one).
- [ ] The in-game browser (docs/browser.md "Milestones"; the messages, pipes, tab host, the Linux helper, the window in both packs and the scripts' and plugins' tabs are done): the helper on macOS (app bundles) and checked on Windows and Linux arm64 (CI), Chromium's sandbox on Windows and macOS.
- [x] Scripts' and plugins' pages held tighter (docs/browser.md "Rules", "Profiles", "Sizes"): a cookie store per add-on, frames and requests on the allowed sites, never this computer or a private network, console lines and page messages bounded in bytes (unit tests; the CEF helper's frame and request refusals in its test).
- [ ] Scripts' and plugins' pages: a public host name whose DNS answer is a private address is not refused (it would need the helper to check the address it connects to); whether CEF hands WebSocket handshakes to `OnBeforeResourceLoad` was not checked.
- [ ] The other local-time calls outside `src/platform/` go through `platform::local_time` (`localtime_r` / `localtime_s` in `net/game_connection_social.cpp` and `mq/tlo_game.cpp`; `std::localtime`, which shares one buffer between threads, in several `app/lua_ui_*.cpp`, `market_ui.cpp`, `client_app_info.cpp` and `net/game_connection_market.cpp`). Bug reports already do.

## Navigation (docs/navigation.md)
- [x] Navigation meshes built from the zone's collision on worker threads, cached per server and zone; areas from the regions (water, lava, zone lines, PvP).
- [x] `/nav` (MQ2Nav's commands), `/moveto`, the `Navigation` TLO; the walk steers by the heading and the movement keys, gets unstuck, opens doors in its way, and asks the server's `/find` path while the mesh is building.
- [x] Tried live on NMS Dev (ecommons): long walks, /moveto, a door, the TLO, the overlay, pause, cancelling with a key, an unreachable point.
- [ ] Live: a moving spawn, a closed door clicked on the way, the server-path fallback, a stock EQEmu server.
- [ ] Find replies: a quiet path request and a shown `/find` in flight together can still swap on stock EQEmu (an empty reply is sent at once, a full path later); the 10 s cut-off for an unanswered request is a guess (docs/unknowns.md).
- [x] /follow helped by the mesh (round corners and walls, unstuck, a 400 leash), switchable with `nav.follow`.
- [ ] /follow's help live (following a player round corners and through a doorway).
- [ ] Lifts, jumps and safe drops as off-mesh links (drops checked against the fall-damage rules).
- [ ] Walks across zones (`/travel <zone>`) over the zone lines and zone points.
- [x] The map's World view and zone finder (docs/navigation.md "Zone atlas"): the atlas pages zoomed and panned, zones searched by name, another zone's map, routes over the server script's zone links or the zone points learned per server, the exits on the map and "Walk there" through the next zone line (modern pack). Unit tests and the server handler under LuaJIT; not tried live.
- [ ] Zone atlas: live against a server with the script and a stock one; the World view in the gamepad pack; the route going on by itself after each zone (with `/travel`); while another zone's map is shown the minimap draws it too (they share the map state).
- [x] `/stick` (distance, behind, id; the `Stick` TLO), the `LineOfSight` TLO and `Spawn.LineOfSight` (the abilities' own check). Unit tests on a built mesh; not tried live.
- [x] The Find window (Hailstone's, in place of FindLocationWnd; docs/navigation.md "Find"): the zone's merchants, bankers, guildmasters, other services and the findable list (OP_SendFindableNPCs) as the modern map's Find page and a gamepad window; search, kinds, target, the find path, NPC marks on the map. Unit and UI-pack tests; not tried live.
- [x] Find: the guide path over the client's own mesh (`app/guide_path.h`), found again as the player strays or the NPC moves; shared terrain support for the find path's and guide path's trail styles (`game/path_trail.h`). Unit tests; not tried live.
- [x] Find: fourteen thin rainbow threads wind and curl a few feet above the terrain through hills and valleys, follow the route's floor in multilevel areas, and omit unsupported stretches. Geometry and particle integration tests; live appearance still to check.
- [x] Find: Options selects a gold ground line (default) or rainbow strands, applied immediately and saved per character in both UI packs. Geometry, runtime switching and settings tests; live acceptance still open.
- [x] Find: Walk there (the navigator to within 10, the guide path showing; gamepad X). Unit tests; not tried live.
- [x] Find: the modern minimap marks the NPCs the map marks (the same kinds and search). UI-pack tests; not tried live.
- [x] Find: quest givers from the server script (0x1436 / 0x1437: the NPCs whose own quest file takes hand-ins or talks, made once per zone process and kept; `quest_npcs`). Unit tests and the script's test harness; not tried on a live server.
- [ ] Find: the gamepad minimap's marks; quest givers measured on a live server (the zone log's line) and the figure put in server/README.md.
- [ ] A key binding of the client's own (the key window holds only the original's commands): walk to the target; `/nav target` on a hot button meanwhile.
- [ ] Tactical queries for bots (a kite point; `/stick`'s pin, `!front`, `moveback`, `uw`, and following a changed target without `hold`).
- [ ] Auto Combat (a script with its own window, an add-on in a repository of its own): Manual (full and partial), Hunter (roam and camp) and Assist modes, Spells and Abilities pages of rows with conditions. Its client side is done: `Me.AbilityReady` / `CombatAbilityReady` / `AltAbilityReady` and timers, `Target.Buff` and the rest from OP_TargetBuffs, `/stick`, line of sight, `hailstone.server_rule`, `hailstone.hold_autoskill` and `autoskill_list`.
- [ ] Auto AA (an add-on of its own, projecthailstone/hailstone-autoaaspend): its client side is done (`hailstone.alt_abilities`, `hailstone.train_aa`, the Modern pack's `lib/aa.lua` with a part on each row, in the details and under the header). Open: the Gamepad pack's AA window as a library a script can extend the same way; a purchase checked live against EQEmu.
- [ ] Waypoints and an in-client mesh editor (areas and links in a shareable per-zone file).
- [x] Swimming: water surfaces in the mesh, the look pitch steered while swimming (breath, diving, climbing out).
- [x] Zone lines marked exactly into the mesh; a walk stops before stepping into one (seen live at Oasis / North Ro).
- [ ] Levitation paths. Swimming live end to end checked (stock PEQ test server, 2026-11-18): a long swim in Qeynos harbour, diving and climbing out; the jump key under water needs a real keyboard's key repeat to judge.

## Item comparison (docs/item-compare.md)
- [x] The item against what the player wears, on a copy of the sheet through the stat rules (caps, recommended level, the shown AC and ATK); the slot choice (pairs, two-handers, the secondary); effects gained and lost; why it cannot be used; the class-weighted verdict; `hs.CompareItem` / `CompareSlotItem` / `CompareBagSlotItem`; both packs' tooltips, panels and item displays; Options (unit, bridge and pack tests).
- [ ] Live: inventory, bags, merchant, loot, a linked item, a two-hander, a ring pair; the gamepad pack.
- [ ] The Advanced Inventory's bank rows; focus effects by strength; the weights tuned with players.

## Isometric camera (docs/isometric.md)
- [x] The camera (type 7): pitch, field of view, free turn, zoom, easing; fog, far plane, region and shadow cascades measured from the player; `/camera`.
- [x] WASD relative to the camera (or classic: `camera.iso_wasd`), the turn keys turning the camera, the gamepad's sticks.
- [x] Click to move and to interact, on the navigator: the ground (held, following the cursor), doors, ground items, ladders; a right click walking to an NPC or a corpse. Never an attack.
- [ ] Live: clicks across a zone, a merchant and a corpse walked to, a door on the way, the held button; the gamepad.
- [x] A mark where a click's walk ends.
- [x] Seeing the player through what is in front: a dithered cut round the player of what is nearer and above their feet (zone, objects, grass), clicks through it.
- [x] The circle only while something is in the way; the roof cut under cover; the silhouette; picks through the cut; a ring under the cursor.
- [x] Options (modern Display > Isometric Camera, the gamepad pack's Camera page; `iso_*` through `hs.Option`), the pad action `camera_iso`, `/camera` toggles. No key command of its own (the key table is the original's): `/camera toggle` on a hot button.
- [ ] Whole objects fading as one; tuning the cut, the roof and the silhouette against real zones.
- [ ] Live: the roof in buildings, caves and stacked dungeons; the silhouette; the hover ring; the options in both packs.

## 4. Check live (implemented, not confirmed)

- [x] Shift-click / Ctrl-click quantity flags (checked live).
- [x] Modern quantity dialog: opened again for another request while its box has the keyboard, the box shows the new amount (headless ImGui test).
- [x] Equip slot availability (the profile's slot mask) and auto-inventory's use check, ported.
- [x] Guilds tried live with a GM-made guild (2026-10-29): invites across zones, chat, MOTD, ranks and permissions, notes, alts, leader change, the guild bank; the join, leave and rank lines and the nameplate's show-guild flag fixed. Open: a second member's view of the guild bank; /guildwar and /guildpeace are only logged by EQEmu.
- [x] The guild window's Tribute page (each member's tribute and trophy states, donations, last donation; the opt-out buttons). Checked live on the NMS dev server (2026-10-31) with a GM-made guild of two: the leader's OptOut for the member (`guild_tribute_opt_out`) came back as OP_GuildOptInOut to both and updated the member's row (tribute on to off, `guild_members.tribute_enable` 0), and the Tribute Benefit window's button (`guild_tribute_opt`) opted the member in, out and in again (playprobe `--tribute-opt-out`, `--tribute-cycle`).
- [x] Guild banners in both UI packs: server kit and status replies, filtered refresh, placement, removal and
  design edits with the modification item. Planted actors use the received patterns and colours.
- [ ] Guild banners: verify editing, planting, removal, benefits and Guild Hall portal travel on a banner-capable
  server (docs/unknowns.md).
- [x] A raid of 18 and the raid MOTD button: checked live on the NMS dev server (2026-10-31): 17 headless characters invited by `/raidinvite`, every client's list reached 18, and the leader's MOTD reached all 17 members ("Raid MOTD: ...") (playprobe `--raid-size`, `--raid-motd`, `--raid-accept`). Open: zoning in, the leader printed "<name> joined the raid." for each of the 17 members (docs/unknowns.md).
- [x] Gamepad pack: after B closes a Tip window opened over the merchant, the pad dropped to game mode and the merchant window no longer took it: the merchant and the Tip asked for the pad in the same frame and only the last request was kept. Every request is kept now, and the window under a closed one takes the focus back; checked live on the NMS dev server (2026-10-31): after the Tip, RB reached the merchant's Parcels page.
- [x] Melee sounds (the cues fire with the right waves; not listened to).
- [x] Augment appearance-slot gating: checked live on the NMS dev server (2026-10-31): with game feature 0x15 on (the client's setting for EQEmu accounts), a Tricorne Hat Ornament (aug type 21) into a Cloth Cap's type-21 socket was not refused (no 5485), went out as OP_AugmentItem and EQEmu stored it (`inventory.augment_six`); the client showed it in socket 6. The refusal with the feature off is unit-tested only (docs/unknowns.md).
- [ ] A server's class names (`set_class_name`, docs/plugins.md "Class names") in a live session: a plugin that names classes on its server's hello, then every window that names a class (both UI packs), `/who`, camping to character select and back, and logging in to another server with the plugin still loaded (the names must go). Unit, plugin and script tests only. Open: the stock-layout windows' own class labels (the XML's "WAR" check box labels in the group search and looking-for-guild windows) keep the stock names.
- [ ] OP_Weblink from a quest's `SendWebLink`, `/url` from a script, and the browser window (`/web`), in a live session (unit and window tests only).

- [ ] Idle appearances freed during a zone (render/look_eviction.h, five minutes unworn): watch a busy hub for an hour (the log's "freed N idle appearances", the GPU memory), and a spawn coming back after its look went (dressed again on the worker). The rule is unit-tested; the renderer side was only built.
- [ ] Free the texture slots of freed appearances too: a freed look keeps its textures (slots are shared between looks), so unique armour and illusion textures still build up over a long stay in a hub. Reference-count each slot by the GpuModels that read it and free one no live model reads, after kFramesInFlight.
- [ ] A GPU without a sampleable D32_SFLOAT depth attachment (the D32_SFLOAT_S8_UINT / D24_UNORM_S8_UINT fallback, now with `separateDepthStencilLayouts`) under the validation layers; and ObjectRenderer's instance flush on a driver with non-coherent host memory. Neither was run on such hardware.

- [ ] The 60 Hz game clock in a live session: /follow and camera smoothness at each rate, strafing feel. Checked live (stock PEQ test server, 2026-11-18): jumps, falls and fall damage the same at 30, 60, 144 and uncapped, turning, a stalled zone load.
- [ ] The cameras 1..5 (docs/mechanics.md "Cameras") in a live session: each one's place, the sweep against walls and doors, mouse look and the camera keys in each, riding and sitting (the tether's bone 0). Each one's place, the wall sweep and sitting checked live (stock PEQ test server, 2026-11-18); mouse look, doors and riding still open.
- [x] Knockout selects overhead and restores the saved camera on recovery; drunkenness at 70 and above selects first person and widens the FOV (regression coverage). Open: live acceptance, missile camera 8 and camera restore after zoning.

## Code health

- [x] Shared asset cache (`assets.shared_memory`, docs/layout.md "Shared archive memory"): server-overlay archives and inflated character WLDs mapped from a content-addressed cache shared by clients, with animation keys read from the mapping.
- [ ] Shared asset cache: live Windows and macOS sessions, sustained play with zoning and more than two clients; whether static collision or geometry are worth sharing too (docs/unknowns.md "Shared archive memory").
- [ ] Shared GPU textures (docs/graphics.md "Shared GPU textures", docs/research/shared-gpu-memory.md): a live Windows test of the failure paths (broker killed, a client killed while it holds the lease, a lease not free in time, a hung frame), checking that every client plays on with private textures; frame-time measurements beside the memory ones; sustained play with zoning; Intel hardware. Until then it stays a build-tree experiment with no client setting.
- [ ] Shared GPU textures on Linux (a Unix socket passing opaque FDs) and more than 16 clients (docs/research/shared-gpu-memory.md).

- [ ] The layer order (`app` -> `game` / ... -> `assets` / `net` / `web` -> `core` / `platform`, docs/plan.md and AGENTS.md) does not hold for `net` and `assets`: they include `game/` headers. In `net`: every `game_connection*.cpp` (GameConnection keeps the game state), and the packet codecs `zone_packets.cpp` (`game/mechanics/remote_spawns.h`: the bit-packed positions), `spell_packets.h` (`game::mechanics::cast::Packet`), `task_packets.h` (`game::Task`), `training_packets.h` (`game::kTrainSkills`, `kTrainLanguages`), `aa_packets.h` (`game/alt_adv.h`) and `ability_packets.h` (`game/mechanics/abilities.h`). In `assets`: `wld_zone.cpp` (`game/zone_names.h`), `model_points.cpp` (`game/sine_table.h`), `eqg_character.cpp` (`game/mechanics/anim_select.h`, `game/race_models.h`), `character.cpp`, `character_parts.cpp` and `character_dress.cpp` (`game/appearance*.h`, `game/actor_look.h`, `game/race_models.h`). Everything builds into the one `hailstone_engine` library, so nothing fails to link today, but the documented order is not what the code does, and no check enforces it. Either move the shared pieces down (the position unpacking, race and appearance tables, zone names and the sine table into `core/` or the layer that reads them, and the packet structs out of `game::` types), or write the real order into docs/plan.md and AGENTS.md (GameConnection above `game`; the tables `assets` reads as a lower part of `game`). Then add a check (a CI grep like the NMS one) so it stays true.
- [ ] Far shadow cascades are redrawn every frame; reusing them needs per-cascade tracking of moving casters first.
- [ ] Session-long caches (`ModelArchiveStore` libraries' dress and model caches, `EqgCharacterStore`) never evict.
- [ ] `tlo_index` makes a finalized userdata holding a `std::string` and a `Value` for every `.Member` step; it could keep the name as a Lua string.
- [ ] Other `hs.*` functions still build `std::string` temporaries inside push expressions; move them to `plugin/lua_keep.h` as they are touched.
- [ ] The pack's consent window is guarded from its first draw only: a script drawing before it on that first frame can still add to it once. Pre-register it as guarded.
- [ ] The client's own plugin consent prompt (app/automation.cpp) has no arming delay; the packs' wait a second before Allow and Don't allow work.
- [ ] Plugin windows' memory: plugin code may use 3/4 of the UI state's limit (a reserve for the pack), not a budget per folder.
- [ ] The Lua review fixes (vendored lstrlib match check, longjmp paths in lua_host/lua_imgui/lua_mq) have not been built with MSVC under Wine.
- [ ] Windows: about 260 `path::string()` calls rely on `cmake/utf8.manifest` making UTF-8 the ANSI code page. Sweep them to `core/path_utf8.h`, or record that the manifest is required.
- [ ] Tests for /camera, /mousespeed and /copylayout through ClientApp itself (their helpers are tested), and for the gamepad pack's browser change.
- [ ] `net::server_file_destination` still accepts versioned shared libraries (`.so.N`) in scripts and plugins as data, while `tools/make_addon.py` refuses them.
- [ ] Updates: `kept-<time>/` folders left by a rollback are never cleaned up; offer to remove them.
- [ ] Conversation journal texts over 0xffe characters still split on reload (the fgets limit), as in the original.

## Not planned

Voice macros and audio triggers, the commercial-service windows (the marketplace's funding pages among them), the zone guide, zone path and tribute trophies (no data on EQEmu), Miles' reverb DSP and DLS synthesis, the client's own graphics settings.

## Memory

- [x] A zone's textures and uploads: a fully opaque power-of-two image's mip chain is built by the GPU (on an sRGB view, in linear light, as the CPU chain) instead of on the CPU, and the zone's uploads are staged to the card in pieces rather than in one lot (docs/graphics.md). The PR author's measurement, on dragonscale in the zone viewer before opaque images of odd sizes went back to the CPU chain: the peak commit fell from 1,468 MB to 1,353 MB and the peak working set from 740 MB to 625 MB; in a live session in a small classic zone (Direwind Cliffs) the peak did not move. Not measured again since.
- [ ] The character archives: the 135 global character archives cost about 926 MB, the largest single term in the client's memory (the client's own log: `zone load: start: 345 MB`, then `character archives: 135 global (with Luclin models) in 1439 ms`, then `zone load: start: 1271 MB`). Archives in the game folder are now mapped read-only instead of held as a heap copy (overlay archives are still read into the heap: docs/hardening.md "Mapped game archives"), so only the index is private and the bytes live in the file cache. Character WLDs keep a compact resource index and decode on first use; a decoded WLD whose last reader is done stays in a retained set of 128 MiB (least recently released out first) so that model after model does not decode it again. The archive-bytes / parsed split of that 926 MB is still unmeasured. Next: measure fresh and repeated-zone memory and decode counts with the retained set (and pick its budget from that); compare Windows private commit with resident checkpoints and model animation/geometry subtotals after shared/compacted animation keys; attribute name tables, parsed EQG source data and allocator retention before further cache policies.
- [ ] The base cost: about 492 MB before any zone is loaded (the sound library's index over the 17 `snd*.pfs` at 119 MB, the spell particle definitions, the sky colour maps, the hero's forge tags, the fonts) and about 200 MB of shadow maps at the defaults (six 2048² cascades and the point-light cubes). Which of these is how much has not been measured one by one; the same goes for the transient above the end-of-zone figure.
- [ ] An allocator of the client's own (mimalloc or jemalloc): the client uses the system allocator throughout, so the memory it asks for is not the memory it holds.

### Server add-on host services

- [x] Append bounded movement profiles, map/Find eligibility, merchant click context, received raid/actor/item
  readers and transient world geometry services to the SDK, preserving stock fallback.
- [ ] Complete live acceptance for enhanced follow/mounts, controller input, private sales, raid actor selection,
  item metadata/loot prompts, instance labels and world overlay presentation; see unknowns.md.
