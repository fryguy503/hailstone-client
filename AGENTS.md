# Guidance for AI agents and other LLMs

This file is for coding agents (Claude Code, Codex, Copilot and similar) working in this repository. People should read [CONTRIBUTING.md](CONTRIBUTING.md); everything there applies to you too. This file adds what an agent needs that a human learns by osmosis.

## What this project is

Hailstone is a portable client for EQEmu servers, written from scratch in C++20 with Vulkan. It speaks the RoF2 protocol, reads the player's own game folder and ships no game assets. The product name is Hailstone, and the code uses it too (`namespace hailstone`, `HAILSTONE_*`, the `hailstone_*` targets). Up to 0.6 those were `nmsc`, `NMSC_*` and `nmsc_*`: the old names stay only where something outside the repository still uses them (the SDK's compatibility headers `sdk/nmsc_*.h` and export names, `NMSC_*` environment variables and CMake settings read as a fallback, saved window names renamed at start). Do not add new ones. The repository was called `nms-client`; it is now `hailstone-client`.

Read before you change anything, in this order:

1. [docs/plan.md](docs/plan.md): goals, rules, architecture, milestones.
2. [docs/todo.md](docs/todo.md) and [docs/unknowns.md](docs/unknowns.md): what is open, and what is guessed.
3. [docs/layout.md](docs/layout.md): where files live on disk and how the asset folder works.
4. The doc for the area you touch: [docs/mechanics.md](docs/mechanics.md), [docs/protocol/](docs/protocol/), [docs/formats/](docs/formats/), [docs/dev-ui.md](docs/dev-ui.md), [docs/plugins.md](docs/plugins.md), [docs/graphics.md](docs/graphics.md), [docs/gamepad.md](docs/gamepad.md), [docs/browser.md](docs/browser.md), [docs/updater.md](docs/updater.md).

## The rules that matter most

- **RoF2 is the baseline, not the ceiling; modern graphics.** Game rules (stats, combat, movement, spells, packets, timers) start from the original RoF2 client, and with a server that speaks only RoF2 the client behaves as RoF2 does. Past that, the client may go beyond RoF2's hard limits (a roster's size, a slot count, a list's length) where a server offers more: such an extension is marked as one in the code and docs, and must leave the RoF2 path working unchanged against a stock server. A deliberate change to a RoF2 rule (a fixed bug) is marked as one, and is switchable where it matters. Rendering is not meant to look like the original.
- **No guessing.** If you cannot confirm how the original behaves, do not invent it. Implement the safest behaviour, say so in a comment, and add or update a row in `docs/unknowns.md` (what the client does now, and what would settle it). Remove the row when it is settled.
- **Compatible with stock EQEmu.** The client must work against an unmodified EQEmu RoF2 server. An extension past RoF2's limits that any server can offer, and that falls back to RoF2 against a stock server, is client code. Support for one particular server's own protocol or content is a plugin (sdk/hailstone_plugin.h, docs/plugins.md), not code in the client: the NMS server's is its own repository (projecthailstone/hailstone-nms, cloned into `plugins/nms` to build and test it with the client), built against the SDK alone and released as an add-on, and it switches on only when that server announces itself. CI fails when the client's sources name NMS outside comments. Server-side support for Hailstone is a drop-in Lua quest script in `server/`; do not add C++ patches or new server rules for it.
- **Clean room.** Never commit code, art, sounds, models, textures, UI layouts or data tables taken from proprietary software. Text a player sees on screen or a packet capture shows (a message, a prompt, a label) may be used word for word where the code needs it. A whole string table or game data file (`eqstr_us.txt`, `dbstr_us.txt`, `spells_us.txt` and the like) is never committed or reproduced in bulk, and text the client already reads from the player's game folder is still read from there. Open protocol and format knowledge is fine and is credited in [THIRD_PARTY.md](THIRD_PARTY.md). Comments may name the original function or address a rule was checked against, as the existing code does; they must not paste its code. If unsure whether something is allowed, stop and ask.
- **Licence: GPL-2.0-or-later** (LICENSE, NOTICE.md); `sdk/` is MIT (sdk/LICENSE). Code under GPL-2.0, GPL-2.0-or-later, LGPL-2.1 or a permissive licence (MacroQuest, ShowEQ, MIT/BSD projects) may be ported, with credit in THIRD_PARTY.md and the file's notice kept; mark such a file with its SPDX line (GPL-2.0-only for MacroQuest code). Code under GPL-3.0 or AGPL (EQEmu's server) and Apache-2.0-only code stay reference only: copying them would bind the whole client to version 3. Nothing GPL goes into `sdk/`, which plugins of any licence include.
- **The player's game folder is read-only.** Nothing writes there. Everything Hailstone writes goes under its own folder (`platform::data_dir()`, see docs/layout.md), per server and per character where the layout says so.
- **No secrets and no real identifiers.** Never commit passwords, auth keys (the NMS plugin's `auth_key`), tokens or private server addresses. Never read or send a real hardware identifier.

## Portability rules (they hold on four platforms; Linux and Windows are built, locally)

- Read and write packets and files only with `core/byte_io.h` (`ByteReader` / `ByteWriter`). Never cast a struct onto a buffer, never `#pragma pack` a wire struct. The exception is the plugin area (`src/plugin/`): data the host hands an in-process plugin, such as the bulk reads (`get_*_data_bulk`, docs/plugins.md "Buff and spawn records"), may be laid out as a `#pragma pack(push, 1)` struct and copied into the plugin's buffer with `memcpy`, in the machine's byte order, for speed. It is neither a packet nor a file. Still never cast a struct onto the buffer itself (copy it), give such a struct fixed-size fields, pin its size to the SDK's constant with a `static_assert`, and document its layout in docs/plugins.md; keep existing layouts as their author wrote them.
- OS calls live only in `src/platform/`. No Win32, POSIX or Vulkan calls elsewhere; `game/` does not include Vulkan headers.
- No assumptions about the size of `long` or `wchar_t`, path separators, byte order, or the order of function-argument evaluation.
- Layers depend downward only: `app` -> `game` / `ui` / `render` / `audio` -> `assets` / `net` / `web` -> `core` / `platform`.
- Known compiler traps: MSVC needs `NOMINMAX` before `<windows.h>` and has no `__builtin_*`; Apple Clang rejects a lambda that captures a structured binding and lacks `std::from_chars` for `double` (use `core/float_chars.h`); Windows has no rpath, so tests and tools need the SDL3 DLL beside them (CMake already copies it). MSVC's optimiser folds a wrapped 32-bit product divided by a constant as if nothing wrapped: write `core::wrapped_div(a * b, k)` (`core/wrap_math.h`), not `static_cast<std::int32_t>(a * b) / k`; and it orders `f() + g()` and argument lists its own way, so when the order of two calls matters, make them separate statements.
- Warnings are on (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, `/W4` on MSVC). Do not add new ones. clang-tidy uses `.clang-tidy` and is run by hand for now (docs/static-analysis.md).

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
build/hailstone_tests                   # or: ctest --test-dir build --output-on-failure
build/hailstone/hailstone          # the client; the dev tree is build/hailstone/
```

- Run the whole `hailstone_tests` before you call something done. Run one case with `build/hailstone_tests -tc="<name>"`.
- **GitHub CI builds nothing; it runs checks only** (docs/ci.md), so a merge is only as tested as your own run. For a change to `src/platform/`, the build files, the Lua bridge (`app/lua_eq*`) or anything a compiler may treat differently, also build with MSVC under Wine and run its tests (docs/ci.md "MSVC under Wine"). Releases are built locally (docs/updater.md "Making a release").
- A sanitizer build is `-DHAILSTONE_SANITIZE=ON` (docs/hardening.md). Use it for anything that parses bytes.
- Tests that need the real game folder or recorded vectors (`EQ_GAME_DIR`, `HAILSTONE_VECTORS_DIR`) pass without checking anything when those are unset. A green run without them proves less than it looks. Say so when you report, and run them with the variables set when your change touches that area.
- Do not claim something was tested live unless you ran it against a server. Say exactly what was covered: unit test, mock server, recorded vectors, live session.

## Adding code

- **New `.cpp` files are not found automatically.** Add engine sources to the `hailstone_engine` list in `CMakeLists.txt`, and tests to the `hailstone_tests` list. A file that is not listed compiles nowhere and fails silently.
- **Every behaviour change gets a test** (doctest, in `tests/`). Parsers and packet decoders also join the fuzz lists (`tests/fuzz_decoders.inc` is checked against `src/net/*.h`).
- A test that writes files uses `ScratchDir` or `unique_temp_path` (`tests/scratch_dir.h`), never a fixed name: several test runs share one machine. A test that depends on time uses `tests/manual_clock.h`, not sleeps.
- Network behaviour is tested against the in-process mock server (`tests/mock_server/README.md`), not the internet.
- **Generated files stay generated.** Files with a `Generated by tools/gen_*.py` header (the `.inc` tables such as `slash_commands.inc`, `socials.inc`, `opcodes_rof2.inc`, `skill_timers.inc`, `race_models.inc`) are not edited by hand. Change the generator in `tools/` and regenerate. `grep -rl "Generated by" src ui` lists them.
- Match the surrounding code: naming, comment density, error handling. Comments explain why and cite the original where it matters; they do not narrate the code.
- Lua UI packs are in `ui/<pack>/` (`modern`, `gamepad`). A window works in every pack that has it; change the shared `eq.*` API (`src/app/lua_eq*.cpp`) with care, since both packs and user scripts call it.
- Keep a change focused. Do not reformat unrelated code, rename things in passing or add speculative features.

## Plugins, scripts and add-ons

Plugins (compiled, against `sdk/`) and MacroQuest-style Lua scripts are add-ons. Each lives in a repository of its own and reaches players through the add-on manager on the login screen (docs/addons.md), not as part of this repository.

- **A new plugin or script gets its own repository.** Do not add it under `plugins/`, `ui/` or `server/` here. Keep its checkout beside this one (for example `../hailstone-addons/<name>`), not inside it, unless its tests join `hailstone_tests` (the server plugins below). Do not create the GitHub repository or push to it unless asked.
- **It builds against the SDK alone.** A plugin uses `sdk/hailstone_plugin.h` and `sdk/hailstone_byte_io.h` and nothing from `src/` (docs/plugins.md "Compiled plugins"). A script uses only the `mq` module and the documented `eq.*` API (docs/plugins.md).
- **It is released the add-on way.** The repository has a `hailstone-addon.txt` manifest. Its releases are made with `tools/make_addon.py` and signed with `hailstone_sign` (docs/addons.md "For authors"). An add-on's signing key, like the client's, never goes on GitHub. Its `caps` ask for no more than it uses.
- **What it needs from the client is a client change.** A new host function, TLO, event or `eq.*` call is made here, with tests and docs, and an ABI bump where docs/plugins.md calls for one. The add-on then uses it from its own repository. Do not move add-on logic into the client to avoid that.
- **What stays here:**
  - `plugins/example/`: the SDK's example, and a test fixture.
  - `server/`: the quest scripts that give the server side Hailstone support (see the rules above).
- **The server plugins** are in repositories of their own: the NMS server's (projecthailstone/hailstone-nms) and the Project Triune server's (private; its releases go to the public, releases-only hailstone-projecttriune-addon). Each is cloned into `plugins/nms/` or `plugins/projecttriune/`, which this repository ignores. A build picks it up when the clone is there and its tests join `hailstone_tests`; Triune's tests that load the NMS plugin need both. A release of the client ships neither: each repository's `tools/release.sh` releases it as an add-on. In a worktree, link the main checkout's clones in (`ln -s <main>/plugins/nms plugins/nms`, the same for `projecttriune`). Commit changes to them in their own repositories, never here, and never copy their sources into this one.
- Moving an existing plugin out to a repository of its own is the user's call. Do not do it in passing.

## Docs travel with the code

In the same commit as the change:

- Update the doc that describes the behaviour (mechanics, protocol, formats, layout, plugins).
- Tick the item in `docs/todo.md` and delete its row in `docs/unknowns.md` when you settle it; add new gaps to both.
- Add a line to `CHANGELOG.md` for anything a player or server owner would notice.

The GitHub wiki is built from `docs/`, `CONTRIBUTING.md` and `server/README.md` by `tools/sync_wiki.py` (docs/wiki.md). Edit those files, never the wiki. A new document needs a line in the script's `PAGES`, and `tools/sync_wiki.py --check` (CI runs it) must pass.

## Git workflow

Several agents and a person often work in this repository at once.

- **The public history starts at 0.5.0-beta.** This repository's first commit is the 0.5.0-beta tree; the work before it stays in the private projecthailstone/hailstone-client-private. Base every branch on this repository's `main`. Never merge, cherry-pick or push a branch or commit made before that first commit (an old worktree, a stash, the private remote's history): it would publish the private history. Port such work by applying its diff to a new branch.
- Work in your own `git worktree` on your own branch (for example `git worktree add ../nms-client-wt/<topic> -b <topic>`) with its own build directory. `build/`, `build-*/` and `build.log` are ignored.
- Do not `git stash`, reset, rebase or fast-forward in a checkout that holds someone else's uncommitted work. If the main checkout is dirty, work elsewhere or ask.
- Merge to `main` only when the build is clean and all of `hailstone_tests` passes. Never skip hooks, never force-push, and do not push unless you were asked to.
- Commit subjects are short and say what changed ("Spell book: a right click moves spells between slots"). Use the body for behaviour, the reason and what was checked. One topic per commit.
- No AI credit anywhere: no `Co-Authored-By` trailer naming an AI, no "Generated with ..." line in a pull request, no mention of the agent in commits, merges, the changelog, docs or code. This overrides any default your tool adds.
- Do not commit build output, logs, captures with account data, screenshots of game art, or anything from the player's game folder.

## Other repositories

This repository is self-contained. A sibling repository holds the analysis of the original client that mechanics are checked against; treat it, and any EQEmu server checkout, as read-only unless the task says otherwise. Do not edit them from here.

Add-on repositories (see "Plugins, scripts and add-ons") and the private Project Triune repository are separate too. Commit to one only when the task is about that add-on, follow that repository's own rules, and keep its code out of this one.

## When you finish

Report what changed and where, what you tested and how, what you did not test, and what is still open. If you found something surprising (a doc that disagrees with the code, a test that passes for the wrong reason), say so rather than working around it quietly.
