// A right click on a good in the point merchants' lists (NewPointMerchantWnd for the adventure merchants, PointMerchantWnd
// for the alternate currency ones; app/lua_ui_market.cpp) inspects it: the handlers send OP_ItemLinkResponse
// with the good's id and name, or with Shift
// or Ctrl the item preview request. Over the hs API, against the mock server.
#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "mock_server/harness.h"
#include "net/item_packets.h"
#include "net/packet_io.h"

using namespace hailstone;
using namespace hailstone::test;

namespace {

constexpr std::uint32_t kMerchant = 5002;

// A merchant of class `class_id` beside the player.
void place_merchant(net::GameConnection& conn, std::uint8_t class_id) {
    auto& w = conn.world_for_test();
    REQUIRE(w.player());
    game::Spawn m = *w.player();
    m.id = kMerchant;
    m.name = "Merchant00";
    m.class_id = class_id;
    m.is_npc = true;
    w.spawns[kMerchant] = m;
}

net::AppPacket text_packet(net::Op op, std::string_view prefix, const std::string& text) {
    net::AppPacket p(net::to_wire(op), {});
    for (char c : prefix) p.data.push_back(static_cast<std::byte>(c));
    for (char c : text) p.data.push_back(static_cast<std::byte>(c));
    p.data.push_back(std::byte{0});
    return p;
}

struct Lua {
    lua_State* L = luaL_newstate();
    explicit Lua(app::UiApi& api) {
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
    }
    ~Lua() { lua_close(L); }
    bool flag(const std::string& code) {
        REQUIRE(luaL_dostring(L, code.c_str()) == LUA_OK);
        bool v = lua_toboolean(L, -1);
        lua_settop(L, 0);
        return v;
    }
};

// The last OP_ItemLinkResponse the server received: the item id and the name (ItemLinkRequest's layout).
std::pair<std::uint32_t, std::string> last_link_request(const std::vector<net::AppPacket>& log) {
    const net::AppPacket* last = nullptr;
    for (const auto& p : log)
        if (net::is_op(p, net::Op::ItemLinkResponse)) last = &p;
    REQUIRE(last);
    REQUIRE(last->data.size() == net::kItemLinkRequestSize);
    std::uint32_t id = 0, flags = 1;
    std::memcpy(&id, last->data.data(), 4);
    std::memcpy(&flags, last->data.data() + 4, 4);
    CHECK(flags == 0);
    std::string name(reinterpret_cast<const char*>(last->data.data() + 8));
    return {id, name};
}

// Right clicks on row 1 of `window`'s `list`: without a key it inspects the good, with Ctrl it previews it.
void check_inspect(Rig& rig, Lua& lua, const std::string& window, const std::string& list, std::uint32_t item_id,
                   const std::string& name) {
    auto& io = ImGui::GetIO();
    io.KeyShift = io.KeyCtrl = false;
    CHECK(lua.flag("return hs.ListClick('" + window + "', '" + list + "', 1, 'right', 0)"));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::ItemLinkResponse) >= 1; }));
    auto [id, sent] = last_link_request(rig.server.zone_log);
    CHECK(id == item_id);
    CHECK(sent == name);

    // A plain left click only selects; with Ctrl it inspects too.
    CHECK(lua.flag("return hs.ListClick('" + window + "', '" + list + "', 1, 'left', 0)"));
    io.KeyCtrl = true;
    CHECK(lua.flag("return hs.ListClick('" + window + "', '" + list + "', 1, 'left', 0)"));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::ItemLinkResponse) >= 2; }));

    // Ctrl and a right click: the item preview, no inspect.
    CHECK(lua.flag("return hs.ListClick('" + window + "', '" + list + "', 1, 'right', 0)"));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, net::Op::ItemPreviewRequest) >= 1; }));
    CHECK(MockEqServer::count(rig.server.zone_log, net::Op::ItemLinkResponse) == 2);
    io.KeyCtrl = false;
}

} // namespace

TEST_CASE("Adventure merchant (LDoN): a right click on a good inspects it") {
    ImGuiContext* ctx = ImGui::CreateContext(); // the hs API reads the keyboard's modifiers
    Rig rig(MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    place_merchant(conn, 61); // an LDoN merchant
    REQUIRE(conn.open_adventure_merchant(kMerchant));
    // Handle_OP_AdventureMerchantRequest: u8 count, then "^name|id|price|theme|stackable|lore|races|classes".
    conn.receive_zone_packet_for_test(text_packet(net::Op::AdventureMerchantResponse, std::string(1, '\x02'),
                                                  "^Cloak of Guk|1234|150|1|0|1|65535|65535^Ring of Takish|5678|40|5|0|1|65535|65535"));
    REQUIRE(conn.adventure_merchant().items.size() == 2);

    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::UiApi api(host);
    Lua lua(api);
    check_inspect(rig, lua, "NewPointMerchantWnd", "NewPointMerchant_ItemList", 5678, "Ring of Takish");
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Alternate currency merchant: a right click on a good inspects it") {
    ImGuiContext* ctx = ImGui::CreateContext();
    Rig rig(MockConfig::standard());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    place_merchant(conn, 70);
    REQUIRE(conn.open_point_merchant(kMerchant));
    conn.receive_zone_packet_for_test(text_packet(net::Op::AltCurrencyMerchantReply, "",
                                                  "31|1|31|2^Shiny Ring|1001|25|0|1|65535|65535^Old Boot|1002|3|0|1|1|2"));
    REQUIRE(conn.point_merchant().list.items.size() == 2);

    app::UiHost host;
    host.in_game = host.ui_ready = true;
    host.conn = &conn;
    app::UiApi api(host);
    Lua lua(api);
    check_inspect(rig, lua, "PointMerchantWnd", "PointMerchant_ItemList", 1002, "Old Boot");
    ImGui::DestroyContext(ctx);
}
