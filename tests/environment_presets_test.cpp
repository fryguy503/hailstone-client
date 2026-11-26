#include <doctest/doctest.h>

#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "audio/environment_presets.h"
#include "audio/listener.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "game_files.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::audio;

namespace {

// A made-up 32-bit PE image with a .data section holding a table of made-up zones ("testzone<i>") at the RoF2 table's
// address. No record of the real table is used.
constexpr std::uint32_t kImageBase = 0x400000;
constexpr std::uint32_t kDataVa = 0x6c0000;
constexpr std::uint32_t kDataRaw = 0x400;
constexpr std::uint32_t kDataSize = 0x10000;
constexpr std::size_t kTableAt = kDataRaw + (kZoneRoomTableVa - kImageBase - kDataVa);

std::vector<std::byte> make_exe() {
    ByteWriter w;
    w.le<std::uint16_t>(0x5a4d);
    while (w.size() < 0x3c) w.le<std::uint8_t>(0);
    w.le<std::uint32_t>(0x80);
    while (w.size() < 0x80) w.le<std::uint8_t>(0);
    w.le<std::uint32_t>(0x00004550);
    w.le<std::uint16_t>(0x14c); // i386
    w.le<std::uint16_t>(2);     // sections
    w.le<std::uint32_t>(0);     // time stamp
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint16_t>(224); // optional header size
    w.le<std::uint16_t>(0x102);
    const std::size_t optional_at = w.size();
    w.le<std::uint16_t>(0x10b);
    while (w.size() < optional_at + 28) w.le<std::uint8_t>(0);
    w.le<std::uint32_t>(kImageBase);
    while (w.size() < optional_at + 224) w.le<std::uint8_t>(0);
    auto section = [&](const char* name, std::uint32_t vsize, std::uint32_t va, std::uint32_t raw_size,
                       std::uint32_t raw_at) {
        w.fixed_string(name, 8);
        w.le(vsize);
        w.le(va);
        w.le(raw_size);
        w.le(raw_at);
        for (int i = 0; i < 4; ++i) w.le<std::uint32_t>(0);
    };
    section(".text", 0x100, 0x1000, 0, 0);
    section(".data", kDataSize + 0x1000, kDataVa, kDataSize, kDataRaw);
    while (w.size() < kTableAt) w.le<std::uint8_t>(0);
    for (std::size_t i = 0; i < kZoneRoomRecords; ++i) {
        w.fixed_string("testzone" + std::to_string(i), 32);
        w.le<std::int32_t>(static_cast<std::int32_t>(i % 26));
        w.le<std::int32_t>(static_cast<std::int32_t>((i + 1) % 26));
        w.le<std::int32_t>(static_cast<std::int32_t>((i + 2) % 26));
    }
    while (w.size() < kDataRaw + kDataSize) w.le<std::uint8_t>(0);
    return w.take();
}

void put_u32(std::vector<std::byte>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::byte>(v >> (8 * i));
}

} // namespace

TEST_CASE("Zone room presets are read from the exe's table: headers map the address, every record is checked") {
    auto exe = make_exe();
    std::string error;
    auto table = read_zone_room_presets(exe, &error);
    REQUIRE_MESSAGE(table, error);
    REQUIRE(table->size() == kZoneRoomRecords);
    CHECK((*table)[0].zone == "testzone0");
    CHECK((*table)[30].outside == 4);
    CHECK((*table)[30].high == 5);
    CHECK((*table)[30].low == 6);
    // SetCurrentZone's lookup: case-insensitive; a zone not in the table keeps the defaults.
    auto r = zone_room_presets(*table, "TestZone30");
    CHECK(r.outside == 4);
    CHECK(r.high == 5);
    CHECK(r.low == 6);
    auto none = zone_room_presets(*table, "elsewhere");
    CHECK(none.outside == RoomPresets{}.outside);
    CHECK(zone_room_presets({}, "testzone30").low == RoomPresets{}.low);

    // Anything that does not look like the table gives none.
    auto bad = exe;
    bad[kTableAt] = std::byte{'T'}; // an upper-case name: another build or not the table
    CHECK_FALSE(read_zone_room_presets(bad, &error));
    bad = exe;
    put_u32(bad, kTableAt + 50 * kZoneRoomRecordSize + 0x24, 26); // past the room types
    CHECK_FALSE(read_zone_room_presets(bad, &error));
    CHECK(error == "a record's room type is out of range");
    bad = exe;
    put_u32(bad, kTableAt + 3 * kZoneRoomRecordSize + 0x28, 0xffffffffu);
    CHECK_FALSE(read_zone_room_presets(bad, &error));
    bad = exe;
    for (std::size_t i = 0; i < 32; ++i) bad[kTableAt + 2 * kZoneRoomRecordSize + i] = std::byte{'a'}; // unterminated
    CHECK_FALSE(read_zone_room_presets(bad, &error));
    bad = exe;
    bad[kTableAt + kZoneRoomRecordSize] = std::byte{0}; // an empty name
    CHECK_FALSE(read_zone_room_presets(bad, &error));
    bad = exe;
    bad[0x84] = std::byte{0x64}; // another machine: not a 32-bit x86 exe
    CHECK_FALSE(read_zone_room_presets(bad, &error));
    bad = exe;
    put_u32(bad, 0x3c, 0x7ffffff0u); // e_lfanew past the file
    CHECK_FALSE(read_zone_room_presets(bad, &error));
    // The table's address in a section's zero-filled memory, past the bytes the file holds.
    bad = exe;
    put_u32(bad, 0x80 + 24 + 224 + 40 + 16, 0x1000); // .data's SizeOfRawData
    CHECK_FALSE(read_zone_room_presets(bad, &error));
    CHECK(error == "the table is not in the file's data");
    // Cut inside the table.
    std::vector<std::byte> cut(exe.begin(), exe.begin() + static_cast<std::ptrdiff_t>(kTableAt + 100));
    CHECK_FALSE(read_zone_room_presets(cut, &error));
    CHECK_FALSE(read_zone_room_presets(std::vector<std::byte>{}, &error));
    std::vector<std::byte> junk(4096, std::byte{0x11});
    CHECK_FALSE(read_zone_room_presets(junk, &error));
}

TEST_CASE("Zone room presets: read from the asset folder's eqgame.exe in place, none without one") {
    test::ScratchDir dir("zone_rooms");
    std::ofstream(dir.path() / "spells_us.txt") << "x";
    CHECK(load_zone_room_presets(GameDir(dir.path())).empty());
    {
        auto exe = make_exe();
        std::ofstream f(dir.path() / "eqgame.exe", std::ios::binary);
        f.write(reinterpret_cast<const char*>(exe.data()), static_cast<std::streamsize>(exe.size()));
    }
    auto table = load_zone_room_presets(GameDir(dir.path()));
    REQUIRE(table.size() == kZoneRoomRecords);
    CHECK(table[126].zone == "testzone126");
}

TEST_CASE("Zone room presets of the installed eqgame.exe" * doctest::skip(!hailstone::test::game_dir())) {
    // Structure only: the table's contents are the player's, and are not repeated here. A game folder without
    // eqgame.exe is valid (docs/layout.md): nothing to check then.
    if (!hailstone::test::game_dir()->find_base("eqgame.exe")) return;
    auto table = load_zone_room_presets(*hailstone::test::game_dir());
    // The table is read at RoF2's address: another build's eqgame.exe (a live install's) is refused whole, never read
    // in part. The reference folder's is RoF2's.
    if (table.empty() && !hailstone::test::reference_folder()) {
        MESSAGE("eqgame.exe is not RoF2's: no zone room table, nothing more to check");
        return;
    }
    REQUIRE(table.size() == kZoneRoomRecords);
    std::set<std::string> names;
    for (const auto& z : table) {
        CHECK_FALSE(z.zone.empty());
        CHECK(z.outside >= 0);
        CHECK(z.outside <= kMaxRoomType);
        CHECK(z.high >= 0);
        CHECK(z.high <= kMaxRoomType);
        CHECK(z.low >= 0);
        CHECK(z.low <= kMaxRoomType);
        names.insert(z.zone);
    }
    CHECK(names.size() == kZoneRoomRecords); // one record per zone
}
