#pragma once

// This process and its threads as the operating system sees them: its id, its memory, a background thread's
// priority, a lock file held for the process's life, a C file opened by path, an environment variable taken out of
// the environment, and the system's name for bug reports.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace hailstone::platform {

// This process's id.
unsigned long process_id();

// The process's resident memory (the working set on Windows) in bytes; nullopt where it cannot be read.
std::optional<std::uint64_t> resident_memory();

// Windows PROCESS_MEMORY_COUNTERS_EX::PrivateUsage (private committed bytes, not working set). Returns nullopt
// on other systems, when the query fails or when it reports 0 (Wine); virtual size and RSS are not substitutes for
// private commit.
std::optional<std::uint64_t> private_commit_memory();

// Lowers the calling thread's scheduling priority (nice 10), for a background loader: the frame and the network come
// first. Linux only, where each thread has its own nice value; elsewhere it does nothing.
void lower_current_thread_priority();

// Takes `lock` (created when missing) for this process until it ends; on Windows the file goes with it. False only
// when another process holds it; a lock that cannot be made at all (a read-only folder) counts as taken by us.
bool hold_lock_file(const std::filesystem::path& lock);

// std::fopen for a path; on Windows by its wide name, so any character in it works. The file is not passed on to the
// processes this one starts (the browser helper and, through it, CEF's): closed across exec on POSIX, a handle that
// is not inheritable on Windows.
std::FILE* open_c_file(const std::filesystem::path& path, const char* mode);

// Windows: a GUI program (no console of its own) started from a console writes its stdout and stderr there from
// now on (a tool's command-line mode). True when it did; elsewhere, where the streams already go to the terminal,
// it does nothing and returns true.
bool attach_parent_console();

// Reads the environment variable `name` and removes it from this process's environment, so that the processes it
// starts afterwards do not inherit it (a launcher's ticket: --ticket-env). Nullopt when it is not set. The copy is the
// caller's to wipe (core/wipe.h). Not thread-safe, like the environment itself: call it before threads start.
std::optional<std::string> take_environment_variable(const std::string& name);

// The system, for bug reports: "Linux, CachyOS (kernel 7.3.0-4-cachyos), wayland", "Windows 10.0.26100".
std::string system_description();

} // namespace hailstone::platform
