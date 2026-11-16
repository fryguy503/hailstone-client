#include "app/nameplate_canvas_painter.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <imgui_internal.h>

namespace hailstone::app {
namespace {
ImFont* canvas_font(int index, const NameplateCanvasPaintStyle& style) {
    if (index != 0 && style.font_at) {
        if (ImFont* font = style.font_at(index)) return font;
    }
    return style.font ? style.font : ImGui::GetFont();
}

ImU32 canvas_color(std::uint32_t rgb, float alpha) {
    const int opacity = static_cast<int>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
    return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, opacity);
}

void rounded_rect(ImDrawList& draw, ImVec2 lo, ImVec2 hi, ImU32 color, float radius,
                  bool filled, float thickness) {
    radius = std::min(radius, std::min((hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f));
    if (radius <= 0.0f) {
        if (filled) draw.AddRectFilled(lo, hi, color);
        else draw.AddRect(lo, hi, color, 0, 0, thickness);
        return;
    }
    // A public command has fixed tessellation, independent of global UI curve settings.
    constexpr float pi = 3.14159265358979323846f;
    draw.PathClear();
    draw.PathArcTo(ImVec2(lo.x + radius, lo.y + radius), radius, pi, pi * 1.5f, 16);
    draw.PathArcTo(ImVec2(hi.x - radius, lo.y + radius), radius, pi * 1.5f, pi * 2.0f, 16);
    draw.PathArcTo(ImVec2(hi.x - radius, hi.y - radius), radius, 0, pi * 0.5f, 16);
    draw.PathArcTo(ImVec2(lo.x + radius, hi.y - radius), radius, pi * 0.5f, pi, 16);
    if (filled) draw.PathFillConvex(color);
    else draw.PathStroke(color, ImDrawFlags_Closed, thickness);
}
} // namespace

std::optional<game::CanvasTextMetrics> measure_nameplate_canvas_text(
    std::string_view text, float size, int index, const NameplateCanvasPaintStyle& style) {
    if (!ImGui::GetCurrentContext() || !std::isfinite(size) || size < 4.0f || size > 256.0f ||
        index < 0 || index > 6 || text.size() > game::kCanvasMaxTextBytes) return std::nullopt;
    ImFont* font = canvas_font(index, style);
    if (!font || font->FontSize <= 0.0f) return std::nullopt;
    const ImVec2 advance = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
    game::CanvasTextMetrics result;
    result.width = advance.x;
    result.height = advance.y;
    float pen_x = 0, pen_y = 0;
    bool ink = false;
    const float scale = size / font->FontSize;
    const char* current = text.data();
    const char* end = current + text.size();
    while (current < end) {
        unsigned int codepoint = 0;
        const int consumed = ImTextCharFromUtf8(&codepoint, current, end);
        if (consumed <= 0) break;
        current += consumed;
        if (codepoint == '\n') { pen_x = 0; pen_y += size; continue; }
        if (codepoint == '\r') continue;
        const ImFontGlyph* glyph = font->FindGlyph(static_cast<ImWchar>(codepoint));
        if (!glyph) continue;
        if (glyph->Visible) {
            const float x0 = pen_x + glyph->X0 * scale, x1 = pen_x + glyph->X1 * scale;
            const float y0 = pen_y + glyph->Y0 * scale, y1 = pen_y + glyph->Y1 * scale;
            if (!ink) {
                result.ink_x0 = x0; result.ink_x1 = x1;
                result.ink_y0 = y0; result.ink_y1 = y1;
                ink = true;
            } else {
                result.ink_x0 = std::min(result.ink_x0, x0); result.ink_x1 = std::max(result.ink_x1, x1);
                result.ink_y0 = std::min(result.ink_y0, y0); result.ink_y1 = std::max(result.ink_y1, y1);
            }
        }
        pen_x += glyph->AdvanceX * scale;
    }
    return result;
}

glm::vec4 nameplate_canvas_bounds(const game::NameplateCanvasModel& model, glm::vec2 anchor) {
    const float x = anchor.x + model.offset_x + model.bounds.x;
    const float y = anchor.y + model.offset_y + model.bounds.y;
    return {x, y, x + model.bounds.w, y + model.bounds.h};
}

bool paint_nameplate_canvas(ImDrawList& draw, const game::NameplateCanvasModel& model,
                            glm::vec2 anchor, const NameplateCanvasPaintStyle& style, float opacity) {
    if (model.hide || !game::valid_canvas_model(model) || !std::isfinite(anchor.x) || !std::isfinite(anchor.y) ||
        !std::isfinite(opacity) || opacity <= 0.0f) return false;
    const glm::vec4 bounds = nameplate_canvas_bounds(model, anchor);
    const int vertices_before = draw.VtxBuffer.Size;
    const ImVec2 origin(anchor.x + model.offset_x, anchor.y + model.offset_y);
    const auto point = [&](float x, float y) { return ImVec2(origin.x + x, origin.y + y); };
    draw.PushClipRect(ImVec2(bounds.x, bounds.y), ImVec2(bounds.z, bounds.w), true);
    for (const auto& command : model.commands) {
        const ImU32 color = canvas_color(command.color, command.alpha * opacity);
        using Kind = game::CanvasCommandKind;
        switch (command.kind) {
        case Kind::Rect:
            rounded_rect(draw, point(command.x0, command.y0), point(command.x1, command.y1), color,
                         command.rounding, false, command.thickness);
            break;
        case Kind::RectFilled:
            rounded_rect(draw, point(command.x0, command.y0), point(command.x1, command.y1), color,
                         command.rounding, true, command.thickness);
            break;
        case Kind::Gradient: {
            if (command.rounding <= 0.0f) {
                const ImU32 lower = canvas_color(command.color2, command.alpha2 * opacity);
                draw.AddRectFilledMultiColor(point(command.x0, command.y0), point(command.x1, command.y1), color, color, lower, lower);
            } else {
                const int start = draw.VtxBuffer.Size;
                rounded_rect(draw, point(command.x0, command.y0), point(command.x1, command.y1), IM_COL32_WHITE,
                             command.rounding, true, command.thickness);
                const float height = command.y1 - command.y0;
                for (int i = start; i < draw.VtxBuffer.Size; ++i) {
                    auto& vertex = draw.VtxBuffer[i];
                    const float t = height > 0 ? std::clamp((vertex.pos.y - origin.y - command.y0) / height, 0.0f, 1.0f) : 0.0f;
                    const auto channel = [&](int shift) {
                        const float upper = static_cast<float>((command.color >> shift) & 255u);
                        const float lower = static_cast<float>((command.color2 >> shift) & 255u);
                        return static_cast<int>(std::lround(upper + (lower - upper) * t));
                    };
                    const float coverage = static_cast<float>((vertex.col >> IM_COL32_A_SHIFT) & 255u) / 255.0f;
                    const float alpha = std::clamp((command.alpha + (command.alpha2 - command.alpha) * t) * opacity, 0.0f, 1.0f);
                    vertex.col = IM_COL32(channel(16), channel(8), channel(0), static_cast<int>(std::lround(coverage * alpha * 255.0f)));
                }
            }
            break;
        }
        case Kind::Line:
            draw.AddLine(point(command.x0, command.y0), point(command.x1, command.y1), color, command.thickness);
            break;
        case Kind::Circle:
            draw.AddCircle(point(command.x, command.y), command.radius, color, command.segments, command.thickness);
            break;
        case Kind::CircleFilled:
            draw.AddCircleFilled(point(command.x, command.y), command.radius, color, command.segments);
            break;
        case Kind::Triangle:
            draw.AddTriangle(point(command.x0, command.y0), point(command.x1, command.y1), point(command.x2, command.y2),
                             color, command.thickness);
            break;
        case Kind::TriangleFilled:
            draw.AddTriangleFilled(point(command.x0, command.y0), point(command.x1, command.y1), point(command.x2, command.y2), color);
            break;
        case Kind::Text:
            draw.AddText(canvas_font(command.font, style), command.size, point(command.x, command.y), color,
                         command.text.data(), command.text.data() + command.text.size());
            break;
        case Kind::Image:
            if (style.image_live && style.image_live(command.texture))
                draw.AddImage(static_cast<ImTextureID>(command.texture), point(command.x0, command.y0),
                              point(command.x1, command.y1), ImVec2(command.u0, command.v0),
                              ImVec2(command.u1, command.v1), color);
            break;
        }
    }
    draw.PopClipRect();
    return draw.VtxBuffer.Size > vertices_before;
}

} // namespace hailstone::app
