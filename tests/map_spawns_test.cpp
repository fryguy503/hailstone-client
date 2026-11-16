// A server's plugin puts spawns on the maps (game/server_ext.h map_spawn, app/lua_ui_map.cpp): the minimap, the zone
// map and the map window draw the marks it gives, and nothing more without it.
#include <doctest/doctest.h>

#include <array>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/guide_path.h"
#include "app/lua_ui.h"
#include "game/server_ext.h"
#include "game/server_ext_chain.h"
#include "mq_test_world.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {

struct ShowRat final : game::ServerExtension {
    bool on = true;
    mutable int rat_asked = 0;
    std::optional<game::MapSpawnMark> map_spawn(const net::GameConnection&, const game::Spawn& s,
                                                const game::MapSpawnMark& defaults) const override {
        if (s.id == 10) ++rat_asked;
        if (!on || s.id != 10) return std::nullopt;
        CHECK(defaults.label == "a rat"); // the display name
        return game::MapSpawnMark{0x123456, 0, "Rat!"};
    }
};

constexpr ImU32 kMark = IM_COL32(0x12, 0x34, 0x56, 0xff);

// Draws `code` (hs.* calls) in a window and counts the vertices in the mark's colour (or with `rgb`, in that colour
// at any alpha).
int marked_vertices(lua_State* L, const std::string& code, std::optional<ImU32> rgb = std::nullopt,
                    ImVec2* center = nullptr) {
    ImGui::GetIO().DisplaySize = ImVec2(800, 600);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(600, 500));
    ImGui::Begin("map");
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        FAIL(lua_tostring(L, -1));
        lua_settop(L, 0);
    }
    int n = 0;
    ImVec2 sum{};
    for (const ImDrawVert& v : ImGui::GetWindowDrawList()->VtxBuffer)
        if (rgb ? (v.col & 0x00ffffffu) == *rgb : v.col == kMark) {
            ++n;
            sum.x += v.pos.x; sum.y += v.pos.y;
        }
    if (center && n) *center = ImVec2(sum.x / static_cast<float>(n), sum.y / static_cast<float>(n));
    ImGui::End();
    ImGui::Render();
    return n;
}

struct MarkerFilter final : game::ServerExtension {
    bool deny_group = false, marks = false;
    mutable int self_queries = 0, group_queries = 0;
    std::uint64_t content = 1, life = 1;
    mutable unsigned policy_queries = 0, lifecycle_queries = 0;
    std::function<void(const game::Spawn&)> on_visible, on_mark;
    std::function<void()> on_policy, on_lifecycle;
    game::MapPolicy map_policy(const net::GameConnection&) const override {
        ++policy_queries;
        const auto callback = on_policy;
        if (callback) callback();
        return {0, "Map filter", content};
    }
    std::uint64_t lifecycle_revision(const net::GameConnection&) const override {
        ++lifecycle_queries;
        const auto callback = on_lifecycle;
        if (callback) callback();
        return life;
    }
    bool map_visible(const net::GameConnection&, const game::Spawn& sp, std::uint32_t purpose) const override {
        if (purpose != 0) return true; // independent Find admission
        if (sp.id == 1) ++self_queries;
        if (sp.id == 12) ++group_queries;
        const auto callback = on_visible;
        if (callback) callback(sp);
        return !(deny_group && sp.id == 12);
    }
    std::optional<game::MapSpawnMark> map_spawn(const net::GameConnection&, const game::Spawn& sp,
                                               const game::MapSpawnMark&) const override {
        const auto callback = on_mark;
        if (callback) callback(sp);
        return marks && sp.id == 10 ? std::optional{game::MapSpawnMark{0x123456, 0, "Rat!"}} : std::nullopt;
    }
};

constexpr ImU32 kGroupRgb = IM_COL32(74, 145, 210, 0), kSelfRgb = IM_COL32(216, 79, 57, 0);
const std::array<std::string, 3> kMapDraws = {
    "hs.UI.map.Draw(0, 0, 600, 500, true)",
    "hs.UI.map.DrawZone(0, 0, 600, 500, 1, 0, 0, true)",
    "hs.UI.map.DrawMinimap(300, 250, 100, 500)"};

struct MarkerRig {
    static net::ConnectionOptions options(std::shared_ptr<game::ServerExtension> extension) {
        net::ConnectionOptions out; out.plugins = std::move(extension); return out;
    }
    mq_test::World world;
    test::ScratchDir scratch{"map_marker_filter"};
    ImGuiContext* context = nullptr;
    app::UiHost host;
    std::unique_ptr<app::UiApi> api;
    lua_State* L = nullptr;
    explicit MarkerRig(std::shared_ptr<game::ServerExtension> extension = {})
        : world("map_marker_filter_world", options(std::move(extension))) {
        context = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().AddMousePosEvent(-1000, -1000);
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        host.in_game = host.ui_ready = host.feet_valid = true;
        host.game = world.game.get(); host.conn = world.conn.get(); host.zone_short_name = "qeynos2";
        host.maps_dir = scratch.path() / "maps";
        std::ofstream(scratch.path() / "UI.ini") << "[MapViewWnd]\nShowGroup=true\n"
            "GroupColor.red=74\nGroupColor.green=145\nGroupColor.blue=210\n"
            "MyColor.red=216\nMyColor.green=79\nMyColor.blue=57\n";
        api = std::make_unique<app::UiApi>(host);
        api->load_layout(scratch.path() / "defaults.ini", scratch.path() / "UI.ini", {});
        api->begin_frame();
        L = luaL_newstate(); luaL_openlibs(L); lua_newtable(L);
        api->register_functions(L); lua_setglobal(L, "hs");
    }
    ~MarkerRig() {
        lua_close(L); api.reset(); ImGui::DestroyContext(context);
    }
    int group(const std::string& draw, ImVec2* center = nullptr) {
        api->begin_frame(); return marked_vertices(L, draw, kGroupRgb, center);
    }
    std::string policy(const std::string& field) {
        const auto code = "return hs.UI.map.Policy()." + field;
        if (luaL_dostring(L, code.c_str()) != LUA_OK) {
            FAIL(lua_tostring(L, -1)); lua_settop(L, 0); return {};
        }
        const auto* text = lua_tostring(L, -1);
        std::string result = text ? text : "";
        lua_settop(L, 0); return result;
    }
};

} // namespace

TEST_CASE("Map spawns: a plugin's marks on the minimap, the zone map and the map window") {
    auto ext = std::make_shared<ShowRat>();
    net::ConnectionOptions opts;
    opts.plugins = ext;
    mq_test::World world("hailstone_map_spawns_test", opts);
    test::ScratchDir scratch("map_spawns_test");
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = world.game.get();
        host.conn = world.conn.get();
        host.zone_short_name = "qeynos2";
        host.maps_dir = scratch.path() / "maps";
        host.feet_valid = true; // the player at the origin; the rat (spawn 10) 50 away
        app::UiApi api(host);
        api.begin_frame();
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");

        CHECK(marked_vertices(L, "hs.DrawMinimap(300, 250, 100, 500)") > 0);
        CHECK(marked_vertices(L, "hs.DrawZoneMap(0, 0, 600, 500, 1, 0, 0, true)") > 0);
        CHECK(marked_vertices(L, "hs.DrawMap(0, 0, 600, 500)") > 0); // the map window
        // Outside the minimap's reach: not drawn.
        CHECK(marked_vertices(L, "hs.DrawMinimap(300, 250, 100, 20)") == 0);
        // ... nor is the plugin asked about it there; the zone map, a rectangle, still asks.
        ext->rat_asked = 0;
        marked_vertices(L, "hs.DrawMinimap(300, 250, 100, 20)");
        CHECK(ext->rat_asked == 0);
        marked_vertices(L, "hs.DrawMinimap(300, 250, 100, 500)");
        CHECK(ext->rat_asked == 1);
        marked_vertices(L, "hs.DrawZoneMap(0, 0, 600, 500, 1, 0, 0, true)");
        CHECK(ext->rat_asked == 2);
        ext->on = false;
        CHECK(marked_vertices(L, "hs.DrawMinimap(300, 250, 100, 500)") == 0);
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Map lines: the minimap draws the lines that cross its circle, however far their ends") {
    mq_test::World world("hailstone_map_lines_test");
    test::ScratchDir scratch("map_lines_test");
    std::filesystem::create_directories(scratch.path() / "maps");
    {
        // Through the player (at the origin), its ends 1000 away; and one 5000 away.
        std::ofstream f(scratch.path() / "maps" / "qeynos2.txt");
        f << "L -1000, 0, 0,  1000, 0, 0,  0, 255, 0\n";
        f << "L 5000, 5000, 0,  5100, 5000, 0,  255, 255, 0\n";
    }
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = world.game.get();
        host.conn = world.conn.get();
        host.zone_short_name = "qeynos2";
        host.global_maps_dir = scratch.path() / "maps"; // read by the map state (the server's comes with the ini)
        host.feet_valid = true;
        app::UiApi api(host);
        api.begin_frame();
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        constexpr ImU32 kNear = IM_COL32(0, 255, 0, 0), kFar = IM_COL32(255, 255, 0, 0);
        CHECK(marked_vertices(L, "hs.DrawMinimap(300, 250, 100, 50)", kNear) > 0);
        CHECK(marked_vertices(L, "hs.DrawMinimap(300, 250, 100, 50, true)", kNear) > 0); // turned with the heading
        CHECK(marked_vertices(L, "hs.DrawMinimap(300, 250, 100, 50)", kFar) == 0);
        CHECK(marked_vertices(L, "hs.DrawZoneMap(0, 0, 600, 500, 1)", kFar) > 0); // the whole zone: both
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Map spawns: group marker admission is deny-wins on every map and independent of self and Find") {
    auto chain = std::make_shared<game::ExtensionChain>();
    auto allow = std::make_shared<MarkerFilter>(), deny = std::make_shared<MarkerFilter>();
    bool active = true;
    chain->add(allow);
    const auto denied = chain->add(deny, [&](const auto&) { return active; });
    MarkerRig r(chain);
    for (std::size_t i = 0; i < kMapDraws.size(); ++i) {
        CAPTURE(i);
        const auto& draw = kMapDraws[i];
        deny->deny_group = false;
        CHECK(r.group(draw) > 0);
        deny->deny_group = true;
        CHECK(r.group(draw) == 0);
        // Neither a provider's MARKER denial nor a roster filter applies to the local player or NPC Find.
        CHECK(marked_vertices(r.L, draw, i == 0 ? kSelfRgb : IM_COL32(255, 255, 255, 0)) > 0);
        CHECK(r.world.conn->npc_find_allowed(10));
        CHECK(app::guide_spawn_position(*r.world.conn, 12)); // group PC navigation remains independently eligible
        CHECK(allow->self_queries == 0); CHECK(deny->self_queries == 0);
        active = false;
        CHECK(r.group(draw) > 0);
        active = true;
        CHECK(r.group(draw) == 0);
    }
    chain->remove(denied); // removal restores the remaining provider's stock allowance immediately
    for (const auto& draw : kMapDraws) CHECK(r.group(draw) > 0);
}

TEST_CASE("Map spawns: no provider retains stock group markers and a denied group has no hover text") {
    {
        MarkerRig stock;
        for (const auto& draw : kMapDraws) CHECK(stock.group(draw) > 0);
    }
    auto filter = std::make_shared<MarkerFilter>();
    MarkerRig r(filter);
    ImVec2 center;
    const auto ordinary = r.group(kMapDraws[0], &center);
    REQUIRE(ordinary > 0);
    ImGui::GetIO().AddMousePosEvent(center.x, center.y);
    CHECK(r.group(kMapDraws[0]) > ordinary); // the admitted group's name is painted in its own colour
    filter->deny_group = true;
    CHECK(r.group(kMapDraws[0]) == 0); // no X, highlight or name at the same mouse position
    filter->deny_group = false;
    CHECK(r.group(kMapDraws[0]) > ordinary);
}

TEST_CASE("Map spawns: reentrant roster, actor and player replacement cannot admit old group snapshots") {
    for (const auto& draw : kMapDraws) for (int change = 0; change < 3; ++change) {
        CAPTURE(draw);
        CAPTURE(change);
        auto filter = std::make_shared<MarkerFilter>();
        MarkerRig r(filter);
        REQUIRE(r.group(draw) > 0);
        bool fired = false;
        filter->on_visible = [&](const game::Spawn& sp) {
            if (sp.id != 12 || fired) return;
            fired = true;
            if (change == 0) { // roster removed during admission
                net::GameConnectionTestAccess::group(*r.world.conn).slots[0].name.clear();
            } else if (change == 1) { // same id and name are a new actor
                net::GameConnectionTestAccess::world(*r.world.conn).spawns.erase(12);
                r.world.add(12, "Groupie", false, 48, {0, 20, 0});
            } else { // player reincarnation changes the interaction context
                ++net::GameConnectionTestAccess::world(*r.world.conn).spawns.at(1).incarnation;
            }
            CHECK(sp.name == "Groupie"); // the callback's input remains owned even after erasure/rehash
        };
        CHECK(r.group(draw) == 0);
        CHECK(fired);
        filter->on_visible = {};
        net::GameConnectionTestAccess::group(*r.world.conn).slots[0].name = "Groupie";
        CHECK(r.group(draw) > 0); // a fresh observation of the replacement is valid
    }
}

TEST_CASE("Map spawns: later provider callbacks retire earlier actor snapshots and fail closed on exceptions") {
    for (const auto& draw : kMapDraws) {
        CAPTURE(draw);
        auto filter = std::make_shared<MarkerFilter>();
        filter->marks = true;
        MarkerRig r(filter);
        REQUIRE(r.group(draw) > 0);
        const auto final_query = filter->policy_queries;
        REQUIRE(final_query > 0);
        filter->policy_queries = 0;
        filter->on_policy = [&] {
            if (filter->policy_queries != final_query) return; // final source query, after both collections
            auto& world = net::GameConnectionTestAccess::world(*r.world.conn);
            world.spawns.erase(12);
            for (std::uint32_t id = 20; id < 100; ++id) r.world.add(id, "Other", true, 1, {0, 0, 0});
            r.world.add(12, "Groupie", false, 48, {0, 20, 0});
        };
        CHECK(r.group(draw) == 0);
        filter->on_policy = {};
        CHECK(r.group(draw) > 0);
        filter->on_visible = [](const auto&) { throw std::runtime_error("filter failed"); };
        CHECK(r.group(draw) == 0);
        CHECK(marked_vertices(r.L, draw) == 0); // no custom marker on a failed provider query
        filter->on_visible = {};
        CHECK(r.group(draw) > 0);
    }
}

TEST_CASE("Map spawns: source retirement during a callback rejects marks until a fresh draw") {
    auto filter = std::make_shared<MarkerFilter>();
    MarkerRig r(filter);
    bool retired = false;
    filter->on_visible = [&](const game::Spawn& sp) {
        if (sp.id == 12 && !retired) { retired = true; ++filter->life; }
    };
    CHECK(r.group(kMapDraws[0]) == 0);
    CHECK(retired);
    CHECK(r.group(kMapDraws[0]) > 0);
}

TEST_CASE("Map spawns: a custom marker callback cannot return a removed or reused actor") {
    for (const auto& draw : kMapDraws) {
        CAPTURE(draw);
        auto filter = std::make_shared<MarkerFilter>();
        filter->marks = true;
        MarkerRig r(filter);
        bool replaced = false;
        filter->on_mark = [&](const game::Spawn& sp) {
            if (sp.id != 10 || replaced) return;
            replaced = true;
            net::GameConnectionTestAccess::world(*r.world.conn).spawns.erase(10);
            for (std::uint32_t id = 20; id < 100; ++id) r.world.add(id, "Other", true, 1, {0, 0, 0});
            r.world.add(10, "a_rat01", true, 2, {30, 40, 0});
            CHECK(sp.name == "a_rat01");
        };
        CHECK(marked_vertices(r.L, draw) == 0);
        CHECK(replaced);
        filter->on_mark = {};
        CHECK(marked_vertices(r.L, draw) > 0);
        CHECK(r.group(draw) > 0);
    }
}

TEST_CASE("Map spawns: nested map drawing cannot recursively borrow the outer marker collection") {
    for (const auto& draw : kMapDraws) {
        CAPTURE(draw);
        auto filter = std::make_shared<MarkerFilter>();
        MarkerRig r(filter);
        REQUIRE(r.group(draw) > 0);
        const auto queries = filter->group_queries;
        filter->group_queries = 0;
        bool nested = false;
        filter->on_visible = [&](const game::Spawn& sp) {
            if (sp.id != 12 || nested) return;
            nested = true;
            CHECK(luaL_dostring(r.L, draw.c_str()) == LUA_OK);
        };
        CHECK(r.group(draw) > 0);
        CHECK(nested);
        CHECK(filter->group_queries == queries); // only the outer collection invokes the provider
    }
}

TEST_CASE("Map policy: source is an opaque lifecycle identity independent of map content") {
    MarkerRig r;
    auto& chain = static_cast<game::ExtensionChain&>(r.world.conn->server_ext());
    CHECK(r.policy("source") == "0");
    auto filter = std::make_shared<MarkerFilter>();
    const auto member = chain.add(filter);
    const auto source = r.policy("source"), revision = r.policy("revision"), context = r.policy("context");
    CHECK(source != "0");
    ++filter->content;
    CHECK(r.policy("source") == source);
    CHECK(r.policy("revision") != revision);
    CHECK(r.policy("context") == context);
    ++filter->life;
    const auto replacement = r.policy("source");
    CHECK(replacement != source);
    chain.remove(member); chain.add(std::make_shared<MarkerFilter>());
    CHECK(r.policy("source") != replacement);
    r.host.conn = nullptr;
    CHECK(r.policy("source") == "0"); CHECK(r.policy("context") == "0");
}

TEST_CASE("Map policy: a reentrant connection transition never labels the old policy with the new source") {
    auto filter = std::make_shared<MarkerFilter>();
    MarkerRig r(filter);
    mq_test::World replacement("map_policy_replacement");
    filter->lifecycle_queries = 0;
    filter->on_lifecycle = [&] {
        // ExtensionChain first reads provider lifecycle with the policy, then the API reads the source field.
        if (filter->lifecycle_queries == 2) r.host.conn = replacement.conn.get();
    };
    CHECK(r.policy("source") == "0");
    CHECK(r.host.conn == replacement.conn.get());
    CHECK(r.policy("context") == std::to_string(replacement.conn->interaction_context()));
    CHECK(r.policy("name").empty());
    r.host.conn = nullptr;
}
