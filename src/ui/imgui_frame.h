#pragma once

// ImGui's frame number widened to 64 bits. ImGui keeps it in an int (ImGuiContext::FrameCount), which runs out after
// 2^31 frames: about 25 days at 1000 frames a second, 103 at 240, and its increment past INT_MAX is undefined.
// Hailstone's own frame numbers (a window's "this frame" marks, the Lua packs' ui.GetFrameCount, anything that
// subtracts or orders them) come from here instead: each call adds the frames since the last call (the 32-bit
// difference, which is right across ImGui's wrap as long as it is called at least once every 2^32 frames), so the
// value only grows and never wraps in practice (about 58 million years at 10 000 frames a second).
//
// It follows ImGui's frames, so it advances whenever a frame is begun, in the client or in a test. A new ImGui
// context (its count starts again at 0, maybe at the old one's address) moves it on by one rather than back: a step of
// 2^31 frames or more between two calls is taken for that, never for frames drawn. Without a context it stays.
// Read on the UI thread only.

#include <cstdint>

#include <imgui.h>

namespace hailstone::ui {

inline std::uint64_t imgui_frame() {
    struct State {
        const ImGuiContext* context = nullptr;
        std::uint32_t last = 0;  // ImGui's count at the last call, as unsigned bits
        std::uint64_t value = 0; // 0 until the first call with a context
    };
    static State s;
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context) return s.value;
    const auto count = static_cast<std::uint32_t>(ImGui::GetFrameCount());
    const auto step = static_cast<std::uint32_t>(count - s.last); // unsigned: the wrap past INT_MAX is one frame
    if (context != s.context || step >= 0x80000000u) {
        s.context = context;
        s.value += 1;
    } else {
        s.value += step;
    }
    s.last = count;
    return s.value;
}

} // namespace hailstone::ui
