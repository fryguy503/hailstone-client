#pragma once

// The minimum text size (game/accessibility.h min_text_size; docs/dev-ui.md "Accessibility"): no UI text drawn smaller
// than the floor, in ImGui's pixels (the points times the display scale). ImGui has no hook for it, so it is held at
// the places text gets its size:
//   - the UI font and the name plates' font are built no smaller (ImGuiLayer::build_fonts);
//   - the Lua bridge's ui.PushFont, ui.PushChatFont and ui.SetWindowFontScale raise a size below it, and ui.DrawText's
//     explicit size (ui/lua/lua_imgui.cpp);
//   - the C++ windows' own font pushes (app/chat_tab_bar.cpp) pass their size through floor_text_px.
// Text may then be larger than a window's layout planned for. Not held: the map's labels (app/lua_ui_map.cpp), which
// draw at an explicit size that follows the zoom (docs/todo.md "Accessibility").

#include <algorithm>

namespace hailstone::ui {

// `px` raised to `floor_px` (0 or less: no floor).
inline float floor_text_px(float px, float floor_px) { return floor_px > 0.0f ? std::max(px, floor_px) : px; }

// The multiplier that brings text now `size_px` tall (drawn with multiplier `scale`) up to `floor_px`; `scale` as it
// is when the text is already big enough, there is no floor, or the size is not known (0).
inline float floor_text_scale(float size_px, float scale, float floor_px) {
    if (floor_px <= 0.0f || size_px <= 0.0f || size_px >= floor_px) return scale;
    return scale * floor_px / size_px;
}

// The floor in force, in ImGui pixels (ImGuiLayer::set_min_text_size times the display scale); 0 none. The setter is
// ImGuiLayer's (and the tests', which have no layer).
float text_floor_px();
void set_text_floor_px(float px);

} // namespace hailstone::ui
