#include "core/binary_measurement.h"

#include <array>
#include <fstream>
#include "platform/executable_path.h"

namespace hailstone {

std::optional<BinaryMeasurement> measure_binary_file(const std::filesystem::path& path, std::uint64_t maximum_bytes) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;
    const auto expected = std::filesystem::file_size(path, ec);
    if (ec || expected > maximum_bytes) return std::nullopt;
    const auto modified = std::filesystem::last_write_time(path, ec);
    if (ec) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    BinaryMeasurement result;
    std::uint64_t fnv = 14695981039346656037ull;
    Sha256 sha;
    std::array<char, 65536> bytes{};
    for (;;) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (result.byte_count > maximum_bytes || count > maximum_bytes - result.byte_count) return std::nullopt;
        result.byte_count += count;
        sha.update(std::as_bytes(std::span(bytes.data(), count)));
        for (std::size_t i = 0; i < count; ++i) {
            fnv ^= static_cast<unsigned char>(bytes[i]);
            fnv *= 1099511628211ull;
        }
        if (input.bad()) return std::nullopt;
        if (input.eof()) break;
        if (input.fail()) return std::nullopt;
    }
    if (result.byte_count != expected || std::filesystem::file_size(path, ec) != expected || ec)
        return std::nullopt;
    if (std::filesystem::last_write_time(path, ec) != modified || ec) return std::nullopt;
    result.fnv1a63 = fnv & 0x7fffffffffffffffull;
    result.sha256 = sha.finish();
    return result;
}

std::optional<BinaryMeasurement> measure_process_executable() {
    const auto path = platform::process_executable_path();
    return path ? measure_binary_file(*path) : std::nullopt;
}

} // namespace hailstone
