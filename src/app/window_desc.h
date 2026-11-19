#pragma once

// The game window as the client starts it (ClientApp's window_): the window as it was when the client last closed
// (hailstone.ini window.*), unless --window-size asked for a size. The UI's places are kept for that screen, so
// starting it smaller piles the windows up.

#include <algorithm>

#include "app/client_app.h"
#include "core/config.h"
#include "platform/window.h"

namespace hailstone::app {

// `saved`: hailstone.ini as read, nullptr when it could not be. --window-size counts whatever size it names,
// 1280x720 (the default's own size) too.
inline platform::WindowDesc window_desc(const ClientOptions& o, const Config* saved) {
    platform::WindowDesc d{.width = o.window_width, .height = o.window_height};
    d.pad_input_in_background = !o.input_script.empty();
    if (o.window_size_given || !saved) return d;
    d.width = std::max(320, saved->get_int("window.width", d.width));
    d.height = std::max(240, saved->get_int("window.height", d.height));
    if (saved->get("window.x") && saved->get("window.y"))
        d.position = {{saved->get_int("window.x", 0), saved->get_int("window.y", 0)}};
    d.maximized = saved->get_bool("window.maximized", false);
    return d;
}

} // namespace hailstone::app
