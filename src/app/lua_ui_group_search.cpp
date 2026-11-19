// The group finder: GroupSearchWnd (CGroupSearchWnd @0x679620) and GroupSearchFiltersWnd (CGroupSearchFiltersWnd
// @0x675e60), over the fields, posts and results the connection keeps (net/game_connection_group_search.cpp) and
// the rules in game/group_search.h.

#include <algorithm>
#include <cstdlib>
#include <format>
#include <map>
#include <string>
#include <vector>

#include "app/lua_ui_windows.h"
#include "game/chat_text.h"
#include "game/eq_names.h"
#include "game/group_search.h"
#include "game/zone_names.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

std::string fmt(const net::GameConnection& c, std::uint32_t id, std::vector<std::string> args = {}) {
    return c.strings().format(id, std::move(args));
}

// CEverQuest::GetClassThreeLetterCode @0x5153c0: "WAR" .. "BER" (eqstr 3065.., the shadow knight and ranger swapped),
// "UNKNOWN CLASS" (2456) for any other id; the server's short name for a class it renamed (game/eq_names.h).
std::string class_code(const net::GameConnection& c, std::int32_t id) {
    if (id >= 1 && id <= 16)
        if (auto own = game::class_name_override(static_cast<std::uint32_t>(id), true)) return *own;
    static constexpr std::uint32_t kCodes[16] = {3065, 3066, 3067, 3069, 3068, 3070, 3071, 3072,
                                                 3073, 3074, 3075, 3076, 3077, 3078, 3079, 5810};
    return fmt(c, id >= 1 && id <= 16 ? kCodes[id - 1] : 2456);
}

// ProfanityFilter_CensorText: with the bad word filter on (mode 1) the comment's bad words are starred.
std::string censored(const net::GameConnection& c, const std::string& text) {
    return c.chat_filters().mode(game::kFilterBadWord) == 1 ? game::censor_bad_words(text) : text;
}

// GetFullZoneNameById, or "UNAVAILABLE" (2145) for an anonymous player.
std::string zone_text(const net::GameConnection& c, std::int32_t zone, bool anon) {
    return anon ? fmt(c, 2145) : game::zone_long_name(static_cast<std::uint32_t>(static_cast<std::uint16_t>(zone)));
}

// The three-digit level boxes and 63-character comment boxes (SetMaxChars 3 / 0x3f).
std::string cut(const std::string& text, std::size_t n) { return text.substr(0, n); }

int box_index(std::string_view id) {
    for (int i = 0; i < 16; ++i)
        if (id == std::format("GIP_{}Checkbox", game::kGroupSearchClassBoxes[static_cast<std::size_t>(i)])) return i;
    return -1;
}

// 0x677830: a double click on a result starts a tell to the name in its first column: "/tell <name> " (13974)
// in the active chat window's input, which takes the keyboard.
void start_tell(const net::GameConnection& c, const std::string& name) {
    if (!name.empty()) request_chat_focus(std::format("{} {} ", fmt(c, 13974), name));
}

// GroupSearchWnd. Two tab boxes: GSW_InfoPages ("I am a:", the player page 0 and the group page 1) and
// GSW_SearchPages ("Show me:", the players list 0 and the groups list 1). OnProcessFrame (every 400 ms while shown)
// refreshes the page's label (0x678000: 6709 with the level, class and "<guild>"; 0x677400: 6711 with the
// level and group size, kept while not grouped) and the lock on Get Matches (CGroupSearchWnd_UpdateQueryButtonText:
// "Get Matches (n)" and disabled for 15 s after a search). The buttons are the WndNotification handlers routed by
// CGroupSearchWnd_RouteListNotification @0x67a9c0 to the page that sent them (0x679b70, 0x67a6f0,
// 0x679c20, 0x67a870). A double click on a result row starts a tell (0x677830); a click on a group shows
// its members (red when the filters drop them) and the classes it wants (0x67a160, 0x679e30). The two
// result lists sort by a header click (CListWnd's sort; the window sets 0x440000): 0x679c20 / 0x67a870 answer
// notification 0x10 for the players' Level and the groups' # and Level with 0x677920 (the cells' atoi), the
// other columns by text.
class GroupSearchWindow : public StockWindow {
public:
    GroupSearchWindow() : StockWindow("GroupSearchWnd", false) {}

    bool shown(UiHost& host) override {
        if (!host.conn) return false;
        auto& gs = host.conn->group_search();
        if (gs.window_open && gs.info_page >= 0) {
            info_page_ = gs.info_page; // 0x676550: CTabWnd::SetPage, then Show
            gs.info_page = -1;
        }
        return gs.window_open;
    }
    void set_shown(UiHost& host, bool show) override {
        if (host.conn) host.conn->group_search().window_open = show;
    }

    void frame(UiHost& host) override {
        if (!host.conn) return;
        auto& c = *host.conn;
        c.group_search_prepare();
        const auto& gs = c.group_search();
        if (serial_ != gs.results_serial) { // 0x6772b0: a new groups reply clears the two detail lists
            serial_ = gs.results_serial;
            group_selected_ = -1;
            detail_ = -1;
            player_selected_ = -1;
        }
        std::uint32_t now = net::GameConnection::eq_time();
        if (!gs.window_open || now - last_frame_ < game::kGroupSearchFrameMs) return;
        last_frame_ = now;
        const game::Spawn* me = c.world().player();
        if (!me) return;
        // 0x678000.
        std::string guild = c.guild().in_guild() ? std::format("<{}>", c.guild().name_of(c.guild().guild_id)) : std::string();
        player_label_ = fmt(c, 6709, {std::to_string(me->level), c.class_desc(static_cast<std::int32_t>(me->class_id)), guild});
        // 0x677400: only in a group.
        if (c.group().active) group_label_ = fmt(c, 6711, {std::to_string(me->level), std::to_string(c.group().count())});
    }

    std::optional<int> tab_page(UiHost&, std::string_view id) override {
        if (id == "GSW_InfoPages") return info_page_;
        if (id == "GSW_SearchPages") return search_page_;
        return std::nullopt;
    }
    void set_tab_page(UiHost&, std::string_view id, int page) override {
        if (id == "GSW_InfoPages") info_page_ = page;
        else if (id == "GSW_SearchPages") search_page_ = page;
    }

    void child(UiHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        auto& c = *host.conn;
        const auto& gs = c.group_search();
        int lock = game::group_search_query_lock(net::GameConnection::eq_time(), gs.last_query_ms);
        if (id == "PIP_PlayerInfoLabel") out.text = player_label_;
        else if (id == "GIP_GroupInfoLabel") out.text = group_label_;
        else if (id == "PIP_CommentsEditbox") out.text = gs.player_comments;
        else if (id == "PIP_LevelRangeMinEditbox") out.text = gs.player_min;
        else if (id == "PIP_LevelRangeMaxEditbox") out.text = gs.player_max;
        else if (id == "GIP_CommentsEditbox") out.text = gs.group_comments;
        else if (id == "GIP_LevelRangeMinEditbox") out.text = gs.group_min;
        else if (id == "GIP_LevelRangeMaxEditbox") out.text = gs.group_max;
        else if (int i = box_index(id); i >= 0) out.checked = gs.roles[static_cast<std::size_t>(i)];
        // The post / Update / off buttons as 0x678df0, 0x678250, 0x6786a0 (the player's) and
        // 0x679d30, 0x678310, 0x679480 (the group's) leave them.
        else if (id == "PIP_PostMyInfoButton") {
            out.shown = out.enabled = !gs.lfg.on;
            out.checked = gs.lfg.on;
        } else if (id == "PIP_UpdateMyInfoButton") {
            out.shown = gs.lfg.on;
            out.enabled = gs.lfg.on && c.group_search_player_changed();
        } else if (id == "PIP_RemoveMyInfoButton") {
            out.enabled = gs.lfg.on;
            out.checked = gs.lfg_remove_checked;
        } else if (id == "GIP_PostOurInfoButton") out.shown = out.enabled = !gs.lfp.on;
        else if (id == "GIP_UpdateOurInfoButton") {
            out.shown = gs.lfp.on;
            out.enabled = gs.lfp.on && c.group_search_group_changed();
        } else if (id == "GIP_RemoveOurInfoButton") out.enabled = gs.lfp.on;
        else if (id == "PLP_ShowMatchesCheckbox") out.checked = !gs.players_show_all;
        else if (id == "PLP_ShowAllCheckbox") out.checked = gs.players_show_all;
        else if (id == "GLP_ShowMatchesCheckbox") out.checked = !gs.groups_show_all;
        else if (id == "GLP_ShowAllCheckbox") out.checked = gs.groups_show_all;
        else if (id == "PLP_RefreshPlayerButton" || id == "GLP_RefreshGroupsButton") {
            // The text the constructor read from the button (the XML's), with the seconds left.
            out.text = lock > 0 ? std::format("{} ({})", kGetMatches, lock) : std::string(kGetMatches);
            out.enabled = lock <= 0;
        } else if (id == "PLP_NumSearchResultsLabel") {
            out.text = gs.players.empty() ? std::string() : fmt(c, 6717, {std::to_string(gs.players.size())});
        } else if (id == "GLP_NumSearchResultsLabel") {
            out.text = gs.groups.empty() ? std::string() : fmt(c, 6717, {std::to_string(gs.groups.size())});
        }
    }

    bool set_text(UiHost& host, std::string_view id, const std::string& text) override {
        if (!host.conn) return false;
        auto& gs = host.conn->group_search();
        if (id == "PIP_CommentsEditbox") gs.player_comments = cut(text, 63);
        else if (id == "PIP_LevelRangeMinEditbox") gs.player_min = cut(text, 3);
        else if (id == "PIP_LevelRangeMaxEditbox") gs.player_max = cut(text, 3);
        else if (id == "GIP_CommentsEditbox") gs.group_comments = cut(text, 63);
        else if (id == "GIP_LevelRangeMinEditbox") gs.group_min = cut(text, 3);
        else if (id == "GIP_LevelRangeMaxEditbox") gs.group_max = cut(text, 3);
        else return false;
        return true;
    }

    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        auto& gs = c.group_search();
        int lock = game::group_search_query_lock(net::GameConnection::eq_time(), gs.last_query_ms);
        if (id == "PIP_PostMyInfoButton") c.group_search_post_player(false);
        else if (id == "PIP_UpdateMyInfoButton") c.group_search_post_player(true);
        else if (id == "PIP_RemoveMyInfoButton") c.group_search_remove_player();
        else if (id == "GIP_PostOurInfoButton") c.group_search_post_group(false);
        else if (id == "GIP_UpdateOurInfoButton") c.group_search_post_group(true);
        else if (id == "GIP_RemoveOurInfoButton") c.group_search_remove_group();
        else if (id == "PIP_AdditionalFiltersButton" || id == "GIP_AdditionalFiltersButton") gs.filters_open = true;
        else if (int i = box_index(id); i >= 0) gs.roles[static_cast<std::size_t>(i)] = !gs.roles[static_cast<std::size_t>(i)];
        else if (id == "GIP_SelectAllRolesButton") gs.roles.fill(true);   // 0x677580
        else if (id == "GIP_DeselectAllRolesButton") gs.roles.fill(false); // 0x6775b0
        else if (id == "PLP_ShowMatchesCheckbox") gs.players_show_all = false; // radio group FilterPlayerMatches
        else if (id == "PLP_ShowAllCheckbox") gs.players_show_all = true;
        else if (id == "GLP_ShowMatchesCheckbox") gs.groups_show_all = false; // FilterGroupMatches
        else if (id == "GLP_ShowAllCheckbox") gs.groups_show_all = true;
        else if (id == "PLP_RefreshPlayerButton") {
            if (lock <= 0) c.group_search_query_players();
        } else if (id == "GLP_RefreshGroupsButton") {
            if (lock <= 0) c.group_search_query_groups();
        } else return false;
        return true;
    }

    const ListData* list(UiHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& c = *host.conn;
        const auto& gs = c.group_search();
        if (id == "PLP_PlayerSearchResultList") {
            // CGroupSearchWnd_AddLfgMatchRow: name, level, class code, zone, comment.
            players_ = {};
            players_.columns = 5;
            players_.labels = {"Name", "Level", "Class", "Zone", "Comments"};
            players_.widths = {90, 40, 40, 80, 400};
            player_rows_.clear();
            for (const auto& m : gs.players) {
                player_rows_.push_back(player_rows_.size());
                players_.rows.push_back(ListRow{{m.name, std::to_string(m.level), class_code(c, m.class_),
                                                 zone_text(c, m.zone, m.anon != 0), censored(c, m.comments)}});
            }
            sort_rows(players_, player_rows_, player_sort_, [](int column) { return column == 1; });
            players_.selected = row_of(player_rows_, player_selected_);
            return &players_;
        }
        if (id == "GLP_PlayerSearchResultList") {
            // CGroupSearchWnd_AddLfpMatchRow: the leader, the member count as sent, the leader's level and zone, the
            // comment.
            groups_ = {};
            groups_.columns = 5;
            groups_.labels = {"Group Leader", "#", "Level", "Zone", "Comments"};
            groups_.widths = {90, 20, 40, 80, 400};
            group_rows_.clear();
            for (const auto& m : gs.groups) {
                const net::LfpMember* l = m.members.empty() ? nullptr : &m.members[0];
                group_rows_.push_back(group_rows_.size());
                groups_.rows.push_back(ListRow{{l ? l->name : std::string(), std::to_string(m.count),
                                                std::to_string(l ? l->level : 0), zone_text(c, l ? l->zone : 0, false),
                                                censored(c, m.comments)}});
            }
            sort_rows(groups_, group_rows_, group_sort_, [](int column) { return column == 1 || column == 2; });
            groups_.selected = row_of(group_rows_, group_selected_);
            return &groups_;
        }
        const net::LfpMatch* g = detail_ >= 0 && detail_ < static_cast<int>(gs.groups.size()) ? &gs.groups[static_cast<std::size_t>(detail_)] : nullptr;
        if (id == "GLP_GroupDetailList") {
            // 0x679e30: name, class code, level; the members the filters drop in red (0xffff0000).
            details_ = {};
            details_.columns = 3;
            details_.labels = {"Name", "Class", "Level"};
            details_.widths = {150, 150, 400};
            if (g)
                for (const auto& e : g->members) {
                    ListRow r{{e.name, class_code(c, e.class_), std::to_string(e.level)}};
                    if (c.group_search_filtered(e.name, e.guild)) r.color = argb_to_rgb(0xffff0000);
                    details_.rows.push_back(std::move(r));
                }
            details_.selected = detail_selected_ < static_cast<int>(details_.rows.size()) ? detail_selected_ : -1;
            return &details_;
        }
        if (id == "GLP_ClassesWantedList") {
            // 0x67a160: 6710 then " ANY" (6718) for all 16 classes, else " <code>" for each in the boxes' order.
            wanted_ = {};
            wanted_.columns = 1;
            wanted_.labels = {"Desired Classes"};
            wanted_.widths = {400};
            if (g) {
                std::string line = fmt(c, 6710);
                if (g->classes == game::group_search_all_classes()) line += " " + fmt(c, 6718);
                else
                    for (int i = 0; i < 16; ++i) {
                        std::uint32_t id_ = game::group_search_class_id(i);
                        if ((g->classes >> id_) & 1) line += " " + class_code(c, static_cast<std::int32_t>(id_));
                    }
                wanted_.rows.push_back(ListRow{{line}});
            }
            return &wanted_;
        }
        return nullptr;
    }

    bool list_click(UiHost& host, std::string_view id, int row, bool right, int) override {
        if (!host.conn || right) return false;
        const auto& gs = host.conn->group_search();
        if (id == "PLP_PlayerSearchResultList") player_selected_ = record_of(player_rows_, row);
        else if (id == "GLP_PlayerSearchResultList") {
            group_selected_ = record_of(group_rows_, row);
            // 0x67a160: the record whose leader has the row's name (the first such).
            detail_ = -1;
            detail_selected_ = -1;
            if (group_selected_ >= 0 && group_selected_ < static_cast<int>(gs.groups.size())) {
                const auto& want = gs.groups[static_cast<std::size_t>(group_selected_)];
                std::string name = want.members.empty() ? std::string() : want.members[0].name;
                for (std::size_t i = 0; i < gs.groups.size(); ++i)
                    if ((gs.groups[i].members.empty() ? std::string() : gs.groups[i].members[0].name) == name) {
                        detail_ = static_cast<int>(i);
                        break;
                    }
            }
        } else if (id == "GLP_GroupDetailList") detail_selected_ = row;
        else return false;
        return true;
    }

    bool list_double_click(UiHost& host, std::string_view id, int row, int) override {
        if (!host.conn) return false;
        const auto* l = list(host, id);
        if (!l || (id != "PLP_PlayerSearchResultList" && id != "GLP_PlayerSearchResultList" && id != "GLP_GroupDetailList"))
            return false;
        if (row >= 0 && row < static_cast<int>(l->rows.size())) start_tell(*host.conn, l->rows[static_cast<std::size_t>(row)].texts[0]);
        return true;
    }

    bool list_header_click(UiHost&, std::string_view id, int column) override {
        if (id == "PLP_PlayerSearchResultList") player_sort_.header_click(column);
        else if (id == "GLP_PlayerSearchResultList") group_sort_.header_click(column);
        else return false;
        return true;
    }

private:
    static constexpr std::string_view kGetMatches = "Get Matches";

    // CListWnd's sort of a result list: 0x677920 (atoi of both cells, -1 / 0 / 1) for the numeric columns, else
    // the texts; `rows` (the records behind the rows) follow.
    template <class Numeric>
    static void sort_rows(ListData& d, std::vector<std::size_t>& rows, const ListSort& sort, Numeric numeric) {
        auto compare = [&](std::size_t a, std::size_t b) -> std::optional<int> {
            if (!numeric(sort.column)) return std::nullopt;
            auto c = static_cast<std::size_t>(sort.column);
            int x = std::atoi(d.rows[a].texts[c].c_str()), y = std::atoi(d.rows[b].texts[c].c_str());
            return x < y ? -1 : x > y ? 1 : 0;
        };
        ListSort::apply(d, rows, sort.order(d.rows, compare));
    }
    static int row_of(const std::vector<std::size_t>& rows, int record) {
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (static_cast<int>(rows[i]) == record) return static_cast<int>(i);
        return -1;
    }
    static int record_of(const std::vector<std::size_t>& rows, int row) {
        return row >= 0 && row < static_cast<int>(rows.size()) ? static_cast<int>(rows[static_cast<std::size_t>(row)]) : -1;
    }

    int info_page_ = 0, search_page_ = 0;
    std::string player_label_, group_label_;
    std::uint32_t last_frame_ = 0;
    int serial_ = 0, player_selected_ = -1, group_selected_ = -1, detail_ = -1, detail_selected_ = -1;
    ListData players_, groups_, details_, wanted_;
    std::vector<std::size_t> player_rows_, group_rows_; // the record behind each row
    ListSort player_sort_, group_sort_;
};

// GroupSearchFiltersWnd ("Group Search: Additional Filters"): the ignore list or friends only (one at a time), the
// excluded or desired guilds (one at a time) with a combo box of every guild the client knows (by id, as
// sub_00675030 fills it from CGuild's names below 50000) and Add / Remove / Remove All, and "Only players who match
// my posted filters can query me" (the posts' match filter). Every change saves the character ini
// (CGroupSearchFiltersWnd::WndNotification → SaveIni).
class GroupSearchFiltersWindow : public StockWindow {
public:
    GroupSearchFiltersWindow() : StockWindow("GroupSearchFiltersWnd", false) {}

    bool shown(UiHost& host) override { return host.conn && host.conn->group_search().filters_open; }
    void set_shown(UiHost& host, bool show) override {
        if (host.conn) host.conn->group_search().filters_open = show;
    }

    void child(UiHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& f = host.conn->group_search().filters;
        if (id == "GSFW_UseIgnoreListCheckbox") out.checked = f.player_mode == 1;
        else if (id == "GSFW_UseFriendsListCheckbox") out.checked = f.player_mode == 2;
        else if (id == "GSFW_ExcludedGuildsCheckbox") out.checked = f.guild_mode == 1;
        else if (id == "GSFW_DesiredGuildsCheckbox") out.checked = f.guild_mode == 2;
        else if (id == "GSFW_ExclusionSearchCheckbox") out.checked = f.exclusive;
    }

    const ComboData* combo(UiHost& host, std::string_view id) override {
        ComboData* d = id == "GSFW_GuildNamesToExcludeSelectionCombo" ? &exclude_
                     : id == "GSFW_GuildNamesToDesireSelectionCombo"  ? &desire_ : nullptr;
        if (!d || !host.conn) return nullptr;
        // sub_00675030: CGuild::ValidGuildName for ids 0 .. 49999, in id order; the first chosen.
        std::map<std::uint32_t, std::string> by_id;
        for (const auto& [gid, name] : host.conn->guild().names)
            if (gid < 50000 && !name.empty()) by_id.emplace(gid, name);
        d->choices.clear();
        for (const auto& [gid, name] : by_id) d->choices.push_back(name);
        if (d->selected < 0 || d->selected >= static_cast<int>(d->choices.size())) d->selected = d->choices.empty() ? -1 : 0;
        return d;
    }
    bool combo_select(UiHost&, std::string_view id, int index) override {
        if (id == "GSFW_GuildNamesToExcludeSelectionCombo") exclude_.selected = index;
        else if (id == "GSFW_GuildNamesToDesireSelectionCombo") desire_.selected = index;
        else return false;
        return true;
    }

    const ListData* list(UiHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& f = host.conn->group_search().filters;
        const std::vector<std::string>* names = id == "GSFW_ExcludedGuildsList" ? &f.excluded
                                              : id == "GSFW_DesiredGuildsList"  ? &f.desired : nullptr;
        if (!names) return nullptr;
        ListData& l = id == "GSFW_ExcludedGuildsList" ? excluded_ : desired_;
        int sel = id == "GSFW_ExcludedGuildsList" ? excluded_sel_ : desired_sel_;
        l = {};
        l.columns = 1;
        l.labels = {"Guild Names"};
        l.widths = {400};
        for (const auto& n : *names) l.rows.push_back(ListRow{{n}});
        l.selected = sel < static_cast<int>(names->size()) ? sel : -1;
        return &l;
    }
    bool list_click(UiHost&, std::string_view id, int row, bool right, int) override {
        if (right) return false;
        if (id == "GSFW_ExcludedGuildsList") excluded_sel_ = row;
        else if (id == "GSFW_DesiredGuildsList") desired_sel_ = row;
        else return false;
        return true;
    }

    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& gs = host.conn->group_search();
        auto& f = gs.filters;
        auto remove_at = [](std::vector<std::string>& v, int& sel) { // CListWnd::RemoveLine(GetCurSel())
            if (sel >= 0 && sel < static_cast<int>(v.size())) v.erase(v.begin() + sel);
            sel = -1;
        };
        auto chosen = [&](ComboData& d) -> std::string { // CComboWnd::GetCurChoiceText
            combo(host, &d == &exclude_ ? "GSFW_GuildNamesToExcludeSelectionCombo" : "GSFW_GuildNamesToDesireSelectionCombo");
            return d.selected >= 0 && d.selected < static_cast<int>(d.choices.size()) ? d.choices[static_cast<std::size_t>(d.selected)] : std::string();
        };
        // CGroupSearchFiltersWnd::WndNotification: a checkbox turns itself, then unchecks its partner.
        if (id == "GSFW_UseIgnoreListCheckbox") f.player_mode = f.player_mode == 1 ? 0 : 1;
        else if (id == "GSFW_UseFriendsListCheckbox") f.player_mode = f.player_mode == 2 ? 0 : 2;
        else if (id == "GSFW_ExcludedGuildsCheckbox") f.guild_mode = f.guild_mode == 1 ? 0 : 1;
        else if (id == "GSFW_DesiredGuildsCheckbox") f.guild_mode = f.guild_mode == 2 ? 0 : 2;
        else if (id == "GSFW_ExclusionSearchCheckbox") f.exclusive = !f.exclusive;
        else if (id == "GSFW_RemoveExcludedGuildButton") remove_at(f.excluded, excluded_sel_);
        else if (id == "GSFW_RemoveAllExcludedGuildButton") f.excluded.clear();
        else if (id == "GSFW_AddExcludedGuildButton") {
            if (auto n = chosen(exclude_); !n.empty()) game::GroupSearchFilters::add_guild(f.excluded, n);
        } else if (id == "GSFW_RemoveDesiredGuildButton") remove_at(f.desired, desired_sel_);
        else if (id == "GSFW_RemoveAllDesiredGuildButton") f.desired.clear();
        else if (id == "GSFW_AddDesiredGuildButton") {
            if (auto n = chosen(desire_); !n.empty()) game::GroupSearchFilters::add_guild(f.desired, n);
        } else return false;
        gs.filters_dirty = true;
        return true;
    }

private:
    ComboData exclude_, desire_;
    ListData excluded_, desired_;
    int excluded_sel_ = -1, desired_sel_ = -1;
};

} // namespace

void add_group_search_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<GroupSearchWindow>());
    out.push_back(std::make_unique<GroupSearchFiltersWindow>());
}

} // namespace hailstone::app
