#pragma once

// What a GPU needs for Hailstone (render/vk_context.cpp pick_physical_device), kept apart from Vulkan so the words a
// player sees when no GPU qualifies are tested. The README's "System requirements" lists the same.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::render {

enum class GpuKind { Other, Integrated, Discrete };

// What one GPU reports, read from its Vulkan properties and features.
struct GpuSupport {
    std::string name;
    GpuKind kind = GpuKind::Other;
    std::uint32_t api_major = 0;
    std::uint32_t api_minor = 0;
    bool dynamic_rendering = false;
    bool synchronization2 = false;
    bool runtime_descriptor_array = false;
    bool nonuniform_sampled_image_indexing = false;
    bool descriptor_binding_partially_bound = false;
    bool descriptor_binding_variable_count = false;
    // Depth-only barriers on a depth/stencil fallback format (vk_context.cpp's depth format); required of every 1.2+
    // device, checked all the same.
    bool separate_depth_stencil_layouts = false;
    bool presents = false; // a queue family that draws and presents to the window
};

// What `gpu` lacks, one item each, in the order checked; empty when it can run Hailstone. Below Vulkan 1.3 only the
// version is named: the features are not read from such a device.
std::vector<std::string> gpu_missing(const GpuSupport& gpu);

// The error for no usable GPU: every GPU with what it lacks, or that none was found.
std::string no_usable_gpu_text(const std::vector<GpuSupport>& gpus);

struct GpuSelection {
    std::optional<std::size_t> index;
    std::string error;
};

// Empty `exact_name` keeps the automatic preference: discrete, integrated, other, first on a tie. A requested
// generic model must match exactly one enumerated GPU and meet every requirement; it never falls back.
GpuSelection choose_gpu(const std::vector<GpuSupport>& gpus, std::string_view exact_name = {});

// The instance extensions to enable, from what the window needs and what the Vulkan library offers.
// VK_KHR_portability_enumeration is the Vulkan loader's: SDL asks for it on macOS, but MoltenVK loaded directly (the
// one a macOS release ships, platform/paths.h) does not have it and lists its GPU without the opt-in. So it is
// enabled, with its flag, only where offered, whoever asked. Any other wanted extension the library lacks is
// `missing`, named in the error rather than left to vkCreateInstance's bare VK_ERROR_EXTENSION_NOT_PRESENT.
struct InstanceExtensions {
    std::vector<std::string> enable;
    std::vector<std::string> missing;
    bool portability = false; // enable has it: set VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
};
InstanceExtensions choose_instance_extensions(const std::vector<std::string>& wanted,
                                              const std::vector<std::string>& offered);

} // namespace hailstone::render
