#pragma once

// Human-readable names for race and class ids, for display only.
// Ids follow the RoF2 client and EQEmu (common/races.h, common/classes.h).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace hailstone::game {

// CEverQuest::GetLangDesc @0x5159f0: the eqstr id of a language's name (EQEmu's language ids;
// 3115 for unknown ones).
std::uint32_t language_eqstr(std::uint32_t lang);

// Race name, or "Race <id>" for ids not in the table.
std::string race_name(std::uint32_t race_id);
// True for the 16 races a player can create (1-12, 128, 130, 330, 522).
bool is_playable_race(std::uint32_t race_id);

// Class name ("Shadow Knight"), or "Class <id>" for unknown ids. A player class (1-16) and its guildmaster
// (20-35, "<name> Guildmaster") show the server's name for it where one is set (set_class_name_override).
std::string class_name(std::uint32_t class_id);
// Three-letter abbreviation ("SHD"), or the full name for non-player classes; the server's short name where one is
// set.
std::string class_short_name(std::uint32_t class_id);
// Joins several classes for multiclass display: "Warrior / Cleric / Wizard".
// Zero ids are skipped. With abbreviate, uses the short names: "WAR/CLR/WIZ".
std::string class_list_name(std::span<const std::uint32_t> class_ids, bool abbreviate = false);
// A class bit mask (bit c-1 = class c, classes 1..16) as the multiclass display shows it: "War/Clr/Wiz"; "" for
// none.
std::string class_mask_name(std::uint32_t mask);

// ---- the server's class names (an extension past RoF2: sdk set_class_name, hailstone.set_class_name) ----
// Display only: class ids, class checks and everything sent to the server keep the stock classes. A plugin or script
// names a player class (1-16) for the server it plays on; the name belongs to it (`owner`, a process-unique token)
// and goes when it is cleared (the owner unloads, loses `extend`, or the session leaves the server). Several owners
// may name one class: the latest call wins, and when its owner clears, the one before shows again.
inline constexpr std::size_t kClassNameMax = 32;      // bytes
inline constexpr std::size_t kClassShortNameMax = 4;  // bytes
// The stock client's names, whatever a server calls the class: for what names a class to the game (a layout's
// CC_Class_<name>_Button, text that goes to the server) rather than to the player.
std::string stock_class_name(std::uint32_t class_id);
std::string stock_class_short_name(std::uint32_t class_id);
// Sets `owner`'s names for player class `class_id`: an empty one shows the stock name. Both empty takes `owner`'s
// names for the class back. Refused (false, nothing changes) for a class outside 1-16, owner 0, a name longer than
// kClassNameMax or a short name longer than kClassShortNameMax bytes, or a control character in either.
bool set_class_name_override(std::uint64_t owner, std::uint32_t class_id, std::string_view name,
                             std::string_view short_name);
// Takes back every name `owner` set; with no owner, every name (the session left the server).
void clear_class_name_overrides(std::uint64_t owner);
void clear_class_name_overrides();
// The server's name (with short_name, its short name) for player class `class_id`, or nullopt where the stock one
// shows: for the client's own stock text (a string table's class names) that should follow the server's.
std::optional<std::string> class_name_override(std::uint32_t class_id, bool short_name = false);
// Changes whenever a class's shown name may have changed: a caller that keeps a formatted name redoes it then.
std::uint64_t class_names_revision();

} // namespace hailstone::game
