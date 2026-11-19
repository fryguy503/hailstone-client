# Navigation

Code: `src/nav/` (the mesh, its cache, the path follower), `src/app/navigator.{h,cpp}` (the client's side),
`src/mq/nav_commands.cpp` (`/nav`, `/moveto`, `/stick`) and the `Navigation` and `Stick` TLOs in `src/mq/tlo_game.cpp`. Tests:
`tests/nav_test.cpp`, and `fuzz: navigation cache files and their tiles` in `tests/fuzz_assets_test.cpp`.

The original client has no navigation of its own. This is a Hailstone addition for scripted and plugin-driven
characters, in the spirit of MacroQuest's MQ2Nav: it takes MQ2Nav's commands and TLO, not its code (MQ2Nav's
repository has no licence). The meshes are built with Recast/Detour (zlib, `THIRD_PARTY.md`).

## What it drives

Only what a player drives: the heading and the movement keys (forward, strafe, jump), once a frame, where `/follow`
steers (`app/client_app_player.cpp`). Speed, collision, gravity and the position updates the server sees are the
movement rules' own (`docs/mechanics.md`), so a navigating character moves exactly as one under a player's hands.
The heading is set at once, as `/follow`'s FollowPlayerAI sets it (`game/follow.h`). Nothing writes the position.

The player's own forward, back, turn or strafe keys end a walk, as they end `/follow`. Starting a walk ends `/follow`.
Nothing moves while the player cannot. A temporary pause (sitting, frozen, `/safelock`) carries on when they can.

Spawn walks and `/stick` keep the selected actor's incarnation and the connection/player context. Despawning,
reusing its ID, zoning or replacing the player's life retires that intent, including a paused or stationary stick.
An explicit new command can select the new actor. Zone unloading also clears the follow helper's mesh detour.

The isometric camera's click to move (`docs/isometric.md`) walks with it too: `Navigator::walk_to_click` for the
ground (quiet when the mesh goes only part of the way, straight at the point in a zone without a mesh), `go_to_spawn`
and `go_to_door` for what a click walks to.

## The mesh

Built from the zone's collision triangles (`game::CollisionMesh`): what the movement sweep collides with is what is
rasterised, so there is no separate tool and no hand-made file.

- Passable triangles (WLD polygon flag 0x10, EQG flag 1) are left out; the sweep never hits them. The exception is
  a water surface: a flat passable triangle with water 1.5 under its middle and none 1.5 over it. It is ground for
  the mesh, so a path can cross water whose bottom is too deep or too steep to walk ("Water" below).
- Triangles wholly outside the terrain's box, 200 units round it, are left out. Zones park the objects they do not
  use far under the world (Field of Bone's trees stand at z -32768); taken in, they stretched the heightfield past
  Recast's span heights (8191 cells, about 2457 units) and the whole mesh was built 32000 units under the zone. The
  movement sweep still has them.
- A triangle whose record normal faces up within 55 degrees is ground. A floor stops a fall only when its normal faces
  up (the sweep skips back faces), so a triangle facing down is never ground, only an obstacle.
- Agent: height 6, radius 2, climb 4, cells of 0.6 x 0.3, tiles of 128 cells (76.8 units). These are not the
  original's numbers. The movement sweep's own radius is provisional (`docs/unknowns.md`), so they are
  conservative: a path keeps 2 units from walls, more than the 1.78 sweep radius of a human. The climb is 4, not 3:
  the sweep starts lifted over the feet (DoCollisionMovement's lift), so a player walks up a step a little over 3
  (the walkway into Trakanon's lair in Old Sebilis is one).
- A path's bends keep further off walls where there is room (`PathOptions::wall_clearance`, 2.5 more than the
  mesh's margin by default, `/nav option clearance <0..6>`; Hailstone's own, so a walk does not scrape along walls and
  catch on corners). The search's bends lie on the margin's edge, wrapped round what is inside each turn; each is
  moved out from the turn (the bisector of the way in and out, read from the path as found, so a corner the mesh
  rounds into two bends moves as a whole) by the clearance less its distance to the nearest wall, or half that, and
  kept only where it is still on the mesh, no nearer another wall, and the lines to it and on from it stay on the
  mesh. Not the ends, nor either end of a drop or swim link. A wider margin for the whole mesh would close narrow
  ways and need every zone's mesh built again.
- Areas come from the zone's regions (`assets::ZoneRegions::lookup`, 2 units above each polygon's middle, and 1.5
  under it for a water surface): water, lava and slime, zone lines, PvP. Zone lines are thin, so their volumes
  (their BSP leaves' boxes, or EQG's boxes, 10 higher for the buried ones `game/zone_lines.h` fires from above) are
  also marked into each tile as it is built (`nav::AreaBox`, Recast's box marking): polygons split along their edges
  and none that only overlaps one passes for ground. The search costs water 3, lava 20 (and never crosses it unless allowed), and zone
  lines 1000 (stepping in leaves the zone, so a path crosses one only when there is no other way).
- A Hailstone difference, on by default (`/nav option passdoors on|off`): while `/nav` or `/stick` moves the
  player, a door that is not shut (open, opening or closing) does not block, neither the movement sweep nor the walk's
  look for doors in its way; shut doors still block and are clicked, and the player's own movement always collides
  as the original's does. The client cannot know a door's state on the server: EQEmu shuts most doors after their
  close timer (5 s) without telling the clients, the client shuts its own after its repop time (10 or 20 s), and an
  NPC leaving a waypoint opens the doors near it, told only to players within 200. Clicks to get an open leaf out of
  the way (the ways round, shutting it to give way, keeping out of its swing, below) swung doors the wrong way and
  left walks stuck against them in the Plane of Hate. With the option on, the walk clicks a shut door in its way,
  once more if the answer leaves it as it was (below), and walks on through it while it swings; the ways round, the
  give-way shutting and the swing hold below apply only with the option off.
- Doors are not in the collision mesh, so the mesh runs through every doorway, open or shut. A door ahead is
  clicked early: every 0.2 s the walk sweeps a body-sized sphere along its path, at most 30 units on, and a door
  whose model it hits is clicked as soon as the player is within reach of it (UseSwitch's test,
  `GameConnection::door_in_reach`), without stopping, so it has swung open by the time the walk gets there. Only a
  door at rest closed that blocks movement is clicked early; one that reads open while in the way is left to the
  stall below. Reaching a door clicked early that is still swinging, the walk holds for it as below; one that is
  shut (it never moved, or it has closed again) is clicked once more there.
- An open door's leaf in the way (doors swing into the passage, and the mesh runs through them) is walked round,
  not clicked: when the sweep 6 on hits a door that rests open, the walk looks out to either side (1.5 to 9, the
  nearer first) for a point it can reach clear of the leaf and the zone's walls from which the way on is clear too,
  and goes by it and a point 4 past the leaf before the path's next corner, at most twice per door and walk (each
  way round starts the stall clock again). A door a walk opened is not clicked shut while there may be a way round
  it, for 2 minutes and by later walks too (a script starts walk after walk), since the click would swing it shut in
  the walk's face (The Plane of Hate's HPMDR401 was opened and shut every 15 s this way). Once its ways round have
  failed, or there is none, it is in the way open: a door that swings across the passage being walked, not the one
  walked through (The Plane of Hate's side doors, HPSDR401SM), and it is shut at the stall. No door is clicked more
  than twice in 2 minutes by the walks, so one that blocks the way open and shut is not swung for ever, with one
  exception: a door shut that way which a walk then finds shut in its way (by the stall's sweep, or ahead on the
  path) is the one being walked through, and it is opened again, once, whatever the count and however lately the
  walk clicked it. A Plane of Hate side door's leaf lies across a passage as wide as the door, so from the leaf's
  side the doorway cannot be reached while it stands open; shut, walked up to and opened from in front, its leaf
  swings past the walker (the walk used to stand against it shut until the 2 minutes were up). What the walks did
  with doors is forgotten at a zone load: door ids are the zone's.
- A door a walk clicks (ahead, or at a stall) is kept clear of while it swings: within 14 of its origin (times its
  scale) the walk steps back away from it, at that edge it holds, and it goes on once the door has come to rest
  (moved and open or shut again), has not moved 1.5 s after the click, or 8 s have passed. A leaf swinging into the
  walker traps it: The Plane of Hate's side doors, clicked ahead from 25 or so, caught a walk that went on into the
  doorway while they swung (a walk farther off, or by a door whose origin is far from its doorway, is not held).
- A door clicked at a stall whose answer leaves it as it was (its timestamp moves, its state never does) is clicked
  again, once: the server had it the other way. EQEmu shuts most doors after their close timer (5 s by default,
  `doors.close_timer_ms`) without telling the clients, and the client shows them open until its own repop time (10
  or 20 s), so a click to shut one in between opens it on the server instead. The walk took that for a door that did
  not open and gave up against the leaf (The Plane of Hate's side doors, shut to give way 5 to 10 s after a walk
  opened them).
- A walk that gives up stuck logs what it saw (`nav: stuck at ...`): where it stood, its next corner, and each door
  within 50 (place, heading and closed heading, swing, state, type, scale; whether its model is on the body, whether
  a walk opened it, its recent clicks), and the door the way on runs into. A door clicked ahead of the walk logs how
  far off it was and from where. A door
  reading open that no walk opened, with no way round (a gate, Kedge Keep's), is clicked at the stall, below. Each
  way round is logged with the door and its points.
- A walk that stalls at a door clicks it, then carries on. The door is the one whose model a body-sized sphere 6 units on towards the next
  corner hits (the movement sweep against the doors), clicked whichever way it rests, since some doors stand in
  the way while their state reads open (inverted ones, a gate's other halves: Kedge Keep's KEDGRATE2 and the
  secret door KEDBOX); or else a closed door whose origin is within 15 in front (a door's origin can be well off
  its doorway). A door is not clicked again within 15 s of the walk clicking it (a second click swings it back). The door is tried at the first sign of a stall, before any jumping and strafing, and the walk
  then holds still until the way is clear: the door moved and no door is hit by the same sweep (the next piece of
  a gate in the way is clicked at once; a gate's state can read opening for a while, and it closes again on its
  own), at most 8 s; its
  stuck counts then start again. A door that never moves (locked, a key, a lever elsewhere) is not clicked again on
  that walk, and a walk that then gets stuck says which door did not open. A door whose click teleports cannot
  be told from the RoF2 door record, so `/nav option doors off` turns this off (`docs/unknowns.md`).

### Links: drops and swims

Recast meshes only what is walked, so a ledge, a pool's floor and the water over it are separate parts of the mesh.
Each tile's build joins them with Detour off-mesh links (`find_links` in `nav/nav_mesh.cpp`):

- **Drops.** From every open edge of the mesh, every 5 units, the builder looks 6 (then 3.5) units out: with nothing
  across the way at the knees and the head, the first floor straight down is the landing. A walkable landing more
  than a climb below gets a one-way link, when the fall is within the safe height or it lands in water. The safe
  height is the most the original's fall-damage formula (ApplyFallDamage, `game/mechanics/movement.h`) forgives a
  character carrying nothing, falling from rest at the default gravity: 20 units. Into water the drop can be any
  depth (up to 2000; no fall damage in water: Kedge Keep's way in is a shaft of water some 270 deep), and it is a
  swim (water's cost, left out with water avoided). A drop into water goes both ways when the water comes up to within 5 of the ledge
  (swum back up and climbed out), whether it lands on the surface or on the floor under it.
- **Swims.** A polygon under deep water (water 2 over its middle) whose column of water reaches a water surface
  over it gets a link both ways to that surface, at most one every 15 units. Water's cost (3) applies.
- A swim link between a ledge and the water beside or under it is taken by way of the open column it was found in:
  the path turns at the top (or foot) of the column, straight up or down there, then across. The straight line
  between its ends runs under the ledge, into the floor of the room over the water (seen in Kedge Keep).
- A drop costs twice its length, so a path takes one when it saves the long way round. The walk takes a link as it
  takes any corner: a drop by walking off the edge towards the landing, a swim by the look pitch (steeper, up to 80
  degrees, when the next corner is straight over or under). A walk that starts off the mesh (floating high over the floor: Kedge Keep's arrivals are 26 over theirs) starts from
  the nearest polygon within 100 up or down. A walk that cannot find a new path while it is off the
  mesh (falling, or in a column of water) carries on along the one it has.
- `/nav ui` draws the drops in orange and the swims (and drops that go both ways) in light blue.

Kedge Keep from the arrival to Phinigel Autropos (all under water: down the shaft and through its halls, up and down
between their levels) and Lower Guk from the undead side's entrance to the ghoul lord are walked too: a drop into a flooded room
whose floor is under the water and the swim back up out of it. Seen in Old Sebilis (the real-zones test with `EQ_GAME_DIR`, walked by the movement rules over the zone's collision):
from the zone-in to Trakanon's lair is a drop of 56 into the water, a swim down to the floor under it and up again,
and the step into the lair. Without the links the walk stopped about 850 units short.

### When it is built

At the zone-in: read from the zone's cache file when there is one, and built otherwise, so a new zone's mesh is under
way before anything asks for it (a `/stick` near its target walks straight and never asks). `nav.prebuild=0` in
`hailstone.ini` turns the build at the zone-in off; the mesh is then built when first wanted: a `/nav`, the overlay or
the Navigation TLO's path questions. The tiles are built on worker threads (`nav.threads`, default 2, at most
8), nearest the player first, and a walk asked for meanwhile starts when its tiles are there. A walk to a spawn asks
the server for its path meanwhile, when there is no mesh under the player yet: the same `OP_FindPersonRequest` as
`/find`, but quiet (`GameConnection::request_quiet_path`: no path drawn, no sound, no chat line). A stock EQEmu server
answers from its own navigation mesh when it has one. The client keeps the requests in flight in order and gives
each reply to the oldest one waiting. EQEmu does not promise that order: it sends an empty reply (no target, no route
or only part of one) at once, but builds a full path on its thread pool and sends it when done, so a quiet request for
a long path and a `/find` that fails just after it can still swap (`docs/unknowns.md`). EQEmu leaves a find of a
trader unanswered (it moves the player to the trader instead), so a request still waiting after 10 s (our own number)
is taken as never answered. A walk that needs a new path while the mesh is unfinished waits
for its tiles (and asks the server again) instead of giving up. A `/nav` word of three letters or more that begins only
one subcommand stands for it (`/nav tar` is `/nav target`). Measured with 4 threads:
qeynos2 0.6 s (720 tiles), befallen 0.07 s, anguish 4.6 s (5440 tiles).

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

The builder fingerprint (`nav::builder_fingerprint`) is a hash of what this build of the client makes of a small fixed
scene with one of each thing the builder makes (`nav::fingerprint_scene`: ground and a step, a drop off a ledge, a
pool's floor joined to its surface, a flooded room dropped into and climbed out of, a zone line's box), and of the
links' numbers. It is worked out once, at the first zone load, in a few milliseconds. A file written by a builder
that makes anything else of that scene (its rules, Recast, the compiler) is built again on its own, with no version
to raise by hand; a test checks the scene still has each kind, so a new kind of link should be added to it.

The fields are little-endian (`core/byte_io.h`). The tile data is Detour's own in-memory layout, so a file is read
back only by a build with the same native layout. Reading Detour's structs straight out of those bytes (the tile
checks in `nav_mesh.cpp`) is the one accepted exception to the rule that wire and file data go through `ByteReader`:
Detour takes a tile only in that layout, and the file is this client's own cache, guarded by the native-layout word
and the checksum. A file whose hashes or grid do not match the zone's geometry and
the build's parameters, or whose checksum fails, is ignored and built again. Each tile is then checked before Detour
sees it (`NavMesh::add_tile`): its size against the counts in its header, every polygon's vertex, neighbour and
detail indices, a boundary edge in each polygon's detail mesh, coordinates finite and within the world, and the
bounding-volume tree's indices.

## Floors over floors

Recast's mesh has every level of a building, a hall and the balcony over it, so a goal's height matters:

- Where the goal stands is the polygon round it (4 across, 12 up or down) that is nearest with height counted four
  times as much as distance across, so the floor under a spawn wins over the one under that, or over it.
- When that polygon cannot be reached from the start, Detour's own fallback would stop at the polygon nearest in a
  straight line, which in a building is usually the floor right under the goal. Instead the polygons within 32 across
  and 64 up or down are tried, best first by the same measure, and the first one a walk can reach (the mesh's links
  flooded from the start) is the end: the near edge of the goal's own floor rather than the hall under it, when there
  is one.
- A corner, or the end, counts as reached within 2 across (the end: the walk's distance) and within 8 in height, so
  standing under a corner of a ramp that winds back over itself, or under the goal, is not being there. The server's
  path (`/find`'s) is at its own mesh's heights and is reached by distance across alone.
- A walk that still ends more than 8 over or under the goal says "Stopped 12 below ...: the mesh has no way up there"
  (a lift, a jump or a drop it does not know).

## Zone lines on the way

A walk never steps into a zone line by accident: when the step ahead (3 units on, a body's middle up) is in a zone
line's region, the walk stops with "Stopped at a zone line on the way to ...", unless zone lines are allowed
(`/nav option zonelines on`) or the goal itself is in one. Seen live: the border of Oasis of Marr and North Ro.

## Water

The mesh has the water surfaces (above) as well as the bottoms, marked Water; the search pays 3 for them, or never
crosses them with `/nav option water off`. Forward swims along the look pitch (the physics' push along the pitch),
so while swimming (BodyWet 5 and up) the walk sets the pitch each frame, towards the path's next corner
(`Navigator::swim_pitch`; /follow does the same towards its path or the spawn):

- the head under water and the air below 35% of the most: straight up, whatever the corner;
- a corner out of the water within 12: up, so forward rises and the original's swim jump (DoSwimJump: forward with
  the pitch up and the surface ahead clear) climbs out onto the shore;
- at the surface (the head out, or no water a little over the head: it cannot rise further) and the way ahead no
  deeper: level, so forward goes across instead of pushing up against the surface for nothing;
- otherwise towards the point 4 units on along the path at the path's height there (`PathFollower::ahead`), at
  most 63 degrees (80 when the next corner is straight over or under: a swim link): diving to a corner below,
  rising to one above. Aiming along the path, not straight at its next corner, keeps the swim at the path's height:
  a path along a pool's floor runs through openings only a little taller than a body, and a straight line at the
  corner from higher up runs into their tops (seen in Old Sebilis at the real frame rate).

These numbers are ours. Out of the water the pitch is left alone.

## /follow

A Hailstone change to the original's /follow (`game/follow.h`, FollowPlayerAI), on by default and switchable:
`hailstone.ini` `nav.follow=0`, or `/nav option follow off`. The original turns only while it has a clear line to the
followed spawn and runs straight at it, so a corner, a doorway or a wall loses it, and it gives up past 200.

- While the spawn is in a clear line that the mesh can walk straight, the original's step steers, exactly as it does.
- Otherwise (out of sight, or a drop or an obstacle on the straight line), the walk takes the mesh's path to where
  the spawn stands, re-planned as it moves (every 0.5 s once it has moved 3), with the walk's own unsticking. The
  original's bands still decide stopping, walking and running, measured along the path: stop inside 15 + both
  heights, walk inside 30 + both heights when the spawn is slower.
- The follow keeps on to 400 away (the original stops at 200, with its "too far" message, which still ends it past
  400 or when the spawn dies).
- The help's own strafes do not end the follow (the original's turn and strafe keys still do, when the player
  presses them). If the help gets stuck it hands back to the original's step for 5 s, then tries again.
- Off the mesh (levitating, swimming, a ledge it left out) the original's step steers alone. `/nav ui` draws the
  follow's path in cyan.
- A `/nav` walk and /follow do not run together: either one started ends the other.

## Getting unstuck

A walk that stops getting closer to its next corner for 1 s jumps and strafes for 0.6 s, one side and then the other.
Those times count only up to 0.25 s of any one frame: a hitch (a zone or a model loading) is not time the character
had to move in, so it is neither a stall nor part of a jump and strafe.
A strafe is held back (the walk only jumps) when the floor 4 to either side is more than 5 under the feet, so it
never steps off a walkway over a pit (seen in Lower Guk with a body wider than the mesh's agent).
After three of those it finds a new path from where it stands, or first clicks a closed door in the way. After three
new paths with no corner reached it stops with "Navigation stuck going to ...".

## Commands

`/nav` (MQ2Nav's set; the 'move' permission for plugins and scripts, `plugin/policy.h`):

| Command | |
|---|---|
| `/nav id <#>` / `target` / `spawn <search>` | to a spawn (MQ's search words, `docs/plugins.md`), following it as it moves; done within 5 |
| `/nav loc <y> <x> <z>` (`locyxz`) / `locxyz <x> <y> <z>` / `locyx <y> <x>` | to a point (`/loc` order, or x y z); done within 2 |
| `/nav door <name> [click]` / `door id <#> [click]` | to the nearest door whose name contains the text; `click` uses it on arrival |
| `distance=<n>` (`dist=<n>`) | anywhere on the line: stop this close, measured along the path (a goal behind a wall is not near through it), however many corners are left; in height the end may be as far off as the distance (at least 8) |
| `/nav stop` / `pause` | stop; pause or go on |
| `/nav status` | the mesh's tiles and the walk |
| `/nav ui` (`overlay`) | the mesh (green ground, blue water, red lava, purple zone lines, yellow PvP), its links (orange drops, light blue swims) and the path, over the 3D view |
| `/nav reload` (`rebuild`) | build the zone's mesh again, ignoring the cache |
| `/nav option water\|lava\|pvp\|zonelines\|doors\|passdoors\|follow on\|off` | swim (on), cross lava (off), enter PvP areas (on), walk through zone lines (off), open doors (on), walk through doors that are not shut (on), help /follow (on) |
| `/nav option clearance <0..6>` | how much further than the mesh's margin a path's bends keep from walls (2.5; 0: hug the margin) |

Not yet: `/nav item`, waypoints (`/nav waypoint`, `/nav recordwaypoint`).

`/moveto` (MQ2MoveUtils' straight walk, no mesh): `/moveto loc <y> <x> [z]` (or `y,x`), `/moveto id [#]` (the target
without a number), `/moveto mdist <n>` (where it stops, default 10), `/moveto off`.

`/stick` (MQ2MoveUtils' words; Hailstone's own walk, `NavControl::stick`): keeps to the target until `/stick off`, the
player's own movement keys, or the spawn dying or leaving. Further than the distance it walks: straight when the spot
is within 40, in plain sight and the straight line stays on the mesh until it is within that distance of the spot,
over the mesh otherwise (straight without a mesh, or off it), the path found again as the spawn moves. Sight at the
body's middle alone lets a wall's corner catch the body; the mesh keeps its margin from walls, and a mob against a
wall stands in that margin itself, hence "within that distance". Near enough it holds still and faces the spawn. `/stick [<distance>] [behind] [id <#>]`: the distance is flat,
by default three quarters of the player's melee range of the spawn (the original's `__get_melee_range`; the quarter
keeps the player in reach while the spawn shifts and its position updates lag: Hailstone's choice); `behind` stands
at its back, within 45 degrees. It keeps to the spawn it was given, whatever is targeted later (MQ2MoveUtils'
`hold`); `/stick` again moves it to the new target. A `/nav` or `/moveto` walk ends a stick, and a stick ends a walk.
Not yet: MQ2MoveUtils' `pin`, `!front`, `moveback`, `uw` and percent distances.

`${Stick}` (ON / OFF), `.Active`, `.Status` (ON, PAUSED, OFF), `.Distance`, `.StickTarget` (the spawn's id, 0 none),
`.StickTargetName`.

### The Navigation TLO

`${Navigation}` (TRUE while walking), `.Active`, `.Paused`, `.MeshLoaded`, `.PathExists[<dest>]`,
`.PathLength[<dest>]` (-1 without a path). Hailstone's own: `.MeshComplete`, `.TilesBuilt`, `.TilesTotal`,
`.Destination`, `.Remaining`. A destination is `id <#>`, `target`, `loc <y> <x> <z>` (`locyxz`), `locxyz <x> <y> <z>` or a spawn
search. Lua reads it as `mq.TLO.Navigation.Active()`; plugins with the SDK's `parse` and run `/nav` with
`do_command`.

## Find

An active server extension may disable NPC Find or own map spawn presentation (`ServerExtension::map_policy`,
`hs.UI.map.Policy()`). Both packs then retire cached NPC overlays and competing controls without writing settings.
Native discovery, targeting, guide, path and walk entry points enforce the same policy. Find-owned guides and walks
pin the original connection, player and NPC incarnations: denial, replacement or zoning retires them before another
destination read or movement. Replacing the walk with ordinary navigation removes that ownership; PC group/raid Find
and coordinate navigation retain their normal behavior.

Find replies carry no request identity, so each is given to the oldest request in flight, as above. Shown Find
paths and quiet navigation requests share an ownership ledger: each records its origin, connection/player context,
actor incarnation and goal generation. A reply with no request waiting, or one whose request was cancelled or
replaced (a new `/find`, a stopped walk, a lost actor), takes its place in the order and is dropped, so later
requests keep their own replies. Reserving a request for admission does not consume replies or start its timeout; a
request rejected before transport leaves an earlier valid request intact. Outgoing callbacks may receive an earlier
reply or issue a nested request before this reservation is sent. A request unanswered after 10 s is dropped, as
above, and later requests carry on.

Tracking keeps one `OP_Track` in flight at a time, since its replies carry no request id either. Any reply answers
it: a list (a short one is read as RoF2 reads it, zero past its end), an empty list or the stock timer reply. One
unanswered after 10 s (our own number) is dropped and the next refresh goes out; the list shown stays. A reply with
nothing asked, or to a request whose window was closed or whose server policy changed since, is not shown. Only a
server extension's tracking rule or map policy keeps Tracking closed (docs/plugins.md); the player's own death empties
the list, as it ends every Find and walk (docs/unknowns.md).

A Hailstone addition, in place of the original's Find window (FindLocationWnd, whose code is gone): a list of the
NPCs a new player looks for in the zone, from what every stock EQEmu server sends (`game/useful_npcs.h`,
`app/lua_ui_find.cpp`). The find key (`CMD_TOGGLE_FIND_WINDOW`), the EQ menu's Find and the Actions window's Find
open and close it as the original's key did its window: open, it closes; closed with a find path shown, the path ends;
else it opens. The modern pack draws it as the map's Find page (the map opens on it; closing the map closes it), the
gamepad pack as a window of its own (More > Find).

- By the spawn's class: merchants (41), bankers (40, 66), guildmasters (20..35), special merchants for points and
  other currencies (58, 59, 61, 67, 68, 70, 73) and other services (tribute 63, 64, 74; adventure 60; fellowship 69;
  mercenaries 71; real estate 72). Never players, corpses, pets or the LDoN objects of class 62.
- Quest givers: with Hailstone's server script (server/README.md "Quest givers in the Find window"), the NPCs whose
  quest file takes hand-ins or talks, asked once a zone when the list is first wanted, of a server whose zone has
  answered the settings request (`net/quest_npc_packets.h`)
  and matched to spawns by the name as shown. An NPC listed for its trade keeps its kind and gets a "!" beside it.
  Without the script there are none: the client does not guess at them.
- Points of interest: the other NPCs on the server's findable list (OP_SendFindableNPCs, the npc_types `findable`
  flag; docs/protocol/rof2-packets.md), which the original's window listed. Zoning clears the list.
- Each row: the name, the surname (else the class's name) and the distance; nearest first, a search on the name and
  surname, and a filter per kind (`find.hidden` in the pack's settings). A click targets the NPC and centres the map
  on it; Show path (or a double click) starts the guide path to it (below); Walk there (the gamepad's X) walks the
  player there as `/nav id` does, to within 10, with the guide path showing (the player's movement keys stop the walk,
  the guide path stays). The map's NPCs button (`map.npcMarks`, off
  at first) marks the listed NPCs on it and on the minimap, by the same kinds and search (`ui/modern/lib/find.lua`);
  the selected and hovered ones are marked always.
- Positions are where the client has the spawn: EQEmu sends far spawns' moves less often, so a walking NPC far off
  may be shown a little behind.

### The guide path

`app/guide_path.h`: a trail from the player to the NPC over the client's own mesh (`Navigator::plan_between`, the
walk's options), so it goes round walls on every zone; the server's find path is a straight line where the server
has no mesh of its own. It is found again when the player is more than 8 (flat) off it or the NPC has moved 6 since,
at most every half second, and trimmed behind the player. It ends within 12 of the NPC (and 10 in height) with "You
have reached ...", when the NPC dies or goes, when there is no way there, on zoning, with End path / End Find, or the
find key with the Find window closed. While the mesh is not ready it says once that the path will show when it is.
A partial path (the NPC off the mesh) is drawn as far as it goes. The maps draw it in gold beside the find path.

### The trail

Options > Display > **Find Trail** (gamepad: Options > Interface) chooses the appearance of both the guide path
and the server's find path (PlayerPath). The choice applies immediately and is saved per character in
`hailstone.ini` as `display.find_trail_style`: **Gold line (default)** (0), or **Rainbow trail** (1).
Unknown values select the default. This is a rendering choice over the existing routes; it does not start a walk.
The world trail is available on the first zone entry, including servers without downloaded assets. Its terrain
source follows zone loads and reloads rather than keeping the previous zone's surface.

`game/path_trail.h`: the default is a narrow gold ribbon with a dark border, in the style of MQ2Nav's route line.
It follows the local ground plane and is depth-tested against the scene. It reuses Hailstone's navigation routes
and ground sampling; the diagnostic mesh overlay is separate (`/nav ui`).

The rainbow choice draws fourteen thin, continuous strands, two in each of seven colours. Each winds sideways
and curls vertically with its own phase, hovering about 3 world units above the terrain (a 0.7-unit vertical
wave). Solid, untextured line segments face the camera across their narrow width, so the colours read as separate
threads instead of diffuse particle puffs. Bright bands move towards the destination at 9 units a second. The
strands fade in over the first 5 units and out at the far end (the guide path's 120..170; the find path's at
Path.ini's FadeEnd). The original draws the find path as a textured tube; this look is Hailstone's.

The ground line is lifted 0.35 units; the rainbow's hover height follows the same terrain profile. Ground probes
start at the player and advance in small steps from the last supported height, so a hill or valley can rise or fall more than
6 units away from the line between route points. Each strand is grounded separately across the trail's width.
Nearby steps are followed; missing ground or a sharp drop leaves a gap instead of bridging unsupported space or
snapping to a distant floor below a bridge. Route vertices can anchor the next supported stretch. Tunnel roofs
are not selected by casting down from high above the zone. Without loaded collision there is no trail.

This changes the trail's presentation, not the route: the guide uses the client's navigation mesh, while a server
without its own mesh can still send a straight route. Ground projection does not find a way around a wall.
The selected style replaces the find path's travelling Path.ini particle clouds, which followed the old elevated line.
Its completion emitters play only at a supported destination, grounded as the trail is. The route, arrival checks,
sounds and map colours stay as before.

Not yet: the gamepad pack's minimap marks, and a key of the client's own for a walk to the target (`/nav target` on a hot button does it) (docs/todo.md).

## Zone atlas

Code: `game/zone_routes.h` (the links and routes), `game/atlas.h` (`index_atlas`), `net/zone_link_packets.h`, the
`hs.Atlas*` / `hs.Zone*` functions in `app/lua_ui_map.cpp`, and the modern pack's `ui/modern/lib/atlas.lua` (drawn
by `map.lua`). Tests: `tests/zone_routes_test.cpp`, the index cases in `tests/atlas_test.cpp`.

The map window's **World** view, a Hailstone addition. RoF2 had an atlas page in the map window (the game folder's
`Atlas\<skin>\*.xml`, kept as the stock window's MVW_AtlasRenderArea, `hs.DrawAtlas`) and a zone guide fed by the
server, which EQEmu does not send (`docs/unknowns.md`). The World view draws the same atlas pages, with:

- **The atlas.** Every page reachable from `Default.xml`, zoomed with the wheel (up to 5x) and panned by dragging,
  with a breadcrumb back to the world. A region click opens its page; a zone click selects it; a double click shows
  its map in the Zone view, with a bar back to your own (the player, the find path and the height fade are not drawn
  on another zone's map, and closing the window goes back to your zone). Your zone pulses, the selected zone is
  outlined, and the zones of the route followed are numbered and joined.
- **The zone finder.** Every zone the atlas links that the server has (below), by the zone table's long name or the atlas' tooltip and its short
  name (prefix and word matches first), or the zones of the page shown when nothing is typed; each with its region
  and expansion. The selected zone's card shows how many zones away it is and the way there, with **On atlas**,
  **Its map** and **Go here**.
- **A route followed** (`ui/settings/modern.txt` `atlas.route`): a banner over your zone's map with the next zone
  and **Walk there**, which runs `/nav locxyz` to the nearest part of the zone line to that zone (2 units inside it,
  so the walk goes through rather than stopping at it; "Zone lines on the way" above). It needs the 'move'
  permission like any `/nav`. Your zone's exits are marked on its map (the next one stands out); the map's right-click
  menu hides them.

**Where routes come from.** How zones connect is kept per server in `servers/<profile>/zone_links.txt`
(`docs/layout.md`), from two sources:

- the server's list, when it runs Hailstone's server script (`server/README.md` "Zone links"): every zone point the
  server's expansion and content flags allow. The client asks (OP_HsZoneLinksRequest 0x1438) once a session, the
  first time the World view is open after a zone has answered the settings request, so a server without the script
  is never asked;
- learned: the targets of each zone's OP_SendZonepoints, as the player enters it.

**Only the server's zones.** The finder lists, and a route passes through, only the zones the server has; the
atlas dims the others ("Not on this server"). With the script's list, those are the zones its current expansion
(`Expansion:CurrentExpansion`) and content flags allow, sent with the links. Without it, the account's expansions
are used (the profile's or OP_ExpansionInfo's bits, expansion n at bit n − 1, Classic always): EQEmu fills those
from `World:ExpansionSettings` or the client version rather than `Expansion:CurrentExpansion`, so a server that
limits only its content shows every zone until it runs the script. The finder's footer says which applies.

A route has the fewest zone changes (the lowest zone ids first among equals). When the links in their own direction
give none, a learned link may be used backwards (a zone line seen only from its far side); most zone lines go both
ways, but not all, so the card says so. The exits on the map come from the zone's zone-line regions and the zone
points they name (`game::collect_zone_exits`, `game/zone_exits.h`), worked out once the zone points are
in; a zone point reached some other way (a teleporter NPC, a boat, a door) has no line, so **Walk there** is off for
it.

## Not done

- Swimming is not yet seen live end to end (unit tests only; the live try was cut short by an attack). Levitation
  and boats are not handled.
- Lifts, and jumps up onto ledges, as links between parts of the mesh; walks across zones (`/travel`); tactical queries (a spot
  in range with line of sight, behind a mob); a mesh editor and waypoints; several characters in one process.
