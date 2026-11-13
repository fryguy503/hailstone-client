#pragma once

// Testing aid (--move-test): drives the player after zone-in and logs what
// the movement does, to compare with the original's numbers:
//   speed   run forward 2 s, walk 4 s back: distance per second (faces the
//           longest run the player's own collision sphere can make, doors included)
//   jump    a standing jump, a press during its cooldown, a running jump: height and air time
//   fall:H  lift the player H units and let go, twice (the first fall after
//           zone-in does no damage): fall speed, damage reported, HP
//   swim    walk to the nearest water, swim down, up and forward: depth and speed
//   walk:D[:S]  run S seconds (default 3) facing yaw D degrees (0 = +X, 90 = +Y), logging the feet
//   observe:D[:S]  hold yaw D and level pitch, tracing normal movement for S seconds (default 25, 0..120),
//           without changing movement keys or position; unlike the driven tests, @ placement is refused
//   escape  from the start, run 1.2 s in 8 directions, then again jumping: how far each gets (stuck: none)
//   wander:S[:N]  run S seconds (seed N), turning to a new heading every 1.5 s and jumping now and then; reports each
//           stretch of 3 s that moved under 1 unit, with that frame's collision decisions
// "<test>@X,Y,Z" first puts the feet at that world point (comparable starts); a "+trace" suffix logs every
// frame's collision decisions (PlayerMovement::set_trace). observe always enables this trace until its time ends.

#include <cstdint>
#include <optional>
#include <string>

#include <glm/glm.hpp>

#include "app/player_movement.h"

namespace hailstone::assets { struct Zone; }

namespace hailstone::app {

class MovementTest {
public:
    explicit MovementTest(std::string spec);
    bool active() const { return !kind_.empty(); }
    // Before the movement update: keys, camera yaw/pitch, lifts.
    void drive(float t, MovementKeys& keys, float& yaw, float& pitch, PlayerMovement& movement,
               net::GameConnection& conn, const assets::Zone* zone, const MoveWorld* world, const glm::vec3& feet);
    // After it.
    void observe(float t, const PlayerMovement::Frame& frame, const PlayerMovement& movement,
                 const net::GameConnection& conn);

private:
    std::string kind_;
    std::optional<glm::vec3> start_; // "@X,Y,Z": the feet's start
    bool trace_ = false;              // "+trace"
    float walk_yaw_ = 0.0f, walk_secs_ = 3.0f;
    int escape_slot_ = -1;
    glm::vec3 escape_from_{0.0f};
    float wander_secs_ = 0.0f;
    std::uint32_t wander_seed_ = 12345;
    glm::vec3 wander_mark_{0.0f};
    float wander_mark_t_ = -1.0f;
    int wander_stuck_ = 0;
    bool started_ = false;
    float height_ = 0.0f;
    float last_log_ = 0.0f;
    float prev_drive_t_ = 0.0f; // the last drive()'s t (presses fire once)
    glm::vec3 mark_{0.0f};
    bool marked_ = false;
    float ground_z_ = 0.0f;
    float max_z_ = 0.0f;
    float air_start_ = -1.0f;
    int lifts_ = 0;
    std::optional<glm::vec3> water_;
    float swim_start_ = -1.0f;
    bool placed_in_water_ = false;
    bool done_ = false;
    bool aimed_ = false, turned_ = false;
    std::int64_t last_hp_ = -1;
};

} // namespace hailstone::app
