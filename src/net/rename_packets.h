#pragma once

// Two server packets that change a spawn after it exists, in EQEmu's layouts (common/eq_packet_structs.h
// GMLastName_Struct, LevelAppearance_Struct) checked against the original's handlers:
// OP_GMLastName (Handle_OP_GMLastName 0x4bbcd0)
// and OP_LevelAppearance (the case at 0x4d0053, which calls 0x58fed0).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "net/packet.h"

namespace hailstone::net {

inline constexpr std::size_t kRenameNameSize = 64; // every name field of OP_GMLastName

// OP_GMLastName (S2C, 200 bytes): name[64] (the key), gmname[64], lastname[64], u16 unknown[4].
struct GMLastName {
    std::string name, gm_name, last_name;
    std::uint32_t apply = 0; // @196 (the original reads a dword there; EQEmu's four u16 hold its flags): 0 = ignored
};
inline constexpr std::size_t kGMLastNameSize = 200;
std::optional<GMLastName> decode_gm_last_name(const AppPacket& p);
AppPacket encode(const GMLastName& v); // server direction, for tests

// OP_LevelAppearance (S2C, 64 bytes): u32 spawn id, then five groups {u32 effect id, u32 attach slot, u32 flag}.
struct LevelAppearanceGroup {
    std::uint32_t effect_id = 0; // EQEmu sends 0x4d..0x51; the original skips a group whose id is not above 0
    std::uint32_t slot = 0;      // index into the spawn model's attach table
    std::uint32_t flag = 0;      // passed on as a byte
};
inline constexpr std::size_t kLevelAppearanceGroups = 5;
struct LevelAppearance {
    std::uint32_t spawn_id = 0;
    std::array<LevelAppearanceGroup, kLevelAppearanceGroups> groups{};
};
inline constexpr std::size_t kLevelAppearanceSize = 64;
std::optional<LevelAppearance> decode_level_appearance(const AppPacket& p);
AppPacket encode(const LevelAppearance& v); // server direction, for tests

// One effect to show, a used group of an OP_LevelAppearance on a spawn the client knows. Taken once a frame with
// GameConnection::take_level_effect_cues().
struct LevelEffectCue {
    std::uint32_t spawn_id = 0;
    std::uint32_t effect_id = 0; // an index into the actor emitter file (actoremittersnew.edd)
    std::uint32_t slot = 0;      // one of the model's ten spell points (assets/model_points.h kSpellPointNames)
    bool flag = false;           // see app/actor_emitters.h start_effect
};
// The most cues kept between two takes (a frame): an older one is dropped for a newer.
inline constexpr std::size_t kLevelEffectCuesKept = 5 * 16;

// The original treats the effect id as a signed int and skips a group whose id is not above 0.
bool level_appearance_group_used(const LevelAppearanceGroup& g);

} // namespace hailstone::net
