// platform/child_processes.h: the box set's other windows are started with their standard streams going nowhere,
// so a start never depends on this program having a console to hand on (a Windows window started from Explorer
// has none, and SDL_CreateProcess's inherited stdout failed there with "DuplicateHandle(): The handle is invalid").
#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>

#include "platform/child_processes.h"

using namespace hailstone;

#ifdef HAILSTONE_MOCK_WEB_HELPER
TEST_CASE("Child processes: a started program runs with its output going nowhere, and is collected once it exits") {
    platform::ChildProcesses children;
    std::string error;
    // The stand-in browser helper's --print writes to stdout and exits at once.
    REQUIRE_MESSAGE(children.start({HAILSTONE_MOCK_WEB_HELPER, "--print"}, error), error);
    CHECK(error.empty());
    CHECK(children.running() == 1);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (children.running() != 0 && std::chrono::steady_clock::now() < until) {
        children.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(children.running() == 0);
}
#endif

TEST_CASE("Child processes: a program that is not there is not started, and says why") {
    platform::ChildProcesses children;
    std::string error;
    CHECK_FALSE(children.start({"/no/such/program/hailstone-test-missing"}, error));
    CHECK_FALSE(error.empty());
    CHECK(children.running() == 0);
}
