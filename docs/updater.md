# The updater

Hailstone looks for a new release when it starts and says when one is out. Nothing is downloaded or installed
unless the player asks: **Download** fetches it in the background, and it is installed when they quit (or at once
with **Restart now**). `update.download=1` makes the download automatic, for a player who wants that. This is a Hailstone feature with no counterpart in the original client: the original was kept up to date by
a separate patcher before the game started. The code is `src/app/updater.*` (check and download),
`src/app/update_install.*` (install and rollback), `src/app/client_app_update.cpp` (the client's part),
`src/core/update_manifest.*`, `src/core/zip_read.*`, `src/core/ed25519.*` and `src/platform/http_get.*`.

## What the player sees

- **The corner.** The login screen's top left corner always shows this build ("Hailstone 0.4.0-alpha") and the
  updater's word on it (`ui::update_status_text`): "Checking for updates...", "Up to date", "Version 0.5.0 is out",
  "Could not check for updates" (offline, no manifest yet: the reason is only logged) or "Updates are off: <why>" (a
  development build, `update.check=0`, no HTTPS, a test run). Under it: **What's new**, a popup with this version's
  part of the `CHANGELOG.md` shipped with the client (`core/changelog.h`; the button is greyed when the file or the
  section is missing), and **GitHub**, which opens Hailstone's page (CMake `HAILSTONE_PROJECT_PAGE`) in the system browser.
  The Modern pack draws it as a small card; the Gamepad pack shows the two lines there and puts What's new (a page)
  and the GitHub link under More, About Hailstone; the client's own ImGui screen draws a small window.
- When a newer release is out, the corner says so in a sentence ("Hailstone 0.2.0-alpha is out (this
  is 0.1.0-alpha).") with **Download**, **Release notes** and **Skip** (the Gamepad pack: under Log in). Download starts the download
  and the notice shows its progress; with `update.download` on (off by default) it starts by itself.
- Once downloaded: "Hailstone 0.2.0-alpha is downloaded: it is installed when you quit." with **Restart now**, which
  quits, installs and starts the new version. A plain quit installs only an update the player asked for in this run
  (Download pressed, or `update.download` on, `update_installs_at_quit`). A package an earlier run left downloaded,
  as clients up to 0.7.0-beta did without asking, shows "Hailstone 0.2.0-alpha is downloaded." and waits for Restart now.
- **Skip** writes `update.skip=<version>`: that release is not offered again, a newer one is. Skip pressed while the
  notice shows, before its automatic download has begun, stops that download from starting.
- A release with no package for the player's system (say, no Linux arm64 build) is shown with its Release notes only.
- The notice is part of the login view (`ui::LoginView::update`), so the Modern and Gamepad packs (`ui/*/login.lua`,
  `view.update`, `hs.LoginDo("updateDownload" | "updateRestart" | "updateSkip" | "updateNotes")`) and the client's own
  ImGui screen all show it the same way (`ui::update_notice_text`). The corner is `view.about` (`version`, `build`,
  `status`, `text`, `notes`, `page`) and `hs.LoginDo("openProject")`.

## Settings (`hailstone.ini`)

| Key | Default | |
|---|---|---|
| `update.check` | 1 | look for a release at start |
| `update.download` | 0 | download a newer one without asking (1: automatic, and a plain quit installs it) |
| `update.install_on_quit` | 1 | install a downloaded one the player asked for at a plain quit (Restart now always installs) |
| `update.feed` | the build's | the manifest's address, for testing a release before it is published |
| `update.skip` | | a release not to offer |

Updates are off, whatever the settings say, in these cases (the log says why):
- **A development tree:** `CMakeCache.txt` beside the Hailstone folder, as in `build/hailstone/`.
- **A build without a release key.**
- **No HTTPS on the system:** on Linux and macOS, the system's libcurl could not be loaded.
- **Test runs:** `--frames` or `--screenshot`.

## The release feed

The feed is the newest GitHub release's `hailstone-update.txt`. CMake's `HAILSTONE_UPDATE_FEED` defaults to
`https://github.com/projecthailstone/hailstone-client/releases/latest/download/hailstone-update.txt`, and GitHub
redirects that link to the newest release that is neither a draft nor a pre-release. It is a plain download, not
the REST API, so the API's limit of 60 requests an hour for each address does not apply. The client sends no
identifier beyond a `Hailstone/<version>` User-Agent.

The repository must be public for this to work. GitHub answers 404 to every unauthenticated request for a private
repository's releases, and the client carries no token. Publishing the releases from a separate public repository
works as well: only the feed address changes.

### The manifest

The format is described in `core/update_manifest.h`:

```
hailstone-update 1
version 0.2.0-alpha
notes https://github.com/projecthailstone/hailstone-client/releases/tag/v0.2.0-alpha
package linux-x64 core 170114879 <sha256> hailstone-0.2.0-alpha-linux-x64.zip
package windows-x64 core 193762460 <sha256> hailstone-0.2.0-alpha-windows-x64.zip
signature <128 hex digits>
```

- **Package file names** are taken relative to the manifest's address, so they name assets of the same release.
- **Sizes** are a limit as well as a check: a download stops as soon as it passes the manifest's size.
- **Packages:** a release has one package for each platform, `core`: the whole folder, `browser/` included. The
  server plugins are not in it: they are add-ons (docs/addons.md). The releases that still shipped them (0.1.0-alpha's
  NMS download, 0.2.0-alpha) left their libraries and windows in `plugins/nms/` and `plugins/projecttriune/`, and the
  install cannot take them out: a folder unpacked by hand has no `installed.txt`, and an update from 0.2.0-alpha is
  installed by 0.2.0-alpha's own code. So from 0.3.0-alpha on the client removes them itself once the login screen is
  up and no update is pending (`retire_old_release_files`): each file of a table of those releases' files whose size
  and SHA-256 still match. A file the player changed stays; the settings a plugin wrote in its folder stay, and the
  add-on installs in beside them. A plugin folder whose library is another build than the ones shipped (the add-on's,
  or one built from the plugin's repository) belongs to that plugin now and keeps every file: the add-ons still ship
  some window files unchanged since 0.2.0-alpha (Project Triune's map, `triunewaypoints.lua`), which 0.3.0-alpha
  removed at every start. It is the same zip a player downloads for a first
  install, so the release page holds only that zip for each platform and this manifest. Every update downloads it
  whole (about 170 MB on Linux, 190 MB on Windows).
- **Other components:** the format and the client also take several packages for a platform: `browser` for
  `browser/` alone, and `plugin-<name>` for a plugin's `plugins/<name>/`, whose last field marks it optional
  (installed only where that folder exists, and never where an add-on installed it, checked at the download and
  again at the install: docs/addons.md). A component whose package hash equals the one installed last
  (`update/installed.txt`) is not downloaded again. Releases do not use them at present, to keep the release page
  to one download for each platform.
- **Unknown records** are skipped, for later clients. A known record that does not parse refuses the whole manifest.
- **Versions** are SemVer and are compared by its precedence (`core/version.h`): `0.1.0-alpha` is older than
  `0.1.0-beta`, which is older than `0.1.0`. The build's own version is `project(VERSION)` plus `HAILSTONE_VERSION_LABEL`
  in `CMakeLists.txt` (`release_version()`), so a release has to change both.

### The release key

The manifest is signed with Ed25519 (RFC 8032, through Monocypher). The signature covers every byte before the
`signature` line. The client holds the key's public half (`update-key.pub` at the top of the tree, or CMake's
`HAILSTONE_UPDATE_KEY`) and checks the signature before it trusts anything in the manifest; each package is then checked
against the manifest's size and SHA-256 twice, once after the download and again before the install.

- **TLS and GitHub's own asset digests** prove only that a file came from GitHub. The signature proves it came from
  whoever holds the release key. Someone with write access to the releases, but without the key, cannot make the
  clients run their code.
- **The same key signs the add-on index** (docs/addons.md "Suggested add-ons"); `hailstone_sign sign` gives an
  index its serial.
- **Keep the secret key off GitHub,** including Actions secrets, unless releases are signed in CI on purpose. Whoever
  has it can run code on every Hailstone install.
- **Changing the key** needs a release signed with the old key that carries the new public key. After that, only
  manifests signed with the new key are accepted.

## The install

The install is described in `app/update_install.h`. Everything lives in `<Hailstone folder>/update/`:

| Path | |
|---|---|
| `download/` | the packages, each checked by size and SHA-256 |
| `ready.txt` | the staged update: its version and packages |
| `installed.txt` | each component's installed package hash and its files |
| `new/` | the packages unpacked, before anything in the folder changes |
| `previous/` | the files the last install replaced or removed, kept until the new version starts |
| `journal.txt` | the install's moves, one a line |
| `state.txt` | `installing <new> <old>`, then `pending <new> <old> <starts>` |
| `failed.txt` | a release that was rolled back, and is not offered again |
| `note.txt` | a rollback's message for the version it starts |
| `trash/` | files that were in use when moved, removed at the next start |
| `kept-<time>/` | what a rollback found in `previous/` that the journal did not name (see below); never removed by the client |

The client installs the update itself once it has quit, at the end of `run_client`, after its window, the browser
helper and the plugins are gone:

1. Each package is checked again and unpacked into `new/`. A package that holds one of the player's paths is refused
   whole, and so is a plugin package with files outside `plugins/<name>/`. The player's paths (`is_player_path`,
   and `tools/make_update.py`'s copy of it) are:
   - the files `hailstone.ini`, `hailstone-windows.ini` and `notes.txt`;
   - the folders `update/`, `servers/`, `logs/`, `cache/`, `screenshots/`, `bug-reports/`, `maps/`, `scripts/`,
     `textures/`, `web/`, `addons/` and `tools/` (the last two the add-ons', docs/addons.md);
   - `ui/settings/` and `ui/loadingbackgrounds/`.

   An update never writes or removes any of them. A plugin folder an add-on installed into (`plugins/<name>/`,
   `addon_owns_folder`) is the add-on's too: a plugin package for it is left out, and the core package's files there
   are left out of the install (and of `installed.txt`), as are those it shipped there before and no longer does.
   A package whose file would go where the folder has a folder is refused before anything moves: an update replaces
   files only, and that folder holds what it did not put there. So is an update whose packages would unpack to more
   than 4 GiB in all (their declared sizes added up before anything is written; a release is a few hundred MB).
   Each unpacked file is flushed to the disk before it is renamed into `new/`.
2. Each old file is moved into `previous/` and its new one into place. The move is written to the journal before
   it is made, and the journal's line is flushed to the disk first (write-ahead), so after a power cut every file in
   `previous/` has its line. Both moves are renames on one volume, which Windows allows for a running program and its
   loaded libraries; each is on the disk before the next begins (the folder synced on Linux and macOS, a write-through
   move on Windows).
3. The files a component shipped before and no longer does are moved into `previous/` as well.
4. `installed.txt` is written, and `state.txt` becomes `pending`.

The install waits while the box set's other windows run (they use the same files), and happens at a later quit.

### Start and rollback

`update_at_start`, the first thing `main` does:

- **`installing`** (the client died mid-install): the journal is undone and the old version is back. A `moved` or
  `removed` line with nothing in `previous/` is a move that never happened (the line is written first), so the file
  in place, the original, stays. If this binary is the new one, the old one is started and this one quits. Only the
  journal's whole lines are undone: a last line without its newline was cut off as it was written, before its move,
  and may be part of a name (`added ui` of `added ui/x.lua`). A line whose name is not a file's path inside the
  folder (`is_safe_zip_path`) or is one of the player's paths is skipped, and a folder is never moved: an update
  moves files only, so a folder where a line points holds what it did not put there (`ui/settings/` under `ui/`).
  After the undo `previous/` holds no file. One still there (a journal the disk lost or damaged, a move back that
  failed) may be the only copy of a replaced file, so `previous/` is renamed `kept-<time>/` and a warning logged,
  instead of being removed.
- **`pending`:** each start of the new version is counted, and reaching the login screen ends the count
  (`update_started_fine` removes `previous/`). If two starts never reached it, the third rolls the update back, writes
  `failed.txt`, starts the old version and quits; the old version shows why (`note.txt`).
- **A different version than the pending one** (the player put one in by hand) ends the pending state.

A launcher's ticket launch (`/ticket:`, `--ticket`, `--ticket-env`; docs/protocol/login.md "Launcher tickets") skips
`update_at_start`: its restart starts the other version without the command line, and a one-use ticket would be lost
with it. The next start without a ticket does that work. The updater and the add-on manager otherwise run as on any
player's run.

## Making a release

1. Set the version (`project(VERSION)`, `HAILSTONE_VERSION_LABEL`) and build each platform's Release, with
   `-DHAILSTONE_BROWSER=ON`, on a clean checkout of the release commit (docs/ci.md):
   - Linux x64 in the `hailstone-linux-build:24.04` container (`tools/docker/linux.Dockerfile`), so it runs on
     Ubuntu 24.04 and newer;
   - Windows x64 with MSVC under Wine (`tools/docker/msvc_build.sh`, docs/ci.md "MSVC under Wine");
   - macOS arm64 by the macOS workflow, started on the release commit (`gh workflow run macos.yml -f ref=<commit>`,
     docs/ci.md "macOS arm64"). It builds, tests, installs and signs ad hoc; its artifact is step 2's folder.

   For the first two, `CMAKE_ARGS=-DHAILSTONE_BROWSER=ON tools/docker/build.sh linux-x64` and the same with
   `windows-x64` (docs/ci.md "Builds in containers") do this step and step 2 from the same Dockerfiles, leaving each
   folder in `dist/<platform>/hailstone`. Under Wine they leave out four tests that only fail there: run those on
   Windows by hand.

   Run each platform's `hailstone_tests` (Windows under Wine) before going on; the macOS workflow runs its own.
2. Install each platform's client (without the server plugins, which are released as add-ons by their own
   repositories' `tools/release.sh`):

   ```sh
   cmake --install build --prefix dist/hailstone
   ```

   On Windows, the default install also writes zlib's `bin/`, `include/` and `lib/`; leave them out of `dist/`.
   For macOS, unpack the workflow's artifact instead, into a folder of its own:

   ```sh
   gh run download <run> -n hailstone-macos-arm64 -D dist-macos && tar -C dist-macos -xf dist-macos/hailstone-macos-arm64.tar
   ```

   and pass `--platform macos-arm64 --dist dist-macos/hailstone` in step 3.
3. Make each platform's download, with the same `--out` for every platform:

   ```sh
   tools/make_update.py --version 0.2.0-alpha --platform linux-x64 --dist dist/hailstone \
       --notes https://github.com/projecthailstone/hailstone-client/releases/tag/v0.2.0-alpha --out release/
   ```

   This writes `hailstone-0.2.0-alpha-linux-x64.zip` (the folder's files with no top folder; a player unpacks it
   into a folder of their own) and its line in `hailstone-update.txt`. The zip is made the same way each time
   (sorted, dated 1980-01-01, the files' Unix permissions), and a symbolic link goes in as the file or folder it
   points to (a link that leads back into a folder holding it stops the tool).
4. Sign the manifest:

   ```sh
   build/hailstone_sign sign <secret-key> release/hailstone-update.txt
   ```

   Then check it:

   ```sh
   build/hailstone_sign verify update-key.pub release/hailstone-update.txt
   ```

5. Try the update on a copy of the last release's folder before uploading. Serve `release/` (for example with
   `python3 -m http.server`), then run:

   ```sh
   build/hailstone_sign try update-key.pub http://127.0.0.1:8000/hailstone-update.txt <copy> <old-version>
   ```

   This runs the client's updater against the feed and installs into the copy.
6. Upload everything in `release/` to the GitHub release: each platform's zip and `hailstone-update.txt`. Releases
   are not marked as pre-releases, since the feed is the newest release that is not one.

The key is made once:

```sh
build/hailstone_sign keygen <secret-key>
```

This prints the public half, which goes in `update-key.pub`. The key file is made readable by its owner only
(0600 on Linux and macOS, from the moment it exists; on Windows it takes its folder's permissions), and never over a
file already there.

## Limits and open points

- **Files the first update cannot remove.** A folder that was extracted by hand has no `installed.txt`, so its first
  update cannot tell the files that were shipped from the player's own, and removes nothing. From the second update
  on, files a release drops are removed.
- **Untested platforms.** The Windows paths (WinHTTP, renaming a running `hailstone.exe`) have only been checked to
  compile: they have not run.
- **macOS.** The release is a plain folder like Linux's (`hailstone` beside `libMoltenVK.dylib`, `libSDL3.0.dylib`
  and the shipped folders), signed ad hoc only: no Developer ID and no notarization, by choice. A download is
  quarantined, so Gatekeeper refuses it until the player clears that (README.md "Download"). The updater's own
  downloads (libcurl) are not quarantined, and it installs into the folder as on Linux. An app bundle would need
  its own install step (the binary is in `Contents/MacOS/`, while `install_dir()` would be `Contents/Resources/`)
  and is not made. No macOS release has been run on a Mac yet, nor its update.
- **Downloads start over.** A broken-off download starts again from the beginning (no HTTP range requests). A
  package already downloaded whole is kept.
- **Other Hailstone windows.** Windows the client did not start itself (not the box set's) are not seen; on Linux they
  run on, while on Windows the files they have open are moved like the quitting client's.
- **The CI job.** Publishing a release (packaging, signing, uploading) is done by hand. A tag-triggered CI job would
  need the signing key in CI.
