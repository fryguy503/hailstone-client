// This process as the system sees it (platform/process.h): its id and memory, the log's lock file, a C file by path
// (not passed on to child processes), an environment variable taken out of the environment.
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "platform/binary_stdio.h"
#include "platform/process.h"
#include "scratch_dir.h"

using namespace hailstone;

TEST_CASE("process: an id, and resident memory where the system tells it") {
    CHECK(platform::process_id() != 0);
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    auto rss = platform::resident_memory();
    REQUIRE(rss);
    CHECK(*rss > 1024u * 1024u); // a test binary holds more than a megabyte
#endif
    platform::lower_current_thread_priority(); // (this thread only; nothing to check but that it returns)
    CHECK_FALSE(platform::system_description().empty());
}

TEST_CASE("process: a lock file is held once; a second holder is turned away") {
    test::ScratchDir dir("process_lock");
    const auto lock = dir.path() / "hailstone.lock";
    CHECK(platform::hold_lock_file(lock));
    // A second open of it (a second client, here the same process again) finds it taken.
    CHECK_FALSE(platform::hold_lock_file(lock));
    // A folder that does not exist: no lock can be made, which counts as held.
    CHECK(platform::hold_lock_file(dir.path() / "missing" / "x.lock"));
}

TEST_CASE("process: a C file opened by a path with any characters in it") {
    test::ScratchDir dir("process_fopen");
    const auto path = dir.path() / std::filesystem::path(u8"log-ä日本.txt");
    std::FILE* f = platform::open_c_file(path, "wb");
    REQUIRE(f);
    std::fputs("x", f);
    std::fclose(f);
    CHECK(std::filesystem::file_size(path) == 1);
}

// The log and the chat log stay open while the client starts the browser helper: neither is passed on to it.
TEST_CASE("process: a C file is not inherited by the processes this one starts") {
    test::ScratchDir dir("process_fopen_inherit");
    for (const char* mode : {"wb", "ab", "rb", "wt"}) {
        CAPTURE(mode);
        std::FILE* f = platform::open_c_file(dir.path() / "file.txt", mode);
        REQUIRE(f);
        CHECK_FALSE(platform::stream_inherited_by_children(f));
        std::fclose(f);
    }
}

TEST_CASE("process: private commit is a Windows measurement, unavailable elsewhere") {
    const auto commit = platform::private_commit_memory();
#if defined(_WIN32)
    // Windows gives a running process's private commit; Wine reports 0, taken as not measured.
    if (commit) CHECK(*commit > 0);
#else
    CHECK_FALSE(commit);
#endif
}

namespace {
// The test's own variable (the environment is this process's alone).
void set_test_env(const std::string& name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name.c_str(), value);
#else
    setenv(name.c_str(), value, 1);
#endif
}
} // namespace

TEST_CASE("process: an environment variable is read once and leaves the environment (--ticket-env)") {
    const std::string name = "HAILSTONE_TEST_TAKE_ENV_" + std::to_string(platform::process_id());
    set_test_env(name, "synthetic-ticket");
    REQUIRE(std::getenv(name.c_str()) != nullptr);
    const auto value = platform::take_environment_variable(name);
    REQUIRE(value);
    CHECK(*value == "synthetic-ticket");
    CHECK(std::getenv(name.c_str()) == nullptr); // a process started now does not inherit it
    CHECK_FALSE(platform::take_environment_variable(name));
    CHECK_FALSE(platform::take_environment_variable(""));
    CHECK_FALSE(platform::take_environment_variable("A=B"));
}
