#pragma once

// Spell particle emitters: the graphics DLL's CParticleSystem (EQGraphicsDX9.dll), traced from its code.
//
// eqgame's SpellEffect_SpawnStageEmitters @0x51aa20 walks the four emitter
// slots of an effect stage and calls CreateSpellEmitter (effect manager vtable
// +0x18, 0x10070580 -> CEmitterDefinitionCollection_CreateEmitter @0x10070440
// -> CEmitterDef::CreateEmitter @0x1006c920) for each:
//   - a slot is skipped when its index is < 1 or the caster's level is below
//     the slot's minimum (level 1 for a missing caster);
//   - the location picks the given point (0), the caster (1), the missile (2)
//     or the target (3); the attach id one of the actor's ten spell points
//     (assets/model_points.h; ActorClient +0x148, 0x58cd90): 1 point 0,
//     2..7 points 1..6 or else point 0, 8 the actor without a point, 9 / 10
//     the eyes (points 7 / 8; the slot is skipped without them), 11 the mouth
//     (point 9), 12 the actor without a point at the chest's x and y and the
//     floor's z, scaled by the actor's particle scale (vtable +0x218);
//   - density = eqclient.ini SpellParticleDensity and opacity =
//     SpellParticleOpacity (clamped to [0, 1]; both 1.0 with the flag).
// CreateEmitter (emitter_counts) turns the density into the emitter's tick rate
// and particle capacity; the per-frame update (FUN_10072160: emission, spawn,
// motion, ramps) and the quad builder are in ParticleSystem::update and
// render::ParticleRenderer. Not traced (docs/unknowns.md): EDD +0x13c / +0x140
// and the globals at +0x148..+0x168, +0x178's orientation, and the DLL's random
// tables (filled at run time; this client draws from its own generator).

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "assets/spell_effects.h"

namespace hailstone::game {

// eqclient.ini [Defaults] SpellParticleDensity / SpellParticleOpacity.
struct SpellParticleSettings {
    float density = 1.0f;
    float opacity = 1.0f;
};
// From the ini values (missing -> 1.0), clamped to [0, 1].
SpellParticleSettings spell_particle_settings(std::optional<float> density, std::optional<float> opacity);

// SpawnBloodSpurt's short-lived cloud, with the player's own I_BLOODSPRITE texture. The resource-manager
// converter gives this cloud a sphere emission, fixed lifetime and a non-additive material (0x58f1c0/0x10071640).
assets::EmitterDefinition blood_spurt_definition(std::string texture);

// Hailstone (not in the original): whose a spell effect stage is, for the Options window's per-source spell
// particle densities, which scale SpellParticleDensity. The original has one density and an "On For" filter
// (SpellParticleCastFilter, 0x514bd0: self, group, raid, all players, and NPCs as a bit); this client offers the
// three densities instead and does not read the filter.
enum class SpellFxSource : std::uint8_t { Self = 0, Players = 1, Npcs = 2 };
inline constexpr std::size_t kSpellFxSourceCount = 3;
// `caster` cast the spell and `struck` is the spawn it lands on (the caster again for the cast stage); `me` the
// player. A spell the player cast, or one that lands on the player, is Self (as 0x514bd0 always shows those). Any
// other is the caster's kind: NPCs (pets and mercenaries included), else Players. Without a known caster, the struck
// spawn's kind; without either, Players.
SpellFxSource spell_fx_source(std::uint32_t caster, std::uint32_t struck, std::uint32_t me,
                              std::optional<bool> caster_is_npc, std::optional<bool> struck_is_npc);

// Spell groups keep the whole caster and spell IDs. Local emitters use their own namespace.
class EmitterKey {
public:
    constexpr EmitterKey(std::uint32_t local = 0) : value_(local) {}
    static constexpr EmitterKey spell(std::uint32_t caster, std::uint32_t id) {
        EmitterKey key;
        key.value_ = (static_cast<std::uint64_t>(caster) << 32) | id;
        key.spell_ = true;
        return key;
    }
    constexpr std::optional<std::uint32_t> local() const {
        if (spell_) return std::nullopt;
        return static_cast<std::uint32_t>(value_);
    }
    friend constexpr bool operator==(const EmitterKey&, const EmitterKey&) = default;

private:
    std::uint64_t value_ = 0;
    bool spell_ = false;
};

// CreateSpellEmitter's arguments (eqgame's order).
struct SpellEmitterRequest {
    std::int32_t emitter = 0;     // 1: EDD record index
    EmitterKey key;              // 2: lifetime group
    std::int32_t duration_ms = 0; // 3: requested duration
    float density = 1.0f;         // 4
    float opacity = 1.0f;         // 5
    glm::vec3 position{0.0f};     // 6
    std::uint32_t actor = 0;      // 7: attach actor (spawn id), 0 = none
    std::int32_t bone_point = -1; // 9: spell point index (0..9), -1 = none
    float scale = 1.0f;           // 11: 1.0, or the actor's particle scale for attach 12
    // An actor emitter that stays with its actor until stopped (app/actor_emitters.h).
    bool persistent = false;
    // The DLL's actor-emitter creator (CParticleSystem vtable +0x20, 0x10070640) gives the emitter only the point,
    // not the actor (emitter +0x68 stays 0): its axes then follow the point (EDD +0x60), not the actor's turn.
    bool point_only = false;
    // Hailstone: an effect the server plays on a spawn (app/actor_emitters.h start_effect). It belongs to the effect
    // list, not to the actor's own mesh emitters, so rebuilding those (stop_actors) leaves it running.
    bool server_effect = false;
};

// An actor a stage can place emitters on.
struct StageActor {
    std::uint32_t id = 0;
    glm::vec3 position{0.0f};
    std::int32_t level = 1;
    float height = 6.0f;
    // Which of the ten spell points the actor's model has (bit n: point n), and
    // the chest's (point 1's) world position when it has one.
    std::uint16_t points = 0;
    glm::vec3 chest{0.0f};
    // CDisplay::GetPlayerFloorHeight under the spawn (from 0.1 above it): attach 12's z. Without it, the position's.
    std::optional<float> floor;
    // GetParticleScaleFactor (actor vtable +0x218): its model's moddat.ini PESFactor, 1 without one. Attach 12's scale.
    float particle_scale = 1.0f;
    // Whether its model is known. Hailstone: a spawn whose model has not loaded yet (an effect sent as it zones in)
    // has no points to read; the original's actor always has them. Its slots keep the point they ask for, and the
    // attachment finds it once the model is drawn (or keeps to the spawn's place).
    bool model_known = true;
    bool has_point(int n) const { return n >= 0 && n < 16 && (points >> n) & 1u; }
    // has_point, or for a model not known yet any of the ten.
    bool may_have_point(int n) const { return model_known ? has_point(n) : n >= 0 && n < 10; }
};

// SpellEffect_SpawnStageEmitters' slot loop for stage 0 (cast), 1
// (projectile) or 2 (impact).
std::vector<SpellEmitterRequest> spawn_stage_emitters(const assets::SpellEffect& effect, int stage,
                                                      std::uint32_t spell_id, const StageActor* caster,
                                                      const StageActor* target, const glm::vec3& point,
                                                      std::int32_t duration_ms, const SpellParticleSettings& settings,
                                                      bool full = false, const StageActor* missile = nullptr);

// The duration a zone fixture's emitter is created with (add_fixture), ms.
inline constexpr std::int32_t kFixtureDurationMs = 9999000;
// The duration an actor's own emitter is created with (CActor::vf126 @0x1003a950), ms.
inline constexpr std::int32_t kActorEmitterDurationMs = 5000000;

// CEmitterDef::CreateEmitter's duration in seconds.
float emitter_duration_seconds(float edd_duration, std::int32_t requested_ms);

// What CEmitterDef::CreateEmitter @0x1006c920 works out from the definition, the density and the duration (s).
struct EmitterCounts {
    std::int32_t initial = 0;  // emitter +0x18: trunc(+0x80 x density), at least min(16, +0x80)
    std::int32_t per_tick = 0; // +0x1c: +0x84 below 16, else trunc(+0x84 x density)
    float rate = 0.0f;         // +0x20: +0x88, times the density when +0x84 is below 16
    std::int32_t capacity = 0; // +0xc: the particle ring's size, 0..100,000
    float floor_initial = 1.0f; // +0x30: 16 / initial above 16, else 1 (the initial burst's distance floor)
    float floor_tick = 1.0f;    // +0x34: the same of the ring past the initial burst (the ticks' floor)
    std::int32_t length_ms = 0; // the return: how long its particles show, 0 past 600 s
};
EmitterCounts emitter_counts(const assets::EmitterDefinition& def, float density, float duration);
// emitter_counts(def, 1, duration).capacity.
std::uint32_t emitter_capacity(const assets::EmitterDefinition& def, float duration);
// The basis a placed node gives its emitter (columns transverse 1, transverse 2, axial in world), from the node's
// world rotation `owner` (columns: its local axes in world; normalised here), then turned by the definition's
// +0xc4 / +0xc8 (512ths). The emitter constructor (0x1006c5b5) takes (axial, transverse 1, transverse 2) from the
// matrix's rows 2, 0 and -1; each particle's spawn (0x10072fe0, 0x100730ea) turns its copy by +0xc4 about transverse
// 2 (axial' = axial cos + t1 sin, t1' = t1 cos - axial sin), then by +0xc8 about transverse 1 (axial'' = axial'
// cos + t2 sin, t2' = t2 cos - axial' sin).
glm::mat3 emitter_axes(const glm::mat3& owner, float angle1, float angle2);
// Those two turns of a basis already taken (columns transverse 1, transverse 2, axial).
glm::mat3 turn_axes(const glm::mat3& axes, float angle1, float angle2);

// The update's count factor for an emitter `distance` from the camera: +0xa0 / distance past +0xa0, else 1, but not
// below `floor` (EmitterCounts' floor_initial for the initial burst, floor_tick for the ticks).
float emission_distance_factor(float reduce_distance, float floor, float distance);

// The flipbook grid (columns, rows) for a frame count: 1 -> 1x1, 2..4 -> 2x2,
// 5..8 -> 4x2, 9..16 -> 4x4, other -> 1x1.
glm::uvec2 flipbook_grid(std::uint32_t frames);

// Hailstone's generated terrain ribbons: width and height lie along Particle::axes[0] and axes[1].
// Kept outside the original EDD billboard modes so their unknown values still face the camera.
inline constexpr std::uint32_t kWorldBasisBillboard = 0x100;
// Generated Find strands keep their segment tangent in axes[1] and turn their width toward the camera.
inline constexpr std::uint32_t kTrailStrandBillboard = 0x101;

// One particle: the DLL's 0x80-byte ring record (polar about the emitter's axis, plus a world drift) and what the
// update works out for drawing it.
struct Particle {
    // Drawn.
    glm::vec3 position{0.0f}; // world
    float width = 1.0f, height = 1.0f; // drawn once at birth from +0xcc..+0x184
    glm::vec4 color{1.0f};    // rgb, alpha
    float size_factor = 1.0f; // +0x16c x the size scale x grow x shrink
    float ramp_height = 1.0f, ramp_width = 1.0f; // +0x190 / +0x194 ramps
    float rotation = 0.0f;    // 512ths, turning the quad clockwise on screen
    std::uint32_t frame = 0;
    glm::vec3 axis{0.0f, 0.0f, 1.0f}; // its axial direction (billboard 1 streaks along it)
    float length = 1.0f;              // billboard 1's streak: height x the size scale
    bool hidden = false;              // below the axis (radius < 0) without +0x198
    // State.
    glm::vec3 origin{0.0f}; // the emitter's place and offsets at birth
    glm::mat3 axes{1.0f};   // its basis at birth, turned (transverse 1, transverse 2, axial)
    float azimuth = 0.0f, radius = 0.0f, axial = 0.0f; // 512ths, units, units
    glm::vec2 radial_dir{1.0f, 0.0f}; // radial motion's (radius, axial) direction
    float v_axial = 0.0f, v_t1 = 0.0f, v_t2 = 0.0f, v_radial = 0.0f, v_orbit = 0.0f, v_gravity = 0.0f;
    glm::vec3 drift{0.0f}; // world: transverse motion, gravity, wind
    float age = 0.0f, lifetime = 1.0f;
    float rotation0 = 0.0f, spin = 0.0f; // 512ths, 512ths / s
};

struct Emitter {
    std::size_t definition = 0;
    EmitterKey key;
    std::uint32_t actor = 0;
    std::int32_t point = -1; // the actor's spell point it hangs from, or -1
    bool point_only = false;  // SpellEmitterRequest::point_only
    bool server_effect = false; // SpellEmitterRequest::server_effect
    glm::vec3 origin{0.0f};
    // The emitter's basis, unturned (columns transverse 1, transverse 2, axial). Without a matrix the constructor's
    // (+X, +Y, +Z); a zone fixture's from its node (emitter_axes); a spell emitter on an actor takes the actor's
    // every frame.
    glm::mat3 axes{1.0f};
    // A zone fixture or an actor's own emitter: emits for as long as it exists.
    bool persistent = false;
    // Its particles are set from outside (set_still) and drawn as they are: no emission, no ageing.
    bool still = false;
    float scale = 1.0f; // the caller's scale (+0x88)
    float density = 1.0f, opacity = 1.0f;
    float duration = 0.0f;
    double age = 0.0; // s since it was made (a fixture's runs for hours)
    EmitterCounts counts;
    std::int32_t emitted = -1;     // +0x10: ticks emitted, -1 before the initial burst
    // Hailstone (not in the original): its owner is out of view, so it emits nothing and its ticks pass unspent
    // (ParticleSystem::set_held); what it has already emitted lives on.
    bool held = false;
    // Held before its initial burst: the burst plays when it comes into view, and the ticks before it stay unspent.
    bool burst_held = false;
    // The last scales and alpha its attachment gave, kept while held so what it emitted keeps its look.
    float held_size_scale = 1.0f, held_offset_scale = 1.0f, held_alpha = 1.0f;
    std::uint32_t spawn_index = 0; // +0x14
    std::vector<Particle> particles;
};

// Where an attached emitter is this frame (the update's per-emitter block, 0x10072478..0x10072a71).
struct EmitterAttachment {
    // The point's world position and axes (columns, their lengths its scale); nullopt without a point, and then a
    // spell emitter stays where it was made (an actor's CHierarchicalActor answers vtable +0x2f8 false).
    std::optional<glm::vec3> position;
    std::optional<glm::mat3> axes;
    // The actor's world turn (columns; actor vtable +0x2c4): a spell emitter's basis every frame.
    std::optional<glm::mat3> actor_axes;
    // GetParticleScaleFactor (+0x218, moddat.ini PESFactor) and the actor's root GetScaleFactor (+0xa0).
    float particle_scale = 1.0f;
    float actor_scale = 1.0f;
    float alpha = 1.0f; // GetAlpha of the actor or the point's owning actor, sampled every frame
};
using AttachmentFn = std::function<std::optional<EmitterAttachment>(std::uint32_t actor, std::int32_t point)>;

class ParticleSystem {
public:
    ParticleSystem() = default;
    explicit ParticleSystem(std::vector<assets::EmitterDefinition> definitions, std::uint32_t seed = 1);

    bool empty() const { return definitions_.empty(); }
    const std::vector<assets::EmitterDefinition>& definitions() const { return definitions_; }
    const std::vector<Emitter>& emitters() const { return emitters_; }
    // particle_glows (game/light_list.h) of each definition, worked out once when it is added: the zone's lights
    // ask it of every emitter each frame.
    bool glows(std::size_t definition) const { return definition < glows_.size() && glows_[definition]; }

    // CreateSpellEmitter: how long its particles show in ms (EmitterCounts::length_ms), or 0 when none was created
    // (index out of range, density <= 0).
    std::int32_t create(const SpellEmitterRequest& request);
    // A zone fixture's emitter (a WLD particle node of a placed object): at
    // the node's world matrix, along emitter_axes, scaled by the node's scale
    // when the definition's +0x6c is set. It is persistent: the DLL creates
    // it with 9999 s and the actor keeps it while the object stands; here it
    // never runs out. Its duration and capacity are those of a 9999 s request
    // (emitter_duration_seconds), so an EDD definition without a duration of
    // its own (an environment emitter, duration 0) holds a lifetime's worth of
    // particles, not one tick's. Returns false for a definition index out of range.
    bool add_fixture(std::size_t definition, const glm::mat4& node_world, std::uint32_t key = 0);
    // The viewer's position for the distance-scaled emission rate
    // (emission_distance_factor); nullopt (the default) emits at full rate.
    void set_viewer(std::optional<glm::vec3> viewer) { viewer_ = viewer; }
    // Stops every emitter with this key (effect manager vtable +0x38).
    void stop(EmitterKey key);
    // Remove a particular owner's attachment without affecting its other spells (e.g. a despawned blood point).
    void stop_attachment(std::uint32_t actor, std::int32_t point);
    void stop_actor(std::uint32_t actor);
    // stop and stop_actor for many at once, in one pass over the emitters: `keys` / `actors` sorted ascending.
    void stop_keys(std::span<const std::uint32_t> keys);
    void stop_actors(std::span<const std::uint32_t> actors);
    // Each attached emitter's Emitter::held, from its actor (`held(actor)`); the others are never held.
    void set_held(const std::function<bool(std::uint32_t actor)>& held);
    // The density of the persistent emitters (zone fixtures, actors' own): what they emit from now on scales with
    // it (the effect manager's density setters, +0xa4 environment / +0xac actor).
    void set_persistent_density(float density);
    // Moves the emitters with this key (the effect's position, vtable +0x14).
    void move(EmitterKey key, const glm::vec3& origin);
    bool has(EmitterKey key) const;
    // A definition of the caller's (a texture to draw still particles with); returns its index.
    std::size_t add_definition(assets::EmitterDefinition d);
    // The particles of the still emitter `key` (of `definition`), drawn as given until set again or stopped.
    void set_still(std::uint32_t key, std::size_t definition, std::vector<Particle> particles);
    // The same, copied into the emitter's own list (its storage kept from the last call: a still set every frame
    // allocates nothing once it has grown).
    void set_still(std::uint32_t key, std::size_t definition, std::span<const Particle> particles);
    void clear() { emitters_.clear(); }
    // Advances by dt seconds; `attachment` places the attached emitters (EmitterAttachment).
    void update(float dt, const AttachmentFn& attachment);

private:
    void spawn(Emitter& e, std::int32_t count, float offset_scale);
    float random(float a, float b);

    // What update's serial pass works out for an emitter's particles to move by (its scales, owner alpha and the
    // place and basis followers take), read by the parallel pass. Kept so its memory is reused frame to frame.
    struct EmitterStep {
        float size_scale = 1.0f, actor_alpha = 1.0f;
        glm::vec3 follow_origin{0.0f};
        glm::mat3 follow_axes{1.0f};
    };
    static constexpr std::size_t kStepGrain = 4; // emitters a worker takes at a time in update's parallel passes

    std::vector<assets::EmitterDefinition> definitions_;
    std::vector<bool> glows_; // per definition (glows)
    std::vector<Emitter> emitters_;
    std::vector<EmitterStep> steps_;
    std::mt19937 rng_{1};
    std::optional<glm::vec3> viewer_;
};

} // namespace hailstone::game
