#pragma once

// Local authenticated transport for the experimental GPU asset broker. Only opaque packets and borrowed exports
// cross this boundary; Vulkan stays in render/. Endpoint/token values are private and must never be logged.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hailstone::platform {

inline constexpr std::size_t kGpuIpcMaxBody = std::size_t{256} << 20;

// Transport codec, exposed for GPU-free malformed-frame tests. Render owns its own packet opcodes and schema.
namespace gpu_ipc_wire {
inline constexpr std::size_t kHeaderSize = 20;
enum class Kind : std::uint8_t { Authenticate = 1, Authenticated = 2, Request = 3, Response = 4 };
struct Header {
    Kind kind = Kind::Request;
    std::uint32_t body_size = 0;
    std::uint64_t sequence = 0;
    bool has_handle = false;
};
std::vector<std::byte> encode_header(const Header& header);
Header decode_header(std::span<const std::byte> bytes);
} // namespace gpu_ipc_wire

struct GpuIpcSession {
    std::string endpoint;
    std::string token; // 256 cryptographically random bits, lowercase hexadecimal; never put in arguments or logs.
    std::uint32_t server_process_id = 0; // optional client-side pin; the OS verifies the actual pipe server.
};

bool gpu_ipc_available() noexcept;
// Windows only for now; unsupported platforms fail explicitly so callers can use their private renderer.
GpuIpcSession make_gpu_ipc_session();
// HAILSTONE_GPU_IPC_ENDPOINT, HAILSTONE_GPU_IPC_TOKEN, optional HAILSTONE_GPU_IPC_SERVER_PID (core::env aliases).
// Returns nullopt when neither endpoint nor token is set. Partial/invalid configuration throws without its values.
std::optional<GpuIpcSession> gpu_ipc_session_from_environment();

class TypedGpuIpcHandle {
public:
    TypedGpuIpcHandle() = default;
    ~TypedGpuIpcHandle();
    TypedGpuIpcHandle(TypedGpuIpcHandle&& other) noexcept;
    TypedGpuIpcHandle& operator=(TypedGpuIpcHandle&& other) noexcept;
    TypedGpuIpcHandle(const TypedGpuIpcHandle&) = delete;
    TypedGpuIpcHandle& operator=(const TypedGpuIpcHandle&) = delete;

    // Takes ownership of a real handle in THIS process, for example a Vulkan-exported or received Win32 handle.
    static TypedGpuIpcHandle adopt(void* native);
    void* get() const noexcept { return reinterpret_cast<void*>(value_); }
    std::uintptr_t native_value() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != 0; }
    void* release() noexcept;
    void reset() noexcept;

private:
    std::uintptr_t value_ = 0;
};

struct GpuIpcPeer {
    std::uint32_t process_id = 0; // obtained from GetNamedPipeClientProcessId, never supplied by an RPC body.
    std::uint64_t connection_id = 0; // server-local identity; use this for lease ownership and disconnect handling.
};

struct GpuIpcReply {
    std::vector<std::byte> body;
    void* borrowed_handle = nullptr;
    // Required with borrowed_handle. Keeps the underlying export/resource alive until duplication and reply send.
    std::shared_ptr<const void> handle_owner;
};

struct GpuIpcResult {
    std::vector<std::byte> body;
    TypedGpuIpcHandle handle;
};

class GpuIpcClient {
public:
    ~GpuIpcClient();
    GpuIpcClient(const GpuIpcClient&) = delete;
    GpuIpcClient& operator=(const GpuIpcClient&) = delete;
    static std::unique_ptr<GpuIpcClient> connect(const GpuIpcSession& session, int timeout_ms = 15000);
    // Serialized per connection. Timeout/protocol/peer failures close the channel; a failed call cannot be retried
    // on an uncertain stream. Receivers own and close each duplicated handle even if import or packet decode fails.
    GpuIpcResult call(std::span<const std::byte> body, int timeout_ms = 15000);
    void close() noexcept;

private:
    struct Impl;
    explicit GpuIpcClient(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class GpuIpcServer {
public:
    using Handler = std::function<GpuIpcReply(const GpuIpcPeer&, std::span<const std::byte>)>;
    using Disconnect = std::function<void(const GpuIpcPeer&)>;
    ~GpuIpcServer();
    GpuIpcServer(const GpuIpcServer&) = delete;
    GpuIpcServer& operator=(const GpuIpcServer&) = delete;
    // Current-user DACL, local clients only, first-instance name ownership, at most 16 connection workers. Handler
    // may run concurrently and must return within 10 seconds. Disconnect runs once for every authenticated peer,
    // including transport failure/shutdown, and must invalidate any lease still owned by that connection.
    static std::unique_ptr<GpuIpcServer> start(GpuIpcSession session, Handler handler,
                                               Disconnect disconnect = {}, std::size_t max_peers = 16);
    const GpuIpcSession& session() const noexcept;
    void stop() noexcept;

private:
    struct Impl;
    explicit GpuIpcServer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace hailstone::platform
