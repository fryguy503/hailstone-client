#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "app/chat_context_menu.h"
#include "app/chat_tabs.h"
#include "app/lua_ui.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "scratch_dir.h"
#include "ui/dev_screens.h"
#include "ui/ui_ini.h"

using namespace hailstone;

namespace {

const app::UiHost::MenuRow& row_named(const std::vector<app::UiHost::MenuRow>& rows, std::string_view name) {
    CAPTURE(name);
    const auto row = std::find_if(rows.begin(), rows.end(), [name](const auto& item) { return item.text == name; });
    REQUIRE(row != rows.end());
    return *row;
}

} // namespace

TEST_CASE("Native chat filters preserve nested categories and route only the selected scope") {
    const test::ScratchDir scratch("native_chat_filters");
    const auto dir = scratch.path();
    std::filesystem::create_directories(dir / "game");
    std::ofstream(dir / "game/eqstr_us.txt") << "EQST0002\n6645 Filters\n6638 Chat Channel\n6640 Spells\n"
                                             << "3135 will go to this window.\n3136 will go to the main window.\n";
    GameDir game(dir / "game");
    net::GameConnection conn(game);
    ui::ChatView view;
    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.game = &game;
    host.conn = &conn;
    host.chat = &view;
    app::UiApi api(host);
    api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
    const int window = app::chat_tabs::new_window("Combat");
    const int tab = app::chat_tabs::group(window).selected;
    REQUIRE(tab > 1);

    const auto root = app::chat_context_rows(tab);
    const auto& filters_row = row_named(root, "Filters");
    REQUIRE(filters_row.submenu);
    const auto filters = filters_row.submenu();
    CHECK_FALSE(row_named(filters, "Chat Channel").enabled);
    CHECK(row_named(filters, "Spells").enabled);
    const auto& extended = row_named(filters, "Extended (server)");
    REQUIRE(extended.submenu);
    const auto categories = extended.submenu();
    CHECK(std::none_of(filters.begin(), filters.end(), [](const auto& item) { return item.text == "Healing"; }));
    CHECK(row_named(categories, "Random Rolls").enabled);
    const auto& healing_row = row_named(categories, "Healing");
    const auto& damage_row = row_named(categories, "Direct Damage");
    REQUIRE(healing_row.submenu);
    REQUIRE(damage_row.submenu);

    const auto healing = healing_row.submenu();
    const auto& received = row_named(healing, "Healing received");
    CHECK_FALSE(received.check);
    REQUIRE(received.act);
    received.act();
    CHECK(row_named(healing_row.submenu(), "Healing received").check);
    const auto damage = damage_row.submenu();
    const auto& show = row_named(damage, "ALL");
    const auto& hide = row_named(damage, "Hide All");
    REQUIRE(show.act);
    REQUIRE(hide.act);
    show.act();
    const auto* ini = app::chat_ui_ini();
    REQUIRE(ini);
    const auto target = std::to_string(tab - 1);
    CHECK(ini->get("ChatManager", "ChannelMap101") == target);
    CHECK(ini->get("ChatManager", "ChannelMap80") == target);
    CHECK(ini->get("ChatManager", "ChannelMap97") == target);
    hide.act();
    CHECK(ini->get("ChatManager", "ChannelMap101") == target);
    CHECK(ini->get("ChatManager", "ChannelMap80") == "0");
    CHECK(ini->get("ChatManager", "ChannelMap97") == "0");
}

TEST_CASE("Native chat menu: the original's rows first, in InitContextMenu's order, then Hailstone's tab rows") {
    const test::ScratchDir scratch("native_chat_menu_order");
    const auto dir = scratch.path();
    std::filesystem::create_directories(dir / "game");
    std::ofstream(dir / "game/eqstr_us.txt") << "EQST0002\n3141 Enter the new name:\n";
    GameDir game(dir / "game");
    net::GameConnection conn(game);
    ui::ChatView view;
    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.game = &game;
    host.conn = &conn;
    host.chat = &view;
    app::UiApi api(host);
    api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});

    const auto rows = app::chat_context_rows(1);
    // CChatWindowManager::InitContextMenu @0x6513c0: Window, Scrollbar | New Chat Window, Rename Window, Always Chat
    // Here | the three tell menus | Filters, Channel, Language, Hit Modes.
    const std::vector<std::string> original = {"Window", "Scroll bar", "", "New Chat Window", "Rename Window",
        "Always Chat Here", "", "Reply to...", "Tell a friend...", "Tell a raidmember...", "", "Filters", "Channel",
        "Language", "Hit Modes"};
    REQUIRE(rows.size() > original.size() + 1);
    for (std::size_t i = 0; i < original.size(); ++i) {
        CAPTURE(i);
        if (original[i].empty()) CHECK(rows[i].separator);
        else CHECK(rows[i].text == original[i]);
    }
    CHECK(rows[original.size()].separator);
    CHECK(rows[original.size() + 1].text == "New Tab");

    // Rename Window: the original's prompt and its 15 characters.
    const auto& rename = row_named(rows, "Rename Window");
    REQUIRE(rename.act);
    rename.act();
    REQUIRE(host.text_entry);
    CHECK(host.text_entry->prompt == "Enter the new name:");
    CHECK(host.text_entry->max_chars == 15);
}
