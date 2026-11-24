#pragma once

// The timed stages of an effect the server plays by number (OP_SpellEffect): eqgame's SpellEffectTimers
// (SpellEffectTimers_Add @0x564330, _Update @0x563f40, _Remove @0x564000, _PlayStage @0x563df0). An entry plays
// its effect's cast stage when added, the projectile stage when its elapsed time passes stage1_ms and the impact
// stage when it passes stage1_ms + stage1_length_ms, after which it is dropped. The effect is a spellsnew.eff index,
// and the emitters' key uses the spawn and effect IDs. The caller plays the stages it is handed.

#include <cstdint>
#include <vector>

namespace hailstone::game {

struct SpellEffectStagePlay {
    std::uint32_t caster = 0, target = 0, effect = 0;
    int stage = 0;
    std::int32_t duration_ms = 0;
    bool full = false; // the packet's flag: the emitters at density and opacity 1
};

class SpellEffectTimers {
public:
    // SpellEffectTimers_Add: the entry, and its cast stage (lasting stage1_ms) at once.
    void add(std::uint32_t caster, std::uint32_t target, std::uint32_t effect, std::uint32_t stage1_ms,
             std::uint32_t stage1_length_ms, std::uint32_t stage2_length_ms, bool full, std::uint64_t now_ms);
    // SpellEffectTimers_Remove: drops every entry with these three. True when one was dropped: the caller then stops
    // the caster/effect group and, unless target is 0xffffffff, the target/effect group. (The
    // original's walk follows a dropped entry's new link into the free list and would spin on a second match there;
    // here every match is dropped.)
    bool remove(std::uint32_t caster, std::uint32_t target, std::uint32_t effect);
    // SpellEffectTimers_Update: the stages whose start the entries passed since the last call (strictly: a stage
    // starting at 0 never plays from here).
    void update(std::uint64_t now_ms);
    // The stages to play, in order, since the last call.
    std::vector<SpellEffectStagePlay> take();
    void clear();
    bool empty() const { return entries_.empty(); }

private:
    struct Entry {
        std::uint32_t caster, target, effect;
        std::uint32_t stage1, stage2, stage2_length;
        bool full;
        std::uint32_t elapsed;
        std::uint64_t last;
    };
    std::vector<Entry> entries_;
    std::vector<SpellEffectStagePlay> plays_;
};

} // namespace hailstone::game
