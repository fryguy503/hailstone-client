#include "gpu_lab/gpu_cache_broker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <volk.h>

#include "core/byte_io.h"
#include "gpu_lab/gpu_cache_broker_core.h"
#include "platform/gpu_external_memory.h"
#include "platform/gpu_ipc.h"
#include "render/gpu_shared_cache.h"

namespace hailstone::gpu_lab {
namespace {

namespace protocol = render::gpu_cache;
constexpr std::uint64_t kFenceTimeout = 5'000'000'000; // ns

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " (Vulkan result " + std::to_string(result) + ")");
}
std::string json_string(std::string_view value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        if (c == '\\' || c == '"') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + '"';
}

struct OwnedBuffer {
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable* api = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    ~OwnedBuffer() {
        if (mapped) api->vkUnmapMemory(device, memory);
        if (buffer) api->vkDestroyBuffer(device, buffer, nullptr);
        if (memory) api->vkFreeMemory(device, memory, nullptr);
    }
};

struct Submission {
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable* api = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    ~Submission() {
        if (pending) api->vkDeviceWaitIdle(device);
        if (fence) api->vkDestroyFence(device, fence, nullptr);
        if (pool) api->vkDestroyCommandPool(device, pool, nullptr);
    }
    void finish(VkQueue queue) {
        check(api->vkEndCommandBuffer(command), "end GPU cache command buffer");
        VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        command_info.commandBuffer = command;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command_info;
        check(api->vkQueueSubmit2(queue, 1, &submit, fence), "submit GPU cache commands");
        pending = true;
        check(api->vkWaitForFences(device, 1, &fence, VK_TRUE, kFenceTimeout), "wait for GPU cache completion");
        pending = false;
    }
};

// An exported image: the GPU objects and the handle the broker duplicates into each client.
struct ExportedImage {
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable* api = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    platform::TypedGpuIpcHandle handle;
    ~ExportedImage() {
        if (image) api->vkDestroyImage(device, image, nullptr);
        if (memory) api->vkFreeMemory(device, memory, nullptr);
    }
};

struct GpuDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VolkDeviceTable api{};
    VkPhysicalDeviceMemoryProperties memory{};
    protocol::Identity identity;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    std::atomic<std::uint64_t> validation_errors{0};
    std::mutex queue_mutex; // uploads run on the transport's worker threads
    ~GpuDevice() {
        if (device) {
            api.vkDeviceWaitIdle(device);
            api.vkDestroyDevice(device, nullptr);
        }
        if (messenger && vkDestroyDebugUtilsMessengerEXT) vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        auto& errors = static_cast<GpuDevice*>(user)->validation_errors;
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++errors;
        std::fprintf(stderr, "GPU cache validation: %s\n", data && data->pMessage ? data->pMessage : "message unavailable");
        return VK_FALSE;
    }
    void initialize(const GpuCacheBrokerOptions& options) {
        require(platform::gpu_ipc_available(), "shared GPU textures are not available on this platform");
        const char* extension = platform::gpu_external_memory_extension();
        require(extension != nullptr, "external GPU memory is not available on this platform");
        check(volkInitialize(), "initialize Vulkan for GPU cache");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Hailstone GPU cache";
        app.apiVersion = VK_API_VERSION_1_3;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* debug_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        if (options.validation) {
            std::uint32_t count = 0;
            check(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate Vulkan layers");
            std::vector<VkLayerProperties> layers(count);
            check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "enumerate Vulkan layers");
            require(std::any_of(layers.begin(), layers.end(),
                                [layer](const auto& l) { return std::strcmp(l.layerName, layer) == 0; }),
                    "the requested validation layer is unavailable");
            create.enabledLayerCount = 1;
            create.ppEnabledLayerNames = &layer;
            create.enabledExtensionCount = 1;
            create.ppEnabledExtensionNames = &debug_extension;
        }
        check(vkCreateInstance(&create, nullptr, &instance), "create GPU cache Vulkan instance");
        volkLoadInstance(instance);
        if (options.validation) {
            require(vkCreateDebugUtilsMessengerEXT != nullptr, "the validation callback is unavailable");
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = debug;
            info.pUserData = this;
            check(vkCreateDebugUtilsMessengerEXT(instance, &info, nullptr, &messenger), "create the validation callback");
        }
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPU cache adapters");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate GPU cache adapters");
        if (options.device_model.empty()) {
            require(options.device_index < devices.size(), "GPU cache adapter index is unavailable");
            physical = devices[options.device_index];
        } else {
            for (const auto candidate : devices) {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(candidate, &properties);
                if (options.device_model == properties.deviceName) {
                    require(!physical, "GPU cache adapter model is ambiguous");
                    physical = candidate;
                }
            }
            require(physical != VK_NULL_HANDLE, "GPU cache adapter model is unavailable");
        }
        identity = render::gpu_cache_identity(physical);
        require(render::gpu_cache_unique_model(instance, identity.model) && identity.api >= VK_API_VERSION_1_3,
                "GPU cache adapter model is ambiguous or Vulkan 1.3 is unavailable");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> properties(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, properties.data());
        bool found = false;
        for (std::uint32_t i = 0; i < families; ++i)
            if (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                family = i;
                found = true;
                break;
            }
        require(found, "GPU cache graphics queue is unavailable");
        std::uint32_t extensions_count = 0;
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensions_count, nullptr), "enumerate GPU cache extensions");
        std::vector<VkExtensionProperties> extensions(extensions_count);
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensions_count, extensions.data()),
              "enumerate GPU cache extensions");
        require(std::any_of(extensions.begin(), extensions.end(),
                            [extension](const auto& e) { return std::strcmp(e.extensionName, extension) == 0; }),
                "the external memory extension is unavailable");
        VkPhysicalDeviceVulkan13Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &supported;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        require(supported.synchronization2, "synchronization2 is unavailable");
        VkPhysicalDeviceVulkan13Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        enabled.synchronization2 = VK_TRUE;
        VkPhysicalDeviceFeatures base_features{};
        base_features.textureCompressionBC = features.features.textureCompressionBC;
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pNext = &enabled;
        info.pEnabledFeatures = &base_features;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue_info;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &extension;
        check(vkCreateDevice(physical, &info, nullptr, &device), "create GPU cache device");
        volkLoadDeviceTable(&api, device);
        api.vkGetDeviceQueue(device, family, 0, &queue);
    }
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags, VkMemoryPropertyFlags preferred = 0) const {
        for (const auto extra : {preferred, VkMemoryPropertyFlags{0}})
            for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
                if ((bits & (std::uint32_t{1} << i)) && (memory.memoryTypes[i].propertyFlags & (flags | extra)) == (flags | extra))
                    return i;
        throw std::runtime_error("required GPU cache memory type is unavailable");
    }
    void make_buffer(OwnedBuffer& out, std::uint64_t size, VkBufferUsageFlags usage) {
        out.device = device;
        out.api = &api;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        check(api.vkCreateBuffer(device, &info, nullptr, &out.buffer), "create GPU cache transfer buffer");
        VkMemoryRequirements required{};
        api.vkGetBufferMemoryRequirements(device, out.buffer, &required);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = required.size;
        allocation.memoryTypeIndex =
            memory_type(required.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(api.vkAllocateMemory(device, &allocation, nullptr, &out.memory), "allocate GPU cache transfer memory");
        check(api.vkBindBufferMemory(device, out.buffer, out.memory, 0), "bind GPU cache transfer buffer");
        check(api.vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped), "map GPU cache transfer memory");
    }
    void begin(Submission& out) {
        out.device = device;
        out.api = &api;
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = family;
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        check(api.vkCreateCommandPool(device, &pool, nullptr, &out.pool), "create GPU cache command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = out.pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(api.vkAllocateCommandBuffers(device, &allocation, &out.command), "allocate GPU cache command buffer");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(api.vkCreateFence(device, &fence, nullptr, &out.fence), "create GPU cache fence");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(api.vkBeginCommandBuffer(out.command, &info), "begin GPU cache commands");
    }
    void barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                 VkPipelineStageFlags2 source, VkAccessFlags2 source_access, VkPipelineStageFlags2 target,
                 VkAccessFlags2 target_access, std::uint32_t base, std::uint32_t levels,
                 std::uint32_t from = VK_QUEUE_FAMILY_IGNORED, std::uint32_t to = VK_QUEUE_FAMILY_IGNORED) const {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = source;
        b.srcAccessMask = source_access;
        b.dstStageMask = target;
        b.dstAccessMask = target_access;
        b.oldLayout = old_layout;
        b.newLayout = new_layout;
        b.srcQueueFamilyIndex = from;
        b.dstQueueFamilyIndex = to;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, levels, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &b;
        api.vkCmdPipelineBarrier2(command, &dependency);
    }
    // Uploads and exports one texture; null when it cannot be shared. Throws on a GPU failure.
    std::shared_ptr<BrokerImage> upload(const assets::Image& image, bool generate, std::uint64_t maximum_size) {
        if (!render::gpu_cache_external_image_supported(physical, image, generate)) return {};
        auto out = std::make_shared<ExportedImage>();
        out->device = device;
        out->api = &api;
        const auto levels = render::gpu_cache_levels(image, generate);
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = platform::gpu_external_memory_handle_type();
        auto info = render::gpu_cache_image_info(image, generate);
        info.pNext = &external;
        check(api.vkCreateImage(device, &info, nullptr, &out->image), "create exported GPU texture");
        VkMemoryRequirements required{};
        api.vkGetImageMemoryRequirements(device, out->image, &required);
        if (!required.size || required.size > maximum_size) return {};
        const auto memory_index = memory_type(required.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = out->image;
        VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        export_info.pNext = &dedicated;
        export_info.handleTypes = platform::gpu_external_memory_handle_type();
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &export_info;
        allocation.allocationSize = required.size;
        allocation.memoryTypeIndex = memory_index;
        check(api.vkAllocateMemory(device, &allocation, nullptr, &out->memory), "allocate exported GPU texture");
        check(api.vkBindImageMemory(device, out->image, out->memory, 0), "bind exported GPU texture");

        std::vector<std::byte> packed;
        std::vector<VkBufferImageCopy> regions;
        auto width = image.width, height = image.height;
        for (std::uint32_t level = 0; level < image.mips.size(); ++level) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = packed.size();
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            copy.imageExtent = {width, height, 1};
            regions.push_back(copy);
            packed.insert(packed.end(), image.mips[level].begin(), image.mips[level].end());
            packed.resize((packed.size() + 15) & ~std::size_t{15});
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        OwnedBuffer staging;
        make_buffer(staging, packed.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(staging.mapped, packed.data(), packed.size());
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = staging.memory;
        range.size = VK_WHOLE_SIZE;
        check(api.vkFlushMappedMemoryRanges(device, 1, &range), "flush GPU cache texture upload");
        Submission submitted;
        begin(submitted);
        barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, levels);
        api.vkCmdCopyBufferToImage(submitted.command, staging.buffer, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(regions.size()), regions.data());
        if (generate) {
            auto w = static_cast<std::int32_t>(image.width), h = static_cast<std::int32_t>(image.height);
            for (std::uint32_t level = 1; level < levels; ++level) {
                barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT, level - 1, 1);
                const auto next_w = std::max(1, w / 2), next_h = std::max(1, h / 2);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
                blit.srcOffsets[1] = {w, h, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                blit.dstOffsets[1] = {next_w, next_h, 1};
                api.vkCmdBlitImage(submitted.command, out->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT, level - 1, 1);
                w = next_w;
                h = next_h;
            }
            barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT, levels - 1, 1);
        } else {
            barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT, 0, levels);
        }
        barrier(submitted.command, out->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0, 0, levels, family, VK_QUEUE_FAMILY_EXTERNAL);
        {
            std::lock_guard lock(queue_mutex);
            submitted.finish(queue);
        }
        void* handle = nullptr;
        check(platform::gpu_export_memory(device, out->memory, &handle), "export GPU texture memory");
        out->handle = platform::TypedGpuIpcHandle::adopt(handle);
        auto entry = std::make_shared<BrokerImage>();
        entry->allocation_size = required.size;
        entry->memory_index = memory_index;
        entry->levels = levels;
        entry->handle = out->handle.get();
        entry->owner = out;
        return entry;
    }
};

void print_status(const protocol::BrokerStats& stats, protocol::Status status, bool stopping,
                  std::uint64_t validation_errors) {
    std::printf("{\"kind\":\"status\",\"epoch\":%llu,\"consumers\":%u,\"owner_connection\":%llu,"
                "\"allocated_bytes\":%llu,\"entries\":%u,\"requests\":%llu,\"hits\":%llu,\"leases_granted\":%llu,"
                "\"leases_released\":%llu,\"failed_requests\":%llu,\"validation_errors\":%llu,\"stopped\":%s}\n",
                static_cast<unsigned long long>(stats.epoch), stats.consumers, static_cast<unsigned long long>(stats.owner),
                static_cast<unsigned long long>(stats.allocated), stats.entries,
                static_cast<unsigned long long>(stats.requests), static_cast<unsigned long long>(stats.hits),
                static_cast<unsigned long long>(stats.grants), static_cast<unsigned long long>(stats.releases),
                static_cast<unsigned long long>(stats.failed_requests), static_cast<unsigned long long>(validation_errors),
                stopping && status == protocol::Status::Ok ? "true" : "false");
}

} // namespace

int run_gpu_cache_broker(const GpuCacheBrokerOptions& options) {
    require(options.hold_seconds && options.hold_seconds <= 7200, "GPU cache broker lifetime is invalid");
    auto session = platform::gpu_ipc_session_from_environment();
    require(session.has_value(), "GPU cache broker configuration is absent");
    GpuDevice gpu;
    gpu.initialize(options);
    GpuCacheBrokerCore::Options core_options;
    core_options.identity = gpu.identity;
    core_options.epoch = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    core_options.budget_bytes = options.budget_bytes;
    core_options.maximum_entries = options.maximum_entries;
    GpuCacheBrokerCore core(core_options, [&gpu](const assets::Image& image, bool generate, std::uint64_t maximum) {
        return gpu.upload(image, generate, maximum);
    });
    auto server = platform::GpuIpcServer::start(
        *session, [&core](const auto& peer, auto bytes) { return core.handle(peer, bytes); },
        [&core](const auto& peer) { core.disconnect(peer); });
    const auto version = gpu.identity.api;
    std::printf("{\"kind\":\"broker-ready\",\"schema_version\":2,\"device_model\":%s,\"device_api\":\"%u.%u.%u\","
                "\"driver_version\":%u,\"budget_bytes\":%llu,\"maximum_entries\":%u,\"validation_enabled\":%s}\n",
                json_string(gpu.identity.model).c_str(), VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                VK_API_VERSION_PATCH(version), gpu.identity.driver, static_cast<unsigned long long>(options.budget_bytes),
                options.maximum_entries, options.validation ? "true" : "false");
    std::fflush(stdout);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(options.hold_seconds);
    while (!core.stop_requested() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    core.stop();
    server->stop();
    print_status(core.stats(), protocol::Status::Ok, false, gpu.validation_errors.load());
    const bool clean = core.idle_and_empty() && gpu.validation_errors.load() == 0;
    std::printf("{\"kind\":\"broker-complete\",\"clean\":%s}\n", clean ? "true" : "false");
    return clean ? 0 : 1;
}

int run_gpu_cache_control(bool stop_if_idle) {
    auto session = platform::gpu_ipc_session_from_environment();
    require(session.has_value(), "GPU cache broker configuration is absent");
    auto client = platform::GpuIpcClient::connect(*session);
    protocol::Request request;
    request.op = stop_if_idle ? protocol::Op::StopIfIdle : protocol::Op::Status;
    auto reply = client->call(protocol::encode_request(request));
    require(!reply.handle, "GPU cache control returned an unexpected handle");
    ByteReader in(reply.body);
    const auto status = protocol::read_status(in);
    require(status == protocol::Status::Ok || status == protocol::Status::Busy, "GPU cache status request failed");
    print_status(protocol::read_stats(in), status, stop_if_idle, 0);
    return status == protocol::Status::Ok ? 0 : 2;
}

int run_gpu_cache_consumer_proof(const GpuCacheBrokerOptions& options, std::uint32_t iterations) {
    require(iterations >= 1 && iterations <= 4096, "GPU cache proof iteration count is invalid");
    GpuDevice gpu;
    gpu.initialize(options);
    render::GpuSharedCache cache(gpu.physical, gpu.device, gpu.family, gpu.identity.model, gpu.instance);
    assets::Image image;
    image.width = image.height = 16;
    image.mips.emplace_back(16 * 16 * 4);
    for (std::size_t i = 0; i < image.mips.front().size(); ++i)
        image.mips.front()[i] = static_cast<std::byte>((i * 71 + i / 11 + 23) & 255);
    auto texture = cache.texture(image, true);
    require(texture != nullptr, "the generated texture was not shared");
    auto snapshot = cache.snapshot();
    require(snapshot.size() == 1, "the shared texture snapshot differs");
    OwnedBuffer readback;
    gpu.make_buffer(readback, image.mips.front().size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::printf("{\"kind\":\"proof-ready\",\"device_model\":%s,\"iterations\":%u}\n",
                json_string(gpu.identity.model).c_str(), iterations);
    std::fflush(stdout);
    for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
        Submission submitted;
        gpu.begin(submitted);
        cache.record_acquire(submitted.command, snapshot);
        gpu.barrier(submitted.command, texture->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {image.width, image.height, 1};
        gpu.api.vkCmdCopyImageToBuffer(submitted.command, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       readback.buffer, 1, &region);
        gpu.barrier(submitted.command, texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, 0, 1);
        cache.record_release(submitted.command, snapshot);
        VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = readback.buffer;
        host.size = VK_WHOLE_SIZE;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &host;
        gpu.api.vkCmdPipelineBarrier2(submitted.command, &dependency);
        auto lease = cache.acquire_frame();
        require(static_cast<bool>(lease), "the frame lease was not granted");
        submitted.finish(gpu.queue);
        require(lease.finish(), "the frame lease release failed");
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = readback.memory;
        range.size = VK_WHOLE_SIZE;
        check(gpu.api.vkInvalidateMappedMemoryRanges(gpu.device, 1, &range), "invalidate GPU cache proof readback");
        require(std::memcmp(readback.mapped, image.mips.front().data(), image.mips.front().size()) == 0,
                "the shared texture's readback differs");
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    const auto stats = cache.stats();
    std::printf("{\"kind\":\"proof-complete\",\"iterations\":%u,\"imports\":%llu,\"fallbacks\":%llu,\"leases\":%llu,"
                "\"validation_errors\":%llu}\n",
                iterations, static_cast<unsigned long long>(stats.imports), static_cast<unsigned long long>(stats.fallbacks),
                static_cast<unsigned long long>(stats.leases), static_cast<unsigned long long>(gpu.validation_errors.load()));
    return gpu.validation_errors.load() ? 1 : 0;
}

} // namespace hailstone::gpu_lab
