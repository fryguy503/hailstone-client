# Plugins, MacroQuest Lua scripts and the server's rules

UI authors should use the shared [UI API](ui-api.md) (`hs.UI`); the flat `hs.*` bindings below are the same guarded
calls.

Three ways to extend the client share one hook bus (`src/plugin/hooks.h`): world and zone packets both ways, chat
lines, typed commands, game state, zoning, spawns and each frame.

| What | Where | Who decides |
|---|---|---|
| **Lua windows**: the game's windows, rewritten in Lua (`ui/<pack>/*.lua`, `src/ui/lua`) | ships with the client | always on |
| **MacroQuest Lua scripts**: automation with MQ's `mq` module, `/lua run <name>` | the server's `scripts/`, then `scripts/` (then the install's) | the server's `hailstone.cfg` |
| **Compiled plugins**: shared libraries with MQ-style callbacks (`sdk/hailstone_plugin.h`) | the server's `plugins/`, then `plugins/` or `plugins/<name>/` (then the install's) | the server's `hailstone.cfg` |

Players install them with the add-on manager on the login screen (docs/addons.md), or by hand. An add-on gets no
more than the capabilities its manifest declares. docs/scripting-reference.md lists every TLO, member, function, hook
and struct; this page gives the rules.

#### Where plugins and scripts keep their files

The folders are in the Hailstone folder (`docs/layout.md`).

- A compiled plugin's binary sits in `plugins/` or its own `plugins/<name>/`. `host->config_dir` points at that
  folder, so the plugin keeps its settings beside its binary.
- A script that may do everything keeps its `mq.pickle` files in `scripts/config/` (`mq.configDir`).
- One that may not (an add-on with a ceiling, the player's scripts under a server's `player_caps`) gets
  `scripts/config/<script>/` and reads and writes only there, so it cannot touch a more trusted script's settings.
- `mq.configDir .. '/MyScript.lua'`, the usual MQ path, works either way.

The folder is the script's name, with `%XX` for `%`, control characters, non-ASCII bytes, `/ \ : * ? " < > |`, a
last `.` or space, and the first letter of a device name (`con`, `nul`, ...). A folder named the 0.6 way (other bytes as `_`) is
moved to the new name at the script's start, unless two scripts shared it.

#### The server decides, through a file

A server with Hailstone's script (`server/`) sends its `client_files/` at the first zone-in of a login
(docs/layout.md "Server files"):

- `hailstone.cfg`: the server's rules (below), into `servers/<profile>/hailstone.cfg`.
- `scripts/`: the server's own Lua scripts, into `servers/<profile>/scripts/`.
- `plugins/`: the server's own compiled plugins, into `servers/<profile>/plugins/`. Only this platform's libraries
  come (`.dll` on Windows, `.so` on Linux, `.dylib` on macOS). The server may sort them into platform folders under
  `client_files/plugins/` (`linux-x64/`, `windows-x64/`, `macos-arm64/` ...); only this build's comes, with the
  folder taken out of the path.

| The server | The player's own scripts and plugins | The server's own |
|---|---|---|
| has no `hailstone.cfg` (plain EQEmu, or the script without one) | run, with every capability | run, with every capability |
| has a `hailstone.cfg` | run only when it allows them, with the capabilities it grants | always run, with every capability; scripts start by themselves, and the player cannot stop them or unload its plugins. Its compiled plugins load only once the player allowed them (below) |

A server can also require, allow or block the player's add-ons by repository ("Opt-in add-on policies" below);
without `[addons] enforce = on`, the table is all there is.

A file is the server's when its SHA-256 is one the server listed this login; editing one makes it the player's.

#### Before the zone-in

From the world login until the first zone-in (character select included), the client uses the rules the last login
left in `servers/<profile>/` (its `hailstone.cfg` and the server's files with their hashes).

- Under those rules only scripts and plugins with the `login` capability run, so a plugin can act at the world login
  (checksums) and at character select (the characters' classes), as a server's own client does.
- A server with no rules known yet (never visited, or without Hailstone's script) grants every capability.
- The zone-in's rules then replace these: what they no longer allow unloads, and plugins without `login` load.
- Lua socials run only from the zone-in. Disconnected, nothing runs.

On a first visit the server's plugins start at the first zone-in; a copy in the player's `plugins/` can run from the
world login.

### The server's compiled plugins ask first

A compiled plugin is native code. Once loaded it can do anything the player's account can on the computer; its
capabilities limit only what it does through Hailstone. So a library in
`servers/<profile>/plugins/` loads only after the player approved its bytes, on every server. The server's Lua scripts
and Lua windows are not asked about. A compiled plugin an opted-in policy requires ("Opt-in add-on policies") uses the
same per-server, per-hash approval:

| Asked about | Title | Text |
|---|---|---|
| the player's own add-on the server requires | "Allow your add-on this server requires?" | the server requires that plugin from the player's add-ons; a warning to allow it only when the add-on's author is trusted |
| the server's own plugins | "Allow this server's plugin?" | a warning about trust in the server |
| both at once | "Allow these plugins?" | |

The words are Hailstone's own; the original has no plugins.

- **The question.** At zone-in, login for a `login` plugin, or a manual `/plugin <name> load`, a plugin with no answer
  for its SHA-256 on this server stays unloaded. A window shows the server and each plugin's name, file, size and
  SHA-256 prefix. It explains that a compiled plugin runs with full access to the computer. Plugins found together
  are asked about together. In-world play and networking continue while the question waits.
- **Before entering the world.** A pending question for a native `login` plugin holds Enter World and Return Home
  at character select. Buttons, keyboard, gamepad and automatic entry use the same check. Automatic entry remembers
  the chosen character while it waits. Allowing, refusing or closing the question lets entry proceed. Optional
  plugins stay unloaded when refused or dismissed. Zone transfers continue without this character-select wait.
- **Allow** loads them at once, from a copy of the hashed bytes, and keeps the yes. Allowed mid-zone, a server
  extension may need the next zone-in to work.
- **Don't allow** keeps them unloaded and keeps the no, so later zone-ins do not ask again.
  - `/plugin <name> load` typed by the player asks again.
  - The same command from a script or plugin (`mq.cmd`, `DoCommand`) never puts the question up, at any point.
  - The server's features that need it are missing, as when a plugin fails to load.
- **The close box** (Gamepad UI: B) puts the question away unanswered: the next zone-in asks again.
- **Other bytes ask again.** Answers are per SHA-256, not file name: an updated or edited plugin asks again. A
  plugin's new answer replaces those for its older versions.

#### Nothing covers the question

While the question is up its window is raised above all others each frame. No plugin's window or script's ImGui can:

- draw over all windows (`ui.Foreground` is refused them);
- find where the question is (`ui.FindWindowRect`);
- draw into it: `ui.Begin` on the question's window (or the client's prompt's name, even before its first frame)
  returns not visible and begins nothing, `ui.BeginChild` inside it likewise, and `ui.End` cannot close it.

The host cleans up open draw scopes and unused next-window settings when a draw function returns. Only the pack's
own code can replace the client's prompt
(`hs.WindowShown("PluginConsent")` from a plugin's code does not). Plugin code neither sees nor answers the question;
an already loaded compiled plugin is not held by this.

#### Changing an answer

- The login screen's add-on list (docs/addons.md): **Don't allow it any more** for one allowed, **Ask me again** for
  one refused.
- The developer window's **Plugins & Lua**: **Forget my answer**, at once (an allowed plugin unloads and asks again).
- Each acts on the plugin's name and on every file the server now lists under it. A refusal carries over when the
  server sends the same bytes under a different name. The list reports completion once all the answers are saved.

#### Where the answers are kept

`servers/<profile>/plugin_approvals.txt` (docs/layout.md), one line a plugin: `allow <sha256> <name>` or
`deny <sha256> <name>`.

- The server cannot write it (its files land only in `hailstone.cfg`, `scripts/`, `plugins/` and `assets/`), and no
  setting skips the question.
- Unwritable, the answer holds for this run only. A box set's windows share the file (within a second).

#### How the packs draw it

The packs (`ui/modern/pluginconsent.lua`, the Gamepad UI's `dialogs.lua`) use two calls:

- `hs.PluginConsent()`: nil, or `{server, serial, title, intro, warning, plugins = {{name, file, size, sha256, short,
  required}}}`. Title, intro and warning are worded for what is asked; `required` is true for the player's own add-on
  the server requires.
- `hs.AnswerPluginConsent(true | false | nil, serial)`. An answer to a question since replaced (another server's,
  other bytes) is refused (false) and nothing is kept.

Both packs keep Allow and Don't allow off for a second after a question shows or changes. Plugin code gets nil.
Without a pack window (`hs.ReplaceWindow("PluginConsent")`) the client draws its own.

## For players

- `/lua run <script> [args...]`, `/lua stop [script]`, `/lua pause [script]`, `/lua list`.
  - From a script or plugin (`mq.cmd`, `DoCommand`, `hs.ChatSubmit`), `/lua stop` and `/lua pause` reach only a
    script that may do no more than the caller ("may not stop or pause" otherwise; with no name, the others are left
    alone). An add-on cannot end yours.
- `/plugin list`, `/plugin <name> [load|unload]`
- **Add-ons** on the login screen installs, updates and removes scripts and plugins (docs/addons.md); the server's
  own are listed but cannot be removed.
- Ctrl+Shift+D opens the developer windows, including **Plugins & Lua**.
  - It shows the server's rules, every script and plugin with its SHA-256 and owner, and run, stop, load and unload.
  - A plugin named like one in the server's `plugins/` is shadowed by the server's on that server.
- The EQ menu's **Lua Scripts** row (Gamepad UI: More > Lua Scripts) opens "The Scripts window" (below).
- `hailstone.ini`: `scripts.autoload` lists your scripts to start, when the rules allow, for every character (empty by
  default; a Lua plugin is usually one). The Scripts window's Autorun is the same for one character.
- `hailstone.ini`: `plugins.autoload = *` (the default) loads every allowed plugin of yours at the zone-in; a
  comma-separated list loads only those, an empty value none. The server's own load regardless, once you allowed them.
- A refused action prints `[MQ] myscript: /cast needs the server's 'actions' permission`. Your own typing is never
  limited.

## Capabilities

Each running file carries a set of capability bits:

- every one for the server's files, and on a server without a `hailstone.cfg`;
- for the player's under a `hailstone.cfg`, those its `player_caps` grant;
- for a file of an add-on the manager installed, no more than its manifest's `caps`, whatever the server allows
  (docs/addons.md "Capabilities"), matched by the file's hash.

When the bits drop (rules arriving at the zone-in, a lowered ceiling), a loaded compiled plugin stops getting what
they covered at once (`OnPacket`, `OnIncomingChat`, `OnFilterChat`, spawn events, `command_line`), and its web tabs
close.

| Bit | Name | Lets a script or plugin |
|---|---|---|
| 1 | `read` | read game state: `mq.TLO`, `${...}`, `mq.parse`, `ParseMacroData`, spawn events, `ability_state`, `get_ability_data_bulk`, `get_spells_data_bulk` |
| 2 | `commands` | run commands outside the groups below: windows, options, `/who`, other plugins' commands, `/lua`; add commands of its own (`mq.bind`, `AddCommand`). The player is told "run other commands (windows, options, /who, other plugins' commands)" |
| 4 | `target` | change the target: `/target`, `/assist`, `/xtarget`, ... |
| 8 | `actions` | act: `/cast`, `/disc`, `/doability`, `/useitem`, `/attack`, `/memspellset`, `/pet`, `/loot`, `/autoinventory`, `/alternateadv` (`/alt`), `/split`, `/consent`, `/guilddelete`, non-movement `/keypress`; Hailstone's own item commands `/destroy`, `/destroycursor` and `/stowcursor`, and `use_ability` |
| 16 | `move` | move: `/face`, movement `/keypress` (forward, jump, ...), `/follow`, `/sit`, `/stand`, `/duck`, `/camp` |
| 32 | `chat` | speak: plain text, `/say`, `/tell`, `/g`, `/gu`, `/rs`, channels, emotes |
| 64 | `events` | see chat lines: `mq.event`, `OnIncomingChat`, and hide them (`OnFilterChat`) |
| 128 | `ui` | draw windows: `mq.imgui.init`, `OnUpdateImGui` |
| 256 | `packets` | see, change, drop and send raw packets (compiled plugins: `OnPacket`, `send_packet`) |
| 512 | `extend` | change the client's rules as a server's own client does (compiled plugins: the server extension below) |
| 1024 | `login` | run before the zone-in: from the world login and at character select (above) |
| 2048 | `net` | open the plugin's own TCP connections (compiled plugins: `tcp_open`, below), for a service the server runs beside the game |
| 4096 | `web` | web pages: `/url`, `/www`, tabs of its own in the in-game browser (`mq.web`, `hs.Web*` in its windows, `web_*`); http and https only, on the hosts `[web] allow` names ("Web pages" below) |

Every command (`mq.cmd`, `/docommand`, `/squelch`, a bind, `DoCommand`) is checked against the running code's
capabilities (`src/mq/commands.cpp`).

- The game's own commands are classified by their entry in the command table.
- Hailstone's own commands need `commands`, except those that destroy or move items (`/destroy`, `/destroycursor`,
  `/stowcursor`), which need `actions`.
- MQ's and plugins' commands declare what they need.

## MacroQuest Lua scripts

Only Lua scripts are supported, not MQ's `.mac` macro language.

A script is `scripts/<name>.lua` or `scripts/<name>/init.lua`, as in MacroQuest's `lua/` folder. Each gets its own
Lua 5.5 state and runs as a coroutine resumed once a frame; `mq.delay` yields to the next frame.

#### Time and memory limits

- **The pause.** A script that runs more than 3 ms without yielding is paused until the next frame, like MQ's turbo.
- **The stop.** Code that cannot pause for 5 s is stopped: an `mq.imgui` function, a backtracking string match, or a
  script's own coroutine (`coroutine.create`, `coroutine.wrap`), which the pause never yields.
- **Catching the stop** fails: it re-raises on every instruction and every `pcall`, `xpcall`, `coroutine.resume` or
  `coroutine.close`, until the client next enters the script.
- **Nested binds.** A bind whose commands start binds of the same script again
  (`mq.bind('/x', function() mq.cmd('/x') end)`) ends the script once they nest 16 deep.
- **Command size.** A command of more than 1000 words for a bind is refused (a chat line), and the script runs on. An
  `mq.cmd` line is at most 2047 bytes (MQ's command buffer; longer is an error). `/docommand`, `/noparse` and
  `/squelch` nest at most 16 deep in one line (the rest is dropped, with a chat line).
- **Memory.** 256 MB a script; past it, "not enough memory".
- **No finalizers.** `setmetatable` refuses `__gc` (finalizers cannot be stopped); an `mq.TLO` value's metatable is
  locked (`getmetatable` gives `"mq.TLO"`).

### The `mq` module

| | |
|---|---|
| `mq.TLO.<Name>...` | MQ's TLOs (below). Call with no arguments for the plain value (`mq.TLO.Me.PctHPs()`, `nil` for NULL). Call with arguments for an indexed member (`mq.TLO.Me.Buff(1).Duration.TotalSeconds()`, `mq.TLO.Spawn('npc radius 50').ID()`). Values also work with `tostring`, `..`, `==`, `<` and `<=`. |
| `mq.cmd(line)`, `mq.cmd.<name>(args...)`, `mq.cmdf(fmt, ...)` | a command line, with `${...}` parsed |
| `mq.delay(ms \| '2s' \| '500ms' \| '1m' [, condition])` | wait, until the time passes or `condition()` is true. A wait beyond 9e15 ms (`math.huge`) is cut to that; NaN is an error |
| `mq.event(name, pattern, fn [, {keepLinks = true}])`, `mq.unevent`, `mq.doevents([name])`, `mq.flushevents([name])` | chat events, matched as MacroQuest's Lua events match them; see "Chat events" below |
| `mq.bind('/cmd', fn)`, `mq.unbind` | a command (needs `commands`); `fn` gets the words and may `mq.delay`; see "Binds" below |
| `mq.imgui.init(name, fn)`, `mq.imgui.destroy`, `mq.imgui.exists` | a window drawn each frame with `ImGui`; the same name replaces its function. At most 64 windows a script (an error past them) |
| `mq.web.open(url [, w, h])`, `.navigate`, `.nav`, `.close`, `.eval`, `.post`, `.messages`, `.text`, `.state`, `.tabs`, `.view(tab, w, h)`, `.available` | the script's own tabs of the in-game browser ("Web pages" below); `view` in an `mq.imgui` window |
| `mq.parse(text)`, `mq.gettime()`, `mq.exit()` | |
| `mq.getAllSpawns()`, `mq.getFilteredSpawns(fn)` | spawn values |
| `mq.pickle(path, table)`, `dofile(path)` / `loadfile(path)` | settings files under `mq.configDir`; see "Settings files" below |
| `mq.luaDir`, `mq.configDir` | |
| `print(...)`, `printf(fmt, ...)` | the chat window |
| `require('mq')`, `require('ImGui')`, `require('mymodule')` | modules next to the script or in the script folders; see "Modules" below |
| `ImGui.*`, `ImGuiWindowFlags`, `ImGuiCond`, `ImGuiCol`, ..., `ImVec2`, `ImVec4` | MQ's ImGui names and argument orders over the client's `ui` functions |
| `ImGui.GetClipboardText()`, `ImGui.SetClipboardText(text)` (also `ui.*`) | the player's clipboard; see "The clipboard" below |

#### Chat events

Events match as MacroQuest's Lua events (its Blech) do (`mq/event_pattern.h`):

- Case of A-Z is ignored. Line breaks are ordinary text.
- A literal before the first `#...#` must start the line; a pattern without any must be the whole line.
- Each `#...#` (`#*#`, `#1#`, `#2#`, ..., or any other name, such as `#who#`) takes text up to the first place the
  literal after it is found. The last one takes the rest of the line, its following literal found first at the line's
  end. So `#*# says #1#` on `A says B says C` gives `B says C`.
- `fn(line, arg1, arg2, ...)` gets each `#n#`'s text by number, `""` for a number the pattern lacks, up to the highest
  `#n#` (at most 255). A larger one, `#*#` and other names match without being passed on.
- `|text|` matches what the script's `${...}` in it reads when the line comes (with `read`; without it, the text as
  written), never parsed again. `##` and `||` stand for `#` and `|`.
- Item links are shown as their text, unless the event has `keepLinks`. Lines of 2048 bytes or more and empty lines
  raise no events.
- Each pattern is matched on its own, where MQ's shared pattern tree can turn down a line (a deliberate difference).

Limits: at most 256 events a script, a pattern at most 1024 bytes and a name 256, a script's patterns together at most
32 KB (an error past them). Matching runs outside the script's time slice.

A line another script or plugin wrote (`print`, `/echo`, `WriteChatf`) raises events too, but the handler then runs as
its own thread (as a bind) held to both scripts' capabilities; `doevents` does not wait for it.

A script whose `events` permission the server takes away gets no more lines: those it had queued are dropped, and
`mq.doevents` runs nothing (no error) until the permission comes back.

#### Binds

- The client's own commands cannot be bound: the game's (with their abbreviations, so not `/g` or `/r` either),
  `/lua`, `/plugin` and MQ's (`/echo`, ...).
- Nor can a name another script or plugin holds (an error naming it).
- A bind runs with the script's capabilities and those of whoever ran the command (another script, a plugin, the
  player) together, at its start and after each `mq.delay`.
- At most 256 binds a script; binding a command again replaces it (an error past them).

#### Settings files

`mq.pickle` and `dofile` / `loadfile` work on files under `mq.configDir` (`scripts/config/`, or
`scripts/config/<script>/` for a script without every capability, above).

- They are read back as data: the file runs with an empty environment.
- `loadfile` hands back only what it returned, and refuses a file whose values are or hold a function (`nil` and why;
  `dofile` raises it).
- A file is at most 8 MiB. A write that would leave `mq.configDir` holding more than 1000 files or 64 MiB is refused
  (an error).

#### Modules

**A module runs with the requiring script's capabilities, so its own file must be allowed at least those.** An
add-on's module does not run in a script that may do more; the player's libraries (`lib/Write.lua`) still serve the
player's scripts and add-ons.

#### The clipboard

`ImGui.GetClipboardText()` and `ImGui.SetClipboardText(text)` work only for a script with every capability (the
server's own, the player's under no `hailstone.cfg` or `player_caps = all`), never for an add-on with a ceiling: the
clipboard may hold a password, and what a script puts there is pasted where the player trusts it.

#### Left out on purpose

- `load` and `string.dump`: no running code from strings.
- `io` and `os.execute`, and every other `os` function except the clock: no file or process access.
- Lua's own `package` searchers: `require` only finds files the server's rules allow.

### TLOs

`Me`, `Target`, `Spawn[...]`, `SpawnCount[...]`, `NearestSpawn[n, ...]`, `Group`, `Zone`, `Spell[id|name]`, `Cursor`,
`FindItem[name|=exact]`, `FindItemCount[...]`, `Pet`, `Window[name]` (`.Open`), `Plugin[name]` (`.IsLoaded`), `Time`,
`GameTime`, `EverQuest` (`.GameState`, `.Server`), `Heading[deg]`, `Int`, `Float`, `Bool`, `String`, `Select`, `Math`,
`If`, `Range`, `LineOfSight[...]`, `Navigation` and `Stick` (below). Compiled plugins can add more.

The data types use MQ's member names and inheritance:

- `character` → `spawn`, `buff` → `spell`, `groupmember` → `spawn`.
- `class`, `race`, `heading` (`Degrees`, `ShortName`, ...), `timestamp` (`TotalSeconds`, `TimeHMS`, ...), `item`.
  A class's `Name` and `ShortName` (and `${Me.Class}`) are the server's where its plugin or script named the class
  ("Class names" below); compare `Class.ID` in a macro that must work on any server.
- MQ's pure types, with all of MQ's members ("The pure types" below): `int` (`Hex`, `Reverse`, `Float`, `Double`,
  `LowPart`, `HighPart`, `Prettify`), `float` (`Deci`, `Centi`, `Milli`, `Precision`, `Int`, `Raw`, `Prettify`),
  `string` (`Length`, `Upper`, `Lower`, `Left`, `Right`, `Mid`, `Find`, `Compare`, `CompareCS`, `Equal`, `NotEqual`,
  `EqualCS`, `NotEqualCS`, `Arg`, `Count`, `Token`, `Replace`, `StripLinks`), `bool`.
- `Math` (MQ2MathType): `Calc`, `Abs`, `Sqrt`, `Sin`/`Cos`/`Tan` (degrees), `Asin`/`Acos`/`Atan`, `Rand[max]` /
  `Rand[min,max]` (both included), `Hex`, `Dec`, `Not`, `Clamp[n,min,max]`, `Distance[y,x,z:y,x,z]` (3D; a missing
  point or coordinate is the player's). See "Math" below.
- `If[condition,whentrue,whenfalse]` (or with exactly two `~`, `If[c~a~b]`): the text of one branch, the false one
  running to the end; NULL when the condition does not calculate. `Range.Between[a,b:n]` and `Range.Inside[a,b:n]`.

#### The pure types

They follow MQ2BasicTypes.cpp, edge cases included:

- A member that needs an index is NULL without one (`Left[0]` too).
- `Arg` splits as MQ's GetArg: a run of separators is one, a "quoted run" is one argument.
- `Token` looks for each next separator from one past the last.
- `Replace`'s result is cut to 2047 bytes (an MQ string's room).
- MQ's int and int64 are one type. A value that fits 32 bits has int's `Hex` ("0xFFFFFFFF" for -1) and 16-bit
  `LowPart`/`HighPart`; a larger one int64's.
- `Int[...]` and `float.Int` cut toward zero, and are NULL for NaN, an infinity or a number past a 64-bit integer
  (MQ's result for those is unchecked).

Two deliberate differences: `float.Prettify` groups the number (MQ's prints the float's bits as an integer), and
`Compare` gives -1, 0 or 1 (MQ's only promises the sign).

#### Math

The arithmetic is MQ's Calculate (`mq/calc.h`), quirks included:

- `NULL`, `TRUE`, `FALSE` are numbers; spaces are skipped.
- `\` divides whole numbers, `^` is power and `^^` xor.
- Every operator groups from the left (`-2^2` is 4). The bit operators work on 32 bits.
- A formula that does not calculate (a letter, a division by zero, a stray `)`) is NULL.
- The result is a float, as in MQ.

#### Members a script uses most

For example `Me.PctHPs`, `Me.Buff[n|name].Duration`, `Me.Gem[n|name]`, `Me.SpellReady[n|name]`, `Me.Casting`,
`Me.Combat`, `Me.XTarget[n]`, `Me.FreeInventory`, `Me.Inventory[mainhand|pack1|23]`, and on spawns `Target.Distance`,
`Target.HeadingTo`, `Target.ConColor`, `Target.Named`, `Spawn[...].Loc`, `X`/`Y` as MQ's (the server's) coordinates.

For a script that fights:

- **Abilities** by name (a discipline's also without its "Rk. II"), an AA's group or a skill's number, as the Ability
  Book sees them: `Me.AbilityReady[Kick]` (a skill), `Me.CombatAbilityReady[name]`, `Me.AltAbilityReady[name|group]`,
  and `Me.AbilityTimer`, `Me.CombatAbilityTimer`, `Me.AltAbilityTimer` (a timestamp, the time left). Ready means the
  reuse timer ran out; endurance and mana are the script's to check.
- **Item click reuse**, for example `FindItem[=Clicky].TimerReady` or `Me.Inventory[mainhand].TimerReady`: seconds left
  on the later of the item's own reuse timer and its shared recast timer, 0 when both elapsed, NULL for an item
  without a click effect (or no item).
- **A spawn's buffs**: the player's own, or the target's as OP_TargetBuffs gave them (below).
- **Line of sight**: `Spawn.LineOfSight` (the player's eyes to the spawn through the zone's collision, as the client's
  own abilities check) and `LineOfSight[y,x,z:y,x,z]` (two points in MQ's order). NULL before the zone's collision is
  loaded, and for a coordinate that is not a finite number below 1000000 in size.
- `/stick` and the `Stick` TLO (`docs/navigation.md`).

The server sends target buffs only to some players (stock EQEmu: GMs, Inspect Buffs AA or leadership,
`AlwaysSendTargetsBuffs`), so check `Target.BuffsPopulated` first:

- `Target.Buff[n|name]` (a buff: `.Duration`, `.Caster`, `.ID` the slot ...), `.BuffCount`, `.BuffDuration[name]`;
- `.MyBuff[name]` and `.MyBuffDuration[name]` (cast by the player);
- the state a buff puts it in, by spell effect (EQEmu's spdat.h): `.Mezzed` (SPA 31), `.Charmed` (22), `.Feared`
  (23), `.Rooted` (99), `.Snared` (3, negative), `.Slowed` (11, under 100%); `.Poisoned` (36), `.Diseased` (35),
  `.Cursed` (116), `.Corrupted` (369), a buff with counters of that kind.

#### Spawn searches

Spawn searches use MQ's words and rules (ParseSearchSpawn and SpawnMatchesSearch, `mq/spawn_search.h`):

- kinds (MQ's spawn types): `pc`, `npc`, `mount`, `pet`, `pcpet`, `npcpet`, `corpse`, `npccorpse`, `pccorpse`,
  `trigger`, `untargetable`, `trap`, `chest`, `timer`, `aura`, `object`, `banner`, `campfire`, `mercenary`, `flyer`,
  `any`, assigned by body type, race or class as MQ's GetSpawnType does. So `npc` leaves out mounts, mercenaries,
  auras, banners, untargetable spawns and the like, and keeps an NPC's pet but not a player's.
- filters: `id N`, `notid N` (one id: the last), `radius R` (3D, from the player or `loc`), `zradius R` (from the
  player's z or `loc`'s), `range MIN MAX`, a bare number (that level), `class X` (the long name) or a bare class
  name (long or short), `race X`, `body X`, `named`, `targetable`, `los`, `group`, `nogroup`, `raid`,
  `fellowship`, `guild`, `guildname X`, `noguild`, `nopet`, `xtarhater`, `nopcnear [R]` (200 without a number),
  `gm` (with `npc`: GM trainers), `merchant`, `banker`, `tribute`, `knight`, `tank`, `healer`, `dps`, `slower`
  (classes; ignored with `npc`), `trader`, `lfg`, `light [NAME]` (MQ's light names: `CDL`, `TR`, `LS`, ...),
  `playerstate N`, `loc X Y [Z]` (a Z of 0 or none is the player's, and the word after is not taken), `alert N`,
  `noalert N`, `nearalert N`, `notnearalert N`
- names: words that are no keyword make a partial name, matched in the name as sent or cleaned. `=Name` or
  `name =Name` asks for the whole cleaned name (MQ keeps the `#` of a `#Name` there, so `=` never finds one)
- `/target`'s `next` / `prev`: the match after / before the current target in distance order, instead of the
  nearest; the last word wins

Results come nearest first (3D, from the player). `/target`'s `next` and `prev` walk that list from the current
target; other callers ignore them and take the nearest (`next` and `prev` do not filter), so `SpawnCount` counts
every match and `NearestSpawn[N]` can take the Nth. With no target, or one the zone no longer has, both directions
take the nearest valid match: the nearest, with you left out while another spawn matches. A bare-name `/target`
without `next` or `prev` is not the same: RoF2's choice (`docs/commands.md`) takes your exact name even then. A
current target that is no match starts from its own distance: `next` takes the first match farther away and `prev`
the last one nearer. Past either end of the list there is none: it does not start again at the other end.
`targetable` is the game's own test, as in MQ: the Targetable flag on and body type not 11 (EQEmu's NoTarget, given
to portal and trigger NPCs). `Spawn.Targetable` is the flag alone, as in MQ, so a script filtering spawns itself
checks `BodyType` too.

Differences from MQ:

- `level N`, `afk`, `linkdead`, `buyer`, `exact` and `merc` are Hailstone's own words.
- No alert lists are kept, so `nearalert` finds nothing and the other alert words ask nothing (as MQ with no list).
- `fellowship` matches the server-supplied roster, including expanded rosters of up to 64 members.
- `los` keeps a spawn while the zone's collision is not loaded.
- `/target next` and `/target prev` measure `radius`, `zradius` and `nopcnear` from the player, where MQ measures them
  from the old target. Both walk the spawns nearest the player first, but MQ sorts by flat (XY) distance
  (`UpdateMQ2SpawnSort`, refreshed every pulse) and Hailstone by 3D distance, as its other searches do.
- `/target` leaves you out while another spawn matches, in every direction: a typed search, `next` and `prev` alike.
  A bare name without `next` or `prev` is the exception, because RoF2's choice takes your exact name even then. That
  is Hailstone's own rule, not MQ's.

Not yet:

- TLOs: `Raid`, `Mercenary`, `Corpse`, `Merchant`, `Ground`, `Macro` and other MQ plugins' TLOs (`Navigation` is
  built in: `docs/navigation.md`)
- the `AltAbility` type (`Me.AltAbility[...]`)

### MQ commands

`/echo`, `/target` (`id N`, `clear`, `mycorpse`, `myself`, or a spawn search; with nothing, yourself; `next` / `prev`
walk the search's matches), `/face` (target, `id`, `heading H`, `loc y,x`,
a search; `fast`), `/keypress <command> [hold|release]` (the game's key command names: `forward`, `jump`, `hot1_1`, ...),
`/squelch <cmd>`, `/docommand <cmd>`, `/noparse <cmd>`, `/lua ...`, `/plugin ...`. `/nav` (MQ2Nav's commands),
`/moveto` and `/stick` are built in (`docs/navigation.md`).

`${...}` is parsed in every command line, typed or scripted, except:

- `/noparse`;
- the lines the client's own windows send for a button (the guild window's Go to URL, Join Channel and Update MOTD, a
  member's or friend's name): their text is what other players and the server wrote.

`${...}` is expanded in one pass, innermost first (`${Me.Buff[${n}].Duration}` works). A value's text is never parsed
again (a spawn named `${Me.Name}` stays that text): a deliberate difference from MQ2's `ParseMacroData`, so names
others choose cannot read the game with a script's `read`. `/docommand` and `/squelch` run the rest of their line as
it was parsed with them, not parsed again (`/docommand /echo ${Spawn[...].Name}` echoes the name as written).

A line a script or plugin runs is checked against its permissions before and after the other line filters, so a
compiled plugin's command-line rewrite (`command_line` in its hook table) cannot turn an allowed line into a forbidden
one.

## Lua plugins

The `hailstone` module (`require('hailstone')`, or the global `hailstone`) lets a script extend the client as a
compiled plugin does. Each part needs the same capability as in a compiled plugin.

| | |
|---|---|
| `hailstone.hooks{ name = function(...) ... end }` | answers the core's questions, named as the compiled plugins' `HailstoneHooks` (below): `is_spellcaster()`, `spell_level_needed(spell_id, class)`, `song_blocked(bard_level)`, `skill_gate(skill, mask, profile_skill)`, `who_lines(who)`, `label(id, text)`, `item_right_click(slot)`, `list_level(item)`, ... nil keeps the stock answer (`extend`). A hooks table with a metatable is read through it in a protected call: an error in its `__index` ends the script |
| `hailstone.hooks{ nameplate = function(spawn, text) ... end }` | a Lua-only hook (`extend`); see "The nameplate hook" below |
| `set_active(on)`, `set_flag(name, on)` | the switches: `wide_item_links`, `items_any_class`, `hide_any_class`, `aa_any_level`, `aa_train_all`, `augment_classes`, `research_classes`, `focus_classes`, `backstab_either_hand`, `solo_adventure` (`extend`) |
| `set_player_classes(mask)`, `set_character_classes(name, mask)`, `set_pet_list{{spawn =, class =}}`, `set_vitals{max_hp =, ...}` | what the server knows (`extend`) |
| `set_class_name(class_id, name [, short_name])` | the server's name and short name for a player class (1-16); see "Class names from Lua" below (`extend`) |
| `on_packet(stream, direction, opcode, fn)` | `fn(opcode, data)` for each such packet (`'world'` / `'zone'`, `'in'` / `'out'`, opcode 0 all): `true` drops it, a string replaces its data. At most 256 a script (each call adds one until the script ends; an error past them). Use `string.pack` / `string.unpack` for layouts (`packets`) |
| `send_packet(stream, opcode, data)` | a packet to the server (`packets`) |
| `ui_function(name, fn)`, `option{name, label, min, max, get, set}`, `menu(label, command)` | for its windows, as a compiled plugin's (`ui`); `hs.UI.plugins.Get("<script name>")`. A Lua social cannot call them (an error) |
| `player()`, `spawn(id)`, `spawns()`, `spell(id)`, `item(slot [, sub])`, `buff(i)`, `xtarget(i)`, `raid()`, `eqstr(id, ...)`, `zone_name(id)`, `server()` | the game state as tables (`read`); see "Game state tables", "The raid roster" and "Item fields" below |
| `spell(id)` display fields | raw spell data: `range`, `ae_range`, `push_back`, `duration_formula`, `duration_cap` (ticks), `spell_type`, `resist`, `resist_adj`, `spell_icon`, `category`, `subcategory`, `cast_on_you`, `cast_on_other`, `wear_off`, and `effects` (12 1-based entries with `effect`, `base`, `base2`, `max`, `formula`). Loaded record values, not computed descriptions |
| `hs.DisplaySpellId(n)`, `hs.DisplayItemData(n)` | see "Display instances" below |
| `right_click_item(slot [, class])`, `close_spell_book()`, `clear_spell_recovery()`, `set_window_pet(id)`, `inspect_item(id [, augments])`, `set_max_camera_distance(d)` | the client's own actions (`actions`) |
| `chat(text [, color])`, `server_dir()`, `character_dir()`, `max_camera_distance()` | a chat line in one of the game's colours; its folders for the server and the character (see "Lua plugin folders" below) |
| `server_rule(section, key)` | a setting from a non-client section of the server's `hailstone.cfg` (`[autocombat] hunter = off`), as written; nil when absent |
| `autoskill_list()` | the player's autoskill list in order: `{key, name, endurance, mana, target_hp_below, target_hp_above, self_hp_below, when, keep_buff}` (`read`) |
| `alt_abilities()` | the abilities the AA window lists, in its order (by tab, then rank id); see "Alternate abilities" below (`read`) |
| `train_aa(group)` | buys the next rank of an ability as `/alternateadv buy` does (no question), when the window's own check allows it; false, with nothing sent, otherwise. One rank a call; points and ranks change when the server answers (`actions`) |
| `use_ability(kind, name)` | uses a skill (`"skill"`), combat ability (`"disc"`) or activated AA (`"aa"`) by name (a combat ability's also without its rank), an AA's group or a skill's number, as its hot button does; false when the player has none such (`actions`) |
| `hold_autoskill(on)` | the Ability Book's autoskill uses nothing while held (its switch stays; the button shows "On (held)"); the hold goes when the script stops (`actions`) |

#### The nameplate hook

`hailstone.hooks{ nameplate = function(spawn, text) ... end }` has no compiled plugin counterpart.

It is called for each visible world nameplate with the spawn table (reused for every plate: valid during the call
only) and the assembled name (title, last name, suffix and marks). Return a string to replace the text, or nil to keep
it; extensions run in order. Only the world nameplate changes, not the target window or the spawn's data.

#### Class names from Lua

`set_class_name(class_id, name [, short_name])`: nil or `""` shows the stock one.

Bad input (see "Class names" below for the limits) is an error; nothing changes. The names go when the script
stops, loses `extend`, or the session leaves the server. A script that names classes stays loaded after its main
chunk returns.

#### Game state tables

- `server()`: `{name, host, port, world, world_host}`, nil before the world login.
- `player()`'s `cur_hp` and `max_hp` include the items' HP that EQEmu's HP update leaves out, as the windows show them
  (as `HailstonePlayer` and `Me.CurrentHPs`). `player()` also has `aa_points` and `aa_spent`.

#### The raid roster

`raid()` gives these columnar tables:
`active, locked, loot_type, count, leader, target, spawn_id[], name[], group[], flags[], level[], class[], pct_hp[], pct_mana[], pct_endurance[], pet_spawn_id[], pet_pct_hp[], x[], y[], z[], distance[]`.

- Members are 1-based; `leader`/`target` are 1-based, 0 none; `-1` is an unknown percentage.
- Flag bits: `RAID_LEADER`, `RAID_GROUP_LEADER`, `RAID_MAIN_ASSIST`, `RAID_MARKER`, `RAID_WAYPOINTER`, `RAID_LOOTER`,
  `RAID_PRESENT` and `RAID_ME` on the `hailstone` module, with the SDK's `HS_BULK_RAID_*` values.

#### Display instances

`hs.DisplaySpellId(n)` and `hs.DisplayItemData(n)` read display instance 1..6 in a Lua UI window.

Item data (the inspected item, not necessarily in inventory) gives `id`, `name`, `icon`, `lore`, `value` (copper),
`stackCount`, `damage`, `delay`, `itemType` and `favor` (old names `stack_count` and `item_type` kept), and the effect
spell IDs `scrollSpell`, `focusSpell`, `procSpell`, `wornSpell` and `clickSpell` (0: none) for `spell(id)` or
`hs.SpellDetails(id)`. An ID can name a spell the local spell file lacks.

#### Lua plugin folders

`server_dir()` and `character_dir()` are the script's folders for the server and the character (`plugin-config/<name>/`,
the name written as for `mq.configDir`).

- A Lua social's dry run and face get the path and make no folder.
- A script named like a compiled plugin (installed, loaded or the add-on manager's, in any case) gets
  `plugin-config/script%3A<name>/` instead, so it cannot read what the plugin keeps (a server's key).
- The add-on manager installs no script and plugin of one name either (docs/addons.md).

#### Item fields

`item(slot [, sub])` gives `id`, `name`, `type`, `item_class`, `classes`, `races`, `icon`, `stack`, `charges`,
`req_level`, `bag_slots` (0: not a bag), `click_spell` and `scroll_spell` (0: none).

- With a click effect also `click_type`, `click_level`, `click_cast_time` (ms), `click_recast` (s),
  `click_recast_type` (the timer group), `click_max_charges` (-1: unlimited) and `click_ready_in` (seconds left, 0:
  ready).
- Slots are `/useitem`'s: 0..22 worn, 23..32 the general slots, `sub` the place in a bag from 0.

#### Alternate abilities

`alt_abilities()` gives, for each ability,
`{id, group, name, tab, category, rank, max_rank, cost, level, maxed, can_train, activated, prereqs}`:

- `id` is the rank the window shows (the next to buy, or the owned last one); `rank` is the ranks owned;
- `cost` and `level` are the next rank's (`cost` 0 at the maximum);
- `can_train` is the window's own check (points, level, class, expansion, prerequisites);
- `prereqs` is `{{group, rank, met}}`.

#### How a Lua plugin runs

A script that gives the client any of these stays loaded after its main chunk returns, until stopped; everything it
gave goes with it. Its hooks run on its own Lua state with its capabilities and must finish within 50 ms; while the
script itself is running (a hook reached from its own command) they give the stock answer. An error in a hook ends the
script.

A script folder (`scripts/<name>/init.lua`) with a `ui/<pack>/` beside it has windows for each UI pack.

#### Starting a script

- The server: `hailstone.cfg` `[scripts] run`.
- The player: `hailstone.ini` `scripts.autoload = name, name` (started when the rules allow), Autorun in the Scripts
  window, Run there, or `/lua run <name>`.
- With the `login` capability it starts at the world login (above).

It first runs on the next frame.

A script started by another script or a plugin (`mq.cmd('/lua run x')`, a plugin's command) gets no more capabilities
than its starter has then. If the starter can give none, it does not start.

`/lua stop x` then `/lua run x` closes the old one first, so the new one keeps the UI functions, options and menu
entries it registers. A script that stops and reruns itself from its own code is refused ("is still stopping").

#### What is never a script

- Nothing under `scripts/config/` (`mq.configDir`, where `mq.pickle` writes) runs or loads as a module, in any case,
  with trailing dots or spaces (`Config/`, `config./`), or through links.
- A script or module name is a path inside the script folders. An absolute one (`/lua run /home/x/evil`,
  `require('/x')`), or a link leading out of the folder, is not found.
- A `:` in a name is refused.

#### Script names and case

`/lua run` finds a script whatever the case of its name, on every platform: `/lua run autoloot` starts
`scripts/AutoLoot/init.lua`.

- A name spelled exactly as on disk wins.
- Otherwise each path part must match exactly one entry that differs only in case: `AutoLoot/` beside `AUTOLOOT/`
  finds neither.
- A module's name (`require`) is looked up as written.

## The Scripts window

`ScriptsWnd` (`ui/<pack>/scripts.lua`, `app/lua_ui_scripts.cpp`), opened from the EQ menu's **Lua Scripts** row
(Gamepad UI: More > Lua Scripts).

It lists every script (`<name>.lua` and `<name>/init.lua`: the server's `scripts/` first, then `scripts/` and the
install's, without `config/`; a duplicate name once, as `/lua run` would pick) with its state (stopped, running,
paused, ended, error):

- **Run** / **Stop** (`/lua run`, `/lua stop`), and **Pause** / **Resume** while it runs (`/lua pause`). The Modern
  UI's Arguments box gives Run its arguments, split as `/lua run` splits them. Gamepad: A runs or stops, X pauses.
- **Autorun** (Gamepad: Y; greyed until a character is in the world): the script starts each time this character
  logs in: at the zone-in, or at the re-check of the rules that follows, when the rules allow it (skipped quietly
  otherwise). Stored in the character's `scripts.ini`
  (`[Autorun] name = 1`, in `servers/<profile>/characters/<world>/<name>/`; docs/layout.md).
- A script the rules do not allow is greyed, with the reason in its tooltip.
- The server's own scripts show Stop and Pause greyed: the player cannot stop them.

Scripts keep running through a zone change. Camping to character select makes the rules provisional again (only
`login` scripts keep running); a disconnect stops every script. The autorun list is read once per login, so a script
the player stopped stays stopped after zoning.

## Lua socials

A Hailstone extension of the game's socials (`src/game/lua_social.h`): a social can hold Lua instead of its five
command lines, run by its hot button with the scripts' `mq` module. Two parts:

- **Press** runs when the button is pressed. It sees `button` = `{social, bar, button, shift, ctrl, alt, presses}`
  and `state`, a table kept from press to press while the code stays the same.
- **Face** (optional) returns how the button looks: `{label, color (0xRRGGBB or {r, g, b}), cooldown (0..1 of a
  sweep left), overlay (corner text), enabled (false: dimmed), tooltip}`, or a label string. It runs at most every
  200 ms, reads the game state only, and cannot send commands.

A social is named `social '<title>'` in messages and folders (`mq.configDir`); no script name may start `social '`.

```lua
-- [press]: heal the target, the big heal under 50% health; Shift heals yourself
if button.shift then mq.cmd('/target id ' .. mq.TLO.Me.ID()) end
if (mq.TLO.Target.PctHPs() or 100) < 50 then mq.cmd('/cast 1') else mq.cmd('/cast 2') end
-- [face]: the target's health on the button
local hp = mq.TLO.Target.PctHPs()
return {label = hp and (hp .. '%') or 'Heal', enabled = hp ~= nil}
```

**For players:**

- **Make or edit** one in the **Lua Social** window: right-click a Lua social, pick **Lua** in the Edit Social
  window, or `/luasocial new` (first empty social) / `/luasocial <1..120>`.
- **A plain social is converted**: lines become `mq.cmd` calls, a `/pause` an `mq.delay` (waited only in full mode)
  and a `/timer` a comment.
- **Commands** at the top goes back: a press of at most five `mq.cmd('...')` / `mq.delay(ms)` lines, with no face,
  becomes command lines again.
- **The editor** completes `mq.TLO` paths from the live game (Tab takes, Ctrl+Up / Ctrl+Down choose), `mq.` and
  `button.`, and marks an error's line.
- **Dry run** runs the press with the chosen modifiers and lists the commands it would send (and which the server
  would block), sending nothing and skipping waits. Anything that acts or stays (`mq.bind`, `mq.event`, `mq.imgui`,
  `mq.pickle`, `mq.web`, the `hailstone` module's actions and hooks) ends it: "does nothing in a dry run". Works on
  any server.
- **Put on cursor** puts the social on the cursor for a hot bar.
- **Copy share text** / **Paste** share a social as one line (`hailstone-social:1:...`). The Edit Social window
  shares plain socials the same way (lines in a `-- [lines]` section); pasting across the two windows converts.
  Older clients do not read a plain social's share text.
- Pressing a running Lua social again (full mode), or right-clicking it, stops it.
- Files: `socials/<n>.lua` beside the character's `hotbuttons.ini`.

**What the server allows** (its `hailstone.cfg`'s `[socials]`):

| Mode | A press... |
|---|---|
| off | does nothing. Only the five-line socials run. |
| press (the default under a `hailstone.cfg`) | runs to its end at once: no `mq.delay`, no chat events, no windows, at most N commands, 50 ms of Lua |
| full (and on a server without a `hailstone.cfg`) | runs like a script, within `caps`: it may wait, use events and draw windows |

Faces run only when `faces` is on, and always with `read` alone.

## Compiled plugins

Compiled add-ons may read `Spell.DurationFormula` and `Spell.DurationCap` through `parse`: plain integers from the
player's spell record, with the cap in six-second ticks. The existing `Spell.Duration` timestamp remains the cap
multiplied by 6000 ms; the raw formula is not a claim about a caster's effective duration.

`get_large_dialog` (`read`) observes the current received game popup, including its host-issued token, button mode,
server IDs, remaining deadline and the current camp token when it was received during that camp. It never exposes
the native-plugin consent question. `answer_large_dialog` (`actions`) answers that exact current popup once: OK,
Yes, No or close. A stale/replaced/expired popup, a button absent from its mode, lost permission or a refused send
returns zero without consuming the question. Both calls are appended ABI 3 services: check `HAILSTONE_HOST_HAS`.

The optional `HAILSTONE_DIALOG_EXPECT_CAMP_DISCONNECT` answer flag also needs an active `extend` owner, Yes and the
snapshot's current nonzero camp token. It lets a server extension identify an accepted server camp offer. It only
routes a subsequent validated server disconnect through the ordinary camp destination; timeouts and socket failures
remain link deaths. It does not assert server acceptance,
send Logout early, alter the player's Fast Camp setting or advance the normal timer. Cancellation, a new camp,
zoning and disconnection retire that lifetime. Without a disconnect the normal camp continues. The player's READ
snapshot reports `HAILSTONE_PLAYER_CAMPING` while the native camp is in progress, including before a popup arrives.
Popup tokens retire when the connection, zone or player lifetime changes. The opaque map-policy context remains
stable through ordinary camping and cancellation; the actual state and action eligibility are checked separately.

A plugin is a `.dll`, `.so` or `.dylib` built against `sdk/hailstone_plugin.h`, a plain C interface that works with
any compiler. It follows MacroQuest's plugin shape: export the callbacks you need and use the MQ-style helpers.

On macOS the client loads only `.dylib`; CMake names a `MODULE` library `.so` there, so set
`CMAKE_SHARED_MODULE_SUFFIX` to `.dylib` (or the target's `SUFFIX`). The client build applies this suffix to its own
plugin targets.

**Licence.** The client is GPL-2.0-or-later, but `sdk/` is MIT (`sdk/LICENSE`). NOTICE.md's plugin and add-on
exception lets a plugin built against it, or a script using the `mq` module and `hs.*`, use any licence, closed ones
included. A plugin that copies the client's own code (anything outside `sdk/`) is covered by the GPL.

```c
#include "hailstone_plugin.h"
HAILSTONE_PLUGIN   /* the required exports and the `host` global (the host table) */

static void hello(void* user, const char* args) { WriteChatf("Hello, %s!", *args ? args : "world"); }

HAILSTONE_EXPORT void InitializePlugin(void) { AddCommand("/hello", hello, 0); }
HAILSTONE_EXPORT void ShutdownPlugin(void) { RemoveCommand("/hello"); }
HAILSTONE_EXPORT void OnPulse(void) { /* every frame */ }
```

Callbacks:

- `InitializePlugin`, `ShutdownPlugin`, `OnPulse`, `SetGameState(state)`
- `OnBeginZone`, `OnEndZone`, `OnZoned`, `OnAddSpawn(id)`, `OnRemoveSpawn(id)`
- `OnIncomingChat(line, color)`, `OnFilterChat(line, color)` (nonzero hides the line); see "Chat hooks" below
- `OnUpdateImGui`, `OnPacket(stream, direction, packet)` (nonzero drops it)

Host functions:

- `WriteChatf`, `DoCommand(f)`;
- `AddCommand` (needs `commands`; 0 for one of the client's own commands, as `mq.bind`), `RemoveCommand`;
- `ParseMacroData` (`${...}`, any TLO);
- `AddTopLevelObject`: a TLO with members the plugin answers; refused when the name is already a TLO (as in MQ);
- `RemoveTopLevelObject` (only the plugin's own);
- `host->send_packet`, `host->resize_packet`, `host->config_dir`, `host->caps`, `host->imgui_context`.

#### Chat hooks

`OnIncomingChat` and `OnFilterChat` get the server's lines and the client's own, not those a script or plugin writes
(`print`, `/echo`, `WriteChatf`, a reply to its command; MQ hands those to `OnWriteChatColor`).

Combat hit lines (melee and spell damage, heals) always arrive as the full sentence, whatever the player's Hit Modes,
so a parser matches one form. A hook that rewrites such a line shows its rewrite in full.

#### The host table

The host table is a `PluginHostApi`, one per plugin, handed over by `hailstone_plugin_set_host` before
`InitializePlugin` and kept in the global `host`. Each function takes `host->host` first, which identifies the
calling plugin.

**Threads.** Call host functions only from inside the plugin's callbacks (`InitializePlugin`, `OnPulse`, a command,
a hook, ...), on the client's thread: its state has no locks. From another thread a call does nothing and returns
0 / NULL (logged once); `ability_state` returns -1 ("unknown"), since its 0 means "on reuse". Hand a worker thread's
results over and act on them in `OnPulse`.

**Exceptions.** A C++ plugin must not let an exception out of anything the client calls (callbacks, commands, TLOs,
hooks, UI functions, options). The client catches what it can: the call counts as unanswered (a TLO is NULL, a hook
gives the stock answer), chat and log say so once, and one out of `InitializePlugin` or `hailstone_plugin_hooks`
unloads the plugin with everything it registered. That is best effort: an exception crossing a C interface is
undefined behaviour, and one from another compiler or runtime may end the client.

#### The names up to 0.6

The SDK was `nmsc_plugin.h` and `nmsc_byte_io.h`, with `NmscHost` and the other `Nmsc*` types, the `NMSC_*` macros,
the global `nmsc`, the namespace `nmsc` for `ByteReader` / `ByteWriter`, and the exports `nmsc_plugin_abi`,
`nmsc_plugin_set_host` and `nmsc_plugin_hooks`.

- Old source builds unchanged (the old headers in `sdk/` add the old names). `nmsc` is a macro for `host`; define
  `NMSC_NO_OLD_GLOBAL` before the include to free the name.
- Old binaries load unchanged: the client tries each export's new name, then its old one.
- `HAILSTONE_PLUGIN` exports the two required functions under both names, so new plugins load in a 0.6 client too;
  one with hooks for 0.6 exports `nmsc_plugin_hooks` as well.

#### Building a plugin

`plugins/example/MQ2Example.c` uses all of it and builds with the client into `build/hailstone/plugins/`. By hand:

```sh
cc -shared -fPIC -O2 -I path/to/hailstone/sdk MyPlugin.c -o MyPlugin.so      # Linux
cl /LD /I path\to\hailstone\sdk MyPlugin.c                                    # Windows
```

MacroQuest's plugin sources do not build as they are: they read the game executable's memory layouts
(`pLocalPlayer`, `PSPAWNINFO`, ...). Port them to TLO reads (`ParseMacroData`) and the callbacks above.

A plugin that draws with Dear ImGui must be built with the client's Dear ImGui version and call
`ImGui::SetCurrentContext(host->imgui_context(host->host))`.

### Server extensions (ABI 2)

A server whose players used a patched client (a DLL beside eqgame.exe, a changed eqgame) can give Hailstone the same
changes as a plugin. With the `extend` capability a plugin can:

- **answer the core's questions** where such a client differs from stock. It exports
  `const HailstoneHooks* hailstone_plugin_hooks(void)`, a table of optional functions covering spell levels, songs,
  memorised gems, combat ability timers and whether their cool-down takes the reuse focus, the class used for casts,
  skill checks and tracking, the guild class column, `/who` output, labels, the item display's Level Needed line and
  extended target slots. Returning 0 (or NULL) leaves the stock answer.
- **switch rules** with `set_flag` (table below).
- **tell the client what its server knows**:
  - `set_player_classes` and `set_character_classes`: multiclass masks, shown as "War/Clr" in the window title,
    character select and the guild window;
  - `set_class_name(class_id, name, short_name)`: the server's name and short name for a player class (1-16) (class 1
    shown as "Vanguard", "VAN"); see "Class names" below;
  - `set_pet_list`: several pets, with their gauges and the focused pet;
  - `set_vitals`: the server's HP, mana and endurance in place of the client's formulas; the values also become the
    current points until the next update.

The `set_flag` rules:

| Rule | Switch (where named) |
|---|---|
| wide item links (8-digit ids) | |
| every item usable (only the server checks) | |
| Hide for every class | |
| alternate abilities from level 1 | |
| the AA window's Train All | |
| an augment for a worn item, over the player's classes | `augment_classes` |
| the research containers, over the player's classes | `research_classes` |
| focus effects over the player's classes: SPA 411, an AA's classes, and no Bard / pure melee focus filter | `focus_classes` |
| Backstab with a piercer in either hand, not only the primary | `backstab_either_hand` |
| an adventure recruiter's Request from a player in no group or raid | `solo_adventure` |

- "The player's classes" are the mask from `set_player_classes`, else the profile's class.
- `backstab_either_hand` is for a server whose backstab uses the better hand.
- `solo_adventure` is for a server that takes solo adventures: stock EQEmu drops such a request unanswered, so without
  it the client stops the request with its own line.
- Clients older than a switch ignore it.

#### Class names

`set_class_name(host, class_id, name, short_name)` is an extension past RoF2 that changes only what the player reads:
class ids, checks, packets and everything sent to the server keep the stock classes. Needs `extend`; added later in
ABI 3, so test `HAILSTONE_HOST_HAS(host, set_class_name)` first. Lua:
`hailstone.set_class_name(class_id, name [, short_name])`.

- **Where it shows.** Inventory, stats, group, raid, guild and
  fellowship windows; `/who` (without the stock level titles); character select and the window title's multiclass
  list ("Van/Clr"); item and spell class lists, the Ability Book, the Find window; the class's guildmasters (20-35 read
  "<name> Guildmaster"); MQ's `${Me.Class}`, `${Spawn.Class}`, `Class.Name` and `Class.ShortName` (`Class.ID` stays
  the id). NPC-only classes (banker, merchant, ...) are not renamed.
- **Stock names stay** in the layouts' child names (`CC_Class_Warrior_Button`), bug reports and the login screen's
  saved account list.
- **Searches take both.** A spawn search (`${Spawn[vanguard]}`, `class Vanguard`) and a `/who` class word take the
  stock name and the server's, long or short. A `/who` word is matched against the stock names first.
- **Input.** An empty name or short name (NULL in C, nil in Lua) shows the stock one; both empty take the caller's
  names back. A name is at most 32 bytes and a short name at most 4 (UTF-8 bytes), with no control characters. Longer
  input, or a class id outside 1-16, is refused, not cut: nothing changes, and a compiled plugin gets a log line (the
  first time), a script an error.
- **Lifetime.** The names belong to their setter. They go when it unloads or stops, loses `extend`, or the session
  leaves the server. Character select on the same server keeps them while their owner runs (naming classes there needs
  the `login` permission). When two owners name one class, the latest call wins; when its names go, the one before
  shows again.
- **For interfaces.** Text the client gives a Lua UI already carries the names. An interface with its own class list
  asks `hs.ClassName(id [, short])` (`API.character.ClassName`, UI API 1.7) and keys a cache on
  `hs.ClassNamesRevision()` (docs/ui-api.md "Class names").

#### What else a plugin can do

- **read the game state** (with `read`) through typed structs; see "Reading the game state" below.
- **take item clicks** (`item_right_click`, then the client's own with `right_click_item`, which needs `actions`)
  and fill the merchant list's level column (`list_level`).
- **act** (with `actions`); see "Actions" below.
- **put spawns on the maps**; see "Spawns on the maps" below.
- **keep files** for the server and for the character:
  - `server_config_dir` is `servers/<profile>/plugin-config/<name>/` (in the world too on a server without
    Hailstone's quest script);
  - `character_config_dir` is the character's `characters/<world>/<character>/plugin-config/<name>/` (0 before there
    is one);
  - `config_dir` stays the folder for every server;
  - `write_chat_color` writes a line in one of the game's chat colours, and `zone_name` gives a zone's short name.
- **read and write packets** with the SDK's `hailstone_byte_io.h` (C++20, header only): the client's own
  `ByteReader` and `ByteWriter`.
- **connect** (with `net`) to a service the server runs beside the game: see Connections below.

#### Reading the game state

| Call | Gives |
|---|---|
| `get_player`, `get_spawn`, `next_spawn` | the player; a spawn; every spawn in the zone |
| `get_spell`, `get_item`, `get_buff`, `get_xtarget` | a spell; an inventory slot; a buff; an extended target |
| `combat_ability` | the character's combat abilities |
| `skill` | a skill's value as the profile holds it |
| `aa_value` | an owned alternate ability's slot value, by its ability id |
| `eqstr` | a game string with its `%1` filled |
| `ability_state` | whether a skill, a discipline or an activated AA is ready, and its remaining and reuse time, by name or number |
| `server_info` | the server this login is on (the saved server's name, host and port, the world's name and address), to tell apart servers that send the same packets |

A spawn also carries rider, mercenary and GM flags, group membership, con colour and body types. Each struct starts
with its size; the client fills what both know.

The bulk reads give packed records (Buff, spawn, extended target, ability and raid records below):

| Call | Gives |
|---|---|
| `get_buff_data_bulk` | every buff slot of the player, in one call |
| `get_pet_buff_data_bulk` | the pet window's buffs |
| `get_target_buff_data_bulk` | the current target's buffs |
| `get_avail_aa_data_bulk` | the owned alternate abilities |
| `get_avail_disc_data_bulk` | the combat abilities |
| `get_ability_data_bulk` | the Ability Book's entries with their readiness |
| `get_buff_name_data_bulk` | the player's buffs with their casters' names |
| `get_spawns_data_bulk` | every spawn in the zone |
| `get_raid_data_bulk` | the raid roster |
| `get_spells_data_bulk` | spells by id, with their effects as text |
| `get_spells_by_name_data_bulk` | every spell of a name, laid out as `get_spells_data_bulk`'s |
| `acquire_bulk_buffer`, `release_bulk_buffer` | the plugin's own 2 MiB buffer for the reads above |

#### Actions

With `actions` a plugin can:

- close the spell book, and end the global spell recovery;
- show another of several pets in the pet window (`set_window_pet`, the client's choice only);
- open an item's display (`inspect_item`);
- open the bank window at any spawn or the player, without a banker's class and range checks (`open_bank`);
- change the camera's zoom limit (`set_max_camera_distance`);
- use one of the player's abilities by name as the Ability Book's row does (`use_ability`; `kind` is
  `HS_ABILITY_SKILL`, `HS_ABILITY_DISCIPLINE` or `HS_ABILITY_ALT`);
- change the hot buttons: `hot_button` reads any button of any bar and page as the stock character ini's string (with
  `read`), `set_hot_button` puts one, `hot_bar_page` and `set_hot_bar_page` read and set a bar's page. Changes are
  saved with the character.

Hooks: `before_use_skill` may refuse a skill's use, and `command_line` may rewrite a typed or scripted line before
the client runs it.

#### Spawns on the maps

The game's maps (map window, minimap, zone map) show only the group, as RoF2's does. `map_spawn` is asked for every
other spawn each time a map draws (the minimap: only inside its circle) and shows those it answers for, with a colour,
a map layer (layer switches and height filter apply) and a label, prefilled from the con colour and name. A Lua
plugin's `map_spawn(spawn)` answers true for the defaults, or a table with `color`, `layer` and `label`.

#### Activity, order and compatibility

A plugin is active from its load. `set_active(0)` keeps its hooks, switches and vitals out until it has seen its
server (its own packets, for example); `set_active(1)` brings them back.

Several plugins may extend the client at once: they are asked in load order and the first answer wins
(`src/game/server_ext_chain.h` lists how each kind of answer combines). Example: `tests/plugins/ExtendTest.c`.

**Compatibility.** The host table and the hook table carry their size and only grow at their end.

- A plugin built for an older ABI loads in a newer client, not the reverse. This SDK reports ABI 3 (which added the
  web pages, `web_*`, "Web pages" below).
- Calls added since keep ABI 3: test `HAILSTONE_HOST_HAS(host, open_bank)` before such a call, since an older client
  has a shorter table. `use_ability`, `ability_state`, `get_ability_data_bulk` and the other calls under "added later
  in ABI 3" in `sdk/hailstone_plugin.h` are such calls.
- The structs a plugin passes to be filled (`HailstonePlayer`, `HailstoneSpawn`, ..., `HailstoneWebState`) carry
  their size too. `HailstoneWebState`'s `size` is a `size_t` (as ABI 3 shipped it), the others' a `uint32_t`.

The current SDK places `get_spells_data_bulk` after the native nameplate functions and before `get_large_dialog`
and `answer_large_dialog`. Windows x64 builds can set `HAILSTONE_LEGACY_POPUP_PLUGIN_SHA256` to the exact SHA-256
of a binary built against the earlier popup-only tail. The host gives the earlier table only to a binary matching
that digest.

A C++ allocation or provider exception at an SDK entry point returns the ordinary zero/void failure result. Side
effects completed before it are not retried or undone.

#### The cursor item's origin

`cursor_origin(host)` says how the item on the cursor got there, for a plugin that puts away or destroys what the
server hands the player. Needs `read`; added later in ABI 3, so test `HAILSTONE_HOST_HAS(host, cursor_origin)` first.

| Value | Meaning |
|---|---|
| `HAILSTONE_CURSOR_ORIGIN_UNKNOWN` (0) | not known, or nothing on the cursor, or the plugin lacks `read`; see below. Treat it as the player's: never destroy it unattended. |
| `HAILSTONE_CURSOR_ORIGIN_INVENTORY` (1) | the player picked it up from their own inventory: a click on a slot, or part of a stack split off with the quantity slider. |
| `HAILSTONE_CURSOR_ORIGIN_SERVER` (2) | the server put it on the empty cursor. |

UNKNOWN also covers an item on the cursor at the zone-in (OP_CharInventory), one that replaced an item already on the
cursor, and any other path the client does not mark.

SERVER goes by how the item arrives, not whether the player chose it: summoned items, tradeskill results and quest
rewards; anything EQEmu queues on the cursor (item packet type 0x6a, and the next queued item moved up with
OP_MoveItem); ground items clicked up and loot onto the cursor (item packets of type 0x67 to the empty cursor). Checked:
`Client::SummonItem`, `Client::PushItemOnCursor`, `Object::HandleClick`, `Client::PutLootInInventory` (EQEmu).

An item packet or server OP_MoveItem onto an occupied cursor (a merge, a swap) reads UNKNOWN, and so does any later
change to the cursor by an unmarked path: never an earlier item's origin.

Read it beside `get_item` on the cursor, then pass the id just read to one of these; each does nothing unless the
cursor still holds an item of that id (two items of one id look the same).

- `/destroycursor <id>`: destroys the item on the cursor (OP_MoveItem to nowhere, as `/destroy` without its
  question). Never coin, never a bag (it may hold other items), never an item with a Keep mark, and only in a zone.
  It does not refuse a real-estate item (docs/unknowns.md).
- `/stowcursor <id>`: puts the item on the cursor into the first free general slot, else the first bag with room
  that takes it, never a worn slot; "There are no open slots for the held item in your inventory." when there is none.

Both need `actions`, as `/destroy` does. The id is a plain decimal number above 0; anything else does nothing.

#### Buff, spawn, extended target, ability and raid records

The calls: `get_buff_data_bulk(host, out, out_size)`, `get_pet_buff_data_bulk(host, out, out_size)`,
`get_spawns_data_bulk(host, out, out_size)`, `get_xtarget_data_bulk(host, out, out_size)`,
`get_target_buff_data_bulk(host, out, out_size)`, `get_avail_aa_data_bulk(host, out, out_size)`,
`get_avail_disc_data_bulk(host, out, out_size)`, `get_buff_name_data_bulk(host, out, out_size)`,
`get_ability_data_bulk(host, out, out_size)`, `get_raid_data_bulk(host, out, out_size)`,
`get_spells_data_bulk(host, ids, id_count, out, out_size)` and
`get_spells_by_name_data_bulk(host, name, out, out_size)`.

- They need `read`; without it they return 0 and say so in the chat window.
- They came later in ABI 3: test `HAILSTONE_HOST_HAS(host, <field>)` first.
- `out` is an `unsigned char*`, `out_size` an `int32_t`. They return the bytes written.

**The records' layout.** The SDK's packed structs (`#pragma pack(push, 1)`): `HS_Bulk_BuffData`,
`HS_Bulk_PetBuffData`, `HS_Bulk_SpawnHeader`, `HS_Bulk_SpawnFixedRecord`, `HS_Bulk_XTargetData`,
`HS_Bulk_TargetBuffFixedRecord`, `HS_Bulk_BuffNameFixedRecord`, `HAILSTONE_ID_NAME_RECORD`, `HS_Bulk_AbilityRecord`,
`HS_Bulk_ListHeader`, `HS_Bulk_RaidHeader`, `HS_Bulk_RaidData`, `HS_Bulk_SpellFixedRecord`. Machine byte order (little-endian on every platform
Hailstone runs on), unaligned.

**Reading them.** The bulk reads are the one place a struct may be cast onto the buffer, for speed.

- Read each field through the packed struct (`rec->level`), which the compiler reads unaligned.
- Never keep a plain pointer or reference to a field (`&rec->x`): it may fault where reads must be aligned.
- `ByteReader` or `memcpy` work too.

**A buffer too small.** The reads that give a list of any length (`get_target_buff_data_bulk`,
`get_avail_aa_data_bulk`, `get_avail_disc_data_bulk`, `get_ability_data_bulk` and `get_raid_data_bulk`):

- write as many whole records as fit, in order;
- state `count` (records in this buffer) and `total` (records the client holds) in the header. `count < total`
  means `out_size` was too small; call again with a larger buffer;
- lead with `HS_Bulk_ListHeader` (`HS_BULK_LIST_HEADER_SIZE` = 8 bytes: i32 `count`, then i32 `total`), except the
  raid read, whose `HS_Bulk_RaidHeader` has `count` and `total` meaning the same;
- write nothing and return 0 below the header's size. With nothing to list, the header alone goes out, `count` 0 and
  `total` 0.

**The bulk buffer.** `acquire_bulk_buffer(host, &size)` lends the plugin its own bulk buffer, `HS_BULK_BUFFER_SIZE`
(2 MiB) bytes, to pass the reads as `out`, so it need not allocate a buffer of its own for each read.
`release_bulk_buffer(host)` ends the loan.

- Each plugin has its own: made at its first `acquire_bulk_buffer`, kept between calls, freed when the plugin unloads.
  No plugin waits on another.
- Outside the plugin's own callbacks, from another thread among them, `acquire_bulk_buffer` returns NULL and sets
  `size` to 0. `size` may be NULL.
- The pointer is the plugin's from `acquire_bulk_buffer` until the first of: `release_bulk_buffer`, the end of the
  callback it was acquired in, or the plugin's unload. Never keep it past that.
- Asked again meanwhile it gives the same buffer, also in one of the plugin's callbacks nested inside that one (its
  own command run with `DoCommand`, say). A `release_bulk_buffer` in such a nested callback does nothing: the outer
  callback still holds it. The nested callback's reads do write into the same bytes, so copy out what the outer one
  still needs before running code that may read into it.
- To keep two reads' results side by side, give the second the rest of the buffer: read the first into `buf`, then
  the second into `buf + n` with `size - n`, where `n` is the bytes the first returned.
- It is never zeroed: it holds whatever the plugin last left. Zero what you need zeroed yourself.
- It needs no permission; the reads still need `read`.
- It came later in ABI 3: test `HAILSTONE_HOST_HAS(host, acquire_bulk_buffer)` first.

```c
int32_t size = 0;
unsigned char* buf = host->acquire_bulk_buffer(host->host, &size);
if (buf) {
    int32_t n = host->get_spells_by_name_data_bulk(host->host, "Third Spire of Necromancy", buf, size);
    /* ... read the n bytes ... */
    host->release_bulk_buffer(host->host);
}
```

##### `get_buff_data_bulk`

One record of `HAILSTONE_BUFF_RECORD_SIZE` (29) bytes per buff slot, slot 0 first: the long buff slots, then the song
slots, then the one discipline slot, always last.

- RoF2 has 42 long buff slots (0 to 41), 20 song slots (42 to 61) and the discipline slot (62), 63 in all.
- `out_size` must hold every buff and song slot, 1798 bytes (62 records), or 0 comes back. The discipline slot comes
  too with room for one record more, 1827 bytes (63).
- The counts are the client's, not the SDK's: tell a record's kind by its last byte, not its position, and pass room
  to spare (4096 bytes holds 141 records).

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | spell id; 0 an empty slot, whose other fields are all 0 |
  | 4 | i32 | the caster's spawn id |
| 8 | i32 | tics left; see the note below |
  | 12 | i32 | hits left |
  | 16 | i32 | the spell's type: 0 detrimental, 1 beneficial, 2 beneficial group-only (0 for a spell not in spells_us.txt) |
  | 20 | i32 | counter type of the spell's first counter effect: -1 none, 0 disease, 1 poison, 2 curse, 3 corruption. An empty slot also has 0 here, so test the spell id before reading it as disease |
  | 24 | i32 | that effect's base: the counters the buff starts with (MacroQuest's `Spell.CounterNumber`); 0 with no counter effect |
  | 28 | u8 | the slot's window: 0 buff (`HAILSTONE_BUFF_SLOT_BUFF`, RoF2's slots 0 to 41), 1 song (`HAILSTONE_BUFF_SLOT_SONG`, 42 to 61), 2 discipline (`HAILSTONE_BUFF_SLOT_DISC`, the last slot, 62); 0 in an empty slot. Taken from the slot, not the spell |

Tics left is the server's count less the 6 s ticks since it came (none while the buff timers are on hold). A permanent
buff is negative, the server's count as sent, never counted down (EQEmu: -1 for duration formula 50, -4 for an aura's
buff, formula 51, -1000 for one a script made permanent). The counters left are not included.

##### `get_buff_name_data_bulk`

`get_buff_data_bulk` with the caster's name in place of the caster's spawn id, same order and fields.

- Each record is a fixed part (`HS_Bulk_BuffNameFixedRecord`, `HAILSTONE_BUFF_NAME_RECORD_SIZE` = 29 bytes) followed
  by the caster's name, `casterNameLength` bytes, no NUL. Walk the records one by one.
- An empty slot is 29 zero bytes with no name.
- `out_size` must hold every buff and song slot with names (at least 1798 bytes plus the names), or 0 comes back; the
  discipline slot comes too when it fits after them. 8192 bytes holds every slot with names of the server's 64-byte
  limit.

The name is the one the server last sent for the slot (`OP_BuffCreate`), without its trailing digits. EQEmu keeps it
while the player stays in the zone, even after the caster dies or leaves; after zoning only when the caster is a player
already in the new zone (never an NPC). So an empty name is common.

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | spell id; 0 an empty slot, whose other fields are all 0 (and no name follows) |
  | 4 | i32 | `casterNameLength`: the bytes of the caster's name that follow; 0 when the server sent none |
  | 8 to 28 | | as `get_buff_data_bulk`'s: tics left, hits left, spell type, counter type, counters, slot's window |
  | 29 | bytes | the caster's name, `casterNameLength` bytes |

##### `get_pet_buff_data_bulk`

One record of `HAILSTONE_PET_BUFF_RECORD_SIZE` (12) bytes per buff in the pet window, in the server's order (0 bytes
with no pet), as many as fit: i32 spell id, i32 tics left (as above), i32 the spell's type (as above). An entry with
spell id 0 is all zeros. The pet window's slot number is not included.

##### `get_spawns_data_bulk`

Every spawn in the zone: a 16-byte header, then one record per spawn.

- Records are added until fewer than 4096 bytes are left in `out`, or the next record (fixed part and seven strings)
  would not fit. Only whole records are written; the header's count says how many.
- Below 4096 bytes it returns 0. One record needs at least 4112 bytes, plus a few hundred bytes per spawn.

  Header (`HS_Bulk_SpawnHeader`, `HS_BULK_SPAWN_HEADER_SIZE` = 16 bytes):

  | Offset | Type | Field |
  |---|---|---|
  | 0 | u32 | the number of spawn records that follow |
  | 4 | f32 | the player's x (0 with no player) |
  | 8 | f32 | the player's y |
  | 12 | f32 | the player's z |

  Each record (`HS_Bulk_SpawnFixedRecord`) is `HS_BULK_SPAWN_RECORD_SIZE` = 92 fixed bytes:

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | spawn id |
  | 4 | u32 | flags (below) |
  | 8 | i32 | body type id |
  | 12 | i32 | class id |
  | 16 | i32 | consider colour, the one `/consider` and the target window show (the original's con level, with the server's PvP rules), as a chat colour (`CONCOLOR_*`: 6 grey, 2 green, 18 light blue, 4 blue, 10 white, 15 yellow, 13 red); 6 also for a corpse |
  | 20 | i64 | current HP: the HP percent, as the server sends it |
  | 28 | i32 | gender |
  | 32 | i64 | guild id |
  | 40 | f32 | heading in degrees (the server's heading x 0.703125) |
  | 44 | f32 | height (the model's avatar height) |
  | 48 | i32 | level |
  | 52 | i32 | master: the pet owner's spawn id, 0 for none |
  | 56 | f32 | max range: 14.5, or 0 for a ground item |
  | 60 | f32 | max range to: 14.5, or 0 for a ground item |
  | 64 | i32 | HP percent |
  | 68 | i32 | player state, as the server sends it |
  | 72 | i32 | race id |
  | 76 | f32 | x |
  | 80 | f32 | y |
  | 84 | f32 | z |
  | 88 | i32 | deity (tells a player's corpse from an NPC's) |

Then seven strings, each a u16 length and that many bytes, no NUL:

1. the body type's description;
2. the clean name;
3. the display name (the same text as the clean name, as `${Spawn.CleanName}` and `${Spawn.DisplayName}` give them);
4. the name;
5. the race's name;
6. the suffix;
7. the spawn type's description.

  Flags (bit: meaning): 0 AFK, 1 aggressive (player state 0x04 or 0x08), 2 anonymous, 3 buyer, 4 dead, 5 ducking,
  6 feigning death, 7 GM, 8 invisible, 9 summoned, 10 levitating, 11 link dead, 12 on a mount, 13 moving (a run speed
  other than 0), 14 named, 15 roleplaying, 16 sitting, 17 sneaking, 18 standing, 19 stunned (player state 0x20),
  20 targetable, 21 trader, 22 underwater. Bits 8, 17 and 22 are never set at present.

##### `get_xtarget_data_bulk`

One record (`HS_Bulk_XTargetData`) of `HS_BULK_XTARGET_RECORD_SIZE` (16) bytes per extended target slot, slot 0 first,
as many whole records as fit (0 bytes before the server sent the list). The slots are the ones `get_xtarget` reads by
index, including one the server's extension hides from the window. RoF2 has at most 20 slots (EQEmu: 5 plus the AA's),
so 512 bytes is plenty.

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | the slot's type, as `HailstoneXTarget.type`: an `HS_BULK_XTARGET_*` value (`enum HS_BULK_XTargetTypes`, the protocol's 0 to 26: 0 empty, 1 auto hater, 2 / 3 a specific PC / NPC, ... 25 / 26 the mercenary and its target) |
  | 4 | i32 | the slot's spawn id, 0 for none |
| 8 | i32 | the aggro meter's percent for the slot's spawn, as the server sent it; see the note below |
| 12 | i32 | the spawn's HP percent, as the extended target window draws it; see the note below |

- **Aggro percent:** the meter's entries 7 to 26 (the window's aggro labels). On EQEmu an NPC's is our hate against
  its current target's, a player's its hate against our target's top. 0 with no spawn in the zone, without the meter
  (EQEmu's `Character:EnableAggroMeter`), or past the 20th slot.
- **HP percent:** the player's own from its points (items' HP included), anyone else's as the server last sent it
  (EQEmu sends it when the percent changes). 0 with no spawn in the zone or for a corpse.

##### `get_target_buff_data_bulk`

The current target's buffs, as the target window draws them (what `OP_TargetBuffs` last gave for that spawn): an
`HS_Bulk_ListHeader`, then one record per buff, in the client's order, each with its slot.

- EQEmu sends only filled slots, in slot order, and resends the whole list when one fades: the records are packed and
  slot numbers can skip.
- An entry with no spell is left out and not counted in `total`.
- The read checks only that the list is the current target's. With the player itself targeted it gives whatever
  `OP_TargetBuffs` gave for the player, although the client's own target buff list draws nothing then.
- Each record is a fixed part (`HS_Bulk_TargetBuffFixedRecord`, `HAILSTONE_TARGET_BUFF_RECORD_SIZE` = 20 bytes)
  followed by the caster's name, `casterNameLength` bytes, no NUL. Walk them one by one, not by a fixed stride.
- Whole records only, none after the first that does not fit. Below 8 bytes nothing is written and 0 comes back.
- The header alone goes out (count 0, total 0, 8 bytes) with no target, no game connection, or while the server has
  not yet sent the new target's buffs. EQEmu sends them only where the target window shows buffs, so an NPC target on
  a server without that shows none.
- Size for the worst case: EQEmu gives an NPC 60 buff slots by default (`Spells:MaxTotalSlotsNPC`) and a caster's name
  at most 63 bytes, so 8 + 60 x 83 = 4988 bytes. 8192 bytes holds any target on a stock server.

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | `count`: the records that follow in this buffer |
  | 4 | i32 | `total`: the number of the target's buffs the client holds |
  | 8 | records | the records, one after another, as many as fit |

  Each record:

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | spell id (never 0: entries with no spell are left out) |
| 4 | i32 | tics left; see the note below |
  | 8 | i32 | the spell's type: 0 detrimental, 1 beneficial, 2 beneficial group-only (0 for a spell not in spells_us.txt) |
  | 12 | i32 | the target's buff slot, as the server sent it (EQEmu: the mob's own slot index, not mapped to RoF2's) |
  | 16 | i32 | `casterNameLength`: the bytes of the caster's name that follow; 0 when the server sent none |
  | 20 | bytes | the caster's name, `casterNameLength` bytes |

Tics left is the server's count less the 6 s ticks since it came, held as the target window holds it: a suspendable
spell in a zone that suspends buffs, and, with the player its own target, the player's buff-timer hold
(docs/mechanics.md). A permanent buff is negative, as above.

##### `get_avail_aa_data_bulk` and `get_avail_disc_data_bulk`

An `HS_Bulk_ListHeader`, then `count` records, each an id and its name.

- Each record is a fixed part (`HAILSTONE_ID_NAME_RECORD`, `HAILSTONE_ID_NAME_RECORD_SIZE` = 8 bytes) followed by the
  name, `nameSize` bytes, no NUL. Walk them one by one.
- Whole records only, in order, none after the first that does not fit. Below 8 bytes nothing is written and 0 comes
  back.
- With nothing to list, before the character's profile, or with no game connection, the header alone goes out (count
  0, total 0, 8 bytes).
- RoF2's lists have at most 300 entries each; 16384 bytes holds either with ordinary names.

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | `count`: the records that follow in this buffer |
  | 4 | i32 | `total`: the entries the list holds, written or not |
  | 8 | records | the records, one after another |

  Each record:

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | the id (below) |
  | 4 | i32 | `nameSize`: the bytes of the name that follow; 0 when the client has no name for it |
  | 8 | bytes | the name, `nameSize` bytes |

`get_avail_aa_data_bulk` lists the alternate abilities the character owns (bought or granted, passive and activated),
in the owned list's order (the profile's, then `OP_RespondAA`'s).

- The id is the ability (group) id every rank shares and `aa_value` takes, not the rank id the server lists.
- The name is the owned rank's title (dbstr_us.txt type 1, as the AA window shows it), empty when the game folder's
  file lacks it.
- An owned rank the AA table does not hold (not yet in `OP_SendAATable`) has no known group id and is left out, as
  `aa_value` gives 0 for it.

`get_avail_disc_data_bulk` lists the character's combat abilities (disciplines) by spell id: the profile's discipline
list that `combat_ability` reads by index, without its empty (0) entries, in list order. The name is the spell's, empty
for a spell not in spells_us.txt.

##### `get_ability_data_bulk`

The Ability Book's usable entries with their readiness in one call, instead of `ability_state` per entry.

- An `HS_Bulk_ListHeader`, then `count` records: an `HS_Bulk_AbilityRecord` (`HAILSTONE_ABILITY_RECORD_SIZE` = 32
  bytes) followed by the name, `nameSize` bytes, no NUL.
- Whole records only, in the book's order: the owned activated AAs, the listed disciplines, then the usable skills
  (the combat page's, then the abilities page's). Passive entries are not listed.
- An owned AA rank the AA table does not hold is left out, as in `get_avail_aa_data_bulk`.
- Match an entry by `kind` and `id`, not by its position.

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | `kind`: `HS_ABILITY_SKILL` (0), `HS_ABILITY_DISCIPLINE` (1) or `HS_ABILITY_ALT` (2) |
  | 4 | i32 | `id`: a skill's id, a discipline's **spell id** (not its profile slot), or an AA's ability (group) id |
  | 8 | i32 | `ready`: 1 its reuse has run out, 0 on reuse |
  | 12 | i64 | `remainingMs`: until it is ready, 0 when ready |
  | 20 | i64 | `reuseMs`: its reuse time |
  | 28 | i32 | `nameSize`: the bytes of the name that follow; 0 when the client has no name for it |
  | 32 | bytes | the name, `nameSize` bytes |

- **Readiness** is what `ability_state` gives: an AA's own reuse (SPA 264 reduction applied), a discipline's reuse
  timer, a skill's timer rounded up to the whole second the book shows; all in ms.
- Before the character's profile, or with no game connection, the header alone goes out (count 0, total 0).
- **Size.** At most 28 activated skill ids in the client's table, 300 discipline slots, and the owned AAs the server
  sends (300 on a stock server). 65536 bytes holds every entry with ordinary names.

##### `get_raid_data_bulk`

The raid roster: a 40-byte header, then one `HS_Bulk_RaidData` record (`HS_BULK_RAID_RECORD_SIZE` = 124 bytes) per
member, in roster order, as many whole records as fit.

- The header is written whenever `out_size` holds it. Outside a raid, `active` 0 and `count` 0.
- Records are 0-based (`slot` 0 is the first member; MacroQuest's `Raid.Member[1]` is the first). `leaderIndex` and
  `targetIndex` are 0-based too, `HS_BULK_RAID_NO_INDEX` (0xffffffff) for none.
- Start the records at the header's `size` and step by its `recordSize`, for `count` records. A later client may
  append fields: never use a fixed stride, and read only the fields `recordSize` covers.
- `version` changes only when an older reader can no longer read the header or a record at all.
- `total` is the roster's member count; `count < total` means retry with a larger buffer.

  Header (`HS_Bulk_RaidHeader`, `HS_BULK_RAID_HEADER_SIZE` = 40 bytes):

  | Offset | Type | Field |
  |---|---|---|
  | 0 | u32 | `sizeof(HS_BulkRaidHeader)` |
  | 4 | u32 | version, 1 |
  | 8 | u32 | `count`: member records that follow (what fit in `out_size`) |
  | 12 | u32 | bytes between records; a later client may grow it |
  | 16 | u32 | leader's 0-based member index, or `HS_BULK_RAID_NO_INDEX` |
  | 20 | u32 | the raid window's selected member, same encoding |
  | 24 | i32 | in a raid (1) or not (0) |
  | 28 | i32 | locked (1) or not (0) |
  | 32 | i32 | loot type, as the raid window names it: 1 Leader Only, 2 Leader and Group Leader, 3 Leader Selected, 4 Entire Raid |
  | 36 | u32 | `total`: the roster's member count, not only those written |

  Each record (`HS_Bulk_RaidData`, `HS_BULK_RAID_RECORD_SIZE` = 124 bytes):

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | roster slot, 0-based |
  | 4 | u32 | spawn id; 0 when the member is not in this zone |
  | 8 | i32 | the server's raid group 0..11; -1 ungrouped or any other value |
  | 12 | u32 | flags (below) |
  | 16 | i32 | level; 0 when unknown |
  | 20 | i32 | class id; 0 when unknown |
  | 24 | i32 | HP percent; -1 when unknown |
  | 28 | i32 | mana percent; -1 when unknown (the client has it only for group members) |
  | 32 | i32 | endurance percent; same |
  | 36 | u32 | pet spawn id (the pet window's for the player, else a spawn the member owns that is neither a mercenary nor a temporary pet); 0 none |
  | 40 | i32 | pet HP percent; -1 unknown or no pet |
  | 44 | f32 | x (the zone's coordinates, as `HailstoneSpawn.x`) |
  | 48 | f32 | y |
  | 52 | f32 | z |
  | 56 | f32 | 3D distance from the player; 0 when the member is not here |
  | 60 | char[64] | member name, NUL-terminated |

  Flags (bit: meaning): 0 leader, 1 group leader, 2 main assist, 3 raid marker (the member holds the Marker role),
4 waypointer, 5 looter, 6 reserved (this client keeps no offline flag), 7 present in this zone, 8 the player.

- Main assist: the member's own flag first, then the raid's assigned assist names.
- A member not in the zone has no spawn id, position or percentages; level and class still come from the roster.
- `LOOTER` answers "may this member loot under the current loot type" (1 Leader Only, 2 Leader and Group Leader,
  3 Leader Selected, 4 Entire Raid). The raid window's own `looter()` check for the member menu differs: it tests only
  the leader and the listed looters.

##### `get_spells_data_bulk`

`get_spells_data_bulk(host, ids, id_count, out, out_size)`: spells by id in one call, each with its effect slots
  decoded to text, so a plugin needs neither `get_spell` once per spell nor its own copy of the effect rules. `ids`
  points at `id_count` spell ids (`const int32_t*`). `out` gets an i32 `count` (`HS_BULK_SPELL_HEADER_SIZE` = 4
  bytes), then one record per id, in the order asked, as many whole records as fit: `count` below `id_count` means
  `out_size` was too small for the next one, so call again for the rest. Each record is an
  `HS_Bulk_SpellFixedRecord` (`HS_BULK_SPELL_RECORD_SIZE` = 265 bytes) and, for a spell the client holds, the
  strings after it; step from record to record by its `recordSize`, never a fixed stride. An id the client has no
  spell for (not in spells_us.txt, 0 or negative) gets the fixed record alone, `found` 0 and the rest 0.

  Fixed record (`HS_Bulk_SpellFixedRecord`, `HS_BULK_SPELL_RECORD_SIZE` = 265 bytes). The column is the field's in
  spells_us.txt, as the client reads it into `game::SpellData`; an array's columns run on from the one given.

  | Offset | Type | Field | Column |
  |---|---|---|---|
  | 0 | i32 | `recordSize`: this record's bytes, everything after it included (slots, class levels, strings, lines); the next record starts here | |
  | 4 | i32 | `id`: the spell id asked for | 0 |
  | 8 | i32 | `found`: 1 the client holds the spell, 0 it does not (every field after is 0 and nothing follows) | |
  | 12 | f32 | `range` | 9 |
  | 16 | f32 | `aeRange` | 10 |
  | 20 | f32 | `pushBack` | 11 |
  | 24 | f32 | `pushUp` | 12 |
  | 28 | i32 | `castTime`, ms | 13 |
  | 32 | i32 | `recoveryTime`, ms | 14 |
  | 36 | i32 | `recastTime`, ms | 15 |
  | 40 | i32 | `durationFormula` | 16 |
  | 44 | i32 | `durationCap`, ticks | 17 |
  | 48 | i32 | `aeDuration` | 18 |
  | 52 | i32 | `mana` | 19 |
  | 56 | i32 | `bookIcon` | 56 |
  | 60 | i32 | `gemIcon` | 57 |
  | 64 | i32[4] | `reagentId` | 58..61 |
  | 80 | i32[4] | `reagentCount` | 62..65 |
  | 96 | i32[4] | `noExpendReagent` | 66..69 |
  | 112 | i32 | `spaIndex` | 123 |
  | 116 | u32 | `deityMask`: bit n set when column 125 + n is; 0 any deity | 125..141 |
  | 120 | i32 | `spellIcon` | 144 |
  | 124 | i32 | `spellAnim` | 145 |
  | 128 | i32 | `resistAdj` | 147 |
  | 132 | i32 | `recourseLink` | 150 |
  | 136 | i32 | `descriptionId` (dbstr type 6; its text is the ninth string) | 155 |
  | 140 | i32 | `category` (dbstr type 5; its name is the twelfth string) | 156 |
  | 144 | i32 | `subcategory` | 157 |
  | 148 | i32 | `subcategory2` | 158 |
  | 152 | i32 | `enduranceCost` | 166 |
  | 156 | i32 | `reuseTimerIndex` | 167 |
  | 160 | i32 | `enduranceUpkeep` | 174 |
  | 164 | i32 | `hitCountType` | 175 |
  | 168 | i32 | `hitCount` | 176 |
  | 172 | i32 | `globalGroup` | 180 |
  | 176 | i32 | `pcnpcOnly` | 183 |
  | 180 | i32 | `spellGroup` | 207 |
  | 184 | i32 | `spellRank` | 208 |
  | 188 | i32 | `spellSubgroup` | 236 |
  | 192 | i32 | `reqAssociation` | 211 |
  | 196 | i32 | `spellClass` | 221 |
  | 200 | i32 | `spellSubclass` | 222 |
  | 204 | i32 | `maxTargets`: an AE's target cap, 0 none | 218 |
  | 208 | f32 | `distanceModStartX` | 227 |
  | 212 | f32 | `distanceModStartMod` | 228 |
  | 216 | f32 | `distanceModEndX` | 229 |
  | 220 | f32 | `distanceModEndMod` | 230 |
  | 224 | f32 | `minRange` | 231 |
  | 228 | u32 | `flags`: the yes/no columns, below | |
  | 232 | i32 | `slotCount`: the effect slots after this record, 12 on RoF2; 0 for `found` 0 | |
  | 236 | i32 | `slotSize`: the bytes between slots, `HS_BULK_SPELL_SLOT_SIZE` (20) today; step by it | |
  | 240 | i32 | `classCount`: the class levels after the slots, one byte each, 16 on RoF2; 0 for `found` 0 | |
  | 244 | i32 | `stringCount`: the strings after the class levels, `HS_BULK_SPELL_STRINGS` (14) today; 0 for `found` 0 | |
  | 248 | i32 | `effectCount`: the effect lines after the strings | |
  | 252 | u8 | `lightType` | 82 |
  | 253 | u8 | `spellType`: 0 detrimental, 1 beneficial, 2 beneficial group-only | 83 |
  | 254 | u8 | `activated` | 84 |
  | 255 | u8 | `resist` | 85 |
  | 256 | u8 | `targetType` | 98 |
  | 257 | u8 | `castDifficulty` | 99 |
  | 258 | u8 | `skill` | 100 |
  | 259 | u8 | `zoneType` | 101 |
  | 260 | u8 | `environment` | 102 |
  | 261 | u8 | `timeOfDay` | 103 |
  | 262 | u8 | `castingAnim` | 120 |
  | 263 | u8 | `targetAnim` | 121 |
  | 264 | u8 | `travelType` | 122 |
  | 265 | | the effect slots, the class levels, then the strings | |

  `flags` (`HS_BULK_SPELL_*`, bit: column):

  | Bit | Flag | Column |
  |---|---|---|
  | 0 | `CANCEL_ON_SIT` | 124 |
  | 1 | `UNINTERRUPTABLE` | 146 |
  | 2 | `NOT_STACKABLE_DOT` | 148 |
  | 3 | `DELETABLE` | 149 |
  | 4 | `DURATION_WINDOW`: shown in the short buff (song) window | 154 |
  | 5 | `NPC_NO_LOS` | 159 |
  | 6 | `IS_SKILL` | 168 |
  | 7 | `CAST_NOT_STANDING` | 184 |
  | 8 | `NO_DISPELL` | 186 |
  | 9 | `SNEAK` | 196 |
  | 10 | `NOT_FOCUSABLE` | 197 |
  | 11 | `STACKS_WITH_SELF` | 203 |
  | 12 | `SUSPENDABLE` | 200 |
  | 13 | `BYPASS_REGEN_CHECK` | 212 |
  | 14 | `CAN_CAST_IN_COMBAT` | 213 |
  | 15 | `CAN_CAST_OUT_OF_COMBAT` | 214 |
  | 16 | `NO_REMOVE` | 232 |
  | 17 | `ONLY_DURING_FAST_REGEN` | 234 |

  After a found spell's fixed record come `slotCount` effect slots, slot 1 first, each an `HS_Bulk_SpellSlot` and
  `slotSize` bytes from the next. The slots are counted rather than fixed arrays, so a server that offers more than
  RoF2's 12 can send them (an extension past RoF2; a stock server's spells have 12). Step by `slotSize` and read only
  the fields it covers: a later client may append some.

  | Offset | Type | Field | Column for slot n (1..12) |
  |---|---|---|---|
  | 0 | i32 | `effect`: the SPA id, 254 unused | 85 + n |
  | 4 | i32 | `base` | 19 + n |
  | 8 | i32 | `base2` (the limit) | 31 + n |
  | 12 | i32 | `max` | 43 + n |
  | 16 | i32 | `formula` (the value formula) | 69 + n |

  Then come `classCount` class levels, one u8 each, class 1 first (column 103 + n for class n): the level class n
  gets the spell at, 255 or 0 when it cannot use it. Counted for the same reason as the slots: a server with more
  than RoF2's 16 classes can send them (an extension past RoF2).

  Then come `stringCount` strings and then `effectCount` effect lines,
  each a u16 length and that many bytes with no NUL, as `get_spawns_data_bulk`'s strings:

  | # | String | Column |
  |---|---|---|
  | 1 | the name | 1 |
  | 2 | the target or model text | 2 |
  | 3 | the teleport zone or pet name (MacroQuest's `Extra`) | 3 |
  | 4 | the cast-by-me text | 4 |
  | 5 | the cast-by-other text | 5 |
  | 6 | the cast-on-you text | 6 |
  | 7 | the cast-on-other text | 7 |
  | 8 | the wear-off text | 8 |
  | 9 | the description, dbstr_us.txt's type 6 text for `descriptionId`: for one of the player's own spells (in the spell book, in the discipline list, or the spell an owned AA casts), as the spell display window shows it to the player, its `#n`, `$n`, `@n` and `%z` codes filled in for the player's level, class and effects; for any other spell, as the file has it, the codes left in. Empty when the game folder's file has none | |
  | 10 | `targetTypeDesc`: the target type's name, MacroQuest's `${Spell.TargetType}` text (`Single`, `Group v2`, ...), `Unknown` for a type it does not name | 98 |
  | 11 | `resistTypeDesc`: the resist type's name, MacroQuest's `${Spell.ResistType}` text (`Unresistable` for 0, `Magic`, `Fire`, `Cold`, ...), `Unknown` for a type it does not name | 85 |
  | 12 | the category's name: dbstr_us.txt type 5, as MacroQuest's `${Spell.Category}` gives it, `Unknown` for 0 or one the game folder's file lacks | 156 |
  | 13 | the subcategory's name, the same way | 157 |
  | 14 | the second subcategory's name, the same way | 158 |
  | 15.. | any strings a later client appends, up to `stringCount`: skip the ones you do not know | |
  | then | the effect lines, `effectCount` of them | |

  Read `stringCount` strings, never a fixed number: the effect lines start after the last of them, so a plugin
  built against this header keeps reading a later client's records correctly.

  The effect lines are MacroQuest's `ParseSpellEffect` text for each slot that has one, in slot order,
  `Slot <n>: ...` (for example `Slot 1: Increase HP by 11 (L1) to 20 (L10)`); a slot with nothing to show (SPA 254,
  the teleport's location slots) has no line. The value ranges run to level 100, as MacroQuest's `/spellslotinfo`
  shows them. Spell names in the text are the client's (`Unknown Spell` for one it has not); skill and faction names
  come from the game folder's string tables (`Unknown[n]`, `Unknown Faction[n]` without one); a summoned item reads
  `[  id]`, since the client keeps no item database. The text is ported from MacroQuest
  (game/spell_effect_text.cpp).

  Below 4 bytes nothing is written and 0 comes back; with no game connection, or no ids, the count alone (0). A
  plugin without `read` gets 0 bytes. A record is 265 bytes plus 240 for its 12 effect slots, 16 for its class levels, its texts and lines, a few hundred bytes for a
  stock spell, so about 1500 bytes per id is ample. Added later in ABI 3: test
  `HAILSTONE_HOST_HAS(host, get_spells_data_bulk)` first.

##### `get_spells_by_name_data_bulk`

`get_spells_by_name_data_bulk(host, name, out, out_size)`: every spell named `name` (a C string, matched without
  regard to case, the whole name only) in one call, so a plugin need not know the ids of a spell's ranks or copies.
  Several spells often share a name (Third Spire of Necromancy is 16290, 16291 and 16292); they come lowest id first.
  The bytes are exactly those `get_spells_data_bulk` writes for those ids: an i32 `count`, then as many whole
  records as fit, laid out as above. A name no spell has, or a NULL one, gives the count alone (0), as does no game
  connection; below 4 bytes nothing is written and 0 comes back, and a plugin without `read` gets 0 bytes. Size
  `out` as for `get_spells_data_bulk`, about 1500 bytes per spell of the name. Added later in ABI 3: test
  `HAILSTONE_HOST_HAS(host, get_spells_by_name_data_bulk)` first.

### Connections (ABI 2)

A plugin with the `net` permission reaches a server's own TCP service through the client's sockets, so it needs no
socket code and works the same on every platform. Nothing waits on the network:

| Function | What |
|---|---|
| `tcp_open(address, port)` | starts connecting without waiting: a handle above 0, or 0 (no `net`, already `HAILSTONE_TCP_MAX` = 8 open); see below |
| `tcp_state(handle)` | 1 connected, 0 still connecting (at most 10 s an address), -1 closed (refused, no answer, dropped, closed by the other side) |
| `tcp_send(handle, data, size)` | takes every byte (what the system cannot take yet waits in the client and goes out at the next `tcp_*` call) and returns the count, or -1 once closed |
| `tcp_recv(handle, out, size)` | what has arrived: the count, 0 for nothing yet, -1 once closed with nothing left |
| `tcp_error(handle, out, size)` | why it closed, as text |
| `tcp_close(handle)` | ends it; unloading a plugin closes its others |

**Names and addresses.** A name is looked up in the background (at most four lookups at once, the rest queued) while
the handle connects. One that does not resolve shows `tcp_state` -1, with `tcp_error` saying why. Each address is
tried in turn (IPv4 first); one refused or silent for 10 s hands over to the next, and the last one's failure closes
the handle.

Call `tcp_recv` each pulse; that also sends what was waiting.

- `net` reaches any address, this computer and the player's network too (a home server is often there). A compiled
  plugin is native code and could connect without the client anyway, so a refusal would hold nothing.
- A server's `hailstone.cfg` grants it to the player's plugins only when it says so.
- When the plugin loses `net` (a narrower policy, a lowered ceiling) its connections close; later `tcp_*` calls see
  -1.

### Plugin windows (ABI 2)

A plugin's windows are Lua files in its folder, one set per UI pack:

```
plugins/MyPlugin/
  MyPlugin.so
  ui/modern/mywindow.lua      the Modern pack's window
  ui/modern/lib/shared.lua    dofile("lib/shared.lua") from the plugin's own folder
  ui/gamepad/mywindow.lua     the Gamepad pack's
```

- A relative `dofile` looks in the plugin's folder first, then the pack's, so a window can use the pack's libraries
  (`dofile("lib/hud.lua")` in Modern, `lib/theme.lua` in Gamepad). Nothing outside those two folders (no absolute
  path, no `..`).
- Every UI Lua file is loaded as text: precompiled chunks are refused (`load` too), and there is no `loadfile`.

#### Covering a pack's window

A plugin's window with the name of a pack's (`ui.window("Pet", ...)`) replaces it: only the plugin's draws.

- The pack's comes back when the plugin unloads; reloading the pack's file does not take the name back.
- With several plugins, the later-loaded covers the earlier.
- In its draw function, `ui.base_window()` draws the covered window and returns true (false when it covers none), so
  a plugin can leave the stock window while it has nothing to add:

```lua
local p = hs.Plugin("MyPlugin")
ui.window("Pet", function()
  local state = p and p.pets()       -- nil until the plugin has seen its server
  if not state then return ui.base_window() end
  ...                                -- the plugin's own Pet window
end)
```

#### Adding to a pack's window

**Notices and unavailable settings.** Add-ons with `ui` and `extend` may contribute a notice or disable an
individual control at a location supported by the interface. The interface renders the text and attributes it to
the add-on. A setting stays disabled for as long as any active contribution disables it; each add-on can update
or remove only its own contribution. Saved values are retained. These are UI controls, so programmatic setting
changes and the game's own settings logic still use their usual rules.

```lua
local hs = require('hailstone')
if hs.ui_contribution then
    hs.ui_contribution{
        id = 'nameplate-settings',
        window = 'OptionsWindow',
        target = 'section:nameplates',
        text = 'Use the add-on settings to customize these nameplates.',
        action_label = 'Open settings',
        command = '/myaddon settings',
    }
end
-- When the feature becomes inactive:
if hs.remove_ui_contribution then hs.remove_ui_contribution('nameplate-settings') end
```

- `id` identifies a contribution within its owner. Calling `ui_contribution` again with the same ID replaces it.
  Unloading, deactivating or losing `ui` or `extend` removes the owner's contributions.
- Omit `target` for a window notice. Stock Options also presents `section:display`, `section:graphics`,
  `section:nameplates`, and `setting:<id>` for supported controls. The IDs are the setting keys the Options scripts
  pass to `hs.UI.settings.Option`, such as `setting:render_scale` and `setting:target_plate_highlight`.
- Set `disabled = true` for a supported individual control. Supply `text` for a custom explanation or omit it to use
  the stock one. Three rows are the player's way out of an add-on or a broken pack and are never disabled:
  `setting:upscaler` (an upscaler add-on), `setting:ui_name` (a UI pack, or one that stopped working) and
  `setting:texture_pack` (a texture pack, such as one a tool add-on built). A disabling entry for one of them is
  refused whole (`ui_contribution` returns false, `set_ui_contribution` 0, and an upscaler's record is left out of
  its snapshot), in any window and from any owner; a notice without `disabled` is still allowed there. An optional action supplies
  both `action_label` and a slash `command`. The player's activation dispatches the command with the add-on's
  current capabilities, as with its Plugins menu entries.
- Native plugins use `set_ui_contribution` and `remove_ui_contribution`, appended to ABI 3. Check
  `HAILSTONE_HOST_HAS(host, set_ui_contribution)` and use the sized `HailstoneUiContribution` record from
  `sdk/hailstone_ui_contribution.h` (56 bytes in a 64-bit build; the client needs `size` to cover `command`). The
  host copies the strings during registration. See
  [the pack API](ui-api.md#ui-contributions) for the presenter contract.

Some pack windows live in a library that takes the plugin's part, so a plugin adds to them without copying.

**Pet.** Both packs (`ui/<pack>/lib/pet.lua`; each file's head lists its options).

- Modern: `pet.window{extra = {height, draw}, fold = <setting>, width = n}` adds a part above the command buttons and
  can fold them.
- Gamepad: gives the window's pieces (`P.visible`, `P.status`, `P.commands`, `P.buffs`) to lay out.

**Extended target.** Modern (`ui/modern/lib/xtarget.lua`):
`xtarget.window{width = n, row = {width = n, draw = fn(r, x0, y0, x1, y1, s), style = fn(r)}}`.

- `row.width` pixels at the right of each filled row are the plugin's. Its buttons take their own clicks; a click on
  the rest still targets.
- `style` can fade a row, outline it, strike its name through or put a small label after it.
- `r.n` is the slot (0-based: `hailstone.xtarget(r.n + 1)` in a script).
- Every slot has a row, but `draw` and `style` are called for filled rows only.
- The rows sit in a clipping, scrolling child of the frame (`width` is the frame's until the player resizes it); the
  coordinates `draw` gets already follow the scroll.

A window named "Extended Target" that calls it covers the pack's:

```lua
local ok, xtarget = pcall(dofile, "lib/xtarget.lua")   -- an older client has no lib/xtarget.lua
ui.window("Extended Target", function()
  local p = hs.Plugin("MyScript")
  if not ok or not p then return ui.base_window() end
  xtarget.window{ width = 250, row = { width = 50, draw = function(r, x0, y0, x1, y1, s) ... end } }
end)
```

**Alternate Advancement.** Modern (`ui/modern/lib/aa.lua`):
`aa.window{row = {width = n, draw = fn(r, x0, y0, x1, y1, s), style = fn(r)}, details = fn(sel, x, y, w, s), banner = fn(x, y, w, s)}`.

- `row` works as the extended target window's.
- `r` has the ability's `name`, `rank`, `max`, `cost` (the next rank's, as text), `category`, `trainable`, `maxed` and
  `page` (its tab, 0-based; the window lists every tab as one list).
- `details` draws under the selected ability's requirements (`sel`: `id`, `name`, `rank`, `max`, `cost`); `banner`
  draws a strip under the header. Each returns the height it took.
- Rows carry no ability id: match `r.name` to `hailstone.alt_abilities()`'s names (ranks share a name).
- A window named "Alternate Advancement" that calls it covers the pack's.

#### Loading, data and actions

The windows load with the player's pack while the plugin is loaded (it needs the `ui` capability) and go when it
unloads. A pack without a folder of its own shows none of them. They are written like the pack's windows
(`ui.window`, the `ui` and `hs` tables). A server ships them in its `client_files/plugins/<name>/ui/` (Lua is taken
there and in `scripts/` only).

The windows get data and send actions through functions the plugin exports:

```lua
local p = hs.Plugin("MyPlugin")        -- nil while it is not loaded
local rows = p and p.rows("open")      -- or p.call("rows", "open")
```

In the plugin, `add_ui_function("rows", fn, user)`. `fn` reads the arguments (`arg`, and `arg_field` for a table
argument's field) and builds the result: a value, or arrays and tables opened with `out_begin_array` /
`out_begin_table`, filled (in a table, `out_key` before each value) and closed with `out_end`. A key written as a whole
number (`"12"`) is the Lua integer 12.

Values cross both ways as plain data:

- tables nested deeper than 16 levels are cut there (nil);
- a value with a table inside itself, more than 100 000 values or more than 16 MiB of text is nil as a whole, with a
  log line;
- a call passes at most 64 arguments; one with more gives nil and does not reach the plugin.

It can also add:

- **options**: `add_option(name, label, min, max, get, set, user)`. A switch when max is 1, else a slider. The packs'
  Options windows list them under the plugin's name (Modern: a Plugins tab; Gamepad: a Plugins page;
  `hs.PluginOptions`, `hs.SetPluginOption`); the plugin keeps the value.
- **menu entries**: `add_menu(label, command)`. The EQ menu (a Plugins submenu) and the gamepad pack's More page list
  them (`hs.PluginMenu`, `hs.PluginMenuClick`). Choosing one runs the command with the plugin's capabilities, even
  though the player chose it: one that needs more than the plugin may do is refused.

Limits per owner (a compiled plugin or a script): 256 UI functions, 64 options and 32 menu entries
(`UiBridge::kMaxFunctions`, `kMaxOptions`, `kMaxMenuEntries`).

- Past them `add_ui_function`, `add_option` and `add_menu` return 0, and the `hailstone` module's `ui_function`,
  `option` and `menu` raise an error.
- A function or option of a name the owner already has replaces it, and a menu entry of a label it already has
  changes that entry's command, without counting twice: a plugin that adds its entry at each load or zone keeps one.

#### Owners

Each window folder belongs to an owner, told apart by kind and name: `plugin:<name>` for a compiled plugin's,
`script:<name>` for a Lua plugin's (a script's `ui/`).

- A script named after a compiled plugin (a script add-on called `nms` beside the NMS server's plugin) gets none of
  the plugin's capabilities, folder, functions, options or menu entries.
- `hs.Plugin(name)` with a bare name finds a compiled plugin's functions before a script's; `hs.Plugin("script:<name>")`
  names the script's.
- `hs.PluginOptions` and `hs.PluginMenu` give each entry's `owner` beside its `plugin` name; `hs.SetPluginOption` takes
  either.
- The add-on manager installs no script with a compiled plugin's name in the player's folders, nor the other way round
  (docs/addons.md).

#### What a plugin's window code may do

These windows share the pack's Lua state but not its authority. A plugin folder's code (its files' top level, its
windows, any function it took from `hs` or `ui`, whoever calls it later) runs with the plugin's capabilities for
whatever it does through the client: a command via `hs.ChatSubmit`, a script it starts, a web page.

A server's own plugins and scripts have every capability, so their windows can do what the pack's do. A player's
add-on gets at most what its manifest declared (docs/addons.md). Whatever its capabilities, a plugin's code:

- **has globals of its own.** It reads the pack's globals (shared state tables such as `modern_colors` or `GPSTATE`),
  but a global it sets is its own, as is every global a pack library it `dofile`s sets.
- **draws on the zone map and the minimap through `hs.UI.map.RegisterOverlay`**, which reaches the map the client
  draws whoever's `map.lua` or `minimap.lua` called it (docs/ui-api.md "Map overlays").
- **sees `hs`, `ui` and the standard libraries read-only.**
- **reaches only its own owner's** exported functions, options and menu entries (the pack's windows reach every
  plugin's).
  - `hs.Plugin` gives nil for another plugin (and another's object found elsewhere does nothing).
  - `hs.PluginOptions` and `hs.PluginMenu` list only its own (`hs.PluginMenuClick(i)` numbers entries as that list
    does).
  - `hs.SetPluginOption` and `hs.PluginMenuClick` leave another's alone.
- **keeps settings of its own**; see "Plugin window settings" below. `ui.DisableWindow(name)` turns off only its own
  window. At most 128 windows (`ui.window` raises past that).
- **runs for at most a second at a time** (a draw, its file's load, a function the pack calls); the pack's own code
  for five.
  - Longer code is stopped with an error its own `pcall` cannot catch, and its window is off until its file changes.
  - A backtracking string match (`string.find`, `match`, `gmatch`, `gsub`) counts too.
  - The UI's Lua state holds at most 1 GiB; an allocation past that fails in the code that asked. Plugins' code gets
    three quarters of it.
  - `collectgarbage` takes only `"count"` and `"step"`.
- **uses the clipboard** (`ui.GetClipboardText`, `ui.SetClipboardText`) only when its owner has every capability, as
  for a script: it may hold a password.
- **gets nil from the pre-game screens** (`hs.LoginScreen` and the others hold the password) and cannot act on them.
- **may not run, stop or pause scripts** or change the autorun list (`hs.ScriptRun` and the others).
- **opens web pages as its owner** (`plugin:<name>`, or `script:<name>`: a Lua plugin's windows share the script's
  `mq.web` tabs) with its own permissions.

#### Plugin window settings

`ui.SetSetting(key, v)` writes `plugin.<owner>.<key>` in the pack's settings file; a window's place under `hud.` is the
character's, as the pack's are. `ui.GetSetting(key)` reads that, else the pack's own key of that name (the palette,
the place of a window it took over).

- Neither reaches the pack's test hooks (`test.*`), the windows' switches (`enabled.*`) or another plugin's keys.
- `ui.SetSetting` refuses (returns false) a key with `=` or a line break, or a value with a line break.
- At most 1024 keys per plugin, each at most 256 bytes, values at most 16 KiB. Past that `ui.SetSetting` returns
  false; an existing key can still change.

#### What plugin windows may call

Reading the game and drawing need no capability beyond `ui`. An `hs` function that acts needs the plugin's capability
for it, called from the plugin's code or from a pack function it calls. The catalog is `app/lua_ui_api.inc`; a function
missing from it is taken to act. Refused, it does nothing and returns nil, with one log line.

| Needs | `hs` functions |
|---|---|
| never | the player's one-shot keys and requests, which the pack's windows take once; see the note below |
| nothing | everything that reads or draws (`hs.Item`, `hs.AllItems`, `hs.ItemStats`, `hs.ItemKept`, `hs.ItemJob`, `hs.ChildState`, `hs.ListRow`, `hs.SpellBook`, `hs.DrawMap`, ...), a window's place and size (`hs.ReplaceWindow`, `hs.WindowPlaced`, `hs.SizeWindow`), the map's zoom and pan, opening a menu (`hs.WindowContextMenu`, `hs.ChatMenu`, `hs.HotBarMenu`; choosing a row is below), the close-top-window key's press (`hs.TakeCloseTopWindow`) |
| `commands` | `hs.ShowWindow`, `hs.SetOption`, `hs.SetUiIni`, `hs.ShowSpell`, the chat windows' setup (`hs.ChatNewWindow`, `hs.ChatSetFilter`, ...), the spell sets' and the autoskill list's editing, the map's find path, the Find window (`hs.FindOpen`) and its guide path (`hs.FindNpcPath`), the social editors, `hs.gamepad.set_*` and `hs.gamepad.request_focus` (menu mode, as `set_mode`) |
| `target` | `hs.FindNpcTarget` (the Find window's row) |
| `move` | `hs.FindNpcWalk` (the Find window's Walk there) and `hs.ZoneExitWalk` (the zone finder's), both `/nav` walks. (`/nav` itself needs `move`, and the doors a walk opens on its way come with it; `/nav door ... click`, which clicks the door on arrival, needs `actions` too) |
| `actions` | a stock window's controls, the menus' rows, items and the cursor, hot buttons and gems, spells and abilities, socials; see the list below |
| checked by the function | the plugins' (above), the scripts' (refused; `hs.Scripts` lists only a script of the plugin's own name, with no folders), the pre-game screens' (nil), the web's (`web`); `hs.ChatSubmit`'s line goes through the command filter, which holds it to the plugin's capabilities as any command it sends (plain text needs `chat`, `/cast` needs `actions`); a plugin's line joins neither the chat input's history nor makes its window the active one |

**Never.** The chat input's keys and requests (`hs.ChatTakeFocus`, with a pending Link All; `hs.ChatPage`,
`hs.ChatHistory`) and the Lua social editor's (`hs.LuaSocialEditRequest`) give nil to a plugin's code. For a plugin,
`hs.ReplaceWindow` and `hs.WindowShown` do not put away the client's consent prompt, nor its dialogs that act when
answered (a confirmation, a destroy's among them; a trade, give, quantity, bazaar purchase, group invitation, respawn,
large dialog or task offer).

**`actions`** covers:

- a stock window's controls: `hs.ClickChild`, `hs.ClickChildAt`, `hs.WindowButton`, `hs.StmlLink`,
  `hs.SetWindowText`, `hs.ListClick`, `hs.ListHeaderClick`, `hs.ComboSelect`, `hs.SetSlider`, `hs.SetTabPage`;
- the menus' rows: `hs.CascadeClick`, `hs.GameMenuClick`;
- items and the cursor: `hs.ClickSlot`, `hs.BagSlotClick`, `hs.BankSlotClick`, `hs.PlaceClick`, `hs.PickUp`,
  `hs.SortItems`, `hs.SellJunk`, `hs.BulkItems` (sell, tribute, destroy or withdraw chosen items), `hs.StopItemJob`,
  `hs.SetItemKept` / `hs.SetItemsKept` (a Keep mark taken off lets the item be destroyed), the parcels,
  `hs.gamepad.drop_cursor`;
- hot buttons and gems: `hs.ClickHotButton`, `hs.PressButtonFrom`, `hs.ClearCursorButton`, `hs.SpellGemClick`;
- spells and abilities: `hs.SpellBookClick`, `hs.SpellBookMemorize`, `hs.SpellBookSwap`, `hs.MemorizeSpellSet`,
  `hs.UseSkill`, `hs.AbilityBookUse`, `hs.AbilityBookAutoskillEnable`. `hs.AbilityBookAutoskill` and
  `hs.AbilityBookAutoskillRule` need `commands` too;
- socials: `hs.PressLuaSocial`, `hs.PressSocial`, `hs.StopSocial`, `hs.SocialToCursor`. `hs.SetLuaSocial` and
  `hs.LuaSocialDryRun` (which runs the given code with the server's Lua social capabilities) need `commands` too.

#### What stays shared

A plugin can still write into the tables the pack keeps in its globals; that is how its windows join the pack's focus
and palette, so it can upset how the pack's windows look or behave. It sees each such table through a view:

- Reads and writes go through to the table.
- A function the plugin puts there runs as the plugin whenever the pack calls it, does nothing once the plugin
  unloads, and cannot be called by another plugin's code.
- The view's metatable cannot be read or changed, `rawset` refuses it, and a plugin's tables cannot have a `__gc`
  finalizer.
- `next` and `rawget` see nothing through a view (`pairs`, `ipairs`, `#` and `table.*` work).

The pack's tables reach the plugin only as views: what a pack function it calls returns or raises, the tables the pack
passes its functions as arguments (its own tables handed back stay its own), and a metatable it did not set itself
from `getmetatable`. It cannot get the pack's own `hs` or `ui` functions to act under the player's authority.

Not covered: a function or table the plugin hands a pack function, or returns to the pack. The pack may keep it as it
is (docs/todo.md). A player who copies the same files into a UI pack makes them the pack's own, with the player's
authority.

#### How plugins load

- The client hashes the file, checks the server's rules, and loads **a copy of exactly the hashed bytes** from
  `cache/plugins/`, so the file cannot be swapped between the check and the load.
- On Windows a plugin's (and an upscaler provider's) own imports are looked for beside the copy, in Hailstone's
  folder and in System32, never in the current folder or PATH; the client sets the same for every library loaded by
  name alone (`SetDefaultDllDirectories`, `platform::restrict_dll_search`). A plugin that needs a DLL of its own loads
  it by its full path.
- Callbacks the plugin's capabilities do not cover stay unhooked, with a note in chat.
- When the rules no longer allow a plugin, `ShutdownPlugin` runs and it is unloaded.

**In this repository:** `plugins/example/` (MQ2Example, plain C). The NMS server's plugin
(projecthailstone/hailstone-nms; its docs/nms.md), Project Triune's (private) and every other plugin or script live in
their own repositories and install through the add-on manager (docs/addons.md "For authors"). NMS or Triune cloned
into `plugins/<name>/` builds with the client and joins `hailstone_tests` (docs/layout.md "Builds and installs").

**Server admins:** a compiled plugin is native code. Capabilities limit the host table, but it can still call the
operating system, and a server's plugins load on every machine whose player allowed them ("The server's compiled
plugins ask first" above): ship only code you trust. Scripts go through the client for everything, so their
capabilities hold. The client enforces all this; a modified client can ignore `hailstone.cfg`, so the server's own
checks stay the real line.

## Upscaler providers

An upscaler provider is an outside temporal upscaler (Intel XeSS, AMD FSR 4 ...) built against
`sdk/hailstone_upscaler.h`. It is not a game plugin: no host table, capabilities or callbacks, and it sees nothing of
the game.

- Servers do not send providers. An opted-in add-on policy can require, allow or block one by repository, checked at
  the character's entry ("Opt-in add-on policies" below).
- The player installs it with the add-on manager (kind `upscaler`, docs/addons.md "Upscalers") and picks it under
  Options, Graphics, Upscaler (`display.upscaler = <name>`). It loads at the next start.
- docs/graphics.md "Upscaler providers" covers the client's side.

**Licence.** `sdk/hailstone_upscaler.h` is MIT. NOTICE.md's plugin and add-on exception covers a provider that talks
to Hailstone only through it, whatever its licence and whatever closed runtime it loads. Meeting that runtime's own
terms is the provider's business.

**Files.** `upscalers/<name>/<name>.dll` (`.so`, `.dylib`) plus whatever it loads. The client loads it from a copy of
that folder in `cache/upscalers/` (the header's `folder`; load a runtime from there by full path), and gives the folder
as `config_dir` for settings and logs.

**The calls**, all on the render thread:

| | |
|---|---|
| `hailstone_upscaler(host)` | the one export: its table (`HailstoneUpscaler`), or NULL to stay out. `host` has the client's ABI, the two folders, a log function and a `vkGetInstanceProcAddr` (callable from `instance_requirements` on) |
| `instance_requirements` | before `vkCreateInstance` (Vulkan 1.3): instance extensions and the lowest Vulkan version |
| `device_requirements` | for the picked GPU, before `vkCreateDevice`: device extensions and features (note below). Say no for a GPU it cannot run on |
| `create` | the device made: instance, physical device, device, the one queue and its family, both `Get*ProcAddr`, and `HAILSTONE_UPSCALER_DEVICE_REACTIVE` when every frame will carry a reactive mask |
| `resize` | render and view sizes, before the first frame and on change; the device is idle |
| `evaluate` | each frame: record the upscale into the given command buffer (outside rendering) |
| `destroy` | before the device goes, when `create` succeeded |

**Device features** are set to `VK_TRUE` in a `VkPhysicalDeviceFeatures2` chained to the Vulkan 1.1, 1.2 and 1.3
feature structs; a core feature goes in through its core field. An extension feature with no core field
(`VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT`, which XeSS needs) goes in as the provider's own struct added to
the chain, valid until `create`; the provider checks its support itself.

A 0 or NULL from a required call, or an exception there, drops the provider; the client falls back to FSR 3.1
(or its own temporal upscaler where the GPU cannot run FSR 3.1) and logs why. A failed `evaluate` drops it from
the next frame.

**A frame** (`HailstoneUpscalerFrame`). Images cross as integers (`VkImage` and `VkImageView` as `uint64_t`; formats,
layouts and usage as `uint32_t`), each with size, usage, aspect and current layout:

| | |
|---|---|
| `color` | RGBA16F at the render size: the scene in linear HDR before exposure and tonemap (`SAMPLED`, `TRANSFER_SRC`) |
| `depth` | reverse Z (1 near, 0 far), the depth aspect (`SAMPLED`) |
| `motion` | RGBA16F, red and green: `previous_uv - uv`, in UV of the render size (x right, y down), without the jitter; `motion_scale` (the render size) turns it into pixels |
| `reactive` | R8, 0..1: how much this frame's colour should replace the history (particles, spells, other blended draws); none (image 0) without `independentBlend` |
| `output` | RGBA16F at the view's size (`STORAGE`, `SAMPLED`, `TRANSFER_DST`), in `GENERAL`, old contents undefined: write all of it and leave it in `GENERAL` |

- Inputs are in `SHADER_READ_ONLY_OPTIMAL`, visible to every stage.
- Jitter is in render pixels, AMD FSR's convention (pixel `t` samples `t + 0.5 - jitter`), with its cycle length.
- Also given: near and far planes, vertical FOV, exposure, frame time, `display.sharpness`, frame slot, and
  `HAILSTONE_UPSCALER_FRAME_RESET` on a cut, teleport, new zone or new targets.
- A provider may bind its own pipelines, sets and push constants and change layouts within the call, restoring each
  image's layout on return.
- With `HAILSTONE_UPSCALER_SHARPENS` it sharpens by `sharpness` itself; otherwise the client sharpens.

**Compatibility.** Every table carries its size and only grows at its end (`HAILSTONE_UPSCALER_ABI` is 1). The client
refuses a provider built for a newer ABI.

**Options notices.** An active provider may supply the optional `ui_contributions` table member. It returns the
count of pointers to `HailstoneUiContribution` records for the current snapshot. An empty snapshot clears the
provider's notices. The records are copied immediately and disappear on fallback. Use the same window and target
IDs as game add-ons; the provider cannot disable the Upscaler row (or the other two above). The renderer provider
fills `action_label` and
`command` with `NULL`. The example below supplies a Graphics notice.

**The example.** `plugins/example/upscaler/example.c` is a whole provider in plain C: it checks the GPU can blit
RGBA16F, then linearly blits the colour to the output (no accumulation). The build puts it in
`build/hailstone/upscalers/example/`; run it with `--upscaler example` in the zone viewer or
`display.upscaler = example`. Tests: `tests/upscaler_provider_test.cpp`.

## Native world nameplate canvases

Plugins with `read`, `ui` and `extend` can replace whole world captions through the nameplate canvas Lua uses. Check
`HAILSTONE_HOST_HAS(host, nameplate_spell_icon)` before using this append-only ABI 3 service. Records and flags are in
`sdk/hailstone_nameplates.h`.

- **Registering.** One callback, `nameplate_register(host, fn, userdata)`. Unregister before registering a
  replacement.
- **What a callback sees.** Actors eligible for a normal world caption, target first, the rest in depth order.
  - Presence flags tell an unknown reading from a known zero.
  - The frame, candidates and counted UTF-8 strings are borrowed until the callback returns.
  - `HAILSTONE_NAMEPLATE_CONCEALED`, `_INVISIBLE` and `_INVISIBLE_UNDEAD` are the Lua context's `concealed`,
    `invisible` and `invisible_undead` (docs/ui-api.md): only what the player could see (the spawn's HideMode, the
    player's buffs, the target's buff list). A clear flag means "not known", not "visible".
- **Record sizes.** Both arrays carry their element size, so either record can grow at its end within ABI 3.
  - Step through `candidates` by the frame's `candidate_size` (the host's `sizeof(HailstoneNameplateCandidate)`) with
    `hailstone_nameplate_candidate(frame, i)`, never by the plugin's own `sizeof`.
  - Test `HAILSTONE_NAMEPLATE_CANDIDATE_HAS(frame, field)` before reading a field newer than the client may be.
  - A model's `command_size` is the stride of its `commands`, normally `sizeof(HailstoneNameplateCommand)`. The host
    refuses one smaller than its own record and reads only known fields from a larger one.
  - `reserved` fields are 0.
- **Caption text.** `caption` is the stock first line, with the player name parts selected by `/shownames` (`/show`).
  - Check `HAILSTONE_NAMEPLATE_CANDIDATE_HAS(frame, secondary_caption)` before reading the stock second line.
  - `secondary_caption` carries a player's `<Guild>` tag or an NPC's parenthesized description or owner name. It is
    empty when the stock caption omits it. The display mode, guild rank, anonymity and pet-owner settings apply.
- **During the callback** `nameplate_measure_text` and `nameplate_spell_icon` are available; an image can use a
  texture returned to that provider during the current callback.
- **Claiming a caption.** `nameplate_submit` with the current frame serial and zero-based candidate index; the host
  copies the model and commands at once. `HAILSTONE_NAMEPLATE_HIDE` claims an empty caption; omitting the model leaves
  it to the stock caption and other providers. Duplicate and stale claims are refused.
- **The shared canvas** enforces geometry budgets, depth and competing claims; clicks target as on a Lua canvas.
  Unregistering, unloading, deactivation or losing a capability retires the provider and its click areas.

## Web pages

Scripts and plugins with the `web` permission open their own tabs in the in-game browser (docs/browser.md): http and
https pages on the hosts the server's `[web] allow` names (any host without it).

#### Where a tab may go

- Every navigation, frame and request (links, redirects, images, scripts, `fetch`) is held to those hosts; a tab that
  lands elsewhere is closed.
- No such tab, `/url` or `/www` reaches this computer or a private network (`localhost`, `192.168.x.x`, `10.x.x.x` and
  the like), even when `[web] allow` names it.
- Each owner has its own cookie store and sees only its own tabs, which are not in the player's browser window.
- Tabs close when the owner stops (a script whose main chunk has returned is stopped) or unloads, and close (Refused,
  with why) when it loses `web` or `[web] allow` changes (docs/browser.md "Rules that change").
- Without the helper (macOS, or not installed) `open` gives nil and why.

#### Web limits

- At most 16 tabs per script or plugin (`WebService::kMaxTabsPerOwner`), counting tabs the helper or the rules closed
  until the owner closes them. A 17th `open` gives nil and why.
- An address is at most 2 MB, a `post` at most 1 MB, an `eval` or `text` at most one helper message (64 MB). Longer is
  refused (false; `navigate` with why) and nothing is sent.
- While 32 MB wait for the helper, `open`, `navigate`, `eval`, `text` and `post` are refused the same way; retry a
  frame later. An empty queue takes the largest message.

#### Talking to the page

The page and its owner talk through `window.hailstone`.

- The page's `hailstone.post(text)` is queued for the owner (`mq.web.messages(tab)`, `hs.WebMessages(tab)`,
  `web_message`).
- A message is at most 1 MB; at most 256 messages and 8 MB wait, oldest dropped first.
- `web_message` with a buffer dequeues the message even when it is cut. With `out` NULL it gives only the length and
  leaves it queued.
- The owner's `post` calls the page's `hailstone.onmessage(text)` when set.

A page cannot run commands or read the game. What a message does is up to its owner, within the owner's permissions.

| | Lua scripts and Lua plugins | A plugin's windows (UI pack) | Compiled plugins (ABI 3) |
|---|---|---|---|
| open | `mq.web.open(url [, w, h])` | `hs.WebOpen(url [, w, h])` | `web_open(host, url, w, h)` |
| go to an address | `mq.web.navigate(tab, url)` | `hs.WebNavigate` | `web_navigate` |
| back, forward, reload, stop | `mq.web.nav(tab, what)` | `hs.WebNav` | `web_nav` |
| run script in the page | `mq.web.eval(tab, js)` | `hs.WebEval` | `web_eval` |
| to the page / from the page | `mq.web.post`, `mq.web.messages` | `hs.WebPost`, `hs.WebMessages` | `web_post`, `web_message` |
| type at the page's cursor | `mq.web.text` | `hs.WebText` | `web_text` |
| state, tabs | `mq.web.state`, `mq.web.tabs` | `hs.WebState`, `hs.WebTabs` | `web_state` |
| draw it (mouse and keyboard) | `mq.web.view(tab, w, h)` in `mq.imgui` | `hs.WebView(tab, w, h)` | `web_view` in `OnUpdateImGui` (needs `ui` too) |
| close | `mq.web.close` | `hs.WebClose` | `web_close` |

A refusal is an error in Lua (`mq.web` without `web`, as for `mq.imgui`); nil or false for `hs.Web*` in a plugin's
window; 0 from a host table function, with the reason in chat once.

A plugin's window acts with its plugin's permissions (a compiled plugin's while loaded, a Lua plugin's while its
script runs, never one for the other); the player's own windows act as the player. The example plugin's
`/webexample` (plugins/example/MQ2Example.c) shows the compiled side.

## The server's rules: `hailstone.cfg`

An ini file at the top of the server's `client_files/` (`plugin::parse_server_cfg`). What it leaves out keeps the
default; `;` and `#` start comments. A section not listed below is kept for scripts to read (`hailstone.server_rule`),
for example `[autocombat] hunter = off`.

`[client] deny = on` makes Hailstone stop before completing its first zone-in and show a notice directing the
player to the server owner. The client checks the policy received through the server's file service on that login.
When none comes (a server without the script) or the download fails, an earlier login's copy does not count and the
client goes in. This check relies on successful policy delivery and an up-to-date, unmodified client.

```ini
[client]
deny = off                ; on blocks Hailstone from entering the server (default off)

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

#### Autoskill

Autoskill (`game/ability_book.h`) is the client's own helper: it uses the abilities on the player's list in order as
they come ready, one at a time 0.6 s apart, through the hot button path.

- By default an ability is used only in a fight (auto attack on an NPC in melee range).
- A beneficial one (a beneficial spell, or a self-only skill: Forage, Mend, Sense Heading, Sense Traps, Fishing, Hide,
  Sneak) may be set to any time, or out of combat (auto attack off and OP_RestState's combat flag clear).
- One that leaves a buff can wait until the buff is off.
- A harmful ability keeps to fights whatever its rule, so autoskill never starts one.
- Each entry also has endurance, mana, own health and target health conditions.
- The on/off switch is kept per character (hotbuttons.ini `[AbilityBook] AutoskillOn`). A server that forbids
  autoskill turns it off without changing that.

It is not a plugin or script, so capabilities do not apply; `[abilities] autoskill` is its switch.

#### Movement rules

NPC collision (`game/npc_collision.h`, docs/mechanics.md "NPC collision"; players, pets, mercenaries, corpses and
mounts stay passable), climbing passable ladders and solid ladders (docs/mechanics.md "Ladders") are Hailstone's own
rules, not the original client's. Each is on unless `[movement]` turns it off; the player has no switch. A change
reaches players at their next login.

### Opt-in add-on policies

A server can require, allow or block add-ons by GitHub repository: any add-on the manager installs (scripts, compiled
plugins, tools such as Texture Studio, upscalers such as XeSS or FSR 4).

Off unless `[addons] enforce = on`; without it nothing here applies, optional suggestions included.
`server/hailstone-addon-policy.cfg.example` is an example; copy it to `client_files/hailstone.cfg` and edit it.

```ini
[scripts]
player = off
;player_caps = read,ui
[plugins]
player = off
;player_caps = read
[addons]
enforce = on
;required = owner/repo, owner/other-repo
;allow = owner/repo
;block = owner/repo
;suggest = owner/repo
default = allow
```

#### Identities

Identities are GitHub repositories, `owner/repo` (`projecthailstone/hailstone-dpsmeter`), compared without case.

- A GitHub link works too; a tag or branch is ignored. They are not version pins.
- They match the source the manager recorded at install, not a folder or manifest name.
- A bare name (`dpsmeter`) is refused, since anyone's package of that name would satisfy it. A refused or malformed
  entry makes the policy invalid, and the log names it
  (`hailstone.cfg: [addons] required: "dpsmeter" is a bare name; add-ons are named by their GitHub repository (owner/repo)`).

#### A minimum version

Only for `required`: `required = owner/repo:1.0.0-preview.4` is met by that installed version or newer.

- Semantic version order: `preview.10` is newer than `preview.4`, `1.0.0` newer than its previews. Build metadata after
  `+` is ignored; prerelease letters keep their case.
- It is the manifest's `version` recorded at install. An installed add-on without a valid one does not meet it.
- A repository named twice keeps the highest minimum. Without the suffix any version does.
- A malformed version (`owner/repo:1.2`), or a suffix on `allow` or `block`, makes the policy invalid.
- The login screen's review and install links name the plain repository.

**Older clients refuse such a server:** they read `owner/repo:version` as malformed, so the invalid policy keeps their
players out of the world. Use the versioned form only once your players run a client that has it.

#### `allow`, `block` and `default`

- `default = deny` refuses every add-on, visual ones included, not named by `allow`, `required` or `suggest`.
- `default = allow` (the default) admits the rest.
- `block` wins over everything. An identity both required and blocked makes the policy invalid.
- An add-on put in by hand has no repository, so no list names it: it runs under `default = allow` (unless it uses a
  blocked add-on's name) and is refused under `default = deny`.
- Renaming a script, plugin or edited file cannot admit unknown bytes: an installed add-on is known by its files'
  recorded hashes.

#### What each kind must meet

- **Interfaces** (`kind ui`): a blocked one, or an unlisted one under `default = deny`, is not offered on that server,
  nor is an interface built on it. The client falls back to the built-in interface (its base, else Modern) without
  changing the player's choice. A hand-placed pack in `ui/` is refused under `default = deny`; the built-in
  `ui/modern` and `ui/gamepad` never are.
- **`required`** add-ons must be installed with every file intact.
  - Required scripts and compiled plugins start at the zone-in even outside the player's autorun, and cannot be
    stopped, paused or unloaded on that server.
  - A required compiled plugin still needs the player's approval of its bytes ("The server's compiled plugins ask
    first").
  - A required upscaler must be the one picked under Options, Graphics, Upscaler and loaded (it loads only at start).
  - A required tool is met by its intact installation; the client neither starts it nor can tell whether it runs.
- **The server's own files** (its `scripts/` and `plugins/`, listed by hash at the zone-in) are not add-ons; the
  policy never refuses them. The server decides by sending them or not.

#### Permission is not capability

Naming an add-on in `allow` or `required` does not turn on `[scripts] player` or `[plugins] player`, nor grant more
than `player_caps` and the add-on's manifest.

- A required script or plugin needing capabilities the server does not give is a requirement not met.
- A `suggest` allows the add-on as `allow` does, but never requires or starts it and grants no capability.

This holds for `required` on purpose: `addons/installed.txt` is the player's file and cannot be verified against a
signed manifest at load time, so letting a named repository lift capabilities would let an edited record bypass
`player = off`.

#### When it is decided

- **At the zone-in**, by the `hailstone.cfg` it brings, never the cached copy (which only feeds the login screen's
  "Required to play on this server" panel), so a fixed policy never locks a player out.
- **Before the world is shown** ("Checking required add-ons"), required add-ons start and a required plugin's question
  waits for the player. See "Entry refused" below.
- **Once in the world**, nothing disconnects. A failed required add-on (a plugin that lets an exception out counts,
  though it stays loaded), or a next zone whose rules are not met, is one chat warning per failure, naming the
  repository:
  `[Add-ons] Required add-on owner/repo did not remain running. You can keep playing on this server.`
  Leaving to character select starts the check over.

**Entry refused.** A requirement that is missing, changed, refused, needs more than the server allows, or fails to
start, or an invalid policy, returns to the login screen with the add-on panel open on what is missing. Reviewing an
entry fetches its manifest (nothing is fetched before that click) and opens the usual install card, with every usual
check. Each reason is also logged (`Add-ons: entry refused by this server's add-on policy: ...`).

**Server add-ons warning and Retry.** While such a failure lasts, a Server add-ons window (Modern, Gamepad, else the
client's own) says what failed and that the player can keep playing. When a required plugin let an exception out or
did not load, or a required script failed or ended, it offers **Retry** (reload or restart once, under every usual
check). Its close box hides it until the next failure. Only the client's own interfaces show it
(`hs.AddonPolicy`, `hs.AddonPolicyAction`, docs/ui-api.md); an add-on interface gets the client's window.

#### When blocking upscalers

An upscaler loads at start, before any server is known, and cannot change while the client runs.

An active upscaler the policy refuses, or whose loaded files no longer match its installed add-on, refuses entry with
instructions to pick another and restart. Under `default = deny`, list permitted ones in `allow` (built-ins need no
entry). Requiring one excludes players whose hardware cannot run it.

#### What a policy does not reach

It governs playing on this server only: other servers keep their rules and nothing is removed. A tool started outside
Hailstone is outside it (the panel's Start button follows it). A texture pack is not an add-on (Texture Studio builds it
into `textures/`); block the tool instead. An older or changed client, or native code the player already approved, can
ignore the policy.

#### The server folder

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

Changes reach a player at their next login (the files come at the first zone-in of a login). Until then the last
login's rules apply provisionally (only `login` runs; "Before the zone-in" above). The zone-in's rules hold while in
the world, revert to the provisional ones at character select, and are dropped on disconnect. Granting `login` in
`player_caps` lets the player's own plugins start at the world login; otherwise they wait for the zone-in.

## The server's settings (protocol)

Two custom opcodes not in EQEmu's `patch_RoF2.conf` (`src/net/settings_packets.h`, which also holds the magic value);
the server's Lua script handles them by wire value. Both little-endian:

- Settings request **0x1432**, client → zone, after every zone-in: `{u32 magic; u16 version 1}`
- Settings **0x1433**, zone → client: `{u32 magic; u16 version 1; u32 flags}`. No flag is defined now (bit 0 once hid
  the server list's player count; nothing reads it). Later fields go after these and are ignored by older clients.

## Setting up a server

Any EQEmu server can send its files with the Lua quest script in `server/` (no C++ changes or rebuild):

1. `python3 server/install.py <server dir>`, then restart the zones.
2. Put `hailstone.cfg`, `scripts/` and `plugins/` in `client_files/` beside `eqemu_config.json`.
3. Check the folder with `hailstone_check`.

`server/README.md` has the rest.

## Server add-on services

These host functions and hooks were appended later in ABI 3, after every earlier entry (the bulk reads through
`get_raid_data_bulk` and the ability calls); a plugin built against an earlier header keeps its positions.

- Check `HAILSTONE_HOST_HAS` before calling a host function, and the struct's returned size before reading an appended
  field (the client checks the hook table's `size`).
- Against a stock server none of these does anything until a plugin uses it.

**Record sizes**, pinned in the client (fields are only added at the end). Each begins with its `size` (the vertex
excepted), which the caller sets and the client respects.

| Record | Bytes |
|---|---|
| `HailstoneFollowProfile` | 56 |
| `HailstoneMountProfile` | 8 |
| `HailstoneFollowStatus` | 24 |
| `HailstoneBinaryMeasurement` | 56 |
| `HailstoneMerchantSellContext` | 32 |
| `HailstoneMapPolicy` | 104 |
| `HailstoneWorldFrame` | 136 |
| `HailstoneWorldAnchor` | 32 |
| `HailstoneWorldVertex` | 28 |
| `HailstoneWorldSurfacePatch` | 64 |
| `HailstoneWorldSurfacePatchResult` | 576 |
| `HailstoneWorldReceipt` | 48 |
| `HailstoneWorldSubmission` (in a 64-bit build) | 56 |

#### Follow and mount profiles

- `follow_profile` and `mount_profile` (`extend`) return bounded data for host-owned movement. A profile never starts a
  follow.
- `manual_follow(host, "start" | "stop" | "toggle")` (`move` and `extend`) validates the selected living player and
  pins both actor incarnations.
- `get_follow_status` (`read`) reports the live target identity.
- Turning and pitch smoothing are time-scaled; movement cancellation is optional. Death, zoning, target replacement
  and profile retirement stop enhanced follow.
- A mount's instant acceleration changes only its ramp, not the speed limits. Field bounds: `sdk/hailstone_follow.h`.

#### Map visibility

`map_visible(spawn, purpose)` (`ui` and `extend`) returns -1 to abstain, 0 to deny, 1 to allow.

- `HAILSTONE_MAP_MARKER` gates extension map/minimap markers.
- `HAILSTONE_MAP_FIND` gates Find results and their target/path/walk actions, including the map and minimap's service
  NPC overlays.
- A denial beats another extension's marker.
- An active guide started from an NPC Find row rechecks before observing its destination's position and retires its
  trail on denial.
- With no policy, stock behaviour remains.
- It controls UI presentation, not spawn data for scripts or native code; the player and group map stay available.

#### Map policy

`map_policy(out)` (`ui` and `extend`) optionally fills `HailstoneMapPolicy`: `size`, `flags`, an optional
NUL-terminated plain display `name[64]`, and an optional content `revision` (advance it when eligibility or provider
data changes without a flags/name change). Return zero to abstain.

The flags:

- `CUSTOM_SPAWNS` makes the first active claiming provider the exclusive extension-marker source, even with no mark.
  Every provider's `map_visible` denial still wins.
- `HIDE_STOCK_NPCS` removes the packs' Find/service NPC overlays and competing stock controls.
- `DISABLE_NPC_FIND` blocks discovery requests, result rows, targets, guides, server paths and Find-owned walks.
- The optional `npc_find_mode` value `HAILSTONE_NPC_FIND_SERVER_ONLY` admits living NPCs advertised in the zone's
  `OP_SendFindableNPCs` list. Show Path requests the route from the server. Client discovery and Find-owned walks
  are unavailable in this mode. All Find visibility hooks still apply to those rows.
- The optional `actor_guides` value `HAILSTONE_MAP_ACTOR_GUIDES` enables visual guides for the player's current
  target and local Bazaar traders. These guides use actor identity and action eligibility. Map and Find discovery
  permissions still govern markers and rows. `DISABLE_NPC_FIND` takes precedence over both optional values.
- Restrictive flags combine across providers; removing them restores stock defaults, saved UI preferences untouched.
- Unaffected: PC group/raid Find, coordinates, ordinary navigation, static map labels, user pins, raw spawn getters.
- Guides and Find walks pin connection/player/NPC identity and recheck eligibility before observing or moving.
- Invalid hook sizes, unknown flags, unterminated names and control characters abstain.

Reading it back:

- `get_map_policy(host,out)` requires `read`, the main thread and an initialized output size; it copies only the
  caller's prefix.
- Calling it from the same plugin's policy hook fails without writing output.
- Both declarations are optional ABI3 tails: use `HAILSTONE_HOST_HAS` and hook-table sizes.

Record prefixes:

- The original 72-byte prefix is still accepted, including four-byte-aligned old reader records.
- The 80-byte prefix adds `revision`, the 88-byte prefix adds `context`, and the 96-byte prefix adds `npc_find_mode`.
- The 104-byte record adds `actor_guides`. Older hosts keep their existing Find behavior.
  Missing optional fields are zero.
- An add-on requiring server-only Find can return `DISABLE_NPC_FIND` when an older host supplies a shorter output
  buffer. Likewise, an older reader sees `DISABLE_NPC_FIND` when the effective policy is server-only.
- Zero-initialize the record before setting `size`; the host preserves that size.
- Success or a returned size of 88 does not prove tail support on an older host; a nonzero `context` does (support for
  context-bound observations).

Revisions and context:

- A provider supplies its own content revision and cannot choose `context`. A reader gets the host's effective
  revision and connection/zone/player context.
- The effective revision covers every active provider's identity and lifetime, including independent
  visibility/Tracking restrictors, activation and permission transitions.
- A same-name replacement or an off/on interval invalidates old policy-derived interactions even if no frame saw it.
- `hs.UI.map.Policy()` exposes these tokens as opaque decimal strings; `name` is presentation only.
- Tracking's source-owned requests are retired by provider replacement, activation and permission transitions, not by
  map-only flags, names or content revisions.

Lua providers:

- `hailstone.hooks{map_policy=fn,map_visible=fn}`, or `hailstone.map_policy(fn)` and `hailstone.map_visible(fn)` (nil
  removes the hook). Both run only with live `ui` and `extend`.
- A policy callback returns a plain table: optional booleans `custom_spawns=false`, `stock_npcs=true`, `npc_find=true`,
  optional printable `name` of at most 63 bytes, nonnegative integer `revision`. Nil or malformed answers abstain.
- A visibility callback gets the spawn and purpose `"map"` or `"find"` and returns false (deny), true (allow) or nil
  (abstain). Denials combine across providers.
- Callbacks keep the script's protected lifetime and execution budget.
- Neither grants authority over Tracking's skill policy, nameplates or other discovery surfaces.

#### Merchant right click

`merchant_right_click(context)` (`packets` and `extend`) may consume a carried item's right click while an NPC merchant
is open.

- The host checks range and the carried slot, and flags an occupied cursor, stock sell refusal or Keep protection,
  pending operations and modifiers.
- Containers are excluded.
- The copied context has the merchant, item, full stack quantity, top-level inventory slot and lossless
  `HailstoneItemSlot`. It grants no inventory ownership.
- An add-on sends its private packet and consumes the click when its option is on. Any context flag means consume
  without sending.
- Returning zero keeps the ordinary item path.
- The client ships no private sale opcode or server-specific slot translation.

#### Raid actions

The roster is read with `get_raid_data_bulk` ("Buff, spawn, extended target, ability and raid records").

- `select_raid_member(spawn_id, incarnation)` (`read` and `target`) targets a roster member in the zone, or a member's
  pet. The spawn id is a record's `spawnID`, the incarnation `get_spawn`'s for it, so a reused spawn id is refused.
- `raid_action(name, HAILSTONE_RAID_MOVE, group)` (`read` and `actions`) moves a member to a group, numbered as the
  bulk read does (0..11, -1 out of every group). Only the leader of a locked raid, only a roster member, only to a group
  with room.
- Both are requests: the roster changes when the server says so.

#### Spawn and player observations

- `HailstoneSpawn.incarnation` identifies a full actor record.
- Resource fields count only with their `HAILSTONE_SPAWN_*_KNOWN` bit, so an absent percentage is not zero or full.
  They are retired on actor replacement/removal.
- `display_name` is the cleaned name; social flags and guild text are received observations.
- `HailstonePlayer.zone_instance` identifies the current instance.
- `HAILSTONE_PLAYER_PROMPT_SAFE` is a local observation: quietly standing/resting, no motion, combat, casting, stun or
  known detrimental buffs. It is not permission for a game action.

#### Items and loot

- `decode_item_packet` (`read`) decodes a received serialized item of at most 256 KiB locally. A failed decode leaves
  both slot and item outputs unchanged, including an invalid item output size.
- `inspect_item_packet` (`read` and `ui`) shows that copied item without inventory change or a link request.
- `HailstoneItem.serial`, `instance_id`, `augment_ids` and `configuration_complete` distinguish received instances and
  installed configurations. Bag contents are never reported as augment IDs.
- `item_flags`, `item_description_append`, `item_presentation_revision` and `loot_confirmation` let an active
  extension supply plain text metadata and an extra loot question. They do not suppress the stock no-drop
  confirmation.
- `zone_display_name(zone_id, instance_id)` changes only a display label (the HUD's and map's zone name), never
  protocol identity: the `Zone` TLO and `Zone.Name` keep the zone's long name.

#### Gems and time

- `get_memorized_spell` (`read`) returns a usable gem's spell ID, zero for empty or unavailable.
- `monotonic_ms` (`read`) shares a world frame's `now_ms` epoch, independent of wall-clock time.

#### Binary measurement

`measure_binary(kind, out)` (`read`, main thread) measures the running executable file (`HAILSTONE_BINARY_EXECUTABLE`)
or the calling plugin's admitted private library copy (`HAILSTONE_BINARY_CALLING_PLUGIN`): byte count, whole-file
SHA-256, and FNV-1a64 with bit 63 cleared.

- No arbitrary file path, no hardware identifiers. Files up to 4 GiB, streamed.
- Updating the installed plugin file does not change the loaded copy's identity.
- A private-copy SHA-256 mismatch, size/mtime change, read error or unknown kind fails without changing the output.
- It measures file bytes, not process memory. Challenge, report format, allowlist and admission belong to the server
  integration; the host makes no legacy-client or server-specific identity claim.

### Transient world geometry

`sdk/hailstone_world_geometry.h` describes `world_frame`, `world_frame_result`, `world_capabilities`,
`world_floor_below`, `world_anchor` and `world_submit`. They require `ui` and `extend`.

- The client supplies acquired scene frames, terrain queries and drawn actor anchors; an add-on converts its private
  protocol to these generic coordinates.
- Query results tell unavailable/deferred from a completed query with no surface.
- Frame and epoch tokens expire, query/vertex/batch budgets are bounded, and a complete submission is copied before
  admission. Owners come from the host, never the add-on.
- Resubmit every batch each frame. Unloading, deactivation and permission loss retire owned geometry.

#### The floor below

`world_floor_below` returns the highest upward drawn opaque or masked surface at or below `seed_z` (the caller's
bounded ceiling for choosing a floor layer).

- Included: terrain, static and passable props, current animated scenery, doors, placed objects.
- Excluded: invisible collision, undersides, transparent surfaces, creatures, dropped items, ghosts, missiles.
- Masked triangles use their geometric footprint; texture alpha is not sampled.
- These surfaces do not change movement collision.

#### Surface patches

Hosts advertising `HAILSTONE_WORLD_SURFACE_PATCH` also provide `world_surface_patch`. Supply three XYZ floor vertices
for the proposed patch, a vertical tolerance and a nonnegative `maximum_rise`; the local ceiling is the interpolated
candidate plane plus that rise.

- `HAILSTONE_WORLD_PATCH_PLANAR` certifies the whole triangle's drawn-surface coverage within tolerance and no higher
  surface below the ceiling, current props and animation included: a geometric coverage proof, not point samples.
- A `MISMATCH` or `MISSING` result supplies an interior XY refinement witness; Z is meaningful only with
  `HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN`.
- `HAILSTONE_WORLD_PATCH_SOURCE_FACE` also supplies the drawn source triangle at that witness, for exact boundary
  splits. Its vertex order follows the native upward `CollisionTriangle` convention.
- Each derived patch must be certified again.
- `MISSING` is a hole in the candidate plane's coverage and can have a lower surface below it.
- Incomplete geometry, invalid/stale tokens or exhausted work return `DEFERRED` without changing the output.
- Candidate inspection and clipping spend the same frame budget as point queries.

`HAILSTONE_WORLD_PATCH_REGION` optionally gives up to 32 double-precision XY vertices of the selected convex
refinement region (a higher-face overlap or uncovered piece). It is a boundary hint inside the input triangle, may
overlap other surfaces and certifies no mesh: intersect it with the source face when refining, and certify every
resulting triangle before submitting it.

**Witness rounding.** A float witness may round onto or just outside the producer boundary only when both a valid
source face and a positive-area convex region are present.

- The region's double-precision centroid must lie strictly inside the producer and source face.
- The float witness XY must equal `float(centroid)` exactly; height and source face then refer to that double
  centroid.
- A point-only witness still needs strictly interior coordinates.
- This never turns an incomplete patch into a planar certificate.

#### Receipts

Admission is not rendering.

- A recorded receipt means the complete batch was recorded.
- A presented receipt means its frame was accepted for presentation.
- Neither proves visible pixels, visibility or attention, and no receipt authorizes gameplay actions.
- A frame-unavailable notification clears earlier availability.
