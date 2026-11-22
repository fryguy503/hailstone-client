#pragma once

// ui/lua/lua_imgui.cpp
namespace hailstone::ui::lua {

// The ui.Draw* functions draw on the current window's own list, unless the code around them asked for
// ui.Foreground(true) in this frame, which draws above every window. A retained callback that must stay inside the
// window it is drawn into (a map overlay inside the map's clip, whichever add-on's map code called it in) holds this
// for as long as it draws: its own ui.Foreground(true) still takes effect, an inherited one does not.
struct WindowDrawScope {
    WindowDrawScope();
    ~WindowDrawScope();
    WindowDrawScope(const WindowDrawScope&) = delete;
    WindowDrawScope& operator=(const WindowDrawScope&) = delete;

private:
    int before_;
};

} // namespace hailstone::ui::lua
