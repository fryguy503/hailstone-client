# Worked changes in Hailstone

Use the closest example to trace a change from its entry point through the owner of the state to a test and an
observable result. All five examples are in the client repository. Build and run commands assume a checkout
and build as described in the [validation guide](validation.md).

## Change an option in the shipped UI

Modern and Gamepad both present the **carried light controls**. The client owns the value, persistence, and
application to the scene.

1. Trace the key from [Modern Options](../../ui/modern/options.lua) and
   [Gamepad Options](../../ui/gamepad/options.lua) to
   [client option handling](../../src/app/client_app_options.cpp). The carried-light controls use the same
   keys in both packs. Their host defaults and ranges belong with the option and lighting code. Confirm the
   units at the boundary, since a percentage in the UI may be a multiplier in the engine.
2. Determine whether the setting is shared, per server, per character, or an unsaved run override from the
   [data layout](../layout.md) and the existing option's load and save paths. Exercise a fresh profile and an
   older profile with the key missing. Check default, bounds, reset, and reload. For changes to a
   per-character setting, switch characters and check that values stay with their owners.
3. Follow the selected setting to its consumer. Carried-light intensity and reach affect the light constructed
   in [game light rules](../../src/game/light_list.cpp). The [lighting tests](../../tests/lighting_test.cpp)
   check normal and clamped values and the zero-intensity case.
   [Settings file tests](../../tests/settings_files_test.cpp) cover per-character values and transient
   overrides.
4. If the UI needs a new public `hs.UI` method, follow the
   [UI API author workflow](../ui-api.md#adding-a-new-client-feature). Extend the host registrar and catalog,
   preserve existing signatures, regenerate the API reference and test adapter, and check unavailable or
   denied results as well as the enabled path. A widget that uses an existing option usually needs the two
   pack changes and any option definition or runtime change, with the existing API intact.
5. Run the focused option or Lua tests, then exercise both packs in the matching build. Check mouse and
   controller interaction as applicable, reopening the window, and a restart for saved changes. The
   [UI demo](../dev-ui.md#demo) covers developer screens. Use the built client with the selected Lua pack to
   verify a shipped Options window.

For a layout-only change, [Lua character settings tests](../../tests/lua_character_settings_test.cpp)
demonstrate that window layout follows the character while other settings can be shared, and that copying
layout leaves unrelated character state intact.

## Change a RoF2 packet or gameplay rule

**Actor property sets** cross packet decoding, game-file lookup, world state and spell eligibility.
[The property-set notes](../mechanics.md#actor-property-sets) record the evidence and the stock server
behavior.

1. Identify the actual stage and opcode in the [world and zone dispatcher](../../src/net/game_connection.cpp)
   and [RoF2 packet reference](../protocol/rof2-packets.md). Trace the decoder in
   [zone packets](../../src/net/zone_packets.cpp), and trace the resulting lookup in the
   [property-set loader](../../src/game/character_property_sets.cpp) and state in
   [world state](../../src/game/world_state.h). The direct property IDs in a spawn are distinct from the set
   ID in an explicit `SetBodyType` update.
2. Pin the behavior using original-client observations and the stock server path. The original clears the list
   before copying the known set's members. An unknown set clears the list when the table is loaded. Hailstone
   uses a single-property fallback if the entire file is absent. Stock EQEmu sends a replacement spawn to
   change the body type. Preserve that path and keep its evidence distinct from the explicit update.
3. Test the consumer as well as the state write. [Cast tests](../../tests/game_connection_cast_test.cpp) build
   a small synthetic table and assert spell eligibility with direct properties, known sets, and a later
   unknown set. They also cover a missing file. [Appearance tests](../../tests/appearance_systems_test.cpp)
   cover targeting by membership, and [item metadata tests](../../tests/item_metadata_display_test.cpp) cover
   the localized bane label.
4. For a new decoder, add a byte-level layout check. The fuzz list, generated opcode table
   ([generator](../../tools/gen_opcodes.py)) and sanitizer build follow [AGENTS.md, Adding code](../../AGENTS.md#adding-code) and
   [AGENTS.md, Build and test](../../AGENTS.md#build-and-test); update [hardening coverage](../hardening.md) as appropriate. If you
   alter transport, zone entry or an outgoing request, add a loopback integration test as described below.
5. Update the packet and mechanic references with the evidence, fallback, and what would resolve any remaining
   uncertainty. Unconfirmed behavior: "No guessing"; server-specific protocol or gameplay: "Compatible with
   stock EQEmu"; both in [AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most).

## Extend the native SDK or an add-on

The **bulk spell data** call `get_spells_data_bulk` lets an add-on use the client's spell and effect data
through the SDK. The wire decoder and plugin buffer have separate contracts.

1. Check whether the documented host, `mq`, or `hs.UI` API already answers the caller's need. The feature's
   presentation and packaging live in the add-on repository. The client owns reusable model and host calls,
   permission checks, and tests for the SDK contract.
2. Follow this example from the [SDK declaration](../../sdk/hailstone_plugin.h) to
   [native host implementation](../../src/plugin/native_plugins.cpp) and
   [record documentation](../plugins.md#get_spells_data_bulk). The record uses explicit sizes and counts. The
   consumer advances by `recordSize`, preserves input order, and handles unknown IDs and a result truncated
   at a whole-record boundary. The host checks `read` access.
3. Follow the compatibility and threading rules in the [host table guide](../plugins.md#the-host-table) and
   SDK header. For a new optional entry, establish the minimum compatible host before using it and keep an
   unavailable path for older supported hosts. Record layouts and SDK-only includes: [AGENTS.md, Portability rules](../../AGENTS.md#portability-rules-they-hold-on-four-platforms-linux-and-windows-are-built-locally)
   and [AGENTS.md, Plugins, scripts and add-ons](../../AGENTS.md#plugins-scripts-and-add-ons). A worker
   can hand results to a plugin callback on the client's thread.
4. Run a caller through the real plugin boundary. [HostProbe](../../tests/plugins/HostProbe.cpp) consumes the
   SDK data and [native plugin tests](../../tests/native_plugins_test.cpp) check known and missing spell IDs,
   record layout, short buffers and a caller without `read`. Add checks for inactive or absent owners or for
   unload when the feature depends on those states.
5. For the add-on itself, use the [example plugin](../../plugins/example/MQ2Example.c) for the build and
   callback pattern and [the add-on guide](../addons.md) for manifest, capabilities and packaging. Capabilities
   and releases follow [AGENTS.md, Plugins, scripts and add-ons](../../AGENTS.md#plugins-scripts-and-add-ons). Test both install and runtime behavior with an isolated
   data folder. If server admission changes, cover stock behavior, opted-in behavior and refusal with
   [add-on requirements tests](../../tests/addon_requirements_test.cpp).

## Fix a world asset or rendering issue

**Horizon Hills** can overlap playable terrain at steep cliffs.
[Horizon mesh generation](../../src/render/horizon.cpp) keeps the generated scenery below the lowest covered
terrain under a mesh quad. The render mesh adds scenery around the zone and the playable terrain remains the
collision authority.

1. Record the zone, source asset format, camera position and heading, lighting mode and setting values. Check
   whether the source geometry is decoded incorrectly, the wrong model or placement is selected, or valid
   geometry is drawn incorrectly. Start at [format notes](../formats/) and the dump tools named in
   [Code overview](../code-overview.md) for decode questions. Start at the owning renderer, shader and
   [graphics notes](../graphics.md) for drawing questions.
2. Reproduce the smallest geometric case in a fixture. [The horizon tests](../../tests/horizon_test.cpp) build
   two adjacent squares at widely separated heights and sample the low side of the generated mesh. Require a
   valid mesh and sample before checking its height. Use an asset fixture only if the reported failure depends
   on the actual resource or format.
3. Inspect the affected zone using the build's viewer with the agreed camera and asset set. For example, with
   a configured game folder on a GPU-capable host:

   ```sh
   build/hailstone/hailstone --zone qeynos2 --lighting modern --frames 30 --validation --screenshot horizon-on.png
   build/hailstone/hailstone --zone qeynos2 --lighting modern --frames 30 --validation --no-horizon --screenshot horizon-off.png
   ```

   Substitute the reported zone and add the same `--pos` and `--look` to both commands for a fixed view.
   Capture one view where the defect used to occur and one where the feature should still be present. For
   model issues, follow `hailstone --help` for the `--model` viewer. For a shader change, verify the built
   SPIR-V is loaded and review the Vulkan validation log in addition to the image.
4. Report CPU geometry checks, asset-backed measurements, Vulkan output, and live gameplay as distinct
   evidence. The viewer can verify the visible seam. A symptom involving movement or collision also needs a
   check at the feature's gameplay entry point. What stays out of Git: [AGENTS.md, Git workflow](../../AGENTS.md#git-workflow).

## Exercise login and zone behavior without the GUI

For **world and zone entry**, the [mock EQEmu server](../../tests/mock_server/README.md) drives the real
session and connection on loopback. [Login tests](../../tests/mock_login_test.cpp) cover the world handoff,
and [game tests](../../tests/mock_game_test.cpp) exercise character selection, zone entry, and transitions.
Both run with synthetic inputs and without a Vulkan window.

1. Select the smallest observation that proves the behavior. A decoder fixture checks bytes. Use the mock for
   a connection state transition, an outgoing packet, receipt ordering, or state reset across zoning. Start
   with `Rig` in the [mock harness](../../tests/mock_server/harness.h), and use its condition-based pump to
   wait for the state or packet you need.
2. Reuse the scripted login/world/zone flow and add only the necessary stimulus. Use the existing manual clock
   and explicit fault scripting for timing or delivery behavior. When the assertion is that a packet was
   ignored, send an ordered marker after it and wait for the marker before checking the absence of a side
   effect.
3. Run the exact regression case and the adjacent login, game or session cases. For example, on a Ninja build
   from a POSIX shell:

   ```sh
   build/hailstone_tests -tc="mock server: world auth*"
   build/hailstone_tests -tc="mock login:*"
   ```

   Doctest treats commas in filters as separators. Use a distinctive prefix or substring when a case name
   contains a comma. After focused checks, run the whole `hailstone_tests` as [AGENTS.md, Build and test](../../AGENTS.md#build-and-test)
   requires ([validation guide](validation.md)).
4. Confirm protocol layouts independently where the mock builds packets with the client's own encoder. Use an
   original observation or documented byte fixture as appropriate. Report the flow as mock-server coverage
   ([AGENTS.md, Build and test](../../AGENTS.md#build-and-test)) and list any unexercised stock server differences. The [playprobe tool](../../tools/playprobe.cpp) is a
   separate command-line path for authorized live EQEmu sessions. Use its argument help to choose an
   observation, use a disposable test character, and record whether the action changes world or character
   state. Live probe output still leaves GUI, input, and render acceptance to the client.
