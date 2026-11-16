#pragma once

#include <filesystem>
#include <optional>

namespace hailstone::platform {

// Resolves the current process executable file, independently of the selected game assets.
std::optional<std::filesystem::path> process_executable_path();

} // namespace hailstone::platform
