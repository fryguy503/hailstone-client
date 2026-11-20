#pragma once

// Writing a file so that it is on the disk, not only in the system's cache, when the call returns: the parts of
// core/atomic_file.h that need the OS. Without them a power cut soon after a save can leave a replaced file empty or
// holding garbage, since the system may write the rename to the disk before the new file's bytes.

#include <filesystem>
#include <string_view>
#include <system_error>

namespace hailstone::platform {

// Creates `path` (replacing a file there) and writes `text` to it, flushed to the disk before the file is closed
// (fsync, F_FULLFSYNC on macOS, FlushFileBuffers on Windows). False when anything fails; the file may then be partly
// written, and the caller removes it.
bool write_file_synced(const std::filesystem::path& path, std::string_view text);

// Renames `from` over `to` (replacing it) and has the rename on the disk before it returns: on POSIX the folder is
// synced after the rename (a failure of that sync is not an error: the rename is done), on Windows the move is made
// with MOVEFILE_WRITE_THROUGH. *ec why it failed.
bool replace_file_synced(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec);

// A second name `to` for the file `from` (a hard link; `to` must not exist), on the disk before it returns (POSIX: the
// folder synced). The updater keeps an old file in place this way while previous/ gets it, so the new file can then
// replace it in one rename. False with *ec where there are no hard links: Windows (where a running program's files
// cannot be replaced anyway) and file systems without them; the caller then moves the file instead.
bool link_file_synced(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec);

// Flushes a file already written (and closed) by other means to the disk: an unpacked file before the rename that
// puts it in place. Opened for reading on POSIX, so a file made read-only can be synced too. False when it cannot be
// opened or the flush fails.
bool sync_file(const std::filesystem::path& path);

// Appends `text` to `path` (creating it) and has it on the disk before it returns: a write-ahead journal's line,
// which must be there before the move it names is made. When the call creates the file, its folder is synced too
// (POSIX), so the file's name is on the disk as well. False when anything fails.
bool append_file_synced(const std::filesystem::path& path, std::string_view text);

} // namespace hailstone::platform
