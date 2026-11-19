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
// Slot zero is the old material path. EQG armor and held models opt into these surface properties in the asset loader.
struct ActorSurface {
    glm::vec4 params{40.0f, 0.2f, 0.0f, 0.0f}; // shininess, gloss, environment strength, coverage UV scale
    glm::uvec4 slots{}; // native normal, octahedral environment, flags, coverage/second diffuse/banner cloth tint
    glm::vec4 slide{0.0f}; // authored colour and alpha UV velocities
};
static_assert(sizeof(ActorSurface) == 48);
static_assert(offsetof(ActorSurface, slots) == 16);
static_assert(offsetof(ActorSurface, slide) == 32);

constexpr std::uint32_t kActorSurfaceSpecular = 1;
constexpr std::uint32_t kActorSurfaceReflective = 2;
constexpr std::uint32_t kActorSurfacePackedNormal = 4;
constexpr std::uint32_t kActorSurfaceGlow = 8;
constexpr std::uint32_t kActorSurfaceCoverage = 16;
constexpr std::uint32_t kActorSurfaceWaterFall = 32;
constexpr std::uint32_t kActorSurfaceLava = 64;
constexpr std::uint32_t kActorSurfaceBanner = 128;

// A part drawn with the lava surface: Lava, and Lava2 as the zone draws it (zone_renderer.cpp: RegionLava2's own
// formula is not settled). Both read e_TextureDiffuse1 and e_TextureNormal0 as their second layer and normal.
inline bool actor_lava_surface(assets::EqgShader shader) {
    return shader == assets::EqgShader::Lava || shader == assets::EqgShader::Lava2;
}
// Whether a part takes its material's MPL coverage map: the coverage map and lava's second layer share slots.w, and
// lava keeps its layer.
inline bool actor_takes_coverage(const assets::Material& m) {
    return !m.effect.coverage_map.empty() && !actor_lava_surface(m.eqg_shader);
}
// Table entries per frame in flight (48 bytes each, host-visible: 192 KiB). A drawn look with any surface takes one
// entry per part, so this holds dozens of distinct armored looks a frame; past it a look keeps plain lighting.
constexpr std::uint32_t kMaxActorSurfaces = 4096;
// camera_pos.w carries this exact integer as a float: a 12-bit surface index and independent material flags.
// Material lighting must still work when the optional armor surface table is full.
constexpr std::uint32_t kActorMaterialUnlit = kMaxActorSurfaces;
constexpr std::uint32_t kActorMaterialNoFog = kMaxActorSurfaces * 2;
inline float actor_surface_token(std::uint32_t index, bool lit, bool fog) {
    return static_cast<float>(index | (lit ? 0u : kActorMaterialUnlit) | (fog ? 0u : kActorMaterialNoFog));
}

inline bool has_actor_surface(const assets::EffectParams& effect) {
    return !effect.normal_map.empty() || !effect.coverage_map.empty() || effect.specular || effect.reflective;
}

inline ActorSurface actor_surface(const assets::EffectParams& effect, std::uint32_t normal,
                                   std::uint32_t environment) {
    const auto finite = [](float value, float fallback, float minimum, float maximum) {
        return std::clamp(std::isfinite(value) ? value : fallback, minimum, maximum);
    };
    ActorSurface surface;
    surface.params = {finite(effect.shininess, 40.0f, 1.0f, 4096.0f),
                      finite(effect.gloss, 0.2f, 0.0f, 16.0f),
                      finite(effect.env_strength, 0.0f, 0.0f, 16.0f),
                      effect.coverage_map.empty() ? 0.0f : finite(effect.coverage_scale, 1.0f, 0.0f, 65536.0f)};
    for (int i = 0; i < 4; ++i)
        surface.slide[i] = std::isfinite(effect.slide[i]) ? effect.slide[i] : 0.0f;
    surface.slots = {normal, environment,
                     (effect.specular ? kActorSurfaceSpecular : 0u) |
                         (effect.reflective && environment != 0 && surface.params.z > 0.0f ? kActorSurfaceReflective : 0u) |
                         (normal != 0 && effect.packed_normal ? kActorSurfacePackedNormal | kActorSurfaceSpecular : 0u) |
                         (normal != 0 && effect.glow ? kActorSurfaceGlow : 0u),
                     0u};
    return surface;
}

// A complete model's table fits or uses the old material path. No partial model can index beyond the frame buffer.
inline std::optional<std::uint32_t> actor_surface_range(std::size_t used, std::size_t count) {
    if (used == 0 || used >= kMaxActorSurfaces || count == 0 || count > kMaxActorSurfaces - used) return std::nullopt;
    return static_cast<std::uint32_t>(used);
}

} // namespace hailstone::render
