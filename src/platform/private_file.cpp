#include "platform/private_file.h"

#include <algorithm>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hailstone::platform {

bool write_private_file(const std::filesystem::path& path, std::string_view text) {
    bool ok = false;
#if defined(_WIN32)
    // CREATE_NEW: made here or not at all, never over what is there (no look first and open after, which another
    // process could slip a file or a link in between, as std::filesystem::exists and then std::ofstream did).
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ok = true;
    std::size_t done = 0;
    while (done < text.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(text.size() - done, 1u << 30));
        DWORD wrote = 0;
        if (!WriteFile(h, text.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        done += wrote;
    }
    if (!CloseHandle(h)) ok = false;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    ok = true;
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) ok = false;
#endif
    if (!ok) {
        std::error_code rm;
        std::filesystem::remove(path, rm);
    }
    return ok;
}

bool make_private_dir(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::error_code ec;
    return std::filesystem::create_directory(path, ec) && !ec;
#else
    return ::mkdir(path.c_str(), S_IRWXU) == 0;
#endif
}

} // namespace hailstone::platform
