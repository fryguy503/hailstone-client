#pragma once

// Collision with the zone objects that play their own animation (assets/object_animation.h): Ssraeshza Temple's
// turning rings, swaying branches. The original tests a placed hierarchical actor without a static collision mesh (no
// WLD skeleton has one) by its parts' meshes where the animation has put them (CActorData::FindIntersectionSphere's
// polygon path, docs/mechanics.md "Animated zone objects"), so a ring's gap lets the player through only while it
// faces the way in.
//
// The triangles that move are left out of the static zone mesh (CollisionMesh::build) and kept here, one shape per
// moving node of each animated object (game/object_collision.h), in the object's first-key pose. set_time poses every
// object once per definition (all placements of a definition share one controller); a query then takes the parts
// near it from a grid (each part stays within its reach of its placement's origin) and tests each through placement x
// its node's motion, as a door is through its transform. A triangle whose corners follow different nodes goes with the
// node of its first moving corner. Some zones have tens of thousands of such parts (Emerald Jungle's trees).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/object_animation.h"
#include "game/object_collision.h"
#include "game/swept_sphere.h"

namespace hailstone::assets { struct Zone; struct Mesh; }

namespace hailstone::game {

using assets::moving_triangle_node;

class AnimatedObjects {
public:
    AnimatedObjects() = default;
    // Every placement of an animated object, its moving parts posed at time 0.
    static AnimatedObjects build(const assets::Zone& zone);

    bool empty() const { return parts_.empty(); }
    std::size_t part_count() const { return parts_.size(); }
    std::size_t definition_count() const { return definitions_.size(); }

    // Poses everything `ms` milliseconds into the zone's animation clock.
    void set_time(std::uint64_t ms);
    std::uint64_t time() const { return time_ms_; }
    // The renderer's current clock can be between physics steps. Visual queries use this separately posed mesh;
    // collision and part_transform keep the physics pose unchanged.
    void set_visual_time(std::uint64_t ms);
    std::uint64_t visual_time() const { return visual_time_ms_; }

    // The movement sweep against the moving parts (CActorData::FindIntersectionSphere's polygon path): a hit at
    // t <= the sweep's t sets t, collided and the normal. True on a hit.
    bool sweep_sphere(SphereSweep& sweep) const;
    // The highest moving surface under (x, y) no higher than p.z + step_up, as CollisionMesh::floor_below.
    std::optional<float> floor_below(const glm::vec3& p, float step_up) const;
    // Render effects: current upward drawn opaque/masked parts, including passable decorative parts.
    std::optional<float> visual_surface_below(const glm::vec3& p) const;
    bool visual_surfaces(const glm::vec2& low,const glm::vec2& high,
                         const std::function<bool(const CollisionTriangle&)>& visit,std::uint32_t& work) const;
    // True when a moving part blocks the segment a -> b.
    bool blocks(const glm::vec3& a, const glm::vec3& b) const;

    // For tests and tools: a part's current object-to-world transform.
    glm::mat4 part_transform(std::size_t i) const;

private:
    struct Definition {
        assets::ObjectAnimation animation;
        std::vector<std::uint16_t> nodes;  // the moving nodes with triangles, one per shape
        std::vector<ObjectShape> shapes;   // their triangles, first-key pose, object space
        std::vector<float> reach;          // per shape: the farthest its triangles get from the object's origin
        std::vector<glm::mat4> pose;       // the last set_time's motion per node (ObjectAnimation::pose)
        std::vector<glm::mat4> pose_inverse;
        std::vector<glm::mat4> visual_pose;
    };
    struct Part {
        std::uint32_t definition = 0, shape = 0;
        glm::mat4 placement{1.0f}, placement_inverse{1.0f};
        float scale = 1.0f;   // the placement's (the tracks keep the scale 1 that every shipped key has)
        glm::vec3 centre{0.0f}; // the placement's origin: the part stays within reach x scale of it
        float reach = 0.0f;     // in world units
    };
    // The grid of parts by the cells their reach covers (kCell units), and the parts near a box.
    static constexpr float kCell = 64.0f;
    static std::int64_t cell_key(std::int32_t x, std::int32_t y) {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(y);
    }
    void near(const glm::vec3& lo, const glm::vec3& hi, std::vector<std::uint32_t>& out) const;
    // A part's transform now and its inverse.
    void placed(const Part& part, glm::mat4& transform, glm::mat4& inverse) const;
    glm::mat4 placed_visual(const Part& part) const;

    std::vector<Definition> definitions_;
    std::vector<Part> parts_;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> grid_;
    std::vector<std::uint32_t> big_; // parts whose reach spans too many cells: in every query
    std::uint64_t time_ms_ = 0;
    std::uint64_t visual_time_ms_ = 0;
};

} // namespace hailstone::game
