// The GPU texture broker's bookkeeping (tools/gpu_lab/gpu_cache_broker_core.h) and, through an in-process loopback,
// the clients' links to it: one client's bad request, crash, lost lease or timeout stays that client's.

#include <doctest/doctest.h>

#include <future>
#include <stdexcept>

#include "gpu_lab/gpu_cache_broker_core.h"
#include "render/gpu_share_link.h"

using namespace hailstone;
using namespace hailstone::gpu_lab;
using render::gpu_cache::Op;
using render::gpu_cache::Status;
namespace protocol = render::gpu_cache;

namespace {

protocol::Identity identity() {
    protocol::Identity out;
    out.model = "Test GPU";
    out.api = 1;
    out.driver = 2;
    return out;
}

constexpr std::uint64_t kEpoch = 41;

int dummy_handle = 0; // the core only passes the handle along

struct Fixture {
    int uploads = 0;
    bool fail_upload = false;
    GpuCacheBrokerCore core;

    explicit Fixture(std::chrono::milliseconds lease_wait = std::chrono::milliseconds(0))
        : core(options(lease_wait), [this](const assets::Image&, bool, std::uint64_t maximum) {
              ++uploads;
              if (fail_upload) throw std::runtime_error("VK_ERROR_OUT_OF_DEVICE_MEMORY");
              auto image = std::make_shared<BrokerImage>();
              image->allocation_size = std::min<std::uint64_t>(4096, maximum);
              image->levels = 1;
              image->handle = &dummy_handle;
              image->owner = std::make_shared<int>(0);
              return image;
          }) {}

    static GpuCacheBrokerCore::Options options(std::chrono::milliseconds lease_wait) {
        GpuCacheBrokerCore::Options o;
        o.identity = identity();
        o.epoch = kEpoch;
        o.lease_wait = lease_wait;
        return o;
    }

    platform::GpuIpcReply send(std::uint64_t peer, const protocol::Request& request) {
        return core.handle({1, peer}, protocol::encode_request(request));
    }
    static Status status_of(const platform::GpuIpcReply& reply) {
        ByteReader in(reply.body);
        return protocol::read_status(in);
    }
    Status hello(std::uint64_t peer) {
        protocol::Request r;
        r.op = Op::Hello;
        r.identity = identity();
        return status_of(send(peer, r));
    }
    // The serial, or 0 when not granted.
    std::uint64_t acquire(std::uint64_t peer, Status* status = nullptr) {
        protocol::Request r;
        r.op = Op::Acquire;
        r.epoch = kEpoch;
        const auto reply = send(peer, r);
        ByteReader in(reply.body);
        const auto s = protocol::read_status(in);
        if (status) *status = s;
        return s == Status::Ok ? protocol::read_lease_grant(in).serial : 0;
    }
    Status release(std::uint64_t peer, std::uint64_t serial) {
        protocol::Request r;
        r.op = Op::Release;
        r.epoch = kEpoch;
        r.serial = serial;
        return status_of(send(peer, r));
    }
    Status texture(std::uint64_t peer, std::uint8_t shade) {
        assets::Image image;
        image.width = image.height = 2;
        image.mips.emplace_back(16, std::byte{shade});
        const auto recipe = protocol::encode_texture(image, false);
        protocol::Request r;
        r.op = Op::Texture;
        r.digest = protocol::texture_key(image, false);
        r.recipe = recipe;
        return status_of(send(peer, r));
    }
    void disconnect(std::uint64_t peer) { core.disconnect({1, peer}); }
};

} // namespace

TEST_CASE("GPU broker lease: one owner at a time, mistakes change nothing") {
    GpuCacheLeaseState state;
    CHECK(state.register_peer(1));
    CHECK(state.register_peer(2));
    CHECK_FALSE(state.register_peer(0));
    CHECK_FALSE(state.register_peer(2));
    using Kind = GpuCacheLeaseState::Acquired::Kind;
    CHECK(state.acquire(3).kind == Kind::Invalid);
    const auto first = state.acquire(1);
    REQUIRE(first.kind == Kind::Granted);
    CHECK(state.acquire(2).kind == Kind::Busy);
    CHECK(state.acquire(1).kind == Kind::Invalid); // asking twice is refused, the lease stays
    CHECK(state.owner() == 1);
    CHECK_FALSE(state.release(2, first.serial));  // not the owner
    CHECK_FALSE(state.release(1, first.serial + 1)); // not its serial
    CHECK(state.owner() == 1);
    CHECK(state.release(1, first.serial));
    const auto next = state.acquire(2);
    REQUIRE(next.kind == Kind::Granted);
    CHECK(next.serial > first.serial);
}

TEST_CASE("GPU broker lease: a peer that disconnects holding the lease gives it back") {
    GpuCacheLeaseState state;
    state.register_peer(1);
    state.register_peer(2);
    REQUIRE(state.acquire(1).kind == GpuCacheLeaseState::Acquired::Kind::Granted);
    CHECK(state.disconnect(1));
    CHECK(state.owner() == 0);
    CHECK_FALSE(state.has_peer(1));
    CHECK(state.acquire(2).kind == GpuCacheLeaseState::Acquired::Kind::Granted);
    CHECK_FALSE(state.disconnect(9));
}

TEST_CASE("GPU broker: a malformed request fails alone") {
    Fixture f;
    REQUIRE(f.hello(1) == Status::Ok);
    REQUIRE(f.hello(2) == Status::Ok);
    const std::vector<std::byte> garbage(13, std::byte{0xee});
    CHECK(Fixture::status_of(f.core.handle({1, 1}, garbage)) == Status::Invalid);
    CHECK(Fixture::status_of(f.core.handle({1, 1}, {})) == Status::Invalid);
    // A forged texture: the digest does not match the recipe.
    assets::Image image;
    image.width = image.height = 2;
    image.mips.emplace_back(16, std::byte{1});
    const auto recipe = protocol::encode_texture(image, false);
    protocol::Request forged;
    forged.op = Op::Texture;
    forged.recipe = recipe;
    CHECK(Fixture::status_of(f.send(1, forged)) == Status::Invalid);
    // Out of turn: a stale epoch, a peer that never said hello, a retirement of nothing.
    protocol::Request stale;
    stale.op = Op::Acquire;
    stale.epoch = kEpoch + 1;
    CHECK(Fixture::status_of(f.send(1, stale)) == Status::Invalid);
    Status status{};
    CHECK(f.acquire(5, &status) == 0);
    CHECK(status == Status::Invalid);
    protocol::Request retire;
    retire.op = Op::RetireTexture;
    retire.epoch = kEpoch;
    CHECK(Fixture::status_of(f.send(2, retire)) == Status::Invalid);
    CHECK(f.core.stats().failed_requests == 6);
    // Both clients go on as before, the one that sent them included.
    const auto serial = f.acquire(1);
    REQUIRE(serial);
    CHECK(f.release(1, serial) == Status::Ok);
    const auto other = f.acquire(2);
    REQUIRE(other);
    CHECK(f.release(2, other) == Status::Ok);
    CHECK(f.texture(1, 3) == Status::Ok);
}

TEST_CASE("GPU broker: a GPU failure declines that texture only") {
    Fixture f;
    REQUIRE(f.hello(1) == Status::Ok);
    f.fail_upload = true;
    CHECK(f.texture(1, 1) == Status::Private);
    f.fail_upload = false;
    CHECK(f.texture(1, 1) == Status::Ok);
    CHECK(f.texture(1, 2) == Status::Ok);
    const auto serial = f.acquire(1);
    CHECK(serial);
    CHECK(f.uploads == 3);
}

TEST_CASE("GPU broker: a wrong release or a second acquire is refused and the owner keeps the lease") {
    Fixture f;
    REQUIRE(f.hello(1) == Status::Ok);
    REQUIRE(f.hello(2) == Status::Ok);
    const auto serial = f.acquire(1);
    REQUIRE(serial);
    Status status{};
    CHECK(f.acquire(1, &status) == 0);
    CHECK(status == Status::Invalid);
    CHECK(f.release(2, serial) == Status::Invalid);
    CHECK(f.release(1, serial + 1) == Status::Invalid);
    CHECK(f.core.stats().owner == 1);
    CHECK(f.release(1, serial) == Status::Ok);
    CHECK(f.acquire(2));
}

TEST_CASE("GPU broker: a lease not free in time is Busy for the waiter only") {
    Fixture f; // no wait at all
    REQUIRE(f.hello(1) == Status::Ok);
    REQUIRE(f.hello(2) == Status::Ok);
    const auto serial = f.acquire(1);
    REQUIRE(serial);
    Status status{};
    CHECK(f.acquire(2, &status) == 0);
    CHECK(status == Status::Busy);
    CHECK(f.core.stats().owner == 1);
    CHECK(f.release(1, serial) == Status::Ok);
    CHECK(f.acquire(2));
}

TEST_CASE("GPU broker: a client that disconnects holding the lease releases it to the waiting one") {
    Fixture f(std::chrono::seconds(30)); // the bound is never reached: the disconnect wakes the waiter
    REQUIRE(f.hello(1) == Status::Ok);
    REQUIRE(f.hello(2) == Status::Ok);
    REQUIRE(f.texture(1, 1) == Status::Ok);
    REQUIRE(f.acquire(1));
    auto waiter = std::async(std::launch::async, [&] { return f.acquire(2); });
    f.disconnect(1); // crashed, or detached after its own fence timed out
    CHECK(waiter.get() != 0);
    const auto stats = f.core.stats();
    CHECK(stats.owner == 2);
    CHECK(stats.consumers == 1);
    CHECK(stats.entries == 0); // its texture went with it
}

TEST_CASE("GPU broker: images live while any client holds them and stop() refuses new clients") {
    Fixture f;
    REQUIRE(f.hello(1) == Status::Ok);
    REQUIRE(f.hello(2) == Status::Ok);
    CHECK(f.texture(1, 1) == Status::Ok);
    CHECK(f.texture(2, 1) == Status::Ok);
    CHECK(f.uploads == 1);
    CHECK(f.core.stats().hits == 1);
    f.disconnect(1);
    CHECK(f.core.stats().entries == 1);
    f.disconnect(2);
    CHECK(f.core.idle_and_empty());
    f.core.stop();
    CHECK(f.hello(3) == Status::Private);
}

namespace {

// A link whose transport is the broker core itself; closing it is the disconnect the transport would report.
struct Loopback {
    GpuCacheBrokerCore& core;
    std::uint64_t peer;
    render::GpuShareLink link;
    Loopback(GpuCacheBrokerCore& c, std::uint64_t p)
        : core(c), peer(p),
          link([this](std::span<const std::byte> body, int) {
                   auto reply = core.handle({1, peer}, body);
                   return platform::GpuIpcResult{std::move(reply.body), {}};
               },
               [this] { core.disconnect({1, peer}); }) {}
};

} // namespace

TEST_CASE("GPU sharing: one client's lost lease or timeout leaves the others playing") {
    Fixture f;
    Loopback a(f.core, 1), b(f.core, 2), c(f.core, 3);
    REQUIRE(a.link.hello(identity()));
    REQUIRE(b.link.hello(identity()));
    REQUIRE(c.link.hello(identity()));
    const auto serial = a.link.acquire();
    REQUIRE(serial);
    // b finds the lease taken past the wait: b alone falls back to private textures.
    CHECK_FALSE(b.link.acquire());
    CHECK_FALSE(b.link.connected());
    CHECK(a.link.connected());
    CHECK(c.link.connected());
    CHECK(f.core.stats().consumers == 2);
    REQUIRE(a.link.release(*serial));
    // a's frame fence times out while it holds the lease: it drops the lease by detaching.
    REQUIRE(a.link.acquire());
    a.link.detach("frame fence timed out");
    CHECK(f.core.stats().owner == 0);
    const auto next = c.link.acquire();
    REQUIRE(next);
    CHECK(c.link.release(*next));
    CHECK(c.link.connected());
}
