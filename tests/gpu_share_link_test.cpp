// The client's reactions to the GPU texture broker (render/gpu_share_link.h), against a scripted transport: any
// failure detaches sharing for this client alone and the game goes on with private textures.

#include <doctest/doctest.h>

#include <deque>
#include <stdexcept>
#include <vector>

#include "core/byte_io.h"
#include "manual_clock.h"
#include "render/gpu_share_link.h"

using namespace hailstone;
using namespace hailstone::render;
using gpu_cache::Op;
using gpu_cache::Status;

namespace {

gpu_cache::Identity identity() {
    gpu_cache::Identity out;
    out.model = "Test GPU";
    out.api = 1;
    out.driver = 2;
    return out;
}

// Replies in order; an empty body stands for a transport failure (the call throws).
struct Script {
    std::deque<std::vector<std::byte>> replies;
    std::vector<Op> sent;
    int closed = 0;

    GpuShareLink link(test::ManualClock* clock = nullptr) {
        GpuShareLink::Clock source;
        if (clock) source = clock->source();
        return GpuShareLink(
            [this](std::span<const std::byte> body, int) {
                sent.push_back(gpu_cache::decode_request(body).op);
                if (replies.empty()) throw std::runtime_error("no reply scripted");
                auto reply = std::move(replies.front());
                replies.pop_front();
                if (reply.empty()) throw std::runtime_error("broker gone");
                return platform::GpuIpcResult{std::move(reply), {}};
            },
            [this] { ++closed; }, source);
    }
    void hello_ok() { replies.push_back(gpu_cache::encode_hello_reply({identity(), 77})); }
    void status(Status s) { replies.push_back(gpu_cache::encode_status(s)); }
    void lease(std::uint64_t serial) { replies.push_back(gpu_cache::encode_lease_grant({77, serial})); }
    void failure() { replies.push_back({}); }
};

} // namespace

TEST_CASE("GPU share link: the handshake connects, a different GPU or a failure leaves textures private") {
    Script ok;
    ok.hello_ok();
    auto connected = ok.link();
    CHECK(connected.hello(identity()));
    CHECK(connected.connected());
    CHECK(connected.detach_reason().empty());

    Script other;
    other.status(Status::Private);
    auto declined = other.link();
    CHECK_FALSE(declined.hello(identity()));
    CHECK_FALSE(declined.connected());
    CHECK(other.closed == 1);
    CHECK_FALSE(declined.detach_reason().empty());

    Script gone;
    gone.failure();
    auto failed = gone.link();
    CHECK_FALSE(failed.hello(identity()));
    CHECK(gone.closed == 1);

    Script wrong;
    auto changed = identity();
    changed.driver = 99;
    wrong.replies.push_back(gpu_cache::encode_hello_reply({changed, 77}));
    auto mismatched = wrong.link();
    CHECK_FALSE(mismatched.hello(identity()));
}

TEST_CASE("GPU share link: a declined texture is private while sharing carries on") {
    Script s;
    s.hello_ok();
    s.status(Status::Private);
    s.lease(1);
    auto link = s.link();
    REQUIRE(link.hello(identity()));
    const std::vector<std::byte> recipe(8, std::byte{1});
    CHECK_FALSE(link.texture({}, recipe));
    CHECK(link.connected());
    CHECK(link.acquire() == std::optional<std::uint64_t>(1));
}

TEST_CASE("GPU share link: an invalid reply, a grant without a handle or a lost broker detaches for good") {
    const std::vector<std::byte> recipe(8, std::byte{1});
    for (int kind = 0; kind < 3; ++kind) {
        Script s;
        s.hello_ok();
        if (kind == 0) s.status(Status::Invalid);
        if (kind == 1) s.replies.push_back(gpu_cache::encode_texture_grant({77, 4096, 0, 1, false, {}}));
        if (kind == 2) s.failure();
        auto link = s.link();
        REQUIRE(link.hello(identity()));
        CHECK_FALSE(link.texture({}, recipe));
        CHECK_FALSE(link.connected());
        CHECK(s.closed == 1);
        CHECK_FALSE(link.detach_reason().empty());
        // Nothing more goes to the broker: textures, leases and retirements are all private from here on.
        const auto sent = s.sent.size();
        CHECK_FALSE(link.texture({}, recipe));
        CHECK_FALSE(link.acquire());
        CHECK_FALSE(link.release(1));
        link.retire({});
        link.keep_alive();
        CHECK(s.sent.size() == sent);
        CHECK(s.closed == 1);
    }
}

TEST_CASE("GPU share link: a lease not free in time detaches this client instead of stopping it") {
    Script s;
    s.hello_ok();
    s.status(Status::Busy);
    auto link = s.link();
    REQUIRE(link.hello(identity()));
    CHECK_FALSE(link.acquire());
    CHECK_FALSE(link.connected());
    CHECK(s.closed == 1); // the broker sees the disconnect and lets go of this client's references
}

TEST_CASE("GPU share link: a lease from another session or a refused release detaches") {
    Script s;
    s.hello_ok();
    s.replies.push_back(gpu_cache::encode_lease_grant({78, 1}));
    auto link = s.link();
    REQUIRE(link.hello(identity()));
    CHECK_FALSE(link.acquire());
    CHECK_FALSE(link.connected());

    Script r;
    r.hello_ok();
    r.lease(4);
    r.status(Status::Invalid);
    auto other = r.link();
    REQUIRE(other.hello(identity()));
    const auto serial = other.acquire();
    REQUIRE(serial);
    CHECK_FALSE(other.release(*serial));
    CHECK_FALSE(other.connected());
}

TEST_CASE("GPU share link: a quiet connection pings before the broker's idle limit") {
    test::ManualClock clock;
    clock.hold();
    Script s;
    s.hello_ok();
    s.status(Status::Ok);
    s.failure();
    auto link = s.link(&clock);
    REQUIRE(link.hello(identity()));
    link.keep_alive();
    CHECK(s.sent.size() == 1);
    clock.advance(std::chrono::seconds(59));
    link.keep_alive();
    CHECK(s.sent.size() == 1);
    clock.advance(std::chrono::seconds(1));
    link.keep_alive();
    REQUIRE(s.sent.size() == 2);
    CHECK(s.sent.back() == Op::Ping);
    CHECK(link.connected());
    clock.advance(std::chrono::seconds(30));
    link.keep_alive();
    CHECK(s.sent.size() == 2);
    clock.advance(std::chrono::seconds(30));
    link.keep_alive(); // the broker is gone: detached, not thrown
    CHECK_FALSE(link.connected());
    CHECK(s.closed == 1);
}
