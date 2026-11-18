#include "render/particle_textures.h"

#include <algorithm>

#include "assets/texture_overrides.h"
#include "core/game_dir.h"
#include "game/particles.h"

namespace hailstone::render {

ParticleTextureFrame particle_texture_frame(const assets::EmitterDefinition& definition, std::uint32_t frame) {
    if (definition.texture_frames.empty())
        return {definition.texture, game::flipbook_grid(definition.frames), frame};
    return {definition.texture_frames[frame % definition.texture_frames.size()], glm::uvec2(1), 0};
}

LoadedParticleTexture load_particle_texture(const std::string& name, std::optional<std::vector<std::byte>> zone_bytes,
                                            const GameDir& game) {
    LoadedParticleTexture out;
    std::optional<std::vector<std::byte>> data = std::move(zone_bytes);
    for (const char* dir : {"SpellEffects/", "EnvEmitterEffects/", "ActorEffects/"}) {
        if (data) break;
        data = game.read(std::string(dir) + name);
    }
    if (!data) return out;
    out.image = assets::decode_game_texture(name, data);
    if (!out.image) return out;
    // Its colour: the mean of rgb x alpha, scaled so the brightest channel is 1.
    assets::Image rgba = assets::to_rgba8(*out.image);
    glm::dvec3 sum(0.0);
    const auto& px = rgba.mips[0];
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        double a = std::to_integer<int>(px[i + 3]) / 255.0;
        sum += glm::dvec3(std::to_integer<int>(px[i]), std::to_integer<int>(px[i + 1]), std::to_integer<int>(px[i + 2])) *
               a;
    }
    double top = std::max({sum.r, sum.g, sum.b});
    out.color = top > 0.0 ? glm::vec3(sum / top) : glm::vec3(1.0f);
    return out;
}

std::uint32_t particle_upload_slot(bool decoded, std::size_t textures, std::uint32_t max_textures) {
    if (!decoded || textures >= max_textures) return kMissingParticleTexture;
    return static_cast<std::uint32_t>(textures);
}

std::optional<std::uint32_t> particle_texture_slot(const std::string& name,
                                                   const std::unordered_map<std::string, std::uint32_t>& slots,
                                                   const std::unordered_set<std::string>& loading) {
    if (name.empty() || loading.contains(name)) return std::nullopt;
    auto found = slots.find(name);
    if (found == slots.end() || found->second == kMissingParticleTexture) return std::nullopt;
    return found->second;
}

} // namespace hailstone::render
