#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <volk.h>

#include "assets/texture.h"
#include "core/sha256.h"

namespace hailstone::render {

// The content is the FINAL image after colour preparation, mip dropping and device-format fallback.
// Recipes and the lease model are GPU-free so malformed IPC and owner-loss paths can be tested directly.
constexpr std::uint64_t kGpuCacheMaximumTextureBytes = std::uint64_t{128} << 20;
constexpr std::uint64_t kGpuCacheMaximumBudget = std::uint64_t{2} << 30;
constexpr std::uint32_t kGpuCacheMaximumEntries = 2048;
bool validate_gpu_cache_texture(const assets::Image& image, bool generate_mips) noexcept;
Sha256Digest gpu_cache_texture_key(const assets::Image& image, bool generate_mips);
std::vector<std::byte> encode_gpu_cache_texture(const assets::Image& image, bool generate_mips);
struct GpuCacheTextureRecipe { assets::Image image; bool generate_mips = false; };
GpuCacheTextureRecipe decode_gpu_cache_texture(std::span<const std::byte> packet);

class GpuCacheLeaseState {
public:
    void register_peer(std::uint64_t peer);
    std::optional<std::uint64_t> acquire(std::uint64_t peer);
    bool release(std::uint64_t peer, std::uint64_t serial);
    void disconnect(std::uint64_t peer);
    void poison() { poisoned_ = true; }
    bool poisoned() const { return poisoned_; }
    std::uint64_t owner() const { return owner_; }
    std::uint64_t serial() const { return serial_; }
    std::size_t peer_count() const { return peers_.size(); }
    bool has_peer(std::uint64_t peer) const;
private:
    std::vector<std::uint64_t> peers_;
    std::uint64_t owner_ = 0, serial_ = 0;
    bool poisoned_ = false;
};

struct GpuSharedTexture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width = 0, height = 0, mip_levels = 0;
private:
    struct Storage;
    std::shared_ptr<Storage> storage_;
    std::string key_;
    friend class GpuSharedCache;
};

class GpuSharedCacheFatal : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Experimental, opt-in external textures. Every client keeps its own device, UI and presentation resources.
// A lease never grants concurrent instance ownership. Its explicit finish follows THIS frame's GPU fence.
class GpuSharedCache {
    struct Impl;
public:
    struct Stats {
        std::uint64_t requests = 0, imports = 0, local_hits = 0, broker_hits = 0;
        std::uint64_t imported_allocation_bytes = 0, fallbacks = 0, leases = 0;
    };
    class FrameLease {
    public:
        FrameLease() = default;
        ~FrameLease();
        FrameLease(FrameLease&& other) noexcept;
        FrameLease& operator=(FrameLease&& other) noexcept;
        FrameLease(const FrameLease&) = delete;
        FrameLease& operator=(const FrameLease&) = delete;
        void finish();
        std::uint64_t serial() const { return serial_; }
    private:
        FrameLease(std::shared_ptr<Impl> owner, std::uint64_t serial);
        std::shared_ptr<Impl> owner_;
        std::uint64_t serial_ = 0;
        friend class GpuSharedCache;
    };
    using Snapshot = std::vector<std::shared_ptr<GpuSharedTexture>>;

    static bool requested();
    static const char* device_extension();
    GpuSharedCache(VkPhysicalDevice physical, VkDevice device, std::uint32_t queue_family, std::string generic_name,
                   VkInstance instance = VK_NULL_HANDLE);
    ~GpuSharedCache();
    GpuSharedCache(const GpuSharedCache&) = delete;
    GpuSharedCache& operator=(const GpuSharedCache&) = delete;

    std::shared_ptr<GpuSharedTexture> texture(const assets::Image& image, bool generate_mips);
    Snapshot snapshot() const;
    FrameLease acquire_frame();
    void record_acquire(VkCommandBuffer command, const Snapshot& textures) const;
    void record_release(VkCommandBuffer command, const Snapshot& textures) const;
    bool active() const;
    Stats stats() const;
private:
    std::shared_ptr<Impl> impl_;
};

struct GpuCacheBrokerOptions {
    std::string device_model;
    std::uint32_t device_index = 0;
    bool validation = false;
    std::uint64_t budget_bytes = kGpuCacheMaximumBudget;
    std::uint32_t maximum_entries = kGpuCacheMaximumEntries;
    std::uint32_t hold_seconds = 4320;
};

// Uses only a generated procedural texture; two independent tool processes exercise production imports/leases.
int run_gpu_cache_broker(const GpuCacheBrokerOptions& options);
int run_gpu_cache_consumer_proof(const GpuCacheBrokerOptions& options, std::uint32_t iterations);
int run_gpu_cache_control(bool stop_if_idle);

} // namespace hailstone::render
