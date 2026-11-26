#pragma once

// The zone's collision as PlayerMovement's MoveWorld (app/player_movement.h): the static mesh, the doors and the
// BSP, with the vehicles and NPC bodies the player's sweep also meets. The remote spawns' contact probe
// (remote_collision_type) uses it too, set up with the same movement rules (apply_rules).

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "app/player_movement.h"
#include "app/world_objects.h"
#include "assets/regions.h"
#include "game/animated_objects.h"
#include "game/boats.h"
#include "game/collision.h"
#include "game/doors.h"
#include "game/npc_collision.h"
#include "game/swept_sphere.h"
#include "plugin/policy.h"

namespace hailstone::app {

// The zone's collision for PlayerMovement (MoveWorld): the static mesh, the
// doors and the BSP.
class ZoneMoveWorld final : public MoveWorld {
public:
    ZoneMoveWorld(const game::CollisionMesh* mesh, const WorldObjects* objects, const assets::ZoneRegions* regions)
        : mesh_(mesh), objects_(objects), regions_(regions) {}
    bool collision_loaded() const override { return mesh_ != nullptr; }
    std::optional<std::pair<glm::vec3, glm::vec3>> world_box() const override {
        return mesh_ ? mesh_->bounds() : std::nullopt;
    }
    Sweep sweep(const glm::vec3& from, const glm::vec3& to, float radius) const override {
        game::SphereSweep s = game::SphereSweep::make(from, to, radius);
        Sweep out;
        if (mesh_ && s.valid()) {
            const std::int32_t tri = mesh_->sweep_sphere_hit(s);
            out.ladder_hit = tri >= 0 && (mesh_->triangles()[static_cast<std::size_t>(tri)].flags & game::kTriangleLadder);
            if (contact_doors_ || contact_per_thread_) {
                // The contact probe: a door hit only matters after a ladder (it would set the switch type over the
                // ladder's), so the doors are swept only then, into the probe's own buffer.
                if (passable_ladders_ && !out.ladder_hit) out.ladder_hit = mesh_->touches_passable_ladder(s);
                if (out.ladder_hit && objects_) {
                    thread_local std::vector<WorldObjects::DoorHit> own; // set_contact_probe_per_thread's
                    std::vector<WorldObjects::DoorHit>& doors = contact_doors_ ? *contact_doors_ : own;
                    doors.clear();
                    out.door_hit = objects_->sweep_sphere(s, &doors, pass_doors_.empty() ? nullptr : &pass_doors_);
                    for (const auto& h : doors) out.door_hits.push_back({h.id, h.type});
                }
                out.out = game::sweep_output(s);
                out.collided = s.collided;
                return out;
            }
            if (objects_) {
                thread_local std::vector<WorldObjects::DoorHit> hits; // (kept: several sweeps a movement step)
                hits.clear();
                out.door_hit = objects_->sweep_sphere(s, &hits, pass_doors_.empty() ? nullptr : &pass_doors_);
                for (const auto& h : hits) out.door_hits.push_back({h.id, h.type});
            }
            // The zone objects' moving parts where their animation has them (game/animated_objects.h).
            if (animated_) animated_->sweep_sphere(s);
            // A boat's hull and rails are walls too (game/boats.h), not only its deck a floor.
            sweep_vehicles(s, radius);
            // Hailstone: live NPCs are walls to the player (game/npc_collision.h), never floors or ceilings.
            if (npcs_) game::sweep_npcs(s, *npcs_);
            // Hailstone: a passable ladder (Cabilis's LADDERCB101) met short of the stop is climbed, though it
            // never stops the sphere (CollisionMesh::touches_passable_ladder).
            if (passable_ladders_ && !out.ladder_hit) out.ladder_hit = mesh_->touches_passable_ladder(s);
        }
        out.out = game::sweep_output(s);
        out.collided = s.collided;
        return out;
    }
    Sweep world_sweep(const glm::vec3& from, const glm::vec3& to, float radius) const override {
        game::SphereSweep s = game::SphereSweep::make(from, to, radius);
        s.floor_only = true;
        Sweep out;
        // GetCeilingHeight's actor filter (0x4764d0) refuses switch actors (vf30 == 3): the doors never count.
        if (mesh_ && s.valid()) mesh_->sweep_sphere(s);
        out.out = game::sweep_output(s);
        out.collided = s.collided;
        return out;
    }
    bool in_world(const glm::vec3& p) const override { return !regions_ || regions_->in_world(p); }
    std::optional<float> floor_below(const glm::vec3& p, float step_up) const override {
        std::optional<float> floor = mesh_ ? mesh_->floor_below(p, step_up) : std::nullopt;
        if (objects_) {
            // Doors, lifts and platforms are floors too.
            auto door_floor = objects_->floor_below(p, step_up);
            if (door_floor && (!floor || *door_floor > *floor)) floor = door_floor;
        }
        if (animated_) {
            auto moving_floor = animated_->floor_below(p, step_up);
            if (moving_floor && (!floor || *moving_floor > *floor)) floor = moving_floor;
        }
        return floor;
    }
    float floor_sweep(const glm::vec3& from, const glm::vec3& to, float radius, std::uint32_t* vehicle = nullptr,
                      std::vector<FloorSwitchHit>* switches = nullptr) const override {
        game::SphereSweep s = game::SphereSweep::make(from, to, radius);
        s.floor_only = true;
        if (mesh_ && s.valid()) {
            mesh_->sweep_sphere(s);
            if (objects_) {
                std::vector<WorldObjects::DoorHit> hits;
                objects_->sweep_floor_switches(s, switches ? &hits : nullptr);
                // CollsnCallbackForFloor's distance: t / (1 / |d|).
                const float length = glm::length(s.delta);
                if (switches)
                    for (const auto& h : hits) switches->push_back({h.id, h.type, h.t * length});
            }
            // The vehicles' models (a player actor whose spawn has vf50): the nearest hit's spawn is the Vehicle.
            if (std::uint32_t v = sweep_vehicles(s, radius); v && vehicle) *vehicle = v;
        }
        return game::sweep_output(s).position.z;
    }
    // The vehicle under a point: a short drop onto the decks only (another spawn's update on a boat, game/boats.h).
    std::uint32_t vehicle_under(const glm::vec3& p) const {
        game::SphereSweep s = game::SphereSweep::make(p + glm::vec3(0, 0, 2.0f), p - glm::vec3(0, 0, 8.0f), 0.5f);
        s.floor_only = true;
        return s.valid() ? sweep_vehicles(s, 0.5f) : 0;
    }
    std::vector<std::uint8_t> touch_overlap(const glm::vec3& at, float radius) const override {
        std::vector<std::uint8_t> out;
        game::SphereSweep s = game::SphereSweep::make(at, at, radius);
        if (!objects_ || !s.valid()) return out;
        std::vector<WorldObjects::DoorHit> hits;
        objects_->touch_overlap(s, hits);
        for (const auto& h : hits) out.push_back(h.id);
        return out;
    }
    // The zone objects that play their own animation, as posed for this step (game/animated_objects.h); null: none.
    void set_animated(const game::AnimatedObjects* animated) { animated_ = animated; }
    // The NPC bodies the movement sweep stops at (ClientApp::update_npc_bodies); null: none.
    void set_npcs(const std::vector<game::NpcBody>* bodies) { npcs_ = bodies; }
    // Hailstone: walking into a ladder whose polygons are all passable climbs it (hailstone.cfg [movement]
    // passable_ladders); off, as in the original, it is only mounted by a click from the top.
    void set_passable_ladders(bool on) { passable_ladders_ = on; }
    // Doors the sweep goes through (Navigator::passes_open_doors: those not shut, while /nav or /stick moves).
    void set_passable_doors(std::vector<std::uint8_t> ids) { pass_doors_ = std::move(ids); }
    // The movement rules every body's sweep follows, the player's and the other spawns' contact probes alike:
    // passable ladders unless the server's hailstone.cfg turns them off, and, while /nav or /stick moves the player
    // through open doors (`pass_open_doors`), the doors that are not shut.
    void apply_rules(const std::optional<plugin::Rules>& rules, const std::vector<game::Door>& doors,
                     bool pass_open_doors) {
        set_passable_ladders(!(rules && rules->configured && !rules->passable_ladders));
        std::vector<std::uint8_t> open;
        if (pass_open_doors)
            for (const game::Door& d : doors)
                if (d.state != game::kSwitchClosed) open.push_back(d.id);
        set_passable_doors(std::move(open));
    }
    // The rules of the other spawns' contact probes: the passable ladders as apply_rules sets them, but never the
    // doors the player's /nav or /stick passes (those are the player's own; another spawn meets every door).
    void apply_remote_rules(const std::optional<plugin::Rules>& rules) { apply_rules(rules, {}, false); }
    const std::vector<std::uint8_t>& passable_doors() const { return pass_doors_; }
    // The other spawns' contact probe (remote_collision_type): sweep() then answers only what the contact needs.
    // Only a ladder gives a contact the animation shows (CollidingType 6), and a door hit after it overrides it
    // with the switch type (3), so the doors are swept only after a ladder hit, into `door_buffer` (kept by the
    // caller across probes, so a probe does not allocate); the vehicles and NPC bodies are not swept. Null: off.
    void set_contact_probe(std::vector<WorldObjects::DoorHit>* door_buffer) {
        contact_doors_ = door_buffer;
        contact_per_thread_ = false;
    }
    // The contact probe with a buffer of each calling thread's own, so one world serves probes run on several
    // threads at once (WorldState::smooth steps the spawns on the worker threads); sweep() is then safe to call so.
    void set_contact_probe_per_thread() {
        contact_doors_ = nullptr;
        contact_per_thread_ = true;
    }
    // The vehicle spawns' models as they were drawn (PlayerClient_IsVehicleRace), for the floor test and the walls;
    // `standing_on` is the vehicle the player stands on, which wins a tie between overlapping decks (game/boats.h).
    using Vehicle = game::VehicleHull;
    void set_vehicles(const std::vector<Vehicle>* v, std::uint32_t standing_on = 0) {
        vehicles_ = v;
        standing_on_ = standing_on;
    }
    // The sweep against the vehicles near it; the id of the one whose triangle stopped it nearest (0: none did).
    std::uint32_t sweep_vehicles(game::SphereSweep& s, float radius) const {
        return vehicles_ ? game::sweep_hulls(s, *vehicles_, radius, standing_on_) : 0;
    }
    // A probe of the helm's boat (game/boats.h hull_probes): stopped by the zone, its doors and objects or another
    // vehicle short of `to`; a probe that touches something where it starts is not held by it.
    bool hull_blocked(const glm::vec3& from, const glm::vec3& to, float radius, std::uint32_t boat) const {
        game::SphereSweep s = game::SphereSweep::make(from, to, radius);
        if (!mesh_ || !s.valid()) return false;
        mesh_->sweep_sphere(s);
        if (objects_) objects_->sweep_sphere(s, nullptr);
        if (animated_) animated_->sweep_sphere(s);
        if (vehicles_) game::sweep_hulls(s, *vehicles_, radius, 0, boat);
        return s.collided && s.t > 0.0f;
    }
    std::optional<float> ceiling_above(const glm::vec3& p) const override {
        return mesh_ ? mesh_->ceiling_above(p) : std::nullopt;
    }

private:
    const game::CollisionMesh* mesh_;
    const WorldObjects* objects_;
    const assets::ZoneRegions* regions_;
    const std::vector<Vehicle>* vehicles_ = nullptr;
    std::uint32_t standing_on_ = 0;
    const std::vector<game::NpcBody>* npcs_ = nullptr;
    const game::AnimatedObjects* animated_ = nullptr;
    bool passable_ladders_ = false;
    std::vector<std::uint8_t> pass_doors_;
    std::vector<WorldObjects::DoorHit>* contact_doors_ = nullptr;
    bool contact_per_thread_ = false;
};

} // namespace hailstone::app
