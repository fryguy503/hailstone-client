#include "platform/durable_file.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hailstone::platform {

bool write_file_synced(const std::filesystem::path& path, std::string_view text) {
#if defined(_WIN32)
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < text.size()) {
        const std::size_t left = text.size() - done;
        const DWORD chunk = left > 0x40000000u ? 0x40000000u : static_cast<DWORD>(left);
        DWORD wrote = 0;
        ok = ::WriteFile(h, text.data() + done, chunk, &wrote, nullptr) != 0 && wrote > 0;
        done += wrote;
    }
    if (ok) ok = ::FlushFileBuffers(h) != 0;
    if (::CloseHandle(h) == 0) ok = false;
    return ok;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return false;
    bool ok = true;
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
#if defined(__APPLE__)
    // fsync on macOS hands the bytes to the drive, which may still hold them in its cache; F_FULLFSYNC asks it to
    // write them out. Not every file system has it, so fsync is the fallback.
    if (ok && ::fcntl(fd, F_FULLFSYNC) != 0) ok = ::fsync(fd) == 0;
#else
    if (ok) ok = ::fsync(fd) == 0;
#endif
    if (::close(fd) != 0) ok = false;
    return ok;
#endif
}

namespace {

#if !defined(_WIN32)
std::filesystem::path folder_of(const std::filesystem::path& p) {
    return p.has_parent_path() ? p.parent_path() : std::filesystem::path(".");
}

bool write_all(int fd, std::string_view text) {
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool sync_fd(int fd) {
#if defined(__APPLE__)
    // As in write_file_synced: F_FULLFSYNC where the file system has it, fsync otherwise.
    if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
    return ::fsync(fd) == 0;
}

void sync_dir(const std::filesystem::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        (void)::fsync(fd);
        ::close(fd);
    }
}
#else
bool write_all(HANDLE h, std::string_view text) {
    std::size_t done = 0;
    while (done < text.size()) {
        const std::size_t left = text.size() - done;
        const DWORD chunk = left > 0x40000000u ? 0x40000000u : static_cast<DWORD>(left);
        DWORD wrote = 0;
        if (::WriteFile(h, text.data() + done, chunk, &wrote, nullptr) == 0 || wrote == 0) return false;
        done += wrote;
    }
    return true;
}
#endif

} // namespace

bool sync_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    // FlushFileBuffers needs write access; the sharing lets it work beside a reader.
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = ::FlushFileBuffers(h) != 0;
    if (::CloseHandle(h) == 0) ok = false;
    return ok;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = sync_fd(fd);
    if (::close(fd) != 0) ok = false;
    return ok;
#endif
}

bool append_file_synced(const std::filesystem::path& path, std::string_view text) {
#if defined(_WIN32)
    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = write_all(h, text) && ::FlushFileBuffers(h) != 0;
    if (::CloseHandle(h) == 0) ok = false;
    return ok;
#else
    bool created = true;
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0 && errno == EEXIST) {
        created = false;
        fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    }
    if (fd < 0) return false;
    bool ok = write_all(fd, text) && sync_fd(fd);
    if (::close(fd) != 0) ok = false;
    if (ok && created) sync_dir(folder_of(path));
    return ok;
#endif
}

bool link_file_synced(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
    ec.clear();
#if defined(_WIN32)
    (void)from;
    (void)to;
    ec = std::make_error_code(std::errc::operation_not_supported);
    return false;
#else
    if (::link(from.c_str(), to.c_str()) != 0) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    sync_dir(folder_of(to));
    return true;
#endif
}

bool replace_file_synced(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
    ec.clear();
#if defined(_WIN32)
    if (!::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return false;
    }
    return true;
#else
    if (::rename(from.c_str(), to.c_str()) != 0) {
        ec = std::error_code(errno, std::generic_category());
        return false;
    }
    // The rename is an entry of the folder: syncing the folder puts it on the disk.
    sync_dir(folder_of(to));
    return true;
#endif
}

} // namespace hailstone::platform
