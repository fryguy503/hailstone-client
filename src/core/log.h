#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace hailstone::log {

enum class Level { Debug, Info, Warn, Error };

void set_level(Level level);
// Whether a line at `level` is written at the level set now.
bool enabled(Level level);
void write(Level level, std::string_view message);
// Whether write() also prints each line to stdout (debug, info) and stderr (warn, error); on by default. The file,
// and the recent lines a crash report copies, are written either way. hailstone_tests turns it off, so a run's
// thousands of expected warnings do not bury its report (tests/test_main.cpp).
void set_console(bool on);
// Also writes every line to `path` from now on (the client's logs/hailstone.log), as well as to stdout and stderr.
// The earlier runs' files are kept as <name>.1<ext> to <name>.3<ext> (hailstone.1.log is the last run's). A program
// that runs while another holds `path` (a second client; <name>.lock beside it is the lock) leaves it alone and
// writes <name>.<process id><ext> instead; such files are removed at a later open, but for the newest 5 and those
// written in the last day. A run's file stops at a size limit (100 MiB), with a last line saying so; the lines after
// it go to stdout and stderr only. False when the file cannot be created: the log then goes to stdout and stderr
// only.
bool open_file(const std::filesystem::path& path);
// The size at which the file stops (0: the default, 100 MiB); for tests.
void set_file_limit(std::uint64_t bytes);
// The file the log writes to (empty when none).
std::filesystem::path file_path();
void close_file();
// The last lines written (at most 16, each cut to 199 characters), oldest first, one per line, into `out` (always
// NUL-terminated); the length written. It takes no lock and allocates nothing, so a crash handler may call it
// (platform/crash_log.h); a line written meanwhile may come out mixed.
std::size_t copy_recent_lines(char* out, std::size_t size);
// The message as write() puts it: a line break ("\n" or "\r\n") goes on over a line indented two spaces, and the
// other control characters (C0 but tab, and DEL) are written \r or \xHH, so a server's text with a line break or a
// NUL in it cannot make a "[level] ..." line of its own or cut one short. UTF-8 is left as it is, and so is a
// backslash. Without a control character it is `message` itself (`scratch` is not touched, so
// nothing is allocated); else the escaped text, in `scratch`.
std::string_view escape_controls(std::string_view message, std::string& scratch);

// Debug lines are not formatted while the level leaves them out: the per-frame paths log a few (spawn appearances,
// animations), and formatting them cost every frame.
template <typename... Args> void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (!enabled(Level::Debug)) return;
    write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args> void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args> void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args> void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace hailstone::log
