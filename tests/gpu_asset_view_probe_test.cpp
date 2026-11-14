#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <string>

#include "render/gpu_asset_view_probe.h"

using namespace hailstone;
using namespace hailstone::render;

namespace {
// Only framing is tested here; the native GPU integration tool supplies compiled shaders and validates draws.
constexpr std::array<std::uint32_t, 5> shader{0x07230203, 0x00010600, 0, 1, 0};
const std::array<glm::mat4, 2> cameras{glm::mat4{1.0f}, glm::mat4{1.0f}};

assets::GpuProbeScene scene() {
    assets::GpuProbeScene out;
    out.vertices.resize(3);
    out.vertices[1].position.x = 1.0f;
    out.vertices[2].position.y = 1.0f;
    out.indices = {0, 1, 2};
    assets::GpuProbeTexture texture;
    texture.image.width = texture.image.height = 1;
    texture.image.mips = {{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}}};
    out.textures.push_back(std::move(texture));
    out.draws.push_back({0, 3, 0, 1.0f, 0.0f});
    return out;
}

bool valid(const assets::GpuProbeScene& input, GpuAssetViewProbeOptions options = {}) {
    return validate_gpu_asset_view_probe_inputs(input, cameras, shader, shader, options);
}
} // namespace

TEST_CASE("GPU asset view input: complete BC mip chains may retain equal block byte sizes") {
    auto input = scene();
    REQUIRE(valid(input));
    auto& image = input.textures[0].image;
    image.width = image.height = 4;
    image.format = assets::PixelFormat::BC1;
    image.mips.assign(3, std::vector<std::byte>(8)); // 4x4, 2x2 and 1x1 each occupy one block
    CHECK(valid(input));
    image.mips.push_back(std::vector<std::byte>(8));
    CHECK_FALSE(valid(input));
    image.mips.pop_back();
    image.mips[1].pop_back();
    CHECK_FALSE(valid(input));
    image.format = assets::PixelFormat::BC3;
    image.mips.assign(3, std::vector<std::byte>(16));
    CHECK(valid(input));
}

TEST_CASE("GPU asset view input: malformed ranges and nonfinite payloads fail before GPU use") {
    auto input = scene();
    input.indices[2] = 3;
    CHECK_FALSE(valid(input));
    input = scene();
    input.draws[0].first_index = std::numeric_limits<std::uint32_t>::max();
    CHECK_FALSE(valid(input));
    input = scene();
    input.draws[0].index_count = 2;
    CHECK_FALSE(valid(input));
    input = scene();
    input.draws[0].texture = 1;
    CHECK_FALSE(valid(input));
    input = scene();
    input.vertices[0].uv.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(valid(input));
    input = scene();
    input.vertices[1].position.x = std::numeric_limits<float>::infinity();
    CHECK_FALSE(valid(input));
    input = scene();
    input.textures[0].image.format = static_cast<assets::PixelFormat>(255);
    CHECK_FALSE(valid(input));
    input = scene();
    input.textures.clear();
    CHECK_FALSE(valid(input));
}

TEST_CASE("GPU asset view input: per-material alpha cutoff has a finite normalized range") {
    auto input = scene();
    CHECK(valid(input)); // Opaque materials accept alpha zero rather than discarding their coverage.
    input.draws[0].alpha_cutoff = 0.5f;
    CHECK(valid(input));
    input.draws[0].alpha_cutoff = 1.0f;
    CHECK(valid(input));
    input.draws[0].alpha_cutoff = -0.01f;
    CHECK_FALSE(valid(input));
    input.draws[0].alpha_cutoff = 1.01f;
    CHECK_FALSE(valid(input));
    input.draws[0].alpha_cutoff = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(valid(input));
    input.draws[0].alpha_cutoff = std::numeric_limits<float>::infinity();
    CHECK_FALSE(valid(input));
}

TEST_CASE("GPU asset view input: camera target and shader bounds are checked without Vulkan initialization") {
    const auto input = scene();
    CHECK(valid(input, {false, 1, 1}));
    CHECK(valid(input, {true, 2048, 2048}));
    CHECK_FALSE(valid(input, {false, 0, 512}));
    CHECK_FALSE(valid(input, {false, 2049, 512}));
    CHECK_FALSE(validate_gpu_asset_view_probe_inputs(input, std::span(cameras).first(1), shader, shader));
    std::array<glm::mat4, 7> too_many{};
    CHECK_FALSE(validate_gpu_asset_view_probe_inputs(input, too_many, shader, shader));
    auto nonfinite = cameras;
    nonfinite[1][0][0] = std::numeric_limits<float>::infinity();
    CHECK_FALSE(validate_gpu_asset_view_probe_inputs(input, nonfinite, shader, shader));
    CHECK_FALSE(validate_gpu_asset_view_probe_inputs(input, cameras, {}, shader));
    auto invalid_shader = shader;
    invalid_shader[0] = 0;
    CHECK_FALSE(validate_gpu_asset_view_probe_inputs(input, cameras, invalid_shader, shader));
    std::string error;
    CHECK_FALSE(GpuAssetViewProbe::create(VK_NULL_HANDLE, input, cameras, shader, shader, {false, 0, 0}, &error));
    CHECK(error == "view dimensions must be 1..2048");
    CHECK_FALSE(GpuAssetViewProbe::create(VK_NULL_HANDLE, input, cameras, shader, shader, {}, &error));
    CHECK(error == "Vulkan instance/device unavailable");
}
