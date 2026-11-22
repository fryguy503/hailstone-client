#include "game/light_list.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <string_view>

#include <glm/geometric.hpp>

#include "assets/model.h"
#include "assets/spell_effects.h"
#include "game/inventory.h"
#include "game/particles.h"
#include "game/world_state.h"
#include "net/item_packets.h"

namespace hailstone::game {

std::vector<LightSource> select_lights(std::span<const assets::PointLight> zone_lights,
                                       std::span<const LightSource> dynamic, const glm::vec3& center, float range,
                                       std::size_t max) {
    auto by_distance = [&](std::vector<std::pair<float, LightSource>>& v) {
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    };
    std::vector<std::pair<float, LightSource>> near_dynamic, near_zone;
    for (const auto& l : dynamic)
        if (l.radius > 0.0f) near_dynamic.emplace_back(glm::distance(l.position, center), l);
    for (const auto& l : zone_lights) {
        if (!(l.radius > 0.0f)) continue;
        // How far the light's sphere is from the centre (0 inside it).
        float gap = std::max(0.0f, glm::distance(l.position, center) - l.radius);
        if (gap > range) continue;
        near_zone.emplace_back(gap, LightSource{l.position, l.color, l.radius});
    }
    by_distance(near_dynamic);
    by_distance(near_zone);
    std::vector<LightSource> out;
    for (auto& [d, l] : near_dynamic)
        if (out.size() < max) out.push_back(l);
    for (auto& [d, l] : near_zone)
        if (out.size() < max) out.push_back(l);
    return out;
}

std::optional<LightSource> scaled_carried_light(const LightSource& light, float strength, float reach) {
    strength = std::clamp(strength, 0.0f, kMaxCarriedLight);
    if (!(strength > 0.0f)) return std::nullopt;
    LightSource out = light;
    out.color *= strength;
    out.radius *= std::clamp(reach, kMinCarriedReach, kMaxCarriedReach);
    return out;
}

std::uint8_t carried_light_value(const Inventory& inventory) {
    std::uint8_t best = 0;
    for (std::int16_t s = 0; s <= net::kSlotGeneral10; ++s)
        if (const net::Item* it = inventory.at(net::ItemSlot::possessions(s))) best = std::max(best, it->light);
    return best;
}

std::optional<LightSource> carried_light(std::uint8_t light_value, const glm::vec3& position) {
    // EQ's light values, dimmest to brightest: 1 candle, 2 torch, 3 tiny glowing skull, 4 small lantern, 5 stein of
    // moggok, 6 large lantern, 7 flameless lantern, 8 globe of stars, 9 light globe, 10 lightstone, 11 greater
    // lightstone, 12 fire beetle eye, 13 coldlight, 14/15 the brightest.
    struct Look {
        glm::vec3 color;
        float radius;
    };
    static constexpr Look kLooks[] = {
        {{0.00f, 0.00f, 0.00f}, 0.0f},   // 0: none
        {{0.75f, 0.55f, 0.30f}, 45.0f},  // candle
        {{1.00f, 0.65f, 0.35f}, 65.0f},  // torch
        {{0.55f, 0.85f, 0.45f}, 55.0f},  // tiny glowing skull
        {{1.00f, 0.75f, 0.45f}, 70.0f},  // small lantern
        {{1.00f, 0.80f, 0.50f}, 75.0f},  // stein of moggok
        {{1.00f, 0.80f, 0.55f}, 85.0f},  // large lantern
        {{0.90f, 0.90f, 0.80f}, 85.0f},  // flameless lantern
        {{0.75f, 0.80f, 1.00f}, 90.0f},  // globe of stars
        {{0.90f, 0.90f, 1.00f}, 95.0f},  // light globe
        {{0.95f, 0.95f, 1.00f}, 100.0f}, // lightstone
        {{1.00f, 1.00f, 1.00f}, 115.0f}, // greater lightstone
        {{1.00f, 0.55f, 0.35f}, 80.0f},  // fire beetle eye
        {{0.80f, 0.90f, 1.10f}, 110.0f}, // coldlight
        {{1.05f, 1.05f, 1.05f}, 125.0f},
        {{1.10f, 1.10f, 1.10f}, 135.0f},
    };
    if (light_value == 0 || light_value >= std::size(kLooks)) return std::nullopt;
    return LightSource{position, kLooks[light_value].color, kLooks[light_value].radius};
}

std::uint8_t carrier_light_value(const Spawn& spawn, std::uint32_t player_id, const Inventory& inventory) {
    return spawn.id == player_id ? carried_light_value(inventory) : spawn.light_type;
}

std::optional<LightSource> spawn_carried_light(const Spawn& spawn) {
    const auto position = spawn.display_valid ? spawn.display_position : spawn.position;
    return carried_light(spawn.light_type, position + glm::vec3(0.0f, 0.0f, 4.0f));
}

LightSource char_select_key_light(const glm::vec3& feet, float height, const glm::vec3& camera) {
    height = std::max(height, 1.0f);
    const glm::vec3 chest = feet + glm::vec3(0.0f, 0.0f, height * 0.6f);
    glm::vec3 to_camera(camera.x - chest.x, camera.y - chest.y, 0.0f);
    float flat = glm::length(to_camera);
    to_camera = flat > 1e-3f ? to_camera / flat : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 left(-to_camera.y, to_camera.x, 0.0f); // the camera's left, seen from the model
    const float reach = std::max(flat, 2.0f * height);
    LightSource l;
    l.position = chest + to_camera * (reach * 0.7f) + left * (reach * 0.35f) +
                 glm::vec3(0.0f, 0.0f, height * 0.5f + reach * 0.25f);
    // A little warm against the daylight; twice the white a carried light gives, as its falloff (light_pool) is
    // steep over the camera distance (tuned in clz at noon).
    l.color = glm::vec3(1.9f, 1.8f, 1.64f);
    l.radius = std::max(glm::length(l.position - chest) * 2.5f, 40.0f);
    return l;
}

bool particle_glows(const assets::EmitterDefinition& definition) {
    // Find threads are emissive guide lines; pooling all their colours would wash a broad patch of ground white.
    if (definition.billboard == kTrailStrandBillboard) return false;
    std::string name = definition.texture;
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto any = [&](std::initializer_list<std::string_view> words) {
        return std::any_of(words.begin(), words.end(), [&](std::string_view w) { return name.find(w) != std::string::npos; });
    };
    if (definition.additive) return !any({"smoke", "dust", "rock", "cloud", "water", "dark"});
    return any({"flame", "fire", "flare", "explosion", "electric", "zap", "spark", "glow", "burst", "star", "corona",
                "globe", "glitter", "pulse", "lightn", "ember", "lava"});
}

std::vector<LightSource> particle_lights(const ParticleSystem& particles,
                                         const std::function<glm::vec3(const std::string& texture)>& texture_color,
                                         std::size_t max) {
    struct Glow {
        glm::vec3 position{0.0f};
        glm::vec3 color{0.0f};
        float weight = 0.0f; // the particles' opacity, summed
        float spread = 0.0f; // their RMS distance from the centre
    };
    std::vector<Glow> glows;
    for (const Emitter& e : particles.emitters()) {
        if (e.particles.empty() || !particles.glows(e.definition)) continue;
        const auto& d = particles.definitions()[e.definition];
        Glow g;
        // Each particle by its opacity and size: a cloud of sparks lights less than a sheet of flame.
        auto weight = [](const Particle& p) {
            float size = std::sqrt(std::max(p.width * p.height, 0.0f)) * p.size_factor;
            return p.color.a * std::clamp(size / 3.0f, 0.1f, 1.0f);
        };
        for (const Particle& p : e.particles) {
            float w = weight(p);
            g.position += p.position * w;
            g.color += glm::vec3(p.color) * w;
            g.weight += w;
        }
        if (g.weight < 0.05f) continue;
        g.position /= g.weight;
        g.color /= g.weight;
        float d2 = 0.0f;
        for (const Particle& p : e.particles) {
            glm::vec3 v = p.position - g.position;
            d2 += glm::dot(v, v) * weight(p);
        }
        g.spread = std::sqrt(d2 / g.weight);
        g.color *= texture_color ? texture_color(d.texture) : glm::vec3(1.0f);
        // Merged into a glow near it: the weighted mean of both.
        auto near = std::find_if(glows.begin(), glows.end(), [&](const Glow& o) {
            return glm::distance(o.position, g.position) < 12.0f;
        });
        if (near == glows.end()) {
            glows.push_back(g);
            continue;
        }
        float w = near->weight + g.weight;
        near->position = (near->position * near->weight + g.position * g.weight) / w;
        near->color = (near->color * near->weight + g.color * g.weight) / w;
        near->spread = std::max(near->spread, g.spread);
        near->weight = w;
    }
    std::vector<LightSource> out;
    for (const Glow& g : glows) {
        float strength = std::min(g.weight / 6.0f, 1.0f);
        out.push_back({g.position, g.color * (1.2f * strength), std::clamp(40.0f + g.spread * 2.5f, 40.0f, 100.0f)});
    }
    auto bright = [](const LightSource& l) { return glm::dot(l.color, glm::vec3(0.2126f, 0.7152f, 0.0722f)); };
    std::sort(out.begin(), out.end(), [&](const LightSource& a, const LightSource& b) { return bright(a) > bright(b); });
    if (out.size() > max) out.resize(max);
    return out;
}

} // namespace hailstone::game
