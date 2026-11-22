#pragma once

// The point lights the modern lighting mode (render/scene_lighting.h) gives the shaders each frame: the light the
// player carries and the glowing spell effects, then the zone's own lights nearest the camera. None of this is the original client's: it draws
// characters and objects with the scene's ambient only. The classic mode does not use it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

namespace hailstone::assets {
struct EmitterDefinition;
struct PointLight;
} // namespace hailstone::assets
namespace hailstone::game {
class Inventory;
class ParticleSystem;
struct Spawn;
} // namespace hailstone::game

namespace hailstone::game {

struct LightSource {
    glm::vec3 position{0.0f}; // world coordinates (game/coords.h)
    glm::vec3 color{1.0f};    // 0..1 per channel, can exceed 1 for bright sources
    float radius = 0.0f;      // no light at or beyond it
};

// The lights to draw with: `dynamic` first (nearest to `center` first), then the zone's lights whose sphere reaches
// within `range` of `center`, nearest first; at most `max` in all. Lights with no radius are skipped.
std::vector<LightSource> select_lights(std::span<const assets::PointLight> zone_lights,
                                       std::span<const LightSource> dynamic, const glm::vec3& center, float range,
                                       std::size_t max);

// The brightest light value (the item record's "light", 1 candle .. 15) among the items the player wears or carries
// in the general slots; 0 none.
std::uint8_t carried_light_value(const Inventory& inventory);

// The light a carried light source gives off at `position` (a torch's warm orange, a lightstone's white ...); nullopt
// for 0 or an unknown value. The colours and radii are this client's choice for the modern mode.
std::optional<LightSource> carried_light(std::uint8_t light_value, const glm::vec3& position);
// The light value a spawn carries. The local player's (`player_id`) is its own inventory's (carried_light_value), as
// before: EQEmu sets a client's spawn light only when it builds the spawn packet (Client::FillSpawnStruct), ignores the
// client's own SpawnAppearance 5 (Handle_OP_SpawnAppearance) and sends AppearanceType::Light only for NPCs, corpses,
// bots and mercenaries, so the record would not follow the player's equipment. Every other spawn uses its record's
// light, changed by SpawnAppearance 5.
std::uint8_t carrier_light_value(const Spawn& spawn, std::uint32_t player_id, const Inventory& inventory);
// Server-selected active light at the carrier's displayed position, using
// the same modern palette and chest offset as the local player's light.
std::optional<LightSource> spawn_carried_light(const Spawn& spawn);

// Hailstone's Carried Light controls (display.carried_light, display.carried_light_reach; 1 as above): a carried
// light's colour x `strength` (0..kMaxCarriedLight) and its radius x `reach` (kMinCarriedReach..kMaxCarriedReach), for
// the player's light and every other carrier's alike; nullopt at strength 0 (no light at all, not a black one).
inline constexpr float kMaxCarriedLight = 3.0f;
inline constexpr float kMinCarriedReach = 0.25f;
inline constexpr float kMaxCarriedReach = 2.0f;
std::optional<LightSource> scaled_carried_light(const LightSource& light, float strength, float reach);

// Character select and creation (modern lighting): a soft key light on the model from the camera's side, above and to
// its left, so the character reads in clz's tree shade. `feet` is the model's position, `height` its height and
// `camera` the camera's position; the light reaches past the model whatever the race's camera distance. Not the
// original's: it lights the model with the zone's sun alone.
LightSource char_select_key_light(const glm::vec3& feet, float height, const glm::vec3& camera);

// Whether an emitter's particles give off light: the additive ones but smoke, dust, rock, cloud, water and darkness;
// and the alpha-blended fire and energy (flame, fire, flare, explosion, electric, spark, glow ... by the texture's
// name).
bool particle_glows(const assets::EmitterDefinition& definition);

// The light the glowing spell effects give off: each glowing emitter's live particles as one light at their centre
// (weighted by their opacity and size, sparks under 3 units counting less), coloured by their tint times `texture_color` (the emitter texture's own colour, from
// the renderer; white when unknown), as bright as there are particles to see (full at 6 particles' weight), reaching
// 40 to 100 units as they spread. Emitters within 12 units of each other merge (an effect is several emitters). The
// `max` brightest. Not the original's (its spells light nothing).
std::vector<LightSource> particle_lights(const ParticleSystem& particles,
                                         const std::function<glm::vec3(const std::string& texture)>& texture_color,
                                         std::size_t max);

} // namespace hailstone::game
