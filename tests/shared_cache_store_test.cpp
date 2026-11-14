#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/shared_file_cache.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {
std::vector<std::byte> payload(std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return {bytes.begin(), bytes.end()};
}

std::string contents(const ImmutableFileBytes& bytes) {
    const auto data = bytes.bytes();
    if (data.empty()) return {};
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}
} // namespace

TEST_CASE("shared cache store: decoded bytes share content identity with file snapshots") {
    test::ScratchDir dir("shared_store_identity");
    const auto root = std::filesystem::canonical(dir.path());
    const auto source = root / "source.bin";
    {
        std::ofstream out(source, std::ios::binary);
        out << "decoded immutable payload";
        REQUIRE(static_cast<bool>(out));
    }
    SharedFileCache cache({root / "cache", 1024});
    auto stored = cache.store(payload("decoded immutable payload"));
    REQUIRE(stored.mapped());
    CHECK(contents(stored) == "decoded immutable payload");
    auto again = cache.store(payload("decoded immutable payload"));
    REQUIRE(again.mapped());
    CHECK(again.bytes().data() == stored.bytes().data());
    auto loaded = cache.load(source);
    REQUIRE(loaded);
    CHECK(loaded->bytes().data() == stored.bytes().data());
    CHECK(cache.stats().builds == 1);
    CHECK(cache.stats().hits == 2);
    CHECK(cache.stats().mapped_bytes == stored.bytes().size());
    CHECK_FALSE(cache.trim(0)); // mapped here, so kept

    auto changed = cache.store(payload("decoded immutable payloaD"));
    REQUIRE(changed.mapped());
    CHECK(contents(stored) == "decoded immutable payload");
    CHECK(contents(changed) == "decoded immutable payloaD");
    CHECK(cache.stats().builds == 2);
}

TEST_CASE("shared cache store: unavailable and undersized caches preserve decoded payloads privately") {
    test::ScratchDir dir("shared_store_fallback");
    const auto root = std::filesystem::canonical(dir.path());
    SharedFileCache disabled({root / "disabled", 0});
    auto original = disabled.store(payload("keep decoded bytes"));
    CHECK_FALSE(original.mapped());
    CHECK(contents(original) == "keep decoded bytes");
    CHECK(disabled.stats().fallbacks == 1);
    CHECK_FALSE(std::filesystem::exists(root / "disabled"));

    SharedFileCache small({root / "small", 4});
    auto too_big = small.store(payload("larger than disk budget"));
    CHECK_FALSE(too_big.mapped());
    CHECK(contents(too_big) == "larger than disk budget");
    auto empty = small.store({});
    CHECK_FALSE(empty.mapped());
    CHECK(empty.bytes().empty());
    CHECK(small.stats().fallbacks == 2);
}

TEST_CASE("shared cache store: a decoded mapping outlives the cache service") {
    test::ScratchDir dir("shared_store_lifetime");
    const auto cache_path = std::filesystem::canonical(dir.path()) / "cache";
    std::optional<ImmutableFileBytes> survivor;
    {
        SharedFileCache first({cache_path, 1024});
        survivor = first.store(payload("reader survives cache service"));
        REQUIRE(survivor->mapped());
    }
    SharedFileCache second({cache_path, 1024});
    CHECK(contents(*survivor) == "reader survives cache service");
    auto warm = second.store(payload("reader survives cache service"));
    REQUIRE(warm.mapped());
    CHECK(contents(warm) == contents(*survivor));
    CHECK(second.stats().hits == 1);
    survivor.reset();
    CHECK_FALSE(second.trim(0));
    warm = {};
    CHECK(second.trim(0));
}

TEST_CASE("shared cache store: corrupt existing snapshots are never replaced under a content key") {
    test::ScratchDir dir("shared_store_corrupt");
    const auto cache_path = std::filesystem::canonical(dir.path()) / "cache";
    SharedFileCache cache({cache_path, 1024});
    {
        auto first = cache.store(payload("immutable"));
        REQUIRE(first.mapped());
    }
    std::filesystem::path blob;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path))
        if (entry.path().extension() == ".blob") blob = entry.path();
    REQUIRE_FALSE(blob.empty());
    // The same instance compares a found blob with the bytes it was given, so even one it wrote is checked.
    std::filesystem::permissions(blob, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    {
        std::ofstream corrupt(blob, std::ios::binary | std::ios::trunc);
        corrupt << "corrupted";
        REQUIRE(static_cast<bool>(corrupt));
    }
    auto retained = cache.store(payload("immutable"));
    CHECK_FALSE(retained.mapped());
    CHECK(contents(retained) == "immutable");
    std::ifstream read_back(blob, std::ios::binary);
    std::string disk(9, '\0');
    read_back.read(disk.data(), static_cast<std::streamsize>(disk.size()));
    CHECK(disk == "corrupted");
}
