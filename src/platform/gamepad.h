#pragma once

// The gamepad as platform::Window reads it once per frame (InputState::pad): SDL3's gamepad layout, whatever the
// pad. A client extension: RoF2 has no gamepad support (docs/gamepad.md). The buttons are named by their place, as
// on an Xbox pad (a the bottom face button, b the right, x the left, y the top); game/gamepad_bindings.h gives
// each pad's own labels.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hailstone::platform {

enum class PadButton : std::uint8_t {
    A, B, X, Y, LB, RB, LT, RT, Back, Start, Guide, LS, RS, Up, Down, Left, Right, Touchpad,
    Count
};
inline constexpr std::size_t kPadButtons = static_cast<std::size_t>(PadButton::Count);

// Sticks -1..1 after the dead zone (y down positive, as SDL reports it), triggers 0..1.
enum class PadAxis : std::uint8_t { LX, LY, RX, RY, LT, RT, Count };
inline constexpr std::size_t kPadAxes = static_cast<std::size_t>(PadAxis::Count);

// The pad's family (SDL_GetGamepadType), for its button labels.
enum class PadType : std::uint8_t { Generic, Xbox, PlayStation, Nintendo };

// The names of docs/gamepad.md section 5 (hs.gamepad, the gamepad.bind.<input> settings).
inline constexpr std::array<std::string_view, kPadButtons> kPadButtonNames = {
    "a", "b", "x", "y", "lb", "rb", "lt", "rt", "back", "start", "guide", "ls", "rs", "up", "down", "left", "right",
    "touchpad"};
inline constexpr std::array<std::string_view, kPadAxes> kPadAxisNames = {"lx", "ly", "rx", "ry", "lt", "rt"};

inline std::string_view pad_button_name(PadButton b) {
    return b < PadButton::Count ? kPadButtonNames[static_cast<std::size_t>(b)] : std::string_view();
}
inline std::optional<PadButton> pad_button_by_name(std::string_view name) {
    for (std::size_t i = 0; i < kPadButtons; ++i)
        if (kPadButtonNames[i] == name) return static_cast<PadButton>(i);
    return std::nullopt;
}
inline std::optional<PadAxis> pad_axis_by_name(std::string_view name) {
    for (std::size_t i = 0; i < kPadAxes; ++i)
        if (kPadAxisNames[i] == name) return static_cast<PadAxis>(i);
    return std::nullopt;
}
inline const char* pad_type_name(PadType t) {
    switch (t) {
    case PadType::Xbox: return "xbox";
    case PadType::PlayStation: return "playstation";
    case PadType::Nintendo: return "nintendo";
    default: return "generic";
    }
}

// The pad this frame. LT and RT also count as buttons, held past the trigger threshold (Window::set_gamepad_tuning).
struct GamepadState {
    bool connected = false;
    std::string name;
    PadType type = PadType::Generic;
    std::array<bool, kPadButtons> down{};
    std::array<bool, kPadButtons> pressed{};  // went down this frame (also when it went up again in the same frame)
    std::array<bool, kPadButtons> released{}; // went up this frame
    std::array<float, kPadAxes> axes{};

    bool is_down(PadButton b) const { return down[static_cast<std::size_t>(b)]; }
    bool was_pressed(PadButton b) const { return pressed[static_cast<std::size_t>(b)]; }
    bool was_released(PadButton b) const { return released[static_cast<std::size_t>(b)]; }
    float axis(PadAxis a) const { return axes[static_cast<std::size_t>(a)]; }
};

// A radial dead zone: inside `dead_zone` of the centre the stick is 0; outside it the distance is rescaled so the
// edge of the dead zone is 0 and full deflection 1, keeping the direction.
inline void radial_dead_zone(float& x, float& y, float dead_zone) {
    float mag = std::sqrt(x * x + y * y);
    if (mag <= dead_zone || mag <= 0.0f || dead_zone >= 1.0f) {
        x = y = 0.0f;
        return;
    }
    float scaled = std::fmin((mag - dead_zone) / (1.0f - dead_zone), 1.0f);
    x = x / mag * scaled;
    y = y / mag * scaled;
}

} // namespace hailstone::platform
