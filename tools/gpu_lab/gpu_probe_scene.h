#pragma once

// Diagnostic static-world payloads. They contain the player's decoded assets only in memory, never in reports.

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "assets/model.h"
#include "assets/texture.h"
#include "core/sha256.h"

namespace hailstone::assets {
struct Zone;

struct GpuProbeVertex {
    glm::vec3 position{0.0f}; // authored Z-up world coordinates
    glm::vec2 uv{0.0f};
    std::uint32_t color = 0xffffffff;
};
static_assert(sizeof(GpuProbeVertex) == 24);

struct GpuProbeTexture {
    Image image;
    Sha256Digest digest{};
    std::uint64_t upload_bytes = 0;
};

struct GpuProbeDraw {
    std::uint32_t first_index = 0, index_count = 0, texture = 0;
    // Opacity is inventory metadata; the shader applies alpha_cutoff but does not reproduce blend/effect passes.
    float opacity = 1.0f, alpha_cutoff = 0.0f;
};

struct GpuProbeInventory {
    std::uint64_t terrain_meshes = 0, placed_objects = 0, missing_objects = 0;
    std::uint64_t missing_textures = 0, material_effect_draws = 0, translucent_draws = 0, masked_draws = 0;
    std::uint64_t omitted_draws = 0, animated_materials = 0;
    std::uint64_t vertex_bytes = 0, index_bytes = 0, texture_bytes = 0, unique_texture_bytes = 0;
    std::uint64_t unique_texture_contents = 0;
};

struct GpuProbeScene {
    std::vector<GpuProbeVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<GpuProbeTexture> textures;
    std::vector<GpuProbeDraw> draws;
    glm::vec3 bounds_min{0.0f}, bounds_max{0.0f};
    Sha256Digest geometry_digest{}, scene_digest{};
    GpuProbeInventory inventory;
};

struct GpuProbeMaterialPolicy {
    bool draw = true, masked = false, reads_alpha = false;
    float opacity = 1.0f, alpha_cutoff = 0.0f;
};

// The tool supplies the existing renderer's material/mip preparation, without an assets -> render dependency.
struct GpuProbeScenePreparation {
    std::function<GpuProbeMaterialPolicy(const Material&)> material;
    std::function<Image(Image, const GpuProbeMaterialPolicy&)> image;
};

Sha256Digest gpu_probe_texture_digest(const Image& image);
// Preserves native formats and complete mip payloads. Rejects malformed geometry/image ranges before GPU use.
GpuProbeScene build_gpu_probe_scene(const Zone& zone, const GpuProbeScenePreparation& preparation);
} // namespace hailstone::assets
