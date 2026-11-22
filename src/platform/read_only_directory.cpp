#include "platform/read_only_directory.h"

#include <cstdint>
#include <limits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hailstone::platform {
namespace {

bool leaf_name(const std::filesystem::path& name) {
    return !name.empty() && name == name.filename() && name != "." && name != ".." &&
           name.native().find_first_of(std::filesystem::path("\\/:").native()) == std::filesystem::path::string_type::npos &&
           name.native().find(std::filesystem::path::value_type{}) == std::filesystem::path::string_type::npos;
}

#ifdef _WIN32
struct FileHandle {
    HANDLE value;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
};

bool plain_directory(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    return ::GetFileType(file) == FILE_TYPE_DISK && ::GetFileInformationByHandle(file, &info) &&
           (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == FILE_ATTRIBUTE_DIRECTORY;
}
#else
struct FileHandle {
    int value;
    ~FileHandle() { if (value >= 0) ::close(value); }
};

constexpr int kDirectoryFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
#endif

} // namespace

struct ReadOnlyDirectory::Handle {
#ifdef _WIN32
    std::filesystem::path path;
    // Keep ancestors open without write or delete sharing for as long as the path is in use.
    std::shared_ptr<const Handle> parent;
    FileHandle file;
    Handle(std::filesystem::path at, std::shared_ptr<const Handle> above)
        : path(std::move(at)), parent(std::move(above)),
          file{::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)} {}
    bool valid() const { return file.value != INVALID_HANDLE_VALUE && plain_directory(file.value); }
#else
    FileHandle file;
    explicit Handle(int fd) : file{fd} {}
    bool valid() const { return file.value >= 0; }
#endif
};

ReadOnlyDirectory::ReadOnlyDirectory(std::shared_ptr<const Handle> handle) : handle_(std::move(handle)) {}

std::optional<ReadOnlyDirectory> ReadOnlyDirectory::open(const std::filesystem::path& path) {
    if (path.empty()) return std::nullopt;
    for (const auto& component : path)
        if (component == "..") return std::nullopt;
    // The folder itself is one the player chose, or Hailstone's own, and the path to it may pass through links (a
    // Wine prefix's dosdevices, a /home that is a link on Fedora Atomic and Bazzite, a games folder on another disk).
    // Those are resolved once here; nothing below the folder is followed.
    std::error_code ec;
    const auto absolute = std::filesystem::canonical(path, ec);
    if (ec || !absolute.is_absolute()) return std::nullopt;
#ifdef _WIN32
    auto handle = std::make_shared<Handle>(absolute.root_path(), nullptr);
#else
    auto handle = std::make_shared<Handle>(::open(absolute.root_path().c_str(), kDirectoryFlags));
#endif
    if (!handle->valid()) return std::nullopt;
    std::optional<ReadOnlyDirectory> directory = ReadOnlyDirectory(std::move(handle));
    for (const auto& component : absolute.relative_path()) {
        if (component.empty()) continue;
        directory = directory->child(component);
        if (!directory) return std::nullopt;
    }
    return directory;
}

std::optional<ReadOnlyDirectory> ReadOnlyDirectory::child(const std::filesystem::path& name) const {
    if (!leaf_name(name)) return std::nullopt;
#ifdef _WIN32
    auto handle = std::make_shared<Handle>(handle_->path / name, handle_);
#else
    auto handle = std::make_shared<Handle>(::openat(handle_->file.value, name.c_str(), kDirectoryFlags));
#endif
    if (!handle->valid()) return std::nullopt;
    return ReadOnlyDirectory(std::move(handle));
}

std::optional<std::vector<ReadOnlyDirectory::Entry>> ReadOnlyDirectory::list(std::size_t max_entries) const {
    std::vector<Entry> entries;
#ifdef _WIN32
    WIN32_FIND_DATAW record{};
    const HANDLE search = ::FindFirstFileW((handle_->path / "*").c_str(), &record);
    if (search == INVALID_HANDLE_VALUE) {
        if (::GetLastError() == ERROR_FILE_NOT_FOUND) return entries;
        return std::nullopt;
    }
    struct SearchHandle {
        HANDLE value;
        ~SearchHandle() { ::FindClose(value); }
    } close{search};
    do {
        std::filesystem::path name(record.cFileName);
        if (name == "." || name == "..") continue;
        if (entries.size() == max_entries) return std::nullopt;
        EntryType type = EntryType::Other;
        if (!(record.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)))
            type = record.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? EntryType::Directory : EntryType::File;
        entries.push_back({std::move(name), type});
    } while (::FindNextFileW(search, &record));
    if (::GetLastError() != ERROR_NO_MORE_FILES) return std::nullopt;
#else
    const int descriptor = ::openat(handle_->file.value, ".", kDirectoryFlags);
    if (descriptor < 0) return std::nullopt;
    DIR* opened = ::fdopendir(descriptor);
    if (!opened) {
        ::close(descriptor);
        return std::nullopt;
    }
    struct SearchHandle {
        DIR* value;
        ~SearchHandle() { ::closedir(value); }
    } search{opened};
    while (true) {
        errno = 0;
        const dirent* record = ::readdir(search.value);
        if (!record) {
            if (errno) return std::nullopt;
            break;
        }
        std::filesystem::path name(record->d_name);
        if (name == "." || name == "..") continue;
        if (entries.size() == max_entries) return std::nullopt;
        EntryType type = EntryType::Other;
        struct stat info{};
        if (::fstatat(handle_->file.value, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISREG(info.st_mode)) type = EntryType::File;
            if (S_ISDIR(info.st_mode)) type = EntryType::Directory;
        }
        entries.push_back({std::move(name), type});
    }
#endif
    return entries;
}

std::optional<std::string> ReadOnlyDirectory::read(const std::filesystem::path& name, std::size_t max_bytes) const {
    if (!leaf_name(name)) return std::nullopt;
#ifdef _WIN32
    const FileHandle file{::CreateFileW((handle_->path / name).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE || ::GetFileType(file.value) != FILE_TYPE_DISK) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER length{};
    if (!::GetFileInformationByHandle(file.value, &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !::GetFileSizeEx(file.value, &length) || length.QuadPart < 0 ||
        static_cast<std::uint64_t>(length.QuadPart) > max_bytes ||
        static_cast<std::uint64_t>(length.QuadPart) > std::numeric_limits<DWORD>::max()) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(length.QuadPart), '\0');
    DWORD count = 0;
    if (!::ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())
        return std::nullopt;
#else
    const FileHandle file{::openat(handle_->file.value, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (file.value < 0) return std::nullopt;
    struct stat before{};
    if (::fstat(file.value, &before) != 0 || !S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) > max_bytes) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(file.value, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return std::nullopt;
        offset += static_cast<std::size_t>(count);
    }
    struct stat after{};
    if (::fstat(file.value, &after) != 0 || before.st_size != after.st_size ||
        before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime) return std::nullopt;
#if defined(__APPLE__)
    if (before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
        before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec) return std::nullopt;
#else
    if (before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        return std::nullopt;
#endif
#endif
    return bytes;
}

} // namespace hailstone::platform
