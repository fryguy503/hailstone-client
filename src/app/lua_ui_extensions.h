#pragma once

#include "app/lua_ui_owner.h"

struct lua_State;

namespace hailstone::app {
// The hs table is on top; callbacks belong to this UI VM and their registering source.
void register_ui_extensions(lua_State* L, const UiCallbackContext& context);
}
