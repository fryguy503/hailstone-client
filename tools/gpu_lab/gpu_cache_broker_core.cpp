#include "gpu_lab/gpu_cache_broker_core.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace hailstone::gpu_lab {

using render::gpu_cache::Op;
using render::gpu_cache::Status;
namespace protocol = render::gpu_cache;

bool GpuCacheLeaseState::register_peer(std::uint64_t peer) {
    if (!peer || has_peer(peer)) return false;
    peers_.push_back(peer);
    return true;
}

GpuCacheLeaseState::Acquired GpuCacheLeaseState::acquire(std::uint64_t peer) {
    using Kind = Acquired::Kind;
    if (!has_peer(peer) || owner_ == peer || serial_ == std::numeric_limits<std::uint64_t>::max()) return {Kind::Invalid};
    if (owner_) return {Kind::Busy};
    owner_ = peer;
    return {Kind::Granted, ++serial_};
}

bool GpuCacheLeaseState::release(std::uint64_t peer, std::uint64_t serial) {
    if (!peer || owner_ != peer || serial_ != serial) return false;
    owner_ = 0;
    return true;
}

bool GpuCacheLeaseState::disconnect(std::uint64_t peer) {
    const bool held = peer && owner_ == peer;
    if (held) owner_ = 0;
    std::erase(peers_, peer);
    return held;
}

bool GpuCacheLeaseState::has_peer(std::uint64_t peer) const {
    return std::find(peers_.begin(), peers_.end(), peer) != peers_.end();
}

namespace {
platform::GpuIpcReply reply(std::vector<std::byte> body) { return {std::move(body), nullptr, {}}; }
platform::GpuIpcReply reply(Status status) { return reply(protocol::encode_status(status)); }
} // namespace

GpuCacheBrokerCore::GpuCacheBrokerCore(Options options, Upload upload)
    : options_(std::move(options)), upload_(std::move(upload)) {
    if (!options_.epoch || !options_.budget_bytes || options_.budget_bytes > protocol::kMaximumBudget ||
        !options_.maximum_entries || options_.maximum_entries > protocol::kMaximumEntries || !upload_)
        throw std::invalid_argument("GPU cache broker options are invalid");
}

bool GpuCacheBrokerCore::stop_requested() const {
    std::lock_guard lock(lease_mutex_);
    return stop_;
}

void GpuCacheBrokerCore::stop() noexcept {
    std::lock_guard lock(lease_mutex_);
    stop_ = true;
    changed_.notify_all();
}

render::gpu_cache::BrokerStats GpuCacheBrokerCore::stats() const {
    std::scoped_lock lock(lease_mutex_, images_mutex_);
    protocol::BrokerStats out;
    out.epoch = options_.epoch;
    out.consumers = static_cast<std::uint32_t>(lease_.peer_count());
    out.owner = lease_.owner();
    out.allocated = allocated_;
    out.entries = static_cast<std::uint32_t>(images_.size());
    out.requests = requests_;
    out.hits = hits_;
    out.grants = grants_;
    out.releases = releases_;
    out.failed_requests = failed_;
    return out;
}

bool GpuCacheBrokerCore::idle_and_empty() const {
    std::scoped_lock lock(lease_mutex_, images_mutex_);
    return lease_.peer_count() == 0 && lease_.owner() == 0 && images_.empty() && allocated_ == 0;
}

void GpuCacheBrokerCore::drop_reference(const std::string& key) noexcept {
    auto found = images_.find(key);
    if (found == images_.end() || !found->second->references) return;
    if (--found->second->references == 0) {
        allocated_ -= found->second->allocation_size;
        images_.erase(found);
    }
}

void GpuCacheBrokerCore::disconnect(const platform::GpuIpcPeer& peer) noexcept {
    std::scoped_lock lock(lease_mutex_, images_mutex_);
    // A client that goes away holding the lease gives it back: its GPU work ended with its process or its device
    // (a crashed client) or it detached after its own fence (render/gpu_share_link.h); the others carry on.
    lease_.disconnect(peer.connection_id);
    std::erase(waiting_, peer.connection_id);
    if (auto found = references_.find(peer.connection_id); found != references_.end()) {
        for (const auto& [key, count] : found->second)
            for (std::uint32_t i = 0; i < count; ++i) drop_reference(key);
        references_.erase(found);
    }
    // A stop acknowledgement must reach its caller before the transport is cancelled.
    if (pending_stop_connection_ && pending_stop_connection_ == peer.connection_id) stop_ = true;
    changed_.notify_all();
}

platform::GpuIpcReply GpuCacheBrokerCore::handle(const platform::GpuIpcPeer& peer,
                                                 std::span<const std::byte> body) noexcept {
    try {
        const auto request = protocol::decode_request(body);
        switch (request.op) {
        case Op::Status: return status(false, peer.connection_id);
        case Op::StopIfIdle: return status(true, peer.connection_id);
        case Op::Hello: return hello(peer, request);
        default: break;
        }
        bool known = false;
        {
            std::lock_guard lock(lease_mutex_);
            known = lease_.has_peer(peer.connection_id);
        }
        const bool has_epoch = request.op != Op::Texture; // every other operation left carries the session's epoch
        if (!known || (has_epoch && request.epoch != options_.epoch)) throw std::runtime_error("request out of turn");
        switch (request.op) {
        case Op::Acquire: return acquire(peer);
        case Op::Release: return release(peer, request);
        case Op::Texture: return texture(peer, request);
        case Op::RetireTexture: return retire(peer, request);
        case Op::Ping: return reply(Status::Ok);
        default: throw std::runtime_error("request out of turn");
        }
    } catch (...) {
    }
    // Malformed or out of turn: this request fails, nothing else changes.
    try {
        std::lock_guard lock(images_mutex_);
        ++failed_;
        return reply(Status::Invalid);
    } catch (...) {
        return {}; // out of memory for even that: the transport closes this connection
    }
}

platform::GpuIpcReply GpuCacheBrokerCore::status(bool ask_stop, std::uint64_t control_peer) {
    bool accepted = false;
    {
        std::scoped_lock lock(lease_mutex_, images_mutex_);
        const bool idle = lease_.peer_count() == 0 && lease_.owner() == 0;
        accepted = ask_stop && idle && control_peer &&
                   (!pending_stop_connection_ || pending_stop_connection_ == control_peer);
        if (accepted) pending_stop_connection_ = control_peer;
    }
    return reply(protocol::encode_stats(ask_stop && !accepted ? Status::Busy : Status::Ok, stats()));
}

platform::GpuIpcReply GpuCacheBrokerCore::hello(const platform::GpuIpcPeer& peer, const protocol::Request& request) {
    if (request.identity != options_.identity) return reply(Status::Private);
    std::scoped_lock lock(lease_mutex_, images_mutex_);
    if (pending_stop_connection_ || stop_) return reply(Status::Private);
    if (!lease_.register_peer(peer.connection_id)) return reply(Status::Invalid);
    references_.try_emplace(peer.connection_id);
    return reply(protocol::encode_hello_reply({options_.identity, options_.epoch}));
}

platform::GpuIpcReply GpuCacheBrokerCore::acquire(const platform::GpuIpcPeer& peer) {
    std::unique_lock lock(lease_mutex_);
    if (!lease_.has_peer(peer.connection_id) || lease_.owner() == peer.connection_id ||
        std::find(waiting_.begin(), waiting_.end(), peer.connection_id) != waiting_.end())
        return reply(Status::Invalid);
    // First come, first served: a waiter takes the lease only at the head of the queue.
    waiting_.push_back(peer.connection_id);
    const bool ready = changed_.wait_for(lock, options_.lease_wait, [&] {
        return stop_ || !lease_.has_peer(peer.connection_id) ||
               (!lease_.owner() && !waiting_.empty() && waiting_.front() == peer.connection_id);
    });
    std::erase(waiting_, peer.connection_id);
    changed_.notify_all(); // the next waiter may now be at the head
    if (!ready || stop_ || !lease_.has_peer(peer.connection_id)) return reply(Status::Busy);
    const auto acquired = lease_.acquire(peer.connection_id);
    if (acquired.kind != GpuCacheLeaseState::Acquired::Kind::Granted) return reply(Status::Busy);
    ++grants_;
    return reply(protocol::encode_lease_grant({options_.epoch, acquired.serial}));
}

platform::GpuIpcReply GpuCacheBrokerCore::release(const platform::GpuIpcPeer& peer, const protocol::Request& request) {
    std::lock_guard lock(lease_mutex_);
    if (!lease_.release(peer.connection_id, request.serial)) return reply(Status::Invalid);
    ++releases_;
    changed_.notify_all();
    return reply(Status::Ok);
}

platform::GpuIpcReply GpuCacheBrokerCore::texture(const platform::GpuIpcPeer& peer, const protocol::Request& request) {
    const auto recipe = protocol::decode_texture(request.recipe);
    if (protocol::texture_key(recipe.image, recipe.generate_mips) != request.digest)
        throw std::runtime_error("recipe digest differs");
    const auto key = to_hex(request.digest);
    std::lock_guard lock(images_mutex_);
    auto client = references_.find(peer.connection_id);
    if (client == references_.end()) throw std::runtime_error("unknown client");
    ++requests_;
    std::shared_ptr<BrokerImage> image;
    const auto found = images_.find(key);
    const bool hit = found != images_.end();
    if (hit) {
        image = found->second;
        ++hits_;
    } else {
        if (images_.size() >= options_.maximum_entries || allocated_ >= options_.budget_bytes) return reply(Status::Private);
        try {
            image = upload_(recipe.image, recipe.generate_mips, options_.budget_bytes - allocated_);
        } catch (...) {
            image.reset(); // a GPU failure (out of memory, say) declines this texture only
        }
        if (!image || !image->handle || !image->owner || !image->allocation_size ||
            image->allocation_size > options_.budget_bytes - allocated_)
            return reply(Status::Private);
        image->references = 0;
    }
    auto& count = client->second[key];
    if (count == std::numeric_limits<std::uint32_t>::max() ||
        image->references == std::numeric_limits<std::uint32_t>::max())
        return reply(Status::Private);
    if (!hit) {
        images_.emplace(key, image);
        allocated_ += image->allocation_size;
    }
    ++count;
    ++image->references;
    protocol::TextureGrant grant;
    grant.epoch = options_.epoch;
    grant.allocation_size = image->allocation_size;
    grant.memory_index = image->memory_index;
    grant.levels = image->levels;
    grant.hit = hit;
    grant.digest = request.digest;
    return {protocol::encode_texture_grant(grant), image->handle, image};
}

platform::GpuIpcReply GpuCacheBrokerCore::retire(const platform::GpuIpcPeer& peer, const protocol::Request& request) {
    const auto key = to_hex(request.digest);
    std::lock_guard lock(images_mutex_);
    auto client = references_.find(peer.connection_id);
    if (client == references_.end()) throw std::runtime_error("unknown client");
    auto reference = client->second.find(key);
    if (reference == client->second.end() || !reference->second) throw std::runtime_error("no such reference");
    if (--reference->second == 0) client->second.erase(reference);
    drop_reference(key);
    return reply(Status::Ok);
}

} // namespace hailstone::gpu_lab
