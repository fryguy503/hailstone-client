// The modern pack's Trade window (ui/modern/trade.lua), run for real over Dear ImGui with no renderer against the
// mock server: who has accepted shows in the window (a status line, an "Accepted" tag on that side's offer, the Trade
// button's label once we have accepted), and our coins fit their clickable chips at each offer width.
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "app/commerce_ui.h"
#include "app/lua_ui.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/item_packets.h"
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

TEST_CASE("Give shortcut hides for NPCs that refuse direct hand-ins") {
    mq_test::World fixture("give_shortcut_ui");
    auto& conn = *fixture.conn;
    auto& world = conn.world_for_test();
    const auto slot = net::ItemSlot::possessions(net::kSlotCursor);
    net::Item held;
    held.id = 1001;
    held.name = "Quest Item";
    net::GameConnectionTestAccess::inventory(conn).put(slot, held);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame

    app::CommerceUi commerce;
    auto give_shown = [&] {
        ImGui::NewFrame();
        commerce.draw(conn, nullptr);
        ImGui::Render();
        const auto* prompt = ImGui::FindWindowByName("##hailstone_give");
        return prompt && prompt->Active;
    };
    auto& npc = world.spawns.at(10);
    npc.position = world.player()->position;
    world.target_id = npc.id;
    CHECK(give_shown());
    for (auto cls : {game::Class_TributeMaster, game::Class_GuildTributeMaster, game::Class_TributeMaster2,
                     game::Class_Banker, game::Class_GuildBanker}) {
        CAPTURE(cls);
        npc.class_id = static_cast<std::uint8_t>(cls);
        CHECK_FALSE(give_shown());
        conn.give_to(npc.id);
        CHECK_FALSE(conn.trade());
    }
    world.target_id = 12; // a player trade still has a shortcut
    CHECK(give_shown());
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Trade window (modern): acceptance and coin offers") {
    fs::path src = ui_dir() / "modern";
    REQUIRE(fs::exists(src / "trade.lua"));
    ScratchDir scratch("trade_ui_test");
    fs::path dir = scratch.path();
    fs::copy(src, dir / "modern", fs::copy_options::recursive);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
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
        for (std::size_t i = 0; i < conn.cursor_coins().size(); ++i) conn.drop_on_doll();
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

        SUBCASE("the partner's accept and ours show in the window") {
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

        SUBCASE("all four coin piles stay exact and clear of their denomination") {
            // Keep the foreground (drawn after the shadow) and use the actual font active at DrawText.
            REQUIRE(luaL_dostring(L, R"lua(
                local chips, chip = {}, nil
                local button, draw = ui.InvisibleButton, ui.DrawText
                ui.InvisibleButton = function(id, ...)
                  local clicked = button(id, ...)
                  local denomination = id:match("^##moneyTRDW_MyMoney([0-3])$")
                  chip = nil
                  if denomination then
                    local i = tonumber(denomination) + 1
                    chip = { rect = { ui.GetItemRect() }, denomination = ("pgsc"):sub(i, i) }
                    chips[i] = chip
                  end
                  return clicked
                end
                ui.DrawText = function(x, y, r, g, b, a, text, ...)
                  if chip and type(text) == "string" then
                    local field = text == chip.denomination and "symbol" or
                      (text:match("^[0-9,]+$") and "amount")
                    if field then
                      local w, h = ui.CalcTextSize(text)
                      chip[field] = { x, y, x + w, y + h, text }
                    end
                  end
                  return draw(x, y, r, g, b, a, text, ...)
                end
                function check_trade_coins(columns, platinum, others)
                  for i = 1, 4 do
                    local c = assert(chips[i], "missing money button " .. i)
                    local symbol = assert(c.symbol, "missing denomination " .. i)
                    local amount = assert(c.amount, "missing coin count " .. i)
                    assert(amount[5] == (i == 1 and platinum or others), "wrong count on button " .. i)
                    assert(amount[1] - symbol[3] >= 6, "coin count crowds its symbol on button " .. i)
                    assert(symbol[1] >= c.rect[1] and amount[3] <= c.rect[3], "text outside button " .. i)
                    if i > 1 then
                      local previous = chips[i - 1].rect
                      if (i - 1) % columns == 0 then
                        assert(c.rect[1] == chips[1].rect[1] and c.rect[2] >= previous[4], "new coin row out of place")
                      else
                        assert(c.rect[1] >= previous[3] and c.rect[2] == previous[2], "coin button out of row")
                      end
                    end
                  end
                  chips = {}
                end
                ui.SetSetting("hud.size.TradeWnd", "470,292")
            )lua") == LUA_OK);
            auto check_coins = [&](int columns, const char* platinum, const char* others) {
                frames();
                REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
                lua_getglobal(L, "check_trade_coins");
                lua_pushinteger(L, columns);
                lua_pushstring(L, platinum);
                lua_pushstring(L, others);
                const int result = lua_pcall(L, 3, 0, 0);
                REQUIRE_MESSAGE(result == LUA_OK, (result == LUA_OK ? "" : lua_tostring(L, -1)));
            };
            auto offer_coin = [&](int index, std::uint32_t count) {
                const int type = net::kCoinPlatinum - index;
                REQUIRE(conn.move_coin(net::kCoinInventory, net::kCoinCursor, type, type, count));
                REQUIRE(api.click_child("TradeWnd", "TRDW_MyMoney" + std::to_string(index)));
            };
            constexpr std::uint32_t large = 2'000'000;
            conn.receive_zone_packet_for_test(net::encode_money_update({large, large, large, large}));
            offer_coin(0, 200);
            check_coins(4, "200", "0");

            offer_coin(0, large - 200);
            for (int i = 1; i < 4; ++i) offer_coin(i, large);
            check_coins(2, "2,000,000", "2,000,000");
            REQUIRE(api.click_child("TradeWnd", "TRDW_Trade_Button"));
            REQUIRE(conn.trade()->my_ready);
            check_coins(2, "2,000,000", "2,000,000");

            REQUIRE(luaL_dostring(L, R"(ui.SetSetting("hud.size.TradeWnd", "365,390"))") == LUA_OK);
            check_coins(1, "2,000,000", "2,000,000");

            REQUIRE(luaL_dostring(L, R"(ui.SetSetting("hud.size.TradeWnd", "960,292"))") == LUA_OK);
            check_coins(4, "2,000,000", "2,000,000");
        }
    }
    ImGui::DestroyContext(ctx);
}
