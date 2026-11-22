#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hailstone {

inline constexpr std::size_t kCharacterImportMaxBytes = 512 * 1024;

enum class CharacterImportFileKind { Settings, Layout, Trader, Barter, HotbarSet };

struct CharacterImportFile {
    CharacterImportFileKind kind;
    std::string name;
};

struct CharacterImportSource {
    std::string character;
    std::string world;
    std::vector<CharacterImportFile> files;
};

struct CharacterImportInventory {
    std::vector<CharacterImportSource> sources;
    std::string error;
};

// Read only the physical game data folder. The file names are relative to that folder and preserve their case.
CharacterImportInventory discover_character_import_sources(const std::filesystem::path& root);

// Recheck the selected file name and read a bounded snapshot to parse or compare with the preview.
std::optional<std::string> read_character_import_source(const std::filesystem::path& root,
                                                        const CharacterImportSource& source, std::string& error);

} // namespace hailstone
