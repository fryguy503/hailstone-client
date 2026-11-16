// The modern map's World view and zone finder (docs/navigation.md "Zone atlas", app/lua_ui_map.cpp): the atlas as
// data, the zone links kept per server (learned from zone points, the server's list), routes, the current zone's
// exits and the walk through one, another zone's map.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "core/game_dir.h"
#include "game/zone_names.h"
#include "mq_test_world.h"
#include "net/zone_link_packets.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

// Runs `code` in a frame (inside a window, as the pack calls it) and returns what it returns as a string.
std::string run(lua_State* L, const std::string& code) {
    ImGui::GetIO().DisplaySize = ImVec2(800, 600);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::Begin("map");
    std::string out;
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        FAIL(lua_tostring(L, -1));
    } else if (lua_gettop(L) > 0 && !lua_isnil(L, -1)) {
        out = luaL_tolstring(L, -1, nullptr);
        lua_pop(L, 1);
    }
    lua_settop(L, 0);
    ImGui::End();
    ImGui::Render();
    return out;
}

std::string links_file_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("Zone atlas: the atlas, routes, exits and another zone's map through hs.*") {
    mq_test::World world("hailstone_zone_atlas_test");
    test::ScratchDir scratch("zone_atlas_api_test");
    // A two-page atlas in the game folder.
    const fs::path atlas = world.dir / "Atlas" / "Default";
    fs::create_directories(atlas);
    std::ofstream(atlas / "Default.xml")
        << "<Image File = \"a.tga\"><Rect X = 0 Y = 0 CX = 256 CY = 128/></Image>"
           "<Label Text = \"Antonica\" X = 5 Y = 6/>"
           "<Link ClickAction = 1 FileToOpen = \"Qeynos.xml\" ToolTip = \"Qeynos Hills\"><Rect X = 0 Y = 0 CX = 50 CY = 50/></Link>";
    std::ofstream(atlas / "Qeynos.xml")
        << "<Link ClickAction = 2 FileToOpen = \"qeynos2\" ToolTip = \"North Qeynos\"><Rect X = 10 Y = 10 CX = 20 CY = 20/></Link>"
           "<Link ClickAction = 2 FileToOpen = \"QEYTOQRG\"><Rect X = 40 Y = 10 CX = 20 CY = 20/></Link>"
           "<Link ClickAction = 2 FileToOpen = \"nowhere_zone\" ToolTip = \"Nowhere\"><Rect X = 70 Y = 10 CX = 20 CY = 20/></Link>";

    const std::uint32_t qeynos2 = game::zone_id_by_short_name("qeynos2");
    const std::uint32_t hills = game::zone_id_by_short_name("qeytoqrg");
    const std::uint32_t surefall = game::zone_id_by_short_name("qrg");
    REQUIRE(qeynos2 != 0);
    REQUIRE(hills != 0);
    REQUIRE(surefall != 0);
    using A = net::GameConnectionTestAccess;
    net::NewZone nz;
    nz.zone_short_name = "qeynos2";
    nz.zone_id = static_cast<std::uint16_t>(qeynos2);
    A::new_zone(*world.conn) = nz;

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        // The game folder read again: GameDir lists its files when it is made.
        GameDir game(world.dir);
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = world.conn.get();
        host.zone_short_name = "qeynos2";
        host.zone_long_name = "North Qeynos";
        host.maps_dir = scratch.path() / "server" / "maps";
        host.server_dir = scratch.path() / "server";
        host.feet_valid = true; // the player at the origin
        std::vector<std::string> submitted;
        host.submit_chat = [&](const std::string& line) { submitted.push_back(line); };
        // A link learned on an earlier visit, kept in the server's file.
        fs::create_directories(host.server_dir);
        std::ofstream(host.server_dir / "zone_links.txt")
            << "l " << hills << " " << surefall << "\n";
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        auto frame = [&] { api.begin_frame(); };

        SUBCASE("the atlas as data") {
            frame();
            CHECK(run(L, "local i = hs.AtlasIndex() return #i.pages .. ' ' .. #i.zones") == "2 3");
            CHECK(run(L, "local i = hs.AtlasIndex() return i.pages[2].title .. '|' .. i.pages[2].parent") == "Qeynos Hills|Default.xml");
            // The zone table's spelling of a short name; an unknown zone keeps the atlas' and has id 0.
            CHECK(run(L, "local z = hs.AtlasIndex().zones return z[2].zone .. '|' .. z[2].name .. '|' .. z[3].id") ==
                  "qeytoqrg|QEYTOQRG|0");
            CHECK(run(L, "local p = hs.AtlasPageData('Default.xml') return p.width .. 'x' .. p.height .. ' ' .. #p.images .. ' ' .. p.labels[1].text .. ' ' .. p.links[1].action") ==
                  "256x128 1 Antonica 1");
            CHECK(run(L, "return tostring(hs.AtlasPageData('Missing.xml'))") == "nil");
        }

        SUBCASE("routes: the file, learned zone points, the server's list") {
            frame(); // the file is read
            CHECK(run(L, "local s, n = hs.ZoneLinks() return tostring(s) .. ' ' .. n") == "false 2");
            CHECK(run(L, "return tostring(hs.ZoneRoute('qrg'))") == "nil"); // nothing leads from North Qeynos yet
            // Zone-in: North Qeynos' zone points lead to the hills.
            net::ZonePoint zp;
            zp.number = 1;
            zp.zone_id = static_cast<std::uint16_t>(hills);
            A::zone_points(*world.conn) = {zp};
            frame();
            CHECK(run(L, "local r = hs.ZoneRoute('qrg') return #r .. ' ' .. r[1].zone .. '>' .. r[2].zone .. '>' .. r[3].zone .. ' ' .. tostring(r.assumed)") ==
                  "3 qeynos2>qeytoqrg>qrg false");
            CHECK(links_file_text(host.server_dir / "zone_links.txt").find("l " + std::to_string(qeynos2) + " " + std::to_string(hills)) !=
                  std::string::npos);
            // Backwards over learned links: assumed.
            CHECK(run(L, "local r = hs.ZoneRoute('qeynos2', 'qrg') return #r .. ' ' .. tostring(r.assumed)") == "3 true");
            // The server's list: asked only once the script answered the settings request.
            net::HailstoneSettings hs;
            A::handle_zone(*world.conn, net::encode(hs));
            net::ZoneLinks links;
            links.links = {{static_cast<std::uint16_t>(surefall), static_cast<std::uint16_t>(qeynos2)}};
            A::handle_zone(*world.conn, net::encode(links));
            frame();
            CHECK(run(L, "local s, n = hs.ZoneLinks() return tostring(s) .. ' ' .. n") == "true 3");
            CHECK(run(L, "local r = hs.ZoneRoute('qeynos2', 'qrg') return #r .. ' ' .. tostring(r.assumed)") == "2 false");
            CHECK(links_file_text(host.server_dir / "zone_links.txt").find("s " + std::to_string(surefall)) != std::string::npos);
        }

        SUBCASE("only the zones the server has: its list, else the account's expansions") {
            frame();
            // No list, every expansion bit: nothing left out.
            CHECK(run(L, "local s, n, limit = hs.ZoneLinks() return tostring(limit)") == "nil");
            // The account's bits: Kunark only (bit 0). Classic zones stay; a Velious one (expansion 2) goes.
            auto& prof = *A::profile(*world.conn);
            prof.expansions = 0x1;
            const std::uint32_t velious = game::zone_id_by_short_name("thurgadina");
            REQUIRE(game::zone_expansion(velious) == 2);
            CHECK(run(L, "local s, n, limit = hs.ZoneLinks() return tostring(limit)") == "account");
            CHECK(run(L, "return tostring(hs.ZoneRoute('thurgadina'))") == "nil");
            CHECK(run(L, "return tostring(hs.AtlasIndex().zones[1].allowed)") == "true"); // qeynos2, Classic
            // The server's list of zones wins: North Qeynos and the hills, not Surefall Glade.
            net::HailstoneSettings hs;
            A::handle_zone(*world.conn, net::encode(hs));
            net::ZoneLinks links;
            links.links = {{static_cast<std::uint16_t>(qeynos2), static_cast<std::uint16_t>(hills)}};
            links.has_zones = true;
            links.expansion = 1;
            links.zones = {static_cast<std::uint16_t>(qeynos2), static_cast<std::uint16_t>(hills)};
            A::handle_zone(*world.conn, net::encode(links));
            frame();
            CHECK(run(L, "local s, n, limit, e = hs.ZoneLinks() return limit .. ' ' .. e") == "server 1");
            CHECK(run(L, "return #hs.ZoneRoute('qeytoqrg')") == "2");
            CHECK(run(L, "return tostring(hs.ZoneRoute('qrg'))") == "nil"); // the server lacks it
            CHECK(links_file_text(host.server_dir / "zone_links.txt").find("e 1\n") != std::string::npos);
        }

        SUBCASE("the exits, and the walk through one") {
            frame();
            CHECK(run(L, "return #hs.ZoneExits()") == "0"); // not worked out for this zone yet
            app::UiHost::ZoneExit e;
            e.zone_id = hills;
            e.lo[0] = 100; e.lo[1] = -20; e.lo[2] = -10;
            e.hi[0] = 110; e.hi[1] = 20; e.hi[2] = 40;
            e.cells.push_back({100, -20, -10, 110, 20, 40});
            host.zone_exits = {e};
            host.zone_exits_zone = qeynos2;
            CHECK(run(L, "local x = hs.ZoneExits()[1] return x.zone .. ' ' .. x.x .. ' ' .. x.y") == "qeytoqrg 0.0 -105.0");
            CHECK(run(L, "return tostring(hs.ZoneExitWalk('qrg'))") == "nil"); // no line there
            CHECK(submitted.empty());
            // The nearest point 2 inside the line: world (102, 0, 0), the server's (0, 102, 0).
            CHECK(run(L, "local x, y = hs.ZoneExitWalk('qeytoqrg') return x .. ',' .. y") == "-0.0,-102.0");
            REQUIRE(submitted.size() == 1);
            CHECK(submitted[0] == "/nav locxyz 0.0 102.0 0.0");
        }

        SUBCASE("another zone's map, then your own again") {
            frame();
            CHECK(run(L, "local l, s, own = hs.MapZoneShown() return s .. ' ' .. tostring(own)") == "qeynos2 true");
            run(L, "hs.ShowZoneMap('qeytoqrg')");
            CHECK(run(L, "local l, s, own = hs.MapZoneShown() return s .. ' ' .. tostring(own)") == "qeytoqrg false");
            // No player on another zone's map.
            CHECK(run(L, "local k, mx, my, px = hs.DrawZoneMap(0, 0, 600, 500, 1) return tostring(px)") == "nil");
            run(L, "hs.ShowZoneMap()");
            frame();
            CHECK(run(L, "local l, s, own = hs.MapZoneShown() return s .. ' ' .. tostring(own)") == "qeynos2 true");
        }
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}
