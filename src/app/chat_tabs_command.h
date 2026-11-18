#pragma once

#include <string_view>

namespace hailstone::net { class GameConnection; }

namespace hailstone::app {

void run_chat_tabs_command(net::GameConnection& conn, std::string_view args);

} // namespace hailstone::app
