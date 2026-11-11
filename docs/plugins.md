# Plugins, MacroQuest Lua scripts and the server's rules

## Managed distributions

An operator can build a managed distribution with `HAILSTONE_MANAGED_CLIENT=ON`
and `HAILSTONE_BUILTIN_ADDON_SOURCE_DIR` naming a separately maintained SDK-only
static module. It must provide the `hailstone_builtin_addon` target, a safe
`HAILSTONE_BUILTIN_ADDON_ID` target property and the descriptor factory in
`sdk/hailstone_builtin_addon.h`. The client requires that module to initialize
before gameplay. Its identity and callbacks are linked into the executable;
there is no user-selectable add-on directory, download, consent or unload step.
The output is `build/hailstone-managed/`, separate from an ordinary client's tree.

Managed mode is enabled before argument parsing and cannot be relaxed by INI
settings, missing/cached server rules, zoning or reconnecting. Dynamic plugins,
player and server Lua scripts, Lua social code/faces/dry runs, add-on management
and tools, MQ command expansion, automatic navigation/stick, mesh-assisted
follow, autoskills, custom bulk/tidy/sell item jobs, Claim All Parcels and gameplay
diagnostic drivers are disabled. The Find and atlas views keep their maps and
guide paths, with their automatic walking controls hidden and walking APIs disabled.
Ordinary RoF2
commands and five-line socials, melody, autofire, native follow and manual
ability/AA use retain their ordinary handlers.

The UI's executable Lua must match both the logical path and content hash in a
manifest compiled from the shipped sources. Edited/replacement packs, modules,
dynamic code loading, developer reloads and test-action settings cannot supply
additional executable UI. Settings such as window positions remain editable.
The linked module's optional `ui/<pack>/` files are packaged under
`builtin/<id>/ui/<pack>/` and receive the same manifest checks.

The generic updater, staged installs and automatic rollback are disabled in
managed mode so they cannot replace it with an unrestricted release. A dedicated
distribution update channel is future work. The module's native code is trusted
first-party code, not a sandbox: host capabilities do not constrain what C++ can
do directly. These restrictions govern the supported client; modified executables
and external input automation still require server-side rule enforcement.

The following sections describe ordinary builds. Managed restrictions take
precedence over all of their server-granted capabilities.

The client can be extended in three ways. All three sit on one hook bus (`src/plugin/hooks.h`) that sees every
world and zone packet in both directions, chat lines, typed commands, the game state, zoning, spawns and each
frame:

| What | Where | Who decides |
|---|---|---|
| **Lua windows**: the game's windows, rewritten in Lua (`ui/<pack>/*.lua`, `src/ui/lua`) | ships with the client | always on |
| **MacroQuest Lua scripts**: automation with MQ's `mq` module, `/lua run <name>` | the server's `scripts/`, then `scripts/` (then the install's) | the server's `hailstone.cfg` |
| **Compiled plugins**: shared libraries with MQ-style callbacks (`sdk/hailstone_plugin.h`) | the server's `plugins/`, then `plugins/` or `plugins/<name>/` (then the install's) | the server's `hailstone.cfg` |

A player installs scripts and plugins from GitHub with the add-on manager on the login screen (docs/addons.md), or
puts them in the folders by hand. An add-on gets no more than the capabilities its manifest declares (below).

The folders are in the Hailstone folder (`docs/layout.md`). A compiled plugin's binary can sit directly in
`plugins/` or in its own folder `plugins/<name>/`; that folder, under the Hailstone folder, is also where
`host->config_dir` points, so the plugin keeps its settings beside its binary. A script's `mq.pickle` files go to
`scripts/config/` (`mq.configDir`) when the script may do everything, and to a folder of its own,
`scripts/config/<script>/`, when it may not (an add-on with a ceiling, the player's scripts under a server's
`player_caps`). Such a script reads and writes only there, so it cannot change or read the settings of a script
that may do more. `mq.configDir .. '/MyScript.lua'`, the usual MQ path, works either way. The folder is the script's
name as it is, but for what would not stay written so on every system, which is `%XX` (two hex digits): `%`, control
characters and bytes past ASCII, `/ \ : * ? " < > |`, a last `.` or space and the first letter of a device name
(`con`, `nul`, ...). So no two scripts share one (`mq/lua_mq.cpp` `script_folder_name`). Up to 0.6 every byte but
letters, digits, `-`, `_` and `.` was `_`, so `a b` and `a_b` shared `config/a_b/`: a script whose old folder is
there, with none of its new name yet, has it moved to its new name at its start, unless another script in the script
folders had the same old folder (then the old folder stays, and the script starts with an empty one).

**The server decides, through a file.** A server with Hailstone's script (`server/`) sends its `client_files/` folder
at the first zone-in of a login (docs/layout.md "Server files"). Three things in it concern this page:

- `hailstone.cfg`: the server's rules (below). They land in `servers/<profile>/hailstone.cfg`.
- `scripts/`: the server's own Lua scripts, into `servers/<profile>/scripts/`.
- `plugins/`: the server's own compiled plugins, into `servers/<profile>/plugins/`. Only this platform's libraries
  come: `.dll` on Windows, `.so` on Linux, `.dylib` on macOS, and from a platform folder (`plugins/linux-x64/`,
  `plugins/windows-x64/`, `plugins/macos-arm64/` ...) only this build's, with the folder taken out of the path.

Then:

| The server | The player's own scripts and plugins | The server's own |
|---|---|---|
| has no `hailstone.cfg` (plain EQEmu, or the script without one) | run, with every capability | run, with every capability |
| has a `hailstone.cfg` | run only when it allows them, with the capabilities it grants | always run, with every capability; scripts start by themselves, and the player cannot stop them or unload its plugins. Its compiled plugins load only once the player allowed them (below) |

A file is the server's when its SHA-256 is one the server listed for its `scripts/` or `plugins/` this login. Editing
one makes it the player's.

**Before the zone-in.** The server's files are checked at the first zone-in of a login. From the world login until
then, at character select too, the client uses the rules the last login left in `servers/<profile>/` (its
`hailstone.cfg` and the server's files with their hashes). Under those rules only scripts and plugins with the
`login` capability run. A server with no rules known yet (never visited, or without Hailstone's script) grants every
capability, as it does after the zone-in. This lets a plugin act where a server's own client does: at the world
login (its checksums), and at character select (the characters' classes). The zone-in's rules then replace these:
what they no longer allow unloads, and plugins without `login` load. Lua socials run only from the zone-in.
Disconnected, nothing runs.

On a server's first visit its own plugins have not come yet, so they start at the first zone-in. A copy the player
installed in `plugins/` can run from the world login.

### The server's compiled plugins ask first

A compiled plugin is native code: once loaded it can do anything the player's account can on the computer, whatever
its capabilities say (they limit only what it does through Hailstone). So a library in the server's folder
(`servers/<profile>/plugins/`) does not load until the player said yes to its bytes, on every server, with or without
a `hailstone.cfg`. The server's Lua scripts and its plugins' Lua windows are not asked about: they run inside
Hailstone's limits.

- **The question.** When such a plugin would load (the zone-in, the world login for a `login` one, `/plugin <name>
  load`) and the player has not answered for its SHA-256 on this server, it stays unloaded and a window asks: which
  server, each plugin's name, file, size and the start of its SHA-256, and that a compiled plugin runs with full
  access to the computer. Plugins found together are asked about together. The game goes on meanwhile: nothing waits
  for the answer, and the network keeps running.
- **Allow** loads them at once (from a copy of the bytes that were hashed, as every plugin loads) and keeps the yes.
  A plugin allowed in the middle of a zone missed what came before it loaded (the zone-in's packets), so a server
  extension may need the next zone-in to start working.
- **Don't allow** keeps them unloaded and keeps the no, so later zone-ins do not ask again; `/plugin <name> load`
  typed by the player asks again. The same command from a script or plugin (`mq.cmd`, `DoCommand`) never puts the
  question up, after a no, after the close box or before any answer: only the player asks for it. The server's features that need the plugin are missing: the client goes on without it, as it does
  when a plugin fails to load.
- **The close box** (Gamepad UI: B) puts the question away without an answer: the next zone-in asks again.
- **Nothing covers it.** While the question is up, its window (the UI pack's, or the client's own when the pack has
  none) is raised above every other window at the end of each frame, and no plugin's window or script's ImGui can
  draw over all windows (`ui.Foreground` is refused them) or find where the question is (`ui.FindWindowRect`).
  Only the pack's own code puts the client's prompt away for a window of its own (`eq.WindowShown("PluginConsent")`
  from a plugin's code does not), and a plugin's code neither sees the question nor answers it. A compiled plugin
  already loaded is native code and is not held by this.
- **Other bytes ask again.** The answers are for a SHA-256, not a file name: a server that updates its plugin asks
  again, and so does a file changed on disk. A plugin's new answer replaces the answers for its older versions.
- **Changing it later.** The add-on list on the login screen (docs/addons.md) shows each of the picked server's
  plugins with the answer: **Don't allow it any more** for one allowed (it stops loading on that server), **Ask me
  again** for one refused (the next visit asks). The developer window's **Plugins & Lua** shows the answer too, with
  **Forget my answer**, which takes effect at once (an allowed plugin unloads and is asked about again). Each of
  these acts on the plugin's bytes as the server has them now (the files it listed under that name) as well as on
  its name, so bytes allowed once stay refused when the server sends them under another name; the list says it is
  done only when the file says so for every one of them.

The answers are kept in `servers/<profile>/plugin_approvals.txt` (docs/layout.md), one line a plugin:
`allow <sha256> <name>` or `deny <sha256> <name>`. The server cannot write that file: its files land only in
`hailstone.cfg`, `scripts/`, `plugins/` and `assets/`. There is no setting or variable that skips the question. When
the file cannot be written, the answer holds for this run only. A box set's windows share the file, so an answer in
one window settles the same question in the others within a second.

The packs draw the question (`ui/modern/pluginconsent.lua`, the Gamepad UI's `dialogs.lua`) with
`eq.PluginConsent()` (nil, or `{server, serial, warning, plugins = {{name, file, size, sha256, short}}}`) and
`eq.AnswerPluginConsent(true | false | nil)`. A plugin's own code in the UI gets nil and cannot answer. Without a
pack window for it (`eq.ReplaceWindow("PluginConsent")`) the client draws its own.

## For players

- `/lua run <script> [args...]`, `/lua stop [script]`, `/lua pause [script]`, `/lua list`. From a script or plugin
  (`mq.cmd`, `DoCommand`, `eq.ChatSubmit`) `/lua stop` and `/lua pause` reach only a script that may do no more than
  it ("may not stop or pause" otherwise; with no name, the others are left alone): an add-on cannot end yours
- `/plugin list`, `/plugin <name> [load|unload]`
- A server's compiled plugins load only once you allow them: see "The server's compiled plugins ask first" above.
- **Add-ons** on the login screen installs a script or plugin from its store or a GitHub link, keeps it up to date and removes it
  (docs/addons.md). It lists the picked server's own too, which cannot be removed or turned off.
- Ctrl+Shift+D opens the developer windows, including **Plugins & Lua**. That window shows the server's rules,
  every script and plugin found with its SHA-256 and whether it is the server's (and for the server's compiled
  plugins, your answer, with **Forget my answer**), and buttons to run, stop, load and unload. A plugin whose name the server's `plugins/` also has is listed once, as the server's: that is the one
  `/plugin <name> load` loads, and yours of that name is not used on this server.
- The EQ menu's **Lua Scripts** row (Gamepad UI: More > Lua Scripts) opens the Scripts window; see "The Scripts
  window" below.
- `hailstone.ini`: `scripts.autoload` lists your scripts to start when the rules allow them, for every character
  (empty by default; a Lua plugin is usually one of these). The Scripts window's Autorun is the same for one
  character.
- `hailstone.ini`: `plugins.autoload = *` (the default) loads every allowed plugin of yours at the zone-in. A
  comma-separated list loads only those plugins, and an empty value loads none. The server's own load regardless,
  once you allowed them.
- When a script tries something its capabilities do not cover, the chat window says so, for example
  `[MQ] myscript: /cast needs the server's 'actions' permission`.
- Your own typing is never limited. Capabilities only limit what scripts and plugins do on your behalf.

## Capabilities

Each running file carries a set of capability bits: every one for the server's files and on a server without a
`hailstone.cfg`; for the player's under a `hailstone.cfg`, those its `player_caps` grant. A file of an add-on the
manager installed gets no more than its manifest's `caps`, whatever the server allows (docs/addons.md "Capabilities":
`plugin::Policy::set_ceilings`, by the file's hash).

The bits are checked again whenever they change (the server's `hailstone.cfg` arriving at the zone-in after a first
visit's provisional rules, a lowered ceiling): a compiled plugin that stays loaded with fewer of them gets no more
packets (`OnPacket`), chat lines (`OnIncomingChat`, `OnFilterChat`), spawn events or command lines (the server
extension's `command_line`) from the moment it lost the bit, and its web tabs close.

| Bit | Name | Lets a script or plugin |
|---|---|---|
| 1 | `read` | read game state: `mq.TLO`, `${...}`, `mq.parse`, `ParseMacroData`, spawn events |
| 2 | `commands` | run commands outside the groups below: windows, options, `/who`, other plugins' commands, `/lua`; add commands of its own (`mq.bind`, `AddCommand`) |
| 4 | `target` | change the target: `/target`, `/assist`, `/xtarget`, ... |
| 8 | `actions` | act: `/cast`, `/disc`, `/doability`, `/useitem`, `/attack`, `/memspellset`, non-movement `/keypress` |
| 16 | `move` | move: `/face`, movement `/keypress` (forward, jump, ...), `/follow`, `/sit`, `/stand`, `/duck`, `/camp` |
| 32 | `chat` | speak: plain text, `/say`, `/tell`, `/g`, `/gu`, `/rs`, channels, emotes |
| 64 | `events` | see chat lines: `mq.event`, `OnIncomingChat`, and hide them (`OnFilterChat`) |
| 128 | `ui` | draw windows: `mq.imgui.init`, `OnUpdateImGui` |
| 256 | `packets` | see, change, drop and send raw packets (compiled plugins: `OnPacket`, `send_packet`) |
| 512 | `extend` | change the client's rules as a server's own client does (compiled plugins: the server extension below) |
| 1024 | `login` | run before the zone-in: from the world login and at character select (above) |
| 2048 | `net` | open the plugin's own TCP connections (compiled plugins: `tcp_open`, below), for a service the server runs beside the game |
| 4096 | `web` | web pages: `/url`, `/www`, tabs of its own in the in-game browser (`mq.web`, `eq.Web*` in its windows, `web_*`); http and https only, on the hosts `[web] allow` names ("Web pages" below) |

The check sits in one place, the command filter (`src/mq/commands.cpp`). It looks at the capabilities of the code
running now (`plugin::caller_caps()`). So a command reaches the same check whether a script sends it with `mq.cmd`,
`/docommand`, `/squelch`, a bind, or a plugin's `DoCommand`. The game's own commands are classified by their entry in the
command table (`plugin::command_caps`). MQ's and plugins' commands declare what they need.

## MacroQuest Lua scripts

Only Lua scripts are supported. MQ's old `.mac` macro language is not.

A script is `lua/<name>.lua` or `lua/<name>/init.lua`, the same layout MacroQuest uses. Each script gets its own Lua
5.4 state and runs as a coroutine that the client resumes once a frame. `mq.delay` yields to the next frame. A
script that runs more than 3 ms without yielding is paused until the next frame, like MQ's turbo, so a busy loop
cannot freeze the client. One that cannot pause for 5 s is stopped; an `mq.imgui` window's function, which cannot
pause, is stopped the same way. Only the script's own run and its binds are paused: a coroutine the script made itself
(`coroutine.create`, `coroutine.wrap`) is never yielded by the pause, which would hand its `coroutine.resume` an
early yield with no values, so long work inside one counts towards the 5 s and is not spread over frames. A bind
whose function runs commands that start binds of the same script again (`mq.bind('/x', function() mq.cmd('/x')
end)`) ends the script once they nest 16 deep; a command of more than 1000 words for a bind is refused (a line in
the chat) and the script runs on. Each script's memory is capped at 256 MB: past it the script's own
code gets "not enough memory", and a script that ends that way is stopped with its error. A script's tables cannot have a `__gc`
finalizer (`setmetatable` refuses one): Lua runs finalizers with no way to stop them, and the last at the script's
end, after what it registered is taken back. For the same reason an `mq.TLO` value's metatable is locked
(`getmetatable` gives `"mq.TLO"`).

### The `mq` module

| | |
|---|---|
| `mq.TLO.<Name>...` | MQ's TLOs (below). Call with no arguments for the plain value (`mq.TLO.Me.PctHPs()`, `nil` for NULL). Call with arguments for an indexed member (`mq.TLO.Me.Buff(1).Duration.TotalSeconds()`, `mq.TLO.Spawn('npc radius 50').ID()`). Values also work with `tostring`, `..`, `==`, `<` and `<=`. |
| `mq.cmd(line)`, `mq.cmd.<name>(args...)`, `mq.cmdf(fmt, ...)` | a command line, with `${...}` parsed |
| `mq.delay(ms \| '2s' \| '500ms' \| '1m' [, condition])` | wait, until the time passes or `condition()` is true. A wait beyond 9e15 ms (`math.huge`) is cut to that; NaN is an error |
| `mq.event(name, pattern, fn)`, `mq.unevent`, `mq.doevents([name])`, `mq.flushevents([name])` | chat events: the whole line must match; `#*#` matches any text (as much as it can); `#1#`, `#2#`, ... (as little as they can) are passed to `fn(line, arg1, ...)` in their number's order; neither takes a line break; any other `#` is literal (`mq/event_pattern.h`). A script has at most 256 events, a pattern is at most 1024 bytes and a name 256, and a script's patterns together at most 32 KB (an error past them): every pattern is matched against every chat line, outside the script's time slice. A line another script or a plugin wrote (`print`, `/echo`, `WriteChatf`) raises events too, as `/echo` does in MQ, but a handler then gets no more than that code may do: one that would get more runs as a thread of its own (as a bind), held to both scripts' capabilities for each of its resumes, and `doevents` goes on without waiting for it |
| `mq.bind('/cmd', fn)`, `mq.unbind` | a command (needs `commands`); `fn` gets the words and may `mq.delay`. The client's own commands cannot be bound: the game's (with their abbreviations, so not `/g` or `/r` either), `/lua`, `/plugin` and MQ's (`/echo`, ...), nor a name another script or plugin has now (an error naming it): what the player types for one is never handed to another. A bind runs with the script's capabilities and those of whoever ran the command (another script, a plugin, the player all) together, at its start and again after each `mq.delay` |
| `mq.imgui.init(name, fn)`, `mq.imgui.destroy`, `mq.imgui.exists` | a window drawn each frame with `ImGui` |
| `mq.web.open(url [, w, h])`, `.navigate`, `.nav`, `.close`, `.eval`, `.post`, `.messages`, `.text`, `.state`, `.tabs`, `.view(tab, w, h)`, `.available` | the script's own tabs of the in-game browser ("Web pages" below); `view` in an `mq.imgui` window |
| `mq.parse(text)`, `mq.gettime()`, `mq.exit()` | |
| `mq.getAllSpawns()`, `mq.getFilteredSpawns(fn)` | spawn values |
| `mq.pickle(path, table)`, `dofile(path)` / `loadfile(path)` | settings files under `mq.configDir` (`scripts/config/`, or `scripts/config/<script>/` for a script without every capability, above); read back as data, with nothing to call |
| `mq.luaDir`, `mq.configDir` | |
| `print(...)`, `printf(fmt, ...)` | the chat window |
| `require('mq')`, `require('ImGui')`, `require('mymodule')` | modules next to the script or in the script folders. **Each module file is checked like the script: the server's, or the player's when allowed. A module runs with the requiring script's capabilities, so its own file must be allowed at least those:** an add-on's module (under its manifest's ceiling) does not run in a script that may do more, while the player's own libraries (`lib/Write.lua`) still serve the player's scripts and add-ons. |
| `ImGui.*`, `ImGuiWindowFlags`, `ImGuiCond`, `ImGuiCol`, ..., `ImVec2`, `ImVec4` | MQ's ImGui names and argument orders over the client's `ui` functions (`src/ui/lua/lua_imgui.cpp`) |
| `ImGui.GetClipboardText()`, `ImGui.SetClipboardText(text)` (also `ui.*`) | the player's clipboard: only for a script that may do everything (every capability; the server's own scripts, the player's under no `hailstone.cfg` or `player_caps = all`), never for an add-on with a ceiling. What the player copied may be a password, and what a script puts there is pasted where the player trusts it |

Left out on purpose:
- `load` and `string.dump`: no running code from strings.
- `io` and `os.execute`, and every other `os` function except the clock: no file or process access.
- Lua's own `package` searchers: `require` only finds files the server's rules allow.

### TLOs

`Me`, `Target`, `Spawn[...]`, `SpawnCount[...]`, `NearestSpawn[n, ...]`, `Group`, `Zone`, `Spell[id|name]`, `Cursor`,
`FindItem[name|=exact]`, `FindItemCount[...]`, `Pet`, `Window[name]` (`.Open`), `Plugin[name]` (`.IsLoaded`), `Time`,
`GameTime`, the game TLO (`.GameState`, `.Server`), `Heading[deg]`, `Int`, `Float`, `Bool`, `String`, `Select`. Compiled
plugins can add more.

The data types use MQ's member names and inheritance:
- `character` → `spawn`, `buff` → `spell`, `groupmember` → `spawn`.
- `class`, `race`, `heading` (`Degrees`, `ShortName`, ...), `timestamp` (`TotalSeconds`, `TimeHMS`, ...), `item`.
- MQ's pure types: `int`, `float` (`Deci`, `Precision`, `Int`), `string` (`Length`, `Upper`, `Left`, `Mid`,
  `Find`, `Arg`, `Token`, `Equal`, ...), `bool`. `Int[...]` and `float.Int` cut toward zero, and are NULL for NaN, an
  infinity or a number past a 64-bit integer (what MQ gives for those is not checked).

For example `Me.PctHPs`, `Me.Buff[n|name].Duration`, `Me.Gem[n|name]`, `Me.SpellReady[n|name]`, `Me.Casting`,
`Me.Combat`, `Me.XTarget[n]`, `Me.FreeInventory`, `Me.Inventory[mainhand|pack1|23]`, and on spawns `Target.Distance`,
`Target.HeadingTo`, `Target.ConColor`, `Target.Named`, `Spawn[...].Loc`, `X`/`Y` as MQ's (the server's) coordinates.

For a script that fights:
- Abilities by name (a discipline's also without its "Rk. II"), an AA's group or a skill's number, as the Ability Book
  sees them: `Me.AbilityReady[Kick]` (a skill), `Me.CombatAbilityReady[name]`, `Me.AltAbilityReady[name|group]`,
  and `Me.AbilityTimer`, `Me.CombatAbilityTimer`, `Me.AltAbilityTimer` (a timestamp, the time left). Ready means
  its reuse timer has run out; its endurance and mana are the script's to check.
- A spawn's buffs: the player's own, or the target's as OP_TargetBuffs gave them. A server sends those only to some
  players (stock EQEmu: GMs, the Inspect Buffs AA or leadership, or its `AlwaysSendTargetsBuffs` rule), so ask
  `Target.BuffsPopulated` first. `Target.Buff[n|name]` (a buff: `.Duration`, `.Caster`, `.ID` the slot ...),
  `.BuffCount`, `.BuffDuration[name]`, `.MyBuff[name]` and `.MyBuffDuration[name]` (cast by the player), and the
  state a buff puts it in, as that buff, by its spell effects (EQEmu's spdat.h): `.Mezzed` (SPA 31), `.Charmed`
  (22), `.Feared` (23), `.Rooted` (99), `.Snared` (3, negative), `.Slowed` (11, under 100%).
- Line of sight: `Spawn.LineOfSight` (the player's eyes to the spawn through the zone's collision, the check the
  client's own abilities make), `LineOfSight[y,x,z:y,x,z]` (two points in MQ's order); NULL before the zone's
  collision is loaded, and for a coordinate that is not a finite number below 1000000 in size.
- `/stick` and the `Stick` TLO (`docs/navigation.md`).

Spawn searches use MQ's words:
- kinds: `pc`, `npc`, `corpse`, `npccorpse`, `pccorpse`, `pet`, `merc`, `any`
- filters: `id N`, `notid N`, `radius R`, `zradius R`, `range MIN MAX`, `level N`, `class X`, `race X`, `named`,
  `targetable`, `group`, `nopet`, `gm`, `trader`, `buyer`, `afk`, `lfg`, `linkdead`, `loc X Y [Z]`
- names: a partial name, `=Exact` or `exact`

`targetable` is the game's own test, as in MQ: the spawn's Targetable flag on and its body type not 11 (EQEmu's
NoTarget, which servers give portal and trigger NPCs). `Spawn.Targetable` is the flag alone, as in MQ, so a script
that filters spawns itself checks `BodyType` too.

Not yet:
- TLOs: `Raid`, `Mercenary`, `Corpse`, `Merchant`, `Ground`, `Macro` and other MQ plugins' TLOs (`Navigation` is
  built in: `docs/navigation.md`)
- `Math.Calc`, the `AltAbility` type (`Me.AltAbility[...]`)

### MQ commands

`/echo`, `/target` (`id N`, `clear`, `mycorpse`, or a spawn search), `/face` (target, `id`, `heading H`, `loc y,x`,
a search; `fast`), `/keypress <command> [hold|release]` (the game's key command names: `forward`, `jump`, `hot1_1`, ...),
`/squelch <cmd>`, `/docommand <cmd>`, `/noparse <cmd>`, `/lua ...`, `/plugin ...`. `${...}` is parsed in every
command line, typed or scripted, except `/noparse` and the lines the client's own windows send for a button
(`mq::LiteralLine`: the guild window's Go to URL, Join Channel and Update MOTD, a member's or friend's name; their
text is what other players and the server wrote, and the original has no `${...}`). `/nav` (MQ2Nav's commands),
`/moveto` and `/stick` are built in (`docs/navigation.md`).

`${...}` is expanded in one pass, innermost first (`${Me.Buff[${n}].Duration}` works as in MQ), and a value's text
is never parsed again: a spawn named `${Me.Name}`, or a guild, title or item name holding `${`, stays that text.
MQ2's `ParseMacroData` scans the line again after each replacement, so there such a value is evaluated too; here
it is not, a deliberate difference, so that what other players and the server name things cannot read the game
with the player's or a script's `read` permission (and send it on in a `/say`).

A line a script or plugin runs is checked against its permissions before and after the other line filters: a
compiled plugin's command-line rewrite (`command_line` in its hook table) cannot turn a line the script may run into
one it may not. MQ's filter sends the line on through the filters again, its final check placed after all of them.

## Lua plugins

A script can extend the client as a compiled plugin does, with no native code: the `hailstone` module
(`require('hailstone')`, or the global `hailstone`). Each part needs the same capability as in a compiled plugin.

| | |
|---|---|
| `hailstone.hooks{ name = function(...) ... end }` | answers the core's questions, by the names of the compiled plugins' `HailstoneHooks` (below): `is_spellcaster()`, `spell_level_needed(spell_id, class)`, `song_blocked(bard_level)`, `skill_gate(skill, mask, profile_skill)`, `who_lines(who)`, `label(id, text)`, `item_right_click(slot)`, `list_level(item)`, ... nil keeps the stock answer (`extend`). A hooks table with a metatable is read through it, in a protected call: an error in its `__index` ends the script |
| `hailstone.hooks{ nameplate = function(spawn, text) ... end }` | a Lua-only hook (no compiled plugin counterpart; `extend`): called for each visible world nameplate with the spawn table and its assembled name (title, last name, suffix and marks included). The spawn table is one the client refills for every plate (nothing is allocated per plate), so it is valid during the call only: copy the fields you need, do not keep the table. Return a string to replace the text, or nil to keep it; extensions run in order. It changes the world nameplate only, not the target window's labels or the spawn's data, and is gone when the script stops |
| `set_active(on)`, `set_flag(name, on)` | the switches: `wide_item_links`, `items_any_class`, `hide_any_class`, `aa_any_level`, `aa_train_all`, `augment_classes`, `research_classes`, `focus_classes`, `backstab_either_hand`, `solo_adventure` (`extend`) |
| `set_player_classes(mask)`, `set_character_classes(name, mask)`, `set_pet_list{{spawn =, class =}}`, `set_vitals{max_hp =, ...}` | what the server knows (`extend`) |
| `on_packet(stream, direction, opcode, fn)` | `fn(opcode, data)` for each such packet (`'world'` / `'zone'`, `'in'` / `'out'`, opcode 0 all): `true` drops it, a string replaces its data. With Lua's `string.pack` / `string.unpack` a script reads and writes any layout (`packets`) |
| `send_packet(stream, opcode, data)` | a packet to the server (`packets`) |
| `ui_function(name, fn)`, `option{name, label, min, max, get, set}`, `menu(label, command)` | for its windows, as a compiled plugin's (`ui`); `eq.Plugin("<script name>")` |
| `player()`, `spawn(id)`, `spawns()`, `spell(id)`, `item(slot [, sub])`, `buff(i)`, `xtarget(i)`, `eqstr(id, ...)`, `zone_name(id)`, `server()` | the game state as tables (`read`); `server()` is the server this login is on (`{name, host, port, world, world_host}`, nil before the world login); `player()`'s `cur_hp` and `max_hp`, like a compiled plugin's `HailstonePlayer` and `Me.CurrentHPs`, include the items' HP that EQEmu's own HP update leaves out, as the windows show them |
| `spell(id)` display fields | raw spell data: `range`, `ae_range`, `push_back`, `duration_formula`, `duration_cap` (ticks), `spell_type`, `resist`, `resist_adj`, `spell_icon`, `category`, `subcategory`, `cast_on_you`, `cast_on_other`, `wear_off`, and `effects` (12 1-based entries with `effect`, `base`, `base2`, `max`, `formula`). These are the loaded record values, not computed duration or effect descriptions; a script decides how to present them. |
| `eq.DisplaySpellId(n)`, `eq.DisplayItemData(n)` | in a Lua UI window, read the actual spell ID or item in display instance 1..6, or nil when absent. Item data gives `id`, `name`, `icon`, `lore`, `value` (copper), `stack_count`, `damage`, `delay`, `item_type` and `favor`; this is the inspected item, not necessarily one in inventory. |
| `right_click_item(slot [, class])`, `close_spell_book()`, `clear_spell_recovery()`, `set_window_pet(id)`, `inspect_item(id [, augments])`, `set_max_camera_distance(d)` | the client's own actions (`actions`) |
| `chat(text [, color])`, `server_dir()`, `character_dir()`, `max_camera_distance()` | a chat line in one of the game's colours; its folders for the server and the character (below: `plugin-config/<name>/`, the name written as for `mq.configDir`; a Lua social's dry run and face get the path and make no folder). A script with the name of a compiled plugin (installed, loaded or the add-on manager's, in any case) gets `plugin-config/script%3A<name>/` instead, so it cannot read or change what the plugin keeps there (a server's key); the add-on manager installs no script and plugin of one name either (docs/addons.md) |
| `server_rule(section, key)` | a setting from a section of the server's `hailstone.cfg` that is not the client's own (`[autocombat] hunter = off`), as written; nil when the server has no `hailstone.cfg` or does not set it |
| `item(slot [, sub])`'s fields | `id`, `name`, `type`, `item_class`, `classes`, `races`, `icon`, `stack`, `charges`, `req_level`, `bag_slots` (0 for an item that is not a bag), `click_spell` and `scroll_spell` (0: none); with a click effect also `click_type`, `click_level`, `click_cast_time` (ms), `click_recast` (s), `click_recast_type` (the timer group), `click_max_charges` (-1: unlimited) and `click_ready_in` (seconds until the recast is over, 0: ready). Slots are `/useitem`'s: 0..22 worn, 23..32 the general slots, `sub` the place in a bag from 0 |
| `autoskill_list()` | the player's autoskill list in order: `{key, name, endurance, mana, target_hp_below, target_hp_above, self_hp_below, when, keep_buff}` (`read`) |
| `alt_abilities()` | the abilities the AA window lists, one per ability in its order (by tab, then rank id): `{id, group, name, tab, category, rank, max_rank, cost, level, maxed, can_train, activated, prereqs}`. `id` is the rank the window shows (the next one to buy, or the owned last one), `rank` the ranks owned, `cost` and `level` the next rank's (`cost` 0 at the maximum), `can_train` the window's own check (points, level, class, expansion and prerequisites), `prereqs` `{{group, rank, met}}`; `player()` has `aa_points` and `aa_spent` (`read`) |
| `train_aa(group)` | buys the next rank of an ability, as `/alternateadv buy` does (no question, unlike the Train button), when the window's own check allows it; false, with nothing sent, when it does not. One rank a call; the points and ranks change when the server answers (`actions`) |
| `use_ability(kind, name)` | uses a skill (`"skill"`), combat ability (`"disc"`) or activated AA (`"aa"`) of the player's, by name (a combat ability's also without its rank), an AA's group or a skill's number, as its hot button does (the Ability Book's path); false when the player has none such (`actions`) |
| `hold_autoskill(on)` | the Ability Book's autoskill uses nothing while the script holds it (its switch stays as the player left it; the button shows "On (held)"); the hold goes when the script stops (`actions`) |

A script that gives the client any of these stays loaded after its main chunk returns, until it is stopped, and
everything it gave goes with it. Its hooks run on its own Lua state with its capabilities and must finish within
50 ms. While the script itself is running (a hook reached from its own command), they give the stock answer. An
error in a hook ends the script, as an error in its main chunk does.

A script in its own folder (`lua/<name>/init.lua`) with a `ui/<pack>/` beside it has windows for each UI pack, as
a compiled plugin has.

To start one: the server lists it in its `hailstone.cfg` `[scripts] run`; the player lists theirs in
`hailstone.ini` `scripts.autoload = name, name` (started when the rules allow them), ticks Autorun in the Scripts
window, or runs it there or with `/lua run <name>`. With the `login` capability it starts at the world login
(above). A script that another script or a plugin starts (`mq.cmd('/lua run x')`, a plugin's command) gets no more
capabilities than its starter has at that moment, whatever its own file would get; one its starter can give nothing
does not start. It first runs on the next frame. A script stopped and run again in one go (`/lua stop x` then
`/lua run x`) is closed before the new one starts, so the new one keeps the UI functions, options and menu entries it
registers; a script that stops itself and runs itself again from its own code is refused ("is still stopping"). Nothing under `scripts/config/` (`mq.configDir`, where `mq.pickle` writes) runs as a script or loads
as a module: those files are data. That holds for the folder in any case and with trailing dots or spaces
(`Config/`, `config./`), as Windows and macOS would open it, and through links. A script or module name is a path
inside the script folders: an absolute one (`/lua run /home/x/evil`, `require('/x')`) or one whose file is a link
leading out of the folder is not found.

## The Scripts window

Hailstone's own window (`ScriptsWnd`; `ui/<pack>/scripts.lua`, `app/lua_eq_scripts.cpp`), opened from the EQ menu's
**Lua Scripts** row (Gamepad UI: More > Lua Scripts), so a player does not need `/lua`. It lists every script in the
script folders (`ScriptHost::found`: `<name>.lua` and `<name>/init.lua`, the server's `scripts/` first, then
`scripts/` and the install's; `config/` left out; a name in two folders is listed once, as the one `/lua run` would
start), with its state (stopped, running, paused, ended, error), and for each:

- **Run** / **Stop** (`/lua run`, `/lua stop`), and **Pause** / **Resume** while it runs (`/lua pause`). The Modern
  UI's Arguments box gives Run its arguments, split as `/lua run` splits them. Gamepad: A runs or stops, X pauses.
- **Autorun** (Gamepad: Y): the script starts each time this character logs in. The list is the character's
  `scripts.ini` (`[Autorun] name = 1`, in `servers/<profile>/characters/<world>/<name>/`; docs/layout.md). It is
  read at the character's zone-in and its scripts start then, or at the next re-check of the rules (the zone-in's
  rules after the login's provisional ones), when the rules allow them, quietly when they do not. Until a character
  is in the world the box is greyed.
- A script the rules do not allow is greyed, with the reason in its tooltip (before the zone-in, a `hailstone.cfg`
  that allows only the server's, an add-on's missing permission). The server's own scripts under its
  `hailstone.cfg` show Stop and Pause greyed: the player cannot stop them.

Every script, however it was started, keeps running through a zone change: the zone-in's rules stay through the
zoning states, so nothing re-checks or stops it. Camping to character select makes the rules provisional again
(only `login` scripts keep running), and a disconnect stops every script. A script the player stopped is not
started again by a zone change: the same character's autorun list is read only once per login.

## Lua socials

A Hailstone extension of the game's socials (`src/game/lua_social.h`). A social can hold Lua instead of its five
command lines, and its hot button then runs that Lua with the same `mq` module scripts get. It has two parts:

- **Press** runs when the button is pressed. It sees `button` = `{social, bar, button, shift, ctrl, alt, presses}`,
  so one button can do different things with a modifier held. It also sees `state`, a table kept from press to
  press while the code stays the same (a rotation's place, a counter).
- **Face** is optional. It returns how the button looks, as a table: `{label, color (0xRRGGBB or {r, g, b}),
  cooldown (0..1 of a sweep left), overlay (corner text), enabled (false: dimmed), tooltip}`, or just a label string.
  It runs at most every 200 ms, reads the game state only, and cannot send commands.

```lua
-- [press]: heal the target, the big heal under 50% health; Shift heals yourself
if button.shift then mq.cmd('/target id ' .. mq.TLO.Me.ID()) end
if (mq.TLO.Target.PctHPs() or 100) < 50 then mq.cmd('/cast 1') else mq.cmd('/cast 2') end
-- [face]: the target's health on the button
local hp = mq.TLO.Target.PctHPs()
return {label = hp and (hp .. '%') or 'Heal', enabled = hp ~= nil}
```

**For players:**
- Make or edit one in the **Lua Social** window. Right-click a Lua social, pick **Lua** at the top of the Edit
  Social window, or use `/luasocial new` (the first empty social) or `/luasocial <1..120>`. A plain social is
  converted: its lines become `mq.cmd` calls, a `/pause` an `mq.delay` (which only a server in full mode waits) and a
  `/timer` a comment. **Commands** at the top of the Lua Social window goes back: a press made only of `mq.cmd('...')`
  and `mq.delay(ms)` lines, at most five, with no face, becomes command lines again.
- The editor colours the Lua and completes `mq.TLO` paths from the live game (Tab takes a name, Ctrl+Up / Ctrl+Down
  choose one), as well as `mq.` and `button.`. It marks the line of an error.
- **Dry run** runs the press with the chosen modifiers and lists the commands it would send, with any the server
  would block. Nothing is sent and waits are not waited. Reading works; anything else that would act or stay
  (`mq.bind`, `mq.event`, `mq.imgui`, `mq.pickle`, `mq.web`, the `hailstone` module's actions and hooks) is an error
  that ends the dry run ("does nothing in a dry run"). A dry run works even on a server without Lua socials.
- **Put on cursor** puts the social on the cursor, ready to drop on a hot bar.
- **Copy share text** / **Paste** share a social as one line (`hailstone-social:1:...`). The Edit Social window
  shares plain socials the same way (their lines in a `-- [lines]` section); pasting a Lua one there opens it here
  unsaved, and pasting a plain one here turns its lines into Lua. A client older than this one does not read a plain
  social's share text.
- Pressing a Lua social again while its Lua still runs (full mode) stops it, and so does a right click.
- The files are `socials/<n>.lua` beside the character's `hotbuttons.ini`, and can be edited outside the game too.

**What the server allows** (its `hailstone.cfg`'s `[socials]`):

| Mode | A press... |
|---|---|
| off | does nothing. Only the five-line socials run. |
| press (the default under a `hailstone.cfg`) | runs to its end at once: no `mq.delay`, no chat events, no windows, at most N commands, 50 ms of Lua |
| full (and on a server without a `hailstone.cfg`) | runs like a script, within `caps`: it may wait, use events and draw windows |

Faces run only when `faces` is on, and always with `read` alone.

## Compiled plugins

A plugin is a `.dll`, `.so` or `.dylib` built against `sdk/hailstone_plugin.h`, a plain C interface that works
with any compiler. On macOS the client loads only `.dylib`: CMake names a `MODULE` library `.so` there, so a
plugin's own build sets `CMAKE_SHARED_MODULE_SUFFIX` to `.dylib` (or the target's `SUFFIX`); the client's build does
this for the plugins built in its tree. It follows MacroQuest's plugin shape: export the callbacks you need and use the MQ-style helpers.

```c
#include "hailstone_plugin.h"
HAILSTONE_PLUGIN   /* the two required exports and the `host` global (the host table) */

static void hello(void* user, const char* args) { WriteChatf("Hello, %s!", *args ? args : "world"); }

HAILSTONE_EXPORT void InitializePlugin(void) { AddCommand("/hello", hello, 0); }
HAILSTONE_EXPORT void ShutdownPlugin(void) { RemoveCommand("/hello"); }
HAILSTONE_EXPORT void OnPulse(void) { /* every frame */ }
```

Callbacks:
- `InitializePlugin`, `ShutdownPlugin`, `OnPulse`, `SetGameState(state)`
- `OnBeginZone`, `OnEndZone`, `OnZoned`, `OnAddSpawn(id)`, `OnRemoveSpawn(id)`
- `OnIncomingChat(line, color)`, `OnFilterChat(line, color)` (nonzero hides the line): the server's lines and the
  client's own, not those a script or plugin writes (`print`, `/echo`, `WriteChatf`, a reply to its command), as MQ
  hands its own lines to `OnWriteChatColor` and not here: a plugin that answers "Bob tells you, ..." would otherwise
  answer whatever another add-on printed
- `OnUpdateImGui`, `OnPacket(stream, direction, packet)` (nonzero drops it)

Host functions: `WriteChatf`, `DoCommand(f)`, `AddCommand` (needs `commands`; 0 for one of the client's own commands, as `mq.bind`), `RemoveCommand`, `ParseMacroData` (`${...}`, so the
plugin can read any TLO), `AddTopLevelObject` (a TLO with members the plugin answers; refused, as in MQ, when the name is already a TLO, so a
plugin cannot replace `Me` or another plugin's TLO), `RemoveTopLevelObject` (only a TLO the plugin added itself),
`host->send_packet`, `host->resize_packet`, `host->config_dir`, `host->caps`, `host->imgui_context`.

The host table is a `PluginHostApi`: a struct of the client's functions, one per plugin, handed over by
`hailstone_plugin_set_host` before `InitializePlugin` and kept in the global `host`. Each function takes the table's own
`host->host` first, which tells the client which plugin is calling.

**The names up to 0.6.** The SDK was `nmsc_plugin.h` and `nmsc_byte_io.h`, with `NmscHost` and the other `Nmsc*`
types, the `NMSC_*` macros, the global `nmsc`, the namespace `nmsc` for `ByteReader` / `ByteWriter`, and the exports
`nmsc_plugin_abi`, `nmsc_plugin_set_host` and `nmsc_plugin_hooks`. A plugin written against them builds unchanged:
the two old headers are still in `sdk/`, include the new ones and add the old names back (`nmsc` is a macro for
`host`; define `NMSC_NO_OLD_GLOBAL` before the include to leave the name free). A built plugin loads unchanged: the
client looks for each export under its new name, then its old one. `HAILSTONE_PLUGIN` exports the two required
functions under both names, so a plugin built with this SDK loads in a 0.6 client too; one with hooks that should
work there exports `nmsc_plugin_hooks` as well.

The client calls a plugin on its own thread, and the host functions are for that thread only, from inside one of the
plugin's callbacks (`InitializePlugin`, `OnPulse`, a command, a hook, ...): the client's state has no locks. A call
from a thread the plugin started does nothing and returns 0 / NULL, with a line in the log once. A plugin that works
on a thread of its own hands its results over and acts on them in `OnPulse`.

A C++ plugin must not let an exception out of anything the client calls (callbacks, commands, TLOs, hooks, UI
functions, options). The client catches what it can: the call counts as unanswered (a TLO is NULL, a hook gives the
stock answer), the chat window says so once and the log names each place once, and an exception out of
`InitializePlugin` or `hailstone_plugin_hooks` unloads the plugin with everything it registered. That is best effort: an
exception crossing a C interface is undefined behaviour, and one from a plugin built with another compiler or runtime
may end the client.

`plugins/example/MQ2Example.c` uses all of it and builds with the client into `build/hailstone/plugins/`. To build one
yourself:

```sh
cc -shared -fPIC -O2 -I path/to/hailstone/sdk MyPlugin.c -o MyPlugin.so      # Linux
cl /LD /I path\to\hailstone\sdk MyPlugin.c                                    # Windows
```

MacroQuest's own plugin sources do not build as they are. They read the game executable's memory layouts (`pLocalPlayer`,
`PSPAWNINFO`, ...), which this client does not have. Port them to TLO reads (`ParseMacroData`) and the callbacks
above. A plugin that draws with Dear ImGui must be built with the client's Dear ImGui version and call
`ImGui::SetCurrentContext(host->imgui_context(host->host))`.

### Server extensions (ABI 2)

A server whose players used a patched client (a DLL beside eqgame.exe, a changed eqgame) can give Hailstone the
same changes as a plugin, with no change to Hailstone itself. With the `extend` capability a plugin can:

- **answer the core's questions** where such a client differs from the stock one: export
  `const HailstoneHooks* hailstone_plugin_hooks(void)`, a table of functions, each optional. They cover spell levels, songs,
  memorised gems, combat ability timers and whether their cool-down takes the reuse focus, the class used for casts,
  skill checks and tracking, the guild class column, `/who` output, labels, the item display's Level Needed line and
  extended target slots. A function that returns 0 (or NULL) leaves the stock answer.
- **switch rules** with `set_flag`: wide item links (8-digit ids), every item usable (only the server checks),
  Hide for every class, alternate abilities from level 1, the AA window's Train All, and class checks over the
  player's classes (the mask from `set_player_classes`, else the profile's class): an augment for a worn item
  (`augment_classes`), the research containers (`research_classes`) and focus effects (`focus_classes`: SPA 411,
  an AA's classes, and no Bard / pure melee focus filter), and Backstab with a piercer in either hand, not only the
  primary (`backstab_either_hand`, for a server whose backstab uses the better hand; a client older than the switch
  ignores it), and an adventure recruiter's Request from a player in no group or raid (`solo_adventure`, for a server
  that takes solo adventures; stock EQEmu drops such a request unanswered, so without the switch the client stops it
  with its own line; an older client ignores the switch).
- **tell the client what its server knows**:
  - `set_player_classes` and `set_character_classes`: multiclass masks, which the client shows as "War/Clr" in the
    window title, character select and the guild window;
  - `set_pet_list`: several pets, with their gauges and the focused pet;
  - `set_vitals`: the server's HP, mana and endurance in place of the client's formulas; the values given also
    become the player's current points until the next update changes them.

- **read the game state** (with `read`) through typed structs rather than `${...}` text, fast enough for the hooks:
  `get_player`, `get_spawn` and `next_spawn` (every spawn in the zone), `get_spell`, `get_item` (an inventory
  slot), `get_buff`, `get_xtarget`, `combat_ability` (the character's combat abilities), `skill` (a skill's value as
  the profile holds it), `aa_value` (an owned alternate ability's slot value, by its ability id), and `eqstr` for a
  game string with its `%1` filled. A spawn also carries its rider, mercenary and GM flags, whether it is one of the
  player's group, its con colour and its body types. Each struct starts with its size; the client fills what both know, so a plugin built
  for an older client keeps working. `server_info` gives the
  server this login is on (the saved server's name, host and port, the world's name and address), so a plugin for
  one server can tell it from another that sends the same packets.
- **take item clicks** (`item_right_click`, then the client's own with `right_click_item`, which needs `actions`)
  and fill the merchant list's level column (`list_level`).
- **act** (with `actions`): close the spell book, end the global spell recovery, show another of several pets in the
  pet window (`set_window_pet`, the client's choice only), open an item's display (`inspect_item`), open the
  bank window at any spawn or at the player itself, without a banker's class and range checks (`open_bank`), change the
  camera's zoom limit (`set_max_camera_distance`), and change the hot buttons: `hot_button` reads any button of
  any bar and page as the stock character ini's string (with `read`), `set_hot_button` puts one, `hot_bar_page`
  and `set_hot_bar_page` a bar's page; a change is saved with the character. Before the use of a skill
  `before_use_skill` may refuse it, and `command_line` may rewrite a typed or scripted line before the client runs
  it (hooks).
- **put spawns on the maps**: the game's maps (the map window, the minimap, the zone map) show only the group, as
  RoF2's map does. `map_spawn` is asked for every other spawn each time a map draws (the minimap: only those
  inside its circle, the only ones it could show) and shows the ones it answers for, with a colour, a map layer (the layer switches and the height filter apply) and a label shown as the map
  shows its labels; the client starts it from the con colour and the name. A Lua plugin's `map_spawn(spawn)`
  answers true for the defaults or a table with `color`, `layer` and `label`.
- **keep files** for the server and for the character: `server_config_dir` is
  `servers/<profile>/plugin-config/<name>/` (in the world too on a server without Hailstone's quest script),
  `character_config_dir` is the character's
  `characters/<world>/<character>/plugin-config/<name>/` (0 before there is one). `config_dir` stays the folder for
  every server. `write_chat_color` writes a line in one of the game's own chat colours, and `zone_name` gives a zone's
  short name.
- **read and write packets** with the SDK's `hailstone_byte_io.h` (C++20, header only): the `ByteReader` and `ByteWriter`
  the client itself uses.
- **connect** (with `net`) to a service the server runs beside the game, such as its card game's: see Connections
  below.

A plugin is active from its load. `set_active(0)` keeps its hooks, switches and vitals out until it has seen its
server (its own packets, for example), and `set_active(1)` brings them back. Several plugins may extend the client at
once: the client asks them in load order, and the first answer wins (`src/game/server_ext_chain.h` lists how each
kind of answer combines). `tests/plugins/ExtendTest.c` uses each part.

Compatibility: the host table and the hook table carry their size and only grow at their end. A plugin built for an
older ABI loads in a newer client; one built for a newer ABI does not load in an older client. ABI 3 added the web
pages (`web_*`, "Web pages" below); a plugin built with this SDK reports ABI 3, so a client older than that does not
load it. Calls added to the host table since then keep ABI 3: test `HAILSTONE_HOST_HAS(host, open_bank)` before such a
call, since a client older than the call has a shorter table. The structs a plugin passes to be filled (`HailstonePlayer`, `HailstoneSpawn`,
..., `HailstoneWebState`) carry their size too and the client fills what both know; `HailstoneWebState`'s `size` is a `size_t`
(as ABI 3 shipped it), the others' a `uint32_t`.

### Connections (ABI 2)

Some servers run a service of their own on a TCP port next to the game. A plugin with the `net` permission reaches it through the client's sockets, so it needs no socket code of its
own and works the same on every platform. Nothing waits on the network:

| Function | What |
|---|---|
| `tcp_open(address, port)` | starts connecting without waiting: a handle above 0, or 0 (no `net`, already `HAILSTONE_TCP_MAX` = 8 open). A name is looked up in the background (at most four lookups at once, the rest queued), the handle connecting meanwhile; one that does not resolve shows as `tcp_state` -1, `tcp_error` saying why. Each of a name's addresses is tried in turn (IPv4 first): one refused, or with no answer in 10 s, hands over to the next, and the last one's failure closes the handle |
| `tcp_state(handle)` | 1 connected, 0 still connecting (at most 10 s an address), -1 closed (refused, no answer, dropped, closed by the other side) |
| `tcp_send(handle, data, size)` | takes every byte (what the system cannot take yet waits in the client and goes out at the next `tcp_*` call) and returns the count, or -1 once closed |
| `tcp_recv(handle, out, size)` | what has arrived: the count, 0 for nothing yet, -1 once closed with nothing left |
| `tcp_error(handle, out, size)` | why it closed, as text |
| `tcp_close(handle)` | ends it; unloading a plugin closes its others |

Call `tcp_recv` each pulse; that also sends what was waiting. A plugin that grants `net` can reach any address, this
computer and the player's network too (a server run at home is often there): unlike a web page, a compiled plugin is
native code and could open such a connection without the client, so a refusal would hold nothing. A server's
`hailstone.cfg` grants it to the player's plugins only when it says so. When the plugin loses `net` (a narrower
policy, a lowered ceiling) its connections close, and each later `tcp_*` call on them sees one closed (-1).

### Plugin windows (ABI 2)

A plugin's windows are Lua files in its folder, one set for each UI pack, so they match whatever UI the player uses:

```
plugins/MyPlugin/
  MyPlugin.so
  ui/modern/mywindow.lua      the Modern pack's window
  ui/modern/lib/shared.lua    dofile("lib/shared.lua") from the plugin's own folder
  ui/gamepad/mywindow.lua     the Gamepad pack's
```

A relative `dofile` looks in the plugin's folder first, then in the pack's, so a plugin's window can use the pack's
own libraries (`dofile("lib/hud.lua")` in the Modern pack, `lib/theme.lua` in the Gamepad pack) and look like the
rest of the pack. `dofile` takes nothing outside those two folders (no absolute path, no `..` out of them), and every
Lua file in the UI, the pack's too, is loaded as text: precompiled chunks are refused (`load` as well), and there is
no `loadfile`.

A plugin's window with the name of one of the pack's (`ui.window("Pet", ...)`) covers the pack's instead of adding a
second: only the plugin's draws. The pack's comes back when the plugin unloads, and a reload of the pack's file does
not take the name back (with several plugins, the later-loaded covers the earlier). From its draw function
`ui.base_window()` draws the window it covers and returns true (false when it covers none), so a plugin can leave the
stock window in place while it has nothing to add, on another server for example:

```lua
local p = eq.Plugin("MyPlugin")
ui.window("Pet", function()
  local state = p and p.pets()       -- nil until the plugin has seen its server
  if not state then return ui.base_window() end
  ...                                -- the plugin's own Pet window
end)
```

To add to a pack's window without copying it, a pack can keep the window in a library that takes the plugin's
part. Both packs' Pet windows do (`ui/<pack>/lib/pet.lua`, each file's head lists its options): the Modern pack's
`pet.window{extra = {height, draw}, fold = <setting>, width = n}` adds a part above the command buttons and can fold
them; the Gamepad pack's gives the window's pieces (`P.visible`, `P.status`, `P.commands`, `P.buffs`) to lay out.
The Modern pack's extended target window does too (`ui/modern/lib/xtarget.lua`): `xtarget.window{width = n, row =
{width = n, draw = fn(r, x0, y0, x1, y1, s), style = fn(r)}}` keeps `row.width` pixels at the right of each filled row
for the plugin's part (its buttons take their own clicks; a click on the rest still targets), and `style` can fade a
row, outline it, strike its name through or put a small label after it. `r.n` is the slot (0-based:
`hailstone.xtarget(r.n + 1)` in a script). A window named "Extended Target" that calls it covers the pack's:

```lua
local ok, xtarget = pcall(dofile, "lib/xtarget.lua")   -- an older client has no lib/xtarget.lua
ui.window("Extended Target", function()
  local p = eq.Plugin("MyScript")
  if not ok or not p then return ui.base_window() end
  xtarget.window{ width = 250, row = { width = 50, draw = function(r, x0, y0, x1, y1, s) ... end } }
end)
```

The Modern pack's AA window is a library as well (`ui/modern/lib/aa.lua`): `aa.window{row = {width = n, draw =
fn(r, x0, y0, x1, y1, s), style = fn(r)}, details = fn(sel, x, y, w, s), banner = fn(x, y, w, s)}`. `row` works as
the extended target window's (`style` can outline a row or put a small label after its name); `r` has the ability's
`name`, `rank`, `max`, `cost` (the next rank's, as text), `category`, `trainable`, `maxed` and `page` (the
ability's tab, 0-based: the window lists every tab's abilities as one list). `details` draws a part under the selected ability's requirements (`sel`: `id`, `name`, `rank`, `max`,
`cost`) and `banner` a strip under the header; each returns the height it took. Rows carry no ability id: match `r.name`
to `hailstone.alt_abilities()`'s names (an ability's ranks share theirs). A window named "Alternate Advancement" that
calls it covers the pack's, as above.

They load with the player's pack while the plugin is loaded (it needs the `ui` capability) and go when it unloads. A
pack without a folder of its own shows none of the plugin's windows. They are written like the pack's own windows
(`ui.window`, the `ui` and `eq` tables). A server ships them in its `client_files/plugins/<name>/ui/` (Lua is
taken there and in `scripts/` only).

The plugin gives its windows data and takes their actions through functions they call:

```lua
local p = eq.Plugin("MyPlugin")        -- nil while it is not loaded
local rows = p and p.rows("open")      -- or p.call("rows", "open")
```

In the plugin, `add_ui_function("rows", fn, user)`: `fn` reads the arguments (`arg`, and `arg_field` for a table
argument's field) and builds the result: a value, or arrays and tables opened with `out_begin_array` /
`out_begin_table`, filled (in a table `out_key` before each value) and closed with `out_end`. A key written as a
whole number (`"12"`) is the Lua integer 12, so a table can be keyed by slot or id.

Values cross both ways as plain data: tables nested deeper than 16 levels are cut there (nil), and a value with a
table inside itself, more than 100 000 values or more than 16 MiB of text is nil as a whole, with a line in the log
(`plugin/lua_value.h`). A call passes at most 64 arguments (`UiBridge::kMaxCallArgs`): one with more gives nil and
does not reach the plugin.

It can also add:
- **options**: `add_option(name, label, min, max, get, set, user)`. A switch when max is 1, else a slider. The
  packs' Options windows list them under the plugin's name (Modern: a Plugins tab; Gamepad: a Plugins page;
  `eq.PluginOptions`, `eq.SetPluginOption`), and the plugin keeps the value itself.
- **menu entries**: `add_menu(label, command)`. The EQ menu (a Plugins submenu) and the gamepad pack's More page
  list them
  (`eq.PluginMenu`, `eq.PluginMenuClick`), and choosing one runs the command, usually a command of the plugin's that
  shows its window. The command is the plugin's, so it runs with the plugin's capabilities even though the player
  chose it: an entry whose command needs more than its plugin may do is refused like any command the plugin sends.

Each window folder belongs to an owner, told apart by kind as well as by name: `plugin:<name>` for a compiled
plugin's, `script:<name>` for a Lua plugin's (a script's `ui/`). A script named after a compiled plugin (a script
add-on called `nms` beside the NMS server's plugin) gets neither the plugin's capabilities nor its folder, functions,
options or menu entries; each owner's stay its own and go only with it. `eq.Plugin(name)` with a bare name finds a
compiled plugin's functions before a script's of that name; `eq.Plugin("script:<name>")` names the script's.
`eq.PluginOptions` and `eq.PluginMenu` give each entry's `owner` beside its `plugin` name, and `eq.SetPluginOption`
takes either. The add-on manager installs no script with the name of a compiled plugin in the player's folders, nor
the other way round (docs/addons.md).

These windows share the pack's Lua state but not its authority. A plugin folder's code (its files' top level, its
windows, and any function it took from `eq` or `ui`, whoever calls it later) runs as the plugin: with its
capabilities for whatever it does through the client (a command it sends with `eq.ChatSubmit`, a script it starts,
a web page), as `LuaUi::caller_tag` and a `plugin::CallerScope` say. A server's own plugins and scripts have every
capability (above), so their windows can do what the pack's do; a player's add-on gets at most what its manifest
declared (docs/addons.md). Whatever its capabilities, a plugin's code:

- has globals of its own: it reads the pack's globals (the shared state tables the pack's libraries keep, such as
  `modern_colors` or `GPSTATE`), but a global it sets is its own, and so is every global a pack library it runs with
  `dofile` sets;
- sees `eq`, `ui` and the standard libraries read-only, so it cannot change what the pack's windows call;
- reaches only its own owner's exported functions, options and menu entries: `eq.Plugin` gives nil for another
  plugin (and an object for another that it finds elsewhere does nothing), `eq.PluginOptions` lists only its own,
  and `eq.SetPluginOption` and `eq.PluginMenuClick` leave another's alone, since those run the other plugin's code
  with the other's capabilities. The pack's own windows reach every plugin's;
- keeps settings of its own: `ui.SetSetting(key, v)` writes `plugin.<owner>.<key>` in the pack's settings file (a
  window's place under `hud.` is the character's, as the pack's are), `ui.GetSetting(key)` reads that, else the
  pack's own key of that name (the palette, the place of a window it took over), and neither reaches the pack's test
  hooks (`test.*`, which click windows' children), the windows' switches (`enabled.*`) or another plugin's keys
  (`ui.SetSetting` refuses, and returns false for, a key with `=` or a line break or a value with a line break, since
  the file is one `key=value` a line and such text would be read back as other keys);
  `ui.DisableWindow(name)` turns off only its own window of that name;
- runs for at most a second at a time (a window's draw, its file's load, a function of its that the pack calls) and
  the pack's own code for five: code that runs longer is stopped with an error, which a `pcall` of its own cannot
  catch, and its window is turned off until its file changes; the UI's Lua state as a whole holds at most 1 GiB, and
  an allocation past that fails in the code that asked for it. `collectgarbage` takes only `"count"` and `"step"`;
- uses the player's clipboard (`ui.GetClipboardText`, `ui.SetClipboardText`) only when its owner has every
  capability, as for a script: it holds whatever the player copied, a password too;
- gets nil from the pre-game screens (`eq.LoginScreen` and the others hold the typed or saved password) and cannot
  act on them, may not run, stop or pause scripts or change the autorun list (`eq.ScriptRun` and the others), and
  opens web pages as its owner (`plugin:<name>`, or `script:<name>`: a Lua plugin's windows share the script's
  `mq.web` tabs) with its own permissions.

What plugin windows may do: reading the game and drawing need no capability beyond `ui`; an `eq` function that acts
needs the plugin's capability for it, from the plugin's own code or from a pack function it calls (the list is
`app/lua_eq_caps.cpp`, and a function missing from it is taken to act). Refused, it does nothing and returns nil, with
a line in the log once.

| Needs | `eq` functions |
|---|---|
| nothing | everything that reads or draws (`eq.Item`, `eq.AllItems`, `eq.ItemStats`, `eq.ItemKept`, `eq.ChildState`, `eq.ListRow`, `eq.SpellBook`, `eq.DrawMap`, ...), a window's place and size (`eq.ReplaceWindow`, `eq.WindowPlaced`, `eq.SizeWindow`), the map's zoom and pan, opening a menu (`eq.WindowContextMenu`, `eq.ChatMenu`, `eq.HotBarMenu`; choosing a row is below), the close-top-window key's press (`eq.TakeCloseTopWindow`) |
| `commands` | `eq.ShowWindow`, `eq.SetOption`, `eq.SetUiIni`, `eq.ShowSpell`, the chat windows' setup (`eq.ChatNewWindow`, `eq.ChatSetFilter`, ...), the spell sets' and the autoskill list's editing, the map's find path, the Find window (`eq.FindOpen`) and its guide path (`eq.FindNpcPath`), the social editors, `eq.gamepad.set_*` |
| `target` | `eq.FindNpcTarget` (the Find window's row) |
| `move` | `eq.FindNpcWalk` (the Find window's Walk there, a `/nav` walk) |
| `actions` | a stock window's controls (`eq.ClickChild`, `eq.ClickChildAt`, `eq.WindowButton`, `eq.StmlLink`, `eq.SetWindowText`, `eq.ListClick`, `eq.ListHeaderClick`, `eq.ComboSelect`, `eq.SetSlider`, `eq.SetTabPage`), the menus' rows (`eq.CascadeClick`, `eq.GameMenuClick`), items and the cursor (`eq.ClickSlot`, `eq.BagSlotClick`, `eq.BankSlotClick`, `eq.PlaceClick`, `eq.PickUp`, `eq.SortItems`, `eq.SellJunk`, `eq.BulkItems` (sell, tribute or destroy chosen items), `eq.ItemJob`, `eq.SetItemKept` (a Keep mark taken off lets the item be destroyed), the parcels, `eq.gamepad.drop_cursor`), hot buttons and gems (`eq.ClickHotButton`, `eq.PressButtonFrom`, `eq.ClearCursorButton`, `eq.SpellGemClick`), spells and abilities (`eq.SpellBookClick`, `eq.SpellBookMemorize`, `eq.SpellBookSwap`, `eq.MemorizeSpellSet`, `eq.UseSkill`, `eq.AbilityBookUse`, `eq.AbilityBookAutoskillEnable`), socials (`eq.PressLuaSocial`, `eq.PressSocial`, `eq.StopSocial`, `eq.SocialToCursor`; `eq.SetLuaSocial` and `eq.LuaSocialDryRun`, which runs the code it is given with the server's Lua social capabilities, need `commands` too) |
| checked by the function | the plugins' (above), the scripts' (refused), the pre-game screens' (nil), the web's (`web`); `eq.ChatSubmit`'s line goes through the command filter, which holds it to the plugin's capabilities as any command it sends (plain text needs `chat`, `/cast` needs `actions`) |

What stays shared: a plugin can still write into the tables the pack keeps in its globals (that is how its windows
take part in the pack's focus and palette), so a plugin can upset how the pack's windows look or behave. It sees
each of those tables through a view: reads and writes go through to the table, but a function the plugin writes there
(or into a table of its own that it puts there) runs as the plugin whenever the pack calls it, does nothing once the
plugin unloads, and cannot be called by another plugin's code; the view's metatable cannot be read or changed,
`rawset` refuses it, and a plugin's tables cannot have a `__gc` finalizer. `next` and `rawget` see nothing through a
view (`pairs`, `ipairs`, `#` and `table.*` work). It cannot get the pack's own `eq` or `ui` functions to do
something under the player's authority. What is not covered: a function or table the plugin hands a pack function
as an argument, which that function may keep as it is (docs/todo.md). A player who copies the same files into a UI
pack makes them the pack's own, with the player's authority.

How plugins load:
- The client hashes the file, checks the server's rules, writes **a copy of exactly the hashed bytes** to
  `cache/plugins/` and loads the copy, so the file cannot be swapped between the check and the load.
- Callbacks the plugin's capabilities do not cover are left unhooked, with a note in the chat window.
- When the rules no longer allow a plugin, `ShutdownPlugin` runs and the plugin is unloaded.

**In this repository:** `plugins/example/` (MQ2Example, plain C). The NMS server's client is a plugin in its own
repository (projecthailstone/hailstone-nms: C++, the SDK alone; its docs/nms.md), released as an add-on, and so is
Project Triune's (private). Either, cloned into `plugins/<name>/`, is built with the client and its tests join
`hailstone_tests` (docs/layout.md "Builds and installs"). The client itself has no server's code. Any other plugin or
script lives in a repository of its own too and is installed with the add-on manager (docs/addons.md "For authors").

**Server admins:** a compiled plugin is native code. The capabilities limit what the client's host table lets it
do, but a plugin can still call the operating system directly, and a server's plugins load on every machine whose
player allowed them ("The server's compiled plugins ask first" above). Ship only plugins whose code you trust. Scripts are different: everything they can do goes
through the client, so their capabilities hold. All of this is enforced by the client: a modified client can ignore
`hailstone.cfg`, so the server's own checks stay the real line.

## Web pages

Scripts and plugins with the `web` permission open tabs of their own in the in-game browser (docs/browser.md):
http and https pages on the hosts the server's `[web] allow` names (any host without it). The helper checks every
navigation of such a tab, links and redirects too, and a tab that lands elsewhere is closed; the page's frames and
requests (images, scripts, `fetch`) are held to the same hosts. No such tab, `/url` or `/www` reaches this computer
or a private network (`localhost`, `192.168.x.x`, `10.x.x.x` and the like), even when `[web] allow` names it. Each
script and plugin has a cookie store of its own, apart from the player's and from each other's. A script or plugin sees
and changes only its own tabs; the player's are not theirs, and theirs are not in the player's browser window. They
close when it stops (a script whose main chunk has returned is stopped: keep it running, as for a window) or unloads.
Without the helper (macOS, or not installed) `open` gives nil and why. A script or plugin holds at most 16 tabs at
once (`WebService::kMaxTabsPerOwner`), those the helper or the rules closed included until it closes them: a 17th
`open` gives nil and why. An address is at most 2 MB, a `post` to the page at most 1 MB, and an `eval` or `text` at
most what one message to the helper holds (64 MB): a longer one is refused (false; `navigate` with why) and nothing
is sent. While 32 MB still wait for the helper to read them, `open`, `navigate`, `eval`, `text` and `post` are refused
the same way; try again a frame or so later. An empty queue takes the largest message.

The page and its owner talk through `window.hailstone`: the page's `hailstone.post(text)` is queued for the owner
(`mq.web.messages(tab)`, `eq.WebMessages(tab)`, `web_message`; a message is at most 1 MB, and at most 256 and 8 MB
wait, the oldest going first; `web_message` with a buffer takes the message off the queue even when it is cut, and
with `out` NULL gives only its length and leaves it queued, so a plugin can make room for a long one first), and the
owner's `post` calls the page's `hailstone.onmessage(text)` when it set one. A tab is closed (Refused, with why)
when its owner loses `web` or the server's `[web] allow` changes (docs/browser.md "Rules that change"). A page cannot run commands or read the game itself; what a
message does is up to its owner, within the owner's own permissions.

| | Lua scripts and Lua plugins | A plugin's windows (UI pack) | Compiled plugins (ABI 3) |
|---|---|---|---|
| open | `mq.web.open(url [, w, h])` | `eq.WebOpen(url [, w, h])` | `web_open(host, url, w, h)` |
| go to an address | `mq.web.navigate(tab, url)` | `eq.WebNavigate` | `web_navigate` |
| back, forward, reload, stop | `mq.web.nav(tab, what)` | `eq.WebNav` | `web_nav` |
| run script in the page | `mq.web.eval(tab, js)` | `eq.WebEval` | `web_eval` |
| to the page / from the page | `mq.web.post`, `mq.web.messages` | `eq.WebPost`, `eq.WebMessages` | `web_post`, `web_message` |
| type at the page's cursor | `mq.web.text` | `eq.WebText` | `web_text` |
| state, tabs | `mq.web.state`, `mq.web.tabs` | `eq.WebState`, `eq.WebTabs` | `web_state` |
| draw it (mouse and keyboard) | `mq.web.view(tab, w, h)` in `mq.imgui` | `eq.WebView(tab, w, h)` | `web_view` in `OnUpdateImGui` (needs `ui` too) |
| close | `mq.web.close` | `eq.WebClose` | `web_close` |

A refusal in Lua (`mq.web` without `web`) is an error, as for `mq.imgui`; `eq.Web*` in a plugin's window give nil
or false; the host table's functions return 0 and say why in the chat window once. A plugin's window acts with its
plugin's permissions (a compiled plugin's while it is loaded, a Lua plugin's while its script runs; never one for
the other); the player's own windows act as the player. The example plugin's `/webexample` (plugins/example/MQ2Example.c) shows the compiled side.

## The server's rules: `hailstone.cfg`

An ini file at the top of the server's `client_files/` (`plugin::parse_server_cfg`). What it leaves out keeps the
default; `;` and `#` start comments. A section that is not one of the client's own below is kept for scripts to read
(`hailstone.server_rule`), so a server can set a script's limits, for example `[autocombat] hunter = off`.

```ini
[scripts]
player = off              ; the player's own scripts: on / off (default off)
player_caps = read,ui     ; what they may do when on (default all)
run = hud, autoloot       ; the server's scripts started at each zone-in (/lua run)

[plugins]
player = off              ; the player's own compiled plugins (default off)
player_caps = read        ; what they may do when on (default all)

[assets]
player = on               ; the player's own files in servers/<profile>/assets/ (default on). Off: only the
                          ; server's files there count, and textures (.dds .bmp .tga .png .jpg); the asset folder
                          ; and texture packs are never touched

[socials]
mode = press              ; Lua socials: off / press / full (default press)
caps = read,target,actions ; what they may do (default read,target,actions)
press_commands = 5        ; press mode: the most commands one press sends (0 no limit; default 5)
faces = on                ; button faces (default on)

[abilities]
autoskill = on            ; the Ability Book's autoskill (default on). Off: its switch is greyed out and a
                          ; running autoskill stops

[movement]
npc_collision = on        ; live NPCs stop the player, who slides along them (default on, with or without this
                          ; file). Off: everyone walks through everyone, as in the original client
passable_ladders = on     ; walking or swimming into a ladder whose polygons are all passable (the Cabilis canal
                          ; ladders) climbs it (default on). Off: only a click from the top mounts one, as in
                          ; the original client
solid_ladders = on        ; the player's movement meets a ladder as one solid slab, not its rails and rungs, so
                          ; every race climbs it (default on). Off: the rungs, as in the original client, where a
                          ; small race slips between them and any race can stick partway up

[web]
allow = example.org       ; the hosts scripts and plugins may open, and those below them (default: any;
                          ; docs/browser.md). The player's own /url and the server's OP_Weblink are not limited

[addons]
suggest = owner/repo      ; add-ons the login screen suggests to players who do not have them (GitHub links, ","
                          ; between; docs/addons.md "Suggested add-ons"). They come with this file, so at the
                          ; first zone-in: Hailstone's add-on index is how a first-time player hears of one
```

Autoskill (`game/ability_book.h`) is the client's own helper: it uses the abilities on the player's list in order
as they come ready, one at a time with a gap of 0.6 s, through the same hot button path as a key press. By default an
ability is used only while the player fights (auto attack on an NPC in melee range); a beneficial one (a beneficial
spell, or a skill on the player alone: Forage, Mend, Sense Heading, Sense Traps, Fishing, Hide, Sneak) may be set to
any time or to out of combat (auto attack off and the server's OP_RestState combat flag clear), and one that leaves a
buff can wait until that buff is off the player. A harmful ability keeps to fights whatever its rule says, so
autoskill never starts one. Each entry also has endurance, mana, own health and target health conditions. Its on/off switch is kept
per character (hotbuttons.ini `[AbilityBook] AutoskillOn`), so it is as the player left it at the next login; a
server that forbids autoskill turns it off without changing that. It is not a plugin or script, so the capabilities above do
not apply to it; `[abilities] autoskill` is its switch.

NPC collision (`game/npc_collision.h`, docs/mechanics.md "NPC collision") is Hailstone's own rule, not the original
client's: a live NPC is a wall the player cannot walk through, while players, pets, mercenaries, corpses and mounts stay
passable. It is on unless the server's file says `[movement] npc_collision = off`; the player has no switch for it.
Climbing passable ladders (docs/mechanics.md "Ladders") is Hailstone's own rule too: in the original the sphere goes
through a ladder whose polygons are all passable, such as the Cabilis canal ladders, and never climbs it. It is on
unless the server's file says `[movement] passable_ladders = off`. Solid ladders are the same: the player's movement
meets a ladder as one slab instead of its rungs (docs/mechanics.md "Ladders") unless the file says
`[movement] solid_ladders = off`.
Players get a change at their next login, with the rest of the file.

A server folder for every platform its players use:

```
client_files/
  hailstone.cfg
  spells_us.txt, dbstr_us.txt, Resources/SkillCaps.txt ...   (EQEmu's export_client_files)
  assets/            zones, models, textures (the same as files at the top: both go to servers/<profile>/assets/)
  scripts/           hud.lua, autoloot/init.lua ...
  plugins/
    windows-x64/MQ2Hud/MQ2Hud.dll
    linux-x64/MQ2Hud/MQ2Hud.so
    macos-arm64/MQ2Hud/MQ2Hud.dylib
```

`hailstone_check <client_files>` (built with the client) prints what the cfg says, warns about what it does not
understand, and lists where each file goes and which a client refuses (programs outside `scripts/` and `plugins/`).

Changes reach each player at their next login (the files come at the first zone-in of a login). Until then the
last login's rules apply, provisionally (only `login` runs; "Before the zone-in" above). The client keeps the
zone-in's rules while the character is in the world, goes back to the provisional ones at character select, and
drops them on disconnect. Granting `login` in `player_caps` lets the player's own plugins start at the world login;
leaving it out holds them to the zone-in.

## The server's settings (protocol)

Two custom opcodes that EQEmu's `patch_RoF2.conf` does not list (`src/net/settings_packets.h`); the server's Lua
script handles them by their wire values. Both are little-endian:

- Settings request **0x1432**, client → zone, after every zone-in: `{u32 magic; u16 version 1}`
- Settings **0x1433**, zone → client: `{u32 magic; u16 version 1; u32 flags}`. No flag is defined now
  (bit 0 hid the world's player count on the server list, which no UI shows any more). Later fields go after these
  and are ignored by clients that do not know them.

The magic value is in `src/net/settings_packets.h`.

## Setting up a server

Any EQEmu server can send its files with the Lua quest script in `server/` (no C++ changes or rebuild):
`python3 server/install.py <server dir>`, then restart the zones. Put `hailstone.cfg`, `scripts/` and `plugins/` in
`client_files/` beside `eqemu_config.json`, and check the folder with `hailstone_check`. `server/README.md` has the
rest.
