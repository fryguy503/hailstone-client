#pragma once

// Animated character models (classic WLD skeletal actors), skinned on the GPU:
// each vertex follows one bone, and every instance's bone matrices (with its
// position, heading and scale folded in) go to a per-frame storage buffer.
// Textures use one bindless array, as in the zone renderer.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "assets/character.h"
#include "core/latest_jobs.h"
#include "game/spawn_animation.h"
#include "game/world_state.h"
#include "render/actor_layers.h"
#include "render/actor_surface.h"
#include "render/culling.h"
#include "render/gpu.h"
#include "render/scene_lighting.h"
#include "render/texture_claims.h"
#include "render/vk_context.h"

namespace hailstone::game {
struct ViewDistance;
}

namespace hailstone::render {

// One model to draw.
struct ActorDraw {
    const assets::CharacterModel* model = nullptr;
    // Geometry to draw (an appearance from CharacterLibrary::dress), or
    // nullptr for the model's default geometry.
    const assets::CharacterMesh* mesh = nullptr;
    glm::vec3 position{0.0f}; // model origin, world coordinates
    float yaw = 0.0f;         // radians, counterclockwise from +X (the model's front)
    float scale = 1.0f;
    // Current clip and time; when blend < 1, the pose is mixed from
    // previous_clip at previous_time (weight 1 - blend).
    const assets::AnimationClip* clip = nullptr;
    float time = 0.0f;
    bool loop = true;
    bool holding = false; // game::holds_item: the clips are the model's holding variants (CharacterModel::clip)
    const assets::AnimationClip* previous_clip = nullptr;
    float previous_time = 0.0f;
    float blend = 1.0f;
    // The upper body's own animation (render/actor_layers.h: an action over the legs): when `upper_clip` is set,
    // the bones `bone_groups` puts in game::kUpperBones are posed from it (blended the same way), the others from
    // `clip`. Unset: the whole body plays `clip`.
    const assets::AnimationClip* upper_clip = nullptr;
    float upper_time = 0.0f;
    bool upper_loop = true;
    const assets::AnimationClip* upper_previous_clip = nullptr;
    float upper_previous_time = 0.0f;
    float upper_blend = 1.0f;
    const std::vector<std::uint8_t>* bone_groups = nullptr; // per bone of the model (game/anim_layers.h)
    // A rider hangs from its mount's dag 0xe (PlayerClient_AttachRiderToMount, the actor vtable +0x1b8): its
    // model's world matrix is the bone's world matrix (`parent`, the mount's scale included; ActorRenderer::seat_rider);
    // its own scale, position and yaw are unused.
    bool attached = false;
    glm::mat4 parent{1.0f};
    float alpha = 1.0f; // actor visibility supplied by its owner, independent of material opacity
};

struct ActorRenderStats {
    std::size_t instances = 0;
    std::size_t unposed = 0;  // instances in neither the view nor a shadow's reach: not posed (actor_needs_pose)
    std::size_t draws = 0;
    std::size_t models = 0;   // meshes (models and appearances) uploaded so far
    std::size_t dressed = 0;  // appearances applied this frame
    std::size_t undressed = 0; // spawns drawn this frame still waiting for theirs (being built, or past the budget)
    double dress_ms = 0.0;    // main-thread time applying them (their meshes and textures put up for the GPU), and
                              // making the default meshes of new models
    double upload_ms = 0.0;   // the frame's one upload submission of new meshes and textures (its fence wait)
    std::size_t textures = 0;
    std::size_t missing_textures = 0; // textures that failed to load (drawn untextured)
    double cpu_ms = 0.0;      // last frame's CPU time in draw()
};

// Whether a spawn whose bounds are the sphere (`center`, `radius`) is posed this frame: when it can reach `view`
// (the camera's frustum; null: always), or when `shadows` can take it, within `shadow_reach` of `shadow_from` (0 or
// less: everywhere), the reach beyond which ActorRenderer's shadow draws pass it over. A spawn neither in view nor
// casting into a shadow shows nowhere: its animation still runs (prepare_spawns), only its bones are not posed.
bool actor_needs_pose(const Frustum* view, const glm::vec3& center, float radius, bool shadows,
                      const glm::vec3& shadow_from, float shadow_reach);

class ActorRenderer {
public:
    // What a zone's renderer leaves the next zone's (release_kept): the meshes on the GPU, with their textures, that
    // came from the libraries' shared cache (assets::SharedCharacterCache) and were drawn in that zone. Frees them
    // when it is not handed on.
    struct Kept;
    // `kept`: the last zone's (release_kept). What of it the new library would not hand out the same (a mesh it does
    // not share, a texture its zone archives may change) is freed; the rest is drawn as it was.
    ActorRenderer(VkContext& vk, std::shared_ptr<const assets::CharacterLibrary> library,
                  const std::filesystem::path& shader_dir, std::unique_ptr<Kept> kept = nullptr);
    ~ActorRenderer();
    ActorRenderer(const ActorRenderer&) = delete;
    ActorRenderer& operator=(const ActorRenderer&) = delete;

    // The actor BuildActorForSpawn makes for a spawn's race and gender. A race without a row uses HUM / HUF
    // (NpcManager_GetModelCode); a row whose actor is not loaded gets HUM / HUF too (`human_fallback`: the
    // spawn becomes a Human, WorldState::make_actors). `model` is null when the actor exists but has nothing
    // to draw (`actor`, e.g. race 127's IVM), or when not even HUM / HUF is loaded (draw a box instead).
    struct ModelChoice {
        std::shared_ptr<const assets::CharacterModel> model;
        bool actor = false;
        bool human_fallback = false;
    };
    ModelChoice choose_model(const game::Spawn& spawn);
    std::shared_ptr<const assets::CharacterModel> model_for(const game::Spawn& spawn) { return choose_model(spawn).model; }
    // The model origin's height above the spawn's z at the actor scale `scale` (SetHeights' +0x31c, a spawn's
    // heights.model_scale): 0 for WLD models, the ROOT_BONE drop for EQG ones (game/spawn_heights.h).
    static float model_z_offset(const assets::CharacterModel& model, float scale, const game::HeightTables* tables);
    // New appearances are built on a worker thread (dressing, reading their textures and building their mips: 10 to
    // 30 ms each in a busy zone); the main thread applies the finished ones in prepare_spawns, putting their meshes
    // and textures up for the GPU. The time a frame may spend applying them, in milliseconds; the rest wait for the
    // next frame (drawn as they were). The first one of a frame is always applied. 4 ms: a quarter of a 60 Hz frame.
    static constexpr double kBuildBudgetMs = 4.0;
    void set_build_budget_ms(double ms) { build_budget_ms_ = ms; }
    // The camera's actor clip distance (game::actor_clip_distance): spawns farther from the camera are not drawn,
    // and draw_spawns / draw_prepared still count them as the actor renderer's. 0 (the default): no clip.
    void set_actor_clip(float distance) { actor_clip_ = distance; }
    // Spawns the game hides (PlayerZoneClient::IsInvisible: the bazaar's Hide Traders, the barter hall's Hide
    // Buyers): not drawn, and not handed to the placeholder boxes either. Null: none.
    void set_hidden_spawns(const std::unordered_set<std::uint32_t>* ids) { hidden_ = ids; }

    // Draws every spawn that has a model, animated from its movement
    // (display_position change: idle, walk or run). Returns the ids it handled
    // (drawn, empty or past the actor clip), so the caller can draw the others
    // as boxes. hide_id: a spawn not to draw.
    const std::unordered_set<std::uint32_t>& draw_spawns(const FrameInfo& frame, const glm::mat4& view_proj,
                                                         const glm::vec3& camera_pos, const game::ViewDistance& view,
                                                         const game::WorldState& world, std::uint32_t hide_id,
                                                         float dt);
    // draw_spawns in two halves, so the shadow cascades can be drawn between them: prepare_spawns animates and
    // poses (before VkContext::begin_scene), draw_prepared records the scene pass and returns the ids drawn.
    void prepare_spawns(const FrameInfo& frame, const glm::vec3& camera_pos, const game::WorldState& world,
                        std::uint32_t hide_id, float dt);
    const std::unordered_set<std::uint32_t>& draw_prepared(const FrameInfo& frame, const glm::mat4& view_proj,
                                                           const glm::vec3& camera_pos, const game::ViewDistance& view);
    // The isometric camera's silhouette (shaders/actor_silhouette.frag): the prepared instances of the spawns `ids`
    // (the player, and its mount) as a flat colour where they are behind what is drawn already. Before
    // draw_prepared, after the zone and its objects, so the actors drawn after cover it where they are in sight.
    // `color` is linear, its alpha the strength.
    void draw_silhouette(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                         std::span<const std::uint32_t> ids, const glm::vec4& color);
    // The prepared instances' opaque and alpha-tested parts into shadow cascade `cascade` (the cascades from the
    // last set_lighting), between VkContext::begin_shadow_cascade and end_shadow_cascade.
    void draw_shadow(const FrameInfo& frame, std::uint32_t cascade);
    // Whether a prepared instance stands within `radius` of `light`; and those that do into face `face` of the
    // light's shadow cube (render/point_shadows.h), between VkContext::begin_point_shadow and end_point_shadow, but
    // spawn `skip_spawn` (prepare_spawns' ids; 0 none) and, with `skip_around`, those whose bounds hold the light: a
    // body round the light it carries, or a spell's on it, would only black out the world.
    bool any_in_reach(const glm::vec3& light, float radius) const;
    void draw_point_shadow(const FrameInfo& frame, const glm::vec3& light, float radius, std::uint32_t face,
                           std::uint32_t skip_spawn = 0, bool skip_around = false);
    // The shadow clip plane (CDisplay_SetShadowClipPlane: ShadowClipPlane x 5 + 10 units): only actors within `reach`
    // of `from` (the camera) cast shadows, into the cascades and the light cubes; 0 or less, all of them.
    void set_shadow_reach(const glm::vec3& from, float reach) {
        shadow_from_ = from;
        shadow_reach_ = reach;
    }
    // The view the frame's scene is drawn with, and whether shadows can take spawns this frame: prepare poses only
    // the instances that can show (actor_needs_pose). Until it is set, every instance is posed (the viewers).
    void set_pose_view(const glm::mat4& view_proj, bool shadows) {
        pose_view_ = frustum_from(view_proj);
        pose_shadows_ = shadows;
    }

    // Draws explicit instances (the model viewer).
    void draw(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
              const game::ViewDistance& view, std::span<const ActorDraw> actors);
    // Poses explicit instances into this frame's bone buffer, for draw_shadow and draw_prepared (draw in two halves).
    void prepare(const FrameInfo& frame, std::span<const ActorDraw> actors);

    // The local player's motion as its controller knows it (the world's spawn
    // has no speeds or water levels for it): drives its animation.
    struct PlayerMotion {
        bool valid = false;
        glm::vec3 velocity{0.0f}; // units per tick
        float speed_run = 0.0f;
        std::uint8_t feet_wet = 0, body_wet = 0;
        float speed_heading = 0.0f; // +0x8c
        bool attacking = false;     // __Attack (auto attack on)
        bool first_person = false;  // CDisplay::cameraType 0
        std::int32_t colliding_type = 0; // CollidingType +0x24 (6: on a ladder)
    };
    void set_player_motion(const PlayerMotion& m) { player_motion_ = m; }
    // The animation object's vf24 on a spawn drawn with a model (game::mechanics::is_jump_animation on its +0x10c0
    // as last chosen); nothing for a spawn not drawn yet.
    std::optional<bool> jump_animation_playing(std::uint32_t id) const;

    // The pose a spawn was last drawn in (read-only), for the mouse pick
    // (game/picking.h): its triangles in world space, each wound so that
    // normalize((v0 - v1) x (v2 - v1)) faces out of the front face (the zone
    // records' rule), and each bone that has a parent as (parent, bone) world
    // positions. False when the spawn was not drawn with a model last frame.
    struct SpawnPose {
        std::vector<glm::vec3> triangles; // 3 vertices each
        std::vector<std::pair<glm::vec3, glm::vec3>> bones;
        glm::vec3 center{0.0f}; // bounding sphere of the triangles
        float radius = 0.0f;
    };
    bool spawn_pose(std::uint32_t id, SpawnPose& out) const;
    // The same triangles in the spawn's own frame (about its position, unturned): a rigid model, such as a vehicle's
    // hull and deck, re-placed at another pose without posing it again (game/boats.h).
    bool spawn_local_triangles(std::uint32_t id, std::vector<glm::vec3>& out) const;
    // Where it was last drawn: the model origin and a generous radius
    // (the model's bounding radius at its scale), without posing it.
    bool spawn_bounds(std::uint32_t id, glm::vec3& origin, float& radius) const;
    // The model a spawn was last drawn with (nullptr when it was not), and a bone's world matrix in that pose
    // (the bone's frame: the matrix a held item or a model point hangs from); false when the spawn was not
    // drawn with a model or the bone is out of range.
    const assets::CharacterModel* drawn_model(std::uint32_t id) const;
    float drawn_alpha(std::uint32_t id) const; // 0 without a drawn actor
    // The model a spawn wears whether or not it was drawn (drawn_model's, else the one kept for a spawn hidden on
    // purpose, such as the player in first person); nullptr when it has none yet.
    const assets::CharacterModel* spawn_model(std::uint32_t id) const;
    // The dressed mesh it was drawn with (nullptr: none, or the model's default geometry).
    const assets::CharacterMesh* drawn_mesh(std::uint32_t id) const;
    bool drawn_bone(std::uint32_t id, int bone, glm::mat4& world) const;
    // The actor's own world matrix as last drawn (its place, turn and scale; the actor's vtable +0x2c4 matrix that
    // spell emitters on it take their axes from); false when it was not drawn.
    bool drawn_transform(std::uint32_t id, glm::mat4& world) const;
    // Actor vfFC(0): the authored head/name role's posed world translation.
    // Missing role metadata leaves the output alone and returns false.
    bool drawn_name_position(std::uint32_t id, glm::vec3& position) const;
    static bool name_position(const ActorDraw& actor, glm::vec3& position);
    // A bone's world matrix for an instance in its pose (the model viewer's draws).
    static bool bone_world(const ActorDraw& a, int bone, glm::mat4& world);
    // An instance's own world matrix (drawn_transform's, for the model viewer's draws).
    static glm::mat4 draw_matrix(const ActorDraw& a) { return instance_matrix(a); }
    // Seats `rider` on `mount` as it is posed (PlayerClient_AttachRiderToMount): on the mount's dag 0xe
    // (CharacterModel::mount_point_bone). False, leaving the rider as it is, when the mount has no such dag.
    static bool seat_rider(const ActorDraw& mount, ActorDraw& rider, const game::HeightTables* tables);
    // The top of a seated rider (attached by seat_rider): its model's rest-pose top in the mount's bone frame it is
    // drawn in, where a standing actor's is its spawn's place plus that top. False when `a` is not seated.
    static bool rider_top(const ActorDraw& a, glm::vec3& top);
    // The same for a spawn as it was last drawn; false when it was not drawn seated on a mount.
    bool drawn_rider_top(std::uint32_t id, glm::vec3& top) const;
    // The bone frames of an instance before skinning (its clip blended with the previous one; the upper body's
    // bones from upper_clip when it has one), model space.
    static void pose_bones(const ActorDraw& a, std::vector<assets::BonePose>& local,
                           std::vector<assets::BonePose>& previous, std::vector<glm::mat4>& world);
    // Whether it stands: drawn in its stand (P01) or idle (O01) clip, or the rest pose; eqgame's STND and IDLE
    // animations (app/actor_emitters.h: the model emitters those start). False when it was not drawn.
    static bool standing(const ActorDraw& a);
    bool drawn_standing(std::uint32_t id) const;
    // Whether the actor drawn last was playing the clip `code` (its whole body or its upper body), and how far in.
    bool drawn_playing(std::uint32_t id, std::string_view code, float& time) const;
    static bool playing(const ActorDraw& a, std::string_view code, float& time);

    // Before the renderer goes with its zone (ClientApp::unload_zone): what the next zone's may keep (see Kept).
    // Waits for the GPU to go idle; the renderer draws no characters afterwards.
    std::unique_ptr<Kept> release_kept();

    const ActorRenderStats& stats() const { return stats_; }
    // The scene light and point lights (render/scene_lighting.h), classic or modern; classic_full_bright until set.
    void set_lighting(const SceneLighting& l) { lighting_ = l; }
    const assets::CharacterLibrary& library() const { return *library_; }

private:
    enum class Pass : std::uint8_t { Opaque, Blended, Additive };
    struct Part {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t texture = 0;
        Pass pass = Pass::Opaque;
        float alpha_ref = 0.0f;
        float opacity = 1.0f;
        std::uint32_t tint = 0; // 0xAARRGGBB, high byte 0 = none
        std::uint32_t texture2 = 0xffffffffu; // SkinMeshLuclin's tinted second layer, or none
        std::uint32_t normal_map = 0, rough_map = 0; // a texture pack's maps' slots (0 = none)
        ActorSurface surface; // native EQG armor maps and highlight; other materials keep the old path
        bool native_surface = false;
        bool two_sided = false;
        bool fog = true;
    };
    struct GpuModel {
        Buffer vertices;
        Buffer indices;
        std::vector<Part> parts;
        bool used = false; // drawn since the renderer was made (release_kept keeps only these)
        std::uint64_t last_frame = 0; // VkContext::frames_begun() when last drawn or put up
        std::uint32_t last_ms = 0;    // now_ms_ then
        bool capacity_blocked = false; // a texture exists but the descriptor array was full
    };
    // A texture's slot (0: none, drawn white), with the names reading it looked up in the library
    // (assets::CharacterLookups): a zone archive defining one of them may give another image.
    struct TextureSlot {
        std::uint32_t slot = 0;
        assets::CharacterLookups::Names lookups;
        bool capacity_blocked = false;
    };
    struct SpawnState {
        std::shared_ptr<const assets::CharacterModel> model;
        std::shared_ptr<const assets::CharacterMesh> mesh; // current appearance, or null
        game::Appearance appearance;
        bool dressed = false;
        game::SpawnAnimState anim; // the original's animation choice state
        const assets::AnimationClip* motion_clip = nullptr; // the movement clip last asked
        ActorLayers layers; // the two bone groups and the action playing (OP_Animation)
        bool new_style = false; // the new-style animation object drew it last
        bool seen = false;
        // Not drawn on purpose (prepare_spawns): kept only so the look it wears is not freed; drawn again, it
        // starts as a new state.
        bool hidden = false;
        std::uint32_t action_seq = 0;
        bool action_seq_known = false;
    };
    // Each bone's group for a model (game::classic_bone_group / new_style_bone_group by its skeleton).
    const std::vector<std::uint8_t>& bone_groups(const assets::CharacterModel& model);
    std::unordered_map<const assets::CharacterModel*, std::vector<std::uint8_t>> bone_groups_;

    // An instance's placement.
    static glm::mat4 instance_matrix(const ActorDraw& a);

    // A texture a part draws with, and how it is read (a colour texture, or a texture pack's map of `name`).
    struct TextureRef {
        std::string name;
        bool masked = false;
        TextureUse use = TextureUse::Color;
        std::optional<assets::TextureMap> map;
        bool environment = false; // a DDS cube converted to an octahedral 2D texture
        std::string key() const; // texture_slots_'s
    };
    // A texture read for the GPU: the image as it goes up as `use` (a colour texture's mips built already), or
    // nullopt with the reason; and the names reading it looked up in the library.
    struct ReadTexture {
        std::optional<assets::Image> image;
        TextureUse use = TextureUse::Data;
        std::string error;
        assets::CharacterLookups::Names lookups;
    };
    // The CPU half of putting a mesh on the GPU: its vertices as the shaders take them, its parts with the textures
    // they name, and the textures read for it so far (by TextureRef::key).
    struct MeshPlan {
        struct PartPlan {
            Part part; // all but its texture slots
            TextureRef texture, normal, rough;
            std::optional<TextureRef> texture2;
            std::optional<TextureRef> native_normal, environment;
        };
        std::vector<std::byte> vertices;
        std::vector<PartPlan> parts;
        std::unordered_map<std::string, ReadTexture> textures;
    };
    static MeshPlan plan_mesh(const assets::CharacterMesh& mesh);
    // Reads a texture through the library's thread-safe calls (any thread).
    static ReadTexture read_texture(const assets::CharacterLibrary& library, const TextureRef& ref);
    // The other half, on the main thread: the mesh's buffers and new textures join `upload`.
    const GpuModel* install(const assets::CharacterMesh& mesh, MeshPlan& plan, UploadBatch& upload);
    // A texture's slot: the one it has, else one for the plan's image, else read here.
    std::uint32_t slot_for(const TextureRef& ref, MeshPlan& plan, UploadBatch& upload);

    // A new appearance built on the worker for a spawn: what it was asked for, the dressed mesh, and its plan.
    struct Dressing {
        std::shared_ptr<const assets::CharacterModel> model;
        game::Appearance appearance;
        std::shared_ptr<const assets::CharacterMesh> mesh;
        std::optional<MeshPlan> plan;
        ClaimHold held;    // the textures this job claimed: given back if it is dropped before they are installed
        int deferred = 0;  // frames it waited for a texture another job is reading
    };
    // The worker's job: touches nothing of the renderer's but `claims`, and the library through its locked calls.
    static Dressing dress_off_thread(const assets::CharacterLibrary& library,
                                     std::shared_ptr<const assets::CharacterModel> model,
                                     const game::Appearance& appearance, std::shared_ptr<TextureClaims> claims);
    // A texture the plan needs that has no slot, is not in the plan and is pending with another job (TextureClaims):
    // installing now would decode it here, on the main thread.
    bool waits_for_claimed(const MeshPlan& plan) const;
    // Asks the worker (started on first use) for a spawn's appearance; supersedes the spawn's request before.
    void request_dress(std::uint32_t id, const std::shared_ptr<const assets::CharacterModel>& model,
                       const game::Appearance& appearance);
    // Stops the worker (waiting for the appearance it is building) and forgets what it finished.
    void stop_dresser();
    std::shared_ptr<TextureClaims> claims_;
    std::unordered_map<std::uint32_t, Dressing> ready_; // done, waiting for a frame's budget
    // The appearances applied so far by model and look: a spawn asking for one again (coming back past the actor
    // clip, a crowd in the same uniform) takes it at once. Holds the model, its key's object.
    struct Looks {
        std::shared_ptr<const assets::CharacterModel> model;
        std::vector<std::pair<game::Appearance, std::shared_ptr<const assets::CharacterMesh>>> looks;
    };
    std::unordered_map<const assets::CharacterModel*, Looks> looks_;
    // Frees the looks no spawn wears that have not been drawn for kLookIdleMs (render/look_eviction.h): their meshes
    // and buffers. Shared textures go only when no remaining model reads their slot.
    // Swept every kLookSweepMs, from prepare_spawns.
    void evict_idle_looks();
    // Frees the texture slots no model reads (when `reclaim_first`), then retries capacity-blocked models
    // (render/look_eviction.h), at most kMaxWornRetries worn ones a sweep.
    void evict_unused_textures(bool reclaim_first);
    static constexpr std::size_t kMaxWornRetries = 1;
    std::uint32_t last_look_sweep_ms_ = 0;
    const std::shared_ptr<const assets::CharacterMesh>* applied_look(const assets::CharacterModel& model,
                                                                     const game::Appearance& a) const;
    // Stopped (stop_dresser) first thing in the destructor and release_kept. Its jobs hold their own references.
    std::unique_ptr<core::LatestJobs<std::uint32_t, Dressing>> dresser_;
    // The mesh on the GPU; a new one's buffers and textures join `upload`, which the caller submits before the
    // frame's commands use them.
    const GpuModel* gpu_model(const assets::CharacterMesh& mesh, UploadBatch& upload);
    // prepare with the frame's uploads so far in `upload` (submitted here, once, before the descriptors are written).
    void prepare(const FrameInfo& frame, std::span<const ActorDraw> actors, UploadBatch& upload);
    const assets::CharacterMesh& default_mesh(const assets::CharacterModel& model);
    // A texture pack's normal or roughness map for the texture `name` (assets/texture_overrides.h), 0 = none.
    std::uint32_t map_slot(const std::string& name, assets::TextureMap map, UploadBatch& upload);
    std::uint32_t texture_slot(const std::string& name, bool masked, UploadBatch& upload,
                               TextureUse use = TextureUse::Color);
    // Writes the textures added since into `slot`'s set (not the other's: its frame may be in flight).
    void rebuild_descriptors(std::uint32_t slot);
    VkDescriptorSet frame_set(const FrameInfo& frame); // the frame's set, brought up to date first
    void create_pipelines(const std::filesystem::path& shader_dir);
    void record(const FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                const game::ViewDistance& view);

    struct Prepared {
        const GpuModel* gpu = nullptr;
        std::uint32_t bone_offset = 0;
        std::uint32_t surface_offset = 0; // the model's first record in this frame's surface table; 0: old path
        bool eqg = false;
        glm::vec3 center{0.0f}; // the model origin and its bounding radius at its scale
        float radius = 0.0f;
        float alpha = 1.0f;
    };
    std::vector<Prepared> prepared_;
    std::vector<std::uint8_t> seen_; // record's: which prepared_ show in its view
    std::vector<std::uint32_t> prepared_ids_; // prepare_spawns' spawns, for draw_prepared

    VkContext& vk_;
    SceneLighting lighting_ = classic_full_bright();
    std::unique_ptr<FrameBuffers> lighting_buffers_; // SceneLighting and armor surfaces per frame (binding 1)
    std::vector<std::byte> lighting_data_; // only the used surface-table prefix is copied each frame
    bool surface_capacity_reported_ = false;
    std::shared_ptr<const assets::CharacterLibrary> library_;
    // The world's racedata tables, kept from prepare_spawns for choose_model's live model codes.
    std::shared_ptr<const game::HeightTables> height_tables_;
    ActorRenderStats stats_;

    std::unordered_map<const assets::CharacterMesh*, GpuModel> models_;
    // Keeps meshes uploaded to the GPU alive (model defaults and appearances).
    std::unordered_map<const assets::CharacterMesh*, std::shared_ptr<const assets::CharacterMesh>> mesh_refs_;
    std::unordered_map<const assets::CharacterModel*, std::shared_ptr<const assets::CharacterMesh>> default_meshes_;
    double build_budget_ms_ = kBuildBudgetMs;
    std::unordered_map<std::string, ModelChoice> by_code_;
    // prepare_spawns' spawns whose actor draws nothing: empty, or past the actor clip (no box either).
    std::vector<std::uint32_t> empty_ids_;
    float actor_clip_ = 0.0f; // set_actor_clip
    const std::unordered_set<std::uint32_t>* hidden_ = nullptr; // set_hidden_spawns
    std::unordered_map<std::uint32_t, SpawnState> spawns_;
    std::unordered_set<std::uint32_t> drawn_;
    std::unordered_map<std::uint32_t, ActorDraw> last_draws_; // the spawns drawn last frame, for spawn_pose
    std::unordered_map<std::uint32_t, std::pair<glm::vec3, float>> last_spawn_poses_; // their spawns' place, heading
    PlayerMotion player_motion_;
    std::uint32_t now_ms_ = 0; // the display clock
    double clock_ = 0.0;
    std::minstd_rand rng_{0x5eed};

    std::vector<Texture> textures_; // a freed slot holds no image (drawn white) until it is used again
    std::vector<std::uint32_t> free_textures_;
    std::unordered_map<std::string, TextureSlot> texture_slots_;
    // The models of the default meshes made for spawns, kept alive with them (default_meshes_ is keyed by address).
    std::unordered_map<const assets::CharacterModel*, std::shared_ptr<const assets::CharacterModel>> model_refs_;
    // A texture into a slot (a freed one first); nullopt when the array is full.
    std::optional<std::uint32_t> add_texture(UploadBatch& upload, const assets::Image& image, TextureUse use);
    // Takes `kept` over, then frees what this renderer's library would not hand out the same.
    void adopt(Kept& kept);
    void free_model(GpuModel& g);
    void retire_model(GpuModel& g);
    std::uint32_t max_textures_ = 0;
    std::array<std::uint32_t, VkContext::kFramesInFlight> descriptor_textures_{}; // textures written into each set

    static constexpr std::uint32_t kMaxBones = 65536;
    std::array<Buffer, VkContext::kFramesInFlight> bones_{};
    std::array<void*, VkContext::kFramesInFlight> bones_mapped_{};

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, VkContext::kFramesInFlight> sets_{};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[3]{};
    VkPipeline shadow_pipeline_ = VK_NULL_HANDLE;
    VkPipeline silhouette_pipeline_ = VK_NULL_HANDLE; // draw_silhouette
    glm::vec3 shadow_from_{0.0f};
    float shadow_reach_ = 0.0f;
    std::optional<Frustum> pose_view_; // set_pose_view
    bool pose_shadows_ = true;
    VkPipeline point_shadow_pipeline_ = VK_NULL_HANDLE;

public:
    struct Kept {
        VkContext* vk = nullptr;
        std::unordered_map<const assets::CharacterMesh*, GpuModel> models;
        std::unordered_map<const assets::CharacterMesh*, std::shared_ptr<const assets::CharacterMesh>> mesh_refs;
        std::unordered_map<const assets::CharacterModel*, std::shared_ptr<const assets::CharacterMesh>> default_meshes;
        std::unordered_map<const assets::CharacterModel*, std::shared_ptr<const assets::CharacterModel>> model_refs;
        std::vector<Texture> textures; // slot 0 is the plain white one
        std::unordered_map<std::string, TextureSlot> texture_slots;
        Kept() = default;
        Kept(const Kept&) = delete;
        Kept& operator=(const Kept&) = delete;
        ~Kept();
    };
};

} // namespace hailstone::render
