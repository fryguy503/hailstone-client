#pragma once

// World anchors describe a rendered pose. Their optional floor is a current-frame collision sample, rather than
// the original client's actor cache; the native cache's plausibility limits still reject implausible layers.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

#include "game/coords.h"
#include "game/animated_objects.h"
#include "game/collision.h"

namespace hailstone::app {

// A visual surface query includes static props and the same current moving geometry as the scene.
// The caller's ceiling selects the layer; a query never reaches upward through another floor.
template<class ObjectFloor>
std::optional<float> world_geometry_surface_floor(const game::CollisionMesh& mesh,
                                                 const game::AnimatedObjects* animated,
                                                 ObjectFloor&& object_floor, const glm::vec3& at) {
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) return std::nullopt;
    std::optional<float> best;
    const auto take = [&](std::optional<float> candidate) {
        if (candidate && std::isfinite(*candidate) && *candidate <= at.z && (!best || *candidate > *best))
            best = candidate;
    };
    take(mesh.visual_surface_below(at));
    take(object_floor(at));
    if (animated) take(animated->visual_surface_below(at));
    return best;
}

struct WorldSceneKey {
    const void* connection = nullptr; // identity only; never dereferenced
    std::uint32_t epoch = 0;
    std::uint16_t zone = 0, instance = 0;
    std::string short_name;
    bool operator==(const WorldSceneKey&) const = default;
    bool matches(const void* current_connection, std::uint32_t current_epoch, std::uint16_t current_zone,
                 std::uint16_t current_instance, std::string_view current_name) const {
        return connection && epoch && !short_name.empty() && connection == current_connection &&
               epoch == current_epoch && zone == current_zone && instance == current_instance && short_name == current_name;
    }
    bool matches(const WorldSceneKey& current) const {
        return matches(current.connection, current.epoch, current.zone, current.instance, current.short_name);
    }
};

inline std::optional<float> drawn_world_heading(const glm::mat4& transform) {
    // ActorRenderer::instance_matrix turns its +X model front about +Z. Attached actors use their actual parent
    // bone transform, so their heading must come from that same matrix, not a spawn's network heading.
    const glm::vec2 front(transform[0]);
    if (!std::isfinite(front.x) || !std::isfinite(front.y) || glm::dot(front, front) <= 1.0e-12f)
        return std::nullopt;
    return game::yaw_to_heading(std::atan2(front.y, front.x));
}

inline std::optional<float> cached_world_anchor_floor(float actor_z, float floor_z, float avatar_height) {
    if (!std::isfinite(actor_z) || !std::isfinite(floor_z)) return std::nullopt;
    const float height = std::isfinite(avatar_height) && avatar_height > 0.1f ? std::min(avatar_height, 100.0f) : 6.0f;
    const float difference = actor_z - floor_z;
    if (difference < -std::max(2.0f, height * 0.25f) || difference > std::max(100.0f, height * 8.0f))
        return std::nullopt;
    return floor_z;
}

} // namespace hailstone::app
