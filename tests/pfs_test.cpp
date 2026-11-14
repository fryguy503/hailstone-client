#include <doctest/doctest.h>

#include <fstream>

#include "assets/pfs.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "core/shared_file_cache.h"
#include "fuzz_support.h"
#include "game_files.h"
#include "pfs_fixtures.h"
#include "scratch_dir.h"

using hailstone::assets::PfsArchive;

TEST_CASE("PfsArchive rejects data that is not PFS") {
    std::vector<std::byte> junk(64, std::byte{0x11});
    CHECK_THROWS(PfsArchive::from_memory(junk));
}

TEST_CASE("PfsArchive reads a classic zone archive" * doctest::skip(!hailstone::test::game_dir())) {
    auto archive = PfsArchive::open(hailstone::test::game_dir()->find("qeynos2.s3d").value());
    CHECK(archive.contains("qeynos2.wld"));
    CHECK(archive.contains("QEYNOS2.WLD"));
    CHECK(archive.contains("objects.wld"));
    CHECK(archive.contains("lights.wld"));
    auto wld = archive.read("qeynos2.wld");
    REQUIRE(wld);
    // WLD magic 0x54503d02
    CHECK(std::to_integer<int>((*wld)[0]) == 0x02);
    CHECK(std::to_integer<int>((*wld)[1]) == 0x3d);
}

TEST_CASE("pfs_name_crc names every file of a classic archive" * doctest::skip(!hailstone::test::game_dir())) {
    // qeynos2.s3d has 152 directory entries: 151 files plus the name table.
    auto archive = PfsArchive::open(hailstone::test::game_dir()->find("qeynos2.s3d").value());
    CHECK(archive.names().size() == 151);
}

TEST_CASE("PfsArchive handles archives with stale names and a 0xffffffff name table" *
          doctest::skip(!hailstone::test::game_dir())) {
    auto gd = hailstone::test::game_dir();
    auto velketor = PfsArchive::open(gd->find("velketor_chr.s3d").value());
    CHECK_FALSE(velketor.contains("kael_chr.wld"));
    CHECK(velketor.contains("velketor_chr.wld"));
    auto gequip = PfsArchive::open(gd->find("gequip_nms.s3d").value());
    CHECK(gequip.contains("bat04.dds"));
}

namespace {
// The archive's directory entry for its first file: {crc, offset, size} after the directory's count.
std::size_t first_entry_at(const std::vector<std::byte>& archive) {
    hailstone::ByteReader r(archive);
    return r.le<std::uint32_t>() + 4;
}
void put_u32(std::vector<std::byte>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::byte>(v >> (8 * i));
}
} // namespace

TEST_CASE("PfsArchive refuses a file size its compressed blocks cannot back, before allocating it") {
    using namespace hailstone::test::pfs_fixtures;
    std::vector<std::byte> text(100, std::byte{'a'});
    auto archive = make_pfs({{"a.txt", text}});
    REQUIRE(PfsArchive::from_memory(archive).read("a.txt").value() == text);

    // A 2 GB size on a 100-byte file: the guard throws HugeAllocation for any request above 64 MB, so a throw of
    // anything else shows the size was refused before the buffer was made.
    auto huge = archive;
    put_u32(huge, first_entry_at(huge) + 8, 0x7fffffffu);
    auto a = PfsArchive::from_memory(huge);
    hailstone::fuzz::arm_allocation_guard(true);
    bool refused = false, allocated = false;
    try {
        (void)a.read("a.txt");
    } catch (const hailstone::fuzz::HugeAllocation&) {
        allocated = true;
    } catch (const std::exception&) {
        refused = true;
    }
    hailstone::fuzz::arm_allocation_guard(false);
    CHECK(refused);
    CHECK_FALSE(allocated);

    // A block claiming more than 1032 bytes per compressed byte.
    hailstone::ByteReader r(archive);
    r.seek(first_entry_at(archive) + 4);
    const std::uint32_t data_at = r.le<std::uint32_t>();
    auto bomb = archive;
    put_u32(bomb, first_entry_at(bomb) + 8, 0x10000000u);
    put_u32(bomb, data_at + 4, 0x10000000u);
    CHECK_THROWS_WITH(PfsArchive::from_memory(bomb).read("a.txt"),
                      doctest::Contains("larger than its compressed bytes"));
}

TEST_CASE("PfsArchive: memory follows the bytes zlib writes, not the size a block declares") {
    using namespace hailstone::test::pfs_fixtures;
    // Within the 1032:1 ratio, 100 KB of junk deflate may declare 103 MB; it used to be zero-filled before zlib
    // failed on the first byte. The guard throws HugeAllocation for any request above 64 MB.
    constexpr std::uint32_t kJunk = 100000;
    constexpr std::uint32_t kDeclared = kJunk * 1032;
    hailstone::ByteWriter body;
    body.le<std::uint32_t>(kJunk);
    body.le<std::uint32_t>(kDeclared);
    body.bytes(std::vector<std::byte>(kJunk, std::byte{0x11}));
    hailstone::ByteWriter names;
    names.le<std::uint32_t>(1);
    names.le<std::uint32_t>(6);
    names.fixed_string("a.txt", 6);
    auto table = deflate_blocks(names.data(), 3000);
    hailstone::ByteWriter w;
    w.le<std::uint32_t>(static_cast<std::uint32_t>(12 + body.size() + table.size()));
    w.le<std::uint32_t>(0x20534650);
    w.le<std::uint32_t>(0x20000);
    w.bytes(body.data());
    w.bytes(table);
    w.le<std::uint32_t>(2);
    w.le<std::uint32_t>(hailstone::assets::pfs_name_crc("a.txt"));
    w.le<std::uint32_t>(12);
    w.le<std::uint32_t>(kDeclared);
    w.le<std::uint32_t>(0x61580ac9u);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(12 + body.size()));
    w.le<std::uint32_t>(static_cast<std::uint32_t>(names.size()));
    auto a = PfsArchive::from_memory(w.take());
    REQUIRE(a.contains("a.txt"));
    hailstone::fuzz::arm_allocation_guard(true);
    bool refused = false, allocated = false;
    try {
        (void)a.read("a.txt");
    } catch (const hailstone::fuzz::HugeAllocation&) {
        allocated = true;
    } catch (const std::exception&) {
        refused = true;
    }
    hailstone::fuzz::arm_allocation_guard(false);
    CHECK(refused);
    CHECK_FALSE(allocated);

    // Real data still reads whole: a 1 MB block (the growing buffer's several steps) and 8 KB blocks.
    std::vector<std::byte> text(1 << 20);
    for (std::size_t i = 0; i < text.size(); ++i) text[i] = static_cast<std::byte>((i * 7) ^ (i >> 9));
    CHECK(PfsArchive::from_memory(make_pfs({{"b.bin", text}}, 1 << 20)).read("b.bin").value() == text);
    CHECK(PfsArchive::from_memory(make_pfs({{"b.bin", text}}, 8192)).read("b.bin").value() == text);
    CHECK(PfsArchive::from_memory(make_pfs({{"e.bin", {}}})).read("e.bin").value().empty());
}

TEST_CASE("PfsArchive: a name listed many times is one member") {
    using namespace hailstone::test::pfs_fixtures;
    std::vector<std::byte> text(10, std::byte{'w'});
    auto archive = make_pfs({{"zone.wld", text}, {"other.txt", text}});
    // Rebuild the name table so it lists zone.wld 1000 times (and other.txt once).
    hailstone::ByteWriter names;
    names.le<std::uint32_t>(1001);
    for (int i = 0; i < 1000; ++i) {
        names.le<std::uint32_t>(9);
        names.fixed_string("ZONE.WLD", 9);
    }
    names.le<std::uint32_t>(10);
    names.fixed_string("other.txt", 10);
    auto table = deflate_blocks(names.data(), 1 << 20);
    const std::size_t dir = first_entry_at(archive) - 4;
    hailstone::ByteWriter w;
    w.bytes({archive.data(), dir});
    const auto table_at = static_cast<std::uint32_t>(w.size());
    w.bytes(table);
    const auto new_dir = static_cast<std::uint32_t>(w.size());
    w.bytes({archive.data() + dir, archive.size() - dir});
    auto out = w.take();
    put_u32(out, 0, new_dir);
    // The name table is the directory's last entry.
    put_u32(out, new_dir + 4 + 2 * 12 + 4, table_at);
    put_u32(out, new_dir + 4 + 2 * 12 + 8, static_cast<std::uint32_t>(names.size()));
    auto a = PfsArchive::from_memory(out);
    CHECK(a.names() == std::vector<std::string>{"zone.wld", "other.txt"});
    CHECK(a.read("zone.wld").value() == text);
}

TEST_CASE("PfsArchive refuses a member or name table larger than any game file") {
    using namespace hailstone::test::pfs_fixtures;
    std::vector<std::byte> text(100, std::byte{'a'});
    auto archive = make_pfs({{"a.txt", text}});
    // Zeros deflate within the ratio check, so the declared size alone has to be refused.
    auto big = archive;
    put_u32(big, first_entry_at(big) + 8, 0x20000000u);
    CHECK_THROWS_WITH(PfsArchive::from_memory(big).read("a.txt"), doctest::Contains("larger than any game file"));
    auto table = archive;
    put_u32(table, first_entry_at(table) + 12 + 8, 0x02000000u);
    CHECK_THROWS_WITH(PfsArchive::from_memory(table), doctest::Contains("name table is too large"));
}

TEST_CASE("PfsArchive::read_prefix inflates a member's first bytes only, across its blocks") {
    using namespace hailstone::test::pfs_fixtures;
    std::vector<std::byte> data(10000);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::byte>(i * 7 % 251);
    auto a = PfsArchive::from_memory(make_pfs({{"thing.mod", data}, {"tiny.txt", {std::byte{'A'}}}}, 3000));
    auto head = a.read_prefix("THING.MOD", 4);
    REQUIRE(head);
    CHECK(*head == std::vector<std::byte>(data.begin(), data.begin() + 4));
    // Across the first block's end (3000 bytes).
    auto more = a.read_prefix("thing.mod", 4500);
    REQUIRE(more);
    CHECK(*more == std::vector<std::byte>(data.begin(), data.begin() + 4500));
    // Past the member's end: the member, not `max` bytes.
    CHECK(a.read_prefix("tiny.txt", 1u << 30).value() == std::vector<std::byte>{std::byte{'A'}});
    CHECK(a.read_prefix("thing.mod", 0).value().empty());
    CHECK_FALSE(a.read_prefix("missing.bmp", 4));
}

TEST_CASE("PfsArchive::read_prefix refuses a block that inflates to less than it declares") {
    using namespace hailstone::test::pfs_fixtures;
    std::vector<std::byte> data(100, std::byte{'x'});
    auto archive = make_pfs({{"a.bin", data}});
    // The first block's header: {deflated, inflated} at offset 12; declare the member and its block bigger.
    put_u32(archive, 16, 200);
    const std::size_t dir = first_entry_at(archive);
    put_u32(archive, dir + 8, 200);
    auto a = PfsArchive::from_memory(archive);
    CHECK(a.read_prefix("a.bin", 50).value().size() == 50);
    CHECK_THROWS(a.read_prefix("a.bin", 150));
}

namespace {
void write_pfs_fixture(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    REQUIRE(out.good());
}
} // namespace

TEST_CASE("PfsArchive mapped snapshots match private parsing and retain their backing") {
    using namespace hailstone;
    using namespace hailstone::test::pfs_fixtures;
    test::ScratchDir dir("pfs-shared");
    const auto source = dir.path() / "fixture.s3d";
    std::vector<std::byte> member(12000);
    for (std::size_t i = 0; i < member.size(); ++i) member[i] = static_cast<std::byte>((i * 7) % 251);
    const auto bytes = make_pfs({{"model.mod", member}, {"empty.bin", {}}}, 3000);
    write_pfs_fixture(source, bytes);
    const auto cache_path = std::filesystem::canonical(dir.path()) / "cache";
    auto first_cache = std::make_shared<SharedFileCache>(SharedFileCache::Options{cache_path});
    auto second_cache = std::make_shared<SharedFileCache>(SharedFileCache::Options{cache_path});
    auto mapped = PfsArchive::open(source, first_cache);
    auto second = PfsArchive::open(source, second_cache);
    auto private_archive = PfsArchive::open(source);
    auto memory_archive = PfsArchive::from_memory(bytes, "fixture.s3d");
    REQUIRE(mapped.backing_mapped());
    REQUIRE(second.backing_mapped());
    CHECK_FALSE(private_archive.backing_mapped());
    CHECK_FALSE(memory_archive.backing_mapped());
    CHECK(mapped.backing_size() == bytes.size());
    CHECK(second_cache->stats().hits >= 1);
    for (const auto* archive : {&mapped, &second, &private_archive, &memory_archive}) {
        CHECK(archive->label() == "fixture.s3d");
        CHECK(archive->names() == memory_archive.names());
        CHECK(archive->contains("MODEL.MOD"));
        CHECK(archive->read("model.mod").value() == member);
        CHECK(archive->read_prefix("model.mod", 4500).value() ==
              std::vector<std::byte>(member.begin(), member.begin() + 4500));
        CHECK(archive->read("empty.bin").value().empty());
        CHECK_FALSE(archive->read("missing.bin"));
    }
    // Archives and copies own their view; destroying the service or the original archive cannot retire it.
    auto copied = mapped;
    first_cache.reset();
    second_cache.reset();
    mapped = PfsArchive::from_memory(make_pfs({{"replacement.bin", {}}}));
    CHECK(copied.backing_mapped());
    CHECK(copied.read("model.mod").value() == member);
    CHECK(second.read("model.mod").value() == member);
}

TEST_CASE("PfsArchive old mappings survive source truncation and distinguish a same-metadata replacement") {
    using namespace hailstone;
    using namespace hailstone::test::pfs_fixtures;
    test::ScratchDir dir("pfs-generation");
    const auto source = dir.path() / "fixture.s3d";
    const std::vector<std::byte> before(128, std::byte{'a'}), after(128, std::byte{'b'});
    const auto before_archive = make_pfs({{"a.bin", before}});
    const auto after_archive = make_pfs({{"a.bin", after}});
    REQUIRE(before_archive.size() == after_archive.size());
    write_pfs_fixture(source, before_archive);
    const auto original_time = std::filesystem::last_write_time(source);
    auto cache = std::make_shared<SharedFileCache>(SharedFileCache::Options{std::filesystem::canonical(dir.path()) / "cache"});
    auto old = PfsArchive::open(source, cache);
    REQUIRE(old.backing_mapped());
    write_pfs_fixture(source, after_archive);
    std::filesystem::last_write_time(source, original_time);
    auto current = PfsArchive::open(source, cache);
    REQUIRE(current.backing_mapped());
    CHECK(current.read("a.bin").value() == after);
    CHECK(old.read("a.bin").value() == before);
    write_pfs_fixture(source, {});
    CHECK_THROWS(PfsArchive::open(source, cache));
    CHECK(current.read("a.bin").value() == after);
    CHECK(old.read("a.bin").value() == before);
}

TEST_CASE("PfsArchive keeps private loading and parser errors when the shared cache is unavailable") {
    using namespace hailstone;
    using namespace hailstone::test::pfs_fixtures;
    test::ScratchDir dir("pfs-fallback");
    const auto source = dir.path() / "fixture.s3d";
    const auto blocked = dir.path() / "cache-is-a-file";
    const std::vector<std::byte> content{std::byte{'a'}};
    write_pfs_fixture(source, make_pfs({{"a.bin", content}}));
    write_pfs_fixture(blocked, content);
    auto cache = std::make_shared<SharedFileCache>(SharedFileCache::Options{blocked});
    auto archive = PfsArchive::open(source, cache);
    CHECK_FALSE(archive.backing_mapped());
    CHECK(archive.read("a.bin").value() == content);
    CHECK(cache->stats().fallbacks >= 1);
    CHECK_THROWS_WITH(PfsArchive::open(dir.path() / "missing.s3d", cache), doctest::Contains("cannot read"));
    write_pfs_fixture(source, std::vector<std::byte>(64, std::byte{0x11}));
    CHECK_THROWS_WITH(PfsArchive::open(source, cache), doctest::Contains("not a PFS archive"));
    CHECK_THROWS_WITH(PfsArchive::open(source), doctest::Contains("not a PFS archive"));
}

TEST_CASE("GameDir preserves shared archive backing across copies moves and asset layer changes") {
    using namespace hailstone;
    test::ScratchDir dir("game-shared-cache");
    const auto base = dir.path() / "base", overlay = dir.path() / "overlay", classic = dir.path() / "classic";
    for (const auto& path : {base, overlay, classic}) std::filesystem::create_directories(path);
    auto cache = std::make_shared<SharedFileCache>(SharedFileCache::Options{dir.path() / "cache"});
    GameDir game(base);
    CHECK_FALSE(game.shared_file_cache());
    game.set_shared_file_cache(cache);
    GameDir copied(game), assigned;
    assigned = game;
    CHECK(copied.shared_file_cache() == cache);
    CHECK(assigned.shared_file_cache() == cache);
    GameDir moved(std::move(copied)), move_assigned;
    move_assigned = std::move(assigned);
    CHECK(moved.shared_file_cache() == cache);
    CHECK(move_assigned.shared_file_cache() == cache);
    game.set_overlay(overlay);
    game.set_classic(classic, [](std::string_view) { return true; });
    game.set_overlay_filter([](std::string_view) { return true; });
    game.rescan();
    game.set_base(base);
    CHECK(game.shared_file_cache() == cache);
    game.set_shared_file_cache({});
    CHECK_FALSE(game.shared_file_cache());
    CHECK(moved.shared_file_cache() == cache);
}

TEST_CASE("PfsArchive mapped stock archives match private member reads" * doctest::skip(!hailstone::test::game_dir())) {
    using namespace hailstone;
    test::ScratchDir dir("pfs-shared-stock");
    auto cache = std::make_shared<SharedFileCache>(SharedFileCache::Options{
        std::filesystem::canonical(dir.path()) / "cache"});
    const auto* game = test::game_dir();
    std::vector<std::filesystem::path> sources;
    for (const auto name : {"qeynos2.s3d", "global_chr.s3d", "sky.s3d"}) {
        const auto source = game->find(name);
        REQUIRE_MESSAGE(source, name);
        sources.push_back(*source);
    }
    // Exercise one available EQG too, without assuming a particular zone is installed.
    for (const auto& entry : game->list("")) {
        if (GameDir::normalize(entry.name).ends_with(".eqg")) {
            sources.push_back(entry.path);
            break;
        }
    }
    for (const auto& source : sources) {
        CAPTURE(source);
        auto private_archive = PfsArchive::open(source);
        auto mapped_archive = PfsArchive::open(source, cache);
        auto warm_archive = PfsArchive::open(source, cache);
        REQUIRE(mapped_archive.backing_mapped());
        CHECK(warm_archive.backing_mapped());
        CHECK(mapped_archive.names() == private_archive.names());
        CHECK(warm_archive.names() == private_archive.names());
        std::size_t checked = 0;
        for (const auto& name : private_archive.names()) {
            CAPTURE(name);
            CHECK(mapped_archive.read(name) == private_archive.read(name));
            CHECK(warm_archive.read_prefix(name, 1024) == private_archive.read_prefix(name, 1024));
            if (++checked == 8) break;
        }
        CHECK(checked > 0);
    }
    CHECK(cache->stats().mapped_bytes == 0);
    CHECK(cache->stats().fallbacks == 0);
}
