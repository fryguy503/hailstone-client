#include "app/model_viewer.h"

#include "app/actor_emitters.h"

#include <chrono>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>

#include "assets/character.h"
#include "assets/model_points.h"
#include "assets/spell_effects.h"
#include "assets/zone.h"
#include "core/asset_folder.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "game/anim_layers.h"
#include "game/appearance.h"
#include "game/coords.h"
#include "game/light_list.h"
#include "game/particles.h"
#include "game/race_models.h"
#include "game/scene_light.h"
#include "game/spawn_heights.h"
#include "game/view_distance.h"
#include "app/window_icon.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/temporal.h"
#include "render/actor_renderer.h"
#include "render/camera.h"
#include "render/particle_renderer.h"
#include "render/ambient_probe.h"
#include "render/scene_lighting.h"
#include "render/vk_context.h"
#include "render/zone_renderer.h"

namespace hailstone::app {

namespace {

struct Entry {
    std::string code;
    std::uint16_t race = 0; // 0: unknown (a code was given)
    std::uint8_t gender = 0;
    game::Appearance look;
    std::optional<float> size; // "size=S" in its look: its own spawn size
};

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    for (std::size_t p = 0; p <= s.size();) {
        std::size_t q = std::min(s.find(sep, p), s.size());
        if (q > p) out.push_back(s.substr(p, q - p));
        p = q + 1;
    }
    return out;
}

// "key=value" settings (see --help) applied to an appearance.
void apply_look(game::Appearance& a, const std::string& spec) {
    static constexpr std::pair<const char*, int> kSlots[] = {
        {"head", 0}, {"chest", 1}, {"arms", 2}, {"wrists", 3}, {"hands", 4}, {"legs", 5}, {"feet", 6}, {"pri", 7}, {"sec", 8},
    };
    for (const auto& kv : split(spec, '/')) {
        auto hs = kv.find('=');
        if (hs == std::string::npos) throw std::runtime_error("appearance setting without '=': " + kv);
        std::string k = kv.substr(0, hs), v = kv.substr(hs + 1);
        auto num = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, k == "tint" ? 16 : 10));
        if (k == "tex") a.texture = static_cast<std::uint8_t>(num);
        else if (k == "helm") a.helm = static_cast<std::uint8_t>(num);
        else if (k == "face") a.face = static_cast<std::uint8_t>(num);
        else if (k == "hair") a.hair_style = static_cast<std::uint8_t>(num);
        else if (k == "beard") a.beard = static_cast<std::uint8_t>(num);
        else if (k == "haircolor") a.hair_color = static_cast<std::uint8_t>(num);
        else if (k == "beardcolor") a.beard_color = static_cast<std::uint8_t>(num);
        else if (k == "eyes") a.eye_color1 = a.eye_color2 = static_cast<std::uint8_t>(num);
        else if (k == "heritage") a.drakkin_heritage = num;
        else if (k == "tattoo") a.drakkin_tattoo = num;
        else if (k == "details") a.drakkin_details = num;
        else if (k == "showhelm") a.show_helm = num != 0;
        else if (k == "npctint") a.npc_tint = num;
        else if (k == "class") a.class_id = static_cast<std::uint8_t>(num);
        // Hero's forge: hf=N gives slot s (0..6) the id N + s (NewArmorTagData's sets, 2000 + slot).
        else if (k == "hf") for (int sl = 0; sl <= 6; ++sl) a.hero_forge[static_cast<std::size_t>(sl)] = num + static_cast<std::uint32_t>(sl);
        // hf0..hf6=N: one slot's hero's forge id (hf1=10801: the chest).
        else if (k.size() == 3 && k.starts_with("hf") && k[2] >= '0' && k[2] <= '6')
            a.hero_forge[static_cast<std::size_t>(k[2] - '0')] = num;
        else if (k == "armor") for (int sl = 1; sl <= 6; ++sl) a.material[static_cast<std::size_t>(sl)] = num;
        else if (k == "tint") for (int sl = 0; sl <= 6; ++sl) a.tint[static_cast<std::size_t>(sl)] = num | 0xff000000u;
        else {
            bool found = false;
            for (auto [name, slot] : kSlots)
                if (k == name) {
                    a.material[static_cast<std::size_t>(slot)] = num;
                    found = true;
                }
            if (!found) throw std::runtime_error("unknown appearance setting " + k);
        }
    }
}

// "HUM", "r:1:0" (race 1, gender 0), with an optional "/key=value/..." look.
Entry resolve(const std::string& s, const std::string& default_look) {
    Entry e;
    auto slash = s.find('/');
    std::string head = s.substr(0, slash);
    e.code = head;
    if (head.starts_with("r:")) {
        char* end = nullptr;
        long race = std::strtol(head.c_str() + 2, &end, 10);
        long gender = (end && *end == ':') ? std::strtol(end + 1, nullptr, 10) : 0;
        e.race = static_cast<std::uint16_t>(race);
        e.gender = static_cast<std::uint8_t>(gender);
        const auto* rm = game::find_race_model(e.race, e.gender);
        if (!rm) throw std::runtime_error("race " + head + " has no model code");
        e.code = std::string(rm->code);
    }
    if (!default_look.empty()) apply_look(e.look, default_look);
    if (slash != std::string::npos) {
        std::string rest;
        for (const auto& kv : split(s.substr(slash + 1), '/')) {
            if (kv.starts_with("size=")) e.size = std::strtof(kv.c_str() + 5, nullptr);
            else rest += (rest.empty() ? "" : "/") + kv;
        }
        if (!rest.empty()) apply_look(e.look, rest);
    }
    return e;
}

} // namespace

int run_model_viewer(const ModelViewerOptions& opts) {
    if (opts.game_dir.empty()) throw std::runtime_error("--model needs --game-dir or EQ_GAME_DIR");
    GameDir game = open_game_dir(opts.game_dir, opts.classic_dir);
    auto t0 = std::chrono::steady_clock::now();
    auto library = std::make_shared<assets::CharacterLibrary>(assets::CharacterLibrary::load(game, opts.zone, opts.luclin));
    log::info("character library: {} archives, {} models in {} ms", library->archives().size(),
              library->model_codes().size(),
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());

    std::vector<std::shared_ptr<const assets::CharacterModel>> models;
    std::vector<std::shared_ptr<const assets::CharacterMesh>> meshes;
    std::vector<Entry> entries;
    std::vector<std::vector<std::uint8_t>> bone_groups(opts.codes.size()); // per model, for --upper-anim
    for (const auto& c : opts.codes) {
        Entry e = resolve(c, opts.look);
        const std::string& code = e.code;
        auto m = library->model(code);
        if (!m) {
            log::warn("model {} ({}) is not available", c, code);
            continue;
        }
        const auto* clip = m->clip(opts.anim);
        log::info("{}: {} bones, {} triangles, rest bounds z {:.2f}..{:.2f}, clip {} {}", m->code, m->bone_names.size(),
                  m->indices.size() / 3, m->bounds_min.z, m->bounds_max.z, opts.anim.empty() ? "rest" : opts.anim,
                  clip ? std::format("{} frames x {} ms", clip->frame_count, clip->frame_ms) : "missing (rest pose)");
        auto mesh = library->dress(*m, e.look);
        if (mesh) log::info("{}: dressed {} vertices, {} parts", c, mesh->vertices.size(), mesh->parts.size());
        meshes.push_back(std::move(mesh));
        entries.push_back(std::move(e));
        models.push_back(std::move(m));
    }
    if (models.empty()) throw std::runtime_error("no model to show");
    // SetHeights in zone (0xddfba4 clear) for the model scale and the EQG root drop.
    auto text = [&](std::string_view file) {
        auto bytes = game.read(file);
        return bytes ? std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()) : std::string();
    };
    const game::HeightTables heights_data = game::parse_height_tables(text("racedata.txt"), text("Resources/moddat.ini"));

    std::optional<assets::Zone> zone;
    if (!opts.zone.empty()) zone = assets::load_zone(game, opts.zone);

    platform::Window window({});
    apply_window_icon(window);
    render::VkContext vk(window, opts.validation);
    std::filesystem::path shader_dir = platform::shaders_dir();
    vk.set_scene_options(opts.scene, shader_dir);
    std::unique_ptr<render::ZoneRenderer> zone_renderer;
    if (zone) zone_renderer = std::make_unique<render::ZoneRenderer>(vk, *zone, shader_dir);
    render::ActorRenderer actors(vk, library, shader_dir);
    render::Daylight sky = render::daylight(opts.hour, true);
    // The zone's baked light where the models stand (render/ambient_probe.h), as the client lights its characters.
    glm::vec4 probe(0.0f);
    if (zone) {
        glm::vec4 at = render::AmbientProbe(*zone).sample(opts.at.value_or(glm::vec3(0.0f)) + glm::vec3(0.0f, 0.0f, 3.0f));
        probe = render::ProbeBlend().update(at, 0.0f);
    }

    // Lay the models out in a row along +Y, facing +X by default.
    glm::vec3 origin = opts.at.value_or(glm::vec3(0.0f));
    std::vector<render::ActorDraw> draws;
    float y = 0.0f, top = 0.0f, scale = 1.0f;
    for (std::size_t i = 0; i < models.size(); ++i) {
        const auto& m = *models[i];
        std::uint16_t race = entries[i].race;
        std::uint8_t gender = entries[i].gender;
        if (!race) // a code: find its race for the scale rule
            for (std::uint16_t r = 1; r < 3000 && !race; ++r)
                for (std::uint8_t g = 0; g < 3; ++g)
                    if (const auto* rm = game::find_race_model(r, g); rm && rm->code == m.code) {
                        race = r;
                        gender = g;
                        break;
                    }
        game::SpawnHeights h;
        h.height = entries[i].size.value_or(opts.size);
        game::set_heights(h, game::height_inputs(&heights_data, race, gender, false), 0.0f, 1.0f);
        scale = h.model_scale;
        log::info("{}: Height {:.2f}, model scale {:.4f}, AvatarHeight {:.4f}, ViewHeight {:.4f}", m.code, h.height,
                  h.model_scale, h.avatar_height, h.view_height);
        if (race) { // as the spawn unserializer does: PC races fold tex into slots 1-6
            game::Appearance look = entries[i].look;
            game::normalize_appearance(look, race);
            if (!(look == entries[i].look)) meshes[i] = library->dress(m, look);
        }
        float width = std::max(m.bounds_max.y - m.bounds_min.y, 2.0f) * scale; // the row runs along Y
        const bool rider = opts.ride && (i % 2) == 1;
        if (i > 0 && !rider) y += width * 0.5f;
        render::ActorDraw a;
        a.model = &m;
        a.mesh = meshes[i].get();
        a.position = origin + glm::vec3(0.0f, y, render::ActorRenderer::model_z_offset(m, scale, &heights_data));
        a.yaw = game::heading_to_yaw(opts.heading);
        a.motion_key = i + 1; // its bones a frame ago (the temporal upscaler's and frame generation's motion)
        a.scale = scale;
        a.clip = m.clip(opts.anim) ? m.clip(opts.anim) : m.clip("");
        if (!opts.upper_anim.empty() && m.clip(opts.upper_anim)) {
            a.upper_clip = m.clip(opts.upper_anim);
            auto& groups = bone_groups[i];
            for (const auto& name : m.bone_names)
                groups.push_back(m.eqg ? game::new_style_bone_group(name) : game::classic_bone_group(name));
            a.bone_groups = &groups;
        }
        if (rider) { // the seated rider's animation (ActorRenderer::prepare_spawns: P09, else standing)
            a.clip = m.clip("P09") ? m.clip("P09") : m.clip("P01") ? m.clip("P01") : m.clip("");
            // Unseated (a mount without the dag), it stands where its spawn is: on the mount's spot.
            a.position.x = draws.back().position.x;
            a.position.y = draws.back().position.y;
            draws.push_back(a);
            continue;
        }
        draws.push_back(a);
        y += width * 0.5f + 4.0f * scale;
        top = std::max(top, m.bounds_max.z * scale + a.position.z - origin.z);
    }
    float row = draws.back().position.y - origin.y;

    render::FlyCamera camera;
    // Fit the row (horizontal half-angle about 54 degrees at 16:9) and the height.
    float distance = std::max(row * 0.42f + 6.0f * scale, top * 2.2f);
    camera.position = opts.camera_pos.value_or(origin + glm::vec3(distance, row * 0.5f, top * 0.25f));
    camera.yaw = glm::radians(opts.yaw_deg.value_or(180.0f));
    camera.pitch = glm::radians(opts.pitch_deg.value_or(-5.0f));
    camera.near_plane = 0.5f;
    // --orbit: round the first model's middle, at the distance and height the camera starts at.
    const glm::vec3 orbit_centre = draws.front().position + glm::vec3(0.0f, 0.0f, top * 0.25f);
    const glm::vec2 orbit_arm = glm::vec2(camera.position - orbit_centre);
    const float orbit_radius = glm::length(orbit_arm);
    const float orbit_start = std::atan2(orbit_arm.y, orbit_arm.x);

    game::ViewDistance view = game::compute_view_distance({}, opts.clip_plane);
    if (!zone) view.fog_color = glm::vec3(0.35f, 0.4f, 0.5f);

    // --spell-effect: the stage's emitters on every model's spell points for 2.5 s (a cast time), replayed every 3 s.
    std::vector<assets::SpellEffect> effects;
    game::ParticleSystem particles;
    std::unique_ptr<render::ParticleRenderer> particle_renderer;
    IniFile sp_offsets;
    if (auto path = game.find("Resources/SPOffsets.ini")) sp_offsets.load(*path);
    assets::SpellPoints spell_points([&](std::string_view section, std::string_view key) {
        std::optional<float> v;
        if (auto value = sp_offsets.get(section, key)) v = std::strtof(value->c_str(), nullptr);
        return v;
    });
    auto point_world = [&](std::size_t i, int point) -> std::optional<glm::mat4> {
        const auto& sp = spell_points.of(*draws[i].model);
        glm::mat4 bone;
        if (point < 0 || point >= 10 || !((sp.mask >> point) & 1u) ||
            !render::ActorRenderer::bone_world(draws[i], sp.bone[static_cast<std::size_t>(point)], bone))
            return std::nullopt;
        return bone * sp.local[static_cast<std::size_t>(point)];
    };
    if (opts.spell_effect > 0) {
        auto edd = game.read("spellsnew.edd");
        auto eff = game.read("spellsnew.eff");
        if (!edd || !eff) throw std::runtime_error("--spell-effect needs spellsnew.edd and spellsnew.eff");
        particles = game::ParticleSystem(assets::parse_edd(*edd));
        effects = assets::parse_spell_effects(*eff);
        if (static_cast<std::size_t>(opts.spell_effect) >= effects.size()) throw std::runtime_error("no such spell effect");
        particle_renderer = std::make_unique<render::ParticleRenderer>(vk, game, shader_dir);
        const auto& fx = effects[static_cast<std::size_t>(opts.spell_effect)];
        log::info("spell effect {} '{}', stage {}", opts.spell_effect, fx.name, opts.spell_stage);
        for (const auto& slot : fx.stages[static_cast<std::size_t>(std::clamp(opts.spell_stage, 0, 2))].slots)
            if (slot.emitter > 0)
                log::info("  emitter {} '{}' location {} attach {}", slot.emitter,
                          particles.definitions()[static_cast<std::size_t>(slot.emitter)].name, slot.location, slot.attach);
    }
    double next_effect = 0.0;
    // The models' and held items' own emitters (app/actor_emitters.h).
    ActorEmitters actor_emitters(vk, game, shader_dir);
    actor_emitters.set_texture_reader([library](const std::string& name) { return library->read_file(name); }, true);
    std::vector<ActorEmitters::Actor> emitter_actors;

    auto start = std::chrono::steady_clock::now();
    auto last = start;
    for (int frame = 0; window.poll_events(); ++frame) {
        if (opts.frames && frame >= opts.frames) break;
        if (opts.screenshot_generated && !opts.screenshot.empty() && opts.frames && frame == opts.frames - 2) {
            std::filesystem::path prev = opts.screenshot; // the real frame before the in-between one
            prev.replace_filename(prev.stem().string() + "_prev" + prev.extension().string());
            vk.request_screenshot(prev);
        }
        if (!opts.screenshot.empty() && opts.frames && frame == opts.frames - 1) {
            vk.screenshot_generated(opts.screenshot_generated);
            vk.request_screenshot(opts.screenshot);
        }
        if (window.input().was_pressed(platform::Key::Escape)) break;
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        double t = std::chrono::duration<double>(now - start).count();
        if (opts.frame_dt > 0.0f) {
            dt = opts.frame_dt;
            t = frame * static_cast<double>(opts.frame_dt);
        }
        actors.set_time(t);

        const auto& in = window.input();
        // A timed run (--frames: a test, a screenshot) takes no input: whatever the person at the computer types or
        // moves while the window has the focus would move the camera.
        const bool interactive = opts.frames == 0;
        window.set_mouse_captured(interactive && in.right_button);
        if (interactive && in.right_button) camera.rotate(-in.mouse_dx * 0.0025f, -in.mouse_dy * 0.0025f);
        glm::vec3 move(0.0f);
        if (in.is_down(platform::Key::W)) move += camera.forward();
        if (in.is_down(platform::Key::S)) move -= camera.forward();
        if (in.is_down(platform::Key::D)) move += camera.right();
        if (in.is_down(platform::Key::A)) move -= camera.right();
        if (in.is_down(platform::Key::Space)) move.z += 1.0f;
        if (in.is_down(platform::Key::LeftCtrl)) move.z -= 1.0f;
        if (interactive && glm::dot(move, move) > 0.0f) camera.position += glm::normalize(move) * 20.0f * dt;
        if (opts.orbit != 0.0f && orbit_radius > 0.0f) {
            const float a = orbit_start + glm::radians(opts.orbit) * static_cast<float>(t);
            camera.position = glm::vec3(orbit_centre.x + orbit_radius * std::cos(a),
                                        orbit_centre.y + orbit_radius * std::sin(a), camera.position.z);
            camera.yaw = a + glm::pi<float>(); // facing the centre
        }

        for (auto& a : draws) a.time = a.upper_time = opts.anim_time.value_or(static_cast<float>(t));
        if (opts.spin != 0.0f)
            for (auto& a : draws) a.yaw = game::heading_to_yaw(opts.heading) + glm::radians(opts.spin) * static_cast<float>(t);
        if (opts.ride)
            for (std::size_t i = 1; i < draws.size(); i += 2) {
                bool seated = render::ActorRenderer::seat_rider(draws[i - 1], draws[i], &heights_data);
                if (frame == 0) {
                    const auto& mm = *draws[i - 1].model;
                    int bone = mm.mount_point_bone(draws[i].model->code);
                    log::info("ride: {} on {}: dag 0xe {} ({}), seated {}, rider origin z {:.3f} (mount origin z {:.3f})",
                              draws[i].model->code, mm.code, bone,
                              bone >= 0 ? mm.bone_names[static_cast<std::size_t>(bone)] : std::string("none"), seated,
                              draws[i].position.z, draws[i - 1].position.z);
                }
            }
        emitter_actors.clear();
        for (std::size_t i = 0; i < draws.size(); ++i)
            if (draws[i].mesh && !draws[i].mesh->emitters.empty())
                emitter_actors.push_back({static_cast<std::uint32_t>(i + 1), draws[i].mesh,
                                          render::ActorRenderer::standing(draws[i]), draws[i].alpha});
        actor_emitters.update(dt, emitter_actors, [&](std::uint32_t id, int bone, glm::mat4& w) {
            return id >= 1 && id <= draws.size() && render::ActorRenderer::bone_world(draws[id - 1], bone, w);
        }, camera.position, [&](std::uint32_t id, std::string_view code, float& time) {
            return id >= 1 && id <= draws.size() && render::ActorRenderer::playing(draws[id - 1], code, time);
        });
        if (particle_renderer) {
            if (t >= next_effect) {
                next_effect = t + 3.0;
                for (std::size_t i = 0; i < draws.size(); ++i) {
                    game::StageActor actor;
                    actor.id = static_cast<std::uint32_t>(i + 1);
                    actor.position = draws[i].position;
                    actor.level = 100;
                    actor.height = opts.size;
                    actor.points = spell_points.of(*draws[i].model).mask;
                    if (auto chest = point_world(i, 1)) actor.chest = glm::vec3((*chest)[3]);
                    actor.particle_scale = heights_data.pes_factor(draws[i].model->code);
                    for (const auto& r : game::spawn_stage_emitters(effects[static_cast<std::size_t>(opts.spell_effect)],
                                                                    opts.spell_stage, 1, &actor, &actor, actor.position,
                                                                    2500, {}, true))
                        particles.create(r);
                }
            }
            particles.update(dt, [&](std::uint32_t id, std::int32_t point) -> std::optional<game::EmitterAttachment> {
                if (id == 0 || id > draws.size()) return std::nullopt;
                const auto& draw = draws[id - 1];
                game::EmitterAttachment a;
                a.particle_scale = heights_data.pes_factor(draw.model->code);
                a.actor_axes = glm::mat3(render::ActorRenderer::draw_matrix(draw));
                if (auto m = point_world(id - 1, point)) {
                    a.position = glm::vec3((*m)[3]);
                    a.axes = glm::mat3(*m);
                } else if (point >= 0) a.position = draw.position;
                return a;
            });
            particle_renderer->prepare(particles);
        }

        auto f = vk.begin_frame_commands();
        if (!f) continue;
        int width = static_cast<int>(f->extent.width), height = static_cast<int>(f->extent.height);
        if (opts.modern) {
            // The models and the zone in the modern lighting, with the sun's shadows.
            std::vector<game::LightSource> effect_lights;
            auto add_actor_lights = [&](const game::ParticleSystem& sim, bool cloud) {
                auto lights = game::particle_lights(sim, [&](const std::string& texture) {
                    return actor_emitters.texture_color(texture, cloud);
                }, 4);
                effect_lights.insert(effect_lights.end(), lights.begin(), lights.end());
            };
            add_actor_lights(actor_emitters.simulation(), false);
            add_actor_lights(actor_emitters.cloud_simulation(), true);
            auto lighting = render::make_scene_lighting(game::SceneLight{}, effect_lights, effect_lights.size(),
                                                        render::LightingMode::Modern, sky);
            lighting.params.w = 0.04f;
            lighting.probe = probe;
            bool shadows = vk.shadows_enabled() && sky.light != glm::vec3(0.0f);
            if (shadows)
                render::set_shadows(lighting,
                                    render::fit_shadow_cascades(camera.shadow_camera(width, height), sky.to_light,
                                                                std::min(render::kShadowDistance, view.far_plane),
                                                                render::kShadowCasterReach, vk.shadow_size()),
                                    vk.shadow_size(), camera.position, camera.forward());
            render::set_haze(lighting, camera.position, std::min(view.fog_end, view.far_plane), view.fog_color);
            actors.set_lighting(lighting);
            if (zone_renderer) zone_renderer->set_lighting(lighting);
            actors.prepare(*f, draws);
            if (shadows)
                for (std::uint32_t c = 0; c < render::kShadowCascades; ++c) {
                    vk.begin_shadow_cascade(*f, c);
                    if (zone_renderer) zone_renderer->draw_shadow(*f, c);
                    actors.draw_shadow(*f, c);
                    vk.end_shadow_cascade(*f);
                }
        }
        // The plain camera first: begin_scene hands it to the scene's shaders for their own motion; the draws go
        // through the temporal upscaler's jitter (none without it).
        const glm::mat4 plain_proj = camera.projection(width, height, view.far_plane);
        vk.set_temporal_view(camera.view(), plain_proj);
        vk.begin_scene(*f, glm::vec4(view.fog_color, 1.0f));
        if (opts.modern) vk.set_ao_view(camera.ao_view(width, height, view.far_plane));
        glm::mat4 vp = render::jittered(plain_proj, vk.projection_jitter()) * camera.view();
        if (zone_renderer) zone_renderer->draw(*f, vp, camera.position, view, t);
        if (opts.modern)
            actors.draw_prepared(*f, vp, camera.position, view);
        else
            actors.draw(*f, vp, camera.position, view, draws);
        actor_emitters.draw(*f, vp, camera.view(), opts.modern ? render::kParticleGlow : 1.0f);
        if (particle_renderer) particle_renderer->draw(*f, vp, camera.view(), particles, opts.modern ? render::kParticleGlow : 1.0f);
        vk.end_frame(*f);
        if (frame == 1 || (opts.frames && frame == opts.frames - 1))
            log::info("actors: {} instances, {} draws, {} textures, {:.3f} ms CPU", actors.stats().instances,
                      actors.stats().draws, actors.stats().textures, actors.stats().cpu_ms);
    }
    return 0;
}

} // namespace hailstone::app
