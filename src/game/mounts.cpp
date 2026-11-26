// Riding: the rider-mount link (game/mounts.h).
#include "game/mounts.h"

#include <algorithm>
#include <cctype>

#include "game/eq_names.h"
#include "game/race_models.h"
#include "game/social.h"
#include "game/world_state.h"

namespace hailstone::game {

namespace {

constexpr std::string_view kMountSuffix = "`s_Mount"; // 0x9d77e8 / 0x9c58b8
constexpr std::uint32_t kStrNoRoom = 0xfe3;            // 4067
constexpr std::uint32_t kStrCannotRide = 0x1594;       // 5524
constexpr double kRoomFactor = 3.0;                    // 0x9c3920: AvatarHeight x 3
constexpr float kViewOnMount = 0.85f;                  // 0x9d7308

bool equal_no_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// PlayerManagerClient::GetSpawnByName @0x599b40: the spawn whose name equals `name` ignoring case.
Spawn* spawn_by_name(WorldState& w, std::string_view name) {
    if (name.empty()) return nullptr;
    for (auto& [id, s] : w.spawns)
        if (equal_no_case(s.name, name)) return &s;
    return nullptr;
}

Spawn* spawn_by_id(WorldState& w, std::uint32_t id) {
    if (id == 0) return nullptr;
    auto it = w.spawns.find(id);
    return it == w.spawns.end() ? nullptr : &it->second;
}

// PlayerManagerClient::GetPlayerFromPartialName @0x599710 over the spawn list (in id order here).
Spawn* spawn_by_partial_name(WorldState& w, std::string_view word) {
    std::vector<std::uint32_t> ids;
    std::vector<std::string> names;
    for (const auto& [id, s] : w.spawns) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (std::uint32_t id : ids) names.push_back(w.spawns.at(id).name);
    auto at = player_from_partial_name(word, names);
    return at ? &w.spawns.at(ids[*at]) : nullptr;
}

bool is_npc_type(const Spawn& s) { return s.is_npc && !s.is_corpse; } // PlayerBase +0x125 == 1

// PlayerClient_AttachRiderToMount @0x58fcd0: a mount race needs a seat dag. Hailstone lets playable forms use
// that seat with either model generation. For other forms the stock rider checks apply with illusion_mounts off.
bool attach_rider(const Spawn& rider, const Spawn& mount, std::int32_t point, MountQueries& q, bool illusion_mounts) {
    if (point == -1) {
        if (!is_mount_race(mount.race)) return false;
        const bool playable_form = !rider.is_npc && !rider.is_corpse && is_playable_race(rider.race);
        if (!illusion_mounts && !playable_form && (riding_blocked(rider) || human_actor_mount(rider))) return false;
        if (!q.mount_point(rider, mount)) return false;
    }
    return true;
}

} // namespace

std::string mount_name_for(std::string_view rider) {
    // SafeSnprintf(name, 0x40, ...): at most 63 characters.
    std::string name = std::string(rider) + "`s_Mount00";
    if (name.size() > 0x3f) name.resize(0x3f);
    return name;
}

std::optional<std::string> rider_name_for_mount(std::string_view mount) {
    if (mount.size() < 9 || mount.find(kMountSuffix) == std::string_view::npos) return std::nullopt;
    if (mount.size() < 10) return std::string();
    return std::string(mount.substr(0, mount.size() - 10));
}

bool riding_blocked(const Spawn& s) {
    // 0x58dde0: the race shown (vf88) 0x16f, 0x20a or 1 always rides; otherwise an actor (+0x1024) whose
    // definition's +0x34 is 0 cannot.
    if (s.race == 0x16f || s.race == 0x20a || s.race == 1) return false;
    return s.actor_made && !s.tunic_point;
}

bool human_actor_mount(const Spawn& s) { return is_mount_race(s.race) && s.actor_made && s.human_actor; }

Spawn* controlled_spawn(WorldState& w) {
    if (Spawn* c = spawn_by_id(w, w.controlled_id)) return c;
    return spawn_by_id(w, w.player_id);
}

const Spawn* controlled_spawn(const WorldState& w) { return controlled_spawn(const_cast<WorldState&>(w)); }

void dismount(WorldState& w, Spawn& rider, MountEvents& ev) {
    Spawn* mount = spawn_by_id(w, rider.mount_id);
    if (!mount) {
        // A mount that is gone (the original's GetMount()->vf44 is null): nothing else changes. The original's link
        // is a pointer that went with the mount; here it is an id, which EQEmu gives the next spawn, so it is
        // cleared rather than left to seat the rider on whatever spawn takes the id.
        rider.mount_id = 0;
        return;
    }
    const bool local = rider.id == w.player_id;
    // AttachRiderToMount(mount, mount race ? -1 : 3) on its own mount: with an actor the rider takes the mount's
    // +0x64..+0x7c (position, velocity, SpeedRun) and the mount's dag 0xe lets go of it.
    if (rider.actor_made) {
        rider.position = mount->position;
        rider.velocity = mount->velocity;
        rider.speed_run = mount->speed_run;
    }
    if (is_mount_race(mount->race)) {
        mount->speed_run = 0.0f;     // +0x7c
        mount->speed_heading = 0.0f; // +0x8c
        if (local) {
            w.controlled_id = 0; // pinstControlledPlayer = pinstLocalPlayer
            ev.released_control = true;
        }
        w.set_heights(rider, 0.0f, 1.0f); // EQPlayer::SetHeights(0, 1.0)
    }
    if (local) ev.close_spell_book = true;
    mount->rider_id = 0; // 0x8cfd30(0) on the mount
    rider.mount_id = 0;  // 0x8cfd20(0)
    // +0x84 (the pitch) = 0, PerFrameUpdate, +0x1f00 (MountAnimationRelated) = 0: drawing state here.
}

void do_stand(WorldState& w, Spawn& rider, Spawn* mount, MountQueries& q, MountEvents& ev) {
    if (!mount) return;
    // The buff tick can run before the frame builds the actors, including after a zone-entry form change.
    // The actor creation pass will try again. Keep the buff until then.
    if (!rider.actor_made || !mount->actor_made) return;
    if (rider.mount_id) {
        if (rider.mount_id == mount->id) return;
        dismount(w, rider, ev);
    }
    const bool local = rider.id == w.player_id;
    std::int32_t point = -1;
    // The local player needs room for 3 x AvatarHeight between the ceiling and the floor (when CanIFitHere finds
    // the position good); room against AvatarHeight * 3.0, unrounded: only a larger need fails.
    if (local && q.can_i_fit_here()) {
        double need = static_cast<double>(rider.heights.avatar_height) * kRoomFactor;
        if (need > q.room_above()) {
            dismount(w, rider, ev);
            ev.messages.push_back(kStrNoRoom);
            ev.remove_mount_affect = true;
            return;
        }
    }
    if (is_mount_race(mount->race)) {
        // PlayerZoneClient::ChangeHeight(mount, the rider's Height (+0x13c), 0, 1.0, 0).
        w.change_height(*mount, rider.heights.height, 0.0f, 1.0f);
    } else {
        point = 3;
    }
    if (!attach_rider(rider, *mount, point, q, w.illusion_mounts)) {
        if (local) {
            ev.messages.push_back(kStrCannotRide);
            ev.remove_mount_affect = true;
        }
        return;
    }
    rider.mount_id = mount->id;
    mount->rider_id = rider.id;
    // A levitating rider (+0x1ebc 2 or 5) gives the mount its gravity mode (vf78, PlayerPhysics_SetFlyMode).
    if (rider.fly_mode == 2) mount->fly_mode = rider.fly_mode;
    if (local && is_mount_race(mount->race)) {
        w.controlled_id = mount->id;
        // The local player's SpeedRun and SpeedHeading 0, then its physics block (+0x64, 12 dwords) onto the
        // mount's; UpdatePlayerActor on the mount; ViewHeight (+0x190) x 0.85.
        rider.speed_run = 0.0f;
        rider.speed_heading = 0.0f;
        mount->position = rider.position;
        mount->velocity = rider.velocity;
        mount->speed_run = rider.speed_run;
        mount->heading = rider.heading;
        mount->speed_heading = rider.speed_heading;
        rider.heights.view_height *= kViewOnMount;
        ev.took_control = true;
    }
    // PerFrameUpdate and Animation (+0x10c0) = 0 are drawing state. A rider standing on a vehicle (+0x150) gives the
    // mount its position and FloorHeight: spawns stand on no vehicles here.
}

void make_rider_mount_up(WorldState& w, Spawn& s, MountQueries& q, MountEvents& ev) {
    if (is_npc_type(s)) {
        if (!is_mount_race(s.race) || s.rider_id || !s.actor_made) return;
        auto name = rider_name_for_mount(s.name);
        if (!name) return;
        if (Spawn* rider = spawn_by_name(w, *name)) do_stand(w, *rider, &s, q, ev);
        return;
    }
    if (s.mount_id) return;
    Spawn* mount = spawn_by_name(w, mount_name_for(s.name));
    if (!mount || mount->rider_id || !is_mount_race(mount->race) || !mount->actor_made) return;
    do_stand(w, s, mount, q, ev);
}

void clear_vehicle_mount_links(WorldState& w, std::uint32_t id, MountEvents& ev) {
    Spawn* self = spawn_by_id(w, id);
    if (!self) return;
    // ~PlayerClient: only a vehicle race or a spawn with a rider or a mount clears its links.
    if (self->rider_id || self->mount_id) {
        for (auto& [other_id, other] : w.spawns) {
            if (other_id == id) continue;
            // (A spawn standing on it (+0x150) steps off: spawns stand on no vehicles here.)
            if (other.mount_id == id) dismount(w, other, ev);
            if (other.rider_id == id) dismount(w, *self, ev);
        }
    }
    // Whatever still names the id lets go of it: the original's links are pointers that die with the spawn, and
    // EQEmu gives the id to the next spawn, which a stale link would seat a rider on.
    for (auto& [other_id, other] : w.spawns) {
        if (other_id == id) continue;
        if (other.rider_id == id) other.rider_id = 0;
        if (other.mount_id == id) other.mount_id = 0;
    }
    if (w.controlled_id == id) w.controlled_id = 0;
}

std::uint32_t change_form_unseat(WorldState& w, Spawn& s, MountEvents& ev) {
    if (Spawn* mount = spawn_by_id(w, s.mount_id)) {
        std::uint32_t kept = mount->id;
        dismount(w, s, ev);
        return kept;
    }
    Spawn* mount = spawn_by_partial_name(w, s.name + std::string(kMountSuffix));
    if (!mount || mount->rider_id != s.id) return 0;
    mount->rider_id = 0;
    return mount->id;
}

void summon_mount_tick(WorldState& w, MountQueries& q, MountEvents& ev) {
    Spawn* me = spawn_by_id(w, w.player_id);
    if (!me || me->mount_id) return;
    std::string name = me->name.substr(0, std::min<std::size_t>(me->name.size(), 0x18));
    if (Spawn* mount = spawn_by_partial_name(w, name + std::string(kMountSuffix))) do_stand(w, *me, mount, q, ev);
}

} // namespace hailstone::game
