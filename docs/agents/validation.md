# Build, test and verify Hailstone

Run the smallest relevant check while developing. Before you call a change
done, run the whole `hailstone_tests` and report what it covered, as
[AGENTS.md, Build and test](../../AGENTS.md#build-and-test) requires. Record which executable,
fixtures and environment produced each result.

## Configure a development build

Run from the repository root in your worktree. Use a build directory belonging
to that worktree and choose separate directories for different compilers,
targets and sanitizer settings. CMake needs Ninja for the commands below,
CMake 3.25 or later, a supported C++20 compiler, and `glslc` or
`glslangValidator`. The [build requirements](../../README.md#build-requirements)
and [dependency declarations](../../cmake/Dependencies.cmake) list the platform
packages and pinned sources.

Run this sequence on Linux with GCC or Clang in your environment.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAILSTONE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build -j 8 --output-on-failure --no-tests=error
```

On Windows x64, use PowerShell with the Visual Studio 2022 x64 developer
environment loaded. Confirm that `cl.exe` is available and that CMake selected
MSVC for this build.

```powershell
cmake -S . -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAILSTONE_BUILD_TESTS=ON
cmake --build build-msvc
ctest --test-dir build-msvc -j 8 --output-on-failure --no-tests=error
```

Stop at the first failed step and inspect its output. The dev client is
`build/hailstone/hailstone` or `build-msvc\hailstone\hailstone.exe` in these
examples. Keep the executable beside its shaders, UI packs and runtime
libraries. On Windows, CMake copies the required SDL3 DLL beside the tests and
client. A launch failure after copying an executable by itself is a reason to
inspect the runtime tree.

The portable client defaults to saving alongside the executable. For smoke
tests, set `HAILSTONE_DATA_DIR` to a writable scratch profile that is separate
from your usual profile and game installation. An install or launch against a
maintainer's managed client is a separate operation. Follow that workspace's
instructions when the task calls for it. Use the [release guide](../updater.md)
for packaging and publication work.

### Adapt to the available environment

| Available environment | How to proceed |
|---|---|
| Dependencies are already cached | Start with a build cache configured for this worktree. For an offline configure, inspect the dependency names and pinned revisions in [Dependencies.cmake](../../cmake/Dependencies.cmake). CMake accepts `FETCHCONTENT_SOURCE_DIR_<UPPERCASE_NAME>` for matching local source trees. Record any overrides. |
| Linux SDL configure fails on Wayland | Install the development packages listed by CMake or use the repository's [Linux container recipe](../../tools/docker/linux.Dockerfile). An intentional X11-only configure can use `-DHAILSTONE_ALLOW_NO_WAYLAND=ON`. Record the platform coverage. |
| CEF is unavailable | A separate configure with `-DHAILSTONE_BROWSER=OFF` can run core checks. Record that the real browser helper and its test were excluded. Keep CEF enabled when verifying embedded browser behavior. |
| Display, GPU or audio device is unavailable | Build `hailstone_tests` and run the device-independent suite. Record the unverified device behavior. |
| Linux host, Windows compiler check needed | Follow [MSVC under Wine](../ci.md#msvc-under-wine) or use `tools/docker/build.sh windows-x64` after its toolchain setup. Report Wine and its excluded cases. The [clang-cl cross-build](../ci.md#windows-builds-from-linux) uses a different compiler and gives separate evidence. |
| macOS or Linux arm64 check needed | See [macOS arm64](../ci.md#macos-arm64) and [Builds in containers](../ci.md#builds-in-containers). Use the current platform support and release notes in [CI and local builds](../ci.md). |
| Native build is blocked | Run the applicable source checks, document the configure or build error, and state which compiler/runtime leg still needs a run. If loopback is blocked, run independent tests and record mock-session coverage as blocked. |

## Select and interpret tests

List the CTest entries with `ctest --test-dir build -N`. CTest normally runs
eight shards of the same Doctest executable. A direct run uses the executable's
normal case order when `HAILSTONE_TEST_SHARD` is unset. Use an exact case name
from the test source or list cases first.

```sh
cmake --build build --target hailstone_tests
./build/hailstone_tests --list-test-cases
./build/hailstone_tests -tc="exact case name"
```

Use `./build-msvc/hailstone_tests.exe` for the same commands in PowerShell.
Check that the selected case actually ran. For a crash, set
`HAILSTONE_TEST_TRACE=1` to print each case as it starts.
`HAILSTONE_TEST_LOG=1` enables console logs. If a failure appears only in a
shard or in a full direct run, record the order, shard and last traced case
before narrowing it down. Compare with the same invocation on the base commit
when determining whether an unexpected failure predates the change.

How to report a run without these inputs is in [AGENTS.md, Build and test](../../AGENTS.md#build-and-test).
The table adds what each one covers.

| Input | What it covers | How to report its absence |
|---|---|---|
| No external fixtures | Synthetic tests and the in-process [mock EQEmu server](../../tests/mock_server/README.md). Mock network cases use loopback and ephemeral ports. | Record any loopback failures or selected cases omitted by a filter. |
| `EQ_GAME_DIR` | Cases that open a local RoF2 asset folder through [game_files.h](../../tests/game_files.h). | Some cases skip and others return early. Report asset coverage as absent if the relevant files were unavailable. |
| `EQ_LIVE_DIR` | A current game install for selected compatibility cases in [character_test.cpp](../../tests/character_test.cpp) and [spells_test.cpp](../../tests/spells_test.cpp). Some cases use both installs. | Report the current-install cases separately. A fixture run checks local asset files. |
| `HAILSTONE_VECTORS_DIR` | The relevant recorded `*.jsonl` comparisons through [vector_files.h](../../tests/vector_files.h). | Check the expected files and record counts. A case can return successfully with no vectors if its file is missing. |
| Optional server plugin checkout | Some integration cases are compiled into `hailstone_tests` only when the corresponding source checkout is present. | Record which optional plugins were built. The [container setup](../ci.md#images-and-containers) can omit checkouts linked from outside its mount. |

Use assets only from an authorized local install; the read-only rule is in
[AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most). The sanitizer build for parsing
changes is required by [AGENTS.md, Build and test](../../AGENTS.md#build-and-test); see the
[ASan and UBSan build](../hardening.md#sanitizer-build) (GCC or Clang). Use the repo's synthetic fixtures to exercise
the affected format. For packets, prefer a byte-level test for layout and a
mock-session test for state transitions. The mock reuses some client encoders,
so matching results on both sides can miss an incorrect layout. File writes
and timers in tests: [AGENTS.md, Adding code](../../AGENTS.md#adding-code)
([ScratchDir](../../tests/scratch_dir.h), [ManualClock](../../tests/manual_clock.h)).

## Reproduce the repository checks

The regular [GitHub workflow](../../.github/workflows/ci.yml) runs Python
checks. The separate [macOS workflow](../../.github/workflows/macos.yml) can
build and test code when triggered. Review the workflow at the PR commit to
see which checks should run. Use `python3` on Linux or a local Python 3
launcher such as `py -3` on Windows.

| Change | Local checks |
|---|---|
| Documentation or wiki tooling | `git diff --check` and `python3 tools/sync_wiki.py --check`. New docs: [AGENTS.md, Docs travel with the code](../../AGENTS.md#docs-travel-with-the-code). |
| Client, SDK or UI source boundaries | `python3 tools/check_core_names.py`. |
| Shared Lua UI API or bundled packs | `python3 tools/migrate_ui_api.py --check`, `python3 tools/migrate_ui_api.py --mock-adapter --check` and `python3 tools/gen_ui_api_docs.py --check`. Run the tooling unit tests from the workflow. |
| Add-on or updater packaging | Run the Python packaging tests from the workflow using local inputs. Follow the appropriate install and persistence checks before claiming an update or add-on installed. |

`python3 -m unittest discover -s tests -p 'test_make_update.py' -v` runs the
update packer's tests. Identify the environment that produced each result.

## Check the running client

Choose the runtime check that reaches the affected path. Confirm the exact
flags with the built client's `--help` or [main.cpp](../../src/app/main.cpp).
Vulkan tests need a supported driver and a display. `--validation` enables
the Vulkan validation layer when available. Inspect the log as well as the
rendered output.

This developer UI demo saves a screenshot and exits after 20 frames.

```sh
scratch_profile=$(mktemp -d) &&
  HAILSTONE_DATA_DIR="$scratch_profile" ./build/hailstone/hailstone --ui-demo chars --frames 20 --screenshot chars.png --validation
```

Use a disposable PowerShell session on Windows.

```powershell
$env:HAILSTONE_DATA_DIR = Join-Path ([System.IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
.\build-msvc\hailstone\hailstone.exe --ui-demo chars --frames 20 --screenshot chars.png --validation
```

Inspect `screenshots/chars.png` and `logs/viewer.log` under the scratch data
folder. Normal client runs log to `logs/hailstone.log`.

| Task | Runtime check |
|---|---|
| Pre-game UI or shared input | For developer UI, use a [demo screen](../dev-ui.md#demo). For shipped Lua windows, use the normal client and check Modern and Gamepad wherever both expose the affected path. For an interaction bug, carry out the actual input sequence in the relevant mode. |
| Rendering, models or zone assets | Run the viewer with `--zone qeynos2 --game-dir "path/to/RoF2" --frames 30 --screenshot zone.png --validation`, using your local asset folder. Choose the zone, asset version, lighting mode and camera position that exercise the change. For nameplate depth behavior see [probe_test.py](../../tools/probe_test.py) and [Name plates depth check](../graphics.md#name-plates-offline-check). |
| Asset selection or parsing | Build and run a [diagnostic tool](../../CMakeLists.txt) in the worktree. [pfsdump](../../tools/pfsdump.cpp) lists archive members, [wlddump](../../tools/wlddump.cpp) inspects classic zones, [eqgdump](../../tools/eqgdump.cpp) inspects EQG assets, and [chrdump](../../tools/chrdump.cpp) examines character models. Keep dumps local. |
| Persisted settings and migration | Exercise the old and new states in a scratch profile. Restart the same build and verify the changed value and unaffected settings. Where appropriate, switch character or server to check scoping. See [Folder layout](../layout.md). |
| Live mechanics or server interoperability | Reproduce with an approved test server and account. Record the server type, clean baseline or extension configuration, expected behavior, observed result and steps. Check the stock EQEmu path when a server extension changes shared behavior. Perform gameplay actions only within the authorized test scenario. |
| Audio, gamepad or platform-specific integration | Check the actual device or platform affected. Record the device, configuration and the relevant input or output that you observed. |

For a bug report, use the logs under the scratch data directory and capture
only the excerpts needed to describe the issue. Review local screenshots,
crash reports, logs and traces for private data before sharing. What stays
out of commits: "No secrets and no real identifiers" in
[AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most), and [AGENTS.md, Git workflow](../../AGENTS.md#git-workflow).
Keep it out of review artifacts too.

## Record evidence for review

For each applicable check, record the base and tested commit, OS, compiler and
configuration, command or manual steps, and the outcome. What to say about
coverage is in [AGENTS.md, Build and test](../../AGENTS.md#build-and-test) and
[AGENTS.md, When you finish](../../AGENTS.md#when-you-finish); a rendered demo and fixture-backed
asset tests are kinds of coverage too. Specify omitted build options and
blocked checks. For performance or visual comparisons,
capture the baseline and candidate with the same scene, settings and hardware.

A handoff can say “eight fixture-free CTest shards passed on Linux with CEF
disabled, and the packet layout case passed under MSVC. The recorded vector
and live-server checks need a run with the appropriate fixtures and server.”
Use the actual results for your change.
