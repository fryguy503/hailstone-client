# Add-ons: scripts, plugins and tools from GitHub

The add-on manager lets a player install a MacroQuest Lua script, a compiled plugin (docs/plugins.md) or a tool (a
program of its own, below) by pasting a GitHub link on the login screen, without opening the Hailstone folder, and
keeps it up to date. It is a Hailstone
feature with no counterpart in the original client. It is built on the updater's parts (docs/updater.md): the HTTPS
download (`platform/http_get`), the zip reader (`core/zip_read`), SHA-256 and Ed25519 (`core/ed25519`).

## What the player sees

### Managed Lua add-ons

A script add-on can declare `autoload 1` in its manifest. Installing one enables it as an installed add-on:
Hailstone starts it automatically when the connected server's game state and policy allow its capabilities.
There is no separate script command or per-character autorun setup. Its **Enabled** switch in Add-ons controls
activation independently of auto-update; disabling it removes its windows, menus and renderer hooks immediately.
The implementation stays in Lua's sandbox, with the same accepted capability ceiling as any installed add-on.

Existing manifests without `autoload` retain the ordinary script behavior. Only `kind script` may opt in.
An update preserves the player's activation switch. An update that newly adds `autoload 1` starts disabled until
the player enables it, so an ordinary script cannot begin running automatically just because it updated.
The install card identifies managed activation before installation.

Managed startup checks the installed entry path and SHA-256, verifies the installed package files, and uses the
ordinary ScriptHost policy checks. A server's or manually installed same-name script is never automatically
started as that installed add-on. Missing, modified or unreadable installed records do not become unrestricted
manual autoruns. Disabling, removing or replacing the managed record stops that add-on's running state; failed
scripts are not restarted every frame.

For authors, `tools/make_addon.py --autoload` writes the opt-in. Local preview tools may install a verified archive
through `hailstone_addon_install_local --root <install> --package <zip> --manifest <manifest> --source <owner/repo>`.
This explicit tool follows the normal package size/hash, unpacking, registry and rollback path, honors signing
pins, and changes no client or character autorun settings. It does not start code or publish anything.

**Add-ons** on the login screen opens a card in the Modern pack (beside the characters card, so Play stays in reach;
it stays open until Close), a page under More in the Gamepad pack, and a window in the client's own ImGui screen. All
three draw `ui::LoginView::addons` (`view.addons` in Lua, app/lua_eq_pregame.cpp), as the update notice is drawn. The
Modern card has two tabs: **Find add-ons** (the store, below), where it opens, and **My add-ons** (the list).

**Find add-ons** lists every add-on of the add-on index (see Finding add-ons), so a player can find one without a
link. A pasted link (below the store in the Modern pack) still works for any add-on.

**Install from a link.** The player pastes a link and presses Look. These forms work:

- `https://github.com/owner/repo`, the newest release;
- `.../releases/tag/<tag>`, that release;
- `.../tree/<branch>`, that branch's files;
- `owner/repo`, `owner/repo@tag` or `owner/repo#branch`.

The client fetches the add-on's manifest and shows a card before it writes anything. The card holds:

- the name, version, kind (script or plugin), author, description and repository;
- what the add-on asks to do, in plain words ("read the game state", "cast spells, use abilities, items and AAs, attack, loot, order your pet");
- whether it is signed, and the start of its key;
- for a plugin, a warning: it is a program, so its permissions limit what it does through Hailstone, not what it can
  do on the computer;
- why it cannot be installed, when it cannot: it needs a newer Hailstone, it has no build for this system, it asks for
  a permission this Hailstone does not know, or its name is taken;
- when it is an older version than the one installed (a link to an older release's tag, or a release taken back),
  a warning that installing it goes back to that version, and **Install the older version** in place of Install:
  `eq.LoginDo("addonInstall")` alone does not take it, `eq.LoginDo("addonInstall", "older")` does
  (`AddonOffer::older`, `AddonManager::install_offer(true)`);
- **Install**, **Cancel** and **Repository**.

**The list.** Three groups:

| Group | What it shows | The player can |
|---|---|---|
| **Installed** (by the manager) | version, source, what it may do, its state | update, approve, switch auto-update, open its repository or folder, remove |
| **In your folders** (in `scripts/` or `plugins/` by hand, or shipped with Hailstone) | name, kind | open its folder |
| **From <server>** (the picked server's own, `servers/<profile>/scripts/` and `plugins/`) | name, kind; a script: "Provided by <server>: it always runs there, and cannot be removed or turned off"; a compiled plugin: whether you allowed it | open its folder; a plugin: **Don't allow it any more** (allowed) or **Ask me again** (refused) |

The server's own are the files the picked server sent at its last login (its `server_files.txt`). They are listed so
the player knows what runs there, but nothing in the list removes, stops or updates them. A compiled plugin of the
server's runs only once the player allowed its bytes (docs/plugins.md "The server's compiled plugins ask first"), and
its row is where that answer is changed. The server sends them again
at each login, and the client keeps them running ("is this server's own script: it keeps running", docs/plugins.md).
A server not visited yet, or one without Hailstone's script, has none.

A row of the other two groups also says what the picked server does with it, from that server's `hailstone.cfg` as
the last login left it:

- this server runs only its own scripts (or plugins), so this one does not run there;
- this server allows only part of what it may do;
- this server has its own of that name, and that one runs there.

**States** of an installed add-on: up to date; a newer version is out (auto-update off); downloading; downloaded (it
is installed at the login screen); it asks to do more (approve to update); you changed files of it (Update anyway
replaces them); not installed (another key, another add-on in the repository, a newer Hailstone needed); failed.

**When files change.** A download is checked and unpacked on a worker thread. Its files move into place only while
the login screen is up with no login under way and no box set window running, since those may run the same files.
Nothing runs at the login screen (docs/plugins.md), and plugins load from copies in `cache/plugins/`. So an install
or an update never changes a file under a running script or plugin. One downloaded while in the world waits for the
next login screen. A run that quits first finds it again at the next start, and the package is not downloaded twice.

**Auto-update is off** for every add-on until the player switches it on in its row: nothing updates unasked.
Installed by a client up to 0.7.0-beta, whose installs switched it on without asking (`auto 1` in `installed.txt`), an
add-on reads as off; the switch is now written as `autoupdate`.

**Updates at start.** A worker thread looks for a newer version of each add-on with auto-update on, at most every
`addons.check_hours`. A newer version is installed only when nothing stands in its way:

- it asks for nothing it was not granted (else it waits for **Approve and update**);
- the player changed none of its files (else it waits for **Update anyway**);
- it carries the same signing key (else it is refused: see Signatures).

**Check for updates** looks at every installed add-on now. Add-ons with auto-update off show the newer version
without taking it.

**The player's word wins over a check under way.** Each add-on is looked at with `installed.txt` as it is when its
turn comes, not as the check found it at its start: an add-on removed meanwhile is left alone (not downloaded or
installed again), and one whose auto-update was switched off is offered, not taken. Switched off while its update
downloads, the update is not installed at the login screen either (the row offers it). An update keeps the add-on's
auto-update switch as it is at install time.

## Tools

A tool is a program of its own that works beside Hailstone, with its files: Hailstone Texture Studio
(projecthailstone/hailstone-texstudio), which builds HD texture packs from the player's game files on their graphics
card, is the first. It installs, updates and is removed as any add-on, into `tools/<name>/`, and the client never
loads or runs anything of it itself:

- **Start** on its row (My add-ons; the gamepad pack's row menu; the client's own add-ons window) starts its program,
  `tools/<name>/<name>` (`<name>.exe` on Windows), with `--data-dir <the data folder>`, so it reads and writes the
  player's Hailstone folder (`hailstone.ini`, `textures/`, its own settings) wherever it was started from. It runs as a
  program of its own and is left running when Hailstone quits (`ClientApp::start_addon_tool`).
- **Options** start the one tool it knows of: Graphics, Textures, **Texture Studio...** once the add-on `texstudio`
  is installed; until then that row says to install it from Add-ons. The button starts it only when it is the real
  one: installed from projecthailstone/hailstone-texstudio and signed with the project's add-on key (the key the
  add-on index lists for the project's add-ons; `ClientApp::kTextureStudioRepo`, `kTextureStudioKey`,
  `find_pinned_addon`). Another add-on that took the name, or an unsigned one, is not started from Options (the log
  says why); its row's **Start** still starts it, where the player sees what it is. A new key for the studio needs a
  client release.
- **Its card** asks for no permissions, and says instead that a tool runs on the computer with the player's rights,
  outside Hailstone: the capability ceiling means nothing to a program of its own, so a tool's manifest says
  `caps none` and one that asks for any is refused.
- **What its package holds** (What is checked, below): its program at the top, named after it and a program for this
  platform by its header (never run at the install); this platform's shared libraries beside it (an SDL it was built
  with, `libSDL3.so.0`); and data and Lua anywhere (the tool's own: the client runs none of it). Nothing else that runs:
  no second program, no shell or system scripts, no libraries in folders or for another platform. On Linux and macOS
  the program alone is made runnable.

An older Hailstone refuses a tool's manifest ("the kind is script or plugin"). A tool needs a release of Hailstone
that has this section.

## Finding add-ons

The store is the add-on index (Suggested add-ons, below) shown whole: each add-on it lists, whatever server it is for.
The first time the store is shown in a run, the manager fetches each add-on's manifest (`AddonManager::browse`, after
the index itself is read or fetched), the same plain downloads a pasted link makes. **Refresh the list** fetches them
again. For each add-on the store shows:

- its name, kind, version, author and description, from its manifest;
- the worlds the index names it for ("for Project Triune"), or nothing when it is for any server;
- **Install**, or **Update to <version>** when a newer version than the installed one is out, or that it is installed,
  or that one of its name is in the player's folders already;
- why it cannot be installed, instead of the button: no manifest, a manifest of another name or not signed with the
  index's key for it (the same check as a suggestion's card), or what a pasted link's card would refuse (a newer
  Hailstone needed, no build for this system, an unknown permission, unsigned with `addons.allow_unsigned` off).

**Install** opens the add-on's install card, held to the index's name and key as a suggestion's is: nothing is
installed until the player presses Install there, after seeing what it asks to do. Only the index's add-ons are in
the store, so an add-on reaches it by a pull request to the index's repository, as a server does (below); an `addon`
record with no `world` record is for any server and is listed but never suggested.

## Suggested add-ons

Some servers have an add-on of their own: a plugin with the server's windows and features, say Project Triune's. A
player who logs in to such a server for the first time does not know it exists, so the login screen tells them.

**Where the client learns of it.** Two places, and the client names no server in its own code:

- **The add-on index**, `hailstone-addon-index.txt` in its own repository (projecthailstone/hailstone-addon-index;
  `core/addon_index.h`). It lists each add-on (its manifest's name, its repository, the key its manifest is signed
  with) and the worlds it is for (the login server's host, `*` for any, and the world's name as the login server
  lists it). It is signed with the release key, the same as the update manifest, and an index that does not check
  is not used. The client fetches it at start, at most every `addons.check_hours`, and keeps it in
  `addons/index.txt` (checked again when it is read). The store lists the same index. Its `serial <n>` record (once,
  after the first line; `hailstone_sign sign` writes the time it signs, in seconds since 1970, and always more than
  the serial it had) keeps an older signed index from being served again: a fetched index with a lower serial than
  the kept one, or with none once the kept one has one, is refused and the kept one stays (`addon_index_older`). An
  index without a serial is still taken while the kept one has none, so the indexes signed before the serial work. Since it is known before any login, it reaches a player on
  their first visit.
- **The server's own `hailstone.cfg`**: `[addons] suggest = owner/repo, ...` (docs/plugins.md). It comes only with
  the server's files at the first zone-in, so it is suggested from the next login screen on.

**What the player sees.**

- **On the server select**, when a world the login server lists has an add-on the player does not have, a line under
  the status says so: go Back and open Add-ons to look at it before playing. Add-ons install only at the login
  screen, while nothing of the folder runs.
- **On the login screen**, for the picked server (the worlds its login server listed last, `worlds.ini`, and its
  cached `hailstone.cfg`), each suggestion's sentence with three buttons:
  - **Look at it** opens the add-ons panel with the add-on's install card, as a pasted link does. Nothing is
    installed until the player presses Install.
  - **Not now** puts it away until Hailstone starts again.
  - **Don't ask again** puts it away for good on this server (`addons.suggest_never.<profile>` in `hailstone.ini`).

An add-on the player has is not suggested: one installed from the same repository (any tag or branch), or a script
or plugin of the same name put in by hand, or the server's own.

**The index's word is held to.** A suggestion from the index opens a card held to the index's name and key: a
manifest with another name, or not signed with that key (another key, or none), is blocked, whatever
`addons.allow_unsigned` says. A server's own suggestion of an add-on the index lists is held the same way. Anyone can
name a server "Project Triune", so a world's name proves nothing; the most it gets a stranger is a suggestion of the
real add-on. A server's suggestion of an add-on the index does not list is a plain link: the card is what a pasted
link gives, signed or not.

**Adding a server** is a pull request to the index's repository. A maintainer signs it with the release key
(`tools/sign.sh` there, `hailstone_sign sign`; `hailstone_sign verify <key> hailstone-addon-index.txt` lists it, its
serial first). Sign it with a `hailstone_sign` that writes the serial (this client's), so that each publish raises it.

## Licence

An add-on may use any licence, closed ones included: the client is GPL-2.0-or-later, but its plugin SDK (`sdk/`) is
MIT and NOTICE.md's plugin and add-on exception covers plugins built against it and scripts and UI packs that use the
documented Lua interfaces. An add-on that copies the client's own code (outside `sdk/`) is covered by the GPL.

## The manifest

An add-on is a GitHub repository with `hailstone-addon.txt`. **A repository without one is refused** ("it is not a
Hailstone add-on"). The format is described in `core/addon_manifest.h`:

```
hailstone-addon 1
name autoloot
kind script
version 1.3.0
min_client 0.2.0
author Some One
description Loots what you tell it to.
caps read,events,actions,ui
notes https://github.com/owner/autoloot/releases/tag/v1.3.0
package any 41233 <sha256> autoloot-1.3.0.zip
key <Ed25519 public key, 64 hex digits>
signature <128 hex digits>
```

| Record | |
|---|---|
| `hailstone-addon 1` | the first line; another version needs a newer Hailstone |
| `name` | 1 to 32 letters, digits, `-` and `_`, starting with a letter or digit: its folder, `scripts/<name>/` or `plugins/<name>/`; a plugin's library is named after it |
| `kind` | `script`, `plugin` or `tool` |
| `version`, `min_client` | SemVer (`core/version.h`); `min_client` is optional |
| `author`, `description` | optional, one line each of at most 200 bytes |
| `caps` | what it needs (docs/plugins.md "Capabilities"); required, and the ceiling below; a tool's is `none` |
| `notes` | optional: its release notes |
| `package <platform> <size> <sha256> <file>` | a script has one, for `any`; a plugin or a tool one for each platform it is built for (`linux-x64`, `windows-x64`, `macos-arm64` ...). The file is a name beside the manifest's address (the same release's asset, or the same branch's file) or an https address on GitHub |
| `key`, `signature` | optional, together: Ed25519 over every byte before the `signature` line, which ends the file |

Unknown records are skipped, for later versions. A known record that does not parse refuses the whole manifest.

**Where the client looks:**

- a repository: the newest release's asset (`.../releases/latest/download/hailstone-addon.txt`), then the default
  branch's file (`raw.githubusercontent.com/<owner>/<repo>/HEAD/hailstone-addon.txt`);
- a tag: that release's asset, then the tag's file;
- a branch: the branch's file.

These are plain downloads, not GitHub's REST API, so the API's limit of 60 requests an hour does not apply. A private
repository answers 404 and is reported as not an add-on.

**For authors:**

```sh
tools/make_addon.py --name autoloot --kind script --version 1.3.0 --caps read,events,actions,ui \
    --dir path/to/autoloot --author "Some One" --description "Loots what you tell it to." --out release/
tools/make_addon.py --name MQ2Hud --kind plugin --version 2.0.0 --caps read,ui \
    --platform linux-x64=build-linux/MQ2Hud --platform windows-x64=build-win/MQ2Hud --out release/
build/hailstone_sign keygen <secret-key-file>                  # once; keep it off GitHub
build/hailstone_sign sign <secret-key-file> release/hailstone-addon.txt
```

The script checks the client's rules for what a package may hold before it makes anything. It makes the zips the
same way each time (sorted, dated 1980-01-01), so an unchanged package keeps its hash. `hailstone_sign sign` writes
the key's line into an add-on's manifest and signs it; `verify` checks it. Upload everything in `release/` to a
GitHub release.

The repository has to be public: a private one answers 404. A plugin whose source must stay private can be released
from a second, public repository that holds only a README and the releases (the manifest and the packages); players
paste that one's link. The Project Triune plugin is released that way (projecthailstone/hailstone-projecttriune-addon),
the NMS plugin and the Auto Combat script from their own repositories. Each has a `tools/release.sh` that builds,
packs, signs and uploads.

## GitHub only

Only `github.com` links are taken. A download may end only on GitHub's own hosts, whatever redirects it took:
`github.com`, `raw.githubusercontent.com`, `codeload.github.com`, `objects.githubusercontent.com` and
`release-assets.githubusercontent.com` (`addon_download_allowed`). The address a transfer ends on is checked before
any of its body is used (`HttpGetOptions::allow_url`: libcurl's effective URL, WinHTTP's `WINHTTP_OPTION_URL`), and
also when it has no body: an empty answer from an address off those hosts is a failure, not an empty file. The
link parser (`core/addon_source.h`) keeps the hosts in one place, so others can be added later.

## Capabilities: the manifest's `caps` are a ceiling

The player's own scripts and plugins get every capability on a server without a `hailstone.cfg`, and the server's
`player_caps` under one (docs/plugins.md). An add-on the manager installed gets no more than its manifest declared
and the player accepted on its card:

> what the add-on gets = what the server allows ∩ what its manifest declared

| The server | A script declaring `read,ui` gets |
|---|---|
| no `hailstone.cfg` | `read,ui` (a script put in by hand: everything) |
| `player_caps = read,ui,actions` | `read,ui` |
| `player_caps = read` | `read` |

A script that goes past them gets the client's usual message (`[MQ] buffs: /tell needs the 'chat' permission`). One
without `login` does not run before the zone-in, whatever the server allows.

- **An update that asks for more** is not installed by itself: its row lists what it adds, and **Approve and update**
  takes it. One that asks for less lowers the ceiling. The approval is for what the row showed: the manifest is
  fetched again when the player clicks, and if it now asks for more than they saw, or is another version, nothing is
  taken: the message says to review it again, and the row shows what it asks for now.
- **Its windows** (a script's `ui/<pack>/`) run in the UI under the same ceiling (docs/plugins.md "Plugin windows"):
  `read,ui` lets them draw and read, not speak, act or start scripts, and never reach the login screen's password.
- **Its modules** serve only scripts that may do no more than they may: a full script's `require` refuses an
  add-on's module (docs/plugins.md "The `mq` module"), so an add-on cannot get its code run with another script's
  capabilities by naming a module after a common library.
- **For scripts this is real protection:** everything a Lua script does goes through the client.
- **For compiled plugins it is a declaration,** not a sandbox. The ceiling limits the host table, but native code can
  still call the system directly. The card says so.
- **The ceiling is on the files' hashes** (`plugin::Policy::set_ceilings`, from `addons/installed.txt` at start and
  after each change). It covers a script's Lua files and a plugin's library. A file the player changes has another
  hash and gets the rules of a file put in by hand: the player's own edit is theirs.
- **A record that cannot be read fails closed.** `installed.txt` is written with a copy, `installed.bak.txt`, and a
  record of it that is lost or does not read is taken from that copy (`read_addon_registry`); at the next start a
  lost or damaged `installed.txt` is written again from it. An add-on `installed.txt` (or its copy) still names whose
  record neither has whole gets no capabilities: each Lua file of its folder and each library at its top, by its
  bytes then, gets a ceiling of nothing, and the log says so. A newer Hailstone's `installed.txt` (another
  `hailstone-addons <n>` header, after an update was rolled back) is read the same way, from its `addon <name> <kind>`
  lines, and never written over. While any add-on does not read, nothing is installed, updated or removed (the
  message says why), so that its record is not lost: the player puts the file right, or takes it out with those
  add-ons' folders. A registry lost with its copy cannot be told from none, and its add-ons' files are then the
  player's own.
- **The server's own** keep every capability, also when an add-on ships the same bytes.

## What is checked

Everything is checked before anything is written; any failure refuses the whole install.

| Check | |
|---|---|
| The manifest | a strict parse within 64 KiB; the parser is in the libFuzzer zip target and the random-input tests |
| The capabilities | each word one this Hailstone knows, and at least one (`plugin::parse_caps_strict`); a tool none |
| The name | not another add-on's (any case), not a folder or file already in `scripts/` or `plugins/` (installed by hand or with Hailstone), whichever kind the add-on is: a script never takes a compiled plugin's name, nor a plugin a script's (their windows run as different owners, docs/plugins.md "Plugin windows", but the player would not tell them apart), not `config` for a script. A `plugins/<name>/` that holds no library (only the settings a plugin left there, as after an update took out a plugin an older release shipped) is not taken: the install goes in beside those files |
| The client | `min_client` against `release_version()`; a plugin or a tool needs a package for `update_platform()` |
| The signature | unsigned add-ons are refused when `addons.allow_unsigned` is off |
| The package | its size and SHA-256 against the manifest (a larger download is cut off at once) |
| The zip | `core/zip_read`: names that stay in the folder, the zip-bomb cut-off, CRC-32; at most 4096 files and 64 MiB (a script) or 512 MiB (a plugin or a tool) unpacked; no two names that differ only in case |
| What it holds | what a server may send into the same folder (`net::server_file_destination`): no programs but a plugin's library, Lua only in a script or in a plugin's `ui/`, no hidden or device names. A script has `init.lua` at its top. A plugin has exactly one library at its top, named after it (`<name>.so`, `lib<name>.so` ...). A tool: see Tools (its own rules, `unpack_tool`; the names' shape as the server's files, `net::is_safe_file_path_shape`) |
| The Lua | every `.lua` compiles as text (`luaL_loadbufferx(..., "t")`, not run): syntax errors and precompiled chunks are refused at the install, not at the first run |
| The library | its header (ELF, PE or Mach-O, a universal file too; `core/binary_header.h`) is a shared library for this platform's system and architecture. It is never loaded at the install; its ABI is checked at load, as for any plugin |
| A tool's program | its header is a program for this platform (`program_fits_platform`: ELF `ET_EXEC` or `ET_DYN`, as a position-independent program is; a PE executable image that is not a DLL; a Mach-O `MH_EXECUTE`). It is never run at the install |

### Signatures

An add-on's manifest may carry its author's key and a signature with it. The key is pinned at the first install. An
update that is not signed with the pinned key is refused, whether it is signed with another key or not signed at
all: the row says so, and neither auto-update nor the player's Update gets past it. Installing again from a link is
refused the same way. To trust a new key, the player removes the add-on and installs it again.

An unsigned add-on that becomes signed takes the key from then on. The signature proves that each version comes from
whoever holds the key, not merely from whoever can push to the repository.

## Files (docs/layout.md)

```
addons/
  installed.txt   each add-on: name, kind, source, version, the caps accepted, its key, auto-update (`autoupdate 0|1`), author,
                  description, notes, and every file it put in with its size and SHA-256
  installed.bak.txt installed.txt's copy, written with it (a lost or damaged record is read from it)
  checked.txt     when the last check at start ran (addons.check_hours)
  index.txt       the add-on index as last fetched (Suggested add-ons); index-checked.txt, when
  download/       packages, by their SHA-256; each goes once it is installed
  new/<name>/     a package unpacked and checked, before anything moves
  previous/<name>/ what an install or a removal is moving out, until it has finished
  journal.txt     an install's or a removal's moves, one a line, and a line once installed.txt is saved
  installed.prev.txt installed.txt as it was before an install or a removal rewrote it, until that has finished
  kept-<time>/    what an undo found in previous/ that the journal did not name (never removed by the client)
scripts/<name>/   an installed script
plugins/<name>/   an installed plugin
tools/<name>/     an installed tool (its program, its libraries, its data)
```

- **An update replaces only the files the add-on shipped.** A file the older version shipped and the newer one does
  not is taken out. The player's other files there stay: a plugin's settings beside it (`host->config_dir`), notes.
  So do empty folders, other than those the update emptied and an empty folder where a file now goes (a folder that
  still holds the player's files stops the update).
- **A removal** takes out the add-on's files and its record. The player's files in its folder stay, and the folder
  goes once it is empty.
- **A cut-off install or removal** (the client died between its moves) is undone at the next start from the journal.
  `installed.txt` is saved as `installed.prev.txt` before it is rewritten, and put back with the old files, so the
  list matches what is on disk. One that wrote `done` is left as it is. Each journal line is flushed to the disk
  before the move it names (write-ahead), each move is on the disk before the next, and each unpacked file is
  flushed before it moves, so a power cut leaves no empty file and no moved file without its line. Should a file
  still be in `previous/` after an undo (a journal the disk lost or damaged, a move back that failed), it may be the
  only copy of the player's old file: `previous/` is renamed `kept-<time>/` and a warning logged, not removed.
- **The core updater** (docs/updater.md) never writes `addons/`, and skips its optional plugin package of a folder an
  add-on installed, both when it downloads and again when it installs (an add-on installed in between wins).
  `scripts/` and `tools/` are the player's to it already. Its `core` package's files in a plugin folder an add-on
  installed are left out of the install, and those it shipped there before are not taken out.

## Settings (`hailstone.ini`)

| Key | Default | |
|---|---|---|
| `addons.check` | 1 | look for newer versions at start |
| `addons.check_hours` | 6 | at most this often (0: every start) |
| `addons.allow_unsigned` | 1 | install add-ons whose manifest is not signed (the card says so) |
| `addons.suggest` | 1 | suggest the add-ons of the servers you log in to (Suggested add-ons); the index is still fetched, for the store |
| `addons.index` | the build's (CMake `HAILSTONE_ADDON_INDEX`) | the add-on index's address, on GitHub's hosts; signed with the release key all the same |
| `addons.suggest_never.<profile>` | | the suggestions put away for good on that server, `owner/repo` with `\|` between |

Nothing is fetched when the system has no HTTPS (Linux and macOS without libcurl) or on test runs (`--frames`,
`--screenshot`). The list still shows what is installed then, and a removal works. The ceilings apply whether or not
anything is fetched. The client sends nothing but the `Hailstone/<version>` User-Agent.

For screenshots: `test.login=addons` in `ui/settings/<pack>.txt` opens the panel as the login screen first appears;
in the Modern pack `test.addontab=store` (or `mine`) picks its tab.

## Code

| | |
|---|---|
| `core/addon_manifest.*` | the manifest |
| `core/addon_source.*` | GitHub links, the manifest's addresses, the hosts a download may end on |
| `core/binary_header.*` | a library's or a program's format and architecture |
| `core/addon_index.*` | the add-on index and which of its add-ons a login's worlds are for |
| `app/addon_suggest.*` | which suggestions the player sees, and the ones put away |
| `app/addon_install.*` | what a package may hold, the Lua and library checks, `installed.txt`, the journaled install and removal, the ceilings |
| `app/addon_manager.*` | the worker: fetch, check, download, unpack; `install_ready` on the main thread; the store's manifests (`browse`); the server's and the hand-installed lists |
| `app/client_app_addons.cpp` | the client's part: the view, the actions, when files may move, starting a tool (`start_addon_tool`) |
| `plugin/policy.*` | `Ceiling`, `parse_caps_strict`, `caps_described` |
| `app/lua_eq_pregame.cpp` | `view.addons`, `eq.LoginSet("addonLink" \| "addonsOpen")`, `eq.LoginDo("addonInspect" \| "addonInstall" \| "addonCancel" \| "addonUpdate" \| "addonRemove" \| "addonAuto" \| "addonFolder" \| "addonPage" \| "addonCheck" \| "addonRevoke" \| "addonAskAgain" \| "addonSuggestLook" \| "addonSuggestLater" \| "addonSuggestNever" \| "addonBrowse" \| "addonStoreLook" \| "addonStart")`, `view.addons.suggestions`, `view.addons.store`; `eq.ServerSelect().addonHint` |
| `ui/modern/login.lua`, `ui/gamepad/login.lua`, `ui/dev_screens.cpp` | the panel, and the suggestions under the login card |
| `ui/modern/serverselect.lua`, `ui/gamepad/serverselect.lua` | the server select's line about a suggested add-on |

A plugin's windows get none of this: `eq.LoginScreen` is nil for them, as for every pre-game screen, so a plugin
cannot install another.

## Tests and what is not checked

- `tests/addon_manifest_test.cpp`: the links, the manifest's checks and signature, random and cut-off input, the
  headers of each platform.
- `tests/addon_install_test.cpp`: what a package may hold, an install and an update keeping the player's files, a
  removal, a cut-off install undone, `installed.txt` and its backup, a damaged, lost or newer one failing closed, the
  names taken, the ceilings, the pinned add-on, the server's and hand-installed lists.
- `tests/addon_manager_test.cpp`: the manager against GitHub in memory: the install card, an older version from a
  link, a package changed or redirected off GitHub, updates that ask for more, that would overwrite the player's
  changes or that carry another key, auto-update off, the check interval.
- `tests/addon_index_test.cpp`: the index's checks and signature, its serial and an older index refused, random and
  cut-off input, which worlds match, which
  suggestions the player sees, `[addons] suggest`, the manager fetching and caching the index, a suggestion's card
  held to the index's name and key, and the store's list (each add-on's manifest held to the index's name and key,
  what keeps one from being installed, once a run unless refreshed, no index).
- `tests/plugin_policy_test.cpp` (the ceiling), `tests/lua_eq_pregame_test.cpp` (the Lua view and actions),
  `tests/ui_pack_windows_test.cpp` (both packs draw the panel and the store in each state), `tests/dev_ui_test.cpp`
  (the client's own window and store).

Not checked yet:

- **The real GitHub.** Fetching from a real public repository, through GitHub's redirects to its asset hosts, has not
  been tried.
- **Windows.** The WinHTTP final-address check only compiles; it has not run.
- **The store live.** The store is checked against GitHub in memory and drawn in tests; it has not listed the real
  index. The index lists Project Triune's add-on, Auto Combat and the DPS meter; the NMS plugin appears once its
  repository is public and it has a line there.
- **Suggested add-ons live.** The index and the suggestions are checked against GitHub in memory and the packs drawn
  in tests; a first login to a real server that lists a suggested world has not been tried.
- **macOS.** No plugin add-on has been installed there.
