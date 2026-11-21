#include "game/character_property_sets.h"

#include <algorithm>
#include <charconv>
#include <optional>

namespace hailstone::game {
namespace {

std::optional<std::uint32_t> parse_id(std::string_view field) {
    const auto first = field.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return std::nullopt;
    field = field.substr(first, field.find_last_not_of(" \t\r") - first + 1);
    const bool hex = field.starts_with("0x") || field.starts_with("0X");
    if (hex) field.remove_prefix(2);
    std::uint32_t value = 0;
    const auto end = field.data() + field.size();
    const auto result = std::from_chars(field.data(), end, value, hex ? 16 : 10);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return value;
}

} // namespace

CharacterPropertySets CharacterPropertySets::parse(std::string_view text) {
    CharacterPropertySets table;
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        const auto sep = line.find('^');
        if (sep == std::string_view::npos) continue;
        const auto set_id = parse_id(line.substr(0, sep));
        const auto property = parse_id(line.substr(sep + 1));
        if (!set_id || !property) continue;
        auto& members = table.sets_[*set_id];
        if (std::find(members.begin(), members.end(), *property) == members.end())
            members.push_back(*property);
    }
    return table;
}

std::span<const std::uint32_t> CharacterPropertySets::find(std::uint32_t set_id) const {
    auto it = sets_.find(set_id);
    return it == sets_.end() ? std::span<const std::uint32_t>{} : std::span<const std::uint32_t>(it->second);
}

} // namespace hailstone::game
