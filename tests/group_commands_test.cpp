// Groups, raids, tasks and expeditions: the shared task and expedition commands (/task*, /dz*), the task window's
// shared task page, /grouproles with its role sets, /groupleader, and the group role changes (CGroupBase_SetMemberRole).

#include <doctest/doctest.h>

#include "core/ini_file.h"
#include "game/combat.h"
#include "game/group_role_sets.h"
#include "game/leadership.h"
#include "mock_server/harness.h"
#include "mq/parse.h"
#include "mq/tlo.h"
#include "net/dz_packets.h"
#include "net/group_packets.h"
#include "net/notice_packets.h"
#include "net/packet_io.h"
#include "net/social_packets.h"
#include "net/task_packets.h"
#include "core/byte_io.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

std::string field(const AppPacket& p, std::size_t at, std::size_t n = 64) {
    std::string s;
    for (std::size_t i = at; i < at + n && i < p.data.size() && p.data[i] != std::byte{0}; ++i)
        s.push_back(static_cast<char>(p.data[i]));
    return s;
}

std::vector<AppPacket> sent(const MockEqServer& server, Op op) {
    std::vector<AppPacket> out;
    for (const auto& p : server.zone_log)
        if (p.opcode == to_wire(op)) out.push_back(p); // 0x36e8 is both OP_AvaliableTask and OP_SharedTaskAddPlayer
    return out;
}

// Puts the mock's character (Gennro) in a group led by `leader` with the others.
void group_up(MockEqServer& server, const std::string& leader, std::vector<std::string> others) {
    GroupUpdateB u;
    u.group_id = 7;
    u.leader = leader;
    std::uint32_t slot = 0;
    GroupUpdateMember me;
    me.slot = slot++;
    me.name = "Gennro";
    u.members.push_back(me);
    for (auto& n : others) {
        GroupUpdateMember m;
        m.slot = slot++;
        m.name = n;
        u.members.push_back(m);
    }
    server.zone.send_reliable(encode(u));
}

} // namespace

TEST_CASE("shared tasks: the commands' packets") {
    AppPacket a = encode_shared_task_add_player("Friend");
    CHECK(a.opcode == 0x36e8);
    REQUIRE(a.data.size() == 72);
    CHECK(field(a, 8) == "Friend");
    CHECK(encode_shared_task_remove_player("x").opcode == 0x4865);
    CHECK(encode_shared_task_make_leader("x").opcode == 0x37f2);
    CHECK(encode_shared_task_make_leader(std::string(90, 'n')).data.size() == 72);
    CHECK(field(encode_shared_task_make_leader(std::string(90, 'n')), 8).size() == 63);
    AppPacket list = encode_shared_task_player_list();
    CHECK(list.opcode == 0x5727);
    CHECK(list.data.size() == 8);
    AppPacket quit = encode_shared_task_quit();
    CHECK(quit.opcode == 0x322e);
    CHECK(quit.data.size() == 12);
}

TEST_CASE("group roles: GroupRole_GetName, GroupRoles_FormatList and OP_GroupRoles") {
    CHECK(std::string(game::group_role_name(1)) == "Main Tank");
    CHECK(std::string(game::group_role_name(3)) == "Puller");
    CHECK(game::group_role_name(0) == nullptr);
    CHECK(game::group_role_name(4) == nullptr);
    CHECK(game::format_group_roles(0, true) == "None");
    CHECK(game::format_group_roles(1 | 4, true) == "(1) Main Tank, (3) Puller");
    CHECK(game::format_group_roles(2 | 4, false) == "Main Assist, Puller");

    GroupRole r;
    r.member = "Tank";
    r.leader = "Gennro";
    r.role = 2;
    r.on = 1;
    AppPacket p = encode_group_role(r);
    REQUIRE(p.data.size() == 148);
    CHECK(p.opcode == 0x70e2);
    auto back = decode_group_role(p);
    REQUIRE(back);
    CHECK(back->member == "Tank");
    CHECK(back->leader == "Gennro");
    CHECK(back->role == 2);
    CHECK(back->on == 1);
}

TEST_CASE("group role sets: saved and read back from [GroupRoleLoadouts], sorted, found by name") {
    game::GroupRoleSets sets;
    CHECK(sets.free_slot() == 0);
    auto& s = sets.at(0);
    s.in_use = true;
    s.name = "Zeta";
    s.members[0] = {1, "Tank"};
    s.members[1] = {6, "Helper"};
    auto& t = sets.at(1);
    t.in_use = true;
    t.name = "alpha";
    sets.sort();
    CHECK(sets.at(0).name == "alpha"); // in use first, by name, case ignored
    CHECK(sets.find("ZETA") == 1);
    CHECK(sets.find("none") == -1);
    CHECK(sets.free_slot() == 2);
    CHECK(sets.take_dirty());
    IniFile ini;
    sets.save(ini);
    CHECK(ini.get_string("GroupRoleLoadouts", "GroupRoleLoadout2.name2") == "Helper");
    CHECK(ini.get_int("GroupRoleLoadouts", "GroupRoleLoadout2.roles2", 0) == 6);
    CHECK(ini.get_int("GroupRoleLoadouts", "GroupRoleLoadout3.inuse", -1) == 0);
    game::GroupRoleSets back;
    back.load(ini);
    CHECK(back.at(1).name == "Zeta");
    CHECK(back.at(1).members[0].name == "Tank");
    CHECK(back.at(1).members[0].roles == 1);
    CHECK_FALSE(back.at(2).in_use);
}

TEST_CASE("mock server: /dz, /task commands, the task window's shared task page and /grouproles") {
    Rig rig(MockConfig::standard(), quick_options());
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());

    // /dzaddplayer, /dzswapplayer (removed first, then added), /dzquit and /dzplayerlist go to the server as typed.
    conn.send_command("/dzaddplayer Friend extra");
    conn.send_command("/dzswapplayer Out In");
    conn.send_command("/dzquit");
    conn.send_command("/dzplayerlist");
    conn.send_command("/dzlisttimers");
    REQUIRE(rig.until([&] { return !sent(server, Op::DzListTimers).empty(); }));
    auto add = sent(server, Op::DzAddPlayer);
    REQUIRE(add.size() == 1);
    CHECK(field(add[0], 8) == "Friend");
    auto swap = sent(server, Op::DzSwapPlayer);
    REQUIRE(swap.size() == 1);
    CHECK(field(swap[0], 8) == "In");
    CHECK(field(swap[0], 72) == "Out");
    CHECK(sent(server, Op::DzQuit).size() == 1);
    CHECK(sent(server, Op::DzPlayerList).size() == 1);

    // /taskaddplayer, /taskmakeleader, /taskquit.
    conn.send_command("/taskaddplayer Pal");
    conn.send_command("/taskmakeleader Pal");
    conn.send_command("/taskquit");
    REQUIRE(rig.until([&] { return !sent(server, Op::SharedTaskQuit).empty(); }));
    auto tadd = sent(server, Op::SharedTaskAddPlayer);
    REQUIRE(tadd.size() == 1);
    CHECK(field(tadd[0], 8) == "Pal");
    CHECK(field(sent(server, Op::SharedTaskMakeLeader).at(0), 8) == "Pal");

    // The member list's leader and the member change.
    server.zone.send_reliable(encode(std::vector<SharedTaskMember>{{"Gennro", true}, {"Pal", false}}));
    REQUIRE(rig.until([&] { return conn.tasks().shared_members.size() == 2; }));
    CHECK(conn.tasks().shared_leader == "Gennro");
    CHECK(conn.shared_task_is_leader());
    server.zone.send_reliable(encode(SharedTaskMemberChange{false, "Pal"}));
    REQUIRE(rig.until([&] { return conn.tasks().shared_members.size() == 1; }));
    // The page's Remove Player with nothing selected sends nothing (0x230f).
    std::size_t removes = sent(server, Op::SharedTaskRemovePlayer).size();
    conn.shared_task_remove_selected(std::nullopt);
    conn.shared_task_remove_selected(std::string("Gennro"));
    REQUIRE(rig.until([&] { return sent(server, Op::SharedTaskRemovePlayer).size() == removes + 1; }));

    // /grouproles: not in a group sends nothing; in a group the leader sets a role by number.
    conn.send_command("/grouproles set Tank 1");
    group_up(server, "Gennro", {"Tank", "Healer"});
    REQUIRE(rig.until([&] { return conn.group().active && conn.group().count() == 3; }));
    conn.send_command("/grouproles set Tank 1");
    conn.send_command("/grouproles u Healer 3"); // "u": a prefix of unset
    conn.send_command("/grouproles set Tank 9");  // no such role (0x1db0)
    REQUIRE(rig.until([&] { return sent(server, Op::GroupRoles).size() == 2; }));
    auto roles = sent(server, Op::GroupRoles);
    auto first = decode_group_role(roles[0]);
    REQUIRE(first);
    CHECK(first->member == "Tank");
    CHECK(first->leader == "Gennro");
    CHECK(first->role == 1);
    CHECK(first->on == 1);
    auto second = decode_group_role(roles[1]);
    REQUIRE(second);
    CHECK(second->member == "Healer");
    CHECK(second->role == 3);
    CHECK(second->on == 0);

    // The server's role change (EQEmu's RoleNumber 3 is the puller); a role is one member's at a time.
    GroupRole change;
    change.member = "Healer";
    change.leader = "Gennro";
    change.role = 3;
    change.on = 1;
    AppPacket cp = encode_group_role(change);
    server.zone.send_reliable(cp);
    REQUIRE(rig.until([&] { return conn.group().slots[2].puller; }));
    change.member = "Tank";
    server.zone.send_reliable(encode_group_role(change));
    REQUIRE(rig.until([&] { return conn.group().slots[1].puller; }));
    CHECK_FALSE(conn.group().slots[2].puller);

    // Role sets: save the group's roles, load them (the leader sends each member's three roles).
    conn.send_command("/grouproles roleset save Raiders");
    REQUIRE(rig.until([&] { return conn.group_role_sets().find("raiders") == 0; }));
    CHECK(conn.group_role_sets().at(0).members[1].name == "Tank");
    CHECK(conn.group_role_sets().at(0).members[1].roles == 4);
    std::size_t before = sent(server, Op::GroupRoles).size();
    conn.send_command("/grouproles roleset load Raiders");
    REQUIRE(rig.until([&] { return sent(server, Op::GroupRoles).size() == before + 9; }));
    conn.send_command("/grouproles roleset delete Raiders");
    REQUIRE(rig.until([&] { return conn.group_role_sets().find("Raiders") == -1; }));
}

TEST_CASE("mock server: group role requests validate the current leader and roster without predicting state") {
    Rig rig;
    auto& conn = rig.conn;
    auto& server = rig.server;
    conn.send_group_role("Gennro", 1, true); // not connected
    REQUIRE(rig.to_zone());
    conn.send_group_role("Gennro", 1, true); // solo
    group_up(server, "Gennro", {"Tank", "Healer"});
    REQUIRE(rig.until([&] { return conn.group().count() == 3; }));

    // A reliable packet after the requests is a barrier for assertions that no role packet was sent.
    auto flush = [&] {
        const auto before = sent(server, Op::RandomReq).size();
        conn.send_zone_packet(encode_random_request(1, 1));
        REQUIRE(rig.until([&] { return sent(server, Op::RandomReq).size() == before + 1; }));
    };
    conn.send_group_role("Outsider", 1, true);
    conn.send_group_role("", 2, true);
    conn.send_group_role("Tank", 0, true);
    conn.send_group_role("Tank", -1, true);
    conn.send_group_role("Tank", 4, true);
    conn.send_command("/grouproles set Tank 999999999999999999999999999999");
    flush();
    CHECK(sent(server, Op::GroupRoles).empty());

    // Self may fill every role. Case-insensitive selections send the canonical roster name.
    for (int role = 1; role <= 3; ++role) conn.send_group_role("gEnNrO", role, true);
    flush();
    auto requests = sent(server, Op::GroupRoles);
    REQUIRE(requests.size() == 3);
    for (int i = 0; i < 3; ++i) {
        auto request = decode_group_role(requests[static_cast<std::size_t>(i)]);
        REQUIRE(request);
        CHECK(request->member == "Gennro");
        CHECK(request->leader == "Gennro");
        CHECK(request->role == i + 1);
        CHECK(request->on == 1);
    }
    CHECK_FALSE(conn.group().slots[0].main_tank);
    CHECK_FALSE(conn.group().slots[0].main_assist);
    CHECK_FALSE(conn.group().slots[0].puller);

    server.zone.send_reliable(encode_group_leader_change("Tank"));
    REQUIRE(rig.until([&] { return conn.group().is_leader("Tank"); }));
    conn.send_group_role("Gennro", 2, true); // menu opened before losing leadership
    conn.send_command("/grouproles set Gennro 1");
    flush();
    CHECK(sent(server, Op::GroupRoles).size() == 3);

    group_up(server, "Gennro", {"Healer"});
    REQUIRE(rig.until([&] { return conn.group().count() == 2 && conn.group().is_leader("Gennro"); }));
    conn.send_group_role("Tank", 1, true); // menu opened before that member left
    conn.send_command("/safelock password");
    REQUIRE(conn.interface_locked());
    conn.send_group_role("Healer", 1, true);
    flush();
    CHECK(sent(server, Op::GroupRoles).size() == 3);
}

TEST_CASE("mock server: confirmed group roles reach autocombat TLOs and survive roster refreshes") {
    Rig rig;
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    group_up(server, "Gennro", {"Tank", "Healer"});
    REQUIRE(rig.until([&] { return conn.group().count() == 3; }));
    mq::Registry registry;
    mq::register_game_tlos(registry);
    mq::Context context;
    context.registry = &registry;
    context.conn = &conn;
    auto tlo = [&](std::string_view text) { return mq::parse_macro_data(context, text); };
    CHECK(tlo("${Group.MainAssist.Name}") == "NULL");

    server.zone.send_reliable(encode_group_role({"Gennro", "Gennro", 2, 1}));
    REQUIRE(rig.until([&] { return conn.group().slots[0].main_assist; }));
    CHECK(tlo("${Group.MainAssist.Name}") == "Gennro");
    CHECK(tlo("${Group.Member[0].MainAssist}") == "TRUE");
    server.zone.send_reliable(encode_group_role({"Tank", "Gennro", 2, 1}));
    REQUIRE(rig.until([&] { return conn.group().slots[1].main_assist; }));
    CHECK_FALSE(conn.group().slots[0].main_assist);
    CHECK(tlo("${Group.MainAssist.Name}") == "Tank");
    server.zone.send_reliable(encode_group_role({"Tank", "Gennro", 2, 0}));
    REQUIRE(rig.until([&] { return !conn.group().slots[1].main_assist; }));
    CHECK(tlo("${Group.MainAssist.Name}") == "NULL");

    GroupUpdateB update;
    update.group_id = 7;
    update.leader = "Gennro";
    GroupUpdateMember me;
    me.name = "Gennro";
    me.main_assist = 1;
    me.puller = 1;
    GroupUpdateMember merc;
    merc.slot = 1;
    merc.name = "Merc";
    merc.merc = 1;
    merc.main_tank = 1;
    update.members = {me, merc};
    server.zone.send_reliable(encode(update));
    REQUIRE(rig.until([&] { return conn.group().slots[1].merc; }));
    CHECK(tlo("${Group.MainAssist.Name}") == "Gennro");
    CHECK(tlo("${Group.Puller.Name}") == "Gennro");
    CHECK(tlo("${Group.MainTank.Name}") == "Merc");
    conn.send_group_role("Merc", 2, true);
    conn.send_group_role("Merc", 3, true);
    conn.send_group_role("Merc", 1, false);
    REQUIRE(rig.until([&] { return sent(server, Op::GroupRoles).size() >= 1; }));
    auto requests = sent(server, Op::GroupRoles);
    REQUIRE(requests.size() == 1);
    CHECK(decode_group_role(requests[0])->role == 1);
    CHECK(decode_group_role(requests[0])->on == 0);
    CHECK(conn.group().slots[1].main_tank); // still awaiting the server

    server.zone.send_reliable(encode_group_generic(Op::GroupDisbandYou, {"Gennro", "Gennro"}));
    REQUIRE(rig.until([&] { return !conn.group().active; }));
    CHECK(tlo("${Group.MainTank.Name}") == "NULL");
    CHECK(tlo("${Group.MainAssist.Name}") == "NULL");
    CHECK(tlo("${Group.Puller.Name}") == "NULL");
}

TEST_CASE("mock server: the leadership abilities' commands and messages (marks, inspect buffs, clear marks)") {
    Rig rig(MockConfig::standard(), quick_options());
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    auto abilities = [&] {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> v;
        for (const auto& p : sent(server, Op::DoGroupLeadershipAbility)) {
            ByteReader r(p.data);
            std::uint32_t a = r.le<std::uint32_t>();
            v.emplace_back(a, r.le<std::uint32_t>());
        }
        return v;
    };

    // Not in a group of three: nothing goes out (the size line 0x2263); a bad mark number: the usage line.
    conn.set_target(101);
    conn.send_command("/gmarknpc 2");
    conn.send_command("/marknpc 4");
    group_up(server, "Gennro", {"Tank", "Healer"});
    REQUIRE(rig.until([&] { return conn.group().count() == 3; }));
    // Without the leader's Mark NPC rank 2 the ability is not active (0x21c2).
    conn.send_command("/gmarknpc 2");
    GroupLeadershipUpdate u;
    u.ranks[0] = 3; // Mark NPC
    u.ranks[6] = 1; // Inspect Buffs
    server.zone.send_reliable(encode(u));
    REQUIRE(rig.until([&] { return conn.leadership().active_ranks[0] == 3; }));
    conn.send_command("/gmarknpc 2");
    REQUIRE(rig.until([&] { return abilities().size() == 1; }));
    CHECK(abilities()[0] == std::pair<std::uint32_t, std::uint32_t>{0, 2});

    // The server's mark; marking the same NPC with the same number again unmarks it (0).
    server.zone.send_reliable(encode_mark_npc(Op::MarkNPC, MarkNpc{101, 2, "a_rat001"}));
    REQUIRE(rig.until([&] { return conn.leadership().group_marks[1] == 101; }));
    conn.send_command("/marknpc 2");
    REQUIRE(rig.until([&] { return abilities().size() == 2; }));
    CHECK(abilities()[1] == std::pair<std::uint32_t, std::uint32_t>{0, 0});
    server.zone.send_reliable(encode_mark_npc(Op::MarkNPC, MarkNpc{101, 0, "a_rat001"}));
    REQUIRE(rig.until([&] { return conn.leadership().group_marks[1] == 0; }));

    // /inspectbuffs needs a player of the group targeted (0x225d otherwise): ourselves here.
    conn.send_command("/inspectbuffs");
    conn.set_target(server.config.own_spawn_id);
    conn.send_command("/inspectbuffs");
    REQUIRE(rig.until([&] { return abilities().size() == 3; }));
    CHECK(abilities()[2] == std::pair<std::uint32_t, std::uint32_t>{6, 1});

    // /clearmarks from the group leader: OP_ClearNPCMarks; the server's answer clears them.
    server.zone.send_reliable(encode_mark_npc(Op::MarkNPC, MarkNpc{102, 1, "Guard_Bob"}));
    REQUIRE(rig.until([&] { return conn.leadership().group_marks[0] == 102; }));
    conn.send_command("/clearmarks");
    REQUIRE(rig.until([&] { return sent(server, Op::ClearNPCMarks).size() == 1; }));
    server.zone.send_reliable(make_empty(Op::ClearNPCMarks));
    REQUIRE(rig.until([&] { return conn.leadership().group_marks[0] == 0; }));

    // A delegated marker and OP_InspectBuffs' lines.
    server.zone.send_reliable(encode(DelegateAbility{1, 0, 0, 55, "Tank"}));
    REQUIRE(rig.until([&] { return conn.leadership().group_marker == 55; }));
    InspectBuffs b;
    b.spells[0] = 200;
    b.ticks[0] = 10;
    server.zone.send_reliable(encode(b));
    bool header = false;
    REQUIRE(rig.until([&] {
        while (auto e = conn.poll_chat())
            if (e->color == 0x14a) header = true;
        return header;
    }));

    // /findpc: no Find Path to PC rank: 0x2269; a short name: 0x2273 (nothing is sent either way).
    conn.send_command("/findpc Tank");
    conn.send_command("/findpc Abc");
    CHECK(sent(server, Op::FindPersonRequest).empty());
}

TEST_CASE("mock server: raid leadership (ranks, raid marks, delegation, the leader's buttons, hot buttons)") {
    Rig rig(MockConfig::standard(), quick_options());
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    auto raid_commands = [&] {
        std::vector<RaidGeneral> v;
        for (const auto& p : sent(server, Op::RaidInvite)) {
            ByteReader r(p.data);
            RaidGeneral g;
            g.action = r.le<std::uint32_t>();
            g.player_name = r.fixed_string(64);
            r.skip(4);
            g.leader_name = r.fixed_string(64);
            g.parameter = r.le<std::uint32_t>();
            v.push_back(g);
        }
        return v;
    };
    auto abilities = [&] {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> v;
        for (const auto& p : sent(server, Op::DoGroupLeadershipAbility)) {
            ByteReader r(p.data);
            std::uint32_t a = r.le<std::uint32_t>();
            v.emplace_back(a, r.le<std::uint32_t>());
        }
        return v;
    };

    // A raid of 18 led by us.
    RaidUpdate add;
    add.general.action = kRaidAddMember;
    add.general.leader_name = "Gennro";
    add.level = 60;
    for (int i = 0; i < 18; ++i) {
        add.general.player_name = i == 0 ? std::string("Gennro") : "Member" + std::to_string(i);
        add.general.parameter = static_cast<std::uint32_t>(i / 6);
        server.zone.send_reliable(encode_raid_update(add));
    }
    REQUIRE(rig.until([&] { return conn.raid().members.size() == 18; }));

    // Without the raid leader's ranks the raid mark is refused (nothing sent); OP_RaidUpdate 30 brings them.
    conn.set_target(101);
    conn.send_command("/rmarknpc 2");
    RaidUpdate lead;
    lead.general.action = kRaidSetLeader;
    lead.general.player_name = lead.general.leader_name = "Gennro";
    lead.leadership[16] = 3; // Raid Mark NPC
    lead.leadership[17] = 1; // NPC Health
    lead.leadership[0x13] = 1;
    lead.leadership[0x1c] = 1;
    server.zone.send_reliable(encode_raid_update(lead));
    REQUIRE(rig.until([&] { return conn.leadership().active_ranks[16] == 3; }));
    CHECK(conn.raid().leader == "Gennro");
    CHECK(conn.leadership().active_ranks[0x13] == 1);
    CHECK(conn.leadership().active_ranks[0] == 0); // the group ranks are not the raid packet's
    conn.send_command("/rmarknpc 2");
    REQUIRE(rig.until([&] { return abilities().size() == 1; }));
    CHECK(abilities()[0] == std::pair<std::uint32_t, std::uint32_t>{0x10, 2});

    // The raid leadership update (14) replaces them.
    RaidUpdate ranks;
    ranks.general.action = kRaidSetLeaderAbilities;
    ranks.general.player_name = ranks.general.leader_name = "Gennro";
    ranks.leadership[16] = 2;
    ranks.leadership[0x13] = 1;
    ranks.leadership[0x1c] = 1;
    server.zone.send_reliable(encode_raid_update(ranks));
    REQUIRE(rig.until([&] { return conn.leadership().active_ranks[16] == 2; }));

    // The leadership window's hot buttons: Raid Mark NPC has three, enabled up to the active rank; passive abilities
    // have none. A type 11 hot button runs the ability (value / 8, value % 8).
    CHECK(conn.leadership_hot_button_enabled(16, 2));
    CHECK_FALSE(conn.leadership_hot_button_enabled(16, 3));
    CHECK_FALSE(conn.leadership_hot_button_enabled(17, 1));
    CHECK_FALSE(conn.leadership_hot_button_enabled(0x13, 2)); // one hot button
    CHECK(game::leadership_hot_button_value(16, 3) == 0x83);
    conn.activate_leadership_ability(16, 1);
    REQUIRE(rig.until([&] { return abilities().size() == 2; }));
    CHECK(abilities()[1] == std::pair<std::uint32_t, std::uint32_t>{0x10, 1});

    // Delegating the raid's main assist (0x13) to a raid member target: OP_RaidDelegateAbility 3 with its name, then
    // OP_DoGroupLeadershipAbility. Our own spawn is a raid member.
    conn.set_target(server.config.own_spawn_id);
    conn.activate_leadership_ability(0x13, 1);
    REQUIRE(rig.until([&] { return sent(server, Op::RaidDelegateAbility).size() == 1; }));
    auto d = decode_delegate_ability(sent(server, Op::RaidDelegateAbility)[0]);
    REQUIRE(d);
    CHECK(d->ability == 3);
    CHECK(d->name == "Gennro");
    REQUIRE(rig.until([&] { return abilities().size() == 3; }));
    CHECK(abilities()[2] == std::pair<std::uint32_t, std::uint32_t>{0x13, 1});

    // The server's delegation: a marker (EQEmu numbers them 0) and a main assist; their Role column flags.
    server.zone.send_reliable(encode(DelegateAbility{4, 0, 0, 77, "Member1"}, Op::RaidDelegateAbility));
    REQUIRE(rig.until([&] { return conn.leadership().raid_markers[0] == 77; }));
    server.zone.send_reliable(encode(DelegateAbility{3, 1, 0, 78, "Member2"}, Op::RaidDelegateAbility));
    REQUIRE(rig.until([&] { return conn.leadership().raid_assists[1] == 78; }));
    CHECK(conn.leadership().raid_assist_names[1] == "Member2");
    auto member = [&](const std::string& n) {
        for (const auto& m : conn.raid().members)
            if (m.name == n) return m;
        return game::RaidMember{};
    };
    CHECK(member("Member1").marker);
    CHECK(member("Member2").main_assist);
    server.zone.send_reliable(encode(DelegateAbility{4, 0, 1, 77, "Member1"}, Op::RaidDelegateAbility));
    REQUIRE(rig.until([&] { return conn.leadership().raid_markers[0] == 0; }));
    CHECK_FALSE(member("Member1").marker);

    // The raid main assist's target (OP_SetGroupTarget number 1).
    server.zone.send_reliable(encode_mark_npc(Op::SetGroupTarget, MarkNpc{101, 1, ""}));
    REQUIRE(rig.until([&] { return conn.leadership().assist_targets[1] == 101; }));

    // The leader's buttons and commands as CRaid sends them.
    std::size_t before = raid_commands().size();
    conn.send_command("/setloottype 5");  // out of range: nothing
    conn.send_command("/setloottype 33"); // not one digit: 5094
    conn.send_command("/setloottype 3");
    REQUIRE(rig.until([&] { return raid_commands().size() == before + 1; }));
    CHECK(raid_commands().back().action == 0x20);
    CHECK(raid_commands().back().player_name == "Gennro");
    CHECK(raid_commands().back().parameter == 3);
    CHECK(conn.raid().loot_type == 3);
    conn.send_command("/addraidlooter"); // no raid target: 5095
    conn.set_raid_target("Member3");
    CHECK(conn.raid().target == "Member3");
    conn.send_command("/addraidlooter");
    REQUIRE(rig.until([&] { return raid_commands().size() == before + 2; }));
    CHECK(raid_commands().back().action == 0x21);
    CHECK(raid_commands().back().player_name == "Gennro");
    CHECK(raid_commands().back().leader_name == "Member3");
    conn.send_command("/addraidlooter"); // already a looter: nothing
    conn.send_command("/removeraidlooter");
    REQUIRE(rig.until([&] { return raid_commands().size() == before + 3; }));
    CHECK(raid_commands().back().action == 0x22);
    conn.raid_lock(true);
    REQUIRE(rig.until([&] { return raid_commands().size() == before + 4; }));
    CHECK(raid_commands().back().action == 8);
    CHECK(raid_commands().back().leader_name == "Gennro");
    conn.raid_move_group("Member3", -1);
    REQUIRE(rig.until([&] { return raid_commands().size() == before + 5; }));
    CHECK(raid_commands().back().action == 6);
    CHECK(raid_commands().back().leader_name == "Member3");
    CHECK(raid_commands().back().parameter == 0xffffffffu);
    conn.raid_remove_group_leader("Member3");
    REQUIRE(rig.until([&] { return raid_commands().size() == before + 6; }));
    CHECK(raid_commands().back().action == 7);
    conn.raid_send_leadership_change(std::nullopt);
    REQUIRE(rig.until([&] { return raid_commands().size() == before + 7; }));
    CHECK(raid_commands().back().action == 0x1e);
    CHECK(raid_commands().back().leader_name == "Member3");
    conn.raid_delegate_ability(4, "");
    conn.raid_delegate_ability(4, conn.raid().target);
    REQUIRE(rig.until([&] { return sent(server, Op::RaidDelegateAbility).size() == 2; }));
    CHECK(decode_delegate_ability(sent(server, Op::RaidDelegateAbility)[1])->ability == 4);
}

TEST_CASE("name sprites: the leadership marks around a non-player's name") {
    game::Spawn rat;
    rat.id = 101;
    rat.is_npc = true;
    rat.hp_percent = 42;
    game::NameSpriteMarks m;
    m.raid_marks[1] = 101;
    CHECK(game::name_sprite_with_marks(rat, "a rat", m) == "a rat"); // no Gates of Discord
    m.leadership = true;
    CHECK(game::name_sprite_with_marks(rat, "a rat", m) == "2 - a rat");
    m.group_marks[0] = 101; // the raid mark wins
    CHECK(game::name_sprite_with_marks(rat, "a rat", m) == "2 - a rat");
    m.npc_health = true;
    m.assist_targets[2] = 101;
    CHECK(game::name_sprite_with_marks(rat, "a rat", m) == ">> 2 - a rat - 42% <<");
    m = {};
    m.leadership = m.npc_health = true;
    CHECK(game::name_sprite_with_marks(rat, "a rat", m) == "a rat"); // neither target nor marked
    m.target_id = 101;
    CHECK(game::name_sprite_with_marks(rat, "a rat", m) == "a rat - 42%");
    game::Spawn corpse = rat;
    corpse.is_corpse = true;
    m.group_marks[2] = 101;
    CHECK(game::name_sprite_with_marks(corpse, "a rat's corpse", m) == "3 - a rat's corpse"); // no HP on a corpse
}
