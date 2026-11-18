# Particles (EDD, spellsnew.eff, WLD particle clouds)

## Legacy switch particles

Type 53 (`0x35`) switches received through `OP_SpawnDoor` have no model. Their `door_param` selects one of the
256 old `spells.eff` records (each `0xae8` bytes), and an active state starts that record's third stage. This is the
separate legacy `DoParticleEffect` path (`0x521ca0`), not the EDD stage player. Waterfall cascades use this mechanism.

`assets/legacy_particles.*` reads the explicit-position slots (selector 1) of that stage and resolves their sprites
from the player's `gequip.s3d`. Each sprite is a WLD 0x26 -> 0x05 -> 0x04 -> 0x03 chain, including separate bitmap
frames and the sprite's material. No effect records or textures are supplied by the client. Invalid file lengths,
unresolved sprites, missing textures and unsafe slots create no fallback particles.

The old stage's nonzero fields override the explicit-position cloud template: gravity, direction, radius, cone
angle, lifetime, velocity, interval, size and RGB tint. The field called Rate is milliseconds between emissions.
The native external-cloud converter (`0x10071640`) differs from the static WLD cloud converter: gravity comes from
the third component of its gravity vector, cone tilt uses `(1 - angle / 512) * 100` along its horizontal direction,
and size is multiplied by 1.5 (except the native smoke-name exception). The converter retains the sprite's material,
frame sequence, full-lifetime opacity fade and bottom-up texture orientation. The first stage-2 slot is available at
every level; the second and third require levels 24 and 39.

`app::SwitchParticles` owns the persistent effects by door id. The normal switch runner starts each active emitter
once, closes it with the switch, and restarts it on reactivation. Door-list replacement, zoning and disconnect
retire the old owners. An unchanged duplicate door packet preserves the running simulation. The environment
particle density controls these effects in both lighting modes, independently of spell effects. Day/night switch
modes use the received game hour: mode 0 keeps the packet state, mode 1 is active at hours 5 through 18 inclusive,
and mode 2 at night. The native mode-2 rain-screen override remains unconnected (docs/unknowns.md).

Coverage includes synthetic parser, converter and lifecycle tests, plus installed-asset checks when `EQ_GAME_DIR`
is set, and a test that the parsed effects are kept until `spells.eff` or `gequip.s3d` changes (each zone-in makes
the switch manager again; `gequip.wld` is parsed only when a file's path, size or time differs). The waterfall was seen
live on the stock PEQ test server in the Stonebrunt Mountains (effect 142). Other old effect selectors that need actor
attachments are not implemented by this switch path.

Code: `src/assets/spell_effects.{h,cpp}` (files), `src/game/particles.{h,cpp}` (stage loop, emitters), `src/render/particle_renderer.{h,cpp}` and `shaders/particle.{vert,frag}` (drawing), `src/app/spell_particles.{h,cpp}` (the game's cues). Tests: `tests/particles_test.cpp`.  Field meanings and the simulation are traced from EQGraphicsDX9.dll (CEmitterDef::CreateEmitter @0x1006c920, the emitter constructor @0x1006c4d0, the per-frame update and quad builder FUN_10072160); OpenEQ's spell effect format notes named the fields first.

## EDD

The file starts with `"EDD\0"` and a 4-byte ASCII version. Version `110` is read directly: (size - 8) / 0x1a0 records; record 0 is the file's own `None` sentinel. Other versions are converted field by field and a file without the magic is the legacy headerless format with 0xc4-byte records; neither occurs in a RoF2 install (`spellsnew.edd` 2,577 records, `actoremittersnew.edd` 944, `environmentemittersnew.edd` 626) and `assets::parse_edd` refuses them.

The record layout is `assets::EmitterDefinition` (offsets in its comments). Texture names are searched in `SpellEffects\`, `EnvEmitterEffects\`, `ActorEffects\`. An emitter whose texture no folder has, that does not decode, or whose name is empty (six `spellsnew.edd` records) draws nothing (`render::particle_texture_slot`), as in the RoF2 client: EQGraphicsDX9's particle batch draw @0x10071b90 tries the three folders, then a resource already loaded under that name, and otherwise disables the batch and skips its draw (spell effect 226's impact emitter 1351 names `spear2.dds`, which no install has). A texture past Hailstone's own texture cap is not drawn either. Fields +0x13c, +0x140, +0x178 and +0x148..+0x168 are not used.

## spellsnew.eff stages

268-byte records: a 64-byte name and three 0x44-byte stages (cast, projectile, impact), each a wave id and four slots `{emitter index, minimum level, location, attach}` (slot k of a stage is at `+0x44 + stage x 0x44 + 16k`). A spell's SpellAnim (spells_us.txt field 145) picks the record.

## Creating emitters

`game::spawn_stage_emitters` runs the slot loop: skip an index < 1 or a caster level below the slot's minimum; location 0 the given point, 1 the caster, 2 the live spell missile actor (its spell-point array starts empty; no eye or mouth point is invented), 3 the target; the attach id picks one of the actor's spell points (below): 1 point 0; 2..7 points 1..6, or point 0 when the model lacks it; 8 the actor without a point; 9 / 10 the eyes (points 7 / 8; without them the slot is skipped); 11 the mouth (point 9); 12 the actor without a point at the chest's x and y, z the floor below (`CDisplay::GetPlayerFloorHeight` from 0.1 above the spawn), with scale = the actor's particle scale (actor vtable +0x218: its model's `Resources\moddat.ini` `PESFactor`, 1 without one); any other id skips the slot. A point the model lacks leaves the emitter on the actor alone. The key is `spawn id << 16 | spell id`. Density and opacity are eqclient.ini `[Defaults] SpellParticleDensity` and `SpellParticleOpacity` (clamped to [0, 1]; 1.0 with the flag argument).

`ParticleSystem::create` (`game::emitter_counts`) works as CreateEmitter does:

- density <= 0 creates nothing; the duration D is `-EDD+0x78` when that is negative, else the requested ms x 0.001 when positive, else `+0x78`;
- the initial count is trunc(+0x80 x density), at least min(16, +0x80); with +0x84 of 16 or more the per-tick count is trunc(+0x84 x density) at +0x88 ticks a second, below 16 it stays +0x84 and the rate becomes +0x88 x density (fewer ticks, not smaller ones);
- with E = D - delay (+0x8c) and L = lifetime (+0x7c), the capacity (the particle ring) is 0 when E < 0, else +0x144 when set, else when L >= 0 and E > L: n = trunc(L x rate), initial + n x per tick when initial > per tick, else (n + 1) x per tick; otherwise initial + trunc(E x rate) x per tick; then at least +0x144, at most 100,000;
- it returns how long the particles show, in ms: D when L < 0, trunc(E x rate) / rate + delay + L when it ticks, else L + delay; 0 past 600 s or with no ring. The stage length is the longest.

The impact stage's caster is the OP_Action's source and its target the struck spawn (EQPlayer::TriggerSpellEffect @0x593eb0 to DoSpellEffect). With a source of 0 or one not in the zone there is no caster, and DoSpellEffect (@0x522500) plays nothing, here too.

## Spell points

An actor has ten named spell points: 0 `SPELLPOINT_DEFAULT`, 1 `_CHEST`, 2 `_HEAD`, 3 `_LHAND`, 4 `_RHAND`, 5 `_LFOOT`, 6 `_RFOOT`, 7 `_LEYE`, 8 `_REYE`, 9 `_MOUTH`. A point (0xa4 bytes) is a name[64], a bone name[64], an offset, a rotation (heading, pitch, roll, 512 per turn) and a scale (`assets/model_points.h`):

- **EQG models:** `<model>.pts` beside the mesh in its archive (`EQPT`, u32 count, u32 version 1, the records; the same loader also reads `<model>.prt`, the actor emitters). dkm.pts: DEFAULT on ROOT_BONE, CHEST on CHEST_CHEST03, HEAD on HEAD_HEAD 0.25 out, the hands on ARML_FARM / ARMR_FARM 1.3 out, the feet, both eyes and the mouth on HEAD_HEAD, and a DAMAGE_POINT.
- **WLD models** (the code being bone 0's first three letters): DEFAULT on bone 0; CHEST on `<code>CHEST_POINT_TRACK`, else `<code>CH_TRACK`; HEAD and MOUTH on `<code>HEHEAD_TRACK` (the mouth at (0.08, 0.6, 0) turned (128, 0, 128): the Luclin models), else on `<code>HEAD_POINT_TRACK` with the mouth's `XOffset` .. `ZScale` from `Resources\SPOffsets.ini` `[<code>]` (defaults 0.6, 0, 0, 0, 0, 0, 1, 1, 1); the hands on `<code>L_POINT_TRACK` / `R_POINT_TRACK`, else `<code>SPELL_POINT_TRACK`; the feet on `<code>BOFOOTL_TRACK` / `BOFOOTR_TRACK`, else `<code>BO_L_TRACK` / `BO_R_TRACK`. No eyes.

A point's bone is the first with exactly its name ; its matrix (`assets::point_matrix`) is the scale, then the rotation built from the table sine and cosine of (heading, -pitch, roll), then the offset, all in the bone's space. Each frame the update puts an emitter that has a point at the point's world position. One on an actor without a point stays where it was made: the update moves it only when the actor answers vtable +0x2f8 true, which a character's actor never does. A spell emitter takes its axes from its actor's world matrix every frame (axial its +Z, transverse 1 its +X, transverse 2 its -Y), whatever +0x60 says; an actor's own emitter (given the point, not the actor) turns with its point when +0x60 is set. The client takes the bone poses the actor renderer last drew (`ActorRenderer::drawn_bone`); the model viewer's `--spell-effect N [--spell-stage S]` replays a stage on its models.

## Actor emitters

An EQG model's `<model>.prt` (beside its mesh and `.pts`) lists emitters of `ActorEmittersNew.edd` on its points (`assets::parse_prt`): `PTCL`, u32 count, u32 version 1..5, records of 0x5c, 0x60, 0x64, 0x68 or 0x6c bytes that the loader widens to 0x6c (the missing fields: floor 0, variant -1, +0x64 0, alternate = the emitter). Each record keeps: the emitter (+0x00), the point name (+0x04), a trigger (+0x44: 0 always on, 1 started by the animation at +0x48), +0x4c, +0x50, a delay (+0x54, ms), a duration (+0x58, 5000 in every shipped file), a floor flag (+0x5c: drops the emitter to the floor below the point), a variant (+0x60: -1 all; those whose variant is -1 or the actor's are made) and an alternate emitter (+0x68, taken when the particle system asks for it).

When an actor is made, each always-on record whose point exists and whose variant is below 1 makes its emitter on the point (CActor::vf126 @0x1003a950): for 5,000,000 ms whatever +0x58 says, with the actor density and opacity, scale 1, its shape and counts scaled by the actor's emitter-scaling radius over the record's +0xac when that radius is 0 or more (OP_ZoneSpawns sends -1 from stock EQEmu: no scaling). With the floor flag it is made on the floor below the point (+ 0.05), along the actor's matrix, without the point. Held items do the same on their own points: an item's points hang from `ATTACH_TO_ORIGIN`, its origin, which is the holding bone's frame.

Here a floor record stays on its point and follows the actor: what makes the original's again as the actor moves was not traced (docs/unknowns.md). The records are made once per dressed mesh; one that makes nothing (an index past the file's end) is not tried again until the actor's mesh changes.

The client (`app/actor_emitters.h`): the dressed mesh carries the always-on records of the EQG body and of its held items (`assets::MeshEmitter`; EQG held items are drawn too, `CharacterLibrary::item_mesh`); each drawn actor keeps a persistent emitter per record, placed every frame on the point as the actor renderer posed it; eqclient.ini `ActorParticleDensity` / `ActorParticleOpacity`. 12,449 always-on records in the shipped files point into its `ActorEmittersNew.edd` (945 records), 315 past its end (e.g. IT12753's 1059), which make nothing.

The trigger 1 records whose animation is not STND or IDLE (1,020 run-dust records on animation 27 NRUN, footfalls,
swings, casts) are started by the client each time their animation (the table row's old-style clip) starts or loops
on the actor, after +0x54, emitting for at most 1.5 s; the original's timing is not traced (docs/graphics.md,
Particles). The billboard mode +0x70 is drawn as docs/graphics.md says.

## Blood

A local player's landed sharp-weapon hit queues a bounded target request. The app reads the player's `gequip.s3d` `I_BLOODSPRITE` resource and creates blood only for a supported single-frame sprite and an exact authored `<code>_BLD_DAG` attachment. Track names cannot substitute for that bone. EQG and missing attachment data leave it unset. The material alias 18 resolves to `0x01080507`. The effect emits for 0.5 s at 200 Hz with capacity 100, radial speed 1.25..5, gravity 1, size 0.375 and half opacity. The implementation ships no proprietary asset or table. Pending requests clear on zone teardown; despawned targets produce no emitter. Physical blood is independent of `/showspelleffects`.

## Simulation and drawing

The update (FUN_10072160, `ParticleSystem::update`), per emitter each frame:

- **Owner basis and opacity.** The actor's or point's axes are normalised (whether the original's carry the actor's scale was not read; the scale applies once, through the scales below). An actor's own emitter takes its point model's PESFactor. The owner's alpha multiplies particle opacity each frame; the renderer fades no actor yet, so it is 1 in play.
- **Scales.** The size scale (quad size and every motion step) applies when +0x6c is 1, the offset scale (offsets and the spawn shape) when +0x19c is 1. Each is the caller's scale (+0x88); with an actor, its root's GetScaleFactor x its GetParticleScaleFactor (PESFactor); with a point, the point matrix's row length x the point's actor's PESFactor.
- **Emission** while time is left: once the delay has passed, the initial burst (the definition's +0x80, not the density-scaled count), then each frame the ticks since: trunc(+0x84 x new ticks). Each batch is scaled by the distance factor (+0xa0 / camera distance past +0xa0), at least the constructor's floors (16 / initial, 16 / the ring past the initial burst). A full ring drops the rest: nothing is recycled. The particles whose time is up leave first, so a particle shows the frame it ends.
- **Spawn.** The particle's origin is the emitter's place plus the offsets times the offset scale, +0xb8 along the axis, +0xbc along transverse 1 and +0xc0 along transverse 2. It keeps a copy of the emitter's basis turned by +0xc4 about transverse 2, then by +0xc8 about transverse 1. Its place is polar about the axis (azimuth in 512ths, radius, axial), scaled by the offset scale: shape 0 a point (azimuth 0), 1 an even ring, 2 a stack of rings (8 rings of 16 for a batch under 9, else 3 to 100 rings by the batch's size), 3 a cylinder side, 4 a disk, 5 a sphere, 6 a box surface, 7 a cone side, 8 a torus, 9 a random ring; elliptic sizes are turned into the polar record. Velocities are drawn between each (min, max) pair: axial, transverse 1, transverse 2, radial, orbital. Lifetime +0x7c; below 0 what is left of the emitter, 0 one frame. Width +0xcc..+0x184 and height +0x17c..+0x180 are drawn once (one draw for both with +0x18c), the start rotation 0..1000 512ths with +0x174, the spin between +0x138 and +0x188.
- **Motion** (semi-implicit, each velocity taking its acceleration first): the axial velocity moves along the axis, the transverse ones drift in the world along the particle's transverse axes, the radial one moves out from the axis (shapes 5 and 6: along the surface's normal), the orbit turns the azimuth (512ths a second, unscaled); gravity (+0x128) is an acceleration downwards and the wind (+0x12c) a steady velocity along world x. The radius is not held at the axis: a particle whose radius passes below 0 is not drawn unless +0x198 is set. Particles that follow (+0x74) take the emitter's current place and basis; the others keep their own.
- **Ramps.** Opacity min(fade in x fade out, +0xa4) x the emitter's opacity; size +0x16c x the size scale x grow x shrink; the height x rem / +0x190 and the width x rem / +0x194 near the end; colour start + round((end - start) x age / life) per channel; flipbook frame round(+0x134 x age) mod +0x130, grids 2x2 (2..4 frames), 4x2 (5..8), 4x4 (9..16). The rotation turns the quad only while the particle spins: age x spin + the start angle.

Drawing (the same function's quad builder): billboard mode +0x70 0 faces the camera, turned clockwise on the screen by the rotation; 2 lies flat on the world's horizontal plane, turned by it; 1 is a streak along the particle's axis, its length the particle's height x the size scale, widened on the screen's x only. The depth offset +0xd0 brings the quad that much nearer along the view. +0x68 = 1 blends with DESTBLEND ONE, otherwise INVSRCALPHA; no particle writes depth (the particle render state turns writes off; +0x64's branch turns them off again and, after its draw, back on, which the client does not copy). Nothing nearer than 2 units or beyond 500 along the view is drawn. EDD +0x170 and +0x13c / +0x140 are not read by the update or the draw.

The box surface (shape 6) as the graphics DLL places it (its shape switch @0x100737be): two random numbers from its signed table (-1..1) for the two free axes, then the face from its unsigned table (0..1) times 6.0 rounded to nearest (fistp), so faces 0..4 are -x, +x, -y, +y, -z and 5 or 6 is +z (the -x face is half as likely as the others, +z one and a half times). The DLL's random numbers are two shuffled tables (2000 values i x 0.001 - 1, 1000 values i x 0.001); the client draws from its own generator.

## In the game

OP_BeginCast plays the cast stage at the caster for the packet's cast time; OP_Action 231 plays the impact stage with the packet source as caster and the struck actor as target. The zone viewer's `--spell-effect N` plays effect N's cast and impact stages 25 units in front of the camera every 4 s.

## WLD particle clouds (torches, lanterns, braziers)

Code: `assets/wld.h` (`ParticleCloud` 0x34, `ParticleSprite` 0x26), `assets/wld_particles.{h,cpp}` (the conversion), `assets::wld::skeleton_particles` and `load_classic_zone` (`Zone::particle_definitions`, `Zone::particle_fixtures`), `game::ParticleSystem::add_fixture`, `game::emitter_axes`, `game::emission_distance_factor`, `app/zone_particles.{h,cpp}` (client and viewer). Tests: `tests/wld_particles_test.cpp`. The format and placement follow OpenEQ's WLD particle notes.

The classic object archives draw flames and smoke as particle clouds, not meshes: a skeleton node's sprite reference can name a 0x34 instead of a 0x2d mesh. Bazaar's lanterns `HAVSTLIGHT300`/`301` (one `L307_PCD` flame each), `HAVSTLIGHT302` (two) and wall torches `FTORCH301` (`L301_PCD` flame, `CSMOKE_PCD` smoke on a child node) are 53 placements and 83 emitters; PoK has 475. All 826 clouds in the shipped WLDs have flags 4 and an 84-byte body; 567 have mode 3 with bits 0x30500 (the survey: modes 1..4, bits 0x0..0x38580).

0x34 body (after the name): u32 flags, selector, mode, bits, capacity; f32 x4 (+0x14 gravity, three more); u32 duration ms (+0x24); f32 radius (+0x28), spread angle (+0x2c, 512ths); u32 life ms (+0x30); f32 speed (+0x34); f32 x3 direction (+0x38..+0x40); u32 interval ms (+0x44); f32 size (+0x48); u32 colour (+0x4c, B, G, R in the low bytes); then flag 0x01 two float triples, 0x02 24 bytes, 0x04 the 0x26 reference. The reference word is resolved in full, and the first definition of a name wins (later same-name clouds, such as the bazaar's at fragments 609..750, reuse it).

0x26 body: flags (0), a 0x05 reference, the material word (0x80000017 everywhere). The texture is the 0x05 -> 0x04 -> 0x03 chain's first file (GENG00.DDS, CSMOKE.DDS, in `<zone>_obj.s3d`).

The converter fills the same 0x1a0-byte descriptor as an EDD record (`assets::particle_cloud_definition`):

| Descriptor | From |
| --- | --- |
| +0x13c, +0x140 | selector, bits |
| +0x74 follow | 1 when bits & 0xc000 |
| +0x6c actor scale | 1 when bits & 0x10000 or following |
| +0x60 | 1 (owner basis) |
| +0x144 capacity, +0x128 gravity | capacity, +0x14 |
| +0xa8 shape | mode 0 -> 6 box (size = half the flag-1 ranges' extent), 1 -> 5 sphere (radial speed 0.25 x speed .. speed), 2 -> 9 ring (radial = speed), 3 -> 4 point (0.1 disk with bit 0x80), 4 -> 4 disk, 5 -> 9 ring (radius +0x28 for 1, 2, 4, 5); modes 0, 4, 5 move along direction x speed in (transverse 1, transverse 2, axial) |
| mode 3 | axial speed = speed; radial 0 .. speed x tan(+0x2c) (table lookup: 512 per turn, index truncated and masked to 0..127); the axis: direction (1,0,0) -> +0xc4 = 128, (0,0,-1) -> 256, (-1,0,0) -> -128, (0,1,0) -> +0xc8 = -128, (0,-1,0) -> 128 (exact compares in that order), else +Z |
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

Placement: each placed actor's particle node owns one emitter. Its origin is the node's world position (placement x rest pose). Its basis: the update takes the emitter axes from the node's world rows 2, 0, -1 as (axial, transverse 1, transverse 2); each particle's spawn turns its copy by +0xc4 about transverse 2, then by +0xc8 about transverse 1 (-128 brings the axis onto the node's +Y: final axes rows 1, 0, 2; `game::emitter_axes`). The lanterns' and torches' flame nodes turn +Y up, so the flames rise. The owner scale (the node matrix's first row length: bazaar's lanterns stand at scale 1.5) scales motion and size when +0x6c is set. Emission scales with distance as every emitter's does (Simulation and drawing). The fixtures are persistent, created as for a 9999 s request: their duration and capacity follow from that by the rules above, so an environment emitter's EDD record with duration 0 holds a lifetime of particles (potranquility's `arc_brazier`, 6 per tick at 10/s for 5 s: (trunc(5 x 10) + 1) x 6 = 306), not one tick's. Spell density and opacity do not apply.

Drawing: additive (SRCALPHA, ONE), no depth writes, alpha test 1, no culling, no fog . The quad corners carry v = 1 at the top and 0 at the bottom, so the texture's last row is at the top (`ParticleRenderer::set_rows_bottom_up`): GENG00's teardrop then has its tail up, like a flame. Textures come from the zone's archives first (`set_texture_reader`).

WLD sprites retain every bitmap reference, including repeats, and select a whole texture for the particle's current
frame; EDD sprites retain their atlas cells. A broken later bitmap reference rejects the sprite rather than changing
its sequence.

Character and equipment clouds: the WLD builder retains converted definitions on their skeleton bones. Dressing
remaps held-item nodes onto the holding bone, retaining their local rest transform. `ActorEmitters` owns a separate
WLD simulation and bottom-up renderer alongside its EDD actor list, with textures read from the character library's
archives. Each instance keeps its own particle state; an equipment or model change replaces its effects, and a
removed instance stops emitting. The same route serves character selection, the model viewer, item previews,
missiles and equipment-backed server objects. Preview particles use the preview's colour/depth targets, without the world's
soft-depth texture or two-unit camera cutoff, and retain alpha coverage outside the item mesh.

The native WLD actor constructor (`0x100523b0`) and standalone creator (`0x1006adc0`) pass descriptor `+0x170` as
opacity and manager `+0xa8` as density. This is independent of EDD `ActorParticleDensity` (`+0xac`). Actor clouds use
the descriptor opacity and density 1, verified in the native manager constructor (`0x10070230`); external density
configuration overrides remain untraced. Owner alpha
and node scale are sampled each frame, without the EQG point's PESFactor.

An item's directly referenced multikey default tracks retain their `ObjectAnimation` sampler (including moving
ancestors): its node motion multiplies the baked rest transform inside the host bone's matrix. Animation data is
shared, particle state is per instance, and equipment replacement starts a new instance clock. No named P01 or
other clip is selected for these default tracks. The native held-item controller's sharing/epoch remains untraced.

Not modelled: the node-scale suppression below about 0.05; radial motion only for visible emitters. EQG zones'
environment emitters (`<zone>_EnvironmentEmitters.txt` over `environmentemittersnew.edd`) are drawn as fixtures too
(`assets::parse_environment_emitters`, `assets/zone_fires.h`; docs/graphics.md).
