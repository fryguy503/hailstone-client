# The Hailstone folder

Hailstone keeps everything in one folder, the one that holds the binary. It is portable: copy the folder and the
settings, servers and characters' files go with it. The game install the player picks (RoF2 or a current one; the asset folder) is only read;
Hailstone never writes into it. Its `eqclient.ini` and `defaults.ini` are defaults only: what the player changes goes
to `hailstone.ini` or to the character's files under `servers/`.

```
hailstone/
  hailstone(.exe)            the binary
  shaders/                   shipped; an update replaces it
  icons/                     shipped: hailstone.png, the window and taskbar icon (hailstone-crest.png, the full crest;
                             on Windows the icon is built into hailstone.exe instead, cmake/windows/hailstone.ico)
  pictures/                  shipped: the default picture behind the pre-game and loading screens, shown when
                             neither the server nor ui/loadingbackgrounds/ has any
  flora/                     shipped: Hailstone's flora rules for zones without flora of their own (docs/graphics.md
                             "Zone flora"): default.txt for every outdoor classic zone, <zone>.txt for one, the
                             shared parts and climate families they include; replaced
                             by an update
  CHANGELOG.md               shipped: the release notes; the login screen's What's new shows this version's part
  ui/<pack>/                 UI packs (modern, gamepad), replaced by an update
  ui/settings/<pack>.txt     each UI pack's script settings shared by every character, and the window layout a
                             character starts from (no ui.ini, so not a pack: a build or update leaves it alone;
                             ui-settings/ before 2026-11, moved here at start-up by move_old_ui_settings)
  browser/                   the in-game browser's helper and CEF's files (docs/browser.md); replaced by an update
  hailstone.ini              the client's settings (core/config.h); a character in the world has its own (below)
  hailstone-windows.ini      Dear ImGui's own window positions
  plugins/                   compiled plugins: plugins/<name>.so, or plugins/<name>/ with the binary and its settings;
                             plugins/<name>/ui/<pack>/ holds its Lua windows for each UI pack (docs/plugins.md)
  scripts/                   MacroQuest Lua scripts; scripts/config/ is mq.configDir (mq.pickle; config/<script>/ for a script without every capability)
  textures/<pack>/           texture packs; display.texture_folder names one (empty: textures/default/). A pack built
                             by Hailstone Texture Studio or tools/texpack holds by-hash/<hash>[.m|.k|.n|.r].dds and
                             pack.ini (what it was built with)
  tools/<name>/              tool add-ons, each a program of its own with its files (docs/addons.md "Tools"): the
                             add-on manager's alone, as nothing else is put there
  maps/                      the player's in-game maps for every server, maps/<dir>/ a maps directory (below)
  servers/<profile>/         one folder per saved server (core/server_profile.h); servers/.deleted/ the deleted ones
    server.ini               [Server] name, host, port, username, remember
    assets/                  the server's own client files, over the asset folder (downloaded, or added by hand)
    hailstone.cfg            the server's rules for plugins, scripts and assets (downloaded; docs/plugins.md)
    scripts/, plugins/       the server's own Lua scripts and compiled plugins (downloaded; docs/plugins.md)
    plugin-config/<plugin>/  a plugin's own files for this server (server_config_dir; the NMS plugin's nms.ini)
    plugin_approvals.txt     the player's answers to the server's compiled plugins, by SHA-256 (plugin/consent.h;
                             docs/plugins.md "The server's compiled plugins ask first"); never the server's to write
    server_files.txt         the files downloaded from the server: SHA-256, size, path (net/server_files.h)
    .download/               a download that has not finished; moved into place when every file has come
    maps/                    map edits made on this server, for all its worlds; maps added by hand for it
    zone_links.txt           how the zones connect, for the map's zone finder: "s <from> <to>" from the server's
                             list, "l <from> <to>" learned from zone points (game/zone_routes.h)
    worlds.ini               [<world>] name, listed: the worlds the login server listed last
    characters/<world>/<character>/
                             UI.ini, hotbuttons.ini, BZR.ini, BART.ini (buy lines), btrlog.txt (barter
                             log), AddressBook.txt, IgnoreList.txt, GuildNotes.txt, chat.ini, options.ini,
                             scripts.ini (the Lua scripts started for this character: the Scripts window's Autorun);
                             hailstone.ini, the client's settings for this character ("A character's settings" below);
                             kept_items.txt (the items marked Keep in the Advanced Inventory window, one
                             "<item id> <name>" a line; game/kept_items.h);
                             bank.dat (the bank and shared bank as last seen, which the Advanced Inventory lists
                             while the server has not sent the bank; game/bank_snapshot.h);
                             ui-<pack>.txt, the UI's window layout for this character (places, sizes, scales,
                             opacity, locks: the `hud.*` settings);
                             eqlog.txt (or /log on <name>: <name>.txt), the chat log; plugin-config/<plugin>/ a
                             plugin's files for the character (character_config_dir; the NMS plugin's itemrules.ini);
                             /outputfile's dumps (the original writes them in the game folder), under any name
                             but those of the settings files above (any case; a chat log's name is allowed, as in
                             the original; is_kept_character_file)
  notes.txt                  the notes window's text (/note)
  web/                       the in-game browser's data: web/<profile>/ one server's cookies and cache, cef.log
                             CEF's log (docs/browser.md)
  logs/hailstone.log         the log of this run, also on stdout; the earlier runs' are hailstone.1.log to .3.log;
                             the viewers write viewer.log, a second client hailstone.<pid>.log
  logs/hailstone.lock        held while a client writes hailstone.log, so a second one writes its own (left behind
                             on Linux and macOS, where it is only locked; Windows deletes it at exit)
  logs/crash-<time>.txt      a crash's report (crash-<time>.reported.txt once bundled into bug-reports/)
  bug-reports/               the bug and crash reports to post (hailstone-bug-<time>.zip, hailstone-crash-<time>.zip;
                             the newest 30 kept)
  screenshots/               the screenshot key's pictures, and where a relative --screenshot FILE goes
  addons/                    the add-on manager's (docs/addons.md): installed.txt (each add-on installed from GitHub,
                             its source, version, accepted capabilities, key and files) and its copy
                             installed.bak.txt, its downloads, an
                             install's journal and the add-on index (index.txt); the add-ons themselves go in
                             scripts/<name>/ and plugins/<name>/
  update/                    the updater's downloads, the installed components' file lists, the files the last
                             update replaced (until the new version has started) and its journal (docs/updater.md)
  cache/                     safe to delete after clients stop: cache/plugins/ holds the loaded plugins' copies, cache/pipelines.bin
                             the compiled Vulkan pipelines, cache/navmesh/<server>/<zone>.navmesh the zones'
                             navigation meshes (docs/navigation.md)
    assets/v1/pfs/           optional shared archive snapshots ("Shared archive memory" below), their stable locks
```

The paths come from `src/platform/paths.h`: `install_dir()` is the binary's folder, from which the shipped parts are
read (`shaders_dir()`, `ui_packs_dir()`), and `data_dir()` is where everything written goes. `data_dir()` is
`install_dir()` itself: every setting, server and character file stays in the Hailstone folder, so copying the folder
takes all of it along, and nothing outside it is read for settings (no `~/.config`, no `%APPDATA%`). Hailstone never
imports another folder's settings either. `HAILSTONE_DATA_DIR`, when the player sets it, names another folder.

`platform::choose_data_dir` decides, and the log's first lines say what it chose. When the Hailstone folder cannot hold
the settings, the client stops at start with a message box saying so and asking for the folder to be moved somewhere
the player owns (`data_dir_problem()`), rather than writing anywhere else:

- the folder is not writable (an install under Program Files or /usr);
- the folder is a game install (it holds `eqgame.exe`, `spells_us.txt` or `eqclient.ini`): Hailstone unpacked into the
  RoF2 install would otherwise write into the asset folder, which it only reads.

The asset picker refuses an asset folder that holds the Hailstone folder (`data_folder_conflict`), for the same
reason. A build made with the CMake option `HAILSTONE_ALLOW_IN_EQ_FOLDER=ON` (off by default) accepts one place
inside it: a folder named `hailstone` (any case) directly in the asset folder, `<EverQuest>/hailstone/`, where a
server's own patcher may install Hailstone. Writing there changes none of the install's files, and Hailstone never
reads that folder as game files: the asset folder is indexed whole, at every depth, except a top-level `hailstone`
folder (`GameDir::kOwnFolder`; a server's `assets` overlay is indexed whole). Anywhere else inside the asset folder
(`maps/`, `uifiles/`, `Resources/`, a deeper `hailstone`, another name) holds or may hold files the game reads, and
stays refused, as does the asset folder itself, in every build. Such a build also starts on the folder that holds a
Hailstone folder named `hailstone` (`../`) when it is an EverQuest install and no saved folder works (a first start,
or a moved install); that default is never saved.

The one exception is a macOS app bundle (`.app/Contents/`): replacing the app would delete the data and writing there
breaks its signature, so a bundle keeps its data in `~/Library/Application Support/hailstone`. A dev build's
`build/hailstone/` is portable like any other copy, so deleting the build folder deletes its servers and settings; set
`HAILSTONE_DATA_DIR` to keep them elsewhere.

`hailstone.ini`, `server.ini` and the characters' files are written through a temporary file renamed over the old
one, so a crash never leaves half a file. The temporary file is synced to the disk before the rename, and the rename
after it (`core/atomic_file.h`, `platform/durable_file.h`: fsync of the file and its folder; on Windows
FlushFileBuffers and MOVEFILE_WRITE_THROUGH), so a power cut leaves the old file or the new one, not an empty one.
Each save waits for the disk that way (a few milliseconds on an SSD, more on a hard disk), so nothing saves every
frame: the UI's and the hot buttons' settings are saved on a change or at most every few seconds. Names that reach these files from a server (a world's or a character's
name, kept as a `char:<name>@<world>` key in `server.ini`) cannot add lines of their own: in a key a line break or
`=` becomes `_` (also a leading `;`, `#` or `[`), and in a section name a line break or `]` (`core/ini_file.h`). A
world or character name is compared with the saved list in that form, so such a world keeps one set of characters.
A save that fails is logged; on Windows, where the replace is refused while another program has the file open, it
is tried a few more times first. Two clients may share `hailstone.ini`: when the file changed since a client read
it, that client writes only the keys it changed over the other's (`core/config.h`).

### Shared archive memory

Several clients started from the same Hailstone data folder can share immutable archive backing and classic
character animation bytes in RAM. Add these
flat keys to the main `hailstone.ini`, then restart each client:

```ini
assets.shared_memory=auto
assets.shared_memory_cache_mb=1024
```

The snapshot cache is experimental and off by default (`assets.shared_memory=off`). With it off, the normal archive
policy still maps base/classic game files read-only and keeps replaceable server-overlay archives in owned memory.
The disk budget defaults to 1024 MiB, with a maximum of 16384 MiB. With `auto` selected, a zero budget uses owned
private fallback rather than the normal cache-off mapping policy. These settings apply to every character and are
read at start-up. The cache stays under `data_dir()/cache/assets/v1/pfs`; separate portable data folders have separate
caches. Different servers' replacements are distinguished by content, including edits that preserve file size and
timestamp. The game install is only read. Cached bytes are local derivatives and must not be redistributed.

Every client maps the same completed snapshot read-only, retaining a use lease until the last reader finishes.
Published files are never rewritten. New contents create a new snapshot, so an external patcher's changes to the
source cannot truncate an existing mapping. Cache copying/verification uses bounded buffers. On a miss, a client
waits at most 250 ms for another publisher before loading privately. Source hashing, snapshots and parsing run
where the existing archive load runs; enabling this can add I/O and CPU work during loading.

The disk budget counts finished snapshots and abandoned temporary files. When more room is needed, old snapshots
are removed only if no client holds any mapping/build lease. While other clients keep the cache in use, a full
cache loads new archives privately. An unavailable cache, unsupported filesystem, unsafe permissions/links, corrupt
entry or mapping failure also uses private loading. A corrupt entry is removed only during exclusive trimming;
it is never replaced underneath another reader. Stop all clients before manually deleting cache contents, and
never remove or replace its `use.lock` or `publish.lock` files while a client is running.

Character WLD members are also cached as immutable inflated snapshots. Their packed animation keys are read from
those bytes on demand, including the clips and animation aliases of loaded models, instead of retaining private
expanded key arrays in each client. Decoding preserves the normal pose conversion and borrowed-bone scaling.
Character WLDs retain a stable validated resource index and decode full payloads when needed. The existing
process-wide 128 MiB LRU retains released decoded payloads; that budget is separate from the snapshot disk budget.
Mapped animation owners remain valid when a decoded payload is evicted.

`AnimationTrack` copies and aliases share keys within a process, detach on builder mutation and compact constant
tracks without changing their logical length or timing. Mapped tracks keep encoded backing through that
compaction; EQG keys keep the owned copy-on-write path. WLD fragment storage and name indexes also use less private
memory in both loading modes. Borrowed lookup names belong to stable deferred resource-index entries, or retained
fragment names for eagerly parsed WLDs, so decoded-payload eviction cannot invalidate the tables.

Windows uses read-only file mappings; Linux and macOS use read-only shared POSIX mappings of the same cache files.
This does not require a named shared-memory segment, `/dev/shm` configuration or administrator privileges. Native
macOS execution and Linux live gameplay remain validation gaps; see the research page for the tested scope.

The performance window (Ctrl+Shift+P) shows shared asset backing and cache hit/build/fallback counts. Mapped bytes
are addressable backing, not measured resident physical memory. Each process's resident figure may include the
same shared pages. With CPU sharing alone, model objects and geometry, collision, other inflated members, texture
uploads and Vulkan allocations still belong to each process. The separate experimental Windows
[GPU texture broker](graphics.md#developer-quickstart-on-windows) has its own opt-in, usage and validation limits. See the
[shared asset memory design](research/shared-asset-memory.md) for the next stages and measurement limits.

### A character's settings

From the entry into the world until it leaves (camp, logout, a lost connection), a character plays with its own
client settings: `servers/<profile>/characters/<world>/<character>/hailstone.ini` (`Config::attach_character`,
`ClientApp::use_character_settings`). The first time a character enters, that file is made from `hailstone.ini`'s keys
as they are then; after that it is the character's, and a change to `hailstone.ini` (on the login screen, or by another
character's client) does not reach it. While the character plays, every setting it changes goes to its file, so
clients run at once can play with different graphics, frame caps, sound, keys and so on. When it leaves, the client
takes `hailstone.ini`'s settings again and applies them.

Some keys stay in `hailstone.ini` whoever plays (`Config::is_shared_key`): the client's own (`assets.*`, `addons.*`,
`update.*`, `news.*`, `browser.*`, `plugins.*`, `scripts.*`), the login screen's (`login.*`, `audio.pregame_music`),
the window's place and mode (`window.*`, `display.window_mode`, `display.resolution`: one window, so it does not jump
when a character enters), and what is taken only at start (`display.msaa`, `ui.name`, `ui.scale`, `ui.cpp_windows`,
`ui.dev_windows`, `ui.perf_*`, `game.fixed_step`, `audio.enabled`, `audio.soundfont`, `audio.mix_ahead`,
`nav.threads`, `nav.prebuild`). The UI packs' script settings (`ui/settings/<pack>.txt`) stay shared too; the window
layout was the character's already (`ui-<pack>.txt`).

Scripts and plugins are also looked for in the install's own `scripts/` and `plugins/` when the data folder is
elsewhere (a macOS app bundle, `HAILSTONE_DATA_DIR`), so an install can ship some.

## The asset folder

The client asks for the game install (RoF2, or a current one) at the first start (`ui/asset_picker.h`). The picker shows when `hailstone.ini`
has no `assets.path` that passes the check, or when `assets.pick=1` asks for it again (the Options window's Display
tab, "Change Asset Folder"). It has a folder field, a "Choose folder..." button (the system's folder dialog,
`SDL_ShowOpenFolderDialog`) and the folders used before (`assets.recent`, newest first, at most 8). Continue is
enabled only when the check passes, and the choice is saved as `assets.path`. `--game-dir DIR` or `EQ_GAME_DIR`
override all of this and skip the picker (`core/asset_folder_choice.h`). The zone and model viewers have no picker:
they use `--game-dir`, `EQ_GAME_DIR` or the saved `assets.path`. "Change Asset Folder" in the game shows the picker
when the player is back on the login screen (camped out), or at once when the Options window is used there; its
Cancel keeps the folder. The login screen's Game files row does the same without the Options window: a list of the
folder in use and `assets.recent`'s, each marked RoF2, Live or Not usable (checked when the list is made), where
picking one switches to it at once (`ClientApp::switch_asset_folder`), and Browse... opens the picker. With
`--game-dir` or `EQ_GAME_DIR` the row only shows the folder. The server's own files have their row below it,
Server files. The new folder is used at once: the game files read before the login are read again, as
for a server's `assets/` (`ClientApp::reload_server_files`), and nothing else holds the old folder's files there
(no connection, no zone). Closing the window in the picker keeps the request for the next start. With `--game-dir` or
`EQ_GAME_DIR` the picker does not show. The folder is saved absolute, so a typed relative one means the same folder from any
start directory. A folder that holds the data folder (Hailstone placed inside the RoF2 install, below its own
folder) is refused, since Hailstone would write into it.

The check (`core/asset_folder.h`) names what is missing. Required: `spells_us.txt`, `eqstr_us.txt`, `dbstr_us.txt`,
`eqlsstr_us.txt`, `racedata.txt`, `soundassets.txt`, `global_chr.s3d`, `sky.s3d`, at least one `snd*.pfs`,
`Resources/SkillCaps.txt`, `Resources/BaseData.txt`, `Resources/GlobalLoad.txt` and
`uifiles/default/EQUI_Animations.xml`. Only RoF2 has `Resources/OnDemandResources.txt`,
`Resources/SpellRequirementAssociations.txt`, `Resources/PlayerCustomization.txt` and `spellsnew.edd`, so without
them the folder is an older client or incomplete. A missing game executable is only a warning: it is read for the login
checksum and for the per-zone sound room table (docs/formats/audio.md, Room), and only read; without it every zone
uses the default rooms. A current install passes the same check.

### RoF2 or live

Either way the files are EverQuest's, Daybreak Game Company's work, which Hailstone does not ship. We highly
encourage anyone who plays on them to subscribe to [EverQuest](https://www.everquest.com) and support the game.

The asset folder may be a RoF2 install or a current live one, so that a player needs no RoF2 files. `is_live_asset_folder` tells them apart by `spells_us_str.txt`, which only the
live client ships. The archive formats are the same versions in both (PFS, WLD v15500, EQGM/EQGT/EQGL/EQGA/EQGS); live
adds BC7 textures under a DX10 header, which the texture loader already reads. What the live files change:

- **Files the server must match.** `spells_us.txt`, `eqstr_us.txt`, `dbstr_us.txt`, `Resources/SkillCaps.txt`,
  `Resources/BaseData.txt` and `Resources/GlobalLoad.txt` should always come from the server's `assets/`
  (`server_matched_files`): live's follow the live game (`eqstr_us.txt` lacks 1185 ids a RoF2 server sends and
  rewords 598 others; spell values, skill caps and base stats changed; `GlobalLoad.txt` loads other archives, and a
  server's own lists its custom ones). Without them the game files' own are read, and logging in warns
  (`missing_server_files`). Live's `spells_us.txt` has 166 fields a line instead of 237, its five texts in
  `spells_us_str.txt` and its effects in the last field (`slot|spa|base|base2|formula|max`, joined by `$`):
  `spells_text_from_live` turns it into RoF2's layout (the column map in `game/spells.cpp`, worked out from the 34420
  spells both files have under the same id and name; over those, cast time, mana, target, icon, class levels and
  texts agree for 96 to 99 % and the effects for 93 %, the rest being live's changes). Live has no book or gem icon,
  `player_1`, activated, global group or deletable columns (0; deletable 1, as every RoF2 spell).
- **The EQ menu.** Live's `uifiles/default/CascadeMenu.txt` groups and orders the windows differently from RoF2's,
  so the game folder's file is not read: the menu is Hailstone's own layout (`CascadeMenu::hailstone_layout`, a
  deliberate difference from the original, which reads the file), the same with either folder. It is in
  alphabetical order: the submenus first, then the other rows, then Help, Report a Bug and Camp & Quit at the bottom;
  each submenu's rows alphabetical (Mercenaries is under Players). A server that wants its own sends
  `uifiles/default/CascadeMenu.txt` with its client files (in its `assets/`), which lays the menu out instead, in the
  file's order.
- **Zones live replaced.** Live has only the revamped versions of commons, ecommons, misty, sro, oasis, highpass,
  tox, oot, lavastorm, bazaar, highpasshold, barter and freeporttemple, and changed everfrost's geometry and objects.
  A RoF2 server places its spawns, paths and doors on the old geometry. A **classic folder** fills them in: another
  install with the old files, such as a classic-era client (`GameDir::set_classic`, below the base; `assets.classic` in
  `hailstone.ini`, or `--classic-dir`). Compared
  with a classic-era client, its copies of commons, misty, sro, oasis, highpass, tox, oot and everfrost have RoF2's
  geometry, collision tree, regions and object placements; ecommons and lavastorm too, except their zone lines,
  which name the destination (zone and place) where RoF2's name a server zone point (ecommons also moves one wooden
  support by 2 units). With a live base, everfrost's and lavastorm's files come from the classic folder before the
  base's (`classic_preferred_file`). That client lacks bazaar.s3d, highpasshold.s3d, barter.eqg and freeporttemple.eqg:
  only RoF2 has them, so a server's `assets/` brings them. Live also has new `.eqg` zones under the RoF2 names
  arena, bazaar, highpasshold, lavastorm and nektulos: those load as `.s3d` when there is one. With a live base and
  none, the `.eqg` loads only when the server's own files bring it, or when the safe point the server sends at
  zone-in (OP_NewZone) stands on its floor: a floor at most 10 units under it (`is_revamped_zone_name`,
  `game/revamped_zone.h`). Otherwise the zone is refused rather than loaded as the revamp. A stock server can run
  either geometry under the one name: PEQ's Bazaar version 0 (expansions 3 to 8) is the old zone, version 1
  (expansion 9 on) the revamp, and the zone-in packets do not say which. The original client loads the `.eqg` by name
  alone (`CDisplay::StartWorldDisplay` tries `.EQG` first), so this test is a deliberate difference. Measured on PEQ's
  rows against the live files: Bazaar and Lava Storm version 1 and High Pass Hold (one row) pass, and version 0's
  safe points have no floor under them. The loaded zone and its collision mesh are kept, not built again. A neighbour
  zone loaded ahead has no safe point yet and keeps the refusal. Live's arena.s3d and nektulos.s3d still win,
  although PEQ runs the revamp there (docs/unknowns.md). Live's chapterhouse, clz, crescent, relic, tutoriala and tutorialb differ from RoF2's
  too (the classic-era client's clz and tutorials as well) and may not match the server's maps.
- **`racedata.txt`.** Live has a line per race and gender: race, gender (0, 1, or 2 for both), the 44 sound ids in
  index order, the male and female heights, then the model code at field 50. `parse_race_sounds` and
  `parse_height_tables` read both layouts.
- **Models.** Live dropped the zones' own `<zone>_chr.s3d`; its `<zone>_chr.txt` lists name per-model archives
  (`kob,kob_chr` for `kob_chr.s3d`), where RoF2's rule takes a name that shares the code's first three letters for an
  EQG: `chr_txt_entry_is_s3d` takes it for an S3D when that file exists. Live also renamed models (Kael's frost giants
  are `FSG`, `GAM` in RoF2): when the client's code for a race has no model, `ActorRenderer::choose_model` tries the
  live `racedata.txt`'s code (`HeightTables::racedata_code`). Heights, dressing and animations still go by the
  client's code. Live keeps most models in their own `<code>_chr.s3d` and its `GlobalLoad.txt` loads few of them
  (`bea_chr` where RoF2's has `pbr_chr`, so East Commonlands' black bears, `PBR`, were in no archive the zone opened
  and were drawn as humans): a model no global, zone or EQG archive has is read from `<code>_chr.s3d` on first use
  (`ModelArchiveStore`, shared by the zones' libraries like the EQG store), dressed and textured from that archive.
  The racedata.txt fallback tries the spawn's gender, then 2 (both), 0 and 1. A sweep of the race table's codes over
  every archive leaves, with live and a classic folder, only HSN (race 216, RoF2's
  `ponightmare_chr`), MCR (race 656, `mcr_chr`) and race 72's PRE (live's boats are SHIP in `ship1`-`ship3_chr`)
  without a model; a server ships those archives.
- **Hero's forge colours.** Live ships each colour of a piece as a base mesh and a texture layer, not its own `.mod`
  (`genericchainblue.eqg` holds only textures and a `.lay`): `Resources/Layers/*.txt` rows `IT27104^IT19814^
  GENERIC_CHAIN_BLUE_V1` make RoF2's IT27104 the mesh IT19814 with that layer's diffuse texture.
  `EqgCharacterStore::part_mesh` builds such a piece when no `.mod` of its name is found (the layer from the
  on-demand index, else `<layer>.eqg`). Without it a piece that hides the body's own mesh (leggings) left a gap.
  Live lacks about 3,300 of RoF2's pieces altogether, nearly all of the `mayhem*` sets.

All files are read through `GameDir` (`core/game_dir.h`), which looks them up in layers: the active server's
`servers/<profile>/assets/` first, then the asset folder. A file in the server layer replaces the asset folder's file
of the same name as a whole, so an archive (`.s3d`, `.eqg`, `.pfs`) replaces the whole archive. A file that only the
server layer has, such as a new zone, is found as well. Texture packs (`textures/<pack>/`) and the server's `maps/`
are Hailstone's own files on top of that.

### Maps

The in-game map's files (`<zone>.txt`, `<zone>_1.txt` .. `_3.txt`, docs/dev-ui.md) are looked for in four places
(`game/map_files.h`), the first that has any of a zone's layers giving all four:

1. `servers/<profile>/maps/`: the player's edits on this server (the map toolbar saves here), or maps put there by hand
   for this server only.
2. `servers/<profile>/assets/maps/`: the server's maps, sent with its client files (`client_files/maps/` or
   `client_files/assets/maps/`) or added by hand. Under `[assets] player = off` only the ones the server sent count.
3. `maps/` in the Hailstone folder: the player's maps for every server.
4. The asset folder's `maps/`.

So a server's map replaces the player's global one, and an edit made on a server replaces both there. A zone's layers
never mix places: a server that sends only `<zone>.txt` shows no labels from a global `<zone>_1.txt` drawn for other
geometry. RoF2 reads one maps folder file by file; this is a deliberate difference. A maps directory (a map pack:
the map window's combo, `[Main] MapSkin`) is a subfolder of `maps/` in any of the four; the chosen one is looked for
in all four first, then the plain folders. The combo lists the subfolders of all four. The Options window picks it
too (Display > Interface > Map Pack in the modern pack, Interface > Map pack in the gamepad pack), and it is kept per
character in the character's `UI.ini` with the map window's other settings. The list is read again each time
Options starts showing it, so a pack dropped into `maps/` during play shows up without a restart.

The layer files are named from the zone's short name, which the server sends (OP_NewZone). A name that is not
letters, digits and `_` (stock short names are lower case alphanumerics and `_`), or that is a Windows device name,
is not used: the connection drops it at OP_NewZone (the zone then loads without scenery), and the map reads and
writes no file for it (`core/zone_name.h`), so a hostile server's `../` or absolute name cannot reach files outside
these folders.

## Servers

The login screen lists the saved servers (`core/server_profile.h`, `ui/dev_screens.h`). The last one used
(`login.profile`) is picked; New, Save and Delete edit the list. A server has a name, an address and port, and the username when "Remember username" is ticked.
The password is never saved. The folder id is made once from the name, so renaming a server keeps its folder. The
Assets row shows the server's `assets/` folder, with a button that opens it. The server's files come by themselves
(below); the player may add others by hand.

### Server files

A server owner puts the files the server needs in `client_files/`, beside `eqemu_config.json`: `spells_us.txt`,
`eqstr_us.txt`, `dbstr_us.txt`, `Resources/SkillCaps.txt`, `Resources/BaseData.txt` and `Resources/GlobalLoad.txt`,
changed or new zones
(`.s3d`, `.eqg`, `.zon`, `.emt`), models, sounds, anything under the paths the client reads, either at the top or
under `assets/`: both land in `servers/<profile>/assets/`. Beside them go `hailstone.cfg` and the folders
`scripts/` and `plugins/` (docs/plugins.md), which land in `servers/<profile>/` under the same names. Hailstone's
server script (server/README.md) sends them; the owner needs no web server.

At the first zone-in of a login the client asks the zone for the list (path, size, SHA-256) and holds the zone-in
while it downloads what `servers/<profile>/` lacks (`net/server_files.h`): it does not ask for the zone's
spawns (`OP_ReqClientSpawn`) nor load the zone, and the zone keeps the character connecting meanwhile. The loading
screen says **Downloading server files** with the files and megabytes done and the speed, so a long first zone-in is
not taken for a hang. The files come into `.download/` a piece at a time (32 KB, 32 pieces asked ahead), each checked
against its hash (as its pieces come; a file with a piece asked for again is read back from `.download/` 16 MB a
frame once all have come, however many pieces of other files the frame takes in), and only when all have come are they moved into place; the files a former list brought that
the server no longer lists are deleted then. A broken-off download keeps the files that came complete for the next
try, and leaves the folder as it was. Once something in `assets/` changed the client reads its files again (`GameDir::rescan`, the
connection's strings, spells and race data, then what `reload_server_files` reads) before the zone loads.

What it trusts: paths are relative, without `..`, parts starting with `.` or ending in `.` or a space (Windows drops
those, so `evil.exe.` would land as `evil.exe`), Windows device names (`CON`, `PRN`, `AUX`, `NUL`, `CONIN$`,
`CONOUT$`, `COM1`-`COM9`, `LPT1`-`LPT9` and `COM`/`LPT` with a superscript 1 to 3, in any case and with any extension,
refused on every platform; `core/windows_names.h`, which the zip reader of updates and add-ons applies too), and programs and scripts (`.exe`,
`.dll`, `.so`, `.lua` ...) are refused, except Lua and data in `scripts/` and this platform's libraries and data in
`plugins/` (other platforms' are skipped). What those may do is the server's `hailstone.cfg`'s (docs/plugins.md).
How much it may write is bounded too, since a size in the list is the server's word (up to 4 GB a file, 65536
files): a file over `assets.server_files_max_file_mb` (hailstone.ini, default 2048) is refused like an unsafe name,
and when what is to come adds up to more than `assets.server_files_max_total_mb` (default 16384) the sync fails
before anything is written, leaving the old files (`ServerFileSync::Limits`). These are Hailstone's bounds, far above
the largest server measured (below). Files the player added by hand stay unless the server lists the same name; one that matches
the server's hash is kept as is. Checking those (a file of the right size that `server_files.txt` does not vouch for,
or one left complete in `.download/`) means hashing it: that is done 16 MB a frame, so a folder of large files copied
in by hand keeps the loading screen drawing (**Checking the server's files**, with the files done) instead of freezing it. A server without the script does not answer, and after 5 s (30 s for a server that
sent files before, whose zone may be hashing new ones) the zone-in goes on with what is there.

On a live asset folder a RoF2 server's `client_files/` needs the RoF2 files live lacks or changed (above): the five
files the server must match, the classic zones live replaced, the zones whose geometry differs, and RoF2's per-zone
`_chr.s3d` model archives. Measured against a dev server: 612 files, 1.1 GB, about 16 MB/s on
one machine (about 70 s).

Speed: the zone connection already compresses every packet (EQEmu's `Network:CompressZoneStream`, on by default),
and the archives are compressed inside, so compressing the files again gains about 5 %. What limits the speed is
how many pieces are asked ahead and how soon: during the download the zone session sends its acks and requests at
once instead of holding them 150 ms to combine them (`Session::set_no_hold`; 2.6 MB/s with the hold, 8.4 MB/s
without), 32 pieces are asked ahead, and the socket's receive buffer is 2 MiB so their burst is not dropped.
A piece that does not come is asked for again (`ServerFileSync::kChunkRetry`, `kChunkRetries`): at once when one
asked after it has come (the script answers in order, so it skipped it, as an older script over its limit did), or
after 3 s with nothing; after 5 requests for the same piece the sync fails and the zone-in goes on with the old
files. When the server's files change during a download (its list id moves on), the script answers each piece asked
for with a stale error: the first one lists the files again and the download goes on with the new list; the others,
for the old list, are left (more than three such restarts in one sync fail it).

The OS window's title is the picked server's name ("Hailstone" before one is picked), and in the world
`<server> - <character>` (`client_window_title`). A character with several classes (multiclass, set by the
server's extension) shows them: `<server> - <character> [War/Clr]`.

Logging in applies the server's layer to the `GameDir` and, when the layers change (this server's `assets/` has
files, or the files read last came from one that had), reads again what was loaded before the login and kept: the
login strings, the stat tables, the UI art, the audio system (its `snd*.pfs` archives, soundassets, mp3index,
racedata) and sound events, the character models' global archives, the world objects' archives and on-demand index,
the spell and item icons, the game menu and the spell particles (`ClientApp::reload_server_files`). Zones read their
files as they load.

The server list shows each world's name and whether it can be joined, never its player count: no UI pack is given
the count (`eq.ServerSelect`). A test login's transient profile keeps a `worlds.ini` (the worlds listed last) as
well, without a `server.ini`, so it stays out of the list.

Delete moves the server's folder to `servers/.deleted/<id>-<date>-<time>/` in one rename: nothing is half deleted
when a file is held open, and it can be brought back by moving it back.

`--server <id or name>` picks a saved server; with `--host`/`--port` as well, those give the address for this run (a
test login saves nothing). `--user` with `--host` and `--port` logs in to a transient server that is never saved; its
characters' files go to the folder made from the world's name (`profile_id_for_world`), or to
`servers/_transient-<id>/` when a saved profile has that id for another address (`transient_profile_id`). The
`--ui-demo login` screen works on copies of the saved servers in a scratch folder and saves nothing.

One login server can list several worlds, and a character's name is only unique on its world, so each character's
files are in `servers/<profile>/characters/<world>/<name>/` (`core/character_files.h`). `<world>` is the world's name
as the login server lists it, made a folder name as a character's name is (`world_folder_name`: characters other
than letters, digits, space, `-`, `_` and `.` become `_`), so two characters of one name on two worlds of the same
login have files of their own. The maps stay per server (`servers/<profile>/maps/`). The files keep the RoF2 client's
formats:

| File | What |
|---|---|
| `UI.ini` | window places and the chat windows |
| `hotbuttons.ini` | hot buttons and socials |
| `BZR.ini` | trader prices |
| `AddressBook.txt` | the mail address book |
| `IgnoreList.txt` | the mail ignore list (the address book's Ignore button), the same format |
| `GuildNotes.txt` | the guild window's personal notes ("%08X %s %s" lines) |
| `chat.ini` | chat filters and user colours |
| `options.ini` | the game options (Options window, /lootnodrop, /trackplayers, /log ...) |
| `<name>.txt` | the chat log (/log; `eqlog` unless named). Kept open while logging, each line written through at once; `/log off` closes it |

The asset folder's `defaults.ini`, `eqclient.ini` and `UI_<name>_<server>.ini` are read as the defaults under these
files and never written; so are its `userdata/AddressBook.txt` and `userdata/IgnoreList.txt`, the books a character
starts from when it has none.

## Builds and installs

A build makes the same folder at `build/hailstone/`: the binary, `shaders/`, `ui/`, `icons/`, `pictures/`, `CHANGELOG.md`, `browser/` (with `HAILSTONE_BROWSER`),
the runtime DLLs on Windows, `libMoltenVK.dylib` on macOS, and
the example plugin in `plugins/`. The server plugins are kept in repositories of their own and reach players as
add-ons (docs/addons.md): the NMS server's (projecthailstone/hailstone-nms) and the Project Triune server's (private).
Cloned into `plugins/nms/` or `plugins/projecttriune/` (which this repository ignores), each is built into the
same folder under `build/hailstone/plugins/`, and its tests join `hailstone_tests`. Run it as
`build/hailstone/hailstone`. Since that folder is writable, a dev build
keeps its `hailstone.ini`, logs and servers beside the binary. The tools (`pfsdump`, `hailstone_check` ...) and the tests
stay in `build/`.

`cmake --install build --prefix <dir>` writes the shipped parts only: `<dir>/hailstone(.exe)`, `shaders/`, `ui/`, `icons/`, `pictures/`, `CHANGELOG.md`,
`browser/` (about 390 MB on Linux with `libcef.so` stripped), and
the shared libraries the build made (SDL3 when it was fetched), which the binary finds through an rpath of its own
folder. On macOS the install also has `libMoltenVK.dylib` and its `MoltenVK-LICENSE.txt`: macOS has no Vulkan of its
own, so the client ships MoltenVK and loads it by its path beside the binary (`platform::bundled_vulkan_library`). The example plugin is a separate component (`--component examples`), and so are the server plugins when
checked out (`--component nms`, `--component projecttriune`). A release includes no server plugin. An install over the same folder
replaces only the shipped parts (the binary, `shaders/`, the shipped UI packs ...); the player's files beside them stay. The updater
(docs/updater.md) installs a release the same way, from zips that hold only the shipped parts, and refuses any
package that would write one of the player's files or folders (`app::is_player_path`: `hailstone.ini`, `servers/`,
`logs/`, `maps/`, `scripts/`, `textures/`, `ui/settings/`, `ui/loadingbackgrounds/` ...). A folder that has a
CMakeCache.txt beside it (a dev build's `build/hailstone/`) never updates itself.

CI builds nothing but macOS (docs/ci.md); a release's folder is installed and packed locally, the macOS one from
the macOS workflow's artifact (docs/updater.md "Making a
release"), without the server plugins.

## The first start

Every start makes the data folder and its empty folders when they are missing (`platform::create_data_dirs()`):
`ui/settings/`, `plugins/`, `scripts/config/`, `textures/default/`, `maps/`, `servers/`, `logs/`, `screenshots/`,
`bug-reports/` and `cache/`. So after the first start the player can see where plugins, scripts, texture packs and
maps go without reading this. Folders and files already there are left alone; a folder that cannot be made (a file
of that name is in the way, or the disk refuses) is named in the log, and the start goes on. Each writer still makes
its own folder before it writes, so deleting one while Hailstone runs is harmless.

Older builds kept their settings in a per-user folder (`~/.config/hailstone`, earlier `~/.config/nms-client`).
Hailstone no longer reads it or copies anything from it: a new Hailstone folder starts empty. That old folder can be
deleted.

Builds of this layout before the files were kept per world wrote them to `servers/<profile>/characters/<name>/`.
When a character enters a world and no world's folder of that name (`characters/*/<name>/`) exists yet, the files in
that older folder are copied to `characters/<world>/<name>/`, once (`adopt_unsorted_character_files`); the older
folder is left as it was. The first world a character of that name enters gets them: a character of the same name on
another world starts with the defaults. The files are copied into a sibling folder and renamed into place, so a copy
that fails leaves nothing behind and the next entry tries again.

The chat filters and colours that older builds wrote into the asset folder's `eqclient.ini` stay in that file, which
`chat.ini` is read over, so they still apply until changed.

## Screenshots, the pipeline cache, crash and bug reports

The screenshot key saves the next frame as `screenshots/hailstone-<YYYYMMDD>-<HHMMSS>.png` (local time; `-2`, `-3`
... for more in the same second). It is the key map's Screen Capture command (`SCREENCAP`), which the default keys leave unbound except in the third set (NPAD -), so Print Screen takes one too, on every screen, unless
the player gave that key to another command. `/screenshot` does the same. `--screenshot FILE` (the test runs) writes
FILE, a relative one under `screenshots/`.

The Vulkan pipelines are compiled through a pipeline cache read from `cache/pipelines.bin` when the device is made
and written back (through a temporary file) when the client closes (`render/pipeline_cache.h`). The file is used only
when it is whole (its size and hash match) and the same GPU and driver made it (the vendor, device, driver version
and the driver's cache UUID); otherwise the cache starts empty and the file is replaced at the end of the run.

A crash (a fatal signal on Linux and macOS, an unhandled exception on Windows, or an unhandled C++ exception) writes
`logs/crash-<YYYYMMDD>-<HHMMSS>.txt` (UTC) with the build (`core/build_info.h`: the version and commit), the time,
what happened, the log's path and its last 16 lines, then the stack where glibc has one (`addr2line -e hailstone
<+offset>` names a frame of a build with symbols); on Windows a minidump, `crash-<...>.dmp`, goes beside it
(`platform/crash_log.h`). The minidump holds the threads' call stacks and registers and the module list, enough for a
symbolised stack, and no secret, since the dump goes into bug reports unchanged: no heap, so not the memory the stacks
point at (the password field, the session key, a plugin's `auth_key`), and of the stacks only the pointers a stack
trace needs (`MiniDumpFilterMemory`), so no bytes a frame copied either; the locals' values are lost with them. The
client itself is on the heap, and the password leaves its fields (overwritten) once the login server has taken it: a
saved one is read from the keyring again when the login screen comes back. The log itself is flushed line by line, so it holds the rest. A stack overflow is reported
on the main thread and on the threads that make a `CrashStack` (the network session's, the sound loader's, navigation's,
the add-on manager's, the updater's, the neighbour zones' and the appearances' workers, `core/latest_jobs.h`). After
the report the handler that was there before runs: the default one (a core dump, the crash exit status), a sanitizer's
report, or on Windows the C++ runtime's, which turns an uncaught C++ exception into `std::terminate` so its message is
in the report.

An error that ends the run without a crash (no Vulkan driver, no GPU with what Hailstone needs, no window, missing
shaders) is logged and shown in a dialog (`app/startup_error.h`), since a client started by a double click has no
console: what kind of problem it is and what usually fixes it, the error itself and the log's path. When no GPU
qualifies, the error names each GPU and what it lacks (`render/gpu_requirements.h`). The test runs (`--frames`,
`--screenshot`) show no dialog.

Bug reports are zips for the player to post in the bug report forum (`app/bug_report.h`), in `bug-reports/`. The bug
report window (`/bugreport`) hides for the frame it opens on, so its screenshot shows what the player saw; Send gives the
server its copy (`OP_Bug`, as the original) and saves `hailstone-bug-<YYYYMMDD>-<HHMMSS>.zip` (local time) on a
thread of its own, so a large log does not stall the game (the window says it is being saved), after which the window
offers Open Folder and Copy Summary and says what the file holds and what was taken out. The start after a crash bundles the crash report into
`hailstone-crash-<...>.zip` with the log of the run that crashed (`<name>.1.log` once this start moved it) and shows
the crash report window (over the login screen too), where a note can be added. A zip holds `report.txt` (the player's
words, the build, system, GPU and driver, display, UI, server, world, character, zone, place and target),
`summary.md` (a forum post under Discord's 2000 characters: Copy Summary), the crash report, the logs (this run's and
the last), `hailstone.ini` and the UI's settings (keys naming a password, token, secret or session blanked), the
window's screenshot (at most 1920 wide) and the player's own from the 15 minutes before. It stays under 9.5 MB, below
Discord's 10 MiB upload limit: what does not fit (the screenshots first, then a log is cut to its end) is named in
`report.txt`. Since the report is posted in public, every text in it (the logs, settings, crash report, `report.txt` and
`summary.md`, the player's words too) has the login account names saved for any server, and the one played, written
`<account>` (a whole word, any case), the home folder (`platform::home_dir()`) written `~`, and the home folder's
name, which on Linux is the system account name, written `<user>` where it is a folder name elsewhere
(`/run/media/<user>/`). The logs and the crash report also have the words of every chat line (`[info] chat: ...`:
tells, guild, group and channel chat, other players' words) taken out; the line itself stays, for its time and
order. A Windows `.dmp` is binary and goes in unchanged (it carries no heap and no stack data, see above); screenshots are not touched.

On Windows the programs carry a manifest that makes UTF-8 their ANSI code page (Windows 10 1903 and later; MSVC-style
linkers, `cmake/utf8.manifest`), so a data or install folder with any characters in its name works; the folder
choice itself reads the environment and the binary's folder as wide or UTF-8 text, so it works on older Windows too.
