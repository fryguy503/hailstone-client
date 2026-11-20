#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "platform/private_file.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;
using hailstone::platform::make_private_dir;
using hailstone::platform::write_private_file;

namespace {
std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("Private file: written owner-only, never over a file already there") {
    hailstone::test::ScratchDir dir("private_file");
    const fs::path key = dir.path() / "release.key";
    REQUIRE(write_private_file(key, "secret\n"));
    CHECK(read_all(key) == "secret\n");
#if !defined(_WIN32)
    const fs::perms perms = fs::status(key).permissions();
    CHECK((perms & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write));
#endif
    CHECK_FALSE(write_private_file(key, "other\n"));
    CHECK(read_all(key) == "secret\n");
    CHECK_FALSE(write_private_file(dir.path() / "no-such-folder" / "k", "x"));
    // A folder at the name is not written into or over, and is left.
    fs::create_directory(dir.path() / "taken");
    CHECK_FALSE(write_private_file(dir.path() / "taken", "x"));
    CHECK(fs::is_directory(dir.path() / "taken"));
#if !defined(_WIN32)
    // Nor a link someone planted at the name, even one to nothing yet: the secret never lands where it points.
    fs::create_symlink(dir.path() / "elsewhere", dir.path() / "planted.key");
    CHECK_FALSE(write_private_file(dir.path() / "planted.key", "secret\n"));
    CHECK_FALSE(fs::exists(dir.path() / "elsewhere"));
#endif
}

TEST_CASE("Private folder: made owner-only, never over a folder or link already there") {
    hailstone::test::ScratchDir dir("private_dir");
    const fs::path d = dir.path() / "scratch";
    REQUIRE(make_private_dir(d));
    CHECK(fs::is_directory(d));
#if !defined(_WIN32)
    CHECK((fs::status(d).permissions() & fs::perms::all) == fs::perms::owner_all);
#endif
    CHECK_FALSE(make_private_dir(d));
    std::ofstream(dir.path() / "file") << "x";
    CHECK_FALSE(make_private_dir(dir.path() / "file"));
    CHECK_FALSE(make_private_dir(dir.path() / "no-such-folder" / "d"));
#if !defined(_WIN32)
    // A link someone planted at the name (to a folder of theirs) is not taken as the new folder.
    fs::create_directory(dir.path() / "theirs");
    fs::create_directory_symlink(dir.path() / "theirs", dir.path() / "planted");
    CHECK_FALSE(make_private_dir(dir.path() / "planted"));
#endif
}
