#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace hailstone::app::chat_tabs {

inline constexpr int kMaxSlots = 32;
inline constexpr std::uint32_t kDefaultHighlightColor = 0xff0000;

// Groups are visible frames. Tabs keep their identity, filters, and lines independently of those frames.
// Group and tab slots are one-based, with zero denoting absence. Views remain valid until the next mutation.
struct Group {
    int slot = 0;
    std::string_view title;
    std::span<const int> tabs;
    int selected = 0;
    bool main = false;
    bool tells = false;
    bool select = false;
};

struct Tab {
    int slot = 0;
    std::uint32_t serial = 0;
    std::string_view title;
    bool tell = false;
    bool unread = false;
    bool highlight = true;
    std::optional<std::uint32_t> highlight_color;
    std::uint64_t tells_in = 0;
    bool scrollbar = true;
};

Group group(int host);
Tab tab(int slot);
int group_of(int tab);
int active_group();
int active_tab();
void consume_selection(int host);

int new_window(std::string_view title = {});
int new_tab(int host, std::string_view title = {});
bool select(int host, int tab);
bool close(int tab);
bool close_group(int host);
int detach(int tab);
bool move(int tab, int host);
bool reorder(int tab, int delta);
int combine(int host);
bool rename_group(int host, std::string_view title);
bool rename_tab(int tab, std::string_view title);
bool prompt_rename_group(int host);
bool prompt_rename_tab(int tab);
bool clear(int tab);
bool set_highlight(int tab, bool on);
bool set_color(int tab, std::optional<std::uint32_t> rgb);
bool set_scrollbar(int tab, bool on);
bool highlight_visible(int tab, double now);

} // namespace hailstone::app::chat_tabs
