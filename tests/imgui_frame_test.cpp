// ui::imgui_frame (ui/imgui_frame.h): ImGui's int frame count widened to 64 bits, right across its wrap past
// INT_MAX and across a new context, standing still without one.
#include <doctest/doctest.h>

#include <climits>
#include <cstdint>

#include <imgui.h>
#include <imgui_internal.h>

#include "ui/imgui_frame.h"

using hailstone::ui::imgui_frame;

TEST_CASE("imgui_frame: follows ImGui's frames, across its int wrap and a new context") {
    ImGuiContext* ctx = ImGui::CreateContext();
    const std::uint64_t start = imgui_frame(); // a new context: one past whatever came before
    CHECK(imgui_frame() == start);             // no frame since: the same
    GImGui->FrameCount += 1;
    CHECK(imgui_frame() == start + 1);
    GImGui->FrameCount += 5; // frames not asked about are counted all the same
    CHECK(imgui_frame() == start + 6);
    // The count going back to 0 (a context made again, maybe at the same address): one on, not 2^32 - 6.
    GImGui->FrameCount = 0;
    CHECK(imgui_frame() == start + 7);
    // ImGui's count reaching INT_MAX and going on (its own increment wraps to INT_MIN): one frame each, upwards.
    GImGui->FrameCount = INT_MAX - 1;
    const std::uint64_t before_wrap = imgui_frame();
    CHECK(before_wrap > start + 7);
    GImGui->FrameCount = INT_MAX;
    CHECK(imgui_frame() == before_wrap + 1);
    GImGui->FrameCount = INT_MIN; // (what the wrapped increment gives)
    CHECK(imgui_frame() == before_wrap + 2);
    GImGui->FrameCount = INT_MIN + 1;
    CHECK(imgui_frame() == before_wrap + 3);
    ImGui::DestroyContext(ctx);
    // No context: it stands where it was.
    CHECK(imgui_frame() == before_wrap + 3);
}
