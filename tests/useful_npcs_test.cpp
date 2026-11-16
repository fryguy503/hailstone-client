// The Find window (a Hailstone addition, docs/navigation.md "Find"): the useful NPCs of a zone by class and by the
// server's findable list (game/useful_npcs.h), OP_SendFindableNPCs (net/movement_packets.h) as GameConnection keeps
// it, and the hs.Find* calls the packs draw it with (app/lua_ui_find.cpp).
#include <doctest/doctest.h>

#include <string>
#include <vector>
#include <unordered_set>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "core/game_dir.h"
#include "core/process_serial.h"
#include "game/useful_npcs.h"
#include "game/world_state.h"
#include "net/game_connection.h"
#include "net/movement_packets.h"
#include "net/quest_npc_packets.h"
#include "net/world_packets.h"
#include "scratch_dir.h"
#include "mq_test_world.h"

using namespace hailstone;
using game::NpcCategory;

namespace {

game::Spawn npc(std::uint32_t id, const std::string& name, std::uint8_t cls, glm::vec3 at = {}, std::string last = {}) {
    game::Spawn s;
    s.id = id;
    s.incarnation = core::next_process_identity();
    s.name = name;
    s.last_name = std::move(last);
    s.class_id = cls;
    s.is_npc = true;
    s.position = at;
    return s;
}

std::string run(lua_State* L, const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

} // namespace

TEST_CASE("Find: an NPC's kind comes from its class, and the findable list makes the rest points of interest") {
    CHECK(game::npc_category(npc(1, "a", 41), false) == NpcCategory::Merchant);
    CHECK(game::npc_category(npc(1, "a", 40), false) == NpcCategory::Banker);
    CHECK(game::npc_category(npc(1, "a", 66), false) == NpcCategory::Banker);
    CHECK(game::npc_category(npc(1, "a", 20), false) == NpcCategory::Guildmaster);
    CHECK(game::npc_category(npc(1, "a", 35), false) == NpcCategory::Guildmaster);
    CHECK(game::npc_category(npc(1, "a", 70), false) == NpcCategory::SpecialMerchant);
    CHECK(game::npc_category(npc(1, "a", 61), false) == NpcCategory::SpecialMerchant);
    CHECK(game::npc_category(npc(1, "a", 63), false) == NpcCategory::Service);
    CHECK(game::npc_category(npc(1, "a", 71), false) == NpcCategory::Service);
    // an ordinary NPC: listed only when the server marks it findable
    CHECK_FALSE(game::npc_category(npc(1, "a", 1), false));
    CHECK(game::npc_category(npc(1, "a", 1), true) == NpcCategory::PointOfInterest);
    // a merchant on the findable list stays a merchant
    CHECK(game::npc_category(npc(1, "a", 41), true) == NpcCategory::Merchant);
    // never listed: LDoN objects, players, corpses, pets
    CHECK_FALSE(game::npc_category(npc(1, "a", 62), true));
    auto pc = npc(1, "a", 41);
    pc.is_npc = false;
    CHECK_FALSE(game::npc_category(pc, true));
    auto corpse = npc(1, "a", 41);
    corpse.is_corpse = true;
    CHECK_FALSE(game::npc_category(corpse, true));
    auto pet = npc(1, "a", 41);
    pet.pet_owner_id = 5;
    CHECK_FALSE(game::npc_category(pet, true));

    for (int i = 0; i < game::kNpcCategoryCount; ++i) {
        CHECK_FALSE(game::npc_category_key(static_cast<NpcCategory>(i)).empty());
        CHECK_FALSE(game::npc_category_label(static_cast<NpcCategory>(i)).empty());
    }
}

TEST_CASE("Find: the list is nearest first, named as shown, with the surname or the class as its subtitle") {
    game::WorldState w;
    w.player_id = 9;
    w.spawns[9] = npc(9, "Me", 41); // the player itself is never listed
    w.spawns[9].is_npc = false;
    w.spawns[1] = npc(1, "Banker_Bob00", 40, {30, 0, 0});
    w.spawns[2] = npc(2, "Arms_Dealer01", 41, {10, 0, 0}, "Weapon Merchant");
    w.spawns[3] = npc(3, "Wilt00", 20, {20, 0, 0});
    w.spawns[4] = npc(4, "a_rat00", 1, {1, 0, 0});    // not listed
    w.spawns[5] = npc(5, "Lady_Vox00", 1, {50, 0, 0}); // on the findable list
    auto list = game::useful_npcs(w, {5}, {0, 0, 0});
    REQUIRE(list.size() == 4);
    CHECK(list[0].spawn_id == 2);
    CHECK(list[0].name == "Arms Dealer");
    CHECK(list[0].subtitle == "Weapon Merchant");
    CHECK(list[0].distance == doctest::Approx(10.0f));
    CHECK(list[1].spawn_id == 3);
    CHECK(list[1].subtitle == "Warrior Guildmaster");
    CHECK(list[2].spawn_id == 1);
    CHECK(list[2].subtitle == "Banker");
    CHECK(list[3].spawn_id == 5);
    CHECK(list[3].category == NpcCategory::PointOfInterest);
    CHECK(list[3].subtitle.empty());

    // the search: name or subtitle, any case, spaces round it ignored
    CHECK(game::useful_npc_matches(list[0], ""));
    CHECK(game::useful_npc_matches(list[0], "  "));
    CHECK(game::useful_npc_matches(list[0], "arms"));
    CHECK(game::useful_npc_matches(list[0], " WEAPON "));
    CHECK_FALSE(game::useful_npc_matches(list[0], "banker"));
    CHECK_FALSE(game::useful_npc_matches(list[0], "Weapon Merchant and more"));
}

TEST_CASE("Find: OP_SendFindableNPCs reads as Handle_OP_SendFindableNPCs does, and GameConnection keeps the list") {
    net::FindableNpc n;
    n.spawn_id = 321;
    n.name = "Guard Gehnus";
    n.last_name = "Qeynos Guard";
    n.race = 71;
    n.class_id = 1;
    auto p = net::encode_findable_npc(n);
    CHECK(p.data.size() == 112); // EQEmu's FindableNPC_Struct
    auto d = net::decode_findable_npc(p);
    REQUIRE(d);
    CHECK_FALSE(d->remove);
    CHECK(d->spawn_id == 321);
    CHECK(d->name == "Guard Gehnus");
    CHECK(d->last_name == "Qeynos Guard");
    CHECK(d->race == 71);
    CHECK(d->class_id == 1);
    p.data.resize(108); // the class byte cut off
    CHECK_FALSE(net::decode_findable_npc(p));

    hailstone::test::ScratchDir scratch("useful_npcs_conn");
    GameDir game(scratch.path());
    net::GameConnection conn(game, {});
    conn.receive_zone_packet_for_test(net::encode_findable_npc(n));
    n.spawn_id = 322;
    conn.receive_zone_packet_for_test(net::encode_findable_npc(n));
    CHECK(conn.findable_npcs().size() == 2);
    n.remove = true;
    conn.receive_zone_packet_for_test(net::encode_findable_npc(n));
    REQUIRE(conn.findable_npcs().size() == 1);
    CHECK(conn.findable_npcs().contains(321));
    // zoning forgets the old zone's ids
    conn.receive_world_packet_for_test(net::encode(net::ZoneServerInfo{"127.0.0.1", 7001}));
    CHECK(conn.findable_npcs().empty());
}

TEST_CASE("Find: hs.FindNpcs lists the zone's NPCs for the packs; FindNpcTarget targets; FindOpen is the window") {
    hailstone::test::ScratchDir scratch("useful_npcs_eq");
    GameDir game(scratch.path());
    net::GameConnection conn(game, {});
    net::GameConnectionTestAccess::state(conn) = net::ConnectionState::InZone;
    conn.world_for_test().player_id = 1;
    conn.world_for_test().spawns[1] = npc(1, "Tester", 1);
    conn.world_for_test().spawns[1].is_npc = false;
    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::UiApi api(host);
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "hs");

    auto& w = conn.world_for_test();
    w.spawns[2] = npc(2, "Arms_Dealer01", 41, {10, 20, 3}, "Weapon Merchant");
    w.spawns[3] = npc(3, "Wilt00", 20, {40, 0, 0});
    w.spawns[4] = npc(4, "a_rat00", 1, {1, 0, 0});
    ++w.spawn_set_serial;
    net::FindableNpc f;
    f.spawn_id = 4;
    conn.receive_zone_packet_for_test(net::encode_findable_npc(f));

    CHECK(run(L, "return #hs.FindNpcs()") == "3");
    CHECK(run(L, "local r = hs.FindNpcs()[1] return r.name .. '|' .. r.category .. '|' .. r.label") ==
          "a rat|poi|Points of interest");
    // map units are the world's negated and swapped (game/map_data.h)
    CHECK(run(L, "local r = hs.FindNpcs()[2] return r.id .. ' ' .. r.x .. ' ' .. r.y .. ' ' .. r.subtitle") ==
          "2 -20.0 -10.0 Weapon Merchant");
    CHECK(run(L, "return #hs.FindNpcs('guildmaster')") == "1");
    CHECK(run(L, "return #hs.FindNpcs('nobody')") == "0");
    CHECK(run(L, "return #hs.FindCategories() .. hs.FindCategories()[1][1]") == "7merchant");

    CHECK(run(L, "return hs.FindNpcTarget(3)") == "true");
    CHECK(conn.target() == 3);
    CHECK(run(L, "return hs.FindNpcTarget(99)") == "false");

    // Walk there: through the host's walk (the navigator in the client), its refusal handed back
    std::vector<std::uint32_t> walked;
    host.walk_to = [&](std::uint32_t id) -> std::string {
        walked.push_back(id);
        return id == 3 ? "" : "no path to x";
    };
    CHECK(run(L, "local ok, why = hs.FindNpcWalk(3) return tostring(ok) .. '|' .. why") == "true|");
    CHECK(run(L, "local ok, why = hs.FindNpcWalk(2) return tostring(ok) .. '|' .. why") == "false|no path to x");
    CHECK(run(L, "local ok, why = hs.FindNpcWalk(99) return tostring(ok) .. '|' .. why") == "false|no such NPC");
    CHECK(walked == std::vector<std::uint32_t>{3, 2});
    // Show path: the guide path when the host has one
    std::uint32_t guided = 0;
    host.guide_to = [&](std::uint32_t id) {
        guided = id;
        return true;
    };
    CHECK(run(L, "return hs.FindNpcPath(2)") == "true");
    CHECK(guided == 2);
    CHECK_FALSE(conn.player_path().active()); // not the server's find path

    CHECK(run(L, "return hs.FindOpen()") == "false");
    CHECK(run(L, "return hs.FindOpen(true)") == "true");
    CHECK(host.find_open);
    CHECK(run(L, "return hs.FindOpen(false)") == "false");

    host.in_game = false;
    CHECK(run(L, "return #hs.FindNpcs()") == "0");
    lua_close(L);
}

TEST_CASE("Find: quest givers come from the server script's list, matched by the name as shown") {
    // the classifier: a class's kind first, then quests, then the findable list
    CHECK(game::npc_category(npc(1, "a", 1), false, game::kQuestTalks) == NpcCategory::QuestGiver);
    CHECK(game::npc_category(npc(1, "a", 1), true, game::kQuestItems) == NpcCategory::QuestGiver);
    CHECK(game::npc_category(npc(1, "a", 41), false, game::kQuestItems) == NpcCategory::Merchant);

    game::WorldState w;
    w.spawns[1] = npc(1, "Guard_Gehnus01", 1, {5, 0, 0});
    w.spawns[2] = npc(2, "Merchant_Mo00", 41, {6, 0, 0});
    w.spawns[3] = npc(3, "a_rat00", 1, {7, 0, 0});
    w.spawns[4] = npc(4, "Old_Man00", 1, {8, 0, 0});
    game::QuestNpcMap quests{{"guard gehnus", game::kQuestItems | game::kQuestTalks},
                             {"merchant mo", game::kQuestTalks},
                             {"old man", game::kQuestTalks}};
    auto list = game::useful_npcs(w, {}, {0, 0, 0}, quests);
    REQUIRE(list.size() == 3);
    CHECK(list[0].category == NpcCategory::QuestGiver);
    CHECK(list[0].subtitle == "Takes items");
    CHECK(list[1].category == NpcCategory::Merchant); // a merchant keeps its kind, its quests noted
    CHECK(list[1].quests == game::kQuestTalks);
    CHECK(list[2].subtitle == "Has something to say");
    CHECK(game::npc_category_key(NpcCategory::QuestGiver) == "quest");

    // the packet both ways
    std::vector<net::QuestNpc> sent{{"Guard_Gehnus", 3}, {"Old_Man", 2}};
    auto p = net::encode_quest_npcs(sent);
    auto got = net::decode_quest_npcs(p);
    REQUIRE(got);
    REQUIRE(got->size() == 2);
    CHECK((*got)[0].name == "Guard_Gehnus");
    CHECK((*got)[0].flags == 3);
    CHECK((*got)[1].name == "Old_Man");
    p.data.pop_back(); // a name cut short
    CHECK_FALSE(net::decode_quest_npcs(p));
    auto req = net::encode_quest_npcs_request();
    CHECK(req.opcode == 0x1436);
    CHECK(req.data.size() == 6);

    // GameConnection keeps them by the name as shown, lower case, until zoning
    hailstone::test::ScratchDir scratch("useful_npcs_quests");
    GameDir game(scratch.path());
    net::GameConnection conn(game, {});
    conn.receive_zone_packet_for_test(net::encode_quest_npcs(sent));
    REQUIRE(conn.quest_npcs().size() == 2);
    CHECK(conn.quest_npcs().at("guard gehnus") == 3);
    CHECK(conn.quest_npcs().at("old man") == 2);
    conn.receive_world_packet_for_test(net::encode(net::ZoneServerInfo{"127.0.0.1", 7001}));
    CHECK(conn.quest_npcs().empty());
}
