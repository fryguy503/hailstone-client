#pragma once

// UiValue (plugin/ui_bridge.h) to and from Lua, for the UI's hs.Plugin (app/lua_ui_plugins.cpp) and the scripts'
// hailstone module (mq/lua_hailstone.cpp).

#include <cstddef>
#include <optional>
#include <string>

#include "plugin/ui_bridge.h"

struct lua_State;

namespace hailstone::plugin {

// How deep tables go both ways; a deeper one is nil.
inline constexpr int kMaxValueDepth = 16;
// How big a value may be both ways: its values (each table, array and plain value one) and the bytes of its strings
// and keys. The depth alone does not bound it: t = {} for i = 1, 8 do t[i] = t end is 8^16 values deep down.
inline constexpr std::size_t kMaxValueNodes = 100000;
inline constexpr std::size_t kMaxValueBytes = 16u << 20;

// A Lua value as a UiValue: a table with only 1..n keys is an array, any other a table of string keys (numbers as
// text; other keys left out). Functions, userdata and tables deeper than kMaxValueDepth are nil. nullopt (`why` says
// which) for a table that holds itself (directly or further down) or a value past kMaxValueNodes / kMaxValueBytes.
std::optional<UiValue> to_value_checked(lua_State* L, int idx, std::string* why = nullptr);
// to_value_checked, with nil for a value it refuses (and a line in the log). `depth`: where idx's table sits already.
UiValue to_value(lua_State* L, int idx, int depth = 0);
// (Neither conversion to a UiValue calls Lua that could raise: Lua is built as C, and a raise would skip their
// vectors' and strings' destructors.)
// Pushes the value (arrays 1..n; a table's whole-number keys, "12", as Lua integers, so to_value's round trip
// keeps them). Tables deeper than kMaxValueDepth are nil, and so is the whole value when it is past kMaxValueNodes /
// kMaxValueBytes. Never raises a Lua error for the stack (lua_checkstack).
void push_value(lua_State* L, const UiValue& v, int depth = 0);

} // namespace hailstone::plugin
