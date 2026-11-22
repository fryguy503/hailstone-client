#pragma once

#include <bitset>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "game/hotbuttons.h"

namespace hailstone::game {

struct CharacterImportOptions {
    bool socials = false;
    bool hot_buttons = false;
    std::bitset<kSocialPages * kSocialButtons> protected_socials{};
};

enum class CharacterImportArea { Social, HotButton };
enum class CharacterImportDisposition { Add, Keep, Skip };

struct CharacterImportRow {
    CharacterImportArea area;
    CharacterImportDisposition disposition = CharacterImportDisposition::Skip;
    int bar = 0; // Display coordinates are one-based. Socials have no bar.
    int page = 0;
    int slot = 0;
    std::string name;
    std::string reason;
    std::vector<std::string> lines;

    bool operator==(const CharacterImportRow&) const = default;
};

struct CharacterImportPlan {
    std::vector<CharacterImportRow> rows;
    std::size_t additions = 0;
    std::string error;

    bool operator==(const CharacterImportPlan&) const = default;
};

// Stages changes against the live settings. An invalid plan leaves staged equal to destination.
CharacterImportPlan plan_character_import(std::string_view bytes, const HotButtonStore& destination,
                                          CharacterImportOptions options, HotButtonStore& staged);

// Persists only the reviewed additions into a copy of the character INI.
void write_character_import_additions(IniFile& ini, const HotButtonStore& staged, const CharacterImportPlan& plan);

} // namespace hailstone::game
