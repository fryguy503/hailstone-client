#pragma once

// Read-only cache payloads and cooperative locks. Native handles stay here; a view's owner must outlive every span.
// Cache directories are private to the current user. Existing ancestors are never chmod'ed or given a new ACL.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>

namespace hailstone::platform {

class ReadOnlyMapping {
public:
    ReadOnlyMapping() = default;
    ~ReadOnlyMapping();
    ReadOnlyMapping(ReadOnlyMapping&& other) noexcept;
    ReadOnlyMapping& operator=(ReadOnlyMapping&& other) noexcept;
    ReadOnlyMapping(const ReadOnlyMapping&) = delete;
    ReadOnlyMapping& operator=(const ReadOnlyMapping&) = delete;

    // Rejects links/reparse points, non-regular and empty files, and lengths that cannot fit this address space.
    // POSIX mappings retain the backing object without an open descriptor; Windows keeps the deny-write file handle.
    static std::optional<ReadOnlyMapping> open(const std::filesystem::path& path);
    std::span<const std::byte> bytes() const { return {data_, size_}; }

private:
    void reset() noexcept;
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    std::intptr_t file_ = -1;
    std::intptr_t section_ = 0;
};

enum class FileLockMode { Shared, Exclusive };

class ScopedFileLock {
public:
    ScopedFileLock() = default;
    ~ScopedFileLock();
    ScopedFileLock(ScopedFileLock&& other) noexcept;
    ScopedFileLock& operator=(ScopedFileLock&& other) noexcept;
    ScopedFileLock(const ScopedFileLock&) = delete;
    ScopedFileLock& operator=(const ScopedFileLock&) = delete;

    // Nonblocking. A missing regular lock file is created privately; failures, including contention, return nullopt.
    // The stable lock file is never unlinked. Closing the owner (or process termination) releases its native lock.
    static std::optional<ScopedFileLock> try_acquire(const std::filesystem::path& path, FileLockMode mode);

private:
    void reset() noexcept;
    std::intptr_t file_ = -1;
};

enum class PublishResult { Published, Exists, Error };

class CacheTempFile {
public:
    CacheTempFile() = default;
    ~CacheTempFile();
    CacheTempFile(CacheTempFile&& other) noexcept;
    CacheTempFile& operator=(CacheTempFile&& other) noexcept;
    CacheTempFile(const CacheTempFile&) = delete;
    CacheTempFile& operator=(const CacheTempFile&) = delete;

    // directory must already be private. Names start with .tmp-; creation is exclusive, and abandoned files are removed.
    static std::optional<CacheTempFile> create(const std::filesystem::path& directory);
    bool write(std::span<const std::byte> bytes);
    // Flushes and closes before atomically publishing in the same directory. Never replaces an existing entry.
    PublishResult publish(const std::filesystem::path& destination);

private:
    void reset() noexcept;
    std::intptr_t file_ = -1;
    std::filesystem::path path_;
    bool good_ = false;
};

// Creates missing ancestors privately; existing ancestors must be directories without links/reparse points.
// The cache leaf must be current-user owned and private already. Never repairs permissions on an existing directory.
bool prepare_private_cache_directory(const std::filesystem::path& path);

struct FileStamp {
    std::uint64_t size = 0;
    // File identity plus modification/change timestamps, from the open handle rather than a second path lookup.
    std::array<std::uint64_t, 4> identity{};
    bool operator==(const FileStamp&) const = default;
};
std::optional<FileStamp> file_stamp(std::FILE* file);

} // namespace hailstone::platform
