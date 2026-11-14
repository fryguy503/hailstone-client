#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

#include "assets/model.h"

namespace hailstone::render {

// Appended to the actor's existing SceneLighting buffer, leaving its push constants and texture descriptors intact.
// Slot zero is the old material path. Only EQG armor opts into these surface properties in the asset loader.
struct ActorSurface {
    glm::vec4 params{40.0f, 0.2f, 0.0f, 0.0f}; // shininess, gloss, environment strength, unused
    glm::uvec4 slots{}; // native normal, octahedral environment, flags, unused
};
static_assert(sizeof(ActorSurface) == 32);
static_assert(offsetof(ActorSurface, slots) == 16);

constexpr std::uint32_t kActorSurfaceSpecular = 1;
constexpr std::uint32_t kActorSurfaceReflective = 2;
constexpr std::uint32_t kMaxActorSurfaces = 65536;

inline bool has_actor_surface(const assets::EffectParams& effect) {
    return !effect.normal_map.empty() || effect.specular || effect.reflective;
}

inline ActorSurface actor_surface(const assets::EffectParams& effect, std::uint32_t normal,
                                   std::uint32_t environment) {
    const auto finite = [](float value, float fallback, float minimum, float maximum) {
        return std::clamp(std::isfinite(value) ? value : fallback, minimum, maximum);
    };
    ActorSurface surface;
    surface.params = {finite(effect.shininess, 40.0f, 1.0f, 4096.0f),
                      finite(effect.gloss, 0.2f, 0.0f, 16.0f),
                      finite(effect.env_strength, 0.0f, 0.0f, 16.0f), 0.0f};
    surface.slots = {normal, environment,
                     (effect.specular ? kActorSurfaceSpecular : 0u) |
                         (effect.reflective && environment != 0 && surface.params.z > 0.0f ? kActorSurfaceReflective : 0u),
                     0u};
    return surface;
}

// A complete model's table fits or uses the old material path. No partial model can index beyond the frame buffer.
inline std::optional<std::uint32_t> actor_surface_range(std::size_t used, std::size_t count) {
    if (used == 0 || used >= kMaxActorSurfaces || count == 0 || count > kMaxActorSurfaces - used) return std::nullopt;
    return static_cast<std::uint32_t>(used);
}

} // namespace hailstone::render
