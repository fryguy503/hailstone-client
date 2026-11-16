#pragma once

#include <functional>
#include <optional>
#include <string_view>
#include <glm/glm.hpp>
#include <imgui.h>
#include "game/nameplate_canvas.h"

namespace hailstone::app {

struct NameplateCanvasPaintStyle {
    ImFont* font = nullptr;
    std::function<ImFont*(int)> font_at;
    std::function<bool(std::uintptr_t)> image_live;
};

std::optional<game::CanvasTextMetrics> measure_nameplate_canvas_text(
    std::string_view text, float size, int font, const NameplateCanvasPaintStyle& style);
glm::vec4 nameplate_canvas_bounds(const game::NameplateCanvasModel& model, glm::vec2 anchor);
bool paint_nameplate_canvas(ImDrawList& draw, const game::NameplateCanvasModel& model,
                            glm::vec2 anchor, const NameplateCanvasPaintStyle& style, float alpha = 1.0f);

} // namespace hailstone::app
