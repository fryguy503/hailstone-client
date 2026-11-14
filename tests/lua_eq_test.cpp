// The eq API's arithmetic taken from the original (app/lua_eq_values.h, ui/ui_textures.h), the EQ menu's rows
// (app/lua_eq_menu.h), and the window functions' lookups: windows by name, lists once a frame, eq.WindowShown.
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include <set>
#include <string>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "app/lua_eq_caps.h"
#include "app/lua_eq_menu.h"
#include "app/lua_eq_values.h"
#include "core/game_dir.h"
#include "game/keymap.h"
#include "ui/dev_screens.h"
#include "ui/ui_textures.h"
#include "scratch_dir.h"

using namespace hailstone;

TEST_CASE("eq_percent: 0x4b37a0, cur / (max * 0.01f) truncated") {
    CHECK(app::eq_percent(59, 59) == 100);
    CHECK(app::eq_percent(0, 59) == 0);
    CHECK(app::eq_percent(5, 0) == 0);
    CHECK(app::eq_percent(1, 3) == 33);
    CHECK(app::eq_percent(2, 3) == 66);
    CHECK(app::eq_percent(99, 100) == 99);
}

TEST_CASE("compare_color: GetValueCompareColor grey, green above, red below") {
    CHECK(app::compare_color(10, 10) == 0xffc0c0c0u);
    CHECK(app::compare_color(11, 10) == 0xff00ff00u);
    CHECK(app::compare_color(9, 10) == 0xffff0000u);
}

TEST_CASE("gem_icon_cell: A_SpellGems, 24-pixel cells, 10 x 10 a sheet, across rows") {
    auto c = ui::gem_icon_cell(0);
    REQUIRE(c);
    CHECK(c->sheet == 1);
    CHECK(c->x == 0);
    CHECK(c->y == 0);
    c = ui::gem_icon_cell(13);
    REQUIRE(c);
    CHECK(c->x == 72);
    CHECK(c->y == 24);
    c = ui::gem_icon_cell(205);
    REQUIRE(c);
    CHECK(c->sheet == 3);
    CHECK(c->x == 120);
    CHECK(c->y == 0);
    CHECK_FALSE(ui::gem_icon_cell(300));
    CHECK_FALSE(ui::gem_icon_cell(-1));
}

// A later client's CascadeMenu.txt (the live game's, cut down): Daybreak's services, windows RoF2 does not have
// (command names the key map does not know) and windows the UI pack does not draw are left out; a submenu left
// empty goes, and so do the separators that part nothing.
TEST_CASE("EQ menu: only the rows this client runs") {
    const char* text =
        "0^3\n"
        "0^4^-1^Actions^TOGGLE_MAINMENUWIN\n"
        "0^4^-1^Advanced Loot^CMD_TOGGLE_ADVANCED_LOOT_WIN\n"
        "0^4^-1^Voice Macro^CMD_TOGGLEVOICEWIN\n"
        "0^4^-1^Alarm^CMD_TOGGLE_ALARM\n"
        "0^5^8^Actions\n"
        "0^3\n"
        "0^4^-1^Alert History^CMD_HISTORIC_ALERTS\n"
        "0^4^-1^Event Calendar^CMD_EVENT_CALENDAR\n"
        "0^5^19^Alerts\n"
        "0^3\n"
        "0^4^-1^Achievements^CMD_ACHIEVEMENT_WINDOW\n"
        "0^4^-1^Auras^CMD_TOGGLE_AURAWND\n"
        "0^4^-1^Zone Guide^TOGGLE_ZONE_GUIDE_WINDOW\n"
        "0^5^5^Character\n"
        "0^3\n"
        "0^4^-1^Rewards^CMD_TOGGLE_REWARD_SELECTION_WIN\n"
        "0^4^-1^Quest Journal^CMD_TOGGLETASKWIN\n"
        "0^5^7^Quests\n"
        "17^3\n"
        "17^4^-1^Manage^CMD_REAL_ESTATE_MANAGE_WINDOW\n"
        "17^5^17^Real Estate\n"
        "0^2\n"
        "0^1^11^Claims^CMD_TOGGLE_CLAIM_WIN\n"
        "0^1^4^Mail^TOGGLE_MAILWIN\n"
        "0^1^14^Marketplace^CMD_MARKETPLACE\n"
        "0^1^13^Mercenaries^CMD_MERCENARIES\n"
        "0^1^18^Polls^CMD_VOTE_RESULTS\n"
        "0^2\n"
        "0^1^11^Upgrade EverQuest^OPEN_ETN_HELP\n"
        "0^2\n"
        "0^1^2^Options^TOGGLE_OPTIONSWIN\n"
        "0^1^0^Camp & Quit^CAMP\n";
    // The pack draws these windows (the classic pack draws them all; the gamepad pack has no aura window).
    std::set<std::string, std::less<>> drawn = {"AlarmWnd", "AuraWindow", "TaskWnd", "MailWindow", "AlertHistoryWnd"};
    auto replaces = [&](std::string_view screen) { return drawn.count(screen) > 0; };
    auto offered = [&](int c) { return app::menu_command_offered(c, replaces); };
    app::CascadeMenu menu;
    menu.load_text(text, 0xffffffffu, 0, offered);
    CHECK(menu.row_texts(0) == std::vector<std::string>{"Actions >", "Character >", "Quests >", "-", "Mail",
                                                        "Mercenaries", "-", "Options", "Lua Scripts", "Web Browser",
                                                        "Report a Bug", "Camp & Quit"});
    CHECK(menu.row_texts(1) == std::vector<std::string>{"Actions", "Alarm"});
    CHECK(menu.row_texts(3) == std::vector<std::string>{"Auras"});
    CHECK(menu.row_texts(4) == std::vector<std::string>{"Quest Journal"});

    // Without the aura window (the gamepad pack) its row goes; the actions, options, skills ... keys always work.
    drawn.erase("AuraWindow");
    app::CascadeMenu pad;
    pad.load_text(text, 0xffffffffu, 0, offered);
    CHECK(pad.row_texts(0) == std::vector<std::string>{"Actions >", "Quests >", "-", "Mail", "Mercenaries", "-",
                                                       "Options", "Lua Scripts", "Web Browser", "Report a Bug",
                                                       "Camp & Quit"});
    // Hailstone's Lua Scripts row returns its own number (the Scripts window).
    {
        auto rows = pad.row_texts(0);
        pad.toggle({0, 0, 50, 20}, 1920, 1080);
        const int at = static_cast<int>(std::find(rows.begin(), rows.end(), "Lua Scripts") - rows.begin());
        CHECK(pad.click(0, at) == app::kMenuScripts);
    }

    // The plugins' entries: a "Plugins" submenu before Hailstone's rows (Lua Scripts, Web Browser, Report a Bug), each
    // row its entry's number; none, none.
    const int menus = pad.menu_count();
    pad.set_plugin_entries({"Ext Window", "Ext Two"});
    auto top = pad.row_texts(0);
    CHECK(top == std::vector<std::string>{"Actions >", "Quests >", "-", "Mail", "Mercenaries", "-", "Options",
                                          "Plugins >", "Lua Scripts", "Web Browser", "Report a Bug", "Camp & Quit"});
    REQUIRE(pad.menu_count() == menus + 1);
    CHECK(pad.row_texts(menus) == std::vector<std::string>{"Ext Window", "Ext Two"});
    pad.toggle({0, 0, 50, 20}, 1920, 1080);
    const int plugins_row = static_cast<int>(std::find(top.begin(), top.end(), "Plugins >") - top.begin());
    pad.hover(0, plugins_row, 1920, 1080);
    CHECK(pad.click(1, 1) == app::kMenuPluginFirst + 1);
    pad.set_plugin_entries({"Other"});
    CHECK(pad.row_texts(pad.menu_count() - 1) == std::vector<std::string>{"Other"});
    pad.set_plugin_entries({});
    CHECK(pad.menu_count() == menus);
    CHECK(pad.row_texts(0) == std::vector<std::string>{"Actions >", "Quests >", "-", "Mail", "Mercenaries", "-",
                                                       "Options", "Lua Scripts", "Web Browser", "Report a Bug",
                                                       "Camp & Quit"});

    using C = game::KeyCommand;
    for (C c : {C::CMD_MARKETPLACE_SILVER, C::CMD_MARKETPLACE_GOLD, C::CMD_TOGGLE_CLAIM_WIN,
                C::CMD_VOTE_RESULTS, C::CMD_HISTORIC_ALERTS,
                C::CMD_REAL_ESTATE_TROPHIES_WINDOW, C::CMD_REAL_ESTATE_LEAVE, C::OPEN_ETN_HELP,
                C::CMD_TOGGLE_TCG, C::CMD_SOCIAL_SHARE})
        CHECK_FALSE(app::menu_command_offered(static_cast<int>(c), [](std::string_view) { return true; }));
    CHECK(std::string(app::toggle_command_window(static_cast<int>(C::CMD_TOGGLE_AURAWND))) == "AuraWindow");
    CHECK(app::toggle_command_window(static_cast<int>(C::CMD_HISTORIC_ALERTS)) == nullptr);
    const int reward = static_cast<int>(C::CMD_TOGGLE_REWARD_SELECTION_WIN);
    CHECK(std::string(app::toggle_command_window(static_cast<int>(C::CMD_ACHIEVEMENT_WINDOW))) == "AchievementsWnd");
    CHECK(app::menu_command_offered(static_cast<int>(C::CMD_ACHIEVEMENT_WINDOW), [](std::string_view screen) { return screen == "AchievementsWnd"; }));
    CHECK(std::string(app::toggle_command_window(reward)) == "RewardSelectionWnd");
    CHECK(app::menu_command_offered(reward, [](std::string_view screen) { return screen == "RewardSelectionWnd"; }));
    CHECK_FALSE(app::menu_command_offered(reward, [](std::string_view) { return false; }));
    // The Marketplace row is offered where the pack draws the window.
    CHECK(std::string(app::toggle_command_window(static_cast<int>(C::CMD_MARKETPLACE))) == "MarketplaceWnd");
    CHECK(app::menu_command_offered(static_cast<int>(C::CMD_MARKETPLACE), [](std::string_view s) { return s == "MarketplaceWnd"; }));
    CHECK_FALSE(app::menu_command_offered(static_cast<int>(C::CMD_MARKETPLACE), [](std::string_view) { return false; }));
    // The aggro meter is the target window's: no row of its own, even when the pack replaces AggroMeterWnd.
    CHECK_FALSE(app::menu_command_offered(static_cast<int>(C::CMD_TOGGLE_AGGRO), [](std::string_view) { return true; }));
    // Real estate's windows are kept (a server with housing feeds them): offered when the pack draws them.
    CHECK(app::menu_command_offered(static_cast<int>(C::CMD_REAL_ESTATE_MANAGE_WINDOW), [](std::string_view) { return true; }));
    CHECK(std::string(app::toggle_command_window(static_cast<int>(C::CMD_REAL_ESTATE_ITEMS_WINDOW))) == "RealEstateItemsWnd");
}

// Hailstone's own EQ menu layout: every row names a command the key map knows, so with every window offered none
// is left out, and the top menu is the same whatever game folder the player uses.
TEST_CASE("EQ menu: Hailstone's layout") {
    std::istringstream in{std::string(app::CascadeMenu::hailstone_layout())};
    std::string line;
    int rows = 0;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::string cur;
        for (char c : line) {
            if (c == '^') {
                f.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        f.push_back(cur);
        REQUIRE(f.size() >= 2);
        if (f[1] != "1" && f[1] != "4") continue;
        REQUIRE(f.size() == 5);
        INFO(line);
        CHECK(game::command_by_name(f[4]).has_value());
        ++rows;
    }
    CHECK(rows > 60);

    app::CascadeMenu menu;
    menu.load_text(app::CascadeMenu::hailstone_layout(), 0xffffffffu, 1);
    // In alphabetical order: the submenus, the other rows, then Help, Report a Bug and Camp & Quit.
    CHECK(menu.row_texts(0) == std::vector<std::string>{"Actions >", "Character >", "Hotkeys >", "Inventory >",
                                                        "Navigation >", "Players >", "Quests >", "Real Estate >", "-",
                                                        "Claims", "Lua Scripts", "Mail", "Marketplace", "Options",
                                                        "Web Browser", "-", "Help", "Report a Bug", "Camp & Quit"});
    // (the Advanced Inventory, added after Inventory, first)
    CHECK(menu.row_texts(2) == std::vector<std::string>{"Advanced Inventory", "Bandolier", "Barter Search",
                                                        "Bazaar Search", "Inventory"});
    // each submenu's rows too (the Ability Book, added after Combat Abilities, first); Mercenaries is a player's
    CHECK(menu.row_texts(3) == std::vector<std::string>{"Ability Book", "Actions", "Alarm", "Combat Abilities",
                                                        "Extended Target", "Target"});
    auto players = menu.row_texts(5);
    CHECK(std::is_sorted(players.begin(), players.end()));
    CHECK(std::find(players.begin(), players.end(), "Mercenaries") != players.end());
    // the plugins' submenu among the submenus, its entries in their own order
    menu.set_plugin_entries({"Zeta", "Alpha"});
    auto top = menu.row_texts(0);
    CHECK(std::vector<std::string>(top.begin() + 5, top.begin() + 8) ==
          std::vector<std::string>{"Players >", "Plugins >", "Quests >"});
    CHECK(menu.row_texts(menu.menu_count() - 1) == std::vector<std::string>{"Zeta", "Alpha"});
    CHECK(top.back() == "Camp & Quit");

    // Without the expansions that gate them (real estate, the search and blocked buff windows) and with no PvP rule
    // set, those rows go and their empty submenu with them.
    app::CascadeMenu early;
    early.load_text(app::CascadeMenu::hailstone_layout(), 0, 0);
    auto earlyTop = early.row_texts(0);
    CHECK(std::find(earlyTop.begin(), earlyTop.end(), "Real Estate >") == earlyTop.end());
    CHECK(early.row_texts(2) == std::vector<std::string>{"Advanced Inventory", "Bandolier", "Inventory"});
    auto character = early.row_texts(1);
    CHECK(std::find(character.begin(), character.end(), "PvP Stats") == character.end());
    CHECK(std::find(character.begin(), character.end(), "Auras") == character.end());
}

// The game folder's CascadeMenu.txt does not lay the menu out (a live client's differs from RoF2's); a server's,
// in its assets folder, does.
TEST_CASE("EQ menu: Hailstone's layout over the game folder's file, a server's file over both") {
    test::ScratchDir tmp("eq_menu_layout");
    auto put = [](const std::filesystem::path& dir, const char* text) {
        std::filesystem::create_directories(dir / "uifiles" / "default");
        std::ofstream(dir / "uifiles" / "default" / "CascadeMenu.txt", std::ios::binary) << text;
    };
    const auto base = tmp.path() / "game", over = tmp.path() / "server";
    put(base, "0^1^2^Game Folder Options^TOGGLE_OPTIONSWIN\n");
    std::filesystem::create_directories(over);
    GameDir game(base, over);

    app::CascadeMenu reference;
    reference.load_text(app::CascadeMenu::hailstone_layout(), 0xffffffffu, 1);
    app::CascadeMenu menu;
    menu.load(game, 0xffffffffu, 1);
    CHECK(menu.row_texts(0) == reference.row_texts(0));

    put(over, "0^1^4^Server Mail^TOGGLE_MAILWIN\n0^1^0^Camp & Quit^CAMP\n");
    game.rescan();
    menu.reload();
    menu.load(game, 0xffffffffu, 1);
    CHECK(menu.row_texts(0) ==
          std::vector<std::string>{"Server Mail", "Lua Scripts", "Web Browser", "Report a Bug", "Camp & Quit"});
}

namespace {

std::string run_eq(lua_State* L, const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

// The eq table over an EqApi, as the UI's script host registers it.
struct EqTable {
    app::EqApi api;
    lua_State* L;
    explicit EqTable(app::EqHost& host) : api(host), L(luaL_newstate()) {
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "eq");
    }
    ~EqTable() { lua_close(L); }
    EqTable(const EqTable&) = delete;
    EqTable& operator=(const EqTable&) = delete;
    std::string run(const std::string& code) { return run_eq(L, code); }
};

ui::ServerEntry world(const char* name) {
    ui::ServerEntry e;
    e.name = name;
    e.status = "Up";
    e.can_join = true;
    return e;
}

} // namespace

TEST_CASE("Fellowship window: the default shortcut and EQ menu resolve to its toggle") {
    std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
    {
        app::EqHost host;
        host.in_game = host.ui_ready = true;
        EqTable eq(host);
        CHECK(eq.run("return eq.ReplaceWindow('FellowshipWnd')") == "false");
        REQUIRE(eq.api.replaces("FellowshipWnd"));
        CHECK_FALSE(eq.api.shown("FellowshipWnd"));

        const int fellowship = static_cast<int>(game::KeyCommand::TOGGLE_FELLOWSHIPWIN);
        game::KeyMap keys;
        keys.reset_defaults(true);
        const game::KeyCombo shortcut{0x21, true, true, false}; // Ctrl+Shift+F
        CHECK(keys.binding(fellowship, 0) == shortcut);
        std::vector<game::KeyMap::Event> events;
        keys.key_down(shortcut, events);
        REQUIRE(events.size() == 1);
        CHECK(events.front().command == fellowship);
        CHECK(events.front().down);
        const char* screen = app::toggle_command_window(events.front().command);
        REQUIRE(screen != nullptr);
        CHECK(std::string(screen) == "FellowshipWnd");
        eq.api.toggle(screen);
        CHECK(eq.api.shown("FellowshipWnd"));

        events.clear();
        keys.key_up(shortcut.code, events);
        REQUIRE(events.size() == 1);
        CHECK_FALSE(events.front().down);
        events.clear();
        keys.key_down(shortcut, events);
        REQUIRE(events.size() == 1);
        screen = app::toggle_command_window(events.front().command);
        REQUIRE(screen != nullptr);
        eq.api.toggle(screen);
        CHECK_FALSE(eq.api.shown("FellowshipWnd"));

        const auto offered = [&](int command) {
            return app::menu_command_offered(command, [&](std::string_view name) { return eq.api.replaces(name); });
        };
        CHECK(offered(fellowship));
        app::CascadeMenu menu;
        menu.load_text("0^1^0^Fellowship^TOGGLE_FELLOWSHIPWIN\n", 0xffffffffu, 0, offered);
        const auto rows = menu.row_texts(0);
        const auto row = std::find(rows.begin(), rows.end(), "Fellowship");
        REQUIRE(row != rows.end());
        menu.toggle({0, 0, 50, 20}, 1920, 1080);
        const auto command = menu.click(0, static_cast<int>(row - rows.begin()));
        REQUIRE(command);
        CHECK(*command == fellowship);
        screen = app::toggle_command_window(*command);
        REQUIRE(screen != nullptr);
        eq.api.toggle(screen);
        CHECK(eq.api.shown("FellowshipWnd"));
    }
}

TEST_CASE("eq UI animation: registered read-only binding returns nil without an artwork provider") {
    ImGuiContext* context = ImGui::CreateContext();
    {
        app::EqHost host;
        EqTable eq(host);
        CHECK(eq.run("return type(eq.UiAnimation)") == "function");
        CHECK(eq.run("return eq.UiAnimation('A_EQ')") == "nil");
        CHECK(eq.run("return eq.UiAnimation('../outside.tga')") == "nil");
        CHECK(eq.run("return select('#',eq.UiAnimation('MissingBadge'))") == "1");
        CHECK(app::eq_call_caps("UiAnimation") == std::optional<std::uint32_t>{0});
    }
    ImGui::DestroyContext(context);
}

TEST_CASE("eq lists: built once a frame, again after a call that can change them") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        app::EqHost host;
        ui::ServerSelectView view;
        view.servers = {world("Alpha"), world("Beta")};
        host.server_select = &view;
        EqTable eq(host);
        const std::string list = "'serverselect', 'ServerList'";
        eq.api.begin_frame();
        CHECK(eq.run("return (eq.ListInfo(" + list + "))") == "2");
        CHECK(eq.run("return eq.ListRow(" + list + ", 1)[1]") == "Beta");
        // The worlds change behind the window's back: this frame's list is kept, by every reader.
        view.servers.push_back(world("Gamma"));
        CHECK(eq.run("return (eq.ListInfo(" + list + "))") == "2");
        CHECK(eq.run("return eq.ListRow(" + list + ", 2)") == "nil");
        CHECK(eq.run("return #eq.WindowList(" + list + ")") == "2");
        // A read of something else keeps it too.
        CHECK(eq.run("return eq.ReplaceWindow('serverselect')") == "true");
        CHECK(eq.run("return (eq.ListInfo(" + list + "))") == "2");
        // A click is a change: the list is built again in the same frame, with the click's selection.
        CHECK(eq.run("return eq.ListClick(" + list + ", 1, 'left')") == "true");
        CHECK(eq.run("return (eq.ListInfo(" + list + "))") == "3");
        CHECK(eq.run("return (select(3, eq.ListInfo(" + list + ")))") == "1");
        CHECK(eq.run("return eq.ListRow(" + list + ", 2)[1]") == "Gamma");
        // So is a header click (a sort), whether or not the window takes it, and any call that is not a read.
        view.servers.push_back(world("Delta"));
        CHECK(eq.run("return (eq.ListInfo(" + list + "))") == "3");
        eq.run("eq.ListHeaderClick(" + list + ", 0)");
        CHECK(eq.run("return (eq.ListInfo(" + list + "))") == "4");
        view.servers.push_back(world("Epsilon"));
        eq.run("eq.ClickChild('serverselect', 'NoSuchButton')");
        CHECK(eq.run("return #eq.WindowList(" + list + ")") == "5");
        // EqApi's own changes too.
        view.servers.push_back(world("Zeta"));
        eq.api.click_list("serverselect", "ServerList", 0, 0);
        CHECK(eq.run("return (eq.ListInfo(" + list + "))") == "6");
        CHECK(eq.run("return (select(3, eq.ListInfo(" + list + ")))") == "0");
        // A new frame builds it again.
        view.servers.pop_back();
        CHECK(eq.run("return (eq.ListInfo(" + list + "))") == "6");
        eq.api.begin_frame();
        CHECK(eq.run("return (eq.ListInfo(" + list + "))") == "5");
        // A window with no such list, and no such window.
        CHECK(eq.run("return eq.ListInfo('serverselect', 'Nope')") == "nil");
        CHECK(eq.run("return eq.ListInfo('NoSuchWnd', 'ServerList')") == "nil");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("eq.WindowShown: eq.ReplaceWindow's shown, the window counted as replaced, no place") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        app::EqHost host;
        EqTable eq(host);
        eq.api.begin_frame();
        // Hidden: the server select shows only with a world list.
        CHECK(eq.run("return eq.WindowShown('serverselect')") == "false");
        CHECK(eq.run("return (eq.ReplaceWindow('serverselect'))") == "false");
        ui::ServerSelectView view;
        host.server_select = &view;
        CHECK(eq.run("return eq.WindowShown('serverselect')") == "true");
        CHECK(eq.run("return (eq.ReplaceWindow('serverselect'))") == "true");
        // Not in the world: an in-game window is not shown, though its model is.
        CHECK(eq.run("return eq.WindowShown('InventoryWindow')") == "false");
        CHECK(eq.run("return (eq.ReplaceWindow('InventoryWindow'))") == "false");
        CHECK(eq.run("return eq.WindowShown('NoSuchWnd')") == "false");
        CHECK(eq.run("return eq.WindowShown()").rfind("error:", 0) == 0);

        // replaced_at: the C++ window stays off while the script asks every frame, as with eq.ReplaceWindow.
        CHECK_FALSE(eq.api.replaces("RaidWindow"));
        eq.run("eq.WindowShown('RaidWindow')");
        CHECK(eq.api.replaces("RaidWindow"));
        for (int i = 0; i < 2; ++i) eq.api.begin_frame();
        CHECK(eq.api.replaces("RaidWindow"));
        eq.api.begin_frame();
        CHECK_FALSE(eq.api.replaces("RaidWindow"));
        eq.run("eq.WindowShown('RaidWindow')");
        CHECK(eq.api.replaces("RaidWindow"));
        // The debug list counts it as replaced too.
        CHECK(eq.run("for _, s in ipairs(eq.DebugScreens()) do if s.name == 'RaidWindow' then return s.replaced end end") ==
              "true");

        // The window's move (the server select centred when it first shows) is left for eq.ReplaceWindow to take.
        host.server_select = nullptr;
        eq.run("eq.WindowShown('serverselect')");
        ImGui::GetIO().DisplaySize = ImVec2(1040, 680);
        host.server_select = &view;
        CHECK(eq.run("return eq.WindowShown('serverselect')") == "true");
        CHECK(eq.run("local s, x, y, w, h, sized, place = eq.ReplaceWindow('serverselect') return x .. ',' .. y .. ',' .. "
                     "tostring(place)") == "200,100,true");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("eq.TakeCloseTopWindow: the close-top-window key's press, taken once") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        app::EqHost host;
        EqTable eq(host);
        CHECK(eq.run("return eq.TakeCloseTopWindow()") == "false");
        host.close_top_window = true; // (ClientApp::key_command: CLOSE_TOP_WINDOW no window of the client's took)
        CHECK(eq.run("return eq.TakeCloseTopWindow()") == "true");
        CHECK_FALSE(host.close_top_window);
        CHECK(eq.run("return eq.TakeCloseTopWindow()") == "false");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("eq windows: found by name as a scan from the front finds them") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        app::EqHost host;
        host.in_game = host.ui_ready = true;
        EqTable eq(host);
        eq.api.begin_frame();
        // Every model by its name (the first of a name, should two share one), the 32 chat windows among them.
        lua_State* L = eq.L;
        REQUIRE(luaL_dostring(L, "local t = {} for _, s in ipairs(eq.DebugScreens()) do t[#t + 1] = {s.name, s.shown} end "
                                 "return t") == LUA_OK);
        std::vector<std::pair<std::string, bool>> screens;
        for (lua_Integer i = 1; lua_rawgeti(L, -1, i) == LUA_TTABLE; ++i) {
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            screens.emplace_back(lua_tostring(L, -2), lua_toboolean(L, -1) != 0);
            lua_pop(L, 3);
        }
        lua_settop(L, 0);
        REQUIRE(screens.size() > 100);
        std::set<std::string> seen;
        int chats = 0;
        for (const auto& [name, shown] : screens) {
            if (!seen.insert(name).second) continue;
            CAPTURE(name);
            CHECK(eq.api.shown(name) == shown);
            if (name.rfind("Chat", 0) == 0 && name.size() > 4 && std::isdigit(static_cast<unsigned char>(name[4]))) ++chats;
        }
        CHECK(chats == 32);
        // The names are matched exactly: case counts.
        CHECK(eq.api.shown("Chat1"));
        CHECK_FALSE(eq.api.shown("chat1"));
        CHECK_FALSE(eq.api.shown("CHAT1"));
        CHECK_FALSE(eq.api.shown("Chat1 "));
        CHECK_FALSE(eq.api.shown(""));
        CHECK(eq.run("return eq.WindowShown('Chat1')") == "true");
        CHECK(eq.run("return eq.WindowShown('chat1')") == "false");
    }
    ImGui::DestroyContext(ctx);
}
