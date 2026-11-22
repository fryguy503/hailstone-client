#include <doctest/doctest.h>
#include <lua.hpp>
#include <stdexcept>

#include "app/guide_path.h"
#include "app/lua_ui.h"
#include "game/server_ext_chain.h"
#include "mq_test_world.h"
#include "mock_server/harness.h"
#include "net/zone_packets.h"
#include "net/movement_packets.h"

using namespace hailstone;
namespace {
struct MapExtension final : game::ServerExtension {
    bool visible = true;
    bool find_visible = false;
    game::MapPolicy policy;
    std::uint64_t lifecycle = 0;
    bool marks = true;
    mutable unsigned mark_calls = 0;
    game::MapPolicy map_policy(const net::GameConnection&) const override { return policy; }
    std::uint64_t lifecycle_revision(const net::GameConnection&) const override { return lifecycle; }
    bool map_visible(const net::GameConnection&, const game::Spawn&, std::uint32_t purpose) const override {
        return purpose == 0 ? visible : find_visible;
    }
    std::optional<game::MapSpawnMark> map_spawn(const net::GameConnection&, const game::Spawn&,
                                               const game::MapSpawnMark& defaults) const override {
        ++mark_calls;
        return marks ? std::optional{defaults} : std::nullopt;
    }
};
// Keep the small actor fixture while using a real zone session for request admission.
void path_actors(net::GameConnection& connection) {
    mq_test::World actors("addon_map_path_actors");
    auto& world = net::GameConnectionTestAccess::world(connection);
    world.spawns = actors.conn->world().spawns;
    world.player_id = actors.conn->world().player_id;
    ++world.spawn_set_serial;
}
struct SaleExtension final : game::ServerExtension {
    int calls = 0;
    net::ItemSlot clicked;
    std::uint32_t item = 0, count = 0;
    bool cursor_occupied = false;
    bool merchant_right_click(net::GameConnection& c, std::uint32_t vendor, const net::ItemSlot& slot,
                              std::uint32_t id, std::uint32_t quantity) override {
        CHECK(vendor == 10);
        ++calls; clicked = slot; item = id; count = quantity; cursor_occupied = c.inventory().cursor() != nullptr; return true;
    }
};
}

TEST_CASE("server addon maps: ownership is exclusive, restrictions combine and inactive providers restore stock") {
    mq_test::World w("addon_map_ownership");
    game::ExtensionChain chain;
    auto before = std::make_shared<MapExtension>();
    auto owner = std::make_shared<MapExtension>();
    auto after = std::make_shared<MapExtension>();
    before->policy = {game::MapPolicyHideStockNpcs, "Other"};
    owner->policy = {game::MapPolicyCustomSpawns, "Owned map"};
    after->policy = {game::MapPolicyCustomSpawns | game::MapPolicyDisableNpcFind, "Later"};
    bool active = true;
    const int first = chain.add(before);
    chain.add(owner, [&](const auto&) { return active; });
    const int last = chain.add(after);
    CHECK(chain.map_policy(*w.conn).flags == 7);
    CHECK(chain.map_policy(*w.conn).name == "Owned map");
    const auto& npc = w.conn->world().spawns.at(10);
    CHECK(chain.map_spawn(*w.conn, npc, {}));
    CHECK(owner->mark_calls == 1);
    CHECK(before->mark_calls == 0); CHECK(after->mark_calls == 0);
    owner->marks = false;
    CHECK_FALSE(chain.map_spawn(*w.conn, npc, {})); // no fallthrough when the owner's class/range rules hide a mark
    CHECK(before->mark_calls == 0); CHECK(after->mark_calls == 0);
    after->visible = false;
    owner->marks = true;
    CHECK_FALSE(chain.map_spawn(*w.conn, npc, {})); // any provider can still deny
    active = false; chain.remove(last);
    CHECK(chain.map_policy(*w.conn).flags == game::MapPolicyHideStockNpcs);
    CHECK(chain.map_spawn(*w.conn, npc, {})); CHECK(before->mark_calls == 1);
    chain.remove(first);
    CHECK(chain.map_policy(*w.conn).flags == 0); CHECK(chain.map_policy(*w.conn).name.empty());
    CHECK_FALSE(chain.map_spawn(*w.conn, npc, {}));
}

TEST_CASE("server addon maps: effective revisions cover ownership content and independent restrictors") {
    mq_test::World world("addon_map_revision");
    game::ExtensionChain chain;
    CHECK(chain.map_policy(*world.conn).revision==0);
    auto owner=std::make_shared<MapExtension>();owner->policy={game::MapPolicyCustomSpawns,"Same name",7};
    const auto member=chain.add(owner);
    const auto first=chain.map_policy(*world.conn);
    CHECK(first.revision!=0);CHECK(chain.map_policy(*world.conn).revision==first.revision);
    owner->policy.revision=8;
    const auto content=chain.map_policy(*world.conn).revision;CHECK(content!=first.revision);
    struct Restrictor final : game::ServerExtension {
        bool map_visible(const net::GameConnection&,const game::Spawn&,std::uint32_t) const override{return false;}
    };
    const auto deny=chain.add(std::make_shared<Restrictor>());
    auto current=chain.map_policy(*world.conn);
    CHECK(current.flags==first.flags);CHECK(current.name==first.name);CHECK(current.revision!=content);
    CHECK(chain.map_policy(*world.conn).revision==current.revision);
    chain.remove(deny);
    const auto removed=chain.map_policy(*world.conn).revision;CHECK(removed!=current.revision);
    chain.remove(member);
    auto replacement=std::make_shared<MapExtension>();replacement->policy=owner->policy;
    const auto other=chain.add(replacement); // replacement was never observed as an empty chain
    current=chain.map_policy(*world.conn);
    CHECK(current.flags==first.flags);CHECK(current.name==first.name);CHECK(current.revision!=removed);
    chain.remove(other);CHECK(chain.map_policy(*world.conn).revision==0);
}

TEST_CASE("server addon maps: throwing self-removal releases deferred members and restores dispatch") {
    mq_test::World world("addon_map_throwing_provider");
    game::ExtensionChain chain;
    struct Throwing final : game::ServerExtension {
        game::ExtensionChain* chain = nullptr;
        int member = 0;
        game::MapPolicy map_policy(const net::GameConnection&) const override {
            chain->remove(member);
            throw std::runtime_error("policy callback failed");
        }
    };
    auto throwing = std::make_shared<Throwing>();
    throwing->chain = &chain;
    throwing->member = chain.add(throwing);
    std::weak_ptr<Throwing> retired = throwing;
    throwing.reset(); // the chain is the only remaining owner
    auto healthy = std::make_shared<MapExtension>();
    healthy->policy = {game::MapPolicyCustomSpawns, "Healthy", 1};
    const auto member = chain.add(healthy);
    CHECK_THROWS_WITH_AS(chain.map_policy(*world.conn), "policy callback failed", std::runtime_error);
    CHECK(retired.expired());
    const auto policy = chain.map_policy(*world.conn);
    CHECK(policy.flags == game::MapPolicyCustomSpawns);
    CHECK(policy.name == "Healthy");
    CHECK(policy.revision != 0);
    CHECK(chain.map_policy(*world.conn).revision == policy.revision);
    std::weak_ptr<MapExtension> remaining = healthy;
    healthy.reset();
    chain.remove(member);
    CHECK(remaining.expired());
    CHECK(chain.empty());
    CHECK(chain.map_policy(*world.conn).revision == 0);
}

TEST_CASE("server addon maps: generic source lifetime excludes map-only content") {
    mq_test::World world("addon_separate_source_lifetime");
    game::ExtensionChain chain;
    CHECK(chain.lifecycle_revision(*world.conn) == 0);
    auto provider = std::make_shared<MapExtension>();
    const auto member = chain.add(provider);
    const auto lifecycle = chain.lifecycle_revision(*world.conn);
    const auto policy = chain.map_policy(*world.conn).revision;
    CHECK(lifecycle != 0);
    provider->policy = {game::MapPolicyDisableNpcFind, "New map content", 7};
    CHECK(chain.map_policy(*world.conn).revision != policy);
    CHECK(chain.lifecycle_revision(*world.conn) == lifecycle);
    ++provider->lifecycle;
    const auto replaced = chain.lifecycle_revision(*world.conn);
    CHECK(replaced != lifecycle);
    CHECK(chain.lifecycle_revision(*world.conn) == replaced);
    chain.remove(member);
    chain.add(std::make_shared<MapExtension>()); // membership alone distinguishes a same-shaped source
    CHECK(chain.lifecycle_revision(*world.conn) != replaced);
}

TEST_CASE("server addon maps: Find walk identity survives neither policy loss, actor replacement nor connection changes") {
    auto policy = std::make_shared<MapExtension>(); policy->find_visible = true;
    net::ConnectionOptions options; options.plugins = policy;
    mq_test::World w("addon_find_walk_identity", options);
    const net::GameConnection* current = w.conn.get();
    auto guard = app::find_walk_eligibility([&] { return current; }, *w.conn, 10);
    REQUIRE(guard());
    policy->policy.flags = game::MapPolicyDisableNpcFind; CHECK_FALSE(guard());
    policy->policy.flags = 0; CHECK_FALSE(guard()); // restoration cannot revive an observation from the old policy
    guard = app::find_walk_eligibility([&] { return current; }, *w.conn, 10); REQUIRE(guard());
    ++net::GameConnectionTestAccess::world(*w.conn).spawns.at(10).incarnation;
    CHECK_FALSE(guard());
    guard = app::find_walk_eligibility([&] { return current; }, *w.conn, 10); REQUIRE(guard());
    current = nullptr; CHECK_FALSE(guard());
    current = w.conn.get(); REQUIRE(guard());
    ++net::GameConnectionTestAccess::world(*w.conn).spawns.at(1).incarnation; CHECK_FALSE(guard());
    CHECK_FALSE(app::find_walk_eligibility([&] { return current; }, *w.conn, 12)()); // player, not NPC
}

TEST_CASE("server addon maps: restricted NPC Find requests retire without cancelling PC or coordinate paths") {
    auto policy = std::make_shared<MapExtension>(); policy->find_visible = true;
    auto options = test::quick_options(); options.plugins = policy;
    test::Rig w(test::MockConfig::standard(), options); REQUIRE(w.to_zone());
    auto& c = w.conn; path_actors(c);
    REQUIRE(c.find_spawn(10));
    c.receive_zone_packet_for_test(net::encode_find_person_reply({{100, 0, 0}, {0, 0, 0}, {100, 0, 0}}));
    REQUIRE(c.player_path().active());
    policy->policy.flags = game::MapPolicyDisableNpcFind;
    CHECK_FALSE(c.find_spawn(10)); CHECK_FALSE(app::guide_spawn_position(c, 10));
    c.find_person_request(); CHECK_FALSE(c.player_path().active());
    c.find_target("a_rat00"); CHECK_FALSE(c.player_path().active());
    CHECK(c.find_spawn(12));
    c.receive_zone_packet_for_test(net::encode_find_person_reply({{100, 0, 0}, {0, 0, 0}, {100, 0, 0}}));
    CHECK(c.player_path().active()); CHECK(c.player_path().target_id() == 12);
    c.find_map_location({500, 500, 0});
    c.receive_zone_packet_for_test(net::encode_find_person_reply({{500, 500, 0}, {0, 0, 0}, {500, 500, 0}}));
    CHECK(c.player_path().active());
    CHECK(c.player_path().kind() == game::FindKind::MapLocation);
    CHECK_FALSE(c.find_spawn(13)); // corpse remains ineligible even if it belongs to a PC
}

TEST_CASE("Find current target: a no-argument slash command reaches the app's local guide before stock Find") {
    auto policy = std::make_shared<MapExtension>();
    policy->policy.flags = game::MapPolicyServerNpcFindOnly | game::MapPolicyActorGuides;
    policy->find_visible = true;
    auto options = test::quick_options();
    options.plugins = policy;
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    auto& connection = rig.conn;
    path_actors(connection);
    connection.set_target(10);
    REQUIRE_FALSE(connection.player_is_gm());
    int guides = 0;
    connection.set_client_commands([&](std::string_view handler, const std::string& args) {
        if (handler != "Cmd_find" || !args.empty()) return false;
        return app::current_target_guide([&] { return &connection; }, [&](std::uint32_t id, std::function<bool()> current) {
            CHECK(id == 10);
            CHECK(current());
            ++guides;
            return true;
        });
    });
    connection.say("/FIND \t");
    CHECK(guides == 1);
    CHECK(net::GameConnectionTestAccess::find_request_count(connection) == 0);
    net::FindableNpc advertised;
    advertised.spawn_id = 10;
    connection.receive_zone_packet_for_test(net::encode_findable_npc(advertised));
    connection.say("/find a_rat01");
    CHECK(guides == 1);
    CHECK(net::GameConnectionTestAccess::find_request_count(connection) == 0); // named Find is a GM command
    auto& world = net::GameConnectionTestAccess::world(connection);
    world.spawns.at(world.player_id).gm = true;
    connection.say("/find a_rat01");
    CHECK(guides == 1);
    CHECK(net::GameConnectionTestAccess::find_request_count(connection) == 1);
}

TEST_CASE("server addon maps: NPC-origin paths and guide identities cannot rebind after despawn or id reuse") {
    auto policy = std::make_shared<MapExtension>(); policy->find_visible = true;
    auto options = test::quick_options(); options.plugins = policy;
    test::Rig w(test::MockConfig::standard(), options); REQUIRE(w.to_zone());
    auto& c = w.conn; path_actors(c);
    auto& world = net::GameConnectionTestAccess::world(c);
    const auto original = world.spawns.at(10);
    REQUIRE(c.find_spawn(10)); world.spawns.erase(10);
    c.find_person_request(); CHECK_FALSE(c.player_path().active());
    world.spawns[10] = original;
    REQUIRE(c.find_spawn(10));
    world.spawns[10].is_npc = false; ++world.spawns[10].incarnation;
    c.find_person_request(); CHECK_FALSE(c.player_path().active());
    world.spawns[10] = original;
    app::GuidePath guide;
    guide.start(10, "Original", app::find_walk_eligibility([&] { return &c; }, c, 10));
    REQUIRE(guide.eligible());
    ++world.spawns[10].incarnation;
    CHECK_FALSE(guide.eligible());
    CHECK(guide.update({}, std::nullopt, 1, {}).gone); CHECK_FALSE(guide.active());
    REQUIRE(c.find_spawn(12));
    world.spawns.erase(12);
    CHECK(c.player_path().target_id() == 12); // original PC Find retains its existing server-owned target semantics
}

// Replies are matched in the order asked (docs/navigation.md): a retired NPC Find's reply takes its place and is dropped,
// and the replacement PC or coordinate Find keeps its own.
TEST_CASE("server addon maps: a retired NPC Find's reply is dropped and its replacement keeps its own") {
    for (const bool coordinate : {false, true}) {
        auto policy = std::make_shared<MapExtension>(); policy->find_visible = true;
        auto options = test::quick_options(); options.plugins = policy;
        test::Rig w(test::MockConfig::standard(), options); REQUIRE(w.to_zone());
        auto& c = w.conn; path_actors(c);
        REQUIRE(c.find_spawn(10));
        policy->policy.flags = game::MapPolicyDisableNpcFind;
        if (coordinate) c.find_map_location({500, 500, 0});
        else REQUIRE(c.find_spawn(12));
        const auto before = c.find_replies();
        const auto old = net::encode_find_person_reply({{100, 0, 0}, {0, 0, 0}, {100, 0, 0}});
        const auto replacement = net::encode_find_person_reply({{500, 500, 0}, {0, 0, 0}, {500, 500, 0}});
        c.receive_zone_packet_for_test(old);
        CHECK(c.find_replies() == before); CHECK_FALSE(c.player_path().active()); CHECK(c.find_path().empty());
        c.receive_zone_packet_for_test(replacement);
        CHECK(c.find_replies() == before + 1); CHECK(c.player_path().active());
        REQUIRE(c.find_spawn(12));
        c.receive_zone_packet_for_test(replacement);
        CHECK(c.find_replies() == before + 2); CHECK(c.player_path().active());
    }
}

// Main's rule kept with a map provider present: an unanswered NPC Find is dropped after the wait and the next Find is
// answered; nothing stays locked until the next zone.
TEST_CASE("server addon maps: an unanswered NPC Find expires and the next Find gets its reply") {
    auto policy = std::make_shared<MapExtension>(); policy->find_visible = true;
    auto options = test::quick_options(); options.plugins = policy;
    test::Rig w(test::MockConfig::standard(), options); REQUIRE(w.to_zone());
    auto& c = w.conn; path_actors(c);
    REQUIRE(c.find_spawn(10));
    net::GameConnectionTestAccess::expire_find_batch(c);
    const auto before = c.find_replies();
    const auto reply = net::encode_find_person_reply({{100, 0, 0}, {0, 0, 0}, {100, 0, 0}});
    c.receive_zone_packet_for_test(reply); // the old request's, late: no request owns it
    CHECK(c.find_replies() == before);
    REQUIRE(c.find_spawn(12)); c.receive_zone_packet_for_test(reply);
    CHECK(c.find_replies() == before + 1); CHECK(c.player_path().active());
}

TEST_CASE("server addon maps: a retired NPC request cannot revive when eligibility returns before its reply") {
    for (const bool explicit_stop : {false, true}) {
        auto policy = std::make_shared<MapExtension>(); policy->find_visible = true;
        auto options = test::quick_options(); options.plugins = policy;
        test::Rig w(test::MockConfig::standard(), options); REQUIRE(w.to_zone());
        auto& c = w.conn; path_actors(c);
        REQUIRE(c.find_spawn(10));
        policy->policy.flags = game::MapPolicyDisableNpcFind;
        if (explicit_stop) c.stop_find_path(); else c.find_person_request();
        policy->policy.flags = 0;
        const auto before = c.find_replies();
        c.receive_zone_packet_for_test(net::encode_find_person_reply({{100, 0, 0}, {0, 0, 0}, {100, 0, 0}}));
        CHECK(c.find_replies() == before); CHECK_FALSE(c.player_path().active());
    }
}

TEST_CASE("server addon maps: UI policy and native Find gates clear cached discovery and actions immediately") {
    auto extension = std::make_shared<MapExtension>(); extension->find_visible = true;
    net::ConnectionOptions options; options.plugins = extension;
    mq_test::World w("addon_find_ui_policy", options);
    auto& c = *w.conn;
    net::GameConnectionTestAccess::world(c).spawns.at(10).class_id = 41;
    app::UiHost host; host.in_game = host.ui_ready = true; host.conn = &c;
    unsigned walks = 0, guides = 0;
    host.walk_to = [&](auto) { ++walks; return std::string{}; };
    host.guide_to = [&](auto) { ++guides; return true; };
    app::UiApi api(host);
    auto* lua = luaL_newstate(); luaL_openlibs(lua);
    lua_newtable(lua); api.register_functions(lua); lua_setglobal(lua, "hs");
    const auto run = [&](const char* code) {
        REQUIRE_MESSAGE(luaL_dostring(lua, code) == LUA_OK, lua_tostring(lua, -1));
        const std::string result = lua_tostring(lua, -1); lua_pop(lua, 1); return result;
    };
    CHECK(run("return tostring(#hs.UI.find.Npcs()>0)") == "true");
    net::GameConnectionTestAccess::enable_quest_discovery(c);
    extension->policy = {game::MapPolicyCustomSpawns | game::MapPolicyHideStockNpcs | game::MapPolicyDisableNpcFind, "Owned map"};
    CHECK(run("local p=hs.UI.map.Policy(); return tostring(p.custom_spawns)..'|'..tostring(p.stock_npcs)..'|'..tostring(p.npc_find)..'|'..p.name") ==
          "true|false|false|Owned map");
    CHECK(run("return tostring(#hs.UI.find.Npcs())") == "0");
    c.request_quest_npcs(); CHECK_FALSE(net::GameConnectionTestAccess::quest_discovery_requested(c));
    c.set_target(12);
    CHECK(run("return tostring(hs.UI.find.NpcTarget(10))") == "false"); CHECK(c.target() == 12);
    CHECK(run("return tostring(hs.UI.find.NpcPath(10))") == "false");
    CHECK(run("return tostring(hs.UI.find.NpcWalk(10))") == "false"); CHECK(walks == 0); CHECK(guides == 0);
    CHECK(run("return tostring(hs.UI.find.Open(true))") == "false");
    extension->policy = {};
    CHECK(run("local p=hs.UI.map.Policy(); return tostring(p.stock_npcs)..'|'..tostring(p.npc_find)") == "true|true");
    CHECK(run("return tostring(#hs.UI.find.Npcs()>0)") == "true");
    CHECK(net::GameConnectionTestAccess::quest_discovery_requested(c));
    lua_close(lua);
}

TEST_CASE("server addon maps: a restriction applies before all providers regardless of load order") {
    mq_test::World w("addon_map_policy");
    game::ExtensionChain chain;
    auto showing = std::make_shared<MapExtension>();
    auto restricting = std::make_shared<MapExtension>(); restricting->visible = false;
    chain.add(showing); const int restriction = chain.add(restricting);
    const auto& npc = w.conn->world().spawns.at(10);
    CHECK_FALSE(chain.map_visible(*w.conn, npc, 0));
    CHECK_FALSE(chain.map_spawn(*w.conn, npc, {}));
    chain.remove(restriction);
    CHECK(chain.map_spawn(*w.conn, npc, {}));
    CHECK_FALSE(chain.map_visible(*w.conn, npc, 1));
    CHECK(game::ServerExtension{}.map_visible(*w.conn, npc, 1));
}

TEST_CASE("server addon maps: a running guide retires when Find eligibility changes and vanilla remains available") {
    auto chain = std::make_shared<game::ExtensionChain>();
    auto policy = std::make_shared<MapExtension>();
    policy->find_visible = true;
    const auto owner = chain->add(policy);
    net::ConnectionOptions options; options.plugins = chain;
    mq_test::World w("addon_find_guide_policy", options);
    auto& c = *w.conn;
    auto& destination = net::GameConnectionTestAccess::world(c).spawns.at(10);
    destination.position = {100, 0, 0}; destination.display_valid = false;
    app::GuidePath guide;
    unsigned plans = 0;
    const app::GuidePath::PathFn planner = [&](const glm::vec3& from, const glm::vec3& to) {
        ++plans;
        return std::optional<std::vector<glm::vec3>>{{from, to}};
    };
    guide.start(10, "Destination");
    const auto initial = guide.update({}, app::guide_spawn_position(c, 10), 0, planner);
    CHECK_FALSE(initial.gone);
    REQUIRE(guide.active());
    REQUIRE_FALSE(guide.line().empty());
    CHECK(guide.line().back().x == 100);
    CHECK(plans == 1);
    policy->find_visible = false;
    destination.position.x = 200;
    const auto denied = guide.update({}, app::guide_spawn_position(c, 10), 1, planner);
    CHECK(denied.gone);
    CHECK_FALSE(guide.active());
    CHECK(guide.line().empty());
    CHECK(plans == 1); // the newly hidden live position never reaches the path planner
    chain->remove(owner);
    const auto vanilla = app::guide_spawn_position(c, 10);
    REQUIRE(vanilla);
    CHECK(vanilla->x == 200);
    guide.start(10, "Destination");
    CHECK_FALSE(guide.update({}, vanilla, 2, planner).gone);
    CHECK(guide.active());
    CHECK(plans == 2);
}

TEST_CASE("server addon merchant: only an eligible carried item exposes a direct sale context") {
    auto addon = std::make_shared<SaleExtension>();
    net::ConnectionOptions options; options.plugins = addon;
    mq_test::World w("addon_merchant_context", options);
    auto& c = *w.conn;
    auto& session = net::GameConnectionTestAccess::merchant(c);
    session.emplace(); session->open = true; session->npc_id = 10;
    net::GameConnectionTestAccess::world(c).spawns.at(10).position = {5, 0, 0};
    const auto loose = net::ItemSlot::possessions(net::kSlotGeneral1);
    const auto bagged = net::ItemSlot::possessions(net::kSlotGeneral1 + 1, 0);
    auto& inventory = net::GameConnectionTestAccess::inventory(c);
    auto item = *inventory.at(loose); item.no_drop = 1; inventory.put(loose, item);
    item = *inventory.at(bagged); item.no_drop = 1; inventory.put(bagged, item);
    c.right_click_item(loose);
    REQUIRE(addon->calls == 1); CHECK(addon->item == 5001); CHECK(addon->count == 5);
    c.right_click_item(bagged);
    REQUIRE(addon->calls == 2); CHECK(addon->clicked.same(bagged)); CHECK(addon->count == 2);
    c.right_click_item(net::ItemSlot::possessions(net::kSlotGeneral1 + 1)); // container
    CHECK(addon->calls == 2);
    inventory.put(net::ItemSlot::possessions(0), item);
    c.right_click_item(net::ItemSlot::possessions(0)); // worn items use the same native sell protocol
    REQUIRE(addon->calls == 3); CHECK(addon->clicked.slot == 0);
    inventory.put(net::ItemSlot::possessions(net::kSlotCursor), item);
    c.right_click_item(loose); CHECK(addon->calls == 4); CHECK(addon->cursor_occupied);
    c.right_click_item(net::ItemSlot::possessions(net::kSlotCursor)); CHECK(addon->calls == 4);
    c.right_click_item(net::ItemSlot::possessions(34)); CHECK(addon->calls == 4);
    c.right_click_item(net::ItemSlot::possessions(0, 0)); CHECK(addon->calls == 4);
    inventory.take(net::ItemSlot::possessions(net::kSlotCursor));
    net::GameConnectionTestAccess::world(c).spawns.at(10).position.x = 21;
    c.right_click_item(loose); CHECK(addon->calls == 4);
    net::GameConnectionTestAccess::world(c).spawns.at(10).position.x = 5;
    session->trader = true;
    c.right_click_item(loose); CHECK(addon->calls == 4);
}

TEST_CASE("server addon observations: replacement actors retire received resource percentages") {
    mq_test::World w("addon_observation_identity");
    auto& c = *w.conn;
    auto& group = net::GameConnectionTestAccess::group(c);
    group.mana_percent[10] = 20; group.endurance_percent[10] = 30;
    net::Spawn replacement;
    replacement.spawn_id = 10; replacement.name = "Replacement"; replacement.hp_percent = 80;
    net::GameConnectionTestAccess::apply_spawn(c, replacement);
    CHECK_FALSE(group.mana_percent.contains(10)); CHECK_FALSE(group.endurance_percent.contains(10));
    CHECK(c.world().spawns.at(10).hp_known);
}
