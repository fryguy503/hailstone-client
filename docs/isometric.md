# Isometric camera

A view from above and to one side, as in isometric RPGs: a Hailstone addition (the original has no such camera), off
by default. Only the viewpoint changes: movement, facing, speed, collision and every game rule stay the original's,
and the server sees ordinary movement.

Code:

- `src/game/iso_camera.{h,cpp}`: the camera, and movement relative to it.
- `src/game/click_intent.{h,cpp}`: what a click does.
- `src/game/iso_occlusion.{h,cpp}` and `reveal_cut` in `shaders/lighting.glsl`: seeing the player through what is in
  front.
- `src/app/client_app_iso.cpp`: the clicks' walks and the keys.
- `ClientApp::update_iso_camera` and `ClientApp::camera_command` (`src/app/client_app_input.cpp`),
  `Navigator::walk_to_click` (`src/app/navigator.cpp`).

Tests: `tests/iso_camera_test.cpp`, `tests/click_intent_test.cpp`, `tests/iso_occlusion_test.cpp`.

## The camera

Camera type 7 (`game::kCameraIsometric`), Hailstone's own beside the camera behind the player (6).

- It looks down at a fixed pitch (45 degrees by default) from 160 units at the start.
- Its perspective lens is 63 degrees across by default (the stock cameras' is about 63.3).
- It follows the player with 0.08 s easing and jumps on a teleport or zone-in.
- It does not stop at scenery: what stands between it and the player is cut away ("Seeing the player").

- The right button held turns the camera about the player, never the player; a short right click still clicks.
- The left button always clicks, never starts a mouse look.
- The wheel zooms a tenth of the distance a notch, 60 to 320 units.
- TOGGLECAM's cycle counts it, as it counts camera 6, as first person (0), so the next camera is 1; the first person
  key leaves it.

To the movement controls it is camera 6 with no mouse look, so neither button runs.

## Movement keys

With `camera.iso_wasd = camera` (the default) the movement keys point relative to the camera: forward up the screen,
back toward the camera, strafe across it, two together on the diagonal.

The player turns at once that way, as /follow and /nav set the heading, and the controls see only forward held, so
speed, run and walk, collision and position updates are the original's.

- The turn keys turn the camera (2 radians a second), not the player.
- Autorun runs the way the player faces.
- At a boat's helm the keys sail the boat as in the other cameras.

The gamepad's left stick moves along its own angle, not eight ways; the right stick turns the camera (sideways) and
zooms (up/down).

With `camera.iso_wasd = classic` (`/camera wasd classic`) the keys move, turn and strafe as in the other cameras.

## Clicks

A click does what it does in the other cameras, or walks first over the navigation mesh (`docs/navigation.md`) and
then does it. It never targets, attacks or casts on its own: auto attack needs its key or `/attack`
(`game::iso_click_action`).

| Click on | Does |
|---|---|
| the ground, left | walks there; held, the walk follows the cursor |
| a door, a ground item or a ladder out of reach, left | walks to it, then clicks it |
| an NPC or a corpse out of reach, right | walks to it (inside the melee range), turns to it, then right clicks it: a merchant's or banker's window, a corpse's loot; a hostile NPC is only walked to |
| any spawn, left | targets it, near or far, as in the other cameras |
| a player, right | their menu, where they stand |
| anything, with an item on the cursor or while placing one | the other cameras' click (drop, place, a key on a door) |

The walk is the navigator's: it goes round walls, opens doors and gets itself unstuck.

- Any movement key ends it silently.
- A walk that cannot start says why ("Cannot walk there: ...").
- A walk to a spot says nothing when the mesh goes only part of the way; without a mesh it goes straight there.
- The final click is made only when the walk ended near enough; a walk stopped short (a key, no path, stuck) clicks
  nothing.

### Click to move in every camera

With `mouse.click_move` set (the `click_move` option; `ClientApp::click_moves`) the same clicks, marker included,
work in the other cameras. It is a Hailstone addition, off by default, so otherwise they click as the original does.
Those cameras keep their own movement keys (never camera-relative), mouse look, and no cut or hover ring.

In first person and the camera behind, the walk turns the player and so the view, as `/nav`'s does. A left-button
mouse look (MouseTurnZoom) clicks only on a short release and does not steer the walk.

## Measured from the player

What the other cameras measure from the camera is measured from the player here:

- **Fog and far plane** (`game::iso_view`): pushed out by the camera's distance, so fog starts where it would in
  first person.
- **The region** for fog and underwater tint is the player's eye's, not the camera's.
- **Shadow cascades** (`game::iso_shadow_near`) start 40 units short of the player, not at the lens, and reach that
  much further, so the sharpest one covers the player.
- **The sound listener** is the player's in every camera already.

#### The click marker

A walk to a spot or an item puts a gold ring there, a decal like the target ring. It closes in with a flash, spins
during the walk, follows the held button's aim and fades when the walk ends. A walk to a spawn or a door shows none.

## Seeing the player

What stands between the camera and the player is cut away round them (walls, trees, hills, roofs, upper floors) (`game::iso_reveal`, `shaders/lighting.glsl` `reveal_discard`). The cut has two
shapes, each eased in over 0.2 s and out over 0.5 s (`game::iso_ease`, `ClientApp::update_iso_reveal`).

**The circle** comes while the line from the camera to the player's feet, middle or eyes is blocked by the zone or
its objects (the camera's sweep, radius 0.25), so a tree that hides nothing stays whole. `camera.iso_reveal_always`
cuts all the time. A surface is cut where it is

- nearer the camera than the player's middle by at least 3 units, fully from 9,
- inside a cone round the camera-player line (a circle on screen): 20 units radius at the player at the starting
  distance (`camera.iso_reveal`), scaled with zoom to keep its screen size, its outer 60 % thinning out, and
- higher than 1 unit above the player's feet, fully from 4.

So the floor and ground falling away in front stay; a wall, a rising slope, a roof or an upper floor goes.

**The roof** comes while something solid is within 60 units over the player's eyes (`camera.iso_roof`). Then
everything above a height just under that cover goes, all round the player:

- The height is 2 units under where the cover test hit, never lower than 1.5 over the eyes (`game::iso_roof_height`).
- The reach is the larger of 60 units and 1.25 times the camera's distance; flat, its outer quarter (at least 15)
  thinning out.
- The height follows a changing cover (an arch, a staircase) over a quarter second.

A hall loses its ceiling but keeps its walls; a low tunnel loses the hill over it.

The zone, its objects (doors, placed objects, ground items) and grass are cut; characters are not. The cut drops
pixels against a 4x4 ordered dither instead of blending, so passes stay opaque and unsorted. Shadow passes do not
cut: what is cut still casts its shadow.

Clicks go through the cut: a world triangle the pick ray meets where the cut takes at least half is skipped
(`ClientApp::pick_world`, `game::iso_pick_skips`, the shader's test on the CPU).

**The silhouette.** Where the player is still hidden (the cut's edge, a character, an uncut object) a pale blue
outline shows through, stronger toward the edge (`camera.iso_silhouette`; `ActorRenderer::draw_silhouette`,
`shaders/actor_silhouette.frag`). It is drawn after the zone and before the characters (depth test, no depth write),
so the player, drawn after, covers it where visible and never outlines their own limbs. A mount is outlined too.

## Under the cursor

A pale ring lies under the spawn, door or ground item the mouse is over (`camera.iso_hover`); not under the target
or the player. The pick runs ten times a second or on mouse movement, through the cut like clicks
(`ClientApp::update_iso_hover`).

## /camera

| Command | Effect |
|---|---|
| `/camera` | which camera, and the isometric camera's settings; in it, what the cut is doing now (the circle's and the roof's strength, the roof's height and reach) |
| `/camera iso` | the isometric camera |
| `/camera first`, `/camera behind`, `/camera toggle` | first person, the camera behind the player, or between those and the isometric camera |
| `/camera pitch <degrees>` | the look down, 20..89 |
| `/camera fov <degrees>` | the field of view across, 10..90 |
| `/camera distance <units>` | the distance, 60..320 |
| `/camera wasd camera` or `classic` | the movement keys relative to the camera, or as in the other cameras |
| `/camera reveal <units>` | the cut round the player: its radius at the starting distance, 0..60 (0 off) |
| `/camera roof`, `silhouette`, `marker`, `hover`, `always` + `on` or `off` | the roof cut, the silhouette, the click marker, the hover ring, the circle even when nothing is in the way |

A number past a range is clamped; `nan` and `inf` are rejected (the usage line), and read as the default from
`hailstone.ini`.

There is no key command: the key table is the original's (`game/keymap_tables.inc`). Put `/camera toggle` on a hot
button or social for a key. The gamepad has the pad action `camera_iso` (`docs/gamepad.md`).

## Options

Options > Display > **Isometric Camera** (modern pack) and the gamepad pack's **Camera** page set these through
`hs.Option` / `hs.SetOption`:

| Option | Values |
|---|---|
| `iso_camera` | 0, 1: the isometric camera off (the camera behind the player) or on |
| `iso_pitch` | 20..89 degrees |
| `iso_fov` | 10..90 degrees |
| `iso_wasd` | 0 relative to the camera, 1 classic |
| `iso_reveal` | 0..60: the circle's radius (0 off) |
| `iso_reveal_always`, `iso_roof`, `iso_silhouette`, `iso_marker`, `iso_hover` | 0, 1 |
| `click_move` | 0, 1: click to move in the other cameras too |

## Settings

`hailstone.ini` (the character's), written by `/camera` and the options:

| Key | Default | |
|---|---|---|
| `camera.iso` | 0 | start in the isometric camera |
| `camera.iso_pitch` | 45 | degrees below the horizon |
| `camera.iso_fov` | 63 | degrees across |
| `camera.iso_distance` | 160 | units from the player |
| `camera.iso_wasd` | camera | `camera`: the movement keys relative to the camera; `classic`: as in the other cameras |
| `camera.iso_reveal` | 20 | the cut round the player: its radius at the player at the starting distance (160), 0 off |
| `camera.iso_reveal_always` | 0 | the circle even when nothing is in the way |
| `camera.iso_roof` | 1 | the roof cut under cover |
| `camera.iso_silhouette` | 1 | the player's silhouette where they are hidden |
| `camera.iso_marker` | 1 | the click marker |
| `camera.iso_hover` | 1 | the ring under the cursor |
| `mouse.click_move` | 0 | click to move in every camera, not only the isometric one |

## Still to do

- Whole objects (a tree, a pillar) fading as one where they hide the player, rather than cut in the circle.
- Tuning against real zones: the roof's reach, radius and height, the circle's size, the silhouette's colour.
