# EQG zones

Implementation: `src/assets/eqg.{h,cpp}` (parsers), `src/assets/eqg_zone.cpp` (zone assembly, `load_eqg_zone`, `load_zone`), `src/assets/regions.cpp` (`ZoneRegions::from_boxes`). Tool: `eqgdump`. Tests: `tests/eqg_test.cpp`.

The layouts follow OpenEQ (`zone.rs`, `terrain.rs`, `loader.rs` and its EQG format docs), checked against a RoF2 install. quail (xackery, MIT) has readers for the same formats.

## Which zones are EQG

`assets::load_zone` loads `<zone>.EQG` when it exists and falls back to `<zone>.s3d` if that load fails. Only
`tutoriala` and `tutorialb` ship both; they load as EQG.

An install has about 250 EQG zones: about 33 binary EQGZ v1, 164 EQGZ v2 and 52 heightmap EQTZP ("v4").

- `pokport_gukta.eqg` holds a `.ter` but no declaration: a portal object, not a zone.
- `poknowledge` is classic S3D (plus `poknowledge_obj3.eqg`, loaded only as extra objects for zone 202). The Bazaar is
  S3D.
- Crescent Reach (`crescent`), Anguish, the Dranik zones and all of the newer zones are EQG.

`<zone>_assets.txt` lists extra `.eqg` object archives (tradeskill objects, doors, portals) used by server-spawned objects, not zone geometry. `<zone>_EnvironmentEmitters.txt` and `<zone>.emt` are particle and sound emitters.

### Finding the declaration

In order (OpenEQ `loader.rs` `read_eqg_declaration`):
1. `<zone>.zon` inside the archive;
2. a loose `<zone>.zon` in the install root. **Most binary zones use this** (e.g. `crescent.zon`; `Foundation.zon`,
   `guildhallLrg.zon` need the case-insensitive lookup);
3. the archive's only usable declaration: an EQGZ naming a `.ter` the archive holds, or an EQTZP naming a `.dat` it
   holds. `feerrott2.eqg` holds `feerrott.zon`, the Dranik archives `catacomba.zon` etc., the chambers
   `chambersa.zon`. Two different usable ones are an error.

A heightmap declaration counts only when its `.dat` exists: `oldcommons.zon` (archived and loose) names `commonlands`, whose `commonlands.dat` and `commonlands.zon` are in `oldcommons.eqg`.

Model files named in a ZON are read from the archive, then from the install root.

## TER / MOD (binary meshes)

```
u32 magic "EQGT" (.ter) / "EQGM" (.mod), u32 version (1..3)
u32 string table size, u32 material count, u32 vertex count, u32 triangle count
.mod only: u32 bone count
string table (NUL-terminated strings, referenced by byte offset)
materials × {u32 id, u32 name, u32 shader, u32 property count,
             properties × {u32 name, u32 type, value}}   type 0 f32, 1 u32 bits, 2 string offset, 3 u32 (ARGB colour)
vertices: v1/v2 {vec3 position, vec3 normal, vec2 uv}                         32 bytes
          v3    {vec3 position, vec3 normal, u32 colour (BGRA), vec2 uv, vec2 uv2}  44 bytes
triangles × {u32 a, b, c, u32 material ordinal, u32 flags}                   20 bytes
.mod: bones and weights follow; TER v2 may append a tagged second UV stream (not read)
```

All versions occur (most MOD files are v3). A few `.mod` members are not EQGM (tiny files in `oldfieldofbone*`,
`pellucid`, an LOD text in `feerrott2`); they are skipped.

**Material ordinal.** A triangle's material is an index into the material records. The reader then uses the **first
record with the same name** (as OpenEQ documents); the authored id is not used. Ordinal `0xffffffff` (-1) means no
material: invisible but solid.

**Flags.** Bit 0 = passable (no collision), as in EQEmu's map generator and OpenEQ. Other bits (0x10000, 0x20000, ...)
are unused. Passable triangles become `MeshPart::solid = false`; water (`Opaque_MaxWater.fx`) is never solid.

**Textures.** `e_TextureDiffuse0` is the drawn texture (`e_TextureDetail1` for the Terrain class; Water falls back to
its normal map). An empty or `None` diffuse makes the material invisible.

- The effect classes read their own properties into `assets::EffectParams` (section "Effects").
- The opaque, Alpha and Chroma classes' `e_TextureNormal0`, `e_fShininess0`, `e_fGloss0` and `e_fEnvMapStrength0` are
  kept for the modern lighting (`EffectParams::normal_map`, `shininess`, `gloss`, `env_strength`; docs/graphics.md).
- A held item's MPL `e_TextureCoverage0` / `e_fCoverageScale0` and a Lava material's second layer are used for actors
  (docs/graphics.md).
- The other maps are parsed and not used yet.

**Second UV set** (`Vertex::uv2`): in v3 vertices, or at the end of a TER v2 file as a tag word 1 or 2 followed by one
float pair per vertex (as OpenEQ documents).

**Packed surface maps.** `CBGG1` and `CBSG1` select the same native material class (class 0xc, render class 0xd);
`CBSGE1` adds an environment reflection, and `CBGGE1`, `CBE1` and `CBSE1` share its render class.

- Packed maps hold RG tangent normals (Z fixed at 0.9 before normalization), B added to ambient illumination and A
  replacing `e_fGloss0` (`EffectParams::packed_normal`, `glow`). Ordinary `CB1`/`CBS1` maps are full RGB normals.
- The client matches the class suffix after removing `.fx` and `Max`; native matches the text anywhere.
- Held WaterFall/Lava materials keep their authored scrolling coordinates and auxiliary textures (docs/graphics.md,
  EQG surface maps).

Checked: `CMaterial::InitFromShaderName` (`EQGraphicsDX9.dll` 0x10014660), the compiled `SkinMeshCBSG1`/`SkinMeshCBSGE1` shaders.

**UVs** use the D3D top-left origin, like WLD, and DDS rows are stored top-down, so they are used unchanged.

- TER triangles whose material shader is exactly `Opaque_MaxCB1.fx` get the SHORT2 conversion (u × 256 truncated, low
  16 bits, / 256; NaN and overflow become 0).
- Other non-finite UVs become 0.

**Vertex colours** (v3) are BGRA like WLD and are converted to RGBA.

**Non-finite positions:** triangles using them are dropped (windsong, chapterhouse, breedinggrounds have some).

### Shader names → blend mode

The family is the name's prefix:

| Shader | Count (materials) | Blend |
|---|---:|---|
| `Opaque_*` (MPLBump, MPLBasic, MaxCB1, MaxCB1_2UV, MPLBump2UV, MPLBlend, MaxLava, MaxWaterFall, ...) | ~460,000 | Opaque |
| `Opaque_MaxWater.fx` | 6,356 | Transparent50, not solid |
| `Alpha*` (Alpha_MPLBasicA/AT, Alpha_MPLBumpA, AlphaSModel*, ...) | ~21,000 | Masked (alpha test) |
| `Chroma*` (Chroma_MPLBasicAT, Chroma_MPLBumpAT, ...) | ~21,700 | Masked |
| names containing `AddAlpha` (AddAlpha_MPLBasicA, AddAlpha_MaxCB1, Opaque_AddAlphaC1Max, ...) | ~2,100 | Additive |

### Effects

The shader class is chosen by case-sensitive substrings in the order WaterFall, Water, Terrain, Lava2, Lava, AddAlpha, Alpha, Chroma, then the opaque suffixes (`eqg::shader_class`). The effects (`shaders/zone.frag`):

| Class | Effect | State | Pixel |
|---|---|---|---|
| `WaterFall` | RegionWaterFall | blend, alpha test, no z-write | `Diffuse0(uv + Slide1 k).rgb x v0`, alpha `Diffuse0(uv + Slide2 k).a` |
| `Water` | RegionWater (harbingers: RegionLavaH) | blend, alpha test, no z-write | two scrolling `Normal0` samples, Fresnel `(1 - bias)(1 - n.v)^power + bias`, colour `lit x lerp(WaterColor1, WaterColor2, 1 - n.v)`, `a = F x 2.9`; the sky-cube reflection is left out |
| `Terrain` | RegionTerrain | opaque | `(Detail1 w.x + Detail2 w.y) x 2 Coverage(uv x CoverageScale) v1 v0`, w = UV set 2 |
| `Lava2`, `Lava` | RegionLava2 (not settled: drawn as Lava), RegionLava | opaque | `lerp(2 Diffuse1(uv + Slide2 k), Diffuse0(uv + Slide1 k) v0, Diffuse0.a)` |
| Alpha CG1/CB1/CBS1, opaque VSB/CBS_2UV on zone meshes | none ("Invalid render material type") | not drawn | |

- `k = fmod(a_fTime, 100)` with `a_fTime = (T mod 100000) x 0.001` seconds, so a slide is in texture units (1/256 of
  the raw UV) per second.
- `Alpha_*` blends with an alpha test at 16/255 and no z-write, `Chroma_*` tests at 192/255, `*AddAlpha*` is ONE/ONE
  unfogged (`render/material_state.h`).
- The CB*/CBS* classes carry multi-pass lighting triples (`Material::multipass_lighting`).

## ZON, binary (EQGZ v1/v2)

```
u32 magic "EQGZ", u32 version (1, 2)
u32 string table size, u32 model count, u32 placement count, u32 region count, u32 light count
string table
models × u32 name offset                          (file names: TER_*.TER, OBJ_*.MOD)
placements × {i32 model index, u32 name offset, vec3 position, vec3 rotation, f32 scale
              v2: u32 count, count × u32 lighting word}
regions × {u32 name offset, vec3 centre, vec3 angles, vec3 signed half-extents}   40 bytes
lights × {u32 name offset, vec3 position, vec3 colour, f32 radius}               32 bytes
```

**Terrain.** `.ter` models are drawn with their file coordinates: their ZON placement (e.g. crescent `TER_crescent` at
(-193.8, -1579.3, 384.8) yaw -π/2) is **not** applied, and the v2 object loop skips object 0. Confirmed by the server
map comparison below.

**Placements.** `rotation` is radians about Z, Y and X in that order. The transform is
`T(position) · Rz(rot[0]) · Ry(rot[1]) · Rx(rot[2]) · S(scale)` (`eqg::placement_transform`). Almost every placement
has yaw ±π/2 (the 3ds Max export orientation).

#### Lighting words

Lighting words (magic "EQGP", u32 count, words) are per-vertex BGRA colours.

- Ordinary objects use inline v2 ZON words or `<placement name>.lit` (`Placement::vertex_colors`) when the count
  matches.
- Terrain uses `<TER file basename>.lit` when the count matches its vertex count, even without a placement record;
  inline ZON colours are skipped. A missing or mismatched sidecar keeps the TER's own colours.
- Repeated TER references still draw one mesh.

As with WLD object colours, RGB is baked light and alpha weighs the scene's sky light (`--lighting none` draws unlit;
docs/graphics.md). Checked: `EQG_LoadFileByMagic` (`EQGraphicsDX9.dll` 0x10065c00), EQGT loader 0x10063f10.

#### Lights and regions

**Lights.** Stored positions are **(y, -x, z)** relative to the geometry, so the loader uses (stored.y, -stored.x,
stored.z), as OpenEQ `zone.rs` does (most of Anguish's and Dranik's lights then sit on geometry; few do as stored).

**Regions** are oriented boxes (AFG boxes take the larger horizontal extent on both axes).

- Angles are used as 512-unit turns after truncation toward zero, so the stored -1.5708 is -1 unit, -0.7°, not -90°.
  They turn about Z, -Y and X.
- The box is `T(centre) · Rz · Ry(-a1) · Rx · S(half-extents)`. A point is inside when its box-local coordinates are
  within [-1, 1].
- Lookups return the first containing box in file order, skipping `APV*` boxes.
- Names give the type with the same rules as WLD region tags: `AWT` water, `ALV` lava, `AVW` freezing water, `APK`
  PvP, `ATP<n>` zone line to server zone point n, `ASL` slippery.

## Heightmap zones (EQTZP)

The `.zon` is text: `EQTZP`, `*NAME` (the `.dat`), `*MINLNG/*MAXLNG`, `*MINLAT/*MAXLAT` (tile range), `*UNITSPERVERT`, `*QUADSPERTILE`, and extents/version fields not needed. A tile is `quads × units` wide (feerrott: 16 × 16 = 256 units).

The `.dat`:

```
u32 version (20, 21, 22), u32 ×2 unknown, cstring base texture, u32 tile count
tile × {
  i32 lng + 100000, i32 lat + 100000, u32 editor id
  (q+1)² f32 heights, row-major (row = +Y, column = +X)
  (q+1)² u32 colours, (q+1)² u32 second colours
  q² u8 quad flags: 0x01 hides the quad, 0x80 splits it 10–01 instead of 00–11
  f32 water elevation; version ≥ 21: i32 water data index, u8 hasSheet, hasSheet ≠ 0: f32 minX, maxX, minY, maxY; f32
  u32 layer count, layers × {cstring ecosystem; layers after the first: u32 n, n² mask bytes}
  u32 count, objects × {cstring model, cstring ecosystem, 8 bytes, vec3 pos, vec3 rot°, vec3 scale, u8, version & 2: u32}
  u32 count, regions × {cstring name, u32 type, cstring name2, i32 lng, i32 lat, f32 x, y, zOffset, vec3 rot°, vec3 scale, vec3 size}
  u32 count, lights × {cstring name, cstring definition, u8, 8 bytes, vec3 pos, 24 bytes, f32 radius}
  u32 count, groups × {cstring .tog name, 8 bytes, vec3 pos, vec3 rot°, vec3 scale, f32 z adjust}
}
```

The parser must consume the file exactly.

- Tile origin = (lng, lat) × tile size; longitude is world X.
- Object Z is relative to the terrain under the object (sampled with the quad diagonals). Group Z is absolute plus
  scale.z × z adjust.
- Rotations are degrees, applied Z · Y · X.
- `.tog` files are text (`*BEGIN_OBJECT`, `*NAME`, `*POSITION`, `*ROTATION`, `*SCALE`, `*END_OBJECT`) placed under
  their group's transform.
- Light colours come from `<definition>.def` (`*COLOR` ARGB, `*INTENSITY`).
- `water.dat` lists finite water sheets (`*WATERSHEET` ... `*MINX/MAXX/MINY/MAXY/ZHEIGHT/UVSCALE` ... `*END_SHEET`),
  drawn as 50% quads with `Resources/WaterSwap/water_c.bmp`.

#### Terrain texture

`assets/eqg_zone.cpp`. Each tile ecosystem (DAT layer) gets two generated S x S textures (S = 64).

**Coverage** (`eqg::eco_layer_weight` / `eco_accumulate`). Per texel, h is the bilinear height and s the slope in whole
degrees. Each `.eco` texture layer, from the last declared back to the second, gets a weight from:

- its height band (`MINHEIGHT/MAXHEIGHT/HEIGHTTOL`, linear ramps);
- its slope band (`MINSLOPE/MAXSLOPE/SLOPETOL`);
- its `LAYERINGMAP` byte (sampled at `((lng mod LAYERINGAREA) S + x, (lat mod area) S + y)`);
- between 0.02 and 0.98, the `BLENDMAP` rule
  `q = (B - trunc(0.01 t^2 - 255)) x trunc(t (100 - BLENDSOFTNESS) 0.001 + 1)`.

Then `mask = trunc(w (255 - acc))`, `acc += mask`. The first layer takes `255 - acc`. `default.bmp` means no map.

**ColorMap.** The base layer's `COVERMAP` x its mask, then each later layer's added, in byte arithmetic (mask < 3
nothing, >= 0xfd the cover byte). Its alpha is 255 for the first ecosystem and the DAT layer mask for the others
(drawn over the first).

**DetailMaskMap.** Layers 0/1/2's masks in R/G/B. A border texel is averaged with the neighbouring tile's opposite
edge.

**Vertices.** z truncated to 1/8 unit.

- UV0 = `trunc(((1 - 2h) col / (N - 1) + h) 256) / 256` with h = 0.5 / S (texel centres on the tile edges).
- UV1 = `trunc(col / (N - 1) 256) / 256`.
- v1 is taken from the DAT's first colour array (0x808080 in Moors), and the lighting colour C from its second
  (0xff000000).

**Effect** by detail layer count n, with `a = ColorMap.a` and detail UVs UV1 x DETAILREPEAT:

| n | Effect |
|---|---|
| 0 | NoDetail (`ColorMap x v0`) |
| 1 | 1Detail `2 v0 v1 ColorMap D0` |
| 2 | 2Detail `... (D0 (1 - m.g) + D1 m.g)` |
| 3 | 3Detail `2 v0 v1 ColorMap (D0 m.r + D1 m.g + D2 m.b)` |

The Bump variants add only a PointLight0 term, zero in the single-pass path.

#### Heightmap regions

`eqg::dat_region_box`. A DAT area record is name, u32 type, string, i32 lng, i32 lat, f32 x, y, zOffset, rot[3],
scale[3], extents[3]. Its box:

- is centred at the tile origin + (x, y) with **z = zOffset** (no terrain height is added);
- has half extents = extents / 2 (**scale is ignored**);
- turns by angles (rot[2], rot[1], rot[0]) x 1.4222222 in 512ths, so R = Rz(rot2) Ry(-rot1) Rx(rot0).

The ATP areas of every tile register first, then the other areas (the region query takes the first containing box).
The `.tog` group areas are never positioned and are not placed.

#### Water sheets

`water.dat` (`eqg::parse_water_file`) has two kinds of block:

- `*WATERSHEET` blocks (`*MINX/MAXX/MINY/MAXY/ZHEIGHT` and inline material keys);
- `*WATERSHEETDATA` ... `*ENDWATERSHEETDATA` blocks: `*INDEX` (decimal or 0x hex, default a running counter),
  `*NORMALMAP`, `*ENVIRONMENTMAP`, `*REFLECTIONCOLOR`, `*UVSCALE`, `*REFLECTIONAMOUNT`, `*FRESNELPOWER`, `*FRESNELBIAS`,
  `*WATERCOLOR1/2`.

DAT v21 tiles carry `waterDataIndex`, `hasSheet` and the sheet's local bounds. The tile's water is the rectangle
`(x0 + minX .. x0 + maxX, y0 + minY .. y0 + maxY)` at the water elevation, with the first data block of that INDEX
(none: a default). Both kinds are drawn as RegionWater with UVs world / 256.

#### Radial flora

`assets/flora.h`. An ecosystem's `*FLORAPART` layers are the same layer class as `*OBJECTPART`: `*FLORA` / `*OBJECT`,
`*TEXTURELAYER`, a height band, a slope band, `*DENSITY` (float), `*ITERATIONS`, `*MINSCALE` / `*MAXSCALE`,
`*MINALPHA`. They name a definition of `RadialFloraDefs.rfd`: texture, width and height ranges, sway, `*CROSSFLORA`,
`*QUADTEXTURE` with `*TEXTUREINDEX` 0..3, `*INHERITVERTEXCOLOR`.

The placement, per tile and layer:

1. The plant count is the texture layer's cover of the tile (mask >= MINALPHA) times DENSITY; a rand() % 10000 draw
   decides the fraction.
2. Up to ITERATIONS tries. Each is a random spot (rand / 32767 each way) and a random width and height of the
   definition.
3. A try is kept when the mask there reaches MINALPHA and the height and slope lie in the bands widened by their
   tolerances.
4. Then a random scale (MINSCALE..MAXSCALE) and turn (rand % 360).

Hailstone bounds what a damaged file can ask (its own limits, not the original's):

- `*ITERATIONS` is clamped to 0..10,000 and `*DENSITY` to 0..100,000 (the game's files reach 500 and 2,800);
- integer fields past int32's range saturate;
- one tile of one ecosystem gets at most 65,536 plants, and a zone at most 4,194,304 (about sixteen times Moors),
  beyond which no more are placed.

Each tile's sequence is seeded with lng × 73856093 ^ lat × 19349663 ^ ecosystem × 83492791, wrapped to 32 bits.

Not read: `floraexclusion.dat` (the exclusion areas) and the other checks of a spot.

#### Unpainted tiles and animated textures

A tile with no DAT layers (in the live files only scorchedwoods' tile (-7, -5)) is not drawn by the original. The
client gives it the base ecosystem of the nearest painted tile (`eqg::nearest_painted_tile`).

**Animated textures.** A texture `<name>.dds` with a `<name>.txt` beside it animates: frame count, ms a frame, the
frames, the first being the texture itself (`eqg::parse_texture_animation`). Example:
bloodfalls' blood01_c.txt is `10`, `200`, then blood01_c.dds .. blood10_c.dds.

Not used yet: the LOD and `.lod`/`.prj` files. The parser accepts DAT versions 20 to 22.

## Coordinates

EQG files use the same world frame as WLD zones, and the server swaps x and y the same way (`game/coords.h`).

Checked against a server's collision maps (`maps/base/<zone>.map`, EQEmu map v2): in crescent, anguish and dranik all
sampled map vertices match our terrain or placed objects within 0.25 units after the classic swap, and none without
it. That confirms the untransformed terrain, the placement rotation order and signs, and the scale. Heightmap zones'
server maps are poor, but their extents show the same swap.

## Survey (`eqgdump --zone all`)

- Every zone loads. Median load time is about 40 ms; heightmap zones take up to about 2 s, mostly the tile bake.
- Largest: feerrott2 (about 11,000 placements, 3M placed triangles).
- Missing from the files: `.tog` groups named by some heightmap zones (as OpenEQ reports for Dead Hills), 2 models of
  `dranikcatacombsa`, the editor texture `grid_standard.dds`.
