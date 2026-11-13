#include "app/movement_test.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "assets/zone.h"
#include "core/log.h"
#include "net/game_connection.h"

namespace hailstone::app {

MovementTest::MovementTest(std::string spec) {
    if (auto plus = spec.find("+trace"); plus != std::string::npos) {
        trace_ = true;
        spec.erase(plus, 6);
    }
    if (spec.rfind("observe:", 0) == 0) {
        // Parse separately from the driven tests: observation must never inherit their @ placement.
        const char* first = spec.c_str() + 8;
        char* end = nullptr;
        const float degrees = std::strtof(first, &end);
        bool valid = end != first;
        float seconds = 25.0f;
        if (valid && *end == ':') {
            first = end + 1;
            seconds = std::strtof(first, &end);
            valid = end != first;
        }
        if (!valid || *end != '\0' || !std::isfinite(degrees) || !std::isfinite(seconds) ||
            seconds < 0.0f || seconds > 120.0f) {
            log::warn("movetest: observe needs a finite yaw and duration in [0, 120], without placement");
            return;
        }
        // Keep subsequent yaw-to-heading arithmetic bounded even for a finite float near FLT_MAX.
        walk_yaw_ = std::fmod(degrees, 360.0f) * (3.14159265f / 180.0f);
        walk_secs_ = seconds;
        trace_ = true;
        kind_ = "observe";
        return;
    }
    // "<test>@X,Y,Z": first put the feet at the world point X Y Z (the same start for runs to compare).
    if (auto at = spec.find('@'); at != std::string::npos) {
        glm::vec3 p{0.0f};
        if (std::sscanf(spec.c_str() + at + 1, "%f,%f,%f", &p.x, &p.y, &p.z) == 3) start_ = p;
        spec.resize(at);
    }
    if (spec.rfind("fall:", 0) == 0) {
        height_ = std::strtof(spec.c_str() + 5, nullptr);
        spec = "fall";
    }
    if (spec.rfind("walk:", 0) == 0) {
        float deg = 0.0f, secs = 3.0f;
        std::sscanf(spec.c_str() + 5, "%f:%f", &deg, &secs);
        walk_yaw_ = deg * 3.14159265f / 180.0f;
        walk_secs_ = secs;
        spec = "walk";
    }
    if (spec.rfind("wander:", 0) == 0) {
        unsigned seed = 12345;
        std::sscanf(spec.c_str() + 7, "%f:%u", &wander_secs_, &seed);
        wander_seed_ = seed;
        spec = "wander";
    }
    if (spec == "speed" || spec == "jump" || spec == "fall" || spec == "swim" || spec == "walk" || spec == "escape" ||
        spec == "wander")
        kind_ = spec;
    else if (!spec.empty()) log::warn("movetest: unknown test '{}'", spec);
}

void MovementTest::drive(float t, MovementKeys& keys, float& yaw, float& pitch, PlayerMovement& movement,
                         net::GameConnection& conn, const assets::Zone* zone, const MoveWorld* world,
                         const glm::vec3& feet) {
    if (!active() || done_) return;
    if (kind_ == "observe") {
        if (t >= walk_secs_) {
            done_ = true;
            movement.set_trace(false);
            log::info("movetest: observe done at ({:.3f}, {:.3f}, {:.3f})", feet.x, feet.y, feet.z);
        } else {
            movement.set_trace(true);
            yaw = walk_yaw_;
            pitch = 0.0f;
        }
        return;
    }
    struct PrevT {
        float& prev;
        float now;
        ~PrevT() { prev = now; }
    } prev_t{prev_drive_t_, t};
    movement.set_trace(trace_ || kind_ == "wander");
    if (start_ && !started_ && t >= 0.5f) {
        started_ = true;
        movement.place(*start_);
        log::info("movetest: feet placed at ({:.2f}, {:.2f}, {:.2f})", start_->x, start_->y, start_->z);
    }
    const auto& body = movement.controller().body;
    if (kind_ == "speed" && !aimed_ && t >= 1.5f) {
        // Face the direction with the longest clear run (of 32, up to 400 units) for the player's own collision
        // sphere (the zone and the doors, as the movement sweeps them; a thin ray misses what the sphere's sides
        // and the doors stop).
        aimed_ = true;
        float best = -1.0f;
        for (int i = 0; i < 32 && world && world->collision_loaded(); ++i) {
            float a = static_cast<float>(i) * 6.2831853f / 32.0f;
            glm::vec3 dir(std::cos(a), std::sin(a), 0.0f), centre = feet + glm::vec3(0, 0, movement.sweep_lift());
            float len = 0.0f;
            while (len < 400.0f) {
                MoveWorld::Sweep s = world->sweep(centre + dir * len, centre + dir * (len + 10.0f),
                                                  movement.collision_radius());
                if (s.collided) break;
                len += 10.0f;
            }
            if (len > best) {
                best = len;
                yaw = a;
            }
        }
        log::info("movetest: facing yaw {:.2f}, {:.0f} units clear", yaw, best);
    }
    if (kind_ == "speed") {
        // Run 2 s, turn around, walk 4 s back.
        if (t >= 2.0f && t < 4.0f) keys.forward = true;
        if (t >= 4.5f && !turned_) {
            conn.set_running(false);
            yaw += 3.14159265f;
            turned_ = true;
        }
        if (t >= 5.0f && t < 9.0f) keys.forward = true;
        if (t >= 9.5f) {
            conn.set_running(true);
            done_ = true;
            log::info("movetest: speed done");
        }
    } else if (kind_ == "jump") {
        // 2.0 standing jump; 2.3 a second press (a jump is pending: ignored);
        // 3.2 a press within 1.5 s of the jump (refused); 5.0..6.5 running,
        // jump at 5.5.
        // Once, on the first frame at or after `at` (frames can be longer than 20 ms).
        auto press = [&](float at) { return t >= at && prev_drive_t_ < at; };
        if (press(2.0f) || press(2.3f) || press(3.2f) || press(5.5f)) {
            keys.jump = true;
            log::info("movetest: t={:.2f} jump pressed (feet z {:.3f})", t, feet.z);
        }
        if (t >= 5.0f && t < 6.5f) keys.forward = true;
        if (t >= 8.0f) {
            done_ = true;
            log::info("movetest: jump done");
        }
    } else if (kind_ == "fall") {
        if ((lifts_ == 0 && t >= 3.0f) || (lifts_ == 1 && t >= 9.0f)) {
            ++lifts_;
            movement.lift(height_);
            log::info("movetest: t={:.2f} lifted {:.1f} units (feet z {:.2f} -> {:.2f}), HP {}", t, height_, feet.z,
                      feet.z + height_, conn.player_status().hp);
        }
        if (t >= 15.0f) {
            done_ = true;
            log::info("movetest: fall done, HP {}/{}", conn.player_status().hp, conn.player_status().max_hp);
        }
    } else if (kind_ == "walk") {
        if (t >= 2.0f && t < 2.0f + walk_secs_) {
            yaw = walk_yaw_;
            pitch = 0.0f;
            keys.forward = true;
        }
        if (t >= 3.0f + walk_secs_) {
            done_ = true;
            log::info("movetest: walk done at ({:.3f}, {:.3f}, {:.3f})", feet.x, feet.y, feet.z);
        }
    } else if (kind_ == "wander") {
        if (t < 2.0f) return;
        int slot = static_cast<int>((t - 2.0f) / 1.5f);
        if (slot != escape_slot_) {
            escape_slot_ = slot;
            wander_seed_ = wander_seed_ * 1103515245u + 12345u;
            yaw = static_cast<float>((wander_seed_ >> 8) % 360u) * 3.14159265f / 180.0f;
            if ((wander_seed_ >> 20) % 4u == 0) keys.jump = true;
        }
        pitch = 0.0f;
        keys.forward = true;
        if (t >= 2.0f + wander_secs_) {
            done_ = true;
            log::info("movetest: wander done at ({:.2f}, {:.2f}, {:.2f}), {} stuck stretches", feet.x, feet.y, feet.z,
                      wander_stuck_);
        }
    } else if (kind_ == "escape") {
        // Slots of 2 s from t = 2: placed at the start, 0.3 s to settle, run 1.2 s, measured at 1.9 s.
        if (t < 2.0f) return;
        int slot = static_cast<int>((t - 2.0f) / 2.0f);
        float in = t - 2.0f - static_cast<float>(slot) * 2.0f;
        if (slot >= 16) {
            done_ = true;
            log::info("movetest: escape done");
            return;
        }
        if (slot != escape_slot_) {
            escape_slot_ = slot;
            if (!start_) start_ = feet;
            movement.place(*start_);
            escape_from_ = *start_;
            marked_ = false;
        }
        yaw = static_cast<float>(slot % 8) * 3.14159265f / 4.0f;
        pitch = 0.0f;
        float prev_in = prev_drive_t_ - 2.0f - static_cast<float>(slot) * 2.0f;
        if (slot >= 8 && in >= 0.3f && prev_in < 0.3f) keys.jump = true; // once, however long the frames
        if (in >= 0.3f && in < 1.5f) keys.forward = true;
        if (in >= 1.9f && !marked_) {
            marked_ = true;
            glm::vec3 d = feet - escape_from_;
            log::info("movetest: escape yaw {:3.0f}{}: moved {:.2f} (xy {:.2f}, z {:+.2f}) to ({:.2f}, {:.2f}, {:.2f})",
                      static_cast<float>(slot % 8) * 45.0f, slot >= 8 ? " jumping" : "", glm::length(d),
                      glm::length(glm::vec2(d)), d.z, feet.x, feet.y, feet.z);
        }
    } else if (kind_ == "swim") {
        if (!water_ && zone && t >= 2.0f) {
            // The nearest water (region type 5) at or below the feet, 8-unit grid, that a swimmer can reach: just
            // under the surface (open, in-world air 6 units up) with a floor more than 8 units below it. The BSP's water regions
            // also fill the space under a pool's floor, where the zone has no geometry; a swimmer placed there
            // is outside every surface and sinks out of the world.
            auto open_water = [&](const glm::vec3& p) {
                if ((zone->regions.lookup(p).flags & 0xff) != 5) return false;
                glm::vec3 air = p + glm::vec3(0, 0, 6.0f);
                if ((zone->regions.lookup(air).flags & 0xff) == 5 || !zone->regions.in_world(air)) return false;
                if (!world || !world->collision_loaded()) return true;
                float floor = world->floor_sweep(p, p - glm::vec3(0, 0, 300.0f), 0.5f);
                // Deep enough to swim: the floor at least 8 below (the body sits AvatarHeight above the feet).
                return floor > p.z - 299.0f && floor < p.z - 8.0f &&
                       zone->regions.in_world(glm::vec3(p.x, p.y, floor + 1.0f));
            };
            float best = 1e30f;
            for (float dx = -1500; dx <= 1500; dx += 8)
                for (float dy = -1500; dy <= 1500; dy += 8)
                    for (float dz = -120; dz <= 2; dz += 6) {
                        glm::vec3 p = feet + glm::vec3(dx, dy, dz);
                        float d = dx * dx + dy * dy;
                        if (d >= best || !open_water(p)) continue;
                        best = d;
                        water_ = p;
                    }
            if (water_) log::info("movetest: water at ({:.1f}, {:.1f}, {:.1f}), {:.0f} units away", water_->x, water_->y,
                                  water_->z, std::sqrt(best));
            else {
                log::info("movetest: no water within 1500 units");
                done_ = true;
                return;
            }
        }
        if (!water_) return;
        if (swim_start_ < 0.0f) {
            glm::vec3 to = *water_ - feet;
            yaw = std::atan2(to.y, to.x);
            keys.forward = true;
            if (body.body_wet > 4) {
                swim_start_ = t;
                log::info("movetest: t={:.2f} in water (body {}), feet z {:.2f}", t, body.body_wet, feet.z);
            } else if (t > 14.0f) {
                log::info("movetest: could not get into the water (at {:.1f}, {:.1f}, {:.1f})", feet.x, feet.y, feet.z);
                done_ = true;
            } else if (t > 10.0f && !placed_in_water_) {
                placed_in_water_ = true;
                // Walls in the way: put the player at the water's edge (a test
                // teleport; the server sees a position jump).
                log::info("movetest: did not reach the water (at {:.1f}, {:.1f}, {:.1f}); placing the player there",
                          feet.x, feet.y, feet.z);
                movement.place(*water_ + glm::vec3(0, 0, 2.0f));
            }
            return;
        }
        float s = t - swim_start_;
        keys.forward = s < 10.0f;
        pitch = s < 3.0f ? -0.6f : s < 7.0f ? 0.6f : 0.0f;
        if (s >= 11.0f) {
            pitch = 0.0f;
            done_ = true;
            log::info("movetest: swim done");
        }
    }
}

void MovementTest::observe(float t, const PlayerMovement::Frame& f, const PlayerMovement& movement,
                           const net::GameConnection& conn) {
    if (!active() || (kind_ == "observe" && done_)) return;
    const auto& body = movement.controller().body;
    if (trace_ && !done_)
        for (const auto& line : f.trace) log::info("trace t={:.3f}: {}", t, line);
    if (kind_ == "wander" && !done_ && t >= 2.0f) {
        if (wander_mark_t_ < 0.0f) {
            wander_mark_ = f.feet;
            wander_mark_t_ = t;
        } else if (glm::length(f.feet - wander_mark_) >= 1.0f) {
            wander_mark_ = f.feet;
            wander_mark_t_ = t;
        } else if (t - wander_mark_t_ >= 3.0f) {
            ++wander_stuck_;
            log::info("movetest: t={:.2f} stuck for 3 s at ({:.3f}, {:.3f}, {:.3f}) body {}", t, f.feet.x, f.feet.y,
                      f.feet.z, body.body_wet);
            for (const auto& line : f.trace) log::info("  {}", line);
            wander_mark_t_ = t;
        }
        if (t - last_log_ >= 1.0f) {
            log::info("movetest: t={:.2f} feet ({:.2f}, {:.2f}, {:.2f}) body {}", t, f.feet.x, f.feet.y, f.feet.z,
                      body.body_wet);
            last_log_ = t;
        }
    }
    if ((kind_ == "walk" || kind_ == "observe") && !done_ && t - last_log_ >= 0.25f) {
        log::info("movetest: t={:.2f} feet ({:.3f}, {:.3f}, {:.3f}) body {} v ({:.3f}, {:.3f}, {:.3f})", t, f.feet.x,
                  f.feet.y, f.feet.z, body.body_wet, body.velocity.x, body.velocity.y, body.velocity.z);
        last_log_ = t;
    }
    if (f.jumped) {
        air_start_ = t;
        ground_z_ = f.feet.z;
        max_z_ = f.feet.z;
        log::info("movetest: t={:.2f} OP_Jump sent, SpeedZ {:.3f}, endurance now {}", t, body.velocity.z,
                  conn.player_status().endurance);
    }
    if (air_start_ >= 0.0f) {
        max_z_ = std::max(max_z_, f.feet.z);
        if (f.landed && t > air_start_) {
            log::info("movetest: t={:.2f} landed: {:.3f} s in the air, {:.3f} units high", t, t - air_start_,
                      max_z_ - ground_z_);
            air_start_ = -1.0f;
        }
    }
    if (f.fall_damage)
        log::info("movetest: t={:.2f} IHaveFallen: SpeedZ {:.3f} at landing, damage {}", t, f.landing_speed,
                  *f.fall_damage);
    for (const auto& m : f.messages) log::info("movetest: chat '{}'", m);
    if (kind_ == "speed") {
        if (!marked_) {
            mark_ = f.feet;
            marked_ = true;
            last_log_ = t;
        }
        if (t - last_log_ >= 1.0f) {
            glm::vec2 d(f.feet.x - mark_.x, f.feet.y - mark_.y);
            log::info("movetest: t={:.2f} {:.2f} units in {:.3f} s ({:.2f}/s), SpeedRun {:.4f}, |v| {:.4f}/tick, {}", t,
                      glm::length(d), t - last_log_, glm::length(d) / (t - last_log_), body.speed_run,
                      glm::length(glm::vec2(body.velocity.x, body.velocity.y)), conn.running() ? "running" : "walking");
            const auto& ch = movement.last_character();
            const auto& fa = movement.last_facts();
            log::info("movetest:   run {:.4f} walk {:.4f} modifier {:.4f} weight {:.4f} (hp {}/{}, carried {} str {}), "
                      "SPA 3 {} 99 {} 271 {} 290 {}",
                      ch.run_speed, ch.walk_speed, ch.speed_multiplier, ch.weight_factor, fa.hp, fa.max_hp,
                      fa.carried, fa.strength, fa.spa3, fa.spa99, fa.spa271, fa.spa290);
            mark_ = f.feet;
            last_log_ = t;
        }
    } else if (kind_ == "swim" && swim_start_ >= 0.0f && !done_) {
        if (t - last_log_ >= 0.25f) {
            log::info("movetest: t={:.2f} feet ({:.1f}, {:.1f}, {:.2f}) body {} SpeedRun {:.3f} v ({:.3f}, {:.3f}, {:.3f})",
                      t, f.feet.x, f.feet.y, f.feet.z, body.body_wet, body.speed_run, body.velocity.x, body.velocity.y,
                      body.velocity.z);
            last_log_ = t;
        }
    } else if (kind_ == "fall") {
        if (conn.player_status().hp != last_hp_) {
            log::info("movetest: t={:.2f} HP {}/{}", t, conn.player_status().hp, conn.player_status().max_hp);
            last_hp_ = conn.player_status().hp;
        }
    }
}

} // namespace hailstone::app
