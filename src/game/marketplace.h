#pragma once

// The marketplace window's rules (CMarketplaceWnd @0x6d1cc0..0x6e4100 and the
// free functions around it). The catalog and the window's rows live on the
// connection (GameConnection::marketplace(), net/game_connection_marketplace.cpp), the window model in
// app/lua_ui_marketplace.cpp. Names given here to the original's unnamed functions:
//  - CMarketplaceWnd_CanBuy @0x6d2df0: kinds 4 and 6 (the Station Cash kinds) always, else price <= wallet.
//  - CMarketplaceWnd_UpdateButtonStates @0x6d5850: Buy, Qty Up and Qty Down.
//  - CMarketplaceWnd_FormatPrice @0x6dbefb (inline in several places): by the entry's money type.
//  - CMarketplaceWnd_FindOrCreateCategory @0x6d89e0 and MarketplaceCategory_AdjustCount @0x6e4080: the category
//    tree from the entries' slash paths.
//  - CMarketplaceWnd_HandlePlayerStudioCategories @0x6d9490: the 0x77a7 state receiver.
//  - MarketplaceItemList_PickItemOfWeek @0x7f9030.
//  - CMarketplaceWnd_HandlePurchaseResult @0x6d43b0: the purchase answer's text.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "net/marketplace_packets.h"

namespace hailstone::game {

class EqStrings;

// SetAvailableFunds' "not known yet" (shown as 0; the manager asks for the catalog while it holds).
inline constexpr std::uint32_t kMarketplaceFundsUnknown = 0xffffffffu;
// UpdateButtonStates' own quantity cap, whatever the entry's.
inline constexpr std::uint32_t kMarketplaceMaxQuantity = 10;
// The 0x77a7 state a row starts in, and the one that keeps it out of its categories' counts.
inline constexpr std::uint32_t kMarketplaceUncounted = 2;
// The leading path the window strips (strncmp 13 characters with "Player Studio" @0x9efc94, then 14 skipped).
inline constexpr std::string_view kMarketplacePlayerStudio = "Player Studio";

// CanBuy: kinds 4 and 6 skip the wallet; any other kind needs price <= wallet (the wallet compared as it is held,
// so the unknown 0xffffffff passes).
bool marketplace_can_buy(const net::MarketplaceEntry& e, std::uint32_t wallet);
// UpdateButtonStates' Qty Up: (no max or qty < max) and qty < 10 and kind != 6 and (qty + 1) * price <= wallet.
bool marketplace_can_raise_quantity(const net::MarketplaceEntry& e, std::uint32_t quantity, std::uint32_t wallet);
// Qty Down: qty > 1.
inline bool marketplace_can_lower_quantity(std::uint32_t quantity) { return quantity > 1; }

// The price text: money type 0 the integer ("%d" of the price, as an unsigned _ultoa), 1 "%1.2f" of price * 0.01
// (real money), anything else "N/A".
std::string marketplace_price_text(std::uint32_t price, std::uint32_t money_type);

// SetAvailableFunds' label: "%d" of the wallet, 0 while unknown.
std::string marketplace_funds_text(std::uint32_t wallet);

// PickItemOfWeek: the first entry with the item-of-the-week flag whose rank no later one beats (strictly greater
// wins); -1 for none.
int marketplace_item_of_week(const std::vector<net::MarketplaceEntry>& catalog);

// SetQuantity's total: money type 0 "%d" of price * quantity (32 bits), 1 "%1.2f" of price * quantity * 0.01,
// else "N/A".
std::string marketplace_total_text(const net::MarketplaceEntry& e, std::uint32_t quantity);
// OnProcessFrame's countdown (once a second): the time left to an absolute time `until` (seconds), "D:HH:MM:SS"
// ("%01d:" with a day or more, then "%02d:%02d:%02d"); none once it has passed or for no timer (-1).
std::optional<std::string> marketplace_countdown(std::int64_t until, std::int64_t now);
// FillPanel's tile timer: of the two, the earlier one only (a <= b keeps a); {available, sale}, -1 for none.
std::pair<std::int64_t, std::int64_t> marketplace_tile_timers(const net::MarketplaceEntry& e);
// ShowItemDetails' description (STML): "Category: " + the display name of the category whose full path is the
// entry's (unstripped) + "<BR>" + the description + "<BR>". `category_name` is that name ("" for none).
std::string marketplace_details_text(const net::MarketplaceEntry& e, const std::string& category_name);
// The STML link handler (0x6dd200): the entry id of a link whose text holds "marketid=" (atoi up to '&'); none
// otherwise.
std::optional<std::uint32_t> marketplace_link_entry(std::string_view link);

// The text of a purchase's answer (HWM case 0x0a37 → HandlePurchaseResult), shown in an OK-only dialog and, without
// its <BR> tags, in MKPW_Results_Result. `entry` is the catalog's entry with the answer's id (null when there is
// none). Success (result code 0, the byte +0xc8 set):
//   [entry's purchase message + "\n\n"] + eqstr (7621 with +0xd8 set, else 5361)
//   + [" " + where it went (+0xe0: 1 the inventory, 2 the bank, 4 the shared bank; the exe's own sentences)]
//   + ["<BR><BR>\n\n" + the +0xf0 text] + ["<BR><BR>\n\n" + the +0x4f0 text]
// Failure (the code at +0xec): 1 and 4 eqstr 7622, 2 7623, 3 7624, 6 7632, 8 714; any other code eqstr 7626 (the
// original keeps a message it was handed instead, which this answer never carries).
std::string marketplace_purchase_text(const EqStrings& strings, const net::MarketplacePurchaseResult& r,
                                      const net::MarketplaceEntry* entry);
// The same text without its <BR> tags (the Find / Delete loop after the dialog), for MKPW_Results_Result.
std::string marketplace_strip_breaks(std::string text);


// ---------------------------------------------------------------- the window's model

// A category node (0x34 bytes, MarketplaceCategory ctor @0x610500). The window's array holds the three built-ins
// first: [0] "New and Featured" (id 0, 0x9efc80), [1] "Player Studio" (id 2, 0x9efc94), [2] "All Items" (id 1,
// 0x9efca4); then the catalog's paths with ids from 3 (wnd +0x334). Every catalog category lives under "All Items";
// a row's Player Studio mirror lives under "Player Studio".
struct MarketplaceCategory {
    std::uint32_t id = 0;     // +0x0c
    std::string name;         // +0x10: the leaf
    std::string path;         // +0x14: the full path FindOrCreateCategory matches (case-sensitive)
    bool visible = true;      // +0x18
    int parent = -1;          // +0x1c, as an index of the array
    std::vector<int> children; // +0x20, first-seen order
    int count = 0;            // +0x30: rows below it (or in it) in a state other than 2
};

// A window row (0x28 bytes), one per catalog entry, built by RefreshCategories in catalog order.
struct MarketplaceRow {
    int entry = -1;               // +0x00: the catalog index
    std::uint32_t state = kMarketplaceUncounted; // +0x08: 2 hidden and uncounted, 0 buyable, 1 shown but not buyable
    std::uint32_t item_id = 0;    // +0x0c: the preview's and Inspect's item (0x77a7)
    std::uint32_t u10 = 0;        // +0x10 (0x77a7)
    std::size_t order = 0;        // +0x14: the catalog index (the Default sort's last key)
    bool find_match = true;       // +0x18
    std::int32_t category = -1;   // +0x1c: the base category's id (-1 for an empty path)
    std::int32_t mirror = -1;     // +0x20: the "Player Studio/<base path>" category's id
    std::string maker;            // +0x24: the Player Studio creator ("Made By: ...")
};

// MKPW_Landing / MKPW_Thumbnails / MKPW_ItemList (0x6d2e20's modes).
enum class MarketplaceView { Landing = 0, Icons = 1, List = 2 };
// MKPW_Subwindows' pages this client offers (the credit-card and funding pages went to the publisher's service).
enum class MarketplacePage { Main = 0, TaxInfo = 1, Confirm = 2, Results = 3 };

// One row of MKPW_CategoriesTree as RebuildCategoryTree @0x6dcf40 lays it out (depth-first, array order at the top,
// child order below).
struct MarketplaceTreeRow {
    std::uint32_t id = 0;
    std::string name;
    int depth = 0;
    bool branch = false; // the node has children (shown or not); else a leaf row
};

// The window's rows, categories, view and selection (CMarketplaceWnd's fields), over the catalog the connection
// keeps. `in_game` is the gamestate test the original makes (false at character select, where the window also
// lives); this client only shows it in game.
//
// A deliberate difference: ResetView @0x6e1df0, SyncShelfToSelection @0x6d8030 and NavigateToEntry's fallback set
// the current category to array[1] ("Player Studio") while they highlight id 1 ("All Items") in the tree; the next
// refilter that a category click did not cause then shows only the Player Studio items. Here they use "All Items",
// which the tree shows and the screen already holds.
class MarketplaceWindow {
public:
    using Catalog = std::vector<net::MarketplaceEntry>;

    std::vector<MarketplaceCategory> categories;
    std::vector<MarketplaceRow> rows;   // wnd +0x270, in the current sort's order
    std::vector<MarketplaceTreeRow> tree;
    std::set<std::uint32_t> expanded;   // the tree's open branches (by id)
    int current = -1;                   // wnd +0x25c: the category shown (an index), -1 none (everything)
    int selected = -1;                  // wnd +0x24c: the selected row (an index), -1 none
    std::uint32_t quantity = 1;         // wnd +0x260
    int item_of_week = -1;              // wnd +0x264: a catalog index
    // What the Item of the Week panel shows: set by RefreshCategories when there is one, never cleared (a catalog
    // without one leaves the last on screen; "None currently listed", eqstr 4686, only before the first).
    std::optional<net::MarketplaceEntry> item_of_week_shown;
    bool stale = false;                 // wnd +0x318: a catalog came while the window was hidden
    std::uint32_t pending_entry = 0;    // wnd +0x340: the entry the next refilter selects (OpenToEntry)
    MarketplaceView view = MarketplaceView::Landing;
    bool list_button = false;           // MKPW_ListBtn checked (else MKPW_IconBtn)
    std::optional<std::uint32_t> highlight; // the tree's selected row (by id)
    int sort_choice = 0;                // MKPW_Sort_Combobox
    MarketplacePage page = MarketplacePage::Main;
    bool preview_tab = false;           // MKPW_AdditionalDetails on PreviewPage (else SubwindowsPage)
    std::vector<int> visible;           // the rows the grid and the list show, in order
    std::vector<int> new_items, featured_items; // the landing panels' entries (catalog indices, populate's order)
    std::vector<int> new_shown, featured_shown; // the rows (indices of `rows`) whose tiles show
    std::uint32_t last_find_ms = 0;     // wnd +0x330
    // The preview request ShowItemDetails makes (CMarketplaceWnd_SendItemPreview): the row's item id, 0 for none.
    std::optional<std::uint32_t> preview_request;

    const net::MarketplaceEntry* selected_entry(const Catalog& c) const;
    const MarketplaceRow* selected_row() const {
        return selected >= 0 && selected < static_cast<int>(rows.size()) ? &rows[static_cast<std::size_t>(selected)] : nullptr;
    }
    int category_index(std::int32_t id) const; // 0x6d3790: by id, -1 none

    // PreItemPopulate @0x6dc3d0: the selection, the item of the week, the rows, the panels and the categories.
    void pre_populate();
    // RefreshCategories @0x6dd260: the built-ins, a row per entry (state 2: nothing shows until the state feed), the
    // categories from the paths (a leading "Player Studio/" stripped), the item of the week, the landing panels, the
    // tree, then a refilter.
    void populate(const Catalog& c, bool in_game);
    // FindOrCreateCategory @0x6d89e0: the index, -1 for an empty path.
    int find_or_create(std::string_view path);
    // The 0x77a7 receiver @0x6d9490 for one record: `changed` when the row's state, item, u10 or maker changed;
    // `categories` when a category came into or went out of view.
    struct StateResult {
        bool changed = false, categories = false;
    };
    StateResult apply_state(const Catalog& c, const net::MarketplaceRowState& s);
    // RebuildCategoryTree: the tree from the shown categories (visible, count > 0); the current category none, "All
    // Items" open, "New and Featured" highlighted in game.
    void rebuild_tree(bool in_game);
    // Refilter @0x6dc550: the shown rows; the first one (or the pending entry's) selected, else the first shown
    // new tile, else the first featured; with none the details are cleared.
    void refilter(const Catalog& c);
    // 0x6d38a0's rule.
    bool row_shown(int row) const;
    // ShowItemDetails @0x6db620: refused for no row or state 2. The quantity 1, the main page, the preview asked.
    bool show_details(const Catalog& c, int row);
    // 0x6d7b30: the quantity 0 (the selection fields stay as refilter left them).
    void clear_details() { quantity = 0; }
    // SetQuantity @0x6d7660: clamped (unsigned) to 1 .. the entry's max, then 10; with no entry the value as given.
    void set_quantity(const Catalog& c, std::uint32_t q);
    // The Sort combo (0x6e1c30 with comparator 0x6e40c0), then a refilter. Ties keep their order here (the original's
    // introsort leaves them in no defined order).
    void sort(const Catalog& c, int choice);
    // Find @0x6dc820 (the button or Enter): ignored within 500 ms of the last; the grid or list shown; a non-empty
    // text matches names case-insensitively (the category none, the tree unselected), an empty one matches all and
    // highlights "All Items". False when debounced.
    bool find(const Catalog& c, std::string_view text, std::uint32_t now_ms);
    // A tree row's click (WndNotification 0x6e1fc4): the current one toggles open; another becomes current (the Find
    // filter cleared, a refilter; "New and Featured" shows the landing page, the others the grid or list).
    void click_category(const Catalog& c, std::uint32_t id);
    // IconBtn / ListBtn.
    void set_view(MarketplaceView v);
    // NavigateToEntry @0x6dca60: the entry's shelf (the landing page for a new or featured one in game, else its
    // category, its ancestors opened), a refilter, then its details.
    void navigate(const Catalog& c, std::uint32_t entry_id, bool in_game);
    // ResetView @0x6e1df0 (every show): the category none, the Find filter cleared (not its text), the Default sort
    // (which selects the first row), the landing page for a new or featured selection (or none), else "All Items" in
    // the grid; the main page.
    void reset_view(const Catalog& c, bool in_game);
    // SyncShelfToSelection @0x6d8030 (after a state feed).
    void sync_shelf(const Catalog& c, bool in_game);

private:
    int adjust(std::int32_t id, bool add); // AdjustCountUp over a category and its ancestors: 1 when it came into or left view
    bool in_subtree(int node, std::int32_t id) const;
    void tree_rows(int node, int depth);
    int find_row(const Catalog& c, std::uint32_t entry_id) const;
    MarketplaceView toggle_view() const { return list_button ? MarketplaceView::List : MarketplaceView::Icons; }
    std::uint32_t next_id_ = 3;
    // find_or_create's index of `categories` by path (only find_or_create adds categories with a path of their own,
    // so each path is there once). An entry is checked against its category before it is used; a miss scans.
    std::map<std::string, int, std::less<>> path_index_;
};

} // namespace hailstone::game
