#include "game/world_state.h"

#include "game/mechanics/remote_spawns.h"

#include <cctype>
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

#include "core/log.h"
#include "core/worker_pool.h"
#include "game/boats.h"
#include "game/coords.h"
#include "game/race_models.h"

namespace hailstone::game {

std::string display_name(const std::string& name) {
    // CEverQuest::trimName (game/social.h trim_name) drops '#' too: the server's "#Name" spawns show as "Name".
    std::string out = name.substr(std::min(name.find_first_not_of('#'), name.size()));
    while (!out.empty() && std::isdigit(static_cast<unsigned char>(out.back()))) out.pop_back();
    for (char& c : out)
        if (c == '_') c = ' ';
    return out;
}

bool display_name_iequals(std::string_view name, std::string_view other) {
    // The same trimming as display_name: leading '#', then trailing digits, '_' compared as ' '.
    std::size_t begin = std::min(name.find_first_not_of('#'), name.size()), end = name.size();
    while (end > begin && std::isdigit(static_cast<unsigned char>(name[end - 1]))) --end;
    if (end - begin != other.size()) return false;
    for (std::size_t i = 0; i < other.size(); ++i) {
        const char c = name[begin + i] == '_' ? ' ' : name[begin + i];
        if (ascii_lower(c) != ascii_lower(other[i])) return false;
    }
    return true;
}

std::optional<ElementalSwap> luclin_elemental_swap(std::uint16_t race, std::uint8_t texture, bool luclin_elementals) {
    constexpr std::uint16_t kElemental = 75, kEarth = 209, kAir = 210, kWater = 211, kFire = 212;
    if (luclin_elementals) {
        if (race != kElemental) return std::nullopt;
        switch (texture) {
        case 0: return ElementalSwap{kEarth, 0};
        case 1: return ElementalSwap{kFire, 0};
        case 2: return ElementalSwap{kWater, 0};
        default: return ElementalSwap{kAir, 0};
        }
    }
    switch (race) {
    case kEarth: return ElementalSwap{kElemental, 0};
    case kFire: return ElementalSwap{kElemental, 1};
    case kWater: return ElementalSwap{kElemental, 2};
    case kAir: return ElementalSwap{kElemental, 3};
    default: return std::nullopt;
    }
}

namespace {
// Spawns a worker steps at a time in WorldState::smooth: a few hundred probes each.
constexpr std::size_t kSmoothGrain = 16;
// How far a spawn at rest may settle onto the floor (see WorldState::smooth).
constexpr float kRestSettleReach = 5.0f;
// Hailstone (WorldState::ground_npcs): how far over its floor an NPC may stand and still be drawn on it. Higher
// up, the server more likely means it (a ledge or a floor the zone files do not have), so it is left there.
constexpr float kGroundNpcReach = 50.0f;
// Lower than this over its floor it counts as standing on it. The landing check settles a spawn only while its last
// update is at most 12 s old, so one standing still longer keeps a smaller gap the server gave it.
constexpr float kGroundNpcSlack = 0.25f;

// How far below its position to draw a spawn that WorldState::ground_npcs puts on the floor (0: where it is);
// nothing when there is no floor under it to measure against.
std::optional<float> ground_drop(const Spawn& s, const WorldState::FloorFn& floor, const WorldState::RegionFn& region) {
    // NPCs and corpses on the ground only: not flying or levitating ones, a rider, or one riding a boat. Flymode 3
    // is EQEmu's GravityBehavior::Water, which it gives NPCs that may swim (a spiderling in East Commonlands, seen
    // live): out of the water they stand on the ground, and the region test below leaves those in the water alone.
    if (!(s.is_npc || s.is_corpse) || (s.fly_mode != 0 && s.fly_mode != 3) || s.mount_id || s.on_vehicle) return 0.0f;
    auto f = floor(s.position);
    if (!f) return std::nullopt;
    const float z = *f + s.heights.avatar_height;
    const float drop = s.position.z - z;
    if (!(drop > kGroundNpcSlack && drop <= kGroundNpcReach)) return 0.0f;
    // Not one in water or lava, nor one over it (a fish, or a spawn on a dock the zone files leave out).
    if (region && (region(s.position) != 0 || region(glm::vec3(s.position.x, s.position.y, z)) != 0)) return 0.0f;
    return drop;
}
} // namespace

void WorldState::smooth_vehicles(float dt, double now) {
    const float one = dt * 1000.0f;
    const std::span<const float> steps = physics_steps ? std::span<const float>(*physics_steps) : std::span<const float>(&one, 1);
    for (auto& [id, s] : spawns) {
        if (id == player_id || id == controlled_id || id == driven_id || !is_vehicle_race(s.race, s.gender)) continue;
        const bool fresh = now - s.update_time <= mechanics::kSpawnStaleMs / 1000.0;
        step_vehicle(s, dt, fresh, steps);
        if (fresh && !steps.empty() && physics_step_time_ms) s.physics_step_ms = physics_step_time_ms;
    }
    vehicles_smoothed = true;
}

void WorldState::smooth(float dt, double now, const FloorFn& floor, const RegionFn& region,
                        const VehicleFn& vehicle_under, const FloorFn& ground_floor, const RemoteCollisionFn& collision,
                        bool ground_floor_includes_zone) {
    const FloorFn& grounding = ground_floor ? ground_floor : floor;
    if (!vehicles_smoothed) smooth_vehicles(dt, now);
    vehicles_smoothed = false;
    float k = 1.0f - std::exp(-dt * 12.0f);
    const float one = dt * 1000.0f;
    const std::span<const float> steps = physics_steps ? std::span<const float>(*physics_steps) : std::span<const float>(&one, 1);
    // Where the contact probes' range is measured from: the body the player moves (its mount while riding).
    std::optional<glm::vec3> probe_from;
    if (auto me = spawns.find(controlled_id ? controlled_id : player_id); me != spawns.end())
        probe_from = me->second.position;
    // The vehicles' riders here, first: each reads the vehicle it rides, another spawn. Every other spawn reads and
    // writes only its own entry, and the probes only read the zone and its doors, which nothing changes meanwhile:
    // those are stepped on the worker threads (core::parallel_for; inline without a pool), each spawn wholly on one,
    // so the result is the same whichever thread steps it. The callbacks are told so (world_state.h).
    smooth_scratch.clear();
    for (auto& [id, s] : spawns) {
        if (id != player_id && id != controlled_id && is_vehicle_race(s.race, s.gender)) continue;
        // A spawn an update put on a vehicle's deck rides it (game/boats.h) until an update puts it elsewhere.
        if (id != player_id && id != controlled_id && !s.mount_id) {
            if (vehicle_under && s.vehicle_checked != s.update_time) {
                s.vehicle_checked = s.update_time;
                std::uint32_t v = vehicle_under(s.position);
                auto vit = v ? spawns.find(v) : spawns.end();
                if (vit != spawns.end()) attach_rider(s, v, vit->second);
                else s.on_vehicle = 0;
            }
            if (s.on_vehicle) {
                auto vit = spawns.find(s.on_vehicle);
                if (vit != spawns.end()) {
                    const bool fresh = now - s.update_time <= mechanics::kSpawnStaleMs / 1000.0;
                    step_rider(s, vit->second, dt, fresh, steps);
                    if (fresh && !steps.empty() && physics_step_time_ms) s.physics_step_ms = physics_step_time_ms;
                    continue;
                }
                s.on_vehicle = 0;
            }
        }
        smooth_scratch.push_back({id, &s});
    }
    auto step = [&](std::uint32_t id, Spawn& s) {
        // The original steps other spawns while their last update is at most 12 s old (AdvanceSpawn), but not the
        // local or the controlled player (the local player's mount, which the player's controls move).
        // The zone floor the last physics step settled this spawn on (its position.z is then floor + AvatarHeight),
        // NaN if none. The ground_npcs short-cut below uses it only when the grounding callback is known to include
        // that floor; NaN fails its isfinite check.
        float landed_floor = std::numeric_limits<float>::quiet_NaN();
        if (id != player_id && id != controlled_id && now - s.update_time <= mechanics::kSpawnStaleMs / 1000.0) {
            // Each step of the frame (core/step_clock.h) steps the body and lands it, as the original's frames do.
            mechanics::RemoteBody b{s.position, s.velocity, s.heading, s.speed_heading, s.speed_run};
            for (float ms : steps) {
                const glm::vec3 before = b.position;
                mechanics::step_remote_spawn(b, ms, s.is_npc);
                // DoCollisionMovement resets +0x24 when moving, and actor hits set it again. A still spawn keeps
                // its last contact. Riders and NoCollide bodies do not run this sweep. The current remote
                // predictor leaves vertical integration to server updates, but a vertical move still probes.
                if (collision && !s.mount_id && !s.is_corpse &&
                    (b.position != before || b.velocity.z != 0.0f)) {
                    const bool no_collide = std::find(s.properties.begin(), s.properties.end(), 0x64u) != s.properties.end();
                    // Hailstone: past remote_probe_range (not drawn) the sweep is not run and the moving spawn has
                    // no contact, as DoCollisionMovement's reset leaves it when nothing is hit.
                    const bool near = !probe_from || remote_probe_range <= 0.0f ||
                                      glm::distance(b.position, *probe_from) <= remote_probe_range;
                    s.colliding_type = no_collide || !near ? 0 : collision(s, before, b.position);
                }
                // CollisionPass's landing check (LandingAndFallCheck) runs with every step, so a spawn stands at
                // floor + AvatarHeight (feet = z - AvatarHeight) at rest too: the server places NPCs with its own
                // per-race offsets (EQEmu's GetZOffset: 0.7 x 0.2 x size for a dwarf), not the client's AvatarHeight.
                // Gravity takes a grounded spawn down to the floor; a flying or levitating one is only kept out of it.
                // A spawn at rest settles only onto a floor within kRestSettleReach; farther (an EQEmu map that puts it
                // in the air) it stays at the server's z, unless ground_npcs draws it lower.
                if (floor) {
                    const bool moved = b.position != before;
                    landed_floor = std::numeric_limits<float>::quiet_NaN(); // only the last step's landing counts
                    if (auto f = floor(b.position)) {
                        float z = *f + s.heights.avatar_height;
                        if (moved) {
                            b.position.z = z;
                            landed_floor = *f;
                        } else if (s.fly_mode == 0 ? std::fabs(z - b.position.z) <= kRestSettleReach : z > b.position.z) {
                            b.position.z = z;
                            landed_floor = *f;
                        }
                    }
                }
            }
            s.velocity = b.velocity;
            s.heading = b.heading;
            s.position = b.position;
            if (!steps.empty() && physics_step_time_ms) s.physics_step_ms = physics_step_time_ms;
            // CollisionPass after the step: PlayerClient::sub_00506d20 @0x506d20 takes the region at the spawn's z
            // as its body environment (+0xa2, BodyEnvironmentChange) and CheckRegionChange @0x508e10 the region at
            // the feet + 0.25 (z + 0.25 - (the collision height - AvatarHeight)) as its feet environment (+0xa1,
            // FeetEnvironmentChange), so other spawns swim and tread water as the local player does.
            if (region) {
                s.body_wet = region(s.position);
                s.feet_wet = region(s.position - glm::vec3(0.0f, 0.0f, s.heights.avatar_height - 0.25f));
            }
        }
        glm::vec3 target = s.position;
        // Hailstone (ground_npcs): measured again when the position changes, after the option is turned on and after
        // a zone's floor is loaded (reground). With no floor under it yet, it is measured again next frame.
        if (id != player_id && id != controlled_id && ground_npcs && grounding) {
            if (s.ground_probe_at != s.position) {
                // Hailstone: a landing on a zone floor f settles the spawn at f + AvatarHeight. If the grounding
                // callback is known to include that same zone floor, it returns at least f here (a higher door or
                // placed-object floor only raises it), so ground_drop's drop = f - grounding <= 0: never the
                // positive drop over 0.25 that gets drawn. Its zone-mesh query is then redundant. Anything else -
                // no landing, no floor, a stale or unstepped spawn, a floor outside the query's reach, bad numbers
                // or a callback that does not promise inclusion - still takes the full query.
                const bool skip = ground_floor_includes_zone && std::isfinite(landed_floor) &&
                                  std::isfinite(s.position.z) && std::isfinite(s.heights.avatar_height) &&
                                  landed_floor <= s.position.z + kGroundingFloorReach;
                if (skip) {
                    // A zero drop would have been measured here: remember the probe the same way ground_drop does.
                    s.ground_drop = 0.0f;
                    s.ground_probe_at = s.position;
                } else {
                    const auto drop = ground_drop(s, grounding, region);
                    s.ground_drop = drop.value_or(0.0f);
                    s.ground_probe_at = drop ? s.position : glm::vec3(std::numeric_limits<float>::quiet_NaN());
                }
            }
            target.z -= s.ground_drop;
        } else {
            s.ground_drop = 0.0f;
            s.ground_probe_at = glm::vec3(std::numeric_limits<float>::quiet_NaN());
        }
        // A position that is not a number (a bad update) is not followed: the spawn stays drawn where it last was.
        if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) return;
        // The local player and its mount are drawn where the camera puts them (set_local_drawn), else where they are.
        if (id != 0 && (id == player_id || id == controlled_id)) {
            if (!local_drawn || !s.display_valid || (eye_id && id == player_id)) {
                s.display_position = target;
                s.display_heading = s.heading;
                s.display_valid = true;
            }
            return;
        }
        // Teleports and new spawns snap, as does a drawn place or heading that is not a number (the test is written so
        // NaN fails it), so it recovers at the next good update.
        if (!s.display_valid || !(glm::distance(s.display_position, target) <= 50.0f) ||
            !std::isfinite(s.display_heading)) {
            s.display_position = target;
            s.display_heading = s.heading;
            s.display_valid = true;
            return;
        }
        s.display_position += (target - s.display_position) * k;
        float dh = std::fmod(s.heading - s.display_heading + 768.0f, 512.0f) - 256.0f; // shortest way round
        s.display_heading = std::fmod(s.display_heading + dh * k + 512.0f, 512.0f);
    };
    core::parallel_for(smooth_scratch.size(), kSmoothGrain, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) step(smooth_scratch[i].first, *smooth_scratch[i].second);
    });
    // PhysicsObject::vf8 @0x59e170 with each step of a rider: it takes its mount's position and heading, SpeedRun 0
    // and (unless +0x163) no velocity. The rider is drawn where its mount is. A rider of a rider takes the place of the
    // chain's bottom (a spawn that rides nothing, or the local player, which this loop does not move), so it is not a
    // frame behind when the map's order copies it before its own mount; a cycle or an absurd chain takes the direct
    // mount, as before.
    constexpr int kMaxMountChain = 16;
    auto carrier = [&](const Spawn& rider) -> const Spawn* {
        auto m = rider.mount_id ? spawns.find(rider.mount_id) : spawns.end();
        if (m == spawns.end()) return nullptr;
        auto at = m;
        for (int depth = 0;; ++depth) {
            if (at->first == player_id || !at->second.mount_id) return &at->second;
            auto next = spawns.find(at->second.mount_id);
            if (next == spawns.end()) return &at->second;
            if (depth == kMaxMountChain) return &m->second;
            at = next;
        }
    };
    for (auto& [id, s] : spawns) {
        const Spawn* m = carrier(s);
        if (!m || id == player_id) continue;
        s.position = m->position;
        s.heading = m->heading;
        s.speed_run = 0.0f;
        s.velocity = glm::vec3(0.0f);
        s.display_position = m->display_position;
        s.display_heading = m->display_heading;
        s.display_valid = m->display_valid;
    }
    // Hailstone (Spawn::stalled): after the riders take their mounts' places, so a rider stalls with its mount.
    for (auto& [id, s] : spawns) {
        if (id == player_id || id == controlled_id) {
            s.stalled = false;
            continue;
        }
        // The test is written so a position that is not a number moves the anchor (and never stalls).
        if (!(glm::distance(s.position, s.stall_anchor) <= kStallReach)) {
            s.stall_anchor = s.position;
            s.stall_since = now;
        }
        s.stalled = now - s.stall_since >= kStallSeconds;
    }
    local_drawn = false;
}

void WorldState::set_local_drawn(const glm::vec3& position, float heading) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        !std::isfinite(heading))
        return;
    for (std::uint32_t id : {player_id, controlled_id}) {
        if (eye_id && id == player_id) continue;
        if (id == 0) continue;
        auto it = spawns.find(id);
        if (it == spawns.end()) continue;
        it->second.display_position = position;
        it->second.display_heading = heading;
        it->second.display_valid = true;
        local_drawn = true;
    }
}

void WorldState::reground() {
    for (auto& [id, s] : spawns) s.ground_probe_at = glm::vec3(std::numeric_limits<float>::quiet_NaN());
}

void WorldState::set_heights(Spawn& s, float view_offset, float anim_scale) const {
    game::set_heights(s.heights, height_inputs(height_tables.get(), s.race, s.gender, old_model_scale), view_offset,
                      anim_scale);
}

void WorldState::change_height(Spawn& s, float size, float view_offset, float anim_scale) {
    // The mount chain: the spawn, its mount (EQPlayer::GetMount's vf3c), that one's mount, ... (a cycle stops).
    std::vector<HeightLink> chain;
    for (Spawn* at = &s; at && chain.size() < spawns.size();) {
        HeightLink link;
        link.heights = &at->heights;
        link.inputs = height_inputs(height_tables.get(), at->race, at->gender, old_model_scale);
        link.type = at->is_corpse ? 2 : (at->is_npc ? 1 : 0);
        link.has_actor = at->actor_made;
        link.mount_race = is_mount_race(at->race);
        chain.push_back(link);
        auto next = at->mount_id ? spawns.find(at->mount_id) : spawns.end();
        at = next == spawns.end() ? nullptr : &next->second;
    }
    game::change_height(std::span(chain), size, view_offset, anim_scale);
}

void WorldState::set_old_model_scale(bool on) {
    if (on == old_model_scale) return;
    old_model_scale = on;
    for (auto& [id, s] : spawns) set_heights(s, 0.0f, 1.0f);
}

std::vector<std::uint32_t> WorldState::make_actors(const ActorModelFn& actor_model) {
    std::vector<std::uint32_t> made;
    if (!actor_model) return made;
    for (auto& [id, s] : spawns) {
        if (s.actor_made) continue;
        // The elemental swap (luclin_elemental_swap): SetRace, then the model lookup. With UseLuclinElementals on,
        // a typed model that is not loaded leaves race 75 (0x48f720: SetRace(0x4b) and ELE_ACTORDEF), and only a
        // found one takes texture 0 and HeadType 0.
        const std::uint16_t sent_race = s.race;
        std::optional<ActorModel> m;
        if (auto swap = luclin_elemental_swap(s.race, s.appearance.texture, luclin_elementals)) {
            s.race = swap->race;
            m = actor_model(s);
            if (!m) {
                s.race = sent_race; // tried again once an actor can be made
                continue;
            }
            if (luclin_elementals && m->human_fallback) {
                s.race = sent_race;
                m = actor_model(s);
            } else {
                s.appearance.texture = swap->texture;
                if (luclin_elementals) s.appearance.helm = 0;
            }
        } else {
            m = actor_model(s);
        }
        if (!m) continue;
        s.actor_made = true;
        s.tunic_point = m->tunic_point;
        s.human_actor = m->human_fallback;
        made.push_back(id);
        // BuildActorForSpawn (0x48f837 / 0x48f85e): HUF_ACTORDEF / HUM_ACTORDEF, then SetRace(1) unless a mount
        // race; the gender is kept.
        if (m->human_fallback && !is_mount_race(s.race)) s.race = 1;
        // The player's and their pets' looks, for reports of models drawn wrong.
        if (id == player_id || (player_id != 0 && s.pet_owner_id == player_id)) {
            const Appearance& a = s.appearance;
            auto list = [](const std::array<std::uint32_t, 9>& v) {
                std::string out;
                for (std::uint32_t x : v) out += (out.empty() ? "" : ",") + std::to_string(x);
                return out;
            };
            log::info("appearance {}{}: race {}{}{} gender {} class {} size {:.1f} texture {} helm {} face {} hair {} / {} "
                      "beard {} / {} eyes {} / {} heritage {} tattoo {} details {} material [{}] tint [{}] hero_forge [{}] "
                      "npc_tint {}",
                      s.name, id == player_id ? "" : " (pet)", sent_race, m->human_fallback ? " (as human)" : "",
                      s.race != sent_race && !m->human_fallback ? " (as " + std::to_string(s.race) + ")" : std::string(),
                      s.gender,
                      s.class_id, s.size, a.texture, a.helm, a.face, a.hair_style, a.hair_color, a.beard, a.beard_color,
                      a.eye_color1, a.eye_color2, a.drakkin_heritage, a.drakkin_tattoo, a.drakkin_details,
                      list(a.material), list(a.tint), list(a.hero_forge), a.npc_tint);
        }
        if (!m->tunic_point) continue;
        float view = s.heights.view_height;
        if (auto to = actor_height_fixup(s.race, s.heights.height)) change_height(s, *to, 0.0f, 1.0f);
        s.heights.view_height = view;
    }
    std::sort(made.begin(), made.end());
    return made;
}

} // namespace hailstone::game
