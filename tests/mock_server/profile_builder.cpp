#include "mock_server/profile_builder.h"

#include <algorithm>
#include <span>
#include <string_view>

#include "core/byte_io.h"
#include "net/opcodes.h"
#include "net/packet_io.h"

namespace hailstone::test {

using namespace hailstone::net;

namespace {

void cstr(ByteWriter& w, std::string_view s) {
    w.bytes({reinterpret_cast<const std::byte*>(s.data()), s.size()});
    w.le<std::uint8_t>(0);
}

} // namespace

AppPacket build_player_profile(const ProfileSpec& spec) {
    ByteWriter w;
    auto u32 = [&](std::uint32_t v) { w.le(v); };
    auto u8 = [&](std::uint8_t v) { w.le(v); };
    auto zeros32 = [&](int n) { for (int i = 0; i < n; ++i) u32(0); };
    u32(0); u32(0); u32(0); u32(0);
    u8(spec.gender);      // gender
    u32(spec.race);       // race
    u8(spec.class_);      // class
    u8(spec.level); u8(spec.level); // level x2
    u32(5);
    for (int r = 0; r < 5; ++r) { u32(202 + static_cast<std::uint32_t>(r)); w.f32(1); w.f32(2); w.f32(3); w.f32(4); }
    u32(spec.deity);      // deity
    u32(0);              // intoxication
    u32(10); zeros32(10);
    u32(22);
    for (int r = 0; r < 9; ++r) { u32(static_cast<std::uint32_t>(r) + 100); zeros32(4); }
    zeros32(13 * 5);
    u32(9); zeros32(9 * 5);
    u32(9); for (int r = 0; r < 7; ++r) u32(0xff00ff00u); zeros32(2);
    u32(9); for (int r = 0; r < 7; ++r) u32(0xff00ff00u); zeros32(2);
    u8(4); u8(5); u32(0); u8(6); u8(7); u8(8); u8(9); u8(10);
    u32(1); u32(2); u32(3);
    u8(0); u8(0xff); u8(1); u8(0xff); u8(1);
    w.f32(5.0f); w.f32(3.0f); w.f32(2.5f); w.f32(5.5f);
    u32(0); u32(0);
    u32(5);              // points
    u32(spec.mana);       // mana
    u32(spec.cur_hp);     // cur_hp
    for (std::uint32_t s : {101u, 102u, 103u, 104u, 105u, 106u, 107u}) u32(s); // STR STA CHA DEX INT AGI WIS
    zeros32(7);
    u32(300); for (int r = 0; r < 300; ++r) { u32(r == 0 ? 7 : 0); u32(r == 0 ? 3 : 0); u32(0); }
    u32(100); for (int r = 0; r < 100; ++r) u32(static_cast<std::uint32_t>(r));
    u32(25); zeros32(25);
    u32(300); zeros32(300);
    u32(20); for (auto t : spec.combat_ability_timers) u32(t);
    u32(20); for (auto t : spec.linked_timers) u32(t);
    u32(100); for (auto t : spec.item_recast_timers) u32(t);
    u32(720); for (int r = 0; r < 720; ++r) u32(r == 0 ? 200 : 0xffffffffu);
    u32(16); for (int r = 0; r < 12; ++r) u32(r == 0 ? 200 : 0xffffffffu); for (int r = 0; r < 4; ++r) u32(0xffffffffu);
    u32(13); zeros32(13);
    u8(0);
    u32(42);
    for (int r = 0; r < 42; ++r) {
        u8(0); w.f32(r == 0 ? 1.0f : 0.0f); u32(r == 0 ? 0x717fd : 0); u8(0); u32(0); u32(r == 0 ? 60 : 0);
        u8(r == 0 ? 50 : 0); u32(r == 0 ? 278 : 0); u8(r == 0 ? 2 : 0); u32(0); u32(0); u32(0);
        w.zeros(44);
    }
    u32(1); u32(2); u32(3); u32(4);       // coin
    u32(5); u32(6); u32(7); u32(8);       // cursor coin
    zeros32(3);
    u32(6000); u32(5999);                  // thirst, hunger
    u32(10);                               // aapoints_spent
    u32(5); zeros32(5); u32(33);           // tabs ... unspent
    u8(0); u8(0);
    u32(20);
    for (int r = 0; r < 20; ++r) { cstr(w, ""); for (int j = 0; j < 4; ++j) { cstr(w, ""); u32(0); u32(0xffffffffu); } }
    u32(5);
    for (int r = 0; r < 5; ++r) { cstr(w, ""); u32(0); u32(0xffffffffu); }
    u32(0xffffffffu); u32(123); u32(234); u32(345);
    for (std::uint32_t s : {0x19u, 0x19u, 0x19u, 0xfu, 0xfu, 0xfu, 0xfu}) u32(s);
    zeros32(5);
    u32(20);
    zeros32(4);
    u32(spec.endurance);                    // endurance
    zeros32(2);
    u32(64); w.fixed_string(spec.name, 64);
    u32(32); w.fixed_string(spec.last_name, 32);
    u32(1600000000); u32(1600000000); u32(1700000000); u32(5000); u32(0); u32(0x7ffff);
    u32(32); for (int r = 0; r < 32; ++r) u8(r == 0 ? 100 : 0);
    w.le<std::uint16_t>(spec.zone_id); w.le<std::uint16_t>(spec.zone_instance);
    w.f32(spec.y); w.f32(spec.x); w.f32(spec.z); w.f32(spec.heading); // y x z heading
    u8(0); u8(0); u8(0); u8(1);           // gm
    u32(42);
    u8(1); u32(0); u8(0); u32(0);
    w.le<std::uint64_t>(123456789ull);
    u8(5);
    u32(11); u32(12); u32(13); u32(14);
    u32(15); zeros32(3); u32(0);
    u32(0xffffffffu); u32(0xffffffffu);
    u32(1200); u32(0); u32(345); u32(0); // career and current tribute points
    u8(0); u8(0);
    u32(5); u32(7); u32(2); for (int r = 1; r < 5; ++r) { u32(0xffffffffu); u32(0); }
    u32(10); for (int r = 0; r < 10; ++r) { u32(0xffffffffu); u32(0); }
    zeros32(6);
    w.zeros(125);
    zeros32(2); zeros32(4); zeros32(3);
    u32(64); w.zeros(64);
    w.zeros(3); zeros32(2); w.zeros(3); u32(0); w.zeros(3); u32(0); u8(0); u32(0);
    u32(64); w.zeros(64);
    u32(64); w.zeros(64);
    u32(0);
    w.zeros(320); w.zeros(343);
    u32(spec.character_id);                // char_id
    u8(1);                                 // leadAAActive
    u32(6); u32(0);
    for (auto points : spec.adventure_points) u32(points);
    u32(spec.adventure_points_available);
    w.f64(750.0); w.f64(1500.0);           // group / raid leadership experience
    u32(3); u32(7);                        // group / raid leadership points
    u32(64); u32(2); zeros32(15); u32(1); zeros32(47); // ranks: Mark NPC 2, raid Mark NPC 1
    u32(0);
    zeros32(7);
    cstr(w, ""); zeros32(6);
    cstr(w, ""); zeros32(6);
    u32(0);
    u32(50); for (int r = 0; r < 50; ++r) { cstr(w, ""); zeros32(6); }
    u32(55555);                            // expAA
    u32(spec.last_fellowship_join);
    w.le(spec.fellowship_vitality_normal);
    u32(spec.fellowship_vitality_aa);
    u8(0); u8(0); u8(0); u8(0);
    u32(65);
    u8(1);                                 // showhelm
    u32(30);                               // rest timer
    u32(1024); w.zeros(1024);
    zeros32(2);
    u8(0);
    auto data = w.take();
    // Checksum size at +4 = size - 9; CRC of [8, size - 1) at +0.
    ByteWriter head;
    std::uint32_t crc = eq_crc32(std::span<const std::byte>(data).subspan(8, data.size() - 9));
    head.le(crc);
    head.le(static_cast<std::uint32_t>(data.size() - 9));
    auto h = head.take();
    std::copy(h.begin(), h.end(), data.begin());
    return AppPacket(to_wire(Op::PlayerProfile), std::move(data));
}

} // namespace hailstone::test
