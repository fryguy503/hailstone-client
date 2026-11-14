#pragma once

// OP_PlayerProfile as the server writes it (rof2.cpp ENCODE(OP_PlayerProfile)). The client has no encoder for it
// (a ~25 KB packet of count-prefixed arrays), so tests build the bytes here, field by field.
// Used by the mock server and by tests/net_packets_test.cpp.

#include <array>
#include <cstdint>
#include <string>

#include "net/packet.h"

namespace hailstone::test {

struct ProfileSpec {
    std::string name = "Gennro";
    std::string last_name = "Smith";
    std::uint8_t gender = 1;
    std::uint32_t race = 522;
    std::uint8_t class_ = 16; // NMS multiclass characters are Bard
    std::uint8_t level = 65;
    std::uint32_t deity = 396;
    std::uint32_t mana = 1234;
    std::uint32_t cur_hp = 4321;
    std::uint32_t endurance = 777;
    std::uint16_t zone_id = 202;
    std::uint16_t zone_instance = 0;
    float y = -148.0f, x = -285.0f, z = 3.5f, heading = 128.0f; // the server's order
    std::uint32_t character_id = 9876;
    std::uint32_t last_fellowship_join = 0;
    std::uint64_t fellowship_vitality_normal = 0;
    std::uint32_t fellowship_vitality_aa = 0;
    std::array<std::uint32_t, 20> combat_ability_timers{}; // EQEmu sends zeros
    std::array<std::uint32_t, 20> linked_timers{};         // EQEmu's recastTimers
    std::array<std::uint32_t, 100> item_recast_timers{};   // EQEmu sends zeros
};

// Array sizes as the server sends them: AA 300, skills 100, innate 25, disciplines 300, recast 20, spell book 720,
// gems 12 of 16, buffs 42, bandoliers 20 x 4, potion belt 5, languages 32, tributes 5. The checksum fields are set.
net::AppPacket build_player_profile(const ProfileSpec& spec = {});

} // namespace hailstone::test
