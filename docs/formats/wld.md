# WLD (classic S3D world files)

Implementation: `src/assets/wld.{h,cpp}` (parser), `src/assets/wld_zone.cpp` (meshes, skeleton rest pose, placements, `load_classic_zone`). Tool: `wlddump`. Tests: `tests/wld_test.cpp`.

Parsed fragments keep common small payloads inline and own the larger mesh and particle-cloud payloads separately.
`Fragment::get<T>()`, `holds<T>()`, and `as<T>()` expose the same decoded values; `as` throws on a mismatched type.
Copies own independent mutable payloads. The name index stores sorted fragment indices instead of another copy of
each string, preserving case-sensitive lookup and the first definition of a repeated name. It remains valid when
a WLD file is copied or moved. Storage and lookup regression tests are in `tests/wld_storage_test.cpp`.

Sources:
- **OpenEQ** (MIT OR Apache-2.0): `docs/ARCHITECTURE.md` (WLD section, coordinate system), `WLD_FRAGMENT_BOUNDARIES.md`, `WLD_STRING_OFFSETS.md`, `WLD_REFERENCE_RESOLUTION.md`, `WLD_PACKED_SCALE.md`, `WLD_PLACED_OBJECTS.md`, `WLD_OBJECT_ANIMATION.md`, and `crates/openeq-assets/src/wld.rs`.
- **libeq** (github.com/cjab/libeq, MIT): `crates/libeq_wld/src/parser/fragments/*.rs`, the most detailed field-level docs (it quotes Windcatcher's WLD spec).
- **LanternExtractor** (github.com/LanternEQ/LanternExtractor, MIT): `Wld/Fragments/*.cs`, `Wld/DataTypes/MaterialType.cs` (render-method table).
- **EQEmu zone-utilities** (github.com/EQEmu/zone-utilities, GPL-2.0) and **EQEmu Server** `zone/map.cpp`: placement rotation order and signs only. No code was copied.

The numbers below come from a RoF2 install.

## File layout

All values are little-endian.

| Offset | Field |
|---|---|
| 0 | u32 magic `0x54503d02` |
| 4 | u32 version: `0x00015500` old, `0x1000c800` new |
| 8 | u32 fragment count |
| 12 | u32 region count, u32 max object bytes (unused here) |
| 20 | u32 string-table size in bytes |
| 24 | u32 string count (unused) |
| 28 | string table, XOR-encoded with `95 3a c5 2a 95 7a 95 6a` (byte i uses key[i % 8]) |
| … | fragments: `u32 size, u32 type, size bytes of payload` |

The fragments follow the string table directly, with no alignment padding. Every payload except type 0x35 starts with an i32 name reference.

**References.** A reference is an i32. A positive n means fragment n (1-based). A negative n means the string at byte offset -n of the decoded table, read up to the NUL. Zero means none. Some fields hold a string reference that names another fragment, for example the actor name in objects.wld. `WldFile::resolve` then finds the named fragment. When a name repeats, the first definition wins: the Plane of Knowledge object WLD has 18 later duplicates, and OpenEQ picks the first as well. Offsets count source bytes. A few tables have high bytes in their trailing padding (per OpenEQ), so the table is kept as raw bytes and never re-encoded before slicing. `INT32_MIN` and out-of-range offsets resolve to nothing.

**Bounds.** Each fragment is parsed with a reader limited to its declared size (per OpenEQ). If a payload is shorter than its fields need, that fragment becomes an empty entry in `errors()`, so the fragment numbering stays intact. If a fragment runs past the end of the file, the whole file is rejected. Unknown fragment types are kept as empty entries, counted in `unknown_types()`, and logged once per file at debug level.

## Fragments decoded

Layouts below are after the name reference. `[flag]` marks a field that is present only when that flag bit is set.

| Type | Name (libeq) | Layout used |
|---|---|---|
| 0x03 | BmInfo | u32 count-1; count × {u16 len, len bytes XOR-encoded with the key restarted at 0 for each name, NUL-terminated}. Name 0 is the diffuse texture. Later names are detail layers, not animation frames (OpenEQ); see "Detail textures". |
| 0x04 | SimpleSpriteDef | u32 flags, u32 frame count, [0x20] u32 current frame, [0x08] u32 frame delay in ms, frame count × ref to 0x03. Shipped flags: 0x10 (53,980), 0x18 (2,176, of which 1,556 have more than one frame). Drawn as frame (t / delay) mod count (`render::texture_frame`). |
| 0x05 | SimpleSprite | ref to 0x04, u32 flags |
| 0x10 | HierarchicalSpriteDef | u32 flags, u32 node count, ref (collision volume), [0x1] 12 bytes, [0x2] f32 radius; per node {i32 name ref, u32 flags, ref 0x13 track, ref sprite (usually 0x2d), u32 child count, child count × i32 node index}; [0x200] u32 mesh count, mesh count × ref to 0x2d |
| 0x11 | HierarchicalSprite | ref to 0x10, u32 flags |
| 0x12 | TrackDef | u32 flags, u32 frame count. With flag 0x8 (packed), each frame is s16 w, x, y, z (/16384), s16 translation ×3 (/256), u16 scale (/256, unsigned per OpenEQ). Otherwise each frame is f32 scale, f32 translation ×3, f32 w, x, y, z. |
| 0x13 | Track | ref to 0x12, u32 flags, [0x1] u32 frame ms |
| 0x14 | ActorDef | u32 flags, string ref callback, u32 action count, u32 sprite count, ref bounds, [0x1] u32, [0x2] 28-byte location; actions are each {u32 lod count, lod count × (u32, f32)}; then sprite count × ref (0x2d mesh refs or 0x11 skeleton refs). Shipped files only use a lod count of 1, where the libeq and OpenEQ action layouts agree. |
| 0x15 | Actor | ref actor (in objects.wld a string ref to `NAME_ACTORDEF`), u32 flags, ref sphere, [0x1] u32 action, [0x2] location {f32 x, y, z, f32 rotate Z, rotate Y, rotate X, u32}, [0x4] f32 bounding radius, [0x8] f32 scale, [0x10] string ref sound, [0x100] ref to 0x33, then u32 user-data size and user data. Shipped flags: 0x2e (22,427), 0x22e (217,249), 0x32e (84,347). Sizes 56/56/60 confirm the layout. Scale and bounding radius are equal in every shipped placement. |
| 0x1b | LightDef | u32 flags, u32 frames, [0x1] u32 current frame, [0x2] u32 sleep, [0x4] frames × f32 level, [0x10] frames × f32 rgb. Shipped flags: 0x4, 0x14, 0x1e. |
| 0x1c | Light | ref to 0x1b, u32 flags |
| 0x28 | PointLight | ref to 0x1c, u32 flags, f32 x, y, z, f32 radius |
| 0x2d | DmSprite | ref to 0x36, u32 flags |
| 0x30 | MaterialDef | u32 flags (0x2: trailing pair present), u32 render method, u32 RGB pen, f32 brightness, f32 scaled ambient, ref to 0x05, [0x2] {u32, f32}. The texture reference comes before the pair: reading it after the pair gives 0 for every material (OpenEQ). |
| 0x31 | MaterialPalette | u32 flags, u32 count, count × ref to 0x30. Slots are positional. Empty slots (ref 0) stay in place. |
| 0x32 | DmRGBTrackDef | u32, u32 count, u32 ×3, count × u32 colour |
| 0x33 | DmRGBTrack | ref to 0x32, u32 flags |
| 0x36 | DmSpriteDef2 | see below |

Region fragments 0x21, 0x22 and 0x29 are described under "Regions" below.

Types seen but not decoded (none is needed for static drawing): 0x08/0x09 camera, 0x16 sphere, 0x17/0x18 polyhedron, 0x2a ambient light, 0x2f/0x37 mesh animation, 0x35 first fragment.

### 0x36 mesh

u32 flags, ref 0x31 material palette, ref animation, 2 × ref (unused), f32 centre ×3, 12 bytes unknown, f32 max distance, f32 min ×3, f32 max ×3, then u16 counts in this order: vertices, uvs, normals, colours, polygons, vertex pieces, polygon render groups, vertex material groups, mesh ops, then u16 scale shift. The arrays follow:

- **Positions:** s16 ×3. Position = centre + value / 2^shift. The centre is added for objects as well as terrain, exactly once.
- **UVs:** old format uses s16 u, v / 256 (1.0 = one texture width). New format uses f32 u, v; the shipped range is -75 to 75, which is plausible. UVs are stored with a top-left texture origin, as in D3D and Vulkan. OpenEQ uses them unchanged.
- **Normals:** s8 ×3 / 127.
- **Colours:** u32 stored as bytes B, G, R, A (LanternExtractor). These are converted to model.h's 0xAABBGGRR.
- **Polygons:** u16 flags (0x10 = passable, no collision), u16 a, b, c.
- **Vertex pieces:** {u16 vertex count, u16 skeleton node}.
- **Render groups:** {u16 polygon count, u16 material slot}. Polygons are stored in group order.

`build_mesh` resolves every palette slot first, keeping empty and unresolved slots, so that the material numbers in the render groups stay valid. It then gathers the polygons per slot and emits one `MeshPart` per used slot, in slot order. `Mesh::materials` keeps only the used slots. Zone meshes all reference the same zone-wide palette, which has thousands of slots. Polygons with a bad slot or index are dropped with a warning; none occur in the shipped data.

**Winding.** In the right-handed Z-up file frame, the stored order (a, b, c) is clockwise when seen from the front. Across all non-character archives, cross(b-a, c-a) points against the vertex normals for 22,091,292 triangles and along them for 41,812. OpenEQ emits (a, c, b) for a counter-clockwise front face. The loader keeps the file order.

**Vertex colours.** Shipped values are dark (mean red 18–68 in qeynos, qeynos2, gfaydark, freportw, poknowledge and akanon). Several zones store many all-zero colours, and akanon has alpha 0 on every vertex. They look like baked light rather than albedo, so multiplying the texture by them directly would blacken zones. The classic lighting path lights them as the RegionC1 vertex shader does: `rgb = C.rgb + C.a·Ambient + SpecialAmbient + C.a·(Bounce·max(N·Dn, 0) + Directional·max(N·−Dn, 0))`, saturated, which then lights the texture (`shaders/zone.vert`; the modern lighting is in docs/graphics.md).

## Render methods → BlendMode

The value is kept in `Material::raw_render_method`. `CMaterial::ResolveTransparency` @0x100138d0 decodes a flag word: none of the bits 0x0100ff00 set, or draw style (bits 0–1) 0, is not drawn; bit 7 is alpha-tested (masked); bit 24 is blended, additive with bit 20 set and otherwise at the opacity in bits 16–19 × 16; anything else is opaque. A standard method (bit 31 clear) is that word. With bit 31 set, the low bits index the 42-entry table `CRender::InitLegacyRenderMethodTable` @0x1009aa40 fills, and its word is decoded the same way. Counts are over all non-character archives; LanternExtractor's names (`MaterialType.cs`) are given where they help, but its guesses are not what the DLL does (it called 0x07 a diffuse).

| Raw | Count | Example | BlendMode |
|---|---:|---|---|
| 0, 0x80000000 | 358 | boundary (OpenEQ: invisible even with a texture) | Invisible |
| 0x8000001a, 0x8000001e | 1 | everfrost `M0001` (untextured) | Invisible |
| 0x80000001–04, 0c–0e, 11, 12, 14, 15, 1f–22 | 48,000+ | diffuse; 0x03 (soldungb, untextured) is drawn, not a boundary | Opaque |
| 0x80000007, 08, 13, 16, 19, 25, 26 | 2,976 | 0x07: qrg `TREE20.BMP`, akanon `curtain.BMP`; 0x13 most foliage | Masked |
| 0x80000006, 09, 24, 27 | 66 | | Transparent25 |
| 0x80000005, 0f, 18, 23 | 302 | 0x18: `POJWATER01`; 0x0f: classic sky | Transparent50 |
| 0x8000000a, 28 | 455 | | Transparent75 |
| 0x8000000b, 10, 17, 29 | 1,096 | fire, light beams, sun and moon | Additive |
| 0x8000001b–1d, 0x8000002a and up | 1 (0x31, gequip `PIPE-MOUTH`) | not in the table; what the DLL reads is not known | Opaque (0x4b and 0x53: Invisible) |
| bit 31 clear, non-zero | 1,439 | 0x4b and 0x53 are not drawn; 0x54b, 0x553 and 0x573 are opaque; 0x5d3 is masked | decoded as above |

For a masked material, palette index 0 of a BMP is the transparent colour. The texture code applies that.

## Detail textures

A BmInfo's names after the diffuse are typed as follows (`assets/wld_detail.h`):

| Names | Example | Meaning |
|---|---|---|
| 1 | `WALL.DDS` | the diffuse only |
| 2, the second with `_LAYER` | `HUMHE0001_LAYER.DDS` | a Luclin character layer (`assets/character_dress.cpp`) |
| 2, the second with `_DETAIL` | `TRANQSANDDET01.DDS_DETAIL_80.000000` | one detail texture (the name cut at `_DETAIL`) over the whole surface, its UVs × `atof` of the text after `_DETAIL_` |
| 3 to 16 | `TRANQBASE2A.DDS`, `TRANQBASE2APAL.BMP`, `1, 3, 2, TRANQSANDDET01.DDS`, … | name 1 is a palette map, names 2.. entries `<n>, <scale>, <value>, <file>`: entry i (name i + 2) has UVs × 18 × `atoi(<scale>)`; `<n>` and `<value>` are not used for drawing |
| 17 or more | | the loader sets the count to 0: the diffuse only |

The second kind becomes a material of type 1 (the detail and its scale) and the third a type 2 (a palette record: the map's name, width, height and pixels, up to 16 entries). The zone-region builder then adds detail batches (mode 2 or 3; effects `RegionOldDetailSingle` and `RegionOldDetailPalette`):

- **Single detail**: one batch of the material's triangles in the region, vertices as they are.
- **Palette**: the map's pixel under each vertex is read from the BMP's raw 8-bit indices in storage order (row 0 is the first row stored, so v = 0 is the bottom row of the picture, as WLD meshes address BMP rows; a top-down BMP, with a negative height, is read last row first so v = 0 stays the bottom row: none ships, and what the original does with one is not traced), at x = round(u·W), y = round(v·H) (rounded, kept as 32 unsigned bits, each modulo the size), as a signed char. A triangle goes into the batch of each entry one of its vertices has (below the entry count); within the pass a vertex has alpha 0xFF where its pixel is the entry and 0 where it is not (RGB unchanged). Triangles are taken in reverse order and join the newest batch of their entry that stays under 150 units across in x and y, else start one.
- **The draw**: a batch is drawn while d, the camera's distance outside the batch's x/y box along x plus along y, is below min(600, FogEnd × 1.414), with TEXTUREFACTOR alpha min(160, round((1 − d/600) × 255)). So details are at most 63 % opaque and fade out by 600 units, a box at a time.
- **The effects**: state flag +5 (SRCALPHA/INVSRCALPHA, ALPHAOP MODULATE), no z-write; the palette technique sets ALPHAOP MODULATE, ALPHAARG1 TFACTOR, ALPHAARG2 DIFFUSE, the single one ALPHAARG1 TFACTOR, so alpha = TEXTUREFACTOR × the vertex alpha and the texture's alpha is not read; colour = texture × oD0. Their vertex shader is RegionC1's without the vertex alpha's weight: `C.rgb + Ambient + SpecialAmbient + Bounce·max(N·Dn, 0) + Directional·max(N·−Dn, 0)`, oD0.a = C.a. No MPL triple (no point lights).

Palette maps are in 22 of the 24 new-format zones (all but abysmal and shadowrest; potranquility, pofire, ponightmare, podisease, povalor and postorms among them); single details also appear in older zones (poknowledge, potorment).

A region mesh without colours gets 0xFF1F1F1F on every vertex, so its RGB is dark grey and its alpha full; every new-format zone's meshes have no colours.

## Objects and skeletons

- Static actor: 0x14 → 0x2d → 0x36. An actor with several sprites is merged into one mesh.
- Skeletal actor: 0x14 → 0x11 → 0x10. It is built in its rest pose, following OpenEQ. Each node's frame 0 gives local = T · R · S, and world = parent · local. Parents come from the child lists. A malformed hierarchy (a bad index, two parents, a cycle) rejects the actor. Frame 0's quaternion keeps its stored W sign, because the rest pose uses the quaternion unchanged (as OpenEQ does). The meshes are the skeleton's mesh list when flag 0x200 is set, otherwise each node's sprite. A mesh with vertex pieces is moved per run. A mesh without them follows the node whose sprite references it, with no fallback to node 0. Particle sprites (0x34 clouds on a node) are not meshes; they become the zone's particle fixtures (docs/formats/particles.md, "WLD particle clouds"). Check: citymist `JNTREE103_ACTORDEF` gives 106 vertices, 72 triangles and 50 placements, matching OpenEQ's witness.
- Character models (skeleton, skinning, animation clips) are described in `docs/formats/characters.md`.
- Zone::objects is keyed by the upper-case actor name, including `_ACTORDEF`. The loader reads object definitions from every WLD of the object archives in this order, the first of a name winning: `obequip_lexit.s3d` in the Lost Dungeons of Norrath zones (zone expansion 6), `<zone>_obj2.s3d`, `<zone>_obj.s3d`, `<zone>_2_obj.s3d`; then from the zone WLD. Zone WLD meshes that an actor definition uses are not terrain. A WLD's bitmaps come from its own archive: when an object archive's file has the name of a different file in the zone archive (qeytoqrg's treea.bmp: an opaque DXT1 on green in `qeytoqrg.s3d`, the DXT5 cut-out of the willows in `qeytoqrg_obj.s3d`), the object's frame becomes `<archive>/<file>` (`qeytoqrg_obj/treea.bmp`) and `Zone::generated_files` holds the object archive's bytes.

## Placements, rotation and coordinates

Coordinates stay in the file's right-handed Z-up frame. Angles are in 1/512 of a full turn, stored in the order Z, Y, X.

    transform = T(position) · Rz(+z) · Ry(-y) · Rx(+x) · S(scale)

- **Order Rz·Ry·Rx, with Z as the heading:** EQEmu zone-utilities `wld_fragment.cpp` (rotate_z first) and `map.cpp` `RotateVertex` (X, then Y, then Z, standard right-handed matrices). The EQEmu server's `zone/map.cpp` applies the same transform to its collision maps, which are used for line-of-sight and pathing on live servers. OpenEQ explains why axis order matters: Greater Faydark's trees lie flat if the angles go to the wrong axes. Test: every gfaydark placement keeps object +Z pointing up.
- **Y is negated:** azone negates the Y angle for S3D placements ("y rotation seems backwards on s3ds"). OpenEQ does not negate it. Check: 23,161 placements have a Y tilt. For tilted placements that sit on a terrain triangle within 20 units, the object's up vector was compared with the terrain normal under each of the four heading/tilt sign combinations. Counting only placements where exactly one combination fits within about 2.5° and the others do not, the results were (+z, -y) 13, (-z, +y) 6, (+z, +y) 2 and (-z, -y) 2. The sample is small, but it agrees with azone. X rotation is 0 in every shipped placement, so its sign cannot be tested.
- **Scale:** the 0x8 scale field, uniform. A non-positive value means 1.
- **Parked objects:** 5,858 placements have Z = -32767.998 (-32768), for example 30 `NEKPINE*` trees at the edges of gfaydark. They are loaded as stored but excluded from `Zone::bounds_*`. It is not known whether the client should hide these objects or drop them to the floor.
- **Vertex colours:** a placement's 0x33 → 0x32 colours are used when their count equals the object's vertex count.
- **Lights:** each 0x28 gives a position and radius. The colour is the first colour of its 0x1b, or its first level as grey.

## Corpus results

- Every WLD in every `.s3d` parses: 1,696 files (24 in the new format: abysmal, dawnshroud, grimling, hollowshade, jaggedpine, letalis, maiden, mseru, natimbi, nedaria, nightmareb, podisease, pofire, ponightmare, postorms, potranquility, povalor, scarlet, shadeweaver, shadowrest, tenebrous, thegrey, twilight, umbral), with 6,181,558 fragments, 0 fragment errors and 0 unreadable archives.
- `wlddump --sweep` covers every `X.s3d` that contains `X.wld`, excluding `_chr`, `_obj`, `_lit`, `_amr` and `*equip*`: 262 zones, 262 pass. The global17–23_amr, ldonequip and obequip_lexit archives match `X.s3d` + `X.wld` but hold equipment, not zones. A zone passes when it has terrain, every index and part is valid, every placement names a loaded object, vertex-colour counts match, and the bounds are finite.
- 320,941 placements are loaded. 10 are skipped: `YKCVDRP400_ACTORDEF` in mmcb is not defined in any archive.

## Regions

Implementation: `src/assets/regions.{h,cpp}` (`ZoneRegions`, `Zone::regions`), `src/game/zone_lines.{h,cpp}` (the zone-line trigger), `src/game/zoning.{h,cpp}` (the tag's target and the teleport). Tools: `wlddump --regions ZONE`, `wlddump --zone-lines ZONE [STEP]` (where each zone line is, by grid sampling), `wlddump --region-survey`. Tests: `tests/regions_test.cpp`.

Sources: OpenEQ `bsp_regions.rs`, `zone_lines.rs`, `liquid_regions.rs` and its zone travel protocol notes for the fragment layouts and the BSP walk; LanternExtractor `BspRegionType.cs` (prefix list, field positions); EQEmu `zone/water_map_v1.cpp` (BSP walk, region types; the server swaps server x/y into WLD order before walking). No code is copied.

| Type | Layout (after the name reference) |
|---|---|
| 0x21 BSP tree | u32 node count; per node 28 bytes: f32 normal x, y, z, f32 distance, u32 region, u32 front, u32 back. Children are 1-based node numbers, 0 = none. A node with no children is a leaf; its region is a 1-based number of a 0x22 fragment in file order (0 = none). One tree per zone WLD. |
| 0x22 region | u32 flags, then visibility and mesh data that is not used here. Only the count and the order of these fragments matter. |
| 0x29 region tag | u32 flags, u32 count, count × u32 region index (**0-based**, unlike the BSP leaves), u32 tag length, tag bytes XOR-encoded with the string key. A zero length means the fragment name is the tag (e.g. `WT_ZONE`). |

**Lookup.** Start at node 1. At a split, d = dot(normal, p) + distance: d > 0 goes to the front child, d < 0 to the back child, d = 0 or a missing child means no region (as the EQEmu water map). The region at a leaf gets its flags from every tag that lists it, applied in file order. Positions are WLD coordinates (server x/y swapped, `game/coords.h`).

**Flags** (names shorter than 4 characters change nothing). The low byte is a type: `DR` clears it (dry), `WT` 5 water, `SL` 6 slime, `LA` 7 lava, `VW` 8 freezing water, `W2` 9, `W3` 10. Then 'P' at [2] sets 0x40000000 (PvP), "TP" at [3] sets 0x80000000 (teleport: zone line), and in names longer than 32 characters 'M' at [0x1f] sets 0x20000000, 'S' at [0x20] 0x10000000 (slippery: the client sets friction 0.99 and acceleration friction 0.02), 'P' at [0x21] 0x8000000, 'F' at [0x21] 0x4000000. Names starting with 'A' (EQG) instead match `AWT` 5, `ALV` 7, `AVW` 8, `APK` PvP, `ATP` teleport (the client rewrites it to `DRNTP00255%05d000000000000000` with `atoi(name + 4)`) and `ASL` slippery. Because tags apply in order, a later `DR...` tag clears an earlier water type. The client uses types 5 to 10 as "wet".

**Zone-line tags** (each field is read only when the name is long enough, with `atol`/`atof`): chars 5–9 are a zone id. Zone **255** makes it a reference: chars 10–15 are the number of a zone point in the server's `OP_SendZonepoints` table (`zone_points.number`), which supplies zone, instance, position and heading. Any other zone is an absolute target: y = chars 10–15, x = 16–21, z = 22–27, heading = 28–30, in server coordinates (y first, as in the zone-point table). Values of 999999 (heading 999) mean "keep the player's own"; a coordinate is applied only when it is below 999999 and a heading below 513. A resolved zone of 0 does nothing. Four hard-coded coordinate adjustments (zones 34→10, 10→34 (x ± 1044 relative to the player), 3→4 (z + 2) and 2→4 (y + 10)) are applied as the original applies them (`game::resolve_teleport_index`, `game/zoning.cpp`): the first three also move the player; EQEmu ignores the coordinates sent.

**Corpus** (`wlddump --region-survey`): 262 classic zones, 261 with a BSP tree (`sky` has none). Tags by prefix (up to the first `_` or digit), as zones / tags / tagged regions:

| Prefix | Zones | Tags | Regions | Meaning | Example |
|---|---:|---:|---:|---|---|
| WT | 53 | 53 | 47,239 | water | `abysmal:WT_ZONE` |
| WTN | 120 | 5,782 | 39,485 | water | `WTN__01210000000000000000000000___000000000000` |
| WTNTP | 12 | 20 | 29 | water + zone line | `qeynos:WTNTP00255000003000000000000000___000000000000` |
| LA | 5 | 5 | 2,970 | lava | `fearplane:LA_ZONE` |
| LAN | 19 | 670 | 4,634 | lava | `acrylia:LAN__00000000000000000000000000___000000000000` |
| LANTP | 2 | 6 | 10 | lava + zone line | `soldunga:LANTP00255000009000000000000000___000000000000` |
| SLN | 5 | 352 | 701 | slime | `cazicthule:SLN__01943000000000000000000000___000000000000` |
| VWN | 2 | 2 | 201 | freezing water | `acrylia:VWN__00000000000000000000000000___000000000000` |
| DRN | 25 | 1,054 | 5,674 | dry; 'S' at [0x20] = slippery | `cabeast:DRN__00129000000000000000000000_N_000000000000` |
| DRNTP | 171 | 555 | 3,579 | zone line | `qeynos:DRNTP00255000033000000000000000___000000000000` |
| DRP | 14 | 58 | 1,045 | PvP | `arena:DRP_ZONE` |

Tagged regions by type: 0 dry 10,298, 5 water 86,753, 6 slime 701, 7 lava 7,614, 8 freezing water 201 (no W2/W3). Flag bits: slippery 5,001 regions, PvP 1,049; no M, P or F bits. Zone-line tags: 551 references (zone 255) and 30 absolute (e.g. qeynos, qeynos2, qrg, erudnext, commons, lavastorm, the Karanas, beholder, highpass). Two tags are just `DRNTP_ZONE` (airplane, lavastorm), which decode to zone 0 and do nothing. Names come in two styles: 46-character `PPPPP` + 5-digit zone + 3 × 6-character coordinates + 3-character heading + `___000000000000`, and short `..._ZONE` names.

**Check against the server's `zone_points`** (a live PEQ database; `wlddump --zone-lines` gives the region boxes):

| Zone | Tag | Region (server x, y) | zone_points row |
|---|---|---|---|
| qeynos | `DRNTP00002-00155-00007999999999…` → qeynos2 at (x −7, y −155) | x −447..−435, y 472..488 | #1 source (−442, 464) → zone 2 at (−5, −151) |
| qeynos | `DRNTP00002-00030000357999999999…` → qeynos2 at (357, −30) | x −83..−71, y 596..612 | #2 source (−80, 586) → zone 2 at (356, −26) |
| qeynos | `DRNTP00255000033…` → point 33 | x −627..−619, y −152..−144 | #33 source (−596, −151) → qcat |
| qeynos | `WTNTP00255000003…` / `…004…` → points 3, 4 | (−483, 174) / (−553, 272) | #3 → qcat (source unset), #4 source (−477, 177) → qcat, #5 source (−552, 276) → qcat |
| qeynos2 | `DRNTP00001000464-00440…` → qeynos at (−440, 464) | x −12..−4, y −179..−163 | #7 source (−5, −151) → zone 1 at (−442, 464) |
| qeynos2 | `DRNTP00004-00350999999…` → qeytoqrg at (keep x, y −350) | y 1401..1997 | #9 source (73, 1272) → zone 4 at (999999, −220); qeytoqrg #1 source y −350 |

The absolute targets agree with the zone-point targets within a few units, and every region lies within about 30 units of the matching source point. Reference numbers are not always the same as the database's: in qeynos the regions tagged 3 and 4 sit at the sources of rows #4 and #5. This makes no difference with EQEmu, which picks the zone point nearest to the player for the requested zone.

**Live result** (`playprobe --zone-line Z`, test server): the client walked into the qeynos→qeynos2 line (absolute tag) and arrived at qeynos2 zone point #1's target; back through qeynos2's absolute line to qeynos; qeynos→qcat through `WTNTP00255000003` (reference; the server chose row #4, the nearest); and qcat→qeynos through `DRNTP00255000002` (reference). Walking on the floor gave no server warning; a position 20 units above the region (first try) and the bottom of the qeynos water line both gave the server's "Closest zone point … WARNING" (its water map did not put the position in a zone line), but the zoning still went through.
