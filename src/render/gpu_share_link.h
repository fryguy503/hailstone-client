#pragma once

// A client's conversation with the GPU texture broker (docs/graphics.md "Shared GPU textures"), Vulkan-free: what
// each reply means for the client. The rule is that sharing never stops the game. Any failure (the broker gone, a
// timeout, a lease not granted in time, a reply that makes no sense) detaches the link for good: it closes the
// connection, so the broker releases whatever this client held, and the client goes on with private textures
// (GpuSharedCache copies the imports it already has into private ones). A texture the broker only declines
// (Status::Private) is uploaded privately while sharing carries on.

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include "platform/gpu_ipc.h"
#include "render/gpu_cache_protocol.h"

namespace hailstone::render {

struct GpuShareTimeouts {
    int request_ms = 15000;
    int texture_ms = 30000;
    // The broker waits up to GpuShareLink::kLeaseWait for the lease and answers Busy; this covers that and the round trip.
    int acquire_ms = 7000;
    // A connection that sent nothing for this long sends a Ping, well within the server's idle limit
    // (platform::kGpuIpcIdleTimeout).
    std::chrono::milliseconds keep_alive{std::chrono::seconds(60)};
};

class GpuShareLink {
public:
    // One request over the connection, with its timeout; throws on any transport failure.
    using Call = std::function<platform::GpuIpcResult(std::span<const std::byte>, int timeout_ms)>;
    using Close = std::function<void()>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    using Timeouts = GpuShareTimeouts;
    // How long the broker lets a client wait for the frame lease before answering Busy.
    static constexpr std::chrono::milliseconds kLeaseWait{std::chrono::seconds(5)};

    struct Texture {
        gpu_cache::TextureGrant grant;
        platform::TypedGpuIpcHandle handle;
    };

    GpuShareLink(Call call, Close close, Clock clock = {}, Timeouts timeouts = {});
    ~GpuShareLink();
    GpuShareLink(const GpuShareLink&) = delete;
    GpuShareLink& operator=(const GpuShareLink&) = delete;

    // The handshake. False when the broker declines this GPU or anything fails; the link is then detached.
    bool hello(const gpu_cache::Identity& identity);
    bool connected() const;
    // Why the link detached (for the log), empty while connected.
    std::string detach_reason() const;
    void detach(const std::string& reason) noexcept;

    // Nullopt: upload privately. The link stays connected when the broker only declined this texture.
    std::optional<Texture> texture(const Sha256Digest& digest, std::span<const std::byte> recipe);
    // The frame lease's serial, or nullopt (the link has detached: Busy, a failure, or it already was).
    std::optional<std::uint64_t> acquire();
    bool release(std::uint64_t serial);
    // This client no longer uses the texture.
    void retire(const Sha256Digest& digest) noexcept;
    // Pings when the connection has been quiet for Timeouts::keep_alive.
    void keep_alive() noexcept;

private:
    // Sends one request; a transport failure detaches and returns nullopt.
    std::optional<platform::GpuIpcResult> send(const gpu_cache::Request& request, int timeout_ms, const char* what);
    void detach_locked(const std::string& reason) noexcept;

    Call call_;
    Close close_;
    Clock clock_;
    Timeouts timeouts_;
    mutable std::mutex mutex_;
    bool connected_ = false;
    std::string reason_ = "not connected";
    std::uint64_t epoch_ = 0;
    std::chrono::steady_clock::time_point last_call_{};
};

} // namespace hailstone::render
