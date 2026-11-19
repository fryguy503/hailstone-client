#include "app/nameplate_input.h"
#include <algorithm>
#include <cmath>

namespace hailstone::app {

std::optional<std::uint32_t> pick_nameplate(const std::vector<NameplateHit>& hits, glm::vec2 at,
                                           const std::function<bool(const NameplateHit&)>& eligible) {
    if (!std::isfinite(at.x) || !std::isfinite(at.y)) return std::nullopt;
    for (auto hit = hits.rbegin(); hit != hits.rend(); ++hit) {
        const auto& box = hit->bounds;
        if (at.x >= box.x && at.x <= box.z && at.y >= box.y && at.y <= box.w && eligible(*hit)) return hit->id;
    }
    return std::nullopt;
}

glm::vec3 nameplate_head(const glm::vec3& base, std::optional<float> body_top, float eye,
                         const std::optional<glm::vec3>& seat) {
    if (seat) return *seat + glm::vec3(0.0f, 0.0f, (body_top ? 0.0f : eye) + kNameplateHeadOffset);
    return base + glm::vec3(0.0f, 0.0f, body_top.value_or(eye) + kNameplateHeadOffset);
}

bool nameplate_mouse_eligible(const game::MaskSpawn& spawn, std::uint8_t options, bool self, int camera) {
    const auto mask = game::spawn_pick_mask(spawn, options);
    if ((mask & game::kClickSkip) != 0 || (mask & game::kClickRequired) == 0) return false;
    game::PickCallInput in;
    in.camera_type = camera;
    in.click_through = options;
    return !self || !game::pick_call(in).exclude_view;
}

std::optional<float> nameplate_scene_depth(const game::PickSegment& ray, const game::PickScene& scene,
                                          const glm::vec3& forward) {
    float nearest = 1.0f;
    const auto triangle = [&](const game::PickTriangle& t) {
        // Picking's exact posed triangles and the zone/door meshes, tested conservatively from either side.
        // Click-through and passable flags do not make geometry disappear from the scene depth buffer.
        const auto hit = game::segment_triangle_distance(ray, t.v0, t.v1, t.v2);
        if (hit.distance_squared <= game::kPickHitDistanceSquared && hit.t >= 0.0f) nearest = std::min(nearest, hit.t);
    };
    if (scene.world) scene.world(ray, triangle);
    const glm::dvec3 precise_dir(ray.dir);
    const double length_squared = glm::dot(precise_dir, precise_dir);
    for (std::size_t i = 0; i < scene.actors.size(); ++i) {
        const auto& actor = scene.actors[i];
        if (actor.disabled || actor.invisible || static_cast<int>(i) == scene.exclude_a || static_cast<int>(i) == scene.exclude_b) continue;
        if (actor.triangles) {
            if (game::point_segment_distance_squared(ray, actor.bound_center) > actor.bound_radius * actor.bound_radius + 0.01f) continue;
            actor.triangles(triangle);
        } else if (actor.volume == 2 && actor.sphere_radius > 0.0f && length_squared > 0.0f) {
            const glm::dvec3 offset = glm::dvec3(ray.start) - glm::dvec3(actor.position);
            const double b = glm::dot(offset, precise_dir);
            const double c = glm::dot(offset, offset) - static_cast<double>(actor.sphere_radius) * actor.sphere_radius;
            const double discriminant = b * b - length_squared * c;
            if (discriminant < 0.0f) continue;
            const double far = (-b + std::sqrt(discriminant)) / length_squared;
            if (far < 0.0f) continue;
            const double near = std::max(0.0, (-b - std::sqrt(discriminant)) / length_squared);
            nearest = std::min(nearest, static_cast<float>(near));
        }
    }
    if (nearest >= 1.0f) return std::nullopt;
    return glm::dot(ray.dir * nearest, forward);
}

std::optional<game::PickSegment> nameplate_click_ray(const glm::mat4& inverse_scene, const glm::vec4& view,
                                                    const glm::vec3& camera, glm::vec2 at) {
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || view.z <= 0.0f || view.w <= 0.0f ||
        at.x < view.x || at.x >= view.x + view.z || at.y < view.y || at.y >= view.y + view.w) return std::nullopt;
    const glm::vec2 ndc((at.x - view.x) / view.z * 2.0f - 1.0f, 1.0f - (at.y - view.y) / view.w * 2.0f);
    const glm::vec4 far_clip = inverse_scene * glm::vec4(ndc, 0.0f, 1.0f);
    if (!std::isfinite(far_clip.w) || std::abs(far_clip.w) < 1.0e-9f) return std::nullopt;
    game::PickSegment ray{camera, glm::vec3(far_clip) / far_clip.w - camera};
    if (!std::isfinite(ray.dir.x) || !std::isfinite(ray.dir.y) || !std::isfinite(ray.dir.z) || glm::dot(ray.dir, ray.dir) <= 0.0f)
        return std::nullopt;
    return ray;
}

bool nameplate_depth_visible(float plate_depth, const std::optional<float>& scene_depth) {
    if (!std::isfinite(plate_depth) || plate_depth <= 0.0f) return false;
    if (!scene_depth) return true;
    return std::isfinite(*scene_depth) && *scene_depth >= 0.0f && plate_depth <= *scene_depth + 3.0f;
}


} // namespace hailstone::app
