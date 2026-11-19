# Plugins, MacroQuest Lua scripts and the server's rules

UI authors should use the shared [UI API](ui-api.md) (`hs.UI`), including its
version and method discovery. The flat `hs.*` host bindings documented below
have the same arguments, results and capability checks. Both
forms refer to the same finalized guarded closures.

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

docs/scripting-reference.md lists every TLO and member, the `mq`, `hailstone`, `ui` and `hs` functions and the
compiled plugins' host table, hooks and structs in one place. This page explains how they fit together and what the
rules are.

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

A server can also require, allow or block the player's add-ons by their repository ("Opt-in add-on policies" below);
without `[addons] enforce = on`, the table is all there is.

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

A compiled plugin an opted-in policy requires ("Opt-in add-on policies") asks the same question, by the same
per-server, per-hash approval. It is the player's own add-on, not the server's, so the question says so: "Allow your
add-on this server requires?", the server requiring that plugin from the player's add-ons, and a warning to allow it
only when the add-on's author is trusted (the server's own plugins: "Allow this server's plugin?" and trust in the
server; both at once: "Allow these plugins?"). The words are Hailstone's own; the original has no plugins.

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
  draw over all windows (`ui.Foreground` is refused them), find where the question is (`ui.FindWindowRect`), or
  draw into it: `ui.Begin` on the question's window (or the client's prompt's name, before its first frame too)
  returns not visible and begins nothing, `ui.BeginChild` inside it likewise, and `ui.End` cannot close it. What a
  script's draw function leaves open, or sets for a next window it never begins, is closed and dropped when it
  returns, so it never reaches the question's window drawn after it.
  Only the pack's own code puts the client's prompt away for a window of its own (`hs.WindowShown("PluginConsent")`
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
`hs.PluginConsent()` (nil, or `{server, serial, title, intro, warning, plugins = {{name, file, size, sha256, short,
required}}}`: the title, the line before the list and the warning worded for what is asked about, `required` true for
the player's own add-on the server requires) and
`hs.AnswerPluginConsent(true | false | nil, serial)`, where `serial` is the question's: an answer to a question that has
since been replaced by another (another server's, or other bytes) is refused (false) and nothing is kept. Both packs
leave Allow and Don't allow off for a second after a question shows or a new one takes its place, so a click or press
meant for something else cannot answer it. A plugin's own code in the UI gets nil and cannot answer. Without a pack
window for it (`hs.ReplaceWindow("PluginConsent")`) the client draws its own.

## For players

- `/lua run <script> [args...]`, `/lua stop [script]`, `/lua pause [script]`, `/lua list`. From a script or plugin
  (`mq.cmd`, `DoCommand`, `hs.ChatSubmit`) `/lua stop` and `/lua pause` reach only a script that may do no more than
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
| 1 | `read` | read game state: `mq.TLO`, `${...}`, `mq.parse`, `ParseMacroData`, spawn events, `ability_state`, `get_ability_data_bulk` |
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

The check sits in one place, the command filter (`src/mq/commands.cpp`). It looks at the capabilities of the code
running now (`plugin::caller_caps()`). So a command reaches the same check whether a script sends it with `mq.cmd`,
`/docommand`, `/squelch`, a bind, or a plugin's `DoCommand`. The game's own commands are classified by their entry in the
command table (`plugin::command_caps`). Hailstone's own commands are not in that table and need `commands`, except
the ones that destroy or move items (`/destroy`, `/destroycursor`, `/stowcursor`, the short `kOwnActions` list in
`src/plugin/policy.cpp`), which need `actions`. MQ's and plugins' commands declare what they need.

## MacroQuest Lua scripts

Only Lua scripts are supported. MQ's old `.mac` macro language is not.

A script is `lua/<name>.lua` or `lua/<name>/init.lua`, the same layout MacroQuest uses. Each script gets its own Lua
5.4 state and runs as a coroutine that the client resumes once a frame. `mq.delay` yields to the next frame. A
script that runs more than 3 ms without yielding is paused until the next frame, like MQ's turbo, so a busy loop
cannot freeze the client. One that cannot pause for 5 s is stopped; an `mq.imgui` window's function, which cannot
pause, is stopped the same way, and so is a long string match (a pattern that backtracks), which cannot pause either. Only the script's own run and its binds are paused: a coroutine the script made itself
(`coroutine.create`, `coroutine.wrap`) is never yielded by the pause, which would hand its `coroutine.resume` an
early yield with no values, so long work inside one counts towards the 5 s and is not spread over frames. Catching
that stop does not keep code running: once it fires, every further instruction of the script, and every `pcall`,
`xpcall`, `coroutine.resume` or `coroutine.close` it returns to, raises it again, until the client next enters the
script (its next frame, a social's next press). A bind
whose function runs commands that start binds of the same script again (`mq.bind('/x', function() mq.cmd('/x')
end)`) ends the script once they nest 16 deep; a command of more than 1000 words for a bind is refused (a line in
the chat) and the script runs on. An `mq.cmd` line is at most 2047 bytes (MQ's command buffer; longer is an
error), and `/docommand`, `/noparse` and `/squelch` nest at most 16 deep in one line (the rest is dropped, with a
line in the chat). Each script's memory is capped at 256 MB: past it the script's own
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
| `mq.event(name, pattern, fn [, {keepLinks = true}])`, `mq.unevent`, `mq.doevents([name])`, `mq.flushevents([name])` | chat events, matched as MacroQuest's Lua events match them (its Blech: `mq/event_pattern.h`): text is compared ignoring the case of A-Z; a literal before the first `#...#` must start the line, and a pattern without any must be the whole line; each `#...#` (`#*#`, `#1#`, `#2#`, ..., or any other name, such as `#who#`) takes text up to the first place the literal after it is found, the last one the rest of the line, and the literal after that one must be found first at the line's end (so `#*# says #1#` on `A says B says C` gives `B says C`); line breaks are ordinary text. `fn(line, arg1, arg2, ...)` gets each `#n#`'s text by its number, `""` for a number the pattern lacks, up to the highest `#n#` (at most 255; a larger one, `#*#` and other names match without being passed on). `|text|` matches what the script's `${...}` in it reads when the line comes (with the script's `read`; without it, the text as written), and its value is never parsed again; `##` and `||` stand for `#` and `|`. The line is matched and passed with its item links shown as their text, unless the event has `keepLinks`; lines of 2048 bytes or more and empty lines raise no events. Each pattern is matched on its own, where MQ's shared tree of patterns can turn down a line (a deliberate difference). A script has at most 256 events, a pattern is at most 1024 bytes and a name 256, and a script's patterns together at most 32 KB (an error past them): every pattern is matched against every chat line, outside the script's time slice. A line another script or a plugin wrote (`print`, `/echo`, `WriteChatf`) raises events too, as `/echo` does in MQ, but a handler then gets no more than that code may do: one that would get more runs as a thread of its own (as a bind), held to both scripts' capabilities for each of its resumes, and `doevents` goes on without waiting for it (back in its own Lua loop, so a handler whose own line raises its event again keeps the script busy, paused each frame like any loop, but cannot hold the client) |
| `mq.bind('/cmd', fn)`, `mq.unbind` | a command (needs `commands`); `fn` gets the words and may `mq.delay`. The client's own commands cannot be bound: the game's (with their abbreviations, so not `/g` or `/r` either), `/lua`, `/plugin` and MQ's (`/echo`, ...), nor a name another script or plugin has now (an error naming it): what the player types for one is never handed to another. A bind runs with the script's capabilities and those of whoever ran the command (another script, a plugin, the player all) together, at its start and again after each `mq.delay` |
| `mq.imgui.init(name, fn)`, `mq.imgui.destroy`, `mq.imgui.exists` | a window drawn each frame with `ImGui` |
| `mq.web.open(url [, w, h])`, `.navigate`, `.nav`, `.close`, `.eval`, `.post`, `.messages`, `.text`, `.state`, `.tabs`, `.view(tab, w, h)`, `.available` | the script's own tabs of the in-game browser ("Web pages" below); `view` in an `mq.imgui` window |
| `mq.parse(text)`, `mq.gettime()`, `mq.exit()` | |
| `mq.getAllSpawns()`, `mq.getFilteredSpawns(fn)` | spawn values |
| `mq.pickle(path, table)`, `dofile(path)` / `loadfile(path)` | settings files under `mq.configDir` (`scripts/config/`, or `scripts/config/<script>/` for a script without every capability, above); read back as data, with nothing to call: the file runs with an empty environment, and `loadfile` hands back only what it returned, refusing a file whose values are or hold a function (`nil` and why; `dofile` raises it), since a script writes these files as it likes. A file is at most 8 MiB, and a write that would leave `mq.configDir` holding more than 1000 files or 64 MiB is refused (an error) |
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
`GameTime`, the game TLO (`.GameState`, `.Server`), `Heading[deg]`, `Int`, `Float`, `Bool`, `String`, `Select`, `Math`,
`If`, `Range`. Compiled plugins can add more.

The data types use MQ's member names and inheritance:
- `character` → `spawn`, `buff` → `spell`, `groupmember` → `spawn`.
- `class`, `race`, `heading` (`Degrees`, `ShortName`, ...), `timestamp` (`TotalSeconds`, `TimeHMS`, ...), `item`.
- MQ's pure types, with all of MQ's members: `int` (`Hex`, `Reverse`, `Float`, `Double`, `LowPart`, `HighPart`,
  `Prettify`), `float` (`Deci`, `Centi`, `Milli`, `Precision`, `Int`, `Raw`, `Prettify`), `string` (`Length`, `Upper`,
  `Lower`, `Left`, `Right`, `Mid`, `Find`, `Compare`, `CompareCS`, `Equal`, `NotEqual`, `EqualCS`, `NotEqualCS`, `Arg`,
  `Count`, `Token`, `Replace`, `StripLinks`), `bool`. They follow MQ2BasicTypes.cpp, edge cases included: a member
  that needs an index is NULL without one (`Left[0]` too), `Arg` splits as MQ's GetArg (a run of separators is one, a
  "quoted run" is one argument), `Token` looks for each next separator from one past the last. `Replace`'s result is
  cut to 2047 bytes (an MQ string's room), so a `Replace` nested in itself cannot grow without end. MQ's int and int64 are
  one type here: a value that fits 32 bits has int's `Hex` ("0xFFFFFFFF" for -1) and 16-bit `LowPart`/`HighPart`, a
  larger one int64's. Two deliberate differences: `float.Prettify` groups the number (MQ's prints the float's bits as
  an integer), and `Compare` gives -1, 0 or 1 (MQ's only promises the sign). `Int[...]` and `float.Int` cut toward
  zero, and are NULL for NaN, an infinity or a number past a 64-bit integer (what MQ gives for those is not checked).
- `Math` (MQ2MathType): `Calc`, `Abs`, `Sqrt`, `Sin`/`Cos`/`Tan` (degrees), `Asin`/`Acos`/`Atan`, `Rand[max]` /
  `Rand[min,max]` (both included), `Hex`, `Dec`, `Not`, `Clamp[n,min,max]`, `Distance[y,x,z:y,x,z]` (3D; a missing
  point or coordinate is the player's). The arithmetic is MQ's Calculate (`mq/calc.h`), quirks included: `NULL`,
  `TRUE`, `FALSE` are numbers, spaces are skipped, `\` divides whole numbers, `^` is power and `^^` xor, every operator
  groups from the left (`-2^2` is 4), the bit operators work on 32 bits. A formula that does not calculate (a letter,
  a division by zero, a stray `)`) is NULL. The result is a float, as MQ keeps it.
- `If[condition,whentrue,whenfalse]` (or with exactly two `~`, `If[c~a~b]`): the text of one branch, the false one
  running to the end; NULL when the condition does not calculate. `Range.Between[a,b:n]` and `Range.Inside[a,b:n]`.

For example `Me.PctHPs`, `Me.Buff[n|name].Duration`, `Me.Gem[n|name]`, `Me.SpellReady[n|name]`, `Me.Casting`,
`Me.Combat`, `Me.XTarget[n]`, `Me.FreeInventory`, `Me.Inventory[mainhand|pack1|23]`, and on spawns `Target.Distance`,
`Target.HeadingTo`, `Target.ConColor`, `Target.Named`, `Spawn[...].Loc`, `X`/`Y` as MQ's (the server's) coordinates.

For a script that fights:
- Abilities by name (a discipline's also without its "Rk. II"), an AA's group or a skill's number, as the Ability Book
  sees them: `Me.AbilityReady[Kick]` (a skill), `Me.CombatAbilityReady[name]`, `Me.AltAbilityReady[name|group]`,
  and `Me.AbilityTimer`, `Me.CombatAbilityTimer`, `Me.AltAbilityTimer` (a timestamp, the time left). Ready means
  its reuse timer has run out; its endurance and mana are the script's to check.
- Item click reuse, for example `FindItem[=Clicky].TimerReady` or `Me.Inventory[mainhand].TimerReady`, gives
  seconds left on the later of the item's own reuse timer and its shared recast timer. It gives 0 when both
  timers have elapsed and NULL for an item without a click effect (or no item).
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

Spawn searches use MQ's words and its rules (ParseSearchSpawn and SpawnMatchesSearch, `mq/spawn_search.h`):
- kinds (MQ's spawn types): `pc`, `npc`, `mount`, `pet`, `pcpet`, `npcpet`, `corpse`, `npccorpse`, `pccorpse`,
  `trigger`, `untargetable`, `trap`, `chest`, `timer`, `aura`, `object`, `banner`, `campfire`, `mercenary`, `flyer`,
  `any`. A body type, race or class makes a spawn one of these as MQ's GetSpawnType does, so `npc` leaves out
  mounts, mercenaries, auras, banners, untargetable spawns and the like; it keeps an NPC's pet but not a player's.
- filters: `id N`, `notid N` (one id: the last), `radius R` (3D, from the player or `loc`), `zradius R` (from the
  player's z or `loc`'s), `range MIN MAX`, a bare number (that level), `class X` (the long name) or a bare class
  name (long or short), `race X`, `body X`, `named`, `targetable`, `los`, `group`, `nogroup`, `raid`,
  `fellowship`, `guild`, `guildname X`, `noguild`, `nopet`, `xtarhater`, `nopcnear [R]` (200 without a number),
  `gm` (with `npc`: GM trainers), `merchant`, `banker`, `tribute`, `knight`, `tank`, `healer`, `dps`, `slower`
  (classes; ignored with `npc`), `trader`, `lfg`, `light [NAME]` (MQ's light names: `CDL`, `TR`, `LS`, ...),
  `playerstate N`, `loc X Y [Z]` (a Z of 0 or none is the player's, and the word after is not taken), `alert N`,
  `noalert N`, `nearalert N`, `notnearalert N`
- names: words that are no keyword make a partial name, matched in the name as sent or cleaned; `=Name` or
  `name =Name` asks for the whole cleaned name (MQ keeps the `#` of a `#Name` there, so `=` never finds one)

Results come nearest first (3D, from the player). `targetable` is the game's own test, as in MQ: the spawn's
Targetable flag on and its body type not 11 (EQEmu's NoTarget, which servers give portal and trigger NPCs).
`Spawn.Targetable` is the flag alone, as in MQ, so a script that filters spawns itself checks `BodyType` too.

Differences from MQ: `level N`, `afk`, `linkdead`, `buyer`, `exact` and `merc` are Hailstone's own words. This client
keeps no alert lists, so `nearalert` finds nothing and the other alert words ask nothing (as in MQ with no such
list). `fellowship` matches the server-supplied roster, including expanded rosters of up to 64 members. `los` keeps a spawn while the zone's collision is not
loaded. `next` and `prev` (MQ's target cycling) are accepted and do nothing yet.

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
| `ui_function(name, fn)`, `option{name, label, min, max, get, set}`, `menu(label, command)` | for its windows, as a compiled plugin's (`ui`); `hs.UI.plugins.Get("<script name>")`. A Lua social cannot call them (an error): its state goes whenever the social is edited, and its name is only its title |
| `player()`, `spawn(id)`, `spawns()`, `spell(id)`, `item(slot [, sub])`, `buff(i)`, `xtarget(i)`, `raid()`, `eqstr(id, ...)`, `zone_name(id)`, `server()` | the game state as tables (`read`); `server()` is the server this login is on (`{name, host, port, world, world_host}`, nil before the world login); `player()`'s `cur_hp` and `max_hp`, like a compiled plugin's `HailstonePlayer` and `Me.CurrentHPs`, include the items' HP that EQEmu's own HP update leaves out, as the windows show them |
| `raid()` | the raid roster as columnar tables: `active, locked, loot_type, count, leader, target, spawn_id[], name[], group[], flags[], level[], class[], pct_hp[], pct_mana[], pct_endurance[], pet_spawn_id[], pet_pct_hp[], x[], y[], z[], distance[]` (members 1-based; `leader`/`target` 1-based, 0 none; `-1` an unknown percentage). The `hailstone` module also has the flag bits as `RAID_LEADER`, `RAID_GROUP_LEADER`, `RAID_MAIN_ASSIST`, `RAID_MARKER`, `RAID_WAYPOINTER`, `RAID_LOOTER`, `RAID_PRESENT` and `RAID_ME`, with the SDK's `HS_BULK_RAID_*` values |
| `spell(id)` display fields | raw spell data: `range`, `ae_range`, `push_back`, `duration_formula`, `duration_cap` (ticks), `spell_type`, `resist`, `resist_adj`, `spell_icon`, `category`, `subcategory`, `cast_on_you`, `cast_on_other`, `wear_off`, and `effects` (12 1-based entries with `effect`, `base`, `base2`, `max`, `formula`). These are the loaded record values, not computed duration or effect descriptions; a script decides how to present them. |
| `hs.DisplaySpellId(n)`, `hs.DisplayItemData(n)` | in a Lua UI window, read the actual spell ID or item in display instance 1..6, or nil when absent. Item data gives `id`, `name`, `icon`, `lore`, `value` (copper), `stackCount`, `damage`, `delay`, `itemType` and `favor` (`stack_count` and `item_type`, the names before, are kept); this is the inspected item, not necessarily one in inventory. It also gives the spell IDs of its effects, `scrollSpell`, `focusSpell`, `procSpell`, `wornSpell` and `clickSpell` (0 when the item has no effect of that kind), which a script can look up with `spell(id)` above or `hs.SpellDetails(id)`; an ID can name a spell the local spell file does not have. |
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

`/lua run` finds a script whatever the case of its name, on Linux as on Windows and macOS (`mq::script_spelled_path`):
`/lua run autoloot` starts `scripts/AutoLoot/init.lua`. A name spelled exactly as on disk wins; otherwise each part of
the path must match exactly one file or folder that differs from it only in case, so `AutoLoot/` beside `AUTOLOOT/`
(possible on a case-sensitive file system only) finds neither. A `:` in a name is refused. A module's name
(`require`) is still looked up as written.

## The Scripts window

Hailstone's own window (`ScriptsWnd`; `ui/<pack>/scripts.lua`, `app/lua_ui_scripts.cpp`), opened from the EQ menu's
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

A social is named `social '<title>'` in messages, its `mq.configDir` and its other folders; no script may have a name
that starts `social '`.

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

**Licence.** The client is GPL-2.0-or-later, but `sdk/` is MIT (`sdk/LICENSE`), and NOTICE.md's plugin and add-on
exception lets a plugin built against it, or a script using the `mq` module and `hs.*`, use any licence, closed ones
included. A plugin that copies the client's own code (anything outside `sdk/`) is covered by the GPL instead.

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
- `OnIncomingChat(line, color)`, `OnFilterChat(line, color)` (nonzero hides the line): the server's lines and the
  client's own, not those a script or plugin writes (`print`, `/echo`, `WriteChatf`, a reply to its command), as MQ
  hands its own lines to `OnWriteChatColor` and not here: a plugin that answers "Bob tells you, ..." would otherwise
  answer whatever another add-on printed
  Combat hit lines (melee and spell damage, heals) reach these hooks as the full sentence whatever the player's Hit
  Modes: unlike MQ, whose hook sees the line as RoF2 displays it, an abbreviated or number-only line is never what a
  hook gets, so a parser matches one form. A hook that rewrites such a line shows its rewrite in full, the hit mode
  set aside for that line
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
from a thread the plugin started does nothing and returns 0 / NULL (`ability_state` -1, "unknown", since its 0
means "on reuse"), with a line in the log once. A plugin that works
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
  game string with its `%1` filled. `ability_state` says whether a skill, a discipline or an activated AA is ready,
  and its remaining and reuse time, by name or number. A spawn also carries its rider, mercenary and GM flags, whether it is one of the
  player's group, its con colour and its body types. Each struct starts with its size; the client fills what both know, so a plugin built
  for an older client keeps working. `get_buff_data_bulk` gives every buff slot of the player in one call,
  `get_pet_buff_data_bulk` the pet window's buffs, `get_target_buff_data_bulk` the current target's buffs,
  `get_avail_aa_data_bulk` the owned alternate abilities, `get_avail_disc_data_bulk` the combat abilities,
  `get_ability_data_bulk` the Ability Book's entries with their readiness (below),
  `get_buff_name_data_bulk` the player's buffs with their casters' names, `get_spawns_data_bulk` every spawn in the
  zone and `get_raid_data_bulk` the raid roster, as packed records (Buff, spawn, extended target, ability and raid
  records below). `server_info` gives the
  server this login is on (the saved server's name, host and port, the world's name and address), so a plugin for
  one server can tell it from another that sends the same packets.
- **take item clicks** (`item_right_click`, then the client's own with `right_click_item`, which needs `actions`)
  and fill the merchant list's level column (`list_level`).
- **act** (with `actions`): close the spell book, end the global spell recovery, show another of several pets in the
  pet window (`set_window_pet`, the client's choice only), open an item's display (`inspect_item`), open the
  bank window at any spawn or at the player itself, without a banker's class and range checks (`open_bank`), change the
  camera's zoom limit (`set_max_camera_distance`), use one of the player's abilities by name as the Ability Book's row
  does (`use_ability`: a skill, a discipline or an activated AA; `kind` is `HS_ABILITY_SKILL`,
  `HS_ABILITY_DISCIPLINE` or `HS_ABILITY_ALT`), and change the hot buttons: `hot_button` reads any button of
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
call, since a client older than the call has a shorter table; `use_ability`, `ability_state`,
`get_ability_data_bulk` and the other calls under "added later in ABI 3" in `sdk/hailstone_plugin.h` are such calls. The structs a plugin passes to be filled (`HailstonePlayer`, `HailstoneSpawn`,
..., `HailstoneWebState`) carry their size too and the client fills what both know; `HailstoneWebState`'s `size` is a `size_t`
(as ABI 3 shipped it), the others' a `uint32_t`.

The host contains C++ allocation/provider exceptions at its SDK entry points and returns the ordinary zero/void
failure result. It does not retry actions or promise to undo side effects completed before an exception.

The cursor item's origin: `cursor_origin(host)` (with `read`; added later in ABI 3, so test
`HAILSTONE_HOST_HAS(host, cursor_origin)` first) says how the item on the cursor now got there, for a plugin that
puts away or destroys what the server hands the player:

| Value | Meaning |
|---|---|
| `HAILSTONE_CURSOR_ORIGIN_UNKNOWN` (0) | not known, or nothing on the cursor, or the plugin lacks `read`. The item was on the cursor at the zone-in (OP_CharInventory), replaced an item already on the cursor, or got there some way the client does not mark. Treat it as the player's: never destroy it unattended. |
| `HAILSTONE_CURSOR_ORIGIN_INVENTORY` (1) | the player picked it up from their own inventory: a click on a slot, or part of a stack split off with the quantity slider. |
| `HAILSTONE_CURSOR_ORIGIN_SERVER` (2) | the server put it on the empty cursor. |

SERVER goes by how the item arrives, not by whether the player chose it. Traced in EQEmu's zone code: summoned items,
tradeskill results and quest rewards (`Client::SummonItem`), and anything else EQEmu queues on the cursor
(`Client::PushItemOnCursor`: the cursor queue's item packet type 0x6a, and the queue's next item moved up with
OP_MoveItem), arrive that way; so do a ground item the player clicks to pick up (`Object::HandleClick`) and an item
looted onto the cursor (`Client::PutLootInInventory`), which come as item packets of type 0x67 to the empty cursor.
An item packet or a server OP_MoveItem onto a cursor that already holds an item (a merge, a swap) reads UNKNOWN.

The answer belongs to the item it was noted for: every change to the cursor slot counts
(`game::Inventory::cursor_placement`), and the origin is kept with the count it was noted at, so when the cursor
changes by a path that marks nothing the answer is UNKNOWN, never an earlier item's origin. Read it beside
`get_item` on the cursor, then act with one of the two commands below, giving the id just read; each does nothing
when the cursor no longer holds an item of that id, so an item the player put there in the meantime is not touched
(the check is by item id, so it cannot tell two items of the same id apart):

- `/destroycursor <id>`: destroys the item on the cursor (OP_MoveItem to nowhere, as `/destroy` without its
  question). Never coin, never a bag (it may hold other items), never an item with a Keep mark, and only in a zone.
  It does not refuse a real-estate item (docs/unknowns.md).
- `/stowcursor <id>`: puts the item on the cursor into the first free general slot, else the first bag with room
  that takes it, never a worn slot; "There are no open slots for the held item in your inventory." when there is none.

Both need `actions`, as `/destroy` does. The id is a plain decimal number above 0; anything else does nothing.

Buff, spawn, extended target, ability and raid records: `get_buff_data_bulk(host, out, out_size)`, `get_pet_buff_data_bulk(host, out, out_size)`,
`get_spawns_data_bulk(host, out, out_size)`, `get_xtarget_data_bulk(host, out, out_size)`,
`get_target_buff_data_bulk(host, out, out_size)`, `get_avail_aa_data_bulk(host, out, out_size)`,
`get_avail_disc_data_bulk(host, out, out_size)`, `get_buff_name_data_bulk(host, out, out_size)`,
`get_ability_data_bulk(host, out, out_size)` and
`get_raid_data_bulk(host, out, out_size)` (with `read`; added later in ABI 3, so test `HAILSTONE_HOST_HAS(host, <field>)` first;
`out` is an `unsigned char*` and `out_size` an `int32_t`) write records into `out` and return the bytes written.
The records are the SDK's packed structs (`HS_Bulk_BuffData`, `HS_Bulk_PetBuffData`, `HS_Bulk_SpawnHeader`,
`HS_Bulk_SpawnFixedRecord`, `HS_Bulk_XTargetData`, `HS_Bulk_TargetBuffFixedRecord`, `HS_Bulk_BuffNameFixedRecord`,
`HAILSTONE_ID_NAME_RECORD`, `HS_Bulk_AbilityRecord`, `HS_Bulk_ListHeader`, `HS_Bulk_RaidHeader`, `HS_Bulk_RaidData`, all in `#pragma pack(push, 1)`), in the machine's byte order (little-endian on every
platform Hailstone runs on) and unaligned. For speed, the bulk reads are the one place where a struct may be cast
onto the buffer, by the client writing it and by a plugin reading it: read each field through the packed struct
(`rec->level`), which the compiler reads unaligned, and never keep a plain pointer or reference to a field (`&rec->x`),
which may fault on a processor that needs aligned reads. `ByteReader` or `memcpy` work too. Without `read` they return 0
and say so in the chat window.

The reads that give a list of any length (`get_target_buff_data_bulk`, `get_avail_aa_data_bulk`,
`get_avail_disc_data_bulk`, `get_ability_data_bulk` and `get_raid_data_bulk`) share one rule for a buffer too small:
they write as many whole records as fit, in order, and their header states both `count`, the records in this buffer,
and `total`, how many the client holds. `count < total` means `out_size` was too small; call again with a larger
buffer. The first four lead with `HS_Bulk_ListHeader` (`HS_BULK_LIST_HEADER_SIZE` = 8 bytes: i32 `count`, then i32
`total`), the raid read with its own `HS_Bulk_RaidHeader`, whose `count` and `total` mean the same. Below the header's
size nothing is written and 0 comes back; with nothing to list the header alone goes out, `count` 0 and `total` 0.

- `get_buff_data_bulk`: one record of `HAILSTONE_BUFF_RECORD_SIZE` (29) bytes per buff slot, slot 0 first, in the
  client's order: the long buff slots, then the song slots, then the one discipline slot, always last. RoF2 has 42
  long buff slots (0 to 41), 20 song slots (42 to 61) and the discipline slot (62), 63 in all. `out_size` must hold
  every buff and song slot, 1798 bytes (62 records), or 0 comes back, so a plugin gets all of them or nothing; the
  discipline slot comes too when it holds one record more, 1827 bytes (63). The counts are the client's, not the
  SDK's: tell a record's kind by its last byte, not by its position, and pass a buffer with room to spare (4096
  bytes holds 141 records).

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | spell id; 0 an empty slot, whose other fields are all 0 |
  | 4 | i32 | the caster's spawn id |
  | 8 | i32 | tics left: the server's count less the 6 s ticks since it came (none while the buff timers are on hold); a permanent buff is negative, the server's count as sent and never counted down (EQEmu: -1 for duration formula 50, -4 for an aura's buff, formula 51, and -1000 for one a script made permanent) |
  | 12 | i32 | hits left |
  | 16 | i32 | the spell's type: 0 detrimental, 1 beneficial, 2 beneficial group-only (0 for a spell not in spells_us.txt) |
  | 20 | i32 | counter type of the spell's first counter effect: -1 none, 0 disease, 1 poison, 2 curse, 3 corruption. An empty slot also has 0 here, so test the spell id before reading it as disease |
  | 24 | i32 | that effect's base: the counters the buff starts with (MacroQuest's `Spell.CounterNumber`); 0 with no counter effect |
  | 28 | u8 | the slot's window: 0 buff (`HAILSTONE_BUFF_SLOT_BUFF`, RoF2's slots 0 to 41), 1 song (`HAILSTONE_BUFF_SLOT_SONG`, 42 to 61), 2 discipline (`HAILSTONE_BUFF_SLOT_DISC`, the last slot, 62); 0 in an empty slot. Taken from the slot, not the spell |

  The counters left (the slot's data from the server) are not included.

- `get_buff_name_data_bulk`: `get_buff_data_bulk` with the caster's name in place of the caster's spawn id. The
  records come in the same order with the same fields, but each is a fixed part (`HS_Bulk_BuffNameFixedRecord`,
  `HAILSTONE_BUFF_NAME_RECORD_SIZE` = 29 bytes) followed at once by the caster's name, `casterNameLength` bytes with
  no terminating NUL; the next record starts after the name, so walk them one by one. An empty slot is 29 zero bytes
  with no name after it. `out_size` must hold every buff and song slot with their names (at least 1798 bytes, plus
  the names), or 0 comes back; the discipline slot comes too when its record and name fit after them. 8192 bytes
  holds every slot with names of the server's 64-byte limit.

  The name is the one the server last sent for the slot (`OP_BuffCreate`), the caster's name without its trailing
  digits. EQEmu keeps it while the player stays in the zone, even after the caster dies or leaves; after zoning it
  keeps it only when the caster is a player already in the new zone, and sends none otherwise (NPC casters
  included), so an empty name is common.

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | spell id; 0 an empty slot, whose other fields are all 0 (and no name follows) |
  | 4 | i32 | `casterNameLength`: the bytes of the caster's name that follow; 0 when the server sent none |
  | 8 to 28 | | as `get_buff_data_bulk`'s: tics left, hits left, spell type, counter type, counters, slot's window |
  | 29 | bytes | the caster's name, `casterNameLength` bytes |

- `get_pet_buff_data_bulk`: one record of `HAILSTONE_PET_BUFF_RECORD_SIZE` (12) bytes per buff in the pet window, in
  the order the server sent them (0 bytes with no pet), as many as fit in `out_size`: i32 spell id, i32 tics left
  (counted down as above), i32 the spell's type (as above). An entry with spell id 0 is all zeros. The pet window's
  slot number is not included.

- `get_spawns_data_bulk`: every spawn in the zone, as a 16-byte header followed by one record per spawn. Records are
  added until fewer than 4096 bytes are left in `out`, or until the next record (its fixed part and seven strings)
  would not fit in what is left: below 4096 bytes it returns 0, and it needs at least 4112 bytes to write any record
  and a few hundred bytes per spawn on top. Only whole records are written; the header's count says how many follow.

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

  Then seven strings, each a u16 length followed by that many bytes with no terminating NUL: the body type's
  description, the clean name, the display name (the same text as the clean name, as `${Spawn.CleanName}` and
  `${Spawn.DisplayName}` give them), the name, the race's name, the suffix, and the spawn type's
  description.

  Flags (bit: meaning): 0 AFK, 1 aggressive (player state 0x04 or 0x08), 2 anonymous, 3 buyer, 4 dead, 5 ducking,
  6 feigning death, 7 GM, 8 invisible, 9 summoned, 10 levitating, 11 link dead, 12 on a mount, 13 moving (a run speed
  other than 0), 14 named, 15 roleplaying, 16 sitting, 17 sneaking, 18 standing, 19 stunned (player state 0x20),
  20 targetable, 21 trader, 22 underwater. Bits 8, 17 and 22 are never set at present.

- `get_xtarget_data_bulk`: one record (`HS_Bulk_XTargetData`) of `HS_BULK_XTARGET_RECORD_SIZE` (16) bytes per extended target slot, slot 0
  first, as many as fit in `out_size` (only whole records; 0 bytes before the server sent the list). The slots are
  the same ones `get_xtarget` reads by index, a slot the server's extension hides from the window included. RoF2 has
  at most 20 slots (EQEmu: 5 plus the AA's), so 512 bytes is plenty.

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | the slot's type, as `HailstoneXTarget.type`: an `HS_BULK_XTARGET_*` value (`enum HS_BULK_XTargetTypes`, the protocol's 0 to 26: 0 empty, 1 auto hater, 2 / 3 a specific PC / NPC, ... 25 / 26 the mercenary and its target) |
  | 4 | i32 | the slot's spawn id, 0 for none |
  | 8 | i32 | the aggro meter's percent for the slot's spawn, as the server sent it (entries 7 to 26 of the meter, the extended target window's aggro labels; on EQEmu an NPC's is our hate against its current target's, a player's is its hate against our target's top). 0 with no spawn in the zone, without the meter (EQEmu's `Character:EnableAggroMeter`), or for a slot past the 20th |
  | 12 | i32 | the spawn's HP percent, as the extended target window draws it: the player's own from its points (items' HP included), anyone else's as the server last sent it (EQEmu sends it to everyone with the spawn on their list when the percent changes); 0 with no spawn in the zone or for a corpse |

- `get_target_buff_data_bulk`: the current target's buffs, the list the target window's buff icons are drawn from
  (what `OP_TargetBuffs` last gave for that spawn): an `HS_Bulk_ListHeader`, then one record per buff the server sent, in the order
  the client keeps them, each with its slot. EQEmu sends only the slots that hold a spell, in slot order, and resends
  the whole list when one fades, so against it the records are packed (no empty ones) and the slot numbers can skip;
  an entry with no spell is left out in any case, and not counted in `total`. The read checks only that the list is the current
  target's, not whose it is: with the player itself targeted it gives whatever `OP_TargetBuffs` gave for the player,
  although the client's own target buff list draws nothing then.

  The header's `total` is the number of the target's buffs the client holds, all of them, and its `count` the records
  written after it. Each record is a fixed part (`HS_Bulk_TargetBuffFixedRecord`, `HAILSTONE_TARGET_BUFF_RECORD_SIZE`
  = 20 bytes) followed at once by the caster's name, `casterNameLength` bytes with no terminating NUL; the next record
  starts after the name, so walk them one by one rather than by a fixed stride. As many whole records, names
  included, are written as fit after the header, in order, none after the first that does not fit, so `count <
  total` means `out_size` cut the list short. The return value is the bytes written; below 8 bytes nothing is written
  and 0 comes back. With no target, while the buffs the client holds are still another spawn's (the server has not
  yet sent the new target's; EQEmu sends them only where the target window shows buffs, so an NPC target on a server
  without that shows none), or with no game connection, the header alone goes out (count 0, total 0, 8 bytes). A
  name is as long as the server sent it, so pass room for the worst case: EQEmu gives an NPC 60 buff slots by default
  (`Spells:MaxTotalSlotsNPC`) and keeps a caster's name in 64 bytes, at most 63 without the NUL, so
  8 + 60 x 83 = 4988 bytes; 8192 bytes holds any target on a stock server.

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | `count`: the records that follow in this buffer |
  | 4 | i32 | `total`: the number of the target's buffs the client holds |
  | 8 | records | the records, one after another, as many as fit |

  Each record:

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | spell id (never 0: entries with no spell are left out) |
  | 4 | i32 | tics left: the server's count less the 6 s ticks since it came, or the count as sent while its countdown is held as the target window holds it (a suspendable spell in a zone that suspends buffs; with the player its own target, also the player's buff-timer hold, docs/mechanics.md); a permanent buff negative, as above |
  | 8 | i32 | the spell's type: 0 detrimental, 1 beneficial, 2 beneficial group-only (0 for a spell not in spells_us.txt) |
  | 12 | i32 | the target's buff slot, as the server sent it (EQEmu: the mob's own slot index, not mapped to RoF2's) |
  | 16 | i32 | `casterNameLength`: the bytes of the caster's name that follow; 0 when the server sent none |
  | 20 | bytes | the caster's name, `casterNameLength` bytes |

- `get_avail_aa_data_bulk` and `get_avail_disc_data_bulk`: an `HS_Bulk_ListHeader`, then `count` records, each an
  id and its name. Each record is a fixed part (`HAILSTONE_ID_NAME_RECORD`, `HAILSTONE_ID_NAME_RECORD_SIZE` = 8 bytes)
  followed at once by the name, `nameSize` bytes with no terminating NUL; the next record starts after the name, so
  walk them one by one. As many whole records, names included, are written as fit after the header, in order, none
  after the first that does not fit; `count` says how many follow and `total` how many the list holds, so `count <
  total` means `out_size` cut it short. Below 8 bytes nothing is written and 0 comes back. With nothing to list,
  before the character's profile, or with no game connection, the header alone goes out (count 0, total 0, 8 bytes).
  RoF2's lists have at most 300 entries each; 16384 bytes holds either with ordinary names.

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

  - `get_avail_aa_data_bulk`: the alternate abilities the character owns (bought or granted, passive and activated
    alike), one per owned ability, by its ability (group) id, the id every rank of it shares and `aa_value` takes;
    not the rank id the server lists. The name is the owned rank's title (dbstr_us.txt type 1, as the AA window shows
    it), empty when the game folder's file lacks it. In the order of the owned list (the profile's, then
    `OP_RespondAA`'s). An owned
    rank the AA table does not hold (the server has not sent it in `OP_SendAATable`) has no known group id and is left
    out, as `aa_value` gives 0 for it.
  - `get_avail_disc_data_bulk`: the character's combat abilities (disciplines), by spell id: the profile's
    discipline list, the one `combat_ability` reads by index, without its empty (0) entries, in list order. The name
    is the spell's, empty for a spell not in spells_us.txt. Before the profile count and total are 0.

- `get_ability_data_bulk`: the Ability Book's usable entries in one call, with their readiness, so a plugin does not
  run `ability_state` once per entry. An `HS_Bulk_ListHeader`, then `count` records, each an
  `HS_Bulk_AbilityRecord` (`HAILSTONE_ABILITY_RECORD_SIZE` = 32 bytes) followed at once by the name, `nameSize`
  bytes with no NUL; the next record starts after the name. Only whole records are written, in the book's order:
  the owned activated AAs, then the listed disciplines, then the usable skills (the combat page's, then the abilities
  page's). Passive entries are not listed, so every record is one the book can use; an owned AA rank the AA table
  does not hold has no group id and is left out, as `get_avail_aa_data_bulk` leaves it out. Match an entry by `kind`
  and `id`, not by its position.

  | Offset | Type | Field |
  |---|---|---|
  | 0 | i32 | `kind`: `HS_ABILITY_SKILL` (0), `HS_ABILITY_DISCIPLINE` (1) or `HS_ABILITY_ALT` (2) |
  | 4 | i32 | `id`: a skill's id, a discipline's **spell id** (not its profile slot), or an AA's ability (group) id |
  | 8 | i32 | `ready`: 1 its reuse has run out, 0 on reuse |
  | 12 | i64 | `remainingMs`: until it is ready, 0 when ready |
  | 20 | i64 | `reuseMs`: its reuse time |
  | 28 | i32 | `nameSize`: the bytes of the name that follow; 0 when the client has no name for it |
  | 32 | bytes | the name, `nameSize` bytes |

  Readiness is what `ability_state` gives: an AA's own reuse (with the SPA 264 reduction applied), a discipline's
  reuse timer, and a skill's timer rounded up to the whole second the book shows, all times in ms. Before the
  character's profile, or with no game connection, the header alone goes out (count 0, total 0), as
  `get_avail_aa_data_bulk` and `get_avail_disc_data_bulk` do; a plugin without `read` gets 0 bytes. The list is
  bounded by what the character has: at most 28 activated skill ids in the client's table, 300 discipline slots, and
  the owned AAs the server sends (300 on a stock server). A 65536-byte buffer holds every entry with ordinary names;
  `count < total` says the buffer was too small, so retry with a larger one. Added later in ABI 3: test `HAILSTONE_HOST_HAS(host, get_ability_data_bulk)` first.

- `get_raid_data_bulk`: the raid roster, a 40-byte header followed by one `HS_Bulk_RaidData` record
  (`HS_BULK_RAID_RECORD_SIZE` = 124 bytes) per member, in roster order, as many whole records as fit in `out_size`.
  The header is written whenever `out_size` holds it: outside a raid it says `active` 0 and `count` 0. The records
  are 0-based (`slot` 0 is the first member), while MacroQuest's `Raid.Member[1]` is the first; `leaderIndex` and
  `targetIndex` are 0-based too, `HS_BULK_RAID_NO_INDEX` (0xffffffff) for none.

  A reader starts the records at the header's `size` and steps by its `recordSize`, for `count` records. A later
  client may append a field to the record: read `recordSize`, never a fixed stride, and read only the fields the
  record's size covers. `version` changes only when an older reader can no longer read the header or a record at
  all. `total` is the roster's member count, so `count < total` means the buffer cut the roster short and the caller
  can retry with a larger one.

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
  4 waypointer, 5 looter, 6 reserved (this client keeps no offline flag), 7 present in this zone, 8 the player. A
  main assist's own member flag is used first, then the raid's assigned assist names. A member who is not in the
  zone has no spawn id, position or percentages; the roster's level and class still answer.

  The `LOOTER` flag follows the loot type: 1 Leader Only, 2 Leader and Group Leader, 3 Leader Selected, 4 Entire
  Raid. That differs from the raid window's own `looter()` check for the member menu, which only tests the leader
  and the listed looters; the flag here answers "may this member loot under the current type".

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
local p = hs.Plugin("MyPlugin")
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
`hailstone.xtarget(r.n + 1)` in a script). Every slot has a row, an empty one too, but `draw` and `style` are called
for filled rows only. The rows sit in a scrolling child window of the frame, which clips them (`width` is the frame's
width until the player resizes it); the screen coordinates `draw` is given already follow the scroll. A window named "Extended Target" that calls it covers the pack's:

```lua
local ok, xtarget = pcall(dofile, "lib/xtarget.lua")   -- an older client has no lib/xtarget.lua
ui.window("Extended Target", function()
  local p = hs.Plugin("MyScript")
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
(`ui.window`, the `ui` and `hs` tables). A server ships them in its `client_files/plugins/<name>/ui/` (Lua is
taken there and in `scripts/` only).

The plugin gives its windows data and takes their actions through functions they call:

```lua
local p = hs.Plugin("MyPlugin")        -- nil while it is not loaded
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
  `hs.PluginOptions`, `hs.SetPluginOption`), and the plugin keeps the value itself.
- **menu entries**: `add_menu(label, command)`. The EQ menu (a Plugins submenu) and the gamepad pack's More page
  list them
  (`hs.PluginMenu`, `hs.PluginMenuClick`), and choosing one runs the command, usually a command of the plugin's that
  shows its window. The command is the plugin's, so it runs with the plugin's capabilities even though the player
  chose it: an entry whose command needs more than its plugin may do is refused like any command the plugin sends.

Each window folder belongs to an owner, told apart by kind as well as by name: `plugin:<name>` for a compiled
plugin's, `script:<name>` for a Lua plugin's (a script's `ui/`). A script named after a compiled plugin (a script
add-on called `nms` beside the NMS server's plugin) gets neither the plugin's capabilities nor its folder, functions,
options or menu entries; each owner's stay its own and go only with it. `hs.Plugin(name)` with a bare name finds a
compiled plugin's functions before a script's of that name; `hs.Plugin("script:<name>")` names the script's.
`hs.PluginOptions` and `hs.PluginMenu` give each entry's `owner` beside its `plugin` name, and `hs.SetPluginOption`
takes either. The add-on manager installs no script with the name of a compiled plugin in the player's folders, nor
the other way round (docs/addons.md).

These windows share the pack's Lua state but not its authority. A plugin folder's code (its files' top level, its
windows, and any function it took from `hs` or `ui`, whoever calls it later) runs as the plugin: with its
capabilities for whatever it does through the client (a command it sends with `hs.ChatSubmit`, a script it starts,
a web page), as `LuaUi::caller_tag` and a `plugin::CallerScope` say. A server's own plugins and scripts have every
capability (above), so their windows can do what the pack's do; a player's add-on gets at most what its manifest
declared (docs/addons.md). Whatever its capabilities, a plugin's code:

- has globals of its own: it reads the pack's globals (the shared state tables the pack's libraries keep, such as
  `modern_colors` or `GPSTATE`), but a global it sets is its own, and so is every global a pack library it runs with
  `dofile` sets;
- sees `hs`, `ui` and the standard libraries read-only, so it cannot change what the pack's windows call;
- reaches only its own owner's exported functions, options and menu entries: `hs.Plugin` gives nil for another
  plugin (and an object for another that it finds elsewhere does nothing), `hs.PluginOptions` and `hs.PluginMenu`
  list only its own (`hs.PluginMenuClick(i)` numbers its entries as that list does), and `hs.SetPluginOption` and
  `hs.PluginMenuClick` leave another's alone, since those run the other plugin's code
  with the other's capabilities. The pack's own windows reach every plugin's;
- keeps settings of its own: `ui.SetSetting(key, v)` writes `plugin.<owner>.<key>` in the pack's settings file (a
  window's place under `hud.` is the character's, as the pack's are), `ui.GetSetting(key)` reads that, else the
  pack's own key of that name (the palette, the place of a window it took over), and neither reaches the pack's test
  hooks (`test.*`, which click windows' children), the windows' switches (`enabled.*`) or another plugin's keys
  (`ui.SetSetting` refuses, and returns false for, a key with `=` or a line break or a value with a line break, since
  the file is one `key=value` a line and such text would be read back as other keys); a plugin keeps at most 1024
  keys, each at most 256 bytes with a value of at most 16 KiB (past that `ui.SetSetting` returns false, and a key it
  has can still change); `ui.DisableWindow(name)` turns off only its own window of that name; it has at most 128
  windows (`ui.window` raises past that);
- runs for at most a second at a time (a window's draw, its file's load, a function of its that the pack calls) and
  the pack's own code for five: code that runs longer is stopped with an error, which a `pcall` of its own cannot
  catch, and its window is turned off until its file changes. A long string match (`string.find`, `match`,
  `gmatch`, `gsub` with a pattern that backtracks) counts too, and is stopped the same way. The UI's Lua state as a
  whole holds at most 1 GiB, and an allocation past that fails in the code that asked for it; a plugin's code gets
  three quarters of it, so what plugins hold leaves the pack's windows room. `collectgarbage` takes only `"count"`
  and `"step"`;
- uses the player's clipboard (`ui.GetClipboardText`, `ui.SetClipboardText`) only when its owner has every
  capability, as for a script: it holds whatever the player copied, a password too;
- gets nil from the pre-game screens (`hs.LoginScreen` and the others hold the typed or saved password) and cannot
  act on them, may not run, stop or pause scripts or change the autorun list (`hs.ScriptRun` and the others), and
  opens web pages as its owner (`plugin:<name>`, or `script:<name>`: a Lua plugin's windows share the script's
  `mq.web` tabs) with its own permissions.

What plugin windows may do: reading the game and drawing need no capability beyond `ui`; an `hs` function that acts
needs the plugin's capability for it, from the plugin's own code or from a pack function it calls (the list is
`app/lua_ui_caps.cpp`, and a function missing from it is taken to act). Refused, it does nothing and returns nil, with
a line in the log once.

| Needs | `hs` functions |
|---|---|
| never | the player's one-shot keys and requests, which the pack's windows take once: the chat input's (`hs.ChatTakeFocus`, with a pending Link All; `hs.ChatPage`, `hs.ChatHistory`) and the Lua social editor's (`hs.LuaSocialEditRequest`); nil to a plugin's code. `hs.ReplaceWindow` and `hs.WindowShown` do not put away the client's consent prompt nor its dialogs that act when answered (a confirmation, among them a destroy's; a trade, give, quantity, bazaar purchase, group invitation, respawn, large dialog or task offer) for a plugin |
| nothing | everything that reads or draws (`hs.Item`, `hs.AllItems`, `hs.ItemStats`, `hs.ItemKept`, `hs.ItemJob`, `hs.ChildState`, `hs.ListRow`, `hs.SpellBook`, `hs.DrawMap`, ...), a window's place and size (`hs.ReplaceWindow`, `hs.WindowPlaced`, `hs.SizeWindow`), the map's zoom and pan, opening a menu (`hs.WindowContextMenu`, `hs.ChatMenu`, `hs.HotBarMenu`; choosing a row is below), the close-top-window key's press (`hs.TakeCloseTopWindow`) |
| `commands` | `hs.ShowWindow`, `hs.SetOption`, `hs.SetUiIni`, `hs.ShowSpell`, the chat windows' setup (`hs.ChatNewWindow`, `hs.ChatSetFilter`, ...), the spell sets' and the autoskill list's editing, the map's find path, the Find window (`hs.FindOpen`) and its guide path (`hs.FindNpcPath`), the social editors, `hs.gamepad.set_*` and `hs.gamepad.request_focus` (menu mode, as `set_mode`) |
| `target` | `hs.FindNpcTarget` (the Find window's row) |
| `move` | `hs.FindNpcWalk` (the Find window's Walk there) and `hs.ZoneExitWalk` (the zone finder's), both `/nav` walks. (`/nav` itself needs `move`, and the doors a walk opens on its way come with it; `/nav door ... click`, which clicks the door on arrival, needs `actions` too) |
| `actions` | a stock window's controls (`hs.ClickChild`, `hs.ClickChildAt`, `hs.WindowButton`, `hs.StmlLink`, `hs.SetWindowText`, `hs.ListClick`, `hs.ListHeaderClick`, `hs.ComboSelect`, `hs.SetSlider`, `hs.SetTabPage`), the menus' rows (`hs.CascadeClick`, `hs.GameMenuClick`), items and the cursor (`hs.ClickSlot`, `hs.BagSlotClick`, `hs.BankSlotClick`, `hs.PlaceClick`, `hs.PickUp`, `hs.SortItems`, `hs.SellJunk`, `hs.BulkItems` (sell, tribute, destroy or withdraw chosen items), `hs.StopItemJob`, `hs.SetItemKept` / `hs.SetItemsKept` (a Keep mark taken off lets the item be destroyed), the parcels, `hs.gamepad.drop_cursor`), hot buttons and gems (`hs.ClickHotButton`, `hs.PressButtonFrom`, `hs.ClearCursorButton`, `hs.SpellGemClick`), spells and abilities (`hs.SpellBookClick`, `hs.SpellBookMemorize`, `hs.SpellBookSwap`, `hs.MemorizeSpellSet`, `hs.UseSkill`, `hs.AbilityBookUse`, `hs.AbilityBookAutoskillEnable`; `hs.AbilityBookAutoskill` and `hs.AbilityBookAutoskillRule`, which choose what the autoskill list uses later, need `commands` too), socials (`hs.PressLuaSocial`, `hs.PressSocial`, `hs.StopSocial`, `hs.SocialToCursor`; `hs.SetLuaSocial` and `hs.LuaSocialDryRun`, which runs the code it is given with the server's Lua social capabilities, need `commands` too) |
| checked by the function | the plugins' (above), the scripts' (refused; `hs.Scripts` lists only a script of the plugin's own name, with no folders), the pre-game screens' (nil), the web's (`web`); `hs.ChatSubmit`'s line goes through the command filter, which holds it to the plugin's capabilities as any command it sends (plain text needs `chat`, `/cast` needs `actions`) |

What stays shared: a plugin can still write into the tables the pack keeps in its globals (that is how its windows
take part in the pack's focus and palette), so a plugin can upset how the pack's windows look or behave. It sees
each of those tables through a view: reads and writes go through to the table, but a function the plugin writes there
(or into a table of its own that it puts there) runs as the plugin whenever the pack calls it, does nothing once the
plugin unloads, and cannot be called by another plugin's code; the view's metatable cannot be read or changed,
`rawset` refuses it, and a plugin's tables cannot have a `__gc` finalizer. `next` and `rawget` see nothing through a
view (`pairs`, `ipairs`, `#` and `table.*` work). The pack's tables reach the plugin only as views, whatever the way:
a pack function it calls (one of the pack's globals, one in a shared table, one the pack put in `ui`) returns views,
and raises them; the tables the pack passes the plugin's functions as arguments are views (its own tables handed back
are its own); and `getmetatable` gives a metatable the plugin did not set itself (a pack table's, a userdata's) as a
view. It cannot get the pack's own `hs` or `ui` functions to do something under the player's authority. What is not
covered: a function or table the plugin hands a pack function as an argument, or returns to the pack from a
function the pack calls, which the pack may keep as it is (docs/todo.md). A player who copies the same files into a UI
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

## Upscaler providers

An upscaler provider is a temporal upscaler from outside the client (Intel XeSS, AMD FSR 4 ...),
built against `sdk/hailstone_upscaler.h`. It is not a game plugin: it has no host table, no capabilities and no
callbacks and sees nothing of the game. Servers do not send providers; an opted-in add-on policy can require, allow
or block one by its repository, checked as the character enters that server ("Opt-in add-on policies" below). The player installs it with the add-on
manager (an add-on of the kind `upscaler`, docs/addons.md "Upscalers") and picks it under Options, Graphics,
Upscaler (`display.upscaler = <name>`); it loads at the next start. docs/graphics.md "Upscaler providers" says what the
client does around it.

**Licence.** `sdk/hailstone_upscaler.h` is MIT, and NOTICE.md's plugin and add-on exception covers a provider that
talks to Hailstone only through it, whatever its licence, and whatever closed runtime it loads (a vendor's SDK
library beside it). Meeting that runtime's own terms (its notices, what it says about redistribution) is the
provider's business.

**Files.** `upscalers/<name>/<name>.dll` (`.so`, `.dylib`), and beside it whatever it loads. The client loads it from
a copy of that folder in `cache/upscalers/` (the header's `folder`; load a runtime from there, by its full path) and
gives the folder itself as `config_dir` for its settings and logs.

**The calls**, all on the render thread:

| | |
|---|---|
| `hailstone_upscaler(host)` | the one export: its table (`HailstoneUpscaler`), or NULL to stay out. `host` has the client's ABI, the two folders, a log function and a `vkGetInstanceProcAddr` (callable from `instance_requirements` on) |
| `instance_requirements` | before `vkCreateInstance` (Vulkan 1.3): the instance extensions it needs and the lowest Vulkan version |
| `device_requirements` | for the picked GPU, before `vkCreateDevice`: device extensions, and features set to `VK_TRUE` in a `VkPhysicalDeviceFeatures2` chained to the Vulkan 1.1, 1.2 and 1.3 feature structs. A feature core Vulkan has goes in through its core field; one of an extension that has none (`VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT`, which XeSS needs) as a struct of the provider's own added to the chain, valid until `create`, whose support the provider checks itself. Say no for a GPU it cannot run on |
| `create` | the device made: the instance, physical device, device, the one queue and its family, both `Get*ProcAddr`, and `HAILSTONE_UPSCALER_DEVICE_REACTIVE` when every frame will carry a reactive mask |
| `resize` | the render and view sizes, before the first frame and when they change; the device is idle |
| `evaluate` | each frame: record the upscale into the command buffer given (outside rendering) |
| `destroy` | before the device goes, when `create` succeeded |

A 0 or NULL from any of them, or an exception out of one, drops the provider: the client falls back to FSR 3.1, or
its own temporal upscaler where the GPU cannot run FSR 3.1, and logs why. A failed `evaluate` drops it from the next
frame.

**A frame** (`HailstoneUpscalerFrame`). Images cross as integers (`VkImage` and `VkImageView` as `uint64_t`, formats,
layouts and usage as `uint32_t`), each with its size, usage, aspect and the layout it is in:

| | |
|---|---|
| `color` | RGBA16F at the render size: the scene in linear HDR before the exposure and the tonemap (`SAMPLED`, `TRANSFER_SRC`) |
| `depth` | reverse Z (1 near, 0 far), the depth aspect (`SAMPLED`) |
| `motion` | RGBA16F, red and green: `previous_uv - uv`, in UV of the render size (x right, y down), without the jitter; `motion_scale` (the render size) turns it into pixels |
| `reactive` | R8, 0..1: how much this frame's colour should replace the history (particles, spells, other blended draws); none (image 0) without `independentBlend` |
| `output` | RGBA16F at the view's size (`STORAGE`, `SAMPLED`, `TRANSFER_DST`), in `GENERAL`, old contents undefined: write all of it and leave it in `GENERAL` |

The inputs are in `SHADER_READ_ONLY_OPTIMAL`, written by draws and made visible to every stage before the call. The
jitter is in render pixels in AMD FSR's convention (the point pixel `t` samples is `t + 0.5 - jitter`), with the
length of its cycle. Also given: the camera's near and far planes and vertical field of view, the client's exposure,
the frame time, `display.sharpness`, the frame slot, and `HAILSTONE_UPSCALER_FRAME_RESET` on a cut, a teleport, a new
zone or new targets. A provider may bind pipelines, sets and push constants of its own and move an image to another
layout within the call, as long as each image is back in its layout when it returns. With
`HAILSTONE_UPSCALER_SHARPENS` it sharpens by `sharpness` itself; without it the client sharpens its output.

**Compatibility.** Every table carries its size and only grows at its end (`HAILSTONE_UPSCALER_ABI` is 1). The client
refuses a provider built for a newer ABI.

**The example.** `plugins/example/upscaler/example.c` is a whole provider in plain C: it checks that the GPU can blit
RGBA16F, then stretches the colour to the output with a linear blit (no accumulation). The client's build puts it in
`build/hailstone/upscalers/example/`; `--upscaler example` in the zone viewer, or `display.upscaler = example`, runs it.
It is built a few more ways as test fixtures (`tests/upscaler_provider_test.cpp`).

## Native world nameplate canvases

Plugins with `read`, `ui` and `extend` can replace whole world captions through the same nameplate canvas used by Lua.
Check `HAILSTONE_HOST_HAS(host, nameplate_spell_icon)` before using this append-only ABI 3 service. The C records and
flags are in `sdk/hailstone_nameplates.h`.

- Register one callback with `nameplate_register(host, fn, userdata)`. Unregister before registering a replacement.
- Each callback sees the actors eligible for a normal world caption, with the target first and remaining candidates
  in depth order. Presence flags distinguish an unknown reading from a known zero. Treat the frame, candidates and
  counted UTF-8 strings as borrowed until the callback returns.
- `nameplate_measure_text` and `nameplate_spell_icon` are available during the callback. An image can use a texture
  returned to that provider during the current callback.
- To claim a caption, call `nameplate_submit` with the current frame serial and zero-based candidate index. The host
  copies the model and commands immediately. Set `HAILSTONE_NAMEPLATE_HIDE` to claim an empty caption, or omit the
  model to leave it eligible for the stock caption and other providers. Duplicate and stale claims are refused.
- The shared canvas checks geometry budgets, depth, and competing claims. A clickable model uses the same guarded
  manual targeting path as a Lua canvas. Unregistering, unloading, deactivation, or losing a required capability
  retires the provider and its click areas.

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
(`mq.web.messages(tab)`, `hs.WebMessages(tab)`, `web_message`; a message is at most 1 MB, and at most 256 and 8 MB
wait, the oldest going first; `web_message` with a buffer takes the message off the queue even when it is cut, and
with `out` NULL gives only its length and leaves it queued, so a plugin can make room for a long one first), and the
owner's `post` calls the page's `hailstone.onmessage(text)` when it set one. A tab is closed (Refused, with why)
when its owner loses `web` or the server's `[web] allow` changes (docs/browser.md "Rules that change"). A page cannot run commands or read the game itself; what a
message does is up to its owner, within the owner's own permissions.

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

A refusal in Lua (`mq.web` without `web`) is an error, as for `mq.imgui`; `hs.Web*` in a plugin's window give nil
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

### Opt-in add-on policies

A server can require, allow or block add-ons by name of their GitHub repository: any add-on the add-on manager
installs (scripts, compiled plugins, tools such as Texture Studio, upscalers such as XeSS or FSR 4). It is off unless
`[addons] enforce = on`; without it nothing here applies and the rest of this file is unchanged, optional suggestions
included. `server/hailstone-addon-policy.cfg.example` is an example of its own; copy it to
`client_files/hailstone.cfg` and edit it for the server.

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

- **Identities are GitHub repositories**, `owner/repo` (`projecthailstone/hailstone-dpsmeter`), compared without
  case; a GitHub link to one works too, and a release tag or branch is ignored. They are matched against the source
  the add-on manager recorded when it installed the package, not a folder or manifest name. A bare name
  (`dpsmeter`) is refused: any package of that name, from anyone, would satisfy it. A refused or malformed entry
  makes the policy invalid (below), and the client's log names it (`hailstone.cfg: [addons] required: "dpsmeter" is a
  bare name; add-ons are named by their GitHub repository (owner/repo)`). They are not version pins: the manager's
  usual update rules still apply.
- **A minimum version** (`required` only): `required = owner/repo:1.0.0-preview.4` is met by that installed version or
  any newer one, in semantic version order (`preview.10` is newer than `preview.4`, and `1.0.0` newer than its
  previews; build metadata after `+` is ignored, prerelease letters keep their case). It is the manifest's `version`
  the manager recorded at install; an installed add-on without a valid one does not meet it. A repository named more
  than once keeps the highest minimum, and without the suffix any version does. A malformed version
  (`owner/repo:1.2`), or a suffix on `allow` or `block`, makes the policy invalid. The login screen's review and
  install links name the plain repository. **Older clients refuse such a server:** a Hailstone before this
  release reads `owner/repo:version` as a malformed entry, so its invalid policy keeps every player on it out of the
  world. Use the versioned form only once your players run a client that has it.
- **`allow`, `block` and `default`.** `default = deny` refuses every add-on not named by `allow`, `required` or
  `suggest` (a repository the server suggests is one it permits, so it need not be named in `allow` too),
  so a closed list can shut out every add-on, visual ones included; `default = allow` (the default) admits the rest.
  `block` wins over everything; an identity both required and blocked makes the policy invalid. An add-on put in by
  hand (not by the manager) has no repository: no list names it, so it runs under `default = allow` (unless it uses
  a blocked add-on's name) and is refused under `default = deny`. Neither can renaming a script, plugin or edited
  file admit unknown bytes: an installed add-on is known by its files' recorded hashes.
- **Interfaces** (`kind ui` add-ons) are add-ons too: a blocked one, or an unlisted one under `default = deny`, is
  not offered on that server, nor is an interface built on it, and the client falls back to the built-in interface
  (its base, else Modern) without changing the player's choice. A pack put in `ui/` by hand is refused under
  `default = deny` as any hand-placed add-on is; the built-in `ui/modern` and `ui/gamepad` never are.
- **`required`** add-ons must be installed with every file intact. Required scripts and compiled plugins start at the
  zone-in even when they are not in the player's autorun, and the player cannot stop, pause or unload them on that
  server. A required compiled plugin still asks for the player's approval of its bytes on this server ("The server's
  compiled plugins ask first"). A required upscaler must be the one picked under Options, Graphics, Upscaler and
  loaded (it loads only at start). A required tool is a program of its own: its intact installation meets the
  requirement; the client neither starts it nor can tell whether it runs.
- **Permission is not capability.** Naming an add-on in `allow` or `required` does not turn on `[scripts] player` or
  `[plugins] player`, nor grant more than `player_caps` and the add-on's manifest. A required script or plugin that
  needs capabilities the server does not give is a requirement not met. A `suggest` stays optional: it allows the
  add-on as `allow` does, but never requires or starts it and grants no capability.
  This holds for `required` too, on purpose: `addons/installed.txt` is the player's file, and nothing kept on disk
  lets the client check at load time that a record's files are the ones a signed manifest named (the manifest is not
  kept, and its signature covers package archives, not the files unpacked from them). Letting a named repository lift
  capabilities would let an edited record give the player's own code everything `player = off` refuses.
- **The server's own files** (its `scripts/` and `plugins/`, listed by hash at the zone-in) are not add-ons: no
  list names them and the policy never refuses them. The server decides by sending them or not.

When it is decided:

- **At the zone-in, by its rules.** The `hailstone.cfg` the zone-in brings decides, never the copy cached from the
  last visit: so a server that fixed a mistake in its policy is never locked out by the old one. The cached copy only
  informs the login screen (its add-on panel shows "Required to play on this server" and offers each missing one).
- **Before the world is shown**, the loading screen reads "Checking required add-ons": required add-ons start, and a
  required plugin's question waits for the player's answer. A requirement that is missing, changed, refused, needs
  more than the server allows, or fails to start returns to the login screen with the add-on panel open on what is
  missing. Reviewing it fetches its manifest and opens the usual install card; nothing is fetched before that click,
  and installing keeps every usual check (signatures, pinned keys, platform, capabilities, approval of native code).
  An invalid policy refuses entry the same way, with the reason. Every reason is written to the client's log as
  well (`Add-ons: entry refused by this server's add-on policy: ...`), one line for each requirement not met.
- **Once the character is in the world**, nothing disconnects: a required add-on that fails (a required plugin
  whose code lets an exception out counts as failed, though it stays loaded), or the next zone's rules that the
  installed add-ons do not meet, is a warning in chat (`[Add-ons] Required add-on owner/repo did not remain
  running. You can keep playing on this server.`), said once for each failure. The add-on is named by its
  repository, as the server's list names it, not by its folder or plugin name. Leaving to character select starts the
  check over for the next entry.
- **Server add-ons warning and Retry.** While such a failure lasts, a small Server add-ons window (Modern, Gamepad,
  and the client's own when the interface has none) says what failed and that the player can keep playing. When the
  running add-ons reported it (a required plugin that let an exception out or did not load, a required script that
  failed or ended), it offers **Retry**: each such plugin is unloaded and loaded again once, and each such script
  started again, under every usual check (rules, approval of native code). Required add-ons still cannot be
  stopped. The window shows nothing while all is well, takes no action by itself, and its close box puts it away
  until the next failure. Only the client's own interfaces show it or press Retry (`hs.AddonPolicy`,
  `hs.AddonPolicyAction`, docs/ui-api.md); an add-on interface gets the client's window instead.

**When blocking upscalers.** An upscaler loads at start, before any server is known, and Vulkan cannot change it
while the client runs. An active upscaler the policy refuses (or whose loaded files no longer match its installed
add-on, as after an update while it ran) refuses entry, with instructions to pick an allowed or built-in one and
restart. With `default = deny`, name in `allow` the upscalers to permit; the built-in choices need no entry. Requiring
one excludes players whose hardware cannot run it.

The policy is about playing on this server, not the player's whole library: other servers keep their own rules, and
nothing installed is removed. The add-on panel's Start button follows it for tools; a program started outside
Hailstone is outside it. A texture pack is not an add-on: Texture Studio builds it from the player's own game files
into `textures/`, it has no repository, and no list names it (block the tool to keep it from being started from the
client). This is the client's behaviour, not proof for the server: an older or changed client, or native code the
player already approved, can ignore it.

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

## Server add-on services

These host functions and hooks are added later in ABI 3, at the end of their tables, after every entry before
them (the bulk reads through `get_raid_data_bulk` and the ability calls). Check `HAILSTONE_HOST_HAS` before calling
a host function, the hook table's `size` (the client does), and the struct's returned size before reading an
appended field. A plugin built against an earlier header keeps its positions. Against a stock server none of these
services does anything until a plugin uses it.

- Record sizes, pinned in the client (a field is only ever added at a record's end): `HailstoneFollowProfile` 56
  bytes, `HailstoneMountProfile` 8, `HailstoneFollowStatus` 24, `HailstoneBinaryMeasurement` 56,
  `HailstoneMerchantSellContext` 32, `HailstoneMapPolicy` 88, `HailstoneWorldFrame` 136, `HailstoneWorldAnchor` 32,
  `HailstoneWorldVertex` 28, `HailstoneWorldSurfacePatch` 64, `HailstoneWorldSurfacePatchResult` 576,
  `HailstoneWorldReceipt` 48 and, in a 64-bit build, `HailstoneWorldSubmission` 56. Each begins with its `size`
  (the vertex excepted), which the caller sets and the client respects.
- `follow_profile` and `mount_profile` (`extend`) return bounded data for host-owned movement. A profile never
  starts a follow. `manual_follow(host, "start" | "stop" | "toggle")` requires `move` and `extend`, validates the
  current selected living player, and pins both actor incarnations. `get_follow_status` requires `read` and reports
  the live target identity. Turning and pitch smoothing are time-scaled. Movement cancellation is optional;
  death, zoning, target replacement and profile retirement stop enhanced follow. A mount's instant acceleration
  changes its ramp only, preserving the computed speed limits. `sdk/hailstone_follow.h` lists all field bounds.
- `map_visible(spawn, purpose)` (`ui` and `extend`) returns -1 to abstain, 0 to deny or 1 to allow. Purpose
  `HAILSTONE_MAP_MARKER` gates extension map/minimap markers; `HAILSTONE_MAP_FIND` gates Find results and their
  target/path/walk actions, including the modern map and minimap's service NPC overlays. A denial takes precedence
  over another extension's marker. An active guide rechecks eligibility before observing its destination's current
  position and retires its trail on denial. With no policy, stock behavior remains. This controls supported UI presentation,
  not the availability of spawn data to scripts or native code. The player and ordinary group map remain available.
- `map_policy(out)` (`ui` and `extend`) optionally returns `HailstoneMapPolicy`: `size`, `flags`, an optional
  NUL-terminated plain display `name[64]`, and optional content `revision`. Advance that revision when eligibility
  or provider data changes without changing the flags/name. Return zero to abstain. `CUSTOM_SPAWNS` makes the first active claiming
  provider the exclusive extension-marker source, even when it returns no mark; every provider's `map_visible`
  denial still wins. `HIDE_STOCK_NPCS` removes the packs' Find/service NPC overlays and competing stock controls;
  `DISABLE_NPC_FIND` blocks discovery requests, result rows, targets, guides, server paths and Find-owned walks.
  Restrictive flags combine across active providers. Removing or deactivating them restores stock defaults without
  changing saved UI preferences. These flags do not affect PC group/raid Find, coordinates, ordinary navigation,
  static map labels, user pins or raw spawn getters. Guides and Find walks pin connection/player/NPC identity and
  recheck eligibility before observing destinations or moving. Invalid hook sizes, unknown flags, unterminated names
  and control characters abstain. `get_map_policy(host,out)` requires `read`, the main thread and an initialized
  output size; it copies only the caller's prefix. Querying it recursively from the same plugin's policy hook fails
  without writing output. Both declarations are optional ABI3 tails: use `HAILSTONE_HOST_HAS` and hook-table sizes.
  The original 72-byte prefix remains accepted, including four-byte-aligned old reader records. The 80-byte
  prefix adds `revision`; the 88-byte record adds `context`. Missing optional fields default to zero. A reader
  must zero-initialize its record before setting `size`. The host preserves that requested size; success
  or a returned size of 88 alone does not establish tail support on an older host. A nonzero `context` identifies
  support for context-bound observations.
  A provider supplies its own content revision and cannot choose `context`; a reader receives the host's effective
  revision and connection/zone/player context. The effective revision includes every active provider's identity and
  lifetime, including independent visibility/Tracking restrictors, activation and permission transitions.
  A same-name replacement or an off/on interval invalidates old policy-derived interactions even if no frame
  observed the intermediate state. `hs.UI.map.Policy()` exposes these tokens as opaque decimal strings; `name`
  remains presentation only. Tracking uses a separate internal provider-lifecycle stamp: provider replacement,
  activation and permission transitions retire its source-owned requests, while map-only flags, names and content
  revisions do not. Saved preferences and raw spawn access are unaffected.
  Lua providers may use `hailstone.hooks{map_policy=fn,map_visible=fn}` or the registration shorthands
  `hailstone.map_policy(fn)` and `hailstone.map_visible(fn)` (nil removes that hook). Both execute only with
  live `ui` and `extend`. Policy callbacks return a plain table with optional booleans `custom_spawns=false`,
  `stock_npcs=true`, `npc_find=true`, optional printable `name` of at most 63 bytes, and a nonnegative integer
  `revision`. Nil or malformed answers abstain. Visibility callbacks receive the current spawn and purpose
  `"map"` or `"find"`, and return false to deny, true to allow, or nil to abstain; denials combine across providers.
  Callbacks keep the script's ordinary protected lifetime and execution budget. Neither helper grants authority
  over Tracking's own skill policy, nameplates or other discovery surfaces.
- `merchant_right_click(context)` (`packets` and `extend`) may consume a carried item's right click while an NPC
  merchant is open. The host checks range and the carried slot, and flags an occupied cursor, stock sell refusal or Keep protection,
  pending operations and modifiers. Containers are excluded. The copied context includes the merchant, item,
  full stack quantity, top-level inventory slot and lossless `HailstoneItemSlot`; it grants no inventory ownership.
  An add-on supplies its private packet and consumes the click when its option is enabled; any context flag requires consuming without sending. Returning zero keeps
  the ordinary item path. The client ships no private sale opcode or server-specific slot translation.
- The raid roster is read with `get_raid_data_bulk` ("Buff and spawn records"). `select_raid_member(spawn_id,
  incarnation)` (`read` and `target`) targets a roster member in the zone, or a member's pet: the spawn id is a
  record's `spawnID`, the incarnation `get_spawn`'s for it, so a spawn id the zone has given to someone else since
  is refused. `raid_action(name, HAILSTONE_RAID_MOVE, group)` (`read` and `actions`) moves a member to a group,
  numbered as the bulk read numbers them (0..11, -1 out of every group): only the leader of a locked raid, only a
  roster member, only to a group with room. Both are requests: the server decides, and the roster changes when it
  says so.
- `HailstoneSpawn.incarnation` identifies a full actor record. Resource fields count only with the corresponding
  `HAILSTONE_SPAWN_*_KNOWN` bit, so an absent percentage is not zero or full. Resource observations are retired on
  actor replacement/removal. `display_name` is the ordinary cleaned name; social flags and guild text are received
  observations. `HailstonePlayer.zone_instance` distinguishes the current instance. `HAILSTONE_PLAYER_PROMPT_SAFE`
  is a local quiet standing/resting observation without motion, combat, casting, stun or known detrimental buffs;
  it is not permission for a game action.
- `decode_item_packet` (`read`) decodes a bounded, at most 256 KiB, serialized received item locally.
  A failed decode leaves both its slot and item outputs unchanged, including an invalid item output size.
  `inspect_item_packet` (`read` and `ui`) shows that copied item without inventory mutation or a link request.
  `HailstoneItem.serial`, `instance_id`, `augment_ids` and `configuration_complete` distinguish received instances
  and installed configurations. Bag contents are never reported as augment IDs.
- `item_flags`, `item_description_append`, `item_presentation_revision` and `loot_confirmation` let an active
  extension supply plain text metadata and an additional loot question. They do not suppress stock no-drop
  confirmation. `zone_display_name(zone_id, instance_id)` changes a display label only (the HUD's and the map's zone name), never protocol identity: the `Zone` TLO and `Zone.Name` stay the zone's own long name.
- `get_memorized_spell` (`read`) returns a received usable gem's spell ID, zero for empty or unavailable.
  `monotonic_ms` (`read`) uses the same epoch as a world frame's `now_ms`, independent of wall-clock time.
- `measure_binary(kind, out)` (`read`, main thread) measures either the platform-resolved running executable file
  (`HAILSTONE_BINARY_EXECUTABLE`) or the calling plugin's actual admitted private library copy
  (`HAILSTONE_BINARY_CALLING_PLUGIN`). It returns byte count, whole-file SHA-256 and FNV-1a64 with bit 63 cleared.
  It accepts no arbitrary file path and exposes no hardware identifiers. Updating the installed plugin file does
  not change the loaded copy's identity; a private-copy SHA-256 mismatch, observed size/mtime change, read error
  or unknown kind fails without changing the output. Reads stream in bounded storage, up to 4 GiB. This describes
  measured file bytes and does not attest process memory. A server integration owns its challenge, report format,
  allowlist and admission decisions; the generic host supplies no legacy-client or server-specific identity claim.

### Transient world geometry

`sdk/hailstone_world_geometry.h` describes `world_frame`, `world_frame_result`, `world_capabilities`,
`world_floor_below`, `world_anchor` and `world_submit`. They require `ui` and `extend`. The application supplies
acquired scene frames, terrain queries and drawn actor anchors; an add-on converts its private protocol to these
generic coordinates. Query results distinguish unavailable/deferred from a completed query with no surface.
Frame and epoch tokens expire, query/vertex/batch budgets are bounded, and a complete submission is copied before
admission. Owners come from the host and cannot be supplied by an add-on. Every batch must be resubmitted each
frame. Unloading, deactivation and permission loss retire owned geometry.

`world_floor_below` returns the highest upward drawn opaque or masked surface at or below `seed_z`,
which is the caller's bounded ceiling for choosing a floor layer. It includes terrain, static and passable
props, current animated scenery, doors and placed objects. Invisible collision, undersides, transparent
surfaces, creatures, dropped items, ghosts and missiles are excluded. Masked triangles use their geometric
footprint; this CPU query does not sample texture alpha. These visual effect surfaces do not change movement
collision.

Hosts advertising `HAILSTONE_WORLD_SURFACE_PATCH` also provide `world_surface_patch`. Supply three XYZ floor
vertices defining the proposed patch, a vertical tolerance, and a nonnegative `maximum_rise`. Its local ceiling
is the interpolated candidate plane plus that rise. `HAILSTONE_WORLD_PATCH_PLANAR` certifies the entire triangle's
drawn-surface coverage within tolerance and absence of higher surfaces below that ceiling, including current
props and animation. This is a geometric coverage proof, rather than agreement among point samples. A
`MISMATCH` or `MISSING` result supplies an interior XY refinement witness; Z is meaningful only with
`HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN`. `HAILSTONE_WORLD_PATCH_SOURCE_FACE` additionally supplies the actual selected
drawn source triangle at that witness, allowing exact boundary splits. Its vertex order follows the native upward
`CollisionTriangle` convention. Derived patches must each be certified again. `MISSING` denotes a hole in the candidate plane's coverage and can have a
lower surface below it. Incomplete geometry, invalid/stale tokens or exhausted work return `DEFERRED` without
changing the output. Candidate inspection and clipping spend the same finite frame budget as point queries.
`HAILSTONE_WORLD_PATCH_REGION` optionally provides up to 32 double-precision XY vertices of the selected convex
refinement region: a higher-face overlap or uncovered piece. It is a boundary hint inside the input triangle,
can overlap other surfaces, and does not certify a resulting mesh. Intersect it with the supplied source face
when refining, and certify every resulting triangle before submitting it.
A float witness may round onto or just outside the producer boundary only when both a valid source face and
positive-area convex region are present. The region's actual double-precision centroid must lie strictly inside
the producer and source face, and the float witness XY must exactly equal `float(centroid)`. The height and source
face then refer to that double centroid. A point-only witness still requires strict interior coordinates. This
exception supplies refinement information and never turns an incomplete patch into a planar certificate.

Admission is not rendering. A recorded receipt means that the complete batch was recorded; a presented receipt
means its frame was accepted for presentation. Neither proves visible monitor pixels, visibility or attention,
and no receipt authorizes gameplay actions. A frame-unavailable notification clears earlier availability.
