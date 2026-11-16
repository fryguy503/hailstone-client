#pragma once

#include <string_view>

#include "app/lua_ui_owner.h"

struct lua_State;
namespace hailstone::app {

// Called while the flat hs registration table is on top. Only UI-owned states can register callbacks.
void register_ui_hooks_api(lua_State* L, const UiCallbackContext& context);
// Replaces the guarded function on top with a stable dispatcher. The original closure stays its upvalue.
void wrap_ui_hook_method(lua_State* L, std::string_view method);

} // namespace hailstone::app
