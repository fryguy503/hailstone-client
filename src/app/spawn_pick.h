#pragma once

// One spawn's part in the two click tests, ClientApp::pick_at (the world click) and
// ClientApp::nameplate_pixel_depth (whether something in front hides a nameplate), without the actor renderer: it
// is reached through SpawnPickSource's callbacks.

#include <functional>
#include <vector>
#include <glm/glm.hpp>
#include "game/picking.h"

namespace hailstone::app {

struct SpawnPickSource {
    glm::vec3 position{0.0f}; // where the spawn stands (its display position)
    float size = 0.0f;        // the spawn's size; 0 or less: none
    // Its model is a body-less skeleton (assets::bodiless_skeleton, race 127's IVM): nothing of it is drawn but what
    // it holds.
    bool bodiless = false;
    // ActorRenderer::spawn_bounds: the model origin and radius it was last drawn with; false (or unset) when it was
    // not drawn with a model.
    std::function<bool(glm::vec3& origin, float& radius)> bounds;
    // Fills `actor` from the posed model (position, triangles, bones); false when no pose is available.
    std::function<bool(game::PickActor& actor, const glm::vec3& origin)> posed;
};

// The world click: appends `actor` (its mask and invisible flag set) for the spawn and returns true, or leaves it
// out (a posed model the segment misses by far, or one not posed). A spawn with a drawn model is its posed
// triangles; any other (a placeholder box, the player in first person, one past the actor clip) is the sphere
// (volume type 2), and so is a body-less skeleton, as it was before it had a model, whatever it holds.
// `excluded_view` (the player under PickCallInput's exclude_view) sets `exclude` to the actor's index.
bool add_world_click_spawn(std::vector<game::PickActor>& actors, int& exclude, game::PickActor actor,
                           const SpawnPickSource& source, const game::PickSegment& seg, bool excluded_view);

// The nameplate's scene depth: appends what of the spawn may hide a plate and returns true, or leaves it out. A
// drawn model is its posed triangles, or until it is posed a sphere of its radius; a spawn without one the
// placeholder box's sphere. A body-less skeleton only ever adds its posed items: bare, past the actor clip or not
// yet posed, it hides nothing.
bool add_nameplate_depth_spawn(std::vector<game::PickActor>& actors, const SpawnPickSource& source,
                               const game::PickSegment& ray);

} // namespace hailstone::app
