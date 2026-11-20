# The classic sky (sky.s3d)

Code: `src/assets/sky.{h,cpp}` (loading, tables), `src/render/sky_renderer.{h,cpp}` and `shaders/sky.{vert,frag}` (drawing), `src/game/sky_clock.h` (game clock).

**RoF2 does not load sky.s3d at all.** It draws a procedural sky from `Resources/Sky/sky.ini` and `weather.ini`
(dome, rings, cloud patches, sun/moon satellites, stars, colour maps). Its geometry is not decoded, so this client
keeps the classic `sky.s3d` layers and adds the procedural sky's moving cloud patches. Its colour maps are read
(`src/game/sky_colours.{h,cpp}`), but only rain and snow take their colours from them.

## sky.s3d / sky.wld

`sky.wld` (old format, 324 fragments) has no actor definitions (0x14) and no placements. Every layer is found by name:

| Name | What |
|---|---|
| `LAYER<N>1_DMSPRITEDEF` (0x36) | the sky dome of sky N |
| `LAYER<N>2_HS_DEF` (0x10) | a skeleton carrying the sun and moon meshes; only N = 1, 3, 6 |
| `LAYER<N>3_DMSPRITEDEF` (0x36) | the cloud layer of sky N |

N runs 1..17; past 10 the names run on as `LAYER11_DMSPRITEDEF`, `LAYER12_HS_DEF`, `LAYER13_DMSPRITEDEF`, ….

On the wire, **OP_NewZone's `sky` byte N selects LAYER\<N\>x**; sky 0 means no sky. Sky 1 (South Qeynos, time type 2)
is the normal blue sky.

| N | Dome texture | Cloud texture | Notes |
|---|---|---|---|
| 1 | NORMALSKY | NORMALCLOUD | sun and moon (MOON, SUN) |
| 2 | DESERTSKY | DESERTCLOUD | |
| 3 | (AIRSKY material) | (AIRCLOUD) | five moons (MOON31..35) |
| 4 | COTTONYSKY | FLUFFYCLOUD | |
| 5 | REDCLOUD | REDCLOUD | fear-style red sky |
| 6..9 | LUCLINSKY3 + LUCLINSKYFOG | LUCLINCLOUD1/4/5/2 | Luclin; 6 has EARTHRISE and SUN62 |
| 10, 15 | THEGREYSKY + THEGREYSFOG | THEGREYCLOUDS | The Grey (black with purple clouds) |
| 11 | POFIRESKY2 | POFIRESKY2 | Plane of Fire |
| 12, 13 | POSTORMSKY*/BOTSKY* | … | Storms, Bastion of Thunder |
| 14 | POWARSKY1 | POWARCLOUDS1 | Plane of War |
| 16 | POTRANQSKY1 | … | Tranquility |
| 17 | POAIRPOP171 | POAIRPOP173 | Plane of Air |

The meshes are unit-sized shallow domes centred on the camera (sky 1's dome spans x ±0.67, z −0.275..0.086; skies 6+
reach z 0.17), drawn with the camera matrix scaled by 5000. The part below z = 0 is normally under the ground.

Render methods: the dome and cloud materials use 0x8000000d ("diffuse skydome") and 0x8000000f ("transparent
skydome"), inconsistently (sky 1's dome is 0xf and its clouds 0xd; skies 2 and 4 the other way round). The sun and
moon use 0x80000010 (additive).

## Drawing

Order: the dome, the sun/moon skeleton, then the cloud layer only with the "double layer" sky option ("3D SKY OFF",
"SINGLE LAYER", "DOUBLE LAYER").

This client draws them around the camera (view rotation only) with depth test equal, depth writes off and
`gl_Position.z = 0` (the clear depth in reverse Z), no culling and no fog. In modern lighting the stack follows opaque
zone geometry where its blend order permits; classic lighting and zones with WLD detail blends keep it first (see
[Graphics: Sky](../graphics.md#sky)).

Materials follow the legacy render-method table: 0x8000000d opaque, 0x8000000f translucent at alpha 0x80 (so sky 1's
clouds cover its dome), 0x80000010 additive; colour bytes convert with x 1/255.

The sky is drawn whenever OP_NewZone's sky byte is set, whatever the time type. RoF2's sky layers have no UV scroll.

## Cloud patches

Code: `src/assets/sky_weather.{h,cpp}` (ini files), `src/game/cloud_patches.{h,cpp}` (motion), `SkyRenderer::draw_clouds`.

**Which clouds.** `sky.ini [SkySetting-<zone>] DefaultWeather` (else `[SkySetting-default]`, `DefaultClear`) names a
`weather.ini [WeatherPattern-<name>]`. Its `Cloud0`, `Cloud1`, ... are `[Cloud-<name>]` sections: `Texture`
(`Resources/Sky/Cloud-<Texture>.dds`), `DesiredCount`, `SpawnIntervalMin/Max`, `Size*`, `Offset*`, `Velocity*`,
`Spin*`, `Rotation*`, `Duration*`, `Elevation`, `Radius`, `SubdivisionX/Y`, `VisibleAngle`, `ColorSet`.

**Time.** `dt_ms` = the tick-count delta x TimeScale (1.0; the viewer's `--sky-timescale S` sets it).

**Motion.** `angle += Velocity dt` (not wrapped), `rotation = fmod(rotation + Spin dt, 2 pi)`; the patch is turned by
-angle about +Y. Velocity and Spin are radians per ms (`VelocityMax=0.000005` is an orbit in about 21 minutes).

**Spawning.**

- Each value is `rand()/32767 x (max - min) + min`, drawn separately.
- A fill puts patch i of n at `i (2 VisibleAngle / n) + (pi/2 - VisibleAngle)`.
- Fades are (in, hold, out) = (5000 ms or 0, Duration x 1000 ms, 5000 ms). A cloud without a Duration never times out.
- The angle a later spawn starts at is not traced (here the start of the visible arc).

**Approximated** (not traced):

- the patch geometry: here a SubdivisionX x SubdivisionY grid on the unit sky sphere, centred on the zenith turned by
  -angle about +Y, Offset radians off the orbit, Size half extents in radians, Rotation spinning it;
- the use of Elevation and Radius;
- the colour sets (here white x the outdoor light);
- the random sequence (MSVC `rand()` seeded with 1).

## Colour by time of day

Each sky layer vertex gets a colour from a fixed table of **60 sky levels × 18 entries**. Entries are 4 bytes B, G, R,
0. Level 14 is full day (entry 0 = RGB 9f ef f7, a light blue), level 44 full night (00 00 27).

A vertex's entry is fixed at load time from its height, `d = (int)(z × 16 × 8)`: `d < 0` → entry 16; `d < 16` → entry
`16 − d`; else entry 0. So the horizon uses entry 16; entry 17 (grey) is unused.

Each frame the entries of the current and next level are mixed by the level's fraction and converted through a float
table (assumed /255); the texture is multiplied by that colour.

### The sky level

With the sky time hour:minute (the world time from OP_TimeOfDay, hours 1..24 as the server sends them, or a forced hour for some zones), q = (60 × hour + minute) / 12:

| q | Hours | Level |
|---|---|---|
| 15..44 | 3:00–8:59 | (q + 29) mod 60: 44 → 59 → 0 → 13 (dawn) |
| 45..74 | 9:00–14:59 | 14 (day) |
| 75..104 | 15:00–20:59 | q − 61: 14 → 43 (dusk) |
| otherwise | 21:00–2:59 | 44 (night) |

The level steps every 36 real seconds (= 12 game minutes); this client uses the continuous value.

#### The game clock

The sky time advances by elapsed ms / 3000 minutes since the last time change: **one game minute per 3 real
seconds**. `game::SkyClock` runs on from the last OP_TimeOfDay.

The calendar clock that `/time` and the `GameTime` TLO show runs at the same rate (`net::advance_time_of_day`,
`GameConnection::current_time_of_day()`):

- It adds 20 × the whole real seconds since the packet (a 32-bit product) to the date with carries: hours 1..24, days
  1..28, months 1..12, a year past 9999 cleared.
- `/time` prints `%#x - %#I %p` with the weekday (day − 1) % 7 from Sunday, so every game month starts on a Sunday.

Checked: `EQWorldData::AdvanceTime` @0x7dbff0, `EQWorldData::CurrentGameTime` @0x7dc130.

### Day-cycle heading

The same function sweeps a heading (512 per turn) by 128 over each 6-hour block: 448 at 9:00, 64 at 15:00, 192 at
21:00, 320 at 3:00. That is a full turn a day with heading 0 at noon (`assets::sky_sweep_heading`).

In RoF2 it feeds the new-style sky. This client uses it to turn the classic sun/moon skeleton (whose rest pose has the
moon overhead) about the north-south axis; that use is an assumption.

### Light

- Light level per sky level: 1.0 at level 13–14, 0 at 44. Without a new-style sky it is the light object's level
  (three times) in zone types 1, 2 and 5.
- Outdoor ambient R, G, B per hour (entry hour − 1, interpolated by minute), clamped to 1 with a floor of 0.08 (higher
  with infravision/ultravision). Indoor type 4 uses 0.5, others 0.1.

Zone geometry is lit with the region shader's formula (`game/scene_light.h`): these ambient values, and the sun or
moon along the day-cycle heading (`(0, sin(512 − h), −cos(512 − h))`, the moon half a turn on at night), coloured by
the light level. Character models and doors are still multiplied by `0.3 + 0.7 × light level`
(`render::outdoor_brightness`) for outdoor time types.

### Ambient and fog per frame

Every frame (`game/fog.h`, `game/scene_fog.h`):

**Ambient A.** Time types 1, 2 and 5: the sky's ambient, or without one the hour tables above at the sky clock's hour
and minute / 60, each channel clamped to 1. Type 4: 0.5 grey. Others: 0.1 grey.

**Light level P** (−1 until the first frame) moves 0.01 per frame toward the player actor's scaled ambient (1 without
an actor).

**The floor.** Infravision (65, 50, 50)/255, ultravision (80, 80, 120)/255 (it wins), else 0.08 grey.

- Each ambient channel below it is raised to it, and the special ambient is S = floor × (1 − P). Both go to the scene
  graph as bytes, truncated.
- The innate vision is set when the player's actor is made, on an illusion and when an SPA 65/66 buff fades.
- Infravision for every race but those below.
- Ultravision for Dark Elf (6, 77, 98), Froglok (330), 367 and NPC races 27, 32, 33, 60, 65, 69, 70, 85, 117, 118,
  122, 123.
- Neither for Human (1, 44, 55, 67, 71), Barbarian (2, 90) and Erudite (3, 78).

**The fog colour** per channel is max(trunc(trunc(A·K)·avg(A)·P), trunc(trunc(S·K)·avg(S))). K depends on the camera's
region type:

| Region type | K |
|---|---|
| water | 240/570/370 |
| slime | 57/57/0 |
| lava | 2550/0/0 |
| 8 | 0/0/125 |
| 9 | 49/52/62 |
| 10 | 179/193/227 |
| else | the zone's NewZone fog colour |

The fog is then set to it (clamped 1..255) for outdoor time types and in regions 5..10. Indoor time types use the
zone's colour as sent. A lightning flash instead sets white ambient and fog bytes max(light × 255, the stored colour).

**The sky flag** is set with the Sky option, a sky or zone name and the sky byte. It is cleared, with fog on, when the
fog end is below 101 or the camera's region type is 5 or more. This client draws the sky only while it is set.

**Not applied:** the procedural sky's ambient and colour (which, with the Sky option and the sky drawn, also replace
the fog colour's RGB). This client always takes the no-procedural-sky path for ambient and fog, so outdoor fog at night
is the hour tables' dark blue-black.

## Viewer

`hailstone --zone NAME --sky N --time HH` (default sky 1, hour 12; `--sky 0` for none).

- Without a server there is no fog density, so nothing fogs.
- `--zone-fog ZTYPE R G B FOGMIN FOGMAX DENSITY MINCLIP MAXCLIP` gives OP_NewZone's values (e.g. gfaydark
  `255 230 255 200 10 1600 0.33 50 1600` with `--sky 0 --time-type 2`).
- The camera's region type picks the region fog.

Example: `--zone qeynos2 --sky 1 --time 18.5 --pos 114 678 40 --look 90 5`.
