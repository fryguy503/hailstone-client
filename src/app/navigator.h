#pragma once

// The client's navigation (docs/navigation.md): the zone's mesh (nav/zone_nav.h), the walk along a path
// (nav/path_follower.h) and the goals /nav and /moveto give it (nav/nav_control.h). The original client has no
// navigation; this is a Hailstone addition. It drives only what a player drives (the heading and the movement keys),
// once a frame, where /follow does (app/client_app_player.cpp).
//
// Doors are not in the zone's collision, so the mesh runs through doorways whatever the door's state. A closed door
// on the path a little way ahead is clicked as soon as it is within reach, without stopping, so it is open by the
// time the walk gets there; one that still blocks is clicked when the walk stalls in front of it
// (PathOptions::open_doors). A door whose click teleports cannot be told apart in RoF2's door record, so the option
// exists to turn this off (docs/unknowns.md).
//
// The mesh is read from the cache at the zone-in when there is a file for the zone, and otherwise built at the
// zone-in (nav.prebuild, on by default in the client) or, with it off, when it is first wanted (a /nav, the overlay,
// the Navigation TLO's path questions). A
// walk asked for before its tiles are there waits for them; a walk to a spawn asks the server for its path meanwhile
// (OP_FindPersonRequest, as /find asks it, but quietly: no path drawn, no sound; GameConnection::request_quiet_path),
// which a stock EQEmu server answers from its own navigation mesh when it has one. A walk that needs a new path while
// the mesh is still unfinished waits for its tiles (and asks the server again) rather than giving up.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "nav/nav_control.h"
#include "nav/path_follower.h"

struct ImDrawList;

namespace hailstone::assets { class ZoneRegions; }
namespace hailstone::game {
class CollisionMesh;
struct Door;
struct Spawn;
} // namespace hailstone::game
namespace hailstone::nav { class ZoneNav; }

namespace hailstone::app {

class Navigator final : public nav::NavControl {
public:
    struct Env {
        std::function<const game::Spawn*(std::uint32_t id)> spawn;
        std::function<const game::Door*(std::uint8_t id)> door;
        std::function<std::vector<const game::Door*>()> doors;
        std::function<bool(std::uint8_t id)> use_door;
        // The closed door whose model a body-sized sphere from `from` to `to` hits first (the movement sweep against
        // the doors), if any.
        std::function<std::optional<std::uint8_t>(const glm::vec3& from, const glm::vec3& to)> door_in_way;
        // Whether the player is within reach of the door for a click (game::door_within_reach, as UseSwitch tests
        // it). Unset: no door is clicked ahead of the walk, only when it stalls at one.
        std::function<bool(std::uint8_t id)> door_reachable;
        std::function<void(const std::string&)> chat;
        // The server's path to a spawn (OP_FindPersonRequest, quietly: GameConnection::request_quiet_path): ask, then
        // the reply once it has come (the destination first, as sent; empty: the server has none).
        std::function<void(std::uint32_t spawn_id)> request_server_path;
        std::function<std::optional<std::vector<glm::vec3>>()> server_path;
        std::function<void()> started; // a walk began: /follow ends
        // The player's melee range of a spawn (the original's __get_melee_range), for /stick's default distance.
        std::function<std::optional<float>(std::uint32_t spawn)> melee_range;
        // The zone's line of sight between two points (world), for /stick's straight walk; null: never straight.
        std::function<bool(const glm::vec3&, const glm::vec3&)> line_of_sight;
        int threads = 2;
        bool prebuild = false; // build at the zone-in, not when first wanted (the client turns it on: nav.prebuild)
        bool assist_follow = true; // /follow's mesh help (hailstone.ini nav.follow)
    };
    explicit Navigator(Env env);
    ~Navigator() override;

    // A zone's collision is loaded. `collision` and `regions` (may be null) must stay until zone_unloaded. The mesh
    // is cached as `cache_file`.
    void zone_loaded(const game::CollisionMesh& collision, const assets::ZoneRegions* regions,
                     std::filesystem::path cache_file);
    void zone_unloaded();

    struct Frame {
        glm::vec3 feet{0.0f};  // world, the player's feet
        double now = 0.0;      // seconds
        bool can_move = true;  // standing or ducking, not frozen, dead or locked
        bool player_input = false; // the player pressed a movement key: the walk ends (as /follow's does)
        float height = 6.0f;   // the player's AvatarHeight, for the door reach
        bool swimming = false; // the body is in water (BodyWet 5 and up): forward follows the look pitch
        bool head_under = false; // the eyes are under water (HeadWet): no air
        float air = 1.0f;      // the air left, of the most (1 when unknown)
    };
    // Once a frame: what to steer (inactive: leave the movement alone).
    nav::SteerOutput frame(const Frame& f);
    // /follow's help (a Hailstone change to the original's FollowPlayerAI, game/follow.h; PathOptions::assist_follow,
    // hailstone.ini nav.follow): while the followed spawn is in a clear, walkable straight line the original's step
    // steers alone. When a wall, a corner or a drop is in the way it walks the mesh's path towards the spawn
    // instead, with the walk's own unsticking, and the follow keeps on up to kFollowLeash away (the original gives
    // up past 200).
    struct FollowAssist {
        bool detour = false;          // false: the original's step steers
        nav::SteerOutput steer;       // with detour: the heading, jump and strafe
        float remaining = 0.0f;       // with detour: the way left to the spawn along the path
        glm::vec3 aim{0.0f};          // with detour: the path's next corner (for swim_pitch)
        glm::vec3 along{0.0f};        // with detour: the point a little on along the path (for swim_pitch)
    };
    static constexpr float kFollowLeash = 400.0f;
    // Swimming towards `aim` (a path corner or the followed spawn's feet): the look pitch to set, since forward
    // swims along it (the physics' push along the pitch); none when not swimming. Out of breath with the head
    // under, straight up. A corner out of the water close ahead: up, so forward rises and the original's swim jump
    // (DoSwimJump: forward with the pitch up, the surface ahead clear) climbs out. At the surface with the way ahead
    // no deeper: level, so the head stays out. Otherwise towards `along` (a point a little on along the path:
    // PathFollower::ahead), so the swim keeps to the path's height (diving, or rising to it); `aim` without it.
    // The follower's unsticking strafe, or 0 when a drop is beside the walk.
    int safe_strafe(const Frame& f, const nav::SteerOutput& out) const;
    std::optional<float> swim_pitch(const Frame& f, const glm::vec3& aim,
                                    std::optional<glm::vec3> along = std::nullopt) const;
    FollowAssist assist_follow(const glm::vec3& feet, const glm::vec3& target_feet, bool clear_line, double now);
    // The follow ended or paused: the detour's path is dropped.
    void end_follow();
    bool follow_assist_on() const;
    // While a walk or a stick moves the player with PathOptions::pass_open_doors on, the doors that are not shut do
    // not block it (the movement sweep skips them; the walk looks only for shut doors in its way). A Hailstone
    // difference from the original's collision: the client cannot know a door's state on the server (EQEmu shuts most
    // doors after 5 s without a word, and an NPC leaving a waypoint opens the doors near it, told only to players
    // within 200), so clicks to get an open leaf out of the way swung doors the wrong way and left walks stuck
    // against them.
    bool passes_open_doors() const { return options_.pass_open_doors && (goal_.kind != GoalKind::None || stick_); }

    // The mesh and the path over the 3D view (/nav ui), around the player.
    void draw_overlay(ImDrawList* dl, const glm::mat4& view_proj, const glm::vec4& area) const;

    // NavControl
    std::string go_to_point(const glm::vec3& world, float distance, std::string label, bool straight) override;
    std::string go_to_spawn(std::uint32_t id, float distance) override;
    std::string go_to_door(std::uint8_t id, bool click) override;
    std::string stick(std::uint32_t id, const nav::StickOptions& options) override;
    void unstick(std::string_view why) override;
    void stop(std::string_view why) override;
    void set_paused(bool paused) override;
    // /stick's default distance: this share of the melee range, so the spawn's small moves and the position updates'
    // lag keep the player in reach (Hailstone's choice; MQ2MoveUtils' own is not taken).
    static constexpr float kStickMeleeShare = 0.75f;
    nav::NavStatus status() const override;
    std::optional<float> path_length(const glm::vec3& world) const override;
    // A path over the mesh from `from` to `to` for the Find window's guide path (app/guide_path.h), with the walk's
    // options: nullopt while the mesh is not ready (its build is started), empty when there is no way. A partial
    // path ends where it gets nearest.
    std::optional<std::vector<glm::vec3>> plan_between(const glm::vec3& from, const glm::vec3& to);
    nav::PathOptions& options() override { return options_; }
    void rebuild() override;
    void set_overlay(bool on) override {
        overlay_ = on;
        if (on) ensure_zone();
    }
    bool overlay() const override { return overlay_; }

    const nav::PathFollower& follower() const { return follower_; }
    const nav::ZoneNav* zone_nav() const { return zone_.get(); }
    // Starts the mesh's build now if it has not started (the overlay, tests).
    void build_now() { ensure_zone(); }

private:
    enum class GoalKind { None, Point, Spawn, Door };
    struct Goal {
        GoalKind kind = GoalKind::None;
        glm::vec3 point{0.0f};   // where the path ends (a spawn's: where it stood when the path was found)
        std::uint32_t spawn = 0;
        std::uint8_t door = 0;
        bool click = false;
        bool straight = false;
        float distance = 2.0f;
        std::string label;
    };
    struct Stick {
        std::uint32_t spawn = 0;
        nav::StickOptions options;
        std::string label;
    };
    std::string begin(Goal g);
    nav::SteerOutput walk_frame(const Frame& f);
    nav::SteerOutput stick_frame(const Frame& f);
    float stick_distance() const;
    bool plan(const glm::vec3& from);
    std::optional<glm::vec3> goal_point() const;
    void finish(const std::string& message);
    bool open_door_ahead(const Frame& f);
    // A closed door on the path a little way ahead, within reach: clicked without stopping.
    void open_door_early(const Frame& f);
    // An open door's leaf in the way a few units on: a way round it, if there is one: a point beside the walk on
    // the clear side, then one past the leaf (feet heights).
    struct DoorWay {
        std::uint8_t door = 0;
        glm::vec3 beside{0.0f}, past{0.0f};
    };
    std::optional<DoorWay> pass_open_door(const Frame& f) const;
    // Clicks on a door in the last kDoorOpenedSeconds, by any walk (doors_clicks_).
    int recent_clicks(std::uint8_t id, double now) const;
    // Whether a door may be clicked again now: under kDoorClicksPerSpell recent clicks, or shut by a walk to give
    // way (doors_gave_way_) and shut still: a walk that finds it in its way opens it again, once.
    bool may_click(const game::Door& d, double now) const;
    // Shut by a walk to give way in the last kDoorOpenedSeconds, and at rest shut now (a reopen is due).
    bool gave_way(const game::Door& d, double now) const;
    int detour_tries(std::uint8_t id) const;
    // Notes a door this walk clicked that has come to rest open (doors_opened_).
    void note_opened(std::uint8_t id, double now);
    bool opened_lately(std::uint8_t id, double now) const;
    // A door the walk clicked that is still swinging, and the walk within its swing: steps back out of it, or
    // holds outside it, until the door comes to rest (nullopt: go on). A leaf that swings into the walker traps it
    // (The Plane of Hate's side doors, clicked ahead while the walk went on into the doorway).
    std::optional<nav::SteerOutput> clear_of_swing(const Frame& f);
    void note_swing(std::uint8_t id, std::uint8_t state_before, double now);
    // What a stuck walk saw, for the log: where it is, its next corner, and the doors within 50 (their place, heading,
    // swing and state; one whose model is at the feet, and the one the way on runs into).
    void log_stuck(const Frame& f) const;
    // The door whose model is in the walk's way a few units on (Env::door_in_way), if any.
    std::optional<std::uint8_t> door_in_way(const Frame& f) const;
    // The door a body-sized sphere from `from` to `to` meets first (Env::door_in_way), unless doors not shut are
    // passed (PathOptions::pass_open_doors) and it is one of them.
    std::optional<std::uint8_t> blocking_door(const glm::vec3& from, const glm::vec3& to) const;
    void start_zone(bool use_cache);
    bool water_at(const glm::vec3& p) const; // the zone's regions; false without them
    void ensure_zone(); // the mesh is being read or built

    Env env_;
    nav::PathOptions options_;
    nav::PathFollower follower_;
    nav::PathFollower follow_path_;     // /follow's detour (assist_follow), apart from /nav's walk
    glm::vec3 follow_planned_to_{0.0f}; // where the spawn stood when the detour was planned
    double follow_replan_at_ = 0.0;
    double follow_retry_at_ = 0.0;      // after a detour failed, the original's step alone until then
    Goal goal_;
    std::optional<Stick> stick_;
    std::string last_finish_; // finish()'s message: a stick's walk that ended with one (no path, stuck) ends the stick
    bool paused_ = false;
    bool overlay_ = false;
    glm::vec3 feet_{0.0f};
    double now_ = 0.0;
    double replan_at_ = 0.0;
    double server_asked_at_ = -1.0; // waiting for the server's path since
    bool waiting_mesh_ = false;     // the goal waits for the mesh's tiles
    bool partial_ = false;          // the path ends short of the goal (the goal is off the mesh or cut off)
    bool partial_told_ = false;
    double door_clicked_at_ = -10.0;
    std::optional<std::uint8_t> door_waiting_; // a door clicked in the way: the walk holds until it is out of the way
    bool door_moved_ = false;                  // the door waited for has left the state it was clicked in
    std::uint8_t door_start_state_ = 0;        // that state
    std::uint32_t door_start_stamp_ = 0;       // its timestamp then: a change with no move is the server's answer
    bool door_answered_ = false;               // clicked again after an answer that left it as it was
    std::vector<std::pair<std::uint8_t, double>> doors_clicked_; // this walk's clicks: a door is not clicked again soon
    double door_wait_until_ = 0.0;
    std::string door_failed_;                  // the last door that did not open, for the stuck message
    std::vector<std::uint8_t> doors_shut_;     // doors clicked this walk that did not open: not clicked again
    struct EarlyDoor {
        std::uint8_t id = 0;
        double at = 0.0;
        bool moved = false; // it has left the closed state since the click
    };
    std::optional<EarlyDoor> early_door_; // the door last clicked ahead of the walk
    struct Swing {
        std::uint8_t id = 0;
        double at = 0.0;
        std::uint8_t from_state = 0;
    };
    std::optional<Swing> swing_; // the door the walk last clicked, until it has swung (clear_of_swing)
    // Doors a walk opened, and when: not clicked again while they stand open, also by a later walk (a script starts
    // walk after walk), since a click would swing the door shut in the walk's face.
    std::vector<std::pair<std::uint8_t, double>> doors_opened_;
    std::optional<glm::vec3> door_detour_; // the point past a leaf the walk is going round by (pass_open_door)
    std::vector<std::pair<std::uint8_t, int>> detour_tries_;    // this walk's ways round each door's leaf
    std::vector<std::pair<std::uint8_t, double>> doors_clicks_; // every walk's door clicks, for recent_clicks
    std::vector<std::pair<std::uint8_t, double>> doors_gave_way_; // doors any walk shut to give way, and when
    double early_checked_at_ = -10.0;
    bool swim_logged_ = false; // the walk is swimming (logged once each way)
    double swim_changed_at_ = -1.0; // when the swimming state last differed from the logged one

    const game::CollisionMesh* collision_ = nullptr;
    std::unique_ptr<nav::ZoneNav> zone_;
    std::shared_ptr<const nav::InputGeometry> geo_;
    const assets::ZoneRegions* regions_ = nullptr;
    std::filesystem::path cache_file_;
};

} // namespace hailstone::app
