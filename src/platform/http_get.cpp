#include "platform/http_get.h"

#include <algorithm>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#else
#include <SDL3/SDL_loadso.h>
#include <mutex>
#endif

namespace hailstone::platform {

namespace {

// "http" or "https" for an address this fetches; "" for any other.
std::string_view scheme_of(const std::string& url) {
    auto starts = [&](std::string_view p) {
        if (url.size() <= p.size()) return false;
        for (std::size_t i = 0; i < p.size(); ++i) {
            char c = url[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != p[i]) return false;
        }
        return true;
    };
    if (starts("https://")) return "https";
    if (starts("http://")) return "http";
    return {};
}

bool is_2xx(int status) { return status >= 200 && status < 300; }

bool is_redirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

std::string status_error(int status) { return "the server answered HTTP " + std::to_string(status); }

} // namespace

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string win_error(const char* what) {
    return std::string(what) + " failed (WinHTTP error " + std::to_string(GetLastError()) + ")";
}

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET v) : h(v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

// The request handle, and a watcher that ends a blocking WinHTTP call once `cancelled` says so: closing the request
// handle from another thread is how WinHTTP cancels a synchronous request (the call it is in fails). Every call on
// the handle goes through call(), so the watcher closes it only while a call is in progress (WinHTTP keeps the
// request alive until that call returns), and once it has, no further call is made on it: the handle's value may
// already name another thread's new request (the updater and the add-on manager fetch at once). The one window
// left is between call() marking itself in progress and WinHTTP taking the handle, a few instructions, against the
// watcher's 100 ms polls. The handle is closed once: by the watcher, or here at the end.
class Request {
public:
    Request(HINTERNET h, const std::function<bool()>& cancelled) : h_(h), cancelled_(cancelled) {
        if (!h_ || !cancelled_) return;
        thread_ = std::thread([this] {
            std::unique_lock lock(mutex_);
            while (!finished_) {
                if (!stop_ && cancelled_()) stop_ = true;
                if (stop_ && in_call_) {
                    WinHttpCloseHandle(h_);
                    closed_ = true;
                    return;
                }
                cv_.wait_for(lock, std::chrono::milliseconds(100));
            }
        });
    }
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    ~Request() {
        finish();
        if (h_ && !closed_) WinHttpCloseHandle(h_);
    }
    explicit operator bool() const { return h_ != nullptr; }

    // f(handle), unless the transfer was cancelled first; false when it was cancelled before or during the call
    // (`*result` is f's answer when f ran).
    template <class F>
    bool call(F&& f, BOOL* result) {
        {
            std::lock_guard lock(mutex_);
            if (stop_ || closed_) {
                refused_ = true;
                return false;
            }
            in_call_ = true;
        }
        *result = f(h_);
        std::lock_guard lock(mutex_);
        in_call_ = false;
        return !closed_;
    }
    // Stops watching; true when the transfer was cancelled (a call was ended or not made because of it).
    bool finish() {
        if (thread_.joinable()) {
            {
                std::lock_guard lock(mutex_);
                finished_ = true;
            }
            cv_.notify_all();
            thread_.join();
        }
        return refused_ || closed_;
    }

private:
    HINTERNET h_ = nullptr;
    const std::function<bool()>& cancelled_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool finished_ = false, stop_ = false, in_call_ = false, closed_ = false, refused_ = false;
    std::thread thread_;
};

} // namespace

bool http_available(std::string* why) {
    if (why) why->clear();
    return true;
}

HttpGetResult http_get(const std::string& url, const HttpGetOptions& options,
                       const std::function<bool(std::span<const std::byte>)>& sink,
                       const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
    HttpGetResult r;
    const std::string_view scheme = scheme_of(url);
    if (scheme.empty()) {
        r.error = "not an http or https address";
        return r;
    }
    std::wstring wurl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wurl.empty() || !WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &parts)) {
        r.error = "not a valid address";
        return r;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const bool secure = scheme == "https";

    const std::wstring agent = widen(options.user_agent);
    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (the system's proxy settings) is Windows 8.1 and later.
    Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) {
        r.error = win_error("WinHttpOpen");
        return r;
    }
    const int connect_ms = std::max(1, options.connect_timeout_s) * 1000;
    const int stall_ms = std::max(1, options.stall_timeout_s) * 1000;
    WinHttpSetTimeouts(session.h, connect_ms, connect_ms, stall_ms, stall_ms);
    DWORD redirects = 10;
    WinHttpSetOption(session.h, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects));

    Handle connection(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
    if (!connection.h) {
        r.error = win_error("WinHttpConnect");
        return r;
    }
    // Declared last, so its handle is closed (and its watcher stopped) before the connection's and the session's.
    Request request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0),
                    options.cancelled);
    if (!request) {
        r.error = win_error("WinHttpOpenRequest");
        return r;
    }
    // Whatever failed once the transfer was cancelled failed because it was.
    auto done = [&](HttpGetResult res) {
        if (request.finish()) {
            res.ok = false;
            res.error = "cancelled";
            res.redirect_url.clear();
        }
        return res;
    };
    // A WinHTTP call on the request; false (and r.error set) when it failed or the transfer was cancelled.
    auto winhttp = [&](const char* what, auto&& f) {
        BOOL ok = FALSE;
        if (!request.call(f, &ok)) {
            r.error = "cancelled";
            return false;
        }
        if (!ok) r.error = win_error(what);
        return ok != FALSE;
    };
    // WinHTTP follows redirects itself and by default never from https to http (WINHTTP_OPTION_REDIRECT_POLICY).
    if (!options.follow_redirects) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!winhttp("WinHttpSetOption", [&](HINTERNET h) {
                return WinHttpSetOption(h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
            }))
            return done(r);
    }
    if (!winhttp("WinHttpSendRequest", [](HINTERNET h) {
            return WinHttpSendRequest(h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        }))
        return done(r);
    if (!winhttp("WinHttpReceiveResponse", [](HINTERNET h) { return WinHttpReceiveResponse(h, nullptr); }))
        return done(r);
    DWORD status = 0, size = sizeof(status);
    if (!winhttp("WinHttpQueryHeaders", [&](HINTERNET h) {
            return WinHttpQueryHeaders(h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                       WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        }))
        return done(r);
    r.status = static_cast<int>(status);
    if (!is_2xx(r.status)) {
        r.error = status_error(r.status);
        if (!options.follow_redirects && is_redirect(r.status)) {
            DWORD location_size = 0;
            BOOL ignored = FALSE;
            if (!request.call([&](HINTERNET h) {
                    return WinHttpQueryHeaders(h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                               nullptr, &location_size, WINHTTP_NO_HEADER_INDEX);
                }, &ignored))
                return done(r);
            if (location_size > 0) {
                std::wstring location(location_size / sizeof(wchar_t), L'\0');
                if (winhttp("WinHttpQueryHeaders", [&](HINTERNET h) {
                        return WinHttpQueryHeaders(h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                                   location.data(), &location_size, WINHTTP_NO_HEADER_INDEX);
                    }))
                    r.redirect_url = narrow(std::wstring_view(location).substr(0, location_size / sizeof(wchar_t)));
            }
        }
        return done(r);
    }
    if (options.allow_url) {
        // The address the redirects ended on.
        DWORD url_size = 0;
        BOOL ignored = FALSE;
        if (!request.call([&](HINTERNET h) { return WinHttpQueryOption(h, WINHTTP_OPTION_URL, nullptr, &url_size); },
                          &ignored))
            return done(r);
        std::wstring final_url(url_size / sizeof(wchar_t) + 1, L'\0');
        url_size = static_cast<DWORD>(final_url.size() * sizeof(wchar_t));
        std::string address;
        if (winhttp("WinHttpQueryOption", [&](HINTERNET h) {
                return WinHttpQueryOption(h, WINHTTP_OPTION_URL, final_url.data(), &url_size);
            })) {
            address = narrow(std::wstring_view(final_url).substr(0, url_size / sizeof(wchar_t)));
        }
        if (r.error == "cancelled") return done(r);
        if (address.empty() || !options.allow_url(address)) {
            r.error = "the download was sent to an address it may not come from";
            return done(r);
        }
    }
    // The length is a 64-bit number as text (the numeric query is only 32 bits).
    std::uint64_t total = 0;
    wchar_t length[32] = {};
    DWORD length_size = sizeof(length);
    BOOL have_length = FALSE;
    if (!request.call(
            [&](HINTERNET h) {
                return WinHttpQueryHeaders(h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length,
                                           &length_size, WINHTTP_NO_HEADER_INDEX);
            },
            &have_length))
        return done(r);
    if (have_length) {
        for (const wchar_t* p = length; *p >= L'0' && *p <= L'9'; ++p)
            total = total * 10 + static_cast<std::uint64_t>(*p - L'0');
    }
    if (options.max_bytes && total > options.max_bytes) {
        r.error = "the answer is larger than allowed";
        return done(r);
    }

    std::vector<std::byte> buffer(64 * 1024);
    std::uint64_t received = 0;
    if (progress && !progress(0, total)) {
        r.error = "cancelled";
        return done(r);
    }
    for (;;) {
        DWORD got = 0;
        if (!winhttp("WinHttpReadData", [&](HINTERNET h) {
                return WinHttpReadData(h, buffer.data(), static_cast<DWORD>(buffer.size()), &got);
            }))
            return done(r);
        if (got == 0) break;
        received += got;
        if (options.max_bytes && received > options.max_bytes) {
            r.error = "the answer is larger than allowed";
            return done(r);
        }
        if (!sink(std::span<const std::byte>(buffer.data(), got)) || (progress && !progress(received, total))) {
            r.error = "cancelled";
            return done(r);
        }
    }
    if (total && received != total) {
        r.error = "the answer ended early";
        return done(r);
    }
    r.ok = true;
    return done(r);
}

#else // libcurl

namespace {

// libcurl's API, declared here as the library is loaded at run time. Every value is from curl/curl.h (libcurl
// 8.23.0): an option is its type base (CURLOPTTYPE_LONG 0, OBJECTPOINT 10000, FUNCTIONPOINT 20000) plus its number.
using CurlHandle = void;
constexpr int kOptWriteData = 10001;         // CURLOPT_WRITEDATA (CBPOINT, 1)
constexpr int kOptUrl = 10002;               // CURLOPT_URL (STRINGPOINT, 2)
constexpr int kOptWriteFunction = 20011;     // CURLOPT_WRITEFUNCTION (FUNCTIONPOINT, 11)
constexpr int kOptUserAgent = 10018;         // CURLOPT_USERAGENT (STRINGPOINT, 18)
constexpr int kOptLowSpeedLimit = 19;        // CURLOPT_LOW_SPEED_LIMIT (LONG, 19)
constexpr int kOptLowSpeedTime = 20;         // CURLOPT_LOW_SPEED_TIME (LONG, 20)
constexpr int kOptNoProgress = 43;           // CURLOPT_NOPROGRESS (LONG, 43)
constexpr int kOptFollowLocation = 52;       // CURLOPT_FOLLOWLOCATION (LONG, 52)
constexpr int kOptXferInfoData = 10057;      // CURLOPT_XFERINFODATA (CBPOINT, 57)
constexpr int kOptMaxRedirs = 68;            // CURLOPT_MAXREDIRS (LONG, 68)
constexpr int kOptConnectTimeout = 78;       // CURLOPT_CONNECTTIMEOUT (LONG, 78)
constexpr int kOptNoSignal = 99;            // CURLOPT_NOSIGNAL (LONG, 99)
constexpr int kOptProtocols = 181;           // CURLOPT_PROTOCOLS (LONG, 181; deprecated by PROTOCOLS_STR)
constexpr int kOptRedirProtocols = 182;      // CURLOPT_REDIR_PROTOCOLS (LONG, 182)
constexpr int kOptXferInfoFunction = 20219;  // CURLOPT_XFERINFOFUNCTION (FUNCTIONPOINT, 219)
constexpr int kOptProtocolsStr = 10318;      // CURLOPT_PROTOCOLS_STR (STRINGPOINT, 318; libcurl 7.85.0)
constexpr int kOptRedirProtocolsStr = 10319; // CURLOPT_REDIR_PROTOCOLS_STR (STRINGPOINT, 319)
constexpr int kInfoResponseCode = 0x200002;  // CURLINFO_RESPONSE_CODE (CURLINFO_LONG 0x200000 + 2)
constexpr int kInfoEffectiveUrl = 0x100001;  // CURLINFO_EFFECTIVE_URL (CURLINFO_STRING 0x100000 + 1)
constexpr int kInfoRedirectUrl = 0x10001f;   // CURLINFO_REDIRECT_URL (CURLINFO_STRING 0x100000 + 31)
constexpr long kProtoHttp = 1L << 0;         // CURLPROTO_HTTP
constexpr long kProtoHttps = 1L << 1;        // CURLPROTO_HTTPS
constexpr long kGlobalDefault = 3;           // CURL_GLOBAL_DEFAULT (CURL_GLOBAL_SSL | CURL_GLOBAL_WINSOCK)
constexpr int kOk = 0;                       // CURLE_OK
constexpr int kWriteError = 23;              // CURLE_WRITE_ERROR
constexpr int kAbortedByCallback = 42;       // CURLE_ABORTED_BY_CALLBACK

using WriteFn = std::size_t (*)(char*, std::size_t, std::size_t, void*);
using XferInfoFn = int (*)(void*, std::int64_t, std::int64_t, std::int64_t, std::int64_t); // curl_off_t

struct Curl {
    SDL_SharedObject* lib = nullptr;
    std::string why;
    int (*global_init)(long) = nullptr;
    CurlHandle* (*easy_init)() = nullptr;
    int (*easy_setopt)(CurlHandle*, int, ...) = nullptr;
    int (*easy_perform)(CurlHandle*) = nullptr;
    int (*easy_getinfo)(CurlHandle*, int, ...) = nullptr;
    void (*easy_cleanup)(CurlHandle*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
};

template <typename F> bool load(SDL_SharedObject* lib, const char* name, F& out) {
    out = reinterpret_cast<F>(SDL_LoadFunction(lib, name));
    return out != nullptr;
}

const Curl& curl() {
    static Curl c;
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef __APPLE__
        const char* names[] = {"/usr/lib/libcurl.4.dylib", "libcurl.4.dylib"};
#else
        const char* names[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
#endif
        for (const char* name : names) {
            c.lib = SDL_LoadObject(name);
            if (c.lib) break;
        }
        if (!c.lib) {
            c.why = "libcurl is not installed (looked for " + std::string(names[0]) + ")";
            return;
        }
        if (!load(c.lib, "curl_global_init", c.global_init) || !load(c.lib, "curl_easy_init", c.easy_init) ||
            !load(c.lib, "curl_easy_setopt", c.easy_setopt) || !load(c.lib, "curl_easy_perform", c.easy_perform) ||
            !load(c.lib, "curl_easy_getinfo", c.easy_getinfo) ||
            !load(c.lib, "curl_easy_cleanup", c.easy_cleanup) ||
            !load(c.lib, "curl_easy_strerror", c.easy_strerror)) {
            c.why = "the system's libcurl lacks a function Hailstone needs";
            SDL_UnloadObject(c.lib);
            c.lib = nullptr;
            return;
        }
        if (c.global_init(kGlobalDefault) != kOk) {
            c.why = "libcurl could not start";
            SDL_UnloadObject(c.lib);
            c.lib = nullptr;
        }
    });
    return c;
}

struct Transfer {
    const Curl* c = nullptr;
    CurlHandle* h = nullptr;
    const HttpGetOptions* options = nullptr;
    const std::function<bool(std::span<const std::byte>)>* sink = nullptr;
    const std::function<bool(std::uint64_t, std::uint64_t)>* progress = nullptr;
    std::uint64_t received = 0;
    bool cancelled = false;
    bool too_large = false;
    bool bad_status = false;
    bool url_checked = false;
    bool refused_url = false;
};

std::size_t on_write(char* data, std::size_t size, std::size_t count, void* user) {
    auto& t = *static_cast<Transfer*>(user);
    const std::size_t n = size * count;
    // A followed redirect's body never reaches here. Discard a small unfollowed redirect's body so libcurl
    // can finish the response and report its redirect URL without giving those bytes to the caller.
    long status = 0;
    t.c->easy_getinfo(t.h, kInfoResponseCode, &status);
    if (!t.options->follow_redirects && is_redirect(static_cast<int>(status))) {
        constexpr std::uint64_t max_redirect_body = 64 * 1024;
        if (n > max_redirect_body - t.received) {
            t.too_large = true;
            return 0;
        }
        t.received += n;
        return n;
    }
    if (!is_2xx(static_cast<int>(status))) {
        t.bad_status = true;
        return 0;
    }
    if (t.options->allow_url && !t.url_checked) {
        // The address the redirects ended on, before any of its body goes on.
        t.url_checked = true;
        char* url = nullptr;
        t.c->easy_getinfo(t.h, kInfoEffectiveUrl, &url);
        if (!url || !t.options->allow_url(std::string_view(url))) {
            t.refused_url = true;
            return 0;
        }
    }
    t.received += n;
    if (t.options->max_bytes && t.received > t.options->max_bytes) {
        t.too_large = true;
        return 0;
    }
    if (!(*t.sink)(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), n))) {
        t.cancelled = true;
        return 0;
    }
    return n;
}

int on_progress(void* user, std::int64_t total, std::int64_t now, std::int64_t, std::int64_t) {
    auto& t = *static_cast<Transfer*>(user);
    // libcurl calls this about once a second even while the transfer waits, so a stop needs no watcher here.
    if (t.options->cancelled && t.options->cancelled()) {
        t.cancelled = true;
        return 1;
    }
    if (t.options->max_bytes && total > 0 && static_cast<std::uint64_t>(total) > t.options->max_bytes) {
        t.too_large = true;
        return 1;
    }
    if (t.progress && *t.progress) {
        if (!(*t.progress)(static_cast<std::uint64_t>(std::max<std::int64_t>(now, 0)),
                           static_cast<std::uint64_t>(std::max<std::int64_t>(total, 0)))) {
            t.cancelled = true;
            return 1;
        }
    }
    return 0;
}

} // namespace

bool http_available(std::string* why) {
    const Curl& c = curl();
    if (why) *why = c.lib ? std::string() : c.why;
    return c.lib != nullptr;
}

HttpGetResult http_get(const std::string& url, const HttpGetOptions& options,
                       const std::function<bool(std::span<const std::byte>)>& sink,
                       const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
    HttpGetResult r;
    const std::string_view scheme = scheme_of(url);
    if (scheme.empty()) {
        r.error = "not an http or https address";
        return r;
    }
    const Curl& c = curl();
    if (!c.lib) {
        r.error = c.why;
        return r;
    }
    CurlHandle* h = c.easy_init();
    if (!h) {
        r.error = "libcurl could not start a transfer";
        return r;
    }
    Transfer t;
    t.c = &c;
    t.h = h;
    t.options = &options;
    t.sink = &sink;
    t.progress = &progress;
    const bool secure = scheme == "https";
    const WriteFn write_fn = on_write;
    const XferInfoFn progress_fn = on_progress;

    c.easy_setopt(h, kOptUrl, url.c_str());
    c.easy_setopt(h, kOptUserAgent, options.user_agent.c_str());
    c.easy_setopt(h, kOptWriteFunction, write_fn);
    c.easy_setopt(h, kOptWriteData, static_cast<void*>(&t));
    c.easy_setopt(h, kOptXferInfoFunction, progress_fn);
    c.easy_setopt(h, kOptXferInfoData, static_cast<void*>(&t));
    c.easy_setopt(h, kOptNoProgress, 0L);
    // Downloads run on several threads at once: without this libcurl may use SIGALRM for the resolver timeout,
    // which is not thread safe (the libcurl docs, CURLOPT_NOSIGNAL).
    c.easy_setopt(h, kOptNoSignal, 1L);
    c.easy_setopt(h, kOptFollowLocation, options.follow_redirects ? 1L : 0L);
    c.easy_setopt(h, kOptMaxRedirs, 10L);
    c.easy_setopt(h, kOptConnectTimeout, static_cast<long>(std::max(1, options.connect_timeout_s)));
    c.easy_setopt(h, kOptLowSpeedLimit, 1L);
    c.easy_setopt(h, kOptLowSpeedTime, static_cast<long>(std::max(1, options.stall_timeout_s)));
    // Only http and https, and once on https never back to http. The _STR options are libcurl 7.85.0 and later.
    const char* redirect = secure ? "https" : "http,https";
    if (c.easy_setopt(h, kOptProtocolsStr, "http,https") != kOk ||
        c.easy_setopt(h, kOptRedirProtocolsStr, redirect) != kOk) {
        c.easy_setopt(h, kOptProtocols, kProtoHttp | kProtoHttps);
        c.easy_setopt(h, kOptRedirProtocols, secure ? kProtoHttps : kProtoHttp | kProtoHttps);
    }

    const int code = c.easy_perform(h);
    long status = 0;
    c.easy_getinfo(h, kInfoResponseCode, &status);
    r.status = static_cast<int>(status);
    if (t.refused_url) {
        r.error = "the download was sent to an address it may not come from";
    } else if (t.too_large) {
        r.error = "the answer is larger than allowed";
    } else if (t.cancelled) {
        r.error = "cancelled";
    } else if (t.bad_status || (code == kOk && !is_2xx(r.status))) {
        r.error = status_error(r.status);
        if (!options.follow_redirects && is_redirect(r.status)) {
            char* location = nullptr;
            if (c.easy_getinfo(h, kInfoRedirectUrl, &location) == kOk && location) r.redirect_url = location;
        }
    } else if (code != kOk) {
        const char* text = c.easy_strerror(code);
        r.error = text ? text : "libcurl error " + std::to_string(code);
        if (code == kWriteError || code == kAbortedByCallback) r.error = "cancelled";
    } else {
        r.ok = true;
        // An answer with no body never reached on_write, so its address is checked here (as WinHTTP's always is):
        // an empty 2xx at the end of a redirect off the allowed hosts is not a success.
        if (options.allow_url && !t.url_checked) {
            char* end_url = nullptr;
            c.easy_getinfo(h, kInfoEffectiveUrl, &end_url);
            if (!end_url || !options.allow_url(std::string_view(end_url))) {
                r.ok = false;
                r.error = "the download was sent to an address it may not come from";
            }
        }
    }
    c.easy_cleanup(h);
    return r;
}

#endif

} // namespace hailstone::platform
