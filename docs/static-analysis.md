# Static analysis

`.clang-tidy` at the root configures clang-tidy; it is run by hand at present (docs/ci.md: the CI job that ran it on Linux was dropped to save Actions minutes). Findings of the enabled checks are errors (`WarningsAsErrors: '*'`). cppcheck was run once by hand; it is not in CI.

## Running it

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release       # writes build/compile_commands.json
run-clang-tidy -p build -j"$(nproc)" -header-filter='.*/src/.*' '.*/src/.*\.cpp$'
cmake -S . -B build -DHAILSTONE_CLANG_TIDY=ON                      # or: run clang-tidy during the build
```

`HAILSTONE_CLANG_TIDY` (default OFF) sets `CXX_CLANG_TIDY` on `hailstone_engine`, `hailstone` and the tools. Tests, `third_party` and the fetched dependencies are not analysed. A full run over `src/` takes about 12 minutes on 4 cores.

## Enabled

- `bugprone-*` minus the exclusions below.
- `performance-`: for-range-copy, implicit-conversion-in-loop, move-const-arg, move-constructor-init, noexcept-move-constructor, unnecessary-copy-initialization, unnecessary-value-param.
- `clang-analyzer-`: core, cplusplus, deadcode (except DeadStores), unix.

## Disabled, and why

This code base is numeric and emulation-heavy, with float and x87 behaviour that is intentional.

| Check | Reason |
|---|---|
| `bugprone-easily-swappable-parameters` | noise; packet and record builders take many ints |
| `bugprone-narrowing-conversions`, `bugprone-implicit-widening-of-multiplication-result`, `bugprone-misplaced-widening-cast` | intentional float/int conversions, 32-bit wraparound that is part of the specified behaviour (59 hits of the last one) |
| `bugprone-integer-division` | the pixel and icon-grid arithmetic is integer on purpose (`lua_ui_*`, `particle_renderer`) |
| `bugprone-signed-char-misuse` | the sign extension of a byte is intentional (`text_overlay.cpp`) or a wire field (`game_connection_group.cpp`) |
| `bugprone-assignment-in-if-condition` | idiom `if ((x = f()))` |
| `bugprone-empty-catch` | the two empty catches (`eqg_zone.cpp`, `game_connection_group.cpp`) deliberately ignore a bad optional record |
| `bugprone-casting-through-void` | Lua extra-space pointer in `lua_host.cpp` |
| `bugprone-unchecked-optional-access`, `-exception-escape`, `-branch-clone`, `-reserved-identifier`, `-macro-parentheses`, `-multi-level-implicit-pointer-conversion`, `-switch-missing-default-case`, `-chained-comparison`, `-inc-dec-in-conditions` | noise or style |
| `clang-analyzer-deadcode.DeadStores` | flags the ports (`mechanics/melee.cpp`: `due`; `zoning.cpp`: `request`), whose dead stores are part of the specified evaluation order and must stay |
| `clang-analyzer-optin.*`, `clang-analyzer-security.*` | slow or noisy (padding, `strcpy` family); not wanted here |
| `performance-inefficient-vector-operation` | only suggests `reserve` (9 hits) |

## Fixes (behaviour unchanged)

- `net/game_connection_death.cpp`: `if (!dead) return;` before the corpse update. `report_death` handles known spawns only, so this never fires in practice; it replaces an unchecked dereference with a return.
- `net/game_connection_doors.cpp`: the drop confirmation text no longer dereferences a possibly empty cursor.
- `net/game_connection_commerce.cpp`, `ui/commerce_windows.cpp`: the `int16_t` loop counter is compared with an `int16_t` bound (`slots()` is 4 or 8), so no narrower-than-bound loop variable.
- `net/game_connection.cpp`: no `std::move` into a const reference (it never moved); `ConnectionOptions` moved into the member.
- `net/game_connection_social.cpp`, `app/client_app.cpp`: arguments moved or taken by const reference instead of copied.

## Suppressions (NOLINT)

| Where | Check | Why |
|---|---|---|
| `assets/character.cpp` (actor listing) | `clang-analyzer-core.NullDereference` | false positive on a structured binding; `actors_` holds only non-null locations |
| `assets/eqg_zone.cpp` (`blend_edge`) | `clang-analyzer-core.NullDereference` | false positive on the map lookup key (an undefined-value path the analyzer cannot rule out through `S`) |
| `net/game_connection_death.cpp` (`in.distance`) | `clang-analyzer-core.NonNullParamChecker` | false positive; `dead` is non-null inside the `if (dead)` block |
| `game/combat.cpp` (`death_message`) | `clang-analyzer-core.NullDereference` | `by` is non-null after the earlier `!by` return, so `by == dead` implies `dead` is non-null |
| `game/spell_record.cpp` (default record) | `bugprone-string-constructor` | the length includes the NUL on purpose; fixed-size record text |
| `game/merchant.cpp` (`+ 0.5` truncation) | `bugprone-incorrect-roundings` | the specified price rounding; the result is checked against recorded vectors |

Nothing in `src/game/mechanics` was edited and no float arithmetic or evaluation order was touched.

## cppcheck (one-off, not in CI)

`cppcheck --project=build/compile_commands.json --file-filter='*/src/*' --enable=warning,performance,portability --inconclusive` found no bug that needed a change. Triage:

- `accessMoved` (`session.cpp`, `game_connection.cpp`, `game_connection_social.cpp`, `actions_ui.cpp`): `x = std::move(m); m.clear();` / `reset()`; the access is the deliberate reset.
- `nullPointerRedundantCheck` (`game_connection_group.cpp`): false positive; `target` is null only when `name` is non-empty, and then it is not dereferenced.
- `incorrectLogicOperator` in `game/swept_sphere.cpp` (`normalize_or_zero`): part of the swept-sphere maths; left alone.
- `uninitMemberVarNoCtor` on plain aggregates (GPU vertex and push-constant structs, small records): they are always brace- or value-initialised.
- `passedByValue` for Lua/chat `args`, `uselessCallsSubstr`, `shiftTooManyBitsSigned`, `constStatement` in `lua_ui_windows.h` macros: style or performance only; not changed.
