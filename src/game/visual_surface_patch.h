#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

#include <glm/glm.hpp>

#include "game/swept_sphere.h"

namespace hailstone::game {

struct VisualSurfacePatch {
    std::array<glm::vec3,3> vertices{}; // proposed surface, before any effect lift
    float maximum_rise=0, tolerance=0;
};
enum class VisualSurfacePatchStatus {Deferred,Planar,Mismatch,Missing};
struct VisualSurfacePatchResult {
    VisualSurfacePatchStatus status=VisualSurfacePatchStatus::Deferred;
    glm::vec3 witness{0};
    bool height_known=false;
    std::array<glm::vec3,3> source_face{};
    bool source_face_known=false;
    std::array<glm::dvec2,32> region{}; // convex refinement hint in native XY, never a planar certificate
    std::uint32_t region_count=0;
    const char* diagnostic=nullptr; // internal diagnostic; never exposed through the addon ABI
};
using VisualTriangleVisitor=std::function<bool(const CollisionTriangle&)>;
// Enumerate every potentially overlapping current drawn triangle. False means work was exhausted or visitor
// stopped. The source charges cell/candidate enumeration against work; the checker charges bounded polygon work.
using VisualTriangleSource=std::function<bool(const glm::vec2&,const glm::vec2&,const VisualTriangleVisitor&,
                                            std::uint32_t& work)>;
using VisualFloorProbe=std::function<std::optional<float>(const glm::vec3&)>;

// Conservative certificate, never a sampling-only flatness guess. Higher faces are checked even after coverage
// is complete. Union coverage is proved by polygon subtraction, so overlaps cannot compensate for holes.
VisualSurfacePatchResult certify_visual_surface_patch(const VisualSurfacePatch&,const VisualTriangleSource&,
                                                       const VisualFloorProbe&,std::uint32_t& work);

} // namespace hailstone::game
