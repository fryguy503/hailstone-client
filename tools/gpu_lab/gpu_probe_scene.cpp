#include "gpu_lab/gpu_probe_scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "assets/texture_overrides.h"
#include "assets/zone.h"
#include "core/byte_io.h"

namespace hailstone::assets {
namespace {
void dimensions(ByteWriter& writer, const Image& image) {
    writer.le(image.width);
    writer.le(image.height);
    writer.le(static_cast<std::uint32_t>(image.format));
    writer.le(static_cast<std::uint32_t>(image.mips.size()));
}

std::uint64_t image_bytes(const Image& image) {
    if (!image.width || !image.height || image.mips.empty()) throw std::runtime_error("empty diagnostic texture");
    if (static_cast<unsigned>(image.format) > static_cast<unsigned>(PixelFormat::BC7))
        throw std::runtime_error("invalid diagnostic texture format");
    std::size_t maximum_mips = 1;
    for (auto dimension = std::max(image.width, image.height); dimension > 1; dimension /= 2) ++maximum_mips;
    if (image.mips.size() > maximum_mips) throw std::runtime_error("too many diagnostic mip levels");
    std::uint32_t width = image.width, height = image.height;
    std::uint64_t total = 0;
    for (const auto& mip : image.mips) {
        if (mip.size() != mip_size(image.format, width, height)) throw std::runtime_error("invalid diagnostic mip size");
        if (mip.size() > std::numeric_limits<std::uint64_t>::max() - total) throw std::runtime_error("texture size overflow");
        total += mip.size();
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    return total;
}

bool finite(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace

Sha256Digest gpu_probe_texture_digest(const Image& image) {
    (void)image_bytes(image);
    ByteWriter metadata;
    metadata.fixed_string("gpu-probe-image-v1", 18);
    dimensions(metadata, image);
    Sha256 hash;
    hash.update(metadata.data());
    for (const auto& mip : image.mips) {
        ByteWriter size;
        size.le(static_cast<std::uint64_t>(mip.size()));
        hash.update(size.data());
        hash.update(mip);
    }
    return hash.finish();
}

GpuProbeScene build_gpu_probe_scene(const Zone& zone, const GpuProbeScenePreparation& preparation) {
    if (!preparation.material || !preparation.image) throw std::invalid_argument("diagnostic preparation callbacks required");
    GpuProbeScene out;
    std::unordered_map<std::string, std::uint32_t> texture_slots;
    std::map<Sha256Digest, std::uint64_t> unique_textures;
    auto keep_image = [&](Image image) {
        const auto size = image_bytes(image);
        const auto digest = gpu_probe_texture_digest(image);
        if (out.textures.size() >= std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("too many textures");
        const auto slot = static_cast<std::uint32_t>(out.textures.size());
        out.textures.push_back({std::move(image), digest, size});
        out.inventory.texture_bytes += size;
        unique_textures.try_emplace(digest, size);
        return slot;
    };
    Image white;
    white.width = white.height = 1;
    white.mips = {{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}}};
    keep_image(std::move(white));
    auto texture = [&](const std::string& name, const GpuProbeMaterialPolicy& policy) {
        if (name.empty()) return std::uint32_t{0};
        const auto key = name + (policy.masked ? "#m" : "#o") + (policy.reads_alpha ? "#a" : "#n");
        if (const auto found = texture_slots.find(key); found != texture_slots.end()) return found->second;
        auto source = zone.read_file(name);
        std::string error;
        auto decoded = decode_game_texture(name, source, {.palette_index0_transparent = policy.masked,
            .wld_bmp_rows = true, .reads_alpha = policy.reads_alpha}, &error, zone.name);
        std::uint32_t slot = 0;
        if (decoded) slot = keep_image(preparation.image(std::move(*decoded), policy));
        else ++out.inventory.missing_textures;
        texture_slots.emplace(key, slot);
        return slot;
    };
    bool has_bounds = false;
    auto append = [&](const Mesh& mesh, const glm::mat4& transform, const std::vector<std::uint32_t>& colors) {
        if (mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max() - out.vertices.size())
            throw std::runtime_error("diagnostic vertex count exceeds32-bit indices");
        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const auto& vertex = mesh.vertices[i];
            const auto position = glm::vec3(transform * glm::vec4(vertex.position, 1.0f));
            if (!finite(position) || !std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y))
                throw std::runtime_error("nonfinite diagnostic vertex");
            out.vertices.push_back({position, vertex.uv, i < colors.size() ? colors[i] : vertex.color});
            if (!has_bounds) { out.bounds_min = out.bounds_max = position; has_bounds = true; }
            else { out.bounds_min = glm::min(out.bounds_min, position); out.bounds_max = glm::max(out.bounds_max, position); }
        }
        for (const auto& part : mesh.parts) {
            if (part.material >= mesh.materials.size() || part.first_index > mesh.indices.size() ||
                part.index_count > mesh.indices.size() - part.first_index || part.index_count % 3 != 0)
                throw std::runtime_error("invalid diagnostic mesh part");
            const auto& material = mesh.materials[part.material];
            const auto policy = preparation.material(material);
            if (!policy.draw || material.blend == BlendMode::Invisible || part.index_count == 0) {
                ++out.inventory.omitted_draws;
                continue;
            }
            if (material.eqg_shader != EqgShader::None || !material.detail_layers.empty() ||
                !material.effect.textures.empty() || !material.effect.normal_map.empty()) ++out.inventory.material_effect_draws;
            if (policy.opacity != 1.0f || material.blend == BlendMode::Additive) ++out.inventory.translucent_draws;
            if (policy.alpha_cutoff != 0.0f) ++out.inventory.masked_draws;
            if (material.frames.size() > 1) ++out.inventory.animated_materials;
            std::uint32_t slot = 0;
            for (std::size_t frame = 0; frame < material.frames.size(); ++frame) {
                const auto found = texture(material.frames[frame], policy);
                if (frame == 0) slot = found;
            }
            if (part.index_count > std::numeric_limits<std::uint32_t>::max() - out.indices.size())
                throw std::runtime_error("diagnostic index count exceeds32-bit draw ranges");
            const auto first = static_cast<std::uint32_t>(out.indices.size());
            for (std::size_t i = part.first_index; i < static_cast<std::size_t>(part.first_index) + part.index_count; ++i) {
                if (mesh.indices[i] >= mesh.vertices.size()) throw std::runtime_error("diagnostic mesh index out of range");
                out.indices.push_back(base + mesh.indices[i]);
            }
            out.draws.push_back({first, part.index_count, slot, policy.opacity, policy.alpha_cutoff});
        }
    };
    for (const auto& mesh : zone.terrain) { append(mesh, glm::mat4(1.0f), {}); ++out.inventory.terrain_meshes; }
    for (const auto& placement : zone.placements) {
        const auto found = zone.objects.find(placement.actor);
        if (found == zone.objects.end()) { ++out.inventory.missing_objects; continue; }
        append(found->second, placement.transform, placement.vertex_colors);
        ++out.inventory.placed_objects;
    }
    if (out.draws.empty() || out.indices.empty()) throw std::runtime_error("zone has no drawable diagnostic geometry");
    out.inventory.vertex_bytes = static_cast<std::uint64_t>(out.vertices.size()) * sizeof(GpuProbeVertex);
    out.inventory.index_bytes = static_cast<std::uint64_t>(out.indices.size()) * sizeof(std::uint32_t);
    out.inventory.unique_texture_contents = unique_textures.size();
    for (const auto& [digest, bytes] : unique_textures) { (void)digest; out.inventory.unique_texture_bytes += bytes; }
    Sha256 geometry;
    geometry.update("gpu-probe-geometry-v2");
    ByteWriter geometry_counts;
    geometry_counts.le(static_cast<std::uint64_t>(out.vertices.size()));
    geometry_counts.le(static_cast<std::uint64_t>(out.indices.size()));
    geometry.update(geometry_counts.data());
    for (const auto& vertex : out.vertices) {
        ByteWriter bytes;
        for (int i = 0; i < 3; ++i) bytes.f32(vertex.position[i]);
        for (int i = 0; i < 2; ++i) bytes.f32(vertex.uv[i]);
        bytes.le(vertex.color);
        geometry.update(bytes.data());
    }
    for (const auto index : out.indices) { ByteWriter bytes; bytes.le(index); geometry.update(bytes.data()); }
    out.geometry_digest = geometry.finish();
    Sha256 scene;
    scene.update("gpu-probe-scene-v1");
    scene.update(std::as_bytes(std::span(out.geometry_digest)));
    for (const auto& image : out.textures) scene.update(std::as_bytes(std::span(image.digest)));
    for (const auto& draw : out.draws) {
        ByteWriter bytes;
        bytes.le(draw.first_index); bytes.le(draw.index_count); bytes.le(draw.texture);
        bytes.f32(draw.opacity); bytes.f32(draw.alpha_cutoff);
        scene.update(bytes.data());
    }
    out.scene_digest = scene.finish();
    return out;
}
} // namespace hailstone::assets
