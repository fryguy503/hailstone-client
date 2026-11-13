#include "render/gpu_share_capabilities.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace hailstone::render {
namespace {
constexpr std::array<VkFormat, 4> image_formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                                             VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK};
constexpr std::uint64_t wait_nanoseconds = 5'000'000'000;

ExternalMemoryCapability memory_capability(const VkExternalMemoryProperties& properties, VkResult result,
                                           VkExternalMemoryHandleTypeFlagBits handle) {
    ExternalMemoryCapability out;
    out.queried = true;
    out.result = result;
    if (result == VK_SUCCESS) {
        out.importable = (properties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
        out.exportable = (properties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
        out.dedicated_only = (properties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
        out.compatible_handle = (properties.compatibleHandleTypes & handle) != 0;
    }
    return out;
}

bool usable(const ExternalMemoryCapability& capability) {
    return capability.queried && capability.result == VK_SUCCESS && capability.importable && capability.exportable &&
           capability.compatible_handle;
}

VkResult device_extensions(VkPhysicalDevice physical, std::vector<VkExtensionProperties>& extensions) {
    std::uint32_t count = 0;
    auto result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    if (result != VK_SUCCESS) return result;
    extensions.resize(count);
    result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
    extensions.resize(count);
    return result;
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

struct SmokeError : std::runtime_error {
    SmokeError(VkResult code, const char* operation) : std::runtime_error(operation), result(code) {}
    VkResult result;
};

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw SmokeError(result, operation);
}

struct SmokeResources {
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable api{};
    std::vector<VkBuffer> views;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE, readback_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool pending = false;

    ~SmokeResources() {
        if (!device) return;
        // A failed bounded fence wait reports a failed proof, but pending Vulkan objects still cannot be freed.
        // Device loss returns from this cleanup wait; a driver that never completes may stall its own teardown.
        if (pending) api.vkDeviceWaitIdle(device);
        if (mapped) api.vkUnmapMemory(device, readback_memory);
        if (fence) api.vkDestroyFence(device, fence, nullptr);
        if (pool) api.vkDestroyCommandPool(device, pool, nullptr);
        if (readback) api.vkDestroyBuffer(device, readback, nullptr);
        for (const auto view : views) if (view) api.vkDestroyBuffer(device, view, nullptr);
        if (readback_memory) api.vkFreeMemory(device, readback_memory, nullptr);
        if (memory) api.vkFreeMemory(device, memory, nullptr);
        api.vkDestroyDevice(device, nullptr);
    }
};

std::uint32_t memory_type(const VkPhysicalDeviceMemoryProperties& properties, std::uint32_t bits,
                          VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) {
    for (const auto extra : {preferred, VkMemoryPropertyFlags{0}})
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((bits & (std::uint32_t{1} << i)) &&
                (properties.memoryTypes[i].propertyFlags & (required | extra)) == (required | extra)) return i;
    throw SmokeError(VK_ERROR_FEATURE_NOT_PRESENT, "required memory type unavailable");
}
} // namespace

const char* gpu_share_transport_name(GpuShareTransport transport) {
    switch (transport) {
    case GpuShareTransport::OpaqueWin32: return "opaque-win32";
    case GpuShareTransport::OpaqueFd: return "opaque-fd";
    default: return "none";
    }
}

bool external_transfer_supported(const GpuShareCapabilities& capabilities) {
    return capabilities.result == VK_SUCCESS && capabilities.transport != GpuShareTransport::None &&
           capabilities.memory_extension && capabilities.semaphore_extension && usable(capabilities.transfer_buffer) &&
           usable(capabilities.geometry_buffer) && capabilities.binary_semaphore_queried &&
           capabilities.binary_semaphore_importable && capabilities.binary_semaphore_exportable;
}

GpuShareCapabilities query_gpu_share_capabilities(VkPhysicalDevice physical) {
    GpuShareCapabilities out;
    for (std::size_t i = 0; i < image_formats.size(); ++i) out.images[i].format = image_formats[i];
    if (!physical || !vkGetPhysicalDeviceProperties || !vkEnumerateDeviceExtensionProperties) return out;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1) {
        out.result = VK_ERROR_INCOMPATIBLE_DRIVER;
        return out;
    }
    const char* memory_extension = nullptr;
    const char* semaphore_extension = nullptr;
    VkExternalMemoryHandleTypeFlagBits handle{};
#if defined(_WIN32)
    out.transport = GpuShareTransport::OpaqueWin32;
    memory_extension = "VK_KHR_external_memory_win32";
    semaphore_extension = "VK_KHR_external_semaphore_win32";
    handle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#elif defined(__linux__)
    out.transport = GpuShareTransport::OpaqueFd;
    memory_extension = "VK_KHR_external_memory_fd";
    semaphore_extension = "VK_KHR_external_semaphore_fd";
    handle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
    out.result = VK_SUCCESS;
    // Metal object handles need a separately designed process transport; they are not opaque fd/Win32 handles.
    if (!memory_extension) return out;
    std::vector<VkExtensionProperties> extensions;
    out.result = device_extensions(physical, extensions);
    if (out.result != VK_SUCCESS) return out;
    out.memory_extension = has_extension(extensions, memory_extension);
    out.semaphore_extension = has_extension(extensions, semaphore_extension);
    if (out.memory_extension && vkGetPhysicalDeviceExternalBufferProperties) {
        auto query = [&](VkBufferUsageFlags usage) {
            VkPhysicalDeviceExternalBufferInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
            info.usage = usage;
            info.handleType = handle;
            VkExternalBufferProperties result{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
            vkGetPhysicalDeviceExternalBufferProperties(physical, &info, &result);
            return memory_capability(result.externalMemoryProperties, VK_SUCCESS, handle);
        };
        out.transfer_buffer = query(VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        out.geometry_buffer = query(VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }
    if (out.memory_extension && vkGetPhysicalDeviceImageFormatProperties2) {
        for (auto& image : out.images) {
            VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
            external.handleType = handle;
            VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
            info.pNext = &external;
            info.format = image.format;
            info.type = VK_IMAGE_TYPE_2D;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VkExternalImageFormatProperties external_result{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
            VkImageFormatProperties2 result{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
            result.pNext = &external_result;
            const auto status = vkGetPhysicalDeviceImageFormatProperties2(physical, &info, &result);
            image.memory = memory_capability(external_result.externalMemoryProperties, status, handle);
        }
    }
    if (out.semaphore_extension && vkGetPhysicalDeviceExternalSemaphoreProperties) {
        VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        info.handleType = out.transport == GpuShareTransport::OpaqueWin32
                              ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
                              : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkExternalSemaphoreProperties result{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &info, &result);
        out.binary_semaphore_queried = true;
        out.binary_semaphore_importable = (result.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
        out.binary_semaphore_exportable = (result.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;
    }
    return out;
}

bool valid_gpu_alias_smoke_options(const GpuAliasSmokeOptions& options) {
    constexpr VkDeviceSize maximum_bytes = VkDeviceSize{1} << 20;
    return options.views >= 1 && options.views <= 64 && options.bytes >= 4 && options.bytes <= maximum_bytes &&
           options.bytes % 4 == 0 && options.bytes <= (VkDeviceSize{64} << 20) / options.views;
}

GpuAliasSmokeResult run_gpu_alias_smoke(VkPhysicalDevice physical, GpuAliasSmokeOptions options) {
    GpuAliasSmokeResult out;
    out.views = options.views;
    out.bytes_per_view = options.bytes;
    if (!physical || !valid_gpu_alias_smoke_options(options)) {
        out.error = "invalid physical device or smoke bounds";
        return out;
    }
    SmokeResources resources;
    try {
        VkPhysicalDeviceProperties device_properties{};
        vkGetPhysicalDeviceProperties(physical, &device_properties);
        if (device_properties.apiVersion < VK_API_VERSION_1_1)
            throw SmokeError(VK_ERROR_INCOMPATIBLE_DRIVER, "smoke requires Vulkan 1.1");
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        std::uint32_t family = count;
        for (std::uint32_t i = 0; i < count; ++i)
            if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
        if (family == count) throw SmokeError(VK_ERROR_FEATURE_NOT_PRESENT, "no graphics queue for transfer smoke");
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        std::vector<VkExtensionProperties> extensions;
        check(device_extensions(physical, extensions), "enumerate device extensions");
        const char* portability = "VK_KHR_portability_subset";
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queue_info;
        if (has_extension(extensions, portability)) {
            create.enabledExtensionCount = 1;
            create.ppEnabledExtensionNames = &portability;
        }
        check(vkCreateDevice(physical, &create, nullptr, &resources.device), "create smoke device");
        volkLoadDeviceTable(&resources.api, resources.device);
        auto& api = resources.api;
        if (!api.vkGetBufferMemoryRequirements2)
            throw SmokeError(VK_ERROR_FEATURE_NOT_PRESENT, "memory requirements2 unavailable");
        VkQueue queue = VK_NULL_HANDLE;
        api.vkGetDeviceQueue(resources.device, family, 0, &queue);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = options.bytes;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        resources.views.resize(options.views, VK_NULL_HANDLE);
        std::uint32_t compatible_types = ~std::uint32_t{0};
        VkDeviceSize allocation_bytes = 0;
        for (auto& view : resources.views) {
            check(api.vkCreateBuffer(resources.device, &buffer, nullptr, &view), "create alias buffer");
            VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
            VkMemoryRequirements2 requirement{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
            requirement.pNext = &dedicated;
            VkBufferMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
            info.buffer = view;
            api.vkGetBufferMemoryRequirements2(resources.device, &info, &requirement);
            if (dedicated.requiresDedicatedAllocation)
                throw SmokeError(VK_ERROR_FEATURE_NOT_PRESENT, "buffer requires a dedicated allocation; alias smoke unavailable");
            compatible_types &= requirement.memoryRequirements.memoryTypeBits;
            allocation_bytes = std::max(allocation_bytes, requirement.memoryRequirements.size);
        }
        if (allocation_bytes > (VkDeviceSize{1} << 20))
            throw SmokeError(VK_ERROR_FEATURE_NOT_PRESENT, "device allocation exceeds smoke bound");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = allocation_bytes;
        allocation.memoryTypeIndex = memory_type(memory, compatible_types, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(api.vkAllocateMemory(resources.device, &allocation, nullptr, &resources.memory), "allocate shared device-local memory");
        out.device_allocation_bytes = allocation_bytes;
        for (const auto view : resources.views)
            check(api.vkBindBufferMemory(resources.device, view, resources.memory, 0), "bind alias buffer");
        buffer.size = options.bytes * options.views;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(api.vkCreateBuffer(resources.device, &buffer, nullptr, &resources.readback), "create readback buffer");
        VkMemoryRequirements requirement{};
        api.vkGetBufferMemoryRequirements(resources.device, resources.readback, &requirement);
        if (requirement.size > (VkDeviceSize{64} << 20))
            throw SmokeError(VK_ERROR_FEATURE_NOT_PRESENT, "readback allocation exceeds smoke bound");
        VkMemoryDedicatedAllocateInfo readback_dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        readback_dedicated.buffer = resources.readback;
        allocation.pNext = &readback_dedicated;
        allocation.allocationSize = requirement.size;
        allocation.memoryTypeIndex = memory_type(memory, requirement.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const bool coherent = (memory.memoryTypes[allocation.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        check(api.vkAllocateMemory(resources.device, &allocation, nullptr, &resources.readback_memory), "allocate readback memory");
        out.readback_allocation_bytes = allocation.allocationSize;
        check(api.vkBindBufferMemory(resources.device, resources.readback, resources.readback_memory, 0), "bind readback memory");
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = family;
        check(api.vkCreateCommandPool(resources.device, &pool, nullptr, &resources.pool), "create command pool");
        VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commands.commandPool = resources.pool;
        commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commands.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        check(api.vkAllocateCommandBuffers(resources.device, &commands, &command), "allocate command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(api.vkBeginCommandBuffer(command, &begin), "begin command buffer");
        constexpr std::uint32_t pattern = 0xa1b2c3d4;
        api.vkCmdFillBuffer(command, resources.views.front(), 0, options.bytes, pattern);
        // A global memory dependency covers aliases, unlike a barrier naming just the filled buffer.
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        api.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 1, &barrier, 0, nullptr, 0, nullptr);
        for (std::uint32_t i = 0; i < options.views; ++i) {
            VkBufferCopy copy{0, options.bytes * i, options.bytes};
            api.vkCmdCopyBuffer(command, resources.views[i], resources.readback, 1, &copy);
        }
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        api.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                 1, &barrier, 0, nullptr, 0, nullptr);
        check(api.vkEndCommandBuffer(command), "end command buffer");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(api.vkCreateFence(resources.device, &fence, nullptr, &resources.fence), "create fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        check(api.vkQueueSubmit(queue, 1, &submit, resources.fence), "submit smoke");
        resources.pending = true;
        check(api.vkWaitForFences(resources.device, 1, &resources.fence, VK_TRUE, wait_nanoseconds), "wait for smoke fence");
        resources.pending = false;
        check(api.vkMapMemory(resources.device, resources.readback_memory, 0, VK_WHOLE_SIZE, 0, &resources.mapped), "map readback");
        if (!coherent) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = resources.readback_memory;
            range.size = VK_WHOLE_SIZE;
            check(api.vkInvalidateMappedMemoryRanges(resources.device, 1, &range), "invalidate readback");
        }
        const auto* bytes = static_cast<const std::byte*>(resources.mapped);
        for (VkDeviceSize offset = 0; offset < buffer.size; offset += sizeof(pattern)) {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes + static_cast<std::size_t>(offset), sizeof(value));
            if (value != pattern) throw SmokeError(VK_ERROR_UNKNOWN, "alias readback differs from GPU fill pattern");
            ++out.verified_words;
        }
        out.result = VK_SUCCESS;
        out.matched = true;
    } catch (const SmokeError& error) {
        out.result = error.result;
        out.error = error.what();
    } catch (const std::exception& error) {
        out.error = error.what();
    }
    return out;
}

} // namespace hailstone::render
