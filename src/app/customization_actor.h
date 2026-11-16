#pragma once

// The player customization window's actor (game::CustomizationActor) on a spawn and the model it is drawn with: the
// local player in game, or the model player of character select and creation (UiHost::model_player). The features
// are read from the spawn when the actor is made and written back by commit(); each SetFHEB call tries the feature on
// a scratch dressing of the spawn's model (CharacterLibrary::try_fheb).

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "app/lua_ui.h"
#include "assets/character.h"
#include "game/appearance_data.h"
#include "game/player_customization.h"
#include "game/world_state.h"

namespace hailstone::app {

class SpawnActor : public game::CustomizationActor {
public:
    // `spawn`: the actor's spawn (null: none); `model`: the model it is drawn with; `library`: its library.
    SpawnActor(const game::Spawn* spawn, std::shared_ptr<const assets::CharacterModel> model,
               const assets::CharacterLibrary* library, std::mt19937& rng)
        : spawn_(spawn), model_(std::move(model)), library_(library), rng_(rng) {
        if (spawn_) f_ = features_of(spawn_->appearance);
    }
    // The spawn's features as the actor keeps them.
    static game::FacialFeatures features_of(const game::Appearance& a) {
        game::FacialFeatures f;
        f.face = a.face, f.hair_color = a.hair_color, f.beard_color = a.beard_color;
        f.eye1 = a.eye_color1, f.eye2 = a.eye_color2, f.hair_style = a.hair_style, f.beard = a.beard;
        f.heritage = static_cast<std::int32_t>(a.drakkin_heritage);
        f.tattoo = static_cast<std::int32_t>(a.drakkin_tattoo);
        f.details = static_cast<std::int32_t>(a.drakkin_details);
        return f;
    }
    static void put_features(game::Appearance& a, const game::FacialFeatures& f) {
        a.face = f.face, a.hair_color = f.hair_color, a.beard_color = f.beard_color;
        a.eye_color1 = f.eye1, a.eye_color2 = f.eye2, a.hair_style = f.hair_style, a.beard = f.beard;
        a.drakkin_heritage = static_cast<std::uint32_t>(f.heritage);
        a.drakkin_tattoo = static_cast<std::uint32_t>(f.tattoo);
        a.drakkin_details = static_cast<std::uint32_t>(f.details);
    }

    // The features onto the spawn (the renderer dresses it again).
    virtual void commit() = 0;

    bool has_actor() override { return spawn_ && model_; }
    bool luclin_model() override { return model_ && model_->luclin; }
    bool new_style_model() override { return model_ && model_->eqg; }
    std::int32_t race() override { return spawn_ ? spawn_->race : 0; }
    std::uint8_t gender() override { return spawn_ ? spawn_->gender : 0; }
    std::uint8_t class_id() override { return spawn_ ? spawn_->class_id : 0; }
    game::FacialFeatures& features() override { return f_; }

    std::uint32_t set_fheb(std::uint8_t id, std::uint8_t type) override { return try_fheb(id, type, false); }
    std::uint32_t set_fheb_color(std::uint8_t color, std::uint8_t type) override { return try_fheb(color, type, true); }

    void set_facial_feature_new_model(std::int32_t value, std::int32_t type) override {
        // ActorClient::SetFacialFeatureNewModel @0x40c7d0: a value past the customization count is 0; the hair and
        // beard styles keep their colours only while those are in range; the eyes set both; the heritage goes by its
        // index (SetHeritage(heritage, 1)).
        const game::CustomizationCounts c = counts();
        auto clamp = [](std::int32_t v, std::int32_t count) { return count <= v ? 0 : v; };
        switch (type) {
        case 1: f_.face = static_cast<std::uint8_t>(clamp(value, c.faces)); break;
        case 2:
            f_.hair_style = static_cast<std::uint8_t>(clamp(value, c.hair_styles));
            if (c.hair_colors <= f_.hair_color) f_.hair_color = 0;
            break;
        case 3: f_.hair_color = static_cast<std::uint8_t>(clamp(value, c.hair_colors)); break;
        case 4: f_.eye1 = f_.eye2 = static_cast<std::uint8_t>(clamp(value, c.eyes)); break;
        case 5:
            f_.beard = static_cast<std::uint8_t>(clamp(value, c.beards));
            if (c.hair_colors <= f_.beard_color) f_.beard_color = 0;
            break;
        case 6: f_.beard_color = static_cast<std::uint8_t>(clamp(value, c.hair_colors)); break;
        case 8: {
            auto list = heritages();
            std::int32_t i = clamp(value, static_cast<std::int32_t>(list.size()));
            if (i < static_cast<std::int32_t>(list.size())) game::actor_set_heritage(*this, list[static_cast<std::size_t>(i)], true);
            break;
        }
        case 9: f_.tattoo = clamp(value, c.tattoos); break;
        case 10: f_.details = clamp(value, c.facial_attachments); break;
        default: break;
        }
        commit();
    }
    void apply_facial_features() override { commit(); }
    void set_heritage(std::int32_t heritage) override {
        if (heritage >= 0) f_.heritage = heritage;
        commit();
    }
    void swap_face(std::uint8_t face) override {
        f_.face = face;
        commit();
    }
    game::CustomizationCounts counts() override {
        game::CustomizationCounts c;
        const game::AppearanceData* d = data();
        if (const game::CustomizationEntry* e = d ? d->customization.find(race(), f_.heritage, gender()) : nullptr) {
            c.faces = e->faces;
            c.hair_styles = e->hair_styles;
            c.hair_colors = static_cast<std::int32_t>(e->colors.size());
            c.eyes = e->eyes;
            c.beards = e->beards;
            c.tattoos = e->tattoos;
            c.facial_attachments = e->facial_attachments;
        }
        return c;
    }
    std::vector<std::int32_t> heritages() override {
        const game::AppearanceData* d = data();
        return d ? d->customization.heritages(race(), class_id(), gender()) : std::vector<std::int32_t>{};
    }
    std::string heritage_name() override {
        const game::AppearanceData* d = data();
        const game::CustomizationEntry* e = d ? d->customization.find(race(), f_.heritage, gender()) : nullptr;
        return e ? e->name : std::string();
    }
    std::int32_t random() override { return static_cast<std::int32_t>(rng_() & 0x7fffffff); }
    std::int32_t random_low_high(std::int32_t low, std::int32_t high) override {
        if (high < low) return low;
        return std::uniform_int_distribution<std::int32_t>(low, high)(rng_);
    }

protected:
    const game::Spawn* spawn_;
    std::shared_ptr<const assets::CharacterModel> model_;
    const assets::CharacterLibrary* library_;
    std::mt19937& rng_;
    game::FacialFeatures f_;

private:
    const game::AppearanceData* data() const { return library_ ? library_->appearance_data() : nullptr; }
    std::uint32_t try_fheb(std::uint8_t value, std::uint8_t type, bool color) {
        if (!spawn_ || !library_ || !model_) return 0;
        game::Appearance a = spawn_->appearance;
        put_features(a, f_);
        std::uint32_t took = library_->try_fheb(*model_, a, value, type, color);
        f_ = features_of(a);
        commit();
        return took;
    }
};

// The model player of character select and creation (UiHost::model_player): the features go straight onto its spawn.
class ModelPlayerActor final : public SpawnActor {
public:
    ModelPlayerActor(UiHost& host, std::mt19937& rng)
        : SpawnActor(host.model_player, host.model_player && host.model_for ? host.model_for(*host.model_player) : nullptr,
                     host.char_library, rng),
          host_(host) {}
    void commit() override {
        if (host_.model_player) put_features(host_.model_player->appearance, f_);
    }
    // The race the creation window picked (the window character's profile +0x3370).
    std::int32_t profile_race() override { return race(); }

private:
    UiHost& host_;
};

} // namespace hailstone::app
