# EQG zones

Implementation: `src/assets/eqg.{h,cpp}` (parsers), `src/assets/eqg_zone.cpp` (zone assembly, `load_eqg_zone`, `load_zone`), `src/assets/regions.cpp` (`ZoneRegions::from_boxes`). Tool: `eqgdump`. Tests: `tests/eqg_test.cpp`.

The layouts follow OpenEQ (`zone.rs`, `terrain.rs`, `loader.rs` and its EQG format docs), checked against a RoF2 install. quail (xackery, MIT) has readers for the same formats.

## Which zones are EQG

The client tries `<zone>.EQG` first and falls back to `<zone>.s3d` only if that load fails. `assets::load_zone` does the same: EQG when `<name>.eqg` exists, else classic. Only `tutoriala` and `tutorialb` ship both; they load as EQG.

Survey of an install (1,532 `.eqg` archives): 250 archives hold a terrain (`.ter`) or a zone declaration (`.zon`); the rest are character, item and object archives. 249 of them load as zones:

| Kind | Zones |
|---|---:|
| binary EQGZ v1 | 33 |
| binary EQGZ v2 | 164 |
| heightmap EQTZP ("v4") | 52 |

The 250th, `pokport_gukta.eqg`, is a portal object with a `.ter` and no declaration anywhere; it is not a zone. `poknowledge` is classic S3D (plus `poknowledge_obj3.eqg`, loaded only as extra objects for zone 202). The Bazaar is S3D. Crescent Reach (`crescent`), Anguish, the Dranik zones and all of the newer zones are EQG.

`<zone>_assets.txt` lists extra `.eqg` object archives (tradeskill objects, doors, portals) used by server-spawned objects, not zone geometry. `<zone>_EnvironmentEmitters.txt` and `<zone>.emt` are particle and sound emitters.

### Finding the declaration

In order (OpenEQ `loader.rs` `read_eqg_declaration`):
1. `<zone>.zon` inside the archive;
2. a loose `<zone>.zon` in the install root. **Most binary zones use this**: 166 loose EQGZ files sit next to the archives (e.g. `crescent.zon`; `Foundation.zon`, `guildhallLrg.zon` need the case-insensitive lookup);
3. the archive's only usable declaration: an EQGZ naming a `.ter` the archive holds, or an EQTZP naming a `.dat` it holds. `feerrott2.eqg` holds `feerrott.zon`, the Dranik archives `catacomba.zon` etc., the chambers `chambersa.zon`. Two different usable ones are an error.

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

Corpus (zone archives): MOD v1 592, v2 2,829, v3 29,123; TER v1 1, v2 32, v3 171. 15 `.mod` members are not EQGM (6 tiny files in `oldfieldofbone*`, `pellucid`, an LOD text in `feerrott2`); they are skipped.

- **Material ordinal.** A triangle's material is an index into the material records; the reader then uses the **first record with the same name** (as OpenEQ documents). The authored id is not used. Ordinal `0xffffffff` (-1) means no material: invisible but solid.
- **Flags.** Bit 0 = passable (no collision); EQEmu's map generator and OpenEQ agree. Other bits (0x10000, 0x20000, ...) are kept but unused. Passable triangles become `MeshPart::solid = false`; water (`Opaque_MaxWater.fx`) is never solid.
- **Textures.** `e_TextureDiffuse0` is the drawn texture (`e_TextureDetail1` for the Terrain class; Water falls back to its normal map). The effect classes read their own properties into `assets::EffectParams` (section "Effects"); the opaque, Alpha and Chroma classes' `e_TextureNormal0`, `e_fShininess0`, `e_fGloss0` and `e_fEnvMapStrength0` are kept for the modern lighting (`EffectParams::normal_map`, `shininess`, `gloss`, `env_strength`; docs/graphics.md); the other maps (`e_TextureCoverage0`, `e_TextureSecond0`, ...) are parsed and not used yet. An empty or `None` diffuse makes the material invisible.
- **Second UV set.** v3 vertices carry it; TER v2 files may end with a tag word 1 or 2 followed by one float pair per vertex (as OpenEQ documents). It becomes `Vertex::uv2`.
- **Packed surface maps.** `CBGG1` and `CBSG1` suffixes select the same native material type 0x0d; static effect 0x2f selects `SPL/SModelCBSG1`. Its high-quality path reads Normal0 RG as normal direction, B as glow, and A as gloss. `EffectParams::packed_normal` and `glow` retain this distinction for the zone renderer; ordinary classes retain their current map interpretation. No separate glow texture is needed for this family.
- **UVs** use the D3D top-left origin, like WLD, and DDS rows are stored top-down, so they are used unchanged. TER triangles whose material shader is exactly `Opaque_MaxCB1.fx` get the SHORT2 conversion (u × 256 truncated, low 16 bits, / 256; NaN and overflow become 0). Other non-finite UVs become 0 (13 zones have some).
- **Vertex colours** (v3) are BGRA like WLD and are converted to RGBA.
- **Non-finite positions**: triangles using them are dropped (windsong 536, chapterhouse 2, breedinggrounds 1).

### Shader names → blend mode

79 shader names occur. The family is the name's prefix:

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

`k = fmod(a_fTime, 100)` with `a_fTime = (T mod 100000) x 0.001` seconds, so a slide is in texture units (1/256 of the raw UV) per second. `Alpha_*` blends with an alpha test at 16/255 and no z-write, `Chroma_*` tests at 192/255, `*AddAlpha*` is ONE/ONE unfogged (`render/material_state.h`). The CB*/CBS* classes carry multi-pass lighting triples (`Material::multipass_lighting`).

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

- **Terrain.** `.ter` models are drawn with their file coordinates. Their ZON placement (e.g. crescent `TER_crescent` at (-193.8, -1579.3, 384.8) yaw -π/2) is **not** applied: TER vertices are used as stored and the v2 object loop skips object 0. Confirmed by the server map comparison below.
- **Placements.** `rotation` is radians about Z, Y and X in that order; the transform is `T(position) · Rz(rot[0]) · Ry(rot[1]) · Rx(rot[2]) · S(scale)` (`eqg::placement_transform`). Almost every placement has yaw ±π/2 (the 3ds Max export orientation).
- **Lighting words** (magic "EQGP", u32 count, words) are per-vertex BGRA colours. Ordinary objects use inline v2 ZON words or `<placement name>.lit`, becoming `Placement::vertex_colors` when the count matches. Terrain uses `<TER file basename>.lit`, replacing its mesh's vertex colours when the count matches, even without a terrain placement record. Its placement transform and inline ZON colours are skipped. A missing or invalid sidecar preserves the TER's own colours. Repeated TER references still draw one mesh. Like WLD object colours, RGB supplies baked light and alpha weighs the scene's sky light (`shaders/zone.vert`, `zone.frag`; `--lighting none` draws unlit). The terrain filename rule follows the original's terrain loader (`EQGraphicsDX9.dll` 0x10063f10 / 0x10065c00).
- **Lights.** Stored positions are **(y, -x, z)** relative to the geometry, so the loader uses (stored.y, -stored.x, stored.z), as OpenEQ `zone.rs` does. Check: of Anguish's 452 lights, 380 lie within 20 units of its geometry this way and 7 as stored; Dranik 280 of 419 vs 12.
- **Regions** are oriented boxes (AFG boxes take the larger horizontal extent on both axes). Angles are used as 512-unit turns after truncation toward zero (so the stored -1.5708 is -1 unit, -0.7°, not -90°), about Z, -Y and X; the box is `T(centre) · Rz · Ry(-a1) · Rx · S(half-extents)` and a point is inside when its box-local coordinates are within [-1, 1]. Lookups return the first containing box in file order, skipping `APV*` boxes. Names give the type with the same rules as WLD region tags: `AWT` water, `ALV` lava, `AVW` freezing water, `APK` PvP, `ATP<n>` zone line to server zone point n, `ASL` slippery. 122 zones have 2,631 boxes.

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

The parser must consume the file exactly (a check that catches layout errors). Tile origin = (lng, lat) × tile size; longitude is world X. Object Z is relative to the terrain under the object (sampled with the quad diagonals); group Z is absolute plus scale.z × z adjust. Rotations are degrees, applied Z · Y · X. `.tog` files are text (`*BEGIN_OBJECT`, `*NAME`, `*POSITION`, `*ROTATION`, `*SCALE`, `*END_OBJECT`) placed under their group's transform. Light colours come from `<definition>.def` (`*COLOR` ARGB, `*INTENSITY`). `water.dat` lists finite water sheets (`*WATERSHEET` ... `*MINX/MAXX/MINY/MAXY/ZHEIGHT/UVSCALE` ... `*END_SHEET`), drawn as 50% quads with `Resources/WaterSwap/water_c.bmp`.

**Terrain texture** (`assets/eqg_zone.cpp`). Each tile ecosystem (DAT layer) gets two generated S x S textures (S = 64):
- **Coverage** (`eqg::eco_layer_weight` / `eco_accumulate`): per texel, with h the bilinear height and s the slope in whole degrees, each `.eco` texture layer from the last declared back to the second gets a weight from its height band (`MINHEIGHT/MAXHEIGHT/HEIGHTTOL`, linear ramps), slope band (`MINSLOPE/MAXSLOPE/SLOPETOL`), `LAYERINGMAP` byte (sampled at `((lng mod LAYERINGAREA) S + x, (lat mod area) S + y)`) and, between 0.02 and 0.98, the `BLENDMAP` rule `q = (B - trunc(0.01 t^2 - 255)) x trunc(t (100 - BLENDSOFTNESS) 0.001 + 1)`; then `mask = trunc(w (255 - acc))`, `acc += mask`. The first layer takes `255 - acc`. `default.bmp` means no map.
- **ColorMap**: the base layer's `COVERMAP` x its mask, then each later layer's added, in byte arithmetic (mask < 3 nothing, >= 0xfd the cover byte). Its alpha is 255 for the first ecosystem and the DAT layer mask for the others (drawn over the first).
- **DetailMaskMap**: layers 0/1/2's masks in R/G/B; a border texel is averaged with the neighbouring tile's opposite edge.
- **Vertices** : z truncated to 1/8 unit, UV0 = `trunc(((1 - 2h) col / (N - 1) + h) 256) / 256` with h = 0.5 / S (texel centres on the tile edges), UV1 = `trunc(col / (N - 1) 256) / 256`. v1 is taken from the DAT's first colour array (0x808080 in Moors) and the lighting colour C from its second (0xff000000).
- **Effect** by detail layer count n: NoDetail (`ColorMap x v0`), 1Detail `2 v0 v1 ColorMap D0`, 2Detail `... (D0 (1 - m.g) + D1 m.g)`, 3Detail `2 v0 v1 ColorMap (D0 m.r + D1 m.g + D2 m.b)`, `a = ColorMap.a`; detail UVs are UV1 x DETAILREPEAT. The Bump variants add only a PointLight0 term, zero in the single-pass path.

**Regions** (`eqg::dat_region_box`). A DAT area record is name, u32 type, string, i32 lng, i32 lat, f32 x, y, zOffset, rot[3], scale[3], extents[3]. Its box is centred at the tile origin + (x, y) with **z = zOffset** (no terrain height is added), half extents = extents / 2 (**scale is ignored**), angles (rot[2], rot[1], rot[0]) x 1.4222222 in 512ths, so R = Rz(rot2) Ry(-rot1) Rx(rot0). The ATP areas of every tile register first, then the other areas (the region query takes the first containing box). The `.tog` group areas are never positioned and are not placed here. Moors keeps 12 water boxes and 4 zone lines; its ponds' boxes now sit at their stored zOffset (AWT_pond3: z -11.6..8.4).

**Water.** `water.dat` (`eqg::parse_water_file`): `*WATERSHEET` blocks (`*MINX/MAXX/MINY/MAXY/ZHEIGHT` and inline material keys) and `*WATERSHEETDATA` ... `*ENDWATERSHEETDATA` blocks (`*INDEX` decimal or 0x hex, default a running counter; `*NORMALMAP`, `*ENVIRONMENTMAP`, `*REFLECTIONCOLOR`, `*UVSCALE`, `*REFLECTIONAMOUNT`, `*FRESNELPOWER`, `*FRESNELBIAS`, `*WATERCOLOR1/2`). DAT v21 tiles carry `waterDataIndex`, `hasSheet` and the sheet's local bounds: the tile's water is the rectangle `(x0 + minX .. x0 + maxX, y0 + minY .. y0 + maxY)` at the water elevation, with the first data block of that INDEX (none: a default). Both kinds are drawn as RegionWater with UVs world / 256.

**Radial flora** (`assets/flora.h`). An ecosystem's `*FLORAPART` layers (the same layer class as `*OBJECTPART`: `*FLORA` / `*OBJECT`, `*TEXTURELAYER`, a height band, a slope band, `*DENSITY` (float), `*ITERATIONS`, `*MINSCALE` / `*MAXSCALE`, `*MINALPHA`) name a definition of `RadialFloraDefs.rfd` (texture, width and height ranges, sway, `*CROSSFLORA`, `*QUADTEXTURE` with `*TEXTUREINDEX` 0..3, `*INHERITVERTEXCOLOR`). The placement, per tile and layer: the texture layer's cover of the tile (mask >= MINALPHA) times DENSITY plants, the fraction by a rand() % 10000 draw (below one, one or none by it); up to ITERATIONS tries, each a random spot (rand / 32767 each way), a random width and height of the definition, kept when the mask there reaches MINALPHA and the height and slope lie in the bands widened by their tolerances, then a random scale (MINSCALE..MAXSCALE) and turn (rand % 360). Moors: 262,194 plants of 14 kinds on 1,794 tiles. Hailstone bounds what a damaged file can ask (its own limits, not the original's): `*ITERATIONS` is clamped to 0..10,000 and `*DENSITY` to 0..100,000 (the game's files reach 500 and 2,800), integer fields past int32's range saturate, and one tile of one ecosystem gets at most 65,536 plants. Each tile's sequence is seeded with lng × 73856093 ^ lat × 19349663 ^ ecosystem × 83492791, wrapped to 32 bits. Not read: `floraexclusion.dat` (the exclusion areas) and the other checks of a spot.

A tile with no DAT layers (in the live files only scorchedwoods' tile (-7, -5), under its canyon's rocks) is not drawn by the original; the client gives it the base ecosystem of the nearest painted tile (`eqg::nearest_painted_tile`).

**Animated textures.** A texture `<name>.dds` with a `<name>.txt` beside it (bloodfalls' blood01_c.txt: `10`, `200`, then blood01_c.dds .. blood10_c.dds) animates: frame count, ms a frame, the frames, the first being the texture itself (`eqg::parse_texture_animation`; 75 archives carry such files).

Not used yet: the LOD and `.lod`/`.prj` files. The parser accepts DAT versions 20 to 22.

## Coordinates

EQG files use the same world frame as WLD zones, and the server swaps x and y the same way (`game/coords.h`). Checked against a server's collision maps (`maps/base/<zone>.map`, EQEmu map v2, built by azone from the same files): of 3,000 sampled map vertices each in crescent and anguish, and in dranik, **all** match a vertex of our terrain or placed objects within 0.25 units after the classic swap, and none without it. This covers the terrain (untransformed), the placement rotation order and signs, and the scale. For heightmap zones the server's maps are sparse and wrong (azone's v4 support), but their extents (moors, feerrott2) show the same axis swap.

## Survey (`eqgdump --zone all`)

249 of 250 candidate zones load (the failure is `pokport_gukta`, not a zone). Load time: median 39 ms; heightmap zones up to about 1.9 s, mostly the tile bake. Largest: feerrott2 with 10,901 placements (OpenEQ's count) and 3.0M placed triangles; oldcommons 2.2M placed triangles plus 795k terrain triangles. Missing: `.tog` groups named by 10 heightmap zones (as OpenEQ reports for Dead Hills), 2 models of `dranikcatacombsa`, the editor texture `grid_standard.dds`.
