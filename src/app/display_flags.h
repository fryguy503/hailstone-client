#pragma once

// The display flags the client takes (main.cpp): each stands for a hailstone.ini display setting, given for that run
// only (Config::set_run_override), as --max-fps stands for display.max_fps. The viewers (--zone, --model) take the
// same flags as their own options.

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hailstone::app {

// The setting and value `flag` (with its argument `arg`, "" for a switch) stands for in the client; nullopt for a
// flag the client does not take: not a display flag, or --lighting none (full-bright, the viewer's alone).
inline std::optional<std::pair<std::string, std::string>> display_flag_setting(std::string_view flag,
                                                                              std::string_view arg = {}) {
    auto is = [&](std::string_view f) { return flag == f; };
    auto as = [](std::string_view key, std::string_view value) {
        return std::optional<std::pair<std::string, std::string>>(std::pair(std::string(key), std::string(value)));
    };
    if (is("--lighting")) {
        if (arg == "modern" || arg == "smooth" || arg == "classic") return as("display.lighting", arg);
        return std::nullopt;
    }
    if (is("--msaa")) return as("display.msaa", arg);
    if (is("--tonemap")) return as("display.tonemap", arg);
    if (is("--exposure")) return as("display.exposure", arg);
    if (is("--shadows")) return as("display.shadows", arg);
    if (is("--light-shadows")) return as("display.light_shadows", arg);
    if (is("--no-ao")) return as("display.ao", "0");
    if (is("--depth-prepass")) return as("display.depth_prepass", "1");
    if (is("--bloom")) return as("display.bloom", arg);
    if (is("--render-scale")) return as("display.render_scale", arg);
    if (is("--sharpness")) return as("display.sharpness", arg);
    if (is("--upscaler")) return as("display.upscaler", arg);
    if (is("--frame-gen")) return as("display.frame_gen", "1");
    if (is("--view-distance")) return as("display.view_distance", arg);
    return std::nullopt;
}

} // namespace hailstone::app
