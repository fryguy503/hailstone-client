#pragma once

// The original's target keys: TOGGLETARGET (Tab), TARGETPC / TARGETNPC /
// TARGETCORPSE and their CYCLE commands (__ExecuteCmd @0x4d7230 cases 26..30
// and 38), with CTargetManager's cycle lists
// (TargetManager_GetNextCycleTarget @0x5c6710 and its helpers).
//
// Follows the original's cycle functions and
// TargetManager_CycleCandidateCallback; the command
// arguments are read from the disassembly at 0x4d8a2f..0x4d8c65:
//   TARGETPC 30 (mode 0, restart), CYCLEPCTARGETS 31 (0), TARGETNPC 32 (1,
//   restart), CYCLENPCTARGETS 33 (1), TARGETCORPSE 34 (2, restart),
//   CYCLECORPSETARGETS 35 (2), range 500 for all.
// Not modelled (the scene graph lives in EQGraphicsDX9.dll, vtable +0x98): the
// distance it measures. The client offers the spawns within 500 units of the
// eye point, in id order, with their 3D distance, that eqgame's filter
// 0x5c58c0 passes (never the local or the controlled player): mode 0 the
// players and the mount races with a rider (whom the candidate callback puts
// in their place), 1 the NPCs that are not mount races and carry no rider,
// 2 the corpses; for modes 0 and 1 it also leaves out the spawns the local
// player's PlayerZoneClient::IsInvisible @0x5a1850 hides (0x5c5a04), of which
// the client models Hide Traders and Hide Buyers (TargetManager::invisible).
// Its other tests (body types, owners, stale updates) are not modelled.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::game {

struct Spawn;
struct WorldState;

struct CycleEntry {
    std::uint32_t id = 0;
    float distance = 0.0f;
    bool visited = false;
};
using CycleList = std::array<CycleEntry, 10>;

// CTargetManager (CTargetManager::Get): the cycle lists and the two
// remembered targets.
struct TargetManager {
    CycleList npcs{};    // +0
    CycleList pcs{};     // +0x78
    CycleList corpses{}; // +0xf0
    std::uint32_t previous = 0;   // +0x168: the target before the last key change
    std::uint32_t remembered = 0; // +0x16c: what TOGGLETARGET returns to
    std::uint32_t red_con = 0;    // +0x170: the last target the red con context tip was offered for
    // Hailstone (option tab_skip_pets, on by default; docs/mechanics.md): the NPC cycle leaves out players' pets
    // and mercenaries. The original offers every NPC with +0x161 set, pets included.
    bool skip_pets = true;
    // PlayerZoneClient::IsInvisible for another spawn (0x5c58c0's test for the PC and NPC cycles): the traders
    // Hide Traders hides and the buyers Hide Buyers hides (GameConnection::market_hidden). Unset: none.
    std::function<bool(const Spawn&)> invisible;
    // Hailstone (option tab_line_of_sight, on by default; docs/mechanics.md): the NPC cycle takes only the NPCs this
    // says the player can see (the zone's collision between the eyes and the spawn). Unset: every NPC, as the
    // original's cycle as modelled here.
    std::function<bool(const Spawn&)> in_sight;
};

// A spawn the scene graph offers to TargetManager_CycleCandidateCallback.
struct CycleCandidate {
    std::uint32_t id = 0;
    float distance = 0.0f;
    bool targetable = true;   // PlayerBase::IsTargetable
    bool banner_race = false; // PlayerClient_IsBannerRace (586, 553..557)
    std::uint16_t race = 0;   // race 567 is refused
    bool cyclable = true;     // +0x161 (NPC mode only)
    bool mount_race = false;  // PlayerBase_IsMountRace
    std::uint32_t rider = 0;  // a mount's rider (PC mode puts it in the mount's place)
};

// TargetManager_GetNextCycleTarget(range, mode, restart): mode 1 NPCs, 0 PCs,
// 2 corpses. The list is refilled from `offered` (sorted by distance, ten at
// most), the old visited flags are merged back, and the result is the first
// entry (restart: the others unmarked) or the next unvisited one (none left:
// the first again). 0 when the list is empty or for another mode.
std::uint32_t get_next_cycle_target(TargetManager& tm, std::int32_t mode, bool restart,
                                    const std::vector<CycleCandidate>& offered);

// PlayerBase::IsTargetable @0x8d0090: no-target actors and utility traps are excluded from ordinary targeting.
bool is_targetable(const Spawn& s);
bool is_banner_race(std::uint32_t race);

// A player's pet or a mercenary (the Hailstone NPC cycle's skip_pets): the spawn's MasterID is the local player or
// another player (an NPC's pet stays in the cycle), or its spawn record says mercenary.
bool is_player_pet(const WorldState& world, const Spawn& s);

// The spawns the client offers for a cycle mode (see the header comment); `skip_pets` leaves players' pets and
// mercenaries out of mode 1, `invisible` (modes 0 and 1) the spawns IsInvisible hides, `in_sight` (mode 1) the
// spawns it says are out of sight.
std::vector<CycleCandidate> cycle_candidates(const WorldState& world, std::int32_t mode, const glm::vec3& eye,
                                             float range = 500.0f, bool skip_pets = false,
                                             const std::function<bool(const Spawn&)>& invisible = {},
                                             const std::function<bool(const Spawn&)>& in_sight = {});

// The red con context tip's test (tip 0x20), shared by __ExecuteCmd's AUTOPRIM (0x4d8d91: turning auto attack on) and
// CharacterZoneClient::DoPassageOfTime (0x45dc80: the target changed while attacking): the player at level 10 or
// below, the target more than two levels above (PlayerZoneClient::GetLevel), and not the target tipped last
// (CTargetManager +0x170), which it then becomes.
bool red_con_tip_due(TargetManager& tm, std::uint32_t target_id, int my_level, int target_level);

// One of the target key commands (24 TARGETME is not here): the new target,
// or nullopt when it stays. `in_game_keys` is __InChatMode (0xddf66c, 1 when
// the keyboard drives the game): TOGGLETARGET does nothing without it.
// 343 TOGGLETWOTARGETS (__ExecuteCmd case 89 @0x4d747f, not while the chat input has the keys): the target before
// the last change (+0x168) comes back when that spawn is still there, and the current target (if any) takes its
// place in +0x168.
std::optional<std::uint32_t> target_key_command(TargetManager& tm, int command, const WorldState& world,
                                                const glm::vec3& eye, bool in_game_keys = true);

// A client extension (the gamepad's LB, docs/gamepad.md): the original has no key to cycle backward. The list is
// refilled as get_next_cycle_target does it; the result is the entry before `current` (the first one: the last,
// the farthest; `current` not in the list: the first, the nearest). The entries up to the result are marked visited
// and the rest not, so the forward cycle goes on from there. 0 when the list is empty or for another mode.
std::uint32_t get_previous_cycle_target(TargetManager& tm, std::int32_t mode, std::uint32_t current,
                                        const std::vector<CycleCandidate>& offered);
// The backward NPC cycle as a target key: the new target, or nullopt when it stays.
std::optional<std::uint32_t> target_previous_npc(TargetManager& tm, const WorldState& world, const glm::vec3& eye);

} // namespace hailstone::game
