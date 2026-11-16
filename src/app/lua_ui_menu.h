#pragma once

// The EQ button's menu (CEQMainWnd's cascade menu): __CreateCascadeMenuItems @0x494bd0 reads
// uifiles/default/CascadeMenu.txt, CascadeMenuMgr_CreateMenu / CCascadeTopWnd_CreateMenuItems @0x645590 stack the
// item windows (CascadeCommandItemWnd 180 x 24, CascadeSubmenuItemWnd 190 x 24, CascadeSeparatorItemWnd 190 x 6) in
// a CascadeTopWnd / CascadeSubWnd with the WDT_RoundedNoTitle border (5 pixels), and CascadeMenuMgr_ShowSubmenu
// @0x645090 puts a menu right of its parent's rectangle (left of it when it would leave the screen, or when its
// parent menu opened to the left), as high as the parent and kept on screen.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui/ui_textures.h"

namespace hailstone { class GameDir; }

namespace hailstone::app {

struct CascadeItemView {
    std::string kind; // "command", "submenu", "separator"
    std::string text;
    std::array<float, 4> rect{}; // x, y, w, h (screen)
    std::optional<ui::UiTextures::Cell> icon, background, arrow;
    std::array<float, 4> arrow_rect{};
    bool enabled = true;
};

struct CascadeMenuView {
    float x = 0, y = 0, w = 0, h = 0;
    std::vector<CascadeItemView> items;
};

// Hailstone's own key command (not in the original's table): the Ability Book (AbilityBookWnd, game/ability_book.h).
// The EQ menu offers it after Combat Abilities.
inline constexpr int kAbilityBookCommand = 1000;
// Hailstone's own: the Advanced Inventory (InventoryManagerWnd, game/kept_items.h), offered after Inventory.
inline constexpr int kInventoryManagerCommand = 1001;

// The stock window a key command shows and hides (the window keys and EQ menu rows of the Lua windows:
// ClientApp::key_command toggles it when the UI pack replaces it), nullptr for other commands.
const char* toggle_command_window(int command);

// Whether the EQ menu offers a row running `command`: not a command left out on purpose (game::key_command_dropped:
// the Marketplace, polls, real estate ...), and one this client runs: a window toggle whose window the UI pack
// draws (`replaces`, UiApi::replaces; the guild, raid, friends, actions, skills, combat abilities, options and first
// hot bar keys have a window of the client's own otherwise), or the inventory, spell book, mercenary and camp
// commands. Newer games' CascadeMenu.txt (an asset folder of a later client) name windows RoF2 does not have or
// this client does not draw (the zone guide, Overseer, the adventure request outside a recruiter ...): those rows
// are left out, so every row of the menu opens something.
bool menu_command_offered(int command, const std::function<bool(std::string_view)>& replaces);

// Hailstone's own EQ menu row, "Report a Bug" (the bug report window: app/bug_report.h): not a key command, so a
// number no key command has; CascadeMenu::click returns it like one.
inline constexpr int kMenuReportBug = 0x10000;
// Hailstone's "Web Browser" row (the in-game browser's window, BrowserWnd; docs/browser.md), before Report a Bug.
inline constexpr int kMenuWebBrowser = 0x10001;
// Hailstone's "Lua Scripts" row (the Scripts window, ScriptsWnd: run, stop and autorun the MacroQuest Lua scripts;
// docs/plugins.md "The Scripts window"), before Web Browser.
inline constexpr int kMenuScripts = 0x10002;
// Hailstone: the plugins' menu entries (plugin/ui_bridge.h) in a "Plugins" submenu; entry i's row returns
// kMenuPluginFirst + i.
inline constexpr int kMenuPluginFirst = 0x20000;

class CascadeMenu {
public:
    // Reads CascadeMenu.txt once; `expansions` is the player's expansion bit mask (a line's first field n > 0 needs
    // bit n - 1). The PvP leaderboard entry (CMD_TOGGLE_PVP_LEADERBOARD) needs a PvP rule set (0xdcee70 != 0);
    // the ETN help entry needs 0xddfd42, which EQEmu never sets. A command row is kept when `offered(command)`
    // (menu_command_offered; every row when it is empty); a command name the key map does not know (a later
    // client's) drops its row. A submenu left empty goes, and so do the separators that no longer part two rows.
    // Hailstone adds "Lua Scripts", "Web Browser" and "Report a Bug" (kMenuScripts, kMenuWebBrowser, kMenuReportBug)
    // to the top menu before Help, else before Camp & Quit.
    //
    // Hailstone: the rows come from hailstone_layout(), the same whatever game folder the player uses (a live
    // client's CascadeMenu.txt differs from RoF2's), unless the server's files bring a CascadeMenu.txt: that one
    // lays the menu out instead. The game folder's own file is not read.
    void load(const GameDir& game, std::uint32_t expansions, std::int32_t pvp_rules,
              const std::function<bool(int)>& offered = {});
    // Hailstone's own layout, in CascadeMenu.txt's format (flag^kind^icon^text^command; the icon a cell of the
    // player's menuicons01.tga, the command a key map name).
    // Loaded, it is put in alphabetical order: the submenus first, then the other rows, then Help, Report a Bug and
    // Camp & Quit; each submenu's rows alphabetical.
    static std::string_view hailstone_layout();
    // load() from CascadeMenu.txt's text (the tests).
    void load_text(std::string_view text, std::uint32_t expansions, std::int32_t pvp_rules,
                   const std::function<bool(int)>& offered = {});
    // The rows of menu `menu` (0 the top one): their texts, a submenu's with " >", a separator "-".
    std::vector<std::string> row_texts(int menu) const;
    int menu_count() const { return static_cast<int>(menus_.size()); }
    // Closes the menus and reads CascadeMenu.txt again at the next load() (the game files' layers changed).
    void reload() {
        loaded_ = false;
        open_.clear();
    }
    bool open() const { return !open_.empty(); }
    // The EQ button: opens the top menu beside `owner` (the EQ window's screen rectangle), or closes every menu.
    void toggle(const std::array<int, 4>& owner, int screen_w, int screen_h);
    void close() { open_.clear(); }
    std::vector<CascadeMenuView> views(ui::UiTextures* textures) const;
    // Hovering a submenu row opens its submenu (closing deeper ones); another row closes this menu's submenus.
    void hover(int menu, int item, int screen_w, int screen_h);
    // A click on a command row: its key command (FindMappableCommand), and every menu closes; nullopt otherwise.
    std::optional<int> click(int menu, int item);
    // The plugins' entries (labels, in order): a "Plugins" submenu of the top menu, before Hailstone's rows ("Lua Scripts", "Web Browser", "Report a Bug"); none when
    // empty. A change closes the menus.
    void set_plugin_entries(std::vector<std::string> labels);

private:
    struct Item {
        int kind = 1; // 1 command, 2 separator, 5 submenu
        int icon = -1;
        std::string text;
        int command = -1;
        int submenu = -1; // index into menus_
    };
    struct Menu {
        std::vector<Item> items;
    };
    struct Open {
        int menu = 0;
        int x = 0, y = 0, w = 0, h = 0;
        bool flipped = false;
    };
    void show(int menu, const std::array<int, 4>& parent, bool parent_flipped, int screen_w, int screen_h);
    std::array<int, 2> size(int menu) const;

    std::vector<Menu> menus_; // menus_[0] is the top menu
    std::vector<Open> open_;  // the top menu, then each submenu
    bool loaded_ = false;
    std::vector<std::string> plugin_labels_;
    int plugin_menu_ = -1; // its index in menus_ (the last), -1 none
    void apply_plugin_entries();
    bool sorted_ = false; // Hailstone's layout: arrange() orders it (a server's file keeps its own order)
    void arrange();
};

} // namespace hailstone::app
