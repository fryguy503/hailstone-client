#pragma once

// The keys, the mouse, the frame and the screen as ImGui has them, for the stock windows' clicks and state
// (app/lua_ui_*.cpp). Scripts and /notify read and press those windows with nothing drawn (a client that draws
// nothing has no ImGui context): then no key is held, the mouse is on no point, the frame stands still and the screen
// has no size. With a context these are exactly ImGui's. Drawing itself (Begin, draw lists, fonts) runs inside a frame and
// needs none of this.

#include <cfloat>
#include <cstdint>

#include <imgui.h>

#include "ui/imgui_frame.h"

namespace hailstone::app {

struct UiKeys {
    bool shift = false, ctrl = false, alt = false;
};

inline bool ui_has_imgui() { return ImGui::GetCurrentContext() != nullptr; }

inline UiKeys ui_keys() {
    if (!ui_has_imgui()) return {};
    const ImGuiIO& io = ImGui::GetIO();
    return {io.KeyShift, io.KeyCtrl, io.KeyAlt};
}

// ImGui's "no mouse" point (-FLT_MAX, -FLT_MAX) without a context, which ui_mouse_pos_valid() refuses.
inline ImVec2 ui_mouse_pos() { return ui_has_imgui() ? ImGui::GetMousePos() : ImVec2(-FLT_MAX, -FLT_MAX); }
inline bool ui_mouse_pos_valid(const ImVec2* pos = nullptr) { return ui_has_imgui() && ImGui::IsMousePosValid(pos); }

// The frame number, 64-bit (ui::imgui_frame: ImGui's int count widened, so it never wraps).
inline std::uint64_t ui_frame() { return ui::imgui_frame(); }

inline ImVec2 ui_display_size() { return ui_has_imgui() ? ImGui::GetIO().DisplaySize : ImVec2(0.0f, 0.0f); }

inline void ui_set_clipboard(const char* text) {
    if (ui_has_imgui()) ImGui::SetClipboardText(text);
}

} // namespace hailstone::app
