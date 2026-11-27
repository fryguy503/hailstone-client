#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <volk.h>

#include "platform/gamepad.h"
#include "platform/mouse_capture.h"

struct SDL_Window;
struct SDL_Joystick;
struct SDL_Gamepad;
union SDL_Event;

namespace hailstone::platform {

struct WindowDesc {
    std::string title = "Hailstone";
    int width = 1280;
    int height = 720;
    bool resizable = true;
    // Where the window was last (Window::geometry()): its top left on the desktop (none: the system's), and
    // whether it was maximized.
    std::optional<std::pair<int, int>> position;
    bool maximized = false;
    // Pad input while another window has the focus (SDL drops it by default): for tests (--input-script), whose
    // windows often open in the background.
    bool pad_input_in_background = false;
    // Render probes can own a Vulkan surface without showing or activating a desktop window.
    bool hidden = false;
};

// How the window fills the screen: a normal window, a borderless window the size of its display, or exclusive
// fullscreen at a chosen display mode.
enum class WindowMode : std::uint8_t { Windowed, Borderless, Fullscreen };

// A display's resolution and refresh rate (Window::display_modes).
struct DisplayMode {
    int width = 0, height = 0;
    float refresh = 0.0f; // Hz, 0 unknown
};

// The window on the desktop: its normal (not maximized) place and size, and whether it is maximized.
struct WindowGeometry {
    int x = 0, y = 0, width = 0, height = 0;
    bool maximized = false;
};

enum class Key : std::uint8_t {
    W, A, S, D, Q, E, Space, LeftShift, LeftCtrl, Escape,
    C, Tab, F1, F8, Grave, Num1, RightCtrl, R,
    Count
};

// Every key by its DirectInput scan code (DIK_*, 0..255): the codes the
// original client's key bindings store (KeyCombo). Extended keys use the DIK values with bit 7
// set (0x9c keypad enter, 0xc8 up arrow, ...). 0 = no key.
inline constexpr std::size_t kKeyCodes = 256;
// The DirectInput scan code of an SDL scancode (0 when the key has none).
std::uint8_t dik_from_sdl_scancode(int sdl_scancode);
// The key code of a mouse button other than left and right (0 for those and for buttons past the eighth). The
// original binds DirectInput mouse buttons 2..7 (middle, X1, X2 and three more) as codes 0xf0..0xf5 (0x51a770
// @0x51a770, called at the end of __ProcessMouseEvents: a press goes through HandleKeyDownEvent @0x5fc680 with the
// held Shift / Ctrl / Alt like a key, a release through HandleKeyUpEvent @0x5f9ad0); KeyCombo::GetTextDescription
// names them "MOUS MID" and "MOUS AUX1".."MOUS AUX5". SDL numbers the same buttons 2, 4, 5, 6, 7, 8.
std::uint8_t dik_from_sdl_mouse_button(int sdl_button);
// A key event, in the order the OS delivered them this frame.
struct KeyEvent {
    std::uint8_t code = 0; // DirectInput scan code
    bool down = false;     // false: released
    bool repeat = false;   // auto-repeat of a held key
};

// Input gathered by Window::poll_events for one frame.
struct InputState {
    std::array<bool, static_cast<std::size_t>(Key::Count)> down{};
    std::array<bool, static_cast<std::size_t>(Key::Count)> pressed{}; // went down this frame
    float mouse_dx = 0.0f; // relative motion this frame, in pixels
    float mouse_dy = 0.0f;
    float wheel = 0.0f;
    bool right_button = false;
    bool left_button = false;               // held
    bool left_clicked = false;              // left button went down this frame
    bool right_clicked = false;             // right button went down this frame (even if it is up again)
    float mouse_x = 0.0f, mouse_y = 0.0f;   // cursor, window coordinates

    // All keys by scan code (DirectInput numbering, see kKeyCodes).
    std::array<bool, kKeyCodes> key_down{};
    std::array<bool, kKeyCodes> key_pressed{};  // went down this frame
    std::array<bool, kKeyCodes> key_released{}; // went up this frame
    std::vector<KeyEvent> key_events;           // this frame's key events in order

    GamepadState pad; // the most recently connected gamepad (platform/gamepad.h)
    // The user gave input this frame: a key, typing, the mouse moved, clicked or wheeled, a pad button or stick, a
    // touch; or a key, mouse button or pad input is held (the AFK keepalive, game/social.h).
    bool user_activity = false;

    bool is_down(Key k) const { return down[static_cast<std::size_t>(k)]; }
    bool was_pressed(Key k) const { return pressed[static_cast<std::size_t>(k)]; }
};

// SDL3 window with a Vulkan surface. One per process.
class Window {
public:
    explicit Window(const WindowDesc& desc);
    WindowGeometry geometry() const;
    // Puts a windowed window at `g` (a place that is on no display is left as it is; a size of 0 too), maximized
    // when it says so: what WindowDesc gives at start, for a window that is already open.
    void set_geometry(const WindowGeometry& g);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Pumps OS events and refreshes input(). Returns false once the user asked to quit.
    bool poll_events();
    // The window's close button (Alt+F4, the title bar's X: WM_CLOSE in the original, which HandleWindowCloseRequest
    // @0x5f9660 answers). Held, a close no longer ends poll_events: take_close_request() reports it once, and the
    // owner decides (the client camps to the desktop in game). SDL_EVENT_QUIT (a signal, the OS logging out) still
    // quits at once.
    void set_hold_close_requests(bool hold);
    bool take_close_request() { return std::exchange(close_requested_, false); }
    // Testing (app/input_script.h): a close request as the window manager sends it.
    void push_close_request();
    const InputState& input() const { return input_; }

    // While captured the cursor is hidden and mouse motion is relative; let go, it is back where it was taken (the
    // original's mouse look puts it back where it was; platform/mouse_capture.h), or in the middle after
    // center_mouse.
    void set_mouse_captured(bool captured);
    // Moves the cursor to the middle of the window (the Alt+Enter key, slot 489; CenterMouseCursor).
    void center_mouse();

    // Size of the drawable area in pixels (differs from window size on HiDPI).
    void framebuffer_size(std::uint32_t& width, std::uint32_t& height) const;
    bool minimized() const;
    void set_title(const std::string& title);
    // The icon the title bar and the taskbar show: RGBA8, top row first, width * height * 4 bytes. False when the
    // pixels are the wrong size or the system refused them (the window keeps the system's icon). On Windows a
    // program with an icon of its own keeps that one, and this does nothing.
    bool set_icon(int width, int height, std::span<const std::uint8_t> rgba);
    // Text input (SDL's text events) on for something that is not an ImGui text field: the in-game browser's page.
    // Off again only if this turned it on (ImGui's own fields switch it themselves).
    void set_text_input(bool on);
    // Whether the window has the keyboard focus.
    bool focused() const;
    // Brings the window to the front with the focus, past Windows' rule against a background program doing so (it
    // otherwise only flashes the taskbar button), and keeps it as it was: maximized, windowed or full screen.
    void raise_focused();

    // Fullscreen: the window's mode, and switching it (Fullscreen uses the display mode closest to `size`, or the
    // desktop's when it is 0 x 0). In Windowed mode a non-zero size resizes the window (and un-maximizes it).
    WindowMode mode() const;
    void set_mode(WindowMode mode, DisplayMode size = {});
    // The fullscreen resolutions of the window's display, largest first, each once (at its highest refresh rate).
    std::vector<DisplayMode> display_modes() const;
    // The window's display's desktop resolution.
    DisplayMode desktop_mode() const;

    // Called with every SDL event before the window handles it (e.g. by the UI layer).
    using EventListener = std::function<void(const SDL_Event&)>;
    void set_event_listener(EventListener listener) { listener_ = std::move(listener); }
    // ProcessMouseInput @0x5f9e30 without MouseRightHanded (0xddf744): the left and right buttons trade places, for
    // the windows (the listener's events) and the game alike.
    void set_swap_mouse_buttons(bool swap) { swap_buttons_ = swap; }

    // The gamepad (a client extension, docs/gamepad.md): the sticks' radial dead zone and the point past which a
    // trigger counts as held (0..1), as InputState::pad reports them from the next frame on.
    void set_gamepad_tuning(float dead_zone, float trigger_threshold) {
        pad_dead_zone_ = dead_zone;
        pad_trigger_threshold_ = trigger_threshold;
    }
    // Rumbles the pad (strengths 0..1) for `ms`; false without a pad or rumble.
    bool rumble_gamepad(float low, float high, int ms);
    // The open pad (null: none), for ImGui's SDL3 backend.
    SDL_Gamepad* sdl_gamepad() const { return pad_; }

    // The pad's pointer (docs/gamepad.md section 2, cursor mode): a mouse pointer the pad moves and clicks. While it
    // is active, input() reports its place and buttons (with the mouse's, which keeps working and moves it too), and
    // ImGuiLayer gives them to the windows. Call the setters each frame after poll_events.
    struct PadPointer {
        bool active = false;
        float x = 0.0f, y = 0.0f;    // window coordinates
        bool left = false, right = false;
        float wheel = 0.0f;          // this frame's scrolling
        bool mouse_used = false;     // the real mouse moved or clicked while it was active (the mouse takes over)
    };
    void set_pad_pointer(bool active);
    void move_pad_pointer(float dx, float dy, float wheel);
    void set_pad_pointer_buttons(bool left, bool right);
    const PadPointer& pad_pointer() const { return pointer_; }

    // MousePointerSpeedMod (/mousespeed; game::pointer_step): above 1 the cursor is the window's own, moved by the
    // mouse's motion times `speed` and drawn by the UI (ImGuiLayer), as the original's own cursor (ProcessMouseInput)
    // is. While it is in the window the system's cursor is held there (relative mode), and let go where the window's
    // cursor is pushed past an edge; it is taken again when it comes back over the window. Every event the listener
    // and the game get carries the window's cursor. `grab` false (test runs on a shared desktop): the system's cursor
    // is never taken, nor by mouse look (set_mouse_captured); the events' motion moves the window's cursor.
    struct FastPointer {
        bool active = false;
        float x = 0.0f, y = 0.0f; // window coordinates
    };
    void set_pointer_speed(int speed, bool grab = true);
    int pointer_speed() const { return pointer_speed_; }
    const FastPointer& fast_pointer() const { return fast_; }

    // Testing (app/input_script.h): input made up as the OS and a pad would send it, read by the next poll_events.
    // A virtual Xbox pad (SDL_AttachVirtualJoystick) goes through the same hot-plug and reading as a real one; the
    // events go through the listener (ImGui) and the game alike.
    bool attach_virtual_gamepad();
    void set_virtual_pad_button(int sdl_button, bool down); // SDL_GamepadButton
    void set_virtual_pad_axis(int sdl_axis, float value);   // SDL_GamepadAxis, -1..1 (triggers 0..1)
    void push_key(int sdl_scancode, bool down);
    void push_text(const std::string& text);
    void push_mouse_button(float x, float y, int sdl_button, bool down); // window coordinates
    void push_mouse_motion(float x, float y);                            // window coordinates

    // Content scale of the window's display (1.0 at 96 dpi, 2.0 for 200% scaling).
    float content_scale() const;
    // The refresh rate the window's display runs at now (its current mode: the fullscreen mode's, or the desktop's),
    // in Hz; 0 when the system does not say. Follows the window to another display.
    float refresh_rate() const;
    // Framebuffer pixels per window coordinate (2.0 on Retina-style displays).
    float pixel_density() const;
    SDL_Window* sdl_window() const { return window_; }

    std::vector<const char*> required_instance_extensions() const;
    VkSurfaceKHR create_surface(VkInstance instance) const;
    // vkGetInstanceProcAddr of the Vulkan library the window loaded when it is the one shipped beside the binary
    // (platform/paths.h bundled_vulkan_library: MoltenVK on macOS), for volkInitializeCustom; volk's own search does
    // not look beside the binary. Null otherwise: volk finds the system's Vulkan itself.
    PFN_vkGetInstanceProcAddr bundled_vulkan_loader() const;

private:
    static bool on_a_display(int x, int y);
    void open_gamepad(std::uint32_t id);
    void close_gamepad();
    void read_gamepad();
    SDL_Window* window_ = nullptr;
    bool bundled_vulkan_ = false; // SDL was pointed at bundled_vulkan_library()
    bool text_input_ = false; // set_text_input turned it on
    mutable WindowGeometry normal_; // the last normal place and size (kept while it is maximized)
    InputState input_;
    EventListener listener_;
    MouseCapture capture_; // mouse look's hold on the system's cursor
    bool swap_buttons_ = false;
    bool hold_close_ = false, close_requested_ = false;
    SDL_Gamepad* pad_ = nullptr;
    SDL_Joystick* virtual_pad_ = nullptr;       // attach_virtual_gamepad's
    std::deque<std::string> pushed_text_;         // push_text's strings, alive until poll() has read their events
    unsigned pushed_mods_ = 0; // the modifiers of the pushed keys held now (SDL_Keymod bits)
    std::uint32_t pad_id_ = 0; // its SDL_JoystickID
    float pad_dead_zone_ = 0.2f, pad_trigger_threshold_ = 0.5f;
    PadPointer pointer_;
    void update_fast_pointer(); // takes or lets go of the system's cursor as set_pointer_speed's rules say
    void release_fast_pointer(bool warp);
    int pointer_speed_ = 0;
    bool pointer_grab_ = true;
    FastPointer fast_;
    bool fast_left_ = false;  // the window's cursor was pushed out: not taken again until the mouse comes back
    float pushed_x_ = -1.0f, pushed_y_ = -1.0f; // push_mouse_motion's last place (its events' relative motion)
};

} // namespace hailstone::platform
