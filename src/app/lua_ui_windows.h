#pragma once

// The stock window models behind app/lua_ui.h (internal to the hs API). A model answers, for one XML screen, what
// the original window's code sets on each child (by ScreenID) and what a click on a child does. Anything a model
// does not set comes from the layout (the stock XML), as the original child would show it untouched.

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/lua_ui.h"
#include "game/buff_overlays.h"
#include "game/hot_button_state.h"
#include "ui/ui_textures.h"

namespace hailstone::net { struct Item; struct ItemSlot; struct ReMenu; class GameConnection; }
namespace hailstone::ui { class UiIni; }

namespace hailstone::app {

using Rgb = std::array<float, 3>;
Rgb argb_to_rgb(std::uint32_t argb);

// A Lua number as an int: a plain cast is undefined for NaN, the infinities and values past int's range, which a
// script can pass. NaN is 0; the rest is truncated as the cast would, then held to int's range.
constexpr int number_to_int(double v) {
    if (v != v) return 0;
    if (v <= -2147483648.0) return -2147483647 - 1;
    if (v >= 2147483647.0) return 2147483647;
    return static_cast<int>(v);
}

// A Lua integer narrowed without wrapping: a plain cast of a value past the type's range wraps it into another one
// that may be a valid target (65536 as an int16 slot is slot 0; 2^32 + 3 as an int is 3). integer_as gives nullopt
// when T does not hold v; held_int holds v to int's range, whose ends no index, slot or count takes.
template <class T, class V> constexpr std::optional<T> integer_as(V v) {
    if (!std::in_range<T>(v)) return std::nullopt;
    return static_cast<T>(v);
}
template <class V> constexpr int held_int(V v) {
    if (std::cmp_less(v, std::numeric_limits<int>::min())) return std::numeric_limits<int>::min();
    if (std::cmp_greater(v, std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
    return static_cast<int>(v);
}
// A 1-based Lua index as a 0-based int, held as held_int is (int's least value stays itself: nothing is below it).
template <class V> constexpr int held_index(V v) {
    const int i = held_int(v);
    return i == std::numeric_limits<int>::min() ? i : i - 1;
}

// What a child shows of an image part (a button's background / decal, a spell gem's icon).
struct ChildArt {
    enum Kind : std::uint8_t {
        Stock, // the layout's art (the original child keeps its XML animation)
        None,  // nothing (an empty buff slot)
        Image, // `cell`
        Anim,  // the stock animation `anim` (EQUI_Animations.xml), as CSidlManager::FindAnimation finds it
    } kind = Stock;
    ui::UiTextures::Cell cell;
    std::string anim;
};

// The state of one child, as the original window's code left it. Unset fields keep the layout's value.
struct Child {
    std::optional<bool> shown;
    std::optional<std::string> text;
    std::optional<bool> enabled;
    std::optional<bool> checked;
    // CXWnd bAction: a disabled button still takes the right button down (CButtonWnd vf18 sends the window 0x2d).
    bool action = false;
    std::optional<int> slider_value, slider_count;
    bool has_gauge = false;     // a gauge whose window sets its value itself (CGaugeWnd TargetVal)
    int gauge = -1;             // 0..1000; -1 hidden
    std::string gauge_text;
    std::optional<Rgb> color;       // CXWnd::CRNormal
    std::optional<Rgb> hover_color; // CButtonWnd::Mouseover
    std::optional<std::array<float, 4>> tint; // decal / static animation tint
    std::optional<std::string> tooltip;
    ChildArt background, decal, spellicon;
    std::optional<std::array<float, 4>> gem_tint; // the background tint; [3] the icon's alpha (CSpellGemWnd)
    std::optional<double> gem_overlay;             // the cool-down fraction (game/spell_gem.h), none when idle
    std::uint32_t gem_overlay_color = 0;           // its ARGB
    // CButtonWnd's cool-down (SetCoolDown, drawn by vtable +0x16c @0x8970a0): its rectangle's fraction (the part
    // gone, at most 0.87; game/spell_gem.h ButtonCoolDown), none when idle.
    std::optional<double> cool_down;
    // Optional timing detail from the window (combat ability buttons): milliseconds left and the ability's
    // focus-adjusted reuse. A shared timer can exceed this ability's reuse; 64 bits keep seconds * 1000 exact.
    std::uint64_t cool_down_left = 0, cool_down_duration = 0;
    bool under = false; // the window draws it in its own Draw, under its children (CTargetWnd::Draw)
};

struct BuffInfo {
    std::uint32_t spell = 0;
    std::int64_t ms_left = 0; // < 0: permanent
    std::string name;
    bool beneficial = true;
    // What CBuffWindow::PostDraw draws over the button (game/buff_overlays.h): the buff and song windows only,
    // each when its option is on.
    std::optional<game::BuffOverlayText> timer, counter, limit;
    // The buff and song windows' fade under 19 s (game::buff_fade): the button's alpha factor, 1 when not fading.
    double fade = 1.0;
};
// The overlays StockWindow::buff fills in (`overlays`, a mask of these): each costs a format or a look at every effect,
// so a window works out only those its caller asks for.
inline constexpr unsigned kBuffTimerOverlay = 1, kBuffCounterOverlay = 2, kBuffLimitOverlay = 4;
inline constexpr unsigned kBuffAllOverlays = kBuffTimerOverlay | kBuffCounterOverlay | kBuffLimitOverlay;

struct HotButtonInfo {
    std::string kind = "empty"; // "spell", "item", "button", "empty"
    std::string label;
    std::optional<ui::UiTextures::Cell> icon;
    // A social's: its text colour (CHotButtonWnd_UpdateButtonStates: GetSocialTextColor), and for a Lua social its
    // face (mq::ButtonFace; the label is the face's when it gives one) and whether its Lua runs.
    std::optional<Rgb> color;
    bool lua = false, running = false, enabled = true;
    float cooldown = 0.0f;
    std::string overlay, tooltip;
    // CHotButtonWnd_UpdateButtonStates (game/hot_button_state.h): the button's check and the cool-down rectangle's
    // fraction (CButtonWnd's, or a spell gem's own), with the time left; a spell gem's tint as the gem shows it
    // (the background colour, [3] the icon's alpha). Also checked by the pet window (CPetInfoWnd_SyncHotButtonStates:
    // labelled "Pet <a toggle command>" that is on).
    bool checked = false;
    std::optional<double> cool_down;
    std::uint32_t cool_down_left = 0;
    std::optional<std::array<float, 4>> gem_tint;
};

// What a hotbutton shows: its kind, text (`label`) and icon (a spell gem's spell, an inventory slot's item).
HotButtonInfo hot_button_info(UiHost& host, const game::HotButton& b, std::string label);
// Whether the pet window checks a hot button with this label (game/pet_buttons.h); nullopt: no command button has it.
std::optional<bool> pet_hot_button_checked(UiHost& host, std::string_view label);

// The hot button states' reads (game/hot_button_state.h) over the host (app/lua_ui_button_state.cpp).
class HostButtonQueries final : public game::HotButtonStateQueries {
public:
    explicit HostButtonQueries(UiHost& host) : h_(host) {}
    std::uint32_t now() override;
    std::uint32_t fast_now() override;
    bool primary_ready() override;
    bool auto_fire() override;
    bool auto_attack() override;
    bool sitting() override;
    std::uint32_t skill_last_used(std::uint32_t skill) override;
    std::uint32_t skill_timer_duration(std::uint32_t skill) override;
    bool skill_available(std::uint32_t skill) override;
    std::optional<std::uint32_t> list_skill(game::HotButtonType type, std::int32_t index) override;
    std::pair<std::uint32_t, std::uint32_t> social_cool_down(std::int32_t social) override;
    std::optional<AltAbility> alt_ability(std::int32_t group) override;
    std::optional<CombatAbility> combat_ability(std::int32_t index) override;
    bool leadership_active(std::int32_t ability, std::int32_t rank) override;
    std::optional<bool> pet_button(std::int32_t index) override;
    std::optional<ItemRecast> item_recast(const game::HotButton& b) override;

private:
    UiHost& h_;
};
// An item's ItemRecast when it has a recast clicky (ItemBase_HasRecastClicky), none otherwise or without an item.
std::optional<HostButtonQueries::ItemRecast> item_recast_of(const net::GameConnection& c, const net::Item* item);
// The Hailstone extras of the button states (option hotbutton_extras, hailstone.ini ui.hotbutton_extras; on by
// default).
bool hotbutton_extras(UiHost& host);
// The pet window's command button `index` (CPetInfoWnd::GetButton): its check while it is enabled (app/lua_ui_misc.cpp).
std::optional<bool> pet_button_checked(UiHost& host, int index);
// GetUserColor(0x15f) as the cool-downs draw it: ARGB with alpha 0x40 (game::spell_gem_overlay_color).
std::uint32_t cool_down_color();

struct ListRow {
    std::vector<std::string> texts;
    Rgb color{1, 1, 1};
    std::optional<ui::UiTextures::Cell> icon;
    std::vector<std::optional<Rgb>> cell_colors; // CListWnd::SetItemColor(row, column): a cell's own colour
    int icon_column = 0; // CListWnd::SetItemIcon's column
    bool disabled = false; // a line added or left disabled (CListWnd line flag 0x100 clear): the packs grey it
};
// A combo box's choices and the chosen one (-1 none).
struct ComboData {
    std::vector<std::string> choices;
    int selected = -1;
};

struct ListData {
    int columns = 1;
    int selected = -1;
    std::vector<int> multi; // the rows selected of a list that selects several (CListWnd style 0x20000)
    std::vector<std::string> labels;
    std::vector<int> widths;
    std::vector<ListRow> rows;
    // CListWnd::SetItemWnd: a combo box in this column of every row (-1 none), the window's child "<list>:<row>"
    // (combo(), combo_select() and child() for its enabled state).
    int combo_column = -1;
    // The window takes an Alt right click on a row (the guild bank's 3D preview): the packs' Alt right-click window
    // menu does not open over the rows.
    bool alt_right_click = false;
};

// CListWnd's own sorting, for the lists whose window sets ListWndStyle 0x40000 | 0x400000: a click on a column
// header sorts by that column ascending, or flips the direction of the column it is sorted by (CListWnd's header
// click, then the window's notification 0xf); CListWnd::Sort orders the rows with CListWnd::Compare @0x855a80, the
// window's answer to notification 0x10 for two rows or else their texts in the column compared byte by byte (CXStr's
// compare helpers @0x806a90 / 0x806660). The rows here are rebuilt from the window's state, so the order is kept as
// they are rebuilt rather than only on the click.
struct ListSort {
    int column = -1; // CListWnd SortCol (-1: the window's order)
    bool ascending = true;
    void header_click(int c) {
        if (column == c) ascending = !ascending;
        else column = c, ascending = true;
    }
    // The rows' order: `compare(a, b)` is the window's 0x10 answer for rows a and b (nullopt: none, the texts).
    template <class Compare>
    std::vector<std::size_t> order(const std::vector<ListRow>& rows, Compare compare) const {
        std::vector<std::size_t> out(rows.size());
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = i;
        if (column < 0) return out;
        auto c = static_cast<std::size_t>(column);
        auto cmp = [&](std::size_t a, std::size_t b) {
            std::optional<int> r = compare(a, b);
            if (!r) {
                const std::string& x = c < rows[a].texts.size() ? rows[a].texts[c] : kEmpty;
                const std::string& y = c < rows[b].texts.size() ? rows[b].texts[c] : kEmpty;
                int k = x.compare(y);
                r = k < 0 ? -1 : k > 0 ? 1 : 0;
            }
            return ascending ? *r < 0 : *r > 0;
        };
        std::stable_sort(out.begin(), out.end(), cmp);
        return out;
    }
    std::vector<std::size_t> order(const std::vector<ListRow>& rows) const {
        return order(rows, [](std::size_t, std::size_t) { return std::optional<int>(); });
    }
    // Reorders `d.rows` (and `keys` with them) by `order`.
    template <class Key>
    static void apply(ListData& d, std::vector<Key>& keys, const std::vector<std::size_t>& order) {
        std::vector<ListRow> rows;
        std::vector<Key> k;
        rows.reserve(order.size());
        for (std::size_t i : order) {
            rows.push_back(std::move(d.rows[i]));
            if (i < keys.size()) k.push_back(keys[i]);
        }
        d.rows = std::move(rows);
        if (keys.size() == order.size()) keys = std::move(k);
    }

private:
    inline static const std::string kEmpty;
};

// CTipWnd::Activate's `force`: StockWindow::activate(tip + kTipForce) on the tip windows.
inline constexpr int kTipForce = 0x100000;

class StockWindow {
public:
    StockWindow(std::string screen, bool shown_by_default) : screen_(std::move(screen)), open_(shown_by_default) {}
    virtual ~StockWindow() = default;
    const std::string& screen() const { return screen_; }

    // Shown: by default the open flag the game's show / hide calls set; some windows follow state (a merchant's
    // window is shown while the merchant session is open).
    virtual bool shown(UiHost&) { return open_; }
    virtual void set_shown(UiHost&, bool show) { open_ = show; }
    // A window of the screens before the world (server select, character creation): shown before the UI is ready.
    virtual bool pre_game() const { return false; }
    // The title the game sets (a merchant's name); "" keeps the XML title.
    virtual std::string title(UiHost&) { return {}; }
    virtual void child(UiHost&, std::string_view id, Child& out) { (void)id, (void)out; }
    // A click on a child: "left", "right", or a right-button phase ("rightdown", "rightheld", "rightup",
    // "rightupheld"). Returns false when the child has no handler.
    virtual bool click(UiHost&, std::string_view id, std::string_view button, bool outside) {
        (void)id, (void)button, (void)outside;
        return false;
    }
    // A <a WndNotify="message,data"> link clicked in the child's STML (the window's notification `message`).
    virtual bool stml_link(UiHost&, std::string_view id, int message, std::string_view data) {
        (void)id, (void)message, (void)data;
        return false;
    }
    virtual const net::Item* slot_item(UiHost&, std::string_view id) { (void)id; return nullptr; }
    virtual std::optional<BuffInfo> buff(UiHost&, std::string_view id, unsigned overlays = kBuffAllOverlays) {
        (void)id, (void)overlays;
        return std::nullopt;
    }
    virtual std::optional<HotButtonInfo> hot_button(UiHost&, std::string_view id) { (void)id; return std::nullopt; }
    // `button`: "left", "right" (a quick right click) or "rightheld" (the right button held, XWM_RCLICKHOLD).
    virtual bool click_hot_button(UiHost&, std::string_view id, std::string_view button) { (void)id, (void)button; return false; }
    // A drag from a child (row: a list's row, else -1): the hotbutton it makes for the cursor (the Actions window's
    // buttons, an AA, a bar's button), none when it makes none.
    virtual std::optional<game::HotButton> pick_up(UiHost&, std::string_view id, int row) {
        (void)id, (void)row;
        return std::nullopt;
    }
    virtual bool gem_click(UiHost&, std::string_view id, bool right) { (void)id, (void)right; return false; }
    virtual const ListData* list(UiHost&, std::string_view id) { (void)id; return nullptr; }
    virtual bool list_click(UiHost&, std::string_view id, int row, bool right, int column) {
        (void)id, (void)row, (void)right, (void)column;
        return false;
    }
    // A double click on a list's row (CListWnd's notification 4, after the click's own list_click).
    // A click on a list's column header (CListWnd's notification 0xf with the column): false when the window does
    // nothing with it.
    virtual bool list_header_click(UiHost&, std::string_view id, int column) { (void)id, (void)column; return false; }
    // A click on a list that selects several rows, with the keys held (1 shift, 2 ctrl; CListWnd::vf14: none
    // selects the row alone, ctrl toggles it, shift extends from the last one, both clear); false: not such a list.
    virtual bool list_select(UiHost&, std::string_view id, int row, int mods) { (void)id, (void)row, (void)mods; return false; }
    virtual bool list_double_click(UiHost&, std::string_view id, int row, int column) {
        (void)id, (void)row, (void)column;
        return false;
    }
    virtual bool set_text(UiHost&, std::string_view id, const std::string& text) { (void)id, (void)text; return false; }
    // A slider dragged to `value` (CSliderWnd::SetValue, then the window's notification 0xe).
    virtual bool set_slider(UiHost&, std::string_view id, int value) { (void)id, (void)value; return false; }
    virtual std::optional<int> tab_page(UiHost&, std::string_view id) { (void)id; return std::nullopt; }
    // Container windows ("Bag1".."Bag36"): the bag's name and slot count, an item in it, a click on a slot.
    virtual std::optional<std::pair<std::string, int>> bag_info(UiHost&) { return std::nullopt; }
    virtual const net::Item* bag_item(UiHost&, int index) { (void)index; return nullptr; }
    // A container window's bag: its slot (null for other windows).
    virtual const net::ItemSlot* bag_slot(UiHost&) { return nullptr; }
    virtual bool bag_click(UiHost&, int index, std::string_view button, bool outside) {
        (void)index, (void)button, (void)outside;
        return false;
    }
    virtual void set_tab_page(UiHost&, std::string_view id, int page) { (void)id, (void)page; }
    // Where the window's code put a child ({x, y, w, h} in the window's client pixels): windows that lay their
    // children out in code (the item display); nullopt keeps the XML place.
    virtual std::optional<std::array<int, 4>> child_rect(UiHost&, std::string_view id) { (void)id; return std::nullopt; }
    // For a child placed by child_rect whose right and bottom stay anchored to its parent's (RightAnchorToLeft and
    // BottomAnchorToTop false): {right offset, bottom offset}; child_rect's w and h are then not used.
    virtual std::optional<std::array<int, 2>> child_anchor(UiHost&, std::string_view id) { (void)id; return std::nullopt; }
    // The size the window's code gives itself ({w, h}: the whole window, frame and title bar included; CXWnd::Resize)
    // and a new size from the user's resizing; nullopt keeps the XML or UI ini size.
    virtual std::optional<std::array<int, 2>> window_size(UiHost&) { return std::nullopt; }
    virtual void size_window(UiHost&, int w, int h) { (void)w, (void)h; }
    // A combo box's choices (null: none) and a choice picked (CComboWnd's notification to the window).
    virtual const ComboData* combo(UiHost&, std::string_view id) { (void)id; return nullptr; }
    virtual bool combo_select(UiHost&, std::string_view id, int index) { (void)id, (void)index; return false; }
    // Once a frame before the scripts run (the window's OnProcessFrame work).
    virtual void frame(UiHost&) {}
    // A setting the window keeps in the UI ini that the Options window shows (UiApi::window_setting): nullopt for
    // one it does not have; set_setting false for one it does not have.
    virtual std::optional<int> setting(std::string_view name) const { (void)name; return std::nullopt; }
    virtual bool set_setting(UiHost&, std::string_view name, int value) { (void)name, (void)value; return false; }
    // The character's UI ini was loaded (LoadIniInfo): `ini` stays valid for the window's settings, `user_dir` is the
    // directory it lives in, the character's folder (core/character_files.h).
    virtual void ini_loaded(UiHost&, ui::UiIni& ini, const std::filesystem::path& user_dir) { (void)ini, (void)user_dir; }
    // Where the window's code moved itself ({x, y} on the screen, taken once): CQuantityWnd::Open at its caller's
    // button, a centred dialog.
    virtual std::optional<std::array<int, 2>> take_move(UiHost&) { return std::nullopt; }
    // A right click on the window that the window's context menu takes (CXWnd's ContextMenuID): opens it at x, y
    // through UiHost::open_menu; false shows this client's window menu instead.
    virtual bool context_menu(UiHost&, float x, float y) { (void)x, (void)y; return false; }
    // A window the game activates with an argument (CTipWnd::Activate(context tip), ...): UiApi::activate_tip.
    virtual bool activate(UiHost&, int arg) { (void)arg; return false; }

protected:
    std::string screen_;
    bool open_ = false;
};

// The windows, by group.
void add_hud_windows(std::vector<std::unique_ptr<StockWindow>>& out);  // app/lua_ui_hud.cpp
void add_item_windows(std::vector<std::unique_ptr<StockWindow>>& out); // app/lua_ui_items.cpp
void add_misc_windows(std::vector<std::unique_ptr<StockWindow>>& out); // app/lua_ui_misc.cpp
inline constexpr int kItemDisplayWindows = 6; // ItemDisplayWindow, ItemDisplayWindow#2 ..
void add_item_display_windows(std::vector<std::unique_ptr<StockWindow>>& out); // app/lua_ui_itemdisplay.cpp
void add_dialog_windows(std::vector<std::unique_ptr<StockWindow>>& out);       // app/lua_ui_dialogs.cpp
void add_social_windows(std::vector<std::unique_ptr<StockWindow>>& out);       // app/lua_ui_social.cpp
void add_real_estate_windows(std::vector<std::unique_ptr<StockWindow>>& out);  // app/lua_ui_realestate.cpp
// The real estate items window's context menu rows for a 0x21ec answer (app/lua_ui_realestate.cpp).
std::vector<UiHost::MenuRow> real_estate_menu_rows(UiHost& host, const net::ReMenu& menu);
void add_progress_windows(std::vector<std::unique_ptr<StockWindow>>& out);     // app/lua_ui_progress.cpp
void add_reward_selection_window(std::vector<std::unique_ptr<StockWindow>>& out); // app/lua_ui_rewards.cpp
void add_achievement_windows(std::vector<std::unique_ptr<StockWindow>>& out); // app/lua_ui_achievements.cpp
void add_map_windows(std::vector<std::unique_ptr<StockWindow>>& out);          // app/lua_ui_map.cpp
// hs.DrawMap, hs.MapZoom, ... over the map window among `windows` (app/lua_ui_map.cpp).
void register_map_api(lua_State* L, UiHost& host, std::vector<std::unique_ptr<StockWindow>>& windows);
void add_player_windows(std::vector<std::unique_ptr<StockWindow>>& out);       // app/lua_ui_player.cpp
void add_market_windows(std::vector<std::unique_ptr<StockWindow>>& out);       // app/lua_ui_market.cpp
void add_marketplace_windows(std::vector<std::unique_ptr<StockWindow>>& out);  // app/lua_ui_marketplace.cpp
void add_mail_windows(std::vector<std::unique_ptr<StockWindow>>& out);         // app/lua_ui_mail.cpp
void add_merc_windows(std::vector<std::unique_ptr<StockWindow>>& out);         // app/lua_ui_merc.cpp
void add_adventure_windows(std::vector<std::unique_ptr<StockWindow>>& out);    // app/lua_ui_adventure.cpp
void add_tint_windows(std::vector<std::unique_ptr<StockWindow>>& out);         // app/lua_ui_tint.cpp
void add_customization_windows(std::vector<std::unique_ptr<StockWindow>>& out); // app/lua_ui_customization.cpp
void add_ability_windows(std::vector<std::unique_ptr<StockWindow>>& out);      // app/lua_ui_abilities.cpp
// The combat ability list's rules (app/lua_ui_abilities.cpp): whether profile ability n is listed (no higher rank of
// its spell group held) and its hotbutton (type 10, 0x84 + n).
bool combat_ability_listed(const net::GameConnection& c, int n);
std::optional<game::HotButton> combat_ability_button(const net::GameConnection& c, int n);
void add_ability_book_window(std::vector<std::unique_ptr<StockWindow>>& out);  // app/lua_ui_abilitybook.cpp
void add_inventory_manager_window(std::vector<std::unique_ptr<StockWindow>>& out); // app/lua_ui_inventory_manager.cpp
void add_socialedit_windows(std::vector<std::unique_ptr<StockWindow>>& out);   // app/lua_ui_socialedit.cpp
void add_group_search_windows(std::vector<std::unique_ptr<StockWindow>>& out); // app/lua_ui_group_search.cpp
// CSocialEditWnd::GetSocialTextColor: social colour 0..19 as its colour button's text colour, else 0xff606060.
Rgb social_text_color(int color);
void add_name_windows(std::vector<std::unique_ptr<StockWindow>>& out);         // app/lua_ui_names.cpp
// PlayerNotesWindow (app/lua_ui_notes.cpp), and /note's dated line added to it (false: the notes are full or
// notes.txt cannot be written).
std::unique_ptr<StockWindow> make_player_notes_window();
bool player_notes_add(StockWindow& w, std::string_view note);
// The map command (app/lua_ui_map.cpp): the eqstr lines it says; empty while the window is
// closed.
std::vector<std::uint32_t> map_command(StockWindow& w, UiHost& host, std::string_view args);
void add_help_windows(std::vector<std::unique_ptr<StockWindow>>& out);         // app/lua_ui_help.cpp
void add_layout_windows(std::vector<std::unique_ptr<StockWindow>>& out);       // app/lua_ui_layout.cpp
void add_video_windows(std::vector<std::unique_ptr<StockWindow>>& out);        // app/lua_ui_video.cpp
void add_zone_guide_windows(std::vector<std::unique_ptr<StockWindow>>& out);   // app/lua_ui_zoneguide.cpp
void add_fellowship_windows(std::vector<std::unique_ptr<StockWindow>>& out);   // app/lua_ui_fellowship.cpp
void add_story_windows(std::vector<std::unique_ptr<StockWindow>>& out);        // app/lua_ui_story.cpp
void add_journal_windows(std::vector<std::unique_ptr<StockWindow>>& out);      // app/lua_ui_journal.cpp
void add_login_windows(std::vector<std::unique_ptr<StockWindow>>& out);        // app/lua_ui_login.cpp
// hs.LoginScreen, hs.ServerSelect, hs.CharacterSelect, hs.CharacterCreate and their actions (app/lua_ui_pregame.cpp).
// A window that reads a screen's view draws that screen in the client's place while it keeps doing so: `at` and
// `frame` are UiApi's replacement record (UiApi::replaces), under the names "LoginScreen", "serverselect",
// "CharacterListWnd" and "CharacterCreateScreen".
struct ScreenReplacements {
    std::map<std::string, int, std::less<>>* at = nullptr;
    const int* frame = nullptr;
};
void register_pregame_api(lua_State* L, UiHost& host, ScreenReplacements& screens);
// CHelpWnd::SetFile on the help window (help\\<file>).
void help_window_set_file(StockWindow& help, UiHost& host, const std::string& file);
// CStmlWnd::LoadPage of a game file (help\\..., help\\tips\\...): the page's body with its new lines as spaces, and
// its <TITLE> in `title` (empty when it has none).
std::string load_stml_page(const GameDir* game, const std::string& path, std::string* title);
// An href link's page in the game folder, as CStmlWnd follows it from `page` (the page shown): file:///x is x from the
// game folder, a bare name is beside the page; none for a web link (not followed).
std::optional<std::string> stml_href_path(const std::string& page, std::string_view href);

// CChatWindowManager::GetRGBAFromIndex as 0xRRGGBB (app/lua_ui_chat.cpp), and text made safe inside STML.
std::uint32_t chat_index_rgb(int index);
// The 99 user colours (CDisplay's table @0xd1f3d8, 0xRRGGBB): eqclient.ini [TextColors] User_<n>_Red/Green/Blue as
// the loader @0x48a160 reads them (the default table @0xac2c40 when one of the three is missing). Out of range
// indices read colour 0 (GetUserDefinedColor). Hailstone adds 54 (indices 99..152, the extended colours 355..408;
// game/chat_windows.h), with defaults of its own.
std::uint32_t user_color_rgb(int index);
std::uint32_t default_user_color_rgb(int index);
std::string_view extended_chat_color_name(int index); // index 99..152; empty outside the extended palette
// COptionsWnd::SaveColors: CDisplay::SetUserDefinedColor and the three keys the original writes to eqclient.ini,
// here to the character's chat.ini (core/character_files.h).
void set_user_color(UiHost& host, int index, std::uint32_t rgb);
std::string stml_escape(std::string_view text);

// A tell to `name` begun as a click on a name in chat begins one: its tell window up with tell windows on, else
// "/tell <name> " in the input line (app/lua_ui_chat.cpp).
void chat_open_tell(const std::string& name);
// Hailstone's right-click menu on another player (game::opens_player_menu; app/lua_ui_player.cpp): the player's
// name, then Send Tell, Invite to Group, Invite to Raid, Trade and Inspect. A row acts on the spawn while it is still
// in the zone, through the same commands as /tell, /invite, /raidinvite, a click with nothing on the cursor and
// /inspect, so each keeps their checks and messages.
std::vector<UiHost::MenuRow> player_menu_rows(UiHost& host, std::uint32_t spawn_id);
// Opens it at x, y. False when it did not open: not such a player, or context menus off (UiHost::context_menus).
bool open_player_menu(UiHost& host, std::uint32_t spawn_id, float x, float y);
// A right click on an item (CInvSlot::HandleRButtonUp): a bag opens or closes its container window, anything else
// shows its information (app/lua_ui_items.cpp).
void item_right_click(UiHost& host, const net::ItemSlot& slot);
// The left button held on an item slot (CInvSlotWnd::HandleLButtonHeld, game::inventory_hot_button): a hotbutton
// for the slot or its item on the cursor, when nothing (item, coin, spell, hotbutton) is on the cursor. True when one
// was put there (app/lua_ui_items.cpp).
bool item_left_held(UiHost& host, const net::ItemSlot& slot);
// Opens a bag's window when it is not open (Hailstone: a search result's bag).
void open_container(UiHost& host, const net::ItemSlot& slot);
// The OPEN_INV_BAGS, CLOSE_INV_BAGS and TOGGLE_INV_BAGS keys: the windows of the bags in the general slots
// (CInvSlotMgr_OpenAllBags / CloseAllBags; app/lua_ui_items.cpp).
void open_all_bags(UiHost& host);
void close_all_bags(UiHost& host);
void toggle_all_bags(UiHost& host);
// The CMD_TOGGLE_BANK_BAGS key: the windows of the bags in the bank and the shared bank (open only while the bank
// window shows).
void toggle_all_bank_bags(UiHost& host);

// CEverQuest::GetDeityDesc: a deity's name (app/lua_ui_itemdisplay.cpp).
std::string deity_desc(const net::GameConnection& c, std::uint32_t deity);

// Pushes hs.Item's table for `item` (nil without one); app/lua_ui.cpp.
int push_item(lua_State* L, const net::Item* item);

// A slot and an optional bag place from arguments `first` and `first + 1` (a missing bag place is -1, none): false
// when either is past int16's range, which a cast would wrap into another slot (app/lua_ui.cpp).
bool slot_args(lua_State* L, int first, std::int16_t& slot, std::int16_t& bag);

} // namespace hailstone::app
