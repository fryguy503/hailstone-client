# Plugin interface v2: what the NMS module touches (phase 0)

The NMS module (`src/plugins/nms`, about 3,800 lines) is compiled into the client and answers the core through
`game::ServerExtension` (`src/game/server_ext.h`). The goal is to move it to a shared library built only against a
public plugin SDK, so that no server's features need changes to the base client. This file lists every point where
the NMS module and the core meet, and what each one becomes. It is the spec for plugin interface v2 (phase 2) and for
the move (phase 6).

Groups:

- **Core**: a general Hailstone feature (multiclass display, several pets). It stays in the client, and plugins feed
  it data through host functions.
- **Flag**: a yes/no change to a stock rule. A plugin sets it with `host->set_flag(...)` while it is active.
- **Callback**: a question the core asks while it runs. It goes in the plugin's hook table; null means the stock
  answer.
- **Have**: the interface before v2 (`sdk/hailstone_plugin.h`, then `src/plugin/hailstone_plugin.h`, ABI 1) already covers it.
- **Plugin**: code that is only NMS. It leaves the core.

## The goal: a server never needs to fork the client

A server owner or developer must be able to build whatever their server needs as plugins and scripts, without
forking Hailstone or changing the client. NMS is the test case: if NMS cannot be done that way, the interface is
missing something. So the rules for v2 are:

- **General, not NMS-shaped.** Every hook and host function in this file is written for any server. A need that
  only NMS has today still becomes a general extension point (custom opcodes, flags, class masks, options,
  windows), never a special case.
- **Native and Lua alike.** What a DLL plugin can do, a Lua script can do too, through the same hook table (phase 5).
  Only raw speed and native libraries need a DLL.
- **Several plugins at once.** Hooks chain in load order: the first plugin with an answer wins, and packet hooks
  see the packet one after another. Two servers' features, or a server's plugin and a player's script, must not get
  in each other's way.
- **A stable interface.** C only, versioned, with size-prefixed tables and structs that only grow at the end. A
  plugin built for an older interface keeps loading. Removing something follows a stated deprecation policy.
- **Everything readable, the important parts replaceable.** Read access to the game state (spawns, the player,
  spells, items, buffs, the group, options), and replacement at the points where a server's client differs: packets,
  commands, chat, labels, item display, rules, windows.
- **The UI belongs to the plugin.** A plugin ships windows for each UI pack and can replace a stock window
  (`hs.ReplaceWindow`), add EQ menu entries, options, key bindings and hot button actions, and read its own
  settings.
- **What stays fixed:** capabilities (for the player's own plugins and scripts on a server with a `hailstone.cfg`),
  the player's game folder is never written, no real hardware identifiers, and portability (the SDK builds with
  MSVC, GCC, Clang and Apple Clang).

Gaps beyond NMS's needs, to cover in phases 2-5:
- key bindings and hot button actions;
- EQ menu entries;
- nameplate and target text;
- sounds;
- custom item display lines for any item;
- the plugin's own settings files;
- dependencies between plugins (one plugin's API for another);
- an SDK package with headers, examples and a CMake template, built outside this repository.

## Progress

- Phase 0 (this file): done.
- Phase 1, multiclass and several pets in the core: done. The core keeps the class masks
  (`GameConnection::set_character_classes`, `set_player_classes`, `server_class_mask`), shows them
  (`game::class_mask_name`: the window title, the guild column, character select), and keeps the pet list
  (`set_pet_list`). The `class_mask`, `character_class_mask`, `class_desc` and `window_title` hooks are gone;
  `guild_member_class` became `guild_member_classes` (a mask). The NMS module feeds the masks from
  `OP_SendCharacterSets` and `OP_ServerAuthStats` key 1. The decoders of those two packets and of `OP_PetList`
  are still in the core until the move (phase 6).
- Phase 2a, the extension chain: done. `game::ExtensionChain` (`src/game/server_ext_chain.h`) asks several
  extensions as one, each while it says it is active, and combines their answers (the header lists how). The
  connection asks a chain of `ConnectionOptions::plugins` and then the built-in module (while `nms_mode()`).
  `wide_item_links`, `skill_rules`, `option` and `set_option` now get the connection, and `SkillRules` functions
  get a context, so the chain can hand out one rules pointer that stays valid.
- Phase 2b, the C interface: done. `sdk/hailstone_plugin.h` (moved from `src/plugin/`) is ABI 2: the `HailstoneHooks` table
  a plugin exports (`hailstone_plugin_hooks`), the `HAILSTONE_FLAG_*` switches, and the host's `set_active`, `set_flag`,
  `set_player_classes`, `set_character_classes`, `set_pet_list` and `set_vitals`, all under the new `extend`
  capability. Each such plugin is one member of the chain `Automation` hands every connection
  (`ConnectionOptions::plugins`). ABI 1 plugins still load. Not yet in the table: `item_right_click` and
  `merchant_level` (they need the item structs of 2c), and `option` / `set_option` / `window_toggle` (host
  functions in phase 4). The connection's events (packets, zoning, the pulse, chat) are the existing callbacks.
- Phase 2c, reading the game: done. Host readers with size-prefixed structs (`get_player`, `get_spawn`,
  `next_spawn`, `get_spell`, `get_item`, `get_buff`, `get_xtarget`, `eqstr`; `read` capability), the
  `item_right_click` and `list_level` hooks, and `right_click_item` (the client's own click; `actions`). Still to
  add when the NMS port needs them (phase 6): closing the spell book, clearing the spell recovery, the pet window's
  pet, a window toggle, and the plugin's per-character folder.
- Phase 3, loading before the zone-in: done. From the world login to the zone-in (character select too) the
  connection holds the last login's rules, provisional (`net::cached_server_rules`, `plugin::Rules::provisional`):
  only files with the new `login` capability (bit 1024) run, so a plugin can rewrite the login checksums and set the
  characters' classes. The zone-in's rules replace them; at character select they come back; disconnected, none.
  Lua socials only from the zone-in, and they never get `extend` or `login`.
- Phase 4a/4b, plugin windows: done. `LuaUi::set_folders` loads a plugin's `ui/<pack>/` beside the pack's own
  (dofile from the plugin's folder; gone with the plugin); a server may ship Lua in `plugins/<name>/ui/`.
  `plugin::UiBridge` holds what plugins give the UI: functions (`hs.Plugin(name).fn(...)`, values as `UiValue`
  trees), options (`hs.PluginOptions`) and menu entries (`hs.PluginMenu`); the C side is `add_ui_function`,
  `arg` / `arg_field`, `out_*`, `add_option` and `add_menu` (`ui` capability).
- Phase 4c, the packs: done. Modern's Options window has a Plugins tab while a plugin has options; Gamepad's has
  a Plugins page. The EQ menu has a "Plugins" submenu before "Report a Bug" (`CascadeMenu::set_plugin_entries`),
  and Gamepad's More page lists the entries. The NMS rows still hard-coded in the packs (system.lua's two entries,
  options' `nms_xtarget_hide_pets`, hudextras' cooldowns) go with the NMS move (phase 6).
- Phase 5, Lua plugins: done. The scripts' `hailstone` module (`src/mq/lua_hailstone.cpp`) gives a script the
  compiled plugins' powers under the same capabilities: hooks (a `LuaExtension` in the chain), switches, class masks,
  pets, vitals, packet hooks and sending, UI functions, options, menu entries, its folder's `ui/<pack>/`, and
  readers. Hooks run on the script's own state within 50 ms, give the stock answer while the script is running, and
  end it on an error. A script that gave the client anything stays after its main chunk returns.
  `scripts.autoload` starts the player's. The script host's structs moved to `src/mq/lua_script.h`.
- Phase 6a, the host functions the NMS port needs: done. `close_spell_book`, `clear_spell_recovery`,
  `set_window_pet`, `inspect_item`, `max_camera_distance` / `set_max_camera_distance`, `write_chat_color`,
  `server_config_dir`, `character_config_dir` and `zone_name`; `right_click_item` takes the class the casting check
  sees; `HailstonePlayer` adds the cast and scribed spells, the stand state, the pet window's pet and the PC Spells filter;
  `set_vitals` also writes the player's current points. The Lua module has each. `core/byte_io.h` moved to the SDK
  (`sdk/hailstone_byte_io.h`). An ini reader stays the plugin's own (the core's needs `core/atomic_file`).
- Phase 6b, NMS as a plugin: done. `plugins/nms/` builds a `MODULE` against `sdk/` alone (its own install
  component, `nms`); `docs/plugins/nms.md` describes it. The core lost `ServerModule` and the Module box,
  `ConnectionOptions::extension` / `force_extension`, `nms_mode()`, the NMS decoders (ServerAuthStats,
  SendCharacterSets, PetList, personal loot) and the ImGui fallback windows; `ServerExtension` lost the hooks only
  the module used (packets, checksums, stats, zone events, heartbeat, commands, chat, options, window toggles,
  draw), which plugins do through their own callbacks. The windows moved to `plugins/nms/ui/<pack>/` over
  `hs.Plugin("nms")`; the waypoint window is Lua in both packs. Found on the way: plugin windows could not
  `dofile` the pack's libraries (now they fall back to the pack), table keys from a plugin were always strings
  (whole numbers now become integers), a plugin's NULL level line dropped the stock line (now it keeps it),
  `HAILSTONE_PLUGIN` did not export C names from C++ (`HAILSTONE_API`), and `HailstonePlayer` lacked `in_zone`.
- Phase 6c, the core's names: done. `opcodes_rof2.inc` is generated from stock EQEmu's `patch_RoF2.conf`
  (`tools/gen_opcodes.py` now needs the path): the 19 NMS opcodes left the table and `MercenarySwitch` (0x1b37)
  joined it; no other name or wire value changed. Hailstone's own server-script packets are `OP_HailstoneGuild*` /
  `OP_HailstoneSettings*` (`HailstoneGuild`, `HailstoneSettings`, `encode_hailstone_guild` ...), with the same
  wire values and magic numbers. `tools/check_core_names.py` fails CI (job "no server code in the client") when
  src/, sdk/ or ui/ name NMS outside comments; `nmsc` and `nms-client` are allowed.
- Since then: the NMS plugin moved to a repository of its own (projecthailstone/hailstone-nms), released as an
  add-on and cloned into `plugins/nms` to build with the client; its docs (`docs/nms.md` there) went with it, so
  `docs/plugins/nms.md` and the `tests/nms_*` files named below are not in this repository (docs/plugins.md). The
  sections below describe the code as it was before the move.

## 1. The `ServerExtension` hooks

| Hook | Called from | Group | v2 form |
|---|---|---|---|
| `world_packet`, `zone_packet` | game_connection.cpp:420, :1062 | Have | `OnPacket` (in, drop). Needs the world stage (§6) |
| `outgoing_checksum` | game_connection.cpp:409 | Have | `OnPacket` (out) with `resize_packet`. World stage |
| `server_stats_changed` | game_connection.cpp:1073 | Plugin | goes away: the plugin decodes `OP_ServerAuthStats` itself |
| `zone_server_changed` | game_connection.cpp:534 | Have | `OnBeginZone` (fires at game_connection.cpp:355; check it is the same moment) |
| `zone_entered` | game_connection.cpp:963 | Have | `OnZoned` |
| `retry_zone_entry` | game_connection.cpp:309 | Callback | `OnZoneEntryRetry` → bool, or host `rerun_zone_entry()` |
| `heartbeat` | game_connection_spells.cpp:127 | Have | `OnPulse` |
| `command` | game_connection_social.cpp:161 | Have | `AddCommand` |
| `handler_command` | game_connection_items.cpp:1010, _progress.cpp:866 | Callback | `OnGameCommand(handler, args)` → handled (`/loot`, `/alt toggle`) |
| `hide_chat_line` | game_connection.cpp:116 | Have | `OnFilterChat` |
| `who_lines` | game_connection_social.cpp:1923, lua_ui_social.cpp:1382 | Callback | `OnWhoLines(response)` → lines (the Who window reads it too, so dropping the packet is not enough) |
| `class_mask`, `character_class_mask` | game_connection.h:1104, :334, game_connection.cpp:607 | Core | host `set_class_mask(mask)`, `set_character_classes(name, mask)` |
| `cast_class` | game_connection_spells.cpp:43 | Callback | host `set_cast_class(c)` from `OnPulse` (the patch's cast spoof) |
| `is_spellcaster` | game_connection_player.cpp:569 | Callback | `OnIsSpellcaster()` (decision 4: the server's rule) |
| `spell_level_needed` | game_connection_spells.cpp:48 | Callback | `OnSpellLevelNeeded(spell_id, class)`, the patch's scribe rules |
| `song_blocked` | 4 sites (cast, item use, ability commands) | Callback | `OnSongBlocked(spell_id)` |
| `own_begin_cast_line` | game_connection_spells.cpp:754, :768 | Callback | `OnOwnBeginCastLine(spell_id)` |
| `can_use_gem` | game_connection_spells.cpp:313 | Callback | `OnCanUseGem(gem)` |
| `combat_ability_timer`, `discipline_reuse_timer` | game_connection_cast.cpp:489, :493 | Callback | `OnAbilityTimerId(spell_id)`, `OnDisciplineReuse(timer_id)` |
| `before_` / `after_combat_ability` | game_connection_cast.cpp:475, :478 | Callback | `OnCombatAbility(spell_id, phase, used)` |
| `combat_effect_label` | lua_ui_abilities.cpp:191 | Callback | `OnLabel` (the same as `label_text` below) |
| `can_use_all_items` | lua_ui_itemdisplay.cpp:390, game_connection_items.cpp ×3 | Flag | `items_any_class` |
| `wide_item_links` | lua_ui_items.cpp:368 | Flag | `wide_item_links` |
| `class_desc` | game_connection.h:1112 | Core | "War/Clr" from the mask; the patch's id encodings (≥1000, 200..449) are not needed once the core holds masks |
| `guild_member_class` | lua_ui_social.cpp:210, social_ui.cpp:32 | Callback + Core | `OnGuildClass(value)` → mask (NMS stores 1000 + mask); the core draws it |
| `window_title` | client_app_login.cpp:652 | Core | "Name [War/Clr]" from the mask |
| `hide_check_class` | actions_ui.cpp:691 | Flag | `hide_any_class` |
| `aa_any_level`, `aa_train_all` | game_connection_progress.cpp:346, :356, :388 | Flag | `aa_any_level`, `aa_train_all` (the core keeps the Train All box, shown under the flag) |
| `max_vitals_classes` | stats_ui.cpp:99, :105 | Callback | `OnVitalsClasses(class)` → classes (decision 4: the server's rule) |
| `character_vitals` | stats_ui.cpp:37, game_connection_cast.cpp:49, _spells.cpp:178, :924 | Core | host `set_vitals(...)` (the plugin sets the server's values) |
| `item_right_click` | game_connection_item_use.cpp:58 | Callback | `OnItemRightClick(slot)` + host `right_click_item_original(slot)` |
| `skill_rules` (`gate`, `spoof_class`, `tracking_allowed`) | stats_ui.cpp:36, actions_ui.cpp:374, mechanics/abilities.cpp | Callback | already plain function pointers: `OnSkillGate`, `OnSkillSpoofClass`, `OnTrackingAllowed` |
| `level_needed_line` | lua_ui_itemdisplay.cpp:392 | Callback | `OnItemLevelLine(spell_id)` → text |
| `merchant_level` | lua_ui_items.cpp:469 | Callback | `OnListLevel(item)` → text |
| `label_text` | lua_ui_values.cpp:617 | Callback | `OnLabel(id, text buffer)` |
| `hide_xtarget_slot` | game_connection_group.cpp:174, lua_ui_player.cpp:750 | Callback | `OnHideXTargetSlot(slot)` |
| `option`, `set_option` | client_app_options.cpp:307, :363 | Host | `add_option(name, label, default)`: Options shows it and saves it under the plugin's key |
| `window_toggle` | client_app_ui.cpp:284 | Host | `add_window(name)` so `/toggle` and the EQ menu know it |
| `draw` | client_app_items.cpp:186 | Have | `OnUpdateImGui` (only the ImGui fallback loot window uses it; see §5) |

Count: 9 have, 6 flags, 21 callbacks, 5 core, 2 host additions, 1 removed.

Hooks that run often (`hide_xtarget_slot`, `song_blocked`, `spell_level_needed`, the labels) are one C call through a
function pointer, which costs about the same as the current virtual call.

## 2. NMS code in the core outside the hooks

| What | Where | Becomes |
|---|---|---|
| `nms_mode()`, `nms_server_ack_`, the 0x140f swallow, `force_extension` | game_connection.h:1089-1098, .cpp:415, :1057 | removed. The plugin does its own detection and calls host `set_active(true)` |
| `ServerModule` (auto / nms / none), the login Module box | server_profile.h:27, dev_screens.cpp:505, client_app_internal.h:472, client_app_login.cpp:496 | removed. An old `server.ini` with `module=` still loads; the value is ignored |
| `[NMS] auth_key`, `hardware_id` | server_profile.h:65, .cpp:203, :245 | the plugin's own settings for each server (host `server_config_dir`, new) |
| `OP_ServerAuthStats` decode, `server_stats_` | zone_packets.h:683, .cpp:1529, game_connection.cpp:1066 | Plugin |
| `OP_SendCharacterSets` decode, `character_sets_`; `CharacterSetRequest` encode | world_packets.h:273, :296, game_connection.cpp:424 | Plugin. It feeds `set_character_classes` |
| `OP_PetList` decode | pet_packets.h:60, game_connection_group.cpp:473 | Plugin decodes it and calls host `set_pet_list(...)`. **Core keeps** `PetState`, focus, the extra pet gauges and the pet list window |
| Personal loot list decode, `personal_loot_`, `personal_loot_action` | item_packets.h:451, .cpp:788, game_connection_items.cpp:284, game_connection.cpp:1316, game_connection.h:1424 | Plugin (it already owns the other four loot packets) |
| The personal loot ImGui window, the UI demo's copy | item_windows.h:5, ui_demo.h:15, :68, ui_demo.cpp:286 | Plugin; removed from the demo |
| `/fov` reaches the camera through `ExtensionAppContext` | client_app_internal.h:137, :1101 | host `set_camera_distance`, `set_max_camera_distance` (the Options zoom range stays core) |
| `nms::add_lua_windows`, `nms::register_lua_api` | lua_ui.cpp:26, :298, :2477 | the plugin window and Lua bridge (§4) |
| NMS sources in `hailstone_engine` | CMakeLists.txt:157-166 | a separate `MODULE` library that can include `sdk/` only |

These stay in the core because they are already general multiclass and multi-pet support: the classes on character
select (dev_screens.h:192), the `player_classes` mask in the item display, inventory and item moves
(item_display.h:33, inventory.h:61, item_moves.h:78), the social class masks (social.h:59), the AA class mask
(alt_adv.h:80), the group pet state (group.h:61-88) and the pet window's gauges (lua_ui_misc.cpp:178, :232).

## 3. Opcodes

`src/net/opcodes_rof2.inc` is generated from **the NMS server's copy** of `patch_RoF2.conf`, not EQEmu's. Comparing
it with the stock file (EQEmu's `utils/patches/patch_RoF2.conf`) gives 21 opcodes that only NMS has:

| Opcodes | Today handled in |
|---|---|
| ServerAuthStats 0x1338, PetList 0x1341, SendCharacterSets 0x1405, CharacterSetRequest 0x1404, PersonalLootList 0x1410, PersonalLootAction 0x1411 | core (move to the plugin) |
| SkillTimers 0x1339, CAuth 0x7777, WaypointList 0x1402, WaypointRequest 0x1403, MulticlassCharSelect 0x1340, PersonalLootFilters / Settings / History 0x1412-0x1414 | the NMS module |
| CustomDiscTimer 0x1400, CharacterSetCreate / Move / UnlockRequest 0x1406-0x1408, SuppressBuffNameInfo 0x1409 | nobody |
| NmsPolicy 0x1420, NmsPolicyRequest 0x1421 | nobody, and not in our table |

Plan: generate the core table from the stock file. The plugin keeps its opcodes by wire value (`OnPacket` already
passes the raw opcode, and `send_packet` takes one). The one stock opcode missing from the NMS copy,
MercenarySwitch 0x1b37, already has its own constant (game/mercenary.h:121).

**Not NMS:** `OP_NmsGuild` / `OP_NmsGuildReply` (0x1430/0x1431, magic "NMSG") and `OP_NmsSettings` /
`OP_NmsSettingsRequest` (0x1432/0x1433, magic "NMSP") belong to Hailstone's own server script (`server/`). They stay
in the core. Their code names become `Hailstone*` (decision 5); the magic numbers stay, because deployed scripts use
them. (The opcodes later moved from 0x1422-0x1425 to 0x1430-0x1433, off Project Triune's 0x1422; the script still
answers the old ones.)

## 4. Lua and the UI packs

The module adds 11 functions to the `hs` table: `NmsLoot`, `NmsLootSend`, `NmsLootRule`, `NmsLootWindow`,
`NmsVault`, `NmsVaultSay`, `NmsVaultWindow`, `NmsAutoplay`, `NmsAutoplayConsole`, `NmsAutoskill`, `NmsCooldowns`.
It also adds a C++ `StockWindow` for its waypoint window (`WaypointsWnd`).

They push straight onto the client's `lua_State`. A DLL cannot do that: Lua is linked statically into the client,
and a plugin with its own copy of Lua would corrupt the state. v2 needs a bridge that passes values instead:
- the plugin registers `name → fn(args, out)`, and the SDK provides a small value API (nil, bool, number, string,
  array, table) to build the answer;
- Lua calls `hs.Plugin("nms").call("loot")` and similar. The `hs.Nms*` names are removed (decision 3).

The waypoint `StockWindow` becomes a Lua window like the others.

UI files that move to `plugins/nms/ui/<pack>/`:
- modern: `nmsloot.lua` + `lib/nmsloot.lua`, `nmsvault.lua` + `lib/nmsvault.lua`, `nmsautoplay.lua`,
  `nmscooldowns.lua`, `nmswaypoints.lua`
- gamepad: `nmsloot.lua`, `nmsvault.lua`, `nmsautoplay.lua`, `waypoints.lua`

Shared files that mention them and need a general replacement:
- `gamepad/system.lua:133` and `gamepad/hudextras.lua`: their list of the server's windows;
- `options.lua` in both packs: the hard-coded `nms_xtarget_hide_pets` row, which becomes plugin options from
  `add_option`.

`dofile("lib/...")` is resolved against the UI folder (lua_host.cpp:276). It needs to resolve against the plugin's
folder for plugin windows.

## 5. What the NMS module reads and calls in the core

This is what the SDK must offer as typed, size-prefixed structs. Text through `ParseMacroData` is too slow and too
loose for the hooks that run often.

| Use | Count | SDK |
|---|---|---|
| `world().spawns`, `player_id`, `player`, `target_id` | 21 | `get_spawn(id)` (id, name, owner, class, type, level), `player_id()`, `target_id()` |
| `profile()->class_`, `player_status().level`, `character_name()`, `visible_class()` | 16 | `get_player()` |
| `player_status_mut()` (writes the server's HP / mana / endurance) | 1 | `set_vitals` (§1) |
| `spell_table()`, `spell_state().buffs` | 6 | `get_spell(id)` (name, class levels, Bard byte, recast timer id), `get_buff(n)` |
| `inventory().at`, `cursor_scroll_spell()` | 3 | `get_item(slot)` (id, type, classes, name, icon) |
| `xtargets()`, `group()`, `raid()`, `pets()` | 7 | `get_xtarget(n)`, `in_group()`, `in_raid()`, `get_pets()` |
| `chat_filters()` (the PCSpells setting) | 1 | `get_option("filter.pc_spells")` |
| `ui_message(text, eqstr, color)` | 14 | `write_chat` (have), plus `write_eqstr(id, args, color)` |
| `send_zone_packet`, `send_command` | 9 | `send_packet`, `do_command` (have) |
| `spell_book_open`, `close_spell_book`, `clear_spell_recovery`, `request_window_toggle`, `set_window_pet`, `right_click_item_original` | 6 | host actions with those names |
| `own_files()` (the character's folder: `itemrules.ini`) | 1 | `character_config_dir()` |
| `state()` | 8 | `SetGameState` (have) plus `game_state()` |
| `nms_server_ack`, `server_stats`, `character_sets` | 10 | none: the plugin keeps these itself |
| Headers: `core/byte_io.h`, `core/ini_file.h`, `core/log.h`, `game/eq_names.h`, `game/zone_names.h`, `game/item_links.h`, `game/slash_commands.h` | | the SDK ships a header-only `byte_io` and `ini`. Names, zones and item links come from host lookups (`class_name`, `zone_name`, `make_item_link`). `log` is the existing host `log` |

## 6. When plugins load

The NMS module acts before the zone: it reads `OP_SendCharacterSets` at character select, rewrites the checksums
sent on `OP_SendCharInfo`, and runs its client check at the first zone-in. Today no plugin runs before the zone-in's
check of the server's files (docs/plugins.md). v2 needs plugins that load at server select, behind a new capability
bit (`login`) for the world-stage hooks.

From reading the code, without a live check: the forced "NMS server" setting is not needed for the checksums. The NMS
world sends `OP_SendCharacterSets` before each `OP_SendCharInfo`, and the client already waits for it
(nms_extension.cpp:404). So detecting the server is enough, as long as the plugin is loaded by then.

## 7. Tests and build

- `tests/nms_module_test.cpp` (643 lines) and `tests/nms_windows_test.cpp` (168) test the module directly. NMS cases
  also sit in `ui_windows_test`, `item_use_test`, `options_social_test`, `layout_test`, `character_files_test`,
  `social_test`, `item_packets_test`, `combat_ability_test`, and the fuzz lists (`fuzz_decoders.inc`,
  `fuzz_libfuzzer.cpp`).
- Proposed:
  - unit tests build the plugin's sources against `sdk/` into their own test binary;
  - integration tests load the built `.so` / `.dll` into the mock server, as the tests already do with `MQ2Example`;
  - the fuzz list follows the decoders into the plugin;
  - a CI grep fails when the core names NMS outside comments.

## 8. Surprises

- `docs/nms-module.md` does not exist. `server_ext.h`, `nms_extension.h` and CMakeLists.txt:157 point to it.
- The core opcode table comes from the NMS server's conf, so NMS opcodes look like stock ones (§3).
- Hailstone's own script opcodes are named `Nms*` (§3).
- The NMS server's conf has `NmsPolicy` 0x1420/0x1421, which nothing here uses. What it does is not known.
- Most NMS "multiclass" rules are the patch's workarounds for characters that are Bards in the profile (class spoofs,
  the 255 level bytes, the scribe spoof). They belong in the plugin. What generalises is the class mask and how it
  is displayed.

## 9. Decisions (2026-10-29)

1. **Delivery: both.** A player can install the plugin in `plugins/nms/`, and a server can ship it in
   `client_files/plugins/` through Hailstone's script. When both exist, the server's copy wins, as plugins.md already
   says for plugins with the same name.
2. **Early loading: yes.** A server's plugins load at server select, and a new `login` capability controls the
   world-stage hooks (§6).
3. **Lua: `hs.Plugin("nms")` only.** There is no shim, so the `hs.Nms*` names go away, and user scripts that call them
   break. The name is capitalised like the rest of the `hs` table.
4. **Multiclass rules: the server decides.** `is_spellcaster` and `max_vitals_classes` become plugin callbacks
   (`OnIsSpellcaster`, `OnVitalsClasses`). The core keeps only the class masks and how they are displayed, and has
   no multiclass rule of its own.
5. **Rename: yes.** `OP_NmsGuild*` / `OP_NmsSettings*` and their structs become `Hailstone*` in the code. The wire
   values and magic numbers stay.
