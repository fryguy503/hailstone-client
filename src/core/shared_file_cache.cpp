#include "core/shared_file_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "core/game_dir.h"
#include "core/sha256.h"
#include "platform/process.h"
#include "platform/shared_file.h"

namespace hailstone {
namespace {

struct CacheCounters {
    std::atomic<std::uint64_t> mapped_bytes{0}, hits{0}, builds{0}, fallbacks{0}, evictions{0};
};

struct CloseSource {
    void operator()(std::FILE* file) const { (void)std::fclose(file); }
};
using SourceFile = std::unique_ptr<std::FILE, CloseSource>;

// Streaming avoids allocating a second whole archive while establishing its identity or building a snapshot.
std::optional<Sha256Digest> stream_source(std::FILE* file, std::uint64_t size,
                                        platform::CacheTempFile* destination = nullptr) {
    Sha256 hash;
    std::array<std::byte, 64 * 1024> chunk;
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, chunk.size()));
        const std::size_t got = std::fread(chunk.data(), 1, count, file);
        if (got != count) return std::nullopt;
        const std::span<const std::byte> bytes(chunk.data(), got);
        hash.update(bytes);
        if (destination && !destination->write(bytes)) return std::nullopt;
        remaining -= got;
    }
    if (std::fgetc(file) != EOF || std::ferror(file)) return std::nullopt;
    return hash.finish();
}

bool blob_name(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name.size() == 69 && name.ends_with(".blob") &&
           std::all_of(name.begin(), name.begin() + 64,
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool temporary_name(const std::filesystem::path& path) { return path.filename().string().starts_with(".tmp-"); }

struct DiskEntry {
    std::filesystem::path path;
    std::uint64_t size;
    std::filesystem::file_time_type used; // last write: set on publication and on each instance's first mapping
};

// Only cache-owned names, never recursive deletion or links.
std::optional<std::vector<DiskEntry>> disk_entries(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::directory_iterator it(directory, ec), end;
    if (ec) return std::nullopt;
    std::vector<DiskEntry> out;
    while (it != end) {
        if (blob_name(it->path()) || temporary_name(it->path())) {
            const auto status = it->symlink_status(ec);
            if (ec || !std::filesystem::is_regular_file(status)) return std::nullopt;
            const auto size = it->file_size(ec);
            if (ec) return std::nullopt;
            const auto written = it->last_write_time(ec);
            if (ec) return std::nullopt;
            out.push_back({it->path(), size, written});
        }
        it.increment(ec);
        if (ec) return std::nullopt;
    }
    return out;
}

std::optional<std::uint64_t> total_size(const std::vector<DiskEntry>& entries) {
    std::uint64_t total = 0;
    for (const auto& e : entries) {
        if (e.size > std::numeric_limits<std::uint64_t>::max() - total) return std::nullopt;
        total += e.size;
    }
    return total;
}

// A source written this recently may still change within its file system's timestamp resolution, so its digest is
// not remembered (the same rule git applies to its index).
constexpr auto kSettledSource = std::chrono::seconds(3);

} // namespace

// There is no cross-process use lock. Every writer holds publish.lock, so a temporary found under it is abandoned. A
// blob is never rewritten in place: on POSIX, removing one another client maps leaves its mapping intact, and
// Windows refuses to remove a file a client has open (ReadOnlyMapping shares only reading).
struct ImmutableFileBytes::Storage {
    std::vector<std::byte> owned;
    std::optional<platform::ReadOnlyMapping> mapping;
    std::shared_ptr<CacheCounters> counters;

    ~Storage() {
        if (mapping && counters) counters->mapped_bytes.fetch_sub(mapping->bytes().size(), std::memory_order_relaxed);
    }
};

ImmutableFileBytes ImmutableFileBytes::owned(std::vector<std::byte> bytes) {
    auto storage = std::make_shared<Storage>();
    storage->owned = std::move(bytes);
    return ImmutableFileBytes(std::move(storage));
}

std::span<const std::byte> ImmutableFileBytes::bytes() const {
    if (!storage_) return {};
    return storage_->mapping ? storage_->mapping->bytes() : std::span<const std::byte>(storage_->owned);
}

bool ImmutableFileBytes::mapped() const { return storage_ && storage_->mapping.has_value(); }

struct SharedFileCache::State {
    explicit State(Options o) : options(std::move(o)) {}
    Options options;
    std::shared_ptr<CacheCounters> counters = std::make_shared<CacheCounters>();
    std::mutex mutex; // guards everything below
    std::unordered_map<std::string, std::weak_ptr<const ImmutableFileBytes::Storage>> live;
    std::unordered_set<std::string> verified; // blobs whose content this instance has checked
    std::unordered_set<std::string> touched;  // blobs whose use time this instance has refreshed
    struct SourceDigest {
        platform::FileStamp stamp;
        Sha256Digest digest;
    };
    std::unordered_map<std::string, SourceDigest> sources;
    bool trimmed = false; // the first-use trim has run

    using Verify = std::function<bool(const std::string& key, std::span<const std::byte>)>;
    using Write = std::function<bool(platform::CacheTempFile&)>;

    std::filesystem::path publish_lock() const { return options.directory / "publish.lock"; }

    bool prepare() {
        if (options.directory.empty() || options.budget_bytes == 0 ||
            !platform::prepare_private_cache_directory(options.directory)) return false;
        bool first;
        {
            std::lock_guard guard(mutex);
            first = !trimmed;
        }
        // A smaller budget than the last session's, or blobs left by a client that crashed, are dealt with once.
        if (first) {
            if (auto writer = platform::ScopedFileLock::try_acquire(publish_lock(), platform::FileLockMode::Exclusive)) {
                (void)prune_locked(options.budget_bytes);
                std::lock_guard guard(mutex);
                trimmed = true;
            }
        }
        return true;
    }

    std::optional<ImmutableFileBytes> live_hit(const std::string& key) {
        std::lock_guard guard(mutex);
        auto it = live.find(key);
        if (it == live.end()) return std::nullopt;
        if (auto storage = it->second.lock()) {
            counters->hits.fetch_add(1, std::memory_order_relaxed);
            return ImmutableFileBytes(std::move(storage));
        }
        live.erase(it);
        return std::nullopt;
    }

    // Maps a blob already on disk. `verify` checks its content unless this instance wrote it.
    std::optional<ImmutableFileBytes> map(const std::filesystem::path& destination, const std::string& key,
                                          std::uint64_t size, const Verify* verify) {
        auto mapping = platform::ReadOnlyMapping::open(destination);
        if (!mapping || mapping->bytes().size() != size) return std::nullopt;
        if (verify && !(*verify)(key, mapping->bytes())) return std::nullopt;
        bool touch;
        {
            std::lock_guard guard(mutex);
            touch = touched.insert(key).second;
        }
        // The use time eviction orders by. A failure only makes this blob look older.
        if (touch) {
            std::error_code ec;
            std::filesystem::last_write_time(destination, std::filesystem::file_time_type::clock::now(), ec);
        }
        auto storage = std::make_shared<ImmutableFileBytes::Storage>();
        storage->mapping = std::move(mapping);
        storage->counters = counters;
        counters->mapped_bytes.fetch_add(storage->mapping->bytes().size(), std::memory_order_relaxed);
        std::lock_guard guard(mutex);
        // Workers may reach the same key together. Retain just one mapping and one byte charge locally.
        auto& slot = live[key];
        if (auto existing = slot.lock()) return ImmutableFileBytes(std::move(existing));
        slot = storage;
        return ImmutableFileBytes(std::move(storage));
    }

    std::optional<ImmutableFileBytes> hit(const std::filesystem::path& destination, const std::string& key,
                                          std::uint64_t size, const Verify& verify) {
        auto bytes = map(destination, key, size, &verify);
        if (bytes) counters->hits.fetch_add(1, std::memory_order_relaxed);
        return bytes;
    }

    // The caller holds publish.lock, so no client is writing a temporary. Blobs this instance maps are kept; on
    // Windows so is any blob another client maps (it cannot be removed while open).
    bool prune_locked(std::uint64_t target) {
        auto entries = disk_entries(options.directory);
        if (!entries) return false;
        auto total = total_size(*entries);
        if (!total) return false;
        std::unordered_set<std::string> in_use;
        {
            std::lock_guard guard(mutex);
            for (const auto& [key, storage] : live)
                if (!storage.expired()) in_use.insert(key + ".blob");
        }
        std::sort(entries->begin(), entries->end(), [](const DiskEntry& a, const DiskEntry& b) {
            const bool ta = temporary_name(a.path), tb = temporary_name(b.path);
            return ta != tb ? ta : a.used < b.used;
        });
        std::error_code ec;
        for (const auto& entry : *entries) {
            const bool temporary = temporary_name(entry.path);
            if (!temporary && *total <= target) break;
            if (!temporary && in_use.contains(entry.path.filename().string())) continue;
            if (!std::filesystem::remove(entry.path, ec) || ec) continue;
            *total -= entry.size;
            if (!temporary) {
                counters->evictions.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard guard(mutex);
                const auto key = entry.path.stem().string();
                verified.erase(key);
                touched.erase(key);
            }
        }
        return *total <= target;
    }

    // The common path of load and store: an existing blob, else one published under publish.lock within the
    // budget. nullopt sends the caller to its private fallback.
    std::optional<ImmutableFileBytes> acquire(const std::string& key, std::uint64_t size, const Verify& verify,
                                              const Write& write) {
        if (auto bytes = live_hit(key)) return bytes;
        const auto destination = options.directory / (key + ".blob");
        std::error_code ec;
        auto exists = [&] { return std::filesystem::exists(std::filesystem::symlink_status(destination, ec)); };
        // A damaged or unsafe entry is never replaced while other processes could map it.
        if (exists()) return hit(destination, key, size, verify);
        if (ec && ec != std::errc::no_such_file_or_directory) return std::nullopt;
        // A short bounded wait lets simultaneous cold launches reuse the winner; a stalled producer still cannot
        // prevent private loading.
        auto writer = platform::ScopedFileLock::try_acquire(publish_lock(), platform::FileLockMode::Exclusive);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!writer && std::chrono::steady_clock::now() < deadline) {
            if (exists()) return hit(destination, key, size, verify);
            if (ec && ec != std::errc::no_such_file_or_directory) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            writer = platform::ScopedFileLock::try_acquire(publish_lock(), platform::FileLockMode::Exclusive);
        }
        if (!writer || exists()) return hit(destination, key, size, verify);
        if (ec && ec != std::errc::no_such_file_or_directory) return std::nullopt;
        auto entries = disk_entries(options.directory);
        const auto used = entries ? total_size(*entries) : std::nullopt;
        if (!used) return std::nullopt;
        if (*used > options.budget_bytes - size && !prune_locked(options.budget_bytes - size)) return std::nullopt;
        auto temporary = platform::CacheTempFile::create(options.directory);
        if (!temporary || !write(*temporary)) return std::nullopt;
        const auto published = temporary->publish(destination);
        if (published == platform::PublishResult::Error) return std::nullopt;
        // Still under publish.lock, so no trim can remove the blob before it is mapped.
        if (published == platform::PublishResult::Exists) return hit(destination, key, size, verify);
        auto bytes = map(destination, key, size, nullptr);
        if (!bytes) return std::nullopt;
        {
            std::lock_guard guard(mutex);
            verified.insert(key);
        }
        counters->builds.fetch_add(1, std::memory_order_relaxed);
        return bytes;
    }
};

std::optional<SharedFileCache::Options> SharedFileCache::configured(std::string_view mode, int budget_mib,
                                                                    std::filesystem::path directory) {
    if (mode != "auto" || budget_mib <= 0) return std::nullopt;
    const auto mib = static_cast<std::uint64_t>(std::min(budget_mib, 16384));
    return Options{std::move(directory), mib * 1024 * 1024};
}

SharedFileCache::SharedFileCache(Options options) : state_(std::make_shared<State>(std::move(options))) {}

std::optional<ImmutableFileBytes> SharedFileCache::load(const std::filesystem::path& source) try {
    const auto state = state_;
    auto fallback = [&]() -> std::optional<ImmutableFileBytes> {
        state->counters->fallbacks.fetch_add(1, std::memory_order_relaxed);
        if (auto bytes = read_file(source)) return ImmutableFileBytes::owned(std::move(*bytes));
        return std::nullopt;
    };
    if (!state->prepare()) return fallback();
    SourceFile file(platform::open_c_file(source, "rb"));
    if (!file) return fallback();
    const auto stamp = platform::file_stamp(file.get());
    if (!stamp || stamp->size == 0 || stamp->size > state->options.budget_bytes ||
        stamp->size > std::numeric_limits<std::size_t>::max()) return fallback();
    const std::string source_key = source.string();
    std::optional<Sha256Digest> digest;
    {
        std::lock_guard guard(state->mutex);
        auto it = state->sources.find(source_key);
        if (it != state->sources.end() && it->second.stamp == *stamp) digest = it->second.digest;
    }
    if (!digest) {
        digest = stream_source(file.get(), stamp->size);
        if (!digest || platform::file_stamp(file.get()) != stamp) return fallback();
        std::error_code ec;
        const auto written = std::filesystem::last_write_time(source, ec);
        if (!ec && std::filesystem::file_time_type::clock::now() - written > kSettledSource) {
            std::lock_guard guard(state->mutex);
            state->sources[source_key] = {*stamp, *digest};
        }
    }
    const std::string key = to_hex(*digest);
    const State::Verify verify = [&](const std::string& blob, std::span<const std::byte> bytes) {
        {
            std::lock_guard guard(state->mutex);
            if (state->verified.contains(blob)) return true;
        }
        if (sha256(bytes) != *digest) return false;
        std::lock_guard guard(state->mutex);
        state->verified.insert(blob);
        return true;
    };
    const State::Write write = [&](platform::CacheTempFile& temporary) {
        if (std::fseek(file.get(), 0, SEEK_SET) != 0) return false;
        const auto captured = stream_source(file.get(), stamp->size, &temporary);
        return captured && *captured == *digest && platform::file_stamp(file.get()) == stamp;
    };
    if (auto bytes = state->acquire(key, stamp->size, verify, write)) return bytes;
    return fallback();
} catch (const std::filesystem::filesystem_error&) {
    state_->counters->fallbacks.fetch_add(1, std::memory_order_relaxed);
    if (auto bytes = read_file(source)) return ImmutableFileBytes::owned(std::move(*bytes));
    return std::nullopt;
}

ImmutableFileBytes SharedFileCache::store(std::vector<std::byte> bytes) try {
    const auto state = state_;
    auto fallback = [&]() {
        state->counters->fallbacks.fetch_add(1, std::memory_order_relaxed);
        return ImmutableFileBytes::owned(std::move(bytes));
    };
    if (bytes.empty() || bytes.size() > state->options.budget_bytes || !state->prepare()) return fallback();
    const std::string key = to_hex(sha256(bytes));
    // The caller still holds the bytes, so a comparison verifies a blob found on disk more cheaply than a hash.
    const State::Verify verify = [&](const std::string&, std::span<const std::byte> mapped) {
        return std::equal(mapped.begin(), mapped.end(), bytes.begin(), bytes.end());
    };
    const State::Write write = [&](platform::CacheTempFile& temporary) { return temporary.write(bytes); };
    if (auto result = state->acquire(key, bytes.size(), verify, write)) return std::move(*result);
    return fallback();
} catch (const std::filesystem::filesystem_error&) {
    state_->counters->fallbacks.fetch_add(1, std::memory_order_relaxed);
    return ImmutableFileBytes::owned(std::move(bytes));
}

SharedFileCache::Stats SharedFileCache::stats() const {
    const auto& c = *state_->counters;
    return {c.mapped_bytes.load(std::memory_order_relaxed), c.hits.load(std::memory_order_relaxed),
            c.builds.load(std::memory_order_relaxed), c.fallbacks.load(std::memory_order_relaxed),
            c.evictions.load(std::memory_order_relaxed)};
}

bool SharedFileCache::trim() { return trim(state_->options.budget_bytes); }

bool SharedFileCache::trim(std::uint64_t target_bytes) try {
    if (state_->options.directory.empty() ||
        !platform::prepare_private_cache_directory(state_->options.directory)) return false;
    auto writer = platform::ScopedFileLock::try_acquire(state_->publish_lock(), platform::FileLockMode::Exclusive);
    return writer && state_->prune_locked(target_bytes);
} catch (const std::filesystem::filesystem_error&) {
    return false;
}

} // namespace hailstone
