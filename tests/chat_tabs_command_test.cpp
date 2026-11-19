#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/chat_context_menu.h"
#include "app/chat_tabs.h"
#include "app/chat_tabs_command.h"
#include "app/lua_ui.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "scratch_dir.h"
#include "ui/dev_screens.h"

using namespace hailstone;
namespace tabs = app::chat_tabs;

TEST_CASE("Chat tabs command: navigation, moving and regrouping use the native layout") {
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ctx(ImGui::CreateContext(), ImGui::DestroyContext);
    test::ScratchDir dir("chat_tabs_command");
    GameDir game_dir(dir.path());
    net::GameConnection conn(game_dir);
    ui::ChatView view;
    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.game = &game_dir;
    host.conn = &conn;
    host.chat = &view;
    app::UiApi api(host);
    api.load_layout(dir.path() / "defaults.ini", dir.path() / "UI.ini", {});
    auto run = [&](std::string_view args) { app::run_chat_tabs_command(conn, args); };

    REQUIRE(tabs::active_tab() == 1);
    run("rename Main tab");
    run("renamewindow Main window");
    CHECK(tabs::tab(1).title == "Main tab");
    CHECK(tabs::group(1).title == "Main window");
    run("rename");
    REQUIRE(host.text_entry);
    CHECK(host.text_entry->text == "Main tab");
    host.text_entry.reset();
    run("close");
    const auto close_notice = conn.poll_chat();
    REQUIRE(close_notice);
    CHECK(close_notice->text == "The main chat tab cannot be closed.");
    run("new   Loot messages  ");
    const int loot = tabs::active_tab();
    REQUIRE(loot > 1);
    CHECK(tabs::tab(loot).title == "Loot messages");
    run("NEXT");
    CHECK(tabs::active_tab() == 1);
    run("prev");
    CHECK(tabs::active_tab() == loot);
    run("left");
    REQUIRE(tabs::group(1).tabs.size() == 2);
    CHECK(tabs::group(1).tabs.front() == loot);

    run("detach");
    const int detached = tabs::active_group();
    REQUIRE(detached > 1);
    CHECK(tabs::group_of(loot) == detached);
    run("status");
    const auto main_status = conn.poll_chat();
    const auto detached_status = conn.poll_chat();
    REQUIRE(main_status);
    REQUIRE(detached_status);
    CHECK(main_status->text == "1 Main window  [Main tab]");
    CHECK(detached_status->text == "* " + std::to_string(detached) + " Loot messages  [Loot messages]");
    run("move 1bad");
    CHECK(tabs::group_of(loot) == detached);
    run("move 1");
    CHECK(tabs::group_of(loot) == 1);
    CHECK(tabs::group(detached).tabs.empty());

    run("detach");
    const int target = tabs::active_group();
    REQUIRE(target > 1);
    run("combine");
    CHECK(tabs::group(1).tabs.empty());
    CHECK(tabs::group_of(1) == target);
    CHECK(tabs::group(target).tabs.size() == 2);
}

TEST_CASE("Chat tabs context menu: the original's order first, tab actions and stable window destinations") {
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ctx(ImGui::CreateContext(), ImGui::DestroyContext);
    test::ScratchDir dir("chat_tabs_context_menu");
    GameDir game_dir(dir.path());
    net::GameConnection conn(game_dir);
    ui::ChatView view;
    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.game = &game_dir;
    host.conn = &conn;
    host.chat = &view;
    app::UiApi api(host);
    api.load_layout(dir.path() / "defaults.ini", dir.path() / "UI.ini", {});

    const auto row = [](const std::vector<app::UiHost::MenuRow>& rows, std::string_view name)
        -> const app::UiHost::MenuRow& {
        const auto item = std::find_if(rows.begin(), rows.end(), [&](const auto& entry) { return entry.text == name; });
        REQUIRE(item != rows.end());
        return *item;
    };

    const auto main_menu = app::chat_context_rows(1);
    CHECK_FALSE(row(main_menu, "Close Tab").enabled);
    CHECK_FALSE(row(main_menu, "Detach Tab").enabled);
    CHECK_FALSE(row(main_menu, "Move Tab to Window").enabled);
    CHECK_FALSE(row(main_menu, "Combine Chat Windows").enabled);

    const int loot = tabs::new_tab(1, "Loot");
    const int target = tabs::new_window("Side");
    const int other = tabs::new_window("Side");
    REQUIRE(loot > 1);
    REQUIRE(target > 1);
    REQUIRE(other > target);
    REQUIRE(tabs::select(1, 1));

    const auto menu = app::chat_context_rows(loot);
    auto next_row = menu.begin();
    // The original's rows first (InitContextMenu @0x6513c0), then Hailstone's.
    for (std::string_view name : {"Window", "Scroll bar", "New Chat Window", "Rename Window", "Always Chat Here",
                                  "Reply to...", "Tell a friend...", "Tell a raidmember...", "Filters", "Channel",
                                  "Language", "Hit Modes", "New Tab", "Close Tab", "Rename Tab", "Move Tab Left",
                                  "Move Tab Right", "Move Tab to Window", "Detach Tab", "Clear Tab Text",
                                  "Highlight New Messages", "Tab Highlight Color", "Switch Tab", "Chat Windows",
                                  "Combine Chat Windows", "Close Window", "Font Size", "Use Tell Windows",
                                  "Timestamps"}) {
        CAPTURE(name);
        const auto match = std::find_if(next_row, menu.end(), [&](const auto& entry) { return entry.text == name; });
        REQUIRE(match != menu.end());
        next_row = match + 1;
    }

    const auto& color = row(menu, "Tab Highlight Color");
    CHECK_FALSE(row(menu, "Reset Tab Highlight Color").enabled);
    REQUIRE(color.act);
    color.act();
    REQUIRE(host.color_pick);
    CHECK(host.color_pick->rgb == tabs::kDefaultHighlightColor);
    const auto old_color_callback = std::move(host.color_pick->done);
    host.color_pick.reset();
    old_color_callback(0xff123456u);
    CHECK(tabs::tab(loot).highlight_color == 0x123456u);
    CHECK_FALSE(tabs::tab(1).highlight_color);
    CHECK(tabs::group(1).selected == 1);
    const auto colored_menu = app::chat_context_rows(loot);
    const auto& reset_color = row(colored_menu, "Reset Tab Highlight Color");
    CHECK(&reset_color == &row(colored_menu, "Tab Highlight Color") + 1);
    REQUIRE(reset_color.enabled);
    REQUIRE(reset_color.act);
    reset_color.act();
    CHECK_FALSE(tabs::tab(loot).highlight_color);

    const auto& highlight = row(menu, "Highlight New Messages");
    REQUIRE(highlight.act);
    CHECK(highlight.check);
    highlight.act();
    CHECK_FALSE(tabs::tab(loot).highlight);
    CHECK(tabs::tab(1).highlight);

    const auto& navigate = row(menu, "Switch Tab");
    REQUIRE(navigate.enabled);
    REQUIRE(navigate.submenu);
    const auto tab_choices = navigate.submenu();
    const auto& next = row(tab_choices, "Next Tab");
    const auto& previous = row(tab_choices, "Previous Tab");
    const auto& loot_choice = row(tab_choices, "Loot");
    REQUIRE(next.act);
    REQUIRE(previous.act);
    REQUIRE(loot_choice.act);
    next.act();
    CHECK(tabs::group(1).selected == loot);
    previous.act();
    CHECK(tabs::group(1).selected == 1);
    loot_choice.act();
    CHECK(tabs::group(1).selected == loot);

    const auto& move = row(menu, "Move Tab to Window");
    REQUIRE(move.enabled);
    REQUIRE(move.submenu);
    const auto destinations = move.submenu();
    REQUIRE(destinations.size() == 2);
    CHECK(destinations[0].text == "Side [" + std::to_string(target) + "]");
    CHECK(destinations[1].text == "Side [" + std::to_string(other) + "]");
    REQUIRE(destinations[0].act);
    destinations[0].act();
    CHECK(tabs::group_of(loot) == target);

    const auto moved_menu = app::chat_context_rows(loot);
    const auto& combine = row(moved_menu, "Combine Chat Windows");
    REQUIRE(combine.enabled);
    REQUIRE(combine.act);
    combine.act();
    CHECK(tabs::group_of(1) == target);
    CHECK(tabs::group_of(loot) == target);
    CHECK(tabs::group(target).tabs.size() == 4);
    CHECK(tabs::group(1).tabs.empty());
    CHECK(tabs::group(other).tabs.empty());

    REQUIRE(tabs::close(loot));
    REQUIRE(tabs::new_tab(target, "Replacement") == loot);
    old_color_callback(0xffabcdefu);
    CHECK_FALSE(tabs::tab(loot).highlight_color);
}
