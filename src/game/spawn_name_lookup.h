#pragma once

// A spawn by name for the lookups the UI makes every frame (each group member's gauge and label): an index of the
// zone's spawns by name, made again only when the spawn set changes (WorldState::spawn_set_serial: a spawn added,
// taken out, replaced or renamed), so a lookup is a hash probe rather than a walk over every spawn.
//
// The name a group member has is a player's: an exact name (case ignored) wins over a display name ("a_rat01" shown
// as "a rat"), and a player over an NPC of the same name, so an NPC whose name or display name matches does not stand
// in for the member. Between equals the lowest id is taken (the spawn map's order is not fixed).

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/ascii_case.h"
#include "game/world_state.h"

namespace hailstone::game {

class SpawnNameLookup {
public:
    // The spawn named `name` (nullptr: none); with `display`, a spawn whose display name is it when none has the
    // exact name.
    const Spawn* find(const WorldState& w, std::string_view name, bool display) {
        if (name.empty()) return nullptr;
        if (&w != world_ || w.spawn_set_serial != serial_ || w.spawns.size() != count_) rebuild(w);
        key_.clear();
        for (char c : name) key_ += ascii_lower(c);
        if (const Spawn* s = at(w, exact_)) return s;
        return display ? at(w, shown_) : nullptr;
    }

private:
    const Spawn* at(const WorldState& w, const std::unordered_map<std::string, std::uint32_t>& index) const {
        auto it = index.find(key_);
        if (it == index.end()) return nullptr;
        auto s = w.spawns.find(it->second);
        return s == w.spawns.end() ? nullptr : &s->second;
    }
    static void offer(std::unordered_map<std::string, std::uint32_t>& index, const WorldState& w, std::string key,
                      std::uint32_t id, const Spawn& s) {
        auto [it, added] = index.try_emplace(std::move(key), id);
        if (added) return;
        const Spawn& held = w.spawns.at(it->second);
        // a player before an NPC; then the lowest id
        if ((held.is_npc && !s.is_npc) || (held.is_npc == s.is_npc && id < it->second)) it->second = id;
    }
    void rebuild(const WorldState& w) {
        exact_.clear();
        shown_.clear();
        for (const auto& [id, s] : w.spawns) {
            std::string key;
            for (char c : s.name) key += ascii_lower(c);
            offer(exact_, w, std::move(key), id, s);
            std::string shown;
            for (char c : display_name(s.name)) shown += ascii_lower(c);
            offer(shown_, w, std::move(shown), id, s);
        }
        world_ = &w;
        serial_ = w.spawn_set_serial;
        count_ = w.spawns.size();
    }

    const WorldState* world_ = nullptr;
    std::uint64_t serial_ = 0;
    std::size_t count_ = 0;
    std::unordered_map<std::string, std::uint32_t> exact_, shown_;
    std::string key_;
};

} // namespace hailstone::game
