#pragma once

// The GPU texture broker's bookkeeping (docs/graphics.md "Shared GPU textures"), Vulkan-free so its failure rules are
// tested directly: the frame lease, the shared images and which client holds references to which. The rule is that
// one client's trouble stays that client's. A malformed or out-of-turn request gets Status::Invalid and changes
// nothing; a texture that cannot be uploaded (out of GPU memory, say) gets Status::Private; a client that disconnects
// gives back its lease and its references; a lease not free in time gets Status::Busy. Nothing stops the others.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include "platform/gpu_ipc.h"
#include "render/gpu_cache_protocol.h"

namespace hailstone::gpu_lab {

// One GPU owner at a time, by connection id.
class GpuCacheLeaseState {
public:
    // False for id 0 or a peer already registered.
    bool register_peer(std::uint64_t peer);
    struct Acquired {
        enum class Kind { Granted, Busy, Invalid } kind = Kind::Invalid;
        std::uint64_t serial = 0;
    };
    // Invalid: an unknown peer, or the owner asking again. Busy: another peer owns it.
    Acquired acquire(std::uint64_t peer);
    // Only the owner, with its serial; anything else changes nothing and returns false.
    bool release(std::uint64_t peer, std::uint64_t serial);
    // Removes the peer and releases its lease. Returns whether it held the lease.
    bool disconnect(std::uint64_t peer);
    std::uint64_t owner() const { return owner_; }
    std::uint64_t serial() const { return serial_; }
    std::size_t peer_count() const { return peers_.size(); }
    bool has_peer(std::uint64_t peer) const;

private:
    std::vector<std::uint64_t> peers_;
    std::uint64_t owner_ = 0, serial_ = 0;
};

// A shared image as the bookkeeping sees it; `owner` keeps the GPU objects and the exported handle alive.
struct BrokerImage {
    std::uint64_t allocation_size = 0;
    std::uint32_t memory_index = 0, levels = 0;
    void* handle = nullptr;
    std::shared_ptr<const void> owner;
    std::uint32_t references = 0;
};

class GpuCacheBrokerCore {
public:
    struct Options {
        render::gpu_cache::Identity identity;
        std::uint64_t epoch = 1;
        std::uint64_t budget_bytes = render::gpu_cache::kMaximumBudget;
        std::uint32_t maximum_entries = render::gpu_cache::kMaximumEntries;
        // How long an Acquire waits for the lease before Busy (render/gpu_share_link.h kLeaseWait).
        std::chrono::milliseconds lease_wait{std::chrono::seconds(5)};
    };
    // Uploads a texture at most `maximum_bytes` large; null when it cannot be shared. May throw (a GPU error):
    // that texture is then declined.
    using Upload = std::function<std::shared_ptr<BrokerImage>(const assets::Image&, bool generate_mips,
                                                              std::uint64_t maximum_bytes)>;

    GpuCacheBrokerCore(Options options, Upload upload);

    // Every request gets a reply, never an exception, and a failed one changes no other client's state.
    platform::GpuIpcReply handle(const platform::GpuIpcPeer& peer, std::span<const std::byte> body) noexcept;
    void disconnect(const platform::GpuIpcPeer& peer) noexcept;
    // Wakes lease waiters (they get Busy) and refuses new clients.
    void stop() noexcept;
    bool stop_requested() const;
    render::gpu_cache::BrokerStats stats() const;
    // Nothing held: no clients, no lease, no images.
    bool idle_and_empty() const;

private:
    platform::GpuIpcReply status(bool ask_stop, std::uint64_t control_peer);
    platform::GpuIpcReply hello(const platform::GpuIpcPeer& peer, const render::gpu_cache::Request& request);
    platform::GpuIpcReply acquire(const platform::GpuIpcPeer& peer);
    platform::GpuIpcReply release(const platform::GpuIpcPeer& peer, const render::gpu_cache::Request& request);
    platform::GpuIpcReply texture(const platform::GpuIpcPeer& peer, const render::gpu_cache::Request& request);
    platform::GpuIpcReply retire(const platform::GpuIpcPeer& peer, const render::gpu_cache::Request& request);
    void drop_reference(const std::string& key) noexcept;

    Options options_;
    Upload upload_;
    mutable std::mutex images_mutex_, lease_mutex_; // lease_mutex_ first when both are taken
    std::condition_variable changed_;
    std::map<std::string, std::shared_ptr<BrokerImage>> images_;
    std::map<std::uint64_t, std::map<std::string, std::uint32_t>> references_;
    GpuCacheLeaseState lease_;
    std::deque<std::uint64_t> waiting_;
    std::uint64_t allocated_ = 0, requests_ = 0, hits_ = 0, grants_ = 0, releases_ = 0, failed_ = 0;
    std::uint64_t pending_stop_connection_ = 0;
    bool stop_ = false;
};

} // namespace hailstone::gpu_lab
