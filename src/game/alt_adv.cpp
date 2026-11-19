#include "game/alt_adv.h"

#include <algorithm>

namespace hailstone::game {

bool AltAbility::class_allowed(std::uint32_t cls) const {
    if (cls - 1 > 15) return false;
    return (classes & (1u << cls)) != 0;
}

const AltAbility* AltAdvState::get(std::uint32_t id) const {
    auto it = table.find(id);
    return it == table.end() ? nullptr : &it->second;
}

bool AltAdvState::has(std::uint32_t id) const {
    if (static_cast<std::int32_t>(id) <= 0) return false;
    return std::any_of(owned.begin(), owned.end(), [&](const OwnedAbility& o) { return o.id == id; });
}

const AltAbility* AltAdvState::owned_rank(std::int32_t group) const {
    for (const OwnedAbility& o : owned)
        if (const AltAbility* a = get(o.id); a && a->group == group) return a;
    return nullptr;
}

std::int32_t AltAdvState::owned_rank_number(const AltAbility& a) const {
    const AltAbility* o = owned_rank(a.group);
    return o ? o->rank : 0;
}

bool AltAdvState::meets_prerequisites(const AltAbility& a) const {
    if (a.prev_id > 0 && !has(static_cast<std::uint32_t>(a.prev_id))) return false;
    if (a.group > 0) {
        // Owning a rank of this ability: it must be the one before this rank.
        if (const AltAbility* o = owned_rank(a.group); o && static_cast<std::int32_t>(o->id) != a.prev_id) return false;
    }
    if (a.prereq_groups.empty()) return true;
    if (a.prereq_groups[0] > 0) {
        // 0x4a2170: prerequisite i is met by an owned rank of that ability at the required rank or above.
        auto meets = [&](std::size_t i, const AltAbility& o) {
            return i < a.prereq_groups.size() && i < a.prereq_ranks.size() && a.prereq_groups[i] == o.group &&
                   o.rank >= a.prereq_ranks[i];
        };
        for (std::size_t i = 0; i < a.prereq_groups.size(); ++i) {
            bool found = false;
            for (const OwnedAbility& slot : owned) {
                const AltAbility* o = get(slot.id);
                if (o && meets(i, *o)) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
    }
    // The legacy category prerequisites: -1 needs 6 points in general (type 1) abilities, -2 needs 12 in
    // archetype (type 2) ones, counted by the owned ranks' total cost.
    auto points_in = [&](std::int32_t type) {
        std::int32_t sum = 0;
        for (const OwnedAbility& slot : owned)
            if (const AltAbility* o = get(slot.id); o && o->type == type) sum += o->total_cost;
        return sum;
    };
    if (a.prereq_groups[0] == -1 && points_in(1) < 6) return false;
    if (a.prereq_groups[0] == -2 && points_in(2) < 12) return false;
    return true;
}

bool AltAdvState::can_train(const AltAbility& a, const AATrainer& who) const {
    if (has(a.id)) return false;
    if (static_cast<std::int32_t>(points) < a.cost) return false;
    if (who.spend_limit != -1 && static_cast<std::int32_t>(spent) + a.cost > who.spend_limit) return false;
    if (!who.ignore_level && who.level < a.level_req) return false;
    // CanTrainAbility @0x4a44c0 refuses a monster-only rank outside a shroud (0x4a4960: profile type 1 or 2); unlike
    // CanSeeAbility it does not refuse an ordinary rank inside one.
    if (a.monster_only() && !who.monster_profile) return false;
    if (!a.class_allowed(who.class_id)) return false;
    if (a.grant_only) return false;
    if (!meets_prerequisites(a)) return false;
    if (a.expansion > 0 && !(who.expansions & (1u << ((a.expansion - 1) & 31)))) return false;
    return true;
}

AAReadiness AltAdvState::readiness(const AltAbility& a, std::int32_t reduction, std::int64_t now) const {
    AAReadiness r;
    // GetCalculatedTimer: the reuse time less SPA 264's reduction (only when there is a reuse time).
    std::int32_t total = a.refresh;
    if (total != 0) total -= reduction;
    if (total == 0) return r;
    if (a.timers.empty() || a.timers[0] < 0 || a.timers[0] >= kAATimers) return r;
    std::int64_t last = -99999; // 0xfffe7961
    for (std::int32_t t : a.timers) {
        if (t < 0 || t >= kAATimers) continue; // the original faults (int3) on such an index
        last = std::max(last, timers[static_cast<std::size_t>(t)]);
    }
    r.total = total;
    r.ready = total + last <= now;
    // The original writes total - now + last either way; its callers only show it while not ready.
    r.remaining = r.ready ? 0 : static_cast<std::int32_t>(total - now + last);
    return r;
}

void AltAdvState::set_timer(std::uint32_t index, std::uint32_t begin, std::uint32_t server_now, std::int64_t now) {
    if (index >= kAATimers) return; // the original also skips negative indices (signed compare)
    // The server's clock is translated to ours: begin + (our time - its time). The original adds in 32 bits; both
    // wire times are unsigned 32-bit seconds (EQEmu's uint32 time()), so the sum is taken in 64 bits: the same until
    // 2038, and past it a start time is not read as negative (which made every timer ready at once).
    timers[index] = static_cast<std::int64_t>(begin) + (now - static_cast<std::int64_t>(server_now));
    timers_seen = true;
}

void AltAdvState::set_owned(std::uint32_t spent_points, const std::array<std::uint32_t, 5>& assigned_points,
                            std::vector<OwnedAbility> list) {
    spent = spent_points;
    assigned = assigned_points;
    owned = std::move(list);
}

std::vector<mechanics::SheetAbility> AltAdvState::sheet_abilities() const {
    std::vector<mechanics::SheetAbility> out;
    sheet_abilities_into(out);
    return out;
}

void AltAdvState::sheet_abilities_into(std::vector<mechanics::SheetAbility>& out) const {
    std::size_t n = 0;
    for (const OwnedAbility& o : owned) {
        const AltAbility* a = o.id ? get(o.id) : nullptr;
        if (!a) continue;
        if (n == out.size()) out.emplace_back();
        mechanics::SheetAbility& s = out[n++];
        s.id = static_cast<std::int32_t>(a->id);
        s.class_mask = a->classes;
        s.required_level = a->level_req;
        s.ignores_level = a->ignores_level != 0;
        s.effects.clear();
        for (const AltAbilityEffect& e : a->effects) s.effects.push_back({e.spa, e.base, e.limit});
    }
    out.resize(n);
}

bool AltAdvState::can_see(const AltAbility& a, std::uint8_t class_id, bool monster_profile) const {
    if (a.monster_only() != monster_profile) return false;
    if (!a.class_allowed(class_id)) return false;
    if ((a.next_id <= 0 || a.rank == a.max_rank) && has(a.id)) return true; // the owned last rank
    if (const AltAbility* prev = get(static_cast<std::uint32_t>(a.prev_id))) {
        if (has(prev->id)) return true; // the next rank to train
    } else if (!owned_rank(a.group)) {
        return true; // a first rank of an ability not yet owned
    }
    return a.grant_only && has(a.id);
}

std::vector<const AltAbility*> AltAdvState::visible(std::int32_t type, std::uint8_t class_id) const {
    std::vector<const AltAbility*> out;
    for (const auto& [id, a] : table)
        if ((type == 0 || a.type == type) && can_see(a, class_id)) out.push_back(&a);
    return out;
}

std::vector<const AltAbility*> AltAdvState::visible(std::int32_t type, std::uint8_t class_id, const AAFilter& filter,
                                                    const AATrainer& who) const {
    std::vector<const AltAbility*> out;
    for (const auto& [id, ability] : table) {
        if ((type != 0 && ability.type != type) || !can_see(ability, class_id, who.monster_profile)) continue;
        const AltAbility* a = &ability;
        // AltAbility_FirstPersistentEffect: the persistent effects are the ability's effect list (EQEmu sends them
        // all as such); its first value is the first effect's SPA record, non-zero when there is one.
        bool persistent = !a->effects.empty();
        if (aa_filter_accepts(filter, a, persistent, [&] { return can_train(*a, who); }, [&] { return has(a->id); }))
            out.push_back(a);
    }
    return out;
}

} // namespace hailstone::game
