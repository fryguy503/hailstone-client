#include "render/gpu_asset_view_probe.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace hailstone::render {
namespace {
constexpr std::uint64_t maximum_upload_bytes = std::uint64_t{768} << 20;
constexpr std::uint64_t maximum_allocated_bytes = std::uint64_t{3} << 30;
constexpr std::uint64_t fence_timeout = 5'000'000'000;
constexpr std::uint32_t maximum_vertices = 4'000'000, maximum_indices = 12'000'000;
constexpr std::uint32_t maximum_textures = 2048, maximum_draws = 100'000;

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

VkFormat texture_format(assets::PixelFormat format) {
    switch (format) {
    case assets::PixelFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case assets::PixelFormat::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case assets::PixelFormat::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case assets::PixelFormat::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case assets::PixelFormat::BC4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case assets::PixelFormat::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case assets::PixelFormat::BC7: return VK_FORMAT_BC7_UNORM_BLOCK;
    }
    throw std::invalid_argument("unknown texture format");
}

bool same_image(const assets::Image& a, const assets::Image& b) {
    return a.width == b.width && a.height == b.height && a.format == b.format && a.mips == b.mips;
}

void validate(const assets::GpuProbeScene& scene, std::span<const glm::mat4> cameras,
              std::span<const std::uint32_t> vertex, std::span<const std::uint32_t> fragment,
              GpuAssetViewProbeOptions options) {
    require(cameras.size() >= 2 && cameras.size() <= 6, "view count must be 2..6");
    require(options.width && options.height && options.width <= 2048 && options.height <= 2048,
            "view dimensions must be 1..2048");
    require(!scene.vertices.empty() && scene.vertices.size() <= maximum_vertices, "vertex count out of bounds");
    require(!scene.indices.empty() && scene.indices.size() <= maximum_indices, "index count out of bounds");
    require(!scene.textures.empty() && scene.textures.size() <= maximum_textures, "texture count out of bounds");
    require(!scene.draws.empty() && scene.draws.size() <= maximum_draws, "draw count out of bounds");
    for (const auto& matrix : cameras)
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row) require(std::isfinite(matrix[column][row]), "nonfinite camera matrix");
    for (const auto& vertex_value : scene.vertices) {
        for (int i = 0; i < 3; ++i) require(std::isfinite(vertex_value.position[i]), "nonfinite vertex position");
        for (int i = 0; i < 2; ++i) require(std::isfinite(vertex_value.uv[i]), "nonfinite vertex UV");
    }
    for (const auto index : scene.indices) require(index < scene.vertices.size(), "vertex index out of range");
    for (const auto& draw : scene.draws) {
        require(draw.texture < scene.textures.size(), "draw texture out of range");
        require(draw.first_index <= scene.indices.size() && draw.index_count <= scene.indices.size() - draw.first_index &&
                draw.index_count % 3 == 0, "draw index range invalid");
        require(std::isfinite(draw.alpha_cutoff) && draw.alpha_cutoff >= 0.0f && draw.alpha_cutoff <= 1.0f,
                "draw alpha cutoff must be finite and 0..1");
    }
    std::uint64_t image_bytes = 0;
    for (const auto& texture : scene.textures) {
        const auto& image = texture.image;
        require(image.width && image.height && image.width <= 8192 && image.height <= 8192, "texture dimensions out of bounds");
        std::uint32_t maximum_levels = 1;
        for (auto extent = std::max(image.width, image.height); extent > 1; extent /= 2) ++maximum_levels;
        require(!image.mips.empty() && image.mips.size() <= maximum_levels, "mip count out of bounds");
        (void)texture_format(image.format);
        auto width = image.width, height = image.height;
        for (std::size_t mip = 0; mip < image.mips.size(); ++mip) {
            require(image.mips[mip].size() == assets::mip_size(image.format, width, height), "mip byte size mismatch");
            image_bytes += image.mips[mip].size();
            require(image_bytes <= (std::uint64_t{512} << 20), "texture payload exceeds 512 MiB");
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
    }
    for (const auto code : {vertex, fragment})
        require(!code.empty() && code.size() <= (std::size_t{1} << 20) && code.front() == 0x07230203,
                "invalid or oversized SPIR-V payload");
}

std::uint32_t memory_type(const VkPhysicalDeviceMemoryProperties& memory, std::uint32_t bits,
                          VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) {
    for (const auto extra : {preferred, VkMemoryPropertyFlags{0}})
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (std::uint32_t{1} << i)) &&
                (memory.memoryTypes[i].propertyFlags & (required | extra)) == (required | extra)) return i;
    throw std::runtime_error("required GPU memory type unavailable");
}

std::uint64_t append_bytes(std::vector<std::byte>& packed, std::span<const std::byte> bytes) {
    const std::size_t padding = (16 - packed.size() % 16) % 16;
    require(packed.size() + padding <= maximum_upload_bytes && bytes.size() <= maximum_upload_bytes - packed.size() - padding,
            "staging payload exceeds bound");
    packed.resize(packed.size() + padding);
    const auto offset = static_cast<std::uint64_t>(packed.size());
    packed.insert(packed.end(), bytes.begin(), bytes.end());
    return offset;
}
} // namespace

struct GpuAssetViewProbe::State {
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize logical = 0, allocated = 0;
        bool coherent = false;
        void* mapped = nullptr;
    };
    struct Image {
        VkImage handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceSize allocated = 0;
        std::uint32_t width = 0, height = 0, levels = 1;
    };
    struct View {
        std::uint32_t color = 0, depth = 0, readback = 0;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };
    struct UploadImage {
        std::uint32_t image = 0;
        std::vector<VkBufferImageCopy> regions;
    };

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable api{};
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceProperties properties{};
    std::uint32_t family = 0;
    std::uint64_t live_bytes = 0, allocation_count = 0;
    std::vector<Buffer> buffers;
    std::vector<Image> images;
    std::vector<View> views;
    std::vector<glm::mat4> cameras;
    std::vector<assets::GpuProbeDraw> draws;
    std::vector<std::uint32_t> texture_remap;
    std::vector<std::array<std::uint32_t, 2>> geometry;
    std::vector<VkDescriptorSet> descriptors;
    std::uint32_t unique_textures = 0;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkShaderModule vertex_shader = VK_NULL_HANDLE, fragment_shader = VK_NULL_HANDLE;
    GpuAssetViewProbeOptions options;
    GpuAssetViewProbeStats stats;
    std::vector<GpuAssetViewPixels> pixels;
    bool pending = false, failed = false;

    ~State() {
        if (!device) return;
        // A failed bounded fence wait cannot permit destruction of submitted work. Driver teardown can still stall.
        if (pending) api.vkDeviceWaitIdle(device);
        for (auto& view : views) if (view.framebuffer) api.vkDestroyFramebuffer(device, view.framebuffer, nullptr);
        if (pipeline) api.vkDestroyPipeline(device, pipeline, nullptr);
        if (vertex_shader) api.vkDestroyShaderModule(device, vertex_shader, nullptr);
        if (fragment_shader) api.vkDestroyShaderModule(device, fragment_shader, nullptr);
        if (pipeline_layout) api.vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (descriptor_pool) api.vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (descriptor_layout) api.vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
        if (sampler) api.vkDestroySampler(device, sampler, nullptr);
        if (render_pass) api.vkDestroyRenderPass(device, render_pass, nullptr);
        if (fence) api.vkDestroyFence(device, fence, nullptr);
        if (command_pool) api.vkDestroyCommandPool(device, command_pool, nullptr);
        for (auto& image : images) {
            if (image.view) api.vkDestroyImageView(device, image.view, nullptr);
            if (image.handle) api.vkDestroyImage(device, image.handle, nullptr);
            if (image.memory) api.vkFreeMemory(device, image.memory, nullptr);
        }
        for (std::uint32_t i = 0; i < buffers.size(); ++i) destroy_buffer(i);
        api.vkDestroyDevice(device, nullptr);
    }

    void allocation_allowed(VkDeviceSize bytes) const {
        require(bytes && bytes <= maximum_allocated_bytes && live_bytes <= maximum_allocated_bytes - bytes,
                "probe allocation budget exceeds 3 GiB");
        require(allocation_count < properties.limits.maxMemoryAllocationCount, "GPU allocation-count limit exceeded");
    }
    void record(GpuAssetMemoryCategory& category, VkDeviceSize logical, VkDeviceSize allocated) {
        ++category.allocations;
        category.logical_bytes += logical;
        category.allocated_bytes += allocated;
        ++allocation_count;
        live_bytes += allocated;
    }
    std::uint32_t buffer(VkDeviceSize bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                         VkMemoryPropertyFlags preferred, GpuAssetMemoryCategory& category) {
        const auto index = static_cast<std::uint32_t>(buffers.size());
        buffers.emplace_back();
        auto& out = buffers.back();
        out.logical = bytes;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(api.vkCreateBuffer(device, &info, nullptr, &out.handle), "create buffer");
        VkMemoryRequirements requirement{};
        api.vkGetBufferMemoryRequirements(device, out.handle, &requirement);
        allocation_allowed(requirement.size);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.buffer = out.handle;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &dedicated;
        allocation.allocationSize = requirement.size;
        allocation.memoryTypeIndex = memory_type(memory_properties, requirement.memoryTypeBits, required, preferred);
        out.coherent = (memory_properties.memoryTypes[allocation.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        check(api.vkAllocateMemory(device, &allocation, nullptr, &out.memory), "allocate buffer memory");
        out.allocated = requirement.size;
        record(category, bytes, requirement.size);
        check(api.vkBindBufferMemory(device, out.handle, out.memory, 0), "bind buffer memory");
        return index;
    }
    void destroy_buffer(std::uint32_t index) {
        auto& entry = buffers[index];
        if (entry.mapped) api.vkUnmapMemory(device, entry.memory);
        if (entry.handle) api.vkDestroyBuffer(device, entry.handle, nullptr);
        if (entry.memory) {
            api.vkFreeMemory(device, entry.memory, nullptr);
            live_bytes -= entry.allocated;
            --allocation_count;
        }
        entry = {};
    }
    void* map(std::uint32_t index) {
        auto& entry = buffers[index];
        if (!entry.mapped)
            check(api.vkMapMemory(device, entry.memory, 0, VK_WHOLE_SIZE, 0, &entry.mapped), "map host buffer");
        return entry.mapped;
    }
    void host_cache(std::uint32_t index, bool invalidate) {
        const auto& entry = buffers[index];
        if (entry.coherent) return;
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = entry.memory;
        range.size = VK_WHOLE_SIZE;
        if (invalidate) check(api.vkInvalidateMappedMemoryRanges(device, 1, &range), "invalidate readback");
        else check(api.vkFlushMappedMemoryRanges(device, 1, &range), "flush staging");
    }
    std::uint32_t image(std::uint32_t width, std::uint32_t height, std::uint32_t levels, VkFormat format,
                        VkImageUsageFlags usage, VkImageAspectFlags aspect, VkDeviceSize logical,
                        GpuAssetMemoryCategory& category) {
        require(width <= properties.limits.maxImageDimension2D && height <= properties.limits.maxImageDimension2D,
                "image exceeds physical-device dimensions");
        const auto index = static_cast<std::uint32_t>(images.size());
        images.emplace_back();
        auto& out = images.back();
        out.width = width; out.height = height; out.levels = levels;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.extent = {width, height, 1};
        info.mipLevels = levels;
        info.arrayLayers = 1;
        info.format = format;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage = usage;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(api.vkCreateImage(device, &info, nullptr, &out.handle), "create image");
        VkMemoryRequirements requirement{};
        api.vkGetImageMemoryRequirements(device, out.handle, &requirement);
        allocation_allowed(requirement.size);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = out.handle;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &dedicated;
        allocation.allocationSize = requirement.size;
        allocation.memoryTypeIndex = memory_type(memory_properties, requirement.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(api.vkAllocateMemory(device, &allocation, nullptr, &out.memory), "allocate image memory");
        out.allocated = requirement.size;
        record(category, logical, requirement.size);
        check(api.vkBindImageMemory(device, out.handle, out.memory, 0), "bind image memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = out.handle;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {aspect, 0, levels, 0, 1};
        check(api.vkCreateImageView(device, &view, nullptr, &out.view), "create image view");
        return index;
    }
    void begin() {
        check(api.vkResetCommandPool(device, command_pool, 0), "reset command pool");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(api.vkBeginCommandBuffer(command, &info), "begin command buffer");
    }
    void submit() {
        check(api.vkEndCommandBuffer(command), "end command buffer");
        check(api.vkResetFences(device, 1, &fence), "reset fence");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &command;
        check(api.vkQueueSubmit(queue, 1, &info, fence), "submit GPU work");
        pending = true;
        check(api.vkWaitForFences(device, 1, &fence, VK_TRUE, fence_timeout), "wait for GPU completion");
        pending = false;
    }
    void image_barrier(const Image& resource, VkImageLayout before, VkImageLayout after,
                        VkPipelineStageFlags source, VkPipelineStageFlags destination,
                        VkAccessFlags source_access, VkAccessFlags destination_access) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = before;
        barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = resource.handle;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, resource.levels, 0, 1};
        api.vkCmdPipelineBarrier(command, source, destination, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void create_device(VkPhysicalDevice chosen) {
        physical = chosen;
        vkGetPhysicalDeviceProperties(physical, &properties);
        require(properties.apiVersion >= VK_API_VERSION_1_3, "asset-view shaders require Vulkan 1.3");
        require(options.width <= properties.limits.maxViewportDimensions[0] &&
                options.height <= properties.limits.maxViewportDimensions[1], "viewport exceeds device limit");
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        family = count;
        for (std::uint32_t i = 0; i < count; ++i)
            if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
        require(family != count, "graphics queue unavailable");
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features available13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        available.pNext = &available13;
        vkGetPhysicalDeviceFeatures2(physical, &available);
        // glslc's Vulkan 1.3 target emits demote-to-helper for the fragment shader's alpha discard, as in VkContext.
        require(available13.shaderDemoteToHelperInvocation == VK_TRUE,
                "asset-view shader requires shaderDemoteToHelperInvocation");
        VkPhysicalDeviceVulkan13Features enabled13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        enabled13.shaderDemoteToHelperInvocation = VK_TRUE;
        VkPhysicalDeviceFeatures features{};
        features.textureCompressionBC = available.features.textureCompressionBC;
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.pNext = &enabled13;
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queue_info;
        create.pEnabledFeatures = &features;
        std::uint32_t extension_count = 0;
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr), "device extension count");
        std::vector<VkExtensionProperties> extensions(extension_count);
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, extensions.data()), "device extensions");
        const char* portability = "VK_KHR_portability_subset";
        if (std::any_of(extensions.begin(), extensions.end(), [portability](const auto& extension) {
                return std::strcmp(extension.extensionName, portability) == 0;
            })) {
            create.enabledExtensionCount = 1;
            create.ppEnabledExtensionNames = &portability;
        }
        check(vkCreateDevice(physical, &create, nullptr, &device), "create asset-view device");
        volkLoadDeviceTable(&api, device);
        api.vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = family;
        check(api.vkCreateCommandPool(device, &pool, nullptr, &command_pool), "create command pool");
        VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commands.commandPool = command_pool;
        commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commands.commandBufferCount = 1;
        check(api.vkAllocateCommandBuffers(device, &commands, &command), "allocate command buffer");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(api.vkCreateFence(device, &fence_info, nullptr, &fence), "create GPU completion fence");
    }

    void upload(const assets::GpuProbeScene& scene) {
        struct Canonical {
            const assets::Image* source = nullptr;
            std::optional<assets::Image> converted;
            const assets::Image& get() const { return converted ? *converted : *source; }
            std::vector<VkBufferImageCopy> regions;
            std::uint64_t logical = 0;
        };
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(physical, &features);
        std::vector<Canonical> canonical;
        std::map<Sha256Digest, std::uint32_t> by_digest;
        std::vector<std::byte> packed;
        const auto vertex_offset = append_bytes(packed, std::as_bytes(std::span(scene.vertices)));
        const auto index_offset = append_bytes(packed, std::as_bytes(std::span(scene.indices)));
        for (const auto& texture : scene.textures) {
            Canonical entry;
            entry.source = &texture.image;
            VkFormatProperties format{};
            vkGetPhysicalDeviceFormatProperties(physical, texture_format(texture.image.format), &format);
            const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if ((texture.image.format != assets::PixelFormat::RGBA8 && !features.textureCompressionBC) ||
                (format.optimalTilingFeatures & required) != required) {
                require(texture.image.format != assets::PixelFormat::RGBA8, "RGBA8 sampled upload unsupported");
                entry.converted = assets::to_rgba8(texture.image);
                vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format);
                require((format.optimalTilingFeatures & required) == required, "RGBA8 fallback unsupported");
            }
            const auto& image_value = entry.get();
            const auto digest = assets::gpu_probe_texture_digest(image_value);
            if (const auto found = by_digest.find(digest); found != by_digest.end()) {
                require(same_image(canonical[found->second].get(), image_value), "texture content digest collision");
                texture_remap.push_back(found->second);
                continue;
            }
            const auto index = static_cast<std::uint32_t>(canonical.size());
            by_digest.emplace(digest, index);
            texture_remap.push_back(index);
            if (entry.converted) ++stats.bc_fallback_textures;
            for (std::uint32_t level = 0; level < image_value.mips.size(); ++level) {
                VkBufferImageCopy region{};
                region.bufferOffset = append_bytes(packed, image_value.mips[level]);
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                region.imageExtent = {std::max(1u, image_value.width >> level), std::max(1u, image_value.height >> level), 1};
                entry.logical += image_value.mips[level].size();
                entry.regions.push_back(region);
            }
            canonical.push_back(std::move(entry));
        }
        unique_textures = static_cast<std::uint32_t>(canonical.size());
        stats.unique_texture_contents = unique_textures;
        const auto staging = buffer(packed.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stats.staging_peak);
        stats.staging_live_bytes = buffers[staging].allocated;
        std::memcpy(map(staging), packed.data(), packed.size());
        host_cache(staging, false);
        const auto copies = options.share_immutable ? 1u : static_cast<std::uint32_t>(cameras.size());
        std::vector<UploadImage> uploads;
        std::vector<std::uint32_t> texture_images;
        for (std::uint32_t copy = 0; copy < copies; ++copy) {
            const auto vertices = buffer(scene.vertices.size() * sizeof(assets::GpuProbeVertex),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                stats.immutable_geometry);
            const auto indices = buffer(scene.indices.size() * sizeof(std::uint32_t),
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                stats.immutable_geometry);
            geometry.push_back({vertices, indices});
            ++stats.geometry_copies;
            for (const auto& texture : canonical) {
                const auto& content = texture.get();
                const auto resource = image(content.width, content.height, static_cast<std::uint32_t>(content.mips.size()),
                    texture_format(content.format), VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, texture.logical, stats.immutable_textures);
                texture_images.push_back(resource);
                uploads.push_back({resource, texture.regions});
                ++stats.texture_copies;
                ++stats.upload_format_copies[static_cast<std::size_t>(content.format)];
            }
        }
        begin();
        for (const auto& mesh : geometry) {
            VkBufferCopy vertices{vertex_offset, 0, buffers[mesh[0]].logical};
            VkBufferCopy indices{index_offset, 0, buffers[mesh[1]].logical};
            api.vkCmdCopyBuffer(command, buffers[staging].handle, buffers[mesh[0]].handle, 1, &vertices);
            api.vkCmdCopyBuffer(command, buffers[staging].handle, buffers[mesh[1]].handle, 1, &indices);
        }
        for (const auto& upload_image : uploads) {
            const auto& resource = images[upload_image.image];
            image_barrier(resource, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            api.vkCmdCopyBufferToImage(command, buffers[staging].handle, resource.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      static_cast<std::uint32_t>(upload_image.regions.size()), upload_image.regions.data());
            image_barrier(resource, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
        api.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0,
                                 1, &barrier, 0, nullptr, 0, nullptr);
        submit();
        destroy_buffer(staging);
        stats.staging_live_bytes = 0;

        VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.maxLod = VK_LOD_CLAMP_NONE;
        check(api.vkCreateSampler(device, &sampler_info, nullptr, &sampler), "create sampler");
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorCount = 1;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 1;
        layout.pBindings = &binding;
        check(api.vkCreateDescriptorSetLayout(device, &layout, nullptr, &descriptor_layout), "create descriptor layout");
        const auto descriptor_count = static_cast<std::uint32_t>(cameras.size()) * unique_textures;
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, descriptor_count};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &pool_size;
        pool.maxSets = descriptor_count;
        check(api.vkCreateDescriptorPool(device, &pool, nullptr, &descriptor_pool), "create descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts(descriptor_count, descriptor_layout);
        VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sets.descriptorPool = descriptor_pool;
        sets.descriptorSetCount = descriptor_count;
        sets.pSetLayouts = layouts.data();
        descriptors.resize(descriptor_count);
        check(api.vkAllocateDescriptorSets(device, &sets, descriptors.data()), "allocate descriptor sets");
        for (std::uint32_t view = 0; view < cameras.size(); ++view)
            for (std::uint32_t texture = 0; texture < unique_textures; ++texture) {
                const auto copy = options.share_immutable ? 0u : view;
                VkDescriptorImageInfo descriptor{sampler, images[texture_images[copy * unique_textures + texture]].view,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = descriptors[view * unique_textures + texture];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &descriptor;
                api.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
    }

    void targets() {
        for (const auto format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
            VkFormatProperties supported{};
            vkGetPhysicalDeviceFormatProperties(physical, format, &supported);
            if (supported.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                stats.depth_format = format;
                break;
            }
        }
        require(stats.depth_format != VK_FORMAT_UNDEFINED, "depth attachment unavailable");
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        attachments[1] = attachments[0];
        attachments[1].format = stats.depth_format;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        subpass.pDepthStencilAttachment = &depth;
        VkSubpassDependency dependencies[2]{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 2;
        pass.pAttachments = attachments;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 2;
        pass.pDependencies = dependencies;
        check(api.vkCreateRenderPass(device, &pass, nullptr, &render_pass), "create render pass");
        views.resize(cameras.size());
        pixels.resize(cameras.size());
        const VkDeviceSize color_bytes = VkDeviceSize(options.width) * options.height * 4;
        const VkDeviceSize depth_bytes = VkDeviceSize(options.width) * options.height *
                                        (stats.depth_format == VK_FORMAT_D16_UNORM ? 2 : 4);
        for (auto& view : views) {
            view.color = image(options.width, options.height, 1, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                color_bytes, stats.private_targets);
            view.depth = image(options.width, options.height, 1, stats.depth_format,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, depth_bytes, stats.private_targets);
            view.readback = buffer(color_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, stats.readback);
            VkImageView attachments_for_view[]{images[view.color].view, images[view.depth].view};
            VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebuffer.renderPass = render_pass;
            framebuffer.attachmentCount = 2;
            framebuffer.pAttachments = attachments_for_view;
            framebuffer.width = options.width;
            framebuffer.height = options.height;
            framebuffer.layers = 1;
            check(api.vkCreateFramebuffer(device, &framebuffer, nullptr, &view.framebuffer), "create framebuffer");
        }
    }

    void graphics(std::span<const std::uint32_t> vertex, std::span<const std::uint32_t> fragment) {
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = vertex.size_bytes();
        shader.pCode = vertex.data();
        check(api.vkCreateShaderModule(device, &shader, nullptr, &vertex_shader), "create vertex shader");
        shader.codeSize = fragment.size_bytes();
        shader.pCode = fragment.data();
        check(api.vkCreateShaderModule(device, &shader, nullptr, &fragment_shader), "create fragment shader");
        const VkPushConstantRange push[] = {
            {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)},
            {VK_SHADER_STAGE_FRAGMENT_BIT, 64, sizeof(float)},
        };
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &descriptor_layout;
        layout.pushConstantRangeCount = 2;
        layout.pPushConstantRanges = push;
        check(api.vkCreatePipelineLayout(device, &layout, nullptr, &pipeline_layout), "create pipeline layout");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_shader;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_shader;
        stages[1].pName = "main";
        VkVertexInputBindingDescription binding{0, sizeof(assets::GpuProbeVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attributes[]{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(assets::GpuProbeVertex, position)},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(assets::GpuProbeVertex, uv)}};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 2;
        input.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blends{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blends.attachmentCount = 1;
        blends.pAttachments = &blend;
        const VkDynamicState dynamic_states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = &input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState = &viewport;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &samples;
        pipeline_info.pDepthStencilState = &depth;
        pipeline_info.pColorBlendState = &blends;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = pipeline_layout;
        pipeline_info.renderPass = render_pass;
        check(api.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline), "create graphics pipeline");
    }

    void draw(bool read_pixels) {
        begin();
        const VkViewport viewport{0, 0, static_cast<float>(options.width), static_cast<float>(options.height), 0, 1};
        const VkRect2D scissor{{0, 0}, {options.width, options.height}};
        VkClearValue clears[2]{};
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil.depth = 1.0f;
        for (std::uint32_t i = 0; i < views.size(); ++i) {
            const auto& view = views[i];
            VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            pass.renderPass = render_pass;
            pass.framebuffer = view.framebuffer;
            pass.renderArea = scissor;
            pass.clearValueCount = 2;
            pass.pClearValues = clears;
            api.vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
            api.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            api.vkCmdSetViewport(command, 0, 1, &viewport);
            api.vkCmdSetScissor(command, 0, 1, &scissor);
            const auto& mesh = geometry[options.share_immutable ? 0 : i];
            const VkDeviceSize offset = 0;
            api.vkCmdBindVertexBuffers(command, 0, 1, &buffers[mesh[0]].handle, &offset);
            api.vkCmdBindIndexBuffer(command, buffers[mesh[1]].handle, 0, VK_INDEX_TYPE_UINT32);
            api.vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &cameras[i]);
            for (const auto& batch : draws) {
                if (!batch.index_count) continue;
                api.vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       64, sizeof(float), &batch.alpha_cutoff);
                const auto descriptor = descriptors[i * unique_textures + texture_remap[batch.texture]];
                api.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &descriptor, 0, nullptr);
                api.vkCmdDrawIndexed(command, batch.index_count, 1, batch.first_index, 0, 0);
            }
            api.vkCmdEndRenderPass(command);
            if (read_pixels) {
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {options.width, options.height, 1};
                api.vkCmdCopyImageToBuffer(command, images[view.color].handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           buffers[view.readback].handle, 1, &copy);
            }
        }
        if (read_pixels) {
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            api.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                     1, &barrier, 0, nullptr, 0, nullptr);
        }
        submit();
        ++stats.frames_rendered;
        if (!read_pixels) return;
        for (std::size_t i = 0; i < views.size(); ++i) {
            auto& result = pixels[i];
            result.width = options.width;
            result.height = options.height;
            result.rgba.resize(static_cast<std::size_t>(buffers[views[i].readback].logical));
            const auto* mapped = map(views[i].readback);
            host_cache(views[i].readback, true);
            std::memcpy(result.rgba.data(), mapped, result.rgba.size());
            result.hash = sha256(std::span<const std::byte>(result.rgba));
            result.non_background_pixels = 0;
            for (std::size_t pixel = 0; pixel < result.rgba.size(); pixel += 4)
                if (result.rgba[pixel] != std::byte{0} || result.rgba[pixel + 1] != std::byte{0} ||
                    result.rgba[pixel + 2] != std::byte{0} || result.rgba[pixel + 3] != std::byte{255})
                    ++result.non_background_pixels;
        }
    }
};

GpuAssetViewProbe::GpuAssetViewProbe(std::unique_ptr<State> state) : state_(std::move(state)) {}
GpuAssetViewProbe::GpuAssetViewProbe(GpuAssetViewProbe&&) noexcept = default;
GpuAssetViewProbe& GpuAssetViewProbe::operator=(GpuAssetViewProbe&&) noexcept = default;
GpuAssetViewProbe::~GpuAssetViewProbe() = default;

bool validate_gpu_asset_view_probe_inputs(const assets::GpuProbeScene& scene,
    std::span<const glm::mat4> view_projections, std::span<const std::uint32_t> vertex_spirv,
    std::span<const std::uint32_t> fragment_spirv, GpuAssetViewProbeOptions options, std::string* error) {
    try {
        validate(scene, view_projections, vertex_spirv, fragment_spirv, options);
        if (error) error->clear();
        return true;
    } catch (const std::exception& failure) {
        if (error) *error = failure.what();
        return false;
    }
}

std::optional<GpuAssetViewProbe> GpuAssetViewProbe::create(VkPhysicalDevice physical, const assets::GpuProbeScene& scene,
    std::span<const glm::mat4> view_projections, std::span<const std::uint32_t> vertex_spirv,
    std::span<const std::uint32_t> fragment_spirv, GpuAssetViewProbeOptions options, std::string* error) {
    if (!validate_gpu_asset_view_probe_inputs(scene, view_projections, vertex_spirv, fragment_spirv, options, error))
        return std::nullopt;
    try {
        require(physical && vkCreateDevice && vkGetPhysicalDeviceProperties, "Vulkan instance/device unavailable");
        auto state = std::make_unique<State>();
        state->options = options;
        state->cameras.assign(view_projections.begin(), view_projections.end());
        state->draws = scene.draws;
        state->create_device(physical);
        state->upload(scene);
        state->targets();
        state->graphics(vertex_spirv, fragment_spirv);
        return GpuAssetViewProbe(std::move(state));
    } catch (const std::exception& failure) {
        if (error) *error = failure.what();
        return std::nullopt;
    }
}

bool GpuAssetViewProbe::render(std::string* error, bool read_pixels) {
    if (!state_ || state_->failed) {
        if (error) *error = "asset-view probe is unavailable after a failed or moved owner";
        return false;
    }
    try {
        state_->draw(read_pixels);
        return true;
    } catch (const std::exception& failure) {
        state_->failed = true;
        if (error) *error = failure.what();
        return false;
    }
}
const std::vector<GpuAssetViewPixels>& GpuAssetViewProbe::pixels() const { return state_->pixels; }
const GpuAssetViewProbeStats& GpuAssetViewProbe::stats() const { return state_->stats; }

} // namespace hailstone::render
