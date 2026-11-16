#pragma once

#include <string_view>
#include "game/nameplate_canvas.h"

namespace hailstone::game { struct Spawn; }
namespace hailstone::net { class GameConnection; }
namespace hailstone::app {
void fill_nameplate_canvas_candidate(game::NameplateCanvasCandidate& candidate,
                                    const net::GameConnection& connection, const game::Spawn& spawn,
                                    std::string_view caption, double distance);
} // namespace hailstone::app
