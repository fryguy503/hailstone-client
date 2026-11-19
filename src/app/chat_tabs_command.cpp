#include "app/chat_tabs_command.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "app/chat_tabs.h"
#include "core/ascii_case.h"
#include "game/slash_commands.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

constexpr std::uint32_t kCommandTextColor = 15;

std::string_view trim_command_args(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

int window_id(std::string_view text) {
    if (text.empty()) return 0;
    int id = 0;
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, id);
    return result.ec == std::errc{} && result.ptr == end && id > 0 && id <= chat_tabs::kMaxSlots ? id : 0;
}

void list_chat_groups(net::GameConnection& conn, int active_group) {
    for (int slot = 1; slot <= chat_tabs::kMaxSlots; ++slot) {
        const auto group = chat_tabs::group(slot);
        if (group.tabs.empty()) continue;
        std::string line = std::format("{}{} {}", slot == active_group ? "* " : "", slot, group.title);
        for (int member : group.tabs) {
            const auto tab = chat_tabs::tab(member);
            line += member == group.selected ? std::format("  [{}]", tab.title) : std::format("  {}", tab.title);
        }
        conn.show_chat(std::move(line), kCommandTextColor);
    }
}

} // namespace

void run_chat_tabs_command(net::GameConnection& conn, std::string_view args) {
    std::string action = game::next_cmd_arg(args);
    std::ranges::transform(action, action.begin(), ascii_lower);
    args = trim_command_args(args);

    if (action.empty() || action == "help") {
        conn.show_chat("/chattabs status, next, prev, new [title], close, clear", kCommandTextColor);
        conn.show_chat("/chattabs rename [title], renamewindow [title], left, right", kCommandTextColor);
        conn.show_chat("/chattabs detach, move <window ID>, combine", kCommandTextColor);
        return;
    }

    const int active = chat_tabs::active_tab();
    const int host = chat_tabs::active_group();
    if (active == 0 || host == 0) {
        conn.show_chat("No active chat tab.", kCommandTextColor);
        return;
    }

    std::string_view error;
    if (action == "status" || action == "list") {
        list_chat_groups(conn, host);
    } else if (action == "next" || action == "prev") {
        const auto tabs = chat_tabs::group(host).tabs;
        const auto selected = std::ranges::find(tabs, active);
        if (tabs.size() > 1 && selected != tabs.end()) {
            const auto index = static_cast<std::size_t>(selected - tabs.begin());
            const auto offset = action == "next" ? std::size_t{1} : tabs.size() - 1;
            chat_tabs::select(host, tabs[(index + offset) % tabs.size()]);
        }
    } else if (action == "new") {
        if (chat_tabs::new_tab(host, args) == 0) error = "No more chat tabs can be opened.";
    } else if (action == "close") {
        if (!chat_tabs::close(active)) error = "The main chat tab cannot be closed.";
    } else if (action == "clear") {
        chat_tabs::clear(active);
    } else if (action == "rename") {
        const bool renamed = args.empty() ? chat_tabs::prompt_rename_tab(active) : chat_tabs::rename_tab(active, args);
        if (!renamed) error = "Unable to rename the chat tab.";
    } else if (action == "renamewindow") {
        const bool renamed = args.empty() ? chat_tabs::prompt_rename_group(host) : chat_tabs::rename_group(host, args);
        if (!renamed) error = "Unable to rename the chat window.";
    } else if (action == "left" || action == "right") {
        chat_tabs::reorder(active, action == "left" ? -1 : 1);
    } else if (action == "detach") {
        if (chat_tabs::detach(active) == 0) error = "The tab already has its own chat window.";
    } else if (action == "move") {
        if (!chat_tabs::move(active, window_id(args))) error = "Use /chattabs status to find another chat window ID.";
    } else if (action == "combine") {
        chat_tabs::combine(host);
    } else {
        error = "Use /chattabs help for chat tab commands.";
    }
    if (!error.empty()) conn.show_chat(std::string(error), kCommandTextColor);
}

} // namespace hailstone::app
