#pragma once

// Spell stage particles in the game: the cast stage at the caster
// (OP_BeginCast), the impact stage at the target (OP_Action 231) and the
// effects the server plays by number (OP_SpellEffect, the nimbus packets;
// game/spell_effect_timers.h), through
// game::spawn_stage_emitters / game::ParticleSystem, drawn by
// render::ParticleRenderer. The emitters hang from the actors' spell points
// (assets/model_points.h) as the actor renderer last posed them. Settings:
// eqclient.ini [Defaults] SpellParticleDensity and SpellParticleOpacity.

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "assets/model_points.h"
#include "assets/spell_effects.h"
#include "core/ini_file.h"
#include "game/light_list.h"
#include "game/particles.h"
#include "game/path_trail.h"
#include "game/spawn_heights.h"
#include "game/spell_effect_timers.h"
#include "render/particle_renderer.h"

namespace hailstone { class GameDir; }
namespace hailstone::assets { struct CharacterModel; }
namespace hailstone::render { class ActorRenderer; }
namespace hailstone::game { class CollisionMesh; struct Missile; }
namespace hailstone::net { class GameConnection; struct SpellEffectCue; struct EffectPacketCue; }

namespace hailstone::app {

class SpellParticles {
public:
    // Resolve the current zone on each probe: construction precedes zone load, and zoning replaces the mesh.
    using CollisionSource = std::function<const game::CollisionMesh*()>;
    SpellParticles(render::VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir,
                   CollisionSource collision);
    // The same gameplay-to-emitter runtime without a GPU; useful to offline tools and protocol integration tests.
    SpellParticles(std::vector<assets::EmitterDefinition> definitions, std::vector<assets::SpellEffect> effects,
                   CollisionSource collision = {});
    const game::ParticleSystem& simulation() const { return particles_; }
    // Offline tools may supply the same named blood-point pose that the actor renderer normally resolves.
    using BloodPoint = std::function<std::optional<glm::mat4>(std::uint32_t)>;
    void set_blood_point(BloodPoint point) { blood_point_ = std::move(point); }
    // The sprite must come from the caller's game assets. No sprite: no blood emitter.
    void set_blood_sprite(assets::EmitterDefinition definition);
    using MissilePose = std::function<std::optional<glm::mat4>(std::uint32_t serial)>;
    void set_missile_pose(MissilePose pose) { missile_pose_ = std::move(pose); }
    void missile_effect(const net::GameConnection& conn, const game::Missile& missile, int stage,
                        const render::ActorRenderer* actors);
    void end_missile(std::uint32_t serial);

    // Plays the frame's spell effect cues (those with a delay when it is up) and effect packets, and advances the
    // emitters; `actors` (may be null) gives the spawns' poses for their spell points.
    void update(net::GameConnection& conn, std::span<const net::SpellEffectCue> cues,
                std::span<const net::EffectPacketCue> effect_packets, const render::ActorRenderer* actors, float dt);
    // Before the frame: loads textures the emitters need.
    void prepare();
    // `modern`: render::ParticleRenderer::draw's.
    void draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& view, float glow = 1.0f,
              const render::SceneLighting* modern = nullptr);
    void clear();
    // SpellParticleDensity (the Options window's combo): the emitters made from now on. eqclient_density() is the
    // ini's, read at the start.
    void set_density(float density) { settings_.density = std::clamp(density, 0.0f, 1.0f); }
    float density() const { return settings_.density; }
    float eqclient_density() const { return eqclient_density_; }
    // Hailstone: the Options window's per-source densities (game::spell_fx_source), each scaling the density above
    // for the stages played from now on; 1 (High) by default.
    void set_source_density(game::SpellFxSource source, float density) {
        source_density_[static_cast<std::size_t>(source)] = std::clamp(density, 0.0f, 1.0f);
    }
    float source_density(game::SpellFxSource source) const { return source_density_[static_cast<std::size_t>(source)]; }
    // The floor below a point (attach 12's GetPlayerFloorHeight): the zone's collision, when it is loaded.
    using FloorFn = std::function<std::optional<float>(const glm::vec3&)>;
    void set_floor(FloorFn floor) { floor_ = std::move(floor); }
    // Visible, upward-facing ground for Find trails; separate from spell attachment floors.
    void set_trail_floor(FloorFn floor) { trail_floor_ = std::move(floor); }
    // The light the glowing effects give off (game::particle_lights), the `max` brightest.
    std::vector<game::LightSource> lights(std::size_t max) const;
    // The Find window's guide path (app/guide_path.h) to draw, the player first; empty: none.
    void set_guide_line(std::vector<glm::vec3> line) { guide_line_ = std::move(line); }
    // Transient UI visibility: hide only route trails and completion effects, retaining both route inputs.
    void set_paths_visible(bool visible) { paths_visible_ = visible; }
    void set_find_trail_style(game::FindTrailStyle style) { find_trail_style_ = style; }
    game::FindTrailStyle find_trail_style() const { return find_trail_style_; }

private:
    void bind_collision(CollisionSource collision);
    // The find path's look in the world (game/player_path.h, PlayerPath_Update's drawing half): its line as glowing
    // still navigation ribbons or rainbow strands and the supported FinishEmitterN at the end.
    void update_path(net::GameConnection& conn);
    // The point's world matrix on a spawn as last drawn.
    std::optional<glm::mat4> point_world(const render::ActorRenderer& actors, std::uint32_t id, int point);
    std::optional<glm::mat4> blood_point_world(const render::ActorRenderer* actors, std::uint32_t id) const;
    void update_blood(net::GameConnection& conn, const render::ActorRenderer* actors);

    // GetParticleScaleFactor: the model's moddat.ini PESFactor.
    float particle_scale(const assets::CharacterModel& model) const;

    std::vector<assets::SpellEffect> effects_;
    game::HeightTables heights_; // moddat.ini's PESFactors
    FloorFn floor_;
    FloorFn trail_floor_;
    IniFile sp_offsets_; // Resources\SPOffsets.ini
    assets::SpellPoints spell_points_;
    game::ParticleSystem particles_;
    game::SpellParticleSettings settings_;
    float eqclient_density_ = 1.0f;
    std::array<float, game::kSpellFxSourceCount> source_density_{1.0f, 1.0f, 1.0f};
    std::unique_ptr<render::ParticleRenderer> renderer_;
    std::optional<std::size_t> path_definition_; // the thin rainbow segments' definition
    std::optional<std::size_t> navigation_definition_;
    game::FindTrailStyle find_trail_style_ = game::FindTrailStyle::Navigation;
    bool paths_visible_ = true;
    std::optional<std::size_t> blood_definition_;
    BloodPoint blood_point_;
    MissilePose missile_pose_;
    std::vector<glm::vec3> guide_line_;          // set_guide_line
    // A trail (game/path_trail.h) as the still particles of `key`, or none.
    void draw_trail(std::uint32_t key, std::span<const glm::vec3> line, float far);
    // A delayed cue and when it plays.
    struct Held {
        double at;
        int stage;
        std::uint32_t caster, target, spell;
        std::uint32_t source; // the impact stage's caster (the cue's source)
        std::int32_t duration_ms;
    };
    std::vector<Held> held_;
    double clock_ = 0.0;
    game::SpellEffectTimers timers_; // OP_SpellEffect's
    void play(net::GameConnection& conn, const Held& cue, const render::ActorRenderer* actors);
    // A stage of an effect by number (SpellEffectTimers_PlayStage, Handle_OP_AddNimbusEffect).
    void play_effect(net::GameConnection& conn, const game::SpellEffectStagePlay& play,
                     const render::ActorRenderer* actors);
    std::optional<game::StageActor> stage_actor(const net::GameConnection& conn, std::uint32_t id,
                                                const render::ActorRenderer* actors);
    // The Options window's density for an effect cast by `cast_by` on `struck` (game::spell_fx_source).
    float source_density_for(const net::GameConnection& conn, std::uint32_t cast_by, std::uint32_t struck) const;
};

} // namespace hailstone::app
