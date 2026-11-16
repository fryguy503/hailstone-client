#include "platform/executable_path.h"

#include <cstdint>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace hailstone::platform {

std::optional<std::filesystem::path> process_executable_path() {
#if defined(_WIN32)
    std::vector<wchar_t> path(1024);
    while (path.size() <= 32768) {
        const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!count) return std::nullopt;
        if (count < path.size()) return std::filesystem::path(std::wstring(path.data(), count));
        path.resize(path.size() * 2);
    }
    return std::nullopt;
#elif defined(__linux__)
    // This refers to the running inode even if a software update renamed its directory entry.
    return std::filesystem::path("/proc/self/exe");
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (!size || size > 1024 * 1024) return std::nullopt;
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) return std::nullopt;
    return std::filesystem::path(path.data());
#else
    return std::nullopt;
#endif
}

} // namespace hailstone::platform
