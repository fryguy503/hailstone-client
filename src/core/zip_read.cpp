#include "core/zip_read.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <set>
#include <stdexcept>
#include <system_error>

#include <zlib.h>

#include "core/byte_io.h"
#include "core/path_utf8.h"
#include "core/windows_names.h"
#include "platform/durable_file.h"

namespace hailstone {

namespace {
constexpr std::uint32_t kLocalSig = 0x04034b50, kCentralSig = 0x02014b50, kEndSig = 0x06054b50;
constexpr std::size_t kLocalHeader = 30, kCentralHeader = 46, kEndRecord = 22;
constexpr std::size_t kMaxComment = 0xffff;
// A release's directory is a few hundred KB; more than this is not an archive the release script made.
constexpr std::uint64_t kMaxDirectory = 64u << 20;
constexpr std::size_t kChunk = 256u << 10;
constexpr std::uint16_t kEncrypted = 0x0001;
constexpr std::uint8_t kMadeOnUnix = 3;
} // namespace

bool is_safe_zip_path(std::string_view name) {
    if (name.empty() || name.front() == '/') return false;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f || c == '\\' || c == ':') return false;
    }
    // A folder entry's one trailing '/' is not an empty part.
    std::string_view rest = name.back() == '/' ? name.substr(0, name.size() - 1) : name;
    while (true) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (part.empty() || part == "." || part == "..") return false;
        // A name Windows would write elsewhere ("a.lua." as a.lua) or open as a device (NUL, com1.txt), refused on
        // every platform so an archive extracts to the same files everywhere.
        if (is_windows_aliased_part(part)) return false;
        if (slash == std::string_view::npos) return true;
        rest.remove_prefix(slash + 1);
    }
}

bool ZipReader::fail(std::string why) {
    error_ = std::move(why);
    return false;
}

bool ZipReader::open(const std::filesystem::path& path) {
    entries_.clear();
    memory_.clear();
    in_memory_ = false;
    error_.clear();
    file_ = std::ifstream(path, std::ios::binary);
    if (!file_) return fail("cannot open " + path_utf8(path));
    file_.seekg(0, std::ios::end);
    const std::streamoff end = file_.tellg();
    if (end < 0) return fail("cannot read " + path_utf8(path));
    size_ = static_cast<std::uint64_t>(end);
    if (read_directory()) return true;
    entries_.clear(); // none of a refused archive's entries are offered
    return false;
}

bool ZipReader::open(std::vector<std::byte> bytes) {
    entries_.clear();
    file_ = std::ifstream();
    error_.clear();
    memory_ = std::move(bytes);
    in_memory_ = true;
    size_ = memory_.size();
    if (read_directory()) return true;
    entries_.clear(); // none of a refused archive's entries are offered
    return false;
}

const ZipEntry* ZipReader::find(std::string_view name) const {
    for (const auto& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

bool ZipReader::read_at(std::uint64_t offset, std::span<std::byte> out) {
    if (offset > size_ || out.size() > size_ - offset) return false;
    if (in_memory_) {
        std::copy_n(memory_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<std::size_t>(file_.gcount()) == out.size();
}

bool ZipReader::read_directory() {
    if (size_ < kEndRecord) return fail("not a zip archive (too short)");
    // The end record is the last thing in the archive, followed only by its comment (at most 64 KiB).
    const std::uint64_t tail_size = std::min<std::uint64_t>(size_, kEndRecord + kMaxComment);
    std::vector<std::byte> tail(static_cast<std::size_t>(tail_size));
    if (!read_at(size_ - tail_size, tail)) return fail("cannot read the archive's end");
    std::size_t at = tail.size() - kEndRecord + 1;
    bool found = false;
    while (at-- > 0) {
        ByteReader r(std::span<const std::byte>(tail).subspan(at));
        if (r.le<std::uint32_t>() != kEndSig) continue;
        r.skip(16);
        const std::uint16_t comment = r.le<std::uint16_t>();
        if (at + kEndRecord + comment == tail.size()) {
            found = true;
            break;
        }
    }
    if (!found) return fail("not a zip archive (no end record)");

    std::uint64_t dir_offset = 0, dir_size = 0;
    std::size_t count = 0;
    {
        ByteReader r(std::span<const std::byte>(tail).subspan(at + 4));
        const std::uint16_t disk = r.le<std::uint16_t>(), dir_disk = r.le<std::uint16_t>();
        const std::uint16_t here = r.le<std::uint16_t>(), total = r.le<std::uint16_t>();
        dir_size = r.le<std::uint32_t>();
        dir_offset = r.le<std::uint32_t>();
        if (disk != 0 || dir_disk != 0 || here != total) return fail("split archives are not supported");
        if (total == 0xffff || dir_size == 0xffffffffu || dir_offset == 0xffffffffu)
            return fail("zip64 archives are not supported");
        count = total;
    }
    const std::uint64_t end_at = size_ - tail_size + at;
    if (dir_offset > end_at || dir_size > end_at - dir_offset) return fail("the central directory is out of bounds");
    if (dir_size > kMaxDirectory) return fail("the central directory is too large");
    if (count * kCentralHeader > dir_size) return fail("the central directory is too small for its entries");

    std::vector<std::byte> dir(static_cast<std::size_t>(dir_size));
    if (!read_at(dir_offset, dir)) return fail("cannot read the central directory");
    std::set<std::string> seen;
    try {
        ByteReader r(dir);
        entries_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (r.le<std::uint32_t>() != kCentralSig) return fail("a central directory entry is damaged");
            const std::uint16_t made_by = r.le<std::uint16_t>();
            r.skip(2); // version needed
            const std::uint16_t flags = r.le<std::uint16_t>();
            ZipEntry e;
            e.method = r.le<std::uint16_t>();
            r.skip(4); // time, date
            e.crc32 = r.le<std::uint32_t>();
            e.packed = r.le<std::uint32_t>();
            e.size = r.le<std::uint32_t>();
            const std::uint16_t name_len = r.le<std::uint16_t>(), extra_len = r.le<std::uint16_t>();
            const std::uint16_t comment_len = r.le<std::uint16_t>();
            r.skip(4); // disk start, internal attributes
            const std::uint32_t external = r.le<std::uint32_t>();
            e.offset = r.le<std::uint32_t>();
            auto name = r.bytes(name_len);
            e.name.assign(reinterpret_cast<const char*>(name.data()), name.size());
            r.skip(static_cast<std::size_t>(extra_len) + comment_len);

            if (flags & kEncrypted) return fail("encrypted entries are not supported: " + e.name);
            if (e.packed == 0xffffffffu || e.size == 0xffffffffu || e.offset == 0xffffffffu)
                return fail("zip64 entries are not supported: " + e.name);
            if (!is_safe_zip_path(e.name)) return fail("unsafe name in the archive: " + e.name);
            // The entry's data lies before the directory; its local header is checked again at extraction.
            if (e.offset > dir_offset || dir_offset - e.offset < kLocalHeader ||
                e.packed > dir_offset - e.offset - kLocalHeader)
                return fail("an entry is out of bounds: " + e.name);
            // Two entries for one file (or two differing only in case or in trailing dots and spaces, one file on
            // Windows and macOS) would make what lands on disk depend on the order they are extracted in.
            std::string key = windows_file_key(e.is_directory() ? e.name.substr(0, e.name.size() - 1) : e.name);
            if (!seen.insert(std::move(key)).second) return fail("the archive names a file twice: " + e.name);
            if ((made_by >> 8) == kMadeOnUnix) e.unix_mode = external >> 16;
            entries_.push_back(std::move(e));
        }
    } catch (const std::out_of_range&) {
        return fail("the central directory is truncated");
    }
    return true;
}

bool ZipReader::extract(const ZipEntry& entry, const Sink& sink) {
    if (entry.method != 0 && entry.method != 8)
        return fail("unsupported compression method " + std::to_string(entry.method) + ": " + entry.name);
    if (entry.method == 0 && entry.packed != entry.size) return fail("a stored entry's sizes differ: " + entry.name);

    std::array<std::byte, kLocalHeader> header{};
    if (!read_at(entry.offset, header)) return fail("cannot read the local header: " + entry.name);
    ByteReader h(header);
    if (h.le<std::uint32_t>() != kLocalSig) return fail("a local header is damaged: " + entry.name);
    h.skip(22);
    const std::uint16_t name_len = h.le<std::uint16_t>(), extra_len = h.le<std::uint16_t>();
    // The local header's sizes and CRC may be zero (a data descriptor follows the data); the directory's are used.
    const std::uint64_t data = std::uint64_t{entry.offset} + kLocalHeader + name_len + extra_len;
    if (data > size_ || entry.packed > size_ - data) return fail("an entry's data is out of bounds: " + entry.name);

    std::vector<std::byte> in(std::min<std::size_t>(kChunk, std::max<std::size_t>(entry.packed, 1)));
    std::uint64_t read = 0, written = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    auto emit = [&](std::span<const std::byte> piece) {
        if (piece.empty()) return true;
        if (piece.size() > entry.size - written) return fail("an entry is larger than it says: " + entry.name);
        written += piece.size();
        crc = crc32(crc, reinterpret_cast<const Bytef*>(piece.data()), static_cast<uInt>(piece.size()));
        if (!sink(piece)) return fail("cannot write " + entry.name);
        return true;
    };

    if (entry.method == 0) {
        while (read < entry.packed) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(in.size(), entry.packed - read));
            if (!read_at(data + read, std::span(in).first(n))) return fail("cannot read " + entry.name);
            read += n;
            if (!emit(std::span<const std::byte>(in).first(n))) return false;
        }
    } else {
        z_stream z{};
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return fail("cannot start inflating " + entry.name);
        struct End {
            z_stream* z;
            ~End() { inflateEnd(z); }
        } end{&z};
        std::vector<std::byte> out(kChunk);
        bool done = false;
        while (!done) {
            if (z.avail_in == 0) {
                if (read == entry.packed) return fail("an entry is truncated: " + entry.name);
                const std::size_t n =
                    static_cast<std::size_t>(std::min<std::uint64_t>(in.size(), entry.packed - read));
                if (!read_at(data + read, std::span(in).first(n))) return fail("cannot read " + entry.name);
                read += n;
                z.next_in = reinterpret_cast<Bytef*>(in.data());
                z.avail_in = static_cast<uInt>(n);
            }
            z.next_out = reinterpret_cast<Bytef*>(out.data());
            z.avail_out = static_cast<uInt>(out.size());
            const int rc = inflate(&z, Z_NO_FLUSH);
            if (rc == Z_STREAM_END) {
                done = true;
            } else if (rc != Z_OK) {
                // Z_BUF_ERROR with output room left means the input ran out mid-stream: the next turn reads more.
                if (!(rc == Z_BUF_ERROR && z.avail_in == 0)) return fail("an entry is damaged: " + entry.name);
            }
            const std::size_t produced = out.size() - z.avail_out;
            if (!emit(std::span<const std::byte>(out).first(produced))) return false;
        }
        if (z.avail_in != 0 || read != entry.packed) return fail("an entry has data past its end: " + entry.name);
    }
    if (written != entry.size) return fail("an entry is smaller than it says: " + entry.name);
    if (crc != entry.crc32) return fail("an entry's CRC-32 does not match: " + entry.name);
    return true;
}

bool ZipReader::extract_to_file(const ZipEntry& entry, const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (entry.is_directory()) {
        fs::create_directories(path, ec);
        return ec ? fail("cannot make " + path_utf8(path) + ": " + ec.message()) : true;
    }
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) return fail("cannot make " + path_utf8(path.parent_path()) + ": " + ec.message());
    }
    static std::atomic<unsigned> serial{0};
    fs::path tmp = path;
    tmp += ".part-" + std::to_string(serial++);
    bool ok;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return fail("cannot write " + path_utf8(tmp));
        ok = extract(entry, [&](std::span<const std::byte> piece) {
            out.write(reinterpret_cast<const char*>(piece.data()), static_cast<std::streamsize>(piece.size()));
            return static_cast<bool>(out);
        });
        out.close();
        if (ok && !out) ok = fail("cannot write " + path_utf8(tmp));
    }
#ifndef _WIN32
    // Windows has no execute bit, and its read-only flag is not wanted on an installed file.
    if (ok && (entry.unix_mode & 0777) != 0) {
        fs::permissions(tmp, static_cast<fs::perms>(entry.unix_mode & 0777), fs::perm_options::replace, ec);
        if (ec) ok = fail("cannot set the permissions of " + path_utf8(tmp) + ": " + ec.message());
    }
#endif
    // On the disk before the rename (platform/durable_file.h): the update's state may say the new files are in
    // place soon after, and a power cut must not leave one of them empty.
    if (ok && !platform::sync_file(tmp)) ok = fail("cannot write " + path_utf8(tmp) + " to the disk");
    if (ok) {
        platform::replace_file_synced(tmp, path, ec);
        if (ec) ok = fail("cannot replace " + path_utf8(path) + ": " + ec.message());
    }
    if (!ok) fs::remove(tmp, ec);
    return ok;
}

} // namespace hailstone
