// core/log.h escape_controls: a message's control characters cannot make a log entry of their own or cut one short.
#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "core/env.h"
#include "core/log.h"

using hailstone::log::escape_controls;

TEST_CASE("log: control characters are escaped, the rest left as it is") {
    std::string scratch;
    // The common case is the message itself, with nothing allocated.
    const std::string_view plain = "zone: entered qeynos \xe2\x80\x94 C:\\EQ\\eqgame\tok";
    CHECK(escape_controls(plain, scratch).data() == plain.data());
    CHECK(scratch.empty());

    using namespace std::string_view_literals;
    // A line break goes on indented, so the next line cannot pass for an entry of its own.
    CHECK(escape_controls("web: refused http://a\n[info] chat: forged"sv, scratch) ==
          "web: refused http://a\n  [info] chat: forged");
    CHECK(escape_controls("a\r\nb\rc\n"sv, scratch) == "a\n  b\\rc\n  ");
    CHECK(escape_controls("lua: x.lua:3: oops\nstack traceback:\n\t[C]: in ?"sv, scratch) ==
          "lua: x.lua:3: oops\n  stack traceback:\n  \t[C]: in ?");
    CHECK(escape_controls("cut\0here"sv, scratch) == "cut\\x00here");
    CHECK(escape_controls("\x1b[31mred\x7f"sv, scratch) == "\\x1b[31mred\\x7f");
    CHECK(escape_controls("\xc3\xa9t\xc3\xa9\x01"sv, scratch) == "\xc3\xa9t\xc3\xa9\\x01"); // UTF-8 stays
}

TEST_CASE("log: a message with a line break goes on indented in the crash report's recent lines") {
    using namespace std::string_view_literals;
    hailstone::log::write(hailstone::log::Level::Error, "log_escape_test: one\ntwo\0three"sv);
    char recent[16 * 200 + 1];
    const std::size_t n = hailstone::log::copy_recent_lines(recent, sizeof recent);
    const std::string_view lines(recent, n);
    CHECK(lines.find("[error] log_escape_test: one\n  two\\x00three\n") != std::string_view::npos);
}

TEST_CASE("log: with the console off a line still reaches the crash report's recent lines") {
    using namespace std::string_view_literals;
    hailstone::log::set_console(false);
    hailstone::log::write(hailstone::log::Level::Warn, "log_escape_test: kept while quiet"sv);
    char buf[4096];
    const std::size_t n = hailstone::log::copy_recent_lines(buf, sizeof buf);
    hailstone::log::set_console(hailstone::core::env("TEST_LOG") != nullptr); // as tests/test_main.cpp left it
    CHECK(std::string_view(buf, n).find("[warn] log_escape_test: kept while quiet\n") != std::string_view::npos);
}
