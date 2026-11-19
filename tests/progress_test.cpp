// Alternate advancement, training, task and skill packets, byte for byte against EQEmu's RoF2 serialisers
// (common/patches/rof2.cpp ENCODE(OP_SendAATable / OP_RespondAA / OP_TaskDescription / OP_TaskHistoryReply),
// zone/aa.cpp, zone/client_process.cpp OPGMTraining, zone/task_manager.cpp, common/tasks.h) and the original's
// readers; and the rules in game/alt_adv.h, game/training.h, game/tasks.h.

#include <doctest/doctest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "game/alt_adv.h"
#include "game/eq_strings.h"
#include "game/tasks.h"
#include "game/spells.h"
#include "game/training.h"
#include "mock_server/harness.h"
#include "net/aa_packets.h"
#include "net/ability_packets.h"
#include "net/opcodes.h"
#include "net/task_packets.h"
#include "net/training_packets.h"
#include "net/zone_packets.h"

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

struct Buf {
    std::vector<std::uint8_t> b;
    Buf& u8(std::uint8_t v) { b.push_back(v); return *this; }
    Buf& u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Buf& i32(std::int32_t v) { return u32(static_cast<std::uint32_t>(v)); }
    Buf& f32(float f) {
        std::uint32_t u;
        std::memcpy(&u, &f, 4);
        return u32(u);
    }
    Buf& str(const char* s) {
        b.insert(b.end(), s, s + std::strlen(s));
        b.push_back(0);
        return *this;
    }
    Buf& lstr(const char* s) { // SerializeBuffer::WriteLengthString
        u32(static_cast<std::uint32_t>(std::strlen(s)));
        b.insert(b.end(), s, s + std::strlen(s));
        return *this;
    }
};

// ENCODE(OP_SendAATable) for one rank with two effects and one prerequisite.
Buf aa_rank_bytes() {
    Buf w;
    w.u32(5100).u8(1).i32(-1).i32(-1).i32(1200).i32(1201).i32(51).i32(3).u32(200).u32(2);
    w.u32(1).i32(199).u32(1).i32(3); // prereqs: skills, points
    w.i32(3).i32(8000).i32(1).i32(42).i32(900).i32(0x10).i32(5).i32(5099).i32(5101).i32(6);
    w.u8(0).u8(0).u8(0).u32(0).i32(4).i32(7).u8(0).u8(0).u8(0).u8(0);
    w.u32(2).u32(264).u32(10).u32(200).u32(1).u32(97).u32(5).u32(0).u32(2);
    return w;
}

} // namespace

TEST_CASE("progress: opcodes are the RoF2 ones") {
    CHECK(to_wire(Op::SendAATable) == 0x66b5);
    CHECK(to_wire(Op::RespondAA) == 0x7a27);
    CHECK(to_wire(Op::UpdateAA) == 0x66f0);
    CHECK(to_wire(Op::AAExpUpdate) == 0x7d14);
    CHECK(to_wire(Op::AAAction) == 0x424e);
    CHECK(to_wire(Op::ClearAA) == 0x0f50);
    CHECK(to_wire(Op::GMTraining) == 0x1966);
    CHECK(to_wire(Op::GMTrainSkill) == 0x2a85);
    CHECK(to_wire(Op::GMEndTraining) == 0x4d6b);
    CHECK(to_wire(Op::GMTrainSkillConfirm) == 0x4b64);
    CHECK(to_wire(Op::TradeSkillCombine) == 0x579a);
    CHECK(to_wire(Op::TaskDescription) == 0x3714);
    CHECK(to_wire(Op::TaskActivity) == 0x08d3);
    CHECK(to_wire(Op::CompletedTasks) == 0x4eba);
    CHECK(to_wire(Op::TaskActivityComplete) == 0x5e19);
    CHECK(to_wire(Op::AcceptNewTask) == 0x0a23);
    CHECK(to_wire(Op::CancelTask) == 0x39f0);
    CHECK(to_wire(Op::TaskHistoryRequest) == 0x5f1c);
    CHECK(to_wire(Op::TaskHistoryReply) == 0x3d05);
    CHECK(to_wire(Op::TaskSelectWindow) == 0x705b);
    CHECK(to_wire(Op::SharedTaskSelectWindow) == 0x48a2);
    CHECK(to_wire(Op::SharedTaskAcceptNew) == 0x6646);
    CHECK(to_wire(Op::TaskRequestTimer) == 0x7a48);
    CHECK(to_wire(Op::TaskTimers) == 0x2b0f);
}

TEST_CASE("aa: OP_SendAATable decodes EQEmu's encoding and re-encodes the same bytes") {
    Buf w = aa_rank_bytes();
    auto a = decode_send_aa_table(packet(Op::SendAATable, w.b));
    REQUIRE(a);
    CHECK(a->id == 5100);
    CHECK(a->unknown04 == 1);
    CHECK(a->title_sid == 1200);
    CHECK(a->desc_sid == 1201);
    CHECK(a->level_req == 51);
    CHECK(a->cost == 3);
    CHECK(a->group == 200);
    CHECK(a->rank == 2);
    REQUIRE(a->prereq_groups.size() == 1);
    CHECK(a->prereq_groups[0] == 199);
    CHECK(a->prereq_ranks[0] == 3);
    CHECK(a->type == 3);
    CHECK(a->spell == 8000);
    REQUIRE(a->timers.size() == 1); // EQEmu's "1" then spell_type: a counted array
    CHECK(a->timers[0] == 42);
    CHECK(a->refresh == 900);
    CHECK(a->classes == 0x10);
    CHECK(a->max_rank == 5);
    CHECK(a->prev_id == 5099);
    CHECK(a->next_id == 5101);
    CHECK(a->total_cost == 6);
    CHECK(a->expansion == 4);
    CHECK(a->category == 7);
    REQUIRE(a->effects.size() == 2);
    CHECK(a->effects[0].spa == 264);
    CHECK(a->effects[0].base == 10);
    CHECK(a->effects[0].limit == 200);
    CHECK(a->effects[1].spa == 97);
    CHECK(a->effects[1].slot == 2);
    CHECK(bytes_of(encode_send_aa_table(*a)) == w.b);
    // A short packet fails cleanly.
    w.b.resize(w.b.size() - 3);
    CHECK_FALSE(decode_send_aa_table(packet(Op::SendAATable, w.b)));
}

TEST_CASE("aa: OP_RespondAA, OP_AAExpUpdate and the server's OP_AAAction") {
    RespondAA r;
    r.spent = 12;
    r.assigned = {12, 0, 0, 0, 0};
    r.owned = {{5100, 6, 0}, {7000, 1, 3}};
    AppPacket p = encode(r);
    CHECK(p.data.size() == 24 + 300 * 12); // AATable_Struct
    auto d = decode_respond_aa(p);
    REQUIRE(d);
    CHECK(d->spent == 12);
    CHECK(d->assigned[0] == 12);
    REQUIRE(d->owned.size() == 2);
    CHECK(d->owned[1].id == 7000);
    CHECK(d->owned[1].charges == 3);

    // A server that sends more than the stock 300 slots: every owned rank is kept (a deliberate difference; the
    // original stops at 300).
    RespondAA big;
    for (std::uint32_t i = 0; i < 450; ++i) big.owned.push_back({1000 + i, 1, 0});
    AppPacket bp = encode(big);
    CHECK(bp.data.size() == 24 + 450 * 12);
    auto bd = decode_respond_aa(bp);
    REQUIRE(bd);
    REQUIRE(bd->owned.size() == 450);
    CHECK(bd->owned[449].id == 1449);
    game::AltAdvState st;
    st.set_owned(bd->spent, bd->assigned, std::move(bd->owned));
    CHECK(st.owned.size() == 450);
    CHECK(st.has(1449));

    // AltAdvStats_Struct: u32 experience, u16 unspent, u16, u8 percentage, 3 bytes.
    Buf e;
    e.u32(165).u8(7).u8(0).u8(0).u8(0).u8(25).u8(0).u8(0).u8(0);
    auto x = decode_aa_exp_update(packet(Op::AAExpUpdate, e.b));
    REQUIRE(x);
    CHECK(x->exp == 165);
    CHECK(x->points == 7);
    CHECK(x->percent == 25);
    CHECK(bytes_of(encode(*x)) == e.b);

    // UseAA_Struct {begin, ability (the timer), end}.
    Buf t;
    t.u32(1000).u32(42).u32(1600);
    auto timer = decode_aa_timer(packet(Op::AAAction, t.b));
    REQUIRE(timer);
    CHECK(timer->begin == 1000);
    CHECK(timer->timer == 42);
    CHECK(timer->server_now == 1600);
    CHECK(bytes_of(encode(*timer)) == t.b);
}

TEST_CASE("aa: OP_ExpUpdate leaves AA experience alone; OP_AAExpUpdate sets the unspent count") {
    test::Rig rig(test::MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());

    // On every kill EQEmu's Client::SetEXP sends OP_AAExpUpdate, then OP_ExpUpdate with 0 in the second dword.
    // Handle_OP_ExpUpdate @0x4b7060 sets only PcBase.Exp, so the AA gauge keeps its value.
    conn.receive_zone_packet_for_test(encode(AAExpUpdate{165, 9, 100}));
    conn.receive_zone_packet_for_test(encode(ExpUpdate{40, 0}));
    CHECK(conn.player_status().exp == 40);
    CHECK(conn.player_status().aa_exp == 165);
    CHECK(conn.alt_adv().points == 9);
    REQUIRE(conn.profile());
    CHECK(conn.profile()->aa_points == 9); // what the AA labels (EQType 71, ${Me.AAPoints}) read
}

TEST_CASE("aa: the client's OP_AAAction is AA_Action {action, ability, target, value}") {
    AppPacket p = encode(AAAction{kAATrain, 5100, 0, 0});
    CHECK(p.opcode == 0x424e);
    Buf w;
    w.u32(3).u32(5100).u32(0).u32(0);
    CHECK(bytes_of(p) == w.b);
    Buf on;
    on.u32(1).u32(0).u32(0).u32(50);
    CHECK(bytes_of(encode(AAAction{kAASetPercent, 0, 0, 50})) == on.b);
    auto back = decode_aa_action(encode(AAAction{kAAActivate, 7000, 1234, 0}));
    REQUIRE(back);
    CHECK(back->action == 0);
    CHECK(back->ability == 7000);
    CHECK(back->target == 1234);
}

namespace {
game::AltAbility rank(std::uint32_t id, std::int32_t group, std::int32_t r, std::int32_t prev, std::int32_t next) {
    game::AltAbility a;
    a.id = id;
    a.group = group;
    a.rank = r;
    a.prev_id = prev;
    a.next_id = next;
    a.max_rank = 3;
    a.cost = 2;
    a.total_cost = 2 * r;
    a.level_req = 51;
    a.classes = 1u << 2; // paladin
    a.type = 1;
    a.prereq_groups = {0};
    a.prereq_ranks = {0};
    return a;
}
} // namespace

TEST_CASE("aa: CanTrainAbility, MeetsPrerequisites and CanSeeAbility") {
    game::AltAdvState s;
    s.add(rank(100, 10, 1, -1, 101));
    s.add(rank(101, 10, 2, 100, 102));
    s.add(rank(102, 10, 3, 101, -1));
    game::AltAbility needs = rank(200, 20, 1, -1, -1);
    needs.prereq_groups = {10};
    needs.prereq_ranks = {2};
    s.add(needs);
    s.points = 10;
    game::AATrainer who;
    who.class_id = 2;
    who.level = 60;

    CHECK(s.can_train(*s.get(100), who));
    CHECK_FALSE(s.can_train(*s.get(101), who)); // rank 1 not owned
    CHECK_FALSE(s.can_train(*s.get(200), who)); // needs rank 2 of ability 10
    CHECK(s.can_see(*s.get(100), 2));
    CHECK_FALSE(s.can_see(*s.get(101), 2));

    s.owned = {{100, 2, 0}};
    CHECK_FALSE(s.can_train(*s.get(100), who)); // owned
    CHECK(s.can_train(*s.get(101), who));
    CHECK_FALSE(s.can_train(*s.get(102), who)); // owning rank 1, rank 3's previous is 101
    CHECK_FALSE(s.can_see(*s.get(100), 2));     // not the last rank: the next one shows instead
    CHECK(s.can_see(*s.get(101), 2));
    CHECK_FALSE(s.can_train(*s.get(200), who));

    s.owned = {{101, 4, 0}};
    CHECK(s.can_train(*s.get(200), who));
    // Points, level, class and expansion.
    s.points = 1;
    CHECK_FALSE(s.can_train(*s.get(200), who));
    s.points = 10;
    who.level = 50;
    CHECK_FALSE(s.can_train(*s.get(200), who));
    who.level = 60;
    who.class_id = 3;
    CHECK_FALSE(s.can_train(*s.get(200), who));
    who.class_id = 2;
    game::AltAbility exp = rank(300, 30, 1, -1, -1);
    exp.expansion = 5;
    s.add(exp);
    CHECK_FALSE(s.can_train(*s.get(300), who));
    who.expansions = 1u << 4;
    CHECK(s.can_train(*s.get(300), who));
    // The owned last rank is visible; so is the visible list.
    s.owned = {{102, 6, 0}};
    CHECK(s.can_see(*s.get(102), 2));
    auto tab = s.visible(1, 2);
    CHECK(tab.size() == 3); // 102, 200 (prereq not met but visible) and 300
    // The legacy category prerequisite: 6 points in general abilities.
    game::AltAbility legacy = rank(400, 40, 1, -1, -1);
    legacy.prereq_groups = {-1};
    s.add(legacy);
    CHECK(s.can_train(*s.get(400), who)); // 102's total cost is 6
    s.owned = {{101, 4, 0}};
    CHECK_FALSE(s.can_train(*s.get(400), who));
}

TEST_CASE("aa: the active form controls which ranks can be listed or trained") {
    game::AltAdvState state;
    state.add(rank(100, 10, 1, -1, -1));
    auto shroud = rank(200, 20, 1, -1, 201);
    shroud.flags[0] = 1;
    state.add(shroud);
    auto next_shroud = rank(201, 20, 2, 200, -1);
    next_shroud.flags[0] = 1;
    state.add(next_shroud);
    state.points = 10;
    game::AATrainer who;
    who.class_id = 2;
    who.level = 60;
    game::AAFilter filter;
    filter.trainable_only = true;
    CHECK(state.visible(1, who.class_id, filter, who) == std::vector{state.get(100)});
    CHECK_FALSE(state.can_train(*state.get(200), who));

    who.monster_profile = true;
    CHECK(state.visible(1, who.class_id, filter, who) == std::vector{state.get(200)});
    // CanTrainAbility @0x4a44c0 does not refuse an ordinary rank in a shroud; only CanSeeAbility hides it.
    CHECK(state.can_train(*state.get(100), who));
    state.owned = {{200, 2, 0}};
    CHECK(state.visible(1, who.class_id, filter, who) == std::vector{state.get(201)});
}

// "Buy every rank" on a server without the buy-all action (a stock EQEmu): the client sends OP_AAAction 3 for the next
// rank each time EQEmu's answer to the last purchase (the owned list, then the points) shows it bought, waits through
// other updates, and stops at the last rank and on a plain train.
TEST_CASE("aa: a title the dbstr does not have is named by the ability's spell, else its group, never empty") {
    // A live game folder's dbstr_us.txt numbers many AAs differently from a RoF2 server's ranks (title_sid), and a
    // server's own export can lack some: the AA window's Ability column showed nothing for those.
    test::Rig rig(test::MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    std::vector<std::string> fields(237, "0");
    fields[0] = "8000";
    fields[1] = "Fixture Discipline";
    for (std::size_t i = 86; i <= 97; ++i) fields[i] = "254";
    std::string line;
    for (const auto& field : fields) line += (line.empty() ? "" : "^") + field;
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(line) == 1);
    conn.set_spell_table_for_test(table);

    game::AltAbility passive = rank(100, 10, 1, -1, -1);
    passive.title_sid = 987654; // in no dbstr
    CHECK(conn.aa_name(passive).empty()); // the raw title, as the plugins and /alternateadv list read it
    CHECK(conn.aa_display_name(passive) == "Ability 10");
    game::AltAbility activated = passive;
    activated.spell = 8000;
    CHECK(conn.aa_display_name(activated) == "Fixture Discipline");
    activated.spell = 8001; // a spell the table does not have
    CHECK(conn.aa_display_name(activated) == "Ability 10");
}

TEST_CASE("aa: buy all ranks on a stock server buys one rank after another") {
    test::Rig rig(test::MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    for (auto [id, r, prev, next] : {std::tuple{100u, 1, -1, 101}, std::tuple{101u, 2, 100, 102},
                                     std::tuple{102u, 3, 101, -1}}) {
        game::AltAbility a = rank(id, 10, r, prev, next);
        a.classes = 1u << 1; // the mock's warrior
        a.level_req = 1;
        conn.receive_zone_packet_for_test(encode_send_aa_table(a));
    }
    conn.receive_zone_packet_for_test(encode(AAExpUpdate{0, 10, 0}));
    auto trains = [&] {
        std::vector<std::uint32_t> out;
        for (const AppPacket& p : rig.server.zone_log)
            if (p.opcode == to_wire(Op::AAAction))
                if (auto a = decode_aa_action(p); a && a->action == kAATrain) out.push_back(a->ability);
        return out;
    };
    auto bought = [&](std::uint32_t id, std::uint32_t points) {
        RespondAA r;
        r.owned = {{id, 1, 0}};
        conn.receive_zone_packet_for_test(encode(r));
        conn.receive_zone_packet_for_test(encode(AAExpUpdate{0, points, 0}));
    };

    REQUIRE(conn.aa_train(100, true));
    REQUIRE(rig.until([&] { return trains().size() == 1; }));
    bought(100, 8);
    REQUIRE(rig.until([&] { return trains().size() == 2; }));
    bought(101, 6);
    REQUIRE(rig.until([&] { return trains().size() == 3; }));
    CHECK(trains() == std::vector<std::uint32_t>{100, 101, 102});
    bought(102, 4); // the last rank: nothing more
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(trains().size() == 3);
    for (const AppPacket& p : rig.server.zone_log)
        if (p.opcode == to_wire(Op::AAAction)) CHECK(decode_aa_action(p)->action != kAATrainAll);

    // An update without the rank owned (experience from a kill) leaves the chain waiting for it.
    RespondAA none;
    conn.receive_zone_packet_for_test(encode(none));
    conn.receive_zone_packet_for_test(encode(AAExpUpdate{0, 10, 0}));
    REQUIRE(conn.aa_train(100, true));
    REQUIRE(rig.until([&] { return trains().size() == 4; }));
    conn.receive_zone_packet_for_test(encode(AAExpUpdate{0, 10, 50}));
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(trains().size() == 4);
    bought(100, 8);
    REQUIRE(rig.until([&] { return trains().size() == 5; }));
    CHECK(trains().back() == 101);

    // A refused purchase (EQEmu sends nothing) leaves it waiting too; a plain train ends it and buys one rank.
    REQUIRE(conn.aa_train(101));
    REQUIRE(rig.until([&] { return trains().size() == 6; }));
    bought(101, 6);
    rig.run_for(std::chrono::milliseconds(50));
    CHECK(trains().size() == 6);
}

TEST_CASE("aa: reuse timers translate the server's clock; readiness subtracts SPA 264") {
    game::AltAdvState s;
    game::AltAbility a = rank(500, 50, 1, -1, -1);
    a.spell = 1000;
    a.refresh = 600;
    a.timers = {7};
    s.add(a);
    // Used on the server at 1000 when its clock read 1100; our clock reads 5000: started at 4900.
    s.set_timer(7, 1000, 1100, 5000);
    CHECK(s.timers[7] == 4900);
    auto r = s.readiness(*s.get(500), 0, 5000);
    CHECK_FALSE(r.ready);
    CHECK(r.total == 600);
    CHECK(r.remaining == 500);
    r = s.readiness(*s.get(500), 100, 5000);
    CHECK(r.remaining == 400);
    CHECK(s.readiness(*s.get(500), 0, 5500).ready);
    // An activation sends begin 0, end 0: the timer starts now.
    s.set_timer(7, 0, 0, 6000);
    CHECK(s.timers[7] == 6000);
    CHECK(s.readiness(*s.get(500), 600, 6000).ready); // no reuse time left after the reduction
    s.set_timer(100, 0, 0, 1);                         // out of range: ignored
    // Past 2038 the 32-bit times are unsigned: a start a minute ago is not ready for its 600 s.
    s.set_timer(7, 0x90000000u - 60u, 0x90000000u, std::int64_t{0x90000000});
    CHECK(s.timers[7] == std::int64_t{0x90000000} - 60);
    CHECK_FALSE(s.readiness(*s.get(500), 0, std::int64_t{0x90000000}).ready);
    // The owned ranks' effects for the character sheet.
    s.owned = {{500, 2, 0}, {999, 1, 0}};
    s.get(500);
    game::AltAbility& own = s.table[500];
    own.effects = {{262, 5, 0, 1}};
    auto sheet = s.sheet_abilities();
    REQUIRE(sheet.size() == 1);
    CHECK(sheet[0].effects[0].spa == 262);
    CHECK(sheet[0].class_mask == a.classes);
    // Into a list that held more and other ranks: the same list, its old entries gone.
    std::vector<game::mechanics::SheetAbility> into(3);
    into[0].id = 1;
    into[0].effects = {{1, 2, 3}, {4, 5, 6}};
    s.sheet_abilities_into(into);
    REQUIRE(into.size() == 1);
    CHECK(into[0].id == sheet[0].id);
    REQUIRE(into[0].effects.size() == 1);
    CHECK(into[0].effects[0].spa == 262);
    CHECK(into[0].class_mask == a.classes);
}

TEST_CASE("training: OP_GMTraining both ways, OP_GMTrainSkill, OP_GMEndTraining, the confirmation") {
    GMTraining req;
    req.trainer_id = 300;
    req.player_id = 12;
    AppPacket p = encode(req);
    REQUIRE(p.data.size() == kGMTrainingSize);
    auto b = bytes_of(p);
    CHECK(b[0] == 44);
    CHECK(b[1] == 1);
    CHECK(b[4] == 12);
    CHECK(b[0x198] == 0);

    // OPGMTraining's answer: the skills, then the 40 "ending" bytes at size - 40.
    Buf w;
    w.u32(300).u32(12);
    for (int i = 0; i < 100; ++i) w.u32(i == 0 ? 50 : (i == 33 ? 100 : 0));
    const std::uint8_t ending[] = {0x34, 0x87, 0x8a, 0x3f, 0x01};
    w.b.insert(w.b.end(), ending, ending + 5);
    for (int i = 0; i < 32; ++i) w.u8(0xc9);
    w.u8(0x76).u8(0x75).u8(0x3f);
    REQUIRE(w.b.size() == 448);
    auto g = decode_gm_training(packet(Op::GMTraining, w.b));
    REQUIRE(g);
    CHECK(g->trainer_id == 300);
    CHECK(g->skills[0] == 50);
    CHECK(g->greed == doctest::Approx(1.0822f).epsilon(0.001));
    CHECK(g->success == 1);
    CHECK(g->languages[31] == 0xc9);

    Buf t;
    t.u32(300).u32(0).u32(33);
    CHECK(bytes_of(encode_gm_train_skill(300, false, 33)) == t.b);
    Buf l;
    l.u32(300).u32(1).u32(4);
    CHECK(bytes_of(encode_gm_train_skill(300, true, 4)) == l.b);
    Buf e;
    e.u32(300).u32(12);
    CHECK(bytes_of(encode_gm_end_training(300, 12)) == e.b);

    GMTrainSkillConfirm c{33, 1250, 0, "Guildmaster Hanson"};
    AppPacket cp = encode(c);
    CHECK(cp.data.size() == 76);
    auto dc = decode_gm_train_skill_confirm(cp);
    REQUIRE(dc);
    CHECK(dc->skill == 33);
    CHECK(dc->cost == 1250);
    CHECK(dc->trainer == "Guildmaster Hanson");
}

TEST_CASE("training: rows, cost, checkLang and the train checks") {
    game::TraineeView who;
    who.race = 1; // human
    who.class_id = 1;
    who.level = 10;
    who.practice_points = 3;
    std::array<std::int32_t, 100> skills{};
    skills[0] = 15;
    who.skill = [&](std::uint32_t s) { return skills[s]; };
    who.language = [](std::uint32_t) { return 0; };
    who.class_cap = [](std::uint32_t s) { return s == 0 || s == 1 || s == 33 || s == 15 || s == 0x3c ? 50 : 0; };
    who.min_level = [](std::uint32_t s) { return s == 0 || s == 33 || s == 15 ? 1 : (s == 1 ? 20 : 0); };
    std::array<std::uint32_t, 100> caps{};
    caps[0] = 50;
    caps[1] = 50;  // the class has it later (min level 20)
    caps[15] = 50; // never listed
    caps[33] = 50; // never listed
    caps[72] = 50; // never listed
    caps[2] = 50;  // the class never gets it
    std::array<std::uint8_t, 32> langs{};
    langs[0] = 0xc9;
    langs[1] = 0xc9;
    auto rows = game::training_rows(who, caps, langs);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].kind == 0);
    CHECK(rows[1].kind == 1);
    CHECK(rows[2].kind == 100); // common tongue: everyone
    CHECK(rows[3].kind == 101); // barbarian for humans from level 10
    CHECK(game::can_learn_language(who, 1));
    who.level = 9;
    CHECK_FALSE(game::can_learn_language(who, 1));
    who.level = 10;
    CHECK_FALSE(game::can_learn_language(who, 25)); // beyond 0x18
    game::TraineeView wiz = who;
    wiz.class_id = 12;
    wiz.level = 8;
    CHECK(game::can_learn_language(wiz, 11)); // wizards: Old Erudian... the class exceptions

    CHECK(game::training_cost(10) == 0);
    CHECK(game::training_cost(15) == 1);  // 125 / 100
    CHECK(game::training_cost(60) == 1250);
    // A server's skill past 1301: the cube wraps in 32 bits, then the signed division (it overflowed).
    CHECK(game::training_cost(1500) == -9870182); // 1490^3 = 0xc52b4bc8
    CHECK(game::training_cost(INT32_MIN) == -10); // the sub wraps too: 0x7ffffff6^3 = 0xfffffc18

    CHECK_FALSE(game::check_train(who, rows[0]));
    auto r = game::check_train(who, rows[1]);
    REQUIRE(r);
    CHECK(r->id == 12929); // not ready: level 20 needed
    skills[0] = 50;
    r = game::check_train(who, rows[0]);
    REQUIRE(r);
    CHECK(r->id == 12931); // more skilled than the guildmaster
    who.practice_points = 0;
    r = game::check_train(who, rows[0]);
    REQUIRE(r);
    CHECK(r->id == 12925);
    // Tradeskills above 20 must be learned in the field.
    who.practice_points = 1;
    skills[0x3c] = 21;
    r = game::check_train(who, game::TrainingRow{0x3c, 50});
    REQUIRE(r);
    CHECK(r->id == 117);
    CHECK_FALSE(r->trainer);
    // Tinkering for non-gnomes: refused silently.
    r = game::check_train(who, game::TrainingRow{0x39, 50});
    REQUIRE(r);
    CHECK(r->id == 0);
}

TEST_CASE("skills: OP_SkillUpdate's announce byte") {
    Buf w;
    w.u32(5).u32(23).u8(1).u8(80).u8(136).u8(54); // EQEmu's observed trailer
    auto u = decode_skill_update(packet(Op::SkillUpdate, w.b));
    REQUIRE(u);
    CHECK(u->skill == 5);
    CHECK(u->value == 23);
    CHECK(u->announce == 1);
    auto e = encode_skill_update(SkillUpdate{5, 23, 0});
    CHECK(e.data.size() == 12);
    CHECK(std::to_integer<int>(e.data[8]) == 0);
}

TEST_CASE("tasks: OP_TaskDescription in the RoF2 encoding") {
    Buf w;
    w.u32(1).u32(77).u8(1).u32(2).u32(0); // header (packed): sequence, task, open window, type, reward type
    w.str("Rat Trouble");
    w.u32(3600).u32(0).u32(120); // duration, code, elapsed
    w.str("Kill the rats.");
    w.u8(1).u32(500).u32(1).u32(0); // rewards
    w.str("Some coin");
    w.str("");
    w.u32(0).u8(0); // trailer
    auto t = decode_task_description(packet(Op::TaskDescription, w.b));
    REQUIRE(t);
    CHECK(t->sequence == 1);
    CHECK(t->task_id == 77);
    CHECK(t->open_window);
    CHECK(t->type == 2);
    CHECK(t->title == "Rat Trouble");
    CHECK(t->duration == 3600);
    CHECK(t->elapsed == 120);
    CHECK(t->description == "Kill the rats.");
    CHECK(t->coin_reward == 500);
    CHECK(t->xp_reward);
    CHECK_FALSE(t->faction_reward);
    CHECK(t->reward == "Some coin");
    CHECK(bytes_of(encode_task_description(*t)) == w.b);
}

TEST_CASE("tasks: OP_TaskActivity long and short forms") {
    Buf w;
    w.u32(1).u32(2).u32(77).u32(0).u32(0);       // sequence, type, task, activity, step
    w.i32(2).u8(0).i32(0).str("a large rat");     // kill, not optional
    w.lstr("").i32(5).lstr("-1").lstr("0").str("1"); // items, goal, skills, spells, zones
    w.i32(0).str("").i32(3).u8(1).str("1");       // dz switch, description, done, 1, zones
    auto a = decode_task_activity(packet(Op::TaskActivity, w.b));
    REQUIRE(a);
    CHECK(a->task_id == 77);
    CHECK_FALSE(a->element.hidden);
    CHECK(a->element.type == 2);
    CHECK(a->element.target_name == "a large rat");
    CHECK(a->element.goal_count == 5);
    CHECK(a->element.skill_list == "-1");
    CHECK(a->element.zones == "1");
    CHECK(a->element.done_count == 3);
    CHECK_FALSE(a->element.done());
    CHECK(bytes_of(encode(*a)) == w.b);

    Buf s;
    s.u32(1).u32(2).u32(77).u32(1).u32(1).u32(0xffffffffu).u8(1);
    REQUIRE(s.b.size() == 25);
    auto h = decode_task_activity(packet(Op::TaskActivity, s.b));
    REQUIRE(h);
    CHECK(h->element.hidden);
    CHECK(h->element.activity_id == 1);
    CHECK(h->element.optional);
    CHECK(bytes_of(encode(*h)) == s.b);

    // The state: the description first, then its elements; a different task id is ignored.
    game::TaskState st;
    game::Task t;
    t.sequence = 1;
    t.type = 2;
    t.task_id = 77;
    t.title = "Rat Trouble";
    st.describe(t);
    CHECK(st.update_element(2, 1, 77, a->element));
    CHECK(st.update_element(2, 1, 77, h->element));
    CHECK_FALSE(st.update_element(2, 1, 78, a->element));
    REQUIRE(st.find(2, 1));
    CHECK(st.find(2, 1)->elements.size() == 2);
    st.describe(t); // the same task again keeps its elements
    CHECK(st.find(2, 1)->elements.size() == 2);
    CHECK(st.remove(2, 1));
    CHECK(st.tasks.empty());
}

TEST_CASE("tasks: completed list, select window, history, completion, cancel and the client's requests") {
    Buf c;
    c.u32(2).u32(10).str("First").u32(1700000000).u32(11).str("Second").u32(1700000100);
    auto done = decode_completed_tasks(packet(Op::CompletedTasks, c.b));
    REQUIRE(done);
    REQUIRE(done->size() == 2);
    CHECK((*done)[1].title == "Second");
    CHECK(bytes_of(encode_completed_tasks(*done)) == c.b);

    Buf s;
    s.u32(1).u32(2).u32(345);                                     // count, type, giver
    s.u32(77).f32(1.0f).u32(0).u32(2).str("Rat Trouble").str("Rats!").u8(0); // selector
    s.u32(1).i32(0).i32(2).i32(0).str("a large rat").lstr("").i32(5).lstr("-1").lstr("0").str("").str("").str("");
    auto o = decode_task_select(packet(Op::TaskSelectWindow, s.b));
    REQUIRE(o);
    CHECK_FALSE(o->shared);
    CHECK(o->giver_id == 345);
    REQUIRE(o->offers.size() == 1);
    CHECK(o->offers[0].task_id == 77);
    CHECK(o->offers[0].duration_code == 2);
    REQUIRE(o->offers[0].elements.size() == 1);
    CHECK(o->offers[0].elements[0].goal_count == 5);
    CHECK(bytes_of(encode_task_select(*o, Op::TaskSelectWindow)) == s.b);
    auto shared = decode_task_select(encode_task_select(*o, Op::SharedTaskSelectWindow));
    REQUIRE(shared);
    CHECK(shared->shared);

    Buf h;
    h.u32(0).u32(1).u32(2).str("a large rat").lstr("").u32(5).lstr("-1").lstr("-1").str("1").u32(0).str("").u8(0x31).str("1");
    auto hist = decode_task_history(packet(Op::TaskHistoryReply, h.b));
    REQUIRE(hist);
    REQUIRE(hist->elements.size() == 1);
    CHECK(hist->elements[0].target_name == "a large rat");
    CHECK(hist->elements[0].goal_count == 5);
    CHECK(bytes_of(encode(*hist)) == h.b);

    Buf a;
    a.u32(1).u32(2).u32(77).u32(0).u32(1).u32(0);
    auto ac = decode_task_activity_complete(packet(Op::TaskActivityComplete, a.b));
    REQUIRE(ac);
    CHECK(ac->completed == 1);
    CHECK(ac->stage_complete == 0);
    CHECK(bytes_of(encode(*ac)) == a.b);

    Buf cancel;
    cancel.u32(1).u32(2);
    CHECK(bytes_of(encode_cancel_task(1, 2)) == cancel.b);
    auto dc = decode_cancel_task(packet(Op::CancelTask, cancel.b));
    REQUIRE(dc);
    CHECK(dc->sequence == 1);
    CHECK(dc->type == 2);

    Buf accept;
    accept.u32(2).u32(77).u32(345);
    CHECK(bytes_of(encode_accept_task(2, 77, 345)) == accept.b);
    Buf sa;
    sa.i32(0).i32(0).u32(345).u32(77).f32(1.0f);
    CHECK(bytes_of(encode_shared_task_accept(345, 77, 1.0f)) == sa.b);
    Buf hr;
    hr.u32(3);
    CHECK(bytes_of(encode_task_history_request(3)) == hr.b);
    Buf timer;
    timer.u32(15000);
    CHECK(decode_task_request_timer(packet(Op::TaskRequestTimer, timer.b)) == 15000u);
}

TEST_CASE("tasks: GetElementDescription's templates") {
    game::EqStrings s;
    s.set(3468, "kill %1 %2");
    s.set(3498, "loot %1 %3 from %2");
    s.set(3470, "loot %1 %3");
    s.set(6052, "use %4 on %2");
    s.set(6054, "use %5 on %2");
    game::TaskElement e;
    e.hidden = false;
    e.type = 2;
    e.goal_count = 5;
    e.target_name = "a large rat";
    CHECK(game::element_description(e, s, {}) == "kill 5 a large rat");
    e.type = 3;
    e.item_list = "rat whiskers";
    CHECK(game::element_description(e, s, {}) == "loot 5 rat whiskers from a large rat");
    e.target_name.clear();
    CHECK(game::element_description(e, s, {}) == "loot 5 rat whiskers");
    e.type = 9;
    e.target_name = "a bat";
    e.spell_list = "200;201";
    auto name = [](std::uint32_t id) { return id == 200 ? std::string("Minor Healing") : std::string("Light Healing"); };
    CHECK(game::element_description(e, s, name) == "use Minor Healing or Light Healing on a bat");
    e.type = 42;
    CHECK(game::element_description(e, s, {}) == "Unknown index 42.");
    e.description = "talk to Guard Gehnus";
    CHECK(game::element_description(e, s, {}) == "talk to Guard Gehnus");
}
