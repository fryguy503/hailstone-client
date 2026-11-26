#pragma once

// Server coordinates vs. the client's world (zone file) coordinates.
//
// EQEmu sends positions as (x, y, z) with x and y swapped relative to the WLD
// zone geometry: live check on qeynos, all 141 spawns stood 3.75 units above
// a floor with the swap and only 24 without it. The client works in zone-file
// coordinates everywhere and converts at the packet boundary.
//
// Headings are 0..512 per turn. On the server, 0 faces +Y and 128 faces +X
// (zone/position.cpp CalculateHeadingAngleBetweenPositions); after the swap
// that is 0 facing +X and 128 facing +Y, i.e. counterclockwise from +X.

#include <cmath>
#include <optional>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace hailstone::game {

inline glm::vec3 server_to_world(const glm::vec3& p) { return {p.y, p.x, p.z}; }
inline glm::vec3 world_to_server(const glm::vec3& p) { return {p.y, p.x, p.z}; }

// Heading (0..512) <-> yaw in radians, counterclockwise from +X in world coordinates.
inline float heading_to_yaw(float heading) { return heading * glm::pi<float>() / 256.0f; }
inline float yaw_to_heading(float yaw) {
    float h = std::fmod(yaw * 256.0f / glm::pi<float>(), 512.0f);
    return h < 0.0f ? h + 512.0f : h;
}

// MQ's degrees (clockwise from north, any number of turns) as a heading 0..512; nullopt for NaN or an infinity, whose
// yaw would stay NaN for good (and a cast of a huge double to float is undefined).
inline std::optional<float> mq_degrees_to_heading(double degrees) {
    if (!std::isfinite(degrees)) return std::nullopt;
    const double turn = std::fmod(std::fmod(degrees, 360.0) + 360.0, 360.0);
    const double h = std::fmod((360.0 - turn) * 512.0 / 360.0, 512.0);
    return static_cast<float>(h);
}

} // namespace hailstone::game
