# Hailstone server support

Hailstone works on any EQEmu server as is. A server can add five things:

- **sending the server's client files**: what the owner puts in `client_files/` (spells, strings, zones, models, and
  the server's own plugins and Lua scripts with the `hailstone.cfg` that sets the rules for plugins, scripts and
  assets) is downloaded by Hailstone at the first zone-in of a login, with no web server (below);
- **guild invitations across zones**: EQEmu's `/guildinvite` only reaches a player in the inviter's zone (anyone
  else gets "Prospective guild member ... must be in zone"). With the script it reaches the player in any zone;
- **declined group invitations reaching the inviter**: a RoF2 client that declines sends OP_CancelInvite (0x2a50),
  which stock EQEmu maps to nothing, so the inviter is never told. The script passes it on (`group_declines`): the
  packet itself to an inviter in the same zone (the client prints "%1 rejects your offer to join the group." and the
  other refusals), else the same line as text. It works for any RoF2 client, Hailstone or not. EQEmu's Lua cannot
  tell whether the named player really invited the sender, so at most 5 declines a sender in 10 s are passed on;
- **quest givers in the Find window**: Hailstone's Find window lists the zone's merchants, bankers and trainers on
  any server; with the script it also lists the NPCs that have quests (below);
- **zone connections for the map's zone finder**: every zone point the server's expansion and content flags allow,
  so the World view can route between zones the player has not visited (below).

This folder does all five with a Lua quest script. It needs no C++ changes, no rebuild and no opcode
edits, and it runs next to the server's own quests (Perl or Lua) without replacing any of them. Tested on
EQEmu 23.10.3 with the PEQ quests.

## Install

```sh
python3 server/install.py <server dir>      # the folder with eqemu_config.json
```

Then restart the zones (or `#reloadquest`). The script copies `quests/` to `<server dir>/hailstone/quests` and adds
it to `quest_paths` in `eqemu_config.json`, after the server's own quests. `install.py <server dir> --remove` undoes
it. The edited `eqemu_config.json` keeps its permissions (it holds the database password) and, when the script runs
as root, its owner.

**By hand**, or on servers too old for `quest_paths`: copy `quests/global/encounters/hailstone.lua` into the
server's `quests/global/encounters/`, and add this line to its `quests/global/script_init.lua` (create the file if
there is none):

```lua
eq.load_encounter("hailstone")
```

Nothing else is needed.

## Plugins, scripts and the rules: `hailstone.cfg`

```
client_files/
  hailstone.cfg          the rules (example: hailstone.cfg.example in this folder)
  spells_us.txt ...      files the server makes (export_client_files), and zones, models ...
  assets/                more of those, if you prefer them in a folder
  scripts/               the server's Lua scripts (MacroQuest's mq module)
  plugins/<platform>/    the server's compiled plugins: windows-x64, linux-x64, macos-arm64 ...
```

- **With a `hailstone.cfg`** the server's own scripts and plugins always run on its players' clients, with every
  capability; plugins load by themselves and players cannot unload them, and `[scripts] run` starts scripts at each
  zone-in. The player's own scripts, plugins and asset files run only as far as the file allows.
- **Without one** (plain EQEmu, or this script without the file) the client sets no rules: players run what they like.

Set these keys in `client_files/hailstone.cfg` to disable Ability Book autoskill and Lua socials.

```ini
[abilities]
autoskill = off

[socials]
mode = off
faces = off
```

`autoskill = off` stops automatic ability use from the Ability Book. Players can still use abilities manually.
`mode = off` blocks Lua social presses. Ordinary command socials still work, and the Lua Social editor can still
preview commands in a dry run without sending them. `faces = off` also disables Lua button faces, which can
otherwise read game state even with `mode = off`.

See the full policy settings in the [plugin and scripting guide](../docs/plugins.md) and the sample in
[`hailstone.cfg.example`](hailstone.cfg.example).

Run `hailstone_check client_files/` to see the policy, warnings about unknown settings, and any files a client
will refuse. The checker is built with the client. Players get changes at their next login.

## Settings

At the top of `hailstone.lua`. Any of them can also be overridden live with a data bucket `hailstone:<name>`
(`#databuckets`). Each zone reads a bucket at most every 10 seconds (EQEmu caches no bucket without a character,
so each read is a database query), and at once after `#reloadquest`.

| Setting | Default | |
|---|---|---|
| `enabled` | 1 | answer at all (0: no request is answered, `client_files/` included, and the zone does not list or hash it at start) |
| `guild_invites_world` | 1 | `/guildinvite` reaches Hailstone players in other zones |
| `client_files` | 1 | send `client_files/` to Hailstone clients (0: they get an empty list) |
| `group_declines` | 1 | tell the inviter when a RoF2 client declines a group invitation |
| `files_rescan` | 300 | seconds the `client_files/` list is kept before a player's request looks at the folder again (never under 30) |
| `files_chunks_per_second` | 768 | file pieces (32 KB each) one player is sent in a second; more wait their turn (0: no limit) |
| `files_chunks_per_second_zone` | 1024 | file pieces all of the zone's players together are sent in a second; more wait their turn (0: no limit) |
| `quest_npcs` | 1 | tell Hailstone's Find window which of the zone's NPCs have quests (0: say nothing) |
| `zone_links` | 1 | send how the zones connect, for the map's zone finder (0: never answered; players' clients then learn the zone lines of the zones they visit) |

## Client files

Put the files players need in `client_files/` in the server folder (beside `eqemu_config.json`), laid out as in a
game install folder: `spells_us.txt`, `Resources/SkillCaps.txt`, `commons.s3d` ... Run EQEmu's `export_client_files`
for `spells_us.txt`, `dbstr_us.txt`, `SkillCaps.txt` and `BaseData.txt` (the last two go in `Resources/`), so they
match the database. Names starting with `.` are not sent, and on a Windows server neither are names with
characters outside ASCII (EQEmu's Lua cannot open them there; the zone's log names each one). `client_files` may be a
link to a folder elsewhere. When a zone lists no files at all (the folder is missing, empty or cannot be read), its
log says so once, until files are listed again. Programs and scripts (`.exe`, `.dll`, `.lua` ...) are
refused by the client outside `scripts/` (Lua) and `plugins/` (each platform's libraries).

A zone hashes the folder (SHA-256) when it starts and keeps the hashes in `client_files/.hailstone-cache` by size
and time, so only new or changed files are hashed again; when a client asks and the list is older than
`files_rescan` (5 minutes), the zone looks again. Looking runs `find` and `sha256sum` and holds the zone while they
run, so a player's request can make it happen at most that often, and never more than every 30 s whatever the
setting says. Adding or changing files needs no restart: players who zone in get them within five minutes, or at
once after `#reloadquest`, which hashes the folder again. The first start after adding a large folder takes a few
seconds per gigabyte in that zone.

A file changed in place within those five minutes is not sent with its old hash: the first time any player asks
for a file's first piece, the zone looks at its size and modification time (one `find` for that file; on Windows the
size only, as a PowerShell start per file would be too slow), and at every piece at the size of the file it has
open. That first look is made once per file until the zone next makes the list (`files_rescan`), for all players
together, so a rewrite that keeps the size after it goes unnoticed until then. A change makes the zone hash the
folder again (at most every 30 s) and tells the client, which lists the files again; until the folder is hashed
again, every request for that file is told the same. A file changed on
Windows without changing its size can still go with its old hash until the next listing; the client then finds the
hash wrong and keeps its old files until the next login.

The hashing uses `find`, `xargs` and `sha256sum` (GNU, as on any Linux server or the Docker images) and on Windows
PowerShell's `Get-FileHash`; the Windows commands are not tested yet. EQEmu's Lua has no `os.remove` or `os.rename`,
so the script removes and renames its scratch files through those commands too.

Hailstone asks at the first zone-in of each login and holds the zone-in, with "Downloading server files" on the
loading screen, until what changed has come; then it reads the files again and the zone loads. Players are sent
the files from the zone they enter, a 32 KB piece at a time as the client asks (`OP_HsFilesRequest` 0x1434,
`OP_HsFiles` 0x1435; the layout is in the client's `src/net/server_files_packets.h`). The zone keeps up to eight
files open for this, so players fetching different files do not make it reopen them at every piece. A player may
ask for `files_chunks_per_second` pieces a second (768, 24 MB/s; one download was measured at about 16 MB/s, so
this leaves it room), and all of a zone's players together for `files_chunks_per_second_zone` (1024, 32 MB/s: two
first downloads at once each get about the measured rate, three or more share it and are slower). A client asking
faster than that has its requests wait, up to 64 of them, and while any wait, every player's are served a player at a
time in turn, so no one is left with nothing; they are served as requests come (while the zone's limit has room that second) and on a 100 ms timer (`eq.set_timer`
on the script's encounter) as the limits allow, and what waits for a player who leaves the zone is dropped. The
client asks again for a piece that does not come. EQEmu keeps every piece it sent until the client acknowledges it,
for up to 30 seconds, and a quest script cannot see those acknowledgements: a modified client that asks and never
acknowledges makes the zone hold what the limits let through in 30 seconds, at most about 1 GB at the defaults
however many characters it uses (about 0.7 GB for one). This is bounded, not gone: on a small host, lower
`files_chunks_per_second_zone` to hold less, at the cost of slower first downloads.

Every Hailstone request is limited per character before the zone does any work for it: eight whole file lists a
minute, ten guild packets and five settings requests in 10 seconds (a malformed one counts too, so a flood of them is not logged line by line), five group declines in 10 seconds, and file piece requests up to twice what the two limits above let a player be sent in a second plus the 64 that may wait (at least 2112 a second; an honest client asks once a piece, and again for a lost one). An honest
client asks for each once a zone-in; what is over the limit is dropped, with one line in the zone's log. A first download of a
gigabyte took about 70 seconds on one machine; a player who has the files only gets the list. Compressing the
files would not help: EQEmu already compresses the zone stream (`Network:CompressZoneStream`), and zone and model
archives are compressed inside.

## Quest givers in the Find window

Hailstone's Find window asks once a zone, the first time a player opens it there (`OP_HsQuestNpcsRequest` 0x1436),
and the script answers with the names of the zone's NPCs that have quests (`OP_HsQuestNpcs` 0x1437; the layout is
in the client's `src/net/quest_npc_packets.h`). An NPC has quests when its own quest file, found as EQEmu finds it
(`quests/<zone>/v<version>/`, then `quests/<zone>/`, then `quests/global/`; by NPC id, name, or name_id; `.lua` or
`.pl`; each folder of `quest_paths` in turn), handles `EVENT_ITEM` / `event_trade` (it takes hand-ins) or
`EVENT_SAY` / `event_say` (it talks). The `default` scripts and `global_npc` are left out, since they run for every
NPC; so is a handler a Perl plugin or an encounter adds at run time. The NPCs are those in the zone's spawn tables
for its version (one query) and those alive when the list is made (spawned by scripts).

The list is made once per zone process, at the first request, and kept: later requests, from any player, get the
kept list (a few KB). The zone's log has a line with how many NPCs it looked at and how long it took. `#reloadquest`
makes it again with the quest files as they are then. Each character may ask three times in 10 seconds; more are
dropped. Set `quest_npcs` to 0 to keep which NPCs are scripted to yourself.

## How it works

The client sends its requests on custom opcodes no server's opcode table lists (the settings request
0x1432, the files' `OP_HsFilesRequest` 0x1434, the guild invitation 0x1430, the quest NPCs 0x1436, the zone links 0x1438), so the zone passes them to the
`event_unhandled_opcode` quest event, and the script replies with raw packets. Hailstone 0.7.0 and older send the
same requests 0x0e lower (0x1422, 0x1424, 0x1426); the script answers those on the old numbers too, so updating the
script first leaves older clients working. (On Project Triune's server 0x1422 is its own opcode and never reaches
the script, so those older clients get no cross-zone guild invitations there.) The script registers that event
through an encounter loaded from `global/script_init.lua`, which EQEmu runs from every quest folder, so the server's
own `global_player` scripts keep running.

## Zone links

The map window's zone finder shows the way from one zone to another (docs/navigation.md "Zone atlas"). With the
script, the client asks once a session, the first time a player opens the map's World view (`OP_HsZoneLinksRequest`
0x1438), and gets every pair of zones a zone point joins (`OP_HsZoneLinks` 0x1439; the layout is in the client's
`src/net/zone_link_packets.h`). The list is the `zone_points` rows of version 0 or -1 whose own row, source zone and
target zone the server's expansion (`Expansion:CurrentExpansion`) and content flags allow, as EQEmu's
`ContentFilterCriteria` filters them when a zone loads its zone points, followed by the current expansion and the
zones (version 0) the expansion and content flags allow: the finder lists and routes through only those, so players
see only the zones of the server's expansion. It is built at the first request in each zone
process with one query of the content database and one of `content_flags`, then kept in memory; `#reloadquest`
builds it again. A character is answered at most twice a minute. A stock database gives a reply of about 7 KB.

## Guild invitations across zones

The inviter's client sends `/guildinvite <name>` (or the guild window's Invite) as EQEmu's own `OP_GuildInvite` when
the player is in its zone, and as a custom guild packet (wire opcode 0x1430) otherwise, once the zone has answered the
client's hello after the zone-in. The script then:

1. in the inviter's zone, makes EQEmu's checks with EQEmu's words: in a guild, the rank's invite permission
   (`guild_permissions`), the name is a character ("could not be found"), not in a guild, no invitation pending;
2. leaves the invitation in the data bucket `hailstone:guildinvite:<character id>` (20 s) and signals the
   character (`eq.cross_zone_signal_client_by_char_id`);
3. in the invitee's zone, sends the client the invitation (wire opcode 0x1431), which asks the usual
   question ("%1 is interested in having you join %2.", 10 s). A client that is not Hailstone could only answer
   EQEmu's same-zone invitation, so the inviter gets EQEmu's "must be in zone" instead. If no zone takes the
   invitation within 3 s the invitee is not online, and the inviter is told so;
4. on the answer (again the guild packet, in whatever zone the invitee is then), declines with EQEmu's words or puts
   the invitee in the guild with `eq.set_guild`, then sends the client the guild's rank permissions, rank names and
   roster as EQEmu does for a new member. EQEmu only forwards a new member's join to zones where world already
   sees a member of the guild, which leaves out an invitee alone in its zone, so without this the new member would
   not see guild chat until it zoned.

Other clients, and Hailstone on a server without the script (or with `guild_invites_world` 0), keep EQEmu's
same-zone invitation. Both packets are 80 bytes: `{u32 magic; u16 version 1; u16 action; char name[64];
u32 guild id; u32 value}`; the actions are in `hailstone.lua` and `src/net/social_packets.h`.
