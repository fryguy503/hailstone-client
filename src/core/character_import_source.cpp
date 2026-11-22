#include "core/character_import_source.h"

#include <algorithm>
#include <map>
#include <string_view>
#include <utility>

#include "core/ascii_case.h"
#include "platform/read_only_directory.h"

namespace hailstone {
namespace {

using Directory = platform::ReadOnlyDirectory;
using FileKind = CharacterImportFileKind;
constexpr std::size_t kMaxEntries = 65536;
constexpr std::size_t kMaxProbeFiles = 4096;
constexpr std::size_t kMaxProbeBytes = 32 * 1024 * 1024;

std::string lower(std::string_view name) {
    std::string result(name);
    std::transform(result.begin(), result.end(), result.begin(), ascii_lower);
    return result;
}

bool ascii_letter(char c) { return ascii_lower(c) >= 'a' && ascii_lower(c) <= 'z'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool world_char(char c) {
    constexpr std::string_view unsafe = R"(\/:*?"<>|#)";
    return c >= ' ' && c <= '~' && unsafe.find(c) == std::string_view::npos;
}

std::optional<std::string> entry_name(const std::filesystem::path& path) {
    std::string name;
    name.reserve(path.native().size());
    for (auto unit : path.native()) {
        if (unit < ' ' || unit > '~') return std::nullopt;
        name.push_back(static_cast<char>(unit));
    }
    return name;
}

struct SourceName {
    FileKind kind;
    std::string_view character;
    std::string_view world;
};

std::optional<SourceName> identify(std::string_view filename, bool userdata) {
    if (!iends_with(filename, ".ini")) return std::nullopt;
    std::string_view stem = filename.substr(0, filename.size() - 4);
    FileKind kind = FileKind::Settings;
    if (istarts_with(stem, "UI_") && !userdata) {
        kind = FileKind::Layout;
        stem.remove_prefix(3);
    } else if (istarts_with(stem, "BZR_") && !userdata) {
        kind = FileKind::Trader;
        stem.remove_prefix(4);
    } else if (istarts_with(stem, "BART_") && userdata) {
        kind = FileKind::Barter;
        stem.remove_prefix(5);
    } else if (istarts_with(stem, "HB_") && userdata) {
        kind = FileKind::HotbarSet;
        stem.remove_prefix(3);
        const auto number_end = stem.find('_');
        if (number_end == std::string_view::npos || number_end == 0 || number_end > 10 ||
            !std::ranges::all_of(stem.substr(0, number_end), digit)) return std::nullopt;
        stem.remove_prefix(number_end + 1);
    } else if (userdata || istarts_with(stem, "BART_") || istarts_with(stem, "HB_")) {
        return std::nullopt;
    }
    const auto separator = stem.find('_');
    if (separator == std::string_view::npos) return std::nullopt;
    const auto character = stem.substr(0, separator);
    const auto world = stem.substr(separator + 1);
    if (character.empty() || character.size() > 32 || !ascii_letter(character.front()) ||
        !std::ranges::all_of(character, [](char c) { return ascii_letter(c) || digit(c); }) ||
        world.empty() || world.front() == ' ' || world.back() == ' ' || !std::ranges::all_of(world, world_char))
        return std::nullopt;
    return SourceName{kind, character, world};
}

std::string_view trim(std::string_view line) {
    const auto start = line.find_first_not_of(" \t\r");
    return start == std::string_view::npos ? std::string_view{} : line.substr(start, line.find_last_not_of(" \t\r") - start + 1);
}

bool character_sections(std::string_view bytes) {
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.remove_prefix(3);
    while (!bytes.empty()) {
        const auto end = bytes.find('\n');
        auto line = trim(bytes.substr(0, end));
        if (line.starts_with('[') && line.ends_with(']')) {
            const auto section = trim(line.substr(1, line.size() - 2));
            if (iequals(section, "Socials") || iequals(section, "Abilities") || iequals(section, "CombatSkills") ||
                iequals(section, "CombatAbilities") || iequals(section, "BlockedSpells") ||
                iequals(section, "BlockedPetSpells") || iequals(section, "SpellLoadouts") ||
                iequals(section, "HBLoadouts") || iequals(section, "HotButtons")) return true;
            if (istarts_with(section, "HotButtons")) {
                const auto number = section.substr(10);
                if (number == "10" || (number.size() == 1 && number.front() >= '2' && number.front() <= '9')) return true;
            }
        }
        if (end == std::string_view::npos) break;
        bytes.remove_prefix(end + 1);
    }
    return false;
}

struct NamedEntry {
    std::string name;
    const Directory::Entry* entry;
};

// A null entry marks any name that collides under case-insensitive lookup.
std::map<std::string, NamedEntry> unique_names(const std::vector<Directory::Entry>& entries) {
    std::map<std::string, NamedEntry> names;
    for (const auto& entry : entries) {
        auto name = entry_name(entry.name);
        if (!name) continue;
        auto key = lower(*name);
        auto [it, unique] = names.try_emplace(std::move(key), NamedEntry{std::move(*name), &entry});
        if (!unique) it->second.entry = nullptr;
    }
    return names;
}

using GroupKey = std::pair<std::string, std::string>;
using Groups = std::map<GroupKey, CharacterImportSource>;

GroupKey key_for(const SourceName& source) { return {lower(source.character), lower(source.world)}; }

void add_file(Groups& groups, const SourceName& source, std::string name) {
    auto& group = groups[key_for(source)];
    if (group.files.empty() || source.kind == FileKind::Settings) {
        group.character = source.character;
        group.world = source.world;
    }
    group.files.push_back({source.kind, std::move(name)});
}

} // namespace

CharacterImportInventory discover_character_import_sources(const std::filesystem::path& root) {
    auto directory = Directory::open(root);
    if (!directory) return {{}, "The game data folder is unavailable for import."};
    auto entries = directory->list(kMaxEntries);
    if (!entries) return {{}, "Could not scan the game data folder for character settings."};
    const auto names = unique_names(*entries);

    Groups groups;
    const auto userdata = names.find("userdata");
    if (userdata != names.end() && userdata->second.entry && userdata->second.entry->type == Directory::EntryType::Directory) {
        auto child = directory->child(userdata->second.entry->name);
        auto files = child ? child->list(kMaxEntries) : std::nullopt;
        if (!files) return {{}, "Could not scan the game data folder for character settings."};
        for (const auto& [key, item] : unique_names(*files)) {
            if (!item.entry || item.entry->type != Directory::EntryType::File) continue;
            if (auto source = identify(item.name, true)) add_file(groups, *source, userdata->second.name + "/" + item.name);
        }
    }

    std::vector<std::pair<SourceName, const NamedEntry*>> candidates;
    for (const auto& [key, item] : names) {
        if (!item.entry || item.entry->type != Directory::EntryType::File) continue;
        auto source = identify(item.name, false);
        if (!source) continue;
        if (source->kind == FileKind::Settings) candidates.emplace_back(*source, &item);
        else add_file(groups, *source, item.name);
    }
    std::size_t probes = 0;
    std::size_t probe_bytes = 0;
    for (const auto& [source, item] : candidates) {
        if (!groups.contains(key_for(source))) {
            if (++probes > kMaxProbeFiles) return {{}, "Too many files to identify character settings safely."};
            auto bytes = directory->read(item->entry->name, kCharacterImportMaxBytes);
            if (!bytes) continue;
            probe_bytes += bytes->size();
            if (probe_bytes > kMaxProbeBytes) return {{}, "Too much data to identify character settings safely."};
            if (!character_sections(*bytes)) continue;
        }
        add_file(groups, source, item->name);
    }

    CharacterImportInventory inventory;
    inventory.sources.reserve(groups.size());
    for (auto& [key, group] : groups) {
        std::ranges::stable_sort(group.files, {}, &CharacterImportFile::kind);
        inventory.sources.push_back(std::move(group));
    }
    return inventory;
}

std::optional<std::string> read_character_import_source(const std::filesystem::path& root,
                                                        const CharacterImportSource& source, std::string& error) {
    error.clear();
    const CharacterImportFile* settings = nullptr;
    for (const auto& file : source.files) {
        if (file.kind != FileKind::Settings) continue;
        if (settings) {
            error = "More than one character settings file was selected.";
            return std::nullopt;
        }
        settings = &file;
    }
    const auto expected = settings ? identify(settings->name, false) : std::nullopt;
    if (!expected || expected->kind != FileKind::Settings || !iequals(source.character, expected->character) ||
        !iequals(source.world, expected->world)) {
        error = "No valid character settings file is selected.";
        return std::nullopt;
    }
    auto directory = Directory::open(root);
    auto entries = directory ? directory->list(kMaxEntries) : std::nullopt;
    if (!entries) {
        error = "Could not check the selected file in the game data folder.";
        return std::nullopt;
    }
    const Directory::Entry* match = nullptr;
    bool exact_name = false;
    for (const auto& entry : *entries) {
        auto name = entry_name(entry.name);
        if (!name || !iequals(*name, settings->name)) continue;
        if (match) {
            error = "The selected file name is ambiguous. Refresh the sources.";
            return std::nullopt;
        }
        match = &entry;
        exact_name = *name == settings->name;
    }
    if (!match || match->type != Directory::EntryType::File || !exact_name) {
        error = "The selected file changed or is unavailable. Refresh the sources.";
        return std::nullopt;
    }
    auto bytes = directory->read(match->name, kCharacterImportMaxBytes);
    if (!bytes) error = "The selected file could not be read safely or exceeds the import size limit.";
    return bytes;
}

} // namespace hailstone
