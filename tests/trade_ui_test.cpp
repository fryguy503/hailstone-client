// The modern pack's Trade window (ui/modern/trade.lua), run for real over Dear ImGui with no renderer against the
// mock server: who has accepted shows in the window (a status line, an "Accepted" tag on that side's offer, the Trade
// button's label once we have accepted), and it draws without script errors in every state.
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "app/lua_ui.h"
#include "mock_server/harness.h"
#include "net/trade_packets.h"
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

// Every text and button label the window drew last frame, joined by '|'.
std::string drawn(lua_State* L) {
    REQUIRE(luaL_dostring(L, "local s = table.concat(test_seen, '|') test_seen = {} return s") == LUA_OK);
    std::string s = lua_tostring(L, -1);
    lua_settop(L, 0);
    return s;
}

} // namespace

TEST_CASE("Trade window (modern): the partner's accept and ours show in the window") {
    fs::path src = ui_dir() / "modern";
    REQUIRE(fs::exists(src / "trade.lua"));
    ScratchDir scratch("trade_ui_test");
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
        auto& world = conn.world_for_test();
        const game::Spawn* me = world.player();
        REQUIRE(me);
        game::Spawn pc;
        pc.id = 4100;
        pc.name = "Partner";
        pc.position = me->position + glm::vec3(3.0f, 0.0f, 0.0f);
        world.spawns[pc.id] = pc;
        // Partner asks; the request is taken and the window opens (Handle_OP_TradeRequest).
        conn.receive_zone_packet_for_test(net::encode_trade_request({world.player_id, pc.id}));
        REQUIRE(conn.trade().has_value());
        REQUIRE(conn.trade()->open);

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
        // Record what is drawn: the status line and the offers' titles are TextColored, the tag is T.Text (DrawText),
        // the buttons' labels are Button.
        REQUIRE(luaL_dostring(L, R"(
            test_seen = {}
            local tc, b, dt = ui.TextColored, ui.Button, ui.DrawText
            ui.TextColored = function(...) local a = { ... } test_seen[#test_seen + 1] = tostring(a[5]) return tc(...) end
            ui.Button = function(l, ...) test_seen[#test_seen + 1] = tostring(l) return b(l, ...) end
            if dt then
              ui.DrawText = function(...)
                for _, v in ipairs({ ... }) do if type(v) == "string" then test_seen[#test_seen + 1] = v end end
                return dt(...)
              end
            end)") == LUA_OK);
        auto frames = [&] {
            for (int i = 0; i < 3; ++i) {
                REQUIRE(luaL_dostring(L, "test_seen = {}") == LUA_OK);
                api.begin_frame();
                frame(lua);
                rig.step();
            }
        };

        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        std::string s = drawn(L);
        CHECK(s.find("Neither of you has accepted yet.") != std::string::npos);
        CHECK(s.find("Trade##TRDW_Trade_Button") != std::string::npos);
        CHECK(s.find("Accepted|") == std::string::npos);

        // Partner clicks Trade.
        conn.receive_zone_packet_for_test(net::encode(net::TradeAccept{pc.id, 1}));
        REQUIRE(conn.trade()->their_ready);
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        s = drawn(L);
        CHECK(s.find("Partner has accepted. Click Trade to complete it.") != std::string::npos);
        CHECK(s.find("Accepted") != std::string::npos); // the tag on their offer
        CHECK(s.find("Trade##TRDW_Trade_Button") != std::string::npos);

        // We click Trade too: both accepted, and our button says so.
        REQUIRE(luaL_dostring(L, "hs.ClickChild('TradeWnd', 'TRDW_Trade_Button', 'left')") == LUA_OK);
        REQUIRE(conn.trade().has_value());
        CHECK(conn.trade()->my_ready);
        frames();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        s = drawn(L);
        CHECK(s.find("Both accepted.") != std::string::npos);
        CHECK(s.find("Accepted##TRDW_Trade_Button") != std::string::npos);
    }
    ImGui::DestroyContext(ctx);
}
