// The marketplace: MarketplaceWnd (CMarketplaceWnd @0x6ddb90) over the catalog, rows and categories the connection
// keeps (net/game_connection_marketplace.cpp) and the rules in game/marketplace.h. The children are the original's
// ScreenIDs (MKPW_*). Beside the original's lists (MKPW_ItemList) the packs read these as lists:
//   MKPW_CategoriesTree  the tree's rows: name, depth, "1" for a branch, "1" when open, the category id; only the rows
//                        under open branches; the highlighted one selected. A click on column 1 (the arrow) opens or
//                        closes a branch, elsewhere it is the tree's click.
//   MKPW_Thumbnails      the grid's tiles (MKPW_NewItems / MKPW_FeaturedItems the landing page's): name, price, was,
//                        the timer's caption, its countdown, "1" for the New mark, the maker, the entry id; the icon.
// The tab boxes: MKPW_Subwindows (0 MainPage, 1 TaxInfoPage, 2 ConfirmPage, 3 ResultsPage: this client's numbering
// of the pages it offers) and MKPW_AdditionalDetails (0 SubwindowsPage, 1 PreviewPage). The funding and credit card
// pages (Add Funds) went to the publisher's web service and are not offered: MKPW_AddFundsBtn stays hidden.

#include <ctime>
#include <format>
#include <string>

#include "app/lua_ui_windows.h"
#include "game/marketplace.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

using game::MarketplacePage;
using game::MarketplaceView;

// The Sort combo's choices: the original's come from its XML (no code fills the box); this client's wording, in the
// order the comparator takes them (0 default, 1 / 2 name up / down, 3 / 4 price up / down).
constexpr const char* kSortChoices[] = {"Default", "Name (A to Z)", "Name (Z to A)", "Price (low to high)",
                                        "Price (high to low)"};

class MarketplaceWindow : public StockWindow {
public:
    MarketplaceWindow() : StockWindow("MarketplaceWnd", false) {}

    bool shown(UiHost& host) override { return host.conn && host.conn->marketplace().open; }
    void set_shown(UiHost& host, bool show) override {
        if (host.conn) host.conn->set_marketplace_open(show);
    }

    std::optional<int> tab_page(UiHost& host, std::string_view id) override {
        if (!host.conn) return std::nullopt;
        const auto& w = host.conn->marketplace().window;
        if (id == "MKPW_Subwindows") return static_cast<int>(w.page);
        if (id == "MKPW_AdditionalDetails") return w.preview_tab ? 1 : 0;
        return std::nullopt;
    }
    void set_tab_page(UiHost& host, std::string_view id, int page) override {
        if (host.conn && id == "MKPW_AdditionalDetails") host.conn->marketplace().window.preview_tab = page == 1;
    }

    void child(UiHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        auto& c = *host.conn;
        const auto& m = c.marketplace();
        const auto& w = m.window;
        const net::MarketplaceEntry* e = w.selected_entry(m.catalog);
        const game::MarketplaceRow* r = w.selected_row();
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        // During processing and a result the window's children are disabled (BeginProcessing, HandlePurchaseResult).
        if (m.busy) out.enabled = false;
        auto icon = [&](std::uint32_t item_icon) {
            if (!host.textures || item_icon == 0) return;
            if (auto cell = host.textures->item_icon(item_icon)) {
                out.decal.kind = ChildArt::Image;
                out.decal.cell = *cell;
            }
        };
        auto timer = [&](std::int64_t until, bool value) {
            auto left = game::marketplace_countdown(until, now);
            out.shown = left.has_value();
            if (left && value) out.text = *left;
        };
        if (id == "MKPW_AvailableFundsUpper") out.text = game::marketplace_funds_text(m.wallet);
        else if (id == "MKPW_AddFundsBtn" || id == "MKPW_CancelAddCardBtn") out.shown = false;
        else if (id == "MKPW_Landing") out.shown = w.view == MarketplaceView::Landing;
        else if (id == "MKPW_Thumbnails") out.shown = w.view == MarketplaceView::Icons;
        else if (id == "MKPW_ItemList") out.shown = w.view == MarketplaceView::List;
        else if (id == "MKPW_HeaderItems_Screen") out.shown = w.view != MarketplaceView::Landing;
        else if (id == "MKPW_IconBtn") out.checked = !w.list_button;
        else if (id == "MKPW_ListBtn") out.checked = w.list_button;
        else if (id == "MKPW_FindEditBox") out.text = m.find_text;
        else if (id == "MKPW_TaxInfoZipCodeEdit") out.text = m.zip_text;
        // The details pane (ShowItemDetails; cleared by 0x6d7b30 when nothing is selected).
        else if (id == "MKPW_DetailsName" || id == "MKPW_ConfirmPurchase") out.text = e ? e->name : std::string();
        else if (id == "MKPW_DetailsIcon" || id == "MKPW_ConfirmPurchase_Icon") {
            if (e) icon(e->icon);
            else out.decal.kind = ChildArt::None;
        } else if (id == "MKPW_DetailsDescription") {
            out.text = e ? game::marketplace_details_text(*e, category_name(w, e->category)) : std::string();
        } else if (id == "MKPW_DetailsPlayerStudioLabel" || id == "MKPW_PreviewPlayerStudioLabel") {
            const bool maker = r && !r->maker.empty();
            out.text = maker ? "Made By: " + r->maker : std::string();
            out.shown = maker && (id == "MKPW_DetailsPlayerStudioLabel" || m.preview.show);
        } else if (id == "MKPW_DetailsPlayerStudioIcon" || id == "MKPW_PreviewPlayerStudioIcon") {
            out.shown = r && !r->maker.empty() && (id == "MKPW_DetailsPlayerStudioIcon" || m.preview.show);
        } else if (id == "MKPW_DetailsTimeLeft_Label") timer(e ? e->available_until : -1, false);
        else if (id == "MKPW_DetailsAvailableTimeLeft_Label") timer(e ? e->available_until : -1, true);
        else if (id == "MKPW_DetailsOnSaleFor_Label") timer(e ? e->sale_until : -1, false);
        else if (id == "MKPW_DetailsSaleTimeLeft_Label") timer(e ? e->sale_until : -1, true);
        else if (id == "MKPW_WasPrice_Label" || id == "MKPW_WasPrice") {
            const bool was = e && e->was_price > e->price;
            out.shown = was;
            out.text = !was ? std::string() : id == "MKPW_WasPrice_Label" ? std::string("Was") : std::to_string(e->was_price);
        } else if (id == "MKPW_BuyPrice" || id == "MKPW_ConfirmPurchase_Price") {
            out.text = e ? game::marketplace_price_text(e->price, e->money_type) : std::string();
        } else if (id == "MKPW_BuyTotal" || id == "MKPW_ConfirmPurchase_Total") {
            out.text = e ? game::marketplace_total_text(*e, w.quantity) : std::string();
        } else if (id == "MKPW_BuyPrice_Icon" || id == "MKPW_BuyTotal_Icon" || id == "MKPW_ConfirmPurchase_PriceIcon" ||
                   id == "MKPW_ConfirmPurchase_TotalIcon" || id == "MKPW_ConfirmPurchase_Label") {
            out.shown = !e || (e->kind != 6 && e->money_type == 0);
        } else if (id == "MKPW_BuyPrice_MoneyTypeLabel" || id == "MKPW_BuyTotal_MoneyTypeLabel" ||
                   id == "MKPW_ConfirmPurchase_PriceMoneyType" || id == "MKPW_ConfirmPurchase_TotalMoneyType") {
            // The currency text comes from the publisher's commerce service only: empty here.
            out.text = std::string();
            out.shown = e && (e->kind == 6 || e->money_type == 1);
        } else if (id == "MKPW_QtyEdit" || id == "MKPW_ConfirmPurchase_Qty") {
            out.text = e ? std::to_string(w.quantity) : std::string();
            if (!e) out.enabled = false;
        } else if (id == "MKPW_QtyUpBtn") out.enabled = c.marketplace_quantity_up_enabled();
        else if (id == "MKPW_QtyDownBtn") out.enabled = c.marketplace_quantity_down_enabled();
        else if (id == "MKPW_BuyBtn") out.enabled = c.marketplace_buy_enabled();
        else if (id == "MKPW_ConfirmPurchase_ConfirmBtn") out.enabled = c.marketplace_confirm_enabled();
        else if (id == "MKPW_DetailsInspect") out.shown = r && e && (r->item_id != 0 || e->kind == 3);
        else if (id == "MKPW_DetailsPreview") out.shown = r && r->item_id != 0;
        else if (id == "MKPW_ConfirmPurchase_ItemRestrictionLabel")
            out.text = c.strings().format(7614, std::vector<std::string>{});
        else if (id == "MKPW_CancelBtn")
            out.shown = w.page == MarketplacePage::TaxInfo || w.page == MarketplacePage::Confirm;
        else if (id == "MKPW_Results_Result") out.text = m.result_text;
        // The preview page (SetPreviewState).
        else if (id == "MKPW_NoItemPreviewAvailable_Label") out.shown = !m.preview.show;
        else if (id == "MKPW_PreviewRenderArea") out.shown = m.preview.show;
        // The Item of the Week panel (RefreshCategories' block).
        else if (id.starts_with("MKPW_ItemOfWeek")) item_of_week(host, id, out, icon);
    }

    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn) return false;
        auto& c = *host.conn;
        if (button == "right") {
            // XWM_RCLICK on MKPW_DetailsIcon alone: Inspect (Alt: the object preview, not read here).
            if (id != "MKPW_DetailsIcon") return false;
            c.marketplace_inspect(false);
            return true;
        }
        if (button != "left") return false;
        if (id == "MKPW_BuyBtn") c.marketplace_buy();
        else if (id == "MKPW_QtyUpBtn") c.marketplace_step_quantity(1);
        else if (id == "MKPW_QtyDownBtn") c.marketplace_step_quantity(-1);
        else if (id == "MKPW_ConfirmPurchase_ConfirmBtn") c.marketplace_confirm();
        else if (id == "MKPW_CancelBtn" || id == "MKPW_Results_OKBtn") c.marketplace_cancel();
        else if (id == "MKPW_TaxInfoNextButton") c.marketplace_tax_next();
        else if (id == "MKPW_FindBtn") c.marketplace_find();
        else if (id == "MKPW_IconBtn") c.marketplace_set_view(MarketplaceView::Icons);
        else if (id == "MKPW_ListBtn") c.marketplace_set_view(MarketplaceView::List);
        else if (id == "MKPW_DetailsInspect") c.marketplace_inspect(false);
        else if (id == "MKPW_DetailsPreview") c.marketplace_preview_window();
        else if (id == "MKPW_ItemOfWeekBuyNowBtn") c.marketplace_item_of_week();
        else return false;
        return true;
    }

    bool set_text(UiHost& host, std::string_view id, const std::string& text) override {
        if (!host.conn) return false;
        auto& m = host.conn->marketplace();
        if (id == "MKPW_FindEditBox") m.find_text = text;
        else if (id == "MKPW_TaxInfoZipCodeEdit") m.zip_text = text;
        else if (id == "MKPW_QtyEdit") host.conn->marketplace_set_quantity_text(text); // every new value
        else return false;
        return true;
    }

    bool stml_link(UiHost& host, std::string_view, int, std::string_view data) override {
        // The window's link handler (wnd +0x224 → 0x6dd200): "marketid=n" opens that entry.
        auto entry = game::marketplace_link_entry(data);
        if (!host.conn || !entry) return false;
        host.conn->marketplace_open_to_entry(*entry);
        return true;
    }

    const ComboData* combo(UiHost& host, std::string_view id) override {
        if (!host.conn || id != "MKPW_Sort_Combobox") return nullptr;
        sort_.choices.assign(std::begin(kSortChoices), std::end(kSortChoices));
        sort_.selected = host.conn->marketplace().window.sort_choice;
        return &sort_;
    }
    bool combo_select(UiHost& host, std::string_view id, int index) override {
        if (!host.conn || id != "MKPW_Sort_Combobox") return false;
        host.conn->marketplace_sort(index);
        return true;
    }

    const ListData* list(UiHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& m = host.conn->marketplace();
        const auto& w = m.window;
        if (id == "MKPW_ItemList") {
            // 0x6d39ad: "*" for a new entry, the icon, the name, the currency (none here), the price.
            items_ = {};
            items_.columns = 5;
            items_.labels = {"", "", "Name", "", "Price"};
            items_.widths = {14, 24, 260, 20, 80};
            for (int i : w.visible) {
                const auto& e = entry(m, w, i);
                ListRow row{{e.is_new ? "*" : "", "", e.name, "", list_price(e)}};
                if (host.textures && e.icon) row.icon = host.textures->item_icon(e.icon);
                row.icon_column = 1;
                items_.rows.push_back(std::move(row));
            }
            items_.selected = position(w.visible, w.selected);
            return &items_;
        }
        if (id == "MKPW_Thumbnails") return tiles(host, thumbs_, w.visible);
        if (id == "MKPW_NewItems") return tiles(host, new_, w.new_shown);
        if (id == "MKPW_FeaturedItems") return tiles(host, featured_, w.featured_shown);
        if (id == "MKPW_CategoriesTree") {
            tree_ = {};
            tree_.columns = 5;
            tree_.labels = {"Category", "depth", "branch", "open", "id"};
            tree_.widths = {200, 0, 0, 0, 0};
            tree_ids_.clear();
            int closed_below = -1; // the depth of a closed branch whose rows are hidden
            for (const auto& t : w.tree) {
                if (closed_below >= 0 && t.depth > closed_below) continue;
                closed_below = -1;
                const bool open = w.expanded.count(t.id) > 0;
                if (t.branch && !open) closed_below = t.depth;
                tree_.rows.push_back(ListRow{{t.name, std::to_string(t.depth), t.branch ? "1" : "", open ? "1" : "",
                                              std::to_string(t.id)}});
                tree_ids_.push_back(t.id);
                if (w.highlight && *w.highlight == t.id) tree_.selected = static_cast<int>(tree_ids_.size()) - 1;
            }
            return &tree_;
        }
        return nullptr;
    }

    bool list_click(UiHost& host, std::string_view id, int row, bool right, int column) override {
        if (!host.conn || right) return false;
        auto& c = *host.conn;
        auto& m = c.marketplace();
        auto& w = m.window;
        if (id == "MKPW_CategoriesTree") {
            if (row < 0 || row >= static_cast<int>(tree_ids_.size())) return true;
            const std::uint32_t cat = tree_ids_[static_cast<std::size_t>(row)];
            if (column == 1) { // the arrow: open or close without selecting
                if (w.expanded.count(cat)) w.expanded.erase(cat);
                else w.expanded.insert(cat);
            } else {
                c.marketplace_click_category(cat);
            }
            return true;
        }
        const std::vector<int>* rows = id == "MKPW_ItemList" || id == "MKPW_Thumbnails" ? &w.visible
                                     : id == "MKPW_NewItems"                            ? &w.new_shown
                                     : id == "MKPW_FeaturedItems"                       ? &w.featured_shown
                                                                                        : nullptr;
        if (!rows) return false;
        if (row >= 0 && row < static_cast<int>(rows->size())) {
            // By entry id, skipping rows in state 2 (0x6e2091, 0x6dc9d0).
            c.marketplace_select_entry(entry(m, w, (*rows)[static_cast<std::size_t>(row)]).id);
        }
        return true;
    }

private:
    static const net::MarketplaceEntry& entry(const net::GameConnection::Marketplace& m, const game::MarketplaceWindow& w,
                                              int row) {
        return m.catalog[static_cast<std::size_t>(w.rows[static_cast<std::size_t>(row)].entry)];
    }
    static int position(const std::vector<int>& v, int row) {
        for (std::size_t i = 0; i < v.size(); ++i)
            if (v[i] == row) return static_cast<int>(i);
        return -1;
    }
    // The category whose full path is the entry's (unstripped): its display name.
    static std::string category_name(const game::MarketplaceWindow& w, const std::string& path) {
        for (const auto& n : w.categories)
            if (n.path == path) return n.name;
        return {};
    }
    // The list's and tiles' price: money 0 the integer, 1 "%1.2f"; nothing for the others (no "N/A" there).
    static std::string list_price(const net::MarketplaceEntry& e) {
        return e.money_type <= 1 ? game::marketplace_price_text(e.price, e.money_type) : std::string();
    }

    const ListData* tiles(UiHost& host, ListData& out, const std::vector<int>& rows) {
        const auto& m = host.conn->marketplace();
        const auto& w = m.window;
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        out = {};
        out.columns = 8;
        out.labels = {"name", "price", "was", "caption", "time", "new", "maker", "id"};
        for (int i : rows) {
            const auto& e = entry(m, w, i);
            const auto& r = w.rows[static_cast<std::size_t>(i)];
            // FillPanel: the earlier timer only; "Was %d" when the old price is higher; New unless a maker is shown.
            auto [a, b] = game::marketplace_tile_timers(e);
            std::string caption, left;
            if (auto t = game::marketplace_countdown(a, now)) {
                caption = "available";
                left = *t;
            } else if (auto u = game::marketplace_countdown(b, now)) {
                caption = "sale";
                left = *u;
            }
            ListRow row{{e.name, list_price(e), e.was_price > e.price ? std::format("Was {}", e.was_price) : std::string(),
                         caption, left, e.is_new && r.maker.empty() ? "1" : "", r.maker, std::to_string(e.id)}};
            if (host.textures && e.icon) row.icon = host.textures->item_icon(e.icon);
            out.rows.push_back(std::move(row));
        }
        out.selected = position(rows, w.selected);
        return &out;
    }

    template <class Icon> void item_of_week(UiHost& host, std::string_view id, Child& out, Icon&& icon) {
        const auto& w = host.conn->marketplace().window;
        const auto& e = w.item_of_week_shown;
        if (id == "MKPW_ItemOfWeekName") out.text = e ? e->name : host.conn->strings().format(4686, std::vector<std::string>{});
        else if (id == "MKPW_ItemOfWeekIcon") {
            out.shown = e.has_value();
            if (e) icon(e->icon);
        } else if (id == "MKPW_ItemOfWeekDescription") out.text = e ? e->description : std::string();
        else if (id == "MKPW_ItemOfWeekNew") out.shown = e && e->is_new;
        else if (id == "MKPW_ItemOfWeekPrice") out.text = e ? std::to_string(e->price) : std::string();
        else if (id == "MKPW_ItemOfWeekWas") {
            out.shown = e && e->was_price > e->price;
            if (e && e->was_price > e->price) out.text = std::format("Was {}", e->was_price);
        } else if (id == "MKPW_ItemOfWeekBuyNowBtn" || id == "MKPW_ItemOfWeekPrice_Icon") out.shown = e.has_value();
    }

    ComboData sort_;
    ListData items_, thumbs_, new_, featured_, tree_;
    std::vector<std::uint32_t> tree_ids_;
};

} // namespace

void add_marketplace_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<MarketplaceWindow>());
}

} // namespace hailstone::app
