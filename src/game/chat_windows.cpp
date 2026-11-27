#include "game/chat_windows.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace hailstone::game {

namespace {

// GetChannelFromColor's table for the colours 0x100..0x15e: each channel with the colours that go to it. A colour
// not listed (and any colour outside the range) goes to kChatChannelOther.
constexpr std::int32_t kFirstColor = 0x100, kLastColor = 0x15e;
struct ChannelColors {
    std::uint8_t channel;
    std::array<std::int32_t, 4> colors; // 0 ends the list
};
constexpr ChannelColors kColorsByChannel[] = {
    {0, {0x100, 0x133}},
    {1, {0x101, 0x134}},
    {2, {0x102, 0x135}},
    {3, {0x147}},
    {4, {0x103, 0x136, 0x159, 0x15d}},
    {5, {0x104, 0x137}},
    {6, {0x105, 0x138}},
    {7, {0x106, 0x139}},
    {8, {0x107, 0x13a, 0x15c}},
    {9, {0x109}},
    {10, {0x108}},
    {11, {0x10e}},
    {12, {0x123, 0x13b}},
    {13, {0x124, 0x13c}},
    {14, {0x125, 0x13d}},
    {15, {0x126, 0x13e}},
    {16, {0x127, 0x13f}},
    {17, {0x128, 0x140}},
    {18, {0x129, 0x141}},
    {19, {0x12a, 0x142}},
    {20, {0x12b, 0x143}},
    {21, {0x12c, 0x144}},
    {23, {0x10b}},
    {24, {0x10a}},
    {25, {0x10c}},
    {26, {0x117}},
    {27, {0x118}},
    {28, {0x115}},
    {29, {0x116}},
    {30, {0x12d}},
    {31, {0x10f}},
    {32, {0x12f}},
    {33, {0x130}},
    {34, {0x131}},
    {35, {0x132}},
    {36, {0x120}},
    {37, {0x121}},
    {38, {0x12e}},
    {39, {0x11c}},
    {40, {0x11b}},
    {41, {0x14d}},
    {42, {0x11f}},
    {43, {0x148}},
    {44, {0x14b}},
    {45, {0x14c}},
    {46, {0x149}},
    {47, {0x14e}},
    {49, {0x14f}},
    {50, {0x119}},
    {51, {0x150}},
    {52, {0x151}},
    {53, {0x152}},
    {54, {0x156}},
    {55, {0x15a}},
    {56, {0x15e}},
};

constexpr std::array<std::uint8_t, kLastColor - kFirstColor + 1> channel_from_color_table() {
    std::array<std::uint8_t, kLastColor - kFirstColor + 1> table{};
    for (auto& channel : table) channel = static_cast<std::uint8_t>(kChatChannelOther);
    for (const ChannelColors& row : kColorsByChannel)
        for (std::int32_t color : row.colors)
            if (color != 0) table[static_cast<std::size_t>(color - kFirstColor)] = row.channel;
    return table;
}
constexpr auto kChannelFromColor = channel_from_color_table();

using M = ChatFilterMenu;
constexpr ChatFilterItem kFilterItems[] = {
    // the Filters menu (+0x180), in InitContextMenu's order
    {M::Filters, 0, 6630},   // Say
    {M::Filters, 1, 6631},   // Tell
    {M::Filters, 2, 6632},   // Group
    {M::Filters, 3, 5113},   // Raid
    {M::Filters, 4, 6633},   // Guild
    {M::Filters, 5, 6634},   // OOC
    {M::Filters, 6, 6635},   // Auction
    {M::Filters, 7, 6636},   // Shout
    {M::Filters, 8, 6637},   // Emotes
    {M::Filters, 49, 5870},  // System Messages
    // Hailstone: the plugins' and scripts' own lines (kChatChannelCode), after System Messages
    {M::Filters, kChatChannelCode, 0, "Plugins and Scripts"},
    // (then the Chat Channel, Melee and Spells submenus)
    {M::Filters, 11, 6641},  // Skills
    {M::Filters, 42, 3343},  // Randoms
    {M::Filters, 22, 6642},  // Other
    {M::Filters, 47, 5871},  // Experience Messages
    {M::Filters, 50, 13950}, // /who
    {M::Filters, 52, 6133},  // Pet Responses
    {M::Filters, 53, 6152},  // Item Speech
    {M::Filters, 54, 4588},  // Fellowship Messages
    {M::Filters, 55, 6361},  // Mercenary Messages
    {M::Filters, 56, 6447},  // PvP Messages
    // Chat Channel (+0x1a4): one item per joined channel (ids 0x70 + k)
    {M::ChatChannel, 12, 0}, {M::ChatChannel, 13, 0}, {M::ChatChannel, 14, 0}, {M::ChatChannel, 15, 0},
    {M::ChatChannel, 16, 0}, {M::ChatChannel, 17, 0}, {M::ChatChannel, 18, 0}, {M::ChatChannel, 19, 0},
    {M::ChatChannel, 20, 0}, {M::ChatChannel, 21, 0},
    // Melee (+0x194)
    {M::Melee, 9, 15014},   // Your Hits
    {M::Melee, 23, 15015},  // Your Misses
    {M::Melee, 24, 15016},  // You Being Hit
    {M::Melee, 25, 15017},  // You Being Missed
    {M::Melee, 26, 15018},  // Others Hits
    {M::Melee, 27, 15019},  // Others Misses
    {M::Melee, 43, 3344},   // Pet messages
    {M::Melee, 44, 5868},   // Pet Rampage/Flurry
    {M::Melee, 45, 5869},   // Pet Criticals
    {M::Melee, 30, 15020},  // Critical Hits
    {M::Melee, 31, 15021},  // Disciplines
    {M::Melee, 32, 15022},  // Melee Warnings
    {M::Melee, 33, 15023},  // NPC Rampage
    {M::Melee, 34, 15024},  // NPC Flurry
    {M::Melee, 35, 15025},  // NPC Enrage
    {M::Melee, 28, 15026},  // My Death
    {M::Melee, 29, 15027},  // Others Death
    {M::Melee, 46, 3376},   // Damage Shield Reports
    // Spells (+0x19c)
    {M::Spells, 10, 15028}, // Mine
    {M::Spells, 36, 15029}, // Others
    {M::Spells, 37, 15030}, // Failures
    {M::Spells, 38, 15031}, // Criticals
    {M::Spells, 39, 15032}, // Worn Off
    {M::Spells, 40, 15033}, // Non Melee Hits
    {M::Spells, 41, 5867},  // Focus Effects
    {M::Spells, 51, 6132},  // Pet Spells

    // Hailstone (a deliberate difference, docs/mechanics.md "Extended chat routing"): the extended routes 57..107,
    // reached only by the colours 355..408 a stock EQEmu server never sends, in their own Extended (server) submenu
    // after the original menu. Their names are Hailstone's own (no eqstr; a RoF2 string file has none for them).
    // Routes 82, 83, 86, 87, 90..94 and 96 are kept free (ChannelMap keeps their numbers) but not in the menu: the
    // list they come from gives them colours below 355 (2, 4, 13, 15, 274, 283, 285, 286, 290, 348, 349), and those
    // keep RoF2's routes here, so nothing reaches them (docs/unknowns.md "Chat").
    {M::Extended, 57, 0, "Your flurries"},
    {M::Extended, 58, 0, "Debug output"},
    {M::Extended, 59, 0, "NPC deaths"},
    {M::Randoms, 60, 0, "Other players' rolls"},
    {M::Randoms, 61, 0, "Group and raid rolls"},
    {M::EnvironmentalDamage, 62, 0, "Your environmental damage"},
    {M::EnvironmentalDamage, 63, 0, "Others' environmental damage"},
    {M::DamageShields, 64, 0, "Damage shields while defending"},
    {M::DamageShields, 65, 0, "Others' damage shields"},
    {M::Extended, 66, 0, "Events"},
    {M::Extended, 67, 0, "Overwritten detrimental spells"},
    {M::Extended, 68, 0, "Overwritten beneficial spells"},
    {M::Other, 69, 0, "Unavailable commands"},
    {M::Other, 70, 0, "Combat ability reuse"},
    {M::Other, 71, 0, "Alternate ability reuse"},
    {M::Extended, 72, 0, "Destroyed items"},
    {M::Extended, 73, 0, "Your auras"},
    {M::Extended, 74, 0, "Others' auras"},
    {M::Healing, 75, 0, "Your healing"},
    {M::Healing, 76, 0, "Others' healing"},
    {M::DamageOverTime, 77, 0, "Your damage over time"},
    {M::DamageOverTime, 78, 0, "Others' damage over time"},
    {M::Extended, 79, 0, "Bard songs"},
    {M::DirectDamage, 80, 0, "Others' direct damage"},
    {M::Extended, 81, 0, "Spell emotes"},
    {M::Extended, 84, 0, "Taunt messages"},
    {M::Extended, 85, 0, "Others' disciplines"},
    {M::Extended, 88, 0, "Food and drink"},
    {M::Extended, 89, 0, "Raid victories"},
    {M::Extended, 95, 0, "Consider messages"},
    {M::DirectDamage, 97, 0, "Others' direct damage criticals"},
    {M::DamageOverTime, 98, 0, "Your damage over time criticals"},
    {M::DamageOverTime, 99, 0, "Others' damage over time criticals"},
    {M::DamageOverTime, 100, 0, "Damage over time received"},
    {M::Healing, 101, 0, "Healing received"},
    {M::Healing, 102, 0, "Your critical healing"},
    {M::Healing, 103, 0, "Others' critical healing"},
    {M::Extended, 104, 0, "Others' melee criticals"},
    {M::Extended, 105, 0, "Others' spell failures"},
    {M::Extended, 106, 0, "Client Messages"},
    {M::Extended, 107, 0, "Alerts"},
};

constexpr ChatFilterGroup kFilterGroups[] = {
    {M::ChatChannel, M::Filters, kStrFilterChatChannel, "Chat Channel"},
    {M::Melee, M::Filters, kStrFilterMelee, "Melee"},
    {M::Spells, M::Filters, kStrFilterSpells, "Spells"},
    // Hailstone: the extended routes' submenus
    {M::Extended, M::Filters, 0, "Extended (server)"},
    {M::DirectDamage, M::Extended, 0, "Direct Damage"},
    {M::DamageOverTime, M::Extended, 0, "Damage Over Time"},
    {M::Healing, M::Extended, 0, "Healing"},
    {M::DamageShields, M::Extended, 0, "Damage Shields"},
    {M::EnvironmentalDamage, M::Extended, 0, "Environmental Damage"},
    {M::Randoms, M::Extended, 0, "Random Rolls"},
    {M::Other, M::Extended, 0, "Other Messages"},
};

constexpr ChatDefaultItem kDefaultItems[] = {
    {0xc8, 8, 6630, true},     // Say
    {0xc9, 7, 6631, false},    // Tell (EnableMenuItem(item, 0) right after it is added)
    {0xca, 2, 6632, true},     // Group
    {0xd9, 0xf, 5113, true},   // Raid
    {0xcb, 0, 6633, true},     // Guild
    {0xcc, 5, 6634, true},     // OOC
    {0xcd, 4, 6635, true},     // Auction
};

} // namespace

int chat_channel_of_color(std::int32_t color) {
    if (color >= kFirstColor && color <= kLastColor)
        return kChannelFromColor[static_cast<std::size_t>(color - kFirstColor)];
    // Hailstone (a deliberate difference): the extended colours, which only a server that uses them sends (stock
    // EQEmu's highest is 340). The original sends every colour outside 0x100..0x15e to channel 22; here so do the
    // colours 351..355, 381 and 384..387, and any other.
    if (color >= 356 && color <= 380) return color - 299; // 57..81
    if (color >= 390 && color <= 396) return 95;          // the seven consider colours
    if (color >= 398 && color <= 408) return color - 301; // 97..107
    switch (color) {
    case 382: return 84;
    case 383: return 85;
    case 388: return 88;
    case 389: return 89;
    case 397: return 40; // Non Melee Hits
    default: return kChatChannelOther;
    }
}

std::span<const ChatFilterItem> chat_filter_items() { return kFilterItems; }
std::span<const ChatFilterGroup> chat_filter_groups() { return kFilterGroups; }

std::uint32_t chat_filter_all_eqstr(ChatFilterMenu menu) {
    switch (menu) {
    case M::Filters: return 6040;     // All normal channels will now filter to this window.
    case M::Melee: return 6041;       // All melee channels ...
    case M::Spells: return 6042;      // All spell channels ...
    case M::ChatChannel: return 6043; // All chat server channels ...
    default: return 0;
    }
}

std::span<const ChatDefaultItem> chat_default_items() { return kDefaultItems; }

std::uint32_t hit_category_eqstr(int category) { return 15014 + static_cast<std::uint32_t>(category); }

std::uint32_t hit_mode_eqstr(int category, int mode) {
    // the miss categories (1, 3, 5) say Miss Only for mode 2; ALL's submenu (6) Number Only
    if (mode == 2) return category >= 0 && category < 6 && (category & 1) ? 15037 : 15036;
    return mode == 1 ? 15035 : 15034;
}

std::uint32_t hit_mode_all_eqstr(int mode) { return 6044 + static_cast<std::uint32_t>(mode); }

std::string_view spell_hit_category_name(int category) {
    switch (category) {
    case 0: return "Direct Spell Damage";
    case 1: return "Damage Over Time";
    case 2: return "Healing";
    case 3: return "Healing Over Time";
    default: return {};
    }
}

std::string tell_menu_text(std::string name, const std::string& tell_word) {
    char c = '/';
    if (name.find('.') != std::string::npos) {
        if (!name.empty() && name[0] == '.') name.erase(0, 1);
        c = ';';
    }
    if (!name.empty() && (name[0] == '[' || name[0] == ';')) {
        c = name[0];
        name.erase(0, 1);
    }
    return std::format("{}{} {} ", c, tell_word, name);
}

bool cycle_last_tell(const std::vector<std::string>& names, int& index, bool newer) {
    constexpr int kLast = 49; // 50 entries
    const int n = static_cast<int>(std::min<std::size_t>(names.size(), kLast + 1));
    auto set = [&](int i) { return i >= 0 && i < n && !names[static_cast<std::size_t>(i)].empty(); };
    if (!set(0)) return false;
    if (newer) {
        if (index > 0) {
            --index;
            return true;
        }
        // round to the last name: entry 49, else the first one set below it
        for (index = kLast; index >= 0; --index)
            if (set(index)) return true;
    } else if (index < kLast && set(index + 1)) {
        ++index;
        return true;
    }
    index = 0;
    return true;
}

std::string reply_target_text(const std::string& name, const std::string& rtarget_word) {
    std::string_view n = name;
    if (!n.empty() && (n[0] == ';' || n[0] == '[')) n.remove_prefix(1);
    return std::format("{} {}", rtarget_word, n);
}

CycleReply cycle_reply(std::string_view input, const std::vector<std::string>& names, int& index, bool newer,
                       const std::string& tell_word, const std::string& rtarget_word) {
    CycleReply out;
    // CXStr::Mid(1) of the input, then CompareN against each word past its first character (its slash)
    std::string_view rest = input.empty() ? input : input.substr(1);
    auto starts = [&](const std::string& word) {
        std::string_view w = word.empty() ? std::string_view() : std::string_view(word).substr(1);
        return rest.size() >= w.size() && rest.substr(0, w.size()) == w;
    };
    const bool rtarget = starts(rtarget_word);
    if (!rtarget && !starts(tell_word)) return out;
    if (!cycle_last_tell(names, index, newer)) out.messages.push_back(kStrNoTellsToReply);
    const std::string* name =
        index >= 0 && static_cast<std::size_t>(index) < names.size() && !names[static_cast<std::size_t>(index)].empty()
            ? &names[static_cast<std::size_t>(index)]
            : nullptr;
    if (!name) {
        out.messages.push_back(kStrNoTellsToTarget);
        return out;
    }
    out.text = rtarget ? reply_target_text(*name, rtarget_word)
                       : tell_menu_text(*name, tell_word.empty() ? tell_word : tell_word.substr(1));
    return out;
}

} // namespace hailstone::game
