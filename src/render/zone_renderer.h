#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "assets/object_animation.h"
#include "assets/zone_flora.h"
#include "render/gpu.h"
#include "render/horizon.h"
#include "render/lod_bands.h"
#include "render/scene_lighting.h"
#include "render/vk_context.h"

namespace hailstone { class GameDir; }
namespace hailstone::assets {
struct Zone;
struct Mesh;
}
namespace hailstone::game {
struct ViewDistance;
struct SceneLight;
}

namespace hailstone::render {

// How zone geometry is lit: the original's region vertex shader
// (game/scene_light.h) or full-bright (a debugging view).
enum class VertexLighting : std::uint32_t { Classic = 0, None = 1 };

// One of the land's materials (render/horizon.h): the zone's plain material it borrows, mapped flat with the zone's
// own texture scale and wound as the zone winds its own, under one colour (the level ground's median baked light).
struct HorizonLook {
    std::vector<std::string> frames;
    std::uint32_t delay = 0;
    bool two_sided = false;
    bool front_ccw = false;
    float density = 0.0f; // texture units a world unit
    bool flip = false;    // the zone's own faces up the other way round
    std::uint32_t color = 0xffffffffu;
    bool valid() const { return !frames.empty() && density > 0.0f; }
};

// What the land round a zone was built from (ZoneRenderer::horizon_build): the field (its grid and lines; how far
// each square lies past the zone's edge), its ground and rock materials.
struct HorizonBuild {
    HorizonField field;
    // Its ground near the zone's edge that grows the zone's grass (render::horizon_lawn_ground), drawn with `ground`.
    std::vector<glm::vec3> lawn;
    HorizonLook ground, rock;
};

// Whether a placed object is a plant the wind moves (the modern look's sway): assets::is_plant.
using assets::is_plant;
// How tall a mesh stands (its vertices' highest z less their lowest), 0 for none.
float mesh_height(const assets::Mesh& mesh);
// A placed plant at least this tall (its mesh's height times its placement's scale) is a tree: the land's trees are
// the zone's own (render::horizon_trees).
inline constexpr float kLandTreeMinHeight = 20.0f;

struct ZoneRenderOptions {
    // eqclient.ini [Defaults] MultiPassLighting (render::read_multipass_lighting):
    // the zone's point lights shade zone geometry whose effect has a
    // multi-pass (MPL) triple. Off draws the single-pass path, where the
    // region shaders' point lights are zero.
    bool multipass_lighting = false;
    // The current zone, whose lights the point shadows (render/point_shadows.h) are made for: each light carries its
    // shadow key (render::point_shadow_keys), so the shaders find its shadow without searching. Off, its lights take no
    // shadow.
    bool light_shadow_keys = false;

    // The classic regions' detail passes, and a texture pack's normal and roughness maps.
    bool details = true;
    bool material_maps = true;
    // Asked between meshes and materials while preparing: true stops it, and prepare() returns null. Unset: never.
    std::function<bool()> cancelled;
    // The current zone: trees on its land's woods (render::horizon_trees), the zone's own drawn far.
    bool land_trees = true;
    // The current zone: where its land's worn terrain is cached (render::horizon_terrain; cache/terrain/<server>/
    // <zone>.bin, app::terrain_cache_path); empty: worn on every load, not kept.
    std::filesystem::path terrain_cache;
};

// CRender::ReadIniRenderSettings @0x10098350 and CDisplay::InitWorld: true
// when eqclient.ini [Defaults] MultiPassLighting starts with t, T or 1. The
// original also needs a ps_1_1 card with a stencil depth format, which every
// Vulkan device here is taken to have.
bool multipass_lighting_setting(std::string_view value);
// The options from the game directory's eqclient.ini.
ZoneRenderOptions zone_render_options(const GameDir& game);

// The liquid surfaces of a classic zone's terrain, which the modern lighting draws as water or lava: per terrain mesh,
// per triangle (index / 3), 0 none, kLiquidWater or kLiquidLava. A level, passable triangle (of a plain material)
// with a liquid region just under it and not just over it; and one of the same material at the same height as such a
// surface where a dry region cuts into the liquid (a zone line's region over the sea, the strip by a zone's edge).
inline constexpr std::uint8_t kLiquidWater = 1, kLiquidLava = 2;
std::vector<std::vector<std::uint8_t>> classic_liquid_triangles(const assets::Zone& zone);

// The EQG effects' scroll time k for a scene time in ms: a_fTime =
// (T mod 100000) x 0.001 s (CRender::SetEffectSceneConstants @0x10088c70),
// k = fmod(a_fTime, 100). A slide offset is k x e_fSlide texture units.
float effect_scroll_seconds(std::uint64_t scene_ms);

// How much a placed plant of `height` units bends in the wind at its top (world units at full strength; lighting.glsl
// wind_offset) and how much its leaves flutter. A replacement with levels of detail (a generated tree,
// docs/graphics.md "Generated trees") does not flutter, and sways only with its sway heights (assets::Mesh::
// sway_heights: each branch with its needle sprays and clump moves as one piece), else stands still: its crown is many
// small crisp sprays crossing each other, and a sway by each vertex's own height (or a flutter) moved crossing cards
// apart, leaves flipping back and forth up close; the game's few big soft cards hide the same motion.
struct PlantSway {
    float bend = 0.0f, flutter = 0.0f;
};
PlantSway plant_sway(const assets::Mesh& mesh, float height);

struct ZoneRenderStats {
    std::size_t triangles = 0;
    std::size_t draws = 0;
    std::size_t textures = 0;
    std::size_t missing_textures = 0;
    std::size_t chunks = 0; // the boxes the passes cull by
    // What making the renderer from a prepared zone cost on the calling thread, in milliseconds, by step: the
    // uploads (textures staged, buffers), the descriptor sets, and the pipelines.
    float upload_ms = 0.0f, descriptors_ms = 0.0f, pipelines_ms = 0.0f;
};

// The zone texture array's size in its descriptor set layout, for a zone of `textures` on a device whose arrays hold
// `max_sampled_images` (VkCaps): one bound for every zone, kZoneTextureBound or the device's limit, so the pipeline
// layout, and with it the pipeline cache's key, is the same from one zone to the next. Sized by each zone's own count,
// every zone load compiled its pipelines afresh, ~370 ms of its upload on the frame's thread. The binding has
// a variable count: the set is allocated with the zone's own. A zone with more textures than the bound keeps its count.
inline constexpr std::uint32_t kZoneTextureBound = 0xffff; // as the actor renderer's
std::uint32_t zone_texture_layout_count(std::uint32_t textures, std::uint32_t max_sampled_images);

// What the zone's passes drew since the last ZoneRenderer::take_drawn: triangles and draw calls into the scene (draw)
// and into the shadow map's layers (draw_shadow, draw_point_shadow); the culling's measure.
struct ZoneDrawn {
    std::size_t triangles = 0;
    std::size_t calls = 0;
    std::size_t shadow_triangles = 0;
    std::size_t shadow_calls = 0;
};

// Which of the zone's draws a draw() call makes. The modern lighting splits the scene around
// VkContext::begin_translucent: Opaque before it (without the water), Translucent after (the water first, reading
// the copies, then the blended and additive draws). All draws everything in one go, the water without the copies.
enum class ZonePhase : std::uint8_t { All, Opaque, Translucent };

// Draws a zone's static geometry: terrain plus every placed object, merged into
// one vertex and index buffer and grouped into one draw per material. Each draw
// is cut into chunks with a bounding box, and every pass draws only the chunks
// its view can reach (render/culling.h): the camera's frustum, the sun cascades'
// boxes, the cover and water maps' square, the lights' reach.
class ZoneRenderer {
public:
    // WLD details blend in the opaque group, including over bases that may leave depth clear.
    // Those zones still need their sky/background before the group.
    bool opaque_blends_background() const { return opaque_blends_background_; }
    // The CPU half of the build (geometry, chunks, decoded textures with their mip chains): ZoneRenderer::prepare,
    // on any thread; the constructor taking it uploads it.
    struct Prepared;
    struct PreparedDeleter { void operator()(Prepared* p) const; };
    using PreparedPtr = std::unique_ptr<Prepared, PreparedDeleter>;
    // `max_images`: the device's sampled image limit (VkContext::caps().max_sampled_images).
    static PreparedPtr prepare(const assets::Zone& zone, const ZoneRenderOptions& options, std::uint32_t max_images);
    // The prepared data's size in bytes (vertices, indices, textures as uploaded), for the logs.
    static std::size_t prepared_bytes(const Prepared& prepared);
    // What a prepare built: the vertices and indices in the buffers, the instances (the identity not counted), the
    // triangles as drawn (an instanced model's once for each placement) and the draws' chunks.
    struct PreparedCounts {
        std::size_t vertices = 0, indices = 0, instances = 0, triangles = 0, chunks = 0;
    };
    static PreparedCounts prepared_counts(const Prepared& prepared);
    static bool prepared_blends_background(const Prepared& prepared);
    // What a prepare built of the land (render/horizon.h): the build it keeps, and the land's ground triangles.
    struct PreparedLand {
        std::shared_ptr<const HorizonBuild> build;
        std::size_t triangles = 0;
        std::size_t water_triangles = 0;  // the land's sea
        bool water_two_sided = true;      // all of it drawn from both faces
        std::size_t trees = 0;            // the land's trees (Draw::land_trees' placements)
        std::size_t tree_kinds = 0;       // of how many of the zone's trees
    };
    static PreparedLand prepared_land(const Prepared& prepared);

    ZoneRenderer(VkContext& vk, const assets::Zone& zone, const std::filesystem::path& shader_dir,
                 const ZoneRenderOptions& options = {});
    ZoneRenderer(VkContext& vk, PreparedPtr prepared, const std::filesystem::path& shader_dir);
    ~ZoneRenderer();
    ZoneRenderer(const ZoneRenderer&) = delete;
    ZoneRenderer& operator=(const ZoneRenderer&) = delete;

    // The chunks in `view_proj`'s frustum.
    void draw(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
              const game::ViewDistance& view, double time_seconds, ZonePhase phase = ZonePhase::All);
    // The opaque and alpha-tested geometry into the scene's depth buffer, before draw's opaque phase
    // (SceneOptions::depth_prepass): the colour pass then tests equal and writes no depth.
    void draw_depth(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                    const game::ViewDistance& view, double time_seconds);
    // The opaque and alpha-tested geometry into shadow cascade `cascade`, between VkContext::begin_shadow_cascade and
    // end_shadow_cascade; the cascades come from the last set_lighting. With kCoverLayer, the cover map
    // (render/cover_map.h): the opaque geometry only; with kWaterLayer, the water map: the water surfaces only. Only
    // the chunks in the layer's view, from the same set_lighting (the cascade's matrix, the maps' square).
    void draw_shadow(const FrameInfo& frame, std::uint32_t cascade);
    // The solid geometry within `radius` of `light`, found once for the light's six faces (point_reach: the chunks in
    // its sphere, in drawing order); then into face `face` of its shadow cube (render/point_shadows.h) those on the
    // face's side of it, between VkContext::begin_point_shadow and end_point_shadow; the plants at rest.
    struct PointReach {
        glm::vec3 light{0.0f};
        float radius = 0.0f;
        std::vector<std::uint32_t> chunks; // in chunks_
    };
    void point_reach(const glm::vec3& light, float radius, PointReach& out) const;
    void draw_point_shadow(const FrameInfo& frame, const PointReach& reach, std::uint32_t face);
    // Whether the zone has water surfaces (the water map is worth drawing).
    bool has_water() const;
    // The zone's animation clock (assets/object_animation.h), ms since the zone was loaded: the objects that play their
    // own animation are drawn where it has them at this time, from the next pass of the frame on.
    void set_animation_time(std::uint64_t ms) { animation_ms_ = ms; }
    // How many placed parts move with their object's animation (drawn instanced, posed each frame).
    std::size_t animated_parts() const { return animated_instances_; }
    // Whether what draw_shadow draws into the cover or water map (kCoverLayer, kWaterLayer) moves from frame to frame
    // in the map's view, from the last set_lighting: the plants' swaying parts, with wind.
    bool map_moves(std::uint32_t layer) const;
    // A number no other zone renderer made in this run has (what the cover and water maps show).
    std::uint64_t id() const { return id_; }
    // The modern lighting's backdrop where no sky is drawn, from the last set_lighting (shaders/underwater.frag): the
    // haze as it is infinitely far away, its glow towards the sun too; with the camera under water
    // (render::set_underwater), the open water, its colour and light shafts. Draw it first in the scene.
    void draw_backdrop(const FrameInfo& frame, const glm::mat4& view_proj);
    // The land round the zone (render/horizon.h), built from its terrain while preparing it: what was measured, and whether draw and draw_depth draw it (off until set; the modern lighting outdoors).
    const HorizonLand& horizon() const { return horizon_; }
    // What the land was built from (its field: how far each square lies past the zone's edge); null without land.
    const std::shared_ptr<const HorizonBuild>& horizon_build() const { return horizon_build_; }
    void set_distant(bool show) { show_distant_ = show; }

    const ZoneRenderStats& stats() const { return stats_; }
    // The counts since the last call; they start again from zero.
    ZoneDrawn take_drawn() { return std::exchange(drawn_, ZoneDrawn{}); }
    void set_vertex_lighting(VertexLighting mode) { lighting_ = mode; }
    // The frame's lighting constants (game::compute_scene_light).
    void set_scene_light(const game::SceneLight& light);
    // The modern mode's switch, daylight and dynamic lights (render/scene_lighting.h); classic: params.x 0.
    void set_lighting(const SceneLighting& lighting);
    // The smooth classic lighting (LightingMode::SmoothClassic, Hailstone's own): the classic region light per pixel
    // instead of per vertex (shaders/zone.frag; SceneLighting::ambient.w). No effect on the modern lighting.
    void set_classic_smooth(bool on) { scene_.lighting.ambient.w = on ? 1.0f : 0.0f; }
    // Turns the depth pre-pass on or off from the next frame (SceneOptions::depth_prepass: display.depth_prepass, the
    // Options window's Depth Pre-pass). On the main thread, before the frame is recorded.
    void set_depth_prepass(bool on) { depth_prepass_on_ = on; }
    // Whether a frame draws the pre-pass (draw_depth): switched on, its pipelines built, in the modern lighting.
    static bool prepass_draws(bool on, bool built, bool modern) { return on && built && modern; }
    // How far the replacement models' levels of detail reach (render/lod_bands.h), times their own distances: the
    // tree detail setting (display.tree_detail). 0.25 to 4.
    void set_lod_scale(float scale) { lod_scale_ = std::clamp(scale, 0.25f, 4.0f); }
    // Leaves the placed objects' flame meshes out of draw (the modern lighting draws its own: render/fire_renderer.h).
    void set_hide_flames(bool hide) { hide_flames_ = hide; }

private:
    enum class Pass : std::uint8_t { Opaque, Blended, Additive };
    // A surface on a water or lava region's top (classic zones, by the regions under and over each triangle), or
    // an EQG water effect: the modern lighting draws them as liquids.
    enum class Liquid : std::uint8_t { None, Water, Lava };

    struct Draw {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::vector<std::uint32_t> frame_slots; // texture slots; more than one = animated
        std::uint32_t frame_delay_ms = 0;
        std::array<std::uint32_t, 4> extra_slots{}; // the effect's other textures (shader slots 1..4)
        std::uint32_t mode = 0;                    // zone.frag's effect
        std::uint32_t maps = 0; // a texture pack's normal map slot | roughness map slot << 16 (kClassic; 0 = none)
        Pass pass = Pass::Opaque;
        float alpha_ref = 0.0f;
        float opacity = 1.0f;
        bool two_sided = false;
        bool front_ccw = false;
        bool fog = true;
        bool lit = true;
        bool multipass = false; // add the point lights (MPL path)
        Liquid liquid = Liquid::None;
        bool flame = false; // a placed object's flame mesh: set_hide_flames leaves it out
        bool distant = false; // the land round the zone: no shadows; set_distant
        // The land's trees (render::horizon_trees, render/tree_impostor.h): an instanced model drawn as the land is,
        // with no shadows and shown by set_distant, but from the shared index buffer.
        bool land_trees = false;
        // The current zone's land splatted with rock and sand by its vertices' weights (uv2; zone.frag kLandSplat):
        // extra_slots[0] the rock's texture, [1] the sand's.
        bool land_splat = false;
        glm::vec4 slide{0.0f}, extra{0.0f}, extra2{0.0f}, extra3{0.0f}, extra4{0.0f};
        float param_z = 0.0f;
        std::uint32_t surface = 0; // the modern lighting's surface maps (kNormalMap, kSpecular, kReflective)
        std::uint32_t first_chunk = 0; // its chunks in chunks_
        std::uint32_t chunk_count = 0;
        glm::vec3 box_min{0.0f}, box_max{0.0f}; // round all of them: a draw wholly out of view tests no chunk
        // A replacement model's level of detail (render/lod_bands.h): its chunks show only within its band
        // (lod_shows).
        LodBand lod;
        // An instanced replacement model's number + 1 (0: the zone's own geometry): its chunks are its placements,
        // each all of the draw's indices with an instance of its own (Chunk::instance).
        std::uint32_t instanced = 0;
    };

    // Whether every shadow pass but the water map draws `d` without its texture: solid (opaque, not alpha-tested), not
    // a flame. These go into solid_.
    // (A level of detail is drawn on its own, so that its shadow changes level with it.)
    static bool casts_untextured(const Draw& d) {
        return d.pass == Pass::Opaque && !d.flame && !d.distant && !(d.alpha_ref > 0.0f) && !d.lod.banded() && d.instanced == 0;
    }
    float lod_scale_ = 1.0f; // set_lod_scale
    // The camera's position the levels of detail are chosen by: the last begin_draw's (the shadow passes, drawn
    // before it in a frame, take the frame before's).
    glm::vec3 lod_eye_{0.0f};
    // Whether the depth pre-pass draws `d`, and the colour pass then tests it equal: the opaque pass's draws but the
    // water, which the modern lighting draws blended after VkContext::begin_translucent.
    static bool in_depth_prepass(const Draw& d) { return d.pass == Pass::Opaque && d.liquid != Liquid::Water; }
    // The modern lighting (set_lighting), not the classic.
    bool modern_lighting() const;
    static bool blends_background(const Draw& draw);

    void upload(Prepared& prepared);
    void create_descriptors();
    void write_texture_set();
    void create_pipelines(const std::filesystem::path& shader_dir);
    void create_shadow_pipeline(const std::filesystem::path& shader_dir);
    void create_shadow_pipeline(const std::filesystem::path& shader_dir, bool point);
    void create_underwater_pipeline(const std::filesystem::path& shader_dir);
    void upload_scene(std::uint32_t frame_slot);
    // What draw and draw_depth share: the geometry and the sets bound, the frame's scene uploaded. Returns the time
    // in milliseconds.
    std::uint64_t begin_draw(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                             const game::ViewDistance& view, double time_seconds);
    // Draw `d`'s own state: its culling and its push constants, with zone.frag's `flags`.
    void bind_draw(VkCommandBuffer cmd, const Draw& d, std::uint32_t flags, std::uint64_t time_ms);

    VkContext& vk_;
    std::uint64_t id_ = 0;
    std::uint64_t shared_listener_ = 0;
    ZoneRenderStats stats_;
    VertexLighting lighting_ = VertexLighting::Classic;
    bool hide_flames_ = false;
    struct SceneUniforms { // std140, zone.vert's Scene block
        glm::mat4 view_proj{1.0f};
        glm::vec4 fog_color{0.0f};
        glm::vec4 fog{0.0f}; // start, end, k (seconds of a_fTime), density
        glm::vec4 camera_pos{0.0f};
        SceneLighting lighting{}; // render/scene_lighting.h; the classic fields from set_scene_light
    };
    SceneUniforms scene_{};
    // The wind's clock a frame ago (lighting.time.y, zone.vert's motion under the temporal upscaler): `wind_time_` is
    // this frame's, for the frame `wind_frame_` (VkContext::frames_begun).
    float wind_time_ = 0.0f, wind_time_before_ = 0.0f;
    std::uint64_t wind_frame_ = 0;
    std::array<Buffer, VkContext::kFramesInFlight> scene_buffers_{};
    std::array<void*, VkContext::kFramesInFlight> scene_mapped_{};
    VkDescriptorSetLayout scene_layout_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, VkContext::kFramesInFlight> scene_sets_{};
    std::vector<Draw> draws_;
    bool opaque_blends_background_ = false; // opaque_blends_background, worked out once draws_ is set
    // Each draw's triangles in pieces of the zone with their bounding box (build), in draw order and, within a draw,
    // in index order: the culling's unit. The opaque draws' are by 128-unit cell; the blended and additive draws keep
    // their triangles' order, cut into runs that stay within a cell's size. The boxes take in how far the plants
    // sway.
    struct Chunk {
        std::uint32_t draw = 0;
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        glm::vec3 box_min{0.0f}, box_max{0.0f};
        bool sways = false; // some of its vertices sway in the wind
        std::uint32_t instance = 0; // in the instance buffer (0: the identity, the zone's own geometry)
    };
    // Whether chunk `c` continues the run drawn from `first` (`count` indices) as instances `instance` on (`instances`
    // of them): the zone's own geometry end to end in the index buffer, or an instanced model's next placement. Grows
    // the run.
    static bool extends_run(const Chunk& c, std::uint32_t first, std::uint32_t& count, std::uint32_t instance,
                            std::uint32_t& instances) {
        if (c.instance == 0 && instance == 0 && c.first_index == first + count) {
            count += c.index_count;
            return true;
        }
        if (c.instance != 0 && c.instance == instance + instances && c.first_index == first && c.index_count == count) {
            ++instances;
            return true;
        }
        return false;
    }
    // Whether chunk `c` of `d` shows by `pick` at its distance from lod_eye_. A banded draw is an instanced model's,
    // its chunks its placements: the distance to where the placement stands, the one zone.vert fades it by
    // (v_lod_distance), so every level of a tree changes together.
    bool lod_shows(const Draw& d, const Chunk& c, LodPick pick = {}) const;
    // Where each instance stands (GpuInstance's translation), for lod_shows.
    std::vector<glm::vec3> instance_origins_;
    // Binds the vertex, instance and index buffers.
    void bind_geometry(VkCommandBuffer cmd) const;
    // Writes the animated objects' motions at animation_ms_, and as they were a frame ago, into frame slot
    // `frame_slot`'s motion buffer; once per frame (VkContext::frames_begun).
    void animate_instances(std::uint32_t frame_slot);
    std::vector<Chunk> chunks_;
    // The casts_untextured draws merged for the shadow passes, one chunk a cell (render/shadow_chunks.h): its chunks
    // follow the draws' in chunks_, its indices the draws' in the index buffer. Only its chunk fields are set.
    Draw solid_;
    // Records draw `d`'s chunks that `keep` (a predicate on a Chunk) passes, a run of neighbouring chunks in one
    // vkCmdDrawIndexed; `first` runs once before the first, so a draw with nothing to show sets up nothing.
    // `counted`: where its triangles and calls go in drawn_. The depth pre-pass counts none: the colour pass draws
    // the same triangles.
    enum class Counted : std::uint8_t { Scene, Shadow, None };
    // `lod`: which level of a banded draw (lod_shows).
    template <class Keep, class First>
    void draw_chunks(VkCommandBuffer cmd, const Draw& d, Counted counted, Keep&& keep, First&& first, LodPick lod = {});
    ZoneDrawn drawn_;
    // The zone's lights for the modern lighting's per-pixel pass (render/light_grid.h): the lights with the grid's
    // header, the cells, the cells' light numbers (zone.frag's set 1, bindings 1 to 3).
    std::array<Buffer, 3> light_grid_buffers_{};
    Buffer vertex_buffer_;
    Buffer instance_buffer_; // GpuInstance: the identity, then the instanced models' placements
    // The animated objects' moving nodes (assets/object_animation.h). Motion k (>= 1) is node motions_[k - 1].node of
    // animations_[motions_[k - 1].animation]; an instance's normal_row[1].w names the motion its object space goes
    // through before its placement (0: none). Each frame in flight has its motions' rows, now and a frame ago, in a
    // storage buffer (zone.vert's and the shadow shaders' set 1 binding 4), written by animate_instances.
    struct Motion {
        std::uint32_t animation = 0;
        std::uint16_t node = 0;
    };
    std::vector<Motion> motions_;
    std::vector<assets::ObjectAnimation> animations_;
    std::vector<std::vector<glm::mat4>> animation_poses_; // per animation, the last animate_instances' motion
    std::vector<std::array<glm::vec4, 3>> motion_rows_;   // per motion, the rows last written (the next frame's past)
    std::array<Buffer, VkContext::kFramesInFlight> motion_slots_{};
    std::array<void*, VkContext::kFramesInFlight> motion_mapped_{};
    std::size_t animated_instances_ = 0;
    std::uint64_t animation_ms_ = 0;
    std::uint64_t animated_frame_ = 0; // VkContext::frames_begun of the last animate_instances (0: none yet)
    Buffer index_buffer_;
    std::vector<Texture> textures_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[3]{};
    // SceneOptions::depth_prepass: the depth-only pipeline, and the opaque colour pipeline that tests equal and writes
    // no depth (the pre-pass wrote it). Built with the zone whether the pre-pass is on or not, so
    // set_depth_prepass switches it from one frame to the next.
    bool depth_prepass_built_ = false;
    bool depth_prepass_on_ = false; // set_depth_prepass
    bool depth_drawn_ = false; // draw_depth wrote this frame's depth: the next opaque draw may test equal
    VkPipeline depth_pipeline_ = VK_NULL_HANDLE;
    VkPipeline opaque_equal_pipeline_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_ = VK_NULL_HANDLE;
    VkPipeline point_shadow_pipeline_ = VK_NULL_HANDLE;
    VkPipeline underwater_pipeline_ = VK_NULL_HANDLE;
    HorizonLand horizon_;
    std::shared_ptr<const HorizonBuild> horizon_build_;
    // The land round the zone's chunks are cells this size.
    static constexpr float kDistantChunk = 256.0f;
    bool show_distant_ = false;
};

} // namespace hailstone::render
