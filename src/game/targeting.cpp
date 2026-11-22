#include "game/targeting.h"

#include <algorithm>

#include "game/race_models.h"
#include "game/world_state.h"

namespace hailstone::game {

namespace {

constexpr float kCycleRange = 500.0f; // 0x43fa0000
constexpr std::uint16_t kRaceAlwaysTargetable = 0x8cd;

// TargetManager_InsertCycleNPC / PC / Corpse: from entry 8 down, filled entries
// the spawn is nearer than move down one; it goes in after the first it is not
// nearer than, or first; entry 9 falls off. An empty list takes it first.
void insert_sorted(CycleList& list, std::uint32_t id, float distance) {
    if (id == 0) return;
    if (list[0].id != 0) {
        for (int k = 8; k >= 0; --k) {
            CycleEntry& e = list[static_cast<std::size_t>(k)];
            if (e.id == 0) continue;
            if (!(distance < e.distance)) {
                list[static_cast<std::size_t>(k) + 1] = {id, distance, false};
                return;
            }
            list[static_cast<std::size_t>(k) + 1] = e;
        }
    }
    list[0] = {id, distance, false};
}

// TargetManager_Merge*Visited: each new entry (up to the first empty one) takes
// the visited flag of the old entries (up to their first empty one) with its id.
void merge_visited(CycleList& list, const CycleList& old) {
    for (CycleEntry& e : list) {
        if (e.id == 0) return;
        for (const CycleEntry& o : old) {
            if (o.id == 0) break;
            if (o.id == e.id) e.visited = o.visited;
        }
    }
}

// TargetManager_NextUnvisitedCycle*.
std::uint32_t next_unvisited(CycleList& list) {
    if (list[0].id == 0) return 0;
    for (CycleEntry& e : list) {
        if (e.id != 0 && !e.visited) {
            e.visited = true;
            return e.id;
        }
    }
    for (std::size_t k = 1; k < list.size(); ++k) list[k].visited = false;
    list[0].visited = true;
    return list[0].id;
}

std::uint32_t pick_first(CycleList& list) {
    for (std::size_t k = 1; k < list.size(); ++k) list[k].visited = false;
    list[0].visited = true;
    return list[0].id;
}

const Spawn* find(const WorldState& w, std::uint32_t id) {
    auto it = w.spawns.find(id);
    return it == w.spawns.end() ? nullptr : &it->second;
}

} // namespace

bool is_banner_race(std::uint32_t race) { return race == 586 || (race >= 553 && race <= 557); }

bool is_targetable(const Spawn& s) {
    return s.targetable && !s.has_property(11) &&
           !(s.has_property(100) && (s.has_property(101) || s.has_property(103)));
}

namespace {

// The refill of TargetManager_GetNextCycleTarget: the mode's list from the offered spawns, old visited flags merged.
CycleList* refill_cycle_list(TargetManager& tm, std::int32_t mode, const std::vector<CycleCandidate>& offered) {
    CycleList* list = mode == 1 ? &tm.npcs : mode == 0 ? &tm.pcs : mode == 2 ? &tm.corpses : nullptr;
    if (!list) return nullptr;
    CycleList old = *list;
    *list = CycleList{};
    // TargetManager_CycleCandidateCallback for each spawn the scene graph offers.
    for (const CycleCandidate& c : offered) {
        if (!c.targetable || c.banner_race || c.race == 0x237) continue;
        if (mode == 1) {
            if (c.cyclable) insert_sorted(*list, c.id, c.distance);
        } else if (mode == 0) {
            insert_sorted(*list, c.mount_race && c.rider != 0 ? c.rider : c.id, c.distance);
        } else {
            insert_sorted(*list, c.id, c.distance);
        }
    }
    merge_visited(*list, old);
    return list;
}

} // namespace

std::uint32_t get_next_cycle_target(TargetManager& tm, std::int32_t mode, bool restart,
                                    const std::vector<CycleCandidate>& offered) {
    CycleList* list = refill_cycle_list(tm, mode, offered);
    if (!list) return 0;
    return restart ? pick_first(*list) : next_unvisited(*list);
}

std::uint32_t get_previous_cycle_target(TargetManager& tm, std::int32_t mode, std::uint32_t current,
                                        const std::vector<CycleCandidate>& offered) {
    CycleList* list = refill_cycle_list(tm, mode, offered);
    if (!list || (*list)[0].id == 0) return 0;
    std::size_t filled = 0, at = list->size();
    for (std::size_t k = 0; k < list->size() && (*list)[k].id != 0; ++k, ++filled)
        if (current != 0 && (*list)[k].id == current) at = k;
    std::size_t pick = at == list->size() ? 0 : at == 0 ? filled - 1 : at - 1;
    for (std::size_t k = 0; k < list->size(); ++k) (*list)[k].visited = k <= pick;
    return (*list)[pick].id;
}

bool is_player_pet(const WorldState& world, const Spawn& s) {
    if (s.mercenary) return true;
    if (s.pet_owner_id == 0) return false;
    if (s.pet_owner_id == world.player_id) return true;
    const Spawn* owner = find(world, s.pet_owner_id);
    return owner && !owner->is_npc;
}

bool npc_in_target_view(const Spawn& spawn, const glm::mat4* view_projection,
                        const std::function<bool(const glm::vec3&)>& clear_line) {
    const glm::vec3 center = spawn.display_valid ? spawn.display_position : spawn.position;
    // Sample the body and eye so a low wall or the edge of the viewport can hide part of the spawn.
    const float eye = spawn.heights.view_height - spawn.heights.avatar_height;
    const float lower_body = -0.5f * spawn.heights.avatar_height;
    for (const float height : {0.0f, eye, lower_body}) {
        const glm::vec3 point = center + glm::vec3(0.0f, 0.0f, height);
        if (view_projection) {
            const glm::vec4 clip = *view_projection * glm::vec4(point, 1.0f);
            // Vulkan clip space. The depth check works with the renderer's reverse-Z projection too.
            if (!(clip.w > 0.0f && glm::abs(clip.x) <= clip.w && glm::abs(clip.y) <= clip.w &&
                  clip.z >= 0.0f && clip.z <= clip.w)) continue;
        }
        if (!clear_line || clear_line(point)) return true;
    }
    return false;
}

std::vector<CycleCandidate> cycle_candidates(const WorldState& world, std::int32_t mode, const glm::vec3& eye,
                                             float range, bool skip_pets,
                                             const std::function<bool(const Spawn&)>& invisible,
                                             const std::function<bool(const Spawn&)>& in_sight) {
    std::vector<CycleCandidate> out;
    for (const auto& [id, s] : world.spawns) {
        if (id == world.player_id || (world.controlled_id != 0 && id == world.controlled_id)) continue;
        // 0x5c58c0: mode 1 leaves out mount races and spawns with a rider; mode 0 takes a non-player only when it
        // is a mount race with a rider.
        const bool mount_race = is_mount_race(s.race);
        bool wanted = mode == 2   ? s.is_corpse
                      : mode == 1 ? (s.is_npc && !s.is_corpse && !mount_race && s.rider_id == 0)
                                  : ((!s.is_npc && !s.is_corpse) || (mount_race && s.rider_id != 0));
        if (!wanted) continue;
        if (mode == 1 && skip_pets && is_player_pet(world, s)) continue;
        if (mode != 2 && invisible && invisible(s)) continue; // the local player's IsInvisible (0x5c5a04)
        glm::vec3 p = s.display_valid ? s.display_position : s.position;
        float d = glm::distance(p, eye);
        if (!(d <= range)) continue;
        if (mode == 1 && in_sight && !in_sight(s)) continue; // Hailstone: view and occlusion, after the range test
        CycleCandidate c;
        c.id = id;
        c.distance = d;
        c.targetable = is_targetable(s);
        c.banner_race = is_banner_race(s.race);
        c.race = s.race;
        c.cyclable = s.cyclable;
        c.mount_race = mount_race;
        c.rider = s.rider_id;
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const CycleCandidate& a, const CycleCandidate& b) { return a.id < b.id; });
    return out;
}

bool red_con_tip_due(TargetManager& tm, std::uint32_t target_id, int my_level, int target_level) {
    if (my_level > 10 || target_level - my_level <= 2 || target_id == tm.red_con) return false;
    tm.red_con = target_id;
    return true;
}

std::optional<std::uint32_t> target_key_command(TargetManager& tm, int command, const WorldState& world,
                                                const glm::vec3& eye, bool in_game_keys) {
    const std::uint32_t current = world.target_id;
    const std::uint32_t self = world.player_id;
    if (command == 273) { // TOGGLETARGET: self and the remembered target, back and forth
        if (!in_game_keys) return std::nullopt;
        if (current != self) {
            if (current != 0) {
                tm.remembered = current;
                tm.previous = current;
            }
            return self;
        }
        if (!find(world, tm.remembered)) return std::nullopt; // GetSpawnByID(+0x16c)
        tm.previous = current;
        return tm.remembered;
    }
    if (command == 343) { // TOGGLETWOTARGETS: the target and the one before it swap
        const std::uint32_t before = tm.previous;
        if (current != 0) tm.previous = current;
        if (before == 0 || !find(world, before) || before == current) return std::nullopt;
        return before;
    }
    std::int32_t mode;
    bool restart;
    switch (command) {
    case 30: mode = 0; restart = true; break;  // TARGETPC
    case 31: mode = 0; restart = false; break; // CYCLEPCTARGETS
    case 32: mode = 1; restart = true; break;  // TARGETNPC
    case 33: mode = 1; restart = false; break; // CYCLENPCTARGETS
    case 34: mode = 2; restart = true; break;  // TARGETCORPSE
    case 35: mode = 2; restart = false; break; // CYCLECORPSETARGETS
    default: return std::nullopt;
    }
    std::uint32_t id =
        get_next_cycle_target(tm, mode, restart,
                              cycle_candidates(world, mode, eye, kCycleRange, tm.skip_pets, tm.invisible, tm.in_sight));
    if (id == 0) return std::nullopt;
    if (current != 0) tm.previous = current;
    std::uint32_t target = id;
    if (command == 32 || command == 34) {
        // TARGETNPC / TARGETCORPSE: a spawn below race 0x8cd that is not targetable reverts the target (a mount
        // with a rider gives the rider for TARGETNPC; mounts are not modelled).
        const Spawn* s = find(world, id);
        if (s && s->race < kRaceAlwaysTargetable && !is_targetable(*s)) target = current;
    }
    if (target == current) return std::nullopt;
    return target;
}

std::optional<std::uint32_t> target_previous_npc(TargetManager& tm, const WorldState& world, const glm::vec3& eye) {
    const std::uint32_t current = world.target_id;
    std::uint32_t id =
        get_previous_cycle_target(tm, 1, current,
                                  cycle_candidates(world, 1, eye, kCycleRange, tm.skip_pets, tm.invisible, tm.in_sight));
    if (id == 0 || id == current) return std::nullopt;
    if (current != 0) tm.previous = current;
    return id;
}

} // namespace hailstone::game
