#pragma once

// Minimal zip writer (bug reports): each file is deflated with zlib, or stored when that does not make it smaller
// (PNG, already compressed). No zip64, so the archive and each file stay under 4 GiB and it holds at most 65534 files;
// no encryption. The archive is built in memory and written in one go.

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone {

class ZipWriter {
public:
    // Adds `name` (a relative path with '/' separators) holding `data`, dated `local` (the zip's MS-DOS time, local
    // time; years past 2107, the format's last, are written as 2107). False when the archive would pass 4 GiB or
    // 65534 files.
    bool add(std::string_view name, std::span<const std::byte> data, const std::tm& local);
    bool add(std::string_view name, std::string_view text, const std::tm& local);
    // The size of the archive so far, without the central directory finish() appends.
    std::size_t size() const { return out_.size(); }
    // add() only while the finished archive stays within `limit` bytes (a central directory entry and the end
    // record counted); false, and nothing added, otherwise.
    bool add_within(std::string_view name, std::span<const std::byte> data, const std::tm& local, std::size_t limit);
    // What finish() would make of the archive now.
    std::size_t finished_size() const;
    // The finished archive (the central directory appended once; add() does nothing after).
    const std::vector<std::byte>& finish();
    // finish() written to `path` (through a temporary file renamed over it); false when it cannot be.
    bool write(const std::filesystem::path& path);

private:
    bool append(std::string_view name, std::span<const std::byte> data, const std::tm& local, std::size_t limit);
    struct Entry {
        std::string name;
        std::uint32_t crc = 0, packed = 0, size = 0, offset = 0;
        std::uint16_t method = 0, time = 0, date = 0;
    };
    std::vector<std::byte> out_;
    std::vector<Entry> entries_;
    std::size_t directory_size_ = 0; // the central directory finish() will write, kept as entries are added
    bool finished_ = false;
};

} // namespace hailstone
