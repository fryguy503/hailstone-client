#pragma once

// Which level of detail of a replacement model (assets::MeshPart::lod_near, lod_far; docs/graphics.md "Model
// overrides") a pass draws (render/zone_renderer.cpp). The scene shows the level whose band holds the placement's
// distance from the camera. A shadow needs less, seen at its map's texels.

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>

namespace hailstone::render {

// One level's band: drawn from `near` to `far` units from the camera (far 0: not a level, always drawn), the
// `level`th of its mesh's `levels`, nearest first.
struct LodBand {
    float near = 0.0f, far = 0.0f;
    std::uint8_t level = 0, levels = 0;
    bool banded() const { return far > 0.0f; }
    bool operator<(const LodBand& o) const {
        return std::tie(near, far, level, levels) < std::tie(o.near, o.far, o.level, o.levels);
    }
};

// Which level a pass draws: the scene's, by distance (level 0, not exact); at least `level`, the camera's where that
// is coarser; or, `exact`, that level whatever the distance. A level past a mesh's last is its last.
struct LodPick {
    std::uint8_t level = 0;
    bool exact = false;
    bool fade = false; // the scene's: a level also shows through its fades (lod_presence), either side of its band
};
inline constexpr LodPick kSceneLod{0, false, true};

// A level fades into the next across a stretch of kLodFade x the distance where they meet, centred on it: the scene
// draws both there, each of a pixel's samples of one or the other (coverage.glsl lod_fade_samples), so a tree
// dissolves into its next level instead of jumping.
inline constexpr float kLodFade = 0.2f;

// How much of `band` shows at `dist` (both scaled), 0 to 1: rising across the fade at its near end (from the start,
// none), falling across the one at its far end. Where two levels meet, the one fading in and the one fading out add
// up to 1 (coverage.glsl lod_fade_samples gives the two complementary samples).
struct LodPresence {
    float in = 1.0f, out = 1.0f;
};
inline LodPresence lod_presence(const LodBand& band, float dist, float scale = 1.0f) {
    LodPresence p;
    if (!band.banded()) return p;
    const float near = band.near * scale, far = band.far * scale;
    if (near > 0.0f) p.in = std::clamp((dist - near * (1.0f - 0.5f * kLodFade)) / (kLodFade * near), 0.0f, 1.0f);
    p.out = std::clamp((far * (1.0f + 0.5f * kLodFade) - dist) / (kLodFade * far), 0.0f, 1.0f);
    return p;
}

// The sun's cascades (render/shadows.h): the two nearest and sharpest the middle level, the others the last (a
// generated tree's far cards).
inline constexpr std::array<std::uint8_t, 4> kShadowLodLevel = {1, 1, 255, 255};
// A light's shadow cube, drawn once and kept (render/point_shadows.h): the middle level wherever the camera is.
inline constexpr LodPick kPointShadowLod{1, true};

// Whether `band` shows by `pick` at `dist` from the camera, its distances times `scale` (the detail setting).
inline bool lod_shows(const LodBand& band, float dist, LodPick pick = {}, float scale = 1.0f) {
    if (!band.banded()) return true;
    const std::uint8_t level = std::min<std::uint8_t>(pick.level, band.levels > 0 ? band.levels - 1 : 0);
    if (pick.exact) return band.level == level;
    if (band.level < level) return false;
    if (pick.fade) {
        const LodPresence p = lod_presence(band, dist, scale);
        return p.in > 0.0f && p.out > 0.0f;
    }
    return (band.level == level || dist >= band.near * scale) && dist < band.far * scale;
}

} // namespace hailstone::render
