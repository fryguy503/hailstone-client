#pragma once

// Smoothed ground (docs/graphics.md, "Smoothed ground"): a classic zone's ground drawn with smooth normals and
// curved, subdivided triangles instead of the file's flat facets. Not the original's; the look only. Zone::terrain
// stays as loaded and keeps collision, the regions, the liquids' classification and everything the server's geometry
// must agree with; the refined copy goes into Zone::draw_terrain, which the zone renderer and the zone flora use.
//
// - Ground: the solid, opaque triangles of the classic (WLD) terrain meshes. Not the cut-outs, the translucent or
//   invisible surfaces, the passable ones (water), nor EQG meshes (their own art is already detailed).
// - Normals: the ground's corners are welded by position across meshes and materials, and each corner takes the
//   angle-weighted average of the faces round it within `crease_degrees` of its own face: hills turn smooth, while a
//   wall's corner or a cliff's edge stays sharp.
// - Shape: each triangle becomes a curved PN triangle (Vlachos et al. 2001) over its corner normals, cut into 2, 4 or
//   8 segments a side where its curve leaves the flat triangle by more than `tolerance`. An edge is curved only where
//   both of its triangles are ground and agree on both corner normals; elsewhere (a crease, the edge of the ground, a
//   seam with a wall or a cut-out) it stays the file's straight edge and is not cut, so the refined ground meets the
//   rest of the zone without cracks. The curve's offset from the flat triangle is held within `max_offset`, so feet
//   and the server's spawn heights (measured on the flat ground) stay where they meet the drawn ground.

#include <cstddef>
#include <cstdint>

#include "assets/zone.h"

namespace hailstone::assets {

struct GroundRefineOptions {
    float crease_degrees = 45.0f;    // faces more than this apart do not share a normal
    float tolerance = 0.05f;         // units: the most a segment may cut through the curve
    float max_offset = 1.0f;         // units: the most the drawn ground leaves the flat triangle
    std::uint32_t max_segments = 8;  // a side, a power of two
};

struct GroundRefineStats {
    std::size_t ground_triangles = 0; // of the terrain, refined
    std::size_t curved_edges = 0;
    std::size_t triangles = 0;        // drawn in their place
    float max_offset = 0.0f;          // the most a drawn vertex leaves its flat triangle
};

// Fills zone.draw_terrain and zone.draw_terrain_source from zone.terrain (cleared first). Leaves both empty when the
// zone has no ground to refine.
GroundRefineStats refine_ground(Zone& zone, const GroundRefineOptions& options = {});

} // namespace hailstone::assets
