# Add-ons: scripts, plugins and tools from GitHub

The add-on manager installs add-ons from a GitHub link pasted on the login screen and keeps them up to date, so the
player never opens the Hailstone folder. Five kinds:

- a MacroQuest Lua script;
- a compiled plugin (docs/plugins.md);
- a tool: a program of its own (below);
- an upscaler: a temporal upscaler from outside the client (below);
- an interface (UI packs, below).

It is a Hailstone feature with no counterpart in the original client, built on the updater's parts (docs/updater.md):
the HTTPS download (`platform/http_get`), the zip reader (`core/zip_read`), SHA-256 and Ed25519 (`core/ed25519`).

A script manifest may opt into managed startup with `autoload 1` (scripts only).

- Installation enables that script. **Enabled** in **My add-ons** controls whether it starts when the server permits
  it.
- Updates keep the management and the player's switch until removal or reinstallation, even if a later manifest drops
  `autoload`. Ordinary script autorun cannot bypass a disabled switch.
- An ordinary script that first opts in through an update waits for the player to enable it.
- Automatic updates remain a separate choice.

## What the player sees

**Add-ons** on the login screen opens a card in the Modern pack, a page under More in the Gamepad pack, or a window
in the client's own ImGui screen. All three draw `ui::LoginView::addons` (`view.addons` in Lua,
app/lua_ui_pregame.cpp). The Modern card opens beside the login and character cards when they fit side by side; in a
narrower window it opens on its own, with **Close** near the top to return to login. The Modern login and Add-ons
cards scroll when their contents exceed the available height. The Modern card has two tabs: **Find add-ons** (the
store, below), where it opens, and **My add-ons** (the list).

**Find add-ons** lists every add-on of the add-on index (see Finding add-ons). A pasted link (below the store in the
Modern pack) works for any add-on.

**Install from a link.** The player pastes a link and presses Look:

- `https://github.com/owner/repo`, the newest release;
- `.../releases/tag/<tag>`, that release;
- `.../tree/<branch>`, that branch's files;
- `owner/repo`, `owner/repo@tag` or `owner/repo#branch`.

The client fetches the manifest and shows a card before it writes anything:

- the name, version, kind (script, plugin, tool, upscaler or ui), author, description and repository;
- what the add-on asks to do, in plain words ("read the game state", "cast spells, use abilities, items and AAs, attack, loot, order your pet");
- whether it is signed, and the start of its key;
- for a plugin, a warning: its permissions limit what it does through Hailstone, not what it can do on the computer;
- why it cannot be installed, when it cannot: it needs a newer Hailstone, has no build for this system, asks for a
  permission this Hailstone does not know, or its name is taken;
- when it is older than the installed version: a warning, and **Install the older version** in place of Install.
  `hs.LoginDo("addonInstall")` alone does not take it; `hs.LoginDo("addonInstall", "older")` does;
- **Install**, **Cancel** and **Repository**.

**The list.** Three groups:

| Group | What it shows | The player can |
|---|---|---|
| **Installed** (by the manager) | version, source, what it may do, its state | update, approve, switch auto-update, open its repository or folder, remove |
| **In your folders** (in `scripts/`, `plugins/` or `upscalers/` by hand, or shipped with Hailstone) | name, kind | open its folder |
| **From <server>** (the picked server's own, `servers/<profile>/scripts/` and `plugins/`) | name, kind; a script: "Provided by <server>: it always runs there, and cannot be removed or turned off"; a compiled plugin: whether you allowed it | open its folder; a plugin: **Don't allow it any more** (allowed) or **Ask me again** (refused) |

The server's own are the files the picked server sent at its last login (`server_files.txt`), listed so the player
knows what runs there. Nothing in the list removes, stops or updates them; the server sends them again at each login
("is this server's own script: it keeps running", docs/plugins.md). A server's compiled plugin runs only once the
player allowed its bytes (docs/plugins.md "The server's compiled plugins ask first"); its row is where that answer
changes.

A row of the other two groups also says what the picked server does with it, from its `hailstone.cfg` as the last
login left it:

- this server runs only its own scripts (or plugins), so this one does not run there;
- this server allows only part of what it may do;
- this server has its own of that name, and that one runs there.

**States** of an installed add-on:

- up to date;
- a newer version is out (auto-update off);
- downloading;
- downloaded (it is installed at the login screen);
- it asks to do more (approve to update);
- you changed files of it (Update anyway replaces them);
- not installed (another key, another add-on in the repository, a newer Hailstone needed);
- failed.

**When files change.** A download is checked and unpacked on a worker thread. Its files move into place only at the
login screen, with no login under way and no box set window running. Nothing runs at the login screen and plugins load
from copies in `cache/plugins/`, so an install never changes a file under a running script or plugin. One downloaded
in the world waits for the next login screen, or the next start; it is not downloaded twice.

The box set's other windows (started with `--box-child`) neither check, download nor install add-ons: the first
window does, once they have quit.

**Auto-update is off** for every add-on until the player switches it on in its row. The switch is written as
`autoupdate`; an old `auto 1` in `installed.txt` (clients up to 0.7.0-beta set it unasked) reads as off.

**Updates at start.** A worker thread looks for a newer version of each add-on with auto-update on, at most every
`addons.check_hours`. It is installed only when:

- it asks for nothing it was not granted (else it waits for **Approve and update**);
- the player changed none of its files (else it waits for **Update anyway**);
- it carries the same signing key (else it is refused: see Signatures).

**Check for updates** looks at every installed add-on now; those with auto-update off show the newer version without
taking it.

**The player's word wins over a check under way.** Each add-on is judged by `installed.txt` as it is when its turn
comes: one removed meanwhile is left alone; one whose auto-update was switched off is offered, not taken, also when
its download is already under way. An update keeps the auto-update switch as it is at install time.

## Tools

A tool is a program of its own that works beside Hailstone. The first is Hailstone Texture Studio
(projecthailstone/hailstone-texstudio), which builds HD texture packs from the player's game files on their graphics
card. It installs, updates and is removed as any add-on, into `tools/<name>/`. The client never loads or runs anything
of it itself.

#### Starting a tool

**Start** on its row (My add-ons; the gamepad pack's row menu; the client's own add-ons window) runs
`tools/<name>/<name>` (`<name>.exe` on Windows) with `--data-dir <the data folder>`, so the tool finds the player's
Hailstone folder (`hailstone.ini`, `textures/`, its own settings).

- It is left running when Hailstone quits.
- A server whose opted-in add-on policy refuses the tool (docs/plugins.md "Opt-in add-on policies") refuses Start
  while that server is picked or played on. A tool already running goes on.

#### Texture Studio in Options

Options, Graphics, Textures has **Texture Studio...** once the add-on `texstudio` is installed; until then the row
says to install it from Add-ons.

- The button starts it only when it is installed from projecthailstone/hailstone-texstudio and signed with the
  project's add-on key (the one the add-on index lists for the project's add-ons; `ClientApp::kTextureStudioKey`).
- Another add-on that took the name, or an unsigned one, is not started from Options (the log says why). Its row's
  **Start** still starts it.
- A new key for the studio needs a client release.

#### A tool's card and package

**Its card** asks for no permissions; it says that a tool runs on the computer with the player's rights, outside
Hailstone. Its manifest says `caps none`, and one that asks for any is refused.

**What its package holds** (What is checked, below):

- its program at the top, named after it and a program for this platform by its header (never run at the install);
- this platform's shared libraries beside it (an SDL it was built with, `libSDL3.so.0`);
- data and Lua anywhere (the tool's own: the client runs none of it).

Nothing else that runs: no second program, no shell or system scripts, no libraries in folders or for another
platform. On Linux and macOS the program alone is made runnable. An older Hailstone refuses a tool's manifest ("the
kind is script or plugin").

## Upscalers

An upscaler is an upscaler provider (docs/plugins.md "Upscaler providers", `sdk/hailstone_upscaler.h`): a temporal
upscaler the client cannot ship itself, such as Intel XeSS or AMD FSR 4, with the vendor's runtime it loads.

It installs, updates and is removed as any add-on, into `upscalers/<name>/`. It appears in Options, Graphics,
Upscaler after the client's own (both packs; `option_choices("upscaler_providers")`). Picked there, it loads at the
next start (docs/graphics.md "Upscaler providers").

- **Its card** asks for no permissions (it sees only the frames it upscales), so its manifest says `caps none`; one
  that asks for any is refused. The card warns that it is still a program inside Hailstone with the player's rights.
- **Servers** do not send providers or receive a provider report. A server's opted-in add-on policy can require,
  allow or block one by its repository; one it refuses keeps the character out of that server until another is picked
  and the client restarted (docs/plugins.md "Opt-in add-on policies").
- **What its package holds:**
  - at its top, its provider: a library for this platform named after it (`xess.dll`, `xess.so`);
  - the libraries it loads, beside it (`libxess.dll`, `libxess.so`);
  - data anywhere (the vendor's licence and notices);
  - nothing else: no library in a folder or for another platform, nothing that runs on its own. Every library's
    header is checked for this platform; none is loaded at the install.
- **While it runs** the client has a copy loaded (`cache/upscalers/`), so an update or removal takes effect at the
  next start.
- **Hailstone's own updates** never write or remove anything in `upscalers/` (docs/updater.md "The install"), so a
  release cannot replace an upscaler add-on that shares a name with one it might ship.

An older Hailstone refuses an upscaler's manifest ("the kind is script, plugin or tool").

## Finding add-ons

The store shows the whole add-on index (Suggested add-ons, below), whatever server each add-on is for. The first time
it is shown in a run, the manager fetches each add-on's manifest (plain downloads, as for a pasted link). **Refresh
the list** fetches them again.

For each add-on the store shows:

- its name, kind, version, author and description, from its manifest;
- the worlds the index names it for ("for Project Triune"), or nothing when it is for any server;
- **Install**, or **Update to <version>**, or that it is installed, or that one of its name is in the player's folders
  already;
- instead of the button, why it cannot be installed:
  - no manifest;
  - a manifest of another name, or not signed with the index's key for it;
  - what a pasted link's card would refuse (a newer Hailstone needed, no build for this system, an unknown
    permission, unsigned with `addons.allow_unsigned` off).

**Install** opens the install card, held to the index's name and key as a suggestion's is. Nothing is installed until
the player presses Install there.

Only the index's add-ons are in the store. An add-on reaches it by a pull request to the index's repository, as a
server does (below). An `addon` record with no `world` record is for any server: listed but never suggested.

## Suggested add-ons

A server can also require, allow or block add-ons by their GitHub repository (docs/plugins.md "Opt-in add-on
policies"). This is off unless its `hailstone.cfg` says `[addons] enforce = on`.

- A required add-on the player does not have shows on the login screen as "Required to play on this server", with
  one button, Review required add-on (no Not now or Don't ask again), and goes through the usual card and approvals.
- Missing at the zone-in, it keeps the character out, with the panel open on it.
- Suggestions stay optional. Under an enforced `default = deny` a suggested repository is also allowed (as if named in
  `allow`). A suggestion never installs, starts or grants a capability.

Some servers have an add-on of their own (a plugin with the server's windows and features, say Project Triune's). The
login screen tells a first-time player it exists. The client names no server in its code; it learns of them from:

- **The add-on index**, `hailstone-addon-index.txt` in its own repository (projecthailstone/hailstone-addon-index;
  `core/addon_index.h`). See below.
- **The server's own `hailstone.cfg`**: `[addons] suggest = owner/repo, ...` (docs/plugins.md). It comes with the
  server's files at the first zone-in, so it is suggested from the next login screen on.

#### The add-on index

- **What it lists:** each add-on (its manifest's name, its repository, the key its manifest is signed with) and the
  worlds it is for (the login server's host, `*` for any, and the world's name as the login server lists it).
- **Signed:** with the release key, as the update manifest. An index that does not check is not used.
- **Fetched:** at start, at most every `addons.check_hours`; kept in `addons/index.txt` and checked again when read.
  Known before any login, it reaches a player on their first visit.
- **Serial:** a `serial <n>` record, once, after the first line. `hailstone_sign sign` writes the time it signs (seconds
  since 1970), always more than the previous serial. A fetched index with a lower serial than the kept one, or with none
  once the kept one has one, is refused and the kept one stays. An index without a serial is taken while the kept one
  has none.

#### What the player sees of suggestions

- **On the server select**, when a listed world has an add-on the player does not have, a line under the status says
  to go Back and open Add-ons before playing (add-ons install only at the login screen).
- **On the login screen**, for the picked server (the worlds its login server listed last, `worlds.ini`, and its
  cached `hailstone.cfg`), each suggestion's sentence with three buttons:
  - **Look at it** opens the add-on's install card, as a pasted link does.
  - **Not now** puts it away until Hailstone starts again.
  - **Don't ask again** puts it away for good on this server (`addons.suggest_never.<profile>` in `hailstone.ini`).

An add-on the player has is not suggested: one installed from the same repository (any tag or branch), a script or
plugin of the same name put in by hand, or the server's own.

**The index's word is held to.** A suggestion from the index opens a card held to the index's name and key.

- A manifest with another name, or not signed with that key (another key, or none), is blocked, whatever
  `addons.allow_unsigned` says.
- A server's own suggestion of an add-on the index lists is held the same way.
- Anyone can name a server "Project Triune", so a world's name proves nothing; the most it gets a stranger is a
  suggestion of the real add-on.
- A server's suggestion of an add-on the index does not list is a plain link, signed or not.

**Adding a server** is a pull request to the index's repository. A maintainer signs it with the release key (the
index repository's `tools/sign.sh`, `hailstone_sign sign`, with a `hailstone_sign` that writes the serial).
`hailstone_sign verify <key> hailstone-addon-index.txt` lists it, its serial first.

## Licence

An add-on may use any licence, closed ones included. The client is GPL-2.0-or-later, but its plugin SDK (`sdk/`) is
MIT. NOTICE.md's plugin and add-on exception covers plugins built against it, and scripts and UI packs that use the
documented Lua interfaces. An add-on that copies the client's own code (outside `sdk/`) is covered by the GPL.

## The manifest

An add-on is a GitHub repository with `hailstone-addon.txt`. **A repository without one is refused** ("it is not a
Hailstone add-on"). The format is in `core/addon_manifest.h`:

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
| `kind` | `script`, `plugin`, `tool`, `upscaler` or `ui` (UI packs, below) |
| `version`, `min_client` | SemVer (`core/version.h`); `min_client` is optional |
| `author`, `description` | optional, one line each of at most 200 bytes |
| `caps` | what it needs (docs/plugins.md "Capabilities"); required, and the ceiling below; a tool's and an upscaler's are `none` |
| `notes` | optional: its release notes |
| `package <platform> <size> <sha256> <file>` | a script has one, for `any`; a plugin, a tool or an upscaler one per platform it is built for (`linux-x64`, `windows-x64`, `macos-arm64` ...); see the note below |
| `key`, `signature` | optional, together: Ed25519 over every byte before the `signature` line, which ends the file |

A `package` file is a name beside the manifest's address (the same release's asset, or the same branch's file) or an
https address on GitHub. Unknown records are skipped; a known record that does not parse refuses the whole manifest.

**Where the client looks:**

- a repository: the newest release's asset (`.../releases/latest/download/hailstone-addon.txt`), then the default
  branch's file (`raw.githubusercontent.com/<owner>/<repo>/HEAD/hailstone-addon.txt`);
- a tag: that release's asset, then the tag's file;
- a branch: the branch's file.

These are plain downloads, not GitHub's REST API, so its limit of 60 requests an hour does not apply. A private
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

- The script checks the client's rules for what a package may hold before it makes anything:
  - the names its zip reader takes (no `\`, `:` or control character, no part ending in `.` or a space, no Windows
    device name such as `aux.lua`);
  - the version as the client parses it (no leading zeros);
  - one `--platform` a platform (a second would overwrite the first's zip and leave two package lines).
- It leaves out files and folders whose name starts with `.`, stores a link as what it points to only when that lies
  inside the package's folder, and refuses an `--out` inside a package's folder, where old zips would be packed.
- Its zips are reproducible (sorted, dated 1980-01-01), so an unchanged package keeps its hash.
- `hailstone_sign sign` writes the key's line into the manifest and signs it (refusing a manifest with lines after
  an old signature line, which it would otherwise drop); `verify` checks it.
- Upload everything in `release/` to a GitHub release.

The repository has to be public. A plugin whose source must stay private can be released from a second, public
repository that holds only a README and the releases (the manifest and the packages); players paste that one's link.
The Project Triune plugin is released that way (projecthailstone/hailstone-projecttriune-addon); the NMS plugin and
the Auto Combat script from their own repositories. Each has its own `tools/release.sh` that builds, packs, signs and
uploads (this repository has none).

## UI packs

A `kind ui` addon installs interfaces under `ui-addons/<addon>/ui/<pack>/ui.ini` in the Hailstone data folder.

- They appear in **Interface** after installation; they do not use `/lua run`, Script Autorun or a script entrypoint.
- Each interface has the stable ID `addon.<addon>.<pack>`. Built-in IDs are unchanged.
- Addon and pack names use letters, digits, underscores or hyphens, beginning with a letter or digit.

**What an interface may do:** draw the game's windows and act in the game as the player does (casting, moving items,
speaking, targeting), as the built-in ones do. It does not get what only `ui/modern` and `ui/gamepad` get:

- The login and server select screens are drawn by the built-in interface under it (its base, else Modern), so it
  never sees the typed or saved password, nor the add-on manager's installs, removals and approvals.
- It cannot answer the server's compiled-plugin question, which the client draws itself.

An interface put in `ui/` by hand is treated the same way; the install card says so. Same-kind updates keep it, and an
addon cannot change its kind in place.

#### What a UI package holds

A UI manifest uses `package any`. `caps` may be absent or `caps none`; it does not limit the full-interface trust.

- Archives contain one or more `ui/<pack>/ui.ini` entries, optional presentation Lua/assets below those folders, and
  optional top-level license/readme notices. No programs or root-level startup script.
- Lua is compiled as text during installation and runs only through the selected interface's presentation lifecycle.
- ZIP path, case-collision, CRC and Lua size checks apply. Nested directories must use consistent spelling and cannot
  also name a file, so a bundle behaves the same on case-sensitive and case-insensitive filesystems.
- UI bundles are limited to 4,096 files and 64 MiB unpacked.
- Updates and removal keep files the addon did not own, and use the transactional journal and pinned signing keys.

An interface can ship raster artwork under its UI folder, such as `ui/<pack>/assets/atlas.png`. API 1.7's
`hs.UI.assets.InterfaceImage("assets/atlas.png")` searches the selected interface and its approved bases. It
accepts DDS, TGA, PNG, BMP and JPEG. Use paths with the package's exact spelling and check
`hs.UI.Supports("assets", "InterfaceImage")` on older hosts. Cached images are specific to their resolved package
files. The lookup rejects paths to unapproved add-ons or links outside approved UI folders.

#### Local presentation data

Player-derived presentation modules live outside the managed files, under `ui-data/<stable-interface-id>/` in the
data folder.

- Interfaces import them with `hs.UI.interfaces.LoadData`; only selected and approved base IDs are searched.
- They are shared, non-secret styles/layouts, not credentials or private settings.
- Core and add-on updates keep them, and removing the add-on leaves them for a later reinstall.
- Portable UI packages contain the original presentation code and metadata. Local presentation data is managed separately.

Build a UI package with:

```sh
python tools/make_addon.py --name my-interface --kind ui --version 1.0.0 \
    --dir path/to/package --out release/
```

Use `base=builtin:modern` to inherit the built-in Modern interface, or an installed qualified interface ID for a
cross-addon base. A missing runtime or game source is reported separately from installation; installing metadata does
not supply missing game resources.

## GitHub only

Only `github.com` links are taken. A download may end only on GitHub's own hosts, whatever redirects it took:

- `github.com`
- `raw.githubusercontent.com`
- `codeload.github.com`
- `objects.githubusercontent.com`
- `release-assets.githubusercontent.com`

The final address is checked before any of the body is used (libcurl's effective URL, WinHTTP's
`WINHTTP_OPTION_URL`), and also when there is no body: an empty answer off those hosts is a failure. The hosts are in
one place, the link parser `core/addon_source.h`.

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

A script that goes past them gets the usual message (`[MQ] buffs: /tell needs the 'chat' permission`). One without
`login` does not run before the zone-in, whatever the server allows.

- **An update that asks for more** is not installed by itself: its row lists what it adds, and **Approve and update**
  takes it. One that asks for less lowers the ceiling.
- **The approval is for what the row showed.** The manifest is fetched again on the click; if it now asks for more or
  is another version, nothing is taken, the message says to review it again, and the row shows the new request.
- **Its windows** (a script's `ui/<pack>/`) run under the same ceiling (docs/plugins.md "Plugin windows"). `read,ui`
  lets them draw and read, not speak, act or start scripts, and never reach the login screen's password.
- **Its modules** serve only scripts that may do no more than they may: a full script's `require` refuses an add-on's
  module (docs/plugins.md "The `mq` module"), so naming a module after a common library gains nothing.
- **For scripts this is real protection:** everything a Lua script does goes through the client.
- **For compiled plugins it is a declaration,** not a sandbox: the ceiling limits the host table, but native code can
  call the system directly. The card says so.
- **The ceiling is on the files' hashes** (from `addons/installed.txt`, at start and after each change): a script's Lua
  files and a plugin's library. A file the player changes gets the rules of a file put in by hand.
- **A record that cannot be read fails closed.** See below.
- **The server's own** keep every capability, also when an add-on ships the same bytes.

#### A record that cannot be read

- `installed.txt` is written with a copy, `installed.bak.txt`. A record lost or unreadable in it is taken from the
  copy, and at the next start a lost or damaged `installed.txt` is rewritten from it.
- An add-on still named but whose record neither file has whole gets no capabilities: each Lua file of its folder and
  each library at its top gets a ceiling of nothing, and the log says so.
- A newer Hailstone's `installed.txt` (another `hailstone-addons <n>` header, after an update was rolled back) is read
  from its `addon <name> <kind>` lines the same way, and never written over.
- While any add-on does not read, nothing is installed, updated or removed (the message says why). The player puts
  the file right, or takes it out with those add-ons' folders.
- A registry lost with its copy cannot be told from none; its add-ons' files are then the player's own.

## What is checked

Everything is checked before anything is written; any failure refuses the whole install.

| Check | |
|---|---|
| The manifest | a strict parse within 64 KiB; the parser is in the libFuzzer zip target and the random-input tests |
| The capabilities | each word one this Hailstone knows, and at least one (`plugin::parse_caps_strict`); a tool or an upscaler none |
| The name | not another add-on's, not already in `scripts/` or `plugins/`, not `config` for a script; see the note below |
| The client | `min_client` against `release_version()`; a plugin, a tool or an upscaler needs a package for `update_platform()` |
| The signature | unsigned add-ons are refused when `addons.allow_unsigned` is off |
| The package | its size and SHA-256 against the manifest (a larger download is cut off at once) |
| The zip | `core/zip_read`: names that stay in the folder, the zip-bomb cut-off, CRC-32; at most 4096 files and 64 MiB (a script) or 512 MiB (a plugin, a tool or an upscaler) unpacked; no two names that differ only in case |
| What it holds | what a server may send into the same folder; see the note below |
| The Lua | every `.lua` compiles as text (`luaL_loadbufferx(..., "t")`, not run): syntax errors and precompiled chunks are refused at the install |
| The library | its header (ELF, PE or Mach-O, a universal file too; `core/binary_header.h`) is a shared library for this platform's system and architecture. Never loaded at the install; its ABI is checked at load |
| A tool's program | its header is a program for this platform (ELF `ET_EXEC` or `ET_DYN`; a PE executable image that is not a DLL; a Mach-O `MH_EXECUTE`). Never run at the install |

**The name.** Not another add-on's (any case), nor a folder or file already in `scripts/` or `plugins/` (by hand or
with Hailstone), whichever kind the add-on is: a script never takes a plugin's name, nor a plugin a script's, since the
player would not tell their windows apart. A script may not be named `config`. A `plugins/<name>/` that holds no
library (only a plugin's leftover settings) is not taken: the install goes in beside those files.

**What it holds.** No programs but a plugin's library, Lua only in a script or in a plugin's `ui/`, no hidden or
device names.

- A script has `init.lua` at its top.
- A plugin has exactly one library at its top, named after it (`<name>.so`, `lib<name>.so` ...).
- A tool: see Tools (names shaped as the server's files, `net::is_safe_file_path_shape`).
- An upscaler: see Upscalers (the same shape).

### Signatures

An add-on's manifest may carry its author's key and a signature. The key is pinned at the first install.

- An update not signed with the pinned key (another key, or none) is refused. The row says so, and neither
  auto-update nor Update gets past it. Installing again from a link is refused the same way.
- To trust a new key, the player removes the add-on and installs it again.
- An unsigned add-on that becomes signed takes the key from then on.

The signature proves each version comes from whoever holds the key, not merely whoever can push to the repository.

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
  install.lock    held by the one Hailstone that unpacks, installs, removes or records add-ons in this folder
scripts/<name>/   an installed script
plugins/<name>/   an installed plugin
tools/<name>/     an installed tool (its program, its libraries, its data)
upscalers/<name>/ an installed upscaler (its provider, the libraries it loads, their notices)
ui-addons/<addon>/ an installed interface add-on (its ui/<pack>/ folders, UI packs above)
```

- **An update replaces only the files the add-on shipped,** and takes out those the newer version no longer ships.
  The player's other files there stay (a plugin's settings beside it, `host->config_dir`; notes), as do empty folders
  other than those the update emptied or where a file now goes. A folder that still holds the player's files stops the
  update.
- **A removal** takes out the add-on's files and record. The player's files stay; the folder goes once empty.
- **A cut-off install or removal** (the client died between moves) is undone at the next start from the journal.
- **One Hailstone at a time** unpacks, installs, removes or writes `installed.txt` in a folder: each takes
  `addons/install.lock` (`lock_addons`, released when the process ends). Two windows started on their own would
  otherwise undo each other's journal at start, empty each other's `new/`, or write `installed.txt` from an older read
  and lose a line (a lost line makes that add-on's files hand-installed ones, with every capability on a server
  without `hailstone.cfg`). While another process holds it:
  - the start leaves the journal, `new/` and `previous/` alone (that install is under way, not cut off); a later
    start looks again;
  - what is downloaded stays staged, and the login screen installs it once the lock is free;
  - a download's unpacking, a removal and a change of the auto-update or enable switch fail with a message, to be
    tried again.

  Within one process the hold is shared: its add-on thread may unpack one add-on while the login screen installs
  another, as before.
- **The core updater** (docs/updater.md) never writes `addons/`. It skips its optional plugin package for a folder an
  add-on installed (checked at download and again at install). `scripts/`, `tools/`, `upscalers/` and `ui-addons/`
  are the player's to it already.
  Its `core` package's files in such a plugin folder are left out of the install, and those it shipped there before
  are not taken out.

#### Undoing a cut-off install

- `installed.txt` is saved as `installed.prev.txt` before it is rewritten and put back with the old files, so the list
  matches the disk. One that wrote `done` is left as it is.
- Each journal line is flushed before the move it names (write-ahead). The lines go in two batches of one synced
  write each, the old files' moves out and then the new files' moves in; every place a batch moves to is checked free
  before its lines are written, so a line for a move a cut-off install never made finds nothing there and the undo
  skips it.
- Each move is on the disk before the next, and
  each unpacked file is flushed before it moves: a power cut leaves no empty file and no moved file without its line.
- A file left in `previous/` after an undo (a lost or damaged journal, a failed move back) may be the only copy of the
  player's old file, so `previous/` is renamed `kept-<time>/` with a warning in the log, not removed.

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
`--screenshot`); the list still shows what is installed, a removal works, and the ceilings apply. The client sends
nothing but the `Hailstone/<version>` User-Agent.

For screenshots: `test.login=addons` in `ui/settings/<pack>.txt` (in the Hailstone folder) opens the panel as the
login screen first appears. In the Modern pack `test.addontab=store` (or `mine`) picks its tab.

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
| `app/addon_requirements.*`, `app/addon_upscaler_policy.*` | the opted-in server add-on policy's entry check, and its check of the running upscaler (docs/plugins.md "Opt-in add-on policies") |
| `app/client_app_addons.cpp`, `app/client_app_addon_upscaler.cpp` | the client's part: the view, the actions, when files may move, starting a tool (`start_addon_tool`), whether the running upscaler meets the server's policy |
| `plugin/policy.*` | `Ceiling`, `parse_caps_strict`, `caps_described` |
| `app/lua_ui_pregame.cpp` | `view.addons`, `hs.LoginSet("addonLink" \| "addonsOpen")`, `hs.LoginDo("addonInspect" \| "addonInstall" \| "addonCancel" \| "addonUpdate" \| "addonRemove" \| "addonAuto" \| "addonFolder" \| "addonPage" \| "addonCheck" \| "addonRevoke" \| "addonAskAgain" \| "addonSuggestLook" \| "addonSuggestLater" \| "addonSuggestNever" \| "addonBrowse" \| "addonStoreLook" \| "addonStart" \| "addonEnabled")`, `view.addons.suggestions`, `view.addons.store`; `hs.ServerSelect().addonHint` |
| `ui/modern/login.lua`, `ui/gamepad/login.lua`, `src/ui/dev_screens.cpp` | the panel, and the suggestions under the login card |
| `ui/modern/serverselect.lua`, `ui/gamepad/serverselect.lua` | the server select's line about a suggested add-on |

A plugin's windows get none of this: `hs.LoginScreen` is nil for them, as for every pre-game screen, so a plugin
cannot install another.

## Tests and what is not checked

Tests: `tests/addon_*_test.cpp` (manifest, install, manager against GitHub in memory, index and store, upscaler,
tool, requirements, upscaler policy), plus `tests/plugin_policy_test.cpp`, `tests/lua_ui_pregame_test.cpp`,
`tests/ui_pack_windows_test.cpp` and `tests/dev_ui_test.cpp`.

Not checked yet:

- **The real GitHub.** Fetching from a real public repository, through GitHub's redirects to its asset hosts, has not
  been tried.
- **Windows.** The WinHTTP final-address check only compiles; it has not run.
- **The store live.** Checked against GitHub in memory and drawn in tests; it has not listed the real index.
- **Suggested add-ons live.** Checked in memory and drawn in tests; a first login to a real server that lists a
  suggested world has not been tried.
- **macOS.** No plugin add-on has been installed there.
