#include "app/navigator.h"

#include "core/distribution.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <system_error>

#include <imgui.h>

#include "assets/regions.h"
#include "core/log.h"
#include "game/collision.h"
#include "game/coords.h"
#include "game/doors.h"
#include "game/world_state.h"
#include "nav/zone_nav.h"

namespace hailstone::app {

namespace {

// How often a walk to a moving spawn looks for a new path, and how far the spawn must have moved.
constexpr double kRepathSeconds = 0.5;
constexpr float kRepathMoved = 3.0f;
// A goal this far over or under where a walk ended is on another floor (nav::FollowerTuning::vertical_reach).
constexpr float kOtherFloor = 8.0f;
// /stick walks straight at its spot within this (flat), when the way is in plain sight; over the mesh beyond.
constexpr float kStickStraight = 40.0f;
// How far on along the path a swimmer aims its pitch (Navigator::swim_pitch).
constexpr float kSwimAhead = 4.0f;
// An unsticking strafe is held back when the floor this far to either side is more than kStrafeDrop under the feet
// (looked for from kStrafeStepUp over them).
constexpr float kStrafeProbe = 4.0f;
constexpr float kStrafeStepUp = 4.0f;
constexpr float kStrafeDrop = 5.0f;
// How long the server has to answer a /find before the walk gives up.
constexpr double kServerPathSeconds = 3.0;
// A door is clicked again no sooner than this.
constexpr double kDoorRetrySeconds = 0.5; // between any two door clicks (each door: kDoorReclickSeconds)
constexpr double kDoorWaitSeconds = 8.0;
constexpr double kDoorReclickSeconds = 15.0; // a door this walk clicked is not clicked again sooner // how long a walk holds for a clicked door to open
constexpr float kDoorProbe = 6.0f; // how far on a stalled walk looks for a door's model in its way
constexpr float kDoorLookAhead = 30.0f;     // how far along the path a door is looked for, to click it early
constexpr double kDoorLookSeconds = 0.2;    // how often
constexpr double kDoorOpenedSeconds = 120.0; // a door a walk opened is not clicked shut for this long while open
constexpr int kDoorDetours = 2;              // ways round one door's leaf a walk tries before it treats it as a stall
constexpr int kDoorClicksPerSpell = 2;       // clicks on one door in kDoorOpenedSeconds, by any walk
constexpr float kDoorSwingClear = 14.0f;     // a walk keeps this far (times the door's scale) from a door swinging
constexpr double kDoorNoAnswerSeconds = 1.5; // a clicked door that has not moved by then is not waited out here

glm::vec3 door_position(const game::Door& d) { return {d.y, d.x, d.z}; }

nav::Area classify(const assets::ZoneRegions* regions, const glm::vec3& p) {
    if (!regions) return nav::Area::Ground;
    assets::RegionInfo r = regions->lookup(p);
    if (r.is_zone_line()) return nav::Area::ZoneLine;
    if (r.is_lava() || r.type() == assets::RegionType::Slime) return nav::Area::Lava;
    if (r.is_water()) return nav::Area::Water;
    if (r.is_pvp()) return nav::Area::PvP;
    return nav::Area::Ground;
}

} // namespace

Navigator::Navigator(Env env) : env_(std::move(env)) { options_.assist_follow = env_.assist_follow; }

Navigator::~Navigator() = default;

void Navigator::zone_loaded(const game::CollisionMesh& collision, const assets::ZoneRegions* regions,
                            std::filesystem::path cache_file) {
    zone_unloaded();
    // Door ids are the zone's: what walks did with the last zone's doors does not carry over.
    doors_opened_.clear();
    doors_clicks_.clear();
    doors_gave_way_.clear();
    swing_.reset();
    collision_ = &collision;
    regions_ = regions;
    cache_file_ = std::move(cache_file);
    std::error_code ec;
    if (env_.prebuild || (!cache_file_.empty() && std::filesystem::exists(cache_file_, ec))) ensure_zone();
}

void Navigator::ensure_zone() {
    if (zone_ || !collision_) return;
    // The regions' water: water surfaces join the mesh (swum on) and are marked Water.
    nav::InputGeometry::WaterTest water;
    if (regions_) water = [regions = regions_](const glm::vec3& p) { return regions->lookup(p).is_water(); };
    // The zone lines' volumes, marked exactly (their BSP leaves' boxes, or EQG's boxes), a little higher than the
    // volume: the client also fires a zone line buried under the ground from above it (game/zone_lines.h).
    std::vector<nav::AreaBox> boxes;
    if (regions_ && collision_->bounds()) {
        const glm::vec3 margin(200.0f);
        auto [lo, hi] = *collision_->bounds();
        for (const auto& c : regions_->leaf_cells(lo - margin, hi + margin, [this](std::uint32_t r) {
                 return regions_->info(r).is_zone_line();
             }))
            boxes.push_back({c.lo, c.hi + glm::vec3(0.0f, 0.0f, 10.0f), nav::Area::ZoneLine});
    }
    geo_ = nav::InputGeometry::from_collision(*collision_, nav::BuildParams{}, water, std::move(boxes));
    start_zone(true);
}

void Navigator::start_zone(bool use_cache) {
    zone_.reset();
    if (!geo_) return;
    if (!use_cache && !cache_file_.empty()) {
        std::error_code ec;
        std::filesystem::remove(cache_file_, ec);
    }
    nav::ZoneNav::Config cfg;
    cfg.threads = env_.threads;
    cfg.cache_file = cache_file_;
    const assets::ZoneRegions* regions = regions_;
    zone_ = std::make_unique<nav::ZoneNav>(geo_, cfg, [regions](const glm::vec3& p) { return classify(regions, p); });
}

void Navigator::zone_unloaded() {
    if (goal_.kind != GoalKind::None) stop("");
    zone_.reset();
    geo_.reset();
    collision_ = nullptr;
    regions_ = nullptr;
}

void Navigator::rebuild() {
    if (goal_.kind != GoalKind::None) stop("");
    if (!geo_) {
        ensure_zone();
        if (zone_ && zone_->from_cache()) start_zone(false);
        return;
    }
    start_zone(false);
}

std::optional<glm::vec3> Navigator::goal_point() const {
    switch (goal_.kind) {
    case GoalKind::Point: return goal_.point;
    case GoalKind::Spawn:
        if (const game::Spawn* s = env_.spawn ? env_.spawn(goal_.spawn) : nullptr)
            return s->position - glm::vec3(0.0f, 0.0f, s->heights.avatar_height);
        return std::nullopt;
    case GoalKind::Door:
        if (const game::Door* d = env_.door ? env_.door(goal_.door) : nullptr) return door_position(*d);
        return std::nullopt;
    case GoalKind::None: break;
    }
    return std::nullopt;
}

bool Navigator::plan(const glm::vec3& from) {
    auto to = goal_point();
    if (!to) return false;
    goal_.point = *to;
    std::vector<glm::vec3> points;
    if (goal_.straight) {
        points = {from, *to};
    } else if (zone_ && zone_->mesh().tile_count() > 0) {
        nav::PathResult r = zone_->mesh().find_path(from, *to, options_);
        // While tiles are still coming, a partial path may only mean the rest is not built yet: wait for them.
        if (r.partial && !zone_->complete()) return false;
        points = std::move(r.points);
        partial_ = r.partial;
    }
    if (points.empty()) return false;
    if (partial_ && !partial_told_ && env_.chat) {
        partial_told_ = true;
        env_.chat(std::format("[Nav] {} cannot be reached over the mesh; going as near as it allows.", goal_.label));
    }
    if (follower_.state() == nav::FollowState::Moving || follower_.state() == nav::FollowState::NeedsPath)
        follower_.replace(std::move(points));
    else
        follower_.start(std::move(points), goal_.distance);
    return true;
}

std::string Navigator::begin(Goal g) {
    follower_.stop();
    goal_ = std::move(g);
    paused_ = false;
    server_asked_at_ = -1.0;
    waiting_mesh_ = false;
    partial_ = false;
    partial_told_ = false;
    door_waiting_.reset();
    door_failed_.clear();
    doors_shut_.clear();
    doors_clicked_.clear();
    early_door_.reset();
    early_checked_at_ = -10.0;
    door_detour_.reset();
    detour_tries_.clear();
    replan_at_ = now_ + kRepathSeconds;
    if (!goal_.straight) ensure_zone();
    if (!goal_.straight && !zone_) {
        goal_ = {};
        return "this zone has no navigation mesh";
    }
    if (!plan(feet_)) {
        if (zone_->complete()) {
            std::string why = "no path to " + goal_.label;
            goal_ = {};
            return why;
        }
        // The tiles on the way are still being built: the walk starts when they are there.
        waiting_mesh_ = true;
        if (env_.chat)
            env_.chat(std::format("Building this zone's navigation mesh ({} of {} tiles); the walk starts when it can.",
                                  zone_->tiles_done(), zone_->tiles_total()));
        // Meanwhile a spawn's path can come from the server, when there is no mesh under the player.
        if (goal_.kind == GoalKind::Spawn && env_.request_server_path && !zone_->mesh().nearest(feet_)) {
            env_.request_server_path(goal_.spawn);
            server_asked_at_ = now_;
        }
    }
    if (env_.started) env_.started();
    log::info("nav: walking to {}", goal_.label);
    return {};
}

std::string Navigator::go_to_point(const glm::vec3& world, float distance, std::string label, bool straight) {
    if (distribution::managed()) return "navigation automation is disabled in this distribution";
    stick_.reset();
    Goal g;
    g.kind = GoalKind::Point;
    g.point = world;
    g.distance = distance > 0.0f ? distance : 2.0f;
    g.straight = straight;
    g.label = std::move(label);
    return begin(std::move(g));
}

std::string Navigator::go_to_spawn(std::uint32_t id, float distance) {
    if (distribution::managed()) return "navigation automation is disabled in this distribution";
    stick_.reset();
    const game::Spawn* s = env_.spawn ? env_.spawn(id) : nullptr;
    if (!s) return "no such spawn";
    Goal g;
    g.kind = GoalKind::Spawn;
    g.spawn = id;
    g.distance = distance > 0.0f ? distance : 5.0f;
    g.label = s->name;
    return begin(std::move(g));
}

std::string Navigator::go_to_door(std::uint8_t id, bool click) {
    if (distribution::managed()) return "navigation automation is disabled in this distribution";
    stick_.reset();
    const game::Door* d = env_.door ? env_.door(id) : nullptr;
    if (!d) return "no such door";
    Goal g;
    g.kind = GoalKind::Door;
    g.door = id;
    g.click = click;
    // Close enough to use it (UseSwitch's reach is the player's height + 20 + the actor's radius).
    g.distance = 8.0f;
    g.label = d->name;
    return begin(std::move(g));
}

void Navigator::finish(const std::string& message) {
    last_finish_ = message;
    if (!message.empty() && env_.chat) env_.chat(message);
    log::info("nav: {}", message.empty() ? std::string("stopped") : message);
    follower_.stop();
    goal_ = {};
    paused_ = false;
    server_asked_at_ = -1.0;
    waiting_mesh_ = false;
}

void Navigator::stop(std::string_view why) {
    stick_.reset();
    finish(std::string(why));
}

void Navigator::set_paused(bool paused) { paused_ = paused && (goal_.kind != GoalKind::None || stick_); }

std::string Navigator::stick(std::uint32_t id, const nav::StickOptions& options) {
    if (distribution::managed()) return "navigation automation is disabled in this distribution";
    const game::Spawn* s = env_.spawn ? env_.spawn(id) : nullptr;
    if (!s || s->is_corpse) return "no such spawn";
    follower_.stop();
    goal_ = {};
    paused_ = false;
    stick_ = Stick{id, options, s->name};
    if (env_.started) env_.started();
    log::info("nav: sticking to {} ({:.1f})", s->name, stick_distance());
    return {};
}

void Navigator::unstick(std::string_view why) {
    if (!stick_) return;
    stick_.reset();
    follower_.stop();
    goal_ = {};
    paused_ = false;
    if (!why.empty() && env_.chat) env_.chat(std::string(why));
    log::info("nav: stick off{}{}", why.empty() ? "" : ": ", why);
}

float Navigator::stick_distance() const {
    if (!stick_) return 0.0f;
    if (stick_->options.distance > 0.0f) return stick_->options.distance;
    const auto melee = env_.melee_range ? env_.melee_range(stick_->spawn) : std::nullopt;
    return melee ? std::max(*melee * kStickMeleeShare, 1.0f) : 10.0f;
}

nav::SteerOutput Navigator::stick_frame(const Frame& f) {
    if (f.player_input) {
        unstick("You are no longer sticking to anything.");
        return {};
    }
    const game::Spawn* s = env_.spawn ? env_.spawn(stick_->spawn) : nullptr;
    if (!s || s->is_corpse) {
        unstick(std::format("[Stick] {} is gone.", stick_->label));
        return {};
    }
    if (!f.can_move || paused_) return {};
    const glm::vec3 at = s->position - glm::vec3(0.0f, 0.0f, s->heights.avatar_height);
    const float want = stick_distance();
    glm::vec2 to_me = glm::vec2(f.feet - at);
    const float away = glm::length(to_me);
    // Behind: the player's bearing from the spawn within 45 degrees of its back.
    const float yaw = game::heading_to_yaw(s->heading);
    const glm::vec2 facing(std::cos(yaw), std::sin(yaw));
    const bool in_arc = !stick_->options.behind || (away > 0.01f && glm::dot(to_me / away, -facing) >= std::cos(glm::radians(45.0f)));
    glm::vec3 spot = at;
    float reach = want * 0.8f; // walked to a little inside, so it does not stop and start at the edge
    if (stick_->options.behind) {
        spot = at - glm::vec3(facing * (want * 0.6f), 0.0f);
        reach = want * 0.3f;
    }
    nav::SteerOutput out;
    if (away <= want && in_arc) {
        // Near enough: hold, facing it.
        if (goal_.kind != GoalKind::None) {
            follower_.stop();
            goal_ = {};
        }
        out.active = true;
        out.yaw = away > 0.01f ? std::atan2(-to_me.y, -to_me.x) : yaw + glm::pi<float>();
        return out;
    }
    // Close and in plain sight: straight at the spot. Plain sight at the body's middle can still have a wall's
    // corner in the body's way, so the straight line must also stay on the mesh (which keeps its margin from walls)
    // until it is within reach of the spot: a mob against a wall stands off the mesh's margin itself. Off the mesh
    // (no answer), sight alone.
    const glm::vec2 to_spot = glm::vec2(spot - f.feet);
    const glm::vec3 eye(0.0f, 0.0f, f.height * 0.5f);
    auto mesh_clear = [&] {
        if (!zone_ || !zone_->mesh().initialised()) return true;
        std::optional<float> part = zone_->mesh().walkable_part(f.feet, spot, options_);
        if (!part) return true;
        return (1.0f - *part) * glm::length(to_spot) <= want;
    };
    if (glm::length(to_spot) <= kStickStraight && env_.line_of_sight && env_.line_of_sight(f.feet + eye, spot + eye) &&
        mesh_clear()) {
        if (goal_.kind != GoalKind::None) {
            follower_.stop();
            goal_ = {};
        }
        out.active = true;
        out.yaw = std::atan2(to_spot.y, to_spot.x);
        out.forward = glm::length(to_spot) > reach;
        if (!out.forward) out.yaw = std::atan2(-to_me.y, -to_me.x);
        return out;
    }
    // Further, or round a corner: over the mesh (straight without one, as MQ2MoveUtils walks), the path found
    // again as the spot moves.
    if (goal_.kind == GoalKind::None) {
        Goal g;
        g.kind = GoalKind::Point;
        g.point = spot;
        g.distance = reach;
        g.label = stick_->label;
        std::string err = begin(g);
        if (!err.empty() && !zone_) {
            g.straight = true;
            err = begin(std::move(g));
        }
        if (!err.empty()) {
            unstick(std::format("[Stick] {}: {}.", stick_->label, err));
            return {};
        }
    } else if (f.now >= replan_at_ && glm::distance(goal_.point, spot) > kRepathMoved) {
        replan_at_ = f.now + kRepathSeconds;
        goal_.point = spot;
        plan(f.feet);
    }
    last_finish_.clear();
    out = walk_frame(f);
    if (goal_.kind == GoalKind::None && !last_finish_.empty()) stick_.reset(); // no path, stuck: said already
    return out;
}

nav::NavStatus Navigator::status() const {
    nav::NavStatus s;
    if (zone_) {
        s.tiles_done = zone_->tiles_done();
        s.tiles_total = zone_->tiles_total();
        s.mesh_loaded = zone_->mesh().tile_count() > 0;
        s.mesh_complete = zone_->complete();
    }
    s.active = goal_.kind != GoalKind::None && !stick_;
    if (stick_) {
        s.stick_spawn = stick_->spawn;
        s.stick_distance = stick_distance();
    }
    s.paused = paused_;
    s.goal = goal_.label;
    s.remaining = follower_.remaining(feet_);
    return s;
}

std::optional<float> Navigator::path_length(const glm::vec3& world) const {
    if (!zone_) return std::nullopt;
    nav::PathResult r = zone_->mesh().find_path(feet_, world, options_);
    if (!r.found() || r.partial) return std::nullopt;
    float len = 0.0f;
    for (std::size_t i = 1; i < r.points.size(); ++i) len += glm::distance(r.points[i - 1], r.points[i]);
    return len;
}

std::optional<std::vector<glm::vec3>> Navigator::plan_between(const glm::vec3& from, const glm::vec3& to) {
    ensure_zone();
    if (!zone_) return std::nullopt;
    nav::PathResult r = zone_->mesh().find_path(from, to, options_);
    if (!r.found()) {
        if (!zone_->complete()) return std::nullopt; // the tiles on the way may not be built yet
        return std::vector<glm::vec3>{};
    }
    return std::move(r.points);
}

std::optional<std::uint8_t> Navigator::door_in_way(const Frame& f) const {
    // A body-sized sphere a few units on towards the next corner (along it in 3D, so a swimmer's way up or down
    // counts, then level), as the movement sweep meets the doors.
    if (!env_.door_in_way || follower_.next() >= follower_.points().size()) return std::nullopt;
    glm::vec3 to = follower_.points()[follower_.next()] - f.feet;
    glm::vec2 flat(to);
    glm::vec3 mid = f.feet + glm::vec3(0.0f, 0.0f, f.height * 0.5f);
    for (glm::vec3 way : {to, glm::vec3(flat, 0.0f)}) {
        if (glm::length(way) < 0.01f) continue;
        if (auto id = blocking_door(mid, mid + glm::normalize(way) * kDoorProbe)) return id;
    }
    return std::nullopt;
}

std::optional<std::uint8_t> Navigator::blocking_door(const glm::vec3& from, const glm::vec3& to) const {
    if (!env_.door_in_way) return std::nullopt;
    auto id = env_.door_in_way(from, to);
    if (!id || !options_.pass_open_doors || !env_.door) return id;
    const game::Door* d = env_.door(*id);
    if (d && d->state != game::kSwitchClosed) return std::nullopt;
    return id;
}

void Navigator::note_opened(std::uint8_t id, double now) {
    std::erase_if(doors_opened_, [&](const auto& e) { return e.first == id || now - e.second > kDoorOpenedSeconds; });
    doors_opened_.emplace_back(id, now);
}

int Navigator::recent_clicks(std::uint8_t id, double now) const {
    int n = 0;
    for (const auto& [door, at] : doors_clicks_)
        if (door == id && now - at < kDoorOpenedSeconds) ++n;
    return n;
}

bool Navigator::gave_way(const game::Door& d, double now) const {
    if (d.state != game::kSwitchClosed) return false;
    for (const auto& [door, at] : doors_gave_way_)
        if (door == d.id && now - at < kDoorOpenedSeconds) return true;
    return false;
}

bool Navigator::may_click(const game::Door& d, double now) const {
    return recent_clicks(d.id, now) < kDoorClicksPerSpell || gave_way(d, now);
}

int Navigator::detour_tries(std::uint8_t id) const {
    for (const auto& [door, n] : detour_tries_)
        if (door == id) return n;
    return 0;
}

bool Navigator::opened_lately(std::uint8_t id, double now) const {
    for (const auto& [door, at] : doors_opened_)
        if (door == id && now - at < kDoorOpenedSeconds) return true;
    return false;
}

std::optional<Navigator::DoorWay> Navigator::pass_open_door(const Frame& f) const {
    if (!env_.door_in_way || !env_.door || follower_.next() >= follower_.points().size()) return std::nullopt;
    auto id = door_in_way(f);
    if (!id) return std::nullopt;
    const game::Door* d = env_.door(*id);
    if (!d || d->state != game::kSwitchOpen) return std::nullopt;
    glm::vec2 to = glm::vec2(follower_.points()[follower_.next()] - f.feet);
    if (glm::length(to) < 0.01f) return std::nullopt;
    const glm::vec2 dir = glm::normalize(to);
    const glm::vec2 right(dir.y, -dir.x);
    const glm::vec3 lift(0.0f, 0.0f, f.height * 0.5f);
    const glm::vec3 mid = f.feet + lift;
    // How far on the leaf is (the same sweep, a little at a time).
    float at = 0.0f;
    while (at < kDoorProbe && !env_.door_in_way(mid + glm::vec3(dir * at, 0.0f), mid + glm::vec3(dir * (at + 0.5f), 0.0f)))
        at += 0.5f;
    // Out to a door's width either way, the nearer first: a point beside the walk from which the way on is clear,
    // and one a little past the leaf. Neither behind a wall (the zone's, line of sight) nor the leaf.
    auto clear = [&](const glm::vec3& a, const glm::vec3& b) {
        return !env_.door_in_way(a, b) && (!env_.line_of_sight || env_.line_of_sight(a, b));
    };
    for (float off : {1.5f, 3.0f, 4.5f, 6.0f, 7.5f, 9.0f})
        for (int sign : {1, -1}) {
            glm::vec3 beside = mid + glm::vec3(right * (off * static_cast<float>(sign)), 0.0f);
            glm::vec3 past = beside + glm::vec3(dir * (at + 4.0f), 0.0f);
            if (clear(mid, beside) && clear(beside, past)) return DoorWay{d->id, beside - lift, past - lift};
        }
    return std::nullopt;
}

void Navigator::note_swing(std::uint8_t id, std::uint8_t state_before, double now) {
    swing_ = Swing{id, now, state_before};
}

std::optional<nav::SteerOutput> Navigator::clear_of_swing(const Frame& f) {
    if (!swing_) return std::nullopt;
    if (options_.pass_open_doors) { // a swinging leaf does not block: nothing to keep out of
        swing_.reset();
        return std::nullopt;
    }
    const game::Door* d = env_.door ? env_.door(swing_->id) : nullptr;
    const double since = f.now - swing_->at;
    const bool moved = d && d->state != swing_->from_state;
    const bool resting = d && (d->state == game::kSwitchOpen || d->state == game::kSwitchClosed);
    if (!d || since >= kDoorWaitSeconds || (moved && resting) || (!moved && since >= kDoorNoAnswerSeconds)) {
        swing_.reset();
        follower_.forgive(f.now); // the stall clock starts again from here
        return std::nullopt;
    }
    glm::vec2 away = glm::vec2(f.feet - door_position(*d));
    float dist = glm::length(away);
    const float clear = kDoorSwingClear * std::max(d->scale(), 0.1f);
    if (dist >= clear + 2.0f) return std::nullopt;     // well outside the swing: on
    if (dist >= clear) return nav::SteerOutput{};     // at its edge: hold
    // Inside it: back away from the door (from the way the walk came, when on top of its origin).
    nav::SteerOutput out;
    out.active = true;
    out.forward = true;
    if (dist > 0.5f) {
        out.yaw = std::atan2(away.y, away.x);
    } else if (follower_.next() < follower_.points().size()) {
        glm::vec2 to = glm::vec2(follower_.points()[follower_.next()] - f.feet);
        out.yaw = std::atan2(-to.y, -to.x);
    } else {
        return nav::SteerOutput{};
    }
    return out;
}

void Navigator::log_stuck(const Frame& f) const {
    const auto& pts = follower_.points();
    if (follower_.next() < pts.size()) {
        const glm::vec3& n = pts[follower_.next()];
        log::info("nav: stuck at ({:.1f}, {:.1f}, {:.1f}); next corner ({:.1f}, {:.1f}, {:.1f}), {:.1f} away", f.feet.x,
                  f.feet.y, f.feet.z, n.x, n.y, n.z, glm::length(glm::vec2(n - f.feet)));
    } else {
        log::info("nav: stuck at ({:.1f}, {:.1f}, {:.1f}); no corner left", f.feet.x, f.feet.y, f.feet.z);
    }
    if (!env_.doors) return;
    const glm::vec3 lift(0.0f, 0.0f, f.height * 0.5f);
    for (const game::Door* d : env_.doors()) {
        if (!d) continue;
        float away = glm::length(glm::vec2(door_position(*d) - f.feet));
        if (away > 50.0f) continue; // an origin can be well off its doorway (a hinge, a wide secret wall)
        // A body-sized sphere barely moved at the feet: a door model that overlaps the body.
        bool on_body = env_.door_in_way && env_.door_in_way(f.feet + lift, f.feet + lift + glm::vec3(0.0f, 0.0f, 0.05f)) == d->id;
        log::info("nav:   door {} ({}) {:.1f} away at ({:.1f}, {:.1f}, {:.1f}), heading {:.1f} (closed {:.1f}), swing "
                  "{:.1f}, state {}, type {}, scale {}{}{}{}",
                  d->id, d->name, away, d->y, d->x, d->z, d->heading, d->default_heading, d->door_angle, d->state,
                  d->type, d->scale_factor, on_body ? ", its model on the body" : "",
                  opened_lately(d->id, f.now) ? ", opened by a walk" : "",
                  recent_clicks(d->id, f.now) > 0 ? std::format(", {} recent clicks", recent_clicks(d->id, f.now)) : "");
    }
    if (auto id = door_in_way(f)) log::info("nav:   the way on runs into door {}", *id);
}

bool Navigator::open_door_ahead(const Frame& f) {
    if (!options_.open_doors || !env_.doors || !env_.use_door || f.now - door_clicked_at_ < kDoorRetrySeconds) return false;
    if (follower_.next() >= follower_.points().size()) return false;
    glm::vec3 to = follower_.points()[follower_.next()] - f.feet;
    glm::vec2 dir = glm::length(glm::vec2(to)) > 0.01f ? glm::normalize(glm::vec2(to)) : glm::vec2(0.0f);
    auto shut = [this](std::uint8_t id) { return std::find(doors_shut_.begin(), doors_shut_.end(), id) != doors_shut_.end(); };
    const game::Door* best = nullptr;
    // The door the walk runs into, found by its model (door_in_way). A door's origin can be well off the doorway (a
    // hinge, a wide secret wall: Kedge Keep's), so this comes first.
    // Its model is in the way, so it is clicked whichever way it rests: some doors stand in the way while their
    // state reads open (inverted ones, a gate's other halves, Kedge Keep's KEDGRATE2 and KEDBOX). Not while it moves,
    // and not again soon after this walk clicked it (a second click would swing it back).
    auto clicked_lately = [&](std::uint8_t id) {
        for (const auto& [door, at] : doors_clicked_)
            if (door == id && f.now - at < kDoorReclickSeconds) return true;
        return false;
    };
    if (auto id = door_in_way(f); id && env_.door) {
        const game::Door* d = env_.door(*id);
        bool resting = d && (d->state == game::kSwitchClosed || d->state == game::kSwitchOpen);
        bool early = d && early_door_ && early_door_->id == d->id && f.now - early_door_->at < kDoorReclickSeconds;
        if (early && !resting) {
            // Clicked ahead of the walk and still swinging: hold for it as for a door clicked here.
            door_waiting_ = d->id;
            door_moved_ = true;
            door_start_state_ = game::kSwitchClosed;
            door_start_stamp_ = d->timestamp;
            door_answered_ = false;
            door_wait_until_ = f.now + kDoorWaitSeconds;
            return true;
        }
        // Clicked ahead and shut now (it never moved, or it has closed again since): clicked again here. One a walk
        // opened that is still in the way: not clicked shut while there may be a way round (a click would swing it
        // into the walk's face), but once the ways round have failed it is in the way open (a door that swings
        // across the passage being walked, not the one walked through), so it is shut; never more than
        // kDoorClicksPerSpell clicks in kDoorOpenedSeconds, so a door that blocks both ways is not swung for ever.
        // A door shut to give way and now shut in a walk's way is one being walked through (an open leaf that lies
        // across a passage as wide as the door blocks the doorway from that side; opened from in front of it, the
        // leaf swings past): it is opened again, past the click budget, once (may_click).
        bool ours_open = d && d->state == game::kSwitchOpen && opened_lately(d->id, f.now);
        bool give_way = ours_open && (detour_tries(d->id) >= kDoorDetours || !pass_open_door(f));
        bool spent = d && !may_click(*d, f.now);
        if (early && d->state == game::kSwitchClosed && game::door_can_be_used(*d) && !shut(d->id) && !spent) best = d;
        else if (resting && (!ours_open || give_way) && !spent && game::door_can_be_used(*d) && !shut(d->id) &&
                 (!clicked_lately(d->id) || gave_way(*d, f.now)))
            best = d;
    }
    float best_d = 15.0f;
    if (!best)
        for (const game::Door* d : env_.doors()) {
            if (!d || d->state != game::kSwitchClosed || !game::door_blocks_movement(*d) || !game::door_can_be_used(*d) ||
                shut(d->id) || (clicked_lately(d->id) && !gave_way(*d, f.now)) || !may_click(*d, f.now))
                continue;
            glm::vec3 p = door_position(*d) - f.feet;
            float dist = glm::length(glm::vec2(p));
            // In front, within reach of the walk.
            if (dist >= best_d || std::abs(p.z) > f.height + 4.0f) continue;
            if (dist > 1.0f && glm::dot(glm::normalize(glm::vec2(p)), dir) < 0.3f) continue;
            best = d;
            best_d = dist;
        }
    if (!best) return false;
    door_clicked_at_ = f.now;
    bool reopen = gave_way(*best, f.now);
    bool shutting = best->state == game::kSwitchOpen && opened_lately(best->id, f.now);
    if (reopen)
        log::info("nav: opening door {} ({}) again: shut to give way, it is the one being walked through", best->id,
                  best->name);
    else
        log::info("nav: {} door {} ({}) in the way", shutting ? "shutting" : "opening", best->id, best->name);
    const std::uint8_t before = best->state;
    const std::uint32_t stamp = best->timestamp; // before the click: an answer may come at once
    if (!env_.use_door(best->id)) return false;
    note_swing(best->id, before, f.now);
    if (shutting) doors_gave_way_.emplace_back(best->id, f.now);
    if (reopen) std::erase_if(doors_gave_way_, [&](const auto& e) { return e.first == best->id; });
    if (early_door_ && early_door_->id == best->id) early_door_.reset();
    doors_clicked_.emplace_back(best->id, f.now);
    doors_clicks_.emplace_back(best->id, f.now);
    door_waiting_ = best->id;
    door_moved_ = false;
    door_start_state_ = before;
    door_start_stamp_ = stamp;
    door_answered_ = false;
    door_wait_until_ = f.now + kDoorWaitSeconds;
    return true;
}

void Navigator::open_door_early(const Frame& f) {
    if (!options_.open_doors || !env_.door_in_way || !env_.door || !env_.use_door || !env_.door_reachable) return;
    if (early_door_)
        if (const game::Door* d = env_.door(early_door_->id); d && d->state != game::kSwitchClosed) {
            early_door_->moved = true;
            if (d->state == game::kSwitchOpen) note_opened(d->id, f.now);
        }
    if (f.now - early_checked_at_ < kDoorLookSeconds || f.now - door_clicked_at_ < kDoorRetrySeconds) return;
    early_checked_at_ = f.now;
    // The path ahead, at the body's middle, as far as kDoorLookAhead: the first door model a body-sized sphere along
    // it hits (the movement sweep against the doors, as door_in_way).
    const auto& points = follower_.points();
    glm::vec3 lift(0.0f, 0.0f, f.height * 0.5f);
    glm::vec3 from = f.feet + lift;
    float left = kDoorLookAhead;
    std::optional<std::uint8_t> hit;
    for (std::size_t i = follower_.next(); i < points.size() && left > 0.0f && !hit; ++i) {
        glm::vec3 to = points[i] + lift;
        float len = glm::length(to - from);
        if (len < 0.01f) continue;
        if (len > left) to = from + (to - from) * (left / len);
        hit = blocking_door(from, to);
        left -= glm::length(to - from);
        from = to;
    }
    if (!hit) return;
    const game::Door* d = env_.door(*hit);
    // Only a door at rest closed that blocks: one that reads open while in the way (an inverted door, a gate's
    // other half) is left to the stall, where a click is sure to be wanted.
    if (!d || d->state != game::kSwitchClosed || !game::door_blocks_movement(*d) || !game::door_can_be_used(*d)) return;
    if (std::find(doors_shut_.begin(), doors_shut_.end(), d->id) != doors_shut_.end()) return;
    for (const auto& [door, at] : doors_clicked_)
        if (door == d->id && f.now - at < kDoorReclickSeconds && !gave_way(*d, f.now)) return;
    if (!env_.door_reachable(d->id)) return; // not yet: looked at again as the walk comes nearer
    if (!may_click(*d, f.now)) return;
    door_clicked_at_ = f.now;
    bool reopen = gave_way(*d, f.now);
    log::info("nav: opening door {} ({}) ahead{}, {:.1f} away, from ({:.1f}, {:.1f}, {:.1f})", d->id, d->name,
              reopen ? " again: shut to give way, it is the one being walked through" : "",
              glm::length(glm::vec2(door_position(*d) - f.feet)), f.feet.x, f.feet.y, f.feet.z);
    const std::uint8_t before = d->state;
    if (!env_.use_door(d->id)) return;
    note_swing(d->id, before, f.now);
    doors_clicked_.emplace_back(d->id, f.now);
    doors_clicks_.emplace_back(d->id, f.now);
    if (reopen) std::erase_if(doors_gave_way_, [&](const auto& e) { return e.first == d->id; });
    early_door_ = EarlyDoor{d->id, f.now, false};
}

nav::SteerOutput Navigator::frame(const Frame& f) {
    if (distribution::managed()) {
        goal_ = {};
        stick_.reset();
        follower_.stop();
        end_follow();
        return {};
    }
    feet_ = f.feet;
    now_ = f.now;
    if (zone_) zone_->update(f.feet);
    if (stick_) return stick_frame(f);
    return walk_frame(f);
}

nav::SteerOutput Navigator::walk_frame(const Frame& f) {
    if (goal_.kind == GoalKind::None) return {};
    if (f.player_input) {
        finish("Navigation stopped.");
        return {};
    }
    if (!f.can_move || paused_) return {};

    // Waiting for the server's path (no mesh under the player when the walk began).
    if (server_asked_at_ >= 0.0) {
        std::optional<std::vector<glm::vec3>> reply = env_.server_path ? env_.server_path() : std::nullopt;
        std::vector<glm::vec3> sp = reply ? std::move(*reply) : std::vector<glm::vec3>{};
        if (reply && sp.empty()) {
            server_asked_at_ = -1.0; // the server has no path: wait for the mesh alone
        } else if (!sp.empty()) {
            // The reply's order is the destination first; the walk wants the player first.
            std::reverse(sp.begin(), sp.end());
            sp.insert(sp.begin(), f.feet);
            // The server's points are at its own mesh's heights, not ours: reached by distance across alone.
            follower_.start(std::move(sp), goal_.distance, false);
            server_asked_at_ = -1.0;
            waiting_mesh_ = false;
        } else if (f.now - server_asked_at_ > kServerPathSeconds) {
            server_asked_at_ = -1.0; // no answer: wait for the mesh alone
        }
    }
    // Waiting for the mesh: try again as tiles arrive, and give up once it is all there.
    if (waiting_mesh_) {
        if (f.now < replan_at_) return {};
        replan_at_ = f.now + kRepathSeconds;
        if (plan(f.feet)) {
            waiting_mesh_ = false;
        } else {
            if (zone_ && zone_->complete() && server_asked_at_ < 0.0) finish("No path to " + goal_.label + ".");
            return {};
        }
    }

    // A goal that moves (a spawn): a new path when it has gone elsewhere.
    if (goal_.kind == GoalKind::Spawn) {
        auto at = goal_point();
        if (!at) {
            finish(goal_.label + " is gone.");
            return {};
        }
        if (f.now >= replan_at_) {
            replan_at_ = f.now + kRepathSeconds;
            if (glm::distance(*at, goal_.point) > kRepathMoved) plan(f.feet);
        }
    }

    // Out of the swing of a door the walk clicked, until it has swung.
    if (auto s = clear_of_swing(f)) return *s;

    // A door clicked in the way: hold still until it is out of the way (open, or moved and no longer hit by the
    // walk's sweep: a gate's state can show it opening for a while, and it closes again on its own), then go on
    // with the stall clock and the counts started again. One that never moved is named if the walk gets stuck.
    if (door_waiting_) {
        const game::Door* d = env_.door ? env_.door(*door_waiting_) : nullptr;
        if (d && d->state != door_start_state_) door_moved_ = true;
        if (d && door_moved_ && d->state == game::kSwitchOpen) note_opened(d->id, f.now);
        // The server's answer left the door as it was (its timestamp moved, its state never did): the server had it
        // the other way. EQEmu shuts most doors after their close timer (5 s by default) without telling the
        // clients, while the client shows them open until its own repop time (10 or 20 s), so a click to shut one
        // in between opens it on the server, and the answer leaves it open here (the Plane of Hate's side doors,
        // shut to give way). The two agree now: clicked again, once.
        if (d && !door_moved_ && !door_answered_ && d->timestamp != door_start_stamp_ &&
            d->state == door_start_state_ && game::door_can_be_used(*d)) {
            door_answered_ = true;
            log::info("nav: door {} ({}) answered {}, as it was: clicked again", d->id, d->name,
                      d->state == game::kSwitchOpen ? "open" : "shut");
            const std::uint8_t before = d->state;
            const std::uint32_t stamp = d->timestamp;
            if (env_.use_door(d->id)) {
                note_swing(d->id, before, f.now);
                doors_clicked_.emplace_back(d->id, f.now);
                doors_clicks_.emplace_back(d->id, f.now);
                door_clicked_at_ = f.now;
                door_start_stamp_ = stamp;
                door_wait_until_ = f.now + kDoorWaitSeconds;
                return {};
            }
        }
        std::optional<std::uint8_t> in_way = door_in_way(f);
        // Another door in the way once this one has moved (a gate's next half): clicked at once.
        if (d && door_moved_ && in_way && *in_way != d->id) {
            door_waiting_.reset();
            if (open_door_ahead(f)) return {};
        }
        bool clear = d && door_moved_ && !in_way;
        if (door_waiting_ && d && !clear && f.now < door_wait_until_) return {};
        if (door_waiting_ && d && !door_moved_) {
            door_failed_ = d->name;
            doors_shut_.push_back(d->id);
            log::info("nav: door {} ({}) did not open", d->id, d->name);
        } else {
            door_failed_.clear();
        }
        door_waiting_.reset();
        follower_.forgive(f.now);
    }
    nav::SteerOutput out = follower_.step(f.feet, f.now);
    if (out.active) open_door_early(f);
    // An open door's leaf in the way (doors swing into the passage, and the mesh runs through them): round it on
    // the side that is clear, rather than stalling against it and clicking it shut.
    // The way round goes into the path ahead of its next corner: beside the walk, then past the leaf.
    if (door_detour_) {
        // Still going round while the point past the leaf is the next corner or the one after it.
        const auto& pts = follower_.points();
        bool ahead = false;
        for (std::size_t i = follower_.next(); i < pts.size() && i <= follower_.next() + 1; ++i)
            if (glm::distance(pts[i], *door_detour_) < 0.01f) ahead = true;
        if (!ahead) door_detour_.reset(); // passed, or a new path
    }
    // A few tries per door and walk: each new way round starts the stall clock again, so one that keeps failing
    // (a leaf across the whole passage) would hold the walk for ever; past them it is a stall (open_door_ahead).
    if (out.active && options_.open_doors && !options_.pass_open_doors && !door_detour_)
        if (auto way = pass_open_door(f); way && detour_tries(way->door) < kDoorDetours) {
            auto it = std::find_if(detour_tries_.begin(), detour_tries_.end(), [&](const auto& e) { return e.first == way->door; });
            if (it == detour_tries_.end()) detour_tries_.emplace_back(way->door, 1);
            else ++it->second;
            std::vector<glm::vec3> pts{f.feet, way->beside, way->past};
            pts.insert(pts.end(), follower_.points().begin() + static_cast<std::ptrdiff_t>(follower_.next()),
                       follower_.points().end());
            follower_.replace(std::move(pts), follower_.checks_height());
            door_detour_ = way->past;
            log::info("nav: round the open door {} from ({:.1f}, {:.1f}) by ({:.1f}, {:.1f}) to ({:.1f}, {:.1f}), try {}",
                      way->door, f.feet.x, f.feet.y, way->beside.x, way->beside.y, way->past.x, way->past.y,
                      detour_tries(way->door));
            out = follower_.step(f.feet, f.now);
        }
    // The first sign of a stall (the follower's first jump): a closed door in the way is clicked before any
    // jumping and strafing.
    if (out.jump && open_door_ahead(f)) return {};
    out.strafe = safe_strafe(f, out);
    // Never into a zone line by accident: the step ahead (3 units, a body's middle) inside one ends the walk, unless
    // zone lines are allowed or the goal itself is in one.
    if (out.active && !options_.allow_zone_lines && regions_) {
        glm::vec3 ahead = f.feet + glm::vec3(std::cos(out.yaw), std::sin(out.yaw), 0.0f) * 3.0f + glm::vec3(0.0f, 0.0f, 2.0f);
        bool goal_in_line = regions_->lookup(goal_.point + glm::vec3(0.0f, 0.0f, 2.0f)).is_zone_line();
        if (!goal_in_line && regions_->lookup(ahead).is_zone_line()) {
            finish(std::format("[Nav] Stopped at a zone line on the way to {} (/nav option zonelines on goes through).",
                               goal_.label));
            return {};
        }
    }
    switch (follower_.state()) {
    case nav::FollowState::Arrived: {
        Goal done = goal_;
        float short_by = glm::length(glm::vec2(done.point - f.feet));
        float rise = done.point.z - f.feet.z;
        if (partial_ && std::abs(rise) > kOtherFloor) {
            // On another floor than the goal's, with no way there over the mesh (a lift, a jump, a drop it lacks).
            finish(std::format("[Nav] Stopped {:.0f} {} {}: the mesh has no way {} there.", std::abs(rise),
                               rise > 0 ? "below" : "above", done.label, rise > 0 ? "up" : "down"));
            log::info("nav: ended {:.1f} {} {}", std::abs(rise), rise > 0 ? "below" : "above", done.label);
            return {};
        }
        if (partial_ && short_by > done.distance) {
            finish(std::format("[Nav] Stopped {:.0f} short of {}: as near as the mesh goes.", short_by, done.label));
            log::info("nav: ended {:.1f} short of {}", short_by, done.label);
            return {};
        }
        finish("");
        if (done.kind == GoalKind::Door && done.click && env_.use_door) env_.use_door(done.door);
        log::info("nav: arrived at {}", done.label);
        return {};
    }
    case nav::FollowState::NeedsPath:
        if (open_door_ahead(f)) {
            // Give the door time to open, then carry on along the same path.
            follower_.replace(std::vector<glm::vec3>(follower_.points().begin() +
                                                         static_cast<std::ptrdiff_t>(follower_.next() > 0 ? follower_.next() - 1 : 0),
                                                     follower_.points().end()),
                                  follower_.checks_height());
            return out;
        }
        if (!plan(f.feet)) {
            // Off the mesh for the moment (falling, or swimming between a deep floor and the surface): no new path
            // from here, so carry on along this one; the follower gives up after a few of these (Failed).
            if (zone_ && !zone_->mesh().nearest(f.feet) && !follower_.points().empty()) {
                follower_.replace(std::vector<glm::vec3>(follower_.points().begin() +
                                                             static_cast<std::ptrdiff_t>(follower_.next() > 0 ? follower_.next() - 1 : 0),
                                                         follower_.points().end()),
                                  follower_.checks_height());
                return out;
            }
            // The mesh may only be unfinished there: wait for its tiles (and ask the server again for a spawn).
            if (zone_ && !zone_->complete()) {
                follower_.stop();
                waiting_mesh_ = true;
                replan_at_ = f.now + kRepathSeconds;
                if (goal_.kind == GoalKind::Spawn && env_.request_server_path && server_asked_at_ < 0.0) {
                    env_.request_server_path(goal_.spawn);
                    server_asked_at_ = f.now;
                }
                return {};
            }
            finish("No path to " + goal_.label + ".");
            return {};
        }
        out = follower_.step(f.feet, f.now);
        out.strafe = safe_strafe(f, out);
        if (out.active && follower_.next() < follower_.points().size())
            out.pitch = swim_pitch(f, follower_.points()[follower_.next()], follower_.ahead(f.feet, kSwimAhead));
        return out;
    case nav::FollowState::Failed:
        log_stuck(f);
        finish("Navigation stuck going to " + goal_.label +
               (door_failed_.empty() ? std::string(".") : " (the door " + door_failed_ + " did not open)."));
        return {};
    default: break;
    }
    if (out.active && follower_.next() < follower_.points().size())
        out.pitch = swim_pitch(f, follower_.points()[follower_.next()], follower_.ahead(f.feet, kSwimAhead));
    // Logged once the change has held for a second (at the surface the body bobs in and out of the water).
    if (f.swimming == swim_logged_) swim_changed_at_ = -1.0;
    else if (swim_changed_at_ < 0.0) swim_changed_at_ = f.now;
    if (out.active && f.swimming != swim_logged_ && f.now - swim_changed_at_ >= 1.0) {
        swim_logged_ = f.swimming;
        swim_changed_at_ = -1.0;
        log::info("nav: {} (air {:.0f}%)", f.swimming ? "swimming" : "out of the water", f.air * 100.0f);
    }
    return out;
}

bool Navigator::water_at(const glm::vec3& p) const { return regions_ && regions_->lookup(p).is_water(); }

int Navigator::safe_strafe(const Frame& f, const nav::SteerOutput& out) const {
    // An unsticking strafe never steps off a ledge (a walkway over a pit): with a drop beside the walk on either
    // side, it only jumps. Both sides are tested, so it holds whichever way the strafe keys turn the heading.
    if (out.strafe == 0 || !collision_ || f.swimming) return out.strafe;
    const glm::vec3 side(-std::sin(out.yaw), std::cos(out.yaw), 0.0f);
    for (float s : {-1.0f, 1.0f}) {
        glm::vec3 p = f.feet + side * (s * kStrafeProbe);
        std::optional<float> floor = collision_->floor_below(p + glm::vec3(0.0f, 0.0f, kStrafeStepUp), 0.0f);
        if (!floor || f.feet.z - *floor > kStrafeDrop) return 0;
    }
    return out.strafe;
}

std::optional<float> Navigator::swim_pitch(const Frame& f, const glm::vec3& aim, std::optional<glm::vec3> along) const {
    if (!f.swimming) return std::nullopt;
    // Our own numbers (docs/navigation.md "Water"): when to head for air, how near a shore to climb, how steep.
    constexpr float kAirLow = 0.35f, kClimbOut = 0.7f, kUp = 1.0f, kSteepest = 1.1f, kShoreReach = 12.0f,
                    kSteepVertical = 1.4f;
    if (f.head_under && f.air < kAirLow) return kUp;
    float d = glm::length(glm::vec2(aim - f.feet));
    float dz = aim.z - f.feet.z;
    // A corner is in the water when there is water over it (the bottom) or just under it (the surface).
    bool aim_wet = water_at(aim + glm::vec3(0.0f, 0.0f, 2.0f)) || water_at(aim - glm::vec3(0.0f, 0.0f, 1.5f));
    if (regions_ && !aim_wet && d < kShoreReach) return kClimbOut;
    if (along) {
        d = glm::length(glm::vec2(*along - f.feet));
        dz = along->z - f.feet.z;
    }
    // At the surface (the head out, or no water a little over it: the swimmer cannot rise further) with the way
    // ahead no deeper: level, so forward goes across rather than pushing up against the surface for nothing.
    bool at_surface = !f.head_under || (regions_ && !water_at(f.feet + glm::vec3(0.0f, 0.0f, f.height + 1.5f)));
    if (at_surface && dz > -2.0f) return 0.0f;
    // Straight over or under (a link between a deep floor and the surface): steeper, so the swim stays on the column.
    float steepest = d < 3.0f ? kSteepVertical : kSteepest;
    return std::clamp(std::atan2(dz, std::max(d, 0.5f)), -steepest, steepest);
}

void Navigator::end_follow() {
    follow_path_.stop();
    follow_retry_at_ = 0.0;
}

bool Navigator::follow_assist_on() const { return !distribution::managed() && options_.assist_follow; }

Navigator::FollowAssist Navigator::assist_follow(const glm::vec3& feet, const glm::vec3& target_feet, bool clear_line,
                                                 double now) {
    FollowAssist out;
    if (distribution::managed()) {
        end_follow();
        return out;
    }
    if (!options_.assist_follow || now < follow_retry_at_) return out;
    ensure_zone();
    if (!zone_ || zone_->mesh().tile_count() == 0) return out;
    const nav::NavMesh& mesh = zone_->mesh();
    // Off the mesh (levitating, swimming, a ledge it left out): the original's step alone.
    if (!mesh.nearest(feet)) {
        follow_path_.stop();
        return out;
    }
    // A clear sight line that can be walked straight: the original's step, as it is.
    if (clear_line && mesh.walkable_line(feet, target_feet, options_)) {
        follow_path_.stop();
        return out;
    }
    bool moving = follow_path_.state() == nav::FollowState::Moving;
    bool replan = !moving || follow_path_.state() == nav::FollowState::NeedsPath ||
                  (now >= follow_replan_at_ && glm::distance(target_feet, follow_planned_to_) > kRepathMoved);
    if (replan) {
        follow_replan_at_ = now + kRepathSeconds;
        nav::PathResult r = mesh.find_path(feet, target_feet, options_);
        if (!r.found()) {
            follow_path_.stop();
            return out;
        }
        follow_planned_to_ = target_feet;
        if (follow_path_.state() == nav::FollowState::Moving || follow_path_.state() == nav::FollowState::NeedsPath)
            follow_path_.replace(std::move(r.points));
        else
            follow_path_.start(std::move(r.points), 1.0f);
    }
    out.steer = follow_path_.step(feet, now);
    if (follow_path_.state() == nav::FollowState::Failed) {
        // Stuck even with the mesh: the original's step for a while, then the mesh again.
        log::info("nav: /follow's detour is stuck; the plain follow for 5 s");
        follow_path_.stop();
        follow_retry_at_ = now + 5.0;
        return {};
    }
    if (!out.steer.active) return out; // arrived at the path's end (the spawn's last place): the original's step
    out.detour = true;
    const auto& pts = follow_path_.points();
    if (follow_path_.next() < pts.size()) {
        out.aim = pts[follow_path_.next()];
        out.along = follow_path_.ahead(feet, kSwimAhead);
    }
    out.remaining = follow_path_.remaining(feet) + (pts.empty() ? 0.0f : glm::length(glm::vec2(target_feet - pts.back())));
    return out;
}

void Navigator::draw_overlay(ImDrawList* dl, const glm::mat4& view_proj, const glm::vec4& area) const {
    if (!overlay_ || !dl) return;
    auto project = [&](const glm::vec3& p, ImVec2& out) {
        glm::vec4 c = view_proj * glm::vec4(p, 1.0f);
        if (c.w <= 0.1f) return false;
        out = ImVec2(area.x + (c.x / c.w * 0.5f + 0.5f) * area.z, area.y + (0.5f - c.y / c.w * 0.5f) * area.w);
        return true;
    };
    dl->PushClipRect(ImVec2(area.x, area.y), ImVec2(area.x + area.z, area.y + area.w));
    constexpr float kRange = 150.0f;
    if (zone_) {
        zone_->mesh().for_each_polygon([&](std::span<const glm::vec3> outline, nav::Area a) {
            if (outline.empty() || glm::distance(outline[0], feet_) > kRange) return;
            ImU32 colour = a == nav::Area::Water      ? IM_COL32(60, 140, 255, 110)
                           : a == nav::Area::Lava     ? IM_COL32(255, 70, 40, 130)
                           : a == nav::Area::ZoneLine ? IM_COL32(220, 80, 255, 130)
                           : a == nav::Area::PvP      ? IM_COL32(255, 200, 40, 110)
                                                      : IM_COL32(60, 220, 120, 90);
            for (std::size_t i = 0; i < outline.size(); ++i) {
                ImVec2 a2, b2;
                const glm::vec3 lift(0.0f, 0.0f, 0.2f);
                if (project(outline[i] + lift, a2) && project(outline[(i + 1) % outline.size()] + lift, b2))
                    dl->AddLine(a2, b2, colour, 1.0f);
            }
        });
        // The links: drops in orange, swims (and drops it can climb back up) in light blue.
        zone_->mesh().for_each_link([&](const glm::vec3& a, const glm::vec3& b, bool both) {
            if (glm::distance(a, feet_) > kRange) return;
            ImVec2 a2, b2;
            if (project(a, a2) && project(b, b2))
                dl->AddLine(a2, b2, both ? IM_COL32(140, 200, 255, 200) : IM_COL32(255, 150, 40, 200), 2.0f);
        });
    }
    for (const nav::PathFollower* f : {&follower_, &follow_path_}) {
        const auto& pts = f->points();
        ImU32 colour = f == &follower_ ? IM_COL32(255, 230, 40, 230) : IM_COL32(80, 220, 255, 230);
        for (std::size_t i = f->next() > 0 ? f->next() - 1 : 0; i + 1 < pts.size(); ++i) {
            ImVec2 a2, b2;
            const glm::vec3 lift(0.0f, 0.0f, 0.5f);
            glm::vec3 from = i + 1 == f->next() ? feet_ : pts[i];
            if (project(from + lift, a2) && project(pts[i + 1] + lift, b2)) dl->AddLine(a2, b2, colour, 3.0f);
        }
    }
    dl->PopClipRect();
}

} // namespace hailstone::app
