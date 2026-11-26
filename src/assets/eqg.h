#pragma once

// EQG zone files: the binary formats of the newer (EQG) zones and the text
// formats of the heightmap ("v4") zones. Layouts and sources: docs/formats/eqg.md.
// In short:
//   .ter / .mod  "EQGT" / "EQGM" meshes: a string table, materials with named
//                properties (shader name, e_TextureDiffuse0, ...), vertices,
//                triangles {a, b, c, material ordinal, flags}
//   .zon         "EQGZ" v1/v2: model names, placements, region boxes, lights;
//                or "EQTZP" text naming a heightmap .dat (see Heightmap)
//   .dat .eco .tog  heightmap tiles, ecosystem texture layers, object groups
// The parsers follow OpenEQ (crates/openeq-assets/src/zone.rs and terrain.rs,
// MIT OR Apache-2.0) and its EQG_* / HEIGHTMAP_* notes on EQGraphicsDX9.dll.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "assets/model.h"
#include "assets/regions.h"

namespace hailstone::assets::eqg {

constexpr std::uint32_t kZonMagic = 0x5a475145; // "EQGZ"
constexpr std::uint32_t kTerMagic = 0x54475145; // "EQGT"
constexpr std::uint32_t kModMagic = 0x4d475145; // "EQGM"

// A material property: type 0 float, 1 integer bits, 2 string, 3 colour/uint.
struct Property {
    std::string name;
    std::uint32_t type = 0;
    float f = 0.0f;
    std::uint32_t u = 0;
    std::string text;
};

struct MaterialDef {
    std::uint32_t id = 0; // authored id; triangles do not use it
    std::string name;
    std::string shader; // e.g. "Opaque_MaxCB1.fx"
    std::vector<Property> properties;

    const Property* find(std::string_view property) const; // case-insensitive
    // A texture property's file name, or empty when absent or "none".
    std::string texture(std::string_view property) const;
};

struct Triangle {
    std::uint32_t v[3]{};
    std::uint32_t material = 0; // ordinal into ModelFile::materials; 0xffffffff = none
    std::uint32_t flags = 0;    // bit 0: passable (no collision)
};

inline constexpr std::uint32_t kNoMaterial = 0xffffffffu;
inline constexpr std::uint32_t kTrianglePassable = 0x1u;

// A parsed .ter or .mod.
struct ModelFile {
    bool terrain = false;
    std::uint32_t version = 0;
    std::vector<MaterialDef> materials;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<glm::vec2> uvs2;       // second UV set: v3 vertices, or a TER v2 tail (tag 1/2); may be empty
    std::vector<std::uint32_t> colors; // v3 only: BGRA as stored
    std::vector<Triangle> triangles;
    std::uint32_t bone_count = 0; // .mod: skeleton present (not used for zone objects)

    // The material a triangle's ordinal selects. The native reader takes that
    // record, then the first record with the same name (OpenEQ
    // docs/EQG_MATERIAL_IDENTITY.md). nullptr for no or a bad ordinal.
    const MaterialDef* material_for(std::uint32_t ordinal) const;
    // Each ordinal's first same-named record (parse_model fills it), so material_for is not a scan per triangle.
    // Left empty (or out of step with materials), material_for scans.
    std::vector<std::uint32_t> first_material;
};

// Throws std::runtime_error on bad magic or a truncated file.
ModelFile parse_model(std::span<const std::byte> data, bool terrain);

struct ZonPlacement {
    std::int32_t model = -1; // index into ZonFile::models
    std::string name;
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // as stored: radians about Z, Y, X
    float scale = 1.0f;
    std::vector<std::uint32_t> lighting; // v2: per-vertex lighting words of the model
};

// An oriented box: centre, angles as stored (native 512-unit turns about Z,
// -Y, X, truncated), signed half-extents (OpenEQ docs/EQGZ_NATIVE_REGIONS.md).
struct ZonRegion {
    std::string name;
    glm::vec3 center{0.0f};
    glm::vec3 angles{0.0f};
    glm::vec3 extents{0.0f};
};

struct ZonLight {
    std::string name;
    glm::vec3 position{0.0f}; // as stored
    glm::vec3 color{1.0f};
    float radius = 0.0f;
};

struct ZonFile {
    std::uint32_t version = 0;
    std::vector<std::string> models; // file names (.ter / .mod), as stored
    std::vector<ZonPlacement> placements;
    std::vector<ZonRegion> regions;
    std::vector<ZonLight> lights;
};

ZonFile parse_zon(std::span<const std::byte> data);

// True when the data is a text heightmap declaration ("EQTZP").
bool is_heightmap_zon(std::span<const std::byte> data);

// ---- Heightmap zones ("EQTZP", OpenEQ docs/HEIGHTMAP_FORMAT.md) ----

struct HeightmapOptions {
    std::string name; // the .dat
    std::int32_t min_lng = 0, max_lng = 0, min_lat = 0, max_lat = 0;
    float units_per_vertex = 0.0f;
    std::uint32_t quads_per_tile = 0;
    float tile_size() const { return units_per_vertex * static_cast<float>(quads_per_tile); }
};

HeightmapOptions parse_heightmap_zon(std::span<const std::byte> data);

struct HeightmapLayer {
    std::string ecosystem;
    std::uint32_t mask_size = 0; // 0 for the first (base) layer
    std::vector<std::uint8_t> mask;
};

struct HeightmapTile {
    std::int32_t lng = 0, lat = 0; // grid coordinates; world origin = (lng, lat) * tile_size
    std::vector<float> heights;    // (q+1)^2, row-major, row = +Y
    std::vector<std::uint32_t> colors;  // colour0 (+0x08 of the vertex), BGRA as stored
    std::vector<std::uint32_t> colors2; // colour1 (+0x0c), BGRA as stored
    std::vector<std::uint8_t> quad_flags; // q^2: bit 0 hides the quad, bit 7 picks the other diagonal
    float water_level = 0.0f;           // waterElevation (+0x28)
    // Indexed water (version > 20; CTerrainTile::ReadDatRecord @0x101004a0):
    // waterDataIndex (+0x2c), hasSheet (+0x30) and the sheet's tile-local
    // bounds (+0x34..+0x40).
    std::int32_t water_index = 0;
    bool has_water_sheet = false;
    glm::vec2 water_min{0.0f}, water_max{0.0f};
    std::vector<HeightmapLayer> layers;

    // Height at tile-local (x, y), following the quad diagonals.
    float height_at(const HeightmapOptions& o, float x, float y) const;
};

struct HeightmapObject {
    std::string model; // lower case, no extension
    glm::mat4 transform{1.0f};
};

// A DAT area record (@0x10100b40): name, u32 type, string, i32 lng, i32 lat,
// f32 x, y, zOffset, rot[3] (degrees), scale[3], extents[3] (full size).
struct HeightmapRegion {
    std::string name;
    std::uint32_t type = 0;
    std::string alt_name;
    std::size_t tile = 0;     // index into Heightmap::tiles
    std::size_t index = 0;    // within its tile
    glm::vec3 position{0.0f}; // tile-local x, y; z = zOffset (absolute: no terrain height is added)
    glm::vec3 rotation{0.0f}, scale{1.0f}, size{0.0f}; // degrees; scale (not applied by the DLL); full size
};

// The region box a DAT area becomes (CEQTerrainSystem::vf3 @0x100a5d40 ->
// CAreaContainer::InitBox): centre = tile origin + (x, y) and z = zOffset,
// half extents = extents / 2 (scale ignored), angles (rot[2], rot[1], rot[0])
// x 1.4222222 in 512ths, so R = Rz(rot2) Ry(-rot1) Rx(rot0).
assets::RegionBox dat_region_box(const HeightmapRegion& region, const HeightmapTile& tile, float tile_size);

struct HeightmapLight {
    std::string name, definition;
    glm::vec3 position{0.0f};
    float radius = 0.0f;
};
// A light's colour from its <definition>.def text: "*COLOR <ARGB as a number>" scaled by "*INTENSITY <k>" (1 when
// absent), or nullopt without *COLOR. A number out of range saturates; a non-finite intensity counts as 1.
std::optional<glm::vec3> light_definition_color(std::string_view text);

struct Heightmap {
    HeightmapOptions options;
    std::uint32_t version = 0;
    std::string base_texture;
    std::vector<HeightmapTile> tiles;
    std::vector<HeightmapObject> objects; // individual objects, world transforms
    std::vector<HeightmapObject> groups;  // .tog group placements
    std::vector<HeightmapRegion> regions;
    std::vector<HeightmapLight> lights;
};

Heightmap parse_heightmap(const HeightmapOptions& options, std::span<const std::byte> dat);

// An .eco texture layer (*TEXTUREPART *LAYER), as the DLL's text reader
// (0x100f0360) stores it; file names lower case without directories,
// "default.bmp" kept as written (it means no map).
struct EcoLayer {
    std::string name;
    std::string cover_map;    // *COVERMAP / *COLORMAP (+0x20)
    std::string blend_map;    // *BLENDMAP (+0x28)
    std::string layering_map; // *LAYERINGMAP / *COVERAGEMAP (+0x50)
    std::string detail_map;   // *DETAILMAP (+0x30)
    std::string normal_map;   // *NORMALMAP (+0x3c)
    std::int32_t blend_softness = 0;  // +0x5c
    std::int32_t layering_area = 1;   // +0x58
    std::uint32_t detail_repeat = 1;  // +0x48 (DetailScale = (float)(uint))
    std::uint32_t normal_repeat = 1;  // +0x4c
    float min_height = -10000.0f, max_height = 10000.0f, height_tolerance = 0.0f; // +0x60..+0x68
    std::int32_t min_slope = 0, max_slope = 90, slope_tolerance = 0;              // +0x6c..+0x74
};

// CEcoTextureLayer_ComputeCoverage @0x100ee790, one texel of one layer
// of the EQG heightmap texturing: the weight
// from the height band, the slope band, the LAYERINGMAP byte and the
// BLENDMAP byte (negative = no map), or nullopt when the layer skips the texel.
std::optional<float> eco_layer_weight(const EcoLayer& layer, float height, std::int32_t slope,
                                      std::int32_t layering = -1, std::int32_t blend = -1);
// Steps 5 and 6: applies a parent mask p (negative = none; 0 skips), writes
// mask = trunc(w (255 - acc)) and advances the accumulator (by the mask, or
// with a parent by trunc(255 / p * mask)).
std::uint8_t eco_accumulate(float weight, std::int32_t& acc, std::int32_t parent = -1);
// The ColorMap byte step: the base layer replaces (mask < 3 -> 0), the others
// add (mask < 3 -> unchanged); mask >= 0xfd copies the cover byte, else
// round(cover * mask / 255) is added with byte wrap-around.
std::uint8_t eco_color(std::uint8_t value, std::uint8_t cover, std::uint8_t mask, bool base);
std::vector<EcoLayer> parse_eco(std::span<const std::byte> data);
// The painted tile (one with DAT layers) nearest to (lng, lat) by grid distance, searched ring by ring out to
// `reach` tiles; ties in a ring go to the lowest lat, then the lowest lng. This client's own fill for unpainted
// tiles. Each cell looked up takes one from `budget` when given; when it runs out the answer is none.
std::optional<std::size_t> nearest_painted_tile(std::int32_t lng, std::int32_t lat,
                                                const std::map<std::pair<std::int32_t, std::int32_t>, std::size_t>& painted,
                                                std::int32_t reach = 16, std::uint64_t* budget = nullptr);

// An animated EQG texture's sidecar, `<texture>.txt`: the frame count, the time
// a frame shows in ms, then the frames' file names, the first being the texture itself. bloodfalls' blood01_c.txt is
// 10, 200, blood01_c.dds .. blood10_c.dds. The count wins over a longer list (chambers' ra_blood01_n_001.txt says 9
// and lists 10); nullopt when there are fewer than two frames.
struct TextureAnimation {
    std::vector<std::string> frames; // lower case
    std::uint32_t delay_ms = 0;
};
std::optional<TextureAnimation> parse_texture_animation(std::span<const std::byte> data);

// An .eco flora layer (*FLORAPART *LAYER), as the DLL's layer reader 0x10107a60 stores it: the flora definition
// (*FLORA, +0x14), the texture layer whose mask it grows on (*TEXTURELAYER, +0x18), the height band (+0x20..+0x28),
// the slope band (+0x2c..+0x34), *DENSITY (+0x38, float), *ITERATIONS (+0x3c), *MINSCALE / *MAXSCALE (+0x40 /
// +0x44) and *MINALPHA (+0x58, a byte).
struct EcoFloraLayer {
    std::string name, flora, texture_layer;
    float min_height = -10000.0f, max_height = 10000.0f, height_tolerance = 0.0f;
    std::int32_t min_slope = 0, max_slope = 90, slope_tolerance = 0;
    float density = 0.0f;
    std::int32_t iterations = 0;
    float min_scale = 1.0f, max_scale = 1.0f;
    std::uint8_t min_alpha = 0;
};
std::vector<EcoFloraLayer> parse_eco_flora(std::span<const std::byte> data);
// Upper bounds parse_eco_flora clamps *ITERATIONS and *DENSITY to (negative ones are 0), so a damaged file cannot
// make a tile try or place billions of plants. The game's files reach 500 and 2800; how the original takes values
// past these is not traced (docs/unknowns.md).
inline constexpr std::int32_t kMaxFloraIterations = 10000;
inline constexpr float kMaxFloraDensity = 100000.0f;

// A radial flora definition (RadialFloraDefs.rfd, *BEGIN_RADIALFLORA .. *END_RADIALFLORA, *FLORADEFINITION ..
// *END_DEFINITION): its texture, its size range, its sway, whether it is two crossed quads (*CROSSFLORA), whether its
// texture holds four pictures in a 2 x 2 grid (*QUADTEXTURE, *TEXTUREINDEX 0..3) and whether it takes the terrain's
// vertex colour (*INHERITVERTEXCOLOR). Names lower case.
struct FloraDefinition {
    std::string name, texture;
    float min_width = 1.0f, max_width = 1.0f, min_height = 1.0f, max_height = 1.0f;
    bool sway = false;
    float sway_displacement = 0.0f, sway_period = 1.0f;
    bool cross = false, quad_texture = false, inherit_vertex_color = false;
    std::int32_t texture_index = 0;
};
std::vector<FloraDefinition> parse_radial_flora(std::span<const std::byte> data);

// A .tog object group: objects relative to the group's transform.
std::vector<HeightmapObject> parse_tog(std::span<const std::byte> data);

// The water material keys of water.dat (CEQWaterSheetData::vf5 @0x100a9710).
// Colours are r g b a floats; texture names as written (Resources\WaterSwap\...).
struct WaterMaterial {
    std::string normal_map, environment_map;
    glm::vec4 reflection_color{1.0f};
    glm::vec4 water_color1{1.0f}, water_color2{1.0f};
    float uv_scale = 1.0f;
    float reflection_amount = 0.0f;
    float fresnel_power = 1.0f, fresnel_bias = 0.0f;
};

// A finite sheet (*WATERSHEET ... *END_SHEET) with its inline material keys.
struct WaterSheet {
    std::string name;
    glm::vec2 min{0.0f}, max{0.0f};
    float height = 0.0f;
    float uv_scale = 1.0f;
    WaterMaterial material;
};

// A *WATERSHEETDATA ... *ENDWATERSHEETDATA block (CWaterSheetData @0x100fa4e0).
struct WaterSheetData {
    std::int32_t index = 0; // *INDEX: decimal or 0x hex; default a running counter
    WaterMaterial material;
};

// water.dat (CWaterSheetManager_LoadWaterDat @0x100fa900).
struct WaterFile {
    std::vector<WaterSheet> sheets;
    std::vector<WaterSheetData> data;
    // The first data block whose INDEX equals `index` (FindDataByIndex
    // @0x100fa4b0, exact match); nullptr when none (the DLL then makes a
    // default data).
    const WaterSheetData* find(std::int32_t index) const;
};
WaterFile parse_water_file(std::span<const std::byte> data);
std::vector<WaterSheet> parse_water_sheets(std::span<const std::byte> data);

// ---- Building zone geometry ----

// Blend mode from a shader name (Opaque_*, Alpha*/Chroma* cut-out, *AddAlpha*
// additive, Opaque_MaxWater blended water).
BlendMode shader_blend(std::string_view shader);
// The render class of a shader name (Alpha, Chroma, AddAlpha or none).
EqgShader shader_class(std::string_view shader);
// True for the zone-mesh classes with no render effect (Alpha CG1/CB1/CBS1,
// opaque VSB and CBS_2UV): the original draws nothing for them.
bool region_effect_unmapped(std::string_view shader);
// The e_* properties the EQG effects read, and the class's extra textures
// (EffectParams::textures, lower case).
EffectParams effect_params(const MaterialDef& def, EqgShader cls);

struct MeshStats {
    std::size_t bad_triangles = 0; // out-of-range indices or non-finite positions (dropped)
    std::size_t bad_uvs = 0;       // non-finite UVs (set to 0)
};

// A drawable mesh with vertices 1:1 with the file's; one part per material
// and collision flag. Triangles without a material (-1) are solid and invisible.
Mesh build_mesh(const ModelFile& model, std::string name, MeshStats& stats);

// Transform of a binary ZON placement: T(position) * Rz(rot.x) * Ry(rot.y) *
// Rx(rot.z) * S(scale); angles in radians.
glm::mat4 placement_transform(const ZonPlacement& placement);

// Object transform of the heightmap files: degrees, applied Z * Y * X.
glm::mat4 heightmap_transform(const glm::vec3& position, const glm::vec3& degrees, const glm::vec3& scale);

} // namespace hailstone::assets::eqg
