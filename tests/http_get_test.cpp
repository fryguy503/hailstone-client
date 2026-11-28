// platform/http_get.h without the network: only http and https addresses are fetched, and asking whether this system
// can fetch at all does not fail; and on the loopback, a stop ends a transfer stalled on a server that never answers.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "platform/http_get.h"
#include "platform/tcp_socket.h"

using namespace hailstone::platform;

namespace {

// Running as a Windows program under Wine (its ntdll exports wine_get_version); the test's only OS call.
bool under_wine() {
#ifdef _WIN32
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
    return false;
#endif
}

HttpGetResult get(const std::string& url) {
    HttpGetOptions options;
    options.user_agent = "hailstone-test";
    bool called = false;
    HttpGetResult r = http_get(url, options, [&](std::span<const std::byte>) { return called = true; }, {});
    CHECK_FALSE(called);
    return r;
}

} // namespace

TEST_CASE("http_get: refuses addresses that are not http or https") {
    for (const char* url : {"ftp://example.com/file", "file:///etc/passwd", "garbage", "", "https:/", "http://",
                            "javascript:alert(1)"}) {
        CAPTURE(url);
        HttpGetResult r = get(url);
        CHECK_FALSE(r.ok);
        CHECK(r.status == 0);
        CHECK_FALSE(r.error.empty());
    }
}

TEST_CASE("http_get: http_available answers and gives a reason when it cannot") {
    std::string why = "unset";
    const bool available = http_available(&why);
    if (available) CHECK(why.empty());
    else CHECK_FALSE(why.empty());
    CHECK(http_available() == available);
}

// Quitting while a fetch waits on a server that took the request and never answers: `cancelled` ends it at once,
// not when the stall timeout (30 s) runs out. The server says it has the request; the stop follows that.
TEST_CASE("http_get: cancelled ends a transfer stalled waiting for the answer") {
    if (!http_available()) return; // no libcurl here: nothing to fetch with
    TcpListener listener;
    std::string error;
    REQUIRE(listener.listen(Endpoint::v4(127, 0, 0, 1, 0), error));
    const auto at = listener.local_endpoint();
    REQUIRE(at);
    std::atomic<bool> got_request{false}, finished{false};
    std::thread server([&] {
        std::optional<TcpSocket> peer;
        std::string request;
        std::vector<std::byte> buf(1024);
        while (!finished) {
            if (!peer) peer = listener.accept();
            if (peer)
                if (auto n = peer->recv(buf); n && *n) {
                    request.append(reinterpret_cast<const char*>(buf.data()), *n);
                    if (request.find("\r\n\r\n") != std::string::npos) got_request = true;
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    HttpGetOptions options;
    options.user_agent = "hailstone-test";
    options.stall_timeout_s = 30;
    options.cancelled = [&] { return got_request.load(); };
    const auto start = std::chrono::steady_clock::now();
    HttpGetResult r = http_get("http://127.0.0.1:" + std::to_string(at->port) + "/stalled", options,
                               [](std::span<const std::byte>) { return true; }, {});
    const auto took = std::chrono::steady_clock::now() - start;
    finished = true;
    server.join();
    CHECK(got_request);
    CHECK_FALSE(r.ok);
    // Wine's WinHTTP does not end a synchronous call when another thread closes its request handle, as Windows'
    // does (WinHttpCloseHandle's documented cancel): there the call runs on (about 21 s) and only the result says
    // "cancelled". Under Wine the time is a warning, not a failure; on Windows and with libcurl it is checked.
    if (under_wine()) WARN(took < std::chrono::seconds(10));
    else CHECK(took < std::chrono::seconds(10));
}

// allow_url holds for an answer with no body too: libcurl's write callback, where the address is checked as the body
// starts, never runs for one, so an empty 2xx from an address it refuses must still fail (as WinHTTP's check, made
// before any body, always did).
TEST_CASE("http_get: allow_url refuses an empty answer from an address it does not allow") {
    if (!http_available()) return; // no libcurl here: nothing to fetch with
    TcpListener listener;
    std::string error;
    REQUIRE(listener.listen(Endpoint::v4(127, 0, 0, 1, 0), error));
    const auto at = listener.local_endpoint();
    REQUIRE(at);
    std::atomic<bool> finished{false};
    std::atomic<int> answered{0};
    std::thread server([&] {
        std::optional<TcpSocket> peer;
        std::string request;
        std::vector<std::byte> buf(1024);
        const std::string answer = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        while (!finished) {
            if (!peer) {
                peer = listener.accept();
                request.clear();
            }
            if (peer)
                if (auto n = peer->recv(buf); n && *n) {
                    request.append(reinterpret_cast<const char*>(buf.data()), *n);
                    if (request.find("\r\n\r\n") != std::string::npos) {
                        peer->send(std::as_bytes(std::span(answer.data(), answer.size())));
                        ++answered;
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        peer.reset();
                    }
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const std::string url = "http://127.0.0.1:" + std::to_string(at->port) + "/empty";
    HttpGetOptions options;
    options.user_agent = "hailstone-test";
    options.stall_timeout_s = 10;
    std::string seen;
    options.allow_url = [&](std::string_view u) {
        seen = std::string(u);
        return false;
    };
    HttpGetResult refused = http_get(url, options, [](std::span<const std::byte>) { return true; }, {});
    options.allow_url = [](std::string_view) { return true; };
    HttpGetResult allowed = http_get(url, options, [](std::span<const std::byte>) { return true; }, {});
    finished = true;
    server.join();
    CHECK(answered == 2);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error == "the download was sent to an address it may not come from");
    CHECK(seen == url);
    CHECK(allowed.ok);
}

TEST_CASE("http_get: the caller can check a redirect before it is followed") {
    if (!http_available()) return;
    TcpListener listener;
    std::string error;
    REQUIRE(listener.listen(Endpoint::v4(127, 0, 0, 1, 0), error));
    const auto endpoint = listener.local_endpoint();
    REQUIRE(endpoint);
    const std::string base = "http://127.0.0.1:" + std::to_string(endpoint->port);
    std::atomic<bool> finished{false};
    std::atomic<int> landed{0};
    std::thread server([&] {
        std::optional<TcpSocket> peer;
        std::string request;
        std::vector<std::byte> buffer(1024);
        while (!finished) {
            if (!peer) {
                peer = listener.accept();
                request.clear();
            }
            if (peer)
                if (auto n = peer->recv(buffer); n && *n) {
                    request.append(reinterpret_cast<const char*>(buffer.data()), *n);
                    if (request.find("\r\n\r\n") != std::string::npos) {
                        const bool redirected = request.starts_with("GET /landing ");
                        if (redirected) ++landed;
                        const std::string answer = redirected
                            ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"
                            : "HTTP/1.1 302 Found\r\nLocation: " + base +
                              "/landing\r\nContent-Length: 5\r\nConnection: close\r\n\r\nmoved";
                        peer->send(std::as_bytes(std::span(answer.data(), answer.size())));
                        peer.reset();
                    }
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    HttpGetOptions options;
    options.user_agent = "hailstone-test";
    options.stall_timeout_s = 10;
    options.follow_redirects = false;
    std::string body;
    auto sink = [&](std::span<const std::byte> piece) {
        body.append(reinterpret_cast<const char*>(piece.data()), piece.size());
        return true;
    };
    const auto redirect = http_get(base + "/redirect", options, sink, {});
    const int requests_before_follow = landed.load();
    const std::string first_body = body;
    options.follow_redirects = true;
    const auto followed = http_get(base + "/redirect", options, sink, {});
    finished = true;
    server.join();
    CHECK_FALSE(redirect.ok);
    CHECK(redirect.status == 302);
    CHECK(redirect.redirect_url == base + "/landing");
    CHECK(requests_before_follow == 0);
    CHECK(first_body.empty());
    CHECK(followed.ok);
    CHECK(followed.redirect_url.empty());
    CHECK(landed == 1);
    CHECK(body == "ok");
}
