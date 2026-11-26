#pragma once

// A read-only mapping of a file: assets::PfsArchive's whole-file storage (src/assets/pfs.h). The view is the file
// itself, not a heap copy: the bytes live in the system's file cache, shared with other readers and with other
// Hailstone processes mapping the same files, and they can leave the resident set under memory pressure instead of
// sitting in private heap for the process's life. wld::WldFile data parsed from those bytes is still private.

#include <cstddef>
#include <filesystem>
#include <span>

namespace hailstone::platform {

// A read-only mapped file. Move-only, like a handle: copies would each map the file again. The view stays valid
// until the mapping is unmapped (moved-from or destroyed); concurrent reads of the view are safe.
//
// Replacement: on POSIX, renaming a new file over the path leaves the view showing the old bytes until it is
// unmapped, and a fresh open sees the new file. On Windows a rename over a file with a mapped view is typically refused (the
// sharing flags below let the file be opened and deleted, not replaced while a section exists), so map only files
// nothing replaces while they are mapped: assets::PfsArchive::storage_for maps the game folder's files and reads the
// server overlay's (which the server files sync replaces) into memory.
//
// A file truncated in place while mapped makes a read of a page past its new end fault (SIGBUS on POSIX; Windows
// refuses the truncation while the view exists, and faults with EXCEPTION_IN_PAGE_ERROR on a read error of the disk
// or share). read() catches that fault around its copy and throws; view() is not guarded, so code that may meet a
// file someone else writes copies through read() and parses the copy. An edit in place still shows through either.
// See docs/hardening.md "Mapped game archives".
//
// OS calls live here only; callers see a span. The Windows and POSIX spellings differ in their .cpp, nothing else.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // Maps `path` read-only. Throws std::runtime_error when it cannot be opened or mapped. An empty file gives an
    // empty (unmapped) result, as a zero-length read did before.
    static MappedFile open(const std::filesystem::path& path);

    // True when a view is held (an empty file is not mapped). size() is the view's length either way.
    bool mapped() const noexcept { return view_ != nullptr; }
    std::size_t size() const noexcept { return size_; }
    std::span<const std::byte> view() const noexcept {
        return std::span<const std::byte>(view_bytes_, size_);
    }

    // Copies out.size() bytes of the view from `offset` into `out`. Throws std::out_of_range past the view's end and
    // std::runtime_error when the read faults (the file shrank under the mapping, or its page could not be read).
    // Safe to call from several threads at once.
    void read(std::size_t offset, std::span<std::byte> out) const;

private:
    void reset() noexcept;

    const std::byte* view_bytes_ = nullptr;
    void* view_ = nullptr;   // the OS view (mmap / MapViewOfFile); POSIX and Windows put it here
    std::size_t size_ = 0;
    void* file_ = nullptr;   // Windows: the file handle (POSIX closes its fd after mmap)
    void* mapping_ = nullptr; // Windows: the file mapping (POSIX: none)
};

} // namespace hailstone::platform
