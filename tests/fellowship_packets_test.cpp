#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <string>

#include "core/byte_io.h"
#include "fellowship_test_packets.h"
#include "fuzz_support.h"
#include "net/fellowship_packets.h"
#include "net/opcodes.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {
AppPacket roster(std::size_t slots = 12, std::uint32_t count = 2, bool camp = true) {
    ByteWriter w;
    w.le<std::uint32_t>(5);
    w.le<std::uint32_t>(123);
    w.fixed_string("Fellowship", 64);
    w.fixed_string("Welcome", 1024);
    w.le(count);
    for (std::size_t i = 0; i < slots; ++i) {
        w.le(static_cast<std::uint32_t>(i + 100));
        w.fixed_string("Member" + std::to_string(i), 64);
        w.le<std::uint32_t>(i == 1 ? 0 : 202);
        w.le<std::uint32_t>(65);
        w.le<std::uint32_t>(2);
        w.le<std::uint32_t>(1700000000);
    }
    w.le<std::uint32_t>(1);
    w.zeros(slots * 32);
    for (std::size_t i = 0; i < slots; ++i) w.le<std::uint8_t>(i == 0 || i == slots - 1 ? 1 : 0);
    for (std::size_t i = 0; i < slots; ++i) w.le<std::uint8_t>(i == slots - 1 ? 1 : 0);
    w.zeros(slots);
    if (camp) {
        w.f32(20); w.f32(10); w.f32(30);
        w.le<std::uint32_t>(202);
        w.le<std::uint32_t>(1700003600);
        w.le<std::uint32_t>(1700003600);
        w.le<std::uint32_t>(0);
        w.le<std::uint32_t>(1);
    }
    return AppPacket(kOpFellowshipRoster, w.take());
}
void put32(AppPacket& p, std::size_t at, std::uint32_t value) {
    ByteWriter w; w.le(value);
    std::copy(w.data().begin(), w.data().end(), p.data.begin() + static_cast<std::ptrdiff_t>(at));
}
}

TEST_CASE("fellowship packets: all native routes have symbolic opcode mappings") {
    for (const auto& [op, wire] : {std::pair{Op::FellowshipUpdate, kOpFellowshipRequest},
            std::pair{Op::FellowshipCampRefresh, kOpCampsiteKitRequest},
            std::pair{Op::FellowshipInvite, kOpFellowshipInvite},
            std::pair{Op::FellowshipUpdateResponse, kOpFellowshipRoster},
            std::pair{Op::FellowshipUpdateAltResponse, kOpCampsiteKitList},
            std::pair{Op::FellowshipVitalityUpdate, kOpFellowshipVitality}}) {
        CHECK(to_wire(op) == wire);
        CHECK(from_wire(wire) == op);
        CHECK(std::string(op_name(op)) != "Unknown");
    }
}

TEST_CASE("fellowship packets: native requests retain exact offsets and terminators") {
    auto p = encode_fellowship_request(FellowshipAction::Motd, 0x12345678, std::string(2000, 'm'), 77);
    CHECK(p.opcode == 0x7121);
    REQUIRE(p.data.size() == 0x434);
    ByteReader r(p.data);
    CHECK(r.le<std::uint32_t>() == 2);
    CHECK(r.le<std::uint32_t>() == 0x12345678);
    r.skip(12);
    CHECK(r.fixed_string(1024) == std::string(1023, 'm'));
    CHECK(p.data[0x413] == std::byte{0});
    r.skip(20);
    CHECK(r.le<std::uint32_t>() == 77);
    CHECK(r.remaining() == 8);
    auto kits = encode_fellowship_kit_request(true, 0xffffffff);
    CHECK(kits.opcode == 0x0c12);
    REQUIRE(kits.data.size() == 8);
    ByteReader kr(kits.data);
    CHECK(kr.le<std::uint32_t>() == 1);
    CHECK(kr.le<std::uint32_t>() == 0xffffffff);
}

TEST_CASE("fellowship packets: native and extended roster arrays stay aligned through member64") {
    for (const auto slots : {12u, 13u, 32u, 64u}) {
        CAPTURE(slots);
        auto p = roster(slots, slots);
        CHECK(p.data.size() == 1136 + slots * 119);
        auto v = decode_fellowship_roster(p);
        REQUIRE(v);
        REQUIRE(v->members.size() == slots);
        CHECK(v->id == 123);
        CHECK(v->name == "Fellowship");
        CHECK(v->motd == "Welcome");
        CHECK(v->is_leader("member0"));
        CHECK_FALSE(v->is_leader("Member1"));
        CHECK(v->members.back().name == "Member" + std::to_string(slots - 1));
        CHECK(v->members.back().sharing);
        CHECK(v->members.back().capped);
        CHECK(v->members[1].offline);
        CHECK(v->member(100) == &v->members.front());
        CHECK(v->member("MEMBER1") == &v->members[1]);
        CHECK(v->camp.active);
        CHECK(v->camp.x == 10.0f);
        CHECK(v->camp.y == 20.0f);
        CHECK(v->camp.z == 30.0f);
        CHECK(v->camp.zone_id == 202);
        CHECK(v->camp.expires == 1700003600);
    }
    auto without = decode_fellowship_roster(roster(12, 2, false));
    REQUIRE(without);
    CHECK_FALSE(without->camp.active);
    auto empty = decode_fellowship_roster(roster(12, 0));
    REQUIRE(empty);
    CHECK_FALSE(empty->active());
    CHECK(empty->id == 123);
}

TEST_CASE("fellowship packets: reject invalid capacities counts duplicate identities and camp coordinates") {
    CHECK_FALSE(decode_fellowship_roster(roster(65, 65)));
    CHECK_FALSE(decode_fellowship_roster(roster(11, 11)));
    CHECK_FALSE(decode_fellowship_roster(roster(12, 13)));
    auto p = roster(); put32(p, 0, 6);
    CHECK_FALSE(decode_fellowship_roster(p));
    p = roster(); put32(p, 4, 0);
    CHECK_FALSE(decode_fellowship_roster(p));
    p = roster(); put32(p, 1100 + 84, 100);
    CHECK_FALSE(decode_fellowship_roster(p));
    p = roster();
    std::copy_n(p.data.begin() + 1104, 64, p.data.begin() + 1104 + 84);
    CHECK_FALSE(decode_fellowship_roster(p));
    p = roster(); put32(p, 0x9e4, 0x7fc00000);
    CHECK_FALSE(decode_fellowship_roster(p));
    p = roster(); p.data.push_back(std::byte{});
    CHECK_FALSE(decode_fellowship_roster(p));
    p = roster(); p.opcode = 0;
    CHECK_FALSE(decode_fellowship_roster(p));
    p = roster();
    for (std::size_t size = 0; size < p.data.size(); ++size) {
        if (size == 0x9e4) continue; // native roster without camp is valid
        CAPTURE(size);
        AppPacket short_p(p.opcode, {p.data.begin(), p.data.begin() + static_cast<std::ptrdiff_t>(size)});
        CHECK_FALSE(decode_fellowship_roster(short_p));
    }
}

TEST_CASE("fellowship packets: camp list bounds invitation identity and vitality widths") {
    ByteWriter kits;
    kits.le<std::uint32_t>(2);
    for (std::uint32_t id : {7u, 9u}) {
        kits.le<std::uint8_t>(id == 7 ? 1 : 0);
        kits.le(id); kits.le<std::uint32_t>(8000 + id);
        kits.fixed_string("Camp", 5);
    }
    AppPacket p(kOpCampsiteKitList, kits.take());
    auto v = decode_fellowship_kits(p);
    REQUIRE(v);
    REQUIRE(v->size() == 2);
    CHECK((*v)[0].available);
    CHECK_FALSE((*v)[1].available);
    CHECK((*v)[1].description_id == 8009);
    CHECK((*v)[1].name == "Camp");
    p.data.pop_back();
    CHECK_FALSE(decode_fellowship_kits(p));
    put32(p, 0, 0xffffffff);
    CHECK_FALSE(decode_fellowship_kits(p));
    p = AppPacket(kOpFellowshipInvite, std::vector<std::byte>(1080));
    put32(p, 0, 5); put32(p, 4, 777);
    auto invite = decode_fellowship_invite(p);
    REQUIRE(invite);
    CHECK(invite->inviter_id == 777);
    put32(p, 4, 0);
    CHECK_FALSE(decode_fellowship_invite(p));
    ByteWriter vit;
    vit.le<std::uint64_t>(0x123456789abcdef0);
    vit.le<std::uint64_t>(0xfedcba9876543210);
    vit.le<std::uint32_t>(0xfffffffe); vit.le<std::uint32_t>(0);
    vit.le<std::uint32_t>(0xffffffff);
    p = AppPacket(kOpFellowshipVitality, vit.take());
    auto vitality = decode_fellowship_vitality(p);
    REQUIRE(vitality);
    CHECK(vitality->normal == 0x123456789abcdef0);
    CHECK(vitality->normal_cap == 0xfedcba9876543210);
    CHECK(vitality->aa == 0xfffffffe);
    CHECK(vitality->aa_cap == 0xffffffff);
    p.data.pop_back();
    CHECK_FALSE(decode_fellowship_vitality(p));
}

TEST_CASE("fellowship packets: received opcodes exercise valid seeds through truncation and mutation") {
    // The generic decoder corpus uses an unrelated opcode. Retain the received
    // opcode here so malformed payloads also exercise the gated parsers' bodies.
    const std::vector<AppPacket> seeds = {roster(), roster(64, 64),
        test::fellowship_fixture::invite(), test::fellowship_fixture::kits(), test::fellowship_fixture::vitality()};
    const auto feed = [](const AppPacket& p) {
        switch (p.opcode) {
        case kOpFellowshipRoster: (void)decode_fellowship_roster(p); break;
        case kOpFellowshipInvite: (void)decode_fellowship_invite(p); break;
        case kOpCampsiteKitList: (void)decode_fellowship_kits(p); break;
        case kOpFellowshipVitality: (void)decode_fellowship_vitality(p); break;
        default: FAIL("unexpected fixture opcode");
        }
    };
    fuzz::Rng rng(0xf311075);
    for (const auto& seed : seeds) {
        CAPTURE(seed.opcode);
        for (std::size_t n = 0; n <= seed.data.size(); ++n) {
            const AppPacket prefix(seed.opcode, {seed.data.begin(), seed.data.begin() + static_cast<std::ptrdiff_t>(n)});
            CHECK_NOTHROW(feed(prefix));
        }
        for (int i = 0; i < 300; ++i) {
            const AppPacket mutated(seed.opcode, fuzz::mutate(seed.data, rng));
            CHECK_NOTHROW(feed(mutated));
        }
    }
}
