#include "render/gpu_shared_cache.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "core/byte_io.h"
#include "core/log.h"
#include "platform/gpu_external_memory.h"
#include "platform/gpu_ipc.h"
#include "render/gpu_share_link.h"

namespace hailstone::render {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " (Vulkan result " + std::to_string(result) + ")");
}

VkImageUsageFlags shared_usage() {
    // Transfer source on every shared image, so a client that loses the broker can copy its imports into private
    // images (GpuSharedCache::localize); mip generation blits from it too.
    return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
}

VkImageView make_view(VkDevice device, const VolkDeviceTable& api, VkImage image, VkFormat format,
                      std::uint32_t levels) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    check(api.vkCreateImageView(device, &info, nullptr, &view), "create shared texture view");
    return view;
}

VkImageMemoryBarrier2 image_barrier2(VkImage image, std::uint32_t levels, VkImageLayout from, VkImageLayout to,
                                     VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                     VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                                     std::uint32_t src_family = VK_QUEUE_FAMILY_IGNORED,
                                     std::uint32_t dst_family = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = src_family;
    b.dstQueueFamilyIndex = dst_family;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    return b;
}

void pipeline_barrier(const VolkDeviceTable& api, VkCommandBuffer command,
                      const std::vector<VkImageMemoryBarrier2>& barriers) {
    if (barriers.empty()) return;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers = barriers.data();
    api.vkCmdPipelineBarrier2(command, &dependency);
}

} // namespace

VkFormat gpu_cache_format(assets::PixelFormat format) {
    switch (format) {
    case assets::PixelFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case assets::PixelFormat::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case assets::PixelFormat::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case assets::PixelFormat::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case assets::PixelFormat::BC4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case assets::PixelFormat::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case assets::PixelFormat::BC7: return VK_FORMAT_BC7_UNORM_BLOCK;
    }
    return VK_FORMAT_UNDEFINED;
}

std::uint32_t gpu_cache_levels(const assets::Image& image, bool generate_mips) {
    return generate_mips ? static_cast<std::uint32_t>(std::bit_width(std::max(image.width, image.height)))
                         : static_cast<std::uint32_t>(image.mips.size());
}

VkImageCreateInfo gpu_cache_image_info(const assets::Image& image, bool generate_mips) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = gpu_cache_format(image.format);
    info.extent = {image.width, image.height, 1};
    info.mipLevels = gpu_cache_levels(image, generate_mips);
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = shared_usage();
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

bool gpu_cache_external_image_supported(VkPhysicalDevice physical, const assets::Image& image, bool generate_mips) {
    const VkExternalMemoryHandleTypeFlagBits handle_type = platform::gpu_external_memory_handle_type();
    if (!handle_type) return false;
    VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    external.handleType = handle_type;
    VkPhysicalDeviceImageFormatInfo2 input{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    input.pNext = &external;
    input.format = gpu_cache_format(image.format);
    input.type = VK_IMAGE_TYPE_2D;
    input.tiling = VK_IMAGE_TILING_OPTIMAL;
    input.usage = shared_usage();
    VkExternalImageFormatProperties external_properties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 output{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    output.pNext = &external_properties;
    if (vkGetPhysicalDeviceImageFormatProperties2(physical, &input, &output) != VK_SUCCESS) return false;
    const auto& capability = external_properties.externalMemoryProperties;
    const auto required = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
    const auto levels = gpu_cache_levels(image, generate_mips);
    if ((capability.externalMemoryFeatures & required) != required ||
        !(capability.compatibleHandleTypes & handle_type) ||
        output.imageFormatProperties.maxExtent.width < image.width ||
        output.imageFormatProperties.maxExtent.height < image.height ||
        output.imageFormatProperties.maxMipLevels < levels)
        return false;
    if (generate_mips && levels > 1) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical, input.format, &properties);
        const auto features = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & features) != features) return false;
    }
    return true;
}

gpu_cache::Identity gpu_cache_identity(VkPhysicalDevice physical) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    ByteWriter encoded;
    encoded.le(memory.memoryTypeCount);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        encoded.le(memory.memoryTypes[i].propertyFlags);
        encoded.le(memory.memoryTypes[i].heapIndex);
    }
    encoded.le(memory.memoryHeapCount);
    for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        encoded.le<std::uint64_t>(memory.memoryHeaps[i].size);
        encoded.le(memory.memoryHeaps[i].flags);
    }
    return {properties.deviceName, properties.apiVersion, properties.driverVersion, sha256(encoded.data())};
}

bool gpu_cache_unique_model(VkInstance instance, const std::string& model) {
    std::uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) return false;
    std::size_t matches = 0;
    for (const auto device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (model == properties.deviceName) ++matches;
    }
    return matches == 1;
}

// The texture's Vulkan objects: an import (image, dedicated imported memory) or, after localize(), a private image.
struct GpuSharedTexture::Storage {
    VkDevice device = VK_NULL_HANDLE;
    // By value: a texture may outlive the cache that made it.
    PFN_vkDestroyImageView destroy_view = nullptr;
    PFN_vkDestroyImage destroy_image = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE; // imported
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE; // private, after localize()
    VmaAllocation allocation = VK_NULL_HANDLE;
    // Tells the broker this client no longer uses it, while the import is alive and the link connected.
    std::function<void()> retire;

    bool imported() const { return memory != VK_NULL_HANDLE; }
    void destroy_objects() noexcept {
        if (view) destroy_view(device, view, nullptr);
        if (allocation) vmaDestroyImage(allocator, image, allocation);
        else if (image) destroy_image(device, image, nullptr);
        if (memory) free_memory(device, memory, nullptr);
        view = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
    }
    ~Storage() {
        const bool was_imported = imported();
        if (device) destroy_objects();
        if (was_imported && retire) retire();
    }
};

bool GpuSharedTexture::imported() const { return storage_ && storage_->imported(); }

struct GpuSharedCache::Impl {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VolkDeviceTable api{};
    std::uint32_t queue_family = 0;
    std::unique_ptr<platform::GpuIpcClient> channel;
    std::unique_ptr<GpuShareLink> link;
    mutable std::mutex mutex;
    mutable std::map<Sha256Digest, std::weak_ptr<GpuSharedTexture>> textures;
    Stats counters;
    std::uint64_t generation = 0;
    bool localize_failed = false;
};

bool GpuSharedCache::requested() {
    try {
        return platform::gpu_ipc_session_from_environment().has_value();
    } catch (...) {
        return true; // a malformed configuration asked for it; the connection then fails and textures stay private
    }
}

const char* GpuSharedCache::device_extension() { return platform::gpu_external_memory_extension(); }

GpuSharedCache::GpuSharedCache(VkPhysicalDevice physical, VkDevice device, std::uint32_t queue_family,
                               const std::string& gpu_name, VkInstance instance)
    : impl_(std::make_shared<Impl>()) {
    require(platform::gpu_ipc_available() && platform::gpu_external_memory_handle_type(),
            "shared GPU textures are not available on this platform");
    auto session = platform::gpu_ipc_session_from_environment();
    require(session.has_value(), "shared GPU textures are not configured");
    impl_->physical = physical;
    impl_->device = device;
    impl_->queue_family = queue_family;
    volkLoadDeviceTable(&impl_->api, device);
    const auto identity = gpu_cache_identity(physical);
    require(identity.model == gpu_name && instance && gpu_cache_unique_model(instance, gpu_name),
            "the GPU's model name is ambiguous");
    impl_->channel = platform::GpuIpcClient::connect(*session);
    platform::GpuIpcClient* channel = impl_->channel.get();
    impl_->link = std::make_unique<GpuShareLink>(
        [channel](std::span<const std::byte> body, int timeout_ms) { return channel->call(body, timeout_ms); },
        [channel] { channel->close(); });
    if (!impl_->link->hello(identity)) throw std::runtime_error(impl_->link->detach_reason());
}

GpuSharedCache::~GpuSharedCache() {
    if (impl_ && impl_->link) impl_->link->detach("closed");
}

bool GpuSharedCache::active() const { return impl_->link->connected(); }
std::string GpuSharedCache::detach_reason() const { return impl_->link->detach_reason(); }
void GpuSharedCache::keep_alive() { impl_->link->keep_alive(); }

GpuSharedCache::Stats GpuSharedCache::stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->counters;
}

std::uint64_t GpuSharedCache::generation() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->generation;
}

std::shared_ptr<GpuSharedTexture> GpuSharedCache::texture(const assets::Image& image, bool generate_mips) {
    std::lock_guard lock(impl_->mutex);
    ++impl_->counters.requests;
    if (!impl_->link->connected() || !gpu_cache::validate_texture(image, generate_mips) ||
        !gpu_cache_external_image_supported(impl_->physical, image, generate_mips)) {
        ++impl_->counters.fallbacks;
        return {};
    }
    std::optional<Sha256Digest> granted_digest; // the broker holds a reference for this client
    try {
        const auto digest = gpu_cache::texture_key(image, generate_mips);
        if (auto found = impl_->textures.find(digest); found != impl_->textures.end())
            if (auto alive = found->second.lock(); alive && alive->imported()) {
                ++impl_->counters.local_hits;
                return alive;
            }
        const auto recipe = gpu_cache::encode_texture(image, generate_mips);
        auto granted = impl_->link->texture(digest, recipe);
        if (!granted) {
            ++impl_->counters.fallbacks;
            return {};
        }
        granted_digest = digest;
        const auto levels = gpu_cache_levels(image, generate_mips);
        require(granted->grant.levels == levels, "the broker's texture has other mip levels");

        auto storage = std::make_shared<GpuSharedTexture::Storage>();
        storage->device = impl_->device;
        storage->destroy_view = impl_->api.vkDestroyImageView;
        storage->destroy_image = impl_->api.vkDestroyImage;
        storage->free_memory = impl_->api.vkFreeMemory;
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = platform::gpu_external_memory_handle_type();
        auto info = gpu_cache_image_info(image, generate_mips);
        info.pNext = &external;
        check(impl_->api.vkCreateImage(impl_->device, &info, nullptr, &storage->image), "create imported texture");
        VkMemoryRequirements requirements{};
        impl_->api.vkGetImageMemoryRequirements(impl_->device, storage->image, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(impl_->physical, &memory);
        const auto type = granted->grant.memory_index;
        require(type < memory.memoryTypeCount && (requirements.memoryTypeBits & (std::uint32_t{1} << type)) &&
                    requirements.size == granted->grant.allocation_size &&
                    (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
                "the imported texture's memory requirements differ");
        check(platform::gpu_import_memory(impl_->device, impl_->api.vkAllocateMemory, storage->image,
                                          granted->grant.allocation_size, type, granted->handle.get(),
                                          &storage->memory),
              "import texture memory");
        check(impl_->api.vkBindImageMemory(impl_->device, storage->image, storage->memory, 0), "bind imported texture");
        storage->view = make_view(impl_->device, impl_->api, storage->image, info.format, levels);
        storage->retire = [weak = std::weak_ptr<Impl>(impl_), digest] {
            if (auto cache = weak.lock()) cache->link->retire(digest);
        };
        granted_digest.reset(); // the storage gives it back from here on

        auto texture = std::make_shared<GpuSharedTexture>();
        texture->image = storage->image;
        texture->view = storage->view;
        texture->width = image.width;
        texture->height = image.height;
        texture->mip_levels = levels;
        texture->format = info.format;
        texture->storage_ = std::move(storage);
        impl_->textures[digest] = texture;
        ++impl_->counters.imports;
        impl_->counters.imported_allocation_bytes += granted->grant.allocation_size;
        if (granted->grant.hit) ++impl_->counters.broker_hits;
        return texture;
    } catch (const std::exception& e) {
        // Only this texture fails: it is uploaded privately and the broker's reference for it given back.
        log::warn("GPU asset sharing: texture kept private ({})", e.what());
    } catch (...) {
    }
    if (granted_digest) impl_->link->retire(*granted_digest);
    ++impl_->counters.fallbacks;
    return {};
}

GpuSharedCache::Snapshot GpuSharedCache::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    Snapshot textures;
    for (auto it = impl_->textures.begin(); it != impl_->textures.end();) {
        auto texture = it->second.lock();
        if (!texture) {
            it = impl_->textures.erase(it);
            continue;
        }
        if (texture->imported()) textures.push_back(std::move(texture));
        ++it;
    }
    return textures;
}

bool GpuSharedCache::needs_localize() const {
    if (impl_->link->connected()) return false;
    std::lock_guard lock(impl_->mutex);
    if (impl_->localize_failed) return false;
    return std::any_of(impl_->textures.begin(), impl_->textures.end(), [](const auto& entry) {
        auto texture = entry.second.lock();
        return texture && texture->imported();
    });
}

GpuSharedCache::FrameLease GpuSharedCache::acquire_frame() {
    const auto serial = impl_->link->acquire();
    if (!serial) return {};
    std::lock_guard lock(impl_->mutex);
    ++impl_->counters.leases;
    return FrameLease(impl_, *serial);
}

namespace {
void ownership_barriers(const VolkDeviceTable& api, VkCommandBuffer command, const GpuSharedCache::Snapshot& textures,
                        std::uint32_t family, bool acquire) {
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(textures.size());
    for (const auto& texture : textures) {
        if (acquire)
            barriers.push_back(image_barrier2(texture->image, texture->mip_levels,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                                              VK_QUEUE_FAMILY_EXTERNAL, family));
        else
            barriers.push_back(image_barrier2(texture->image, texture->mip_levels,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                              VK_PIPELINE_STAGE_2_NONE, 0, family, VK_QUEUE_FAMILY_EXTERNAL));
    }
    pipeline_barrier(api, command, barriers);
}
} // namespace

void GpuSharedCache::record_acquire(VkCommandBuffer command, const Snapshot& textures) const {
    ownership_barriers(impl_->api, command, textures, impl_->queue_family, true);
}

void GpuSharedCache::record_release(VkCommandBuffer command, const Snapshot& textures) const {
    ownership_barriers(impl_->api, command, textures, impl_->queue_family, false);
}

std::size_t GpuSharedCache::localize(VkQueue queue, VmaAllocator allocator) {
    std::lock_guard lock(impl_->mutex);
    struct Move {
        std::shared_ptr<GpuSharedTexture> texture;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
    };
    std::vector<Move> moves;
    for (const auto& entry : impl_->textures)
        if (auto texture = entry.second.lock(); texture && texture->imported()) moves.push_back({std::move(texture)});
    if (moves.empty()) return 0;

    const VolkDeviceTable& api = impl_->api;
    const VkDevice device = impl_->device;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    try {
        for (auto& m : moves) {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = m.texture->format;
            info.extent = {m.texture->width, m.texture->height, 1};
            info.mipLevels = m.texture->mip_levels;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (vmaCreateImage(allocator, &info, &ai, &m.image, &m.allocation, nullptr) != VK_SUCCESS) {
                m.image = VK_NULL_HANDLE; // this one keeps its import
                m.allocation = VK_NULL_HANDLE;
            }
        }
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = impl_->queue_family;
        check(api.vkCreateCommandPool(device, &pool_info, nullptr, &pool), "create localisation command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        check(api.vkAllocateCommandBuffers(device, &allocation, &command), "allocate localisation commands");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(api.vkCreateFence(device, &fence_info, nullptr, &fence), "create localisation fence");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(api.vkBeginCommandBuffer(command, &begin), "begin localisation commands");

        // Each import comes from the external owner as every frame's acquire takes it, then is read once.
        std::vector<VkImageMemoryBarrier2> acquire, before, after;
        for (const auto& m : moves) {
            if (!m.image) continue;
            const auto& t = *m.texture;
            acquire.push_back(image_barrier2(t.image, t.mip_levels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                                             VK_QUEUE_FAMILY_EXTERNAL, impl_->queue_family));
            before.push_back(image_barrier2(t.image, t.mip_levels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                            VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                                            VK_ACCESS_2_TRANSFER_READ_BIT));
            before.push_back(image_barrier2(m.image, t.mip_levels, VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                                            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT));
            after.push_back(image_barrier2(m.image, t.mip_levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
        }
        pipeline_barrier(api, command, acquire);
        pipeline_barrier(api, command, before);
        for (const auto& m : moves) {
            if (!m.image) continue;
            const auto& t = *m.texture;
            std::vector<VkImageCopy> regions;
            for (std::uint32_t level = 0; level < t.mip_levels; ++level) {
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                region.dstSubresource = region.srcSubresource;
                region.extent = {std::max(1u, t.width >> level), std::max(1u, t.height >> level), 1};
                regions.push_back(region);
            }
            api.vkCmdCopyImage(command, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()),
                               regions.data());
        }
        pipeline_barrier(api, command, after);
        check(api.vkEndCommandBuffer(command), "end localisation commands");
        VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        command_info.commandBuffer = command;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command_info;
        check(api.vkQueueSubmit2(queue, 1, &submit, fence), "submit localisation commands");
    } catch (const std::exception& e) {
        log::warn("GPU asset sharing: could not copy the shared textures ({}); keeping the imports", e.what());
        for (auto& m : moves)
            if (m.allocation) vmaDestroyImage(allocator, m.image, m.allocation);
        if (fence) api.vkDestroyFence(device, fence, nullptr);
        if (pool) api.vkDestroyCommandPool(device, pool, nullptr);
        impl_->localize_failed = true;
        return 0;
    }
    constexpr std::uint64_t kCopyTimeout = 10'000'000'000; // ns
    if (api.vkWaitForFences(device, 1, &fence, VK_TRUE, kCopyTimeout) != VK_SUCCESS) {
        // The copy may still run, so its objects are left alone (a one-off leak), and the imports kept: they stay
        // valid without the broker.
        log::warn("GPU asset sharing: copying the shared textures did not finish; keeping the imports");
        impl_->localize_failed = true;
        return 0;
    }
    api.vkDestroyFence(device, fence, nullptr);
    api.vkDestroyCommandPool(device, pool, nullptr);

    std::size_t moved = 0;
    for (auto& m : moves) {
        if (!m.image) continue;
        GpuSharedTexture& t = *m.texture;
        VkImageView view = VK_NULL_HANDLE;
        try {
            view = make_view(device, api, m.image, t.format, t.mip_levels);
        } catch (...) {
            vmaDestroyImage(allocator, m.image, m.allocation);
            continue;
        }
        auto& storage = *t.storage_;
        storage.retire = {}; // the broker let go of this client's references when the link detached
        storage.destroy_objects();
        storage.image = m.image;
        storage.allocator = allocator;
        storage.allocation = m.allocation;
        storage.view = view;
        t.image = m.image;
        t.view = view;
        ++moved;
    }
    ++impl_->generation;
    impl_->counters.localized += moved;
    if (moved != moves.size()) impl_->localize_failed = true; // the rest keep their imports
    return moved;
}

GpuSharedCache::FrameLease::FrameLease(std::shared_ptr<Impl> owner, std::uint64_t serial)
    : owner_(std::move(owner)), serial_(serial) {}

GpuSharedCache::FrameLease::~FrameLease() {
    if (owner_) owner_->link->detach("a frame lease was dropped without its release");
}

GpuSharedCache::FrameLease::FrameLease(FrameLease&& other) noexcept
    : owner_(std::move(other.owner_)), serial_(std::exchange(other.serial_, 0)) {}

GpuSharedCache::FrameLease& GpuSharedCache::FrameLease::operator=(FrameLease&& other) noexcept {
    if (this != &other) {
        if (owner_) owner_->link->detach("a frame lease was dropped without its release");
        owner_ = std::move(other.owner_);
        serial_ = std::exchange(other.serial_, 0);
    }
    return *this;
}

bool GpuSharedCache::FrameLease::finish() {
    if (!owner_) return false;
    const bool released = owner_->link->release(serial_);
    owner_.reset();
    serial_ = 0;
    return released;
}

} // namespace hailstone::render
