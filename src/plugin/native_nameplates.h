#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "game/nameplate_canvas.h"
#include "hailstone_nameplates.h"

namespace hailstone::plugin {

// A native provider's temporary view and owned submissions for one registry callback.
class NativeNameplateFrame {
public:
    NativeNameplateFrame(game::NameplateCanvasRegistry& registry, const game::NameplateCanvasFrame& frame);
    NativeNameplateFrame(const NativeNameplateFrame&) = delete;
    NativeNameplateFrame& operator=(const NativeNameplateFrame&) = delete;

    const HailstoneNameplateFrame& view() const { return view_; }
    int submit(const HailstoneNameplateModel* model);
    int measure_text(const char* text, std::uint32_t text_bytes, float font_size, std::int32_t font,
                     HailstoneNameplateTextMetrics* out) const;
    int spell_icon(std::uint32_t icon, HailstoneNameplateImage* out);
    std::vector<game::NameplateCanvasModel> take_models();

private:
    game::NameplateCanvasRegistry& registry_;
    const game::NameplateCanvasFrame& frame_;
    std::vector<HailstoneNameplateCandidate> candidates_;
    HailstoneNameplateFrame view_{};
    std::vector<game::NameplateCanvasModel> models_;
    std::bitset<game::kCanvasMaxCandidates> claimed_;
    std::vector<std::uintptr_t> textures_;
    std::size_t command_count_ = 0;
    std::size_t vertex_count_ = 0;
};

} // namespace hailstone::plugin
