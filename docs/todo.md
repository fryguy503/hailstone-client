# To do

What is left before the client is feature complete, in working order. Each gap's details are in `docs/unknowns.md`; this list only orders them.

Tick an item when it is done and tested, and remove its row from `docs/unknowns.md` at the same time. Add new gaps to both.

Written 2026-10-28 from `docs/unknowns.md` and `docs/plan.md`.

## Road to 1.0

The steps are in `docs/plan.md` "Road to 1.0". This section maps items to steps; most are written out in the sections below. Tick them in both places.

### 0.8, freeze the surfaces

- [x] Lua 5.4.7 to 5.5.1: packs and add-on repositories fixed, the break in the changelog (8b43300, in 0.7.2-beta).
- [ ] **Review `hs.*` across both packs and the `mq` module.**
  - Rename or remove what is not wanted for good.
  - Then write the stable set down (`docs/ui-api.md`, `docs/plugins.md`) with the rules for adding to it after 1.0.
- [ ] **Plugin ABI: declare `HAILSTONE_PLUGIN_ABI` stable.**
  - Make the last breaking change, if any, first.
  - Write down how it grows after 1.0: new functions at the end of the tables, no changed layouts.
- [ ] **Remove the `nmsc` compatibility layer.**
  - `sdk/nmsc_*.h` and the `nmsc_*` exports.
  - The `NMSC_*` environment variables (`core/env.h`) and CMake settings.
  - The saved window names renamed at start (`core/old_ui_settings`).
  - The rule about them in AGENTS.md.
- [ ] **Remove the older builds' fallbacks.**
  - Chat filters and colours read from the asset folder's `eqclient.ini` under `chat.ini`.
  - Unsorted character folders adopted per world (docs/layout.md).
- [ ] **Add-on safety** ("Add-ons" and "Code health").
  - Wrap the plugin functions and tables passed to pack functions.
  - The arming delay on the client's own consent prompt.
  - The pack's consent window guarded from before its first draw.
  - `server_file_destination` refusing `.so.N` as `make_addon.py` does.
- [ ] Layer order: `net` and `assets` stop including `game/` headers ("Code health").
- [ ] Local-time calls outside `src/platform/` go through `platform::local_time` ("UI").

### 0.9, platforms and releases

- [ ] **Windows** ("Updates", "Add-ons", "Code health").
  - The updater run (WinHTTP, renaming the running `hailstone.exe`).
  - Add-ons run.
  - The Lua review fixes built with MSVC under Wine.
  - The `path::string()` sweep, or the manifest recorded as required.
- [ ] A scripted release (packages, manifest, signature, upload), local or in CI ("Updates").
- [ ] Resumable downloads; the rollback's `kept-<time>/` folders cleaned up.
- [ ] Add-ons against the real GitHub (XeSS and FSR 4 are in the add-on index already).
- macOS is on hold until someone can test and maintain its releases; it stays a test build and does not block 1.0.

### 0.10, live play on stock EQEmu

- [ ] **Write the checklist.**
  - Its own doc, played on the stock PEQ test server, both packs, a physical controller.
  - Built from the live items of "Combat and spells", "Movement", "Items and commerce", "Social and friends", "Navigation", "Item comparison", "Isometric camera" and "Check live".
- [ ] Play it and fix what it finds.
- [ ] Shadow step (SPA 379): the zone collision `land_spell` needs ("Combat and spells").
- [ ] The collision radius, too wide for doorways ("Movement").
- [ ] Leaving an adventure clears the window ("Adventures").
- [ ] UCS reconnection and a wrong chat key ("Items and commerce").

### 0.11, real estate

- [ ] Placement: the ghost's wall and surface fit, free mode's mouse moves, the flatness test.
- [ ] **The rest of the housing windows.**
  - Item-move pre-checks and button greying.
  - Alternate-currency deposits.
  - NPC-item binding.
  - The item menu's unbuilt entries.
  - Neighborhood polls and vote window.
- [ ] Played live on a server with housing; the real estate rows of `docs/unknowns.md` settled.

### 0.12, memory and soak

- [ ] Character archives and the base cost ("Memory").
- [ ] Session-long caches evicting ("Code health"); freed appearances' texture slots are freed already ("Check live").
- [ ] An hour in a busy hub with zoning, and several clients on the shared asset cache.
- [ ] Golden-image tests for a few zones (docs/plan.md section 5).

### 1.0 RC

- [ ] Bug fixes only; a known-issues page from what is left in `docs/unknowns.md`; the wiki and changelog.

### After 1.0

Not blocking 1.0:

- `/gems`.
- `/travel`, waypoints and the mesh editor.
- Levitation paths.
- Tactical queries for bots.
- Frame generation's pacing and 3x/4x.
- FSR's water mask.
- Zone flora rules.
- An allocator of the client's own.
- Shared GPU textures on Linux and past 16 clients.
- The MacroQuest alert lists and `/target next`.
- The "trace the original" rows that change nothing a player sees.

Server-dependent systems stock EQEmu never sends keep their live checks open past 1.0: shrouds and missions, LDoN, fellowships, guild banners, achievements, reward selection, evolving items.

## Spirit Shrouds and Monster Missions

- [x] Server-supplied form catalogs, stats and detail requests, character and observer form updates, shared-task template rows, shroud progress and Shroud Bank item recovery in both UI packs.
- [ ] Live acceptance on a RoF2 server with shroud forms and missions configured: zoning or relogging in a form, mission completion, withdrawing stored possessions.

## Server add-on policy

- [x] **Optional required/allow/block lists by GitHub repository.**
  - Named as owner/repo; bare names refused. Covers every kind the add-on manager installs.
  - Intact package identities, required autostart the player cannot stop, consent gates, active upscaler checks.
  - Off unless `[addons] enforce = on`.
- [x] Server add-ons warning with Retry for a required plugin or script that failed in the world (Modern, Gamepad, the client's own window; closable, no emergency stop). From fryguy503's PR #93.
- [ ] **Letting a required add-on run above `player_caps`** (asked for in PR #93).
  - Needs a load-time check that its files are the ones a signed manifest named.
  - The client keeps neither the signed manifest nor per-file signed hashes, so naming a repository grants no capabilities for now.
- [x] Minimum versions on required add-ons (`required = owner/repo:1.0.0-preview.4`, SemVer order, highest minimum wins, malformed = invalid policy). From fryguy503's PR #93.
- [x] **Decided at the zone-in by its own rules.**
  - The cached copy only informs the login screen.
  - Before the world shows, a problem refuses entry; once in, a failure warns once in chat and never disconnects.
  - Readiness and approvals cached by rules, approvals and running set; package hashes remembered by size and time.
- [x] Native plugin permission requests kept above the loading screen, tested in Modern and Gamepad.
- [ ] **Live acceptance.**
  - A first visit and a return visit; a policy fixed by the server.
  - Missing/refused/failed requirements at the zone-in and mid-session.
  - Both UI packs and controller input.
  - An upscaler allowed/blocked/updated, then restart.

## Launcher login

- [x] **One-use token authentication.**
  - Explicit tokens, native-style `patchme /ticket:`, environment/argument transports, endpoint selection.
  - Optional world/character entry and memory-only returned-token session sign-in.
- [ ] **Live launcher/server acceptance.**
  - Fresh, expired, consumed and refused tickets.
  - World restrictions; character entry.
  - Return to login and obtain a fresh ticket.
  - Patcher executable selection stays separate.

## 1. Gameplay a player will hit

### Legacy environmental particles

- [x] Explicit-position legacy particle switches drawn from server door records and the player's `spells.eff`/global sprites, waterfall cascades included.
- [x] **Live waterfall rendering acceptance.** Seen live in the Stonebrunt Mountains (effect-142 switches, doors 41 and 44). Open: the level gates of the second and third slots (tested with a level-69 character).
- [ ] Connect the native day/night switch mode-2 rain-screen override and one-second shutdown tail (unknowns.md).

### Server map ownership

- [x] Optional exclusive spawn-marker ownership, stock NPC overlay/Find suppression, shared UI metadata and native Find lifecycle guards.
- [ ] Live acceptance across tracking-class changes, GM state, zoning, policy retirement and real server Find replies in both UI packs.

### Modular interfaces

- [x] Generic owner-scoped method hooks with explicit exposure policies and protected callbacks.

- [x] Generic owner-scoped world nameplate canvas, bounded drawing and measurements, whole-caption claims, guarded physical clicks, automatic Lua addon activation and color widgets.
- [x] Native SDK registration for the shared world-caption canvas (scoped measurements, drawing, observed concealment fields).
- [ ] Live acceptance of custom nameplate callbacks, scene depth, dense layouts, click/drag/focus handling, reloads and zoning.

- [x] Shared versioned UI data/action API, module discovery, preserved guarded closures and a central method/capability catalog; Modern and Gamepad call the same contract.
- [x] Declared UI base chains, window/helper overrides, per-interface settings, sandboxed inherited resources and a starter interface.
- [x] **Managed interface addons register in the native selector without script startup.** Options shows selected and loaded interface metadata, fallback state and loading diagnostics.
- [ ] **Live acceptance of both migrated interfaces and a derived interface**, with zoning, switching, input focus and controller workflows.

### Adventures (LDoN)
- [x] **Alt+V and recruiter request/offer windows in both UI packs.**
  - Entry/completion timers and progress, profile point balances.
  - Seven-column theme stats and native leaderboard categories, risk choices and tied ranks.
- [ ] **Complete an adventure live.**
  - Request, accept/decline, zone in, objective updates, finish, leave and cross-zone refresh.
  - Confirm stats, leaderboard standings and physical controller operation.
  - Remaining protocol ambiguity is in unknowns.md.
- [ ] **Leaving an adventure.**
  - Stock EQEmu sends nothing after a granted leave, so the window stays on the adventure until zoning.
  - Settle what RoF2 shows (unknowns.md) and clear the window to match.

### Fellowships

- [x] **Native fellowship membership and actions.**
  - Invitations, leader actions, MOTD, personal/leader XP toggles, vitality, camp requests, `/fsay`, window key and MQ fellowship search.
  - Both UI packs take up to 64 server-supplied members.
- [ ] **Live acceptance on a fellowship server.**
  - Cross-zone invitations/roster updates, XP sharing, camp creation/destruction.
  - Stock EQEmu sends no fellowship packets; EQEmu PR #5134 (unmerged) serializes twelve slots, so larger rosters need matching server support.
- [x] **Native vitality gauges and inventory bars in both packs.**
  - Campsite categories from the player's resources; DBString type-0 kit descriptions, gamepad leader markers, a server-expiry countdown.
  - Category masks and description lookup checked against the original handlers.

### Social and friends

- [x] **`/who friends` sends OP_FriendsWho** with RoF2's zone/all scope and saved names, and shows the server's normal who results. Native serialization checked.
- [ ] **Compare `/who friends` and `/who all friends` live with RoF2.**
  - Seen live 2026-11-18: both list an online friend in another zone and nothing once it camps; no logon/logoff notice.
  - Open: anonymous and roleplaying friends, a comparison with RoF2.

### Combat and spells (do first)
- [ ] **Target and pet window buff slot limit.** Check the original's against the 97 the client keeps (`net::kMaxSpawnBuffSlots`, docs/unknowns.md).
- [x] **Cure counters over multiple effect slots of one buff.**
  - A cure spends against that buff's whole local counter pool and caps it at zero, leaving removal to the server's fade.
  - Surplus is not carried to later buffs, as in RoF2. Tests: `tests/cure_counters_test.cpp`.
- [ ] **Counters at zone-in.**
  - `OP_BuffCreate` all-buffs lists carry no `slot_data`, so curse counters are lost on zone-in.
  - `ProfileBuff.counters` is decoded but not mapped to `BuffSlot.slot_data` (docs/unknowns.md "Combat, abilities and spells").
- [ ] **Shadow step (SPA 379) does nothing live.**
  - `land_spell` has no zone collision: point_in_world, collide_sphere, collide_line, floor_height and is_valid_teleport are stubbed in src/game/hit_landing.cpp.
  - The result has no position for the connection to apply.
  - Needs collision in HitLandingContext, a port of IsValidTeleport and a teleport result applied with a position update.
- [x] **Spell Hit Modes: Normal, Abbreviated and Number Only** for direct damage, DoTs, heals and HoTs; full log lines and incoming-chat hooks kept (docs/mechanics.md "Spell hit modes"). Seen live 2026-11-18 with direct damage and heals; DoTs and HoTs not tried.
- [x] **Personal and guild tribute.**
  - Received bonus items feed local stats/effects; they retire on deactivation, selection changes, opt-out and context changes.
  - Guild benefit controls enforce received permissions; guild platinum donations wait for matching replies.
  - The full tribute workflow not checked live.
  - Donate is not refused for an item worth no favour or a bag that holds anything, as the original's
    CheckItemDonatable @0x785de0 and UpdateDonateButton @0x785f40 do (eqstr 3382, 3462).
- [x] **Raid leadership controls and leadership hotkey creation in both UI packs.**
  - Correct trainable ability identities, delegate/mark sync, lifecycle cleanup.
  - Active leadership bonuses in local stats; raid invitations from expedition/shared-task rosters.
- [x] **Leadership window colours in both packs:** native green for a supplied leader rank, gold for personal purchases, white/grey training availability.
- [ ] **Raid leadership live acceptance.**
  - Member context menus with a mouse and controller.
  - Assign/remove multiple assists and markers; mark/clear NPCs.
  - Zone/rejoin; leadership hotkeys.
  - Accept expedition/shared-task roster invitations with real players and a controller.
- [x] Extended chat channels 57..107 and user colours 355..408 for servers that send a later client's chat colours (a Hailstone extension; stock routing and colours unchanged; docs/mechanics.md "Extended chat routing").
- [x] Missiles: a despawned source or target handled as DeleteMyMissiles; flying through walls with a target is the original's behaviour (60b1934). Not tested live (no archer).
- [x] **Valid spell missiles.**
  - Spell lifetime, launch and ending stages, stationary area contact, source physics clock, spell 859 local presentation. Not checked live.
  - Invalid or absent spell ids still end immediately.
- [x] Environmental hits 0xf3 to 0xfd: animations, chat lines, sounds; our own fall, drowning and lava damage reported like ApplyDamage (eb42f20; falls tested live).
- [x] ReportSuccessfulHit's shared tail: the spell book closes and a sitting or mounted player stands when damage lands; a spell heal from OP_Damage (ff39361; the stand checked live).
- [x] **Blood spurts on supported WLD actors:** landed local sharp-weapon hits, exact authored blood attachment, the player's single-frame blood sprite. Missing attachments or unsupported sprite data omit the effect.
- [x] **Spell particles: the impact stage's caster is the packet's source;** nothing when the source is unknown, as DoSpellEffect. The actor's opacity and the point model's PESFactor on actor emitters. Checked: EQPlayer::TriggerSpellEffect @0x593eb0.
- [ ] **Spell particles: two open questions** (unknowns.md).
  - Whether an actor's world matrix rows carry its scale (normalised here).
  - What makes a floor-flag actor emitter again as the actor moves (kept on the actor here).
- [x] Linked spell timers and discipline timers from the player profile.
- [x] Combat Abilities: active discipline duration gauge 26 and seconds display in both UIs, icon cancellation (live acceptance open).
- [x] **Combat Abilities: red reuse overlays and seconds in both UIs.** Shared timer groups stay marked even when the wait exceeds an ability's own recast, discipline hot buttons included. Live acceptance open.
- [x] Item recast on cast items (789134d).
- [x] The distance modifier of effect values, and the extra long-buff slots from level 72 and 75.
- [x] Buff tick timers held by SPA 0x1b4 and suspendable spells.
- [x] **Buff countdowns held in zones that suspend buffs** (OP_NewZone +876, 0xddfc5c). Zone flag and SPA 0x1b4 exception order checked at @0x4dcf20. Seen live in poknowledge.
- [ ] `get_pet_buff_data_bulk`: hold a suspendable pet buff in a zone that suspends buffs, as the pet windows do (unknowns.md; the method's author).
- [x] **DoPassageOfTime's 6 s tick runs whole in the client, plus its 1 s endurance upkeep:** ProcessAffects' buff HP, mana and endurance, local expiry at 0, the mesmerize break; HP, endurance and mana regen. Not tried live.
- [x] **Passage-of-time extras:** owner status/bleeding, live running endurance penalty, flag-1 cast interruption, drunkenness decay/sway/camera, Eye summon control. Live acceptance open.
- [x] Combat line filters 13 to 16.
- [x] Temporary-pet combat lines keep the spawn flag and use native focused-pet or temporary-pet-plus-owner classification.
- [ ] **Check owned-pet line filters and colour live** with a permanent pet and several temporary pets, on a server that supplies the temporary flag and owner. Stock EQEmu's RoF2 swarm records omit it (unknowns.md).
- [x] Hand-to-hand delay and fist damage from the profile (EQEmu sends 0); the player's own BodyWet in the attack checks.
- [x] `AltAttack` and the attack lock match the original; the rank swap's limit and GM check ported (never swaps on EQEmu's gold membership).
- [x] Ranged attacks do not turn the player in the original either.
- [x] Held buffs seen live (a SPA 436 buff freezes Courage's countdown). Melee checked by hand.
- [ ] Distance-scaled spell effects live (needs a spell with a distance modifier cast at range). Buff caster ids are in unknowns.md.
- [x] Commands: `/discipline`, `/doability`, `/stopdisc`, `/memspellset`, `/spellscribe`, `/targetgroupbuff` / `/tgb`, `/autofire`, `/melody`, `/dismount`, `/aggrometer`, `/buyer`, `/claim` (f3c5c5d).
- [ ] `/gems`: the Gems mini-game (CGemsGameWnd, 0x6705f0 to 0x6734e0), a port of its own.
- [x] **`/discipline` after `/stopdisc`** (358bf04, checked live).
  - A second `/discipline` right after `/stopdisc` is not refused by the client (NMS disc timer map or a missing server timer packet).
  - The reuse list is empty after zone-in until the server sends timers.

- [x] **Struck down by the gods (OP_GMKill):** the named player's client sets its HP to -10000 after the line and the wave, as the original's case at 0x4c8a23 does (docs/mechanics.md). Unit-tested; not seen live.
- [ ] OP_GMKill on a player already hovering dead: the original clears the RespawnTimer, which stops the respawn window's auto-select and lets transactions through; Hailstone's hover state has no separate RespawnTimer (docs/mechanics.md).

### Movement
- [x] **Server flymodes.** The local player takes the spawn record's flymode and `SpawnAppearance` 19 (all six modes) and keeps them until the effect cache is rebuilt, when `UpdateLevitationState`'s rules apply again (docs/mechanics.md "fly mode"). Seen live 2026-11-18.
- [x] **The game rules on a 60 Hz game clock** (`game.fixed_step`, docs/mechanics.md "Frame rate"): movement, other spawns, missiles, weather and fog steps, the frame-gated timers; the camera drawn between steps.
- [ ] **Collision radius too wide for doorways.**
  - The static reading gives the model's bounding radius (6.2 for a human); the stand-in stays until settled.
  - Settle with a run-time read of the actor's vf44 in the real client: rof2re's radius_log, which needs a new rof2re.dll in the game folder and a user-run session.
- [ ] Other spawns: bodies never block in the original (no per-bone meshes); only bone attachments such as held weapons could, and that test is not ported.
- [x] **NPC collision** (Hailstone's own, switchable by the server's hailstone.cfg `[movement] npc_collision`): live NPCs stop the player; players, pets, mercenaries, corpses and mounts do not. game/npc_collision.h.
- [x] The jump's crouch window from the duck clip's length (453db92); the ceiling query and auto-duck match the original apart from the radius.
- [x] **New-style models' duck clip and the jump animation.** Duck clip: CRCH_BA_1, 0x406740's row 0x7a - 0x4f. The jump animation comes from the playing animation (+0x10c0, vf24), not 500 ms after a jump.
- [x] **Placed objects that play their own animation** (Ssraeshza Temple's turning rings, swaying tree branches in 75 zones).
  - One instanced model per definition, a few matrices a frame.
  - Solid where the animation has them, so the rings let a player into the water shaft and out at the top. Seen live 2026-11-18.
- [ ] **Animated zone objects, open** (docs/unknowns.md).
  - The phase against the original's clock.
  - D3DX's key compression.
  - The other spawns' probes, sight, picking and /nav not meeting the moving parts.
- [x] Lifts carry riders (checked live on the Paineel lift).
- [x] Teleport doors (a Bazaar IT78 door, the PoK books' type, checked live).
- [x] Touch-activated switches: PlayerClient::TouchingSwitch ported, traps checked live (f500930).
- [x] **Boats, as our own design** (docs/mechanics.md "Boats"): hull floors and walls, smoothed boat motion, other riders, zone-in onto a deck, climbing aboard, sailing a small boat at the helm. Checked live in Butcherblock and OOT.
- [x] **Boats with a live asset folder: not the client's job.** The classic boat models (LAUNCH etc.) are only in RoF2's per-zone `_chr` archives, which live dropped; a server that wants them supplies them in its `client_files/` (user's decision, 2026-10-29).
- [x] **Boats: decks, animation, helm and rails** (tests/boats_test.cpp).
  - Overlapping decks: the nearest, the one stood on in a tie.
  - The boat's own animation in its collision: triangles taken again every 200 ms.
  - The helm against docks and other boats: hull probes refuse the step and the boat stops; the turn alone may still swing it.
  - Rails on a moving deck, the LAUNCH skiff's flat one included.
- [ ] **Boats, check live.**
  - Seen live 2026-10-31 (qeytoqrg's row boat): zone-in onto the deck, taking the helm (OP_ControlBoat) and sailing, walking off a still boat.
  - Where a boat script puts the player (the Timorous shuttle sent the character to the Bazaar) is the server's quest script, not the client.
  - Still open: the helm's stop against a dock or another boat; boarding a moving boat from a dock; the LAUNCH skiff's rails (Butcherblock's Captains_Skiff, grids 232 / 233).
- [x] Zone points that request a teleport by index: the player is held while waiting (untested, EQEmu never sets them).
- [x] **Reachable zone lines in toxxulia, mistythicket and oldbloodfield.** A client fallback fires buried EQG zone-line boxes from the ground over them (81d4a59; 39 boxes across the EQG zones). All 7 lines checked live. oldbloodfield's ATP_3 has no zone point 30 in the server's table.
- [x] Position history (PositionTracker, OP_FloatListThing), the retried and the 30-minute saves, closing on loading screens (60069a0). Missing samples are in unknowns.md.
- [x] **The live folder's revamped bazaar, lavastorm and highpasshold load** when the server's safe point (OP_NewZone) stands on their floor (game/revamped_zone.h).
  - Covers PEQ's Bazaar and Lava Storm version 1 (expansion 9 on) and its only High Pass Hold row.
  - Version 0's safe points have no floor there, so those stay refused.
- [ ] **Arena and Nektulos with a live folder** (unknowns.md, Movement and collision). Its arena.s3d and nektulos.s3d win over the revamped .eqg, but PEQ's Arena (its only row) and Nektulos version 1 (expansion 10 on) run the revamp: their spawns stand on the live .eqg.
- [x] **IVM's animation** (race 127/600, the enchanter pet; game::animation_source). It has no named clips; GetAlternateAnimTag @0x406a60 maps its wood-elf prefix group to ELM, so it plays ELM's clips and holds its weapons as an elf does.
- [ ] IVM's click volume: whether the original's click meets its masked body mesh, a sphere or only its held items (unknowns.md, Characters and models).
- [x] WLD 0x2C meshes (docs/formats/wld.md "0x2C mesh"): read into a MeshDef, centre added, so the Fiery Avenger (IT62) shows its blade, not only its flames, and Erud's Crossing's ghost ship its hull; a 0x2C body that can draw nothing (IVM's) is dropped.
- [ ] The 0x2C loader in EQGraphicsDX9.dll: the fields read past and the polygon flags word (unknowns.md, World rendering).

### Items and commerce
- [ ] **A bag sub-slot past the bag's own BagSlots.** Settle what the original does with an item placed there (docs/unknowns.md "Items, inventory and commerce"); `game::Inventory` keeps it for now.
- [x] Evolving-item Final Result inspection: native action-4 request, ordinary read-only item reply, received progress updates and a focusable gamepad action.
- [ ] **Evolving-item live acceptance.**
  - Inspect Final Result from owned and linked items.
  - Receive progress through 100%; confirm the server's replacement level/template.
  - Use the gamepad action with a physical controller.
- [x] **Stock achievements:** bounded compressed catalog, primary state and counters, categories/search/status filters, linked details, nearby-player comparisons, reward views and refresh, both UI packs. Live use reported generally working (2026-11-12); targeted acceptance below.
- [x] **Achievement browser usability:** collapsible categories with deduplicated totals, compact rows, inline objectives and reward actions, responsive layouts in both packs. No request runs from filtering or drawing.
- [x] **Catalog reloads keep the decoding schema of in-flight comparison/link requests.** Valid retired replies drain without updating newer views; malformed replies and timeouts keep their guards.
- [x] Reward-window presentation: numbered choices with measured long-name captions, full item details and tile/name inspection, prominent claim/confirmation controls, separate maintenance actions in both packs.
- [x] **Stock XML category badges:** base/hover/selected variants, native aspect, right-aligned counts; missing art keeps the category name. Reward selection uses full-card choices, grant summaries and a fixed claim/confirmation footer in both packs.
- [ ] Achievements: live catalog refresh, earned/guild messages, local/nonlocal links, comparison and reward delivery; native completion timing, icon resource selection and legacy achievement gauges (docs/unknowns.md).
- [x] **Stock pending reward selection and task reward previews** (`0x6411` / `0x6471`) in both UI packs, explicit claims and item inspection. Live use reported generally working (2026-11-12); targeted acceptance below.
- [x] **Task offers: native-confirmed Preview Reward in both UI packs and the starting-zone description hint** (eqstr 6029, not reward amounts).
- [ ] Reward selection: live offered-task previews, claims/delivery and item inspection on a RoF2 server (docs/unknowns.md).
- [x] NPC service distance closure and dependent quantity/confirmation cleanup in both UI packs; every container closes with the bank (docs/mechanics.md "Leaving an NPC interaction").
- [x] **World tradeskill station distance closure** in recipe, Experiment and hidden modes, with out-of-reach and busy replies and world-only quantity/mold/batch cleanup. Carried tradeskill bags stay when only the station closes.
- [x] **The service windows' ranges and timers checked against the original** (docs/mechanics.md "Leaving an NPC interaction"). Open: the mercenary liaison's check cadence (docs/unknowns.md) and walking away from each service live.
- [x] **Trades with players cancel out of range:** beyond the partner's avatar height + 20, on the same 500 ms check as hand-ins; outstanding items and coin return through the server. Checked: CTradeWnd::OnProcessFrame @0x780b50.
- [ ] **Trades with players: the merchant page-handler-2 exception** (docs/mechanics.md "Leaving an NPC interaction"). Live separation checked 2026-11-18: walking or teleporting out of reach cancels, items and coin intact.
- [x] Partial-stack looting asks the quantity (EQEmu ignores it); Link All; the loot music (entry 5 has no asset).
- [x] **Loot All keeps an active queue on repeated activation.** An individual request in flight does not block it; it keeps the first NO DROP question when a later confirmation aborts the queue (docs/mechanics.md "Loot All").
- [x] Loot All: a second click while its "are you sure" question is up or queued asks nothing more.
- [ ] **Loot All: live activation with delayed replies** (only LAN latency tried). Several NO DROP items and rapid clicks checked live 2026-11-18.
- [ ] Loot All: trace native auto-close timing when an accepted NO DROP question overlaps the last automatic request (docs/unknowns.md).
- [x] Ctrl + right-click bag swapping; Ctrl+click bag contents' slot order and refusal text (game/bag_move).
- [x] The tradeskill window: favourites, request waits, combine checks, trivial colours.
- [x] **Hailstone's tradeskill window:** each component's carried count (green / red), how many can be made, Combine / Combine N / Combine All with each result put away and a stop when the inventory is full (docs/mechanics.md). Not seen live.
- [x] Trades: the item request counter in IsOkToTransact; a busy answer cancels the trade.
- [x] Bazaar purchase: EQEmu's TradeRequestFailed reads as a failure (5cf3f63; mock test only).
- [x] **Seen live (5ba3a3b and before):** partial-stack corpse loot, Link All and Loot All; a two-client trade with coin and busy refusals; Ctrl bag moves and the bag swap; tradeskill search and favourites; the bandolier's Make Hot Button and Auto Swap; the merchant's Qty and Lvl.
- [x] **The item display's icon links its item into the active chat window's input** (CItemDisplayWnd::WndNotification, IconButton), both packs; X in the gamepad one. Seen live 2026-11-18.
- [x] **Evolving-item experience transfer:** both packs pick owned source/destination items, ask the server for compatibility and projected results, show max-transfer level and confirm the approved pair. Live acceptance pending.
- [x] Link All in the gamepad loot window; the inventory doll drop in the modern pack (checked live); the bandolier's Activate swapping weapons (seen live 2026-11-18).
- [x] Bazaar: Find Trader, Hide Traders, the result row's Buy button, the stall check, the price load of a selected slot (f7401f3; Hide Traders by unit tests only).
- [x] Barter: the feature check, offline mode, Hide Buyers (f7401f3; Hide Buyers and the feature check by unit tests only).
- [x] Mail: the ignore list (kept and edited only, as in the original); the composition window's From (351d492).
- [x] **UCS authentication gates chat/mail commands and login-time events:** bounded queues, denial (an explicit refusal or the stock UCS closing the link before replying), timeout and backed-off reconnect.
- [x] **UCS: a silent chat link ends and is retried.** It ends after 60 s with nothing received; a socket closed under it ends it at once.
- [ ] UCS: check reconnection after a lost chat link, and a wrong chat key, against a live stock EQEmu server (the 60 s silence limit not yet seen live).
- [x] Mail's From seen live after a mail login: "SOE.EQ.nmsdev.Nmscmistwo" (2026-10-29).
- [x] **Bazaar: the PC and NPC target keys skip hidden traders and buyers.** The NMS bazaar's porch stalls (type 153) lie outside the zone geometry, so only the FRUGS302 rugs work as stalls (server data).
- [x] **Bazaar: a right click on a trader or buyer the server sends as an NPC opens its shop or barter window** (e.g. Project Triune's offline traders). RightClickedOnPlayer tests the trader / buyer flag after the service classes for any spawn. Not retried live.
- [x] Bandolier: Make Hot Button, Auto Swap saved, the slots' held right button (the original has no slot drag handler).
- [x] Merchant: the Qty and Lvl columns (checked live).
- [ ] **Merchant: Usable only listed too few tomes.** Reported: only the Resistant and Fearless tomes listed with Usable only ticked and no discipline learned; not reproduced. Compare the class and race masks of a shown and a missing tome with the character's on that server.
- [x] **The marketplace window (CMarketplaceWnd) in the modern and gamepad packs.**
  - The original's store packets: catalog, states, wallet, purchase and its answer, preview.
  - Category tree, find, sort, landing page and item of the week, quantity, the zip and confirmation pages; `/marketplace`, its key and the EQ button.
  - Seen live on Project Triune 2026-11-01 (523 catalog rows). Open: buying, and the rows docs/unknowns.md lists.
- [x] **Merchant: the inventory money buttons' hook on the Parcels page** (0x698290; page 5a71bbc / dd8f339). The bag item let go, the coin outlined, the quantity answer picks the money to send. Seen live 2026-10-31 (sent, stored in `character_parcels`, claimed), including the gamepad send page. playprobe `--parcel-money`, `--parcel-claim`.

### Network and session
- [x] Declining a group invite: the client handles 0x2a50; stock EQEmu maps it to nothing, so the server script relays it (`group_declines`, 5cf3f63).
- [x] **The updated server script deployed on the NMS test server** (`install.py`, `#reload quest global`). Group decline seen live 2026-10-29.
- [ ] **The server script's limits, check live on an EQEmu server.**
  - The limits: files_rescan, files_chunks_per_second and files_chunks_per_second_zone (requests over them held on a timer); eight files kept open; a file changed since its hashing; 5 group declines a sender in 10 s.
  - Tested so far against a stand-in for EQEmu's API (tests/server_script_test.cpp).
  - Check a first download of a large client_files/ with two and three players at once (each served in turn under files_chunks_per_second_zone), and with a low files_chunks_per_second.
  - Check whether 768 pieces a second a player and 1024 a zone slow honest first downloads.
- [x] **Server files: a missing piece is asked for again** at once when a later one comes, after 3 s of silence otherwise; 5 tries.
- [x] A refused duel is reported to the challenger (EQEmu's 12-byte OP_DuelDecline; checked live).
- [x] The duel partner leaving or dying keeps the original's 8-byte OP_DuelDecline: EQEmu's handler would dereference the departed partner's missing entity, and ends the duel itself.
- [x] **Strings the client sends fit stock EQEmu's fields.**
  - A tell's recipient (63) and text (2047), as SendNewText cuts them; EQEmu's DECODE strcpy's names into `char[64]` on its stack.
  - The bandolier name (31), our own name in the world and zone requests, the name-change answer, the barter lines' names. The audit's list is in the commit.
- [x] /guildstatus sends the original's 140-byte GuildStatus_Struct, which EQEmu accepts (it dropped the 128-byte one).
- [x] The trade partner leaving the zone cancels and closes the trade, as msgEQRmPlayer does.
- [x] Keep-alive packets: `00 06` after 15 s idle (14.8 s login, 44 s chat) (515e7ef; checked live; EQEmu drops an idle client at 60 s without them).
- [x] Login: the kick request after 302/114 (eqmain's 0x16 / 0x32) with a Yes/No box (5c3a3cc; EQEmu never sends 302/114, mock tests only).
- [ ] Login: the reply's two leading bytes (unused by OnPlayerLogin), pushes 0x25 to 0x30 (EQEmu sends only 0x31).
- [x] **The world and zone link-death timeout is the original's 180 s of silence** (Params 0x12, UdpConnection +0x15c), not 15 s.
  - 15 s is the keepalive delay (Params 0xe) and /offlinemode's exit window (0x3a98 @0x53ddc3).
  - Login (14.8 s) and chat (44 s) keep the client's limits (docs/unknowns.md). Checked live 2026-10-31.
- [x] **Session limits against a hostile or broken server.**
  - Early packets bounded in bytes (4 MiB) as well as count; the delivered-but-unread queue bounded (64 MiB, then the session ends).
  - An ack for a packet not yet sent ignored.
  - A too-short first fragment ends the session.
  - Datagrams failing the CRC do not keep the link alive; oversized datagrams dropped on every platform.
- [ ] **What the original checks in OP_OutOfSession** (unknowns.md), and whether its last-receive time is set before its CRC check. Until then a forged `00 1d` from the server's address ends a session.
- [ ] Whether the original checks the address a SessionResponse comes from (the client takes any with its connect code, as multi-homed EQEmu hosts need; unknowns.md).
- [ ] eqhost.txt: the port the original uses for a `Host` without one, and reading the `Host1`.. fallback entries eqmain's `LoginClient::ReadHosts` reads (unknowns.md).
- [ ] The original CRT's atol on a number out of range: wrap or saturate (`/split`, `game::cmd_atol`; unknowns.md).
- [x] What OP_MobRename's handler sends after a rename, and when (0x4c9a6b): 0x2b8e alone, for the own spawn, while the server list window is up. Hailstone has no such window and sends nothing.
- [x] The player's own rename: the character name change (0x4d5a10). The connection's character name and the profile's take the new name, so every reader follows it.
- [ ] OP_LevelAppearance's flag-on emitter takes the actor's turn (vf126 @0x1003a950; unknowns.md). It takes the world's axes.

### Real estate (player housing, guild halls)
- [x] The packets, the manager (catalog, records, access, item rows, owned lists, areas, placed objects, occupancy), result messages, upkeep time left.
- [x] Buy / sell / transfer checks, the offer dialog, escrow deposit from and withdrawal to the cursor, /realestate, /leaverealestate, the windows' keys.
- [x] The guild bank in every guild hall zone; an instanced hall only for the guild whose plot holds it.
- [x] The plot, manage, items, neighborhood and plot search windows and the item context menu, in both packs.
- [x] Placed objects drawn in the world (collision, right-click menu), placement mode with its keys, the area check and the 0x711a confirm.
- [ ] Placement: the ghost's wall and surface fit, free mode's mouse moves, the flatness test (unknowns.md).
- [ ] Play it live against a server with housing (Project Triune) and settle the rows of the real estate section in unknowns.md.
- [ ] The item-move pre-checks and button greying, alternate-currency deposits, the NPC-item binding, the item menu's unbuilt entries, the neighborhood polls and vote window (unknowns.md).

## 2. Looks and sound

- [x] Zone MSAA coverage on Radeon RX 9070 XT: alpha and LOD sample masks combined consistently in the colour and depth shaders. [Qualification limits](unknowns.md#world-rendering).
- [x] **Grass AO with MSAA on Radeon RX 9070 XT.** Zero-height lawn blades no longer send NaN. A global resolve-write to AO-read barrier (spec-redundant, an unconfirmed driver workaround) is kept. [Qualification limits](unknowns.md#world-rendering).
- [ ] **Re-test the RX 9070 XT grass AO flicker with the lawn NaN fix alone**; remove the AO memory barrier in `VkContext::begin_overlay` if it stays clear ([unknowns](unknowns.md#world-rendering)).
- [x] **Shared actor visibility, texture slots, EQG armour and carried lights:** shared actor/nameplate/pick/target visibility, idle texture-slot reclamation and capacity retry, four-weight EQG armor bindings, server-selected carried lights. Live GPU acceptance pending.
- [x] **Held-item effects and textures:** WLD clouds and separate bitmap animation frames, animated actor textures, EQG surface maps and authored additive lighting, character selection/item previews, instance cleanup.
- [ ] **Held-item effects, open** (unknowns.md).
  - Native WLD density setting and small-node suppression.
  - Animation-trigger timing.
  - Live comparison across equipment changes, zoning and supported model families.
- [ ] **Actor particle hold.** Measure how far the actors' own effects' particles reach from their point and set the 30-unit view margin from it (docs/unknowns.md).
- [ ] **Other players' carried lights after an equipment change.** Stock EQEmu sends nothing for them (docs/unknowns.md); trace how the original updates a remote player's light.
- [x] **Name plates: occlusion, draw order and fading.**
  - Same-frame per-pixel occlusion against the scene's depth: a hidden name is dropped, no ghost.
  - Nearer over farther with overlap fading: 10 %-20 % coverage to 10 % alpha, tau 0.3 s; the Fade Overlapping Names option.
  - The target always full at its projected head; a Highlight Target Name option.
- [x] **Name plate text options** (Options > Display "Name Plates"): font face, size, bold, shadow / outline / no edge, shrink with distance, guild and owner line size.
- [x] Invisibility markers on nameplates and HUD captions, with optional brackets for known invisibility versus undead.
- [x] **Animation layering:** the original's upper/lower bone groups, actions over the legs, per-group cross-fades and SyncToBoneGroup; rider actions on the arms, other spawns' water (d8c9fc2). Live split not seen in a screenshot.
- [x] Animation: a busy upper body queues (classic, +0x10c4) or drops an action as the original does; the same action never replays over itself (84046fb).
- [x] **Animation: classic P08 / O02 and the ladder climb.**
  - Classic P08 / O02 after an animation 0x4b0860 does not name (the run, jumps, socials), not after an action, which keeps P01. The follow-up's +0x10c4 = 0x47.
  - The local player's climbing animation on a ladder.
  - Seen live 2026-10-31 on another player's Luclin human (P01 after a walk stop or zoning, P08 after a run stop or `/wave`).
  - Open: the local player's own stand from the front; the ladder climb (that server has no LADDER* ground object; its OBJ_LADDER doors, xorbb door 8 and pillarsalra door 7, are switches here; docs/unknowns.md).
- [ ] **Movement collision: replay the reworked sphere sweep against the recorded vectors.**
  - The sweep: `game/sphere_sweep.cpp` (capsule candidate order, tangent and zero-length edges, parallel edge-region refusal). Only synthetic unit tests so far.
  - Run: `HAILSTONE_VECTORS_DIR=... build/hailstone_tests -tc="*movement*"` (tests/movement_vectors_test.cpp).
  - Update docs/mechanics.md's vector counts.
- [x] **Remote ladder collision state reaches animation.** New-style riders use column-5 sequences; sequence identity includes the style column. Live ladder/rider acceptance open.
- [x] **Animation speed and pre-processing.**
  - 0x4afc90's flags reach the classic movement and action rates: UseLuclinElementals for races 209..212, a TUNIC_POINT model's D05 and gender-1 animations 2 / 4.
  - 0x4b15b0's pre-processing before it: big races, race 42 / 120, race 330, a styled model's stand / social.
  - The interrupt test uses the requested number; the queue and state use the chosen one.
  - A pet's idle needs a roll above 99 (+0x38c); a classic model turns on the spot (P03); the global auto attack suppresses every actor's fidget; a missing clip and replacement is the stand path.
  - An EQG clip keeps its real key times, with the missile delay from its own period.
  - Seen live 2026-11-19.
- [ ] **Animation: 0x4b15b0's posture rules for OP_Animation actions that have a clip.** Only the stand path has them; the layers play those clips as given.
  - A sitting actor's P08 request becomes P07, and O01 / O02 become 0x49 (0x4b191a; race 0x3c and the local player in first person play nothing).
  - 0x27..0x29 play nothing while sitting.
  - An actor not standing plays nothing for P03, P08, O01, O02 or 0xc..0xf (0x4b1b97).
- [ ] **Animation fields not traced** (docs/unknowns.md "Characters and models"). At their defaults until their readers are traced: the mount fields beyond GetMount's motion, +0x1229, the spell book's slot 0x94, +0x1198, and the jump / fall fields (+0x10, +0x163, +0x180).
- [x] **Riding while illusioned** (Hailstone's own, switchable: `game.illusion_mounts`, on by default). The rider's model no longer refuses the mount; the mount's seat dag must still exist (docs/mechanics.md "Riding while illusioned"). Seen live 2026-11-18.
- [x] Mounted form changes unseat before rebuilding the actor and re-seat against the saved mount identity; departure and id-reuse covered.
- [x] **Ladders placed in the zone** (objects.wld LADDER* actors, type 6). Walking into one climbs it (CollidingType 6 from the sweep's hit); a click from the top mounts it. Seen live 2026-11-18.
- [x] **Passable ladders** (Cabilis's LADDERCB101, every polygon passable). Clickable from the top as in the original; climbed by walking or swimming into them by Hailstone's own rule, switched by `hailstone.cfg` `[movement] passable_ladders`. Seen live 2026-11-18.
- [x] **Solid ladders** (Hailstone's own, switched by `hailstone.cfg` `[movement] solid_ladders`). The movement meets a ladder as one slab, so every race climbs the rung ladders small races fell through. Seen live 2026-11-18.
- [x] Complete five-word armor records across spawn/wear/save/preview paths, Hero's Forge category and membership filters, Drakkin illusion feature updates.
- [x] **Modern Hero's Forge aliases resolved in an armor-only cache**, carrying their native surface maps and layer parameters into modern actor lighting.
- [ ] **Compare modern Hero's Forge fits against the original client** across races/genders, animations, previews and illusion/equipment transitions.
- [x] Luclin eye colours and `SwapFace` (e7a5849). Open: one-texture Luclin materials' tint (female dwarf faces 2 and 3).
- [x] **Elementals: UseLuclinElementals** (default on). On, race 75 is drawn as the typed Luclin elementals 209–212 by texture; off draws those as race 75 (CreatePlayerActor's swap). Not seen live.
- [x] **UseLuclinElementals off skips the implicit Global5_chr load** ("Global NPC Models", @0x492780). Hailstone loads neither Global5_chr2 nor frog_mount_chr implicitly.
- [ ] UseLuclinElementals off: whether the original also skips those archives when `GlobalLoad.txt` names them (docs/unknowns.md); they still load here.
- [ ] **Hero's forge pieces without bones on Luclin models.** Bracers' UVs land on the mail fill; gloves beside the hands. Needs the DLL's rigid attach traced (ChangeBoneAttachment @0x10042c60).
- [x] Head items: the head's armour colour on helms (110650f); "keep head" has no effect in the original; the Velious helms beyond the table don't exist in the RoF2 files.
Fog, weather, sky, EQG surfaces, lighting and spell particles are a look-and-feel pass, not ports (user, 2026-10-29: "make it look good"). Branches: `look-atmosphere` and `look-surfaces`.

- [x] Fog and haze: eased between places, the sky's colour, a storm band, lightning (e069ae2). The original's area fog has no data in either install.
- [x] Weather transitions over tens of seconds (e069ae2).
- [x] **Sky: a tilted sun path, ozone, ambient from the sky model, a colour grade** (09624d4). Rough: dense forest shade, red dusk haze in lopingplains/commons, everfrost flat, befallen blue.
- [x] **Volumetric clouds over the atmosphere** (`display.volumetric_clouds`). Marched at half size, a quarter a frame; the weather's storm closes them over. Seen live 2026-11-18.
  - Open: cloud shadows on the ground, god rays through the clouds, the cost on an integrated GPU.
- [x] **EQG surfaces** (look-surfaces): water reflects the drawn sky and noswap environment maps; modern lava (crust, LavaH walls), blood/coloured falls, foam only on rising floors; animated texture sidecars; the one unpainted heightmap tile (scorchedwoods).
  - Open: heightmap lighting colour, glow maps in the classic look, planar reflections, TAA.
- [x] **Upscaling: a render scale** (`display.render_scale`, 50 .. 100 %) with AMD FSR 1 (EASU + RCAS, `display.sharpness`). Not seen in game or with /viewport.
- [x] **Hailstone Texture Studio: HD texture packs built from the player's files on their GPU** (ncnn Vulkan, the PBRify models).
  - A tool add-on in its own repository (projecthailstone/hailstone-texstudio), built on `src/texpack`; the add-on manager's tool kind (docs/addons.md "Tools") installs and starts it.
  - Options and the login screen's More options pick the pack (from the next start); Options starts the studio.
  - Open: an install from the real GitHub on Windows; measuring an integrated GPU (Intel Iris Xe), NVIDIA and Windows' drivers; the 17 live textures the decoder cannot read; a pack's settings changing without a new pack; a pack applied without a restart.
- [x] **Photo materials in built packs** (docs/graphics.md "Photo materials"). A classic zone's ground tiles rebuilt from Poly Haven's CC0 photo textures by the studio's rules, keeping the original's layout and colour; a zone file may set a material's colour in its art. The 34 classic outdoor zones built.
  - Open: the expansions' zones; the photos' repeat on large flat areas; a material tile preview in the studio; the studio's size estimate ignores the material tiles.
- [x] **Model overrides** (docs/graphics.md "Model overrides"). Binary glTF models in a texture pack drawn in place of a zone's objects; collision stays on the game's mesh.
  - Open: doors and other moving objects (the object renderer); a scale or offset for hand-made models.
- [x] **Generated trees** (docs/graphics.md "Generated trees").
  - A classic zone's trees grown again as models fitted to the game's trunks and leaves, with photo bark, from the studio's rules. The 34 classic outdoor zones built (129 trees, dead trees and palms).
  - Near, middle and far levels (far as picture cards), about a tenth of the triangles; shadows draw coarser levels. Each kind stored once and drawn instanced; levels dither into each other; Options > Tree Detail Distance.
  - Pines thinned; the middle level keeps every card. Needles keep close to their spray's mean shade, to stop wind flicker and walking shimmer.
  - Open: the expansions' zones; palms' fronds hang less low than the game's; cut-out leaves thin out in smaller mips; EQG zones' trees; Greater Faydark draws about 1 ms slower than with the game's trees; the flicker not reproduced with a still camera; needles that still shimmer a little more than the game's trees, and broadleaf leaves.
- [x] **Horizon hills** (docs/graphics.md "Horizon hills"). Land past an outdoor zone's edges, built from the zone's own terrain and drawn with its ground, rock and water materials: rising into hills and mountains, the sea past a shore. Not yet in a session.
  - Trees on its woods: the zone's own, drawn far as pictures on crossed cards.
  - The land as generated terrain: warped hills worn by water drops and slumping, held off the zone's ground and the sea, cached per zone; coasts following the ground; rock, ground and sand splatted in one draw, the rock triplanar with its repeat broken up.
  - Open: no snow; the wear's gullies are finer than the 64-unit grid shows; one ground and one rock texture (the zone's most used); an EQG zone's terrain-tile materials unused (no land when the zone has no plain one); the lid over a pocket the zone closes round is flat.
- [x] **Zone edge fog** (docs/graphics.md "Zone edge fog"). A fog bank on the ground past an outdoor zone's terrain, marched per pixel through a field from the land's, with a soft swelling top and drifting patches. Not yet in a session.
  - Open: its colour is the haze's alone (no light on its top, no cloud shadow); it marches to sample 0's depth of an MSAA scene, so an edge against the fog keeps that sample's depth; tuned by eye, not against a reference.
- [x] **Neighbour zones, the world atlas and the distant copies removed.** They showed walls, filler and seams at the zone lines that no rule fixed across the game's zones. The land round a zone is its own (docs/graphics.md "Horizon hills"). The map's zone exits keep the zone-line grouping (`game/zone_exits.h`).
- [x] **Smoothed ground** (docs/graphics.md "Smoothed ground"). A classic zone's solid, opaque ground drawn with crease-aware smooth normals and curved PN triangles, up to 8 segments a side, within 1 unit of the flat ground, without cracks. Collision and everything the server measures keep the file's terrain; the zone flora stands on it. Not yet in a session.
  - Open: a larger offset would need feet and the server's spawns raised to the drawn ground; EQG zones drawn flat; no Options entry; the shadow cascades draw the refined triangles too.
- [x] **Zone flora for classic zones** (docs/graphics.md "Zone flora"): rules files of Hailstone's own, plants from a donor EQG archive, `+green` texel masks. Not yet in a session.
- [x] **Zone flora for every outdoor classic zone** (flora/default.txt). No plants under the zone's own roofs, ceilings and bridges, nor on water, built surfaces or rock; grown tile by tile round the camera. Checked over the 148 outdoor classic zones with tools/florascan; not yet in a session.
- [x] **Flora lit in the shaders:** the sun's shadow per pixel; sky, place light and lamps per vertex; bending round spawns standing in it; up to 262,144 plants a frame; climate families of donors. Not yet in a session.
- [x] **Zone flora's grass is a lawn built in the shaders** (no game asset): a patch per 3.2-unit square, blades at three levels of detail, wind, bending, tapered edges, the donors' kinds scattered over it. Not yet in a session, nor with the classic lighting.
- [ ] **Zone flora, open.**
  - A placed object's roof (a tent, a hut that is an object) still has grass under it.
  - Zones the default suits less well may want files of their own.
  - A dev-UI view to try rules in a zone and save them.
  - The classic lighting and the bending in a session.
- [x] **Upscaling, temporal groundwork and TAA/TAAU** (docs/graphics.md "Temporal upscaling", `display.upscaler = taa`).
  - Projection jitter (Halton 2,3, FSR 2's phase count), kept off picking and the name plates.
  - Camera motion vectors rebuilt from depth; AO applied before the resolve.
  - Our own resolve at the view's size (proximity-weighted samples, YCoCg variance clip, Catmull-Rom history, luma-weighted); the post pass at the view's size, RCAS after it.
  - History reset on zone-in, camera cuts and resize. Not yet in a session.
- [x] **Upscaling, temporal: motion and reactive mask** (docs/graphics.md "Temporal upscaling" 6). The scene pipelines write motion (wind sway, bones, door and object transforms, flora and lawn); a reactive mask from blended and additive draws. Not yet in a session, so characters and doors are untested on screen.
- [ ] **Upscaling, temporal, open.**
  - A negative texture LOD bias below a render scale of 100.
  - The bloom from the resolved picture.
  - Held items and other attachments keyed for their motion.
- [x] **Upscaling: AMD FSR 3.1's upscaler** (`display.upscaler = fsr3`, docs/graphics.md "AMD FSR 3.1"). The SDK v1.1.4's GLSL passes vendored (MIT), compiled by our CMake, run by our own host side on the temporal upscaler's inputs; its own RCAS at display.sharpness; auto exposure. Not yet in a session, nor on a GPU other than the developer's.
- [ ] Upscaling, FSR 3.1: try a transparency-and-composition mask for the water.
- [x] **Upscaler providers** (`display.upscaler = <name>`, docs/graphics.md "Upscaler providers", docs/plugins.md, `sdk/hailstone_upscaler.h`).
  - Temporal upscalers from outside the client, as add-ons of the kind `upscaler` in `upscalers/<name>/`; loaded at start from a copy, their instance and device requirements checked and added.
  - Run in FSR 3.1's place; FSR 3.1 or taa when one is missing, refused or failing. The example provider is a linear blit.
  - Tested in the zone viewer on the developer's AMD GPU, under MSVC/Wine with the XeSS and FSR 4 add-ons, and on Windows on an Intel Iris Xe tablet (XeSS runs; FSR 4 refuses the Intel GPU for FSR 3.1).
  - Not yet in a session, with the camera moving, or on an NVIDIA GPU.
- [x] **Upscaler add-ons, each in its own repository** against `sdk/hailstone_upscaler.h`, released as 1.0.0 for clients from 0.7.2-beta.
  - Intel XeSS (projecthailstone/hailstone-xess): Windows only (the SDK ships no Linux build); any vendor's GPU.
  - AMD FSR 4 (projecthailstone/hailstone-fsr4): Windows only; RX 7000 and 9000.
  - NVIDIA DLSS is not planned: its licence forbids use under an open-source licence and puts logo and approval duties on the application.
- [ ] AMD FSR 4 add-on: the FidelityFX SDK's licence file lists its signed DLLs under MIT; confirm that with AMD.
- [x] Upscaler add-ons: XeSS and FSR 4 are listed in the add-on index (since 0.7.2-beta).
- [x] **Frame generation** (`display.frame_gen`, docs/graphics.md "Frame generation"). An in-between frame from the temporal upscaler's motion and depth (scatter at t = 0.5, holes from the last frame, reactive pixels from this one); the UI composited sharp over both; FIFO with one more swapchain image. Seen in a session with validation. Not on a GPU other than the developer's, nor on a variable refresh rate screen.
- [ ] **Frame generation: pace by the measured frame time** (kettle-framegen's) instead of FIFO alone, so a game that cannot hold half the refresh is not stuck at a third. Consider a 3x/4x mode for 240 Hz screens.
- [x] **Doors.** Orientation matches SetEulerRotation512 (655 of 665 hinged doors in 21 zones fit their doorways); invisible switches block movement as in the original; Harbingers' BCN loads from its .mds (c34ca9e, d59971c). Walked live.
  - Open: BCN's actordef name, piece and animation; the door-angle sign.
- [x] Lighting: characters and doors take the light of their place (ambient probe), tinted baked light balanced (192ec47).
- [x] EQG terrain: keep its baked RGB and sky-light alpha from a matching `<TER file basename>.lit` sidecar, without applying the terrain placement transform.
- [x] **EQG static `CBGG1` / `CBSG1` materials:** packed RG normals, blue glow and alpha gloss decoded; modern HDR glow for authored self-illumination and additive zone rays. The channel layout is not confirmed (docs/unknowns.md, EQG packed surface maps).
- [x] **EQG packed surface maps on characters** (ef55a2a; docs/graphics.md). The actor renderer takes `EffectParams::packed_normal` and `glow` (`render/actor_surface.h`), so an EQG character or Hero's Forge piece on `CBSG1` reads its packed map as RG normal, glow and gloss.
- [ ] EQG `AddAlpha` HDR gain: decide whether the modern lighting's 2.5x applies to every EQG zone's additive surfaces or only where it was checked (Forgotten Halls).
- [ ] **Indoor lighting: tune Forgotten Halls' overall readability** (docs/graphics.md "Modern lighting"). Terrain lighting and packed material channels are restored: lamp pools back, sunlit bands gone, but the room is darker overall. Then check other indoor and outdoor zones.
- [x] Spell particles: the EDD billboard modes (flat and axis), animation-started actor emitters (look-surfaces; billboard meanings read from names, not traced).
- [x] **Spell particles traced from EQGraphicsDX9.dll:** CreateEmitter's counts and capacity, the update's emission, spawn shapes, motion and ramps, the quad builder's billboard modes. What the update reads from the actor and the creator's arguments are in unknowns.md.
- [x] Audio: the local player has no loop sound in the original; `/reversesound` has no effect in RoF2, here it swaps the whole output's channels (our choice) (f7b19af).

### Updates (docs/updater.md)
- [x] **The updater:** the signed manifest from the release feed; core, browser and installed plugins downloaded as their own packages, an unchanged one skipped; install at quit with Restart now; rollback of a cut-off install or a version that does not start; the notice in both packs' login screens and the client's own. Run for real on Linux against a local HTTP server (`hailstone_sign try`, packages from `make_update.py`).
- [x] **The login screen's corner:** this build, whether it is up to date, What's new (this version's part of the shipped CHANGELOG.md) and the GitHub page, in both packs and the client's own. Open: seen with a real update check.
- [x] The repository (or a public one for releases) has to be public: GitHub answers 404 to the client for a private one's releases. Public from 0.5.0-beta (AGENTS.md "Git workflow").
- [x] A release key (`hailstone_sign keygen`, the public half in `update-key.pub`); builds without it have no updates.
- [ ] The updater on Windows: WinHTTP and the install's renames of the running `hailstone.exe` and its DLLs (compiles; not run).
- [ ] Windows: a power cut between moving the running `hailstone.exe` into `previous/` and moving the new one in leaves no program (docs/updater.md "Limits and open points"). Linux and macOS keep the old file until the new one replaces it in one rename (hard link into `previous/`); Windows cannot replace a running program's file, so an order without that moment is still to find (a launcher stub, or a staged name the next start swaps in).
- [x] The box set's other windows (`--box-child`) have no updater and fetch no add-ons: only the first window downloads and installs (tests/updater_test.cpp).
- [ ] A lock between Hailstone windows the client did not start (two copies opened by hand): each still has its own updater and add-on manager on the same `download/` and `addons/` files.
- [ ] A release job in CI (packages, manifest, signature, upload).
- [x] **macOS arm64 releases:** a plain folder like Linux's with MoltenVK beside the binary; built, tested and signed ad hoc by the macOS workflow (docs/ci.md "macOS arm64"), packed and signed into the manifest locally. No Developer ID or notarization, by choice: players clear the quarantine (README.md).
- [ ] **macOS on a Mac.** Only the build and headless tests have run.
  - The client on Apple Silicon: MoltenVK's device features, the window, input, audio, the Keychain.
  - An update installed there.
- [ ] **macOS: the saved passwords use deprecated Keychain calls** (SecKeychain, deprecated since macOS 10.10; Apple Clang warns, `platform/credentials.cpp`). SecItem's calls replace them.
- [ ] **macOS: the startup error for a missing Vulkan** names the graphics card makers' drivers (app/startup_error.cpp); on macOS it would be the missing `libMoltenVK.dylib`.
- [ ] Resumable downloads (HTTP range requests) instead of starting over.

### Add-ons (docs/addons.md)
- [x] The manifest and GitHub link parsers, the library header reader; `tools/make_addon.py`, and `hailstone_sign` for add-on manifests.
- [x] **Install and uninstall from a GitHub link on the login screen**, scripts and plugins: content rules, the Lua and library checks, `addons/installed.txt`, a journaled install undone when cut off. The server's own listed and not removable. The Modern pack's card, the Gamepad pack's page and the client's own window.
- [x] **The add-on store:** every add-on of the signed add-on index with its manifest (held to the index's name and key), Install through its card; the Modern pack's add-ons card beside the characters card.
  - Open: listing the real index live, and index lines for add-ons beyond Project Triune's.
- [x] **Lua Scripts window** (`ScriptsWnd`, both packs, the EQ menu's Lua Scripts row, Gamepad More > Lua Scripts).
  - Every script in the folders with its state; Run / Stop / Pause; the reason a script may not run.
  - A per-character Autorun list (`scripts.ini`), started at the login's zone-in, not again at a zone change.
  - Seen live in both packs 2026-11-18.
- [x] The manifest's `caps` as a ceiling on what an installed add-on gets (`plugin::Policy::set_ceilings`).
- [x] **Plugin and add-on windows confined in the UI's Lua state:** their own globals; `hs` / `ui` / the libraries read-only; their code and every `hs` function they took run under their capabilities; text-only loads; `dofile` kept to the plugin's and the pack's folders.
- [x] An update's approval holds only for the capabilities and version the player saw.
- [x] **The player's word wins over a check under way:** an add-on removed meanwhile is not installed again; auto-update switched off meanwhile is not overridden; an update keeps the switch as it is at install time.
- [x] Scripts started by a script or plugin get no more than their starter; `scripts/config/` never runs as code.
- [x] **A server's compiled plugins load only once the player allowed their bytes for that server** (`servers/<profile>/plugin_approvals.txt`). The prompt in both packs and the client's own; changed from the add-on list.
- [x] A module runs only in a script it may do as much as: an add-on's module is refused in a script that may do more.
- [x] **Binding a command needs 'commands'.** Scripts and plugins cannot shadow the game's or the client's own commands (`Hooks::new_guest`, `reserved_command`).
- [x] The clipboard (`ui.GetClipboardText` / `SetClipboardText`) only for scripts with every capability.
- [x] A script's or plugin's `/plugin <name> load` never shows the consent question; only the player's own asks again.
- [x] Values between Lua and plugins: cycles refused, a budget of values and bytes (`plugin/lua_value.h`).
- [x] Script and module names stay inside the script folders (no absolute names, no links out); the config check canonical and case-insensitive.
- [x] `mq.pickle`: a script without every capability gets `scripts/config/<script>/`.
- [x] **Scripts' tables cannot have `__gc`:** a finalizer at the close registered hooks as the client's own, keeping the freed script. Nothing is registered while a state closes.
- [x] A Lua social's dry run does nothing but note its commands and waits; `hs.LuaSocialDryRun` needs `commands` and `actions` from a plugin.
- [x] The Lua bridge checks its arguments before it makes C++ strings (an error longjmps past them: Lua is built as C).
- [x] `mq.imgui` windows held to the 5 s runaway limit, also after a hook ran; a window that destroys windows no longer reads past the list.
- [x] `mq.delay` and the hooks' number answers defined for NaN, infinities and huge numbers.
- [x] `${Int[...]}` and `float.Int` defined for NaN, infinities and huge numbers.
- [x] `mq.TLO` values' metatable locked, so a script cannot swap in a `__gc`.
- [x] **MQ's `Math` TLO (its Calculate arithmetic), `If` and `Range`,** and the rest of MQ's int, float and string members with MQ's edge cases (`mq/calc.h`).
- [x] **Spawn searches follow MacroQuest's words and matching rules** (`mq/spawn_search.h`): spawn types, pets, 3D radius, named test, group, raid, guild, xtarget, light, player state.
- [x] **The alerts key** (CMD_HISTORIC_ALERTS, __ExecuteCmd case 168) toggles AlertHistoryWnd in both packs; listed with its Shift+H default. Seen live in the Modern pack.
- [ ] **MacroQuest's alert lists:** `/alert`, and the spawn search's `alert`, `noalert`, `nearalert`, `notnearalert`. None kept yet, so those words act as with an empty list.
- [ ] **The spawn search's `next` / `prev`: accepted and ignored.** MacroQuest's `/target next` cycles from the current target through the matching spawns.
- [x] **`mq.event` patterns match as MacroQuest's Lua events do (Blech)** (`mq/event_pattern.h`).
  - Case-insensitive; every `#...#` up to the first place the next literal is.
  - `#name#`, `|${...}|`, `##` / `||`; arguments by number with gaps.
  - Item links shown as their text unless `keepLinks`.
- [x] **Host functions only on the client's thread.** Off-thread calls do nothing (`MainOnly`); the caller has no capabilities there.
- [x] C++ exceptions out of a plugin caught at every entry point (`guarded`); a failed `InitializePlugin` leaves nothing registered.
- [x] `web_state` fills what a shorter `HailstoneWebState` holds; its `size_t` size kept for ABI 3.
- [ ] **The server plugins' prompt live.**
  - A first visit to a server that ships a plugin (NMS).
  - Allow mid-zone and Don't allow, in both packs and with a gamepad.
  - Whether a server extension allowed mid-zone needs the next zone-in.
- [x] **Plugin windows: the pack's global tables are views.** Plugin functions left in the pack's global tables (`GPSTATE`, `modern`, `ui_palette`) used to run as the player; the plugin now sees those tables through views that make what it writes run as the plugin (and stop when it unloads). The views refuse setmetatable, rawset and `__gc` (ui/lua/lua_host.cpp).
- [x] **Plugin windows: the pack's functions are wrapped.** What they return or raise reaches the plugin as a view; the pack's arguments to a plugin's callbacks are views; `getmetatable` gives a view of a metatable the plugin did not set (ui/lua/lua_host.cpp).
- [ ] **A function or table a plugin passes to the pack is not wrapped** (as an argument to a pack function, or returned from a callback).
  - A pack function that keeps it and calls it later runs the plugin's code as the player.
  - A full membrane (wrap every value crossing into pack code) would close it, at a cost on every call.
  - The packs' shared libraries that keep callbacks should keep them per window instead.
- [x] **Auto-update:** the check at start (`addons.check_hours`), locally modified files, files moved only at the login screen, approval when the caps grow.
- [x] Signatures: the key pinned at the first install, an update with another key (or unsigned) refused.
- [x] **Suggested add-ons:** the signed add-on index (projecthailstone/hailstone-addon-index) and a server's `[addons] suggest`. A line on the server select; Look at it / Not now / Don't ask again on the login screen, in both packs and the client's own. A suggestion's card held to the index's name and key.
- [ ] Suggested add-ons live: a first login to Project Triune from a fresh install, with the real index on GitHub.
- [ ] Add-ons against the real GitHub: a public repository's release and branch, through GitHub's redirects to its asset hosts (unit tests and an in-memory GitHub only).
- [ ] Add-ons on Windows (the WinHTTP final-address check compiles; not run) and a plugin add-on on macOS.
- [x] **A `core` package does not write over an add-on's plugin folder:** its files in such a folder are left out of the install (5df5960; docs/addons.md).

## 3. UI

- [ ] Trace and fix the upstream gamepad all-pack smoke test's cursor-extent assertion with ImGui debug checks enabled (docs/unknowns.md "UI").

- [x] **Group role controls in both packs.** Right-click member names or the player's own name (Y on a gamepad Group window row); server-confirmed role icons in the Group and Player windows; leader-only Main Tank / Main Assist / Puller changes, mercenaries restricted to tank. Not checked live.
- [ ] Check RoF2 group targeting after a member dies, including when several of their corpses are in the zone (docs/unknowns.md "UI").
- [x] **AA names missing from dbstr_us.txt** (a live folder's renumbered titles, gaps in a server's export). The windows show the spell's name, else "Ability <group>" (GameConnection::aa_display_name); plugins and /alternateadv keep the raw title. Seen live.
- [x] Modern AA window: aligned headers and rows at the dialog's scale, current-frame column bounds, saved column resizing, clipped cells and Reset columns (live acceptance pending).
- [x] **Hover respawn:** availability validated; same-zone player identity, placement and resources recovered; resurrection confirmation, server timeout and disconnect handled. Live acceptance open.
- [x] **Modern Quest Journal and independent Quest Tracker.**
  - Compact tracked-first Active and History pages, shared tasks in Active.
  - A Shared members popup for the native roster, member roles and permission-gated membership/leader actions.
  - Objectives/Description/Rewards sections, individual hidden objective rows with native group separators, per-character tracking flags.
  - The tracker reads authoritative active tasks while the journal is closed and uses the shared HUD move/resize/lock/roll-up behaviour.
  - Open: live quest updates across zoning, shared roster updates and membership actions, mouse interaction.

- [x] **Labels: combat timer, breath, respawn gauge, aggro meter, title, ATK, regeneration, and more EQTypes**, in the modern and gamepad packs (90aa43f; checked live). ATK leaves out profile +0x3468. Untracked label and gauge types are in unknowns.md.
- [x] Buff icon fading below 19 s; gauge colours for special values (not seen live).
- [x] Hot buttons: the AA and gem right clicks, vertical bars (modern pack, checked live), the key-map text corner; the ranged attack.
- [x] Hot buttons: vertical bars in the gamepad pack (checked live). Open: the AA and gem right clicks seen live.
- [x] **Hot buttons from inventory slots.** An inventory or bag slot held (or dragged, modern pack; LB in the gamepad pack) gives an item link or inventory slot button; item links find their item, show its icon and use it. Open: seen live, the Krono slot's hold (type 13), the bank's slots.
- [x] **The login, server select and character select screens drawn by the modern and gamepad packs** (`app/lua_ui_pregame.cpp`, `ui/<pack>/login.lua`, `serverselect.lua`, `charselect.lua`). Login: Continue as, the server's settings and More options folded away. Server and character select seen live in both packs.
  - Open, played live from these screens: a world played, a character entered, created and deleted, a box login, a server saved, deleted and its interface switched.
- [x] **Return Home on the character select** (CLW_Home_Button), in the ImGui screen and both packs. Enabled by the list's go-home flag, asked first with eqstr 6137, then OP_EnterWorld with the home flag (`hs.CharacterSelectDo("home")`, `canGoHome`, `homeQuestion`).
  - Open: tried live against a server with `World:EnableReturnHomeButton` on; the tutorial button (CLW_Tutorial_Button, the same packet's tutorial flag).
- [x] **The character creation screen drawn by the modern and gamepad packs** (`hs.CharacterCreate`, `ui/<pack>/charcreate.lua`): name with a random one, gender, race, class, deity, start city, stats, the game's descriptions, Appearance. Open: played through live in both packs.
- [x] The spell book: memorize only into an empty gem; Mem. This Page does nothing in RoF2 either.
- [x] The compass's heading error below Sense Heading 200, reset by a successful Sense Heading (checked live); sense heading's raw heading and the South band match the original.
- [x] The item display's placeable, collectible, No Destroy, No Storage and trophy lines (stock EQEmu sends none of this data).
- [x] **Context tips** for experience, invites, consent, red-con target, world containers and more. Tip table ids fixed (two missing entries had shifted every tip from 0x38); OP_ConsentResponse / OP_DenyResponse handled. Open: the tips listed in unknowns.md.
- [x] The chat server's per-window channel state is saved.
- [ ] **The chat log (/log): match the original's file handling.** Trace `CEverQuest::OutputTextToLog` @0x5130c0: opened and closed per line, or held open. The client holds the file open while logging (docs/unknowns.md).
- [x] Tracking window: the controls run the /track commands, auto update saved (modern checked live).
- [x] Tracking death cleanup (WORLD-007): tracked-target deaths send the stop request and tracking message;
  known deaths remove the cached entry, local deaths included (live acceptance pending).
- [x] Tracking in the gamepad pack's More window for characters with the skill (checked live).
- [x] `/copylayout`, `/offlinemode`, `/bugreport` (f3c5c5d; `/offlinemode`'s yes path not seen live).
- [x] **Ability Book** (Hailstone's own, `game/ability_book.h`, `ui/modern/abilitybook.lua`).
  - Activated AAs, combat abilities and activated skills in one searchable, sortable list with descriptions and timer groups; double-click / drag to a bar / right-click menu.
  - The EQ menu row after Combat Abilities, and `/abilitybook`. A class filter shows that class's abilities and the shared ones.
  - Client-side autoskill: an ordered list; while fighting, any time, or out of combat for beneficial abilities; only when its buff has worn off; endurance, mana, own health and target health conditions; failure backoff; the switch kept per character; `hailstone.cfg` `[abilities] autoskill = off`. Checked live on a cleric.
  - Open: the gamepad pack's page; the class filter and sort with a multiclass character (NMS sends every AA as every class's, docs/unknowns.md); a key binding.
- [x] **Advanced Inventory** (Hailstone's own, `InventoryManagerWnd`, `ui/modern/inventorymanager.lua`, `app/lua_ui_inventory_manager.cpp`).
  - Everything worn, carried, banked and in the shared bank in one list or grid.
  - Search; place / kind / mark / flag / stat filters with counts (a stat filter adds its column); Clear filters. Column-heading sort (place grouped by bag, name, level, the stat, quantity, value, weight), a second click reverses; the grid has a Sort menu.
  - Multi-select in the list and the grid (click, Ctrl, Shift, Ctrl+A).
  - Bulk actions (`GameConnection::start_bulk_job`, docs/mechanics.md "The Advanced Inventory's bulk actions"): Sell; Tribute (personal or guild master); Destroy (carried, or banked while the bank is open); Withdraw (from the open bank into the packs, never equipped). Each checked against the item's serial and quantity at confirmation and while the job runs.
  - `/finditem [text]` and `/advi`.
  - Each icon is the item's slot (pick up, drop, use, inspect, hot button); banked ones while the bank is open.
  - Keep and Sell marks per item (in bulk too); Sell marked at a merchant; Stack / Sort / Tidy.
  - The EQ menu row after Inventory, and a button in the inventory window, whose search, Stack / Sort and ... menu moved here.
  - Keep (`game/kept_items.h`, `kept_items.txt` per character) blocks destroying, dropping, trading, giving, selling, parcels, barter sales, bazaar offers, buyer compensation and tribute (docs/mechanics.md "Kept items"). Bulk Keep marks write kept_items.txt once.
  - Open: not checked live (bulk tribute, withdraw and destroy against a real EQEmu especially); the gamepad pack's page; a key binding.
- [x] **The bank as last seen** (`game/bank_snapshot.h`, `bank.dat` per character; docs/mechanics.md "The bank as last seen"). EQEmu's `Inventory:LazyLoadBank` sends the bank only near a banker, so the Advanced Inventory and `/finditem` list the last copy until then, marked stale and out of reach (`hs.BankSeen`). Seen live (playprobe `--bank-file`).
  - Open: a server with the rule off, the real client's window live (docs/unknowns.md); socketed augments are not listed or searched yet.
- [x] **Modern UI item marks are kept per server and character** (junk, locked slots, bag filters; `ui/modern/lib/itemtools.lua`). `items.` keys are the character's settings (`LuaUi::character_setting`), in `servers/<profile>/.../ui-modern.txt`. Marks from the shared file are read until the character changes them; a layout copy leaves them.
- [x] **The client's settings per character** (docs/layout.md "A character's settings"). The character's own `hailstone.ini` from the entry into the world, made from the shared one at its first entry; applied on entering and leaving: graphics, frame caps, sound, mouse, keys, gamepad, the game switches.
  - Open: not seen live with two clients; copying one character's settings to another or back to the shared ones; the window's place per character; the character's file in a bug report (it would share `settings/hailstone.ini` with the shared one).
- [ ] **The in-game browser** (docs/browser.md "Milestones").
  - Done: the messages, pipes, tab host, the Linux helper, the window in both packs and the scripts' and plugins' tabs.
  - Open: the helper on macOS (app bundles), and checked on Windows and Linux arm64 (CI).
  - Open: Chromium's sandbox on Windows and macOS.
- [x] **Scripts' and plugins' pages held tighter** (docs/browser.md "Rules", "Profiles", "Sizes"): a cookie store per add-on; frames and requests only on the allowed sites, never this computer or a private network; console lines and page messages bounded in bytes.
- [ ] **Scripts' and plugins' pages, open.**
  - A public host name whose DNS answer is a private address is not refused (it would need the helper to check the address it connects to).
  - Whether CEF hands WebSocket handshakes to `OnBeforeResourceLoad` was not checked.
- [ ] **The other local-time calls outside `src/platform/` go through `platform::local_time`.** Bug reports already do.
  - `localtime_r` / `localtime_s` in `net/game_connection_social.cpp` and `mq/tlo_game.cpp`.
  - `std::localtime`, which shares one buffer between threads, in several `app/lua_ui_*.cpp`, `market_ui.cpp`, `client_app_info.cpp` and `net/game_connection_market.cpp`.

## Navigation (docs/navigation.md)
- [x] Navigation meshes built from the zone's collision on worker threads, cached per server and zone; areas from the regions (water, lava, zone lines, PvP).
- [x] `/nav` (MQ2Nav's commands), `/moveto`, the `Navigation` TLO; the walk steers by the heading and the movement keys, gets unstuck, opens doors in its way, and asks the server's `/find` path while the mesh is building.
- [x] Tried live on NMS Dev (ecommons): long walks, /moveto, a door, the TLO, the overlay, pause, cancelling with a key, an unreachable point.
- [ ] Live: a moving spawn, a closed door clicked on the way, the server-path fallback, a stock EQEmu server.
- [ ] **Find replies can swap** (docs/unknowns.md). A quiet path request and a shown `/find` in flight together can still swap on stock EQEmu (an empty reply at once, a full path later). The 10 s cut-off for an unanswered request is a guess.
- [x] /follow helped by the mesh (round corners and walls, unstuck, a 400 leash), switchable with `nav.follow`.
- [ ] /follow's help live (following a player round corners and through a doorway).
- [ ] Lifts, jumps and safe drops as off-mesh links (drops checked against the fall-damage rules).
- [ ] Walks across zones (`/travel <zone>`) over the zone lines and zone points.
- [x] **The map's World view and zone finder** (docs/navigation.md "Zone atlas"; modern pack). Atlas pages zoomed and panned; zones searched by name; another zone's map; routes over the server script's zone links or the zone points learned per server; the exits on the map and "Walk there" through the next zone line. Not tried live.
- [ ] **Zone atlas, open.**
  - Live against a server with the script and a stock one.
  - The World view in the gamepad pack.
  - The route going on by itself after each zone (with `/travel`).
  - While another zone's map is shown the minimap draws it too (they share the map state).
- [x] `/stick` (distance, behind, id; the `Stick` TLO), the `LineOfSight` TLO and `Spawn.LineOfSight` (the abilities' own check). Not tried live.
- [x] **The Find window** (Hailstone's, in place of FindLocationWnd; docs/navigation.md "Find"): the zone's merchants, bankers, guildmasters, other services and the findable list (OP_SendFindableNPCs), as the modern map's Find page and a gamepad window; search, kinds, target, the find path, NPC marks on the map. Not tried live.
- [x] **Find: the guide path over the client's own mesh** (`app/guide_path.h`), found again as the player strays or the NPC moves; shared terrain support for both trail styles (`game/path_trail.h`). Not tried live.
- [x] **Find: fourteen thin rainbow threads** winding a few feet above the terrain, following the route's floor in multilevel areas and omitting unsupported stretches. Live appearance still to check.
- [x] **Find: Options selects a gold ground line (default) or rainbow strands,** applied at once and saved per character in both UI packs. Live acceptance open.
- [x] Find: Walk there (the navigator to within 10, the guide path showing; gamepad X). Not tried live.
- [x] Find: the modern minimap marks the NPCs the map marks (the same kinds and search). Not tried live.
- [x] **Find: quest givers from the server script** (0x1436 / 0x1437; `quest_npcs`): the NPCs whose own quest file takes hand-ins or talks, made once per zone process and kept. Not tried on a live server.
- [ ] Find: the gamepad minimap's marks; quest givers measured on a live server (the zone log's line) and the figure put in server/README.md.
- [ ] A key binding of the client's own (the key window holds only the original's commands): walk to the target; `/nav target` on a hot button meanwhile.
- [ ] Tactical queries for bots (a kite point; `/stick`'s pin, `!front`, `moveback`, `uw`, and following a changed target without `hold`).
- [ ] **Auto Combat** (a script with its own window, an add-on in a repository of its own).
  - Manual (full and partial), Hunter (roam and camp) and Assist modes; Spells and Abilities pages of rows with conditions.
  - Client side done: `Me.AbilityReady` / `CombatAbilityReady` / `AltAbilityReady` and timers, `Target.Buff` and the rest from OP_TargetBuffs, `/stick`, line of sight, `hailstone.server_rule`, `hailstone.hold_autoskill` and `autoskill_list`.
- [ ] **Auto AA** (an add-on of its own, projecthailstone/hailstone-autoaaspend).
  - Client side done: `hailstone.alt_abilities`, `hailstone.train_aa`, the Modern pack's `lib/aa.lua` with a part on each row, in the details and under the header.
  - Open: the Gamepad pack's AA window as a library a script can extend the same way; a purchase checked live against EQEmu.
- [ ] Waypoints and an in-client mesh editor (areas and links in a shareable per-zone file).
- [x] Swimming: water surfaces in the mesh, the look pitch steered while swimming (breath, diving, climbing out).
- [x] Zone lines marked exactly into the mesh; a walk stops before stepping into one (seen live at Oasis / North Ro).
- [ ] **Levitation paths.** Swimming checked live end to end 2026-11-18. The jump key under water needs a real keyboard's key repeat to judge.

## Item comparison (docs/item-compare.md)
- [x] **The item against what the player wears**, on a copy of the sheet through the stat rules (caps, recommended level, the shown AC and ATK).
  - The slot choice: pairs, two-handers, the secondary.
  - Effects gained and lost; why it cannot be used; the class-weighted verdict.
  - `hs.CompareItem` / `CompareSlotItem` / `CompareBagSlotItem`; both packs' tooltips, panels and item displays; Options.
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
- [x] **Options** (modern Display > Isometric Camera, the gamepad pack's Camera page; `iso_*` through `hs.Option`). The pad action `camera_iso`; `/camera` toggles. No key command of its own (the key table is the original's): `/camera toggle` on a hot button.
- [ ] Whole objects fading as one; tuning the cut, the roof and the silhouette against real zones.
- [ ] Live: the roof in buildings, caves and stacked dungeons; the silhouette; the hover ring; the options in both packs.

## 4. Check live (implemented, not confirmed)

- [x] Shift-click / Ctrl-click quantity flags (checked live).
- [x] Modern quantity dialog: opened again for another request while its box has the keyboard, the box shows the new amount.
- [x] Equip slot availability (the profile's slot mask) and auto-inventory's use check, ported.
- [x] **Guilds tried live with a GM-made guild** (2026-10-29): invites across zones, chat, MOTD, ranks and permissions, notes, alts, leader change, the guild bank.
  - Open: a second member's view of the guild bank; /guildwar and /guildpeace are only logged by EQEmu.
- [x] **The guild window's Tribute page:** each member's tribute and trophy states, donations, last donation; the opt-out buttons (`guild_tribute_opt_out`, `guild_tribute_opt`). Seen live 2026-10-31 (playprobe `--tribute-opt-out`, `--tribute-cycle`).
- [x] Guild banners in both UI packs: server kit and status replies, filtered refresh, placement, removal and
  design edits with the modification item. Planted actors use the received patterns and colours.
- [ ] Guild banners: verify editing, planting, removal, benefits and Guild Hall portal travel on a banner-capable
  server (docs/unknowns.md).
- [x] **A raid of 18 and the raid MOTD button.** Seen live 2026-10-31 (playprobe `--raid-size`, `--raid-motd`, `--raid-accept`).
  - Open: zoning in, the leader printed "<name> joined the raid." for each of the 17 members (docs/unknowns.md).
- [x] **Gamepad pack: the window under a closed one takes the pad back.** Two windows asking for the pad in one frame kept only the last request; every request is kept now. Checked live 2026-10-31.
- [x] Melee sounds (the cues fire with the right waves; not listened to).
- [ ] **The six RoF2 sound triggers added together:** an item moved by a click (0x92), bags and containers
  (0x85/0x86/0x91), an adventure's loss and win (0x8c/0x8d), a quest reward (0x8d), struck down by the gods (0x8f),
  water in and out (0x64/0x63). Each cue and its condition is unit-tested, the water path end to end through
  `app::PlayerMovement`; none listened to live.
- [x] **Augment appearance-slot gating.** With game feature 0x15 on (the client's setting for EQEmu accounts) a type-21 ornament into a type-21 socket is not refused; seen live 2026-10-31 (stored in `inventory.augment_six`). The refusal with the feature off is unit-tested only (docs/unknowns.md).
- [ ] **A server's class names in a live session** (`set_class_name`, docs/plugins.md "Class names"). Unit, plugin and script tests only.
  - A plugin that names classes on its server's hello, then every window that names a class (both UI packs).
  - `/who`; camping to character select and back.
  - Logging in to another server with the plugin still loaded (the names must go).
  - Open: the stock-layout windows' own class labels (the XML's "WAR" check box labels in the group search and looking-for-guild windows) keep the stock names.
- [ ] **Ground objects made only of passable triangles are clicked by them** (as the Plane of Fire orb door is).
  Unit-tested only; open: a live dropped item or ground spawn whose model has no solid triangle.
- [ ] OP_Weblink from a quest's `SendWebLink`, `/url` from a script, and the browser window (`/web`), in a live session (unit and window tests only).

- [ ] **Idle appearances freed during a zone** (render/look_eviction.h, five minutes unworn). The rule is unit-tested; the renderer side was only built.
  - Watch a busy hub for an hour: the log's "freed N idle appearances", the GPU memory.
  - A spawn coming back after its look went (dressed again on the worker).
- [x] Free the texture slots of freed appearances too: a freed look's shared textures go once no remaining model reads their slot (`ActorRenderer::evict_unused_textures`; PR #27, 53b346a).
- [ ] **Untested GPU paths** (neither run on such hardware).
  - A GPU without a sampleable D32_SFLOAT depth attachment (the D32_SFLOAT_S8_UINT / D24_UNORM_S8_UINT fallback, with `separateDepthStencilLayouts`) under the validation layers.
  - ObjectRenderer's instance flush on a driver with non-coherent host memory.

- [ ] **The 60 Hz game clock in a live session.** Checked live 2026-11-18: jumps, falls and fall damage the same at 30, 60, 144 and uncapped; turning; a stalled zone load. Open: /follow and camera smoothness at each rate, strafing feel.
- [ ] **The cameras 1..5 in a live session** (docs/mechanics.md "Cameras"). Each one's place, the wall sweep and sitting (the tether's bone 0) checked live 2026-11-18. Open: mouse look and the camera keys in each, doors, riding.
- [x] **Knockout and drunkenness cameras.** Knockout selects overhead and restores the saved camera on recovery; drunkenness at 70 and above selects first person and widens the FOV.
  - Open: live acceptance, missile camera 8 and camera restore after zoning.

## Code health

- [x] **Shared asset cache** (`assets.shared_memory`, docs/layout.md "Shared archive memory"). Server-overlay archives and inflated character WLDs mapped from a content-addressed cache shared by clients; animation keys read from the mapping.
- [ ] **Shared asset cache, open** (docs/unknowns.md "Shared archive memory").
  - Live Windows and macOS sessions.
  - Sustained play with zoning and more than two clients.
  - Whether static collision or geometry are worth sharing too.
- [ ] **Shared GPU textures** (docs/graphics.md "Shared GPU textures", docs/research/shared-gpu-memory.md). Until these are done it stays a build-tree experiment with no client setting.
  - A live Windows test of the failure paths, every client playing on with private textures: broker killed, a client killed while holding the lease, a lease not free in time, a hung frame.
  - Frame-time measurements beside the memory ones.
  - Sustained play with zoning; Intel hardware.
- [ ] Shared GPU textures on Linux (a Unix socket passing opaque FDs) and more than 16 clients (docs/research/shared-gpu-memory.md).

- [ ] **The layer order does not hold for `net` and `assets`: they include `game/` headers.**
  - The documented order: `app` -> `game` / ... -> `assets` / `net` / `web` -> `core` / `platform` (docs/plan.md and AGENTS.md).
  - In `net`: every `game_connection*.cpp` (GameConnection keeps the game state); the packet codecs `zone_packets.cpp` (`game/mechanics/remote_spawns.h`: the bit-packed positions), `spell_packets.h` (`game::mechanics::cast::Packet`), `task_packets.h` (`game::Task`), `training_packets.h` (`game::kTrainSkills`, `kTrainLanguages`), `aa_packets.h` (`game/alt_adv.h`) and `ability_packets.h` (`game/mechanics/abilities.h`).
  - In `assets`: `wld_zone.cpp` (`game/zone_names.h`), `model_points.cpp` (`game/sine_table.h`); `eqg_character.cpp` (`game/mechanics/anim_select.h`, `game/race_models.h`); `character.cpp`, `character_parts.cpp` and `character_dress.cpp` (`game/appearance*.h`, `game/actor_look.h`, `game/race_models.h`).
  - All of it builds into the one `hailstone_engine` library, so nothing fails to link, and no check enforces the order.
  - Fix, one of:
    - move the shared pieces down: the position unpacking, race and appearance tables, zone names and the sine table into `core/` or the layer that reads them, and the packet structs out of `game::` types;
    - or write the real order into docs/plan.md and AGENTS.md: GameConnection above `game`; the tables `assets` reads as a lower part of `game`.
  - Then add a check (a CI grep like the NMS one) so it stays true.
- [ ] Far shadow cascades are redrawn every frame; reusing them needs per-cascade tracking of moving casters first.
- [ ] Session-long caches (`ModelArchiveStore` libraries' dress and model caches, `EqgCharacterStore`) never evict.
- [ ] `tlo_index` makes a finalized userdata holding a `std::string` and a `Value` for every `.Member` step; it could keep the name as a Lua string.
- [ ] Other `hs.*` functions still build `std::string` temporaries inside push expressions; move them to `plugin/lua_keep.h` as they are touched.
- [ ] **The pack's consent window is guarded from its first draw only:** a script drawing before it on that first frame can still add to it once. Pre-register it as guarded.
- [ ] The client's own plugin consent prompt (app/automation.cpp) has no arming delay; the packs' wait a second before Allow and Don't allow work.
- [ ] Plugin windows' memory: plugin code may use 3/4 of the UI state's limit (a reserve for the pack), not a budget per folder.
- [ ] The Lua review fixes (vendored lstrlib match check, longjmp paths in lua_host/lua_imgui/lua_mq) have not been built with MSVC under Wine.
- [ ] Windows: about 260 `path::string()` calls rely on `cmake/utf8.manifest` making UTF-8 the ANSI code page. Sweep them to `core/path_utf8.h`, or record that the manifest is required.
- [ ] Tests for /camera, /mousespeed and /copylayout through ClientApp itself (their helpers are tested), and for the gamepad pack's browser change.
- [ ] `net::server_file_destination` still accepts versioned shared libraries (`.so.N`) in scripts and plugins as data, while `tools/make_addon.py` refuses them.
- [ ] Updates: `kept-<time>/` folders left by a rollback are never cleaned up; offer to remove them.
- [ ] Conversation journal texts over 0xffe characters still split on reload (the fgets limit), as in the original.

## Not planned

Voice macros and audio triggers; the commercial-service windows (the marketplace's funding pages among them); the zone guide, zone path and tribute trophies (no data on EQEmu); Miles' reverb DSP and DLS synthesis; the client's own graphics settings.

## Memory

- [x] **A zone's textures and uploads** (docs/graphics.md). A fully opaque power-of-two image's mip chain is built by the GPU (on an sRGB view, in linear light, as the CPU chain); the zone's uploads are staged to the card in pieces. The PR author measured a peak commit about 115 MB lower in the zone viewer; a live session in a small classic zone showed no change. Not measured again since.
- [ ] **The character archives: the largest single term in the client's memory.**
  - The 135 global character archives cost about 926 MB (the log's `zone load: start: 345 MB`, then `zone load: start: 1271 MB` after them).
  - Archives in the game folder are mapped read-only, so only the index is private; overlay archives are still read into the heap (docs/hardening.md "Mapped game archives").
  - Character WLDs keep a compact resource index and decode on first use; a decoded WLD whose last reader is done stays in a retained set of 128 MiB (least recently released out first).
  - The archive-bytes / parsed split of that 926 MB is unmeasured.
  - Next:
    - measure fresh and repeated-zone memory and decode counts with the retained set, and pick its budget from that;
    - compare Windows private commit with resident checkpoints and model animation/geometry subtotals after shared/compacted animation keys;
    - attribute name tables, parsed EQG source data and allocator retention before further cache policies.
- [ ] **The base cost.**
  - About 492 MB before any zone is loaded: the sound library's index over the 17 `snd*.pfs` at 119 MB, the spell particle definitions, the sky colour maps, the hero's forge tags, the fonts.
  - About 200 MB of shadow maps at the defaults (six 2048² cascades and the point-light cubes).
  - The split has not been measured one by one; nor has the transient above the end-of-zone figure.
- [ ] An allocator of the client's own (mimalloc or jemalloc): the client uses the system allocator throughout, so the memory it asks for is not the memory it holds.

### Server add-on host services

- [x] Append bounded movement profiles, map/Find eligibility, merchant click context, received raid/actor/item
  readers and transient world geometry services to the SDK, preserving stock fallback.
- [ ] Complete live acceptance for enhanced follow/mounts, controller input, private sales, raid actor selection,
  item metadata/loot prompts, instance labels and world overlay presentation; see unknowns.md.
