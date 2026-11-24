#pragma once

// The actors' own particle emitters (assets/model_points.h ModelEmitter): an
// EQG model's `<model>.prt`, a held EQG item's, and WLD skeleton particle
// clouds, e.g. a glowing weapon or a
// creature's flames. 0x1006b130 / 0x1006b290 make one through
// CParticleSystem vtable +0x20 (0x10070640, ActorEmittersNew.edd) for each
// always-on entry (trigger 0, variant below 1) whose model point exists, when
// the actor is made; it stays with the actor. Here the dressed mesh carries the
// entries (assets::MeshEmitter) and each drawn actor keeps a persistent emitter
// per entry on its point, placed each frame from the pose for that frame.
// Density and opacity: eqclient.ini [Defaults] ActorParticleDensity /
// ActorParticleOpacity. Of the animation-started entries (trigger 1; their
// animation is an index into eqgame's animation table at 0xaae3d8), those of
// STND (31) and IDLE (32) run while the actor stands: the rune circles'
// (#TPTrigger*, T05) blue lights. The others (footfalls, swings, casts) start
// each time their animation starts (or loops round) on the actor, after the
// entry's delay (+0x54), and emit while the animation plays, at most the
// entry's duration: the animation's clip is the table row's old-style code
// (new_to_old_anim, old_anim_clip). How the original times them is not
// traced; this is the client's own reading.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "core/worker_pool.h"
#include "game/particles.h"
#include "game/spawn_heights.h"
#include "render/particle_renderer.h"

namespace hailstone { class GameDir; }
namespace hailstone::assets { struct CharacterMesh; struct MeshEmitter; }
namespace hailstone::assets { struct ObjectAnimation; }

namespace hailstone::app {

class ActorEmitters {
public:
    ActorEmitters(render::VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir,
                  bool scene = true);
    // Without a renderer (the tests). `async`: stepped by simulate_async as the scene's are, not by update.
    explicit ActorEmitters(std::vector<assets::EmitterDefinition> definitions, game::HeightTables heights = {},
                           bool async = false);
    // The simulations as of the last step (waiting for a step still running on a worker).
    const game::ParticleSystem& simulation() const { finish(); return particles_; }
    const game::ParticleSystem& cloud_simulation() const { finish(); return clouds_; }
    // The loaded character/equipment archives supply WLD cloud sprites (outside the EDD effect folders).
    // `any_thread`: render::ParticleRenderer::set_texture_reader.
    void set_texture_reader(render::ParticleRenderer::TextureReader reader, bool any_thread = false);
    glm::vec3 texture_color(const std::string& name, bool cloud = false) const;

    // An actor drawn this frame and the mesh it was drawn with.
    struct Actor {
        std::uint32_t id = 0;
        const assets::CharacterMesh* mesh = nullptr;
        bool standing = false; // in its stand or idle animation (render::ActorRenderer::standing)
        float alpha = 1.0f;
        // Hailstone (not in the original): false when no particle of its could show (outside the camera's view, or
        // past the classic lighting's 500 units). Its emitters then emit nothing and are not placed (no pose is
        // asked for), its animation-started entries start none, and what it emitted already lives on.
        bool in_view = true;
    };
    // A bone's world matrix on an actor as drawn (false: not drawn).
    using BoneWorld = std::function<bool(std::uint32_t id, int bone, glm::mat4& world)>;
    // Whether the prepared actor pose plays `code`, and how far in (render::ActorRenderer::drawn_playing).
    using Playing = std::function<bool(std::uint32_t id, std::string_view code, float& time)>;

    // Starts the emitters of actors that came or changed their mesh, stops those of actors gone, and queues textures
    // for existing renderers. Previews also advance the particles and prepare textures here, outside a frame.
    void update(float dt, std::span<const Actor> actors, const BoneWorld& bone_world, const glm::vec3& viewer,
                const Playing& playing = {});
    // Outside a frame, create the renderers as needed and upload completed particle textures.
    void prepare();
    // Advances scene particles using this frame's poses. The step runs on a worker when one is available. Reading
    // the simulation or drawing waits for the step so both use the current pose.
    void simulate_async(float dt, const BoneWorld& bone_world);
    // `modern`: render::ParticleRenderer::draw's.
    void draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& view, float glow = 1.0f,
              const render::SceneLighting* modern = nullptr);
    void clear();

    // A model's spell point on a spawn as drawn: its world matrix (assets::SpellPoints; false: not drawn or no such
    // point). Set by the app each frame; start_effect and the placing of its emitters read it.
    using PointWorld = std::function<bool(std::uint32_t id, int point, glm::mat4& world)>;
    // The floor below a point (the zone's collision), when it is loaded.
    using FloorFn = std::function<std::optional<float>(const glm::vec3&)>;
    void set_point_world(PointWorld point_world) { point_world_ = std::move(point_world); }
    void set_floor(FloorFn floor) { floor_ = std::move(floor); }
    // An effect the server plays on a spawn by number (OP_LevelAppearance): the actor file's emitter `effect` on one
    // of the spawn's ten spell points, as CActor vf126 @0x1003a950 makes it. Without `flag` the emitter is attached
    // to the point and follows it. With it the original places the emitter once, from the point's position cast
    // down onto the floor and raised by 0.05, with the point's scale and the actor's turn, instead of attaching it;
    // here the same with the world's axes for the turn (docs/unknowns.md), and the point's own z without a floor. Like every emitter of the actor file it is made at the actor
    // density and opacity and ends with its definition's own duration. Returns false, starting nothing, for an effect
    // not in the file, a slot the model has no point for, or a spawn not drawn.
    struct Effect {
        std::uint32_t actor = 0;
        std::uint32_t effect = 0;
        std::uint32_t slot = 0;
        bool flag = false;
    };
    bool start_effect(const Effect& effect);
    // Drops the effects of spawns `alive` no longer knows, and the records of those that ended. At most
    // kMaxEffects run: a newer one stops the oldest.
    void prune_effects(const std::function<bool(std::uint32_t id)>& alive);
    static constexpr std::size_t kMaxEffects = 64;
    std::size_t effect_count() const { return effects_.size(); }
    std::size_t emitters() const {
        finish();
        return particles_.emitters().size() + clouds_.emitters().size();
    }
    // ActorParticleDensity (the Options window's combo; the effect manager's +0xac): the actors' running emitters
    // emit at it from now on, keeping their particles; at 0 the EDD entries are stopped and none is tried, and from 0
    // they are made again. eqclient_density() is the ini's, read at the start.
    void set_density(float density);
    float density() const { return settings_.density; }
    float eqclient_density() const { return eqclient_density_; }

private:
    struct Live;
    static glm::mat4 local_matrix(Live& live, const assets::MeshEmitter& me);
    std::optional<game::EmitterAttachment> attach(std::uint32_t id, std::int32_t i, const BoneWorld& bone_world);
    static std::uint64_t attachment_key(std::uint32_t actor, std::int32_t point) {
        return std::uint64_t{actor} << 32 | static_cast<std::uint32_t>(point);
    }
    void finish() const; // waits for simulate_async's step
    // An effect's attached emitter uses point numbers from here up: kEffectPointBase + the spell point (an ordinary
    // actor emitter's point is its index in the mesh's emitter list).
    static constexpr std::int32_t kEffectPointBase = 0x1000;

    struct Live {
        // The mesh from the last update, and which one it is: a mesh freed and another dressed at its address (the
        // spawn dressed again) has another serial (assets::MeshSerial), so its emitters start afresh.
        const assets::CharacterMesh* mesh = nullptr;
        std::uint64_t mesh_serial = 0;
        std::vector<std::uint32_t> keys;
        std::vector<std::uint32_t> cloud_keys;
        double elapsed = 0.0; // the attached item's own animation controller starts with this instance/mesh
        struct CloudPose {
            std::uint64_t ms = ~std::uint64_t{0};
            std::vector<glm::mat4> motion;
        };
        std::unordered_map<const assets::ObjectAnimation*, CloudPose> cloud_poses;
        bool restart_edd = false; // ActorParticleDensity off or on: the separate WLD list keeps running
        float alpha = 1.0f;
        bool held = false; // Actor::in_view false this frame
        std::vector<std::uint32_t> standing_keys; // the STND / IDLE entries', while it stands
        // The other animation-started entries, by entry: the time into their animation last frame (negative: not
        // playing), a start waiting for the entry's delay (seconds left, negative none) and the emitter running.
        struct Triggered {
            float last = -1.0f, pending = -1.0f, length = 0.0f;
            std::uint32_t key = 0;
        };
        std::unordered_map<std::size_t, Triggered> triggered;
        bool seen = false;
    };
    game::ParticleSystem particles_;
    game::ParticleSystem clouds_{std::vector<assets::EmitterDefinition>{assets::EmitterDefinition{}}};
    // Own the definitions as long as their registered indexes can be used. Cleared with the zone/preview subject.
    std::unordered_map<const assets::EmitterDefinition*, std::size_t> cloud_indexes_;
    std::vector<std::shared_ptr<const assets::EmitterDefinition>> cloud_definitions_;
    game::HeightTables heights_;
    game::SpellParticleSettings settings_;
    float eqclient_density_ = 1.0f;
    std::unique_ptr<render::ParticleRenderer> renderer_;
    std::unique_ptr<render::ParticleRenderer> cloud_renderer_;
    render::ParticleRenderer::TextureReader texture_reader_;
    bool texture_reader_any_thread_ = false;
    render::VkContext* vk_ = nullptr;
    const GameDir* game_ = nullptr;
    std::filesystem::path shader_dir_;
    bool scene_ = true;
    std::unordered_map<std::uint32_t, Live> live_;
    // update's scratch, kept so its memory is reused frame to frame: the emitters to stop in one pass each (by key,
    // by actor in each system), the actors whose entries start afresh (their index, whether the mesh changed) and
    // each actor's Live (nullptr: no emitters).
    std::vector<std::uint32_t> stop_keys_, stop_actors_, stop_cloud_actors_;
    std::vector<std::pair<std::size_t, bool>> restarts_;
    std::vector<Live*> lives_;
    std::uint32_t next_key_ = 1;
    PointWorld point_world_;
    FloorFn floor_;
    struct RunningEffect {
        std::uint32_t key = 0, actor = 0;
    };
    std::vector<RunningEffect> effects_; // oldest first
    bool async_ = false; // the scene's: simulate_async steps the systems
    // simulate_async's table of the attached emitters' places, by attachment_key; the step reads it alone.
    std::unordered_map<std::uint64_t, std::optional<game::EmitterAttachment>> attachments_;
    mutable core::WorkerPool::Task step_; // last: destroyed first, so a running step ends before what it uses goes
};

} // namespace hailstone::app
