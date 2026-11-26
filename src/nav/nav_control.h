#pragma once

// What the MQ side (/nav, /moveto, the Navigation TLO: mq/commands.cpp, mq/tlo_game.cpp) asks of the client's
// navigation (app/navigator.h), without knowing the client.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

#include "nav/nav_mesh.h"

namespace hailstone::nav {

struct NavStatus {
    bool mesh_loaded = false; // some of the zone's mesh is there
    bool mesh_complete = false;
    std::size_t tiles_done = 0, tiles_total = 0;
    bool active = false;      // a goal is being walked to
    bool paused = false;
    std::string goal;         // what is being walked to, for the chat and the TLO
    float remaining = 0.0f;   // the flat distance left along the path
    // /stick (NavControl::stick): the spawn kept to, 0 none, and how near.
    std::uint32_t stick_spawn = 0;
    float stick_distance = 0.0f;
    // /moveto (a go_to_point with `straight`): one is being walked (MoveTo.Moving), and the last one arrived
    // (MoveTo.Stopped: MQ2MoveUtils' StoppedMoveto, cleared by the next /moveto, not by /moveto off).
    bool moveto = false;
    bool moveto_arrived = false;
    // /afollow (NavControl::afollow): the spawn followed, 0 none; how near it stops; the trail's points left and their
    // length (with the gap to the spawn); waiting where the spawn warped.
    std::uint32_t afollow_spawn = 0;
    float afollow_distance = 0.0f;
    std::size_t afollow_waypoints = 0;
    float afollow_length = 0.0f;
    bool afollow_waiting_warp = false;
};

// /afollow's options (MQ2AdvPath's words, mq/nav_commands.cpp).
struct AFollowOptions {
    float distance = 10.0f; // stops when the trail left, and the gap to the spawn, is this short
    bool doors = true;      // opens doors in its way (nodoor: not)
};

// /stick's options (MQ2MoveUtils' words, mq/nav_commands.cpp).
struct StickOptions {
    float distance = 0.0f; // flat; 0: inside the player's melee range of the spawn (Navigator::kStickMeleeShare)
    bool behind = false;   // stand at its back (within 45 degrees of straight behind it)
};

class NavControl {
public:
    virtual ~NavControl() = default;
    // Each returns "" when the walk started, or why not (for the chat).
    // A point, world coordinates; done within `distance` (flat) of it, and in height within the follower's vertical
    // reach. `straight`: no mesh, walk straight at it (/moveto). `flat`: its height is not checked at all (a /moveto
    // loc without a Z, whose height is only the player's own when it started; a /moveto id, whose spawn may levitate).
    virtual std::string go_to_point(const glm::vec3& world, float distance, std::string label, bool straight = false,
                                    bool flat = false) = 0;
    // /moveto id: straight at where the spawn stands, flat (its height is not checked), as go_to_point. Each second
    // the walk takes the spawn's place again: it turns after a spawn that moves, and the look pitch follows it up
    // or down (out of the water the view alone; swimming, the way it swims).
    virtual std::string moveto_spawn(std::uint32_t id, float distance) = 0;
    // A spawn, followed as it moves.
    virtual std::string go_to_spawn(std::uint32_t id, float distance) = 0;
    // A door or switch; `click` uses it on arrival.
    virtual std::string go_to_door(std::uint8_t id, bool click) = 0;
    // Keeps to a spawn until unstick, the spawn goes, or the player moves: walks (over the mesh when it is far or out
    // of sight) whenever it is further than the distance, and faces it while near enough. A walk ends it.
    virtual std::string stick(std::uint32_t id, const StickOptions& options) = 0;
    virtual void unstick(std::string_view why) = 0;
    // Follows a spawn along the trail it leaves (MQ2AdvPath's /afollow): walks where it walked, not straight at it,
    // until afollow_off, the player's own movement keys, or the spawn gone and its trail walked. A walk or a stick
    // ends it, and it ends them.
    virtual std::string afollow(std::uint32_t id, const AFollowOptions& options) = 0;
    virtual void afollow_off(std::string_view why) = 0;
    // Ends the walk and the stick.
    virtual void stop(std::string_view why) = 0;
    virtual void set_paused(bool paused) = 0;
    virtual NavStatus status() const = 0;
    // The path's length from the player to a point over the mesh; nullopt when there is no path.
    virtual std::optional<float> path_length(const glm::vec3& world) const = 0;
    virtual PathOptions& options() = 0;
    // /moveto's arrival distance (flat; MoveTo.ArrivalDist), kept for the walks after it is set (MQ2MoveUtils' dist
    // and mdist). 10 until then, as MQ2MoveUtils' default. Not saved.
    virtual float& moveto_distance() = 0;
    // Builds the zone's mesh again from its geometry, ignoring the cache.
    virtual void rebuild() = 0;
    virtual void set_overlay(bool on) = 0;
    virtual bool overlay() const = 0;
};

} // namespace hailstone::nav
