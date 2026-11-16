#pragma once

// The gamepad's mode, settings and bindings (docs/gamepad.md), shared by the client (app/client_app_gamepad.cpp,
// which plays them) and the hs.gamepad table of the Lua UI (app/lua_ui_gamepad.cpp, which shows and edits them).
// A client extension: RoF2 has no gamepad support.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/gamepad_bindings.h"
#include "platform/gamepad.h"

namespace hailstone { class Config; }

namespace hailstone::app {

// docs/gamepad.md section 2: game (the pad plays), menu (it drives ImGui's navigation in the focused window),
// cursor (it moves and clicks a mouse pointer).
enum class PadMode : std::uint8_t { Game, Menu, Cursor };
const char* pad_mode_name(PadMode m);
std::optional<PadMode> pad_mode_by_name(std::string_view name);

// The gamepad pack's chat input windows (ui/gamepad/chat.lua: the ticker the chat key opens it from, and the log with
// its on-screen keyboard; their ui.window and ImGui names). With the interface locked (/safelock) the windows lose the
// pad as they lose the mouse (__ProcessMouseEvents), but these keep it: it is the pad player's keyboard, which the
// original leaves working, so /safelock can still be typed to unlock.
inline bool pad_chat_window(std::string_view name) { return name == "Chat Ticker" || name == "Chat Log"; }

class Gamepad {
public:
    // The settings and bindings are hailstone.ini's gamepad.* keys of `config`, saved to `path` as they change.
    Gamepad(Config& config, std::filesystem::path path);
    // The settings and bindings read again (a character's settings took over, Config::attach_character).
    void reload();

    // The pad as the window read it this frame (platform::InputState::pad).
    void set_state(const platform::GamepadState* state) { state_ = state ? state : &none_; }
    const platform::GamepadState& state() const { return *state_; }
    // Connected and not turned off (gamepad.enabled).
    bool active() const { return settings_.enabled && state_->connected; }

    PadMode mode() const { return mode_; }
    void set_mode(PadMode m) { mode_ = m; }
    // The cross hotbar set being held in game mode (0 none, 1 LT, 2 RT, 3 both).
    int cross_set() const;

    const game::GamepadSettings& settings() const { return settings_; }
    std::optional<float> setting(std::string_view name) const { return settings_.get(name); }
    bool set_setting(std::string_view name, float value);

    game::GamepadBindings& bindings() { return bindings_; }
    const game::GamepadBindings& bindings() const { return bindings_; }
    // hs.gamepad.binding: the input's own action text, the default for a plain input, "" for a chord without one
    // (its cross hotbar slot or the plain button's action applies); nullopt for text that is no input.
    std::optional<std::string> binding_text(std::string_view input) const;
    // An empty action puts the default back; false for an unknown input or action.
    bool set_binding_text(std::string_view input, std::string_view action);
    bool set_cross_slot(int set, int slot, int bar, int button);

    // hs.gamepad.request_focus: menu mode, with ImGui's focus on that window (the client hands the requests to
    // ui::ImGuiLayer::request_window_focus at the next frame, in order, so the last one has the focus and the ones
    // before it are the pad windows under it: two windows appearing in one frame, a Tip over the merchant, both
    // count, and the one below gets the pad back when the top one closes).
    void request_focus(std::string window);
    std::vector<std::string> take_focus_requests();

    // hs.gamepad.takes_b: a UI window closes itself on B this frame (game mode), so B does nothing else (the
    // original's Escape: the top window closes, the target stays).
    void window_takes_b() { window_takes_b_ = true; }
    bool take_window_takes_b() { return std::exchange(window_takes_b_, false); }

    // Rumble (0..1 strengths, for `ms`), when gamepad.rumble is on: the client gives the window's. NaN or an infinite
    // strength is refused (false); strengths are clamped to 0..1 and `ms` to 0..kMaxRumbleMs, so a script cannot
    // leave the pad shaking.
    static constexpr int kMaxRumbleMs = 5000;
    void set_rumble(std::function<bool(float, float, int)> fn) { rumble_ = std::move(fn); }
    bool rumble(float low, float high, int ms);

private:
    void save();
    Config& config_;
    std::filesystem::path path_;
    game::GamepadSettings settings_;
    game::GamepadBindings bindings_;
    PadMode mode_ = PadMode::Game;
    platform::GamepadState none_;
    const platform::GamepadState* state_ = &none_;
    std::vector<std::string> focus_requests_;
    bool window_takes_b_ = false;
    std::function<bool(float, float, int)> rumble_;
};

} // namespace hailstone::app
