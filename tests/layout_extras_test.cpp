// The layout's smaller parts: the screenshot key's file names (platform/paths.h), the pipeline cache file
// (render/pipeline_cache.h) and the crash report (platform/crash_log.h).
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "core/latest_jobs.h"
#include "core/log.h"
#include "platform/crash_log.h"
#include "platform/paths.h"
#include "render/pipeline_cache.h"

namespace fs = std::filesystem;
using namespace hailstone;

namespace {
struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / (std::string("hailstone_extras_") + tag + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

render::PipelineCacheDevice test_device() {
    render::PipelineCacheDevice d;
    d.vendor_id = 0x10de;
    d.device_id = 0x2684;
    d.driver_version = 0x87654321;
    for (std::size_t i = 0; i < d.uuid.size(); ++i) d.uuid[i] = static_cast<std::uint8_t>(0xa0 + i);
    return d;
}

// A driver's cache data: VkPipelineCacheHeaderVersionOne for `d`, then `extra` bytes.
std::vector<std::byte> driver_data(const render::PipelineCacheDevice& d, std::size_t extra) {
    std::vector<std::byte> out;
    auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(v >> (8 * i)));
    };
    put32(32);
    put32(1);
    put32(d.vendor_id);
    put32(d.device_id);
    for (auto b : d.uuid) out.push_back(static_cast<std::byte>(b));
    for (std::size_t i = 0; i < extra; ++i) out.push_back(static_cast<std::byte>(i * 7));
    return out;
}
} // namespace

TEST_CASE("Screenshots: the key's file names") {
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 9;
    t.tm_mday = 4;
    t.tm_hour = 7;
    t.tm_min = 5;
    t.tm_sec = 9;
    CHECK(platform::screenshot_file_name(t) == "hailstone-20261004-070509.png");
    CHECK(platform::screenshot_file_name(t, 2) == "hailstone-20261004-070509-2.png");

    TempDir tmp("shots");
    const std::time_t now = std::time(nullptr);
    fs::path first = platform::new_screenshot_path(tmp.path, now);
    CHECK(first.parent_path() == tmp.path);
    CHECK(first.filename().string().starts_with("hailstone-"));
    CHECK(first.extension() == ".png");
    std::ofstream(first) << "x";
    fs::path second = platform::new_screenshot_path(tmp.path, now); // the same second: a new name
    CHECK(second != first);
    CHECK(second.stem().string() == first.stem().string() + "-2");

    // --screenshot FILE: a relative name goes to screenshots/, an absolute one stays.
    platform::set_data_dir_for_tests(tmp.path);
    CHECK(platform::screenshot_path("a.png") == tmp.path / "screenshots" / "a.png");
    CHECK(platform::screenshot_path(tmp.path / "b.png") == tmp.path / "b.png");
    CHECK(platform::pipeline_cache_file() == tmp.path / "cache" / "pipelines.bin");
    platform::set_data_dir_for_tests({});
}

TEST_CASE("Pipeline cache: the file is used only when whole and made by this GPU and driver") {
    const auto dev = test_device();
    const auto data = driver_data(dev, 100);
    const auto file = render::pack_pipeline_cache(data, dev);
    REQUIRE(file.size() == render::kPipelineCacheFileHeader + data.size());
    CHECK(std::memcmp(file.data(), "HSPC", 4) == 0);

    std::string why;
    auto back = render::unpack_pipeline_cache(file, dev, &why);
    REQUIRE(back);
    CHECK(*back == data);

    auto refused = [&](const std::vector<std::byte>& f, const render::PipelineCacheDevice& d) {
        why.clear();
        bool none = !render::unpack_pipeline_cache(f, d, &why);
        CHECK(!why.empty());
        return none;
    };
    auto other = dev;
    other.vendor_id = 0x1002;
    CHECK(refused(file, other));
    other = dev;
    other.device_id = 1;
    CHECK(refused(file, other));
    other = dev;
    other.uuid[15] ^= 1;
    CHECK(refused(file, other));
    other = dev;
    other.driver_version = 1;
    CHECK(refused(file, other));

    auto cut = file;
    cut.resize(cut.size() - 1);
    CHECK(refused(cut, dev));
    auto damaged = file;
    damaged.back() ^= std::byte{0x40};
    CHECK(refused(damaged, dev));
    auto magic = file;
    magic[0] = std::byte{'X'};
    CHECK(refused(magic, dev));
    CHECK(refused({}, dev));
    // The driver's own header must be there and name this device.
    CHECK(refused(render::pack_pipeline_cache(std::vector<std::byte>(10), dev), dev));
    auto other_gpu = dev;
    other_gpu.device_id = 7;
    CHECK(refused(render::pack_pipeline_cache(driver_data(other_gpu, 4), dev), dev));
}

TEST_CASE("Pipeline cache: saved and read back from cache/pipelines.bin") {
    TempDir tmp("cache");
    const auto dev = test_device();
    const fs::path path = tmp.path / "cache" / "pipelines.bin";
    CHECK(render::load_pipeline_cache(path, dev).empty()); // none yet
    const auto data = driver_data(dev, 1000);
    REQUIRE(render::save_pipeline_cache(path, data, dev));
    CHECK(render::load_pipeline_cache(path, dev) == data);
    for (const auto& e : fs::directory_iterator(path.parent_path()))
        CHECK(e.path().filename() == "pipelines.bin"); // no temporary file left
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "garbage";
    CHECK(render::load_pipeline_cache(path, dev).empty());
}

TEST_CASE("Crash log: the report's name and text") {
    char name[64];
    CHECK(platform::format_crash_name(name, sizeof name, 0) == std::strlen("crash-19700101-000000.txt"));
    CHECK(std::string(name) == "crash-19700101-000000.txt");
    platform::format_crash_name(name, sizeof name, 1792826709); // 2026-10-24 07:25:09 UTC
    CHECK(std::string(name) == "crash-20261024-072509.txt");
    platform::format_crash_name(name, sizeof name, 951782400); // 2000-02-29 00:00:00 (a leap day)
    CHECK(std::string(name) == "crash-20000229-000000.txt");
    char tiny[8];
    CHECK(platform::format_crash_name(tiny, sizeof tiny, 0) == 7);
    CHECK(std::string(tiny) == "crash-1");

    char report[512];
    platform::format_crash_report(report, sizeof report, 1792826709, "signal 11 (SIGSEGV)", "/x/logs/hailstone.log",
                                  "[info] one\n[warn] two\n");
    CHECK(std::string(report) == "Hailstone crash report\n"
                                 "time: 2026-10-24 07:25:09 UTC\n"
                                 "what: signal 11 (SIGSEGV)\n"
                                 "log: /x/logs/hailstone.log\n"
                                 "last log lines:\n"
                                 "[info] one\n[warn] two\n");

    platform::format_crash_report(report, sizeof report, 0, "test", "", "", "0.7.3-beta (abc1234)", "00ff");
    CHECK(std::string(report) == "Hailstone crash report\n"
                                 "build: 0.7.3-beta (abc1234)\n"
                                 "build-id: 00ff\n"
                                 "time: 1970-01-01 00:00:00 UTC\n"
                                 "what: test\n"
                                 "last log lines:\n");
}

namespace {
// An ELF note as it lies in memory: name size, descriptor size, type in the machine's order, then the padded name
// and descriptor.
void put_note(std::vector<unsigned char>& out, std::uint32_t type, std::string_view name,
              const std::vector<unsigned char>& desc, std::size_t align) {
    auto word = [&](std::uint32_t v) {
        unsigned char b[4];
        std::memcpy(b, &v, 4);
        out.insert(out.end(), b, b + 4);
    };
    auto pad = [&] { while (out.size() % align) out.push_back(0); };
    word(static_cast<std::uint32_t>(name.size()));
    word(static_cast<std::uint32_t>(desc.size()));
    word(type);
    out.insert(out.end(), name.begin(), name.end());
    pad();
    out.insert(out.end(), desc.begin(), desc.end());
    pad();
}
} // namespace

TEST_CASE("Crash log: the GNU build ID in a note segment") {
    const std::vector<unsigned char> id = {0xde, 0xad, 0xbe, 0xef, 0x01};
    for (std::size_t align : {std::size_t{4}, std::size_t{8}}) {
        std::vector<unsigned char> notes;
        put_note(notes, 5, std::string_view("GNU\0", 4), {1, 2, 3, 4, 5, 6, 7, 8}, align); // a property note first
        put_note(notes, 3, std::string_view("XYZ\0", 4), {9, 9}, align);                   // another vendor's type 3
        put_note(notes, 3, std::string_view("GNU\0", 4), id, align);
        const unsigned char* found = nullptr;
        REQUIRE(platform::find_gnu_build_id(notes.data(), notes.size(), align, &found) == id.size());
        CHECK(std::vector<unsigned char>(found, found + id.size()) == id);
        // Cut short anywhere, it reads nothing past the end.
        for (std::size_t n = 0; n < notes.size() - 7; ++n)
            CHECK(platform::find_gnu_build_id(notes.data(), n, align, &found) == 0);
    }
    std::vector<unsigned char> none;
    put_note(none, 1, std::string_view("GNU\0", 4), {1, 2, 3, 4}, 4); // NT_GNU_ABI_TAG
    CHECK(platform::find_gnu_build_id(none.data(), none.size(), 4, nullptr) == 0);
    std::vector<unsigned char> huge;
    put_note(huge, 3, std::string_view("GNU\0", 4), id, 4);
    huge[4] = 0xff; // a descriptor size past the end
    huge[5] = 0xff;
    CHECK(platform::find_gnu_build_id(huge.data(), huge.size(), 4, nullptr) == 0);
    CHECK(platform::find_gnu_build_id(huge.data(), huge.size(), 3, nullptr) == 0); // not an alignment
}

namespace {
struct CountedFormat {
    int* formatted;
};
} // namespace

template <> struct std::formatter<CountedFormat> : std::formatter<int> {
    auto format(const CountedFormat& c, std::format_context& ctx) const {
        ++*c.formatted;
        return std::formatter<int>::format(*c.formatted, ctx);
    }
};

TEST_CASE("Log: debug lines are formatted only while the level writes them") {
    int formatted = 0;
    log::set_level(log::Level::Info);
    CHECK_FALSE(log::enabled(log::Level::Debug));
    CHECK(log::enabled(log::Level::Info));
    log::debug("left out {}", CountedFormat{&formatted});
    CHECK(formatted == 0);
    log::set_level(log::Level::Debug);
    CHECK(log::enabled(log::Level::Debug));
    log::debug("written {}", CountedFormat{&formatted});
    CHECK(formatted == 1);
    log::set_level(log::Level::Info);
}

TEST_CASE("Crash log: the last log lines and the report file") {
    for (int i = 0; i < 20; ++i) log::info("crash test line {}", i);
    char recent[4096];
    std::size_t n = log::copy_recent_lines(recent, sizeof recent);
    std::string lines(recent, n);
    CHECK(lines.find("[info] crash test line 19\n") != std::string::npos);
    CHECK(lines.find("crash test line 4\n") != std::string::npos);
    CHECK(lines.find("crash test line 3\n") == std::string::npos); // 16 kept
    char small[10];
    CHECK(log::copy_recent_lines(small, sizeof small) == 9);

    TempDir tmp("crash");
    platform::set_crash_report_dir(tmp.path / "logs");
    REQUIRE(platform::write_crash_report("test"));
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(tmp.path / "logs")) files.push_back(e.path());
    REQUIRE(files.size() == 1);
    CHECK(files[0].filename().string().starts_with("crash-"));
    CHECK(files[0].extension() == ".txt");
    std::string text = read_all(files[0]);
    CHECK(text.find("what: test\n") != std::string::npos);
    CHECK(text.find("crash test line 19") != std::string::npos);
    CHECK(text.find("build: ") != std::string::npos);
#if defined(__linux__)
    CHECK(text.find("\nstack:\n") != std::string::npos);
    // The tests are linked with --build-id (cmake/SeparateSymbols.cmake), as the client is.
    const std::string build_id = platform::crash_build_id();
    CHECK(build_id.size() >= 16);
    CHECK(build_id.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(text.find("\nbuild-id: " + build_id + "\n") != std::string::npos);
#endif
    platform::set_crash_report_dir({});
    CHECK_FALSE(platform::write_crash_report("nowhere"));
}

#ifndef _WIN32
#if defined(__SANITIZE_ADDRESS__)
#define HAILSTONE_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HAILSTONE_TEST_ASAN 1
#endif
#endif

namespace {
volatile bool g_stop_recursion = false;

// Recurses until the stack runs out.
int use_up_stack(int depth) {
    volatile char pad[4096];
    pad[0] = static_cast<char>(depth);
    if (g_stop_recursion) return 0;
    return use_up_stack(depth + 1) + pad[0];
}

// Runs `crash` in a child process that has a SIGSEGV and SIGBUS handler of its own (exit status 42) installed before the crash
// handler; the child's exit status, or -1 when it did not exit by itself.
int crash_in_child(const fs::path& logs, void (*crash)()) {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::alarm(20); // a hang ends the child, not the tests
        struct sigaction earlier{};
        earlier.sa_handler = [](int) { ::_exit(42); };
        sigemptyset(&earlier.sa_mask);
        earlier.sa_flags = SA_ONSTACK;
        sigaction(SIGSEGV, &earlier, nullptr);
        sigaction(SIGBUS, &earlier, nullptr); // macOS: a thread's stack overflow hits its guard page as SIGBUS
        platform::install_crash_handler(logs);
        crash();
        ::_exit(1);
    }
    int status = 0;
    if (pid < 0 || ::waitpid(pid, &status, 0) != pid) return -2;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::size_t crash_files(const fs::path& logs) {
    std::size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(logs, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename().string().starts_with("crash-")) ++n;
    return n;
}
} // namespace

TEST_CASE("Crash log: after the report the handler that was there before still runs") {
    TempDir tmp("crash_chain");
    CHECK(crash_in_child(tmp.path / "logs", [] { std::raise(SIGSEGV); }) == 42);
    CHECK(crash_files(tmp.path / "logs") == 1);
}

#ifndef HAILSTONE_TEST_ASAN // AddressSanitizer reports a stack overflow its own way
TEST_CASE("Crash log: a stack overflow on a thread with a CrashStack is reported") {
    TempDir tmp("crash_thread");
    CHECK(crash_in_child(tmp.path / "logs", [] {
              std::thread([] {
                  platform::CrashStack stack;
                  use_up_stack(0);
              }).join();
          }) == 42);
    CHECK(crash_files(tmp.path / "logs") == 1);
}

TEST_CASE("Crash log: the client's worker threads make their CrashStack (a LatestJobs job's overflow)") {
    TempDir tmp("crash_jobs");
    CHECK(crash_in_child(tmp.path / "logs", [] {
              core::LatestJobs<int, int> jobs;
              jobs.submit(1, [] { return use_up_stack(0); });
              jobs.wait_idle();
          }) == 42);
    CHECK(crash_files(tmp.path / "logs") == 1);
}
#endif
#endif
