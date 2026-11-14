#include "render/gpu_share_link.h"

#include <exception>
#include <utility>

namespace hailstone::render {

using gpu_cache::Op;
using gpu_cache::Status;

GpuShareLink::GpuShareLink(Call call, Close close, Clock clock, Timeouts timeouts)
    : call_(std::move(call)), close_(std::move(close)), clock_(std::move(clock)), timeouts_(timeouts) {
    if (!clock_) clock_ = [] { return std::chrono::steady_clock::now(); };
}

GpuShareLink::~GpuShareLink() { detach("closed"); }

bool GpuShareLink::connected() const {
    std::lock_guard lock(mutex_);
    return connected_;
}

std::string GpuShareLink::detach_reason() const {
    std::lock_guard lock(mutex_);
    return connected_ ? std::string() : reason_;
}

void GpuShareLink::detach(const std::string& reason) noexcept {
    std::lock_guard lock(mutex_);
    detach_locked(reason);
}

void GpuShareLink::detach_locked(const std::string& reason) noexcept {
    // Closing the connection is what tells the broker: it releases this client's lease and references.
    const bool open = static_cast<bool>(call_);
    connected_ = false;
    if (reason_.empty() || reason_ == "not connected") reason_ = reason;
    if (open && close_) {
        try { close_(); } catch (...) {}
    }
    call_ = {};
}

std::optional<platform::GpuIpcResult> GpuShareLink::send(const gpu_cache::Request& request, int timeout_ms,
                                                         const char* what) {
    if (!call_) return std::nullopt;
    try {
        const auto body = gpu_cache::encode_request(request);
        auto result = call_(body, timeout_ms);
        last_call_ = clock_();
        return result;
    } catch (const std::exception& e) {
        detach_locked(std::string(what) + ": " + e.what());
    } catch (...) {
        detach_locked(what);
    }
    return std::nullopt;
}

bool GpuShareLink::hello(const gpu_cache::Identity& identity) {
    std::lock_guard lock(mutex_);
    gpu_cache::Request request;
    request.op = Op::Hello;
    request.identity = identity;
    auto result = send(request, timeouts_.request_ms, "handshake failed");
    if (!result) return false;
    try {
        ByteReader in(result->body);
        const Status status = gpu_cache::read_status(in);
        if (status == Status::Private) {
            detach_locked("the broker uses a different GPU, driver or memory layout");
            return false;
        }
        if (status != Status::Ok || result->handle) throw std::runtime_error("unexpected reply");
        const auto reply = gpu_cache::read_hello_reply(in);
        if (reply.identity != identity) throw std::runtime_error("the broker's GPU differs");
        epoch_ = reply.epoch;
        connected_ = true;
        reason_.clear();
        return true;
    } catch (const std::exception& e) {
        detach_locked(std::string("handshake failed: ") + e.what());
    }
    return false;
}

std::optional<GpuShareLink::Texture> GpuShareLink::texture(const Sha256Digest& digest,
                                                           std::span<const std::byte> recipe) {
    std::lock_guard lock(mutex_);
    if (!connected_) return std::nullopt;
    gpu_cache::Request request;
    request.op = Op::Texture;
    request.digest = digest;
    request.recipe = recipe;
    auto result = send(request, timeouts_.texture_ms, "texture request failed");
    if (!result) return std::nullopt;
    try {
        ByteReader in(result->body);
        const Status status = gpu_cache::read_status(in);
        if (status == Status::Private) {
            if (!in.empty() || result->handle) throw std::runtime_error("malformed decline");
            return std::nullopt;
        }
        if (status != Status::Ok) throw std::runtime_error("the broker refused the request");
        if (!result->handle) throw std::runtime_error("no memory handle");
        Texture out{gpu_cache::read_texture_grant(in), std::move(result->handle)};
        if (out.grant.epoch != epoch_ || out.grant.digest != digest) throw std::runtime_error("grant for another texture");
        return out;
    } catch (const std::exception& e) {
        detach_locked(std::string("texture request failed: ") + e.what());
    }
    return std::nullopt;
}

std::optional<std::uint64_t> GpuShareLink::acquire() {
    std::lock_guard lock(mutex_);
    if (!connected_) return std::nullopt;
    gpu_cache::Request request;
    request.op = Op::Acquire;
    request.epoch = epoch_;
    auto result = send(request, timeouts_.acquire_ms, "frame lease failed");
    if (!result) return std::nullopt;
    try {
        ByteReader in(result->body);
        const Status status = gpu_cache::read_status(in);
        if (status == Status::Busy) {
            detach_locked("the frame lease was not free in time");
            return std::nullopt;
        }
        if (status != Status::Ok || result->handle) throw std::runtime_error("the broker refused the request");
        const auto grant = gpu_cache::read_lease_grant(in);
        if (grant.epoch != epoch_) throw std::runtime_error("lease from another session");
        return grant.serial;
    } catch (const std::exception& e) {
        detach_locked(std::string("frame lease failed: ") + e.what());
    }
    return std::nullopt;
}

bool GpuShareLink::release(std::uint64_t serial) {
    std::lock_guard lock(mutex_);
    if (!connected_) return false;
    gpu_cache::Request request;
    request.op = Op::Release;
    request.epoch = epoch_;
    request.serial = serial;
    auto result = send(request, timeouts_.request_ms, "frame lease release failed");
    if (!result) return false;
    try {
        ByteReader in(result->body);
        if (gpu_cache::read_status(in) != Status::Ok || !in.empty() || result->handle)
            throw std::runtime_error("the broker refused the request");
        return true;
    } catch (const std::exception& e) {
        detach_locked(std::string("frame lease release failed: ") + e.what());
    }
    return false;
}

void GpuShareLink::retire(const Sha256Digest& digest) noexcept {
    std::lock_guard lock(mutex_);
    if (!connected_) return;
    gpu_cache::Request request;
    request.op = Op::RetireTexture;
    request.epoch = epoch_;
    request.digest = digest;
    auto result = send(request, timeouts_.request_ms, "texture retirement failed");
    if (!result) return;
    try {
        ByteReader in(result->body);
        if (gpu_cache::read_status(in) != Status::Ok || !in.empty() || result->handle)
            throw std::runtime_error("the broker refused the request");
    } catch (const std::exception& e) {
        detach_locked(std::string("texture retirement failed: ") + e.what());
    }
}

void GpuShareLink::keep_alive() noexcept {
    std::lock_guard lock(mutex_);
    if (!connected_) return;
    if (clock_() - last_call_ < timeouts_.keep_alive) return;
    gpu_cache::Request request;
    request.op = Op::Ping;
    request.epoch = epoch_;
    auto result = send(request, timeouts_.request_ms, "keep-alive failed");
    if (!result) return;
    try {
        ByteReader in(result->body);
        if (gpu_cache::read_status(in) != Status::Ok || !in.empty() || result->handle)
            throw std::runtime_error("the broker refused the request");
    } catch (const std::exception& e) {
        detach_locked(std::string("keep-alive failed: ") + e.what());
    }
}

} // namespace hailstone::render
