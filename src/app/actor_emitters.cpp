#include "app/actor_emitters.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>

#include "assets/character.h"
#include "assets/model_points.h"
#include "assets/object_animation.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "core/worker_pool.h"
#include "game/mechanics/anim_select.h"

namespace hailstone::app {

namespace {
// eqgame's animation table (0xaae3d8): the standing and idle animations.
constexpr std::int32_t kStnd = 31, kIdle = 32;

// The clip an entry's animation (a row of that table) plays: the row's old-style code.
std::string_view triggered_clip(std::int32_t animation) {
    if (animation < 1 || animation > 0xb6 - 0x4f) return {};
    namespace m = game::mechanics;
    return m::old_anim_clip(m::new_to_old_anim(static_cast<std::uint8_t>(0x4f + animation)));
}
} // namespace

ActorEmitters::ActorEmitters(std::vector<assets::EmitterDefinition> definitions, game::HeightTables heights,
                             bool async)
    : particles_(std::move(definitions)), heights_(std::move(heights)), async_(async) {}

ActorEmitters::ActorEmitters(render::VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir,
                             bool scene)
    : vk_(&vk), game_(&game), shader_dir_(shader_dir), scene_(scene), async_(scene) {
    try {
        if (auto data = game.read("actoremittersnew.edd")) particles_ = game::ParticleSystem(assets::parse_edd(*data));
    } catch (const std::exception& e) {
        log::warn("actor emitters: {}", e.what());
    }
    IniFile ini;
    if (auto path = game.find("eqclient.ini")) ini.load(*path);
    auto value = [&](std::string_view key) -> std::optional<float> {
        auto v = ini.get("Defaults", key);
        if (!v || v->empty()) return std::nullopt;
        return std::strtof(v->c_str(), nullptr);
    };
    settings_ = game::spell_particle_settings(value("ActorParticleDensity"), value("ActorParticleOpacity"));
    eqclient_density_ = settings_.density;
    if (auto moddat = game.read("Resources/moddat.ini"))
        heights_ = game::parse_height_tables({}, std::string_view(reinterpret_cast<const char*>(moddat->data()), moddat->size()));
    // The scene's renderer is made now, so the first emitter in the world costs no hitch; an item preview's only when
    // its item has an emitter: each renderer maps its own vertex ring (about 17 MB), and most items have none.
    if (!particles_.empty() && scene) renderer_ = std::make_unique<render::ParticleRenderer>(vk, game, shader_dir, scene);
}

void ActorEmitters::set_texture_reader(render::ParticleRenderer::TextureReader reader, bool any_thread) {
    texture_reader_ = std::move(reader);
    texture_reader_any_thread_ = any_thread;
    if (cloud_renderer_) cloud_renderer_->set_texture_reader(texture_reader_, texture_reader_any_thread_);
}

glm::vec3 ActorEmitters::texture_color(const std::string& name, bool cloud) const {
    const auto* renderer = cloud ? cloud_renderer_.get() : renderer_.get();
    return renderer ? renderer->texture_color(name) : glm::vec3(1.0f);
}

glm::mat4 ActorEmitters::local_matrix(Live& live, const assets::MeshEmitter& me) {
    if (!me.cloud_animation) return me.local;
    const auto& animation = *me.cloud_animation;
    auto& posed = live.cloud_poses[&animation];
    const auto ms = animation.period_ms > 0
                        ? static_cast<std::uint64_t>(std::fmod(live.elapsed * 1000.0, animation.period_ms))
                        : std::uint64_t{0};
    if (posed.ms != ms) {
        animation.pose(ms, posed.motion);
        posed.ms = ms;
    }
    return me.cloud_animation_node < posed.motion.size() ? posed.motion[me.cloud_animation_node] * me.local
                                                         : me.local;
}

std::optional<game::EmitterAttachment> ActorEmitters::attach(std::uint32_t id, std::int32_t i,
                                                             const BoneWorld& bone_world) {
    if (i >= kEffectPointBase) {
        // A level-up effect's emitter: its spell point on the spawn as drawn.
        glm::mat4 m;
        if (!point_world_ || !point_world_(id, i - kEffectPointBase, m)) return std::nullopt;
        game::EmitterAttachment at;
        at.position = glm::vec3(m[3]);
        at.axes = glm::mat3(m);
        return at;
    }
    auto it = live_.find(id);
    if (it == live_.end() || !it->second.mesh || i < 0 ||
        static_cast<std::size_t>(i) >= it->second.mesh->emitters.size())
        return std::nullopt;
    const auto& me = it->second.mesh->emitters[static_cast<std::size_t>(i)];
    glm::mat4 bone;
    if (!bone_world(id, me.bone, bone)) return std::nullopt;
    glm::mat4 m = bone * local_matrix(it->second, me);
    game::EmitterAttachment at;
    at.position = glm::vec3(m[3]);
    at.axes = glm::mat3(m);
    at.alpha = it->second.alpha;
    at.particle_scale = me.cloud ? 1.0f : heights_.pes_factor(me.model_code);
    return at;
}

bool ActorEmitters::start_effect(const Effect& effect) {
    finish();
    if (effect.slot >= assets::kSpellPointNames.size() || !point_world_) return false;
    glm::mat4 point;
    if (!point_world_(effect.actor, static_cast<int>(effect.slot), point)) return false;
    game::SpellEmitterRequest r;
    r.emitter = static_cast<std::int32_t>(effect.effect);
    const auto key = next_key_++;
    r.key = key;
    // vf126 gives an actor's emitter 5,000,000 ms like the always-on ones; this one is not persistent, so it ends
    // with the definition's own duration.
    r.duration_ms = game::kActorEmitterDurationMs;
    r.density = settings_.density;
    r.opacity = settings_.opacity;
    r.position = glm::vec3(point[3]);
    if (effect.flag) {
        const float scale = glm::length(glm::vec3(point[0]));
        r.scale = std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
        // vf126 casts the point 1000 units straight down (0x1013681c) and raises the hit by 0.05 (0x10132e6c).
        if (floor_)
            if (auto z = floor_(r.position); z && r.position.z - *z <= 1000.0f) r.position.z = *z + 0.05f;
    } else {
        r.actor = effect.actor;
        r.bone_point = kEffectPointBase + static_cast<std::int32_t>(effect.slot);
        r.point_only = true;
    }
    r.server_effect = true;
    // create() adds the emitter even when its particles last 0 ms, so whether one was made is told by the list.
    const std::size_t before = particles_.emitters().size();
    particles_.create(r);
    if (particles_.emitters().size() == before) return false; // an effect not in the file, or density 0
    while (effects_.size() >= kMaxEffects) {
        const std::uint32_t oldest = effects_.front().key;
        particles_.stop_keys(std::span(&oldest, 1));
        effects_.erase(effects_.begin());
    }
    effects_.push_back({key, effect.actor});
    return true;
}

void ActorEmitters::prune_effects(const std::function<bool(std::uint32_t id)>& alive) {
    if (effects_.empty()) return;
    finish();
    std::vector<std::uint32_t> gone;
    std::erase_if(effects_, [&](const RunningEffect& e) {
        const bool running = particles_.has(e.key);
        if (!running) return true; // ended on its own
        if (alive && alive(e.actor)) return false;
        gone.push_back(e.key);
        return true;
    });
    std::sort(gone.begin(), gone.end());
    particles_.stop_keys(gone);
}

void ActorEmitters::update(float dt, std::span<const Actor> actors, const BoneWorld& bone_world,
                           const glm::vec3& viewer, const Playing& playing) {
    finish();
    for (auto& [id, live] : live_) live.seen = false;
    // One persistent emitter for a mesh's entry `i` on the actor.
    auto start = [&](const Actor& a, std::size_t i, std::int32_t duration_ms = -1) -> std::optional<std::uint32_t> {
        const auto& me = a.mesh->emitters[i];
        glm::mat4 bone;
        if (!bone_world(a.id, me.bone, bone)) return std::nullopt;
        game::SpellEmitterRequest r;
        r.emitter = static_cast<std::int32_t>(me.emitter.emitter);
        const auto key = next_key_++;
        r.key = key;
        // CActor::vf126 @0x1003a950 creates an actor's emitter for 5,000,000 ms (past 4,000 s the DLL keeps it
        // going), at the actor density and opacity, scale 1; the entry's own +0x58 is not passed. It sets the
        // particle capacity and what a lifetime below 0 lasts; the emitter itself lasts as long as the actor.
        r.duration_ms = duration_ms >= 0 ? duration_ms : game::kActorEmitterDurationMs;
        r.density = settings_.density;
        r.opacity = settings_.opacity;
        r.position = glm::vec3((bone * local_matrix(live_.at(a.id), me))[3]);
        r.actor = a.id;
        r.bone_point = static_cast<std::int32_t>(i);
        r.point_only = true;
        r.persistent = duration_ms < 0;
        if (me.cloud) {
            auto [index, added] = cloud_indexes_.try_emplace(me.cloud.get(), 0);
            if (added) {
                index->second = clouds_.add_definition(*me.cloud);
                cloud_definitions_.push_back(me.cloud);
            }
            r.emitter = static_cast<std::int32_t>(index->second);
            // WLD actor creation (0x100523b0 / 0x1006adc0) takes opacity from descriptor +0x170 and a separate
            // manager density (+0xa8), not ActorParticleDensity (+0xac). The constructor (0x10070230) sets 1;
            // external configuration overrides remain untraced.
            r.duration_ms = game::kFixtureDurationMs;
            r.density = 1.0f;
            r.opacity = me.cloud->scale2;
            clouds_.create(r);
            return key;
        }
        particles_.create(r);
        return key;
    };
    // An actor whose mesh changed (or whose EDD entries are made again at a new density) and an actor gone lose their
    // emitters first, all in one pass over each system (stop_actors), before any are made again: a stop by actor
    // after the new ones were made would take those too.
    const bool edd_on = settings_.density > 0.0f; // ActorParticleDensity 0 makes no EDD emitter: none is tried
    stop_keys_.clear();
    stop_actors_.clear();
    stop_cloud_actors_.clear();
    restarts_.clear();
    lives_.assign(actors.size(), nullptr);
    for (std::size_t ai = 0; ai < actors.size(); ++ai) {
        const Actor& a = actors[ai];
        if (!a.mesh || a.mesh->emitters.empty()) continue;
        Live& live = live_[a.id];
        lives_[ai] = &live;
        live.seen = true;
        live.alpha = a.alpha;
        live.held = !a.in_view;
        live.mesh = a.mesh; // (the same mesh, or one at the same address: the attachments below read this one)
        const bool changed = live.mesh_serial != a.mesh->serial.value();
        if (changed) {
            live.elapsed = 0.0;
            live.cloud_poses.clear();
        } else if (std::isfinite(dt) && dt > 0.0f) {
            live.elapsed += dt;
        }
        if (changed || live.restart_edd) {
            // A new mesh (dressed again, another weapon): its emitters replace the old ones. Every EDD emitter of
            // the actor is its own (always-on, standing and animation-started); its WLD clouds go only with the mesh.
            stop_actors_.push_back(a.id);
            if (changed) stop_cloud_actors_.push_back(a.id);
            live.keys.clear();
            if (changed) live.cloud_keys.clear();
            live.standing_keys.clear();
            live.triggered.clear();
            live.mesh_serial = a.mesh->serial.value();
            live.restart_edd = false;
            restarts_.push_back({ai, changed});
        }
    }
    for (auto it = live_.begin(); it != live_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        stop_actors_.push_back(it->first);
        stop_cloud_actors_.push_back(it->first);
        it = live_.erase(it); // (the other entries, and lives_' pointers to them, stay where they are)
    }
    std::sort(stop_actors_.begin(), stop_actors_.end());
    std::sort(stop_cloud_actors_.begin(), stop_cloud_actors_.end());
    particles_.stop_actors(stop_actors_);
    clouds_.stop_actors(stop_cloud_actors_);
    for (const auto& [ai, changed] : restarts_) {
        const Actor& a = actors[ai];
        Live& live = *lives_[ai];
        // Tried once per mesh: an entry that makes nothing (an index past the file's end, density 0) is not
        // tried again each frame; a new mesh (another look or weapon) tries its entries afresh.
        for (std::size_t i = 0; i < a.mesh->emitters.size(); ++i) {
            const auto& e = a.mesh->emitters[i].emitter;
            if (a.mesh->emitters[i].cloud) {
                if (changed)
                    if (auto key = start(a, i)) live.cloud_keys.push_back(*key);
                continue;
            }
            if (!edd_on || e.trigger != 0 || e.variant >= 1) continue;
            if (auto key = start(a, i)) live.keys.push_back(*key);
        }
    }
    for (std::size_t ai = 0; ai < actors.size(); ++ai) {
        if (!lives_[ai]) continue;
        const Actor& a = actors[ai];
        Live& live = *lives_[ai];
        // The standing entries start when it stands and stop when it moves, casts or acts.
        if (edd_on && a.standing && live.standing_keys.empty()) {
            for (std::size_t i = 0; i < a.mesh->emitters.size(); ++i) {
                const auto& e = a.mesh->emitters[i].emitter;
                if (a.mesh->emitters[i].cloud) continue;
                if (e.trigger != 1 || e.variant >= 1 || (e.animation != kStnd && e.animation != kIdle)) continue;
                if (auto key = start(a, i)) live.standing_keys.push_back(*key);
            }
        } else if ((!edd_on || !a.standing) && !live.standing_keys.empty()) {
            stop_keys_.insert(stop_keys_.end(), live.standing_keys.begin(), live.standing_keys.end());
            live.standing_keys.clear();
        }
        // The other animation-started entries: each start (or loop) of the animation starts one after its delay.
        if (playing && edd_on)
            for (std::size_t i = 0; i < a.mesh->emitters.size(); ++i) {
                const auto& e = a.mesh->emitters[i].emitter;
                if (a.mesh->emitters[i].cloud) continue;
                if (e.trigger != 1 || e.variant >= 1 || e.animation == kStnd || e.animation == kIdle) continue;
                const std::string_view code = triggered_clip(e.animation);
                if (code.empty()) continue;
                auto& t = live.triggered[i];
                float time = 0.0f;
                const bool on = playing(a.id, code, time);
                if (on && (t.last < 0.0f || time + 1e-4f < t.last)) {
                    t.pending = static_cast<float>(std::max(e.delay_ms, 0)) * 0.001f;
                    t.length = 0.0f;
                }
                if (on) t.length = std::max(t.length, time);
                t.last = on ? time : -1.0f;
                if (t.pending >= 0.0f) {
                    t.pending -= dt;
                    if (t.pending < 0.0f) {
                        if (t.key) stop_keys_.push_back(t.key);
                        t.key = 0;
                        // Hailstone: an actor out of view starts none (a footfall or a swing nobody sees); one
                        // that comes back into view starts them again with its next step or swing.
                        if (live.held) continue;
                        // Emitting for about one play of the animation (a swing, a step), at most the entry's own.
                        std::int32_t ms = e.duration_ms > 0 ? std::min(e.duration_ms, 1500) : 1000;
                        t.key = start(a, i, ms).value_or(0u);
                        log::debug("actor emitters: {} starts entry {} (emitter {}, clip {}): key {}", a.id, i,
                                   e.emitter, code, t.key);
                    }
                }
            }
    }
    // The emitters replaced above, in one pass (their replacements have keys of their own).
    std::sort(stop_keys_.begin(), stop_keys_.end());
    particles_.stop_keys(stop_keys_);
    auto held = [this](std::uint32_t id) {
        auto it = live_.find(id);
        return it != live_.end() && it->second.held;
    };
    particles_.set_held(held);
    clouds_.set_held(held);
    particles_.set_viewer(viewer);
    clouds_.set_viewer(viewer);
    auto attachment = [&](std::uint32_t id, std::int32_t i) -> std::optional<game::EmitterAttachment> {
        if (held(id)) return std::nullopt; // out of view: left where it was, its pose not asked for
        return attach(id, i, bone_world);
    };
    if (!async_) {
        particles_.update(dt, attachment);
        clouds_.update(dt, attachment);
    }
    if (renderer_) renderer_->request_textures(particles_);
    if (cloud_renderer_) cloud_renderer_->request_textures(clouds_);
    if (!async_) prepare();
}

void ActorEmitters::prepare() {
    finish();
    if (renderer_) renderer_->upload_textures();
    else if (!particles_.emitters().empty() && vk_ && game_) {
        renderer_ = std::make_unique<render::ParticleRenderer>(*vk_, *game_, shader_dir_, scene_);
        renderer_->request_textures(particles_);
    }
    if (cloud_renderer_) cloud_renderer_->upload_textures();
    else if (!clouds_.emitters().empty() && vk_ && game_) {
        cloud_renderer_ = std::make_unique<render::ParticleRenderer>(*vk_, *game_, shader_dir_, scene_);
        cloud_renderer_->set_rows_bottom_up(true);
        cloud_renderer_->set_texture_reader(texture_reader_, texture_reader_any_thread_);
        cloud_renderer_->request_textures(clouds_);
    }
}

void ActorEmitters::simulate_async(float dt, const BoneWorld& bone_world) {
    finish();
    if (!async_) return;
    // Every attached emitter's place, from the poses as drawn this frame, read here: the actors, the bones and the
    // objects belong to this thread. The step itself runs on a worker over the two systems and this table alone.
    attachments_.clear();
    auto keep = [&](const game::ParticleSystem& system) {
        for (const auto& e : system.emitters())
            if (e.actor != 0 && !e.held) { // a held one (out of view) stays where it was, its pose not asked for
                const std::uint64_t key = attachment_key(e.actor, e.point);
                if (!attachments_.contains(key)) attachments_.emplace(key, attach(e.actor, e.point, bone_world));
            }
    };
    keep(particles_);
    keep(clouds_);
    auto step = [this, dt] {
        auto attachment = [this](std::uint32_t id, std::int32_t i) -> std::optional<game::EmitterAttachment> {
            auto it = attachments_.find(attachment_key(id, i));
            return it == attachments_.end() ? std::nullopt : it->second;
        };
        particles_.update(dt, attachment);
        clouds_.update(dt, attachment);
    };
    if (core::WorkerPool* pool = core::workers()) step_ = pool->run(step);
    else step();
}

void ActorEmitters::finish() const {
    if (step_.valid()) step_.wait();
}

void ActorEmitters::draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& view,
                         float glow, const render::SceneLighting* modern) {
    finish();
    if (renderer_) renderer_->draw(frame, view_proj, view, particles_, glow, {}, modern);
    if (cloud_renderer_) cloud_renderer_->draw(frame, view_proj, view, clouds_, glow, {}, modern);
}

void ActorEmitters::set_density(float density) {
    density = std::clamp(density, 0.0f, 1.0f);
    if (density == settings_.density) return;
    const bool switched = (density > 0.0f) != (settings_.density > 0.0f);
    settings_.density = density;
    if (switched) {
        // Off, or on again: the EDD entries are made again (or not at all) at the next update.
        for (auto& [id, live] : live_) live.restart_edd = true;
        return;
    }
    // The always-on and standing emitters keep their particles and emit at the new density from now on (the
    // effect manager's +0xac setter, ParticleSystem::set_persistent_density); an animation-started one keeps its
    // own until it ends. They were all made again before, which dropped every particle at once.
    finish();
    particles_.set_persistent_density(density);
}

void ActorEmitters::clear() {
    finish();
    particles_.clear();
    clouds_ = game::ParticleSystem({assets::EmitterDefinition{}});
    cloud_indexes_.clear();
    cloud_definitions_.clear();
    effects_.clear();
    live_.clear();
}

} // namespace hailstone::app
