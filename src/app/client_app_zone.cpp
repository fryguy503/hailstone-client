// ClientApp: Zone load and unload, scene fog, listener and music, entering the zone.

#include "app/client_app_internal.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include <SDL3/SDL_video.h>

#include "assets/zone_flora.h"
#include "core/ascii_case.h"
#include "core/ini_file.h"
#include "core/server_profile.h"
#include "mq/lua_mq.h"
#include "game/mechanics/abilities.h"
#include "game/option_commands.h"
#include "game/particles.h"
#include "game/revamped_zone.h"
#include "nav/zone_nav.h"
#include "platform/process.h"
#include <unordered_set>

namespace hailstone::app::client_internal {

void ClientApp::load_zone(const std::string& short_name) {
    log_memory("zone load: before unload");
    unload_zone();
    if (audio_) {
        audio_->set_zone(short_name);
        sound_events_->reset();
    }
    loading_view_.detail = "Loading " + short_name;
    auto t0 = Clock::now();
    // What each step of the load leaves in memory, so a load that holds much more than its geometry can be read.
    auto mark = [&](const char* phase) { log_memory(std::string("zone load: ") + phase); };
    mark("start");
    try {
        // A live folder's revamped .eqg loads when the server's safe point (OP_NewZone, already here: it asked for
        // this load) stands on its floor (game/revamped_zone.h). The test's collision mesh is kept below: nothing
        // added to the zone after the load (flora, emitters) is collision geometry.
        std::optional<game::CollisionMesh> tested_collision;
        assets::RevampedZoneTest revamped_test;
        if (conn_)
            if (const auto& z = conn_->zone_info(); z && iequals(z->zone_short_name, short_name))
                revamped_test = game::safe_point_test(game::server_to_world({z->safe_x, z->safe_y, z->safe_z}),
                                                      tested_collision);
        zone_ = assets::load_zone(game_, short_name, {}, revamped_test);
        mark("zone data");
        // Hailstone's own flora for a zone without any (assets/zone_flora.h, display.zone_flora).
        if (config_.get_bool("display.zone_flora", true)) {
            // flora/default.txt only where the server calls the zone outdoor (OP_NewZone's time type 1, 2 or 5), so
            // a dungeon's mossy floor stays bare.
            bool outdoor = false;
            if (conn_)
                if (const auto& z = conn_->zone_info()) outdoor = z->time_type == 1 || z->time_type == 2 || z->time_type == 5;
            assets::add_zone_flora(game_, *zone_, platform::flora_dir(), outdoor);
        }
        mark("flora");
        zone_particles_ = std::make_unique<ZoneParticles>(vk_, game_, shader_dir_, *zone_);
        zone_particles_->set_density(environment_particle_density_);
        // PlayerClient::PostBuildInit makes the spawns' lights only with ShowDynamicLights (/dynamiclights: "takes
        // effect the next time you zone").
        dynamic_lights_ = config_.get_bool("display.dynamic_lights", true);
        // The zone's flames for the modern lighting, and a light for those that have none (before the zone
        // renderer takes the lights).
        {
            auto meshes = assets::placement_fires(*zone_);
            auto fires = assets::merge_fires(meshes, zone_particles_->fires());
            std::size_t lights = assets::add_fire_lights(*zone_, fires);
            zone_fires_.clear();
            for (const auto& fire : fires) render::add_fire_sources(fire, zone_fires_);
            // Without the particle clouds (EnvironmentParticleDensity 0) only the flame meshes burn.
            zone_mesh_fires_.clear();
            for (const auto& fire : meshes) render::add_fire_sources(fire, zone_mesh_fires_);
            log::info("zone fires: {} flames, {} lights added for them", zone_fires_.size(), lights);
            if (!fire_renderer_) fire_renderer_ = std::make_unique<render::FireRenderer>(vk_, shader_dir_);
        }
        zone_renderer_ = std::make_unique<render::ZoneRenderer>(vk_, *zone_, shader_dir_, render::zone_render_options(game_));
        distant_hidden_key_ = 0;
        mark("zone renderer");
        flora_renderer_.reset();
        if (!zone_->flora.empty()) flora_renderer_ = std::make_unique<render::FloraRenderer>(vk_, *zone_, shader_dir_);
        mark("flora renderer");
        point_shadows_.reset();
        if (spell_particles_) spell_particles_->clear();
        if (spell_bolts_) spell_bolts_->clear();
        if (actor_emitters_) actor_emitters_->clear();
        collision_ = tested_collision ? std::move(*tested_collision) : game::CollisionMesh::build(*zone_);
        mark("collision");
        if (conn_) conn_->reground(); // ground_npcs measured the spawns against no floor, or the last zone's
        zone_ladders_ = game::zone_ladders(*zone_);
        if (nav_)
            nav_->zone_loaded(*collision_, &zone_->regions,
                              platform::cache_dir() / "navmesh" /
                                  (server_profile_.id.empty() ? std::string("default") : server_profile_.id) /
                                  nav::ZoneNav::cache_name(short_name));
        mark("navigation");
        ambient_probe_ = render::AmbientProbe(*zone_);
        mark("ambient probe");
        probe_blend_.reset();
        // Zone-line boxes under the terrain (a client addition, game/zone_lines.h): walking over them fires them.
        zone_line_.set_buried(game::find_buried_zone_lines(zone_->regions, *collision_));
        for (const auto& b : zone_line_.buried())
            log::info("zone line {} (box {}) is buried: box z {:.1f}..{:.1f}, ground {:.1f}..{:.1f} over it ({} of {} samples); "
                      "the fallback fires over its footprint up to z {:.1f}",
                      b.tag, b.region, b.box_lo_z, b.box_hi_z, b.ground_lo, b.ground_hi, b.covered, b.samples, b.top_z);
        // --load-stall: a slow disk or a busy machine; the network must ride it out.
        if (opts_.load_stall_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(opts_.load_stall_ms));
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        log::info("zone {} loaded in {} ms ({} triangles)", short_name, ms, zone_renderer_->stats().triangles);
    } catch (const std::exception& e) {
        // Missing or malformed zone files: stay connected without scenery.
        log::warn("cannot load zone {}: {}", short_name, e.what());
        game_view_.chat.add(ui::ChatChannel::Error,
                            "This zone's scenery cannot be loaded (" + short_name + "); spawns are still shown.");
        unload_zone();
    }
    world_objects_->set_zone(zone_ ? &*zone_ : nullptr, nullptr);
    try {
        auto library = characters_.library_for_zone(game_, short_name);
        eq_host_.char_library = nullptr; // the old renderer's library goes with it
        actor_renderer_ = std::make_unique<render::ActorRenderer>(vk_, library, shader_dir_, std::move(actor_kept_));
        eq_host_.char_library = &actor_renderer_->library();
        actor_library_ = library;
        world_objects_->set_zone(zone_ ? &*zone_ : nullptr, library); // item models for dropped items
    } catch (const std::exception& e) {
        log::warn("character models unavailable in {}: {}", short_name, e.what());
    }
    if (const auto& z = conn_->zone_info()) {
        game::ZoneFogInput fog;
        fog.fog_enabled = z->zone_type != 0; // SetFog(ztype != 0, ...)
        fog.fog_color = glm::vec3(z->fog_red[0], z->fog_green[0], z->fog_blue[0]) / 255.0f;
        fog.fog_minclip = z->fog_minclip[0];
        fog.fog_maxclip = z->fog_maxclip[0];
        fog.minclip = z->minclip;
        fog.maxclip = z->maxclip;
        fog.fog_density = z->fog_density;
        fog.time_type = z->time_type;
        zone_fog_ = fog;
        view_ = game::compute_view_distance(fog, opts_.clip_plane);
        scene_fog_.set_zone(fog, z->sky != 0);
        // The procedural sky's setting: SkyString2 (not sent), else zone_short_name2 (RealRender_World @0x49d237).
        load_precipitation();
        sky_state_.set_sky(z->zone_short_name2.empty() ? z->zone_short_name : z->zone_short_name2);
        player_light_.reset();
        haze_blend_.reset();
        camera_region_ = 0;
        // The classic sky (sky.s3d). The time-of-day light follows the
        // original's outdoor zone types 1, 2 and 5 (CDisplay_UpdateSunLighting).
        // The sky itself does not: it is drawn whenever OP_NewZone's sky byte is
        // set (with the Sky option on and SkyString2 or the zone's short name
        // non-empty; SetFog and CDisplay_SetSkyEnabled @0x4b37c0);
        // the time type is never read for it.
        outdoor_light_ = z->time_type == 1 || z->time_type == 2 || z->time_type == 5;
        time_type_ = z->time_type;
        sky_renderer_.reset();
        if (z->sky > 0 && zone_renderer_) {
            try {
                if (auto sky = assets::load_sky(game_, z->sky))
                    sky_renderer_ = std::make_unique<render::SkyRenderer>(vk_, *sky, shader_dir_, &game_, zone_ ? zone_->name : std::string());
                else
                    log::warn("{}: sky {} not found in sky.s3d", short_name, z->sky);
            } catch (const std::exception& e) {
                log::warn("{}: sky {} unavailable: {}", short_name, z->sky, e.what());
            }
        }
    }
    if (loading_since_)
        zone_loaded_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - *loading_since_).count();
}

namespace {
// The time a frame behind the loading screen spends on new appearances (render::ActorRenderer::kBuildBudgetMs in the
// open): nobody sees the frame that builds them, so a crowd is dressed in a few. 100 ms keeps the loading screen's
// bar moving about ten times a second, while the rest of each frame (about 10 to 20 ms in the bazaar) stays a small
// part of the time. The bazaar's 327 appearances took 12 frames of 32 each, up to 800 ms a frame.
constexpr double kSettleBuildBudgetMs = 100.0;
} // namespace

void ClientApp::begin_zone_settle() {
    auto now = Clock::now();
    if (loading_since_)
        zone_in_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(now - *loading_since_).count();
    zone_settle_.begin(std::chrono::duration<double>(now - start_time_).count());
    settle_frame_ = {};
    if (actor_renderer_) actor_renderer_->set_build_budget_ms(kSettleBuildBudgetMs);
    loading_view_.detail = zone_settle_.detail();
    loading_view_.progress = 0.5f;
}

// After each frame drawn behind the loading screen: what it left undone, and whether the loading screen can go.
void ClientApp::update_zone_settle() {
    if (!zone_settling()) return;
    if (screen_ != Screen::InGame || !player_ready_) { // gone meanwhile (the link died, zoning again)
        zone_settle_.cancel();
        if (actor_renderer_) actor_renderer_->set_build_budget_ms(render::ActorRenderer::kBuildBudgetMs);
        return;
    }
    auto now = Clock::now();
    if (actor_renderer_) settle_frame_.actors_waiting = actor_renderer_->stats().undressed;
    bool done = zone_settle_.update(settle_frame_, std::chrono::duration<double>(now - start_time_).count());
    settle_frame_ = {};
    loading_view_.detail = zone_settle_.detail();
    loading_view_.progress = 0.5f + 0.5f * zone_settle_.progress();
    if (!done) return;
    if (actor_renderer_) actor_renderer_->set_build_budget_ms(render::ActorRenderer::kBuildBudgetMs);
    auto ms = [](Clock::duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
    std::int64_t total = loading_since_ ? ms(now - *loading_since_) : -1;
    log::info("zone-in: loading screen up {} ms (zone loaded at {} ms, zone-in done at {} ms, then {} frames in {} ms, "
              "{} appearances waiting at most{})",
              total, zone_loaded_ms_, zone_in_ms_, zone_settle_.frames(),
              zone_in_ms_ >= 0 && total >= 0 ? total - zone_in_ms_ : -1, zone_settle_.most_waiting(),
              zone_settle_.timed_out() ? "; timed out: " + zone_settle_.detail() : std::string());
    log_memory("zone-in: settled appearances", true);
    if (actor_library_) {
        std::size_t loaded = 0, count = 0, bytes = 0, decodes = 0, releases = 0;
        std::unordered_set<const assets::CharacterArchive*> seen;
        for (const auto& archive : actor_library_->archives()) {
            if (!seen.insert(archive.get()).second) continue;
            bytes += archive->parsed_bytes();
            for (const auto& w : archive->wlds) {
                ++count;
                if (w.materialized()) ++loaded;
                const auto stats = w.decode_stats();
                decodes += stats.decodes;
                releases += stats.releases;
            }
        }
        log::info("memory character WLDs: {} / {} materialized, index and decoded estimate {:.2f} MiB "
                  "(retained after use: {:.2f} of {:.0f} MiB)",
                  loaded, count, double(bytes) / (1024.0 * 1024.0),
                  double(assets::wld::WldFile::retained_total()) / (1024.0 * 1024.0),
                  double(assets::wld::WldFile::retain_budget()) / (1024.0 * 1024.0));
        log::info("memory character WLD activity: {} decodes, {} releases (current archives, cumulative)",
                  decodes, releases);
        const auto caches = actor_library_->cache_memory();
        constexpr double mib = 1024.0 * 1024.0;
        log::info("memory character caches: {} models {:.2f} MiB, {} appearances {:.2f} MiB, "
                  "{} item copies {:.2f} MiB (payload estimates)",
                  caches.models, double(caches.model_bytes) / mib, caches.appearances,
                  double(caches.appearance_bytes) / mib, caches.items, double(caches.item_bytes) / mib);
        log::info("memory model breakdown: animation/skeleton {:.2f} MiB, geometry/materials {:.2f} MiB, other {:.2f} MiB",
                  double(caches.model_animation_bytes) / mib, double(caches.model_geometry_bytes) / mib,
                  double(caches.model_other_bytes) / mib);
    }
    loading_since_.reset();
    // The zone-in clocks (the input script's, the test drivers') start as the world shows.
    zone_entered_ = now;
}

void ClientApp::unload_zone() {
    zone_settle_.cancel();
    if (pre_game_loaded_) {
        // Character select's scene goes with its zone: the model player and its camera's field of view.
        pre_game_loaded_ = false;
        eq_host_.model_player = nullptr;
        camera_.fov_units = pre_game_saved_fov_;
    }
    pre_game_world_.reset();
    missiles_.clear(); // CEverQuest_ClearZoneWorldState -> EQMissile_UpdateAll
    if (audio_) audio_->set_zone("");
    if (zone_renderer_) vk_.wait_idle();
    if (neighbours_) neighbours_->clear();
    eq_host_.zone_exits.clear();
    eq_host_.zone_exits_zone = 0;
    zone_renderer_.reset();
    flora_renderer_.reset(); // holds the zone's plants
    zone_particles_.reset(); // reads the zone's archives
    zone_fires_.clear();
    zone_mesh_fires_.clear();
    frame_fires_.clear();
    frame_fire_list_ = {};
    sky_renderer_.reset();
    if (actor_renderer_) actor_kept_ = actor_renderer_->release_kept();
    actor_renderer_.reset();
    eq_host_.char_library = nullptr; // its library went with it (update_eq_host sets it again)
    for (auto& slot : previews_) {
        slot.view.reset();
        slot.serial = 0xffffffffu;
    }
    actor_library_.reset();
    if (nav_) nav_->zone_unloaded();
    collision_.reset();
    zone_ladders_.clear();
    zone_line_.set_buried({});
    weather_.clear_particles(); // they land on the old zone's ground
    if (world_objects_) world_objects_->set_zone(nullptr, nullptr); // its models read the zone's archives
    zone_.reset();
}

void ClientApp::load_precipitation() {
    if (precipitation_loaded_ || !game_.has_base()) return;
    precipitation_loaded_ = true;
    if (auto defs = game::PrecipitationDefinitions::load(game_)) {
        if (weather_renderer_) weather_renderer_->load_textures(game_, *defs);
        weather_.set_precipitation_definitions(std::move(*defs));
    } else {
        log::warn("Resources/Precipitation/default.ini not found: no rain or snow falls");
    }
    sky_state_ = game::SkyState(game::SkyDefinitions::load(game_));
}

void ClientApp::update_precipitation(float dt, float sky_hour) {
    // CSky_Update's colour map. The original brings it up to date only while the sky is drawn, so in a zone without
    // a sky its drops keep the colours of the last sky drawn (none since login: 0, invisible); here it follows the
    // zone's setting and the time of day always (docs/unknowns.md, Weather).
    if (sky_state_.enabled() && sky_hour >= 0.0f) {
        sky_state_.set_time_of_day(std::clamp(sky_hour / 24.0f, 0.0f, 1.0f));
        sky_state_.update(dt * 1000.0f, weather_.precipitation(), 0xff000000u);
    }
    game::PrecipitationField::Frame f;
    // The modern look thickens and thins the drops over tens of seconds (render::WeatherLook::precipitation), and
    // starts more of them (render::PrecipitationStyle::density).
    if (lighting_mode_ == render::LightingMode::Modern)
        f.eased_intensity = weather_look_.precipitation * render::kModernPrecipitation.density;
    // On the game clock the drops are counted per step and move by the drawn frame (game::PrecipitationField).
    if (step_clock_.fixed()) f.motion_seconds = dt;
    f.camera = camera_.position;
    f.heading = std::fmod(camera_.yaw * (512.0f / (2.0f * std::numbers::pi_v<float>)), 512.0f);
    if (f.heading < 0.0f) f.heading += 512.0f;
    // CSky +0x38(2 kind) and +0x38(2 kind + 1): colour map rows 4 + 2 kind and 5 + 2 kind, column 31.
    const std::int32_t kind = weather_.particles().kind();
    if (const auto& map = sky_state_.current_map(); map && kind >= 0 && kind < 12) {
        f.colours = {game::colour_cell(*map, 4 + 2 * kind), game::colour_cell(*map, 5 + 2 * kind)};
    }
    f.random = [this] { return std::uniform_int_distribution<std::int32_t>(0, 0x7fff)(weather_random_); };
    f.ground = [this](const glm::vec3& a, const glm::vec3& b,
                      bool back_faces) -> std::optional<game::PrecipitationHit> {
        if (!collision_) return std::nullopt;
        auto hit = collision_->first_hit(a, b, back_faces);
        if (!hit) return std::nullopt;
        return game::PrecipitationHit{hit->t, hit->normal.z};
    };
    weather_.update_particles(f);
}

// One frame of CDisplay_UpdateSkyWeatherAndFog -> ApplyWeatherFog -> SetFog (game::SceneFog), with the region type
// at the camera (the camera driver's 0x48c890 after IsPointInWorld; outside the world the last one stays),
// infravision and ultravision (the race's innate slots +0x3248 / +0x3264, SPA 65 / 66) and the player's light level.
game::SceneFogFrame ClientApp::scene_fog_frame(float sky_hour, double t) {
    game::SceneFogInput in;
    float h = std::fmod(sky_hour, 24.0f);
    if (h < 0.0f) h += 24.0f;
    int hour = static_cast<int>(std::floor(h));
    in.minute = std::clamp(static_cast<int>((h - static_cast<float>(hour)) * 60.0f), 0, 59);
    in.hour = hour < 1 ? hour + 24 : hour; // GetSkyTime: 1..24
    // The isometric camera (Hailstone's own) floats over the scenery: the region is the player's eye's, so its water
    // and lava tints are the player's and not the air's or the hillside's the camera hangs in.
    const glm::vec3 region_at = iso() ? eye_position() : camera_.position;
    if (zone_ && !zone_->regions.empty() && zone_->regions.in_world(region_at))
        camera_region_ = static_cast<std::uint8_t>(zone_->regions.lookup(region_at).flags & 0xff);
    in.camera_region = camera_region_;
    if (conn_) {
        // EQ_Character::InitInnates with the actor's race (an illusion's too) and the profile's class: the client's
        // own innates, not the profile packet's (EQEmu sends 0xff in every slot).
        if (const auto& p = conn_->profile()) {
            std::uint32_t race = p->race;
            if (const game::Spawn* me = conn_->world().player()) race = me->race;
            const auto innates = game::mechanics::init_innates(race, p->class_);
            in.infravision_innate = innates[game::mechanics::kInnateInfravision] != 0xff;
            in.ultravision_innate = innates[game::mechanics::kInnateUltravision] != 0xff;
        }
    }
    if (const game::mechanics::Character* ch = stats_ui_ ? stats_ui_->character() : nullptr) {
        in.infravision_effect = ch->total_effect(65);
        in.ultravision_effect = ch->total_effect(66);
    }
    in.actor_light = player_light_.update(static_cast<std::uint32_t>(t * 1000.0), feet_ + glm::vec3(0, 0, floor_offset_),
                                          collision_ ? &*collision_ : nullptr);
    in.clip_plane = opts_.clip_plane;
    if (step_clock_.fixed()) in.steps = static_cast<int>(frame_steps_.size()); // per step of the game clock
    return scene_fog_.frame(in);
}

void ClientApp::update_listener() {
    // RealRender_World: the local player's position (PlayerBase +0x64..+0x6c, the feet plus AvatarHeight) with the
    // camera's heading; EqSoundManager::SetListenerLocation turns the heading into radians.
    glm::vec3 position = feet_ + glm::vec3(0, 0, floor_offset_);
    float heading = game::yaw_to_heading(camera_.yaw);
    audio::ListenerLocation loc = audio::eq_listener_location(0, 0, 0, std::bit_cast<std::uint32_t>(heading));
    audio_->set_listener(position, std::bit_cast<float>(loc.angle));

    // CDisplay::SetupEmitterEnvironment @0x48a500 (audio/listener.h): indoors when the player actor's scaled ambient
    // (its +0xf0, CActor::GetScaledAmbient: game/scaled_ambient.h) is below 0.48; EmitterSetIndoors tells the sound
    // manager, which keeps the rain and wind emitters off indoors.
    listener_indoors_ = player_light_.level() < audio::kIndoorAmbient;
    audio_->set_indoors(listener_indoors_);
    // CDisplay::ApplyWeatherState: EmitterSetRaining once the rain has faded in, EmitterSetWind once the snow has
    // (LightningStrike too), both off once faded out.
    audio_->set_weather(weather_.raining(), weather_.windy());

    // PlayerZoneClient's HeadWet change: under water (5) sets room type 0x16 and the special flag
    // (0x48a480); coming out runs UpdateListenerEnvironment again.
    std::uint8_t head_wet = motion_head_wet_;
    if (head_wet != head_wet_) {
        if (head_wet == 5) {
            audio_->set_special_room(audio::kRoomUnderwater);
            special_room_ = true;
        } else if (head_wet_ == 5) {
            audio_->set_special_room(std::nullopt);
            special_room_ = false;
        }
        head_wet_ = head_wet;
    }
    // StunMe sets room type 0x17 and the special flag the same way; EndStun runs UpdateListenerEnvironment.
    if (const bool stunned = conn_ && conn_->stunned(); stunned != stun_room_) {
        audio_->set_special_room(stunned ? std::optional<std::int32_t>(audio::kRoomDrugged) : std::nullopt);
        special_room_ = stunned;
        stun_room_ = stunned;
    }
    if (special_room_) return; // RealRender_World skips the update while +0x2d68 is set

    // CDisplay_UpdateListenerEnvironment: the ceiling is PlayerClient +0x11b4, which CanIFitHere writes when the
    // player fits under one (game/mechanics/can_i_fit_here.h); the floor is FloorHeight (+0x28).
    listener_ceiling_ = movement_.player_ceiling();
    float floor = movement_.floor_height().value_or(feet_.z);
    std::int32_t environment = audio::kEnvironmentOutside; // no ceiling found yet: open sky
    if (listener_ceiling_)
        environment = audio::listener_environment(true, *listener_ceiling_, floor, listener_indoors_).value_or(environment);
    if (environment != audio_->listener_environment())
        log::debug("listener: feet {:.2f} {:.2f} {:.2f}, floor {:.2f}, ceiling {}", feet_.x, feet_.y, feet_.z, floor,
                   listener_ceiling_ ? std::to_string(*listener_ceiling_) : std::string("none"));
    audio_->set_listener_environment(environment);
}

void ClientApp::update_music() {
    if (!audio_) return;
    bool in_world = screen_ == Screen::InGame && player_ready_ && conn_;
    auto select = [this](bool& on, bool want, std::int32_t id) {
        if (on == want) return;
        on = want;
        audio_->music_selection(id, want);
    };
    // CEverQuest::SetAutoAttack: on a change, entry 2 with the attack on and the CombatMusic option, else off.
    bool attack = in_world && conn_->auto_attack();
    if (attack != music_attack_) {
        music_attack_ = attack;
        audio_->music_selection(audio::kMusicCombat, attack && audio_->settings().combat_music);
    }
    // The bank (CBankWnd), merchant (CMerchantWnd) and trainer (CTrainWnd) windows: their music while open.
    select(music_bank_, in_world && conn_->bank().has_value(), audio::kMusicBank);
    select(music_merchant_, in_world && conn_->merchant() && conn_->merchant()->open, audio::kMusicMerchant);
    select(music_trainer_, in_world && conn_->training().has_value(), audio::kMusicTrainer);
    // The loot window: Handle_OP_MoneyOnCorpse @0x6be950 selects entry 5 when the corpse opens (responses 1 and 3)
    // and unk_CLootWnd_ResetLootSession @0x6bd0b0 drops it. The entry has no asset, so the zone's music is silent
    // while looting.
    select(music_loot_, in_world && conn_->loot_session() && conn_->loot_session()->open, audio::kMusicLoot);
    // PlayerClient's death of the local player (IDied): the death music, until the next zone-in.
    if (conn_ && conn_->player_status().dead && !music_dead_) {
        music_dead_ = true;
        audio_->music_selection(audio::kMusicDeath, true);
    }
}

void ClientApp::enter_zone() {
    local_body_movement_.reset();
    driven_eye_ = 0;
    const game::Spawn* me = conn_->world().player();
    if (!me) return;
    // AvatarHeight (+0x138): EQPlayer::SetHeights' moddat.ini ROffset (3.125 for the player races, which have
    // none) times the classic model scale (size x 0.2); the spawn's z is the feet plus this. (Measuring it from
    // the server's z over the floor kept any offset a saved position carried, and the collision sphere sits on it.)
    // CreatePlayerActor's in-world test: the spawn's position (its own OP_ZoneEntry record), raised by 2 up to four
    // times while it is outside the world's regions, else the zone's safe point; no horizontal push-out. The
    // feet are AvatarHeight below it (game/spawn_heights.h); the first landing check stands it on the floor.
    glm::vec3 position = me->position;
    if (zone_ && !zone_->regions.empty()) {
        std::array<float, 3> safe{position.x, position.y, position.z};
        if (const auto& nz = conn_->zone_info()) {
            glm::vec3 sp = game::server_to_world({nz->safe_x, nz->safe_y, nz->safe_z});
            safe = {sp.x, sp.y, sp.z};
        }
        auto placed = game::place_in_world({position.x, position.y, position.z}, safe, [&](const std::array<float, 3>& p) {
            return zone_->regions.in_world(glm::vec3(p[0], p[1], p[2]));
        });
        if (placed[2] != position.z || placed[0] != position.x)
            log::info("zone-in: ({:.2f} {:.2f} {:.2f}) is outside the world; placed at ({:.2f} {:.2f} {:.2f})", position.x,
                      position.y, position.z, placed[0], placed[1], placed[2]);
        position = glm::vec3(placed[0], placed[1], placed[2]);
    }
    floor_offset_ = me->heights.avatar_height;
    feet_ = position - glm::vec3(0, 0, floor_offset_);
    // The last zone's last step is not drawn across into this one: the body and camera are drawn between steps
    // (update_player), and a frame before the first step here would offset them by it.
    step_move_ = glm::vec3(0.0f);
    step_turn_ = step_pitch_ = 0.0f;
    if (audio_ && music_dead_) {
        // The death music stops on the zone-in after it (MSG_READY / the repop).
        audio_->music_selection(audio::kMusicDeath, false);
        music_dead_ = false;
    }
    listener_ceiling_.reset();
    head_wet_ = 0;
    if (audio_ && special_room_) audio_->set_special_room(std::nullopt);
    special_room_ = false;
    stun_room_ = false;
    yaw_ = game::heading_to_yaw(me->heading);
    pitch_ = 0.0f;
    movement_.reset(feet_, yaw_);
    target_manager_ = {};
    zone_entered_ = Clock::now();
    zone_line_.reset();
    // The character's own files: servers/<profile>/characters/<world>/<name>/ (core/character_files.h).
    std::string profile = files_profile();
    CharacterFiles files = character_files(profile, char_view_.server_name, game::display_name(me->name));
    use_character_settings(files.dir); // (taken over at the entry into the world: the same, unless the name differs)
    actions_ui_->enter_zone(files.hot_buttons());
    // The character's autorun scripts (the Scripts window; the same file at each zone-in starts nothing again).
    if (automation_) automation_->scripts().set_autorun_file(files.scripts_ini());
    // The UI's window layout is the character's (ui-<pack>.txt), its other settings shared.
    ui_character_dir_ = files.dir;
    if (lua_ui_) lua_ui_->set_character_settings(files.ui_settings(ui_name_));
    conn_->set_character_ini(
        [this](std::string_view section, std::string_view key, int fallback) {
            return actions_ui_ ? actions_ui_->char_ini_int(section, key, fallback) : fallback;
        },
        [this](std::string_view section, std::string_view key, const std::string& value) {
            if (actions_ui_) actions_ui_->write_char_ini(section, key, value);
        });
    conn_->load_name_change_flags();
    if (eq_api_) {
        // The window places: the character's UI.ini over the game's defaults.ini; the game's own
        // UI_<name>_<server>.ini, when there is one, is the starting layout (InitCharacterIniFilenames). Both are
        // only read. The map edits go to the server's maps folder.
        std::string file = files.ui_ini().string();
        if (file != layout_for_) {
            layout_for_ = file;
            eq_host_.maps_dir = profile_maps_dir(profile);
            eq_host_.server_dir = profile_dir(profile);
            // Made empty, so the player sees where maps for every server go.
            eq_host_.global_maps_dir = platform::maps_dir();
            std::error_code maps_ec;
            std::filesystem::create_directories(eq_host_.global_maps_dir, maps_ec);
            auto defaults = game_.find("defaults.ini");
            auto seed = game_.find("UI_" + game::display_name(me->name) + "_" + char_view_.server_name + ".ini");
            std::error_code ui_ec;
            bool first_entry = !files.empty() && !seed && !std::filesystem::exists(files.ui_ini(), ui_ec);
            eq_api_->load_layout(defaults ? *defaults : std::filesystem::path(), files.ui_ini(),
                                 seed ? *seed : std::filesystem::path());
            // CDisplay::InitGameUI: the story window when there is a new story and Automatic is on, above level 1.
            if (conn_->player_status().level > 1) eq_api_->activate_window("StoryWnd", 0);
            // Hailstone: a character with no layout of its own yet is offered another character's (the layout copy
            // window's activate, app/lua_eq_layout.cpp; not in the original).
            if (first_entry) eq_api_->activate_window("LayoutCopyWindow", 0);
        }
        // CEverQuest::ProcessGame at each zone-in: the tip of the day (CTipWnd::Activate(pinstCTipWnd_1, -1, 0)).
        eq_api_->activate_tip_of_day(false);
    }
    if (!opts_.abilities_test.empty()) {
        std::vector<std::uint32_t> skills;
        for (std::size_t pos = 0; pos < opts_.abilities_test.size();) {
            std::size_t comma = opts_.abilities_test.find(',', pos);
            skills.push_back(static_cast<std::uint32_t>(std::atoi(opts_.abilities_test.substr(pos, comma - pos).c_str())));
            pos = comma == std::string::npos ? opts_.abilities_test.size() : comma + 1;
        }
        actions_ui_->setup_test_buttons(skills);
        actions_ui_->open_all();
    }
    player_ready_ = true;
}


namespace {
constexpr std::size_t kSpellLights = 8; // of render::kMaxSceneLights
constexpr std::size_t kBoltLights = 6;
} // namespace

render::SceneLighting ClientApp::scene_lighting(const game::SceneLight& scene, float hour,
                                                render::DynamicShadowLights* moving) const {
    std::vector<game::LightSource> dynamic;
    std::vector<game::LightSource> lights;
    render::Daylight sky;
    if (lighting_mode_ == render::LightingMode::Modern) {
        sky = render::daylight(hour, time_type_ == 1 || time_type_ == 2 || time_type_ == 5, moon_phase_, blood_moon_);
        // The weather: the storm's grey light, the lightning lighting everything from the sky.
        render::apply_storm(sky, weather_look_.storm);
        sky.sky += glm::vec3(weather_look_.flash * 1.5f);
        // The carried lights, the player's at about chest height. The player's light comes from its own inventory,
        // every other carrier's from its spawn record and SpawnAppearance 5 (game::carrier_light_value says why).
        // A spawn the viewer cannot see (market_hidden_, last frame's IsInvisible set) carries none. Without dynamic
        // lights (/dynamiclights off at the zone-in) none of these lights is made.
        if (dynamic_lights_ && conn_ && screen_ == Screen::InGame && player_ready_) {
            const std::uint32_t player_id = conn_->world().player_id;
            for (const auto& [id, spawn] : conn_->world().spawns) {
                if (market_hidden_.contains(id)) continue;
                std::optional<game::LightSource> l;
                if (id == player_id)
                    l = game::carried_light(game::carrier_light_value(spawn, player_id, conn_->inventory()),
                                            feet_ + glm::vec3(0.0f, 0.0f, floor_offset_ + 4.0f));
                else
                    l = game::spawn_carried_light(spawn);
                if (!l) continue;
                dynamic.push_back(*l);
                if (moving && id == player_id) {
                    moving->carried = *l;
                    moving->carrier = id;
                }
            }
        }
        // Character select and creation: a key light on the model (game::char_select_key_light; not the original's).
        if (!pre_game_hidden_ && pre_game_scene())
            if (auto it = pre_game_world_->spawns.find(kModelPlayerId); it != pre_game_world_->spawns.end()) {
                // A spawn's position is AvatarHeight (about half its height) above its feet.
                const game::SpawnHeights& h = it->second.heights;
                dynamic.push_back(game::char_select_key_light(
                    it->second.position - glm::vec3(0.0f, 0.0f, h.avatar_height), h.avatar_height * 2.0f,
                    camera_.position));
            }
        // The glowing spell effects light what is round them, and the bolts and comets what they pass.
        if (spell_particles_ && dynamic_lights_)
            for (const auto& l : spell_particles_->lights(kSpellLights)) {
                dynamic.push_back(l);
                if (moving) moving->spells.push_back(l);
            }
        if (spell_bolts_ && dynamic_lights_)
            for (const auto& l : spell_bolts_->lights(kBoltLights)) {
                dynamic.push_back(l);
                if (moving) moving->spells.push_back(l);
            }
        std::span<const assets::PointLight> zone_lights;
        if (zone_) zone_lights = zone_->lights;
        lights = game::select_lights(zone_lights, dynamic, camera_.position, 600.0f, render::kMaxSceneLights);
    }
    render::SceneLighting out = render::make_scene_lighting(scene, lights, dynamic.size(), lighting_mode_, sky);
    out.params.w = 0.04f; // the light characters and objects never fall below (readable in the dark), linear
    return out;
}

namespace {

// The Display page's sliders: 21 ticks over each setting's range.
struct SliderRange {
    std::string_view name;   // display_option's name
    std::string_view key;    // hailstone.ini
    float at_zero, per_tick; // value = at_zero + tick x per_tick
    float fallback;
};
constexpr SliderRange kSliders[] = {
    {"view_distance", "display.view_distance", 1.0f, 0.25f, 2.5f}, // 1x .. 6x
    {"exposure", "display.exposure", -2.0f, 0.2f, 0.0f},           // -2 .. +2 EV
    {"brightness", "display.brightness", -0.2f, 0.02f, 0.0f},
    {"contrast", "display.contrast", 0.5f, 0.05f, 1.0f},
    {"gamma", "display.gamma", 0.5f, 0.05f, 1.0f},
    {"bloom", "display.bloom", 0.0f, 0.05f, 0.5f}, // 0 off .. 1
    {"sharpness", "display.sharpness", 0.0f, 0.05f, 0.5f}, // FSR 1's, 0 none .. 1
};

const SliderRange* slider_range(std::string_view name) {
    for (const auto& r : kSliders)
        if (r.name == name) return &r;
    return nullptr;
}

constexpr std::uint32_t kShadowSizes[] = {0, 1024, 2048, 4096}; // Off, Low, Medium, High
constexpr int kMsaaSamples[] = {1, 2, 4, 8};
constexpr const char* kTonemaps[] = {"auto", "none", "agx", "aces"};
// The per-source spell particle options, in game::SpellFxSource's order (config display.<name>).
constexpr std::array<std::string_view, game::kSpellFxSourceCount> kSpellFxSourceOptions = {
    "spell_particles_self", "spell_particles_players", "spell_particles_npcs"};

} // namespace

void ClientApp::update_view_area() {
    // The window in its own coordinates (ImGui's, the mouse's) and in pixels (the swapchain's).
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window_.sdl_window(), &ww, &wh);
    std::uint32_t pw = 0, ph = 0;
    window_.framebuffer_size(pw, ph);
    view_area_ = {0.0f, 0.0f, static_cast<float>(std::max(ww, 1)), static_cast<float>(std::max(wh, 1))};
    if (!viewport_ || screen_ != Screen::InGame || ww <= 0 || wh <= 0) {
        vk_.set_scene_rect({});
        return;
    }
    game::ViewRect r = game::viewport_rect(*viewport_, ww, wh);
    view_area_ = {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w), static_cast<float>(r.h)};
    const float sx = static_cast<float>(pw) / static_cast<float>(ww), sy = static_cast<float>(ph) / static_cast<float>(wh);
    VkRect2D px{};
    px.offset = {static_cast<std::int32_t>(std::lround(view_area_.x * sx)), static_cast<std::int32_t>(std::lround(view_area_.y * sy))};
    px.extent = {static_cast<std::uint32_t>(std::max(1L, std::lround(view_area_.w * sx))),
                 static_cast<std::uint32_t>(std::max(1L, std::lround(view_area_.h * sy)))};
    vk_.set_scene_rect(px);
}

float ClientApp::particle_density(std::string_view kind) const {
    if (kind == "spell") return spell_particles_ ? spell_particles_->density() : 1.0f;
    if (kind == "actor") return actor_emitters_ ? actor_emitters_->density() : 1.0f;
    return environment_particle_density_;
}

void ClientApp::set_particle_density(std::string_view kind, float density) {
    density = std::clamp(density, 0.0f, 1.0f);
    if (kind == "spell") {
        if (spell_particles_) spell_particles_->set_density(density);
    } else if (kind == "actor") {
        if (actor_emitters_) actor_emitters_->set_density(density);
    } else {
        environment_particle_density_ = density;
        if (zone_particles_) zone_particles_->set_density(density);
    }
    config_.set(std::format("display.{}_particle_density", kind), std::format("{:.2f}", density));
}

void ClientApp::apply_particle_densities() {
    // eqclient.ini [Defaults] EnvironmentParticleDensity (CEverQuest's constructor, default 1); the spell and actor
    // ones were read by their emitters.
    IniFile eqclient;
    if (auto path = game_.find("eqclient.ini")) eqclient.load(*path);
    std::optional<float> environment;
    if (auto v = eqclient.get("Defaults", "EnvironmentParticleDensity"); v && !v->empty())
        environment = std::strtof(v->c_str(), nullptr);
    environment_particle_density_ = game::spell_particle_settings(environment, std::nullopt).density;
    for (std::string_view kind : {"spell", "environment", "actor"}) {
        std::string key = std::format("display.{}_particle_density", kind);
        if (config_.get(key)) set_particle_density(kind, std::clamp(config_float(key, 1.0f), 0.0f, 1.0f));
    }
    // Hailstone: the per-source spell particle densities (display.spell_particles_self / _players / _npcs, the
    // choice 0..3; High when unset).
    if (spell_particles_)
        for (std::size_t i = 0; i < kSpellFxSourceOptions.size(); ++i) {
            int choice = std::clamp(config_.get_int(std::format("display.{}", kSpellFxSourceOptions[i]), 3), 0, 3);
            spell_particles_->set_source_density(static_cast<game::SpellFxSource>(i), game::particle_density_value(choice));
        }
}

std::optional<int> ClientApp::display_option(std::string_view name) const {
    // The advanced display window's shadow clip plane slider (0..100) and Radial Flora box (ShowGrass).
    if (name == "shadow_clip_plane") return shadow_clip_plane_;
    if (name == "grass") return grass_ ? 1 : 0;
    // Hailstone: how far the flora is drawn, in units (render::FloraRenderer::set_reach).
    if (name == "flora_distance") return static_cast<int>(std::lround(flora_reach_));
    // The Options window's particle density combos (SyncDisplayPage's DensityChoice).
    for (std::string_view kind : {"spell", "environment", "actor"})
        if (name.starts_with(kind) && name.substr(kind.size()) == "_particle_density")
            return game::particle_density_choice(particle_density(kind));
    for (std::size_t i = 0; i < kSpellFxSourceOptions.size(); ++i)
        if (name == kSpellFxSourceOptions[i])
            return std::clamp(config_.get_int(std::format("display.{}", name), 3), 0, 3);
    if (const auto* r = slider_range(name))
        return std::clamp(static_cast<int>(std::lround((config_float(r->key, r->fallback) - r->at_zero) / r->per_tick)), 0, 20);
    if (name == "lighting") return lighting_mode_ == render::LightingMode::Modern ? 0 : 1;
    if (name == "ao") return config_.get_bool("display.ao", true) ? 1 : 0;
    if (name == "render_scale") return std::clamp(config_.get_int("display.render_scale", 100), 50, 100);
    if (name == "distance_blur") return config_.get_bool("display.distance_blur", true) ? 1 : 0;
    if (name == "color_grade") return config_.get_bool("display.color_grade", true) ? 1 : 0;
    if (name == "neighbour_zones") return neighbour_zones_ ? 1 : 0;
    if (name == "horizon_hills") return horizon_hills_ ? 1 : 0;
    if (name == "tree_detail") return tree_detail_;
    // Hailstone: NPCs the server leaves in the air are drawn on the floor (game::WorldState::ground_npcs), on by default.
    if (name == "ground_npcs") return config_.get_bool("display.ground_npcs", true) ? 1 : 0;
    if (name == "find_trail_style")
        return static_cast<int>(game::normalize_find_trail_style(config_.get_int("display.find_trail_style", 0)));
    // Hailstone: the target ring's cues (the sweep on a new target, the pulse while attacking), on by default, and its
    // hidden part showing faintly through what stands in front, off by default (app/target_ring.cpp).
    if (name == "target_ring_cues") return ring_cues_ ? 1 : 0;
    if (name == "target_ring_xray") return ring_xray_ ? 1 : 0;
    if (auto r = ring_option(name)) return r;
    if (name == "shadows") {
        int size = config_.get_int("display.shadows", 2048);
        int best = 0;
        for (int i = 0; i < 4; ++i)
            if (std::abs(static_cast<int>(kShadowSizes[i]) - size) < std::abs(static_cast<int>(kShadowSizes[best]) - size))
                best = i;
        return best;
    }
    if (name == "msaa") {
        int samples = config_.get_int("display.msaa", 4);
        int best = 0;
        for (int i = 0; i < 4; ++i)
            if (kMsaaSamples[i] <= samples) best = i;
        return best;
    }
    if (name == "tonemap") {
        std::string t = config_.get_string("display.tonemap", "auto");
        for (int i = 0; i < 4; ++i)
            if (t == kTonemaps[i]) return i;
        return 0;
    }
    return std::nullopt;
}

bool ClientApp::set_display_option(std::string_view name, int value) {
    if (name == "find_trail_style") {
        config_.set_int("display.find_trail_style", static_cast<int>(game::normalize_find_trail_style(value)));
        config_.save(config_path_);
        return true;
    }
    if (name == "grass") {
        // CAdvancedDisplayOptionsWnd's +0x22c: ShowGrass, the scene's flora (virtual +0xb8), saved.
        grass_ = value != 0;
        config_.set_bool("display.grass", grass_);
        config_.save(config_path_);
        return true;
    }
    if (name == "flora_distance") {
        flora_reach_ = std::clamp(static_cast<float>(value), render::kFloraReach, render::kMaxFloraReach);
        config_.set_int("display.flora_distance", static_cast<int>(std::lround(flora_reach_)));
        config_.save(config_path_);
        return true;
    }
    if (name == "neighbour_zones") {
        // The zones past the outdoor zone lines (app/neighbour_zones.h): freed at once when turned off.
        neighbour_zones_ = value != 0;
        config_.set_bool("display.neighbour_zones", neighbour_zones_);
        config_.save(config_path_);
        return true;
    }
    if (name == "horizon_hills") {
        // The distant hills round an outdoor zone (render/horizon.h).
        horizon_hills_ = value != 0;
        config_.set_bool("display.horizon_hills", horizon_hills_);
        config_.save(config_path_);
        return true;
    }
    if (name == "tree_detail") {
        tree_detail_ = std::clamp(value, 50, 200);
        config_.set_int("display.tree_detail", tree_detail_);
        config_.save(config_path_);
        return true;
    }
    if (name == "ground_npcs") {
        config_.set_bool("display.ground_npcs", value != 0);
        config_.save(config_path_);
        if (conn_) conn_->set_ground_npcs(value != 0);
        return true;
    }
    if (set_ring_option(name, value)) return true;
    if (name == "target_ring_cues" || name == "target_ring_xray") {
        (name == "target_ring_cues" ? ring_cues_ : ring_xray_) = value != 0;
        config_.set_bool("display." + std::string(name), value != 0);
        config_.save(config_path_);
        return true;
    }
    if (name == "shadow_clip_plane") {
        // CAdvancedDisplayOptionsWnd's slider: CDisplay_SetShadowClipPlane, saved as ShadowClipPlane.
        shadow_clip_plane_ = std::clamp(value, 0, 100);
        config_.set_int("display.shadow_clip_plane", shadow_clip_plane_);
        config_.save(config_path_);
        return true;
    }
    for (std::string_view kind : {"spell", "environment", "actor"})
        if (name.starts_with(kind) && name.substr(kind.size()) == "_particle_density") {
            // SetDensity: choices 0..3 are 0, 0.25, 0.5, 1; others leave it.
            if (value >= 0 && value <= 3) set_particle_density(kind, game::particle_density_value(value));
            config_.save(config_path_);
            return true;
        }
    for (std::size_t i = 0; i < kSpellFxSourceOptions.size(); ++i)
        if (name == kSpellFxSourceOptions[i]) {
            // Hailstone: Off / Low / Medium / High, on top of the spell particle density.
            int choice = std::clamp(value, 0, 3);
            config_.set_int(std::format("display.{}", name), choice);
            if (spell_particles_)
                spell_particles_->set_source_density(static_cast<game::SpellFxSource>(i), game::particle_density_value(choice));
            config_.save(config_path_);
            return true;
        }
    if (const auto* r = slider_range(name)) {
        float v = r->at_zero + static_cast<float>(std::clamp(value, 0, 20)) * r->per_tick;
        config_.set(std::string(r->key), std::format("{:.2f}", v));
        if (name == "view_distance") view_distance_ = v;
    } else if (name == "lighting") {
        lighting_command(value == 0 ? "modern" : "classic"); // saves and applies
        return true;
    } else if (name == "ao" || name == "distance_blur" || name == "color_grade") {
        config_.set_bool("display." + std::string(name), value != 0);
    } else if (name == "render_scale") {
        config_.set_int("display.render_scale", std::clamp(value, 50, 100));
    } else if (name == "shadows") {
        config_.set("display.shadows", std::to_string(kShadowSizes[std::clamp(value, 0, 3)]));
    } else if (name == "msaa") {
        // Taken at start-up: the renderers' pipelines are built for the sample count.
        config_.set("display.msaa", std::to_string(kMsaaSamples[std::clamp(value, 0, 3)]));
    } else if (name == "tonemap") {
        config_.set("display.tonemap", kTonemaps[std::clamp(value, 0, 3)]);
    } else {
        return false;
    }
    config_.save(config_path_);
    apply_scene_options();
    return true;
}

void ClientApp::lighting_command(std::string_view args) {
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
    std::string a(args);
    for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (a.empty()) {
        lighting_mode_ = lighting_mode_ == render::LightingMode::Classic ? render::LightingMode::Modern
                                                                         : render::LightingMode::Classic;
    } else if (auto mode = render::parse_lighting_mode(a)) {
        lighting_mode_ = *mode;
    } else {
        if (conn_) conn_->ui_message("Usage: /lighting [modern | classic]");
        return;
    }
    bool modern = lighting_mode_ == render::LightingMode::Modern;
    config_.set("display.lighting", modern ? "modern" : "classic");
    config_.save(config_path_);
    apply_scene_options(); // the automatic tonemap follows the mode
    if (conn_)
        conn_->ui_message(modern ? "Lighting: modern (per-pixel sun, sky and point lights)."
                                 : "Lighting: classic (the original's look).");
}

void ClientApp::update_missiles() {
    if (!conn_) return;
    const std::uint32_t now = net::GameConnection::eq_time();
    const auto& world = conn_->world();
    for (game::MissilePacket& p : conn_->take_missiles()) {
        // The constructor: with the source spawn, Send_OP_Animation_0058f540 plays its attack animation and
        // the launch waits for 70 % of it (PlayerClient::CalcAnimLength).
        std::uint32_t delay = 0;
        if (auto it = world.spawns.find(p.source_id); it != world.spawns.end()) {
            const game::Spawn& s = it->second;
            // PlayAttackAnimation reads the source's class and BodyWet (+0xa2: the player's own, other spawns' as
            // WorldState::smooth probes them) and asks NpcManager_IsNewStyleModelRace.
            const auto* rm = game::find_race_model(s.race, s.gender);
            const bool new_style = rm && (rm->flags & game::kNewStyleModel);
            const std::uint8_t body_wet = s.id == world.player_id ? motion_body_wet_ : s.body_wet;
            std::uint8_t anim =
                game::missile_launch_animation(p.skill, p.item_type, p.range, s.race, s.class_id, body_wet, new_style);
            if (anim != 0xfe && anim != 0x4f) conn_->play_animation(s.id, anim);
            std::uint32_t length = 0;
            if (actor_renderer_) {
                if (auto model = actor_renderer_->model_for(s)) {
                    // CalcAnimLength: the clip's length (a new-style model's first variant).
                    std::string code = game::action_clip(
                        anim, [&](std::string_view c) { return model->clip(c) != nullptr; },
                        new_style && model->eqg, {s.player_state, 0, -1});
                    if (const assets::AnimationClip* c = code.empty() ? nullptr : model->clip(code))
                        length = static_cast<std::uint32_t>(c->frame_count) * static_cast<std::uint32_t>(c->frame_ms);
                }
            }
            delay = game::missile_launch_delay(length, p.range);
        }
        log::debug("missile {} from {} at {} ({} type {} range {}), launch in {} ms", p.model, p.source_id, p.target_id,
                   p.item_id, p.item_type, p.range, delay);
        missiles_.add(game::make_missile(p, now, delay));
        // A spell the server flies (item type 20): in the modern lighting its bolt follows the missile.
        if (spell_bolts_ && lighting_mode_ == render::LightingMode::Modern && p.item_type == 20)
            spell_bolts_->missile(missiles_.missiles().back().serial, p.source_id, p.target_id, bolt_clock());
    }
    if (missiles_.missiles().empty()) {
        if (world_objects_) world_objects_->set_missiles({});
        return;
    }
    game::MissileWorld mw;
    mw.zone_id = conn_->zone_id();
    mw.spell = [this](std::uint32_t id) { return conn_->spell_table().get(id); };
    mw.launch_point = [this](std::uint32_t id) -> std::optional<glm::vec3> {
        glm::vec3 point(0.0f);
        return actor_renderer_ && actor_renderer_->drawn_name_position(id, point) ? std::optional(point) : std::nullopt;
    };
    mw.effect = [this](const game::Missile& m, int stage) {
        if (spell_particles_ && spell_effects_) spell_particles_->missile_effect(*conn_, m, stage, actor_renderer_.get());
    };
    mw.ended = [this](const game::Missile& m) {
        if (spell_particles_) spell_particles_->end_missile(m.serial);
    };
    mw.spell_hit = [this](const game::Missile& m) { conn_->spell_missile_hit(m); };
    if (spell_particles_)
        spell_particles_->set_missile_pose([this](std::uint32_t serial) -> std::optional<glm::mat4> {
            for (const auto& m : missiles_.missiles())
                if (m.serial == serial && m.has_actor && !m.done) {
                    constexpr float unit = 2.0f * std::numbers::pi_v<float> / 512.0f;
                    glm::mat4 t = glm::translate(glm::mat4(1), m.actor_position);
                    t = glm::rotate(t, m.heading * unit, glm::vec3(0, 0, 1));
                    t = glm::rotate(t, -m.tilt * unit, glm::vec3(0, 1, 0));
                    return glm::scale(t, glm::vec3(m.actor_scale));
                }
            return std::nullopt;
        });
    mw.spawn = [this, &world](std::uint32_t id) -> std::optional<game::MissileSpawn> {
        auto it = world.spawns.find(id);
        if (it == world.spawns.end()) return std::nullopt;
        const game::Spawn& s = it->second;
        game::MissileSpawn m;
        m.id = s.id;
        m.position = s.position;
        m.avatar_height = s.heights.avatar_height;
        m.height = s.heights.height;
        m.race = s.race;
        m.dead = s.stand_state == 0x78;
        m.speed_run = s.speed_run;
        m.is_npc = s.is_npc;
        m.rider_id = s.rider_id;
        m.has_actor = s.actor_made;
        if (s.physics_step_ms) {
            const auto epoch = static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(start_time_.time_since_epoch()).count());
            m.stepped_at = epoch + (*s.physics_step_ms - 1u);
        }
        return m;
    };
    mw.local_id = world.player_id;
    mw.spawns = [&world, spawn = mw.spawn] {
        std::vector<game::MissileSpawn> out;
        out.reserve(world.spawns.size());
        for (const auto& [id, s] : world.spawns)
            if (auto entry = spawn(id)) out.push_back(*entry);
        return out;
    };
    mw.visible = [this](const glm::vec3& from, const glm::vec3& to) {
        return !collision_ || collision_->line_of_sight(from, to);
    };
    mw.contact = [this, &world](const game::Missile& m, const game::MissileSpawn& target) {
        const auto* spell = conn_->spell_table().get(m.packet.spell);
        if (!m.area_contact || !spell || !spell->has_effect(76)) return;
        if (target.id == world.player_id) conn_->ui_message(conn_->strings().format(0x1a44, {}), 0, 0x108);
        else if (auto it = world.spawns.find(target.id); it != world.spawns.end())
            conn_->ui_message(conn_->strings().format(0x1a45, std::vector<std::string>{game::display_name(it->second.name)}), 0, 0x108);
    };
    mw.sweep = [this](const glm::vec3& from, const glm::vec3& to, float radius) {
        game::SphereSweep sweep = game::SphereSweep::make(from, to, radius);
        if (!sweep.valid()) return false;
        if (collision_) collision_->sweep_sphere(sweep);
        if (world_objects_) world_objects_->sweep_sphere(sweep);
        return sweep.collided;
    };
    mw.inside = [this](const glm::vec3& p) {
        if (!collision_ || !collision_->bounds()) return true;
        const auto& [lo, hi] = *collision_->bounds();
        return p.x >= lo.x && p.y >= lo.y && p.x <= hi.x && p.y <= hi.y;
    };
    // Stepped once per step of the game clock (core/step_clock.h), at the step's time on eq_time's clock: a missile
    // moves every other step (its 32 ms gate), as at 60 frames a second in the original. Per-frame mode: once now.
    if (!step_clock_.fixed()) {
        missiles_.update(now, mw);
    } else {
        const auto real_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_time_).count();
        for (const auto& st : frame_steps_) {
            const std::int64_t behind = real_ms - static_cast<std::int64_t>(st.ms);
            missiles_.update(now - static_cast<std::uint32_t>(std::max<std::int64_t>(behind, 0)), mw);
        }
    }
    if (world_objects_) {
        // The missiles a spell bolt follows are not drawn as their models.
        const SpellBolts* bolts = spell_bolts_.get();
        world_objects_->set_missiles(missiles_.missiles(), [bolts](std::uint32_t serial) {
            return bolts && bolts->follows(serial);
        });
    }
}

} // namespace hailstone::app::client_internal
