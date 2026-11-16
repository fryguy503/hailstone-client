#pragma once

// What a plugin's code in the UI needs to call each `hs` function (docs/plugins.md "What plugin windows may do"). The
// pack's own code acts as the player and calls them all. A plugin's code (ui::lua::LuaUi::caller_tag, under its
// plugin::CallerScope) calls a function that acts in the game (clicks a stock window's buttons, moves items, casts,
// trains, sends a packet) only with the capability it names: refused, it does nothing and returns nil. Reading and
// drawing need none. Every function is listed (tests/plugin_ui_sandbox_test.cpp checks), and one that is not is
// taken to act (kCapActions).

#include <cstdint>
#include <optional>
#include <string_view>

struct lua_State;

namespace hailstone::app {

// The capabilities `name` ("ClickSlot", or "gamepad.set_mode" in a sub-table) needs from a plugin's code; 0 none;
// nullopt for a name not listed.
std::optional<std::uint32_t> ui_call_caps(std::string_view name);

// Over the hs table on the top of the stack (and its sub-tables of functions): each function that needs a capability
// becomes one that checks the caller first (ui_call_caps).
void gate_ui_calls(lua_State* L);

} // namespace hailstone::app
