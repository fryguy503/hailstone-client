#pragma once

struct lua_State;

namespace hailstone::app {

struct UiHost;
namespace chat_tabs { struct Tab; }

// Native controls embedded in the Modern HUD frame.
void register_modern_chat_tabs(lua_State* L, UiHost& host);
void position_detached_chat(UiHost& host, int source, int destination);
// A tab in the strip shows the unread dot: not the one up, unseen lines, and its flashing off (a flashing tab shows
// them in its highlight colour instead).
bool unread_dot(const chat_tabs::Tab& tab, bool selected);

} // namespace hailstone::app
