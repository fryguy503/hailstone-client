#pragma once

// Where the original samples a spawn's regions, and what a change of the
// region under its feet does.
//
// Follows the original exactly, checked against its vectors
// (tests/movement_vectors_test.cpp). A spawn's z is its feet plus
// AvatarHeight; the probes (docs/unknowns.md rows 20, 21 and 40):
//   body  z (PlayerClient::sub_00506d20, BodyEnvironmentChange)
//   head  z + (ViewHeight - AvatarHeight) (HeadEnvironmentChange)
//   feet  z + 0.25 - (GetCollisionHeight - AvatarHeight), normally the feet
//         + 0.25 (CheckRegionChange, FeetEnvironmentChange)

#include <cstdint>
#include <optional>

#include "game/mechanics/fpu.h"
#include "game/sound_rules.h"

namespace hailstone::game::mechanics {

// The spawn fields GetCollisionHeight reads.
struct CollisionHeightSpawn {
    std::uint8_t stand_state = 0; // +0x35c (0x6f: AvatarHeight itself)
    bool mount_race = false;      // PlayerBase_IsMountRace
    bool has_rider = false;       // PlayerBase_GetRider
    float avatar_height = 0.0f;   // +0x138
    float length = 0.0f;          // +0x144 (eqlib's Length)
};

// PlayerClient_GetCollisionHeight @0x506340: AvatarHeight in stand state 0x6f;
// 3 x AvatarHeight for a mount race with a rider; else +0x144 when it is above
// 0, else 2 x AvatarHeight (a NaN +0x144 too). Unrounded, as the original
// leaves it on the FPU.
double collision_height(const CollisionHeightSpawn& s, const Fpu& fpu = Fpu{});

// The region sample's z: ((0.25 - (height - AvatarHeight)) + z) stored as a
// float.
float region_sample_z(const CollisionHeightSpawn& s, float z, const Fpu& fpu = Fpu{});

inline constexpr std::uint32_t kRegionIce = 0x10000000;      // bit 28: ice (not for NPCs)
inline constexpr std::uint32_t kRegionPvp = 0x40000000;      // bit 30: the +0x2e1 flag
inline constexpr std::uint32_t kRegionZoneLine = 0x80000000; // bit 31: a zone line
inline constexpr std::int32_t kNoPvpFlagZone = 0x7e;         // the +0x2e1 flag is never set here
inline constexpr std::uint32_t kStrPvpRegion = 0x2ee9;

// The spawn fields CheckRegionChange reads and writes.
struct RegionSpawn {
    CollisionHeightSpawn body;
    std::uint8_t type = 0;              // +0x125: 0 player, 1 NPC, 2+ corpse
    std::uint32_t held_flags = 0xFFFFFFFFu; // +0x9c: the regions the feet were in (ResetVariables: all ones)
    float area_friction = 0.0f;         // +0x1c
    float acceleration_friction = 0.0f; // +0x20
    std::uint8_t pvp_flag = 0;          // +0x2e1
};

struct RegionPoint {
    float x = 0.0f, y = 0.0f, z = 0.0f; // the original's order (+0, +4, +8)
};

// What CheckRegionChange asks and tells, in its order.
class RegionQueries {
public:
    virtual ~RegionQueries() = default;
    virtual bool inside(const RegionPoint& p) = 0;               // scene graph vf +0x4c
    virtual std::uint32_t regions(const RegionPoint& p) = 0;     // scene graph vf +0x24
    virtual bool do_teleport(const RegionPoint& p) = 0;          // PlayerZoneClient::DoTeleport(-1, p)
    virtual bool is_player_spawn() = 0;                          // IsSpawnInfoPlayerSpawn
    virtual void print(std::uint32_t string_id) = 0;             // dsp_chat(getString(id))
    virtual void feet_environment_change(std::uint32_t flags) = 0;
};

struct RegionZone {
    float area_friction = 0.625f;        // 0xddf67c AreaFriction
    float acceleration_friction = 0.8f;  // 0xddf680 AccelerationFriction
    std::int32_t zone_id = 0;            // unk_CurrentZoneID
};

struct RegionChange {
    bool teleported = false; // the return value
    RegionPoint sample;      // where the regions were asked
    bool asked = false;      // the point was inside the world
};

// PlayerClient::CheckRegionChange @0x508e10.
RegionChange check_region_change(RegionSpawn& spawn, const RegionPoint& position, const RegionZone& zone,
                                 RegionQueries& q, const Fpu& fpu = Fpu{});

// ---- the liquid region under the feet -------------------------------------

// PlayerClient::SetFeetWetness @0x59ec30: the sound of the type of the region
// under the feet changing. The type is the region flags' lowest byte, kept in
// the spawn's +0xa1; the sound plays only on a change.
//   - to type 7, or from a nonzero type to 0: 0x63 (out of the water);
//   - to types 5, 6, 8, 9 or 10: 0x64 (into the water) when the vertical speed
//     is at least 1 or at most -1 (a NaN is neither), else nothing;
//   - to any other type: nothing.
// The type values are assets::RegionType's (assets/regions.h).
inline constexpr std::uint8_t kRegionWaterType = 5;         // "WT", EQG "AWT"
inline constexpr std::uint8_t kRegionSlimeType = 6;         // "SL"
inline constexpr std::uint8_t kRegionLavaType = 7;          // "LA", EQG "ALV"
inline constexpr std::uint8_t kRegionFreezingWaterType = 8; // "VW", EQG "AVW"
inline constexpr std::uint8_t kRegionWater2Type = 9;        // "W2"
inline constexpr std::uint8_t kRegionWater3Type = 10;       // "W3"
inline constexpr float kWaterVerticalSpeed = 1.0f;          // 0x59ec6b: >= 1 or <= -1
class FeetWetness {
public:
    std::optional<std::int32_t> change(std::uint32_t flags, float speed_z);
    void reset() { type_ = 0; }
    std::uint8_t type() const { return type_; } // +0xa1

private:
    std::uint8_t type_ = 0;
};

} // namespace hailstone::game::mechanics
