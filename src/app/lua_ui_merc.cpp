// The mercenary windows: MMTW_MerchantWnd (CMercenaryMerchantWnd), MMGW_ManageWnd (CMercenaryManageWnd) and
// MIW_InfoWnd (the info window) over GameConnection's mercenary manager (game/mercenary.h,
// net/game_connection_merc.cpp). What each child shows follows the original's: the merchant's
// type combo and subtype list (CMercenaryMerchantWnd_FillMercTypeList @0x6ecbb0), the costs as cost / 10 platinum
// and cost % 10 gold, the manage window's buttons by state (CMercenaryManageWnd_UpdateButtons @0x6e5770, the
// suspend button @0x6e63b0), TimeLeftText and its tooltip, and the owned list (the fill at 0x6e6c70).

#include <map>
#include <string>
#include <vector>

#include "app/lua_ui_windows.h"
#include "game/mercenary.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

std::string db(UiHost& host, std::uint32_t id, std::uint32_t kind) {
    return host.conn ? host.conn->strings().db_string(id, kind) : std::string();
}

// The account's membership as FreeToPlayClient::Instance() reports it (OP_SendMembership; EQEmu always sends gold,
// 2). Before world has sent it, gold.
std::uint32_t account_membership(UiHost& host) {
    return host.conn && host.conn->membership() ? host.conn->membership()->level : 2;
}

// The cost texts both the merchant and the info window show for a record.
void cost_child(const game::MercRecord& r, std::string_view id, Child& out) {
    bool alt = r.alt_currency != 0;
    if (id == "PurchaseCostPlatText") out.text = std::to_string(game::merc_cost_platinum(r.purchase_cost));
    else if (id == "PurchaseCostGoldText") out.text = std::to_string(game::merc_cost_gold(r.purchase_cost));
    else if (id == "UpkeepCostPlatText") out.text = std::to_string(game::merc_cost_platinum(r.upkeep_cost));
    else if (id == "UpkeepCostGoldText") out.text = std::to_string(game::merc_cost_gold(r.upkeep_cost));
    else if (id == "AltPurchaseCostText") out.text = std::to_string(r.alt_purchase_cost), out.shown = alt;
    else if (id == "AltUpkeepCostText") out.text = std::to_string(r.alt_upkeep_cost), out.shown = alt;
    else if (id == "AltPurchaseCostIcon" || id == "AltUpkeepCostIcon" || id == "AltCurrencyDividerText") out.shown = alt;
}

// The description (dbstr kind 23) and a stance's long description (kind 25) as the STML boxes show them.
void info_child(UiHost& host, const game::MercRecord& r, std::uint32_t stance, std::string_view id, Child& out) {
    if (id == "InfoText") out.text = db(host, r.subtype, 23);
    else if (id == "StanceInfoText") out.text = stance ? db(host, stance, 25) : std::string();
}

// ---------------------------------------------------------------- MMTW_MerchantWnd

class MercMerchantWindow : public StockWindow {
public:
    MercMerchantWindow() : StockWindow("MMTW_MerchantWnd", false) {}

    bool shown(UiHost& host) override { return host.conn && host.conn->merc_window_open(0); }
    void set_shown(UiHost& host, bool show) override {
        if (host.conn && show != host.conn->merc_window_open(0)) host.conn->set_merc_window_open(0, show);
    }

    void frame(UiHost& host) override {
        if (!host.conn) return;
        // Event 0xd (the list arrived): the type combo's first type and the subtype list's row 0.
        if (host.conn->merc_list_serial() != serial_) {
            serial_ = host.conn->merc_list_serial();
            type_ = 0;
            pick_row(host, 0);
        }
    }

    const ComboData* combo(UiHost& host, std::string_view id) override {
        if (!host.conn || id != "TypeComboBox") return nullptr;
        types_ = {};
        for (std::uint32_t t : host.conn->merc().merc_types) types_.choices.push_back(db(host, t, 21));
        types_.selected = types_.choices.empty() ? -1 : type_;
        return &types_;
    }
    bool combo_select(UiHost& host, std::string_view id, int index) override {
        if (!host.conn || id != "TypeComboBox") return false;
        type_ = index;
        pick_row(host, 0);
        return true;
    }

    const ListData* list(UiHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& m = host.conn->merc();
        if (id == "SubtypeListBox") {
            subtypes_ = {};
            for (std::size_t k : entries(host)) {
                const auto& r = m.merchant_list[k];
                ListRow row;
                std::string text = db(host, r.subtype, 22);
                if (r.required_membership == 1) text += " (SILVER)";
                else if (r.required_membership == 2) text += " (GOLD)";
                if (r.required_membership > account_membership(host)) row.color = argb_to_rgb(0xffff0000);
                row.texts = {text};
                subtypes_.rows.push_back(std::move(row));
            }
            subtypes_.selected = row_;
            return &subtypes_;
        }
        if (id == "StanceListBox") {
            stances_ = {};
            for (const auto& s : m.selected.stances) stances_.rows.push_back({{db(host, static_cast<std::uint32_t>(s.stance), 24)}});
            stances_.selected = stance_row_;
            return &stances_;
        }
        return nullptr;
    }
    bool list_click(UiHost& host, std::string_view id, int row, bool, int) override {
        if (!host.conn) return false;
        if (id == "SubtypeListBox") {
            pick_row(host, row);
            return true;
        }
        if (id == "StanceListBox") {
            stance_row_ = row;
            return true;
        }
        return false;
    }

    void child(UiHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& m = host.conn->merc();
        const game::MercRecord& r = m.selected;
        std::uint32_t stance = 0;
        if (stance_row_ >= 0 && static_cast<std::size_t>(stance_row_) < r.stances.size())
            stance = static_cast<std::uint32_t>(r.stances[static_cast<std::size_t>(stance_row_)].stance);
        cost_child(r, id, out);
        info_child(host, r, stance, id, out);
        if (id == "HireButton") out.enabled = row_ >= 0 && r.id != 0;
    }

    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        if (id == "HireButton") {
            // The hire request (code at 0x564dd0): the selected row's item data, the mercenary id.
            host.conn->merc_hire(host.conn->merc().selected.id);
            return true;
        }
        return false;
    }

private:
    // The merchant entries of the chosen type, in list order (one row each).
    std::vector<std::size_t> entries(UiHost& host) const {
        std::vector<std::size_t> out;
        const auto& m = host.conn->merc();
        if (type_ < 0 || static_cast<std::size_t>(type_) >= m.merc_types.size()) return out;
        for (std::size_t k = 0; k < m.merchant_list.size(); ++k)
            if (m.merchant_list[k].type == m.merc_types[static_cast<std::size_t>(type_)]) out.push_back(k);
        return out;
    }
    void pick_row(UiHost& host, int row) {
        auto e = entries(host);
        row_ = row >= 0 && static_cast<std::size_t>(row) < e.size() ? row : -1;
        stance_row_ = 0;
        if (row_ >= 0) host.conn->merc_select_entry(e[static_cast<std::size_t>(row_)]); // 0x566b00
    }

    std::uint32_t serial_ = 0;
    int type_ = 0, row_ = -1, stance_row_ = 0;
    ComboData types_;
    ListData subtypes_, stances_;
};

// ---------------------------------------------------------------- MMGW_ManageWnd

class MercManageWindow : public StockWindow {
public:
    MercManageWindow() : StockWindow("MMGW_ManageWnd", false) {}

    bool shown(UiHost& host) override { return host.conn && host.conn->merc_window_open(1); }
    void set_shown(UiHost& host, bool show) override {
        if (host.conn && show != host.conn->merc_window_open(1)) host.conn->set_merc_window_open(1, show);
    }

    void frame(UiHost& host) override {
        if (!host.conn) return;
        // CMercenaryManageWnd_UpdateButtons by the state: 0..2 disable every child, then the suspend and dismiss
        // buttons; 3 enables the suspend button; 5 enables every child; other states leave them.
        const auto& m = host.conn->merc();
        game::MercManageButtons b = game::merc_manage_buttons(m);
        if (m.state <= 2 || m.state == 5) {
            for (const char* c : kButtons) enabled_[c] = b.all_enabled;
            enabled_["SuspendButton"] = b.suspend_enabled;
            enabled_["DismissButton"] = b.dismiss_enabled;
            if (m.state == 5) enabled_["CallForAssistButton"] = b.call_for_assist_enabled;
        } else if (m.state == 3) {
            enabled_["SuspendButton"] = true;
        }
        if (!b.suspend_label.empty()) suspend_label_ = b.suspend_label;
        if (!b.suspend_tooltip.empty()) suspend_tooltip_ = b.suspend_tooltip;
        if (m.state <= 2) {
            // The suspend button's availability (@0x6e63b0): combat, a raid, the restoration timer, state 2.
            game::MercSuspendButton s = game::merc_suspend_button(m, host.conn->in_combat(), host.conn->raid().active);
            enabled_["SuspendButton"] = s.enabled;
            suspend_tooltip_ = s.tooltip;
        }
    }

    const ListData* list(UiHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& m = host.conn->merc();
        if (id == "StanceListBox") {
            stances_ = {};
            stance_data_.clear();
            for (const auto& s : m.active.stances) {
                ListRow row{{db(host, static_cast<std::uint32_t>(s.stance), 24)}};
                // The active stance's row is green (Send_OP_MercenaryCommand's colours).
                if (static_cast<std::uint32_t>(s.stance) == m.active.stance) row.color = argb_to_rgb(game::kMercRowActive);
                stances_.rows.push_back(std::move(row));
                stance_data_.push_back(static_cast<std::uint32_t>(s.stance));
            }
            stances_.selected = stance_row_;
            return &stances_;
        }
        if (id == "OwnedListBox") {
            owned_ = {};
            owned_.columns = 2;
            owned_.widths = {20, 200};
            auto rows = game::merc_owned_rows(m, [&](std::uint32_t subtype) { return db(host, subtype, 23); });
            owned_rows_ = rows;
            for (const auto& r : rows) {
                owned_.rows.push_back({{r.marker, r.text}});
                // The second loop selects the (last) row whose first column has text: the current mercenary.
                if (!r.marker.empty()) owned_.selected = static_cast<int>(owned_.rows.size() - 1);
            }
            if (owned_row_ >= 0) owned_.selected = owned_row_;
            return &owned_;
        }
        return nullptr;
    }
    bool list_click(UiHost& host, std::string_view id, int row, bool, int) override {
        if (!host.conn) return false;
        if (id == "StanceListBox") {
            // Choosing a row: Send_OP_MercenaryCommand.
            stance_row_ = row;
            host.conn->merc_choose_stance(stance_data_, row);
            return true;
        }
        if (id == "OwnedListBox") {
            owned_row_ = row;
            return true;
        }
        return false;
    }

    void child(UiHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& m = host.conn->merc();
        if (auto it = enabled_.find(std::string(id)); it != enabled_.end()) out.enabled = it->second;
        if (id == "TimeLeftText") {
            out.text = game::merc_time_left_text(m);
            out.tooltip = game::merc_upkeep_tooltip(m);
        } else if (id == "TotalSlotsText") {
            out.text = std::to_string(m.slot_count);
        } else if (id == "AssistModeCheckbox") {
            out.checked = m.active.assist_mode != 0;
        } else if (id == "SuspendButton") {
            if (!suspend_label_.empty()) out.text = suspend_label_;
            if (!suspend_tooltip_.empty()) out.tooltip = suspend_tooltip_;
        }
    }

    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (id == "SuspendButton") c.merc_suspend();
        else if (id == "DismissButton") c.merc_dismiss();
        else if (id == "CallForAssistButton") c.merc_call_for_assist();
        else if (id == "AssistModeCheckbox") c.merc_set_assist(c.merc().active.assist_mode == 0);
        else if (id == "SetActiveStanceButton") c.merc_choose_stance(stance_data_, stance_row_ < 0 ? 0 : stance_row_);
        else if (id == "SetOwnerButton") c.merc_set_owner();
        else if (id == "InfoButton") c.set_merc_window_open(2, !c.merc_window_open(2));
        else if (id == "SwitchButton") {
            if (owned_row_ >= 0 && static_cast<std::size_t>(owned_row_) < owned_rows_.size() &&
                owned_rows_[static_cast<std::size_t>(owned_row_)].data > 0)
                c.merc_switch(owned_rows_[static_cast<std::size_t>(owned_row_)].data - 1);
        } else {
            return false; // the hotkey buttons and the marketplace's Buy are not built here
        }
        return true;
    }

private:
    static constexpr const char* kButtons[] = {"StanceListBox", "AssistModeCheckbox", "CallForAssistButton",
                                               "SetActiveStanceButton", "SetStanceHotkeyButton", "SetOwnerButton",
                                               "DismissButton", "InfoButton", "SuspendButton", "OwnedListBox",
                                               "SwitchButton", "BuyButton", "SetMercenaryHotkeyButton"};
    std::map<std::string, bool> enabled_;
    std::string suspend_label_, suspend_tooltip_;
    int stance_row_ = -1, owned_row_ = -1;
    std::vector<std::uint32_t> stance_data_;
    std::vector<game::MercOwnedRow> owned_rows_;
    ListData stances_, owned_;
};

// ---------------------------------------------------------------- MIW_InfoWnd

class MercInfoWindow : public StockWindow {
public:
    MercInfoWindow() : StockWindow("MIW_InfoWnd", false) {}
    bool shown(UiHost& host) override { return host.conn && host.conn->merc_window_open(2); }
    void set_shown(UiHost& host, bool show) override {
        if (host.conn && show != host.conn->merc_window_open(2)) host.conn->set_merc_window_open(2, show);
    }
    void child(UiHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& r = host.conn->merc().active;
        cost_child(r, id, out);
        info_child(host, r, r.stance, id, out);
    }
};

} // namespace

void add_merc_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<MercMerchantWindow>());
    out.push_back(std::make_unique<MercManageWindow>());
    out.push_back(std::make_unique<MercInfoWindow>());
}

} // namespace hailstone::app
