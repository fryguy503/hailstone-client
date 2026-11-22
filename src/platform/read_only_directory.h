#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hailstone::platform {

// Pins a directory for bounded enumeration and direct child reads. open() resolves links in the path to the directory
// itself; no symlinks or reparse points below it are followed.
class ReadOnlyDirectory {
public:
    enum class EntryType { File, Directory, Other };
    struct Entry {
        std::filesystem::path name;
        EntryType type;
    };

    static std::optional<ReadOnlyDirectory> open(const std::filesystem::path& path);
    std::optional<ReadOnlyDirectory> child(const std::filesystem::path& name) const;
    std::optional<std::vector<Entry>> list(std::size_t max_entries) const;
    std::optional<std::string> read(const std::filesystem::path& name, std::size_t max_bytes) const;

private:
    struct Handle;
    explicit ReadOnlyDirectory(std::shared_ptr<const Handle> handle);
    std::shared_ptr<const Handle> handle_;
};

} // namespace hailstone::platform
