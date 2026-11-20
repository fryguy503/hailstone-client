# The updater

Hailstone looks for a new release when it starts and says when one is out. Nothing is downloaded or installed
unless the player asks. **Download** fetches it in the background, and it is installed when they quit (or at once
with **Restart now**). `update.download=1` makes the download automatic, for a player who wants that.

A Hailstone feature: the original was patched by a separate program before the game started.

Code: `src/app/updater.*` (check, download), `src/app/update_install.*` (install, rollback),
`src/app/client_app_update.cpp`, and `src/core/update_manifest.*`, `src/core/zip_read.*`, `src/core/ed25519.*`,
`src/platform/http_get.*`.

## What the player sees

- **The corner.** The login screen's top left corner always shows this build ("Hailstone 0.4.0-alpha") and the
  updater's word on it (`ui::update_status_text`):
  - "Checking for updates...", "Up to date", "Version 0.5.0 is out";
  - "Could not check for updates" (offline, no manifest yet: the reason is only logged);
  - "Updates are off: <why>" (a development build, `update.check=0`, no HTTPS, a test run, a box set's other window).

  Under it, **What's new** (this version's part of the shipped `CHANGELOG.md`, `core/changelog.h`; greyed when
  missing) and **GitHub** (CMake `HAILSTONE_PROJECT_PAGE`, in the system browser). The Gamepad pack puts both under
  More, About Hailstone.
- **A newer release.** "Hailstone 0.2.0-alpha is out (this is 0.1.0-alpha)." with **Download**, **Release notes**
  and **Skip** (the Gamepad pack: under Log in). The notice shows the download's progress. With `update.download` on
  it starts by itself.
- **Once downloaded:** "Hailstone 0.2.0-alpha is downloaded: it is installed when you quit." with **Restart now**,
  which quits, installs and starts the new version.
  - A plain quit installs only an update the player asked for in this run (Download pressed, or `update.download`
    on, `update_installs_at_quit`).
  - A package an earlier run left downloaded shows "Hailstone 0.2.0-alpha is downloaded." and waits for Restart
    now.
- **Skip** writes `update.skip=<version>`: that release is not offered again, a newer one is. Skip before an
  automatic download has begun stops it starting.
- A release with no package for the player's system (say, no Linux arm64 build) is shown with its Release notes only.
- For packs: the notice is `view.update` (`ui::LoginView::update`, text from `ui::update_notice_text`) with
  `hs.LoginDo("updateDownload" | "updateRestart" | "updateSkip" | "updateNotes")`; the corner is `view.about`
  (`version`, `build`, `status`, `text`, `notes`, `page`) and `hs.LoginDo("openProject")`.

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

The feed is the newest GitHub release's `hailstone-update.txt`: CMake's `HAILSTONE_UPDATE_FEED`, by default
`https://github.com/projecthailstone/hailstone-client/releases/latest/download/hailstone-update.txt`, which GitHub
redirects to the newest release that is neither a draft nor a pre-release.

A plain download, not the REST API, so the API's 60 requests an hour do not apply. The client sends no identifier
beyond a `Hailstone/<version>` User-Agent. The repository must be public (the client carries no token); a separate
public releases repository works too, with only the feed address changed.

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
- **Packages:** one per platform, `core`: the whole folder, `browser/` included, the same zip as a first install.
  Every update downloads it whole (about 170 MB on Linux, 190 MB on Windows). The server plugins are add-ons
  (docs/addons.md), not in it.
- **Other components:** the format and the client also take several packages for a platform:
  - `browser` for `browser/` alone;
  - `plugin-<name>` for a plugin's `plugins/<name>/`, whose last field marks it optional: installed only where that
    folder exists, and never where an add-on installed it, checked at the download and again at the install
    (docs/addons.md).

  A component whose package hash equals the one installed last (`update/installed.txt`) is not downloaded again.
  Releases do not use them, to keep one download per platform.
- **Unknown records** are skipped, for later clients. A known record that does not parse refuses the whole manifest.
- **Versions** are SemVer and are compared by its precedence (`core/version.h`): `0.1.0-alpha` is older than
  `0.1.0-beta`, which is older than `0.1.0`. The build's own version is `project(VERSION)` plus `HAILSTONE_VERSION_LABEL`
  in `CMakeLists.txt` (`release_version()`), so a release has to change both.

#### Old release files

Releases up to 0.2.0-alpha shipped the server plugins in `plugins/nms/` and `plugins/projecttriune/`, which the
install cannot remove (a hand-unpacked folder has no `installed.txt`). Once the login screen is up and no update is
pending, the client removes each file from a table of those releases' files whose size and SHA-256 still match
(`retire_old_release_files`).

- A file the player changed stays, as do settings a plugin wrote; the add-on installs in beside them.
- A plugin folder whose library is not one of the shipped builds (the add-on's, or a local build) keeps every file,
  since the add-ons still ship some of the same window files (Project Triune's map, `triunewaypoints.lua`).
- The table also holds pack files a release renamed, which a hand-unpacked folder would keep beside the new name:
  `ui/modern/eqmain.lua` (0.5.0-beta to 0.7.1-beta), now `game_menu.lua`.
- A pack's folder stays, even if that empties it.

### The release key

The manifest is signed with Ed25519 (RFC 8032, through Monocypher). The signature covers every byte before the
`signature` line.

The client holds the public half (`update-key.pub` at the top of the tree, or CMake's `HAILSTONE_UPDATE_KEY`) and
checks the signature before trusting anything in the manifest. Each package is checked against the manifest's size
and SHA-256 after the download and again before the install.

- **Why sign:** TLS and GitHub's digests prove only that a file came from GitHub. Someone with write access to the
  releases but not the key cannot make clients run their code.
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

The client installs the update itself once it has quit (end of `run_client`, after its window, the browser helper
and the plugins are gone):

1. Each package is checked again and unpacked into `new/`. Refused whole: a package that holds one of the player's
   paths, and a plugin package with files outside `plugins/<name>/`. The player's paths (`is_player_path`, and
   `tools/make_update.py`'s copy of it) are:
   - the files `hailstone.ini`, `hailstone-windows.ini` and `notes.txt`;
   - the folders `update/`, `servers/`, `logs/`, `cache/`, `screenshots/`, `bug-reports/`, `maps/`, `scripts/`,
     `textures/`, `web/`, `addons/` and `tools/` (the last two the add-ons', docs/addons.md), and `ui-data/` and
     `ui-addons/` (the player's interface data and the UI add-ons);
   - `ui/settings/` and `ui/loadingbackgrounds/`.

   An update never writes or removes any of them.

   A plugin folder an add-on installed into (`plugins/<name>/`, `addon_owns_folder`) is the add-on's too: plugin
   packages and the core package's files for it are left out of the install and of `installed.txt`.

   Also refused before anything moves:
   - a package with a file whose path in the install folder is a folder (an update replaces files only, and such a
     folder may hold files the update did not put there);
   - an update whose packages' declared sizes add up to more than 4 GiB.

   Each unpacked file is flushed to the disk before it is renamed into `new/`.
2. Each old file is moved into `previous/` and its new one into place.
   - Each move's journal line is flushed before the move (write-ahead), so after a power cut every file in
     `previous/` has its line.
   - On Linux and macOS `previous/` gets a hard link to the old file, which stays in place until the new one is
     renamed over it, so no moment leaves a name (the program itself among them) without a file. On Windows, and on a
     file system without hard links, the old file is renamed away first (see "Limits and open points").
   - Both are renames on one volume, which Windows allows for a running program and its loaded libraries, and each
     is on the disk before the next begins.
3. The files a component shipped before and no longer does are moved into `previous/` as well.
4. `installed.txt` is written, and `state.txt` becomes `pending`.

The install waits while the box set's other windows run (they use the same files), and happens at a later quit.
Those windows are started with `--box-child` (`app/updater.h` `kBoxChildArg`) and have no updater and no add-on
downloads of their own: only the first window downloads into `download/` and `addons/` and installs, so two windows
never write the same `.part` file, and a window that quits first never installs while its siblings run.

### Start and rollback

`update_at_start`, the first thing `main` does:

- **`installing`** (the client died mid-install): the journal is undone and the old version is back. If this binary
  is the new one, the old one is started and this one quits.
  - A `moved` or `removed` line with nothing in `previous/` never happened, so the original in place stays.
  - Only whole lines are undone: a last line without its newline was cut off before its move and may be part of a
    name (`added ui` of `added ui/x.lua`).
  - A line whose name is not a file's path inside the folder (`is_safe_zip_path`) or is one of the player's paths is
    skipped.
  - A folder is never moved: one where a line points holds what the update did not put there (`ui/settings/` under
    `ui/`).
  - A file still in `previous/` after the undo (a damaged journal, a failed move back) may be the only copy, so
    `previous/` is renamed `kept-<time>/` with a warning, not removed.
- **`pending`:** each start of the new version is counted, and reaching the login screen ends the count
  (`update_started_fine` removes `previous/`). If two starts never reached it, the third rolls the update back, writes
  `failed.txt`, starts the old version and quits. The old version shows why (`note.txt`).
- **A different version than the pending one** (the player put one in by hand) ends the pending state.

A launcher's ticket launch (`/ticket:`, `--ticket`, `--ticket-env`; docs/protocol/login.md "Launcher tickets") skips
`update_at_start`, whose restart would lose the one-use ticket; the next start without a ticket does that work. The
updater and the add-on manager otherwise run as usual.

## Making a release

1. Set the version (`project(VERSION)`, `HAILSTONE_VERSION_LABEL`) and build each platform's Release, with
   `-DHAILSTONE_BROWSER=ON`, on a clean checkout of the release commit (docs/ci.md):
   - Linux x64 in the `hailstone-linux-build:24.04` container (`tools/docker/linux.Dockerfile`), so it runs on
     Ubuntu 24.04 and newer;
   - Windows x64 with MSVC under Wine (`tools/docker/msvc_build.sh`, docs/ci.md "MSVC under Wine");
   - macOS arm64 is on hold (docs/plan.md, 0.9) and not part of a release for now. When a test build is wanted, the
     macOS workflow makes it on the release commit (`gh workflow run macos.yml -f ref=<commit>`, docs/ci.md "macOS
     arm64"): it builds, tests, installs and signs ad hoc, and its artifact is step 2's folder.

   For the first two, `CMAKE_ARGS=-DHAILSTONE_BROWSER=ON tools/docker/build.sh linux-x64` (and `windows-x64`;
   docs/ci.md "Builds in containers") does this step and step 2, leaving `dist/<platform>/hailstone`. Under Wine it
   leaves out the tests that only fail there (docs/ci.md "Builds in containers"): run those on Windows by hand.

   Run each platform's `hailstone_tests` (Windows under Wine) before going on; the macOS workflow runs its own.
2. Install each platform's client (without the server plugins, which are released as add-ons, each by a
   `tools/release.sh` in its own repository; this repository has none):

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

   This writes `hailstone-0.2.0-alpha-linux-x64.zip` (no top folder) and its line in `hailstone-update.txt`. The zip
   is reproducible (sorted, dated 1980-01-01, Unix permissions kept). Files and folders whose name starts with `.`
   are left out. A symbolic link goes in as its target, which must lie inside `--dist` (a link out of it, or back into
   a folder holding it, stops the tool); `--out` may not lie inside `--dist`. A name the updater's zip reader would
   refuse (core/zip_read.cpp `is_safe_zip_path`) stops it too.
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

This prints the public half, for `update-key.pub`. The key file is owner-only (0600 on Linux and macOS; on Windows
its folder's permissions) and never written over an existing file.

## Limits and open points

- **Files the first update cannot remove.** A hand-extracted folder has no `installed.txt`, so its first update
  removes nothing. From the second on, files a release drops are removed.
- **Untested platforms.** The Windows paths (WinHTTP, renaming a running `hailstone.exe`) have only been checked to
  compile: they have not run.
- **macOS.** The release is a plain folder like Linux's (`hailstone` beside `libMoltenVK.dylib`, `libSDL3.0.dylib`
  and the shipped folders), signed ad hoc only: no Developer ID and no notarization, by choice.
  - A download is quarantined, so Gatekeeper refuses it until the player clears that (README.md "Download"). The
    updater's own downloads are not, and install as on Linux.
  - An app bundle would need its own install step (the binary is in `Contents/MacOS/`, while `install_dir()` would
    be `Contents/Resources/`) and is not made.
  - No macOS release has been run on a Mac yet, nor its update.
- **Windows: a moment without the program.** Windows lets a running program's files be renamed but not replaced, so
  the old `hailstone.exe` (and each other file) is moved into `previous/` before the new one is moved in.
  - Both moves are on the disk before the next begins, but a power cut exactly between the two leaves the folder
    without `hailstone.exe`.
  - The player then moves `update/previous/hailstone.exe` back by hand, and the next start rolls the rest of the
    cut-off install back (a `moved` line with nothing in `previous/` leaves the file in place).
  - Linux and macOS keep the old file in place until the new one replaces it in one rename.
- **Downloads start over.** A broken-off download starts again from the beginning (no HTTP range requests). A
  package already downloaded whole is kept.
- **Other Hailstone windows** the client did not start (not the box set's) are not seen. On Linux they run on; on
  Windows their open files are moved like the quitting client's.
- **The CI job.** Releases are published by hand; a tag-triggered CI job would need the signing key in CI.
