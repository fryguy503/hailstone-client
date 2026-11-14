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
};

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
    return (band.level == level || dist >= band.near * scale) && dist < band.far * scale;
}

} // namespace hailstone::render
