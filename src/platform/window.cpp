#include "platform/window.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdexcept>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "core/log.h"
#include "game/mouse_look.h"
#include "platform/paths.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace hailstone::platform {

namespace {
constexpr SDL_Scancode kScancodes[] = {
    SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_Q,
    SDL_SCANCODE_E, SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL, SDL_SCANCODE_ESCAPE,
    SDL_SCANCODE_C, SDL_SCANCODE_TAB, SDL_SCANCODE_F1, SDL_SCANCODE_F8, SDL_SCANCODE_GRAVE, SDL_SCANCODE_1,
    SDL_SCANCODE_RCTRL, SDL_SCANCODE_R,
};
static_assert(std::size(kScancodes) == static_cast<std::size_t>(Key::Count));

// The pad's buttons in PadButton order (LT and RT are axes: none here).
constexpr int kNoButton = -1;
constexpr int kPadSdlButtons[] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kNoButton, kNoButton,
    SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_TOUCHPAD,
};
static_assert(std::size(kPadSdlButtons) == kPadButtons);

std::optional<PadButton> pad_button_of(int sdl_button) {
    for (std::size_t i = 0; i < kPadButtons; ++i)
        if (kPadSdlButtons[i] == sdl_button) return static_cast<PadButton>(i);
    return std::nullopt;
}

PadType pad_type_of(SDL_GamepadType t) {
    switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return PadType::Xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return PadType::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return PadType::Nintendo;
    default: return PadType::Generic;
    }
}

float axis_value(SDL_Gamepad* pad, SDL_GamepadAxis axis) {
    return std::clamp(static_cast<float>(SDL_GetGamepadAxis(pad, axis)) / 32767.0f, -1.0f, 1.0f);
}

// SDL scancodes (USB HID usage numbers) to DirectInput scan codes (dinput.h DIK_*).
struct DikPair {
    SDL_Scancode sdl;
    std::uint8_t dik;
};
constexpr DikPair kDik[] = {
    {SDL_SCANCODE_ESCAPE, 0x01}, {SDL_SCANCODE_1, 0x02}, {SDL_SCANCODE_2, 0x03}, {SDL_SCANCODE_3, 0x04},
    {SDL_SCANCODE_4, 0x05}, {SDL_SCANCODE_5, 0x06}, {SDL_SCANCODE_6, 0x07}, {SDL_SCANCODE_7, 0x08},
    {SDL_SCANCODE_8, 0x09}, {SDL_SCANCODE_9, 0x0a}, {SDL_SCANCODE_0, 0x0b}, {SDL_SCANCODE_MINUS, 0x0c},
    {SDL_SCANCODE_EQUALS, 0x0d}, {SDL_SCANCODE_BACKSPACE, 0x0e}, {SDL_SCANCODE_TAB, 0x0f},
    {SDL_SCANCODE_Q, 0x10}, {SDL_SCANCODE_W, 0x11}, {SDL_SCANCODE_E, 0x12}, {SDL_SCANCODE_R, 0x13},
    {SDL_SCANCODE_T, 0x14}, {SDL_SCANCODE_Y, 0x15}, {SDL_SCANCODE_U, 0x16}, {SDL_SCANCODE_I, 0x17},
    {SDL_SCANCODE_O, 0x18}, {SDL_SCANCODE_P, 0x19}, {SDL_SCANCODE_LEFTBRACKET, 0x1a},
    {SDL_SCANCODE_RIGHTBRACKET, 0x1b}, {SDL_SCANCODE_RETURN, 0x1c}, {SDL_SCANCODE_LCTRL, 0x1d},
    {SDL_SCANCODE_A, 0x1e}, {SDL_SCANCODE_S, 0x1f}, {SDL_SCANCODE_D, 0x20}, {SDL_SCANCODE_F, 0x21},
    {SDL_SCANCODE_G, 0x22}, {SDL_SCANCODE_H, 0x23}, {SDL_SCANCODE_J, 0x24}, {SDL_SCANCODE_K, 0x25},
    {SDL_SCANCODE_L, 0x26}, {SDL_SCANCODE_SEMICOLON, 0x27}, {SDL_SCANCODE_APOSTROPHE, 0x28},
    {SDL_SCANCODE_GRAVE, 0x29}, {SDL_SCANCODE_LSHIFT, 0x2a}, {SDL_SCANCODE_BACKSLASH, 0x2b},
    {SDL_SCANCODE_Z, 0x2c}, {SDL_SCANCODE_X, 0x2d}, {SDL_SCANCODE_C, 0x2e}, {SDL_SCANCODE_V, 0x2f},
    {SDL_SCANCODE_B, 0x30}, {SDL_SCANCODE_N, 0x31}, {SDL_SCANCODE_M, 0x32}, {SDL_SCANCODE_COMMA, 0x33},
    {SDL_SCANCODE_PERIOD, 0x34}, {SDL_SCANCODE_SLASH, 0x35}, {SDL_SCANCODE_RSHIFT, 0x36},
    {SDL_SCANCODE_KP_MULTIPLY, 0x37}, {SDL_SCANCODE_LALT, 0x38}, {SDL_SCANCODE_SPACE, 0x39},
    {SDL_SCANCODE_CAPSLOCK, 0x3a}, {SDL_SCANCODE_F1, 0x3b}, {SDL_SCANCODE_F2, 0x3c}, {SDL_SCANCODE_F3, 0x3d},
    {SDL_SCANCODE_F4, 0x3e}, {SDL_SCANCODE_F5, 0x3f}, {SDL_SCANCODE_F6, 0x40}, {SDL_SCANCODE_F7, 0x41},
    {SDL_SCANCODE_F8, 0x42}, {SDL_SCANCODE_F9, 0x43}, {SDL_SCANCODE_F10, 0x44},
    {SDL_SCANCODE_NUMLOCKCLEAR, 0x45}, {SDL_SCANCODE_SCROLLLOCK, 0x46}, {SDL_SCANCODE_KP_7, 0x47},
    {SDL_SCANCODE_KP_8, 0x48}, {SDL_SCANCODE_KP_9, 0x49}, {SDL_SCANCODE_KP_MINUS, 0x4a},
    {SDL_SCANCODE_KP_4, 0x4b}, {SDL_SCANCODE_KP_5, 0x4c}, {SDL_SCANCODE_KP_6, 0x4d},
    {SDL_SCANCODE_KP_PLUS, 0x4e}, {SDL_SCANCODE_KP_1, 0x4f}, {SDL_SCANCODE_KP_2, 0x50},
    {SDL_SCANCODE_KP_3, 0x51}, {SDL_SCANCODE_KP_0, 0x52}, {SDL_SCANCODE_KP_PERIOD, 0x53},
    {SDL_SCANCODE_NONUSBACKSLASH, 0x56}, {SDL_SCANCODE_F11, 0x57}, {SDL_SCANCODE_F12, 0x58},
    {SDL_SCANCODE_KP_ENTER, 0x9c}, {SDL_SCANCODE_RCTRL, 0x9d}, {SDL_SCANCODE_KP_DIVIDE, 0xb5},
    {SDL_SCANCODE_PRINTSCREEN, 0xb7}, {SDL_SCANCODE_RALT, 0xb8}, {SDL_SCANCODE_PAUSE, 0xc5},
    {SDL_SCANCODE_HOME, 0xc7}, {SDL_SCANCODE_UP, 0xc8}, {SDL_SCANCODE_PAGEUP, 0xc9}, {SDL_SCANCODE_LEFT, 0xcb},
    {SDL_SCANCODE_RIGHT, 0xcd}, {SDL_SCANCODE_END, 0xcf}, {SDL_SCANCODE_DOWN, 0xd0},
    {SDL_SCANCODE_PAGEDOWN, 0xd1}, {SDL_SCANCODE_INSERT, 0xd2}, {SDL_SCANCODE_DELETE, 0xd3},
    {SDL_SCANCODE_LGUI, 0xdb}, {SDL_SCANCODE_RGUI, 0xdc}, {SDL_SCANCODE_APPLICATION, 0xdd},
};
} // namespace

std::uint8_t dik_from_sdl_scancode(int sdl_scancode) {
    for (const auto& p : kDik)
        if (static_cast<int>(p.sdl) == sdl_scancode) return p.dik;
    return 0;
}

std::uint8_t dik_from_sdl_mouse_button(int sdl_button) {
    switch (sdl_button) {
    case SDL_BUTTON_MIDDLE: return 0xf0;
    case SDL_BUTTON_X1: return 0xf1;
    case SDL_BUTTON_X2: return 0xf2;
    case SDL_BUTTON_X2 + 1: return 0xf3;
    case SDL_BUTTON_X2 + 2: return 0xf4;
    case SDL_BUTTON_X2 + 3: return 0xf5;
    default: return 0;
    }
}

Window::Window(const WindowDesc& desc) {
    if (desc.pad_input_in_background) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Two warps to the middle with the system's cursor hidden (the UI drawing the pad's or the window's own cursor)
    // would otherwise put SDL in relative mode by itself on Wayland, holding the cursor until something left it;
    // relative mode is only ever mouse look's and the window cursor's here (platform/mouse_capture.h).
    SDL_SetHint(SDL_HINT_MOUSE_EMULATE_WARP_WITH_RELATIVE, "0");
    // A release's own MoltenVK (macOS has no system Vulkan). SDL loads Vulkan when it makes the window, from this
    // hint's path; without it SDL and volk would only look in the system's folders.
    if (const auto lib = bundled_vulkan_library(install_dir()); !lib.empty()) {
        const std::u8string u = lib.u8string(); // SDL takes UTF-8
        SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, reinterpret_cast<const char*>(u.c_str()));
        bundled_vulkan_ = true;
        log::info("Vulkan: the bundled {}", reinterpret_cast<const char*>(u.c_str()));
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    // Gamepads (docs/gamepad.md): without them the client still runs. SDL sends SDL_EVENT_GAMEPAD_ADDED for the pads
    // already plugged in.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) log::warn("SDL gamepad support unavailable: {}", SDL_GetError());

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (desc.hidden) flags |= SDL_WINDOW_HIDDEN;
    if (desc.maximized) flags |= SDL_WINDOW_MAXIMIZED;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, desc.title.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, desc.width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, desc.height);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<Sint64>(flags));
    if (desc.position && on_a_display(desc.position->first, desc.position->second)) {
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, desc.position->first);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, desc.position->second);
    }
    window_ = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window_) {
        std::string err = SDL_GetError();
        SDL_Quit();
        throw std::runtime_error("SDL_CreateWindow failed: " + err);
    }
    // The normal place to come back to when it is maximized now (geometry() reads it from here on).
    SDL_GetWindowPosition(window_, &normal_.x, &normal_.y);
    normal_.width = desc.width;
    normal_.height = desc.height;
}

bool Window::on_a_display(int x, int y) {
    // A place on a monitor that is still there (a saved place on an unplugged screen is not used).
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    bool on = false;
    for (int i = 0; displays && i < count && !on; ++i) {
        SDL_Rect r;
        if (SDL_GetDisplayBounds(displays[i], &r)) on = x >= r.x && y >= r.y && x < r.x + r.w - 40 && y < r.y + r.h - 40;
    }
    SDL_free(displays);
    return on;
}

WindowGeometry Window::geometry() const {
    WindowGeometry g = normal_;
    SDL_WindowFlags f = SDL_GetWindowFlags(window_);
    g.maximized = (f & SDL_WINDOW_MAXIMIZED) != 0;
    if (!g.maximized && !(f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN))) {
        SDL_GetWindowPosition(window_, &g.x, &g.y);
        SDL_GetWindowSize(window_, &g.width, &g.height);
        normal_ = g;
        normal_.maximized = false;
    }
    return g;
}

void Window::set_geometry(const WindowGeometry& g) {
    SDL_RestoreWindow(window_);
    if (g.width > 0 && g.height > 0) {
        SDL_SetWindowSize(window_, g.width, g.height);
        normal_.width = g.width;
        normal_.height = g.height;
    }
    if (on_a_display(g.x, g.y)) {
        SDL_SetWindowPosition(window_, g.x, g.y);
        normal_.x = g.x;
        normal_.y = g.y;
    }
    if (g.maximized) SDL_MaximizeWindow(window_);
    SDL_SyncWindow(window_);
}

Window::~Window() {
    close_gamepad();
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void Window::set_hold_close_requests(bool hold) {
    hold_close_ = hold;
    // Else SDL follows the last window's close request with SDL_EVENT_QUIT.
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, hold ? "0" : "1");
}

void Window::push_close_request() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    ev.window.windowID = SDL_GetWindowID(window_);
    SDL_PushEvent(&ev);
}

bool Window::poll_events() {
    auto previous = input_.down;
    input_.mouse_dx = input_.mouse_dy = input_.wheel = 0.0f;
    input_.left_clicked = false;
    input_.right_clicked = false;
    input_.key_pressed.fill(false);
    input_.key_released.fill(false);
    input_.key_events.clear();
    input_.pad.pressed.fill(false);
    input_.pad.released.fill(false);
    pointer_.wheel = 0.0f;
    bool activity = false;
    auto key_event = [this](std::uint8_t code, bool down, bool repeat) {
        input_.key_events.push_back({code, down, repeat});
        if (down && !repeat && !input_.key_down[code]) input_.key_pressed[code] = true;
        if (!down && input_.key_down[code]) input_.key_released[code] = true;
        input_.key_down[code] = down;
    };

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_FINGER_DOWN: activity = true; break;
        default: break;
        }
        if (swap_buttons_ && (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP)) {
            if (ev.button.button == SDL_BUTTON_LEFT) ev.button.button = SDL_BUTTON_RIGHT;
            else if (ev.button.button == SDL_BUTTON_RIGHT) ev.button.button = SDL_BUTTON_LEFT;
        }
        if (fast_.active) {
            // The window's own cursor (set_pointer_speed): the events carry it.
            int w = 0, h = 0;
            SDL_GetWindowSize(window_, &w, &h);
            if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                bool leave = false;
                if (!capture_.captured()) { // mouse look holds the cursor still
                    game::PointerStep step = game::pointer_step(fast_.x, fast_.y, ev.motion.xrel, ev.motion.yrel,
                                                                pointer_speed_, static_cast<float>(w),
                                                                static_cast<float>(h));
                    fast_.x = step.x;
                    fast_.y = step.y;
                    leave = step.leave && pointer_grab_;
                }
                ev.motion.x = fast_.x;
                ev.motion.y = fast_.y;
                if (leave) {
                    fast_left_ = true;
                    release_fast_pointer(true);
                }
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                ev.button.x = fast_.x;
                ev.button.y = fast_.y;
            } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                ev.wheel.mouse_x = fast_.x;
                ev.wheel.mouse_y = fast_.y;
            }
        }
        if (ev.type == SDL_EVENT_WINDOW_MOUSE_ENTER) fast_left_ = false;
        if (listener_) listener_(ev);
        switch (ev.type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (hold_close_) {
                close_requested_ = true;
                break;
            }
            return false;
        case SDL_EVENT_QUIT:
            return false;
        case SDL_EVENT_MOUSE_MOTION:
            input_.mouse_dx += ev.motion.xrel;
            input_.mouse_dy += ev.motion.yrel;
            if (pointer_.active && !capture_.captured()) { // the mouse moves the pad's pointer too
                pointer_.x = ev.motion.x;
                pointer_.y = ev.motion.y;
                if (ev.motion.xrel != 0.0f || ev.motion.yrel != 0.0f) pointer_.mouse_used = true;
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            open_gamepad(ev.gdevice.which); // the most recent pad is the one used
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (ev.gdevice.which == pad_id_) {
                close_gamepad();
                // Another pad still plugged in takes over (the most recently connected).
                int count = 0;
                SDL_JoystickID* ids = SDL_GetGamepads(&count);
                for (int i = count - 1; ids && i >= 0 && !pad_; --i)
                    if (ids[i] != ev.gdevice.which) open_gamepad(ids[i]);
                SDL_free(ids);
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            // A press and release within one frame still counts as a press (read_gamepad keeps it).
            if (ev.gbutton.which == pad_id_)
                if (auto b = pad_button_of(ev.gbutton.button)) {
                    auto i = static_cast<std::size_t>(*b);
                    (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? input_.pad.pressed : input_.pad.released)[i] = true;
                }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (pointer_.active && !capture_.captured()) {
                pointer_.x = ev.button.x;
                pointer_.y = ev.button.y;
                pointer_.mouse_used = true;
            }
            if (ev.button.button == SDL_BUTTON_LEFT) {
                input_.left_clicked = true;
                input_.mouse_x = ev.button.x;
                input_.mouse_y = ev.button.y;
            }
            // A click shorter than a frame is up again by the time the button state is read (low frame rates).
            if (ev.button.button == SDL_BUTTON_RIGHT) input_.right_clicked = true;
            if (std::uint8_t code = dik_from_sdl_mouse_button(ev.button.button)) key_event(code, true, false);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (std::uint8_t code = dik_from_sdl_mouse_button(ev.button.button)) key_event(code, false, false);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            input_.wheel += ev.wheel.y;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (std::uint8_t code = dik_from_sdl_scancode(static_cast<int>(ev.key.scancode)))
                key_event(code, ev.type == SDL_EVENT_KEY_DOWN, ev.key.repeat);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // Keys released while unfocused never report: let them all go.
            for (std::size_t c = 0; c < kKeyCodes; ++c)
                if (input_.key_down[c]) {
                    input_.key_down[c] = false;
                    input_.key_released[c] = true;
                    input_.key_events.push_back({static_cast<std::uint8_t>(c), false, false});
                }
            break;
        default:
            break;
        }
    }
    pushed_text_.clear(); // the queue is empty: every pushed text event has been read (and copied by its reader)

    const bool* keys = SDL_GetKeyboardState(nullptr);
    for (std::size_t i = 0; i < input_.down.size(); ++i) {
        input_.down[i] = keys[kScancodes[i]];
        input_.pressed[i] = input_.down[i] && !previous[i];
    }
    float mx = 0.0f, my = 0.0f;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
    input_.right_button = (buttons & (swap_buttons_ ? SDL_BUTTON_LMASK : SDL_BUTTON_RMASK)) != 0;
    input_.left_button = (buttons & (swap_buttons_ ? SDL_BUTTON_RMASK : SDL_BUTTON_LMASK)) != 0;
    if (!input_.left_clicked) {
        input_.mouse_x = pointer_.active ? pointer_.x : fast_.active ? fast_.x : mx;
        input_.mouse_y = pointer_.active ? pointer_.y : fast_.active ? fast_.y : my;
    }
    update_fast_pointer();
    read_gamepad();
    if (!activity)
        activity = input_.left_button || input_.right_button ||
                   std::any_of(input_.key_down.begin(), input_.key_down.end(), [](bool d) { return d; }) ||
                   std::any_of(input_.pad.down.begin(), input_.pad.down.end(), [](bool d) { return d; }) ||
                   std::any_of(input_.pad.axes.begin(), input_.pad.axes.end(), [](float a) { return a != 0.0f; });
    input_.user_activity = activity;
    geometry(); // keeps the normal place up to date (what is saved while maximized)
    return true;
}

bool Window::attach_virtual_gamepad() {
    if (virtual_pad_) return true;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x045e; // an Xbox One S pad, so its type and glyphs are the Xbox's
    desc.product_id = 0x02ea;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Virtual gamepad (input script)";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (!id) {
        log::warn("virtual gamepad: {}", SDL_GetError());
        return false;
    }
    virtual_pad_ = SDL_OpenJoystick(id);
    return virtual_pad_ != nullptr;
}

void Window::set_virtual_pad_button(int sdl_button, bool down) {
    if (virtual_pad_) SDL_SetJoystickVirtualButton(virtual_pad_, sdl_button, down);
}

void Window::set_virtual_pad_axis(int sdl_axis, float value) {
    if (virtual_pad_)
        SDL_SetJoystickVirtualAxis(virtual_pad_, sdl_axis, static_cast<Sint16>(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

void Window::push_key(int sdl_scancode, bool down) {
    SDL_Event ev{};
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.windowID = SDL_GetWindowID(window_);
    ev.key.scancode = static_cast<SDL_Scancode>(sdl_scancode);
    ev.key.key = SDL_GetKeyFromScancode(ev.key.scancode, SDL_KMOD_NONE, false);
    ev.key.down = down;
    // A pushed modifier key holds its modifier for the keys and clicks after it, as the OS reports it.
    SDL_Keymod bit = SDL_KMOD_NONE;
    switch (ev.key.scancode) {
    case SDL_SCANCODE_LALT: bit = SDL_KMOD_LALT; break;
    case SDL_SCANCODE_RALT: bit = SDL_KMOD_RALT; break;
    case SDL_SCANCODE_LCTRL: bit = SDL_KMOD_LCTRL; break;
    case SDL_SCANCODE_RCTRL: bit = SDL_KMOD_RCTRL; break;
    case SDL_SCANCODE_LSHIFT: bit = SDL_KMOD_LSHIFT; break;
    case SDL_SCANCODE_RSHIFT: bit = SDL_KMOD_RSHIFT; break;
    default: break;
    }
    pushed_mods_ = down ? (pushed_mods_ | bit) : (pushed_mods_ & ~bit);
    ev.key.mod = static_cast<SDL_Keymod>(pushed_mods_);
    SDL_PushEvent(&ev);
}

void Window::push_text(const std::string& text) {
    // SDL copies the event but not the text it points at, and SDL3 has no public way to hand it memory
    // it frees with the event. The text lives in a deque (whose push_back moves no element, so a short
    // string's inline buffer stays put) until poll() has drained the queue.
    pushed_text_.push_back(text);
    SDL_Event ev{};
    ev.type = SDL_EVENT_TEXT_INPUT;
    ev.text.windowID = SDL_GetWindowID(window_);
    ev.text.text = pushed_text_.back().c_str();
    if (!SDL_PushEvent(&ev)) pushed_text_.pop_back(); // filtered or the queue is full: nothing points at it
}

void Window::push_mouse_motion(float x, float y) {
    SDL_Event move{};
    move.type = SDL_EVENT_MOUSE_MOTION;
    move.motion.windowID = SDL_GetWindowID(window_);
    move.motion.x = x;
    move.motion.y = y;
    // The motion since the last pushed place, as a mouse's events carry it.
    if (pushed_x_ >= 0.0f) {
        move.motion.xrel = x - pushed_x_;
        move.motion.yrel = y - pushed_y_;
    }
    pushed_x_ = x;
    pushed_y_ = y;
    SDL_PushEvent(&move); // (the events alone: warping would move the desktop's one cursor away from its user)
}

void Window::push_mouse_button(float x, float y, int sdl_button, bool down) {
    push_mouse_motion(x, y);
    SDL_Event ev{};
    ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    ev.button.windowID = SDL_GetWindowID(window_);
    ev.button.button = static_cast<Uint8>(sdl_button);
    ev.button.down = down;
    ev.button.clicks = 1;
    ev.button.x = x;
    ev.button.y = y;
    SDL_PushEvent(&ev);
}

void Window::open_gamepad(std::uint32_t id) {
    SDL_Gamepad* pad = SDL_OpenGamepad(static_cast<SDL_JoystickID>(id));
    if (!pad) {
        log::warn("gamepad {}: {}", id, SDL_GetError());
        return;
    }
    if (pad_ && pad_id_ != id) close_gamepad();
    else if (pad_) SDL_CloseGamepad(pad_); // the same pad opened twice: keep one reference
    pad_ = pad;
    pad_id_ = id;
    const char* name = SDL_GetGamepadName(pad);
    input_.pad.connected = true;
    input_.pad.name = name ? name : "Gamepad";
    input_.pad.type = pad_type_of(SDL_GetGamepadType(pad));
    log::info("gamepad: {} ({})", input_.pad.name, pad_type_name(input_.pad.type));
}

void Window::close_gamepad() {
    if (!pad_) return;
    SDL_CloseGamepad(pad_);
    pad_ = nullptr;
    pad_id_ = 0;
    // What was held lets go.
    for (std::size_t i = 0; i < kPadButtons; ++i) {
        if (input_.pad.down[i]) input_.pad.released[i] = true;
        input_.pad.down[i] = false;
    }
    input_.pad.axes.fill(0.0f);
    input_.pad.connected = false;
    input_.pad.name.clear();
    input_.pad.type = PadType::Generic;
}

void Window::read_gamepad() {
    GamepadState& p = input_.pad;
    if (!pad_) return;
    float lx = axis_value(pad_, SDL_GAMEPAD_AXIS_LEFTX), ly = axis_value(pad_, SDL_GAMEPAD_AXIS_LEFTY);
    float rx = axis_value(pad_, SDL_GAMEPAD_AXIS_RIGHTX), ry = axis_value(pad_, SDL_GAMEPAD_AXIS_RIGHTY);
    radial_dead_zone(lx, ly, pad_dead_zone_);
    radial_dead_zone(rx, ry, pad_dead_zone_);
    p.axes = {lx, ly, rx, ry, std::max(axis_value(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0.0f),
              std::max(axis_value(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0.0f)};
    for (std::size_t i = 0; i < kPadButtons; ++i) {
        bool now = false;
        if (static_cast<PadButton>(i) == PadButton::LT) now = p.axis(PadAxis::LT) > pad_trigger_threshold_;
        else if (static_cast<PadButton>(i) == PadButton::RT) now = p.axis(PadAxis::RT) > pad_trigger_threshold_;
        else now = SDL_GetGamepadButton(pad_, static_cast<SDL_GamepadButton>(kPadSdlButtons[i]));
        if (now && !p.down[i]) p.pressed[i] = true;
        if (!now && p.down[i]) p.released[i] = true;
        p.down[i] = now;
    }
}

bool Window::rumble_gamepad(float low, float high, int ms) {
    if (!pad_) return false;
    // NaN passes std::clamp, and NaN cast to Uint16 is undefined: it is no rumble. Callers keep `ms` short
    // (app::Gamepad::kMaxRumbleMs); SDL's own limit is 0xFFFF ms.
    auto strength = [](float v) {
        return v == v ? static_cast<Uint16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f) : Uint16{0};
    };
    return SDL_RumbleGamepad(pad_, strength(low), strength(high), static_cast<Uint32>(std::clamp(ms, 0, 0xFFFF)));
}

void Window::set_pad_pointer(bool active) {
    if (active == pointer_.active) return;
    pointer_.mouse_used = false;
    if (active) {
        SDL_GetMouseState(&pointer_.x, &pointer_.y);
        // A mouse away from the window (or never moved over it): the pointer starts in the middle, where the windows
        // open, not at an edge.
        if (SDL_GetMouseFocus() != window_) {
            int w = 0, h = 0;
            SDL_GetWindowSize(window_, &w, &h);
            pointer_.x = static_cast<float>(w) / 2.0f;
            pointer_.y = static_cast<float>(h) / 2.0f;
        }
    } else {
        pointer_.left = pointer_.right = false;
        // The system's cursor comes back where the pad's pointer was (only while it is over this window: elsewhere
        // it belongs to the rest of the desktop).
        if (SDL_GetMouseFocus() == window_) SDL_WarpMouseInWindow(window_, pointer_.x, pointer_.y);
    }
    pointer_.active = active;
}

void Window::move_pad_pointer(float dx, float dy, float wheel) {
    if (!pointer_.active) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    pointer_.x = std::clamp(pointer_.x + dx, 0.0f, static_cast<float>(std::max(w - 1, 0)));
    pointer_.y = std::clamp(pointer_.y + dy, 0.0f, static_cast<float>(std::max(h - 1, 0)));
    pointer_.wheel += wheel;
    input_.wheel += wheel;
    if (!input_.left_clicked) {
        input_.mouse_x = pointer_.x;
        input_.mouse_y = pointer_.y;
    }
}

void Window::set_pad_pointer_buttons(bool left, bool right) {
    if (!pointer_.active) return;
    if (left && !pointer_.left) {
        input_.left_clicked = true;
        input_.mouse_x = pointer_.x;
        input_.mouse_y = pointer_.y;
    }
    if (right && !pointer_.right) input_.right_clicked = true;
    pointer_.left = left;
    pointer_.right = right;
    input_.left_button = input_.left_button || left;
    input_.right_button = input_.right_button || right;
}

void Window::set_pointer_speed(int speed, bool grab) {
    pointer_speed_ = speed;
    pointer_grab_ = grab;
    update_fast_pointer();
}

void Window::update_fast_pointer() {
    // The window's own cursor while the speed is above 1, the window has the focus and the system's cursor is over
    // it (not left past an edge), and the pad's pointer is not in use.
    bool over = !pointer_grab_ || (SDL_GetMouseFocus() == window_ && !fast_left_);
    bool want = pointer_speed_ > 1 && (focused() || !pointer_grab_) && over && !pointer_.active;
    if (want == fast_.active) return;
    if (!want) {
        release_fast_pointer(false);
        return;
    }
    SDL_GetMouseState(&fast_.x, &fast_.y);
    if (!pointer_grab_ && pushed_x_ >= 0.0f) {
        fast_.x = pushed_x_;
        fast_.y = pushed_y_;
    }
    fast_.active = true;
    if (pointer_grab_ && !capture_.captured()) {
        // KWin's lock hint where the cursor is (platform/mouse_capture.h): its lock's end goes there first.
        SDL_WarpMouseInWindow(window_, fast_.x, fast_.y);
        SDL_SetWindowRelativeMouseMode(window_, true);
    }
}

void Window::release_fast_pointer(bool warp) {
    if (!fast_.active) return;
    fast_.active = false;
    if (!pointer_grab_ || capture_.captured()) return;
    // The system's cursor comes back where the window's was (set while still held: on Wayland only a held pointer
    // can be placed).
    if (warp || SDL_GetMouseFocus() == window_) SDL_WarpMouseInWindow(window_, fast_.x, fast_.y);
    SDL_SetWindowRelativeMouseMode(window_, false);
}

namespace {
// MouseCapture's cursor: SDL's.
struct SdlCursor final : MouseCapture::Cursor {
    explicit SdlCursor(SDL_Window* w) : window(w) {}
    SDL_Window* window;
    void position(float& x, float& y) override { SDL_GetMouseState(&x, &y); }
    void set_relative(bool on) override { SDL_SetWindowRelativeMouseMode(window, on); }
    void warp(float x, float y) override { SDL_WarpMouseInWindow(window, x, y); }
    bool focused() override { return SDL_GetKeyboardFocus() == window; }
};
} // namespace

void Window::set_mouse_captured(bool captured) {
    if (captured == capture_.captured()) return;
    // The window's cursor holds the system's already; mouse look leaves it where it was.
    if (fast_.active && pointer_grab_) {
        capture_.set_held(captured, fast_.x, fast_.y);
        return;
    }
    // A test run on a shared desktop (set_pointer_speed's grab false): its looks come from pushed events, and the
    // system's cursor stays the user's.
    if (!pointer_grab_) {
        capture_.set_held(captured, pushed_x_, pushed_y_);
        return;
    }
    SdlCursor cursor(window_);
    capture_.set(captured, cursor);
}

void Window::center_mouse() {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    capture_.hold_at(static_cast<float>(w) / 2.0f, static_cast<float>(h) / 2.0f);
    if (fast_.active) {
        fast_.x = capture_.x();
        fast_.y = capture_.y();
        if (pointer_grab_) return;
    }
    SDL_WarpMouseInWindow(window_, capture_.x(), capture_.y());
}

void Window::framebuffer_size(std::uint32_t& width, std::uint32_t& height) const {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
}

bool Window::minimized() const { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0; }

float Window::content_scale() const {
    float s = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window_));
    return s > 0.0f ? s : 1.0f;
}

float Window::refresh_rate() const {
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    return m && m->refresh_rate > 0.0f ? m->refresh_rate : 0.0f;
}

float Window::pixel_density() const {
    float d = SDL_GetWindowPixelDensity(window_);
    return d > 0.0f ? d : 1.0f;
}

void Window::set_title(const std::string& title) { SDL_SetWindowTitle(window_, title.c_str()); }

bool Window::set_icon(int width, int height, std::span<const std::uint8_t> rgba) {
    const auto w = static_cast<std::size_t>(width), h = static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || rgba.size() != w * h * 4) return false;
#ifdef _WIN32
    // An .exe with its own icon (cmake/windows/hailstone.rc) keeps it: SDL has made it the window class's, and
    // Windows takes the size each place needs from it. One image set here would stand for every size instead.
    if (FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */))
        return true;
#endif
    // SDL_PIXELFORMAT_RGBA32 is R, G, B, A in memory whatever the byte order.
    SDL_Surface* s = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!s) return false;
    const std::size_t row = w * 4, pitch = static_cast<std::size_t>(s->pitch);
    for (std::size_t y = 0; y < h; ++y)
        std::memcpy(static_cast<std::uint8_t*>(s->pixels) + y * pitch, rgba.data() + y * row, row);
    const bool ok = SDL_SetWindowIcon(window_, s);
    if (!ok) log::warn("window icon not set: {}", SDL_GetError());
    SDL_DestroySurface(s);
    return ok;
}

void Window::set_text_input(bool on) {
    if (on) {
        // Each frame: ImGui's backend stops it when one of its fields lets go.
        if (!SDL_TextInputActive(window_)) SDL_StartTextInput(window_);
        text_input_ = true;
    } else if (text_input_) {
        text_input_ = false;
        // An ImGui field that takes the keys in this frame starts it again at the frame's end (its IME data).
        if (SDL_TextInputActive(window_)) SDL_StopTextInput(window_);
    }
}

bool Window::focused() const { return (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0; }

void Window::raise_focused() {
#ifdef _WIN32
    // Not SDL's forced raise (SDL_HINT_FORCE_RAISEWINDOW): it restores the window first (ShowWindow SW_RESTORE),
    // which took a maximized window back to its smaller normal size. The same way past Windows' rule against a
    // background program taking the foreground, without that: this thread's input attached to the foreground
    // window's for the call. Only a minimized window is restored (to how it was: maximized stays maximized).
    if (HWND hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))) {
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        HWND front = GetForegroundWindow();
        const DWORD mine = GetCurrentThreadId();
        const DWORD theirs = front ? GetWindowThreadProcessId(front, nullptr) : 0;
        const bool attached = theirs != 0 && theirs != mine && AttachThreadInput(theirs, mine, TRUE);
        BringWindowToTop(hwnd);
        if (!SetForegroundWindow(hwnd)) log::warn("window: Windows would not bring it to the front");
        SetFocus(hwnd);
        if (attached) AttachThreadInput(theirs, mine, FALSE);
        return;
    }
#endif
    if (!SDL_RaiseWindow(window_)) log::warn("window: could not raise it: {}", SDL_GetError());
}

WindowMode Window::mode() const {
    if (!(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN)) return WindowMode::Windowed;
    return SDL_GetWindowFullscreenMode(window_) ? WindowMode::Fullscreen : WindowMode::Borderless;
}

void Window::set_mode(WindowMode mode, DisplayMode size) {
    if (mode == WindowMode::Windowed) {
        SDL_SetWindowFullscreen(window_, false);
        if (size.width > 0 && size.height > 0) {
            SDL_RestoreWindow(window_);
            SDL_SetWindowSize(window_, size.width, size.height);
        }
        SDL_SyncWindow(window_);
        return;
    }
    if (mode == WindowMode::Borderless) {
        SDL_SetWindowFullscreenMode(window_, nullptr); // the desktop's mode
    } else {
        SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
        if (size.width <= 0 || size.height <= 0) {
            DisplayMode d = desktop_mode();
            size = d;
        }
        SDL_DisplayMode closest;
        if (SDL_GetClosestFullscreenDisplayMode(display, size.width, size.height, size.refresh, true, &closest))
            SDL_SetWindowFullscreenMode(window_, &closest);
        else
            SDL_SetWindowFullscreenMode(window_, nullptr);
    }
    SDL_SetWindowFullscreen(window_, true);
    SDL_SyncWindow(window_);
}

std::vector<DisplayMode> Window::display_modes() const {
    std::vector<DisplayMode> out;
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window_), &count);
    // SDL lists them largest first, then by refresh rate (highest first): keep each size's first.
    for (int i = 0; modes && i < count; ++i) {
        const SDL_DisplayMode& m = *modes[i];
        bool seen = false;
        for (const auto& o : out) seen = seen || (o.width == m.w && o.height == m.h);
        if (!seen) out.push_back({m.w, m.h, m.refresh_rate});
    }
    SDL_free(modes);
    if (out.empty()) out.push_back(desktop_mode());
    return out;
}

DisplayMode Window::desktop_mode() const {
    const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window_));
    if (!m) return {1920, 1080, 60.0f};
    return {m->w, m->h, m->refresh_rate};
}

std::vector<const char*> Window::required_instance_extensions() const {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError());
    return {names, names + count};
}

VkSurfaceKHR Window::create_surface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface))
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
    return surface;
}

PFN_vkGetInstanceProcAddr Window::bundled_vulkan_loader() const {
    if (!bundled_vulkan_) return nullptr;
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
}

} // namespace hailstone::platform
