#pragma once

// Lua nameplate presentation API 2. These values describe appearance only; the app owns world visibility,
// projection, occlusion and drawing. An absent presentation leaves the ordinary nameplate intact.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hailstone::game {

struct NameplateCast {
    std::uint32_t spell_id = 0;
    std::string name;
    std::uint32_t elapsed_ms = 0, duration_ms = 0;
};

struct NameplateContext {
    std::string name; // the assembled name, after the ordinary text hook
    bool target = false, self = false, in_group = false, in_raid = false;
    float distance = 0.0f;
    double now_ms = 0.0, dt_ms = 0.0;
    std::optional<float> hp_percent, mana_percent, endurance_percent, aggro_percent;
    int group_mark = 0, raid_mark = 0;
    std::optional<NameplateCast> casting;
};

struct NameplateBar {
    float value = 0.0f; // percent, 0..100
    std::uint32_t color = 0xffffff;
    std::string text;
};

struct NameplateBadge {
    std::string icon; // eye, peace, sleep, star, sun, moon, diamond, triangle, cross, skull, sword, circle
    std::string label;
    std::uint32_t color = 0xffffff;
    std::optional<std::uint32_t> spell_icon; // the player's loaded spell art, 0..65535; vector icon remains fallback
};

struct NameplatePresentation {
    bool hide = false, show_name = true, show_marker = true;
    bool stack = false;
    bool through_walls = false, clickable = true;
    std::optional<std::string> name, subtitle;
    float width = 132.0f, height = 12.0f, mana_height = 4.0f, cast_height = 7.0f;
    float font_scale = 1.0f, alpha = 1.0f, scale = 1.0f, offset_y = 0.0f;
    float stack_gap = 4.0f, max_stack = 96.0f;
    float border_width = 1.0f, rounding = 2.0f;
    float badge_scale = 1.0f, cast_gap = 2.0f, cast_width = 0.0f;
    std::string texture = "flat", name_position = "above", text_align = "center";
    std::string frame_style = "solid", target_style = "arrow", badge_position = "above";
    std::string health_text_align = "center", cast_text_align = "center";
    std::uint32_t text_color = 0xffffff, border_color = 0x20252e, background_color = 0x11151d;
    std::optional<NameplateBar> health, mana, cast;
    std::vector<NameplateBadge> badges; // at most eight
};

} // namespace hailstone::game
