#include "assets/flora.h"

#include <algorithm>
#include <cmath>

namespace hailstone::assets {

namespace {

constexpr float kRandScale = 3.051851e-05f; // 1 / 32767, the DLL's

float unit(MsvcRand& rand) { return static_cast<float>(rand()) * kRandScale; }

} // namespace

std::uint32_t flora_tile_seed(std::int32_t lng, std::int32_t lat, std::size_t ecosystem) {
    return static_cast<std::uint32_t>(lng) * 73856093u ^ static_cast<std::uint32_t>(lat) * 19349663u ^
           static_cast<std::uint32_t>(ecosystem * 83492791u);
}

void place_flora(const FloraTile& tile, std::span<const eqg::EcoFloraLayer> layers,
                 std::span<const eqg::FloraDefinition> definitions, MsvcRand& rand, std::vector<FloraInstance>& out,
                 std::size_t zone_cap) {
    const std::uint32_t S = tile.texels;
    if (out.size() >= zone_cap) return;
    if (S == 0 || tile.heights.size() < std::size_t{S} * S || tile.slopes.size() < std::size_t{S} * S) return;
    // A DAT mask, when there is one, and each layer's mask (below) cover every texel, or mask_at reads past them.
    if (!tile.dat_mask.empty() && tile.dat_mask.size() < std::size_t{S} * S) return;
    // The mask a layer grows on at texel i: its texture layer's, within the ecosystem's share of the tile.
    auto mask_at = [&](std::size_t k, std::size_t i) -> std::uint32_t {
        std::uint32_t m = tile.layer_masks[k][i];
        if (!tile.dat_mask.empty()) m = m * tile.dat_mask[i] / 255;
        return m;
    };
    const std::size_t first = out.size();
    for (const auto& layer : layers) {
        auto def = std::find_if(definitions.begin(), definitions.end(),
                                [&](const eqg::FloraDefinition& d) { return d.name == layer.flora; });
        if (def == definitions.end() || layer.density <= 0.0f || layer.iterations <= 0) continue;
        std::size_t k = 0;
        while (k < tile.layer_names.size() && tile.layer_names[k] != layer.texture_layer) ++k;
        if (k >= tile.layer_names.size() || k >= tile.layer_masks.size()) continue;
        if (tile.layer_masks[k].size() < std::size_t{S} * S) continue;
        // The share of the tile the texture layer covers (0x100f65b0), times the density.
        std::size_t covered = 0;
        for (std::size_t i = 0; i < std::size_t{S} * S; ++i)
            if (mask_at(k, i) >= layer.min_alpha) ++covered;
        const float target = static_cast<float>(covered) / static_cast<float>(S * S) * layer.density;
        if (!(target > 0.0f)) continue;
        // Bounded here too, for layers not made by parse_eco_flora (so the float-to-int casts below stay defined).
        const std::int32_t iterations = std::min(layer.iterations, eqg::kMaxFloraIterations);
        const float bounded = std::min(target, eqg::kMaxFloraDensity);
        std::int32_t wanted = 0;
        if (bounded < 1.0f) {
            // Below one: one, by chance.
            wanted = rand() % 10000 <= static_cast<std::int32_t>(bounded * 10000.0f) ? 1 : 0;
        } else {
            wanted = static_cast<std::int32_t>(bounded);
            const float rest = bounded - static_cast<float>(wanted);
            if (rest >= 0.01f && rand() % 10000 <= static_cast<std::int32_t>(rest * 10000.0f)) ++wanted;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(def - definitions.begin());
        std::int32_t placed = 0;
        for (std::int32_t attempt = 0; attempt < iterations && placed < wanted; ++attempt) {
            if (out.size() - first >= kMaxFloraPerTile || out.size() >= zone_cap) return;
            const float u = unit(rand), v = unit(rand);
            const float width = def->min_width + unit(rand) * (def->max_width - def->min_width);
            const float height = def->min_height + unit(rand) * (def->max_height - def->min_height);
            const auto tx = std::min(static_cast<std::uint32_t>(u * static_cast<float>(S)), S - 1);
            const auto ty = std::min(static_cast<std::uint32_t>(v * static_cast<float>(S)), S - 1);
            const std::size_t i = std::size_t{ty} * S + tx;
            if (mask_at(k, i) < layer.min_alpha) continue;
            const float h = tile.height_at ? tile.height_at(u, v) : tile.heights[i];
            if (h > layer.max_height + layer.height_tolerance || h < layer.min_height - layer.height_tolerance) continue;
            // In 64 bits: a damaged file's bands near int32's limits would overflow the sums.
            const std::int64_t slope = tile.slopes[i], tol = layer.slope_tolerance;
            if (slope < layer.min_slope - tol || slope > layer.max_slope + tol) continue;
            const float scale = (layer.max_scale - layer.min_scale) * unit(rand) + layer.min_scale;
            FloraInstance f;
            f.position = glm::vec3(tile.origin + glm::vec2(u, v) * tile.tile_size, h);
            f.rotation = static_cast<float>(rand() % 360);
            f.width = width * scale;
            f.height = height * scale;
            f.color = tile.color_at ? tile.color_at(u, v) : 0xffffffffu;
            f.definition = index;
            out.push_back(f);
            ++placed;
        }
    }
}

} // namespace hailstone::assets
