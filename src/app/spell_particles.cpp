#include "app/spell_particles.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <system_error>

#include "assets/character.h"
#include "assets/pfs.h"
#include "assets/wld_particles.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "game/collision.h"
#include "game/path_trail.h"
#include "game/spells.h"
#include "game/missiles.h"
#include "net/game_connection.h"
#include "render/actor_renderer.h"

namespace hailstone::app {

namespace {
// This is a separate named actor role, not one of the ten spell points.
constexpr int kBloodPoint = 10;
constexpr std::uint32_t kMissileActorBit = 0x80000000u; // app/world_objects.h's actor namespace

// The blood cloud's definition and its texture's bytes, the archive itself not kept.
struct BloodSprite {
    assets::EmitterDefinition definition;
    std::string texture; // GameDir::normalize'd
    std::shared_ptr<const std::vector<std::byte>> bytes;
};

std::optional<BloodSprite> read_blood_sprite(const std::filesystem::path& path,
                                           std::shared_ptr<SharedFileCache> cache) {
    auto archive = assets::PfsArchive::open(path, std::move(cache));
    auto wld_bytes = archive.read("gequip.wld");
    if (!wld_bytes) return std::nullopt;
    auto wld = assets::wld::WldFile::parse(*wld_bytes, "gequip.s3d:gequip.wld");
    const auto* sprite = wld.find("I_BLOODSPRITE");
    if (!sprite) return std::nullopt;
    const auto ref = static_cast<assets::wld::Ref>(sprite - wld.fragments().data() + 1);
    auto tex = assets::particle_texture(wld, ref);
    if (!tex || tex->frames != 1) return std::nullopt;
    auto material = tex->material & 0x80000000u ? assets::particle_material_alias(tex->material)
                                               : std::optional(tex->material);
    if (!material || !(*material & 0x1000000u)) return std::nullopt;
    auto bytes = archive.read(tex->file);
    if (!bytes) return std::nullopt;
    BloodSprite out;
    out.definition = game::blood_spurt_definition(tex->file);
    out.definition.additive = (*material >> 20) & 1u;
    out.texture = GameDir::normalize(tex->file);
    out.bytes = std::make_shared<const std::vector<std::byte>>(std::move(*bytes));
    return out;
}

// gequip.s3d's sprite, read again only when the file found is another or has changed: reload_server_files makes
// SpellParticles again for every change of the server's files, and this parses the whole of gequip.wld.
std::optional<BloodSprite> load_blood_sprite(const GameDir& game) {
    struct Cached {
        std::filesystem::path path;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type time{};
        std::optional<BloodSprite> sprite;
    };
    static std::optional<Cached> cache;
    auto path = game.find("gequip.s3d");
    if (!path) return std::nullopt;
    std::error_code size_error, time_error;
    const auto size = std::filesystem::file_size(*path, size_error);
    const auto time = std::filesystem::last_write_time(*path, time_error);
    if (cache && !size_error && !time_error && cache->path == *path && cache->size == size && cache->time == time)
        return cache->sprite;
    std::optional<BloodSprite> sprite;
    try {
        sprite = read_blood_sprite(*path, game.shared_file_cache());
    } catch (const std::exception& e) {
        log::warn("blood particles: {}", e.what());
    }
    if (!size_error && !time_error) cache = Cached{*path, size, time, sprite};
    return sprite;
}
} // namespace

SpellParticles::SpellParticles(std::vector<assets::EmitterDefinition> definitions,
                               std::vector<assets::SpellEffect> effects, CollisionSource collision)
    : effects_(std::move(effects)), particles_(std::move(definitions)) {
    bind_collision(std::move(collision));
}

SpellParticles::SpellParticles(render::VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir,
                               CollisionSource collision) {
    bind_collision(std::move(collision));
    try {
        if (auto data = game.read("spellsnew.edd")) particles_ = game::ParticleSystem(assets::parse_edd(*data));
        if (auto data = game.read("spellsnew.eff")) effects_ = assets::parse_spell_effects(*data);
    } catch (const std::exception& e) {
        log::warn("spell particles: {}", e.what());
    }
    IniFile ini;
    if (auto path = game.find("eqclient.ini")) ini.load(*path);
    auto value = [&](std::string_view key) -> std::optional<float> {
        auto v = ini.get("Defaults", key);
        if (!v || v->empty()) return std::nullopt;
        return std::strtof(v->c_str(), nullptr);
    };
    settings_ = game::spell_particle_settings(value("SpellParticleDensity"), value("SpellParticleOpacity"));
    eqclient_density_ = settings_.density;
    if (auto path = game.find("Resources/SPOffsets.ini")) sp_offsets_.load(*path);
    if (auto moddat = game.read("Resources/moddat.ini"))
        heights_ = game::parse_height_tables({}, std::string_view(reinterpret_cast<const char*>(moddat->data()),
                                                                  moddat->size()));
    spell_points_ = assets::SpellPoints([this](std::string_view section, std::string_view key) {
        std::optional<float> v;
        if (auto text = sp_offsets_.get(section, key)) v = std::strtof(text->c_str(), nullptr);
        return v;
    });
    // The original resolves I_BLOODSPRITE from the global equipment resources; without a supported sprite chain
    // there is no blood.
    std::optional<BloodSprite> blood = load_blood_sprite(game);
    if (blood) set_blood_sprite(blood->definition);
    if (!particles_.empty()) {
        renderer_ = std::make_unique<render::ParticleRenderer>(vk, game, shader_dir);
        if (blood)
            renderer_->set_texture_reader([name = blood->texture, bytes = blood->bytes](const std::string& wanted) {
                return GameDir::normalize(wanted) == name ? std::optional(*bytes) : std::nullopt;
            });
        log::info("spell particles: {} emitter definitions, {} effects, density {:.2f}, opacity {:.2f}",
                  particles_.definitions().size(), effects_.size(), settings_.density, settings_.opacity);
    }
}

void SpellParticles::bind_collision(CollisionSource collision) {
    if (!collision) return;
    floor_ = [source = collision](const glm::vec3& p) -> std::optional<float> {
        const auto* mesh = source();
        return mesh ? mesh->floor_below(p, 0.0f) : std::nullopt;
    };
    trail_floor_ = [source = std::move(collision)](const glm::vec3& p) -> std::optional<float> {
        const auto* mesh = source();
        return mesh ? mesh->ground_below(p) : std::nullopt;
    };
}

void SpellParticles::set_blood_sprite(assets::EmitterDefinition definition) {
    if (definition.texture.empty() || blood_definition_) return;
    if (particles_.empty()) particles_.add_definition({}); // definition zero is the sentinel
    blood_definition_ = particles_.add_definition(std::move(definition));
}

std::optional<glm::mat4> SpellParticles::blood_point_world(const render::ActorRenderer* actors,
                                                         std::uint32_t id) const {
    if (blood_point_) return blood_point_(id);
    if (!actors) return std::nullopt;
    const auto* model = actors->drawn_model(id);
    if (!model || model->blood_bone < 0) return std::nullopt;
    glm::mat4 point;
    return actors->drawn_bone(id, model->blood_bone, point) ? std::optional(point) : std::nullopt;
}

void SpellParticles::update_blood(net::GameConnection& conn, const render::ActorRenderer* actors) {
    const auto& spawns = conn.world().spawns;
    for (auto id : conn.take_blood_spurts()) {
        if (!blood_definition_ || !spawns.contains(id)) continue;
        auto point = blood_point_world(actors, id);
        if (!point) continue;
        game::SpellEmitterRequest r;
        r.emitter = static_cast<std::int32_t>(*blood_definition_);
        r.actor = id;
        r.bone_point = kBloodPoint;
        r.point_only = true;
        r.position = glm::vec3((*point)[3]);
        particles_.create(r);
    }
    std::vector<std::uint32_t> gone;
    for (const auto& e : particles_.emitters())
        if (e.point == kBloodPoint && !spawns.contains(e.actor)) gone.push_back(e.actor);
    for (auto id : gone) particles_.stop_attachment(id, kBloodPoint);
}

void SpellParticles::missile_effect(const net::GameConnection& conn, const game::Missile& missile, int stage,
                                     const render::ActorRenderer* actors) {
    const auto* spell = conn.spell_table().get(missile.packet.spell);
    if (!spell || spell->spell_anim < 1 || static_cast<std::size_t>(spell->spell_anim) >= effects_.size()) return;
    auto caster = stage_actor(conn, missile.packet.source_id, actors);
    game::StageActor at;
    // Without its actor (spell 54's destruction impact, played as the missile goes) the location 2 slots stay at
    // the missile's last place rather than on an actor end_missile removes.
    at.id = missile.has_actor ? kMissileActorBit | missile.serial : 0u;
    at.position = missile.actor_position;
    at.points = 0; // no fabricated eyes, mouth or spell bones on a missile actor
    game::SpellParticleSettings settings = settings_;
    settings.density *= source_density_for(conn, missile.packet.source_id, 0);
    for (const auto& r : game::spawn_stage_emitters(effects_[static_cast<std::size_t>(spell->spell_anim)], stage,
                                                   spell->id, caster ? &*caster : nullptr, nullptr, at.position,
                                                   0, settings, false, &at))
        particles_.create(r);
}

void SpellParticles::end_missile(std::uint32_t serial) { particles_.stop_actor(kMissileActorBit | serial); }

std::optional<glm::mat4> SpellParticles::point_world(const render::ActorRenderer& actors, std::uint32_t id, int point) {
    const assets::CharacterModel* model = actors.drawn_model(id);
    if (!model || point < 0 || point >= 10) return std::nullopt;
    const assets::SpellPoints::Model& sp = spell_points_.of(*model);
    if (!((sp.mask >> point) & 1u)) return std::nullopt;
    glm::mat4 bone(1.0f);
    if (!actors.drawn_bone(id, sp.bone[static_cast<std::size_t>(point)], bone)) return std::nullopt;
    return bone * sp.local[static_cast<std::size_t>(point)];
}

void SpellParticles::update(net::GameConnection& conn, std::span<const net::SpellEffectCue> cues,
                            std::span<const net::EffectPacketCue> effect_packets, const render::ActorRenderer* actors,
                            float dt) {
    clock_ += dt;
    update_blood(conn, actors);
    for (const auto& cue : cues) {
        Held h{clock_ + cue.delay, cue.stage, cue.caster, cue.target, cue.spell, cue.source, cue.duration_ms};
        if (cue.delay > 0.0) held_.push_back(h);
        else play(conn, h, actors);
    }
    const auto now_ms = static_cast<std::uint64_t>(clock_ * 1000.0);
    using Kind = net::EffectPacketCue::Kind;
    for (const auto& e : effect_packets) {
        log::info("effect packet {}: effect {} caster {} target {} ({} / {} / {} ms, full {})", static_cast<int>(e.kind),
                  e.effect, e.caster, e.target, e.stage1_ms, e.stage1_length_ms, e.stage2_length_ms, e.full);
        switch (e.kind) {
        case Kind::Add:
            timers_.add(e.caster, e.target, e.effect, e.stage1_ms, e.stage1_length_ms, e.stage2_length_ms, e.full,
                        now_ms);
            break;
        case Kind::Remove:
            // SpellEffectTimers_Remove: the emitters stop only when an entry was dropped. A nimbus (EQEmu sends
            // it as an OP_SpellEffect add) outlives its entry, so its removal is OP_RemoveNimbusEffect's.
            if (timers_.remove(e.caster, e.target, e.effect)) {
                particles_.stop(e.caster << 16 | (e.effect & 0xffffu));
                if (e.target != 0xffffffffu) particles_.stop(e.target << 16 | (e.effect & 0xffffu));
            }
            break;
        case Kind::AddNimbus:
            // Handle_OP_AddNimbusEffect @0x4bce70: the impact stage, the spawn as caster and target, duration 0.
            play_effect(conn, {e.caster, e.caster, e.effect, 2, 0, false}, actors);
            break;
        case Kind::RemoveNimbus:
            // Handle_OP_RemoveNimbusEffect @0x4b7a10.
            particles_.stop(e.caster << 16 | (e.effect & 0xffffu));
            break;
        }
    }
    timers_.update(now_ms);
    for (const auto& play : timers_.take()) play_effect(conn, play, actors);
    for (std::size_t i = 0; i < held_.size();) {
        if (held_[i].at > clock_) {
            ++i;
            continue;
        }
        Held h = held_[i];
        held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        play(conn, h, actors);
    }
    update_path(conn);
    const auto& world = conn.world();
    auto position = [&](std::uint32_t id) -> std::optional<glm::vec3> {
        auto it = world.spawns.find(id);
        if (it == world.spawns.end()) return std::nullopt;
        const auto& s = it->second;
        return s.display_valid ? s.display_position : s.position;
    };
    particles_.update(dt, [&](std::uint32_t id, std::int32_t pt) -> std::optional<game::EmitterAttachment> {
        if ((id & kMissileActorBit) != 0) {
            auto pose = missile_pose_ ? missile_pose_(id & ~kMissileActorBit) : std::nullopt;
            if (!pose) return std::nullopt;
            game::EmitterAttachment a;
            a.position = glm::vec3((*pose)[3]);
            a.actor_axes = glm::mat3(*pose);
            a.actor_scale = glm::length((*pose)[0]);
            return a;
        }
        auto it = world.spawns.find(id);
        if (it == world.spawns.end()) return std::nullopt;
        game::EmitterAttachment a;
        // The actor's root GetScaleFactor (its model scale) and GetParticleScaleFactor (moddat.ini PESFactor).
        if (it->second.heights.model_scale > 0.0f) a.actor_scale = it->second.heights.model_scale;
        if (actors) {
            if (const assets::CharacterModel* model = actors->spawn_model(id)) a.particle_scale = particle_scale(*model);
            if (pt >= 0 && pt < kBloodPoint)
                if (auto m = point_world(*actors, id, pt)) {
                    a.position = glm::vec3((*m)[3]);
                    a.axes = glm::mat3(*m);
                }
            glm::mat4 t;
            if (actors->drawn_transform(id, t)) {
                a.actor_axes = glm::mat3(t);
                a.alpha = actors->drawn_alpha(id);
            }
        }
        if (pt == kBloodPoint)
            if (auto m = blood_point_world(actors, id)) {
                a.position = glm::vec3((*m)[3]);
                a.axes = glm::mat3(*m);
            }
        // A point the model turned out not to have (asked for before it loaded): point 0, as the slot's own fallback
        // would have chosen. Without its model drawn, a pointed emitter keeps to the spawn's place.
        if (pt > 0 && pt < kBloodPoint && !a.position && actors)
            if (auto m = point_world(*actors, id, 0)) {
                a.position = glm::vec3((*m)[3]);
                a.axes = glm::mat3(*m);
            }
        if (pt >= 0 && pt < kBloodPoint && !a.position) a.position = position(id);
        return a;
    });
}

float SpellParticles::particle_scale(const assets::CharacterModel& model) const {
    return heights_.pes_factor(model.code);
}

namespace {
// The keys of the path's emitters (above any spawn id << 16 | spell id a spell's emitters use).
constexpr std::uint32_t kPathLineKey = 0xfff00000u;
constexpr std::uint32_t kPathFinishKey = 0xfff00001u;
constexpr std::uint32_t kGuideLineKey = 0xfff00002u;
} // namespace

void SpellParticles::draw_trail(std::uint32_t key, std::span<const glm::vec3> line, float far) {
    game::TrailStyle st;
    st.far_end = far;
    st.far_start = far * 0.7f;
    if (find_trail_style_ == game::FindTrailStyle::Navigation) {
        if (!navigation_definition_) {
            assets::EmitterDefinition d;
            d.name = "FindNavigationLine";
            d.no_zwrite = 1;
            d.billboard = game::kWorldBasisBillboard;
            d.frames = 1;
            navigation_definition_ = particles_.add_definition(std::move(d));
        }
        std::vector<game::Particle> quads;
        const auto ribbon = game::build_navigation_trail(line, st, clock_, trail_floor_);
        quads.reserve(ribbon.size());
        for (const auto& piece : ribbon) {
            game::Particle q;
            q.position = piece.position;
            q.axes[0] = piece.across;
            q.axes[1] = piece.forward;
            q.width = piece.width;
            q.height = piece.length;
            q.color = glm::vec4(piece.color, piece.alpha);
            quads.push_back(q);
        }
        if (quads.empty()) particles_.stop(key);
        else particles_.set_still(key, *navigation_definition_, std::move(quads));
        return;
    }
    if (!path_definition_) {
        // Solid, thin segments: their width faces the camera while their endpoints follow the hovering strands.
        assets::EmitterDefinition d;
        d.name = "FindRainbowStrands";
        d.additive = 1;
        d.no_zwrite = 1;
        d.billboard = game::kTrailStrandBillboard;
        d.frames = 1;
        path_definition_ = particles_.add_definition(std::move(d));
    }
    std::vector<game::Particle> quads;
    const auto trail = game::build_rainbow_strands(line, st, clock_, trail_floor_);
    quads.reserve(trail.size());
    for (const auto& t : trail) {
        game::Particle q;
        q.position = t.position;
        q.axes[0] = t.across;
        q.axes[1] = t.forward;
        q.width = t.width;
        q.height = t.length;
        q.color = glm::vec4(t.color, t.alpha);
        q.lifetime = 1.0f;
        quads.push_back(q);
    }
    if (quads.empty()) particles_.stop(key);
    else particles_.set_still(key, *path_definition_, std::move(quads));
}

void SpellParticles::update_path(net::GameConnection& conn) {
    if (particles_.empty()) return;
    const game::PlayerPath& path = conn.player_path();
    const game::PathSettings& ps = path.settings();
    // Both styles follow PlayerPath's route and fade at Path.ini's range. This client's look replaces the original
    // textured tube (ThickLineSystem with Path.ini's Texture and ShapeSet).
    {
        std::vector<glm::vec3> line;
        if (path.active()) {
            const auto& pts = path.points(); // the destination first: the trail runs from the player
            line.assign(pts.rbegin(), pts.rend());
            // PlayerPath stores its grounded points at FloorOffset * AvatarHeight, rather than at the feet.
            // Undo that presentation offset before following terrain; large races can be more than six above it.
            if (const auto* me = conn.world().player(); me && !me->gm)
                for (auto& p : line) p.z -= path.floor_offset();
        }
        draw_trail(kPathLineKey, line, std::max(ps.fade_end, 40.0f));
    }
    // The Find window's guide path (app/guide_path.h): the same selected style.
    draw_trail(kGuideLineKey, guide_line_, 170.0f);
    // The selected trail replaces Path.ini's travelling clouds: those follow elevated, sine-displaced points and
    // would obscure the strands or float above the ground. PlayerPath's timing, sounds and route stay unchanged.
    // PlayerPath_OnDestinationReached: the FinishEmitterN at the destination for their LifeSpan.
    if (auto at = conn.take_path_finish(); at && trail_floor_) {
        auto ground = trail_floor_(*at + glm::vec3(0, 0, 0.5f));
        if (!ground || !std::isfinite(*ground) || *ground > at->z + 0.5f || *ground < at->z - 6.0f) return;
        for (const auto& es : ps.finish_emitters) {
            if (es.definition < 1) continue;
            game::SpellEmitterRequest r;
            r.emitter = es.definition;
            r.key = kPathFinishKey;
            r.duration_ms = es.life_span;
            r.density = es.density * settings_.density;
            r.opacity = es.opacity * settings_.opacity;
            r.position = {at->x, at->y, *ground + 0.35f};
            particles_.create(r);
        }
    }
}

std::optional<game::StageActor> SpellParticles::stage_actor(const net::GameConnection& conn, std::uint32_t id,
                                                            const render::ActorRenderer* actors) {
    const auto& world = conn.world();
    auto it = world.spawns.find(id);
    if (it == world.spawns.end()) return std::nullopt;
    const auto& s = it->second;
    game::StageActor a;
    a.id = id;
    a.position = s.display_valid ? s.display_position : s.position;
    a.level = s.level > 0 ? s.level : 1;
    a.height = s.heights.height; // Height (+0x13c): the size, or racedata's default
    // The points are the model's, drawn or not: a spawn not drawn this frame (the player in first person, one out of
    // view) still has them in the original, and an emitter made without a point would stay where it was made.
    a.model_known = false;
    if (actors)
        if (const assets::CharacterModel* model = actors->spawn_model(id)) {
            a.model_known = true;
            a.points = spell_points_.of(*model).mask;
            if (auto chest = point_world(*actors, id, 1)) a.chest = glm::vec3((*chest)[3]);
            a.particle_scale = particle_scale(*model);
        }
    if (floor_) {
        // Attach 12's z: GetPlayerFloorHeight at the chest's x and y (the spawn's without a chest), from 0.1 above
        // the spawn.
        const glm::vec3 at = a.points & 2u ? glm::vec3(a.chest.x, a.chest.y, a.position.z) : a.position;
        a.floor = floor_(at + glm::vec3(0.0f, 0.0f, 0.1f));
    }
    return a;
}

float SpellParticles::source_density_for(const net::GameConnection& conn, std::uint32_t cast_by,
                                         std::uint32_t struck) const {
    const auto& world = conn.world();
    auto is_npc = [&](std::uint32_t id) -> std::optional<bool> {
        auto it = world.spawns.find(id);
        if (id == 0 || it == world.spawns.end()) return std::nullopt;
        return it->second.is_npc;
    };
    return source_density(game::spell_fx_source(cast_by, struck, world.player_id, is_npc(cast_by), is_npc(struck)));
}

void SpellParticles::play(net::GameConnection& conn, const Held& cue, const render::ActorRenderer* actors) {
    if (particles_.empty()) return;
    const game::SpellData* sp = conn.spell_table().get(cue.spell);
    if (!sp || sp->spell_anim < 1 || static_cast<std::size_t>(sp->spell_anim) >= effects_.size()) return;
    // Hailstone: the Options window's density for whose spell it is (the impact stage's caster is the cue's source).
    const std::uint32_t cast_by = cue.stage == 2 ? cue.source : cue.caster;
    game::SpellParticleSettings settings = settings_;
    settings.density *= source_density_for(conn, cast_by, cue.target != 0 ? cue.target : cue.caster);
    if (settings.density <= 0.0f) return;
    // The impact stage's caster is the packet's source: EQPlayer::TriggerSpellEffect (@0x593eb0) passes it to
    // DoSpellEffect as the caster, the struck spawn as the target. With a source of 0 or one not in the zone
    // DoSpellEffect (@0x522500) plays nothing: no caster, no stage.
    auto caster = stage_actor(conn, cast_by, actors);
    if (!caster && cue.stage == 2) return;
    auto target = stage_actor(conn, cue.target, actors);
    glm::vec3 point = caster ? caster->position : target ? target->position : glm::vec3(0.0f);
    std::int32_t longest = 0;
    for (const auto& r : game::spawn_stage_emitters(effects_[static_cast<std::size_t>(sp->spell_anim)], cue.stage,
                                                     cue.spell, caster ? &*caster : nullptr,
                                                     target ? &*target : nullptr, point, cue.duration_ms, settings))
        longest = std::max(longest, particles_.create(r));
    log::debug("spell {} stage {}: effect {} ({} ms)", cue.spell, cue.stage, sp->spell_anim, longest);
}

void SpellParticles::play_effect(net::GameConnection& conn, const game::SpellEffectStagePlay& play,
                                 const render::ActorRenderer* actors) {
    // SpellEffectTimers_PlayStage / Handle_OP_AddNimbusEffect: an effect in 1..count-1 on a caster that exists, at
    // the caster's position; the target may be missing.
    if (particles_.empty() || play.effect < 1 || play.effect >= effects_.size()) return;
    auto caster = stage_actor(conn, play.caster, actors);
    if (!caster) return;
    auto target = stage_actor(conn, play.target, actors);
    // The packet's flag puts the emitters at density and opacity 1 (SpawnStageEmitters' `full`), past
    // SpellParticleDensity. Hailstone: the Options window's per-source density still scales them, and Off hides them.
    game::SpellParticleSettings settings = settings_;
    const float source = source_density_for(conn, play.caster, play.target != 0 ? play.target : play.caster);
    if (play.full) settings = {source, 1.0f};
    else settings.density *= source;
    if (settings.density <= 0.0f) return;
    std::int32_t longest = 0;
    for (const auto& r : game::spawn_stage_emitters(effects_[play.effect], play.stage, play.effect, &*caster,
                                                     target ? &*target : nullptr, caster->position, play.duration_ms,
                                                     settings))
        longest = std::max(longest, particles_.create(r));
    log::debug("effect {} stage {} on {}: {} ms", play.effect, play.stage, play.caster, longest);
}

void SpellParticles::prepare() {
    if (renderer_) renderer_->prepare(particles_);
}

void SpellParticles::draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::mat4& view,
                          float glow, const render::SceneLighting* modern) {
    if (renderer_) renderer_->draw(frame, view_proj, view, particles_, glow, {}, modern);
}

void SpellParticles::clear() {
    particles_.clear();
    held_.clear();
    timers_.clear();
}

std::vector<game::LightSource> SpellParticles::lights(std::size_t max) const {
    return game::particle_lights(
        particles_, [this](const std::string& t) { return renderer_ ? renderer_->texture_color(t) : glm::vec3(1.0f); },
        max);
}

} // namespace hailstone::app
