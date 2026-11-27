# Startup and zone command files (.cfg)

Hailstone runs MacroQuest's `.cfg` files: plain text files of commands, one a line, that run at set moments (entering
the world, each zone-in, a plugin loading) as if you typed them. A MacroQuest `Config` folder's files work unchanged
when copied into `cfg/` in Hailstone's data folder. Code: `src/app/autoexec.*`, `src/app/automation.cpp`.

## Where the files go

Three folders, from the broadest to the most specific (docs/layout.md):

| Folder | For |
|---|---|
| `<data>/cfg/` | every server and character |
| `<data>/servers/<profile>/cfg/` | one server |
| `<data>/servers/<profile>/characters/<world>/<name>/cfg/` | one character |

For each file name, only the most specific copy runs: a character's `zoned.cfg` is used instead of the server's or
the global one, not as well as them. Within a folder, an `autoexec/` subfolder is looked in first, as MacroQuest looks
in `Config\Autoexec\` before `Config\`. File and folder names match in any case on every system, but only the ASCII
letters A to Z fold: a name with other letters (`Bärbel.cfg`) must be written in the file's own case.

Nothing is read from the game folder, and a server cannot put a file in any of these folders: the files a server
sends go to the profile's `assets/`, `scripts/` and `plugins/` folders and `hailstone.cfg` only.

## When each file runs

The names are MacroQuest's:

| File | Runs |
|---|---|
| `AutoExec.cfg` | once each time Hailstone starts, at the first character select or world entry |
| `CharSelect.cfg` | each time character select shows |
| `InGame.cfg` | entering the world from character select (not when zoning) |
| `<server>_<character>.cfg` | after `InGame.cfg`; `<server>` is the world's name as `${EverQuest.Server}` gives it (`My Server_Tester.cfg`) |
| `<Class>.cfg` | after that, by the class's stock name (`Cleric.cfg`, `Shadow Knight.cfg`), whatever a server calls the class |
| `<zone>.cfg` | after every zone-in, the first included, by the zone's short name (`poknowledge.cfg`) |
| `zoned.cfg` | after every zone-in, one frame later than `<zone>.cfg` (MacroQuest runs it delayed, so the zone's own lines run first) |
| `<plugin>-AutoExec.cfg` | when that compiled plugin loads |
| `<script>-AutoExec.cfg` | when that Lua script starts (a Hailstone addition: MacroQuest has none for Lua) |

At a world entry the order is `InGame`, `<server>_<character>`, `<Class>`, `<zone>`, then `zoned` a frame later.

## The lines

- One command a line: game commands (`/sit`, `/join`), MacroQuest's (`/echo`, `/bind`), `/lua run`, `/plugin load`.
- A line that starts with `;` is a comment, as in MacroQuest. Hailstone also takes `#`, ignores blank lines and leading
  spaces, reads the file as UTF-8 with either kind of line end, and has no line length limit (MacroQuest cuts a line at
  2047 bytes).
- Each line runs as one you typed: with your own rights, not a script's, and `${...}` is read when the line runs, so a
  line sees what the lines before it did. Its replies are not lines from code (the Plugins and Scripts chat filter).
- The lines run in the frame after the moment they belong to, outside whatever raised it.

## /loadcfg

`/loadcfg <name>` runs a file now. A name is looked up as above (`.cfg` added when it has no `.`). Without a file it
says `Could not /loadcfg '<name>'`.

A name with a folder in it is a path, relative to the data folder (`/loadcfg cfg/raids/fights`) or absolute. A path
is taken only from a line you typed (or one of your own `.cfg` files ran), and only when it names a `.cfg` file inside
one of the three folders above, subfolders included; anything else in the data folder (a plugin's settings, the chat
history, a server's `assets/`) is not run. A script or plugin may name a file but not give a path: its path is refused
with a chat line, as it cannot read files itself and must not send another file's lines as chat. MacroQuest takes any
path from anyone.

A file larger than 64 KB is not run, with a chat line saying so.

A file's lines run as whoever sent `/loadcfg`: a script that sends it runs the lines with the script's own rights. A
file may `/loadcfg` another, but not one already running further out (a file that loads itself, or two that load each
other, stop there with a chat line). `/loadcfg`, `/docommand`, `/noparse` and `/squelch` nest at most 16 deep. One
`/loadcfg` runs at most 1000 lines in all, the lines of the files it loads in turn included, and so do the files that
run together at one moment (a world entry's `InGame`, `<server>_<character>`, `<Class>` and `<zone>`); past that the
rest is dropped with one chat line.
