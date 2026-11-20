#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "platform/process.h"

namespace hailstone::log {

namespace {
std::atomic<Level> g_level{Level::Info};
std::atomic<bool> g_console{true};
std::mutex g_mutex;
std::FILE* g_file = nullptr; // guarded by g_mutex
std::filesystem::path g_path; // the file g_file writes
constexpr std::uint64_t kDefaultFileLimit = 100ull * 1024 * 1024;
std::uint64_t g_file_limit = kDefaultFileLimit; // guarded by g_mutex
std::uint64_t g_file_written = 0;               // guarded by g_mutex: bytes in g_file
bool g_file_full = false;                       // guarded by g_mutex: the limit was reached, nothing more goes in

// The last lines, for a crash report: written under g_mutex, read without it.
constexpr std::size_t kRecentLines = 16;
constexpr std::size_t kRecentLength = 200;
char g_recent[kRecentLines][kRecentLength] = {};
std::atomic<std::size_t> g_recent_next{0}; // the slot the next line goes to (counts up; modulo kRecentLines)

void remember_line(const char* tag, std::string_view message) {
    std::size_t n = g_recent_next.load(std::memory_order_relaxed);
    char* line = g_recent[n % kRecentLines];
    std::snprintf(line, kRecentLength, "[%s] %.*s", tag, static_cast<int>(message.size()), message.data());
    g_recent_next.store(n + 1, std::memory_order_release);
}

// <name>.<n><ext>: hailstone.log -> hailstone.1.log.
std::filesystem::path numbered(const std::filesystem::path& path, const std::string& n) {
    std::filesystem::path p = path;
    p.replace_extension("." + n + path.extension().string());
    return p;
}

// The lock beside a log file (<name>.lock), held while the process runs: the program holding it owns the file. A
// second client (or a viewer) started meanwhile finds it taken and writes a log of its own.
bool lock_log(const std::filesystem::path& path) {
    static std::vector<std::filesystem::path> held; // guarded by g_mutex; this process's locks
    std::filesystem::path lock = path;
    lock.replace_extension(".lock");
    if (std::find(held.begin(), held.end(), lock) != held.end()) return true;
    held.push_back(lock); // undone below when the lock is taken by another program
    if (platform::hold_lock_file(lock)) return true; // (a lock that cannot be made at all counts as held)
    held.pop_back();
    return false;
}

// The logs of second clients (<name>.<process id><ext>, which nothing else removes): the newest few stay, and so does
// any written in the last day (its client may still be running; on Linux and macOS a removed file it writes would
// vanish with its lines), never `in_use`. The numbers 1 to `kept` are the earlier runs' files, not these.
void remove_old_pid_logs(const std::filesystem::path& path, const std::filesystem::path& in_use, int kept) {
    namespace fs = std::filesystem;
    constexpr std::size_t kNewestKept = 5;
    const std::string stem = path.stem().string() + ".", ext = path.extension().string();
    std::vector<std::pair<fs::file_time_type, fs::path>> logs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(path.parent_path(), ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() <= stem.size() + ext.size() || !name.starts_with(stem) || !name.ends_with(ext)) continue;
        const std::string_view number =
            std::string_view(name).substr(stem.size(), name.size() - stem.size() - ext.size());
        if (!std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
            (number.size() == 1 && number[0] >= '1' && number[0] < '1' + kept))
            continue;
        std::error_code t_ec;
        const auto when = fs::last_write_time(e.path(), t_ec);
        if (!t_ec && e.is_regular_file(t_ec)) logs.emplace_back(when, e.path());
    }
    std::sort(logs.begin(), logs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const auto day_ago = fs::file_time_type::clock::now() - std::chrono::hours(24);
    for (std::size_t i = kNewestKept; i < logs.size(); ++i) {
        if (logs[i].first > day_ago || logs[i].second == in_use) continue;
        std::error_code rm_ec;
        fs::remove(logs[i].second, rm_ec); // still open on Windows: it stays, and goes at a later start
    }
}

const char* tag(Level level) {
    switch (level) {
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    }
    return "?";
}
} // namespace

void set_level(Level level) { g_level = level; }
void set_console(bool on) { g_console = on; }
bool enabled(Level level) { return level >= g_level.load(); }

bool open_file(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::lock_guard lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path target = path;
    bool own = lock_log(path);
    if (own && fs::exists(path, ec)) {
        // The earlier runs' files: hailstone.log -> .1 -> .2 -> .3 (the oldest goes), so a crash's log outlives a
        // few starts after it.
        constexpr int kKept = 3; // remove_old_pid_logs is told the same
        for (int n = kKept - 1; n >= 1; --n) {
            std::error_code rename_ec;
            if (fs::exists(numbered(path, std::to_string(n)), rename_ec))
                fs::rename(numbered(path, std::to_string(n)), numbered(path, std::to_string(n + 1)), rename_ec);
        }
        fs::rename(path, numbered(path, "1"), ec);
        if (ec) own = false; // still open somewhere (Windows): leave it alone
    }
    if (!own) target = numbered(path, std::to_string(platform::process_id())); // hailstone.<pid>.log
    remove_old_pid_logs(path, target, 3);
    g_file = platform::open_c_file(target, "wb");
    g_file_written = 0;
    g_file_full = false;
    if (g_file) g_path = target;
    return g_file != nullptr;
}

std::filesystem::path file_path() {
    std::lock_guard lock(g_mutex);
    return g_file ? g_path : std::filesystem::path();
}

void set_file_limit(std::uint64_t bytes) {
    std::lock_guard lock(g_mutex);
    g_file_limit = bytes == 0 ? kDefaultFileLimit : bytes;
}

void close_file() {
    std::lock_guard lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

std::size_t copy_recent_lines(char* out, std::size_t size) {
    if (size == 0) return 0;
    std::size_t at = 0;
    const std::size_t next = g_recent_next.load(std::memory_order_acquire);
    const std::size_t first = next > kRecentLines ? next - kRecentLines : 0;
    for (std::size_t i = first; i < next; ++i) {
        const char* line = g_recent[i % kRecentLines];
        for (std::size_t k = 0; k < kRecentLength && line[k] && at + 1 < size; ++k) out[at++] = line[k];
        if (at + 1 < size) out[at++] = '\n';
    }
    out[at] = '\0';
    return at;
}

std::string_view escape_controls(std::string_view message, std::string& scratch) {
    // A line break goes on over an indented line (a Lua traceback stays readable), which no "[level] " line starts
    // as (app/bug_report.cpp redact_chat takes such lines with the chat line they follow).
    static constexpr std::string_view kContinuation = "\n  ";
    auto control = [](char c) {
        const auto b = static_cast<unsigned char>(c);
        return (b < 0x20 && b != '\t') || b == 0x7f;
    };
    auto first = std::find_if(message.begin(), message.end(), control);
    if (first == message.end()) return message;
    scratch.assign(message.begin(), first);
    static constexpr char kHex[] = "0123456789abcdef";
    for (auto it = first; it != message.end(); ++it) {
        const char c = *it;
        if (!control(c)) scratch.push_back(c);
        else if (c == '\n') scratch += kContinuation;
        else if (c == '\r' && it + 1 != message.end() && it[1] == '\n') {
            scratch += kContinuation; // "\r\n" is one line break
            ++it;
        } else if (c == '\r') scratch += "\\r";
        else {
            const auto b = static_cast<unsigned char>(c);
            scratch += "\\x";
            scratch.push_back(kHex[b >> 4]);
            scratch.push_back(kHex[b & 0xf]);
        }
    }
    return scratch;
}

void write(Level level, std::string_view message) {
    if (level < g_level.load()) return;
    std::string escaped; // empty, so no allocation, unless the message has a control character
    message = escape_controls(message, escaped);
    std::lock_guard lock(g_mutex);
    remember_line(tag(level), message);
    if (g_console.load())
        std::fprintf(level >= Level::Warn ? stderr : stdout, "[%s] %.*s\n", tag(level),
                     static_cast<int>(message.size()), message.data());
    if (g_file && !g_file_full) {
        // Flushed at once, so the file holds everything up to a crash.
        const int n = std::fprintf(g_file, "[%s] %.*s\n", tag(level), static_cast<int>(message.size()), message.data());
        if (n > 0) g_file_written += static_cast<std::uint64_t>(n);
        if (g_file_written >= g_file_limit) {
            // A run that logs without end (a message every frame) must not fill the disk: the file stops here, with
            // a line saying so, and the rest goes to stdout and stderr only.
            std::fprintf(g_file, "[warn] the log reached its limit of %llu bytes: nothing more is written to it in "
                                 "this run\n",
                         static_cast<unsigned long long>(g_file_limit));
            g_file_full = true;
        }
        std::fflush(g_file);
    }
}

} // namespace hailstone::log
