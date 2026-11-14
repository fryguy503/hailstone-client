#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>

#include "gpu_lab/gpu_probe_scene.h"
#include "assets/zone.h"
#include "core/byte_io.h"
#include "game_files.h"
#include "render/gpu.h"
#include "render/material_state.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {
GpuProbeScenePreparation preparation() {
    return {[](const Material& material) {
                GpuProbeMaterialPolicy policy;
                policy.draw = material.blend != BlendMode::Invisible;
                policy.masked = material.blend == BlendMode::Masked;
                policy.reads_alpha = policy.masked;
                policy.alpha_cutoff = policy.masked ? 0.5f : 0.0f;
                return policy;
            }, [](Image image, const GpuProbeMaterialPolicy&) { return image; }};
}
Zone triangle_zone() {
    Zone zone;
    Mesh mesh;
    mesh.vertices.resize(3);
    mesh.vertices[1].position.x = 1;
    mesh.vertices[2].position.y = 2;
    mesh.indices = {0, 1, 2};
    mesh.materials.emplace_back();
    mesh.parts.push_back({0, 3, 0});
    zone.terrain.push_back(std::move(mesh));
    return zone;
}
std::vector<std::byte> red_bmp() {
    ByteWriter bytes;
    bytes.fixed_string("BM", 2);
    bytes.le<std::uint32_t>(58); bytes.zeros(4); bytes.le<std::uint32_t>(54);
    bytes.le<std::uint32_t>(40); bytes.le<std::int32_t>(1); bytes.le<std::int32_t>(1);
    bytes.le<std::uint16_t>(1); bytes.le<std::uint16_t>(24);
    bytes.le<std::uint32_t>(0); bytes.le<std::uint32_t>(4); bytes.zeros(16);
    bytes.bytes(std::array{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{0}});
    return bytes.take();
}
} // namespace

TEST_CASE("GPU probe scene: actual static mesh placements transform geometry and preserve private colors") {
    auto zone = triangle_zone();
    zone.objects.emplace("OBJECT", zone.terrain[0]);
    Placement placement;
    placement.actor = "OBJECT";
    placement.transform = glm::translate(glm::mat4(1), glm::vec3(10, 0, 3));
    placement.vertex_colors = {0x12345678};
    zone.placements.push_back(placement);
    zone.placements.push_back({"MISSING"});
    const auto scene = build_gpu_probe_scene(zone, preparation());
    CHECK(scene.vertices.size() == 6);
    CHECK(scene.indices == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5});
    CHECK(scene.draws.size() == 2);
    CHECK(scene.vertices[3].position.x == 10);
    CHECK(scene.vertices[3].position.z == 3);
    CHECK(scene.vertices[3].color == 0x12345678);
    CHECK(scene.vertices[4].color == 0xffffffff);
    CHECK(scene.bounds_max.x == 11);
    CHECK(scene.inventory.placed_objects == 1);
    CHECK(scene.inventory.missing_objects == 1);
    CHECK(scene.inventory.vertex_bytes == 6 * sizeof(GpuProbeVertex));
    CHECK(scene.inventory.index_bytes == 6 * sizeof(std::uint32_t));
    CHECK(scene.scene_digest == build_gpu_probe_scene(zone, preparation()).scene_digest);
    zone.placements[0].transform[3].x = 11;
    CHECK(scene.geometry_digest != build_gpu_probe_scene(zone, preparation()).geometry_digest);
}

TEST_CASE("GPU probe scene: texture inventory decodes referenced assets and retains native mip formats") {
    auto zone = triangle_zone();
    zone.generated_files.emplace("red.bmp", red_bmp());
    zone.terrain[0].materials[0].frames = {"red.bmp", "missing.bmp"};
    const auto scene = build_gpu_probe_scene(zone, preparation());
    REQUIRE(scene.textures.size() == 2); // white fallback and the authored red pixel
    CHECK(scene.textures[1].image.format == PixelFormat::RGBA8);
    CHECK(scene.textures[1].image.mips[0][0] == std::byte{255});
    CHECK(scene.draws[0].texture == 1);
    CHECK(scene.inventory.missing_textures == 1);
    CHECK(scene.inventory.animated_materials == 1);
    CHECK(scene.inventory.texture_bytes == 8);
    Image bc;
    bc.width = bc.height = 4;
    bc.format = PixelFormat::BC1;
    bc.mips = {std::vector<std::byte>(8), std::vector<std::byte>(8), std::vector<std::byte>(8)};
    const auto digest = gpu_probe_texture_digest(bc);
    bc.mips[1][0] = std::byte{1};
    CHECK(digest != gpu_probe_texture_digest(bc));
    bc.mips.push_back(std::vector<std::byte>(8));
    CHECK_THROWS_AS(gpu_probe_texture_digest(bc), std::runtime_error);
}

TEST_CASE("GPU probe scene: invalid geometry and texture bounds fail before GPU use") {
    auto zone = triangle_zone();
    zone.terrain[0].indices[2] = 3;
    CHECK_THROWS_AS(build_gpu_probe_scene(zone, preparation()), std::runtime_error);
    zone = triangle_zone();
    zone.terrain[0].parts[0].index_count = 4;
    CHECK_THROWS_AS(build_gpu_probe_scene(zone, preparation()), std::runtime_error);
    zone = triangle_zone();
    zone.terrain[0].vertices[0].position.x = std::numeric_limits<float>::infinity();
    CHECK_THROWS_AS(build_gpu_probe_scene(zone, preparation()), std::runtime_error);
    zone = triangle_zone();
    zone.terrain[0].materials[0].blend = BlendMode::Invisible;
    CHECK_THROWS_AS(build_gpu_probe_scene(zone, preparation()), std::runtime_error);
    CHECK_THROWS_AS(build_gpu_probe_scene(triangle_zone(), {}), std::invalid_argument);
    Image invalid;
    invalid.width = invalid.height = 2;
    invalid.mips = {std::vector<std::byte>(4)};
    CHECK_THROWS_AS(gpu_probe_texture_digest(invalid), std::runtime_error);
}

TEST_CASE("GPU probe scene: geometry identity includes vertex and index array boundaries") {
    auto first = triangle_zone();
    for (auto& vertex : first.terrain[0].vertices) { vertex.position = {}; vertex.uv = {}; vertex.color = 0; }
    first.terrain[0].indices.assign(9, 0);
    first.terrain[0].parts[0].index_count = 9;
    auto second = first;
    second.terrain[0].vertices.push_back(first.terrain[0].vertices.front());
    second.terrain[0].indices.assign(3, 0);
    second.terrain[0].parts[0].index_count = 3;
    const auto a = build_gpu_probe_scene(first, preparation());
    const auto b = build_gpu_probe_scene(second, preparation());
    // Both unframed byte streams contain108 zero bytes: three vertices+nine indices vs four vertices+three indices.
    CHECK(a.inventory.vertex_bytes + a.inventory.index_bytes == b.inventory.vertex_bytes + b.inventory.index_bytes);
    CHECK(a.geometry_digest != b.geometry_digest);
}

TEST_CASE("GPU probe scene: stock gfaydark factory prepares bounded immutable static uploads without a GPU"
          * doctest::skip(!test::game_dir())) {
    const auto zone = load_zone(*test::game_dir(), "gfaydark");
    const GpuProbeScenePreparation actual_preparation{
        [](const Material& material) {
            const auto state = render::material_state(material);
            return GpuProbeMaterialPolicy{state.draw, state.masked_texture, !state.alpha_unused(), state.opacity, state.alpha_ref};
        }, [](Image image, const GpuProbeMaterialPolicy&) {
            if (auto prepared = render::prepare_texture_image(image, render::TextureUse::Color, false)) return std::move(*prepared);
            return image;
        }};
    const auto scene = build_gpu_probe_scene(zone, actual_preparation);
    REQUIRE_FALSE(scene.vertices.empty());
    REQUIRE_FALSE(scene.indices.empty());
    REQUIRE_FALSE(scene.draws.empty());
    REQUIRE_FALSE(scene.textures.empty());
    CHECK(scene.inventory.terrain_meshes == zone.terrain.size());
    CHECK(scene.inventory.vertex_bytes == scene.vertices.size() * sizeof(GpuProbeVertex));
    CHECK(scene.inventory.index_bytes == scene.indices.size() * sizeof(std::uint32_t));
    for (const auto index : scene.indices) CHECK(index < scene.vertices.size());
    for (const auto& draw : scene.draws) {
        REQUIRE(draw.first_index <= scene.indices.size());
        CHECK(draw.index_count <= scene.indices.size() - draw.first_index);
        CHECK(draw.index_count % 3 == 0);
        CHECK(draw.texture < scene.textures.size());
    }
    std::uint64_t texture_bytes = 0;
    for (const auto& texture : scene.textures) {
        REQUIRE(texture.image.width > 0);
        REQUIRE(texture.image.height > 0);
        REQUIRE_FALSE(texture.image.mips.empty());
        auto width = texture.image.width, height = texture.image.height;
        std::uint64_t image_size = 0;
        for (const auto& mip : texture.image.mips) {
            CHECK(mip.size() == mip_size(texture.image.format, width, height));
            image_size += mip.size();
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        CHECK(image_size == texture.upload_bytes);
        CHECK(texture.digest == gpu_probe_texture_digest(texture.image));
        texture_bytes += image_size;
    }
    CHECK(texture_bytes == scene.inventory.texture_bytes);
    const auto repeated = build_gpu_probe_scene(zone, actual_preparation);
    CHECK(repeated.scene_digest == scene.scene_digest);
    CHECK(repeated.geometry_digest == scene.geometry_digest);
    CHECK(repeated.inventory.texture_bytes == scene.inventory.texture_bytes);
    CHECK(repeated.inventory.unique_texture_contents == scene.inventory.unique_texture_contents);
}
