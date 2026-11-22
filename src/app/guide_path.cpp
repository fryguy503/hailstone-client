#include "app/guide_path.h"

#include <cmath>
#include <limits>

#include "game/server_ext.h"
#include "net/game_connection.h"

namespace hailstone::app {

std::function<bool()> actor_guide_eligibility(std::function<const net::GameConnection*()> current,
    const net::GameConnection& connection, std::uint32_t spawn, std::function<bool()> action) {
    const auto actor = connection.captureActorRef(spawn);
    const auto owner = connection.captureActorRef(connection.world().player_id);
    if (!action || !actor || !owner || *actor == *owner) return {};
    const auto policy = connection.server_ext().map_policy(connection);
    if (!policy.actor_guides_enabled()) return {};
    return [current = std::move(current), expected = &connection, actor = *actor, owner = *owner,
            revision = policy.revision, action = std::move(action)] {
        auto same = [&] {
            return current && current() == expected && expected->matches(owner) &&
                   expected->matches(actor) && !expected->player_status().dead;
        };
        if (!same()) return false;
        const auto now = expected->server_ext().map_policy(*expected);
        return now.actor_guides_enabled() && now.revision == revision && same() && action() && same();
    };
}

bool current_target_guide(std::function<const net::GameConnection*()> current,
    const std::function<bool(std::uint32_t, std::function<bool()>)>& start) {
    const auto* connection = current ? current() : nullptr;
    if (!connection || connection->player_status().dead) return false;
    const auto id = connection->target();
    if (!id || id == connection->world().player_id) return false;
    const auto actor = connection->captureActorRef(id);
    if (!actor || !connection->server_ext().map_policy(*connection).actor_guides_enabled()) return false;
    auto still_target = [current = std::move(current), connection, actor = *actor] {
        return current() == connection && connection->target() == actor.id && connection->matches(actor) &&
               !connection->player_status().dead;
    };
    return still_target() && (!start || start(id, std::move(still_target)));
}

std::function<bool()> find_walk_eligibility(std::function<const net::GameConnection*()> current,
                                          const net::GameConnection& connection, std::uint32_t spawn) {
    const auto found = connection.world().spawns.find(spawn);
    if (found == connection.world().spawns.end() || !found->second.is_npc || found->second.is_corpse)
        return [] { return false; };
    const auto incarnation = found->second.incarnation;
    const auto* player = connection.world().player();
    if (!player) return [] { return false; };
    const auto player_incarnation = player->incarnation;
    const auto context = connection.interaction_context();
    const auto policy = connection.server_ext().map_policy(connection).revision;
    const auto* expected = &connection;
    return [current = std::move(current), expected, spawn, incarnation, player_incarnation, context, policy] {
        if (!current || current() != expected || expected->state() != net::ConnectionState::InZone ||
            expected->interaction_context() != context || expected->server_ext().map_policy(*expected).revision != policy ||
            !expected->npc_find_allowed(spawn) || current() != expected ||
            expected->interaction_context() != context || expected->server_ext().map_policy(*expected).revision != policy)
            return false;
        const auto* owner = expected->world().player();
        if (!owner || owner->incarnation != player_incarnation) return false;
        const auto actor = expected->world().spawns.find(spawn);
        return actor != expected->world().spawns.end() && actor->second.incarnation == incarnation;
    };
}

namespace {

float flat(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a) - glm::vec2(b)); }

// The nearest point to `p` on the segment a-b (flat), and its distance.
glm::vec3 nearest_on(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p, float& t) {
    const glm::vec2 ab = glm::vec2(b) - glm::vec2(a);
    const float l2 = glm::dot(ab, ab);
    t = l2 > 0.0f ? glm::clamp(glm::dot(glm::vec2(p) - glm::vec2(a), ab) / l2, 0.0f, 1.0f) : 0.0f;
    return a + (b - a) * t;
}

} // namespace

std::optional<glm::vec3> guide_spawn_position(const net::GameConnection& connection, std::uint32_t spawn) {
    const auto& spawns = connection.world().spawns;
    auto target = spawns.find(spawn);
    if (target == spawns.end() || target->second.is_corpse) return std::nullopt;
    const auto incarnation = target->second.incarnation;
    if (target->second.is_npc) {
        if (!connection.npc_find_allowed(spawn)) return std::nullopt;
    } else if (!connection.server_ext().map_visible(connection, target->second, 1)) return std::nullopt;
    target = spawns.find(spawn);
    if (target == spawns.end() || target->second.incarnation != incarnation) return std::nullopt;
    return target->second.display_valid ? target->second.display_position : target->second.position;
}

void GuidePath::start(std::uint32_t spawn, std::string name, std::function<bool()> eligible, Mode mode) {
    ++revision_;
    eligible_ = std::move(eligible);
    spawn_ = spawn;
    name_ = std::move(name);
    line_.clear();
    partial_ = false;
    direction_only_ = mode == Mode::Actor;
    told_waiting_ = false;
    next_plan_ = 0.0;
    mode_ = mode;
}

bool GuidePath::eligible() const {
    const auto check = eligible_; // the callback may replace/retire this guide
    try { return !check || check(); }
    catch (...) { return false; }
}

void GuidePath::stop() {
    ++revision_;
    eligible_ = {};
    spawn_ = 0;
    name_.clear();
    line_.clear();
    partial_ = false;
    direction_only_ = false;
}

void GuidePath::trim(const glm::vec3& feet) {
    // The segment nearest the player: what is behind it goes, and the line starts at the player.
    if (line_.size() < 2) return;
    std::size_t best = 0;
    float best_d = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i + 1 < line_.size(); ++i) {
        float t = 0.0f;
        const glm::vec3 q = nearest_on(line_[i], line_[i + 1], feet, t);
        const float d = flat(q, feet);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (best > 0) line_.erase(line_.begin(), line_.begin() + static_cast<std::ptrdiff_t>(best));
    line_[0] = feet;
    if (line_.size() >= 2 && flat(line_[0], line_[1]) < 0.5f && line_.size() > 2) line_.erase(line_.begin() + 1);
}

GuidePath::Update GuidePath::update(const glm::vec3& feet, std::optional<glm::vec3> goal, double now,
                                    const PathFn& path) {
    Update u;
    if (!active()) return u;
    if (!goal) {
        u.gone = true;
        stop();
        return u;
    }
    const bool actor = mode_ == Mode::Actor;
    const float arrive = actor ? 2.0f : kArrive;
    const float arrive_z = actor ? 2.0f : 10.0f;
    if (flat(feet, *goal) <= arrive && std::abs(feet.z - goal->z) <= arrive_z) {
        u.arrived = true;
        stop();
        return u;
    }
    // Off the line, or the NPC moved: found again (at most every kReplanGap).
    bool replan = line_.size() < 2 || direction_only_;
    if (!replan && (flat(*goal, planned_goal_) > kGoalMoved ||
                    (actor && std::abs(goal->z - planned_goal_.z) > kGoalMoved))) replan = true;
    if (!replan) {
        float t = 0.0f, off = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i + 1 < line_.size(); ++i)
            off = std::min(off, flat(nearest_on(line_[i], line_[i + 1], feet, t), feet));
        if (off > kStray) replan = true;
    }
    const bool held = replan && (now < next_plan_ || !path); // a search due, not yet allowed
    if (replan && !held) {
        auto r = path(feet, *goal);
        if (actor && (!r || r->size() < 2 || flat(r->back(), *goal) > kArrive ||
                      std::abs(r->back().z - goal->z) > 10.0f)) {
            // The trail still points toward the observed actor while the mesh builds or cannot reach it.
            direction_only_ = true;
            next_plan_ = now + kMeshRetry;
        } else if (!r) {
            next_plan_ = now + kMeshRetry;
            if (!told_waiting_) {
                told_waiting_ = true;
                u.waiting_mesh = true;
            }
        } else if (r->size() < 2) {
            u.no_way = true;
            stop();
            return u;
        } else {
            direction_only_ = false;
            line_ = std::move(*r);
            planned_goal_ = *goal;
            partial_ = flat(line_.back(), *goal) > kArrive;
            next_plan_ = now + kReplanGap;
        }
    }
    if (direction_only_) {
        line_ = {feet, *goal};
        return u;
    }
    // Not while a search is held back: a line snapped to the player would hide that the player left it.
    if (!held) trim(feet);
    return u;
}

} // namespace hailstone::app
