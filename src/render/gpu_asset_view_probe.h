#pragma once

// A headless diagnostic renderer. One Vulkan device, separate view targets, and optional immutable resource reuse.
// It does not render game clients or implement cross-process sharing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <volk.h>

#include "assets/gpu_probe_scene.h"
#include "core/sha256.h"

namespace hailstone::render {

struct GpuAssetViewProbeOptions {
    bool share_immutable = false;
    std::uint32_t width = 512, height = 512;
};

struct GpuAssetMemoryCategory {
    std::uint64_t allocations = 0, logical_bytes = 0, allocated_bytes = 0;
};

struct GpuAssetViewProbeStats {
    GpuAssetMemoryCategory immutable_geometry, immutable_textures, private_targets, readback;
    GpuAssetMemoryCategory staging_peak;
    std::uint64_t staging_live_bytes = 0;
    std::uint64_t unique_texture_contents = 0, texture_copies = 0, geometry_copies = 0;
    std::uint64_t bc_fallback_textures = 0, frames_rendered = 0;
    // Indexed by assets::PixelFormat; counts actual GPU image copies after device-specific fallback.
    std::array<std::uint64_t, 7> upload_format_copies{};
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
};

struct GpuAssetViewPixels {
    std::uint32_t width = 0, height = 0;
    std::vector<std::byte> rgba;
    Sha256Digest hash{};
    std::uint64_t non_background_pixels = 0;
};

// Pure container/range validation, including SPIR-V framing rather than shader semantic validation.
// No Vulkan entry points are called; malformed inputs can be rejected on machines without a GPU.
bool validate_gpu_asset_view_probe_inputs(const assets::GpuProbeScene& scene,
    std::span<const glm::mat4> view_projections, std::span<const std::uint32_t> vertex_spirv,
    std::span<const std::uint32_t> fragment_spirv, GpuAssetViewProbeOptions options = {}, std::string* error = nullptr);

class GpuAssetViewProbe {
public:
    GpuAssetViewProbe(GpuAssetViewProbe&&) noexcept;
    GpuAssetViewProbe& operator=(GpuAssetViewProbe&&) noexcept;
    ~GpuAssetViewProbe();
    GpuAssetViewProbe(const GpuAssetViewProbe&) = delete;
    GpuAssetViewProbe& operator=(const GpuAssetViewProbe&) = delete;

    // Requires Vulkan 1.3+ and loaded instance entry points. Inputs are consumed synchronously; no caller-owned
    // geometry, image, shader or camera storage is retained after return. Views must contain 2..6 finite matrices.
    // The simple shader ignores vertex colors/material effects; this is not client-renderer image parity.
    static std::optional<GpuAssetViewProbe> create(VkPhysicalDevice physical, const assets::GpuProbeScene& scene,
        std::span<const glm::mat4> view_projections, std::span<const std::uint32_t> vertex_spirv,
        std::span<const std::uint32_t> fragment_spirv, GpuAssetViewProbeOptions options = {}, std::string* error = nullptr);

    // Each call waits for completed GPU draws. Pixel copies/hashes can be omitted during a fixed-rate native hold;
    // readback resources remain allocated identically in either mode. GPU resources remain owned until destruction.
    bool render(std::string* error = nullptr, bool read_pixels = true);
    const std::vector<GpuAssetViewPixels>& pixels() const;
    const GpuAssetViewProbeStats& stats() const;

private:
    struct State;
    explicit GpuAssetViewProbe(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace hailstone::render
