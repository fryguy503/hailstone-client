#include "plugin/policy.h"

#include <algorithm>
#include <set>

#include "game/slash_commands.h"
#include "game/social.h"

namespace hailstone::plugin {
namespace {

constexpr std::string_view kNonRof2 = "non_rof2";
constexpr std::string_view kMovement = "movement_automation";
constexpr std::string_view kHelpers = "command_helpers";
constexpr std::string_view kAddons = "addon_commands";

struct ExtendedCommand {
    std::string_view name, description, group = {}, alias = {};
};

constexpr ExtendedCommand kExtended[] = {
    {"/abilitybook", "Open the Ability Book or toggle autoskill."},
    {"/afollow", "Follow a target or spawn along its path.", kMovement},
    {"/beep", "Play the Beep sound, or a game sound by name.", kHelpers},
    {"/camera", "Change camera mode and settings."},
    {"/chattabs", "Manage tabs and windows in chat."},
    {"/click", "Left- or right-click your target as the mouse would."},
    {"/ctrlkey", "Run a command with Ctrl held for its clicks.", kHelpers},
    {"/debug_animinfo", "Inspect model and animation state."},
    {"/destroy", "Destroy the item on your cursor."},
    {"/destroycursor", "Destroy a cursor item after checking its ID."},
    {"/docommand", "Run another command.", kHelpers},
    {"/doortarget", "Target a door by id or name, or clear it."},
    {"/echo", "Print a line to your chat.", kHelpers},
    {"/exchange", "Move an item from your packs into a worn slot."},
    {"/face", "Turn toward a target, coordinate, or heading.", kMovement},
    {"/finditem", "Search items across your inventory.", {}, "/advi"},
    {"/import", "Import eligible socials and hot buttons."},
    {"/invoke", "Read a line's ${...} for its window methods.", kHelpers},
    {"/itemnotify", "Click an inventory slot, bag slot or item by name."},
    {"/layoutprofiles", "Open named layout profiles."},
    {"/keypress", "Tap, hold, or release a game binding.", kMovement},
    {"/lighting", "Switch lighting modes."},
    {"/loadcfg", "Run the commands in a .cfg file.", kHelpers},
    {"/lua", "Start, pause, stop, or list Lua scripts."},
    {"/luasocial", "Open the Lua social editor."},
    {"/makemevisible", "Drop your own invisibility."},
    {"/mapfilter", "Choose what the map shows: spawn filters, colours, and vanilla mode."},
    {"/memspell", "Memorize a spell from your book into a gem."},
    {"/moveto", "Walk directly to a position or spawn.", kMovement},
    {"/nav", "Walk a route using the navigation mesh.", kMovement},
    {"/nomodkey", "Run a command with no modifier keys for its clicks.", kHelpers},
    {"/noparse", "Run a command without macro expansion.", kHelpers},
    {"/notify", "Click, select or set a window's control, as MQ's /notify."},
    {"/perf", "Open the performance window."},
    {"/popcustom", "Show a line in the centre of the screen.", kHelpers},
    {"/popup", "Show a line in the centre of the screen.", kHelpers},
    {"/popupecho", "Show a line in the centre of the screen and in chat.", kHelpers},
    {"/plugin", "Manage compiled plugins."},
    {"/removeaura", "Remove one of your auras by name."},
    {"/removebuff", "Click off one of your or your pet's buffs by name."},
    {"/removelev", "Click off your levitation."},
    {"/removepetbuff", "Click off one of your pet's buffs by name."},
    {"/screenshot", "Save a screenshot."},
    {"/servermanagement", "Open the server policy editor when authorized."},
    {"/shiftkey", "Run a command with Shift held for its clicks.", kHelpers},
    {"/squelch", "Run a command without showing its chat output.", kHelpers},
    {"/stick", "Maintain distance and position relative to a target.", kMovement},
    {"/stowcursor", "Pack a cursor item after checking its ID."},
    {"/timed", "Run a command after a delay.", kHelpers},
    {"/unequip", "Move an item from a worn slot into your packs."},
    {"/web", "Open the in-game browser."},
};

constexpr std::pair<std::string_view, std::string_view> kStockAliases[] = {
    {"'", "/say"}, {":", "/emote"}, {"/me", "/emote"}, {"/msg", "/tell"}, {"/send", "/tell"},
    {"/marknpc", "/gmarknpc"}, {"/tgb", "/targetgroupbuff"}, {"/fb", "/facebook"},
    {"/twitter", "/tweet"}, {"/sdriver", "/adriver"},
};

constexpr std::string_view kCommonShortNames[] = {
    "/alt", "/autoinv", "/con", "/disc", "/g", "/inv", "/loc", "/r", "/t", "/wh",
};

constexpr std::pair<std::string_view, std::string_view> kStockDescriptions[] = {
    {"/afk", "Set an away message."}, {"/assist", "Target another character's target."},
    {"/attack", "Toggle auto attack."}, {"/auction", "Send a zone-wide auction message."},
    {"/autofire", "Toggle ranged auto attack."}, {"/autoinventory", "Put the cursor item away."},
    {"/autojoin", "Set the channels joined at login."}, {"/bandolier", "Manage equipment sets."},
    {"/bazaar", "Open the bazaar search window."}, {"/book", "Open or close the spell book."},
    {"/camp", "Camp or return to character select."}, {"/cast", "Cast a spell from a spell gem."},
    {"/channel", "Set the chat window's default channel."}, {"/consider", "Consider the selected target."},
    {"/consent", "Let another player drag your corpse."}, {"/corpsedrag", "Start dragging a corpse."},
    {"/discipline", "Activate a discipline."}, {"/disband", "Leave or disband a group."},
    {"/dismount", "Dismount."}, {"/doability", "Use a skill, ability or discipline."},
    {"/duel", "Challenge the selected player to a duel."}, {"/emote", "Send a custom emote."},
    {"/exit", "Quit the game."}, {"/facepick", "Open face customization."},
    {"/find", "Find a player or spawn."}, {"/follow", "Follow the selected player."},
    {"/friends", "Manage your friends list."}, {"/fsay", "Send a message to your fellowship."},
    {"/gsay", "Send a message to your group."}, {"/guildsay", "Send a message to your guild."},
    {"/hail", "Hail a target."}, {"/help", "Display command help."},
    {"/hidecorpses", "Set corpse visibility."}, {"/ignore", "Manage your ignore list."},
    {"/inspect", "Inspect the target."}, {"/invite", "Invite a player to your group."},
    {"/join", "Join a chat channel."}, {"/leave", "Leave a chat channel."},
    {"/location", "Show your coordinates."}, {"/log", "Turn chat logging on or off."},
    {"/loot", "Loot the selected corpse."}, {"/map", "Open the map."},
    {"/melody", "Start a bard song sequence."}, {"/netstats", "Show connection statistics."},
    {"/ooc", "Send an out-of-character message."}, {"/outputfile", "Write game data to a local text file."},
    {"/pet", "Issue a command to your pet."}, {"/played", "Show time played on this character."},
    {"/quit", "Quit the game."}, {"/random", "Roll a random number."},
    {"/reply", "Reply to the last tell."}, {"/rewind", "Ask the server to move you back when stuck."},
    {"/rsay", "Send a message to your raid."}, {"/safelock", "Lock or unlock the interface."},
    {"/say", "Speak to nearby players."}, {"/shout", "Shout to the zone."}, {"/sit", "Sit down."},
    {"/split", "Split coin with the group."}, {"/stand", "Stand up."}, {"/stopcast", "Stop casting."},
    {"/stopsocial", "Stop the running social."}, {"/target", "Select a target by name or spawn search."},
    {"/tell", "Send a private message."}, {"/time", "Show the game time."},
    {"/useitem", "Activate an inventory item."}, {"/who", "List players."},
    {"/xtarget", "Manage extended target slots."}, {"/yell", "Call for help."},
};

std::string normalized_name(std::string_view value) {
    const auto start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos) return {};
    value = value.substr(start, value.find_last_not_of(" \t") - start + 1);
    if (value == "'" || value == ":") return std::string(value);
    if (value == "/" || std::any_of(value.begin(), value.end(), [](unsigned char c) {
            return c <= 0x20 || c == 0x7f || c == ',' || c == ';' || c == '#';
        })) return {};
    std::string name(value.front() == '/' ? "" : "/");
    for (char c : value) name.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    return name;
}

struct Catalog {
    std::map<std::string, CommandCatalogEntry, std::less<>> entries;
    std::map<std::string, std::string, std::less<>> aliases;

    void alias(std::string_view name, std::string_view primary) {
        if (name == primary || !aliases.emplace(name, primary).second) return;
        const auto entry = entries.find(primary);
        if (entry != entries.end()) entry->second.aliases.emplace_back(name);
    }
};

const Catalog& catalog() {
    static const Catalog source = [] {
        Catalog c;
        for (const auto& cmd : game::slash_commands()) {
            std::string name = normalized_name(cmd.name);
            if (std::any_of(std::begin(kStockAliases), std::end(kStockAliases),
                            [&](const auto& entry) { return entry.first == name; })) continue;
            const auto* text = std::find_if(std::begin(kStockDescriptions), std::end(kStockDescriptions),
                                            [&](const auto& entry) { return entry.first == name; });
            std::string_view description;
            if (text != std::end(kStockDescriptions)) description = text->second;
            else if (cmd.category == 2 || game::social_for_handler(cmd.handler)) description = "Play a social emote.";
            else if (cmd.category == 3) description = "Guild command.";
            else if (cmd.category == 6) description = "Chat or channel command.";
            c.entries.try_emplace(name, CommandCatalogEntry{name, std::string(description), CommandOrigin::Stock,
                                                            {}, {}, cmd.restriction > 0});
        }
        for (const auto& [name, primary] : kStockAliases) c.alias(name, primary);
        for (const auto& entry : kExtended) {
            CommandCatalogEntry row{std::string(entry.name), std::string(entry.description), CommandOrigin::Extended,
                                    {}, {std::string(kNonRof2)}, entry.name == "/servermanagement"};
            if (!entry.group.empty()) row.groups.emplace_back(entry.group);
            c.entries.emplace(entry.name, std::move(row));
            if (!entry.alias.empty()) c.alias(entry.alias, entry.name);
        }
        for (std::string_view name : kCommonShortNames) {
            if (auto index = game::find_slash_command(name, false)) {
                std::string primary = normalized_name(game::slash_commands()[*index].name);
                if (auto it = c.aliases.find(primary); it != c.aliases.end()) primary = it->second;
                if (!c.entries.contains(name)) c.alias(name, primary);
            }
        }
        c.entries.emplace("/pause", CommandCatalogEntry{"/pause", "Wait before continuing a social.",
                                                        CommandOrigin::Stock, {}, {}, false});
        c.entries.emplace("/timer", CommandCatalogEntry{"/timer", "Start a social button cooldown.",
                                                        CommandOrigin::Stock, {}, {}, false});
        return c;
    }();
    return source;
}

bool in_list(std::span<const std::string> values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

std::string_view command_origin_text(CommandOrigin origin) {
    switch (origin) {
    case CommandOrigin::Stock: return "stock";
    case CommandOrigin::Extended: return "extended";
    case CommandOrigin::Addon: return "addon";
    }
    return {};
}

std::string canonical_command_name(std::string_view name) {
    std::string normalized = normalized_name(name);
    const auto& c = catalog();
    if (c.entries.contains(normalized)) return normalized;
    if (auto alias = c.aliases.find(normalized); alias != c.aliases.end()) return alias->second;
    if (auto index = game::find_slash_command(normalized, true)) {
        normalized = normalized_name(game::slash_commands()[*index].name);
        if (auto alias = c.aliases.find(normalized); alias != c.aliases.end()) return alias->second;
    }
    return normalized;
}

bool known_extended_command(std::string_view name) {
    const auto& c = catalog();
    std::string spelling = normalized_name(name);
    if (auto alias = c.aliases.find(spelling); alias != c.aliases.end()) spelling = alias->second;
    const auto entry = c.entries.find(spelling);
    return entry != c.entries.end() && entry->second.origin == CommandOrigin::Extended;
}

bool known_command_group(std::string_view name) {
    return name == kNonRof2 || name == kMovement || name == kHelpers || name == kAddons;
}

std::vector<CommandCatalogEntry> command_catalog(std::span<const std::string> registered) {
    const auto& c = catalog();
    std::vector<CommandCatalogEntry> rows;
    rows.reserve(c.entries.size() + registered.size());
    for (const auto& entry : c.entries) rows.push_back(entry.second);
    std::set<std::string> addons;
    for (const auto& name : registered) {
        std::string spelling = normalized_name(name);
        if (!spelling.empty() && !c.entries.contains(spelling) && !c.aliases.contains(spelling))
            addons.insert(std::move(spelling));
    }
    for (const auto& spelling : addons)
        rows.push_back({spelling, "Provided by an add-on.", CommandOrigin::Addon, {}, {std::string(kAddons)}, false});
    return rows;
}

CommandDecision CommandRules::decision(std::string_view name, std::optional<int> account_status) const {
    if (!enabled) return {true, "Command policy is off", std::nullopt};
    const auto canonical = canonical_command_name(name);
    if (in_list(block, canonical)) return {false, "Explicit block", std::nullopt};
    if (const auto rule = gm.find(canonical); rule != gm.end()) {
        const auto minimum = rule->second;
        if (!minimum) return {false, "Invalid GM status rule", std::nullopt};
        return {account_status && *account_status >= *minimum, "GM status rule", minimum};
    }
    if (in_list(allow, canonical)) return {true, "Explicit allowance", std::nullopt};
    const auto& entries = catalog().entries;
    const auto entry = entries.find(canonical);
    auto matches = [&](std::string_view group) {
        return entry == entries.end() ? group == kAddons : in_list(entry->second.groups, group);
    };
    if (auto it = std::find_if(block_groups.begin(), block_groups.end(), matches); it != block_groups.end())
        return {false, *it, std::nullopt};
    if (auto it = std::find_if(allow_groups.begin(), allow_groups.end(), matches); it != allow_groups.end())
        return {true, *it, std::nullopt};
    return {default_allow, "Default policy", std::nullopt};
}

} // namespace hailstone::plugin
