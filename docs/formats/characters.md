# Character models (WLD, Luclin, EQG) and appearance

Implementation:

| File | Role |
|---|---|
| `src/assets/character.{h,cpp}` | archives, models, clips |
| `src/assets/character_dress.cpp` | appearance: armour, faces, helms, robes, tints, held items, Luclin layers and parts |
| `src/assets/eqg_character.{h,cpp}` | EQG models |
| `src/game/appearance.{h,cpp}` | which value drives which part, OP_WearChange |
| `src/game/race_models.{h,cpp}` | with the generated `race_models.inc` (`tools/gen_race_models.py`) |
| `src/render/actor_renderer.{h,cpp}` | with `shaders/actor.{vert,frag}` (GPU skinning, tints) |

Tools: `chrdump` (`--luclin`, `--names TEXT`), `hailstone --model` (appearance per model, `--dress`, `--luclin`).
Tests: `tests/character_test.cpp`, `tests/appearance_test.cpp`. Fragment layouts are in `docs/formats/wld.md`.

Credits:

- Clip assembly, donor clips, rigid skinning, the bottom-up character BMP rows and +X as the model front follow
  OpenEQ (MIT OR Apache-2.0, `src/assets/character.cpp`, see `THIRD_PARTY.md`).
- Which models borrow clips comes from LanternExtractor's (MIT) `animationsources.txt`, copied to
  `tools/data/lantern_animationsources.txt`.

## Archives and load order

`global_character_archives()` follows the client's start-up order:

1. `Resources/GlobalLoad.txt` (`phase,?,flags,file,text`), phases 1 to 4 in file order. It keeps the entries whose
   file contains `_chr` and is not an `.eqg` (globalfroglok, globalske_chr2, ske_chr, the 3rd-phase NPC archives,
   global6/4/_chr, global2/3_chr ...).
2. `global5_chr` and `global7_chr`, which the client loads next to the Luclin models. Then the extended armour
   textures `global17_amr` … `global23_amr` (`Global%d_amr`, textures such as `HUMCH1701` for materials 17+).
3. `Resources/GlobalLoad_chr.txt` (`count`, then `code,file`): S3D entries only. When the code and the file share
   their first three letters (`cwr,cwr`) the file is an EQG.

Per zone, `zone_character_archives()` adds `<zone>_chr2`, `<zone>2_chr`, `<zone>_chr` and the S3D entries of
`<zone>_chr.txt`.

The first definition of an actor or track name wins. The app keeps the global archives between zones
(`CharacterArchiveCache`).

#### Memory and caches

- Character archives in the asset folder are memory-mapped read-only (`PfsArchive::storage_for`,
  `platform::MappedFile`); a server overlay's (`servers/<id>/assets`) are read into the heap, since the server files
  sync replaces them while the client runs, which Windows refuses for a mapped file (docs/hardening.md "Mapped game
  archives").
- Each WLD is validated at load but kept only as a compact index; its payload is decoded on first use
  (`WldFile::deferred`, under `WldFile::ReadScope` leases). Decoded payloads are kept after the last lease in a
  least-recently-released set within `WldFile::kDefaultRetainBudget` (128 MiB; `WldFile::set_retain_budget`, 0 drops
  them at once), which is not emptied on a zone change. Ordinary zone WLDs stay eager.
- What was built from the global archives alone (models, appearances, item models) is shared across zones
  (`SharedCharacterCache`). Each entry records the names its build looked up (`CharacterLookups`), and a zone's library
  takes an entry only when its own archives define none of them, so a zone's override never comes from, or goes into,
  the cache. The renderer keeps those meshes and textures on the GPU across a zone change (`ActorRenderer::release_kept`).
- The global archives' name tables are built once (`CharacterArchiveIndex`, again after `clear()`); a zone's library
  indexes only its own archives on top, and resolves exactly as one library over all the archives would.

#### Equipment and Luclin archives

The GlobalLoad.txt entries whose flags' fifth letter is `E` (equipment: `gequip*`, `frogequip`, `global_obj`,
`loyequip`, `ldonequip`, `gatesequip`) are loaded too. They hold the `IT<n>_ACTORDEF` item models.

With Luclin models on (the app always asks; eqclient.ini decides), `luclin_character_archives()` reads eqclient.ini
`[Defaults]`:

- `AllLuclinPcModelsOff`;
- `UseLuclin<Race><Male|Female>` (default TRUE; race names: Human, Barbarian, Erudite, WoodElf, HighElf, DarkElf,
  HalfElf, Dwarf, Troll, Ogre, Halfling, Gnome, Iksar, VahShir).

It loads `global<code>_chr2.s3d`, `global<code>_chr.s3d` for each enabled code, plus the part archives `lgequip`,
`lgequip2`, `lgequip_amr`, `lgequip_amr2`, between GlobalLoad phases 2 and 3. Since the first definition wins, the
Luclin `HUM_ACTORDEF` then replaces global_chr's.

EQG models are not in these archives: see "EQG models" below.

## Model

`<CODE>_ACTORDEF` (0x14) → 0x11 → skeleton 0x10. Example, HUM in `global_chr.wld`:

- **Nodes.** 25 nodes. Each names a 0x13 track (`HUM_TRACK`, `HUMPE_TRACK` pelvis, `HUMTH_R_TRACK` ...,
  `HUMR_POINT_TRACK`, `HUMHEAD_POINT_TRACK`, `HUMSHIELD_POINT_TRACK`). The parent of a node is the node whose child
  list holds it. A bad index, two parents or a cycle rejects the model.
- **Rest pose.** Frame 0 of each node's own track: local = T · R · S (quaternion normalised), model = parent · local.
  The rest pose is a T-pose about 6.7 units tall with the origin about 3 units above the soles (HUM z −3.00 … 3.67).
- **Meshes.** The skeleton's mesh list (flag 0x200): `HUM_DMSPRITEDEF` (body, 669 vertices) and
  `HUMHE00_DMSPRITEDEF` (head, 122 vertices). Without the flag, each node's sprite is used, and a mesh without vertex
  pieces follows its node.
- **Skinning.** Every vertex belongs to one node through the 0x36 vertex pieces `{count, node}` and is stored in that
  node's space. Skinning is a single bone matrix per vertex. No inverse bind matrices.
- **Materials.** From the mesh's palette, grouped per render group like zone meshes (`wld::build_mesh`). The
  palettes also list robe (`CLK04xx`) and helm materials that the default render groups do not use.

HUM's materials are nearly all render method 0x80000014 (opaque):

| Material | Part |
|---|---|
| `HUMCH0001` | chest |
| `HUMLG000n` | legs |
| `HUMFT000n` | feet |
| `HUMUA0001` | upper arms |
| `HUMFA0001` | forearms |
| `HUMHN000n` | hands |
| `HUMHE000n` | head |

The texture number encodes variant and part (`HUMCH0001` = chest, variant 00, piece 01).

A static (non-skeletal) actor without a mesh is a build error. A skeleton with no body that can show is still built
(below).

#### A skeleton with no drawable body: IVM

Race 127's `IVM` (the enchanter pet; race 600 uses it too) has 26 bones and no named clips. Its meshes are 0x2c
fragments (`IVM_DMSPRITEDEF`; docs/formats/wld.md, "0x2C mesh"). Nothing of that body would show in the original:
their only material, `IVM_MDF`, is masked (0x80000013) with an 8 x 8 texture all of palette index 0, the masked
colour. Such a body (all 0x2c meshes, every material invisible or masked with no texel that draws) is dropped when
the model is built, so IVM stays a body-less skeleton (`bodiless_skeleton`). RoF2's global_chr goes further: its
two IVM meshes have no polygons at all (their skin pieces count 0), and a 0x2C mesh without polygons is left out. A live folder's `ivm_chr.s3d` has an IVM
of its own, a 0x36 body that is kept.

- Dressing skins the primary and secondary items onto its `R_POINT` / `L_POINT` bones, so the held weapon shows with
  an invisible body; with no item held nothing is drawn.
- It borrows ELM's clips (the prefix rule below), matched by bone name: both have 26 bones and share 25 track names
  (IVM has `HE00` where ELM has `GUILD_POINT`).
- Its culling and shadow radius is the farthest its clips can carry the dressed items. A world click takes it through
  the selection sphere of a spawn without a model, not its weapon's triangles (app/spawn_pick.h). Its nameplate stands
  at its eye height.

#### Orientation and BMP rows

Character BMPs are stored bottom-up relative to the model UVs, so the renderer flips the rows of BMP files. It does
not flip DDS files, even when named `.bmp`. Without the flip faces and chests are upside down.

The model front is +X (HUM's toes are at +X once the pelvis' 90° turn is applied). The model is placed with
T(position) · Rz(heading_to_yaw(heading)) · S(scale), using `game/coords.h`'s heading, which is 0 = +X,
counterclockwise.

## Clips

An animation clip is a set of 0x13 tracks named `<clip><bone track>`, for example `L01HUMPE_TRACK`. A clip code is a
letter and two digits:

| Code | Use |
|---|---|
| L01, L02 | walk, run (L03–L09: jump, fall, crouch-walk, climb, swim ...) |
| P01 | stand (passive); O01–O03 idle variations |
| C01–C11 | combat (kick, pierce, slash, bash, ...) |
| D01–D05 | damage; D05 death |
| P02–P08 | sit, stand up, crouch, ... |
| S01–S28 | social emotes; T02–T09 spell casting |

The library indexes every track whose name starts with a clip code by the rest of its name (the bone track), so a
model's clips are found from its bone names. For HUM, `global_chr.wld` holds only L01, L02 and P01; everything else is
borrowed.

#### Donors

A model borrows from a donor model listed in LanternExtractor's `animationsources.txt` (HUM, BAM, ERM, HIM, DAM, HAM
... → ELM; HUF ... → ELF; TRM, TRF, OGM → OGF; HOM, GNM → DWM; ...). The donor's bone track is the model's with the
code replaced (`HUMPE_TRACK` → `ELMPE_TRACK`).

The code is replaced by position: the bone name's first letters, as many as the model's code has, whatever they are.
A few skeletons name a bone with another model's code. South Karana's Halas female (HLF) has `HUFBI_L_TRACK`,
`HUFL_POINT_TRACK` and `HLMHEAD_POINT_TRACK`; matched by its own code alone, its left upper arm took no ELF track and
stood out at its rest angle in every clip. LanternExtractor matches tracks to bones by position too and renames those
three bones (`CharacterFixer.FixHalasFemale`). Among the global, Luclin, South Karana and Halas models this changes
only HLF's clips; how the original names the borrowed track is not traced (docs/unknowns.md).

The RoF2 client's own list (`ActorAnimation::GetAlternateAnimTag` @0x406a60) names some donors Lantern's leaves out;
its unconditional three-letter ones are added from `tools/data/rof2_animation_sources.txt` (FRF → FRM, SHF → SHM,
WAF → WAL, KES → KEM and others).

`game::animation_source` asks in this order:

1. the `flag` exceptions (Luclin player models, below);
2. that table;
3. the rule of `GetAlternateAnimTag`'s wood-elf prefix group. A code the table does not name under one of those
   prefixes (DA, HI, HA, HH, QC, NG, FP, EG, BR, HL, ZO, IV, GF, GE and the three-letter FEM, FEF, GHM, GHF) takes
   the wood elf's two letters and keeps its last (`0x4077f0`: race 127's IVM → ELM).

The codes the original tests before that group (ERO, SIR, HAG, ABH, BRI, COK, DR2, CCD, BWD, PRI) are all in the
table, which answers first.

#### Own tracks and borrowed clips

A clip for which the model has at least one track of its own uses only its own tracks. Otherwise the whole clip is
borrowed (OpenEQ). Bones without a track keep their rest pose.

#### Luclin player model donors

A Luclin player model is one with a `TUNIC_POINT` bone (passed to `GetAlternateAnimTag` as its `flag`;
InitAnimationTracks @0x407800). Such a model takes other donors for some codes (`game::animation_source`):

- HU* and ER* take the human's clips (ERM → HUM, ERF → HUF; HUM and HUF none but their own).
- BA* none but its own.
- SKE HUM's.
- Every other code takes its donor from the table or the prefix rule as without the flag (HIM, DAM, HAM, IVM ... →
  ELM).

(With the wood elf's clips a Luclin erudite's face and beard bones go wrong; the human's key them correctly.)

#### Skeleton matching

A track lends only to its own skeleton: a track in another WLD that defines the same code with a different skeleton
is not the model's (live's `wol_v2_chr` WOL shares the root and point names with the classic WOL).

A borrowed clip is lent only when at least one of its tracks is in a WLD whose own donor skeleton has an equivalent of
at least half of the model's bones (or that does not define the donor). Otherwise a Luclin model borrowing from the
classic ELF (which shares only the root, PE, CH, NE and HE) would throw or kick in a T-pose.

#### Frame time

The 0x13 frame time (flag 1) of the first multi-frame track, otherwise 100 ms. All classic clips use 100 ms except
T08/T09 (50 ms). The clip length is the longest track. Shorter tracks wrap.

**Long tracks** of some archives play at half their frame time: a track of more than 15 keys from an archive the
original opens with the loader byte +0x30 set (EQGraphicsDX9 `0x1001b26e`, `CS3DFile::Open` `0x100c0470`).
`CDisplay::InitWorld` sets that byte for:

- the Luclin player models' `global<code>_chr2` and `global<code>_chr` (`0x492507`, `0x492597`);
- a GlobalLoad.txt line whose fourth FLAGS letter is `T` (`0x492063`; stock: `GlobalPCFroglok_chr`,
  `GlobalSKE_chr2`).

The classic `global_chr`, the armor, Global NPC Models and zone `_chr` loads leave it 0 (`assets::long_track_archives`).

(The Luclin clips are keyed about twice as densely as the classic ones.) The original halves per track; a clip here
has one frame time, taken from the track named above, so a track of 15 keys or fewer in a halved clip plays at the
halved time too.

#### Sampling

Sampling interpolates between frames: slerp for rotations (shortest path), lerp for translation and scale. Looping
clips wrap; the corpse pose (D05) holds its last frame.

Newer models use a fallback race for missing tracks (`<anim>_<CODE>` retried with HUM → 522); the classic clips do
not.

### Animation key storage

Key arrays are shared between copied clips and models (`AnimationTrack`; a builder detaches before changing keys).
`CharacterModel::compact_animations` interns bit-identical tracks within a model (no cross-zone table), and stores a
constant track as one key with its original logical length. This affects storage only: lookup and sampling are
unchanged. Model memory estimates count shared key arrays once per model, so per-model estimates cannot simply be
summed.

## Luclin models

A Luclin model (`HUM_ACTORDEF` in `globalhum_chr.wld`) is a WLD skeletal actor like the classic ones, with 104 bones
and one body mesh. The bones include face bones, `HAIR_POINT`, `BEARD_POINT`, `HEAD_POINT`, `TUNIC_POINT`,
`CHEST_POINT`, `SHOULDL/R_POINT`, `GAUNTL/R_POINT`, `LEGL/R_POINT` and robe bones.

#### Takes

Track names carry a fourth letter, the take: `C01AHUMPEBIP01_TRACK`, `L10BHUM…`. The library indexes `<code3>` and
`<code4>` readings. A model plays `C01A` (else `C01B`) as `C01` when it has no animated `C01` of its own (OpenEQ's
alias rule). The `_chr2` archives hold the takes.

The take is the holding column (`InitAnimationTracks` @0x407800 binds a tunic-point model's A..H to columns 0..7):

- `PlayerClient_UpdateHoldingAnimation` @0x58d780 gives a spawn holding nothing column 0 (A), and one holding
  anything in either hand 1..7 (3 with both hands).
- `0x4b1210` takes column 1 (B) for a column the model has no take of. No model ships C..H.

So a Luclin model holding something plays its B takes (`CharacterModel::held_aliases`, `clip(code, holding)`,
`game::holds_item`).

#### Layered materials

Materials are palettes of layers. `HUMCH0001_MDF` names a bitmap whose file list is the skin (`humchsk01.dds`) and an
armour layer with alpha (`humch0001.dds`). A `_LAYER` suffix on a name is dropped. They are drawn as
`lerp(t1.a, t1.rgb × Tint.rgb, t0.rgb)`, lit after; `Material::tint_layer` is the second texture.

Heads: the material is `<code>HE<selector><piece>_MDF`, as OpenEQ names them.

- The selector is 0 (Erudites: the hair style, scalp glyphs; Barbarians: face / 10).
- The face is the piece's tens digit (`HUMHE0011`).
- The face is swapped in as the original's SwapFace does (`game::swap_face`, `game/actor_look.h`).

Only the Luclin model WLDs, `globalpcfroglok_chr` and `*_amr*` WLDs are searched.

#### Dressing a Luclin model

Dressing (`game/actor_look.h`) runs over `assets::DressEnv` (`character_parts.cpp`), which stands for the model:

1. The body materials for slots 1–6 from material 0: replace `<code><part><mm>01_MDF`, with more passes for the piece
   digit. Vah Shir and Froglok use `<code><part><mm><v>1_MDF`.
2. The facial features:
   - hair `IT<1000 + style + offset>` on `HAIR_POINT`;
   - beard `IT<2000 + …>` on `BEARD_POINT`;
   - Vah Shir (face ≠ 7) and Froglok `IT0` with no hair;
   - colours from a 24-entry table within the race's allowed hair colour range.
3. The armour textures (the hair goes when a helm took):

| Part | Item | Point |
|---|---|---|
| helm (with show_helm) | `IT<offset + 5000 + m>` | `HEAD_POINT` |
| tunic / robe | 4010 + m / 4000 + (m − 10) | `TUNIC_POINT` |
| plate chest | 6000 | `CHEST_POINT` |
| shoulder pairs | 7000 + 2m | |
| wrist pairs | 8000 + 2m | `GAUNTR/L_POINT` |
| legs | 4020 + m | `PELVIS_POINT` |
| feet pairs | 9000 + 2m | `LEGL/R_POINT` |
| hands | `0` | |

`offset` is the race/sex item offset: 30 for females, plus Human 360, Barbarian 540, Erudite 480, Wood elf 240, High
elf 300, Dark elf 180, Half elf 60, Dwarf 120, Troll 660, Ogre 720, Halfling 780, Gnome 0, Iksar 420, Vah Shir 600,
Froglok 840.

- Dag point names are `<code>HEAD_POINT_DAG`, …, `FIHANDL/R_TRACK`.
- Parts come from lgequip (Luclin), frogequip (Froglok) or EQG (hero's forge).
- Every part's `SetTint` colour multiplies it (RGB 0 is untinted).

#### Parts and bones

A skeletal WLD part's bones follow the body bone of the same name (`IT1012HAIR1_TRACK` → `HUMHAIR1_TRACK`), or else
the dag point. Static parts hang off the point.

EQG pieces (hero's forge) on Luclin models: each vertex goes from the piece's bind space into its bone's space, times
the NewArmorAdjData matrix. The bone is the piece bone's match (`CHChest` → `<code>CHCHEST_TRACK`), else the nearest
named ancestor, else the point.

A live client's colour of a piece (a `Resources/Layers/*.txt` row `piece^base^layer`) is its base mesh with the layer's
textures. A chain of such rows is followed at most 8 deep (`EqgCharacterStore::kMaxLayerDepth`; live rows are one
deep).

## EQG models

#### Hero's Forge armour lookup

Hero's Forge attachments use `EqgCharacterStore::armor_part_mesh`, whose layer-row alias takes precedence over a
direct item model or WLD actor of the same name (only geometry and layer are resolved; the name and race/sex
adjustment stay the attachment's). Ordinary held items, hair and NPC parts keep their direct-first lookup
(`part_mesh`), even when a modern armour mapping reuses their IT number.

- **Base mesh missing.** The row cannot be followed (`EqgCharacterStore::armor_alias` is false), so the piece resolves
  as with no row: the direct EQG model, or on a Luclin model the WLD actor, plainly lit. A Hailstone policy for mixed
  installs (RoF2 reads no layer rows).
- **Base present, layer unavailable.** A direct model with matching bone, piece, vertex and triangle counts keeps its
  legacy rendering; one with a different layout is rejected.
- A usable modern mapping always takes precedence.

The modern/v2 armour revision uses the existing EQGM/EQGL formats; no game files are changed or supplied. Two materials
of the same name and diffuse share a slot only when their surface maps and parameters agree (`append_pieces`), so a
plain part never takes an armour part's maps.

#### Layered armour

Layered armor applies the layer to every material of a copy of the base mesh.

- The layer's supplied C/N/E textures replace the corresponding base shader properties; omitted textures retain the
  base maps.
- EQGL v3 supplies shininess and environment strength 1; v4 stores both. These override existing material properties
  (`SetLayer` @0x10048130). The layer's environment strength is copied as stored, unlike the initial mesh property's
  doubled value.
- The parser retains these scalars only for v3/v4; older scalar tails remain unverified.
- Ordinary EQG NPC texture variants keep their existing diffuse-only layer application.
- Armor surface maps and highlights are used by modern lighting (docs/graphics.md "EQG surface maps"). The classic
  look retains its existing lighting.

#### Where EQG models come from

Races whose code has no WLD actor load from EQG archives (`assets::EqgCharacterStore`). The mesh is the `EQGS`/`EQGM`
entry `<CODE>_ACTORDEF` of `Resources/OnDemandResources.txt` (`archive^member^name^type`, e.g.
`hnm.eqg^HNM.MDS^HNM_ACTORDEF^EQGS`), else `<code>.eqg` with `<code>.mds` / `<code>.mod`.

Examples: rodents (race 415, `RTN`, from `qeynos_chr.txt` `rtn,rtn`) and Qeynos guards (566, `HNM`/`HNF`, new-style
flag 0x08).

#### Mesh layouts

All little-endian; after OpenEQ's `modern.rs`:

- Header `magic, version (1–3), string bytes, material count`, then `bone count, piece count` (`EQGS`) or
  `vertex count, triangle count, bone count` (`EQGM`).
- The string table.
- Materials `{id, name, shader, property count, {name, type, value}}` (type 2: a string offset;
  `e_TextureDiffuse0` is the texture).
- Bones `{name, next sibling, child count, first child, translation xyz, rotation xyzw, scale xyz}`.
- Per piece (`EQGS`: `{main flag, name, vertex count, triangle count, assignment count}`):
  - vertices `{position, normal, [v3: colour u32], uv, [v3: 8 bytes]}`;
  - triangles `{3 indices, material (−1 none), flags}`;
  - per vertex `{count ≤ 4, 4 × {bone, weight}}` (weights normalised).
- An `EQGM` puts its single piece's vertices and triangles before the bones.

Stored rotations are the conjugate of the usual convention.

The v direction depends on bones:

- A file with bones stores v running up from the bottom of the texture, so the reader turns v into 1 − v.
- A file without bones (a held item, a hero's forge helm or bracer) stores v from the top, as the zones' `.mod`
  objects do, and is read as stored.

Vertices are in bind (model) space, so the renderer multiplies each bone's animated matrix by the inverse bind
matrix. Vertices blend up to four bones.

#### EQGA animations

`EQGA` layout: `magic, version, string bytes, track count, [version > 1: u32]`, strings, then per track
`{frame count, bone name, frames {time ms, translation, rotation, scale}}`. Tracks match bones by name
(case-insensitive).

Resampling:

- Clips are resampled to fixed frames at the longest track's mean key spacing, clamped to 10..200 ms.
- The last frame sits at the .ani's last key time (`AnimationClip::frame_times_ms`, `duration_ms`), not on the
  resampled grid, so a clip keeps its exact length and a one-shot shows its end pose.
- A clip is cut at 4096 frames (`eqg_character::kMaxClipFrames`; stock clips run seconds), so a broken file's key
  times cannot ask for millions of poses.

Names are `<ANIM>_<take>_<n>_<CODE>`. Only `_BA_` takes are used, `_1` (IDLE `_2`, `_3` become O02, O03), mapped by
`eqg_character::clip_code`:

| Clips | | | |
|---|---|---|---|
| STND P01 | IDLE O01 | WALK L01 | NRUN L02 |
| JMPA L03 | JMPU L04 | FALL L05 | CWLK L06 |
| CLMB L07 | CRCH L08 | SWIM P06 | NSIT P07 |
| KNEL P05 | CRMP D05 | FLCH D01 | STUN D02 |
| KICK C01 | PATK/STPR C02 | SL2H C03 | BL2H C04 |
| SLPR/SATK C05 | BLPR C06 | BASH C07 | MHA1 C08 |
| NBOW C09 | RKCK C11 | MCST T02 | GCST T03 |
| DCST T04 | | | |

A model's own animations come from the index and `<code>.eqg`. For clips it lacks, the race table's animation
fallback race is followed (flag 0x04): HNM → race 1 (HUM, no EQG anims) → race 522 (DKM, `dkm.eqg`/`dkm_anims.eqg`).
So guards walk with Drakkin animations.

#### Pieces and variants

An EQG model has body pieces `<code><nn>` and head pieces `<code>HE<nn>` (HNM: HNM00, HNM07, HNMHE00–HNMHE07, each
with its own materials).

The appearance picks body `<code><TextureType>` and head `<code>HE<HeadType>`, falling back to `<code>`,
`<code>00` / `<code>HE00`. Then it applies the TextureType's material layers:

- `"%02d"` of TextureType (0–30) selects the variant.
- Material i of the file (in file order) takes the layer `C_<code>_S<vv>_M<i + 1>` when one is loaded, and is left
  alone otherwise.
- A model with no layer for the variant replaces `_sNN` in texture names with TextureType where that file exists (a
  fallback rule).

#### EQGL layers

Layers are the `EQGL` (`.lay`) members of the EQG archives (the client loads every member) and the `EQGL` lines of
`OnDemandResources.txt` (`gbn.eqg^GBN.LAY^C_GBN_S01_M01^EQGL`). The first loaded name wins.

Layout: `magic, version, string bytes, entry count`, strings, then entries of 10 / 16 / 7 / 8 u32 for versions 1–4:

- the name;
- five texture names (−1 none), whose role is the character five from the end (`C` diffuse, `N` normal, `E`
  environment);
- scalar fields (v3: shininess; v4: shininess and environment strength, used by armor as described above).

Example, GUS (race 549, the small goo): a flat translucent blob with a skeleton and debris inside, `GUS_m00` body
`Alpha_MaxCBS1` and `GUS_m01` debris.

- Texture 1 is `C_GUS_BD_S01_C.DDS` (green) on the body, with the debris unchanged.
- Texture 4 is `C_GUS_BK_S04_C.DDS` on body and debris (not `c_gus_bd_s04_c.dds`, which no layer uses).
- Texture 5 is the mercury `C_GUS_EV_S05_C.DDS`.

Bind-pose bounds: RTN z −0.01 … 1.49, HNM 0 … 6.0. The origin is at the feet.

## Appearance

#### The armour record

Armour records keep all five words of RoF2's ArmorProperties:

| RoF2 | EQEmu Texture_Struct | Hailstone |
|---|---|---|
| type | Material | `Appearance::material` |
| variation | Unknown1 | `material_variation` |
| material | EliteMaterial | `elite_material` |
| newArmorID | HeroForgeModel | `hero_forge` |
| newArmorType | Material2 | `new_armor_type` |

The wire's record is stored unchanged (ArmorProperties, +0x8c). A new-style model's texture is
`<code>_<material>_<variation>_<part>`, overlaid with `A_<code>_<material>_<variation>_S<elite material>` when the third
word is set. Spawns, wear changes, character selection, item previews, appearance cache keys and saves carry the
complete record. Checked: RefreshEquipSlot @00594e50, PlayerClient_SetActorEquipmentSlot @0042e6f0,
UpdateNewArmorSlot.

#### Hero's forge filter bits

`Appearance::new_armor_bits` holds the `ActorNewArmorFilter` bits that show a spawn's hero's forge
(PlayerClient_ApplyNewArmorFilter @0x595de0):

- 0x10 for an NPC;
- for a player 8, plus 1 for the local player, 2 for the local player or a group member, 4 for a raid member;
- none for a corpse (EQEmu sends NPC 2 for every corpse; RoF2 takes any Type above 1 as one).

They are made when the spawn arrives and again when the group or raid changes; a change the active filter does not
see rebuilds nothing.

`OP_Illusion` applies decoded Drakkin heritage, tattoo and detail changes. Zero resets a feature and the omitted-value
sentinel keeps it.

#### game::Appearance

`game::Appearance` (on `game::Spawn`) holds TextureType, HeadType, face, hair/beard style and colour, eyes, Drakkin
features, show_helm, and per slot the complete five-word armor record and tint (`0xAARRGGBB`, Tint_Struct). The slots
are 0 head, 1 chest, 2 arms, 3 wrists, 4 hands, 5 legs, 6 feet, 7 primary, 8 secondary.

It comes from the spawn packet (`GameConnection::apply_spawn`), `OP_WearChange` and `OP_Illusion`. `ActorRenderer`
rebuilds a spawn's mesh when it changes, dressing `game::drawn_appearance`. A race outside the per-slot armour races
(1-12, 128, 130, 330, 522) shows no armour colours (slots 0-6), as the RoF2 client draws a player under a skeleton
illusion (docs/unknowns.md).

#### Which value drives what

- **PC-model races** (race < 13, 128, 130, 330, 522) always dress per slot: a TextureType ≥ 0 in the spawn becomes the
  material of slots 1–6 (`normalize_appearance`).
- **Other races** with TextureType ≥ 0 wear that body texture on every part and ignore armour. `OP_WearChange` to
  their slots 0–6 is ignored; slots 7/8 still change.
- **OP_WearChange** (27 bytes: `u16 spawn, 5 × u32 armour {material, ?, elite, hero's forge, ?}, u32 tint, u8 slot`):
  material −1 resets the slot.
- **The own model after a gear change** (`refresh_worn_slot`; no OP_WearChange comes for it) shows each worn item with
  its ornament (`game::worn_look`). A held item shows the model of its ornament strings (`IT<n>`); armour shows its own
  Material with the ornament's hero's forge model. A disabled ornament (the first type 20/21 socket's augment with its
  stats disabled) shows the plain item (PcClient_RefreshWornSlotVisual @0x4919b0). An ornament augment's own Material
  is not shown: EQEmu sends the item's Material to everyone else and to the player at zone-in, so the augment's would
  be a look only this player sees, and only until the next zone.
- **Heads.** The head of a PC-model race shows its armour only with spawn bit 23 (show_helm) set. Head material 7 is
  drawn as 2.
- **Robes:** chest 10–16. Arms and wrists then show the chest's material. On a Luclin model they also take the chest's
  tint and hero's forge flag, and a Froglok's legs do the same (`PlayerClient::HandleMaterialEx` @0x594e50;
  `game::luclin_armor_slot`).

#### Classic (WLD) dressing

Classic (WLD) dressing (`dress_wld`) takes the skeleton's meshes, with two swaps when the replacement exists:

- `<code>HE00_DMSPRITEDEF` becomes `<code>HE<nn>` for the head material (NPC body textures: HeadType);
- `<code>_DMSPRITEDEF` becomes `<code>01_DMSPRITEDEF` under a robe.

Texture names are `<code(3)><part(2)><variant(2)><piece(2)>` (parts HE CH UA FA HN LG FT).

- They get the slot's material as variant (robes 10–16 → 04–10), when that file exists in any archive: `HUMCH0001` →
  `HUMCH0301` for plate.
- Head textures keep their variant (body textures: HeadType's, else TextureType's) and take the face as the piece's
  tens digit (`HUMHE0011` = face 1).
- Robe body textures `CLK<vv><nn>` take chest − 6.

Each material gets its slot's tint when the slot's material is not 0 and, for materials 1–99, that variant's texture
exists: as SwapBodyMaterial's ReplaceMaterial, a missing variant leaves the part as it was, untinted (SKE has chest
variants 00–04 only). Heads are tinted only when the variant is not 00, so bare skin stays untinted. Tints whose colour
is black or white count as none.

#### Held items

Held items (slots 7, 8): the material is the item's model number `IT<n>` (IDFile). Static items (0x2d meshes) are in
grip space; skeletal ones are baked in their rest pose.

- Classic and Luclin models carry them on `<code>R_POINT_TRACK` / `<code>L_POINT_TRACK`.
- A secondary goes on `SHIELD_POINT` when its model number is a shield to `CDisplay::IsShield` @0x489d70
  (`game::is_shield_model`: IT200..228, 10530..10544, 10664..10671, 11001..11020, 11496..11497 and 25 single numbers
  such as 14000). The original asks `Resources\ActorTagInfo.txt` first when it loaded, and RoF2 ships none.
- Texture names do not count.
- Matching is exact (`find_bone`, so `HAIR_POINT` is not `R_POINT`).
- EQG models use `ARMR_WEAP`, `ARML_WEAP`, `ARML_SHLD` and the bone's bind matrix.
- Items are not tinted (`itmt.ini[Primary/SecondaryTintIndex]` is not read).

#### Extra shield models

Hailstone adds, as a deliberate difference (`display.extra_shields`, on by default), 336 later shield models that RoF2
does not list and so holds in the hand like a weapon: `src/game/shield_models.inc`.

- `tools/gen_shield_models.py` made most of them from an EQEmu items table and the game folders: models mostly used
  by shield items (type 8), and shield ornaments' models whose EQG mesh is flat and covers most of its box seen from
  the broad side (as shields do and weapon ornaments do not), plus shields the measure misses (`SEEN_SHIELDS`).
- The rest were added by hand from an items-table query, less the weapon models some shields share (IT0, IT7, IT63,
  IT10201, IT10650). Running the generator again would drop them.

So the Haunting Nightmares ornament (IT101064, IT101068) is worn on the forearm like other shield ornaments, where RoF2
holds it in the hand.

#### Tints

RGB 0 becomes white, so black means untinted. Armour colours are `RGB | 0xff000000`; hero's forge pieces are
untinted. Classic models draw `lit × Tint.rgb`.

#### Drakkin and other new models of classic races

New models of the classic races (Drakkin, `DKM`/`DKF` from `dkm.eqg`): the facial features go through
`Resources/PlayerCustomization.txt` (`{race, heritage, gender}`: counts, `COLOR_LIST`, `BASE_COLOR`, values clamped to
the counts):

- face texture `C_<code>_HEAD_S%02d_M01`;
- hair `<code>_HAIR_%02d` (dag 8) and beard `<code>_FACIALHAIR_%02d` (dag 9), tinted `COLOR_LIST[colour]`;
- tattoo `<code>_TATTOO_00` (dag 0x19, texture `A_<code>_TATTOO_S%02d_M01`) and facial attachment
  `<code>_FACIALATT_%02d` (dag 0x18), both tinted with the heritage's `BASE_COLOR`.

Then per slot: `<code>_<a>_<b>_<bone>`, tinted with the slot tint, with robe overlays `A_<code>_<a>_<b>_S<c>`. `a` is
the normalised slot texture id, `b` the variation, and the bone the part's slot:

| Slot | Bone |
|---|---|
| head | `CHEST_CHEST02` |
| chest | `ROOT_BODY` |
| arms | `CHEST_CHEST01` |
| wrists | `ARMR/L_CLAV` |
| hands | `ARMR/L_BCEP` |
| legs | `PELV` |
| feet | `LEGL/R_THGH` |

The parts are `<name>.mod` files whose bones follow the body's by name. The head material is matched by texture
family (`c_dkm_head_s`), as the palette order is not the file's.

#### Hero's forge

Hero's forge (`newArmorID`): the tag for (id, code) is looked up unless `ActorNewArmorFilter` hides it (eqclient.ini,
default 24: players 8 and NPCs 0x10; 1 self, 2 group, 4 raid; `Appearance::new_armor_bits` above).

- `NewArmorTagData.txt`'s `*` / `L` / `N` tags expand to the race table's codes with flags 0x20 / 0x20 without 8 /
  0x28 (M / F one gender; a later line replaces a record).
- The tag's `IT<number>` (and `IT<pair>` on the second part) replaces the slot's part, with `NewArmorAdjData`
  (`RotationYawPitchRoll(rx°, ry°, rz°)`, rows × s, + t) unless the tag ignores adjustment.
- Hide Mesh hides the body slot's meshes; Show Hair keeps the hair under a helm.
- Pieces are EQG `.mod` entries of `OnDemandResources.txt` (`genericplate.eqg`); bone-less ones are static.

#### NPC tints

For new-style races (flag 0x08): `npct.ini` Value<index> (entry index − 1) on the whole actor (not its held items).
Index 0 or past the table does nothing. The index comes from the spawn packet's NpcTintIndex and
`OP_SpawnAppearance` 39; 43 sets show_helm.

`npct.ini` and `itmt.ini` hold `[Data] Number` entries, `Value1..ValueN` (default 0xffffff), alpha forced to 0xff
(ColorTable_LoadFromIni @0x574b60). Deliberate difference: the client reads at most 65536 entries (the original any
positive Number), so a forged file in a server's asset folder cannot make it fill billions.

#### Building appearances off the main thread

Appearances are cached per model and appearance key in the library (geometry) and per mesh on the GPU. A new one is
built on the renderer's worker thread (`core::LatestJobs`: the dress, vertices, textures decoded, Luclin layers
composited, mips), and applied on the main thread in `prepare_spawns` within a frame budget (`set_build_budget_ms`:
4 ms in the open, 100 ms behind the loading screen; the first of a frame is always applied).

- Meanwhile the spawn is drawn in its previous mesh, or the model's default one; a look applied before for the same
  model is taken at once.
- Only a spawn's latest request counts; a spawn that leaves (or goes past the actor clip) drops its request.
- The library's caches, the shared cache and the EQG and model-archive stores are guarded by one mutex per
  `CharacterArchiveCache`; texture decoding runs outside it.
- A frame's new meshes and textures go to the GPU in one submission (`stats().upload_ms`; the perf window's "actors new
  appearances" and "actors upload").

## Race → model

`race_models.inc` is generated (`tools/gen_race_models.py`) from the model table: 859 rows, 731 race ids, gender 0
male, 1 female, 2 neutral.

- The pair is looked up exactly, falling back to HUM/HUF.
- `find_race_model` instead tries the exact gender, then 2, 0 and 1, because EQEmu sends single-model NPC races with
  any gender.
- Codes without a WLD actor try the EQG store; races with neither fall back to the box.
- `chrdump --sweep` lists which race ids have a classic model.

#### Elemental swap

Before the lookup, CreatePlayerActor swaps elementals (`luclin_elemental_swap` in `game/world_state.h`). eqclient.ini `[Defaults] UseLuclinElementals` (CDisplay +0x129, default TRUE, read as
GetEQClientIniBool reads: T, t or 1) decides the direction.

- **On.** Race 75 (`ELE`, one body with four skins: 0 earth, 1 fire, 2 water, 3 air) becomes the typed Luclin
  elemental from `global5_chr`: texture 0 → 209 `EEL`, 1 → 212 `FEL`, 2 → 211 `WEL`, any other → 210 `AEL`, with
  texture and HeadType 0 (the typed models have one skin). When the typed model is not loaded the spawn stays race 75
  with its texture.
- **Off.** Races 209–212 become race 75 with texture 0 / 1 / 2 / 3.

The spawn's race really changes (SetRace), as with the HUM fallback. This is why EQEmu's magician pets (race 75 with
the element in the texture) are drawn with the Luclin models by default.

## Size and floor placement

For a classic model the drawing scale is Height × 0.2 (Height ≤ 0 → 6, so a size-6 human is drawn at 1.2). The avatar
height is moddat.ini `ROffset` (default 3.125) × scale = 3.75 for a size-6 human.

That matches the height a live server puts a standing human above the floor, so the renderer draws the model origin
at the spawn's z, scaled by size × 0.2, with no per-model offset. The rest-pose soles of the classic player models are
2.5–3.2 units below the origin (dwarves may float slightly).

- New-style races (flag 0x08) scale by Height / 6 × 1.3, and classic ones by Height × 0.2 (both / 1.3 until the first
  OP_NewZone).
- EQG models have their origin at the feet, and the animation loader lowers their ROOT_BONE by moddat.ini's ROffset.
  They are drawn ROffset × scale below the spawn's z.

The rules are in `game/spawn_heights.h`.

## Rendering

#### Held-item effects

WLD held-item meshes keep their skeleton particle nodes (even with no triangles), following the holding bone. EQG
held items keep normal and reflection maps and their `.pts`/`.prt` data. The same route serves world, character
selection and item previews (docs/formats/particles.md).

#### EQG armour on WLD bodies

An EQG armour piece on a WLD body keeps its bind-space vertices and up to four weights. Each influence gets a palette
entry of the matched host bone (else the nearest named ancestor) with its adjustment/inverse-bind correction, used for
drawing, shadows and picking. Boneless pieces are bound rigidly.

#### Skinning and upload

`ActorRenderer` uploads a mesh's vertices (position, normal, uv, four bone indices and weights) and indices the first
time a spawn uses it (a model's default mesh or an appearance).

Each frame it samples every instance's clip on the CPU. It folds the instance transform (and for EQG models the
inverse bind matrices) into the bone matrices and writes them to a per-frame storage buffer (64k matrices). A push
constant gives the instance's first bone, the texture slot and the part's tint, which multiplies the texture.

#### Texture slots

Textures share one bindless array per renderer; each frame's descriptors update after its own fence.

Idle models' texture slots are freed only after the GPU is done with them. A texture rejected because the array was
full is retried once space is reclaimed (one worn model per one-second sweep). Missing images stay cached as missing.

#### Passes and lighting

Materials map to passes like zone materials (`render/material_state.h`):

| Material | Pass |
|---|---|
| masked | alpha-test at 0.5 |
| transparent | blend |
| additive | add |
| EQG `Alpha_*` | blend with a 16/255 test |
| EQG `Chroma_*` | test at 192/255 |
| EQG `AddAlpha` | adds unfogged |

- Back faces are culled (WLD fronts clockwise, EQG counter-clockwise in this frame; two-sided materials not culled).
- Cross-fades last 100 ms, 175 ms into P01/O01.
- Fog matches `zone.frag`. A soft key light (0.8 + 0.25·N·L) keeps bodies readable while the zone is full-bright.

#### Animation choice

Animation choice per spawn (`draw_spawns`): the clip, its direction and rate come from `game/spawn_animation.h`. It
chooses as the original's two animation objects do (`game/mechanics/anim_select.h`):

- Classic models from the movement speed and stand state. Sitting down plays P02 once, then holds P07 with O03 as the
  fidget; standing up plays P02 reversed.
- New-style (EQG) models through PlayerAnimation_SelectMovementAnim.

Clip changes cross-fade as above. Corpses play D05 once and hold it.

### Memory diagnostics

Log lines for tracking memory (`logs/hailstone.log`; MiB):

| Line | Level | What |
|---|---|---|
| `memory archive:` / `memory archives:` | debug / info | per archive and total: mapped file length (not resident), owned heap, parsed WLD estimate |
| `memory checkpoint:` | debug; info after appearances settle | process resident memory at start-up stages and zone loads; on Windows also `private commit` (`PrivateUsage`) |
| `memory GPU:` | | VMA allocation bytes per heap (not driver budgets; do not add to RSS) |
| `memory WLD materialized:` | debug | each deferred WLD decode |
| `memory character WLDs:` | after zone settling | materialized/total WLDs, index and payload estimate, retained set against its budget |
| `memory character WLD activity:` | | cumulative decodes and releases |
| `memory character caches:` | | unique model and appearance payloads (shared objects once; item cache copies each) |

None of these is a full heap census: they exclude allocator overhead, capacities, cache maps and renderer copies, and
asynchronous loading overlaps checkpoints. Compare a fresh start through a settled zone-in with a second zone load.

#### Model breakdown

`memory model breakdown:` splits the model subtotal into three categories:

- animation/skeleton: clip tracks and keys, clip/held aliases, bone names, parent/order and inverse-bind arrays;
- geometry/materials: vertices, indices, parts and owned material payloads;
- other: the model object, identifying strings, points and emitters.

They sum to the model payload estimate (geometry excludes image pixels and GPU allocations). A gap against private
commit does not by itself prove a leak.
