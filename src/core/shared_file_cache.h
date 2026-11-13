#pragma once

// Immutable file bytes, either privately owned or mapped from a content-addressed cache. Cache failures always
// leave the normal file reader available. Native mappings and locks live in platform/shared_file.h.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
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
    };

    explicit SharedFileCache(Options options);
    // Hashes the opened source on each acquisition; metadata alone cannot establish freshness. Thread-safe.
    std::optional<ImmutableFileBytes> load(const std::filesystem::path& source);
    // Publishes already-decoded immutable bytes by content digest. Failed caching retains the supplied bytes
    // privately; callers need not write an intermediate source file or keep a second owned payload.
    ImmutableFileBytes store(std::vector<std::byte> bytes);
    Stats stats() const;
    // Removes finished blobs and abandoned temporaries only if no client holds a cache-use lease. Nonblocking.
    bool trim();

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace hailstone
