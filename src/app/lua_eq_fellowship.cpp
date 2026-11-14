// Fellowship, alert history and trophy tribute window models.

#include <algorithm>
#include <ctime>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "app/lua_eq_windows.h"
#include "core/game_dir.h"
#include "game/eq_names.h"
#include "game/fellowship_categories.h"
#include "game/zone_names.h"
#include "net/game_connection.h"
#include "platform/local_time.h"

namespace hailstone::app {

namespace {

// CFellowshipWnd::UpdateDisplay @0x667ed0 and WndNotification @0x669440: roster, leader actions, personal XP
// sharing and campsite kits. Selection follows character ids, including when an expanded roster is sorted.
class FellowshipWindow : public StockWindow {
public:
    FellowshipWindow() : StockWindow("FellowshipWnd", false) {
        filter_.choices = {"All categories"}, filter_.selected = 0;
        members_.columns = 7;
        members_.labels = {"Name", "Lvl", "Class", "L", "Zone", "Last On", "XP Sharing"};
        members_.widths = {100, 45, 60, 25, 100, 80, 80};
        kits_.columns = 2;
        kits_.labels = {"", "Campsite Item"};
        kits_.widths = {10, 500};
    }

    void set_shown(EqHost& host, bool show) override {
        if (show && !open_ && host.conn) refresh(host);
        open_ = show;
    }

    std::string title(EqHost& host) override {
        if (!host.conn || !host.conn->fellowship().active()) return "Fellowship";
        return "Fellowship (" + std::to_string(host.conn->fellowship().members.size()) + " members)";
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        sync(host);
        if (!host.conn) return;
        const auto& c = *host.conn;
        const auto& f = c.fellowship();
        const auto* m = selected_member(host);
        const auto* kit = selected_kit(host);
        const bool ready = c.state() == net::ConnectionState::InZone;
        const bool active = f.active();
        const bool leader = f.is_leader(c.character_name());
        const auto* own = f.member(c.character_name());
        const bool self = m && own && m->character_id == own->character_id;
        if (id == "CreateFellowshipButton") out.enabled = ready && !active;
        else if (id == "LeaveFellowshipButton") out.enabled = ready && active;
        else if (id == "EndFellowshipButton") out.enabled = ready && leader;
        else if (id == "MakeLeaderButton" || id == "RemoveMemberButton") out.enabled = ready && leader && m && !self;
        else if (id == "InviteMemberButton") out.enabled = ready && leader && f.members.size() < net::kMaxFellowshipMembers;
        else if (id == "MOTDInput" || id == "UpdateMOTDButton") out.enabled = ready && leader;
        else if (id == "ToggleExpSharingButton") out.enabled = ready && m && (self || leader) && (!m->capped || m->sharing);
        else if (id == "CreateCampsite") out.enabled = ready && active && !f.camp.active && kit && kit->available;
        else if (id == "DestroyCampsite") out.enabled = ready && active && f.camp.active;
        else if (id == "RefreshListButton") out.enabled = ready;
        if (id == "MOTDInput") out.text = motd_;
        else if (id == "FellowshipVitality")
            out.text = std::format("XP vitality: {} / {}    AA vitality: {} / {}", f.vitality.normal,
                                   f.vitality.normal_cap, f.vitality.aa, f.vitality.aa_cap);
        else if (id == "ShowAllButton") out.checked = show_all_;
        else if (id == "CurrentCampLoc") out.text = f.camp.active ? game::zone_long_name(f.camp.zone_id) : "";
        else if (id == "CampsiteViewer")
            out.text = f.camp.active ? std::format("Location: {:.1f}, {:.1f}, {:.1f}", f.camp.y, f.camp.x, f.camp.z) : "";
        else if (id == "CampsiteTime") {
            out.text = "";
            if (f.camp.active) {
                if (!f.camp.expires) out.text = "Remaining time unavailable";
                else {
                    const auto now = host.unix_time ? host.unix_time() : static_cast<std::int64_t>(std::time(nullptr));
                    const auto expiry = static_cast<std::int64_t>(f.camp.expires);
                    // A negative wall clock is treated as the epoch. This display
                    // never expires the campsite or changes its server-owned state.
                    const auto left = std::max<std::int64_t>(0, expiry - std::max<std::int64_t>(0, now));
                    out.text = left == 0 ? "Awaiting campsite update" :
                        std::format("Time remaining: {}:{:02d}:{:02d}", left / 3600, (left / 60) % 60, left % 60);
                }
            }
        }
        else if (id == "CampsiteDescription") {
            out.text = "";
            if (kit) {
                // CFellowshipWnd::WndNotification @0x66950a resolves the catalog
                // reference through DBString type 0, then passes its STML to the
                // description viewer. The UI's existing parser handles markup.
                const auto description = c.strings().db_string(kit->description_id, 0);
                out.text = stml_escape(kit->name) + "<br><br>" +
                    (description.empty() ? "No description supplied." : description) + "<br><br>" +
                    (kit->available ? "Available to create." : "Creation requirements are not met.");
            }
        }
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        sync(host);
        if (!host.conn) return nullptr;
        const auto& f = host.conn->fellowship();
        if (id == "MemberList") {
            members_.rows.clear();
            member_ids_.clear();
            members_.selected = -1;
            for (const auto& m : f.members) {
                const bool online = m.zone_id != 0 && !m.offline;
                ListRow row;
                row.texts = {m.name, std::to_string(m.level), game::class_short_name(m.class_),
                             !f.members.empty() && m.character_id == f.members.front().character_id ? "L" : "",
                             online ? game::zone_long_name(m.zone_id) : "Offline", online ? "Online" : last_on(m.last_seen),
                             m.capped ? "Capped" : m.sharing ? "On" : "Off"};
                row.color = online ? Rgb{1, 1, 1} : argb_to_rgb(0xffc0c0c0);
                members_.rows.push_back(std::move(row));
                member_ids_.push_back(m.character_id);
            }
            ListSort::apply(members_, member_ids_, member_sort_.order(members_.rows, [&](std::size_t a, std::size_t b) {
                if (member_sort_.column != 1) return std::optional<int>();
                const auto left = f.members[a].level, right = f.members[b].level;
                return std::optional<int>(left < right ? -1 : left > right ? 1 : 0);
            }));
            for (std::size_t i = 0; i < member_ids_.size(); ++i)
                if (member_ids_[i] == selected_member_) members_.selected = static_cast<int>(i);
            return &members_;
        }
        if (id == "CampsiteKitList") {
            kits_.rows.clear();
            kit_ids_.clear();
            kits_.selected = -1;
            for (const auto& kit : f.kits) {
                ListRow row{{kit.available ? "Yes" : "No", kit.name}};
                if (!kit.available) row.color = argb_to_rgb(0xffc0c0c0);
                if (kit.camp_type_id == selected_kit_) kits_.selected = static_cast<int>(kits_.rows.size());
                kits_.rows.push_back(std::move(row));
                kit_ids_.push_back(kit.camp_type_id);
            }
            return &kits_;
        }
        return nullptr;
    }

    bool list_click(EqHost& host, std::string_view id, int row, bool right, int) override {
        if (right) return id == "MemberList" || id == "CampsiteKitList";
        if (id == "MemberList") {
            if (row >= 0 && row < static_cast<int>(member_ids_.size())) selected_member_ = member_ids_[static_cast<std::size_t>(row)];
            sync(host);
            return true;
        }
        if (id == "CampsiteKitList") {
            if (row >= 0 && row < static_cast<int>(kit_ids_.size())) selected_kit_ = kit_ids_[static_cast<std::size_t>(row)];
            return true;
        }
        return false;
    }

    bool list_header_click(EqHost&, std::string_view id, int column) override {
        if (id != "MemberList" || column < 0 || column >= members_.columns) return false;
        member_sort_.header_click(column);
        return true;
    }

    bool set_text(EqHost& host, std::string_view id, const std::string& text) override {
        if (id != "MOTDInput" || !host.conn || !host.conn->fellowship().is_leader(host.conn->character_name())) return false;
        sync(host);
        motd_ = text.substr(0, 1023);
        return true;
    }

    const ComboData* combo(EqHost& host, std::string_view id) override {
        sync(host);
        return id == "CampsiteFilter" ? &filter_ : nullptr;
    }
    bool combo_select(EqHost& host, std::string_view id, int index) override {
        sync(host);
        if (id != "CampsiteFilter" || index < 0 || index >= static_cast<int>(filter_.choices.size())) return false;
        filter_.selected = index;
        refresh(host);
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (button != "left" || !host.conn) return false;
        sync(host);
        Child state;
        child(host, id, state);
        if (state.enabled == false) return true;
        auto& c = *host.conn;
        const auto* m = selected_member(host);
        if (id == "CreateFellowshipButton") c.fellowship_create();
        else if (id == "UpdateMOTDButton") c.fellowship_set_motd(motd_);
        else if (id == "InviteMemberButton") c.fellowship_invite_target();
        else if (id == "ToggleExpSharingButton") { if (m) c.fellowship_toggle_sharing(m->character_id); }
        else if (id == "CreateCampsite") c.fellowship_create_camp(selected_kit_);
        else if (id == "RefreshListButton") refresh(host);
        else if (id == "ShowAllButton") {
            show_all_ = !show_all_;
            refresh(host);
        } else if (id == "MakeLeaderButton" && m)
            confirm(host, "Make " + m->name + " the fellowship leader?", [member = m->character_id](auto& conn) { conn.fellowship_promote(member); });
        else if (id == "RemoveMemberButton" && m)
            confirm(host, "Remove " + m->name + " from the fellowship?", [member = m->character_id](auto& conn) { conn.fellowship_remove(member); });
        else if (id == "LeaveFellowshipButton")
            confirm(host, "Leave the fellowship?", [](auto& conn) { conn.fellowship_leave(); });
        else if (id == "EndFellowshipButton")
            confirm(host, "End the fellowship for all members?", [](auto& conn) { conn.fellowship_end(); });
        else if (id == "DestroyCampsite")
            confirm(host, "Destroy the fellowship campsite?", [](auto& conn) { conn.fellowship_destroy_camp(); });
        else return false;
        return true;
    }

private:
    void sync(EqHost& host) {
        // Called for every child, list and combo each frame: the category file is
        // looked up again only when the game folder changes or the window opens.
        if (!categories_loaded_ || category_game_ != host.game) load_categories(host);
        if (!host.conn) return;
        const auto& f = host.conn->fellowship();
        if (f.id != fellowship_id_ || f.motd != received_motd_) {
            motd_ = received_motd_ = f.motd;
            if (f.id != fellowship_id_) selected_member_ = selected_kit_ = 0;
            fellowship_id_ = f.id;
        }
        if (!f.member(selected_member_)) selected_member_ = 0;
    }
    const net::FellowshipMember* selected_member(EqHost& host) const {
        return host.conn->fellowship().member(selected_member_);
    }
    const net::FellowshipKit* selected_kit(EqHost& host) const {
        const auto& list = host.conn->fellowship().kits;
        const auto it = std::find_if(list.begin(), list.end(), [&](const auto& kit) { return kit.camp_type_id == selected_kit_; });
        return it == list.end() ? nullptr : &*it;
    }
    void load_categories(EqHost& host) {
        const auto old_mask = category_mask();
        categories_ = host.game ? game::load_fellowship_categories(*host.game) : std::vector<game::FellowshipCategory>{};
        filter_.choices = {"All categories"};
        filter_.selected = 0;
        for (const auto& category : categories_) {
            filter_.choices.push_back(category.name);
            if (category.mask() == old_mask) filter_.selected = static_cast<int>(filter_.choices.size()) - 1;
        }
        categories_loaded_ = true;
        category_game_ = host.game;
    }
    std::uint32_t category_mask() const {
        const auto selected = filter_.selected;
        return selected > 0 && static_cast<std::size_t>(selected) <= categories_.size()
            ? categories_[static_cast<std::size_t>(selected - 1)].mask() : 0xffffffffu;
    }
    void refresh(EqHost& host) {
        load_categories(host);
        if (host.conn) host.conn->fellowship_refresh_kits(!show_all_, category_mask());
    }
    void confirm(EqHost& host, std::string text, std::function<void(net::GameConnection&)> action) const {
        if (host.confirm) return;
        const auto id = host.conn->fellowship().id;
        auto* c = host.conn;
        EqHost::Confirm q;
        q.text = std::move(text);
        q.done = [c, id, action = std::move(action)](bool yes) {
            if (yes && c->fellowship().active() && c->fellowship().id == id) action(*c);
        };
        host.confirm = std::move(q);
    }
    static std::string last_on(std::uint32_t t) {
        if (!t) return {};
        const auto date = platform::local_time(static_cast<std::time_t>(t));
        return date ? std::format("{:02d}/{:02d}/{:02d}", date->tm_mon + 1, date->tm_mday, date->tm_year % 100) : "";
    }
    ListData members_, kits_;
    ListSort member_sort_;
    std::vector<std::uint32_t> member_ids_, kit_ids_;
    std::uint32_t selected_member_ = 0, selected_kit_ = 0, fellowship_id_ = 0;
    std::string motd_, received_motd_;
    ComboData filter_;
    std::vector<game::FellowshipCategory> categories_;
    const GameDir* category_game_ = nullptr;
    bool categories_loaded_ = false;
    bool show_all_ = false;
};

// CAlertHistoryWnd @0x61a9e0: AH_Closed_Alert_List (the alerts closed, from the alert list the server sends with
// opcode 0x42c4, Handle_AlertListUpdate @0x412a90, and the UI ini's [AlertsWindow]) and AH_Open_Alert_Btn, enabled
// while a row is selected (OnProcessFrame @0x61ab60); Open reopens the alert (CAlertHistoryWnd_ReopenAlert). EQEmu
// sends no alerts, so the list stays empty (AlertStackWnd and AlertWnd never show).
class AlertHistoryWindow : public StockWindow {
public:
    AlertHistoryWindow() : StockWindow("AlertHistoryWnd", false) {
        list_.columns = 3; // the XML's columns
        list_.labels = {"Title", "Alert Text", ""};
        list_.widths = {100, 350, 10};
    }

    void child(EqHost&, std::string_view id, Child& out) override {
        if (id == "AH_Open_Alert_Btn") out.enabled = list_.selected != -1;
    }
    const ListData* list(EqHost&, std::string_view id) override { return id == "AH_Closed_Alert_List" ? &list_ : nullptr; }

private:
    ListData list_;
};

// CTributeTrophyWnd @0x7881d0: AboutToShow @0x788fe0 refuses without the
// House of Thule expansion (the profile's expansion bit 0x10000; 5422 in colour 0xd), else asks for the trophy
// tribute (opcode 0x51a9, empty; not in EQEmu's RoF2 table) and shows the personal or guild view as its radio
// buttons say. CTributeWnd_UpdateGuildControls @0x5cb130: the personal view shows the cost, time, favour and career
// favour labels, the guild view the guild's cost, time and favour with the status text and box and the Modify
// button (5424 "Modify", 5423 "Save" while editing; enabled with the guild's trophy permission). The lists
// (TTW_AvailableTrophyBenefitList, TTW_ActiveTrophyBenefitList) are the trophy benefits the server sends, which EQEmu
// does not: they stay empty, and Upgrade / Downgrade with them. The favour labels are EQTypes 117 / 118 / 122 (the
// tribute's, app/lua_eq_values.cpp); the trophy cost and time (EQTypes 297..300) are not known without the data.
class TributeTrophyWindow : public StockWindow {
public:
    TributeTrophyWindow() : StockWindow("TributeTrophyWnd", false) {
        available_.columns = 4; // the XML's columns
        available_.labels = {"Benefit Name", "Tier", "Level", "Cost"};
        available_.widths = {120, 30, 50, 50};
        active_.columns = 3;
        active_.labels = {"Benefit Name", "Tier", "Cost"};
        active_.widths = {120, 30, 50};
    }

    void set_shown(EqHost& host, bool show) override {
        if (show && !open_) {
            if (!host.conn) return;
            const auto& p = host.conn->profile();
            if (!p || (p->expansions & 0x10000) == 0) {
                host.conn->show_eqstr(5422, {}, 0xd);
                return;
            }
            host.conn->send_zone_packet(net::AppPacket(kOpTrophyTributeRequest));
        }
        open_ = show;
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        static constexpr std::string_view kPersonal[] = {"TTW_CostLabel", "TTW_TimeLabel", "TTW_CurrentPointsLabel",
                                                         "TTW_TotalPointsText", "TTW_TotalPointsLabel"};
        static constexpr std::string_view kGuild[] = {"TTW_GuildCostLabel", "TTW_GuildTimeLabel",
                                                      "TTW_GuildCurrentPointsLabel", "TTW_StatusText", "TTW_StatusButton",
                                                      "TTW_LockButton"};
        for (auto p : kPersonal)
            if (id == p) out.shown = !guild_;
        for (auto g : kGuild)
            if (id == g) out.shown = guild_;
        if (id == "TTW_ViewPersonal_Btn") out.checked = !guild_;
        else if (id == "TTW_ViewGuild_Btn") out.checked = guild_;
        else if (id == "TTW_CurrentPointsLabel") out.text = std::to_string(host.conn->tribute().points);
        else if (id == "TTW_TotalPointsLabel") out.text = std::to_string(host.conn->tribute().career);
        else if (id == "TTW_GuildCurrentPointsLabel") out.text = std::to_string(host.conn->guild_tribute().active.favor);
        else if (id == "TTW_LockButton") {
            out.text = host.conn->strings().format(5424, {});
            out.enabled = false; // the guild's trophy permission (LocalPC_HasGuildPermission(0x19)) is not kept
        } else if (id == "TTW_UpgradeButton" || id == "TTW_DowngradeButton") {
            out.enabled = false;
        } else if (id == "TTW_Description") {
            out.text = "";
        }
    }
    const ListData* list(EqHost&, std::string_view id) override {
        if (id == "TTW_AvailableTrophyBenefitList") return &available_;
        if (id == "TTW_ActiveTrophyBenefitList") return &active_;
        return nullptr;
    }
    bool click(EqHost&, std::string_view id, std::string_view button, bool) override {
        if (button != "left") return false;
        if (id == "TTW_ViewPersonal_Btn") guild_ = false;
        else if (id == "TTW_ViewGuild_Btn") guild_ = true;
        else return false;
        return true;
    }

private:
    static constexpr std::uint16_t kOpTrophyTributeRequest = 0x51a9;
    ListData available_, active_;
    bool guild_ = false;
};

} // namespace

void add_fellowship_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<FellowshipWindow>());
    out.push_back(std::make_unique<AlertHistoryWindow>());
    out.push_back(std::make_unique<TributeTrophyWindow>());
}

} // namespace hailstone::app
