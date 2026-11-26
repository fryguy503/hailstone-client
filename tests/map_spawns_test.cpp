// A server's plugin puts spawns on the maps (game/server_ext.h map_spawn, app/lua_ui_map.cpp): the minimap, the zone
// map and the map window draw the marks it gives, and nothing more without it.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/guide_path.h"
#include "app/lua_ui.h"
#include "game/server_ext.h"
#include "game/server_ext_chain.h"
#include "mq/map_filter.h"
#include "fuzz_support.h"
#include "mq_test_world.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

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
                    ImVec2* center = nullptr, std::vector<ImVec2>* positions = nullptr) {
    ImGui::GetIO().DisplaySize = ImVec2(800, 600);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(600, 500));
    ImGui::Begin("map", nullptr, ImGuiWindowFlags_NoTitleBar);
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        FAIL(lua_tostring(L, -1));
        lua_settop(L, 0);
    }
    int n = 0;
    ImVec2 sum{};
    if (positions) positions->clear();
    for (const ImDrawVert& v : ImGui::GetWindowDrawList()->VtxBuffer)
        if (rgb ? (v.col & 0x00ffffffu) == *rgb : v.col == kMark) {
            ++n;
            sum.x += v.pos.x; sum.y += v.pos.y;
            if (positions) positions->push_back(v.pos);
        }
    if (center && n) *center = ImVec2(sum.x / static_cast<float>(n), sum.y / static_cast<float>(n));
    ImGui::End();
    ImGui::Render();
    return n;
}

// Runs an already loaded Lua chunk in one map frame, without scanning its vertices.
void draw_prepared(lua_State* L) {
    ImGui::GetIO().DisplaySize = ImVec2(800, 600);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(600, 500));
    ImGui::Begin("map", nullptr, ImGuiWindowFlags_NoTitleBar);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        FAIL(lua_tostring(L, -1));
        lua_settop(L, 0);
    }
    ImGui::End();
    ImGui::Render();
}

struct MarkerFilter final : game::ServerExtension {
    bool deny_group = false, deny_npcs = false, marks = false, deny_find = false;
    std::uint32_t flags = 0;
    mutable int self_queries = 0, group_queries = 0;
    mutable std::size_t visible_queries = 0;
    std::uint64_t content = 1, life = 1;
    mutable unsigned policy_queries = 0, lifecycle_queries = 0, mark_queries = 0;
    std::function<void(const game::Spawn&)> on_visible, on_mark;
    std::function<void()> on_policy, on_lifecycle;
    game::MapPolicy map_policy(const net::GameConnection&) const override {
        ++policy_queries;
        const auto callback = on_policy;
        if (callback) callback();
        return {flags, "Map filter", content};
    }
    std::uint64_t lifecycle_revision(const net::GameConnection&) const override {
        ++lifecycle_queries;
        const auto callback = on_lifecycle;
        if (callback) callback();
        return life;
    }
    bool map_visible(const net::GameConnection&, const game::Spawn& sp, std::uint32_t purpose) const override {
        if (purpose != 0) return !(deny_find && sp.is_npc); // independent Find admission
        ++visible_queries;
        if (deny_npcs && sp.is_npc) return false;
        if (sp.id == 1) ++self_queries;
        if (sp.id == 12) ++group_queries;
        const auto callback = on_visible;
        if (callback) callback(sp);
        return !(deny_group && sp.id == 12);
    }
    std::optional<game::MapSpawnMark> map_spawn(const net::GameConnection&, const game::Spawn& sp,
                                               const game::MapSpawnMark&) const override {
        ++mark_queries;
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
    mq::MapFilters filters;
    std::unique_ptr<app::UiApi> api;
    lua_State* L = nullptr;
    explicit MarkerRig(std::shared_ptr<game::ServerExtension> extension = {}, bool map_label = false,
                       bool map_line = false)
        : world("map_marker_filter_world", options(std::move(extension))) {
        context = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().AddMousePosEvent(-1000, -1000);
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        host.in_game = host.ui_ready = host.feet_valid = true;
        host.map_filters = &filters;
        host.game = world.game.get(); host.conn = world.conn.get(); host.zone_short_name = "qeynos2";
        host.maps_dir = scratch.path() / "maps";
        if (map_label || map_line) {
            std::filesystem::create_directories(host.maps_dir);
            std::ofstream(host.maps_dir / "qeynos2.txt")
                << (map_line ? "L -100, 0, 0,  100, 0, 0,  0, 255, 0\n" : "")
                << (map_label ? "P 0, 0, 0,  255, 255, 0,  1,  Test_Label\n" : "");
        }
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
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
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
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
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
        // Numbers past any screen or map (a float cast of them is undefined): nothing drawn, nothing returned; a
        // centre that is not finite falls back to the zone's middle.
        CHECK(marked_vertices(L, "hs.DrawMinimap(1e300, 250, 100, 50)", kNear) == 0);
        CHECK(marked_vertices(L, "hs.DrawMinimap(300, 250, 100, 0/0)", kNear) == 0);
        CHECK(marked_vertices(L, "hs.DrawZoneMap(0, 0, math.huge, 500, 1)", kFar) == 0);
        CHECK(marked_vertices(L, "hs.DrawZoneMap(0, 0, 600, 500, 1e300)", kFar) == 0);
        CHECK(marked_vertices(L, "hs.DrawZoneMap(0, 0, 600, 500, 1, 0/0, 1e300)", kFar) > 0);
        REQUIRE(luaL_dostring(L, "return select('#', hs.DrawMinimap(1e300, 0, 1, 1))") == LUA_OK);
        CHECK(lua_tointeger(L, -1) == 0);
        lua_settop(L, 0);
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Map filters: command state draws spawn marks on every map view") {
    MarkerRig r;
    constexpr ImU32 kNpc = IM_COL32(0x40, 0x40, 0x40, 0);
    r.filters.set_enabled(mq::MapFilter::Npc, true);
    for (const auto& draw : kMapDraws) CHECK(marked_vertices(r.L, draw, kNpc) > 0);

    r.filters.set_enabled(mq::MapFilter::All, false);
    for (const auto& draw : kMapDraws) CHECK(marked_vertices(r.L, draw, kNpc) == 0);
    r.filters.set_enabled(mq::MapFilter::All, true);
    r.filters.set_enabled(mq::MapFilter::Npc, false);
    r.filters.set_enabled(mq::MapFilter::Target, true);
    constexpr ImU32 kTarget = IM_COL32(0xc0, 0, 0, 0);
    for (const auto& draw : kMapDraws) CHECK(marked_vertices(r.L, draw, kTarget) > 0);
}

TEST_CASE("Map filters: NPC marks follow the server's NPC map policy; the target and other spawns stay") {
    auto filter = std::make_shared<MarkerFilter>();
    MarkerRig r(filter);
    auto& conn = *r.world.conn;
    auto& world = net::GameConnectionTestAccess::world(conn);
    constexpr ImU32 kNpc = IM_COL32(0x40, 0x40, 0x40, 0), kTarget = IM_COL32(0xc0, 0, 0, 0);
    r.filters.set_enabled(mq::MapFilter::Npc, true);
    world.target_id = 0;
    auto npc_marks = [&] {
        int n = 0;
        for (const auto& draw : kMapDraws) n += marked_vertices(r.L, draw, kNpc) > 0 ? 1 : 0;
        return n;
    };
    REQUIRE(npc_marks() == 3);
    const std::array<std::uint32_t, 4> restrictive{game::MapPolicyHideStockNpcs, game::MapPolicyDisableNpcFind,
                                                   game::MapPolicyCustomSpawns,
                                                   game::MapPolicyServerNpcFindOnly | game::MapPolicyDisableNpcFind};
    for (const std::uint32_t flags : restrictive) {
        CAPTURE(flags);
        filter->flags = flags;
        CHECK(npc_marks() == 0);
        // A custom search for NPCs takes the same path.
        r.filters.set_custom("npc");
        CHECK(npc_marks() == 0);
        r.filters.clear_custom();
    }
    filter->flags = 0;
    filter->deny_find = true; // the Find visibility hook hides them as it hides Find's own rows
    CHECK(npc_marks() == 0);
    filter->deny_find = false;

    // Server-only Find: only the NPCs the server lists.
    filter->flags = game::MapPolicyServerNpcFindOnly;
    CHECK(npc_marks() == 0);
    net::FindableNpc listed;
    listed.spawn_id = 11;
    conn.receive_zone_packet_for_test(net::encode_findable_npc(listed));
    CHECK(npc_marks() == 3);
    filter->deny_find = true; // listed, but the Find hook still has the last word
    CHECK(npc_marks() == 0);
    filter->deny_find = false;
    listed.remove = true;
    conn.receive_zone_packet_for_test(net::encode_findable_npc(listed));
    CHECK(npc_marks() == 0);

    // The current target keeps its mark and lines; the player's own group mark is not an NPC's.
    filter->flags = game::MapPolicyHideStockNpcs | game::MapPolicyDisableNpcFind;
    world.target_id = 10;
    r.filters.set_enabled(mq::MapFilter::Target, true);
    for (const auto& draw : kMapDraws) {
        CHECK(marked_vertices(r.L, draw, kTarget) > 0);
        CHECK(r.group(draw) > 0);
    }
    filter->flags = 0;
    CHECK(npc_marks() == 3); // removing the flags restores the marks
}

TEST_CASE("Map filters: suspended filters draw nothing until the server allows /mapfilter again") {
    MarkerRig r;
    constexpr ImU32 kNpc = IM_COL32(0x40, 0x40, 0x40, 0), kTargetLine = IM_COL32(0x80, 0, 0, 0);
    r.filters.set_enabled(mq::MapFilter::Npc, true);
    r.filters.set_enabled(mq::MapFilter::Target, true);
    r.filters.set_enabled(mq::MapFilter::TargetLine, true);
    r.filters.set_suspended(true);
    for (const auto& draw : kMapDraws) {
        CHECK(marked_vertices(r.L, draw, kNpc) == 0);
        CHECK(marked_vertices(r.L, draw, kTargetLine) == 0);
        CHECK(r.group(draw) > 0); // the game's own group marks are not /mapfilter's
    }
    r.filters.set_suspended(false);
    for (const auto& draw : kMapDraws) {
        CHECK(marked_vertices(r.L, draw, kNpc) > 0);
        CHECK(marked_vertices(r.L, draw, kTargetLine) > 0);
    }
}

TEST_CASE("Map filters: vanilla keeps map lines and navigation but no marks or provider calls") {
    auto filter = std::make_shared<MarkerFilter>();
    filter->marks = true;
    MarkerRig r(filter, true, true);
    constexpr ImU32 kNpc = IM_COL32(0x40, 0x40, 0x40, 0);
    constexpr ImU32 kTargetLine = IM_COL32(0x80, 0, 0, 0);
    constexpr ImU32 kRadius = IM_COL32(0x80, 0x80, 0x80, 0);
    constexpr ImU32 kLabel = IM_COL32(255, 255, 0, 0);
    constexpr ImU32 kLine = IM_COL32(0, 255, 0, 0);
    r.filters.set_enabled(mq::MapFilter::Npc, true);
    r.filters.set_enabled(mq::MapFilter::Target, true);
    r.filters.set_enabled(mq::MapFilter::TargetLine, true);
    r.filters.set_radius(mq::MapFilter::TargetRadius, 30.0f);
    for (std::size_t i = 0; i < kMapDraws.size(); ++i) {
        CAPTURE(i);
        CHECK(marked_vertices(r.L, kMapDraws[i], kNpc) > 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], kMark & 0x00ffffffu) > 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], kGroupRgb) > 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], i == 0 ? kSelfRgb : IM_COL32(255, 255, 255, 0)) > 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], kTargetLine) > 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], kRadius) > 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], kLine) > 0);
    }
    CHECK(marked_vertices(r.L, kMapDraws[0], kLabel) > 0);
    CHECK(marked_vertices(r.L, kMapDraws[1], kLabel) > 0);

    filter->policy_queries = filter->lifecycle_queries = filter->mark_queries = 0;
    filter->visible_queries = filter->self_queries = filter->group_queries = 0;
    r.filters.set_vanilla_enabled(true);
    for (std::size_t i = 0; i < kMapDraws.size(); ++i) {
        CAPTURE(i);
        CHECK(marked_vertices(r.L, kMapDraws[i], kNpc) == 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], kMark & 0x00ffffffu) == 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], kGroupRgb) == 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], i == 0 ? kSelfRgb : IM_COL32(255, 255, 255, 0)) == 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], kTargetLine) == 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], kRadius) == 0);
        CHECK(marked_vertices(r.L, kMapDraws[i], kLine) > 0); // the background is still drawn
    }
    CHECK(marked_vertices(r.L, kMapDraws[0], kLabel) == 0);
    CHECK(marked_vertices(r.L, kMapDraws[1], kLabel) == 0);
    CHECK(filter->policy_queries == 0); // vanilla does not ask map_visible, map_policy or map_spawn
    CHECK(filter->lifecycle_queries == 0);
    CHECK(filter->mark_queries == 0);
    CHECK(filter->visible_queries == 0);
    CHECK(filter->self_queries == 0);
    CHECK(filter->group_queries == 0);

    marked_vertices(r.L, "local nx, ny, px, py, k = hs.DrawMinimap(300, 250, 100, 500); "
                        "nav_minimap = type(nx) == 'number' and type(ny) == 'number' and px ~= nil and k > 0", kLine);
    marked_vertices(r.L, "local k, mx, my, px, py = hs.DrawZoneMap(0, 0, 600, 500, 1); "
                        "nav_zone = k > 0 and mx ~= nil and my ~= nil and px ~= nil", kLine);
    for (const char* name : {"nav_minimap", "nav_zone"}) {
        lua_getglobal(r.L, name);
        CHECK(lua_toboolean(r.L, -1));
        lua_pop(r.L, 1);
    }
}

TEST_CASE("Map filters: vanilla tells the packs and leaves their own drawing to them") {
    MarkerRig r({}, false, true);
    ui::lua::register_ui_api(r.L);
    auto vanilla = [&] {
        REQUIRE(luaL_dostring(r.L, "return hs.UI.map.Vanilla()") == LUA_OK);
        const bool on = lua_toboolean(r.L, -1) != 0;
        lua_settop(r.L, 0);
        return on;
    };
    CHECK_FALSE(vanilla());
    r.filters.set_vanilla_enabled(true);
    CHECK(vanilla());
    r.filters.set_vanilla_enabled(false);
    r.filters.set_vanilla_locked(true); // the server's rule
    CHECK(vanilla());
    r.filters.set_vanilla_locked(false);
    r.filters.set_vanilla_enabled(true);

    // ui.Draw* is the packs' own: text, bars and marks over a vanilla map all draw (a pack asks hs.UI.map.Vanilla
    // to leave its marks out), and the map's lines still draw.
    constexpr ImU32 kPack = IM_COL32(1, 2, 3, 0), kLine = IM_COL32(0, 255, 0, 0);
    const std::string pack =
        "local hr,hg,hb=1/255,2/255,3/255; ui.DrawCircleFilled(300,250,5,hr,hg,hb,1); "
        "ui.DrawText(280,220,hr,hg,hb,1,'x2 (panned)'); ui.DrawRectFilled(260,120,300,160,hr,hg,hb,1)";
    for (const auto& draw : kMapDraws) {
        CAPTURE(draw);
        CHECK(marked_vertices(r.L, draw + "; " + pack, kPack) > 0);
        CHECK(marked_vertices(r.L, draw, kLine) > 0);
    }
}

TEST_CASE("Map filters: on and off commands add and remove NPC marks") {
    MarkerRig r;
    constexpr ImU32 kNpc = IM_COL32(0x40, 0x40, 0x40, 0);
    std::vector<std::string> lines;
    auto command = [&](std::string_view text) {
        lines.clear();
        run_map_filter_command(r.filters, text, {}, [&](std::string_view line) { lines.emplace_back(line); });
    };
    command("npc off");
    REQUIRE(lines.size() == 1);
    CHECK(lines.back() == "NPC is now set to: hide");
    CHECK(marked_vertices(r.L, kMapDraws[0], kNpc) == 0);
    command("npc on");
    CHECK(lines.back() == "NPC is now set to: show");
    CHECK(marked_vertices(r.L, kMapDraws[0], kNpc) > 0);
    command("npc off");
    CHECK(marked_vertices(r.L, kMapDraws[0], kNpc) == 0);
    command("npc"); // no value still toggles, as MQ2Map does
    CHECK(marked_vertices(r.L, kMapDraws[0], kNpc) > 0);
}

TEST_CASE("Map filters: ground objects draw without a full item database") {
    MarkerRig r;
    game::GroundObject object;
    object.drop_id = 99;
    object.name = "IT63_ACTORDEF";
    object.position = game::server_to_world({10.0f, 0.0f, 0.0f});
    net::GameConnectionTestAccess::ground_objects(*r.world.conn).push_back(object);
    r.filters.set_enabled(mq::MapFilter::Ground, true);
    constexpr ImU32 kGround = IM_COL32(0xc0, 0xc0, 0xc0, 0);
    for (const auto& draw : kMapDraws) CHECK(marked_vertices(r.L, draw, kGround) > 0);
}

TEST_CASE("Map filters: a ground object gone by the end of collection is not drawn") {
    for (const auto& draw : kMapDraws) {
        CAPTURE(draw);
        auto filter = std::make_shared<MarkerFilter>();
        MarkerRig r(filter);
        auto& ground = net::GameConnectionTestAccess::ground_objects(*r.world.conn);
        for (const std::uint32_t id : {98u, 99u}) {
            game::GroundObject object;
            object.drop_id = id;
            object.name = "IT63_ACTORDEF";
            object.position = game::server_to_world({id == 98u ? 20.0f : -20.0f, 0.0f, 0.0f});
            ground.push_back(object);
        }
        r.filters.set_enabled(mq::MapFilter::Ground, true);
        constexpr ImU32 kGround = IM_COL32(0xc0, 0xc0, 0xc0, 0);
        filter->policy_queries = 0;
        const int both = marked_vertices(r.L, draw, kGround);
        REQUIRE(both > 0);
        const auto final_query = filter->policy_queries;
        filter->policy_queries = 0;
        filter->on_policy = [&] {
            if (filter->policy_queries != final_query) return; // the final source query, after the collection
            std::erase_if(ground, [](const game::GroundObject& o) { return o.drop_id == 99; });
        };
        const int one = marked_vertices(r.L, draw, kGround);
        filter->on_policy = {};
        CHECK(one > 0);
        CHECK(one < both);
        CHECK(marked_vertices(r.L, draw, kGround) == one); // and stays gone
    }
}

TEST_CASE("Map filters: vectors, markers and target overlays draw on every map view") {
    MarkerRig r;
    r.filters.set_enabled(mq::MapFilter::Npc, true);
    r.filters.set_color(mq::MapFilter::Npc, 0x123456);
    constexpr ImU32 kNpc = IM_COL32(0x12, 0x34, 0x56, 0);
    std::array<int, kMapDraws.size()> baseline_counts{};
    for (std::size_t i = 0; i < kMapDraws.size(); ++i) baseline_counts[i] = marked_vertices(r.L, kMapDraws[i], kNpc);
    for (int count : baseline_counts) REQUIRE(count > 0);

    r.filters.set_enabled(mq::MapFilter::Vector, true);
    std::array<int, kMapDraws.size()> vector_counts{};
    for (std::size_t i = 0; i < kMapDraws.size(); ++i) vector_counts[i] = marked_vertices(r.L, kMapDraws[i], kNpc);
    for (std::size_t i = 0; i < kMapDraws.size(); ++i) REQUIRE(vector_counts[i] > baseline_counts[i]);
    r.filters.set_marker(mq::MapFilter::Npc, mq::MapMarker::Square, 20);
    for (std::size_t i = 0; i < kMapDraws.size(); ++i)
        CHECK(marked_vertices(r.L, kMapDraws[i], kNpc) > vector_counts[i]);

    r.filters.set_enabled(mq::MapFilter::Target, true);
    r.filters.set_enabled(mq::MapFilter::TargetLine, true);
    constexpr ImU32 kTargetLine = IM_COL32(0x80, 0, 0, 0);
    for (const auto& draw : kMapDraws) CHECK(marked_vertices(r.L, draw, kTargetLine) > 0);
    r.filters.set_radius(mq::MapFilter::TargetRadius, 30.0f);
    constexpr ImU32 kRadius = IM_COL32(0x80, 0x80, 0x80, 0);
    for (const auto& draw : kMapDraws) CHECK(marked_vertices(r.L, draw, kRadius) > 0);
    r.filters.set_radius(mq::MapFilter::TargetRadius, 0.0f);
    r.filters.set_enabled(mq::MapFilter::TargetMelee, true);
    constexpr ImU32 kMelee = IM_COL32(0xff, 0x80, 0x80, 0);
    for (const auto& draw : kMapDraws) CHECK(marked_vertices(r.L, draw, kMelee) > 0);
    r.filters.set_enabled(mq::MapFilter::TargetMelee, false);
    r.filters.set_radius(mq::MapFilter::CastRadius, 40.0f);
    for (const auto& draw : kMapDraws) CHECK(marked_vertices(r.L, draw, kRadius) > 0);
}

TEST_CASE("Map filters: marker sizes, shapes and vector direction use the MQ geometry") {
    MarkerRig r;
    auto& world = net::GameConnectionTestAccess::world(*r.world.conn);
    world.spawns.at(11).is_npc = false;
    world.spawns.at(13).is_npc = false;
    auto& rat = world.spawns.at(10);
    rat.heading = 0.0f;
    rat.speed_run = 0.0f;
    rat.velocity = {0.0f, 0.0f, 0.0f};
    r.filters.set_enabled(mq::MapFilter::Npc, true);
    r.filters.set_color(mq::MapFilter::Npc, 0x123456);
    constexpr ImU32 kNpc = IM_COL32(0x12, 0x34, 0x56, 0);
    const std::string minimap = "hs.DrawMinimap(300, 250, 100, 100)";
    ImVec2 center{};
    std::vector<ImVec2> points;
    REQUIRE(marked_vertices(r.L, minimap, kNpc, &center, &points) == 8); // two 1.5-pixel cross lines

    r.filters.set_marker(mq::MapFilter::Npc, mq::MapMarker::Square, 20);
    REQUIRE(marked_vertices(r.L, minimap, kNpc, nullptr, &points) == 24);
    auto width = [&] {
        const auto [lo, hi] = std::minmax_element(points.begin(), points.end(), [](ImVec2 a, ImVec2 b) { return a.x < b.x; });
        return hi->x - lo->x;
    };
    CHECK(width() == doctest::Approx(22.5f).epsilon(0.02)); // 20 units plus the 1.5-pixel stroke and its AA fringe
    r.filters.set_marker(mq::MapFilter::Npc, mq::MapMarker::Square, 10);
    CHECK(marked_vertices(r.L, minimap, kNpc, nullptr, &points) == 24);
    CHECK(width() == doctest::Approx(12.5f).epsilon(0.02));
    r.filters.set_marker(mq::MapFilter::Npc, mq::MapMarker::Diamond, 10);
    CHECK(marked_vertices(r.L, minimap, kNpc) == 24);
    r.filters.set_marker(mq::MapFilter::Npc, mq::MapMarker::Triangle, 10);
    CHECK(marked_vertices(r.L, minimap, kNpc) == 20);
    r.filters.set_marker(mq::MapFilter::Npc, mq::MapMarker::Ring, 10);
    CHECK(marked_vertices(r.L, minimap, kNpc) == 40);

    r.filters.set_marker(mq::MapFilter::Npc, mq::MapMarker::None, 0);
    std::vector<ImVec2> cross_points;
    REQUIRE(marked_vertices(r.L, minimap, kNpc, &center, &cross_points) == 8);
    r.filters.set_enabled(mq::MapFilter::Vector, true);
    REQUIRE(marked_vertices(r.L, minimap, kNpc, nullptr, &points) == 12);
    std::vector<ImVec2> vector_points;
    auto refresh_vector_points = [&] {
        vector_points.clear();
        for (const auto& point : points)
            if (std::none_of(cross_points.begin(), cross_points.end(), [&](ImVec2 old) {
                    const float dx = point.x - old.x, dy = point.y - old.y;
                    return dx * dx + dy * dy < 0.01f;
                }))
                vector_points.push_back(point);
    };
    refresh_vector_points();
    REQUIRE(vector_points.size() == 4);
    const auto farthest = [&] {
        return *std::max_element(vector_points.begin(), vector_points.end(), [&](ImVec2 a, ImVec2 b) {
            const auto ax = a.x - center.x, ay = a.y - center.y, bx = b.x - center.x, by = b.y - center.y;
            return ax * ax + ay * ay < bx * bx + by * by;
        });
    };
    auto tip = farthest(); // heading 0 points north on the map
    CHECK(tip.y < center.y - 2.0f);
    CHECK(std::abs(tip.x - center.x) < 2.0f);
    rat.heading = 128.0f; // a quarter turn points left
    REQUIRE(marked_vertices(r.L, minimap, kNpc, nullptr, &points) == 12);
    refresh_vector_points();
    REQUIRE(vector_points.size() == 4);
    tip = farthest();
    CHECK(tip.x < center.x - 2.0f);
    CHECK(std::abs(tip.y - center.y) < 2.0f);

    r.filters.set_enabled(mq::MapFilter::Vector, false);
    r.filters.set_color(mq::MapFilter::CastRadius, 0x654321);
    r.filters.set_radius(mq::MapFilter::CastRadius, 40.0f);
    constexpr ImU32 kCircle = IM_COL32(0x65, 0x43, 0x21, 0);
    REQUIRE(marked_vertices(r.L, minimap, kCircle, nullptr, &points) == 144); // 36 thick line segments
    CHECK(width() == doctest::Approx(82.5f).epsilon(0.02));
}

TEST_CASE("Map filters: minimap target geometry reaches in from outside the circle") {
    MarkerRig r;
    auto& world = net::GameConnectionTestAccess::world(*r.world.conn);
    auto& target = world.spawns.at(10);
    target.position = game::server_to_world({600.0f, 0.0f, 0.0f});
    target.display_valid = false;
    r.filters.set_enabled(mq::MapFilter::Target, true);
    r.filters.set_enabled(mq::MapFilter::TargetLine, true);
    constexpr ImU32 kTarget = IM_COL32(0xc0, 0, 0, 0);
    constexpr ImU32 kLine = IM_COL32(0x80, 0, 0, 0);
    constexpr ImU32 kRadius = IM_COL32(0x80, 0x80, 0x80, 0);
    const std::string minimap = "hs.DrawMinimap(300, 250, 100, 500)";
    CHECK(marked_vertices(r.L, minimap, kTarget) == 0); // the target's own dot stays outside
    CHECK(marked_vertices(r.L, minimap, kLine) > 0);   // but the line crosses the visible circle
    r.filters.set_radius(mq::MapFilter::TargetRadius, 150.0f);
    CHECK(marked_vertices(r.L, minimap, kRadius) > 0); // and so does the near arc of its circle

    auto filter = std::make_shared<MarkerFilter>();
    filter->deny_npcs = true;
    MarkerRig denied(filter);
    auto& denied_world = net::GameConnectionTestAccess::world(*denied.world.conn);
    auto& denied_target = denied_world.spawns.at(10);
    denied_target.position = game::server_to_world({600.0f, 0.0f, 0.0f});
    denied_target.display_valid = false;
    denied.filters.set_enabled(mq::MapFilter::Target, true);
    denied.filters.set_enabled(mq::MapFilter::TargetLine, true);
    denied.filters.set_radius(mq::MapFilter::TargetRadius, 150.0f);
    CHECK(marked_vertices(denied.L, minimap, kLine) == 0);
    CHECK(marked_vertices(denied.L, minimap, kRadius) == 0);
}

TEST_CASE("Map filters: server marker denial wins over command filters") {
    auto filter = std::make_shared<MarkerFilter>();
    MarkerRig r(filter);
    r.filters.set_enabled(mq::MapFilter::Npc, true);
    constexpr ImU32 kNpc = IM_COL32(0x40, 0x40, 0x40, 0);
    for (const auto& draw : kMapDraws) CHECK(marked_vertices(r.L, draw, kNpc) > 0);
    filter->deny_npcs = true;
    for (const auto& draw : kMapDraws) CHECK(marked_vertices(r.L, draw, kNpc) == 0);
    filter->deny_npcs = false;
    for (const auto& draw : kMapDraws) CHECK(marked_vertices(r.L, draw, kNpc) > 0);
}

TEST_CASE("Map filters: NormalLabels hides only the game's map labels") {
    MarkerRig r({}, true);
    constexpr ImU32 kLabel = IM_COL32(255, 255, 0, 0);
    CHECK(marked_vertices(r.L, kMapDraws[0], kLabel) > 0);
    CHECK(marked_vertices(r.L, kMapDraws[1], kLabel) > 0);
    r.filters.set_enabled(mq::MapFilter::NormalLabels, false);
    CHECK(marked_vertices(r.L, kMapDraws[0], kLabel) == 0);
    CHECK(marked_vertices(r.L, kMapDraws[1], kLabel) == 0);

    r.api->begin_frame();
    REQUIRE(luaL_dostring(r.L, "hs.ShowWindow('MapToolbarWnd', true)") == LUA_OK);
    CHECK(marked_vertices(r.L, kMapDraws[0], kLabel) > 0); // the editor still needs its labels
    CHECK(marked_vertices(r.L, kMapDraws[1], kLabel) == 0); // the minimap has no editor
}

TEST_CASE("test support: scoped allocation count measures global new requests") {
    fuzz::start_allocation_count();
    void* memory = ::operator new(256);
    static_cast<volatile unsigned char*>(memory)[0] = 1;
    const auto count = fuzz::stop_allocation_count();
    ::operator delete(memory);
    CHECK(count.calls >= 1);
    CHECK(count.bytes >= 256);
}

TEST_CASE("Map filters: benchmark collection cost when requested") {
    if (!std::getenv("HAILSTONE_MAPFILTER_BENCH")) return;
    std::printf("MAPFILTER_BENCH,sizeof_spawn=%zu\n", sizeof(game::Spawn));
    constexpr std::array<std::string_view, 4> cases{"none", "target", "npc", "custom"};
    constexpr std::array<std::pair<std::string_view, std::string_view>, 2> views{
        std::pair{"map", "hs.UI.map.Draw(0, 0, 600, 500, true)"},
        std::pair{"minimap", "hs.DrawMinimap(300, 250, 100, 500)"}};

    for (const std::size_t count : {std::size_t{1000}, std::size_t{3000}}) {
        for (const bool provider : {false, true}) {
            auto filter = provider ? std::make_shared<MarkerFilter>() : std::shared_ptr<MarkerFilter>{};
            MarkerRig r(filter);
            auto& world = net::GameConnectionTestAccess::world(*r.world.conn);
            constexpr int kSide = 61;
            const int rows = static_cast<int>((count + kSide - 1) / kSide);
            for (std::size_t i = 0; i < count; ++i) {
                const int x = static_cast<int>(i % kSide) - kSide / 2;
                const int y = static_cast<int>(i / kSide) - rows / 2;
                r.world.add(static_cast<std::uint32_t>(1000 + i), "Bench_NPC_" + std::to_string(i), true, 10,
                            game::server_to_world({static_cast<float>(x * 100), static_cast<float>(y * 100), 0.0f}));
            }
            world.target_id = static_cast<std::uint32_t>(1000 + count / 2);
            auto configure = [&](std::string_view name) {
                for (const auto& spec : mq::map_filter_specs()) {
                    r.filters.set_enabled(spec.filter, false);
                    r.filters.set_radius(spec.filter, 0.0f);
                    r.filters.set_marker(spec.filter, mq::MapMarker::None, 0);
                }
                r.filters.set_enabled(mq::MapFilter::All, true);
                r.filters.set_enabled(mq::MapFilter::NormalLabels, true);
                r.filters.set_custom({});
                if (name == "target") r.filters.set_enabled(mq::MapFilter::Target, true);
                else if (name == "npc") r.filters.set_enabled(mq::MapFilter::Npc, true);
                else if (name == "custom") r.filters.set_custom("npc radius 500");
            };
            const int iterations = count == 1000 ? 120 : 60;
            for (const auto& name : cases) {
                configure(name);
                for (const auto& [view, code] : views) {
                    const std::string draw_code(code);
                    REQUIRE(luaL_loadstring(r.L, draw_code.c_str()) == LUA_OK);
                    for (int i = 0; i < 5; ++i) {
                        lua_pushvalue(r.L, -1);
                        draw_prepared(r.L);
                    }
                    if (filter) filter->visible_queries = 0;
                    fuzz::start_allocation_count();
                    const auto start = std::chrono::steady_clock::now();
                    for (int i = 0; i < iterations; ++i) {
                        lua_pushvalue(r.L, -1);
                        draw_prepared(r.L);
                    }
                    const double ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    const auto allocations = fuzz::stop_allocation_count();
                    lua_pop(r.L, 1); // the prepared chunk
                    const auto queries = filter ? filter->visible_queries : 0;
                    std::printf("MAPFILTER_BENCH,provider=%d,n=%zu,case=%.*s,view=%.*s,iterations=%d,ms=%.6f,"
                                "allocs=%zu,bytes=%zu,visible_queries=%zu\n",
                                provider ? 1 : 0, count, static_cast<int>(name.size()), name.data(),
                                static_cast<int>(view.size()), view.data(), iterations, ms, allocations.calls,
                                allocations.bytes, queries);
                }
            }
        }
    }
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
