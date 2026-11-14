#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/shared_file_cache.h"
#include "platform/helper_process.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {
void write_source(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(static_cast<bool>(out));
}

std::string text(const ImmutableFileBytes& bytes) {
    const auto view = bytes.bytes();
    return {reinterpret_cast<const char*>(view.data()), view.size()};
}

std::vector<fs::path> cache_files(const fs::path& directory, std::string_view extension) {
    std::vector<fs::path> out;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().extension().string() == extension) out.push_back(entry.path());
    return out;
}

// Published blobs are read-only (0400 on POSIX); a test that damages one makes it writable first.
void damage(const fs::path& blob, std::string_view text) {
    fs::permissions(blob, fs::perms::owner_write, fs::perm_options::add);
    write_source(blob, text);
}

fs::path cache_path(const test::ScratchDir& dir) {
    return fs::canonical(dir.path()) / "cache";
}

// The deadline bounds a crashed or silent helper; readiness itself is an explicit pipe handshake, not a delay.
bool read_until(platform::HelperProcess& process, std::string& output, std::string_view marker) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::array<std::byte, 4096> buffer;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto n = process.read(buffer);
        if (n != 0) output.append(reinterpret_cast<const char*>(buffer.data()), n);
        std::erase(output, '\r'); // the helper's C stdout uses CRLF on Windows
        if (output.find(marker) != std::string::npos) return true;
        if (!process.running()) {
            const auto remaining = process.read(buffer);
            if (remaining != 0) output.append(reinterpret_cast<const char*>(buffer.data()), remaining);
            std::erase(output, '\r');
            return output.find(marker) != std::string::npos;
        }
        std::this_thread::yield();
    }
    return false;
}

void release_gate(platform::HelperProcess& process) {
    const std::array<std::byte, 1> go{std::byte{'g'}};
    process.write(go);
}
} // namespace

TEST_CASE("shared cache: cold publication and warm reads retain one immutable mapping locally") {
    test::ScratchDir dir("shared_cache_warm");
    const auto source = dir.path() / "asset.eqg";
    write_source(source, "immutable source payload");
    SharedFileCache cache({cache_path(dir), 1024});
    auto first = cache.load(source);
    REQUIRE(first);
    REQUIRE(first->mapped());
    CHECK(text(*first) == "immutable source payload");
    CHECK(cache.stats().builds == 1);
    CHECK(cache.stats().mapped_bytes == first->bytes().size());
    auto second = cache.load(source);
    REQUIRE(second);
    CHECK(second->mapped());
    CHECK(second->bytes().data() == first->bytes().data());
    CHECK(cache.stats().hits == 1);
    CHECK(cache.stats().fallbacks == 0);
    CHECK(cache.stats().mapped_bytes == first->bytes().size());
    first.reset();
    CHECK(cache.stats().mapped_bytes == second->bytes().size());
    CHECK_FALSE(cache.trim(0)); // a blob this client maps is never evicted
    CHECK(cache_files(cache_path(dir), ".blob").size() == 1);
    second.reset();
    CHECK(cache.stats().mapped_bytes == 0);
    auto warm = cache.load(source);
    REQUIRE(warm);
    CHECK(warm->mapped());
    CHECK(cache.stats().hits == 2);
    CHECK(cache.stats().builds == 1);
    warm.reset();
    CHECK(cache.trim(0));
    CHECK(cache.stats().evictions == 1);
    CHECK(cache_files(cache_path(dir), ".blob").empty());
}

TEST_CASE("shared cache: payloads outlive the cache and another instance's trim") {
    test::ScratchDir dir("shared_cache_lifetime");
    const auto source = dir.path() / "source";
    write_source(source, "kept by the last view");
    std::optional<ImmutableFileBytes> retained;
    {
        SharedFileCache cache({cache_path(dir), 1024});
        retained = cache.load(source);
        REQUIRE(retained);
        REQUIRE(retained->mapped());
    }
    SharedFileCache other({cache_path(dir), 1024});
    auto copy = *retained;
    retained.reset();
    CHECK(text(copy) == "kept by the last view");
    // Another client cannot know this mapping. On POSIX its trim removes only the name and the mapping stays
    // readable; Windows refuses to remove a mapped file.
    (void)other.trim(0);
    CHECK(text(copy) == "kept by the last view");
    copy = {};
    CHECK(other.trim(0));
    CHECK(cache_files(cache_path(dir), ".blob").empty());
}

TEST_CASE("shared cache: source content selects snapshots across names layers and unchanged metadata") {
    test::ScratchDir dir("shared_cache_identity");
    fs::create_directories(dir.path() / "base");
    fs::create_directories(dir.path() / "server");
    const auto base = dir.path() / "base" / "same.eqg";
    const auto overlay = dir.path() / "server" / "same.eqg";
    write_source(base, "base-111");
    write_source(overlay, "layer222");
    SharedFileCache cache({cache_path(dir), 1024});
    auto a = cache.load(base);
    auto b = cache.load(overlay);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->mapped());
    CHECK(b->mapped());
    CHECK(text(*a) == "base-111");
    CHECK(text(*b) == "layer222");
    CHECK(cache_files(cache_path(dir), ".blob").size() == 2);
    const auto stamp = fs::last_write_time(base);
    write_source(base, "edit-333");
    fs::last_write_time(base, stamp);
    auto edited = cache.load(base);
    REQUIRE(edited);
    CHECK(edited->mapped());
    CHECK(text(*edited) == "edit-333");
    CHECK(text(*a) == "base-111");
    CHECK(cache.stats().builds == 3);
    write_source(overlay, "edit-333");
    auto identical = cache.load(overlay);
    REQUIRE(identical);
    CHECK(identical->bytes().data() == edited->bytes().data());
    CHECK(cache.stats().hits == 1);
    CHECK(cache_files(cache_path(dir), ".blob").size() == 3);
}

TEST_CASE("shared cache: a damaged entry falls back privately until a trim removes it") {
    test::ScratchDir dir("shared_cache_corrupt");
    const auto source = dir.path() / "asset";
    write_source(source, "correct bytes");
    SharedFileCache cache({cache_path(dir), 1024});
    auto initial = cache.load(source);
    REQUIRE(initial);
    REQUIRE(initial->mapped());
    initial.reset();
    const auto files = cache_files(cache_path(dir), ".blob");
    REQUIRE(files.size() == 1);
    damage(files.front(), "corrupt bytes"); // same length: the payload hash, not just size, must reject it
    // An instance trusts a blob it has checked or written itself; the next client to start checks it again.
    SharedFileCache next({cache_path(dir), 1024});
    auto fallback = next.load(source);
    REQUIRE(fallback);
    CHECK_FALSE(fallback->mapped());
    CHECK(text(*fallback) == "correct bytes");
    CHECK(next.stats().fallbacks == 1);
    write_source(cache_path(dir) / "unrelated.txt", "preserve");
    REQUIRE(next.trim(0));
    CHECK(fs::exists(cache_path(dir) / "unrelated.txt"));
    auto rebuilt = next.load(source);
    REQUIRE(rebuilt);
    CHECK(rebuilt->mapped());
    CHECK(text(*rebuilt) == "correct bytes");
    CHECK(next.stats().builds == 1);
    CHECK(text(*fallback) == "correct bytes");
}

TEST_CASE("shared cache: disabled unavailable oversized and empty inputs preserve private loading") {
    test::ScratchDir dir("shared_cache_fallback");
    const auto source = dir.path() / "source";
    write_source(source, "source bytes");
    SUBCASE("zero disk budget disables the cache") {
        SharedFileCache cache({cache_path(dir), 0});
        auto loaded = cache.load(source);
        REQUIRE(loaded);
        CHECK_FALSE(loaded->mapped());
        CHECK(text(*loaded) == "source bytes");
        CHECK(cache.stats().fallbacks == 1);
        CHECK_FALSE(fs::exists(cache_path(dir)));
    }
    SUBCASE("a file cannot be used as the cache directory") {
        const auto blocked = dir.path() / "blocked";
        write_source(blocked, "untouched");
        SharedFileCache cache({blocked, 1024});
        auto loaded = cache.load(source);
        REQUIRE(loaded);
        CHECK_FALSE(loaded->mapped());
        CHECK(text(*loaded) == "source bytes");
        CHECK(fs::file_size(blocked) == 9);
    }
    SUBCASE("a single payload larger than the budget is private") {
        SharedFileCache cache({cache_path(dir), 4});
        auto loaded = cache.load(source);
        REQUIRE(loaded);
        CHECK_FALSE(loaded->mapped());
        CHECK(text(*loaded) == "source bytes");
        CHECK(cache_files(cache_path(dir), ".blob").empty());
    }
    SUBCASE("empty and missing sources retain normal read semantics") {
        write_source(source, "");
        SharedFileCache cache({cache_path(dir), 1024});
        auto empty = cache.load(source);
        REQUIRE(empty);
        CHECK(empty->bytes().empty());
        CHECK_FALSE(empty->mapped());
        CHECK_FALSE(cache.load(dir.path() / "missing"));
    }
}

TEST_CASE("shared cache: the disk budget keeps blobs this client maps and evicts the others") {
    test::ScratchDir dir("shared_cache_budget");
    const auto first_source = dir.path() / "first";
    const auto second_source = dir.path() / "second";
    write_source(first_source, "11111111");
    write_source(second_source, "22222222");
    SharedFileCache cache({cache_path(dir), 12});
    auto held = cache.load(first_source);
    REQUIRE(held);
    REQUIRE(held->mapped());
    auto busy = cache.load(second_source);
    REQUIRE(busy);
    CHECK_FALSE(busy->mapped());
    CHECK(text(*held) == "11111111");
    CHECK(text(*busy) == "22222222");
    CHECK(cache_files(cache_path(dir), ".blob").size() == 1);
    held.reset();
    auto idle = cache.load(second_source);
    REQUIRE(idle);
    CHECK(idle->mapped());
    CHECK(text(*idle) == "22222222");
    const auto files = cache_files(cache_path(dir), ".blob");
    REQUIRE(files.size() == 1);
    CHECK(fs::file_size(files.front()) == 8);
    CHECK(cache.stats().evictions == 1);
}

TEST_CASE("shared cache: simultaneous worker acquisitions converge on one mapping") {
    test::ScratchDir dir("shared_cache_workers");
    const auto source = dir.path() / "source";
    const std::string payload(128 * 1024, 'w');
    write_source(source, payload);
    SharedFileCache cache({cache_path(dir), 1024 * 1024});
    constexpr std::size_t workers = 6;
    std::array<std::optional<ImmutableFileBytes>, workers> loaded;
    std::barrier gate(static_cast<std::ptrdiff_t>(workers));
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < workers; ++i)
        threads.emplace_back([&, i] { gate.arrive_and_wait(); loaded[i] = cache.load(source); });
    for (auto& worker : threads) worker.join();
    const std::byte* mapped_data = nullptr;
    for (const auto& value : loaded) {
        REQUIRE(value);
        CHECK(text(*value) == payload);
        if (value->mapped()) {
            if (mapped_data) CHECK(value->bytes().data() == mapped_data);
            else mapped_data = value->bytes().data();
        }
    }
    REQUIRE(mapped_data != nullptr);
    const auto cold = cache.stats();
    CHECK(cold.builds == 1);
    CHECK(cold.hits + cold.fallbacks == workers - 1);
    CHECK(cold.mapped_bytes == payload.size());
    CHECK(cache_files(cache_path(dir), ".blob").size() == 1);
    // Cold readers may use the documented private fallback if publication exceeds its bounded wait. Once the
    // snapshot exists, every warm acquisition must share it, including readers that previously fell back.
    std::array<std::optional<ImmutableFileBytes>, workers> warm;
    for (auto& value : warm) {
        value = cache.load(source);
        REQUIRE(value);
        CHECK(value->mapped());
        CHECK(text(*value) == payload);
        CHECK(value->bytes().data() == mapped_data);
    }
    CHECK(cache.stats().builds == 1);
    CHECK(cache.stats().hits == cold.hits + workers);
    CHECK(cache.stats().mapped_bytes == payload.size());
    CHECK(cache.stats().fallbacks == cold.fallbacks);
    CHECK(cache_files(cache_path(dir), ".blob").size() == 1);
}

TEST_CASE("shared cache: many distinct retained archives share one cache lease without exhausting file handles") {
    test::ScratchDir dir("shared_cache_many_archives");
    SharedFileCache cache({cache_path(dir), 1024 * 1024});
    constexpr std::size_t count = 160;
    std::vector<ImmutableFileBytes> retained;
    retained.reserve(count);
    std::uint64_t expected_bytes = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto source = dir.path() / ("archive-" + std::to_string(i) + ".eqg");
        const auto payload = "distinct retained archive " + std::to_string(i);
        write_source(source, payload);
        auto loaded = cache.load(source);
        REQUIRE(loaded);
        REQUIRE(loaded->mapped());
        expected_bytes += payload.size();
        retained.push_back(std::move(*loaded));
    }
    for (std::size_t i = 0; i < count; ++i)
        CHECK(text(retained[i]) == "distinct retained archive " + std::to_string(i));
    CHECK(cache.stats().mapped_bytes == expected_bytes);
    CHECK(cache.stats().builds == count);
    CHECK(cache.stats().hits == 0);
    CHECK(cache.stats().fallbacks == 0);
    CHECK(cache_files(cache_path(dir), ".blob").size() == count);
    CHECK_FALSE(cache.trim(0));
    CHECK(cache_files(cache_path(dir), ".blob").size() == count);
    retained.clear();
    CHECK(cache.stats().mapped_bytes == 0);
    CHECK(cache.trim(0));
    CHECK(cache_files(cache_path(dir), ".blob").empty());
}

TEST_CASE("shared cache: abandoned temporary files count against the disk budget and go at the next insertion") {
    test::ScratchDir dir("shared_cache_orphan_budget");
    const auto first_source = dir.path() / "first";
    const auto second_source = dir.path() / "second";
    write_source(first_source, "11111111");
    write_source(second_source, "22222222");
    SharedFileCache cache({cache_path(dir), 16});
    auto held = cache.load(first_source);
    REQUIRE(held);
    REQUIRE(held->mapped());
    // A client that died while publishing; every writer holds publish.lock, so whoever holds it next may remove it.
    const auto orphan = cache_path(dir) / ".tmp-abandoned";
    write_source(orphan, "orphan-99");
    auto second = cache.load(second_source);
    REQUIRE(second);
    CHECK(second->mapped());
    CHECK(text(*second) == "22222222");
    CHECK(text(*held) == "11111111");
    CHECK_FALSE(fs::exists(orphan));
    CHECK(cache_files(cache_path(dir), ".blob").size() == 2);
    CHECK(cache.stats().evictions == 0);
}

TEST_CASE("shared cache: simultaneous client processes publish and retain the same snapshot") {
    test::ScratchDir dir("shared_cache_processes");
    const auto source = dir.path() / "source";
    const std::string payload(64 * 1024, 'p');
    write_source(source, payload);
    constexpr std::size_t count = 4;
    std::array<std::unique_ptr<platform::HelperProcess>, count> clients;
    std::array<std::string, count> output;
    std::string error;
    for (std::size_t i = 0; i < count; ++i) {
        clients[i] = std::make_unique<platform::HelperProcess>();
        REQUIRE_MESSAGE(clients[i]->start({HAILSTONE_SHARED_CACHE_PROBE, "--cache", cache_path(dir).string(),
                                          "--gate", "--hold", source.string()}, error), error);
    }
    for (std::size_t i = 0; i < count; ++i) REQUIRE(read_until(*clients[i], output[i], "waiting\n"));
    for (auto& client : clients) release_gate(*client);
    std::size_t builders = 0;
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE_MESSAGE(read_until(*clients[i], output[i], " pid="), output[i]);
        const bool mapped = output[i].find("bytes=65536 mapped=65536 ") != std::string::npos;
        const bool fallback = output[i].find("bytes=65536 mapped=0 ") != std::string::npos;
        CHECK((mapped || fallback));
        if (mapped) {
            CHECK(output[i].find("fallbacks=0") != std::string::npos);
            CHECK((output[i].find("hits=0 builds=1 ") != std::string::npos ||
                   output[i].find("hits=1 builds=0 ") != std::string::npos));
        } else {
            CHECK(output[i].find("hits=0 builds=0 fallbacks=1 ") != std::string::npos);
        }
        CHECK(output[i].find("checksum=7340032") != std::string::npos);
        if (output[i].find("builds=1") != std::string::npos) ++builders;
        CHECK(clients[i]->running());
    }
    CHECK(builders == 1);
    CHECK(cache_files(cache_path(dir), ".blob").size() == 1);
    for (auto& client : clients) {
        client->stop(false);
        REQUIRE(client->wait_exit(5000));
        CHECK(client->exit_code() == 0);
        client.reset();
    }
    // Test shared-reader lifetime with warm clients, for which publication latency cannot require a fallback.
    for (std::size_t i = 0; i < count; ++i) {
        output[i].clear();
        clients[i] = std::make_unique<platform::HelperProcess>();
        REQUIRE_MESSAGE(clients[i]->start({HAILSTONE_SHARED_CACHE_PROBE, "--cache", cache_path(dir).string(),
                                          "--hold", source.string()}, error), error);
    }
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE_MESSAGE(read_until(*clients[i], output[i], " pid="), output[i]);
        CHECK(output[i].find("bytes=65536 mapped=65536 hits=1 builds=0 fallbacks=0 ") != std::string::npos);
        CHECK(output[i].find("checksum=7340032") != std::string::npos);
        CHECK(clients[i]->running());
    }
    CHECK(cache_files(cache_path(dir), ".blob").size() == 1);
    clients.front()->stop(true); // another client closing cannot invalidate the remaining readers
    REQUIRE(clients.front()->wait_exit(5000));
    // Nor can a trim by a client that does not map the blob: its readers keep their bytes.
    SharedFileCache cache({cache_path(dir), 1024 * 1024});
    (void)cache.trim(0);
    const std::array<std::byte, 1> recheck{std::byte{'c'}};
    for (std::size_t i = 1; i < count; ++i) {
        CHECK(clients[i]->running());
        clients[i]->write(recheck);
        REQUIRE_MESSAGE(read_until(*clients[i], output[i], "recheck checksum=7340032\n"), output[i]);
        clients[i]->stop(false);
        REQUIRE(clients[i]->wait_exit(5000));
        CHECK(clients[i]->exit_code() == 0);
    }
    CHECK(cache.trim(0));
    CHECK(cache_files(cache_path(dir), ".blob").empty());
}

TEST_CASE("shared cache: a killed producer releases locks and its incomplete payload is never served") {
    test::ScratchDir dir("shared_cache_producer_death");
    const auto source = dir.path() / "source";
    write_source(source, "valid source after producer death");
    platform::HelperProcess producer;
    std::string error, output;
    REQUIRE_MESSAGE(producer.start({HAILSTONE_SHARED_CACHE_PROBE, "--abandon", cache_path(dir).string()}, error), error);
    REQUIRE_MESSAGE(read_until(producer, output, "prepared\n"), output);
    SharedFileCache cache({cache_path(dir), 1024});
    CHECK_FALSE(cache.trim());
    auto blocked = cache.load(source);
    REQUIRE(blocked);
    CHECK_FALSE(blocked->mapped());
    CHECK(text(*blocked) == "valid source after producer death");
    producer.stop(true);
    REQUIRE(producer.wait_exit(5000));
    auto recovered = cache.load(source);
    REQUIRE(recovered);
    CHECK(recovered->mapped());
    CHECK(text(*recovered) == "valid source after producer death");
    CHECK(cache_files(cache_path(dir), ".blob").size() == 1);
    recovered.reset();
    CHECK(cache.trim());
    for (const auto& entry : fs::directory_iterator(cache_path(dir)))
        CHECK_FALSE(entry.path().filename().string().starts_with(".tmp-"));
    CHECK(text(*blocked) == "valid source after producer death");
}

TEST_CASE("shared cache: eviction follows the last use, and a smaller budget applies at the next start") {
    test::ScratchDir dir("shared_cache_lru");
    std::array<fs::path, 4> sources;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        sources[i] = dir.path() / ("source-" + std::to_string(i));
        write_source(sources[i], std::string(8, static_cast<char>('a' + i)));
    }
    std::array<fs::path, 3> blobs;
    {
        SharedFileCache first({cache_path(dir), 24});
        for (std::size_t i = 0; i < blobs.size(); ++i) REQUIRE(first.load(sources[i])->mapped());
        auto files = cache_files(cache_path(dir), ".blob");
        REQUIRE(files.size() == 3);
        // Name each blob by its content, and age them in build order.
        const auto now = fs::file_time_type::clock::now();
        for (const auto& file : files) {
            std::ifstream in(file, std::ios::binary);
            const auto i = static_cast<std::size_t>(in.get() - 'a');
            REQUIRE(i < blobs.size());
            blobs[i] = file;
            fs::last_write_time(file, now - std::chrono::hours(3 - static_cast<int>(i)));
        }
    }
    SharedFileCache second({cache_path(dir), 24});
    // Using the oldest blob makes it the newest; the next insertion then evicts the second-oldest instead.
    REQUIRE(second.load(sources[0])->mapped());
    auto fourth = second.load(sources[3]);
    REQUIRE(fourth);
    CHECK(fourth->mapped());
    CHECK(fs::exists(blobs[0]));
    CHECK_FALSE(fs::exists(blobs[1]));
    CHECK(fs::exists(blobs[2]));
    CHECK(second.stats().evictions == 1);
    fourth.reset();
    // A client started with a smaller budget trims to it on first use, keeping the most recently used.
    SharedFileCache smaller({cache_path(dir), 16});
    REQUIRE(smaller.load(sources[0])->mapped());
    auto files = cache_files(cache_path(dir), ".blob");
    CHECK(files.size() == 2);
    CHECK(fs::exists(blobs[0]));
    CHECK_FALSE(fs::exists(blobs[2]));
}

TEST_CASE("shared cache: an edited source is hashed again even with its old modification time") {
    test::ScratchDir dir("shared_cache_settled");
    const auto source = dir.path() / "overlay.eqg";
    write_source(source, "first-11");
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(1);
    fs::last_write_time(source, old); // settled, so its digest is remembered
    SharedFileCache cache({cache_path(dir), 1024});
    auto a = cache.load(source);
    REQUIRE(a);
    auto again = cache.load(source);
    REQUIRE(again);
    CHECK(again->bytes().data() == a->bytes().data());
    write_source(source, "second22");
    fs::last_write_time(source, old);
    auto b = cache.load(source);
    REQUIRE(b);
    CHECK(b->mapped());
    CHECK(text(*b) == "second22");
    CHECK(text(*a) == "first-11");
    CHECK(cache.stats().builds == 2);
}

#if !defined(_WIN32)
TEST_CASE("shared cache: published blobs are read-only") {
    test::ScratchDir dir("shared_cache_readonly");
    const auto source = dir.path() / "source";
    write_source(source, "read only payload");
    SharedFileCache cache({cache_path(dir), 1024});
    REQUIRE(cache.load(source)->mapped());
    REQUIRE(cache.store({std::byte{1}, std::byte{2}}).mapped());
    const auto files = cache_files(cache_path(dir), ".blob");
    REQUIRE(files.size() == 2);
    for (const auto& file : files) CHECK(fs::status(file).permissions() == fs::perms::owner_read);
}
#endif

TEST_CASE("shared cache: hailstone.ini turns the cache on only with auto and a budget above 0") {
    const fs::path where = "cache-dir";
    CHECK_FALSE(SharedFileCache::configured("off", 1024, where));
    CHECK_FALSE(SharedFileCache::configured("", 1024, where));
    CHECK_FALSE(SharedFileCache::configured("auto", 0, where));
    CHECK_FALSE(SharedFileCache::configured("auto", -5, where));
    const auto on = SharedFileCache::configured("auto", 512, where);
    REQUIRE(on);
    CHECK(on->directory == where);
    CHECK(on->budget_bytes == std::uint64_t{512} << 20);
    CHECK(SharedFileCache::configured("auto", 99999, where)->budget_bytes == std::uint64_t{16384} << 20);
}
