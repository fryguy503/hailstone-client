#pragma once

// Immutable file bytes, either privately owned or mapped from a content-addressed cache that several clients on one
// machine share (assets.shared_memory, docs/layout.md "Shared archive memory"). Cache failures always leave the
// normal file reader available. Native mappings and locks live in platform/shared_file.h.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace hailstone {

class ImmutableFileBytes {
public:
    ImmutableFileBytes() = default;
    static ImmutableFileBytes owned(std::vector<std::byte> bytes);
    std::span<const std::byte> bytes() const;
    bool mapped() const;

private:
    struct Storage;
    explicit ImmutableFileBytes(std::shared_ptr<const Storage> storage) : storage_(std::move(storage)) {}
    std::shared_ptr<const Storage> storage_;
    friend class SharedFileCache;
};

class SharedFileCache {
public:
    struct Options {
        std::filesystem::path directory;
        std::uint64_t budget_bytes = std::uint64_t{1} << 30;
    };
    struct Stats {
        std::uint64_t mapped_bytes = 0; // live mapped backing in this cache instance; not measured physical residency
        std::uint64_t hits = 0;
        std::uint64_t builds = 0;
        std::uint64_t fallbacks = 0;
        std::uint64_t evictions = 0; // blobs this instance removed to keep the disk budget
    };

    // The options hailstone.ini asks for: none (the cache is off) unless `mode` is "auto" and the budget is above 0.
    static std::optional<Options> configured(std::string_view mode, int budget_mib, std::filesystem::path directory);

    explicit SharedFileCache(Options options);
    // A source file's snapshot. The source is hashed once per cache instance while its size, identity and
    // timestamps stay the same, and a blob found on disk is hashed once per instance before it is trusted.
    // Thread-safe.
    std::optional<ImmutableFileBytes> load(const std::filesystem::path& source);
    // Publishes already-decoded immutable bytes by content digest; a blob found on disk is compared with them.
    // Failed caching returns the supplied bytes privately.
    ImmutableFileBytes store(std::vector<std::byte> bytes);
    Stats stats() const;
    // Removes abandoned temporaries, then evicts the least recently used blobs this instance has not mapped until
    // the cache holds at most `target_bytes` (default: the budget). A blob another client maps stays on Windows; on
    // POSIX its readers keep their mapping and only the name goes. Nonblocking: false when another client is
    // publishing, or when the target cannot be reached. Runs by itself on first use and whenever an insertion
    // would exceed the budget.
    bool trim();
    bool trim(std::uint64_t target_bytes);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace hailstone
