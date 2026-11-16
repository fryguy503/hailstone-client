#pragma once

// The group finder's rules (CGroupSearchWnd "GroupSearchWnd" @0x676470..0x67aa90, CGroupSearchFiltersWnd
// "GroupSearchFiltersWnd" @0x674bd0..0x676400 and the
// unnamed helpers between them). The window state lives on the connection (GameConnection::group_search()), the
// window models in app/lua_ui_group_search.cpp. Names given here to the original's unnamed functions:
//  - ClassSortOrder_GetClassId @0x677c40: the "Want classes" checkbox index (the XML's alphabetical order, Brd .. Wiz)
//    to the class id.
//  - GroupSearch_ClassMask @0x678830: the checked boxes as class bits (1 << class id).
//  - GroupSearch_DefaultLevels @0x6779f0: the level range a new window fills in, level - 5 .. level + 5
//    within 1 .. 100.
//  - GroupSearch_ShowError @0x677ce0: 1 6713 (bad range), 2 6714 (no class), 3 6715 (not the leader), each
//    in ConfirmationDialog(0, 0xb, text, flags 1 (OK), 15000 ms).
//  - GroupSearch_CheckPlayerFields @0x6785b0, GroupSearch_CheckGroupFields @0x679370: the posting checks (0
//    good, 1 the range, 2 no class).
//  - CGroupSearchWnd_UpdateQueryButtonText @0x677690: the 15 s lock on both Get Matches buttons.
//  - CGroupSearchWnd_IsRowFiltered @0x6761a0 with CGroupSearchFiltersWnd_IsExcludedGuild @0x675710 and
//    CGroupSearchFiltersWnd_IsDesiredGuild @0x6757e0: the additional filters on a result.
//  - CGroupSearchFiltersWnd::SaveIni @0x6758b0 and LoadIni @0x675b70 (sub_): [ADDITIONALFILTERS] in the character
//    ini (0x761d60, the file CharacterIni_GetString reads): FilterByPlayerMode (0 none, 1 the ignore list, 2
//    friends only; default 1), FilterByGuildMode (0 none, 1 excluded guilds, 2 desired guilds; default 0),
//    FilterByExclusiveSearch (default 0), GUILDEXCLUDED<n> / GUILDDESIRED<n> from 0 up to the first empty one (the
//    saver writes one empty key past the list's end).

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone { class IniFile; }

namespace hailstone::game {

// The group info page's class boxes, GIP_<abbr>Checkbox (the strings @0x9e9714.. sub_00676670 formats) and the
// class each one stands for (ClassSortOrder_GetClassId).
inline constexpr std::array<std::string_view, 16> kGroupSearchClassBoxes{
    "Brd", "Bst", "Ber", "Clr", "Dru", "Enc", "Mag", "Mnk", "Nec", "Pal", "Ran", "Rog", "Shd", "Shm", "War", "Wiz"};
std::uint32_t group_search_class_id(int index); // 0 outside 0..15
// GroupSearch_ClassMask over the 16 boxes (index order).
std::uint32_t group_search_class_mask(const std::array<bool, 16>& checked);
// sub_00678a50's mask of all 16 classes (the groups list says "ANY" for it).
std::uint32_t group_search_all_classes();

// GroupSearch_DefaultLevels: {from, to}; with no player {1, 100}.
std::array<int, 2> group_search_default_levels(int level, bool have_player);
// The range checks (HandleQueryingForPlayers, 0x678ed0, the posting checks): to >= 1, from <= 100, from <= to.
bool group_search_range_ok(int from, int to);
// GroupSearch_ShowError's text id for an error code; 0 for none.
std::uint32_t group_search_error_eqstr(int code);
// CXStr::ConvertToInt on an edit box's text (atoi).
int group_search_int(std::string_view text);

// CGroupSearchWnd_UpdateQueryButtonText: the seconds left of the 15 s lock after the last query
// ((now - last) / 1000 unsigned, 15 - that while under 15, else 0).
int group_search_query_lock(std::uint32_t now_ms, std::uint32_t last_ms);
inline constexpr int kGroupSearchQueryLockSeconds = 15;
// OnProcessFrame's refresh period (the info labels and the lock text).
inline constexpr std::uint32_t kGroupSearchFrameMs = 400;

// The additional filters window's settings (CGroupSearchFiltersWnd).
struct GroupSearchFilters {
    int player_mode = 1; // 0 none, 1 GSFW_UseIgnoreListCheckbox, 2 GSFW_UseFriendsListCheckbox
    int guild_mode = 0;  // 0 none, 1 GSFW_ExcludedGuildsCheckbox, 2 GSFW_DesiredGuildsCheckbox
    bool exclusive = false; // GSFW_ExclusionSearchCheckbox: the posts' match filter byte
    std::vector<std::string> excluded, desired; // the two guild lists

    void load(const IniFile& ini); // LoadIni
    void save(IniFile& ini) const; // SaveIni
    // The guild Add buttons (sub_00675350 / sub_006754d0): the combo's name unless the list has it (strcmp).
    static void add_guild(std::vector<std::string>& list, const std::string& name);
};

// CGroupSearchWnd_IsRowFiltered: a result for `name` in `guild` (CGuild::GetGuildName's text) is dropped. Each
// active rule overwrites the answer of the one before (the original's order): the ignore list (BeingIgnored), friends
// only (not on the friends list, 0x516ba0: case ignored), excluded guilds (the guild listed, strcmp), desired
// guilds (the guild not listed).
bool group_search_filtered(const GroupSearchFilters& f, const std::string& name, const std::string& guild,
                           const std::function<bool(const std::string&)>& ignored,
                           const std::function<bool(const std::string&)>& is_friend);

} // namespace hailstone::game
