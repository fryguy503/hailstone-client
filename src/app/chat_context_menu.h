#pragma once

#include "app/lua_ui.h"

namespace hailstone::app {

// Chat context actions shared by the Modern stock chat and hs.ChatMenu. The logical tab slot is one-based.
std::vector<UiHost::MenuRow> chat_context_rows(int tab);

} // namespace hailstone::app
