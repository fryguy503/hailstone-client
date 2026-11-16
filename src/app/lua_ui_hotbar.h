#pragma once

// The hot bar window's own parts (app/lua_ui_hotbar.cpp): CHotButtonWnd's settings in the UI ini and its window
// menu (hotbutton sets, Clear current hotbuttons, Button Size, Show Key Map, Show Spinner, Open New Bar;
// game/hot_button_sets.h), the key map text on its buttons (0x68a720), a hotbutton's custom icon (0x54ff40),
// and the icon selection window (CIconSelectionWnd, game/icon_selection.h) a hotbutton's Assign Icon opens.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/lua_ui.h"
#include "ui/ui_textures.h"

struct lua_State;
namespace hailstone::ui { class UiIni; }

namespace hailstone::app {

class StockWindow;

// A bar's settings (CHotButtonWnd +0x29c PercentSize, +0x2a4 ShowKeyMap, +0x2a5 ShowSpinner): LoadIniInfo reads them
// from the bar's UI ini section (its IniStorageName) over the constructor's 100, 1, 1; StoreIniInfo writes all three.
struct HotBarSettings {
    int percent = 100;
    bool key_map = true;
    bool spinner = true;
};
const HotBarSettings& hot_bar_settings(int bar);
void load_hot_bar_settings(int bar, ui::UiIni& ini, std::string_view section);
// The bar's window (HotButtonWnd, HotButtonWnd2 ..) for Open New Bar: the first hidden one is shown.
void register_hot_bar_window(int bar, StockWindow* window);
void unregister_hot_bar_window(int bar, StockWindow* window);

// The bar's window menu (CHotButtonWnd's ContextMenuID: the right button on the window) at x, y. `layout_lock`: with
// the Lock layout row (the modern UI's frames, which the window's own menu opens; hs.HotBarMenu, the gamepad pack's
// pages, has no frame to lock).
void open_hot_bar_menu(UiHost& host, int bar, float x, float y, bool layout_lock = false);
// 0x68a170: CQuantityWnd::Open(bar, 175, PercentSize, x, y, 0, 40, "Percent"); its answer (0x24) is the bar's
// PercentSize, stored (StoreIniInfo). The bar's Button Size and a button's Change Size.
void open_hot_bar_size(UiHost& host, int bar, float x, float y);
// 0x68f330: the icon selection window on bar 0..9's button 0..11 (its {icon type, icon}) at x, y.
void open_icon_selection(UiHost& host, int bar, int button, float x, float y);
// 0x54ff40: a button's own icon (icon type != -1: __GetAnimationCache(type)->GetIcon(icon)), drawn instead of
// what the button would show (its decal, its item's icon, its gem's spell icon). Nullopt: none.
std::optional<ui::UiTextures::Cell> hot_button_custom_icon(UiHost& host, std::int32_t type, std::int32_t icon);
// 0x68a720: the key bound to HOT<bar + 1>_<button + 1> (KeypressHandler::GetKeyAttachedToEqCommand(0x25 + bar *
// 12 + button), KeyCombo::GetTextDescription), "" when unbound; 0x5506b0 draws it on the button while the bar's
// ShowKeyMap is on.
std::string hot_button_key_text(UiHost& host, int bar, int button);

void add_hotbar_windows(std::vector<std::unique_ptr<StockWindow>>& out);
// hs.IconSelectionIcons(): the icon selection window's page of icons.
void register_hotbar_api(lua_State* L, UiHost& host);

} // namespace hailstone::app
