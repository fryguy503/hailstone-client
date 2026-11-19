// ClientApp: the scene behind character select and creation.
//
// The original keeps a world display up before the game: CEverQuest::StartNetworkGame @0x53bdd0 makes
// the local PC and player (pinstLocalPlayer = pinstModelPlayer) at 0, 0, 7 in zone 0xbe and starts the world display
// there (0x49d920: "clz", the CharSelectZone; Yon 5000), 0x517840 puts the camera (Cameras[7]) on it;
// DoCharacterSelection @0x53aac0 sets the time to 12:00, the sun light, no shadows, Yon 2500 and fog 150 .. 2500
// (grey 200), and CharSelect_PositionCameraForModel(0). The model player is the character selected
// (CCharacterListWnd::SelectCharacter @0x42fec0: renamed, class, level, deity, zone, then
// CEverQuest::SetPlayerAppearanceFromPInfo @0x51c050 and the spot 0, 0, 0 + 1, heading 0, placed on the floor by
// PlayerClient_PlaceOnFloorForCharSelect @0x59ecf0), hidden while none is (CCharacterListWnd::OnProcessFrame: the
// actor's vtable +0x34). Creation (DoNewCharacterCreation @0x53a760) keeps the scene and switches the model to what is
// being made (game/char_create_look.h). The camera is game/char_select_camera.h's port.
//
// The scene is drawn by the client's renderers as a zone (the modern lighting when it is on); the sky is sky 1 (the
// original's at character select is not known: docs/unknowns.md).

#include "app/client_app_internal.h"

#include "app/customization_actor.h"
#include "game/appearance.h"
#include "game/player_customization.h"
#include "game/spawn_heights.h"

namespace hailstone::app::client_internal {

namespace {

constexpr const char* kCharSelectZone = "clz"; // str_CharSelectZone_clz (zone 0xbe)
constexpr float kStepSeconds = 0.05f;          // the loops' TimeStamp + 0x32

} // namespace

game::Spawn* ClientApp::model_player() {
    if (!pre_game_world_) return nullptr;
    auto it = pre_game_world_->spawns.find(kModelPlayerId);
    return it == pre_game_world_->spawns.end() ? nullptr : &it->second;
}

void ClientApp::load_pre_game_scene() {
    log::info("character select: loading the scene ({})", kCharSelectZone);
    load_zone(kCharSelectZone);
    pre_game_loaded_ = zone_.has_value();
    // DoCharacterSelection: WorldData_SetTime(12, 0), the sun light, SetYon(2500), fog 150 .. 2500 in grey 200,
    // shadows off (the original's; the modern lighting keeps its own).
    game::ZoneFogInput fog;
    fog.fog_enabled = true;
    fog.fog_color = glm::vec3(200.0f / 255.0f);
    fog.fog_minclip = 150.0f;
    fog.fog_maxclip = 2500.0f;
    fog.minclip = 10.0f;
    fog.maxclip = 2500.0f;
    fog.time_type = 1;
    zone_fog_ = fog;
    view_ = game::compute_view_distance(fog, opts_.clip_plane);
    scene_fog_.set_zone(fog, true);
    player_light_.reset();
    camera_region_ = 0;
    outdoor_light_ = true;
    time_type_ = 1;
    sky_renderer_.reset();
    if (zone_renderer_) {
        try {
            if (auto sky = assets::load_sky(game_, 1))
                sky_renderer_ = std::make_unique<render::SkyRenderer>(vk_, *sky, shader_dir_, &game_, zone_->name);
        } catch (const std::exception& e) {
            log::warn("character select: no sky: {}", e.what());
        }
    }
    pre_game_world_.emplace();
    if (conn_) pre_game_world_->height_tables = conn_->world().height_tables;
    pre_game_world_->old_model_scale = true; // 0xddfba4 stays 1 until the first OP_NewZone
    pre_game_world_->player_id = kModelPlayerId;
    game::Spawn m;
    m.id = kModelPlayerId;
    m.race = 1;
    m.class_id = 1;
    m.level = 1;
    m.position = glm::vec3(0.0f, 0.0f, 7.0f);
    pre_game_world_->spawns.emplace(m.id, std::move(m));
    pre_game_hidden_ = true;
    pre_game_shown_.clear();
    pre_game_model_key_ = {};
    create_choice_ = {};
    pre_game_zoom_ = -1;
    pre_game_feature_ = 0;
    pre_game_step_ = 0.0;
    pre_game_saved_fov_ = camera_.fov_units;
    // 0x517840: SetCameraType(7) and the first placement.
    game::Spawn* p = model_player();
    pre_game_world_->set_heights(*p);
    game::start_network_game_camera(pre_game_camera_, p->heights.view_height);
}

game::CCreateInputs ClientApp::ccreate_inputs() const {
    if (!pre_game_world_) return {};
    auto it = pre_game_world_->spawns.find(kModelPlayerId);
    if (it == pre_game_world_->spawns.end()) return {};
    return ccreate_inputs_for(it->second, pre_game_world_->height_tables.get());
}

game::CCreateInputs ClientApp::ccreate_inputs_for(const game::Spawn& m, const game::HeightTables* t) const {
    game::CCreateInputs in;
    in.race = m.race;
    in.gender = m.gender;
    if (actor_renderer_)
        if (auto model = actor_renderer_->choose_model(m).model) {
            in.new_style_model = model->eqg;
            in.model_settings = model->luclin;
        }
    in.can_be_bald = game::can_be_bald(m.race, m.gender);
    in.heading = m.heading;
    in.size = m.heights.height;
    in.view_height = m.heights.view_height;
    if (t) {
        const std::string_view code = game::height_model_code(m.race, m.gender);
        in.ccc_distance = t->ccc_distance(code);
        in.ccc_height = t->ccc_height(code);
    }
    return in;
}

void ClientApp::switch_model_player() {
    // CharSelect_SwitchPlayerModel @0x42f2c0: the new actor, Height -1 and SetHeights(0, 1) (the race's default
    // size), the sounds, the actor built again (CreatePlayerActor), the face swapped in; for the local player
    // PlaceOnFloorForCharSelect, UpdatePlayerActor, the view actor and CharSelect_PositionCameraForModel(0).
    game::Spawn* m = model_player();
    if (!m) return;
    m->heights = {};
    m->heights.height = -1.0f;
    m->size = 0.0f;
    pre_game_world_->set_heights(*m);
    m->actor_made = false;
    if (actor_renderer_)
        pre_game_world_->make_actors([this](const game::Spawn& s) -> std::optional<game::WorldState::ActorModel> {
            auto choice = actor_renderer_->choose_model(s);
            return game::WorldState::ActorModel{choice.model && choice.model->find_bone("TUNIC_POINT_TRACK") >= 0,
                                                choice.human_fallback};
        });
    // The spot (0, 0, 0 + 1), heading 0, then the floor below (GetPlayerFloorHeight: a sweep down from there).
    m->position = glm::vec3(0.0f, 0.0f, 1.0f);
    m->heading = 0.0f;
    if (collision_)
        if (auto floor = collision_->floor_below(m->position, 0.0f)) m->position.z = *floor + m->heights.avatar_height;
    m->display_position = m->position;
    m->display_heading = m->heading;
    m->velocity = glm::vec3(0.0f);
    pre_game_model_key_ = {m->race, m->gender, m->class_id};
    game::CCreateResult r = game::set_ccreate_camera(pre_game_camera_, 0, ccreate_inputs());
    if (r.fov) camera_.fov_units = *r.fov;
    log::info("model player: race {} gender {} class {}, Height {:.2f}, at z {:.2f} (avatar {:.2f}, view {:.2f})",
              m->race, m->gender, m->class_id, m->heights.height, m->position.z, m->heights.avatar_height,
              m->heights.view_height);
}

void ClientApp::show_select_model() {
    // CCharacterListWnd::SelectCharacter / SetPlayerAppearanceFromPInfo for the selected row; hidden without one.
    game::Spawn* m = model_player();
    if (!m || !conn_) return;
    const auto& list = conn_->characters();
    const int i = char_view_.selected;
    if (i < 0 || i >= static_cast<int>(list.size())) {
        pre_game_hidden_ = true;
        pre_game_shown_.clear();
        return;
    }
    // The same row of the same list: nothing to look at again (the key below is built only when either changed).
    const std::pair<std::uint32_t, int> source{conn_->characters_serial(), i};
    if (!pre_game_shown_.empty() && pre_game_shown_ != "create" && source == pre_game_shown_for_) {
        pre_game_hidden_ = false;
        return;
    }
    pre_game_shown_for_ = source;
    const net::CharacterSelectEntry& e = list[static_cast<std::size_t>(i)];
    // SetPlayerAppearanceFromPInfo @0x51c050 uses the roster's ShroudRace and ShroudClass for the preview.
    const auto model_race = e.shroud_race ? e.shroud_race : e.race;
    const auto model_class = e.shroud_class ? e.shroud_class : e.class_;
    // What the model shows changes with the row and with what the list says of it (a new list after a creation).
    std::string key = e.name + "/" + std::to_string(model_race) + "/" + std::to_string(model_class) + "/" +
                      std::to_string(e.gender) + "/" +
                      std::to_string(e.face) + "/" + std::to_string(e.level);
    for (const auto& q : e.equip) key += "/" + std::to_string(q.material) + ":" + std::to_string(q.color);
    pre_game_hidden_ = false;
    if (key == pre_game_shown_) return;
    pre_game_shown_ = key;
    m->name = e.name;
    m->class_id = model_class;
    m->level = e.level;
    m->deity = e.deity;
    m->race = static_cast<std::uint16_t>(model_race);
    m->gender = e.gender;
    game::Appearance& a = m->appearance;
    a = {};
    a.face = e.face;                // +0x55
    a.beard = e.beard;              // +0x14d
    a.beard_color = e.beard_color;  // +0x149
    a.hair_style = e.hair_style;    // +0x14c
    a.hair_color = e.hair_color;    // +0x148
    a.eye_color1 = e.eye_color1;    // +0x14a
    a.eye_color2 = e.eye_color2;    // +0x14b
    a.texture = e.unknown15;        // +0x130 TextureType
    a.helm = e.unknown19;           // +0x133 HeadType
    a.drakkin_heritage = e.drakkin_heritage;
    a.drakkin_tattoo = e.drakkin_tattoo;
    a.drakkin_details = e.drakkin_details;
    a.class_id = model_class;
    // RefreshEquipSlot for the nine slots (material, the hero's forge model, the tint), then the held models
    // IT<primary> / IT<secondary> ("0" without one).
    for (std::size_t s = 0; s < 9; ++s) {
        a.material[s] = e.equip[s].material;
        a.material_variation[s] = e.equip[s].unknown1;
        a.elite_material[s] = e.equip[s].elite_material;
        a.hero_forge[s] = e.equip[s].hero_forge_model;
        a.new_armor_type[s] = e.equip[s].material2;
        a.tint[s] = e.equip[s].color;
    }
    a.material[7] = e.primary_id_file > 0 ? e.primary_id_file : 0;
    a.material[8] = e.secondary_id_file > 0 ? e.secondary_id_file : 0;
    game::normalize_appearance(a, model_race);
    log::info("character select: {} (race {} gender {} face {} hair {} / {} beard {} / {} eyes {} / {} heritage {} "
              "tattoo {} details {})", e.name, model_race, e.gender, e.face, e.hair_style, e.hair_color, e.beard,
              e.beard_color, e.eye_color1, e.eye_color2, e.drakkin_heritage, e.drakkin_tattoo, e.drakkin_details);
    log::debug("character select: {} materials {} {} {} {} {} {} {} held {} {} texture {} head {}", e.name, a.material[0],
               a.material[1], a.material[2], a.material[3], a.material[4], a.material[5], a.material[6], a.material[7],
               a.material[8], e.unknown15, e.unknown19);
    switch_model_player();
}

void ClientApp::sync_create_model() {
    // The client's own creation screen (the stock CharacterCreation draws its own and dresses the model itself,
    // app/lua_ui_login.cpp): its race, gender and class onto the model, with CCharacterCreation's look.
    game::Spawn* m = model_player();
    if (!m) return;
    std::uint32_t cls = create_view_.class_id;
    if (create_view_.option >= 0 && create_view_.option < static_cast<int>(create_view_.options.size()))
        cls = create_view_.options[static_cast<std::size_t>(create_view_.option)].class_id;
    const std::array<std::uint32_t, 3> choice{create_view_.race, create_view_.gender, cls};
    if (choice == create_choice_) return;
    const std::array<std::uint32_t, 3> was = create_choice_;
    create_choice_ = choice;
    m->race = static_cast<std::uint16_t>(choice[0]);
    m->gender = static_cast<std::uint8_t>(choice[1]);
    m->class_id = static_cast<std::uint8_t>(choice[2]);
    m->appearance.class_id = m->class_id;
    ModelPlayerActor a(ui_host_, pre_game_rng_);
    if (was[0] != choice[0]) char_create_look_.race_changed(a);
    else if (was[1] != choice[1]) char_create_look_.gender_changed(a);
    else char_create_look_.class_changed(a);
    a.commit();
}

void ClientApp::update_pre_game(float dt) {
    const bool select = screen_ == Screen::Characters || screen_ == Screen::Create;
    if (!select || !conn_) {
        ui_host_.model_player = nullptr;
        return;
    }
    if (!pre_game_loaded_) {
        // A failed load is not retried every frame (each try rebuilds the renderers and reports the error again):
        // start_game, a new connection, tries again.
        if (pre_game_failed_) return;
        load_pre_game_scene();
        if (!pre_game_loaded_) {
            pre_game_failed_ = true;
            return;
        }
    }
    game::Spawn* m = model_player();
    if (!m) return;
    // What the windows reach of the model (the creation window and the customization window dress it).
    ui_host_.model_player = m;
    ui_host_.char_create_look = &char_create_look_;
    ui_host_.char_library = actor_renderer_ ? &actor_renderer_->library() : nullptr;
    if (!ui_host_.model_for)
        ui_host_.model_for = [this](const game::Spawn& s) -> std::shared_ptr<const assets::CharacterModel> {
            return actor_renderer_ ? actor_renderer_->choose_model(s).model : nullptr;
        };
    if (screen_ == Screen::Characters) {
        if (create_choice_ != std::array<std::uint32_t, 3>{}) {
            // Back from creation: SelectCharacter shows the row again.
            create_choice_ = {};
            pre_game_shown_.clear();
        }
        show_select_model();
    } else {
        if (pre_game_shown_ != "create") {
            // ApplyNewPCToPlayer: the nine equipment slots cleared and refreshed from the new character, who has no
            // items, the held models 7 and 8 "0": creation's model is bare.
            game::Appearance& a = m->appearance;
            a.material = {};
            a.material_variation = {};
            a.elite_material = {};
            a.tint = {};
            a.hero_forge = {};
            a.new_armor_type = {};
            a.texture = game::kNoTexture;
            a.helm = 0;
            m->name.clear();
        }
        pre_game_shown_ = "create";
        pre_game_hidden_ = false;
        const bool stock = ui_api_ && ui_api_->replaces("CharacterCreation");
        if (!stock) sync_create_model();
        else if (create_choice_ == std::array<std::uint32_t, 3>{})
            create_choice_ = {m->race, m->gender, m->class_id}; // the stock window dressed it
    }
    // A new race, gender or class switches the model (CharSelect_SwitchPlayerModel).
    if (std::array<std::uint32_t, 3>{m->race, m->gender, m->class_id} != pre_game_model_key_) switch_model_player();

    // The customization window at creation: its camera calls (AboutToShow's face camera, its steps' zoom and face
    // camera) in order; AboutToHide's CharSelect_PositionCameraForModel(0).
    game::CCreateInputs in = ccreate_inputs();
    auto apply = [&](const game::CCreateResult& r) {
        if (r.fov) camera_.fov_units = *r.fov;
        if (r.animation) {
            m->action = *r.animation;
            ++m->action_seq;
        }
    };
    const bool customizing = screen_ == Screen::Create && ui_host_.customization_shown;
    if (customizing) {
        for (const auto& call : std::exchange(ui_host_.customization_camera, {}))
            apply(game::customization_camera(pre_game_camera_, call, in));
        pre_game_zoom_ = ui_host_.customization_zoom;
        pre_game_feature_ = ui_host_.customization_feature;
    } else if (pre_game_zoom_ >= 0) {
        apply(game::set_ccreate_camera(pre_game_camera_, 0, in));
        pre_game_zoom_ = -1;
        pre_game_feature_ = 0;
    }
    // The loops' 50 ms camera step.
    pre_game_step_ += dt;
    while (pre_game_step_ >= kStepSeconds) {
        pre_game_step_ -= kStepSeconds;
        if (screen_ == Screen::Characters) game::char_select_camera_tick(pre_game_camera_, m->heading);
        else apply(game::char_create_camera_tick(pre_game_camera_, customizing ? pre_game_feature_ : 0, in));
    }
    // EQCamera_User::vf2 on the model player.
    game::UserCameraTarget target;
    target.position = m->position;
    target.eye_height = game::eye_height(m->heights, m->race);
    game::UserCameraPose pose = game::user_camera_update(pre_game_camera_, target);
    camera_.position = pose.position;
    camera_.yaw = game::heading_to_yaw(pose.heading);
    camera_.pitch = pose.pitch * glm::pi<float>() / 256.0f;
    m->display_position = m->position;
    m->display_heading = m->heading;
}

} // namespace hailstone::app::client_internal
