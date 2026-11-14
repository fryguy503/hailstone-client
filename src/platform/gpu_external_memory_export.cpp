// The exporting side of platform/gpu_external_memory.h: only the broker tool (tools/gpu_lab) links it.
#include "platform/gpu_external_memory.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

namespace hailstone::platform {

VkResult gpu_export_memory(VkDevice device, VkDeviceMemory memory, void** handle) noexcept {
#if defined(_WIN32)
    if (!handle) return VK_ERROR_INITIALIZATION_FAILED;
    auto get = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
    if (!get) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkMemoryGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    info.memory = memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    HANDLE out = nullptr;
    const VkResult result = get(device, &info, &out);
    *handle = result == VK_SUCCESS ? out : nullptr;
    return result;
#else
    (void)device; (void)memory; (void)handle;
    return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
}

} // namespace hailstone::platform
