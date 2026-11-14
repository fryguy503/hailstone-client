#pragma once

// Automated live tests must never enter a different world when their saved world is absent.

#include <optional>
#include <span>
#include <string_view>

#include "core/server_profile.h"
#include "net/login.h"

namespace hailstone::app {

inline std::optional<std::uint32_t> benchmark_world_id(std::span<const net::ServerInfo> worlds,
                                                       std::string_view requested) {
    if (requested.empty()) return std::nullopt;
    const net::ServerInfo* match = nullptr;
    for (const auto& world : worlds) {
        if (world.name != requested && saved_character_key_part(world.name) != requested) continue;
        if (match) return std::nullopt;
        match = &world;
    }
    if (!match || (match->status & 1) != 0) return std::nullopt;
    for (const auto& world : worlds)
        if (&world != match && world.id == match->id) return std::nullopt;
    return match->id;
}

} // namespace hailstone::app
