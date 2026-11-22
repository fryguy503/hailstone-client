# Find where a change belongs

Use this guide to follow a symptom to the code that owns it and choose a test that exercises that path. The
[code overview](../code-overview.md) describes the layers and frame loop. The [compatibility guide](contracts.md)
lists the behavior to preserve when work crosses those layers.

## Start from a concrete observation

Record the client revision, server type, UI pack, and what the player did. Include the zone, character state,
first login versus zoning, and original client behavior when they affect the result. A report may span several
layers. For example, a missing group member could come from an omitted packet, a rejected layout, state reset
during zoning, or a UI lookup that cannot resolve a member outside the zone. Trace the value from its producer
to its consumers before deciding where to change it.

| Starting point | Follow the normal entry point | Evidence to collect |
| --- | --- | --- |
| A warning, prompt, chat line, command or key | Search for the visible text, string ID, or command in [command dispatch](../../src/app/client_app_commands.cpp), the [command generator](../../tools/gen_slash_commands.py), and [chat state](../../src/game/chat_windows.cpp). See the [command reference](../commands.md). | Record whether the behavior comes from typed chat, key input, a UI action, or a server message. For chat, follow message classification, routing and display independently. |
| A server opcode or missing world update | Find the opcode in [world and zone dispatch](../../src/net/game_connection.cpp). Follow the area's `game_connection_*.cpp` handler to its decoder and owned state. Start below dispatch for framing, retries or fragments. [Session](../protocol/session.md) · [Login](../protocol/login.md) · [World and zone](../protocol/rof2-packets.md) | Verify expected bytes and opcode direction with a fixture, then check the state after dispatch. Use [mock integration](../../tests/mock_server/README.md) when the connection and packet order matter. |
| A value or gameplay rule | Start with [game state](../../src/game/world_state.h) or the feature's model under [game](../../src/game/). For movement, spells, combat, stats and timers, use the [mechanics map](../mechanics.md) to find the corresponding function and call site. | Establish whether the server supplies the value or the client derives it. If the rule has a recorded-vector suite, identify the cases and inputs that cover it. |
| A window or button behaves differently across interfaces | Find the Lua presenter in [Modern](../../ui/modern/) and [Gamepad](../../ui/gamepad/). Look up its `hs.UI` method in the [API catalog](../../src/app/lua_ui_api.inc), then find the binding and authoritative state or action. [Developer UI](../dev-ui.md) · [UI API](../ui-api.md) | Identify the pack, input mode, layout state and capability context. Check the shared call first when both interfaces have the same symptom. |
| Movement, collision, or `/nav` | Follow [player movement](../../src/app/player_movement.cpp) into [movement rules](../../src/game/mechanics/movement.cpp) and [collision movement](../../src/game/mechanics/collision_movement.cpp). For paths, use the [navigation guide](../navigation.md). For controller-only input, follow [Gamepad](../gamepad.md). | Note grounded/airborne state, fixed-step input, and whether the player is riding, swimming, or zoning. Server corrections, navigation output and rendering interpolation have separate owners. |
| A model, zone, texture or sound is missing | Start with the [asset folder index](../../src/core/game_dir.h), [archive reader](../../src/assets/pfs.cpp), and relevant [format reference](../formats/). Inspect loaded data with the [dump tools](../../tools/). For sound, continue through [sound events](../../src/app/sound_events.cpp) and the [audio guide](../formats/audio.md). | Identify the effective asset layer, classic WLD or newer EQG format, and the exact archive/member involved. Isolate parsing from resource lookup and rendering. |
| A visual artifact or GPU hitch | Follow [scene draw integration](../../src/app/client_app_draw.cpp) to the relevant [renderer](../../src/render/) and [shaders](../../shaders/). Start with [Vulkan context](../../src/render/vk_context.h) for frame or swapchain problems. [Graphics](../graphics.md) | Record GPU/driver, renderer settings, viewport, camera and content. If work is queued, check when the submit or flush actually consumes it. |
| A preference or layout resets | Identify the intended scope in [Folder Layout](../layout.md). Follow [Config](../../src/core/config.h), [server profiles](../../src/core/server_profile.h), [character files](../../src/core/character_files.h) and [UI layout](../../src/ui/ui_ini.h) as appropriate. | Compare fresh settings with an older saved file. Check entry into and exit from a character, the effective profile/pack, and whether a launch override was active. |
| A script, native plugin or server extension behaves differently | Start with [Plugins and Scripts](../plugins.md). For scripts, find the `mq` binding in [Lua MQ](../../src/mq/lua_mq.cpp) or the expression in [game TLOs](../../src/mq/tlo_game.cpp). For native plugins, follow the [SDK](../../sdk/hailstone_plugin.h) into [plugin loading](../../src/plugin/native_plugins.cpp) and the relevant host implementation. | Record file origin, host support, declared and effective capabilities, server policy, consent and active state. Locate any server-specific implementation in its own repository. |
| A download, install or update fails | Follow [add-on management](../../src/app/addon_manager.cpp), [add-on install](../../src/app/addon_install.cpp), [server files](../../src/net/server_files.h) or the [client updater](../../src/app/updater.cpp), depending on who supplies the files. [Add-ons](../addons.md) · [Updater](../updater.md) | Identify the failed phase and what was installed before it. Check the prior installed files, recovery state and the result of a retry with test data. |

Search from the repository root. These searches work in a POSIX shell and PowerShell. Use Git's tracked-file
search if `rg` is unavailable.

```sh
rg -n 'GroupUpdateB' src tests docs
rg -n 'UI_METHOD\("Gauge"|API\.character\.Gauge' src/app ui
git grep -n 'GroupUpdateB' -- src tests docs
```

## Trace stock group state into both interfaces

Use an apparently missing group member to decide which test or code to change.

1. Check when the report occurred. An initial group update, a member joining during play, and the roster being
   resent after zoning take different paths. [RoF2 world and zone packets](../protocol/rof2-packets.md)
2. Search for `Op::GroupUpdateB` in [world and zone dispatch](../../src/net/game_connection.cpp). Follow the dispatch
   to the group handler, which calls the [group decoder](../../src/net/group_packets.cpp) and updates
   [group state](../../src/game/group.h). Check the [group handler](../../src/net/game_connection_group.cpp) for
   zoning behavior and other update opcodes that affect the same state.
3. If the roster is correct there, follow `hs.UI.character.Gauge` from the [catalog](../../src/app/lua_ui_api.inc)
   into the UI bridge. Compare how the [Modern group window](../../ui/modern/group.lua) and
   [Gamepad group window](../../ui/gamepad/group.lua) read the result. A layout change belongs in the presenter.
   A correction to what those results mean belongs in the shared model or binding.
4. For a layout issue on the wire, start with [group packet tests](../../tests/group_packets_test.cpp). Use a case
   that drives the connection if the state is lost during dispatch or zoning. Use a UI test when the symptom
   depends on the window's handling of valid state. Test the layer whose output first diverges, and exercise the
   normal caller when wiring or lifecycle is involved.

## Trace a presentation issue without logging into a server

Use a zone viewer when the issue concerns static terrain, sky or loading a zone. Discover the flags supported by
the executable under test with `--help`. See [main](../../src/app/main.cpp) and the
[rendering guide](../graphics.md).

1. Start with an asset set you are allowed to use. Record the source and any overrides, and reproduce from a
   comparable viewpoint on each revision.
2. When geometry is wrong at load, inspect the classic or EQG parser and the object returned to the renderer.
   When the same geometry loads correctly but draws differently, compare draw passes and resources.
3. For a horizon change, [horizon generation](../../src/render/horizon.cpp) and
   [horizon tests](../../tests/horizon_test.cpp) are useful starting points. The tests can pin terrain inputs
   and generated surfaces. The viewer can show blending, occlusion and relative height at a real zone boundary.
   State which evidence came from which run.
4. If a result depends on a character, a server's asset overlay or packets during gameplay, exercise that path
   separately. A viewer run establishes what it loaded and drew in the viewer configuration.

## Place an add-on integration

Start with the smallest public surface that the add-on needs. Check the [Lua reference](../scripting-reference.md),
[UI API reference](../ui-api-reference.md) and [plugin header](../../sdk/hailstone_plugin.h) before adding one.
For example, a nameplate presentation can use the existing generic callback and the context the host already
exposes. Follow the [nameplate canvas context](../../src/app/nameplate_canvas_context.cpp), the
[input handling](../../src/app/nameplate_input.cpp) and the [contract for a presentation](../ui-api.md#optional-world-nameplate-presentations).
Look for the corresponding [lifetime test](../../tests/lua_nameplate_lifetime_test.cpp) if the callback survives
a reload or zone change. Where server-specific decisions and missing host support go: [AGENTS.md, Plugins, scripts and add-ons](../../AGENTS.md#plugins-scripts-and-add-ons). Give
the add-on a way to discover a new API at runtime.

## Changes that cross build or registry boundaries

- New source files, tests, fuzz lists and generated files: [AGENTS.md, Adding code](../../AGENTS.md#adding-code).
- When a Lua binding changes, follow [Adding a new client feature](../ui-api.md#adding-a-new-client-feature) to update
  the catalog, reference and mock adapter and run their checks.
- For file or packet parsing, add a focused fixture and follow the [fuzz and sanitizer guidance](../hardening.md).
  Assets and packets from outside the repository may have separate requirements and availability.
- Docs, to-do, unknowns and changelog: [AGENTS.md, Docs travel with the code](../../AGENTS.md#docs-travel-with-the-code).
