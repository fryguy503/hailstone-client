#pragma once

// Diagnostic capabilities and a bounded same-device alias smoke test. These do not enable cross-process rendering.

#include <array>
#include <cstdint>
#include <string>

#include <volk.h>

namespace hailstone::render {

enum class GpuShareTransport { None, OpaqueWin32, OpaqueFd };
const char* gpu_share_transport_name(GpuShareTransport transport);

struct ExternalMemoryCapability {
    bool queried = false;
    bool importable = false;
    bool exportable = false;
    bool dedicated_only = false;
    bool compatible_handle = false;
    VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;
};

struct GpuSharedImageCapability {
    VkFormat format = VK_FORMAT_UNDEFINED;
    ExternalMemoryCapability memory;
};

struct GpuShareCapabilities {
    GpuShareTransport transport = GpuShareTransport::None;
    bool memory_extension = false;
    bool semaphore_extension = false;
    ExternalMemoryCapability transfer_buffer;
    ExternalMemoryCapability geometry_buffer;
    bool binary_semaphore_queried = false;
    bool binary_semaphore_importable = false;
    bool binary_semaphore_exportable = false;
    std::array<GpuSharedImageCapability, 4> images{}; // RGBA8, BC1, BC3, BC7; optimal sampled/transfer images
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
};

// Requires a valid Vulkan 1.1+ physical device and loaded instance entry points. Does not query ID/UUID properties.
GpuShareCapabilities query_gpu_share_capabilities(VkPhysicalDevice physical);
// Capability bits for a potential buffer handoff, not proof of matching external devices or parallel client reads.
bool external_transfer_supported(const GpuShareCapabilities& capabilities);

struct GpuAliasSmokeOptions {
    std::uint32_t views = 24;
    VkDeviceSize bytes = 65536;
};
// Bound device-local memory to 1 MiB and readback to 64 MiB, with word-aligned transfer ranges.
bool valid_gpu_alias_smoke_options(const GpuAliasSmokeOptions& options);

struct GpuAliasSmokeResult {
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    bool matched = false;
    std::uint32_t views = 0;
    VkDeviceSize bytes_per_view = 0;
    VkDeviceSize device_allocation_bytes = 0;
    VkDeviceSize readback_allocation_bytes = 0;
    std::uint64_t verified_words = 0;
    std::string error;
};

// Creates its own logical device and local dispatch table. One allocation backs all buffer views, whose GPU
// copies are ordered after a fill. This is a single-device broker primitive, not a multi-process/parallel-client test.
GpuAliasSmokeResult run_gpu_alias_smoke(VkPhysicalDevice physical, GpuAliasSmokeOptions options = {});

} // namespace hailstone::render
