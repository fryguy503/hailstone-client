#include <algorithm>
#include <limits>

#include "mock_server/harness.h"
#include "net/packet_io.h"
#include "net/task_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

constexpr std::uint32_t kForm = 2401;
constexpr std::uint32_t kMission = 810;

AppPacket shroud_offer(std::uint32_t context) {
    ByteWriter w;
    w.le(context);
    w.zeros(8);
    net::detail::write_cstring(w, "PROGRESSION\a" "10\a" "Forms\a" "Example forms\a"
                                  "BRANCH\a" "20\a" "Fighters\a" "Try a fighter\a" "25\a"
                                  "TEMPLATE\a" "Gnoll\a" "2401\a" "20\a" "1\a"
                                  "TEMPLATE\a" "Orc\a" "2402\a" "25\a" "0");
    return make_packet(Op::ShroudSelectionWindow, std::move(w));
}

AppPacket shroud_stats(std::uint32_t id) {
    ByteWriter w;
    w.le(id);
    w.le<std::uint32_t>(1);
    net::detail::write_cstring(w, std::to_string(id) + "^Gnoll^Fighter^1^20^300^0^200^100^90^40^70^45^75^50^0^0");
    return make_packet(Op::ShroudRespondStats, std::move(w));
}

AppPacket form_transition(const Spawn& spawn, const std::vector<Item>* items) {
    const auto spawn_packet = encode(spawn);
    ByteWriter w;
    w.le(spawn.spawn_id);
    w.le(static_cast<std::uint16_t>(6 + spawn_packet.data.size()));
    w.bytes(spawn_packet.data);
    if (items) {
        ByteWriter profile;
        profile.zeros(0xc);
        profile.le(spawn.race);
        profile.le(spawn.class_);
        profile.le(spawn.level);
        profile.zeros(0x39c - 0x12);
        profile.le<std::uint32_t>(300);
        profile.zeros(0x4ff8 - 0x3a0);
        w.bytes(profile.take());
        w.bytes(encode_char_inventory(*items).data);
    }
    return make_packet(Op::Shroud, std::move(w));
}

std::size_t sent_count(const Rig& rig, Op op) {
    return static_cast<std::size_t>(std::count_if(rig.server.zone_log.begin(), rig.server.zone_log.end(),
                                                  [op](const AppPacket& p) { return is_op(p, op); }));
}

} // namespace

TEST_CASE("mock server: Monster Mission selection uses the current task and server offer") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& zone = rig.server.zone;

    game::Task task;
    task.task_id = kMission;
    task.title = "A shared mission";
    task.type = game::kTaskTypeShared;
    zone.send_reliable(encode_task_description(task));
    REQUIRE(rig.until([&] { return conn.tasks().find_id(kMission) != nullptr; }));
    CHECK_FALSE(conn.shared_task_monster_select(kMission));

    zone.send_reliable(shroud_offer(kMission));
    REQUIRE(rig.until([&] { return conn.shroud_selection_open(); }));
    CHECK(conn.shared_task_monster_select_available(kMission));
    CHECK_FALSE(conn.shared_task_monster_select_available(kMission + 1));
    const auto revision = conn.shroud_selection_revision();
    CHECK_FALSE(conn.select_shroud(revision, kForm + 1));
    CHECK_FALSE(conn.request_shroud_stats(revision, 0));
    CHECK(conn.request_shroud_stats(revision, kForm));
    REQUIRE(rig.until([&] { return sent_count(rig, Op::ShroudRequestStats) == 1; }));
    zone.send_reliable(shroud_stats(kForm + 1));
    zone.send_reliable(shroud_stats(kForm));
    REQUIRE(rig.until([&] { return conn.shroud_stats().has_value(); }));
    CHECK(conn.shroud_stats()->selection_id == kForm);
    CHECK(conn.cancel_shroud_selection(revision));
    CHECK_FALSE(conn.shroud_selection_open());
    CHECK_FALSE(conn.select_shroud(revision, kForm));
    CHECK(conn.shared_task_monster_select(kMission));
    CHECK(conn.select_shroud(conn.shroud_selection_revision(), kForm));
    REQUIRE(rig.until([&] { return sent_count(rig, Op::ShroudSelect) == 1; }));
    CHECK(sent_count(rig, Op::ShroudSelectCancel) == 0);

    zone.send_reliable(encode(std::vector<SharedTaskMember>{{"Gennro", true, 1}, {"Helper", false, 3}}));
    REQUIRE(rig.until([&] { return conn.tasks().shared_members.size() == 2; }));
    CHECK(conn.shared_task_member_template("Helper") == 3);
    zone.send_reliable(encode(SharedTaskMemberChange{false, "Helper"}));
    REQUIRE(rig.until([&] { return conn.tasks().shared_members.size() == 1; }));
    CHECK(conn.shared_task_member_template("Helper") == 0);

    zone.send_reliable(encode_cancel_task(0, game::kTaskTypeShared));
    REQUIRE(rig.until([&] { return !conn.tasks().find_id(kMission); }));
    CHECK_FALSE(conn.shroud_selection_open());
    CHECK_FALSE(conn.shared_task_monster_select(kMission));
    CHECK(conn.shared_task_member_template("Gennro") == 0);
}

TEST_CASE("mock server: RoF2 Monster Mission templates enforce roster capacity and await the server result") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& zone = rig.server.zone;
    game::Task task;
    task.task_id = kMission;
    task.title = "A native mission";
    task.type = game::kTaskTypeShared;
    zone.send_reliable(encode_task_description(task));
    zone.send_reliable(encode(std::vector<SharedTaskMember>{{"Gennro", true}, {"Helper", false, kForm}}));

    ByteWriter catalog;
    catalog.zeros(8);
    catalog.le(kMission);
    catalog.zeros(4);
    catalog.le<std::uint8_t>(1); // A member with an assigned role cannot change it.
    catalog.le<std::uint32_t>(1);
    catalog.le(kForm);
    catalog.le<std::int32_t>(1);
    catalog.le<std::int32_t>(1);
    catalog.zeros(4);
    catalog.le<std::uint8_t>(1);
    net::detail::write_cstring(catalog, "Scout");
    zone.send_reliable(make_packet(Op::MonsterMissionCatalog, std::move(catalog)));
    REQUIRE(rig.until([&] { return conn.shared_task_monster_select_available(kMission); }));
    CHECK(conn.shroud_selection_open());
    CHECK(conn.shared_task_member_template_name("Helper") == "Scout");
    CHECK(conn.cancel_shroud_selection(conn.shroud_selection_revision()));
    CHECK(conn.shared_task_monster_select(kMission));
    REQUIRE(rig.until([&] { return sent_count(rig, Op::MonsterMissionCatalog) == 1; }));
    const auto revision = conn.shroud_selection_revision();
    CHECK_FALSE(conn.select_shroud(revision, kForm));

    const auto assign = [&](std::string_view name, std::uint32_t role) {
        ByteWriter assignment;
        assignment.zeros(8);
        assignment.fixed_string(name, 64);
        assignment.le(role);
        zone.send_reliable(make_packet(Op::MonsterMissionAssignment, std::move(assignment)));
    };
    assign("Helper", 0);
    REQUIRE(rig.until([&] { return conn.shared_task_member_template("Helper") == 0; }));
    assign("Gennro", kForm + 1);
    REQUIRE(rig.until([&] { return conn.shared_task_member_template("Gennro") == kForm + 1; }));
    CHECK_FALSE(conn.select_shroud(revision, kForm));
    assign("Gennro", 0);
    REQUIRE(rig.until([&] { return conn.shared_task_member_template("Gennro") == 0; }));
    CHECK(conn.request_shroud_stats(revision, kForm));
    REQUIRE(rig.until([&] { return sent_count(rig, Op::MonsterMissionStats) == 1; }));
    CHECK(conn.select_shroud(revision, kForm));
    CHECK(conn.shroud_selection_pending());
    CHECK_FALSE(conn.select_shroud(revision, kForm));
    REQUIRE(rig.until([&] { return sent_count(rig, Op::MonsterMissionSelect) == 1; }));
    CHECK(sent_count(rig, Op::ShroudSelect) == 0);

    ByteWriter result;
    result.zeros(8);
    result.le(kForm);
    result.le<std::uint32_t>(0);
    zone.send_reliable(make_packet(Op::MonsterMissionResult, std::move(result)));
    REQUIRE(rig.until([&] { return !conn.shroud_selection_pending(); }));
    CHECK_FALSE(conn.shroud_selection_open());
    CHECK_FALSE(conn.shrouded());
}

TEST_CASE("mock server: Shroud replaces its own profile and possessions only after a full transition") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& zone = rig.server.zone;
    REQUIRE(conn.profile());
    const auto normal = *conn.profile();
    const auto possession = ItemSlot::possessions(kSlotGeneral1);
    const ItemSlot bank{kInvBank, 0, -1, -1};
    Item pack;
    pack.slot = possession;
    pack.id = 10001;
    pack.name = "Normal possessions";
    Item banked = pack;
    banked.slot = bank;
    banked.id = 10002;
    Item shroud_item = pack;
    shroud_item.id = 10003;
    zone.send_reliable(encode_char_inventory({pack, banked}));
    REQUIRE(rig.until([&] { return conn.inventory().at(bank) != nullptr; }));
    Spawn keeper;
    keeper.spawn_id = 2469;
    keeper.name = "Spirit keeper";
    keeper.npc = 1;
    keeper.motion.x = normal.x + 30.0f;
    keeper.motion.y = normal.y + 25.0f;
    keeper.motion.z = normal.z;
    zone.send_reliable(encode(keeper));
    zone.send_reliable(shroud_offer(keeper.spawn_id));
    REQUIRE(rig.until([&] { return conn.shroud_selection_open(); }));
    CHECK(conn.shroud_selection()->context_id == 0);
    CHECK_FALSE(conn.select_shroud(conn.shroud_selection_revision(), kForm));
    const auto old_incarnation = conn.world().spawns.at(keeper.spawn_id).incarnation;
    keeper.motion.x = normal.x;
    keeper.motion.y = normal.y;
    zone.send_reliable(encode(keeper));
    REQUIRE(rig.until([&] { return conn.world().spawns.at(keeper.spawn_id).incarnation != old_incarnation; }));
    CHECK(conn.select_shroud(conn.shroud_selection_revision(), kForm));
    CHECK(conn.shroud_selection_pending());
    CHECK_FALSE(conn.select_shroud(conn.shroud_selection_revision(), kForm));

    Spawn spawn;
    spawn.spawn_id = conn.world().player_id;
    spawn.name = conn.character_name();
    spawn.race = 39;
    spawn.class_ = 1;
    spawn.level = 20;
    spawn.size = 5.5f;
    zone.send_reliable(form_transition(spawn, nullptr));
    const std::vector form_items{shroud_item};
    zone.send_reliable(form_transition(spawn, &form_items));
    REQUIRE(rig.until([&] { return conn.shrouded(); }));
    CHECK_FALSE(conn.monster_shrouded());
    CHECK_FALSE(conn.shroud_selection_open());
    CHECK(conn.profile()->class_ == 1);
    CHECK(conn.profile()->race == 39);
    CHECK(conn.profile()->character_id == normal.character_id);
    CHECK(conn.player_status().level == 20);
    REQUIRE(conn.world().player());
    CHECK(conn.world().player()->race == 39);
    REQUIRE(conn.inventory().at(possession));
    CHECK(conn.inventory().at(possession)->id == shroud_item.id);
    REQUIRE(conn.inventory().at(bank));
    CHECK(conn.inventory().at(bank)->id == banked.id);

    zone.send_reliable(make_empty(Op::ShroudRemove));
    spawn.race = normal.race;
    spawn.class_ = normal.class_;
    spawn.level = normal.level;
    const std::vector normal_items{pack};
    zone.send_reliable(form_transition(spawn, &normal_items));
    REQUIRE(rig.until([&] { return !conn.shrouded(); }));
    CHECK(conn.profile()->level == normal.level);
    REQUIRE(conn.inventory().at(possession));
    CHECK(conn.inventory().at(possession)->id == pack.id);
}

TEST_CASE("mock server: Shroud Bank withdrawals wait for the matching acknowledgement") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& zone = rig.server.zone;
    CHECK_FALSE(conn.can_open_shroud_bank());

    Spawn banker;
    banker.spawn_id = 2468;
    banker.name = "Banker";
    banker.npc = 1;
    banker.class_ = 40;
    banker.motion.x = conn.profile()->x;
    banker.motion.y = conn.profile()->y;
    banker.motion.z = conn.profile()->z;
    zone.send_reliable(encode(banker));
    REQUIRE(rig.until([&] { return conn.world().spawns.contains(banker.spawn_id); }));

    constexpr std::uint64_t key = 0x10200000003ull;
    ByteWriter list;
    list.le<std::uint32_t>(3600);
    list.le<std::uint32_t>(1);
    net::detail::write_cstring(list, "Recovered item");
    list.le<std::uint32_t>(501);
    list.le(key);
    list.le<std::uint32_t>(2);
    zone.send_reliable(encode_alt_storage_open(banker.spawn_id));
    zone.send_reliable(make_packet(Op::AltStorageList, std::move(list)));
    REQUIRE(rig.until([&] { return conn.shroud_bank() && conn.shroud_bank()->loaded; }));
    const auto revision = conn.shroud_bank()->revision;
    CHECK_FALSE(conn.withdraw_shroud_bank(revision, key + 1));
    CHECK(conn.withdraw_shroud_bank(revision, key));
    CHECK_FALSE(conn.withdraw_shroud_bank(revision, key));
    REQUIRE(rig.until([&] { return sent_count(rig, Op::AltStorage) == 1; }));
    zone.send_reliable(encode_alt_storage_withdraw(std::numeric_limits<std::uint64_t>::max()));
    REQUIRE(rig.until([&] { return !conn.shroud_bank()->pending_key; }));
    CHECK(conn.shroud_bank()->entries.size() == 1);
    CHECK(conn.withdraw_shroud_bank(revision, key));
    zone.send_reliable(encode_alt_storage_withdraw(key));
    REQUIRE(rig.until([&] { return conn.shroud_bank()->entries.empty(); }));
    CHECK_FALSE(conn.inspect_shroud_bank(revision, key, false));
}

TEST_CASE("mock server: the Shroud Bank notices read the game's strings in the original's colours") {
    Rig rig;
    REQUIRE(rig.to_zone());
    auto& conn = rig.conn;
    auto& zone = rig.server.zone;
    std::vector<ChatEvent> lines;
    const auto drain = [&] { while (auto e = conn.poll_chat()) lines.push_back(std::move(*e)); };
    const auto has = [&](std::string_view text, std::uint32_t color) {
        drain();
        return std::any_of(lines.begin(), lines.end(),
                           [&](const ChatEvent& e) { return e.text == text && e.color == color; });
    };

    // 0x414320: a nonzero status is the "empty your Shroud Bank first" line, eqstr 4557 in colour 0xd.
    ByteWriter refused;
    refused.le<std::uint32_t>(0);
    refused.le<std::uint32_t>(1);
    refused.le<std::uint32_t>(0);
    zone.send_reliable(make_packet(Op::ShroudSelectionWindow, std::move(refused)));
    REQUIRE(rig.until([&] { return has("SHROUD BANK NOT EMPTY", 0xd); }));
    CHECK_FALSE(conn.shroud_selection());

    // 0x4136f0: the empty message is eqstr 4558 in colour 0xf.
    zone.send_reliable(make_empty(Op::AltStorageNoItems));
    REQUIRE(rig.until([&] { return has("SHROUD BANK NOTHING TO MOVE", 0xf); }));

    // CAltStorageWnd_HandleMessage case 6: coin handed back is a line (eqstr 6188), with or without the window.
    ByteWriter coin;
    coin.le<std::uint32_t>(6);
    coin.le<std::uint32_t>(3);
    coin.zeros(12);
    zone.send_reliable(make_packet(Op::AltStorage, std::move(coin)));
    REQUIRE(rig.until([&] { return has("SHROUD COIN 3 platinum", 0xf); }));
    CHECK_FALSE(conn.shroud_bank());
}
