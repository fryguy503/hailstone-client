#include "render/horizon.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "assets/texture.h"
#include "render/atmosphere.h"

namespace hailstone::render {

namespace {

// The value at which `share` of the total weight lies below, of (value, weight) pairs (sorted here).
float weighted_percentile(std::vector<std::pair<float, float>>& samples, float total, float share) {
    std::sort(samples.begin(), samples.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
    const float at = total * share;
    float run = 0.0f;
    for (const auto& [value, weight] : samples) {
        run += weight;
        if (run >= at) return value;
    }
    return samples.back().first;
}

float smooth(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// An RGB565 endpoint, display values 0..1.
glm::vec3 rgb565(std::uint16_t c) {
    return {static_cast<float>((c >> 11) & 31) / 31.0f, static_cast<float>((c >> 5) & 63) / 63.0f,
            static_cast<float>(c & 31) / 31.0f};
}

} // namespace

HorizonLand horizon_land(std::span<const HorizonTriangle> triangles, std::span<const std::optional<glm::vec3>> colors,
                         float seed) {
    HorizonLand out;
    out.seed = seed;
    std::vector<std::pair<float, float>> xs, ys, zs;
    float level_area = 0.0f, ground_weight = 0.0f, slope_weight = 0.0f;
    glm::vec3 ground_sum(0.0f), slope_sum(0.0f);
    for (const auto& t : triangles) {
        const glm::vec3 n = glm::cross(t.b - t.a, t.c - t.a);
        const float twice = glm::length(n);
        if (!(twice > 1e-4f) || !std::isfinite(twice)) continue;
        const float area = 0.5f * twice;
        const float up = std::abs(n.z) / twice;
        if (up < kHorizonSlope) continue; // walls
        const glm::vec3 mid = (t.a + t.b + t.c) / 3.0f;
        const std::optional<glm::vec3>* color = t.texture < colors.size() ? &colors[t.texture] : nullptr;
        if (up >= kHorizonLevel) {
            level_area += area;
            xs.emplace_back(mid.x, area);
            ys.emplace_back(mid.y, area);
            zs.emplace_back(mid.z, area);
            if (color && *color) {
                ground_sum += **color * area;
                ground_weight += area;
            }
        } else if (color && *color) {
            slope_sum += **color * area;
            slope_weight += area;
        }
    }
    if (level_area < kHorizonMinArea) return out;
    const float x0 = weighted_percentile(xs, level_area, kHorizonExtentCut);
    const float x1 = weighted_percentile(xs, level_area, 1.0f - kHorizonExtentCut);
    const float y0 = weighted_percentile(ys, level_area, kHorizonExtentCut);
    const float y1 = weighted_percentile(ys, level_area, 1.0f - kHorizonExtentCut);
    out.centre = {0.5f * (x0 + x1), 0.5f * (y0 + y1)};
    out.radius = 0.5f * glm::length(glm::vec2(x1 - x0, y1 - y0)) + kHorizonMargin;
    const float z10 = weighted_percentile(zs, level_area, 0.1f);
    out.ground = weighted_percentile(zs, level_area, 0.5f);
    const float z90 = weighted_percentile(zs, level_area, 0.9f);
    out.relief = std::clamp(z90 - z10, kHorizonMinRelief, kHorizonMaxRelief);
    out.ground_color = ground_weight > 0.0f ? ground_sum / ground_weight : kHorizonGround;
    // No slopes with a colour: the ground's, darker and greyer (rock under the same light).
    const float grey = glm::dot(out.ground_color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    out.slope_color = slope_weight > 0.0f ? slope_sum / slope_weight
                                          : glm::mix(out.ground_color, glm::vec3(grey), 0.6f) * 0.85f;
    out.valid = true;
    return out;
}

std::optional<glm::vec3> mean_texture_color(const assets::Image& image) {
    using assets::PixelFormat;
    if (image.mips.empty() || image.width == 0 || image.height == 0) return std::nullopt;
    // The smallest level with at least 16 texels (4x4), or the first.
    std::size_t level = 0;
    std::uint32_t w = image.width, h = image.height;
    while (level + 1 < image.mips.size() && (w / 2) * (h / 2) >= 16) {
        ++level;
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    const auto& data = image.mips[level];
    glm::dvec3 sum(0.0);
    double weight = 0.0;
    auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(data[i]); };
    if (image.format == PixelFormat::RGBA8) {
        const std::size_t texels = static_cast<std::size_t>(w) * h;
        if (data.size() < texels * 4) return std::nullopt;
        // At most about 4096 texels, evenly spread.
        const std::size_t step = std::max<std::size_t>(1, texels / 4096);
        for (std::size_t i = 0; i < texels; i += step) {
            const double a = byte(i * 4 + 3) / 255.0;
            const glm::vec3 display(byte(i * 4) / 255.0f, byte(i * 4 + 1) / 255.0f, byte(i * 4 + 2) / 255.0f);
            sum += glm::dvec3(linear_color(display)) * a;
            weight += a;
        }
    } else if (image.format == PixelFormat::BC1 || image.format == PixelFormat::BC2 || image.format == PixelFormat::BC3) {
        // The colour block's two endpoints, as the mean of its texels' (the palette lies between them). BC1's
        // punch-through blocks (colour0 <= colour1) count the same: their clear texels are not known without decoding.
        const std::size_t block = image.format == PixelFormat::BC1 ? 8 : 16;
        const std::size_t offset = image.format == PixelFormat::BC1 ? 0 : 8;
        const std::size_t blocks = data.size() / block;
        const std::size_t step = std::max<std::size_t>(1, blocks / 1024);
        for (std::size_t i = 0; i < blocks; i += step) {
            const std::size_t at = i * block + offset;
            const auto c0 = static_cast<std::uint16_t>(byte(at) | byte(at + 1) << 8);
            const auto c1 = static_cast<std::uint16_t>(byte(at + 2) | byte(at + 3) << 8);
            sum += glm::dvec3(linear_color(rgb565(c0)) + linear_color(rgb565(c1))) * 0.5;
            weight += 1.0;
        }
    } else {
        return std::nullopt;
    }
    if (!(weight > 0.0)) return std::nullopt;
    return glm::vec3(sum / weight);
}

float horizon_seed(std::string_view zone) {
    std::uint32_t h = 2166136261u; // FNV-1a
    for (char c : zone) {
        h ^= static_cast<std::uint8_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        h *= 16777619u;
    }
    return static_cast<float>(h % 10007u) * 0.1f; // 0 .. 1000.6: offsets into the noise, kept small for float precision
}

float horizon_strength(float fog_end, float extend, float overcast) {
    const float fog = smooth(kHorizonFogNone, kHorizonFogFull, fog_end);
    return fog * std::clamp(extend, 0.0f, 1.0f) * (1.0f - 0.75f * std::clamp(overcast, 0.0f, 1.0f));
}

} // namespace hailstone::render
