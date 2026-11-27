#include "platform/crash_log.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <iterator>
#include <string>

#include "core/build_info.h"
#include "core/log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h.
#include <dbghelp.h>
#else
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define HAILSTONE_HAVE_BACKTRACE 1
#endif
#include <fcntl.h>
#if !defined(__APPLE__) && __has_include(<link.h>)
#include <link.h>
#define HAILSTONE_HAVE_PHDR 1
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hailstone::platform {

namespace {
constexpr std::size_t kCrashStackSize = 64 * 1024;

// Everything the handlers use is set up front: no allocation at the crash.
#ifdef _WIN32
wchar_t g_dir[1024] = {};
#else
char g_dir[1024] = {};
#endif
char g_log_path[1024] = {};
char g_recent[4096] = {};
char g_report[8192] = {};
char g_build_id[2 * 64 + 1] = {}; // a build ID is 20 bytes (SHA-1, the linkers' default) or fewer; 64 at most here
std::atomic<bool> g_reported{false}; // one report per run: a crash in the handler, or abort after terminate
#ifdef _WIN32
wchar_t g_path[1200] = {}; // the report's file: kept off the stack, which may be nearly used up
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
EXCEPTION_POINTERS* g_exception = nullptr; // the structured exception being reported: a minidump goes beside it
#else
char g_path[1200] = {};
#endif

// Appends to a fixed buffer, cutting what does not fit; `out` stays NUL-terminated.
struct Writer {
    char* out;
    std::size_t size;
    std::size_t at = 0;
    void put(const char* s) {
        for (; s && *s && at + 1 < size; ++s) out[at++] = *s;
        out[at] = '\0';
    }
    void num(std::uint64_t v, int width = 0) {
        char digits[24];
        int n = 0;
        do {
            digits[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v && n < 20);
        while (n < width && n < 20) digits[n++] = '0';
        char text[24];
        for (int i = 0; i < n; ++i) text[i] = digits[n - 1 - i];
        text[n] = '\0';
        put(text);
    }
    void hex(std::uint64_t v, int width) {
        char text[20];
        for (int i = 0; i < width && i < 16; ++i) text[i] = "0123456789abcdef"[(v >> (4 * (width - 1 - i))) & 0xf];
        text[width < 16 ? width : 16] = '\0';
        put(text);
    }
};

struct Civil {
    std::int64_t year;
    unsigned month, day, hour, minute, second;
};

// The UTC date and time of a Unix time (days_from_civil's inverse, H. Hinnant): only arithmetic.
Civil civil(std::int64_t t) {
    std::int64_t days = t / 86400, rest = t % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    Civil c{};
    c.day = doy - (153 * mp + 2) / 5 + 1;
    c.month = mp < 10 ? mp + 3 : mp - 9;
    c.year = static_cast<std::int64_t>(yoe) + era * 400 + (c.month <= 2 ? 1 : 0);
    c.hour = static_cast<unsigned>(rest / 3600);
    c.minute = static_cast<unsigned>(rest / 60 % 60);
    c.second = static_cast<unsigned>(rest % 60);
    return c;
}

std::int64_t now_seconds() { return static_cast<std::int64_t>(std::time(nullptr)); }

void copy_dir(const std::filesystem::path& dir) {
#ifdef _WIN32
    const std::wstring s = dir.wstring();
#else
    const std::string s = dir.string();
#endif
    std::size_t n = s.size() < std::size(g_dir) - 1 ? s.size() : std::size(g_dir) - 1;
    for (std::size_t i = 0; i < n; ++i) g_dir[i] = s[i];
    g_dir[n] = 0;
}

#ifdef HAILSTONE_HAVE_PHDR
// dl_iterate_phdr reports the program itself first: its note segments hold the build ID the linker wrote
// (-Wl,--build-id, cmake/SeparateSymbols.cmake).
int read_build_id(dl_phdr_info* info, std::size_t, void*) {
    for (std::size_t i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const unsigned char* id = nullptr;
        const std::size_t n = find_gnu_build_id(reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr),
                                                static_cast<std::size_t>(ph.p_memsz),
                                                ph.p_align == 8 ? 8 : 4, &id);
        if (n == 0 || n * 2 >= sizeof g_build_id) continue;
        Writer w{g_build_id, sizeof g_build_id};
        for (std::size_t k = 0; k < n; ++k) w.hex(id[k], 2);
        break;
    }
    return 1; // the program only, not the libraries after it
}
#endif

#ifndef _WIN32
const char* signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV, invalid memory access";
    case SIGBUS: return "SIGBUS, bus error";
    case SIGFPE: return "SIGFPE, arithmetic error";
    case SIGILL: return "SIGILL, illegal instruction";
    case SIGABRT: return "SIGABRT, aborted";
    }
    return "fatal signal";
}

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
// What each signal did before ours (the default, or another handler such as AddressSanitizer's), by kSignals' order.
struct sigaction g_previous[std::size(kSignals)] = {};

void on_signal(int sig) {
    if (!g_reported.exchange(true)) {
        char what[96];
        Writer w{what, sizeof what};
        w.put("signal ");
        w.num(static_cast<std::uint64_t>(sig));
        w.put(" (");
        w.put(signal_name(sig));
        w.put(")");
        write_crash_report(what);
    }
    // The earlier action is put back and the signal raised again: the default action lets the system see the crash
    // (core dump, exit status), and an earlier handler (AddressSanitizer's report) still runs.
    bool restored = false;
    for (std::size_t i = 0; i < std::size(kSignals); ++i)
        if (kSignals[i] == sig) restored = sigaction(sig, &g_previous[i], nullptr) == 0;
    if (!restored) {
        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(sig, &dfl, nullptr);
    }
    std::raise(sig);
}
#else
constexpr DWORD kCxxException = 0xe06d7363; // what MSVC throws a C++ exception as

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
    const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    // An uncaught C++ exception: the CRT's own filter, the one ours replaced, calls std::terminate, where
    // on_terminate reports it with its what().
    if (code == kCxxException && g_previous_filter) return g_previous_filter(info);
    if (!g_reported.exchange(true)) {
        char what[96];
        Writer w{what, sizeof what};
        w.put("exception 0x");
        w.hex(code, 8);
        w.put(" at 0x");
        w.hex(info && info->ExceptionRecord ? reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress) : 0,
              static_cast<int>(sizeof(void*) * 2));
        g_exception = info;
        write_crash_report(what);
        g_exception = nullptr;
    }
    return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}
#endif

void on_terminate() {
    if (!g_reported.exchange(true)) {
        std::string what = "std::terminate without an exception";
        if (std::exception_ptr e = std::current_exception()) {
            what = "unhandled exception";
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& x) {
                what += ": ";
                what += x.what();
            } catch (...) {
            }
        }
        write_crash_report(what.c_str());
    }
    std::abort();
}
} // namespace

std::size_t format_crash_name(char* out, std::size_t size, std::int64_t unix_seconds) {
    if (size == 0) return 0;
    const Civil c = civil(unix_seconds);
    Writer w{out, size};
    w.out[0] = '\0';
    w.put("crash-");
    w.num(static_cast<std::uint64_t>(c.year < 0 ? 0 : c.year), 4);
    w.num(c.month, 2);
    w.num(c.day, 2);
    w.put("-");
    w.num(c.hour, 2);
    w.num(c.minute, 2);
    w.num(c.second, 2);
    w.put(".txt");
    return w.at;
}

std::size_t format_crash_report(char* out, std::size_t size, std::int64_t unix_seconds, const char* what,
                                const char* log_path, const char* recent, const char* build, const char* build_id) {
    if (size == 0) return 0;
    const Civil c = civil(unix_seconds);
    Writer w{out, size};
    w.out[0] = '\0';
    w.put("Hailstone crash report\n");
    if (build && *build) {
        w.put("build: ");
        w.put(build);
        w.put("\n");
    }
    if (build_id && *build_id) {
        w.put("build-id: ");
        w.put(build_id);
        w.put("\n");
    }
    w.put("time: ");
    w.num(static_cast<std::uint64_t>(c.year < 0 ? 0 : c.year), 4);
    w.put("-");
    w.num(c.month, 2);
    w.put("-");
    w.num(c.day, 2);
    w.put(" ");
    w.num(c.hour, 2);
    w.put(":");
    w.num(c.minute, 2);
    w.put(":");
    w.num(c.second, 2);
    w.put(" UTC\nwhat: ");
    w.put(what);
    w.put("\n");
    if (log_path && *log_path) {
        w.put("log: ");
        w.put(log_path);
        w.put("\n");
    }
    w.put("last log lines:\n");
    w.put(recent);
    return w.at;
}

std::size_t find_gnu_build_id(const unsigned char* notes, std::size_t size, std::size_t align,
                              const unsigned char** id) {
    constexpr std::uint32_t kGnuBuildId = 3; // NT_GNU_BUILD_ID
    if (!notes || (align != 4 && align != 8)) return 0;
    // Each note: name size, descriptor size, type (32 bits each), then the name and the descriptor, each starting at
    // a multiple of the alignment (the segment itself is aligned so).
    const auto skip = [align, size](std::size_t at, std::size_t n) {
        const std::size_t end = (at + n + align - 1) / align * align;
        return end < size ? end : size;
    };
    std::size_t at = 0;
    while (size - at >= 12) {
        std::uint32_t name_size = 0, desc_size = 0, type = 0;
        std::memcpy(&name_size, notes + at, 4);
        std::memcpy(&desc_size, notes + at + 4, 4);
        std::memcpy(&type, notes + at + 8, 4);
        at += 12;
        if (name_size > size - at) return 0;
        const std::size_t name_at = at;
        at = skip(at, name_size);
        if (desc_size > size - at) return 0;
        if (type == kGnuBuildId && name_size == 4 && std::memcmp(notes + name_at, "GNU", 4) == 0 && desc_size > 0) {
            if (id) *id = notes + at;
            return desc_size;
        }
        at = skip(at, desc_size);
    }
    return 0;
}

const char* crash_build_id() { return g_build_id; }

void set_crash_report_dir(const std::filesystem::path& dir) {
#ifdef HAILSTONE_HAVE_PHDR
    if (!g_build_id[0]) dl_iterate_phdr(read_build_id, nullptr);
#endif
    copy_dir(dir);
    std::string log;
    try {
        log = log::file_path().string();
    } catch (...) {
    }
    std::size_t n = log.size() < sizeof g_log_path - 1 ? log.size() : sizeof g_log_path - 1;
    log.copy(g_log_path, n);
    g_log_path[n] = '\0';
}

bool write_crash_report(const char* what) {
    if (!g_dir[0]) return false;
    const std::int64_t now = now_seconds();
    log::copy_recent_lines(g_recent, sizeof g_recent);
    const std::size_t length = format_crash_report(g_report, sizeof g_report, now, what, g_log_path, g_recent,
                                                     build_version(), g_build_id);
    char name[64];
    format_crash_name(name, sizeof name, now);
#ifdef _WIN32
    wchar_t* path = g_path;
    std::size_t at = 0;
    for (; g_dir[at] && at < 1100; ++at) path[at] = g_dir[at];
    path[at] = 0;
    CreateDirectoryW(path, nullptr); // normally there already: the log is in it
    path[at++] = L'\\';
    for (std::size_t i = 0; name[i]; ++i) path[at++] = static_cast<wchar_t>(name[i]);
    path[at] = 0;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, g_report, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(h);
    if (g_exception && at >= 4) {
        // crash-<date>.dmp beside it: the threads' call stacks and registers and the module list, for a debugger.
        // The dump goes into bug reports as it is, so it holds no secret: no heap (MiniDumpWithIndirectlyReferenced-
        // Memory would add what the stacks point at: the password field, the session key, a plugin's auth_key), and
        // MiniDumpFilterMemory keeps of the stacks only the pointers a stack trace needs, so a short password in a
        // std::string's own storage, or one a frame copied, is not in it either. The cost is the locals' values.
        // The client itself lives on the heap (run_client) and wipes the password once the login server has it.
        path[at - 3] = L'd';
        path[at - 2] = L'm';
        path[at - 1] = L'p';
        HANDLE dump = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = g_exception;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                              static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpFilterMemory),
                              &mei, nullptr, nullptr);
            CloseHandle(dump);
        }
    }
    return ok && written == length;
#else
    char* path = g_path;
    Writer p{path, sizeof g_path};
    p.put(g_dir);
    ::mkdir(path, 0755); // normally there already: the log is in it
    p.put("/");
    p.put(name);
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < length) {
        ssize_t n = ::write(fd, g_report + done, length - done);
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
#ifdef HAILSTONE_HAVE_BACKTRACE
    // The stack: tools/symbolize_crash.py names a "+0x..." frame from the build's symbol file (docs/hardening.md).
    static const char kStack[] = "\nstack:\n";
    if (::write(fd, kStack, sizeof kStack - 1) > 0) {
        void* frames[64];
        backtrace_symbols_fd(frames, backtrace(frames, 64), fd);
    }
#endif
    ::close(fd);
    return done == length;
#endif
}

void install_crash_handler(const std::filesystem::path& dir) {
    set_crash_report_dir(dir);
    std::set_terminate(on_terminate);
#ifdef _WIN32
    ULONG guarantee = kCrashStackSize;
    SetThreadStackGuarantee(&guarantee); // room for the handler after a stack overflow
    g_previous_filter = SetUnhandledExceptionFilter(on_exception);
#else
    // Its own stack, so a stack overflow's SIGSEGV can still be handled (the main thread's; every thread the client
    // starts makes a CrashStack first: the session, sound library, appearance jobs, neighbour zone, navigation,
    // add-on and updater workers).
    static char alt_stack[kCrashStackSize];
    stack_t ss{};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof alt_stack;
    sigaltstack(&ss, nullptr);
#ifdef HAILSTONE_HAVE_BACKTRACE
    // The first backtrace loads the unwinder (it allocates): done now, not in the handler.
    void* frame[1];
    backtrace(frame, 1);
#endif
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK;
    for (std::size_t i = 0; i < std::size(kSignals); ++i) sigaction(kSignals[i], &sa, &g_previous[i]);
#endif
}

CrashStack::CrashStack() {
#ifdef _WIN32
    ULONG guarantee = kCrashStackSize;
    SetThreadStackGuarantee(&guarantee);
#else
    stack_t old{};
    // A thread that has one already (made by a sanitizer's runtime, or an outer CrashStack) keeps it.
    if (sigaltstack(nullptr, &old) == 0 && !(old.ss_flags & SS_DISABLE)) return;
    stack_ = std::malloc(kCrashStackSize);
    if (!stack_) return;
    stack_t ss{};
    ss.ss_sp = stack_;
    ss.ss_size = kCrashStackSize;
    if (sigaltstack(&ss, nullptr) != 0) {
        std::free(stack_);
        stack_ = nullptr;
    }
#endif
}

CrashStack::~CrashStack() {
#ifndef _WIN32
    if (!stack_) return;
    stack_t ss{};
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, nullptr);
    std::free(stack_);
#endif
}

} // namespace hailstone::platform
