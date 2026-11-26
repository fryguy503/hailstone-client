#include "core/zip_write.h"

#include <limits>

#include <zlib.h>

#include "core/atomic_file.h"

namespace hailstone {

namespace {
constexpr std::size_t kLocalHeader = 30, kCentralHeader = 46, kEndRecord = 22;
constexpr std::size_t kMax = std::numeric_limits<std::uint32_t>::max();
// The end record's count is 16 bits, and 0xffff in it means "see the zip64 record", which this does not write.
constexpr std::size_t kMaxEntries = 0xfffe;

void put16(std::vector<std::byte>& out, std::uint32_t v) {
    out.push_back(static_cast<std::byte>(v & 0xff));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xff));
}
void put32(std::vector<std::byte>& out, std::uint32_t v) {
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}
void put_name(std::vector<std::byte>& out, std::string_view name) {
    for (char c : name) out.push_back(static_cast<std::byte>(c));
}

// The MS-DOS date and time (2-second steps, from 1980).
std::uint16_t dos_time(const std::tm& t) {
    return static_cast<std::uint16_t>((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2));
}
std::uint16_t dos_date(const std::tm& t) {
    // Seven bits of years from 1980: a date before is 1980, one after 2107 is 2107 (it would spill into the month).
    const int year = t.tm_year + 1900 < 1980 ? 0 : (t.tm_year + 1900 - 1980 > 127 ? 127 : t.tm_year + 1900 - 1980);
    return static_cast<std::uint16_t>((year << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday);
}

// Raw deflate (no zlib header), as zip wants; empty when it fails.
std::vector<std::byte> deflate_raw(std::span<const std::byte> data) {
    z_stream z{};
    if (deflateInit2(&z, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
    std::vector<std::byte> out(deflateBound(&z, static_cast<uLong>(data.size())));
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(data.data()));
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    const int r = deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return r == Z_STREAM_END ? out : std::vector<std::byte>{};
}
} // namespace

bool ZipWriter::add(std::string_view name, std::span<const std::byte> data, const std::tm& local) {
    return append(name, data, local, kMax);
}

bool ZipWriter::add(std::string_view name, std::string_view text, const std::tm& local) {
    return add(name, std::as_bytes(std::span(text.data(), text.size())), local);
}

bool ZipWriter::add_within(std::string_view name, std::span<const std::byte> data, const std::tm& local,
                           std::size_t limit) {
    return append(name, data, local, limit < kMax ? limit : kMax);
}

std::size_t ZipWriter::finished_size() const {
    if (finished_) return out_.size();
    return out_.size() + directory_size_ + kEndRecord;
}

bool ZipWriter::append(std::string_view name, std::span<const std::byte> data, const std::tm& local,
                       std::size_t limit) {
    if (finished_ || data.size() > kMax || name.size() > 0xffff || entries_.size() >= kMaxEntries) return false;
    std::vector<std::byte> packed = data.empty() ? std::vector<std::byte>{} : deflate_raw(data);
    const bool stored = data.empty() || packed.empty() || packed.size() >= data.size();
    const std::span<const std::byte> body = stored ? data : std::span<const std::byte>(packed);
    // This file's local header and body, and its central directory entry.
    const std::size_t grows = kLocalHeader + name.size() + body.size() + kCentralHeader + name.size();
    if (finished_size() + grows > limit || out_.size() + kLocalHeader + name.size() + body.size() > kMax) return false;

    Entry e;
    e.name = std::string(name);
    e.crc = static_cast<std::uint32_t>(
        crc32(0L, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
    e.packed = static_cast<std::uint32_t>(body.size());
    e.size = static_cast<std::uint32_t>(data.size());
    e.offset = static_cast<std::uint32_t>(out_.size());
    e.method = stored ? 0 : 8;
    e.time = dos_time(local);
    e.date = dos_date(local);

    put32(out_, 0x04034b50);
    put16(out_, 20);      // version needed: 2.0
    put16(out_, 0x0800);  // flags: the name is UTF-8
    put16(out_, e.method);
    put16(out_, e.time);
    put16(out_, e.date);
    put32(out_, e.crc);
    put32(out_, e.packed);
    put32(out_, e.size);
    put16(out_, static_cast<std::uint32_t>(name.size()));
    put16(out_, 0); // no extra field
    put_name(out_, name);
    out_.insert(out_.end(), body.begin(), body.end());
    entries_.push_back(std::move(e));
    directory_size_ += kCentralHeader + name.size();
    return true;
}

const std::vector<std::byte>& ZipWriter::finish() {
    if (finished_) return out_;
    const auto directory = static_cast<std::uint32_t>(out_.size());
    for (const Entry& e : entries_) {
        put32(out_, 0x02014b50);
        put16(out_, 20); // made by: 2.0, MS-DOS attributes
        put16(out_, 20);
        put16(out_, 0x0800);
        put16(out_, e.method);
        put16(out_, e.time);
        put16(out_, e.date);
        put32(out_, e.crc);
        put32(out_, e.packed);
        put32(out_, e.size);
        put16(out_, static_cast<std::uint32_t>(e.name.size()));
        put16(out_, 0); // extra
        put16(out_, 0); // comment
        put16(out_, 0); // disk
        put16(out_, 0); // internal attributes
        put32(out_, 0); // external attributes
        put32(out_, e.offset);
        put_name(out_, e.name);
    }
    const auto directory_size = static_cast<std::uint32_t>(out_.size() - directory);
    const auto count = static_cast<std::uint32_t>(entries_.size()); // at most kMaxEntries: append() refuses more
    put32(out_, 0x06054b50);
    put16(out_, 0);
    put16(out_, 0);
    put16(out_, count);
    put16(out_, count);
    put32(out_, directory_size);
    put32(out_, directory);
    put16(out_, 0); // no comment
    finished_ = true;
    return out_;
}

bool ZipWriter::write(const std::filesystem::path& path) {
    const auto& bytes = finish();
    return write_file_atomic(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

} // namespace hailstone
