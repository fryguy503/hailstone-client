#include "platform/shared_file.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hailstone::platform {
namespace {

void close_file(std::intptr_t file) noexcept {
    if (file == -1) return;
#if defined(_WIN32)
    ::CloseHandle(reinterpret_cast<HANDLE>(file));
#else
    ::close(static_cast<int>(file));
#endif
}

std::optional<std::filesystem::path> absolute_path(const std::filesystem::path& path) {
    if (path.empty()) return std::nullopt;
    // Do not normalize '..' across a link before checking it.
    for (const auto& component : path)
        if (component == "..") return std::nullopt;
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec) return std::nullopt;
    return absolute.lexically_normal();
}

#if defined(_WIN32)
struct UserSecurity {
    std::vector<std::byte> token_info;
    std::vector<std::byte> acl_storage;
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};

    PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(token_info.data())->User.Sid; }
    bool initialize() {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        DWORD size = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (size == 0) {
            ::CloseHandle(token);
            return false;
        }
        token_info.resize(size);
        const bool got = ::GetTokenInformation(token, TokenUser, token_info.data(), size, &size) != 0;
        ::CloseHandle(token);
        if (!got || !::IsValidSid(sid())) return false;
        const DWORD acl_size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) +
                               ::GetLengthSid(sid());
        acl_storage.resize(acl_size);
        auto* acl = reinterpret_cast<ACL*>(acl_storage.data());
        if (!::InitializeAcl(acl, acl_size, ACL_REVISION) ||
            !::AddAccessAllowedAceEx(acl, ACL_REVISION, CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE,
                                     FILE_ALL_ACCESS, sid()) ||
            !::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !::SetSecurityDescriptorOwner(&descriptor, sid(), FALSE) ||
            !::SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
            !::SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) return false;
        attributes = {sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
        return true;
    }
};

bool regular_handle(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    return ::GetFileType(file) == FILE_TYPE_DISK && ::GetFileInformationByHandle(file, &info) &&
           (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool private_handle(HANDLE file, PSID user) {
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                          &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    bool ok = owner && ::EqualSid(owner, user) && acl;
    for (DWORD i = 0; ok && i < acl->AceCount; ++i) {
        void* entry = nullptr;
        if (!::GetAce(acl, i, &entry)) { ok = false; break; }
        const auto* header = static_cast<const ACE_HEADER*>(entry);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; }
        const auto* allow = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
        // The new protected DACL grants only this user. Accept equivalent inherited DACLs only when equally private.
        if (allow->Mask != 0 && !::EqualSid(const_cast<DWORD*>(&allow->SidStart), user)) ok = false;
    }
    if (descriptor) ::LocalFree(descriptor);
    return ok;
}

bool directory_at(const std::filesystem::path& path, bool private_leaf, UserSecurity* user) {
    HANDLE file = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | (private_leaf ? READ_CONTROL : 0),
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = ::GetFileInformationByHandle(file, &info) &&
              (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
              (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    if (ok && private_leaf) ok = user && private_handle(file, user->sid());
    ::CloseHandle(file);
    return ok;
}
#else
bool directory_at(const std::filesystem::path& path, bool private_leaf) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
    if (private_leaf) return info.st_uid == ::geteuid() && (info.st_mode & 0077) == 0;
    // Root-owned ancestors and sticky temp directories are normal. An arbitrary writable ancestor is not.
    if (info.st_uid != ::geteuid() && info.st_uid != 0) return false;
    return (info.st_mode & 0022) == 0 || (info.st_mode & S_ISVTX) != 0;
}
#endif

bool directory_chain(const std::filesystem::path& path, bool create, bool private_leaf) {
    const auto absolute = absolute_path(path);
    if (!absolute) return false;
#if defined(_WIN32)
    UserSecurity user;
    if ((create || private_leaf) && !user.initialize()) return false;
#endif
    std::filesystem::path current = absolute->root_path();
#if defined(_WIN32)
    if (!directory_at(current, false, nullptr)) return false;
#else
    if (!directory_at(current, false)) return false;
#endif
    for (const auto& component : absolute->relative_path()) {
        current /= component;
        const bool leaf = current == *absolute;
#if defined(_WIN32)
        if (create && !::CreateDirectoryW(current.c_str(), &user.attributes) &&
            ::GetLastError() != ERROR_ALREADY_EXISTS) return false;
        if (!directory_at(current, private_leaf && leaf, &user)) return false;
#else
        if (create && ::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) return false;
        if (!directory_at(current, private_leaf && leaf)) return false;
#endif
    }
    // A filesystem root must never become a cache leaf.
    return !private_leaf || *absolute != absolute->root_path();
}

bool same_directory(const std::filesystem::path& a, const std::filesystem::path& b) {
    const auto left = absolute_path(a);
    const auto right = absolute_path(b);
    return left && right && *left == *right;
}

} // namespace

ReadOnlyMapping::~ReadOnlyMapping() { reset(); }
ReadOnlyMapping::ReadOnlyMapping(ReadOnlyMapping&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
      file_(std::exchange(other.file_, -1)), section_(std::exchange(other.section_, 0)) {}
ReadOnlyMapping& ReadOnlyMapping::operator=(ReadOnlyMapping&& other) noexcept {
    if (this != &other) {
        reset();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        file_ = std::exchange(other.file_, -1);
        section_ = std::exchange(other.section_, 0);
    }
    return *this;
}
void ReadOnlyMapping::reset() noexcept {
    if (data_) {
#if defined(_WIN32)
        ::UnmapViewOfFile(data_);
#else
        ::munmap(const_cast<std::byte*>(data_), size_);
#endif
    }
#if defined(_WIN32)
    if (section_) ::CloseHandle(reinterpret_cast<HANDLE>(section_));
#endif
    close_file(file_);
    data_ = nullptr;
    size_ = 0;
    file_ = -1;
    section_ = 0;
}

std::optional<ReadOnlyMapping> ReadOnlyMapping::open(const std::filesystem::path& path) {
    const auto absolute = absolute_path(path);
    if (!absolute || !directory_chain(absolute->parent_path(), false, false)) return std::nullopt;
    ReadOnlyMapping result;
#if defined(_WIN32)
    HANDLE file = ::CreateFileW(absolute->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    result.file_ = reinterpret_cast<std::intptr_t>(file);
    LARGE_INTEGER length{};
    if (!regular_handle(file) || !::GetFileSizeEx(file, &length) || length.QuadPart <= 0 ||
        static_cast<std::uint64_t>(length.QuadPart) > std::numeric_limits<std::size_t>::max()) return std::nullopt;
    result.size_ = static_cast<std::size_t>(length.QuadPart);
    HANDLE section = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!section) return std::nullopt;
    result.section_ = reinterpret_cast<std::intptr_t>(section);
    result.data_ = static_cast<const std::byte*>(::MapViewOfFile(section, FILE_MAP_READ, 0, 0, result.size_));
    if (!result.data_) return std::nullopt;
#else
    const int file = ::open(absolute->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (file < 0) return std::nullopt;
    result.file_ = file;
    struct stat info{};
    if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<std::uint64_t>(info.st_size) > std::numeric_limits<std::size_t>::max()) return std::nullopt;
    result.size_ = static_cast<std::size_t>(info.st_size);
    void* mapped = ::mmap(nullptr, result.size_, PROT_READ, MAP_SHARED, file, 0);
    if (mapped == MAP_FAILED) return std::nullopt;
    result.data_ = static_cast<const std::byte*>(mapped);
    // mmap retains the file object itself. Keeping another descriptor per archive needlessly exhausts small limits.
    ::close(file);
    result.file_ = -1;
#endif
    return result;
}

ScopedFileLock::~ScopedFileLock() { reset(); }
ScopedFileLock::ScopedFileLock(ScopedFileLock&& other) noexcept : file_(std::exchange(other.file_, -1)) {}
ScopedFileLock& ScopedFileLock::operator=(ScopedFileLock&& other) noexcept {
    if (this != &other) { reset(); file_ = std::exchange(other.file_, -1); }
    return *this;
}
void ScopedFileLock::reset() noexcept { close_file(file_); file_ = -1; }

std::optional<ScopedFileLock> ScopedFileLock::try_acquire(const std::filesystem::path& path, FileLockMode mode) {
    const auto absolute = absolute_path(path);
    if (!absolute || !directory_chain(absolute->parent_path(), false, true)) return std::nullopt;
    ScopedFileLock result;
#if defined(_WIN32)
    UserSecurity user;
    if (!user.initialize()) return std::nullopt;
    HANDLE file = ::CreateFileW(absolute->c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &user.attributes, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    result.file_ = reinterpret_cast<std::intptr_t>(file);
    if (!regular_handle(file) || !private_handle(file, user.sid())) return std::nullopt;
    OVERLAPPED overlap{};
    const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (mode == FileLockMode::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    if (!::LockFileEx(file, flags, 0, 1, 0, &overlap)) return std::nullopt;
#else
    const int file = ::open(absolute->c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (file < 0) return std::nullopt;
    result.file_ = file;
    struct stat info{};
    if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 0077) != 0) return std::nullopt;
    if (::flock(file, (mode == FileLockMode::Exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) return std::nullopt;
#endif
    return result;
}

CacheTempFile::~CacheTempFile() { reset(); }
CacheTempFile::CacheTempFile(CacheTempFile&& other) noexcept
    : file_(std::exchange(other.file_, -1)), path_(std::move(other.path_)), good_(std::exchange(other.good_, false)) {
    other.path_.clear();
}
CacheTempFile& CacheTempFile::operator=(CacheTempFile&& other) noexcept {
    if (this != &other) {
        reset();
        file_ = std::exchange(other.file_, -1);
        path_ = std::move(other.path_);
        other.path_.clear();
        good_ = std::exchange(other.good_, false);
    }
    return *this;
}
void CacheTempFile::reset() noexcept {
    close_file(file_);
    file_ = -1;
    if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        path_.clear();
    }
    good_ = false;
}

std::optional<CacheTempFile> CacheTempFile::create(const std::filesystem::path& directory) {
    const auto absolute = absolute_path(directory);
    if (!absolute || !directory_chain(*absolute, false, true)) return std::nullopt;
    static std::atomic<std::uint64_t> sequence{0};
#if defined(_WIN32)
    UserSecurity user;
    if (!user.initialize()) return std::nullopt;
    const auto pid = static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    const auto pid = static_cast<std::uint64_t>(::getpid());
#endif
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        std::uint64_t random = 0;
        try {
            std::random_device source;
            random = (static_cast<std::uint64_t>(source()) << 32) ^ source();
        } catch (...) { return std::nullopt; }
        CacheTempFile result;
        result.path_ = *absolute / (".tmp-" + std::to_string(pid) + "-" + std::to_string(sequence++) + "-" +
                                    std::to_string(random));
#if defined(_WIN32)
        HANDLE file = ::CreateFileW(result.path_.c_str(), GENERIC_WRITE, 0, &user.attributes, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            result.path_.clear();
            if (::GetLastError() == ERROR_FILE_EXISTS || ::GetLastError() == ERROR_ALREADY_EXISTS) continue;
            return std::nullopt;
        }
        result.file_ = reinterpret_cast<std::intptr_t>(file);
#else
        const int file = ::open(result.path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (file < 0) {
            result.path_.clear();
            if (errno == EEXIST) continue;
            return std::nullopt;
        }
        result.file_ = file;
#endif
        result.good_ = true;
        return result;
    }
    return std::nullopt;
}

bool CacheTempFile::write(std::span<const std::byte> bytes) {
    if (!good_ || file_ == -1) return false;
    std::size_t done = 0;
    while (done < bytes.size()) {
        const std::size_t chunk = std::min<std::size_t>(bytes.size() - done, 0x40000000u);
#if defined(_WIN32)
        DWORD wrote = 0;
        if (!::WriteFile(reinterpret_cast<HANDLE>(file_), bytes.data() + done, static_cast<DWORD>(chunk), &wrote,
                         nullptr) || wrote == 0) { good_ = false; return false; }
        done += wrote;
#else
        const ssize_t wrote = ::write(static_cast<int>(file_), bytes.data() + done, chunk);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) { good_ = false; return false; }
        done += static_cast<std::size_t>(wrote);
#endif
    }
    return true;
}

PublishResult CacheTempFile::publish(const std::filesystem::path& destination) {
    const auto absolute = absolute_path(destination);
    if (!good_ || file_ == -1 || !absolute || !same_directory(path_.parent_path(), absolute->parent_path()) ||
        !directory_chain(absolute->parent_path(), false, true)) return PublishResult::Error;
#if defined(_WIN32)
    bool synced = ::FlushFileBuffers(reinterpret_cast<HANDLE>(file_)) != 0;
    if (!::CloseHandle(reinterpret_cast<HANDLE>(file_))) synced = false;
#else
    // Read-only once published: a blob is never rewritten, and a stray write through its name would change bytes
    // other clients have mapped (docs/hardening.md).
    bool synced = ::fchmod(static_cast<int>(file_), 0400) == 0 && ::fsync(static_cast<int>(file_)) == 0;
    if (::close(static_cast<int>(file_)) != 0) synced = false;
#endif
    file_ = -1;
    good_ = false;
    if (!synced) return PublishResult::Error;
#if defined(_WIN32)
    // Without REPLACE_EXISTING this move never substitutes a new file object for an existing content key.
    if (!::MoveFileExW(path_.c_str(), absolute->c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = ::GetLastError();
        return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ? PublishResult::Exists : PublishResult::Error;
    }
#else
    // link is an atomic publish-if-absent on both Linux and macOS. The temporary name is removed only afterwards.
    if (::link(path_.c_str(), absolute->c_str()) != 0) return errno == EEXIST ? PublishResult::Exists : PublishResult::Error;
    (void)::unlink(path_.c_str());
    const int directory = ::open(absolute->parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory >= 0) { (void)::fsync(directory); ::close(directory); }
#endif
    path_.clear();
    return PublishResult::Published;
}

bool prepare_private_cache_directory(const std::filesystem::path& path) {
    return directory_chain(path, true, true);
}

std::optional<FileStamp> file_stamp(std::FILE* file) {
    if (!file) return std::nullopt;
    FileStamp stamp;
#if defined(_WIN32)
    const std::intptr_t native = ::_get_osfhandle(::_fileno(file));
    if (native == -1) return std::nullopt;
    HANDLE handle = reinterpret_cast<HANDLE>(native);
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO times{};
    if (!regular_handle(handle) || !::GetFileInformationByHandle(handle, &info) ||
        !::GetFileInformationByHandleEx(handle, FileBasicInfo, &times, sizeof times)) return std::nullopt;
    stamp.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    stamp.identity = {info.dwVolumeSerialNumber,
                      (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
                      static_cast<std::uint64_t>(times.LastWriteTime.QuadPart),
                      static_cast<std::uint64_t>(times.ChangeTime.QuadPart)};
#else
    struct stat info{};
    if (::fstat(::fileno(file), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) return std::nullopt;
    stamp.size = static_cast<std::uint64_t>(info.st_size);
#if defined(__APPLE__)
    const auto modified = info.st_mtimespec;
    const auto changed = info.st_ctimespec;
#else
    const auto modified = info.st_mtim;
    const auto changed = info.st_ctim;
#endif
    stamp.identity = {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino),
                      static_cast<std::uint64_t>(modified.tv_sec) * 1000000000u + static_cast<std::uint64_t>(modified.tv_nsec),
                      static_cast<std::uint64_t>(changed.tv_sec) * 1000000000u + static_cast<std::uint64_t>(changed.tv_nsec)};
#endif
    return stamp;
}

} // namespace hailstone::platform
