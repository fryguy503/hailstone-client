// platform/durable_file.h and core/atomic_file.h: a save is synced to the disk before it replaces the old file, so a
// power cut leaves one or the other. What a sync does on the disk cannot be seen from a test; what is checked is that
// the writes and the replace do what they say, and fail cleanly.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "core/atomic_file.h"
#include "platform/durable_file.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

TEST_CASE("durable_file: a synced write and a synced replace") {
    test::ScratchDir dir("durable_file");
    const fs::path a = dir.path() / "a.txt", b = dir.path() / "b.txt";
    REQUIRE(platform::write_file_synced(a, "first"));
    CHECK(read_text_file(a) == "first");
    REQUIRE(platform::write_file_synced(a, "second, longer"));
    CHECK(read_text_file(a) == "second, longer");
    REQUIRE(platform::write_file_synced(a, ""));
    CHECK(read_text_file(a) == "");
    CHECK_FALSE(platform::write_file_synced(dir.path() / "no such folder" / "x.txt", "x"));

    REQUIRE(platform::write_file_synced(a, "new"));
    REQUIRE(platform::write_file_synced(b, "old"));
    std::error_code ec;
    CHECK(platform::replace_file_synced(a, b, ec));
    CHECK_FALSE(ec);
    CHECK(read_text_file(b) == "new");
    CHECK_FALSE(fs::exists(a));
    CHECK_FALSE(platform::replace_file_synced(a, b, ec)); // nothing to move
    CHECK(ec);
    CHECK(read_text_file(b) == "new");
}

// The updater's way to replace a file without a moment when its name has none (app/update_install.cpp): previous/
// gets a second name for the old file, then the new one is renamed over the first.
TEST_CASE("durable_file: a synced hard link keeps the old file while the new one replaces it") {
    test::ScratchDir dir("durable_link");
    const fs::path target = dir.path() / "hailstone", saved = dir.path() / "previous-hailstone",
                   fresh = dir.path() / "new-hailstone";
    REQUIRE(platform::write_file_synced(target, "old"));
    REQUIRE(platform::write_file_synced(fresh, "new"));
    std::error_code ec;
#if defined(_WIN32)
    CHECK_FALSE(platform::link_file_synced(target, saved, ec)); // the caller moves the file instead
    CHECK(ec);
    CHECK_FALSE(fs::exists(saved));
#else
    REQUIRE(platform::link_file_synced(target, saved, ec));
    CHECK_FALSE(ec);
    CHECK(read_text_file(target) == "old"); // still there under its own name
    CHECK(read_text_file(saved) == "old");
    CHECK(platform::replace_file_synced(fresh, target, ec));
    CHECK(read_text_file(target) == "new");
    CHECK(read_text_file(saved) == "old"); // the old file, kept for a roll-back
    CHECK_FALSE(platform::link_file_synced(target, saved, ec)); // never over a file already there
    CHECK(ec);
    CHECK(read_text_file(saved) == "old");
#endif
}

TEST_CASE("atomic_file: the file is replaced whole and no temporary file stays") {
    test::ScratchDir dir("atomic_file");
    const fs::path f = dir.path() / "sub" / "settings.ini";
    REQUIRE(write_file_atomic(f, "a=1\n"));
    REQUIRE(write_file_atomic(f, "a=2\nb=3\n"));
    CHECK(read_text_file(f) == "a=2\nb=3\n");
    int files = 0;
    for (const auto& e : fs::directory_iterator(f.parent_path())) {
        (void)e;
        ++files;
    }
    CHECK(files == 1);
    // A folder where the file goes: the write fails and leaves nothing behind.
    fs::create_directories(dir.path() / "taken");
    CHECK_FALSE(write_file_atomic(dir.path() / "taken", "x"));
    CHECK(fs::is_directory(dir.path() / "taken"));
    files = 0;
    for (const auto& e : fs::directory_iterator(dir.path())) {
        (void)e;
        ++files;
    }
    CHECK(files == 2); // sub/ and taken/
}
