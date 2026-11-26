#include "platform/process.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>

#include <SDL3/SDL_platform.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#endif

namespace hailstone::platform {

unsigned long process_id() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

std::optional<std::uint64_t> resident_memory() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return pmc.WorkingSetSize;
    return std::nullopt;
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size;
    return std::nullopt;
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return std::nullopt;
    char line[128] = {};
    bool ok = std::fgets(line, sizeof line, f) != nullptr;
    std::fclose(f);
    if (!ok) return std::nullopt;
    // "size resident shared ..." in pages.
    char* end = nullptr;
    std::strtoull(line, &end, 10);
    char* after = nullptr;
    unsigned long long resident = std::strtoull(end, &after, 10);
    if (after == end) return std::nullopt;
    return static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#else
    return std::nullopt;
#endif
}

std::optional<std::uint64_t> private_commit_memory() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = static_cast<DWORD>(sizeof counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             static_cast<DWORD>(sizeof counters)) &&
        counters.PrivateUsage > 0) // Wine reports 0: not measured there
        return static_cast<std::uint64_t>(counters.PrivateUsage);
#endif
    return std::nullopt;
}

void lower_current_thread_priority() {
#if defined(__linux__)
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}

bool hold_lock_file(const std::filesystem::path& lock) {
#if defined(_WIN32)
    // No sharing: a second open fails while this handle lives; the file goes when the process ends.
    HANDLE h = CreateFileW(lock.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) return true; // kept open until the process ends
    // Only another holder makes it taken; any other failure (a read-only folder) is no lock possible, as on POSIX.
    return GetLastError() != ERROR_SHARING_VIOLATION;
#else
    int fd = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return true; // no lock possible (a read-only folder)
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    return true; // fd kept open: the lock lasts until the process ends
#endif
}

std::FILE* open_c_file(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
    std::wstring wide_mode;
    for (const char* c = mode; *c; ++c) wide_mode += static_cast<wchar_t>(*c);
    wide_mode += L'N'; // _O_NOINHERIT: SDL starts the helpers with bInheritHandles, which passes on every other handle
    return _wfopen(path.c_str(), wide_mode.c_str());
#else
    std::string closed_on_exec = mode;
#if defined(__linux__)
    closed_on_exec += 'e'; // O_CLOEXEC at the open itself (glibc, musl)
#endif
    std::FILE* f = std::fopen(path.c_str(), closed_on_exec.c_str());
    // Elsewhere (and should the C library not know 'e') afterwards: SDL's posix_spawn closes nothing it is not told to.
    if (f)
        if (const int flags = ::fcntl(::fileno(f), F_GETFD); flags >= 0 && (flags & FD_CLOEXEC) == 0)
            ::fcntl(::fileno(f), F_SETFD, flags | FD_CLOEXEC);
    return f;
#endif
}

std::optional<std::string> take_environment_variable(const std::string& name) {
    if (name.empty() || name.find('=') != std::string::npos || name.find('\0') != std::string::npos) return std::nullopt;
#if defined(_WIN32)
    // The process's own block (GetEnvironmentVariableW, what CreateProcess hands a child), by wide names so any
    // character works; _wputenv_s with an empty value removes it from that block and the CRT's copy alike.
    const int wn = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (wn <= 0) return std::nullopt;
    std::wstring wname(static_cast<std::size_t>(wn), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wname.data(), wn);
    const DWORD need = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (need == 0) return std::nullopt; // not set (an empty value reads as set, with need 1)
    std::wstring wvalue(need, L'\0');
    const DWORD got = GetEnvironmentVariableW(wname.c_str(), wvalue.data(), need);
    wvalue.resize(got < need ? got : 0);
    std::string value;
    if (!wvalue.empty()) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wvalue.data(), static_cast<int>(wvalue.size()), nullptr, 0,
                                          nullptr, nullptr);
        if (n > 0) {
            value.resize(static_cast<std::size_t>(n));
            WideCharToMultiByte(CP_UTF8, 0, wvalue.data(), static_cast<int>(wvalue.size()), value.data(), n, nullptr,
                                nullptr);
        }
        SecureZeroMemory(wvalue.data(), wvalue.size() * sizeof(wchar_t));
    }
    _wputenv_s(wname.c_str(), L"");
    SetEnvironmentVariableW(wname.c_str(), nullptr);
    return value;
#else
    const char* found = std::getenv(name.c_str());
    if (!found) return std::nullopt;
    std::string value(found);
    ::unsetenv(name.c_str());
    return value;
#endif
}

std::string system_description() {
    std::string out = SDL_GetPlatform();
#if defined(_WIN32)
    // GetVersionEx answers what the manifest claims; ntdll's RtlGetVersion the real version.
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (auto get = reinterpret_cast<RtlGetVersion>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
            OSVERSIONINFOW v{};
            v.dwOSVersionInfoSize = sizeof v;
            if (get(&v) == 0) out += std::format(" {}.{}.{}", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
        }
#else
    if (std::ifstream in("/etc/os-release", std::ios::binary); in) {
        std::stringstream text;
        text << in.rdbuf();
        const std::string release = text.str();
        const std::size_t at = release.find("PRETTY_NAME=");
        if (at != std::string::npos) {
            std::string name = release.substr(at + 12, release.find('\n', at) - at - 12);
            if (name.size() >= 2 && name.front() == '"') name = name.substr(1, name.size() - 2);
            out += ", " + name;
        }
    }
    if (utsname u{}; uname(&u) == 0) out += std::format(" (kernel {}, {})", u.release, u.machine);
    if (const char* session = std::getenv("XDG_SESSION_TYPE")) out += std::string(", ") + session;
#endif
    return out;
}

bool attach_parent_console() {
#if defined(_WIN32)
    if (GetConsoleWindow()) return true; // (a console build has its own)
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    return true;
#else
    return true;
#endif
}

} // namespace hailstone::platform
