#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "app/lua_eq.h"
#include "core/byte_io.h"
#include "mock_server/harness.h"
#include "net/dz_packets.h"
#include "net/group_packets.h"
#include "net/social_packets.h"
#include "net/task_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

void raid_update(GameConnection& conn, std::uint32_t action, const std::string& name,
                 const std::string& leader = "Gennro") {
    RaidUpdate update;
    update.general.action = action;
    update.general.player_name = name;
    update.general.leader_name = leader;
    conn.receive_zone_packet_for_test(encode_raid_update(update));
}

struct RaidRequest {
    std::uint32_t action;
    std::string member, leader;
};

std::vector<RaidRequest> invitations(const MockEqServer& server) {
    std::vector<RaidRequest> result;
    for (const auto& packet : server.zone_log) {
        if (packet.opcode != to_wire(Op::RaidInvite)) continue;
        ByteReader reader(packet.data);
        auto action = reader.le<std::uint32_t>();
        std::string member = reader.fixed_string(64);
        reader.skip(4);
        result.push_back({action, std::move(member), reader.fixed_string(64)});
    }
    return result;
}

void expedition(GameConnection& conn) {
    conn.receive_zone_packet_for_test(encode(DzInfo{true, 72, "Test expedition", "SomeoneElse"}));
    conn.receive_zone_packet_for_test(encode(std::vector<DzMember>{{"gEnNrO", DzMemberStatus::Online},
                                                                {"existing", DzMemberStatus::Online},
                                                                {"ExpeditionOnly", DzMemberStatus::Offline}}));
}

void shared_task(GameConnection& conn) {
    conn.receive_zone_packet_for_test(encode(std::vector<SharedTaskMember>{{"gEnNrO", false},
                                                                        {"Existing", true},
                                                                        {"TaskOnly", false}}));
}

} // namespace

TEST_CASE("raid window: expedition and task buttons invite missing roster members into the raid") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    app::EqHost host;
    host.conn = &conn;
    host.in_game = host.ui_ready = true;
    app::EqApi api(host);
    raid_update(conn, kRaidCreate, "Gennro");
    raid_update(conn, kRaidAddMember, "Gennro");
    raid_update(conn, kRaidAddMember, "Existing");
    expedition(conn);
    shared_task(conn);

    REQUIRE(api.child_state("RaidWindow", "RAID_InviteExpedButton") == true);
    REQUIRE(api.child_state("RaidWindow", "RAID_InviteTaskButton") == true);
    REQUIRE(api.click_child("RaidWindow", "RAID_InviteExpedButton"));
    REQUIRE(api.click_child("RaidWindow", "RAID_InviteTaskButton"));
    REQUIRE(rig.until([&] { return invitations(rig.server).size() == 2; }));
    auto sent = invitations(rig.server);
    CHECK(sent[0].action == kRaidAddMember);
    CHECK(sent[0].member == "ExpeditionOnly");
    CHECK(sent[0].leader == "Gennro");
    CHECK(sent[1].action == kRaidAddMember);
    CHECK(sent[1].member == "TaskOnly");
    CHECK(sent[1].leader == "Gennro");
    CHECK(conn.raid().members.size() == 2); // invitations wait for server acceptance
    for (const auto& packet : rig.server.zone_log) {
        CHECK(packet.opcode != to_wire(Op::DzAddPlayer));
        CHECK(packet.opcode != to_wire(Op::SharedTaskAddPlayer));
    }
}

TEST_CASE("raid roster invitations recheck lock, leadership, capacity and source membership") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    app::EqHost host;
    host.conn = &conn;
    host.in_game = host.ui_ready = true;
    app::EqApi api(host);
    raid_update(conn, kRaidCreate, "Gennro");
    raid_update(conn, kRaidAddMember, "Gennro");
    CHECK(api.child_state("RaidWindow", "RAID_InviteExpedButton") == false);
    CHECK(api.child_state("RaidWindow", "RAID_InviteTaskButton") == false);
    expedition(conn);
    shared_task(conn);

    auto click_both = [&] {
        REQUIRE(api.click_child("RaidWindow", "RAID_InviteExpedButton"));
        REQUIRE(api.click_child("RaidWindow", "RAID_InviteTaskButton"));
    };
    raid_update(conn, kRaidLock, "Gennro");
    CHECK(api.child_state("RaidWindow", "RAID_InviteExpedButton") == false);
    click_both();
    raid_update(conn, kRaidUnlock, "Gennro");
    raid_update(conn, kRaidSetLeader, "Other", "Other");
    CHECK(api.child_state("RaidWindow", "RAID_InviteTaskButton") == false);
    click_both();
    raid_update(conn, kRaidSetLeader, "Gennro");
    for (int i = 0; i < 71; ++i) raid_update(conn, kRaidAddMember, "Full" + std::to_string(i));
    REQUIRE(conn.raid().members.size() == 72);
    CHECK(api.child_state("RaidWindow", "RAID_InviteExpedButton") == false);
    click_both();
    conn.send_command("/dzlisttimers"); // a later packet fences all preceding no-send paths
    REQUIRE(rig.until([&] {
        for (const auto& packet : rig.server.zone_log)
            if (packet.opcode == to_wire(Op::DzListTimers)) return true;
        return false;
    }));
    CHECK(invitations(rig.server).empty());
}

TEST_CASE("raid roster invitations can form a raid without expedition or task leadership") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    expedition(conn);
    conn.raid_invite_expedition();
    REQUIRE(rig.until([&] { return invitations(rig.server).size() == 2; }));
    auto sent = invitations(rig.server);
    CHECK(sent[0].action == kRaidInvite);
    CHECK(sent[0].member == "existing");
    CHECK(sent[1].action == kRaidInvite);
    CHECK(sent[1].member == "ExpeditionOnly");
    CHECK_FALSE(conn.raid().active);
}

TEST_CASE("raid roster invitations refuse stale clicks while the interface is locked") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    app::EqHost host;
    host.conn = &conn;
    host.in_game = host.ui_ready = true;
    app::EqApi api(host);
    expedition(conn);
    shared_task(conn);
    conn.send_command("/safelock test");
    REQUIRE(conn.interface_locked());
    CHECK(api.child_state("RaidWindow", "RAID_InviteExpedButton") == false);
    CHECK(api.child_state("RaidWindow", "RAID_InviteTaskButton") == false);
    REQUIRE(api.click_child("RaidWindow", "RAID_InviteExpedButton"));
    REQUIRE(api.click_child("RaidWindow", "RAID_InviteTaskButton"));
    rig.run_for(std::chrono::milliseconds(20));
    CHECK(invitations(rig.server).empty());
}

TEST_CASE("raid group moves retain delegates, marks and active ranks until a real disband") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto member = [&](const std::string& name) -> const game::RaidMember* {
        const auto& members = conn.raid().members;
        const auto found = std::find_if(members.begin(), members.end(),
                                        [&](const game::RaidMember& candidate) { return candidate.name == name; });
        return found == members.end() ? nullptr : &*found;
    };
    raid_update(conn, kRaidCreate, "Gennro");
    raid_update(conn, kRaidAddMember, "Gennro");
    raid_update(conn, kRaidAddMember, "Assist");
    RaidUpdate ranks;
    ranks.general.action = kRaidSetLeaderAbilities;
    ranks.leadership[16] = 3;
    conn.receive_zone_packet_for_test(encode_raid_update(ranks));
    const auto self = conn.world().player_id;
    conn.receive_zone_packet_for_test(encode(DelegateAbility{4, 0, 0, self, "Gennro"}, Op::RaidDelegateAbility));
    conn.receive_zone_packet_for_test(encode(DelegateAbility{3, 1, 0, 77, "Assist"}, Op::RaidDelegateAbility));
    conn.receive_zone_packet_for_test(encode_mark_npc(Op::MarkRaidNPC, MarkNpc{101, 1, "Target"}));
    conn.receive_zone_packet_for_test(encode_mark_npc(Op::SetGroupTarget, MarkNpc{101, 1, "Target"}));

    raid_update(conn, kRaidRemoveMember, "Assist");
    raid_update(conn, kRaidAddMember, "Assist");
    REQUIRE(member("Assist"));
    CHECK(member("Assist")->main_assist);
    CHECK(conn.leadership().raid_assists[0] == 77);
    CHECK(conn.leadership().assist_targets[1] == 101);

    // The moved client's roster is rebuilt too; its own removal does not leave the raid.
    raid_update(conn, kRaidRemoveMember, "Gennro");
    raid_update(conn, kRaidCreate, "Gennro");
    raid_update(conn, kRaidAddMember, "Assist");
    raid_update(conn, kRaidAddMember, "Gennro");
    REQUIRE(member("Gennro"));
    REQUIRE(member("Assist"));
    CHECK(member("Gennro")->marker);
    CHECK(member("Assist")->main_assist);
    CHECK(conn.leadership().raid_markers[0] == self);
    CHECK(conn.leadership().raid_marks[0] == 101);
    CHECK(conn.leadership().assist_targets[1] == 101);
    CHECK(conn.leadership().active_ranks[16] == 3);

    raid_update(conn, kRaidDisband, "Gennro");
    CHECK_FALSE(conn.raid().active);
    CHECK(conn.leadership().raid_markers[0] == 0);
    CHECK(conn.leadership().raid_assists[0] == 0);
    CHECK(conn.leadership().raid_marks[0] == 0);
    CHECK(conn.leadership().assist_targets[1] == 0);
    CHECK(conn.leadership().active_ranks[16] == 0);
}

TEST_CASE("raid delegation refuses stale clicks after rank or leadership changes") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    raid_update(conn, kRaidCreate, "Gennro");
    raid_update(conn, kRaidAddMember, "Gennro");
    for (int i = 0; i < 17; ++i) raid_update(conn, kRaidAddMember, "Member" + std::to_string(i));
    auto ranks = [&](const std::string& leader, std::uint32_t assist) {
        RaidUpdate update;
        update.general.action = kRaidSetLeader;
        update.general.leader_name = leader;
        update.leadership[19] = assist;
        update.leadership[20] = 1;
        conn.receive_zone_packet_for_test(encode_raid_update(update));
    };
    ranks("Gennro", 1);
    conn.set_raid_target("Member0");
    app::EqHost host;
    host.conn = &conn;
    host.in_game = host.ui_ready = true;
    app::EqApi api(host);
    auto count = [&] {
        std::size_t result = 0;
        for (const auto& packet : rig.server.zone_log)
            if (packet.opcode == to_wire(Op::RaidDelegateAbility)) ++result;
        return result;
    };
    REQUIRE(api.child_state("RaidWindow", "RAID_MainAssistButton") == true);
    REQUIRE(api.click_child("RaidWindow", "RAID_MainAssistButton"));
    REQUIRE(rig.until([&] { return count() == 1; }));
    ranks("Gennro", 0);
    CHECK(api.child_state("RaidWindow", "RAID_MainAssistButton") == false);
    REQUIRE(api.click_child("RaidWindow", "RAID_MainAssistButton"));
    ranks("Other", 1);
    REQUIRE(api.click_child("RaidWindow", "RAID_MarkNPCButton"));
    ranks("Gennro", 1);
    conn.raid_delegate_ability(3, "Missing");
    conn.raid_delegate_ability(3, "");
    conn.raid_delegate_ability(5, "Member0");
    conn.send_command("/safelock test");
    REQUIRE(conn.interface_locked());
    CHECK(api.child_state("RaidWindow", "RAID_MarkNPCButton") == false);
    REQUIRE(api.click_child("RaidWindow", "RAID_MarkNPCButton"));
    rig.run_for(std::chrono::milliseconds(20));
    CHECK(count() == 1);
}
