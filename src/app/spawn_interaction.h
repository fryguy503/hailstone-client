#pragma once

#include <cstdint>

namespace hailstone::net {
class GameConnection;
}

namespace hailstone::app {

// Run a spawn's right-click action. The physical mouse can also consider ordinary NPCs.
bool right_click_spawn(net::GameConnection& conn, std::uint32_t id, bool consider_npc);

} // namespace hailstone::app
