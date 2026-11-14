// The GPU IPC server: only the broker tool (tools/gpu_lab) and the tests link it; the client has only the
// connecting side (gpu_ipc.cpp).
#include "platform/gpu_ipc.h"

#include "core/byte_io.h"
#include "platform/gpu_ipc_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <bcrypt.h>
#endif

namespace hailstone::platform {
using namespace gpu_ipc_detail;
namespace {

#if defined(_WIN32)
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

thread_local const void* current_server_worker = nullptr;

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

#if defined(_WIN32)
struct GpuIpcServer::Impl {
    GpuIpcSession session;
    Handler handler;
    Disconnect disconnect;
    WinHandle stop_event;
    std::vector<WinHandle> pipes;
    std::vector<std::thread> workers;
    std::atomic<std::uint64_t> next_connection{1};
    std::mutex stop_mutex;
    std::chrono::milliseconds idle_timeout{};
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
                    const auto request = read_header(pipe, stop_event.get(), process.get(), Deadline::max(),
                                                     Clock::now() + idle_timeout);
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
struct GpuIpcServer::Impl { GpuIpcSession session; };
#endif

GpuIpcServer::GpuIpcServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GpuIpcServer::~GpuIpcServer() { stop(); }

std::unique_ptr<GpuIpcServer> GpuIpcServer::start(GpuIpcSession session, Handler handler,
                                                Disconnect disconnect, std::size_t max_peers,
                                                std::chrono::milliseconds idle_timeout) {
    validate_session(session);
    if (!handler || max_peers < 1 || max_peers > 16 || idle_timeout.count() < 1) throw std::invalid_argument("GPU IPC server configuration is invalid");
#if defined(_WIN32)
    if (session.server_process_id && session.server_process_id != ::GetCurrentProcessId())
        throw std::runtime_error("GPU IPC server process pin is invalid");
    session.server_process_id = ::GetCurrentProcessId();
    auto impl = std::make_unique<Impl>();
    impl->session = std::move(session);
    impl->handler = std::move(handler);
    impl->disconnect = std::move(disconnect);
    impl->idle_timeout = idle_timeout;
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
    (void)idle_timeout;
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
