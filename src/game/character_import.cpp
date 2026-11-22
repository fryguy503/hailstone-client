#include "game/character_import.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <span>
#include <utility>

#include "core/ascii_case.h"
#include "core/character_import_source.h"
#include "core/ini_file.h"
#include "game/spells.h"

namespace hailstone::game {
namespace {

constexpr int kSocialCount = kSocialPages * kSocialButtons;
constexpr std::size_t kMaxLineBytes = 4096;
constexpr std::size_t kSocialName = 0, kSocialColor = 1, kSocialFirstLine = 2;

using Field = std::optional<std::string_view>;
using SocialFields = std::array<Field, kSocialFirstLine + kSocialLines>;

struct SourceFields {
    std::array<SocialFields, kSocialCount> socials;
    std::array<Field, kHotBars * kHotPages * kHotButtons> buttons;
};

struct SocialRecord {
    Social value;
    bool present = false;
    std::string_view error;
};

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

std::string lower_ascii(std::string_view text) {
    std::string folded(text);
    std::transform(folded.begin(), folded.end(), folded.begin(), ascii_lower);
    return folded;
}

bool number(std::string_view text, int& value) {
    if (text.empty()) return false;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool bounded_slot(std::string_view text, int limit, int& value) {
    return !text.empty() && text.front() != '0' && number(text, value) && value >= 1 && value <= limit;
}

std::size_t button_index(int bar, int page, int slot) {
    return static_cast<std::size_t>((bar * kHotPages + page) * kHotButtons + slot);
}

std::string slot_key(int page, int slot) {
    return "Page" + std::to_string(page) + "Button" + std::to_string(slot);
}

int source_section(std::string_view section, const CharacterImportOptions& options) {
    if (section == "socials" && (options.socials || options.hot_buttons)) return kHotBars;
    if (!options.hot_buttons || !section.starts_with("hotbuttons")) return -1;
    section.remove_prefix(std::string_view("hotbuttons").size());
    if (section.empty()) return 0;
    int bar = 0;
    return bounded_slot(section, kHotBars, bar) && bar >= 2 ? bar - 1 : -1;
}

std::optional<std::size_t> social_field(std::string_view tail) {
    if (tail == "name") return kSocialName;
    if (tail == "color") return kSocialColor;
    if (!tail.starts_with("line")) return std::nullopt;
    int line = 0;
    if (!bounded_slot(tail.substr(4), kSocialLines, line)) return std::nullopt;
    return kSocialFirstLine + static_cast<std::size_t>(line - 1);
}

std::string_view read_source(std::string_view text, const CharacterImportOptions& options, SourceFields& fields) {
    if (text.size() > kCharacterImportMaxBytes) return "The source file is too large.";
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    if (text.find('\0') != std::string_view::npos) return "The source file uses an unsupported encoding.";
    std::bitset<kHotBars + 1> seen;
    int section = -1;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        auto line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.size() > kMaxLineBytes || line.find('\r') != std::string_view::npos)
            return "The source file contains an invalid line.";
        line = trim(line);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            if (!line.ends_with(']') || line.find(']') != line.size() - 1)
                return "The source file contains an invalid section header.";
            section = source_section(lower_ascii(trim(line.substr(1, line.size() - 2))), options);
            if (section < 0) continue;
            const auto index = static_cast<std::size_t>(section);
            if (seen.test(index)) return "The source file repeats a settings section.";
            seen.set(index);
            continue;
        }
        if (section < 0) continue;
        const auto equals = line.find('=');
        const std::string key = lower_ascii(trim(line.substr(0, equals)));
        if (!key.starts_with("page")) continue;
        const auto button = key.find("button", 4);
        if (button == std::string::npos) continue;
        const auto slot_begin = button + 6;
        const auto tail_begin = key.find_first_not_of("0123456789", slot_begin);
        const auto key_view = std::string_view(key);
        const auto digits = key_view.substr(slot_begin, tail_begin == std::string::npos ? tail_begin : tail_begin - slot_begin);
        const auto tail = tail_begin == std::string::npos ? std::string_view{} : key_view.substr(tail_begin);
        const bool is_social = section == kHotBars;
        auto field = is_social ? social_field(tail) : std::nullopt;
        if ((is_social && !field) || (!is_social && !tail.empty())) continue;
        int page = 0, slot = 0;
        if (equals == std::string_view::npos ||
            !bounded_slot(key_view.substr(4, button - 4), is_social ? kSocialPages : kHotPages, page) ||
            !bounded_slot(digits, is_social ? kSocialButtons : kHotButtons, slot))
            return "The source file contains an invalid slot key.";
        auto& output = is_social
            ? fields.socials[static_cast<std::size_t>((page - 1) * kSocialButtons + slot - 1)][*field]
            : fields.buttons[button_index(section, page - 1, slot - 1)];
        if (output) return "The source file repeats a settings key.";
        output = line.substr(equals + 1);
    }
    return {};
}

bool printable_ascii(std::string_view text) {
    return std::ranges::all_of(text, [](char c) { return c >= ' ' && c <= '~'; });
}

bool name_arg(std::string_view text) {
    return !text.empty() && text.size() <= 63 && std::ranges::all_of(text, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// Permit only native commands with literal arguments. Timing modifiers may prefix one such command.
bool safe_social_line(std::string_view line) {
    if (line.find_first_of(";$%#|&\\`{}") != std::string_view::npos) return false;
    bool seen_pause = false, seen_timer = false;
    while (!line.empty()) {
        const bool timing_prefix_allowed = line.starts_with('/');
        line = trim(line);
        const auto space = line.find(' ');
        const auto word = lower_ascii(line.substr(0, space));
        auto args = space == std::string_view::npos ? std::string_view{} : trim(line.substr(space + 1));
        if (word == "/pause" || word == "/timer") {
            auto& seen = word == "/pause" ? seen_pause : seen_timer;
            if (seen || !timing_prefix_allowed) return false;
            seen = true;
            const auto comma = args.find(',');
            int amount = 0;
            if (!number(trim(args.substr(0, comma)), amount)) return false;
            if (word == "/pause" ? amount < 1 || amount > 600 : amount < 0 || amount > 360000) return false;
            if (comma == std::string_view::npos) return true;
            line = args.substr(comma + 1);
            if (line.empty()) return false;
            continue;
        }
        if (word == "/assist") return args.empty() || name_arg(args);
        if (word == "/target") return name_arg(args);
        if (word == "/sit" || word == "/stand") return args.empty();
        if (word == "/cast") {
            int gem = 0;
            return bounded_slot(args, kSpellGems, gem);
        }
        if (word == "/say" || word == "/gsay" || word == "/rsay") return !args.empty() && !args.starts_with('/');
        return false;
    }
    return true;
}

SocialRecord read_social(const SocialFields& fields) {
    SocialRecord record;
    const auto name = fields[kSocialName].value_or("");
    const auto color = fields[kSocialColor].value_or("");
    const auto lines = std::span(fields).subspan(kSocialFirstLine);
    const bool has_lines = std::ranges::any_of(lines, [](const Field& line) { return line && !line->empty(); });
    record.present = (name != "*" && !name.empty()) || has_lines;
    if (!record.present) return record;
    if (name.empty() || name.starts_with('*') || name != trim(name) || name.size() > 15 || !printable_ascii(name)) {
        record.error = "Invalid or unsupported social name.";
    } else {
        record.value.name = name;
    }
    if (!color.empty() && (!number(color, record.value.color) || record.value.color < 0 || record.value.color > 19))
        record.error = "Invalid social color.";
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto line = lines[index].value_or("");
        if (line.size() > 255 || !printable_ascii(line)) {
            record.error = "A command line is too long or uses unsupported text.";
            continue;
        }
        record.value.lines[index] = line;
        if (!safe_social_line(line) && record.error.empty()) record.error = "Contains an unsupported command or argument.";
    }
    return record;
}

bool same_social(const Social& a, const Social& b) {
    return !a.empty() && !b.empty() && !a.is_lua() && !b.is_lua() &&
           a.name == b.name && a.color == b.color && a.lines == b.lines;
}

bool same_button(const HotButton& a, const HotButton& b) {
    return a.type == b.type && a.slot == b.slot && a.icon_type == b.icon_type && a.icon_slot == b.icon_slot &&
           a.guid == b.guid && a.item_id == b.item_id && a.label == b.label;
}

bool read_button(std::string_view text, HotButton& button) {
    std::array<std::string_view, 4> fields;
    for (auto& field : fields) {
        const auto comma = text.find(',');
        if (comma == std::string_view::npos) return false;
        field = text.substr(0, comma);
        text.remove_prefix(comma + 1);
    }
    int slot = 0, icon_slot = 0, item_id = 0;
    if (fields[0].empty() || fields[0].front() < 'B' || fields[0].front() > 'N' || !number(fields[0].substr(1), slot) ||
        fields[1].empty() || fields[1].front() != '@' || !number(fields[1].substr(1), icon_slot) || icon_slot != -1 ||
        fields[2] != "0000000000000000" || !number(fields[3], item_id) || item_id != 0 ||
        text.size() > 63 || !printable_ascii(text)) return false;
    button.type = static_cast<HotButtonType>(fields[0].front() - 'A');
    button.slot = slot;
    button.label = text;
    return true;
}

std::string_view menu_name(int slot) {
    switch (static_cast<MenuButton>(slot)) {
    case MenuButton::Who: return "Who";
    case MenuButton::Sit: return "Sit";
    case MenuButton::RunWalk: return "Run/Walk";
    case MenuButton::Options: return "Options";
    case MenuButton::ToggleView: return "Toggle view";
    case MenuButton::Inventory: return "Inventory";
    case MenuButton::SpellBook: return "Spell book";
    case MenuButton::Find: return "Find";
    default: return {};
    }
}

std::bitset<kSocialCount> referenced_socials(const HotButtonStore& destination) {
    std::bitset<kSocialCount> slots;
    for (int bar = 0; bar < kHotBars; ++bar)
        for (int page = 0; page < kHotPages; ++page)
            for (int slot = 0; slot < kHotButtons; ++slot) {
                const HotButton& button = destination.at(bar, page, slot);
                if (button.type == HotButtonType::Social && button.slot >= 0 && button.slot < kSocialCount)
                    slots.set(static_cast<std::size_t>(button.slot));
            }
    return slots;
}

void plan_socials(const std::array<SocialRecord, kSocialCount>& source, const HotButtonStore& destination,
                  const CharacterImportOptions& options, HotButtonStore& staged, CharacterImportPlan& plan) {
    const auto referenced = referenced_socials(destination);
    for (int index = 0; index < kSocialCount; ++index) {
        const auto i = static_cast<std::size_t>(index);
        const auto& record = source[i];
        if (!record.present) continue;
        const Social& existing = destination.social(index);
        CharacterImportRow row{CharacterImportArea::Social, CharacterImportDisposition::Skip,
                               0, index / kSocialButtons + 1, index % kSocialButtons + 1,
                               record.value.name, {}, {record.value.lines.begin(), record.value.lines.end()}};
        if (!record.error.empty()) row.reason = record.error;
        else if (existing.is_lua() || options.protected_socials.test(i)) row.reason = "The destination has a Lua social.";
        else if (same_social(existing, record.value)) {
            row.disposition = CharacterImportDisposition::Keep;
            row.reason = "Already matches.";
        } else if (!existing.empty()) row.reason = "Occupied by " + existing.name + ".";
        else if (referenced.test(i)) row.reason = "An existing hotbar button uses this slot.";
        else {
            staged.social(index) = record.value;
            ++plan.additions;
            row.disposition = CharacterImportDisposition::Add;
            row.reason = "Fills an empty slot.";
        }
        plan.rows.push_back(std::move(row));
    }
}

std::string_view button_error(const HotButton& button, const std::array<SocialRecord, kSocialCount>& socials,
                              const CharacterImportOptions& options, const HotButtonStore& staged) {
    if (button.type == HotButtonType::MenuButton && !menu_name(button.slot).empty()) return {};
    if (button.type == HotButtonType::SpellGem) return "References a character's spell gem slot.";
    if (button.type != HotButtonType::Social || button.slot < 0 || button.slot >= kSocialCount)
        return "The button type or action cannot be imported safely.";
    const auto i = static_cast<std::size_t>(button.slot);
    const auto& requested = socials[i];
    if (!requested.present || !requested.error.empty() || options.protected_socials.test(i) ||
        !same_social(staged.social(button.slot), requested.value))
        return "Requires the matching social in the destination slot.";
    return {};
}

void plan_buttons(const SourceFields& fields, const std::array<SocialRecord, kSocialCount>& socials,
                  const CharacterImportOptions& options, const HotButtonStore& destination,
                  HotButtonStore& staged, CharacterImportPlan& plan) {
    for (int bar = 0; bar < kHotBars; ++bar)
        for (int page = 0; page < kHotPages; ++page)
            for (int slot = 0; slot < kHotButtons; ++slot) {
                const auto value = fields.buttons[button_index(bar, page, slot)];
                if (!value || value->empty() || *value == "*") continue;
                CharacterImportRow row{CharacterImportArea::HotButton, CharacterImportDisposition::Skip,
                                       bar + 1, page + 1, slot + 1, {}, {}, {}};
                HotButton button;
                const auto& existing = destination.at(bar, page, slot);
                if (!read_button(*value, button)) row.reason = "Invalid or unsupported button fields.";
                else {
                    row.name = button.label;
                    if (row.name.empty() && button.type == HotButtonType::MenuButton) row.name = menu_name(button.slot);
                    // An unlabelled social button shows its social's name, also when it is skipped.
                    if (row.name.empty() && button.type == HotButtonType::Social && button.slot >= 0 &&
                        button.slot < kSocialCount)
                        row.name = socials[static_cast<std::size_t>(button.slot)].value.name;
                    row.reason = button_error(button, socials, options, staged);
                    if (row.reason.empty() && same_button(existing, button)) {
                        row.disposition = CharacterImportDisposition::Keep;
                        row.reason = "Already matches.";
                    } else if (row.reason.empty() && !existing.empty()) {
                        row.reason = "The destination slot is occupied.";
                    } else if (row.reason.empty()) {
                        if (button.type == HotButtonType::Social) {
                            const Social& social = staged.social(button.slot);
                            if (row.name.empty()) row.name = social.name;
                            row.reason = "Uses the matching social " + social.name + ".";
                            row.lines.assign(social.lines.begin(), social.lines.end());
                        } else {
                            row.reason = "Adds the " + std::string(menu_name(button.slot)) + " action.";
                        }
                        staged.at(bar, page, slot) = std::move(button);
                        ++plan.additions;
                        row.disposition = CharacterImportDisposition::Add;
                    }
                }
                plan.rows.push_back(std::move(row));
            }
}

} // namespace

CharacterImportPlan plan_character_import(std::string_view bytes, const HotButtonStore& destination,
                                          CharacterImportOptions options, HotButtonStore& staged) {
    staged = destination;
    CharacterImportPlan plan;
    SourceFields fields{};
    plan.error = read_source(bytes, options, fields);
    if (!plan.error.empty()) return plan;
    std::array<SocialRecord, kSocialCount> socials;
    for (std::size_t i = 0; i < socials.size(); ++i) socials[i] = read_social(fields.socials[i]);
    if (options.socials) plan_socials(socials, destination, options, staged, plan);
    if (options.hot_buttons) plan_buttons(fields, socials, options, destination, staged, plan);
    return plan;
}

void write_character_import_additions(IniFile& ini, const HotButtonStore& staged, const CharacterImportPlan& plan) {
    for (const auto& row : plan.rows) {
        if (row.disposition != CharacterImportDisposition::Add) continue;
        const auto key = slot_key(row.page, row.slot);
        if (row.area == CharacterImportArea::HotButton) {
            ini.set(hot_button_section(row.bar - 1), key, format_hot_button(staged.at(row.bar - 1, row.page - 1, row.slot - 1)));
            continue;
        }
        const auto& social = staged.social((row.page - 1) * kSocialButtons + row.slot - 1);
        ini.set("Socials", key + "Name", social.name);
        ini.set_int("Socials", key + "Color", social.color);
        for (std::size_t i = 0; i < social.lines.size(); ++i) {
            const auto line_key = key + "Line" + std::to_string(i + 1);
            if (social.lines[i].empty()) ini.erase("Socials", line_key);
            else ini.set("Socials", line_key, social.lines[i]);
        }
    }
}

} // namespace hailstone::game
