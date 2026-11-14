#pragma once

// Dear ImGui context and its SDL3 + Vulkan backends: the temporary developer
// UI used until the SIDL UI engine (milestone M5) exists. See docs/dev-ui.md.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <volk.h>

struct ImFont;

namespace hailstone::platform { class Window; }
namespace hailstone::render { class VkContext; struct FrameInfo; }

namespace hailstone::ui {

struct ImGuiLayerDesc {
    float font_size = 15.0f;          // in points at 100% display scale
    std::filesystem::path font_path;  // TTF to use; empty tries common system fonts, then ImGui's built-in font
    float scale_override = 0.0f;      // > 0 replaces the display content scale
    // Where ImGui keeps its windows' places and sizes (the client's own windows; the Lua windows keep theirs in
    // the character's UI ini); empty keeps none.
    std::filesystem::path ini_path;
};

// One per process. Construct after the VkContext; destroy before it.
class ImGuiLayer {
public:
    ImGuiLayer(platform::Window& window, render::VkContext& vk, const ImGuiLayerDesc& desc = {});
    ~ImGuiLayer();
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Starts a UI frame. Call once per loop iteration after Window::poll_events,
    // then issue ImGui calls, then render() inside the frame's rendering.
    void new_frame();
    // Ends the frame's scene (VkContext::begin_overlay) and records the UI over it.
    // Call before VkContext::end_frame. If the frame was skipped, call
    // end_frame_without_render() instead so ImGui's frame is closed.
    void render(const render::FrameInfo& frame);
    void end_frame_without_render();

    // True while the UI uses the mouse or keyboard; the game should ignore them then. The keyboard: see
    // keyboard_for_ui.
    bool wants_mouse() const;
    bool wants_keyboard() const;

    float scale() const { return scale_; }
    // The UI font's face (/fontface, ui.font_face; ui/font_faces.h): its regular and bold files (empty: the
    // Arial-class font found), for every font from the next frame.
    void set_font_face(const std::filesystem::path& regular, const std::filesystem::path& bold);
    // The name plates' font (game/nameplate_style.h): its face's regular and bold files (empty: the UI font's), its
    // size in points at 100 % scale and whether it is bold, from the next frame (plate_font()).
    void set_plate_font(const std::filesystem::path& regular, const std::filesystem::path& bold, float size, bool bold_face);

    // The gamepad (a client extension, docs/gamepad.md): on, ImGui's gamepad navigation runs on the window's pad (menu
    // mode: the SDL3 backend drives it); off, ImGui gets nothing from the pad. Applied at the next new_frame. The pad's
    // pointer (platform::Window::pad_pointer, cursor mode) reaches ImGui as the mouse's place and buttons.
    void set_pad_navigation(bool on) { pad_nav_ = on; }
    // The interface locked (/safelock, game/option_commands.h): __ProcessMouseEvents gives the windows no mouse, so
    // from the next new_frame no button press or wheel (the mouse's or the pad pointer's) reaches ImGui; releases
    // still do, so nothing stays held.
    void set_mouse_locked(bool on) { mouse_locked_ = on; }
    // Gives ImGui's focus (and the navigation cursor) to the window of that name once it exists (a gamepad window
    // asking for the pad); a request no window answers within two seconds of frames is dropped.
    void request_window_focus(std::string name);
    bool window_focus_pending() const { return !focus_request_.empty(); }
    // The windows that asked for the pad (request_window_focus), newest last, while they are still drawn: whether one
    // is open, whether it has the focus, and giving it the focus back (after a mouse click took it elsewhere).
    bool pad_window_open();
    bool pad_window_focused() const;
    // The window the pad's focus is on or going to (a pending request_window_focus first), "" for none.
    std::string pad_focus_window() const;
    void refocus_pad_window();
    // Testing (app/input_script.h): the focused window, the active and nav items and what ImGui captures, in a line.
    std::string debug_state() const;

private:
    void apply_pad_navigation();
    void feed_pad_pointer();
    platform::Window& window_;
    render::VkContext& vk_;
    std::string ini_path_; // io.IniFilename points into it
    float last_w_ = 0.0f, last_h_ = 0.0f; // the display size of the last frame not minimized
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
    std::uint32_t min_image_count_ = 0;
    float scale_ = 1.0f;
    void build_fonts();
    float font_size_ = 15.0f;
    std::filesystem::path font_path_, face_, face_bold_;
    std::filesystem::path plate_face_, plate_face_bold_; // set_plate_font's
    float plate_size_ = 15.0f;
    bool plate_bold_ = false;
    bool fonts_dirty_ = false;
    bool frame_open_ = false;
    int keyboard_request_ = -1; // the last frame's SetNextFrameWantCaptureKeyboard (-1: none), read before NewFrame
    bool pad_nav_ = false;
    bool mouse_locked_ = false;
    std::vector<std::string> pad_windows_; // request_window_focus's windows, newest last
    bool pad_nav_applied_ = false;
    void* nav_pad_ = nullptr;                  // the SDL_Gamepad ImGui's backend was given
    bool pointer_left_ = false, pointer_right_ = false; // the pad pointer's buttons as ImGui has them
    std::string focus_request_;
    int focus_frames_ = 0;
};

// Whether the keys are the UI's in the current ImGui frame: ImGuiIO::WantCaptureKeyboard, less an item the mouse
// holds. ImGui gives the keyboard to any active item, so a press on a button, a slider, a title bar or a window's
// background took it, and the game let go of the movement keys still held. As in the original, a click on a window
// leaves the keys the game's. An edit box being typed in, a modal dialog, an item made active from the keyboard or
// the pad, and a frame that asked for the keys (`requested` == 1: the last frame's SetNextFrameWantCaptureKeyboard,
// ui.CaptureKeyboard) keep them.
bool keyboard_for_ui(int requested);

// Called before ImGui::NewFrame: a Tab pressed while no item is active does not move ImGui's focus into a window's
// first field (the chat input), so it stays the game's. Tab still moves between fields while one is typed in.
void keep_tab_from_windows();

// The name plates' font as ImGuiLayer::set_plate_font set it (nullptr when the built-in font stands in: draw with
// ImGui's font at plate_font_size()), and its size in ImGui's pixels. plate_font_faux_bold: bold was asked for and
// the face has no bold file, so the plates strike each letter twice.
ImFont* plate_font();
float plate_font_size();
bool plate_font_faux_bold();

// The current ImGui context's keys as on Windows on every system: Ctrl is Ctrl in the windows, as in the game's own
// keys, the original client and the Ctrl-clicks the windows use (on items, loot, the spell gems). ImGui's macOS
// default would make Cmd the windows' Ctrl, with Mac-style text editing; a deliberate difference from a usual Mac
// program, so a Ctrl-click means the same in a window as in the game.
void configure_keys();

} // namespace hailstone::ui
