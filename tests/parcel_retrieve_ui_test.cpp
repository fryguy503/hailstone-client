// The merchant's Parcels page in the modern pack (ui/modern/merchant.lua over hs.Parcels / hs.RetrieveParcel),
// driven with the mouse against the mock server: a click on a parcel's row picks it, and Retrieve takes it.
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "app/lua_ui.h"
#include "mock_server/harness.h"
#include "net/item_packets.h"
#include "net/merchant_packets.h"
#include "net/packet_io.h"
#include "ui/lua/lua_host.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::test;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kMerchant = 5003;

fs::path ui_dir() { return fs::path(__FILE__).parent_path().parent_path() / "ui"; }

void open_parcel_merchant(net::GameConnection& conn) {
    auto& w = conn.world_for_test();
    REQUIRE(w.player());
    game::Spawn m = *w.player();
    m.id = kMerchant;
    m.name = "Parcel_Merchant00";
    m.class_id = 41;
    m.is_npc = true;
    w.spawns[kMerchant] = m;
    conn.open_merchant(kMerchant);
    REQUIRE(conn.merchant());
    net::MerchantClick open;
    open.npc_id = kMerchant;
    open.player_id = w.player_id;
    open.rate = 1.0f;
    open.tab_display = 7;
    conn.receive_zone_packet_for_test(net::encode(open));
    REQUIRE(conn.merchant()->open);
}

void deliver_parcel(net::GameConnection& conn, std::int16_t slot, std::uint32_t id, const std::string& name) {
    net::ItemPacket ip;
    ip.type = net::kItemPacketParcel;
    ip.item = make_item(id, name, slot);
    ip.parcel_sent_time = 1'790'000'000;
    ip.parcel_from = "Somebody";
    conn.receive_zone_packet_for_test(net::encode_item_packet(false, ip));
}

// The parcel retrieved last: its slot (ParcelRetrieve's parcel_slot), or -1 when none was sent.
long long last_retrieved(const std::vector<net::AppPacket>& log) {
    long long slot = -1;
    for (const auto& p : log)
        if (net::is_op(p, net::Op::ShopRetrieveParcel))
            if (auto r = net::decode_parcel_retrieve(p)) slot = r->parcel_slot;
    return slot;
}

} // namespace

TEST_CASE("Parcels page (modern): a click on a row picks the parcel, Retrieve takes the picked or the first") {
    fs::path src = ui_dir() / "modern";
    REQUIRE(fs::exists(src / "merchant.lua"));
    ScratchDir scratch("parcel_retrieve_ui_test");
    fs::path dir = scratch.path();
    fs::copy(src, dir / "modern", fs::copy_options::recursive);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char* px = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
    {
        net::ConnectionOptions options = quick_options();
        options.servers_dir = dir / "servers";
        Rig rig(MockConfig::standard(), options);
        REQUIRE(rig.to_zone());
        auto& conn = rig.conn;
        open_parcel_merchant(conn);
        deliver_parcel(conn, 0, 3001, "Gem Encrusted Axe");
        deliver_parcel(conn, 1, 3002, "Rusty Dagger");
        REQUIRE(conn.merchant()->parcels.size() == 2);

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
        // Where the rows and buttons are drawn: each hit box's and button's rectangle, by id.
        REQUIRE(luaL_dostring(L, R"lua(
test_rects = {}
local invisible = ui.InvisibleButton
ui.InvisibleButton = function(id, ...)
  local r = invisible(id, ...)
  test_rects[id] = { ui.GetItemRect() }
  return r
end
hs.SetTabPage('MerchantWnd', 'MW_MerchantSubwindows', 2)
)lua") == LUA_OK);
        auto frame = [&] {
            api.begin_frame();
            io.DisplaySize = ImVec2(1280, 800);
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
            rig.step();
            REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
        };
        auto centre = [&](const char* id) {
            REQUIRE(luaL_dostring(L, (std::string("local r = test_rects['") + id +
                                      "'] if not r then return nil end return (r[1] + r[3]) / 2, (r[2] + r[4]) / 2")
                                         .c_str()) == LUA_OK);
            REQUIRE(lua_isnumber(L, 1));
            ImVec2 c(static_cast<float>(lua_tonumber(L, 1)), static_cast<float>(lua_tonumber(L, 2)));
            lua_settop(L, 0);
            return c;
        };
        auto click = [&](const char* id) {
            const ImVec2 at = centre(id);
            io.AddMousePosEvent(at.x, at.y);
            frame();
            io.AddMouseButtonEvent(0, true);
            frame();
            io.AddMouseButtonEvent(0, false);
            frame();
            frame();
        };
        api.show("MerchantWnd", true);
        for (int i = 0; i < 4; ++i) frame();
        api.show("TipWindow", false); // the merchant's first-open tip, over the window
        for (int i = 0; i < 2; ++i) frame();

        SUBCASE("a picked row is the one taken") {
            click("##parcel2");
            click("##retrieve");
            REQUIRE(rig.until([&] { return last_retrieved(rig.server.zone_log) >= 0; }));
            CHECK(last_retrieved(rig.server.zone_log) == 1);
        }
        SUBCASE("with none picked, Retrieve takes the first") {
            click("##retrieve");
            REQUIRE(rig.until([&] { return last_retrieved(rig.server.zone_log) >= 0; }));
            CHECK(last_retrieved(rig.server.zone_log) == 0);
        }
    }
    ImGui::DestroyContext(ctx);
}
