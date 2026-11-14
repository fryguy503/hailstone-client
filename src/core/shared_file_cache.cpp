#include "core/shared_file_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#include "core/game_dir.h"
#include "core/sha256.h"
#include "platform/process.h"
#include "platform/shared_file.h"

namespace hailstone {
namespace {

struct CacheCounters {
    std::atomic<std::uint64_t> mapped_bytes{0}, hits{0}, builds{0}, fallbacks{0};
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

struct DiskEntry {
    std::filesystem::path path;
    std::uint64_t size;
    std::filesystem::file_time_type written;
};

// Only cache-owned names, never recursive deletion or links. Callers hold the use/publish locks as appropriate.
std::optional<std::vector<DiskEntry>> disk_entries(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::directory_iterator it(directory, ec), end;
    if (ec) return std::nullopt;
    std::vector<DiskEntry> out;
    while (it != end) {
        if (blob_name(it->path()) || it->path().filename().string().starts_with(".tmp-")) {
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

} // namespace

struct ImmutableFileBytes::Storage {
    std::vector<std::byte> owned;
    // Reverse destruction order keeps the use lease until the mapping is gone.
    std::shared_ptr<platform::ScopedFileLock> lease;
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
    std::mutex mutex;
    std::unordered_map<std::string, std::weak_ptr<const ImmutableFileBytes::Storage>> live;
    std::weak_ptr<platform::ScopedFileLock> use_lease;

    std::shared_ptr<platform::ScopedFileLock> acquire_use() {
        std::lock_guard guard(mutex);
        if (auto lease = use_lease.lock()) return lease;
        auto native = platform::ScopedFileLock::try_acquire(options.directory / "use.lock",
                                                           platform::FileLockMode::Shared);
        if (!native) return {};
        auto lease = std::make_shared<platform::ScopedFileLock>(std::move(*native));
        use_lease = lease; // The idle service must not prevent exclusive cleanup.
        return lease;
    }

    bool prepare() const {
        return !options.directory.empty() && options.budget_bytes > 0 &&
               platform::prepare_private_cache_directory(options.directory);
    }

    // No client (including this one) may hold a shared use lease here. Locks themselves stay at stable paths.
    bool prune(std::uint64_t target) const {
        auto lease = platform::ScopedFileLock::try_acquire(options.directory / "use.lock",
                                                          platform::FileLockMode::Exclusive);
        if (!lease) return false;
        auto entries = disk_entries(options.directory);
        if (!entries) return false;
        auto total = total_size(*entries);
        if (!total) return false;
        std::sort(entries->begin(), entries->end(), [](const DiskEntry& a, const DiskEntry& b) {
            return a.written < b.written;
        });
        std::error_code ec;
        for (const auto& entry : *entries) {
            if (*total <= target && target != 0) break;
            if (!std::filesystem::remove(entry.path, ec) || ec) return false;
            *total -= entry.size;
        }
        // A use lease covers builders as well as mapped readers, so every temporary here is abandoned.
        std::filesystem::directory_iterator it(options.directory, ec), end;
        if (ec) return false;
        while (it != end) {
            if (it->path().filename().string().starts_with(".tmp-")) {
                const auto status = it->symlink_status(ec);
                if (ec || !std::filesystem::is_regular_file(status)) return false;
                if (!std::filesystem::remove(it->path(), ec) || ec) return false;
            }
            it.increment(ec);
            if (ec) return false;
        }
        return true;
    }
};

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
    const auto digest = stream_source(file.get(), stamp->size);
    if (!digest || platform::file_stamp(file.get()) != stamp) return fallback();
    const std::string key = to_hex(*digest);
    const auto destination = state->options.directory / (key + ".blob");
    auto lease = state->acquire_use();
    if (!lease) return fallback();
    {
        std::lock_guard guard(state->mutex);
        auto it = state->live.find(key);
        if (it != state->live.end()) {
            if (auto live = it->second.lock()) {
                state->counters->hits.fetch_add(1, std::memory_order_relaxed);
                return ImmutableFileBytes(std::move(live));
            }
            state->live.erase(it);
        }
    }
    auto map = [&]() -> std::optional<ImmutableFileBytes> {
        auto mapping = platform::ReadOnlyMapping::open(destination);
        if (!mapping || mapping->bytes().size() != stamp->size || sha256(mapping->bytes()) != *digest)
            return std::nullopt;
        auto storage = std::make_shared<ImmutableFileBytes::Storage>();
        storage->lease = std::move(lease);
        storage->mapping = std::move(mapping);
        storage->counters = state->counters;
        state->counters->mapped_bytes.fetch_add(storage->mapping->bytes().size(), std::memory_order_relaxed);
        {
            std::lock_guard guard(state->mutex);
            // Workers may reach the same key together. Retain just one mapping and one byte charge locally.
            auto& slot = state->live[key];
            if (auto live = slot.lock()) return ImmutableFileBytes(std::move(live));
            slot = storage;
        }
        return ImmutableFileBytes(std::move(storage));
    };
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::symlink_status(destination, ec))) {
        if (auto bytes = map()) {
            state->counters->hits.fetch_add(1, std::memory_order_relaxed);
            return bytes;
        }
        return fallback(); // Never replace a corrupt/unsafe entry while other processes could map it.
    }
    if (ec && ec != std::errc::no_such_file_or_directory) return fallback();

    auto entries = disk_entries(state->options.directory);
    const auto used = entries ? total_size(*entries) : std::nullopt;
    if (!used) return fallback();
    if (*used > state->options.budget_bytes - stamp->size) {
        lease.reset();
        if (!state->prune(state->options.budget_bytes - stamp->size)) return fallback();
        lease = state->acquire_use();
        if (!lease) return fallback();
    }
    // One insertion lock also serializes the disk-budget check. A short bounded wait lets simultaneous cold
    // launches reuse the winner; a stalled producer still cannot prevent private loading.
    auto writer = platform::ScopedFileLock::try_acquire(state->options.directory / "publish.lock",
                                                       platform::FileLockMode::Exclusive);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (!writer && std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(std::filesystem::symlink_status(destination, ec))) {
            if (auto bytes = map()) {
                state->counters->hits.fetch_add(1, std::memory_order_relaxed);
                return bytes;
            }
            return fallback();
        }
        if (ec && ec != std::errc::no_such_file_or_directory) return fallback();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        writer = platform::ScopedFileLock::try_acquire(state->options.directory / "publish.lock",
                                                      platform::FileLockMode::Exclusive);
    }
    if (!writer) {
        if (auto bytes = map()) {
            state->counters->hits.fetch_add(1, std::memory_order_relaxed);
            return bytes;
        }
        return fallback();
    }
    if (std::filesystem::exists(std::filesystem::symlink_status(destination, ec))) {
        if (auto bytes = map()) {
            state->counters->hits.fetch_add(1, std::memory_order_relaxed);
            return bytes;
        }
        return fallback();
    }
    if (ec && ec != std::errc::no_such_file_or_directory) return fallback();
    entries = disk_entries(state->options.directory);
    const auto current = entries ? total_size(*entries) : std::nullopt;
    if (!current || *current > state->options.budget_bytes - stamp->size) return fallback();
    auto temporary = platform::CacheTempFile::create(state->options.directory);
    if (!temporary || std::fseek(file.get(), 0, SEEK_SET) != 0) return fallback();
    const auto captured = stream_source(file.get(), stamp->size, &*temporary);
    if (!captured || *captured != *digest || platform::file_stamp(file.get()) != stamp) return fallback();
    const auto published = temporary->publish(destination);
    if (published == platform::PublishResult::Error) return fallback();
    if (auto bytes = map()) {
        if (published == platform::PublishResult::Published)
            state->counters->builds.fetch_add(1, std::memory_order_relaxed);
        else state->counters->hits.fetch_add(1, std::memory_order_relaxed);
        return bytes;
    }
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
    const auto digest = sha256(bytes);
    const std::string key = to_hex(digest);
    const auto destination = state->options.directory / (key + ".blob");
    auto lease = state->acquire_use();
    if (!lease) return fallback();
    {
        std::lock_guard guard(state->mutex);
        auto it = state->live.find(key);
        if (it != state->live.end()) {
            if (auto live = it->second.lock()) {
                state->counters->hits.fetch_add(1, std::memory_order_relaxed);
                return ImmutableFileBytes(std::move(live));
            }
            state->live.erase(it);
        }
    }
    auto map = [&]() -> std::optional<ImmutableFileBytes> {
        auto mapping = platform::ReadOnlyMapping::open(destination);
        if (!mapping || mapping->bytes().size() != bytes.size() || sha256(mapping->bytes()) != digest)
            return std::nullopt;
        auto storage = std::make_shared<ImmutableFileBytes::Storage>();
        storage->lease = std::move(lease);
        storage->mapping = std::move(mapping);
        storage->counters = state->counters;
        state->counters->mapped_bytes.fetch_add(storage->mapping->bytes().size(), std::memory_order_relaxed);
        {
            std::lock_guard guard(state->mutex);
            auto& slot = state->live[key];
            if (auto live = slot.lock()) return ImmutableFileBytes(std::move(live));
            slot = storage;
        }
        return ImmutableFileBytes(std::move(storage));
    };
    auto hit = [&]() -> std::optional<ImmutableFileBytes> {
        if (auto result = map()) {
            state->counters->hits.fetch_add(1, std::memory_order_relaxed);
            return result;
        }
        return std::nullopt;
    };
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::symlink_status(destination, ec))) {
        if (auto result = hit()) return std::move(*result);
        return fallback();
    }
    if (ec && ec != std::errc::no_such_file_or_directory) return fallback();
    auto entries = disk_entries(state->options.directory);
    auto used = entries ? total_size(*entries) : std::nullopt;
    if (!used) return fallback();
    if (*used > state->options.budget_bytes - bytes.size()) {
        lease.reset();
        if (!state->prune(state->options.budget_bytes - bytes.size())) return fallback();
        lease = state->acquire_use();
        if (!lease) return fallback();
    }
    auto writer = platform::ScopedFileLock::try_acquire(state->options.directory / "publish.lock",
                                                       platform::FileLockMode::Exclusive);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (!writer && std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(std::filesystem::symlink_status(destination, ec))) {
            if (auto result = hit()) return std::move(*result);
            return fallback();
        }
        if (ec && ec != std::errc::no_such_file_or_directory) return fallback();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        writer = platform::ScopedFileLock::try_acquire(state->options.directory / "publish.lock",
                                                      platform::FileLockMode::Exclusive);
    }
    if (!writer) {
        if (auto result = hit()) return std::move(*result);
        return fallback();
    }
    if (std::filesystem::exists(std::filesystem::symlink_status(destination, ec))) {
        if (auto result = hit()) return std::move(*result);
        return fallback();
    }
    if (ec && ec != std::errc::no_such_file_or_directory) return fallback();
    entries = disk_entries(state->options.directory);
    used = entries ? total_size(*entries) : std::nullopt;
    if (!used || *used > state->options.budget_bytes - bytes.size()) return fallback();
    auto temporary = platform::CacheTempFile::create(state->options.directory);
    if (!temporary || !temporary->write(bytes)) return fallback();
    const auto published = temporary->publish(destination);
    if (published == platform::PublishResult::Error) return fallback();
    if (auto result = map()) {
        if (published == platform::PublishResult::Published)
            state->counters->builds.fetch_add(1, std::memory_order_relaxed);
        else state->counters->hits.fetch_add(1, std::memory_order_relaxed);
        return std::move(*result);
    }
    return fallback();
} catch (const std::filesystem::filesystem_error&) {
    state_->counters->fallbacks.fetch_add(1, std::memory_order_relaxed);
    return ImmutableFileBytes::owned(std::move(bytes));
}

SharedFileCache::Stats SharedFileCache::stats() const {
    const auto& c = *state_->counters;
    return {c.mapped_bytes.load(std::memory_order_relaxed), c.hits.load(std::memory_order_relaxed),
            c.builds.load(std::memory_order_relaxed), c.fallbacks.load(std::memory_order_relaxed)};
}

bool SharedFileCache::trim() try {
    return state_->prepare() && state_->prune(0);
} catch (const std::filesystem::filesystem_error&) {
    return false;
}

} // namespace hailstone
