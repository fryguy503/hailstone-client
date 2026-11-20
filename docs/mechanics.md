# Character mechanics (src/game/mechanics)

The rules the client computes itself for the local player: effect totals (TotalEffect), stats, resists and caps,
maximum and current HP / mana / endurance, AC, haste, weight, regeneration, skills and caps, buff durations and cast
time. They run on plain data (`CharacterSheet`) and are covered by recorded test vectors (`tests/*_vectors_test.cpp`).

## Files

| File | What |
|---|---|
| `character_sheet.h` | the inputs: identity, profile base values, skills, possessions and the 21 extra item lookups, buffs, AAs, situation |
| `character.h` | `Character`: one sheet's rules, `Hooks` to replay recorded callees, `DerivedStats` |
| `effects.cpp` | calc_affect_change, the spell cache (negations, buffs, worn items, AAs), TotalEffect; `total_effect()` |
| `stats.cpp` | process_all_stats: stats, resists, caps, max / current HP, mana and endurance, AC, item haste, weight, max air supply |
| `item_sums.*` | the item part of process_all_stats: item stats summed over the worn items and other lookups, with caps |
| `regen.cpp` | HP, mana and endurance regeneration |
| `ticks.*` | DoPassageOfTime: the 6 s buff tick and mana regen tick, the 1 s endurance upkeep |
| `skills.cpp` | racial skill bonus, skill cap, adjusted skill, skill with item mods |
| `spell_math.cpp` | spell duration, casting time modifier, the SPA 127 focus, the cast time |
| `abilities.*`, `skill_timers.inc` | skills and combat abilities on a `SkillContext`; see note 1 |
| `use_skill.*` | the /doability path over plain structs and `useskill::Queries` |
| `melee.*` | melee range, attack speed, hand-to-hand delay, double attack, dual wield, the attacks, ranged attack, can-I-hit, swing timers; see note 2 |
| `item_stats.*` | the item getters (sums over augments, charm scaling, masks) |
| `tables.*`, `skill_info.inc` | BaseData.txt and SkillCaps.txt from the game directory; the built-in skill table (`tools/gen_skill_info.py`) |
| `combat_ability.*` | doCombatAbility and its checks, /discipline; see "Cast checks" |
| `movement.*` | run speed, weight, swimming, jumps, levitation, falling and fall damage, air supply |
| `physics.*` | the per-frame physics Step, the landing and fall check |
| `regions.*` | where body, head and feet regions are sampled, and what a region change under the feet does |
| `remote_spawns.*` | other spawns' position updates and their per-frame step (AdvanceSpawn) |
| `controls.*` | per-frame control processing and the movement key handlers |
| `anim_select.*` | the animation choices; see note 3 |
| `hit_by_spell.*` | a spell landing on the character, over `HitQueries`; live callees in `game/hit_landing.*` |
| `cast_spell.*` | every check before OP_CastSpell, the commit and the packet, over `cast::Queries`; live callees `CastQueries` in `net/game_connection_cast.cpp` |
| `fpu.h` | x87 arithmetic at 24-bit or 53-bit precision (two quiet NaNs with the same significand give the positive one) |
| `collision_movement.*` | the movement collision; see "Movement collision" |
| `movement_inputs.*` | the character values a movement frame reads; see "Movement inputs" |
| `switches.*` | door / switch movement and heading; see "Movement collision" |
| `floor_height.*` | the player floor test that carries lift riders |
| `touching_switch.*` | what touching a switch does (traps, touch pads, pressure plates); see "Room to stand" |
| `can_i_fit_here.*` | room to stand, and whether a still spawn must be collided |
| `vehicle.*` | a passenger into and out of a vehicle's frame, corpse slide stop; boats are `game/boats.*` (see "Boats") |
| `ladder.*` | climbing onto a clicked ladder through `LadderQueries`; live in `app/player_ladder.cpp` |
| `attack_rules.*` | who may attack whom (the PvP rules) through `AttackQueries`; live in `game/attack_check.*` |
| `spell_gems.*` | memorising, linked timers, the usable-slot rule, gem readiness; see "Spell gems and buff stacking" |
| `find_affect_slot.*` | buff slot choice and stacking over `AffectQueries`; live callees `game/hit_landing.*` |
| `shown_stats.*` | the AC and ATK the inventory window shows (label cases 0x15 / 0x16), shared with the item comparison (docs/item-compare.md) |
| `sheet_builder.*` | the sheet from the live connection; see note 4 |

Notes:

1. `abilities.*`: the skill table's reuse / base damage / combat type (`tools/gen_skill_timers.py`), skill timers,
   skill reuse time (SPA 227), can-use-combat-skill, and the adapters that run `use_skill.*` and `melee.*` on the
   context. The fist is `melee::kFistItem`. A deliberate change from RoF2: a combat skill's reuse below 0 is 0 rather
   than wrapping its unsigned timer, as the non-combat skills' already is.
2. `melee.*`: the attack for slots 11, 13, 14, 100 and 101; the ranged attack is the range slot's OP_CombatAbility
   {target, 11, the item's skill}; can-I-hit uses 3D distance squared and bearing.
3. `anim_select.*`: the movement animation and rate, the idle choice (fidgets, turning, ducking, treading water,
   stun), the attack animation choosers, the sequence table and the new/old-style maps. Used per spawn by
   `game/spawn_animation.*` (EQG track `<tag>_<style>_<n>`).
4. `sheet_builder.*`: callers add the owned AA ranks' effects (`AltAdvState::sheet_abilities`). A buff's ticks left
   are the server's count less the 6 s ticks since it arrived, except a permanent buff (kept negative) and while buff
   timers are on hold or the zone suspends buffs.

Outside this directory, with vectors: `net/reliable_channel.*` (resend timeout min(1.25 rtt + 300, 8000) ms), and the
UI rules `game/spell_book.h`, `group_rules.h`, `quantity_dialog.h`, `trade_rules.h`, `cursor_drop.h`.

## Tribute bonus items

The server sends personal tribute bonus items in inventory type 6, slots 0..4, and guild ones in type 8, slots 0..1.
`sheet_builder.cpp` puts them in the extra item lookups 0..4 and 15..16, so their stats, worn and focus effects count
like other items. Their stack count does not multiply them.

- Trophy containers have no source on stock EQEmu.
- The bulk inventory at login keeps separately received bonus items instead of erasing them.
- Stock EQEmu removes these items without deletion packets, so the connection retires them on deactivation, changed
  selections, self opt-out, guild changes and session transitions.
- A toggle request alone grants or removes nothing; repeated unchanged active-state messages keep the items.

Full tribute workflows are not tested live.

## Exactness

- Integer math wraps (explicit unsigned adds and multiplies).
- x87 precision is 24-bit in game: the D3D9 device is created without `D3DCREATE_FPU_PRESERVE`
  (behaviour flags 0x24/0x84/0x44 | 4). `Fpu` rounds every
  result as the FPU does at either precision; `-ffp-contract=off` stops fused operations. Example: drunkenness 64 and
  INT 150 give 102 at 24 bits, 101 at 53.
- A deliberate difference where the original never ends: shadow step (SPA 379) walks its distance down by 2 as a float
  while above 5; from 2^25 up `distance - 2` can round back to `distance` and the original loops for ever. Hailstone
  stops at the first step that leaves the distance unchanged; every base that ends in the original ends at the same
  step.

#### Vector replay by family

The vector tests (`tests/*_vectors_test.cpp`) replay each record's mocked callees call by call. Where a record leaves
out a callee's answer (hit-by-spell, cast-spell), the test ports the analysis generator's randomizer to rebuild the
world and checks the rebuilt inputs first. Melee results compare bit for bit; a NaN result is the positive quiet NaN
(0x7fc00000), which the exporter writes as `"nan"`. The vectors' item field "delay" is the item's skill (the Delay
byte), which the fist and hand-to-hand (type 0x16) items use for the reuse time.

## Not covered by vectors

Checked against vectors: the item sums, process-affects, the mana regen tick and endurance upkeep.

Not checked yet: the buff-slot and worn-item effect caches, recommended-level scaling, the is-spellcaster check and
the focus level falloff. The AA focus collector's iterator is modelled (an AA focus effect takes the limits that
follow it).

#### The distance modifier

A spell with a distance range scales the generic value by the modifier at the spawn's 2D distance from the player:

- The range is columns 227..230 (near distance and modifier, far distance and modifier) and the parser's ratio +0x4e9.
- The distance is clamped to the range, at least 0; the far modifier applies when the spawn is not found.
- The result rounds half away from zero; a negative value rounding to 0 gives -1.
- Measured from: the buff cache from the slot's caster (`SheetBuff::caster_distance`); HitBySpell from its caster;
  FindAffectSlot from the existing buff's caster.
- Worn effects and base2 values are not scaled.

Checked: CharacterZoneClient::CalcAffectChange @0x436670, PlayerClient::CalcSpellDistanceMod @0x59fbe0.

The player's extra long-buff slots: one from level 72, two from 75 (`affect::pc_extra_buff_slots`; @0x580460).

The levitation mode reads `cached_buff(57, k)` (buff slot effects key SPA 57 by Base2). Spell descriptions
(`game/spell_description.h`) call `calc_affect_change(spell, level, slot, nullptr, Base, true)` (no distance
scaling) and `spell_duration`.

## Frame rate: the game clock

The original runs its game rules once per drawn frame, and several are written per frame (air friction 0.99 a frame,
the turn ramp, LookSpring, /follow's turn). Run as written, at 500 fps a fall never deals fall damage and a running
jump carries a third as far; at 15, falls hit far harder.

Hailstone runs them as the original would at a steady 60 frames a second (`core/step_clock.h`). A deliberate
difference: `hailstone.ini` `game.fixed_step`, on by default; `0` runs one step per drawn frame, as written.

- A drawn frame runs the steps that came due: none at high frame rates, several below 60.
- A step's time is what the original's millisecond clock read at 60 fps (16, 17, 17 ms apart).
- A frame longer than 15 steps (250 ms) runs 15 and drops the rest.
- Whether the original was played at 60 is not known; 60 is the chosen rate.

On the game clock (`update_player`'s steps, app/client_app_player.cpp):

- the player's movement: ProcessControls, physics, collision, landing and fall damage, position samples, a boat's helm;
- /follow's and /nav's steering;
- other spawns' and boats' physics between updates (`WorldState::physics_steps`);
- missiles (`MissileList::update`; a missile moves every other step);
- the weather's fog band, strikes and drop count; the light level and fog (`SceneFogInput::steps`);
- the once-a-frame timers (`GameConnection::set_game_steps`): scribing, memorizing, the 1 s and 6 s ticks, socials'
  lines and `/pause`.

Not on it: what is timed already (casting, cool-downs, doors, network, animations, particles, sounds) and the
scripts' and plugins' frame (`mq.delay`, OnPulse). Keys, clicks (kept for the next step), mouse look and the camera
run once a drawn frame.

The camera is drawn between the last two steps, at most one step (17 ms) behind. The player's body and mount are drawn
at that same place (`WorldState::set_local_drawn`), not eased as other spawns are, which would trail the camera.

Tests: `tests/frame_rate_test.cpp` (15 to 1000 fps give the same result).

## Movement inputs

`PlayerMovement::update` (app/player_movement.cpp) takes the character's side of a movement frame from
`movement_facts(const Character&)` (`game/mechanics/movement_inputs.h`). Before the first Character exists, a fallback
approximates: buffs' base values per SPA, profile STR / STA and raw skills, heroic AGI 0, plain item weights.

| Input | Source |
|---|---|
| run rate | `TotalEffect(99)` and `TotalEffect(3)` through `movement_rate_effect`; running floor `TotalEffect(271)`; speed cap raise `TotalEffect(290)` |
| swim speed | adjusted skill 50 |
| weight factor | `calculate_weight`; see below |
| fall damage | current weight, adjusted skill 39, the profile's Safe Fall skill, `TotalEffect(228)`, heroic AGI bonus; no-fall-damage is the zone's (OP_NewZone +894) |
| air supply and region damage | max air supply with STA and `TotalEffect(246)`; water breathing `TotalEffect(14)`; fire / cold (disease in zone 200) resists |
| fly mode | `local_fly_mode` (below) |
| sneaking | the Sneak flag; see below |

- Weight factor: carried weight (`item_weight`: each item's own weight, charm-scaled, without augments or stack
  count; bag reduction SPA 221) against STR with items and buffs; also current / max HP and `slowdown_any_level`
  (level threshold 0 instead of 3). Bazaar and barter zones come from the zone table.
- Sneaking: OP_SpawnAppearance type 15 sets it; OP_CancelSneakHide or a cast's make-me-visible clears it. Then
  `sneak_speed(class, level)` x 0.46 forward and half walk-back speed (`controls.cpp`).

Live check (`--move-test speed`, Echo of Selo at level 60): running and walking speeds and the low-HP weight factors
matched the expected values, and EQEmu's MovementCheck flagged nothing.

#### Local fly mode

The fly mode (physics object +0xc) is state, as in the original. Its writers (`mech::local_fly_mode`), in order within
a frame:

1. A levitate buff fading with no levitation left sets 0, a flying mode 1 included.
   Checked: `CharacterZoneClient_RemoveAffect` @0x439700 (SPA 57), `PlayerZoneClient::Levitating` @0x5a1140.
2. A levitate buff landing sets 2, or 5 for a non-zero Base2, over mode 1 too; 0 in a no-levitation zone.
   Checked: `EQ_Character::HitBySpell` SPA 57, FUN_00432b20 @0x432b20.
3. The server sets any mode 0 to 5 outright: the spawn record at zone-in and OP_SpawnAppearance 19. Above 5 is ignored
   (the original throws). Checked: `Handle_OP_SpawnAppearance` @0x4b4b60, `PlayerPhysics_SetFlyMode` @0x8d0910.
4. The effect cache rebuild gives 2 or 5 from the levitate buffs, else 0. It keeps mode 1, a rider takes its mount's
   mode, and a no-levitation zone forces 0. Checked: `ReCacheSpellEffects` @0x438910, `UpdateLevitationState` @0x433480.

The rebuild runs only when the cache is cleared, so a server-set mode lasts until the buffs next change: after GM
`#set flymode 3` or `#set flymode 0`, the next buff landing or fading resets it; `#set flymode 1` survives buff
changes except a levitate buff landing or fading, and a no-levitation zone grounds it at the next rebuild.

Hailstone sees the buff slots, not the packets that clear the cache: a slot whose spell changes is a rebuild, one that
loses a levitate spell a fade, one that gains one a landing (docs/unknowns.md). Zoning applies the spawn record, then
rebuilds.

Tests: `tests/movement_test.cpp`, `tests/fly_mode_test.cpp`, `tests/movement_inputs_test.cpp`.

## Movement and view keys

`__ExecuteCmd`'s movement commands go to `execute_movement_command` (game/mechanics/controls.h) on press and release:

| Keys | Commands | What they do |
|---|---|---|
| move, turn, strafe | 0..7 | |
| PITCHUP / PITCHDOWN | 17 / 18 | CameraAngle ramps by count x rate twice a step, count up to 12, clamped to +-128 |
| CENTERVIEW | 19 | see "Cameras" |
| zoom | 20 / 21 | the active camera's distance by rate x 2 a step, between the spawn's AvatarHeight and 400 |
| camera keys | 328..333 | |

- The rate is g_LocalPhysicsTimeFactor: the last step's ms x 0.02 (1/3 at 60 steps a second).
- Look, zoom and camera keys work while frozen; the move keys and CENTERVIEW do not.
- During mouse look the turn keys strafe in the first person, overhead, chase and behind-the-player cameras.

HOT1_1 .. HOT10_12 press bar (command - 37) / 12's buttons. HOTPAGE1_1 .. HOTPAGE10_10 turn bar (command - 157) /
10's page.

## Mouse buttons as keys

The middle button and the buttons past right (DirectInput's 2..7) bind like keys, shown as "MOUS MID" and
"MOUS AUX1".."MOUS AUX5". The left and right buttons are never keys. A press is key code 0xf0 + n with the held
Shift / Ctrl / Alt, sent only with no window under the cursor. Checked: 0x51a770 (end of __ProcessMouseEvents).

Hailstone feeds them to the key map with the keys, so whatever holds keys back (an edit box, a modal dialog, the
loading screen) holds them too. SDL buttons 2, 4, 5, 6, 7 and 8 map to these (`platform::dik_from_sdl_mouse_button`).

## Cameras

`game/cameras.h` ports the original's cameras (EverQuest::Cameras, CDisplay::cameraType): first person (0), overhead
(1), chase (2), user 1 and 2 (3, 4) and tether (5). This client adds the wheel-follow camera behind the player (6) and
the isometric camera (7, docs/isometric.md).

The player's model is drawn in every camera but first person. Each camera, the one behind the player included, stands
where a 0.25 sphere swept from the player's eye toward its point stops (unk_EQCamera_SetPosition,
`wheel_follow_position`).

| Camera | Where it stands (each frame, on the player as drawn) | Mouse look: sideways / up-down | Keys that act |
|---|---|---|---|
| Overhead (1) | above the eye by the height (25), looking the player's way at the pitch (-100) | the player turns / the pitch (+-128) | pan (height), pitch |
| Chase (2) | behind the player's heading by eye height + distance, above the eye by the height; looks at the player, 8.5 below its pitch (all 0 by default) | the player turns / the height | all |
| User 1, 2 (3, 4) | around the player at their own heading and distance (30 / 192 / height 5; 82 / 277 / 18), looking at the player's eye | the camera turns / the distance (at least 3) | all |
| Tether (5) | keeps last frame's direction from the player and is pulled along at 4..20 units; the pitch raises it; the turn keys turn it once | the camera turns / the pitch (-120..64) | pan, pitch |

- Distance is clamped to 2..200, height to 0..30.
- TOGGLECAM cycles 0 -> 1 -> .. -> 5 -> 0, and from camera 6 or 7 to overhead. FIRST_PERSON_CAMERA .. TETHER_CAMERA
  pick one.
- The mouse wheel zooms only between first person and camera 6; any other camera puts that camera's distance to 0.
- Zoom and camera turn keys act only in the chase and user cameras; pan and pitch in any but first person. Camera 6
  takes neither (it is this client's).
- CENTERVIEW levels the pitch in first person, resets the overhead's to -100 and points the others at the player; a
  pitch key or mouse pitch on a user camera stops that.
- The gamepad's camera button switches between first person and camera 6; its stick turns the player in cameras 1..5.
- Knockout selects overhead until consciousness returns. A drunkenness tick at 70 or more selects first person and
  widens the FOV by the value above 70.
- The missile camera (8) is not used.

Deliberate differences:

- The cameras keep the client's own field of view (ToggleView's +0x40, 90 by default).
- The user cameras are not saved: the original's loadOptions reads Camera2-* into camera 1's globals, so both reset
  each session anyway.
- Zoning does not restore the saved camera.
- The drunkenness widening is added to the player's own FOV (Options, /fov) and drops at once below 70 (or at death or
  bind), so the chosen FOV comes back exactly.

## Movement collision

#### The pass and placement

- `PlayerMovement::update` (app/player_movement.cpp) runs the physics Step (with the landing check), then
  `do_collision_movement` from the actor's location to the integrated position. The spawn's z is the feet plus
  AvatarHeight (3.125 x size x 0.2 for the player races).
- A placement (zone-in, teleport, height change) finds its floor from the placed z before the physics Step, and the
  first landing check stands the feet on it. Tested after the Step, a zone point on the ground itself (freportw ->
  ecommons) falls through to the floor below.

#### The swept sphere

The radius is `sweep_radius` (collision_movement.h): AvatarHeight x 0.5 x 0.95 (1.78 for a human). The type 1 volume
radius (`model_sweep_radius`, 6.68 for a human) cannot pass South Qeynos' doorways, hence this stand-in.

- The centre is above the spawn's z by 0 when ducking, dead or in a liquid; else 1.9, or AvatarHeight x 0.5 for races
  2, 9 and 10, and at least the radius.
- Steps lower than the sphere's bottom pass under it.
- With the stand-in radius the sphere's bottom can sit up to 0.12 above the spawn's z, so the floor test
  (`PlayerMovement::floor_test`) starts at the sphere's bottom when that is higher (a deliberate difference,
  `floor_probe_lift`). Without it a step slightly taller than AvatarHeight (the Bazaar arena's tiers) leaves the player
  hanging inside it.

Two things in eqgame point away from the type 1 reading: the centre's 1.9 fallback fits the player value, and
ChangeHeight keeps the volume's sphere radius (vf48) in step with size although only a type 2 volume reads it. The
static chain (CreatePlayerActor type 1, CHierarchicalActor vf44 -> 0x10045fd0, model +0x38) points to type 1, so only
reading vf44 in the running game can settle it.

#### Other spawns in the original

By the static reading, not ported; Hailstone's NPC collision below is its own rule.

- `CollisionFilter_Movement` lets a player's actor through, an NPC's only when its vfa8 or vf50 (a vehicle race)
  answers, a corpse never.
- The type 1 actor test (`CActorData::FindIntersectionSphere` @0x10045840) passes the bounding sphere and box, then,
  without collision triangles, tests only each bone's attachments (0x1003df50).
- Shipped skeletons have no per-bone meshes, so a body never blocks; held items and attached actors would.

#### The jump's crouch window

min(500, the duck track's length truncated + 1), or 500 without one (`jump_crouch_window`). The track
(`jump_duck_clip`) is slot 0 of row 0x18 (L08) on a classic skeleton, `CRCH_BA_1` on an EQG one.
Checked: 0x406740 (HasNewStyleHierarchicalModel, vtable +0xbc).

#### Sweep geometry

`game/sphere_sweep.*`: the sphere against a triangle (face, edge capsule, edge-region and vertex cases), the
zero-length overlap test, GetClampPoint, ComputeSkitter. It is Hailstone's own implementation in float arithmetic,
not the original's instruction stream, so it agrees with the recordings within a tolerance, not bit for bit
(docs/unknowns.md).

- An edge hit with no contact point (the sphere already holding the start vertex) uses that vertex.
- Capsule candidates keep cylinder, start-cap, far-cap order; one usable root selects its first candidate, two the
  smaller, negative time clamped to zero.
- A parallel edge-region approach is refused rather than divided by zero.
- Lengths are summed in double; a NaN length gives a NaN stop.

Tests: `tests/swept_sphere_test.cpp`.

`game/collision_callbacks.*`: the movement collision filter and the colliding-type setter. `game/swept_sphere.*`,
`game/collision.*`, `game/object_collision.*`: the collision library (no vectors).

- Triangles carry the passable (WLD 0x10, EQG 1) and transparent flags and n = normalize((v0 - v1) x (v2 - v1)); EQG
  winds the other way and is taken as (a, c, b).
- Passable never blocks; back faces (n . delta > 0) are skipped.
- The zone mesh is tested first, then the doors (a door hit sets CollidingType 3).

#### Doors

Every switch with an actor blocks (`game::door_blocks_movement`) except types 0x32 and 0x33, which
`CollisionFilter_Movement` @0x4760c0 drops.

Invisible switches block too: PostInit hides types 0x32, 0x36 and 0x39 with a door angle of exactly 1, but only the
line-segment queries (picking, line of sight) skip invisible actors; the movement sweep (0x100c92e0) skips only
`IsDisabled` ones. So open type 54 doors (crushbone's DOOR620X20) block, and hidden touch and floor switches still act.
Checked: PostInit's actor vf34(1), `IsInvisible` vf38, `IsDisabled` vf4; the DLL's line-segment filter 0x100080a0.

Placement (`game::door_transform`): R = Rz(Heading) Ry(-DoorAngle) Rx(0), counterclockwise, the actor at (Y, X, Z),
scaled by ScaleFactor %. Checked: CDisplay::CreateActor (actor vf74), CSimpleActor 0x10052af0,
CHierarchicalActor 0x10043160 (+0x21c / +0x220), `CActorData::SetEulerRotation512` @0x100c4200; against 21 classic
zones, 655 of 665 closed doors sit in their doorways (313 with the heading clockwise). Walked live in halas
and felwithea.

#### Animated zone objects

A placed object whose skeleton plays its own animation (Ssraeshza Temple's turning rings, swaying branches) is solid
where the animation has it (`game/animated_objects.h`; docs/formats/wld.md, "Placed-object animation").

In the original, `CS3DFile::CreateZoneActorFromActorInst` (0x1001cad0) gives a zone object volume type 1 only with
placement flag 0x80 (none shipped), and a WLD skeleton's line-segment test takes its per-part mesh path (0x100c66e0).
The sphere test is taken to work the same way (not traced, docs/unknowns.md).

Here:

- The moving triangles are kept out of `CollisionMesh`, per moving node.
- Each movement step poses every definition once (`set_time`: ms since the zone loaded).
- The player's sweep, floor test and a boat's hull probes meet nearby parts through placement x the node's motion.
- Other spawns' probes, line of sight, picking and the navigation mesh do not see moving parts.

Checked against Ssraeshza Temple's water shaft (`tests/object_animation_test.cpp`, needs the game files).

#### Vectors

**The current sweep has not been replayed against these recordings** since the capsule and parallel edge-region
changes; run `movement_vectors_test` with `HAILSTONE_VECTORS_DIR` set before trusting them (docs/todo.md).

| Function | Records |
|---|---|
| DoCollisionMovement | 1000 |
| SweptSphereTriangle | 3000 |
| SweepStopAndSlide | 3000 |
| s3dCollideActorSweep | 2000 |
| CollisionHit_SetCollidingType | 1500 |
| CollisionFilter_Movement | 2000 |

- The sweep's three compare within a tolerance: t 1e-4, unit vectors 1e-3, points 1e-3 + 1e-6 of their size.
- SweptSphereTriangle allows 1% disagreement (edge and vertex-region capsule cases). Records with garbage inputs (NaN,
  infinities, non-unit normals) are not failures; hit normals from the original's uninitialised contact point (an
  edge-case bug) are not compared.

#### Floor, ceiling and the world test

- The floor and ceiling queries skip every terrain triangle whose normal has z == 0 exactly, so walls never stop them
  (`SphereSweep::floor_only`); placed objects still block. A move's sweep does not skip them.
- The ceiling query refuses switch actors, so doors never give a ceiling (`MoveWorld::world_sweep`).
- IsPointInWorld: inside the terrain's box grown by the margin, the BSP walk stops at the first node with a region
  (region - 1) or a null child (-1: outside). A heightmap zone asks its terrain system.
- BSP regions also fill space under a floor with no geometry (South Qeynos' canal). A spawn put there sinks, every move
  is refused and the floor test reports none (-1e27).
- Testing (`--move-test`): `walk:D[:S]`, `escape` and `wander:S[:N]`; `+trace` logs each frame's sweeps and tests
  (PlayerMovement::set_trace).
- Not exact yet: the ceiling height, IsPointInWorld on EQG zones, the library's culls and AABB-tree order (equal-t
  ties).

Lifts and the floor:

- `switches.*` moves every door toward its state. A moving floor switch resets the CollisionCounter of spawns within
  its actor radius + theirs + 20. game/doors.cpp runs it each frame.
- RepopSwitch: a door of type 0x28 or 0x2d off its default state past its repop time (its parameter, else 20 s) sends
  an OP_ClickDoor with no player, which EQEmu answers by toggling the door. A deliberate difference: the original
  resends every frame until OP_MoveDoor returns (doors that open on their own at high frame rates); Hailstone sends
  once per repop period.
- `floor_height.*`: the cached floor, else a sphere of AvatarHeight x 0.1 swept down over the zone and floor switches.
  The floor is the hit's z - r + 0.001, -1e27 when nothing is below.
- A lift carries its rider only through this test (checked live: a type 0x3b lift).
- Vectors: ProcessSwitches 3000, GetPlayerFloorHeight 3000, CollsnCallbackForFloor 2500, FloorActorFilter 2500.

Room to stand (`can_i_fit_here.*`):

- After each player move that is not kept, the test takes the floor at the lifted position, the ceiling (cached when
  CollisionCounter > 2, else the ceiling height with the sweep radius) and the height needed: AvatarHeight + 1
  ducking, 3 x AvatarHeight on a ridden mount race, +0x144 when above 0, else 2 x AvatarHeight. F < C <= F + h + 0.5
  or C - F < h refuses, and the spawn is put back.
- `must_collide_still`: a jump animation (`is_jump_animation`: L03 / L04 classic, JMPA / JMPU new-style), levitation
  2 / 5, the auto-duck or a turning vehicle keep a still spawn colliding. A spawn not drawn (first person) counts the
  500 ms after a jump instead.
- Held in the air (Hailstone, a deliberate difference, `PlayerMovement::update`): a collision hold that keeps the
  player in place zeroes SpeedZ. Above about 126 fps gravity alone cannot move 0.01 in a frame, so a jump hung at its
  top. A body in ground fly mode, dry and more than 0.05 over its floor keeps its SpeedZ through a hold
  (`CollisionMove::held`). On the game clock this matters only with `game.fixed_step = 0`.
- Vectors: CanIFitHere 4000, MustCollideStill 2000.

Ladders (`game/zone_ladders.*`, `ladder.*`):

- **What a ladder is.** An actor of type 6: an object whose tag starts with LADDER. About thirty classic zones place
  them in objects.wld (LADDER14 .. LADDER60, solid slats 0.7 deep; Cabilis's LADDERCB101, every polygon passable).
- **Walking into one.** A kept sweep hit on a ladder sets CollidingType 6, zeroed on every moving step. At 6,
  GetEnvCoefficients returns (0.8, 0.8, 0.625, 0.625) before gravity and the run push's vertical part
  (sin(pitch) x SpeedRun) is kept: forward looking up climbs, looking down descends. Zone ladder triangles are marked
  `kTriangleLadder` and tested before the doors (the original's actor order is not traced).
- **Clicking one** (Ladder_MountClickedLadder): from the top (feet above 0.7 of its height), within 25, puts the
  player on it facing down. A zone ladder's heading is its placement's first angle, its height the top of its bounds
  (the actor's vtable +0x60 is not traced). A passable ladder is clicked like a solid one.
- **A passable ladder (LADDERCB101) in the original.** The sphere goes through it and never climbs it, so the canal
  ladders are left only by clicking from the top. Checked: the DLL's model sweep (0x100c5af0) skips a passable
  triangle unless bit 2 is set; the WLD loader (0x10057540) never sets it.
- **Passable ladders, Hailstone's own rule** (server `hailstone.cfg` `[movement] passable_ladders`, on by default,
  docs/plugins.md). A passable ladder triangle the sphere meets or starts in counts as the ladder's hit
  (`CollisionMesh::touches_passable_ladder`); the sphere still goes through, and the climb runs as for a solid one.
- **Solid ladders, Hailstone's own rule** (`hailstone.cfg` `[movement] solid_ladders`, on by default). Movement meets
  a solid ladder as one slab, the box of its solid polygons placed as the ladder is (`game::ladder_slab`,
  `CollisionMesh::set_solid_ladders`), for the sweep, floor test and CanIFitHere's ceiling; sight, picking, rain and
  the navigation mesh still see the rungs. Why: with the stand-in sweep radius a small race's sphere slips between
  rungs, and CanIFitHere takes rungs 3.7 apart as floor and ceiling, so ladders in Guk, Unrest and Ak'Anon could not
  be climbed.

## NPC collision

Hailstone's own rule, a deliberate difference (`game/npc_collision.h`). In RoF2 no spawn's body stops the player
(above, "Other spawns"); here a live NPC does.

- **Who blocks**: an NPC that is not a corpse, pet, swarm pet, mercenary, mount or vehicle race, and that the
  renderer draws.
- **Swarm pets** (`is_swarm_pet`): stock EQEmu sends them with no owner, so they are told by what `NPC::PetOnSpawn`
  sets: not cyclable (`targetable_with_hotkey` off), or the last name "<owner>'s Pet" (EQEmu's
  `Pets:ClientPetsUseOwnerNameInLastName`, on by default). Any NPC kept off the
  target-cycling keys is therefore passable too.
- **The body**: a 12-sided upright prism of the NPC collision volume radius (`collision_volume_radius`: AvatarHeight
  x 0.5 x 0.35), from 2 under its feet to twice its AvatarHeight above (at least 8). Walls only, in the movement
  sweep only (`ZoneMoveWorld::sweep`): the player never stands on an NPC.
- **Never held**: a sweep that starts inside a body (closer than both radii less 0.25) ignores it.
- **The switch**: server `hailstone.cfg` `[movement] npc_collision` (docs/plugins.md), on by default; off gives the
  original's walk-through. The player has no switch.
- The server moves NPCs without this rule, so one can still walk through the player.

## Boats

A deliberate design, not a port: only `vehicle.*`'s frame conversion is taken from the original; what is kept is what
EQEmu sends and expects. Code: `game/boats.*`, `app/player_movement.cpp` (the carry), `app/client_app_player.cpp`
(hull, zone-in hold, climbing aboard, helm), `net/game_connection_vehicle.cpp`; tests `tests/boats_test.cpp`.

#### What EQEmu does

- A boat is a vehicle-race NPC on a grid. Updates come on start, turn, speed change and every 5 s, speed in the
  animation field, no deltas; z is never fixed.
- A rider's OP_ClientUpdate with a vehicle id is a position in the boat's frame (ignored while the boat turns). The
  server never sends a vehicle id back: other riders arrive as world positions.
- Boats change zone by quest scripts (`quest::movepc`), which put the player at a fixed point, usually the dock.
- The warp detector measures world position, so a boat faster than the rider's run speed is logged (only logged) as a
  warp.

#### Riding

- **The boat's motion.** Stepped between updates along its heading at SpeedRun (0.58 x speed units a second, matched
  live). An update becomes a display error that fades (1.2 per second); a jump over 60 units is a teleport. Vehicles
  step before the player (`WorldState::smooth_vehicles`), so the player is carried where the boat is drawn.
- **Standing on it.** The floor test that finds the deck sets the vehicle (+0x150). Each frame body, velocity, heading
  and last good position move with the boat's drawn pose, and the position goes out in the drawn boat's frame.
- **The hull.** The drawn model's triangles in the boat's frame, retaken every 200 ms, are floors and walls.
- **Overlapping vehicles.** The nearest stopping triangle wins; decks level to 0.01 keep the current vehicle
  (`game::sweep_hulls`).
- **Rails.** While the carrying vehicle moves, a walking step off its deck over a floor more than 4 units below is
  undone. A jump, swimming or levitating goes over; a still boat has no rails.
- **Getting on and off.** Walking onto a deck is the floor test finding it. Swimming forward against a hull whose deck
  is at most 30 units above the feet climbs aboard.
- **Zoning.** While a vehicle within 80 units has no collision model yet, the player is held at the zone-in point for
  up to 5 s.
- **Other spawns.** One whose update lands it on a deck rides that boat with its own deltas (in the boat's frame)
  until an update puts it elsewhere.

#### The helm

OP_ControlBoat: EQEmu grants it for races 141 BOAT and 502 ROW and echoes it. Right-clicking the small boat you stand
on asks for it. At the helm the movement keys sail the boat:

| Motion | Rate |
|---|---|
| ahead | 14 units a second, at 7 per second each second |
| astern | 5 |
| turning | 40 heading units a second |
| no key | drifts to a stop at 3 per second |

- A step is refused, stopping the boat, when the waterline leaves the water or the hull meets the zone, a door or
  object, or another hull; the turn alone is still tried.
- The hull is 16 probes (radius 1, just inside the outline) swept from old to new pose; a probe touching something at
  its start is not held by it.
- The position goes out as OP_ClientUpdate under the boat's id ten times a second while moving.
- Leaving the deck, a zone change or a same-zone respawn lets go.

## Cast checks, chat, /who and /follow

Ports outside this directory that use its rules; vectors in `tests/rows2_vectors_test.cpp` (/who class text, spell
target-type-needs-target, language garbling, profanity filter, partial name lookup, follow AI).

#### Casting from a gem

`cast_gem` (`net/game_connection_cast.cpp`) runs `game::mechanics::cast::cast_spell` through `CastQueries`:

- The character's cached totals answer SPA 40 (174), SPA 312 (Sanctuary, removed unless beneficial on yourself or
  type 6) and SPA 324 (no mana check).
- OP_RestState's combat flag and downtime answer can-cast-in-combat / out-of-combat (9190..9194) and
  `IsRestingForFastRegen` (6460/6461), which also feeds the sheet's `fast_regen_resting`.
- Casting runs MakeMeVisible(0xb0, 1) unless the spell is a sneak attack or CastNotStanding; a sitting caster stands
  (ChangePosition 100).
- Line of sight uses the app's collision (eqstr 108).
- The target-type switch measures 2D distance against Range + focus (at most 5 x Range), MinRange (4602), targeted
  AE's 39.99 height and the spawn's body type.

The range focus (`Character::focus_range_modifier`, SPA 129) adds the best item, buff and AA focus, each Range x its
scaled base x 0.01 truncated toward zero. Checked: GetFocusRangeModifier @0x42cb80. EQEmu (Mob::GetActSpellRange)
sums them untruncated, adds the target's size and measures in 3D, so it accepts what the client lets out unless the
height difference puts the target out of reach.

#### Combat abilities and disciplines

`game/mechanics/combat_ability.h`: doCombatAbility refuses an ability whose reuse timer runs, and a duration self
discipline while one is active (5821). Reuse timers (ids 0..19) come from the profile at zone-in, then
OP_DisciplineTimer; the client never starts a stock timer itself.

/discipline, the combat abilities window and hot buttons all call doCombatAbility through its address, so a server
extension's `before_combat_ability` / `after_combat_ability` hooks run for each (for example reuse timers past 20).

Reuse is shared by timer id, id 0 included: a timer marks every ability using that id. Ability tiles and discipline
hot buttons show a faded red fill and a seconds countdown from that shared deadline; a shorter ability's overlay
stays full until the shared wait fits its own reuse (a presentation fix).

While a discipline is active, gauge 26 gives remaining ticks x 1000 / `CharacterZoneClient::SpellDuration`(spell,
level, false), 0 when that is not positive, capped at 1000. Checked: gauge 26 helper @0x761600,
PcZoneClient_IsDisciplineActive @0x5802c0 (its effect @0x5802a0), SpellDuration @0x434420. RoF2 gives this gauge no
text; Hailstone adds the seconds, interpolation between ticks, "Permanent" with a full bar, and a still bar while buff
timers are suspended.

Clicking the active icon follows `/stopdisc`: it requests removal and waits for the server's fade. In the gamepad UI
it is LB; navigation leaves the icon out so A cannot end the discipline.

#### The rank swap

In CastSpell and doCombatAbility, a non-GM casting a SpellGroup spell from a gem gets a lower rank when the
membership's tier limit is not -1 and below the spell's rank: that rank or the highest below it
(`SpellTable::by_group_and_rank`). EQEmu's gold membership sends -1, so no swap. Checked: GM test vtable +0x90
(@0x580300); tier limit PcClient::GetGameFeature_ @0x5784e0, feature 3.

AltAttack (PlayerZoneClient +0x340) stays 0 (only ever cleared or copied). The attack lock (0xddf70c) is set by
opcodes 0x57da / 0x4d09, which EQEmu never sends.

#### Chat text and melee lines

`game/chat_text.*`: the new-text filters, bad word filter, broken speech and language garbling (`ChatRandom`).

Chat codes (`game::expand_chat_variables`, CEverQuest::DoPercentConvert @0x51b600; unit tests in
`tests/social_test.cpp`, mock-server test in `tests/mock_game_test.cpp`), in either case:

- `%T` the target's name (`Target` without one), `%M` the pet's name (`Pet`).
- `%S` / `%O` / `%P` the target's pronoun (eqstr 12209-12211 / 12297, 12298, 6766 / 12301, 12299, 12300, the third of
  each also without a target).
- `%R` a player target's race (dbstr type 11, GetRaceDesc), eqstr 6767 for a corpse, 6768 for an NPC and 6769 without
  a target.
- A code is a `%` less than two bytes past where the last conversion left off, or after a space or an apostrophe. That
  point is two bytes past the start of the last expansion, so a code straight after a long expansion is not one
  (`%T%S` with a target "a gnoll" sends "a gnoll%S"). After eleven expansions every further code position gets `@`.
- They are converted when the line runs, in text the client sends: SendNewText's channels (say, a targeted say,
  group, guild, raid, fellowship, shout, auction, OOC, tell, emote), the chat-server lines (`;` and `[` lines,
  `/1`..`/10` after the bad word filter, a chat-server `/reply`), the whole line a social emote sends, after its period
  (send_social @0x528cf2), and the names of `/assist` and `/shield`. Social hot buttons run their lines as typed, each
  when it runs.
- The local echoes show the converted text: the channel echoes show the sent text; the emote echo and a social's own
  line are converted as shown lines (dsp_chat, where a `%` that is no code is left as it is). Received chat is never
  converted (msg_new_text shows it with dsp_chat's conversion off, @0x4c0e02).

Differences: the original sends `&PCT;` for a `%` that is no code and its chat window shows it as `%`; Hailstone sends
the `%` itself (docs/unknowns.md), and copies an item or achievement link whole, so a `%` in its label is never a code.
`%M` is the pet window's pet, where the original takes the first spawn in its list whose owner is the player.

Melee lines (`game::damage_report`, `hit_line_shown`; tests `tests/combat_test.cpp`) follow ReportSuccessfulHit's
chat filters; a filter hides when its mode is 1 (setting 0):

- A hit by somebody else on somebody else, under filter 15, shows exactly when the player's pet hit (and was
  reported).
- An NPC hitting itself is never reported.
- A miss on the player is hidden under 16, the player's own under 13; others' misses under 14 show only for the pet.
- The pet's lines take colour 0x148.

Handle_OP_Damage reports a hit between two others only within 75 units of the victim; OP_Death reports the killing
blow and death text only within 50 (`game::report_death`). Damage by or on the player always reports. Checked:
@0x4b5a8a.

**Extension** (hailstone.ini `combat.report_range`, unset by default): one player-to-victim distance replaces both,
for others' melee lines, hit sounds, killing blows and "X has been slain by Y". Unset, not a number or not above 0
keeps 75 and 50. Spell and server text are not affected. Stock EQEmu sends OP_Damage only within
`Range:DamageMessages` (50) of the attacker, so a larger value helps only where the server raises that. A character
setting (`servers/<profile>/characters/<world>/<character>/hailstone.ini`, or the main `hailstone.ini` as default).

**Deliberate change from RoF2 (a fix):** an unreported line stays hidden whatever the filters; the original swapped
the report flag for the pet flag under filters 15 / 14, showing the pet's out-of-range lines. Checked: @0x53092c,
@0x53078b.

An attacker is the player's pet when it is the focused pet (PetID), or has the temporary-pet byte set and MasterID
equal to the player's SpawnID; the summoned bit alone does not count. Checked: ReportSuccessfulHit @0x52efd3. Tests:
`tests/pet_combat_chat_test.cpp`; not tested live.

Temporary pets never replace PetID, so a swarm leaves the permanent pet's window alone. Checked: @0x59687a,
PlayerClient_LinkPetToOwner @0x4b9b10. Stock EQEmu writes the temporary-pet byte as zero and omits petOwnerId for
swarms, so those cannot be attributed; names and target-cycling flags do not establish ownership.

#### Social and /follow

- `game/social.*`: class titles and the /who class column, name trimming, partial name lookup.
- `game/follow.*`: the follow AI's step with `Fpu` (bearing via fpatan, an equal x with y1 < y2 takes the 270 branch;
  the turn snaps below 1, the gap below 4, 9 above 128, else 4; the bands). Its autorun / run mode writes go to the
  movement state.

**Deliberate change (switchable):** the navigation mesh helps /follow where the straight line fails
(`docs/navigation.md` "/follow"): the mesh's path steers with the same bands measured along it, and the follow keeps
on to 400 instead of 200. `hailstone.ini` `nav.follow=0` or `/nav option follow off` gives the original's.

## Listener, music, spell sounds and spell gems

Ports outside this directory that use `fpu.h`; vectors in `tests/audio_vectors_test.cpp`.

- `audio/listener.*`: the listener environment (ftol at the record's precision); SetListenerLocation's heading x 2pi
  x 1/512 through `Fpu`; fsin / fcos as the x87 computes them (reduction by its 66-bit pi, an operand of 2^63 or more
  returned unchanged, infinity the default NaN); room presets as tables.
- `audio/music_manager.*`: global music entries and active-entry selection (zone emitters, start-up, character select,
  combat, bank, merchant, trainer, death).
- `game/sound_rules.*` `spell_effect_stage`: a spell effect's stage sound and particle decisions.
- `game/spell_gem.*`: the gem window's tints and cool-down rectangle.

`app/sound_events.cpp`, a deliberate difference: another spawn's walking steps and loop are made only within their
reach plus 50 units (`audio::spawn_out_of_hearing`: 12 x height for a step, 24 x max(height / 2, 2) for the loop).
The original also plays a far spawn's muted steps, each holding one of pool 0's 16 voices. OP_Animation's sounds play
wherever the spawn is.

## Guild ranks, the AA filter, skill limits and task text

Vectors in `tests/rows3_vectors_test.cpp`.

#### Guild ranks

`game/guild_ranks.*`:

- Rank compare: b - a when both ranks are non-zero, else a - b.
- The permission table: dword[1 + rank * 2 + permission / 32], bit permission % 32, filled by OP_GuildUpdate action 5
  (+0x50 rank, +0x54 permission, byte +0x58).
- The local player's permission: no character, no; a GM, yes; else the rank's bit.

Used by:

- the chat filter: permission 0x13 for channels 0 / 17;
- /guildinvite: 7, else 5410;
- /guilddemote: 9 for another member, 0x1e for yourself, the rank checks unless a GM;
- /guildcreate: 4..27 characters, 6369 / 6370 / 6371;
- /guilddelete: the leader only, 9059; confirmation 0x6d with 9060, then OP_GuildDelete {guild id, own name} and 9061.

#### The AA window

`game/alt_adv.*`: the AA list in ascending rank id order, through `aa_filter_accepts`. "Passive" asks for the effect
list's first value. The "trainable only" box drives it; the kind, expansion and category combos stay at "any".

Deliberate differences:

- Once the selected rank is bought, the window selects the next rank, so Train buys it without another click.
- "Buy every rank" works on any server: one with `aa_train_all` gets OP_AAAction 4; others get OP_AAAction 3 per
  rank, the next when OP_RespondAA then OP_AAExpUpdate show the last owned. It runs until the last rank, too few
  points, a plain train or zoning; a refused purchase (unanswered by EQEmu) leaves it waiting.

#### Skill limits and the text overlay

- `game/training.*`: `pc_skill_limit` and `at_skill_limit` feed the trainer's 12930 refusal: the skill cap, the
  level-capped flag, SPA 263 / 248, and the current level (EQEmu's profile carries no other).
- `game/text_overlay.*`: one line, replaced unless the new one's priority is lower; fade-in trunc(t / in x alpha),
  hold counted from the start, then fade-out.

OP_TaskActivityComplete shows 6005 (colour 15, priority 0), or 6003 / 6004 with the task title (colours 14 / 13,
priority 1), alpha 255, 500 / 500 / 3000 ms, nothing in chat. An invalid type / index or another task id is dropped.

#### Task offers

A left click on `TSEL_PreviewRewardButton` needs a selected offer with its preview byte enabled, then sends the
`0x6471` action-4 request with the task id and leaves the offers open. Checked: `CTaskSelectWnd::WndNotification`
(@0x76b660), preview manager @0x463a90. Actions stay tied to the displayed offer when a packet replaces the list.

The description gets eqstr 6029 (`0x178d`, "This task begins in %1") appended when element slot 0 (the received
activity id, not packet order) has a type other than 0/-1 and a nonempty zone list; full zone names joined with
" or ". It ignores the preview flag and is not a reward line. Checked: `CTaskSelectWnd_ShowTaskDescription`
(@0x76b310), `TaskElement_FormatTargetNames` (@0x480780). Static tracing only; not tested live.

## Item moves and coin

`game/item_moves.*`; vectors in `tests/item_moves_vectors_test.cpp`.

#### Equip slots and use checks

`can_item_go_in_equip_slot`:

- No item or an unavailable slot: no. General slots 23..32: anything.
- Bags, books and augments: never.
- Range refuses types 27/28 and takes range > 0; ammo takes 19/27/28; otherwise the equip-slot bit.
- Primary, with the secondary filled, refuses a two-hander (1/4/35) and two instruments (23..26).
- Secondary refuses a two-hander, a weapon (Damage > 0, type not 27/28/43/44) without Dual Wield (skill 22), two
  instruments and a two-handed primary.

A drop on a worn slot then checks the class only (6611). An Attuneable item with NoDrop clear asks (confirmation
0x7a, eqstr 5912); yes is the ordinary MoveItem with NoDrop set.

`can_use_item`: class, then deity 6857, level 136, race 6856 (skipped for type 20), prestige; refusals print for a
player. Auto-inventory equips only what passes it.

#### Server moves

- `plan_move_item_apply`: a server OP_MoveItem moves only Possessions <-> Possessions: a swap, MoveItemCount onto a
  stack, AdjustItemStackCount for a stackable source and empty `to`. An invalid `to` deletes a possessions / bank /
  limbo item. A false result prints eqstr 1389.
- OP_MoveMultipleItems (`net::encode_move_multiple`): Ctrl with a bag under the cursor moves the cursor item, or a
  cursor bag's contents, into the bag's first free slots.

#### Coin

`move_money` (32-bit wrapping):

- Conversion (int)10^|dcoin| only with the bank window up.
- Onto the cursor: it must be free; sound 0x8a plays.
- To smaller coins the destination gets amount x factor if it stays within 10^9.
- To bigger coins only amount - amount % factor moves.
- OP_MoveCoin {from, to, 3 - coin, 3 - coin, amount asked}.
- The cursor coin type is the first cursor counter above zero.

`clicked_money_button`: cursor coin goes into the inventory as the button's coin; else a known amount comes onto the
cursor, or the quantity dialog opens with all of that coin.

On the merchant's Parcels page (0x698290, `GameConnection::parcel_coin_button`) the button is first outlined (+0x259,
0x6909a0) and the merchant's item and row are cleared (0x6f2120); any later SelectSlot clears the outline. The
original puts the answer on the cursor for its SendMoneyWnd. Hailstone (a deliberate difference: no SendMoneyWnd)
makes it the money picked for sending (`set_parcel_money`, `hs.Parcels()`' sixth value); the coin stays in the
inventory until Send money (inventory -> trade, then OP_ShopSendParcel), so none is lost or counted twice.

## Marketplace: the shelf after an open

A deliberate difference: on open Hailstone uses "All Items" as the current category, as the tree shows
(`game::MarketplaceWindow`); the rest follows the original (game/marketplace.h; docs/protocol/rof2-packets.md
"Marketplace"). The original sets index 1, "Player Studio", while highlighting "All Items", so the next refilter not
caused by a category click (a sort, a find) shows only Player Studio items. Checked: ResetView (@0x6e1df0),
SyncShelfToSelection (@0x6d8030), NavigateToEntry's fallback.

## Tradeskill window: Hailstone's combine batch

A deliberate difference: the original combines once per click. Hailstone's window adds what the character carries of
each component, how many combines that makes, and Combine / Combine N / Combine All (`game/tradeskill.h`,
`GameConnection::tradeskill_start_batch`). Each combine is the window's own OP_RecipeAutoCombine, which stock EQEmu
answers from the general slots and bags (HandleAutoCombine). `CombineButton` is unchanged.

- Counting (`recipe_needs`): SendTradeskillDetails lists an item once per one needed. The carried count is general
  slots 23..32 and their bags (not worn, cursor or bank). More than ten listed components shows "up to" N.
- Pace: one combine at a time, after the server's answer and the window's 2.1 s wait.
- Results: after a 400 ms settle, each cursor item arriving within 1.5 s of the answer or the last put-away is put
  away (`put_away_slot`): onto a stack with room, else an empty general slot, else a bag that takes it (`bag_fit`);
  never a worn slot.

It stops, said in chat ("Combining stopped after N combines: why."), when:

- the count is done, or the components run out;
- there is no room before a combine (`has_room_for_result`), or a result cannot be put away;
- the server refuses (0xfffffff5) or does not answer in 10 s;
- the window closes, or the recipe or list changes;
- coin or a player-picked item is on the cursor;
- a trade, an item job, the quantity dialog or death gets in the way.

## Spell gems and buff stacking

Vectors in `tests/spell_gems_vectors_test.cpp`, replayed call for call.

#### Gem and recovery times

- Gem timers (`SpellGem::ready`, gems 0..11 and 12 for items / AAs) and the recovery lock (`SpellState::recovery_ready`)
  are absolute client-time ms (`GameConnection::eq_time()`, low 32 bits of the steady clock), compared unsigned. The
  profile's spellSlotRefresh is ms remaining, made absolute on load.
- Linked timers (`SpellState::linked_ready`) are Unix seconds, indices 1..19.
- The profile's three timer arrays are copied as they are: discipline reuse (+0x69bc, `discipline_ready`), linked spell
  (+0x6a0c) and item recast by type (+0x6a5c). Checked: PcClient_ApplyBaseProfileData @0x5789b0.

#### Memorising

`get_spell_mem_ticks_left` runs once per draw of the book's memorise gauge:

- A tick when now - last > max(100, (levelNeeded - level + 5) x 100).
- Step 3 + Meditate / 10, Meditate = max(adjusted skill 31, 10).
- A bard (class 8) has base (CalcDEX + CalcINT) / 30 + 1, steps 2 x base with AA 286, else base + Meditate / 10 + 2.
- At 0 of 92: IsItemPending + 1, OP_MemorizeSpell {gem, spell, 1, ?}, last = 0, the book's gem and spell -1.
- The gauge reads ticks_left x 1000 / 92.
- Meditate is GetAdjustedSkill @0x449fe0 against the game's SkillCaps.txt. A zero cap returns only the racial bonus,
  so without the table trained Meditate reads 0 and every memorise takes the slowest step. When the game folder has
  no SkillCaps.txt the client uses the profile's trained value instead: the client's own fallback, as the original
  always has the file.

`finish_memorizing`: the gem gets the spell, eqstr 12007 in colour 264, gem timer now + RecastTime + 1000. Mode 1
hides the book unless it was open before, mode 2 goes on, else the cast window refreshes.

`handle_op_memorize_spell`:

- -1 / 0 / 1 / 2 each answer one pending request (0 finish scribing, 1 finish memorizing, 2 forget).
- 3 after a cast: the cast item's recast; then for gem <= 11 (signed) or 12 with a valid spell the gem timer now +
  RecastTime - reduction (gem 12: RecastTime - 1000 x SPA 295).
- For gems <= 11 also the casting gem 0xff, the recovery lock now + RecoveryTime (bards: now) and a twisting bard's
  next song.

#### Gem readiness and bard songs

- `is_linked_spell_timer_ready`: false without a record, true outside 1..19, else ready <= FastTime.
- `can_use_memorized_spell_slot`: gems < 8, then g - 8 < the SPA 326 total. No profile or profile +0x340c: all.
- The click needs now > gem timer and now > recovery; the tint needs not casting, a valid spell, now >= both and the
  linked timer ready. The focus reuse mod only shortens the overlay's total.

A gem click that cannot cast while a bard song plays stops the song (ClearBardTwist, StopCurrentSpellCast(0): "Your
song ends.") instead of "You haven't recovered yet..."; the next click casts. Songs stay cast after landing (EQEmu's
keepcasting), so this is how a bard changes songs (`GameConnection::cast_gem`).

#### Where a buff lands

`find_affect_slot`, in order:

1. A player caster's stack-blocked check: an existing SPA 148 blocks the SPA its base names in the slot its calc
   names, up to |max|.
2. A copy of the same spell is refreshed in place (a lower level refused unless SPA 444; SPA 67 / 101 / 113
   refuse), except a stackable DoT from another caster, which takes a slot of the same spell from a missing
   caster or the character itself, else the first empty one.
3. Each existing buff is compared slot by slot: SPA 254 ends, a new SPA 45 refuses, the SPAs ignored by stacking skip,
   SPA 446..449 refuse a smaller base; then the rules of SPA 0/1/10/161/162/332/416/444/450..452, then click-off, SPA
   58/156, SPA 101 and the special spell ids. Finally calc_affect_change old vs new decides overwrite / block / take
   the detrimental slot.
4. Else the open effect slot (songs from max - 21, the last slot for a discipline) and, for a beneficial spell, the
   first droppable buff.

Buffs of an unknown spell are cleared on the way. GetEffect gives slot 0 for an index outside 0..96.

Its only caller is the hit-by-spell path (`game/hit_landing.*`): no stacking is checked before a cast. The client
predicts where a buff lands when OP_Action 231 arrives (`HitLandingResult::placed_buffs`), and the server's OP_Buff
overwrites the slot.

#### Replacing a memorised spell

The original memorizes a book spell onto a gem only when the gem is empty. Checked: HandleSpellLeftClick,
StartSpellMemorization(slot, gem, bReplace 0) @0x75bf30.

Enhancement (option `replace_gem_spell`, hailstone.ini `spells.replace_gem`, Options > Display "Drop Replaces a
Memorized Spell", on by default; `UiHost::gem_takes_book_spell`): a usable full gem takes it too. The gem keeps the
old spell until 0 ticks; stock EQEmu's `Client::MemSpell` then forgets the old one (OP_MemorizeSpell 2, no "You
forget") and memorizes the new.

The modern book's double-click and Memorize button use the gem picked in its strip, else the first empty one; with
every gem full and none picked, the status line says how to replace one.

#### Cure counters

Locally placed buffs carry their effect slot data, counters included, into the buff window. The counters are
client-local: EQEmu's OP_Buff refresh sends `slot_data` as zeros. An all-zero refresh, or a full list that keeps the
spell in its slot, keeps them; a real value overwrites them.

A cure (SPA 35/36/116/369):

- touches only the first matching debuff;
- spends its own counter type first, then that buff's other counter effects (EQEmu keeps one pool per buff);
- never removes the buff locally; the server's fade does.

Surplus is not carried on, and counters unknown from zone-in stop the scan (docs/unknowns.md).

This is a deliberate change to a RoF2 rule: the original spends the first counter slot of the first buff and removes
it itself below 1, which against EQEmu's zeroed `slot_data` removed debuffs the server kept. The vector replay keeps
the original rule (`HitState::rof2_single_slot_cures`). Checked: HitBySpell @0x43e8c0, GetPCSpellAffect @0x433fd0,
RemovePCAffectex.

EQEmu fades a fully cured buff (OP_Buff fade 1) before the cure's OP_Action (`Mob::SpellOnTarget`), so a fade of a
counter type in the same network update skips the prediction for that type.

#### Feign death by spell

SPA 74 (Imitate Death, Death Peace) ignores the spell and base: it clears the bard twist, stops the cast and calls
ChangePosition(0x73), which lies the player down, stops auto attack and sends OP_SpawnAppearance {me, 14, 115}.
Checked: HitBySpell -> CharacterZoneClient_StopCastAndFeignDeath @0x433b80.

The landing reports it (`HitLandingResult::feigned`) and `GameConnection::land_spell_on_me` does the rest; EQEmu's
SE_FeignDeath sends no appearance, so the client's own lies the character down for everyone.

Lying feigned (StandState 115, `CharacterInput::lying`), the player cannot move:

- ProcessControls moves, turns, strafes and jumps only standing (100) or ducking (111).
- The forward key's auto-stand stands only a sitting player (110).
- A feigned player stays down until /sit or /stand.

## Loot All

Loot All visits bags first, then other items, in corpse-slot order within each pass, the player's own corpse
included. Its guard ignores activation while its list (+0x3a0) exists; neither it nor the button looks at an
individual loot request awaiting its reply. Every OP_LootItem reply, an individual loot's too, advances the list by
one, and the list is not checked against the corpse, so an item asked for just before Loot All is asked for again
(as in the original). Checked: CLootWnd::LootAll @0x6bdba0, guard @0x6bdbf5, WndNotification @0x6be180,
Handle_OP_LootItem @0x6bd414.

Hailstone's own additions:

- While a Loot All is active the button does not open the "are you sure" question, whose yes would be ignored.
- While that question is up or queued, a second click does not ask again (whether the original would is not traced).
- A loot request that cannot be sent cancels the queue, so the player can retry.

The player's own corpse skips the Loot All, NO DROP and stack quantity questions: a player corpse whose name before
`'s corpse` is the player's. OP_Death renames the dead spawn as PlayerZoneClient::IDied @0x5a3d60 does, with the
packet's corpse id: a player `Name's corpse<id>`, the name EQEmu gives a player corpse it spawns, so the corpse left by
a same-zone resurrection is the player's too; an NPC `name's_corpse<id>` (MakeCorpseName @0x4dcad0: a digit among the
name's last three characters ends it; an LDoN object, class 62, `name_<id>`).

A NO DROP item needing confirmation is left out of the queue and opens the first question during the scan
(@0x6bdf93). Another such item aborts the queue but leaves that question alive for its slot. A loot question pending
before the scan stays unchanged.

Closing after Loot All waits for every outstanding loot reply. The original's close timing for that overlap is
untraced (docs/unknowns.md).

## Leaving an NPC interaction

Each service window closes when its NPC is more than 20 units away in 3D (exactly 20 stays open), or the NPC or local
player is missing. Hailstone checks on connection updates, so both UI packs behave the same. Checked: each window's
OnProcessFrame calls PlayerClient_IsWithinInteractRange (npc, 0, 20.0) @0x5a06f0 (PlayerDistance, z scale 1).

| Window | Original | Timer | Exception |
|---|---|---|---|
| Merchant | CMerchantWnd::OnProcessFrame @0x6f2400 | 500 ms | stays while a buy or sell awaits its answer |
| Bank | CBankWnd::OnProcessFrame @0x627850 | 500 ms | |
| Barter buyer | CBarterMerchantWnd::OnProcessFrame @0x62a500 | every frame | |
| Alternate currency merchant | CNewPointMerchantWnd::OnProcessFrame @0x6fba10 | 500 ms | |
| Adventure merchant | CPointMerchantWnd::OnProcessFrame @0x719e90 | 500 ms | |
| Tribute master, personal and guild | CTributeMasterWnd::OnProcessFrame @0x787ae0 | 500 ms | see note 1 |
| Trainer | CTrainWnd::OnProcessFrame @0x7812a0 | 500 ms | |
| Adventure recruiter | AdventureMgr_Process @0x41fa80 | more than 500 ms since the state was set or last checked | see note 2 |
| Mercenary liaison | the window's observer 0x6eb560 | every update (see docs/unknowns.md) | only with a liaison id set |
| NPC hand-in or player trade | CTradeWnd::OnProcessFrame @0x780b50 | 500 ms | reach is the partner's avatar height plus 20; exactly that stays open |
| World tradeskill station | CContainerMgr::Process @0x65d740 (0x4dc160) | more than 500 ms since the last check | the ground item's 3D distance, at most 20; a missing item closes it too |

1. Tribute master: out of reach it stays while an item request is pending (IsItemPending > 0); a missing master
   closes it regardless.
2. Adventure recruiter: only in states 1 and 2 (the recruiter's words or an offer up); on an adventure it stays.

#### How each window closes

The normal way: the merchant's OP_ShopEnd; the buyer's end of inspection; the trainer's OP_GMEndTraining; the trade's
OP_CancelTrade (the server returns what was offered); the station's empty OP_ClickObjectAction; the adventure
recruiter declines an offer that is up (CAdventureRequestWnd::AboutToHide @0x618420, 0x41fe10: OP_LDoNButton 0).

Closing a tribute master leaves benefits, favour and outstanding donation replies alone. The trainer and trades also
close when their partner dies (Handle_OP_Death @0x532b50).

#### Trades out of reach

Player trades use the hand-ins' 3D distance check, whoever moves; only an open trade is checked. Cancelling credits
nothing locally; the server's item packets and OP_MoneyUpdate return them.

The client that cancels says nothing; the lines come from the server's OP_CancelTrade: 1448 "<name> has cancelled
the trade.", and 1450 "You have cancelled the trade." for the receiver's own id. Checked: CTradeWnd_CancelAndClose
@0x780af0, Send_OP_CancelTrade @0x780550, Handle_OP_CancelTrade @0x4b7710. Stock EQEmu forwards a player trade's
cancel to the partner with the partner's own id, so whoever cancels first gets no line and the other reads 1450:
walking away closes silently, being teleported away usually shows the line. Both are the original's behaviour.

Hiding the trade or Give window cancels the trade it shows (CTradeWnd::AboutToHide); hiding the other leaves it alone.

#### Late replies, the bank and sessions

A late open reply cannot show a window out of reach: the tribute master's (CTributeMasterWnd::AboutToShow @0x7878b0)
and the trainer's (Handle_OP_GMTraining @0x4b49c0) apply the 20-unit check to the answer. Hailstone releases a station
whose open reply comes out of reach at once (the original at its next 500 ms check); a busy reply ends the request.

When the bank window hides, every container window closes, carried bags included; the world container is released,
the tradeskill window and quantity window hide, and both "all bags" keys go back to opening (0x697e40). Checked:
CBankWnd::OnShow @0x627760 -> CContainerMgr::CloseAllContainers @0x65d4e0.

Quantity questions, point-purchase confirmations and guild-bank text entry end with their owning session; reopening
the NPC makes a new one. World-slot questions and world-mold confirmations close with the station, and a world recipe
batch stops; carried tradeskill bags stay.

Hailstone's IsItemPending (GameConnection::transact_state) counts item clicks, casts, memorizing, augments and the
guild bank's requests. A tribute donation does not raise it, in the original either (Send_OP_TributeItem @0x484190,
Send_OP_GuildTributeDonateItem @0x484870).

Not done yet: CTradeWnd skips the trade-distance check while the merchant window shows its page handler 2; Hailstone
does not.

## The merchant's "Show only items I can use"

The purchase page's MW_UsableButton is ticked when the window is made. While ticked, the list
(`game::merchant_usable_hides`) leaves out:

- a common item (class 0) whose class mask lacks the character's class, or whose race mask lacks the race's bit
  (0x6f7160: races below 13 as they are, Iksar 13, Vah Shir 14, Froglok 15, Drakkin 16, any other 17);
- a spell scroll (type 20) whose spell is known, or whose group has a higher rank known.

Bags and books always show; level, deity and lore are not looked at. Checked: PurchasePageHandler::UpdateList
@0x6f0bb0.

A deliberate change from RoF2: the original's SpellBookInfo::IsSpellKnown @0x449cd0 looks only in the 720 book slots
(PcProfile +0x2520), so a learned discipline tome stays listed. Hailstone also counts the profile's discipline list
(kept current by OP_DisciplineUpdate).

A Hailstone extension: a server-supplied multiclass mask lets any class in it match.

A click flips the box for the session. The modern pack's box is "Usable only" beside "Affordable only"; the gamepad
pack's Buy page switches it with X.

## Kept items: Hailstone's Keep mark

A deliberate difference with no counterpart in the original: the Advanced Inventory window (`InventoryManagerWnd`,
`ui/modern/inventorymanager.lua`, opened from the EQ menu and the inventory window) marks items Keep or Sell by item
id, so every copy has the mark; one clears the other.

- Sell is what the merchant's Sell marked sells (`hs.SellJunk`; "junk" in the code and settings).
- Keep is client-side (`game/kept_items.h`, `GameConnection::set_item_kept`), saved per character in `kept_items.txt`.

While an item is kept, or a bag holds one, the client refuses, saying why in chat:

- destroying it (`destroy_cursor`, before the confirmation);
- dropping it on the ground (`request_drop_cursor`, `drop_cursor_on_ground`);
- putting it in a trade or NPC give (`trade_drop_cursor`);
- selling to a merchant, point merchant or adventure merchant (`merchant_sell`, `point_merchant_sell`,
  `adventure_merchant_sell`); Sell marked leaves kept items out;
- sending it by parcel (`send_parcel`);
- selling to a barter buyer, posting a bazaar offer or offering it as buyer compensation;
- donating it to a tribute master.

Nothing goes to the server, so stock EQEmu needs nothing. Checks run again at confirmation.

Marking an offered item Keep ends the active trader or buyer session; an unpriced satchel item does not.
`hs.SetItemsKept` validates a batch, then saves it and bumps the Keep revision once.

## The bank as last seen

A Hailstone addition. EQEmu's `Inventory:LazyLoadBank` (on by default) leaves the bank and shared bank out of
OP_CharInventory and sends them (type 0x67 item packets) only when a banker comes near (Mob::IsCloseToBanker), so
after most zone-ins the client does not know the bank, and the Advanced Inventory and `/finditem` would find nothing
there.

The client keeps a copy (`game/bank_snapshot.h`, `GameConnection::bank_seen`), saved per character in `bank.dat`. It
is taken when the bank is known and about to be lost or finished with: the next OP_CharInventory, the bank window
closing, camping and disconnecting. The bank counts as known (`bank_live`) from an OP_CharInventory with bank items, a
bank-slot item packet or the bank window opening; not known from an OP_CharInventory without any.

While the bank is not known, `hs.AllItems` and `hs.FindItems` list the copy, its rows marked `stale`; `hs.BankSeen`
says how old it is. The window shows them dimmed ("Bank as last seen ...") and refuses to move or use them. The copy
never enters the inventory, so nothing can act on it, and it may be out of date (the shared bank, for one).

## The Advanced Inventory's bulk actions

Also Hailstone's own: selected rows can be sold, donated as tribute, destroyed or withdrawn from the bank
(`GameConnection::start_bulk_job`, `hs.BulkItems`), one item at a time, sending what a player's clicks would.

- Sell and tribute take only carried items; destroy also banked items while the bank is open. These three exclude
  kept items. Withdraw is not blocked by Keep. Nothing worn is taken.
- Sell, at an open merchant: OP_ShopPlayerSell for the whole stack, the next after the answer. Not a bag, nor what the
  merchant refuses (`game::sell_refusal`).
- Tribute, at an open personal or guild master: each item is donated from the cursor (OP_TributeItem), the next after
  the answer; a refused one (0 favour) goes back. Items worth no favour and bags are left out. Changing or closing the
  master, or no answer in 5 s, stops the job.
- Withdraw, at an open bank: each item goes through the cursor into the first free general slot, else a bag with room
  (`stow_cursor_in_packs`, never equipped). A bag and its children move once. Closing the bank stops the job.
- Destroy: each item is destroyed from the cursor (OP_MoveItem to nowhere), with one confirmation for the batch. A
  non-empty bag and No Destroy items are left out, rechecked just before each pickup.

The window shows beforehand how many items each action takes. A job stops, with a chat line and the cursor put away,
if the items change under it. The selection records each item's ID, serial and quantity, which must still match, so a
replacement in the same slot is not touched. Trader Add and the buyer-start acknowledgment also recheck Keep.

#### Pending tribute donations

One unresolved item donation is allowed across personal and guild tribute. Neither reply has a transaction ID, so a
timed-out donation stays pending, and another waits for the answer, a new zone/session, the same kind of master's
window being answered again, or 30 seconds. Stock EQEmu answers in the handler or not at all, so a later master's
answer comes after any earlier one. Closing the window alone keeps waiting.

A reply reconciles only one unchanged original instance, even if moved. Replacement cursor items and changed stacks
are kept, with a message to refresh by zoning.

## Using items

`net/game_connection_item_use.cpp` (tests `tests/item_use_test.cpp`, mock server).

#### Right click and use

`right_click_item` is a quick right click; the held button shows the item display.

- Nothing while IsItemPending, for an invalid slot, the inspect container or a bank slot past the window's count.
- Alt is the item preview; Ctrl + Shift on a trader's slot selects it.
- While casting only BardCastBard lets it through.
- A corpse slot loots (RequestLootSlot(slot, 1)).
- A container opens or closes (a placeable one: 5311).
- In the possessions, a book or note is read (OP_ReadBook) and a common item is used.

`use_item` (also the useitem command and the potion belt):

- An item whose click spell is not CastNotStanding stands a sitting player up when auto-stand is on, else 12441 (300
  for a skill) and, feigning, 12368, and nothing is sent.
- Then IsItemPending + 1 and OP_ItemVerifyRequest {slot, target}. The client checks no level, recast or charges.

#### The verify reply

`handle_item_verify_reply`: every reply but slot 0x407's takes back IsItemPending; a negative spell stops there. By
item type:

- Food and drink (types 14 / 15) at 6000 or more: 12074 / 12077.
- Other food and drink: 8501/12075 (8502/12078) with the name; hunger or thirst + FoodDuration x 100 / 2, up to 6000;
  drinks play wave 0x95; 12076 / 12079; OP_Consume {slot, 0xffffffff, 1 / 2, 1}; one item less.
- Alcohol (38) under 200 drunkenness: 12081; drunkenness + ftol(max(0, 270 - Alcohol Tolerance) x (1/270)f x 30 + 10),
  up to 200; OP_DeleteItem {slot, -1, -1}; 12079; one item less. Otherwise 12080.
- Poison (42) runs `apply_poison` (below).

Then the click effect (the reply's spell, else the item's), unless a charm's stat scale is below 1, is cast with
CastSpell(gem 12, the item) when nothing is being cast or the class is Bard; potions play wave 0x95; a retarget is
undone. That CastSpell sends nothing (send argument 0, @0x4bff60): it only sets the recast timer or cast bar. The
server already cast the item on OP_ItemVerifyRequest; a second OP_CastSpell would cast an instant clicky twice.

CastSpell's item checks refuse silently for an item cast:

- click level (block 0 +4, the wire's level2) above the level (136);
- an Attuneable item whose NoDropFlag is clear (7586);
- click type 4 not in a worn slot 0..22 (179);
- for types 4 and 5, CanUseItem(item, 1) (the server extension's "all usable" first).

So an unattuned Attuneable clicky or a must-equip one in a bag starts no cast here while EQEmu casts it anyway: only
the server's "You begin casting" shows. Equipping attunes it (the 0x7a confirmation).

#### Applying poison

`apply_poison`:

- Without the Apply Poison skill (6): 12383.
- A standing player sits (ChangePosition 110); not sitting then: 12381.
- Nothing while already applying, camping, quitting, interacting or with an item request pending.
- Else the start time and the top-level slot are kept and 12382 is said. A poison inside a bag names its bag, so
  EQEmu answers failure and keeps the poison.

The periodic update (not camping, interacting or pending) sends OP_ApplyPoison {slot, -1, -1, unknown, success: 0}
once more than TotalEffect(SPA 234) ms have passed (10000 without it), and sets interacting and IsItemPending to 1.

Any stand state change cancels it (12497); zoning clears it. The reply says 12905 on success, else 12906 (colour
0x10e). The Apply Poison ability (UseSkill 6) only starts its timer; EQEmu does the skill-up, deletes the poison and
adds the proc (only with a 1H piercing primary).

## Mouse picking and clicks

`game/clicks.*` (game side), `game/picking.*` (rendering side); vectors in `tests/picking_vectors_test.cpp`.

#### Pick masks and the ray

- Masks: NPCs 6, every other spawn 2 (corpses too); | 8 for bClickThrough (bit 31), a mercenary with /clickthrough
  4, and a player's pet with /clickthrough 2 (which also clears bit 8 when off). Doors and objects 2 (type 0x37
  doors 0). Clicks need bit 2 and skip bit 8; hover skips nothing.
- The view actor (and its mount) is excluded only in first person or with /clickthrough bit 0.
- The ray: k = cos/sin at trunc(h/2) & 511 folded into 0..127 (1e30 at 0); camera direction
  ((x - cx + 0.5) / (cx k), (cy - 0.5 - y) / (cy k (h/v)), 1), rotated by the camera, normalised (zero below
  FLT_EPSILON), times Yon, from last frame's camera position.

#### The four passes

Mode 0 with passable triangles blocking, mode 1, then both with them ignored, until the nearest actor has the
required bit. In a pass:

- World triangles first (back faces skipped; a hit needs dist^2 <= FLT_EPSILON; world best = nearest t).
- An actor is accepted when its t <= world best and < the actor best (first tested wins a tie).
- Type 2 actors, and mode 1 actors whose model definition has +0x5c, are spheres ranked by centre distance.
- Others are tested by triangles behind a bounding-sphere gate; the static mesh path takes t x 0.99 and skips
  n . dir >= 0.
- In mode 0 a hierarchical actor must also touch a bone sphere (bone midpoint with its child, radius max(half length,
  0.25)).
- The hit point stops 0.001 short.

The client tests the zone mesh (CollisionMesh::for_each_on_segment), each spawn's last drawn pose
(ActorRenderer::spawn_pose), a spawn drawn as a placeholder box as a sphere, and the doors' and objects' solid
triangles. Every pick is logged ("pick (left click) ...").

#### Left clicks and switches

Left click on a player: the clicked spawn becomes the target unless its race is below 2253 and it is not targetable;
then the "who is this" lines ([Defaults] LeftClickTargetHelp, default 1: 6780/6791, then class / gender); then the
cursor item is given.

Touching a switch (`game/mechanics/touching_switch.*`; checked: PlayerClient::TouchingSwitch @0x5939e0) has three
triggers:

- The movement sweep's hit on a touch door (types 0x33, 0x73, 0x74, 0x78, 0x7d, 0x82, 0x87, 0x8c, 0x91, 0x92,
  0x96..0x98) acts at once.
- The floor test marks a floor switch less than Height + 2 below the spawn, handled at the next collision pass.
- A non-colliding spawn is overlap-tested against touch switches with half its radius.

Touch types:

- From 0x73: hurts (GetSwitchDamage, OP_EnvDamage type 0xfd) at most once a second per switch, every 400 ms when the
  repop time is under 1 s.
- 0x23: used by nobody on a roll above 50, at most once a second.
- 0x39: used every 2 s with the cursor item as key, only when the zone point's zone has a `.s3d` or `.eqg`, or the
  door has an adventure, dynamic zone or script id. EQEmu sends none of these and parameter -1, so its touch pads
  never fire.
- 0x32: used closed, and put back to closed past its repop time.

Checked live with a spear trap. Clicked switch: trap types use adjusted Disarm Traps (17) with key -1; a type 12
common item is the key with adjusted Pick Lock (35); any other cursor item is the key with skill 0.

#### Right click on a player

In the original a trader opens its shop, a buyer its barter window, and any other player is inspected with
/toggleinspect on. Checked: CEverQuest::RightClickedOnPlayer @0x536d80.

A deliberate difference (`game::opens_player_menu`, `app::player_menu_rows`): right-clicking another player who is
not a trader or buyer targets them and opens a menu: their name, Send Tell, Invite to Group, Invite to Raid, Trade and
Inspect. Each row runs its command's path (`/invite`, `/raidinvite`, the trade request, `/inspect` regardless of
/toggleinspect) and does nothing once the player has left. Invite to Group is greyed while we hold a group invitation
(`/invite` would accept it).

Options > General "Player Right-Click Menu" (hailstone.ini `mouse.player_menu`, on by default) switches it off. With
context menus off (the ContextMenus option, or the player down) the original's right click happens; the gamepad's
Interact keeps the original's.

## Death, teleports, the respawn at bind and camping

Code: `game/death_report.*`, `game/zoning.*`, `net/game_connection_death.cpp`; vectors in
`tests/zoning_vectors_test.cpp`.

#### OP_Death

- The killing blow is reported unless neither side is us and it died more than 50 away.
- A tracked dead spawn ends tracking with one OP_TrackTarget {u32 0} and message 12102 (colour 0x111).
- Every death removes that spawn from the cached tracking list, the player's own entry included; an unrelated tracked
  spawn stays tracked.

#### Zone lines

- The region change hands the teleport the region's text. An ATP region's text is "DRNTP00255%05d0..." of
  atoi(name + 4), which reads back as zone point 10 x N.
- The teleport index resolve reads the zone (5 digits), then each 6-character field only when the text is long enough.
- For zone 255 it takes the first zone point row with that number; with none, a zeroed position and failure.
- Four zone pairs adjust the destination: 34 -> 10: world y - 1044; 10 -> 34: world y + 1044; 3 -> 4: z + 2; 2 -> 4:
  world x + 10. The first three also apply the offset to the position reported before zoning, which supplies any
  coordinate EQEmu keeps across the line. Change to a RoF2 rule: the original writes the offset into the player's own
  position, so a refused request left the player displaced in the old zone; Hailstone leaves the player where it
  crossed and reports that position again once the request ends.

#### Heightmap zones' ATP boxes

The DAT area's zOffset is the box centre's z as stored, with no terrain height added. Checked:
CEQTerrainSystem::vf3 -> CTerrainSystem vf42 @0x100eb380 (adds only the tile origin to x and y).

In toxxulia, mistythicket and oldbloodfield (post-RoF2 files) every ATP box lies 200 to 600 under the ground, so the
original never crosses a zone line there; the fallback below does. Their x and y match the server's zone points 10 x N
(EQEmu's x, y are the client's y, x). EQEmu's virtual zone points (`zone_points.is_virtual`, `width`, `height`) are
the server-side fix.

#### Buried zone lines

A client addition, not a port (`survey_zone_lines` / `find_buried_zone_lines` in `game/zone_lines.*`). At zone load
each zone-line box of an EQG zone is sampled over its footprint (its cross-section at the centre's height; up to 16 x
16 cells at least 4 wide) against the zone's collision. A box is buried when all three hold:

1. No sample has a floor inside the box or less than 50 under its bottom (a cave or tunnel with a real floor keeps it).
2. Ground lies over at least 75% of the samples, every one at least 100 over the box's top (a pit or void keeps it).
3. At 75% of those the top is inside the ground: more upward-facing surfaces over it than undersides (not a roof).

For a buried box, when the feet are in no zone line of their own, the trigger tests the feet's x and y against the
footprint, with z inside from the box's bottom to 50 over the highest surface over it; the rest runs as for any box. A
buried box the player zones in over does not fire until the feet leave its footprint (mistythicket's arrival from
runnyeye).

Every other box and BSP zones are unchanged. The load logs each buried box ("zone line ... is buried");
`EQGDUMP_REGIONS=1 eqgdump --zone NAME` prints the survey. Over 358 EQG zones 39 boxes are buried (toxxulia,
mistythicket, oldbloodfield and 22 others); the void and pit teleporters and boxes under a roof are kept.

#### Do-teleport and do-teleport-B

- Do-teleport: a row with a nonzero +0x18 sends 0xcadf {u32 zone point} and waits for the server's 0xcadf (if already
  waiting, it only clears the flag). Otherwise: do-teleport-B(zone, the destination through the FPU, reason 0, -1, 0).
- Do-teleport-B:
  - refused while frozen (0x66) or link-dead;
  - reason 0 sends the position first;
  - coordinates below 999999 and a heading below 513 are taken;
  - another zone (unless GameState 1): z + 1 for races 8, 11, 12, then MoveToZone(zone, save 0, reason, x, y, z,
    (int)heading, arg8, 2 for reasons 15/17/19 else -1, arg9);
  - the same zone: a local move. The floor comes from the player floor height at (x, y, z + 1) unless exactly
    -9.99999988e26 (an 80-bit compare). Colliding type, standing-on and touching-switch are cleared.

#### OP_ZonePlayerToBind

Ignored at 20 bytes or less. Otherwise:

1. The respawn window closes; 539 with the zone name (colour 13).
2. PrepareForRepop: buffs, vitals to max, drunkenness 0.
3. Vitals from the packet, air to max, gems refreshed.
4. The same zone (the whole dword with the instance): death music off, rewind reset, the position list sent, no
   move. Another zone: MoveToZone(zone, save 1, reason 10, x, y, z, (int)heading, -1, -1, 0).

A deliberate difference after a same-zone hover respawn: mana and endurance are not set to max, and the packet's are
taken only when not 0. EQEmu's RoF2 encoder writes 0 placeholders there, and its resource updates can arrive first.

The respawn window's Select waits for the packet's +8 delay and an available selected destination, then sends
{u32 option} once. The option's final byte is availability, not a resurrection kind. The server resolves the timeout.

#### Same-zone hover respawn

Completion waits for the living own spawn with its new ID before releasing movement.

- The old corpse stays. The new spawn's position, heading and fresh physics body replace the movement state; pending
  isometric click-to-move is cancelled.
- Resurrection confirmations take input priority while the respawn window stays drawn.
- Yes to an offer for the current zone (zone dword with instance) sends no OP_RezzAnswer: it enables the reserved final
  resurrection row beside bind, and nothing is committed until Select. Checked: ConfirmationDialog_HandleResponse
  @0x52d540 case 0x67; 0x74e9e0 enables and greens the lines.
- Yes to an offer naming another zone or instance sends OP_RezzAnswer at once.
- Yes does not move the selection; the row keeps its "r" marker.
- The original also requires the offer's corpse name to match; that is not compared (docs/unknowns.md).
- Resurrection text is red while unavailable, green while available. No declines at once and leaves bind usable.
- The server's final-row ordering identifies resurrection. A replaced offer or changed destination invalidates
  consent; pending dialogs drop on completion, zoning or disconnect.

Tests: `tests/hover_respawn_test.cpp`, `tests/player_placement_test.cpp`; protocol in `docs/protocol/rof2-packets.md`.

#### Camp

/camp sits, sends OP_Camp and prints 12293, then 12684 every 5 s. It ignores an open trade.

- A second /camp or a position change (Stand, the sit key, moving forward while sitting) cancels it (12290). Any
  other stand prints 12683.
- At the end the position history goes (when not riding), then OP_Logout.
- A zone change ends a camp silently. Checked: EnterZone -> ResetClientStateForZone(1) @0x5319f0 clears 0xddf8a0 and
  0xdd2664.
- The window's close button in game is a /camp desktop (HandleWindowCloseRequest).

Deliberate difference (always on, `app/close_camp.h`): when a zone change ends that close-button camp, the client
camps to the desktop again once the next zone shows. A camp the player ended is not made again.

Enhancement (option `fast_camp`, hailstone.ini `game.fast_camp`, Options > General "Fast Camp", off by default):
OP_Logout follows OP_Camp at once. A stock EQEmu zone ends a camp cleanly only after its 29 s timer (a GM's at once);
earlier is a disconnect, leaving the character link dead for `Zone:ClientLinkdeadMS` (90 s by default).

Deliberate difference (always on): a mounted player camps from the saddle without sitting; getting off cancels it
(12683). The original refuses ("You must dismount and be sitting": 12291, or 12292 when sitting). Stock EQEmu's
`Handle_OP_Camp` checks neither.

#### Saves and the position history

- Saves (`Save` @0x52ae60): refused while interacting or with an item request pending, which sets the save counter to
  300. The 6 s tick counts it up and saves (kind 0xf) above 300, about every half hour.
- Position history (`game/position_tracker.*`, OP_FloatListThing 0x46c6; EQEmu's cheat checks expect it at least every
  70 s): samples {Y, X, Z, reason, time's low word, entry count}. Reasons:
  - 1: a move that keeps its position (not again at the same spot within 1 s); sends samples older than 2 s, at most
    once a second.
  - 2: every OP_ClientUpdate, which carries the sample count as its sequence.
  - 4: a zone line (5 when its teleport fails).
  - 3: DoTeleportB for a local move. Spells record 3 / 7 / 9.
- Reasons 3, 4, 5 and 8, /quit, /exit, a zone change, the player's death and the camp's end send everything at once;
  the same-zone bind flushes it.

## Spawn heights

`game/spawn_heights.*` (on `fpu.h`); vectors in `tests/spawn_heights_vectors_test.cpp`.

`set_heights`, in order:

1. Length 0. A Height not above 0 (a NaN is kept) takes racedata.txt's default: column 89, or 90 for gender 1; 6.0
   for a missing row or a value below 1.
2. AvatarHeight = ROffset (moddat.ini `[code] ROffset`, 3.125) x the scale, unrounded. The scale is Height x 1/6 x
   1.3 for new-style races (flag 0x08), Height x 0.2 for classic ones. While the new-zone flag is set, a new-style race
   leaves out the 1.3 and a classic one is multiplied by 0.7692308 (1 / 1.3).
3. ViewHeight = 6.2 x the stored scale.
4. ComputeSpecialHeights; then ViewHeight += the view offset. A special scale is stored without recomputing the others.
5. JumpStrength 1.35, SwimStrength 3.375.
6. MaxSpeakDistance AvatarHeight x 0.3 (from 4.0) or x 0.6, at least 0.3.
7. An AvatarHeight of 0 becomes 0.1.

- `change_height` (OP_ChangeSize): Height = size, SetHeights(viewOffset, animScale), the actor's scale and volume
  radius. The mount chain does the same with SetHeights(0, 1.0).
- Also `eye_height`, `calc_scale_factor_for_height`, `actor_height_fixup` and `place_in_world` (the player actor's
  fix-up table and in-world raise).

#### How the client uses spawn heights

`WorldState` keeps each spawn's `heights`: `set_heights` on OP_ZoneEntry / OP_NewSpawn and after an illusion;
`change_height` on OP_ChangeSize; every spawn again when OP_NewZone changes the new-zone flag (+0x2b4; 1 until the
first OP_NewZone); `make_actors` for the fix-up.

feet = z - AvatarHeight everywhere:

- the actor is drawn at z (EQG models lowered by ROffset x scale);
- boxes and the target ring start from the feet;
- the player's AvatarHeight is used for the collision sweep (radius still `sweep_radius`, see "Movement collision"),
  the floor and fit-here tests, regions (head at z + ViewHeight - AvatarHeight) and ladders;
- the camera eye is z + eye height.

A change of the player's AvatarHeight keeps its z (the feet move). At zone-in the player's position is its
OP_ZoneEntry record's, raised by 2 up to four times while outside the world's regions, else the zone's safe point.

Remote spawns: a moving spawn in flymode 0 is kept at floor + AvatarHeight; one at rest settles onto a floor at most 5
below or above it (`WorldState::smooth`), so an NPC a poor server map leaves higher stays in the air (see "Grounding
NPCs").

#### Stopping

Another spawn steps on with its last update's SpeedRun and turn rate while the update is at most 12 s old
(AdvanceSpawn). A stand state of sitting (110), looting (105) or lying (115) clears both, so it slides to a stop.
Checked: PlayerZoneClient::ChangePosition @0x5a0bc0's StopMoving.

The local player's body loses them the same way, and loses SpeedRun when a stun starts (the mount's too;
`GameConnection::speed_stops`). Checked: CharacterZoneClient::StunMe @0x4432c0.

#### Stalled spawns

A deliberate difference. A spawn other than the player and its mount that stays within 0.25 of one place for a
second (`Spawn::stalled`, in `WorldState::smooth`) plays its standing and idle clips and makes no footsteps; its
velocity and SpeedRun are kept for the physics. A rider stalls with its mount. It changes only looks and sound, so it
has no option.

The original walks in place there: AdvanceSpawn @0x4877e0 stops stepping a spawn 12 s after its last update without
clearing its speeds, and Handle_OP_ClientUpdate @0x4b3d00 clears them only on a stop (animation 66). EQEmu sends a
stop once, unreliably and only within MobCloseScanDistance, so a client that misses it walks the NPC on for 12 s,
then in place.

#### Stuns

OP_Stun calls StunMe(duration, 0, 0, the byte at +4) (the case @0x4c7093).

- StunMe does nothing under stun immunity (TotalEffect(40) > 0) or while a stun's timer runs.
- Below 1000 ms is 1000 ms for a player, so EQEmu's UnStun (an OP_Stun of 0) stuns for a second when none runs.
- A stun sets the timer and state, prints "You are stunned!" (filter 0x1a hides it), stops the player and mount, and
  sets the listener's room 0x17.
- With the byte at +4 it sets the turn rate to 32 (EQEmu sends 0).
- DoPassageOfTime ends the stun when the timer passes ("You are unstunned.") unless mesmerized; then the mez's removal
  ends it.

#### Grounding NPCs

A deliberate difference: option `ground_npcs` (hailstone.ini `display.ground_npcs`, Options > Graphics "Ground
Floating NPCs", on by default).

An NPC or corpse on the ground more than 0.25 and at most 50 over the floor is drawn at floor + AvatarHeight. The
floor includes the zone's terrain, placed objects and doors (Plane of Knowledge's portal stones), as the player's
floor test has them. It covers the gaps the landing check leaves: more than 5, and any spawn whose last update is
over 12 s old.

- Flymode 3 counts as on the ground (EQEmu's GravityBehavior::Water, East Commonlands' spiderlings).
- Left alone: flying and levitating spawns (flymode 1 and 2; EQEmu's 4 and 5), riders, spawns on a boat, players, and
  spawns in or over water, slime or lava.
- Above 50 the server more likely means it.

A spawn is measured again when it moves, and every spawn after `reground()` (zone collision loaded, a door came, went
or moved); with no floor found, every frame until there is one. Only `display_position` is lowered (model, name,
ring, picking, NPC walls); `position` keeps the server's z for range and melee. When a physics step has just landed a
spawn on the zone floor, the draw reuses that landing instead of a second zone-mesh query.

#### Riding while illusioned

A deliberate difference: option `illusion_mounts` (hailstone.ini `game.illusion_mounts`, Options > General "Ride
While Illusioned", on by default). On, no rider's model is checked, local or remote; off, the original's checks apply.
EQEmu does not check the rider's race. The mount's model must still have a seat: `<rider code>_MOUNT01`,
`CHEST03_MOUNT01` or a `RIDER` dag (the Skystriders' `mpg.eqg` has `CHEST03_MOUNT01`).

The original's PlayerClient_AttachRiderToMount (game/mounts.cpp) refuses a rider whose model has no tunic point
(0x58dde0; races 1, 367 and 522 always pass) or is a mount race drawn as the human fallback, which in practice refuses
an illusioned player: the spell fails with 5524 ("You must have both the Horse Models and your current Luclin
Character Model enabled to summon a mount.") and the buff is removed.

## Name sprites

`draw_nameplates` (app) with `game::name_sprite_switched_on`, `name_sprite_shows_guild`, `name_sprite_market_words`
and `npc_owner_line` (`game/combat.h`). Checked: PlayerClient::SetNameSpriteState @0x58e2d0,
unk_PlayerClient_UpdateNameSprite @0x58f050.

#### Which names show

- Options > Display "Show Player Names" / "Show NPC Names" (hailstone.ini `display.pc_names`, `display.npc_names`;
  `/shownames off` clears the first) switch the sprites by kind. Corpses go with the NPCs.
- The player's own name is drawn as the original draws it, with the same `/shownames` parts: title and suffix above
  3, last name at 2, 3, 4 and 6, the `<guild>` line at 3 and 4; not in first person.
- A deliberate difference: Options > Display "Show My Name" (hailstone.ini `display.own_name`, on by default) switches
  the own name alone.
- Not done: the original shows the title window's chosen title and suffix on the own sprite while that window is open.

#### Offline, trader and buyer words

- A player's name starts with "Offline " (eqstr 766) in offline mode (other_data bit 1, then SpawnAppearance 53),
  then "Trader " (5503) or "Buyer " (6055) in the title's place (`game::name_sprite_market_words`).
- Players only: an NPC marked as a trader keeps its plain name.
- Stock EQEmu sets neither offline flag.

#### Invisible names

An invisible player or live NPC the viewer can see has its name in parentheses, `(Name)`, inside the title, last name
and leadership marks (`GameConnection::name_invisibility`), when HideMode +0x338 is above 0, never on a corpse or a
race from 0x8cd up. Checked: PlayerClient::SetNameSpriteState @0x58e2d0.

- Another spawn's HideMode is the server's (spawn record, then SpawnAppearance type 3).
- The player's own comes from Hide and its effects: above 0 while hidden (the local hide flag, Shroud of Stealth
  included) or for regular invisibility (SPAs 12, 314); -2 / -1 for versus undead / animals, no parentheses. Sneak
  alone adds no mark. Checked: EQ_Character::UpdateMyVisibleStatus @0x459510 by PcZoneClient's virtual @0x5804e0,
  the Hide level @0x57f5f0 and CalcInvisLevel @0x442770.
- The hide flag is the client's own, set when its Hide check passes (CharacterZoneClient::UseSkill @0x45b2d0).
  Stock EQEmu sends the hider nothing (the others get SpawnAppearance 3), and nothing at all on a failed server roll,
  so the mark shows then too, as in the original.

A Hailstone addition, off by default: Options > Display **Bracket Invis vs. Undead** (`display.show_invis_undead`)
shows `[Name]` for invisibility versus undead the client knows of (SPAs 28 and 315 in the own buffs, or the target's
OP_TargetBuffs), `([Name])` with both.

A deliberate difference a UI pack asks for: the player, target and target-of-target captions show the same marks
(the original's are plain: __GetLabelFromEQ @0x763640, __GetGaugeValueFromEQ @0x762410).

#### Name colours

- A GM's name is green (`#1da64b`, `game::gm_name_colour`): the gm flag, not a live NPC, Anon 0. Checked:
  PlayerClient::SetNameSpriteTint @0x58bf00.
- A deliberate difference: every corpse gets the ordinary corpse colour (the original keeps a gm corpse green; stock
  EQEmu sends corpses with gm 0).
- A targeted GM keeps the green, where the original fades toward white.
- The other colours are this client's; consider-coloured NPC names are `display.con_name_colour` (on by default):
  docs/graphics.md "Name plates".

#### Occlusion and overlap

Hailstone's own:

- A plate's text is hidden pixel by pixel wherever the scene is nearer.
- Of overlapping names the nearer is drawn over and the covered one fades.
- The target's name never fades and stays at its head; the scene hides it unless
  `display.target_plate_through_walls` (Options > Display "Target Name Through Walls", off by default).
- Its highlight is `display.target_plate_highlight` (Options > Display "Highlight Target Name", on by default):
  docs/graphics.md "Name plates".

## Extended chat routing

A deliberate Hailstone extension (`game/chat_windows.h`): channels 57..107 and user colours 355..408 (User_100..153)
for a server that sends a later client's extended chat colour ids.

The original routes each line by colour to one of 57 channels: user colours 0x100..0x15e by its table, every other to
Other (22), and keeps 99 user colours (User_1..99). Checked: `CChatWindowManager::GetChannelFromColor` @0x6504c0.

- Colours 356..380 go to channels 57..81, 382 / 383 to 84 / 85, 388 / 389 to 88 / 89, the consider colours 390..396
  to 95, 397 to Non Melee Hits (40), 398..408 to 97..107. 355, 381 and 384..387 go to Other. (The original sends
  them all to Other in user colour 0's RGB: `GetUserDefinedColor` reads index 0 past 98.)
- Every colour up to 354 goes where the original sends it: `Chat::NonMelee` (283) to Non Melee Hits,
  `Chat::DotDamage` (325) and colours below 256 to Other, 348 to Emotes, 349 to Guild.
- The client's own lines keep the original's colours (environmental hits 0x10a / 0x149 / 0x117, deaths 0x116, randoms
  0x11f, interruptions 289, considers); none is extended.
- The Filters menu is InitContextMenu's for channels 0..56; the extended channels are in an Extended (server) submenu
  after ALL (Direct Damage, Damage Over Time, Healing, Damage Shields, Environmental Damage, Random Rolls, Other
  Messages). Channels 82, 83, 86, 87, 90..94 and 96 are in no menu.
- The extended user colours follow the original's rows on the Options Colors page, with Hailstone's defaults; Reset
  Default Colors resets them. `/usercolor` keeps 1..99 (100 a no-op).

Stock EQEmu sends no colour above 340 (`common/eq_constants.h` `Chat`), so on a stock server everything routes as in
the original; the extension has no switch. The names and meanings of 355..408 are not confirmed (docs/unknowns.md
"Chat").

## Chat keys, the PvP rule set, trade and group invites

Code: `game/chat_input.*`, `game/combat.*` (con_level_value), `game/zone_lines.*`, `game/trade_rules.*`,
`game/group_rules.*`; vectors in `tests/chat_trade_vectors_test.cpp`.

#### The chat input's key slots

The chat input takes the keyboard only through key slots 479..499, which fire while no window has the keys:

- Enter (488) toggles the chat focus. Giving the input the focus while movement keys are held sends AUTORUN with the
  held mask: forward 1, else back 2; turn left 4, else turn right 8; 0x10 while strafing.
- ';', '/', '[' (481, 482, 499) go into the input.
- 493..496 are the character select keys; 498 leaves character creation.

#### Named chat keys

These work while the keys go to the game (`ClientApp::key_command`, `app/lua_ui_chat.cpp`); each replaces the input
and gives it the keys.

| Key | Effect |
|---|---|
| TELL | puts "/tell " into the active chat window's input |
| CMDMODE_SAY | "'" (InterpretCmd's say command, whatever the window's default channel) |
| CMDMODE_EMOTE | ":" (InterpretCmd's emote command) |
| RTARGET | "/rtarget <name>" (12365 without one) |
| HISTORY_UP / HISTORY_DOWN | the lines sent before (the input's own Up / Down) |

PAGEUP / PAGEDOWN scroll the active window's output by its height less 8.

The reply list's index (0xde21d8), read by REPLY and RTARGET, goes back to the newest with every new tell.
CYCLEREPLY (Tab) steps it one older, CYCLEREPLY_BACK (Shift+Tab) one newer, wrapping (LastTell_CycleIndex), only
while the chat input has the keys and holds a "?tell" or "?rtarget" line (`game::cycle_reply`). Here they are the
input's own Tab and Shift+Tab; another key bound to them does not reach it.

#### The active chat window

The active chat window (GetActiveChatWindow) is the Always Chat Here window (chat menu item 0x2b, kept as
`LockedActiveWindow`), else the one clicked last. A click into another window's input still types there.

A Hailstone difference: the main window is locked by default (no `LockedActiveWindow` key, the locked window closed,
or another unticked), so Enter and '/' start a line there. Unticking the main window gives the original's last-clicked
behaviour (`LockedActiveWindow=-1`, kept). The original has no locked window until one is ticked.

#### PvP rule set, regions, trade and invites

- PvP rule set (OP_LogServer +4) governs:
  - con levels: rules 1 / 2 / 4 give level 1 below -4 / -8 / -5 levels, level 7 from 5 / 9 / 6 levels, else 5;
    every rule but 0 then clamps levels up to 5;
  - who may attack whom (`attack_rules.*`), CastSpell 210, hit-by-spell, resist debuffs x 1.5, steal, bind wound 145,
    invites by deity team under rule 4, the death coin rule.
- Under rules 4 and 6 a detrimental spell between players below level 6 is refused in a newbie zone
  (`game::is_newbie_zone`). Every known spawn's Zone (+0x210) is taken as the current zone (instance in the high 16
  bits; the low 15 bits kept above 1000).
- Region flags start at 0xFFFFFFFF on zone-in, so the first region check always counts as a change.
- Trade: [Options] Trade (2 everyone, 1 group, 0 no one); is-ok-to-transact in its order; the refusal OP_TradeBusy and
  1447. CastSpell asks the same check.
- Group invites: 148 bytes out. In: declines 1 / 4 / 5 or the pending invitation with the u32 at +0x94.

## Group roles

The group leader assigns Main Tank (1), Main Assist (2) and Puller (3) in either UI pack by right-clicking a member's
name in the Group window or their own in the Player window (Y on a row in the gamepad pack). Role icons beside the
names show the assignments, full names on hover.

- A member may hold several roles; each role belongs to at most one member.
- Mercenaries may only hold Main Tank.
- On stock EQEmu, assigning a role needs the member in the leader's zone; clearing does not. The menus say so.

Differences from the original (`app/lua_ui_hud.cpp`, `GameConnection::send_group_role`):

- Hailstone opens a roles-only menu from the name, with no "Window" or "Leader" row (/makeleader stays). The original
  has a "Roles" submenu of the windows' own context menu, starting with "Leader" (CGroupWnd::sub_0067ac60 @0x67ac60,
  CPlayerWnd::sub_00717f60 @0x717f60, CGroupWnd constructor @0x67bc00).
- Everyone in a group sees the menu, role rows greyed for all but the leader; the original offers Roles only to the
  leader (FUN_00443670 in 0x67ac60).
- For a mercenary Hailstone greys Leader, Main Assist and Puller; the original refuses on the click with eqstr 7607
  "A NPC cannot be that role." (CGroupWnd::WndNotification @0x67c620), as `send_group_role` still does.
- `send_group_role` checks leadership (0x1db1) and CanUseCommandNow's refusals, as Cmd_grouproles @0x4f8360 does, so a
  menu left open after leadership changed cannot send. The original's menu sends without checking.

The UI and `/grouproles set|unset <member> <role number>` share `GameConnection::send_group_role`, which checks group,
leader, member and role before sending `OP_GroupRoles`. A request changes nothing locally: `OP_GroupRoles` replies and
the `OP_GroupUpdateB` roster set the flags, which the icons, menu and scripts' `Group.MainTank`, `Group.MainAssist`,
`Group.Puller` read. A disband or zone change clears them until the server restores them.

## Group and raid leadership

Both UI packs expose raid Main Assist and Mark NPC delegation, Find PC, and the Leadership window's three Create
Hotkey buttons (the hotkey goes on the cursor; the gamepad pack opens its slot picker). Passive abilities have none.

The purchased rank list is separate from the active leaders' ranks. The records RoF2 marks untrainable (2, 3, 5, 18,
21) are not listed, although stock EQEmu still sells 2, 3 and 18; ranks bought through another client do not show, and
an old hot button for one does nothing.

#### Ability colours

Both packs use the original's colours (CLeadershipWnd refresh @0x6b77f0), in this order:

| Colour | Meaning |
|---|---|
| Green (`#00FF00`) | The current group or raid leader supplies at least rank one, even if the player has not purchased it. |
| Gold (`#D6B228`) | The player has purchased a rank, but the current leader does not supply one (inferred, see below). |
| White (`#FFFFFF`) | Unpurchased and affordable at the displayed native cost; Delegate Mark NPC also requires the player's corresponding Mark NPC rank. |
| Grey (`#C0C0C0`) | Unpurchased and not affordable, or that prerequisite is missing. |

Gold is inferred: the check @0x55fb60 is unnamed, but it is the one used for the Mark NPC prerequisite, which must be
the player's own rank. Green uses Leadership_HasAbilityRank @0x55eb10, not Leadership_IsAbilityActive @0x55eb70, so it
ignores population, expansion and delegation (EQEmu sends the leader's ranks even below three group or eighteen raid
members). White uses the native cost estimate; the server decides the price.

#### The raid member menu

A Hailstone UI extension in both packs: right-click a member, or select one and use Member actions (Y on the focused
row in the gamepad pack).

- Main Assist and Mark NPC delegate those roles; they do not mark the member as an NPC target.
- Also Find PC, leadership and looter changes, and group removal.
- It captures the member's name and rechecks membership and permissions on the action.
- Scripts keep the original button APIs. Raid-wide controls stay outside the menu; dragging and the gamepad's group
  picker still move members.
- With Context Menus off, both packs show the direct member buttons.

#### Raid marks

`/rmarknpc 1`, `/rmarknpc 2`, `/rmarknpc 3` or leadership hotkeys; marking the same target again sends the same
request and the server decides. `/clearmarks` clears all marks for the leader or a delegated marker.

Two deliberate changes from the original (Leadership_ActivateAbility @0x55f0f0):

- The original sent mark 0 to unmark a target already holding that mark; EQEmu refuses 0, so it is not sent.
- The original marked any target; the client marks only a living NPC (others get the no-target line), although EQEmu
  would take a player.

State:

- Delegation and marks change locally only when the server confirms them.
- Delegates are reconciled by member name, also against zero-numbered roster snapshots.
- Departures, disbanding and zoning clear marks and delegate spawn IDs.
- Zoning keeps the delegates' names and the members' Main Assist and Mark NPC flags: EQEmu re-announces a delegate
  only when already in the new zone, and its delegate request toggles, so re-choosing a forgotten one would clear it.

#### Purchase and effects

A deliberate change: Purchase is offered whenever the next rank exists and the player has a point of that kind. The
original disables it below its own cost table (Mark NPC 3 / 4 / 5, Delegate Mark NPC 6 / 7 / 8), but the server sets
costs (stock EQEmu 1 / 2 / 3 and 6 / 6 / 6) and checks every purchase. The cost column still shows RoF2's table.

The expansion and population checks remain: Gates of Discord, at least three group or eighteen raid members, and the
active leader's rank. HP, mana, HP regeneration and offense enhancements use the active ranks, stack group and raid
effects, and feed the character sheet and ATK label; the server stays authoritative.

Spell Awareness adds the spell name to another caster's casting message when the active leader grants it and the
caster is at most five levels above the player (the spell must be in the player's spell data). GM spell
identification is independent of it.

Invite Expedition and Invite Task in the raid window invite missing members of the player's expedition or shared task
**into the raid**: ordinary named raid invitations, skipping the player and existing members, rechecking raid
leadership, lock and capacity; no expedition/task leadership needed.

Not tested live with several players.

## Hot buttons, socials and tracking

Code: `game/hot_button_press.*`, `game/tracking.*`, `app/actions_ui.cpp`, `net/game_connection_tracking.cpp`; vectors
in `tests/hotbutton_tracking_vectors_test.cpp`.

#### Hot button press

- Stunned (0x66) or an item request pending prints 12084.
- A mouse press of types 5 / 13 does nothing.
- Then it acts by type (see the header). Types 2 / 3 hold an index into the Actions window's [CombatSkills] /
  [Abilities] lists, type 9 a skill id.

A social (4) is only queued; the periodic update runs one line per step: after a line one step waits, /pause N waits
N, /timer N starts the display cooldown; 120 + n runs /alternateadv act n. Only the comma after a prefix is skipped,
so a second prefix follows with no space (`/timer 600,/pause 10, /cast 1`); after `, ` the rest is the command
(`game::split_social_line`). A queued social stops when another character enters the world.

#### The hot button's mouse and menu

`game/hot_button_menu.*`:

- A left or quick right click on an empty button opens the menu unless the cursor holds something.
- A quick right click (on release) of a filled button activates it, except on items.
- The right button held 500 ms opens the menu on any button.

Rows are shown or hidden by button (not greyed):

- Activate / Consume (krono), Inspect, Clear;
- Change Size, Assign Icon, Clear Icon;
- Assign Label (15 characters; a social's label renames the social);
- Assign Social (10 pages of 12, "<New Social>" opening the social editor);
- Memorize (the gem's spell select menu, in spell categorization order; memorizes 500 ms after showing the book) and
  Unmemorize.

#### The hot bar's own menu and settings

`game/hot_button_sets.*`, `game/icon_selection.*`:

- PercentSize (UI ini, 40..175, default 100) sizes each button. ShowKeyMap / ShowSpinner default on.
- Assign Icon's {icon type, icon} draws icon - base of A_DragItem (500..), A_SpellGems or A_MenuIcons.
- Clear current hotbuttons clears the bar's 120 buttons.

Hotbutton sets: 10 named entries in the character ini's [HBLoadouts] (HBSet<n>.inuse / name / fileNum); each bar's 10
pages go to HB_<fileNum>.ini [Hotbuttons] Page<p>.Slot<b>.

- Saving takes the name's set or the first unused entry, then sorts (in use first, __stricmp).
- A set saved again keeps its file number; a new one (or one sharing its file) takes the lowest number no other set in
  use has.
- Load clears every button, then parses the file.

#### Tracking

The reply's entries are ordered by an introsort (insertion sort up to 32, median of nine above 40, heap sort when
depth runs out) by the ranger's /tracksort order or the reply order, so ties land in a fixed place. The filter sets
the standard con bit and hides by the con mask and the player / pet / mercenary filters.

## Hot button checks and cool-downs

`game/hot_button_state.*` (tests `tests/hot_button_state_test.cpp`), drawn through `hs.HotButtonState` and
`hs.ButtonCoolDown`. Both UIs draw a shade shrinking toward the bottom, the seconds left over a second, and an accent
frame while checked.

#### A button's cool-down

A cool-down is (begin, duration) in client time; set-cool-down sets it unless its time is up and answers whether it
runs. The draw is the spell gem's rectangle: user colour 0x15f at alpha 0x40, from the top moved down by
ftol(min(part gone, 0.87) * height) (`game::ButtonCoolDown`, `spell_gem_overlay_top`).

#### Button state update

Each button updates every 100 ms (200 for an item link), starting with SetCheck(0):

| Button | Check and timer |
|---|---|
| Weapon slots | checked while the primary swing is not ready or auto fire is on |
| Skills | SetCoolDown(last used, timer duration); checked while not available, so Sneak and Hide stay checked while sneaking / hidden |
| Socials | their /timer |
| Alternate abilities | the ability-ready (left, total); unowned ones checked |
| Combat abilities | discipline reuse timer in client time - reuse, with reuse = recast - focus reuse mod |
| Leadership | checked while the ability is not active |
| Pet buttons | the pet window button's check |
| Spell gem buttons | the gem's own window (the bar's tint stages and cool-down) |
| Items | their recast timer from the inventory slot draw |

#### Item recast timers

`GameConnection::item_recast_timer` / `set_item_recast_timer`, `game/hot_button_state.h`, in Unix seconds (as EQEmu's
last_cast_time).

- The timer is the later (unsigned) of the item's own (header last_cast_time) and the profile's by recast type (types
  0..99; -1 has none). Setting it writes now + delay to both.
- CastSpell refuses a clicky with 236 while now < that timer; it sets the timer at once for an instant item (cast time
  <= 100 ms).
- OP_ItemRecastDelay (cast item valid, or the packet's ignore byte) sets the timer; OP_MemorizeSpell 3 sets the cast
  item's timer = recast + the client time offset, in client-time ms.

The inventory slot draw (every item slot: `hs.Item` / `hs.BagSlotItem` / `hs.SlotItem` give `cooldown`; inventory hot
buttons) takes the time left (timer - now). When the button has no cool-down, or its cool-down differs from that time
by 1000 ms or more, the draw restarts it (or stops it once the time is up).
The potion belt starts a slot's only while its button has none and the timer is ahead.

#### Item preview

`game/item_preview.h`: Alt + right click on an item slot or in the guild bank, and the item display's, merchant's,
bazaar's and barter's Preview buttons send OP_ItemPreviewRequest (id and ornamentation) and show the preview window.

The item shows on a copy of the player in its first visible worn slot (head, arms, wrist 10, range, hands, primary,
secondary, chest, legs, feet): weapons held, the other hand kept unless a two-hander is involved; range in the left
hand; armour by material and tint. Otherwise alone, unless its IDFile is IT63 / IT64 ("No item preview available.").
Hailstone uses its own orbit camera.

#### Evolving items: Final Result

An incomplete evolving item's Final Result link asks for inspection with OP_EvolveItem action 4. Checked:
`CItemDisplayWnd::WndNotification @0x6ae870` → `0x69ef60`.

- The request keeps the source/final template ids, current level, unaugmented link hash and native !Shift flag; a
  missing final-id hint is sent as received (stock servers may need a nonzero one).
- The gamepad pack adds a focusable Final Result button.
- The reply uses the ordinary item display and changes no inventory.
- Action-0 updates match both halves of the instance id and refresh progress, activation and an open display.
- Progress above 100% is kept; no level advances locally: the server sends the evolved replacement.

Not tested live.

#### Evolving-item experience transfer

`net/game_connection_evolving.cpp`. Both packs offer Transfer evolving experience on an owned evolving item's display
(`ItemExpTransferWnd`); the server's action 1 opens it too, clearing any selection. Hailstone lists worn, carried,
bagged and cursor items in two selectors (where EQEmu's `HasEvolvingItem` looks); RoF2's buttons take the cursor item
instead (CItemExpTransferWnd::WndNotification @0x6b0230).

1. Each complete selection sends action 2 with both 64-bit evolving IDs (header `parcel_item_id` high word,
   `instance_id` low word).
2. The server answers with the compatibility percent, the max-transfer level (zero is valid) and projected forms.
   Transfer stays disabled until both projections exist and compatibility is nonzero.
3. Transfer asks the native confirmation (eqstr 6148).
4. Confirmation sends action 3 once, bound to the original IDs, the preview's revision and the live items less their
   progress and activation.

Previews never enter the inventory and no success is reported locally: the server replaces both items on the cursor.
A refusal changes nothing (EQEmu prints "Transfer Failed"). RoF2 clears both selections after sending (`0x6b00b0`).
Zoning or disconnecting closes the window.

#### Other rules

- Stats-disabled flags (`game::update_stats_disabled`): from the UseItem (0x13) and Ornament (0x15) game features and
  OP_SendMembership, on worn items before the stat sums and on a shown item. A disabled child adds nothing unless its
  owner is disabled too. EQEmu sends UseItem -1 and Ornament 1, so nothing is disabled there.
- The Actions window's buttons follow the same skill and social rules; Melee and Range Attack are checked while the
  primary swing is not ready or auto fire is on.
- Focus reuse mod (`Character::focus_reuse_mod`): SPA 310 from the best item focus, buff and AA, each times its
  percent / 100, at most the recast time; gems, hot buttons and the combat ability window subtract it.

#### Slam

Skill 111, `slam_innate`. A deliberate change: Hailstone takes the innate Slam from the spawn's current race
(`app::skill_race`), as EQEmu does (Client::OPCombatAbility checks `GetRace()`). The combat lists gain or lose Slam
with an illusion, and Bash without a shield uses the Slam path; both send `DoAttack(100, 10)`. The original uses the
profile race (EQ_Character::InitInnates).

#### More Hot Button States

Enhancement (option `hotbutton_extras`, hailstone.ini `ui.hotbutton_extras`, Options > Display "More Hot Button
States"; on by default).

| Button | Without the option | With the option |
|---|---|---|
| Actions window list buttons (types 2 / 3) on a hot bar | no timer or check | their skill's timer and check |
| Melee Attack | unchecked between the primary swings' ready frames | checked while auto attack is on |
| Sit menu button | unchecked | checked while sitting |

With it, a replaced hot button also drops the old one's cool-down.

## AFK and the NPC target keys

#### Viewer visibility

Actor drawing, nameplates, picking and target cycling share viewer visibility (`game::spawn_invisible`,
PlayerZoneClient::IsInvisible @0x5a1850).

- Invisibility ranks and SpawnAppearance type 3 are compared with the viewer's SPA 13 total, built from the sheet only
  while some spawn is hidden at a rank.
- Group members are exempt. A GM target stays hidden from a non-GM viewer whose rank reaches it.
- The player's own model is hidden only for a GM with hide mode 3001.
- Corpses and zero-HP targets are never hidden; remote HP comes from the server's percentage.
- Any other spawn with property 100 (PlayerBase::HasProperty(0x64): body type 100 or in the property list) is always
  hidden.
- Visibility is independent of material opacity.

#### AFK

The client has no idle timer.

- The AFK flag changes only with /afk (13199 / 13200, `;afk on|off silent` to the chat server, OP_SpawnAppearance {me,
  0x18, flag}) and OP_SpawnAppearance type 0x18 for our own spawn, so the zone's auto AFK sets it and /afk toggles it.
- While set, a tell gets the away reply (1418 and the /afk message, or 1419).

"You are now AFK." / "You are no longer AFK." come from EQEmu's zone (`Client::CheckAutoIdleAFK`,
Character:EnableAutoAFK, SecondsBeforeAFK{Combat,NonCombat}Zone 1800 / 600 s, SecondsBeforeIdle* 1700 / 640 s). An
OP_ClientUpdate at the last position more than the AFK time after `ResetAFKTimer` sets AFK. Only OP_CastSpell,
OP_ChannelMessage, OP_MoveItem, auto attack and OP_SpawnAppearance AFK reset that timer, not walking, so a player who
only runs and clicks goes AFK at his next stop (seen live).

Enhancement (`game::AfkKeepalive`, always on): while the user gives input (`InputState::user_activity`: keys, mouse,
pad, touch, held or moved), the client sends OP_SpawnAppearance {me, 24, 0} at once, then at most once a minute. The
zone takes it as the AFK toggle off (ResetAFKTimer); input after a zone-set AFK clears it with 13200. An /afk stays
until /afk. Without input nothing is sent.

#### The NPC target keys

TARGETNPC 32 and CYCLENPCTARGETS 33 take every NPC with bTargetCyclable set, with no owner check, so pets and
mercenaries too. The two options below filter the NPC cycle and the pad's backward cycle; PC and corpse cycles are
not filtered.

- Enhancement (option `tab_skip_pets`, hailstone.ini `target.skip_pets`, Options > Display "Tab Skips Pets", on by
  default): leaves out a spawn whose MasterID is a player, and mercenaries. An NPC's own pet stays.
- Enhancement (option `tab_line_of_sight`, hailstone.ini `target.line_of_sight`, Options > Display "Tab Needs Line of
  Sight", on by default): only NPCs with a clear segment from the player's eyes to 1 over the spawn (Spawn.LineOfSight's
  test). Without the zone's collision every NPC counts as seen.

#### Target death and auto targeting

The target's death clears the target and turns auto attack and auto fire off.

Enhancement (option `attack_until_xtarget_clear`, hailstone.ini `combat.attack_until_xtarget_clear`, Options >
General "Attack Until XTarget Is Clear", off by default): auto attack stays on while the extended target list holds a
living spawn (server-hidden slots left out). Taking a new target ends the hold. Auto fire still goes off.

The original never picks a target itself. Enhancement (option `auto_target_xtarget`, hailstone.ini
`combat.auto_target_xtarget`, Options > General "Auto Target XTarget Attackers", on by default):

- With no target, an NPC that attacks the player (an OP_Damage, a miss included) and is on the extended target list
  becomes the target; also when the target dies and such an attacker remains (latest attacker first, else slot order).
- Players' pets, corpses and untargetable spawns are skipped.
- Clearing the target by hand holds until the next hit. Nothing is picked while dead.

#### Facing and role sets

- The attack key (AUTOPRIM) and attack buttons turn the player to the target when they turn auto attack on;
  `/attack` never does. Enhancement (option `face_on_attack`, hailstone.ini `combat.face_on_attack`, Options > General
  "Face Target On Attack", on by default): off leaves the heading alone (`GameConnection::toggle_auto_attack`).
- XTarget role sets (the character ini's `TargetSet<n>`, CExtendedTargetWnd_ApplyRoleSetString @0x54a990): each listed
  slot is requested again, then auto hater for the rest. Deliberate difference: the client stops at the window's 20
  rows, where the original follows the string's count.

## Cadences (the periodic update)

The periodic update drives the timed mechanics. It and UseSkill send uninitialised stack bytes to the server (OP
0x31e6, OP 0x52fa), so they are not covered by vectors.

#### The schedule

- Every frame: process_all_stats.
- More than 1 s (the spawn's +0x2d8):
  - for a living player, the endurance upkeep: the buffs' cached upkeep comes off the stored endurance; below 0 it
    becomes 0 and the discipline in the last buff slot goes, told to the server with OP_Buff (ApplyEnduranceUpkeep
    @0x448bb0);
  - then the SPA 0x63 movement rate, air supply and environment damage.
- More than 6 s since the last tick: the six-second tick below (one timer per character at spawn+0x24c; packets do
  not restart it).
- More than 18 s: the profile+0x3408 countdown.
- More than 60 s: stats marked dirty.

#### The six-second tick

1. Drunkenness: a positive value decreases by one, even without BaseData, and updates the body's sway. At 70 or more
   it selects first person and widens the FOV; at the alcohol-tolerance mount threshold it dismounts.
2. ProcessAffects (`game::six_second_tick`, game/passage_of_time.h, over `mechanics::process_affects`):
   - every buff of type 2 and up whose duration was already 0 (or -1 without the permanent formula 50) is removed
     locally, with no packet;
   - every positive duration counts down by 1;
   - each buff's slots apply their per-tick effects: HP, mana, endurance, the mount summon, the fading lines 0x30c2
     / 0x30c3, the cast interrupt chance;
   - HP is CalcTickHPChange (caster distance, bard modifier, an inverted buff negated, a player caster's PvP loss
     75% / 70%), summed, then ModifyCurHP;
   - mana is SPA 15 / 0x1a1, not for classes 1, 7, 8, 9, 16; endurance is SPA 189;
   - a draining beneficial buff the pool cannot pay is removed (OP_Buff);
   - a tick's HP loss breaks the first mesmerize (SPA 31) buff (OP_Buff) and ends the stun.
3. For a living player below max HP, the HP regen (GetHPRegen, then ModifyCurHP); when its flag says so, "You are
   bleeding to death!" (0x318d).
4. Dead or alive, the endurance regen (Cur_Endurance(1) clamps it) and ApplyManaRegenTick.

Without the game folder's BaseData.txt row for the character the ticks wait. The server's HP, mana and endurance
updates overwrite the pools whenever they come; the ticks move them on in between, as in the original.

#### Held buff countdowns

A countdown is held while:

- any buff has SPA 0x1b4 before its first blank slot (GetPCSpellAffect);
- the spell is suspendable (column 200, +0x279) and the zone suspends buffs (Spell_IsBuffCountdownHeld @0x4dcf20;
  OP_NewZone +876, EQEmu's `zone.suspendbuffs`, where the server stops ticking them too);
- without SPA 0x1b4, the buff timers are on hold (the spawn's +0x22d).

The hold (+0x22d) is set by a SPA 0x1b4 buff landing or ticking and by OP_SpawnAppearance 38 non-zero; cleared by 38
with 0 and by a new spawn at zone-in. Stock EQEmu never sends 38. The SPA 0x1b4 exception covers only the spawn's
hold: with 0xddfc5c set every suspendable spell is held.

A held buff costs no upkeep. The buff, pet and target windows, the Buff TLOs and the plugins' target buff read show
its time as received. The client knows only the player's hold, so it holds the target list only while the player is
its own target. Checked: CEverQuest::HandleWorldMessage copies OP_NewZone to 0xddf8f0 at 0x4c9edf (+0x36c is
0xddfc5c); CTargetWnd @0x76b1b4 and CPetInfoWnd @0x7105e4 pass the spawn's +0x2cc.

#### Running, consciousness and stopping a cast

The live sheet gets the actual running and drunkenness state; running needs nonzero forward run speed and not riding.

Damage below 1 HP can make the player unconscious (3): movement stops, the player lies down, the camera goes
overhead, and the six-second tick bleeds. Positive HP recovers on the tick. Server death stays authoritative.

SPA 343's flag-1 interruption stops any local cast without an extra interrupt packet. StopSpellCast
(`GameConnection::stop_spell_cast`):

- a bard's non-song only resets the cast, with no interrupt packet and no twist restart; it is kept outright with
  recovery at most 100 ms when not an item cast;
- anything else sends the empty OP_ManaChange for reason 0 only (StopCastAndFeignDeath, /stopsong) and resets the
  cast;
- a bard song then also restarts the twist for a reason other than 0.

Tests: `tests/passage_of_time_test.cpp`, `tests/ability_commands_test.cpp`.

#### Eye control

SPA 67 acquires an Eye summon named from the first 24 bytes of the player's name. Its view and control are separate
from the body, which keeps gravity, air and environment damage; the Eye sends its own spawn ID's movement. Fade,
despawn, death and zoning restore control. Once held, control stays while that spawn lives.

Deliberate safety differences:

- ambiguous matching Eye names refuse acquisition;
- the name must not continue with a letter after the player's name (EQEmu's `Eye_of_<name>` plus three digits, so
  `Eye_of_Bobby000` is not Bob's); the original takes any partial match.

Tests: `tests/eye_control_test.cpp`.

#### Swing timers

`ActionsUi::melee_timers` (melee.h) runs each frame before the movement update:

1. While auto attack is on and the primary flag is still set at its start, both flags clear and the primary,
   secondary and ranged timers restart at now.
2. A player not standing (StandState 100) has auto attack turned off.
3. The primary flag is set when now - primary timer reaches the attack speed modification of the slot-13 weapon's
   delay x 100 (GetReuseTime(28) for the fist; the ranged delay after a ranged attack).
4. The secondary flag is set when now - secondary timer reaches the slot-14 delay.

The control processing clears a set flag and asks for an attack in slot 13 / 14 (`ActionsUi::melee_attack`):

- It swings the slot's item when it is a usable weapon, else the fist (the profile's +0x346c damage or skill 28's base
  damage).
- Nothing to swing (true) unless it is a weapon, type 0xd or 0x16.
- The delay is Delay x 100, or the hand-to-hand delay (the profile's +0x3470 for the fist when above 0), through the
  attack speed modification.
- Too early (elapsed below the delay, no target with +0x251, no NPC in its attack, no special melee) returns 1
  without a swing.
- Otherwise the timer becomes last + delay when delay <= elapsed < 2 x delay, else now.
- Then the stand (12369 / 12368), range (124), facing (108), line of sight (123) and BodyWet (123) refusals.

The profile's +0x346c / +0x3470 are the dwords before the endurance (EQEmu sends 0). BodyWet is each spawn's
(`Spawn::body_wet`).

#### Swing slots

The swing is the combat ability attack (13). The double attack check must return exactly 1 for a second one.

- Slot 14 swings only when the dual wield check returns 1; it double attacks with Skill[20] above 149 or SPA 225.
- Slot 100 (a combat skill) sets the fist from the skill and runs on the skill's own timer.
- A player's Backstab (8) needs a piercer (type 2) in the primary, else 12874 ("You need a piercing weapon in your
  primary hand") and nothing is sent. A server extension's `backstab_either_hand` also takes one in the secondary (a
  deliberate difference).
- Slot 101 strikes with the primary or the fist.
- Slot 11 (Range) runs the ranged attack: a bow's arrows (1), a thrown stack (2) or the range item itself (0), else
  "You have run out of ammo!!" (12875). It sends OP_CombatAbility {target, 11, archery 7 or throwing 51}; EQEmu
  shoots and sends the missile.

The player is not turned toward the target. Checked: SendCombatAbilityAttack's AimAtTarget(target, 0) @0x4fb42f
reaches AimAtLocation @0x59e6c0, which writes the heading only when given a missile; the missile constructor's own
AimAtTarget @0x500d79 is only on the path of a missile the client makes itself.

#### Attack and hit animations

The combat ability attack computes the attack animation and drops it, so the client's own attack plays no swing. The
server's OP_Animation does, for the attacker too (EQEmu's DoAnim includes the sender; checked live); the handler
(case 0x7177) plays the action at speed / 10 on any spawn.

The client chooses only a missile source's launch animation (slot 7: C09 / BATK for arrows) and the target's hit
animation (`game::hit_animation`): D02 on a damaging hit, a spell's TargetAnim, D01 / D03 / D04 for environmental
types.

#### Proc and song cast animations

A deliberate difference, option `cast_animation_fallback` (hailstone.ini `game.cast_animation_fallback`, Options >
General "Proc Cast Animations", on by default). The cast stage Hailstone adds for a spell that lands with no
OP_BeginCast (docs/graphics.md) plays the spell's casting animation on its source, and the local player's is sent as
an OP_Animation. A casting animation of 0 plays 43 (CastHeals) for a beneficial spell, 44 (CastNuke) for a
detrimental one (`game::spell_cast_animation`); off, nothing plays. The original plays no cast stage here; its
animation for a spell without one is not confirmed (docs/unknowns.md).

A bard song's pulse plays its casting animation on the bard without a cast stage: a singing song always, an
instrument song per "Bone groups" below. The local player's OP_Animation is sent once per spell's landings within
500 ms.

#### Missiles (`game/missiles.h`)

- An item missile with a target does not sweep the zone; spell missiles sweep and also check the target segment.
- Source despawn ends its missiles; target despawn removes homing.
- Item type 9 needs a loaded spell id in 1..45000.
- Duration uses normalized AE duration (capped at 14 s except first-slot SPA 76), otherwise 30 s for travel type 3
  or 50 ms.
- Area target types 8, 21, 22, 23, 28 and 45 stay at the target or destination and play stage 2; other spell
  missiles launch stage 1.
- SPA 76 area contact checks moving targets every 2 s inside 2.5 times AE range with sight; other local contacts use a
  3 s gate and source sight, respecting NPCNoLoS. Contact is presentation only, never damage.
- Spell 54 plays its destruction impact one velocity step behind, in the world, so it outlives the missile
  (EQMissile_UpdateAll @0x501415).
- Spell 859 can run a local hit presentation inside a strict 40-unit horizontal radius (@0x501342), sending nothing
  and granting nothing.

The launch delay is 70 % of the animation's own length (the .ani's last key time), multiplied by the float 0.7 at the
FPU's extended precision and truncated, so 1000 ms gives 699. A result below 1 ms becomes 1500 ms; without a range
byte the delay is 0. Checked:
0x58f540's CalcAnimLength, 0x58f5fe-0x58f622.

#### Launch placement and pulse timing

A large source launches from the renderer's posed role-0/name anchor; missing data keeps the packet origin.

Pulse timing uses the source's completed physics step: a non-area launch moves (now - that step) x 0.02 velocity steps
further. Hailstone: a step time not before now counts as none, and the age stops at 12 s, so a stale or wrapped time
cannot throw the missile across the zone. Spawns start with no clock; local, remote and boat/rider steps publish
StepClock time + 1, bridged to the missile's 32-bit clock. Not checked against recorded vectors or live.

### Spell hit modes

Hailstone extends the melee Hit Modes menu with **Direct Spell Damage**, **Damage Over Time**, **Healing** and
**Healing Over Time**, each with:

- **Normal** (the default): the original sentence;
- **Abbreviated**: a concise amount, effect kind and available actor/spell names;
- **Number Only**: the amount.

The modern chat's right-click menu offers **Hit Modes** beside **Filters**; the gamepad has them in Options >
Interface. **ALL** changes melee and spell categories together. Settings are per character in UI.ini's
`[ChatManager] SpellHitMode0..3`; the original `HitMode0..7` keep their meaning.

A deliberate display addition: no spell rules, filters or colours change. Checked: the original's six melee report
helpers (`0x4f7d70..0x4f8270`) select the display by modes 0/1/2 while `OutputTextToLog` gets the full input; the
spell branch of `ReportSuccessfulHit` (`0x52ee40`) bypasses them.

#### Typed spell hit messages

Recognised by OP_FormattedMessage's string id and validated numeric substitutions (`game::spell_message_hit_report`):

| Category | String ids |
|---|---|
| Direct damage | 9073, 434, 12481; critical blasts 428/1040 |
| DoTs | 9072/12954/13327 |
| Heals | 9068/419; critical heals 427/1039 |
| HoTs | 12976/12997/12998 |

- 434/12481 also cover damage shields when the server sends them that way.
- Critical heals have no direct/tick marker, so both use **Healing**.
- A critical sends its own notice beside the hit line, so **Number Only** shows two numbers for one crit.
- Incoming/observer DoTs leave ambiguous caster/spell names out of the abbreviation.

Chat hooks (`OnIncomingChat`, `OnFilterChat`, `mq.event`) get the full sentence whatever the mode; MQ's `dsp_chat`
hook sees the line as displayed (docs/plugins.md).

#### Plain-text hit reports

A complete English combat sentence in OP_FormattedMessage's generic string 6688 (`%1`), or an unnamed, unjournaled
OP_SpecialMesg with speak mode and language zero, also follows the modes when its message type is a known combat type
and the whole text matches a non-melee hit or heal report with a valid amount (`game::spell_text_hit_report`).

- An optional spell name in parentheses stays in Abbreviated.
- Dialogue, other message types, malformed amounts and unrecognised or translated sentences stay full.
- An explicit DoT message type selects Damage Over Time; generic healing follows Healing; nothing else is used to guess
  periodicity.
- Caster and target must look like names: a comma, double quote, colon, or " says", " tells", " shouts", " auctions",
  " whispers" marks a speaker and keeps the line full.

Stock EQEmu's one plain-text report is `Mob::HealDamage`'s "You have been healed for N points of damage."
(OP_SpecialMesg, NonMelee 283), which abbreviates to "You: N heal" like YOU_HEALED (419).

#### OP_Damage landings, logging and hooks

An OP_Damage spell landing on the player with nonzero signed damage also follows Direct Spell Damage or Healing;
another target's landing text has no amount. Everything unmatched keeps its full sentence; no damage or heal is
inferred.

`/log` and the diagnostic log get the full line once, before its display form, melee modes included. A hook that
rewrites a line supplies the replacement for display and log. Not tested live.

#### Environmental hits

Types 0xf3..0xfd: mind burns, rotting, chilled, freezing, tormented, burned, thorns, burning, drowning, falling, a
trap (`game::environment_hit`).

The player's own fall, drowning or lava damage sends OP_EnvDamage, then OP_Damage {the player, no attacker, the type,
spell -1, the damage} unless HP reached -10 - SPA 0x148, and reports it. EQEmu passes that OP_Damage on to nearby
clients (the sender too, but for a fall). When the damage is not 0, ReportSuccessfulHit:

- prints "YOU are burning!" (colour 0x10a) or "Bob is burning." (0x149 for 0xf3..0xfa, hidden by chat filter 0x11 at
  mode 1; 0x117 for the rest);
- plays fall_hit (0x73) for a fall and a crushing trap;
- plays the victim's Hit1..4 sound (no FlshHit);
- plays D01 (0xf3..0xf9), D04 (0xfa, 0xfb) or D03 (0xfc, 0xfd).

#### The shared hit tail

ReportSuccessfulHit's tail (`game::hit_landed`, @0x5311ee) runs after every branch. When the target is not a corpse
and the hit landed (a melee hit, any type above 231, a detrimental spell):

- on the local player it closes the spell book (standing a player the book sat down);
- it stands a kneeling (105) or sitting player, and sets standing while mounted;
- the target's Hit1..4 sound plays (the melee branch adds FlshHit 0x93 / 0x94 before it).

Handle_OP_Damage, for a hit on the player by another, closes the book for any damage above 0, takes melee and
environmental damage off HP, and heals a spell's negative damage.

The blood spurt (`PlayerClient_SpawnBloodSpurt` @0x58f1c0) goes with the player's own landed hit of skill 0..3, 8, 10,
36 or 77 when the target has its authored blood attachment and the sprite loads (docs/formats/particles.md).

## Animations

`game/spawn_animation.*` runs two animation objects per spawn (`render/actor_renderer.cpp`).

#### Classic and new-style selection

Classic (WLD) models take old-style numbers (C01..T09), with the old-style movement and idle choice, the sitting
substitutions, the replacement for a missing clip, and a 100 ms cross-fade.

New-style models (EQG races) take sequence codes 0x4f..0xb6 (old-style numbers go through the old-to-new map), with
the movement selection while moving, the idle choice while still, and the track search:

- The tracks are the `<tag>_<style>_<n>` animations: styles BA 1H 2H DW ST MT, variants 1..4. Other names are not
  loaded.
- A missing sequence falls back through the table (BLSC -> SLSC -> SLPR -> STND).
- The weapon style is the column picked from the player-state weapon bits.
- STND / IDLE become STDA / IDLA while aggressive or after an attack.
- The variant is rand() % the column's variants.
- The cross-fade is (new.blendIn + current.blendOut) / 2.

EQG models also answer the classic codes: each old-style number's sequence in the base style, variant 1
(`assets/eqg_character.h` classic_aliases).

#### Classic playback rate

A classic model's playback rate is 0x4afc90's, doubled for:

- L03 (0x13, but race 8's males) and L04 (0x14);
- D05 (0x10) on a styled model (the animation set's +0x34, set by InitAnimationTracks @0x407800 for an actor with a
  TUNIC_POINT dag: Luclin player models and frogloks);
- race 216, and races 209..212 while UseLuclinElementals (+0x129) is on;
- a styled model's gender-1 animations 2 and 4 (race 2's 5).

Race 330's L04 plays at 0.75.

An OP_Animation action's rate (speed / 10) and the classic movement's go through 0x4b15b0's pre-processing first:

- mount races (0x8cf970's list, `is_mount_race`) run at 0.4, their other animations at 0.5;
- races 42 and 120's neutral gender (2) non-walk at 0.5;
- race 330 at 0.65;
- a styled model's stand / idle at 0.5 and social (S clips) at 0.75, when the packet asked 1.0.

Then 0x4afc90 applies, so a Luclin elemental's action and a styled D05 play at twice the rate. A new-style model's
action does not (0x58afa0 has no such call).

#### Fidgets and stand requests

0x4afe60 also reads a pet's master (+0x38c: the fidget needs a roll above 99), the turning argument (P03 on the spot)
and the global auto attack switch (0xe635aa, read by 0x4b01dd without a local-player check: no fidget for any actor
while on).

A P01 request, or an action whose clip and 0x4b1210 replacement are both missing, is 0x4b15b0's stand request: P01 /
P08, or the follow-up +0x10c4.

- A stand request from a sitting actor becomes the sit clip (0x4b1917); without P07, for race 0x3c, or for the local
  player in first person, nothing plays.
- An actor not standing plays nothing for an idle request (0x4b1b97: P01, P03, P08, O01, O02 or 0xc..0xf).

A corpse's D05 bypasses 0x4afc90: 0x490366 calls slot 0x20 (0x4afd50 -> 0x4096c0) with speed 0, holding the end pose.
Hailstone plays D05 once at rate 1 and holds its last frame.

#### Bone groups

`game/anim_layers.*`, `render/actor_layers.*`. Each actor splits its bones into two CBoneGroups:

- group 0, the upper body: classic tags whose part after the model code starts FA HE FI FO BI NE HI CH, but
  CHCHEST_; EQG tags CHEST* but CHEST_CHEST01, HEAD*, ARM*, NECK*;
- group 1, the rest: root, pelvis, legs, and unnamed points such as R_POINT.

Each group cross-fades on its own, linearly; asked for what it plays, it does nothing. The old track runs through the
blend. A played-out one-shot holds its last frame; a looping clip wraps. Checked: 0x4090f0 / 0x409260,
EnumerateBones @0x10040f80, BlendToAnimation @0x1003fcc0.

Which group plays what:

- Classic models (0x4b15b0): the animation asked goes to group 0, the "queued" one to group 1, the same except:
  - a rider: the rider animation 0x4a / 0x4e on the legs, the action on the arms;
  - a moving (+0x7c not 0) humanoid (body type 1) starting an action of table 0xdced38 (C02..C09 and 0x27..0x2c):
    nothing queued, the legs keep moving.

  Until group 0 plays such an action through (HasCompletedCycle, slot 0x1c), a humanoid's walk or run (table 0xdced88)
  starts on the legs alone and other requests wait; then group 0 cross-fades back (100 ms) and syncs (SyncToBoneGroup).
- New-style models (0x58afa0): a layer-kind-1 sequence (attacks, casting) plays on group 0 alone while group 1 plays
  a kind 2 one (walk, run, turn), or for a rider. A walk / run / turn then takes the legs while the action keeps the
  upper body (0x5890a0, 0x58ac90's modes). Another sequence (stand, idle) starts on both groups when either's mode is
  2, cutting the action (0x589410). Each group's cross-fade is (new.blendIn + that group's old blendOut) / 2.
- Bard songs (0x4b15b0's bard test):
  - An instrument song's animation (0x27 drum, 0x28 lute, 0x29 horn) plays on a classic or Luclin model only when not
    sitting (110), and for a bard (class 8) only with an instrument in either hand (PlayerState bit 0x10, item types
    23..26).
  - A singing song (0x2a..0x2c) always plays.
  - A new-style model has no such block (0x58afa0), but the original's landing path (0x593d10) still checks posture
    and a bard's instrument first.
  - The local player's bit comes from the inventory (0x5923f0) and is sent as OP_PlayerStateAdd /
    OP_PlayerStateRemove once the zone is ready (OP_ClientReady; stock EQEmu ignores them before). The server relays
    it.
- New-style riders select movement and idle sequences from mounted style column 5, using the mount's motion. The
  style column is part of sequence identity, so the same sequence in another column can start.
- A request that finds the upper body busy (`render/actor_layers.cpp` start_action):
  - Classic: 0x4b0900 decides whether it may start at once (`game::classic_may_interrupt`): the blend and rider
    animations, D05, 0x21 and 0x2c always; never over itself; else only over an animation of table 0xdcece8. 0x4b0900
    gets the number the packet asked for, while 0x4afc90, the queue and +0x10c0 use 0x4b1210's choice (a missing T06
    becomes T05; a CastNuke still interrupts C05).
  - Classic, refused while group 0's cycle is not through: actions 1..0xb and 0x2d..0x2f become the follow-up +0x10c4
    (the last wins); the movement ones (0x11..0x19, 0x21, 0x24, 0x4b, 0x4c) reset it to 0x20. The next stand request
    once the upper body is free plays the follow-up.
  - New-style: mode 1 records the sequence at +0x10c4 / +0x10cc, which nothing plays back, so it is dropped. Mode 3
    (same sequence and column) keeps what plays. 0x588e20 also plays nothing while group 0's sequence is of layer kind
    0 and there is no mount answer.
- The same animation asked again: BlendToAnimation leaves a group playing it alone, so an action still playing is not
  replayed.
- Classic P08 / O02 (0x4b15b0's idle block with 0x4b0860: `game::classic_plain_stand_after`):
  - A stand request plays the follow-up +0x10c4 when there is one.
  - Otherwise P01 when +0x10c0 is P01, C01..D05 (1..0x10), L01 (0x11), P03 (0x22), T04..T09 (0x2a..0x2f), L11 (0x4b)
    or none (0xfe); after anything else P08 (0x47): run, jump or fall, duck, posture change, instruments, a social,
    P08 itself.
  - An idle request (O01) after such an animation is O02 (0x48). A model without P08 / O02 plays P01 / O01.
  - The stand does not replace what plays on a corpse, during a posture change, or while an unfinished action plays
    that 0x4b0900 says it may not interrupt. The stand keeps the same clip's phase (0x4b1ab3).
  - When the stand plays the follow-up, +0x10c4 becomes 0x20 after a named animation, else 0x47.
  - The layers keep an OP_Animation action as +0x10c0 until it ends, unless a walk or run took the legs: after a
    social the stand is P08, after an attack P01.
- Ducking (PlayerZoneClient::ChangePosition @0x5a0bc0): a change to 'o' (111) plays the duck 0x18 (L08; CRCH on a
  new-style model) once, unless it or the duck walk 0x16 (L06 / CWLK) is playing; 'd' (100) from 'o' plays it
  backward. It is a plain play: a walk or jump replaces it (`game::choose_spawn_animation`).
- Corpses (is_corpse, or stand state 115) play the death clip D05 (D04, else P01) on both groups once and drop any
  action.
  - A spawn that dies while drawn plays it from the start, even if its model changes that frame.
  - One already dead when first drawn shows the last frame at once.
  - OP_Illusion rebuilds the actor; a lying corpse keeps its end pose (also on a held-variant change or redraw,
    `docs/unknowns.md`).
  - The cross-fade out of the death clip starts from the lying pose.

  Checked: BuildActorForSpawn @0x48f4b0 calls slot 0x20 at 0x490366 with D05, reverse 1, speed 0 for Type 2+ or stand
  state 115 (Death_SoundID +0x1110 cleared around it); CBoneGroup::SetCurrentAnimation @0x1003ff10 puts a reversed
  clip at its length; do_change_form @0x596b20 calls BuildActorForSpawn.
- Climbing: CollidingType 6 reaches 0x4afe60 / SelectMovementAnim, which plays L07 / CLMB forward going up and reversed
  going down.
  - Fresh moving remote spawns probe the world sweep for ladder and switch contacts; a still spawn keeps its contact;
    riders, corpses and NoCollide actors do not probe.
  - The probe follows the player's passable-ladder rule (`ZoneMoveWorld::apply_remote_rules`); other spawns' probes
    meet every door.
  - Hailstone, for frame cost: only spawns within the actor clip are probed (`WorldState::remote_probe_range`,
    `game::actor_clip_distance`, 550 at the default ActorClipPlane), and doors only after a ladder hit; not vehicles
    or NPC bodies.
  - Full remote collision correction and the physical-radius gap remain open.
- Other spawns' FeetWet / BodyWet (+0xa1 / +0xa2) are probed with each step (`WorldState::smooth`), so they swim and
  tread water.

## Character appearance

An illusion received while mounted unseats before rebuilding the actor and re-seats the saved mount after; a departed
mount clears it, so a reused spawn id cannot attach another mount (with `illusion_mounts`).

Disabling `UseLuclinElementals` also skips the implicit global NPC archive (`global5_chr`); an archive named in
`GlobalLoad.txt` still loads (docs/unknowns.md).

`game/actor_look.*` follows the ActorClient appearance functions (vectors `tests/appearance_vectors_test.cpp`);
`assets/character_parts.cpp` (Luclin and new models) and `assets/character_dress.cpp` (classic) apply them.

- Faces: face f is index f % 10 and variant f / 10 (split_face).
  - The head materials `<code>HE<variant 2 digits><index><piece>_MDF` replace face `from`'s: piece 1, then by race:
    2 and 4, 5 for Erudites, Trolls and Ogres; 3 (tinted) and 4, 5 for male elves and female dwarves; 4, 5 for the
    others; 2..8 for Iksar, Vah Shir and Froglok, whose body materials are then redone with the face index.
  - Male elves and female dwarves have no variant above 3, and take the beard colour on pieces 1 and 3 of a variant
    face.
  - A Vah Shir face 7 has its own hair part (IT<1000 + offset>).
  - A fixed body texture (NPCs) uses the HeadType variant for face index 0, matching the palette entry by the name's
    characters 0..4, 7 and 8.

  Checked: SwapFace @0x40d1a0, called by PlayerClient_DressSpawn with (face, 0, class).
- Eyes on Luclin models:
  - +0x150 finds the palette entries whose names, past the model code, start L_EYE (+0x140) and R_EYE (+0x144). A
    male Halfling's right eye is R_01; a Froglok's is ?EYE for both.
  - +0x15c puts `CHR_EYE<colour + race offset, 3 digits>_MDF` on one. Offsets: Dark Elf 20, Troll 100, Ogre 80, Iksar
    40, Vah Shir 60, Froglok 120 (materials in lgequip and frogequip).
  - Eye patches: the right eye of a male human's face 3 or male Barbarian's face 6 takes colour 0xd1; a male dwarf's
    face 5 or male Barbarian's face 4 takes 0xcf.
  - ApplyFacialFeatures sets both eyes from +0xb.

  Checked: ActorClient::sub_0040c350; +0x150 @0x10040c50; +0x15c @0x10040d50.
- Helms on classic models:
  - SwapHead's head DAG tints every palette material named HELM* or CHAIN* with the head's armour colour; a helm item
    (IT<n> on the head point) takes the same colour through ChangeDag.
  - 0x40a5b0's "keep head" output changes nothing.
  - On Luclin models the head table's materials 4..27 name IT<5000 + m + offset> parts the RoF2 files lack, so no
    helm shows, as in the original.

  Checked: actor +0x14c @0x10043250.

## Mercenaries

`game/mercenary.*`, `net/game_connection_merc.cpp`, `app/lua_ui_merc.cpp`; vectors `tests/merc_vectors_test.cpp`
(every handler and sender of the mercenary manager).

- The manager: has-mercenary flag, state, restoration timer, active record, owned list, slot count (reset to 1),
  current index (-1), liaison id, the timer packet's interval and +8, the last upkeep tick.
- States: 5 active, 1 suspended, 2 suspended and not allowed in this zone, 0 dead; 3 and 4 only from
  OP_MercenaryTimer. 0, 1, 2 and 4 count as no active mercenary.
- Zone-in keeps the owned list and zeroes the rest (state 5, active since FastTime), then sends
  OP_MercenaryTimerRequest and 0x4820 {8}.
- Every frame while active, the time left goes down by GetTickCount - last tick, stopping at 0 (unsigned compare).
- Costs are gold: cost / 10 platinum and cost % 10 gold (signed). Upkeep reports print FormatDurationLong ("%d hours,
  %d minutes, and %d seconds" and shorter forms).
- A mercenary spawn is an NPC with IsMercenary that counts as a player for buff slots and PC-only checks. Its
  nameplate is amber (`#e1b511`; docs/graphics.md "Name plates"); in the group window it follows its owner
  (`game/group_rules.h`).

#### The mercenary windows

- TimeLeftText is FormatDurationShort(time left) ("%uh %um", "%uh", "%um %us", "%um") in state 5 with time left, else
  "0m 0s".
- Its tooltip in state 5 is "Time left until you get charged upkeep cost: %dp %dg." (or %dp / %dg / "This
  mercenary's upkeep cost is 0."); in 0 / 1, 2, 4 the revive / unsuspend lines.
- The manage window's buttons follow UpdateButtons: 0..2 disable all, then Unsuspend / Revive and Dismiss; 3 the
  suspend button; 5 all, "Suspend", call for assistance unless auto-assist is on.
- The suspend button checks combat, raid, the restoration timer, state 2.
- The owned list has 6 rows: each mercenary ("X" on the current one, "race - type", its first four description lines
  as the tooltip), then "Empty" up to the slot count, then "Locked".

## Inputs the client cannot provide yet

- An item's current power: only its maximum comes, so the power line shows 100%. A deliberate difference: the
  original's `current * 100 / max` is an int32 product that wraps past 21.4 million; the client multiplies in 64
  bits.
- The server's PvP ruleset.
- The server's regen rule for `IsRestingForFastRegen`.
- The 21 extra item lookups (tribute-like containers).
- The caster level of buffs that arrived without one (OP_BuffCreate; the player's level is used).

#### The player's stored and shown HP

The sheet's stored HP (profile +0x3394 BaseHP) is the server's current HP (`LiveState::hp`), as OP_HPUpdate's value is
stored there. Left at 0, the low-health slowdown would apply (AGI halved, weight factor 0.5).

The player's own HP is OP_HPUpdate's points plus the item sum (`DerivedStats::hp_bonus`), as Cur_HP and Max_HP add it:
EQEmu sends the player's HP without the items'. A server extension's vitals (`CharacterVitals`) are the whole value.
Nothing is added while dead.

The app passes the item sum each in-zone frame (app/stats_ui.cpp `share_own_hp_items`). Everything that shows,
reports or tests the player's current HP reads `net::GameConnection::own_hp`:

- the windows: labels 17, 18, 19 and 70, gauge 1, the ability book's HP percent;
- the player's spawn HP percent (`Spawn[...].PctHPs`, a self target, a plugin's spawn);
- `Me.CurrentHPs`, `Me.MaxHPs`, `Me.PctHPs`, `hailstone.player()`'s `cur_hp` / `max_hp`, a plugin's `HailstonePlayer`;
- the knocked-out test (`player_down`);
- UseSkill's Cur_HP check, the fall damage knock-out floor, CastSpell's Max_HP and the hit sound's HP.

The bare packet value goes below 1 while a geared player still stands, so none of these read it. The regeneration
ticks and the stored HP keep the packet's value.

## Guild banners

The Guild window's Banner page requests the guild's design, the active banner and its timer, and a kit list filtered
by category (the player's `Resources/BannerCategories.txt`) and inventory. Both UI packs show the active banner and
kits.

The two design layers are previewed with the game folder's textures, with pattern and colour controls; Cancel
discards the draft. Saving needs a modification item reported by the server (only an item ID of -1 refuses it). The
next received design replaces the preview; a status update asks for the kit list and status again.

Change, Plant and Remove need separate guild permissions and The Buried Sea. Plant also checks zone placement and
floor proximity; the server decides everything else (where, nearby members, kits, removal, benefits, portal travel).
The countdown is display-only.

An inactive kit can stay listed with Show All. Banner state clears with the guild window, membership or zone. Stock
EQEmu leaves the kit list empty and the actions disabled. The Guild Hall portal uses the server's merchant,
interaction and zone-transfer paths. Planted actors use the cloth background and emblem with their received colours.
See [the RoF2 packet notes](protocol/rof2-packets.md#guild-banners).

## Fellowships

Fellowship state comes from the server. Create, invitations, MOTD, leadership, removal, leaving, disbanding, XP
sharing and camps send native requests and wait for the next roster. Stock EQEmu sends no RoF2 fellowship packets, so
there the window stays empty.

- The first roster member leads. Members toggle their own sharing; the leader may toggle another's.
- Confirmation dialogs recheck fellowship identity and permission.
- An invitation shows its dialog only when the inviter's spawn is known; yes sends action 3, no action 4. Checked:
  `Handle_Opcode_0x62ab` @0x667db0, `Send_Opcode_0x7121` @0x668710.
- `/fsay` uses channel 21 and needs membership, else eqstr 4573 in colour 342 (`FellowshipSay` @0x524330).
- MQ's `fellowship` spawn filter matches the roster by name.
- The window opens with Ctrl+Shift+F (Options > Keys) or the EQ menu's Fellowship entry.

#### The fellowship window

Both UI packs show sortable members, online/last-seen, sharing/capped state, a 1023-byte MOTD draft, exact normal/AA
vitality pools and caps, camp kits and the active camp's location and time left. The inventory shows vitality bars
through native gauge types 147/148 (AA from level 51), the truncated pool/cap ratio as in the original; zero caps draw
empty.

Campsite categories come from the layered `Resources/campsitecategories.txt`; selecting one sends its category bit.
Kit descriptions use DBString type 0. Missing resources keep an all-category filter and the server's kit name.

The server enforces admission, XP, supplies, placement, cooldown and expiry; the camp countdown is presentation only.
Zoning keeps membership but clears pending invitations and kit queries; disconnecting clears everything.

#### Fellowship extensions

**Extension beyond RoF2:** up to 64 members instead of twelve (capacity, not an entitlement to join). Twelve-slot
messages are read as RoF2 reads them; expanded messages follow the layout in `docs/protocol/rof2-packets.md`. EQEmu PR
#5134 "TOB: Fellowships" (open, unmerged; a protocol reference only) stays at twelve.

**Extension beyond RoF2:** a 1080-byte invitation (that PR's form, inviter at 4 when 12 is zero) is also accepted, and
for it alone a dialog without the inviter's name shows when the inviter is not in the zone. The RoF2 0x434-byte
invitation from an unknown spawn shows nothing.

Not tested live.

## Spirit Shrouds and Monster Missions

A shroudkeeper on a supporting server opens the Spirit Shroud picker: offered forms, unlock progress, levels,
abilities and equipment previews. Returning can restore the original form.

A shared task with a Monster Mission catalog adds Monster Select to the Quest Journal: required and optional roles,
counted against each role's capacity; the server acknowledges the assignment and sends the form change when the
character takes the role.

A form change replaces the server-supplied profile and carried items (spells, buffs, AAs); character select uses
alternate appearance data when supplied. The inventory shows shroud progress. At an eligible banker the Shroud Bank
inspects and retrieves items the server stored; the server confirms each withdrawal.

See [the RoF2 packet layouts](protocol/rof2-packets.md#spirit-shrouds-and-monster-missions-netshroud_packetsh)
and [the remaining server-compatibility and gameplay checks](unknowns.md#spirit-shrouds-and-monster-missions).

## Adventures (LDoN)

#### Recruiters and the request window

Alt+V (`TOGGLE_ADVREQUESTWIN`, index 351) toggles the adventure window. A right click on a class-60 recruiter within
20 units in XYZ (inclusive) requests its introduction. Risk 1/2 and objective 0..4 are sent as chosen (the risk
selector is one-based). Introductions and offers use the shared 500 ms recruiter range check; assigned status stays
open beyond range.

Closing the window runs RoF2's `Send_OP_LDoNButton_0041fe10`:

- an unaccepted offer is declined and the recruiter's words dismissed, forgetting the recruiter;
- closed while a request awaits its answer, it keeps the recruiter; the answer shows the window again (`0x420130`,
  `0x4201b0`);
- an offer with no recruiter (after zoning) is declined at once.

Closing the status window keeps the mission; leaving uses a separate confirmed control. The server picks the mission
and decides eligibility, acceptance and departure.

#### Adventure status

- AdventureData selects the entry timer when nonzero, otherwise the completion timer.
- Displays round remaining seconds up to whole minutes and clamp at zero (`0x618a00`, `0x618ae0`).
- Progress counts are kept whenever they come and shown only during completion (`0x4204e0`).
- The risk selector shows AdventureData's risk: nonzero is high. Stock EQEmu sends 0 normal / 1 high, not the
  request's 1 / 2, which RoF2's `0x618790` assumes.
- The compass uses the server's zone-local XYZ/flag.
- AdventureFinish clears assignment, timers, compass and progress and closes the window (`0x4206eb`).
- Zone changes clear the old recruiter and zone-local data; selecting another character clears cached state.

#### Adventure stats

Themes 1..5 then Totals: win rank, successes, failures, win percentage, percentage rank and theme point balances.
Profile data supplies initial points. Point updates replace balances and request fresh stats, open window or not, as
RoF2 does (`0x4208f5`), within the ten-second guard.

After a granted leave stock EQEmu sends nothing, so the window shows the adventure until the next AdventureData
(docs/unknowns.md). Rank zero shows a placeholder. Totals use wide arithmetic; row percentages round to binary32 before
one-decimal formatting.

#### Adventure leaderboard

Categories interleave wins and percentage within each theme; risk is All/Normal/High (stock EQEmu combines risks).

- Row ranks are competition ranks (1, 1, 3), counting empty wire slots.
- Wins ties need equal wins and failures; percentage ties equal binary32 percentage and wins (`0x6175c0`).
- The personal rank is the header value, unless the player appears in the rows; then the computed row rank (this also
  matches RoF2 when EQEmu's header carries a zero-based rank).
- Selectors and refresh keep their selection across window closes.

Hailstone queues the latest changed category during a request instead of disabling selectors as RoF2 does. Refresh
guards and missing-reply recovery: `docs/protocol/rof2-packets.md`.

## Optional host movement profiles

An active server extension may provide a bounded follow profile; with none, the stock follow path is unchanged.

- Enhanced follow starts only through `/follow` or an admitted `manual_follow` control, on a current living player
  within the supplied start range.
- It uses an absolute planar stop distance with a restart margin and time-scaled turn and pitch smoothing.
- A profile can allow ungrouped targets, keep the player's run mode and cancel on manual movement.
- No pathfinding; the ordinary movement/collision controller moves the actor.
- An optional mount profile can remove acceleration ramping without raising the speed limit.

See docs/plugins.md "Server add-on services" for map visibility, merchant click interception and raid controls. Find
eligibility also applies to an active guide: each update checks the policy before reading the destination, and denial
retires the trail. All private protocol, class policies and content names belong to the extension.
