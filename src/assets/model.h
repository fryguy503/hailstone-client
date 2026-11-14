#pragma once

// Renderer-independent geometry produced by the asset loaders (WLD now, EQG
// later). Coordinates are EQ world units in the files' own Z-up frame; the
// renderer converts to its own frame.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::assets {

enum class BlendMode : std::uint8_t {
    Opaque,
    Masked,        // alpha test: palette index 0 (BMP) or BC1/alpha < 0.5 is cut out
    Transparent25, // blended at 25/50/75% opacity
    Transparent50,
    Transparent75,
    Additive,
    Invisible,     // not drawn, but still solid for collision
};

// The EQG shader classes that decide render state (CMaterial::InitFromShaderName
// @0x10014660 -> CRender::SelectRenderEffectForMaterial @0x1008f0d0). EQG
// materials of these classes keep BlendMode Masked (Alpha, Chroma) or Additive
// (AddAlpha) for collision; the renderers draw them by this class.
enum class EqgShader : std::uint8_t {
    None,
    Alpha,     // Alpha_*: SRCALPHA/INVSRCALPHA, alpha test >= 16/255, no z-write
    Chroma,    // Chroma_*: alpha test >= 192/255, opaque, z-write
    AddAlpha,  // *AddAlpha*: ONE/ONE, alpha test >= 16/255, no z-write, fog off
    Water,     // SPL/RegionWater: SRCALPHA blend, alpha test, no z-write
    WaterFall, // SPL/RegionWaterFall: SRCALPHA blend, alpha test, no z-write; scrolling
    Terrain,   // SPL/RegionTerrain: opaque; two details weighted by the second UV set
    Lava,      // SPL/RegionLava: opaque; two scrolling layers
    Lava2,     // SPL/RegionLava2: opaque (formula not settled; drawn as Lava)
    LavaH,     // SPL/RegionLavaH: harbingers.eqg water (formula not settled; drawn as Water)
    // A heightmap terrain tile (SPL/Terrain_NoDetail, _1Detail .. _3Detail and
    // their Bump variants; CRender::CreateTerrainTileNode @0x1008ff80): frames
    // hold the tile's ColorMap, EffectParams::textures {DetailMaskMap,
    // DetailColorMap0..2}, EffectParams::detail_scale the DetailScales and the
    // detail layer count.
    TerrainTile,
    // A zone-mesh class SelectRenderEffectForMaterial has no case for (Alpha
    // CG1/CB1/CBS1, opaque VSB and CBS_2UV): the batch fails and nothing draws.
    Unmapped,
    // Not an EQG class: a classic region's detail pass (SPL/RegionOldDetailSingle
    // and RegionOldDetailPalette), which the zone renderer makes from
    // Material::detail_layers (assets/wld_detail.h).
    WldDetail,
};

// The e_* material properties the EQG material shaders read.
struct EffectParams {
    glm::vec4 slide{0.0f}; // e_fSlide1X, e_fSlide1Y, e_fSlide2X, e_fSlide2Y: texture units per second
    // Extra textures by class: Water/LavaH {Normal0, Environment0};
    // Lava/Lava2 {Diffuse1, Normal0}; Terrain {Detail2, Coverage} (frames
    // hold Detail1).
    std::vector<std::string> textures;
    glm::vec4 water_color1{1.0f}, water_color2{1.0f}, reflection_color{1.0f};
    float fresnel_bias = 0.0f, fresnel_power = 1.0f, reflection_amount = 0.0f;
    float uv_scale = 1.0f;       // e_fUVScale (water)
    float coverage_scale = 1.0f; // e_fCoverageScale (Terrain)
    glm::vec4 detail_scale{1.0f, 1.0f, 1.0f, 0.0f}; // TerrainTile: DetailScale0..2, detail layer count
    // TerrainTile of a tile's second or later ecosystem: drawn over the first
    // with the ColorMap alpha (the DAT layer mask) as its opacity.
    bool overlay = false;
    // The opaque, Alpha and Chroma classes' surface maps (the modern lighting draws them; the original binds a
    // Normal map only with bump mapping on, InitFromShaderName): e_TextureNormal0 (a tangent-space normal map),
    // e_fShininess0 and e_fGloss0 (the scene's 40 and 0.2 when the material sets none), and e_fEnvMapStrength0
    // (stored doubled). `specular` / `reflective`: the shader name asks for a specular term (CBS*, MPL*SB, *GB,
    // *Full) or an environment reflection (CE1, CBSGE1).
    std::string normal_map;
    float shininess = 40.0f, gloss = 0.2f, env_strength = 0.0f;
    bool specular = false, reflective = false;
};

// A classic material's detail texture (assets/wld_detail.h): the file and the
// factor its pass multiplies the material's UVs by.
struct DetailLayer {
    std::string texture;
    float uv_scale = 1.0f;
};

struct Material {
    std::string name;
    BlendMode blend = BlendMode::Opaque;
    // Texture file names inside the archives; more than one means an animated
    // texture that advances every frame_delay_ms.
    std::vector<std::string> frames;
    std::uint32_t frame_delay_ms = 0;
    // Luclin character materials (SkinMeshLuclin): the second texture, whose
    // alpha selects where its tinted colour replaces frames[0]; or empty.
    std::string tint_layer;
    std::uint32_t raw_render_method = 0; // as stored, for debugging and later shader work
    // Drawn without back-face culling: WLD 0x30 flags bit 0, which
    // CMaterial::ResolveTransparency @0x100138d0 copies to the material's
    // +0x28 byte; D3DXMesh subsets with it draw with CULL_NONE.
    bool two_sided = false;
    EqgShader eqg_shader = EqgShader::None;
    EffectParams effect;
    // EQG: the effect has a multi-pass lighting (MPL) triple (the CB* and
    // CBS* classes); the zone's point lights then shade it when
    // MultiPassLighting is on.
    bool multipass_lighting = false;
    // Classic WLD detail passes over a zone region (assets/wld_detail.h): one
    // layer over the whole surface, or with `detail_palette` (the palette
    // map's file) the entries its pixels pick per vertex.
    std::vector<DetailLayer> detail_layers;
    std::string detail_palette;
};

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec2 uv{0.0f};
    std::uint32_t color = 0xffffffff; // baked vertex colour, RGBA8 packed as 0xAABBGGRR
    glm::vec2 uv2{0.0f};              // second UV set (EQG v3 meshes, heightmap detail UVs)
    std::uint32_t color2 = 0xffffffff; // second vertex colour (heightmap colour1), RGBA8
};

// A run of indices drawn with one material.
struct MeshPart {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material = 0; // index into Mesh::materials
    bool solid = true;          // false: passable, no collision (EQG triangle flag bit 0)
    // A level of detail (a replacement model's, assets/gltf_mesh.h): drawn only from `lod_near` to `lod_far` world
    // units from the camera. lod_far 0: at every distance (the game's meshes).
    float lod_near = 0.0f, lod_far = 0.0f;
};

struct Mesh {
    std::string name;
    std::vector<Vertex> vertices; // final positions (mesh centre already added)
    std::vector<std::uint32_t> indices; // triangle list
    std::vector<MeshPart> parts;
    std::vector<Material> materials;
    // Winding of the front faces in this frame. WLD triangles are clockwise
    // from the front; EQG .ter/.mod triangles come out the other way round
    // here (found by testing; how the original handles them is not traced,
    // docs/unknowns.md). Renderers cull the back faces accordingly;
    // collision records take such triangles as (a, c, b) so that floors face
    // up by the original's normal rule (game/swept_sphere.h).
    bool front_ccw = false;
    // Heightmap terrain: drawn with heights truncated to 1/8 unit, as the
    // DLL packs them (z = trunc(height * 8) as a short, TerrainTileChunk_BuildVertices).
    bool heightmap_terrain = false;
};

// A placed static object (ActorInstance) in a zone.
struct Placement {
    std::string actor;  // key into Zone::objects
    glm::mat4 transform{1.0f}; // object space -> world space, rotation and scale included
    // Per-vertex colours from the zone's object lighting, one per vertex of the
    // object's mesh, or empty to use the mesh's own colours.
    std::vector<std::uint32_t> vertex_colors;
};

struct PointLight {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float radius = 0.0f;
};

// Whether a light read from a zone file is all finite numbers. A damaged file's NaN or infinity is left out at load,
// so the light grid never bins it (the original's files hold none).
inline bool finite_light(const PointLight& l) {
    return std::isfinite(l.position.x) && std::isfinite(l.position.y) && std::isfinite(l.position.z) &&
           std::isfinite(l.color.r) && std::isfinite(l.color.g) && std::isfinite(l.color.b) && std::isfinite(l.radius);
}

} // namespace hailstone::assets
