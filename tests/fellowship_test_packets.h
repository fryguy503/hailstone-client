#pragma once

#include "core/byte_io.h"
#include "net/fellowship_packets.h"

namespace hailstone::test::fellowship_fixture {

inline net::AppPacket roster(std::uint32_t count = 2, std::size_t capacity = 12, std::uint32_t id = 75,
                             bool other_leader = false) {
    ByteWriter w;
    w.le<std::uint32_t>(5); w.le(id);
    w.fixed_string("Test fellowship", 64); w.fixed_string("Meet at the camp", 1024); w.le(count);
    for (std::size_t i = 0; i < capacity; ++i) {
        const auto index = static_cast<std::uint32_t>(i);
        w.le<std::uint32_t>(i < count ? 9876u + index : 0u);
        w.fixed_string(other_leader && i == 0 ? "OtherLeader" :
                       other_leader && i == 1 ? "Gennro" : i == 0 ? "Gennro" : "Member" + std::to_string(i), 64);
        w.le<std::uint32_t>(202); w.le<std::uint32_t>(65); w.le<std::uint32_t>(16); w.le<std::uint32_t>(1700000000);
    }
    w.le<std::uint32_t>(1); w.zeros(capacity * 32);
    for (std::size_t i = 0; i < capacity; ++i) w.le<std::uint8_t>(i == 1 ? 1 : 0);
    w.zeros(capacity * 2);
    w.f32(20); w.f32(10); w.f32(30); w.le<std::uint32_t>(202);
    w.le<std::uint32_t>(1800000000); w.le<std::uint32_t>(1800000000); w.le<std::uint32_t>(0); w.le<std::uint32_t>(1);
    return net::AppPacket(net::kOpFellowshipRoster, w.take());
}

// RoF2's 0x434-byte invitation: the inviter spawn at 12.
inline net::AppPacket invite(std::uint32_t inviter = 333) {
    ByteWriter w; w.le<std::uint32_t>(5); w.zeros(8); w.le(inviter);
    w.zeros(net::kNativeFellowshipInviteSize - 16);
    return net::AppPacket(net::kOpFellowshipInvite, w.take());
}

// The 1080-byte form (an extension beyond RoF2): the inviter at 4, zero at 12.
inline net::AppPacket extended_invite(std::uint32_t inviter = 333) {
    ByteWriter w; w.le<std::uint32_t>(5); w.le(inviter);
    w.zeros(net::kExtendedFellowshipInviteSize - 8);
    return net::AppPacket(net::kOpFellowshipInvite, w.take());
}

inline net::AppPacket vitality() {
    ByteWriter w; w.le<std::uint64_t>(900); w.le<std::uint64_t>(1200);
    w.le<std::uint32_t>(200); w.le<std::uint32_t>(0); w.le<std::uint32_t>(600);
    return net::AppPacket(net::kOpFellowshipVitality, w.take());
}

inline net::AppPacket kits() {
    ByteWriter w; w.le<std::uint32_t>(1); w.le<std::uint8_t>(1); w.le<std::uint32_t>(17);
    w.le<std::uint32_t>(0); w.fixed_string("Test camp", 10);
    return net::AppPacket(net::kOpCampsiteKitList, w.take());
}

} // namespace hailstone::test::fellowship_fixture
