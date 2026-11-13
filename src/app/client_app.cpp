// ClientApp: the main loop, and run_client.

#include "app/client_app_internal.h"
#include "app/frame_pace.h"
#include "core/build_info.h"
#include "core/frame_sections.h"
#include "core/wipe.h"

namespace hailstone::app {

namespace client_internal {

int ClientApp::run() {
    if (quit_) return 0; // the asset-folder picker was quit: nothing else was set up
    login_view_.version = build_version();
    start_updater();
    start_addons();
    auto last = Clock::now();
    auto start = last;
    auto last_drawn = last;
    float fps = 0.0f;
    // The game clock (core/step_clock.h): the rules run at 60 steps a second; game.fixed_step false steps once a
    // drawn frame instead, as the original's code is written (docs/mechanics.md "Frame rate").
    step_clock_.set_fixed(config_.get_bool("game.fixed_step", true));
    for (int frame = 0; !quit_ && window_.poll_events(); ++frame) {
        if (opts_.frames && frame >= opts_.frames) break;
        retired_ui_textures_.reset(); // the last frame, which may have drawn it, is submitted (waits for the GPU)
        perf_ui_.begin_frame();
        // The frame cap (display.max_fps, --max-fps), and display.background_fps while another window has the focus:
        // below 10 there, the loop runs on at 10 a second and draws only some frames, or none (app/frame_pace.h).
        const FramePace pace = frame_pace(opts_.max_fps, background_fps_, window_.focused(),
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - last_drawn));
        if (pace.cap > 0) std::this_thread::sleep_until(last + std::chrono::microseconds(1000000 / pace.cap));
        perf_ui_.mark(FramePhase::Sleep);
        if (!opts_.screenshot.empty() && opts_.frames && frame == opts_.frames - 1) vk_.request_screenshot(opts_.screenshot);
        screenshot_this_frame_ = false;
        if (window_.input().key_pressed[0xb7] && print_screen_takes_screenshot()) take_screenshot();
        change_asset_folder_at_login();
        if (window_.take_close_request()) window_close_request();
        // A close request held through a loading screen reaches the window procedure once a screen pumps messages.
        if (close_pending_ && screen_ != Screen::Loading) {
            close_pending_ = false;
            window_close_request();
        }
        if (quit_) break;
        auto now = Clock::now();
        float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;
        if (dt > 0.0f) fps = fps * 0.95f + (1.0f / dt) * 0.05f;
        double t = std::chrono::duration<double>(now - start).count();
        frame_steps_ = step_clock_.advance(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - start_time_).count()));
        if (conn_) {
            // The steps on the connection's clock, for its timers that run once a frame (memorizing, the ticks).
            std::optional<std::vector<std::int64_t>> steps;
            if (step_clock_.fixed()) {
                const std::int64_t conn_now = net::GameConnection::now_ms();
                const auto real_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
                steps.emplace();
                for (const auto& st : frame_steps_)
                    steps->push_back(conn_now - std::max<std::int64_t>(0, real_ms - static_cast<std::int64_t>(st.ms)));
            }
            conn_->set_game_steps(std::move(steps));
        }

        // The Update phase's sections, for the hitch log: a zone's load runs in update_connection, a new race's
        // model is built in the spawns' smoothing (CreatePlayerActor's model choice).
        core::frame_sections::Lap update_lap;
        box_clients_.poll();
        update_updater();
        update_addons();
        update_login();
        update_lap.mark("update login, updater, add-ons");
        update_connection();
        update_lap.mark("update connection");
        close_orphan_tabs();
        if (browser_)
            browser_->poll(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count()));
        update_window_title();
        // ProcessMouseInput @0x5f9e30: without MouseRightHanded (0xddf744) the buttons trade places (from the next
        // events on).
        window_.set_swap_mouse_buttons(conn_ && !conn_->client_option(game::ClientOption::MouseRightHanded));
        update_lap.mark("browser, window");
        if (automation_) automation_->frame();
        update_lap.mark("automation");
        if (conn_ && world_objects_ && screen_ == Screen::InGame && player_ready_) {
            world_objects_->update(*conn_);
            // A door that came or moved (a portal stone's base is one) is floor for ground_npcs.
            if (world_objects_->floor_generation() != grounded_doors_) {
                grounded_doors_ = world_objects_->floor_generation();
                conn_->reground();
            }
            update_missiles();
        }
        update_lap.mark("world objects update");
        // The gamepad before the UI frame: its mode decides what ImGui gets from it (docs/gamepad.md).
        update_gamepad(dt);
        // /safelock: the windows get no mouse while the interface is locked (__ProcessMouseEvents).
        // Behind the loading screen (zone_settle_) neither do they.
        ui_.set_mouse_locked(conn_ && screen_ == Screen::InGame && (conn_->interface_locked() || zone_settling()));
        ui_.new_frame();
        update_lap.mark("gamepad, UI new frame");
        // The boats move first: the player is carried by, and collides with, a boat where it is drawn this frame.
        if (conn_) {
            // The other spawns' and the vehicles' physics runs once per step of the game clock (core/step_clock.h).
            std::optional<std::vector<float>> steps;
            if (step_clock_.fixed()) {
                steps.emplace();
                for (const auto& st : frame_steps_) steps->push_back(static_cast<float>(st.elapsed_ms));
            }
            const auto completed_time = frame_steps_.empty() ? std::optional<std::uint32_t>{}
                : std::optional<std::uint32_t>{static_cast<std::uint32_t>(frame_steps_.back().ms + 1)};
            conn_->set_physics_steps(std::move(steps), completed_time);
            conn_->smooth_vehicles(dt);
        }
        if (screen_ == Screen::InGame && player_ready_) update_player(dt);
        update_lap.mark("player update");
        if (conn_) {
            // Moving spawns follow the floor between server updates.
            game::WorldState::FloorFn floor;
            if (collision_)
                floor = [this](const glm::vec3& p) { return collision_->floor_below(p, 4.0f); };
            // CreatePlayerActor once a spawn's actor can be made (the zone is in): whether its model is the
            // Human fallback, and whether it has a tunic point (InitAnimationTracks' +0x34) for the height fix-up.
            game::WorldState::ActorModelFn actor_model;
            if (actor_renderer_ && screen_ == Screen::InGame)
                actor_model = [this](const game::Spawn& s) -> std::optional<game::WorldState::ActorModel> {
                    auto choice = actor_renderer_->choose_model(s);
                    return game::WorldState::ActorModel{
                        choice.model && choice.model->find_bone("TUNIC_POINT_TRACK") >= 0, choice.human_fallback};
                };
            game::WorldState::RegionFn region;
            if (zone_ && !zone_->regions.empty())
                region = [this](const glm::vec3& p) {
                    return static_cast<std::uint8_t>(zone_->regions.lookup(p).flags & 0xff);
                };
            // Another spawn an update puts on a boat's deck rides it (game/boats.h).
            game::WorldState::VehicleFn vehicle_under;
            if (!vehicles_.empty())
                vehicle_under = [this](const glm::vec3& p) {
                    ZoneMoveWorld decks(nullptr, nullptr, nullptr);
                    decks.set_vehicles(&vehicles_);
                    return decks.vehicle_under(p);
                };
            // ground_npcs measures against the doors too (Plane of Knowledge's portal stones stand on a door's base),
            // as the player's floor test does; the landing check above keeps the zone's floor alone.
            game::WorldState::FloorFn ground_floor;
            if (collision_) ground_floor = [this](const glm::vec3& p) { return grounding_floor(p); };
            // The other spawns' ladder contact (remote_collision_type), built with the player's passable-ladder rule
            // (ZoneMoveWorld::apply_remote_rules; not the doors the player's /nav passes) and in the contact-probe mode, which reuses one door buffer. Only spawns
            // within the actor clip are probed (WorldState::remote_probe_range): farther ones are not drawn.
            game::WorldState::RemoteCollisionFn remote_collision;
            std::optional<ZoneMoveWorld> remote_world;
            if (collision_ && screen_ == Screen::InGame) {
                remote_world.emplace(&*collision_, world_objects_.get(), zone_ ? &zone_->regions : nullptr);
                remote_world->apply_remote_rules(conn_->server_rules());
                remote_world->set_contact_probe(&remote_door_hits_);
                remote_collision = [&remote_world](const game::Spawn& spawn, const glm::vec3& from,
                                                   const glm::vec3& to) {
                    return remote_collision_type(spawn, from, to, *remote_world);
                };
                conn_->set_remote_probe_range(game::actor_clip_distance(actor_clip_plane_, view_.far_plane));
            }
            conn_->smooth(dt, floor, actor_model, region, vehicle_under, ground_floor, remote_collision);
            trace_spawn(dt);
        }
        update_lap.mark("spawn smoothing");
        // Ctrl+Shift+D: the developer windows (info panel, spawn list, "Lua UI") on or off.
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_D)) {
            game_view_.dev_windows = !game_view_.dev_windows;
            if (lua_ui_) lua_ui_->set_show_control(game_view_.dev_windows);
        }
        perf_ui_.mark(FramePhase::Update);
        core::frame_sections::Lap lap;
        sync_item_preview();
        lap.mark("item preview sync");
        draw_ui(fps);
        lap.mark("draw_ui");
        if (lua_ui_) {
            update_eq_host();
            sync_plugin_windows();
            lap.mark("eq host update");
            eq_api_->begin_frame();
            lap.mark("eq api begin_frame");
            lua_ui_->frame();
            eq_host_.close_top_window = false; // (a press no window took this frame)
            lap.mark("Lua UI frame");
        }
        window_.set_text_input(std::exchange(web_text_input_, false)); // a page that has the keys (app/lua_eq_web.cpp)
        settle_frame_.ui = true; // the UI pack's in-game frame ran (or there is none)
        if (screen_ == Screen::InGame && zone_settling()) ui::loading_overlay(loading_view_);
        if (automation_) automation_->imgui(game_view_.dev_windows);
        // The select clock runs on through character creation (input scripts drive both screens).
        if (screen_ != Screen::Characters && screen_ != Screen::Create) select_shown_.reset();
        else if (!select_shown_) select_shown_ = Clock::now();
        if (input_script_)
            input_script_->frame(screen_ == Screen::InGame && player_ready_ && !zone_settling()
                                     ? std::chrono::duration<double>(Clock::now() - zone_entered_).count()
                                     : -1.0,
                                 select_shown_ ? std::chrono::duration<double>(Clock::now() - *select_shown_).count()
                                               : -1.0,
                                 link_death_since_
                                     ? std::chrono::duration<double>(Clock::now() - *link_death_since_).count()
                                     : -1.0);
        {
            PerfInputs perf;
            if (zone_renderer_) perf.zone = &zone_renderer_->stats();
            if (actor_renderer_) perf.actors = &actor_renderer_->stats();
            perf.conn = conn_.get();
            perf.gpu = &vk_;
            if (conn_) {
                const auto& w = conn_->world();
                perf.zone_name = w.zone_long_name.empty() ? w.zone_short_name : w.zone_long_name;
                perf.spawns = w.spawns.size();
            }
            perf.camera = camera_.position;
            perf.display = glm::vec2(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
            perf_ui_.draw(perf);
            lap.mark("perf window");
        }
        perf_ui_.mark(FramePhase::Ui);

        if (conn_) {
            const auto& tod = conn_->time_of_day();
            if (tod && (!sky_time_seen_ || tod->hour != sky_time_seen_->hour || tod->minute != sky_time_seen_->minute ||
                        tod->day != sky_time_seen_->day || tod->month != sky_time_seen_->month)) {
                sky_clock_.set(tod->hour, tod->minute, t, tod->day, tod->month);
                if (!sky_time_seen_) log::info("game time {}:{:02}, sky hour {:.2f}", tod->hour, tod->minute, sky_clock_.hour(t));
                sky_time_seen_ = tod;
            }
        }
        bool in_world = screen_ == Screen::InGame && player_ready_;
        // Character select and creation: clz with the model player at noon (app/client_app_pregame.cpp).
        update_pre_game(dt);
        const bool pre_game = pre_game_scene();
        float sky_hour = pre_game ? 12.0f : sky_clock_.hour(t);
        sky_hour_ = sky_clock_.valid() ? sky_hour : -1.0f;
        moon_phase_ = opts_.moon_phase >= 0.0f ? opts_.moon_phase : sky_clock_.moon_phase(t);
        {
            float blood = opts_.blood_moon >= 0.0f ? opts_.blood_moon : sky_clock_.blood_moon(t);
            // The blood moon's coming, said once (it is the modern lighting's own, as is its look).
            if (blood >= 0.5f && blood_moon_ < 0.5f && conn_ && lighting_mode_ == render::LightingMode::Modern)
                conn_->show_chat("The moon turns the color of blood.");
            blood_moon_ = blood;
        }
        if (audio_) {
            auto now_ms = static_cast<std::uint32_t>(t * 1000.0) + 1;
            if (in_world && conn_) {
                update_listener();
                audio_->set_hour(static_cast<int>(sky_hour));
                sound_events_->update(*conn_, motion_, now_ms);
                motion_.jumped = motion_.land_sound = false;
                motion_.fall_damage.reset();
            }
            update_music();
            audio_->update(now_ms);
        }
        if (zone_particles_ && (in_world || pre_game)) zone_particles_->update(dt, camera_.position);
        if (spell_particles_ && in_world && conn_) {
            auto cues = conn_->take_spell_effect_cues();
            auto effect_packets = conn_->take_effect_packets();
            if (!spell_effects_) { // /showspelleffects off: none start (the removals still stop what is shown)
                cues.clear();
                std::erase_if(effect_packets, [](const net::EffectPacketCue& e) {
                    return e.kind == net::EffectPacketCue::Kind::Add || e.kind == net::EffectPacketCue::Kind::AddNimbus;
                });
            }
            const double bolt_now = bolt_clock();
            if (spell_bolts_ && lighting_mode_ == render::LightingMode::Modern)
                spell_bolts_->take_cues(*conn_, cues, bolt_now);
            update_guide();
            spell_particles_->set_guide_line(guide_.line());
            spell_particles_->set_find_trail_style(game::normalize_find_trail_style(
                config_.get_int("display.find_trail_style", 0)));
            spell_particles_->update(*conn_, cues, effect_packets, actor_renderer_.get(), dt);
            spell_particles_->prepare();
            if (spell_bolts_)
                spell_bolts_->update(*conn_, bolt_now, [this](std::uint32_t serial) -> std::optional<glm::vec3> {
                    for (const auto& m : missiles_.missiles())
                        if (m.serial == serial && !m.done) return m.has_actor ? m.actor_position : m.position;
                    return std::nullopt;
                });
        }
        if (actor_emitters_ && in_world && conn_ && actor_renderer_) {
            // The drawn spawns' own emitters, on the pose drawn last frame.
            actor_emitter_list_.clear();
            for (const auto& [id, spawn] : conn_->world().spawns)
                if (const auto* mesh = actor_renderer_->drawn_mesh(id); mesh && !mesh->emitters.empty())
                    actor_emitter_list_.push_back({id, mesh, actor_renderer_->drawn_standing(id),
                                                   actor_renderer_->drawn_alpha(id)});
            // The missiles' (a spell bolt's flames: its model's own emitters, app/world_objects.h).
            const WorldObjects* objects = world_objects_.get();
            if (objects)
                for (const auto& m : objects->missile_actors()) actor_emitter_list_.push_back({m.id, m.emitters});
            const render::ActorRenderer* actors = actor_renderer_.get();
            actor_emitters_->update(dt, actor_emitter_list_,
                                    [actors, objects](std::uint32_t id, int bone, glm::mat4& w) {
                                        if (id & WorldObjects::kMissileActorBit)
                                            return objects && bone == 0 && objects->missile_transform(id, w);
                                        return actors->drawn_bone(id, bone, w);
                                    },
                                    camera_.position,
                                    [actors](std::uint32_t id, std::string_view code, float& time) {
                                        return actors->drawn_playing(id, code, time);
                                    });
        }
        float brightness = (in_world || pre_game) && outdoor_light_ ? render::outdoor_brightness(sky_hour) : 1.0f;
        std::optional<game::SceneFogFrame> fog_frame;
        if (in_world || pre_game) {
            fog_frame = scene_fog_frame(sky_hour, t);
            view_ = fog_frame->view;
            // The isometric camera stands back from the player: the fog and the far plane are measured from the
            // player (game::iso_view), so as much of the zone shows round them as in the other cameras.
            if (iso()) view_ = game::iso_view(view_, iso_focus_distance_);
        }
        // What the renderers draw with: the original's view, or in the modern mode the longer one with its haze
        // (mouse picking keeps the original's yon).
        const bool modern = lighting_mode_ == render::LightingMode::Modern;
        // The weather (OP_Weather; game/weather.h), in the open: zone time types 1, 2 and 5.
        const bool outdoor = time_type_ == 1 || time_type_ == 2 || time_type_ == 5;
        render::ModernView modern_view{view_, 0.0f};
        // The modern haze, eased between places and weathers (render/atmosphere.h): its reach, length and colour.
        render::HazeTarget haze;
        if (modern) {
            const bool extend = !fog_frame || fog_frame->sky;
            const render::Daylight sun = render::daylight(sky_hour, true, moon_phase_, blood_moon_);
            haze = haze_blend_.update(render::haze_target(view_, view_distance_, extend, outdoor,
                                                          sky_renderer_ != nullptr && extend, sun.to_sun,
                                                          weather_look_.storm, weather_look_.snowing),
                                      dt);
            modern_view = render::modern_view(view_, view_distance_, haze.extend);
        }
        const game::ViewDistance& lit = modern_view.view;
        glm::vec4 horizon_haze(0.0f); // the sky's blend into the haze (render::SkyFrame)
        perf_ui_.mark(FramePhase::Prepare);
        update_view_area();
        // A frame not drawn (the background frame rate, or a minimised window) runs as one the GPU had no image for.
        const bool screenshot_frame = !opts_.screenshot.empty() && opts_.frames && frame == opts_.frames - 1;
        auto f = pace.draw || screenshot_frame ? vk_.begin_frame_commands() : std::nullopt;
        perf_ui_.mark(FramePhase::Wait);
        if (f) last_drawn = now;
        if (!f) {
            ui_.end_frame_without_render();
            continue;
        }
        lap = core::frame_sections::Lap();
        const bool in_game = screen_ == Screen::InGame && player_ready_;
        const bool scene = in_game || pre_game; // the world drawn: the zone, or character select's clz
        if (conn_)
            for (const auto& w : conn_->take_weather()) weather_.handle_message(w.kind, w.intensity);
        if (in_game) {
            auto random = [this] { return std::uniform_int_distribution<std::int32_t>(0, 0x7fff)(weather_random_); };
            const std::uint16_t zone_id = conn_ ? conn_->zone_id() : std::uint16_t{0};
            // LightningStrike plays the thunder with the flash, WavePlay(id, 0); ApplyWeatherState shows the start.
            auto weather_step = [&](double at) {
                weather_.update(at, outdoor, random, zone_id);
                if (std::int32_t sound = weather_.thunder(); sound && audio_) audio_->play_wave_2d(sound);
                if (std::uint32_t message = weather_.message(); message && conn_) conn_->show_eqstr(message);
            };
            // Once per step of the game clock (core/step_clock.h): the fog band's fade is a fixed step an update.
            if (step_clock_.fixed())
                for (const auto& st : frame_steps_) weather_step(static_cast<double>(st.ms) / 1000.0);
            else
                weather_step(t);
            weather_look_.update(weather_, dt, zone_id);
            update_precipitation(dt, sky_hour);
        }
        const int width = static_cast<int>(f->extent.width), height = static_cast<int>(f->extent.height);
        // The modern lighting's water with the camera in it (render::set_underwater), lit by the open sky's daylight.
        const bool under_water = in_game && modern && assets::is_water_type(camera_region_);
        if (under_water) haze_blend_.reset(); // out of the water the haze is there at once
        glm::vec3 water_color(0.0f);
        if (under_water) {
            render::Daylight above = render::daylight(sky_hour, outdoor, moon_phase_, blood_moon_);
            render::apply_storm(above, weather_look_.storm);
            water_color = render::underwater_color(above);
        }
        if (scene) {
            // The lighting, and the sun's shadow cascades, come before the scene.
            {
                game::SceneLightInput light;
                light.time_type = time_type_;
                light.hour = sky_hour;
                if (zone_) light.global_ambient = zone_->global_ambient;
                if (fog_frame) {
                    light.player_light = fog_frame->light_level;
                    light.ambient_colour = fog_frame->ambient;
                    light.special_colour = fog_frame->special_ambient;
                    light.vision_lift = fog_frame->vision_lift;
                }
                game::SceneLight scene_light = game::compute_scene_light(light);
                if (zone_renderer_) zone_renderer_->set_scene_light(scene_light);
                // modern or classic (/lighting, display.lighting): the same scene light for every renderer.
                render::DynamicShadowLights moving_lights;
                render::SceneLighting lighting = scene_lighting(scene_light, sky_hour, &moving_lights);
                // The isometric camera's cut through what stands in front of the player (game/iso_occlusion.h).
                {
                    const game::IsoReveal r = iso_reveal();
                    lighting.reveal_origin = r.origin;
                    lighting.reveal_axis = r.axis;
                    lighting.reveal_shape = r.shape;
                    lighting.reveal_roof = r.roof;
                }
                bool shadows = lighting_mode_ == render::LightingMode::Modern && vk_.shadows_enabled() &&
                               zone_renderer_ && glm::vec3(lighting.light) != glm::vec3(0.0f);
                if (shadows) {
                    // The isometric camera's cascades start a little short of the player, not at the lens.
                    render::ShadowCamera shadow_view = camera_.shadow_camera(width, height);
                    float shadow_distance = render::kShadowDistance;
                    if (iso()) {
                        shadow_view.near_plane = game::iso_shadow_near(iso_focus_distance_, shadow_view.near_plane);
                        shadow_distance += iso_focus_distance_;
                    }
                    auto cascades = render::fit_shadow_cascades(
                        shadow_view, glm::vec3(lighting.to_light), std::min(shadow_distance, lit.far_plane),
                        render::kShadowCasterReach, vk_.shadow_size());
                    render::set_shadows(lighting, cascades, vk_.shadow_size(), camera_.position, camera_.forward());
                }
                // Under the rain or snow, the zone seen from above: what is covered stays dry (render/cover_map.h).
                const bool covering = modern && outdoor && vk_.shadows_enabled() && zone_renderer_ &&
                                      (weather_.precipitation() > 0.0f || weather_look_.wet > 0.0f ||
                                       weather_look_.snow > 0.0f);
                // The water surfaces seen from above: what lies under them (render/cover_map.h, the caustics and the
                // shafts under water).
                const bool watering = modern && vk_.shadows_enabled() && zone_renderer_ && zone_renderer_->has_water();
                if (covering || watering) {
                    cover_ = render::fit_cover_map(camera_.position, vk_.shadow_size());
                    render::set_cover(lighting, cover_, covering, watering);
                }
                {
                    // The light of the place the player stands in, for the characters (and in the modern mode the
                    // doors) round them, in either mode; before the game, of the place the model stands in.
                    glm::vec3 probe_at = camera_.position;
                    if (in_game) probe_at = feet_ + glm::vec3(0.0f, 0.0f, floor_offset_ + 3.0f);
                    else if (pre_game)
                        if (auto it = pre_game_world_->spawns.find(kModelPlayerId); it != pre_game_world_->spawns.end())
                            probe_at = it->second.position + glm::vec3(0.0f, 0.0f, 3.0f);
                    lighting.probe = probe_blend_.update(ambient_probe_.sample(probe_at), dt);
                }
                if (modern) {
                    lighting.wind = render::wind_at(t, weather_look_.storm);
                    // A storm closes the distance in; under water, the water does.
                    if (under_water)
                        render::set_underwater(lighting, camera_.position, water_color);
                    else
                        render::set_haze_linear(lighting, camera_.position, haze.length,
                                                render::lightning_haze(haze.color, weather_look_.flash, weather_look_.storm));
                    render::set_screen(lighting, camera_.ao_view(width, height, lit.far_plane).near_plane, lit.far_plane,
                                       f->extent.width, f->extent.height, t);
                    render::set_weather(lighting, weather_look_);
                    horizon_haze = glm::vec4(glm::vec3(lighting.haze_color), 1.0f);
                }
                // The lights' shadows: which lights have a cube (the zone's, the carried light, the spells'), before the
                // renderers take the lighting.
                const bool light_shadows = modern && zone_ && zone_renderer_ && vk_.point_shadow_slots() > 0;
                std::vector<std::uint32_t> point_builds;
                // The doors and ground objects as they stand this frame (WorldObjects::update ran), so they can
                // cast shadows: a door swinging in a light's reach keeps its slot live (render/point_shadow_pass.h).
                if (world_objects_) world_objects_->prepare(*f);
                lap.mark("objects prepare");
                render::ObjectRenderer* objects = world_objects_ ? world_objects_->renderer() : nullptr;
                // The flames: the zone's, then the doors' and objects' as they stand (the modern lighting's own).
                if (zone_renderer_) zone_renderer_->set_hide_flames(modern);
                if (objects) objects->set_hide_flames(modern);
                frame_fire_list_ = {};
                if (modern) {
                    // The zone's list as it is, copied only when the doors and objects add theirs.
                    const auto& zone_list = environment_particle_density_ > 0.0f ? zone_fires_ : zone_mesh_fires_;
                    frame_fire_list_ = zone_list;
                    moving_fires_.clear();
                    if (objects) objects->prepared_fires(moving_fires_);
                    if (!moving_fires_.empty()) {
                        frame_fires_.assign(zone_list.begin(), zone_list.end());
                        for (const auto& fire : moving_fires_) render::add_fire_sources(fire, frame_fires_);
                        frame_fire_list_ = frame_fires_;
                    }
                }
                if (light_shadows) {
                    render::sync_point_shadows(point_shadows_, vk_);
                    point_shadows_.set_dynamic(moving_lights, camera_.position);
                }
                if (light_shadows)
                    point_builds = render::plan_point_shadows(point_shadows_, vk_, zone_->lights, camera_.position, dt,
                                                              actor_renderer_.get(), lighting, objects);
                lap.mark("plan point shadows");
                settle_frame_.shadow_builds = point_builds.size();
                if (zone_renderer_) zone_renderer_->set_lighting(lighting);
                // The zone's exits for the map and its zone finder (EqHost::zone_exits), once its zone points are in.
                if (in_game && zone_ && conn_ && eq_host_.zone_exits_zone == 0 && conn_->zone_id() != 0 &&
                    !conn_->zone_points().empty()) {
                    std::vector<game::ZonePointTarget> points;
                    for (const auto& p : conn_->zone_points()) points.push_back({p.number, p.zone_id, p.x, p.y, p.z});
                    const auto zid = static_cast<std::uint32_t>(conn_->zone_id());
                    eq_host_.zone_exits.clear();
                    for (const auto& g : game::collect_neighbour_lines(zone_->regions, zone_->bounds_min, zone_->bounds_max,
                                                                       points, zid)) {
                        EqHost::ZoneExit e;
                        e.zone_id = g.zone_id;
                        for (int i = 0; i < 3; ++i) {
                            e.lo[i] = g.lo[i];
                            e.hi[i] = g.hi[i];
                        }
                        for (const auto& c : g.cells) e.cells.push_back({c.lo.x, c.lo.y, c.lo.z, c.hi.x, c.hi.y, c.hi.z});
                        eq_host_.zone_exits.push_back(std::move(e));
                    }
                    eq_host_.zone_exits_zone = zid;
                }
                if (neighbours_) {
                    // The zone past each outdoor zone line, once the zone points are in (app/neighbour_zones.h).
                    if (in_game && zone_ && conn_ && !neighbours_->entered()) {
                        NeighbourZones::ZoneIn in;
                        in.zone = &*zone_;
                        in.collision = collision_ ? &*collision_ : nullptr;
                        for (const auto& p : conn_->zone_points())
                            in.points.push_back({p.number, p.zone_id, p.x, p.y, p.z});
                        in.zone_id = static_cast<std::uint32_t>(conn_->zone_id());
                        const auto& z = conn_->zone_info();
                        in.outdoor = z && z->sky != 0 && outdoor;
                        neighbours_->enter(in);
                    }
                    neighbours_->update(camera_.position, lit.far_plane, modern && neighbour_zones_ && in_game);
                    lap.mark("neighbour zones update");
                    neighbours_->set_scene_light(scene_light);
                    neighbours_->set_lighting(lighting);
                }
                frame_lighting_ = lighting;
                if (actor_renderer_) actor_renderer_->set_lighting(lighting);
                if (world_objects_) {
                    world_objects_->set_lighting(lighting);
                    world_objects_->set_time(t); // the zone renderer's clock: flames on doors flicker in step
                }
                // The spawns are animated and posed now, so they can cast shadows; drawn in the scene below.
                if (actor_renderer_) {
                    // The original's Yon, not the modern view's longer one: which spawns appear stays the same.
                    actor_renderer_->set_actor_clip(game::actor_clip_distance(actor_clip_plane_, view_.far_plane));
                    update_market_hidden(in_game);
                    actor_renderer_->set_hidden_spawns(&market_hidden_);
                    actor_renderer_->set_shadow_reach(camera_.position, game::shadow_clip_distance(shadow_clip_plane_));
                    // Only the spawns that can show are posed: in the view the scene is drawn with below, or within the
                    // shadow reach while a shadow (the sun's, a light's) can take them.
                    actor_renderer_->set_pose_view(camera_.projection(width, height, lit.far_plane) * camera_.view(),
                                                   shadows || light_shadows);
                    actor_renderer_->prepare_spawns(*f, camera_.position, pre_game ? *pre_game_world_ : conn_->world(),
                                                    pre_game ? pre_game_hidden_id() : hidden_spawn(), dt);
                    lap.mark("actors prepare_spawns");
                }
                if (shadows)
                    for (std::uint32_t c = 0; c < render::kShadowCascades; ++c) {
                        vk_.begin_shadow_cascade(*f, c);
                        zone_renderer_->draw_shadow(*f, c);
                        if (actor_renderer_) actor_renderer_->draw_shadow(*f, c);
                        if (world_objects_) world_objects_->draw_shadow(*f, c);
                        vk_.end_shadow_cascade(*f);
                    }
                lap.mark("sun shadows");
                // Each map only when what it shows has changed: its view, the zone, the doors and objects in it, the
                // plants swaying in it (render/cover_map.h). Its layer keeps its depths meanwhile.
                const bool draw_cover = render::cover_map_stale(
                    cover_state_, covering, cover_,
                    render::cover_contents(covering ? zone_renderer_->id() : 0, vk_.shadow_generation(),
                                           covering && objects ? objects->cover_signature() : 0),
                    covering && zone_renderer_->map_moves(render::kCoverLayer));
                const bool draw_water = render::cover_map_stale(
                    water_state_, watering, cover_,
                    render::cover_contents(watering ? zone_renderer_->id() : 0, vk_.shadow_generation(), 0),
                    watering && zone_renderer_->map_moves(render::kWaterLayer));
                for (std::uint32_t layer : {render::kCoverLayer, render::kWaterLayer}) {
                    if (!(layer == render::kCoverLayer ? draw_cover : draw_water)) continue;
                    vk_.begin_shadow_cascade(*f, layer);
                    zone_renderer_->draw_shadow(*f, layer);
                    if (world_objects_) world_objects_->draw_shadow(*f, layer);
                    vk_.end_shadow_cascade(*f);
                }
                cover_drawn_ = covering;
                lap.mark("cover and water maps");
                if (light_shadows)
                    render::draw_point_shadows(vk_, *f, point_shadows_, zone_->lights, point_builds, *zone_renderer_,
                                               actor_renderer_.get(), objects);
                lap.mark("draw point shadows");
            }
        }
        for (auto& slot : previews_)
            if (slot.view) slot.view->record(*f, dt); // the item previews' own passes (their images)
        if (web_textures_ && browser_) web_textures_->record(*f, *browser_); // the browser tabs' changes
        lap.mark("item preview record");
        // The loading screen's backdrop is black (ui::loading_overlay's too, while the zone settles behind it).
        vk_.begin_scene(*f, screen_ == Screen::Loading ? glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)
                                                       : glm::vec4(under_water ? water_color : lit.fog_color, 1.0f));
        if (scene && modern) {
            vk_.set_ao_view(camera_.ao_view(width, height, lit.far_plane));
            // The colour grade: the time of day, the weather and the zone's fog hue (render/atmosphere.h).
            render::Daylight day = render::daylight(sky_hour, outdoor, moon_phase_, blood_moon_);
            const auto g = render::color_grade(day, outdoor, render::linear_color(view_.fog_color), weather_look_.storm);
            vk_.set_color_grade(glm::vec4(g.tint, g.saturation));
        }
        // God rays from the sun, in the open under a sky (not under water or a storm's cover).
        if (scene && modern && outdoor && (!fog_frame || fog_frame->sky)) {
            render::Daylight sun = render::daylight(sky_hour, true, moon_phase_, blood_moon_);
            render::apply_storm(sun, weather_look_.storm);
            glm::mat4 vp = camera_.projection(width, height, lit.far_plane) * camera_.view();
            render::SunShafts shafts = render::sun_shafts_for(vp, sun.to_sun, sun.light * 0.35f);
            shafts.strength *= 1.0f - weather_look_.storm;
            vk_.set_sun_shafts(shafts);
        }
        if (scene) {
            glm::mat4 proj = camera_.projection(width, height, lit.far_plane);
            glm::mat4 vp = proj * camera_.view();
            // The sky is drawn while SetFog leaves CDisplay +0x2d38 set (cleared under water, in lava and with
            // fog ending before 101).
            render::SkyFrame sky_frame{sky_hour, t, horizon_haze};
            // The open sky's daylight under this weather, for the sky, its clouds and the rain.
            render::Daylight day = render::daylight(sky_hour, true, moon_phase_, blood_moon_);
            render::apply_storm(day, weather_look_.storm);
            if (modern) {
                sky_frame.atmosphere = true;
                sky_frame.to_sun = day.to_sun;
                sky_frame.to_moon = day.to_moon;
                sky_frame.blood_moon = day.blood_moon;
                sky_frame.aurora = opts_.aurora >= 0.0f ? opts_.aurora : sky_clock_.aurora(t);
                sky_frame.cloud_light = render::cloud_light(day) + glm::vec3(weather_look_.flash);
                sky_frame.storm = weather_look_.storm;
                sky_frame.flash = weather_look_.flash;
            }
            const bool sky_drawn = sky_renderer_ && (!fog_frame || fog_frame->sky);
            if (sky_drawn) sky_renderer_->draw(*f, proj, camera_.view(), sky_frame);
            lap.mark("sky");
            // Where no sky is drawn, the modern haze fills the background as it fills the distance (with the sun's glow
            // in it), not the plain fog colour; under water, the water.
            if (zone_renderer_ && (under_water || (modern && !sky_drawn))) zone_renderer_->draw_backdrop(*f, vp);
            // The modern water reads the opaque world: the zone's other draws wait for VkContext::begin_translucent.
            const render::ZonePhase first = modern ? render::ZonePhase::Opaque : render::ZonePhase::All;
            if (zone_renderer_) zone_renderer_->draw_depth(*f, vp, camera_.position, lit, t);
            if (zone_renderer_) zone_renderer_->draw(*f, vp, camera_.position, lit, t, first);
            lap.mark("zone draw");
            const bool neighbours = neighbours_ && modern && neighbour_zones_ && in_game;
            if (neighbours) neighbours_->draw(*f, vp, camera_.position, lit, t, first);
            lap.mark("neighbour zones draw");
            if (world_objects_) {
                world_objects_->set_brightness(brightness);
                world_objects_->draw(*f, vp, camera_.position, lit);
            }
            lap.mark("objects draw");
            if (flora_renderer_ && grass_) {
                flora_renderer_->set_reach(flora_reach_);
                // The grass bends away from the spawns standing in it (Hailstone's own): the nearest to the camera
                // (in the isometric camera, which stands far off, to the player) within 150 units, by their feet,
                // round them by their size.
                auto& near = flora_near_;
                near.clear();
                const glm::vec3 near_to = iso() ? feet_ : camera_.position;
                if (in_game)
                    for (const auto& [id, s] : conn_->world().spawns) {
                        const glm::vec3 at = s.display_valid ? s.display_position : s.position;
                        const glm::vec2 off = glm::vec2(at) - glm::vec2(near_to);
                        if (!(glm::dot(off, off) <= 150.0f * 150.0f)) continue;
                        const glm::vec3 feet = at - glm::vec3(0.0f, 0.0f, s.heights.avatar_height);
                        near.emplace_back(glm::length(off), glm::vec4(feet, std::clamp(s.size * 0.7f, 3.0f, 12.0f)));
                    }
                // The nearest kMaxPushers, nearest first.
                const auto take = std::min(near.size(), render::FloraRenderer::kMaxPushers);
                const auto nearer = [](const auto& a, const auto& b) { return a.first < b.first; };
                std::partial_sort(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(take), near.end(), nearer);
                flora_pushers_.clear();
                for (std::size_t i = 0; i < take; ++i) flora_pushers_.push_back(near[i].second);
                flora_renderer_->set_pushers(flora_pushers_);
            }
            const std::unordered_set<std::uint32_t>* modelled = nullptr;
            // The isometric camera's silhouette of the player (and its mount), where the zone or an object still
            // hides them: before the characters, which cover it where the player is in sight.
            if (actor_renderer_ && in_game && iso() && iso_silhouette_)
                if (const game::Spawn* me = conn_->world().player()) {
                    const std::uint32_t ids[2] = {me->id, me->mount_id};
                    actor_renderer_->draw_silhouette(*f, vp, camera_.position,
                                                     std::span<const std::uint32_t>(ids, me->mount_id ? 2u : 1u),
                                                     glm::vec4(0.55f, 0.75f, 1.0f, 0.55f));
                }
            if (actor_renderer_) modelled = &actor_renderer_->draw_prepared(*f, vp, camera_.position, lit);
            lap.mark("actors draw");
            if (in_game) spawn_renderer_->draw(*f, vp, camera_.position, lit, conn_->world(), hidden_spawn(), modelled);
            lap.mark("spawns draw");
            // The depth copy is taken before the grass: the target ring lies on the ground under it, not on the
            // blades (a ring traced over them twists into a coil). The water, drawn after the grass, still meets it
            // in the depth buffer.
            if (modern || (in_game && (target_ring_shown() || iso_marker_shown() || (iso() && iso_hover_.shown))))
                vk_.begin_translucent(*f);
            if (flora_renderer_ && grass_) flora_renderer_->draw(*f, vp, camera_.position, t, frame_lighting_, modern);
            lap.mark("flora draw");
            if (modern) {
                if (zone_renderer_)
                    zone_renderer_->draw(*f, vp, camera_.position, lit, t, render::ZonePhase::Translucent);
                if (neighbours) neighbours_->draw(*f, vp, camera_.position, lit, t, render::ZonePhase::Translucent);
            }
            // The target ring lies on the depth copy (taken above, or by it in the classic lighting), under the
            // weather and particles. Character select draws no target.
            if (in_game) draw_target_ring(*f, vp, lit.far_plane, modern, t);
            if (in_game) draw_iso_marker(*f, vp, lit.far_plane, modern, t);
            if (in_game) draw_iso_hover(*f, vp, lit.far_plane, modern, t);
            // Rain or snow: the precipitation manager's particles. The original draws them in every zone type,
            // with or without a sky (CRender's scene, 0x10097420).
            if (weather_renderer_ && in_game) {
                const float pixel = 2.0f / (static_cast<float>(std::max(1, width)) * proj[0][0]);
                weather_renderer_->draw(*f, vp, camera_.position, camera_.forward(), pixel, weather_.particles(),
                                        1.0f + weather_look_.flash,
                                        modern ? render::kModernPrecipitation : render::kClassicPrecipitation);
            }
            // The modern particles read the depth copy (after begin_translucent above) and the frame's lighting.
            const render::SceneLighting* particle_lighting = modern ? &frame_lighting_ : nullptr;
            if (zone_particles_)
                zone_particles_->draw(*f, vp, camera_.view(), modern ? render::kParticleGlow : 1.0f, !modern,
                                      particle_lighting);
            if (modern && fire_renderer_)
                fire_renderer_->draw(*f, vp, camera_.position, t, frame_fire_list_, frame_lighting_,
                                     std::min(lit.far_plane, 1500.0f), render::kParticleGlow);
            if (modern && spell_bolts_ && in_game)
                spell_bolts_->draw(*f, vp, camera_.position, t, frame_lighting_, render::kParticleGlow);
            if (actor_emitters_ && in_game)
                actor_emitters_->draw(*f, vp, camera_.view(), modern ? render::kParticleGlow : 1.0f, particle_lighting);
            if (spell_particles_ && in_game)
                spell_particles_->draw(*f, vp, camera_.view(), lighting_mode_ == render::LightingMode::Modern ? render::kParticleGlow : 1.0f,
                                       particle_lighting);
            lap.mark("translucent, weather, particles");
            if (in_game) { // character select draws no target, names or picking
                if (nav_ && nav_->overlay())
                    nav_->draw_overlay(ImGui::GetBackgroundDrawList(), vp,
                                       glm::vec4(view_area_.x, view_area_.y, view_area_.w, view_area_.h));
                draw_nameplates(*f, vp, f->extent, lit.far_plane);
                lap.mark("nameplates");
                draw_text_overlay();
                lap.mark("text overlay");
                // The camera as CCamera::GetScreenRay reads it, in the 3D view's window (mouse) coordinates.
                auto fov = game::camera_fov(camera_.view_fov_units(), static_cast<int>(f->extent.width), static_cast<int>(f->extent.height));
                pick_camera_.position = camera_.position;
                pick_camera_.forward = camera_.forward();
                pick_camera_.right = camera_.right();
                pick_camera_.up = glm::cross(pick_camera_.right, pick_camera_.forward);
                pick_camera_.half_width = view_area_.w * 0.5f;
                pick_camera_.half_height = view_area_.h * 0.5f;
                pick_camera_.half_fov = fov.h * 0.5f;
                pick_camera_.fov_ratio = fov.v != 0.0f ? fov.h / fov.v : 1.0f;
                pick_camera_.yon = view_.far_plane;
                pick_camera_valid_ = view_area_.w > 0.0f && view_area_.h > 0.0f;
            }
        }
        lap.mark("scene other");
        ui_.render(*f);
        lap.mark("ImGui render");
        perf_ui_.mark(FramePhase::Record);
        vk_.end_frame(*f);
        perf_ui_.mark(FramePhase::Present);
        settle_frame_.scene = in_game;
        update_zone_settle();
    }
    if (eq_api_) eq_api_->save_layout();
    save_chat_history(true);
    if (opts_.window_width == 1280 && opts_.window_height == 720) {
        // The game window's place for the next start (window_desc); not a --window-size run's.
        platform::WindowGeometry g = window_.geometry();
        config_.set_int("window.x", g.x);
        config_.set_int("window.y", g.y);
        config_.set_int("window.width", g.width);
        config_.set_int("window.height", g.height);
        config_.set_bool("window.maximized", g.maximized);
        config_.set_bool("ui.perf_window", perf_ui_.open());
        config_.set_bool("ui.perf_log_hitches", perf_ui_.log_hitches());
        config_.save(config_path_);
    }
    if (automation_) automation_->shutdown();
    if (conn_) conn_->disconnect();
    vk_.wait_idle();
    audio_.reset();
    return 0;
}

void ClientApp::update_guide() {
    // The Find window's guide path (app/guide_path.h): the NPC's place, found again over the mesh as the player goes.
    if (!guide_.active() || !conn_) return;
    if (conn_->world().zone_short_name != guide_zone_) {
        guide_.stop();
        return;
    }
    std::optional<glm::vec3> goal;
    const auto& spawns = conn_->world().spawns;
    if (auto it = spawns.find(guide_.spawn()); it != spawns.end() && !it->second.is_corpse)
        goal = it->second.display_valid ? it->second.display_position : it->second.position;
    const double now = std::chrono::duration<double>(Clock::now() - start_time_).count();
    const std::string name = guide_.name();
    auto u = guide_.update(feet_, goal, now, [this](const glm::vec3& from, const glm::vec3& to) {
        return nav_ ? nav_->plan_between(from, to) : std::nullopt;
    });
    // Hailstone's own lines (the original has no guide path).
    if (u.arrived) conn_->ui_message("You have reached " + name + ".");
    else if (u.gone) conn_->ui_message(name + " is no longer here.");
    else if (u.no_way) conn_->ui_message("There is no way to " + name + " from here.");
    else if (u.waiting_mesh) conn_->ui_message("The path to " + name + " will show once this zone's map is ready.");
}


} // namespace client_internal

int run_client(ClientOptions& options) {
    int code = 0;
    client_internal::ClientApp::UpdateExit update;
    {
        // Without --game-dir or EQ_GAME_DIR the asset folder comes from hailstone.ini or the picker
        // (choose_asset_folder). On the heap: a Windows crash dump holds the threads' stacks, and the client's
        // members (the login fields, the session key) must not be in it.
        auto app = std::make_unique<client_internal::ClientApp>(options);
        core::wipe(options.auto_password);
        code = app->run();
        update = app->update_exit();
    }
    // The client is gone (its window, the browser helper, the plugins): a downloaded update goes in now.
    if (update.install) finish_update_at_exit(update.restart);
    return code;
}

} // namespace hailstone::app
