// Group and pet packets, byte for byte against the layouts in the server's
// RoF2 translator (common/patches/rof2.cpp, rof2_structs.h), its pass-through
// structs (eq_packet_structs.h) and the NMS handlers (zone/pets.cpp
// SendPetList); and the group / pet state rules (game/group.h).

#include <doctest/doctest.h>

#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "game/group.h"
#include "game/leadership.h"
#include "net/group_packets.h"
#include "net/opcodes.h"
#include "net/pet_packets.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

std::vector<std::uint8_t> bytes_of(const AppPacket& p) {
    std::vector<std::uint8_t> out;
    for (std::byte b : p.data) out.push_back(std::to_integer<std::uint8_t>(b));
    return out;
}

AppPacket packet(Op op, const std::vector<std::uint8_t>& b) {
    std::vector<std::byte> d;
    for (auto v : b) d.push_back(std::byte{v});
    return AppPacket(to_wire(op), std::move(d));
}

void put_str(std::vector<std::uint8_t>& b, std::size_t at, const char* s) { std::memcpy(b.data() + at, s, std::strlen(s)); }
void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
void append_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void append_str(std::vector<std::uint8_t>& b, const char* s) {
    b.insert(b.end(), s, s + std::strlen(s));
    b.push_back(0);
}

} // namespace

TEST_CASE("group: opcodes are the RoF2 ones") {
    CHECK(to_wire(Op::GroupInvite) == 0x6110);
    CHECK(to_wire(Op::GroupInvite2) == 0x32c2);
    CHECK(to_wire(Op::GroupFollow) == 0x1649);
    CHECK(to_wire(Op::GroupFollow2) == 0x2060);
    CHECK(to_wire(Op::CancelInvite) == 0x2a50);
    CHECK(to_wire(Op::GroupDisband) == 0x4c10);
    CHECK(to_wire(Op::GroupDisbandYou) == 0x1ae5);
    CHECK(to_wire(Op::GroupDisbandOther) == 0x74da);
    CHECK(to_wire(Op::GroupUpdate) == 0x3abb);
    CHECK(to_wire(Op::GroupUpdateB) == 0x6194);
    CHECK(to_wire(Op::GroupLeaderChange) == 0x21b4);
    CHECK(to_wire(Op::GroupMakeLeader) == 0x4229);
    CHECK(to_wire(Op::PetCommands) == 0x0159);
}

TEST_CASE("group: OP_GroupInvite is GroupInvite_Struct (148 bytes)") {
    GroupInvite inv;
    inv.invitee = "Nmscgrptwo";
    inv.inviter = "Nmscgrpone";
    auto p = encode(inv);
    CHECK(p.opcode == 0x6110);
    std::vector<std::uint8_t> want(148, 0);
    put_str(want, 0, "Nmscgrptwo");
    put_str(want, 64, "Nmscgrpone");
    CHECK(bytes_of(p) == want);
    CHECK(encode(inv, true).opcode == 0x32c2);

    // As EQEmu sends it (ENCODE(OP_GroupInvite): invitee_name = name1, inviter_name = name2).
    auto d = decode_group_invite(packet(Op::GroupInvite, want));
    REQUIRE(d);
    CHECK(d->invitee == "Nmscgrptwo");
    CHECK(d->inviter == "Nmscgrpone");
    CHECK(d->follow_cookie == 0);
    CHECK_FALSE(decode_group_invite(packet(Op::GroupInvite, std::vector<std::uint8_t>(100, 0))));
}

TEST_CASE("group: OP_GroupFollow is GroupFollow_Struct (152 bytes)") {
    GroupFollow f;
    f.inviter = "Leader";
    f.invitee = "Joiner";
    f.unknown148 = 0x11223344;
    auto p = encode(f, false);
    CHECK(p.opcode == 0x1649);
    std::vector<std::uint8_t> want(152, 0);
    put_str(want, 0, "Leader");
    put_str(want, 64, "Joiner");
    put32(want, 148, 0x11223344);
    CHECK(bytes_of(p) == want);
    CHECK(encode(f, true).opcode == 0x2060);
    auto d = decode_group_follow(p);
    REQUIRE(d);
    CHECK(d->inviter == "Leader");
    CHECK(d->invitee == "Joiner");
}

TEST_CASE("group: OP_CancelInvite is GroupCancel_Struct with the reason at +148") {
    GroupCancel c{"Leader", "Joiner", kCancelDeclined};
    auto p = encode(c);
    CHECK(p.opcode == 0x2a50);
    std::vector<std::uint8_t> want(152, 0);
    put_str(want, 0, "Leader");
    put_str(want, 64, "Joiner");
    put32(want, 148, 3);
    CHECK(bytes_of(p) == want);
    auto d = decode_group_cancel(p);
    REQUIRE(d);
    CHECK(d->reason == 3);
}

TEST_CASE("group: OP_GroupDisband and the disband notices are GroupGeneric_Struct (148 bytes)") {
    auto p = encode_group_generic(Op::GroupDisband, {"Member", "Leader"});
    CHECK(p.opcode == 0x4c10);
    std::vector<std::uint8_t> want(148, 0);
    put_str(want, 0, "Member");
    put_str(want, 64, "Leader");
    CHECK(bytes_of(p) == want);
    auto d = decode_group_generic(packet(Op::GroupDisbandOther, want));
    REQUIRE(d);
    CHECK(d->name1 == "Member");
    CHECK(d->name2 == "Leader");
}

TEST_CASE("group: OP_GroupUpdate is the RoF2 GroupJoin_Struct (148 bytes, member name at +64)") {
    std::vector<std::uint8_t> raw(148, 0);
    put_str(raw, 64, "Newbie");
    raw[128] = 0;
    put32(raw, 132, 12);
    auto d = decode_group_join(packet(Op::GroupUpdate, raw));
    REQUIRE(d);
    CHECK(d->member_name == "Newbie");
    CHECK(d->owner_name.empty());
    CHECK(d->level == 12);
    GroupJoin j;
    j.member_name = "Newbie";
    j.level = 12;
    CHECK(bytes_of(encode(j)) == raw);
}

TEST_CASE("group: OP_GroupUpdateB as ENCODE(OP_GroupUpdate) writes it from a GroupUpdate2_Struct") {
    // rof2.cpp: u32 0, u32 member count, leader name; then per member u32
    // index, name, u16 0, u8 0 (empty owner string), u32 level (0x46 for the
    // receiver, 0x41 for the rest), u8 ×3, u32 0, u32 0.
    std::vector<std::uint8_t> raw;
    append_u32(raw, 0);
    append_u32(raw, 2);
    append_str(raw, "Nmscgrpone");
    append_u32(raw, 0);
    append_str(raw, "Nmscgrptwo");
    raw.push_back(0); raw.push_back(0); // u16 merc
    raw.push_back(0);                   // owner ""
    append_u32(raw, 0x46);
    raw.push_back(0); raw.push_back(0); raw.push_back(0);
    append_u32(raw, 0);
    append_u32(raw, 0);
    append_u32(raw, 1);
    append_str(raw, "Nmscgrpone");
    raw.push_back(0); raw.push_back(0);
    raw.push_back(0);
    append_u32(raw, 0x41);
    raw.push_back(0); raw.push_back(0); raw.push_back(0);
    append_u32(raw, 0);
    append_u32(raw, 0);
    auto d = decode_group_update_b(packet(Op::GroupUpdateB, raw));
    REQUIRE(d);
    CHECK(d->leader == "Nmscgrpone");
    REQUIRE(d->members.size() == 2);
    CHECK(d->members[0].slot == 0);
    CHECK(d->members[0].name == "Nmscgrptwo");
    CHECK(d->members[0].level == 0x46);
    CHECK(d->members[1].slot == 1);
    CHECK(d->members[1].name == "Nmscgrpone");
    CHECK(bytes_of(encode(*d)) == raw);

    // Client::SendGroupCreatePacket: count 1, empty leader name, the real level.
    std::vector<std::uint8_t> create;
    append_u32(create, 0);
    append_u32(create, 1);
    create.push_back(0);
    append_u32(create, 0);
    append_str(create, "Nmscgrpone");
    create.push_back(0); create.push_back(0);
    create.push_back(0);
    append_u32(create, 3);
    create.push_back(0); create.push_back(0); create.push_back(0);
    append_u32(create, 0);
    append_u32(create, 0);
    CHECK(create.size() == 32 + std::strlen("Nmscgrpone")); // EQEmu's allocation: 32 + strlen(name)
    auto c = decode_group_update_b(packet(Op::GroupUpdateB, create));
    REQUIRE(c);
    CHECK(c->leader.empty());
    REQUIRE(c->members.size() == 1);
    CHECK(c->members[0].level == 3);

    // Truncated or absurd packets are refused.
    raw.resize(raw.size() - 3);
    CHECK_FALSE(decode_group_update_b(packet(Op::GroupUpdateB, raw)));
}

TEST_CASE("group: OP_GroupLeaderChange has the leader at +64 (148 bytes)") {
    auto p = encode_group_leader_change("Boss");
    std::vector<std::uint8_t> want(148, 0);
    put_str(want, 64, "Boss");
    CHECK(bytes_of(p) == want);
    CHECK(decode_group_leader_change(p) == std::optional<std::string>("Boss"));
}

TEST_CASE("group: OP_GroupMakeLeader is GroupMakeLeader_Struct (456 bytes) as Cmd_makeleader fills it") {
    GroupMakeLeader m{"Old", "New"};
    auto p = encode(m);
    CHECK(p.opcode == 0x4229);
    std::vector<std::uint8_t> want(456, 0);
    put32(want, 0, 8);
    put_str(want, 4, "Old");
    put_str(want, 68, "New");
    CHECK(bytes_of(p) == want);
    auto d = decode_group_make_leader(p);
    REQUIRE(d);
    CHECK(d->new_leader == "New");

    auto mentor = encode_group_mentor(0, {});
    CHECK(mentor.opcode == 0x3342);
    CHECK(bytes_of(mentor) == std::vector<std::uint8_t>(68, 0));
}

TEST_CASE("group: OP_MobManaUpdate / OP_MobEnduranceUpdate are 3 bytes") {
    auto p = encode_mob_percent(Op::MobManaUpdate, {0x1234, 57});
    CHECK(p.opcode == 0x2404);
    CHECK(bytes_of(p) == std::vector<std::uint8_t>{0x34, 0x12, 57});
    auto d = decode_mob_percent(p);
    REQUIRE(d);
    CHECK(d->spawn_id == 0x1234);
    CHECK(d->percent == 57);
}

TEST_CASE("pets: OP_PetCommands is 12 bytes: command, target, flag") {
    PetCommandPacket c;
    c.command = kPetAttack;
    c.target = 0x0203;
    auto p = encode(c);
    CHECK(p.opcode == 0x0159);
    CHECK(bytes_of(p) == std::vector<std::uint8_t>{2, 0, 0, 0, 3, 2, 0, 0, 0, 0, 0, 0});
    auto d = decode_pet_command(p);
    REQUIRE(d);
    CHECK(d->command == 2);
    CHECK(d->target == 0x203);
}

TEST_CASE("pets: OP_PetCommandState, OP_PetHoTT") {
    auto p = encode(PetCommandState{5, 1});
    CHECK(bytes_of(p) == std::vector<std::uint8_t>{5, 0, 0, 0, 1, 0, 0, 0});
    auto d = decode_pet_command_state(p);
    REQUIRE(d);
    CHECK(d->button == 5);
    CHECK(d->state == 1);
    CHECK(decode_pet_hott(packet(Op::PetHoTT, {0x10, 0x01, 0, 0})) == std::optional<std::uint32_t>(0x110));
}

TEST_CASE("group state: add, remove, leader, full") {
    game::GroupState g;
    CHECK(g.add("Alpha", 1, false, {}));
    CHECK_FALSE(g.add("alpha", 1, false, {})); // already there (case-insensitive, as CGroup_GetMemberByName)
    CHECK(g.active);
    for (const char* n : {"B", "C", "D", "E", "F"}) CHECK(g.add(n, 1, false, {}));
    CHECK(g.count() == 6);
    CHECK_FALSE(g.add("G", 1, false, {}));
    CHECK(g.remove("C"));
    CHECK(g.slot_of("C") == -1);
    CHECK(g.add("G", 1, false, {}));
    CHECK(g.slot_of("G") == 2); // the freed slot
    g.leader = "Alpha";
    CHECK(g.is_leader("ALPHA"));
    g.clear();
    CHECK_FALSE(g.active);
    CHECK(g.count() == 0);
}

TEST_CASE("group state: the leader leaving clears the leader; the last one leaving ends the group") {
    game::GroupState g;
    CHECK(g.add("Alpha", 1, false, {}));
    CHECK(g.add("Beta", 1, false, {}));
    g.leader = "Alpha";
    g.mana_percent[7] = 50;
    g.endurance_percent[7] = 60;
    g.mana_percent[8] = 70;
    CHECK(g.remove("alpha", 7));
    CHECK(g.leader.empty());
    CHECK(g.active);
    CHECK_FALSE(g.mana_percent.contains(7));
    CHECK_FALSE(g.endurance_percent.contains(7));
    CHECK(g.mana_percent.contains(8));
    CHECK(g.remove("Beta"));
    CHECK_FALSE(g.active);
    CHECK(g.mana_percent.empty());
    CHECK_FALSE(g.remove("Beta"));
}

TEST_CASE("pets: /pet words as cmd_pet reads them") {
    using game::parse_pet_command;
    auto c = [](const char* s) { return parse_pet_command(s); };
    CHECK(c("health")->command == 0);
    CHECK(c("report health")->command == 0);
    CHECK(c("leader")->leader_target);
    CHECK(c("follow")->command == 4);
    CHECK(c("guard")->command == 5);
    CHECK(c("sit")->command == 6);
    CHECK(c("sit on")->command == 7);
    CHECK(c("Sit Off")->command == 8);
    CHECK(c("taunt")->command == 12);
    CHECK(c("taunt on")->command == 13);
    CHECK(c("no taunt")->command == 14);
    CHECK(c("hold")->command == 15);
    CHECK(c("ghold off")->command == 20);
    CHECK(c("spellhold")->command == 21);
    CHECK(c("no cast")->command == 22);
    CHECK(c("focus")->command == 24);
    CHECK(c("feign")->command == 27);
    CHECK(c("back off")->command == 28);
    CHECK(c("calm down")->command == 28);
    CHECK(c("get lost")->command == 29);
    CHECK(c("leave")->command == 29);
    CHECK(c("target")->command == 30);
    CHECK(c("regroup on")->command == 32);
    CHECK(c("stop")->command == 9);
    auto q = c("qattack");
    CHECK(q->command == 3);
    CHECK(q->uses_target);
    auto a = c("attack");
    CHECK(a->command == 2);
    CHECK(a->uses_target);
    CHECK(a->named_target.empty());
    auto k = c("kill large rat");
    CHECK(k->command == 2);
    CHECK(k->named_target == "large_rat");
    CHECK_FALSE(c("dance"));
    CHECK_FALSE(c(""));
}

TEST_CASE("leadership: the toggle, a purchase, the rank and experience updates, and the ability table") {
    CHECK(bytes_of(encode_leadership_exp_toggle(true)) == std::vector<std::uint8_t>{1});
    CHECK(bytes_of(encode_purchase_leadership(16)) == std::vector<std::uint8_t>{16, 0, 0, 0});
    std::vector<std::uint8_t> r(12, 0);
    put32(r, 0, 9);
    put32(r, 4, 2);
    put32(r, 8, 5);
    auto rank = decode_leadership_rank(packet(Op::UpdateLeadershipAA, r));
    REQUIRE(rank);
    CHECK(rank->ability == 9);
    CHECK(rank->rank == 2);
    CHECK(rank->points_left == 5);
    LeadershipExp e{250.0, 1800.0, 3, 1};
    auto eb = bytes_of(encode(e));
    REQUIRE(eb.size() == 28); // RoF2 LeadershipExpUpdate_Struct
    auto back = decode_leadership_exp(encode(e));
    REQUIRE(back);
    CHECK(back->group_exp == 250.0);
    CHECK(back->raid_exp == 1800.0);
    CHECK(back->group_points == 3);
    CHECK(back->raid_points == 1);

    CHECK(game::leadership_ability(0).name == "Mark NPC");
    CHECK(game::leadership_max_rank(0) == 3);
    CHECK(game::leadership_next_cost(0, 0) == 3);
    CHECK(game::leadership_next_cost(0, 3) == 0);
    CHECK(game::leadership_max_rank(9) == 5); // offense enhancement
    CHECK(game::leadership_ability(5).name.empty());
    CHECK(game::leadership_ability(18).name.empty()); // unavailable waypoint, never raid main assist
    CHECK(game::leadership_ability(19).name == "Delegate Main Assist");
    CHECK(game::leadership_max_rank(19) == 3);
    CHECK(game::leadership_next_cost(19, 2) == 8);
    CHECK(game::leadership_ability(20).name == "Delegate Mark NPC");
    CHECK(game::leadership_next_cost(20, 2) == 6);
    CHECK(game::leadership_hot_button_def(18).count == 0);
    CHECK(game::leadership_hot_button_def(19).count == 1);
    CHECK(game::leadership_next_cost(24, 4) == 10);
    CHECK(game::leadership_ability(40).name.empty());
}

TEST_CASE("leadership gauge: the server's f64 experience is clamped before it becomes an int") {
    CHECK(game::leadership_gauge(250.0, game::kGroupLeadershipExpPerPoint) == 250);
    CHECK(game::leadership_gauge(1999.0, game::kRaidLeadershipExpPerPoint) == 999);
    CHECK(game::leadership_gauge(-5.0, 1000.0) == 0);
    CHECK(game::leadership_gauge(1e300, 1000.0) == 1000); // was INT_MIN on x86 (undefined), so an empty gauge
    CHECK(game::leadership_gauge(std::numeric_limits<double>::infinity(), 1000.0) == 1000);
    CHECK(game::leadership_gauge(std::numeric_limits<double>::quiet_NaN(), 1000.0) == 0);
}

TEST_CASE("raid delegation: decode the complete EQEmu pass-through record") {
    std::vector<std::uint8_t> raw(92, 0);
    put32(raw, 0, 3);
    put32(raw, 4, 3); // third assist is one-based in live EQEmu assignments
    put32(raw, 20, 1234);
    put_str(raw, 28, "ThirdAssist");
    auto decoded = decode_delegate_ability(packet(Op::RaidDelegateAbility, raw));
    REQUIRE(decoded);
    CHECK(decoded->ability == 3);
    CHECK(decoded->member == 3);
    CHECK(decoded->spawn_id == 1234);
    CHECK(decoded->name == "ThirdAssist");
    raw.resize(91);
    CHECK_FALSE(decode_delegate_ability(packet(Op::RaidDelegateAbility, raw)));
}
