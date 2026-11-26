#pragma once

// Writing a settings file so that a crash or a full disk never leaves it half written: the text goes to a new file
// beside it, which then replaces the old one in one rename.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace hailstone {

// Writes `text` to `path` (creating its parent folder) through a temporary file in the same folder, renamed over
// `path` once it is complete. False when anything fails (with a warning in the log); `path` is then as it was. On
// Windows a replace refused because another program has the file open is tried again a few times (about 0.6 s).
bool write_file_atomic(const std::filesystem::path& path, std::string_view text);

// Same durable save, with an exclusive destination name.
enum class AtomicCreate { Created, Exists, Failed };
AtomicCreate create_file_atomic(const std::filesystem::path& path, std::string_view text);

// The whole file as text, or nullopt when it cannot be read.
std::optional<std::string> read_text_file(const std::filesystem::path& path);

} // namespace hailstone
