# Isometric camera

Code: `src/game/iso_camera.{h,cpp}` (the camera and movement relative to it), `src/game/click_intent.{h,cpp}` (what a
click does), `src/game/iso_occlusion.{h,cpp}` and `reveal_cut` in `shaders/lighting.glsl` (seeing the player through
what is in front), `src/app/client_app_iso.cpp` (the clicks' walks and the keys), `ClientApp::update_iso_camera` and
`ClientApp::camera_command` (`src/app/client_app_input.cpp`), `Navigator::walk_to_click` (`src/app/navigator.cpp`).
Tests: `tests/iso_camera_test.cpp`, `tests/click_intent_test.cpp`, `tests/iso_occlusion_test.cpp`.

The original client has no such camera. This is a Hailstone addition, off unless the player chooses it: a view from
above and to one side, in the manner of isometric RPGs. It changes only where the scene is seen from. Movement,
facing, speed, collision and every game rule stay the original's, and the server sees ordinary movement.

## The camera

Camera type 7 (`game::kCameraIsometric`), Hailstone's own beside the camera behind the player (6). It looks down at a
fixed pitch (45 degrees by default) from a distance (160 units at the start), through a perspective lens (63 degrees
across by default, about the stock cameras' 63.3). It follows the player's body with a little easing (0.08 s) and
jumps with a teleport or a zone-in. It does not stop at scenery: what stands between it and the player is cut away
instead ("Seeing the player" below).

- The right button held turns the camera freely about the player, never the player. A short right click still
  clicks.
- The left button always clicks; it never starts a mouse look.
- The wheel zooms, a tenth of the distance a notch, from 60 to 320 units.
- TOGGLECAM's cycle counts it as first person (0 -> 1), as it counts camera 6; the first person camera's key leaves
  it.

To the movement controls it is the camera behind the player (6) with no mouse look, so neither button runs.

## Movement keys

With `camera.iso_wasd = camera` (the default) the movement keys point relative to the camera, as in isometric RPGs:
forward runs up the screen, back down it toward the camera, the strafe keys left and right across it, and two
together the diagonal between. The player turns at once to that way (as /follow and /nav set the heading) and the
controls see only the forward key held, so the speed, the run and walk, collision and the position updates are the
original's. The turn keys turn the camera (2 radians a second), not the player. Autorun runs the way the player
faces. At a boat's helm the keys sail the boat as in the other cameras.

The gamepad's left stick moves along its own angle, not eight ways; the right stick turns the camera (sideways) and
zooms (up and down).

With `camera.iso_wasd = classic` (`/camera wasd classic`) the keys move, turn and strafe as in the other cameras.

## Clicks

A click either does what it does in the other cameras or walks first, over the navigation mesh
(`docs/navigation.md`), and then does that. It never targets, attacks or casts on its own: auto attack is turned on
with its key or `/attack`, as everywhere (`game::iso_click_action`).

| Click on | Does |
|---|---|
| the ground, left | walks there; held, the walk follows the cursor |
| a door, a ground item or a ladder out of reach, left | walks to it, then clicks it |
| an NPC or a corpse out of reach, right | walks to it (inside the melee range), turns to it, then right clicks it: a merchant's or banker's window, a corpse's loot; a hostile NPC is only walked to |
| any spawn, left | targets it, near or far, as in the other cameras |
| a player, right | their menu, where they stand |
| anything, with an item on the cursor or while placing one | the other cameras' click (drop, place, a key on a door) |

The walk is the navigator's, so it goes round walls, opens doors on the way and gets itself unstuck. Any movement key
ends it, without a word in the chat (each click would say so). A walk that cannot be started says why ("Cannot walk
there: ..."). A click that walks to the spot says nothing when the mesh goes only part of the way, and goes straight
at the spot in a zone without a mesh. The click at the end is made only when the walk ended near enough; a walk
stopped short (a key, no path, stuck) clicks nothing.

### Click to move in every camera

The same clicks work in the other cameras when `mouse.click_move` is set (the `click_move` option, off by default:
a Hailstone addition, so the other cameras click as the original does until it is switched on;
`ClientApp::click_moves`). The table above holds as it is, the click marker included. What stays the other cameras'
own: the movement keys (never relative to the camera there), the mouse look, and no see-through cut or hover ring. In
first person and the camera behind the player, the walk turns the player and so the view, as `/nav`'s does. A
left-button mouse look (MouseTurnZoom in the camera behind) clicks only at a short release, as it always has, and the
button held during the look does not steer the walk.

## Measured from the player

The camera stands well back from the player, so what the other cameras measure from the camera is measured from the
player here:

- **Fog and far plane** (`game::iso_view`): the fog's start and end and the far plane are pushed out by the camera's
  distance, so the fog starts at the player where it would in first person and as much of the zone shows round them.
- **The region** the fog and the underwater tint read is the player's eye's, not the air's or the hillside's the
  camera hangs in.
- **Shadow cascades** (`game::iso_shadow_near`) start 40 units short of the player rather than at the lens, and reach
  that much further, so the sharpest cascade covers the player.
- **The sound listener** is the player's in every camera already.

A click that walks to a spot or an item puts a gold ring on the ground there (drawn as the target ring is, a decal on
the scene's depth): it lands wide and closes in with a flash, spins slowly while the walk goes on, follows the held
button's aim, and fades when the walk ends. A walk to a spawn or a door shows none (the target ring, or the door
itself, is there).

## Seeing the player

What stands between the camera and the player is cut away round them, so they stay in sight behind walls, trees,
hills, roofs and the floors above (`game::iso_reveal`, `shaders/lighting.glsl` `reveal_discard`). The cut has two
shapes, each eased in over 0.2 s and out over 0.5 s (`game::iso_ease`, `ClientApp::update_iso_reveal`).

**The circle** comes while the line from the camera to the player's feet, middle or eyes is blocked by the zone or
its objects (the camera's own sweep, radius 0.25), so a tree beside the player that hides nothing is left whole
(`camera.iso_reveal_always` cuts all the time). A surface is cut where it is

- nearer the camera than the player's middle by at least 3 units, fully from 9,
- inside a cone round the line from the camera to the player, which shows as a circle on the screen: 20 units in
  radius at the player at the camera's starting distance (`camera.iso_reveal`), scaled with the zoom so it keeps its
  size on the screen, its outer 60 % thinning out, and
- higher than 1 unit above the player's feet, fully from 4.

So the floor the player stands on and the ground falling away in front stay, and a wall, a slope rising in front,
a roof or an upper floor goes.

**The roof** comes while something solid is within 60 units over the player's eyes (a ceiling, a roof, a bridge,
an overhang; `camera.iso_roof`). Then everything above a height just under that cover (2 units under where the cover
test hit, never lower than 1.5 over the eyes; `game::iso_roof_height`) goes round the player, in front or behind,
out to 60 units or 1.25 times the camera's distance, the larger (flat; the outer quarter, at least 15, thinning out),
so it clears what the view shows. A hall keeps its walls up to its ceiling and loses the ceiling; a low tunnel loses
the whole hill over it; a house loses its roof. The height follows a changing cover (an arch, a sloping roof, a
staircase) over a quarter second.

The zone, its objects (doors, placed objects, ground items) and the grass are cut; the characters are not. The cut
drops pixels against a 4x4 ordered dither rather than blending, so the passes stay opaque and nothing has to be
sorted. The shadow passes do not cut: what is cut still casts its shadow.

A click goes through the cut as well: a world triangle the pick's ray meets where the cut takes at least half is
skipped (`ClientApp::pick_world`, `game::iso_pick_skips`, the shader's test on the CPU), so a click on a spawn, a door
or the ground behind a cut wall reaches it.

**The silhouette.** Where the player is still hidden (by the edge of the cut, a character, an object the cut leaves) a
pale blue outline of them, stronger toward their edge, shows through (`camera.iso_silhouette`;
`ActorRenderer::draw_silhouette`, `shaders/actor_silhouette.frag`). It is drawn after the zone and its objects and
before the characters, where the player's body lies behind what is drawn (a depth test, no depth write); the player
drawn after covers it wherever they are in sight, so their own arms and legs never outline themselves. Their mount's
outline shows with theirs.

## Under the cursor

A pale ring lies under the spawn, door or ground item the mouse is over (`camera.iso_hover`; not the target, which
has its own ring, nor the player), so it is clear what a click will do. The pick is made ten times a second, or when
the mouse moves, without logging (`ClientApp::update_iso_hover`); it goes through the cut as the clicks do. A door's
ring lies on the floor under it.

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

There is no key command of its own: the key table is the original's (`game/keymap_tables.inc`). A hot button or a
social with `/camera toggle` gives the camera a key. The gamepad has a pad action of its own, `camera_iso`
(`docs/gamepad.md`).

## Options

Options > Display > **Isometric Camera** (the modern pack) and the gamepad pack's **Camera** page set the same things,
through `eq.Option` / `eq.SetOption`:

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
