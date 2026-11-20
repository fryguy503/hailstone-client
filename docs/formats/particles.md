# Particles (EDD, spellsnew.eff, WLD particle clouds)

Code: `src/assets/spell_effects.{h,cpp}` (files), `src/game/particles.{h,cpp}` (stage loop, emitters), `src/render/particle_renderer.{h,cpp}` and `shaders/particle.{vert,frag}` (drawing), `src/app/spell_particles.{h,cpp}` (the game's cues). Tests: `tests/particles_test.cpp`.

Field meanings and the simulation are traced from EQGraphicsDX9.dll; OpenEQ's spell effect format notes named the
fields first. Checked: CEmitterDef::CreateEmitter @0x1006c920, the emitter constructor @0x1006c4d0, the per-frame
update and quad builder FUN_10072160.

## Legacy switch particles

Type 53 (`0x35`) switches received through `OP_SpawnDoor` have no model. Their `door_param` selects one of the
256 old `spells.eff` records (each `0xae8` bytes), and an active state starts that record's third stage. Waterfall
cascades use this. It is the legacy `DoParticleEffect` path (`0x521ca0`), not the EDD stage player.

`assets/legacy_particles.*` reads the explicit-position slots (selector 1) of that stage and resolves their sprites
from the player's `gequip.s3d` (a WLD 0x26 -> 0x05 -> 0x04 -> 0x03 chain, with its bitmap frames and material). The
client supplies no effect records or textures: bad file lengths, unresolved sprites, missing textures and unsafe slots
create nothing.

The old stage's nonzero fields override the explicit-position cloud template: gravity, direction, radius, cone angle,
lifetime, velocity, interval, size and RGB tint. Rate is milliseconds between emissions. The native external-cloud
converter (`0x10071640`) differs from the static WLD cloud converter:

- gravity comes from the third component of its gravity vector;
- cone tilt uses `(1 - angle / 512) * 100` along its horizontal direction;
- size is multiplied by 1.5 (except the native smoke-name exception).

It keeps the sprite's material, frame sequence, full-lifetime opacity fade and bottom-up texture orientation. The
first stage-2 slot is available at every level; the second and third require levels 24 and 39.

`app::SwitchParticles` owns the effects by door id: an active emitter starts once, stops with the switch and restarts
on reactivation; door-list replacement, zoning and disconnect retire them; an unchanged duplicate door packet keeps
the running simulation. The environment particle density controls them, independently of spell effects.

Day/night switch modes use the received game hour: mode 0 keeps the packet state, mode 1 is active at hours 5 through
18 inclusive, and mode 2 at night. The native mode-2 rain-screen override remains unconnected (docs/unknowns.md).
Other old effect selectors that need actor attachments are not implemented. Seen live on the stock PEQ test server
(Stonebrunt Mountains waterfall, effect 142).

## EDD

The file starts with `"EDD\0"` and a 4-byte ASCII version.

- Version `110` is read directly: (size - 8) / 0x1a0 records. Record 0 is the file's own `None` sentinel.
- Other versions, and the legacy headerless format with 0xc4-byte records, do not occur in a RoF2 install;
  `assets::parse_edd` refuses them.

The record layout is `assets::EmitterDefinition` (offsets in its comments). Fields +0x13c, +0x140, +0x178 and
+0x148..+0x168 are not used.

Texture names are searched in `SpellEffects\`, `EnvEmitterEffects\`, `ActorEffects\`. An emitter draws nothing
(`render::particle_texture_slot`) when its texture is in no folder, does not decode, is past Hailstone's texture cap
or has an empty name, as in RoF2 (particle batch draw @0x10071b90). Spell effect 226's impact emitter 1351 names
`spear2.dds`, which no install has.

## spellsnew.eff stages

268-byte records: a 64-byte name and three 0x44-byte stages (cast, projectile, impact). Each stage is a wave id and four slots `{emitter index, minimum level, location, attach}` (slot k of a stage is at `+0x44 + stage x 0x44 + 16k`). A spell's SpellAnim (spells_us.txt field 145) picks the record.

## Creating emitters

`game::spawn_stage_emitters` runs the slot loop. It skips an index < 1 or a caster level below the slot's minimum.

Location:

| Value | Where |
|---|---|
| 0 | the given point |
| 1 | the caster |
| 2 | the live spell missile actor (its spell-point array starts empty; no eye or mouth point is invented) |
| 3 | the target |

The attach id picks one of the actor's spell points (below):

| Id | Point |
|---|---|
| 1 | point 0 |
| 2..7 | points 1..6, or point 0 when the model lacks it |
| 8 | the actor without a point |
| 9 / 10 | the eyes (points 7 / 8; without them the slot is skipped) |
| 11 | the mouth (point 9) |
| 12 | the actor without a point, at the chest's x and y, z the floor below (note) |
| any other | skips the slot |

For id 12 the floor is `CDisplay::GetPlayerFloorHeight` from 0.1 above the spawn, and scale = the actor's particle
scale (actor vtable +0x218: its model's `Resources\moddat.ini` `PESFactor`, 1 without one).

- A point the model lacks leaves the emitter on the actor alone.
- The key is `spawn id << 16 | spell id`.
- Density and opacity are eqclient.ini `[Defaults] SpellParticleDensity` and `SpellParticleOpacity` (clamped to
  [0, 1]; 1.0 with the flag argument).

`ParticleSystem::create` (`game::emitter_counts`) follows CreateEmitter:

- **Density and duration.** Density <= 0 creates nothing. The duration D is `-EDD+0x78` when that is negative, else
  the requested ms x 0.001 when positive, else `+0x78`.
- **Counts.** The initial count is trunc(+0x80 x density), at least min(16, +0x80). With +0x84 of 16 or more the
  per-tick count is trunc(+0x84 x density) at +0x88 ticks a second. Below 16 it stays +0x84 and the rate becomes
  +0x88 x density (fewer ticks, not smaller ones).
- **Capacity** (the particle ring), with E = D - delay (+0x8c) and L = lifetime (+0x7c):
  - 0 when E < 0;
  - else +0x144 when set;
  - else when L >= 0 and E > L: n = trunc(L x rate); initial + n x per tick when initial > per tick, else
    (n + 1) x per tick;
  - otherwise initial + trunc(E x rate) x per tick;
  - then at least +0x144, at most 100,000.
- **Return value:** how long the particles show, in ms. D when L < 0; trunc(E x rate) / rate + delay + L when it
  ticks; else L + delay. 0 past 600 s or with no ring. The stage length is the longest.

The impact stage's caster is the OP_Action's source and its target the struck spawn. With a source of 0 or one not in
the zone there is no caster and nothing plays. Checked: EQPlayer::TriggerSpellEffect @0x593eb0, DoSpellEffect
@0x522500.

## Spell points

An actor has ten named spell points: 0 `SPELLPOINT_DEFAULT`, 1 `_CHEST`, 2 `_HEAD`, 3 `_LHAND`, 4 `_RHAND`, 5 `_LFOOT`, 6 `_RFOOT`, 7 `_LEYE`, 8 `_REYE`, 9 `_MOUTH`. A point (0xa4 bytes) is a name[64], a bone name[64], an offset, a rotation (heading, pitch, roll, 512 per turn) and a scale (`assets/model_points.h`).

**EQG models:** `<model>.pts` beside the mesh in its archive (`EQPT`, u32 count, u32 version 1, the records). The same
loader reads `<model>.prt`, the actor emitters. Example, dkm.pts: DEFAULT on ROOT_BONE, CHEST on CHEST_CHEST03, HEAD
on HEAD_HEAD 0.25 out, the hands on ARML_FARM / ARMR_FARM 1.3 out, the feet, both eyes and the mouth on HEAD_HEAD, and
a DAMAGE_POINT.

**WLD models:** the code is bone 0's first three letters. WLD models have no eyes.

| Point | Bone |
|---|---|
| DEFAULT | bone 0 |
| CHEST | `<code>CHEST_POINT_TRACK`, else `<code>CH_TRACK` |
| HEAD and MOUTH | `<code>HEHEAD_TRACK`, else `<code>HEAD_POINT_TRACK` (note) |
| hands | `<code>L_POINT_TRACK` / `R_POINT_TRACK`, else `<code>SPELL_POINT_TRACK` |
| feet | `<code>BOFOOTL_TRACK` / `BOFOOTR_TRACK`, else `<code>BO_L_TRACK` / `BO_R_TRACK` |

On `<code>HEHEAD_TRACK` (the Luclin models) the mouth is at (0.08, 0.6, 0) turned (128, 0, 128). On
`<code>HEAD_POINT_TRACK` the mouth's `XOffset` .. `ZScale` come from `Resources\SPOffsets.ini` `[<code>]` (defaults
0.6, 0, 0, 0, 0, 0, 1, 1, 1).

**Matrices and following.** A point's bone is the first with exactly its name. Its matrix (`assets::point_matrix`) is
the scale, then the rotation from the table sine and cosine of (heading, -pitch, roll), then the offset, in the bone's
space.

- An emitter with a point is moved to the point's world position each frame (from the bone poses the actor renderer
  last drew).
- One on an actor without a point stays where it was made (the original moves it only when actor vtable +0x2f8
  answers true, which a character's actor never does).
- A spell emitter takes its axes from its actor's world matrix every frame (axial its +Z, transverse 1 its +X,
  transverse 2 its -Y), whatever +0x60 says.
- An actor's own emitter (given the point, not the actor) turns with its point when +0x60 is set.

The model viewer's `--spell-effect N [--spell-stage S]` replays a stage on its models.

## Actor emitters

An EQG model's `<model>.prt` (beside its mesh and `.pts`) lists emitters of `ActorEmittersNew.edd` on its points
(`assets::parse_prt`). Layout: `PTCL`, u32 count, u32 version 1..5, then records of 0x5c, 0x60, 0x64, 0x68 or 0x6c
bytes. The loader widens them to 0x6c (the missing fields: floor 0, variant -1, +0x64 0, alternate = the emitter).

| Offset | Field |
|---|---|
| +0x00 | the emitter |
| +0x04 | the point name |
| +0x44 | trigger: 0 always on, 1 started by the animation at +0x48 |
| +0x4c, +0x50 | kept |
| +0x54 | delay, ms |
| +0x58 | duration (5000 in every shipped file) |
| +0x5c | floor flag: drops the emitter to the floor below the point |
| +0x60 | variant: -1 all; those whose variant is -1 or the actor's are made |
| +0x68 | alternate emitter, taken when the particle system asks for it |

When an actor is made, each always-on record whose point exists and whose variant is below 1 makes its emitter on the
point (CActor::vf126 @0x1003a950):

- for 5,000,000 ms whatever +0x58 says;
- with the actor density and opacity (eqclient.ini `ActorParticleDensity` / `ActorParticleOpacity`), scale 1;
- its shape and counts scaled by the actor's emitter-scaling radius over the record's +0xac, when that radius is 0 or
  more (OP_ZoneSpawns sends -1 from stock EQEmu: no scaling).

With the floor flag it is made on the floor below the point (+ 0.05), along the actor's matrix, without the point.
Held items do the same on their own points: an item's points hang from `ATTACH_TO_ORIGIN`, the holding bone's frame.
A record pointing past the end of the EDD (some shipped files do, e.g. IT12753's 1059) makes nothing.

The client (`app/actor_emitters.h`) keeps a persistent emitter per record for each drawn actor, from the EQG body and
its held items (`assets::MeshEmitter`, `CharacterLibrary::item_mesh`). Differences and gaps:

- No records are made while ActorParticleDensity is 0; a change between non-zero values recounts running emitters in
  place. An actor out of view emits nothing (docs/graphics.md, Particles: a deliberate difference).
- A floor record stays on its point and follows the actor. What makes the original's again as the actor moves was not
  traced (docs/unknowns.md).
- The records are made once per dressed mesh; one that makes nothing is not tried again until the mesh changes.

Trigger 1 records whose animation is not STND or IDLE (run dust on animation 27 NRUN, footfalls, swings, casts) start
each time their animation (the table row's old-style clip) starts or loops. They start after +0x54 and emit for at
most 1.5 s; the original's timing is not traced (docs/graphics.md, Particles). The billboard mode +0x70 is drawn as
docs/graphics.md says.

## Blood

A local player's landed sharp-weapon hit queues a bounded target request; the effect uses the player's `gequip.s3d`
`I_BLOODSPRITE`.

- Blood is made only for a supported single-frame sprite and an exact authored `<code>_BLD_DAG` attachment (no track
  name substitutes). EQG and missing attachment data give none.
- The material alias 18 resolves to `0x01080507`.
- The effect emits for 0.5 s at 200 Hz with capacity 100, radial speed 1.25..5, gravity 1, size 0.375 and half
  opacity.
- Requests clear on zone teardown; despawned targets get no emitter.
- Physical blood is independent of `/showspelleffects`.

## Simulation and drawing

The update (FUN_10072160, `ParticleSystem::update`) runs per emitter each frame.

**Owner basis and opacity.** The actor's or point's axes are normalised; whether the original's carry the actor's
scale was not read, so the scale applies once, through the scales below. An actor's own emitter takes its point
model's PESFactor. The owner's alpha multiplies particle opacity each frame (1 in play: no actor fades yet).

**Scales.** The size scale (quad size and every motion step) applies when +0x6c is 1; the offset scale (offsets and
the spawn shape) when +0x19c is 1. Each is the caller's scale (+0x88): with an actor, its root's GetScaleFactor x its
GetParticleScaleFactor (PESFactor); with a point, the point matrix's row length x the point's actor's PESFactor.

**Emission.** While time is left, once the delay has passed: the initial burst (the definition's +0x80, not the
density-scaled count), then each frame the ticks since: trunc(+0x84 x new ticks).

- Each batch is scaled by the distance factor (+0xa0 / camera distance past +0xa0), at least the constructor's floors
  (16 / initial, 16 / the ring past the initial burst).
- A full ring drops the rest: nothing is recycled.
- The particles whose time is up leave first, so a particle shows the frame it ends.

**Spawn.** The particle's origin is the emitter's place plus the offsets times the offset scale: +0xb8 along the axis,
+0xbc along transverse 1 and +0xc0 along transverse 2. It keeps a copy of the emitter's basis turned by +0xc4 about
transverse 2, then by +0xc8 about transverse 1. Its place is polar about the axis (azimuth in 512ths, radius, axial),
scaled by the offset scale; elliptic sizes are turned into the polar record. Shapes:

| Shape | |
|---|---|
| 0 | a point (azimuth 0) |
| 1 | an even ring |
| 2 | a stack of rings (8 rings of 16 for a batch under 9, else 3 to 100 rings by the batch's size) |
| 3 | a cylinder side |
| 4 | a disk |
| 5 | a sphere |
| 6 | a box surface |
| 7 | a cone side |
| 8 | a torus |
| 9 | a random ring |

- Velocities are drawn between each (min, max) pair: axial, transverse 1, transverse 2, radial, orbital.
- Lifetime +0x7c; below 0 what is left of the emitter, 0 one frame.
- Width +0xcc..+0x184 and height +0x17c..+0x180 are drawn once (one draw for both with +0x18c).
- The start rotation is 0..1000 512ths with +0x174; the spin is between +0x138 and +0x188.

**The box surface** (shape 6, shape switch @0x100737be): two signed random numbers (-1..1) for the two free axes; the
face is an unsigned random (0..1) times 6.0, rounded to nearest (fistp). Faces 0..4 are -x, +x, -y, +y, -z, and 5 or 6
is +z, so -x is half as likely as the others and +z one and a half times. The DLL draws from two shuffled tables (2000
values i x 0.001 - 1, 1000 values i x 0.001); the client uses its own generator.

**Motion.** Semi-implicit, each velocity taking its acceleration first:

- the axial velocity moves along the axis;
- the transverse ones drift in the world along the particle's transverse axes;
- the radial one moves out from the axis (shapes 5 and 6: along the surface's normal);
- the orbit turns the azimuth (512ths a second, unscaled);
- gravity (+0x128) is an acceleration downwards, and the wind (+0x12c) a steady velocity along world x.

A particle whose radius passes below 0 is not drawn unless +0x198 is set. Particles that follow (+0x74) take the
emitter's current place and basis; the others keep their own.

**Ramps.**

- Opacity: min(fade in x fade out, +0xa4) x the emitter's opacity.
- Size: +0x16c x the size scale x grow x shrink. Near the end the height is x rem / +0x190 and the width x rem /
  +0x194.
- Colour: start + round((end - start) x age / life) per channel.
- Flipbook frame: round(+0x134 x age) mod +0x130, grids 2x2 (2..4 frames), 4x2 (5..8), 4x4 (9..16).
- The rotation turns the quad only while the particle spins: age x spin + the start angle.

**Drawing.** Billboard mode +0x70:

- 0 faces the camera, turned clockwise on the screen by the rotation;
- 2 lies flat on the world's horizontal plane, turned by it;
- 1 is a streak along the particle's axis, its length the particle's height x the size scale, widened on the screen's
  x only.

Other draw rules:

- The depth offset +0xd0 brings the quad that much nearer along the view.
- +0x68 = 1 blends with DESTBLEND ONE, otherwise INVSRCALPHA.
- No particle writes depth (the original's +0x64 branch turns writes back on after its draw; the client does not copy
  that).
- Nothing nearer than 2 units or beyond 500 along the view is drawn.
- EDD +0x170 and +0x13c / +0x140 are not read by the update or the draw.

## In the game

- OP_BeginCast plays the cast stage at the caster for the packet's cast time.
- OP_Action 231 plays the impact stage with the packet source as caster and the struck actor as target.
- The zone viewer's `--spell-effect N` plays effect N's cast and impact stages 25 units in front of the camera every
  4 s.

## WLD particle clouds (torches, lanterns, braziers)

Code: `assets/wld.h` (`ParticleCloud` 0x34, `ParticleSprite` 0x26), `assets/wld_particles.{h,cpp}` (the conversion), `load_classic_zone` (`Zone::particle_fixtures`), `game::ParticleSystem::add_fixture`, `app/zone_particles.{h,cpp}`. Tests: `tests/wld_particles_test.cpp`. The format and placement follow OpenEQ's WLD particle notes.

Classic object archives draw flames and smoke as particle clouds: a skeleton node's sprite reference can name a 0x34
instead of a 0x2d mesh (Bazaar's lanterns `HAVSTLIGHT300`/`301` with one `L307_PCD` flame each, `HAVSTLIGHT302` with
two, wall torches `FTORCH301` with an `L301_PCD` flame and `CSMOKE_PCD` smoke on a child node). Every cloud in the
shipped WLDs has flags 4 and an 84-byte body; modes 1..4 and bits 0x0..0x38580 occur, mode 3 with 0x30500 most often.

**0x34 body** (after the name):

| Offset | Field |
|---|---|
| | u32 flags, selector, mode, bits, capacity |
| +0x14 | f32 x4 (gravity, three more) |
| +0x24 | u32 duration ms |
| +0x28 | f32 radius |
| +0x2c | f32 spread angle (512ths) |
| +0x30 | u32 life ms |
| +0x34 | f32 speed |
| +0x38..+0x40 | f32 x3 direction |
| +0x44 | u32 interval ms |
| +0x48 | f32 size |
| +0x4c | u32 colour (B, G, R in the low bytes) |
| | then flag 0x01 two float triples, 0x02 24 bytes, 0x04 the 0x26 reference |

The reference word is resolved in full, and the first definition of a name wins; later same-name clouds reuse it.

**0x26 body:** flags (0), a 0x05 reference, the material word (0x80000017 everywhere). The texture is the 0x05 -> 0x04
-> 0x03 chain's first file (GENG00.DDS, CSMOKE.DDS, in `<zone>_obj.s3d`).

**Conversion.** The converter fills the same 0x1a0-byte descriptor as an EDD record (`assets::particle_cloud_definition`):

| Descriptor | From |
| --- | --- |
| +0x13c, +0x140 | selector, bits |
| +0x74 follow | 1 when bits & 0xc000 |
| +0x6c actor scale | 1 when bits & 0x10000 or following |
| +0x60 | 1 (owner basis) |
| +0x144 capacity, +0x128 gravity | capacity, +0x14 |
| +0xa8 shape | from the mode (below) |
| mode 3 | axial and radial speed, and the axis (below) |
| +0x78 duration | -(ms x 0.001), -9999 for 0 |
| +0x7c lifetime | ms x 0.001 |
| bits & 0x100 | +0x94 fade out = lifetime, +0x170 = +0xa4 max opacity = 1; without it 0.5 and no fade |
| +0x84 per tick, +0x88 ticks/s | 1, 1000 / interval |
| +0x16c | size (the update multiplies the quad by it; 1 in every shipped EDD record) |
| +0xd4..+0xe8 colours | start = end = R, G, B |
| +0x130, +0x134 | max(texture frames, 1), 1000 / timing (timing < 2 counts as 100) |
| +0x68 additive | material (negative: through the alias table; 0x80000017 -> 0x011b0507) with bit 24: bit 20 |
| +0x64 no z-write | 1 |
| name "TFIRE2*" | lifetime 0.08, capacity 8, radius 0.05, 33.3 ticks/s |

Shape (+0xa8) from the mode:

| Mode | Shape |
|---|---|
| 0 | 6 box (size = half the flag-1 ranges' extent) |
| 1 | 5 sphere (radial speed 0.25 x speed .. speed) |
| 2 | 9 ring (radial = speed) |
| 3 | 4 point (0.1 disk with bit 0x80) |
| 4 | 4 disk |
| 5 | 9 ring |

- Radius +0x28 for modes 1, 2, 4, 5.
- Modes 0, 4, 5 move along direction x speed in (transverse 1, transverse 2, axial).
- Mode 3: axial speed = speed; radial 0 .. speed x tan(+0x2c) (table lookup: 512 per turn, index truncated and masked
  to 0..127). The axis: direction (1,0,0) -> +0xc4 = 128, (0,0,-1) -> 256, (-1,0,0) -> -128, (0,1,0) -> +0xc8 = -128,
  (0,-1,0) -> 128 (exact compares in that order), else +Z.

**Placement.** Each placed actor's particle node owns one emitter at the node's world position (placement x rest
pose). The emitter axes are the node's world rows 2, 0, -1 as (axial, transverse 1, transverse 2); a spawn's +0xc8 of
-128 brings the axis onto the node's +Y, giving final axes rows 1, 0, 2 (`game::emitter_axes`). Flame nodes turn +Y up,
so flames rise.

- The owner scale (the node matrix's first row length: bazaar's lanterns stand at scale 1.5) scales motion and size
  when +0x6c is set.
- Emission scales with distance as every emitter's does; spell density and opacity do not apply.

The fixtures are persistent, created as for a 9999 s request, and their duration and capacity follow by the rules
above. So an environment emitter's EDD record with duration 0 holds a lifetime of particles, not one tick's:
potranquility's `arc_brazier`, 6 per tick at 10/s for 5 s, holds (trunc(5 x 10) + 1) x 6 = 306.

**Drawing clouds.** Additive (SRCALPHA, ONE), no depth writes, alpha test 1, no culling, no fog. Textures come from the
zone's archives first. The quad corners carry v = 1 at the top, so the texture's last row is at the top
(`ParticleRenderer::set_rows_bottom_up`): GENG00's teardrop has its tail up, like a flame.

WLD sprites keep every bitmap reference, repeats included, and show a whole texture per frame; EDD sprites keep their
atlas cells. A broken later bitmap reference rejects the sprite.

**Character and equipment clouds.** Converted definitions stay on their skeleton bones; dressing moves held-item nodes
onto the holding bone with their local rest transform. `ActorEmitters` runs them as a separate WLD simulation, with
textures from the character library's archives, per instance (an equipment or model change replaces the effects). The
same route serves character selection, the model viewer, item previews, missiles and equipment-backed server objects.
Preview particles draw into the preview's targets, without the world's soft-depth texture or two-unit cutoff.

- Actor clouds use descriptor `+0x170` as opacity and density 1 (manager `+0xa8`), independent of EDD
  `ActorParticleDensity` (`+0xac`). Checked: WLD actor constructor `0x100523b0`, standalone creator `0x1006adc0`,
  manager constructor `0x10070230`. External density overrides remain untraced.
- Owner alpha and node scale are sampled each frame, without the EQG point's PESFactor.
- An item's directly referenced multikey default tracks keep their `ObjectAnimation` sampler; node motion multiplies
  the baked rest transform inside the host bone's matrix. Animation data is shared, particle state is per instance,
  and replacement starts a new clock. No named P01 or other clip is selected. The native held-item controller's
  sharing/epoch is untraced.

**Not modelled:** the node-scale suppression below about 0.05; radial motion only for visible emitters.

EQG zones' environment emitters (`<zone>_EnvironmentEmitters.txt` over `environmentemittersnew.edd`) are drawn as
fixtures too (`assets::parse_environment_emitters`, `assets/zone_fires.h`; docs/graphics.md).
