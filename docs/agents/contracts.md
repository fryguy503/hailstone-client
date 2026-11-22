# Compatibility and ownership contracts

Use this page to identify what callers, old data and stock servers rely on. Check the linked implementation and
specification at the revision under review. If behavior and documentation differ, report the discrepancy and
establish the intended contract before changing callers.

## Stock protocol and gameplay

| Change | What to preserve | Where to check |
| --- | --- | --- |
| UDP session or login | Keep framing, application opcodes and login data distinct. The session header fields are big-endian, while RoF2 application opcodes and the documented world and zone payload fields are little-endian. Follow the encoding for each field. | [Session codec](../../src/net/session_codec.cpp) · [Session protocol](../protocol/session.md) · [Login protocol](../protocol/login.md) |
| World or zone handling | Confirm opcode direction, layout, size and connection phase. Check initial login, zone transitions and repeated updates where relevant. Keep packet decoding separate from state mutation. Preserve the state expected on the stock path. | [RoF2 packets](../protocol/rof2-packets.md) · [Connection](../../src/net/game_connection.cpp) · [Mock integration](../../tests/mock_server/README.md) |
| Client-side mechanics | The baseline and deviation rules: "RoF2 is the baseline" and "No guessing" in [AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most). Distinguish server-authoritative state, locally predicted state, and presentation. | [Mechanics](../mechanics.md) · [Unknowns](../unknowns.md) |
| A generally useful extension beyond a RoF2 limit | The rule: "RoF2 is the baseline" and "Compatible with stock EQEmu" in [AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most). Exercise the supported extension and the stock fallback. | [Plan](../plan.md) · [Server extension defaults](../../src/game/server_ext.h) |
| A single server's private behavior | The rule: "Compatible with stock EQEmu" in [AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most). The core defines a reusable hook with the stock default. The server's add-on supplies the override and activates on its matching server. | [Server extension chain](../../src/game/server_ext_chain.h) · [Plugin activity and order](../plugins.md#activity-order-and-compatibility) · [Server script](../../server/README.md) |

Packet and file I/O follows [AGENTS.md, Portability rules](../../AGENTS.md#portability-rules-they-hold-on-four-platforms-linux-and-windows-are-built-locally); tests and fuzz lists follow [AGENTS.md, Adding code](../../AGENTS.md#adding-code). Validate the
available bytes, field limits and allocations before consuming input. Add decoder and connection-level cases
according to what changed. Round trips and mock
traffic built by the client's own encoder can share the same mistake, so pin the relevant bytes independently.
Use a synthetic fixture derived from the documented layout, or cite a separately verified observation. Missing
evidence: "No guessing" in [AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most). [Byte I/O](../../src/core/byte_io.h) ·
[Parser checks](../hardening.md)

## Lua interface and script contracts

- **One owner for game rules and actions.** Put shared state, permission checks and actions in the client. Let
  each pack draw its own presentation. Check the same calling contract through Modern, Gamepad and any affected
  add-on surface ([AGENTS.md, Adding code](../../AGENTS.md#adding-code)). Publish reusable UI methods through the shared catalog. [UI API](../ui-api.md) ·
  [Catalog](../../src/app/lua_ui_api.inc)
- **Compatible within a major.** Preserve existing arguments, return shapes, nil versus false, defaults and
  side effects. Check for new members with `hs.UI.Supports(module, member)`. Check the API version when an
  existing member gains optional behavior. Discovery describes the host. Callers must still respect current
  data, action availability and permissions. UI API version, client release version and plugin ABI are
  independent. [Compatibility policy](../ui-api.md#compatibility-policy)
- **Aliases share the guarded closure.** Publish catalog aliases after capability and invalidation wrappers.
  Re-registering the raw binding would bypass those guarantees or split state. New pack code uses `hs.UI`.
  Existing integrations may call flat `hs.*` or the legacy `eq` alias. Regenerate the API reference and mock
  adapter from the catalog, then check registration and stale-list behavior. [Adding a client feature](../ui-api.md#adding-a-new-client-feature) ·
  [Contract tests](../../tests/lua_ui_contract_test.cpp)
- **Read views can expire.** Where an API accepts an `into` list, assume the next fill may reuse its records.
  Copy only values needed later. Keep frame tokens, owner handles and context revisions within the scope their
  API documents. For a target action, resolve fresh context and validate identity, including incarnation where
  the API distinguishes reuse. A stored handle or a selected row conveys no authority. [Chat records](../ui-api.md#chat-tabs-and-windows) ·
  [Named window extensions](../ui-api.md#named-window-extensions)
- **Authority and lifetime follow the caller.** Keep owner and capability checks at registration and execution.
  Retire callbacks, drawing claims and click regions when the owner reloads, stops, loses permission or
  leaves the applicable context. Test both the valid path and stale or revoked paths. A full UI pack has a
  different trust model from a script or plugin's window. [Interface loading](../ui-api.md#interface-discovery-and-loading) ·
  [Lua script API](../scripting-reference.md) · [Lifetime tests](../../tests/lua_nameplate_lifetime_test.cpp)

## Native plugin ABI and add-on installation

The [SDK headers](../../sdk/) and [compiled plugin documentation](../plugins.md#compiled-plugins) define the
contract across compilers and releases. The [example plugin](../../plugins/example/MQ2Example.c) shows the
basic lifecycle. How an external plugin is built and released: [AGENTS.md, Plugins, scripts and add-ons](../../AGENTS.md#plugins-scripts-and-add-ons).

| Boundary | Preserve |
| --- | --- |
| Host table and hook table | Keep existing entries and their offsets. Append compatible entries at the end and honor the caller's table size. A plugin can use `HAILSTONE_HOST_HAS(host, field)` when a member was added within the same ABI. Check the ABI policy before proposing a breaking change. |
| Structures and bulk records | Respect each record's advertised size and version. Step through a bulk list by its advertised stride and read only the fields present. Packed in-process records and byte serialization: [AGENTS.md, Portability rules](../../AGENTS.md#portability-rules-they-hold-on-four-platforms-linux-and-windows-are-built-locally). |
| Strings and callbacks | Copy borrowed data before its validity ends. Keep C++ exceptions and types inside the plugin. Use the host table in a host callback on the client thread. Background work hands results back to that thread for host calls. |
| Add-on identity and permissions | Manifest capabilities: [AGENTS.md, Plugins, scripts and add-ons](../../AGENTS.md#plugins-scripts-and-add-ons). Check installed identity, effective capability, current server policy and consent where each applies. Eligibility to load, ability to call a host function and authority to change server state are distinct checks. |
| Package lifecycle | Preserve signed-manifest verification, the journal and recovery behavior, and files owned by the player. A partial download or install must be recoverable by the existing retry path. Exercise the failed and successful paths with fixture packages. |

Native code has the player's OS privileges once loaded. Host capabilities restrict calls through the host
API. Keep consent tied to the appropriate source and exact binary where the policy requires approval. An
opted-in server's installed add-on requirements and the per-call capability ceiling are separate. Check the
fresh zone-in policy and the behavior when policy changes while the player is already in the world.
[Consent](../plugins.md#the-servers-compiled-plugins-ask-first) · [Opt-in policies](../plugins.md#opt-in-add-on-policies) ·
[Manifest and install](../addons.md) · [Policy tests](../../tests/plugin_policy_test.cpp) ·
[Install tests](../../tests/addon_install_test.cpp)

## Assets, settings and saved state

- **Resolve game assets through `GameDir`.** Preserve its case-insensitive relative lookups across platforms.
  The server overlay wins for the whole file, including whole archives. The player's base folder and the optional
  classic folder supply the rest according to their configured preference. Use the layered listing as well as
  layered lookup. Recheck what cached objects need to reload when the source or overlay changes.
  [Asset lookup contract](../../src/core/game_dir.h) · [Folder layout](../layout.md#the-asset-folder)
- **Separate installed files from data written by the client.** Read shipped UI packs and shaders from the
  install. Use the existing Hailstone path helpers for writable data. The game folder is read-only
  ([AGENTS.md, The rules that matter most](../../AGENTS.md#the-rules-that-matter-most)).
  Keep the updater's replacement scope and player-owned files in mind when adding an installed directory or
  changing a file format. [Paths](../../src/platform/paths.h) · [Updater](../updater.md)
- **Pick a setting's scope before adding it.** Check whether it belongs to the app, server profile, character,
  pack's shared settings, or per-character UI layout. Identify when it becomes effective and what a legacy
  missing key means. Test existing files, fresh defaults and any migration. A launch-only override must stay
  out of persisted settings. [Character settings](../layout.md#a-characters-settings) ·
  [Config](../../src/core/config.h) · [Settings tests](../../tests/settings_files_test.cpp)
- **Use the current save path.** Reuse durable writes and the existing merge logic for shared configuration.
  Preserve other keys, per-character data and retry behavior. Write on the relevant state change or save event.
  Preserve old-file/new-file consistency if a write is interrupted.
  [Safe saves](../layout.md#safe-saves) · [Atomic files](../../src/core/atomic_file.h) ·
  [Durable writes](../../src/platform/durable_file.h)
- **Treat authentication data as transient or secret according to its contract.** A launcher login ticket is
  scoped to the explicit endpoint for that run and stays out of settings, the keyring and diagnostic evidence.
  Use the established redaction in reports and synthetic identifiers in fixtures. [Login protocol](../protocol/login.md) ·
  [Credentials interface](../../src/platform/credentials.h)

## Threads, time and rendering

| Boundary | What to check in review |
| --- | --- |
| Session to game thread | The session service thread delivers complete application packets to the connection update. Keep gameplay mutations on the connection's owning thread. Preserve the simulation clock and the existing distinction between fixed-step rules and interpolated rendering. [Session](../../src/net/session.h) · [Step clock](../../src/core/step_clock.h) |
| Game thread to background work | Give jobs stable input and apply results only while their owner and context are current. `LatestJobs` is the existing pattern for keyed work whose older results are superseded. Check cancellation and shutdown before modifying what a job captures. [Latest jobs](../../src/core/latest_jobs.h) · [Worker threads](../graphics.md#worker-threads) |
| CPU to Vulkan | Hold resources through the frames and uploads that still use them. Use the existing retirement path for GPU objects and preserve flush ordering for recorded draws. Check fallback paths when optional device features or parallel recording are off. [Vulkan retirement](../../src/render/retire_queue.h) · [Graphics](../graphics.md) |
| Rendering to world rules | Keep presentation decisions in the renderer or presenter and preserve collision, targeting and selection in their owners. Maintain the separation between current frame geometry, game-state identity and action permission. [Nameplate canvas contract](../ui-api.md#optional-world-nameplate-presentations) · [Nameplate input tests](../../tests/nameplate_input_test.cpp) |
| Platform to portable code | OS calls, Vulkan placement, compiler traps and the packed-record exception: [AGENTS.md, Portability rules](../../AGENTS.md#portability-rules-they-hold-on-four-platforms-linux-and-windows-are-built-locally). |

## Review the old and new paths

For each touched contract, name the existing caller, input or profile that must continue to work. Include a
stock-server case when connection behavior changes, older hosts or callers when an interface changes, and existing files
when persistent state changes. Choose evidence that crosses the same boundary as the change. Report the asset
set, platforms, server and kinds of tests actually exercised, and any coverage that depends on a maintainer's
fixtures, hardware or live session.
