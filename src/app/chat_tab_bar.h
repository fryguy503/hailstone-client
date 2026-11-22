#pragma once

struct lua_State;

namespace hailstone::app {

struct UiHost;

// Native controls embedded in the Modern HUD frame.
void register_modern_chat_tabs(lua_State* L, UiHost& host);
void position_detached_chat(UiHost& host, int source, int destination);

} // namespace hailstone::app
