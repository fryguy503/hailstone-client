#include "assets/pfs.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include <zlib.h>

#include "core/byte_io.h"
#include "core/game_dir.h"
#include "core/log.h"

namespace hailstone::assets {

namespace {
constexpr std::uint32_t kMagic = 0x20534650; // "PFS "
constexpr std::uint32_t kNameTableCrc = 0x61580ac9;
// The most a zlib stream expands: deflate's limit is about 1032:1 (real archives stay under 300:1).
constexpr std::uint64_t kMaxDeflateRatio = 1032;
// A valid zlib stream of zeros stays inside that ratio, so the size a member declares is capped as well. The largest
// stock member (theater.eqg's) is about 37 MB and the largest name table about 100 KB.
constexpr std::uint32_t kMaxMemberSize = 256u << 20;
constexpr std::uint32_t kMaxNameTableSize = 16u << 20;

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i << 24;
        for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04c11db7u : c << 1;
        t[i] = c;
    }
    return t;
}
constexpr auto kCrcTable = make_crc_table();

// Appends one zlib stream of exactly `size` bytes to `out`, growing it with what zlib really writes (64 KB, then
// doubling) rather than zero-filling `size` up front: the ratio check lets ~4 MB of junk declare 4 GB. Throws on error.
void inflate_append(std::span<const std::byte> in, std::size_t size, std::vector<std::byte>& out) {
    constexpr std::size_t kFirstChunk = 64 * 1024;
    z_stream z{};
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    if (inflateInit(&z) != Z_OK) throw std::runtime_error("inflateInit failed");
    const std::size_t start = out.size();
    std::size_t produced = 0;
    int r = Z_OK;
    while (r == Z_OK) {
        std::size_t chunk = std::min(size - produced, std::max(kFirstChunk, produced));
        // At the declared size, a spare byte shows whether the stream ends there or runs on.
        std::byte spare{};
        Bytef* to = reinterpret_cast<Bytef*>(&spare);
        if (chunk > 0) {
            out.resize(start + produced + chunk);
            to = reinterpret_cast<Bytef*>(out.data() + start + produced);
        }
        z.next_out = to;
        z.avail_out = chunk > 0 ? static_cast<uInt>(chunk) : 1u;
        r = inflate(&z, Z_NO_FLUSH);
        std::size_t wrote = (chunk > 0 ? chunk : 1u) - z.avail_out;
        if (chunk == 0 && wrote > 0) r = Z_DATA_ERROR; // more than declared
        if (chunk == 0 && r == Z_OK) r = Z_DATA_ERROR; // no end at the declared size
        if (chunk > 0) produced += wrote;
        if (r == Z_OK && wrote == 0 && chunk > 0) r = Z_BUF_ERROR; // input ran out
    }
    inflateEnd(&z);
    out.resize(start + produced);
    if (r != Z_STREAM_END || produced != size) throw std::runtime_error("zlib block did not inflate to its declared size");
}

// Inflates the first `out.size()` bytes of one zlib stream, the rest left alone. Throws when it holds fewer.
void inflate_prefix(std::span<const std::byte> in, std::span<std::byte> out) {
    if (out.empty()) return;
    z_stream z{};
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    if (inflateInit(&z) != Z_OK) throw std::runtime_error("inflateInit failed");
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    int r = Z_OK;
    while (r == Z_OK && z.avail_out > 0) {
        const uInt before = z.avail_out;
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_OK && z.avail_out == before) r = Z_BUF_ERROR; // input ran out
    }
    const bool full = z.avail_out == 0;
    inflateEnd(&z);
    if (!full) throw std::runtime_error("zlib block is shorter than its declared size");
}
} // namespace

std::uint32_t pfs_name_crc(std::string_view name) {
    std::uint32_t c = 0;
    auto step = [&](unsigned char b) { c = (c << 8) ^ kCrcTable[((c >> 24) ^ b) & 0xff]; };
    for (char ch : name) step(static_cast<unsigned char>(ch));
    step(0);
    return c;
}

void zlib_inflate(std::span<const std::byte> in, std::span<std::byte> out) {
    z_stream z{};
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    if (inflateInit(&z) != Z_OK) throw std::runtime_error("inflateInit failed");
    int r = inflate(&z, Z_FINISH);
    std::size_t produced = out.size() - z.avail_out;
    inflateEnd(&z);
    if (r != Z_STREAM_END || produced != out.size())
        throw std::runtime_error("zlib block did not inflate to its declared size");
}

PfsArchive PfsArchive::open(const std::filesystem::path& path, Storage storage,
                            std::shared_ptr<SharedFileCache> cache) {
    // A direct mapping of a game-folder file is already shared between clients through the page cache, so the
    // cache only takes archives that would otherwise be read onto the heap (the server's overlay files).
    if (cache && storage == Storage::Heap) {
        auto data = cache->load(path);
        if (!data) throw std::runtime_error("cannot read " + path.string());
        return from_bytes(std::move(*data), path.filename().string());
    }
    if (storage == Storage::Mapped) {
        auto bytes = std::make_shared<Bytes>();
        try {
            bytes->mapped_file = platform::MappedFile::open(path);
        } catch (const std::exception& e) {
            // No address space for it, or a file system that cannot map: read it as before.
            log::debug("{}: not mapped ({}); reading it into memory", path.string(), e.what());
            bytes.reset();
        }
        // An empty file maps to nothing: the heap read below gives the same "not a PFS archive".
        if (bytes && bytes->mapped_file.mapped()) {
            PfsArchive a;
            a.label_ = path.filename().string();
            a.storage_ = std::move(bytes);
            a.parse_index();
            return a;
        }
    }
    auto data = read_file(path);
    if (!data) throw std::runtime_error("cannot read " + path.string());
    return from_memory(std::move(*data), path.filename().string());
}

PfsArchive PfsArchive::open(const std::filesystem::path& path, std::shared_ptr<SharedFileCache> cache) {
    return open(path, Storage::Heap, std::move(cache));
}

namespace {
// Whether `path` is inside `root`, by their components (both as GameDir builds them: the root joined with the
// file's relative path).
bool within(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (root.empty()) return false;
    const auto p = path.lexically_normal(), r = root.lexically_normal();
    auto pi = p.begin();
    for (auto ri = r.begin(); ri != r.end(); ++ri, ++pi) {
        if (ri->empty() && std::next(ri) == r.end()) break; // a trailing separator
        if (pi == p.end() || *pi != *ri) return false;
    }
    return pi != p.end();
}
} // namespace

PfsArchive::Storage PfsArchive::storage_for(const GameDir& game, const std::filesystem::path& path) {
    // The overlay first: a data folder may sit inside the game folder (<EverQuest>/hailstone/, GameDir::kOwnFolder).
    if (auto overlay = game.overlay(); overlay && within(path, *overlay)) return Storage::Heap;
    if (within(path, game.base_root())) return Storage::Mapped;
    if (auto classic = game.classic(); classic && within(path, *classic)) return Storage::Mapped;
    return Storage::Heap;
}

PfsArchive PfsArchive::from_memory(std::vector<std::byte> data, std::string label) {
    return from_bytes(ImmutableFileBytes::owned(std::move(data)), std::move(label));
}

PfsArchive PfsArchive::from_bytes(ImmutableFileBytes data, std::string label) {
    PfsArchive a;
    a.label_ = std::move(label);
    auto storage = std::make_shared<Bytes>();
    storage->immutable = std::move(data);
    a.storage_ = storage;
    a.parse_index();
    return a;
}

bool PfsArchive::mapped() const {
    return storage_ && (storage_->mapped_file.mapped() || storage_->immutable.mapped());
}

std::size_t PfsArchive::storage_bytes() const { return bytes().size(); }

std::size_t PfsArchive::owned_bytes() const { return mapped() ? 0 : bytes().size(); }

void PfsArchive::parse_index() {
    ByteReader r(bytes());
    auto dir_offset = r.le<std::uint32_t>();
    if (r.le<std::uint32_t>() != kMagic) throw std::runtime_error(label_ + ": not a PFS archive");
    r.seek(dir_offset);
    auto count = r.le<std::uint32_t>();
    if (count > r.remaining() / 12) throw std::runtime_error(label_ + ": bad PFS directory count");

    std::vector<Entry> entries(count);
    for (auto& e : entries) {
        e.crc = r.le<std::uint32_t>();
        e.offset = r.le<std::uint32_t>();
        e.size = r.le<std::uint32_t>();
    }

    auto table = std::find_if(entries.begin(), entries.end(), [](const Entry& e) { return e.crc == kNameTableCrc; });
    if (table == entries.end())
        table = std::max_element(entries.begin(), entries.end(),
                                 [](const Entry& x, const Entry& y) { return x.offset < y.offset; });
    if (table == entries.end()) throw std::runtime_error(label_ + ": PFS archive has no name table");
    Entry table_entry = *table;
    entries.erase(table);

    std::unordered_map<std::uint32_t, Entry> by_crc;
    for (const auto& e : entries) by_crc.emplace(e.crc, e);

    if (table_entry.size > kMaxNameTableSize) throw std::runtime_error(label_ + ": PFS name table is too large");
    auto table_data = inflate_entry(table_entry);
    ByteReader tr(table_data);
    auto name_count = tr.le<std::uint32_t>();
    for (std::uint32_t i = 0; i < name_count; ++i) {
        auto len = tr.le<std::uint32_t>();
        std::string key = GameDir::normalize(tr.fixed_string(len));
        auto it = by_crc.find(pfs_name_crc(key));
        if (it == by_crc.end()) continue;
        // A name listed again is one member: callers walk names(), so a repeat would parse it once per copy.
        if (by_name_.emplace(key, it->second).second) names_.push_back(std::move(key));
    }
    std::sort(names_.begin(), names_.end(), [&](const std::string& x, const std::string& y) {
        return by_name_.at(x).offset < by_name_.at(y).offset;
    });
}

bool PfsArchive::contains(std::string_view name) const { return by_name_.contains(GameDir::normalize(name)); }

std::optional<std::vector<std::byte>> PfsArchive::read(std::string_view name) const {
    auto it = by_name_.find(GameDir::normalize(name));
    if (it == by_name_.end()) return std::nullopt;
    return inflate_entry(it->second);
}

std::optional<std::vector<std::byte>> PfsArchive::read_prefix(std::string_view name, std::size_t max) const {
    auto it = by_name_.find(GameDir::normalize(name));
    if (it == by_name_.end()) return std::nullopt;
    const Entry& e = it->second;
    if (e.size > kMaxMemberSize) throw std::runtime_error(label_ + ": PFS file is larger than any game file");
    // At most the member's declared size, itself capped above: `max` cannot make this allocate more.
    std::vector<std::byte> out(std::min<std::size_t>(max, e.size));
    ByteReader r(bytes());
    r.seek(e.offset);
    std::size_t have = 0;
    for (std::uint64_t total = 0; have < out.size();) {
        auto deflated = r.le<std::uint32_t>();
        auto inflated = r.le<std::uint32_t>();
        if (inflated > e.size - total) throw std::runtime_error(label_ + ": PFS block overruns its file");
        if (inflated > std::uint64_t{deflated} * kMaxDeflateRatio)
            throw std::runtime_error(label_ + ": PFS block is larger than its compressed bytes can hold");
        const std::size_t n = std::min<std::size_t>(inflated, out.size() - have);
        inflate_prefix(r.bytes(deflated), std::span<std::byte>(out).subspan(have, n));
        have += n;
        total += inflated;
    }
    return out;
}

std::vector<std::byte> PfsArchive::inflate_entry(const Entry& e) const {
    // The block headers are walked before anything is allocated, so the size a file declares is trusted only as far
    // as its compressed bytes back it: each block expands by at most kMaxDeflateRatio, and the blocks add up to
    // exactly the file's size.
    if (e.size > kMaxMemberSize) throw std::runtime_error(label_ + ": PFS file is larger than any game file");
    ByteReader r(bytes());
    r.seek(e.offset);
    for (std::uint64_t total = 0; total < e.size;) {
        auto deflated = r.le<std::uint32_t>();
        auto inflated = r.le<std::uint32_t>();
        if (inflated > e.size - total) throw std::runtime_error(label_ + ": PFS block overruns its file");
        if (inflated > std::uint64_t{deflated} * kMaxDeflateRatio)
            throw std::runtime_error(label_ + ": PFS block is larger than its compressed bytes can hold");
        r.skip(deflated);
        total += inflated;
    }
    // Even within the ratio, ~4 MB of compressed junk can declare 4 GB, so memory follows the bytes zlib writes.
    std::vector<std::byte> out;
    out.reserve(std::min<std::size_t>(e.size, std::size_t{16} << 20));
    r.seek(e.offset);
    while (out.size() < e.size) {
        auto deflated = r.le<std::uint32_t>();
        auto inflated = r.le<std::uint32_t>();
        inflate_append(r.bytes(deflated), inflated, out);
    }
    return out;
}

} // namespace hailstone::assets
