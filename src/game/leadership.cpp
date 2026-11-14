#include "game/leadership.h"

#include <algorithm>
#include <cmath>

namespace hailstone::game {

namespace {

const std::array<LeadershipAbility, kLeadershipAbilities> kAbilities = {{
    {"Mark NPC", {3, 4, 5, 0, 0, 0}},
    {"NPC Health", {2, 0, 0, 0, 0, 0}},
    {{}, {0, 0, 0, 0, 0, 0}}, // Mark Waypoint: unavailable in RoF2
    {{}, {0, 0, 0, 0, 0, 0}}, // legacy group main-assist training: unavailable in RoF2
    {"Delegate Mark NPC", {4, 0, 0, 0, 0, 0}},
    {{}, {0, 0, 0, 0, 0, 0}},
    {"Inspect Buffs", {4, 6, 0, 0, 0, 0}},
    {{}, {0, 0, 0, 0, 0, 0}},
    {"Spell Awareness", {6, 0, 0, 0, 0, 0}},
    {"Offense Enhancement", {4, 5, 6, 7, 8, 0}},
    {"Mana Enhancement", {4, 6, 8, 0, 0, 0}},
    {"Health Enhancement", {4, 6, 8, 0, 0, 0}},
    {"Health Regeneration", {4, 6, 8, 0, 0, 0}},
    {"Find Path to PC", {4, 0, 0, 0, 0, 0}},
    {"Health of Target's Target", {7, 0, 0, 0, 0, 0}},
    {{}, {0, 0, 0, 0, 0, 0}},
    {"Mark NPC", {5, 6, 7, 0, 0, 0}},
    {"NPC Health", {4, 0, 0, 0, 0, 0}},
    {{}, {0, 0, 0, 0, 0, 0}}, // Mark Waypoint: unavailable in RoF2
    {"Delegate Main Assist", {6, 7, 8, 0, 0, 0}},
    {"Delegate Mark NPC", {6, 6, 6, 0, 0, 0}},
    {{}, {0, 0, 0, 0, 0, 0}},
    {{}, {0, 0, 0, 0, 0, 0}},
    {"Spell Awareness", {8, 0, 0, 0, 0, 0}},
    {"Offense Enhancement", {6, 7, 8, 9, 10, 0}},
    {"Mana Enhancement", {6, 8, 10, 0, 0, 0}},
    {"Health Enhancement", {6, 8, 10, 0, 0, 0}},
    {"Health Regeneration", {6, 8, 10, 0, 0, 0}},
    {"Find Path to PC", {5, 0, 0, 0, 0, 0}},
    {"Health of Target's Target", {9, 0, 0, 0, 0, 0}},
    {{}, {0, 0, 0, 0, 0, 0}},
    {{}, {0, 0, 0, 0, 0, 0}},
}};

const LeadershipAbility kNone{{}, {0, 0, 0, 0, 0, 0}};

const std::array<LeadershipHotButtonDef, kLeadershipAbilities> kHotButtons = [] {
    std::array<LeadershipHotButtonDef, kLeadershipAbilities> t{};
    auto set = [&](int a, int count, std::uint32_t w0, std::uint32_t w1, std::uint32_t w2) {
        t[static_cast<std::size_t>(a)] = {count, {w0, w1, w2}};
    };
    set(0, 3, 0x2190, 0x2191, 0x2192);    // Group Mark NPC
    set(4, 1, 0x21ff, 0x2200, 0x2201);
    set(6, 1, 0x219e, 0x219f, 0x21a0);
    set(13, 1, 0x21a1, 0x21a2, 0x21a3);
    set(14, 1, 0x2195, 0x2196, 0x2197);
    set(16, 3, 0x21da, 0x21db, 0x21dc);   // Raid Mark NPC
    set(19, 1, 0x21df, 0x21e0, 0x21e1);   // Raid Main Assist
    set(20, 1, 0x2204, 0x2205, 0x2206);   // Raid Delegate Mark
    set(28, 1, 0x21fa, 0x21fb, 0x21fc);   // Raid Find PC
    set(29, 1, 0x21b3, 0x21b4, 0x21b5);
    return t;
}();

} // namespace

const LeadershipHotButtonDef& leadership_hot_button_def(int ability) {
    static const LeadershipHotButtonDef none{};
    return ability >= 0 && ability < kLeadershipAbilities ? kHotButtons[static_cast<std::size_t>(ability)] : none;
}

bool leadership_passive(int ability) {
    return ability == 1 || (ability >= 7 && ability <= 12) || ability == 0x11 || (ability >= 0x16 && ability <= 0x1b);
}

const LeadershipAbility& leadership_ability(int index) {
    return index >= 0 && index < kLeadershipAbilities ? kAbilities[static_cast<std::size_t>(index)] : kNone;
}

int leadership_max_rank(int index) {
    const auto& a = leadership_ability(index);
    int n = 0;
    while (n < kLeadershipRanks && a.costs[static_cast<std::size_t>(n)] != 0) ++n;
    return n;
}

std::uint32_t leadership_next_cost(int index, std::uint32_t rank) {
    if (rank >= static_cast<std::uint32_t>(kLeadershipRanks)) return 0;
    return leadership_ability(index).costs[rank];
}

int leadership_gauge(double exp, double per) {
    const double g = exp / per * 1000.0;
    return std::isnan(g) ? 0 : static_cast<int>(std::clamp(g, 0.0, 1000.0));
}

} // namespace hailstone::game
