// The bazaar and barter windows: BazaarWnd (CBazaarWnd, the trader window), BazaarSearchWnd (CBazaarSearchWnd),
// BazaarConfirmationWnd (CBazaarConfirmationWnd) and BarterMerchantWnd (CBarterMerchantWnd) over GameConnection's
// market state (game/bazaar.h, net/game_connection_market.cpp). The connection keeps the original's rules; these
// models lay its state out as the original windows do (addresses at each class).

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/lua_eq_windows.h"
#include "game/barter.h"
#include "game/bazaar.h"
#include "game/client_options.h"
#include "game/quantity_dialog.h"
#include "net/game_connection.h"
#include "ui/ui_ini.h"

namespace hailstone::app {

namespace {

int index_after(std::string_view id, std::string_view prefix) {
    if (id.size() <= prefix.size() || id.substr(0, prefix.size()) != prefix) return -1;
    int n = 0;
    for (char c : id.substr(prefix.size())) {
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return n;
}

std::string eqstr(EqHost& host, std::uint32_t id, std::vector<std::string> args = {}) {
    return host.conn ? host.conn->strings().format(id, args) : std::string();
}

int atoi_text(const std::string& s) {
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
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

// The dialog's place at a button's centre (CBazaarWnd and CBazaarSearchWnd open the quantity dialog there).
void at_click_centre(EqHost& host, game::QuantityOpen& q) {
    const auto& r = host.click_rect;
    if (r[0] < 0) return;
    q.x = static_cast<std::int32_t>((r[0] + r[2]) / 2);
    q.y = static_cast<std::int32_t>((r[1] + r[3]) / 2);
}

// ---------------------------------------------------------------- BazaarWnd (CBazaarWnd)

// CBazaarWnd (WndNotification @0x639bb0): the 200 trader slots (the Trader's Satchels' items, a click selects one:
// SelectBazaarSlotItem), the price as four money buttons (UpdatePriceButtons: the copper price / 1000, % 1000 / 100,
// % 100 / 10, % 10), each opening the quantity dialog at its centre with its amount (max 1,000,000 platinum, 100 for
// the others; sub_00638760), Set Price, Clear, Begin / End Trader (UpdateButtons shows one by trader mode), Add,
// Remove and List (trader mode), and the status text (AddBazaarText: each line in its chat colour, "</c><br>").
// The buttons call GameConnection's trader_* functions, which keep the original's rules and messages.
class BazaarWindow : public StockWindow {
public:
    BazaarWindow() : StockWindow("BazaarWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->trader_state().window_open; }
    void set_shown(EqHost& host, bool show) override {
        if (!host.conn) return;
        if (show && !host.conn->trader_state().window_open) host.conn->open_trader_window();
        else if (!show) host.conn->close_trader_window();
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& t = host.conn->trader_state();
        bool selected = t.selected >= 0 && t.selected < static_cast<int>(t.slots.size());
        if (id == "BZW_BazaarText") {
            std::string stml;
            for (const auto& l : t.log)
                stml += std::format("<c \"#{:06x}\">{}</c><br>", chat_index_rgb(static_cast<int>(l.color)), stml_escape(l.text));
            out.text = std::move(stml);
        } else if (int m = index_after(id, "BZW_Money"); m >= 0 && m < 4) {
            auto parts = game::split_price(t.entered_price);
            out.text = std::to_string(parts[static_cast<std::size_t>(m)]);
            out.enabled = selected; // sub_00638760 acts only with an item selected
        } else if (int n = index_after(id, "BZR_BazaarSlot"); n >= 0) {
            out.checked = n == t.selected;
        } else if (id == "BZW_End_Button") {
            out.shown = t.trader;
        } else if (id == "BZW_Start_Button") {
            out.shown = !t.trader;
        } else if (id == "BZW_SetPrice_Button") {
            // UpdateButtons @0x638500: Set Price with an item selected; Clear with a price in the money buttons;
            // in trader mode with a price Add (the selection not listed) or Remove (listed); List in trader mode.
            out.enabled = selected;
        } else if (id == "BZW_Clear_Button") {
            out.enabled = t.entered_price != 0;
        } else if (id == "BZW_Add_Button") {
            out.enabled = t.trader && selected && t.entered_price != 0 && !t.slots[static_cast<std::size_t>(t.selected)].active;
        } else if (id == "BZW_Remove_Button") {
            out.enabled = t.trader && selected && t.entered_price != 0 && t.slots[static_cast<std::size_t>(t.selected)].active;
        } else if (id == "BZW_List_Button") {
            out.enabled = t.trader;
        } else if (id == "BZW_OfflineMode_Button") {
            out.enabled = t.trader; // CBazaarWnd::UpdateButtons @0x63862a: in trader mode (+0x9f0)
        }
    }

    const net::Item* slot_item(EqHost& host, std::string_view id) override {
        int n = index_after(id, "BZR_BazaarSlot");
        if (!host.conn || n < 0) return nullptr;
        const auto& t = host.conn->trader_state();
        if (n >= static_cast<int>(t.slots.size())) return nullptr;
        return host.conn->inventory().at(t.slots[static_cast<std::size_t>(n)].where);
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn) return false;
        auto& c = *host.conn;
        const auto& t = c.trader_state();
        if (int n = index_after(id, "BZR_BazaarSlot"); n >= 0) {
            if (button == "left" && n < static_cast<int>(t.slots.size())) c.trader_select(n);
            else if ((button == "right" || button == "rightup") && n < static_cast<int>(t.slots.size()))
                item_right_click(host, t.slots[static_cast<std::size_t>(n)].where);
            return true;
        }
        if (button != "left") return false;
        if (int m = index_after(id, "BZW_Money"); m >= 0 && m < 4) {
            if (t.selected < 0) return true;
            auto parts = game::split_price(t.entered_price);
            game::QuantityOpen q;
            q.max = game::trader_coin_max(m);
            q.start = static_cast<std::int32_t>(parts[static_cast<std::size_t>(m)]);
            at_click_centre(host, q);
            open_quantity(host, q, [&host, m](std::int32_t v) {
                // 0x6386a0: that coin of the price becomes the dialog's amount.
                if (!host.conn) return;
                auto p = game::split_price(host.conn->trader_state().entered_price);
                p[static_cast<std::size_t>(m)] = static_cast<std::uint32_t>(std::max(0, v));
                host.conn->trader_enter_price(game::join_price(p[0], p[1], p[2], p[3]));
            });
        } else if (id == "BZW_SetPrice_Button") {
            c.trader_set_price();
        } else if (id == "BZW_Clear_Button") {
            c.trader_clear_price();
        } else if (id == "BZW_Start_Button") {
            c.trader_start();
        } else if (id == "BZW_End_Button") {
            c.trader_end();
        } else if (id == "BZW_Add_Button") {
            c.trader_add();
        } else if (id == "BZW_Remove_Button") {
            c.trader_remove();
        } else if (id == "BZW_List_Button") {
            c.trader_list();
        } else if (id == "BZW_OfflineMode_Button") {
            c.offlinemode_command(); // CBazaarWnd::WndNotification @0x63a31b: Cmd_offlinemode
        } else {
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------- BazaarSearchWnd (CBazaarSearchWnd)

// The search combos' tables (the client's .rdata, read by the ctor @0x634220 and doQuery @0x632e40): {the
// choice's eqstr (race: dbstr type 11), the value doQuery sends}; each combo's first choice is "any" (eqstr
// 6756..6760), which keeps BazaarQuery's default.
struct ComboEntry {
    std::uint32_t text, value;
};
constexpr ComboEntry kTypes[] = {{13855, 3}, {13900, 2}, {13856, 0}, {13857, 4}, {5520, 35}, {13858, 1}, {5519, 38},
                                 {3704, 57}, {5526, 46}, {5507, 10}, {5515, 27}, {5929, 54}, {5513, 18}, {5516, 31},
                                 {5504, 5},  {13872, 25}, {4801, 66}, {5512, 17}, {7698, 67}, {5510, 15}, {5531, 49},
                                 {5509, 14}, {5521, 29}, {5518, 33}, {5511, 16}, {5522, 45}, {5508, 11}, {5517, 32},
                                 {13898, 26}, {4626, 65}, {5514, 21}, {5506, 20}, {5505, 8}, {13916, 24}, {13919, 19},
                                 {13923, 23}};
constexpr ComboEntry kSlots[] = {{576, 22}, {564, 7}, {565, 8}, {5776, 0}, {572, 17}, {559, 1}, {561, 3},
                                 {574, 19}, {571, 15}, {568, 12}, {560, 2}, {573, 18}, {562, 5}, {9178, 21},
                                 {569, 13}, {567, 11}, {570, 14}, {563, 6}, {575, 20}, {566, 9}};
constexpr ComboEntry kRaces[] = {{2, 2}, {6, 6}, {8, 8}, {3, 3}, {330, 15}, {12, 12}, {7, 7}, {11, 11},
                                 {5, 5}, {1, 1}, {128, 13}, {10, 10}, {9, 9}, {130, 14}, {4, 4}, {522, 16}};
constexpr ComboEntry kClasses[] = {{1530, 8}, {1558, 15}, {5791, 16}, {1506, 2}, {1522, 6}, {1554, 14}, {1550, 13}, {1526, 7},
                                   {1542, 11}, {1510, 3}, {1518, 4}, {1534, 9}, {1514, 5}, {1538, 10}, {1502, 1}, {1546, 12}};
constexpr ComboEntry kStats[] = {{591, 14}, {580, 2}, {583, 6}, {578, 3}, {582, 4}, {579, 1}, {577, 0},
                                 {581, 5}, {588, 8}, {589, 11}, {587, 9}, {586, 7}, {590, 10}, {585, 13},
                                 {584, 12}, {6800, 15}, {5849, 16}, {5850, 17}, {5851, 18}, {5852, 19}, {5853, 20},
                                 {7567, 22}, {7568, 23}, {7569, 24}, {7570, 25}, {7574, 26}, {7577, 27}, {7572, 28},
                                 {7576, 29}, {7573, 30}, {7571, 31}, {7575, 32}, {13867, 33}, {13864, 34}, {13882, 35},
                                 {13884, 36}, {13888, 37}, {13893, 38}, {13904, 39}, {13920, 40}, {5837, 41}};

// CBazaarSearchWnd: the trader combo (UpdatePlayerCombo: "All Traders" 6755 with data 0, eqstr 783 with
// 0xffffffff, then the traders by name with their character ids), the item filters, the edits (AboutToShow and
// Default: max price 9999999, min price 0, levels 1..100, 200 results a trader), Query (doQuery), Welcome (OP_TraderShop
// 9), Default, Inspect (the selected row; a Shift or Ctrl click on a row too), and the result list (AddItemtoList:
// icon, name, count, the price in four coin columns, the trader, the row's own Buy button in column 8 (the ctor
// hides the BZR_BuyButton template; drawn here as a "Buy" cell), the stat's value). A row's Buy: the RoF expansion
// (7728), no item pending, not from ourselves, one item straight to the confirmation, more through the quantity
// dialog (max = start = the count). Find Trader (and a right click on the Trader column) paths to the row's trader
// in this zone; Hide Traders (UI ini HideTraders) hides every other trader.
class BazaarSearchWindow : public StockWindow {
public:
    BazaarSearchWindow() : StockWindow("BazaarSearchWnd", false) { defaults(); }

    bool shown(EqHost& host) override {
        bool open = host.conn && host.conn->bazaar_search_open();
        if (open && !was_open_) defaults(); // AboutToShow (sub_00636440)
        was_open_ = open;
        return open;
    }
    void ini_loaded(EqHost& host, ui::UiIni& ini, const std::filesystem::path&) override {
        // The ctor @0x634220: ReadUIINIBool("BazaarSearchWnd", "HideTraders") checks the button.
        ini_ = &ini;
        hide_ = game::ini_bool_value(ini.get("BazaarSearchWnd", "HideTraders"), false);
        if (host.conn) host.conn->bazaar_set_hide_traders(hide_, false);
    }
    void set_shown(EqHost& host, bool show) override {
        if (!host.conn || show == host.conn->bazaar_search_open()) return;
        if (show) host.conn->open_bazaar_search();
        else host.conn->close_bazaar_search();
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& s = host.conn->bazaar_search_state();
        bool row = selected_ >= 0 && selected_ < static_cast<int>(s.results.size());
        if (auto* e = edit(id)) out.text = *e;
        else if (id == "BZR_SearchResultLabel") out.text = std::to_string(s.results.size());
        else if (id == "BZR_RequestItemButton" || id == "BZR_RequestPreviewButton") out.enabled = row;
        else if (id == "BZR_FindTraderButton") out.enabled = host.conn->bazaar_find_trader_enabled(selected_);
        else if (id == "BZR_HideTradersButton") out.checked = hide_;
        else if (id == "BZR_BuyButton") {
            out.shown = false; // the template of the rows' buttons (the ctor hides it)
            out.enabled = row;
        }
        else if (id == "BZR_QueryButton") out.enabled = !s.searching;
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        auto* e = edit(id);
        if (!e) return false;
        *e = text.substr(0, id == "BZR_ItemNameInput" ? 30 : id == "BZR_MaxPriceInput" || id == "BZR_MinPriceInput" ? 7 : 3);
        return true;
    }

    const ComboData* combo(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        auto fill = [&](ComboData& d, std::uint32_t any, const ComboEntry* t, std::size_t n, bool race) {
            int sel = d.selected;
            d.choices.clear();
            d.choices.push_back(eqstr(host, any));
            for (std::size_t i = 0; i < n; ++i)
                d.choices.push_back(race ? host.conn->strings().db_string(t[i].text, 11) : eqstr(host, t[i].text));
            d.selected = std::clamp(sel, 0, static_cast<int>(d.choices.size()) - 1);
            return &d;
        };
        if (id == "BZR_ItemTypeCombobox") return fill(type_, 6756, kTypes, std::size(kTypes), false);
        if (id == "BZR_ClassSlotCombobox") return fill(class_, 6757, kClasses, std::size(kClasses), false);
        if (id == "BZR_RaceSlotCombobox") return fill(race_, 6758, kRaces, std::size(kRaces), true);
        if (id == "BZR_StatSlotCombobox") return fill(stat_, 6759, kStats, std::size(kStats), false);
        if (id == "BZR_ItemSlotCombobox") return fill(slot_, 6760, kSlots, std::size(kSlots), false);
        if (id == "BZR_ItemPrestigeCombobox") {
            int sel = prestige_.selected;
            prestige_.choices = {eqstr(host, 4687), eqstr(host, 4688), eqstr(host, 4689)};
            prestige_.selected = std::clamp(sel, 0, 2);
            return &prestige_;
        }
        if (id == "BZR_ItemAugmentCombobox") {
            // The augment types: "any" (4690), then dbstr type 16 names 1..31 longer than 3 characters, not 30.
            int sel = augment_.selected;
            augment_.choices = {eqstr(host, 4690)};
            augment_data_ = {0xffffffffu};
            for (std::uint32_t i = 1; i < 0x20; ++i) {
                std::string n = host.conn->strings().db_string(i, 16);
                if (n.size() > 3 && i != 0x1e) augment_.choices.push_back(n), augment_data_.push_back(i);
            }
            augment_.selected = std::clamp(sel, 0, static_cast<int>(augment_.choices.size()) - 1);
            return &augment_;
        }
        if (id == "BZR_PlayersCombobox") {
            int sel = players_.selected;
            players_.choices = {eqstr(host, 6755), eqstr(host, 783)};
            players_data_ = {0u, 0xffffffffu};
            std::vector<std::pair<std::string, std::uint32_t>> traders;
            for (const auto& [cid, t] : host.conn->bazaar_search_state().traders) traders.emplace_back(t.name, cid);
            std::sort(traders.begin(), traders.end());
            for (auto& [n, cid] : traders) players_.choices.push_back(n), players_data_.push_back(cid);
            players_.selected = std::clamp(sel, 0, static_cast<int>(players_.choices.size()) - 1);
            return &players_;
        }
        return nullptr;
    }
    bool combo_select(EqHost&, std::string_view id, int index) override {
        ComboData* d = id == "BZR_ItemTypeCombobox"    ? &type_
                       : id == "BZR_ClassSlotCombobox" ? &class_
                       : id == "BZR_RaceSlotCombobox"  ? &race_
                       : id == "BZR_StatSlotCombobox"  ? &stat_
                       : id == "BZR_ItemSlotCombobox"  ? &slot_
                       : id == "BZR_ItemPrestigeCombobox" ? &prestige_
                       : id == "BZR_ItemAugmentCombobox"  ? &augment_
                       : id == "BZR_PlayersCombobox"      ? &players_
                                                          : nullptr;
        if (!d) return false;
        d->selected = index;
        return true;
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || id != "BZR_ItemList") return nullptr;
        const auto& s = host.conn->bazaar_search_state();
        // The XML's columns; doQuery names the stat column after the stat picked.
        data_ = make_list({{"", 20}, {"Item Name", 125}, {"Qty", 40}, {"", 72}, {"", 24}, {"", 24}, {"", 24},
                           {"Trader", 80}, {"Lvl", 30}, {"Stat", 80}});
        if (stat_label_) data_.labels[9] = *stat_label_;
        for (const auto& r : s.results) {
            ListRow row;
            auto t = s.traders.find(r.trader_id);
            std::uint32_t p = r.cost;
            row.texts = {"", r.name, std::to_string(r.quantity), std::to_string(p / 1000), std::to_string(p % 1000 / 100),
                         std::to_string(p % 100 / 10), std::to_string(p % 10), t == s.traders.end() ? std::string() : t->second.name,
                         "Buy", r.stat ? std::to_string(r.stat) : std::string()};
            row.cell_colors.resize(10);
            row.cell_colors[8] = argb_to_rgb(0xff80c0ff); // the row's Buy button (BDT_Blue)
            if (host.textures) row.icon = host.textures->item_icon(r.icon);
            data_.rows.push_back(std::move(row));
        }
        if (selected_ >= static_cast<int>(data_.rows.size())) selected_ = -1;
        data_.selected = selected_;
        return &data_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int column) override {
        if (!host.conn || id != "BZR_ItemList") return false;
        auto& c = *host.conn;
        selected_ = row;
        const ImGuiIO& io = ImGui::GetIO();
        bool flags = io.KeyShift || io.KeyCtrl; // GetKeyboardFlags & 0xc
        if (row < 0 || row >= static_cast<int>(c.bazaar_search_state().results.size())) return true;
        if (right) {
            // Message 3 on the list: the Trader column finds the trader (the path at most every 15 s); else with
            // Shift or Ctrl the item preview (sub_00633660), without them the inspect (Send_OP_TraderShop_00633520).
            if (column == 7) c.bazaar_find_trader(static_cast<std::size_t>(row), true);
            else if (flags) c.preview_item_id(c.bazaar_search_state().results[static_cast<std::size_t>(row)].item_id);
            else c.bazaar_inspect(static_cast<std::size_t>(row), io.KeyShift);
            return true;
        }
        if (column == 8) {
            buy(host); // the row's Buy button
            return true;
        }
        // A click with Shift or Ctrl (keyboard flags 0xc) inspects.
        if (flags) c.bazaar_inspect(static_cast<std::size_t>(row), io.KeyShift);
        return true;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        const auto& s = c.bazaar_search_state();
        if (id == "BZR_QueryButton") {
            query(host);
        } else if (id == "BZR_WelcomeButton") {
            c.bazaar_welcome();
        } else if (id == "BZR_UpdatePlayerButton") {
            // UpdatePlayerCombo: the list is rebuilt from the traders the connection keeps (combo()).
        } else if (id == "BZR_Default") {
            defaults();
        } else if (id == "BZR_RequestItemButton") {
            if (selected_ >= 0) c.bazaar_inspect(static_cast<std::size_t>(selected_));
        } else if (id == "BZR_BuyButton") {
            buy(host);
        } else if (id == "BZR_RequestPreviewButton") {
            // CBazaarSearchWnd::sub_00633660: the selected row's item id previewed (Send_OP_ItemPreviewRequest @0x6ffc20).
            if (selected_ >= 0 && selected_ < static_cast<int>(s.results.size()))
                c.preview_item_id(s.results[static_cast<std::size_t>(selected_)].item_id);
        } else if (id == "BZR_FindTraderButton") {
            if (selected_ >= 0 && selected_ < static_cast<int>(s.results.size()))
                c.bazaar_find_trader(static_cast<std::size_t>(selected_));
        } else if (id == "BZR_HideTradersButton") {
            hide_ = !hide_;
            c.bazaar_set_hide_traders(hide_, true);
            if (ini_) {
                ini_->put("BazaarSearchWnd", "HideTraders", hide_ ? "1" : "0"); // WriteBoolToUIINI
                ini_->save();
            }
        } else {
            return false;
        }
        return true;
    }

    // The confirmation's purchase (BazaarConfirmationWnd reads it).
    struct Purchase {
        std::size_t result = 0;
        std::uint32_t quantity = 0;
        std::string name;
        std::uint32_t icon = 0, price = 0;
    };
    static std::optional<Purchase>& purchase() {
        static std::optional<Purchase> p;
        return p;
    }

private:
    std::string* edit(std::string_view id) {
        if (id == "BZR_ItemNameInput") return &name_;
        if (id == "BZR_MaxPriceInput") return &max_price_;
        if (id == "BZR_MinPriceInput") return &min_price_;
        if (id == "BZR_MaxLevelInput") return &max_level_;
        if (id == "BZR_MinLevelInput") return &min_level_;
        if (id == "BZR_MaxResultsPerTraderInput") return &max_results_;
        return nullptr;
    }
    void defaults() {
        name_.clear();
        max_price_ = "9999999";
        min_price_ = "0";
        max_level_ = "100";
        min_level_ = "1";
        max_results_ = "200";
        for (ComboData* d : {&type_, &class_, &race_, &stat_, &slot_, &prestige_, &augment_, &players_}) d->selected = 0;
        stat_label_.reset();
    }
    // doQuery: each combo past its first choice gives its table's value; the edits are read with atoi.
    void query(EqHost& host) {
        net::BazaarQuery q;
        auto pick = [](const ComboData& d, const ComboEntry* t, std::size_t n, std::uint32_t& field) {
            if (d.selected >= 1 && static_cast<std::size_t>(d.selected) <= n) field = t[d.selected - 1].value;
        };
        pick(race_, kRaces, std::size(kRaces), q.race);
        pick(class_, kClasses, std::size(kClasses), q.class_);
        pick(stat_, kStats, std::size(kStats), q.stat);
        pick(slot_, kSlots, std::size(kSlots), q.slot);
        pick(type_, kTypes, std::size(kTypes), q.type);
        if (stat_.selected >= 1 && stat_.selected < static_cast<int>(stat_.choices.size())) stat_label_ = stat_.choices[static_cast<std::size_t>(stat_.selected)];
        q.name = name_;
        q.max_cost = static_cast<std::uint32_t>(atoi_text(max_price_));
        q.min_cost = static_cast<std::uint32_t>(atoi_text(min_price_));
        q.max_level = static_cast<std::uint32_t>(atoi_text(max_level_));
        q.min_level = static_cast<std::uint32_t>(atoi_text(min_level_));
        q.max_results = static_cast<std::uint32_t>(atoi_text(max_results_));
        if (players_.selected >= 0 && players_.selected < static_cast<int>(players_data_.size()))
            q.trader = players_data_[static_cast<std::size_t>(players_.selected)];
        if (prestige_.selected == 1) q.prestige = 0xffffffffu;
        else if (prestige_.selected == 2) q.prestige = 0xfffffffeu;
        if (augment_.selected >= 1 && augment_.selected < static_cast<int>(augment_data_.size()))
            q.augment = augment_data_[static_cast<std::size_t>(augment_.selected)];
        selected_ = -1;
        host.conn->bazaar_search(q);
    }
    void buy(EqHost& host) {
        auto& c = *host.conn;
        const auto& s = c.bazaar_search_state();
        if (selected_ < 0 || selected_ >= static_cast<int>(s.results.size())) return;
        const net::BazaarResult& r = s.results[static_cast<std::size_t>(selected_)];
        if (!c.bazaar_row_buy_allowed()) return;
        // Not from ourselves (the result's seller id against our UniquePlayerID; dsp_chat colour 13).
        if (c.profile() && r.trader_id == c.profile()->character_id) {
            c.ui_message("You cannot purchase an item from yourself!", chat_index_rgb(13));
            return;
        }
        std::size_t index = static_cast<std::size_t>(selected_);
        auto confirm = [&host, index](std::int32_t qty) {
            if (!host.conn || qty <= 0) return;
            const auto& res = host.conn->bazaar_search_state().results;
            if (index >= res.size()) return;
            purchase() = Purchase{index, static_cast<std::uint32_t>(qty), res[index].name, res[index].icon, res[index].cost};
        };
        if (r.quantity < 2) {
            confirm(1);
            return;
        }
        game::QuantityOpen q;
        q.max = q.start = static_cast<std::int32_t>(r.quantity);
        const auto& rc = host.click_rect;
        if (rc[0] >= 0) q.x = static_cast<std::int32_t>(rc[0]), q.y = static_cast<std::int32_t>(rc[1]);
        open_quantity(host, q, confirm);
    }

    std::string name_, max_price_, min_price_, max_level_, min_level_, max_results_;
    ComboData type_, class_, race_, stat_, slot_, prestige_, augment_, players_;
    std::vector<std::uint32_t> players_data_, augment_data_;
    std::optional<std::string> stat_label_;
    ListData data_;
    int selected_ = -1;
    bool was_open_ = false;
    ui::UiIni* ini_ = nullptr;
    bool hide_ = false;
};

// ---------------------------------------------------------------- BazaarConfirmationWnd (CBazaarConfirmationWnd)

// SetPurchaseInfo @0x41d220: the item's icon, "<name> x <n>" for more than one, "Item Price: <compact coin of the
// total>", and the parcel cost (FormatConvertedPrice: 10% of the total rounded to its leading digit, at least 1c).
// The voucher cost "x <n>" (format "x %u") from the search window's u16 by the token icon (item icon 0xb1b, set in the
// constructor). To Me (BZC_UseTokenButton: method 2, paid in Expedient Delivery Vouchers, straight to the inventory;
// the reply's "and %hu vouchers") and To Parcels (BZC_UsePlatButton: method 1, the platinum fee, to the parcels)
// send the purchase (WndNotification @0x41d6a0: method = (button == UseToken) + 1), Cancel closes. Shown
// centred on the search window's place (CenterInRect).
class BazaarConfirmationWindow : public StockWindow {
public:
    BazaarConfirmationWindow() : StockWindow("BazaarConfirmationWnd", false) {}

    bool shown(EqHost& host) override {
        auto& p = BazaarSearchWindow::purchase();
        if (p && !host.conn) p.reset();
        if (p && !was_) move_ = centred(host);
        was_ = p.has_value();
        return was_;
    }
    void set_shown(EqHost&, bool show) override {
        if (!show) BazaarSearchWindow::purchase().reset();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        const auto& p = BazaarSearchWindow::purchase();
        if (!p) return;
        std::int64_t total = static_cast<std::int64_t>(p->price) * p->quantity;
        if (id == "BZC_ItemIcon" && host.textures) {
            if (auto cell = host.textures->item_icon(p->icon)) {
                out.decal.kind = ChildArt::Image;
                out.decal.cell = *cell;
            }
        } else if (id == "BZC_ItemLabel") {
            out.text = p->quantity > 1 ? std::format("{} x {}", p->name, p->quantity) : p->name;
        } else if (id == "BZC_TotalPrice") {
            out.text = "Item Price: " + game::coin_compact(total);
        } else if (id == "BZC_PlatLabel") {
            out.text = game::coin_compact(converted(total));
        } else if (id == "BZC_TokenLabel" && host.conn) {
            out.text = std::format("x {}", host.conn->bazaar_search_state().voucher_cost);
        } else if (id == "BZC_TokenIcon" && host.textures) {
            if (auto cell = host.textures->item_icon(kVoucherIcon)) {
                out.decal.kind = ChildArt::Image;
                out.decal.cell = *cell;
            }
        }
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        auto& p = BazaarSearchWindow::purchase();
        if (!p || !host.conn || button != "left") return false;
        if (id == "BZC_UseTokenButton") host.conn->bazaar_buy(p->result, p->quantity, false);
        else if (id == "BZC_UsePlatButton") host.conn->bazaar_buy(p->result, p->quantity, true);
        else if (id != "BZC_CancelButton") return false;
        p.reset();
        return true;
    }
    std::optional<std::array<int, 2>> take_move(EqHost&) override {
        auto m = move_;
        move_.reset();
        return m;
    }

private:
    static constexpr std::uint32_t kVoucherIcon = 0xb1b; // the constructor's IconCache::GetIcon(0xb1b)
    // FormatConvertedPrice @0x41d0a0 with the window's rate 0.1.
    static std::int64_t converted(std::int64_t total) {
        auto round_half = [](double v) { return static_cast<std::int64_t>(v >= 0 ? std::floor(v + 0.5) : std::ceil(v - 0.5)); };
        std::int64_t v = round_half(static_cast<double>(static_cast<float>(total) * 0.1f));
        if (v > 999) v = round_half(static_cast<double>(static_cast<float>(v) * 0.001f)) * 1000;
        else if (v > 99) v = round_half(static_cast<double>(v) / 100.0) * 100;
        else if (v > 9) v = round_half(static_cast<double>(v) / 10.0) * 10;
        return v > 0 ? v : 1;
    }
    std::array<int, 2> centred(EqHost& host) {
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        auto r = host.textures ? host.textures->screen_rect("BazaarConfirmationWnd") : std::nullopt;
        int w = r ? r->w : 400, h = r ? r->h : 250;
        return {(static_cast<int>(ds.x) - w) / 2, (static_cast<int>(ds.y) - h) / 2};
    }
    bool was_ = false;
    std::optional<std::array<int, 2>> move_;
};

// ---------------------------------------------------------------- the barter windows

namespace {

// A list's rows sorted by their name (the original's lists sort on column 1, CListWnd SortCol = 1), as the indexes
// of `names` in their row order.
std::vector<int> sorted_by_name(const std::vector<std::string>& names) {
    std::vector<int> order(names.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return names[static_cast<std::size_t>(a)] < names[static_cast<std::size_t>(b)];
    });
    return order;
}

int row_of(const std::vector<int>& order, int index) {
    for (std::size_t r = 0; r < order.size(); ++r)
        if (order[r] == index) return static_cast<int>(r);
    return -1;
}

int index_of(const std::vector<int>& order, int row) {
    return row >= 0 && row < static_cast<int>(order.size()) ? order[static_cast<std::size_t>(row)] : -1;
}

void icon_of(EqHost& host, ListRow& r, std::uint32_t icon) {
    if (host.textures) r.icon = host.textures->item_icon(icon);
}

// The inventory lists (CBarterWnd_FillInventoryList @0x631050, CBarterSearchWnd::UpdateInventoryList @0x62c960):
// icon, name, count.
ListData inventory_list(EqHost& host, const std::vector<game::BarterInventoryItem>& items, std::vector<int>& order) {
    ListData d = make_list({{"", 24}, {"Item Name", 120}, {"Qty", 30}});
    std::vector<std::string> names;
    for (const auto& e : items) names.push_back(e.name);
    order = sorted_by_name(names);
    for (int i : order) {
        const auto& e = items[static_cast<std::size_t>(i)];
        ListRow r;
        r.texts = {"", e.name, std::to_string(e.count)};
        icon_of(host, r, e.icon);
        d.rows.push_back(std::move(r));
    }
    return d;
}

// The compensation lists (CBarterWnd_UpdateLineEditor, CBarterSearchWnd_SelectBuyLine, CBarterMerchantWnd_
// FillDetailList): icon, name, quantity per item bought.
ListData compensation_list(EqHost& host, const std::vector<net::BuyLineTrade>& items, int width) {
    ListData d = make_list({{"", 24}, {"Item Name", width}, {"Qty", 30}});
    for (const auto& t : items) {
        ListRow r;
        r.texts = {"", t.name, std::to_string(t.quantity)};
        icon_of(host, r, t.icon);
        d.rows.push_back(std::move(r));
    }
    return d;
}

bool mod_alt() { return ImGui::GetIO().KeyAlt; }

} // namespace

// ---------------------------------------------------------------- BarterMerchantWnd (CBarterMerchantWnd)

// A buyer's window (from the server's answer to a right click on a buyer): MerchantNameLabel the buyer's name
// (CBarterMerchantWnd_Refresh @0x62a1d0), the buy lines (CBarterMerchantWnd_FillBuyLineList @0x629e10: icon, Item
// Name, Qty, Price; the rows the player cannot sell to in 0xffc08080), the selected line's compensation items
// (DetailList; a right click inspects one), Sell (enabled by sub_0062a3d0 every 300 ms; the quantity dialog for up
// to 20 starting at 1). The rules are GameConnection's barter_merchant_*.
class BarterMerchantWindow : public StockWindow {
public:
    BarterMerchantWindow() : StockWindow("BarterMerchantWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->barter_merchant().buyer_id != 0; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn) host.conn->close_buyer();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& m = host.conn->barter_merchant();
        if (id == "MerchantNameLabel") {
            auto it = host.conn->world().spawns.find(m.buyer_id);
            if (it != host.conn->world().spawns.end()) out.text = it->second.name; // +0xa4, the spawn's name
        } else if (id == "SellButton") {
            out.enabled = m.sell_enabled;
        }
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& m = host.conn->barter_merchant();
        if (id == "BuyLineList") {
            lines_ = make_list({{"", 24}, {"Item Name", 100}, {"Qty", 30}, {"Price", 80}});
            for (const auto& l : m.lines) {
                ListRow r;
                r.texts = {"", l.item_name, std::to_string(l.quantity), game::buy_line_price_text(l)};
                icon_of(host, r, l.icon);
                bool can = l.item_id != net::kKronoItemId && game::barter_has_items(host.conn->inventory(), l.item_id, 1);
                r.color = argb_to_rgb(can ? 0xffffffffu : 0xffc08080u);
                if (l.slot == m.selected_slot) lines_.selected = static_cast<int>(lines_.rows.size());
                lines_.rows.push_back(std::move(r));
            }
            return &lines_;
        }
        if (id == "DetailList") {
            details_ = make_list({{"", 24}, {"Item Name", 160}, {"Qty", 30}});
            for (const auto& l : m.lines)
                if (l.slot == m.selected_slot) details_ = compensation_list(host, l.trade_items, 160);
            details_.selected = detail_sel_;
            return &details_;
        }
        return nullptr;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int) override {
        if (!host.conn) return false;
        const auto& m = host.conn->barter_merchant();
        if (id == "BuyLineList") {
            if (row >= 0 && row < static_cast<int>(m.lines.size()))
                host.conn->barter_merchant_select(m.lines[static_cast<std::size_t>(row)].slot);
            detail_sel_ = -1;
            return true;
        }
        if (id == "DetailList") {
            detail_sel_ = row;
            if (right) host.conn->barter_merchant_inspect(row, ImGui::GetIO().KeyShift);
            return true;
        }
        return false;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (id != "SellButton") return false;
        if (button != "left" || !host.conn) return true;
        auto max = host.conn->barter_merchant_sell_start();
        if (!max) return true;
        game::QuantityOpen q;
        q.max = static_cast<std::int32_t>(*max);
        q.start = 1;
        at_click_centre(host, q);
        const auto* conn = host.conn;
        const auto serial = conn->barter_merchant().serial;
        const auto selected = conn->barter_merchant().selected_slot;
        open_quantity(host, q, [&host](std::int32_t v) {
            if (host.conn && v > 0) host.conn->barter_merchant_sell(static_cast<std::uint32_t>(v));
        }, [&host, conn, serial, selected] {
            return host.conn == conn && conn && conn->barter_merchant().buyer_id &&
                   conn->barter_merchant().serial == serial && conn->barter_merchant().selected_slot == selected;
        });
        return true;
    }

private:
    ListData lines_, details_;
    int detail_sel_ = -1;
};

// ---------------------------------------------------------------- BarterSearchWnd (CBarterSearchWnd)

// /barter's window (CBarterSearchWnd::WndNotification @0x62d810): the inventory (Krono and the items of slots 0..33
// and their bags; a click puts the name in the search field; Refresh), the search text and the buyers to ask
// (PlayerCombo: "All Barterers", "Local Barterers", then the buyers the server listed), Search (also Enter; 3 seconds
// apart), the matching lines (BuyLineList: icon, Item Name, Qty, Price, Buyer and a Sell button per line, drawn here
// as a "Sell" cell; a right click inspects the item, or finds the buyer on the last columns), the selected line's
// compensation items (DetailsList), Inspect and Preview (the item preview window), Welcome, Greeting, Find
// Buyer (15 seconds apart) and Hide Buyers (the UI ini's HideBuyers).
class BarterSearchWindow : public StockWindow {
public:
    BarterSearchWindow() : StockWindow("BarterSearchWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->barter_search_state().open; }
    void set_shown(EqHost& host, bool show) override {
        if (!host.conn) return;
        host.conn->set_barter_search_open(show);
    }
    void ini_loaded(EqHost& host, ui::UiIni& ini, const std::filesystem::path&) override {
        ini_ = &ini;
        hide_ = game::ini_bool_value(ini.get("BarterSearchWnd", "HideBuyers"), false);
        if (host.conn) host.conn->barter_search_set_hide(hide_, false);
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& s = host.conn->barter_search_state();
        auto now = static_cast<std::uint32_t>(host.conn->now_ms());
        bool line = s.selected >= 0;
        if (id == "ItemSearchEdit") out.text = s.text;
        else if (id == "SearchButton") out.enabled = now > s.last_search_ms + 3000;
        else if (id == "GreetingButton" || id == "BuyLineInspectButton") out.enabled = line;
        else if (id == "CompensationInspectButton") out.enabled = s.detail_sel >= 0;
        else if (id == "CompensationPreviewButton") out.enabled = s.detail_sel >= 0;
        else if (id == "FindButton") {
            const auto& l = s.lines;
            out.enabled = line && s.selected < static_cast<int>(l.size()) &&
                          l[static_cast<std::size_t>(s.selected)].zone == host.conn->zone_id() &&
                          now > s.last_find_ms + 15000;
        } else if (id == "HideButton") out.checked = hide_;
        else if (id == "SellButton") out.shown = false; // the per-line button's template
    }
    bool set_text(EqHost& host, std::string_view id, const std::string& text) override {
        if (id != "ItemSearchEdit" || !host.conn) return false;
        host.conn->barter_search_set_text(text.substr(0, 63));
        return true;
    }
    const ComboData* combo(EqHost& host, std::string_view id) override {
        if (id != "PlayerCombo" || !host.conn) return nullptr;
        const auto& s = host.conn->barter_search_state();
        combo_.choices.clear();
        for (const auto& [name, data] : s.combo_choices()) combo_.choices.push_back(name);
        combo_.selected = std::clamp(s.combo, 0, static_cast<int>(combo_.choices.size()) - 1);
        return &combo_;
    }
    bool combo_select(EqHost& host, std::string_view id, int index) override {
        if (id != "PlayerCombo" || !host.conn) return false;
        host.conn->barter_search_set_combo(index);
        return true;
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& s = host.conn->barter_search_state();
        if (id == "InventoryList") {
            inventory_ = inventory_list(host, s.inventory, inventory_order_);
            return &inventory_;
        }
        if (id == "BuyLineList") {
            // CBarterSearchWnd_FillBuyLineList @0x62b6a0: the lines still wanting some.
            lines_ = make_list({{"", 20}, {"Item Name", 120}, {"Qty", 40}, {"Price", 100}, {"Buyer", 120}, {"", 35}});
            std::vector<std::string> names;
            for (const auto& l : s.lines) names.push_back(l.item_name);
            line_order_.clear();
            for (int i : sorted_by_name(names))
                if (s.lines[static_cast<std::size_t>(i)].quantity > 0) line_order_.push_back(i);
            for (int i : line_order_) {
                const auto& l = s.lines[static_cast<std::size_t>(i)];
                ListRow r;
                r.texts = {"", l.item_name, std::to_string(l.quantity), game::buy_line_price_text(l), l.buyer_name, "Sell"};
                icon_of(host, r, l.icon);
                r.cell_colors.resize(6);
                r.cell_colors[5] = argb_to_rgb(0xff80c0ff); // the Sell button (BDT_Blue)
                lines_.rows.push_back(std::move(r));
            }
            lines_.selected = row_of(line_order_, s.selected);
            return &lines_;
        }
        if (id == "DetailsList") {
            details_ = make_list({{"", 24}, {"Item Name", 180}, {"Qty", 30}});
            if (s.selected >= 0 && s.selected < static_cast<int>(s.lines.size()))
                details_ = compensation_list(host, s.lines[static_cast<std::size_t>(s.selected)].trade_items, 180);
            details_.selected = s.detail_sel;
            return &details_;
        }
        return nullptr;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int column) override {
        if (!host.conn) return false;
        auto& c = *host.conn;
        const auto& s = c.barter_search_state();
        bool shift = ImGui::GetIO().KeyShift;
        if (id == "InventoryList") {
            int i = index_of(inventory_order_, row);
            if (!right && i >= 0) c.barter_search_set_text(s.inventory[static_cast<std::size_t>(i)].name);
            return true;
        }
        if (id == "BuyLineList") {
            int i = index_of(line_order_, row);
            if (!right && column == 5 && i >= 0) {
                // A line's Sell button.
                auto max = c.barter_search_sell_start(i);
                if (!max) return true;
                game::QuantityOpen q;
                q.max = static_cast<std::int32_t>(*max);
                q.start = 1;
                at_click_centre(host, q);
                open_quantity(host, q, [&host](std::int32_t v) {
                    if (host.conn && v > 0) host.conn->barter_search_sell(static_cast<std::uint32_t>(v));
                });
                return true;
            }
            if (right) {
                // A right click: the buyer's columns find the buyer (sub_0062beb0), the others inspect the item.
                c.barter_search_select(i);
                if (column > 3) c.barter_search_find();
                else c.barter_search_inspect(-1, shift);
                return true;
            }
            if (mod_alt()) c.barter_search_inspect(-1, shift);
            c.barter_search_select(i);
            return true;
        }
        if (id == "DetailsList") {
            c.barter_search_select_detail(row);
            if (right && !mod_alt()) c.barter_search_inspect(row, shift);
            else if (!right && mod_alt()) c.barter_search_inspect(row, shift);
            return true;
        }
        return false;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        const auto& s = c.barter_search_state();
        bool shift = ImGui::GetIO().KeyShift;
        if (id == "SearchButton") c.barter_search(s.text);
        else if (id == "RefreshInventoryButton") c.barter_search_refresh_inventory();
        else if (id == "WelcomeButton") c.barter_search_welcome();
        else if (id == "GreetingButton") c.barter_search_greeting();
        else if (id == "FindButton") c.barter_search_find();
        else if (id == "BuyLineInspectButton") c.barter_search_inspect(-1, shift);
        else if (id == "CompensationInspectButton") c.barter_search_inspect(s.detail_sel, shift);
        else if (id == "HideButton") {
            hide_ = !hide_;
            c.barter_search_set_hide(hide_, true); // +0x2c0 and the found buyer forgotten (-1)
            if (ini_) {
                ini_->put("BarterSearchWnd", "HideBuyers", hide_ ? "1" : "0"); // WriteBoolToUIINI
                ini_->save();
            }
        } else if (id == "CompensationPreviewButton") {
            // CBarterSearchWnd::sub_0062b3e0(compensation): the selected line's compensation item's id previewed.
            const int line = s.selected, k = s.detail_sel;
            if (line >= 0 && line < static_cast<int>(s.lines.size()) && k >= 0 &&
                k < static_cast<int>(s.lines[static_cast<std::size_t>(line)].trade_items.size()))
                c.preview_item_id(s.lines[static_cast<std::size_t>(line)].trade_items[static_cast<std::size_t>(k)].item_id);
        } else return false;
        return true;
    }

private:
    ComboData combo_;
    ListData inventory_, lines_, details_;
    std::vector<int> inventory_order_, line_order_;
    ui::UiIni* ini_ = nullptr;
    bool hide_ = false;
};

// ---------------------------------------------------------------- BarterWnd (CBarterWnd)

// /buyer's window (CBarterWnd::WndNotification @0x631a10): the item search (ItemSearchEdit and Search, 5 seconds
// apart; ItemSearchList: a click makes the item the edited line, Shift + Alt searches the buyers for it, Shift + Ctrl
// opens the bazaar search, a right click inspects it), the inventory (the general slots' tradeable items; Refresh)
// with Add / Remove into the compensation items (the quantity dialog), the price as four coin buttons (the quantity
// dialog, 1..1,000,000), Count (0..999), Create Buy / Modify; the buy lines (BuyLineList: On "X", icon, Item Name,
// Cnt, Offering; a click edits one) with Remove and Toggle; the status text with List and Clear; the welcome text
// and Update; Start / Stop Barter; Offline Mode (/offlinemode). Button states are
// CBarterWnd_UpdateStartStopButton's (game::barter_window_buttons).
class BarterWindow : public StockWindow {
public:
    BarterWindow() : StockWindow("BarterWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->barter_window().open; }
    void set_shown(EqHost& host, bool show) override {
        if (!host.conn) return;
        if (show) host.conn->open_barter_window();
        else host.conn->close_barter_window();
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& w = host.conn->barter_window();
        auto b = host.conn->barter_window_buttons();
        if (w.welcome != welcome_seen_) {
            welcome_seen_ = w.welcome; // InitOnShow puts the kept text in the edit
            welcome_ = w.welcome;
        }
        if (id == "ItemSearchEdit") out.text = search_;
        else if (id == "CountEdit") out.text = std::to_string(w.editor.quantity);
        else if (id == "WelcomeEdit") out.text = welcome_;
        else if (int m = coin_index(id); m >= 0) out.text = std::to_string(game::split_price(w.editor.cost)[static_cast<std::size_t>(m)]);
        else if (id == "ItemSearchButton") out.enabled = b.search;
        else if (id == "InventoryAddButton") out.enabled = b.inventory_add;
        else if (id == "InventoryRemoveButton") out.enabled = b.inventory_remove;
        else if (id == "BuyLineAddButton") out.shown = b.create_shown, out.enabled = b.create;
        else if (id == "BuyLineModifyButton") out.shown = b.modify_shown, out.enabled = b.modify;
        else if (id == "BuyLineRemoveButton") out.enabled = b.remove;
        else if (id == "BuyLineActiveButton") out.enabled = b.toggle;
        else if (id == "OfflineModeButton") out.enabled = b.offline; // in buyer mode (0xb822e0)
        else if (id == "UpdateWelcomeButton") out.enabled = w.welcome_changed;
        else if (id == "StartStopButton") {
            out.text = b.start_stop;
            out.color = argb_to_rgb(b.start_stop_color);
        } else if (id == "StatusText") {
            std::string stml;
            for (const auto& l : w.status)
                stml += std::format("<c \"#{:06x}\">{}</c><br>", chat_index_rgb(static_cast<int>(l.color)), stml_escape(l.text));
            out.text = std::move(stml);
        }
    }
    bool set_text(EqHost& host, std::string_view id, const std::string& text) override {
        if (!host.conn) return false;
        if (id == "ItemSearchEdit") search_ = text.substr(0, 64);
        else if (id == "CountEdit") host.conn->barter_set_count(atoi_text(text.substr(0, 3)));
        else if (id == "WelcomeEdit") {
            welcome_ = text.substr(0, 255);
            host.conn->barter_welcome_edited();
        } else return false;
        return true;
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& w = host.conn->barter_window();
        if (id == "ItemSearchList") {
            results_ = make_list({{"", 24}, {"Item Name", 250}});
            std::vector<std::string> names;
            for (const auto& e : w.item_results) names.push_back(e.name);
            result_order_ = sorted_by_name(names);
            for (int i : result_order_) {
                const auto& e = w.item_results[static_cast<std::size_t>(i)];
                ListRow r;
                r.texts = {"", e.name};
                icon_of(host, r, e.icon);
                results_.rows.push_back(std::move(r));
            }
            results_.selected = row_of(result_order_, w.item_sel);
            return &results_;
        }
        if (id == "InventoryList") {
            inventory_ = inventory_list(host, w.inventory, inventory_order_);
            inventory_.selected = row_of(inventory_order_, w.inventory_sel);
            return &inventory_;
        }
        if (id == "CompensationList") {
            compensation_ = compensation_list(host, w.editor.trade_items, 140);
            compensation_.widths[2] = 40;
            compensation_.selected = w.compensation_sel;
            return &compensation_;
        }
        if (id == "BuyLineList") {
            // CBarterWnd_FillBuyLineList @0x62f040: the enabled lines in slot order.
            lines_ = make_list({{"On", 20}, {"", 24}, {"Item Name", 150}, {"Cnt", 30}, {"Offering", 120}});
            slots_.clear();
            for (int i = 0; i < game::kBuyLineCount; ++i) {
                const auto& l = w.lines[static_cast<std::size_t>(i)];
                if (!l.enabled) continue;
                ListRow r;
                r.texts = {l.toggle ? "X" : "", "", l.item_name, std::to_string(l.quantity), game::buy_line_price_text(l)};
                icon_of(host, r, l.icon);
                r.icon_column = 1;
                if (i == w.line_sel) lines_.selected = static_cast<int>(lines_.rows.size());
                slots_.push_back(i);
                lines_.rows.push_back(std::move(r));
            }
            return &lines_;
        }
        return nullptr;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int) override {
        if (!host.conn) return false;
        auto& c = *host.conn;
        const auto& io = ImGui::GetIO();
        if (id == "ItemSearchList") {
            int i = index_of(result_order_, row);
            if (right) c.barter_item_inspect(i, io.KeyShift);
            else c.barter_item_click(i, io.KeyShift, io.KeyCtrl, io.KeyAlt);
            return true;
        }
        if (id == "InventoryList") {
            c.barter_select_inventory(index_of(inventory_order_, row));
            return true;
        }
        if (id == "CompensationList") {
            c.barter_select_compensation(row);
            return true;
        }
        if (id == "BuyLineList") {
            c.barter_select_line(row >= 0 && row < static_cast<int>(slots_.size()) ? slots_[static_cast<std::size_t>(row)] : -1);
            return true;
        }
        return false;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (int m = coin_index(id); m >= 0) {
            // CBarterWnd_SetPriceCoin(-1): the dialog for 1..1,000,000 at the button's centre.
            game::QuantityOpen q;
            q.max = 1000000;
            q.start = 1;
            at_click_centre(host, q);
            open_quantity(host, q, [&host, m](std::int32_t v) {
                if (host.conn && v >= 0) host.conn->barter_set_price_coin(m, static_cast<std::uint32_t>(v));
            });
        } else if (id == "ItemSearchButton") {
            if (c.barter_window_buttons().search) c.barter_item_search(search_);
        } else if (id == "InventoryAddButton") {
            if (auto r = c.barter_compensation_add_start()) {
                game::QuantityOpen q;
                q.max = r->first;
                q.start = r->second;
                at_click_centre(host, q);
                open_quantity(host, q, [&host](std::int32_t v) {
                    if (host.conn && v > 0) host.conn->barter_compensation_add(static_cast<std::uint32_t>(v));
                });
            }
        } else if (id == "InventoryRemoveButton") {
            if (auto r = c.barter_compensation_remove_start()) {
                game::QuantityOpen q;
                q.max = r->first;
                q.start = r->second;
                at_click_centre(host, q);
                open_quantity(host, q, [&host](std::int32_t v) {
                    if (host.conn && v > 0) host.conn->barter_compensation_remove(static_cast<std::uint32_t>(v));
                });
            }
        } else if (id == "BuyLineAddButton") c.barter_create_line();
        else if (id == "BuyLineModifyButton") c.barter_modify_line();
        else if (id == "BuyLineRemoveButton") c.barter_remove_line();
        else if (id == "BuyLineActiveButton") c.barter_toggle_line();
        else if (id == "ListLinesButton") c.barter_list_lines();
        else if (id == "ClearStatusButton") c.barter_clear_status();
        else if (id == "UpdateWelcomeButton") c.barter_update_welcome(welcome_);
        else if (id == "StartStopButton") c.barter_start_stop();
        else if (id == "InventoryRefreshButton") c.barter_refresh_inventory();
        else if (id == "OfflineModeButton") c.offlinemode_command(); // WndNotification @0x631fc3: Cmd_offlinemode
        else return false;
        return true;
    }

private:
    static int coin_index(std::string_view id) {
        if (id == "CompensationPlat") return 0;
        if (id == "CompensationGold") return 1;
        if (id == "CompensationSilver") return 2;
        if (id == "CompensationCopper") return 3;
        return -1;
    }

    std::string search_, welcome_, welcome_seen_;
    ListData results_, inventory_, compensation_, lines_;
    std::vector<int> result_order_, inventory_order_, slots_;
};

// ---------------------------------------------------------------- PointMerchantWnd (CPointMerchantWnd)

// A click on a good in either point merchant's item list (CPointMerchantWnd::WndNotification @0x71d350,
// CNewPointMerchantWnd::WndNotification @0x6fea70): message 3 (a right click) on columns 0..2 inspects the good with
// Send_OP_ItemLinkResponse(item id, its name, 0), or with Shift or Ctrl (GetKeyboardFlags & 0xc) shows it in the item
// preview (Send_OP_ItemPreviewRequest @0x6ffc20); a left click with Shift or Ctrl inspects as well as selecting.
// The lists hold only the goods' ids and names, so the item display waits for the server's OP_ItemLinkResponse.
void point_merchant_good_click(net::GameConnection& c, std::uint32_t item_id, const std::string& name, bool right, int column) {
    const ImGuiIO& io = ImGui::GetIO();
    bool flags = io.KeyShift || io.KeyCtrl;
    if (right) {
        if (column >= 3) return;
        if (flags) c.preview_item_id(item_id);
        else c.request_item_link(item_id, name, 0, false);
    } else if (flags) {
        c.request_item_link(item_id, name, 0, false);
    }
}

// A point (alternate currency) merchant (a right click on a class-70 NPC). CPointMerchantWnd is not decoded; this
// follows its XML over EQEmu's OP_AltCurrencyMerchantReply: the goods (PointMerchant_ItemList: name, cost), the
// merchant's currency and how much of it the player holds, Purchase (OP_AltCurrencyPurchase), Sell for the item
// clicked in the inventory while the window is open (its worth asked with OP_AltCurrencySellSelection, then
// OP_AltCurrencySell), Done, and a right click on a good inspects it. Equip is not built.
class PointMerchantWindow : public StockWindow {
public:
    PointMerchantWindow() : StockWindow("PointMerchantWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->point_merchant().npc != 0; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn) host.conn->close_point_merchant();
    }
    void frame(EqHost& host) override {
        // The item the player picks up while the window is open is the one Sell offers (as the merchant window
        // takes the cursor's item): ask what it is worth.
        if (!host.conn || !host.conn->point_merchant().npc) {
            asked_ = 0; // a reopened window asks again for the item still held
            return;
        }
        const net::Item* held = host.conn->inventory().cursor();
        std::uint32_t id = held ? held->id : 0;
        if (id != asked_) {
            asked_ = id;
            if (held) host.conn->point_merchant_quote(net::ItemSlot::possessions(net::kSlotCursor));
        }
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& m = host.conn->point_merchant();
        std::uint32_t have = 0;
        if (auto it = host.conn->alt_currencies().find(m.list.currency); it != host.conn->alt_currencies().end())
            have = it->second.amount;
        if (id == "PointMerchant_NameLabel") {
            auto it = host.conn->world().spawns.find(m.npc);
            out.text = it == host.conn->world().spawns.end() ? std::string() : game::display_name(it->second.name);
        } else if (id == "PointMerchant_PointsAvailableValue") {
            out.text = std::to_string(have);
        } else if (id == "PointMerchant_PurchaseButton") {
            const auto* item = selected(m);
            out.enabled = item && item->cost <= have;
        } else if (id == "PointMerchant_SellButton") {
            out.enabled = m.quote && m.quote->cost > 0;
            if (m.quote) out.tooltip = std::format("{} for {}", m.quote->item_name, m.quote->cost);
        } else if (id == "PointMerchant_EquipButton" || id == "PointMerchant_EquipLabel") {
            out.enabled = false;
        }
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& m = host.conn->point_merchant();
        if (id == "PointMerchant_ItemList") {
            items_ = make_list({{"Item", 200}, {"Cost", 60}});
            for (std::size_t k = 0; k < m.list.items.size(); ++k) {
                items_.rows.push_back(ListRow{{m.list.items[k].name, std::to_string(m.list.items[k].cost)}});
                if (static_cast<int>(k) == selected_) items_.selected = selected_;
            }
            return &items_;
        }
        if (id == "PointMerchant_PointList") {
            points_ = make_list({{"Currency", 100}, {"Amount", 50}});
            if (m.list.currency) {
                std::uint32_t have = 0;
                if (auto it = host.conn->alt_currencies().find(m.list.currency); it != host.conn->alt_currencies().end())
                    have = it->second.amount;
                points_.rows.push_back(ListRow{{std::format("Currency {}", m.list.currency), std::to_string(have)}});
            }
            return &points_;
        }
        return nullptr;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int column) override {
        if (id == "PointMerchant_ItemList") {
            selected_ = row;
            if (const auto* item = host.conn ? selected(host.conn->point_merchant()) : nullptr)
                point_merchant_good_click(*host.conn, item->item_id, item->name, right, column);
        }
        return id == "PointMerchant_ItemList" || id == "PointMerchant_PointList";
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (id == "PointMerchant_PurchaseButton") {
            if (const auto* item = selected(c.point_merchant())) c.point_merchant_purchase(item->item_id);
        } else if (id == "PointMerchant_SellButton") {
            c.point_merchant_sell();
            asked_ = 0;
        } else if (id == "PointMerchant_DoneButton") {
            c.close_point_merchant();
        } else {
            return id == "PointMerchant_EquipButton";
        }
        return true;
    }

private:
    const net::PointMerchantItem* selected(const net::GameConnection::PointMerchant& m) const {
        return selected_ >= 0 && selected_ < static_cast<int>(m.list.items.size()) ? &m.list.items[static_cast<std::size_t>(selected_)]
                                                                                   : nullptr;
    }

    ListData items_, points_;
    int selected_ = -1;
    std::uint32_t asked_ = 0;
};


// NewPointMerchantWnd for EQEmu's adventure merchants (classes 59, 61, 67, 68; the window's own code is not decoded):
// the goods with their cost and LDoN theme, the points available (LDoN points from OP_AdventurePointsUpdate, PvP
// points from OP_PVPStats, crystals from OP_CrystalCountUpdate), Purchase for the selected item and Sell for the held item, at the RoF2 client's
// sell-back price; a right click on a good inspects it.
class NewPointMerchantWindow : public StockWindow {
public:
    NewPointMerchantWindow() : StockWindow("NewPointMerchantWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->adventure_merchant().npc != 0; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn) host.conn->close_adventure_merchant();
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& m = host.conn->adventure_merchant();
        // What the merchant's currency has: LDoN points, PvP points (OP_PVPStats) or crystals
        // (OP_CrystalCountUpdate); -1 when the server has not said.
        std::int64_t have = -1;
        if (m.type == 1) have = host.conn->adventure().points.available;
        else if (m.type == 2 && host.conn->pvp().stats) have = host.conn->pvp().stats->points;
        else if (m.type == 4 && host.conn->crystals()) have = host.conn->crystals()->radiant;
        else if (m.type == 5 && host.conn->crystals()) have = host.conn->crystals()->ebon;
        if (id == "NewPointMerchant_NameLabel") {
            auto it = host.conn->world().spawns.find(m.npc);
            out.text = it == host.conn->world().spawns.end() ? std::string() : game::display_name(it->second.name);
        } else if (id == "NewPointMerchant_PointsNameLabel") {
            out.text = currency_name(m.type);
        } else if (id == "NewPointMerchant_PointsAvailableValue") {
            out.text = have >= 0 ? std::to_string(have) : std::string("-");
        } else if (id == "NewPointMerchant_PurchaseButton") {
            const auto* item = selected(m);
            out.enabled = item && (have < 0 || item->cost <= have);
        } else if (id == "NewPointMerchant_SellButton") {
            const net::Item* held = host.conn->inventory().cursor();
            std::uint32_t price = held && m.type != 2
                                      ? net::adventure_sell_price(held->ldon_price, held->ldon_sellback_rate, held->ldon_sold)
                                      : 0;
            out.enabled = price > 0;
            if (price) out.tooltip = std::format("{} for {}", held->name, price * (held->stackable() ? held->stack_count : 1));
        } else if (id == "NewPointMerchant_EquipButton" || id == "NewPointMerchant_EquipLabel") {
            out.enabled = false;
        }
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || id != "NewPointMerchant_ItemList") return nullptr;
        const auto& m = host.conn->adventure_merchant();
        static const char* const kThemes[] = {"", "Deepest Guk", "Miragul's", "Mistmoore", "Rujarkian", "Takish"};
        items_ = make_list({{"Item", 200}, {"Cost", 60}, {"Theme", 90}});
        for (std::size_t k = 0; k < m.items.size(); ++k) {
            const auto& it = m.items[k];
            items_.rows.push_back(ListRow{{it.name, std::to_string(it.cost), it.theme < 6 ? kThemes[it.theme] : ""}});
            if (static_cast<int>(k) == selected_) items_.selected = selected_;
        }
        return &items_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int column) override {
        if (id != "NewPointMerchant_ItemList") return false;
        selected_ = row;
        if (const auto* item = host.conn ? selected(host.conn->adventure_merchant()) : nullptr)
            point_merchant_good_click(*host.conn, item->item_id, item->name, right, column);
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (id == "NewPointMerchant_PurchaseButton") {
            if (const auto* item = selected(c.adventure_merchant())) c.adventure_merchant_purchase(item->item_id);
        } else if (id == "NewPointMerchant_SellButton") {
            c.adventure_merchant_sell(net::ItemSlot::possessions(net::kSlotCursor));
        } else if (id == "NewPointMerchant_DoneButton") {
            c.close_adventure_merchant();
        } else {
            return id == "NewPointMerchant_EquipButton";
        }
        return true;
    }

private:
    static std::string currency_name(std::uint32_t type) {
        switch (type) {
        case 1: return "LDoN Points";
        case 2: return "PvP Points";
        case 4: return "Radiant Crystals";
        case 5: return "Ebon Crystals";
        default: return {};
        }
    }
    const net::AdventureMerchantItem* selected(const net::GameConnection::AdventureMerchant& m) const {
        return selected_ >= 0 && selected_ < static_cast<int>(m.items.size()) ? &m.items[static_cast<std::size_t>(selected_)] : nullptr;
    }

    ListData items_;
    int selected_ = -1;
};

} // namespace

void add_market_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<BazaarWindow>());
    out.push_back(std::make_unique<BazaarSearchWindow>());
    out.push_back(std::make_unique<BazaarConfirmationWindow>());
    out.push_back(std::make_unique<BarterMerchantWindow>());
    out.push_back(std::make_unique<BarterSearchWindow>());
    out.push_back(std::make_unique<BarterWindow>());
    out.push_back(std::make_unique<PointMerchantWindow>());
    out.push_back(std::make_unique<NewPointMerchantWindow>());
}

} // namespace hailstone::app
