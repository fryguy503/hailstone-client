#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <imgui.h>
#include "app/nameplate_canvas_painter.h"

namespace {
struct CanvasGui {
    ImGuiContext* context = ImGui::CreateContext();
    CanvasGui() {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1600, 1200};
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* data = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&data, &width, &height);
        ImGui::NewFrame();
    }
    ~CanvasGui() { ImGui::EndFrame(); ImGui::DestroyContext(context); }
};
hailstone::game::NameplateCanvasModel model() {
    hailstone::game::NameplateCanvasModel value;
    value.token = "current"; value.bounds = {-120, -80, 240, 100};
    return value;
}
}

TEST_CASE("nameplate canvas painter: primitives preserve local anchors and clip to the declared bounds") {
    CanvasGui gui;
    using namespace hailstone;
    auto value = model();
    for (int kind = 0; kind < 10; ++kind) {
        game::CanvasCommand command;
        command.kind = static_cast<game::CanvasCommandKind>(kind);
        command.x0 = -60; command.y0 = -50; command.x1 = 60; command.y1 = -20;
        command.x2 = 0; command.y2 = -10; command.x = 0; command.y = -40;
        command.radius = 8; command.text = "Canvas"; command.texture = 1;
        value.commands.push_back(command);
    }
    auto* draw = ImGui::GetBackgroundDrawList();
    app::NameplateCanvasPaintStyle style;
    style.image_live = [](std::uintptr_t texture) { return texture == 1; };
    const auto before = draw->VtxBuffer.Size;
    REQUIRE(app::paint_nameplate_canvas(*draw, value, {300, 200}, style));
    CHECK(draw->VtxBuffer.Size > before);
    const auto bounds = app::nameplate_canvas_bounds(value, {300, 200});
    CHECK(bounds == glm::vec4(180, 120, 420, 220));
    for (const auto& command : draw->CmdBuffer) {
        if (command.ElemCount == 0) continue;
        CHECK(command.ClipRect.x >= bounds.x); CHECK(command.ClipRect.y >= bounds.y);
        CHECK(command.ClipRect.z <= bounds.z); CHECK(command.ClipRect.w <= bounds.w);
    }
}

TEST_CASE("nameplate canvas painter: alpha composes and hidden or invalid commands emit no geometry") {
    CanvasGui gui;
    using namespace hailstone;
    auto value = model();
    game::CanvasCommand command;
    command.kind = game::CanvasCommandKind::RectFilled;
    command.x0 = -20; command.y0 = -20; command.x1 = 20; command.y1 = 0;
    command.color = 0x123456; command.alpha = 0.5f;
    value.commands.push_back(command);
    auto* draw = ImGui::GetBackgroundDrawList();
    const auto first = draw->VtxBuffer.Size;
    REQUIRE(app::paint_nameplate_canvas(*draw, value, {200, 200}, {}, 0.5f));
    for (int i = first; i < draw->VtxBuffer.Size; ++i) {
        const auto alpha = (draw->VtxBuffer[i].col >> IM_COL32_A_SHIFT) & 255u;
        CHECK(alpha <= 64);
    }
    const auto count = draw->VtxBuffer.Size;
    value.hide = true;
    CHECK_FALSE(app::paint_nameplate_canvas(*draw, value, {200, 200}, {}));
    value.hide = false; value.commands[0].x0 = std::numeric_limits<float>::infinity();
    CHECK_FALSE(app::paint_nameplate_canvas(*draw, value, {200, 200}, {}));
    CHECK(draw->VtxBuffer.Size == count);
}

TEST_CASE("nameplate canvas painter: image handles are checked again at draw time") {
    CanvasGui gui;
    using namespace hailstone;
    auto value = model();
    game::CanvasCommand image;
    image.kind = game::CanvasCommandKind::Image;
    image.x0 = -20; image.y0 = -20; image.x1 = 20; image.y1 = 0; image.texture = 42;
    value.commands.push_back(image);
    app::NameplateCanvasPaintStyle style;
    style.image_live = [](std::uintptr_t) { return false; };
    CHECK_FALSE(app::paint_nameplate_canvas(*ImGui::GetBackgroundDrawList(), value, {200, 200}, style));
    CHECK(ImGui::GetBackgroundDrawList()->VtxBuffer.Size == 0);
}

TEST_CASE("nameplate canvas painter: rounded primitive costs bound actual tessellation") {
    CanvasGui gui;
    using namespace hailstone;
    for (const bool filled : {false, true}) {
        auto value = model();
        game::CanvasCommand command;
        command.kind = filled ? game::CanvasCommandKind::RectFilled : game::CanvasCommandKind::Rect;
        command.x0 = -100; command.y0 = -80; command.x1 = 100; command.y1 = 20;
        command.rounding = 2048; command.thickness = 64;
        value.commands.push_back(command);
        auto* draw = ImGui::GetBackgroundDrawList();
        const auto before = draw->VtxBuffer.Size;
        REQUIRE(app::paint_nameplate_canvas(*draw, value, {300, 300}, {}));
        CHECK(static_cast<std::size_t>(draw->VtxBuffer.Size - before) <= game::canvas_command_vertices(command));
    }
}

TEST_CASE("nameplate canvas measurement: advance and glyph ink match rendered text at scale") {
    CanvasGui gui;
    using namespace hailstone;
    app::NameplateCanvasPaintStyle style;
    for (const float size : {4.0f, 16.0f, 47.5f, 256.0f}) {
        const auto measured = app::measure_nameplate_canvas_text("gjQ!", size, 0, style);
        REQUIRE(measured);
        auto value = model(); value.bounds = {-500, -500, 1000, 1000};
        game::CanvasCommand command;
        command.kind = game::CanvasCommandKind::Text; command.text = "gjQ!"; command.size = size;
        value.commands.push_back(command);
        auto* draw = ImGui::GetBackgroundDrawList();
        const auto first = draw->VtxBuffer.Size;
        REQUIRE(app::paint_nameplate_canvas(*draw, value, {600, 600}, style));
        for (int i = first; i < draw->VtxBuffer.Size; ++i) {
            const auto& vertex = draw->VtxBuffer[i];
            CHECK(vertex.pos.x >= 600 + measured->ink_x0 - 1.01f);
            CHECK(vertex.pos.x <= 600 + measured->ink_x1 + 1.01f);
            CHECK(vertex.pos.y >= 600 + measured->ink_y0 - 1.01f);
            CHECK(vertex.pos.y <= 600 + measured->ink_y1 + 1.01f);
        }
    }
    CHECK_FALSE(app::measure_nameplate_canvas_text("test", std::numeric_limits<float>::quiet_NaN(), 0, style));
    CHECK_FALSE(app::measure_nameplate_canvas_text("test", 512, 0, style));
}

TEST_CASE("nameplate canvas painter: rounded gradients preserve corner shape and interpolated alpha") {
    CanvasGui gui;
    using namespace hailstone;
    auto value = model();
    game::CanvasCommand gradient;
    gradient.kind = game::CanvasCommandKind::Gradient;
    gradient.x0 = -60; gradient.y0 = -60; gradient.x1 = 60; gradient.y1 = -20;
    gradient.rounding = 12; gradient.color = 0xff0000; gradient.color2 = 0x0000ff;
    gradient.alpha = 0.25f; gradient.alpha2 = 0.75f;
    value.commands.push_back(gradient);
    auto* draw = ImGui::GetBackgroundDrawList();
    const auto first = draw->VtxBuffer.Size;
    REQUIRE(app::paint_nameplate_canvas(*draw, value, {300, 300}, {}));
    CHECK(static_cast<std::size_t>(draw->VtxBuffer.Size - first) <= game::canvas_command_vertices(gradient));
    bool top = false, bottom = false;
    for (int i = first; i < draw->VtxBuffer.Size; ++i) {
        const auto& vertex = draw->VtxBuffer[i];
        const auto alpha = (vertex.col >> IM_COL32_A_SHIFT) & 255u;
        CHECK(alpha <= 192);
        if (alpha == 0) continue;
        CHECK_FALSE((std::abs(vertex.pos.x - 240) < 0.01f && std::abs(vertex.pos.y - 240) < 0.01f));
        if (vertex.pos.y < 241) {
            top = true;
            CHECK(((vertex.col >> IM_COL32_R_SHIFT) & 255u) > 240);
            CHECK(alpha <= 70);
        }
        if (vertex.pos.y > 279) {
            bottom = true;
            CHECK(((vertex.col >> IM_COL32_B_SHIFT) & 255u) > 240);
            CHECK(alpha > 150);
        }
    }
    CHECK(top); CHECK(bottom);
}
