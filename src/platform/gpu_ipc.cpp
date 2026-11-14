#include "platform/gpu_ipc.h"

#include "core/byte_io.h"
#include "core/env.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace hailstone::platform {
namespace {

constexpr std::string_view kPipePrefix = R"(\\.\pipe\hailstone-gpu-)";
constexpr std::uint32_t kMagic = 0x50494748; // HGIP, assembled by ByteReader/ByteWriter.
constexpr std::uint16_t kVersion = 1;

[[noreturn]] void invalid_frame() { throw std::runtime_error("GPU IPC protocol frame is invalid"); }
[[maybe_unused, noreturn]] void transport_failure() {
    throw std::runtime_error("GPU IPC peer, cancellation or timeout failure");
}

void validate_header(const gpu_ipc_wire::Header& h) {
    using K = gpu_ipc_wire::Kind;
    if (h.body_size > kGpuIpcMaxBody) invalid_frame();
    switch (h.kind) {
    case K::Authenticate:
        if (h.sequence != 0 || h.body_size != 32 || h.has_handle) invalid_frame();
        break;
    case K::Authenticated:
        if (h.sequence != 0 || h.body_size != 0 || h.has_handle) invalid_frame();
        break;
    case K::Request:
        if (h.sequence == 0 || h.has_handle) invalid_frame();
        break;
    case K::Response:
        if (h.sequence == 0) invalid_frame();
        break;
    default: invalid_frame();
    }
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::array<std::byte, 32> token_bytes(std::string_view token) {
    if (token.size() != 64) throw std::runtime_error("GPU IPC session configuration is invalid");
    std::array<std::byte, 32> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = hex_digit(token[i * 2]), low = hex_digit(token[i * 2 + 1]);
        if (high < 0 || low < 0) throw std::runtime_error("GPU IPC session configuration is invalid");
        out[i] = static_cast<std::byte>((high << 4) | low);
    }
    return out;
}

void validate_session(const GpuIpcSession& session) {
    if (!session.endpoint.starts_with(kPipePrefix) || session.endpoint.size() != kPipePrefix.size() + 32)
        throw std::runtime_error("GPU IPC session configuration is invalid");
    for (char c : std::string_view(session.endpoint).substr(kPipePrefix.size()))
        if (hex_digit(c) < 0) throw std::runtime_error("GPU IPC session configuration is invalid");
    (void)token_bytes(session.token);
}

void validate_timeout(int timeout_ms) {
    if (timeout_ms < 1 || timeout_ms > 120000) throw std::invalid_argument("GPU IPC timeout is out of range");
}

#if defined(_WIN32)

class WinHandle {
public:
    WinHandle() = default;
    explicit WinHandle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~WinHandle() { reset(); }
    WinHandle(WinHandle&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) { reset(); h_ = std::exchange(other.h_, nullptr); }
        return *this;
    }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }
    void reset() noexcept { if (h_) ::CloseHandle(std::exchange(h_, nullptr)); }
private:
    HANDLE h_ = nullptr;
};

struct UserSecurity {
    std::vector<std::byte> token_info, acl_storage;
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};
    void initialize() {
        HANDLE raw = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw)) transport_failure();
        WinHandle token(raw);
        DWORD size = 0;
        ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
        if (!size) transport_failure();
        token_info.resize(size);
        if (!::GetTokenInformation(token.get(), TokenUser, token_info.data(), size, &size)) transport_failure();
        PSID sid = reinterpret_cast<const TOKEN_USER*>(token_info.data())->User.Sid;
        if (!::IsValidSid(sid)) transport_failure();
        const DWORD acl_size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) +
                               ::GetLengthSid(sid);
        acl_storage.resize(acl_size);
        auto* acl = reinterpret_cast<ACL*>(acl_storage.data());
        if (!::InitializeAcl(acl, acl_size, ACL_REVISION) ||
            !::AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS, sid) ||
            !::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !::SetSecurityDescriptorOwner(&descriptor, sid, FALSE) ||
            !::SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
            !::SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) transport_failure();
        attributes = {sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    }
};

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
thread_local const void* current_server_worker = nullptr;

DWORD remaining_ms(Deadline deadline) {
    if (deadline == Deadline::max()) return INFINITE;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) return 0;
    return static_cast<DWORD>(std::min<std::int64_t>(remaining, INFINITE - 1));
}

bool stopped(HANDLE stop, HANDLE peer = nullptr) {
    return ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
           (peer && ::WaitForSingleObject(peer, 0) == WAIT_OBJECT_0);
}

bool complete_io(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, HANDLE peer, Deadline deadline, DWORD& transferred) {
    HANDLE waits[]{operation.hEvent, stop, peer};
    const DWORD result = ::WaitForMultipleObjects(peer ? 3 : 2, waits, FALSE, remaining_ms(deadline));
    if (result == WAIT_OBJECT_0) return ::GetOverlappedResult(pipe, &operation, &transferred, FALSE) != 0;
    // Drain cancellation before the OVERLAPPED and its buffer leave scope.
    ::CancelIoEx(pipe, &operation);
    ::GetOverlappedResult(pipe, &operation, &transferred, TRUE);
    return false;
}

bool transfer(HANDLE pipe, std::span<std::byte> bytes, bool write, HANDLE stop, HANDLE peer, Deadline deadline) {
    WinHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (stopped(stop, peer) || remaining_ms(deadline) == 0) return false;
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        ::ResetEvent(event.get());
        DWORD transferred = 0;
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1U << 20));
        const BOOL immediate = write ? ::WriteFile(pipe, bytes.data() + offset, count, &transferred, &operation)
                                     : ::ReadFile(pipe, bytes.data() + offset, count, &transferred, &operation);
        if (!immediate && (::GetLastError() != ERROR_IO_PENDING ||
                           !complete_io(pipe, operation, stop, peer, deadline, transferred))) return false;
        if (!transferred || transferred > count) return false;
        offset += transferred;
    }
    return true;
}

void read_bytes(HANDLE pipe, std::span<std::byte> bytes, HANDLE stop, HANDLE peer, Deadline deadline) {
    if (!transfer(pipe, bytes, false, stop, peer, deadline)) transport_failure();
}

void write_bytes(HANDLE pipe, std::span<const std::byte> bytes, HANDLE stop, HANDLE peer, Deadline deadline) {
    // WriteFile does not modify its input; transfer shares the bounded completion logic with reads.
    auto mutable_bytes = std::span<std::byte>(const_cast<std::byte*>(bytes.data()), bytes.size());
    if (!transfer(pipe, mutable_bytes, true, stop, peer, deadline)) transport_failure();
}

gpu_ipc_wire::Header read_header(HANDLE pipe, HANDLE stop, HANDLE peer, Deadline deadline, bool idle = false) {
    std::array<std::byte, gpu_ipc_wire::kHeaderSize> bytes{};
    if (idle) {
        // Idle authenticated peers can stay connected; a partially sent header and its body each have deadlines.
        read_bytes(pipe, std::span(bytes).first(1), stop, peer, Deadline::max());
        deadline = Clock::now() + std::chrono::seconds(15);
        read_bytes(pipe, std::span(bytes).subspan(1), stop, peer, deadline);
    } else {
        read_bytes(pipe, bytes, stop, peer, deadline);
    }
    return gpu_ipc_wire::decode_header(bytes);
}

void write_header(HANDLE pipe, const gpu_ipc_wire::Header& header, HANDLE stop, HANDLE peer, Deadline deadline) {
    const auto bytes = gpu_ipc_wire::encode_header(header);
    write_bytes(pipe, bytes, stop, peer, deadline);
}

bool connect_pipe(HANDLE pipe, HANDLE stop) {
    WinHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (::ConnectNamedPipe(pipe, &operation)) return true;
    const DWORD error = ::GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    DWORD transferred = 0;
    return complete_io(pipe, operation, stop, nullptr, Deadline::max(), transferred);
}

std::string hexadecimal(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::byte byte : bytes) {
        const auto n = std::to_integer<unsigned>(byte);
        out.push_back(digits[n >> 4]);
        out.push_back(digits[n & 15]);
    }
    return out;
}

bool token_equal(std::span<const std::byte, 32> a, std::span<const std::byte, 32> b) {
    unsigned different = 0;
    for (std::size_t i = 0; i < a.size(); ++i) different |= std::to_integer<unsigned>(a[i] ^ b[i]);
    return different == 0;
}

#endif
} // namespace

namespace gpu_ipc_wire {
std::vector<std::byte> encode_header(const Header& header) {
    validate_header(header);
    ByteWriter out;
    out.le(kMagic);
    out.le(kVersion);
    out.le(static_cast<std::uint8_t>(header.kind));
    out.le(static_cast<std::uint8_t>(header.has_handle ? 1 : 0));
    out.le(header.body_size);
    out.le(header.sequence);
    return out.take();
}

Header decode_header(std::span<const std::byte> bytes) {
    if (bytes.size() != kHeaderSize) invalid_frame();
    ByteReader in(bytes);
    if (in.le<std::uint32_t>() != kMagic || in.le<std::uint16_t>() != kVersion) invalid_frame();
    Header out;
    out.kind = static_cast<Kind>(in.le<std::uint8_t>());
    const auto flags = in.le<std::uint8_t>();
    if ((flags & ~1U) != 0) invalid_frame();
    out.has_handle = (flags & 1) != 0;
    out.body_size = in.le<std::uint32_t>();
    out.sequence = in.le<std::uint64_t>();
    validate_header(out);
    return out;
}
} // namespace gpu_ipc_wire

bool gpu_ipc_available() noexcept {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

GpuIpcSession make_gpu_ipc_session() {
#if defined(_WIN32)
    std::array<std::byte, 48> random{};
    if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(random.data()), static_cast<ULONG>(random.size()),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) transport_failure();
    return {std::string(kPipePrefix) + hexadecimal(std::span(random).first(16)),
            hexadecimal(std::span(random).subspan(16)), 0};
#else
    throw std::runtime_error("GPU IPC is not available on this platform");
#endif
}

std::optional<GpuIpcSession> gpu_ipc_session_from_environment() {
    const char* endpoint = core::env("GPU_IPC_ENDPOINT");
    const char* token = core::env("GPU_IPC_TOKEN");
    const char* server = core::env("GPU_IPC_SERVER_PID");
    const bool has_endpoint = endpoint && *endpoint, has_token = token && *token, has_server = server && *server;
    if (!has_endpoint && !has_token && !has_server) return std::nullopt;
    if (!has_endpoint || !has_token) throw std::runtime_error("GPU IPC session environment is incomplete");
    GpuIpcSession session{endpoint, token, 0};
    validate_session(session);
    if (has_server) {
        const auto parsed = std::from_chars(server, server + std::char_traits<char>::length(server), session.server_process_id);
        if (parsed.ec != std::errc{} || *parsed.ptr || !session.server_process_id)
            throw std::runtime_error("GPU IPC session process pin is invalid");
    }
    return session;
}

TypedGpuIpcHandle::~TypedGpuIpcHandle() { reset(); }
TypedGpuIpcHandle::TypedGpuIpcHandle(TypedGpuIpcHandle&& other) noexcept : value_(std::exchange(other.value_, 0)) {}
TypedGpuIpcHandle& TypedGpuIpcHandle::operator=(TypedGpuIpcHandle&& other) noexcept {
    if (this != &other) { reset(); value_ = std::exchange(other.value_, 0); }
    return *this;
}
TypedGpuIpcHandle TypedGpuIpcHandle::adopt(void* native) {
    TypedGpuIpcHandle out;
    if (!native) return out;
#if defined(_WIN32)
    if (native == INVALID_HANDLE_VALUE) throw std::invalid_argument("GPU IPC handle is invalid");
    out.value_ = reinterpret_cast<std::uintptr_t>(native);
    return out;
#else
    throw std::runtime_error("GPU IPC is not available on this platform");
#endif
}
void* TypedGpuIpcHandle::release() noexcept { return reinterpret_cast<void*>(std::exchange(value_, 0)); }
void TypedGpuIpcHandle::reset() noexcept {
#if defined(_WIN32)
    if (value_) ::CloseHandle(reinterpret_cast<HANDLE>(std::exchange(value_, 0)));
#else
    value_ = 0;
#endif
}

#if defined(_WIN32)
struct GpuIpcClient::Impl {
    WinHandle pipe, stop_event, peer_process;
    std::mutex mutex;
    std::uint64_t sequence = 0;
};

struct GpuIpcServer::Impl {
    GpuIpcSession session;
    Handler handler;
    Disconnect disconnect;
    WinHandle stop_event;
    std::vector<WinHandle> pipes;
    std::vector<std::thread> workers;
    std::atomic<std::uint64_t> next_connection{1};
    std::mutex stop_mutex;
    bool joined = false;

    void worker(HANDLE pipe) noexcept {
        using K = gpu_ipc_wire::Kind;
        current_server_worker = this;
        const auto expected_token = token_bytes(session.token);
        while (!stopped(stop_event.get())) {
            if (!connect_pipe(pipe, stop_event.get())) break;
            GpuIpcPeer peer;
            bool authenticated = false;
            try {
                ULONG process_id = 0;
                if (!::GetNamedPipeClientProcessId(pipe, &process_id) || !process_id) transport_failure();
                WinHandle process(::OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                                 FALSE, process_id));
                if (!process || ::GetProcessId(process.get()) != process_id ||
                    ::WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) transport_failure();
                peer = {process_id, next_connection.fetch_add(1)};
                if (!peer.connection_id) transport_failure();
                const Deadline hello_deadline = Clock::now() + std::chrono::seconds(5);
                const auto hello = read_header(pipe, stop_event.get(), process.get(), hello_deadline);
                if (hello.kind != K::Authenticate) invalid_frame();
                std::array<std::byte, 32> received_token{};
                read_bytes(pipe, received_token, stop_event.get(), process.get(), hello_deadline);
                if (!token_equal(received_token, expected_token)) transport_failure();
                write_header(pipe, {K::Authenticated, 0, 0, false}, stop_event.get(), process.get(), hello_deadline);
                authenticated = true;
                std::uint64_t last_sequence = 0;
                while (!stopped(stop_event.get(), process.get())) {
                    const auto request = read_header(pipe, stop_event.get(), process.get(), Deadline::max(), true);
                    if (request.kind != K::Request || last_sequence == std::numeric_limits<std::uint64_t>::max() ||
                        request.sequence != last_sequence + 1) invalid_frame();
                    last_sequence = request.sequence;
                    std::vector<std::byte> body(request.body_size);
                    read_bytes(pipe, body, stop_event.get(), process.get(), Clock::now() + std::chrono::seconds(15));
                    GpuIpcReply reply = handler(peer, body);
                    if (reply.body.size() > kGpuIpcMaxBody || (reply.borrowed_handle && !reply.handle_owner)) invalid_frame();
                    const Deadline reply_deadline = Clock::now() + std::chrono::seconds(15);
                    write_header(pipe, {K::Response, static_cast<std::uint32_t>(reply.body.size()), request.sequence,
                                        reply.borrowed_handle != nullptr}, stop_event.get(), process.get(), reply_deadline);
                    if (reply.borrowed_handle) {
                        HANDLE remote = nullptr;
                        if (stopped(stop_event.get(), process.get()) ||
                            !::DuplicateHandle(::GetCurrentProcess(), reply.borrowed_handle, process.get(), &remote,
                                                0, FALSE, DUPLICATE_SAME_ACCESS)) transport_failure();
                        ByteWriter handle_value;
                        handle_value.le(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(remote)));
                        // The destination owns this handle now. Never CloseHandle its integer in this process or
                        // close a possibly reused destination handle after a partial/broken transport write.
                        write_bytes(pipe, handle_value.data(), stop_event.get(), process.get(), reply_deadline);
                    }
                    write_bytes(pipe, reply.body, stop_event.get(), process.get(), reply_deadline);
                }
            } catch (...) {
                // The client sees a broken channel. Never relay a handler exception or authentication material.
            }
            if (authenticated && disconnect) {
                try { disconnect(peer); } catch (...) { ::SetEvent(stop_event.get()); }
            }
            ::DisconnectNamedPipe(pipe);
        }
        ::DisconnectNamedPipe(pipe);
        current_server_worker = nullptr;
    }

    void stop() noexcept {
        ::SetEvent(stop_event.get());
        // A callback may request stopping, but only a control thread joins workers after callbacks return.
        if (current_server_worker == this) return;
        std::lock_guard lock(stop_mutex);
        if (joined) return;
        for (auto& pipe : pipes) ::CancelIoEx(pipe.get(), nullptr);
        for (auto& thread : workers) if (thread.joinable()) thread.join();
        pipes.clear();
        joined = true;
    }
    ~Impl() { stop(); }
};
#else
struct GpuIpcClient::Impl {};
struct GpuIpcServer::Impl { GpuIpcSession session; };
#endif

GpuIpcClient::GpuIpcClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuIpcClient::~GpuIpcClient() { close(); }

std::unique_ptr<GpuIpcClient> GpuIpcClient::connect(const GpuIpcSession& session, int timeout_ms) {
    validate_session(session);
    validate_timeout(timeout_ms);
#if defined(_WIN32)
    auto impl = std::make_unique<Impl>();
    impl->stop_event = WinHandle(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!impl->stop_event) transport_failure();
    const std::wstring name(session.endpoint.begin(), session.endpoint.end());
    const Deadline deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        impl->pipe = WinHandle(::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (impl->pipe) break;
        if (::GetLastError() != ERROR_PIPE_BUSY || !remaining_ms(deadline)) transport_failure();
        ::WaitNamedPipeW(name.c_str(), std::min<DWORD>(remaining_ms(deadline), 100));
    }
    ULONG server_id = 0;
    if (!::GetNamedPipeServerProcessId(impl->pipe.get(), &server_id) || !server_id ||
        (session.server_process_id && server_id != session.server_process_id)) transport_failure();
    impl->peer_process = WinHandle(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server_id));
    if (!impl->peer_process || ::GetProcessId(impl->peer_process.get()) != server_id) transport_failure();
    const auto token = token_bytes(session.token);
    write_header(impl->pipe.get(), {gpu_ipc_wire::Kind::Authenticate, 32, 0, false}, impl->stop_event.get(),
                 impl->peer_process.get(), deadline);
    write_bytes(impl->pipe.get(), token, impl->stop_event.get(), impl->peer_process.get(), deadline);
    const auto accepted = read_header(impl->pipe.get(), impl->stop_event.get(), impl->peer_process.get(), deadline);
    if (accepted.kind != gpu_ipc_wire::Kind::Authenticated) invalid_frame();
    return std::unique_ptr<GpuIpcClient>(new GpuIpcClient(std::move(impl)));
#else
    throw std::runtime_error("GPU IPC is not available on this platform");
#endif
}

GpuIpcResult GpuIpcClient::call(std::span<const std::byte> body, int timeout_ms) {
    validate_timeout(timeout_ms);
    if (body.size() > kGpuIpcMaxBody) throw std::invalid_argument("GPU IPC request exceeds the body limit");
#if defined(_WIN32)
    std::lock_guard lock(impl_->mutex);
    if (!impl_->pipe || stopped(impl_->stop_event.get(), impl_->peer_process.get())) transport_failure();
    try {
        if (impl_->sequence == std::numeric_limits<std::uint64_t>::max()) transport_failure();
        const auto sequence = ++impl_->sequence;
        const Deadline deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        write_header(impl_->pipe.get(), {gpu_ipc_wire::Kind::Request, static_cast<std::uint32_t>(body.size()), sequence, false},
                     impl_->stop_event.get(), impl_->peer_process.get(), deadline);
        write_bytes(impl_->pipe.get(), body, impl_->stop_event.get(), impl_->peer_process.get(), deadline);
        const auto response = read_header(impl_->pipe.get(), impl_->stop_event.get(), impl_->peer_process.get(), deadline);
        if (response.kind != gpu_ipc_wire::Kind::Response || response.sequence != sequence) invalid_frame();
        GpuIpcResult result;
        if (response.has_handle) {
            std::array<std::byte, 8> bytes{};
            read_bytes(impl_->pipe.get(), bytes, impl_->stop_event.get(), impl_->peer_process.get(), deadline);
            const auto value = ByteReader(bytes).le<std::uint64_t>();
            if (!value || value > std::numeric_limits<std::uintptr_t>::max()) invalid_frame();
            result.handle = TypedGpuIpcHandle::adopt(reinterpret_cast<void*>(static_cast<std::uintptr_t>(value)));
        }
        result.body.resize(response.body_size);
        read_bytes(impl_->pipe.get(), result.body, impl_->stop_event.get(), impl_->peer_process.get(), deadline);
        return result;
    } catch (...) {
        ::SetEvent(impl_->stop_event.get());
        ::CancelIoEx(impl_->pipe.get(), nullptr);
        impl_->pipe.reset();
        throw;
    }
#else
    (void)body;
    throw std::runtime_error("GPU IPC is not available on this platform");
#endif
}

void GpuIpcClient::close() noexcept {
#if defined(_WIN32)
    if (!impl_) return;
    ::SetEvent(impl_->stop_event.get());
    std::lock_guard lock(impl_->mutex);
    if (impl_->pipe) ::CancelIoEx(impl_->pipe.get(), nullptr);
    impl_->pipe.reset();
#endif
}

GpuIpcServer::GpuIpcServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuIpcServer::~GpuIpcServer() { stop(); }

std::unique_ptr<GpuIpcServer> GpuIpcServer::start(GpuIpcSession session, Handler handler,
                                                Disconnect disconnect, std::size_t max_peers) {
    validate_session(session);
    if (!handler || max_peers < 1 || max_peers > 16) throw std::invalid_argument("GPU IPC server configuration is invalid");
#if defined(_WIN32)
    if (session.server_process_id && session.server_process_id != ::GetCurrentProcessId())
        throw std::runtime_error("GPU IPC server process pin is invalid");
    session.server_process_id = ::GetCurrentProcessId();
    auto impl = std::make_unique<Impl>();
    impl->session = std::move(session);
    impl->handler = std::move(handler);
    impl->disconnect = std::move(disconnect);
    impl->stop_event = WinHandle(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!impl->stop_event) transport_failure();
    UserSecurity security;
    security.initialize();
    const std::wstring name(impl->session.endpoint.begin(), impl->session.endpoint.end());
    for (std::size_t i = 0; i < max_peers; ++i) {
        const DWORD first = i == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0;
        WinHandle pipe(::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | first,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                         static_cast<DWORD>(max_peers), 65536, 65536, 0, &security.attributes));
        if (!pipe) transport_failure();
        impl->pipes.push_back(std::move(pipe));
    }
    try {
        for (const auto& pipe : impl->pipes)
            impl->workers.emplace_back([state = impl.get(), handle = pipe.get()] { state->worker(handle); });
    } catch (...) { impl->stop(); throw; }
    return std::unique_ptr<GpuIpcServer>(new GpuIpcServer(std::move(impl)));
#else
    (void)disconnect;
    throw std::runtime_error("GPU IPC is not available on this platform");
#endif
}

const GpuIpcSession& GpuIpcServer::session() const noexcept { return impl_->session; }
void GpuIpcServer::stop() noexcept {
#if defined(_WIN32)
    if (impl_) impl_->stop();
#endif
}

} // namespace hailstone::platform
