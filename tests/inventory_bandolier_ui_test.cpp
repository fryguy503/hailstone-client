// The inventory's Bandolier page (ui/modern/inventory.lua over lib/bandolier.lua), run for real over Dear ImGui with no
// renderer against the mock server: it draws without script errors, and the game's BandolierWnd it drives answers
// while that window stays hidden (Save stores a set, Activate swaps to it).
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "app/lua_ui.h"
#include "mock_server/harness.h"
#include "net/packet_io.h"
#include "net/player_packets.h"
#include "ui/lua/lua_host.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::test;
namespace fs = std::filesystem;

namespace {

fs::path ui_dir() { return fs::path(__FILE__).parent_path().parent_path() / "ui"; }

void frame(ui::lua::LuaUi& lua) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    lua.frame();
    ImGui::Render();
}

bool window_named(std::string_view part) {
    for (ImGuiWindow* w : GImGui->Windows)
        if (w->Active && std::string_view(w->Name).find(part) != std::string_view::npos) return true;
    return false;
}

} // namespace

TEST_CASE("Inventory's Bandolier page: draws, and its Save and Activate reach the hidden BandolierWnd") {
    fs::path src = ui_dir() / "modern";
    REQUIRE(fs::exists(src / "lib" / "bandolier.lua"));
    ScratchDir scratch("inventory_bandolier_ui_test");
    fs::path dir = scratch.path();
    fs::copy(src, dir / "modern", fs::copy_options::recursive);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        Rig rig;
        REQUIRE(rig.to_zone());
        auto& conn = rig.conn;
        REQUIRE(conn.bandolier_sets() >= 4);

        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &rig.game;
        host.conn = &conn;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        ui::lua::LuaUi lua(dir / "modern", dir / "modern.txt");
        lua.add_ui_api([&](lua_State* L) {
            api.register_functions(L);
            lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
            lua_setfield(L, -2, "InGame");
            hailstone::test::sync_ui_api_aliases(L);
        });
        lua_State* L = lua.state();
        REQUIRE(luaL_dostring(L, "ui.SetSetting('test.inv.page', 'bandolier')") == LUA_OK);
        api.show("InventoryWindow", true);
        auto frames = [&] {
            for (int i = 0; i < 3; ++i) {
                api.begin_frame();
                frame(lua);
                rig.step();
            }
        };
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(ImGui::FindWindowByName("###modern.InventoryWindow") != nullptr);
        CHECK(window_named("##bandolierset"));
        CHECK_FALSE(api.shown("BandolierWnd"));

        // Save on the selected set (row 0): the name asked for, then the set stored and sent.
        REQUIRE(luaL_dostring(L, "hs.ClickChild('BandolierWnd', 'AddButton', 'left')") == LUA_OK);
        REQUIRE(host.text_entry);
        host.text_entry->done("Melee");
        host.text_entry.reset();
        CHECK(conn.bandolier()[0].name == "Melee");
        frames(); // the page shows the named set
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(luaL_dostring(L, "return (hs.ListRow('BandolierWnd', 'WeaponSetList', 0) or {})[1]") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1)) == "Melee");
        lua_settop(L, 0);

        // Activate: OP_Bandolier action 2 for set 0.
        REQUIRE(luaL_dostring(L, "hs.ClickChild('BandolierWnd', 'UseButton', 'left')") == LUA_OK);
        auto sent = [&](std::uint32_t action) {
            return std::any_of(rig.server.zone_log.begin(), rig.server.zone_log.end(), [&](const net::AppPacket& p) {
                if (!net::is_op(p, net::Op::Bandolier)) return false;
                auto b = net::decode_bandolier(p);
                return b && b->action == action && b->number == 0;
            });
        };
        REQUIRE(rig.until([&] { return sent(net::kBandolierCreate) && sent(net::kBandolierSet); }));
        CHECK_FALSE(api.shown("BandolierWnd"));
    }
    ImGui::DestroyContext(ctx);
}
