// The group finder's posts and searches (CGroupSearchWnd; game/group_search.h). The window models
// (app/lua_ui_group_search.cpp) edit the fields kept here; the code below is the original's button handlers.

#include <algorithm>
#include <cctype>

#include "game/group_search.h"
#include "net/game_connection.h"
#include "net/opcodes.h"
#include "net/social_packets.h"

namespace hailstone::net {

namespace {

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string comments63(const std::string& s) { return s.substr(0, 63); } // _strncpy(.., 0x3f)

} // namespace

void GameConnection::group_search_prepare() {
    auto& gs = group_search_;
    const game::Spawn* me = world_.player();
    if (gs.prepared || !me) return;
    gs.prepared = true;
    // sub_00678a50 (the player page) and sub_00677d30 (the group page): both ranges from GroupSearch_DefaultLevels,
    // three digits; every class box checked; Show Matches checked.
    auto lv = game::group_search_default_levels(static_cast<int>(me->level), true);
    gs.player_min = gs.group_min = std::to_string(lv[0]);
    gs.player_max = gs.group_max = std::to_string(lv[1]);
    gs.roles.fill(true);
    gs.players_show_all = gs.groups_show_all = false;
}

void GameConnection::send_lfg(const LfgCommand& v) {
    // Send_OP_LFGCommand @0x5191c0: the local player's LFG flag (its name sprite) follows every send.
    if (state_ != ConnectionState::InZone) return;
    social_.lfg = v.on;
    send_zone(encode(v));
}

void GameConnection::group_search_error(int code) {
    // GroupSearch_ShowError @0x677ce0: ConfirmationDialog(0, 0xb, text, flags 1 (OK), 15000 ms).
    std::uint32_t id = game::group_search_error_eqstr(code);
    if (!id) return;
    Confirmation c;
    c.id = kConfirmNotice;
    c.text = strings_.format(id, std::vector<std::string>{});
    c.timeout_ms = 15000;
    c.ok_only = true;
    ask(std::move(c));
}

void GameConnection::group_search_post_player(bool update) {
    group_search_prepare();
    auto& gs = group_search_;
    // GroupSearch_CheckPlayerFields @0x6785b0, then 0x6783b0: the record from the player page (comments cut to
    // 63), on, the match filter from the filters window's GSFW_ExclusionSearchCheckbox.
    int from = game::group_search_int(gs.player_min), to = game::group_search_int(gs.player_max);
    if (!game::group_search_range_ok(from, to)) return group_search_error(1);
    LfgCommand c;
    c.comments = comments63(gs.player_comments);
    c.from_level = static_cast<std::uint32_t>(from);
    c.to_level = static_cast<std::uint32_t>(to);
    c.on = true;
    c.match_filter = gs.filters.exclusive;
    gs.lfg = c;
    send_lfg(c);
    // Posting hides the post button, disables Update and enables LFG off; updating only disables Update.
    // Both follow from `lfg` here (app/lua_ui_group_search.cpp).
    if (!update) gs.lfg_remove_checked = false;
}

void GameConnection::group_search_remove_player() {
    // 0x678250: a zeroed record, off; LFG off disabled and checked, Update hidden, the post shown.
    auto& gs = group_search_;
    gs.lfg = LfgCommand{};
    send_lfg(gs.lfg);
    gs.lfg_remove_checked = true;
}

void GameConnection::group_search_post_group(bool update) {
    group_search_prepare();
    auto& gs = group_search_;
    // 0x679d30: a member who is not the group's leader may not post the group (6715); Update (0x679e00)
    // skips that check.
    if (!update && group_.active && !group_.is_leader(character_)) return group_search_error(3);
    // GroupSearch_CheckGroupFields @0x679370: the range (1), then at least one class (2).
    int from = game::group_search_int(gs.group_min), to = game::group_search_int(gs.group_max);
    std::uint32_t classes = game::group_search_class_mask(gs.roles);
    if (!game::group_search_range_ok(from, to)) return group_search_error(1);
    if (classes == 0) return group_search_error(2);
    // 0x6791f0: the record from the group page.
    LfpCommand c;
    c.comments = comments63(gs.group_comments);
    c.from_level = static_cast<std::uint32_t>(from);
    c.to_level = static_cast<std::uint32_t>(to);
    c.classes = classes;
    c.on = true;
    c.match_filter = gs.filters.exclusive;
    gs.lfp = c;
    if (state_ == ConnectionState::InZone) send_zone(encode(c));
}

void GameConnection::group_search_remove_group() {
    // 0x678310: a zeroed record, off.
    auto& gs = group_search_;
    gs.lfp = LfpCommand{};
    if (state_ == ConnectionState::InZone) send_zone(encode(gs.lfp));
}

bool GameConnection::group_search_player_changed() const {
    const auto& gs = group_search_;
    // 0x6786a0: the comments (strcmp with the record's 63), or either level, differ from the record's.
    return gs.lfg.comments != comments63(gs.player_comments) ||
           static_cast<int>(gs.lfg.from_level) != game::group_search_int(gs.player_min) ||
           static_cast<int>(gs.lfg.to_level) != game::group_search_int(gs.player_max);
}

bool GameConnection::group_search_group_changed() const {
    const auto& gs = group_search_;
    // 0x679480: as above, and the class mask.
    return gs.lfp.comments != comments63(gs.group_comments) ||
           static_cast<int>(gs.lfp.from_level) != game::group_search_int(gs.group_min) ||
           static_cast<int>(gs.lfp.to_level) != game::group_search_int(gs.group_max) ||
           gs.lfp.classes != game::group_search_class_mask(gs.roles);
}

void GameConnection::group_search_query_players() {
    group_search_prepare();
    auto& gs = group_search_;
    // HandleQueryingForPlayers @0x679050: Show All asks for levels 1..100 of every class (0xffffffff) and keeps the
    // results unfiltered; else the group page's range and classes, checked (6713 / 6714).
    int from = 1, to = 100;
    std::uint32_t classes = 0xffffffffu;
    if (gs.players_show_all) {
        gs.players_query_all = true;
    } else {
        from = game::group_search_int(gs.group_min);
        to = game::group_search_int(gs.group_max);
        classes = game::group_search_class_mask(gs.roles);
        gs.players_query_all = false;
        if (!game::group_search_range_ok(from, to)) return group_search_error(1);
        if (classes == 0) return group_search_error(2);
    }
    if (state_ == ConnectionState::InZone)
        send_zone(encode_lfg_matches_request(static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(to), classes));
    gs.last_query_ms = eq_time();
}

void GameConnection::group_search_query_groups() {
    group_search_prepare();
    auto& gs = group_search_;
    // 0x678ed0: Show All 1..100 unfiltered, else the player page's range (6713).
    int from = 1, to = 100;
    if (gs.groups_show_all) {
        gs.groups_query_all = true;
    } else {
        from = game::group_search_int(gs.player_min);
        to = game::group_search_int(gs.player_max);
        gs.groups_query_all = false;
        if (!game::group_search_range_ok(from, to)) return group_search_error(1);
    }
    if (state_ == ConnectionState::InZone)
        send_zone(encode_lfp_matches_request(static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(to)));
    gs.last_query_ms = eq_time();
}

void GameConnection::group_search_joined_group() {
    // 0x678550 (0x443670: in a group and its leader).
    bool leader = group_.active && group_.is_leader(character_);
    group_search_remove_player();
    if (!leader || group_.count() == game::kGroupSlots) group_search_remove_group();
}

bool GameConnection::group_search_filtered(const std::string& name, std::int32_t guild) const {
    // CGuild::GetGuildName: "Unknown Guild" for an id it does not know (and asks the server for it).
    std::string gname = guild_.name_of(static_cast<std::uint32_t>(guild));
    if (gname.empty()) gname = "Unknown Guild";
    return game::group_search_filtered(
        group_search_.filters, name, gname, [this](const std::string& n) { return is_ignored(n); },
        [this](const std::string& n) {
            return std::any_of(social_.friends.begin(), social_.friends.end(), [&](const std::string& f) { return iequals(f, n); });
        });
}

void GameConnection::group_search_toggle_from_group_window() {
    auto& gs = group_search_;
    if (gs.window_open) {
        gs.window_open = false;
        return;
    }
    gs.info_page = group_.active && group_.is_leader(character_) ? 1 : 0;
    gs.window_open = true;
}

bool GameConnection::handle_group_search(const AppPacket& p) {
    auto& gs = group_search_;
    switch (from_wire(p.opcode)) {
    case Op::LFGGetMatchesResponse: {
        // Handle_OP_LFGGetMatchesResponse @0x4b7a30: the list cleared (0x677370), each entry through
        // CGroupSearchWnd_AddLfgMatchRow, which drops the filtered ones unless the query was Show All.
        auto v = decode_lfg_matches(p);
        if (!v) return true;
        gs.players.clear();
        for (auto& m : *v)
            if (gs.players_query_all || !group_search_filtered(m.name, m.guild)) gs.players.push_back(std::move(m));
        ++gs.results_serial;
        return true;
    }
    case Op::LFPGetMatchesResponse: {
        // Handle_OP_LFPGetMatchesResponse @0x4b7b40 → CGroupSearchWnd_AddLfpMatchRow: by the leader (member 0).
        auto v = decode_lfp_matches(p);
        if (!v) return true;
        gs.groups.clear();
        for (auto& m : *v) {
            const std::string leader = m.members.empty() ? std::string() : m.members[0].name;
            std::int32_t guild = m.members.empty() ? 0 : m.members[0].guild;
            if (gs.groups_query_all || !group_search_filtered(leader, guild)) gs.groups.push_back(std::move(m));
        }
        ++gs.results_serial;
        return true;
    }
    default:
        return false;
    }
}

} // namespace hailstone::net
