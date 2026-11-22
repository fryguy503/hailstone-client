# The Hailstone folder

Hailstone keeps everything in one folder, the one that holds the binary. It is portable: copy the folder and the
settings, servers and characters' files go with it.

The game install the player picks (RoF2 or a current one; the asset folder) is only read, never written. Its
`eqclient.ini` and `defaults.ini` are defaults only: changes go to `hailstone.ini` or the character's files under
`servers/`.

```
hailstone/
  hailstone(.exe)            the binary
  shaders/                   shipped; an update replaces it
  icons/                     shipped: hailstone.png, the window and taskbar icon (hailstone-crest.png, the full crest;
                             on Windows the icon is built into hailstone.exe instead, cmake/windows/hailstone.ico)
  pictures/                  shipped: the default picture behind the pre-game and loading screens, shown when
                             neither the server nor ui/loadingbackgrounds/ (below) has any
  flora/                     shipped: Hailstone's flora rules for zones without flora of their own (docs/graphics.md
                             "Zone flora"): default.txt for every outdoor classic zone, <zone>.txt for one, the
                             shared parts and climate families they include; replaced
                             by an update
  CHANGELOG.md               shipped: the release notes; the login screen's What's new shows this version's part
  ui/<pack>/                 UI packs (modern, gamepad), replaced by an update
  ui-addons/<addon>/ui/<pack>/  UI add-ons' interfaces (docs/addons.md "UI packs"), the add-on manager's; an update
                             never writes here
  ui-data/<pack-id>/         player-owned shared presentation Lua modules; LoadData reads only the selected ID and
                             approved base IDs. Updates and add-on removal leave these files for later reinstalls
  ui/settings/<pack>.txt     the player's, made at start (not shipped): each UI pack's script settings shared by
                             every character, and the window layout a character starts from (no ui.ini, so not a
                             pack: a build or update leaves it alone; ui-settings/ before 2026-11, moved here at
                             start-up by move_old_ui_settings)
  ui/loadingbackgrounds/     the player's, made empty at start (not shipped): pictures (PNG, JPEG, BMP, TGA, DDS) for
                             the pre-game and loading screens, used unless the server's
                             servers/<profile>/assets/loadingbackgrounds/ has any (src/ui/screen_pictures.h); made beside
                             the UI packs, so under HAILSTONE_DATA_DIR it stays in the install's ui/
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
  upscalers/<name>/          upscaler add-ons, each a temporal upscaler (docs/addons.md "Upscalers"): <name>.so (.dll,
                             .dylib) and the runtime it loads; display.upscaler = <name> loads it at the start, from
                             a copy in cache/upscalers/. A build from source puts the example provider in
                             upscalers/example/
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
                             chat_history.txt (the chat windows' last lines, each its last 1000, put back at the
                             next entry into the world);
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
                             the viewers write viewer.log, a second client hailstone.<pid>.log (removed at a later
                             start, but for the newest 5 and those written in the last day). A run's file stops at
                             100 MiB with a last line saying so; stdout and stderr still get every line
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
  cache/                     safe to delete while no client runs: cache/plugins/ holds the loaded plugins' copies,
                             cache/upscalers/<name>-<key>/ the loaded upscaler's copy of its folder, cache/pipelines.bin
                             the compiled Vulkan pipelines, cache/navmesh/<server>/<zone>.navmesh the zones'
                             navigation meshes (docs/navigation.md), cache/terrain/<server>/<zone>.bin the land's
                             worn terrain round a zone (docs/graphics.md "Horizon hills"; the viewer's under
                             cache/terrain/viewer/). Up to 0.7 the neighbour zones, the world atlas and the distant
                             copies kept cache/horizon/, cache/atlas/ and cache/proxies/ here; nothing reads them
                             now, and they are not removed (delete them by hand, or the whole cache/)
    assets/v1/pfs/           the shared asset cache, when on ("Shared archive memory" below)
```

The paths come from `src/platform/paths.h`:

- `install_dir()` is the binary's folder; the shipped parts are read from it (`shaders_dir()`, `ui_packs_dir()`).
- `data_dir()` is where everything written goes: `install_dir()` itself.
- `HAILSTONE_DATA_DIR`, when set, names another folder. An EverQuest folder named there (or given to
  `platform::use_data_dir`) is refused at start as below (`platform::given_data_dir_problem`).

No settings are read from outside the folder (no `~/.config`, no `%APPDATA%`) or imported from another folder.

#### When the folder cannot hold the settings

`platform::choose_data_dir` decides, and the log's first lines say what it chose. Rather than write anywhere else, the
client stops at start with a message box asking for the folder to be moved somewhere the player owns
(`data_dir_problem()`) when:

- the folder is not writable (under Program Files or /usr);
- the folder is a game install (it holds `eqgame.exe`, `spells_us.txt` or `eqclient.ini`), which Hailstone must only
  read.

For the same reason the asset picker refuses an asset folder that holds the Hailstone folder (`data_folder_conflict`).

#### Inside the asset folder (HAILSTONE_ALLOW_IN_EQ_FOLDER)

A build with the CMake option `HAILSTONE_ALLOW_IN_EQ_FOLDER=ON` (off by default) accepts one place inside the asset
folder: `<EverQuest>/hailstone/` (any case), where a server's patcher may install Hailstone. Writing there changes none
of the install's files, and that top-level folder is never indexed as game files (`GameDir::kOwnFolder`).

- Anywhere else inside the asset folder (`maps/`, `uifiles/`, `Resources/`, a deeper `hailstone`, another name), and
  the asset folder itself, stays refused in every build.
- Such a build also starts on `../` when it is an EverQuest install and no saved folder works. That default is never
  saved.

#### macOS bundles and dev builds

A macOS app bundle (`.app/Contents/`) keeps its data in `~/Library/Application Support/hailstone`: replacing the app
would delete it, and writing there breaks the signature.

A dev build's `build/hailstone/` is portable too, so deleting the build folder deletes its servers and settings. Set
`HAILSTONE_DATA_DIR` to keep them elsewhere.

#### Safe saves

`hailstone.ini`, `server.ini` and the characters' files are written to a temporary file that is synced and renamed
over the old one (`core/atomic_file.h`, `platform/durable_file.h`), so a crash or power cut leaves the old file or the
new one. Each save waits for the disk, so nothing saves every frame: the UI's and hot buttons' settings save on a
change, at most every few seconds.

- A failed save is logged (on Windows, where an open file refuses the replace, after a few retries).
- Two clients may share `hailstone.ini`: when the file changed since a client read it, that client writes only the
  keys it changed (`core/config.h`).

Names from a server (a world's or character's name, kept as a `char:<name>@<world>` key in `server.ini`) cannot add
lines (`core/ini_file.h`): in a key, a line break or `=` becomes `_` (also a leading `;`, `#` or `[`); in a section
name, a line break or `]` does. The saved list is compared in that form.

### Shared archive memory

Clients started from the same data folder can share the bytes of some immutable assets in RAM. It is off by default;
to turn it on, add to the main `hailstone.ini` and restart each client:

```ini
assets.shared_memory=auto
assets.shared_memory_cache_mb=1024
```

The budget is the cache's disk space, 1 to 16384 MiB (default 1024); 0 turns the cache off. Both apply to every
character and are read at start. The cache is `data_dir()/cache/assets/v1/pfs`, so each data folder has its own.

What it shares:
- A server's own archives (its assets folder), otherwise read onto each client's heap, are copied into the cache and
  mapped. The game folder's archives are always mapped read-only, so the page cache shares them without it.
- Inflated character WLDs, whose animation keys are read from the mapping when sampled.

Model geometry, collision, other inflated members, textures and game state stay per client.

Cache files are named by their content's SHA-256, written once (0400 on POSIX) and never changed. A client checks a
file it did not write once before trusting it, and loads privately (as with the cache off) when the cache cannot
serve: unavailable, unsafe permissions or links, a damaged file, a full budget, or another client publishing for more
than 250 ms.

A client trims the cache to the budget (finished and abandoned temporary files) on first use and before an insertion
would pass it, least recently used first, skipping files it maps. On Windows a file another client maps stays; on
Linux and macOS its space returns when the readers let go. The cache can be deleted while no client runs.

The performance window (Ctrl+Shift+P) shows the mapped backing and the cache's hit, build, fallback and eviction
counts (mapped bytes are address space, not resident memory). Design and measurements:
[Shared asset memory](research/shared-asset-memory.md). The Windows experiment that shares GPU textures is in
[graphics.md](graphics.md#shared-gpu-textures-experimental-windows).

### A character's settings

From entering the world until leaving it (camp, logout, a lost connection), a character plays with its own
`servers/<profile>/characters/<world>/<character>/hailstone.ini` (`Config::attach_character`).

- Its first entry makes that file from `hailstone.ini`'s keys as they are then.
- After that it is the character's: changes to `hailstone.ini` do not reach it.
- Settings changed while it plays go to its file, so clients run at once can use different graphics, frame caps,
  sound, keys and so on.
- On leaving, the client applies `hailstone.ini`'s settings again.

#### Shared keys

Some keys stay in `hailstone.ini` whoever plays (`Config::is_shared_key`):

- the client's own: `assets.*`, `addons.*`, `update.*`, `news.*`, `browser.*`, `plugins.*`, `scripts.*`;
- the login screen's: `login.*`, `audio.pregame_music`;
- the window's place and mode: `window.*`, `display.window_mode`, `display.resolution` (one window, so it does not
  jump when a character enters);
- the computer's graphics tier: `display.gpu_tier`;
- what is taken only at start: `display.msaa`, `display.upscaler`, `ui.name`, `ui.scale`, `ui.cpp_windows`,
  `ui.dev_windows`, `ui.perf_*`, `game.fixed_step`, `audio.enabled`, `audio.soundfont`, `audio.mix_ahead`,
  `nav.threads`, `nav.prebuild`.

A shared key still in a character's file moves into `hailstone.ini` when the character enters, if that has none. The
UI packs' script settings (`ui/settings/<pack>.txt`) are shared too; the window layout is per character
(`ui-<pack>.txt`).

#### Quest preferences

The modern UI's quest state (tracked IDs, tracker visibility, collapsed quests, journal category and section:
`tasks.tracked.*`, `tasks.tracker.*`, `tasks.category`, `tasks.section.*`) is per character in `ui-modern.txt`, with
no shared fallback; Copy layout does not copy it. The tracker's position, size and lock are ordinary `hud.*` layout
keys.

#### Scripts and plugins in the install

When the data folder is elsewhere (a macOS app bundle, `HAILSTONE_DATA_DIR`), scripts and plugins are also looked for
in the install's own `scripts/` and `plugins/`, so an install can ship some.

## The asset folder

The client asks for the game install (RoF2, or a current one) at the first start (`src/ui/asset_picker.h`).

#### The picker

The picker shows when `hailstone.ini` has no `assets.path` that passes the check, or when `assets.pick=1` asks again
(Options > Display, "Change Asset Folder").

- It has a folder field, "Choose folder..." (`SDL_ShowOpenFolderDialog`) and the folders used before
  (`assets.recent`, newest first, at most 8).
- Continue needs the check to pass. The choice is saved as `assets.path`, absolute, so a typed relative folder means
  the same from any start directory.
- Closing the window in the picker keeps the request for the next start.

`--game-dir DIR` or `EQ_GAME_DIR` override all this and skip the picker (`core/asset_folder_choice.h`). The zone and
model viewers have no picker: they use those or the saved `assets.path`.

#### Changing the folder

"Change Asset Folder" shows the picker once the player is back on the login screen (at once when used there); Cancel
keeps the folder.

The login screen's Game files row lists the folder in use and `assets.recent`'s, each marked RoF2, Live or Not usable.
Picking one switches at once (`ClientApp::switch_asset_folder`), rereading the files loaded before the login as for a
server's `assets/` (`ClientApp::reload_server_files`); Browse... opens the picker. With `--game-dir` or `EQ_GAME_DIR`
the row only shows the folder. Server files has the row below.

#### The check

The check (`core/asset_folder.h`) names what is missing.

- **Required:** `spells_us.txt`, `eqstr_us.txt`, `dbstr_us.txt`, `eqlsstr_us.txt`, `racedata.txt`,
  `soundassets.txt`, `global_chr.s3d`, `sky.s3d`, at least one `snd*.pfs`, `Resources/SkillCaps.txt`,
  `Resources/BaseData.txt`, `Resources/GlobalLoad.txt` and `uifiles/default/EQUI_Animations.xml`.
- **Only RoF2 has** `Resources/OnDemandResources.txt`, `Resources/SpellRequirementAssociations.txt`,
  `Resources/PlayerCustomization.txt` and `spellsnew.edd`. Without them the folder is an older client or incomplete.
- **A missing game executable is only a warning.** It is only read, for the login checksum and the per-zone sound room
  table (docs/formats/audio.md, Room); without it every zone uses the default rooms.

A current install passes the same check.

### RoF2 or live

Either way the files are EverQuest's, Daybreak Game Company's work, which Hailstone does not ship. We highly
encourage anyone who plays on them to subscribe to [EverQuest](https://www.everquest.com) and support the game.

The asset folder may be a RoF2 install or a current live one, so a player needs no RoF2 files.
`is_live_asset_folder` tells them apart by `spells_us_str.txt`, which only live ships. The archive formats are the
same versions (PFS, WLD v15500, EQGM/EQGT/EQGL/EQGA/EQGS); live's BC7 textures under a DX10 header are read too.

What the live files change:

- **Files the server must match.** `spells_us.txt`, `eqstr_us.txt`, `dbstr_us.txt`, `Resources/SkillCaps.txt`,
  `Resources/BaseData.txt` and `Resources/GlobalLoad.txt` should always come from the server's `assets/`
  (`server_matched_files`). Live's follow the live game:
  - `eqstr_us.txt` lacks 1185 ids a RoF2 server sends and rewords 598 others;
  - spell values, skill caps and base stats changed;
  - `GlobalLoad.txt` loads other archives, and a server's own lists its custom ones.

  Without them the game files' own are read, and logging in warns (`missing_server_files`). Live's spell file is
  converted ([Live spells](#live-spells)).
- **The EQ menu.** Live's `uifiles/default/CascadeMenu.txt` groups the windows differently from RoF2's, so the game
  folder's file is not read. The menu is Hailstone's own layout (`CascadeMenu::hailstone_layout`) with either folder: a
  deliberate difference from the original, which reads the file.
  - Submenus first, then the other rows, then Help, Report a Bug and Camp & Quit at the bottom; each alphabetical
    (Mercenaries is under Players).
  - A server that sends its own `uifiles/default/CascadeMenu.txt` (in its `assets/`) gets the menu in the file's
    order.
- **Zones live replaced**, which a classic folder fills in ([below](#zones-live-replaced-detail)).
- **`racedata.txt`.** Live has a line per race and gender: race, gender (0, 1, or 2 for both), the 44 sound ids in
  index order, the male and female heights, then the model code at field 50. `parse_race_sounds` and
  `parse_height_tables` read both layouts.
- **Models**, renamed and moved to per-model archives ([Live models](#live-models)).
- **Hero's forge colours.** Live ships each colour of a piece as a base mesh and a texture layer, not its own `.mod`
  (`genericchainblue.eqg` holds only textures and a `.lay`). A `Resources/Layers/*.txt` row
  `IT27104^IT19814^GENERIC_CHAIN_BLUE_V1` makes RoF2's IT27104 the mesh IT19814 with that layer's diffuse texture;
  `EqgCharacterStore::part_mesh` builds such a piece when no `.mod` of its name is found. Live lacks about 3,300 of
  RoF2's pieces altogether, nearly all of the `mayhem*` sets.

#### Live spells

Live's `spells_us.txt` has 166 fields a line instead of 237. Its five texts are in `spells_us_str.txt` and its effects
in the last field (`slot|spa|base|base2|formula|max`, joined by `$`). `spells_text_from_live` turns it into RoF2's
layout (the column map in `game/spells.cpp`).

The map was fitted on the 34420 spells both files share by id and name: cast time, mana, target, icon, class levels
and texts agree for 96 to 99 %, the effects for 93 %, the rest being live's changes. Live has no book or gem icon,
`player_1`, activated, global group or deletable columns (0; deletable 1, as every RoF2 spell).

#### Zones live replaced (detail)

Live has only the revamped commons, ecommons, misty, sro, oasis, highpass, tox, oot, lavastorm, bazaar, highpasshold,
barter and freeporttemple, and changed everfrost; a RoF2 server places its spawns, paths and doors on the old
geometry. A **classic folder** fills them in: another install with the old files, such as a classic-era client
(`GameDir::set_classic`, below the base; `assets.classic` in `hailstone.ini`, or `--classic-dir`).

- A classic-era client's commons, misty, sro, oasis, highpass, tox, oot and everfrost match RoF2's (geometry,
  collision, regions, objects). ecommons and lavastorm do too, except zone lines that name the destination where
  RoF2's name a server zone point (and one ecommons support moved 2 units).
- With a live base, everfrost's and lavastorm's files come from the classic folder first (`classic_preferred_file`).
- bazaar.s3d, highpasshold.s3d, barter.eqg and freeporttemple.eqg are RoF2's alone, so a server's `assets/` brings
  them.

Live also has new `.eqg` zones under the RoF2 names arena, bazaar, highpasshold, lavastorm and nektulos. Those load as
`.s3d` when there is one. With a live base and none, the `.eqg` loads only when the server's own files bring it or the
safe point sent at zone-in (OP_NewZone) has its floor at most 10 units under it (`is_revamped_zone_name`,
`game/revamped_zone.h`); otherwise the zone is refused rather than loaded as the revamp.

Why: a stock server can run either geometry under one name and the zone-in packets do not say which (PEQ's Bazaar
version 0, expansions 3 to 8, is the old zone; version 1, expansion 9 on, the revamp). The original loads the `.eqg` by
name alone, so this test is a deliberate difference. On PEQ's rows the revamps pass and version 0's safe points have
no floor under them. Checked: `CDisplay::StartWorldDisplay` (tries `.EQG` first).

Still open:

- Live's arena.s3d and nektulos.s3d still win, although PEQ runs the revamp there (docs/unknowns.md).
- Live's chapterhouse, clz, crescent, relic, tutoriala and tutorialb differ from RoF2's too (the classic-era client's
  clz and tutorials as well) and may not match the server's maps.

#### Live models

- Live dropped the zones' own `<zone>_chr.s3d`; its `<zone>_chr.txt` names each model's own archive (`kob,kob_chr` for
  `kob_chr.s3d`). RoF2's rule would take such a line for an EQG; `chr_txt_entry_is_s3d` takes it for an S3D when that
  file exists.
- Live renamed models (Kael's frost giants are `FSG`, `GAM` in RoF2). When the client's code for a race has no model,
  `ActorRenderer::choose_model` tries the live `racedata.txt`'s code for the spawn's gender, then 2 (both), 0 and 1.
  Heights, dressing and animations still go by the client's code.
- A model no global, zone or EQG archive has is read from `<code>_chr.s3d` on first use (`ModelArchiveStore`), since
  live's `GlobalLoad.txt` loads few of them.
- With live and a classic folder, only these race codes have no model: HSN (race 216, RoF2's `ponightmare_chr`), MCR
  (race 656, `mcr_chr`) and race 72's PRE (live's boats are SHIP in `ship1`-`ship3_chr`). A server ships those
  archives.

#### Layers

All files are read through `GameDir` (`core/game_dir.h`), in layers: the active server's `servers/<profile>/assets/`
first, then the asset folder.

- A server file replaces the asset folder's file of the same name whole, so an archive (`.s3d`, `.eqg`, `.pfs`)
  replaces the whole archive.
- A file only the server layer has, such as a new zone, is found as well.
- Texture packs (`textures/<pack>/`) and the server's `maps/` are Hailstone's own files on top.

### Maps

The in-game map's files (`<zone>.txt`, `<zone>_1.txt` .. `_3.txt`, docs/dev-ui.md) are looked for in four places
(`game/map_files.h`). The first that has any of a zone's layers gives all four:

1. `servers/<profile>/maps/`: the player's edits on this server (the map toolbar saves here), or maps put there by hand
   for this server only.
2. `servers/<profile>/assets/maps/`: the server's maps, sent with its client files (`client_files/maps/` or
   `client_files/assets/maps/`) or added by hand. Under `[assets] player = off` only the ones the server sent count.
3. `maps/` in the Hailstone folder: the player's maps for every server.
4. The asset folder's `maps/`.

So a server's map replaces the player's global one, and an edit made on a server replaces both. Layers never mix
places: a server that sends only `<zone>.txt` shows no labels from a global `<zone>_1.txt` drawn for other geometry.
RoF2 reads one maps folder file by file; this is a deliberate difference.

#### Map packs

A map pack is a subfolder of `maps/` in any of the four locations above.

Choose **Map Pack** in the map window (on the Zone tab in Modern). In Gamepad, use the D-pad to focus the dropdown
and A to open it. Options also offers the setting under Display > Interface in Modern and Interface in Gamepad.
Both controls use the same per-character `[Main] MapSkin` setting in `UI.ini`.

The map looks in the chosen subfolder in all four locations first, then in the plain folders. **Default** uses
the plain folders. The list holds the subfolders of all four. It is read again each time the map window opens, and
with the map closed whenever Options asks for it, so a pack dropped in shows up without a restart.

#### Zone names

The layer files are named from the zone's short name, which the server sends (OP_NewZone). Stock short names are
lower case alphanumerics and `_`. A name that is not letters, digits and `_`, or is a Windows device name, is not used
(`core/zone_name.h`):

- the connection drops it at OP_NewZone (the zone then loads without scenery);
- the map reads and writes no file for it.

So a hostile server's `../` or absolute name cannot reach files outside these folders.

## Servers

The login screen lists the saved servers (`core/server_profile.h`, `src/ui/dev_screens.h`), with the last one used
(`login.profile`) picked; New, Save and Delete edit the list.

- A server has a name, an address and port, and the username when "Remember username" is ticked. The password is
  never saved.
- The folder id is made once from the name, so renaming keeps the folder.
- The Assets row opens the server's `assets/` folder, where the player may add files beside the downloaded ones.

### Server files

A server owner puts the files the server needs in `client_files/`, beside `eqemu_config.json`, at the top or under
`assets/`. Both land in `servers/<profile>/assets/`:

- `spells_us.txt`, `eqstr_us.txt`, `dbstr_us.txt`, `Resources/SkillCaps.txt`, `Resources/BaseData.txt` and
  `Resources/GlobalLoad.txt`;
- changed or new zones (`.s3d`, `.eqg`, `.zon`, `.emt`), models, sounds, anything under the paths the client reads.

Beside them go `hailstone.cfg` and the folders `scripts/` and `plugins/` (docs/plugins.md), which land in
`servers/<profile>/` under the same names. Hailstone's server script (server/README.md) sends them; no web server is
needed.

#### The download

At a login's first zone-in the client asks the zone for the list (path, size, SHA-256) and, before asking for spawns
(`OP_ReqClientSpawn`), downloads what `servers/<profile>/` lacks (`net/server_files.h`). The loading screen shows
**Downloading server files** with progress and speed.

1. The files come into `.download/` in pieces (32 KB, 32 asked ahead), each checked against its hash.
2. Only when all have come are they moved into place, and files the server no longer lists are deleted.
3. When something in `assets/` changed, the client rereads its files before the zone loads (`GameDir::rescan`).

A broken-off download keeps the complete files for the next try and leaves the folder as it was.

A server without the script does not answer. After 5 s (30 s for a server that sent files before, whose zone may be
hashing new ones) the zone-in goes on with what is there.

#### What it trusts

Paths must be relative. Refused:

- `..`, and parts starting with `.` or ending in `.` or a space (Windows drops those, so `evil.exe.` would land as
  `evil.exe`);
- Windows device names, on every platform: `CON`, `PRN`, `AUX`, `NUL`, `CONIN$`, `CONOUT$`, `COM1`-`COM9`,
  `LPT1`-`LPT9` and `COM`/`LPT` with a superscript 1 to 3, in any case and with any extension
  (`core/windows_names.h`, which the zip reader of updates and add-ons applies too);
- programs and scripts (`.exe`, `.dll`, `.so`, `.lua` ...), except Lua and data in `scripts/` and this platform's
  libraries and data in `plugins/` (other platforms' are skipped). What those may do is up to the server's
  `hailstone.cfg` (docs/plugins.md).

How much it may write is bounded too, since a size in the list is the server's word (up to 4 GB a file, 65536 files):

| Setting (hailstone.ini) | Default | Past it |
|---|---|---|
| `assets.server_files_max_file_mb` | 2048 | the file is refused like an unsafe name |
| `assets.server_files_max_total_mb` | 16384 | the sync fails before anything is written, leaving the old files |

These are Hailstone's bounds (`ServerFileSync::Limits`), far above the largest server measured (below).

Files the player added by hand stay unless the server lists the same name; one that matches the server's hash is kept.
Hashing such files runs 16 MB a frame, so the loading screen keeps drawing (**Checking the server's files**).

#### On a live asset folder

On a live asset folder a RoF2 server's `client_files/` needs the RoF2 files live lacks or changed (above):

- the five files the server must match;
- the classic zones live replaced, and the zones whose geometry differs;
- RoF2's per-zone `_chr.s3d` model archives.

Measured against a dev server: 612 files, 1.1 GB, about 70 s at 16 MB/s.

#### Speed

Packets are already compressed (EQEmu's `Network:CompressZoneStream`, on by default) and so are the archives, so
compressing the files again would gain about 5 %. Speed comes from asking ahead:

- During the download the zone session sends acks and requests at once instead of holding them 150 ms
  (`Session::set_no_hold`; 2.6 MB/s with the hold, 8.4 MB/s without).
- 32 pieces are asked ahead, into a 2 MiB socket receive buffer.

A missing piece is asked for again (`ServerFileSync::kChunkRetry`, `kChunkRetries`) at once when a later one has come
(the script answers in order), or after 3 s with nothing.

- A piece skipped again after 5 requests fails the sync.
- A server that just goes quiet fails it once 30 s pass with nothing from it (`kChunkTimeout`).

Either way the zone-in goes on with the old files.

When the server's files change during a download (its list id moves on), the script answers with a stale error and
the download restarts on the new list; more than three restarts in one sync fail it.

#### Window title

The OS window's title is the picked server's name ("Hailstone" before one is picked), and in the world
`<server> - <character>` (`client_window_title`). A character with several classes (multiclass, a server extension)
shows them: `<server> - <character> [War/Clr]`.

#### Reloading at login

Logging in applies the server's layer to the `GameDir`. When the layers change, what was loaded before the login is
read again (`ClientApp::reload_server_files`): login strings, stat tables, UI art, the audio system (`snd*.pfs`,
soundassets, mp3index, racedata) and sound events, global model and object archives and the on-demand index, spell and
item icons, the game menu and spell particles. Zones read their files as they load.

#### The world list

The server list shows each world's name and whether it can be joined, never its player count: no UI pack is given the
count (`hs.ServerSelect`). A test login's transient profile keeps a `worlds.ini` too, but no `server.ini`, so it stays
out of the list.

#### Deleting a server

Delete moves the server's folder to `servers/.deleted/<id>-<date>-<time>/` in one rename, so nothing is half deleted
when a file is held open, and moving it back restores it.

#### Command line

- `--server <id or name>` picks a saved server. With `--host`/`--port` as well, those give the address for this run
  (a test login saves nothing).
- `--user` with `--host` and `--port` logs in to a transient server that is never saved. Its characters' files go to
  the folder made from the world's name (`profile_id_for_world`), or to `servers/_transient-<id>/` when a saved
  profile has that id for another address (`transient_profile_id`).
- The `--ui-demo login` screen works on copies of the saved servers in a scratch folder and saves nothing.

#### Characters' files

One login server can list several worlds, and a character's name is unique only on its world, so each character's
files are in `servers/<profile>/characters/<world>/<name>/` (`core/character_files.h`).

`<world>` is the listed world name with characters other than letters, digits, space, `-`, `_` and `.` made `_`
(`world_folder_name`). Maps stay per server. The files keep the RoF2 client's formats:

| File | What |
|---|---|
| `UI.ini` | window places and the chat windows |
| `hotbuttons.ini` | the original's character ini (`<name>_<server>.ini`): hot buttons and socials, and the blocked buff lists (see below) |
| `BZR.ini` | trader prices |
| `AddressBook.txt` | the mail address book |
| `IgnoreList.txt` | the mail ignore list (the address book's Ignore button), the same format |
| `GuildNotes.txt` | the guild window's personal notes ("%08X %s %s" lines) |
| `chat.ini` | chat filters and user colours |
| `options.ini` | the game options (Options window, /lootnodrop, /trackplayers, /log ...) |
| `<name>.txt` | the chat log (/log; `eqlog` unless named). Kept open while logging, each line written through at once; `/log off` closes it |

The blocked buff lists in `hotbuttons.ini` are `[BlockedSpells]` `BlockedSpellID0..29` and `[BlockedPetSpells]`
`BlockedPetSpellID0..29`, -1 for an empty slot, sent to the server at each zone-in.

The asset folder's `defaults.ini`, `eqclient.ini` and `UI_<name>_<server>.ini` are read as defaults under these files
and never written; so are its `userdata/AddressBook.txt` and `userdata/IgnoreList.txt`, the books a character starts
from when it has none.

#### Character settings import

Use `/import` while playing the character that should receive the settings. Select a source character and world from
the configured game data folder, then choose socials or hotbar buttons. The dialog lists detected layout, trader,
barter, and saved hotbar files for reference.

The preview shows additions, matching slots, and skips with reasons. It displays social commands for review and
skips unsupported commands, occupied slots (including scripted socials), and unresolved action references. Applying
fills only the reviewed empty slots. Imported socials run when activated in play. The source files are read-only.

Hailstone saves the previous `hotbuttons.ini` under `import-backups/` in the active character's directory before
updating the settings. **Restore backup** in the import window is available while the current settings match a
saved import revision. Edits after an import prevent that backup from overwriting them.

## Builds and installs

A build makes the same folder at `build/hailstone/`: the binary, `shaders/`, `ui/`, `icons/`, `pictures/`,
`CHANGELOG.md`, `browser/` (with `HAILSTONE_BROWSER`), the runtime DLLs on Windows, `libMoltenVK.dylib` on macOS, and
the example plugin in `plugins/`. Run it as `build/hailstone/hailstone`; it keeps its `hailstone.ini`, logs and
servers beside the binary. The tools (`pfsdump`, `hailstone_check` ...) and the tests stay in `build/`.

The server plugins (projecthailstone/hailstone-nms; Project Triune's, private) are add-ons (docs/addons.md). Cloned
into `plugins/nms/` or `plugins/projecttriune/`, each builds into `build/hailstone/plugins/` and its tests join
`hailstone_tests`.

#### cmake --install

`cmake --install build --prefix <dir>` writes the shipped parts only:

- `<dir>/hailstone(.exe)`, `shaders/`, `ui/`, `icons/`, `pictures/`, `CHANGELOG.md`;
- `browser/` (about 390 MB on Linux with `libcef.so` stripped);
- the shared libraries the build made (SDL3 when fetched), found through an rpath of the binary's folder;
- on macOS, `libMoltenVK.dylib` and its `MoltenVK-LICENSE.txt`, loaded from beside the binary
  (`platform::bundled_vulkan_library`).

The example plugin is a separate component (`--component examples`), and so are the server plugins when checked out
(`--component nms`, `--component projecttriune`). A release includes no server plugin.

An install over the same folder replaces only the shipped parts. The updater (docs/updater.md) does the same and
refuses any package that would write one of the player's files or folders (`app::is_player_path`: `hailstone.ini`, `servers/`, `logs/`,
`maps/`, `scripts/`, `textures/`, `ui-data/`, `ui-addons/`, `ui/settings/`, `ui/loadingbackgrounds/` ...). A folder
with a CMakeCache.txt beside it (a dev build's `build/hailstone/`) never updates itself.

CI builds nothing but macOS (docs/ci.md). A release's folder is installed and packed locally, the macOS one from the
macOS workflow's artifact (docs/updater.md "Making a release"), without the server plugins.

## The first start

Every start makes the data folder and these folders when missing (`platform::create_data_dirs()`): `ui/settings/`,
`plugins/`, `scripts/config/`, `textures/default/`, `maps/`, `servers/`, `logs/`, `screenshots/`, `bug-reports/` and
`cache/`, so the player can see where plugins, scripts, texture packs and maps go.

Existing ones are left alone; one that cannot be made is logged and the start goes on. Each writer also makes its own
folder before writing, so deleting one while Hailstone runs is harmless.

A new install's graphics settings are fitted to the computer's GPU and CPU tier before first use (docs/graphics.md
"Detected settings").

#### Older builds' files

Older builds' per-user folder (`~/.config/hailstone`, earlier `~/.config/nms-client`) is no longer read or copied
from; it can be deleted.

Files in the older `servers/<profile>/characters/<name>/` are copied once, atomically, to
`characters/<world>/<name>/` for the first world a character of that name enters while no `characters/*/<name>/`
exists (`adopt_unsorted_character_files`); the older folder is left as it was.

Chat filters and colours older builds wrote into the asset folder's `eqclient.ini` still apply until changed, since
`chat.ini` is read over that file.

## Screenshots, the pipeline cache, crash and bug reports

#### Screenshots

The screenshot key saves the next frame as `screenshots/hailstone-<YYYYMMDD>-<HHMMSS>.png` (local time; `-2`, `-3`
... for more in the same second).

- It is the key map's Screen Capture command (`SCREENCAP`), unbound by default except in the third set (NPAD -).
- Print Screen takes one too, on every screen, unless the player gave that key to another command.
- `/screenshot` does the same.
- `--screenshot FILE` (the test runs) writes FILE, a relative one under `screenshots/`.

#### The pipeline cache

The Vulkan pipeline cache is read from `cache/pipelines.bin` when the device is made and written back (through a
temporary file) at close (`render/pipeline_cache.h`). The file is used only when it is whole (size and hash match) and
the same GPU and driver made it (vendor, device, driver version and the driver's cache UUID); otherwise the cache
starts empty and the file is replaced at the end of the run.

#### Crash reports

A crash (a fatal signal on Linux and macOS, an unhandled exception on Windows, or an unhandled C++ exception) writes
`logs/crash-<YYYYMMDD>-<HHMMSS>.txt` (UTC) with:

- the build (`core/build_info.h`: version and commit), the time and what happened;
- the log's path and its last 16 lines;
- the stack where glibc has one (`addr2line -e hailstone <+offset>` names a frame of a build with symbols).

On Windows a minidump, `crash-<...>.dmp`, goes beside it (`platform/crash_log.h`): call stacks, registers and the
module list. It goes into bug reports unchanged, so it holds no heap (not the password field, the session key or a
plugin's `auth_key`) and, of the stacks, only the pointers a trace needs (`MiniDumpFilterMemory`). The password is
also overwritten once the login server has taken it.

A stack overflow is reported on the main thread and on threads that make a `CrashStack` (`core/latest_jobs.h`).

After the report the previous handler runs (a core dump, a sanitizer's report, or on Windows the C++ runtime's).

#### Start-up errors

An error that ends the run without a crash (no Vulkan driver, no qualifying GPU, no window, missing shaders) is logged
and shown in a dialog (`app/startup_error.h`) with the kind of problem, the usual fix, the error and the log's path.
When no GPU qualifies, it names each GPU and what it lacks (`render/gpu_requirements.h`). The test runs (`--frames`,
`--screenshot`) show no dialog.

#### Bug reports

Bug reports are zips in `bug-reports/` for the player to post in the bug report forum (`app/bug_report.h`).

The bug report window (`/bugreport`) hides for the frame it opens on, so its screenshot shows what the player saw.
Send gives the server its copy (`OP_Bug`, as the original) and saves `hailstone-bug-<YYYYMMDD>-<HHMMSS>.zip` (local
time) in the background, then offers Open Folder and Copy Summary.

The start after a crash bundles the crash report and the crashed run's log (`<name>.1.log`) into
`hailstone-crash-<...>.zip`, and shows the crash report window for a note.

A zip holds:

- `report.txt`: the player's words, the build, system, GPU and driver, display, UI, server, world, character, zone,
  place and target;
- `summary.md`: a forum post under Discord's 2000 characters (Copy Summary);
- the crash report;
- the logs (this run's and the last);
- `hailstone.ini` and the UI's settings, with keys naming a password, token, secret or session blanked;
- the window's screenshot (at most 1920 wide) and the player's own from the 15 minutes before.

It stays under 9.5 MB, below Discord's 10 MiB upload limit. What does not fit (the screenshots first, then a log cut
to its end) is named in `report.txt`.

The report is posted in public, so every text in it is scrubbed:

- every saved or played login account name becomes `<account>` (a whole word, any case);
- the home folder (`platform::home_dir()`) becomes `~`, and its name (on Linux the system account name) `<user>`
  where it is a folder name elsewhere (`/run/media/<user>/`);
- in the logs and the crash report, the words of every chat line (`[info] chat: ...`) are taken out; the line stays.

A Windows `.dmp` goes in unchanged (see above); screenshots are not touched.

#### UTF-8 on Windows

On Windows the programs carry a manifest that makes UTF-8 their ANSI code page (Windows 10 1903 and later; MSVC-style
linkers, `cmake/utf8.manifest`), so a data or install folder with any characters in its name works. The folder choice
itself reads the environment and the binary's folder as wide or UTF-8 text, so it works on older Windows too.
