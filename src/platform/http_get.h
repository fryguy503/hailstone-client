#pragma once

// One HTTP(S) GET, for the updater (app/updater.h): the system's own HTTPS, so Hailstone ships no TLS library and
// uses the system's certificates. Windows: WinHTTP. Linux and macOS: the system's libcurl, loaded at run time
// (libcurl.so.4, /usr/lib/libcurl.4.dylib); without it there is no fetching (http_available() says why) and the
// client goes on without updates.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace hailstone::platform {

struct HttpGetOptions {
    std::string user_agent;          // required by GitHub
    int connect_timeout_s = 15;
    int stall_timeout_s = 30;        // abort when under 1 byte/s for this long
    std::uint64_t max_bytes = 0;     // 0: no limit; more than this is an error
    // Whether the answer may come from this address: the one the transfer ends on, after its redirects. Checked
    // before any of the body is passed on; a refused one is an error. Empty: any (the add-on manager keeps its
    // downloads on GitHub with it, docs/addons.md).
    std::function<bool(std::string_view url)> allow_url;
    // Asked from another thread (about every 100 ms) while the transfer runs, so a stop ends a call that is waiting
    // on the network too, not only between reads (WinHTTP's calls block until their timeouts): true ends it as
    // cancelled. Empty: only `sink` and `progress` cancel. It must be safe to call from any thread (an atomic flag).
    std::function<bool()> cancelled;
    bool follow_redirects = true;   // false: return the first redirect and let the caller vet its destination
};

struct HttpGetResult {
    bool ok = false;    // a 2xx answer read to the end
    int status = 0;     // the final HTTP status (0: none)
    std::string error;  // why not, for the log and the player
    std::string redirect_url; // Location of an unfollowed 301, 302, 303, 307 or 308 response, if present
};

// Blocking: call from a worker thread. `sink` gets the body in pieces; returning false cancels. `progress` (may be
// empty) gets (received, total or 0 when unknown); returning false cancels. Redirects are followed by default
// (GitHub's release links redirect to another host), at most 10, https only once started on https. Only http:// and
// https:// addresses are fetched. A body is passed to `sink` only for a 2xx answer.
HttpGetResult http_get(const std::string& url, const HttpGetOptions& options,
                       const std::function<bool(std::span<const std::byte>)>& sink,
                       const std::function<bool(std::uint64_t, std::uint64_t)>& progress);

// Whether this system can fetch at all (Linux/macOS: libcurl could be loaded), and why not.
bool http_available(std::string* why = nullptr);

} // namespace hailstone::platform
