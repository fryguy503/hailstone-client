#include "ui/imgui_layer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName and the focused window, for the gamepad's focus requests
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include "core/log.h"
#include "platform/window.h"
#include "render/vk_context.h"
#include "ui/imgui_vk_textures.h"
#include "ui/lua/lua_host.h"

namespace hailstone::ui {

namespace {

void check_vk(VkResult r) {
    if (r != VK_SUCCESS) log::error("imgui vulkan backend: VkResult {}", static_cast<int>(r));
}

// EQ UI fonts 1..6 (the <Font> numbers in the stock XML): Arial at these pixel sizes,
// here the Arial-class font found below.
constexpr std::array<float, 7> kEqFontSizes = {0, 10, 12, 13, 14, 16, 20};
std::array<ImFont*, 7> g_eq_fonts{};
// The font list a chat window's font indexes (/chatfontsize 0..10, CChatWindow::SetChatFont: CXWndManager +0x118):
// EQGraphicsDX9's font table @0x100b2100, Arial at these cell heights, fonts 5, 6 and 10 bold (weight 700; the
// others 100 or 0, drawn regular).
constexpr std::array<float, kChatFonts> kChatFontSizes = {10, 12, 14, 15, 16, 20, 24, 20, 24, 14, 40};
constexpr std::array<bool, kChatFonts> kChatFontBold = {false, false, false, false, false, true, true, false, false, false, true};
std::array<ImFont*, kChatFonts> g_chat_fonts{};
float g_ui_scale = 1.0f; // the display scale the fonts were built for
// The name plates' (ImGuiLayer::set_plate_font).
ImFont* g_plate_font = nullptr;
float g_plate_font_size = 15.0f;
bool g_plate_faux_bold = false;

// Metric-compatible Arial-class fonts first (plan section 2), then common sans fonts.
// These are plain file paths; a missing file is simply skipped.
constexpr std::array kFontCandidates = {
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/Library/Fonts/Arial.ttf",
};

// Their bold faces, in the same order.
constexpr std::array kBoldFontCandidates = {
    "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
    "C:/Windows/Fonts/segoeuib.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/Library/Fonts/Arial Bold.ttf",
};

// A path as UTF-8, which ImGui's file functions take (they widen it on Windows). path::string() would be in the
// ANSI code page there, and throws for a name that page cannot show.
std::string utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::filesystem::path find_font(const std::filesystem::path& wanted) {
    std::error_code ec;
    if (!wanted.empty()) {
        if (std::filesystem::is_regular_file(wanted, ec)) return wanted;
        log::warn("UI font {} not found", utf8(wanted));
    }
    for (const char* p : kFontCandidates)
        if (std::filesystem::is_regular_file(p, ec)) return p;
    return {};
}

// The bold face going with `regular` (one of kFontCandidates), if there is one.
std::filesystem::path find_bold_font(const std::filesystem::path& regular) {
    std::error_code ec;
    for (std::size_t i = 0; i < kFontCandidates.size(); ++i)
        if (regular == kFontCandidates[i] && std::filesystem::is_regular_file(kBoldFontCandidates[i], ec))
            return kBoldFontCandidates[i];
    return {};
}

} // namespace

ImGuiLayer::ImGuiLayer(platform::Window& window, render::VkContext& vk, const ImGuiLayerDesc& desc)
    : window_(window), vk_(vk) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // The client's own windows keep their places (ImGui saves them every few seconds and at the end).
    ini_path_ = utf8(desc.ini_path);
    io.IniFilename = ini_path_.empty() ? nullptr : ini_path_.c_str();
    // No keyboard navigation: as in the original, the keyboard stays the game's after a click on a window (with it
    // on, a focused window took every key, and Space / Enter pressed its last clicked button). Only an edit box
    // being typed in or a modal dialog keeps the keys, not a widget the mouse holds (keyboard_for_ui); Tab still
    // moves between fields.

    scale_ = desc.scale_override > 0.0f ? desc.scale_override : window_.content_scale();
    g_ui_scale = scale_;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.ScaleAllSizes(scale_);

    font_size_ = desc.font_size;
    font_path_ = desc.font_path;
    build_fonts();
    // Script windows (ui/lua) rely on ImGui's error recovery to close what a failed script left open.
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = true;
    configure_keys();

    if (!ImGui_ImplSDL3_InitForVulkan(window_.sdl_window())) throw std::runtime_error("ImGui_ImplSDL3_InitForVulkan failed");
    window_.set_event_listener([](const SDL_Event& ev) { ImGui_ImplSDL3_ProcessEvent(&ev); });
    // The gamepad is the window's (platform::Window opens it): the backend gets it only in menu mode
    // (apply_pad_navigation), never opening pads itself.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);

    // The backend's own textures (the font atlas, made again as it grows). The client's (item icon sheets, spell
    // icons, the stock UI art the Lua windows draw, browser tabs) come from pools of their own that grow as they fill
    // (ui/imgui_vk_textures.h).
    constexpr std::uint32_t kBackendSets = 32;
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kBackendSets};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = kBackendSets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(vk_.device(), &pi, nullptr, &pool_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDescriptorPool for ImGui failed");

    // The UI draws in the frame's overlay pass (render::VkContext::begin_overlay):
    // the swapchain image, one sample, no depth.
    color_format_ = vk_.color_format(); // the backend keeps a pointer to it
    min_image_count_ = vk_.min_image_count();
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = vk_.instance();
    info.PhysicalDevice = vk_.physical_device();
    info.Device = vk_.device();
    info.QueueFamily = vk_.queue_family();
    info.Queue = vk_.queue();
    info.DescriptorPool = pool_;
    info.PipelineCache = vk_.pipeline_cache();
    info.MinImageCount = min_image_count_;
    // Vertex buffers rotate per RenderDrawData call; at least one per frame in flight.
    info.ImageCount = std::max({vk_.image_count(), render::VkContext::kFramesInFlight, min_image_count_});
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format_;
    info.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("ImGui_ImplVulkan_Init failed");
    init_imgui_textures(vk_.device());
}

ImGuiLayer::~ImGuiLayer() {
    vk_.wait_idle();
    shutdown_imgui_textures();
    window_.set_event_listener({});
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (pool_) vkDestroyDescriptorPool(vk_.device(), pool_, nullptr);
}

void ImGuiLayer::build_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_eq_fonts.fill(nullptr);
    g_chat_fonts.fill(nullptr);
    g_plate_font = nullptr;
    g_plate_font_size = std::round(plate_size_ * scale_);
    g_plate_faux_bold = false;
    // Rasterise at the framebuffer density so text stays sharp on HiDPI screens.
    ImFontConfig cfg;
    cfg.RasterizerDensity = window_.pixel_density();
    float size = std::round(font_size_ * scale_);
    // The face set (set_font_face) when its file is there, else the Arial-class font found.
    std::filesystem::path font, bold;
    try {
        std::error_code ec;
        font = !face_.empty() && std::filesystem::is_regular_file(face_, ec) ? face_ : find_font(font_path_);
        bold = font == face_ ? face_bold_ : find_bold_font(font);
        if (bold.empty() || !std::filesystem::is_regular_file(bold, ec)) bold = font;
    } catch (const std::exception& e) {
        // A saved face that cannot be used must not stop every start: the built-in font stands in.
        log::warn("UI font: {}; using the built-in font", e.what());
        font.clear();
        bold.clear();
    }
    auto use_default = [&] {
        io.Fonts->Clear();
        g_eq_fonts.fill(nullptr);
        g_chat_fonts.fill(nullptr);
        g_plate_font = nullptr;
        g_plate_faux_bold = plate_bold_;
        ImFontConfig def = cfg;
        def.SizePixels = std::round(13.0f * scale_);
        io.Fonts->AddFontDefault(&def);
        log::debug("UI font: built-in, scale {:.2f}", scale_);
    };
    ImFont* loaded = nullptr;
    if (!font.empty()) loaded = io.Fonts->AddFontFromFileTTF(utf8(font).c_str(), size, &cfg);
    if (!loaded) {
        use_default();
        return;
    }
    log::debug("UI font: {} at {:.0f} px, scale {:.2f}", utf8(font), size, scale_);

    {
        for (std::size_t i = 1; i < kEqFontSizes.size(); ++i) {
            ImFontConfig eq_cfg;
            eq_cfg.RasterizerDensity = window_.pixel_density();
            g_eq_fonts[i] = io.Fonts->AddFontFromFileTTF(utf8(font).c_str(), std::round(kEqFontSizes[i] * scale_),
                                                         &eq_cfg);
        }
        for (std::size_t i = 0; i < kChatFonts; ++i) {
            ImFontConfig chat_cfg;
            chat_cfg.RasterizerDensity = window_.pixel_density();
            const std::filesystem::path& file = kChatFontBold[i] ? bold : font;
            g_chat_fonts[i] = io.Fonts->AddFontFromFileTTF(utf8(file).c_str(),
                                                           std::round(kChatFontSizes[i] * scale_), &chat_cfg);
        }
        // The name plates': their own face when its file is there, else the UI's; its bold file when bold is asked
        // for, else the regular one struck twice (plate_font_faux_bold).
        std::error_code ec;
        const bool own = !plate_face_.empty() && std::filesystem::is_regular_file(plate_face_, ec);
        const std::filesystem::path& regular = own ? plate_face_ : font;
        std::filesystem::path plate_bold = own ? plate_face_bold_ : (bold == font ? std::filesystem::path() : bold);
        if (!plate_bold.empty() && !std::filesystem::is_regular_file(plate_bold, ec)) plate_bold.clear();
        const std::filesystem::path& file = plate_bold_ && !plate_bold.empty() ? plate_bold : regular;
        g_plate_faux_bold = plate_bold_ && plate_bold.empty();
        ImFontConfig plate_cfg;
        plate_cfg.RasterizerDensity = window_.pixel_density();
        g_plate_font = io.Fonts->AddFontFromFileTTF(utf8(file).c_str(), g_plate_font_size, &plate_cfg);
    }
    // A file that reads but is not a font the rasteriser can parse fails here, not when the texture is made.
    if (!io.Fonts->Build()) {
        log::warn("UI font {} could not be built; using the built-in font", utf8(font));
        use_default();
    }
}

void ImGuiLayer::set_font_face(const std::filesystem::path& regular, const std::filesystem::path& bold) {
    if (regular == face_ && bold == face_bold_) return;
    face_ = regular;
    face_bold_ = bold;
    fonts_dirty_ = true;
}

void ImGuiLayer::set_plate_font(const std::filesystem::path& regular, const std::filesystem::path& bold, float size,
                                bool bold_face) {
    if (regular == plate_face_ && bold == plate_face_bold_ && size == plate_size_ && bold_face == plate_bold_) return;
    plate_face_ = regular;
    plate_face_bold_ = bold;
    plate_size_ = size;
    plate_bold_ = bold_face;
    fonts_dirty_ = true;
}

void ImGuiLayer::new_frame() {
    if (frame_open_) end_frame_without_render();
    if (fonts_dirty_) {
        // The UI font changed (/fontface): the atlas built again, its texture made again by the backend's NewFrame.
        fonts_dirty_ = false;
        vk_.wait_idle();
        ImGui_ImplVulkan_DestroyFontsTexture();
        build_fonts();
    }
    std::uint32_t min_images = vk_.min_image_count();
    if (min_images != min_image_count_) {
        min_image_count_ = min_images;
        ImGui_ImplVulkan_SetMinImageCount(min_images);
    }
    ImGui_ImplVulkan_NewFrame();
    apply_pad_navigation();
    ImGui_ImplSDL3_NewFrame();
    feed_pad_pointer();
    if (mouse_locked_) {
        // set_mouse_locked: this frame's presses and wheel turns are dropped before ImGui sees them.
        ImVector<ImGuiInputEvent>& q = GImGui->InputEventsQueue;
        for (int i = 0; i < q.Size;) {
            const ImGuiInputEvent& e = q[i];
            if ((e.Type == ImGuiInputEventType_MouseButton && e.MouseButton.Down) || e.Type == ImGuiInputEventType_MouseWheel)
                q.erase(q.Data + i);
            else
                ++i;
        }
    }
    // Minimized (or not sized yet) the display is 0 x 0: ImGui would pull every window onto that empty screen, and
    // the UI ini would keep those places. The last real size stands in until the window is back.
    ImGuiIO& io = ImGui::GetIO();
    if (window_.minimized() || io.DisplaySize.x < 64.0f || io.DisplaySize.y < 64.0f) {
        if (last_w_ > 0.0f) io.DisplaySize = ImVec2(last_w_, last_h_);
    } else {
        last_w_ = io.DisplaySize.x;
        last_h_ = io.DisplaySize.y;
    }
    keyboard_request_ = GImGui->WantCaptureKeyboardNextFrame; // (NewFrame applies it to io, then clears it)
    keep_tab_from_windows();
    ImGui::NewFrame();
    frame_open_ = true;
    // The pad's X (ImGui's "menu" key) is the windows' own shortcut (use, take, none, ...; the Lua windows read it from
    // eq.gamepad). Held, ImGui would start its window switcher (a list of every window, the focus moved on release)
    // and, tapped, toggle the menu layer. Locked to an owner (from the frame it goes down until it is let go), ImGui's
    // navigation does not see it; the on-screen keyboard asks for it with that owner.
    ImGui::SetKeyOwner(ImGuiKey_GamepadFaceLeft, ImHashStr("##PadX"), ImGuiInputFlags_LockUntilRelease);
    if (!focus_request_.empty()) {
        if (ImGui::FindWindowByName(focus_request_.c_str())) {
            ImGui::SetWindowFocus(focus_request_.c_str());
            ImGui::SetNavCursorVisible(true);
            focus_request_.clear();
        } else if (++focus_frames_ > 120) {
            log::debug("gamepad: no window '{}' took the focus", focus_request_);
            focus_request_.clear();
        }
    }
}

void ImGuiLayer::apply_pad_navigation() {
    ImGuiIO& io = ImGui::GetIO();
    // Called before the backend's frame: a pad the window closed (unplugged) is taken back from it first.
    SDL_Gamepad* pad = pad_nav_ ? window_.sdl_gamepad() : nullptr;
    if (pad != nav_pad_) {
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, pad ? &pad : nullptr, pad ? 1 : 0);
        nav_pad_ = pad;
    }
    if (pad_nav_ == pad_nav_applied_) return;
    pad_nav_applied_ = pad_nav_;
    if (pad_nav_) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        return;
    }
    // Off: the backend stops updating the pad's keys, so whatever was held is let go here.
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; ++k)
        io.AddKeyAnalogEvent(static_cast<ImGuiKey>(k), false, 0.0f);
}

void ImGuiLayer::feed_pad_pointer() {
    ImGuiIO& io = ImGui::GetIO();
    const platform::Window::PadPointer& p = window_.pad_pointer();
    const platform::Window::FastPointer& fast = window_.fast_pointer();
    // ImGui draws the pointer itself (the backend hides the system's), so it shows where the pad put it, or the
    // window's own cursor (Window::set_pointer_speed) is.
    io.MouseDrawCursor = p.active || fast.active;
    if (fast.active && !p.active) {
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(fast.x, fast.y);
    }
    if (p.active) {
        // After the backend's frame: its own place for the mouse (the system's) comes first and this one wins.
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(p.x, p.y);
        if (p.wheel != 0.0f) io.AddMouseWheelEvent(0.0f, p.wheel);
    }
    const bool left = p.active && p.left, right = p.active && p.right;
    if (left != pointer_left_) io.AddMouseButtonEvent(ImGuiMouseButton_Left, left);
    if (right != pointer_right_) io.AddMouseButtonEvent(ImGuiMouseButton_Right, right);
    pointer_left_ = left;
    pointer_right_ = right;
}

void ImGuiLayer::request_window_focus(std::string name) {
    std::erase(pad_windows_, name);
    pad_windows_.push_back(name);
    focus_request_ = std::move(name);
    focus_frames_ = 0;
}

bool ImGuiLayer::pad_window_open() {
    // Between frames: a window drawn in the last frame is Active (its Begin ran) until the next NewFrame.
    const std::string top = pad_windows_.empty() ? std::string() : pad_windows_.back();
    std::erase_if(pad_windows_, [](const std::string& name) {
        const ImGuiWindow* w = ImGui::FindWindowByName(name.c_str());
        return !w || !(w->Active || w->WasActive);
    });
    // The window on top closed (a Tip over the merchant): the pad window under it takes the focus at once, so the
    // pad's next button goes to it rather than only giving it the focus back.
    if (!pad_windows_.empty() && pad_windows_.back() != top && focus_request_.empty())
        request_window_focus(pad_windows_.back());
    return !pad_windows_.empty();
}

bool ImGuiLayer::pad_window_focused() const {
    const ImGuiContext* g = ImGui::GetCurrentContext();
    return g && g->NavWindow && !pad_windows_.empty() && pad_windows_.back() == g->NavWindow->RootWindow->Name;
}

std::string ImGuiLayer::pad_focus_window() const {
    if (!focus_request_.empty()) return focus_request_;
    const ImGuiContext* g = ImGui::GetCurrentContext();
    return g && g->NavWindow ? std::string(g->NavWindow->RootWindow->Name) : std::string();
}

void ImGuiLayer::refocus_pad_window() {
    if (!pad_windows_.empty() && focus_request_.empty()) request_window_focus(pad_windows_.back());
}

std::string ImGuiLayer::debug_state() const {
    const ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return "no context";
    const ImGuiIO& io = ImGui::GetIO();
    char buf[768];
    std::snprintf(buf, sizeof buf,
                  "nav window '%s', nav id %08x, active id %08x, nav cursor %d, nav active %d, nav visible %d, "
                  "pad nav %d, has gamepad %d, wants mouse %d, wants keyboard %d, text input %d, dpad down %d (owner "
                  "%08x), nav move dir %d, nav init %d, nav layer %d, items in nav window %d",
                  g->NavWindow ? g->NavWindow->Name : "", g->NavId, g->ActiveId, g->NavCursorVisible ? 1 : 0,
                  io.NavActive ? 1 : 0, io.NavVisible ? 1 : 0,
                  (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) ? 1 : 0,
                  (io.BackendFlags & ImGuiBackendFlags_HasGamepad) ? 1 : 0, io.WantCaptureMouse ? 1 : 0,
                  io.WantCaptureKeyboard ? 1 : 0, io.WantTextInput ? 1 : 0, ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown) ? 1 : 0,
                  ImGui::GetKeyOwner(ImGuiKey_GamepadDpadDown), static_cast<int>(g->NavMoveDir),
                  g->NavInitRequest ? 1 : 0, static_cast<int>(g->NavLayer),
                  g->NavWindow ? g->NavWindow->DC.NavLayersActiveMask : -1);
    std::string out = buf;
    out += "; pad windows";
    for (const auto& w : pad_windows_) out += " '" + w + "'";
    return out;
}

void ImGuiLayer::render(const render::FrameInfo& frame) {
    vk_.begin_overlay(frame);
    if (!frame_open_) return;
    ImGui::Render();
    frame_open_ = false;
    ImDrawData* data = ImGui::GetDrawData();
    if (data->DisplaySize.x <= 0.0f || data->DisplaySize.y <= 0.0f) return;
    // The backend sets its own viewport and scissor, replacing the frame's Y-flipped viewport.
    ImGui_ImplVulkan_RenderDrawData(data, frame.cmd);
}

void ImGuiLayer::end_frame_without_render() {
    if (!frame_open_) return;
    ImGui::EndFrame();
    frame_open_ = false;
}

bool ImGuiLayer::wants_mouse() const { return ImGui::GetIO().WantCaptureMouse; }
bool ImGuiLayer::wants_keyboard() const { return keyboard_for_ui(keyboard_request_); }

void configure_keys() { ImGui::GetIO().ConfigMacOSXBehaviors = false; }

void keep_tab_from_windows() {
    // ImGui's tabbing is always on (NavUpdateCreateTabbingRequest, whatever ImGuiConfigFlags_NavEnableKeyboard says)
    // and, with no item active, a Tab focuses the first field of the focused window: the chat's input took the keys
    // from a Tab meant for the game. Owned for the frame, the press is not ImGui's navigation's (it tests for no
    // owner), while ImGui::IsKeyPressed (any owner: a browser page's keys) still sees it. A field being typed in
    // leaves Tab alone, so it still moves to the next field.
    if (GImGui->ActiveId == 0) ImGui::SetKeyOwner(ImGuiKey_Tab, ImHashStr("##GameTab"));
}

bool keyboard_for_ui(int requested) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard) return false;
    if (requested == 1 || io.WantTextInput || ImGui::GetTopMostPopupModal()) return true;
    const ImGuiContext& g = *GImGui;
    return g.ActiveId == 0 || g.ActiveIdSource != ImGuiInputSource_Mouse;
}

float ui_scale() { return g_ui_scale; }
void set_ui_scale(float scale) { g_ui_scale = scale; }

ImFont* chat_font(int index) {
    return index >= 0 && index < static_cast<int>(kChatFonts) ? g_chat_fonts[static_cast<std::size_t>(index)] : nullptr;
}

float chat_font_size(int index) {
    return index >= 0 && index < static_cast<int>(kChatFonts) ? kChatFontSizes[static_cast<std::size_t>(index)] : 0.0f;
}

ImFont* plate_font() { return g_plate_font; }
float plate_font_size() { return g_plate_font_size; }
bool plate_font_faux_bold() { return g_plate_faux_bold; }

ImFont* eq_font(int index) {
    return index >= 1 && index < static_cast<int>(kEqFontSizes.size()) ? g_eq_fonts[static_cast<std::size_t>(index)]
                                                                         : nullptr;
}

} // namespace hailstone::ui
