#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>

#include "core/env.h"
#include "core/log.h"
#include "test_shard.h"

namespace {

// The log's lines stay off the console unless HAILSTONE_TEST_LOG is set: a run writes thousands of expected warnings
// (stand-in game folders without racedata.txt or spells_us.txt, scripts refused on purpose), which buried a real
// one. The log still keeps its recent lines, and a test that opens a log file still gets every line in it.
//
// With HAILSTONE_TEST_TRACE set, each test case's name goes to stderr as it starts. A crash that ends the process at once
// (on Windows a fast fail, 0xc0000409) loses the buffered report on stdout, so without this a CI log cannot say which
// test it was in. stderr is unbuffered, and the name is flushed before the test runs.
struct TraceListener : doctest::IReporter {
    const bool on = hailstone::core::env("TEST_TRACE") != nullptr;

    explicit TraceListener(const doctest::ContextOptions&) {}

    void test_run_start() override {
        if (hailstone::core::env("TEST_LOG") == nullptr) hailstone::log::set_console(false);
    }

    void test_case_start(const doctest::TestCaseData& tc) override {
        if (!on) return;
        std::fprintf(stderr, "[test] %s (%s:%d)\n", tc.m_name, tc.m_file.c_str(), tc.m_line);
        std::fflush(stderr);
    }
    void report_query(const doctest::QueryData&) override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

} // namespace

REGISTER_LISTENER("hailstone_trace", 1, TraceListener);

int main(int argc, char** argv) {
    doctest::Context context;
    // Set before the command line, which wins: a -tc run under ctest still picks its cases. The seed is fixed so every
    // part shuffles the list the same way and the ranges do not overlap.
    if (const char* spec = hailstone::core::env("TEST_SHARD")) {
        const auto range = hailstone::test::shard_range(spec, doctest::detail::getRegisteredTests().size());
        if (!range) {
            std::fprintf(stderr, "HAILSTONE_TEST_SHARD=%s is not <k>/<n> with 1 <= k <= n\n", spec);
            return 2;
        }
        context.setOption("order-by", "rand");
        context.setOption("rand-seed", 1);
        context.setOption("first", static_cast<int>(range->first));
        context.setOption("last", static_cast<int>(range->last));
    }
    context.applyCommandLine(argc, argv);
    return context.run();
}
