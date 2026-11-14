#include <doctest/doctest.h>

#include "core/byte_io.h"
#include "platform/gpu_ipc.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace hailstone;
using namespace hailstone::platform;

TEST_CASE("GPU IPC frames: bounded little-endian headers round trip") {
    using namespace gpu_ipc_wire;
    const std::array headers{Header{Kind::Authenticate, 32, 0, false}, Header{Kind::Authenticated, 0, 0, false},
                             Header{Kind::Request, 0, 1, false},
                             Header{Kind::Request, static_cast<std::uint32_t>(kGpuIpcMaxBody), 42, false},
                             Header{Kind::Response, 7, std::numeric_limits<std::uint64_t>::max(), true}};
    for (const Header& expected : headers) {
        const auto encoded = encode_header(expected);
        REQUIRE(encoded.size() == kHeaderSize);
        CHECK(encoded[0] == std::byte{'H'});
        CHECK(encoded[1] == std::byte{'G'});
        CHECK(encoded[2] == std::byte{'I'});
        CHECK(encoded[3] == std::byte{'P'});
        const auto decoded = decode_header(encoded);
        CHECK(decoded.kind == expected.kind);
        CHECK(decoded.body_size == expected.body_size);
        CHECK(decoded.sequence == expected.sequence);
        CHECK(decoded.has_handle == expected.has_handle);
    }
}

TEST_CASE("GPU IPC frames: malformed headers are rejected before body allocation") {
    using namespace gpu_ipc_wire;
    const auto good = encode_header({Kind::Request, 4, 1, false});
    for (std::size_t n = 0; n < good.size(); ++n) CHECK_THROWS(decode_header(std::span(good).first(n)));
    auto extra = good;
    extra.push_back(std::byte{0});
    CHECK_THROWS(decode_header(extra));
    for (std::size_t offset : {std::size_t{0}, std::size_t{4}, std::size_t{6}, std::size_t{7}}) {
        auto corrupt = good;
        corrupt[offset] = std::byte{0xff};
        CHECK_THROWS(decode_header(corrupt));
    }
    ByteWriter oversized;
    oversized.le<std::uint32_t>(0x50494748);
    oversized.le<std::uint16_t>(1);
    oversized.le<std::uint8_t>(static_cast<std::uint8_t>(Kind::Request));
    oversized.le<std::uint8_t>(0);
    oversized.le<std::uint32_t>(static_cast<std::uint32_t>(kGpuIpcMaxBody + 1));
    oversized.le<std::uint64_t>(1);
    CHECK_THROWS(decode_header(oversized.data()));

    CHECK_THROWS(encode_header({Kind::Authenticate, 31, 0, false}));
    CHECK_THROWS(encode_header({Kind::Authenticate, 32, 1, false}));
    CHECK_THROWS(encode_header({Kind::Authenticate, 32, 0, true}));
    CHECK_THROWS(encode_header({Kind::Authenticated, 1, 0, false}));
    CHECK_THROWS(encode_header({Kind::Request, 0, 0, false}));
    CHECK_THROWS(encode_header({Kind::Request, 0, 1, true}));
    CHECK_THROWS(encode_header({Kind::Response, 0, 0, false}));
}

TEST_CASE("GPU IPC handles: ownership is move-only and empty handles are harmless") {
    static_assert(!std::is_copy_constructible_v<TypedGpuIpcHandle>);
    static_assert(!std::is_copy_assignable_v<TypedGpuIpcHandle>);
    static_assert(std::is_nothrow_move_constructible_v<TypedGpuIpcHandle>);
    static_assert(std::is_nothrow_move_assignable_v<TypedGpuIpcHandle>);
    auto a = TypedGpuIpcHandle::adopt(nullptr);
    CHECK_FALSE(a);
    CHECK(a.get() == nullptr);
    auto b = std::move(a);
    CHECK_FALSE(a);
    CHECK_FALSE(b);
    a = std::move(b);
    CHECK(a.native_value() == 0);
    CHECK(a.release() == nullptr);
    a.reset();
}

TEST_CASE("GPU IPC sessions: malformed local endpoints and tokens fail without exposing values") {
    GpuIpcSession invalid{"remote-or-invalid-endpoint", std::string(64, 'a'), 0};
    try {
        (void)GpuIpcClient::connect(invalid, 50);
        FAIL("invalid endpoint accepted");
    } catch (const std::exception& error) {
        const std::string message = error.what();
        const bool leaked = message.find(invalid.endpoint) != std::string::npos || message.find(invalid.token) != std::string::npos;
        CHECK_FALSE(leaked);
    }
    invalid.endpoint = R"(\\.\pipe\hailstone-gpu-)" + std::string(32, 'a');
    invalid.token = "not-a-session-token";
    CHECK_THROWS(GpuIpcClient::connect(invalid, 50));
}

#if defined(_WIN32)
TEST_CASE("GPU IPC Windows: authenticated peers, first-instance ownership and disconnect callbacks") {
    std::atomic<unsigned> disconnected = 0;
    auto session = make_gpu_ipc_session();
    auto other = make_gpu_ipc_session();
    CHECK(session.token.size() == 64);
    const bool token_distinct = session.token != other.token;
    const bool endpoint_distinct = session.endpoint != other.endpoint;
    CHECK(token_distinct);
    CHECK(endpoint_distinct);
    auto echo = [](const GpuIpcPeer& peer, std::span<const std::byte> body) {
        ByteWriter out;
        out.le(peer.process_id);
        out.le(peer.connection_id);
        out.bytes(body);
        return GpuIpcReply{out.take(), nullptr, {}};
    };
    auto server = GpuIpcServer::start(session, echo, [&](const GpuIpcPeer&) { ++disconnected; }, 2);
    CHECK_THROWS(GpuIpcServer::start(server->session(), echo));
    auto bad_token = server->session();
    bad_token.token[0] = bad_token.token[0] == '0' ? '1' : '0';
    CHECK_THROWS(GpuIpcClient::connect(bad_token, 2000));
    auto wrong_process = server->session();
    ++wrong_process.server_process_id;
    CHECK_THROWS(GpuIpcClient::connect(wrong_process, 2000));

    auto a = GpuIpcClient::connect(server->session());
    auto b = GpuIpcClient::connect(server->session());
    const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}};
    const auto reply_a = a->call(payload), reply_b = b->call(payload);
    CHECK_FALSE(reply_a.handle);
    CHECK_FALSE(reply_b.handle);
    ByteReader in_a(reply_a.body), in_b(reply_b.body);
    const auto pid_a = in_a.le<std::uint32_t>(), pid_b = in_b.le<std::uint32_t>();
    CHECK(pid_a > 0);
    CHECK(pid_a == pid_b);
    CHECK(in_a.le<std::uint64_t>() != in_b.le<std::uint64_t>());
    CHECK(std::ranges::equal(in_a.bytes(payload.size()), payload));
    CHECK(std::ranges::equal(in_b.bytes(payload.size()), payload));
    a->close();
    b->close();
    server->stop();
    CHECK(disconnected.load() == 2);
    CHECK_THROWS(a->call({}));
}

TEST_CASE("GPU IPC Windows: concurrent calls are serialized per connection") {
    auto server = GpuIpcServer::start(make_gpu_ipc_session(),
        [](const GpuIpcPeer&, std::span<const std::byte> body) { return GpuIpcReply{{body.begin(), body.end()}, nullptr, {}}; }, {}, 1);
    auto client = GpuIpcClient::connect(server->session());
    std::atomic<bool> valid = true;
    std::vector<std::thread> callers;
    for (unsigned i = 0; i < 4; ++i) callers.emplace_back([&, i] {
        try {
            const std::array message{static_cast<std::byte>(i)};
            for (unsigned n = 0; n < 8; ++n) {
                const auto reply = client->call(message);
                if (!std::ranges::equal(reply.body, message) || reply.handle) valid = false;
            }
        } catch (...) { valid = false; }
    });
    for (auto& thread : callers) thread.join();
    CHECK(valid.load());
    client->close();
    server->stop();
}

TEST_CASE("GPU IPC Windows: cancellation and request deadlines close uncertain channels") {
    for (bool cancel : {false, true}) {
        std::promise<void> entered, release;
        auto ready = entered.get_future();
        auto proceed = release.get_future().share();
        auto server = GpuIpcServer::start(make_gpu_ipc_session(), [&](const GpuIpcPeer&, std::span<const std::byte>) {
            entered.set_value();
            (void)proceed.wait_for(std::chrono::seconds(3));
            return GpuIpcReply{};
        }, {}, 1);
        auto client = GpuIpcClient::connect(server->session());
        auto request = std::async(std::launch::async, [&] {
            try { (void)client->call({}, cancel ? 5000 : 500); return false; } catch (...) { return true; }
        });
        const bool handler_entered = ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        CHECK(handler_entered);
        if (cancel) client->close();
        const bool completed = request.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        CHECK(completed);
        release.set_value();
        CHECK(request.get());
        client->close();
        server->stop();
    }
}
#else
TEST_CASE("GPU IPC: unsupported platforms expose explicit private-renderer fallback") {
    CHECK_FALSE(gpu_ipc_available());
    CHECK_THROWS(make_gpu_ipc_session());
    const GpuIpcSession session{R"(\\.\pipe\hailstone-gpu-)" + std::string(32, '0'), std::string(64, '0'), 0};
    CHECK_THROWS(GpuIpcClient::connect(session));
    CHECK_THROWS(GpuIpcServer::start(session, [](const GpuIpcPeer&, std::span<const std::byte>) { return GpuIpcReply{}; }));
}
#endif
