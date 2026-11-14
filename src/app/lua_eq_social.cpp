// The social windows: FriendsWindow (CFriendsWnd), GuildManagementWnd (CGuildMgmtWnd), RaidWindow (CRaidWnd) over
// the client's social state (game/social.h). Lists have the stock XML's columns; the buttons run the slash
// commands the C++ windows (app/social_ui.cpp) ran. What the windows do beyond that is listed in docs/unknowns.md.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <format>
#include <functional>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_eq_windows.h"
#include "core/web_url.h"
#include "game/eq_names.h"
#include "game/guild_notes.h"
#include "game/guild_ranks.h"
#include "game/quantity_dialog.h"
#include "game/zone_names.h"
#include "mq/commands.h"
#include "net/game_connection.h"
#include "net/social_packets.h"
#include "ui/ui_ini.h"

namespace hailstone::app {

namespace {

std::string eqstr(EqHost& host, std::uint32_t id, std::vector<std::string> args = {}) {
    return host.conn ? host.conn->strings().format(id, args) : std::string();
}

// A button's command line, sent as it is: no ${...} expansion (mq::LiteralLine). Its text is what other players and
// the server wrote (a guild's URL, channel or MOTD, a member's name), never typed by the player as a command; the
// original has no ${...} at all.
void send_literal(net::GameConnection& c, const std::string& line) {
    mq::LiteralLine literal;
    c.send_command(line);
}

std::string strip_server(const std::string& s) {
    auto dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(dot + 1);
}

ListData make_list(std::initializer_list<std::pair<const char*, int>> cols) {
    ListData d;
    d.columns = static_cast<int>(cols.size());
    for (auto [l, w] : cols) {
        d.labels.emplace_back(l);
        d.widths.push_back(w);
    }
    return d;
}

class FriendsWindow : public StockWindow {
public:
    FriendsWindow() : StockWindow("FriendsWindow", false) {}

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& s = host.conn->social();
        if (id == "FriendsList") {
            friends_ = make_list({{"", 144}});
            friends_.selected = sel_friend_;
            for (const auto& f : s.friends) friends_.rows.push_back({{f}, {1, 1, 1}, std::nullopt});
            return &friends_;
        }
        if (id == "IgnoreList") {
            ignored_ = make_list({{"", 144}});
            ignored_.selected = sel_ignore_;
            for (const auto& i : s.ignored) ignored_.rows.push_back({{strip_server(i)}, {1, 1, 1}, std::nullopt});
            return &ignored_;
        }
        return nullptr;
    }
    bool list_click(EqHost&, std::string_view id, int row, bool, int) override {
        if (id == "FriendsList") sel_friend_ = row;
        else if (id == "IgnoreList") sel_ignore_ = row;
        else return false;
        return true;
    }
    void child(EqHost&, std::string_view id, Child& out) override {
        if (id == "NameInput") out.text = name_;
        else if (id == "IgnoreNameInput") out.text = ignore_name_;
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id == "NameInput") name_ = text;
        else if (id == "IgnoreNameInput") ignore_name_ = text;
        else return false;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        auto selected = [](const ListData& d, int sel) {
            return sel >= 0 && sel < static_cast<int>(d.rows.size()) ? d.rows[static_cast<std::size_t>(sel)].texts[0] : std::string();
        };
        // /friends and /ignore toggle a name on or off the list.
        if (id == "AddButton" && !name_.empty()) send_literal(c, "/friends " + name_), name_.clear();
        else if (id == "DeleteButton") {
            if (auto n = selected(friends_, sel_friend_); !n.empty()) send_literal(c, "/friends " + n);
        } else if (id == "IgnoreAddButton" && !ignore_name_.empty()) send_literal(c, "/ignore " + ignore_name_), ignore_name_.clear();
        else if (id == "IgnoreDeleteButton") {
            if (auto n = selected(ignored_, sel_ignore_); !n.empty()) send_literal(c, "/ignore " + n);
        } else if (id == "WhoButton") {
            if (auto n = selected(friends_, sel_friend_); !n.empty()) send_literal(c, "/who all " + n);
        } else return false;
        return true;
    }
    std::optional<int> tab_page(EqHost&, std::string_view id) override { return id == "Subwindows" ? std::optional<int>(page_) : std::nullopt; }
    void set_tab_page(EqHost&, std::string_view id, int page) override {
        if (id == "Subwindows") page_ = page;
    }

private:
    ListData friends_, ignored_;
    int sel_friend_ = -1, sel_ignore_ = -1, page_ = 0;
    std::string name_, ignore_name_;
};

// GuildManagementWnd (CGuildMgmtWnd). One selected member across the member,
// note and tribute lists (+0x104), which show the members the ShowOffline / HideAlts toggles let through (ini keys of
// those names, default on). The buttons are WndNotification's: Invite, Remove, Make Leader, Promote and Demote run
// /guildinvite, /guildremove, /guildleader and Send_OP_GuildPromote / Send_OP_GuildDemote on the member; Alt sets its
// alt flag (Send_OP_GuildManageBanker); the Notes page's Update its public note (permission 12); the Information
// page the MOTD (permission 0x12), the guild's web page (0x12) and chat channel (the leader), View (/url) and Join
// (/join). The Access page lists the ranks with their names (OP_GuildUpdate action 4) and the permissions per rank;
// a click on a rank's cell flips the permission (action 5) given permission 5 over a lower rank holding it yourself
// (CGuildMgmtWnd_RefreshRanksPage's checkboxes). A double click on a rank (the ranks view) renames it (text entry
// 7657, OP_GuildUpdate action 4) given permission 6 over a lower rank (or as the leader); the members view gives
// each member a combo box of the ranks you may move it to (CGuildMgmtWnd_UpdateRankListMember), which promotes or
// demotes it. The personal notes are the Notes page's third column (game/guild_notes.h, the character's
// GuildNotes.txt); Dump writes /outputfile guild's file. The Tribute page lists each member's tribute and trophy
// tribute states, donations and last donation (CGuildMgmtWnd_UpdateTributeListMember); its two buttons opt the
// selected member out (UpdateButtons: the member opted in, and yourself or permission 22 / 24); the favour label
// is EQType 122. The Banner page shows what the original shows without banner messages (EQEmu sends none): an
// empty kit list, "No active banner" (eqstr 636), an empty description, and its controls as
// CGuildMgmtWnd_UpdateBannerPage enables them with no kit selected and no planted banner (docs/unknowns.md).
class GuildWindow : public StockWindow {
public:
    GuildWindow() : StockWindow("GuildManagementWnd", false) {}

    // The lists are built once a frame, and again after the window's own state changed (a click, a toggle, a
    // selection): a sorted or filtered list reads every row (eq.ListRow), each read asking for the list again, so
    // building it on every ask cost the roster's size squared a frame.
    void frame(EqHost&) override { ++generation_; }

    void ini_loaded(EqHost&, ui::UiIni& ini, const std::filesystem::path&) override {
        ++generation_;
        ini_ = &ini;
        auto read = [&](const char* key) {
            auto v = ini.get("GuildManagementWnd", key);
            char c = v && !v->empty() ? (*v)[0] : '1';
            return c == '1' || c == 't' || c == 'T';
        };
        show_offline_ = read("ShowOffline");
        hide_alts_ = read("HideAlts");
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        const auto& g = c.guild();
        bool motd_perm = c.has_guild_permission(kPermMotd);
        const game::GuildMemberInfo* m = selected_member(host);
        if (id == "GuildName") out.text = g.name_of(g.guild_id);
        else if (id == "PlayerCount") out.text = std::to_string(g.members.size());
        else if (id == "MOTDViewer") {
            out.text = g.motd;
            out.shown = !motd_perm; // UpdateButtons: the viewer for those who may not set it
        } else if (id == "MOTDAuthorLabel") out.text = g.motd_set_by;
        else if (id == "MOTDInput") {
            out.text = motd_.value_or(g.motd);
            out.enabled = motd_perm;
            out.shown = motd_perm;
        } else if (id == "UpdateMOTDButton") out.enabled = out.shown = motd_perm;
        else if (id == "PublicInput") {
            out.text = public_;
            out.enabled = m && c.has_guild_permission(kPermPublicNote);
        } else if (id == "UpdatePublicButton") out.enabled = m && c.has_guild_permission(kPermPublicNote) && public_ != m->public_note;
        // UpdateButtons: the personal note's input with a member selected, its Update while it differs from the
        // member's note.
        else if (id == "PersonalInput") {
            out.text = personal_;
            out.enabled = m != nullptr;
        } else if (id == "UpdatePersonalButton") out.enabled = m && personal_ != personal_note(host, m->name);
        else if (id.rfind("GT_Member_List:", 0) == 0) {
            if (const game::GuildMemberInfo* who = access_member(host, id)) out.enabled = rank_choices(host, *who).enabled;
        } else if (id == "URLInput") {
            out.text = url_.value_or(g.url);
            out.enabled = motd_perm;
        } else if (id == "UpdateURL") out.enabled = motd_perm;
        else if (id == "ChannelInput") {
            out.text = channel_.value_or(g.channel);
            out.enabled = g.rank == 1;
        } else if (id == "UpdateChannel") out.enabled = g.rank == 1;
        else if (id == "ShowOfflineButton") out.checked = show_offline_;
        else if (id == "HideAltButton") out.checked = hide_alts_;
        else if (id == "GT_Member_List") out.shown = !view_ranks_;
        else if (id == "GT_Rank_List") out.shown = view_ranks_;
        else if (id == "AltFlagButton") {
            out.enabled = member_button(host, id);
            out.checked = m && m->alt;
        } else if (id == "PromoteButton" || id == "DemoteButton" || id == "MakeLeaderButton" || id == "RemoveButton")
            out.enabled = member_button(host, id);
        else if (id == "InviteButton") // CGuildMgmtWnd's update (every 500 ms): a target and permission 7
            out.enabled = c.world().target_id != 0 && c.has_guild_permission(game::kGuildPermInvite);
        // UpdateButtons: a tribute opt-out for a selected member opted in, yourself (strcmp with your name) or
        // with permission 0x16 (0x18 for the trophy tribute).
        else if (id == "TributeOptOutButton")
            out.enabled = m && m->tribute_on && (m->name == c.character_name() || c.has_guild_permission(kPermTributeOthers));
        else if (id == "TrophyTributeOptOutButton")
            out.enabled = m && m->trophy_on && (m->name == c.character_name() || c.has_guild_permission(kPermTrophyOthers));
        // OnProcessFrame: without a planted banner's countdown (+0x618 -1, as AboutToShow leaves it until a banner
        // message sets it) the label shows string 0x27c.
        else if (id == "TimeRemainingValueLabel") out.text = eqstr(host, 636);
        else if (id == "BannerDescription") out.text = std::string(); // no kit row selected
        // UpdateBannerPage, never editing (+0x364) and no kit row: the filter, Show All and Refresh enabled; Plant
        // and Remove need a kit row (and Remove a planted banner); Edit, the layer and colour buttons, Save and
        // Cancel also need a character flag (bit 0x1000 of PcBase's virtual +0x5c) whose meaning is not known, so
        // they stay off (docs/unknowns.md).
        else if (id == "BannerFilter" || id == "RefreshBannerListButton") out.enabled = true;
        else if (id == "ShowAllPossibleBannersButton") {
            out.enabled = true;
            out.checked = show_all_banners_;
        } else if (id == "PlantBannerButton" || id == "RemoveBannerButton" || id == "EditBannerButton" ||
                   id == "SaveBannerSettings" || id == "CancelEditBanner" || id == "Layer1TextureNext" ||
                   id == "Layer1TexturePrev" || id == "Layer2TextureNext" || id == "Layer2TexturePrev" ||
                   id == "Layer1ColorBtn" || id == "Layer2ColorBtn")
            out.enabled = false;
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& g = host.conn->guild();
        auto fresh = [this](std::uint64_t& built) {
            if (built == generation_) return true;
            built = generation_;
            return false;
        };
        if (id == "MemberList" || id == "NoteList") {
            if (fresh(id == "MemberList" ? members_built_ : notes_built_)) return id == "MemberList" ? &members_ : &notes_;
            rebuild_names(host);
        }
        if (id == "MemberList") {
            // CGuildMgmtWnd::UpdateListMember @0x6842b0: name, level, class, rank, "A" for an alt, last on as
            // "%02d/%02d/%02d" (month/day/year, local time; empty for 0), the zone for an online member (eqstr 3122
            // ANONYMOUS for an anonymous one), the public note; online rows white, offline 0xc0c0c0.
            members_ = make_list({{"Name", 100}, {"Lvl", 45}, {"Class", 60}, {"Rank", 45}, {"Alt", 15}, {"Last On", 80},
                                  {"Zone", 100}, {"Public Note", 120}, {"", 15}, {"", 10}});
            for (const auto* m : shown_) {
                ListRow r;
                std::string zone;
                if (m->online()) zone = m->zone_id == 0xffff ? host.conn->strings().format(3122, {}) : game::zone_long_name(m->zone_id);
                r.texts = {m->name, std::to_string(m->level), host.conn->guild_member_class(m->class_),
                           g.rank_name(m->rank), m->alt ? "A" : "", last_on(m->last_on), zone, m->public_note, "", ""};
                r.color = m->online() ? Rgb{1, 1, 1} : argb_to_rgb(0xffc0c0c0);
                if (m->name == selected_name_) members_.selected = static_cast<int>(members_.rows.size());
                members_.rows.push_back(std::move(r));
            }
            return &members_;
        }
        if (id == "NoteList") {
            // CGuildMgmtWnd_UpdateNoteListMember: the name, the public note and the personal note (member +0x2a0,
            // from LoadPersonalNotes' file); both notes grey for an offline member.
            notes_ = make_list({{"Name", 100}, {"Public Note", 240}, {"Personal Note", 200}});
            for (const auto* m : shown_) {
                if (m->name == selected_name_) notes_.selected = static_cast<int>(notes_.rows.size());
                ListRow r{{m->name, m->public_note, personal_note(host, m->name)}, {1, 1, 1}, std::nullopt};
                if (!m->online()) r.cell_colors = {std::nullopt, argb_to_rgb(0xffc0c0c0), argb_to_rgb(0xffc0c0c0)};
                notes_.rows.push_back(std::move(r));
            }
            return &notes_;
        }
        if (id == "TributeList") {
            if (fresh(tribute_built_)) return &tribute_;
            rebuild_names(host);
            // CGuildMgmtWnd_UpdateTributeListMember @0x682830: the name, the tribute and trophy states (string
            // 0x3292 + on), the donations and the last donation ("%02d/%02d/%02d", empty for 0); the states white
            // with the member's flag (+0x10), else yellow; the other cells grey for an offline member.
            tribute_ = make_list({{"Name", 100}, {"Tribute Status", 100}, {"Trophy Tribute Status", 140}, {"Donations", 85},
                                  {"Last Donation", 130}});
            for (const auto* m : shown_) {
                if (m->name == selected_name_) tribute_.selected = static_cast<int>(tribute_.rows.size());
                ListRow r{{m->name, eqstr(host, kStrOff + (m->tribute_on ? 1u : 0u)), eqstr(host, kStrOff + (m->trophy_on ? 1u : 0u)),
                           std::to_string(m->donations), last_on(m->last_donation)},
                          {1, 1, 1},
                          std::nullopt};
                Rgb online = m->online() ? Rgb{1, 1, 1} : argb_to_rgb(0xffc0c0c0);
                Rgb state = m->tribute_flag ? Rgb{1, 1, 1} : argb_to_rgb(0xffffff00);
                r.cell_colors = {online, state, state, online, online};
                tribute_.rows.push_back(std::move(r));
            }
            return &tribute_;
        }
        if (id == "BannerKitList") {
            // HandleBannerKitList fills it from a server message EQEmu does not send; 0x681960 adds a row only
            // for a planted banner.
            banner_kits_ = make_list({{"", 0}, {"Banner", 400}});
            return &banner_kits_;
        }
        if (id == "GT_Rank_List") {
            if (fresh(ranks_built_)) return &ranks_;
            // CGuildMgmtWnd_RefreshRanksPage: each rank's number, name (0x7d89e0) and how many members hold it.
            ranks_ = make_list({{"", 16}, {"Name", 140}, {"#", 30}});
            for (std::uint32_t r = 1; r <= 8; ++r) {
                int n = 0;
                for (const auto& m : g.members) n += m.rank == r ? 1 : 0;
                ranks_.rows.push_back({{std::to_string(r), g.rank_name(r), std::to_string(n)}, {1, 1, 1}, std::nullopt});
            }
            return &ranks_;
        }
        if (id == "GT_Member_List") {
            // CGuildMgmtWnd_UpdateRankListMember: the members the toggles let through, each with the name and a combo
            // box of ranks in column 2 (the child "GT_Member_List:<row>").
            if (fresh(access_built_)) return &access_;
            rebuild_names(host);
            access_ = make_list({{"", 16}, {"Name", 130}, {"Group", 110}, {"", 10}});
            access_.combo_column = 2;
            for (const auto* m : shown_) {
                access_.rows.push_back({{"", m->name, g.rank_name(m->rank), ""}, {1, 1, 1}, std::nullopt});
            }
            return &access_;
        }
        if (id == "GT_Permissions_List") {
            if (fresh(permissions_built_)) return &permissions_;
            // The permissions 1..30 (names: table 0xb617f8), a column per rank 1..8, ticked where the rank has it;
            // a cell you may not change is greyed.
            permissions_ = make_list({{"Permission", 200}, {"1", 20}, {"2", 20}, {"3", 20}, {"4", 20}, {"5", 20},
                                      {"6", 20}, {"7", 20}, {"8", 20}});
            for (std::int32_t p = 1; p <= 30; ++p) {
                ListRow row;
                row.texts.push_back(kPermissionNames[p - 1]);
                row.cell_colors.push_back(std::nullopt);
                for (std::int32_t r = 1; r <= 8; ++r) {
                    row.texts.push_back(g.permissions.has(r, p) ? "X" : "");
                    row.cell_colors.push_back(can_change(host, r, p) ? std::nullopt : std::optional<Rgb>(kGrey));
                }
                permissions_.rows.push_back(std::move(row));
            }
            return &permissions_;
        }
        return nullptr;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int column) override {
        ++generation_;
        if (!host.conn) return false;
        if (id == "MemberList" || id == "NoteList" || id == "TributeList") {
            if (right) return true;
            rebuild_names(host);
            if (row < 0 || row >= static_cast<int>(shown_.size())) return true;
            // The member is selected in all the lists; its public and personal notes go to the Notes page's inputs.
            selected_name_ = shown_[static_cast<std::size_t>(row)]->name;
            public_ = shown_[static_cast<std::size_t>(row)]->public_note;
            personal_ = personal_note(host, selected_name_);
            return true;
        }
        if (id == "GT_Permissions_List") {
            std::int32_t p = row + 1, r = column;
            if (right || p < 1 || p > 30 || r < 1 || r > 8 || !can_change(host, r, p)) return true;
            host.conn->guild_set_rank_permission(static_cast<std::uint32_t>(r), static_cast<std::uint32_t>(p),
                                                 !host.conn->guild().permissions.has(r, p));
            return true;
        }
        return id == "GT_Rank_List" || id == "GT_Member_List" || id == "BannerKitList";
    }
    bool list_double_click(EqHost& host, std::string_view id, int row, int) override {
        ++generation_;
        // WndNotification 4 on the rank list: with permission 6, as a GM, over a rank below yours or as the leader,
        // CTextEntryWnd::Activate(this, 0, eqstr 7657, 1, 50, the rank's name); OK (0x26, 1) sends OP_GuildUpdate
        // action 4 with the rank and the text.
        if (id != "GT_Rank_List" || !host.conn) return false;
        auto& c = *host.conn;
        auto rank = static_cast<std::int32_t>(row + 1);
        auto mine = static_cast<std::int32_t>(c.guild().rank);
        const game::Spawn* me = c.world().player();
        if (row < 0 || row > 7 || !c.has_guild_permission(kPermRankNames) ||
            !((me && me->gm) || game::guild_rank_above(mine, rank) || mine == 1))
            return true;
        EqHost::TextEntry e;
        e.prompt = eqstr(host, 7657);
        e.min_chars = 1;
        e.max_chars = 50;
        e.text = c.guild().rank_name(static_cast<std::uint32_t>(rank));
        e.done = [&host, rank](const std::string& text) {
            if (host.conn) host.conn->guild_rename_rank(static_cast<std::uint32_t>(rank), text.substr(0, 99));
        };
        host.text_entry = std::move(e);
        return true;
    }
    const ComboData* combo(EqHost& host, std::string_view id) override {
        const game::GuildMemberInfo* m = host.conn ? access_member(host, id) : nullptr;
        if (!m) return nullptr;
        RankChoices r = rank_choices(host, *m);
        rank_combo_.choices.clear();
        for (std::int32_t rank : r.ranks) rank_combo_.choices.push_back(host.conn->guild().rank_name(static_cast<std::uint32_t>(rank)));
        rank_combo_.selected = r.selected;
        return &rank_combo_;
    }
    bool combo_select(EqHost& host, std::string_view id, int index) override {
        ++generation_;
        const game::GuildMemberInfo* m = host.conn ? access_member(host, id) : nullptr;
        if (!m) return false;
        // The choice's data is its rank: a promotion or demotion when allowed, else the box keeps the member's rank.
        RankChoices r = rank_choices(host, *m);
        if (index >= 0 && index < static_cast<int>(r.ranks.size()) && r.enabled)
            host.conn->guild_change_member_rank(m->name, static_cast<std::uint32_t>(r.ranks[static_cast<std::size_t>(index)]));
        return true;
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        ++generation_;
        if (id == "PersonalInput") personal_ = text.substr(0, 255);
        else if (id == "PublicInput") public_ = text.substr(0, 255);
        else if (id == "MOTDInput") motd_ = text;
        else if (id == "URLInput") url_ = text.substr(0, 0xfff);
        else if (id == "ChannelInput") channel_ = text.substr(0, 0xfff);
        else return false;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        ++generation_;
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        const auto& g = c.guild();
        const game::GuildMemberInfo* m = selected_member(host);
        if (id == "InviteButton") send_literal(c, "/guildinvite");
        else if (id == "RemoveButton" || id == "MakeLeaderButton" || id == "PromoteButton" || id == "DemoteButton") {
            if (m)
                send_literal(c, (id == "RemoveButton" ? "/guildremove " : id == "MakeLeaderButton" ? "/guildleader "
                                : id == "PromoteButton" ? "/guildpromote " : "/guilddemote ") + m->name);
        } else if (id == "AltFlagButton") {
            if (m) c.guild_set_alt(m->name, !m->alt);
        } else if (id == "UpdatePublicButton") {
            if (m && c.has_guild_permission(kPermPublicNote)) c.guild_set_public_note(m->name, public_);
        } else if (id == "UpdatePersonalButton") {
            // SetPersonalNote, the member's +0x2a0, then SavePersonalNotes.
            if (m) {
                load_notes(host);
                notes_file_.set(m->name, personal_, static_cast<std::uint32_t>(std::time(nullptr)));
                save_notes();
            }
        } else if (id == "DumpButton") {
            // DumpToFile(0, "\t"): /outputfile guild's file under its default name.
            if (host.submit_chat) host.submit_chat("/outputfile guild");
        } else if (id == "UpdateMOTDButton") {
            // Sent by a member of rank 1..8 (Send_OP_SetGuildMOTD).
            if (g.rank >= 1 && g.rank <= 8) send_literal(c, "/guildmotd " + motd_.value_or(g.motd));
            motd_.reset();
        } else if (id == "UpdateURL") {
            c.guild_set_url(url_.value_or(g.url));
            url_.reset();
        } else if (id == "UpdateChannel") {
            c.guild_set_channel(channel_.value_or(g.channel));
            channel_.reset();
        } else if (id == "GotoURL") {
            // The guild's URL is what a guild officer set: only an http(s) page is opened (one without a scheme as
            // /www does, http://). A deliberate difference: what the original opens is not traced (docs/browser.md).
            if (!g.url.empty()) {
                std::string url = g.url.find(':') == std::string::npos ? "http://" + g.url : g.url;
                if (web::url_allowed(url, {})) send_literal(c, "/url " + url);
                else c.ui_message(url + " is not an http or https address.");
            }
        } else if (id == "JoinChannel") {
            if (!g.channel.empty()) send_literal(c, "/join " + g.channel);
        } else if (id == "ShowOfflineButton" || id == "HideAltButton") {
            bool& v = id == "ShowOfflineButton" ? show_offline_ : hide_alts_;
            v = !v;
            if (ini_) ini_->put("GuildManagementWnd", id == "ShowOfflineButton" ? "ShowOffline" : "HideAlts", v ? "1" : "0");
        } else if (id == "TributeOptOutButton" || id == "TrophyTributeOptOutButton") {
            // WndNotification: Send_OP_GuildOptInOut / Send_OP_GuildTrophyOptInOut(the selected member, 0), sent
            // as the button is (the original does not check its state again).
            if (m) {
                if (id == "TributeOptOutButton") c.guild_tribute_opt_out(m->name);
                else c.guild_trophy_opt_out(m->name);
            }
        } else if (id == "ShowAllPossibleBannersButton") {
            // The original then asks for the kit list (CGuildMgmtWnd_RequestBannerKitList: opcodes 0x0d92 and
            // 0x4924, which EQEmu does not know); not sent here (docs/unknowns.md).
            show_all_banners_ = !show_all_banners_;
        } else if (id == "RefreshBannerListButton") {
            // RequestBannerKitList as above: nothing is sent.
        } else if (id == "GT_ViewMembers_Btn") view_ranks_ = false;
        else if (id == "GT_ViewRanks_Btn") view_ranks_ = true;
        else return false;
        return true;
    }
    std::optional<int> tab_page(EqHost&, std::string_view id) override { return id == "Subwindows" ? std::optional<int>(page_) : std::nullopt; }
    void set_tab_page(EqHost&, std::string_view id, int page) override {
        ++generation_;
        if (id == "Subwindows") page_ = page;
    }

private:
    static constexpr std::int32_t kPermPublicNote = 12, kPermMotd = 0x12, kPermRankPermissions = 5, kPermRankNames = 6,
                                  kPermTributeOthers = 0x16, kPermTrophyOthers = 0x18;
    static constexpr std::uint32_t kStrOff = 0x3292; // "off"; + 1 "on"

    static constexpr Rgb kGrey{0.5f, 0.5f, 0.5f};
    static constexpr const char* kPermissionNames[30] = {
        "Banner: Change", "Banner: Plant", "Banner: Remove", "Display Guild Name", "Ranks: Change Permissions",
        "Ranks: Change Rank Names", "Members: Invite", "Members: Promote", "Members: Demote", "Members: Remove",
        "Edit Recruiting Settings", "Edit Public Notes", "Bank: Deposit Items", "Bank: Withdraw Items", "Bank: View Items",
        "Bank: Promote Items", "Bank: Change Item Permissions", "Change the MOTD", "Guild Chat: See",
        "Guild Chat: Speak In", "Send the Whole Guild E-Mail", "Tribute: Change for Others",
        "Tribute: Change Active Benefit", "Trophy Tribute: Change for Others", "Trophy Tribute: Change Active Benefit",
        "Members: Change Alt Flag for Others", "Real Estate: Guild Plot - Buy", "Real Estate: Guild Plot - Sell",
        "Real Estate: Modify Trophies", "Members: Demote Self"};

    // CGuildMgmtWnd_UpdateRankListMember's combo box for a member: the ranks 1..8 it may be given (the
    // template's eight with the rank names, pruned), the one showing the member's rank, and whether it is enabled.
    struct RankChoices {
        std::vector<std::int32_t> ranks;
        int selected = -1;
        bool enabled = false;
    };
    RankChoices rank_choices(EqHost& host, const game::GuildMemberInfo& m) const {
        const auto& c = *host.conn;
        const game::Spawn* me = c.world().player();
        // the local rank (1 for a GM, the PcClient's virtual +0x90)
        std::int32_t mine = me && me->gm ? 1 : static_cast<std::int32_t>(c.guild().rank);
        auto rank = static_cast<std::int32_t>(m.rank);
        bool own = iequals_ascii(m.name, c.character_name());
        auto may = [&](std::int32_t p) { return c.has_guild_permission(p); };
        RankChoices r;
        if (rank >= 1 && rank <= 8) {
            bool promote = game::guild_rank_above(mine, rank);
            for (std::int32_t choice = 1; choice <= 8; ++choice) {
                bool keep;
                if (choice == rank) keep = true;
                else if (rank == 1 || choice == 1 || game::guild_rank_above(choice, mine)) keep = false;
                else {
                    bool above = game::guild_rank_above(rank, choice);
                    if (own && above) keep = true;
                    else if (!promote) keep = false;
                    else keep = may(above ? game::kGuildPermDemote : game::kGuildPermPromote);
                }
                if (keep) r.ranks.push_back(choice);
            }
            // FindChoiceByText(the rank's name)
            std::string name = c.guild().rank_name(static_cast<std::uint32_t>(rank));
            for (std::size_t i = 0; i < r.ranks.size(); ++i)
                if (c.guild().rank_name(static_cast<std::uint32_t>(r.ranks[i])) == name) {
                    r.selected = static_cast<int>(i);
                    break;
                }
        }
        r.enabled = rank != 1 && !(own && !may(game::kGuildPermDemoteSelf)) &&
                    (may(game::kGuildPermPromote) || may(game::kGuildPermDemote));
        return r;
    }
    // The member behind a combo box child "GT_Member_List:<row>" (the access list's rows are the members the toggles
    // let through).
    const game::GuildMemberInfo* access_member(EqHost& host, std::string_view id) {
        constexpr std::string_view kPrefix = "GT_Member_List:";
        if (id.substr(0, kPrefix.size()) != kPrefix) return nullptr;
        int row = std::atoi(std::string(id.substr(kPrefix.size())).c_str());
        rebuild_names(host);
        if (row < 0 || row >= static_cast<int>(shown_.size())) return nullptr;
        return shown_[static_cast<std::size_t>(row)];
    }
    static bool iequals_ascii(std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    }
    // LoadPersonalNotes, once per character (the original: when the window is made and at each guild refresh), then
    // AddMember's lookup (sub_00682450) for each member.
    void load_notes(EqHost& host) {
        auto files = host.conn->own_files();
        std::filesystem::path path = files.empty() ? std::filesystem::path() : files.guild_notes();
        if (path == notes_path_) return;
        notes_path_ = path;
        notes_file_ = {};
        if (path.empty()) return;
        std::ifstream in(path, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto now = static_cast<std::uint32_t>(std::time(nullptr)); // Util::FastTime
        notes_file_.parse(text, now);
        for (const auto& m : host.conn->guild().members) notes_file_.find(m.name, now);
    }
    // SavePersonalNotes ("%08X %s %s\n" lines).
    void save_notes() {
        if (notes_path_.empty()) return;
        std::ofstream out(notes_path_, std::ios::binary | std::ios::trunc);
        if (out) out << notes_file_.format();
    }
    std::string personal_note(EqHost& host, const std::string& name) {
        load_notes(host);
        const std::string* n = notes_file_.get(name);
        return n ? *n : std::string();
    }

    // CleanAndRefillListWnd's filter: alts only without HideAlts, offline members only with ShowOffline.
    void rebuild_names(EqHost& host) {
        shown_.clear();
        for (const auto& m : host.conn->guild().members)
            if ((!m.alt || !hide_alts_) && (m.online() || show_offline_)) shown_.push_back(&m);
    }
    // A permission checkbox's state (RefreshRanksPage): permission 5, a rank below yours, and the permission yours.
    bool can_change(EqHost& host, std::int32_t rank, std::int32_t permission) const {
        const auto& c = *host.conn;
        auto mine = static_cast<std::int32_t>(c.guild().rank);
        return c.has_guild_permission(kPermRankPermissions) && game::guild_rank_above(mine, rank) &&
               c.guild().permissions.has(mine, permission);
    }
    // CGuildMgmtWnd::UpdateButtons @0x67fef0 for the selected member: `manage` when the local rank is above the
    // member's (0x7d7360), or a GM's over anyone but the leader. MakeLeader: the leader, another member;
    // Promote: manage, permission 8, the member below rank 2; Demote: (yourself, not the leader, permission 0x1e)
    // or (manage, permission 9), the member above rank 8; Remove: yourself, or manage and permission 0xa; Alt:
    // yourself, or manage and permission 0x1a.
    bool member_button(EqHost& host, std::string_view id) const {
        const game::GuildMemberInfo* m = selected_member(host);
        if (!m) return false;
        const auto& c = *host.conn;
        const std::int32_t mine = static_cast<std::int32_t>(c.guild().rank);
        const std::int32_t theirs = static_cast<std::int32_t>(m->rank);
        const game::Spawn* me = c.world().player();
        bool self = m->name == c.character_name();
        bool manage = game::guild_rank_above(mine, theirs) || (me && me->gm && theirs != 1);
        if (id == "MakeLeaderButton") return mine == 1 && !self;
        if (id == "PromoteButton") return manage && c.has_guild_permission(game::kGuildPermPromote) && game::guild_rank_above(2, theirs);
        if (id == "DemoteButton")
            return ((self && mine != 1 && c.has_guild_permission(game::kGuildPermDemoteSelf)) ||
                    (manage && c.has_guild_permission(game::kGuildPermDemote))) &&
                   game::guild_rank_above(theirs, 8);
        if (id == "RemoveButton") return self || (manage && c.has_guild_permission(0xa));
        return self || (manage && c.has_guild_permission(game::kGuildPermBanker)); // AltFlagButton
    }
    const game::GuildMemberInfo* selected_member(EqHost& host) const {
        if (selected_name_.empty()) return nullptr;
        for (const auto& m : host.conn->guild().members)
            if (m.name == selected_name_) return &m;
        return nullptr;
    }
    static std::string last_on(std::uint32_t t) {
        if (t == 0) return {};
        std::time_t tt = static_cast<std::time_t>(t);
        std::tm* tm = std::localtime(&tt);
        if (!tm) return {};
        return std::format("{:02d}/{:02d}/{:02d}", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100);
    }
    ui::UiIni* ini_ = nullptr;
    ListData members_, notes_, tribute_, ranks_, access_, permissions_, banner_kits_;
    std::uint64_t generation_ = 1; // the lists' generation (frame and changes); each list's when it was built
    std::uint64_t members_built_ = 0, notes_built_ = 0, tribute_built_ = 0, ranks_built_ = 0, access_built_ = 0,
                  permissions_built_ = 0;
    ComboData rank_combo_;
    std::vector<const game::GuildMemberInfo*> shown_;
    game::GuildPersonalNotes notes_file_;
    std::filesystem::path notes_path_;
    int page_ = 0;
    bool show_offline_ = true, hide_alts_ = true, view_ranks_ = false;
    bool show_all_banners_ = false; // ShowAllPossibleBannersButton (the constructor leaves it unchecked)
    std::string public_, personal_, selected_name_;
    std::optional<std::string> motd_, url_, channel_; // what is being typed (none: the guild's)
};

class RaidWindow : public StockWindow {
public:
    RaidWindow() : StockWindow("RaidWindow", false) {}

    // The class colours (CRaidWnd +0x2d8, sub_00729700; CRaidOptionsWnd's sub_00728040 reads the same keys): [RaidWindow]
    // ClassColor0..15 of the UI ini, by the options window's button order (Bard, Beastlord, Cleric, Druid, Enchanter,
    // Magician, Monk, Necro, Paladin, Ranger, Rogue, Shadow Knight, Shaman, Warrior, Wizard, Berserker); a key the ini
    // lacks ("*NULL*") is written as -1, the ARGB white the class column then shows.
    void ini_loaded(EqHost&, ui::UiIni& ini, const std::filesystem::path&) override {
        ini_ = &ini;
        for (int i = 0; i < 16; ++i) {
            std::string key = std::format("ClassColor{}", i);
            auto v = ini.get("RaidWindow", key);
            if (!v || v->empty() || (*v)[0] == '*') {
                ini.put("RaidWindow", key, "-1");
                class_colors_[static_cast<std::size_t>(i)] = 0xffffffffu;
            } else {
                class_colors_[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(std::atoi(v->c_str()));
            }
        }
    }
    std::uint32_t class_color(int button) const { return class_colors_[static_cast<std::size_t>(button & 15)]; }
    // CRaidOptionsWnd::WndNotification's 0x25 (a colour picked for a class button): the ini key, then
    // CRaid::UpdateClassColor (CRaidWnd::SetClassColor and the members' class cells, CRaidWnd::SetPlayerClassColor).
    void set_class_color(int button, std::uint32_t argb) {
        if (button < 0 || button > 15) return;
        class_colors_[static_cast<std::size_t>(button)] = argb;
        if (ini_) ini_->put("RaidWindow", std::format("ClassColor{}", button), std::to_string(static_cast<std::int32_t>(argb)));
    }

    // CRaid_HandleInvite: an invitation opens the window ("click the accept button in the raid window").
    bool shown(EqHost& host) override {
        bool invited = host.conn && !host.conn->raid().inviter.empty();
        if (invited && !was_invited_) open_ = true;
        was_invited_ = invited;
        return open_;
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& r = host.conn->raid();
        bool invited = !r.inviter.empty();
        // CRaidWnd::UpdateButtons @0x728f10: the invitation's pair while invited (enabled unlocked), else Invite
        // (unlocked; with a player target, only when it is someone else, and in a raid only for its leader under 72
        // members) and Leave (in a raid); the group moves only for the leader of a locked raid, Lock for the
        // leader of an unlocked one, Unlock for a locked one's; Options in a raid; the looters for the leader.
        bool leader = r.active && r.leader == host.conn->character_name();
        bool locked_leader = r.locked && leader;
        if (id == "RAID_AcceptButton" || id == "RAID_DeclineButton") out.shown = invited, out.enabled = !r.locked;
        else if (id == "RAID_InviteButton") {
            out.shown = !invited;
            bool enabled = !r.locked;
            const auto& w = host.conn->world();
            auto t = w.spawns.find(w.target_id);
            if (!r.locked && t != w.spawns.end() && !t->second.is_npc && !t->second.is_corpse) {
                bool other = t->first != w.player_id;
                if (!r.active) enabled = other;
                else if (leader && r.members.size() < 72) enabled = other;
            }
            out.enabled = enabled;
        } else if (id == "RAID_DisbandButton") out.shown = !invited, out.enabled = r.active;
        else if (id == "RAID_LockButton") out.shown = !locked_leader, out.enabled = !r.locked && leader;
        else if (id == "RAID_UnlockButton") out.shown = locked_leader, out.enabled = locked_leader;
        else if (id == "RAID_OptionsButton") out.enabled = r.active;
        else if (id == "RAID_PlayerCountLabel") out.text = std::to_string(r.members.size());
        else if (id == "RAID_LevelAverageLabel") {
            int sum = 0;
            for (const auto& m : r.members) sum += m.level;
            out.text = r.members.empty() ? std::string("0") : std::to_string(sum / static_cast<int>(r.members.size()));
        } else if (id == "MOTDViewer") out.text = r.motd;
        else if (id == "MOTDInput") out.text = motd_, out.shown = leader, out.enabled = leader;
        // The MOTD button: the leader's, enabled when the typed MOTD differs from the shown one.
        else if (id == "UpdateMOTDButton") out.shown = leader, out.enabled = leader && motd_ != r.motd;
        else if (id.rfind("RAID_Group", 0) == 0 || id == "RAID_NoGroupButton" || id == "RAID_RemoveLeaderButton")
            out.enabled = locked_leader;
        else if (id == "RAID_AddLooterButton" || id == "RAID_RemoveLooterButton") out.enabled = leader;
        else if (id == "RAID_MakeLeaderButton") {
            // Enabled for the leader of an unlocked raid with another player targeted.
            const auto& w = host.conn->world();
            auto t = w.spawns.find(w.target_id);
            out.enabled = !r.locked && leader && t != w.spawns.end() && !t->second.is_npc && !t->second.is_corpse &&
                          t->first != w.player_id;
        }
        // The leadership abilities: Main Assist and Mark NPC for the leader with 0x13 / 0x14 active, Find PC in a
        // raid with 0x1c active (Gates of Discord in each).
        else if (id == "RAID_MainAssistButton") out.enabled = leader && host.conn->leadership_ability_active(0x13, 1);
        else if (id == "RAID_MarkNPCButton") out.enabled = leader && host.conn->leadership_ability_active(0x14, 1);
        else if (id == "RAID_FindPCButton") out.enabled = r.active && host.conn->leadership_ability_active(0x1c, 1);
        else if (id == "RAID_InviteExpedButton" || id == "RAID_InviteTaskButton")
            out.enabled = false;
        // The note page (UpdateButtons): the note input is editable for the leader with a note list row selected;
        // Update is the leader's while the input differs from that row's note.
        else if (id == "NoteEdit") {
            const game::RaidMember* m = note_member(host);
            follow_note(m);
            out.text = note_edit_;
            out.enabled = leader && m;
        } else if (id == "NoteUpdateButton") {
            const game::RaidMember* m = note_member(host);
            follow_note(m);
            out.enabled = leader && m && note_edit_ != m->note;
        }
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id == "NoteEdit") {
            note_edit_ = text.substr(0, 63);
            return true;
        }
        if (id != "MOTDInput") return false;
        motd_ = text;
        return true;
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& r = host.conn->raid();
        if (id == "RAID_NoteList") {
            // CRaidWnd::AddPlayertoList's note row for each member, in the order they joined: the group (group + 1,
            // empty for none), the name, the note (CRaidWnd_SetMemberNote).
            notes_ = make_list({{"Grp", 30}, {"Name", 100}, {"Note", 200}});
            for (const auto& m : r.members) {
                if (m.name == note_name_) notes_.selected = static_cast<int>(notes_.rows.size());
                notes_.rows.push_back({{m.group < 12 ? std::to_string(m.group + 1) : std::string(), m.name, m.note},
                                       {1, 1, 1},
                                       std::nullopt});
            }
            return &notes_;
        }
        bool grouped = id == "RAID_PlayerList";
        if (!grouped && id != "RAID_NotInGroupPlayerList") return nullptr;
        ListData& d = grouped ? grouped_ : ungrouped_;
        d = make_list({{"Grp", 30}, {"Player Name", 100}, {"Lvl", 30}, {"Class", 100}, {"Role", 50}, {"Raid Rank", 80},
                       {"Exped Missing", 20}, {"Task Missing", 20}});
        (grouped ? grouped_names_ : ungrouped_names_).clear();
        for (const auto& m : r.members) {
            bool in = m.group < 12;
            if (in != grouped) continue;
            if (m.name == r.target) d.selected = static_cast<int>(d.rows.size());
            (grouped ? grouped_names_ : ungrouped_names_).push_back(m.name);
            std::string rank = m.name == r.leader ? "Raid Leader" : m.group_leader ? "Group Leader" : "";
            // The Role column (CRaidWnd_SetMemberAssistMarkerFlags): "A" a main assist, then "M" a marker.
            std::string role = std::string(m.main_assist ? "A" : "") + (m.marker ? "M" : "");
            ListRow row{{in ? std::to_string(m.group + 1) : std::string(), m.name, std::to_string(m.level),
                         m.class_ ? game::class_name(m.class_) : std::string(), role, rank, "", ""},
                        {1, 1, 1},
                        std::nullopt};
            // CRaidWnd_SetRowClassColor: the class column in the class's colour.
            if (int b = class_button(m.class_); b >= 0)
                row.cell_colors = {std::nullopt, std::nullopt, std::nullopt, argb_to_rgb(class_color(b))};
            d.rows.push_back(std::move(row));
        }
        return &d;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        // CRaidWnd::WndNotification @0x72d180: the member buttons
        // act on the raid target (a click in the lists).
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        const auto& r = c.raid();
        const bool leader = r.active && r.leader == c.character_name();
        if (id == "RAID_InviteButton") send_literal(c, "/raidinvite");
        else if (id == "RAID_DisbandButton") c.raid_leave();
        else if (id == "RAID_AcceptButton") c.answer_raid_invite(true);
        else if (id == "RAID_DeclineButton") c.answer_raid_invite(false);
        else if (id == "RAID_LockButton") c.raid_lock(true);
        else if (id == "RAID_UnlockButton") c.raid_lock(false);
        else if (id == "RAID_OptionsButton") c.set_raid_options_open(!c.raid_options().open);
        else if (id == "UpdateMOTDButton") c.raid_set_motd(motd_);
        else if (id == "NoteUpdateButton") {
            // The note list's selected row (its name) with the note input's text.
            if (const game::RaidMember* m = note_member(host)) c.raid_set_note(m->name, note_edit_);
        } else if (id == "RAID_DumpButton") {
            // CRaidWnd_DumpRosterToFile(0): /outputfile raid's file under its default name.
            if (host.submit_chat) host.submit_chat("/outputfile raid");
        }
        else if (id == "RAID_MakeLeaderButton") c.raid_send_leadership_change(std::nullopt);
        else if (id == "RAID_AddLooterButton") c.raid_add_looter();
        else if (id == "RAID_RemoveLooterButton") c.raid_remove_looter();
        else if (id == "RAID_MainAssistButton") c.raid_delegate_ability(3, r.target);
        else if (id == "RAID_MarkNPCButton") c.raid_delegate_ability(4, r.target);
        else if (id == "RAID_FindPCButton") c.raid_find_path_to_target();
        else if (id == "RAID_RemoveLeaderButton") {
            if (leader && !r.target.empty()) c.raid_remove_group_leader(r.target);
        } else if (id == "RAID_NoGroupButton") {
            if (leader && !r.target.empty()) c.raid_move_group(r.target, -1);
        } else if (id.rfind("RAID_Group", 0) == 0 && id.size() > 16) {
            // RAID_Group1Button .. RAID_Group12Button: groups 0..11.
            int n = std::atoi(std::string(id.substr(10, id.size() - 16)).c_str());
            if (leader && !r.target.empty() && n >= 1 && n <= 12) c.raid_move_group(r.target, n - 1);
        } else return false;
        return true;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool, int) override {
        if (id == "RAID_NoteList") {
            // The row's member becomes the raid target and its note goes to the note input.
            if (!host.conn || row < 0 || row >= static_cast<int>(host.conn->raid().members.size())) return true;
            const game::RaidMember& m = host.conn->raid().members[static_cast<std::size_t>(row)];
            note_name_ = m.name;
            note_edit_ = note_seen_ = m.note;
            host.conn->set_raid_target(m.name);
            return true;
        }
        const auto& names = id == "RAID_PlayerList" ? grouped_names_ : ungrouped_names_;
        if (id != "RAID_PlayerList" && id != "RAID_NotInGroupPlayerList") return false;
        // CRaid::SetTargetRaidPlayer with the row's name.
        if (host.conn && row >= 0 && row < static_cast<int>(names.size()))
            host.conn->set_raid_target(names[static_cast<std::size_t>(row)]);
        return true;
    }
    std::optional<int> tab_page(EqHost&, std::string_view id) override { return id == "Subwindows" ? std::optional<int>(page_) : std::nullopt; }
    void set_tab_page(EqHost&, std::string_view id, int page) override {
        if (id == "Subwindows") page_ = page;
    }

private:
    // The options window's button for a class (the inverse of CRaid::UpdateClassColor's button-to-class table), -1
    // for none. Indexed by class id: warrior 1 is button 13, bard 8 button 0, berserker 16 button 15.
    static int class_button(std::uint8_t class_id) {
        static constexpr std::int8_t kButtonOfClass[17] = {-1, 13, 2, 8, 9, 11, 3, 6, 0, 10, 12, 7, 14, 5, 4, 1, 15};
        return class_id < 17 ? kButtonOfClass[class_id] : -1;
    }
    const game::RaidMember* note_member(EqHost& host) const {
        if (!host.conn || note_name_.empty()) return nullptr;
        for (const auto& m : host.conn->raid().members)
            if (m.name == note_name_) return &m;
        return nullptr;
    }
    // CRaidWnd_SetMemberNote: a note arriving for the selected row replaces the input's text.
    void follow_note(const game::RaidMember* m) {
        if (m && m->note != note_seen_) note_edit_ = note_seen_ = m->note;
    }

    ListData grouped_, ungrouped_, notes_;
    std::vector<std::string> grouped_names_, ungrouped_names_;
    std::string motd_, note_name_, note_edit_, note_seen_;
    bool was_invited_ = false;
    int page_ = 0;
    ui::UiIni* ini_ = nullptr;
    std::array<std::uint32_t, 16> class_colors_ = [] {
        std::array<std::uint32_t, 16> a{};
        a.fill(0xffffffffu);
        return a;
    }();
};


// RaidOptionsWindow (CRaidOptionsWnd): UpdateComponents (on showing and every
// 250 ms) enables Set Loot Type in a raid for its leader and shows the loot type as a number ("%d") with the combo box
// on it; picking a type sends it as /setloottype does (CRaid::SetLootType with itoa(choice + 1)). The looter list is
// CRaid::UpdateOptionsWindow's: the leader, then the group leaders (type 2) or the raid looters (type 3), or only
// "Entire Raid" (type 4). The sixteen class buttons open the colour picker on their colour (CRNormal); the colour
// picked is the class's in the raid window (RaidWindow::set_class_color).
class RaidOptionsWindow : public StockWindow {
public:
    explicit RaidOptionsWindow(RaidWindow& raid) : StockWindow("RaidOptionsWindow", false), raid_(raid) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->raid_options().open && host.conn->raid().active; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_raid_options_open(show);
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& r = host.conn->raid();
        if (id == "RAIDOPTIONS_CurrentLootType") {
            if (r.active) out.text = std::to_string(r.loot_type);
        } else if (id == "RAIDOPTIONS_SetLootTypeCombo") out.enabled = r.active && is_leader(host);
        else if (int b = class_button(id); b >= 0) out.color = argb_to_rgb(raid_.class_color(b));
    }
    const ComboData* combo(EqHost& host, std::string_view id) override {
        if (!host.conn || id != "RAIDOPTIONS_SetLootTypeCombo") return nullptr;
        // The XML's choices; CComboWnd::SetChoice(loot type - 1) in a raid.
        types_.choices = {"Leader Only", "Leader and Group Leaders", "Leader Selected", "Entire Raid"};
        std::uint32_t t = host.conn->raid().loot_type;
        if (host.conn->raid().active) types_.selected = t >= 1 && t <= 4 ? static_cast<int>(t) - 1 : -1;
        return &types_;
    }
    bool combo_select(EqHost& host, std::string_view id, int index) override {
        if (!host.conn || id != "RAIDOPTIONS_SetLootTypeCombo") return false;
        if (index >= 0 && index < 4) host.conn->raid_set_loot_type(std::to_string(index + 1));
        return true;
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || id != "RAIDOPTIONS_LooterList") return nullptr;
        const auto& r = host.conn->raid();
        looters_ = {};
        auto add = [&](const std::string& n) { looters_.rows.push_back(ListRow{{n}}); };
        if (!r.leader.empty()) add(r.leader);
        if (r.loot_type == 2) {
            for (const auto& m : r.members)
                if (m.group_leader && !iequals(m.name, r.leader)) add(m.name);
        } else if (r.loot_type == 3) {
            for (const auto& n : r.looters) add(n);
        } else if (r.loot_type == 4) {
            looters_.rows.clear();
            add("Entire Raid");
        }
        return &looters_;
    }
    bool list_click(EqHost&, std::string_view id, int, bool, int) override { return id == "RAIDOPTIONS_LooterList"; }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        int b = class_button(id);
        if (b < 0 || button != "left") return false;
        // CColorPickerWnd::Open(this, the button's colour); its answer (0x25) is the class's colour.
        RaidWindow& raid = raid_;
        host.pick_color(raid.class_color(b), [&raid, b](std::uint32_t argb) { raid.set_class_color(b, argb); }, this);
        return true;
    }

private:
    static bool is_leader(EqHost& host) { return iequals(host.conn->raid().leader, host.conn->character_name()); }
    static bool iequals(const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    }
    // RAIDOPTIONS_ClassColor0..15 → 0..15, else -1.
    static int class_button(std::string_view id) {
        constexpr std::string_view kPrefix = "RAIDOPTIONS_ClassColor";
        if (id.size() <= kPrefix.size() || id.substr(0, kPrefix.size()) != kPrefix) return -1;
        std::string_view n = id.substr(kPrefix.size());
        if (n.empty() || n.size() > 2 || !std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; })) return -1;
        int v = std::atoi(std::string(n).c_str());
        return v < 16 ? v : -1;
    }
    RaidWindow& raid_;
    ComboData types_;
    ListData looters_;
};

// LFGuildWnd (TOGGLE_LFGUILDWIN; CLFGuildWnd is not decoded, this follows its XML over EQEmu's OP_LFGuild): our
// player entry (comment, time zone, on / off, Update), the guild's (the classes and levels it wants, a minimum AA
// count, comment, time zone, on / off, Update), and the two searches with their matches (a selected match's comment
// below them). The time zone list is this client's: index 0 none, then UTC-12 .. UTC+14.
class LFGuildWindow : public StockWindow {
public:
    LFGuildWindow() : StockWindow("LFGuildWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->lfguild().open; }
    void set_shown(EqHost& host, bool show) override {
        if (!host.conn) return;
        host.conn->set_lfguild_open(show);
        if (show) {
            const auto& l = host.conn->lfguild();
            player_comment_ = l.player.comment;
            player_tz_.selected = static_cast<int>(l.player.time_zone);
            guild_ = l.guild;
            guild_tz_.selected = static_cast<int>(l.guild.time_zone);
        }
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& l = host.conn->lfguild();
        const auto* me = host.conn->world().player();
        if (id == "PLFG_CommentsEdit") out.text = player_comment_;
        else if (id == "PLFG_LevelClassLabel")
            out.text = me ? std::format("Level {} {}", me->level, game::class_name(me->class_id)) : std::string();
        else if (id == "PLFG_OnButton") out.checked = l.player.on;
        else if (id == "PLFG_OffButton") out.checked = !l.player.on;
        else if (id == "PLFG_ExpireLabel") out.text = posted(l.player.on, l.player.posted);
        else if (id == "GLFP_CommentsEdit") out.text = guild_.comment;
        else if (id == "GLFP_LevelRangeMinEditbox") out.text = std::to_string(guild_.from_level);
        else if (id == "GLFP_LevelRangeMaxEditbox") out.text = std::to_string(guild_.to_level);
        else if (id == "GLFP_MinAACountEdit") out.text = std::to_string(guild_.aa);
        else if (id == "GLFP_OnButton" || id == "GLFP_OffButton") {
            out.checked = (id == "GLFP_OnButton") == l.guild.on;
            out.enabled = host.conn->guild().in_guild();
        } else if (id == "GLFP_ExpireLabel") out.text = posted(l.guild.on, l.guild.posted);
        else if (id == "GLFP_UpdateButton") out.enabled = host.conn->guild().in_guild();
        else if (int c = class_box(id, "GLFP_"); c >= 0) out.checked = (guild_.classes >> c) & 1;
        else if (int d = class_box(id, "GSFP_"); d >= 0) out.checked = (search_classes_ >> d) & 1;
        else if (id == "GSFP_LevelRangeMinEditbox") out.text = std::to_string(search_from_);
        else if (id == "GSFP_LevelRangeMaxEditbox") out.text = std::to_string(search_to_);
        else if (id == "GSFP_MinAACountEdit") out.text = std::to_string(search_aa_);
        else if (id == "PSFG_LevelEdit") out.text = std::to_string(guild_search_level_ ? guild_search_level_ : (me ? me->level : 1));
        else if (id == "PSFG_AACountEdit") out.text = std::to_string(guild_search_aa_);
        else if (id == "GSFP_Comment") {
            const auto& m = l.players;
            out.text = player_sel_ >= 0 && player_sel_ < static_cast<int>(m.size())
                           ? stml_escape(m[static_cast<std::size_t>(player_sel_)].comment) : std::string();
        } else if (id == "PSFG_Comment") {
            const auto& m = l.guilds;
            out.text = guild_sel_ >= 0 && guild_sel_ < static_cast<int>(m.size())
                           ? stml_escape(m[static_cast<std::size_t>(guild_sel_)].comment) : std::string();
        }
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        auto num = [&](std::uint32_t& v) { v = static_cast<std::uint32_t>(std::max(0, std::atoi(text.c_str()))); };
        if (id == "PLFG_CommentsEdit") player_comment_ = text.substr(0, 255);
        else if (id == "GLFP_CommentsEdit") guild_.comment = text.substr(0, 255);
        else if (id == "GLFP_LevelRangeMinEditbox") num(guild_.from_level);
        else if (id == "GLFP_LevelRangeMaxEditbox") num(guild_.to_level);
        else if (id == "GLFP_MinAACountEdit") num(guild_.aa);
        else if (id == "GSFP_LevelRangeMinEditbox") num(search_from_);
        else if (id == "GSFP_LevelRangeMaxEditbox") num(search_to_);
        else if (id == "GSFP_MinAACountEdit") num(search_aa_);
        else if (id == "PSFG_LevelEdit") num(guild_search_level_);
        else if (id == "PSFG_AACountEdit") num(guild_search_aa_);
        else return false;
        return true;
    }
    const ComboData* combo(EqHost&, std::string_view id) override {
        ComboData* c = id == "PLFG_TimeZoneCombo" ? &player_tz_ : id == "GLFP_TimeZoneCombo" ? &guild_tz_
                     : id == "GSFP_TimeZoneCombo" ? &search_tz_ : id == "PSFG_TimeZoneCombo" ? &guild_search_tz_ : nullptr;
        if (c) {
            c->choices = {"None"};
            for (int h = -12; h <= 14; ++h) c->choices.push_back(std::format("UTC{:+}", h));
            if (c->selected < 0 || c->selected >= static_cast<int>(c->choices.size())) c->selected = 0;
            return c;
        }
        if (id == "PSFG_ClassCombo") {
            class_.choices.clear();
            for (std::uint32_t k = 1; k <= 16; ++k) class_.choices.push_back(game::class_name(k));
            if (class_.selected < 0) class_.selected = 0;
            return &class_;
        }
        return nullptr;
    }
    bool combo_select(EqHost&, std::string_view id, int index) override {
        if (id == "PLFG_TimeZoneCombo") player_tz_.selected = index;
        else if (id == "GLFP_TimeZoneCombo") guild_tz_.selected = index;
        else if (id == "GSFP_TimeZoneCombo") search_tz_.selected = index;
        else if (id == "PSFG_TimeZoneCombo") guild_search_tz_.selected = index;
        else if (id == "PSFG_ClassCombo") class_.selected = index;
        else return false;
        return true;
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& l = host.conn->lfguild();
        if (id == "GSFP_SearchResultList") {
            players_ = {};
            players_.columns = 4;
            players_.labels = {"Name", "Level", "Class", "AA"};
            players_.widths = {120, 45, 110, 45};
            for (const auto& m : l.players)
                players_.rows.push_back(ListRow{{m.name, std::to_string(m.level), game::class_name(m.class_), std::to_string(m.aa)}});
            players_.selected = player_sel_;
            return &players_;
        }
        if (id == "PSFG_SearchResultList") {
            guilds_ = {};
            guilds_.columns = 2;
            guilds_.labels = {"Guild", "Time Zone"};
            guilds_.widths = {200, 80};
            for (const auto& m : l.guilds) guilds_.rows.push_back(ListRow{{m.name, zone_text(m.time_zone)}});
            guilds_.selected = guild_sel_;
            return &guilds_;
        }
        return nullptr;
    }
    bool list_click(EqHost&, std::string_view id, int row, bool, int) override {
        if (id == "GSFP_SearchResultList") player_sel_ = row;
        else if (id == "PSFG_SearchResultList") guild_sel_ = row;
        else return false;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        auto post_player = [&](bool on) {
            net::LFGuildPlayer p;
            p.comment = player_comment_;
            p.time_zone = static_cast<std::uint32_t>(std::max(0, player_tz_.selected));
            p.on = on;
            c.lfguild_post_player(p);
        };
        auto post_guild = [&](bool on) {
            net::LFGuildGuild g = guild_;
            g.time_zone = static_cast<std::uint32_t>(std::max(0, guild_tz_.selected));
            g.on = on;
            c.lfguild_post_guild(g);
        };
        if (id == "PLFG_UpdateButton") post_player(c.lfguild().player.on);
        else if (id == "PLFG_OnButton") post_player(true);
        else if (id == "PLFG_OffButton") post_player(false);
        else if (id == "GLFP_UpdateButton") post_guild(c.lfguild().guild.on);
        else if (id == "GLFP_OnButton") post_guild(true);
        else if (id == "GLFP_OffButton") post_guild(false);
        else if (id == "GLFP_SelectAllRolesButton") guild_.classes = 0xffff;
        else if (id == "GLFP_DeselectAllRolesButton") guild_.classes = 0;
        else if (id == "GSFP_SelectAllRolesButton") search_classes_ = 0xffff;
        else if (id == "GSFP_DeselectAllRolesButton") search_classes_ = 0;
        else if (int k = class_box(id, "GLFP_"); k >= 0) guild_.classes ^= 1u << k;
        else if (int m = class_box(id, "GSFP_"); m >= 0) search_classes_ ^= 1u << m;
        else if (id == "GSFP_SearchButton") {
            player_sel_ = -1;
            c.lfguild_search_players(search_from_, search_to_, search_aa_, static_cast<std::uint32_t>(std::max(0, search_tz_.selected)),
                                     search_classes_);
        } else if (id == "PSFG_SearchButton") {
            guild_sel_ = -1;
            const auto* me = c.world().player();
            c.lfguild_search_guilds(guild_search_level_ ? guild_search_level_ : (me ? me->level : 1), guild_search_aa_,
                                    static_cast<std::uint32_t>(std::max(0, guild_search_tz_.selected)),
                                    static_cast<std::uint32_t>(std::max(0, class_.selected) + 1));
        } else return false;
        return true;
    }

private:
    // The class bit a GLFP_ / GSFP_<abbreviation>Checkbox names (bit 0 warrior .. 15 berserker), else -1.
    static int class_box(std::string_view id, std::string_view prefix) {
        static constexpr const char* kAbbr[16] = {"WAR", "CLR", "PAL", "RNG", "SHD", "DRU", "MNK", "BRD",
                                                  "ROG", "SHM", "NEC", "WIZ", "MAG", "ENC", "BST", "BER"};
        if (id.rfind(prefix, 0) != 0) return -1;
        for (int i = 0; i < 16; ++i)
            if (id == std::string(prefix) + kAbbr[i] + "Checkbox") return i;
        return -1;
    }
    static std::string zone_text(std::uint32_t tz) { return tz == 0 || tz > 27 ? std::string("-") : std::format("UTC{:+}", static_cast<int>(tz) - 13); }
    static std::string posted(bool on, std::uint32_t when) {
        if (!on || !when) return {};
        std::time_t t = static_cast<std::time_t>(when);
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", std::localtime(&t));
        return std::string("Posted ") + buf;
    }

    std::string player_comment_;
    net::LFGuildGuild guild_;
    std::uint32_t search_from_ = 1, search_to_ = 100, search_aa_ = 0, search_classes_ = 0xffff;
    std::uint32_t guild_search_level_ = 0, guild_search_aa_ = 0;
    ComboData player_tz_, guild_tz_, search_tz_, guild_search_tz_, class_;
    ListData players_, guilds_;
    int player_sel_ = -1, guild_sel_ = -1;
};

// GuildBankWnd (CGuildBankWnd) over the connection's guild bank
// (net/game_connection_guild_bank.cpp). The two lists show the items you may see (permission 15, else your own),
// the main list through the filter; one selection across both. The buttons follow CGuildBankWnd_UpdateButtons and
// WndNotification: Withdraw and Split ask how many in the quantity dialog, an item's permission Single Member and
// Change Member ask for the member's name in the text entry window, and an Alt click on a row views its item.
// A right click selects and views (an Alt right click asks for the item's 3D preview, EqHost::preview_item); a column
// header sorts the list (CListWnd's sort with WndNotification 0x10: the icon, quantity and the main list's permission
// by number, the other columns by text).
class GuildBankWindow : public StockWindow {
public:
    GuildBankWindow() : StockWindow("GuildBankWnd", false) {}

    void frame(EqHost& host) override {
        const auto serial = shown(host) ? host.conn->guild_bank().serial : 0;
        if (serial != session_serial_) {
            session_serial_ = serial;
            asking_ = asking_quantity_ = false;
        }
    }

    bool shown(EqHost& host) override { return host.conn && host.conn->guild_bank().banker; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn) host.conn->close_guild_bank();
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        const auto& b = c.guild_bank();
        // sub_0067dff0 / sub_0067e240 count every item for the labels (eqstr 6099).
        if (id == "BankCountLabel") {
            out.text = eqstr(host, 6099, {std::to_string(net::kGuildBankMainSlots - count(b.main))});
            return;
        }
        if (id == "DepositCountLabel") {
            out.text = eqstr(host, 6099, {std::to_string(net::kGuildBankDepositSlots - count(b.deposit))});
            return;
        }
        // CGuildBankWnd_UpdateButtons @0x67db70.
        bool pending = b.pending > 0;
        std::int32_t key = b.selected;
        const net::GuildBankItem* it = c.guild_bank_item(key);
        bool deposit = it && (key & 0xffff0000) == 0, main = it && (key & 0xffff0000) == 0x10000;
        auto perm = [&](std::int32_t p) { return c.has_guild_permission(p); };
        if (id == "DepositButton")
            out.enabled = count(b.deposit) < net::kGuildBankDepositSlots && c.inventory().cursor() && perm(game::kGuildPermBankDeposit) && !pending;
        else if (id == "ViewItemButton") out.enabled = !pending && it && c.guild_bank_can_view(key);
        else if (id == "WithdrawButton") out.enabled = !pending && it && c.guild_bank_can_withdraw(key); // with SelectItem's
        else if (id == "PermissionCombo") out.enabled = main && !pending && perm(game::kGuildPermBankPermissions);
        else if (id == "ChangeTargetButton")
            out.enabled = main && !pending && it->permission == net::GuildBankPermission::SingleMember && perm(game::kGuildPermBankPermissions);
        else if (id == "MergeButton") out.enabled = main && !pending && it->allow_merge && perm(game::kGuildPermBankPermissions);
        else if (id == "SplitButton") out.enabled = main && !pending && it->quantity >= 2 && perm(game::kGuildPermBankPermissions);
        else if (id == "PromoteButton") out.enabled = deposit && !pending && perm(game::kGuildPermBankPromote);
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& c = *host.conn;
        const auto& b = c.guild_bank();
        bool main = id == "ItemList";
        if (!main && id != "DepositList") return nullptr;
        ListData& l = main ? main_list_ : deposit_list_;
        auto& keys = main ? main_keys_ : deposit_keys_;
        // The XML's columns: the icon, Item Name, Qty, and Permissions or Depositor.
        l = main ? make_list({{"", 24}, {"Item Name", 200}, {"Qty", 30}, {"Permissions", 100}})
                 : make_list({{"", 24}, {"Item Name", 180}, {"Qty", 30}, {"Depositor", 100}});
        l.alt_right_click = true; // the 3D preview
        keys.clear();
        bool see_all = c.has_guild_permission(game::kGuildPermBankView);
        const auto& area = main ? b.main : b.deposit;
        for (std::size_t i = 0; i < area.size(); ++i) {
            const auto& it = area[i];
            if (!it) continue;
            std::int32_t key = net::guild_bank_key(main ? net::GuildBankArea::Main : net::GuildBankArea::Deposit, static_cast<int>(i));
            if (!see_all && !c.guild_bank_mine(key)) continue;
            if (main && !filter_accepts(c, *it, key)) continue;
            // CGuildBankWnd_SetItemRow @0x67d690: a quantity only for a stackable item; the main list's permission
            // (the member for a single member) and its colour (0xffc8a0a0 when you may not withdraw it).
            ListRow row{{"", it->name, it->allow_merge ? std::to_string(it->quantity) : std::string(" "),
                         main ? permission_text(host, *it) : it->donator}};
            if (host.textures) row.icon = host.textures->item_icon(it->icon);
            if (main && !c.guild_bank_can_withdraw(key)) row.cell_colors = {std::nullopt, kRefused, kRefused, kRefused};
            keys.push_back(key);
            l.rows.push_back(std::move(row));
        }
        // WndNotification 0x10: column 0 by the icon (+0xc), 2 by the quantity (+0x10), 3 in the main list by the
        // permission (+0x14); the name and the depositor by their text.
        auto by_key = [&](std::size_t ka, std::size_t kb) -> std::optional<int> {
            const net::GuildBankItem* x = c.guild_bank_item(keys[ka]);
            const net::GuildBankItem* y = c.guild_bank_item(keys[kb]);
            if (!x || !y) return 0;
            auto sign = [](std::int64_t d) { return d < 0 ? -1 : d > 0 ? 1 : 0; };
            ListSort& sort = main ? main_sort_ : deposit_sort_;
            if (sort.column == 0) return sign(static_cast<std::int64_t>(x->icon) - y->icon);
            if (sort.column == 2) return sign(static_cast<std::int64_t>(x->quantity) - y->quantity);
            if (sort.column == 3 && main) return sign(static_cast<std::int64_t>(x->permission) - static_cast<std::int64_t>(y->permission));
            return std::nullopt;
        };
        const ListSort& sort = main ? main_sort_ : deposit_sort_;
        ListSort::apply(l, keys, sort.order(l.rows, by_key));
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == b.selected) l.selected = static_cast<int>(i);
        return &l;
    }
    bool list_header_click(EqHost&, std::string_view id, int column) override {
        // CListWnd's own sort; then 0xf puts the selection back in view (CListWnd_FindLineByData, as rebuilt here).
        if (id == "ItemList") main_sort_.header_click(column);
        else if (id == "DepositList") deposit_sort_.header_click(column);
        else return false;
        return true;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int) override {
        bool main = id == "ItemList";
        if (!main && id != "DepositList") return false;
        if (!host.conn) return true;
        const auto& keys = main ? main_keys_ : deposit_keys_;
        if (row < 0 || row >= static_cast<int>(keys.size())) return true;
        // WndNotification 1 (a click): CGuildBankWnd_SelectItem, and with Alt (keyboard flags & 0xc) the item's view;
        // 3 (a right click): the selection, then the view, or with Alt the 3D preview (sub_0067cb80).
        select(host, keys[static_cast<std::size_t>(row)]);
        bool alt = ImGui::GetIO().KeyAlt;
        if (!right) {
            if (alt) view(host);
        } else if (!alt) {
            view(host);
        } else if (const net::GuildBankItem* it = host.conn->guild_bank_item(keys[static_cast<std::size_t>(row)])) {
            if (host.preview_item) host.preview_item(it->item_id);
        }
        return true;
    }

    const ComboData* combo(EqHost& host, std::string_view id) override {
        if (id == "FilterCombo") {
            // The XML's choices; 0x67da70 reads the index.
            filter_.choices = {"Show All", "Public Only", "Useable Only", "Withdrawable", "Not Withdrawable"};
            if (filter_.selected < 0) filter_.selected = 0;
            return &filter_;
        }
        if (id == "PermissionCombo") {
            permission_.choices.clear();
            for (std::uint32_t s = 6103; s <= 6106; ++s) permission_.choices.push_back(eqstr(host, s)); // table 0xb276cc
            const net::GuildBankItem* it = host.conn ? host.conn->guild_bank_item(host.conn->guild_bank().selected) : nullptr;
            permission_.selected = asking_ ? 1 : it ? static_cast<int>(it->permission) : 0;
            return &permission_;
        }
        return nullptr;
    }
    bool combo_select(EqHost& host, std::string_view id, int index) override {
        if (id == "FilterCombo") {
            filter_.selected = index;
            return true;
        }
        if (id != "PermissionCombo" || !host.conn) return false;
        auto& c = *host.conn;
        std::int32_t key = c.guild_bank().selected;
        if (key == -1) return true;
        const net::GuildBankItem* it = c.guild_bank_item(key);
        if (index != 1 || !it) {
            c.guild_bank_set_permission(static_cast<net::GuildBankPermission>(index), std::string());
            return true;
        }
        // Single Member: the member's name, the item's (else your own) to start with.
        ask_member(host, it->who_for.empty() ? c.character_name() : it->who_for);
        return true;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        const net::GuildBankItem* it = c.guild_bank_item(c.guild_bank().selected);
        if (id == "DepositButton") c.guild_bank_deposit();
        else if (id == "ViewItemButton") view(host);
        else if (id == "PromoteButton") {
            if (it) c.guild_bank_send(net::GuildBankAction::Promote, 0);
        } else if (id == "MergeButton") {
            if (it) c.guild_bank_send(net::GuildBankAction::Merge, 0);
        } else if (id == "SplitButton") {
            // Up to half the stack; the split goes when the stack keeps at least one.
            if (it && it->quantity > 1) {
                std::uint32_t qty = it->quantity;
                ask_quantity(host, static_cast<std::int32_t>(qty / 2), [&host, qty](std::int32_t n) {
                    if (host.conn && static_cast<std::int64_t>(n) + 1 <= qty)
                        host.conn->guild_bank_send(net::GuildBankAction::Split, static_cast<std::uint32_t>(n));
                });
            }
        } else if (id == "WithdrawButton") {
            if (!it || it->quantity < 2) c.guild_bank_send(net::GuildBankAction::Withdraw, 1);
            else ask_quantity(host, static_cast<std::int32_t>(it->quantity), [&host](std::int32_t n) {
                if (host.conn && host.conn->guild_bank_item(host.conn->guild_bank().selected))
                    host.conn->guild_bank_send(net::GuildBankAction::Withdraw, static_cast<std::uint32_t>(n));
            });
        } else if (id == "ChangeTargetButton") {
            if (it) ask_member(host, it->who_for);
        } else return false;
        return true;
    }

private:
    static constexpr Rgb kRefused{0xc8 / 255.0f, 0xa0 / 255.0f, 0xa0 / 255.0f};

    static int count(const std::vector<std::optional<net::GuildBankItem>>& area) {
        return static_cast<int>(std::count_if(area.begin(), area.end(), [](const auto& it) { return it.has_value(); }));
    }
    static std::string permission_text(EqHost& host, const net::GuildBankItem& it) {
        if (it.permission == net::GuildBankPermission::SingleMember) return it.who_for;
        auto p = static_cast<std::uint32_t>(it.permission);
        return p <= 3 ? eqstr(host, 6103 + p) : std::string();
    }
    // 0x67da70: 1 public, 2 public if usable, 3 withdrawable, 4 not withdrawable, else all.
    bool filter_accepts(const net::GameConnection& c, const net::GuildBankItem& it, std::int32_t key) const {
        switch (filter_.selected) {
        case 1: return it.permission == net::GuildBankPermission::Public;
        case 2: return it.permission == net::GuildBankPermission::PublicIfUsable;
        case 3: return c.guild_bank_can_withdraw(key);
        case 4: return !c.guild_bank_can_withdraw(key);
        default: return true;
        }
    }
    void select(EqHost& host, std::int32_t key) {
        // CGuildBankWnd_SelectItem: its quantity and text entry questions close.
        host.conn->guild_bank_select(key);
        if (asking_quantity_ && host.quantity && host.quantity->serial == quantity_serial_) host.quantity.reset();
        if (asking_ && host.text_entry) host.text_entry.reset();
        asking_ = asking_quantity_ = false;
    }
    void view(EqHost& host) {
        // Send_OP_GuildBank_0067cc00: 1 unless Shift is held.
        if (host.conn->guild_bank_item(host.conn->guild_bank().selected))
            host.conn->guild_bank_send(net::GuildBankAction::View, ImGui::GetIO().KeyShift ? 0 : 1);
    }
    void ask_quantity(EqHost& host, std::int32_t max, std::function<void(std::int32_t)> done) {
        // CQuantityWnd::Open(window, max, start 1, the button's centre, keys, min 0).
        game::QuantityOpen q;
        q.max = max;
        q.start = 1;
        if (host.click_rect[0] >= 0) {
            q.x = static_cast<std::int32_t>((host.click_rect[0] + host.click_rect[2]) / 2);
            q.y = static_cast<std::int32_t>((host.click_rect[1] + host.click_rect[3]) / 2);
        }
        open_quantity(host, q, std::move(done), question_owner(host));
        asking_quantity_ = host.quantity.has_value();
        if (host.quantity) quantity_serial_ = host.quantity->serial;
    }
    void ask_member(EqHost& host, const std::string& name) {
        // CTextEntryWnd::Activate(window, 1, eqstr 6109, 0, 64, name); OK sends Single Member to that name when the
        // permission choice is still Single Member, permission 17 held and an item selected (notification 0x26).
        EqHost::TextEntry e;
        e.prompt = eqstr(host, 6109);
        e.max_chars = 64;
        e.text = name;
        e.owner_valid = question_owner(host);
        e.done = [this, &host, valid = e.owner_valid](const std::string& text) {
            asking_ = false;
            if (!valid() || !host.conn->has_guild_permission(game::kGuildPermBankPermissions)) return;
            host.conn->guild_bank_set_permission(net::GuildBankPermission::SingleMember, text);
        };
        host.text_entry = std::move(e);
        asking_ = true;
    }

    static std::function<bool()> question_owner(EqHost& host) {
        const auto* conn = host.conn;
        const auto serial = conn->guild_bank().serial;
        const auto selected = conn->guild_bank().selected;
        return [&host, conn, serial, selected] {
            return host.conn == conn && conn && conn->guild_bank().banker && conn->guild_bank().serial == serial &&
                   conn->guild_bank().selected == selected;
        };
    }

    ListData main_list_, deposit_list_;
    std::vector<std::int32_t> main_keys_, deposit_keys_;
    ListSort main_sort_, deposit_sort_;
    ComboData filter_, permission_;
    bool asking_ = false, asking_quantity_ = false; // a member name or quantity question of ours is open
    int quantity_serial_ = 0;
    std::uint64_t session_serial_ = 0;
};

} // namespace

void add_social_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    auto raid = std::make_unique<RaidWindow>();
    RaidWindow& raid_window = *raid;
    out.push_back(std::make_unique<GuildBankWindow>());
    out.push_back(std::make_unique<LFGuildWindow>());
    out.push_back(std::make_unique<RaidOptionsWindow>(raid_window));
    out.push_back(std::make_unique<FriendsWindow>());
    out.push_back(std::make_unique<GuildWindow>());
    out.push_back(std::move(raid));
}

namespace {

// count, players = eq.WhoResults()   -- the last /who reply (GameConnection::last_who): how many replies have come
// (0: none yet), and its players as { name, level, class, race (ids), raceName, guild, zone (short name, "" when the
// reply leaves it out), anonymous (format 5024), roleplay (5023), afk, linkdead, trader, buyer, offline (the flag
// bits), line (the player's line as the chat window printed it) }. RoF2 shows /who only in the chat window; the
// gamepad pack's Who window lists it.
int WhoResults(lua_State* L) {
    auto& h = *static_cast<EqHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    const net::WhoResponse* r = h.conn ? h.conn->last_who() : nullptr;
    lua_pushinteger(L, h.conn ? h.conn->who_count() : 0);
    lua_newtable(L);
    if (!r) return 2;
    auto set_str = [&](const char* k, const std::string& v) {
        lua_pushlstring(L, v.data(), v.size());
        lua_setfield(L, -2, k);
    };
    auto set_int = [&](const char* k, lua_Integer v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, k);
    };
    auto set_bool = [&](const char* k, bool v) {
        lua_pushboolean(L, v);
        lua_setfield(L, -2, k);
    };
    int n = 0;
    for (const auto& e : r->entries) {
        if (e.format == 5026 || e.format == 5027) continue; // "ZONE: %1 is UP": not a player
        // The player's own line: the reply cut down to this entry, without its title and footer.
        net::WhoResponse one;
        one.entries.push_back(e);
        auto ext = h.conn->server_ext().who_lines(*h.conn, one);
        auto lines = ext ? std::move(*ext) : game::format_who_response(one, h.conn->strings(), false);
        lua_newtable(L);
        set_str("name", e.name);
        set_int("level", e.level);
        set_int("class", e.class_);
        set_int("race", e.race);
        set_str("raceName", e.race ? game::race_name(e.race) : std::string());
        set_str("guild", e.guild);
        std::string zone;
        if (e.zone_string != 0xffffffff) zone = (e.zone_id >> 15) & 1 ? e.zone_name : game::zone_short_name(e.zone_id);
        set_str("zone", zone);
        set_bool("anonymous", e.format == 5024);
        set_bool("roleplay", e.format == 5023);
        set_bool("afk", (e.flags & 1) != 0);
        set_bool("linkdead", (e.flags & 8) != 0);
        set_bool("trader", (e.flags & 0x10) != 0);
        set_bool("buyer", (e.flags & 0x20) != 0);
        set_bool("offline", (e.flags & 0x400) != 0);
        set_str("line", lines.empty() ? e.name : lines.front());
        lua_rawseti(L, -2, ++n);
    }
    return 2;
}

} // namespace

void register_social_api(lua_State* L, EqHost& h) {
    static const luaL_Reg fns[] = {{"WhoResults", WhoResults}, {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
