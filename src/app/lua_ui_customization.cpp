// PlayerCustomizationWnd: CPlayerCustomizationWnd (game/player_customization.h) on the local player in game, and at
// character creation (the creation window's Appearance button) on the model player of the pre-game scene. The
// window's model steps the features through the model (CharacterLibrary::try_fheb on the model the spawn is drawn
// with, app/customization_actor.h), writes them on the spawn (the renderer dresses it again), and Accept sends
// OP_FaceChange in game (GameConnection::accept_face_change) or keeps them in the creation's character
// (CCharacterCreation_CopyAppearanceFrom; the creation's packet reads the model's). While it shows, the client frames
// the face with the window's camera calls (UiHost::customization_camera, game/char_select_camera.h: the pre-game
// scene's camera at creation, ClientApp's customization camera in game).

#include <memory>
#include <random>
#include <string>

#include "app/customization_actor.h"
#include "app/lua_ui_windows.h"
#include "assets/character.h"
#include "game/appearance_data.h"
#include "game/char_create_look.h"
#include "game/player_customization.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

// The local player in game: the features come from the connection and go back to it (GameConnection::
// set_player_features).
class PlayerActor final : public SpawnActor {
public:
    PlayerActor(UiHost& host, std::mt19937& rng)
        : SpawnActor(host.conn ? host.conn->world().player() : nullptr, host.player_model, host.char_library, rng),
          host_(host) {
        if (host_.conn) f_ = host_.conn->player_features();
    }
    void commit() override {
        if (host_.conn && !(f_ == host_.conn->player_features())) host_.conn->set_player_features(f_);
    }
    std::int32_t profile_race() override {
        return host_.conn && host_.conn->profile() ? static_cast<std::int32_t>(host_.conn->profile()->race) : -1;
    }

private:
    UiHost& host_;
};

class PlayerCustomizationWindow : public StockWindow {
public:
    PlayerCustomizationWindow() : StockWindow("PlayerCustomizationWnd", false), rng_(std::random_device{}()) {}

    // In game on the local player, at character creation (GameState 2, CCharacterCreation's Appearance button) on
    // the model player.
    bool shown(UiHost& host) override { return open_ && usable(host, create_); }
    bool pre_game() const override { return true; } // at character creation too (shown() checks where)
    void set_shown(UiHost& host, bool show) override {
        if (show == open_) return;
        if (show) {
            const bool create = host.game_state == 2;
            if (!usable(host, create)) return;
            std::uint32_t refusal = 0;
            bool ok = with_actor(host, create, [&](SpawnActor& a) {
                return model_.about_to_show(a, host.body_tint_open, create, refusal);
            });
            if (!ok) {
                if (refusal && host.conn) host.conn->system_message(refusal);
                return;
            }
            open_ = true;
            create_ = create;
            publish(host);
            if (host.activate_tip) host.activate_tip(10); // AboutToShow: CTipWnd::Activate(pinstCTipWnd_2, 10, 0)
            return;
        }
        // AboutToHide @0x714620: at GameState 2 CharSelect_PositionCameraForModel(0) and the creation window shows
        // again; in game RestoreSavedCameraType.
        open_ = false;
        model_.about_to_hide();
        host.customization_zoom = -1;
        host.customization_feature = 0;
        host.customization_shown = false;
        host.customization_camera.clear();
    }

    void frame(UiHost& host) override {
        if (!open_) return;
        if (!usable(host, create_)) {
            set_shown(host, false);
            return;
        }
        with_actor(host, create_, [&](SpawnActor& a) {
            model_.frame(a);
            return true;
        });
        publish(host);
    }

    void child(UiHost&, std::string_view id, Child& out) override {
        // A label's colour follows its state (0x88e4a0 / 0x88e4b0: the piece's TextColor or DisabledColor,
        // 128 grey for every label of EQUI_PlayerCustomizationWnd.xml; the heritage labels are green).
        auto label = [&](bool on, bool green) {
            out.enabled = on;
            out.color = !on ? Rgb{128 / 255.0f, 128 / 255.0f, 128 / 255.0f}
                            : green ? Rgb{182 / 255.0f, 253 / 255.0f, 130 / 255.0f} : Rgb{1.0f, 1.0f, 1.0f};
        };
        for (int row = 0; row < game::kCustomizationRows; ++row) {
            if (!*game::customization_text(row)) continue;
            auto on = model_.row_enabled(row);
            if (!on) continue;
            if (id == game::customization_down_button(row) || id == game::customization_up_button(row)) out.enabled = *on;
            else if (id == game::customization_text(row)) label(*on, row == static_cast<int>(game::CustomizationRow::Heritage));
        }
        if (id == "ParentId_Text" && model_.heritage_text_set()) out.text = model_.heritage_text();
        if (id == "Heritage_Text")
            if (auto on = model_.heritage_label_enabled()) label(*on, true);
    }

    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (button != "left" || !usable(host, create_)) return false;
        game::FacialFeatures accepted;
        auto r = game::PlayerCustomizationModel::Result::None;
        with_actor(host, create_, [&](SpawnActor& a) {
            r = model_.click(a, id);
            accepted = a.features();
            return true;
        });
        publish(host);
        if (r == game::PlayerCustomizationModel::Result::Accept) {
            // WndNotification's Accept: the features into the profile; at GameState 2 CopyAppearanceFrom into the
            // creation's character (no packet: Send_OP_CharacterCreate reads the model's), in game OP_FaceChange.
            if (create_) {
                if (host.char_create_look) host.char_create_look->copy_appearance_from(accepted);
            } else if (host.conn) {
                host.conn->accept_face_change();
            }
            set_shown(host, false);
        } else if (r == game::PlayerCustomizationModel::Result::Cancel) {
            set_shown(host, false);
        }
        return true;
    }

private:
    static bool usable(UiHost& host, bool create) {
        return create ? host.game_state == 2 && host.model_player != nullptr : host.in_game && host.conn != nullptr;
    }
    // `f` on the window's actor, whose features then go back to their spawn.
    template <typename F> bool with_actor(UiHost& host, bool create, F&& f) {
        if (create) {
            ModelPlayerActor a(host, rng_);
            bool r = f(a);
            a.commit();
            return r;
        }
        PlayerActor a(host, rng_);
        bool r = f(a);
        a.commit();
        return r;
    }
    void publish(UiHost& host) {
        for (const auto& call : model_.take_camera_calls()) host.customization_camera.push_back(call);
        host.customization_zoom = model_.camera_zoom();
        host.customization_feature = model_.feature();
        host.customization_shown = true;
    }

    game::PlayerCustomizationModel model_;
    std::mt19937 rng_;
    bool create_ = false;
};

} // namespace

void add_customization_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<PlayerCustomizationWindow>());
}

} // namespace hailstone::app
