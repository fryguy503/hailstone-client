#include "app/spawn_pick.h"

#include <algorithm>

namespace hailstone::app {

namespace {

// Hailstone's long-standing spawn picking sizes (not checked against the original): a spawn without a size is taken
// as 6 units; its sphere is half that for a click and 0.9 of it for the nameplate depth (a little wider, so a plate
// is not seen through the edge of a placeholder box), never under 1.5. A drawn model is posed for a click only when
// the segment passes within radius x 1.5 + 2 of it (radius + 2 for the nameplate depth).
constexpr float kDefaultSpawnSize = 6.0f;
constexpr float kClickSphereSize = 0.5f;
constexpr float kPlateSphereSize = 0.9f;
constexpr float kMinSphereRadius = 1.5f;
constexpr float kClickReachFactor = 1.5f;
constexpr float kReachSlack = 2.0f;
constexpr std::uint8_t kSphereVolume = 2; // the collision-volume type (+0x84) of the old sphere actor

float sphere_radius(float size, float factor) {
    return std::max(kMinSphereRadius, (size > 0.0f ? size : kDefaultSpawnSize) * factor);
}

bool drawn_bounds(const SpawnPickSource& s, glm::vec3& origin, float& radius) {
    return s.bounds && s.bounds(origin, radius);
}

bool posed(const SpawnPickSource& s, game::PickActor& actor, const glm::vec3& origin) {
    return s.posed && s.posed(actor, origin);
}

} // namespace

bool add_world_click_spawn(std::vector<game::PickActor>& actors, int& exclude, game::PickActor actor,
                           const SpawnPickSource& source, const game::PickSegment& seg, bool excluded_view) {
    glm::vec3 origin{0.0f};
    float radius = 0.0f;
    if (!source.bodiless && drawn_bounds(source, origin, radius)) {
        const float reach = radius * kClickReachFactor + kReachSlack;
        if (game::point_segment_distance_squared(seg, origin) > reach * reach) return false;
        if (!posed(source, actor, origin)) return false;
    } else {
        actor.volume = kSphereVolume;
        actor.position = source.position;
        actor.sphere_radius = sphere_radius(source.size, kClickSphereSize);
    }
    if (excluded_view) exclude = static_cast<int>(actors.size());
    actors.push_back(std::move(actor));
    return true;
}

bool add_nameplate_depth_spawn(std::vector<game::PickActor>& actors, const SpawnPickSource& source,
                               const game::PickSegment& ray) {
    glm::vec3 origin{0.0f};
    float radius = 0.0f;
    const bool have_bounds = drawn_bounds(source, origin, radius);
    if (have_bounds && game::point_segment_distance_squared(ray, origin) > (radius + kReachSlack) * (radius + kReachSlack))
        return false;
    game::PickActor actor;
    if (!(have_bounds && posed(source, actor, origin))) {
        if (source.bodiless) return false;
        // A not-yet-available posed mesh is conservatively bounded, never treated as transparent to clicks.
        actor = game::PickActor{};
        actor.volume = kSphereVolume;
        actor.position = have_bounds ? origin : source.position;
        actor.sphere_radius = have_bounds ? radius : sphere_radius(source.size, kPlateSphereSize);
    }
    actors.push_back(std::move(actor));
    return true;
}

} // namespace hailstone::app
