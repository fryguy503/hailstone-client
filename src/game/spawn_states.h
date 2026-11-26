#pragma once

// Per-spawn state kept by spawn id that starts again when the id comes back as another spawn: the server may
// despawn a spawn and spawn a new one under the same id within one frame, so the id alone would hand the new
// spawn the old one's state (game::Spawn::incarnation tells them apart).

#include <cstdint>
#include <unordered_map>

namespace hailstone::game {

template <class State>
class SpawnStates {
public:
    // The state of spawn `id` in its incarnation `incarnation`; a new State for a new id or a new incarnation.
    State& get(std::uint32_t id, std::uint64_t incarnation) {
        auto [it, fresh] = states_.try_emplace(id);
        if (!fresh && it->second.incarnation != incarnation) it->second.state = State{};
        it->second.incarnation = incarnation;
        return it->second.state;
    }
    // Drops the ids `spawns` (a map by spawn id) no longer holds.
    template <class Spawns>
    void prune(const Spawns& spawns) {
        std::erase_if(states_, [&](const auto& kv) { return !spawns.contains(kv.first); });
    }
    std::size_t size() const { return states_.size(); }
    void clear() { states_.clear(); }

private:
    struct Entry {
        std::uint64_t incarnation = 0;
        State state{};
    };
    std::unordered_map<std::uint32_t, Entry> states_;
};

} // namespace hailstone::game
