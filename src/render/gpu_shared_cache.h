#pragma once

// Shared GPU textures, the client's side (docs/graphics.md "Shared GPU textures"): an immutable texture's memory
// comes from the broker (tools/gpu_lab) as an imported handle instead of a private upload, so several clients on one
// machine hold one copy. Experimental and opt-in (HAILSTONE_GPU_IPC_*), Windows only. Every client keeps its own
// device, UI and presentation; only the payload is shared, and only while the broker is reachable: the link
// (render/gpu_share_link.h) detaches on any failure, and the client copies its imports into private images and plays on.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <volk.h>
#include <vk_mem_alloc.h>

#include "assets/texture.h"
#include "render/gpu_cache_protocol.h"

namespace hailstone::render {

// An imported texture, or after localisation a private copy of one. Holders keep `image`/`view` copies
// (render/gpu.h Texture), refreshed by refresh_shared_texture after GpuSharedCache::localize.
struct GpuSharedTexture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width = 0, height = 0, mip_levels = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool imported() const;

private:
    struct Storage;
    std::shared_ptr<Storage> storage_;
    friend class GpuSharedCache;
};

// The Vulkan side shared with the broker tool: both must create the image the same way.
VkFormat gpu_cache_format(assets::PixelFormat format);
std::uint32_t gpu_cache_levels(const assets::Image& image, bool generate_mips);
VkImageCreateInfo gpu_cache_image_info(const assets::Image& image, bool generate_mips);
bool gpu_cache_external_image_supported(VkPhysicalDevice physical, const assets::Image& image, bool generate_mips);
gpu_cache::Identity gpu_cache_identity(VkPhysicalDevice physical);
// Whether exactly one adapter has this model name (the identity is matched by name across processes).
bool gpu_cache_unique_model(VkInstance instance, const std::string& model);

class GpuSharedCache {
    struct Impl;

public:
    struct Stats {
        std::uint64_t requests = 0, imports = 0, local_hits = 0, broker_hits = 0;
        std::uint64_t imported_allocation_bytes = 0, fallbacks = 0, leases = 0, localized = 0;
    };
    // The frame lease: held from the submit of a frame that samples imports until its fence. A lease dropped
    // without finish() detaches the link (the broker then releases it).
    class FrameLease {
    public:
        FrameLease() = default;
        ~FrameLease();
        FrameLease(FrameLease&& other) noexcept;
        FrameLease& operator=(FrameLease&& other) noexcept;
        FrameLease(const FrameLease&) = delete;
        FrameLease& operator=(const FrameLease&) = delete;
        // False when the release failed (the link has detached).
        bool finish();
        explicit operator bool() const { return owner_ != nullptr; }

    private:
        FrameLease(std::shared_ptr<Impl> owner, std::uint64_t serial);
        std::shared_ptr<Impl> owner_;
        std::uint64_t serial_ = 0;
        friend class GpuSharedCache;
    };
    using Snapshot = std::vector<std::shared_ptr<GpuSharedTexture>>;

    // Whether HAILSTONE_GPU_IPC_* asks for sharing.
    static bool requested();
    static const char* device_extension();
    // Connects and shakes hands; throws when sharing cannot start (the caller keeps private textures).
    GpuSharedCache(VkPhysicalDevice physical, VkDevice device, std::uint32_t queue_family, const std::string& gpu_name,
                   VkInstance instance);
    ~GpuSharedCache();
    GpuSharedCache(const GpuSharedCache&) = delete;
    GpuSharedCache& operator=(const GpuSharedCache&) = delete;

    // The shared texture, or null: upload this one privately. Never throws.
    std::shared_ptr<GpuSharedTexture> texture(const assets::Image& image, bool generate_mips);
    // The live imports (not those localized), for this frame's ownership barriers.
    Snapshot snapshot() const;
    // An empty lease when the lease was not granted; the link has then detached.
    FrameLease acquire_frame();
    void record_acquire(VkCommandBuffer command, const Snapshot& textures) const;
    void record_release(VkCommandBuffer command, const Snapshot& textures) const;
    bool active() const;
    std::string detach_reason() const;
    void keep_alive();
    // Detached with imports still alive: localize() should run before the next frame records.
    bool needs_localize() const;
    // Copies every live import into a private image and moves each GpuSharedTexture onto it, then frees the
    // imports. Call with the device idle; waits for its own copy. Returns how many moved; one that cannot be copied
    // keeps its import (still valid: the memory holds its own reference to the payload).
    std::size_t localize(VkQueue queue, VmaAllocator allocator);
    // Bumped by each localize(): holders refresh their image/view copies.
    std::uint64_t generation() const;
    Stats stats() const;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace hailstone::render
