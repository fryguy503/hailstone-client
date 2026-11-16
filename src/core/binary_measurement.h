#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include "core/sha256.h"

namespace hailstone {

struct BinaryMeasurement {
    std::uint64_t byte_count = 0;
    std::uint64_t fnv1a63 = 0;
    Sha256Digest sha256{};
};

// File bytes only, with bounded streaming storage. Rejects read errors and observed size/mtime changes.
std::optional<BinaryMeasurement> measure_binary_file(const std::filesystem::path&,
                                                     std::uint64_t maximum_bytes = 4ull * 1024 * 1024 * 1024);
// The current process executable, resolved by the platform; never the asset folder's legacy client executable.
std::optional<BinaryMeasurement> measure_process_executable();

} // namespace hailstone
