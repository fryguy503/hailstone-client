# Continuous integration and local builds

GitHub Actions runs checks only (`.github/workflows/ci.yml`), on every push to `main`, `cloud/**` or `claude/**` and
on pull requests to `main`:

- `core-names`: `tools/check_core_names.py` checks that the client's sources name no server outside comments. Lua
  long strings and C++ raw strings count as code, and Objective-C++ and the ini, JSON and text files under those
  folders are scanned too.
  - The old names pass only in their own spellings (`nmsc`, `nmsc_`, `nmsc::`, `NMSC_`, the SDK's `Nmsc<Word>`,
    `nms-client`), so `NMSChat` or `kNmsCombatOp` fails (tests/server_tools_test.cpp).
  - The one exception, let through by file and exact line shape: the updater's table of files old releases shipped
    under `plugins/nms/` (`retired_release_files()` in `src/app/update_install.cpp`).
- `wiki`: `tools/sync_wiki.py --check`. Every document of `docs/`, `CONTRIBUTING.md` and `server/README.md` has a
  wiki page, and no link points at a missing file (docs/wiki.md).
- `ui-api`: the shared UI contract's generated files are fresh.
  - `tools/migrate_ui_api.py --check` (the shipped packs call `hs.UI`) and its `--mock-adapter --check`
    (`tests/lua_ui_api_mock.lua`).
  - `tools/gen_ui_api_docs.py --check` (`docs/ui-api-reference.md`; it links each binding's file, not a line, so
    unrelated edits do not make it stale).
  - The Python tests of those tools and of `tools/make_addon.py`'s UI and autoload packages
    (`tests/test_migrate_ui_api.py`, `test_gen_ui_api_docs.py`, `test_make_addon_ui.py`,
    `test_make_addon_autoload.py`).
  - `tests/test_make_update.py`: `tools/make_update.py` keeps out of a package exactly the paths the client's
    `is_player_path` refuses.
  - `tests/test_release_tools.py`: the release packers' versions, names and contents (`tools/make_addon.py` and
    `tools/make_update.py`).

One workflow builds: `.github/workflows/macos.yml` ("macOS arm64" below), for the platform Linux cannot build,
started by hand or by a push to a `macos/**` branch.

Everything else is built and tested on the developer's machine, to save Actions minutes (the old build legs are in
the workflow's history). Before a merge to `main`:

| Platform | How | When |
|---|---|---|
| Linux | `cmake --build build && build/hailstone_tests` (AGENTS.md "Build and test"), with `EQ_GAME_DIR` set when the change touches assets | every merge |
| macOS arm64 | the macOS workflow (below): Apple Clang, `hailstone_tests` on GitHub's Apple Silicon runner | on hold until someone can test and maintain macOS (docs/plan.md, 0.9): not part of a release or a merge for now; run it by hand for a change to `src/platform/` or the build files when convenient |
| Windows | MSVC, CI's old compiler, under Wine: `tools/docker/msvc_build.sh`, then `wine build-msvc/hailstone_tests.exe` ("MSVC under Wine" below) | a change to `src/platform/`, the build files, the Lua bridge (`app/lua_ui*`) or anything a compiler may treat differently; every release |

Releases are built the same way (docs/updater.md "Making a release"):

- Linux in the `hailstone-linux-build` container (Ubuntu 24.04, so the client runs on 24.04 and newer).
- Windows with MSVC under Wine.

Run the sanitizer build (`-DHAILSTONE_SANITIZE=ON`, docs/hardening.md) and clang-tidy (below) by hand too, and keep
the code building with Clang and Apple Clang (AGENTS.md's portability rules).

## The tests

- No window, Vulkan device or audio device is needed (Vulkan is reached through volk). The window, GPU and
  audio-device code (`platform/window.cpp`, `render/`, `platform/audio_device.cpp`) is compiled and linked, not run.
- `tests/mock_*_test.cpp` run `LoginClient`, `GameConnection` and `Session` against an in-process mock EQEmu on
  loopback (`tests/mock_server/README.md`): about 4 s, on `127.0.0.1` ephemeral ports, with no game files.
  - A test that asserts a timeout or a hold runs its sessions on `tests/manual_clock.h` (`SessionOptions::clock`)
    and advances the time itself, so a stalled thread cannot fire or skip a timer.
- Several `hailstone_tests` run at once on a shared machine, so a test writes files in `tests/scratch_dir.h`'s
  `ScratchDir` (or `unique_temp_path`), never a fixed name under the temp folder: one run's `remove_all` would empty
  another's folders mid-test.
- With `EQ_GAME_DIR` unset or empty, the tests that need an installed RoF2 game are skipped where they are marked
  `doctest::skip`, and elsewhere return early and pass without checking anything (`tests/game_files.h`,
  `hailstone::test::game_dir()`). Run them with it set.
- `ctest --test-dir build -j <n>` runs `hailstone_tests` as 8 parts at once (`HAILSTONE_TEST_SHARDS`, a CMake
  setting; 1 registers the single test it was before). Each part is the same executable with
  `HAILSTONE_TEST_SHARD=<k>/<n>` set: it shuffles doctest's list with a fixed seed, so the slow UI pack cases spread
  over the parts, and runs its range (`tests/test_shard.h`). A part's cases run in another order than a plain
  `build/hailstone_tests`, which runs everything in one process in file order. The slowest case takes about 30 s,
  so more parts than cores gains little.
- The log's lines stay off the console during a run (`log::set_console`, `tests/test_main.cpp`): a run writes
  thousands of expected warnings (stand-in game folders without `racedata.txt` or `spells_us.txt`, scripts refused
  on purpose), which buried a real one. `HAILSTONE_TEST_LOG` set puts them back. The crash report's recent lines and
  a log file a test opens get every line either way.
- With `HAILSTONE_TEST_TRACE` set, each case's name goes to stderr as it starts (`tests/test_main.cpp`), so the last
  `[test]` line names a case that crashed the process before doctest's report.
- The browser test against the real CEF helper runs when the build has it (`HAILSTONE_BROWSER`). On Linux without
  a display it uses Chromium's headless platform.
- The server plugins live in repositories of their own (projecthailstone/hailstone-nms and the private
  projecthailstone/hailstone-projecttriune) and are released as add-ons by each one's `tools/release.sh`
  (docs/addons.md). Cloned into `plugins/nms` or `plugins/projecttriune`, each is built with the client and its tests
  join `hailstone_tests`. The Actions secrets `NMS_DEPLOY_KEY` and `TRIUNE_DEPLOY_KEY` are no longer used.

## Problems the first runs showed

Each fix left behaviour unchanged; they explain rules in AGENTS.md.

| Leg | Failure | Fix |
|---|---|---|
| Apple Clang (Xcode 15; GCC and recent Clang accept it) | `game_connection_market.cpp`: a lambda captured a structured binding (`serial`), which C++20 does not allow | capture a named reference |
| Apple Clang | `std::from_chars` for `double` is missing from Xcode 15's libc++ (`game/spell_record.cpp`, `assets/eqg.cpp`) | `core/float_chars.h`: `from_chars` where `__cpp_lib_to_chars` says it exists, else `strtod` with the same accepted syntax |
| MSVC | `__builtin_memset` and `__builtin_bit_cast` in `tests/cast_spell_vectors_test.cpp` | `std::memset`, `std::bit_cast` |
| MSVC | `hailstone_tests.exe` exits with 0xc0000135: `SDL3.dll` is not next to it (Windows has no rpath) | post-build copy of `$<TARGET_RUNTIME_DLLS:...>` for `hailstone_tests` and `hailstone` |
| MSVC under Wine (19.44, /O2) | Recorded vectors Social_Stop and GetFocusCastingTimeModifier: a wrapped 32-bit product divided by a constant, folded as if it had not wrapped (note 1) | `core/wrap_math.h` `wrapped_div`, and `wrapped_mul_div` (note 1) |
| MSVC under Wine | Recorded vectors CSpellBookWnd::GetSpellMemTicksLeft: `calc_dex() + calc_int()` called INT first | the calls as separate statements, in the original's order; the other query calls an operator or argument list ordered too |
| MSVC under Wine | "http_get: cancelled ends a transfer stalled waiting for the answer" takes about 21 s (note 2) | not a client fault (note 2) |
| MSVC under Wine | The build stops for good at `hailstone_engine.lib` (note 3) | `CMAKE_NINJA_FORCE_RESPONSE_FILE` when building for Windows on another host: the objects go in a response file |
| MSVC under Wine | 33 shared-cache, mapped-archive and animation tests: the cache never mapped (note 4) | `platform/shared_file.cpp` accepts a LocalSystem entry beside the user's (note 4) |
| Linux, Windows | CI set-up only (SDL3 dev packages; the glslang asset name and executable name) | see the workflow |

Notes:

1. The pattern is `static_cast<std::int32_t>(a * b) / k`. Use `wrapped_div` at every such division,
   `wrapped_mul_div` where a helper (`mul`, `wrap_mul`) made the product: inlined, `mul(a, b) / k` is the same
   pattern.
2. Wine's WinHTTP does not end a synchronous call when another thread closes the request handle. Under Wine the
   test's time is a doctest WARN, not a failure; only a run on Windows checks that the close ends the call at once.
3. The archive command, with every engine object on it, passed a Windows command line's 32,767 characters, and
   `lib.exe` never started.
4. Wine adds a full-access LocalSystem (S-1-5-18) entry to every DACL it reports, so `prepare_private_cache_directory`
   saw its own user-only folder as shared. SYSTEM passes file ACLs anyway; a Windows case in `shared_file_test.cpp`
   checks that a leaf granting Everyone is still refused. Wine also reports a DACL set on an existing folder from
   its Unix mode, so a test that needs one sets it when it creates the folder.

## Not covered

- A change merged without the builds above is not built anywhere.
- Linux arm64 is built only by hand, under QEMU on an x64 machine (`tools/docker/build.sh linux-arm64`, "Builds in
  containers" below). Windows arm64 is not built. macOS arm64 is built only when its workflow is started.
- No macOS build has been run on a Mac with a display; on the runner the window, MoltenVK and the rendering are
  compiled and linked only.
- Nothing runs against a real game install or a server unless run by hand.

## Windows builds from Linux

`cmake/toolchains/clang-cl-xwin.cmake` builds Windows x64 on Linux with clang-cl and lld-link against the MSVC C
runtime and the Windows SDK, which [xwin](https://github.com/Jake-Shadle/xwin) downloads from Microsoft (accepting
Microsoft's license).

```sh
xwin --accept-license --arch x86_64 splat --output ~/.xwin      # once, about 1 GB; HAILSTONE_XWIN_DIR names another folder
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/clang-cl-xwin.cmake
cmake --build build-windows
cmake --install build-windows --prefix dist/hailstone            # the client's folder, as a release has it
```

- It needs clang-cl, lld-link, llvm-lib, llvm-rc and llvm-mt (an LLVM install has them) and the build machine's
  glslang for the shaders.
- RelWithDebInfo writes `hailstone.pdb`; beside `hailstone.exe` it lets a crash report name the function.
- The tests run under Wine (`wine build-windows/hailstone_tests.exe`, with a `WINEPREFIX` of its own).

The compiler wrapper `clang-cl-xwin.sh` puts the target and the SDK's headers on every compile (CEF's CMake files set
`CMAKE_CXX_FLAGS` themselves) and turns off three warnings CEF's `/WX` would make errors and MSVC does not give:
`/MP` unused, missing field initializers, an undefined variable template. `xwin-include/` forwards a header CEF names
in another case than the SDK.

This is not MSVC: the release and the Windows check use MSVC under Wine (below).

### MSVC under Wine

MSVC 19.44 (Visual Studio 2022, as on the old `windows-2022` runner) runs on Linux under Wine with
[msvc-wine](https://github.com/mstorsjo/msvc-wine), to find crashes and warnings only MSVC shows. The download means
accepting Microsoft's license. Everything runs in containers, so msvc-wine's scripts never run on the host:

```sh
docker build -t hailstone-linux-build:24.04 -f tools/docker/linux.Dockerfile tools/docker   # also the Linux release image
docker build -t hailstone-msvc-build:24.04 -f tools/docker/msvc.Dockerfile tools/docker
git clone https://github.com/mstorsjo/msvc-wine ~/opt/msvc-wine-src
mkdir -p ~/opt/msvc ~/opt/msvc-home
# once, about 3.3 GB: MSVC 2022 and the Windows SDK, then msvc-wine's wrappers (bin/x64/cl ...)
docker run --rm -v ~/opt:/opt/host hailstone-linux-build:24.04 bash -c "apt-get update -qq && apt-get install -y -qq msitools \
    && cd /opt/host/msvc-wine-src && python3 vsdownload.py --major 17 --accept-license --architecture x64 \
       --with-asan no --with-atl no --dest /opt/host/msvc && chown -R $(id -u):$(id -g) /opt/host/msvc"
docker run --rm --user $(id -u):$(id -g) -e HOME=$HOME/opt/msvc-home -v $HOME/opt:$HOME/opt hailstone-msvc-build:24.04 \
    $HOME/opt/msvc-wine-src/install.sh $HOME/opt/msvc
# a build: Release, in build-msvc/ (the browser helper too: HAILSTONE_BROWSER is on by default)
docker run --rm --cpus 20 --user $(id -u):$(id -g) -e HOME=$HOME/opt/msvc-home -e MSVC_DIR=$HOME/opt/msvc \
    -v $HOME/opt:$HOME/opt -v "$PWD:$PWD" -w "$PWD" hailstone-msvc-build:24.04 tools/docker/msvc_build.sh . build-msvc
WINEDEBUG=-all EQ_GAME_DIR= HAILSTONE_TEST_TRACE=1 wine build-msvc/hailstone_tests.exe
```

- Wine refuses a home folder it does not own, hence `HOME` under `~/opt`.
- The folders the build reads must be mounted at the same path: a worktree's dependency sources through
  `FETCHCONTENT_SOURCE_DIR_*`, a symlinked plugin clone.
- Under Wine a fast fail shows as doctest's "test case CRASHED: SIGSEGV", with the case named.

This is the build the Windows release is made from (docs/updater.md "Making a release").

### Builds in containers

`tools/docker/build.sh` builds, tests and installs a platform in one command (`compose.yaml`): a shortcut for the
commands above, with the same Dockerfiles, Release, and `msvc_build.sh`.

```sh
tools/docker/build.sh linux-x64      # build-linux-x64/, then dist/linux-x64/hailstone
tools/docker/build.sh windows-x64    # build-windows-x64/, then dist/windows-x64/hailstone
tools/docker/build.sh linux-arm64    # native on an arm64 machine, under QEMU elsewhere (slow); not a release platform
tools/docker/build.sh all            # the three; --no-test after the platform skips the tests
```

#### Settings

- `CPUS` is the containers' CPU limit (default 8). `JOBS`, ninja's `-j`, follows it unless set. Keep the sum of
  every build running on the machine within its cores.
- `CMAKE_ARGS` adds configure options, split at whitespace (an option cannot hold a space), also to an existing
  build folder.

#### Images and containers

- The images are tagged `hailstone-compose-linux:24.04` (`-arm64`) and `hailstone-compose-msvc:24.04`, apart from
  the ones built by hand above.
- The Linux image has ccache, kept per platform in `.docker/ccache/<platform>` (ignored by git).
- The MSVC image adds `wine32`. Without it `wineboot` can stop before Wine has a `TEMP` folder, and `cl` fails with
  D8037.
- The containers run as the checkout's owner, so nothing ends up owned by root and Chromium (which refuses root)
  can run the browser tests.
- Only the checkout is mounted, at its own path, with git's folder read-only for the version line.
  - A plugin clone that is a symbolic link to another folder (a worktree's `plugins/nms`) is left out of the build.
  - A plugin cloned into `plugins/` is built and tested as usual. The install leaves it out, as a release does.
- Only the Linux test runs drop Docker's seccomp and AppArmor profiles. Chromium's sandbox makes user namespaces,
  which they refuse.

#### Windows and other platforms

- Windows uses an existing msvc-wine install, read-only: `MSVC_DIR`. By default that is `~/opt/msvc` when it has
  `bin/x64/cl` (the install above), else `.docker/msvc`.
  - With none there, `MSVC_ACCEPT_LICENSE=1` accepts Microsoft's license and downloads one into `MSVC_DIR`
    (`tools/docker/msvc_setup.sh`, msvc-wine pinned to a commit). Without it the script stops.
  - Wine's home is `.docker/wine-home`, one per checkout.
- The Windows tests run under Wine with Microsoft's runtime DLLs copied beside them. They leave out the cases that
  fail only under Wine:
  - the browser tests (Chromium does not run under Wine);
  - "cancelled ends a transfer stalled waiting for the answer" (above);
  - "allow_url refuses an empty..." (Wine's WinHTTP reports another address to the check);
  - "File time" and "Story window: a page's href links..." (MSVC's `clock_cast` from a file time needs Windows'
    `icu.dll`, which every Windows the client supports has).

  Run them on Windows by hand when a change touches the browser, `web/` or file times, and for a release.
- Under Wine, `create_symlink` reports success but makes something `is_symlink` does not take for a link, so the
  tests of links that escape a folder (shared files, UI packs, UI data) skip their link checks there, as they do on
  a Windows that cannot make links (no Developer Mode). Only a run on Windows with links allowed checks them.
- The script does not register QEMU for linux-arm64, since that changes the host's kernel settings. It prints the
  pinned `tonistiigi/binfmt` command to run once per boot.
- macOS is not built here: Apple's compiler and SDK run only on a Mac ("macOS arm64" below).

## clang-tidy

Not in CI. By hand: configure the tree with Clang (no build needed) and run
`run-clang-tidy -p build -quiet -header-filter='.*/src/.*' '.*/src/.*\.cpp$'` with the root `.clang-tidy`.

## macOS arm64

On hold until someone can test and maintain macOS (docs/plan.md, 0.9): a release does not wait for it, and no macOS
build comes with one. The workflow is kept, so it can be started as below.

`.github/workflows/macos.yml` runs on GitHub's `macos-15` runner (Apple Silicon, Apple Clang):

1. a Release build;
2. `hailstone_tests` (without game files or vectors);
3. `cmake --install`;
4. an ad hoc signature on every Mach-O file of the install (Apple Silicon runs no unsigned code, and rpath edits can
   leave the linker's signature stale).

It fails when the binary needs a library outside the system's and its own folder. The artifact,
`hailstone-macos-arm64`, is the installed folder as a tar (which keeps the executable bits a zip drops).

Start it from the Actions tab, with `gh workflow run macos.yml -f ref=<commit or tag>`, or by pushing a
`macos/<topic>` branch; then `gh run download <run> -n hailstone-macos-arm64` and `tar xf` it. The repository is
public, so its minutes are free.

The build:

- sets `CMAKE_OSX_DEPLOYMENT_TARGET` to 13.3 (the first macOS whose libc++ formats floating-point numbers);
- fetches MoltenVK's prebuilt release (cmake/Dependencies.cmake);
- leaves the in-game browser out (CEF needs app bundles there).

There is no Developer ID signature or notarization, by choice. Players clear the download's quarantine (README.md
"Download").
