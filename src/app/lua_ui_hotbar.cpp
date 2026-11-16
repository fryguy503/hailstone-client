// The hot bar window's own menu and settings, a hotbutton's Change Size / Assign Icon, and the icon selection window
// (app/lua_ui_hotbar.h; the original's CHotButtonWnd and CIconSelectionWnd).

#include "app/lua_ui_hotbar.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <format>

#include <imgui.h>
#include <lua.hpp>

#include "app/actions_ui.h"
#include "app/lua_ui_windows.h"
#include "game/hot_button_sets.h"
#include "game/icon_selection.h"
#include "game/keymap.h"
#include "game/quantity_dialog.h"
#include "net/game_connection.h"
#include "ui/ui_ini.h"

namespace hailstone::app {

namespace {

struct Bar {
    HotBarSettings settings;
    ui::UiIni* ini = nullptr;
    std::string section;
    StockWindow* window = nullptr;
};
std::array<Bar, game::kHotBars> g_bars;

bool valid_bar(int bar) { return bar >= 0 && bar < game::kHotBars; }

std::string text(UiHost& host, std::uint32_t id, std::vector<std::string> args = {}) {
    return host.conn ? host.conn->strings().format(id, args) : std::string();
}

std::string screen_of(int bar) { return bar == 0 ? "HotButtonWnd" : "HotButtonWnd" + std::to_string(bar + 1); }

// CHotButtonWnd::StoreIniInfo: PercentSize, ShowKeyMap and ShowSpinner ("%d") into the character's UI ini.
void store(int bar) {
    Bar& b = g_bars[static_cast<std::size_t>(bar)];
    if (!b.ini) return;
    b.ini->put(b.section, "PercentSize", std::to_string(b.settings.percent));
    b.ini->put(b.section, "ShowKeyMap", b.settings.key_map ? "1" : "0");
    b.ini->put(b.section, "ShowSpinner", b.settings.spinner ? "1" : "0");
}

UiHost::MenuRow row(std::string t, std::function<void()> act = {}) {
    UiHost::MenuRow r;
    r.text = std::move(t);
    r.act = std::move(act);
    return r;
}

UiHost::MenuRow separator() {
    UiHost::MenuRow r;
    r.separator = true;
    return r;
}

// CContextMenu::EnableMenuItem(row, false): disabled, in the disabled colour.
UiHost::MenuRow disabled(UiHost::MenuRow r) {
    r.enabled = false;
    r.rgb = UiHost::kMenuDisabledRgb;
    return r;
}

// A ConfirmationDialog(handler, id, question, 0xc: Yes / No, -1: no time limit) whose Yes runs `yes`.
void ask(UiHost& host, std::uint32_t question, std::function<void()> yes) {
    UiHost::Confirm c;
    c.text = text(host, question);
    c.done = [yes = std::move(yes)](bool ok) {
        if (ok && yes) yes();
    };
    host.confirm = std::move(c);
}

// ---------------------------------------------------------------- IconSelectionWnd (CIconSelectionWnd)

class IconSelectionWindow;
IconSelectionWindow* g_icon_selection = nullptr;

// The window's choices (EQUI_IconSelectionWnd.xml).
const std::vector<std::string> kTypeChoices = {"Item", "Spell", "Menu"};
const std::vector<std::string> kPerPageChoices = {"50", "100", "500", "1000", "All"};

class IconSelectionWindow : public StockWindow {
public:
    IconSelectionWindow() : StockWindow("IconSelectionWnd", false) { g_icon_selection = this; }
    ~IconSelectionWindow() override {
        if (g_icon_selection == this) g_icon_selection = nullptr;
    }

    // 0x68f330 on bar / button's {icon type, icon} at x, y (the button's centre).
    void open(UiHost& host, int bar, int button, float x, float y) {
        if (!host.hot_buttons) return;
        ranges(host);
        const game::HotButton& b = host.hot_buttons->current(bar, button);
        owner_ = {bar, button};
        if (!sel_.open(b.icon_type, b.icon_slot)) return;
        combo_type_ = sel_.type(); // CComboWnd::SetChoice(type)
        // placed at x, y kept on the screen (CXWndManager +0x10c / +0x110), centred without a point
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        auto r = host.textures ? host.textures->screen_rect(screen_) : std::nullopt;
        int w = r ? r->w : 450, h = r ? r->h : 300;
        int px = static_cast<int>(x), py = static_cast<int>(y);
        if (px < 0 || py < 0) {
            px = (static_cast<int>(ds.x) - w) / 2;
            py = (static_cast<int>(ds.y) - h) / 2;
        } else {
            if (static_cast<int>(ds.x) < w + px) px = static_cast<int>(ds.x) - w;
            if (static_cast<int>(ds.y) < h + py) py = static_cast<int>(ds.y) - h;
        }
        move_ = std::array<int, 2>{px, py};
        open_ = true;
    }
    // Shown some other way (hs.ShowWindow, /windows): the original's caches are made at start-up (InitIconCaches),
    // so it lists the current type's icons (Spell at first) without an owner.
    void set_shown(UiHost& host, bool show) override {
        if (show) ranges(host);
        open_ = show;
    }

    void child(UiHost& host, std::string_view id, Child& out) override {
        if (id == "ISW_PageIndex_Label") {
            out.text = std::to_string(sel_.page_number());
        } else if (id == "ISW_PageRange_Label") {
            out.text = sel_.range_text();
        } else if (id == "ISW_Slot_EditBox") {
            out.text = std::to_string(sel_.selected());
        } else if (id == "ISW_SelectedItem_Button") {
            // its +0x240 decal: the selected icon (GetIcon)
            if (auto c = icon(host, sel_.selected())) {
                out.decal.kind = ChildArt::Image;
                out.decal.cell = *c;
            } else {
                out.decal.kind = ChildArt::None;
            }
        } else if (id == "ISW_IconTemplate_Button") {
            out.shown = false; // only copied into ISW_Icons
        }
    }

    const ComboData* combo(UiHost&, std::string_view id) override {
        if (id == "ISW_Type_Combobox") {
            type_combo_.choices = kTypeChoices;
            type_combo_.selected = combo_type_ >= 0 && combo_type_ < 3 ? combo_type_ : -1;
            return &type_combo_;
        }
        if (id == "ISW_PerPage_Combobox") {
            per_page_combo_.choices = kPerPageChoices;
            per_page_combo_.selected = sel_.per_page_choice();
            return &per_page_combo_;
        }
        return nullptr;
    }

    bool combo_select(UiHost& host, std::string_view id, int index) override {
        ranges(host);
        if (id == "ISW_Type_Combobox") {
            combo_type_ = index;
            sel_.choose_type(index);
        } else if (id == "ISW_PerPage_Combobox") {
            sel_.choose_per_page(index);
        } else {
            return false;
        }
        return true;
    }

    // ISW_Slot_EditBox's notifications 6 and 0x21: SetSelectedIcon(the number typed).
    bool set_text(UiHost& host, std::string_view id, const std::string& t) override {
        if (id != "ISW_Slot_EditBox") return false;
        ranges(host);
        sel_.set_selected(std::atoi(t.c_str()));
        return true;
    }

    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (button != "left") return false;
        ranges(host);
        if (id == "ISW_PageLeft_Button") {
            sel_.page_left();
        } else if (id == "ISW_PageRight_Button") {
            sel_.page_right();
        } else if (id == "ISW_Accept_Button") {
            // sub_0068e710: hidden, and the owner's WndNotification(this, 0x28, {type, selected}): 0x550040 sets the
            // button's icon (on the page it shows now) and saves.
            open_ = false;
            if (owner_ && host.hot_buttons && host.set_hot_button) {
                game::HotButton b = host.hot_buttons->current(owner_->first, owner_->second);
                if (!b.empty()) {
                    b.icon_type = sel_.type();
                    b.icon_slot = sel_.selected();
                    host.set_hot_button(owner_->first, owner_->second, b);
                }
            }
            owner_.reset();
        } else if (id.starts_with("ISW_Icon:")) {
            // a click on one of ISW_Icons' buttons (CXWnd::IsDescendantOf): SetSelectedIcon(its icon, +0x34)
            int n = 0;
            auto rest = id.substr(9);
            std::from_chars(rest.data(), rest.data() + rest.size(), n);
            sel_.set_selected(n);
        } else {
            return false;
        }
        return true;
    }

    std::optional<std::array<int, 2>> take_move(UiHost&) override {
        auto m = move_;
        move_.reset();
        return m;
    }

    // The icon n of the current type (IconCache::GetIcon).
    std::optional<ui::UiTextures::Cell> icon(UiHost& host, std::int32_t n) const {
        return hot_button_custom_icon(host, sel_.type(), n);
    }
    const game::IconSelection& selection() const { return sel_; }

private:
    // InitIconCaches: the four caches' ranges from the skin's grid animations.
    void ranges(UiHost& host) {
        for (int t = 0; t < game::kIconTypes; ++t) {
            std::optional<std::int32_t> cells;
            if (host.textures)
                if (auto c = host.textures->grid_cells(game::icon_type_animation(t))) cells = *c;
            sel_.ranges[t] = game::icon_cache_range(t, cells);
        }
    }

    game::IconSelection sel_;
    int combo_type_ = 1; // ISW_Type_Combobox starts on Spell
    std::optional<std::pair<int, int>> owner_; // the CHotButton (+0x224): bar, button
    std::optional<std::array<int, 2>> move_;
    ComboData type_combo_, per_page_combo_;
};

// first, last, selected = hs.IconSelectionIcons()   -- the icon selection window's page: its icons first..last
int IconSelectionIcons(lua_State* L) {
    if (!g_icon_selection) return 0;
    const game::IconSelection& s = g_icon_selection->selection();
    lua_pushinteger(L, s.first());
    lua_pushinteger(L, s.last());
    lua_pushinteger(L, s.selected());
    return 3;
}

// tex, u0, v0, u1, v1 = hs.IconSelectionIcon(n)   -- icon n of the window's type (nothing: none)
int IconSelectionIcon(lua_State* L) {
    auto* host = static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!g_icon_selection || !host) return 0;
    auto c = g_icon_selection->icon(*host, static_cast<std::int32_t>(luaL_checkinteger(L, 1)));
    if (!c) return 0;
    lua_pushlightuserdata(L, c->id);
    lua_pushnumber(L, c->u0);
    lua_pushnumber(L, c->v0);
    lua_pushnumber(L, c->u1);
    lua_pushnumber(L, c->v1);
    return 5;
}

// value = hs.HotBarSetting(bar, name)   -- bar 1..10's "PercentSize", "ShowKeyMap" or "ShowSpinner"
int HotBarSetting(lua_State* L) {
    int bar = held_index(luaL_checkinteger(L, 1));
    std::string_view name = luaL_checkstring(L, 2);
    if (!valid_bar(bar)) return 0;
    const HotBarSettings& s = g_bars[static_cast<std::size_t>(bar)].settings;
    if (name == "PercentSize") lua_pushinteger(L, s.percent);
    else if (name == "ShowKeyMap") lua_pushboolean(L, s.key_map);
    else if (name == "ShowSpinner") lua_pushboolean(L, s.spinner);
    else return 0;
    return 1;
}

// ok = hs.HotBarMenu(bar [, x, y])   -- bar 1..10's window menu at x, y (default: the mouse; a pad's bar)
int HotBarMenu(lua_State* L) {
    auto* host = static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    int bar = held_index(luaL_checkinteger(L, 1));
    if (!host || !valid_bar(bar)) return 0;
    ImVec2 mp = ImGui::GetMousePos();
    if (!ImGui::IsMousePosValid(&mp)) mp = ImVec2(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y / 2);
    // (held to a screen's range: a double past float's would make the cast undefined)
    float x = lua_isnumber(L, 2) ? static_cast<float>(std::clamp(lua_tonumber(L, 2), -1e6, 1e6)) : mp.x;
    float y = lua_isnumber(L, 3) ? static_cast<float>(std::clamp(lua_tonumber(L, 3), -1e6, 1e6)) : mp.y;
    open_hot_bar_menu(*host, bar, x, y);
    lua_pushboolean(L, !host->menus.empty());
    return 1;
}

} // namespace

const HotBarSettings& hot_bar_settings(int bar) {
    static const HotBarSettings kNone;
    return valid_bar(bar) ? g_bars[static_cast<std::size_t>(bar)].settings : kNone;
}

void load_hot_bar_settings(int bar, ui::UiIni& ini, std::string_view section) {
    if (!valid_bar(bar)) return;
    Bar& b = g_bars[static_cast<std::size_t>(bar)];
    b.ini = &ini;
    b.section = std::string(section);
    // ReadUIINIString_IfEnabled(section, key, the current value) and atoi
    auto read = [&](const char* key, int current) {
        auto v = ini.get(section, key);
        return v ? std::atoi(v->c_str()) : current;
    };
    b.settings = HotBarSettings{};
    b.settings.percent = read("PercentSize", b.settings.percent);
    b.settings.key_map = read("ShowKeyMap", b.settings.key_map ? 1 : 0) != 0;
    b.settings.spinner = read("ShowSpinner", b.settings.spinner ? 1 : 0) != 0;
}

void register_hot_bar_window(int bar, StockWindow* window) {
    if (valid_bar(bar)) g_bars[static_cast<std::size_t>(bar)].window = window;
}

void unregister_hot_bar_window(int bar, StockWindow* window) {
    if (!valid_bar(bar) || g_bars[static_cast<std::size_t>(bar)].window != window) return;
    g_bars[static_cast<std::size_t>(bar)].window = nullptr;
    g_bars[static_cast<std::size_t>(bar)].ini = nullptr; // the UI ini goes with the windows
}

void open_hot_bar_size(UiHost& host, int bar, float x, float y) {
    if (!valid_bar(bar) || host.quantity) return; // unk_CXWnd_IsActive(pinstCQuantityWnd): one at a time
    game::QuantityOpen q;
    q.min = game::kHotButtonPercentMin;
    q.max = game::kHotButtonPercentMax;
    q.start = g_bars[static_cast<std::size_t>(bar)].settings.percent;
    q.x = static_cast<std::int32_t>(x);
    q.y = static_cast<std::int32_t>(y);
    q.use_keys = false;
    q.has_title = true; // "Percent"
    open_quantity(host, q, [bar](std::int32_t v) {
        // WndNotification 0x24: PercentSize = the quantity, StoreIniInfo (the buttons take it in their
        // CHotButtonWnd_UpdateButtonStates: CHotButton::SetButtonSize(PercentSize, 1))
        g_bars[static_cast<std::size_t>(bar)].settings.percent = v;
        store(bar);
    });
}

void open_hot_bar_menu(UiHost& host, int bar, float x, float y, bool layout_lock) {
    if (!host.conn || !valid_bar(bar)) return;
    const std::string screen = screen_of(bar);
    std::vector<UiHost::MenuRow> rows;
    // 6643 Window: CContextMenuManager's default menu (this client's window menu)
    rows.push_back(row(text(host, game::kStrHbWindow), [&host, screen] { host.window_menu = screen; }));
    rows.push_back(separator());
    // sub_0068abd0: the sets in use into the Load and Delete submenus (table order); both disabled with none
    const int sets = host.actions ? host.actions->hot_button_sets().in_use_count() : 0;
    auto set_rows = [&host, bar](bool load) {
        std::vector<UiHost::MenuRow> out;
        if (!host.actions) return out;
        for (int n = 0; n < game::kHotButtonSets; ++n) {
            const game::HotButtonSet& s = host.actions->hot_button_sets().at(n);
            if (!s.in_use) continue;
            out.push_back(row(s.name, [&host, bar, n, load] {
                // ConfirmationDialog 0x80 (8030) / 0x81 (8031); Yes: HotButtons_LoadFromIniFile
                ask(host, load ? game::kStrHbAskLoad : game::kStrHbAskDelete, [&host, bar, n, load] {
                    if (!host.actions || !host.conn) return;
                    if (load) host.actions->load_hot_button_set(*host.conn, bar, n);
                    else host.actions->delete_hot_button_set(*host.conn, n);
                });
            }));
        }
        return out;
    };
    UiHost::MenuRow load = row(text(host, game::kStrHbLoadSet));
    load.submenu = [set_rows] { return set_rows(true); };
    rows.push_back(sets > 0 ? std::move(load) : disabled(std::move(load)));
    rows.push_back(row(text(host, game::kStrHbSaveSet), [&host, bar] {
        // CTextEntryWnd::Activate(this, 0, 8008, 1, 0x18, ""): its OK (0x26, 1) saves the set
        UiHost::TextEntry e;
        e.prompt = text(host, game::kStrHbEnterSetName);
        e.min_chars = 1;
        e.max_chars = static_cast<int>(game::kHotButtonSetNameMax);
        e.done = [&host, bar](const std::string& name) {
            if (host.actions && host.conn) host.actions->save_hot_button_set(*host.conn, bar, name);
        };
        host.text_entry = std::move(e);
    }));
    UiHost::MenuRow del = row(text(host, game::kStrHbDeleteSet));
    del.submenu = [set_rows] { return set_rows(false); };
    rows.push_back(sets > 0 ? std::move(del) : disabled(std::move(del)));
    rows.push_back(row(text(host, game::kStrHbClearAll), [&host, bar] {
        ask(host, game::kStrHbAskClear, [&host, bar] {
            if (host.actions && host.conn) host.actions->clear_hot_bar(*host.conn, bar);
        });
    }));
    rows.push_back(separator());
    // 0x1964 Button Size: CQuantityWnd at the mouse
    rows.push_back(row(text(host, game::kStrHbButtonSize), [&host, bar] {
        ImVec2 mp = ImGui::GetMousePos();
        if (!ImGui::IsMousePosValid(&mp)) mp = ImVec2(-1, -1);
        open_hot_bar_size(host, bar, mp.x, mp.y);
    }));
    const HotBarSettings& s = g_bars[static_cast<std::size_t>(bar)].settings;
    // 0x19c8 Show Key Map / 0x1a2c Show Spinner: checked by the setting, toggle it and StoreIniInfo
    UiHost::MenuRow keys = row(text(host, game::kStrHbShowKeyMap), [bar] {
        auto& b = g_bars[static_cast<std::size_t>(bar)].settings;
        b.key_map = !b.key_map;
        store(bar);
    });
    keys.check = s.key_map;
    rows.push_back(std::move(keys));
    UiHost::MenuRow spinner = row(text(host, game::kStrHbShowSpinner), [bar] {
        auto& b = g_bars[static_cast<std::size_t>(bar)].settings;
        b.spinner = !b.spinner;
        store(bar);
    });
    spinner.check = s.spinner;
    rows.push_back(std::move(spinner));
    // The frame's layout lock (hud.lock.<screen>, the same lock the modern UI's menu has): a drag no longer moves or
    // resizes the bar. Hailstone's row, not the original's.
    if (layout_lock && host.set_layout_setting) {
        rows.push_back(separator());
        const std::string lock_key = "hud.lock." + screen;
        UiHost::MenuRow lock = row("Lock layout", [&host, lock_key] {
            if (!host.set_layout_setting) return;
            const bool locked = host.layout_setting && host.layout_setting(lock_key) == "1";
            host.set_layout_setting(lock_key, locked ? "0" : "1");
        });
        lock.check = host.layout_setting && host.layout_setting(lock_key) == "1";
        rows.push_back(std::move(lock));
    }
    // 0x1a90 Open New Bar (0x68b6f0): enabled while a bar is hidden; shows the first (its Show stored)
    int hidden = -1;
    for (int b = 0; b < game::kHotBars && hidden < 0; ++b)
        if (StockWindow* w = g_bars[static_cast<std::size_t>(b)].window; w && !w->shown(host)) hidden = b;
    UiHost::MenuRow open_new = row(text(host, game::kStrHbOpenNewBar), [&host] {
        for (int b = 0; b < game::kHotBars; ++b)
            if (StockWindow* w = g_bars[static_cast<std::size_t>(b)].window; w && !w->shown(host)) {
                w->set_shown(host, true);
                return;
            }
    });
    rows.push_back(hidden >= 0 ? std::move(open_new) : disabled(std::move(open_new)));
    host.open_menu(x, y, std::move(rows));
}

void open_icon_selection(UiHost& host, int bar, int button, float x, float y) {
    if (g_icon_selection && valid_bar(bar)) g_icon_selection->open(host, bar, button, x, y);
}

std::optional<ui::UiTextures::Cell> hot_button_custom_icon(UiHost& host, std::int32_t type, std::int32_t icon) {
    const char* anim = game::icon_type_animation(type);
    if (!anim || !host.textures) return std::nullopt;
    std::optional<std::int32_t> cells;
    if (auto c = host.textures->grid_cells(anim)) cells = *c;
    const game::IconCacheRange r = game::icon_cache_range(type, cells);
    if (icon < r.min || icon >= r.max) return std::nullopt;
    return host.textures->grid_cell(anim, icon - r.base);
}

std::string hot_button_key_text(UiHost& host, int bar, int button) {
    if (!host.actions || !valid_bar(bar) || button < 0 || button >= game::kHotButtons) return {};
    const game::KeyCombo& k = host.actions->keymap().binding(0x25 + bar * game::kHotButtons + button, 0);
    return k.bound() ? game::key_text(k) : std::string(); // KeyCombo_IsSet, else ""
}

void add_hotbar_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<IconSelectionWindow>());
}

void register_hotbar_api(lua_State* L, UiHost& h) {
    static const luaL_Reg fns[] = {{"IconSelectionIcons", IconSelectionIcons}, {"IconSelectionIcon", IconSelectionIcon},
                                   {"HotBarSetting", HotBarSetting}, {"HotBarMenu", HotBarMenu}, {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
