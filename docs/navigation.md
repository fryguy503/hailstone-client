# Navigation

Code:

- `src/nav/`: the mesh, its cache, the path follower.
- `src/app/navigator.{h,cpp}`: the client's side.
- `src/mq/nav_commands.cpp`: `/nav`, `/moveto`, `/stick`.
- `src/mq/tlo_game.cpp`: the `Navigation` and `Stick` TLOs.

Tests: `tests/nav_test.cpp`, and `fuzz: navigation cache files and their tiles` in `tests/fuzz_assets_test.cpp`.

The original client has no navigation of its own. This is a Hailstone addition for scripted and plugin-driven
characters, in the spirit of MacroQuest's MQ2Nav. It takes MQ2Nav's commands and TLO, not its code (MQ2Nav's
repository has no licence). The meshes are built with Recast/Detour (zlib, `THIRD_PARTY.md`).

## What it drives

Only what a player drives: the heading and the movement keys (forward, strafe, jump), once a frame, where `/follow`
steers (`app/client_app_player.cpp`). Speed, collision, gravity and the position updates the server sees are the
movement rules' own (`docs/mechanics.md`), so a navigating character moves exactly as one under a player's hands. The
heading is set at once, as `/follow`'s FollowPlayerAI sets it (`game/follow.h`). Nothing writes the position.

- The player's own forward, back, turn or strafe keys end a walk, as they end `/follow`.
- Starting a walk ends `/follow`.
- Nothing moves while the player cannot. A temporary pause (sitting, frozen, `/safelock`) carries on when they can.

Spawn walks and `/stick` are tied to the actor they were given: its despawn, the reuse of its ID, zoning or the
player's death ends them, even paused. A new command can select the new actor.

The isometric camera's click to move (`docs/isometric.md`) walks with it too: `Navigator::walk_to_click` for the
ground (quiet when the mesh goes only part of the way, straight at the point in a zone without a mesh), and
`go_to_spawn` and `go_to_door` for what a click walks to.

## The mesh

Built from the zone's collision triangles (`game::CollisionMesh`): what the movement sweep collides with is what is
rasterised, so there is no separate tool and no hand-made file.

**Which triangles.**

- Passable triangles (WLD polygon flag 0x10, EQG flag 1) are left out; the sweep never hits them.
- The exception is a water surface: a flat passable triangle with water 1.5 under its middle and none 1.5 over it.
  It is ground for the mesh, so a path can cross water whose bottom is too deep or too steep to walk ("Water" below).
- Triangles wholly outside the terrain's box, 200 units round it, are left out (the movement sweep still has them).
  Zones park unused objects at z -32768, which would stretch the heightfield past Recast's span heights.
- A triangle whose record normal faces up within 55 degrees is ground. A triangle facing down is never ground, only an
  obstacle (the sweep skips back faces).

**The agent.** Height 6, radius 2, climb 4, cells of 0.6 x 0.3, tiles of 128 cells (76.8 units). These are not the
original's numbers. The movement sweep's own radius is provisional (`docs/unknowns.md`), so they are conservative: a
path keeps 2 units from walls, more than the 1.78 sweep radius of a human. The climb is 4, not 3, because the sweep
starts lifted over the feet (DoCollisionMovement's lift), so a player walks up a step a little over 3.

**Wall clearance.** A path's bends keep further off walls where there is room, so a walk does not scrape along walls
and catch on corners (Hailstone's own). `PathOptions::wall_clearance`: 2.5 more than the mesh's margin by default,
`/nav option clearance <0..6>`.

Each bend is moved out along the bisector of the way in and out, by the clearance less
its distance to the nearest wall, or half that; a moved bend is kept only where it, and the lines to and from it, stay
on the mesh and no nearer another wall. The ends, and either end of a drop or swim link, are not moved. (A wider margin
for the whole mesh would close narrow ways.)

**Areas** come from the zone's regions (`assets::ZoneRegions::lookup`, 2 units above each polygon's middle, and 1.5
under it for a water surface): water, lava and slime, zone lines, PvP. Zone lines are thin, so their volumes (BSP
leaves' or EQG boxes, 10 higher for buried ones) are also marked into each tile (`nav::AreaBox`).

| Area | Search cost |
|---|---|
| Water | 3 |
| Lava | 20, and never crossed unless allowed |
| Zone lines | 1000: stepping in leaves the zone, so a path crosses one only when there is no other way |

### Doors

Doors are not in the collision mesh, so the mesh runs through every doorway, open or shut. The client cannot know a
door's state on the server: EQEmu shuts most doors after their close timer (5 s, `doors.close_timer_ms`) without
telling clients, the client shuts its own after its repop time (10 or 20 s), and an NPC leaving a waypoint opens doors
told only to players within 200.

**Passing open doors** (a Hailstone difference, on by default: `/nav option passdoors on|off`). While `/nav` or
`/stick` moves the player, a door that is not shut (open, opening or closing) does not block, for the movement sweep or
the walk's door checks. Shut doors still block and are clicked. The player's own movement always collides as the
original's does.

With the option on, the walk clicks a shut door in its way (once more if the answer leaves it as it
was) and walks through while it swings. The ways round and give-way shutting below apply only with the option off: they
swung doors the wrong way and stuck walks in the Plane of Hate.

**Clicked ahead.** Every 0.2 s the walk sweeps a body-sized sphere up to 30 units along its path. A door at rest
closed that blocks movement and whose model it hits is clicked as soon as the player is within reach
(`GameConnection::door_in_reach`), without stopping. A door that reads open while in the way is left to the stall
handling. Reaching a door clicked early that is still swinging, the walk holds; one still shut is clicked once more.

**Walking round an open leaf** (option off). When the sweep 6 on hits a door that rests open, the walk looks to either
side (1.5 to 9, nearer first) for a reachable point clear of the leaf and walls, with the way on clear, then goes by it
and a point 4 past the leaf. At most twice per door and walk; each restarts the stall clock.

- A door a walk opened is not clicked shut while there may be a way round it, for 2 minutes and by later walks too.
- Once its ways round have failed, the door is taken to swing across the passage and is shut at the stall.
- No door is clicked more than twice in 2 minutes by the walks. Exception: a door shut that way which a walk then
  finds shut in its way is opened again, once.
- What the walks did with doors is forgotten at a zone load: door ids are the zone's.

**Keeping out of the swing.** A door a walk clicks is kept clear of while it swings: within 14 of its origin (times its
scale) the walk steps back and holds at that edge until the door comes to rest, has not moved 1.5 s after the click,
or 8 s have passed.

**A click the server reads the other way.** A door clicked at a stall whose answer leaves it as it was (its timestamp
moves, its state never does) is clicked again, once: the server had it the other way (its close timer shut it while
the client still showed it open).

**Clicked at a stall.** A walk that stalls at a door clicks it, then carries on. The door is:

- the one whose model a body-sized sphere 6 units on towards the next corner hits, clicked whichever way it rests
  (some doors stand in the way while their state reads open: inverted ones, a gate's other halves, Kedge Keep's
  KEDGRATE2 and the secret door KEDBOX);
- or else a closed door whose origin is within 15 in front.

The door is tried at the first sign of a stall, before any jumping and strafing. The walk then holds until the door
moved and no door is hit by the same sweep, at most 8 s, and its stuck counts restart; the next piece of a gate is
clicked at once.

A door is not clicked again within 15 s of the walk clicking it. A door that never moves (locked, a
key, a lever elsewhere) is not clicked again on that walk, and a walk that then gets stuck says which door did not
open. A door whose click teleports cannot be told from the RoF2 door record, so `/nav option doors off` turns all
this off (`docs/unknowns.md`).

**The stuck log.** A walk that gives up logs `nav: stuck at ...`: where it stood, its next corner, each door within 50
(place, heading and closed heading, swing, state, type, scale, whether its model is on the body, whether a walk opened
it, its recent clicks) and the door the way on runs into. Early clicks and ways round are logged too.

### Links: drops and swims

Recast meshes only what is walked, so a ledge, a pool's floor and the water over it are separate parts of the mesh.
Each tile's build joins them with Detour off-mesh links (`find_links` in `nav/nav_mesh.cpp`).

**Drops.** From every open edge of the mesh, every 5 units, the builder looks 6 (then 3.5) units out. With nothing
across the way at the knees and the head, the first floor straight down is the landing. A walkable landing more than
a climb below gets a one-way link when the fall is within the safe height or it lands in water.

- The safe height is 20 units: the most the original's fall-damage formula (ApplyFallDamage,
  `game/mechanics/movement.h`) forgives a character carrying nothing, falling from rest at the default gravity.
- Into water the drop can be any depth up to 2000 (no fall damage in water). It is a swim (water's cost, left out
  with water avoided).
- A drop into water goes both ways when the water comes up to within 5 of the ledge, whether it lands on the surface
  or the floor under it.

**Swims.** A polygon under deep water (water 2 over its middle) whose column of water reaches a water surface over it
gets a link both ways to that surface, at most one every 15 units, at water's cost (3). A swim link between a ledge
and the water beside or under it turns at the top (or foot) of the open column it was found in, since the straight
line between its ends can run into the floor over the water.

**Walking the links.**

- A drop costs twice its length, so a path takes one when it saves the long way round.
- The walk takes a link as it takes any corner: a drop by walking off the edge towards the landing, a swim by the look
  pitch (steeper, up to 80 degrees, when the next corner is straight over or under).
- A walk that starts off the mesh starts from the nearest polygon within 100 up or down.
- A walk that cannot find a new path while off the mesh (falling, or in a column of water) carries on along the one it
  has.
- `/nav ui` draws the drops in orange and the swims (and drops that go both ways) in light blue.

**Walked:** Kedge Keep from the arrival to Phinigel Autropos, all under water; Lower Guk from the undead side's
entrance to the ghoul lord, a drop into a flooded room and the swim back out. The real-zones test (`EQ_GAME_DIR`) walks
Old Sebilis from the zone-in to Trakanon's lair over the zone's collision, through a drop into water and a swim.

### When it is built

At the zone-in: read from the zone's cache file when there is one, and built otherwise.

- `nav.prebuild=0` in `hailstone.ini` turns the build at the zone-in off. The mesh is then built when first wanted: a
  `/nav`, the overlay or the Navigation TLO's path questions.
- The tiles are built on worker threads (`nav.threads`, default 2, at most 8), nearest the player first. A walk asked
  for meanwhile starts when its tiles are there; one that needs a new path while the mesh is unfinished waits for its
  tiles instead of giving up.

A build takes from under 0.1 s (befallen) to about 5 s (anguish) with 4 threads.

**Asking the server meanwhile.** A walk to a spawn with no mesh under the player yet asks the server for its path: the
same `OP_FindPersonRequest` as `/find`, but quiet (`GameConnection::request_quiet_path`: no path drawn, no sound, no
chat line). A stock EQEmu server answers from its own navigation mesh when it has one.

Replies carry no request id, so each goes to the oldest request in flight. EQEmu does not promise that order (an empty
reply is sent at once, a full path when its thread pool is done), so a quiet request and a failing `/find` just after
it can swap (`docs/unknowns.md`). EQEmu leaves a find of a trader unanswered; a request still waiting after 10 s (our
own number) is taken as never answered.

A `/nav` word of three letters or more that begins only one subcommand stands for it (`/nav tar` is `/nav target`).

### Cache

`cache/navmesh/<server profile>/<zone>.navmesh` under the data folder (`docs/layout.md`; `default` without a saved
server). Safe to delete. Layout (`nav::encode_cache`):

```
u32 magic "HSNM"   u32 version (4)   u32 native layout (Detour version | sizeof(dtPolyRef) << 16 | little-endian << 24)
u64 geometry hash  u32 build-parameter hash   u64 builder fingerprint
f32 origin x, y, z (Recast axes)   f32 tile size   i32 tiles x, tiles y
u32 tile count, then for each: i32 tx, i32 ty, u32 size, the tile's Detour data
u64 FNV-1a of everything before it
```

**Builder fingerprint** (`nav::builder_fingerprint`): a hash of what this build of the client makes of a small fixed
scene (`nav::fingerprint_scene`, one of each thing the builder makes: ground and a step, a drop, a pool's floor joined
to its surface, a flooded room, a zone line's box) and of the links' numbers, computed once at the first zone load. A
file from a builder that makes anything else of it is rebuilt, with no version to raise by hand. A new kind of link
should be added to the scene (a test checks each kind is there).

**Reading the file.** The fields are little-endian (`core/byte_io.h`). The tile data is Detour's own in-memory layout,
so a file is read back only by a build with the same native layout. Reading Detour's structs straight out of those
bytes (the tile checks in `nav_mesh.cpp`) is the one accepted exception to the rule that file data goes through
`ByteReader`: Detour takes a tile only in that layout, and the file is this client's own cache, guarded by the
native-layout word and the checksum.

A file whose hashes or grid do not match the zone's geometry and the build's parameters, or whose checksum fails, is
rebuilt. Each tile is then checked before Detour sees it (`NavMesh::add_tile`): its size against its header's counts,
every polygon's vertex, neighbour and detail indices, a boundary edge in each detail mesh, finite coordinates within
the world, and the bounding-volume tree's indices.

## Floors over floors

Recast's mesh has every level of a building, so a goal's height matters:

- The goal's polygon is the nearest round it (4 across, 12 up or down), height counting four times as much as distance
  across. The floor under a spawn wins over the one under that, or over it.
- When that polygon cannot be reached, instead of Detour's fallback (usually the floor right under the goal) the
  polygons within 32 across and 64 up or down are tried, best first by the same measure; the first reachable one is
  the end (the near edge of the goal's own floor, when there is one).
- A corner, or the end, counts as reached within 2 across (the end: the walk's distance) and within 8 in height. The
  server's path (`/find`'s) is at its own mesh's heights and is reached by distance across alone.
- A walk that still ends more than 8 over or under the goal says "Stopped 12 below ...: the mesh has no way up there"
  (a lift, a jump or a drop it does not know).

## Zone lines on the way

A walk never steps into a zone line by accident. When the step ahead (3 units on, a body's middle up) is in a zone
line's region, the walk stops with "Stopped at a zone line on the way to ...", unless zone lines are allowed
(`/nav option zonelines on`) or the goal itself is in one. Seen live at the border of Oasis of Marr and North Ro.

## Water

The mesh has the water surfaces (above) as well as the bottoms, marked Water. The search pays 3 for them, or never
crosses them with `/nav option water off`.

Forward swims along the look pitch, so while swimming (BodyWet 5 and up) the walk sets the pitch each frame
(`Navigator::swim_pitch`; /follow does the same):

- The head under water and the air below 35% of the most: straight up, whatever the corner.
- A corner out of the water within 12: up, so the original's swim jump (DoSwimJump) climbs out onto the shore.
- At the surface (the head out, or no water a little over the head) and the way ahead no deeper: level.
- Otherwise towards the point 4 units on along the path at the path's height there (`PathFollower::ahead`), at most
  63 degrees (80 when the next corner is straight over or under: a swim link).

Aiming along the path, not straight at its next corner, keeps the swim at the path's height through low openings.
These numbers are ours. Out of the water the pitch is left alone.

## /follow

A Hailstone change to the original's /follow (`game/follow.h`, FollowPlayerAI), on by default and switchable:
`hailstone.ini` `nav.follow=0`, or `/nav option follow off`. The original turns only while it has a clear line to the
followed spawn and runs straight at it, so a corner loses it, and it gives up past 200.

- While the spawn is in a clear line that the mesh can walk straight, the original's step steers, exactly as it does.
- Otherwise the walk takes the mesh's path to where the spawn stands, re-planned as it moves (every 0.5 s once it has
  moved 3), with the walk's own unsticking.
- The original's bands still decide stopping, walking and running, measured along the path: stop inside 15 + both
  heights, walk inside 30 + both heights when the spawn is slower.
- The follow keeps on to 400 away. The original's "too far" message still ends it past 400 or when the spawn dies.
- The help's own strafes do not end the follow (the player's turn and strafe keys still do). If the help gets stuck
  it hands back to the original's step for 5 s, then tries again.
- Off the mesh (levitating, swimming, a ledge it left out) the original's step steers alone.
- `/nav ui` draws the follow's path in cyan.
- A `/nav` walk and /follow do not run together: either one started ends the other.

## Getting unstuck

1. A walk that stops getting closer to its next corner for 1 s jumps and strafes for 0.6 s, one side and then the
   other.
2. After three of those it finds a new path from where it stands, or first clicks a closed door in the way.
3. After three new paths with no corner reached it stops with "Navigation stuck going to ...".

Those times count only up to 0.25 s of any one frame, so a hitch is not a stall. A strafe is held back (the walk only
jumps) when the floor 4 to either side is more than 5 under the feet, so it never steps off a walkway over a pit.

## Commands

`/nav` (MQ2Nav's set; the 'move' permission for plugins and scripts, `plugin/policy.h`):

| Command | |
|---|---|
| `/nav id <#>` / `target` / `spawn <search>` | to a spawn (MQ's search words, `docs/plugins.md`), following it as it moves; done within 5 |
| `/nav loc <y> <x> <z>` (`locyxz`) / `locxyz <x> <y> <z>` / `locyx <y> <x>` | to a point (`/loc` order, or x y z); done within 2 |
| `/nav door <name> [click]` / `door id <#> [click]` | to the nearest door whose name contains the text; `click` uses it on arrival |
| `distance=<n>` (`dist=<n>`) | anywhere on the line: stop this close |
| `/nav help` (also `/nav` alone) | the commands |
| `/nav stop` / `pause` | stop; pause or go on |
| `/nav status` | the mesh's tiles and the walk |
| `/nav ui` (`overlay`) | the mesh, its links and the path, over the 3D view |
| `/nav reload` (`rebuild`) | build the zone's mesh again, ignoring the cache |
| `/nav option water\|lava\|pvp\|zonelines\|doors\|passdoors\|follow on\|off` | switches |
| `/nav option clearance <0..6>` | how much further than the mesh's margin a path's bends keep from walls (2.5; 0: hug the margin) |

- `distance=` is measured along the path, however many corners are left. In height the end may be as far off as the
  distance (at least 8).
- `/nav ui` colours: the mesh in green ground, blue water, red lava, purple zone lines, yellow PvP; its links in
  orange drops, light blue swims.
- `/nav option` switches and defaults: swim (on), cross lava (off), enter PvP areas (on), walk through zone lines
  (off), open doors (on), walk through doors that are not shut (on), help /follow (on).

Not yet: `/nav item`, waypoints (`/nav waypoint`, `/nav recordwaypoint`).

`/moveto` (MQ2MoveUtils' straight walk, no mesh): `/moveto loc <y> <x> [z]` (or `y,x`), `/moveto id [#]` (the target
without a number), `/moveto mdist <n>` (where it stops, default 10), `/moveto off`.

**/stick** (MQ2MoveUtils' words; Hailstone's own walk, `NavControl::stick`) keeps to the target until `/stick off`, the
player's own movement keys, or the spawn dying or leaving. Near enough it holds still and faces the spawn. Further, it
walks, the path found again as the spawn moves: straight when the spot is within 40, in plain sight, and the straight
line stays on the mesh until within that distance of the spot; over the mesh otherwise (straight without a mesh, or
off it).

`/stick [<distance>] [behind] [id <#>]`:

- The distance is flat. By default it is three quarters of the player's melee range of the spawn (the original's
  `__get_melee_range`), to stay in reach while the spawn shifts: Hailstone's choice.
- `behind` stands at its back, within 45 degrees.
- It keeps to the spawn it was given, whatever is targeted later (MQ2MoveUtils' `hold`). `/stick` again moves it to
  the new target.
- A `/nav` or `/moveto` walk ends a stick, and a stick ends a walk.

Not yet: MQ2MoveUtils' `pin`, `!front`, `moveback`, `uw` and percent distances.

`${Stick}` (ON / OFF), `.Active`, `.Status` (ON, PAUSED, OFF), `.Distance`, `.StickTarget` (the spawn's id, 0 none),
`.StickTargetName`.

### The Navigation TLO

`${Navigation}` (TRUE while walking), `.Active`, `.Paused`, `.MeshLoaded`, `.PathExists[<dest>]`,
`.PathLength[<dest>]` (-1 without a path). Hailstone's own: `.MeshComplete`, `.TilesBuilt`, `.TilesTotal`,
`.Destination`, `.Remaining`.

A destination is `id <#>`, `target`, `loc <y> <x> <z>` (`locyxz`), `locxyz <x> <y> <z>` or a spawn search. Lua reads
it as `mq.TLO.Navigation.Active()`; plugins with the SDK's `parse` and run `/nav` with `do_command`.

## Find

**Server policy.** An active server extension may disable NPC Find or own map spawn presentation
(`ServerExtension::map_policy`, `hs.UI.map.Policy()`). Both packs then hide cached NPC overlays and competing controls
without writing settings, and native discovery, targeting, guide, path and walk entry points enforce the same policy.
Find's guides and walks are tied to the connection, player and NPC they started with; denial, replacement or zoning
ends them. PC group/raid Find and coordinate navigation are unaffected.

**Matching replies.** Find replies carry no request identity, so each goes to the oldest request in flight; shown Find
paths and quiet navigation requests share that order. A reply whose request was cancelled or replaced (a new `/find`, a
stopped walk, a lost actor), or with no request waiting, is dropped in its place, and later requests keep their own. A
request unanswered after 10 s is dropped.

**Tracking** keeps one `OP_Track` in flight at a time, since its replies carry no request id either.

- Any reply answers it: a list (a short one is read as RoF2 reads it, zero past its end), an empty list or the stock
  timer reply.
- One unanswered after 10 s (our own number) is dropped and the next refresh goes out; the list shown stays.
- A reply with nothing asked, or to a request whose window was closed or whose server policy changed since, is not
  shown.
- Only a server extension's tracking rule or map policy keeps Tracking closed (docs/plugins.md).
- The player's own death empties the list, as it ends every Find and walk (docs/unknowns.md).

### The Find window

A Hailstone addition, in place of the original's Find window (FindLocationWnd, whose code is gone): a list of the NPCs
a new player looks for in the zone, from what every stock EQEmu server sends (`game/useful_npcs.h`,
`app/lua_ui_find.cpp`).

The find key (`CMD_TOGGLE_FIND_WINDOW`), the EQ menu's Find and the Actions window's Find toggle it as the original's
key did: open, it closes; closed with a find path shown, the path ends; else it opens. The modern pack draws it as the
map's Find page; the gamepad pack as a window of its own (More > Find).

What it lists:

- **By the spawn's class.** Merchants (41), bankers (40, 66), guildmasters (20..35), special merchants for points and
  other currencies (58, 59, 61, 67, 68, 70, 73) and other services (tribute 63, 64, 74; adventure 60; fellowship 69;
  mercenaries 71; real estate 72). Never players, corpses, pets or the LDoN objects of class 62.
- **Quest givers.** With Hailstone's server script (server/README.md "Quest givers in the Find window"): the NPCs whose
  quest file takes hand-ins or talks, asked once a zone when the list is first wanted (`net/quest_npc_packets.h`) and
  matched to spawns by the name as shown. An NPC listed for its trade gets a "!" beside it. Without the script there
  are none: the client does not guess.
- **Points of interest.** The other NPCs on the server's findable list (OP_SendFindableNPCs, the npc_types `findable`
  flag; docs/protocol/rof2-packets.md), which the original's window listed. Zoning clears the list.

How it works:

- Each row: the name, the surname (else the class's name) and the distance. Nearest first, a search on the name and
  surname, and a filter per kind (`find.hidden` in the pack's settings).
- A click targets the NPC and centres the map on it.
- Show path (or a double click) starts the guide path to it (below).
- Walk there (the gamepad's X) walks the player there as `/nav id` does, to within 10, with the guide path showing.
  The player's movement keys stop the walk; the guide path stays.
- The map's NPCs button (`map.npcMarks`, off at first) marks the listed NPCs on it and on the minimap, by the same
  kinds and search (`ui/modern/lib/find.lua`). The selected and hovered ones are marked always.
- Positions are where the client has the spawn, so a walking NPC far off may be shown a little behind.

### The guide path

`app/guide_path.h`: a trail from the player to the NPC over the client's own mesh (`Navigator::plan_between`, the
walk's options), so it goes round walls on every zone, unlike the server's find path where the server has no mesh.

- It is found again when the player is more than 8 (flat) off it or the NPC has moved 6 since, at most every half
  second, and trimmed behind the player.
- It ends within 12 of the NPC (and 10 in height) with "You have reached ...". It also ends when the NPC dies or goes,
  when there is no way there, on zoning, with End path / End Find, or the find key with the Find window closed.
- While the mesh is not ready it says once that the path will show when it is.
- A partial path (the NPC off the mesh) is drawn as far as it goes.
- The maps draw it in gold beside the find path.

### The trail

Options > Display > **Find Trail** (gamepad: Options > Interface) chooses the look of both the guide path and the
server's find path (PlayerPath), immediately, saved per character in `hailstone.ini` as `display.find_trail_style`:
**Gold line (default)** (0), or **Rainbow trail** (1). Unknown values select the default. It changes only the look,
not the route, and does not start a walk. It works from the first zone entry, also on servers without downloaded
assets.

`game/path_trail.h`: the default is a narrow gold ribbon with a dark border, in the style of MQ2Nav's route line,
following the ground and depth-tested against the scene (the diagnostic overlay is `/nav ui`).

**Rainbow trail:** fourteen thin, solid strands, two in each of seven colours, each winding sideways and curling
vertically with its own phase, about 3 units above the terrain (a 0.7-unit vertical wave). Bright bands move towards
the destination at 9 units a second. The strands fade in over the first 5 units and out at the far end (the guide
path's 120..170; the find path's at Path.ini's FadeEnd). The original draws the find path as a textured tube; this look
is Hailstone's.

**Following the ground.** The ground line is lifted 0.35 units; the rainbow follows the same profile, each strand
grounded separately across the trail's width. Probes advance in small steps from the last supported height, so hills
and valleys between route points are followed. Missing ground or a sharp drop leaves a gap rather than bridging it or
snapping to a floor below a bridge; tunnel roofs are not picked. Without loaded collision there is no trail.

The selected style replaces the find path's travelling Path.ini particle clouds. Its completion emitters play only at
a supported destination, grounded as the trail is. The route, arrival checks, sounds and map colours are unchanged.

Not yet: the gamepad pack's minimap marks, and a key of the client's own for a walk to the target (`/nav target` on a
hot button does it) (docs/todo.md).

## Zone atlas

Code: `game/zone_routes.h` (the links and routes), `game/atlas.h` (`index_atlas`), `net/zone_link_packets.h`, the
`hs.Atlas*` / `hs.Zone*` functions in `app/lua_ui_map.cpp`, and the modern pack's `ui/modern/lib/atlas.lua` (drawn
by `map.lua`). Tests: `tests/zone_routes_test.cpp`, the index cases in `tests/atlas_test.cpp`.

The map window's **World** view, a Hailstone addition. RoF2 had an atlas page in the map window (the game folder's
`Atlas\<skin>\*.xml`, kept as the stock window's MVW_AtlasRenderArea, `hs.DrawAtlas`) and a zone guide fed by the
server, which EQEmu does not send (`docs/unknowns.md`). The World view draws the same atlas pages, with:

- **The atlas.** Every page reachable from `Default.xml`, zoomed with the wheel (up to 5x) and panned by dragging,
  with a breadcrumb back to the world.
  - A region click opens its page; a zone click selects it.
  - A double click shows its map in the Zone view, with a bar back to your own (without the player, find path or
    height fade); closing the window goes back to your zone.
  - Your zone pulses, the selected zone is outlined, and the zones of the route followed are numbered and joined.
- **The zone finder.** Every zone the atlas links that the server has (below), by the zone table's long name or the
  atlas' tooltip and its short name (prefix and word matches first); with nothing typed, the zones of the page shown.
  Each comes with its region and expansion. The selected zone's card shows how many zones away it is and the way
  there, with **On atlas**, **Its map** and **Go here**.
- **A route followed** (`atlas.route` in `ui/settings/modern.txt` under the data folder): a banner over your zone's
  map with the next zone and **Walk there**.
  - **Walk there** runs `/nav locxyz` to the nearest part of the zone line to that zone, 2 units inside it, so the
    walk goes through rather than stopping at it ("Zone lines on the way" above). It needs the 'move' permission.
  - Your zone's exits are marked on its map (the next one stands out); the map's right-click menu hides them.

**Where routes come from.** How zones connect is kept per server in `servers/<profile>/zone_links.txt`
(`docs/layout.md`), from two sources:

- The server's list, when it runs Hailstone's server script (`server/README.md` "Zone links"): every zone point the
  server's expansion and content flags allow. The client asks (OP_HsZoneLinksRequest 0x1438) once a session, the
  first time the World view is open after a zone has answered the settings request. A server without the script is
  never asked.
- Learned: the targets of each zone's OP_SendZonepoints, as the player enters it.

**Only the server's zones.** The finder lists, and a route passes through, only the zones the server has; the atlas
dims the others ("Not on this server"). The finder's footer says which rule applies:

- With the script's list: the zones its current expansion (`Expansion:CurrentExpansion`) and content flags allow,
  sent with the links.
- Without it: the account's expansions (the profile's or OP_ExpansionInfo's bits, expansion n at bit n − 1, Classic
  always). EQEmu fills those from `World:ExpansionSettings` or the client version rather than
  `Expansion:CurrentExpansion`, so a server that limits only its content shows every zone until it runs the script.

**Routes and exits.**

- A route has the fewest zone changes (the lowest zone ids first among equals).
- When the links in their own direction give none, a learned link may be used backwards (a zone line seen only from
  its far side); the card says so, since not all zone lines go both ways.
- The exits on the map come from the zone's zone-line regions and the zone points they name
  (`game::collect_zone_exits`, `game/zone_exits.h`).
- A zone point reached some other way (a teleporter NPC, a boat, a door) has no line, so **Walk there** is off for it.

## Not done

- Swimming is not yet seen live end to end (unit tests only). Levitation and boats are not handled.
- Lifts, and jumps up onto ledges, as links between parts of the mesh; walks across zones (`/travel`); tactical
  queries (a spot in range with line of sight, behind a mob); a mesh editor and waypoints; several characters in one
  process.
