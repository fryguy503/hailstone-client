#include <doctest/doctest.h>
#include <lua.hpp>

#include <memory>
#include <functional>
#include <stdexcept>
#include <string>

#include "app/lua_ui.h"
#include "core/process_serial.h"
#include "game/server_ext_chain.h"
#include "mq_test_world.h"
#include "net/movement_packets.h"

using namespace hailstone;
namespace {
struct FindProvider final : game::ServerExtension {
    bool visible = true;
    game::MapPolicy policy{0, "Shared provider"};
    mutable int queries = 0;
    std::function<void(int)> on_query;
    bool throw_query = false;
    game::MapPolicy map_policy(const net::GameConnection&) const override {
        if (throw_query) throw std::runtime_error("policy fixture exception");
        const int query = ++queries;
        if (on_query) on_query(query);
        return policy;
    }
    bool map_visible(const net::GameConnection&, const game::Spawn&, std::uint32_t purpose) const override {
        return purpose != 1 || visible;
    }
};
struct FindUi {
    std::shared_ptr<game::ExtensionChain> chain = std::make_shared<game::ExtensionChain>();
    std::shared_ptr<FindProvider> provider = std::make_shared<FindProvider>();
    net::ConnectionOptions options;
    std::unique_ptr<mq_test::World> world;
    app::UiHost host;
    std::unique_ptr<app::UiApi> api;
    lua_State* state = nullptr;
    int provider_id = 0, paths = 0, walks = 0;

    FindUi() {
        provider_id = chain->add(provider);
        options.plugins = chain;
        world = std::make_unique<mq_test::World>("find_identity", options);
        auto& actors = net::GameConnectionTestAccess::world(*world->conn).spawns;
        actors.at(10).class_id = 41;
        host.in_game = host.ui_ready = true;
        host.zone_short_name = "qeynos2";
        host.conn = world->conn.get();
        host.guide_to = [&](std::uint32_t id) { CHECK(id == 10); ++paths; return true; };
        host.walk_to = [&](std::uint32_t id) { CHECK(id == 10); ++walks; return std::string{}; };
        api = std::make_unique<app::UiApi>(host);
        state = luaL_newstate();
        if (!state) throw std::bad_alloc();
        luaL_openlibs(state);
        lua_newtable(state); api->register_functions(state); lua_setglobal(state, "hs");
    }
    ~FindUi() { lua_close(state); }
    std::string run(const char* source) {
        const int result = luaL_dostring(state, source);
        if (result != LUA_OK) {
            const char* text = lua_tostring(state, -1);
            const std::string why = text ? text : "non-string error";
            lua_settop(state, 0); return "error: " + why;
        }
        // Action APIs may return a result plus an explanation; assertions inspect the result.
        const char* text = luaL_tolstring(state, 1, nullptr);
        const std::string out = text ? text : "nil";
        lua_settop(state, 0); return out;
    }
};
}

TEST_CASE("Find identity: observed actions reject reused actors while current and legacy intent remain usable") {
    FindUi ui;
    REQUIRE(ui.run("row=hs.UI.find.Npcs()[1]; return row and type(row.token)=='string' and type(row.context)=='string' and type(row.incarnation)=='string'") == "true");
    CHECK(ui.run("return hs.UI.find.NpcTarget(row.id,row.token)") == "true");
    CHECK(ui.run("return hs.UI.find.NpcPath(row.id,row.token)") == "true");
    CHECK(ui.run("return hs.UI.find.NpcWalk(row.id,row.token)") == "true");
    REQUIRE(ui.paths == 1); REQUIRE(ui.walks == 1);

    auto& actor = net::GameConnectionTestAccess::world(*ui.world->conn).spawns.at(10);
    actor.incarnation = core::next_process_identity();
    actor.name = "Replacement_Merchant00";
    ++net::GameConnectionTestAccess::world(*ui.world->conn).spawn_set_serial;
    ui.world->conn->set_target(12);
    CHECK(ui.run("return hs.UI.find.NpcTarget(row.id,row.token)") == "false");
    CHECK(ui.run("return hs.UI.find.NpcPath(row.id,row.token)") == "false");
    CHECK(ui.run("return hs.UI.find.NpcWalk(row.id,row.token)") == "false");
    CHECK(ui.world->conn->target() == 12);
    CHECK(ui.paths == 1); CHECK(ui.walks == 1);
    CHECK(ui.run("fresh=hs.UI.find.Npcs()[1]; return fresh.token~=row.token and hs.UI.find.NpcTarget(fresh.id,fresh.token)") == "true");
    CHECK(ui.run("return hs.UI.find.NpcTarget(10)") == "true");
    CHECK(ui.run("return hs.UI.find.NpcPath(10,'')") == "false");
}

TEST_CASE("Find identity: provider replacement content and player context retire retained observations") {
    FindUi ui;
    REQUIRE(ui.run("row=hs.UI.find.Npcs()[1]; return row~=nil") == "true");
    ui.chain->remove(ui.provider_id);
    ui.provider_id = ui.chain->add(ui.provider); // identical display name and flags, new owner
    CHECK(ui.run("return hs.UI.find.NpcTarget(row.id,row.token)") == "false");
    REQUIRE(ui.run("row=hs.UI.find.Npcs()[1]; return hs.UI.find.NpcPath(row.id,row.token)") == "true");
    ++ui.provider->policy.revision;
    CHECK(ui.run("return hs.UI.find.NpcWalk(row.id,row.token)") == "false");
    REQUIRE(ui.run("row=hs.UI.find.Npcs()[1]; return hs.UI.find.NpcTarget(row.id,row.token)") == "true");
    auto& player = net::GameConnectionTestAccess::world(*ui.world->conn).spawns.at(1);
    player.incarnation = core::next_process_identity();
    CHECK(ui.run("return hs.UI.find.NpcTarget(row.id,row.token)") == "false");
    CHECK(ui.run("fresh=hs.UI.find.Npcs()[1]; return fresh.context~=row.context and hs.UI.find.NpcTarget(fresh.id,fresh.token)") == "true");
}

TEST_CASE("Find identity: live eligibility is checked independently of cached classification and tokens") {
    FindUi ui;
    REQUIRE(ui.run("row=hs.UI.find.Npcs()[1]; return row~=nil") == "true");
    ui.provider->visible = false; // no provider flags/revision or 200ms geometry deadline changes
    CHECK(ui.run("return #hs.UI.find.Npcs()") == "0");
    CHECK(ui.run("return hs.UI.find.NpcTarget(row.id,row.token)") == "false");
    CHECK(ui.run("return hs.UI.find.NpcPath(row.id,row.token)") == "false");
    CHECK(ui.run("return hs.UI.find.NpcWalk(row.id,row.token)") == "false");
    CHECK(ui.paths == 0); CHECK(ui.walks == 0);
    ui.provider->visible = true;
    CHECK(ui.run("fresh=hs.UI.find.Npcs()[1]; return hs.UI.find.NpcWalk(fresh.id,fresh.token)") == "true");
    ui.host.guide_to = [&](std::uint32_t) { ui.provider->visible = false; return false; };
    CHECK(ui.run("return hs.UI.find.NpcPath(fresh.id,fresh.token)") == "false");
    CHECK(ui.world->conn->find_path().empty());
}

TEST_CASE("Find policy: server-only rows and actions follow the server's findable list") {
    FindUi ui;
    auto& conn = *ui.world->conn;
    net::GameConnectionTestAccess::enable_quest_discovery(conn);
    ui.provider->policy.flags = game::MapPolicyServerNpcFindOnly | game::MapPolicyHideStockNpcs;

    CHECK(ui.run("local p=hs.UI.map.Policy(); return p.npc_find and p.npc_find_server_only and not p.stock_npcs") == "true");
    CHECK(ui.run("return hs.UI.find.Open(true)") == "true");
    CHECK(ui.run("return #hs.UI.find.Npcs()") == "0");
    CHECK_FALSE(net::GameConnectionTestAccess::quest_discovery_requested(conn));
    CHECK(ui.run("return hs.UI.find.NpcTarget(10)") == "false");
    CHECK(ui.run("return hs.UI.find.NpcPath(10)") == "false");

    net::FindableNpc npc;
    npc.spawn_id = 11;
    conn.receive_zone_packet_for_test(net::encode_findable_npc(npc));
    REQUIRE(ui.run("local rows=hs.UI.find.Npcs(); row=rows[1]; return #rows==1 and row.id==11") == "true");
    CHECK(ui.run("return hs.UI.find.NpcTarget(row.id,row.token)") == "true");
    CHECK(conn.target() == 11);
    CHECK(ui.run("return hs.UI.find.NpcPath(row.id,row.token)") == "true");
    CHECK(ui.run("return hs.UI.find.NpcWalk(row.id,row.token)") == "false");
    CHECK(ui.paths == 0);
    CHECK(ui.walks == 0);

    ui.provider->visible = false;
    CHECK(ui.run("return #hs.UI.find.Npcs()") == "0");
    CHECK(ui.run("return hs.UI.find.NpcTarget(row.id,row.token)") == "false");
    ui.provider->visible = true;

    npc.remove = true;
    conn.receive_zone_packet_for_test(net::encode_findable_npc(npc));
    npc.spawn_id = 10;
    npc.remove = false;
    conn.receive_zone_packet_for_test(net::encode_findable_npc(npc));
    // The list stays the same size and both spawns stay present, but the old row loses eligibility.
    CHECK(ui.run("local rows=hs.UI.find.Npcs(); return #rows==1 and rows[1].id==10") == "true");
    CHECK(ui.run("return hs.UI.find.NpcTarget(row.id,row.token)") == "false");
    CHECK(ui.run("return hs.UI.find.NpcPath(row.id,row.token)") == "false");

    ui.provider->policy.flags |= game::MapPolicyDisableNpcFind;
    CHECK(ui.run("return hs.UI.map.Policy().npc_find") == "false");
    CHECK(ui.run("return hs.UI.find.Open(true)") == "false");
    CHECK(ui.run("return #hs.UI.find.Npcs()") == "0");
    CHECK(ui.run("return hs.UI.find.NpcPath(10)") == "false");
}

TEST_CASE("Find identity: the final policy callback cannot replace the observed actor before activation") {
    FindUi ui;
    REQUIRE(ui.run("row=hs.UI.find.Npcs()[1]; return row~=nil") == "true");
    ui.world->conn->set_target(12);
    const int replace_at = ui.provider->queries + 4;
    bool replaced = false;
    ui.provider->on_query = [&](int query) {
        if (query != replace_at) return;
        auto& actor = net::GameConnectionTestAccess::world(*ui.world->conn).spawns.at(10);
        actor.incarnation = core::next_process_identity();
        replaced = true;
    };
    CHECK(ui.run("return hs.UI.find.NpcTarget(row.id,row.token)") == "false");
    REQUIRE(replaced);
    CHECK(ui.world->conn->target() == 12);
    ui.provider->on_query = {};
    CHECK(ui.run("fresh=hs.UI.find.Npcs()[1]; return hs.UI.find.NpcTarget(fresh.id,fresh.token)") == "true");
}

TEST_CASE("Find identity: provider exceptions become Lua errors without breaking later queries") {
    FindUi ui;
    ui.provider->throw_query = true;
    CHECK(ui.run("return (pcall(hs.UI.find.Open,true))") == "false");
    CHECK(ui.run("return (pcall(hs.UI.map.Policy))") == "false");
    CHECK(ui.run("return (pcall(hs.UI.find.Npcs))") == "false");
    ui.provider->throw_query = false;
    CHECK(ui.run("return hs.UI.find.Open(true)") == "true");
    CHECK(ui.run("return #hs.UI.find.Npcs()") == "1");
}

TEST_CASE("Map policy: an unchanged policy is the same table, made again when any of it changes") {
    FindUi ui;
    REQUIRE(ui.run("first=hs.MapPolicy(); return rawequal(first, hs.MapPolicy()) and first.name") == "Shared provider");
    // No garbage while it stays the same (the windows ask several times a frame).
    CHECK(ui.run("collectgarbage('stop') local b=collectgarbage('count') local p=hs.MapPolicy() "
                 "local a=collectgarbage('count') collectgarbage('restart') return rawequal(p, first) and a==b") == "true");
    ui.provider->policy.name = "Renamed";
    CHECK(ui.run("second=hs.MapPolicy(); return (not rawequal(second, first)) and first.name=='Shared provider' and second.name") == "Renamed");
    ui.provider->policy.flags = game::MapPolicyHideStockNpcs;
    CHECK(ui.run("local p=hs.MapPolicy(); return (not rawequal(p, second)) and p.stock_npcs") == "false");
    ui.host.conn = nullptr; // another context
    CHECK(ui.run("local p=hs.MapPolicy(); return (not rawequal(p, second)) and p.context") == "0");
}

TEST_CASE("Map policy: a caller that changes the kept table does not change what the next call returns") {
    FindUi ui;
    ui.provider->policy.flags = game::MapPolicyDisableNpcFind;
    REQUIRE(ui.run("first=hs.MapPolicy(); return tostring(first.npc_find)") == "false");
    // A write to a field: the next call is a new table with the policy's values, and the written one is left alone.
    CHECK(ui.run("first.npc_find=true; local p=hs.MapPolicy(); "
                 "return tostring(not rawequal(p, first))..'|'..tostring(p.npc_find)..'|'..tostring(first.npc_find)") ==
          "true|false|true");
    // A field added, or one removed: made again too, with every field as the policy has it.
    CHECK(ui.run("second=hs.MapPolicy(); second.extra=1; local p=hs.MapPolicy(); "
                 "return tostring(not rawequal(p, second))..'|'..tostring(p.extra)..'|'..p.name") ==
          "true|nil|Shared provider");
    CHECK(ui.run("third=hs.MapPolicy(); third.name=nil; local p=hs.MapPolicy(); "
                 "return tostring(not rawequal(p, third))..'|'..p.name") == "true|Shared provider");
    // A metatable put on it (whose __index could answer for a removed field): made again.
    CHECK(ui.run("fourth=hs.MapPolicy(); setmetatable(fourth, {}); return tostring(not rawequal(hs.MapPolicy(), fourth))") ==
          "true");
    // Left alone, the new table is kept again, and checking it makes no garbage.
    CHECK(ui.run("local p=hs.MapPolicy(); collectgarbage('stop') local b=collectgarbage('count') local q=hs.MapPolicy() "
                 "local a=collectgarbage('count') collectgarbage('restart') return tostring(rawequal(p, q) and a==b)") ==
          "true");
}
