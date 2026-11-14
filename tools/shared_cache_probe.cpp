// Holds immutable file payloads for cache subprocess tests and multi-client memory measurements.
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/game_dir.h"
#include "core/shared_file_cache.h"
#include "platform/process.h"
#include "platform/shared_file.h"

using namespace hailstone;

namespace {
void wait_for_input_end() {
    while (std::getchar() != EOF) {}
}

int abandon(const std::filesystem::path& directory) {
    if (!platform::prepare_private_cache_directory(directory)) return 1;
    auto writer = platform::ScopedFileLock::try_acquire(directory / "publish.lock", platform::FileLockMode::Exclusive);
    auto temporary = platform::CacheTempFile::create(directory);
    const std::string_view partial = "unpublished partial cache payload";
    if (!writer || !temporary || !temporary->write(std::as_bytes(std::span(partial.data(), partial.size()))))
        return 1;
    std::puts("prepared");
    std::fflush(stdout);
    wait_for_input_end();
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--abandon") return abandon(argv[2]);
        std::filesystem::path directory;
        std::uint64_t budget = std::uint64_t{1} << 30;
        bool private_bytes = false, gate = false, hold = false;
        std::vector<std::filesystem::path> sources;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--cache" && i + 1 < argc) directory = argv[++i];
            else if (arg == "--budget" && i + 1 < argc) {
                const std::string_view number(argv[++i]);
                const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), budget);
                if (error != std::errc{} || end != number.data() + number.size()) return 2;
            } else if (arg == "--private") private_bytes = true;
            else if (arg == "--gate") gate = true;
            else if (arg == "--hold") hold = true;
            else if (arg.starts_with("--")) return 2;
            else sources.emplace_back(argv[i]);
        }
        if (sources.empty() || (!private_bytes && directory.empty())) {
            std::fputs("usage: hailstone_shared_cache_probe --cache DIR [--budget BYTES] [--private] [--gate] [--hold] SOURCE...\n",
                       stderr);
            return 2;
        }
        if (gate) {
            std::puts("waiting");
            std::fflush(stdout);
            if (std::getchar() == EOF) return 1;
        }
        SharedFileCache cache({directory, budget});
        std::vector<ImmutableFileBytes> retained;
        std::uint64_t bytes = 0, mapped = 0, checksum = 0;
        for (const auto& source : sources) {
            std::optional<ImmutableFileBytes> loaded;
            if (private_bytes) {
                if (auto file = read_file(source)) loaded = ImmutableFileBytes::owned(std::move(*file));
            } else loaded = cache.load(source);
            if (!loaded) return 1;
            if (loaded->bytes().size() > std::numeric_limits<std::uint64_t>::max() - bytes) return 1;
            bytes += loaded->bytes().size();
            if (loaded->mapped()) mapped += loaded->bytes().size();
            // Visit every byte: merely mapping reserves address space without making its payload resident.
            for (const auto value : loaded->bytes()) checksum += static_cast<unsigned char>(value);
            retained.push_back(std::move(*loaded));
        }
        const auto stats = cache.stats();
        std::printf("ready bytes=%llu mapped=%llu hits=%llu builds=%llu fallbacks=%llu checksum=%llu pid=%lu\n",
                    static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(mapped),
                    static_cast<unsigned long long>(stats.hits), static_cast<unsigned long long>(stats.builds),
                    static_cast<unsigned long long>(stats.fallbacks), static_cast<unsigned long long>(checksum),
                    platform::process_id());
        std::fflush(stdout);
        // With --hold, each 'c' on the input reads every retained byte again (after another client trimmed the
        // cache, say) and reports the checksum.
        for (int c = hold ? std::getchar() : EOF; c != EOF; c = std::getchar()) {
            if (c != 'c') continue;
            std::uint64_t again = 0;
            for (const auto& payload : retained)
                for (const auto value : payload.bytes()) again += static_cast<unsigned char>(value);
            std::printf("recheck checksum=%llu\n", static_cast<unsigned long long>(again));
            std::fflush(stdout);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "shared cache probe: %s\n", error.what());
        return 1;
    }
}
