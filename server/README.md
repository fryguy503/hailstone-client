# Hailstone server support

Hailstone works on any EQEmu server as is. A server can add five things:

- **sending the server's client files**: Hailstone downloads what the owner puts in `client_files/` at the first
  zone-in of a login, with no web server (below): spells, strings, zones, models, and the server's own plugins and
  Lua scripts with the `hailstone.cfg` that sets their rules;
- **guild invitations across zones**: EQEmu's `/guildinvite` only reaches a player in the inviter's zone (anyone
  else gets "Prospective guild member ... must be in zone"); with the script it reaches any zone;
- **declined group invitations reaching the inviter**: see [below](#declined-group-invitations);
- **quest givers in the Find window**: besides the merchants, bankers and trainers it lists on any server, the
  Find window lists the NPCs that have quests (below);
- **zone connections for the map's zone finder**: every zone point the server's expansion and content flags allow,
  so the World view can route between zones the player has not visited (below).

This folder does all five with a Lua quest script: no C++ changes, rebuild or opcode edits. It runs next to the
server's own quests (Perl or Lua) without replacing any. Tested on EQEmu 23.10.3 with the PEQ quests.

#### Declined group invitations

A RoF2 client that declines sends OP_CancelInvite (0x2a50), which stock EQEmu maps to nothing, so the inviter is
never told. The script passes it on (`group_declines`):

- to an inviter in the same zone, the packet itself (the client prints "%1 rejects your offer to join the group."
  and the other refusals);
- otherwise, the same line as text.

It works for any RoF2 client. EQEmu's Lua cannot tell whether the named player really invited the sender, so at
most 5 declines a sender in 10 s are passed on.

## Install

```sh
python3 server/install.py <server dir>      # the folder with eqemu_config.json
```

Then restart the zones (or `#reloadquest`).

- The script copies `quests/` to `<server dir>/hailstone/quests` and adds it to `quest_paths` in
  `eqemu_config.json`, after the server's own quests.
- `install.py <server dir> --remove` undoes it.
- The edited `eqemu_config.json` keeps its permissions (it holds the database password) and, when the script runs as
  root, its owner.

**By hand**, or on servers too old for `quest_paths`: copy `quests/global/encounters/hailstone.lua` into the
server's `quests/global/encounters/`, and add this line to its `quests/global/script_init.lua` (create it if needed):

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
  capability. Plugins load by themselves and players cannot unload them. `[scripts] run` starts scripts at each
  zone-in. The player's own scripts, plugins and asset files run only as far as the file allows.
- **Without one** (plain EQEmu, or this script without the file) the client sets no rules: players run what they like.

### Deny Hailstone access

Set the following in `client_files/hailstone.cfg` and keep the Lua script's client file service enabled.

```ini
[client]
deny = on
```

When Hailstone receives this setting during the first zone-in, it disconnects before completing entry and shows
“This server does not allow Hailstone. Please contact the server owner.” The default is `deny = off`.
This check relies on successful policy delivery and an up-to-date, unmodified Hailstone client.

### Automation controls

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

The settings are at the top of `hailstone.lua`. Any can be overridden live with a data bucket `hailstone:<name>`
(`#databuckets`). Each zone reads a bucket at most every 10 seconds (each read is a database query), and at once
after `#reloadquest`.

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
| `zone_links` | 1 | send how the zones connect, for the map's zone finder (0: never answered; see note) |

With `zone_links` 0, players' clients learn the zone lines of the zones they visit.

## Client files

Put the files players need in `client_files/` in the server folder (beside `eqemu_config.json`). Lay them out as in
a game install folder: `spells_us.txt`, `Resources/SkillCaps.txt`, `commons.s3d` ...

Run EQEmu's `export_client_files` for `spells_us.txt`, `dbstr_us.txt`, `SkillCaps.txt` and `BaseData.txt` (the last
two go in `Resources/`), so they match the database.

- Names starting with `.` are not sent.
- On a Windows server, names with characters outside ASCII are not sent (EQEmu's Lua cannot open them); the zone's
  log names each one.
- `client_files` may be a link to a folder elsewhere.
- When a zone lists no files at all (the folder is missing, empty or cannot be read), its log says so once, until
  files are listed again.
- The client refuses programs and scripts (`.exe`, `.dll`, `.lua` ...) outside `scripts/` (Lua) and `plugins/`
  (each platform's libraries).

#### Hashing and rescans

A zone hashes the folder (SHA-256) when it starts, keeping the hashes in `client_files/.hailstone-cache` by size and
time so only new or changed files are hashed again.

One zone hashes at a time:

- A zone with files to hash first makes the folder `client_files/.hailstone-lock` (its start time and name in
  `owner`).
- Another zone that finds it waits, a second at a time, then takes the hashes the first wrote to the cache and
  hashes only what is still missing, so a server starting all its zones at once hashes a new folder once.
- A zone that already has a list keeps it while another hashes, rather than wait in the middle of a player's request.
- A lock older than 15 minutes, or one with no owner after 10 seconds, is a stopped zone's: it is removed with a line
  in the log. After waiting five minutes a zone hashes anyway. Deleting `.hailstone-lock` by hand is safe.

When a client asks and the list is older than `files_rescan` (5 minutes), the zone looks again. Looking runs `find`
and `sha256sum` and holds the zone while they run, so it happens at most that often and never more than every 30 s.

Adding or changing files needs no restart: players who zone in get them within five minutes, or at once after
`#reloadquest`. The first start after adding a large folder takes a few seconds per gigabyte.

#### Files changed in place

A file changed in place within those five minutes is not sent with its old hash:

- The first time any player asks for a file's first piece, the zone looks at its size and modification time (one
  `find`; on Windows the size only, as a PowerShell start per file would be too slow).
- At every piece, it checks the size of the file it has open.
- That first look is made once per file, for all players, until the zone next makes the list (`files_rescan`), so a
  later rewrite that keeps the size goes unnoticed until then.

A change makes the zone hash the folder again (at most every 30 s) and tells the client, which lists the files again;
until then every request for that file is told the same.

A file changed on Windows without changing its size can still go with its old hash until the next listing. The client
then finds the hash wrong and keeps its old files until the next login.

#### Commands the hashing uses

The hashing uses `find`, `xargs` and `sha256sum` (GNU, as on any Linux server or the Docker images), and on Windows
PowerShell's `Get-FileHash`. The Windows commands are not tested yet. The script also removes and renames its
scratch files through them (EQEmu's Lua has no `os.remove` or `os.rename`).

#### Downloading

Hailstone asks at the first zone-in of each login and holds the zone-in ("Downloading server files" on the loading
screen) until what changed has come.

The zone they enter sends the files a 32 KB piece at a time as the client asks (`OP_HsFilesRequest` 0x1434,
`OP_HsFiles` 0x1435; layout in the client's `src/net/server_files_packets.h`). It keeps up to eight files open, so
players fetching different files do not make it reopen them at every piece.

Rate limits:

- One player may ask for `files_chunks_per_second` pieces a second (768, 24 MB/s), above the about 16 MB/s one
  download was measured at.
- All of a zone's players together may ask for `files_chunks_per_second_zone` (1024, 32 MB/s): two first downloads at
  once each get about the measured rate; three or more share it.
- A client asking faster than that has its requests wait, up to 64 of them.
- While any wait, players are served in turn, as requests come (while the zone's limit has room that second) and on
  a 100 ms timer (`eq.set_timer` on the script's encounter).
- What waits for a player who leaves the zone is dropped; the client asks again for a piece that does not come.

EQEmu keeps every piece it sent until the client acknowledges it, for up to 30 seconds, and a quest script cannot see
those acknowledgements. A modified client that never acknowledges makes the zone hold what the limits let through in
30 seconds: at the defaults at most about 1 GB however many characters it uses (about 0.7 GB for one). On a small
host, lower `files_chunks_per_second_zone` to hold less, at the cost of slower first downloads.

#### Request limits

Every Hailstone request is limited per character before the zone does any work for it:

| Request | Limit |
|---|---|
| whole file lists | eight a minute |
| guild packets | ten in 10 seconds (a malformed one counts too) |
| settings requests | five in 10 seconds (a malformed one counts too) |
| group declines | five in 10 seconds |
| file piece requests | twice what the two limits above let a player be sent in a second, plus the 64 that may wait (at least 2112 a second) |

Counting malformed requests keeps a flood of them out of the log. An honest client asks for each file piece once
(again for a lost one), for the file list once a login (again if it changes meanwhile, at most three times), and for
the guild hello and the settings once a zone-in; it sends invitations and declines only when the player acts. What is
over the limit is dropped, with one line in the zone's log.

A first download of a gigabyte took about 70 seconds in a test; a player who has the files only gets the list. Compressing
the files would not help: EQEmu already compresses the zone stream (`Network:CompressZoneStream`), and zone and model
archives are compressed inside.

## Quest givers in the Find window

The Find window asks once a zone, the first time a player opens it there (`OP_HsQuestNpcsRequest` 0x1436). The
script answers with the names of the zone's NPCs that have quests (`OP_HsQuestNpcs` 0x1437; layout in the client's
`src/net/quest_npc_packets.h`).

An NPC has quests when its own quest file handles `EVENT_ITEM` / `event_trade` (it takes hand-ins) or `EVENT_SAY` /
`event_say` (it talks). The file is found as EQEmu finds it:

- `quests/<zone>/v<version>/`, then `quests/<zone>/`, then `quests/global/`;
- by NPC id, name, or name_id;
- `.lua` or `.pl`;
- each folder of `quest_paths` in turn.

The `default` scripts and `global_npc` are left out, since they run for every NPC; so is a handler a Perl plugin or
an encounter adds at run time. The NPCs are those in the zone's spawn tables for its version (one query) and those
alive when the list is made (spawned by scripts).

The list is made once per zone process, at the first request, and kept (a few KB); the zone's log says how many NPCs
it looked at and how long it took. `#reloadquest` makes it again.

Each character may ask three times in 10 seconds; more are dropped. Set `quest_npcs` to 0 to keep which NPCs are
scripted to yourself.

## How it works

The client sends its requests on custom opcodes that no server's opcode table lists:

| Request | Opcode | Hailstone 0.7.0 and older |
|---|---|---|
| settings request | 0x1432 | 0x1424 |
| files (`OP_HsFilesRequest`) | 0x1434 | 0x1426 |
| guild invitation | 0x1430 | 0x1422 |
| quest NPCs | 0x1436 | |
| zone links | 0x1438 | |

So the zone passes them to the `event_unhandled_opcode` quest event, and the script replies with raw packets.

Hailstone 0.7.0 and older send the same requests 0x0e lower (last column); the script answers those too, so
updating the script first leaves older clients working. (On Project Triune's server 0x1422 is its own opcode, so
those older clients get no cross-zone guild invitations there.)

The script registers that event through an encounter loaded from `global/script_init.lua`, which EQEmu runs from
every quest folder, so the server's own `global_player` scripts keep running.

## Zone links

The map window's zone finder shows the way from one zone to another (docs/navigation.md "Zone atlas"). The client
asks once a session, the first time a player opens the map's World view (`OP_HsZoneLinksRequest` 0x1438), and gets
every pair of zones a zone point joins (`OP_HsZoneLinks` 0x1439; layout in the client's `src/net/zone_link_packets.h`).

The list is:

1. the `zone_points` rows of version 0 or -1 whose own row, source zone and target zone the server's expansion
   (`Expansion:CurrentExpansion`) and content flags allow, as EQEmu's `ContentFilterCriteria` filters them when a zone
   loads its zone points;
2. then the current expansion and the zones (version 0) the expansion and content flags allow.

The finder lists and routes through only those zones, so players see only the server's expansion.

It is built at the first request in each zone process (one query of the content database, one of `content_flags`)
and kept; `#reloadquest` builds it again. A character is answered at most twice a minute. A stock database gives
about 7 KB.

## Guild invitations across zones

The inviter's client sends `/guildinvite <name>` (or the guild window's Invite):

- as EQEmu's own `OP_GuildInvite` when the player is in its zone;
- otherwise as a custom guild packet (wire opcode 0x1430), once the zone has answered the client's hello after the
  zone-in.

The script then:

1. in the inviter's zone, makes EQEmu's checks with EQEmu's words: in a guild, the rank's invite permission
   (`guild_permissions`), the name is a character ("could not be found"), not in a guild, no invitation pending;
   then its own: an inviter invites at most five different players a minute (counted by the inviter's zone), since
   an invitation reaches any Hailstone player online as a prompt, and EQEmu's only reached the inviter's zone;
2. leaves the invitation in the data bucket `hailstone:guildinvite:<character id>` (20 s) and signals the
   character (`eq.cross_zone_signal_client_by_char_id`);
3. in the invitee's zone, sends the client the invitation (wire opcode 0x1431), which asks the usual
   question ("%1 is interested in having you join %2.", 10 s). A non-Hailstone invitee gets no invitation and the
   inviter gets EQEmu's "must be in zone". If no zone takes the invitation within 3 s the invitee is not online, and
   the inviter is told so;
4. on the answer (again the guild packet, in whatever zone the invitee is then), declines with EQEmu's words or puts
   the invitee in the guild with `eq.set_guild`, then sends the client the guild's rank permissions, rank names and
   roster as EQEmu does for a new member. (EQEmu forwards a join only to zones where world sees a guild member, so
   without this an invitee alone in its zone would not see guild chat until it zoned.)

Other clients, and Hailstone on a server without the script (or with `guild_invites_world` 0), keep EQEmu's
same-zone invitation. Both packets are 80 bytes: `{u32 magic; u16 version 1; u16 action; char name[64];
u32 guild id; u32 value}`; the actions are in `hailstone.lua` and `src/net/social_packets.h`.
