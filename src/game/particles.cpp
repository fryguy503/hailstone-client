#include "game/particles.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/worker_pool.h"
#include "game/light_list.h"
#include "game/sine_table.h"

namespace hailstone::game {

namespace {

constexpr float kTurn = 2.0f * std::numbers::pi_v<float>;

} // namespace

SpellParticleSettings spell_particle_settings(std::optional<float> density, std::optional<float> opacity) {
    SpellParticleSettings s;
    s.density = std::clamp(density.value_or(1.0f), 0.0f, 1.0f);
    s.opacity = std::clamp(opacity.value_or(1.0f), 0.0f, 1.0f);
    return s;
}

SpellFxSource spell_fx_source(std::uint32_t caster, std::uint32_t struck, std::uint32_t me,
                              std::optional<bool> caster_is_npc, std::optional<bool> struck_is_npc) {
    if (me != 0 && (caster == me || struck == me)) return SpellFxSource::Self;
    std::optional<bool> npc = caster_is_npc ? caster_is_npc : struck_is_npc;
    return npc.value_or(false) ? SpellFxSource::Npcs : SpellFxSource::Players;
}

std::vector<SpellEmitterRequest> spawn_stage_emitters(const assets::SpellEffect& effect, int stage,
                                                      std::uint32_t spell_id, const StageActor* caster,
                                                      const StageActor* target, const glm::vec3& point,
                                                      std::int32_t duration_ms, const SpellParticleSettings& settings,
                                                      bool full, const StageActor* missile) {
    std::vector<SpellEmitterRequest> out;
    if (stage < 0 || stage > 2) return out;
    const std::int32_t level = caster ? caster->level : 1;
    for (const auto& slot : effect.stages[static_cast<std::size_t>(stage)].slots) {
        if (slot.emitter < 1 || level < slot.min_level) continue;
        const StageActor* at = nullptr;
        SpellEmitterRequest r;
        r.emitter = slot.emitter;
        r.key = EmitterKey::spell(caster ? caster->id : 0u, spell_id);
        r.duration_ms = duration_ms;
        r.density = full ? 1.0f : settings.density;
        r.opacity = full ? 1.0f : settings.opacity;
        switch (slot.location) {
        case 1: at = caster; break;
        case 2: at = missile; break;
        case 3: at = target; break;
        default: break;
        }
        if ((slot.location == 1 || slot.location == 3) && !at) continue;
        r.position = at ? at->position : point;
        if (at && slot.attach != 0) {
            // 1 -> point 0; 2..7 -> points 1..6, else point 0; 8 -> no point; 9 / 10 -> points 7 / 8 or the slot
            // is skipped; 11 -> point 9; 12 -> no point, at the chest's x and y (z: GetPlayerFloorHeight 0.1 above
            // the actor, see below), scaled by the actor's particle scale. A missing point leaves the actor without one.
            auto existing = [&](int n) { return at->may_have_point(n) ? n : -1; };
            if (slot.attach == 1) r.bone_point = existing(0);
            else if (slot.attach >= 2 && slot.attach <= 7) {
                r.bone_point = existing(slot.attach - 1);
                if (r.bone_point < 0) r.bone_point = existing(0);
            } else if (slot.attach == 9 || slot.attach == 10) {
                if (!at->may_have_point(slot.attach - 2)) continue;
                r.bone_point = slot.attach - 2;
            } else if (slot.attach == 11) r.bone_point = existing(9);
            else if (slot.attach == 12) {
                // The chest's x and y, z = GetPlayerFloorHeight(x, y, z + 0.1) (FUN_00507a40), scaled by the
                // actor's GetParticleScaleFactor (or its ActorAnimation's +0x38: the same moddat.ini PESFactor).
                if (at->has_point(1)) r.position = {at->chest.x, at->chest.y, r.position.z};
                if (at->floor) r.position.z = *at->floor;
                r.scale = at->particle_scale;
            } else if (slot.attach != 8) continue; // other ids: no case in the switch, the slot is skipped
            r.actor = at->id;
        }
        out.push_back(r);
    }
    return out;
}

float emitter_duration_seconds(float edd_duration, std::int32_t requested_ms) {
    if (edd_duration < 0.0f) return -edd_duration;
    if (requested_ms > 0) return static_cast<float>(requested_ms) * 0.001f;
    return edd_duration;
}

namespace {

// _ftol: toward zero, saturated here rather than undefined past the int range.
std::int32_t trunc_int(double v) {
    if (!(v == v)) return 0;
    return static_cast<std::int32_t>(std::clamp(v, -2147483648.0, 2147483647.0));
}

} // namespace

EmitterCounts emitter_counts(const assets::EmitterDefinition& d, float density, float duration) {
    // CEmitterDef::CreateEmitter @0x1006c920 (0x1006c987..0x1006cb01, 0x1006cbfd..0x1006cc19).
    EmitterCounts c;
    const auto init = static_cast<std::int32_t>(d.initial_count), tick = static_cast<std::int32_t>(d.per_tick);
    c.initial = trunc_int(static_cast<double>(init) * density);
    if (c.initial < 16) c.initial = std::min(16, init);
    if (tick >= 16) {
        c.per_tick = trunc_int(static_cast<double>(tick) * density);
        c.rate = d.ticks_per_second;
    } else {
        // Few per tick: the emitter ticks less often rather than emitting fewer each time.
        c.per_tick = tick;
        c.rate = d.ticks_per_second * density;
    }
    const float emitting = duration - d.delay, life = d.lifetime;
    std::int64_t cap = 0;
    if (emitting >= 0.0f) {
        if (d.capacity != 0) cap = d.capacity;
        else if (life >= 0.0f && emitting > life) {
            const std::int64_t n = trunc_int(static_cast<double>(life) * c.rate);
            cap = c.initial > c.per_tick ? c.initial + n * c.per_tick : (n + 1) * c.per_tick;
        } else
            cap = c.initial + static_cast<std::int64_t>(trunc_int(static_cast<double>(emitting) * c.rate)) * c.per_tick;
    }
    cap = std::max<std::int64_t>(cap, static_cast<std::int64_t>(d.capacity));
    c.capacity = static_cast<std::int32_t>(std::clamp<std::int64_t>(cap, 0, 100000));
    // The constructor's batch floors (0x1006c687).
    c.floor_initial = c.initial > 16 ? 16.0f / static_cast<float>(c.initial) : 1.0f;
    const std::int32_t rest = c.initial > c.per_tick ? c.capacity - c.initial + c.per_tick : c.capacity;
    c.floor_tick = rest > 16 ? 16.0f / static_cast<float>(rest) : 1.0f;
    double t = 0.0;
    if (c.capacity > 0) {
        if (life < 0.0f) t = duration;
        else if (c.per_tick > 0 && c.rate > 0.0f)
            t = static_cast<double>(trunc_int(static_cast<double>(emitting) * c.rate)) / c.rate + d.delay + life;
        else if (c.initial > 0) t = static_cast<double>(life) + d.delay;
    }
    if (t > 600.0) t = 0.0;
    c.length_ms = trunc_int(t * 1000.0);
    return c;
}

std::uint32_t emitter_capacity(const assets::EmitterDefinition& d, float duration) {
    return static_cast<std::uint32_t>(emitter_counts(d, 1.0f, duration).capacity);
}

glm::mat3 turn_axes(const glm::mat3& axes, float angle1, float angle2) {
    glm::vec3 t1 = axes[0], t2 = axes[1], ax = axes[2];
    if (angle1 != 0.0f) {
        // +0xc4 (0x10072fe0): about transverse 2.
        const float c = cmath_cos_interp(angle1), sn = cmath_sin_interp(angle1);
        const glm::vec3 a = ax * c + t1 * sn;
        t1 = t1 * c - ax * sn;
        ax = a;
    }
    if (angle2 != 0.0f) {
        // +0xc8 (0x100730ea): about transverse 1, on the turned axis.
        const float c = cmath_cos_interp(angle2), sn = cmath_sin_interp(angle2);
        const glm::vec3 a = ax * c + t2 * sn;
        t2 = t2 * c - ax * sn;
        ax = a;
    }
    return glm::mat3(t1, t2, ax);
}

glm::mat3 emitter_axes(const glm::mat3& owner, float angle1, float angle2) {
    auto unit = [](glm::vec3 v, glm::vec3 fallback) {
        float len = glm::length(v);
        return std::isfinite(len) && len > 1e-6f ? v / len : fallback;
    };
    // The constructor with a matrix (0x1006c5b5): axial = row 2, transverse 1 = row 0, transverse 2 = -row 1.
    glm::vec3 r0 = unit(owner[0], {1, 0, 0}), r1 = unit(owner[1], {0, 1, 0}), r2 = unit(owner[2], {0, 0, 1});
    return turn_axes(glm::mat3(r0, -r1, r2), angle1, angle2);
}

float emission_distance_factor(float reduce_distance, float floor, float distance) {
    const float f = distance > reduce_distance && distance > 0.0f ? reduce_distance / distance : 1.0f;
    return std::max(f, floor);
}

glm::uvec2 flipbook_grid(std::uint32_t frames) {
    if (frames >= 2 && frames <= 4) return {2, 2};
    if (frames >= 5 && frames <= 8) return {4, 2};
    if (frames >= 9 && frames <= 16) return {4, 4};
    return {1, 1};
}

ParticleSystem::ParticleSystem(std::vector<assets::EmitterDefinition> definitions, std::uint32_t seed)
    : definitions_(std::move(definitions)), rng_(seed) {
    glows_.reserve(definitions_.size());
    for (const auto& d : definitions_) glows_.push_back(particle_glows(d));
}

assets::EmitterDefinition blood_spurt_definition(std::string texture) {
    assets::EmitterDefinition d;
    d.name = "BloodSpurt";
    d.texture = std::move(texture);
    d.bone_orientation = 1;
    d.no_zwrite = 1;
    d.duration = -0.5f;
    d.lifetime = 0.5f;
    d.per_tick = 1;
    d.ticks_per_second = 200.0f;
    d.capacity = 100;
    d.shape = 5;
    d.motion[3] = {1.25f, 5.0f, 0.0f};
    d.gravity = 1.0f;
    d.scale1 = 0.375f;
    d.scale2 = d.max_opacity = 0.5f;
    d.frames = 1;
    d.frames_per_second = 10.0f;
    return d;
}

float ParticleSystem::random(float a, float b) {
    return a + (b - a) * std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_);
}

std::int32_t ParticleSystem::create(const SpellEmitterRequest& r) {
    if (r.emitter < 1 || static_cast<std::size_t>(r.emitter) >= definitions_.size()) return 0; // out of range
    if (r.density <= 0.0f) return 0;
    const auto& d = definitions_[static_cast<std::size_t>(r.emitter)];
    Emitter e;
    e.definition = static_cast<std::size_t>(r.emitter);
    e.key = r.key;
    e.actor = r.actor;
    e.point = r.actor != 0 ? r.bone_point : -1;
    e.point_only = r.point_only;
    e.server_effect = r.server_effect;
    e.origin = r.position;
    e.scale = r.scale;
    e.density = r.density;
    e.opacity = r.opacity;
    e.duration = emitter_duration_seconds(d.duration, r.duration_ms);
    e.counts = emitter_counts(d, r.density, e.duration);
    e.persistent = r.persistent;
    emitters_.push_back(std::move(e));
    return emitters_.back().counts.length_ms;
}

bool ParticleSystem::add_fixture(std::size_t definition, const glm::mat4& node, std::uint32_t key) {
    if (definition < 1 || definition >= definitions_.size()) return false;
    const auto& d = definitions_[definition];
    Emitter e;
    e.definition = definition;
    e.key = key;
    e.origin = glm::vec3(node[3]);
    glm::mat3 rot(node);
    e.axes = emitter_axes(rot, 0.0f, 0.0f);
    // The owner scale getter: the first row's length.
    float s = glm::length(rot[0]);
    e.scale = std::isfinite(s) && s > 0.0f ? s : 1.0f;
    e.duration = emitter_duration_seconds(d.duration, kFixtureDurationMs);
    e.counts = emitter_counts(d, 1.0f, e.duration);
    e.persistent = true;
    emitters_.push_back(std::move(e));
    return true;
}

void ParticleSystem::set_persistent_density(float density) {
    for (auto& e : emitters_)
        if (e.persistent && !e.still && e.density != density) {
            // The DLL's density acts when an emitter is made; one made again keeps its particles.
            e.density = density;
            e.counts = emitter_counts(definitions_[e.definition], density, e.duration);
            if (e.particles.size() > static_cast<std::size_t>(e.counts.capacity))
                e.particles.resize(static_cast<std::size_t>(e.counts.capacity));
        }
}

void ParticleSystem::stop(EmitterKey key) {
    std::erase_if(emitters_, [&](const Emitter& e) { return e.key == key; });
}

void ParticleSystem::stop_attachment(std::uint32_t actor, std::int32_t point) {
    std::erase_if(emitters_, [&](const Emitter& e) { return e.actor == actor && e.point == point; });
}

void ParticleSystem::stop_actor(std::uint32_t actor) {
    std::erase_if(emitters_, [&](const Emitter& e) { return e.actor == actor; });
}

void ParticleSystem::stop_keys(std::span<const std::uint32_t> keys) {
    if (keys.empty()) return;
    std::erase_if(emitters_, [&](const Emitter& e) {
        auto key = e.key.local();
        return key && std::binary_search(keys.begin(), keys.end(), *key);
    });
}

void ParticleSystem::stop_actors(std::span<const std::uint32_t> actors) {
    if (actors.empty()) return;
    std::erase_if(emitters_, [&](const Emitter& e) {
        return e.actor != 0 && !e.server_effect && std::binary_search(actors.begin(), actors.end(), e.actor);
    });
}

void ParticleSystem::set_held(const std::function<bool(std::uint32_t actor)>& held) {
    for (auto& e : emitters_) e.held = e.actor != 0 && held && held(e.actor);
}

void ParticleSystem::move(EmitterKey key, const glm::vec3& origin) {
    for (auto& e : emitters_)
        if (e.key == key) e.origin = origin;
}

bool ParticleSystem::has(EmitterKey key) const {
    return std::any_of(emitters_.begin(), emitters_.end(), [&](const Emitter& e) { return e.key == key; });
}

std::size_t ParticleSystem::add_definition(assets::EmitterDefinition d) {
    glows_.push_back(particle_glows(d));
    definitions_.push_back(std::move(d));
    return definitions_.size() - 1;
}

void ParticleSystem::set_still(std::uint32_t key, std::size_t definition, std::vector<Particle> particles) {
    if (definition >= definitions_.size()) return;
    for (auto& e : emitters_)
        if (e.key == key && e.still) {
            e.definition = definition;
            e.particles = std::move(particles);
            return;
        }
    Emitter e;
    e.definition = definition;
    e.key = key;
    e.still = true;
    e.persistent = true;
    e.particles = std::move(particles);
    emitters_.push_back(std::move(e));
}

void ParticleSystem::set_still(std::uint32_t key, std::size_t definition, std::span<const Particle> particles) {
    if (definition >= definitions_.size()) return;
    for (auto& e : emitters_)
        if (e.key == key && e.still) {
            e.definition = definition;
            e.particles.assign(particles.begin(), particles.end());
            return;
        }
    set_still(key, definition, std::vector<Particle>(particles.begin(), particles.end()));
}

namespace {

// The ring layout of shape 2 for a batch of n (0x100727e0..): rings, particles per ring.
std::pair<std::int32_t, std::int32_t> ring_layout(std::int32_t n) {
    if (n < 9) return {8, 16};
    static constexpr std::pair<std::int32_t, std::int32_t> kRings[] = {
        {18, 3}, {32, 4}, {72, 6}, {128, 8}, {200, 10}, {288, 12}, {512, 16}, {1152, 24}, {3200, 40}, {7200, 60}};
    std::int32_t rings = 100;
    for (auto [most, r] : kRings)
        if (n <= most) {
            rings = r;
            break;
        }
    return {rings, std::max(n / rings, 1)};
}

// atan2 in 512ths of a turn, 0..512.
float azimuth512(float y, float x) {
    float a = std::atan2(y, x) * 512.0f / kTurn;
    return a < 0.0f ? a + 512.0f : a;
}

} // namespace

void ParticleSystem::spawn(Emitter& e, std::int32_t count, float offset_scale) {
    const auto& d = definitions_[e.definition];
    // The ring's size (+0xc): no recycling; what does not fit is dropped, the running index still counting.
    const float d0 = d.shape_size.x, d1 = d.shape_size.y != 0.0f ? d.shape_size.y : d.shape_size.x;
    const float d2 = d.shape_size.z;
    const auto rings = ring_layout(count);
    // The emitter's place and offsets (0x10072f14: +0xb8 along the axis, +0xbc transverse 1, +0xc0 transverse 2),
    // before the turns, then the basis each particle keeps.
    const glm::vec3 origin = e.origin + offset_scale * (d.offset.x * e.axes[2] + d.offset.y * e.axes[0] +
                                                        d.offset.z * e.axes[1]);
    const glm::mat3 axes = turn_axes(e.axes, d.angle1, d.angle2);
    for (std::int32_t i = 0; i < count; ++i) {
        if (e.particles.size() >= static_cast<std::size_t>(std::max(e.counts.capacity, 0))) {
            e.spawn_index += static_cast<std::uint32_t>(count - i); // the rest dropped, counted all the same
            break;
        }
        const std::uint32_t k = e.spawn_index++;
        Particle p;
        float az = random(0.0f, 512.0f), r = 0.0f, z = 0.0f;
        bool along_normal = false;
        // A shape in the transverse plane as (x, y), turned into the polar record (elliptic sizes: atan2 of it).
        auto planar = [&](float x, float y) {
            r = std::sqrt(x * x + y * y);
            if (x != 0.0f || y != 0.0f) az = azimuth512(y, x);
        };
        switch (d.shape) {
        case 0: az = 0.0f; break; // a point sprays along transverse 1
        case 1: { // ring, evenly spaced
            const float a = 512.0f * static_cast<float>(k % static_cast<std::uint32_t>(std::max(count, 1))) /
                            static_cast<float>(std::max(count, 1));
            planar(d0 * cmath_cos_interp(a), d1 * cmath_sin_interp(a));
            if (d0 == d1) az = a;
            break;
        }
        case 2: { // stacked rings: a cylinder of `rings` rings
            const auto [n_rings, per_ring] = rings;
            az = 512.0f / static_cast<float>(per_ring) * static_cast<float>(k);
            r = d0;
            z = d2 / static_cast<float>(n_rings) *
                    static_cast<float>((k / static_cast<std::uint32_t>(per_ring)) % static_cast<std::uint32_t>(n_rings)) -
                d2 * 0.5f;
            break;
        }
        case 3: planar(d0 * cmath_cos_interp(az), d1 * cmath_sin_interp(az)), z = (random(0.0f, 1.0f) - 0.5f) * d2; break;
        case 4: { // disk interior, rejection sampled
            float x, y;
            do {
                x = random(-1.0f, 1.0f);
                y = random(-1.0f, 1.0f);
            } while (x * x + y * y > 1.0f);
            planar(x * d0, y * d1);
            break;
        }
        case 5: { // sphere surface
            const float u = random(-1.0f, 1.0f);
            r = d0 * std::sqrt(std::max(0.0f, 1.0f - u * u));
            z = (d2 != 0.0f ? d2 : d0) * u;
            along_normal = true;
            break;
        }
        case 6: { // box surface
            // EQGraphicsDX9.dll's shape switch, case 6 (@0x100737be): two draws from the signed table (-1..1) for the
            // two free axes, in that order, then the face from the unsigned one (0..1) times 6.0 stored as an integer
            // (rounded to nearest, so 0..6: faces 0 and 6 are half as likely). Faces 0..4 are -x, +x, -y, +y, -z;
            // anything else +z.
            glm::vec3 dim(d0, d1, d2 != 0.0f ? d2 : d0);
            const float r1 = random(-1.0f, 1.0f);
            const float r2 = random(-1.0f, 1.0f);
            const int face = static_cast<int>(std::nearbyint(random(0.0f, 1.0f) * 6.0f));
            glm::vec3 v;
            switch (face) {
            case 0: v = {-1.0f, r1, r2}; break;
            case 1: v = {1.0f, r1, r2}; break;
            case 2: v = {r1, -1.0f, r2}; break;
            case 3: v = {r1, 1.0f, r2}; break;
            case 4: v = {r1, r2, -1.0f}; break;
            default: v = {r1, r2, 1.0f}; break;
            }
            v *= dim;
            planar(v.x, v.y);
            z = v.z;
            along_normal = true;
            break;
        }
        case 7: { // cone side
            const float u = random(0.0f, 1.0f);
            r = d0 * u;
            z = d2 * u;
            break;
        }
        case 8: { // torus surface
            const float phi = random(0.0f, 512.0f);
            r = d0 + d2 * cmath_cos_interp(phi);
            z = d2 * cmath_sin_interp(phi);
            break;
        }
        case 9: r = d0; break;
        default: break;
        }
        // Shapes 5 and 6 move radially along their surface's normal (0x10073307), the others out from the axis.
        if (along_normal) {
            const float len = std::sqrt(r * r + z * z);
            p.radial_dir = len > 0.0f ? glm::vec2(r / len, z / len) : glm::vec2(1.0f, 0.0f);
        }
        p.azimuth = az;
        p.radius = r * offset_scale;
        p.axial = z * offset_scale;
        p.origin = origin;
        p.axes = axes;
        p.v_axial = random(d.motion[0].x, d.motion[0].y);
        p.v_t1 = random(d.motion[1].x, d.motion[1].y);
        p.v_t2 = random(d.motion[2].x, d.motion[2].y);
        p.v_radial = random(d.motion[3].x, d.motion[3].y);
        p.v_orbit = random(d.motion[4].x, d.motion[4].y);
        // +0x7c below 0: the emitter's time left (its particles end with it); 0: one frame.
        if (d.lifetime > 0.0f) p.lifetime = d.lifetime;
        else if (d.lifetime == 0.0f) p.lifetime = 0.0f;
        else p.lifetime = e.persistent ? e.duration : static_cast<float>(std::max(e.duration - e.age, 0.0));
        const float hr = random(0.0f, 1.0f);
        p.height = d.height_start + (d.height_end - d.height_start) * hr;
        const float wr = d.same_random_size ? hr : random(0.0f, 1.0f);
        p.width = d.width + (d.width_end - d.width) * wr;
        // rand() / 32767 x 1000: 0..1000 512ths (the start angle shows only while it spins).
        p.rotation0 = d.random_rotation ? random(0.0f, 1000.0f) : 0.0f;
        p.spin = random(d.spin, d.spin2);
        e.particles.push_back(p);
    }
}

void ParticleSystem::update(float dt, const AttachmentFn& attachment) {
    // Three passes over the emitters, each emitter's work in the order the DLL's update does it. The particles' ends
    // and their motion touch only their own emitter, so they are split over the worker threads
    // (core::parallel_for_nested: the actor effects step on a worker); the attachments (the callers' own, which need
    // not be thread-safe) and the emission (one random sequence for the whole system) stay on the calling thread, in
    // the emitters' order, so a seed draws the same particles however the work is split.
    // Particles whose time is up leave the ring's head first (0x10072a71): one drawn last frame at its end.
    core::parallel_for_nested(emitters_.size(), kStepGrain, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            Emitter& e = emitters_[i];
            if (e.still) continue;
            std::erase_if(e.particles, [](const Particle& p) { return p.lifetime - p.age <= 0.0f; });
        }
    });
    steps_.resize(emitters_.size());
    for (std::size_t i = 0; i < emitters_.size(); ++i) {
        Emitter& e = emitters_[i];
        if (e.still) continue;
        const auto& d = definitions_[e.definition];
        // The scales (0x10072478): the size and motion scale with +0x6c, the offsets and spawn shape with +0x19c,
        // each the caller's scale (+0x88), or with an actor its root's GetScaleFactor x its GetParticleScaleFactor,
        // or with a point the point matrix's row length x the point's actor's GetParticleScaleFactor.
        float size_scale = d.actor_scales_size == 1 && e.actor == 0 ? e.scale : 1.0f;
        float offset_scale = d.actor_scales_offsets == 1 ? e.scale : 1.0f;
        float actor_alpha = 1.0f;
        if (e.held) {
            // Hailstone: out of view its pose is not asked for; what it emitted keeps the scales and alpha it had.
            size_scale = e.held_size_scale;
            offset_scale = e.held_offset_scale;
            actor_alpha = e.held_alpha;
        } else if (e.actor != 0 && attachment)
            if (auto at = attachment(e.actor, e.point)) {
                actor_alpha = std::isfinite(at->alpha) ? std::clamp(at->alpha, 0.0f, 1.0f) : 0.0f;
                if (!e.point_only) {
                    const float actor = at->actor_scale * at->particle_scale;
                    if (d.actor_scales_size == 1) size_scale = actor;
                    if (d.actor_scales_offsets == 1) offset_scale = actor;
                }
                if (e.point >= 0 && at->position) {
                    e.origin = *at->position;
                    if (at->axes) {
                        const float row = glm::length((*at->axes)[0]) * at->particle_scale;
                        if (d.actor_scales_size == 1) size_scale = row;
                        if (d.actor_scales_offsets == 1) offset_scale = row;
                    }
                }
                // The basis: a spell emitter's from its actor every frame, whatever +0x60 says (axial = row 2,
                // transverse 1 = row 0, transverse 2 = -row 1); an actor's own emitter's from its point with +0x60.
                if (!e.point_only && at->actor_axes) e.axes = emitter_axes(*at->actor_axes, 0.0f, 0.0f);
                else if (e.point_only && d.bone_orientation == 1 && at->axes) e.axes = emitter_axes(*at->axes, 0.0f, 0.0f);
            }
        if (!e.held) {
            e.held_size_scale = size_scale;
            e.held_offset_scale = offset_scale;
            e.held_alpha = actor_alpha;
        }
        // Emission (0x10072aa7..0x10072c47): while time is left, the initial burst once its delay has passed, then
        // the ticks since, each batch scaled by the distance factor (at least the batch floors). The counts are the
        // definition's own: the density set the rate and the ring's size.
        const double left = e.duration - e.age;
        if (e.persistent || left > 0.0) {
            const double elapsed = e.age - d.delay;
            if (elapsed >= 0.0 && e.held) {
                // Hailstone: out of view it emits nothing, and the ticks pass unspent, so it does not make them all
                // up in one burst at one place when it comes back into view. One held before its initial burst
                // keeps it for when it first comes into view.
                if (e.emitted < 0) e.burst_held = true;
                else e.emitted = std::max(e.emitted, trunc_int(elapsed * e.counts.rate));
            } else if (elapsed >= 0.0) {
                const std::int32_t target = trunc_int(elapsed * e.counts.rate);
                const float distance = viewer_ ? glm::distance(*viewer_, e.origin) : 0.0f;
                if (e.emitted < 0) {
                    const float f = emission_distance_factor(d.reduce_distance, e.counts.floor_initial, distance);
                    spawn(e, trunc_int(static_cast<double>(d.initial_count) * f), offset_scale);
                    e.emitted = e.burst_held ? target : 0;
                    e.burst_held = false;
                }
                if (e.emitted < target) {
                    const float f = emission_distance_factor(d.reduce_distance, e.counts.floor_tick, distance);
                    const std::int32_t batch = trunc_int(static_cast<double>(d.per_tick) * (target - e.emitted));
                    const std::int32_t n = trunc_int(static_cast<double>(batch) * f);
                    if (n > 0) {
                        spawn(e, n, offset_scale);
                        e.emitted = target;
                    }
                }
            }
        }
        EmitterStep& frame = steps_[i];
        frame.size_scale = size_scale;
        frame.actor_alpha = actor_alpha;
        frame.follow_origin = e.origin + offset_scale * (d.offset.x * e.axes[2] + d.offset.y * e.axes[0] +
                                                         d.offset.z * e.axes[1]);
        frame.follow_axes = turn_axes(e.axes, d.angle1, d.angle2);
    }
    core::parallel_for_nested(emitters_.size(), kStepGrain, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            Emitter& e = emitters_[i];
            if (e.still) continue;
            const auto& d = definitions_[e.definition];
            const EmitterStep& frame = steps_[i];
            const float size_scale = frame.size_scale, actor_alpha = frame.actor_alpha;
            const glm::vec3& follow_origin = frame.follow_origin;
            const glm::mat3& follow_axes = frame.follow_axes;
            const bool same_color = d.color_start == d.color_end;
            for (auto& p : e.particles) {
                // Motion (0x10074740..): each velocity takes its acceleration, then moves the particle; the axial,
                // transverse and radial steps and gravity's and the wind's are scaled by the size scale, the orbit not.
                // Transverse motion and gravity drift in the world; the radius is not held at the axis.
                const glm::mat3& axes = d.follow_attachment ? follow_axes : p.axes;
                const float step = dt * size_scale;
                p.v_axial += d.motion[0].z * dt;
                p.axial += p.v_axial * step;
                p.v_t1 += d.motion[1].z * dt;
                p.drift += axes[0] * (p.v_t1 * step);
                p.v_t2 += d.motion[2].z * dt;
                p.drift += axes[1] * (p.v_t2 * step);
                p.v_radial += d.motion[3].z * dt;
                p.radius += p.radial_dir.x * p.v_radial * step;
                p.axial += p.radial_dir.y * p.v_radial * step;
                p.v_orbit += d.motion[4].z * dt;
                p.azimuth += p.v_orbit * dt;
                p.v_gravity += d.gravity * dt;
                p.drift.z -= p.v_gravity * step;
                p.drift.x += d.wind_x * step; // a steady wind, not an acceleration
                p.age += dt;
                const glm::vec3& base = d.follow_attachment ? follow_origin : p.origin;
                p.position = base + p.drift + axes[2] * p.axial +
                             axes[0] * (p.radius * cmath_cos(p.azimuth)) + axes[1] * (p.radius * cmath_sin(p.azimuth));
                p.hidden = d.negative_radial != 1 && p.radius < 0.0f;
                p.axis = axes[2];
                p.length = p.height * size_scale;
                // Ramps (0x10074aff..): opacity min(fade in x fade out, max) x the emitter's; size grow x shrink; the
                // height and width ramps +0x190 / +0x194 near the end.
                const float rem = p.lifetime - p.age;
                auto ramp_in = [&](float t) { return t > 0.0f && p.age < t ? p.age / t : 1.0f; };
                auto ramp_out = [&](float t) { return t > 0.0f && rem < t ? std::max(rem, 0.0f) / t : 1.0f; };
                const float alpha = std::min(ramp_in(d.fade_in) * ramp_out(d.fade_out), d.max_opacity) * e.opacity * actor_alpha;
                p.size_factor = d.scale1 * size_scale * ramp_in(d.grow) * ramp_out(d.shrink);
                p.ramp_height = ramp_out(d.shrink_x);
                p.ramp_width = ramp_out(d.shrink_y);
                // The colour: each channel start + round((end - start) x age / life).
                glm::vec3 rgb = glm::vec3(d.color_start);
                if (!same_color && p.lifetime > 0.0f) {
                    const float t = std::clamp(p.age / p.lifetime, 0.0f, 1.0f);
                    for (int c = 0; c < 3; ++c)
                        rgb[c] = static_cast<float>(d.color_start[c]) +
                                 std::round((static_cast<float>(d.color_end[c]) - static_cast<float>(d.color_start[c])) * t);
                }
                p.color = glm::vec4(rgb / 255.0f, std::clamp(alpha, 0.0f, 1.0f));
                // Spinning: age x spin + the start angle; one that does not spin is not turned (+0x178's orientation is
                // not traced).
                p.rotation = p.spin != 0.0f ? p.age * p.spin + p.rotation0 : 0.0f;
                if (d.frames > 1) {
                    // round(+0x134 x age) mod +0x130 (FUN_10005ed0 rounds to nearest).
                    auto grid = flipbook_grid(d.frames);
                    const std::int64_t n = std::min<std::int64_t>(d.frames, grid.x * grid.y);
                    const auto f = static_cast<std::int64_t>(std::nearbyint(p.age * d.frames_per_second));
                    p.frame = static_cast<std::uint32_t>(((f % n) + n) % n);
                }
            }
            e.age += dt;
        }
    });
    // An emitter whose time is up goes when its ring is empty.
    std::erase_if(emitters_, [&](const Emitter& e) {
        return !e.persistent && !e.still && e.age >= e.duration && e.particles.empty();
    });
}

} // namespace hailstone::game
