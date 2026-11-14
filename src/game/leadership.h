#pragma once

// Leadership abilities: RoF2's trainable indexes and rank progression (LeadershipAbility constructor @0x55d8e0),
// 0..15 group and 16..31 raid, at most six ranks. Unavailable waypoint/legacy abilities are omitted. EQEmu's
// common/features.h supplies the experience scale: 1000 per group point, 2000 per raid point. The server validates
// purchases; older EQEmu cost tables differ from RoF2 for some indexes.

#include <array>
#include <cstdint>
#include <string_view>

namespace hailstone::game {

inline constexpr int kLeadershipAbilities = 32, kLeadershipRanks = 6, kFirstRaidAbility = 16;
inline constexpr double kGroupLeadershipExpPerPoint = 1000, kRaidLeadershipExpPerPoint = 2000;

struct LeadershipAbility {
    std::string_view name; // empty: no trainable RoF2 ability
    std::array<std::uint8_t, kLeadershipRanks> costs;
};
const LeadershipAbility& leadership_ability(int index);

// The leadership window's experience gauge, 0..1000: `exp` of `per`. The experience is the server's f64, clamped
// before it becomes an int (a NaN or a value past int's range would be undefined); NaN shows as 0.
int leadership_gauge(double exp, double per);

// How many ranks an ability has (its leading non-zero costs) and the cost of the next one (0: none).
int leadership_max_rank(int index);
std::uint32_t leadership_next_cost(int index, std::uint32_t rank);

// The original's leadership ability records (0x55d8e0) as
// the leadership window's hot buttons read them: how many hot buttons the ability has (+8: 3 for the marks and
// the main assists, else 1) and the three string ids of its hot button text (+0xc, +0x10, +0x14; 0 none).
struct LeadershipHotButtonDef {
    int count = 0;
    std::array<std::uint32_t, 3> words{};
};
const LeadershipHotButtonDef& leadership_hot_button_def(int ability);
// 0x560180's passive abilities (1, 7..12, 17, 22..27), which never get a hot button.
bool leadership_passive(int ability);
// The hot button's value (CLeadershipWnd::WndNotification: ability * 8 + n, the cursor attachment's type 0xd; a hot
// button of type 11 runs Leadership_ActivateAbility(value / 8, value % 8)).
inline constexpr std::int32_t leadership_hot_button_value(int ability, int n) { return ability * 8 + n; }

} // namespace hailstone::game
