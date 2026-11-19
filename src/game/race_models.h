#pragma once

// Race and gender -> 3-letter model code, from the original client's table
// (generated into race_models.inc by tools/gen_race_models.py).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::game {

struct RaceModel {
    std::uint16_t race = 0;
    std::uint8_t gender = 0; // 0 male, 1 female, 2 neutral
    std::string_view code;   // e.g. "HUM"; the actor is "<code>_ACTORDEF"
    std::uint8_t flags = 0;  // the table's record+0x2c flags
    std::uint16_t anim_fallback = 0; // record+0x28: race whose animations to try next (flag 0x04)
};

// Bit 0x08 of RaceModel::flags: a "new-style" model, scaled differently by
// EQPlayer::SetHeights (game/spawn_heights.h is_new_style_model_race).
inline constexpr std::uint8_t kNewStyleModel = 0x08;
// Bit 0x04: the row names an animation fallback race (NpcManager_HasAnimationFallbackRace).
inline constexpr std::uint8_t kHasAnimationFallback = 0x04;

// The row for (race, gender). The original looks the pair up exactly and
// uses HUM/HUF when it is missing; this falls back to the race's gender-2,
// then gender-0, then gender-1 row first, because EQEmu sends NPCs of
// single-model races with any gender. nullptr when the race has no row.
const RaceModel* find_race_model(std::uint16_t race, std::uint8_t gender);

// Model code that `code` borrows animation clips from (LanternExtractor's
// animationsources.txt), or empty. `tunic_point`: the actor has a
// TUNIC_POINT dag (the Luclin player models), which InitAnimationTracks
// @0x407800 passes to GetAlternateAnimTag @0x406a60 as its flag: then a
// human or erudite (HU*, ER*) and the skeleton take the human's clips
// (HUM / HUF; a human its own), a barbarian only its own, where without it
// they all take the wood elf's. A code the table does not name under one of
// GetAlternateAnimTag's wood-elf prefixes takes the wood elf's code with its own
// last letter (race 127's IVM: ELM).
std::string animation_source(std::string_view code, bool tunic_point = false);

// The model code whose EQG animations `code` uses when it lacks one
// (FindAnimationTrackForModel @00406830 retries "<anim>_<CODE>" with the
// fallback race: HNM -> HUM, HUM -> DKM), or empty.
std::string_view animation_fallback_code(std::string_view code);

// NpcManager_CollectModelCodesByFlags: the codes of the rows whose flags
// include all of `flags` and none of `exclude`, in table order (repeated
// codes once).
std::vector<std::string_view> model_codes_with_flags(std::uint8_t flags, std::uint8_t exclude);
// NpcManager_FindRaceGenderByModelCode: the first row with this code, or nullptr.
const RaceModel* find_model_code(std::string_view code);

// PlayerBase_IsMountRace @0x8cf970: the 35 mount races.
bool is_mount_race(std::int32_t race);

// PlayerClient_IsVehicleRace @0x59e530 (vtable +0x50): the race/gender record's (+0x234) flag 0x01, which only the
// ships, boats and launches have: 72 SHIP / PRE, 73 LAUNCH, 114 GSP, 141 BOAT, 404 SHP, 502 ROW, 533 SHI, 544..546
// and 550..552 B01..B06, 691..693 B07..B09, 699 B10. PlayerClient_IsSmallBoatRace @0x59e5b0 (vtable +0x58): a
// vehicle without flag 0x02 (vtable +0x54, a large ship): 141 BOAT and 502 ROW. The record is find_race_model's
// (the one the model is drawn from; the original looks the pair up exactly and takes HUM / HUF without one).
inline constexpr std::uint8_t kVehicleRace = 0x01;
inline constexpr std::uint8_t kLargeShipRace = 0x02;
bool is_vehicle_race(std::uint16_t race, std::uint8_t gender);
bool is_small_boat_race(std::uint16_t race, std::uint8_t gender);

// Number of distinct race ids in the table.
std::size_t race_model_race_count();

} // namespace hailstone::game
