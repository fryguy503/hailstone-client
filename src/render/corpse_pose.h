#pragma once

// The corpse branch of ActorRenderer::prepare_spawns, kept apart from Vulkan so the rule is tested: whether a
// corpse's death clip starts at its end, and the request that plays it.

#include <cstdint>

#include "assets/character.h"
#include "game/animation_types.h"
#include "game/spawn_animation.h" // game::kBaseStyleColumn
#include "game/spell_book.h"      // game::kStandDead
#include "render/actor_layers.h"

namespace hailstone::render {

// Dead as the renderer draws it: a corpse, or a spawn lying down (stand state 115, feign death).
constexpr bool spawn_dead(bool is_corpse, std::uint8_t stand_state) {
    return is_corpse || stand_state == game::kStandDead;
}

// Whether the spawn was drawn alive last frame: a hidden spawn (its state kept, not drawn) was not.
constexpr bool drawn_alive_last_frame(bool drawn_alive, bool hidden) {
    return drawn_alive && !hidden;
}

// A corpse shows its death clip's end at once unless it was drawn alive last frame: a visible living -> dead
// transition plays the fall, even when the model changes in the same frame. A fresh state (zone-in, coming into
// view, or back from hiding) has no such transition.
constexpr bool corpse_starts_at_end(bool fresh, bool was_alive, bool dead) {
    return fresh && dead && !was_alive;
}

// The request that plays a corpse's death clip (D05, D04, else P01) on both groups, once, at its end when the spawn
// was already dead when first drawn.
inline void corpse_request(LayerRequest& req, const assets::AnimationClip& death, bool at_end) {
    req.base = &death;
    req.base_anim = static_cast<std::uint8_t>(AnimationType::Death);
    req.loop = false;
    req.corpse = true;
    // The death clip is the base style's (classic_aliases), not the movement choice's column: a changed weapon
    // style must not restart it.
    req.base_column = game::kBaseStyleColumn;
    req.corpse_at_end = at_end;
}

} // namespace hailstone::render
