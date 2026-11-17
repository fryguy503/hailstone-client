// The hs API's arithmetic taken from the original (app/lua_ui_values.h, ui/ui_textures.h), the EQ menu's rows
// (app/lua_ui_menu.h), and the window functions' lookups: windows by name, lists once a frame, hs.WindowShown.
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

#include "app/lua_ui.h"
#include "app/lua_ui_caps.h"
#include "app/lua_ui_menu.h"
#include "app/lua_ui_values.h"
#include "core/game_dir.h"
#include "game/keymap.h"
#include "ui/dev_screens.h"
#include "ui/ui_textures.h"
#include "scratch_dir.h"

using namespace hailstone;

TEST_CASE("ui_percent: cur / (max * 0.01f) truncated") {
    CHECK(app::ui_percent(59, 59) == 100);
    CHECK(app::ui_percent(0, 59) == 0);
    CHECK(app::ui_percent(5, 0) == 0);
    CHECK(app::ui_percent(1, 3) == 33);
    CHECK(app::ui_percent(2, 3) == 66);
    CHECK(app::ui_percent(99, 100) == 99);
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
                                                        "Mercenaries", "-", "Options", "Server add-ons", "Lua Scripts", "Web Browser",
                                                        "Report a Bug", "Camp & Quit"});
    CHECK(menu.row_texts(1) == std::vector<std::string>{"Actions", "Alarm"});
    CHECK(menu.row_texts(3) == std::vector<std::string>{"Auras"});
    CHECK(menu.row_texts(4) == std::vector<std::string>{"Quest Journal"});

    // Without the aura window (the gamepad pack) its row goes; the actions, options, skills ... keys always work.
    drawn.erase("AuraWindow");
    app::CascadeMenu pad;
    pad.load_text(text, 0xffffffffu, 0, offered);
    CHECK(pad.row_texts(0) == std::vector<std::string>{"Actions >", "Quests >", "-", "Mail", "Mercenaries", "-",
                                                       "Options", "Server add-ons", "Lua Scripts", "Web Browser", "Report a Bug",
                                                       "Camp & Quit"});
    // Hailstone's Lua Scripts row returns its own number (the Scripts window).
    {
        auto rows = pad.row_texts(0);
        pad.toggle({0, 0, 50, 20}, 1920, 1080);
        const int at = static_cast<int>(std::find(rows.begin(), rows.end(), "Lua Scripts") - rows.begin());
        CHECK(pad.click(0, at) == app::kMenuScripts);
    }
    {
        const auto rows = pad.row_texts(0);
        pad.toggle({0, 0, 50, 20}, 1920, 1080);
        const int at = static_cast<int>(std::find(rows.begin(), rows.end(), "Server add-ons") - rows.begin());
        CHECK(pad.command(0, at) == app::kMenuAddonPolicy);
        CHECK(pad.open());
        CHECK_FALSE(pad.command(-1, at));
        CHECK_FALSE(pad.command(0, -1));
        CHECK(pad.click(0, at) == app::kMenuAddonPolicy);
    }

    // The plugins' entries: a "Plugins" submenu before Hailstone's rows (Lua Scripts, Web Browser, Report a Bug), each
    // row its entry's number; none, none.
    const int menus = pad.menu_count();
    pad.set_plugin_entries({"Ext Window", "Ext Two"});
    auto top = pad.row_texts(0);
    CHECK(top == std::vector<std::string>{"Actions >", "Quests >", "-", "Mail", "Mercenaries", "-", "Options",
                                          "Plugins >", "Server add-ons", "Lua Scripts", "Web Browser", "Report a Bug", "Camp & Quit"});
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
                                                       "Options", "Server add-ons", "Lua Scripts", "Web Browser", "Report a Bug",
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
                                                        "Server add-ons", "Web Browser", "-", "Help", "Report a Bug", "Camp & Quit"});
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
          std::vector<std::string>{"Server Mail", "Server add-ons", "Lua Scripts", "Web Browser", "Report a Bug", "Camp & Quit"});
}

namespace {

std::string run_ui(lua_State* L, const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

// The hs table over an UiApi, as the UI's script host registers it.
struct UiTable {
    app::UiApi api;
    lua_State* L;
    explicit UiTable(app::UiHost& host) : api(host), L(luaL_newstate()) {
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
    }
    ~UiTable() { lua_close(L); }
    UiTable(const UiTable&) = delete;
    UiTable& operator=(const UiTable&) = delete;
    std::string run(const std::string& code) { return run_ui(L, code); }
};

ui::ServerEntry world(const char* name) {
    ui::ServerEntry e;
    e.name = name;
    e.status = "Up";
    e.can_join = true;
    return e;
}

} // namespace

TEST_CASE("Server add-ons: visibility starts shown and is saved with each character's UI layout") {
    std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
    ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
    test::ScratchDir scratch("addon_policy_visibility");
    const auto defaults = scratch.path() / "defaults.ini";
    const auto first = scratch.path() / "first_server_character.ini";
    const auto second = scratch.path() / "second_server_character.ini";
    app::UiHost host;
    host.in_game = host.ui_ready = true;
    ui::AddonPolicyView policy;
    policy.enabled = true;
    policy.required = {"maker/helper"};
    host.addon_policy = &policy;
    {
        UiTable hs(host);
        hs.api.load_layout(defaults, first, {});
        CHECK(hs.api.shown("AddonPolicy"));
        CHECK(hs.run("return hs.WindowShown('AddonPolicy')") == "true");
        hs.api.begin_frame();
        hs.api.show("AddonPolicy", false);
        hs.api.begin_frame();
        hs.api.save_layout();

        // Failures and fresh rules change recovery data, never the player's visibility choice.
        policy.degraded = true;
        policy.serial = 1;
        policy.message = "Required add-on stopped.";
        hs.api.begin_frame();
        CHECK_FALSE(hs.api.shown("AddonPolicy"));
        policy.degraded = false;
        ++policy.serial;
        hs.api.begin_frame();
        CHECK_FALSE(hs.api.shown("AddonPolicy"));

        hs.api.load_layout(defaults, second, {});
        CHECK(hs.api.shown("AddonPolicy")); // another character starts with its own default
        hs.api.load_layout(defaults, first, {});
        CHECK_FALSE(hs.api.shown("AddonPolicy"));
        hs.api.begin_frame();
        hs.api.toggle("AddonPolicy");
        hs.api.begin_frame();
        hs.api.save_layout();
    }
    {
        UiTable hs(host);
        hs.api.load_layout(defaults, first, {});
        CHECK(hs.api.shown("AddonPolicy")); // the reopened state also survives a new UI model
    }
}

TEST_CASE("Fellowship window: the default shortcut and EQ menu resolve to its toggle") {
    std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
    {
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        UiTable hs(host);
        CHECK(hs.run("return hs.ReplaceWindow('FellowshipWnd')") == "false");
        REQUIRE(hs.api.replaces("FellowshipWnd"));
        CHECK_FALSE(hs.api.shown("FellowshipWnd"));

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
        hs.api.toggle(screen);
        CHECK(hs.api.shown("FellowshipWnd"));

        events.clear();
        keys.key_up(shortcut.code, events);
        REQUIRE(events.size() == 1);
        CHECK_FALSE(events.front().down);
        events.clear();
        keys.key_down(shortcut, events);
        REQUIRE(events.size() == 1);
        screen = app::toggle_command_window(events.front().command);
        REQUIRE(screen != nullptr);
        hs.api.toggle(screen);
        CHECK_FALSE(hs.api.shown("FellowshipWnd"));

        const auto offered = [&](int command) {
            return app::menu_command_offered(command, [&](std::string_view name) { return hs.api.replaces(name); });
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
        hs.api.toggle(screen);
        CHECK(hs.api.shown("FellowshipWnd"));
    }
}

TEST_CASE("hs UI animation: registered read-only binding returns nil without an artwork provider") {
    ImGuiContext* context = ImGui::CreateContext();
    {
        app::UiHost host;
        UiTable hs(host);
        CHECK(hs.run("return type(hs.UiAnimation)") == "function");
        CHECK(hs.run("return hs.UiAnimation('A_EQ')") == "nil");
        CHECK(hs.run("return hs.UiAnimation('../outside.tga')") == "nil");
        CHECK(hs.run("return select('#',hs.UiAnimation('MissingBadge'))") == "1");
        CHECK(app::ui_call_caps("UiAnimation") == std::optional<std::uint32_t>{0});
    }
    ImGui::DestroyContext(context);
}

TEST_CASE("hs UI image: read-only public binding preserves nil results and structural discovery") {
    ImGuiContext* context = ImGui::CreateContext();
    {
        app::UiHost host;
        UiTable hs(host);
        CHECK(hs.run("return hs.UI.assets.Image == hs.UiImage") == "true");
        CHECK(hs.run("return hs.UI.Supports('assets', 'Image')") == "true");
        CHECK(hs.run("return hs.UI.Describe('assets', 'Image').requiredCaps") == "0");
        CHECK(hs.run("return hs.UI.assets.Image('images/missing.png')") == "nil");
        CHECK(hs.run("return select('#', hs.UI.assets.Image('images/missing.png'))") == "1");
        CHECK(hs.run("return hs.UI.assets.Image('../outside.png')") == "nil");
        CHECK(hs.run("return hs.UI.assets.Image('images/bad' .. string.char(0) .. '.png')") == "nil");
        CHECK(hs.run("return hs.UI.assets.Image('metadata.xml')") == "nil");
        CHECK(app::ui_call_caps("UiImage") == std::optional<std::uint32_t>{0});
    }
    ImGui::DestroyContext(context);
}

TEST_CASE("UI image lookup: safe names use indexed asset layers and cannot name an outside link") {
    namespace fs = std::filesystem;
    test::ScratchDir scratch("ui_image_layers");
    const auto base = scratch.path() / "base", overlay = scratch.path() / "overlay";
    fs::create_directories(base / "images");
    fs::create_directories(overlay / "images");
    std::ofstream(base / "images/atlas.png") << "base fixture";
    std::ofstream(overlay / "images/atlas.png") << "overlay fixture";
    const auto outside = scratch.path() / "outside.png";
    std::ofstream(outside) << "outside fixture";
    std::error_code ec;
    fs::create_symlink(outside, base / "images/escape.png", ec);
    GameDir game(base, overlay);
    const auto path = ui::ui_image_path("Images\\Atlas.PNG");
    REQUIRE(path);
    const auto selected = game.find(*path);
    REQUIRE(selected);
    CHECK(*selected == overlay / "images/atlas.png");
    game.set_overlay(std::nullopt);
    CHECK(game.find(*path) == base / "images/atlas.png");
    if (ec) { MESSAGE("File link creation is unavailable on this host"); return; }
    const auto escape = ui::ui_image_path("images/escape.png");
    REQUIRE(escape);
    CHECK_FALSE(game.find(*escape));
}

TEST_CASE("hs lists: built once a frame, again after a call that can change them") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        app::UiHost host;
        ui::ServerSelectView view;
        view.servers = {world("Alpha"), world("Beta")};
        host.server_select = &view;
        UiTable hs(host);
        const std::string list = "'serverselect', 'ServerList'";
        hs.api.begin_frame();
        CHECK(hs.run("return (hs.ListInfo(" + list + "))") == "2");
        CHECK(hs.run("return hs.ListRow(" + list + ", 1)[1]") == "Beta");
        // The worlds change behind the window's back: this frame's list is kept, by every reader.
        view.servers.push_back(world("Gamma"));
        CHECK(hs.run("return (hs.ListInfo(" + list + "))") == "2");
        CHECK(hs.run("return hs.ListRow(" + list + ", 2)") == "nil");
        CHECK(hs.run("return #hs.WindowList(" + list + ")") == "2");
        // A read of something else keeps it too.
        CHECK(hs.run("return hs.ReplaceWindow('serverselect')") == "true");
        CHECK(hs.run("return (hs.ListInfo(" + list + "))") == "2");
        // A click is a change: the list is built again in the same frame, with the click's selection.
        CHECK(hs.run("return hs.ListClick(" + list + ", 1, 'left')") == "true");
        CHECK(hs.run("return (hs.ListInfo(" + list + "))") == "3");
        CHECK(hs.run("return (select(3, hs.ListInfo(" + list + ")))") == "1");
        CHECK(hs.run("return hs.ListRow(" + list + ", 2)[1]") == "Gamma");
        // So is a header click (a sort), whether or not the window takes it, and any call that is not a read.
        view.servers.push_back(world("Delta"));
        CHECK(hs.run("return (hs.ListInfo(" + list + "))") == "3");
        hs.run("hs.ListHeaderClick(" + list + ", 0)");
        CHECK(hs.run("return (hs.ListInfo(" + list + "))") == "4");
        view.servers.push_back(world("Epsilon"));
        hs.run("hs.ClickChild('serverselect', 'NoSuchButton')");
        CHECK(hs.run("return #hs.WindowList(" + list + ")") == "5");
        // UiApi's own changes too.
        view.servers.push_back(world("Zeta"));
        hs.api.click_list("serverselect", "ServerList", 0, 0);
        CHECK(hs.run("return (hs.ListInfo(" + list + "))") == "6");
        CHECK(hs.run("return (select(3, hs.ListInfo(" + list + ")))") == "0");
        // A new frame builds it again.
        view.servers.pop_back();
        CHECK(hs.run("return (hs.ListInfo(" + list + "))") == "6");
        hs.api.begin_frame();
        CHECK(hs.run("return (hs.ListInfo(" + list + "))") == "5");
        // A window with no such list, and no such window.
        CHECK(hs.run("return hs.ListInfo('serverselect', 'Nope')") == "nil");
        CHECK(hs.run("return hs.ListInfo('NoSuchWnd', 'ServerList')") == "nil");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("hs.WindowShown: hs.ReplaceWindow's shown, the window counted as replaced, no place") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        app::UiHost host;
        UiTable hs(host);
        hs.api.begin_frame();
        // Hidden: the server select shows only with a world list.
        CHECK(hs.run("return hs.WindowShown('serverselect')") == "false");
        CHECK(hs.run("return (hs.ReplaceWindow('serverselect'))") == "false");
        ui::ServerSelectView view;
        host.server_select = &view;
        CHECK(hs.run("return hs.WindowShown('serverselect')") == "true");
        CHECK(hs.run("return (hs.ReplaceWindow('serverselect'))") == "true");
        // Not in the world: an in-game window is not shown, though its model is.
        CHECK(hs.run("return hs.WindowShown('InventoryWindow')") == "false");
        CHECK(hs.run("return (hs.ReplaceWindow('InventoryWindow'))") == "false");
        CHECK(hs.run("return hs.WindowShown('NoSuchWnd')") == "false");
        CHECK(hs.run("return hs.WindowShown()").rfind("error:", 0) == 0);

        // replaced_at: the C++ window stays off while the script asks every frame, as with hs.ReplaceWindow.
        CHECK_FALSE(hs.api.replaces("RaidWindow"));
        hs.run("hs.WindowShown('RaidWindow')");
        CHECK(hs.api.replaces("RaidWindow"));
        for (int i = 0; i < 2; ++i) hs.api.begin_frame();
        CHECK(hs.api.replaces("RaidWindow"));
        hs.api.begin_frame();
        CHECK_FALSE(hs.api.replaces("RaidWindow"));
        hs.run("hs.WindowShown('RaidWindow')");
        CHECK(hs.api.replaces("RaidWindow"));
        // The debug list counts it as replaced too.
        CHECK(hs.run("for _, s in ipairs(hs.DebugScreens()) do if s.name == 'RaidWindow' then return s.replaced end end") ==
              "true");

        // The window's move (the server select centred when it first shows) is left for hs.ReplaceWindow to take.
        host.server_select = nullptr;
        hs.run("hs.WindowShown('serverselect')");
        ImGui::GetIO().DisplaySize = ImVec2(1040, 680);
        host.server_select = &view;
        CHECK(hs.run("return hs.WindowShown('serverselect')") == "true");
        CHECK(hs.run("local s, x, y, w, h, sized, place = hs.ReplaceWindow('serverselect') return x .. ',' .. y .. ',' .. "
                     "tostring(place)") == "200,100,true");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("hs.TakeCloseTopWindow: the close-top-window key's press, taken once") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        app::UiHost host;
        UiTable hs(host);
        CHECK(hs.run("return hs.TakeCloseTopWindow()") == "false");
        host.close_top_window = true; // (ClientApp::key_command: CLOSE_TOP_WINDOW no window of the client's took)
        CHECK(hs.run("return hs.TakeCloseTopWindow()") == "true");
        CHECK_FALSE(host.close_top_window);
        CHECK(hs.run("return hs.TakeCloseTopWindow()") == "false");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("hs windows: found by name as a scan from the front finds them") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        UiTable hs(host);
        hs.api.begin_frame();
        // Every model by its name (the first of a name, should two share one), the 32 chat windows among them.
        lua_State* L = hs.L;
        REQUIRE(luaL_dostring(L, "local t = {} for _, s in ipairs(hs.DebugScreens()) do t[#t + 1] = {s.name, s.shown} end "
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
            CHECK(hs.api.shown(name) == shown);
            if (name.rfind("Chat", 0) == 0 && name.size() > 4 && std::isdigit(static_cast<unsigned char>(name[4]))) ++chats;
        }
        CHECK(chats == 32);
        // The names are matched exactly: case counts.
        CHECK(hs.api.shown("Chat1"));
        CHECK_FALSE(hs.api.shown("chat1"));
        CHECK_FALSE(hs.api.shown("CHAT1"));
        CHECK_FALSE(hs.api.shown("Chat1 "));
        CHECK_FALSE(hs.api.shown(""));
        CHECK(hs.run("return hs.WindowShown('Chat1')") == "true");
        CHECK(hs.run("return hs.WindowShown('chat1')") == "false");
    }
    ImGui::DestroyContext(ctx);
}
