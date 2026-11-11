// ClientApp: Testing aids that drive the player: --auto-door and the drop/pickup test.

#include "app/client_app_internal.h"

#include "core/env.h"
#include "core/distribution.h"
#include "game/boats.h"

namespace hailstone::app::client_internal {

// HAILSTONE_AUTO_BOARD=name (testing boats, game/boats.h): HAILSTONE_AUTO_BOARD_SECONDS (6) after zoning in, the player is put
// a few units above the deck of the nearest vehicle whose name starts with `name` and drops onto it (as a teleport:
// the server takes the next position update). Once, and again after each zone-in.
void ClientApp::drive_auto_board() {
    if (distribution::managed()) return;
    static const char* want = hailstone::core::env("AUTO_BOARD");
    if (!want || !conn_) return;
    // HAILSTONE_AUTO_BOARD_HELM: then, once standing on it, the right click that asks for the helm of a small boat.
    static const bool helm = hailstone::core::env("AUTO_BOARD_HELM") != nullptr;
    if (helm && auto_board_zone_ == zone_entered_ && !auto_board_helm_ && conn_->standing_on()) {
        auto_board_helm_ = true;
        conn_->right_click_boat(conn_->standing_on());
    }
    if (auto_board_zone_ == zone_entered_) return;
    auto_board_helm_ = false;
    static const char* wait_s = hailstone::core::env("AUTO_BOARD_SECONDS");
    const float wait = wait_s ? static_cast<float>(std::atof(wait_s)) : 6.0f;
    if (std::chrono::duration<float>(Clock::now() - zone_entered_).count() < wait) return;
    // HAILSTONE_AUTO_BOARD_SWIM: in the water beside a boat lying still instead (a little below its z), facing it, to swim
    // aboard; it waits for one.
    static const bool swim = hailstone::core::env("AUTO_BOARD_SWIM") != nullptr;
    const game::Spawn* best = nullptr;
    float best_d = 0.0f;
    for (const auto& [id, s] : conn_->world().spawns) {
        if (!game::is_vehicle_race(s.race, s.gender) || game::display_name(s.name).rfind(want, 0) != 0) continue;
        if (swim && (s.speed_run != 0.0f || !vehicle_models_.contains(id))) continue; // a boat lying still, drawn
        float d = glm::distance(game::drawn_pose(s).position, feet_);
        if (!best || d < best_d) {
            best = &s;
            best_d = d;
        }
    }
    if (!best && swim) return; // until one stops near enough to be drawn
    auto_board_zone_ = zone_entered_;
    if (!best) {
        log::warn("auto-board: no vehicle named {}", want);
        return;
    }
    // Over the middle of the model, above its top (taken once it is drawn; a model's height before that).
    float top = 25.0f;
    glm::vec3 middle(0.0f);
    if (auto m = vehicle_models_.find(best->id); m != vehicle_models_.end() && !m->second.triangles.empty()) {
        middle = glm::vec3(m->second.center.x, m->second.center.y, 0.0f);
        top = -1e9f;
        for (const auto& p : m->second.triangles)
            if (glm::length(glm::vec2(p - middle)) < 4.0f) top = std::max(top, p.z);
        if (top < -1e8f) top = m->second.center.z + m->second.radius;
    }
    glm::vec3 at = game::boat_to_world(game::drawn_pose(*best), middle + glm::vec3(0, 0, top + 3.0f));
    if (swim) {
        float side = 6.0f;
        if (auto m = vehicle_models_.find(best->id); m != vehicle_models_.end())
            for (const auto& p : m->second.triangles) side = std::max(side, std::fabs(p.y));
        at = game::boat_to_world(game::drawn_pose(*best), middle + glm::vec3(0, -(side + 3.0f), -4.0f));
        const glm::vec3 to = game::boat_to_world(game::drawn_pose(*best), middle) - at;
        yaw_ = std::atan2(to.y, to.x);
    }
    feet_ = at;
    movement_.place(feet_);
    log::info("auto-board: onto {} ({}) {:.0f} away, at ({:.1f} {:.1f} {:.1f})", game::display_name(best->name),
              best->id, best_d, at.x, at.y, at.z);
}

// --auto-door (testing): a few seconds after zoning in, walk to the nearest
// usable door, click it, and log what the server answers.
void ClientApp::drive_auto_door(MovementKeys& mk) {
    if (distribution::managed()) return;
    if (opts_.auto_door == 0 || !conn_ || !world_objects_ || auto_door_step_ == 3) return;
    if (opts_.auto_door == 4) {
        drive_drop_pickup();
        return;
    }
    auto now = Clock::now();
    float since = std::chrono::duration<float>(now - zone_entered_).count();
    glm::vec3 me = feet_ + glm::vec3(0, 0, floor_offset_);
    const game::Spawn* self = conn_->world().player();
    float height = self && self->size > 0.0f ? self->size : 6.0f;
    if (auto_door_step_ == 0) {
        // Testing: HAILSTONE_AUTO_DOOR_DELAY=S waits S seconds after zoning in (default 3), e.g. for a #goto first.
        static const char* delay_s = hailstone::core::env("AUTO_DOOR_DELAY");
        if (since < (delay_s ? std::strtof(delay_s, nullptr) : 3.0f)) return;
        const game::Door* best = nullptr;
        float best_d = 1e9f;
        for (const auto& d : conn_->doors()) {
            if (!game::door_can_be_used(d) || game::door_is_invisible(d) || world_objects_->door_radius(d.id) <= 0.0f)
                continue;
            if (opts_.auto_door >= 2 && d.type >= 9) continue; // hinged doors only
            // Testing: HAILSTONE_AUTO_DOOR_ID=N walks to that door instead of the nearest.
            static const char* want_id = hailstone::core::env("AUTO_DOOR_ID");
            if (want_id && d.id != std::atoi(want_id)) continue;
            float dist = glm::distance(me, d.world_position());
            if (dist < best_d) {
                best = &d;
                best_d = dist;
            }
        }
        if (!best) {
            log::warn("auto-door: no usable door ({} doors, {} with models)", conn_->doors().size(),
                      world_objects_->modelled_doors());
            auto_door_step_ = 3;
            return;
        }
        auto_door_id_ = best->id;
        auto_door_at_ = now;
        auto_door_step_ = 1;
        auto_door_center_ = world_objects_->door_center(best->id).value_or(best->world_position());
        auto_door_last_ = feet_;
        auto_door_moved_at_ = now;
        log::info("auto-door: door {} {} type {} state {} at ({:.1f} {:.1f} {:.1f}), {:.1f} away", best->id, best->name,
                  best->type, best->state, best->x, best->y, best->z, best_d);
    }
    const game::Door* d = conn_->door(auto_door_id_);
    if (!d) {
        auto_door_step_ = 3;
        return;
    }
    if (opts_.auto_door == 3) {
        drive_door_walk(mk, *d);
        return;
    }
    if (auto_door_step_ == 1) {
        glm::vec3 to = d->world_position() - me;
        yaw_ = std::atan2(to.y, to.x);
        bool near = game::door_within_reach(*d, me, height, world_objects_->door_radius(d->id) - 3.0f);
        static const char* walk_s = hailstone::core::env("AUTO_DOOR_SECONDS"); // testing: a longer walk
        bool timeout = now - auto_door_at_ > std::chrono::seconds(walk_s ? std::atoi(walk_s) : 25);
        if (!near && !timeout) {
            mk.forward = true;
            return;
        }
        if (now - auto_door_logged_ < std::chrono::seconds(1)) return;
        auto_door_logged_ = now;
        log::info("auto-door: clicking door {} from {:.1f} away (state {})", d->id, glm::length(to), d->state);
        // Refused (the server's position not here yet, after a #goto): again a second later, for 10 s.
        if (!use_door(d->id) && ++auto_door_retries_ < 10) return;
        auto_door_at_ = now;
        auto_door_step_ = 2;
        auto_door_state_ = d->state;
        return;
    }
    if (d->state != auto_door_state_) {
        log::info("auto-door: door {} state {} -> {} ({} OP_MoveDoor so far), heading {:.1f} z {:.1f}", d->id,
                  auto_door_state_, d->state, conn_->door_moves_seen(), d->heading, d->z);
        auto_door_state_ = d->state;
    }
    // A lift's ride: the door's and the player's heights twice a second.
    if (now - auto_door_logged_ > std::chrono::milliseconds(500)) {
        auto_door_logged_ = now;
        log::info("auto-door: door {} state {} z {:.2f}; player feet ({:.2f} {:.2f} {:.2f}), standing on {}", d->id,
                  d->state, d->z, feet_.x, feet_.y, feet_.z, movement_.standing_on());
    }
    static const char* hold_s = hailstone::core::env("AUTO_DOOR_SECONDS"); // testing: a longer ride
    if (now - auto_door_at_ > std::chrono::seconds(hold_s ? std::max(15, std::atoi(hold_s)) : 15)) auto_door_step_ = 3;
}

// --auto-door 3 (testing collision): open the door, step out to one side,
// close it, walk into it (it must stop us), open it, walk through.
void ClientApp::drive_door_walk(MovementKeys& mk, const game::Door& d) {
    if (distribution::managed()) return;
    auto now = Clock::now();
    auto elapsed = now - auto_door_at_;
    auto next = [&](int step) {
        auto_door_step_ = step;
        auto_door_at_ = now;
        auto_door_last_ = feet_;
        auto_door_moved_at_ = now;
    };
    auto click = [&] { use_door(d.id); };
    auto walk_to = [&](const glm::vec3& target) {
        glm::vec3 to = target - feet_;
        yaw_ = std::atan2(to.y, to.x);
        if (glm::distance(feet_, auto_door_last_) > 0.3f) {
            auto_door_last_ = feet_;
            auto_door_moved_at_ = now;
        }
        mk.forward = true;
        return glm::length(glm::vec2(to));
    };
    // The door's normal: its local X axis turned by its closed heading.
    float h = game::heading_to_yaw(d.default_heading);
    glm::vec3 n(std::cos(h), std::sin(h), 0.0f);
    if (glm::dot(feet_ - auto_door_center_, n) < 0.0f) n = -n; // the side we are on
    switch (auto_door_step_) {
    case 1: // open it
        click();
        next(10);
        break;
    case 10:
        if (d.state == game::kSwitchOpen) next(11);
        else if (elapsed > std::chrono::seconds(5)) next(11);
        break;
    case 11: { // step out to this side
        float left = walk_to(auto_door_center_ + n * 12.0f);
        if (left < 1.0f || elapsed > std::chrono::seconds(6)) {
            mk.forward = false;
            // The server forgets the open state after its close timer
            // without a packet; the client closes the door itself after
            // RepopFrequency (10 s for type 0), so wait for that.
            log::info("auto-door: {:.1f} from the doorway on one side; waiting for the door to close",
                      glm::length(glm::vec2(feet_ - auto_door_center_)));
            next(12);
        }
        break;
    }
    case 12:
        if (d.state == game::kSwitchClosed || elapsed > std::chrono::seconds(15)) next(13);
        break;
    case 13: { // into the closed door
        float left = walk_to(auto_door_center_);
        bool stalled = now - auto_door_moved_at_ > std::chrono::milliseconds(1500);
        if (stalled || left < 0.5f || elapsed > std::chrono::seconds(8)) {
            mk.forward = false;
            log::info("auto-door: walking into the closed door (state {}) stopped {:.1f} from its centre: {}", d.state, left,
                      stalled && left > 1.0f ? "blocked by the door" : "NOT blocked");
            click();
            next(14);
        }
        break;
    }
    case 14:
        if (d.state == game::kSwitchOpen || elapsed > std::chrono::seconds(5)) {
            auto_door_min_ = 1e9f;
            auto_door_beyond_ = auto_door_center_ - n * 12.0f;
            next(15);
        }
        break;
    case 15: { // through the open door
        float left = walk_to(auto_door_beyond_);
        auto_door_min_ = std::min(auto_door_min_, glm::length(glm::vec2(feet_ - auto_door_center_)));
        if (left < 1.0f || elapsed > std::chrono::seconds(6)) {
            mk.forward = false;
            log::info("auto-door: through the open door (state {}): within {:.1f} of the doorway centre, {:.1f} short of the far side: {}",
                      d.state, auto_door_min_, left, left < 3.0f ? "passed" : "did not pass");
            next(3);
        }
        break;
    }
    default:
        break;
    }
}

// --auto-door 4 (testing ground items): put a droppable general item on
// the cursor, drop it, click the ground item that appears, check that it
// comes back to the cursor and the object goes away, then put it back.
void ClientApp::drive_drop_pickup() {
    if (distribution::managed()) return;
    auto now = Clock::now();
    auto elapsed = now - auto_door_at_;
    auto next = [&](int step) {
        auto_door_step_ = step;
        auto_door_at_ = now;
    };
    const auto& inv = conn_->inventory();
    switch (auto_door_step_) {
    case 0: {
        if (now - zone_entered_ < std::chrono::seconds(4)) return;
        for (std::int16_t slot = net::kSlotGeneral1; slot <= net::kSlotGeneral10; ++slot) {
            const net::Item* it = inv.at(net::ItemSlot::possessions(slot));
            if (!it || it->is_bag() || it->no_trade() || it->no_ground) continue;
            log::info("auto-drop: picking up {} from general slot {}", it->name, slot);
            conn_->click_slot(net::ItemSlot::possessions(slot));
            next(20);
            return;
        }
        log::warn("auto-drop: no droppable item in the general slots");
        next(3);
        break;
    }
    case 20:
        if (inv.cursor()) {
            auto_drop_seen_.clear();
            for (const auto& o : conn_->ground_objects()) auto_drop_seen_.insert(o.drop_id);
            log::info("auto-drop: dropping {} ({} ground objects before)", inv.cursor()->name, auto_drop_seen_.size());
            conn_->drop_cursor_on_ground();
            next(21);
        } else if (elapsed > std::chrono::seconds(3)) {
            log::warn("auto-drop: the item did not reach the cursor");
            next(3);
        }
        break;
    case 21:
        for (const auto& o : conn_->ground_objects()) {
            if (auto_drop_seen_.contains(o.drop_id)) continue;
            if (elapsed < std::chrono::seconds(4)) {
                // Leave it lying a moment; with --screenshot and no --frames, take the picture now.
                if (elapsed > std::chrono::seconds(2) && !opts_.screenshot.empty() && opts_.frames == 0 && !auto_shot_) {
                    vk_.request_screenshot(opts_.screenshot);
                    auto_shot_ = true;
                }
                return;
            }
            glm::vec3 me = feet_ + glm::vec3(0, 0, floor_offset_);
            log::info("auto-drop: it lies on the ground as object {} {} type {}, {:.1f} away; clicking it", o.drop_id, o.name,
                      o.type, glm::distance(me, o.position));
            auto_door_id_ = 0;
            auto_door_min_ = static_cast<float>(o.drop_id);
            conn_->click_object(o.drop_id);
            next(22);
            return;
        }
        if (elapsed > std::chrono::seconds(8)) {
            log::warn("auto-drop: no new ground object appeared");
            next(3);
        }
        break;
    case 22: {
        auto id = static_cast<std::uint32_t>(auto_door_min_);
        bool gone = std::none_of(conn_->ground_objects().begin(), conn_->ground_objects().end(),
                                 [&](const game::GroundObject& o) { return o.drop_id == id; });
        if (gone && inv.cursor()) {
            log::info("auto-drop: picked up: {} is on the cursor and object {} is gone; putting it away", inv.cursor()->name, id);
            conn_->auto_inventory();
            next(3);
            if (!opts_.screenshot.empty() && opts_.frames == 0) quit_ = true;
        } else if (elapsed > std::chrono::seconds(8)) {
            log::warn("auto-drop: pickup incomplete (object {} {}, cursor {})", id, gone ? "gone" : "still there",
                      inv.cursor() ? inv.cursor()->name : std::string("empty"));
            next(3);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace hailstone::app::client_internal
