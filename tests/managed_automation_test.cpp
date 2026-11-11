#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include <lua.hpp>

#include "app/lua_eq.h"
#include "app/lua_eq_plugins.h"
#include "app/lua_eq_windows.h"
#include "app/navigator.h"
#include "core/distribution.h"
#include "core/game_dir.h"
#include "game/slash_commands.h"
#include "mq/commands.h"
#include "mq/nav_commands.h"
#include "plugin/hooks.h"
#include "net/game_connection.h"
#include "scratch_dir.h"

namespace hailstone::app {
void register_ability_book_api(lua_State*, EqHost&);
void register_luasocial_api(lua_State*, EqHost&);
void register_inventory_manager_api(lua_State*, EqHost&);
void add_ability_book_window(std::vector<std::unique_ptr<StockWindow>>&);
} // namespace hailstone::app

namespace hailstone::net {
struct GameConnectionTestAccess {
    static void ready(GameConnection& c) {
        c.state_ = ConnectionState::InZone;
        c.world_.player_id = 1;
    }
    static game::Inventory& inventory(GameConnection& c) { return c.inventory_; }
    static MerchantSession& merchant(GameConnection& c) {
        c.merchant_.emplace();
        c.merchant_->open = true;
        c.merchant_->tabs = 4;
        c.merchant_->npc_id = 2;
        return *c.merchant_;
    }
    static void seed_item_job(GameConnection& c, GameConnection::ItemJobMode mode, const ItemSlot& at) {
        GameConnection::ItemJob job;
        job.status.mode = mode;
        job.clicks.push_back({at, 0});
        c.item_job_ = std::move(job);
    }
    static void update_item_job(GameConnection& c) { c.update_item_job(); }
    static void update_bulk_job(GameConnection& c) { c.update_bulk_job(0); }
    static void next_parcel_claim(GameConnection& c) { c.next_parcel_claim(); }
};
} // namespace hailstone::net

using namespace hailstone;

TEST_CASE("managed automation: MQ additions leave stock commands and chat literal") {
    REQUIRE(distribution::managed());
    plugin::Hooks hooks;
    mq::Context context;
    mq::CommandEnv env;
    env.hooks = &hooks;
    env.context = [&]() -> const mq::Context& { return context; };
    mq::Commands commands(env);

    for (const char* word : {"/echo", "/noparse", "/docommand", "/squelch", "/face", "/keypress", "/nav", "/moveto", "/stick"})
        CHECK_FALSE(hooks.has_command(word));
    // The RoF2 interpreter owns these; MQ must not replace /target with a spawn-search implementation.
    for (const char* word : {"/target", "/melody", "/autofire", "/follow", "/cast", "/stopsocial"}) {
        CHECK(game::find_slash_command(word, false).has_value());
        CHECK_FALSE(hooks.has_command(word));
        std::string line = std::string(word) + " original arguments";
        const std::string before = line;
        CHECK_FALSE(hooks.command(line));
        CHECK(line == before);
    }
    std::string chat = "/say ${Math.Calc[1+1]}";
    CHECK_FALSE(hooks.command(chat));
    CHECK(chat == "/say ${Math.Calc[1+1]}");
    std::string pause = "/pause 5, /cast 1";
    CHECK_FALSE(hooks.command(pause));
    CHECK(pause == "/pause 5, /cast 1");

    const auto owner = hooks.new_owner("direct navigation registration");
    mq::add_nav_commands(hooks, owner, env, [](const std::string&) {});
    CHECK_FALSE(hooks.has_command("/nav"));
    CHECK_FALSE(hooks.has_command("/moveto"));
    CHECK_FALSE(hooks.has_command("/stick"));
}

TEST_CASE("managed automation: direct navigation and mesh follow cannot activate") {
    REQUIRE(distribution::managed());
    unsigned lookups = 0, starts = 0, path_requests = 0;
    app::Navigator::Env env;
    env.spawn = [&](std::uint32_t) -> const game::Spawn* { ++lookups; return nullptr; };
    env.door = [&](std::uint8_t) -> const game::Door* { ++lookups; return nullptr; };
    env.started = [&] { ++starts; };
    env.request_server_path = [&](std::uint32_t) { ++path_requests; };
    env.assist_follow = true;
    app::Navigator nav(std::move(env));
    CHECK(nav.go_to_point({100.0f, 0.0f, 0.0f}, 2.0f, "goal", false).find("disabled") != std::string::npos);
    CHECK(nav.go_to_spawn(1, 5.0f).find("disabled") != std::string::npos);
    CHECK(nav.go_to_door(1, true).find("disabled") != std::string::npos);
    CHECK(nav.stick(1, {}).find("disabled") != std::string::npos);
    nav.options().assist_follow = true; // an editable setting cannot reopen it
    CHECK_FALSE(nav.follow_assist_on());
    const auto help = nav.assist_follow({}, {100.0f, 0.0f, 0.0f}, false, 1.0);
    CHECK_FALSE(help.detour);
    CHECK_FALSE(help.steer.active);
    CHECK_FALSE(nav.frame({}).active);
    CHECK(lookups == 0);
    CHECK(starts == 0);
    CHECK(path_requests == 0);
}

TEST_CASE("managed automation: script and Lua social UI calls cannot execute user code") {
    REQUIRE(distribution::managed());
    std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), &lua_close);
    REQUIRE(state);
    lua_State* L = state.get();
    luaL_openlibs(L);
    app::EqHost host;
    lua_newtable(L);
    app::register_scripts_api(L, host);
    app::register_luasocial_api(L, host);
    app::register_ability_book_api(L, host);
    app::register_inventory_manager_api(L, host);
    lua_setglobal(L, "eq");
    const char* code = R"lua(
        local ok, why = eq.ScriptRun('unapproved')
        assert(ok == false and why:find('disabled'))
        assert(eq.SetScriptAutorun('unapproved', true) == false)
        assert(eq.PressLuaSocial(0) == false)
        local dry = eq.LuaSocialDryRun(0, {press = "error('must not run')"})
        assert(dry.ok == false and dry.mode == 'off')
        assert(eq.LuaSocialFace(0) == nil)
        eq.OpenLuaSocialEditor(0)
        assert(eq.LuaSocialEditRequest() == nil)
        eq.SetLuaSocial(0, {name='unapproved', press="error('must not run')"})
        eq.AbilityBookAutoskillEnable(true)
        assert(eq.AbilityBookState().autoskill.allowed == false)
        assert(eq.AbilityBookState().autoskill.on == false)
        assert(type(eq.AbilityBookUse) == 'function') -- manual ability use stays exposed
        local count, err = eq.BulkItems('destroy', {'i:23'}, true)
        assert(count == 0 and err:find('disabled'))
        count, err = eq.BulkItems('sell', {'i:23'})
        assert(count == 0 and err:find('disabled'))
    )lua";
    const int status = luaL_dostring(L, code);
    INFO(status == LUA_OK ? "" : lua_tostring(L, -1));
    REQUIRE(status == LUA_OK);

    std::vector<std::unique_ptr<app::StockWindow>> windows;
    app::add_ability_book_window(windows);
    REQUIRE(windows.size() == 1);
    CHECK(windows.front()->click(host, "AutoskillOn", "left", false));
    windows.front()->frame(host);
    CHECK(luaL_dostring(L, "assert(not eq.AbilityBookState().autoskill.on)") == LUA_OK);
    CHECK(host.lua_social_edit == -1);
}

TEST_CASE("managed automation: custom inventory jobs stop while individual item and parcel actions work") {
    REQUIRE(distribution::managed());
    ScratchDir scratch("managed_inventory_jobs");
    GameDir game(scratch.path());
    net::GameConnection conn(game);
    using Access = net::GameConnectionTestAccess;
    using Mode = net::GameConnection::ItemJobMode;
    using Scope = net::GameConnection::ItemJobScope;
    Access::ready(conn);
    const auto from = net::ItemSlot::possessions(net::kSlotGeneral1);
    const auto to = net::ItemSlot::possessions(net::kSlotGeneral1 + 1);
    net::Item item;
    item.id = 12345;
    item.name = "Test item";
    Access::inventory(conn).put(from, item);

    for (Mode mode : {Mode::Sort, Mode::Stack, Mode::Tidy})
        CHECK(conn.start_item_job(Scope::Inventory, {}, mode).find("disabled") != std::string::npos);
    CHECK(conn.start_sell_job({item.id}).find("disabled") != std::string::npos);
    for (Mode mode : {Mode::Sell, Mode::Tribute, Mode::Destroy}) {
        CHECK(conn.bulk_items_blocked(mode).find("disabled") != std::string::npos);
        CHECK(conn.start_bulk_job(mode, {from}).find("disabled") != std::string::npos);
    }
    CHECK_FALSE(conn.item_job());
    Access::seed_item_job(conn, Mode::Sort, from);
    Access::update_item_job(conn);
    CHECK_FALSE(conn.item_job());
    REQUIRE(conn.inventory().at(from));
    CHECK(conn.inventory().at(from)->id == item.id);
    CHECK_FALSE(conn.inventory().cursor());

    // The managed rule stops queued work without moving the player's cursor item as cleanup.
    conn.click_slot(from, true, false);
    REQUIRE(conn.inventory().cursor());
    Access::seed_item_job(conn, Mode::Destroy, from);
    Access::update_bulk_job(conn);
    CHECK_FALSE(conn.item_job());
    REQUIRE(conn.inventory().cursor());
    CHECK(conn.inventory().cursor()->id == item.id);
    conn.click_slot(to, true, false);
    CHECK_FALSE(conn.inventory().cursor());
    REQUIRE(conn.inventory().at(to));
    CHECK(conn.inventory().at(to)->id == item.id);

    auto& merchant = Access::merchant(conn);
    merchant.parcels[7].item = item;
    conn.claim_all_parcels();
    CHECK(merchant.claiming.empty());
    CHECK_FALSE(merchant.parcel_pending);
    merchant.claiming.push_back(7); // a stale queued continuation cannot claim anything either
    Access::next_parcel_claim(conn);
    CHECK(merchant.claiming.empty());
    CHECK_FALSE(merchant.parcel_pending);
    conn.retrieve_parcel(7);
    CHECK(merchant.parcel_pending); // the ordinary single-parcel request remains available
}
